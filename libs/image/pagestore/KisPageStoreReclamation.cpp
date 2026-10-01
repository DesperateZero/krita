/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisPageStoreReclamation_p.h"
#include "KisMutationStorage_p.h"
#include "KisPageWriteCoordinator_p.h"

#include <QRunnable>
#include <QThreadPool>
#include <QThread>
#include <QCoreApplication>
#include <QScopeGuard>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <boost/intrusive/set.hpp>
#include <mutex>
#include <thread>
#include <utility>

struct KisPageReclamationDelayState : boost::intrusive::set_base_hook<>
{
    std::weak_ptr<KisPageReclamationDelayClock> clock;
    KisPageReadinessCallback dispatch;
    std::chrono::steady_clock::time_point deadline;
    std::shared_ptr<KisPageReclamationDelayState> queuedPin;
    bool queued = false; // protected by clock->mutex
    bool armed = false;
    quint64 generation = 0;
    quint64 readyGeneration = 0;
};
struct DeadlineLess {
    bool operator()(const KisPageReclamationDelayState &a, const KisPageReclamationDelayState &b) const
    { return a.deadline < b.deadline; }
};
struct KisPageReclamationDelayClock
{
    using Time = std::chrono::steady_clock;
    using Tasks = boost::intrusive::multiset<KisPageReclamationDelayState, boost::intrusive::compare<DeadlineLess>>;
    struct Wakes {
        KisPageReclamationWakeState *first = nullptr;
        KisPageReclamationWakeState *last = nullptr;
    };
    Wakes dormantWakes;
    Wakes readyWakes;
    std::mutex mutex;
    std::condition_variable changed;
    Tasks tasks;
    bool stopping = false;
};

namespace {
enum class WakeQueue : qint8 { None = -1, Dormant = 0, Ready = 1 };

void placeWake(KisPageReclamationDelayClock &clock,
               KisPageReclamationWakeState &wake,
               WakeQueue target) noexcept
{
    if (wake.registered) {
        auto &source = wake.ready ? clock.readyWakes : clock.dormantWakes;
        if (wake.previous) wake.previous->next = wake.next;
        else source.first = wake.next;
        if (wake.next) wake.next->previous = wake.previous;
        else source.last = wake.previous;
    }
    wake.previous = nullptr;
    wake.next = nullptr;
    wake.registered = target != WakeQueue::None;
    wake.ready = target == WakeQueue::Ready;
    if (!wake.registered) return;
    auto &destination = wake.ready ? clock.readyWakes : clock.dormantWakes;
    wake.previous = destination.last;
    if (destination.last) destination.last->next = &wake;
    else destination.first = &wake;
    destination.last = &wake;
}

void releaseWake(KisPageReclamationWakeState *wake) noexcept
{
    if (!wake || wake->references.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return;
    auto storage = std::move(wake->storage);
    std::destroy_at(wake);
    storage.deallocate(wake, 1);
}

// There is exactly one non-expiring worker in this dedicated pool. Reading a
// dynamic-library thread_local flag can allocate TLS storage on a caller's
// first cleanup (notably on Darwin), when allocation must no longer be needed.
// Publish the active worker identity instead. It stays valid through capture
// destruction, and is cleared before the worker can leave this job or exit.
std::atomic<Qt::HANDLE> reclaimerThread{nullptr};
void shutdownReclamation();

class ReclamationExecutor
{
public:
    ReclamationExecutor()
    {
        pool.setMaxThreadCount(1);
        pool.setExpiryTimeout(-1);
        m_runner.reset(QRunnable::create([this] { runJobs(); }));
        m_runner->setAutoDelete(false);
        qAddPostRoutine(shutdownReclamation);
        try {
            pool.start(m_runner.get());
            std::unique_lock<std::mutex> lock(m_jobMutex);
            m_jobsDrained.wait(lock, [this] { return m_workerReady; });
        } catch (...) {
            qRemovePostRoutine(shutdownReclamation);
            stopJobs();
            throw;
        }
    }
    ~ReclamationExecutor()
    {
        qRemovePostRoutine(shutdownReclamation);
        stopDelays();
        stopJobs();
    }

    void enqueue(KisPageReclamationJob *job)
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        Q_ASSERT(job && !job->next);
        Q_ASSERT(!m_jobsStopping || kisOnPageStoreReclamationThread());
        if (m_lastJob) m_lastJob->next = job;
        else m_firstJob = job;
        m_lastJob = job;
        m_jobChanged.notify_one();
    }

    void drainJobs()
    {
        std::unique_lock<std::mutex> lock(m_jobMutex);
        m_jobsDrained.wait(lock, [this] { return !m_firstJob && !m_jobRunning; });
    }

    void stopJobs()
    {
        {
            std::lock_guard<std::mutex> lock(m_jobMutex);
            m_jobsStopping = true;
            m_jobChanged.notify_one();
        }
        pool.waitForDone();
    }

    std::shared_ptr<KisPageReclamationDelayState> prepareDelay(
        KisPageReadinessCallback dispatch, KisBackingBudgetController *storageBudget)
    {
        if (!dispatch) return {};
        dispatch.fund(storageBudget);
        auto task = std::allocate_shared<KisPageReclamationDelayState>(
            dispatch.storageAllocator<KisPageReclamationDelayState>());
        task->dispatch = std::move(dispatch);
        std::lock_guard<std::mutex> initialization(m_delayMutex);
        if (m_stopping) return {};
        ensureClockLocked();
        task->clock = m_clock;
        return task;
    }

    KisPageReclamationWakeState *prepareWake(
        KisPageReadinessCallback dispatch,
        KisBackingBudgetController *storageBudget)
    {
        if (!dispatch) return {};
        dispatch.fund(storageBudget);
        auto storage = dispatch.storageAllocator<KisPageReclamationWakeState>();
        auto *raw = storage.allocate(1);
        try {
            ::new (static_cast<void *>(raw)) KisPageReclamationWakeState(storage);
        } catch (...) {
            storage.deallocate(raw, 1);
            throw;
        }
        std::unique_ptr<KisPageReclamationWakeState, decltype(&releaseWake)> prepared(raw, releaseWake);
        raw->dispatch = std::move(dispatch);
        std::lock_guard<std::mutex> initialization(m_delayMutex);
        if (m_stopping) return {};
        ensureClockLocked();
        raw->clock = m_clock;
        {
            std::lock_guard<std::mutex> lock(m_clock->mutex);
            placeWake(*m_clock, *raw, WakeQueue::Dormant);
        }
        return prepared.release();
    }

    void stopDelays()
    {
        KisPageReclamationDelayClock::Tasks cancelled;
        std::shared_ptr<KisPageReclamationDelayClock> clock;
        {
            std::lock_guard<std::mutex> initialization(m_delayMutex);
            m_stopping = true;
            if (m_clock) {
                clock = m_clock;
                std::lock_guard<std::mutex> lock(m_clock->mutex);
                m_clock->stopping = true;
                cancelled.swap(m_clock->tasks);
                for (auto &entry : cancelled) {
                    entry.queued = false;
                    entry.armed = false;
                    entry.readyGeneration = 0;
                }
                m_clock->changed.notify_one();
            }
        }
        if (m_monitor.joinable()) m_monitor.join();
        // No producer may outlive application teardown. Drop callbacks only
        // after the monitor is stopped and outside either scheduling gate.
        while (!cancelled.empty()) {
            auto &entry = *cancelled.begin();
            auto task = std::move(entry.queuedPin);
            cancelled.erase(cancelled.begin());
            task->dispatch = {};
        }
        while (clock) {
            KisPageReclamationWakeState *wake = nullptr;
            {
                std::lock_guard<std::mutex> lock(clock->mutex);
                wake = clock->readyWakes.first
                    ? clock->readyWakes.first : clock->dormantWakes.first;
                if (!wake) break;
                wake->references.fetch_add(1, std::memory_order_relaxed);
                placeWake(*clock, *wake, WakeQueue::None);
            }
            wake->dispatch = {};
            releaseWake(wake);
        }
    }

    QThreadPool pool;
private:
    void runJobs()
    {
        const auto worker = QThread::currentThreadId();
        {
            std::lock_guard<std::mutex> lock(m_jobMutex);
            m_workerReady = true;
            m_jobsDrained.notify_all();
        }
        for (;;) {
            KisPageReclamationJob *job = nullptr;
            {
                std::unique_lock<std::mutex> lock(m_jobMutex);
                m_jobChanged.wait(lock, [this] { return m_firstJob || m_jobsStopping; });
                job = m_firstJob;
                if (!job) return;
                m_firstJob = job->next;
                if (!m_firstJob) m_lastJob = nullptr;
                job->next = nullptr;
                m_jobRunning = true;
            }
            // The original composition-root reference must outlive captures,
            // physical task storage and its charge. In particular, releasing
            // it from the callback body can destroy the budget too early.
            const auto finished = job->finished;
            void *context = job->finishContext;
            const bool reusable = job->reusable;
            reclaimerThread.store(worker, std::memory_order_release);
            {
                const auto cleanup = qScopeGuard([&] {
                    if (!reusable) job->dispose();
                    if (finished) finished(context);
                });
                job->invoke();
            }
            reclaimerThread.store(nullptr, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(m_jobMutex);
                m_jobRunning = false;
                if (!m_firstJob) m_jobsDrained.notify_all();
            }
        }
    }

    void ensureClockLocked()
    {
        if (!m_clock) {
            auto clock = std::make_shared<KisPageReclamationDelayClock>();
            // Lazy, process-wide deadline monitor. No waiting work occupies
            // the sole reclamation worker or requires a GUI event loop.
            auto monitor = std::thread([clock] {
                std::unique_lock<std::mutex> lock(clock->mutex);
                bool preferWake = true;
                while (!clock->stopping) {
                    if (clock->readyWakes.first && (preferWake || clock->tasks.empty() ||
                        clock->tasks.begin()->deadline > KisPageReclamationDelayClock::Time::now())) {
                        preferWake = false;
                        auto *state = clock->readyWakes.first;
                        state->references.fetch_add(1, std::memory_order_relaxed);
                        placeWake(*clock, *state, WakeQueue::Dormant);
                        lock.unlock();
                        state->dispatch();
                        releaseWake(state); // temporary ownership ends outside the monitor gate
                        lock.lock();
                        continue;
                    }
                    if (clock->tasks.empty()) {
                        clock->changed.wait(lock);
                        continue;
                    }
                    const auto deadline = clock->tasks.begin()->deadline;
                    if (deadline > KisPageReclamationDelayClock::Time::now()) {
                        clock->changed.wait_until(lock, deadline);
                        continue;
                    }
                    preferWake = true;
                    auto task = std::move(clock->tasks.begin()->queuedPin);
                    clock->tasks.erase(clock->tasks.begin());
                    task->queued = false;
                    task->readyGeneration = task->generation;
                    auto dispatch = task->dispatch;
                    lock.unlock();
                    dispatch();
                    dispatch = {}; // captures destroyed outside the clock gate
                    task.reset();
                    lock.lock();
                }
            });
            m_clock = std::move(clock);
            m_monitor = std::move(monitor);
        }
    }

    std::mutex m_delayMutex;
    std::mutex m_jobMutex;
    std::condition_variable m_jobChanged;
    std::condition_variable m_jobsDrained;
    std::unique_ptr<QRunnable> m_runner;
    KisPageReclamationJob *m_firstJob = nullptr;
    KisPageReclamationJob *m_lastJob = nullptr;
    bool m_jobRunning = false;
    bool m_workerReady = false;
    bool m_jobsStopping = false;
    bool m_stopping = false;
    std::shared_ptr<KisPageReclamationDelayClock> m_clock;
    std::thread m_monitor;
};

ReclamationExecutor &executor()
{
    static ReclamationExecutor instance;
    return instance;
}

void shutdownReclamation()
{
    executor().stopDelays();
    executor().stopJobs();
}
}

KisPageReclamationDelay::KisPageReclamationDelay() = default;
KisPageReclamationDelay::KisPageReclamationDelay(
    KisPageReadinessCallback dispatch, KisBackingBudgetController *storageBudget)
    : d(executor().prepareDelay(std::move(dispatch), storageBudget)) {}
KisPageReclamationDelay::~KisPageReclamationDelay() { reset(); }
KisPageReclamationDelay::KisPageReclamationDelay(KisPageReclamationDelay &&) noexcept = default;
KisPageReclamationDelay &KisPageReclamationDelay::operator=(KisPageReclamationDelay &&other) noexcept
{
    if (this != &other) { reset(); d = std::move(other.d); }
    return *this;
}
void KisPageReclamationDelay::reset()
{
    cancel();
    d.reset();
}
void KisPageReclamationDelay::cancel()
{
    if (!d) return;
    const auto clock = d->clock.lock();
    if (!clock) return;
    std::shared_ptr<KisPageReclamationDelayState> queued;
    {
        std::lock_guard<std::mutex> lock(clock->mutex);
        d->armed = false;
        d->readyGeneration = 0;
        if (d->queued) {
            clock->tasks.erase(clock->tasks.iterator_to(*d));
            queued = std::move(d->queuedPin);
            d->queued = false;
        }
    }
    clock->changed.notify_one();
}
bool KisPageReclamationDelay::isValid() const { return bool(d); }
bool KisPageReclamationDelay::arm(int delayMs)
{
    if (!d) return false;
    const auto clock = d->clock.lock();
    if (!clock) return false;
    {
        std::lock_guard<std::mutex> lock(clock->mutex);
        if (clock->stopping || !d->dispatch || d->generation == std::numeric_limits<quint64>::max())
            return false;
        if (d->queued) clock->tasks.erase(clock->tasks.iterator_to(*d));
        ++d->generation;
        d->readyGeneration = 0;
        d->armed = true;
        d->deadline = KisPageReclamationDelayClock::Time::now() +
            std::chrono::milliseconds(std::max(1, delayMs));
        d->queuedPin = d;
        clock->tasks.insert(*d);
        d->queued = true;
    }
    clock->changed.notify_one();
    return true;
}
bool KisPageReclamationDelay::takeReady()
{
    if (!d) return false;
    const auto clock = d->clock.lock();
    if (!clock) return false;
    std::lock_guard<std::mutex> lock(clock->mutex);
    if (clock->stopping || !d->armed || d->readyGeneration != d->generation) return false;
    d->armed = false;
    d->readyGeneration = 0;
    return true;
}

KisPageReclamationWake::KisPageReclamationWake() = default;
KisPageReclamationWake::~KisPageReclamationWake() { reset(); }
KisPageReclamationWake::KisPageReclamationWake(KisPageReclamationWake &&other) noexcept
    : d(std::exchange(other.d, nullptr)) {}
KisPageReclamationWake &KisPageReclamationWake::operator=(KisPageReclamationWake &&other) noexcept
{
    if (this != &other) { reset(); d = std::exchange(other.d, nullptr); }
    return *this;
}
bool KisPageReclamationWake::isValid() const { return d; }
void KisPageReclamationWake::reset()
{
    auto *wake = std::exchange(d, nullptr);
    if (!wake) return;
    const auto clock = wake->clock.lock();
    if (clock) {
        std::lock_guard<std::mutex> lock(clock->mutex);
        if (wake->registered) placeWake(*clock, *wake, WakeQueue::None);
    }
    releaseWake(wake);
}
void KisPageReclamationWake::notify() const noexcept
{
    if (!d) return;
    const auto clock = d->clock.lock();
    if (!clock) return;
    {
        std::lock_guard<std::mutex> lock(clock->mutex);
        if (!d->registered || d->ready || clock->stopping) return;
        placeWake(*clock, *d, WakeQueue::Ready);
    }
    clock->changed.notify_one();
}
KisPageReclamationWake kisPreparePageStoreReclamationWake(
    KisPageReadinessCallback dispatch,
    KisBackingBudgetController *storageBudget)
{
    KisPageReclamationWake result;
    result.d = executor().prepareWake(std::move(dispatch), storageBudget);
    return result;
}

void kisEnqueuePageStoreReclamation(KisPageReclamationJob *job)
{
    auto &runtime = executor();
    if (job) runtime.enqueue(job);
}

bool kisOnPageStoreReclamationThread()
{
    const auto worker = reclaimerThread.load(std::memory_order_acquire);
    return worker && worker == QThread::currentThreadId();
}

void kisDrainPageStoreReclamation()
{
    Q_ASSERT(!kisOnPageStoreReclamationThread());
    executor().drainJobs();
}

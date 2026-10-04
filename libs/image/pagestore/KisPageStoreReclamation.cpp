/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisPageStoreReclamation_p.h"
#include "KisMutationStorage_p.h"
#include "KisPageWriteCoordinator_p.h"

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
#ifdef Q_OS_DARWIN
#include <pthread.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

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

// The function/context live inside the prepared executor. Darwin needs no
// std::thread callable allocation and uses an explicitly owned stack. Native
// pthread control/TLS mapping is measured before the thread may run callbacks.
class ReclamationThread
{
public:
    ~ReclamationThread() { join(); }
    void start(void (*function)(void *), void *context)
    {
#ifdef Q_OS_DARWIN
        Q_ASSERT(!m_joinable);
        m_started = false;
        m_accepted = false;
        m_function = function;
        m_context = context;
        m_pageSize = size_t(sysconf(_SC_PAGESIZE));
        constexpr size_t stackBytes = 512 * 1024;
        constexpr size_t nativePreparation = 64 * 1024;
        m_stack = kisAllocatePageProcessStorage(stackBytes, m_pageSize);
        m_stackBytes = stackBytes;
        try {
            kisReservePageProcessStorage(nativePreparation);
            m_nativeBytes = nativePreparation;
            pthread_attr_t attributes;
            if (pthread_attr_init(&attributes)) throw std::bad_alloc();
            const auto clearAttributes = qScopeGuard([&] { pthread_attr_destroy(&attributes); });
            if (pthread_attr_setstack(&attributes, m_stack, m_stackBytes) ||
                pthread_create(&m_thread, &attributes, &ReclamationThread::invoke, this))
                throw std::bad_alloc();
            m_joinable = true;
            mach_vm_address_t address = reinterpret_cast<mach_vm_address_t>(m_thread);
            mach_vm_size_t actualBytes = 0;
            vm_region_basic_info_data_64_t information{};
            mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t object = MACH_PORT_NULL;
            const auto query = mach_vm_region(mach_task_self(), &address, &actualBytes,
                VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&information), &count, &object);
            if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
            const auto threadAddress = reinterpret_cast<mach_vm_address_t>(m_thread);
            if (!m_pageSize || sizeof(*m_thread) > std::numeric_limits<size_t>::max() - (m_pageSize - 1))
                throw std::bad_alloc();
            const size_t controlBytes =
                (sizeof(*m_thread) + m_pageSize - 1) / m_pageSize * m_pageSize;
            // Adjacent pthread controls can be coalesced with external/runtime
            // mappings into one VM region. The PageStore thread owns only its
            // page-rounded SDK control object; validate that exact span is
            // mapped instead of charging the containing region.
            if (query != KERN_SUCCESS || address > threadAddress
                || actualBytes == 0
                || actualBytes > std::numeric_limits<mach_vm_address_t>::max() - address
                || threadAddress % m_pageSize != 0
                || threadAddress - address > actualBytes
                || controlBytes > actualBytes - (threadAddress - address)
                || controlBytes > nativePreparation)
                throw std::bad_alloc();
            kisReleasePageProcessStorage(nativePreparation - controlBytes);
            m_nativeBytes = controlBytes;
            {
                std::lock_guard<std::mutex> lock(m_startMutex);
                m_accepted = true;
                m_started = true;
            }
            m_startChanged.notify_one();
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(m_startMutex);
                m_started = true;
            }
            m_startChanged.notify_one();
            join();
            throw;
        }
#else
        // Other platform runtimes retain their existing execution contract.
        // Their native thread/control allocation census is platform-specific.
        m_thread = std::thread(function, context);
#endif
    }
    void join() noexcept
    {
#ifdef Q_OS_DARWIN
        if (m_joinable) {
            const int result = pthread_join(m_thread, nullptr);
            Q_ASSERT(result == 0);
            m_joinable = false;
        }
        if (m_nativeBytes) kisReleasePageProcessStorage(std::exchange(m_nativeBytes, 0));
        if (m_stack) {
            kisFreePageProcessStorage(m_stack, m_stackBytes, m_pageSize);
            m_stack = nullptr;
        }
#else
        if (m_thread.joinable()) m_thread.join();
#endif
    }
private:
#ifdef Q_OS_DARWIN
    static void *invoke(void *context)
    {
        auto *thread = static_cast<ReclamationThread *>(context);
        {
            std::unique_lock<std::mutex> lock(thread->m_startMutex);
            thread->m_startChanged.wait(lock, [&] { return thread->m_started; });
            if (!thread->m_accepted) return nullptr;
        }
        thread->m_function(thread->m_context);
        return nullptr;
    }
    pthread_t m_thread{};
    bool m_joinable = false;
    bool m_started = false;
    bool m_accepted = false;
    void (*m_function)(void *) = nullptr;
    void *m_context = nullptr;
    void *m_stack = nullptr;
    size_t m_stackBytes = 0, m_pageSize = 0, m_nativeBytes = 0;
    std::mutex m_startMutex;
    std::condition_variable m_startChanged;
#else
    std::thread m_thread;
#endif
};

class ReclamationExecutor : public KisPageProcessStorageObject
{
public:
    ReclamationExecutor()
    {
        qAddPostRoutine(kisStopPageStoreReclamation);
        try {
            m_worker.start([](void *context) { static_cast<ReclamationExecutor *>(context)->runJobs(); }, this);
            std::unique_lock<std::mutex> lock(m_jobMutex);
            m_jobsDrained.wait(lock, [this] { return m_workerReady; });
        } catch (...) {
            qRemovePostRoutine(kisStopPageStoreReclamation);
            stopJobs();
            throw;
        }
    }
    ~ReclamationExecutor()
    {
        qRemovePostRoutine(kisStopPageStoreReclamation);
        stopDelays();
        stopJobs();
    }

    bool enqueue(KisPageReclamationJob *job)
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        if (m_jobsStopping && !kisOnPageStoreReclamationThread()) return false;
        if (!job) return true;
        Q_ASSERT(job && !job->next);
        if (m_lastJob) m_lastJob->next = job;
        else m_firstJob = job;
        m_lastJob = job;
        m_jobChanged.notify_one();
        return true;
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
        m_worker.join();
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
                clock = std::move(m_clock);
                std::lock_guard<std::mutex> lock(clock->mutex);
                clock->stopping = true;
                cancelled.swap(clock->tasks);
                for (auto &entry : cancelled) {
                    entry.queued = false;
                    entry.armed = false;
                    entry.readyGeneration = 0;
                }
                clock->changed.notify_one();
            }
        }
        m_monitor.join();
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
            auto clock = std::allocate_shared<KisPageReclamationDelayClock>(
                KisMutationStorageAllocator<KisPageReclamationDelayClock>{});
            // Lazy, process-wide deadline monitor. No waiting work occupies
            // the sole reclamation worker or requires a GUI event loop.
            m_monitor.start([](void *context) {
                auto *clock = static_cast<KisPageReclamationDelayClock *>(context);
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
            }, clock.get());
            m_clock = std::move(clock);
        }
    }

    std::mutex m_delayMutex;
    std::mutex m_jobMutex;
    std::condition_variable m_jobChanged;
    std::condition_variable m_jobsDrained;
    ReclamationThread m_worker;
    KisPageReclamationJob *m_firstJob = nullptr;
    KisPageReclamationJob *m_lastJob = nullptr;
    bool m_jobRunning = false;
    bool m_workerReady = false;
    bool m_jobsStopping = false;
    bool m_stopping = false;
    std::shared_ptr<KisPageReclamationDelayClock> m_clock;
    ReclamationThread m_monitor;
};

ReclamationExecutor &executor()
{
    static const auto instance = std::make_unique<ReclamationExecutor>();
    static_assert(sizeof(instance) == sizeof(std::unique_ptr<KisPageReclamationJob>));
    return *instance;
}

}

void kisStopPageStoreReclamation()
{
    executor().stopDelays();
    executor().stopJobs();
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

bool kisEnqueuePageStoreReclamation(KisPageReclamationJob *job)
{
    return executor().enqueue(job);
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

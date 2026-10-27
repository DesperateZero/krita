/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisPageHistoryCollector_p.h"

#include "KisPageStoreReclamation_p.h"

#include <QMutexLocker>
#include <QDeadlineTimer>

#include <algorithm>
#include <tuple>
#include <limits>
#include <utility>

struct KisPageHistoryWakeContext
{
    QMutex mutex;
    KisPageHistoryCollector *collector = nullptr;
    KisPageReclamationDelay retry;
    qint64 timerDeadline = 0;
    quint64 retryWakeups = 0;
    bool accepting = false;
    bool jobScheduled = false;
    qsizetype activities = 0;
    KisPageWaitCondition idle;
};

KisPageHistoryCollector::KisPageHistoryCollector(
    KisPageMetadataCoordinator &metadata,
    KisImageEpochReferenceModel &epochs,
    KisPageRetirementQueue &retirementQueue,
    QMutex &ownerMutex,
    QAtomicInt &ownerLifetimeReferences,
    bool &operational,
    bool &closing,
    bool &backgroundReclamation,
    void *ownerContext,
    ReleaseOwnerLifetime releaseOwnerLifetime,
    HasAbandonedMutations hasAbandonedMutations,
    CollectAbandonedMutations collectAbandonedMutations,
    NextIdleDeadline nextIdleDeadline,
    CollectIdleCapacity collectIdleCapacity)
    : m_metadata(metadata)
    , m_epochs(epochs)
    , m_retirementQueue(retirementQueue)
    , m_ownerMutex(ownerMutex)
    , m_ownerLifetimeReferences(ownerLifetimeReferences)
    , m_operational(operational)
    , m_closing(closing)
    , m_backgroundReclamation(backgroundReclamation)
    , m_ownerContext(ownerContext)
    , m_releaseOwnerLifetime(releaseOwnerLifetime)
    , m_hasAbandonedMutations(hasAbandonedMutations)
    , m_collectAbandonedMutations(collectAbandonedMutations)
    , m_nextIdleDeadline(nextIdleDeadline)
    , m_collectIdleCapacity(collectIdleCapacity)
{
    static_assert(PageAdmissionBudget <= KisImageEpochReferenceModel::ReachabilityScanLimit);
    static_assert(RootVisitBudget <= KisImageEpochReferenceModel::ReachabilityRootBudget);
    static_assert(VersionScanBudget <= 32);
    Q_ASSERT(m_ownerContext);
    Q_ASSERT(m_releaseOwnerLifetime);
    Q_ASSERT(bool(m_hasAbandonedMutations) == bool(m_collectAbandonedMutations));
    Q_ASSERT(bool(m_nextIdleDeadline) == bool(m_collectIdleCapacity));
}

KisPageHistoryCollector::~KisPageHistoryCollector()
{
    if (m_wakeContext) {
        QMutexLocker lock(&m_wakeContext->mutex);
        Q_ASSERT(!m_wakeContext->jobScheduled);
        m_wakeContext->collector = nullptr;
    }
    clearLocked();
}

void KisPageHistoryCollector::requestKeyLocked(const KisPageKey &key) noexcept
{
    requestKeyLocked(key, false);
}

void KisPageHistoryCollector::requestKeyLocked(const KisPageKey &key, bool historyAlreadyObserved) noexcept
{
    try {
        auto entry = m_work.lower_bound(key);
        const bool inserted = entry == m_work.end() || !(entry->key == key);
        if (inserted) {
            // A publication/release event need not have produced history.
            // Do not fund a key/scan/retry just to discover an empty slice.
            // The Store gate serializes producing history and requesting it;
            // existing work retains its original scan/repeat responsibility.
            if (!historyAlreadyObserved && !m_metadata.hasHistory(key)) return;
            auto *work = m_workAllocator.allocate(1);
            // Work construction only binds its inline values and cannot fail.
            new (work) Work(key);
            entry = m_work.insert_before(entry, *work);
        }
        queueWorkLocked(*entry);
    } catch (const std::bad_alloc &) {
        // Metadata keeps every historical version until collection accepts
        // its retirement. The original collector retains the missing-key scan,
        // for both foreground and background entry points.
        m_rescanRequested = true;
        m_blocked = true;
    }
}

void KisPageHistoryCollector::queueWorkLocked(Work &work) noexcept
{
    // Protection can disappear between slices, when the root cookie is gone
    // but the cursor has already passed versions that must now be revisited.
    if (work.scanning) work.repeat = true;
    if (!work.is_linked()) m_ready.push_back(work);
}

void KisPageHistoryCollector::disposeWork(Work *work) noexcept
{
    Q_ASSERT(!work->is_linked() && !work->indexHook.is_linked() && !work->reachability);
    std::destroy_at(work);
    m_workAllocator.deallocate(work, 1);
}

void KisPageHistoryCollector::endScanLocked(Work &work, bool finished)
{
    if (work.reachability) {
        m_epochs.cancelReachabilityScan(work.reachability);
        work.reachability = 0;
    }
    work.candidates.clear();
    if (finished) {
        work.reachableMask = 0;
        if (work.scanning) --m_activeScans;
        work.scanning = false;
    }
}

void KisPageHistoryCollector::Work::markReachable(const KisPageVersion &version) noexcept
{
    // Metadata's original history chain orders one key by generation/revision.
    // Resolve a root into that set without scanning every candidate again.
    const auto found = std::lower_bound(candidates.cbegin(), candidates.cend(), version,
        [](const KisPageVersion &a, const KisPageVersion &b) {
            return std::tie(a.generation.value, a.defaultPixelRevision)
                < std::tie(b.generation.value, b.defaultPixelRevision);
        });
    if (found != candidates.cend() && *found == version)
        reachableMask |= quint32(1) << size_t(found - candidates.cbegin());
}

KisPageHistoryCollector::SliceResult KisPageHistoryCollector::collectSliceLocked(
    Work &work, quint64 &visitedVersions, qsizetype &rootBudget)
{
    if (!work.reachability) {
        if (!rootBudget || (!work.scanning && m_activeScans == PageAdmissionBudget)) return SliceResult::Ready;
        if (!work.scanning) {
            work.scanning = true;
            ++m_activeScans;
        }
        const auto slice = m_metadata.historySlice(work.key, work.after, work.candidates, VersionScanBudget);
        visitedVersions += quint64(work.candidates.size());
        if (work.candidates.empty()) {
            endScanLocked(work, true);
            m_ready.erase(m_ready.iterator_to(work));
            if (!slice.total) {
                m_work.erase_and_dispose(m_work.iterator_to(work), [this](Work *value) { disposeWork(value); });
                return SliceResult::Removed;
            }
            if (work.repeat) {
                work.repeat = false;
                work.after = {};
                m_ready.push_back(work);
            }
            return SliceResult::Ready;
        }
        work.reachableMask = 0;
        work.sliceAfter = slice.after;
        work.historicalCount = slice.total;
        ++m_reachabilityRefreshes;
    }
    ++m_pagesVisited;
    if (!kisOnPageStoreReclamationThread()) ++m_foregroundPagesVisited;
    const auto roots = m_epochs.visitReachableVersions(work.key, work.reachability, rootBudget, &work,
        +[](void *context, const KisPageVersion &version) {
            static_cast<Work *>(context)->markReachable(version);
        });
    rootBudget -= roots.rootsVisited;
    m_reachabilityRootsVisited += quint64(roots.rootsVisited);
    if (roots.capacityUnavailable) return SliceResult::Blocked;
    if (!roots.valid) {
        endScanLocked(work);
        work.reachableMask = 0;
        work.after = {};
        ++m_reachabilityRestarts;
        return SliceResult::Ready;
    }
    if (!roots.complete) return SliceResult::Ready;

    quint32 removed = 0;
    if (!m_metadata.discardHistory(work.key, work.candidates.data(), qsizetype(work.candidates.size()),
                                  work.reachableMask, &removed)) return SliceResult::Blocked;
    work.historicalCount -= qsizetype(qPopulationCount(removed));
    work.after = work.sliceAfter;
    endScanLocked(work, !work.after.isValid());
    if (!work.after.isValid()) {
        if (work.repeat) {
            work.repeat = false;
            work.after = {};
        }
        else {
            m_ready.erase(m_ready.iterator_to(work));
            if (!work.historicalCount) {
                m_work.erase_and_dispose(m_work.iterator_to(work), [this](Work *value) { disposeWork(value); });
                return SliceResult::Removed;
            }
        }
    }
    return SliceResult::Ready;
}

bool KisPageHistoryCollector::collectPassLocked()
{
    if (m_rescanRequested) {
        m_rescanRequested = false;
        m_metadata.visitPageKeys(this, +[](void *p, const KisPageKey &key) {
            static_cast<KisPageHistoryCollector *>(p)->requestKeyLocked(key, true);
        }, true);
    }
    std::array<Work *, PageAdmissionBudget> keys{};
    qsizetype count = 0;
    for (auto &work : m_ready) {
        keys[size_t(count++)] = &work;
        if (count == PageAdmissionBudget) break;
    }
    quint64 versions = 0;
    qsizetype rootsRemaining = RootVisitBudget;
    bool blocked = m_rescanRequested;
    for (qsizetype i = 0; i < count; ++i) {
        auto &work = *keys[size_t(i)];
        const qsizetype allowance = rootsRemaining / (count - i);
        qsizetype remaining = allowance;
        SliceResult result = SliceResult::Ready;
        try {
            result = collectSliceLocked(work, versions, remaining);
            blocked = (result == SliceResult::Blocked) || blocked;
        }
        catch (const std::bad_alloc &) { blocked = true; }
        rootsRemaining -= allowance - remaining;
        // The Store gate never left this pass. Only this slice can dispose its
        // original Work; otherwise its pointer remains the round-robin link.
        if (result != SliceResult::Removed && work.is_linked())
            m_ready.splice(m_ready.end(), m_ready, m_ready.iterator_to(work));
    }
    m_maximumRootsPerPass = std::max(m_maximumRootsPerPass, quint64(RootVisitBudget - rootsRemaining));
    m_maximumVersionsPerPass = std::max(m_maximumVersionsPerPass, versions);
    m_maximumPagesPerPass = std::max(m_maximumPagesPerPass, quint64(count));
    return blocked;
}

void KisPageHistoryCollector::collectUnreachableLocked(bool scanAll)
{
    if (scanAll) {
        // Every accepted key stays indexed while history remains. Do not
        // visit unchanged pages merely because a retained root was released.
        for (auto &work : m_work) queueWorkLocked(work);
    }
    if (!m_backgroundReclamation || m_closing || m_automaticWakeupsStopped) {
        while (!m_ready.empty() || m_rescanRequested) {
            if (collectPassLocked()) {
                m_blocked = true;
                break;
            }
        }
    }
    scheduleLocked();
}

bool KisPageHistoryCollector::hasSemanticWorkLocked() const
{
    return m_epochs.hasCollectionWork() || !m_ready.empty() || m_rescanRequested
        || (m_hasAbandonedMutations && m_hasAbandonedMutations(m_ownerContext));
}

bool KisPageHistoryCollector::enqueueTask(KisPageHistoryWakeContext &context)
{
    if (!context.accepting) return false;
    if (context.jobScheduled) return true;
    int count = m_ownerLifetimeReferences.loadAcquire();
    for (;;) {
        if (count <= 0 || count == std::numeric_limits<int>::max()) return false;
        if (m_ownerLifetimeReferences.testAndSetOrdered(count, count + 1)) break;
        count = m_ownerLifetimeReferences.loadAcquire();
    }
    context.jobScheduled = true;
    ++context.activities;
    if (kisEnqueuePageStoreReclamation(m_task.get())) return true;
    context.jobScheduled = false;
    --context.activities;
    context.accepting = false;
    context.retry.cancel();
    context.timerDeadline = 0;
    // Runtime rejection is terminal (its worker is stopping). Facade exit
    // closes this same notification gate before dropping its reference, so
    // rollback cannot be the last owner or run destruction on the monitor.
    const bool alive = m_ownerLifetimeReferences.deref();
    Q_ASSERT(alive); Q_UNUSED(alive);
    context.idle.wakeAll();
    return false;
}

void KisPageHistoryCollector::prepareTask(KisBackingBudgetController &budget)
{
    if (m_task) return;
    Q_ASSERT(m_work.empty());
    m_workAllocator = KisMutationStorageAllocator<Work>(&budget);
    auto context = std::allocate_shared<KisPageHistoryWakeContext>(
        KisMutationStorageAllocator<KisPageHistoryWakeContext>::retained(&budget));
    context->collector = this;
    auto task = kisPreparePageStoreReclamation([this] {
        bool collectAbandoned = false;
        bool processRetirements = false;
        {
            QMutexLocker lock(&m_ownerMutex);
            if (!m_closing && m_operational && m_backgroundReclamation && !m_automaticWakeupsStopped) {
                const qint64 now = QDeadlineTimer::current().deadline();
                const qint64 idleDeadline = m_nextIdleDeadline ? m_nextIdleDeadline(m_ownerContext) : 0;
                if (idleDeadline && idleDeadline <= now)
                    m_collectIdleCapacity(m_ownerContext, now, IdleCapacityBudget);
                // An Idle-only wake must neither retry blocked history nor
                // reset its backoff. Both kinds use the same original task.
                if (hasSemanticWorkLocked() && (!m_blocked || (m_pressureDeadline && now >= m_pressureDeadline))) {
                    m_blocked = false;
                    m_pressureDeadline = 0;
                    m_epochs.collectFinishedTransactions(VersionScanBudget);
                    m_epochs.collectUnretainedRoots(VersionScanBudget);
                    m_blocked = collectPassLocked() || m_blocked;
                    collectAbandoned = m_hasAbandonedMutations && m_hasAbandonedMutations(m_ownerContext);
                    processRetirements = true;
                }
            }
        }
        // Scope cancellation acquires scope -> owner, and can release the
        // owner gate for provider destruction. Keep that original lock order.
        if (collectAbandoned) m_collectAbandonedMutations(m_ownerContext);
        if (processRetirements) m_retirementQueue.process(8);
        if (collectAbandoned) {
            QMutexLocker lock(&m_ownerMutex);
            m_blocked = m_hasAbandonedMutations(m_ownerContext) || m_blocked;
        }
    }, &budget, +[](void *value) {
        auto *collector = static_cast<KisPageHistoryCollector *>(value);
        const auto context = collector->m_wakeContext;
        {
            QMutexLocker lock(&collector->m_ownerMutex);
            {
                QMutexLocker notificationLock(&context->mutex);
                context->jobScheduled = false;
            }
            if (!collector->m_blocked) collector->m_retryDelayMs = 1;
            // A timer consumed while this pass was running needs no second
            // ready flag: recompute real deadlines/work before dropping it.
            collector->scheduleLocked();
        }
        collector->m_releaseOwnerLifetime(collector->m_ownerContext);
        QMutexLocker notificationLock(&context->mutex);
        --context->activities;
        context->idle.wakeAll();
    }, this);
    context->retry = KisPageReclamationDelay(KisPageReadinessCallback(
        [weak = std::weak_ptr<KisPageHistoryWakeContext>(context)] {
            const auto state = weak.lock();
            if (!state) return;
            QMutexLocker lock(&state->mutex);
            if (!state->collector || !state->accepting || !state->retry.takeReady()) return;
            ++state->retryWakeups;
            state->timerDeadline = 0;
            // No Store gate or business work on the process-wide monitor.
            // A queued/running task already owns the next readiness check.
            state->collector->enqueueTask(*state);
        }, &budget), &budget);
    if (!context->retry.isValid()) throw std::bad_alloc();
    m_task = std::move(task);
    m_wakeContext = std::move(context);
}

void KisPageHistoryCollector::scheduleLocked()
{
    if (!m_wakeContext) return;
    auto &context = *m_wakeContext;
    if (m_closing || m_automaticWakeupsStopped || !m_backgroundReclamation || !m_operational) {
        m_pressureDeadline = 0;
        QMutexLocker notificationLock(&context.mutex);
        context.accepting = false;
        context.retry.cancel();
        context.timerDeadline = 0;
        context.idle.wakeAll();
        return;
    }
    const qint64 now = QDeadlineTimer::current().deadline();
    qint64 deadline = m_nextIdleDeadline ? m_nextIdleDeadline(m_ownerContext) : 0;
    const bool idleDue = deadline && deadline <= now;
    const bool work = hasSemanticWorkLocked();
    if (!work || (m_pressureDeadline && now >= m_pressureDeadline)) {
        m_blocked = false;
        m_pressureDeadline = 0;
        if (!work) m_retryDelayMs = 1;
    }
    if (work && m_blocked && !m_pressureDeadline) {
        m_pressureDeadline = now + m_retryDelayMs;
        m_retryDelayMs = std::min(100, m_retryDelayMs * 2);
    }
    QMutexLocker notificationLock(&context.mutex);
    context.accepting = true;
    if ((idleDue || (work && !m_blocked)) && !enqueueTask(context)) return;
    // A task owns the due prefix and will continue it in bounded passes.
    // Do not repeatedly arm a past deadline while it is queued/running.
    if (idleDue && context.jobScheduled) deadline = 0;
    if (work && m_blocked && (!deadline || m_pressureDeadline < deadline))
        deadline = m_pressureDeadline;
    if (deadline != context.timerDeadline) {
        if (deadline) {
            context.timerDeadline = context.retry.arm(int(std::max<qint64>(1, deadline - now))) ? deadline : 0;
        } else {
            context.retry.cancel();
            context.timerDeadline = 0;
        }
    }
}

void KisPageHistoryCollector::collectAfterEpochChangeLocked(bool rescanHistory)
{
    if (!m_backgroundReclamation) {
        m_epochs.collectFinishedTransactions();
        m_epochs.collectUnretainedRoots();
    }
    // Root protection changed, not the set of registered history keys.
    // Missing-key recovery alone needs m_rescanRequested's metadata walk.
    // This also pumps foreground abort, which has no later publish callback.
    collectUnreachableLocked(rescanHistory);
}

void KisPageHistoryCollector::waitForIdleLocked()
{
    // Observe in-flight work through the one notification admission. Future
    // pressure retries may retain work; idle is deliberately not a drain.
    const auto context = m_wakeContext;
    if (!context) return;
    for (;;) {
        QMutexLocker notificationLock(&context->mutex);
        if (!context->activities) return;
        m_ownerMutex.unlock();
        while (context->activities) context->idle.wait(&context->mutex);
        notificationLock.unlock();
        m_ownerMutex.lock();
    }
}

void KisPageHistoryCollector::stopAutomaticWakeups()
{
    QMutexLocker lock(&m_ownerMutex);
    m_automaticWakeupsStopped = true;
    m_pressureDeadline = 0;
    if (m_wakeContext) {
        QMutexLocker notificationLock(&m_wakeContext->mutex);
        m_wakeContext->collector = nullptr;
        m_wakeContext->accepting = false;
        m_wakeContext->retry.cancel();
        m_wakeContext->timerDeadline = 0;
        m_wakeContext->idle.wakeAll();
    }
}

void KisPageHistoryCollector::clearLocked()
{
    if (m_wakeContext) {
        QMutexLocker notificationLock(&m_wakeContext->mutex);
        Q_ASSERT(!m_wakeContext->jobScheduled);
        m_wakeContext->accepting = false;
        m_wakeContext->retry.cancel();
        m_wakeContext->timerDeadline = 0;
    }
    m_pressureDeadline = 0;
    m_ready.clear();
    for (auto &work : m_work) endScanLocked(work, true);
    m_work.clear_and_dispose([this](Work *work) { disposeWork(work); });
    m_rescanRequested = false;
}

KisPageHistoryCollectorSnapshot KisPageHistoryCollector::snapshotLocked() const
{
    KisPageHistoryCollectorSnapshot result;
    result.pendingPages = qsizetype(m_ready.size());
    result.deferredPages = qsizetype(m_work.size()) - result.pendingPages;
    result.activeScans = m_activeScans;
    if (m_wakeContext) {
        QMutexLocker notificationLock(&m_wakeContext->mutex);
        result.retryWakeups = m_wakeContext->retryWakeups;
        result.retryScheduled = m_wakeContext->timerDeadline != 0;
        result.jobScheduled = m_wakeContext->jobScheduled;
    }
    for (const auto &work : m_work) {
        result.cachedReachableVersions += quint64(qPopulationCount(work.reachableMask));
    }
    result.pagesVisited = m_pagesVisited;
    result.foregroundPagesVisited = m_foregroundPagesVisited;
    result.maximumPagesPerPass = m_maximumPagesPerPass;
    result.maximumVersionsPerPass = m_maximumVersionsPerPass;
    result.reachabilityRefreshes = m_reachabilityRefreshes;
    result.reachabilityRootsVisited = m_reachabilityRootsVisited;
    result.maximumRootsPerPass = m_maximumRootsPerPass;
    result.reachabilityRestarts = m_reachabilityRestarts;
    return result;
}

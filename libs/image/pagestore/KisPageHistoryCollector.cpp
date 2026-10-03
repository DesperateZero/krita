/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisPageHistoryCollector_p.h"

#include "KisPageStoreReclamation_p.h"

#include <QMutexLocker>

#include <algorithm>
#include <utility>

struct KisPageHistoryWakeContext
{
    QMutex mutex;
    KisPageHistoryCollector *collector = nullptr;
    QAtomicInt *references = nullptr;
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
    RemoveDescriptor removeDescriptor,
    HasAbandonedMutations hasAbandonedMutations,
    CollectAbandonedMutations collectAbandonedMutations)
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
    , m_removeDescriptor(removeDescriptor)
    , m_hasAbandonedMutations(hasAbandonedMutations)
    , m_collectAbandonedMutations(collectAbandonedMutations)
{
    static_assert(PageAdmissionBudget <= KisImageEpochReferenceModel::ReachabilityScanLimit);
    static_assert(RootVisitBudget <= KisImageEpochReferenceModel::ReachabilityRootBudget);
    static_assert(VersionScanBudget <= 32);
    Q_ASSERT(m_ownerContext);
    Q_ASSERT(m_releaseOwnerLifetime);
    Q_ASSERT(m_removeDescriptor);
    Q_ASSERT(bool(m_hasAbandonedMutations) == bool(m_collectAbandonedMutations));
}

KisPageHistoryCollector::~KisPageHistoryCollector()
{
    Q_ASSERT(!m_jobScheduled);
    m_retry.cancel();
    if (m_wakeContext) {
        QMutexLocker lock(&m_wakeContext->mutex);
        m_wakeContext->collector = nullptr;
    }
    clearLocked();
}

void KisPageHistoryCollector::requestKeyLocked(const KisPageKey &key) noexcept
{
    try {
        auto [entry, inserted] = m_work.try_emplace(key);
        auto &work = entry->second;
        work.key = key;
        // Protection can disappear between slices, when the root cookie is gone
        // but the cursor has already passed versions that must now be revisited.
        if (!inserted && work.scanning)
            work.repeat = true;
        if (!work.is_linked()) m_ready.push_back(work);
    } catch (const std::bad_alloc &) {
        // Metadata keeps every historical version until collection accepts
        // its retirement. The original collector retains the missing-key scan,
        // for both foreground and background entry points.
        m_rescanRequested = true;
        m_blocked = true;
    }
}

void KisPageHistoryCollector::endScanLocked(Work &work, bool finished)
{
    if (work.reachability) {
        m_epochs.endReachabilityScan(work.reachability);
        work.reachability = 0;
    }
    work.candidateCount = 0;
    if (finished) {
        work.reachableMask = 0;
        if (work.scanning) --m_activeScans;
        work.scanning = false;
    }
}

bool KisPageHistoryCollector::collectSliceLocked(
    Work &work, quint64 &visitedVersions, qsizetype &rootBudget)
{
    const auto markReachable = [&](const KisPageVersion &version) {
        for (qsizetype i = 0; i < work.candidateCount; ++i)
            if (work.candidates[size_t(i)] == version) work.reachableMask |= quint32(1) << i;
    };
    if (!work.reachability) {
        if (!rootBudget || (!work.scanning && m_activeScans == PageAdmissionBudget)) return true;
        if (!work.scanning) {
            work.scanning = true;
            ++m_activeScans;
        }
        const auto slice = m_metadata.historySlice(work.key, work.after, VersionScanBudget);
        visitedVersions += quint64(slice.count);
        if (!slice.count) {
            endScanLocked(work, true);
            m_ready.erase(m_ready.iterator_to(work));
            if (!slice.total) m_work.erase(work.key);
            else if (work.repeat) {
                work.repeat = false;
                work.after = {};
                m_ready.push_back(work);
            }
            return true;
        }
        work.candidateCount = slice.count;
        work.reachableMask = 0;
        for (qsizetype i = 0; i < work.candidateCount; ++i)
            work.candidates[size_t(i)] = slice.versions[size_t(i)];
        work.sliceAfter = slice.after;
        work.historicalCount = slice.total;
        const auto start = m_epochs.beginReachabilityScan(work.key);
        if (!start.cookie) return false;
        work.reachability = start.cookie;
        markReachable(start.current);
        --rootBudget;
        ++m_reachabilityRootsVisited;
        ++m_reachabilityRefreshes;
    }
    ++m_pagesVisited;
    if (!kisOnPageStoreReclamationThread()) ++m_foregroundPagesVisited;
    const auto roots = m_epochs.advanceReachabilityScan(work.reachability, rootBudget);
    rootBudget -= roots.rootsVisited;
    m_reachabilityRootsVisited += quint64(roots.rootsVisited);
    if (!roots.valid) {
        endScanLocked(work);
        work.reachableMask = 0;
        work.after = {};
        ++m_reachabilityRestarts;
        return true;
    }
    for (qsizetype i = 0; i < roots.rootsVisited; ++i) markReachable(roots.versions[size_t(i)]);
    if (!roots.complete) return true;

    quint32 removed = 0;
    if (!m_metadata.discardHistory(work.key, work.candidates.data(), work.candidateCount,
                                  work.reachableMask, &removed)) return false;
    for (qsizetype i = 0; i < work.candidateCount; ++i) {
        if (removed & (quint32(1) << i)) {
            --work.historicalCount;
            m_removeDescriptor(m_ownerContext, work.candidates[size_t(i)]);
        }
    }
    work.after = work.sliceAfter;
    endScanLocked(work, !work.after.isValid());
    if (!work.after.isValid()) {
        if (work.repeat) {
            work.repeat = false;
            work.after = {};
        }
        else {
            m_ready.erase(m_ready.iterator_to(work));
            if (!work.historicalCount) m_work.erase(work.key);
        }
    }
    return true;
}

bool KisPageHistoryCollector::collectPassLocked()
{
    if (m_rescanRequested) {
        m_rescanRequested = false;
        m_metadata.visitPageKeys(this, +[](void *p, const KisPageKey &key) {
            static_cast<KisPageHistoryCollector *>(p)->requestKeyLocked(key);
        });
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
        const auto key = work.key;
        const qsizetype allowance = rootsRemaining / (count - i);
        qsizetype remaining = allowance;
        try { blocked = !collectSliceLocked(work, versions, remaining) || blocked; }
        catch (const std::bad_alloc &) { blocked = true; }
        rootsRemaining -= allowance - remaining;
        auto found = m_work.find(key);
        if (found != m_work.end() && found->second.is_linked())
            m_ready.splice(m_ready.end(), m_ready, m_ready.iterator_to(found->second));
    }
    m_maximumRootsPerPass = std::max(m_maximumRootsPerPass, quint64(RootVisitBudget - rootsRemaining));
    m_maximumVersionsPerPass = std::max(m_maximumVersionsPerPass, versions);
    m_maximumPagesPerPass = std::max(m_maximumPagesPerPass, quint64(count));
    return blocked;
}

void KisPageHistoryCollector::collectUnreachableLocked(
    const KisPageKey *candidateKeys, qsizetype candidateCount, bool scanAll)
{
    if (scanAll) {
        // Every accepted key stays indexed while history remains. Do not
        // visit unchanged pages merely because a retained root was released.
        for (const auto &entry : m_work) requestKeyLocked(entry.first);
    }
    for (qsizetype i = 0; i < candidateCount; ++i) requestKeyLocked(candidateKeys[i]);
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

void KisPageHistoryCollector::prepareTask(KisBackingBudgetController &budget)
{
    if (m_task) return;
    Q_ASSERT(m_work.empty());
    m_work = WorkIndex(KeyLess{}, KisMutationStorageAllocator<std::pair<const KisPageKey, Work>>(&budget));
    auto context = std::allocate_shared<KisPageHistoryWakeContext>(
        KisMutationStorageAllocator<KisPageHistoryWakeContext>::retained(&budget));
    context->collector = this;
    context->references = &m_ownerLifetimeReferences;
    auto task = kisPreparePageStoreReclamation([this] {
        bool collectAbandoned = false;
        bool processRetirements = false;
        {
            QMutexLocker lock(&m_ownerMutex);
            m_blocked = false;
            if (!m_closing && m_operational && m_backgroundReclamation && !m_automaticWakeupsStopped) {
                m_epochs.collectFinishedTransactions(VersionScanBudget);
                m_epochs.collectUnretainedRoots(VersionScanBudget);
                m_blocked = collectPassLocked() || m_blocked;
                collectAbandoned = m_hasAbandonedMutations && m_hasAbandonedMutations(m_ownerContext);
                processRetirements = true;
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
            collector->m_jobScheduled = false;
            collector->scheduleLocked();
            collector->m_idle.wakeAll();
        }
        collector->m_releaseOwnerLifetime(collector->m_ownerContext);
        QMutexLocker notificationLock(&context->mutex);
        --context->activities;
        context->idle.wakeAll();
    }, this);
    KisPageReclamationDelay retry(KisPageReadinessCallback(
        [weak = std::weak_ptr<KisPageHistoryWakeContext>(context)] {
            const auto state = weak.lock();
            if (!state) return;
            KisPageHistoryCollector *collector;
            {
                QMutexLocker lock(&state->mutex);
                collector = state->collector;
                if (!collector) return;
                state->references->ref();
                ++state->activities;
            }
            {
                QMutexLocker lock(&collector->m_ownerMutex);
                if (collector->m_retry.takeReady()) {
                    ++collector->m_retryWakeups;
                    collector->m_retryScheduled = false;
                    collector->m_blocked = false;
                    collector->scheduleLocked();
                }
            }
            collector->m_releaseOwnerLifetime(collector->m_ownerContext);
            QMutexLocker notificationLock(&state->mutex);
            --state->activities;
            state->idle.wakeAll();
        }, &budget), &budget);
    if (!retry.isValid()) throw std::bad_alloc();
    m_task = std::move(task);
    m_wakeContext = std::move(context);
    m_retry = std::move(retry);
}

void KisPageHistoryCollector::scheduleLocked()
{
    if (m_closing || m_automaticWakeupsStopped) {
        m_retry.cancel();
        m_retryScheduled = false;
        m_idle.wakeAll();
        return;
    }
    if (!m_backgroundReclamation || m_jobScheduled || !m_operational || m_retryScheduled) return;
    if (!m_epochs.hasCollectionWork() && m_ready.empty() && !m_rescanRequested
        && !(m_hasAbandonedMutations && m_hasAbandonedMutations(m_ownerContext))) return;
    if (m_blocked) {
        m_retryScheduled = m_retry.arm(m_retryDelayMs);
        m_retryDelayMs = std::min(100, m_retryDelayMs * 2);
        return;
    }
    m_retryDelayMs = 1;
    m_jobScheduled = true;
    m_ownerLifetimeReferences.ref();
    { QMutexLocker notificationLock(&m_wakeContext->mutex); ++m_wakeContext->activities; }
    Q_ASSERT(m_task);
    if (!kisEnqueuePageStoreReclamation(m_task.get())) {
        m_jobScheduled = false;
        const bool alive = m_ownerLifetimeReferences.deref();
        Q_ASSERT(alive); Q_UNUSED(alive);
        QMutexLocker notificationLock(&m_wakeContext->mutex);
        --m_wakeContext->activities;
        m_wakeContext->idle.wakeAll();
        m_idle.wakeAll();
    }
}

void KisPageHistoryCollector::collectEpochBookkeepingLocked(bool rescanHistory)
{
    if (m_backgroundReclamation) {
        m_rescanRequested = m_rescanRequested || rescanHistory;
        scheduleLocked();
    } else {
        m_epochs.collectFinishedTransactions();
        m_epochs.collectUnretainedRoots();
    }
}

void KisPageHistoryCollector::waitForIdleLocked()
{
    // Idle observes in-flight jobs, as the physical queue does. A future retry
    // can retain semantic work under permanent pressure; it is not a drain.
    for (;;) {
        while (m_jobScheduled) m_idle.wait(&m_ownerMutex);
        if (!m_wakeContext) return;
        QMutexLocker notificationLock(&m_wakeContext->mutex);
        if (!m_wakeContext->activities) return;
        // The retained context survives the callback's final root release.
        // Drop the owner gate while that callback may still need it.
        m_ownerMutex.unlock();
        while (m_wakeContext->activities)
            m_wakeContext->idle.wait(&m_wakeContext->mutex);
        notificationLock.unlock();
        m_ownerMutex.lock();
    }
}

void KisPageHistoryCollector::stopAutomaticWakeups()
{
    QMutexLocker lock(&m_ownerMutex);
    m_automaticWakeupsStopped = true;
    if (m_wakeContext) {
        QMutexLocker notificationLock(&m_wakeContext->mutex);
        m_wakeContext->collector = nullptr;
    }
    m_retry.cancel();
    m_retryScheduled = false;
    m_idle.wakeAll();
}

void KisPageHistoryCollector::clearLocked()
{
    Q_ASSERT(!m_jobScheduled);
    m_retry.cancel();
    m_retryScheduled = false;
    m_ready.clear();
    for (auto &entry : m_work) endScanLocked(entry.second, true);
    m_work.clear();
    m_rescanRequested = false;
}

KisPageHistoryCollectorSnapshot KisPageHistoryCollector::snapshotLocked() const
{
    KisPageHistoryCollectorSnapshot result;
    result.pendingPages = qsizetype(m_ready.size());
    result.deferredPages = qsizetype(m_work.size()) - result.pendingPages;
    result.activeScans = m_activeScans;
    result.retryWakeups = m_retryWakeups;
    result.retryScheduled = m_retryScheduled;
    for (const auto &entry : m_work) {
        result.cachedReachableVersions += quint64(qPopulationCount(entry.second.reachableMask));
    }
    result.jobScheduled = m_jobScheduled;
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

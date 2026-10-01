/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageHistoryCollector_p.h"

#include "KisPageStoreReclamation_p.h"

#include <QMutexLocker>

#include <algorithm>
#include <utility>

namespace {
bool retirementEligible(const KisPageVersionStateSnapshot &version)
{
    if (!version.capturedReadViews.isEmpty() ||
        (!version.authority.isValid() && !version.isVirtualDefault()))
        return false;
    for (const auto &replica : version.replicas) {
        if ((replica.validity != KisReplicaValidity::Valid &&
             replica.validity != KisReplicaValidity::Failed) ||
            !replica.readLeases.isEmpty() || replica.pinCount != 0 ||
            !replica.pendingLastUses.isEmpty() || replica.activeOperation.isValid())
            return false;
    }
    return true;
}
}

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
    RemoveDescriptor removeDescriptor)
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
{
    static_assert(PageAdmissionBudget <= KisImageEpochReferenceModel::ReachabilityScanLimit);
    static_assert(RootVisitBudget <= KisImageEpochReferenceModel::ReachabilityRootBudget);
    static_assert(VersionScanBudget <= 32); // One reachability bit per candidate.
    Q_ASSERT(m_ownerContext);
    Q_ASSERT(m_releaseOwnerLifetime);
    Q_ASSERT(m_removeDescriptor);
}

KisPageHistoryCollector::~KisPageHistoryCollector()
{
    Q_ASSERT(!m_jobScheduled);
    for (const auto &scan : m_scans) m_epochs.endReachabilityScan(scan.reachability);
}

void KisPageHistoryCollector::requestKeyLocked(const KisPageKey &key)
{
    auto scan = m_scans.find(key);
    if (scan != m_scans.end()) scan->repeat = true;
    if (!m_queued.contains(key)) m_pending.insert(key);
}

void KisPageHistoryCollector::queueContinuationLocked(const KisPageKey &key)
{
    if (m_queued.contains(key)) return;
    m_pending.remove(key);
    m_queued.insert(key);
    m_ready.push_back(key);
}

QVector<KisPageTransitionEffect>
KisPageHistoryCollector::collectUnreachableLocked(
    const KisPageKey *candidateKeys, qsizetype candidateCount,
    bool scanAll)
{
    if (m_backgroundReclamation) {
        for (qsizetype i = 0; i < candidateCount; ++i) requestKeyLocked(candidateKeys[i]);
        m_rescanRequested = m_rescanRequested || scanAll;
        scheduleLocked();
        return {};
    }
    return collectBatchLocked(candidateKeys, candidateCount, scanAll);
}

QVector<KisPageTransitionEffect> KisPageHistoryCollector::collectBatchLocked(
    const KisPageKey *candidateKeys, qsizetype candidateCount,
    bool scanAll)
{
    QVector<KisPageKey> keys;
    if (scanAll) {
        keys = m_metadata.pageKeys();
    } else {
        QSet<KisPageKey> seen;
        keys.reserve(candidateCount);
        for (qsizetype i = 0; i < candidateCount; ++i) {
            const KisPageKey &key = candidateKeys[i];
            if (seen.contains(key)) continue;
            seen.insert(key);
            keys.append(key);
        }
    }
    const QSet<KisPageVersion> reachable =
        m_epochs.reachablePageVersions(keys);
    QVector<KisPageTransitionEffect> effects;
    for (const KisPageKey &key : keys) {
        KisPageVersion after;
        qsizetype historicalCount = 0;
        qsizetype discardedCount = 0;
        bool exists = false;
        bool failed = false;
        for (;;) {
            const auto history = m_metadata.historySlice(key, after, VersionScanBudget);
            if (!history.exists) break;
            if (!exists) historicalCount = history.total;
            exists = true;
            KisPageTransition discard;
            discard.kind = KisPageTransitionKind::DiscardHistoricalVersions;
            for (const auto &version : history.versions) {
                if (reachable.contains(version.version)) continue;
                if (retirementEligible(version)) discard.versions.append(version.version);
            }
            if (!discard.versions.isEmpty()) {
                const auto discarded = m_metadata.applyOwner(key, discard);
                if (!discarded.accepted) {
                    failed = true;
                    break;
                }
                effects += discarded.effects;
                discardedCount += discard.versions.size();
                for (const auto &version : discard.versions)
                    m_removeDescriptor(m_ownerContext, version);
            }
            if (!history.after.isValid()) break;
            after = history.after;
        }
        if (!exists) continue;
        ++m_pagesVisited;
        if (!kisOnPageStoreReclamationThread()) ++m_foregroundPagesVisited;
        if (failed || historicalCount > discardedCount) m_deferred.insert(key);
        else m_deferred.remove(key);
    }
    return effects;
}

QVector<KisPageTransitionEffect> KisPageHistoryCollector::collectSliceLocked(
    const KisPageKey &key, quint64 &visitedVersions, qsizetype &rootBudget)
{
    auto &scan = m_scans[key];
    ++m_pagesVisited;
    const auto markReachable = [&](const KisPageVersion &version) {
        for (qsizetype i = 0; i < scan.candidates.size(); ++i)
            if (scan.candidates[i] == version) scan.reachableMask |= quint32(1) << i;
    };
    if (!scan.reachability) {
        if (!rootBudget) { queueContinuationLocked(key); return {}; }
        const auto slice = m_metadata.historySlice(key, scan.after, VersionScanBudget);
        visitedVersions += quint64(slice.versions.size());
        if (slice.versions.isEmpty()) {
            const bool repeat = scan.repeat;
            if (!slice.total) m_deferred.remove(key);
            m_scans.remove(key);
            if (repeat) queueContinuationLocked(key);
            return {};
        }
        scan.candidates.clear();
        scan.reachableMask = 0;
        for (const auto &version : slice.versions) scan.candidates.append(version.version);
        scan.sliceAfter = slice.after;
        scan.historicalCount = slice.total;
        const auto start = m_epochs.beginReachabilityScan(key);
        if (!start.cookie) {
            // Exhausted cursor storage/identity grants no permission to retire.
            m_deferred.insert(key);
            m_scans.remove(key);
            return {};
        }
        scan.reachability = start.cookie;
        markReachable(start.current);
        --rootBudget;
        ++m_reachabilityRootsVisited;
        ++m_reachabilityRefreshes;
    }
    const auto roots = m_epochs.advanceReachabilityScan(scan.reachability, rootBudget);
    rootBudget -= roots.rootsVisited;
    m_reachabilityRootsVisited += quint64(roots.rootsVisited);
    if (!roots.valid) {
        m_epochs.endReachabilityScan(scan.reachability);
        scan.reachability = 0;
        scan.candidates.clear();
        scan.after = {}; // The changed key can insert history below the cursor.
        ++m_reachabilityRestarts;
        queueContinuationLocked(key);
        return {};
    }
    for (qsizetype i = 0; i < roots.rootsVisited; ++i) markReachable(roots.versions[size_t(i)]);
    if (!roots.complete) { queueContinuationLocked(key); return {}; }

    // Root validity was just rechecked under the epoch gate. The caller still
    // holds the store owner gate, excluding publication/capture until detach.
    // Re-read only these candidate identities; never reuse old pin/writer state.
    KisPageTransition discard;
    discard.kind = KisPageTransitionKind::DiscardHistoricalVersions;
    for (qsizetype i = 0; i < scan.candidates.size(); ++i) {
        if (scan.reachableMask & (quint32(1) << i)) continue;
        KisPageStateSnapshot current;
        const auto &identity = scan.candidates[i];
        if (!m_metadata.versionSnapshot(identity, &current)) continue;
        const auto *version = current.findVersion(identity);
        if (version && version->publication == KisPagePublicationState::Historical &&
            retirementEligible(*version)) discard.versions.append(identity);
    }
    QVector<KisPageTransitionEffect> effects;
    qsizetype discardedCount = 0;
    if (!discard.versions.isEmpty()) {
        const auto discarded = m_metadata.applyOwner(key, discard);
        if (discarded.accepted) {
            effects = discarded.effects;
            discardedCount = discard.versions.size();
            for (const auto &version : discard.versions) m_removeDescriptor(m_ownerContext, version);
        }
    }
    if (scan.historicalCount > discardedCount) m_deferred.insert(key);
    else m_deferred.remove(key);
    m_epochs.endReachabilityScan(scan.reachability);
    scan.reachability = 0;
    scan.candidates.clear();
    if (scan.sliceAfter.isValid()) {
        scan.after = scan.sliceAfter;
        queueContinuationLocked(key);
    } else {
        const bool repeat = scan.repeat;
        m_scans.remove(key);
        if (repeat) queueContinuationLocked(key);
    }
    return effects;
}

void KisPageHistoryCollector::prepareTask(KisBackingBudgetController &budget)
{
    if (m_task) return;
    m_task = kisPreparePageStoreReclamation([this] {
        QVector<KisPageTransitionEffect> effects;
        {
            QMutexLocker lock(&m_ownerMutex);
            if (!m_closing && m_operational) {
                m_epochs.collectFinishedTransactions(VersionScanBudget);
                m_epochs.collectUnretainedRoots(VersionScanBudget);
                if (m_pending.isEmpty() && m_ready.empty() &&
                    m_rescanRequested) {
                    m_pending.swap(m_deferred);
                    m_rescanRequested = false;
                }
                // At most 16 active scans, each with <=32 candidate identities
                // and one bit per candidate. No history-sized reachability set.
                const qsizetype admissionSlots = std::max(
                    qsizetype(0), PageAdmissionBudget - m_queued.size());
                for (qsizetype admitted = 0;
                     admitted < admissionSlots && !m_pending.isEmpty();
                     ++admitted) {
                    const auto it = m_pending.begin();
                    const auto key = *it;
                    m_pending.erase(it);
                    queueContinuationLocked(key);
                }
                QVector<KisPageKey> keys;
                while (!m_ready.empty() &&
                       keys.size() < PageAdmissionBudget) {
                    keys.append(m_ready.front());
                    m_queued.remove(m_ready.front());
                    m_ready.pop_front();
                }
                quint64 versions = 0;
                qsizetype rootsRemaining = RootVisitBudget;
                for (qsizetype i = 0; i < keys.size(); ++i) {
                    // Share the pass budget between ready keys. A deep/hot
                    // first key cannot consume every root visit indefinitely.
                    const qsizetype allowance = rootsRemaining / (keys.size() - i);
                    qsizetype remaining = allowance;
                    effects += collectSliceLocked(keys[i], versions, remaining);
                    rootsRemaining -= allowance - remaining;
                }
                m_maximumRootsPerPass = std::max(m_maximumRootsPerPass,
                    quint64(RootVisitBudget - rootsRemaining));
                m_maximumVersionsPerPass =
                    std::max(m_maximumVersionsPerPass, versions);
                m_maximumPagesPerPass = std::max(
                    m_maximumPagesPerPass, quint64(keys.size()));
            }
        }
        m_retirementQueue.retireEffects(effects, m_backgroundReclamation);
    }, &budget, +[](void *value) {
        auto *collector = static_cast<KisPageHistoryCollector *>(value);
        {
            QMutexLocker lock(&collector->m_ownerMutex);
            collector->m_jobScheduled = false;
            collector->scheduleLocked();
            collector->m_idle.wakeAll();
        }
        collector->m_releaseOwnerLifetime(collector->m_ownerContext);
    }, this);
}

void KisPageHistoryCollector::scheduleLocked()
{
    if (!m_backgroundReclamation || m_jobScheduled || m_closing ||
        !m_operational) {
        return;
    }
    if (!m_epochs.hasCollectionWork() && m_pending.isEmpty() &&
        m_ready.empty() && !(m_rescanRequested && !m_deferred.isEmpty())) {
        m_rescanRequested = false;
        return;
    }
    m_jobScheduled = true;
    m_ownerLifetimeReferences.ref();
    Q_ASSERT(m_task);
    kisEnqueuePageStoreReclamation(m_task.get());
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
    while (m_jobScheduled) m_idle.wait(&m_ownerMutex);
}

QVector<KisPageKey> KisPageHistoryCollector::deferredKeysLocked() const
{
    return m_deferred.values();
}

void KisPageHistoryCollector::clearLocked()
{
    Q_ASSERT(!m_jobScheduled);
    m_pending.clear();
    m_ready.clear();
    m_queued.clear();
    for (const auto &scan : m_scans) m_epochs.endReachabilityScan(scan.reachability);
    m_scans.clear();
    m_deferred.clear();
    m_rescanRequested = false;
}

KisPageHistoryCollectorSnapshot
KisPageHistoryCollector::snapshotLocked() const
{
    KisPageHistoryCollectorSnapshot result;
    result.pendingPages = m_pending.size() + m_queued.size();
    result.deferredPages = m_deferred.size();
    result.activeScans = m_scans.size();
    for (const auto &scan : m_scans)
        result.cachedReachableVersions += quint64(qPopulationCount(scan.reachableMask));
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

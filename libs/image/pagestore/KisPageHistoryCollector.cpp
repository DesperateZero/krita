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
    Q_ASSERT(m_ownerContext);
    Q_ASSERT(m_releaseOwnerLifetime);
    Q_ASSERT(m_removeDescriptor);
}

KisPageHistoryCollector::~KisPageHistoryCollector()
{
    Q_ASSERT(!m_jobScheduled);
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
    const QVector<KisPageKey> &candidateKeys,
    bool scanAll)
{
    if (m_backgroundReclamation) {
        for (const auto &key : candidateKeys) requestKeyLocked(key);
        m_rescanRequested = m_rescanRequested || scanAll;
        scheduleLocked();
        return {};
    }
    return collectBatchLocked(candidateKeys, scanAll);
}

QVector<KisPageTransitionEffect> KisPageHistoryCollector::collectBatchLocked(
    const QVector<KisPageKey> &candidateKeys,
    bool scanAll)
{
    QVector<KisPageKey> keys;
    if (scanAll) {
        keys = m_metadata.pageKeys();
    } else {
        QSet<KisPageKey> seen;
        keys.reserve(candidateKeys.size());
        for (const KisPageKey &key : candidateKeys) {
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
    const KisPageKey &key,
    QVector<QSet<KisPageVersion>> &releasedReachability,
    quint64 &visitedVersions)
{
    auto &scan = m_scans[key];
    const auto slice = m_metadata.historySlice(key, scan.after, VersionScanBudget);
    const auto head = m_epochs.captureCommittedRoot().epoch();
    if (!slice.versions.isEmpty() && !(scan.rootEpoch == head)) {
        releasedReachability.append(std::move(scan.reachable));
        quint64 visitedRoots = 0;
        scan.reachable = m_epochs.reachablePageVersions({key}, &visitedRoots);
        m_reachabilityRootsVisited += visitedRoots;
        scan.rootEpoch = head;
        ++m_reachabilityRefreshes;
    }
    ++m_pagesVisited;
    visitedVersions += quint64(slice.versions.size());
    KisPageTransition discard;
    discard.kind = KisPageTransitionKind::DiscardHistoricalVersions;
    for (const auto &version : slice.versions) {
        if (scan.reachable.contains(version.version)) continue;
        if (retirementEligible(version)) discard.versions.append(version.version);
    }
    QVector<KisPageTransitionEffect> effects;
    qsizetype discardedCount = 0;
    if (!discard.versions.isEmpty()) {
        const auto discarded = m_metadata.applyOwner(key, discard);
        if (discarded.accepted) {
            effects = discarded.effects;
            discardedCount = discard.versions.size();
            for (const auto &version : discard.versions) {
                m_removeDescriptor(m_ownerContext, version);
            }
        }
    }
    if (slice.total > discardedCount) m_deferred.insert(key);
    else m_deferred.remove(key);
    if (slice.after.isValid()) {
        scan.after = slice.after;
        queueContinuationLocked(key);
    } else {
        const bool repeat = scan.repeat;
        releasedReachability.append(std::move(scan.reachable));
        m_scans.remove(key);
        if (repeat) queueContinuationLocked(key);
    }
    return effects;
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
    kisSchedulePageStoreReclamation([this] {
        QVector<KisPageTransitionEffect> effects;
        QVector<QSet<KisPageVersion>> releasedReachability;
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
                // A scan caches reachability for one page until all of that
                // page's history slices finish. Fill only vacant work slots;
                // otherwise long histories could admit one more cache per
                // pass and retain a document-sized second reachability set.
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
                for (const auto &key : std::as_const(keys)) {
                    effects += collectSliceLocked(
                        key, releasedReachability, versions);
                }
                m_maximumVersionsPerPass =
                    std::max(m_maximumVersionsPerPass, versions);
                m_maximumPagesPerPass = std::max(
                    m_maximumPagesPerPass, quint64(keys.size()));
            }
        }
        m_retirementQueue.retireEffects(effects, m_backgroundReclamation);
        {
            QMutexLocker lock(&m_ownerMutex);
            m_jobScheduled = false;
            scheduleLocked();
            m_idle.wakeAll();
        }
        m_releaseOwnerLifetime(m_ownerContext);
    });
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
        result.cachedReachableVersions += quint64(scan.reachable.size());
    result.jobScheduled = m_jobScheduled;
    result.pagesVisited = m_pagesVisited;
    result.foregroundPagesVisited = m_foregroundPagesVisited;
    result.maximumPagesPerPass = m_maximumPagesPerPass;
    result.maximumVersionsPerPass = m_maximumVersionsPerPass;
    result.reachabilityRefreshes = m_reachabilityRefreshes;
    result.reachabilityRootsVisited = m_reachabilityRootsVisited;
    return result;
}

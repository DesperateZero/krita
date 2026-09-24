/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_HISTORY_COLLECTOR_P_H
#define KIS_PAGE_HISTORY_COLLECTOR_P_H

#include "KisImageEpochReferenceModel.h"
#include "KisPageMetadataCoordinator.h"
#include "KisPageRetirementQueue_p.h"

#include <QAtomicInt>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QVector>
#include <QWaitCondition>

#include <deque>

struct KisPageHistoryCollectorSnapshot
{
    qsizetype pendingPages = 0;
    qsizetype deferredPages = 0;
    qsizetype activeScans = 0;
    quint64 cachedReachableVersions = 0;
    bool jobScheduled = false;
    quint64 pagesVisited = 0;
    quint64 foregroundPagesVisited = 0;
    quint64 maximumPagesPerPass = 0;
    quint64 maximumVersionsPerPass = 0;
    quint64 reachabilityRefreshes = 0;
    quint64 reachabilityRootsVisited = 0;
};

/**
 * Retained-root reachability and bounded historical-version collection.
 *
 * All methods ending in Locked require the PageStore owner mutex. Physical
 * retirement is delegated after releasing that mutex; this service never
 * owns provider operations or publishes epoch roots.
 */
class KisPageHistoryCollector final
{
public:
    static constexpr qsizetype PageAdmissionBudget = 16;
    static constexpr qsizetype VersionScanBudget = 32;

    using ReleaseOwnerLifetime = void (*)(void *context);
    using RemoveDescriptor = void (*)(void *context,
                                      const KisPageVersion &version);

    KisPageHistoryCollector(KisPageMetadataCoordinator &metadata,
                            KisImageEpochReferenceModel &epochs,
                            KisPageRetirementQueue &retirementQueue,
                            QMutex &ownerMutex,
                            QAtomicInt &ownerLifetimeReferences,
                            bool &operational,
                            bool &closing,
                            bool &backgroundReclamation,
                            void *ownerContext,
                            ReleaseOwnerLifetime releaseOwnerLifetime,
                            RemoveDescriptor removeDescriptor);
    ~KisPageHistoryCollector();

    KisPageHistoryCollector(const KisPageHistoryCollector &) = delete;
    KisPageHistoryCollector &operator=(const KisPageHistoryCollector &) = delete;
    KisPageHistoryCollector(KisPageHistoryCollector &&) = delete;
    KisPageHistoryCollector &operator=(KisPageHistoryCollector &&) = delete;

    QVector<KisPageTransitionEffect> collectUnreachableLocked(
        const QVector<KisPageKey> &candidateKeys,
        bool scanAll = false);
    void collectEpochBookkeepingLocked(bool rescanHistory = false);
    void requestKeyLocked(const KisPageKey &key);
    void scheduleLocked();
    void waitForIdleLocked();
    QVector<KisPageKey> deferredKeysLocked() const;
    void clearLocked();

    KisPageHistoryCollectorSnapshot snapshotLocked() const;

private:
    struct Scan
    {
        KisPageVersion after;
        KisImageEpochId rootEpoch;
        QSet<KisPageVersion> reachable;
        bool repeat = false;
    };

    void queueContinuationLocked(const KisPageKey &key);
    QVector<KisPageTransitionEffect> collectBatchLocked(
        const QVector<KisPageKey> &candidateKeys,
        bool scanAll);
    QVector<KisPageTransitionEffect> collectSliceLocked(
        const KisPageKey &key,
        QVector<QSet<KisPageVersion>> &releasedReachability,
        quint64 &visitedVersions);

    KisPageMetadataCoordinator &m_metadata;
    KisImageEpochReferenceModel &m_epochs;
    KisPageRetirementQueue &m_retirementQueue;
    QMutex &m_ownerMutex;
    QAtomicInt &m_ownerLifetimeReferences;
    bool &m_operational;
    bool &m_closing;
    bool &m_backgroundReclamation;
    void *m_ownerContext = nullptr;
    ReleaseOwnerLifetime m_releaseOwnerLifetime = nullptr;
    RemoveDescriptor m_removeDescriptor = nullptr;

    QSet<KisPageKey> m_deferred;
    QSet<KisPageKey> m_pending;
    std::deque<KisPageKey> m_ready;
    QSet<KisPageKey> m_queued;
    QHash<KisPageKey, Scan> m_scans;
    bool m_rescanRequested = false;
    bool m_jobScheduled = false;
    QWaitCondition m_idle;

    quint64 m_pagesVisited = 0;
    quint64 m_foregroundPagesVisited = 0;
    quint64 m_maximumPagesPerPass = 0;
    quint64 m_maximumVersionsPerPass = 0;
    quint64 m_reachabilityRefreshes = 0;
    quint64 m_reachabilityRootsVisited = 0;
};

#endif // KIS_PAGE_HISTORY_COLLECTOR_P_H

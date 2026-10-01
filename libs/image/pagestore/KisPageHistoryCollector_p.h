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
#include <QMutex>
#include <QVector>
#include <QWaitCondition>

#include <array>
#include <map>

struct KisPageHistoryWakeContext;

struct KisPageHistoryCollectorSnapshot
{
    qsizetype pendingPages = 0;
    qsizetype deferredPages = 0;
    qsizetype activeScans = 0;
    qsizetype pendingEffects = 0;
    quint64 retryWakeups = 0;
    bool retryScheduled = false;
    quint64 cachedReachableVersions = 0;
    bool jobScheduled = false;
    quint64 pagesVisited = 0;
    quint64 foregroundPagesVisited = 0;
    quint64 maximumPagesPerPass = 0;
    quint64 maximumVersionsPerPass = 0;
    quint64 reachabilityRefreshes = 0;
    quint64 reachabilityRootsVisited = 0;
    quint64 maximumRootsPerPass = 0;
    quint64 reachabilityRestarts = 0;
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
    static constexpr qsizetype RootVisitBudget = 32;

    using ReleaseOwnerLifetime = void (*)(void *context);
    using RemoveDescriptor = void (*)(void *context,
                                      const KisPageVersion &version);

    KRITAIMAGE_EXPORT KisPageHistoryCollector(KisPageMetadataCoordinator &metadata,
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
    KRITAIMAGE_EXPORT ~KisPageHistoryCollector();
    KRITAIMAGE_EXPORT void prepareTask(KisBackingBudgetController &budget);

    KisPageHistoryCollector(const KisPageHistoryCollector &) = delete;
    KisPageHistoryCollector &operator=(const KisPageHistoryCollector &) = delete;
    KisPageHistoryCollector(KisPageHistoryCollector &&) = delete;
    KisPageHistoryCollector &operator=(KisPageHistoryCollector &&) = delete;

    KRITAIMAGE_EXPORT void collectUnreachableLocked(
        const KisPageKey *candidateKeys, qsizetype candidateCount,
        bool scanAll = false);
    void collectEpochBookkeepingLocked(bool rescanHistory = false);
    void requestKeyLocked(const KisPageKey &key);
    void scheduleLocked();
    KRITAIMAGE_EXPORT void waitForIdleLocked();
    KRITAIMAGE_EXPORT void stopAutomaticWakeups();
    void clearLocked();

    KRITAIMAGE_EXPORT KisPageHistoryCollectorSnapshot snapshotLocked() const;

private:
    struct Work : boost::intrusive::list_base_hook<>
    {
        KisPageKey key;
        KisPageVersion after;
        KisPageVersion sliceAfter;
        std::array<KisPageVersion, VersionScanBudget> candidates{};
        qsizetype candidateCount = 0;
        quint64 reachability = 0;
        quint32 reachableMask = 0;
        qsizetype historicalCount = 0;
        bool scanning = false;
        bool repeat = false;
        // Metadata prepares this charged packet before authoritative detach.
        KisPageMetadataCoordinator::HistoryEffects effects;
        qsizetype nextEffect = 0;
    };
    struct KeyLess {
        bool operator()(const KisPageKey &a, const KisPageKey &b) const {
            if (a.surface.value != b.surface.value) return a.surface.value < b.surface.value;
            if (a.page.row != b.page.row) return a.page.row < b.page.row;
            return a.page.column < b.page.column;
        }
    };
    using WorkIndex = std::map<KisPageKey, Work, KeyLess,
        KisMutationStorageAllocator<std::pair<const KisPageKey, Work>>>;
    bool collectSliceLocked(Work &work,
        quint64 &visitedVersions, qsizetype &rootBudget);
    bool finishEffectsLocked(Work &work);
    bool collectPassLocked();
    void endScanLocked(Work &work, bool finished = false);

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

    WorkIndex m_work;
    boost::intrusive::list<Work> m_ready;
    qsizetype m_activeScans = 0;
    bool m_rescanRequested = false;
    bool m_jobScheduled = false;
    KisPageReclamationJobPointer m_task;
    std::shared_ptr<KisPageHistoryWakeContext> m_wakeContext;
    KisPageReclamationDelay m_retry;
    bool m_retryScheduled = false;
    bool m_blocked = false;
    bool m_automaticWakeupsStopped = false;
    int m_retryDelayMs = 1;
    quint64 m_retryWakeups = 0;
    QWaitCondition m_idle;

    quint64 m_pagesVisited = 0;
    quint64 m_foregroundPagesVisited = 0;
    quint64 m_maximumPagesPerPass = 0;
    quint64 m_maximumVersionsPerPass = 0;
    quint64 m_reachabilityRefreshes = 0;
    quint64 m_reachabilityRootsVisited = 0;
    quint64 m_maximumRootsPerPass = 0;
    quint64 m_reachabilityRestarts = 0;
};

#endif // KIS_PAGE_HISTORY_COLLECTOR_P_H

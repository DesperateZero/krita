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
#include "KisPageWaitCondition_p.h"


struct KisPageHistoryWakeContext;

struct KisPageHistoryCollectorSnapshot
{
    qsizetype pendingValues = 0;
    quint64 retryWakeups = 0;
    bool retryScheduled = false;
    bool jobScheduled = false;
    quint64 valuesVisited = 0;
    quint64 foregroundValuesVisited = 0;
    quint64 maximumValuesPerPass = 0;
    quint64 collectionPasses = 0;
    quint64 retiredValues = 0;
};

/**
 * Bounded historical-version collection from original Value live edges.
 *
 * All methods ending in Locked require the PageStore owner mutex. Physical
 * retirement is delegated after releasing that mutex; this service never
 * owns provider operations or publishes epoch roots. Abandoned mutations stay
 * on the Store's original scope links and share this prepared collection pass
 * and retry delay; their cancellation runs without the owner mutex.
 */
class KisPageHistoryCollector final
{
public:
    static constexpr qsizetype EpochCollectionBudget = 32;
    static constexpr qsizetype IdleCapacityBudget = 16;

    using ReleaseOwnerLifetime = void (*)(void *context);
    using HasAbandonedMutations = bool (*)(void *context);
    using CollectAbandonedMutations = void (*)(void *context);
    // Observing the original chain never retires capacity. Only the worker
    // invokes collection, under the Store gate and with a bounded allowance.
    using NextIdleDeadline = qint64 (*)(void *context);
    using CollectIdleCapacity = void (*)(void *context, qint64 now, qsizetype budget);

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
                            HasAbandonedMutations hasAbandonedMutations = nullptr,
                            CollectAbandonedMutations collectAbandonedMutations = nullptr,
                            NextIdleDeadline nextIdleDeadline = nullptr,
                            CollectIdleCapacity collectIdleCapacity = nullptr);
    KRITAIMAGE_EXPORT ~KisPageHistoryCollector();
    KRITAIMAGE_EXPORT void prepareTask(KisBackingBudgetController &budget);
    // Connect only after metadata configuration and successful Store adoption.
    void startLocked();

    KisPageHistoryCollector(const KisPageHistoryCollector &) = delete;
    KisPageHistoryCollector &operator=(const KisPageHistoryCollector &) = delete;
    KisPageHistoryCollector(KisPageHistoryCollector &&) = delete;
    KisPageHistoryCollector &operator=(KisPageHistoryCollector &&) = delete;

    KRITAIMAGE_EXPORT void collectUnreachableLocked();
    // Role and protection events already carry the original Values. The
    // owner only pumps this one bounded collector after changing epoch state.
    void collectAfterEpochChangeLocked();
    KRITAIMAGE_EXPORT void scheduleLocked();
    KRITAIMAGE_EXPORT void waitForIdleLocked();
    KRITAIMAGE_EXPORT void stopAutomaticWakeups();
    void clearLocked();

    KRITAIMAGE_EXPORT KisPageHistoryCollectorSnapshot snapshotLocked() const;

private:
    bool collectPassLocked();
    bool hasSemanticWorkLocked() const;
    // Requires the notification gate only. Both foreground and timer use
    // this one admission; neither can queue a second instance of the task.
    bool enqueueTask(KisPageHistoryWakeContext &context);

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
    HasAbandonedMutations m_hasAbandonedMutations = nullptr;
    CollectAbandonedMutations m_collectAbandonedMutations = nullptr;
    NextIdleDeadline m_nextIdleDeadline = nullptr;
    CollectIdleCapacity m_collectIdleCapacity = nullptr;

    KisPageReclamationJobPointer m_task;
    std::shared_ptr<KisPageHistoryWakeContext> m_wakeContext;
    qint64 m_pressureDeadline = 0;
    bool m_blocked = false;
    bool m_automaticWakeupsStopped = false;
    int m_retryDelayMs = 1;

    quint64 m_valuesVisited = 0;
    quint64 m_foregroundValuesVisited = 0;
    quint64 m_maximumValuesPerPass = 0;
    quint64 m_collectionPasses = 0;
    quint64 m_retiredValues = 0;
};

#endif // KIS_PAGE_HISTORY_COLLECTOR_P_H

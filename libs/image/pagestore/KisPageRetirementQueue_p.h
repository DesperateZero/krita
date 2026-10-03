/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_RETIREMENT_QUEUE_P_H
#define KIS_PAGE_RETIREMENT_QUEUE_P_H

#include "KisPageMetadataCoordinator.h"
#include "KisPageOwnerLedger.h"
#include "KisPageReplicaProvider.h"
#include "KisPageStore.h"
#include "KisPageStoreReclamation_p.h"
#include "KisPageRetirementRecord_p.h"

#include <QAtomicInt>
#include <QMutex>
#include <QSharedPointer>
#include <QVector>
#include <QWaitCondition>

#include <memory>

class KisBackingBudgetReservation;
struct KisPageRetirementWait;
struct KisPageRetirementWakeContext;

struct KisPageRetirementQueueSnapshot
{
    qsizetype pendingReplicas = 0;
    qsizetype activeReplicas = 0;
    quint64 pendingBytes = 0;
    bool jobScheduled = false;
    quint64 backgroundPasses = 0;
    quint64 maximumReplicasPerPass = 0;
    qsizetype peakPendingReplicas = 0;
    quint64 peakPendingBytes = 0;
    quint64 retryRequeues = 0;
    quint64 closeDrainedReplicas = 0;
    qsizetype delayedReplicas = 0;
    bool retryScheduled = false;
    quint64 retryWakeups = 0;
    quint64 maximumReplicasPerRetryWake = 0;
    int nextRetryDelayMs = 1;
};

/**
 * Completion-qualified physical retirement for detached replicas.
 *
 * The queue owns physical retirement records, including rare synchronous
 * retries still charged to their original class when Debt admission failed.
 * History reachability and root publication remain with their semantic owners. Provider calls are made with
 * neither the PageStore owner mutex nor this queue's mutex held.
 */
class KisPageRetirementQueue final
{
public:
    static constexpr qsizetype WorkerBatchBudget = 32;
    using ReleaseOwnerLifetime = void (*)(void *context);

    // Existing cold composition boundary exported for integration tests; the
    // queue remains embedded by value in its PageStore owner.
    KRITAIMAGE_EXPORT KisPageRetirementQueue(KisPageOwnerLedger &owner,
                           KisPageMetadataCoordinator &metadata,
                           KisBackingBudgetController &budget,
                           QAtomicInt &ownerLifetimeReferences,
                           void *ownerLifetimeContext,
                           ReleaseOwnerLifetime releaseOwnerLifetime);
    KRITAIMAGE_EXPORT ~KisPageRetirementQueue();
    KRITAIMAGE_EXPORT void prepareTask();

    KisPageRetirementQueue(const KisPageRetirementQueue &) = delete;
    KisPageRetirementQueue &operator=(const KisPageRetirementQueue &) = delete;
    KisPageRetirementQueue(KisPageRetirementQueue &&) = delete;
    KisPageRetirementQueue &operator=(KisPageRetirementQueue &&) = delete;

    KRITAIMAGE_EXPORT void retireOrDefer(const KisReplicaHandle &replica,
                       const std::shared_ptr<KisPageReplicaProvider> &provider,
                       const KisCompletionTicket &lastUse,
                       KisPageBackingPreparation &&backing);
    void processAcceptedEffects(bool backgroundReclamation);
    // Detach committed Debt and retain the original record. Transfer is
    // infallible under the metadata owner's gate and invokes no provider.
    KRITAIMAGE_EXPORT void acceptEffect(const KisPageTransitionEffect &effect) noexcept;
    KRITAIMAGE_EXPORT KisPageStoreRetirementProgress process(qsizetype replicaBudget);

    KRITAIMAGE_EXPORT void stopAutomaticWakeups();
    KRITAIMAGE_EXPORT void beginClose();
    void cancelCloseAndSchedule();
    KRITAIMAGE_EXPORT void waitForIdle();
    KRITAIMAGE_EXPORT KisPageRetirementRecords takeForClose();
    KRITAIMAGE_EXPORT bool retireRecord(KisPageRetirementRecord &record);
    KRITAIMAGE_EXPORT bool isDrained() const;

    KRITAIMAGE_EXPORT KisPageRetirementQueueSnapshot snapshot() const;

private:
    KisPageRetirementRecordPointer takeEffectRecord(const KisPageTransitionEffect &effect) noexcept;
    bool admitOwnedRetirementDebt(KisPageRetirementRecord &record,
                                  KisBackingBudgetReservation *reservation,
                                  KisBackingBudgetClass currentClass);
    void defer(KisPageRetirementRecordPointer record);
    KisPageStoreRetirementProgress retireBatch(KisPageRetirementRecords &batch);
    void finishAttemptLocked(KisPageRetirementRecords &batch, KisPageRetirementRecords::iterator entry, bool retired);
    void schedulePassLocked();
    void scheduleRetryLocked();
    void cancelRetryLocked();
    void disarmAutomaticWakeupsLocked();
    void updatePeaksLocked();
    void prepareWait(KisPageRetirementRecord &record);
    std::shared_ptr<KisPageRetirementWait> prepareWaitState(
        const std::shared_ptr<KisPageRetirementWakeContext> &context);
    void enqueuePendingLocked(KisPageRetirementRecords &source, KisPageRetirementRecords::iterator entry);
    static void disarmLocked(KisPageRetirementRecord &record, bool takeGrant = false);
    static void dispatchReady(const std::weak_ptr<KisPageRetirementWakeContext> &context,
                               const std::weak_ptr<KisPageRetirementWait> &wait);

    KisPageOwnerLedger &m_owner;
    KisPageMetadataCoordinator &m_metadata;
    KisBackingBudgetController &m_budget;
    QAtomicInt &m_ownerLifetimeReferences;
    void *m_ownerLifetimeContext = nullptr;
    ReleaseOwnerLifetime m_releaseOwnerLifetime = nullptr;

    mutable QMutex m_mutex;
    KisPageRetirementRecords m_pending;
    // Accepted first attempts and notified retries share original records;
    // only records with background permission are eligible for worker passes.
    KisPageRetirementRecords m_ready;
    // Same sole record owner, partitioned to bound a no-signal retry wake.
    KisPageRetirementRecords m_retryPending;
    KisPageReclamationDelay m_retryTask;
    std::shared_ptr<KisPageRetirementWait> m_retryWait;
    bool m_retryScheduled = false;
    int m_nextRetryDelayMs = 1;
    quint64 m_retryWakeups = 0;
    quint64 m_maximumReplicasPerRetryWake = 0;
    std::shared_ptr<KisPageRetirementWakeContext> m_wakeContext;
    QAtomicInt m_pendingNotifications{0};
    QWaitCondition m_idle;
    bool m_jobScheduled = false;
    KisPageReclamationJobPointer m_task;
    bool m_closing = false;
    bool m_automaticWakeupsStopped = false;
    qsizetype m_activeReplicas = 0;
    quint64 m_pendingBytes = 0;
    quint64 m_backgroundPasses = 0;
    quint64 m_maximumReplicasPerPass = 0;
    qsizetype m_peakPendingReplicas = 0;
    quint64 m_peakPendingBytes = 0;
    quint64 m_retryRequeues = 0;
    quint64 m_closeDrainedReplicas = 0;
};

#endif // KIS_PAGE_RETIREMENT_QUEUE_P_H

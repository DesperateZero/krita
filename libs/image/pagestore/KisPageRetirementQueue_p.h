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

#include <QAtomicInt>
#include <QMutex>
#include <QSharedPointer>
#include <QVector>
#include <QWaitCondition>

#include <deque>
#include <memory>

class KisBackingBudgetReservation;

struct KisPageRetirementRecord
{
    KisReplicaHandle replica;
    QSharedPointer<KisPageReplicaProvider> provider;
    KisCompletionTicket lastUse;
    KisPageOperationId retirementOperation;
    // Rare provider-result rejection: keep the preallocation reservation
    // charged until an unregistered physical replica reaches terminal retire.
    std::shared_ptr<KisBackingBudgetReservation> orphanReservation;
};

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

    KisPageRetirementQueue(KisPageOwnerLedger &owner,
                           KisPageMetadataCoordinator &metadata,
                           QAtomicInt &ownerLifetimeReferences,
                           void *ownerLifetimeContext,
                           ReleaseOwnerLifetime releaseOwnerLifetime);
    ~KisPageRetirementQueue();

    KisPageRetirementQueue(const KisPageRetirementQueue &) = delete;
    KisPageRetirementQueue &operator=(const KisPageRetirementQueue &) = delete;
    KisPageRetirementQueue(KisPageRetirementQueue &&) = delete;
    KisPageRetirementQueue &operator=(KisPageRetirementQueue &&) = delete;

    void retireOrDefer(const KisReplicaHandle &replica,
                       const QSharedPointer<KisPageReplicaProvider> &provider,
                       const KisCompletionTicket &lastUse,
                       KisBackingBudgetReservation &&orphanReservation);
    void retireEffects(const QVector<KisPageTransitionEffect> &effects,
                       bool backgroundReclamation);
    KisPageStoreRetirementProgress process(qsizetype replicaBudget);

    void beginClose();
    void cancelCloseAndSchedule();
    void waitForIdle();
    QVector<KisPageRetirementRecord> takeForClose();
    bool retireRecord(KisPageRetirementRecord &record);
    bool isDrained() const;

    KisPageRetirementQueueSnapshot snapshot() const;

private:
    bool admitOwnedRetirementDebt(KisPageRetirementRecord &record,
                                  KisBackingBudgetReservation *reservation,
                                  KisBackingBudgetClass currentClass);
    void defer(KisPageRetirementRecord record);
    void finishAttemptLocked(KisPageRetirementRecord &record, bool retired);
    void schedulePassLocked();
    void updatePeaksLocked();

    KisPageOwnerLedger &m_owner;
    KisPageMetadataCoordinator &m_metadata;
    QAtomicInt &m_ownerLifetimeReferences;
    void *m_ownerLifetimeContext = nullptr;
    ReleaseOwnerLifetime m_releaseOwnerLifetime = nullptr;

    mutable QMutex m_mutex;
    std::deque<KisPageRetirementRecord> m_pending;
    std::deque<KisPageRetirementRecord> m_ready;
    QWaitCondition m_idle;
    bool m_jobScheduled = false;
    bool m_closing = false;
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

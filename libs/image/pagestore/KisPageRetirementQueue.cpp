/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageRetirementQueue_p.h"

#include "KisPageStoreReclamation_p.h"
#include "KisPageWriteCoordinator_p.h"

#include <QMutexLocker>

#include <algorithm>
#include <utility>

KisPageRetirementQueue::KisPageRetirementQueue(
    KisPageOwnerLedger &owner,
    KisPageMetadataCoordinator &metadata,
    QAtomicInt &ownerLifetimeReferences,
    void *ownerLifetimeContext,
    ReleaseOwnerLifetime releaseOwnerLifetime)
    : m_owner(owner)
    , m_metadata(metadata)
    , m_ownerLifetimeReferences(ownerLifetimeReferences)
    , m_ownerLifetimeContext(ownerLifetimeContext)
    , m_releaseOwnerLifetime(releaseOwnerLifetime)
{
    Q_ASSERT(m_ownerLifetimeContext);
    Q_ASSERT(m_releaseOwnerLifetime);
}

KisPageRetirementQueue::~KisPageRetirementQueue()
{
    Q_ASSERT(!m_jobScheduled);
    Q_ASSERT(m_activeReplicas == 0);
}

bool KisPageRetirementQueue::admitOwnedRetirementDebt(
    KisPageRetirementRecord &record,
    KisBackingBudgetReservation *reservation,
    KisBackingBudgetClass currentClass)
{
    if (currentClass == KisBackingBudgetClass::RetirementDebt) {
        if (reservation) reservation->release();
        return true;
    }
    if (reservation && reservation->isValid() &&
        m_owner.reclassifyBacking(
            record.replica, KisBackingBudgetClass::RetirementDebt, *reservation))
        return true;
    KisPageTransitionEffect effect;
    effect.replica = record.replica;
    effect.lastUse = record.lastUse;
    quint64 cookie = 0;
    if (!m_owner.prepareRetirementDebt({effect}, &cookie)) return false;
    m_owner.commitRetirementDebt(cookie);
    if (reservation) reservation->release();
    return true;
}

bool KisPageRetirementQueue::retireRecord(KisPageRetirementRecord &record)
{
    if (!record.provider) return false;
    const auto backingClass = m_owner.backingClass(record.replica);
    if (backingClass != KisBackingBudgetClass::Count &&
        !admitOwnedRetirementDebt(record, record.orphanReservation.get(), backingClass))
        return false;
    if (record.orphanReservation && !record.orphanReservation->isValid())
        record.orphanReservation.reset();
    if (!record.retirementOperation.isValid()) {
        const auto operation = m_owner.nextOperationId();
        const auto result = record.provider->retire(
            operation, record.replica, record.lastUse);
        if (!result.isValid() || !(result.operation == operation) ||
            !(result.replica == record.replica) ||
            !m_owner.bindRetirementOperation(operation, result)) {
            return false;
        }
        record.retirementOperation = operation;
    }
    // A late completion is polled, never replayed as a second retire.
    const auto terminal =
        m_owner.verifyProviderOperation(record.retirementOperation);
    if (!terminal.isValid()) return false;
    if (!m_owner.releaseTerminalProviderOperation(record.retirementOperation)) {
        return false;
    }
    record.retirementOperation = {};
    if (!terminal.succeeded()) return false;
    m_owner.releaseRetiredBacking(record.replica);
    return true;
}

void KisPageRetirementQueue::updatePeaksLocked()
{
    const qsizetype pending =
        qsizetype(m_pending.size() + m_ready.size()) + m_activeReplicas;
    m_peakPendingReplicas = std::max(m_peakPendingReplicas, pending);
    m_peakPendingBytes = std::max(m_peakPendingBytes, m_pendingBytes);
}

void KisPageRetirementQueue::defer(KisPageRetirementRecord record)
{
    // A failed synchronous retire may still own its original backing class;
    // only Debt-admitted records enter the background-ready queue. An orphan
    // sharing an already-accounted physical backing needs no own reservation.
    Q_ASSERT(record.replica.isValid());
    QMutexLocker lock(&m_mutex);
    m_pendingBytes += record.replica.layout.byteSize;
    m_pending.push_back(std::move(record));
    ++m_retryRequeues;
    updatePeaksLocked();
}

void KisPageRetirementQueue::finishAttemptLocked(KisPageRetirementRecord &record,
                                                  bool retired)
{
    --m_activeReplicas;
    if (retired) {
        m_pendingBytes -= record.replica.layout.byteSize;
    } else {
        m_pending.push_back(std::move(record));
        ++m_retryRequeues;
    }
}

// m_mutex held. Each queued pass holds the PageStore composition root alive;
// the service itself remains by-value and does not own another service.
void KisPageRetirementQueue::schedulePassLocked()
{
    if (m_jobScheduled || m_closing || m_ready.empty()) return;
    m_jobScheduled = true;
    m_ownerLifetimeReferences.ref();
    kisSchedulePageStoreReclamation([this] {
        std::deque<KisPageRetirementRecord> batch;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_closing && !m_ready.empty() &&
                   qsizetype(batch.size()) < WorkerBatchBudget) {
                batch.push_back(std::move(m_ready.front()));
                m_ready.pop_front();
            }
            m_activeReplicas += qsizetype(batch.size());
            ++m_backgroundPasses;
            m_maximumReplicasPerPass = std::max(
                m_maximumReplicasPerPass, quint64(batch.size()));
            updatePeaksLocked();
        }
        for (auto &record : batch) {
            const bool retired = retireRecord(record);
            QMutexLocker lock(&m_mutex);
            finishAttemptLocked(record, retired);
        }
        {
            QMutexLocker lock(&m_mutex);
            m_jobScheduled = false;
            schedulePassLocked();
            m_idle.wakeAll();
        }
        m_releaseOwnerLifetime(m_ownerLifetimeContext);
    });
}

void KisPageRetirementQueue::retireOrDefer(
    const KisReplicaHandle &replica,
    const QSharedPointer<KisPageReplicaProvider> &provider,
    const KisCompletionTicket &lastUse,
    KisBackingBudgetReservation &&orphanReservation)
{
    if (!replica.isValid()) return;
    KisPageRetirementRecord record{replica, provider, lastUse, {}, {}};
    const auto backingClass = m_owner.backingClass(replica);
    const bool owned = backingClass != KisBackingBudgetClass::Count;
    bool debtAdmitted = false;
    if (owned) {
        debtAdmitted = admitOwnedRetirementDebt(record, &orphanReservation,
                                                backingClass);
    } else if (orphanReservation.isValid()) {
        debtAdmitted = m_owner.retainRetirementDebtReservation(
            replica, orphanReservation);
        if (debtAdmitted) {
            record.orphanReservation = std::make_shared<KisBackingBudgetReservation>(
                std::move(orphanReservation));
        }
    }
    // Foreign/stale handles carry no store accounting and are attempted
    // synchronously; they are never admitted to the asynchronous queue.
    if (!debtAdmitted && !owned && retireRecord(record)) return;
    if (!debtAdmitted) {
        if (orphanReservation.isValid()) {
            record.orphanReservation = std::make_shared<KisBackingBudgetReservation>(
                std::move(orphanReservation));
        }
        if (owned || record.orphanReservation)
            defer(std::move(record)); // explicit process/close retries without background admission
        return;
    }
    if (provider && provider->capabilities().backgroundRetirement) {
        QMutexLocker lock(&m_mutex);
        m_pendingBytes += replica.layout.byteSize;
        m_ready.push_back(std::move(record));
        updatePeaksLocked();
        schedulePassLocked();
        return;
    }
    if (!retireRecord(record)) defer(std::move(record));
}

KisPageStoreRetirementProgress KisPageRetirementQueue::process(
    qsizetype replicaBudget)
{
    KisPageStoreRetirementProgress result;
    std::deque<KisPageRetirementRecord> batch;
    {
        QMutexLocker lock(&m_mutex);
        const auto count = std::min(std::max(qsizetype(0), replicaBudget),
                                    qsizetype(m_pending.size()));
        for (qsizetype i = 0; i < count; ++i) {
            batch.push_back(std::move(m_pending.front()));
            m_pending.pop_front();
        }
        m_activeReplicas += count;
        updatePeaksLocked();
    }
    // No queue/owner gate is held while the provider checks its local pin gate
    // and performs destruction. A blocked item rotates behind other debt.
    for (auto &record : batch) {
        ++result.attempted;
        const bool retired = retireRecord(record);
        QMutexLocker lock(&m_mutex);
        if (retired) ++result.retired;
        finishAttemptLocked(record, retired);
    }
    QMutexLocker lock(&m_mutex);
    result.pending =
        qsizetype(m_pending.size() + m_ready.size()) + m_activeReplicas;
    return result;
}

void KisPageRetirementQueue::retireEffects(
    const QVector<KisPageTransitionEffect> &effects,
    bool backgroundReclamation)
{
    if (!backgroundReclamation || kisOnPageStoreReclamationThread()) process(8);
    std::deque<KisPageRetirementRecord> ready;
    quint64 bytes = 0;
    for (const KisPageTransitionEffect &effect : effects) {
        m_metadata.removeCpuReadBinding(effect.replica);
        const auto provider = m_owner.provider(
            effect.replica.provider, effect.replica.providerEpoch);
        const auto backingClass = m_owner.backingClass(effect.replica);
        const bool debtAdmitted = backingClass == KisBackingBudgetClass::RetirementDebt;
        Q_ASSERT(debtAdmitted);
        if (provider && provider->capabilities().backgroundRetirement && debtAdmitted) {
            ready.push_back({effect.replica, provider, effect.lastUse, {}, {}});
            bytes += effect.replica.layout.byteSize;
        } else {
            retireOrDefer(effect.replica, provider, effect.lastUse, {});
        }
    }
    if (ready.empty()) return;
    QMutexLocker lock(&m_mutex);
    m_pendingBytes += bytes;
    while (!ready.empty()) {
        m_ready.push_back(std::move(ready.front()));
        ready.pop_front();
    }
    updatePeaksLocked();
    schedulePassLocked();
}

void KisPageRetirementQueue::beginClose()
{
    QMutexLocker lock(&m_mutex);
    m_closing = true;
}

void KisPageRetirementQueue::cancelCloseAndSchedule()
{
    QMutexLocker lock(&m_mutex);
    m_closing = false;
    schedulePassLocked();
}

void KisPageRetirementQueue::waitForIdle()
{
    QMutexLocker lock(&m_mutex);
    while (m_jobScheduled) m_idle.wait(&m_mutex);
}

QVector<KisPageRetirementRecord> KisPageRetirementQueue::takeForClose()
{
    QVector<KisPageRetirementRecord> result;
    QMutexLocker lock(&m_mutex);
    Q_ASSERT(!m_jobScheduled);
    result.reserve(qsizetype(m_ready.size() + m_pending.size()));
    while (!m_ready.empty()) {
        result.append(std::move(m_ready.front()));
        m_ready.pop_front();
    }
    while (!m_pending.empty()) {
        result.append(std::move(m_pending.front()));
        m_pending.pop_front();
    }
    m_closeDrainedReplicas += quint64(result.size());
    m_pendingBytes = 0;
    return result;
}

bool KisPageRetirementQueue::isDrained() const
{
    QMutexLocker lock(&m_mutex);
    return m_pending.empty() && m_ready.empty() &&
           m_activeReplicas == 0 && !m_jobScheduled && m_pendingBytes == 0;
}

KisPageRetirementQueueSnapshot KisPageRetirementQueue::snapshot() const
{
    QMutexLocker lock(&m_mutex);
    KisPageRetirementQueueSnapshot result;
    result.pendingReplicas =
        qsizetype(m_pending.size() + m_ready.size()) + m_activeReplicas;
    result.activeReplicas = m_activeReplicas;
    result.pendingBytes = m_pendingBytes;
    result.jobScheduled = m_jobScheduled;
    result.backgroundPasses = m_backgroundPasses;
    result.maximumReplicasPerPass = m_maximumReplicasPerPass;
    result.peakPendingReplicas = m_peakPendingReplicas;
    result.peakPendingBytes = m_peakPendingBytes;
    result.retryRequeues = m_retryRequeues;
    result.closeDrainedReplicas = m_closeDrainedReplicas;
    return result;
}

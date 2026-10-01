/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageRetirementQueue_p.h"

#include "KisPageStoreReclamation_p.h"
#include "KisPageWriteCoordinator_p.h"
#include "KisCpuResidentBinding_p.h"

#include <QMutexLocker>

#include <algorithm>
#include <utility>
#include <limits>

void KisPageRetirementRecordDeleter::operator()(KisPageRetirementRecord *record) const noexcept
{
    if (!record) return;
    auto storage = std::move(record->storage);
    std::destroy_at(record);
    storage.deallocate(record, 1);
}
KisPageRetirementRecordPointer kisPreparePageRetirementRecord(KisBackingBudgetController *budget)
{
    auto storage = KisMutationStorageAllocator<KisPageRetirementRecord>::retained(budget);
    auto *raw = storage.allocate(1);
    try { return KisPageRetirementRecordPointer(::new (raw) KisPageRetirementRecord(storage)); }
    catch (...) { storage.deallocate(raw, 1); throw; }
}

// These are notification identities only. All fields of a wait are protected
// by the queue gate, except the early notification atomic. The iterator is
// usable only while queued is true.
struct KisPageRetirementWait
{
    KisPageReadinessSubscription subscription;
    KisBackingBudgetWaiter budgetWaiter;
    KisBackingBudgetReservation budgetGrant;
    KisPageRetirementRecords::iterator entry;
    bool queued = false;
    QAtomicInt notified{0};
    bool retryPass = false;
    std::weak_ptr<KisPageRetirementWakeContext> context;
    KisPageRetirementQueue *notificationQueue = nullptr;
    KisPageReclamationJobPointer task;
    // Only an active dispatch owns this pin; dormant subscriptions retain no
    // root or self-cycle. Protected by the original wake-context mutex.
    std::shared_ptr<KisPageRetirementWait> inFlight;
};

struct KisPageRetirementWakeContext
{
    QMutex mutex;
    KisPageRetirementQueue *queue = nullptr;
    bool accepting = true;
};

KisPageRetirementQueue::KisPageRetirementQueue(
    KisPageOwnerLedger &owner,
    KisPageMetadataCoordinator &metadata,
    KisBackingBudgetController &budget,
    QAtomicInt &ownerLifetimeReferences,
    void *ownerLifetimeContext,
    ReleaseOwnerLifetime releaseOwnerLifetime)
    : m_owner(owner)
    , m_metadata(metadata)
    , m_budget(budget)
    , m_ownerLifetimeReferences(ownerLifetimeReferences)
    , m_ownerLifetimeContext(ownerLifetimeContext)
    , m_releaseOwnerLifetime(releaseOwnerLifetime)
{
    Q_ASSERT(m_ownerLifetimeContext);
    Q_ASSERT(m_releaseOwnerLifetime);
}

KisPageRetirementQueue::~KisPageRetirementQueue()
{
    if (m_wakeContext) {
        QMutexLocker lock(&m_wakeContext->mutex);
        m_wakeContext->queue = nullptr;
    }
    cancelRetryLocked();
    Q_ASSERT(!m_jobScheduled);
    Q_ASSERT(m_activeReplicas == 0);
    Q_ASSERT(m_pendingNotifications.loadAcquire() == 0);
}

void KisPageRetirementQueue::dispatchReady(
    const std::weak_ptr<KisPageRetirementWakeContext> &weakContext,
    const std::weak_ptr<KisPageRetirementWait> &weakWait)
{
    const auto wait = weakWait.lock();
    if (!wait) return;
    // Remember an event even if close currently rejects dispatch. A record
    // registering outside the queue gate can be inserted after close resumes.
    wait->notified.storeRelease(1);
    const auto context = weakContext.lock();
    if (!context) return;
    QMutexLocker lifetimeLock(&context->mutex);
    auto *queue = context->queue;
    if (!queue || !context->accepting || wait->inFlight) return;
    // A dormant waiter must not retain the composition root. Acquisition is
    // synchronized with queue destruction and cannot resurrect a zero count.
    auto &references = queue->m_ownerLifetimeReferences;
    int count = references.loadAcquire();
    for (;;) {
        if (count <= 0 || count == std::numeric_limits<int>::max()) return;
        if (references.testAndSetOrdered(count, count + 1)) break;
        count = references.loadAcquire();
    }
    queue->m_pendingNotifications.ref();
    // The producer may still hold a provider gate. Do not acquire queue/owner
    // gates or perform retirement here; the existing executor consumes it.
    wait->inFlight = wait;
    kisEnqueuePageStoreReclamation(wait->task.get());
}

std::shared_ptr<KisPageRetirementWait> KisPageRetirementQueue::prepareWaitState(
    const std::shared_ptr<KisPageRetirementWakeContext> &context)
{
    auto state = std::allocate_shared<KisPageRetirementWait>(
        KisMutationStorageAllocator<KisPageRetirementWait>::retained(&m_budget));
    state->context = context;
    state->notificationQueue = this;
    state->task = kisPreparePageStoreReclamation([queue = this, wait = state.get()] {
        {
            QMutexLocker lock(&queue->m_mutex);
            if (wait->retryPass) wait->notified.storeRelease(0);
            if (!queue->m_closing && !queue->m_automaticWakeupsStopped) {
                if (wait->retryPass && wait == queue->m_retryWait.get() &&
                    queue->m_retryScheduled && queue->m_retryTask.takeReady()) {
                    queue->m_retryScheduled = false;
                    qsizetype moved = 0;
                    while (!queue->m_retryPending.empty() && moved < WorkerBatchBudget) {
                        queue->m_ready.splice(queue->m_ready.end(), queue->m_retryPending,
                                              queue->m_retryPending.begin());
                        ++moved;
                    }
                    ++queue->m_retryWakeups;
                    queue->m_maximumReplicasPerRetryWake = std::max(
                        queue->m_maximumReplicasPerRetryWake, quint64(moved));
                    queue->schedulePassLocked();
                    queue->scheduleRetryLocked();
                } else if (wait->queued) {
                    const auto entry = wait->entry;
                    wait->queued = false;
                    queue->m_ready.splice(queue->m_ready.end(), queue->m_pending, entry);
                    queue->disarmLocked(*entry, true);
                    queue->schedulePassLocked();
                }
            }
        }
    }, &m_budget, +[](void *value) {
        auto *wait = static_cast<KisPageRetirementWait *>(value);
        auto *queue = wait->notificationQueue;
        const auto context = wait->context.lock();
        std::shared_ptr<KisPageRetirementWait> pin;
        {
            QMutexLocker lock(&context->mutex);
            pin = std::move(wait->inFlight);
            // A new arm can fire before this reusable task finishes. Retain
            // the original activity pin and coalesce that event into the next
            // invocation, which still validates the current timer generation.
            if (wait->retryPass && context->queue == queue && context->accepting &&
                wait->notified.fetchAndStoreOrdered(0)) {
                wait->inFlight = std::move(pin);
                kisEnqueuePageStoreReclamation(wait->task.get());
                return;
            }
        }
        // The last active wait/task can now physically free and return its
        // charge; the original root pin still protects queue/idle publication.
        pin.reset();
        {
            QMutexLocker lock(&queue->m_mutex);
            queue->m_pendingNotifications.deref();
            queue->m_idle.wakeAll();
        }
        queue->m_releaseOwnerLifetime(queue->m_ownerLifetimeContext);
    }, state.get());
    return state;
}


void KisPageRetirementQueue::prepareWait(KisPageRetirementRecord &record) try
{
    record.wakeEligible = record.provider &&
        record.provider->capabilities().backgroundRetirement;
    if (!record.wakeEligible) return;
    const auto backingClass = m_owner.backingClass(record.replica);
    const bool needsBudget = backingClass != KisBackingBudgetClass::RetirementDebt &&
                             !record.orphanDebtAdmitted;
    // Unowned orphans can only consume their original reservation. A waiter
    // never manufactures backing ownership or missing cancellation headroom.
    if (needsBudget && backingClass == KisBackingBudgetClass::Count) return;
    std::shared_ptr<KisPageRetirementWakeContext> context;
    {
        QMutexLocker lock(&m_mutex);
        if (m_closing || m_automaticWakeupsStopped) return;
        context = m_wakeContext;
    }
    Q_ASSERT(context); // Cold composition admitted the fallback before any backing.
    if (!context) return;
    auto wait = prepareWaitState(context);
    const KisPageReadinessCallback scheduleReady([context = std::weak_ptr<KisPageRetirementWakeContext>(context),
                               weakWait = std::weak_ptr<KisPageRetirementWait>(wait)] {
        dispatchReady(context, weakWait);
    }, &m_budget);
    auto status = KisPageReadinessStatus::Unavailable;
    if (needsBudget) {
        status = m_owner.watchRetirementBudget(record.replica, scheduleReady, &wait->budgetWaiter);
    } else if (record.retirementOperation.isValid()) {
        status = m_owner.watchProviderOperation(record.retirementOperation,
                                                scheduleReady, &wait->subscription);
    } else {
        if (record.lastUse.isValid())
            status = m_owner.watchCompletion(record.lastUse, scheduleReady, &wait->subscription);
        if (status != KisPageReadinessStatus::Waiting) {
            const auto binding = record.provider->cpuResidentBinding(record.replica);
            status = binding ? binding->watchRetirementReadiness(
                record.replica.allocationIdentity(), scheduleReady, &wait->subscription)
                : KisPageReadinessStatus::Unavailable;
        }
    }
    if (status == KisPageReadinessStatus::Waiting) {
        record.readinessRechecked = false;
    } else if (status == KisPageReadinessStatus::Ready && (needsBudget || !record.readinessRechecked)) {
        // Close the release-before-registration race once. Repeated provider
        // rejection with an idle binding is not evidence of a readiness event.
        record.readinessRechecked = true;
        wait->notified.storeRelease(1);
    } else {
        return;
    }
    record.wait = std::move(wait);
}
catch (const std::bad_alloc &) {
    // The sole retirement record is still owned by the caller/queue. Failed
    // notification preparation cannot drop its physical debt or fabricate a
    // ready grant. The cold-prepared retry resumes the same debt automatically.
    record.wait.reset();
}

void KisPageRetirementQueue::disarmLocked(KisPageRetirementRecord &record, bool takeGrant)
{
    if (!record.wait) return;
    if (takeGrant) {
        record.wait->budgetGrant = record.wait->budgetWaiter.take();
        if (record.wait->budgetGrant.isValid())
            record.admissionReservation = std::move(record.wait->budgetGrant);
    }
    record.wait->queued = false;
    record.wait.reset();
}

void KisPageRetirementQueue::enqueuePendingLocked(
    KisPageRetirementRecords &source, KisPageRetirementRecords::iterator entry)
{
    auto &record = *entry;
    if (m_closing || m_automaticWakeupsStopped) disarmLocked(record);
    if (record.wait && record.wait->notified.loadAcquire()) {
        disarmLocked(record, true);
        m_ready.splice(m_ready.end(), source, entry);
        schedulePassLocked();
    } else if (!record.wait && record.wakeEligible) {
        m_retryPending.splice(m_retryPending.end(), source, entry);
        scheduleRetryLocked();
    } else {
        m_pending.splice(m_pending.end(), source, entry);
        if (record.wait) {
            record.wait->entry = entry;
            record.wait->queued = true;
        }
    }
    ++m_retryRequeues;
}

void KisPageRetirementQueue::cancelRetryLocked()
{
    m_retryTask.cancel(); // invalidate even a generation already taken by the monitor
    m_retryScheduled = false;
    if (m_retryWait) m_retryWait->notified.storeRelease(0);
}

void KisPageRetirementQueue::scheduleRetryLocked()
{
    if (m_closing || m_automaticWakeupsStopped || m_retryPending.empty() || m_retryScheduled) return;
    Q_ASSERT(m_retryWait && m_retryTask.isValid());
    m_retryScheduled = m_retryTask.arm(m_nextRetryDelayMs);
    if (m_retryScheduled)
        m_nextRetryDelayMs = std::min(100, m_nextRetryDelayMs * 2);
}

bool KisPageRetirementQueue::admitOwnedRetirementDebt(
    KisPageRetirementRecord &record,
    KisBackingBudgetReservation *reservation,
    KisBackingBudgetClass currentClass)
{
    if (currentClass == KisBackingBudgetClass::RetirementDebt) {
        record.admissionReservation.release();
        if (reservation) reservation->release();
        return true;
    }
    if (record.admissionReservation.isValid()) {
        const bool admitted = m_owner.reclassifyBacking(
            record.replica, KisBackingBudgetClass::RetirementDebt, record.admissionReservation);
        record.admissionReservation.release();
        if (admitted) {
            if (reservation) reservation->release();
            return true;
        }
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
        !admitOwnedRetirementDebt(record, &record.orphanReservation, backingClass))
        return false;
    if (backingClass == KisBackingBudgetClass::Count && record.orphanReservation.isValid() && !record.orphanDebtAdmitted) {
        if (!m_owner.retainRetirementDebtReservation(record.replica, record.orphanReservation))
            return false;
        record.orphanDebtAdmitted = true;
    }

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
    // Release orphan charge while the composition root/budget still lives;
    // batch records can outlast the worker's final owner-lifetime release.
    record.orphanReservation.release();
    record.admissionReservation.release();
    return true;
}

void KisPageRetirementQueue::updatePeaksLocked()
{
    const qsizetype pending =
        qsizetype(m_pending.size() + m_retryPending.size() + m_ready.size()) + m_activeReplicas;
    m_peakPendingReplicas = std::max(m_peakPendingReplicas, pending);
    m_peakPendingBytes = std::max(m_peakPendingBytes, m_pendingBytes);
}

void KisPageRetirementQueue::defer(KisPageRetirementRecordPointer record)
{
    prepareWait(*record);
    Q_ASSERT(record->replica.isValid());
    KisPageRetirementRecords admitted;
    admitted.push_back(*record.release());
    QMutexLocker lock(&m_mutex);
    m_pendingBytes += admitted.front().replica.layout.byteSize;
    enqueuePendingLocked(admitted, admitted.begin());
    updatePeaksLocked();
}

void KisPageRetirementQueue::finishAttemptLocked(
    KisPageRetirementRecords &batch, KisPageRetirementRecords::iterator entry, bool retired)
{
    --m_activeReplicas;
    if (retired) {
        m_pendingBytes -= entry->replica.layout.byteSize;
        m_nextRetryDelayMs = 1;
    } else {
        enqueuePendingLocked(batch, entry);
    }
}

// m_mutex held. Each queued pass holds the PageStore composition root alive;
// the service itself remains by-value and does not own another service.
void KisPageRetirementQueue::prepareTask()
{
    if (m_task) return;
    auto task = kisPreparePageStoreReclamation([this] {
        KisPageRetirementRecords batch;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_closing && !m_ready.empty() &&
                   qsizetype(batch.size()) < WorkerBatchBudget) {
                disarmLocked(m_ready.front(), true);
                batch.splice(batch.end(), m_ready, m_ready.begin());
            }
            m_activeReplicas += qsizetype(batch.size());
            ++m_backgroundPasses;
            m_maximumReplicasPerPass = std::max(
                m_maximumReplicasPerPass, quint64(batch.size()));
            updatePeaksLocked();
        }
        for (auto it = batch.begin(); it != batch.end();) {
            const auto entry = it++;
            const bool retired = retireRecord(*entry);
            if (!retired) prepareWait(*entry);
            QMutexLocker lock(&m_mutex);
            finishAttemptLocked(batch, entry, retired);
        }
    }, &m_budget, +[](void *value) {
        auto *queue = static_cast<KisPageRetirementQueue *>(value);
        {
            QMutexLocker lock(&queue->m_mutex);
            queue->m_jobScheduled = false;
            queue->schedulePassLocked();
            queue->m_idle.wakeAll();
        }
        queue->m_releaseOwnerLifetime(queue->m_ownerLifetimeContext);
    }, this);
    auto context = std::allocate_shared<KisPageRetirementWakeContext>(
        KisMutationStorageAllocator<KisPageRetirementWakeContext>::retained(&m_budget));
    context->queue = this;
    auto retry = prepareWaitState(context);
    retry->retryPass = true;
    KisPageReclamationDelay timer(KisPageReadinessCallback(
        [weakContext = std::weak_ptr<KisPageRetirementWakeContext>(context),
         weakWait = std::weak_ptr<KisPageRetirementWait>(retry)] {
            dispatchReady(weakContext, weakWait);
        }, &m_budget), &m_budget);
    if (!timer.isValid()) throw std::bad_alloc();
    m_wakeContext = std::move(context);
    m_retryWait = std::move(retry);
    m_retryTask = std::move(timer);
    m_task = std::move(task);
}

void KisPageRetirementQueue::schedulePassLocked()
{
    if (m_jobScheduled || m_closing || m_ready.empty()) return;
    m_jobScheduled = true;
    m_ownerLifetimeReferences.ref();
    Q_ASSERT(m_task);
    kisEnqueuePageStoreReclamation(m_task.get());
}

void KisPageRetirementQueue::retireOrDefer(
    const KisReplicaHandle &replica,
    const QSharedPointer<KisPageReplicaProvider> &provider,
    const KisCompletionTicket &lastUse,
    KisPageBackingPreparation &&backing)
{
    if (!replica.isValid()) return;
    const auto backingClass = m_owner.backingClass(replica);
    const bool owned = backingClass != KisBackingBudgetClass::Count;
    auto record = owned ? m_owner.takeRetirementRecord(replica) : std::move(backing.retirement);
    if (!record) {
        // Foreign/stale handles are not asynchronously owned. Never fabricate
        // an unaccounted record after an external provider result.
        Q_ASSERT(!owned && !backing.reservation.isValid());
        if (!owned && !backing.reservation.isValid()) {
            KisPageRetirementRecord external(KisMutationStorageAllocator<KisPageRetirementRecord>{});
            external.replica = replica; external.provider = provider; external.lastUse = lastUse;
            retireRecord(external);
        }
        return;
    }
    record->replica = replica; record->provider = provider; record->lastUse = lastUse;
    record->orphanReservation = std::move(backing.reservation);
    bool debtAdmitted = false;
    if (owned) {
        debtAdmitted = admitOwnedRetirementDebt(*record, &record->orphanReservation, backingClass);
    } else if (record->orphanReservation.isValid()) {
        debtAdmitted = m_owner.retainRetirementDebtReservation(replica, record->orphanReservation);
        record->orphanDebtAdmitted = debtAdmitted;
    }
    if (!debtAdmitted && !owned && retireRecord(*record)) return;
    if (!debtAdmitted) {
        if (owned || record->orphanReservation.isValid()) defer(std::move(record));
        return;
    }
    if (provider && provider->capabilities().backgroundRetirement) {
        QMutexLocker lock(&m_mutex);
        m_pendingBytes += replica.layout.byteSize;
        m_ready.push_back(*record.release());
        updatePeaksLocked();
        schedulePassLocked();
        return;
    }
    if (!retireRecord(*record)) defer(std::move(record));
}

KisPageStoreRetirementProgress KisPageRetirementQueue::process(
    qsizetype replicaBudget)
{
    KisPageStoreRetirementProgress result;
    KisPageRetirementRecords batch;
    {
        QMutexLocker lock(&m_mutex);
        const auto count = std::min(std::max(qsizetype(0), replicaBudget),
                                    qsizetype(m_pending.size() + m_retryPending.size()));
        for (qsizetype i = 0; i < count; ++i) {
            auto &pending = m_pending.empty() ? m_retryPending : m_pending;
            disarmLocked(pending.front(), true);
            batch.splice(batch.end(), pending, pending.begin());
        }
        if (m_retryPending.empty()) cancelRetryLocked();
        m_activeReplicas += count;
        updatePeaksLocked();
    }
    // No queue/owner gate is held while the provider checks its local pin gate
    // and performs destruction. A blocked item rotates behind other debt.
    for (auto it = batch.begin(); it != batch.end();) {
        const auto entry = it++;
        ++result.attempted;
        const bool retired = retireRecord(*entry);
        if (!retired) prepareWait(*entry);
        QMutexLocker lock(&m_mutex);
        if (retired) ++result.retired;
        finishAttemptLocked(batch, entry, retired);
    }
    QMutexLocker lock(&m_mutex);
    result.pending =
        qsizetype(m_pending.size() + m_retryPending.size() + m_ready.size()) + m_activeReplicas;
    return result;
}

void KisPageRetirementQueue::retireEffects(
    const QVector<KisPageTransitionEffect> &effects,
    bool backgroundReclamation)
{
    if (!backgroundReclamation || kisOnPageStoreReclamationThread()) process(8);
    KisPageRetirementRecords ready;
    quint64 bytes = 0;
    for (const KisPageTransitionEffect &effect : effects) {
        m_metadata.removeCpuReadBinding(effect.replica);
        const auto provider = m_owner.provider(
            effect.replica.provider, effect.replica.providerEpoch);
        const auto backingClass = m_owner.backingClass(effect.replica);
        const bool debtAdmitted = backingClass == KisBackingBudgetClass::RetirementDebt;
        Q_ASSERT(debtAdmitted);
        if (provider && provider->capabilities().backgroundRetirement && debtAdmitted) {
            auto record = m_owner.takeRetirementRecord(effect.replica);
            Q_ASSERT(record); // Every registered backing admitted its node.
            if (!record) continue;
            record->provider = provider;
            record->lastUse = effect.lastUse;
            ready.push_back(*record.release());
            bytes += effect.replica.layout.byteSize;
        } else {
            retireOrDefer(effect.replica, provider, effect.lastUse, {});
        }
    }
    if (ready.empty()) return;
    QMutexLocker lock(&m_mutex);
    m_pendingBytes += bytes;
    m_ready.splice(m_ready.end(), ready);
    updatePeaksLocked();
    schedulePassLocked();
}

void KisPageRetirementQueue::disarmAutomaticWakeupsLocked()
{
    if (m_wakeContext) {
        QMutexLocker notificationGate(&m_wakeContext->mutex);
        m_wakeContext->accepting = false;
    }
    // Admissions before the notification gate closed are already counted;
    // later producers cannot post behind close's waitForIdle observation.
    cancelRetryLocked();
    for (auto &record : m_retryPending) disarmLocked(record);
    for (auto &record : m_pending) disarmLocked(record);
    for (auto &record : m_ready) disarmLocked(record);
}

void KisPageRetirementQueue::stopAutomaticWakeups()
{
    QMutexLocker lock(&m_mutex);
    m_automaticWakeupsStopped = true;
    disarmAutomaticWakeupsLocked();
    // Already-ready work may finish with its existing leases. Failed records
    // cannot arrange recurring continuations after the facade has gone away.
}

void KisPageRetirementQueue::beginClose()
{
    QMutexLocker lock(&m_mutex);
    m_closing = true;
    disarmAutomaticWakeupsLocked();
}

void KisPageRetirementQueue::cancelCloseAndSchedule()
{
    QMutexLocker lock(&m_mutex);
    m_closing = false;
    if (m_wakeContext) {
        QMutexLocker notificationGate(&m_wakeContext->mutex);
        m_wakeContext->accepting = !m_automaticWakeupsStopped;
    }
    m_ready.splice(m_ready.end(), m_retryPending);
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        const auto current = it++;
        if (current->wakeEligible) {
            disarmLocked(*current);
            m_ready.splice(m_ready.end(), m_pending, current);
        }
    }
    schedulePassLocked();
}

void KisPageRetirementQueue::waitForIdle()
{
    QMutexLocker lock(&m_mutex);
    while (m_jobScheduled || m_pendingNotifications.loadAcquire()) m_idle.wait(&m_mutex);
}

KisPageRetirementRecords KisPageRetirementQueue::takeForClose()
{
    KisPageRetirementRecords result;
    QMutexLocker lock(&m_mutex);
    Q_ASSERT(!m_jobScheduled);
    cancelRetryLocked();
    for (auto &record : m_ready) disarmLocked(record);
    for (auto &record : m_pending) disarmLocked(record);
    for (auto &record : m_retryPending) disarmLocked(record);
    result.splice(result.end(), m_ready);
    result.splice(result.end(), m_pending);
    result.splice(result.end(), m_retryPending);
    m_closeDrainedReplicas += quint64(result.size());
    m_pendingBytes = 0;
    return result;
}

bool KisPageRetirementQueue::isDrained() const
{
    QMutexLocker lock(&m_mutex);
    return m_pending.empty() && m_retryPending.empty() && m_ready.empty() && !m_retryScheduled &&
           m_activeReplicas == 0 && !m_jobScheduled && m_pendingBytes == 0 &&
           m_pendingNotifications.loadAcquire() == 0;
}

KisPageRetirementQueueSnapshot KisPageRetirementQueue::snapshot() const
{
    QMutexLocker lock(&m_mutex);
    KisPageRetirementQueueSnapshot result;
    result.pendingReplicas =
        qsizetype(m_pending.size() + m_retryPending.size() + m_ready.size()) + m_activeReplicas;
    result.activeReplicas = m_activeReplicas;
    result.pendingBytes = m_pendingBytes;
    result.jobScheduled = m_jobScheduled || m_pendingNotifications.loadAcquire() != 0;
    result.backgroundPasses = m_backgroundPasses;
    result.maximumReplicasPerPass = m_maximumReplicasPerPass;
    result.peakPendingReplicas = m_peakPendingReplicas;
    result.peakPendingBytes = m_peakPendingBytes;
    result.retryRequeues = m_retryRequeues;
    result.closeDrainedReplicas = m_closeDrainedReplicas;
    result.delayedReplicas = qsizetype(m_retryPending.size());
    result.retryScheduled = m_retryScheduled;
    result.retryWakeups = m_retryWakeups;
    result.maximumReplicasPerRetryWake = m_maximumReplicasPerRetryWake;
    result.nextRetryDelayMs = m_nextRetryDelayMs;
    return result;
}

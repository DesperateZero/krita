/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisPageHistoryCollector_p.h"

#include "KisPageStoreReclamation_p.h"

#include <QMutexLocker>
#include <QDeadlineTimer>

#include <algorithm>
#include <limits>
#include <utility>

struct KisPageHistoryWakeContext
{
    QMutex mutex;
    KisPageHistoryCollector *collector = nullptr;
    KisPageReclamationDelay retry;
    qint64 timerDeadline = 0;
    quint64 retryWakeups = 0;
    bool accepting = false;
    bool jobScheduled = false;
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
    HasAbandonedMutations hasAbandonedMutations,
    CollectAbandonedMutations collectAbandonedMutations,
    NextIdleDeadline nextIdleDeadline,
    CollectIdleCapacity collectIdleCapacity)
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
    , m_hasAbandonedMutations(hasAbandonedMutations)
    , m_collectAbandonedMutations(collectAbandonedMutations)
    , m_nextIdleDeadline(nextIdleDeadline)
    , m_collectIdleCapacity(collectIdleCapacity)
{
    Q_ASSERT(m_ownerContext);
    Q_ASSERT(m_releaseOwnerLifetime);
    Q_ASSERT(bool(m_hasAbandonedMutations) == bool(m_collectAbandonedMutations));
    Q_ASSERT(bool(m_nextIdleDeadline) == bool(m_collectIdleCapacity));
}

KisPageHistoryCollector::~KisPageHistoryCollector()
{
    // Fence producers before disconnecting the original notification owner.
    m_metadata.setValueReclaimWake(nullptr, nullptr);
    if (m_wakeContext) {
        QMutexLocker lock(&m_wakeContext->mutex);
        Q_ASSERT(!m_wakeContext->jobScheduled);
        m_wakeContext->collector = nullptr;
    }
    clearLocked();
}

bool KisPageHistoryCollector::collectPassLocked()
{
    const auto work = m_metadata.collectHistoryValues(&m_epochs,
        +[](void *owner, const KisPageVersion &version) {
            KisPageVersion current;
            return static_cast<KisImageEpochReferenceModel *>(owner)->resolve(version.key, {}, &current)
                && current == version;
        });
    ++m_collectionPasses;
    m_valuesVisited += work.examined;
    m_retiredValues += work.retired;
    if (!kisOnPageStoreReclamationThread()) m_foregroundValuesVisited += work.examined;
    m_maximumValuesPerPass = std::max(m_maximumValuesPerPass, quint64(work.examined));
    return work.blocked;
}

void KisPageHistoryCollector::collectUnreachableLocked()
{
    if (!m_backgroundReclamation || m_closing || m_automaticWakeupsStopped) {
        while (m_metadata.pendingValueChecks()) {
            if (collectPassLocked()) {
                m_blocked = true;
                break;
            }
        }
    }
    scheduleLocked();
}

bool KisPageHistoryCollector::hasSemanticWorkLocked() const
{
    return m_epochs.hasCollectionWork() || m_metadata.pendingValueChecks()
        || (m_hasAbandonedMutations && m_hasAbandonedMutations(m_ownerContext));
}

bool KisPageHistoryCollector::enqueueTask(KisPageHistoryWakeContext &context)
{
    if (!context.accepting) return false;
    if (context.jobScheduled) return true;
    int count = m_ownerLifetimeReferences.loadAcquire();
    for (;;) {
        if (count <= 0 || count == std::numeric_limits<int>::max()) return false;
        if (m_ownerLifetimeReferences.testAndSetOrdered(count, count + 1)) break;
        count = m_ownerLifetimeReferences.loadAcquire();
    }
    context.jobScheduled = true;
    ++context.activities;
    if (kisEnqueuePageStoreReclamation(m_task.get())) return true;
    context.jobScheduled = false;
    --context.activities;
    context.accepting = false;
    context.retry.cancel();
    context.timerDeadline = 0;
    // Runtime rejection is terminal (its worker is stopping). Facade exit
    // closes this same notification gate before dropping its reference, so
    // rollback cannot be the last owner or run destruction on the monitor.
    const bool alive = m_ownerLifetimeReferences.deref();
    Q_ASSERT(alive); Q_UNUSED(alive);
    context.idle.wakeAll();
    return false;
}

void KisPageHistoryCollector::prepareTask(KisBackingBudgetController &budget)
{
    if (m_task) return;
    auto context = std::allocate_shared<KisPageHistoryWakeContext>(
        KisMutationStorageAllocator<KisPageHistoryWakeContext>::retained(&budget));
    context->collector = this;
    auto task = kisPreparePageStoreReclamation([this] {
        bool collectAbandoned = false;
        bool processRetirements = false;
        {
            QMutexLocker lock(&m_ownerMutex);
            if (!m_closing && m_operational && m_backgroundReclamation && !m_automaticWakeupsStopped) {
                const qint64 now = QDeadlineTimer::current().deadline();
                const qint64 idleDeadline = m_nextIdleDeadline ? m_nextIdleDeadline(m_ownerContext) : 0;
                if (idleDeadline && idleDeadline <= now)
                    m_collectIdleCapacity(m_ownerContext, now, IdleCapacityBudget);
                // An Idle-only wake must neither retry blocked history nor
                // reset its backoff. Both kinds use the same original task.
                if (hasSemanticWorkLocked() && (!m_blocked || (m_pressureDeadline && now >= m_pressureDeadline))) {
                    m_blocked = false;
                    m_pressureDeadline = 0;
                    m_epochs.collectFinishedTransactions(EpochCollectionBudget);
                    m_epochs.collectUnretainedRoots(EpochCollectionBudget);
                    m_blocked = collectPassLocked() || m_blocked;
                    collectAbandoned = m_hasAbandonedMutations && m_hasAbandonedMutations(m_ownerContext);
                    processRetirements = true;
                }
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
            {
                QMutexLocker notificationLock(&context->mutex);
                context->jobScheduled = false;
            }
            if (!collector->m_blocked) collector->m_retryDelayMs = 1;
            // A timer consumed while this pass was running needs no second
            // ready flag: recompute real deadlines/work before dropping it.
            collector->scheduleLocked();
        }
        collector->m_releaseOwnerLifetime(collector->m_ownerContext);
        QMutexLocker notificationLock(&context->mutex);
        --context->activities;
        context->idle.wakeAll();
    }, this);
    context->retry = KisPageReclamationDelay(KisPageReadinessCallback(
        [weak = std::weak_ptr<KisPageHistoryWakeContext>(context)] {
            const auto state = weak.lock();
            if (!state) return;
            QMutexLocker lock(&state->mutex);
            if (!state->collector || !state->accepting || !state->retry.takeReady()) return;
            ++state->retryWakeups;
            state->timerDeadline = 0;
            // No Store gate or business work on the process-wide monitor.
            // A queued/running task already owns the next readiness check.
            state->collector->enqueueTask(*state);
        }, &budget), &budget);
    if (!context->retry.isValid()) throw std::bad_alloc();
    m_task = std::move(task);
    m_wakeContext = std::move(context);

}

void KisPageHistoryCollector::startLocked()
{
    Q_ASSERT(m_task && m_wakeContext);
    m_metadata.setValueReclaimWake(this, +[](void *value) noexcept {
        auto *collector = static_cast<KisPageHistoryCollector *>(value);
        auto &state = *collector->m_wakeContext;
        QMutexLocker notificationLock(&state.mutex);
        if (state.collector == collector) collector->enqueueTask(state);
    });
    scheduleLocked();
}

void KisPageHistoryCollector::scheduleLocked()
{
    if (!m_wakeContext) return;
    auto &context = *m_wakeContext;
    if (m_closing || m_automaticWakeupsStopped || !m_backgroundReclamation || !m_operational) {
        m_pressureDeadline = 0;
        QMutexLocker notificationLock(&context.mutex);
        context.accepting = false;
        context.retry.cancel();
        context.timerDeadline = 0;
        context.idle.wakeAll();
        return;
    }
    const qint64 now = QDeadlineTimer::current().deadline();
    qint64 deadline = m_nextIdleDeadline ? m_nextIdleDeadline(m_ownerContext) : 0;
    const bool idleDue = deadline && deadline <= now;
    const bool work = hasSemanticWorkLocked();
    if (!work || (m_pressureDeadline && now >= m_pressureDeadline)) {
        m_blocked = false;
        m_pressureDeadline = 0;
        if (!work) m_retryDelayMs = 1;
    }
    if (work && m_blocked && !m_pressureDeadline) {
        m_pressureDeadline = now + m_retryDelayMs;
        m_retryDelayMs = std::min(100, m_retryDelayMs * 2);
    }
    QMutexLocker notificationLock(&context.mutex);
    context.accepting = true;
    if ((idleDue || (work && !m_blocked)) && !enqueueTask(context)) return;
    // A task owns the due prefix and will continue it in bounded passes.
    // Do not repeatedly arm a past deadline while it is queued/running.
    if (idleDue && context.jobScheduled) deadline = 0;
    if (work && m_blocked && (!deadline || m_pressureDeadline < deadline))
        deadline = m_pressureDeadline;
    if (deadline != context.timerDeadline) {
        if (deadline) {
            context.timerDeadline = context.retry.arm(int(std::max<qint64>(1, deadline - now))) ? deadline : 0;
        } else {
            context.retry.cancel();
            context.timerDeadline = 0;
        }
    }
}

void KisPageHistoryCollector::collectAfterEpochChangeLocked()
{
    if (!m_backgroundReclamation) {
        m_epochs.collectFinishedTransactions();
        m_epochs.collectUnretainedRoots();
    }
    // This also pumps foreground abort, which has no later publish callback.
    collectUnreachableLocked();
}

void KisPageHistoryCollector::waitForIdleLocked()
{
    // Observe in-flight work through the one notification admission. Future
    // pressure retries may retain work; idle is deliberately not a drain.
    const auto context = m_wakeContext;
    if (!context) return;
    for (;;) {
        QMutexLocker notificationLock(&context->mutex);
        if (!context->activities) return;
        m_ownerMutex.unlock();
        while (context->activities) context->idle.wait(&context->mutex);
        notificationLock.unlock();
        m_ownerMutex.lock();
    }
}

void KisPageHistoryCollector::stopAutomaticWakeups()
{
    QMutexLocker lock(&m_ownerMutex);
    m_automaticWakeupsStopped = true;
    m_pressureDeadline = 0;
    if (m_wakeContext) {
        QMutexLocker notificationLock(&m_wakeContext->mutex);
        m_wakeContext->collector = nullptr;
        m_wakeContext->accepting = false;
        m_wakeContext->retry.cancel();
        m_wakeContext->timerDeadline = 0;
        m_wakeContext->idle.wakeAll();
    }
}

void KisPageHistoryCollector::clearLocked()
{
    if (m_wakeContext) {
        QMutexLocker notificationLock(&m_wakeContext->mutex);
        Q_ASSERT(!m_wakeContext->jobScheduled);
        m_wakeContext->accepting = false;
        m_wakeContext->retry.cancel();
        m_wakeContext->timerDeadline = 0;
    }
    m_pressureDeadline = 0;
    m_metadata.clearValueChecks();
}

KisPageHistoryCollectorSnapshot KisPageHistoryCollector::snapshotLocked() const
{
    KisPageHistoryCollectorSnapshot result;
    result.pendingValues = qsizetype(m_metadata.pendingValueChecks());
    if (m_wakeContext) {
        QMutexLocker notificationLock(&m_wakeContext->mutex);
        result.retryWakeups = m_wakeContext->retryWakeups;
        result.retryScheduled = m_wakeContext->timerDeadline != 0;
        result.jobScheduled = m_wakeContext->jobScheduled;
    }
    result.valuesVisited = m_valuesVisited;
    result.foregroundValuesVisited = m_foregroundValuesVisited;
    result.maximumValuesPerPass = m_maximumValuesPerPass;
    result.collectionPasses = m_collectionPasses;
    result.retiredValues = m_retiredValues;
    return result;
}

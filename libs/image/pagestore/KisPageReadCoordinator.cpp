/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageReadCoordinator_p.h"
#include "KisCpuResidentBinding_p.h"
#include "KisPageStoreReclamation_p.h"

#include <QMutexLocker>
#include <QScopeGuard>
#include <QWaitCondition>

#include <algorithm>
#include <optional>
#include <limits>
#include <new>
#include <utility>

KisPageReadCleanup::KisPageReadCleanup(KisPageReadCoordinator &owner)
    : m_owner(owner)
    , m_requests(std::less<quint64>{}, owner.m_requests.get_allocator())
    , m_reads(std::less<quint64>{}, owner.m_activeReads.get_allocator())
{
}

KisPageReadCleanup::~KisPageReadCleanup()
{
    finish();
}

bool KisPageReadCleanup::prepareRequestLocked(QMutexLocker<QMutex> &ownerLock, QString *error)
{
    Q_ASSERT(!m_preparedRequestId);
    const auto id = m_owner.m_owner.nextRequestId();
    if (!id.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore read identity allocation failed"));
        return false;
    }
    KisPageReadCoordinator::PendingReadMap::node_type node;
    ++m_owner.m_activeProviderCalls;
    ownerLock.unlock();
    try {
        KisPageReadCoordinator::PendingReadMap prepared(std::less<quint64>{}, m_requests.get_allocator());
        prepared.emplace(id.value, KisPagePendingReadRecord{});
        node = prepared.extract(prepared.begin());
    } catch (const std::bad_alloc &) {
        ownerLock.relock(); --m_owner.m_activeProviderCalls;
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore read request storage is unavailable"));
        return false;
    }
    ownerLock.relock(); --m_owner.m_activeProviderCalls;
    m_requests.insert(std::move(node));
    m_preparedRequestId = id.value;
    retainLocked(); // Also covers refused cold candidates until actual gate-free disposal.
    if (!m_owner.m_operational) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore changed during read preparation"));
        return false;
    }
    return true;
}

void KisPageReadCleanup::retainLocked()
{
    if (!m_active && (!m_storage.isEmpty() || !m_requests.empty() || !m_reads.empty())) {
        ++m_owner.m_activeProviderCalls;
        m_active = true;
    }
}

void KisPageReadCleanup::finish()
{
    if (!m_active) return;
    // Keep both the metadata charge and the owner's unlocked activity until
    // the detached payload has actually been destroyed.
    m_storage.clear();
    m_requests.clear();
    m_reads.clear();
    QMutexLocker lock(&m_owner.m_ownerMutex);
    --m_owner.m_activeProviderCalls;
    m_active = false;
}

void KisPageReadCleanup::finishUnlocked(QMutexLocker<QMutex> &ownerLock)
{
    if (!m_active) return;
    ownerLock.unlock();
    finish();
    ownerLock.relock();
}

struct KisPageReadCoordinator::LastUseWakeContext
{
    QMutex mutex;
    QWaitCondition idle;
    KisPageReadCoordinator *coordinator = nullptr;
    KisPageReclamationWake wake;
    KisPageReclamationJobPointer task;
    std::shared_ptr<ActiveReadRecord> head;
    ActiveReadRecord *tail = nullptr;
    qsizetype readyCount = 0;
    bool accepting = true;
    bool jobScheduled = false;
    KisPageReclamationDelay retry;
    bool retryScheduled = false;
    bool cleanupPending = false;
    int nextRetryDelayMs = 1;
    quint64 dispatchFailures = 0;

    void clearReadyLocked()
    {
        while (head) {
            auto record = std::move(head);
            head = std::move(record->nextReady);
            record->readyQueued = false;
        }
        tail = nullptr;
        readyCount = 0;
    }
};

KisPageReadCoordinator::~KisPageReadCoordinator()
{
    Q_ASSERT(m_capturedReleaseRetries.empty());
    if (m_lastUseWakeContext) {
        QMutexLocker lock(&m_lastUseWakeContext->mutex);
        Q_ASSERT(!m_lastUseWakeContext->jobScheduled);
        m_lastUseWakeContext->coordinator = nullptr;
        m_lastUseWakeContext->accepting = false;
        m_lastUseWakeContext->clearReadyLocked();
        m_lastUseWakeContext->retry.cancel();
        m_lastUseWakeContext->wake.reset();
    }
}

void KisPageReadCoordinator::prepareTask()
{
    if (m_lastUseWakeContext) return;
    auto prepared = std::allocate_shared<LastUseWakeContext>(
        KisMutationStorageAllocator<LastUseWakeContext>::retained(&m_budget));
    prepared->coordinator = this;
    prepared->task = kisPreparePageStoreReclamation([this] {
        processLastUses(m_lastUseWakeContext);
    }, &m_budget, +[](void *value) {
        auto *coordinator = static_cast<KisPageReadCoordinator *>(value);
        const auto context = coordinator->m_lastUseWakeContext;
        {
            QMutexLocker lock(&context->mutex);
            context->jobScheduled = false;
            if (context->accepting && !context->retryScheduled &&
                (context->head || context->cleanupPending))
                context->wake.notify();
            context->idle.wakeAll();
        }
        coordinator->m_releaseLifetime(coordinator->m_lifetimeContext);
    }, this);
    prepared->wake = kisPreparePageStoreReclamationWake(
        KisPageReadinessCallback([weak = std::weak_ptr<LastUseWakeContext>(prepared)] {
            dispatchLastUses(weak);
        }, &m_budget), &m_budget);
    prepared->retry = KisPageReclamationDelay(KisPageReadinessCallback(
        [weak = std::weak_ptr<LastUseWakeContext>(prepared)] { dispatchLastUses(weak); }, &m_budget), &m_budget);
    if (!prepared->wake.isValid() || !prepared->retry.isValid()) throw std::bad_alloc();
    m_lastUseWakeContext = std::move(prepared);
}

void KisPageReadCoordinator::armLastUseLocked(const std::shared_ptr<ActiveReadRecord> &source)
{
    const auto record = source;
    if (!m_backgroundReclamation || m_automaticWakeupsStopped || m_lastUseClosing ||
        !record->metadataReleased || record->lastUseSubscription.isValid()) return;
    try {
        Q_ASSERT(m_lastUseWakeContext);
        const auto active = m_activeReads.find(record->leaseId);
        if (active == m_activeReads.end() || active->second != record) return;
        const std::weak_ptr<LastUseWakeContext> context = m_lastUseWakeContext;
        const std::weak_ptr<ActiveReadRecord> pending = record;
        KisPageReadinessSubscription subscription;
        const auto discard = qScopeGuard([&] {
            m_ownerMutex.unlock();
            subscription.reset();
            m_ownerMutex.lock();
        });
        KisPageReadinessStatus status;
        {
            m_ownerMutex.unlock();
            const auto relock = qScopeGuard([&] { m_ownerMutex.lock(); });
            status = m_completions->watchTerminal(record->releaseLastUse,
                KisPageReadinessCallback([context, pending] { notifyLastUse(context, pending); }, &m_budget),
                &subscription);
        }
        const auto winner = m_activeReads.find(record->leaseId);
        if (winner == m_activeReads.end() || winner->second != record ||
            m_automaticWakeupsStopped || m_lastUseClosing || !record->metadataReleased ||
            record->lastUseSubscription.isValid()) return;
        record->lastUseSubscription = std::move(subscription);
        if (status != KisPageReadinessStatus::Waiting || !record->lastUseSubscription.isValid())
            notifyLastUse(context, pending);
    } catch (const std::bad_alloc &) {
        // The original cold task/timer owns progress when a subscription does
        // not fit. Retain the exact record and poll its original terminal.
        notifyLastUse(m_lastUseWakeContext, record);
    }
}

void KisPageReadCoordinator::notifyLastUse(
    const std::weak_ptr<LastUseWakeContext> &weakContext,
    const std::weak_ptr<ActiveReadRecord> &weakRecord)
{
    const auto context = weakContext.lock();
    const auto record = weakRecord.lock();
    if (!context || !record) return;
    QMutexLocker lock(&context->mutex);
    if (!context->coordinator || !context->accepting) return;
    if (!record->readyQueued) {
        record->readyQueued = true;
        if (context->tail) context->tail->nextReady = record;
        else context->head = record;
        context->tail = record.get();
        ++context->readyCount;
    }
    context->retry.cancel();
    context->retryScheduled = false;
    // The producer may still hold provider/registry gates. Only splice an
    // already prepared monitor node here; never enter the owner or metadata.
    context->wake.notify();
}

void KisPageReadCoordinator::dispatchLastUses(const std::weak_ptr<LastUseWakeContext> &weakContext)
{
    const auto context = weakContext.lock();
    if (!context) return;
    QMutexLocker lock(&context->mutex);
    auto *coordinator = context->coordinator;
    if (!coordinator || !context->accepting || (!context->head && !context->cleanupPending)) return;
    if (context->retryScheduled) {
        if (!context->retry.takeReady()) return;
        context->retryScheduled = false;
    }
    // A timer can publish while the task is still finishing. Consume that
    // notification now; the original finish point will enqueue the next pass.
    if (context->jobScheduled) return;
    auto &references = coordinator->m_ownerLifetime;
    int count = references.loadAcquire();
    for (;;) {
        if (count <= 0 || count == std::numeric_limits<int>::max()) return;
        if (references.testAndSetOrdered(count, count + 1)) break;
        count = references.loadAcquire();
    }
    context->jobScheduled = true;
    lock.unlock();
    kisEnqueuePageStoreReclamation(context->task.get());
}

void KisPageReadCoordinator::processLastUses(const std::shared_ptr<LastUseWakeContext> &context)
{
    KisPageReadCleanup cleanup(*this);
    bool retry = false;
    bool progressed = false;
    {
        QMutexLocker ownerLock(&m_ownerMutex);
        constexpr qsizetype maximumPerPass = 32;
        qsizetype visited = 0;
        qsizetype readyBudget = 0;
        {
            QMutexLocker notificationLock(&context->mutex);
            readyBudget = context->readyCount;
        }
        if (!m_cancelRetries.empty() && m_operational) {
            retryCancelledRequestsLocked(ownerLock, cleanup);
            retry = !m_cancelRetries.empty();
            ++visited;
        }
        if (!m_capturedReleaseRetries.empty()) {
            retryCapturedReleasesLocked(ownerLock);
            retry = retry || !m_capturedReleaseRetries.empty();
            ++visited;
        }
        const qsizetype passEnd = std::min(maximumPerPass, visited + readyBudget);
        for (; visited < passEnd; ++visited) {
            std::shared_ptr<ActiveReadRecord> record;
            {
                QMutexLocker lock(&context->mutex);
                if (!context->accepting || !context->head) break;
                record = std::move(context->head);
                context->head = std::move(record->nextReady);
                if (!context->head) context->tail = nullptr;
                record->readyQueued = false;
                --context->readyCount;
            }
            const auto active = m_activeReads.find(record->leaseId);
            if (!m_operational ||
                active == m_activeReads.end() || active->second != record) continue;
            if (record->releaseHook.is_linked()) {
                m_releaseRetries.erase(m_releaseRetries.iterator_to(*record));
                if (!finishReleasedReadLocked(record->leaseId, cleanup)) {
                    m_releaseRetries.push_back(*record);
                    notifyLastUse(context, record);
                    retry = true;
                    continue;
                }
            }
            if (record->metadataReleased) {
                const auto terminal = m_completions->verifyTerminal(record->releaseLastUse);
                if (!terminal.isValid() || !acknowledgeRecordLocked(record, terminal, cleanup)) {
                    notifyLastUse(context, record);
                    retry = true;
                    continue;
                }
            }
            if (record->historyPending) {
                if (finishHistoryReadLocked(record, ownerLock, cleanup)) progressed = true;
                else { notifyLastUse(context, record); retry = true; }
            }
        }
        if (visited) {
            ++m_lastUsePasses;
            m_maximumLastUsesPerPass = std::max(m_maximumLastUsesPerPass, quint64(visited));
        }
        QMutexLocker notificationLock(&context->mutex);
        context->cleanupPending = !m_cancelRetries.empty() || !m_capturedReleaseRetries.empty();
    }
    cleanup.finish(); // complete physical cleanup before idle/lifetime publication
    {
        QMutexLocker lock(&context->mutex);
        if (context->accepting && retry && (context->head || context->cleanupPending)) {
            if (progressed) context->nextRetryDelayMs = 1;
            context->retryScheduled = context->retry.arm(context->nextRetryDelayMs);
            context->nextRetryDelayMs = std::min(100, context->nextRetryDelayMs * 2);
        } else if (progressed) {
            context->nextRetryDelayMs = 1;
        }
    }
}

void KisPageReadCoordinator::beginCloseLocked()
{
    m_lastUseClosing = true;
    if (m_lastUseWakeContext) {
        QMutexLocker lock(&m_lastUseWakeContext->mutex);
        m_lastUseWakeContext->accepting = false;
        m_lastUseWakeContext->retry.cancel();
        m_lastUseWakeContext->retryScheduled = false;
        m_lastUseWakeContext->clearReadyLocked();
        m_lastUseWakeContext->idle.wakeAll();
    }
    for (const auto &[id, record] : std::as_const(m_activeReads))
        record->lastUseSubscription.reset();
}

void KisPageReadCoordinator::cancelCloseLocked()
{
    m_lastUseClosing = false;
    if (!m_backgroundReclamation || m_automaticWakeupsStopped) return;
    Q_ASSERT(m_lastUseWakeContext);
    if (m_lastUseWakeContext) {
        QMutexLocker lock(&m_lastUseWakeContext->mutex);
        m_lastUseWakeContext->accepting = true;
        m_lastUseWakeContext->cleanupPending = !m_cancelRetries.empty() || !m_capturedReleaseRetries.empty();
        if (m_lastUseWakeContext->cleanupPending) m_lastUseWakeContext->wake.notify();
    }
    const quint64 last = m_activeReads.empty() ? 0 : m_activeReads.rbegin()->first;
    quint64 after = 0;
    for (;;) {
        const auto it = m_activeReads.upper_bound(after);
        if (it == m_activeReads.end() || it->first > last) break;
        after = it->first;
        const auto record = it->second;
        if (record->metadataReleased) armLastUseLocked(record);
        else if (record->historyPending || record->releaseHook.is_linked())
            notifyLastUse(m_lastUseWakeContext, record);
    }
}

void KisPageReadCoordinator::stopAutomaticWakeups()
{
    QMutexLocker lock(&m_ownerMutex);
    m_automaticWakeupsStopped = true;
    beginCloseLocked();
}

void KisPageReadCoordinator::waitForIdle()
{
    std::shared_ptr<LastUseWakeContext> context;
    {
        QMutexLocker lock(&m_ownerMutex);
        context = m_lastUseWakeContext;
    }
    if (!context) return;
    QMutexLocker lock(&context->mutex);
    while (context->jobScheduled ||
           (context->accepting && !context->retryScheduled && (context->head || context->cleanupPending)))
        context->idle.wait(&context->mutex);
}

KisPageReadCoordinator::ActiveReadRecord::ActiveReadRecord(
    const QSharedPointer<KisPageReplicaProvider> &providerValue,
    const KisReplicaHandle &replicaValue)
    : provider(providerValue)
    , replica(replicaValue)
{
}

KisPageReadCoordinator::ActiveReadRecord::~ActiveReadRecord()
{
    if (provider && access && access->isValid()) {
        provider->releaseAccess(std::move(*access), {});
    }
}

KisPageReadCoordinator::KisPageReadCoordinator(
    KisPageMetadataCoordinator &metadata,
    KisImageEpochReferenceModel &epochs,
    KisPageOwnerLedger &owner,
    KisBackingBudgetController &budget,
    KisPageHistoryCollector &history,
    KisPageRetirementQueue &retirementQueue,
    QSharedPointer<KisCompletionRegistry> &completions,
    QMutex &ownerMutex,
    qsizetype &activeProviderCalls,
    bool &operational,
    bool &backgroundReclamation,
    QAtomicInt &ownerLifetime,
    void *lifetimeContext,
    void (*releaseLifetime)(void *))
    : m_metadata(metadata)
    , m_epochs(epochs)
    , m_owner(owner)
    , m_budget(budget)
    , m_history(history)
    , m_retirementQueue(retirementQueue)
    , m_completions(completions)
    , m_ownerMutex(ownerMutex)
    , m_activeProviderCalls(activeProviderCalls)
    , m_operational(operational)
    , m_backgroundReclamation(backgroundReclamation)
    , m_ownerLifetime(ownerLifetime)
    , m_lifetimeContext(lifetimeContext)
    , m_releaseLifetime(releaseLifetime)
    , m_requests(std::less<quint64>{}, PendingReadAllocator(&budget))
    , m_activeReads(std::less<quint64>{}, ActiveReadAllocator(&budget))
{
}

QSharedPointer<KisCpuReadBindingLink> KisPageReadCoordinator::discoverCpuReadBinding(
    KisPageMetadataCoordinator &metadata, KisPageOwnerLedger &owner,
    const KisPageVersion &version, const QSharedPointer<KisCpuReadBindingLink> &stale)
{
    if (stale) {
        if (!(stale->replica.version == version)) return {};
        metadata.removeCpuReadBinding(stale->replica, stale.data());
    }
    // Another reader may already have installed the replacement. Preserve its
    // shared one-time provider lookup rather than creating another candidate.
    if (auto current = metadata.cpuReadBinding(version)) return current;
    const auto replica = metadata.cpuReadReplica(version);
    if (!replica.isValid()) return {};
    const auto provider = owner.provider(replica.provider, replica.providerEpoch);
    return metadata.installCpuReadBinding(replica, provider);
}

KisReadRequest KisPageReadCoordinator::registerRequestLocked(
    const KisPageVersion &version,
    const KisReplicaHandle &replica,
    KisPageAccessRequirement access,
    const KisCompletionTicket &readiness,
    KisPageTransactionId transaction,
    KisPageReadCleanup &cleanup)
{
    KisReadRequest request;
    request.access = access;
    request.id = KisPageRequestId{cleanup.m_preparedRequestId};
    request.version = version;
    request.readiness = readiness;
    request.status = request.id.isValid() && request.readiness.isValid()
            && replica.version == version
        ? KisPageRequestStatus::Ready : KisPageRequestStatus::Failed;
    if (request.status == KisPageRequestStatus::Failed) {
        request.error = QStringLiteral("PageStore read identity allocation failed");
        return request;
    }
    KisPagePendingReadRecord pending;
    pending.replica = replica;
    pending.access = access;
    pending.accessOperation = m_owner.nextOperationId();
    pending.reservedLease = m_owner.nextLeaseId();
    pending.readiness = readiness;
    pending.requestId = request.id.value;
    if (!pending.accessOperation.isValid() ||
        !pending.reservedLease.isValid()) {
        request = {};
        request.status = KisPageRequestStatus::Failed;
        request.error = QStringLiteral(
            "PageStore read operation identity allocation failed");
        return request;
    }
    auto node = cleanup.m_requests.extract(request.id.value);
    Q_ASSERT(!node.empty());
    node.mapped() = pending;
    cleanup.m_preparedRequestId = 0;
    const auto inserted = m_requests.insert(std::move(node));
    Q_ASSERT(inserted.inserted);
    // No caller can observe this node before AcquireRead succeeds. Rejection
    // removes it; successful protection already has its cancellation owner.
    auto removePrepared = qScopeGuard([&] {
        cleanup.m_requests.insert(m_requests.extract(request.id.value));
        cleanup.retainLocked();
    });
    KisPageTransition acquire;
    acquire.kind = KisPageTransitionKind::AcquireRead;
    acquire.version = pending.replica.version;
    acquire.target = pending.replica;
    acquire.lease = pending.reservedLease;
    acquire.transaction = transaction;
    const auto stateResult = m_metadata.applyOwner(version.key, acquire);
    if (!stateResult.accepted) {
        request.status = KisPageRequestStatus::Failed;
        request.error = stateResult.rejectionReason;
        return request;
    }
    removePrepared.dismiss();
    ++m_requestsCreated;
    return request;
}

KisReadLease KisPageReadCoordinator::resolveLocked(
    const KisReadRequest &request,
    const KisCompletionTicket &completion,
    QMutexLocker<QMutex> &ownerLock, KisPageReadCleanup &cleanup)
{
    auto requestIt = m_requests.find(request.id.value);
    if (!m_operational || !request.isValid() ||
        requestIt == m_requests.end() ||
        requestIt->second.state != KisPagePendingReadRecord::State::Pending ||
        !(requestIt->second.replica.version == request.version) ||
        !(requestIt->second.readiness == completion) || !m_completions ||
        !m_completions->verifyTerminal(completion).succeeded()) {
        return {};
    }
    const auto provider = m_owner.provider(
        requestIt->second.replica.provider, requestIt->second.replica.providerEpoch);
    if (!provider) return {};

    const KisPageLeaseId leaseId = requestIt->second.reservedLease;
    const KisPagePendingReadRecord pending = requestIt->second;
    std::shared_ptr<ActiveReadRecord> active;
    ActiveReadMap::node_type preparedNode;
    const auto discard = qScopeGuard([&] {
        if (!active && preparedNode.empty()) return;
        ownerLock.unlock();
        preparedNode = {};
        active.reset();
        ownerLock.relock();
    });
    requestIt->second.state = KisPagePendingReadRecord::State::Resolving;
    ownerLock.unlock();
    try {
        active = std::allocate_shared<ActiveReadRecord>(
            KisMutationStorageAllocator<ActiveReadRecord>::retained(&m_budget), provider, pending.replica);
        active->leaseId = leaseId.value;
        ActiveReadMap prepared(std::less<quint64>{}, m_activeReads.get_allocator());
        prepared.emplace(leaseId.value, active);
        preparedNode = prepared.extract(prepared.begin());
    } catch (const std::bad_alloc &) {
        // No physical access has begun; the same pending request is retryable.
        ownerLock.relock();
        requestIt = m_requests.find(request.id.value);
        if (requestIt != m_requests.end() &&
            requestIt->second.state == KisPagePendingReadRecord::State::Resolving)
            requestIt->second.state = KisPagePendingReadRecord::State::Pending;
        return {};
    }
    try {
        active->access.emplace(provider->resolveAccess(
            leaseId, pending.accessOperation, pending.replica,
            pending.access, KisPageAccessMode::Read));
    } catch (const std::bad_alloc &) {
        ownerLock.relock();
        requestIt = m_requests.find(request.id.value);
        if (requestIt != m_requests.end() &&
            requestIt->second.state == KisPagePendingReadRecord::State::Resolving)
            requestIt->second.state = KisPagePendingReadRecord::State::Pending;
        return {};
    }
    ownerLock.relock();
    auto &access = *active->access;
    requestIt = m_requests.find(request.id.value);
    if (requestIt == m_requests.end() ||
        requestIt->second.state != KisPagePendingReadRecord::State::Resolving) {
        if (access.isValid()) {
            ownerLock.unlock();
            provider->releaseAccess(std::move(access), {});
            ownerLock.relock();
        }
        return {};
    }
    if (!access.isValid(pending.access)) {
        // A provider may return a valid access for an incompatible requirement.
        // Release it outside the owner gate before cancelling metadata.
        if (access.isValid()) {
            ownerLock.unlock();
            provider->releaseAccess(std::move(access), {});
            ownerLock.relock();
            requestIt = m_requests.find(request.id.value);
            if (requestIt == m_requests.end()) return {};
        }
        requestIt->second.state = KisPagePendingReadRecord::State::Pending;
        cancelLocked(request, ownerLock, cleanup);
        return {};
    }

    KisReadLease lease;
    lease.m_leaseId = leaseId;
    lease.m_version = pending.replica.version;
    lease.m_access = pending.access;
    lease.m_cpuData = access.cpuReadData;
    lease.m_gpuAccess = access.gpuAccess;
    lease.m_rowStride = access.replica.layout.rowStride;
    lease.m_byteSize = access.replica.layout.byteSize;
    lease.m_lifetime = active;
    // The reserved lease identity is unique and Resolving excludes a second
    // consumer. Node transfer cannot allocate or invalidate another lease.
    const auto installed = m_activeReads.insert(std::move(preparedNode));
    Q_ASSERT(installed.inserted);
    cleanup.m_requests.insert(m_requests.extract(requestIt));
    cleanup.retainLocked();
    return lease;
}

bool KisPageReadCoordinator::cancelLocked(
    const KisReadRequest &request, QMutexLocker<QMutex> &ownerLock,
    KisPageReadCleanup &cleanup)
{
    auto requestIt = m_requests.find(request.id.value);
    if (!m_operational || !request.isValid() ||
        requestIt == m_requests.end() ||
        requestIt->second.state == KisPagePendingReadRecord::State::Resolving ||
        requestIt->second.state == KisPagePendingReadRecord::State::Collecting ||
        !(requestIt->second.replica.version == request.version) ||
        !(requestIt->second.readiness == request.readiness) ||
        !(requestIt->second.access == request.access)) {
        return false;
    }
    auto &pending = requestIt->second;
    const bool released = pending.state == KisPagePendingReadRecord::State::Released
        || releaseReadLocked(pending.replica, pending.reservedLease, cleanup);
    pending.state = released ? KisPagePendingReadRecord::State::Released
                             : KisPagePendingReadRecord::State::Cancelling;
    if (released) {
        pending.state = KisPagePendingReadRecord::State::Collecting;
        try {
            m_history.collectUnreachableLocked(&request.version.key, 1);
            processRetirementsUnlocked(ownerLock);
            requestIt = m_requests.find(request.id.value);
            Q_ASSERT(requestIt != m_requests.end());
            if (requestIt->second.cancelHook.is_linked())
                m_cancelRetries.erase(m_cancelRetries.iterator_to(requestIt->second));
            cleanup.m_requests.insert(m_requests.extract(requestIt));
            cleanup.retainLocked();
            return true;
        } catch (const std::bad_alloc &) {
            pending.state = KisPagePendingReadRecord::State::Released;
        }
    }
    if (!pending.cancelHook.is_linked()) m_cancelRetries.push_back(pending);
    if (m_backgroundReclamation && !m_lastUseClosing && !m_automaticWakeupsStopped) {
        QMutexLocker notificationLock(&m_lastUseWakeContext->mutex);
        m_lastUseWakeContext->cleanupPending = true;
        m_lastUseWakeContext->retry.cancel();
        m_lastUseWakeContext->retryScheduled = false;
        m_lastUseWakeContext->wake.notify();
    }
    return false;
}

void KisPageReadCoordinator::retryCancelledRequestsLocked(
    QMutexLocker<QMutex> &ownerLock, KisPageReadCleanup &cleanup, bool drain)
{
    const size_t attempts = drain ? m_cancelRetries.size()
                                  : std::min<size_t>(1, m_cancelRetries.size());
    for (size_t i = 0; i < attempts && !m_cancelRetries.empty(); ++i) {
        const quint64 id = m_cancelRetries.front().requestId;
        m_cancelRetries.pop_front();
        const auto pending = m_requests.find(id);
        if (pending == m_requests.cend()) continue;
        KisReadRequest request;
        request.status = KisPageRequestStatus::Ready;
        request.id = KisPageRequestId{id};
        request.version = pending->second.replica.version;
        request.access = pending->second.access;
        request.readiness = pending->second.readiness;
        cancelLocked(request, ownerLock, cleanup); // Refusal relinks the same original node.
    }
}

void KisPageReadCoordinator::releaseLocked(
    KisReadLease lease,
    const KisCompletionTicket &consumerLastUse,
    QMutexLocker<QMutex> &ownerLock, KisPageReadCleanup &cleanup)
{
    auto activeIt = m_activeReads.find(lease.m_leaseId.value);
    if (!lease.isValid() || activeIt == m_activeReads.end() ||
        !(activeIt->second->replica.version == lease.m_version) ||
        (!activeIt->second->access || !activeIt->second->access->isValid())) {
        return;
    }
    const auto active = activeIt->second;
    active->releaseLastUse = consumerLastUse;
    ownerLock.unlock();
    active->provider->releaseAccess(
        std::move(*active->access), consumerLastUse);
    ownerLock.relock();
    activeIt = m_activeReads.find(lease.m_leaseId.value);
    if (activeIt == m_activeReads.end()) return;
    active->provider.clear();
    if (!finishReleasedReadLocked(lease.m_leaseId.value, cleanup)) {
        if (!active->releaseHook.is_linked()) m_releaseRetries.push_back(*active);
        if (m_backgroundReclamation) notifyLastUse(m_lastUseWakeContext, active);
        return;
    }
    if (active->historyPending && !finishHistoryReadLocked(active, ownerLock, cleanup) && m_backgroundReclamation)
        notifyLastUse(m_lastUseWakeContext, active);
}

bool KisPageReadCoordinator::finishReleasedReadLocked(
    quint64 leaseId, KisPageReadCleanup &cleanup, bool *lastUseAcknowledged)
{
    if (lastUseAcknowledged) *lastUseAcknowledged = false;
    auto activeIt = m_activeReads.find(leaseId);
    if (activeIt == m_activeReads.end() ||
        (activeIt->second->access && activeIt->second->access->isValid()) || activeIt->second->provider)
        return false;
    const auto active = activeIt->second;
    if (!releaseReadLocked(active->replica, KisPageLeaseId{leaseId},
                           cleanup, active->releaseLastUse))
        return false;
    if (active->releaseLastUse.isValid()) {
        // Keep the original lease record until its last-use transition ends.
        // Metadata owns protection; this record owns its cleanup obligation.
        active->metadataReleased = true;
        ++m_pendingLastUses;
        const auto terminal = m_completions
            ? m_completions->verifyTerminal(active->releaseLastUse)
            : KisVerifiedCompletion{};
        if (terminal.isValid() && acknowledgeRecordLocked(active, terminal, cleanup)) {
            if (lastUseAcknowledged) *lastUseAcknowledged = true;
        } else {
            armLastUseLocked(active);
        }
    } else {
        active->historyPending = true;
        ++m_pendingHistoryReads;
    }
    return true;
}

bool KisPageReadCoordinator::finishHistoryReadLocked(
    const std::shared_ptr<ActiveReadRecord> &record,
    QMutexLocker<QMutex> &ownerLock, KisPageReadCleanup &cleanup)
{
    if (!record->historyPending) return true;
    if (record->historyProcessing) return false;
    record->historyProcessing = true;
    const auto done = qScopeGuard([&] { record->historyProcessing = false; });
    try {
        const auto key = record->replica.version.key;
        m_history.collectUnreachableLocked(&key, 1);
        processRetirementsUnlocked(ownerLock);
    } catch (const std::bad_alloc &) {
        // Preserve the exact key in its original admitted read record. A
        // failed GC notification must not discard that obligation after ack.
        QMutexLocker notificationLock(&m_lastUseWakeContext->mutex);
        ++m_lastUseWakeContext->dispatchFailures;
        return false;
    }
    const auto active = m_activeReads.find(record->leaseId);
    Q_ASSERT(active != m_activeReads.end() && active->second == record);
    record->historyPending = false;
    --m_pendingHistoryReads;
    if (record->releaseHook.is_linked()) m_releaseRetries.erase(m_releaseRetries.iterator_to(*record));
    cleanup.m_reads.insert(m_activeReads.extract(active));
    cleanup.retainLocked();
    return true;
}

bool KisPageReadCoordinator::releaseReadLocked(
    const KisReplicaHandle &replica,
    KisPageLeaseId lease, KisPageReadCleanup &cleanup,
    const KisCompletionTicket &completion)
{
    KisPageTransition release;
    release.kind = KisPageTransitionKind::ReleaseRead;
    release.version = replica.version;
    release.target = replica;
    release.lease = lease;
    release.completion = completion;
    const auto result = m_metadata.applyOwner(replica.version.key, release, &cleanup.m_storage);
    cleanup.retainLocked();
    return result.accepted;
}

void KisPageReadCoordinator::retryReleasedReadsLocked(
    QMutexLocker<QMutex> &ownerLock, KisPageReadCleanup &cleanup, const KisCompletionTicket &observed,
    bool *acknowledged, bool drain)
{
    if (acknowledged) *acknowledged = false;
    const size_t attempts = drain || observed.isValid() ? m_releaseRetries.size()
                                                       : std::min<size_t>(1, m_releaseRetries.size());
    for (size_t i = 0; i < attempts && !m_releaseRetries.empty(); ++i) {
        const quint64 leaseId = m_releaseRetries.front().leaseId;
        m_releaseRetries.pop_front();
        const auto found = m_activeReads.find(leaseId);
        const auto active = found == m_activeReads.end() ? nullptr : found->second;
        if (!active || active->metadataReleased) continue;
        const bool matches = observed.isValid() && active && active->releaseLastUse == observed;
        bool lastUseAcknowledged = false;
        if (finishReleasedReadLocked(leaseId, cleanup, &lastUseAcknowledged)) {
            if (active->historyPending && !finishHistoryReadLocked(active, ownerLock, cleanup) && m_backgroundReclamation)
                notifyLastUse(m_lastUseWakeContext, active);
            if (acknowledged && matches && lastUseAcknowledged) *acknowledged = true;
        } else {
            m_releaseRetries.push_back(*active);
            if (m_backgroundReclamation) notifyLastUse(m_lastUseWakeContext, active);
        }
    }
}

KisPageLastUseAcknowledgeResult
KisPageReadCoordinator::acknowledgeLastUseLocked(
    const KisVerifiedCompletion &completion, QMutexLocker<QMutex> &ownerLock, KisPageReadCleanup &cleanup)
{
    KisPageLastUseAcknowledgeResult result;
    // Explicit acknowledgement is a cold compatibility endpoint. Automatic
    // notifications address the original record directly and never scan this map.
    const quint64 last = m_activeReads.empty() ? 0 : m_activeReads.rbegin()->first;
    quint64 after = 0;
    for (;;) {
        const auto it = m_activeReads.upper_bound(after);
        if (it == m_activeReads.end() || it->first > last) break;
        after = it->first;
        const auto record = it->second;
        if (!record->metadataReleased || !(record->releaseLastUse == completion.ticket())) continue;
        result.matched = true;
        if (acknowledgeRecordLocked(record, completion, cleanup)) {
            if (!finishHistoryReadLocked(record, ownerLock, cleanup) && m_backgroundReclamation)
                notifyLastUse(m_lastUseWakeContext, record);
        } else {
            result.accepted = false;
        }
    }
    return result;
}

void KisPageReadCoordinator::acknowledgeCompletedLastUsesLocked(
    QMutexLocker<QMutex> &ownerLock, KisPageReadCleanup &cleanup)
{
    const quint64 last = m_activeReads.empty() ? 0 : m_activeReads.rbegin()->first;
    quint64 after = 0;
    for (;;) {
        const auto it = m_activeReads.upper_bound(after);
        if (it == m_activeReads.end() || it->first > last) break;
        after = it->first;
        const auto record = it->second;
        if (record->metadataReleased) {
            const auto terminal = m_completions->verifyTerminal(record->releaseLastUse);
            if (terminal.isValid()) acknowledgeRecordLocked(record, terminal, cleanup);
        }
        if (record->historyPending) finishHistoryReadLocked(record, ownerLock, cleanup);
    }
}

bool KisPageReadCoordinator::acknowledgeRecordLocked(
    const std::shared_ptr<ActiveReadRecord> &record, const KisVerifiedCompletion &completion,
    KisPageReadCleanup &cleanup)
{
    const auto active = m_activeReads.find(record->leaseId);
    if (!record->metadataReleased || !(record->releaseLastUse == completion.ticket()) ||
        active == m_activeReads.end() || active->second != record)
        return false;
    // One terminal ticket can cover several leases of this exact replica.
    // Metadata clears all matching occurrences in one owner transition. The
    // remaining original records observe that fact, without a second ack ledger.
    bool pending = true;
    if (!completion.isValid() ||
        !m_metadata.queryLastUsePending(record->replica, completion.ticket(), &pending)) return false;
    if (pending) {
        const auto result = m_metadata.acknowledgeLastUse(
            record->replica.version, record->replica, completion, &cleanup.m_storage);
        cleanup.retainLocked();
        if (!result.accepted) return false;
    }
    record->metadataReleased = false;
    --m_pendingLastUses;
    record->lastUseSubscription.reset();
    record->historyPending = true;
    ++m_pendingHistoryReads;
    return true;
}

bool KisPageReadCoordinator::belongsToPreparedTransactionLocked(
    const KisPageVersion &version,
    KisPageTransactionId transaction) const
{
    KisPageMetadataCoordinator::VersionInfo page;
    if (!m_metadata.versionSnapshot(version, &page)) return false;
    return page.version.isValid() && page.publication == KisPagePublicationState::Prepared &&
        page.preparedBy == transaction && !page.captured;
}

bool KisPageReadCoordinator::protectsPreparedTransactionLocked(
    KisPageTransactionId transaction) const
{
    for (const auto &[id, active] : m_activeReads) {
        if (belongsToPreparedTransactionLocked(active->replica.version, transaction)) {
            return true;
        }
    }
    return false;
}

bool KisPageReadCoordinator::cancelPreparedRequestsLocked(
    KisPageTransactionId transaction, KisPageReadCleanup &cleanup)
{
    for (auto it = m_requests.begin(); it != m_requests.end();) {
        if (!belongsToPreparedTransactionLocked(it->second.replica.version, transaction)) {
            ++it;
            continue;
        }
        if (it->second.state == KisPagePendingReadRecord::State::Resolving ||
            it->second.state == KisPagePendingReadRecord::State::Collecting) return false;
        if (it->second.state != KisPagePendingReadRecord::State::Released &&
            !releaseReadLocked(it->second.replica, it->second.reservedLease, cleanup)) {
            return false;
        }
        if (it->second.cancelHook.is_linked()) m_cancelRetries.erase(m_cancelRetries.iterator_to(it->second));
        const auto retired = it++;
        cleanup.m_requests.insert(m_requests.extract(retired));
        cleanup.retainLocked();
    }
    return true;
}

void KisPageReadCoordinator::processRetirementsUnlocked(QMutexLocker<QMutex> &ownerLock)
{
    if (m_backgroundReclamation) return;
    ++m_activeProviderCalls;
    ownerLock.unlock();
    const auto relock = qScopeGuard([&] {
        ownerLock.relock();
        --m_activeProviderCalls;
    });
    m_retirementQueue.process(std::numeric_limits<qsizetype>::max());
}

void KisPageReadCoordinator::noteCapturedViewCreatedLocked()
{
    ++m_capturedViewsCreated;
}

void KisPageCapturedReleaseDeleter::operator()(KisPageCapturedRelease *record) const noexcept
{
    if (!record) return;
    std::destroy_at(record);
    KisMutationStorageAllocator<KisPageCapturedRelease>(budget).deallocate(record, 1);
}

KisPageCapturedReleasePointer KisPageCapturedRelease::prepare(KisBackingBudgetController &budget)
{
    auto *record = KisMutationStorageAllocator<KisPageCapturedRelease>(&budget).allocate(1);
    return {new (record) KisPageCapturedRelease(budget), KisPageCapturedReleaseDeleter{&budget}};
}

bool KisPageReadCoordinator::releaseCapturedView(
    KisPageCapturedReleasePointer pending,
    QMutexLocker<QMutex> *heldOwnerLock)
{
    std::optional<QMutexLocker<QMutex>> acquired;
    if (!heldOwnerLock) {
        acquired.emplace(&m_ownerMutex);
        heldOwnerLock = &*acquired;
    }
    auto &lock = *heldOwnerLock;
    Q_ASSERT(pending && pending->token.isValid());
    m_ownerLifetime.ref();
    auto *record = pending.release();
    m_capturedReleaseRetries.push_back(*record);
    const bool finished = finishCapturedReleaseLocked(*record, lock);
    if (!finished && m_backgroundReclamation && !m_automaticWakeupsStopped && !m_lastUseClosing) {
        QMutexLocker notificationLock(&m_lastUseWakeContext->mutex);
        m_lastUseWakeContext->cleanupPending = true;
        m_lastUseWakeContext->retry.cancel();
        m_lastUseWakeContext->retryScheduled = false;
        m_lastUseWakeContext->wake.notify();
    }
    return finished;
}

void KisPageReadCoordinator::retryCapturedReleasesLocked(QMutexLocker<QMutex> &ownerLock,
                                                         bool drain)
{
    const size_t attempts = drain ? m_capturedReleaseRetries.size()
                                  : std::min<size_t>(1, m_capturedReleaseRetries.size());
    for (size_t i = 0; i < attempts && !m_capturedReleaseRetries.empty(); ++i) {
        auto &pending = m_capturedReleaseRetries.front();
        if (pending.processing) return;
        if (!finishCapturedReleaseLocked(pending, ownerLock))
            m_capturedReleaseRetries.splice(m_capturedReleaseRetries.end(),
                m_capturedReleaseRetries, m_capturedReleaseRetries.begin());
    }
}

bool KisPageReadCoordinator::finishCapturedReleaseLocked(
    KisPageCapturedRelease &pending, QMutexLocker<QMutex> &lock)
{
    Q_ASSERT(!pending.processing);
    pending.processing = true;
    try {
        while (pending.next < pending.retained) {
            const auto &version = pending.versions[pending.next];
            if (!pending.versionReleased) {
                KisPageTransition release;
                release.kind = KisPageTransitionKind::ReleaseCapturedVersion;
                release.version = version;
                release.readView = pending.token;
                if (!m_metadata.applyOwner(version.key, release, &pending.cleanup).accepted) {
                    pending.processing = false;
                    return false;
                }
                pending.versionReleased = true;
            }
            // Keep the exact key until history accepts it. Retrying a refused
            // handoff must not release the already removed metadata token again.
            m_history.collectUnreachableLocked(&version.key, 1);
            processRetirementsUnlocked(lock);
            ++pending.next;
            pending.versionReleased = false;
        }
        if (!pending.snapshotReleased) {
            bool becameUnretained = false;
            if (!m_operational || !m_epochs.releaseSnapshot(pending.token, nullptr, &becameUnretained)) {
                pending.processing = false;
                return false;
            }
            pending.snapshotReleased = true;
            pending.historyPending = becameUnretained;
            if (pending.capturedScope) ++m_capturedViewReleases;
        }
        if (pending.historyPending) {
            if (m_backgroundReclamation) {
                m_history.collectUnreachableLocked(nullptr, 0, true);
            } else {
                m_epochs.collectUnretainedRoots();
                m_history.collectUnreachableLocked(nullptr, 0, true);
                processRetirementsUnlocked(lock);
            }
            pending.historyPending = false;
        }
    } catch (const std::bad_alloc &) {
        pending.processing = false;
        return false; // The original linked record retains every unfinished fact.
    }
    m_capturedReleaseRetries.erase(m_capturedReleaseRetries.iterator_to(pending));
    KisPageCapturedReleasePointer finished(&pending, KisPageCapturedReleaseDeleter{&m_budget});
    // This record has handed off all semantic work. Its original lifetime pin
    // covers physical disposal; it must not make a concurrent restore Busy.
    lock.unlock();
    finished.reset();
    lock.relock();
    m_releaseLifetime(m_lifetimeContext);
    return true;
}

bool KisPageReadCoordinator::releaseSnapshot(
    KisImageEpochSnapshotToken token,
    const QVector<KisPageKey> *changedPages)
{
    QMutexLocker lock(&m_ownerMutex);
    const bool released = releaseSnapshotLocked(token, changedPages, lock);
    if (released)
        retryCapturedReleasesLocked(lock);
    return released;
}

bool KisPageReadCoordinator::releaseSnapshotLocked(
    KisImageEpochSnapshotToken token,
    const QVector<KisPageKey> *changedPages,
    QMutexLocker<QMutex> &lock)
{
    bool becameUnretained = false;
    if (!m_operational ||
        !m_epochs.releaseSnapshot(token, nullptr, &becameUnretained)) {
        return false;
    }
    if (m_backgroundReclamation) {
        if (!becameUnretained) return true;
        m_history.collectUnreachableLocked(
            changedPages ? changedPages->constData() : nullptr, changedPages ? changedPages->size() : 0, true);
        return true;
    }
    m_epochs.collectUnretainedRoots();
    m_history.collectUnreachableLocked(changedPages ? changedPages->constData() : nullptr,
        changedPages ? changedPages->size() : 0, !changedPages);
    processRetirementsUnlocked(lock);
    return true;
}

KisPageReadCoordinatorSnapshot KisPageReadCoordinator::snapshotLocked() const
{
    KisPageReadCoordinatorSnapshot result{qsizetype(m_requests.size()), qsizetype(m_activeReads.size()) - m_pendingLastUses - m_pendingHistoryReads,
        m_pendingLastUses, m_requestsCreated, m_capturedViewsCreated, m_capturedViewReleases};
    result.lastUsePasses = m_lastUsePasses;
    result.maximumLastUsesPerPass = m_maximumLastUsesPerPass;
    result.pendingCapturedReleases = qsizetype(m_capturedReleaseRetries.size());
    if (m_lastUseWakeContext) {
        QMutexLocker lock(&m_lastUseWakeContext->mutex);
        result.lastUseJobScheduled = m_lastUseWakeContext->jobScheduled ||
            (m_lastUseWakeContext->accepting && !m_lastUseWakeContext->retryScheduled &&
             (m_lastUseWakeContext->head || m_lastUseWakeContext->cleanupPending));
        result.lastUseDispatchFailures = m_lastUseWakeContext->dispatchFailures;
    }
    return result;
}

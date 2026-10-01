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

KisPageReadCleanup::~KisPageReadCleanup()
{
    finish();
}

void KisPageReadCleanup::retainLocked()
{
    if (!m_active && !m_storage.isEmpty()) {
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
    bool accepting = true;
    bool jobScheduled = false;
    bool dispatchFailed = false;
    quint64 dispatchFailures = 0;

    void clearReadyLocked()
    {
        while (head) {
            auto record = std::move(head);
            head = std::move(record->nextReady);
            record->readyQueued = false;
        }
        tail = nullptr;
    }
};

KisPageReadCoordinator::~KisPageReadCoordinator()
{
    if (m_lastUseWakeContext) {
        QMutexLocker lock(&m_lastUseWakeContext->mutex);
        Q_ASSERT(!m_lastUseWakeContext->jobScheduled);
        m_lastUseWakeContext->coordinator = nullptr;
        m_lastUseWakeContext->accepting = false;
        m_lastUseWakeContext->clearReadyLocked();
        m_lastUseWakeContext->wake.reset();
    }
}

bool KisPageReadCoordinator::prepareLastUseContextLocked()
{
    if (m_lastUseWakeContext) return true;
    std::shared_ptr<LastUseWakeContext> prepared;
    const auto discard = qScopeGuard([&] {
        if (!prepared) return;
        m_ownerMutex.unlock();
        prepared.reset();
        m_ownerMutex.lock();
    });
    {
        m_ownerMutex.unlock();
        const auto relock = qScopeGuard([&] { m_ownerMutex.lock(); });
        try {
            prepared = std::allocate_shared<LastUseWakeContext>(
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
                    if (!context->dispatchFailed && context->accepting && context->head)
                        context->wake.notify();
                    context->idle.wakeAll();
                }
                coordinator->m_releaseLifetime(coordinator->m_lifetimeContext);
            }, this);
            prepared->wake = kisPreparePageStoreReclamationWake(
                KisPageReadinessCallback([weak = std::weak_ptr<LastUseWakeContext>(prepared)] {
                    dispatchLastUses(weak);
                }, &m_budget), &m_budget);
        } catch (const std::bad_alloc &) {
            // Original read debt is still present; no consumer was installed.
        }
    }
    if (!prepared || !prepared->wake.isValid() || !m_backgroundReclamation ||
        m_automaticWakeupsStopped || m_lastUseClosing) return false;
    if (!m_lastUseWakeContext) m_lastUseWakeContext = std::move(prepared);
    return true;
}

void KisPageReadCoordinator::armLastUseLocked(const std::shared_ptr<ActiveReadRecord> &source)
{
    const auto record = source;
    if (!m_backgroundReclamation || m_automaticWakeupsStopped || m_lastUseClosing ||
        !record->metadataReleased || record->lastUseSubscription.isValid()) return;
    try {
        if (!prepareLastUseContextLocked()) return;
        const auto active = m_activeReads.find(record->leaseId);
        if (active == m_activeReads.end() || active->second != record ||
            m_automaticWakeupsStopped || m_lastUseClosing ||
            !record->metadataReleased || record->lastUseSubscription.isValid()) return;
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
        if (status == KisPageReadinessStatus::Ready) notifyLastUse(context, pending);
    } catch (const std::bad_alloc &) {
        // The original record and metadata protection survive. Explicit
        // acknowledgement/close still consume them; never drop a last-use debt.
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
    if (!context->coordinator || !context->accepting || record->readyQueued) return;
    record->readyQueued = true;
    if (context->tail) context->tail->nextReady = record;
    else context->head = record;
    context->tail = record.get();
    context->dispatchFailed = false;
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
    if (!coordinator || !context->accepting || !context->head || context->jobScheduled) return;
    auto &references = coordinator->m_ownerLifetime;
    int count = references.loadAcquire();
    for (;;) {
        if (count <= 0 || count == std::numeric_limits<int>::max()) return;
        if (references.testAndSetOrdered(count, count + 1)) break;
        count = references.loadAcquire();
    }
    context->jobScheduled = true;
    context->dispatchFailed = false;
    lock.unlock();
    kisEnqueuePageStoreReclamation(context->task.get());
}

void KisPageReadCoordinator::processLastUses(const std::shared_ptr<LastUseWakeContext> &context)
{
    KisPageReadCleanup cleanup(*this);
    bool failed = false;
    try {
        QMutexLocker ownerLock(&m_ownerMutex);
        constexpr qsizetype maximumPerPass = 32;
        QVector<KisPageKey> released;
        released.reserve(maximumPerPass);
        qsizetype visited = 0;
        for (; visited < maximumPerPass; ++visited) {
            std::shared_ptr<ActiveReadRecord> record;
            {
                QMutexLocker lock(&context->mutex);
                if (!context->accepting || !context->head) break;
                record = std::move(context->head);
                context->head = std::move(record->nextReady);
                if (!context->head) context->tail = nullptr;
                record->readyQueued = false;
            }
            const auto active = m_activeReads.find(record->leaseId);
            if (!m_operational || !record->metadataReleased ||
                active == m_activeReads.end() || active->second != record) continue;
            const auto terminal = m_completions->verifyTerminal(record->releaseLastUse);
            if (terminal.isValid() && acknowledgeRecordLocked(record, terminal, cleanup))
                released.append(record->replica.version.key);
        }
        if (visited) {
            ++m_lastUsePasses;
            m_maximumLastUsesPerPass = std::max(m_maximumLastUsesPerPass, quint64(visited));
        }
        if (!released.isEmpty())
            retireEffectsUnlocked(m_history.collectUnreachableLocked(released), ownerLock);
    } catch (const std::bad_alloc &) {
        failed = true;
    }
    cleanup.finish(); // complete physical cleanup before idle/lifetime publication
    {
        QMutexLocker lock(&context->mutex);
        context->dispatchFailed = failed;
        if (failed) ++context->dispatchFailures;
    }
}

void KisPageReadCoordinator::beginCloseLocked()
{
    m_lastUseClosing = true;
    if (m_lastUseWakeContext) {
        QMutexLocker lock(&m_lastUseWakeContext->mutex);
        m_lastUseWakeContext->accepting = false;
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
    if (!m_lastUseWakeContext && !m_activeReads.empty() && !prepareLastUseContextLocked()) return;
    if (m_lastUseWakeContext) {
        QMutexLocker lock(&m_lastUseWakeContext->mutex);
        m_lastUseWakeContext->accepting = true;
    }
    for (const auto &[id, record] : std::as_const(m_activeReads))
        if (record->metadataReleased) armLastUseLocked(record);
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
           (context->accepting && context->head && !context->dispatchFailed))
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
    KisPageTransactionId transaction)
{
    KisReadRequest request;
    request.access = access;
    request.id = m_owner.nextRequestId();
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
    if (!pending.accessOperation.isValid() ||
        !pending.reservedLease.isValid()) {
        request = {};
        request.status = KisPageRequestStatus::Failed;
        request.error = QStringLiteral(
            "PageStore read operation identity allocation failed");
        return request;
    }
    try {
        m_requests.emplace(request.id.value, pending);
    } catch (const std::bad_alloc &) {
        request.status = KisPageRequestStatus::Failed;
        request.error = QStringLiteral("PageStore read request storage is unavailable");
        return request;
    }
    // No caller can observe this node before AcquireRead succeeds. Rejection
    // removes it; successful protection already has its cancellation owner.
    auto removePrepared = qScopeGuard([&] { m_requests.erase(request.id.value); });
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
    m_requests.erase(requestIt);
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
        !(requestIt->second.replica.version == request.version) ||
        !(requestIt->second.readiness == request.readiness) ||
        !(requestIt->second.access == request.access)) {
        return false;
    }
    if (!releaseReadLocked(requestIt->second.replica, requestIt->second.reservedLease, cleanup)) {
        if (requestIt->second.state != KisPagePendingReadRecord::State::Cancelling) {
            requestIt->second.state = KisPagePendingReadRecord::State::Cancelling;
            m_cancelRetries.push_back(request.id.value);
        }
        return false;
    }
    m_requests.erase(requestIt);
    retireEffectsUnlocked(m_history.collectUnreachableLocked({request.version.key}), ownerLock);
    return true;
}

void KisPageReadCoordinator::retryCancelledRequestsLocked(
    QMutexLocker<QMutex> &ownerLock, KisPageReadCleanup &cleanup, bool drain)
{
    const size_t attempts = drain ? m_cancelRetries.size()
                                  : std::min<size_t>(1, m_cancelRetries.size());
    for (size_t i = 0; i < attempts && !m_cancelRetries.empty(); ++i) {
        const quint64 id = m_cancelRetries.front();
        m_cancelRetries.pop_front();
        const auto pending = m_requests.find(id);
        if (pending == m_requests.cend()) continue;
        KisReadRequest request;
        request.status = KisPageRequestStatus::Ready;
        request.id = KisPageRequestId{id};
        request.version = pending->second.replica.version;
        request.access = pending->second.access;
        request.readiness = pending->second.readiness;
        if (!cancelLocked(request, ownerLock, cleanup)) m_cancelRetries.push_back(id);
    }
}

KisPageKey KisPageReadCoordinator::releaseLocked(
    KisReadLease lease,
    const KisCompletionTicket &consumerLastUse,
    QMutexLocker<QMutex> &ownerLock, KisPageReadCleanup &cleanup)
{
    KisPageKey releasedKey;
    auto activeIt = m_activeReads.find(lease.m_leaseId.value);
    if (!lease.isValid() || activeIt == m_activeReads.end() ||
        !(activeIt->second->replica.version == lease.m_version) ||
        (!activeIt->second->access || !activeIt->second->access->isValid())) {
        return {};
    }
    const auto active = activeIt->second;
    active->releaseLastUse = consumerLastUse;
    ownerLock.unlock();
    active->provider->releaseAccess(
        std::move(*active->access), consumerLastUse);
    ownerLock.relock();
    activeIt = m_activeReads.find(lease.m_leaseId.value);
    if (activeIt == m_activeReads.end()) return {};
    active->provider.clear();
    if (!finishReleasedReadLocked(lease.m_leaseId.value, &releasedKey, cleanup)) {
        m_releaseRetries.push_back(lease.m_leaseId.value);
        return {};
    }
    return releasedKey;
}

bool KisPageReadCoordinator::finishReleasedReadLocked(
    quint64 leaseId, KisPageKey *key, KisPageReadCleanup &cleanup, bool *lastUseAcknowledged)
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
        m_activeReads.erase(activeIt);
    }
    if (key) *key = active->replica.version.key;
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

QVector<KisPageKey> KisPageReadCoordinator::retryReleasedReadsLocked(
    KisPageReadCleanup &cleanup, const KisCompletionTicket &observed,
    bool *acknowledged, bool drain)
{
    if (acknowledged) *acknowledged = false;
    QVector<KisPageKey> released;
    const size_t attempts = drain || observed.isValid() ? m_releaseRetries.size()
                                                       : std::min<size_t>(1, m_releaseRetries.size());
    for (size_t i = 0; i < attempts && !m_releaseRetries.empty(); ++i) {
        const quint64 leaseId = m_releaseRetries.front();
        m_releaseRetries.pop_front();
        const auto found = m_activeReads.find(leaseId);
        const auto active = found == m_activeReads.end() ? nullptr : found->second;
        if (!active || active->metadataReleased) continue;
        const bool matches = observed.isValid() && active && active->releaseLastUse == observed;
        KisPageKey key;
        bool lastUseAcknowledged = false;
        if (finishReleasedReadLocked(leaseId, &key, cleanup, &lastUseAcknowledged)) {
            released.append(key);
            if (acknowledged && matches && lastUseAcknowledged) *acknowledged = true;
        } else {
            m_releaseRetries.push_back(leaseId);
        }
    }
    return released;
}

KisPageLastUseAcknowledgeResult
KisPageReadCoordinator::acknowledgeLastUseLocked(
    const KisVerifiedCompletion &completion, KisPageReadCleanup &cleanup)
{
    KisPageLastUseAcknowledgeResult result;
    // Explicit acknowledgement is a cold compatibility endpoint. Automatic
    // notifications address the original record directly and never scan this map.
    QVector<std::shared_ptr<ActiveReadRecord>> matching;
    for (const auto &[id, record] : std::as_const(m_activeReads)) {
        if (record->metadataReleased && record->releaseLastUse == completion.ticket())
            matching.append(record);
    }
    for (const auto &record : std::as_const(matching)) {
        result.matched = true;
        if (acknowledgeRecordLocked(record, completion, cleanup))
            result.releasedKeys.append(record->replica.version.key);
        else
            result.accepted = false;
    }
    return result;
}

QVector<KisPageKey> KisPageReadCoordinator::acknowledgeCompletedLastUsesLocked(
    KisPageReadCleanup &cleanup)
{
    QVector<std::shared_ptr<ActiveReadRecord>> pending;
    for (const auto &[id, record] : std::as_const(m_activeReads))
        if (record->metadataReleased) pending.append(record);
    QVector<KisPageKey> released;
    for (const auto &record : std::as_const(pending)) {
        const auto terminal = m_completions->verifyTerminal(record->releaseLastUse);
        if (terminal.isValid() && acknowledgeRecordLocked(record, terminal, cleanup))
            released.append(record->replica.version.key);
    }
    return released;
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
    m_activeReads.erase(record->leaseId);
    return true;
}

bool KisPageReadCoordinator::belongsToPreparedTransactionLocked(
    const KisPageVersion &version,
    KisPageTransactionId transaction) const
{
    KisPageStateSnapshot page;
    if (!m_metadata.versionSnapshot(version, &page)) return false;
    const auto *state = page.findVersion(version);
    return state && state->publication == KisPagePublicationState::Prepared &&
        state->preparedBy == transaction && state->capturedReadViews.isEmpty();
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
        if (it->second.state == KisPagePendingReadRecord::State::Resolving) return false;
        if (!releaseReadLocked(it->second.replica, it->second.reservedLease, cleanup)) {
            return false;
        }
        it = m_requests.erase(it);
    }
    return true;
}

void KisPageReadCoordinator::retireEffectsUnlocked(
    QVector<KisPageTransitionEffect> effects,
    QMutexLocker<QMutex> &ownerLock)
{
    if (effects.isEmpty()) return;
    ++m_activeProviderCalls;
    ownerLock.unlock();
    m_retirementQueue.retireEffects(effects, m_backgroundReclamation);
    ownerLock.relock();
    --m_activeProviderCalls;
}

void KisPageReadCoordinator::noteCapturedViewCreatedLocked()
{
    ++m_capturedViewsCreated;
}

bool KisPageReadCoordinator::releaseCapturedView(
    KisImageEpochSnapshotToken token,
    const QHash<KisPageKey, KisPageVersion> &versions,
    bool capturedScope,
    QMutexLocker<QMutex> *heldOwnerLock)
{
    std::optional<QMutexLocker<QMutex>> acquired;
    if (!heldOwnerLock) {
        acquired.emplace(&m_ownerMutex);
        heldOwnerLock = &*acquired;
    }
    auto &lock = *heldOwnerLock;
    retryCapturedReleasesLocked(lock);
    PendingCapturedRelease pending{token, {}, capturedScope};
    for (const auto &version : versions) {
        pending.versions.append(version);
    }
    const auto keys = releaseCapturedVersionsLocked(pending);
    retireEffectsUnlocked(m_history.collectUnreachableLocked(keys), lock);
    if (pending.versions.isEmpty() && releaseSnapshotLocked(token, nullptr, capturedScope, lock))
        return true;
    m_ownerLifetime.ref();
    m_capturedReleaseRetries.push_back(std::move(pending));
    return false;
}

void KisPageReadCoordinator::retryCapturedReleasesLocked(QMutexLocker<QMutex> &ownerLock,
                                                         bool drain)
{
    const size_t attempts = drain ? m_capturedReleaseRetries.size()
                                  : std::min<size_t>(1, m_capturedReleaseRetries.size());
    for (size_t i = 0; i < attempts && !m_capturedReleaseRetries.empty(); ++i) {
        PendingCapturedRelease pending = std::move(m_capturedReleaseRetries.front());
        m_capturedReleaseRetries.pop_front();
        const auto keys = releaseCapturedVersionsLocked(pending);
        retireEffectsUnlocked(m_history.collectUnreachableLocked(keys), ownerLock);
        if (pending.versions.isEmpty() &&
            releaseSnapshotLocked(pending.token, nullptr, pending.capturedScope, ownerLock)) {
            m_releaseLifetime(m_lifetimeContext);
        } else {
            m_capturedReleaseRetries.push_back(std::move(pending));
        }
    }
}

QVector<KisPageKey> KisPageReadCoordinator::releaseCapturedVersionsLocked(
    PendingCapturedRelease &pending)
{
    QVector<KisPageVersion> failed;
    QVector<KisPageKey> released;
    for (const auto &version : std::as_const(pending.versions)) {
        KisPageTransition release;
        release.kind = KisPageTransitionKind::ReleaseCapturedVersion;
        release.version = version;
        release.readView = pending.token;
        if (m_metadata.applyOwner(version.key, release).accepted)
            released.append(version.key);
        else
            failed.append(version);
    }
    pending.versions = std::move(failed);
    return released;
}

bool KisPageReadCoordinator::releaseSnapshot(
    KisImageEpochSnapshotToken token,
    const QVector<KisPageKey> *changedPages)
{
    QMutexLocker lock(&m_ownerMutex);
    const bool released = releaseSnapshotLocked(token, changedPages, false, lock);
    if (released)
        retryCapturedReleasesLocked(lock);
    return released;
}

bool KisPageReadCoordinator::releaseSnapshotLocked(
    KisImageEpochSnapshotToken token,
    const QVector<KisPageKey> *changedPages,
    bool capturedScope,
    QMutexLocker<QMutex> &lock)
{
    bool becameUnretained = false;
    if (!m_operational ||
        !m_epochs.releaseSnapshot(token, nullptr, &becameUnretained)) {
        return false;
    }
    if (capturedScope) ++m_capturedViewReleases;
    if (m_backgroundReclamation) {
        if (!becameUnretained) return true;
        m_history.collectUnreachableLocked(
            changedPages ? *changedPages : QVector<KisPageKey>{}, true);
        return true;
    }
    const qsizetype removedRoots = m_epochs.collectUnretainedRoots();
    if (capturedScope && removedRoots == 0) return true;
    const QVector<KisPageTransitionEffect> retirements = changedPages
        ? m_history.collectUnreachableLocked(*changedPages)
        : capturedScope
            ? m_history.collectUnreachableLocked(m_history.deferredKeysLocked())
            : m_history.collectUnreachableLocked({}, true);
    retireEffectsUnlocked(retirements, lock);
    return true;
}

KisPageReadCoordinatorSnapshot KisPageReadCoordinator::snapshotLocked() const
{
    KisPageReadCoordinatorSnapshot result{qsizetype(m_requests.size()), qsizetype(m_activeReads.size()) - m_pendingLastUses,
        m_pendingLastUses, m_requestsCreated, m_capturedViewsCreated, m_capturedViewReleases};
    result.lastUsePasses = m_lastUsePasses;
    result.maximumLastUsesPerPass = m_maximumLastUsesPerPass;
    if (m_lastUseWakeContext) {
        QMutexLocker lock(&m_lastUseWakeContext->mutex);
        result.lastUseJobScheduled = m_lastUseWakeContext->jobScheduled ||
            (m_lastUseWakeContext->accepting && m_lastUseWakeContext->head &&
             !m_lastUseWakeContext->dispatchFailed);
        result.lastUseDispatchFailures = m_lastUseWakeContext->dispatchFailures;
    }
    return result;
}

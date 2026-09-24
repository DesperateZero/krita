/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageReadCoordinator_p.h"

#include <QMutexLocker>

#include <algorithm>
#include <optional>
#include <utility>

KisPageReadCoordinator::ActiveReadRecord::ActiveReadRecord(
    const QSharedPointer<KisPageReplicaProvider> &providerValue,
    KisReplicaAccess &&accessValue)
    : provider(providerValue)
    , replica(accessValue.replica)
    , access(std::move(accessValue))
{
}

KisPageReadCoordinator::ActiveReadRecord::~ActiveReadRecord()
{
    if (provider && access.isValid()) {
        provider->releaseAccess(std::move(access), {});
    }
}

KisPageReadCoordinator::KisPageReadCoordinator(
    KisPageMetadataCoordinator &metadata,
    KisImageEpochReferenceModel &epochs,
    KisPageOwnerLedger &owner,
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
{
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
    m_requests.insert(request.id.value, pending);
    ++m_requestsCreated;
    return request;
}

KisReadLease KisPageReadCoordinator::resolveLocked(
    const KisReadRequest &request,
    const KisCompletionTicket &completion,
    QMutexLocker<QMutex> &ownerLock)
{
    auto requestIt = m_requests.find(request.id.value);
    if (!m_operational || !request.isValid() ||
        requestIt == m_requests.end() ||
        requestIt->state != KisPagePendingReadRecord::State::Pending ||
        !(requestIt->replica.version == request.version) ||
        !(requestIt->readiness == completion) || !m_completions ||
        !m_completions->verifyTerminal(completion).succeeded()) {
        return {};
    }
    const auto provider = m_owner.provider(
        requestIt->replica.provider, requestIt->replica.providerEpoch);
    if (!provider) return {};

    const KisPageLeaseId leaseId = requestIt->reservedLease;
    requestIt->state = KisPagePendingReadRecord::State::Resolving;
    const KisPagePendingReadRecord pending = requestIt.value();
    ownerLock.unlock();
    KisReplicaAccess access = provider->resolveAccess(
        leaseId, pending.accessOperation, pending.replica,
        pending.access, KisPageAccessMode::Read);
    ownerLock.relock();
    requestIt = m_requests.find(request.id.value);
    if (requestIt == m_requests.end() ||
        requestIt->state != KisPagePendingReadRecord::State::Resolving) {
        if (access.isValid()) {
            ownerLock.unlock();
            provider->releaseAccess(std::move(access), {});
            ownerLock.relock();
        }
        return {};
    }
    if (!access.isValid(pending.access)) {
        requestIt->state = KisPagePendingReadRecord::State::Pending;
        cancelLocked(request, ownerLock);
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
    auto active = std::make_shared<ActiveReadRecord>(
        provider, std::move(access));
    lease.m_lifetime = active;
    m_activeReads.insert(leaseId.value, active);
    m_requests.erase(requestIt);
    return lease;
}

bool KisPageReadCoordinator::cancelLocked(
    const KisReadRequest &request, QMutexLocker<QMutex> &ownerLock)
{
    auto requestIt = m_requests.find(request.id.value);
    if (!m_operational || !request.isValid() ||
        requestIt == m_requests.end() ||
        requestIt->state == KisPagePendingReadRecord::State::Resolving ||
        !(requestIt->replica.version == request.version) ||
        !(requestIt->readiness == request.readiness) ||
        !(requestIt->access == request.access)) {
        return false;
    }
    if (!releaseReadLocked(requestIt->replica, requestIt->reservedLease)) {
        if (requestIt->state != KisPagePendingReadRecord::State::Cancelling) {
            requestIt->state = KisPagePendingReadRecord::State::Cancelling;
            m_cancelRetries.push_back(request.id.value);
        }
        return false;
    }
    m_requests.erase(requestIt);
    retireEffectsUnlocked(m_history.collectUnreachableLocked({request.version.key}), ownerLock);
    return true;
}

void KisPageReadCoordinator::retryCancelledRequestsLocked(QMutexLocker<QMutex> &ownerLock,
                                                           bool drain)
{
    const size_t attempts = drain ? m_cancelRetries.size()
                                  : std::min<size_t>(1, m_cancelRetries.size());
    for (size_t i = 0; i < attempts && !m_cancelRetries.empty(); ++i) {
        const quint64 id = m_cancelRetries.front();
        m_cancelRetries.pop_front();
        const auto pending = m_requests.constFind(id);
        if (pending == m_requests.cend()) continue;
        KisReadRequest request;
        request.status = KisPageRequestStatus::Ready;
        request.id = KisPageRequestId{id};
        request.version = pending->replica.version;
        request.access = pending->access;
        request.readiness = pending->readiness;
        if (!cancelLocked(request, ownerLock)) m_cancelRetries.push_back(id);
    }
}

KisPageKey KisPageReadCoordinator::releaseLocked(
    KisReadLease lease,
    const KisCompletionTicket &consumerLastUse,
    QMutexLocker<QMutex> &ownerLock)
{
    KisPageKey releasedKey;
    auto activeIt = m_activeReads.find(lease.m_leaseId.value);
    if (!lease.isValid() || activeIt == m_activeReads.end() ||
        !(activeIt.value()->replica.version == lease.m_version) ||
        !activeIt.value()->access.isValid()) {
        return {};
    }
    const auto active = activeIt.value();
    active->releaseLastUse = consumerLastUse;
    ownerLock.unlock();
    active->provider->releaseAccess(
        std::move(active->access), consumerLastUse);
    ownerLock.relock();
    activeIt = m_activeReads.find(lease.m_leaseId.value);
    if (activeIt == m_activeReads.end()) return {};
    active->provider.clear();
    if (!finishReleasedReadLocked(lease.m_leaseId.value, &releasedKey)) {
        m_releaseRetries.push_back(lease.m_leaseId.value);
        return {};
    }
    return releasedKey;
}

bool KisPageReadCoordinator::finishReleasedReadLocked(quint64 leaseId, KisPageKey *key,
                                                       bool *lastUseAcknowledged)
{
    if (lastUseAcknowledged) *lastUseAcknowledged = false;
    auto activeIt = m_activeReads.find(leaseId);
    if (activeIt == m_activeReads.end() ||
        activeIt.value()->access.isValid() || activeIt.value()->provider)
        return false;
    const auto active = activeIt.value();
    if (!releaseReadLocked(active->replica, KisPageLeaseId{leaseId},
                           active->releaseLastUse))
        return false;
    m_activeReads.erase(activeIt);
    if (active->releaseLastUse.isValid()) {
        m_pendingLastUses.append(
            {active->replica, active->releaseLastUse});
        const auto terminal = m_completions
            ? m_completions->verifyTerminal(active->releaseLastUse)
            : KisVerifiedCompletion{};
        if (terminal.isValid()) {
            if (m_metadata.acknowledgeLastUse(
                    active->replica.version, active->replica, terminal).accepted) {
                m_pendingLastUses.removeLast();
                if (lastUseAcknowledged) *lastUseAcknowledged = true;
            }
        }
    }
    if (key) *key = active->replica.version.key;
    return true;
}

bool KisPageReadCoordinator::releaseReadLocked(
    const KisReplicaHandle &replica,
    KisPageLeaseId lease,
    const KisCompletionTicket &completion)
{
    KisPageTransition release;
    release.kind = KisPageTransitionKind::ReleaseRead;
    release.version = replica.version;
    release.target = replica;
    release.lease = lease;
    release.completion = completion;
    return m_metadata.applyOwner(replica.version.key, release).accepted;
}

QVector<KisPageKey> KisPageReadCoordinator::retryReleasedReadsLocked(
    const KisCompletionTicket &observed, bool *acknowledged, bool drain)
{
    if (acknowledged) *acknowledged = false;
    QVector<KisPageKey> released;
    const size_t attempts = drain || observed.isValid() ? m_releaseRetries.size()
                                                       : std::min<size_t>(1, m_releaseRetries.size());
    for (size_t i = 0; i < attempts && !m_releaseRetries.empty(); ++i) {
        const quint64 leaseId = m_releaseRetries.front();
        m_releaseRetries.pop_front();
        const auto active = m_activeReads.value(leaseId);
        const bool matches = observed.isValid() && active && active->releaseLastUse == observed;
        KisPageKey key;
        bool lastUseAcknowledged = false;
        if (finishReleasedReadLocked(leaseId, &key, &lastUseAcknowledged)) {
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
    const KisVerifiedCompletion &completion)
{
    KisPageLastUseAcknowledgeResult result;
    for (qsizetype i = m_pendingLastUses.size(); i > 0; --i) {
        const auto &pending = m_pendingLastUses.at(i - 1);
        if (!(pending.completion == completion.ticket())) continue;
        result.matched = true;
        const auto acknowledged = m_metadata.acknowledgeLastUse(
            pending.replica.version, pending.replica, completion);
        result.accepted = result.accepted && acknowledged.accepted;
        if (acknowledged.accepted) {
            result.releasedKeys.append(pending.replica.version.key);
            m_pendingLastUses.removeAt(i - 1);
        }
    }
    return result;
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
    for (const auto &active : m_activeReads) {
        if (belongsToPreparedTransactionLocked(active->replica.version, transaction)) {
            return true;
        }
    }
    for (const auto &pending : m_pendingLastUses) {
        if (belongsToPreparedTransactionLocked(pending.replica.version, transaction)) {
            return true;
        }
    }
    return false;
}

bool KisPageReadCoordinator::cancelPreparedRequestsLocked(
    KisPageTransactionId transaction)
{
    for (auto it = m_requests.begin(); it != m_requests.end();) {
        if (!belongsToPreparedTransactionLocked(it->replica.version, transaction)) {
            ++it;
            continue;
        }
        if (it->state == KisPagePendingReadRecord::State::Resolving) return false;
        if (!releaseReadLocked(it->replica, it->reservedLease)) {
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
    return {m_requests.size(), m_activeReads.size(), m_pendingLastUses.size(),
            m_requestsCreated, m_capturedViewsCreated,
            m_capturedViewReleases};
}

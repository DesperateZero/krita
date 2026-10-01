/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisCpuResidentBinding_p.h"
#include <QScopeGuard>
#include "KisCompletionRegistry.h"
#include <QMutexLocker>
#include <cstring>
#include <limits>
#include <new>

KisCpuResidentBinding::~KisCpuResidentBinding()
{
    Q_ASSERT(m_readers == 0 && m_writeState == WriteState::Idle);
}

bool KisCpuResidentBinding::matchesHandle(const KisReplicaHandle &expected) const
{
    QMutexLocker lock(&m_mutex);
    return !m_retired && m_handle == expected;
}

bool KisCpuResidentBinding::matchesAllocation(KisReplicaAllocationToken expected) const
{
    QMutexLocker lock(&m_mutex);
    return !m_retired && m_handle.allocation == expected;
}

bool KisCpuResidentBinding::matchesReadIdentity(
    const KisReplicaAllocationIdentity &expected, const KisPageVersion &version) const
{
    QMutexLocker lock(&m_mutex);
    return !m_retired && m_handle.allocationIdentity() == expected && m_handle.version == version;
}

bool KisCpuResidentBinding::prepareHandoff(const KisReplicaHandle &source,
    const KisPageVersion &target, const KisPageAllocationDescriptor &descriptor,
    KisReplicaHandle *result) const
{
    QMutexLocker lock(&m_mutex);
    if (!supportsBackingHandoff() || m_retired || !(m_handle == source) ||
        source.domain != KisPageAccessDomain::CpuRam || !source.layout.matches(descriptor) ||
        !target.isValid() || !(target.key == source.version.key) ||
        target.generation.value <= source.version.generation.value ||
        source.allocation.generation == std::numeric_limits<quint64>::max()) return false;
    *result = source;
    result->version = target;
    ++result->allocation.generation;
    return true;
}

bool KisCpuResidentBinding::tryClaimHandoff(const KisReplicaHandle &source)
{
    if (!m_mutex.tryLock()) return false;
    const auto unlock = qScopeGuard([&] { m_mutex.unlock(); });
    if (m_retired || !(m_handle == source) || m_readers || m_writeState != WriteState::Idle)
        return false;
    m_data = tryClaimStorageForHandoff();
    if (!m_data) return false;
    m_writeState = WriteState::Handoff;
    return true;
}

void KisCpuResidentBinding::finishHandoff(const KisReplicaHandle *target) noexcept
{
    QMutexLocker lock(&m_mutex);
    Q_ASSERT(m_writeState == WriteState::Handoff && m_data && !m_retired);
    if (target) m_handle = *target;
    finishStorageHandoff(target != nullptr);
    m_writeState = target ? WriteState::Reserved : WriteState::Idle;
    if (!target) m_data = nullptr;
}

KisCpuBackingHandoff::~KisCpuBackingHandoff() { reset(); }
KisCpuBackingHandoff::KisCpuBackingHandoff(KisCpuBackingHandoff &&other) noexcept
    : m_provider(std::move(other.m_provider)), m_binding(std::move(other.m_binding)),
      m_source(other.m_source), m_target(other.m_target), m_claimed(std::exchange(other.m_claimed, false)) {}
KisCpuBackingHandoff &KisCpuBackingHandoff::operator=(KisCpuBackingHandoff &&other) noexcept
{
    if (this != &other) {
        reset();
        m_provider = std::move(other.m_provider); m_binding = std::move(other.m_binding);
        m_source = other.m_source; m_target = other.m_target;
        m_claimed = std::exchange(other.m_claimed, false);
    }
    return *this;
}
KisCpuBackingHandoff KisCpuBackingHandoff::prepare(
    const QSharedPointer<KisPageReplicaProvider> &provider, const KisReplicaHandle &source,
    const KisPageVersion &target, const KisPageAllocationDescriptor &descriptor)
{
    KisCpuBackingHandoff result;
    if (!provider) return result;
    auto binding = provider->cpuResidentBinding(source);
    if (!binding || !binding->prepareHandoff(source, target, descriptor, &result.m_target)) return result;
    result.m_provider = provider;
    result.m_binding = std::move(binding);
    result.m_source = source;
    return result;
}
bool KisCpuBackingHandoff::tryClaim()
{
    if (!m_binding || m_claimed) return false;
    m_claimed = m_binding->tryClaimHandoff(m_source);
    return m_claimed;
}
KisCpuWriteBindingReservation KisCpuBackingHandoff::commit() noexcept
{
    KisCpuWriteBindingReservation result;
    if (!m_binding || !m_claimed) return result;
    m_binding->finishHandoff(&m_target);
    result.m_binding = std::move(m_binding);
    m_claimed = false;
    return result;
}
void KisCpuBackingHandoff::reset() noexcept
{
    if (m_claimed) m_binding->finishHandoff(nullptr);
    m_claimed = false;
    m_binding.clear();
    m_provider.clear();
    m_source = {}; m_target = {};
}

bool KisCpuResidentProviderState::configure(
    const KisCpuResidentReplicaProviderConfig &requested,
    const QSharedPointer<KisCompletionRegistry> &registry,
    const QString &providerLabel, QString *error)
{
    const auto fail = [error, &providerLabel](const char *reason) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("%1 provider %2").arg(providerLabel, QString::fromLatin1(reason)));
        return false;
    };
    if (!requested.isValid() || !registry || !registry->isOperational())
        return fail("configuration is invalid");
    QMutexLocker locker(&mutex);
    if (config.isValid()) return fail("is already configured");
    const quint64 source = registry->registerSource(KisCompletionDomain::CpuJob);
    if (!source) return fail("completion source registration failed");
    config = requested;
    completions = registry;
    completionSource = source;
    KisPageStoreDetail::setError(error, {});
    return true;
}

const void *KisCpuResidentBinding::acquireRead(const KisReplicaAllocationIdentity &expected,
                                             bool residentOnly, KisCpuResidentReadStatus *status,
                                             bool waitForLocalGate)
{
    if (status) *status = KisCpuResidentReadStatus::Busy;
    // A native try must never wait behind a swap-in in the generic path.
    // Storage preparation can throw before pinning; always release this gate.
    std::unique_lock<QMutex> lock(m_mutex, std::defer_lock);
    if (residentOnly && !waitForLocalGate) {
        if (!lock.try_lock()) return nullptr;
    } else {
        lock.lock();
    }
    if (!expected.isValid() || !(m_handle.allocationIdentity() == expected)) {
        if (status) *status = KisCpuResidentReadStatus::InvalidIdentity;
        return nullptr;
    }
    if (m_retired || m_writeState != WriteState::Idle ||
        m_readers == std::numeric_limits<quint64>::max()) {
        if (status && m_retired) *status = KisCpuResidentReadStatus::Retired;
        return nullptr;
    }
    if (!m_readers) m_data = residentOnly && waitForLocalGate
        ? pinResidentStorageAfterGateWait(status) : pinStorage(residentOnly, status);
    const void *result = m_data;
    if (result) {
        ++m_readers;
        if (status) *status = KisCpuResidentReadStatus::Ready;
    }
    return result;
}

void KisCpuResidentBinding::releaseRead()
{
    QMutexLocker lock(&m_mutex);
    Q_ASSERT(m_readers);
    if (!m_readers) return;
    if (--m_readers == 0) {
        unpinStorage();
        m_data = nullptr;
        if (m_retired) releaseStorage();
        const auto readiness = std::move(m_retirementReadiness);
        lock.unlock();
        if (readiness) readiness->notify();
    }
}

void *KisCpuResidentBinding::acquireWrite(const KisReplicaAllocationIdentity &expected)
{
    QMutexLocker lock(&m_mutex);
    if (!expected.isValid() || !(m_handle.allocationIdentity() == expected) ||
        m_retired || m_writeState != WriteState::Idle || m_readers) return nullptr;
    m_data = pinStorage(false, nullptr);
    if (m_data) m_writeState = WriteState::Active;
    return m_data;
}

void KisCpuResidentBinding::releaseWriter(WriteState state)
{
    QMutexLocker lock(&m_mutex);
    Q_ASSERT(m_writeState == state);
    if (m_writeState != state) return;
    if (m_data) unpinStorage();
    m_data = nullptr;
    m_writeState = WriteState::Idle;
    if (m_retired) releaseStorage();
    const auto readiness = std::move(m_retirementReadiness);
    lock.unlock();
    if (readiness) readiness->notify();
}

void KisCpuResidentBinding::releaseWrite() { releaseWriter(WriteState::Active); }

KisPageReadinessStatus KisCpuResidentBinding::watchRetirementReadiness(
    const KisReplicaAllocationIdentity &expected, KisPageReadinessCallback scheduleReady,
    KisPageReadinessSubscription *subscription)
{
    if (!subscription || !scheduleReady) return KisPageReadinessStatus::Unavailable;
    subscription->reset();
    std::shared_ptr<KisPageReadinessSignal> preparedSignal;
    KisPageReadinessSubscription prepared;
    QMutexLocker lock(&m_mutex);
    if (m_retired || !(expected == m_handle.allocationIdentity()) || m_writeState == WriteState::Handoff)
        return KisPageReadinessStatus::Unavailable;
    if (!m_readers && m_writeState == WriteState::Idle) return KisPageReadinessStatus::Ready;
    preparedSignal = m_retirementReadiness;
    lock.unlock();
    try {
        const auto storage = scheduleReady.storageAllocator<std::byte>();
        prepared = KisPageReadinessSubscription(std::move(scheduleReady));
        if (!preparedSignal) preparedSignal = std::allocate_shared<KisPageReadinessSignal>(
            KisMutationStorageAllocator<KisPageReadinessSignal>(storage), storage);
    } catch (const std::bad_alloc &) { return KisPageReadinessStatus::Unavailable; }
    lock.relock();
    if (m_retired || !(expected == m_handle.allocationIdentity()) || m_writeState == WriteState::Handoff)
        return KisPageReadinessStatus::Unavailable;
    if (!m_readers && m_writeState == WriteState::Idle) return KisPageReadinessStatus::Ready;
    if (!m_retirementReadiness) {
        if (!preparedSignal) return KisPageReadinessStatus::Unavailable;
        m_retirementReadiness = std::move(preparedSignal);
    }
    if (!m_retirementReadiness->subscribeRetained(prepared)) return KisPageReadinessStatus::Unavailable;
    *subscription = std::move(prepared);
    return KisPageReadinessStatus::Waiting;
}

KisReplicaAccess KisCpuResidentBinding::acquireAccess(
    KisPageLeaseId lease, KisPageOperationId operation,
    const KisReplicaHandle &replica, KisPageAccessRequirement requirement,
    KisPageAccessMode mode)
{
    if (requirement.kind != KisPageAccessKind::CpuPointer ||
        replica.domain != requirement.domain) {
        return {};
    }
    const void *readData = mode == KisPageAccessMode::Read ? acquireRead(replica.allocationIdentity(), false) : nullptr;
    void *writeData = mode == KisPageAccessMode::Read ? nullptr : acquireWrite(replica.allocationIdentity());
    if (!readData && !writeData) return {};

    KisReplicaAccess access;
    access.lease = lease; access.operation = operation;
    access.replica = replica;
    access.mode = mode;
    access.cpuReadData = readData; access.cpuWriteData = writeData;
    return access;
}

bool KisCpuResidentBinding::reserveWrite(const KisReplicaAllocationIdentity &expected)
{
    QMutexLocker lock(&m_mutex);
    if (!expected.isValid() || !(m_handle.allocationIdentity() == expected) ||
        m_retired || m_writeState != WriteState::Idle || m_readers) return false;
    m_writeState = WriteState::Reserved;
    return true;
}

void *KisCpuResidentBinding::pinReservedWrite(bool residentOnly, KisCpuResidentReadStatus *status)
{
    QMutexLocker lock(&m_mutex);
    if (status) *status = KisCpuResidentReadStatus::InvalidIdentity;
    if (m_writeState != WriteState::Reserved || m_retired) {
        if (status && m_retired) *status = KisCpuResidentReadStatus::Retired;
        return nullptr;
    }
    if (!m_data) m_data = residentOnly
        ? pinResidentStorageAfterGateWait(status) : pinStorage(false, status);
    if (m_data && status) *status = KisCpuResidentReadStatus::Ready;
    return m_data;
}

void KisCpuResidentBinding::unpinReservedWrite()
{
    QMutexLocker lock(&m_mutex);
    Q_ASSERT(m_writeState == WriteState::Reserved);
    if (m_writeState != WriteState::Reserved || !m_data) return;
    unpinStorage();
    m_data = nullptr;
}

void KisCpuResidentBinding::releaseReservedWrite() { releaseWriter(WriteState::Reserved); }

KisCpuWriteBindingReservation::~KisCpuWriteBindingReservation() { reset(); }
KisCpuWriteBindingReservation &KisCpuWriteBindingReservation::operator=(KisCpuWriteBindingReservation &&other) noexcept
{
    if (this != &other) { reset(); m_binding = std::move(other.m_binding); }
    return *this;
}
KisCpuWriteBindingReservation KisCpuWriteBindingReservation::acquire(
    const QSharedPointer<KisCpuResidentBinding> &binding, const KisReplicaAllocationIdentity &expected)
{
    KisCpuWriteBindingReservation result;
    if (binding && binding->reserveWrite(expected)) result.m_binding = binding;
    return result;
}
void *KisCpuWriteBindingReservation::pinResident(KisCpuResidentReadStatus *status)
{
    if (status) *status = KisCpuResidentReadStatus::InvalidIdentity;
    return m_binding ? m_binding->pinReservedWrite(true, status) : nullptr;
}
void *KisCpuWriteBindingReservation::materialize()
{
    return m_binding ? m_binding->pinReservedWrite(false, nullptr) : nullptr;
}
void KisCpuWriteBindingReservation::unpin() { if (m_binding) m_binding->unpinReservedWrite(); }
void KisCpuWriteBindingReservation::reset()
{
    if (m_binding) m_binding->releaseReservedWrite();
    m_binding.clear();
}

bool KisCpuResidentBinding::retire(const KisReplicaAllocationIdentity &expected)
{
    QMutexLocker lock(&m_mutex);
    if (!expected.isValid() || !(m_handle.allocationIdentity() == expected) ||
        m_readers || m_writeState != WriteState::Idle || m_retired) return false;
    m_retired = true;
    releaseStorage();
    return true;
}

KisReplicaAllocationIdentity KisCpuResidentBinding::readIdentity(const KisPageVersion &version) const
{
    QMutexLocker lock(&m_mutex);
    return m_readers && !m_retired && m_handle.version == version
        ? m_handle.allocationIdentity() : KisReplicaAllocationIdentity{};
}

void KisCpuResidentBinding::revoke()
{
    QMutexLocker lock(&m_mutex);
    m_retired = true;
    if (!m_readers && m_writeState == WriteState::Idle) releaseStorage();
    const auto readiness = std::move(m_retirementReadiness);
    lock.unlock();
    if (readiness) readiness->notify();
}

KisReplicaAccess kisAcquireCpuBindingAccess(KisCpuBindingLeaseMap &activeLeases,
    const QSharedPointer<KisCpuResidentBinding> &binding, KisPageLeaseId lease, KisPageOperationId operation,
    const KisReplicaHandle &replica, KisPageAccessRequirement requirement, KisPageAccessMode mode)
{
    if (!binding || !lease.isValid() || !operation.isValid() ||
        activeLeases.find(lease.value) != activeLeases.end()) return {};
    try {
        // Allocate the exact original provider record before acquiring a pin.
        // Failed insertion preserves every existing lease; no QHash noexcept
        // insertion can terminate after the physical pin has already succeeded.
        activeLeases.emplace(lease.value, KisCpuBindingLease{replica.allocation, mode});
    } catch (const std::bad_alloc &) {
        return {};
    }
    auto removePrepared = qScopeGuard([&] { activeLeases.erase(lease.value); });
    KisReplicaAccess access = binding->acquireAccess(
        lease, operation, replica, requirement, mode);
    if (!access.isValid(requirement)) return {};
    removePrepared.dismiss();
    return access;
}

void kisReleaseCpuBindingAccess(KisCpuBindingLeaseMap &activeLeases,
    const QSharedPointer<KisCpuResidentBinding> &binding, const KisReplicaAccess &access)
{
    auto lease = activeLeases.find(access.lease.value);
    if (!binding || !access.isValid() || lease == activeLeases.end() ||
        !(lease->second.allocation == access.replica.allocation) || lease->second.mode != access.mode) {
        return;
    }
    if (lease->second.mode == KisPageAccessMode::Read) binding->releaseRead();
    else binding->releaseWrite();
    activeLeases.erase(lease);
}

KisReplicaOperation kisTransferCpuBinding(const KisReplicaTransferRequest &request,
    const QSharedPointer<KisCompletionRegistry> &completions, quint64 completionSource,
    const QSharedPointer<KisCpuResidentBinding> &source,
    const QSharedPointer<KisCpuResidentBinding> &target, const QString &providerLabel)
{
    const KisCompletionTicket completion = completions->allocatePending(completionSource);
    if (!completion.isValid()) {
        return KisReplicaOperation::failed(request.operation,
            QStringLiteral("%1 transfer completion allocation failed").arg(providerLabel));
    }
    const void *sourceData = source->acquireRead(request.source.allocationIdentity(), false);
    if (!sourceData) {
        completions->complete(completion, KisCompletionStatus::Failed);
        return KisReplicaOperation::failed(request.operation,
            QStringLiteral("%1 transfer source is busy").arg(providerLabel));
    }
    void *targetData = target->acquireWrite(request.target.allocationIdentity());
    if (!targetData) {
        source->releaseRead();
        completions->complete(completion, KisCompletionStatus::Failed);
        return KisReplicaOperation::failed(request.operation,
            QStringLiteral("%1 transfer target is pinned").arg(providerLabel));
    }
    std::memcpy(targetData, sourceData, size_t(request.target.layout.byteSize));
    target->releaseWrite();
    source->releaseRead();
    if (!completions->complete(completion, KisCompletionStatus::Succeeded)) {
        return KisReplicaOperation::failed(request.operation,
            QStringLiteral("%1 transfer completion publication failed").arg(providerLabel));
    }
    return {KisPageRequestStatus::Ready, request.operation, request.target, completion, {}};
}

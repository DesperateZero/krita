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
    Q_ASSERT(m_readers == 0 && m_writeState == WriteState::Idle && !m_storageBusy);
}

bool KisCpuResidentBinding::matchesHandle(const KisReplicaHandle &expected) const
{
    std::unique_lock<QMutex> lock(m_mutex);
    return !m_retired && matchesHandleLocked(expected);
}

KisReplicaHandle KisCpuResidentBinding::projectHandle(
    const KisPageVersion &version, quint64 incarnation) const
{
    // Only immutable physical fields are read; the caller supplies its own
    // issued logical facts. Projection neither observes nor authorizes access.
    return {m_slot.provider, m_slot.providerEpoch, {m_slot.slot, incarnation}, version, m_domain, m_layout};
}

bool KisCpuResidentBinding::matchesAllocation(KisReplicaAllocationToken expected) const
{
    std::unique_lock<QMutex> lock(m_mutex);
    return !m_retired && m_slot.slot == expected.slot && m_incarnation == expected.generation;
}

bool KisCpuResidentBinding::matchesReadIdentity(
    const KisReplicaAllocationIdentity &expected, const KisPageVersion &version) const
{
    std::unique_lock<QMutex> lock(m_mutex);
    return !m_retired && allocationIdentityLocked() == expected && matchesVersionLocked(version);
}

bool KisCpuResidentBinding::prepareHandoff(const KisPageWriteSource &source,
    const KisPageVersion &target, const KisPageAllocationDescriptor &descriptor,
    quint64 *incarnation) const
{
    std::unique_lock<QMutex> lock(m_mutex);
    // Advisory rejection only: readers may arrive after this mutex is released,
    // so tryClaimHandoff must still perform the final physical exclusion.
    if (!supportsBackingHandoff() || m_storageBusy || m_retired || !source.matches(allocationIdentityLocked(), m_domain, m_layout,
            {m_key, m_generation, m_defaultPixelRevision}) ||
        m_readers || m_writeState != WriteState::Idle ||
        source.domain() != KisPageAccessDomain::CpuRam || !source.layout().matches(descriptor) ||
        !target.isValid() || !(target.key == source.version.key) ||
        target.generation.value <= source.version.generation.value ||
        source.allocationIdentity().allocation.generation == std::numeric_limits<quint64>::max()) return false;
    *incarnation = m_incarnation;
    return true;
}

bool KisCpuResidentBinding::tryClaimHandoff(quint64 incarnation)
{
    std::unique_lock<QMutex> lock(m_mutex, std::try_to_lock);
    // Preparation checked the complete external source. Stable physical facts
    // cannot change; every version advance also advances this incarnation.
    if (!lock.owns_lock() || m_storageBusy || m_retired || m_incarnation != incarnation
        || m_readers || m_writeState != WriteState::Idle) return false;
    startStorageTransition();
    lock.unlock();
    void *data = tryClaimStorageForHandoff();
    lock.lock();
    if (data && m_retired) {
        lock.unlock();
        finishStorageHandoff(false);
        lock.lock();
        data = nullptr;
    }
    if (data) {
        m_data = data;
        m_writeState = WriteState::Handoff;
    }
    finishStorageTransition(lock);
    return data != nullptr;
}

void KisCpuResidentBinding::finishHandoff(const KisPageVersion *target) noexcept
{
    std::unique_lock<QMutex> lock(m_mutex);
    Q_ASSERT(m_writeState == WriteState::Handoff && m_data && !m_storageBusy);
    if (target) {
        // Complete the already issued exclusive capability. Revoke closes
        // future access but cannot steal this in-flight physical obligation.
        Q_ASSERT(m_incarnation != std::numeric_limits<quint64>::max());
        Q_ASSERT(target->key == m_key);
        m_generation = target->generation;
        m_defaultPixelRevision = target->defaultPixelRevision;
        ++m_incarnation;
    }
    startStorageTransition();
    lock.unlock();
    finishStorageHandoff(target != nullptr);
    lock.lock();
    m_writeState = target ? WriteState::Reserved : WriteState::Idle;
    if (!target) m_data = nullptr;
    finishStorageTransition(lock);
}

KisCpuBackingHandoff::~KisCpuBackingHandoff() { reset(); }
KisCpuBackingHandoff::KisCpuBackingHandoff(KisCpuBackingHandoff &&other) noexcept
    : m_provider(std::move(other.m_provider)), m_binding(std::move(other.m_binding)),
      m_incarnation(other.m_incarnation), m_targetVersion(other.m_targetVersion),
      m_claimed(std::exchange(other.m_claimed, false)) {}
KisCpuBackingHandoff &KisCpuBackingHandoff::operator=(KisCpuBackingHandoff &&other) noexcept
{
    if (this != &other) {
        reset();
        m_provider = std::move(other.m_provider); m_binding = std::move(other.m_binding);
        m_incarnation = other.m_incarnation; m_targetVersion = other.m_targetVersion;
        m_claimed = std::exchange(other.m_claimed, false);
    }
    return *this;
}
KisCpuBackingHandoff KisCpuBackingHandoff::prepare(
    const std::shared_ptr<KisPageReplicaProvider> &provider, const KisReplicaHandle &source,
    const KisPageVersion &target, const KisPageAllocationDescriptor &descriptor,
    std::shared_ptr<KisCpuResidentBinding> binding)
{
    return prepare(provider, KisPageWriteSource(source), target, descriptor, std::move(binding));
}
KisCpuBackingHandoff KisCpuBackingHandoff::prepare(
    const std::shared_ptr<KisPageReplicaProvider> &provider, const KisPageWriteSource &source,
    const KisPageVersion &target, const KisPageAllocationDescriptor &descriptor,
    std::shared_ptr<KisCpuResidentBinding> binding)
{
    KisCpuBackingHandoff result;
    if (!provider) return result;
    if (binding && source.binding() && binding != source.binding()) return result;
    if (!binding) binding = source.binding();
    if (!binding) binding = provider->cpuResidentBinding(source.handle()); // Actual unbound external input.
    if (!binding || !binding->prepareHandoff(source, target, descriptor, &result.m_incarnation)) return result;
    result.m_provider = provider;
    result.m_binding = std::move(binding);
    result.m_targetVersion = target;
    return result;
}

KisReplicaHandle KisCpuBackingHandoff::target() const
{
    return m_binding ? m_binding->projectHandle(m_targetVersion, m_incarnation + 1) : KisReplicaHandle{};
}
bool KisCpuBackingHandoff::tryClaim()
{
    if (!m_binding || m_claimed) return false;
    m_claimed = m_binding->tryClaimHandoff(m_incarnation);
    return m_claimed;
}
KisCpuWriteBindingReservation KisCpuBackingHandoff::commit() noexcept
{
    KisCpuWriteBindingReservation result;
    if (!m_binding || !m_claimed) return result;
    m_binding->finishHandoff(&m_targetVersion);
    result.m_binding = std::move(m_binding);
    m_claimed = false;
    return result;
}
void KisCpuBackingHandoff::reset() noexcept
{
    if (m_claimed) m_binding->finishHandoff(nullptr);
    m_claimed = false;
    m_binding.reset();
    m_provider.reset();
    m_incarnation = 0; m_targetVersion = {};
}

bool KisCpuResidentProviderState::configure(
    const KisCpuResidentReplicaProviderConfig &requested,
    const std::shared_ptr<KisCompletionRegistry> &registry,
    QString *error)
{
    const auto fail = [error](const QString &reason) {
        KisPageStoreDetail::setError(error, reason);
        return false;
    };
    if (!requested.isValid() || !registry || !registry->isOperational())
        return fail(QStringLiteral("provider configuration is invalid"));
    QMutexLocker locker(&mutex);
    if (config.isValid()) return fail(QStringLiteral("provider is already configured"));
    if (completions && completions != registry)
        return fail(QStringLiteral("provider configuration retry changed its completion registry"));
    if (!completionSource) {
        completionSource = registry->registerSource(KisCompletionDomain::CpuJob);
        if (completionSource) completions = registry;
    }
    if (!completionSource) return fail(QStringLiteral("provider completion source registration failed"));
    // The first slot is funded by source admission. Preparing Ready consumes
    // that original capacity without a separate storage request.
    const auto ready = registry->allocatePending(completionSource);
    if (!ready.isValid()) return fail(QStringLiteral("provider ready completion preparation failed"));
    registry->completePrepared(ready, KisCompletionStatus::Succeeded);
    config = requested;
    readyCompletion = ready;
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisCpuResidentBinding::awaitStorage(std::unique_lock<QMutex> &lock, bool wait)
{
    while (m_storageBusy) {
        if (!wait) return false;
        lock.unlock();
        m_storageGate.lock();
        m_storageGate.unlock();
        lock.lock();
    }
    return true;
}

void KisCpuResidentBinding::startStorageTransition()
{
    Q_ASSERT(!m_storageBusy);
    // Called under the state gate. An earlier physical owner releases the
    // storage gate before clearing the state gate. At this point only a cold
    // waiter can briefly own it; that waiter releases it before taking state.
    m_storageGate.lock();
    m_storageBusy = true;
}

void KisCpuResidentBinding::finishStorageTransition(std::unique_lock<QMutex> &lock) noexcept
{
    Q_ASSERT(lock.owns_lock() && m_storageBusy);
    if (m_retired && !m_readers && m_writeState == WriteState::Idle && !m_storageReleased) {
        m_storageReleased = true;
        lock.unlock();
        releaseStorage();
        lock.lock();
    }
    m_storageBusy = false;
    m_storageGate.unlock();
    auto readiness = m_retired || (!m_readers && m_writeState == WriteState::Idle)
        ? std::move(m_retirementReadiness) : std::shared_ptr<KisPageReadinessSignal>{};
    lock.unlock();
    if (readiness) readiness->notify();
}

void *KisCpuResidentBinding::pinStorageUnlocked(std::unique_lock<QMutex> &lock,
    bool residentOnly, bool waitedPin, KisCpuResidentReadStatus *status)
{
    Q_ASSERT(lock.owns_lock() && !m_storageBusy && !m_data && !m_retired);
    startStorageTransition();
    lock.unlock();
    void *data;
    try {
        data = residentOnly && waitedPin
            ? pinResidentStorageAfterGateWait(status) : pinStorage(residentOnly, status);
    } catch (...) {
        lock.lock();
        finishStorageTransition(lock);
        throw;
    }
    lock.lock();
    // No capability was delivered while physical preparation was in progress.
    // Revoke wins this race and the same transition returns the physical pin.
    if (m_retired) {
        if (data) {
            lock.unlock();
            unpinStorage();
            lock.lock();
        }
        data = nullptr;
        if (status) *status = KisCpuResidentReadStatus::Retired;
    }
    return data;
}

const void *KisCpuResidentBinding::acquireRead(const KisReplicaAllocationIdentity &expected,
    bool residentOnly, KisCpuResidentReadStatus *status, bool waitForLocalGate)
{
    if (status) *status = KisCpuResidentReadStatus::Busy;
    std::unique_lock<QMutex> lock(m_mutex, std::defer_lock);
    if (residentOnly && !waitForLocalGate) {
        if (!lock.try_lock()) return nullptr;
    } else lock.lock();
    if (!awaitStorage(lock, !residentOnly || waitForLocalGate)) return nullptr;
    if (!expected.isValid() || !(allocationIdentityLocked() == expected)) {
        if (status) *status = KisCpuResidentReadStatus::InvalidIdentity;
        return nullptr;
    }
    if (m_retired || m_writeState != WriteState::Idle ||
        m_readers == std::numeric_limits<quint64>::max()) {
        if (status && m_retired) *status = KisCpuResidentReadStatus::Retired;
        return nullptr;
    }
    if (m_readers) {
        ++m_readers;
        if (status) *status = KisCpuResidentReadStatus::Ready;
        return m_data;
    }
    void *result = pinStorageUnlocked(lock, residentOnly, waitForLocalGate, status);
    if (result) {
        m_data = result;
        m_readers = 1;
        if (status) *status = KisCpuResidentReadStatus::Ready;
    }
    finishStorageTransition(lock);
    return result;
}

void KisCpuResidentBinding::releaseRead()
{
    std::unique_lock<QMutex> lock(m_mutex);
    Q_ASSERT(m_readers && !m_storageBusy);
    if (!m_readers || --m_readers) return;
    m_data = nullptr;
    startStorageTransition();
    lock.unlock();
    unpinStorage();
    lock.lock();
    finishStorageTransition(lock);
}

void *KisCpuResidentBinding::acquireWrite(const KisReplicaAllocationIdentity &expected)
{
    std::unique_lock<QMutex> lock(m_mutex);
    awaitStorage(lock, true);
    if (!expected.isValid() || !(allocationIdentityLocked() == expected) ||
        m_retired || m_writeState != WriteState::Idle || m_readers) return nullptr;
    void *data = pinStorageUnlocked(lock, false, false, nullptr);
    if (data) {
        m_data = data;
        m_writeState = WriteState::Active;
    }
    finishStorageTransition(lock);
    return data;
}

void KisCpuResidentBinding::releaseWriter(WriteState state)
{
    std::unique_lock<QMutex> lock(m_mutex);
    Q_ASSERT(m_writeState == state && !m_storageBusy);
    if (m_writeState != state) return;
    const bool pinned = std::exchange(m_data, nullptr) != nullptr;
    m_writeState = WriteState::Idle;
    if (!pinned && !m_retired) {
        // Ending an unpinned reservation changes only permission. It owns no
        // physical transition and must not enter the storage wait protocol.
        auto readiness = std::move(m_retirementReadiness);
        lock.unlock();
        if (readiness) readiness->notify();
        return;
    }
    startStorageTransition();
    lock.unlock();
    if (pinned) unpinStorage();
    lock.lock();
    finishStorageTransition(lock);
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
    std::unique_lock<QMutex> lock(m_mutex);
    if (m_retired || !(expected == allocationIdentityLocked()) || m_writeState == WriteState::Handoff)
        return KisPageReadinessStatus::Unavailable;
    if (!m_storageBusy && !m_readers && m_writeState == WriteState::Idle) return KisPageReadinessStatus::Ready;
    preparedSignal = m_retirementReadiness;
    lock.unlock();
    try {
        const auto storage = scheduleReady.storageAllocator<std::byte>();
        prepared = KisPageReadinessSubscription(std::move(scheduleReady));
        if (!preparedSignal) preparedSignal = std::allocate_shared<KisPageReadinessSignal>(
            KisMutationStorageAllocator<KisPageReadinessSignal>(storage), storage);
    } catch (const std::bad_alloc &) { return KisPageReadinessStatus::Unavailable; }
    lock.lock();
    if (m_retired || !(expected == allocationIdentityLocked()) || m_writeState == WriteState::Handoff)
        return KisPageReadinessStatus::Unavailable;
    if (!m_storageBusy && !m_readers && m_writeState == WriteState::Idle) return KisPageReadinessStatus::Ready;
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
    std::unique_lock<QMutex> lock(m_mutex);
    if (!expected.isValid() || !(allocationIdentityLocked() == expected) ||
        m_retired || m_storageBusy || m_writeState != WriteState::Idle || m_readers) return false;
    m_writeState = WriteState::Reserved;
    return true;
}

void *KisCpuResidentBinding::pinReservedWrite(bool residentOnly, KisCpuResidentReadStatus *status)
{
    std::unique_lock<QMutex> lock(m_mutex);
    if (status) *status = KisCpuResidentReadStatus::InvalidIdentity;
    Q_ASSERT(!m_storageBusy);
    if (m_writeState != WriteState::Reserved) return nullptr;
    // An already pinned writer survives revoke until its original release.
    if (m_data) {
        if (status) *status = KisCpuResidentReadStatus::Ready;
        return m_data;
    }
    if (m_retired) {
        if (status) *status = KisCpuResidentReadStatus::Retired;
        return nullptr;
    }
    void *data = pinStorageUnlocked(lock, residentOnly, residentOnly, status);
    if (data) {
        m_data = data;
        if (status) *status = KisCpuResidentReadStatus::Ready;
    }
    finishStorageTransition(lock);
    return data;
}

void KisCpuResidentBinding::unpinReservedWrite()
{
    std::unique_lock<QMutex> lock(m_mutex);
    Q_ASSERT(m_writeState == WriteState::Reserved && !m_storageBusy);
    if (m_writeState != WriteState::Reserved || !m_data) return;
    m_data = nullptr;
    startStorageTransition();
    lock.unlock();
    unpinStorage();
    lock.lock();
    finishStorageTransition(lock);
}

void KisCpuResidentBinding::releaseReservedWrite() { releaseWriter(WriteState::Reserved); }

KisCpuWriteBindingReservation::~KisCpuWriteBindingReservation() { reset(); }
KisCpuWriteBindingReservation &KisCpuWriteBindingReservation::operator=(KisCpuWriteBindingReservation &&other) noexcept
{
    if (this != &other) { reset(); m_binding = std::move(other.m_binding); }
    return *this;
}
KisCpuWriteBindingReservation KisCpuWriteBindingReservation::acquire(
    const std::shared_ptr<KisCpuResidentBinding> &binding, const KisReplicaAllocationIdentity &expected)
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
    m_binding.reset();
}

bool KisCpuResidentBinding::retire(const KisReplicaAllocationIdentity &expected)
{
    std::unique_lock<QMutex> lock(m_mutex);
    if (!expected.isValid() || !(allocationIdentityLocked() == expected) || m_storageBusy ||
        m_readers || m_writeState != WriteState::Idle || m_retired) return false;
    m_retired = true;
    startStorageTransition();
    finishStorageTransition(lock);
    return true;
}

KisReplicaAllocationIdentity KisCpuResidentBinding::readIdentity(const KisPageVersion &version) const
{
    std::unique_lock<QMutex> lock(m_mutex);
    return m_readers && !m_retired && matchesVersionLocked(version)
        ? allocationIdentityLocked() : KisReplicaAllocationIdentity{};
}

void KisCpuResidentBinding::revoke()
{
    std::unique_lock<QMutex> lock(m_mutex);
    m_retired = true;
    if (!m_storageBusy && !m_readers && m_writeState == WriteState::Idle && !m_storageReleased) {
        startStorageTransition();
        finishStorageTransition(lock);
        return;
    }
    // The active pin/transition keeps the one release obligation. No second
    // free, wait for physical I/O, or callback is performed under this gate.
    auto readiness = std::move(m_retirementReadiness);
    lock.unlock();
    if (readiness) readiness->notify();
}

KisReplicaAccess kisAcquireCpuBindingAccess(KisCpuBindingLeaseMap &activeLeases,
    const std::shared_ptr<KisCpuResidentBinding> &binding, KisPageLeaseId lease, KisPageOperationId operation,
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
    const std::shared_ptr<KisCpuResidentBinding> &binding, const KisReplicaAccess &access)
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
    const KisCompletionTicket &readyCompletion,
    const std::shared_ptr<KisCpuResidentBinding> &source,
    const std::shared_ptr<KisCpuResidentBinding> &target)
{
    if (!readyCompletion.isValid()) {
        return KisReplicaOperation::failed(request.operation,
            QStringLiteral("transfer ready completion is unavailable"));
    }
    const void *sourceData = source->acquireRead(request.source.allocationIdentity(), false);
    if (!sourceData) {
        return KisReplicaOperation::failed(request.operation,
            QStringLiteral("transfer source is busy"));
    }
    auto releaseSource = qScopeGuard([&] { source->releaseRead(); });
    void *targetData = target->acquireWrite(request.target.allocationIdentity());
    if (!targetData) {
        return KisReplicaOperation::failed(request.operation,
            QStringLiteral("transfer target is pinned"));
    }
    std::memcpy(targetData, sourceData, size_t(request.target.layout.byteSize));
    target->releaseWrite();
    source->releaseRead();
    releaseSource.dismiss();
    return {KisPageRequestStatus::Ready, request.operation, request.target, readyCompletion, {}};
}

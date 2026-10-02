/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_CPU_RESIDENT_BINDING_P_H
#define KIS_CPU_RESIDENT_BINDING_P_H

#include <QMutex>
#include <bitset>
#include <map>
#include <mutex>
#include <utility>
#include <unordered_map>
#include "KisCompletionRegistry.h"
#include "KisPageReplicaProvider.h"

class KisCpuWriteBindingReservation;
class KisCpuBackingHandoff;

/**
 * Provider-owned, stable allocation record. Not a consumer authorization: a
 * PageStore guard must also hold an exact captured root. Generic and native
 * access, transfer and retirement use this same page-local gate.
 */
class KRITAIMAGE_EXPORT KisCpuResidentBinding
{
public:
    virtual ~KisCpuResidentBinding();
    KisCpuResidentBinding(const KisCpuResidentBinding &) = delete;
    KisCpuResidentBinding &operator=(const KisCpuResidentBinding &) = delete;

    const void *acquireRead(const KisReplicaAllocationIdentity &expected,
                            bool residentOnly, KisCpuResidentReadStatus *status = nullptr,
                            bool waitForLocalGate = false);
    void releaseRead();
    void *acquireWrite(const KisReplicaAllocationIdentity &expected);
    void releaseWrite();
    // Cold retirement waiter. The callback only dispatches work; it must not
    // enter provider/owner gates (a generic release can hold an outer gate).
    KisPageReadinessStatus watchRetirementReadiness(
        const KisReplicaAllocationIdentity &expected, KisPageReadinessCallback scheduleReady,
        KisPageReadinessSubscription *subscription);
    KisReplicaAccess acquireAccess(KisPageLeaseId lease, KisPageOperationId operation,
                                   const KisReplicaHandle &replica,
                                   KisPageAccessRequirement requirement, KisPageAccessMode mode);
    bool retire(const KisReplicaAllocationIdentity &expected);
    // Provider destruction closes new access; existing native holders keep
    // their storage until their final unpin. It does not grant new access.
    void revoke();
    bool matchesHandle(const KisReplicaHandle &expected) const;
    bool matchesAllocation(KisReplicaAllocationToken expected) const;
    // Selection hint only. A matching identity still needs a physical pin.
    bool matchesReadIdentity(const KisReplicaAllocationIdentity &expected,
                             const KisPageVersion &version) const;

protected:
    explicit KisCpuResidentBinding(const KisReplicaHandle &handle)
        : m_handle(handle) { Q_ASSERT(handle.isValid()); }
    virtual void *pinStorage(bool residentOnly, KisCpuResidentReadStatus *status) = 0;
    virtual void *pinResidentStorageAfterGateWait(KisCpuResidentReadStatus *status)
    { return pinStorage(true, status); }
    virtual void unpinStorage() = 0;
    virtual void releaseStorage() = 0;
    virtual bool supportsBackingHandoff() const { return false; }
    // Nonblocking physical exclusion, not a normal resident read pin. The
    // matching finish either cancels or converts to the ordinary writer pin.
    virtual void *tryClaimStorageForHandoff() { return nullptr; }
    virtual void finishStorageHandoff(bool writable) noexcept { Q_UNUSED(writable); }

private:
    friend class KisCpuWriteBindingReservation;
    friend class KisCpuBackingHandoff;
    friend class KisTiles3PageReplicaProvider;
    // Only exported through an already active owner-issued read guard.
    KisReplicaAllocationIdentity readIdentity(const KisPageVersion &version) const;
    enum class WriteState : quint8 { Idle, Active, Reserved, Handoff };
    bool prepareHandoff(const KisReplicaHandle &source, const KisPageVersion &target,
                         const KisPageAllocationDescriptor &descriptor, KisReplicaHandle *result) const;
    bool tryClaimHandoff(const KisReplicaHandle &source);
    void finishHandoff(const KisReplicaHandle *target) noexcept;
    bool reserveWrite(const KisReplicaAllocationIdentity &expected);
    void releaseWriter(WriteState state);
    void *pinReservedWrite(bool residentOnly, KisCpuResidentReadStatus *status);
    void unpinReservedWrite();
    void releaseReservedWrite();
    mutable QMutex m_mutex;
    quint64 m_readers = 0;
    WriteState m_writeState = WriteState::Idle;
    bool m_retired = false;
    void *m_data = nullptr;
    // Keep the hot gate/pin state next to the provider-minted identity at the
    // start of the handle. The stable binding is the single allocation record;
    // slot indices retain only it, so retag has no second handle to update.
    KisReplicaHandle m_handle;
    std::shared_ptr<KisPageReadinessSignal> m_retirementReadiness;
};

struct KisCpuBindingLease
{
    KisReplicaAllocationToken allocation;
    KisPageAccessMode mode;
};

using KisCpuBindingLeaseMap = std::unordered_map<quint64, KisCpuBindingLease,
    std::hash<quint64>, std::equal_to<quint64>,
    KisMutationStorageAllocator<std::pair<const quint64, KisCpuBindingLease>>>;

struct KRITAIMAGE_EXPORT KisCpuResidentProviderState
{
    static constexpr quint64 OperationReplayWindow = 4096;

    bool configure(const KisCpuResidentReplicaProviderConfig &requested,
                   const QSharedPointer<KisCompletionRegistry> &registry,
                   const QString &providerLabel, QString *error);
    bool operationAvailable(KisPageOperationId operation) const
    {
        if (!operation.isValid()) return false;
        if (operation.value > consumedOperationHighWater) return true;
        const quint64 age = consumedOperationHighWater - operation.value;
        return age < OperationReplayWindow
            && !consumedOperations.test(size_t(operation.value % OperationReplayWindow));
    }
    void consumeOperation(KisPageOperationId operation)
    {
        Q_ASSERT(operationAvailable(operation));
        if (operation.value > consumedOperationHighWater) {
            const quint64 advance = operation.value - consumedOperationHighWater;
            if (advance >= OperationReplayWindow) {
                consumedOperations.reset();
            } else {
                for (quint64 offset = 1; offset <= advance; ++offset) {
                    consumedOperations.reset(size_t(
                        (consumedOperationHighWater + offset) % OperationReplayWindow));
                }
            }
            consumedOperationHighWater = operation.value;
        }
        consumedOperations.set(size_t(operation.value % OperationReplayWindow));
    }
    KisReplicaProviderId providerId() const { std::lock_guard<QMutex> lock(mutex); return config.provider; }
    KisReplicaProviderEpoch providerEpoch() const { std::lock_guard<QMutex> lock(mutex); return config.providerEpoch; }

    KisCpuResidentReplicaProviderConfig config;
    mutable QMutex mutex;
    QSharedPointer<KisCompletionRegistry> completions;
    quint64 completionSource = 0;
    quint64 nextSlot = 1;
    quint64 committedBytes = 0;
    KisCpuBindingLeaseMap activeLeases;
    // Operation ids are globally monotonic but provider calls may arrive out
    // of order. Keep a fixed replay window and fail closed for older ids;
    // replay protection must not become a session-lifetime QSet.
    std::bitset<OperationReplayWindow> consumedOperations;
    quint64 consumedOperationHighWater = 0;
};

KRITAIMAGE_EXPORT KisReplicaAccess kisAcquireCpuBindingAccess(KisCpuBindingLeaseMap &activeLeases,
    const QSharedPointer<KisCpuResidentBinding> &binding, KisPageLeaseId lease, KisPageOperationId operation,
    const KisReplicaHandle &replica, KisPageAccessRequirement requirement, KisPageAccessMode mode);
KRITAIMAGE_EXPORT void kisReleaseCpuBindingAccess(KisCpuBindingLeaseMap &activeLeases,
    const QSharedPointer<KisCpuResidentBinding> &binding, const KisReplicaAccess &access);
KRITAIMAGE_EXPORT KisReplicaOperation kisTransferCpuBinding(const KisReplicaTransferRequest &request,
    const QSharedPointer<KisCompletionRegistry> &completions, quint64 completionSource,
    const QSharedPointer<KisCpuResidentBinding> &source,
    const QSharedPointer<KisCpuResidentBinding> &target, const QString &providerLabel);

template<typename Allocation>
struct KisCpuResidentAllocationIndex : KisCpuResidentProviderState
{
    using AllocationMap = std::map<quint64, Allocation, std::less<quint64>,
        KisMutationStorageAllocator<std::pair<const quint64, Allocation>>>;
    using Iterator = typename AllocationMap::iterator;
    struct Retirement {
        Iterator allocation;
        KisCompletionTicket completion;
        const char *failure = nullptr;
    };

    bool owns(const KisReplicaHandle &handle) const
    {
        return handle.isValid() && handle.provider == config.provider &&
               handle.providerEpoch == config.providerEpoch &&
               handle.domain == KisPageAccessDomain::CpuRam;
    }
    Iterator findExactAllocation(const KisReplicaHandle &handle)
    {
        auto found = allocations.find(handle.allocation.slot);
        return owns(handle) && found != allocations.end() && found->second.binding && found->second.binding->matchesHandle(handle)
            ? found : allocations.end();
    }

    void revokeBindings()
    {
        std::lock_guard<QMutex> locker(mutex);
        for (const auto &[id, lease] : std::as_const(activeLeases)) {
            const auto found = allocations.find(lease.allocation.slot);
            if (found == allocations.end() ||
                !found->second.binding || !found->second.binding->matchesAllocation(lease.allocation)) continue;
            if (lease.mode == KisPageAccessMode::Read) found->second.binding->releaseRead();
            else found->second.binding->releaseWrite();
        }
        for (const auto &[slot, allocation] : std::as_const(allocations)) {
            if (allocation.binding) allocation.binding->revoke();
            if (allocation.retirementCompletion.isValid())
                completions->completePrepared(allocation.retirementCompletion, KisCompletionStatus::Cancelled);
        }
    }

    KisReplicaAccess resolveAccess(KisPageLeaseId lease, KisPageOperationId operation,
                                   const KisReplicaHandle &replica, KisPageAccessRequirement requirement,
                                   KisPageAccessMode mode)
    {
        std::lock_guard<QMutex> locker(mutex);
        const auto allocation = findExactAllocation(replica);
        return allocation == allocations.end() ? KisReplicaAccess{} : kisAcquireCpuBindingAccess(
            activeLeases, allocation->second.binding, lease, operation, replica, requirement, mode);
    }

    void releaseAccess(const KisReplicaAccess &access)
    {
        std::lock_guard<QMutex> locker(mutex);
        const auto allocation = findExactAllocation(access.replica);
        if (allocation != allocations.end()) kisReleaseCpuBindingAccess(
            activeLeases, allocation->second.binding, access);
    }

    QSharedPointer<KisCpuResidentBinding> binding(
        const KisReplicaHandle &handle, KisCpuResidentReadStatus *status)
    {
        std::lock_guard<QMutex> locker(mutex);
        const auto found = findExactAllocation(handle);
        if (status) *status = found == allocations.end()
            ? KisCpuResidentReadStatus::InvalidIdentity : KisCpuResidentReadStatus::Ready;
        return found == allocations.end()
            ? QSharedPointer<KisCpuResidentBinding>{} : found->second.binding;
    }

    KisReplicaOperation transfer(const KisReplicaTransferRequest &request,
                                 const QString &providerLabel)
    {
        const auto fail = [&request, &providerLabel](const char *reason) {
            return KisReplicaOperation::failed(request.operation,
                QStringLiteral("%1 transfer %2").arg(providerLabel, QString::fromLatin1(reason)));
        };
        if (!request.isSameProviderTransfer() || !owns(request.source) ||
            !owns(request.target) || !operationAvailable(request.operation))
            return fail("request is invalid");
        consumeOperation(request.operation);
        const auto source = findExactAllocation(request.source);
        const auto target = findExactAllocation(request.target);
        if (source == allocations.end() || target == allocations.end() ||
            !request.source.layout.matches(request.descriptor) ||
            !request.target.layout.matches(request.descriptor) ||
            !source->second.binding || !target->second.binding) return fail("allocation is stale");
        return kisTransferCpuBinding(request, completions, completionSource,
                                     source->second.binding, target->second.binding, providerLabel);
    }

    Retirement beginRetirement(KisPageOperationId operation,
                               const KisReplicaHandle &replica,
                               const KisCompletionTicket &lastUse)
    {
        if (!owns(replica) || !operationAvailable(operation))
            return {allocations.end(), {}, "request is invalid"};
        if (lastUse.isValid()) {
            const auto status = completions->status(lastUse);
            if (status == KisCompletionStatus::Unknown || status == KisCompletionStatus::Pending)
                return {allocations.end(), {}, "last use is incomplete"};
        }
        const auto allocation = findExactAllocation(replica);
        if (allocation == allocations.end())
            return {allocation, {}, "allocation is stale"};
        // Cold allocation prepared this terminal capacity before physical
        // adoption. A refusal to retire keeps the same private ticket Pending.
        const auto completion = allocation->second.retirementCompletion;
        if (!completion.isValid()) qFatal("Live allocation has no prepared retirement completion");
        return {allocation, completion};
    }

    AllocationMap allocations;
};

/** Move-only page-local writer reservation, independent of RAM residency.
 * Only the owner of an unpublished independent backing may use this token.
 * It is NOT a physical exclusivity claim or a logical in-place permit.
 * While parked, competing binding readers/writers/retirement remain blocked,
 * but tiles3 may swap the bytes. No raw pointer survives unpin().
 */
class KRITAIMAGE_EXPORT KisCpuWriteBindingReservation
{
public:
    KisCpuWriteBindingReservation() = default;
    ~KisCpuWriteBindingReservation();
    KisCpuWriteBindingReservation(KisCpuWriteBindingReservation &&) noexcept = default;
    KisCpuWriteBindingReservation &operator=(KisCpuWriteBindingReservation &&) noexcept;
    KisCpuWriteBindingReservation(const KisCpuWriteBindingReservation &) = delete;
    KisCpuWriteBindingReservation &operator=(const KisCpuWriteBindingReservation &) = delete;
    static KisCpuWriteBindingReservation acquire(const QSharedPointer<KisCpuResidentBinding> &binding,
                                                 const KisReplicaAllocationIdentity &expected);
    bool isValid() const { return !m_binding.isNull(); }
    // Waits only for the local gate/swap barrier, never initiates swap-in.
    void *pinResident(KisCpuResidentReadStatus *status = nullptr);
    // Explicit cold control path; the caller must first observe NonResident.
    // Current tiles3 storage restores synchronously, outside the owner gate.
    void *materialize();
    void unpin();
    void reset();
private:
    friend class KisCpuBackingHandoff;
    QSharedPointer<KisCpuResidentBinding> m_binding;
};

/** Prepared physical transfer only; no logical write or recovery permission.
 * prepare() retains the provider and mints the target before metadata/storage
 * preparation. tryClaim() excludes physical consumers without exposing bytes.
 * After logical installation, commit() retags once and returns an already
 * pinned writer reservation. No allocation or provider-index edit is needed.
 * Claimed tokens are thread-affine and short lived; all other preparation
 * must precede tryClaim(). Destroy/reset the consumed token outside owner
 * gates: it keeps the provider alive even after commit, deferring destruction.
 */
class KRITAIMAGE_EXPORT KisCpuBackingHandoff
{
public:
    KisCpuBackingHandoff() = default;
    ~KisCpuBackingHandoff();
    KisCpuBackingHandoff(KisCpuBackingHandoff &&other) noexcept;
    KisCpuBackingHandoff &operator=(KisCpuBackingHandoff &&other) noexcept;
    KisCpuBackingHandoff(const KisCpuBackingHandoff &) = delete;
    KisCpuBackingHandoff &operator=(const KisCpuBackingHandoff &) = delete;
    static KisCpuBackingHandoff prepare(const QSharedPointer<KisPageReplicaProvider> &provider,
                                        const KisReplicaHandle &source, const KisPageVersion &target,
                                        const KisPageAllocationDescriptor &descriptor);
    bool isValid() const { return !m_binding.isNull(); }
    bool isClaimed() const { return m_claimed; }
    const KisReplicaHandle &target() const { return m_target; }
    bool tryClaim();
    KisCpuWriteBindingReservation commit() noexcept;
    void reset() noexcept;
private:
    QSharedPointer<KisPageReplicaProvider> m_provider;
    QSharedPointer<KisCpuResidentBinding> m_binding;
    KisReplicaHandle m_source;
    KisReplicaHandle m_target;
    bool m_claimed = false;
};

// Metadata-owned immutable candidate; provider lookup is done once, outside
// owner/shard locks. Provider allocation nodes stay stable until retirement.
class KisCpuReadBindingLink
{
public:
    KisCpuReadBindingLink(const KisReplicaHandle &handle,
                         const QSharedPointer<KisPageReplicaProvider> &provider)
        : replica(handle), m_provider(provider) {}
    QSharedPointer<KisCpuResidentBinding> resolve(KisCpuResidentReadStatus *status = nullptr) const
    {
        std::call_once(m_once, [this] {
            auto candidate = m_provider->cpuResidentBinding(replica, &m_status);
            if (candidate) {
                m_binding = std::move(candidate);
                m_status = KisCpuResidentReadStatus::Ready;
            } else if (m_status == KisCpuResidentReadStatus::Ready) {
                m_status = KisCpuResidentReadStatus::InvalidIdentity;
            }
        });
        if (status) *status = m_status;
        return m_binding;
    }
    const KisReplicaHandle replica;
private:
    QSharedPointer<KisPageReplicaProvider> m_provider;
    mutable std::once_flag m_once;
    mutable QSharedPointer<KisCpuResidentBinding> m_binding;
    mutable KisCpuResidentReadStatus m_status = KisCpuResidentReadStatus::BindingUnavailable;
};

#endif

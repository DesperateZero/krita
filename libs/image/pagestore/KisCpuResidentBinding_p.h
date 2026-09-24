/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_CPU_RESIDENT_BINDING_P_H
#define KIS_CPU_RESIDENT_BINDING_P_H

#include <QHash>
#include <QMutex>
#include <bitset>
#include <mutex>
#include <utility>
#include "KisCompletionRegistry.h"
#include "KisPageReplicaProvider.h"

class KisCpuWriteBindingReservation;

/**
 * Provider-owned, stable allocation record. Not a consumer authorization: a
 * PageStore guard must also hold an exact captured root. Generic and native
 * access, transfer and retirement use this same page-local gate.
 */
class KRITAIMAGE_EXPORT KisCpuResidentBinding
{
public:
    KisCpuResidentBinding() = default;
    virtual ~KisCpuResidentBinding();
    KisCpuResidentBinding(const KisCpuResidentBinding &) = delete;
    KisCpuResidentBinding &operator=(const KisCpuResidentBinding &) = delete;

    const void *acquireRead(bool residentOnly, KisCpuResidentReadStatus *status = nullptr,
                            bool waitForLocalGate = false);
    void releaseRead();
    void *acquireWrite();
    void releaseWrite();
    KisReplicaAccess acquireAccess(KisPageLeaseId lease, KisPageOperationId operation,
                                   const KisReplicaHandle &replica,
                                   KisPageAccessRequirement requirement, KisPageAccessMode mode);
    bool retire();
    // Provider destruction closes new access; existing native holders keep
    // their storage until their final unpin. It does not grant new access.
    void revoke();

protected:
    virtual void *pinStorage(bool residentOnly, KisCpuResidentReadStatus *status) = 0;
    virtual void *pinResidentStorageAfterGateWait(KisCpuResidentReadStatus *status)
    { return pinStorage(true, status); }
    virtual void unpinStorage() = 0;
    virtual void releaseStorage() = 0;

private:
    friend class KisCpuWriteBindingReservation;
    enum class WriteState : quint8 { Idle, Active, Reserved };
    bool reserveWrite();
    void releaseWriter(WriteState state);
    void *pinReservedWrite(bool residentOnly, KisCpuResidentReadStatus *status);
    void unpinReservedWrite();
    void releaseReservedWrite();
    QMutex m_mutex;
    quint64 m_readers = 0;
    WriteState m_writeState = WriteState::Idle;
    bool m_retired = false;
    void *m_data = nullptr;
};

struct KisCpuBindingLease
{
    KisReplicaAllocationToken allocation;
    KisPageAccessMode mode;
};

struct KisCpuResidentProviderState
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
    QHash<quint64, KisCpuBindingLease> activeLeases;
    // Operation ids are globally monotonic but provider calls may arrive out
    // of order. Keep a fixed replay window and fail closed for older ids;
    // replay protection must not become a session-lifetime QSet.
    std::bitset<OperationReplayWindow> consumedOperations;
    quint64 consumedOperationHighWater = 0;
};

KisReplicaAccess kisAcquireCpuBindingAccess(QHash<quint64, KisCpuBindingLease> &activeLeases,
    const QSharedPointer<KisCpuResidentBinding> &binding, KisPageLeaseId lease, KisPageOperationId operation,
    const KisReplicaHandle &replica, KisPageAccessRequirement requirement, KisPageAccessMode mode);
void kisReleaseCpuBindingAccess(QHash<quint64, KisCpuBindingLease> &activeLeases,
    const QSharedPointer<KisCpuResidentBinding> &binding, const KisReplicaAccess &access);
KisReplicaOperation kisTransferCpuBinding(const KisReplicaTransferRequest &request,
    const QSharedPointer<KisCompletionRegistry> &completions, quint64 completionSource,
    const QSharedPointer<KisCpuResidentBinding> &source,
    const QSharedPointer<KisCpuResidentBinding> &target, const QString &providerLabel);

template<typename Allocation>
struct KisCpuResidentAllocationIndex : KisCpuResidentProviderState
{
    using Iterator = typename QHash<quint64, Allocation>::iterator;
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
    typename QHash<quint64, Allocation>::iterator findExactAllocation(const KisReplicaHandle &handle)
    {
        auto found = allocations.find(handle.allocation.slot);
        return owns(handle) && found != allocations.end() && found->handle == handle
            ? found : allocations.end();
    }

    void revokeBindings()
    {
        std::lock_guard<QMutex> locker(mutex);
        for (const auto &lease : std::as_const(activeLeases)) {
            const auto found = allocations.find(lease.allocation.slot);
            if (found == allocations.end() ||
                !(found->handle.allocation == lease.allocation) || !found->binding) continue;
            if (lease.mode == KisPageAccessMode::Read) found->binding->releaseRead();
            else found->binding->releaseWrite();
        }
        for (const auto &allocation : std::as_const(allocations))
            if (allocation.binding) allocation.binding->revoke();
    }

    KisReplicaAccess resolveAccess(KisPageLeaseId lease, KisPageOperationId operation,
                                   const KisReplicaHandle &replica, KisPageAccessRequirement requirement,
                                   KisPageAccessMode mode)
    {
        std::lock_guard<QMutex> locker(mutex);
        const auto allocation = findExactAllocation(replica);
        return allocation == allocations.end() ? KisReplicaAccess{} : kisAcquireCpuBindingAccess(
            activeLeases, allocation->binding, lease, operation, replica, requirement, mode);
    }

    void releaseAccess(const KisReplicaAccess &access)
    {
        std::lock_guard<QMutex> locker(mutex);
        const auto allocation = findExactAllocation(access.replica);
        if (allocation != allocations.end()) kisReleaseCpuBindingAccess(
            activeLeases, allocation->binding, access);
    }

    QSharedPointer<KisCpuResidentBinding> binding(
        const KisReplicaHandle &handle, KisCpuResidentReadStatus *status)
    {
        std::lock_guard<QMutex> locker(mutex);
        const auto found = findExactAllocation(handle);
        if (status) *status = found == allocations.end()
            ? KisCpuResidentReadStatus::InvalidIdentity : KisCpuResidentReadStatus::Ready;
        return found == allocations.end()
            ? QSharedPointer<KisCpuResidentBinding>{} : found->binding;
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
            !source->handle.layout.matches(request.descriptor) ||
            !target->handle.layout.matches(request.descriptor) ||
            !source->binding || !target->binding) return fail("allocation is stale");
        return kisTransferCpuBinding(request, completions, completionSource,
                                     source->binding, target->binding, providerLabel);
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
        const auto completion = completions->allocatePending(completionSource);
        return completion.isValid() ? Retirement{allocation, completion}
                                    : Retirement{allocation, {}, "completion allocation failed"};
    }

    QHash<quint64, Allocation> allocations;
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
    static KisCpuWriteBindingReservation acquire(const QSharedPointer<KisCpuResidentBinding> &binding);
    bool isValid() const { return !m_binding.isNull(); }
    // Waits only for the local gate/swap barrier, never initiates swap-in.
    void *pinResident(KisCpuResidentReadStatus *status = nullptr);
    // Explicit cold control path; the caller must first observe NonResident.
    // Current tiles3 storage restores synchronously, outside the owner gate.
    void *materialize();
    void unpin();
    void reset();
private:
    QSharedPointer<KisCpuResidentBinding> m_binding;
};

// Metadata-owned immutable candidate; provider lookup is done once, outside
// owner/shard locks. The allocation record itself is stable across QHash moves.
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

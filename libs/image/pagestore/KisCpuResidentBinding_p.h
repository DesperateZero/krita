/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_CPU_RESIDENT_BINDING_P_H
#define KIS_CPU_RESIDENT_BINDING_P_H

#include <QMutex>
#include <bitset>
#include <boost/intrusive/set.hpp>
#include <mutex>
#include <utility>
#include <unordered_map>
#include <variant>
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
        : m_slot(handle.physicalSlotIdentity()), m_incarnation(handle.allocation.generation),
          m_key(handle.version.key), m_generation(handle.version.generation),
          m_defaultPixelRevision(handle.version.defaultPixelRevision),
          m_domain(handle.domain), m_layout(handle.layout)
    { Q_ASSERT(handle.isValid()); }
    virtual void *pinStorage(bool residentOnly, KisCpuResidentReadStatus *status) = 0;
    virtual void *pinResidentStorageAfterGateWait(KisCpuResidentReadStatus *status)
    { return pinStorage(true, status); }
    virtual void unpinStorage() noexcept = 0;
    virtual void releaseStorage() noexcept = 0;
    virtual bool supportsBackingHandoff() const { return false; }
    // Nonblocking physical exclusion, not a normal resident read pin. The
    // matching finish either cancels or converts to the ordinary writer pin.
    virtual void *tryClaimStorageForHandoff() noexcept { return nullptr; }
    virtual void finishStorageHandoff(bool writable) noexcept { Q_UNUSED(writable); }

private:
    friend class KisPageMetadataCoordinator;
    friend struct KisPageReplicaState;
    friend struct KisPageBackingRecord;
    friend class KisCpuWriteBindingReservation;
    friend class KisCpuBackingHandoff;
    friend class KisCpuReplicaPreparation;
    friend class KisTiles3PageReplicaProvider;
    template<class Binding> friend struct KisCpuResidentAllocationIndex;
    // Only exported through an already active owner-issued read guard.
    KisReplicaAllocationIdentity readIdentity(const KisPageVersion &version) const;
    enum class WriteState : quint8 { Idle, Active, Reserved, Handoff };
    KisReplicaAllocationIdentity allocationIdentityLocked() const
    { return {m_slot.provider, m_slot.providerEpoch, {m_slot.slot, m_incarnation}}; }
    bool matchesVersionLocked(const KisPageVersion &version) const
    {
        return m_key == version.key && m_generation == version.generation
            && m_defaultPixelRevision == version.defaultPixelRevision;
    }
    bool matchesHandleLocked(const KisReplicaHandle &expected) const
    {
        return allocationIdentityLocked() == expected.allocationIdentity()
            && matchesVersionLocked(expected.version) && m_domain == expected.domain && m_layout == expected.layout;
    }
    KisReplicaHandle projectHandle(const KisPageVersion &version, quint64 incarnation) const;
    bool prepareHandoff(const KisReplicaHandle &source, const KisPageVersion &target,
                         const KisPageAllocationDescriptor &descriptor, quint64 *incarnation) const;
    bool tryClaimHandoff(quint64 incarnation);
    void finishHandoff(const KisPageVersion *target) noexcept;
    bool reserveWrite(const KisReplicaAllocationIdentity &expected);
    void releaseWriter(WriteState state);
    void *pinReservedWrite(bool residentOnly, KisCpuResidentReadStatus *status);
    void unpinReservedWrite();
    void releaseReservedWrite();
    bool awaitStorage(std::unique_lock<QMutex> &lock, bool wait);
    void startStorageTransition();
    void *pinStorageUnlocked(std::unique_lock<QMutex> &lock, bool residentOnly,
                            bool waitedPin, KisCpuResidentReadStatus *status);
    // Ends the original transition, including deferred revoke/free, and
    // returns with the state gate unlocked. Physical work never runs under R.
    void finishStorageTransition(std::unique_lock<QMutex> &lock) noexcept;
    mutable QMutex m_mutex;
    // Physical transition owner holds this gate outside m_mutex. A cold
    // waiter drops m_mutex, waits here, releases it, then rechecks state.
    QMutex m_storageGate;
    bool m_storageBusy = false;
    bool m_storageReleased = false;
    quint64 m_readers = 0;
    WriteState m_writeState = WriteState::Idle;
    bool m_retired = false;
    void *m_data = nullptr;
    // Physical facts and logical page never change during this lifetime.
    // The immutable key also orders multi-Resource acceptance gates without
    // reading mutable version state. Only incarnation/generation/default
    // revision advance, together under the original gate.
    // External handles are projections, never another mutable resource record.
    const KisReplicaPhysicalSlotIdentity m_slot;
    quint64 m_incarnation;
    const KisPageKey m_key;
    KisPageGeneration m_generation;
    quint64 m_defaultPixelRevision;
    const KisPageAccessDomain m_domain;
    const KisReplicaLayout m_layout;
    std::shared_ptr<KisPageReadinessSignal> m_retirementReadiness;
};

// Borrowed inputs of one synchronous creation call; no storage/owner or
// second mutable write selection. Only the public-provider adapter may issue
// an external operation into the caller's original expected-id slot.
class KisPageOwnerLedger;
struct KisCpuReplicaInput
{
    enum class Kind { Default, Copy, Payload, SourceCopy, Alias };
    KisCpuReplicaInput(Kind kind, KisPageOwnerLedger &owner, KisPageOperationId &operation,
        const KisPageVersion &version, const KisPageAllocationDescriptor &descriptor,
        KisPageWriteMode mode, KisPagePriority priority, const KisReplicaHandle &source,
        const std::shared_ptr<KisCpuResidentBinding> &sourceBinding, const KisCpuPagePayload *payload,
        const std::shared_ptr<const KisPageReplicaSource> &initialization)
        : kind(kind), version(version), descriptor(descriptor), mode(mode), priority(priority),
          source(source), sourceBinding(sourceBinding), payload(payload), initialization(initialization),
          m_owner(owner), m_operation(operation) {}
    Kind kind;
    const KisPageVersion &version;
    const KisPageAllocationDescriptor &descriptor;
    KisPageWriteMode mode;
    KisPagePriority priority;
    const KisReplicaHandle &source;
    const std::shared_ptr<KisCpuResidentBinding> &sourceBinding;
    const KisCpuPagePayload *payload;
    const std::shared_ptr<const KisPageReplicaSource> &initialization;
private:
    friend class KisPageReplicaProvider;
    KisPageOwnerLedger &m_owner;
    KisPageOperationId &m_operation;
};

// Only the actual native producer may issue the compact ready alternative.
// A custom provider's public operation remains untrusted until the original
// owner validates it. This stack sum type adds no heap allocation or cleanup
// authority: every owned rejected result uses the original retirement record.
class KRITAIMAGE_EXPORT KisCpuReplicaPreparation
{
public:
    KisCpuReplicaPreparation() : m_result(Native{}) {}
    KisCpuReplicaPreparation(KisCpuReplicaPreparation &&) noexcept = default;
    KisCpuReplicaPreparation &operator=(KisCpuReplicaPreparation &&) noexcept = default;
    KisCpuReplicaPreparation(const KisCpuReplicaPreparation &) = delete;
    KisCpuReplicaPreparation &operator=(const KisCpuReplicaPreparation &) = delete;
    static KisCpuReplicaPreparation external(KisReplicaOperation result)
    { KisCpuReplicaPreparation value; value.m_result = std::move(result); return value; }
    static KisCpuReplicaPreparation failed(QString error)
    { KisCpuReplicaPreparation value; value.m_result = std::move(error); return value; }
    bool nativeReady() const
    { const auto *native = std::get_if<Native>(&m_result); return native && native->binding; }
    bool nativeReadyFrom(const KisPageReplicaProvider &provider) const
    { const auto *native = std::get_if<Native>(&m_result); return native && native->binding && native->issuer == &provider; }
    const KisReplicaOperation *externalOperation() const
    { return std::get_if<KisReplicaOperation>(&m_result); }
    KisReplicaHandle replica(const KisPageVersion &version) const
    {
        if (const auto *external = externalOperation()) return external->replica;
        const auto *native = std::get_if<Native>(&m_result);
        // Fresh producer slots always begin at incarnation one. Never infer
        // the expected identity from mutable state after returning the result.
        return native && native->binding ? native->binding->projectHandle(version, 1) : KisReplicaHandle{};
    }
    std::shared_ptr<KisCpuResidentBinding> takeNativeBinding()
    {
        auto *native = std::get_if<Native>(&m_result);
        return native ? std::move(native->binding) : std::shared_ptr<KisCpuResidentBinding>{};
    }
    QString error() const
    {
        if (const auto *external = externalOperation()) return external->error;
        const auto *failure = std::get_if<QString>(&m_result);
        return failure ? *failure : QString{};
    }
    KisReplicaOperation takeExternalOperation(KisPageOperationId operation)
    {
        if (auto *external = std::get_if<KisReplicaOperation>(&m_result)) return std::move(*external);
        Q_ASSERT(!nativeReady());
        return KisReplicaOperation::failed(operation, error());
    }
private:
    friend class KisTiles3PageReplicaProvider;
    struct Native {
        std::shared_ptr<KisCpuResidentBinding> binding;
        const KisPageReplicaProvider *issuer = nullptr; // Borrowed during this synchronous call only.
    };
    explicit KisCpuReplicaPreparation(std::shared_ptr<KisCpuResidentBinding> binding)
        : m_result(Native{std::move(binding), nullptr}) {}
    std::variant<Native, KisReplicaOperation, QString> m_result;
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
                   const std::shared_ptr<KisCompletionRegistry> &registry,
                   QString *error);
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
    std::shared_ptr<KisCompletionRegistry> completions;
    quint64 completionSource = 0;
    // Synchronous producers return this only after their operation succeeds.
    // Retirement remains a distinct, pending ticket owned by each allocation.
    KisCompletionTicket readyCompletion;
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
    const std::shared_ptr<KisCpuResidentBinding> &binding, KisPageLeaseId lease, KisPageOperationId operation,
    const KisReplicaHandle &replica, KisPageAccessRequirement requirement, KisPageAccessMode mode);
KRITAIMAGE_EXPORT void kisReleaseCpuBindingAccess(KisCpuBindingLeaseMap &activeLeases,
    const std::shared_ptr<KisCpuResidentBinding> &binding, const KisReplicaAccess &access);
KRITAIMAGE_EXPORT KisReplicaOperation kisTransferCpuBinding(const KisReplicaTransferRequest &request,
    const KisCompletionTicket &readyCompletion,
    const std::shared_ptr<KisCpuResidentBinding> &source,
    const std::shared_ptr<KisCpuResidentBinding> &target);

// The stable binding is the allocation record. The intrusive index owns one
// original shared reference in that record, rather than a separately paid map
// node/control protocol. Preparation never installs that reference; extract and
// index destruction always move it out after unlinking the original node.
template<class Binding>
struct KisCpuResidentAllocationIndex : KisCpuResidentProviderState
{
    struct SlotKey {
        using type = quint64;
        const quint64 &operator()(const Binding &binding) const noexcept
        { return binding.m_slot.slot; }
    };
    using AllocationIndex = boost::intrusive::set<Binding, boost::intrusive::key_of_value<SlotKey>>;
    using Iterator = typename AllocationIndex::iterator;
    struct Retirement {
        Iterator allocation;
        KisCompletionTicket completion;
        QString failure;
    };

    ~KisCpuResidentAllocationIndex()
    {
        while (!allocations.empty()) {
            auto released = extractAllocation(allocations.begin());
        }
    }

    const std::shared_ptr<Binding> &providerBinding(Iterator entry) const noexcept
    {
        Q_ASSERT(entry != allocations.end() && entry->providerOwnership.get() == &*entry);
        return entry->providerOwnership;
    }
    bool installAllocation(std::shared_ptr<Binding> &candidate, const KisCompletionTicket &retirement)
    {
        Q_ASSERT(candidate && !candidate->is_linked() && !candidate->providerOwnership && retirement.isValid());
        const auto installed = allocations.insert(*candidate);
        if (!installed.second) return false;
        candidate->retirementCompletion = retirement;
        candidate->providerOwnership = std::move(candidate);
        return true;
    }
    std::shared_ptr<Binding> extractAllocation(Iterator entry) noexcept
    {
        auto &record = *entry;
        allocations.erase(entry);
        return std::move(record.providerOwnership);
    }

    bool owns(const KisReplicaHandle &handle) const
    {
        return handle.isValid() && handle.provider == config.provider &&
               handle.providerEpoch == config.providerEpoch &&
               handle.domain == KisPageAccessDomain::CpuRam;
    }
    Iterator findExactAllocation(const KisReplicaHandle &handle)
    {
        auto found = allocations.find(handle.allocation.slot);
        return owns(handle) && found != allocations.end() && found->matchesHandle(handle)
            ? found : allocations.end();
    }

    void revokeBindings()
    {
        std::lock_guard<QMutex> locker(mutex);
        for (const auto &[id, lease] : std::as_const(activeLeases)) {
            const auto found = allocations.find(lease.allocation.slot);
            if (found == allocations.end() || !found->matchesAllocation(lease.allocation)) continue;
            if (lease.mode == KisPageAccessMode::Read) found->releaseRead();
            else found->releaseWrite();
        }
        for (auto &allocation : allocations) {
            allocation.revoke();
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
            activeLeases, providerBinding(allocation), lease, operation, replica, requirement, mode);
    }

    void releaseAccess(const KisReplicaAccess &access)
    {
        std::lock_guard<QMutex> locker(mutex);
        const auto allocation = findExactAllocation(access.replica);
        if (allocation != allocations.end()) kisReleaseCpuBindingAccess(
            activeLeases, providerBinding(allocation), access);
    }

    std::shared_ptr<KisCpuResidentBinding> binding(
        const KisReplicaHandle &handle, KisCpuResidentReadStatus *status)
    {
        std::lock_guard<QMutex> locker(mutex);
        const auto found = findExactAllocation(handle);
        if (status) *status = found == allocations.end()
            ? KisCpuResidentReadStatus::InvalidIdentity : KisCpuResidentReadStatus::Ready;
        return found == allocations.end()
            ? std::shared_ptr<KisCpuResidentBinding>{} : providerBinding(found);
    }

    KisReplicaOperation transfer(const KisReplicaTransferRequest &request)
    {
        const auto fail = [&request](const QString &reason) {
            return KisReplicaOperation::failed(request.operation, reason);
        };
        if (!request.isSameProviderTransfer() || !owns(request.source) ||
            !owns(request.target) || !operationAvailable(request.operation))
            return fail(QStringLiteral("transfer request is invalid"));
        consumeOperation(request.operation);
        const auto source = findExactAllocation(request.source);
        const auto target = findExactAllocation(request.target);
        if (source == allocations.end() || target == allocations.end() ||
            !request.source.layout.matches(request.descriptor) ||
            !request.target.layout.matches(request.descriptor)) return fail(QStringLiteral("transfer allocation is stale"));
        return kisTransferCpuBinding(request, readyCompletion,
                                     providerBinding(source), providerBinding(target));
    }

    Retirement beginRetirement(KisPageOperationId operation,
                               const KisReplicaHandle &replica,
                               const KisCompletionTicket &lastUse)
    {
        if (!owns(replica) || !operationAvailable(operation))
            return {allocations.end(), {}, QStringLiteral("retirement request is invalid")};
        if (lastUse.isValid()) {
            const auto status = completions->status(lastUse);
            if (status == KisCompletionStatus::Unknown || status == KisCompletionStatus::Pending)
                return {allocations.end(), {}, QStringLiteral("retirement last use is incomplete")};
        }
        const auto allocation = findExactAllocation(replica);
        if (allocation == allocations.end())
            return {allocation, {}, QStringLiteral("retirement allocation is stale")};
        const auto completion = allocation->retirementCompletion;
        if (!completion.isValid()) qFatal("Live allocation has no prepared retirement completion");
        return {allocation, completion, {}};
    }

    AllocationIndex allocations;
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
    static KisCpuWriteBindingReservation acquire(const std::shared_ptr<KisCpuResidentBinding> &binding,
                                                 const KisReplicaAllocationIdentity &expected);
    bool isValid() const { return bool(m_binding); }
    const std::shared_ptr<KisCpuResidentBinding> &binding() const { return m_binding; }
    // Waits only for the local gate/swap barrier, never initiates swap-in.
    void *pinResident(KisCpuResidentReadStatus *status = nullptr);
    // Explicit cold control path; the caller must first observe NonResident.
    // Current tiles3 storage restores synchronously, outside the owner gate.
    void *materialize();
    void unpin();
    void reset();
private:
    friend class KisCpuBackingHandoff;
    std::shared_ptr<KisCpuResidentBinding> m_binding;
};

/** Prepared physical transfer only; no logical write or recovery permission.
 * prepare() retains the provider and mints the target before metadata/storage
 * preparation, rejecting already known readers/writers. Retry preparation
 * after such a refusal; an observation never grants future write permission.
 * tryClaim() rechecks and excludes physical consumers without exposing bytes.
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
    static KisCpuBackingHandoff prepare(const std::shared_ptr<KisPageReplicaProvider> &provider,
                                        const KisReplicaHandle &source, const KisPageVersion &target,
                                        const KisPageAllocationDescriptor &descriptor,
                                        std::shared_ptr<KisCpuResidentBinding> binding = {});
    bool isValid() const { return bool(m_binding); }
    bool isClaimed() const { return m_claimed; }
    // Project for external metadata/ledger consumers before consuming this
    // capability. A moved, reset or committed token has no target authority.
    KisReplicaHandle target() const;
    bool tryClaim();
    KisCpuWriteBindingReservation commit() noexcept;
    void reset() noexcept;
private:
    std::shared_ptr<KisPageReplicaProvider> m_provider;
    std::shared_ptr<KisCpuResidentBinding> m_binding;
    quint64 m_incarnation = 0;
    KisPageVersion m_targetVersion;
    bool m_claimed = false;
};

#endif

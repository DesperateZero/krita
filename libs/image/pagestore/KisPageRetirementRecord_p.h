/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_PAGE_RETIREMENT_RECORD_P_H
#define KIS_PAGE_RETIREMENT_RECORD_P_H
#include "KisPageRetirementStorage_p.h"
#include "KisPageBackingBudget_p.h"
#include "KisPageReplicaProvider.h"
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/set.hpp>
#include <tuple>
struct KisPageRetirementWait;
struct KisPagePreparedBackingChange;
struct KisPagePhysicalBackingRecord;
class KisCpuResidentBinding;

// Original idle ownership plus its stable physical binding. The binding still
// checks the exact generation and live reader/writer state before handoff.
struct KisPageIdleCapacity {
    KisReplicaHandle replica;
    std::shared_ptr<KisCpuResidentBinding> binding;
    // Publication transfers its original detached record to the current
    // operation. Its page/session claim excludes another checkout and close;
    // removing it from the idle list excludes expiry. The caller must consume
    // it by handoff or its existing retirement guard before ending that claim.
    // This location alone grants no ledger/physical write permission.
    KisPageRetirementRecord *original = nullptr;
};

struct KisPageBackingRecord
{
    KisReplicaHandle replica;
    KisBackingBudgetClass budgetClass = KisBackingBudgetClass::ActivePending;
    KisBackingBudgetReservation retirementHeadroom;
    // Non-null only while the ledger owns the enclosing retirement record.
    KisPageRetirementRecord *retirement = nullptr;
    KisPagePhysicalBackingRecord *physicalRecord = nullptr;
    KisPageBackingRecord *previous = nullptr;
    KisPageBackingRecord *next = nullptr;
    // The provider's stable allocation record follows this original backing,
    // including idle/handoff and rejected retirement. No D workspace survives.
    std::shared_ptr<KisCpuResidentBinding> nativeBinding;
};

struct KisPagePhysicalBackingRecord : boost::intrusive::set_base_hook<boost::intrusive::optimize_size<true>>
{
    explicit KisPagePhysicalBackingRecord(KisMutationStorageAllocator<KisPagePhysicalBackingRecord> allocator)
        : storage(std::move(allocator)) {}
    KisReplicaPhysicalSlotIdentity identity;
    // Independent actual allocation/refund lifetime. The first terminal may
    // end before a surviving alias; it must not keep dead terminal bytes.
    KisMutationStorageAllocator<KisPagePhysicalBackingRecord> storage;
    struct Key {
        using type = KisReplicaPhysicalSlotIdentity;
        const type &operator()(const KisPagePhysicalBackingRecord &record) const { return record.identity; }
    };
    KisReplicaHandle representative;
    KisPageAccessDomain domain = KisPageAccessDomain::Unknown;
    quint64 domainRevision = 0;
    quint64 domainClaim = 0;
    const KisPagePreparedBackingChange *classClaim = nullptr;
    std::array<quint32, static_cast<size_t>(KisBackingBudgetClass::Count)> references{};
    KisBackingBudgetClass chargedClass = KisBackingBudgetClass::Count;
    KisPageBackingRecord *members = nullptr;

    quint64 byteSize() const { return representative.layout.byteSize; }
    bool shared() const { return members && members->next; }
    // Only the original physical record edits membership, under the ledger
    // gate. Budget projections copy this view without editing any links.
    void link(KisPageBackingRecord &record) noexcept
    {
        Q_ASSERT(!record.physicalRecord);
        record.physicalRecord = this;
        record.next = members;
        if (members) members->previous = &record;
        members = &record;
    }
    void unlink(KisPageBackingRecord &record) noexcept
    {
        if (record.previous) record.previous->next = record.next;
        else members = record.next;
        if (record.next) record.next->previous = record.previous;
        record.previous = record.next = nullptr;
        record.physicalRecord = nullptr;
    }
};

struct KisPageBackingIdentityLess
{
    bool operator()(const KisReplicaAllocationIdentity &a,
                    const KisReplicaAllocationIdentity &b) const noexcept
    {
        return std::tie(a.provider.value, a.providerEpoch.value, a.allocation.slot, a.allocation.generation)
            < std::tie(b.provider.value, b.providerEpoch.value, b.allocation.slot, b.allocation.generation);
    }
    bool operator()(const KisReplicaPhysicalSlotIdentity &a,
                    const KisReplicaPhysicalSlotIdentity &b) const noexcept
    {
        return std::tie(a.provider.value, a.providerEpoch.value, a.slot)
            < std::tie(b.provider.value, b.providerEpoch.value, b.slot);
    }
};
struct KisPageBackingNode : boost::intrusive::set_base_hook<> {
    KisReplicaAllocationIdentity first;
    KisPageBackingRecord second;
    struct Key {
        using type = KisReplicaAllocationIdentity;
        const type &operator()(const KisPageBackingNode &node) const { return node.first; }
    };
};
using KisPageBackingIndex = boost::intrusive::set<KisPageBackingNode,
    boost::intrusive::key_of_value<KisPageBackingNode::Key>,
    boost::intrusive::compare<KisPageBackingIdentityLess>>;
using KisPagePhysicalBackingIndex = boost::intrusive::set<KisPagePhysicalBackingRecord,
    boost::intrusive::key_of_value<KisPagePhysicalBackingRecord::Key>,
    boost::intrusive::compare<KisPageBackingIdentityLess>>;

// Transient ledger preparation in the caller's frozen publication row. The
// physical claim protects these locations; no copied replica or second input
// owns the operation. Generic independent preparations use the same row.
struct KisPagePreparedBackingChange
{
    KisPageBackingIndex::iterator backing;
    KisPagePhysicalBackingRecord *physical = nullptr;
    size_t order = 0;
    KisBackingBudgetClass after = KisBackingBudgetClass::Count;
    KisBackingBudgetClass chargedAfter = KisBackingBudgetClass::Count;
    KisPageRetirementRecord **idleResult = nullptr;
    const void *claim = nullptr;
    const KisPagePreparedBackingChange *next = nullptr;
    KisPageBackingRecord *credit = nullptr;
    KisBackingBudgetClass creditBefore = KisBackingBudgetClass::Count;
};

struct KisPageProviderOperationRecord
{
    KisCompletionTicket completion;
    KisPageRetirementRecord *retirement = nullptr;
};
struct KisPageProviderOperationNode : boost::intrusive::set_base_hook<> {
    quint64 first = 0;
    KisPageProviderOperationRecord second;
    struct Key {
        using type = quint64;
        const quint64 &operator()(const KisPageProviderOperationNode &node) const { return node.first; }
    };
};
using KisPageProviderOperationIndex = boost::intrusive::set<KisPageProviderOperationNode,
    boost::intrusive::key_of_value<KisPageProviderOperationNode::Key>>;

struct KisPageRetirementRecord : boost::intrusive::list_base_hook<>
{
    explicit KisPageRetirementRecord(KisMutationStorageAllocator<KisPageRetirementRecord> allocator)
        : storage(std::move(allocator))
    {
        operationNode.second.retirement = this;
    }
    KRITAIMAGE_EXPORT void preparePhysicalStorage();
    KisMutationStorageAllocator<KisPageRetirementRecord> storage;
    KisReplicaHandle replica;
    std::shared_ptr<KisPageReplicaProvider> provider;
    // Valid only on the non-owning publication idle chain; the ledger owns
    // this same record until checkout or original retirement handoff.
    qint64 idleDeadline = 0;
    // Fixed at original retirement-owner admission; retries never re-query
    // provider code or maintain another wake-eligibility policy.
    bool backgroundRetirement = false;
    KisCompletionTicket lastUse;
    KisPageOperationId retirementOperation;
    KisReplicaOperation retirementResult;
    KisPageProviderOperationNode operationNode;
    KisPageBackingNode backingNode;
    KisPagePhysicalBackingPointer physicalStorage;
    // Rare provider-result rejection: keep the preallocation reservation
    // charged until an unregistered physical replica reaches terminal retire.
    KisBackingBudgetReservation orphanReservation;
    // Funded once for this retirement lifetime. The queue distinguishes a
    // cached state from its currently armed subscription/permission.
    std::shared_ptr<KisPageRetirementWait> wait;
    KisBackingBudgetReservation admissionReservation;
    bool readinessRechecked = false;
    bool orphanDebtAdmitted = false;
};

// These containers own already-admitted nodes. Splice and close transfer the
// same storage; no partition or pass creates another record allocation.
struct KisPageRetirementRecords : boost::intrusive::list<KisPageRetirementRecord> {
    KisPageRetirementRecords() = default;
    KisPageRetirementRecords(KisPageRetirementRecords &&other) noexcept { swap(other); }
    KisPageRetirementRecords &operator=(KisPageRetirementRecords &&other) noexcept
    { if (this != &other) { clear_and_dispose(KisPageRetirementRecordDeleter{}); swap(other); } return *this; }
    ~KisPageRetirementRecords() { clear_and_dispose(KisPageRetirementRecordDeleter{}); }
};
struct KisPageBackingPreparation {
    KisBackingBudgetReservation reservation;
    KisPageRetirementRecordPointer retirement;
};

#endif

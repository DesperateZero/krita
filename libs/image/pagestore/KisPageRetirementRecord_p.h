/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_PAGE_RETIREMENT_RECORD_P_H
#define KIS_PAGE_RETIREMENT_RECORD_P_H
#include "KisPageRetirementStorage_p.h"
#include "KisPageWriteCoordinator_p.h"
#include "KisPageReplicaProvider.h"
#include <boost/intrusive/list.hpp>
#include <map>
#include <tuple>
struct KisPageRetirementWait;

struct KisPageBackingRecord
{
    KisReplicaHandle replica;
    KisBackingBudgetClass budgetClass = KisBackingBudgetClass::ActivePending;
    KisReplicaPhysicalSlotIdentity physical;
    KisBackingBudgetReservation retirementHeadroom;
    KisPageRetirementRecordPointer retirement;
    KisPageBackingRecord *previous = nullptr;
    KisPageBackingRecord *next = nullptr;
};

struct KisPagePhysicalBackingRecord
{
    KisReplicaHandle representative;
    KisPageAccessDomain domain = KisPageAccessDomain::Unknown;
    quint64 domainRevision = 0;
    quint64 domainClaim = 0;
    quint64 classClaim = 0;
    std::array<quint32, static_cast<size_t>(KisBackingBudgetClass::Count)> references{};
    KisBackingBudgetClass chargedClass = KisBackingBudgetClass::Count;
    KisPageBackingRecord *members = nullptr;

    quint64 byteSize() const { return representative.layout.byteSize; }
    bool shared() const { return members && members->next; }
    // Only the original physical record edits membership, under the ledger
    // gate. Budget projections copy this view without editing any links.
    void link(KisPageBackingRecord &record) noexcept
    {
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
using KisPageBackingIndex = std::map<KisReplicaAllocationIdentity, KisPageBackingRecord,
    KisPageBackingIdentityLess,
    KisMutationStorageAllocator<std::pair<const KisReplicaAllocationIdentity, KisPageBackingRecord>>>;
using KisPagePhysicalBackingIndex = std::map<KisReplicaPhysicalSlotIdentity, KisPagePhysicalBackingRecord,
    KisPageBackingIdentityLess,
    KisMutationStorageAllocator<std::pair<const KisReplicaPhysicalSlotIdentity, KisPagePhysicalBackingRecord>>>;

struct KisPageProviderOperationRecord
{
    KisCompletionTicket completion;
    bool detachedRetirement = false;
    KisPageRetirementRecord *retirement = nullptr;
};
using KisPageProviderOperationIndex = std::map<quint64, KisPageProviderOperationRecord,
    std::less<quint64>, KisMutationStorageAllocator<std::pair<const quint64, KisPageProviderOperationRecord>>>;

struct KisPageRetirementRecord : boost::intrusive::list_base_hook<>
{
    explicit KisPageRetirementRecord(KisMutationStorageAllocator<KisPageRetirementRecord> allocator)
        : storage(std::move(allocator))
    {
        KisPageProviderOperationIndex prepared(std::less<quint64>{},
            KisMutationStorageAllocator<std::pair<const quint64, KisPageProviderOperationRecord>>(storage));
        prepared.emplace(0, KisPageProviderOperationRecord{{}, true, this});
        operationStorage = prepared.extract(0);
        prepareBackingStorage();
    }
    void prepareBackingStorage()
    {
        // Prepare real installation nodes before a provider can create pixels.
        // Ordered indexes transfer these exact nodes without bucket growth.
        KisPageBackingIndex backing(KisPageBackingIdentityLess{},
            KisMutationStorageAllocator<std::pair<const KisReplicaAllocationIdentity, KisPageBackingRecord>>(storage));
        if (backingStorage.empty()) {
            backing.emplace(KisReplicaAllocationIdentity{}, KisPageBackingRecord{});
            backingStorage = backing.extract(backing.begin());
        }
        KisPagePhysicalBackingIndex physical(KisPageBackingIdentityLess{},
            KisMutationStorageAllocator<std::pair<const KisReplicaPhysicalSlotIdentity, KisPagePhysicalBackingRecord>>(storage));
        if (physicalStorage.empty()) {
            physical.emplace(KisReplicaPhysicalSlotIdentity{}, KisPagePhysicalBackingRecord{});
            physicalStorage = physical.extract(physical.begin());
        }
    }
    KisMutationStorageAllocator<KisPageRetirementRecord> storage;
    KisReplicaHandle replica;
    QSharedPointer<KisPageReplicaProvider> provider;
    bool backgroundRetirement = false;
    KisCompletionTicket lastUse;
    KisPageOperationId retirementOperation;
    KisReplicaOperation retirementResult;
    KisPageProviderOperationIndex::node_type operationStorage;
    KisPageBackingIndex::node_type backingStorage;
    KisPagePhysicalBackingIndex::node_type physicalStorage;
    // Rare provider-result rejection: keep the preallocation reservation
    // charged until an unregistered physical replica reaches terminal retire.
    KisBackingBudgetReservation orphanReservation;
    std::shared_ptr<KisPageRetirementWait> wait;
    KisBackingBudgetReservation admissionReservation;
    bool wakeEligible = false;
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

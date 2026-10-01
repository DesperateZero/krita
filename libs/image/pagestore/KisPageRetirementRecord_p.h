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
struct KisPageRetirementWait;

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
    }
    KisMutationStorageAllocator<KisPageRetirementRecord> storage;
    KisReplicaHandle replica;
    QSharedPointer<KisPageReplicaProvider> provider;
    bool backgroundRetirement = false;
    KisCompletionTicket lastUse;
    KisPageOperationId retirementOperation;
    KisReplicaOperation retirementResult;
    KisPageProviderOperationIndex::node_type operationStorage;
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

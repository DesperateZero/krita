/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_OWNER_LEDGER_H
#define KIS_PAGE_OWNER_LEDGER_H
#include "KisPagePublicationRows_p.h"

#include <QScopedPointer>
#include <QSharedPointer>
#include <vector>

#include "KisCompletionRegistry.h"
#include "KisPageRetirementStorage_p.h"
#include "KisPageRetirementRecord_p.h"
#include "KisPageMetadataCoordinator.h"
#include "KisPageReplicaProvider.h"
#include "KisPageWriteCoordinator_p.h"

class KisBackingBudgetController;
class KisBackingBudgetReservation;
class KisBackingBudgetWaiter;
class KisPageOwnerDomainAdmission;
class KisPageOwnerLedger;
struct KisPageRetirementRecords;
struct KisPageBackingRecord;
struct KisPagePhysicalBackingRecord;
enum class KisBackingBudgetClass : quint8;

struct KisBackingClassChange
{
    KisReplicaHandle replica;
    KisBackingBudgetClass before;
    KisBackingBudgetClass after;
    // Borrowed output in the original publication candidate. Written only
    // on commit; the publication gate protects the ledger-owned record until
    // its immediate Idle handover. Cancellation never publishes a pointer.
    KisPageRetirementRecord **idleResult = nullptr;
};

/**
 * One-shot owner reservation for a frozen set of backing-class changes.
 * Destruction cancels the budget reservation and releases the physical claims.
 * A successfully admitted empty set is valid without an entity slot/claim.
 */
class KRITAIMAGE_EXPORT KisBackingClassChangeReservation final
{
public:
    KisBackingClassChangeReservation() = default;
    ~KisBackingClassChangeReservation();
    KisBackingClassChangeReservation(KisBackingClassChangeReservation &&) noexcept;
    KisBackingClassChangeReservation &operator=(KisBackingClassChangeReservation &&) noexcept;
    KisBackingClassChangeReservation(const KisBackingClassChangeReservation &) = delete;
    KisBackingClassChangeReservation &operator=(const KisBackingClassChangeReservation &) = delete;

    bool isValid() const;
    void release() noexcept;

private:
    KisBackingClassChangeReservation(KisPageOwnerLedger *owner, quint64 cookie);
    KisPageOwnerLedger *m_owner = nullptr;
    quint64 m_cookie = 0;
    KisPagePreparedBackingChange *m_rows = nullptr;
    KisPagePublicationRows m_publicationRows;
    size_t m_count = 0;
    KisBackingBudgetReservation m_budget;

    friend class KisPageOwnerLedger;
};

/** Prepared accounting only; does not authorize a physical or logical write.
 * The ledger and its budget must outlive the reservation, as for class changes.
 */
class KRITAIMAGE_EXPORT KisBackingHandoffReservation final
{
public:
    KisBackingHandoffReservation() = default;
    ~KisBackingHandoffReservation();
    KisBackingHandoffReservation(KisBackingHandoffReservation &&) noexcept;
    KisBackingHandoffReservation &operator=(KisBackingHandoffReservation &&) noexcept;
    bool isValid() const { return m_owner; }
    void release() noexcept;
private:
    // The original caller already owns this one-shot lifetime. Its physical
    // claim keeps both records stable; no second slot, cookie or target node.
    KisPageOwnerLedger *m_owner = nullptr;
    KisPageBackingIndex::iterator m_source;
    KisPagePhysicalBackingRecord *m_physical = nullptr;
    KisReplicaHandle m_target;
    KisBackingBudgetReservation m_budget;
    friend class KisPageOwnerLedger;
};

/**
 * Document/session scoped identity and provider owner. It is the only BR1
 * component allowed to mint request/operation/lease/writer IDs, account for
 * exact physical backing identities, and seal a PreparedPageProof after
 * checking metadata, provider layout and a terminal producer completion.
 * It never calls a provider while holding its mutex.
 */
class KRITAIMAGE_EXPORT KisPageOwnerLedger
{
public:
    KisPageOwnerLedger();
    ~KisPageOwnerLedger();
    // Terminal root destruction only. Queue records can still hold rejected
    // provider results, so their embedded index members must be detached first.
    void detachRetirementIndexesForDestruction() noexcept;

    bool configure(const std::shared_ptr<KisCompletionRegistry> &completions,
                   QString *error = nullptr);
    void attachBackingBudget(KisBackingBudgetController &budget);
    bool registerBacking(const KisReplicaHandle &replica,
                         KisBackingBudgetReservation &reservation,
                         KisBackingBudgetClass budgetClass,
                         QString *error = nullptr,
                         KisPageRetirementRecordPointer *preparedRetirement = nullptr);
    KisPageRetirementRecordPointer takeRetirementRecord(const KisReplicaHandle &replica,
                                                       KisBackingBudgetClass *originalClass = nullptr);
    KisBackingBudgetClass backingClass(const KisReplicaHandle &replica) const;
    bool reclassifyBacking(const KisReplicaHandle &replica,
                           KisBackingBudgetClass budgetClass,
                           QString *error = nullptr);
    bool reclassifyBacking(const KisReplicaHandle &replica,
                           KisBackingBudgetClass budgetClass,
                           KisBackingBudgetReservation &reservation,
                           QString *error = nullptr);
    bool retainRetirementDebtReservation(const KisReplicaHandle &replica,
                                         KisBackingBudgetReservation &reservation,
                                         QString *error = nullptr);
    bool synchronizeBackingDomains(QString *error = nullptr);
    KisBackingClassChangeReservation prepareBackingChanges(
        KisPageSnapshotArray<KisBackingClassChange> changes,
        const KisPageSnapshotArray<KisPageTransitionEffect> &retirementEffects,
        QString *error = nullptr);
    void commitBackingChanges(KisBackingClassChangeReservation &&reservation) noexcept;
    // Freeze a sole, resident physical owner and reserve its real transition
    // to ActivePending before taking the physical claim. Current/Pending also
    // retain cancellation debt. Detached Optional borrows without new future
    // debt; cancellation retains its Pending charge in the original retirement
    // record. The old handle stays authoritative until commit; cancel this
    // preparation before provider retag. Logical detachment is the caller's duty.
    KisBackingHandoffReservation prepareBackingHandoff(
        const KisReplicaHandle &source, const KisReplicaHandle &target,
        QString *error = nullptr, KisPageRetirementRecord *originalIdle = nullptr);
    // Caller has installed logical detachment and consumed physical retag under
    // its admission gate. Reuses the existing index node; no provider call or
    // allocation. Keeps any admitted debt headroom until ordinary publication
    // or retirement consumes it; callers must not own a second terminal token.
    bool commitBackingHandoff(
        KisBackingHandoffReservation &&reservation,
        const std::shared_ptr<KisCpuResidentBinding> &binding = {}) noexcept;
    /**
     * Pre-admit every replica emitted by a local metadata transition to
     * RetirementDebt. The returned cookie freezes the exact backing classes;
     * metadata must commit it only after its authoritative detach succeeds,
     * or cancel it on every rejection/stale retry. Each effect must still own
     * its registered backing and original retirement record.
     */
    bool prepareRetirementDebt(const KisPageTransitionEffect *effects, qsizetype count,
                               quint64 *cookie,
                               QString *error = nullptr);
    void commitRetirementDebt(quint64 cookie) noexcept;
    void cancelRetirementDebt(quint64 cookie) noexcept;

    bool registerProvider(const std::shared_ptr<KisPageReplicaProvider> &provider,
                          QString *error = nullptr);
    std::shared_ptr<KisPageReplicaProvider> provider(
        KisReplicaProviderId provider,
        KisReplicaProviderEpoch epoch) const;
    std::shared_ptr<KisPageReplicaProvider> providerFor(
        KisPageAccessRequirement access) const;

    KisPageRequestId nextRequestId();
    KisPageOperationId nextOperationId();
    KisPageLeaseId nextLeaseId();
    KisPageWriterToken nextWriterToken();

    bool bindProviderOperation(KisPageOperationId operation,
                               const KisReplicaOperation &result,
                               QString *error = nullptr);
    // Synchronous callers retain their result and activity until acceptance.
    // Verification needs no operation node; pending work uses explicit binding.
    KisVerifiedCompletion verifyTerminalProviderResult(
        KisPageOperationId operation,
        const KisReplicaOperation &result,
        QString *error = nullptr) const;
    KisVerifiedCompletion verifyProviderOperation(
        KisPageOperationId operation,
        QString *error = nullptr) const;
    bool releaseTerminalProviderOperation(KisPageOperationId operation,
                                          QString *error = nullptr);
    KisPageReadinessStatus watchCompletion(const KisCompletionTicket &ticket,
        KisPageReadinessCallback scheduleReady, KisPageReadinessSubscription *subscription) const;
    qsizetype providerOperationCount() const;
    // One locked O(1) snapshot; do not subtract two independently sampled
    // counts while retirement workers can bind/release operations.
    qsizetype publicationBlockingOperationCount() const;
    qsizetype sealedProofCount() const;
    KisPreparedPageProof exportPublication(const KisPagePublicationIdentity &) const;
    bool ownsPreparedPageProof(const KisPreparedPageProof &proof) const;
    // Store configuration binds exactly one original metadata owner. The CPU
    // producer consumes its prepaid version slot; exported proofs keep their
    // independent capability lifetime and may coexist for the same version.
    bool bindCpuProofs(KisPageMetadataCoordinator &metadata, const KisCompletionTicket &completion);
    // The caller freezes original rows through completion and acceptance.
    // One issuance range and retained provider serve the actual batch; each
    // original writer still validates its dynamic allocation and reservation.
    bool validatePrivateWrites(KisPagePublicationRows rows, KisPageTransactionId transaction,
                               QString *error = nullptr, bool complete = false);

    // Caller protects the Prepared version/provider across validation (owner
    // gate or equivalent transaction + per-key mutation claims). The ledger
    // mutex is not held across provider code and does not provide that claim.
    bool sealPreparedPage(const KisPageMetadataCoordinator &metadata,
                          const KisPageVersion &version,
                          KisPageTransactionId transaction,
                          const KisPageAllocationDescriptor &descriptor,
                          const KisCompletionTicket &producerCompletion,
                          KisPreparedPageProof *proof,
                          QString *error = nullptr,
                          bool *storageRefused = nullptr);
    bool validatePreparedPage(const KisPageMetadataCoordinator &metadata,
                              const KisPreparedPageProof &proof,
                              const KisPageAllocationDescriptor &descriptor,
                              QString *error = nullptr) const;
    bool revokePreparedPage(const KisPreparedPageProof &proof);

private:
    // Publication retains the original transaction/key claims. These consume
    // its compact identity directly; exported capabilities keep their checks.
    bool validatePublicationIdentity(const KisPageMetadataCoordinator &, const KisPagePublicationIdentity &) const;
    bool releasePublicationIdentity(const KisPagePublicationIdentity &);
    using BackingChanges = std::vector<KisBackingClassChange, KisMutationStorageAllocator<KisBackingClassChange>>;
    KisPageRetirementRecords takeShutdownRetirementRecords();
    // Synchronously read immutable publication facts into the caller's original
    // workspace. Reader: 1 = change, 0 = end, -1 = invalid input. It must not
    // acquire an owner/metadata/provider lock while the ledger gate is held.
    // Prepared rows outlive the reservation; input facts need only this call.
    KisBackingClassChangeReservation prepareBackingChanges(
        KisPagePreparedBackingChange *rows, size_t capacity, void *context,
        int (*read)(void *, KisBackingClassChange *), QString *error = nullptr,
        KisPagePublicationRows publicationRows = {});
    // Read the original frozen release rows; no intermediate class-change array.
    // Uses the same claims, accounting policy and terminal as generic changes.
    KisBackingClassChangeReservation prepareIdleCapacity(
        KisPagePublicationRows rows, quint64 allowance,
        QString *error = nullptr);
    KisBackingClassChangeReservation prepareBackingChangesImpl(
        BackingChanges changes,
        const KisPageTransitionEffect *effects, qsizetype count, QString *error);
    KisReplicaBackingFootprint observeBackingFootprint(
        const KisReplicaHandle &replica) const;
    // Internal only: the original queue/caller already owns this ledger record.
    // Same storage budget alone is not proof of ledger identity.
    KisPageReadinessStatus watchRetirementBudget(KisPageRetirementRecord &record,
        KisPageReadinessCallback notify, KisBackingBudgetWaiter *waiter);
    KisPageRetirementRecordPointer takeRetirementRecordImpl(const KisReplicaHandle &replica,
        KisBackingBudgetClass *originalClass, bool *registered);
    enum class RecordAdmission { Unregistered, Refused, Accepted };
    RecordAdmission admitRetirementRecord(KisPageRetirementRecord &record);
    RecordAdmission admitRetirementRecordLocked(KisPageRetirementRecord &record);
    // The queue exclusively owns the original record until completion/close.
    // Admission and embedded-node preparation share one ledger window.
    RecordAdmission prepareRetirementOperation(KisPageRetirementRecord &record,
                                               KisPageOperationId *operation);
    void cancelRetirementOperation(KisPageRetirementRecord &record);
    KisVerifiedCompletion observeRetirementOperation(KisPageRetirementRecord &record);
    bool consumeRetirementOperation(KisPageRetirementRecord &record,
                                    const KisVerifiedCompletion &terminal);
    KisPageReadinessStatus watchRetirementOperation(const KisPageRetirementRecord &record,
        KisPageReadinessCallback notify, KisPageReadinessSubscription *subscription) const;
    // Only initial adoption may withdraw an unpublished registration without
    // provider completion. Physical ownership stays with the external caller.
    bool rollbackBackingRegistration(KisPageRetirementRecord &record) noexcept;
    KisPageRetirementRecordPointer takeRetirementRecord(KisPageRetirementRecord &record,
        const KisReplicaHandle &expected);
    KisPageRetirementRecordPointer takeRetirementRecordLocked(KisPageBackingRecord &backing,
        const KisReplicaHandle &replica, KisBackingBudgetClass *originalClass);
    bool reclassifyBackingLocked(KisPageBackingIndex::iterator found,
        const KisReplicaHandle &replica, KisBackingBudgetClass budgetClass,
        KisBackingBudgetReservation *reservation, QString *error);
    bool reclassifyBackingImpl(const KisReplicaHandle &replica,
                               KisBackingBudgetClass budgetClass,
                               KisBackingBudgetReservation *reservation,
                               QString *error);
    void commitPreparedBackingChanges(quint64 cookie) noexcept;
    void cancelPreparedBackingChanges(quint64 cookie) noexcept;
    void finishBorrowedBackingChanges(KisBackingClassChangeReservation &, bool commit) noexcept;
    explicit KisPageOwnerLedger(const KisMutationStorageAllocator<KisPageOwnerLedger> &storage);
    class Private;
    struct PrivateReleaser { static void cleanup(Private *); };
    QScopedPointer<Private, PrivateReleaser> d;
    friend class KisPageStore;
    friend class KisPageMutationSession;
    friend class KisPageStoreCpuMutationTest;
    friend class KisPageOwnerDomainAdmission;
    friend class KisBackingClassChangeReservation;
    friend class KisBackingHandoffReservation;
    friend class KisPageRetirementQueue;
    friend class KisPagePublicationCoordinator;
    friend class KisPageStoreReferenceTest;
    friend class KisPageOwnerLedgerTestAccess;
};

#endif // KIS_PAGE_OWNER_LEDGER_H

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
    // Internal original-result location. Caller retains it through the ledger
    // preparation window; this is not a capability at an external boundary.
    KisPageRetirementRecord *original = nullptr;
};

/** Prepared accounting only; does not authorize a physical or logical write.
 * The ledger and its budget must outlive this physical handoff reservation.
 */
class KRITAIMAGE_EXPORT KisBackingHandoffReservation final
{
public:
    KisBackingHandoffReservation() = default;
    ~KisBackingHandoffReservation();
    KisBackingHandoffReservation(KisBackingHandoffReservation &&) noexcept;
    KisBackingHandoffReservation &operator=(KisBackingHandoffReservation &&) noexcept;
    bool isValid() const { return m_owner; }
    KisPageRetirementRecord *originalRecord() const
    { return m_owner ? m_source->second.retirement : nullptr; }
    void release() noexcept;
private:
    // The original caller already owns this one-shot lifetime. Its physical
    // claim keeps both records stable; no second slot, cookie or target node.
    KisPageOwnerLedger *m_owner = nullptr;
    KisPageBackingIndex::iterator m_source;
    KisPagePhysicalBackingRecord *m_physical = nullptr;
    // The original source/claim fixes all physical facts and next incarnation.
    KisPageGeneration m_targetGeneration;
    quint64 m_targetDefaultRevision = 0;
    KisBackingBudgetReservation m_budget;
    bool m_cpuOptionalFee = false;
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
    enum class BackingAcceptance { FeesRefused, InstallRejected, Accepted };
    // Freeze a sole, resident physical owner and reserve its real transition
    // to ActivePending before taking the physical claim. Current/Pending also
    // retain cancellation debt. Detached Optional borrows without new future
    // debt; cancellation retains its Pending charge in the original retirement
    // record. The old handle stays authoritative until commit; cancel this
    // preparation before provider retag. Logical detachment is the caller's duty.
    KisBackingHandoffReservation prepareBackingHandoff(
        const KisPageWriteSource &source, const KisReplicaHandle &target,
        QString *error = nullptr, KisPageRetirementRecord *originalIdle = nullptr);
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
    // Internal synchronous acceptance. The installer must be preallocated,
    // obey R -> V, and leave metadata unchanged on refusal. Delivery and
    // destruction of released references occur after this call has returned.
    bool acceptRetirementDebt(const KisPageTransitionEffect *effects, qsizetype count,
                              void *context, bool (*install)(void *), QString *error);

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
    bool matchesPublicationIdentity(const KisPagePublicationIdentity &, const KisPreparedPageProof &) const;
    bool ownsPublicationIdentity(const KisPagePublicationIdentity &) const;
    KisReplicaHandle publicationAuthority(const KisPagePublicationIdentity &) const;
    bool ownsPreparedPageProof(const KisPreparedPageProof &proof) const;
    // Store configuration binds exactly one original metadata owner. The CPU
    // producer consumes its prepaid version slot; exported proofs keep their
    // independent capability lifetime and may coexist for the same version.
    bool bindCpuProofs(KisPageMetadataCoordinator &metadata, const KisCompletionTicket &completion);
    // The caller freezes original rows through completion and acceptance.
    // Provenance/custom provider preparation precedes grouped Value freezing;
    // actual dynamic CPU Resource gates cover the eventual acceptance cut.
    bool preparePrivatePublication(KisPagePublicationRows rows, KisPageTransactionId transaction,
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
    bool preparePublicationIdentity(const KisPageMetadataCoordinator &, const KisPagePublicationIdentity &) const;
    bool inspectPublicationIdentity(const KisPageMetadataCoordinator &, const KisPagePublicationIdentity &, bool preparation) const;
    bool releasePublicationIdentity(const KisPagePublicationIdentity &);
    KisPageRetirementRecords takeShutdownRetirementRecords();
    // Provider domain synchronization must finish BEFORE Resource gates.
    // These prepared entries invoke no provider callback or physical I/O.
    // One synchronous ledger -> sorted Resource -> epoch -> metadata acceptance.
    // Budget preparation precedes Resource entry. Inputs and all installation
    // capacity are prepared before entry; install cannot call a provider,
    // reenter this ledger, or leave partial state on refusal. Final references
    // and physical delivery stay in the caller's unlocked cleanup.
    // Reader: 1 = change, 0 = end, -1 = invalid input. It consumes immutable
    // facts without acquiring another owner/metadata/provider gate.
    BackingAcceptance acceptPreparedBackingChanges(
        KisPagePreparedBackingChange *rows, size_t capacity, void *reader,
        int (*read)(void *, KisBackingClassChange *),
        void *context, bool (*install)(void *), QString *error = nullptr);
    BackingAcceptance acceptPreparedIdleCapacity(
        KisPagePublicationRows rows, quint64 allowance,
        void *context, bool (*install)(void *), QString *error = nullptr);
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
    KisVerifiedCompletion acceptRetirementOperation(KisPageRetirementRecord &record,
                                                    const KisReplicaOperation &result);
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
        KisBackingBudgetClass *originalClass);
    bool reclassifyBackingLocked(KisPageBackingIndex::iterator found,
        KisBackingBudgetClass budgetClass,
        KisBackingBudgetReservation *reservation, QString *error);
    bool reclassifyBackingImpl(const KisReplicaHandle &replica,
                               KisBackingBudgetClass budgetClass,
                               KisBackingBudgetReservation *reservation,
                               QString *error);
    explicit KisPageOwnerLedger(const KisMutationStorageAllocator<KisPageOwnerLedger> &storage);
    class Private;
    struct PrivateReleaser { static void cleanup(Private *); };
    QScopedPointer<Private, PrivateReleaser> d;
    friend class KisPageStore;
    friend class KisPageMutationSession;
    friend class KisPageStoreCpuMutationTest;
    friend class KisPageOwnerDomainAdmission;
    friend class KisBackingHandoffReservation;
    friend class KisPageRetirementQueue;
    friend class KisPagePublicationCoordinator;
    friend class KisPageStoreReferenceTest;
    friend class KisPageOwnerLedgerTestAccess;
};

#endif // KIS_PAGE_OWNER_LEDGER_H

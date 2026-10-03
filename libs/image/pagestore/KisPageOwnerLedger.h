/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_OWNER_LEDGER_H
#define KIS_PAGE_OWNER_LEDGER_H

#include <QScopedPointer>
#include <QSharedPointer>
#include <vector>

#include "KisCompletionRegistry.h"
#include "KisPageRetirementStorage_p.h"
#include "KisPageMetadataCoordinator.h"
#include "KisPageReplicaProvider.h"

class KisBackingBudgetController;
class KisBackingBudgetReservation;
class KisBackingBudgetWaiter;
class KisPageOwnerDomainAdmission;
class KisPageOwnerLedger;
struct KisPageRetirementRecords;
enum class KisBackingBudgetClass : quint8;

struct KisBackingClassChange
{
    KisReplicaHandle replica;
    KisBackingBudgetClass before;
    KisBackingBudgetClass after;
};

/**
 * One-shot owner reservation for a frozen set of backing-class changes.
 * Destruction cancels the budget reservation and releases the physical claims.
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

    friend class KisPageOwnerLedger;
};

/** Prepared accounting only; does not authorize a physical or logical write.
 * The ledger and its budget must outlive the reservation, as for class changes.
 */
class KRITAIMAGE_EXPORT KisBackingHandoffReservation final
{
public:
    KisBackingHandoffReservation() = default;
    KisBackingHandoffReservation(KisBackingHandoffReservation &&) noexcept = default;
    KisBackingHandoffReservation &operator=(KisBackingHandoffReservation &&) noexcept = default;
    bool isValid() const { return m_change.isValid(); }
    void release() noexcept { m_change.release(); }
private:
    explicit KisBackingHandoffReservation(KisBackingClassChangeReservation change)
        : m_change(std::move(change)) {}
    KisBackingClassChangeReservation m_change;
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

    bool configure(const std::shared_ptr<KisCompletionRegistry> &completions,
                   QString *error = nullptr);
    void attachBackingBudget(KisBackingBudgetController &budget);
    bool registerBacking(const KisReplicaHandle &replica,
                         KisBackingBudgetReservation &reservation,
                         KisBackingBudgetClass budgetClass,
                         QString *error = nullptr,
                         KisPageRetirementRecordPointer *preparedRetirement = nullptr);
    KisPageRetirementRecordPointer takeRetirementRecord(const KisReplicaHandle &replica);
    KisBackingBudgetClass backingClass(const KisReplicaHandle &replica) const;
    bool reclassifyBacking(const KisReplicaHandle &replica,
                           KisBackingBudgetClass budgetClass,
                           QString *error = nullptr);
    bool reclassifyBacking(const KisReplicaHandle &replica,
                           KisBackingBudgetClass budgetClass,
                           KisBackingBudgetReservation &reservation,
                           QString *error = nullptr);
    // Cold budget admission only. The consumer must revalidate the exact
    // physical/class state using reclassifyBacking before calling its provider.
    KisPageReadinessStatus watchRetirementBudget(const KisReplicaHandle &replica,
        KisPageReadinessCallback notify, KisBackingBudgetWaiter *waiter);
    bool retainRetirementDebtReservation(const KisReplicaHandle &replica,
                                         KisBackingBudgetReservation &reservation,
                                         QString *error = nullptr);
    bool synchronizeBackingDomains(QString *error = nullptr);
    KisBackingClassChangeReservation prepareBackingChanges(
        KisPageSnapshotArray<KisBackingClassChange> changes,
        const KisPageSnapshotArray<KisPageTransitionEffect> &retirementEffects,
        QString *error = nullptr);
    void commitBackingChanges(KisBackingClassChangeReservation &&reservation) noexcept;
    // Freeze a sole, resident physical owner and reserve ActivePending plus
    // cancellation debt before taking the physical claim. The old handle stays
    // authoritative until commit. Cancel preparation before provider retag.
    KisBackingHandoffReservation prepareBackingHandoff(
        const KisReplicaHandle &source, const KisReplicaHandle &target,
        QString *error = nullptr);
    // Caller has installed logical detachment and consumed physical retag under
    // its admission gate. Reuses the existing index node; no provider call or
    // allocation. Keeps debt headroom in this ledger until ordinary publication
    // or retirement consumes it; callers must not own a second terminal token.
    bool commitBackingHandoff(
        KisBackingHandoffReservation &&reservation) noexcept;
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
    void releaseRetiredBacking(const KisReplicaHandle &replica) noexcept;

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
    // Fill the original record's preinstalled retirement operation only.
    // Callers must prepare it before invoking the provider; no result-time
    // allocation or fallback binding is permitted.
    bool bindRetirementOperation(KisPageOperationId operation,
                                 const KisReplicaOperation &result,
                                 QString *error = nullptr);
    KisVerifiedCompletion verifyProviderOperation(
        KisPageOperationId operation,
        QString *error = nullptr) const;
    bool releaseTerminalProviderOperation(KisPageOperationId operation,
                                          QString *error = nullptr);
    KisPageReadinessStatus watchProviderOperation(KisPageOperationId operation,
        KisPageReadinessCallback scheduleReady, KisPageReadinessSubscription *subscription) const;
    KisPageReadinessStatus watchCompletion(const KisCompletionTicket &ticket,
        KisPageReadinessCallback scheduleReady, KisPageReadinessSubscription *subscription) const;
    qsizetype providerOperationCount() const;
    // One locked O(1) snapshot; do not subtract two independently sampled
    // counts while retirement workers can bind/release operations.
    qsizetype publicationBlockingOperationCount() const;
    qsizetype sealedProofCount() const;
    bool ownsPreparedPageProof(const KisPreparedPageProof &proof) const;

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
    using BackingChanges = std::vector<KisBackingClassChange, KisMutationStorageAllocator<KisBackingClassChange>>;
    KisPageRetirementRecords takeShutdownRetirementRecords();
    // Original publication storage moves into the prepared ledger slot. The
    // allocator must belong to this ledger's backing budget.
    KisBackingClassChangeReservation prepareBackingChanges(
        BackingChanges changes, const KisPageTransitionEffect *effects,
        qsizetype count, QString *error = nullptr);
    KisBackingClassChangeReservation prepareBackingChangesImpl(
        BackingChanges changes,
        const KisPageTransitionEffect *effects, qsizetype count, QString *error);
    KisReplicaBackingFootprint observeBackingFootprint(
        const KisReplicaHandle &replica) const;
    KisPageOperationId prepareRetirementOperation(KisPageRetirementRecord &record);
    void cancelRetirementOperation(KisPageOperationId operation);
    bool reclassifyBackingImpl(const KisReplicaHandle &replica,
                               KisBackingBudgetClass budgetClass,
                               KisBackingBudgetReservation *reservation,
                               QString *error);
    void commitPreparedBackingChanges(quint64 cookie) noexcept;
    void cancelPreparedBackingChanges(quint64 cookie) noexcept;
    explicit KisPageOwnerLedger(const KisMutationStorageAllocator<KisPageOwnerLedger> &storage);
    class Private;
    struct PrivateReleaser { static void cleanup(Private *); };
    QScopedPointer<Private, PrivateReleaser> d;
    friend class KisPageStore;
    friend class KisPageStoreCpuMutationTest;
    friend class KisPageOwnerDomainAdmission;
    friend class KisBackingClassChangeReservation;
    friend class KisPageRetirementQueue;
    friend class KisPagePublicationCoordinator;
    friend class KisPageStoreReferenceTest;
};

#endif // KIS_PAGE_OWNER_LEDGER_H

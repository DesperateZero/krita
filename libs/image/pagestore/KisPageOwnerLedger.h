/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_OWNER_LEDGER_H
#define KIS_PAGE_OWNER_LEDGER_H

#include <QScopedPointer>
#include <QSharedPointer>

#include "KisCompletionRegistry.h"
#include "KisPageMetadataCoordinator.h"
#include "KisPageReplicaProvider.h"

class KisBackingBudgetController;
class KisBackingBudgetReservation;
enum class KisBackingBudgetClass : quint8;

struct KisBackingClassChange
{
    KisReplicaHandle replica;
    KisBackingBudgetClass before;
    KisBackingBudgetClass after;
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

    bool configure(const QSharedPointer<KisCompletionRegistry> &completions,
                   QString *error = nullptr);
    void attachBackingBudget(KisBackingBudgetController &budget);
    bool registerBacking(const KisReplicaHandle &replica,
                         KisBackingBudgetReservation &reservation,
                         KisBackingBudgetClass budgetClass,
                         QString *error = nullptr);
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
    KisBackingBudgetReservation prepareBackingChanges(
        QVector<KisBackingClassChange> *changes,
        const QVector<KisPageTransitionEffect> &retirementEffects,
        QString *error = nullptr);
    void commitBackingChanges(KisBackingBudgetReservation &&reservation,
                              const QVector<KisBackingClassChange> &changes) noexcept;
    /**
     * Pre-admit every owned replica emitted by a local metadata transition to
     * RetirementDebt. The returned cookie freezes the exact backing classes;
     * metadata must commit it only after its authoritative detach succeeds,
     * or cancel it on every rejection/stale retry.
     */
    bool prepareRetirementDebt(const QVector<KisPageTransitionEffect> &effects,
                               quint64 *cookie,
                               QString *error = nullptr);
    void commitRetirementDebt(quint64 cookie) noexcept;
    void cancelRetirementDebt(quint64 cookie) noexcept;
    void releaseRetiredBacking(const KisReplicaHandle &replica) noexcept;

    bool registerProvider(const QSharedPointer<KisPageReplicaProvider> &provider,
                          QString *error = nullptr);
    QSharedPointer<KisPageReplicaProvider> provider(
        KisReplicaProviderId provider,
        KisReplicaProviderEpoch epoch) const;
    QSharedPointer<KisPageReplicaProvider> providerFor(
        KisPageAccessRequirement access) const;

    KisPageRequestId nextRequestId();
    KisPageOperationId nextOperationId();
    KisPageLeaseId nextLeaseId();
    KisPageWriterToken nextWriterToken();

    bool bindProviderOperation(KisPageOperationId operation,
                               const KisReplicaOperation &result,
                               QString *error = nullptr);
    KisVerifiedCompletion consumeTerminalProviderOperation(
        KisPageOperationId operation,
        const KisReplicaOperation &result,
        QString *error = nullptr);
    // Owner-only classification: the replica has already been detached from
    // reachable metadata. A provider must not classify its own result here.
    bool bindRetirementOperation(KisPageOperationId operation,
                                 const KisReplicaOperation &result,
                                 QString *error = nullptr);
    KisVerifiedCompletion verifyProviderOperation(
        KisPageOperationId operation,
        QString *error = nullptr) const;
    bool releaseTerminalProviderOperation(KisPageOperationId operation,
                                          QString *error = nullptr);
    qsizetype providerOperationCount() const;
    // One locked O(1) snapshot; do not subtract two independently sampled
    // counts while retirement workers can bind/release operations.
    qsizetype publicationBlockingOperationCount() const;
    qsizetype sealedProofCount() const;

    // Caller protects the Prepared version/provider across validation (owner
    // gate or equivalent transaction + per-key mutation claims). The ledger
    // mutex is not held across provider code and does not provide that claim.
    bool sealPreparedPage(const KisPageMetadataCoordinator &metadata,
                          const KisPageVersion &version,
                          KisPageTransactionId transaction,
                          const KisPageAllocationDescriptor &descriptor,
                          const KisCompletionTicket &producerCompletion,
                          KisPreparedPageProof *proof,
                          QString *error = nullptr);
    bool validatePreparedPage(const KisPageMetadataCoordinator &metadata,
                              const KisPreparedPageProof &proof,
                              const KisPageAllocationDescriptor &descriptor,
                              QString *error = nullptr) const;
    bool revokePreparedPage(const KisPreparedPageProof &proof);

private:
    KisReplicaBackingFootprint observeBackingFootprint(
        const KisReplicaHandle &replica) const;
    bool reclassifyBackingImpl(const KisReplicaHandle &replica,
                               KisBackingBudgetClass budgetClass,
                               KisBackingBudgetReservation *reservation,
                               QString *error);
    bool bindProviderOperationImpl(KisPageOperationId operation,
                                   const KisReplicaOperation &result,
                                   bool detachedRetirement,
                                   QString *error);
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_PAGE_OWNER_LEDGER_H

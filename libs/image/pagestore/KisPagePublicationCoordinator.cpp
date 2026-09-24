/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPagePublicationCoordinator_p.h"
#include "KisPageStoreDiagnostics_p.h"
#include "KisPageWriteCoordinator_p.h"

#include <QScopeGuard>

#include <limits>
#include <utility>

namespace
{

struct PreparedDescriptorChange {
    KisPageVersion version;
    KisPageAllocationDescriptor descriptor;
};

template<typename PreparedPublication>
bool appendPublicationBackingChanges(
                                     const PreparedPublication &publication,
                                     const KisImageEpochRootSnapshot &currentRoot,
                                     const KisPageVersion &target,
                                     KisBackingBudgetClass targetBefore,
                                     KisBackingBudgetClass oldAfter,
                                     QVector<KisBackingClassChange> *changes)
{
    KisPageVersion current;
    if (!currentRoot.resolve(target.key, &current))
        return false;
    if (current == target)
        return true;
    const KisReplicaHandle oldAuthority = publication.backingAuthority(current);
    const KisReplicaHandle targetAuthority = publication.backingAuthority(target);
    if ((!oldAuthority.isValid() && !current.isDefaultPixel())
        || (!targetAuthority.isValid() && !target.isDefaultPixel()))
        return false;
    if (oldAuthority.isValid())
        changes->append({oldAuthority, KisBackingBudgetClass::Current, oldAfter});
    if (targetAuthority.isValid())
        changes->append({targetAuthority, targetBefore, KisBackingBudgetClass::Current});
    return true;
}

template<typename Publication, typename RootCandidate, typename Lock>
void discardPreparedCandidates(Publication &publication,
                               RootCandidate &rootCandidate,
                               Lock &ownerLock)
{
    ownerLock.unlock();
    publication = {};
    rootCandidate = {};
    ownerLock.relock();
}

} // namespace

bool KisPagePublicationCoordinator::ensureVirtualDefaultLocked(
    const KisPageVersion &version, const KisSurfaceEpochState &surface, QString *error)
{
    if (!version.isValid() || !version.isDefaultPixel() || !surface.isValid()
        || !(surface.surface == version.key.surface)
        || surface.defaultPixelRevision != version.defaultPixelRevision) {
        KisPageStoreDetail::setError(error, QStringLiteral("virtual default identity is invalid"));
        return false;
    }
    const KisPageAllocationDescriptor descriptor = surface.allocationDescriptor();
    if (!descriptor.isValid())
        return false;
    KisPageStateSnapshot page;
    if (m_metadata.versionSnapshot(version, &page)) {
        if (!page.findVersion(version)) {
            KisPageTransition attach;
            attach.kind = KisPageTransitionKind::AttachHistoricalDefault;
            attach.version = version;
            const auto result = m_metadata.applyOwner(version.key, attach);
            if (!result.accepted) {
                KisPageStoreDetail::setError(error, result.rejectionReason);
                return false;
            }
        }
    } else {
        // A first access through an older transaction/view must not turn its
        // default revision into today's published head of a virgin coordinate.
        const auto current = m_epochs.captureCommittedRoot();
        KisPageVersion head;
        KisSurfaceEpochState currentSurface;
        if (!current.resolve(version.key, &head) || !head.isDefaultPixel()
            || !current.surfaceState(version.key.surface, &currentSurface))
            return false;
        KisPageVersionStateSnapshot implicit;
        implicit.version = head;
        implicit.publication = KisPagePublicationState::Published;
        page.key = version.key;
        page.publishedEpoch = current.epoch();
        page.publishedGeneration = head.generation;
        page.publishedDefaultPixelRevision = head.defaultPixelRevision;
        page.nextGeneration = KisPageGeneration{head.generation.value + 1};
        page.versions.append(implicit);
        if (!m_metadata.registerPage(page, error))
            return false;
        putDescriptorLocked(head, currentSurface.allocationDescriptor());
        if (!(head == version))
            return ensureVirtualDefaultLocked(version, surface, error);
    }
    putDescriptorLocked(version, descriptor);
    KisPageStoreDetail::setError(error, {});
    return true;
}

class KisPagePublicationCoordinator::KisPreparedMutationCommit::Data
{
public:
    KisPagePublicationCoordinator *owner = nullptr;
    KisPageTransaction transaction;
    KisPageMetadataCoordinator::PreparedPublication metadata;
    KisImageEpochReferenceModel::PreparedCommit epoch;
    KisImageEpochReferenceModel::PreparedRootReservation restoreEpoch;
    QVector<PreparedDescriptorChange> descriptorChanges;
    KisCompletionTicket completion;
    QVector<KisPageTransitionEffect> publicationRetirements;
    KisBackingClassChangeReservation backingReservation;
    KisPageMetadataCoordinator::DeferredPublicationCleanup metadataCleanup;
    KisImageEpochCommitResult result;
};

KisPagePublicationCoordinator::KisPreparedMutationCommit::KisPreparedMutationCommit() = default;

KisPagePublicationCoordinator::KisPreparedMutationCommit::~KisPreparedMutationCommit()
{
    cancel();
}

KisPagePublicationCoordinator::KisPreparedMutationCommit::KisPreparedMutationCommit(
    KisPreparedMutationCommit &&) noexcept = default;

KisPagePublicationCoordinator::KisPreparedMutationCommit &
KisPagePublicationCoordinator::KisPreparedMutationCommit::operator=(KisPreparedMutationCommit &&other) noexcept
{
    if (this != &other) {
        cancel();
        data = std::move(other.data);
    }
    return *this;
}

bool KisPagePublicationCoordinator::KisPreparedMutationCommit::isValid() const
{
    return data && data->owner && data->metadata.isValid()
        && (data->epoch.isValid() != data->restoreEpoch.isValid()) && data->completion.isValid();
}

bool KisPagePublicationCoordinator::KisPreparedMutationCommit::tryInstall()
{
    if (!isValid()) {
        cancel();
        return false;
    }

    Data &candidate = *data;
    KisPagePublicationCoordinator &owner = *candidate.owner;
    const KisImageEpochReferenceModel::InstallMetadataFunction installMetadata = [](void *context,
                                                                                    KisImageEpochId epoch) {
        auto &prepared = *static_cast<Data *>(context);
        KisPagePublicationCoordinator &coordinator = *prepared.owner;
        if (!coordinator.m_metadata.installPublication(std::move(prepared.metadata),
                                                       prepared.transaction,
                                                       epoch,
                                                       &prepared.publicationRetirements,
                                                       nullptr,
                                                       &prepared.metadataCleanup)) {
            return false;
        }
        for (const PreparedDescriptorChange &change : std::as_const(prepared.descriptorChanges)) {
            coordinator.putDescriptorLocked(change.version, change.descriptor);
        }
        coordinator.m_statistics.descriptorInstallations += quint64(prepared.descriptorChanges.size());
        return true;
    };
    candidate.result = candidate.restoreEpoch.isValid()
        ? owner.m_epochs.installRestore(std::move(candidate.restoreEpoch), &candidate, installMetadata)
        : owner.m_epochs.installCommit(std::move(candidate.epoch), &candidate, installMetadata);

    if (candidate.result.isCommitted()) {
        owner.m_owner.commitBackingChanges(std::move(candidate.backingReservation));
        ++owner.m_statistics.installedMutationCommits;
    } else {
        candidate.backingReservation.release();
        ++owner.m_statistics.cancelledMutationCommits;
    }
    candidate.owner = nullptr;
    return candidate.result.isCommitted();
}

void KisPagePublicationCoordinator::KisPreparedMutationCommit::cancel() noexcept
{
    if (!data || data->result.isCommitted()) {
        return;
    }
    KisPagePublicationCoordinator *owner = data->owner;
    data->metadata = {};
    data->epoch = {};
    data->restoreEpoch = {};
    data->backingReservation.release();
    data->owner = nullptr;
    if (owner)
        ++owner->m_statistics.cancelledMutationCommits;
}

KisPagePublicationCoordinator::KisPagePublicationCoordinator(
    KisImageEpochReferenceModel &epochs,
    KisPageMetadataCoordinator &metadata,
    KisPageOwnerLedger &owner,
    KisPageRetirementQueue &retirementQueue,
    KisPageHistoryCollector &history,
    KisCompletionTicket &readyHostCompletion,
    qsizetype &activeProviderCalls,
    bool &operational,
    bool &backgroundReclamation,
    void *ownerContext,
    TransactionHasMutationActivity transactionHasMutationActivity,
    PageWriteClaimed pageWriteClaimed,
    MutationPreparation beginMutationPreparation,
    MutationPreparation endMutationPreparation,
    DisposeMetadataCleanup disposeMetadataCleanup,
    RestoreIsIdle restoreIsIdle,
    PrepareAbort prepareAbort)
    : m_epochs(epochs)
    , m_metadata(metadata)
    , m_owner(owner)
    , m_retirementQueue(retirementQueue)
    , m_history(history)
    , m_readyHostCompletion(readyHostCompletion)
    , m_activeProviderCalls(activeProviderCalls)
    , m_operational(operational)
    , m_backgroundReclamation(backgroundReclamation)
    , m_ownerContext(ownerContext)
    , m_transactionHasMutationActivity(transactionHasMutationActivity)
    , m_pageWriteClaimed(pageWriteClaimed)
    , m_beginMutationPreparation(beginMutationPreparation)
    , m_endMutationPreparation(endMutationPreparation)
    , m_disposeMetadataCleanup(disposeMetadataCleanup)
    , m_restoreIsIdle(restoreIsIdle)
    , m_prepareAbort(prepareAbort)
{
    Q_ASSERT(m_ownerContext);
    Q_ASSERT(m_transactionHasMutationActivity);
    Q_ASSERT(m_pageWriteClaimed);
    Q_ASSERT(m_beginMutationPreparation);
    Q_ASSERT(m_endMutationPreparation);
    Q_ASSERT(m_disposeMetadataCleanup);
    Q_ASSERT(m_restoreIsIdle);
    Q_ASSERT(m_prepareAbort);
}

bool KisPagePublicationCoordinator::resolveSurfaceLocked(KisSurfaceId surface,
                                                         const KisPageReadView &view,
                                                         KisSurfaceEpochState *state) const
{
    if (!m_operational || !surface.isValid() || !state || !m_epochs.surfaceState(surface, view, state)) {
        return false;
    }
    if (view.kind == KisPageReadViewKind::TransactionOverlay) {
        const auto prepared = m_preparedTransactions.constFind(view.transaction.value);
        if (prepared != m_preparedTransactions.constEnd()) {
            for (const KisSurfaceEpochChange &change : prepared->surfaceChanges) {
                if (change.after.surface == surface) {
                    *state = change.after;
                    break;
                }
            }
        }
    }
    return true;
}

bool KisPagePublicationCoordinator::hasActiveTransactionLocked(
    const KisPageTransaction &transaction) const
{
    if (!m_operational || !transaction.isValid())
        return false;
    const auto snapshot = m_epochs.transaction(transaction.id);
    return snapshot.transaction == transaction && snapshot.isActive();
}

void KisPagePublicationCoordinator::retireEffectsUnlocked(
    QVector<KisPageTransitionEffect> effects, QMutexLocker<QMutex> &ownerLock)
{
    if (effects.isEmpty()) return;
    ++m_activeProviderCalls;
    ownerLock.unlock();
    m_retirementQueue.retireEffects(effects, m_backgroundReclamation);
    effects.clear();
    ownerLock.relock();
    --m_activeProviderCalls;
}

bool KisPagePublicationCoordinator::resolveVersionLocked(const KisPageKey &key,
                                                         const KisPageReadView &view,
                                                         KisPageVersion *version) const
{
    if (!m_operational || !version || !key.isValid() || !view.isValidFor(key)
        || !m_epochs.resolve(key, view, version)) {
        return false;
    }
    if (view.kind != KisPageReadViewKind::TransactionOverlay)
        return true;
    const auto prepared = m_preparedTransactions.constFind(view.transaction.value);
    if (prepared != m_preparedTransactions.constEnd()) {
        const auto proof = prepared->proofs.constFind(key);
        if (proof != prepared->proofs.constEnd()) {
            *version = proof->authority.version;
            return true;
        }
    }
    if (version->isDefaultPixel()
        || (prepared != m_preparedTransactions.constEnd() && prepared->removals.contains(key))) {
        KisSurfaceEpochState surface;
        if (!resolveSurfaceLocked(key.surface, view, &surface))
            return false;
        *version = {key, KisPageGeneration{1}, surface.defaultPixelRevision};
    }
    return version->isValid();
}

bool KisPagePublicationCoordinator::stageSurfaceDefaultPixelLocked(const KisPageTransaction &transaction,
                                                                   KisSurfaceId surface,
                                                                   const QByteArray &pixel,
                                                                   QString *error)
{
    const auto overlay = KisPageReadView::transactionOverlay(transaction.id);
    KisSurfaceEpochState after;
    if (!resolveSurfaceLocked(surface, overlay, &after) || pixel.size() != qsizetype(after.format.pixelStride)) {
        KisPageStoreDetail::setError(error, QStringLiteral("default pixel surface or size is invalid"));
        return false;
    }
    if (after.format.defaultPixel != pixel) {
        const auto highWater = defaultRevisionHighWaterLocked(surface);
        if (highWater == std::numeric_limits<quint64>::max()) {
            KisPageStoreDetail::setError(error, QStringLiteral("default pixel revision exhausted"));
            return false;
        }
        after.format.defaultPixel = pixel;
        after.defaultPixelRevision = highWater + 1;
    }
    return stageSurfaceMetadataLocked(transaction, after, error);
}

bool KisPagePublicationCoordinator::stageSurfaceMetadataLocked(const KisPageTransaction &transaction,
                                                               const KisSurfaceEpochState &after,
                                                               QString *error)
{
    if (m_transactionHasMutationActivity(m_ownerContext, transaction.id) || isPreparingCommitLocked(transaction.id)) {
        KisPageStoreDetail::setError(error,
                            QStringLiteral("finish mutation preparation and writes before changing surface metadata"));
        return false;
    }
    if (!after.isValid() || !hasActiveTransactionLocked(transaction)) {
        KisPageStoreDetail::setError(error, QStringLiteral("surface metadata transaction is invalid or unavailable"));
        return false;
    }

    KisSurfaceEpochState before;
    KisSurfaceEpochChange change;
    const auto baseView = KisPageReadView::transactionBase(transaction.id);
    if (!m_epochs.surfaceState(after.surface, baseView, &before)) {
        KisPageStoreDetail::setError(error, QStringLiteral("surface metadata is absent from the transaction base epoch"));
        return false;
    }
    change.before = before;
    change.after = after;
    const auto overlay = KisPageReadView::transactionOverlay(transaction.id);
    KisSurfaceEpochState staged;
    if (resolveSurfaceLocked(after.surface, overlay, &staged) && staged == after) {
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    if (!change.isValid()) {
        KisPageStoreDetail::setError(error,
                            QStringLiteral("surface metadata change violates immutable layout or revision ordering"));
        return false;
    }

    QVector<KisSurfaceEpochChange> &changes =
        m_preparedTransactions[transaction.id.value].surfaceChanges;
    KisSurfaceEpochChange *existing = nullptr;
    for (KisSurfaceEpochChange &candidate : changes) {
        if (!(candidate.after.surface == after.surface))
            continue;
        if (candidate.after == after) {
            KisPageStoreDetail::setError(error, {});
            return true;
        }
        KisSurfaceEpochChange incremental;
        incremental.before = candidate.after;
        incremental.after = after;
        if (!incremental.isValid()) {
            KisPageStoreDetail::setError(error, QStringLiteral("surface already has a competing staged metadata change"));
            return false;
        }
        existing = &candidate;
        break;
    }
    const auto &previous = existing ? existing->after : before;
    if (previous.defaultPixelRevision != after.defaultPixelRevision) {
        auto &highWater = defaultRevisionHighWaterLocked(after.surface);
        if (after.defaultPixelRevision <= highWater) {
            KisPageStoreDetail::setError(error, QStringLiteral("default pixel revision must be fresh across abort and restore"));
            return false;
        }
        highWater = after.defaultPixelRevision;
    }
    if (existing)
        existing->after = after;
    else
        changes.append(change);
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPagePublicationCoordinator::stagePageRemovalLocked(const KisPageTransaction &transaction,
                                                           const KisPageVersion &observed,
                                                           QString *error,
                                                           QMutexLocker<QMutex> &ownerLock)
{
    const KisPageKey &key = observed.key;
    if (m_pageWriteClaimed(m_ownerContext, key)
        || isPreparingCommitLocked(transaction.id)) {
        KisPageStoreDetail::setError(error, QStringLiteral("page removal conflicts with a mutation claim"));
        return false;
    }
    const KisImageEpochRootSnapshot baseRoot = m_epochs.root(transaction.baseEpoch);
    if (!key.isValid() || !hasActiveTransactionLocked(transaction) || !baseRoot.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("page removal transaction is invalid or unavailable"));
        return false;
    }
    const auto overlay = KisPageReadView::transactionOverlay(transaction.id);
    KisPageVersion current;
    if (!resolveVersionLocked(key, overlay, &current) || !(current == observed)) {
        KisPageStoreDetail::setError(error, QStringLiteral("conditional page removal observed a superseded version"));
        return false;
    }
    m_beginMutationPreparation(m_ownerContext, transaction.id);
    const auto preparationClaim = qScopeGuard([&] {
        m_endMutationPreparation(m_ownerContext, transaction.id);
    });

    QVector<KisPageTransitionEffect> retirementEffects;
    const auto *proofs = findProofsLocked(transaction.id);
    const KisPreparedPageProof proof = proofs ? proofs->value(key) : KisPreparedPageProof{};
    if (proof.isValid()) {
        KisPageTransition transition;
        transition.kind = KisPageTransitionKind::AbortTransaction;
        transition.transaction = transaction.id;
        const KisPageTransitionResult result = m_metadata.applyOwner(key, transition);
        if (!result.accepted || !revokePreparedProofLocked(proof)) {
            KisPageStoreDetail::setError(error,
                                result.rejectionReason.isEmpty()
                                    ? QStringLiteral("prepared page removal lost its owner proof")
                                    : result.rejectionReason);
            return false;
        }
        retirementEffects = result.effects;
    }

    if (baseRoot.containsPage(key))
        setRemovalLocked(transaction.id, key, true);
    retireEffectsUnlocked(std::move(retirementEffects), ownerLock);
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPreparedPageSet KisPagePublicationCoordinator::preparedPagesLocked(const KisPageTransaction &transaction) const
{
    KisPreparedPageSet result;
    if (!hasActiveTransactionLocked(transaction)) {
        return result;
    }
    result = transactionDeltaLocked(transaction.id);
    return result.isValid() ? result : KisPreparedPageSet();
}

bool KisPagePublicationCoordinator::preparedPageExtentLocked(const KisPageTransaction &transaction,
                                                             KisSurfaceId surface,
                                                             QRect *extent,
                                                             QString *error,
                                                             QMutexLocker<QMutex> &ownerLock) const
{
    if (extent)
        *extent = {};
    const auto overlay = KisPageReadView::transactionOverlay(transaction.id);
    KisSurfaceEpochState state;
    if (!extent || !hasActiveTransactionLocked(transaction)
        || !resolveSurfaceLocked(surface, overlay, &state)) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared extent transaction/surface is unavailable"));
        return false;
    }
    KisPreparedPageSet delta = transactionDeltaLocked(transaction.id);
    delta.surfaceChanges.clear();
    const auto root = m_epochs.captureCommittedRoot();
    ownerLock.unlock();
    const bool valid = root.contentExtentAfterDelta(surface, state.logicalPageExtent, delta, extent);
    ownerLock.relock();
    if (!valid) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared extent exceeds QRect range"));
        return false;
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisImageEpochCommitTicket KisPagePublicationCoordinator::commitLocked(const KisPageTransaction &transaction,
                                                                      const KisPreparedPageSet &preparedPages,
                                                                      KisRetainedImageEpochSnapshot *retainedAfter,
                                                                      const KisPageStore *diagnosticOwner,
                                                                      QMutexLocker<QMutex> &ownerLock)
{
    using Phase = KisPageStoreDiagnosticPhase;
    const quint64 pageWork = quint64(preparedPages.proofs.size() + preparedPages.removedPages.size());
    KisPageStoreDiagnosticTimer diagnostic(diagnosticOwner, Phase::CommitOwnerWait, pageWork);
    if (retainedAfter)
        *retainedAfter = {};
    if (m_transactionHasMutationActivity(m_ownerContext, transaction.id) || isPreparingCommitLocked(transaction.id)) {
        return {};
    }
    diagnostic.next(Phase::CommitProofValidation, quint64(preparedPages.proofs.size()));
    if (!hasActiveTransactionLocked(transaction) || !preparedPages.isValid()
        || !(preparedPages.transaction == transaction.id)) {
        return {};
    }
    QHash<quint64, const KisSurfaceEpochChange *> surfaceChanges;
    bool changesSurfaceDefault = false;
    surfaceChanges.reserve(preparedPages.surfaceChanges.size());
    for (const KisSurfaceEpochChange &change : preparedPages.surfaceChanges) {
        surfaceChanges.insert(change.after.surface.value, &change);
        if (change.before.defaultPixelRevision != change.after.defaultPixelRevision)
            changesSurfaceDefault = true;
    }
    QSet<KisPageKey> preparedKeys;
    preparedKeys.reserve(preparedPages.proofs.size());
    for (const KisPreparedPageProof &proof : preparedPages.proofs)
        preparedKeys.insert(proof.authority.version.key);

    QVector<KisPageAllocationDescriptor> proofDescriptors;
    proofDescriptors.reserve(preparedPages.proofs.size());
    {
        const auto found = m_preparedTransactions.constFind(transaction.id.value);
        if (found == m_preparedTransactions.constEnd())
            return {};
        const PreparedTransactionState &owned = found.value();
        if (owned.proofs.size() != preparedPages.proofs.size()
            || owned.surfaceChanges.size() != preparedPages.surfaceChanges.size()
            || owned.removals.size() != preparedPages.removedPages.size()) {
            return {};
        }
        for (const KisPreparedPageProof &proof : preparedPages.proofs) {
            const auto ownedProof = owned.proofs.constFind(proof.authority.version.key);
            const bool found = ownedProof != owned.proofs.constEnd() && proof == ownedProof.value();
            KisPageAllocationDescriptor descriptor;
            if (!found || !descriptorLocked(proof.authority.version, &descriptor))
                return {};
            proofDescriptors.append(descriptor);
        }
        for (const KisSurfaceEpochChange &change : owned.surfaceChanges) {
            const auto supplied = surfaceChanges.constFind(change.after.surface.value);
            if (supplied == surfaceChanges.constEnd() || !(change == *supplied.value()))
                return {};
        }
        for (const KisPageKey &key : preparedPages.removedPages) {
            if (!owned.removals.contains(key))
                return {};
        }
    }

    m_preparingCommits.insert(transaction.id.value);
    const auto commitClaim = qScopeGuard([&] {
        m_preparingCommits.remove(transaction.id.value);
    });
    ownerLock.unlock();
    diagnostic.next(Phase::CommitProviderValidation, quint64(proofDescriptors.size()));
    bool proofsValid = true;
    for (qsizetype i = 0; i < preparedPages.proofs.size(); ++i) {
        if (!m_owner.validatePreparedPage(m_metadata, preparedPages.proofs.at(i), proofDescriptors.at(i))) {
            proofsValid = false;
            break;
        }
    }
    proofDescriptors = {};
    diagnostic.next(Phase::CommitEpochDeltaPrepare, pageWork);
    const bool deltaPrepared = proofsValid && m_epochs.prepare(preparedPages);
    diagnostic.next(Phase::CommitInputOwnerWait, pageWork);
    ownerLock.relock();
    if (!deltaPrepared)
        return {};

    for (;;) {
        diagnostic.next(Phase::CommitDefaultRemovalPreparation, quint64(preparedPages.removedPages.size()));
        QVector<PreparedDescriptorChange> descriptorChanges;
        const KisImageEpochRootSnapshot currentRoot = m_epochs.captureCommittedRoot();
        if (!currentRoot.isValid() || currentRoot.epoch().value == std::numeric_limits<quint64>::max()) {
            return {};
        }
        const KisImageEpochId prospectiveEpoch{currentRoot.epoch().value + 1};
        QVector<KisPageTransition> publicationTransitions;
        publicationTransitions.reserve(preparedPages.proofs.size() + preparedPages.removedPages.size());
        const quint64 directoryRevision = m_metadata.pageRegistrationCount();
        const QVector<KisPageStateSnapshot> registeredPages =
            changesSurfaceDefault ? m_metadata.publicationHeaders() : QVector<KisPageStateSnapshot>();
        for (const KisPageStateSnapshot &page : registeredPages) {
            const auto surfaceChange = surfaceChanges.constFind(page.key.surface.value);
            if (surfaceChange == surfaceChanges.constEnd()
                || surfaceChange.value()->before.defaultPixelRevision
                    == surfaceChange.value()->after.defaultPixelRevision)
                continue;
            if (preparedKeys.contains(page.key))
                continue;
            const KisSurfaceEpochChange &change = *surfaceChange.value();
            const KisPageVersion currentVersion{page.key, page.publishedGeneration, page.publishedDefaultPixelRevision};
            if (!currentVersion.isDefaultPixel())
                continue;

            PreparedDescriptorChange replacement;
            replacement.version = {page.key, page.publishedGeneration, change.after.defaultPixelRevision};
            replacement.descriptor = change.after.allocationDescriptor();
            KisPageStateSnapshot inputs;
            if (!m_metadata.publicationSnapshot(page.key, replacement.version, &inputs)) {
                return {};
            }
            const KisPageVersionStateSnapshot *published = inputs.findVersion(currentVersion);
            if (!published || (!published->authority.isValid() && !published->isVirtualDefault())
                || !replacement.descriptor.isValid()) {
                return {};
            }
            const KisPageVersionStateSnapshot *existingReplacement =
                inputs.findVersion(replacement.version);
            KisReplicaHandle replacementReplica;
            if (existingReplacement) {
                KisPageAllocationDescriptor existingDescriptor;
                if (existingReplacement->publication != KisPagePublicationState::Historical
                    || (!existingReplacement->authority.isValid() && !existingReplacement->isVirtualDefault())
                    || !descriptorLocked(replacement.version, &existingDescriptor)
                    || !(existingDescriptor == replacement.descriptor)) {
                    return {};
                }
                replacementReplica = existingReplacement->authority;
            }

            KisPageTransition transition;
            transition.kind = KisPageTransitionKind::ReplaceDefaultPixel;
            transition.version = replacement.version;
            transition.target = replacementReplica;
            transition.imageEpoch = prospectiveEpoch;
            publicationTransitions.append(transition);
            descriptorChanges.append(replacement);
        }

        const auto transactionView = KisPageReadView::transactionOverlay(transaction.id);
        for (const KisPageKey &key : preparedPages.removedPages) {
            KisPageStateSnapshot page;
            KisSurfaceEpochState surface;
            if (!m_metadata.publicationSnapshot(key, {}, &page)
                || !m_epochs.surfaceState(key.surface, transactionView, &surface)) {
                return {};
            }
            const auto surfaceChange = surfaceChanges.constFind(key.surface.value);
            if (surfaceChange != surfaceChanges.constEnd())
                surface = surfaceChange.value()->after;
            const KisPageVersion target{key, KisPageGeneration{1}, surface.defaultPixelRevision};
            const KisPageVersion currentVersion{key, page.publishedGeneration, page.publishedDefaultPixelRevision};
            const KisPageVersionStateSnapshot *published = page.findVersion(currentVersion);
            if (!published || (!published->authority.isValid() && !published->isVirtualDefault())) {
                return {};
            }
            const KisPageAllocationDescriptor descriptor = surface.allocationDescriptor();
            if (!descriptor.isValid())
                return {};
            KisPageTransition transition;
            transition.kind = KisPageTransitionKind::RestoreCommittedVersion;
            transition.version = target;
            transition.imageEpoch = prospectiveEpoch;
            publicationTransitions.append(transition);
            descriptorChanges.append({target, descriptor});
        }

        diagnostic.next(Phase::CommitTransitionInputs,
                        quint64(preparedPages.proofs.size() + publicationTransitions.size()));
        for (const KisPreparedPageProof &proof : preparedPages.proofs) {
            KisPageTransition transition;
            transition.kind = KisPageTransitionKind::CommitTransaction;
            transition.version = proof.authority.version;
            transition.transaction = transaction.id;
            transition.imageEpoch = prospectiveEpoch;
            publicationTransitions.append(transition);
        }
        const qsizetype descriptorAdditions = descriptorChanges.size();
        reserveDescriptorAdditionsLocked(descriptorAdditions);
        const quint64 descriptorRevision = m_descriptorRevision;
        ownerLock.unlock();
        const quint64 publicationTransitionCount = quint64(publicationTransitions.size());
        diagnostic.next(Phase::CommitTransitionPreflight, publicationTransitionCount);
        auto publication = m_metadata.preparePublication(transaction, prospectiveEpoch, publicationTransitions);
        publicationTransitions = {};
        diagnostic.next(Phase::CommitEpochPrepare, pageWork);
        KisImageEpochCommitResult candidateFailure;
        auto rootCandidate = m_epochs.prepareCommit(transaction, &candidateFailure);
        diagnostic.next(Phase::CommitPublishOwnerWait, pageWork);
        ownerLock.relock();
        diagnostic.next(Phase::CommitPublicationRevalidate, pageWork);
        const bool rootChanged = !(m_epochs.captureCommittedRoot().epoch() == currentRoot.epoch());
        const bool directoryChanged = changesSurfaceDefault && m_metadata.pageRegistrationCount() != directoryRevision;
        const bool descriptorChanged = descriptorAdditions != 0 && m_descriptorRevision != descriptorRevision;
        if (rootChanged || directoryChanged || descriptorChanged || publication.needsReprepare()) {
            m_statistics.rootRebases += rootChanged;
            m_statistics.directoryRepreparations += directoryChanged;
            m_statistics.descriptorRepreparations += descriptorChanged;
            m_statistics.metadataRepreparations += publication.needsReprepare();
            discardPreparedCandidates(publication, rootCandidate, ownerLock);
            continue;
        }
        if (!publication.isValid() || !rootCandidate.isValid()) {
            discardPreparedCandidates(publication, rootCandidate, ownerLock);
            return {};
        }
        diagnostic.next(Phase::CommitCompletion);
        const KisCompletionTicket commitCompletion = m_readyHostCompletion;
        if (!commitCompletion.isValid()) {
            discardPreparedCandidates(publication, rootCandidate, ownerLock);
            return {};
        }

        QVector<KisBackingClassChange> backingChanges;
        backingChanges.reserve(preparedPages.proofs.size() * 2 + descriptorChanges.size() * 2);
        bool backingInputsValid = true;
        for (const KisPreparedPageProof &proof : preparedPages.proofs) {
            backingInputsValid = appendPublicationBackingChanges(
                publication, currentRoot, proof.authority.version, KisBackingBudgetClass::ActivePending,
                KisBackingBudgetClass::RetainedHistory,
                &backingChanges);
            if (!backingInputsValid) break;
        }
        for (const PreparedDescriptorChange &target : std::as_const(descriptorChanges)) {
            if (!backingInputsValid) break;
            backingInputsValid = appendPublicationBackingChanges(
                publication, currentRoot, target.version,
                KisBackingBudgetClass::RetainedHistory, KisBackingBudgetClass::RetainedHistory,
                &backingChanges);
        }
        if (!backingInputsValid)
            return {};
        auto backingReservation = m_owner.prepareBackingChanges(
            std::move(backingChanges), publication.retirementEffects());
        if (!backingReservation.isValid())
            return {};

        KisPreparedMutationCommit preparedCommit;
        preparedCommit.data = std::make_unique<KisPreparedMutationCommit::Data>();
        preparedCommit.data->owner = this;
        preparedCommit.data->transaction = transaction;
        preparedCommit.data->metadata = std::move(publication);
        preparedCommit.data->epoch = std::move(rootCandidate);
        preparedCommit.data->backingReservation = std::move(backingReservation);
        preparedCommit.data->descriptorChanges = std::move(descriptorChanges);
        preparedCommit.data->completion = commitCompletion;
        ++m_statistics.preparedMutationCommits;
        Q_ASSERT(preparedCommit.isValid());

        diagnostic.next(Phase::CommitRoot, pageWork);
        diagnostic.next(Phase::CommitMetadataApply, publicationTransitionCount);
        const bool installed = preparedCommit.tryInstall();
        const bool metadataRejected =
            preparedCommit.data->result.status == KisImageEpochCommitStatus::Rejected;
        const KisImageEpochCommitResult committed = preparedCommit.data->result;
        auto metadataCleanup = std::move(preparedCommit.data->metadataCleanup);
        if (installed)
            diagnostic.next(Phase::CommitRootPublication, pageWork);
        if (committed.isCommitted() && retainedAfter) {
            *retainedAfter = m_epochs.retainSnapshot(committed.root.epoch());
        }
        ownerLock.unlock();
        diagnostic.next(Phase::CommitCleanup, pageWork);
        m_disposeMetadataCleanup(m_ownerContext, std::move(metadataCleanup));
        if (!installed)
            preparedCommit = {};
        ownerLock.relock();
        if (!committed.isCommitted()) {
            if (!metadataRejected)
                return {};
            ++m_statistics.metadataRepreparations;
            continue;
        }

        for (const KisPreparedPageProof &proof : preparedPages.proofs) {
            const bool revoked = revokePreparedProofLocked(proof);
            Q_ASSERT(revoked);
            Q_UNUSED(revoked);
        }
        m_preparedTransactions.remove(transaction.id.value);
        m_history.collectEpochBookkeepingLocked(!(transaction.baseEpoch == currentRoot.epoch()));
        QVector<KisPageKey> historyCandidates;
        historyCandidates.reserve(preparedPages.proofs.size() + preparedCommit.data->descriptorChanges.size());
        for (const KisPreparedPageProof &proof : preparedPages.proofs) {
            historyCandidates.append(proof.authority.version.key);
        }
        for (const PreparedDescriptorChange &change : std::as_const(preparedCommit.data->descriptorChanges)) {
            historyCandidates.append(change.version.key);
        }
        QVector<KisPageTransitionEffect> historyRetirements = std::move(preparedCommit.data->publicationRetirements);
        diagnostic.next(Phase::CommitHistoryCollect, quint64(historyCandidates.size()));
        historyRetirements += m_history.collectUnreachableLocked(historyCandidates);
        diagnostic.next(Phase::CommitProviderRetire, quint64(historyRetirements.size()));
        retireEffectsUnlocked(std::move(historyRetirements), ownerLock);
        ++m_committedTransactions;
        const KisImageEpochCommitTicket ticket{committed.root.epoch(), preparedCommit.data->completion};
        ownerLock.unlock();
        preparedCommit = {};
        ownerLock.relock();
        return ticket;
    }
}

KisImageEpochCommitTicket
KisPagePublicationCoordinator::restoreRetainedEpochLocked(const KisRetainedImageEpochSnapshot &retained,
                                                          const QVector<KisPageKey> *changedPages,
                                                          const KisPageStore *diagnosticOwner,
                                                          QMutexLocker<QMutex> &ownerLock)
{
    using Phase = KisPageStoreDiagnosticPhase;
    if (!m_operational || !m_epochs.validateRetainedSnapshot(retained) || !m_restoreIsIdle(m_ownerContext)) {
        return {};
    }
    const auto source = m_epochs.retainSnapshot(retained.snapshot.epoch);
    if (!source.isValid())
        return {};
    auto sourceClaim = qScopeGuard([&] {
        m_epochs.releaseSnapshot(source.token);
    });
    const auto expectedHead = m_epochs.captureCommittedRoot().epoch();
    const KisImageEpochId nextEpoch{expectedHead.value + 1};
    if (!nextEpoch.isValid())
        return {};

    KisPageReadView retainedView;
    retainedView.kind = KisPageReadViewKind::CommittedEpoch;
    retainedView.epoch = source.snapshot.epoch;
    retainedView.retention = source.token;
    const auto retainedRoot = m_epochs.root(source.snapshot.epoch);
    const auto currentRoot = m_epochs.captureCommittedRoot();
    QSet<quint64> changedDefaults;
    for (const auto &surface : retainedRoot.surfaces()) {
        KisSurfaceEpochState currentSurface;
        if (!currentRoot.surfaceState(surface.surface, &currentSurface)) {
            return {};
        }
        if (currentSurface.defaultPixelRevision != surface.defaultPixelRevision) {
            changedDefaults.insert(surface.surface.value);
        }
    }
    QVector<KisPageKey> historyCandidates;
    KisImageEpochCommitResult restored;
    KisCompletionTicket completion;
    for (;;) {
        const quint64 directoryRevision = m_metadata.pageRegistrationCount();
        QVector<KisPageVersion> restoreTargets;
        if (changedPages) {
            QSet<KisPageKey> seen;
            restoreTargets.reserve(changedPages->size());
            for (const KisPageKey &key : *changedPages) {
                if (!key.isValid() || seen.contains(key))
                    continue;
                seen.insert(key);
                KisPageVersion target;
                if (!m_epochs.resolve(key, retainedView, &target))
                    return {};
                restoreTargets.append(target);
            }
        } else {
            if (!retainedRoot.isValid())
                return {};
            restoreTargets = retainedRoot.manifest();
        }
        if (!changedPages || !changedDefaults.isEmpty()) {
            QSet<KisPageKey> targeted;
            targeted.reserve(restoreTargets.size());
            for (const auto &target : std::as_const(restoreTargets)) {
                targeted.insert(target.key);
            }
            const QVector<KisPageStateSnapshot> registeredPages = m_metadata.publicationHeaders();
            for (const KisPageStateSnapshot &page : registeredPages) {
                if (targeted.contains(page.key))
                    continue;
                if (changedPages
                    && (!changedDefaults.contains(page.key.surface.value)
                        || !KisPageVersion{page.key, page.publishedGeneration, page.publishedDefaultPixelRevision}
                                .isDefaultPixel())) {
                    continue;
                }
                KisPageVersion target;
                if (!m_epochs.resolve(page.key, retainedView, &target)) {
                    return {};
                }
                restoreTargets.append(target);
            }
        }

        QVector<KisPageTransition> restoreTransitions;
        QVector<PreparedDescriptorChange> restoredDefaultVersions;
        restoreTransitions.reserve(restoreTargets.size());
        restoredDefaultVersions.reserve(restoreTargets.size());
        for (const KisPageVersion &version : std::as_const(restoreTargets)) {
            KisPageStateSnapshot page;
            if (!m_metadata.publicationSnapshot(version.key, version, &page)) {
                return {};
            }
            if (!page.findVersion(version) && version.isDefaultPixel()) {
                KisSurfaceEpochState surface;
                if (!m_epochs.surfaceState(version.key.surface, retainedView, &surface)
                    || surface.defaultPixelRevision != version.defaultPixelRevision
                    || !surface.allocationDescriptor().isValid()) {
                    return {};
                }
                restoredDefaultVersions.append({version, surface.allocationDescriptor()});
            }
            KisPageTransition transition;
            transition.kind = KisPageTransitionKind::RestoreCommittedVersion;
            transition.version = version;
            transition.imageEpoch = nextEpoch;
            restoreTransitions.append(transition);
        }

        const qsizetype descriptorAdditions = restoredDefaultVersions.size();
        reserveDescriptorAdditionsLocked(descriptorAdditions);
        const quint64 descriptorRevision = m_descriptorRevision;

        ownerLock.unlock();
        KisPageStoreDiagnosticTimer diagnostic(diagnosticOwner,
                                               Phase::RestoreTransitionPreflight,
                                               quint64(restoreTransitions.size()));
        const quint64 restoreTransitionCount = quint64(restoreTransitions.size());
        auto publication = m_metadata.prepareRestoration(nextEpoch, restoreTransitions);
        restoreTransitions = {};
        KisImageEpochCommitResult candidateFailure;
        auto rootCandidate = m_epochs.prepareRestore(source, &candidateFailure);
        diagnostic.next(Phase::RestorePublishOwnerWait, restoreTransitionCount);
        ownerLock.relock();
        diagnostic.next(Phase::RestorePublication, restoreTransitionCount);
        if (!m_operational || (!publication.isValid() && !publication.needsReprepare()) || !rootCandidate.isValid()
            || !(m_epochs.captureCommittedRoot().epoch() == expectedHead) || m_epochs.activeTransactionCount() != 0) {
            discardPreparedCandidates(publication, rootCandidate, ownerLock);
            return {};
        }
        const bool directoryChanged = (!changedPages || !changedDefaults.isEmpty())
            && m_metadata.pageRegistrationCount() != directoryRevision;
        const bool descriptorChanged = descriptorAdditions != 0 && m_descriptorRevision != descriptorRevision;
        if (publication.needsReprepare() || directoryChanged || descriptorChanged) {
            m_statistics.restoreMetadataRepreparations += publication.needsReprepare();
            m_statistics.restoreDirectoryRepreparations += directoryChanged;
            discardPreparedCandidates(publication, rootCandidate, ownerLock);
            continue;
        }
        completion = m_readyHostCompletion;
        if (!completion.isValid()) {
            discardPreparedCandidates(publication, rootCandidate, ownerLock);
            return {};
        }

        QVector<KisBackingClassChange> backingChanges;
        backingChanges.reserve(restoreTargets.size() * 2);
        for (const KisPageVersion &target : std::as_const(restoreTargets)) {
            if (!appendPublicationBackingChanges(publication, currentRoot, target,
                                                 KisBackingBudgetClass::RetainedHistory,
                                                 KisBackingBudgetClass::RetainedHistory,
                                                 &backingChanges))
                return {};
        }
        auto backingReservation = m_owner.prepareBackingChanges(
            std::move(backingChanges), publication.retirementEffects());
        if (!backingReservation.isValid())
            return {};

        KisPreparedMutationCommit preparedCommit;
        preparedCommit.data = std::make_unique<KisPreparedMutationCommit::Data>();
        preparedCommit.data->owner = this;
        preparedCommit.data->metadata = std::move(publication);
        preparedCommit.data->restoreEpoch = std::move(rootCandidate);
        preparedCommit.data->backingReservation = std::move(backingReservation);
        preparedCommit.data->descriptorChanges = std::move(restoredDefaultVersions);
        preparedCommit.data->completion = completion;
        ++m_statistics.preparedMutationCommits;
        Q_ASSERT(preparedCommit.isValid());

        const bool installed = preparedCommit.tryInstall();
        const bool metadataRejected =
            preparedCommit.data->result.status == KisImageEpochCommitStatus::Rejected;
        restored = preparedCommit.data->result;
        auto metadataCleanup = std::move(preparedCommit.data->metadataCleanup);
        if (installed) {
            completion = preparedCommit.data->completion;
            historyCandidates.reserve(restoreTargets.size());
            for (const KisPageVersion &target : std::as_const(restoreTargets))
                historyCandidates.append(target.key);
        }
        ownerLock.unlock();
        m_disposeMetadataCleanup(m_ownerContext, std::move(metadataCleanup));
        preparedCommit = {};
        ownerLock.relock();
        if (restored.isCommitted()) {
            break;
        }
        if (!metadataRejected)
            return {};
        ++m_statistics.restoreMetadataRepreparations;
    }
    m_epochs.releaseSnapshot(source.token);
    sourceClaim.dismiss();
    m_history.collectEpochBookkeepingLocked();
    retireEffectsUnlocked(m_history.collectUnreachableLocked(historyCandidates), ownerLock);
    return {restored.root.epoch(), completion};
}

bool KisPagePublicationCoordinator::abortLocked(const KisPageTransaction &transaction, QMutexLocker<QMutex> &ownerLock)
{
    if (isPreparingCommitLocked(transaction.id))
        return false;
    if (!hasActiveTransactionLocked(transaction)) {
        return false;
    }

    m_preparingCommits.insert(transaction.id.value);
    const auto preparation = qScopeGuard([&] { m_preparingCommits.remove(transaction.id.value); });
    QVector<KisPageTransitionEffect> retirementEffects;
    // A later page can reject cancellation after earlier pages detached.
    // Keep the remaining transaction retryable, but never drop those effects.
    const auto retire = qScopeGuard([&] {
        retireEffectsUnlocked(std::move(retirementEffects), ownerLock);
    });
    if (!m_prepareAbort(m_ownerContext, transaction.id, &retirementEffects)) {
        return false;
    }

    while (const auto *remainingProofs = findProofsLocked(transaction.id)) {
        const KisPreparedPageProof proof = remainingProofs->cbegin().value();
        KisPageTransition transition;
        transition.kind = KisPageTransitionKind::AbortTransaction;
        transition.transaction = transaction.id;
        const KisPageTransitionResult result = m_metadata.applyOwner(proof.authority.version.key, transition);
        if (!result.accepted)
            return false;
        retirementEffects += result.effects;
        if (!revokePreparedProofLocked(proof))
            return false;
    }
    m_preparedTransactions.remove(transaction.id.value);
    if (!m_epochs.abort(transaction))
        return false;
    m_history.collectEpochBookkeepingLocked(!(transaction.baseEpoch == m_epochs.captureCommittedRoot().epoch()));
    return true;
}

bool KisPagePublicationCoordinator::isPreparingCommitLocked(KisPageTransactionId transaction) const
{
    return m_preparingCommits.contains(transaction.value);
}

const QHash<KisPageKey, KisPreparedPageProof> *
KisPagePublicationCoordinator::findProofsLocked(KisPageTransactionId transaction) const
{
    const auto found = m_preparedTransactions.constFind(transaction.value);
    return found == m_preparedTransactions.constEnd() || found->proofs.isEmpty() ? nullptr : &found->proofs;
}

KisPreparedPageSet
KisPagePublicationCoordinator::transactionDeltaLocked(KisPageTransactionId transaction) const
{
    KisPreparedPageSet result;
    result.transaction = transaction;
    const auto found = m_preparedTransactions.constFind(transaction.value);
    if (found != m_preparedTransactions.constEnd()) {
        result.proofs = found->proofs.values();
        result.surfaceChanges = found->surfaceChanges;
        result.removedPages = found->removals.values();
    }
    return result;
}

bool KisPagePublicationCoordinator::stagesRemovalLocked(
    KisPageTransactionId transaction, const KisPageKey &key) const
{
    const auto found = m_preparedTransactions.constFind(transaction.value);
    return found != m_preparedTransactions.constEnd() && found->removals.contains(key);
}

bool KisPagePublicationCoordinator::revokePreparedProofLocked(
    const KisPreparedPageProof &proof)
{
    auto transaction = m_preparedTransactions.find(proof.transaction.value);
    Q_ASSERT(transaction != m_preparedTransactions.end());
    if (transaction == m_preparedTransactions.end())
        return false;
    auto stored = transaction->proofs.find(proof.authority.version.key);
    Q_ASSERT(stored != transaction->proofs.end()
             && stored.value() == proof);
    if (stored == transaction->proofs.end()
        || !(stored.value() == proof)) {
        return false;
    }
    if (!m_owner.revokePreparedPage(proof))
        return false;
    transaction->proofs.erase(stored);
    if (transaction->isEmpty())
        m_preparedTransactions.erase(transaction);
    return true;
}

void KisPagePublicationCoordinator::installPreparedProofLocked(
    const KisPreparedPageProof &proof)
{
    Q_ASSERT(proof.isValid());
    auto &transaction = m_preparedTransactions[proof.transaction.value];
    transaction.removals.remove(proof.authority.version.key);
    transaction.proofs.insert(proof.authority.version.key, proof);
}

void KisPagePublicationCoordinator::setRemovalLocked(
    KisPageTransactionId transaction, const KisPageKey &key, bool removed)
{
    if (removed) {
        m_preparedTransactions[transaction.value].removals.insert(key);
    } else {
        auto transactionState = m_preparedTransactions.find(transaction.value);
        if (transactionState == m_preparedTransactions.end())
            return;
        transactionState->removals.remove(key);
        if (transactionState->isEmpty())
            m_preparedTransactions.erase(transactionState);
    }
}

quint64 &KisPagePublicationCoordinator::defaultRevisionHighWaterLocked(KisSurfaceId surface)
{
    return m_defaultRevisionHighWater[surface.value];
}

void KisPagePublicationCoordinator::putDescriptorLocked(const KisPageVersion &version,
                                                        const KisPageAllocationDescriptor &descriptor)
{
    auto found = m_descriptors.find(version);
    if (found == m_descriptors.end()) {
        m_descriptors.insert(version, descriptor);
        ++m_descriptorRevision;
    } else if (!(found.value() == descriptor)) {
        found.value() = descriptor;
        ++m_descriptorRevision;
    }
}

void KisPagePublicationCoordinator::removeDescriptorLocked(const KisPageVersion &version)
{
    if (m_descriptors.remove(version) != 0)
        ++m_descriptorRevision;
}

bool KisPagePublicationCoordinator::descriptorLocked(const KisPageVersion &version,
                                                     KisPageAllocationDescriptor *descriptor) const
{
    const auto found = m_descriptors.constFind(version);
    if (found == m_descriptors.constEnd())
        return false;
    if (descriptor)
        *descriptor = found.value();
    return true;
}

void KisPagePublicationCoordinator::reserveDescriptorAdditionsLocked(qsizetype additions)
{
    if (additions <= 0)
        return;
    m_descriptors.reserve(m_descriptors.size() + additions);
    ++m_statistics.descriptorCapacityPreparations;
}

KisPageStorePublicationStatistics KisPagePublicationCoordinator::statisticsLocked() const
{
    auto result = m_statistics;
    result.activeCommitPreparations = m_preparingCommits.size();
    return result;
}

KisPagePublicationCoordinatorSnapshot KisPagePublicationCoordinator::snapshotLocked() const
{
    KisPagePublicationCoordinatorSnapshot result;
    for (const PreparedTransactionState &prepared : m_preparedTransactions) {
        result.preparedProofs += prepared.proofs.size();
        result.preparedSurfaceChanges += prepared.surfaceChanges.size();
        result.stagedPageRemovals += prepared.removals.size();
    }
    result.committedTransactions = m_committedTransactions;
    return result;
}

/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPagePublicationCoordinator_p.h"
#include "KisPageStoreDiagnostics_p.h"
#include "KisPageWriteCoordinator_p.h"

#include <QScopeGuard>
#include <QReadWriteLock>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace
{

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
    DescriptorMap descriptorNodes;
    KisCompletionTicket completion;
    QVector<KisPageTransitionEffect> publicationRetirements;
    KisBackingClassChangeReservation backingReservation;
    KisPageMetadataCoordinator::DeferredPublicationCleanup metadataCleanup;
    KisImageEpochCommitResult result;
};

class KisPagePublicationCoordinator::KisPreparedOverlayUpdate::Data
{
public:
    struct Change {
        OverlayChange replacement;
        KisPreparedPageProof superseded;
        bool removalWasInBase = false;
        bool wasRemoved = false;
    };

    KisPagePublicationCoordinator *owner = nullptr;
    KisPageTransaction transaction;
    std::vector<Change> changes;
    QVector<KisPageTransition> detachments;
    QVector<KisPageKey> historical;
    std::shared_ptr<PreparedTransactionState> state;
    ProofMap proofNodes;
    RemovalSet removalNodes;
    std::vector<ProofMap::node_type> oldProofNodes;
    std::vector<RemovalSet::node_type> oldRemovalNodes;
    size_t proofInsertions = 0;
    size_t removalInsertions = 0;
    KisPageMetadataCoordinator::PreparedPublication metadata;
    KisSurfaceEpochChange surfaceChange;
    qsizetype surfaceIndex = -1;
    bool surfacePrepared = false;
    bool prepared = false;
    bool installed = false;
    bool collected = false;

    void releaseCapacity() noexcept
    {
        if (proofInsertions) {
            state->proofInsertions.fetch_sub(proofInsertions);
            proofInsertions = 0;
        }
        if (removalInsertions) {
            state->removalInsertions.fetch_sub(removalInsertions);
            removalInsertions = 0;
        }
    }
    ~Data() { releaseCapacity(); }
};

KisPagePublicationCoordinator::KisPreparedOverlayUpdate::
    KisPreparedOverlayUpdate() = default;

KisPagePublicationCoordinator::KisPreparedOverlayUpdate::
    ~KisPreparedOverlayUpdate()
{
    cancel();
}

KisPagePublicationCoordinator::KisPreparedOverlayUpdate::
    KisPreparedOverlayUpdate(KisPreparedOverlayUpdate &&) noexcept = default;

KisPagePublicationCoordinator::KisPreparedOverlayUpdate &
KisPagePublicationCoordinator::KisPreparedOverlayUpdate::operator=(
    KisPreparedOverlayUpdate &&other) noexcept
{
    if (this != &other) {
        cancel();
        data = std::move(other.data);
    }
    return *this;
}

bool KisPagePublicationCoordinator::KisPreparedOverlayUpdate::isValid() const
{
    return data && data->owner && data->transaction.isValid()
        && !data->changes.empty() && !data->installed;
}

qsizetype KisPagePublicationCoordinator::KisPreparedOverlayUpdate::
    metadataChangeCount() const
{
    return isValid() ? data->detachments.size() : 0;
}

bool KisPagePublicationCoordinator::KisPreparedOverlayUpdate::prepare(
    QString *error)
{
    if (!isValid() || data->prepared) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("overlay update cannot be prepared"));
        return false;
    }
    if (!data->detachments.isEmpty()) {
        data->metadata = data->owner->m_metadata.prepareMutation(
            data->transaction, data->detachments, error);
        if (!data->metadata.isValid())
            return false;
    }
    data->prepared = true;
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPagePublicationCoordinator::KisPreparedOverlayUpdate::prepareSurfaceLocked(
    QString *error)
{
    if (!isValid() || !data->prepared || data->surfacePrepared) {
        KisPageStoreDetail::setError(error, QStringLiteral("overlay surface preparation is invalid"));
        return false;
    }
    auto &owner = *data->owner;
    const auto surface = owner.m_derivedExtentSurface;
    if (!surface.isValid()) {
        data->surfacePrepared = true;
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    try {
        const auto overlay = KisPageReadView::transactionOverlay(data->transaction.id);
        KisSurfaceEpochState current;
        if (!owner.resolveSurfaceLocked(surface, overlay, &current)) return false;
        const auto base = owner.m_epochs.root(data->transaction.baseEpoch);
        QRect extent = current.contentExtent;
        RemovalSet removals;
        for (const auto &change : data->changes)
            if (change.replacement.key.surface == surface && change.replacement.removal)
                removals.insert(change.replacement.key);
        if (!removals.empty()) {
            // Derive from the same base + overlay used by captured readers.
            // Persistent subtree bounds avoid exporting the document manifest.
            auto delta = owner.transactionDeltaLocked(data->transaction.id);
            delta.proofs.erase(std::remove_if(delta.proofs.begin(), delta.proofs.end(),
                [&](const auto &proof) { return removals.count(proof.authority.version.key) != 0; }), delta.proofs.end());
            for (const auto &change : data->changes) {
                if (change.replacement.removal) delta.removedPages.append(change.replacement.key);
                else delta.proofs.append(change.replacement.proof);
            }
            if (!base.contentExtentAfterDelta(surface, current.logicalPageExtent, delta, &extent)) {
                KisPageStoreDetail::setError(error, QStringLiteral("derived surface extent exceeds QRect range"));
                return false;
            }
        } else {
            // Normal brush publication extends only by this segment's keys.
            // Include a sibling's already installed extent, not a stale copy
            // captured before provider/metadata preparation released owner.
            for (const auto &change : data->changes) {
                const auto &key = change.replacement.key;
                if (!(key.surface == surface) || change.replacement.removal) continue;
                qint64 left = qint64(key.page.column) * current.logicalPageExtent.width();
                qint64 top = qint64(key.page.row) * current.logicalPageExtent.height();
                qint64 right = left + current.logicalPageExtent.width() - 1;
                qint64 bottom = top + current.logicalPageExtent.height() - 1;
                if (!extent.isEmpty()) {
                    left = qMin(left, qint64(extent.left()));
                    top = qMin(top, qint64(extent.top()));
                    right = qMax(right, qint64(extent.right()));
                    bottom = qMax(bottom, qint64(extent.bottom()));
                }
                const qint64 limit = std::numeric_limits<int>::max();
                if (left < std::numeric_limits<int>::min() || top < std::numeric_limits<int>::min()
                    || right > limit || bottom > limit || right - left + 1 > limit || bottom - top + 1 > limit) {
                    KisPageStoreDetail::setError(error, QStringLiteral("derived surface extent exceeds QRect range"));
                    return false;
                }
                extent = QRect(int(left), int(top), int(right - left + 1), int(bottom - top + 1));
            }
        }
        if (extent != current.contentExtent) {
            if (current.extentRevision == std::numeric_limits<quint64>::max()) {
                KisPageStoreDetail::setError(error, QStringLiteral("surface extent revision exhausted"));
                return false;
            }
            KisSurfaceEpochChange change;
            if (!base.surfaceState(surface, &change.before)) return false;
            change.after = current;
            change.after.contentExtent = extent;
            ++change.after.extentRevision;
            if (!change.isValid()) {
                KisPageStoreDetail::setError(error, QStringLiteral("derived surface revision is invalid"));
                return false;
            }
            auto &changes = data->state->surfaceChanges;
            for (qsizetype i = 0; i < changes.size(); ++i)
                if (changes.at(i).after.surface == surface) { data->surfaceIndex = i; break; }
            // Captures may share the output vector. Detach/grow before the
            // installation interval, without publishing the new surface yet.
            changes.detach();
            if (data->surfaceIndex < 0) changes.reserve(changes.size() + 1);
            if ((!changes.isEmpty() && !changes.isDetached())
                || (data->surfaceIndex < 0 && changes.capacity() <= changes.size())) {
                KisPageStoreDetail::setError(error, QStringLiteral("overlay surface storage preparation was refused"));
                return false;
            }
            data->surfaceChange = std::move(change);
        }
        data->surfacePrepared = true;
        KisPageStoreDetail::setError(error, {});
        return true;
    } catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("overlay surface storage preparation was refused"));
        return false;
    }
}

bool KisPagePublicationCoordinator::KisPreparedOverlayUpdate::
    tryInstallLocked(
        KisPageMetadataCoordinator::DeferredPublicationCleanup *metadataCleanup,
        QString *error)
{
    if (!isValid() || !data->prepared || !data->surfacePrepared || !metadataCleanup) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("overlay update cannot be installed"));
        return false;
    }
    KisPagePublicationCoordinator &owner = *data->owner;
    const auto transaction = owner.m_preparedTransactions.constFind(
        data->transaction.id.value);
    if (transaction == owner.m_preparedTransactions.cend()
        || transaction.value() != data->state) {
        KisPageStoreDetail::setError(error, QStringLiteral("overlay transaction storage changed"));
        return false;
    }
    auto &state = *data->state;
    for (const Data::Change &change : std::as_const(data->changes)) {
        const KisPreparedPageProof current = owner.findPreparedProofLocked(
            data->transaction.id, change.replacement.key);
        if (!(current == change.superseded)
            || (state.removals.count(change.replacement.key) != 0) != change.wasRemoved
            || (current.isValid()
                && !owner.m_owner.ownsPreparedPageProof(current))
            || (change.replacement.proof.isValid()
                && !owner.m_owner.ownsPreparedPageProof(
                    change.replacement.proof))) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("overlay update source changed"));
            return false;
        }
    }

    if (!data->detachments.isEmpty()
        && !owner.m_metadata.installMutation(
            std::move(data->metadata), data->transaction, error,
            metadataCleanup)) {
        return false;
    }

    // Every missing node and all buckets were constructed before metadata
    // installation. Keep extracted nodes alive until unlocked cleanup.
    for (const Data::Change &change : std::as_const(data->changes)) {
        const auto &key = change.replacement.key;
        if (change.replacement.removal) {
            if (change.superseded.isValid())
                data->oldProofNodes.push_back(state.proofs.extract(key));
            if (change.removalWasInBase && !change.wasRemoved) {
                auto inserted = state.removals.insert(data->removalNodes.extract(key));
                Q_ASSERT(inserted.inserted);
            } else if (!change.removalWasInBase && change.wasRemoved) {
                data->oldRemovalNodes.push_back(state.removals.extract(key));
            }
        } else {
            if (change.wasRemoved)
                data->oldRemovalNodes.push_back(state.removals.extract(key));
            if (change.superseded.isValid()) {
                state.proofs.find(key)->second = change.replacement.proof;
            } else {
                auto inserted = state.proofs.insert(data->proofNodes.extract(key));
                Q_ASSERT(inserted.inserted);
            }
        }
    }
    if (data->surfaceChange.after.surface.isValid()) {
        if (data->surfaceIndex >= 0) state.surfaceChanges[data->surfaceIndex] = data->surfaceChange;
        else state.surfaceChanges.append(data->surfaceChange);
    }
    data->releaseCapacity();
    data->installed = true;
    KisPageStoreDetail::setError(error, {});
    return true;
}

void KisPagePublicationCoordinator::KisPreparedOverlayUpdate::collectRetirementsLocked()
{
    if (!data || !data->installed || data->collected) return;
    // The old identities no longer select the overlay. Their ledger records
    // and historical metadata are disposal work, not part of publication.
    for (const auto &change : std::as_const(data->changes)) {
        if (!change.superseded.isValid()) continue;
        const bool revoked = data->owner->m_owner.revokePreparedPage(change.superseded);
        Q_ASSERT(revoked);
        Q_UNUSED(revoked);
    }
    if (!data->historical.isEmpty())
        data->owner->m_history.collectUnreachableLocked(
            data->historical.constData(), data->historical.size());
    data->collected = true;
}

void KisPagePublicationCoordinator::KisPreparedOverlayUpdate::cancel() noexcept
{
    if (!data || data->installed || !data->owner) {
        Q_ASSERT(!data || !data->installed || data->collected);
        data.reset();
        return;
    }
    for (const Data::Change &change : std::as_const(data->changes)) {
        if (change.replacement.proof.isValid())
            data->owner->m_owner.revokePreparedPage(
                change.replacement.proof);
    }
    data.reset();
}

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
        coordinator.installDescriptorAdditionsLocked(&prepared.descriptorNodes);
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
    BeginMutationPreparation beginMutationPreparation,
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

bool KisPagePublicationCoordinator::configureDerivedExtentLocked(KisSurfaceId surface)
{
    KisSurfaceEpochState state;
    if (m_operational || m_derivedExtentSurface.isValid() || !surface.isValid()
        || !m_epochs.surfaceState(surface, {}, &state)) return false;
    m_derivedExtentSurface = surface;
    return true;
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
            for (const KisSurfaceEpochChange &change : (*prepared)->surfaceChanges) {
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
    if (effects.isEmpty() && m_backgroundReclamation) return;
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
        const auto proof = (*prepared)->proofs.find(key);
        if (proof != (*prepared)->proofs.cend()) {
            *version = proof->second.authority.version;
            return true;
        }
    }
    if (version->isDefaultPixel()
        || (prepared != m_preparedTransactions.constEnd() && (*prepared)->removals.count(key))) {
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
        const auto highWater = currentDefaultRevisionLocked(surface);
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
        preparedStateLocked(transaction.id)->surfaceChanges;
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
        if (!reserveDefaultRevisionLocked(after.surface, after.defaultPixelRevision, error))
            return false;
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
    if (!m_beginMutationPreparation(m_ownerContext, transaction.id, ownerLock, error)) return false;
    const auto preparationClaim = qScopeGuard([&] {
        m_endMutationPreparation(m_ownerContext, transaction.id);
    });
    if (m_pageWriteClaimed(m_ownerContext, key) || isPreparingCommitLocked(transaction.id)) {
        KisPageStoreDetail::setError(error, QStringLiteral("page removal changed during admission"));
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

    // Removal consumes the same prepared overlay protocol as seal and generic
    // publication. It must not detach a proof before its removal node exists.
    auto update = prepareOverlayUpdateLocked(transaction, {{key, {}, true}}, error);
    KisPageMetadataCoordinator::DeferredPublicationCleanup metadataCleanup;
    const auto dispose = qScopeGuard([&] {
        ownerLock.unlock();
        m_disposeMetadataCleanup(m_ownerContext, std::move(metadataCleanup));
        update = {};
        ownerLock.relock();
    });
    if (!update.isValid()) return false;
    ownerLock.unlock();
    const bool prepared = update.prepare(error);
    ownerLock.relock();
    if (!prepared) return false;
    if (m_pageWriteClaimed(m_ownerContext, key)
        || !resolveVersionLocked(key, overlay, &current) || !(current == observed)) {
        KisPageStoreDetail::setError(error, QStringLiteral("conditional page removal changed during preparation"));
        return false;
    }
    if (!update.prepareSurfaceLocked(error) || !update.tryInstallLocked(&metadataCleanup, error)) return false;
    update.collectRetirementsLocked();
    retireEffectsUnlocked({}, ownerLock);
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

KisImageEpochCommitTicket KisPagePublicationCoordinator::commitLocked(const KisPageTransaction &transaction,
                                                                      const KisPreparedPageSet &preparedPages,
                                                                      KisRetainedImageEpochSnapshot *retainedAfter,
                                                                      const KisPageStore *diagnosticOwner,
                                                                      QMutexLocker<QMutex> &ownerLock,
                                                                      QWriteLocker *publicationLock)
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
        // Storage preparation may leave an empty original transaction record
        // after refusal or an implicit-default removal. It is not a delta.
        if (found == m_preparedTransactions.constEnd() || (*found)->isEmpty())
            return {};
        const PreparedTransactionState &owned = *found.value();
        if (owned.proofs.size() != size_t(preparedPages.proofs.size())
            || owned.surfaceChanges.size() != preparedPages.surfaceChanges.size()
            || owned.removals.size() != size_t(preparedPages.removedPages.size())) {
            return {};
        }
        for (const KisPreparedPageProof &proof : preparedPages.proofs) {
            const auto ownedProof = owned.proofs.find(proof.authority.version.key);
            const bool found = ownedProof != owned.proofs.cend() && proof == ownedProof->second;
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
            if (!owned.removals.count(key))
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
        auto descriptorNodes = prepareDescriptorAdditionsLocked(descriptorChanges);
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
        preparedCommit.data->descriptorNodes = std::move(descriptorNodes);
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
        // The backend's original selector was hidden before commit. Once
        // root/metadata installation succeeds, readers can select that root
        // while disposal proceeds. Failed attempts retain the original gate.
        if (installed && publicationLock) publicationLock->unlock();
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
        m_history.collectUnreachableLocked(historyCandidates.constData(), historyCandidates.size());
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
        auto descriptorNodes = prepareDescriptorAdditionsLocked(restoredDefaultVersions);
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
            || !(m_epochs.captureCommittedRoot().epoch() == expectedHead) || m_epochs.activeTransactionCount() != 0
            || !m_restoreIsIdle(m_ownerContext)) {
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
        preparedCommit.data->descriptorNodes = std::move(descriptorNodes);
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
    m_history.collectUnreachableLocked(historyCandidates.constData(), historyCandidates.size());
    retireEffectsUnlocked({}, ownerLock);
    return {restored.root.epoch(), completion};
}

bool KisPagePublicationCoordinator::abortLocked(
    const KisPageTransaction &transaction, QMutexLocker<QMutex> &ownerLock, KisPageReadCleanup &cleanup)
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
    if (!m_prepareAbort(m_ownerContext, transaction.id, &retirementEffects, cleanup)) {
        return false;
    }

    while (const auto *remainingProofs = findProofsLocked(transaction.id)) {
        const KisPreparedPageProof proof = remainingProofs->cbegin()->second;
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

std::shared_ptr<KisPagePublicationCoordinator::PreparedTransactionState>
KisPagePublicationCoordinator::preparedStateLocked(KisPageTransactionId transaction)
{
    auto found = m_preparedTransactions.find(transaction.value);
    if (found != m_preparedTransactions.end()) return found.value();
    auto state = std::make_shared<PreparedTransactionState>();
    m_preparedTransactions.insert(transaction.value, state);
    return state;
}

const KisPagePublicationCoordinator::ProofMap *
KisPagePublicationCoordinator::findProofsLocked(KisPageTransactionId transaction) const
{
    const auto found = m_preparedTransactions.constFind(transaction.value);
    return found == m_preparedTransactions.constEnd() || (*found)->proofs.empty() ? nullptr : &(*found)->proofs;
}

KisPreparedPageProof KisPagePublicationCoordinator::findPreparedProofLocked(
    KisPageTransactionId transaction, const KisPageKey &key) const
{
    const auto *proofs = findProofsLocked(transaction);
    if (!proofs) return {};
    const auto found = proofs->find(key);
    return found == proofs->end() ? KisPreparedPageProof{} : found->second;
}

KisPreparedPageSet
KisPagePublicationCoordinator::transactionDeltaLocked(KisPageTransactionId transaction) const
{
    KisPreparedPageSet result;
    result.transaction = transaction;
    const auto found = m_preparedTransactions.constFind(transaction.value);
    if (found != m_preparedTransactions.constEnd()) {
        result.proofs.reserve(qsizetype((*found)->proofs.size()));
        for (const auto &proof : (*found)->proofs) result.proofs.append(proof.second);
        result.surfaceChanges = (*found)->surfaceChanges;
        result.removedPages.reserve(qsizetype((*found)->removals.size()));
        for (const auto &key : (*found)->removals) result.removedPages.append(key);
    }
    return result;
}

bool KisPagePublicationCoordinator::stagesRemovalLocked(
    KisPageTransactionId transaction, const KisPageKey &key) const
{
    const auto found = m_preparedTransactions.constFind(transaction.value);
    return found != m_preparedTransactions.constEnd() && (*found)->removals.count(key);
}

KisPagePublicationCoordinator::KisPreparedOverlayUpdate
KisPagePublicationCoordinator::prepareOverlayUpdateLocked(
    const KisPageTransaction &transaction,
    QVector<OverlayChange> changes,
    QString *error)
{
    if (!hasActiveTransactionLocked(transaction) || changes.isEmpty()) {
        KisPageStoreDetail::setError(error, QStringLiteral("overlay update input is invalid"));
        return {};
    }

    try {
        auto candidate = std::make_unique<KisPreparedOverlayUpdate::Data>();
        candidate->owner = this;
        candidate->transaction = transaction;
        candidate->changes.reserve(changes.size());
        candidate->detachments.reserve(changes.size());
        if (candidate->detachments.capacity() < changes.size()) throw std::bad_alloc();
        RemovalSet keys;
        size_t oldProofNodes = 0, oldRemovalNodes = 0;
        keys.reserve(changes.size());
        const auto base = m_epochs.root(transaction.baseEpoch);
        candidate->state = preparedStateLocked(transaction.id);
        auto &state = *candidate->state;
        for (OverlayChange &change : changes) {
            const bool hasProof = change.proof.isValid();
            if (!change.key.isValid() || change.removal == hasProof
                || keys.count(change.key)
                || (hasProof
                    && (!(change.proof.transaction == transaction.id)
                        || !(change.proof.authority.version.key == change.key)
                        || !m_owner.ownsPreparedPageProof(change.proof)))) {
                KisPageStoreDetail::setError(error, QStringLiteral("overlay update change is invalid"));
                return {};
            }
            keys.insert(change.key);
            const auto superseded = findPreparedProofLocked(transaction.id, change.key);
            if (superseded.isValid()) {
                if (!m_owner.ownsPreparedPageProof(superseded)
                    || (hasProof
                        && superseded.authority.version.generation.value
                            >= change.proof.authority.version.generation.value)) {
                    KisPageStoreDetail::setError(error, QStringLiteral("overlay update does not replace its current proof"));
                    return {};
                }
                KisPageTransition detach;
                detach.kind = KisPageTransitionKind::DetachPreparedVersion;
                detach.version = superseded.authority.version;
                detach.transaction = transaction.id;
                candidate->detachments.append(detach);
            }
            const bool removalWasInBase = change.removal && base.containsPage(change.key);
            const bool wasRemoved = state.removals.count(change.key) != 0;
            if (hasProof && !superseded.isValid())
                candidate->proofNodes.emplace(change.key, change.proof);
            if (removalWasInBase && !wasRemoved)
                candidate->removalNodes.insert(change.key);
            oldProofNodes += change.removal && superseded.isValid();
            oldRemovalNodes += wasRemoved && (hasProof || !removalWasInBase);
            candidate->changes.push_back({std::move(change), superseded, removalWasInBase, wasRemoved});
        }

        candidate->oldProofNodes.reserve(oldProofNodes);
        candidate->oldRemovalNodes.reserve(oldRemovalNodes);
        candidate->historical.reserve(candidate->detachments.size());
        if (candidate->historical.capacity() < candidate->detachments.size()) throw std::bad_alloc();
        for (const auto &detach : std::as_const(candidate->detachments))
            candidate->historical.append(detach.version.key);

        // Node transfer alone is not enough: a sibling may install while this
        // candidate prepares metadata. Include every outstanding insertion,
        // and never shrink a reservation made for another candidate.
        const auto reserve = [](auto &map, size_t outstanding, size_t additions) {
            if (additions > map.max_size() - map.size()
                || outstanding > map.max_size() - map.size() - additions)
                throw std::bad_alloc();
            const size_t required = map.size() + outstanding + additions;
            if (required > map.bucket_count() * map.max_load_factor()) map.reserve(required);
        };
        reserve(state.proofs, state.proofInsertions.load(), candidate->proofNodes.size());
        reserve(state.removals, state.removalInsertions.load(), candidate->removalNodes.size());
        candidate->proofInsertions = candidate->proofNodes.size();
        candidate->removalInsertions = candidate->removalNodes.size();
        state.proofInsertions.fetch_add(candidate->proofInsertions);
        state.removalInsertions.fetch_add(candidate->removalInsertions);
        KisPreparedOverlayUpdate result;
        result.data = std::move(candidate);
        KisPageStoreDetail::setError(error, {});
        return result;
    } catch (const std::bad_alloc &) {
        // The caller still owns the replacement proofs on refusal. No
        // metadata or visible overlay value has changed.
        KisPageStoreDetail::setError(error, QStringLiteral("overlay storage preparation was refused"));
        return {};
    }
}

bool KisPagePublicationCoordinator::revokePreparedProofLocked(
    const KisPreparedPageProof &proof)
{
    auto transaction = m_preparedTransactions.find(proof.transaction.value);
    Q_ASSERT(transaction != m_preparedTransactions.end());
    if (transaction == m_preparedTransactions.end()) return false;
    auto &state = **transaction;
    auto stored = state.proofs.find(proof.authority.version.key);
    Q_ASSERT(stored != state.proofs.end() && stored->second == proof);
    if (stored == state.proofs.end() || !(stored->second == proof)) return false;
    if (!m_owner.revokePreparedPage(proof)) return false;
    state.proofs.erase(stored);
    if (state.isEmpty()) m_preparedTransactions.erase(transaction);
    return true;
}

bool KisPagePublicationCoordinator::importDefaultRevisionLocked(
    KisSurfaceId surface, quint64 revision, QString *error)
{
    if (!surface.isValid() || revision == 0
        || m_defaultRevisionHighWater.contains(surface.value)) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("default pixel revision import is invalid or duplicated"));
        return false;
    }
    m_defaultRevisionHighWater.insert(surface.value, revision);
    KisPageStoreDetail::setError(error, {});
    return true;
}

quint64 KisPagePublicationCoordinator::currentDefaultRevisionLocked(KisSurfaceId surface) const
{
    return m_defaultRevisionHighWater.value(surface.value);
}

bool KisPagePublicationCoordinator::reserveDefaultRevisionLocked(
    KisSurfaceId surface, quint64 revision, QString *error)
{
    auto highWater = m_defaultRevisionHighWater.find(surface.value);
    if (!surface.isValid() || highWater == m_defaultRevisionHighWater.end()
        || revision <= highWater.value()) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("default pixel revision must be fresh across abort and restore"));
        return false;
    }
    highWater.value() = revision;
    KisPageStoreDetail::setError(error, {});
    return true;
}

void KisPagePublicationCoordinator::putDescriptorLocked(const KisPageVersion &version,
                                                        const KisPageAllocationDescriptor &descriptor)
{
    auto found = m_descriptors.find(version);
    if (found == m_descriptors.end()) {
        m_descriptors.emplace(version, descriptor);
        ++m_descriptorRevision;
    } else if (!(found->second == descriptor)) {
        found->second = descriptor;
        ++m_descriptorRevision;
    }
}

void KisPagePublicationCoordinator::removeDescriptorLocked(const KisPageVersion &version)
{
    if (m_descriptors.erase(version) != 0)
        ++m_descriptorRevision;
}

bool KisPagePublicationCoordinator::descriptorLocked(const KisPageVersion &version,
                                                     KisPageAllocationDescriptor *descriptor) const
{
    const auto found = m_descriptors.find(version);
    if (found == m_descriptors.end())
        return false;
    if (descriptor)
        *descriptor = found->second;
    return true;
}

KisPagePublicationCoordinator::DescriptorMap
KisPagePublicationCoordinator::prepareDescriptorAdditionsLocked(const QVector<PreparedDescriptorChange> &changes)
{
    DescriptorMap nodes;
    if (changes.isEmpty()) return nodes;
    for (const auto &change : changes) nodes.emplace(change.version, change.descriptor);
    const size_t required = m_descriptors.size() + nodes.size();
    // Never shrink a bucket reservation made for an overlapping candidate.
    // Descriptor revision revalidation covers intervening semantic edits.
    if (required > m_descriptors.bucket_count() * m_descriptors.max_load_factor())
        m_descriptors.reserve(required);
    ++m_statistics.descriptorCapacityPreparations;
    return nodes;
}

void KisPagePublicationCoordinator::installDescriptorAdditionsLocked(DescriptorMap *prepared)
{
    for (auto it = prepared->begin(); it != prepared->end();) {
        auto current = it++;
        auto found = m_descriptors.find(current->first);
        if (found == m_descriptors.end()) {
            // Node and buckets already exist. C++17 node transfer allocates
            // neither; no candidate value is destroyed during installation.
            m_descriptors.insert(prepared->extract(current));
            ++m_descriptorRevision;
        } else if (!(found->second == current->second)) {
            std::swap(found->second, current->second);
            ++m_descriptorRevision;
        }
    }
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
    for (const auto &prepared : m_preparedTransactions) {
        result.preparedProofs += qsizetype(prepared->proofs.size());
        result.preparedSurfaceChanges += prepared->surfaceChanges.size();
        result.stagedPageRemovals += qsizetype(prepared->removals.size());
    }
    result.committedTransactions = m_committedTransactions;
    return result;
}

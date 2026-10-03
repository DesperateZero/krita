/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPagePublicationCoordinator_p.h"
#include "KisPageStoreDiagnostics_p.h"
#include "KisPageWriteCoordinator_p.h"
#include "KisPageReadCoordinator_p.h"

#include <QScopeGuard>
#include <QReadWriteLock>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace
{

bool overlayKeyLess(const KisPageKey &a, const KisPageKey &b)
{
    if (a.surface.value != b.surface.value) return a.surface.value < b.surface.value;
    if (a.page.row != b.page.row) return a.page.row < b.page.row;
    return a.page.column < b.page.column;
}

template<typename PreparedPublication, typename Changes>
bool appendPublicationBackingChanges(
                                     const PreparedPublication &publication,
                                     const KisImageEpochRootSnapshot &currentRoot,
                                     const KisPageVersion &target,
                                     KisBackingBudgetClass targetBefore,
                                     KisBackingBudgetClass oldAfter,
                                     Changes *changes)
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
        changes->push_back({oldAuthority, KisBackingBudgetClass::Current, oldAfter});
    if (targetAuthority.isValid())
        changes->push_back({targetAuthority, targetBefore, KisBackingBudgetClass::Current});
    return true;
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
    DescriptorMap requested;
    try { requested = prepareDescriptorLocked(version, descriptor); }
    catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("virtual default descriptor storage budget was refused"));
        return false;
    }
    const auto attachHistorical = [&] {
        KisPageTransition attach;
        attach.kind = KisPageTransitionKind::AttachHistoricalDefault;
        attach.version = version;
        const auto result = m_metadata.applyOwner(version.key, attach);
        if (!result.accepted) KisPageStoreDetail::setError(error, result.rejectionReason);
        return result.accepted;
    };
    KisPageMetadataCoordinator::VersionInfo page;
    if (m_metadata.versionSnapshot(version, &page)) {
        if (!page.version.isValid() && !attachHistorical()) return false;
    } else {
        // A first access through an older transaction/view must not turn its
        // default revision into today's published head of a virgin coordinate.
        const auto current = m_epochs.captureCommittedRoot();
        KisPageVersion head;
        KisSurfaceEpochState currentSurface;
        if (!current.resolve(version.key, &head) || !head.isDefaultPixel()
            || !current.surfaceState(version.key.surface, &currentSurface))
            return false;
        DescriptorMap published;
        if (!(head == version)) {
            try { published = prepareDescriptorLocked(head, currentSurface.allocationDescriptor()); }
            catch (const std::bad_alloc &) {
                KisPageStoreDetail::setError(error, QStringLiteral("virtual default descriptor storage budget was refused"));
                return false;
            }
        }
        KisPageVersionStateSnapshot implicit;
        implicit.version = head;
        implicit.publication = KisPagePublicationState::Published;
        KisPageStateSnapshot initial;
        initial.key = version.key;
        initial.publishedEpoch = current.epoch();
        initial.publishedGeneration = head.generation;
        initial.publishedDefaultPixelRevision = head.defaultPixelRevision;
        initial.nextGeneration = KisPageGeneration{head.generation.value + 1};
        initial.versions.append(implicit);
        if (!m_metadata.registerPage(initial, error))
            return false;
        if (!(head == version)) {
            installDescriptorAdditionsLocked(&published);
            if (!attachHistorical()) return false;
        }
    }
    installDescriptorAdditionsLocked(&requested);
    KisPageStoreDetail::setError(error, {});
    return true;
}

class KisPagePublicationCoordinator::KisPreparedMutationCommit::Data
{
public:
    explicit Data(const KisMutationStorageAllocator<char> &storage)
        : descriptorChanges(storage), descriptorNodes(VersionLess{}, storage)
        , directoryHeads(storage), restoreTargets(storage), publicationChanges(storage), backingChanges(storage) {}
    KisPagePublicationCoordinator *owner = nullptr;
    KisPageTransaction transaction;
    KisPageMetadataCoordinator::PreparedPublication metadata;
    KisImageEpochReferenceModel::PreparedCommit epoch;
    KisImageEpochReferenceModel::PreparedRootReservation restoreEpoch;
    std::vector<PreparedDescriptorChange, KisMutationStorageAllocator<PreparedDescriptorChange>> descriptorChanges;
    DescriptorMap descriptorNodes;
    KisPageMetadataCoordinator::PublicationHeads directoryHeads;
    KisPageMetadataCoordinator::PublicationHeads restoreTargets;
    std::vector<KisPageMetadataCoordinator::PublicationChange,
        KisMutationStorageAllocator<KisPageMetadataCoordinator::PublicationChange>> publicationChanges;
    KisPageOwnerLedger::BackingChanges backingChanges;
    KisCompletionTicket completion;
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

    explicit Data(const KisMutationStorageAllocator<char> &storage)
        : changes(storage), detachments(storage)
        , proofNodes(0, PageHash{}, std::equal_to<KisPageKey>{}, storage)
        , removalNodes(0, PageHash{}, std::equal_to<KisPageKey>{}, storage)
        , oldProofNodes(storage), oldRemovalNodes(storage) {}

    KisPagePublicationCoordinator *owner = nullptr;
    KisPageTransaction transaction;
    std::vector<Change, KisMutationStorageAllocator<Change>> changes;
    std::vector<KisPageVersion, KisMutationStorageAllocator<KisPageVersion>> detachments;
    std::shared_ptr<PreparedTransactionState> state;
    ProofMap proofNodes;
    RemovalSet removalNodes;
    std::vector<ProofMap::node_type, KisMutationStorageAllocator<ProofMap::node_type>> oldProofNodes;
    std::vector<RemovalSet::node_type, KisMutationStorageAllocator<RemovalSet::node_type>> oldRemovalNodes;
    size_t proofInsertions = 0;
    size_t removalInsertions = 0;
    KisPageMetadataCoordinator::PreparedPublication metadata;
    KisSurfaceEpochChange surfaceChange;
    qsizetype surfaceIndex = -1;
    bool surfacePrepared = false;
    bool prepared = false;
    bool installed = false;
    bool collected = false;

    const Change *findChange(const KisPageKey &key) const
    {
        const auto found = std::lower_bound(changes.cbegin(), changes.cend(), key,
            [](const Change &change, const KisPageKey &value) { return overlayKeyLess(change.replacement.key, value); });
        return found != changes.cend() && found->replacement.key == key ? &*found : nullptr;
    }

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

void KisPagePublicationCoordinator::KisPreparedOverlayUpdate::DataDeleter::operator()(Data *value) const noexcept
{
    if (!value) return;
    value->~Data();
    auto allocator = storage;
    allocator.deallocate(value, 1);
}

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
    if (!data->detachments.empty()) {
        data->metadata = data->owner->m_metadata.prepareMutation(
            data->transaction, data->detachments.data(), qsizetype(data->detachments.size()), error);
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
        if (std::any_of(data->changes.cbegin(), data->changes.cend(), [&](const auto &change) {
            return change.replacement.key.surface == surface && change.replacement.removal;
        })) {
            // Derive from the same base + overlay used by captured readers.
            // Persistent subtree bounds avoid exporting the document manifest.
            const auto storage = data->changes.get_allocator();
            std::vector<KisPageKey, KisMutationStorageAllocator<KisPageKey>> removals(storage), additions(storage);
            removals.reserve(data->state->removals.size() + data->changes.size());
            additions.reserve(data->state->proofs.size() + data->changes.size());
            for (const auto &key : data->state->removals) removals.push_back(key);
            for (const auto &proof : data->state->proofs) {
                const auto changed = data->findChange(proof.first);
                if (!changed || !changed->replacement.removal) additions.push_back(proof.first);
            }
            for (const auto &change : data->changes) {
                if (change.replacement.removal) removals.push_back(change.replacement.key);
                else additions.push_back(change.replacement.key);
            }
            std::sort(removals.begin(), removals.end(), overlayKeyLess);
            removals.erase(std::unique(removals.begin(), removals.end()), removals.end());
            if (!base.contentExtentAfterPages(surface, current.logicalPageExtent,
                removals.data(), removals.size(), additions.data(), additions.size(), &extent)) {
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
            for (size_t i = 0; i < changes.size(); ++i)
                if (changes.at(i).after.surface == surface) { data->surfaceIndex = qsizetype(i); break; }
            // The authority array is never shared with a Qt export. Reserve
            // its actual paid capacity before the installation interval.
            if (data->surfaceIndex < 0) changes.reserve(changes.size() + 1);
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
    const auto transaction = owner.m_preparedTransactions.find(
        data->transaction.id.value);
    if (transaction == owner.m_preparedTransactions.cend()
        || transaction->second != data->state) {
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

    if (!data->detachments.empty()
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
        else state.surfaceChanges.push_back(data->surfaceChange);
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
        data->owner->m_history.requestKeyLocked(change.replacement.key);
    }
    if (!data->detachments.empty())
        data->owner->m_history.collectUnreachableLocked(nullptr, 0);
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

KisPagePublicationCoordinator::KisPreparedMutationCommit::KisPreparedMutationCommit(KisBackingBudgetController &budget)
{
    auto storage = KisMutationStorageAllocator<Data>::retained(&budget);
    auto *raw = storage.allocate(1);
    try { std::allocator_traits<decltype(storage)>::construct(storage, raw, storage); }
    catch (...) { auto allocator = storage; allocator.deallocate(raw, 1); throw; }
    data = std::unique_ptr<Data, DataDeleter>(raw, {storage});
}

void KisPagePublicationCoordinator::KisPreparedMutationCommit::DataDeleter::operator()(Data *value) const noexcept
{
    if (!value) return;
    value->~Data();
    auto allocator = storage;
    allocator.deallocate(value, 1);
}

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
    KisBackingBudgetController &budget,
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
    , m_budget(budget)
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
    , m_preparedTransactions(KisMutationStorageAllocator<TransactionMap::value_type>(&budget))
    , m_defaultRevisionHighWater(std::less<quint64>{}, KisMutationStorageAllocator<DefaultRevisionMap::value_type>(&budget))
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
        const auto prepared = m_preparedTransactions.find(view.transaction.value);
        if (prepared != m_preparedTransactions.cend()) {
            for (const KisSurfaceEpochChange &change : prepared->second->surfaceChanges) {
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
    return m_epochs.activeTransaction(transaction.id) == transaction;
}

void KisPagePublicationCoordinator::processRetirementsUnlocked(QMutexLocker<QMutex> &ownerLock)
{
    ++m_activeProviderCalls;
    ownerLock.unlock();
    m_retirementQueue.processAcceptedEffects(m_backgroundReclamation);
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
    const auto prepared = m_preparedTransactions.find(view.transaction.value);
    if (prepared != m_preparedTransactions.cend()) {
        const auto proof = prepared->second->proofs.find(key);
        if (proof != prepared->second->proofs.cend()) {
            *version = proof->second.authority.version;
            return true;
        }
    }
    if (version->isDefaultPixel()
        || (prepared != m_preparedTransactions.cend() && prepared->second->removals.count(key))) {
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
                                                               QString *error) try
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

    auto &changes = preparedStateLocked(transaction.id)->surfaceChanges;
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
    if (!existing) changes.reserve(changes.size() + 1);
    if (previous.defaultPixelRevision != after.defaultPixelRevision) {
        if (!reserveDefaultRevisionLocked(after.surface, after.defaultPixelRevision, error))
            return false;
    }
    if (existing)
        existing->after = after;
    else
        changes.push_back(change);
    KisPageStoreDetail::setError(error, {});
    return true;
}
catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("surface metadata storage preparation was refused"));
    return false;
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
    const OverlayChange removal{key, {}, true};
    auto update = prepareOverlayUpdateLocked(transaction, &removal, 1, error);
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
    processRetirementsUnlocked(ownerLock);
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
    const auto found = m_preparedTransactions.find(transaction.id.value);
    // Storage preparation may leave an empty original transaction record
    // after refusal or an implicit-default removal. It is not a delta.
    if (found == m_preparedTransactions.cend() || found->second->isEmpty())
        return {};
    const PreparedTransactionState &owned = *found->second;
    // The synchronous preparation claim below protects these original facts
    // through every owner unlock, including rebase and unlocked cleanup.
    const auto surfaceChangeFor = [&](KisSurfaceId surface) -> const KisSurfaceEpochChange * {
        const auto change = std::find_if(owned.surfaceChanges.cbegin(), owned.surfaceChanges.cend(),
            [&](const auto &value) { return value.after.surface == surface; });
        return change == owned.surfaceChanges.cend() ? nullptr : &*change;
    };

    CommitPreparation preparation(transaction.id);
    m_preparingCommits.push_back(preparation);
    const auto commitClaim = qScopeGuard([&] {
        if (!ownerLock.isLocked()) ownerLock.relock();
        m_preparingCommits.erase(m_preparingCommits.iterator_to(preparation));
    });
    using Descriptor = std::shared_ptr<const KisPageAllocationDescriptor>;
    std::vector<Descriptor, KisMutationStorageAllocator<Descriptor>> proofDescriptors{
        KisMutationStorageAllocator<Descriptor>(&m_budget)};
    const auto disposeProofDescriptors = qScopeGuard([&] {
        if (!proofDescriptors.capacity()) return;
        const bool locked = ownerLock.isLocked();
        if (locked) ownerLock.unlock();
        proofDescriptors = decltype(proofDescriptors){proofDescriptors.get_allocator()};
        if (locked) ownerLock.relock();
    });
    try {
        if (owned.proofs.size() != size_t(preparedPages.proofs.size())
            || owned.surfaceChanges.size() != size_t(preparedPages.surfaceChanges.size())
            || owned.removals.size() != size_t(preparedPages.removedPages.size())) {
            return {};
        }
        proofDescriptors.reserve(size_t(preparedPages.proofs.size()));
        for (const KisPreparedPageProof &proof : preparedPages.proofs) {
            const auto ownedProof = owned.proofs.find(proof.authority.version.key);
            const bool found = ownedProof != owned.proofs.cend() && proof == ownedProof->second;
            const auto descriptor = m_descriptors.find(proof.authority.version);
            if (!found || descriptor == m_descriptors.cend())
                return {};
            proofDescriptors.push_back(descriptor->second);
        }
        for (const KisSurfaceEpochChange &change : preparedPages.surfaceChanges) {
            const auto original = surfaceChangeFor(change.after.surface);
            if (!original || !(change == *original))
                return {};
        }
        for (const KisPageKey &key : preparedPages.removedPages) {
            if (!owned.removals.count(key))
                return {};
        }
    } catch (const std::bad_alloc &) { return {}; }
    const bool changesSurfaceDefault = std::any_of(owned.surfaceChanges.cbegin(), owned.surfaceChanges.cend(),
        [](const auto &change) { return change.before.defaultPixelRevision != change.after.defaultPixelRevision; });

    ownerLock.unlock();
    diagnostic.next(Phase::CommitProviderValidation, quint64(proofDescriptors.size()));
    bool proofsValid = true;
    for (qsizetype i = 0; i < preparedPages.proofs.size(); ++i) {
        if (!m_owner.validatePreparedPage(m_metadata, preparedPages.proofs.at(i), *proofDescriptors.at(i))) {
            proofsValid = false;
            break;
        }
    }
    proofDescriptors = decltype(proofDescriptors){proofDescriptors.get_allocator()};
    diagnostic.next(Phase::CommitEpochDeltaPrepare, pageWork);
    const bool deltaPrepared = proofsValid && m_epochs.preparePublication(preparedPages);
    diagnostic.next(Phase::CommitInputOwnerWait, pageWork);
    ownerLock.relock();
    if (!deltaPrepared)
        return {};

    for (;;) {
        diagnostic.next(Phase::CommitDefaultRemovalPreparation, quint64(preparedPages.removedPages.size()));
        KisPreparedMutationCommit preparedCommit;
        const auto discardCommit = qScopeGuard([&] {
            if (!preparedCommit.data) return;
            if (ownerLock.isLocked()) ownerLock.unlock();
            preparedCommit = {};
            ownerLock.relock();
        });
        try { preparedCommit = KisPreparedMutationCommit(m_budget); }
        catch (const std::bad_alloc &) { return {}; }
        auto &descriptorChanges = preparedCommit.data->descriptorChanges;
        const KisImageEpochRootSnapshot currentRoot = m_epochs.captureCommittedRoot();
        if (!currentRoot.isValid() || currentRoot.epoch().value == std::numeric_limits<quint64>::max()) {
            return {};
        }
        const KisImageEpochId prospectiveEpoch{currentRoot.epoch().value + 1};
        auto &publicationChanges = preparedCommit.data->publicationChanges;
        auto &registeredPages = preparedCommit.data->directoryHeads;
        const quint64 directoryRevision = m_metadata.pageRegistrationCount();
        try { if (changesSurfaceDefault) registeredPages = m_metadata.publicationHeads(); }
        catch (const std::bad_alloc &) { return {}; }
        const auto replacesDefault = [&](const KisPageVersion &page) {
            const auto surfaceChange = surfaceChangeFor(page.key.surface);
            return surfaceChange
                && surfaceChange->before.defaultPixelRevision != surfaceChange->after.defaultPixelRevision
                && !owned.proofs.count(page.key)
                && page.isDefaultPixel();
        };
        const auto replacements = std::count_if(registeredPages.cbegin(), registeredPages.cend(), replacesDefault);
        try {
            const size_t descriptorCount = size_t(replacements) + size_t(preparedPages.removedPages.size());
            descriptorChanges.reserve(descriptorCount);
            publicationChanges.reserve(size_t(preparedPages.proofs.size()) + descriptorCount);
        }
        catch (const std::bad_alloc &) { return {}; }
        for (const KisPageVersion &page : registeredPages) {
            if (!replacesDefault(page)) continue;
            const KisSurfaceEpochChange &change = *surfaceChangeFor(page.key.surface);
            const KisPageVersion currentVersion = page;

            PreparedDescriptorChange replacement;
            replacement.version = {page.key, page.generation, change.after.defaultPixelRevision};
            replacement.descriptor = change.after.allocationDescriptor();
            KisPageMetadataCoordinator::PublicationInfo inputs;
            if (!m_metadata.queryPublication(page.key, replacement.version, &inputs)) {
                return {};
            }
            const auto &published = inputs.current;
            if (!(published.version == currentVersion) || (!published.authority.isValid() && !published.isVirtualDefault())
                || !replacement.descriptor.isValid()) {
                return {};
            }
            const auto &existingReplacement = inputs.target;
            KisReplicaHandle replacementReplica;
            if (existingReplacement.version.isValid()) {
                KisPageAllocationDescriptor existingDescriptor;
                if (existingReplacement.publication != KisPagePublicationState::Historical
                    || (!existingReplacement.authority.isValid() && !existingReplacement.isVirtualDefault())
                    || !descriptorLocked(replacement.version, &existingDescriptor)
                    || !(existingDescriptor == replacement.descriptor)) {
                    return {};
                }
                replacementReplica = existingReplacement.authority;
            }

            publicationChanges.push_back({KisPageTransitionKind::ReplaceDefaultPixel, replacement.version, replacementReplica});
            descriptorChanges.push_back(replacement);
        }

        const auto transactionView = KisPageReadView::transactionOverlay(transaction.id);
        for (const KisPageKey &key : preparedPages.removedPages) {
            KisPageMetadataCoordinator::PublicationInfo page;
            KisSurfaceEpochState surface;
            if (!m_metadata.queryPublication(key, {}, &page)
                || !m_epochs.surfaceState(key.surface, transactionView, &surface)) {
                return {};
            }
            if (const auto surfaceChange = surfaceChangeFor(key.surface))
                surface = surfaceChange->after;
            const KisPageVersion target{key, KisPageGeneration{1}, surface.defaultPixelRevision};
            const auto &published = page.current;
            if (!published.version.isValid() || (!published.authority.isValid() && !published.isVirtualDefault())) {
                return {};
            }
            const KisPageAllocationDescriptor descriptor = surface.allocationDescriptor();
            if (!descriptor.isValid())
                return {};
            publicationChanges.push_back({KisPageTransitionKind::RestoreCommittedVersion, target, {}});
            descriptorChanges.push_back({target, descriptor});
        }

        diagnostic.next(Phase::CommitTransitionInputs,
                        quint64(preparedPages.proofs.size() + publicationChanges.size()));
        for (const KisPreparedPageProof &proof : preparedPages.proofs) {
            publicationChanges.push_back({KisPageTransitionKind::CommitTransaction, proof.authority.version, {}});
        }
        const qsizetype descriptorAdditions = descriptorChanges.size();
        try { preparedCommit.data->descriptorNodes = prepareDescriptorAdditionsLocked(descriptorChanges.data(), descriptorChanges.size()); }
        catch (const std::bad_alloc &) { return {}; }
        const quint64 descriptorRevision = m_descriptorRevision;
        ownerLock.unlock();
        registeredPages = decltype(preparedCommit.data->directoryHeads){registeredPages.get_allocator()};
        const quint64 publicationTransitionCount = quint64(publicationChanges.size());
        diagnostic.next(Phase::CommitTransitionPreflight, publicationTransitionCount);
        auto &publication = preparedCommit.data->metadata;
        publication = m_metadata.preparePublication(transaction, prospectiveEpoch,
            publicationChanges.data(), qsizetype(publicationChanges.size()));
        publicationChanges = decltype(preparedCommit.data->publicationChanges){publicationChanges.get_allocator()};
        diagnostic.next(Phase::CommitEpochPrepare, pageWork);
        KisImageEpochCommitResult candidateFailure;
        auto &rootCandidate = preparedCommit.data->epoch;
        rootCandidate = m_epochs.prepareCommit(transaction, &candidateFailure);
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
            continue;
        }
        if (!publication.isValid() || !rootCandidate.isValid()) {
            return {};
        }
        diagnostic.next(Phase::CommitCompletion);
        const KisCompletionTicket commitCompletion = m_readyHostCompletion;
        Q_ASSERT(commitCompletion.isValid()); // Prepared once by Store configuration.

        auto &backingChanges = preparedCommit.data->backingChanges;
        bool backingInputsValid = true;
        try {
            backingChanges.reserve(size_t(preparedPages.proofs.size()) * 2 + descriptorChanges.size() * 2);
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
        } catch (const std::bad_alloc &) { return {}; }
        if (!backingInputsValid)
            return {};
        auto backingReservation = m_owner.prepareBackingChanges(
            std::move(backingChanges), nullptr, 0);
        if (!backingReservation.isValid())
            return {};

        preparedCommit.data->owner = this;
        preparedCommit.data->transaction = transaction;
        preparedCommit.data->backingReservation = std::move(backingReservation);
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

        for (const KisPreparedPageProof &proof : preparedPages.proofs)
            revokePreparedProofLocked(proof);
        auto releasedOverlay = m_preparedTransactions.extract(transaction.id.value);
        m_history.collectEpochBookkeepingLocked(!(transaction.baseEpoch == currentRoot.epoch()));
        for (const KisPreparedPageProof &proof : preparedPages.proofs) {
            m_history.requestKeyLocked(proof.authority.version.key);
        }
        for (const PreparedDescriptorChange &change : std::as_const(preparedCommit.data->descriptorChanges)) {
            m_history.requestKeyLocked(change.version.key);
        }
        diagnostic.next(Phase::CommitHistoryCollect, quint64(preparedPages.proofs.size() + descriptorChanges.size()));
        m_history.collectUnreachableLocked(nullptr, 0);
        diagnostic.next(Phase::CommitProviderRetire, 0);
        processRetirementsUnlocked(ownerLock);
        ++m_committedTransactions;
        const KisImageEpochCommitTicket ticket{committed.root.epoch(), preparedCommit.data->completion};
        ownerLock.unlock();
        releasedOverlay = {};
        preparedCommit = {};
        ownerLock.relock();
        return ticket;
    }
}

KisImageEpochCommitTicket
KisPagePublicationCoordinator::restoreRetainedEpochLocked(const KisRetainedImageEpochSnapshot &retained,
                                                          const KisPageKeyStorage *changedPages,
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
    bool changesDefaults = false;
    for (size_t i = 0; i < retainedRoot.surfaceCount(); ++i) {
        const auto &surface = retainedRoot.surfaceAt(i);
        KisSurfaceEpochState currentSurface;
        if (!currentRoot.surfaceState(surface.surface, &currentSurface)) {
            return {};
        }
        if (currentSurface.defaultPixelRevision != surface.defaultPixelRevision) {
            changesDefaults = true;
        }
    }
    const auto defaultChanged = [&](KisSurfaceId identity) {
        KisSurfaceEpochState before, after;
        return retainedRoot.surfaceState(identity, &before) && currentRoot.surfaceState(identity, &after)
            && before.defaultPixelRevision != after.defaultPixelRevision;
    };
    KisImageEpochCommitResult restored;
    KisCompletionTicket completion;
    for (;;) {
        const quint64 directoryRevision = m_metadata.pageRegistrationCount();
        KisPreparedMutationCommit preparedCommit;
        const auto discardCommit = qScopeGuard([&] {
            if (!preparedCommit.data) return;
            if (ownerLock.isLocked()) ownerLock.unlock();
            preparedCommit = {};
            ownerLock.relock();
        });
        try { preparedCommit = KisPreparedMutationCommit(m_budget); }
        catch (const std::bad_alloc &) { return {}; }
        auto &restoreTargets = preparedCommit.data->restoreTargets;
        auto &registeredPages = preparedCommit.data->directoryHeads;
        auto &restoredDefaultVersions = preparedCommit.data->descriptorChanges;
        try {
            if (changedPages) {
                restoreTargets.reserve(size_t(changedPages->size()));
                for (const KisPageKey &key : *changedPages) {
                    if (!key.isValid())
                        continue;
                    KisPageVersion target;
                    if (!m_epochs.resolve(key, retainedView, &target))
                        return {};
                    restoreTargets.push_back(target);
                }
                std::sort(restoreTargets.begin(), restoreTargets.end(), [](const auto &a, const auto &b) {
                    return overlayKeyLess(a.key, b.key);
                });
                restoreTargets.erase(std::unique(restoreTargets.begin(), restoreTargets.end(),
                    [](const auto &a, const auto &b) { return a.key == b.key; }), restoreTargets.end());
            } else {
                if (!retainedRoot.isValid())
                    return {};
                restoreTargets.resize(size_t(retainedRoot.pageCount()));
                retainedRoot.copyPageVersions(restoreTargets.data());
            }
            if (!changedPages || changesDefaults) {
                // The original target prefix is sorted and unique. Directory keys
                // are unique by shard ownership; suffix insertions need no second
                // membership authority or quadratic prefix scan.
                const size_t originalTargets = restoreTargets.size();
                registeredPages = m_metadata.publicationHeads();
                for (const KisPageVersion &page : registeredPages) {
                    const auto end = restoreTargets.cbegin() + originalTargets;
                    const auto found = std::lower_bound(restoreTargets.cbegin(), end, page.key,
                        [](const auto &version, const auto &key) { return overlayKeyLess(version.key, key); });
                    if (found != end && found->key == page.key)
                        continue;
                    if (changedPages
                        && (!defaultChanged(page.key.surface) || !page.isDefaultPixel())) {
                        continue;
                    }
                    KisPageVersion target;
                    if (!m_epochs.resolve(page.key, retainedView, &target)) {
                        return {};
                    }
                    restoreTargets.push_back(target);
                }
            }

            for (const KisPageVersion &version : std::as_const(restoreTargets)) {
                KisPageMetadataCoordinator::PublicationInfo page;
                if (!m_metadata.queryPublication(version.key, version, &page)) {
                    return {};
                }
                if (!page.target.version.isValid() && version.isDefaultPixel()) {
                    KisSurfaceEpochState surface;
                    if (!m_epochs.surfaceState(version.key.surface, retainedView, &surface)
                        || surface.defaultPixelRevision != version.defaultPixelRevision
                        || !surface.allocationDescriptor().isValid()) {
                        return {};
                    }
                    restoredDefaultVersions.push_back({version, surface.allocationDescriptor()});
                }
            }
        } catch (const std::bad_alloc &) { return {}; }

        const qsizetype descriptorAdditions = restoredDefaultVersions.size();
        try { preparedCommit.data->descriptorNodes = prepareDescriptorAdditionsLocked(restoredDefaultVersions.data(), restoredDefaultVersions.size()); }
        catch (const std::bad_alloc &) { return {}; }
        const quint64 descriptorRevision = m_descriptorRevision;

        ownerLock.unlock();
        registeredPages = decltype(preparedCommit.data->directoryHeads){registeredPages.get_allocator()};
        KisPageStoreDiagnosticTimer diagnostic(diagnosticOwner,
                                               Phase::RestoreTransitionPreflight,
                                               quint64(restoreTargets.size()));
        const quint64 restoreTransitionCount = quint64(restoreTargets.size());
        auto &publication = preparedCommit.data->metadata;
        publication = m_metadata.prepareRestoration(nextEpoch, restoreTargets.data(), qsizetype(restoreTargets.size()));
        KisImageEpochCommitResult candidateFailure;
        auto &rootCandidate = preparedCommit.data->restoreEpoch;
        rootCandidate = m_epochs.prepareRestore(source, &candidateFailure);
        diagnostic.next(Phase::RestorePublishOwnerWait, restoreTransitionCount);
        ownerLock.relock();
        diagnostic.next(Phase::RestorePublication, restoreTransitionCount);
        if (!m_operational || (!publication.isValid() && !publication.needsReprepare()) || !rootCandidate.isValid()
            || !(m_epochs.captureCommittedRoot().epoch() == expectedHead) || m_epochs.activeTransactionCount() != 0
            || !m_restoreIsIdle(m_ownerContext)) {
            return {};
        }
        const bool directoryChanged = (!changedPages || changesDefaults)
            && m_metadata.pageRegistrationCount() != directoryRevision;
        const bool descriptorChanged = descriptorAdditions != 0 && m_descriptorRevision != descriptorRevision;
        if (publication.needsReprepare() || directoryChanged || descriptorChanged) {
            m_statistics.restoreMetadataRepreparations += publication.needsReprepare();
            m_statistics.restoreDirectoryRepreparations += directoryChanged;
            continue;
        }
        completion = m_readyHostCompletion;
        Q_ASSERT(completion.isValid()); // Prepared once by Store configuration.

        auto &backingChanges = preparedCommit.data->backingChanges;
        try {
            backingChanges.reserve(restoreTargets.size() * 2);
            for (const KisPageVersion &target : std::as_const(restoreTargets)) {
                if (!appendPublicationBackingChanges(publication, currentRoot, target,
                                                     KisBackingBudgetClass::RetainedHistory,
                                                     KisBackingBudgetClass::RetainedHistory,
                                                     &backingChanges))
                    return {};
            }
        } catch (const std::bad_alloc &) { return {}; }
        auto backingReservation = m_owner.prepareBackingChanges(
            std::move(backingChanges), nullptr, 0);
        if (!backingReservation.isValid())
            return {};

        preparedCommit.data->owner = this;
        preparedCommit.data->backingReservation = std::move(backingReservation);
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
            for (const KisPageVersion &target : std::as_const(restoreTargets))
                m_history.requestKeyLocked(target.key);
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
    m_history.collectUnreachableLocked(nullptr, 0);
    processRetirementsUnlocked(ownerLock);
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

    CommitPreparation preparation(transaction.id);
    m_preparingCommits.push_back(preparation);
    const auto commitClaim = qScopeGuard([&] {
        if (!ownerLock.isLocked()) ownerLock.relock();
        m_preparingCommits.erase(m_preparingCommits.iterator_to(preparation));
    });
    // Earlier pages already transferred their records even if a later page
    // refuses. Keep the remainder retryable and preserve the foreground pump.
    const auto retire = qScopeGuard([&] {
        processRetirementsUnlocked(ownerLock);
    });
    if (!m_prepareAbort(m_ownerContext, transaction.id, cleanup)) {
        return false;
    }

    while (const auto *remainingProofs = findProofsLocked(transaction.id)) {
        const KisPreparedPageProof proof = remainingProofs->cbegin()->second;
        KisPageTransition transition;
        transition.kind = KisPageTransitionKind::AbortTransaction;
        transition.transaction = transaction.id;
        const KisPageMetadataTransitionResult result = m_metadata.applyOwner(proof.authority.version.key, transition);
        if (!result.accepted)
            return false;
        revokePreparedProofLocked(proof);
    }
    // Admission and the original preparation claim keep this transaction
    // active until its epoch exit; the public identity guard cannot reject it.
    if (!m_epochs.abort(transaction))
        qFatal("Owned transaction changed before abort completion");
    auto releasedOverlay = m_preparedTransactions.extract(transaction.id.value);
    const auto disposeOverlay = qScopeGuard([&] {
        ownerLock.unlock();
        releasedOverlay = {};
        ownerLock.relock();
    });
    m_history.collectEpochBookkeepingLocked(!(transaction.baseEpoch == m_epochs.captureCommittedRoot().epoch()));
    return true;
}

bool KisPagePublicationCoordinator::isPreparingCommitLocked(KisPageTransactionId transaction) const
{
    return std::any_of(m_preparingCommits.cbegin(), m_preparingCommits.cend(), [&](const auto &claim) {
        return claim.transaction == transaction;
    });
}

std::shared_ptr<KisPagePublicationCoordinator::PreparedTransactionState>
KisPagePublicationCoordinator::preparedStateLocked(KisPageTransactionId transaction)
{
    auto found = m_preparedTransactions.find(transaction.value);
    if (found != m_preparedTransactions.end()) return found->second;
    const auto storage = KisMutationStorageAllocator<PreparedTransactionState>::retained(&m_budget);
    auto state = std::allocate_shared<PreparedTransactionState>(storage, storage);
    m_preparedTransactions.emplace(transaction.value, state);
    return state;
}

const KisPagePublicationCoordinator::ProofMap *
KisPagePublicationCoordinator::findProofsLocked(KisPageTransactionId transaction) const
{
    const auto found = m_preparedTransactions.find(transaction.value);
    return found == m_preparedTransactions.cend() || found->second->proofs.empty() ? nullptr : &found->second->proofs;
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
    const auto found = m_preparedTransactions.find(transaction.value);
    if (found != m_preparedTransactions.cend()) {
        result.proofs.reserve(qsizetype(found->second->proofs.size()));
        for (const auto &proof : found->second->proofs) result.proofs.append(proof.second);
        result.surfaceChanges.reserve(qsizetype(found->second->surfaceChanges.size()));
        for (const auto &change : found->second->surfaceChanges) result.surfaceChanges.append(change);
        result.removedPages.reserve(qsizetype(found->second->removals.size()));
        for (const auto &key : found->second->removals) result.removedPages.append(key);
    }
    return result;
}

bool KisPagePublicationCoordinator::captureDeltaLocked(
    KisPageTransactionId transaction, KisPageCapturedRelease &capture,
    size_t *versions, size_t *removals, size_t *surfaces) const
{
    const auto found = m_preparedTransactions.find(transaction.value);
    const auto *state = found == m_preparedTransactions.cend() ? nullptr : found->second.get();
    *versions = state ? state->proofs.size() : 0;
    *removals = state ? state->removals.size() : 0;
    *surfaces = state ? size_t(state->surfaceChanges.size()) : 0;
    if (capture.versions.capacity() < *versions || capture.removedPages.capacity() < *removals
        || capture.stagedSurfaces.capacity() < *surfaces) return false;
    capture.versions.clear(); capture.removedPages.clear(); capture.stagedSurfaces.clear();
    if (state) {
        for (const auto &proof : state->proofs) capture.versions.push_back(proof.second.authority.version);
        for (const auto &key : state->removals) capture.removedPages.push_back(key);
        for (const auto &change : state->surfaceChanges) capture.stagedSurfaces.push_back(change.after);
    }
    return true; // One owner cut, using already prepared actual storage only.
}

bool KisPagePublicationCoordinator::stagesRemovalLocked(
    KisPageTransactionId transaction, const KisPageKey &key) const
{
    const auto found = m_preparedTransactions.find(transaction.value);
    return found != m_preparedTransactions.cend() && found->second->removals.count(key);
}

KisPagePublicationCoordinator::KisPreparedOverlayUpdate
KisPagePublicationCoordinator::prepareOverlayUpdateLocked(
    const KisPageTransaction &transaction,
    const OverlayChange *changes, size_t count,
    QString *error)
{
    if (!hasActiveTransactionLocked(transaction) || !changes || !count) {
        KisPageStoreDetail::setError(error, QStringLiteral("overlay update input is invalid"));
        return {};
    }

    try {
        using Data = KisPreparedOverlayUpdate::Data;
        auto storage = KisMutationStorageAllocator<Data>::retained(&m_budget);
        std::unique_ptr<Data, KisPreparedOverlayUpdate::DataDeleter> candidate(nullptr, {storage});
        auto *raw = storage.allocate(1);
        try { std::allocator_traits<decltype(storage)>::construct(storage, raw, storage); }
        catch (...) { auto allocator = storage; allocator.deallocate(raw, 1); throw; }
        candidate.reset(raw);
        candidate->owner = this;
        candidate->transaction = transaction;
        candidate->changes.reserve(count);
        for (size_t i = 0; i < count; ++i) candidate->changes.push_back({changes[i], {}, false, false});
        std::sort(candidate->changes.begin(), candidate->changes.end(), [](const auto &a, const auto &b) {
            return overlayKeyLess(a.replacement.key, b.replacement.key);
        });
        size_t oldProofNodes = 0, oldRemovalNodes = 0;
        const auto base = m_epochs.root(transaction.baseEpoch);
        candidate->state = preparedStateLocked(transaction.id);
        auto &state = *candidate->state;
        candidate->detachments.reserve(std::min(count, state.proofs.size()));
        KisPageKey previous;
        for (auto &entry : candidate->changes) {
            const auto &change = entry.replacement;
            const bool hasProof = change.proof.isValid();
            if (!change.key.isValid() || change.removal == hasProof
                || previous == change.key
                || (hasProof
                    && (!(change.proof.transaction == transaction.id)
                        || !(change.proof.authority.version.key == change.key)
                        || !m_owner.ownsPreparedPageProof(change.proof)))) {
                KisPageStoreDetail::setError(error, QStringLiteral("overlay update change is invalid"));
                return {};
            }
            previous = change.key;
            const auto superseded = findPreparedProofLocked(transaction.id, change.key);
            if (superseded.isValid()) {
                if (!m_owner.ownsPreparedPageProof(superseded)
                    || (hasProof
                        && superseded.authority.version.generation.value
                            >= change.proof.authority.version.generation.value)) {
                    KisPageStoreDetail::setError(error, QStringLiteral("overlay update does not replace its current proof"));
                    return {};
                }
                candidate->detachments.push_back(superseded.authority.version);
            }
            const bool removalWasInBase = change.removal && base.containsPage(change.key);
            const bool wasRemoved = state.removals.count(change.key) != 0;
            if (hasProof && !superseded.isValid())
                candidate->proofNodes.emplace(change.key, change.proof);
            if (removalWasInBase && !wasRemoved)
                candidate->removalNodes.insert(change.key);
            oldProofNodes += change.removal && superseded.isValid();
            oldRemovalNodes += wasRemoved && (hasProof || !removalWasInBase);
            entry.superseded = superseded;
            entry.removalWasInBase = removalWasInBase;
            entry.wasRemoved = wasRemoved;
        }

        candidate->oldProofNodes.reserve(oldProofNodes);
        candidate->oldRemovalNodes.reserve(oldRemovalNodes);

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

void KisPagePublicationCoordinator::revokePreparedProofLocked(
    const KisPreparedPageProof &proof) noexcept
{
    auto transaction = m_preparedTransactions.find(proof.transaction.value);
    if (transaction == m_preparedTransactions.end())
        qFatal("Owned prepared proof has no transaction record");
    auto &state = *transaction->second;
    auto stored = state.proofs.find(proof.authority.version.key);
    // Commit/abort select this proof from the original authority under their
    // preparation claim. Ledger revocation unlinks its paid node and frees it;
    // there is no capacity admission or provider action to retry here.
    if (stored == state.proofs.end() || !(stored->second == proof)
        || !m_owner.revokePreparedPage(proof))
        qFatal("Owned prepared proof changed before revocation");
    state.proofs.erase(stored);
    // Keep the original state/index until commit or abort has fully changed
    // visibility. Their final exit disposes its aggregate outside the gate.
}

bool KisPagePublicationCoordinator::prepareDefaultRevisionsLocked(
    const KisPageSnapshotArray<KisSurfaceEpochState> &surfaces, QString *error) try
{
    if (!m_operational && !m_defaultRevisionHighWater.empty()
        && m_defaultRevisionHighWater.size() == size_t(surfaces.size())
        && std::all_of(m_defaultRevisionHighWater.begin(), m_defaultRevisionHighWater.end(), [&](const auto &entry) {
            return std::count_if(surfaces.begin(), surfaces.end(), [&](const auto &surface) {
                return surface.surface.value == entry.first && surface.defaultPixelRevision == entry.second;
            }) == 1;
        })) {
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    if (!m_defaultRevisionHighWater.empty() || m_operational) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("default pixel revisions are already configured"));
        return false;
    }
    DefaultRevisionMap prepared(m_defaultRevisionHighWater.get_allocator());
    for (const auto &surface : surfaces) {
        if (!surface.surface.isValid() || !surface.defaultPixelRevision
            || !prepared.emplace(surface.surface.value, surface.defaultPixelRevision).second) {
            KisPageStoreDetail::setError(error, QStringLiteral("default pixel revision import is invalid or duplicated"));
            return false;
        }
    }
    m_defaultRevisionHighWater.swap(prepared);
    KisPageStoreDetail::setError(error, {});
    return true;
}
catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("default pixel revision storage budget was refused"));
    return false;
}

quint64 KisPagePublicationCoordinator::currentDefaultRevisionLocked(KisSurfaceId surface) const
{
    const auto found = m_defaultRevisionHighWater.find(surface.value);
    return found == m_defaultRevisionHighWater.end() ? 0 : found->second;
}

bool KisPagePublicationCoordinator::reserveDefaultRevisionLocked(
    KisSurfaceId surface, quint64 revision, QString *error)
{
    auto highWater = m_defaultRevisionHighWater.find(surface.value);
    if (!surface.isValid() || highWater == m_defaultRevisionHighWater.end()
        || revision <= highWater->second) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("default pixel revision must be fresh across abort and restore"));
        return false;
    }
    highWater->second = revision;
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPagePublicationCoordinator::DescriptorMap
KisPagePublicationCoordinator::prepareDescriptorLocked(const KisPageVersion &version,
                                                       const KisPageAllocationDescriptor &descriptor)
{
    const PreparedDescriptorChange change{version, descriptor};
    return prepareDescriptorAdditionsLocked(&change, 1);
}

void KisPagePublicationCoordinator::removeDescriptorLocked(const KisPageVersion &version)
{
    if (m_descriptors.erase(version) != 0)
        ++m_descriptorRevision;
}

const KisPageAllocationDescriptor *KisPagePublicationCoordinator::descriptorLocked(
    const KisPageVersion &version, KisPageAllocationDescriptor *descriptor) const
{
    const auto found = m_descriptors.find(version);
    if (found == m_descriptors.end())
        return nullptr;
    if (descriptor)
        *descriptor = *found->second;
    return found->second.get();
}

KisPagePublicationCoordinator::DescriptorMap
KisPagePublicationCoordinator::prepareDescriptorAdditionsLocked(const PreparedDescriptorChange *changes, size_t count)
{
    const auto storage = KisMutationStorageAllocator<DescriptorMap::value_type>::retained(&m_budget);
    if (m_descriptors.get_allocator() != storage) {
        Q_ASSERT(m_descriptors.empty());
        m_descriptors = DescriptorMap(VersionLess{}, storage);
    }
    DescriptorMap nodes(VersionLess{}, storage);
    if (!count) return nodes;
    for (size_t i = 0; i < count; ++i) {
        const auto found = m_descriptors.find(changes[i].version);
        if (found != m_descriptors.end() && *found->second == changes[i].descriptor) continue;
        // The original index owns immutable descriptor values. Equal surface
        // layouts share that value instead of paying for a copy per version;
        // the candidate is storage only, never another visible descriptor map.
        std::shared_ptr<const KisPageAllocationDescriptor> value;
        for (const auto *index : {&m_descriptors, &nodes}) {
            const auto same = std::find_if(index->cbegin(), index->cend(), [&](const auto &entry) {
                return *entry.second == changes[i].descriptor;
            });
            if (same != index->cend()) { value = same->second; break; }
        }
        if (!value) value = std::allocate_shared<const KisPageAllocationDescriptor>(
            KisMutationStorageAllocator<KisPageAllocationDescriptor>(storage), changes[i].descriptor);
        nodes.emplace(changes[i].version, std::move(value));
    }
    // The original ordered index consumes these exact nodes. Overlapping
    // preparation cannot exhaust buckets or require installation growth.
    if (!nodes.empty()) ++m_statistics.descriptorCapacityPreparations;
    return nodes;
}

void KisPagePublicationCoordinator::installDescriptorAdditionsLocked(DescriptorMap *prepared)
{
    Q_ASSERT(prepared->empty() || prepared->get_allocator() == m_descriptors.get_allocator());
    for (auto it = prepared->begin(); it != prepared->end();) {
        auto current = it++;
        auto found = m_descriptors.find(current->first);
        if (found == m_descriptors.end()) {
            // The actual node already exists; transfer never allocates or
            // destroys a candidate value during installation.
            m_descriptors.insert(prepared->extract(current));
            ++m_descriptorRevision;
        } else if (!(*found->second == *current->second)) {
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
        result.preparedProofs += qsizetype(prepared.second->proofs.size());
        result.preparedSurfaceChanges += qsizetype(prepared.second->surfaceChanges.size());
        result.stagedPageRemovals += qsizetype(prepared.second->removals.size());
    }
    result.committedTransactions = m_committedTransactions;
    return result;
}

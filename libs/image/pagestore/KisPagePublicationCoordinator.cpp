/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPagePublicationCoordinator_p.h"
#include "KisPageStoreDiagnostics_p.h"
#include "KisPageWriteCoordinator_p.h"
#include "KisPageReadCoordinator_p.h"
#include "KisPageMetadataCoordinator_p.h"

#include <QScopeGuard>
#include <QDeadlineTimer>

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
                                     Changes *changes, quint64 *idleAllowance = nullptr)
{
    KisPageVersion current;
    if (!currentRoot.resolve(target.key, &current))
        return false;
    if (current == target)
        return true;
    bool detached = false;
    KisPageRetirementRecord **idleResult = nullptr;
    const KisReplicaHandle oldAuthority = publication.backingAuthority(current, &detached, &idleResult);
    const KisReplicaHandle targetAuthority = publication.backingAuthority(target);
    if ((!oldAuthority.isValid() && !current.isDefaultPixel())
        || (!targetAuthority.isValid() && !target.isDefaultPixel()))
        return false;
    // Eligible detached capacity is prepaid as idle. If optional admission
    // fails, the same member keeps Current until the original retirement
    // record obtains Debt; cache retention must not prevent publication.
    const bool cacheDetached = detached && idleAllowance && oldAuthority.isValid()
        && oldAuthority.layout.byteSize <= *idleAllowance;
    if (cacheDetached) *idleAllowance -= oldAuthority.layout.byteSize;
    if (oldAuthority.isValid() && (!detached || cacheDetached))
        changes->push_back({oldAuthority, KisBackingBudgetClass::Current,
                           detached ? KisBackingBudgetClass::IdleCapacity : oldAfter,
                           cacheDetached ? idleResult : nullptr});
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
    PreparedDescriptors requested;
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
        PreparedDescriptors published;
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
        : descriptorChanges(storage), descriptorNodes(PreparedDescriptors::allocator_type(storage))
        , directoryHeads(storage), restoreTargets(storage), publicationChanges(storage), backingChanges(storage) {}
    KisPagePublicationCoordinator *owner = nullptr;
    KisPageTransaction transaction;
    KisPageMetadataCoordinator::PreparedPublication metadata;
    KisImageEpochReferenceModel::PreparedCommit epoch;
    KisImageEpochReferenceModel::PreparedRootReservation restoreEpoch;
    std::vector<PreparedDescriptorChange, KisMutationStorageAllocator<PreparedDescriptorChange>> descriptorChanges;
    PreparedDescriptors descriptorNodes;
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

class KisPagePublicationCoordinator::KisPreparedPagePublication::Data
{
public:
    using Change = KisPagePublicationChange;

    explicit Data(const KisMutationStorageAllocator<char> &storage)
        : changes(storage)
        , proofNodes(0, PageHash{}, std::equal_to<KisPageKey>{}, storage)
        , removalNodes(0, PageHash{}, std::equal_to<KisPageKey>{}, storage)
        , oldProofNodes(storage), oldRemovalNodes(storage) {}

    KisPagePublicationCoordinator *owner = nullptr;
    KisPageTransaction transaction;
    KisPagePublicationChanges changes;
    size_t mutationCount = 0;
    std::shared_ptr<KisPagePublicationDelta> state;
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
    bool canonical = false;
    bool nativeRows = false;
    bool retryNativeCapacity = false;

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

void KisPagePublicationCoordinator::KisPreparedPagePublication::DataDeleter::operator()(Data *value) const noexcept
{
    if (!value) return;
    value->~Data();
    auto allocator = storage;
    allocator.deallocate(value, 1);
}

KisPagePublicationCoordinator::KisPreparedPagePublication::
    KisPreparedPagePublication() = default;

KisPagePublicationCoordinator::KisPreparedPagePublication::
    ~KisPreparedPagePublication()
{
    cancel();
}

KisPagePublicationCoordinator::KisPreparedPagePublication::
    KisPreparedPagePublication(KisPreparedPagePublication &&) noexcept = default;

KisPagePublicationCoordinator::KisPreparedPagePublication &
KisPagePublicationCoordinator::KisPreparedPagePublication::operator=(
    KisPreparedPagePublication &&other) noexcept
{
    if (this != &other) {
        cancel();
        data = std::move(other.data);
    }
    return *this;
}

bool KisPagePublicationCoordinator::KisPreparedPagePublication::isValid() const
{
    return data && data->owner && data->transaction.isValid()
        && !data->changes.empty() && !data->installed;
}

qsizetype KisPagePublicationCoordinator::KisPreparedPagePublication::
    metadataChangeCount() const
{
    return isValid() ? data->mutationCount : 0;
}

bool KisPagePublicationCoordinator::KisPreparedPagePublication::forEachAdapterChange(
    const void *visitor, bool (*visit)(const void *, const KisPageKey &, bool)) const
{
    Q_ASSERT(data);
    for (const auto &change : data->changes)
        if (!visit(visitor, change.replacement.key, !change.replacement.removal)) return false;
    return true;
}

bool KisPagePublicationCoordinator::KisPreparedPagePublication::acceptsCanonical() const
{
    return isValid() && data->canonical;
}

bool KisPagePublicationCoordinator::KisPreparedPagePublication::tryAcceptCanonicalLocked(
    const KisPageStore *diagnosticOwner, QMutexLocker<QMutex> &ownerLock)
{
    if (!acceptsCanonical() || !data->prepared || !data->surfacePrepared) return false;
    auto &owner = *data->owner;
    if (!owner.hasActiveTransactionLocked(data->transaction)
        || owner.isPreparingCommitLocked(data->transaction.id)
        || owner.hasPreparedDeltaLocked(data->transaction.id)) return false;
    size_t proofs = 0, removals = 0;
    for (const auto &change : data->changes) {
        proofs += !change.replacement.removal;
        removals += change.replacement.removal && change.removalWasInBase;
    }
    const KisPageOperationPublication input{{data->changes, proofs}, {data->changes, removals},
        {&data->surfaceChange, size_t(data->surfaceChange.after.surface.isValid())}};
    if (!proofs && !removals && !input.surfaceChanges.size()) {
        data->installed = owner.m_epochs.abort(data->transaction);
    } else {
        data->installed = owner.commitInputLocked(data->transaction, input, nullptr, diagnosticOwner, ownerLock).isValid();
    }
    return data->installed;
}

bool KisPagePublicationCoordinator::KisPreparedPagePublication::prepare(
    QString *error, bool *storageRefused)
{
    if (storageRefused) *storageRefused = false;
    if (!isValid() || data->prepared) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("overlay update cannot be prepared"));
        return false;
    }
    data->nativeRows = !data->retryNativeCapacity && data->mutationCount && KisPageMetadataCoordinator::canBorrowNativeMutation(
        data->transaction, data->changes.data(), qsizetype(data->changes.size()));
    if (data->mutationCount && !data->nativeRows) {
        data->metadata = data->owner->m_metadata.prepareMutation(
            data->transaction, data->changes.data(), qsizetype(data->changes.size()), error);
        if (!data->metadata.isValid()) {
            if (storageRefused) *storageRefused = data->metadata.wasStorageRefused();
            return false;
        }
        for (auto &change : data->changes) change.releasesCapacity = false;
        data->metadata.forEachCapacityRelease(data.get(), [](void *context, size_t i) {
            static_cast<Data *>(context)->changes.at(i).releasesCapacity = true;
        });
    }
    data->prepared = true;
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPagePublicationCoordinator::KisPreparedPagePublication::tryAcceptLocked(
    KisPageMetadataCoordinator::DeferredPublicationCleanup *cleanup,
    QMutexLocker<QMutex> &ownerLock, QString *error, bool *storageRefused)
{
    if (installOverlayLocked(cleanup, error)) return true;
    if (!cleanup || (!cleanup->needsCapacityReprepare() && !data->retryNativeCapacity)) return false;
    // One common retry for native seal, generic publication and removal.
    // Keep pixels, proofs and unaffected release permissions. Full-D claims
    // are repeated, but only invalidated rows need additional capacity.
    bool prepared = false;
    ownerLock.unlock();
    {
        const auto relock = qScopeGuard([&] { ownerLock.relock(); });
        cleanup->forEachCapacityInvalidation(data.get(), [](void *context, size_t i) {
            static_cast<Data *>(context)->changes.at(i).releasesCapacity = false;
        });
        *cleanup = {};
        data->prepared = false;
        data->surfacePrepared = false;
        prepared = prepare(error, storageRefused);
        data->retryNativeCapacity = false;
    }
    return prepared && prepareSurfaceLocked(error, storageRefused) && installOverlayLocked(cleanup, error);
}

bool KisPagePublicationCoordinator::KisPreparedPagePublication::prepareSurfaceLocked(
    QString *error, bool *storageRefused)
{
    if (storageRefused) *storageRefused = false;
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
            removals.reserve((data->state ? data->state->removals.size() : 0) + data->changes.size());
            additions.reserve((data->state ? data->state->proofs.size() : 0) + data->changes.size());
            if (data->state) {
                for (const auto &key : data->state->removals) removals.push_back(key);
                for (const auto &proof : data->state->proofs) {
                    const auto changed = data->findChange(proof.first);
                    if (!changed || !changed->replacement.removal) additions.push_back(proof.first);
                }
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
            if (data->state) {
                auto &changes = data->state->surfaceChanges;
                for (size_t i = 0; i < changes.size(); ++i)
                    if (changes.at(i).after.surface == surface) { data->surfaceIndex = qsizetype(i); break; }
                if (data->surfaceIndex < 0) changes.reserve(changes.size() + 1);
            }
            data->surfaceChange = std::move(change);
        }
        data->surfacePrepared = true;
        KisPageStoreDetail::setError(error, {});
        return true;
    } catch (const std::bad_alloc &) {
        if (storageRefused) *storageRefused = true;
        KisPageStoreDetail::setError(error, QStringLiteral("overlay surface storage preparation was refused"));
        return false;
    }
}

bool KisPagePublicationCoordinator::KisPreparedPagePublication::
    installOverlayLocked(
        KisPageMetadataCoordinator::DeferredPublicationCleanup *metadataCleanup,
        QString *error)
{
    if (!isValid() || data->canonical || !data->prepared || !data->surfacePrepared || !metadataCleanup) {
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
        const auto found = state.proofs.find(change.replacement.key);
        const KisPagePublicationProof current = found == state.proofs.end() ? KisPagePublicationProof{} : found->second.proof;
        if (!(current == change.superseded)
            || (state.removals.count(change.replacement.key) != 0) != change.wasRemoved
            || (current.isValid() && !current.native
                && !owner.m_owner.ownsPreparedPageProof(owner.projectProof(current)))
            || (change.replacement.proof.isValid() && !change.replacement.proof.native
                && !owner.m_owner.ownsPreparedPageProof(owner.projectProof(change.replacement.proof)))) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("overlay update source changed"));
            return false;
        }
    }

    // The frozen release set is the only input to optional fees. Admission
    // refusal keeps the original charge and terminal owner; publication is
    // still legal. No physical or fee decision is invented after acceptance.
    KisBackingClassChangeReservation capacityReservation;
    for (size_t i = 0; i < data->changes.size(); ++i) {
        if (!data->changes[i].releasesCapacity) continue;
        const auto allowance = owner.idleAllowanceLocked();
        if (allowance)
            capacityReservation = owner.m_owner.prepareIdleCapacity(
                data->changes.data() + i, data->changes.size() - i, allowance);
        break;
    }

    if (data->nativeRows) {
        const auto accepted = owner.m_metadata.acceptNativeMutation(data->transaction,
            data->changes.data(), qsizetype(data->changes.size()), error);
        data->retryNativeCapacity = accepted == KisPageMetadataCoordinator::NativeAcceptance::NeedsCapacity;
        if (accepted != KisPageMetadataCoordinator::NativeAcceptance::Accepted) return false;
    } else {
        if (data->mutationCount
            && !owner.m_metadata.installMutation(std::move(data->metadata), data->transaction, error, metadataCleanup))
            return false;
    }
    if (capacityReservation.isValid())
        owner.m_owner.commitBackingChanges(std::move(capacityReservation));

    // Every missing node and all buckets were constructed before metadata
    // installation. Keep extracted nodes alive until unlocked cleanup.
    for (const Data::Change &change : std::as_const(data->changes)) {
        const auto &key = change.replacement.key;
        auto previous = state.proofs.find(key);
        if (change.releasesCapacity)
            owner.acceptCapacityLocked(change.supersededAuthority, change.acceptedIdle);
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
                previous->second.proof = change.replacement.proof;
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
    if (!owner.m_idleCapacity.empty()) owner.m_history.scheduleLocked();
    ++owner.m_statistics.overlayAcceptances;
    KisPageStoreDetail::setError(error, {});
    return true;
}

void KisPagePublicationCoordinator::KisPreparedPagePublication::finishUnlocked(
    KisPageMetadataCoordinator::DeferredPublicationCleanup cleanup,
    QMutexLocker<QMutex> &ownerLock)
{
    Q_ASSERT(!ownerLock.isLocked());
    if (!data) {
        Q_ASSERT(cleanup.isEmpty());
        return;
    }
    auto &owner = *data->owner;
    // Candidate activities can otherwise keep detached metadata alive during
    // collection. Release or hand off that storage before requesting history.
    owner.m_disposeMetadataCleanup(owner.m_ownerContext, std::move(cleanup));
    if (data->installed) {
        ownerLock.relock();
        // Only accepted replacements transfer the superseded proofs. Rejected
        // preparation still borrows its producer's proofs and must not revoke.
        bool historyRequested = false;
        for (const auto &change : std::as_const(data->changes)) {
            if (!change.superseded.isValid() || change.releasesCapacity) continue;
            const bool revoked = owner.m_owner.revokePreparedPage(owner.projectProof(change.superseded));
            Q_ASSERT(revoked);
            Q_UNUSED(revoked);
            owner.m_history.requestKeyLocked(change.replacement.key);
            historyRequested = true;
        }
        if (historyRequested)
            owner.m_history.collectUnreachableLocked();
        ownerLock.unlock();
    }
    // Consume the allocator retention as well as its payload. An empty
    // unique_ptr can still keep the charged controller alive in its deleter.
    data = decltype(data){};
}

void KisPagePublicationCoordinator::KisPreparedPagePublication::cancel() noexcept
{
    Q_ASSERT(!data || !data->installed);
    // Replacements belong to their original page/lease until install accepts
    // them. Discarding preparation releases capacity, never those proofs.
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

bool KisPagePublicationCoordinator::KisPreparedMutationCommit::tryInstall(KisRetainedImageEpochSnapshot *retainedAfter)
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
        : owner.m_epochs.installCommit(std::move(candidate.epoch), &candidate, installMetadata, retainedAfter);

    if (candidate.result.isCommitted()) {
        owner.m_owner.commitBackingChanges(std::move(candidate.backingReservation));
        candidate.metadataCleanup.forEachDetachedBacking(&owner, [](void *context, const KisReplicaHandle &replica,
                                                                   KisPageRetirementRecord *record) {
            auto &coordinator = *static_cast<KisPagePublicationCoordinator *>(context);
            coordinator.acceptCapacityLocked(replica, record);
            ++coordinator.m_statistics.detachedCurrentBackings;
        });
        if (!owner.m_idleCapacity.empty()) owner.m_history.scheduleLocked();
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
            *version = proof->second.proof.version;
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
    auto update = preparePagePublicationLocked(transaction, &removal, 1, error);
    KisPageMetadataCoordinator::DeferredPublicationCleanup metadataCleanup;
    auto dispose = qScopeGuard([&] {
        ownerLock.unlock();
        update.finishUnlocked(std::move(metadataCleanup), ownerLock);
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
    if (!update.prepareSurfaceLocked(error) || !update.tryAcceptLocked(&metadataCleanup, ownerLock, error)) return false;
    ownerLock.unlock();
    update.finishUnlocked(std::move(metadataCleanup), ownerLock);
    ownerLock.relock();
    dispose.dismiss();
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

bool KisPagePublicationCoordinator::transactionDirtyExtentLocked(
    const KisPageTransaction &transaction, KisSurfaceId surface, QRect *extent) const
{
    if (!extent || !hasActiveTransactionLocked(transaction)) return false;
    KisSurfaceEpochState state;
    if (!resolveSurfaceLocked(surface, KisPageReadView::transactionOverlay(transaction.id), &state)) return false;
    qint64 left = std::numeric_limits<qint32>::max(), top = left;
    qint64 right = std::numeric_limits<qint32>::min(), bottom = right;
    bool changed = false;
    const auto include = [&](const KisPageKey &key) {
        if (!(key.surface == surface)) return;
        changed = true;
        left = qMin(left, qint64(key.page.column)); right = qMax(right, qint64(key.page.column));
        top = qMin(top, qint64(key.page.row)); bottom = qMax(bottom, qint64(key.page.row));
    };
    const auto found = m_preparedTransactions.find(transaction.id.value);
    if (found != m_preparedTransactions.cend()) {
        for (const auto &entry : found->second->proofs) include(entry.first);
        for (const auto &key : found->second->removals) include(key);
    }
    if (!changed) { *extent = {}; return true; }
    left *= state.logicalPageExtent.width(); right = (right + 1) * state.logicalPageExtent.width() - 1;
    top *= state.logicalPageExtent.height(); bottom = (bottom + 1) * state.logicalPageExtent.height() - 1;
    if (left < std::numeric_limits<qint32>::min() || top < std::numeric_limits<qint32>::min() ||
        right > std::numeric_limits<qint32>::max() || bottom > std::numeric_limits<qint32>::max() ||
        right - left >= std::numeric_limits<qint32>::max() || bottom - top >= std::numeric_limits<qint32>::max()) return false;
    *extent = QRect(QPoint(qint32(left), qint32(top)), QPoint(qint32(right), qint32(bottom)));
    return true;
}

bool KisPagePublicationCoordinator::captureAllocatedPageInputsLocked(
    KisSurfaceId surface, const KisPageTransaction &transaction, KisImageEpochRootSnapshot *root,
    KisPageKeyStorage *additions, KisPageKeyStorage *removals) const
{
    if (!m_operational || !surface.isValid()) return false;
    if (!transaction.isValid()) {
        *root = m_epochs.captureCommittedRoot();
        return root->isValid();
    }
    if (!hasActiveTransactionLocked(transaction)) return false;
    *root = m_epochs.root(transaction.baseEpoch);
    const auto found = m_preparedTransactions.find(transaction.id.value);
    if (found != m_preparedTransactions.cend()) {
        // One owner cut; export only keys needed by this full enumeration.
        // No proof/provider/surface facts or pixel retention are required.
        const auto &delta = *found->second;
        additions->reserve(delta.proofs.size());
        removals->reserve(delta.removals.size());
        for (const auto &entry : delta.proofs)
            if (entry.first.surface == surface) additions->push_back(entry.first);
        for (const auto &key : delta.removals)
            if (key.surface == surface) removals->push_back(key);
    }
    return root->isValid();
}

KisImageEpochCommitTicket KisPagePublicationCoordinator::commitLocked(const KisPageTransaction &transaction,
                                                                      const KisPreparedPageSet &preparedPages,
                                                                      KisRetainedImageEpochSnapshot *retainedAfter,
                                                                      const KisPageStore *diagnosticOwner,
                                                                      QMutexLocker<QMutex> &ownerLock)
{
    // External snapshots are untrusted commit input. Internal producers do
    // not manufacture this snapshot just to compare it with its own owner.
    if (retainedAfter) *retainedAfter = {};
    {
        KisPageStoreDiagnosticTimer validation(diagnosticOwner, KisPageStoreDiagnosticPhase::CommitProofValidation,
                                               quint64(preparedPages.proofs.size()));
        if (!preparedPages.isValid() || !(preparedPages.transaction == transaction.id)) return {};
        const auto found = m_preparedTransactions.find(transaction.id.value);
        if (found == m_preparedTransactions.cend()) return {};
        const auto &owned = *found->second;
        if (owned.proofs.size() != size_t(preparedPages.proofs.size()) ||
            owned.surfaceChanges.size() != size_t(preparedPages.surfaceChanges.size()) ||
            owned.removals.size() != size_t(preparedPages.removedPages.size())) return {};
        for (const auto &proof : preparedPages.proofs) {
            const auto original = owned.proofs.find(proof.authority.version.key);
            if (original == owned.proofs.cend() || (!original->second.proof.matches(proof) || !m_owner.ownsPreparedPageProof(proof))) return {};
        }
        for (const auto &change : preparedPages.surfaceChanges)
            if (std::find(owned.surfaceChanges.cbegin(), owned.surfaceChanges.cend(), change) == owned.surfaceChanges.cend()) return {};
        for (const auto &key : preparedPages.removedPages)
            if (!owned.removals.count(key)) return {};
    }
    return commitOwnedLocked(transaction, retainedAfter, diagnosticOwner, ownerLock);
}

bool KisPagePublicationCoordinator::hasPreparedDeltaLocked(KisPageTransactionId transaction) const
{
    const auto found = m_preparedTransactions.find(transaction.value);
    return found != m_preparedTransactions.cend() && found->second->hasChanges();
}

KisImageEpochCommitTicket KisPagePublicationCoordinator::commitOwnedLocked(
    const KisPageTransaction &transaction, KisRetainedImageEpochSnapshot *retainedAfter,
    const KisPageStore *diagnosticOwner, QMutexLocker<QMutex> &ownerLock)
{
    if (retainedAfter)
        *retainedAfter = {};
    if (!hasActiveTransactionLocked(transaction) ||
        m_transactionHasMutationActivity(m_ownerContext, transaction.id) || isPreparingCommitLocked(transaction.id)) {
        return {};
    }
    const auto found = m_preparedTransactions.find(transaction.id.value);
    // Storage preparation may leave an empty original transaction record
    // after refusal or an implicit-default removal. It is not a delta.
    if (found == m_preparedTransactions.cend() || !found->second->hasChanges())
        return {};
    return commitInputLocked(transaction, *found->second, retainedAfter, diagnosticOwner, ownerLock);
}

static const KisPagePublicationProof &publicationIdentity(const KisPagePublicationChange::Replacement &entry) { return entry.proof; }
static const KisPagePublicationProof &publicationIdentity(const KisPagePublicationDelta::ProofMap::value_type &entry)
{
    return entry.second.proof;
}

template<class Input>
KisImageEpochCommitTicket KisPagePublicationCoordinator::commitInputLocked(
    const KisPageTransaction &transaction, const Input &owned, KisRetainedImageEpochSnapshot *retainedAfter,
    const KisPageStore *diagnosticOwner, QMutexLocker<QMutex> &ownerLock)
{
    using Phase = KisPageStoreDiagnosticPhase;
    const auto publicationProof = [this](const auto &entry) {
        if constexpr (std::is_same_v<Input, KisPageOperationPublication>) return this->projectProof(entry);
        else return this->projectProof(publicationIdentity(entry));
    };
    const quint64 pageWork = quint64(owned.proofs.size() + owned.removals.size());
    KisPageStoreDiagnosticTimer diagnostic(diagnosticOwner, Phase::CommitOwnerWait, pageWork);
    diagnostic.next(Phase::CommitProofValidation, quint64(owned.proofs.size()));
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
        proofDescriptors.reserve(owned.proofs.size());
        for (const auto &entry : owned.proofs) {
            auto descriptor = m_metadata.versionDescriptor(publicationIdentity(entry).version);
            if (!descriptor)
                return {};
            proofDescriptors.push_back(std::move(descriptor));
        }
    } catch (const std::bad_alloc &) { return {}; }
    const bool changesSurfaceDefault = std::any_of(owned.surfaceChanges.cbegin(), owned.surfaceChanges.cend(),
        [](const auto &change) { return change.before.defaultPixelRevision != change.after.defaultPixelRevision; });

    ownerLock.unlock();
    diagnostic.next(Phase::CommitProviderValidation, quint64(proofDescriptors.size()));
    bool proofsValid = true;
    size_t proofIndex = 0;
    for (const auto &entry : owned.proofs) {
        if (!m_owner.validatePreparedPage(m_metadata, publicationProof(entry), *proofDescriptors.at(proofIndex++))) {
            proofsValid = false;
            break;
        }
    }
    proofDescriptors = decltype(proofDescriptors){proofDescriptors.get_allocator()};
    diagnostic.next(Phase::CommitEpochDeltaPrepare, pageWork);
    const bool deltaPrepared = proofsValid && m_epochs.preparePublication(transaction.id, owned);
    diagnostic.next(Phase::CommitInputOwnerWait, pageWork);
    ownerLock.relock();
    if (!deltaPrepared)
        return {};

    bool disableDirectDetachment = false;
    for (;;) {
        diagnostic.next(Phase::CommitDefaultRemovalPreparation, quint64(owned.removals.size()));
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
            const size_t descriptorCount = size_t(replacements) + owned.removals.size();
            descriptorChanges.reserve(descriptorCount);
            publicationChanges.reserve(owned.proofs.size() + descriptorCount);
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
        for (const KisPageKey &key : owned.removals) {
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
                        quint64(owned.proofs.size() + publicationChanges.size()));
        for (const auto &entry : owned.proofs) {
            publicationChanges.push_back({KisPageTransitionKind::CommitTransaction, publicationIdentity(entry).version, {}});
        }
        const qsizetype descriptorAdditions = descriptorChanges.size();
        try { preparedCommit.data->descriptorNodes = prepareDescriptorAdditionsLocked(descriptorChanges.data(), descriptorChanges.size()); }
        catch (const std::bad_alloc &) { return {}; }
        const quint64 descriptorRevision = m_descriptorRevision;
        ownerLock.unlock();
        registeredPages = decltype(preparedCommit.data->directoryHeads){registeredPages.get_allocator()};
        const quint64 publicationTransitionCount = quint64(publicationChanges.size());
        diagnostic.next(Phase::CommitEpochPrepare, pageWork);
        KisImageEpochCommitResult candidateFailure;
        auto &rootCandidate = preparedCommit.data->epoch;
        rootCandidate = m_epochs.prepareCommit(transaction, &candidateFailure, bool(retainedAfter), m_derivedExtentSurface);
        diagnostic.next(Phase::CommitTransitionPreflight, publicationTransitionCount);
        auto &publication = preparedCommit.data->metadata;
        publication = m_metadata.preparePublication(transaction, prospectiveEpoch,
            publicationChanges.data(), qsizetype(publicationChanges.size()), nullptr,
            !disableDirectDetachment && rootCandidate.mayDetachPreviousRoot());
        publicationChanges = decltype(preparedCommit.data->publicationChanges){publicationChanges.get_allocator()};
        const bool detachesBacking = publication.hasDetachedBacking();
        if (detachesBacking) rootCandidate.requireDetachedPreviousRoot();
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
        const auto prepareFees = [&](bool cacheDetached) -> KisBackingClassChangeReservation {
          try {
            quint64 allowance = cacheDetached ? idleAllowanceLocked() : 0;
            backingChanges.clear();
            backingChanges.reserve(owned.proofs.size() * 2 + descriptorChanges.size() * 2);
            for (const auto &entry : owned.proofs) {
                if (!appendPublicationBackingChanges(
                    publication, currentRoot, publicationIdentity(entry).version, KisBackingBudgetClass::ActivePending,
                    KisBackingBudgetClass::RetainedHistory,
                    &backingChanges, &allowance)) return {};
            }
            for (const PreparedDescriptorChange &target : std::as_const(descriptorChanges)) {
                if (!appendPublicationBackingChanges(
                    publication, currentRoot, target.version,
                    KisBackingBudgetClass::RetainedHistory, KisBackingBudgetClass::RetainedHistory,
                    &backingChanges, &allowance)) return {};
            }
            return m_owner.prepareBackingChanges(std::move(backingChanges), nullptr, 0, nullptr);
          } catch (const std::bad_alloc &) { return {}; }
        };
        auto backingReservation = prepareFees(detachesBacking);
        if (!backingReservation.isValid() && detachesBacking)
            backingReservation = prepareFees(false);
        if (!backingReservation.isValid() && detachesBacking) {
            // Keeping the old physical charge until retirement has a real
            // Current peak. If refused, preserve the original history plan;
            // no pixels or accepted work are replayed by this cold downgrade.
            disableDirectDetachment = true;
            ++m_statistics.detachmentBudgetFallbacks;
            continue;
        }
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
        const bool installed = preparedCommit.tryInstall(retainedAfter);
        const bool metadataRejected =
            preparedCommit.data->result.status == KisImageEpochCommitStatus::Rejected;
        const KisImageEpochCommitResult committed = preparedCommit.data->result;
        auto metadataCleanup = std::move(preparedCommit.data->metadataCleanup);
        if (installed)
            diagnostic.next(Phase::CommitRootPublication, pageWork);
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

        // Acceptance consumes every proof at once. Keep the original input
        // intact for history key delivery, then release storage outside the gate.
        // Abort instead erases each accepted revocation so refusal can resume.
        for (const auto &entry : owned.proofs) {
            if (!m_owner.revokePreparedPage(publicationProof(entry)))
                qFatal("Owned committed proof changed before revocation");
        }
        auto releasedOverlay = m_preparedTransactions.extract(transaction.id.value);
        for (const auto &entry : owned.proofs) {
            m_history.requestKeyLocked(publicationIdentity(entry).version.key);
        }
        for (const PreparedDescriptorChange &change : std::as_const(preparedCommit.data->descriptorChanges)) {
            m_history.requestKeyLocked(change.version.key);
        }
        diagnostic.next(Phase::CommitHistoryCollect, quint64(owned.proofs.size() + descriptorChanges.size()));
        // Ending a non-current base affects other keys only when its last
        // logical protection is gone. A live sibling or retained snapshot
        // keeps the same reachability; do not rescan H keys for that exit.
        // The Store gate excludes new protection changes across this query.
        const bool releasedOldBase = !(transaction.baseEpoch == currentRoot.epoch())
            && !m_epochs.root(transaction.baseEpoch).isValid();
        m_history.collectAfterEpochChangeLocked(releasedOldBase);
        diagnostic.next(Phase::CommitProviderRetire, 0);
        processRetirementsUnlocked(ownerLock);
        ++m_committedTransactions;
        if constexpr (std::is_same_v<Input, KisPageOperationPublication>)
            ++m_statistics.canonicalOperationCommits;
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
                                                          const KisPageStore *diagnosticOwner,
                                                          QMutexLocker<QMutex> &ownerLock,
    const std::function<bool(const KisImageEpochRootSnapshot &)> &prepareAdapter,
    const std::function<void(const KisPageVersion *, qsizetype)> &completeAdapter)
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
    bool adapterPrepared = false;
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
            if (!retainedRoot.appendRestoreTargets(currentRoot, &restoreTargets)) return {};
            if (changesDefaults) {
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
                    if (!defaultChanged(page.key.surface) || !page.isDefaultPixel()) {
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
        // Current/retained roots are fixed across retries. Directory growth
        // can only add virtual-default targets; it cannot add cold allocated
        // wrappers. The caller's original manager gate preserves prepared
        // table identity and warm coverage until the successful delivery.
        if (!adapterPrepared) {
            if (prepareAdapter && !prepareAdapter(retainedRoot)) return {};
            adapterPrepared = true;
        }
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
        const bool directoryChanged = changesDefaults
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
        if (installed && completeAdapter)
            completeAdapter(restoreTargets.data(), qsizetype(restoreTargets.size()));
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
    m_history.collectAfterEpochChangeLocked();
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
        const KisPreparedPageProof proof = projectProof(remainingProofs->cbegin()->second.proof);
        if (!m_owner.ownsPreparedPageProof(proof))
            qFatal("Owned abort source proof changed before metadata transition");
        KisPageTransition transition;
        transition.kind = KisPageTransitionKind::AbortTransaction;
        transition.transaction = transaction.id;
        const KisPageMetadataTransitionResult result = m_metadata.applyOwner(proof.authority.version.key, transition);
        if (!result.accepted)
            return false;
        finishAbortedProofLocked(proof);
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
    m_history.collectAfterEpochChangeLocked(!m_epochs.root(transaction.baseEpoch).isValid());
    return true;
}

bool KisPagePublicationCoordinator::isPreparingCommitLocked(KisPageTransactionId transaction) const
{
    return std::any_of(m_preparingCommits.cbegin(), m_preparingCommits.cend(), [&](const auto &claim) {
        return claim.transaction == transaction;
    });
}

std::shared_ptr<KisPagePublicationDelta>
KisPagePublicationCoordinator::preparedStateLocked(KisPageTransactionId transaction)
{
    auto found = m_preparedTransactions.find(transaction.value);
    if (found != m_preparedTransactions.end()) return found->second;
    const auto storage = KisMutationStorageAllocator<KisPagePublicationDelta>::retained(&m_budget);
    auto state = std::allocate_shared<KisPagePublicationDelta>(storage, storage);
    m_preparedTransactions.emplace(transaction.value, state);
    return state;
}

const KisPagePublicationCoordinator::ProofMap *
KisPagePublicationCoordinator::findProofsLocked(KisPageTransactionId transaction) const
{
    const auto found = m_preparedTransactions.find(transaction.value);
    return found == m_preparedTransactions.cend() || found->second->proofs.empty() ? nullptr : &found->second->proofs;
}

KisPreparedPageProof KisPagePublicationCoordinator::projectProof(const KisPagePublicationProof &proof) const
{
    return proof.isValid() ? m_owner.projectPreparedPage(proof.version, proof.transaction, proof.stamp, proof.allocation)
        : KisPreparedPageProof{};
}

KisPreparedPageProof KisPagePublicationCoordinator::projectProof(
    const KisPagePublicationChange::Replacement &entry) const
{
    if (!entry.native || !entry.proof.native) return projectProof(entry.proof);
    // Canonical input still borrows its original result, including after
    // metadata accepts it. Do not discard this fact and look it up again.
    const auto value = entry.native->proofValue(m_readyHostCompletion);
    return entry.proof.matches(value) ? value : KisPreparedPageProof{};
}

KisPageVersion KisPagePublicationCoordinator::findPreparedVersionLocked(
    KisPageTransactionId transaction, const KisPageKey &key) const
{
    const auto *proofs = findProofsLocked(transaction);
    if (!proofs) return {};
    const auto found = proofs->find(key);
    return found == proofs->end() ? KisPageVersion{} : found->second.proof.version;
}

KisPreparedPageSet
KisPagePublicationCoordinator::transactionDeltaLocked(KisPageTransactionId transaction) const
{
    KisPreparedPageSet result;
    result.transaction = transaction;
    const auto found = m_preparedTransactions.find(transaction.value);
    if (found != m_preparedTransactions.cend()) {
        result.proofs.reserve(qsizetype(found->second->proofs.size()));
        for (const auto &proof : found->second->proofs) result.proofs.append(projectProof(proof.second.proof));
        result.surfaceChanges.reserve(qsizetype(found->second->surfaceChanges.size()));
        for (const auto &change : found->second->surfaceChanges) result.surfaceChanges.append(change);
        result.removedPages.reserve(qsizetype(found->second->removals.size()));
        for (const auto &key : found->second->removals) result.removedPages.append(key);
    }
    return result;
}

KisReplicaHandle KisPagePublicationCoordinator::takeIdleCapacityLocked(const KisPageKey &key)
{
    const qint64 now = QDeadlineTimer::current().deadline();
    expireIdleCapacityLocked(now);
    auto *record = m_metadata.exchangeIdleCapacity(key, nullptr);
    if (!record) return {};
    Q_ASSERT(record->is_linked());
    m_idleCapacity.erase(m_idleCapacity.iterator_to(*record));
    record->idleDeadline = 0;
    return record->replica; // Ledger retains the record until retag or retirement.
}

qint64 KisPagePublicationCoordinator::expireIdleCapacityLocked(qint64 now, bool all)
{
    while (!m_idleCapacity.empty()) {
        auto &record = m_idleCapacity.front();
        if (!all && record.idleDeadline > now) return record.idleDeadline;
        auto *indexed = m_metadata.exchangeIdleCapacity(record.replica.version.key, nullptr);
        Q_ASSERT(indexed == &record); Q_UNUSED(indexed);
        m_idleCapacity.pop_front();
        record.idleDeadline = 0;
        m_retirementQueue.acceptDetachedBacking(record.replica);
    }
    return 0;
}

void KisPagePublicationCoordinator::acceptCapacityLocked(const KisReplicaHandle &replica,
                                                         KisPageRetirementRecord *record)
{
    if (!record) {
        m_retirementQueue.acceptDetachedBacking(replica);
        return;
    }
    Q_ASSERT(record->replica == replica && !record->is_linked());
    auto *previous = m_metadata.exchangeIdleCapacity(replica.version.key, record);
    if (previous) {
        m_idleCapacity.erase(m_idleCapacity.iterator_to(*previous));
        previous->idleDeadline = 0;
        m_retirementQueue.acceptDetachedBacking(previous->replica);
    }
    record->idleDeadline = QDeadlineTimer::current().deadline() + IdleCapacityAgeMs;
    m_idleCapacity.push_back(*record);
}

quint64 KisPagePublicationCoordinator::idleAllowanceLocked() const
{
    // Advisory selection; every original budget admission rechecks both the
    // idle sub-limit and the combined optional limit under its own gate.
    return m_backgroundReclamation ? m_budget.idleCapacityAllowance() : 0;
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
        for (const auto &proof : state->proofs) capture.versions.push_back(proof.second.proof.version);
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

KisPagePublicationCoordinator::KisPreparedPagePublication
KisPagePublicationCoordinator::preparePagePublicationLocked(
    const KisPageTransaction &transaction,
    const OverlayChange *changes, size_t count,
    QString *error, bool *storageRefused)
{
    return preparePagePublicationInputLocked(transaction, changes, count,
        [](const void *input, size_t i) { return static_cast<const OverlayChange *>(input)[i]; },
        error, storageRefused);
}

KisPagePublicationCoordinator::KisPreparedPagePublication
KisPagePublicationCoordinator::preparePagePublicationInputLocked(
    const KisPageTransaction &transaction, const void *input, size_t count,
    OverlayChange (*readChange)(const void *, size_t), QString *error, bool *storageRefused, bool canonical)
{
    if (storageRefused) *storageRefused = false;
    if (!hasActiveTransactionLocked(transaction) || !input || !count || !readChange) {
        KisPageStoreDetail::setError(error, QStringLiteral("overlay update input is invalid"));
        return {};
    }
    try {
        KisPagePublicationChanges changes(KisMutationStorageAllocator<KisPagePublicationChange>::retained(&m_budget));
        changes.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto value = readChange(input, i);
            if (!value.removal && !m_owner.ownsPreparedPageProof(value.proof)) {
                KisPageStoreDetail::setError(error, QStringLiteral("overlay input capability is not owned"));
                return {};
            }
            auto *native = value.removal ? nullptr : m_metadata.nativeWriteForProof(value.proof);
            auto identity = KisPagePublicationProof(value.proof);
            identity.native = bool(native);
            changes.push_back({{value.key, identity, value.removal, native}, {}, false, false, false, {}});
        }
        return preparePagePublicationLocked(transaction, std::move(changes), error, storageRefused, canonical);
    } catch (const std::bad_alloc &) {
        if (storageRefused) *storageRefused = true;
        KisPageStoreDetail::setError(error, QStringLiteral("overlay storage preparation was refused"));
        return {};
    }
}

KisPagePublicationCoordinator::KisPreparedPagePublication
KisPagePublicationCoordinator::preparePagePublicationLocked(
    const KisPageTransaction &transaction, KisPagePublicationChanges changes,
    QString *error, bool *storageRefused, bool canonical)
{
    if (storageRefused) *storageRefused = false;
    if (!hasActiveTransactionLocked(transaction) || changes.empty()) {
        KisPageStoreDetail::setError(error, QStringLiteral("overlay update input is invalid"));
        return {};
    }
    try {
        using Data = KisPreparedPagePublication::Data;
        auto storage = KisMutationStorageAllocator<Data>::retained(&m_budget);
        std::unique_ptr<Data, KisPreparedPagePublication::DataDeleter> candidate(nullptr, {storage});
        auto *raw = storage.allocate(1);
        try { std::allocator_traits<decltype(storage)>::construct(storage, raw, storage); }
        catch (...) { auto allocator = storage; allocator.deallocate(raw, 1); throw; }
        candidate.reset(raw);
        candidate->owner = this;
        candidate->transaction = transaction;
        candidate->canonical = canonical;
        candidate->changes = std::move(changes);
        std::sort(candidate->changes.begin(), candidate->changes.end(), [](const auto &a, const auto &b) {
            return overlayKeyLess(a.replacement.key, b.replacement.key);
        });
        size_t oldProofNodes = 0, oldRemovalNodes = 0;
        const auto base = m_epochs.root(transaction.baseEpoch);
        if (!canonical) candidate->state = preparedStateLocked(transaction.id);
        else if (hasPreparedDeltaLocked(transaction.id) || isPreparingCommitLocked(transaction.id)) return {};
        auto *state = candidate->state.get();
        KisPageKey previous;
        for (auto &entry : candidate->changes) {
            const auto &change = entry.replacement;
            const bool hasProof = change.proof.isValid();
            auto *native = hasProof ? change.native : nullptr;
            if (!change.key.isValid() || change.removal == hasProof
                || previous == change.key
                || (hasProof
                    && (!(change.proof.transaction == transaction.id)
                        || !(change.proof.version.key == change.key)
                        || (!native && !m_owner.ownsPreparedPageProof(projectProof(change.proof)))))) {
                KisPageStoreDetail::setError(error, QStringLiteral("overlay update change is invalid"));
                return {};
            }
            previous = change.key;
            if (hasProof && !native) native = m_metadata.nativeWrite(change.proof.version);
            // This is the metadata consumer's original result, not permission
            // to skip validation of an independently exported capability.
            entry.replacement.native = native;
            const auto superseded = [&] {
                if (!state) return KisPagePublicationProof{};
                const auto found = state->proofs.find(change.key);
                return found == state->proofs.end() ? KisPagePublicationProof{} : found->second.proof;
            }();
            if (superseded.isValid()) {
                if ((!superseded.native && !m_owner.ownsPreparedPageProof(projectProof(superseded)))
                    || (hasProof
                        && superseded.version.generation.value
                            >= change.proof.version.generation.value)) {
                    KisPageStoreDetail::setError(error, QStringLiteral("overlay update does not replace its current proof"));
                    return {};
                }
            }
            if (superseded.isValid()) {
                // The original result already retains its exact source. A
                // foreign/generic source projects at its real boundary.
                entry.supersededAuthority = native && native->source.version == superseded.version
                    && native->source.allocationIdentity() == superseded.allocation
                    ? native->source : projectProof(superseded).authority;
            }
            if (!canonical && (superseded.isValid() || native)) {
                ++candidate->mutationCount;
                entry.releasesCapacity = superseded.isValid()
                    && m_owner.backingClass(entry.supersededAuthority) == KisBackingBudgetClass::ActivePending;
            }
            const bool removalWasInBase = change.removal && base.containsPage(change.key);
            const bool wasRemoved = state && state->removals.count(change.key) != 0;
            if (!canonical && hasProof && !superseded.isValid())
                candidate->proofNodes.emplace(change.key, KisPagePublicationDelta::PageState{change.proof});
            if (!canonical && removalWasInBase && !wasRemoved)
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
        if (state) {
            reserve(state->proofs, state->proofInsertions.load(), candidate->proofNodes.size());
            reserve(state->removals, state->removalInsertions.load(), candidate->removalNodes.size());
            candidate->proofInsertions = candidate->proofNodes.size();
            candidate->removalInsertions = candidate->removalNodes.size();
            state->proofInsertions.fetch_add(candidate->proofInsertions);
            state->removalInsertions.fetch_add(candidate->removalInsertions);
        }
        KisPreparedPagePublication result;
        result.data = std::move(candidate);
        KisPageStoreDetail::setError(error, {});
        return result;
    } catch (const std::bad_alloc &) {
        if (storageRefused) *storageRefused = true;
        // The caller still owns the replacement proofs on refusal. No
        // metadata or visible overlay value has changed.
        KisPageStoreDetail::setError(error, QStringLiteral("overlay storage preparation was refused"));
        return {};
    }
}

void KisPagePublicationCoordinator::finishAbortedProofLocked(
    const KisPreparedPageProof &proof) noexcept
{
    auto transaction = m_preparedTransactions.find(proof.transaction.value);
    if (transaction == m_preparedTransactions.end())
        qFatal("Owned prepared proof has no transaction record");
    auto &state = *transaction->second;
    auto stored = state.proofs.find(proof.authority.version.key);
    // Abort verified this original capability before its accepted metadata
    // transition. Removing the version also consumes its inline CPU proof;
    // a protected Historical version or exported capability still needs
    // explicit revocation. There is no post-acceptance resource refusal.
    if (stored == state.proofs.end() || !stored->second.proof.matches(proof))
        qFatal("Owned prepared proof changed before revocation");
    if (!m_owner.revokePreparedPage(proof)) {
        KisPageMetadataCoordinator::VersionInfo remaining;
        m_metadata.versionSnapshot(proof.authority.version, &remaining);
        if (remaining.version.isValid())
            qFatal("Owned aborted proof survived without its capability");
    }
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

void KisPagePublicationCoordinator::preparePrivateLayoutLocked(KisPagePrivateWrite &result,
    const KisPageAllocationDescriptor &descriptor,
    std::shared_ptr<const KisPageAllocationDescriptor> selected)
{
    Q_ASSERT(!result.reserved && !result.descriptor);
    Q_ASSERT(!selected || selected.get() == &descriptor);
    if (!selected) selected = std::allocate_shared<const KisPageAllocationDescriptor>(
        KisMutationStorageAllocator<KisPageAllocationDescriptor>::retained(&m_budget), descriptor);
    result.descriptor = std::move(selected);
    // Conservative invalidation also covers a rejected private preparation;
    // no committed/default descriptor may rely on a stale revision.
    ++m_descriptorRevision;
}

KisPagePublicationCoordinator::PreparedDescriptors
KisPagePublicationCoordinator::prepareDescriptorLocked(const KisPageVersion &version,
                                                       const KisPageAllocationDescriptor &descriptor)
{
    return prepareDescriptorLocked(version, descriptor, {});
}

KisPagePublicationCoordinator::PreparedDescriptors
KisPagePublicationCoordinator::prepareDescriptorLocked(const KisPageVersion &version,
                                                       const KisPageAllocationDescriptor &descriptor,
                                                       const KisPageVersion &base)
{
    const PreparedDescriptorChange change{version, descriptor};
    const auto shared = base.isValid() ? m_metadata.versionDescriptor(base) : nullptr;
    return prepareDescriptorAdditionsLocked(&change, 1, shared ? &shared : nullptr);
}

void KisPagePublicationCoordinator::noteVersionExitLocked(const KisPageVersion &version)
{
    Q_UNUSED(version);
    // Metadata already removed the original version and its layout. Retain
    // this invalidation until outstanding default/restore preparations finish.
    ++m_descriptorRevision;
}

const KisPageAllocationDescriptor *KisPagePublicationCoordinator::descriptorLocked(
    const KisPageVersion &version, KisPageAllocationDescriptor *descriptor) const
{
    const auto *found = m_metadata.borrowVersionDescriptor(version);
    if (!found)
        return nullptr;
    if (descriptor)
        *descriptor = *found;
    return found;
}

KisPagePublicationCoordinator::PreparedDescriptors
KisPagePublicationCoordinator::prepareDescriptorAdditionsLocked(const PreparedDescriptorChange *changes, size_t count,
    const std::shared_ptr<const KisPageAllocationDescriptor> *sharedLayout)
{
    const auto storage = KisMutationStorageAllocator<DescriptorEntry>::retained(&m_budget);
    PreparedDescriptors nodes{PreparedDescriptors::allocator_type(storage)};
    if (!count) return nodes;
    nodes.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto found = m_metadata.versionDescriptor(changes[i].version);
        if (found && *found == changes[i].descriptor) continue;
        std::shared_ptr<const KisPageAllocationDescriptor> value;
        if (sharedLayout && **sharedLayout == changes[i].descriptor) value = *sharedLayout;
        if (!value) {
            const auto same = std::find_if(nodes.cbegin(), nodes.cend(), [&](const auto &entry) {
                return entry.second && *entry.second == changes[i].descriptor;
            });
            if (same != nodes.cend()) value = same->second;
        }
        if (!value) value = std::allocate_shared<const KisPageAllocationDescriptor>(
            KisMutationStorageAllocator<KisPageAllocationDescriptor>(storage), changes[i].descriptor);
        nodes.emplace_back(changes[i].version, std::move(value));
    }
    if (!nodes.empty()) ++m_statistics.descriptorCapacityPreparations;
    return nodes;
}

void KisPagePublicationCoordinator::discardDescriptorAdditionsLocked(
    PreparedDescriptors *prepared, QMutexLocker<QMutex> &ownerLock)
{
    if (prepared->empty()) return;
    if (ownerLock.isLocked()) ownerLock.unlock();
    *prepared = {};
    ownerLock.relock();
}

void KisPagePublicationCoordinator::installDescriptorAdditionsLocked(PreparedDescriptors *prepared)
{
    for (auto &entry : *prepared) {
        // The version and its cold slot were prepaid by the metadata
        // transition. Exchange keeps the last old descriptor in this original
        // candidate until its existing lock-external cleanup.
        const bool installed = m_metadata.exchangeVersionDescriptor(entry.first, &entry.second);
        Q_ASSERT(installed);
        if (!installed) qFatal("descriptor installation lost its metadata version");
        ++m_descriptorRevision;
    }
    // Empty inline preparation has no allocation or last descriptor to defer.
    // Retain spilled storage and replacements for the original unlocked tail.
    if (prepared->capacity() <= PreparedDescriptors::internal_capacity()
        && std::all_of(prepared->cbegin(), prepared->cend(), [](const auto &entry) { return !entry.second; }))
        prepared->clear();
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

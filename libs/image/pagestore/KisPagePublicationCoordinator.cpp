/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPagePublicationCoordinator_p.h"
#include "KisPageRestoration_p.h"
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
    return KisPagePublicationDelta::PageLess{}(a, b);
}

// The transaction owns the only mutable selection. Capacity was admitted for
// all sibling candidates before metadata acceptance. Structural changes merge
// once in that storage; value-only updates touch only D validated positions.
// Memory holds transfer from the original preparation. The old Value remains
// alive through this removal even if metadata already revoked its content.
template<bool Proof, typename Table>
void acceptSelection(Table &table, KisPagePublicationRows changes)
{
    const auto selected = [](const auto &row) {
        if constexpr (Proof) return !row.replacement.removal;
        else return row.replacement.removal && row.removalWasInBase;
    };
    size_t additions = 0, removals = 0;
    for (auto &row : changes) {
        const bool before = Proof ? row.superseded.isValid() : row.wasRemoved;
        const bool after = selected(row);
        additions += after && !before;
        removals += before && !after;
        if constexpr (Proof) if (before) {
            auto &entry = (table.begin() + row.acceptedOverlayIndex)->second;
            std::swap(entry.value, row.acceptedValue);
            if (after) entry.stamp = row.replacement.proofValue().stamp;
        }
    }
    const auto take = [](auto &row) {
        const auto proof = row.replacement.proofValue();
        Q_ASSERT(row.acceptedValue);
        return KisPagePublicationSelection{std::exchange(row.acceptedValue, nullptr), proof.stamp};
    };
    if (!additions && !removals) return;
    auto values = table.extract_sequence();
    using Value = typename decltype(values)::value_type;
    static_assert(std::is_nothrow_move_assignable_v<Value>);
    static_assert(std::is_nothrow_default_constructible_v<Value>);
    const auto key = [](const Value &value) -> const KisPageKey & {
        if constexpr (Proof) return value.first;
        else return value;
    };
    size_t old = values.size();
    size_t output = old + additions;
    Q_ASSERT(output <= values.capacity());
    values.resize(output);
    for (auto row = changes.rbegin(); row != changes.rend(); ++row) {
        const auto &changedKey = row->replacement.key();
        while (old && overlayKeyLess(changedKey, key(values[old - 1]))) {
            --old; --output;
            if (old != output) values[output] = std::move(values[old]);
        }
        if (old && key(values[old - 1]) == changedKey) --old;
        if (selected(*row)) {
            --output;
            if constexpr (Proof) {
                if (row->superseded.isValid()) {
                    if (old != output) values[output] = std::move(values[old]);
                } else values[output] = Value{changedKey, take(*row)};
            } else values[output] = changedKey;
        }
    }
    // The prefix before the first changed key is already final. Do not scan
    // or copy it (in particular, appending sparse keys must not visit N rows).
    Q_ASSERT(output == old + removals);
    if (removals) {
        std::move(values.begin() + output, values.end(), values.begin() + old);
        values.resize(values.size() - removals);
    }
    table.adopt_sequence(boost::container::ordered_unique_range, std::move(values));
}

// The immutable prepared publication/root need no owner/metadata/provider
// locks. Feed at most one old/new pair at a time to the ledger, without a
// document-sized input carrier or a second lifetime for its prepared rows.
template<typename PreparedPublication, typename NextTarget>
auto publicationBackingReader(const PreparedPublication &publication,
                              const KisImageEpochRootSnapshot &currentRoot,
                              NextTarget nextTarget, quint64 idleAllowance = 0)
{
    return [&publication, &currentRoot, nextTarget = std::move(nextTarget), idleAllowance,
            pending = boost::container::static_vector<KisBackingClassChange, 2>{}, index = size_t(0)]
           (KisBackingClassChange *change) mutable -> int {
        while (index == pending.size()) {
            pending.clear();
            index = 0;
            KisPageVersion target;
            KisBackingBudgetClass targetBefore;
            if (!nextTarget(&target, &targetBefore)) return 0;
            KisPageVersion current;
            if (!currentRoot.resolve(target.key, &current)) return -1;
            if (current == target) continue;
            bool detached = false;
            KisPageRetirementRecord **idleResult = nullptr;
            const auto oldAuthority = publication.backingAuthority(current, &detached, &idleResult);
            const auto targetAuthority = publication.backingAuthority(target);
            if ((!oldAuthority.isValid() && !current.isDefaultPixel())
                || (!targetAuthority.isValid() && !target.isDefaultPixel())) return -1;
            // Refused optional idle admission is retried without caching. The
            // old Current charge then survives until its original retirement.
            const bool cacheDetached = detached && oldAuthority.isValid()
                && oldAuthority.layout.byteSize <= idleAllowance;
            if (cacheDetached) idleAllowance -= oldAuthority.layout.byteSize;
            if (oldAuthority.isValid() && (!detached || cacheDetached))
                pending.push_back({oldAuthority, KisBackingBudgetClass::Current,
                    detached ? KisBackingBudgetClass::IdleCapacity : KisBackingBudgetClass::RetainedHistory,
                    cacheDetached ? idleResult : nullptr});
            if (targetAuthority.isValid())
                pending.push_back({targetAuthority, targetBefore, KisBackingBudgetClass::Current, nullptr,
                    publication.originalBacking(target)});
        }
        *change = pending[index++];
        return 1;
    };
}

} // namespace

// Synchronous commit state never escapes this call. Independent metadata,
// root outputs retain their own original lifetime owners.
class KisPagePublicationCoordinator::KisPreparedMutationCommit final
{
public:
    using PublicationChange = KisPageMetadataCoordinator::PublicationChange;
    explicit KisPreparedMutationCommit(KisBackingBudgetController &budget)
        : workspace(&budget), directoryKeys(KisMutationStorageAllocator<KisPageKey>(&budget))
        , restoreTargets(KisMutationStorageAllocator<KisPageVersion>(&budget)) {}
    ~KisPreparedMutationCommit() = default;
    bool isValid() const;
    bool installRoot(KisRetainedImageEpochSnapshot *retainedAfter);
    void finishInstallation();
    void clear() noexcept;
    void release() noexcept
    {
        // Attempts have one semantic cleanup guard. Before the first attempt,
        // proof validation can fail; only its original proof view remains.
        Q_ASSERT(!owner && !metadata.isValid());
        directoryKeys = decltype(directoryKeys){directoryKeys.get_allocator()};
        restoreTargets = decltype(restoreTargets){restoreTargets.get_allocator()};
        workspace.reset();
    }
    void prepareChanges(size_t semanticCount, size_t changes, size_t feeChanges = 0,
                        size_t adapterCount = 0, bool preserveAdapter = false)
    {
        Q_ASSERT(semanticTargets.empty() && publicationChanges.empty());
        size_t semanticOffset = 0;
        const auto bytes = layout(semanticCount, changes, feeChanges ? feeChanges : changes, adapterCount, semanticOffset);
        if (preserveAdapter) workspace.reserveWithPrefix<void *>(bytes, adapterCount);
        else {
            workspace.reserve(bytes);
            if (adapterCount) std::uninitialized_value_construct_n(workspace.at<void *>(0), adapterCount);
        }
        semanticTargets.bind(workspace.at<KisPageVersion>(semanticOffset), semanticCount);
        publicationChanges.bind(workspace.at<PublicationChange>(phaseOffset), changes);
    }
    void beginBackingChanges()
    {
        publicationChanges.clear();
    }
    template<class Reader, class Install>
    KisPageOwnerLedger::BackingAcceptance acceptFees(KisPageOwnerLedger &ledger,
        const KisPageMetadataCoordinator::PreparedPublication::CpuResources &resources,
        Reader &reader, Install &install)
    {
        // Reserve fees before taking any Resource gate: admission may grow
        // budget slots or reclaim terminal management capacity. The original
        // ledger window then protects the actual class changes through the
        // Resource check and metadata/root cut. Installation cannot reenter it.
        auto acceptResources = [&] {
            return resources.accept(&install,
                [](void *context) { return (*static_cast<Install *>(context))(); });
        };
        const auto accepted = ledger.acceptPreparedBackingChanges(workspace.at<KisPagePreparedBackingChange>(phaseOffset),
            backingCapacity, &reader, [](void *context, KisBackingClassChange *change) {
                return (*static_cast<Reader *>(context))(change);
            }, &acceptResources, [](void *context) { return (*static_cast<decltype(acceptResources) *>(context))(); });
        return accepted;
    }
    KisPagePublicationCoordinator *owner = nullptr;
    KisPageTransaction transaction;
    KisPageMetadataCoordinator::PreparedPublication metadata;
    KisImageEpochReferenceModel::PreparedCommit epoch;
    KisImageEpochReferenceModel::PreparedRootReservation restoreEpoch;
    // Views are destroyed before their one physical allocation.
    KisMutationWorkspace workspace;
    KisMutationWorkspace::Array<KisPageVersion> semanticTargets;
    KisMutationWorkspace::Array<PublicationChange> publicationChanges;
    KisPageMetadataCoordinator::RegisteredKeys directoryKeys;
    std::vector<KisPageVersion, KisMutationStorageAllocator<KisPageVersion>> restoreTargets;
    KisCompletionTicket completion;
    KisPageMetadataCoordinator::DeferredPublicationCleanup metadataCleanup;
    KisImageEpochCommitResult result;
private:
    size_t layout(size_t semanticCount, size_t changes, size_t feeChanges,
                  size_t adapterCount, size_t &semanticOffset)
    {
        if (feeChanges > std::numeric_limits<size_t>::max() / 2) throw std::bad_alloc();
        size_t bytes = 0;
        KisMutationWorkspace::append<void *>(bytes, adapterCount);
        semanticOffset = KisMutationWorkspace::append<KisPageVersion>(bytes, semanticCount);
        phaseOffset = KisMutationWorkspace::append<std::max_align_t>(bytes, 0);
        size_t publicationBytes = bytes, backingBytes = bytes;
        KisMutationWorkspace::append<PublicationChange>(publicationBytes, changes);
        backingCapacity = feeChanges * 2;
        KisMutationWorkspace::append<KisPagePreparedBackingChange>(backingBytes, backingCapacity);
        return std::max(publicationBytes, backingBytes);
    }
    size_t phaseOffset = 0;
    size_t backingCapacity = 0;
};

const KisPagePublicationChange *
KisPagePublicationCoordinator::KisPreparedPagePublication::Data::findChange(const KisPageKey &key) const
{
    const auto found = std::lower_bound(changes.cbegin(), changes.cend(), key,
        [](const Change &change, const KisPageKey &value) { return overlayKeyLess(change.replacement.key(), value); });
    return found != changes.cend() && found->replacement.key() == key ? &*found : nullptr;
}

KisPagePublicationCoordinator::KisPreparedPagePublication::
    KisPreparedPagePublication() = default;

KisPagePublicationCoordinator::KisPreparedPagePublication::
    ~KisPreparedPagePublication()
{
    cancel();
}

KisPagePublicationCoordinator::KisPreparedPagePublication::
    KisPreparedPagePublication(KisPreparedPagePublication &&other) noexcept : data(std::move(other.data))
{
    other.data.reset();
}

KisPagePublicationCoordinator::KisPreparedPagePublication &
KisPagePublicationCoordinator::KisPreparedPagePublication::operator=(
    KisPreparedPagePublication &&other) noexcept
{
    if (this != &other) {
        cancel();
        data = std::move(other.data);
        other.data.reset();
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
    if (data->mutationCount) {
        data->metadata = data->owner->m_metadata.prepareAcceptance(
            data->transaction, data->changes, error);
        if (!data->metadata.isValid()) {
            if (storageRefused) *storageRefused = data->metadata.wasStorageRefused();
            return false;
        }
    }
    data->prepared = true;
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPagePublicationCoordinator::KisPreparedPagePublication::tryAcceptLocked(
    QMutexLocker<QMutex> &ownerLock, QString *error, bool *storageRefused)
{
    if (installOverlayLocked(error)) return true;
    if (!data->metadata.needsAcceptanceRevalidation()) return false;
    // One common retry for native seal, generic publication and removal.
    // Keep the already constructed values and pixels. A late reader changes
    // the original source disposition/fees; it creates no new value storage.
    bool prepared = false;
    ownerLock.unlock();
    {
        const auto relock = qScopeGuard([&] { ownerLock.relock(); });
        // Preserve original ordering and positions while surface and source
        // retention are revalidated against the same result.
        data->surfacePrepared = false;
        prepared = data->owner->m_metadata.revalidateAcceptance(data->metadata, error);
        if (storageRefused) *storageRefused = data->metadata.wasStorageRefused();
    }
    return prepared && prepareSurfaceLocked(error, storageRefused) && installOverlayLocked(error);
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
            return change.replacement.key().surface == surface && change.replacement.removal;
        })) {
            // Derive from the same base + overlay used by captured readers.
            // Persistent subtree bounds avoid exporting the document manifest.
            const auto storage = data->ownedChanges.get_allocator();
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
                if (change.replacement.removal) removals.push_back(change.replacement.key());
                else additions.push_back(change.replacement.key());
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
                const auto &key = change.replacement.key();
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
    installOverlayLocked(QString *error)
{
    if (!isValid() || data->canonical || !data->prepared || !data->surfacePrepared) {
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
    for (Data::Change &change : data->changes) {
        // Preparation already located this original selection. A sibling may
        // have moved flat-map entries while owner was unlocked, so verify the
        // key at the saved ordinal before using it. Identity and removal
        // checks below still consume the current locked cut.
        const auto &key = change.replacement.key();
        auto found = state.proofs.end();
        if (change.superseded.isValid() && change.acceptedOverlayIndex < state.proofs.size()
            && (state.proofs.begin() + change.acceptedOverlayIndex)->first == key)
            found = state.proofs.begin() + change.acceptedOverlayIndex;
        else found = state.proofs.find(key);
        const KisPagePublicationIdentity current = found == state.proofs.end() ? KisPagePublicationIdentity{} : found->second.identity(data->transaction.id);
        if (!(current == change.superseded)
            || (state.removals.count(change.replacement.key()) != 0) != change.wasRemoved
            || (current.isValid() && !current.native
                && !owner.m_owner.ownsPreparedPageProof(owner.projectProof(current)))
            || (change.replacement.hasProof() && !change.replacement.isNativeValue()
                && !owner.m_owner.ownsPreparedPageProof(owner.projectProof(change.replacement.proofValue())))) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("overlay update source changed"));
            return false;
        }
        change.acceptedOverlayIndex = size_t(found - state.proofs.begin());
    }

    const auto allowance = owner.idleAllowanceLocked();
    const bool hasIdleChange = allowance && std::any_of(data->changes.begin(), data->changes.end(),
        [](const auto &row) { return row.detachesSuperseded; });
    const bool idleFeesReady = hasIdleChange && owner.m_owner.synchronizeBackingDomains(error);
    const auto resources = data->metadata.prepareCpuResources(error);
    if (!resources.isValid()) return false;
    auto accept = [&] {
        if (data->mutationCount && !owner.m_metadata.installAcceptance(
                std::move(data->metadata), data->transaction, error)) return false;
        acceptSelection<true>(state.proofs, data->changes);
        acceptSelection<false>(state.removals, data->changes);
        if (data->surfaceChange.after.surface.isValid()) {
            if (data->surfaceIndex >= 0) state.surfaceChanges[data->surfaceIndex] = data->surfaceChange;
            else state.surfaceChanges.push_back(data->surfaceChange);
        }
        return true;
    };
    auto install = [&] {
        return resources.accept(&accept,
            [](void *p) { return (*static_cast<decltype(accept) *>(p))(); }, error);
    };
    // Fee refusal is optional; a rejected Resource/metadata cut is terminal
    // for this attempt. All actual selection changes stay inside that cut.
    using Acceptance = KisPageOwnerLedger::BackingAcceptance;
    auto accepted = Acceptance::FeesRefused;
    for (size_t i = 0; i < data->changes.size(); ++i) {
        if (!data->changes[i].detachesSuperseded) continue;
        if (idleFeesReady)
            accepted = owner.m_owner.acceptPreparedIdleCapacity(data->changes.subspan(i), allowance,
                &install, [](void *context) { return (*static_cast<decltype(install) *>(context))(); }, error);
        break;
    }
    if (accepted == Acceptance::InstallRejected
        || (accepted == Acceptance::FeesRefused && !install())) return false;
    for (const auto &change : std::as_const(data->changes)) {
        if (change.detachesSuperseded)
            owner.acceptCapacityLocked(change.supersededReplica(), change.acceptedIdle);
    }
    data->releaseCapacity();
    data->installed = true;
    if (!owner.m_idleCapacity.empty()) owner.m_history.scheduleLocked();
    ++owner.m_statistics.overlayAcceptances;
    KisPageStoreDetail::setError(error, {});
    return true;
}

void KisPagePublicationCoordinator::KisPreparedPagePublication::finishUnlocked(
    QMutexLocker<QMutex> &ownerLock)
{
    Q_ASSERT(!ownerLock.isLocked());
    if (!data) return;
    auto &owner = *data->owner;
    if (data->installed) {
        owner.m_metadata.finishAcceptance(data->changes);
        ownerLock.relock();
        // Only accepted replacements transfer the superseded proofs. Rejected
        // preparation still borrows its producer's proofs and must not revoke.
        bool historyRequested = false;
        for (const auto &change : std::as_const(data->changes)) {
            if (!change.superseded.isValid() || change.detachesSuperseded) continue;
            const bool revoked = owner.m_owner.releasePublicationIdentity(change.superseded);
            Q_ASSERT(revoked);
            Q_UNUSED(revoked);
            historyRequested = true;
        }
        if (historyRequested)
            owner.m_history.collectUnreachableLocked();
        ownerLock.unlock();
    }
    data.reset();
}

void KisPagePublicationCoordinator::KisPreparedPagePublication::cancel() noexcept
{
    Q_ASSERT(!data || !data->installed);
    // Replacements belong to their original page/lease until install accepts
    // them. Discarding preparation releases capacity, never those proofs.
    data.reset();
}

bool KisPagePublicationCoordinator::KisPreparedMutationCommit::isValid() const
{
    return owner && metadata.isValid()
        && (epoch.isValid() != restoreEpoch.isValid()) && completion.isValid();
}

bool KisPagePublicationCoordinator::KisPreparedMutationCommit::installRoot(KisRetainedImageEpochSnapshot *retainedAfter)
{
    if (!isValid()) return false; // The original attempt guard owns rejection cleanup.

    auto &candidate = *this;
    KisPagePublicationCoordinator &owner = *candidate.owner;
    ++owner.m_statistics.preparedMutationCommits;
    const KisImageEpochReferenceModel::InstallMetadataFunction installMetadata = [](void *context,
                                                                                    KisImageEpochId epoch) {
        auto &prepared = *static_cast<KisPreparedMutationCommit *>(context);
        KisPagePublicationCoordinator &coordinator = *prepared.owner;
        if (!coordinator.m_metadata.installPublication(std::move(prepared.metadata),
                                                       prepared.transaction,
                                                       epoch,
                                                       nullptr,
                                                       &prepared.metadataCleanup)) {
            return false;
        }
        return true;
    };
    candidate.result = candidate.restoreEpoch.isValid()
        ? owner.m_epochs.installRestore(std::move(candidate.restoreEpoch), &candidate, installMetadata)
        : owner.m_epochs.installCommit(std::move(candidate.epoch), &candidate, installMetadata, retainedAfter);

    return candidate.result.isCommitted();
}

void KisPagePublicationCoordinator::KisPreparedMutationCommit::finishInstallation()
{
    auto &coordinator = *owner;
    // The synchronous fee acceptance has already ended. Actual capacity
    // transfer may reenter the ledger/queue and must stay outside its gate.
    if (result.isCommitted()) {
        metadata.forEachDetachedBacking(owner, [](void *context, const KisReplicaHandle &replica,
                                                   KisPageRetirementRecord *record) {
            auto &coordinator = *static_cast<KisPagePublicationCoordinator *>(context);
            coordinator.acceptCapacityLocked(replica, record);
            ++coordinator.m_statistics.detachedCurrentBackings;
        });
        if (!coordinator.m_idleCapacity.empty()) coordinator.m_history.scheduleLocked();
        ++coordinator.m_statistics.installedMutationCommits;
    } else ++coordinator.m_statistics.cancelledMutationCommits;
    owner = nullptr;
}

void KisPagePublicationCoordinator::KisPreparedMutationCommit::clear() noexcept
{
    metadata = {};
    epoch = {};
    restoreEpoch = {};
    metadataCleanup = {};
    semanticTargets.clear();
    publicationChanges.clear();
    directoryKeys.clear();
    // Root restore inputs and their workspace prefix span retries. Ordinary
    // commits never populate restoreTargets; release() ends this input lifetime.
    transaction = {};
    completion = {};
    result = {};
    owner = nullptr;
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
            *version = proof->second.version();
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

bool KisPagePublicationCoordinator::prepareNativeReadSelectionLocked(KisSurfaceId surface,
    const KisPageReadView &view, NativeReadSelection *selection) const
{
    if (!m_operational || !selection || !surface.isValid()) return false;
    *selection = {};
    if (view.kind == KisPageReadViewKind::CurrentCommittedEpoch) {
        selection->root = m_epochs.captureCommittedRoot();
    } else if (view.kind == KisPageReadViewKind::TransactionOverlay) {
        const auto transaction = m_epochs.activeTransaction(view.transaction);
        if (!transaction.isValid()) return false;
        selection->root = m_epochs.root(transaction.baseEpoch);
        const auto found = m_preparedTransactions.find(transaction.id.value);
        if (found != m_preparedTransactions.end()) selection->delta = found->second.get();
    } else return false;
    if (!selection->root.surfaceState(surface, &selection->surface)) return false;
    if (selection->delta)
        for (const auto &change : selection->delta->surfaceChanges)
            if (change.after.surface == surface) { selection->surface = change.after; break; }
    return true;
}

KisPageViewValue *KisPagePublicationCoordinator::NativeReadSelection::selectedValue(const KisPageKey &key) const
{
    Q_ASSERT(key.isValid() && root.isValid() && key.surface == surface.surface);
    KisPageViewValue *value = nullptr;
    if (delta) {
        const auto found = delta->proofs.find(key);
        if (found != delta->proofs.end()) value = found->second.value;
        else if (delta->removals.count(key)) return nullptr;
        else value = root.selectedValue(key);
    } else value = root.selectedValue(key);
    // Range defaults resolve from the selected surface revision, including
    // an explicit old default under a staged surface change.
    return value && !value->version.isDefaultPixel() ? value : nullptr;
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
    auto dispose = qScopeGuard([&] {
        ownerLock.unlock();
        update.finishUnlocked(ownerLock);
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
    if (!update.prepareSurfaceLocked(error) || !update.tryAcceptLocked(ownerLock, error)) return false;
    ownerLock.unlock();
    update.finishUnlocked(ownerLock);
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
            if (original == owned.proofs.cend()
                || !m_owner.matchesPublicationIdentity(original->second.identity(transaction.id), proof)) return {};
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

static const KisPageVersion &publicationVersion(const KisPagePublicationChange::Replacement &entry) { return entry.version(); }
static const KisPageVersion &publicationVersion(const KisPagePublicationDelta::SelectionMap::value_type &entry)
{ return entry.second.version(); }
static KisPagePublicationIdentity publicationIdentity(const KisPagePublicationDelta::SelectionMap::value_type &entry, KisPageTransactionId transaction)
{
    return entry.second.identity(transaction);
}

template<class Input>
KisImageEpochCommitTicket KisPagePublicationCoordinator::commitInputLocked(
    const KisPageTransaction &transaction, const Input &owned, KisRetainedImageEpochSnapshot *retainedAfter,
    const KisPageStore *diagnosticOwner, QMutexLocker<QMutex> &ownerLock)
{
    using Phase = KisPageStoreDiagnosticPhase;
    const auto publicationProof = [&](const auto &entry) {
        if constexpr (std::is_same_v<Input, KisPageOperationPublication>) return entry.proofValue();
        else return publicationIdentity(entry, transaction.id);
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
    KisPreparedMutationCommit preparedCommit(m_budget);
    const auto releaseCommit = qScopeGuard([&] {
        if (ownerLock.isLocked()) ownerLock.unlock();
        preparedCommit.release();
        ownerLock.relock();
    });
    const bool changesSurfaceDefault = std::any_of(owned.surfaceChanges.cbegin(), owned.surfaceChanges.cend(),
        [](const auto &change) { return change.before.defaultPixelRevision != change.after.defaultPixelRevision; });

    ownerLock.unlock();
    diagnostic.next(Phase::CommitProviderValidation, quint64(owned.proofs.size()));
    bool proofsValid = true;
    for (const auto &entry : owned.proofs) {
        if (!m_owner.preparePublicationIdentity(m_metadata, publicationProof(entry))) {
            proofsValid = false;
            break;
        }
    }
    diagnostic.next(Phase::CommitEpochDeltaPrepare, pageWork);
    const bool deltaPrepared = proofsValid;
    diagnostic.next(Phase::CommitInputOwnerWait, pageWork);
    ownerLock.relock();
    if (!deltaPrepared)
        return {};

    std::optional<KisPageMetadataCoordinator::PreparedPublication> repair;
    KisImageEpochId repairBase;
    bool disableDirectDetachment = false;
    for (;;) {
        diagnostic.next(Phase::CommitDefaultRemovalPreparation, quint64(owned.removals.size()));
        const auto discardCommit = qScopeGuard([&] {
            if (ownerLock.isLocked()) ownerLock.unlock();
            preparedCommit.clear();
            ownerLock.relock();
        });
        auto &semanticTargets = preparedCommit.semanticTargets;
        const KisImageEpochRootSnapshot currentRoot = m_epochs.captureCommittedRoot();
        if (!currentRoot.isValid() || currentRoot.epoch().value == std::numeric_limits<quint64>::max()) {
            return {};
        }
        const KisImageEpochId prospectiveEpoch{currentRoot.epoch().value + 1};
        auto &publicationChanges = preparedCommit.publicationChanges;
        auto &registeredPages = preparedCommit.directoryKeys;
        const quint64 directoryRevision = m_metadata.pageRegistrationCount();
        try { if (changesSurfaceDefault) registeredPages = m_metadata.registeredPageKeys(); }
        catch (const std::bad_alloc &) { return {}; }
        const auto replacesDefault = [&](const KisPageVersion &page) {
            const auto surfaceChange = surfaceChangeFor(page.key.surface);
            return surfaceChange
                && surfaceChange->before.defaultPixelRevision != surfaceChange->after.defaultPixelRevision
                && !owned.proofs.count(page.key)
                && page.isDefaultPixel();
        };
        const auto replacements = std::count_if(registeredPages.cbegin(), registeredPages.cend(), [&](const auto &key) {
            KisPageVersion page;
            return currentRoot.resolve(key, &page) && replacesDefault(page);
        });
        try {
            const size_t semanticCount = size_t(replacements) + owned.removals.size();
            preparedCommit.prepareChanges(semanticCount, semanticCount, owned.proofs.size() + semanticCount);
        }
        catch (const std::bad_alloc &) { return {}; }
        for (const KisPageKey &key : registeredPages) {
            KisPageVersion page;
            if (!currentRoot.resolve(key, &page) || !replacesDefault(page)) continue;
            const KisSurfaceEpochChange &change = *surfaceChangeFor(page.key.surface);
            const KisPageVersion currentVersion = page;

            const KisPageVersion replacement{page.key, page.generation, change.after.defaultPixelRevision};
            const auto descriptor = change.after.allocationDescriptor();
            KisPageMetadataCoordinator::PublicationInfo inputs;
            if (!m_metadata.queryPublication(currentVersion, replacement, &inputs)) {
                return {};
            }
            const auto &published = inputs.current;
            if (!(published.version == currentVersion) || (!published.authority.isValid() && !published.isVirtualDefault())
                || !descriptor.isValid()) {
                return {};
            }
            const auto &existingReplacement = inputs.target;
            KisReplicaHandle replacementReplica;
            if (existingReplacement.version.isValid()) {
                KisPageAllocationDescriptor existingDescriptor;
                if (existingReplacement.publication != KisPagePublicationState::Historical
                    || (!existingReplacement.authority.isValid() && !existingReplacement.isVirtualDefault())
                    || (existingReplacement.authority.isValid()
                        && (!descriptorLocked(replacement, &existingDescriptor) || !(existingDescriptor == descriptor)))) {
                    return {};
                }
                replacementReplica = existingReplacement.authority;
            }

            publicationChanges.push_back({KisPageTransitionKind::ReplaceDefaultPixel, replacement, replacementReplica});
            semanticTargets.push_back(replacement);
        }

        const auto transactionView = KisPageReadView::transactionOverlay(transaction.id);
        for (const KisPageKey &key : owned.removals) {
            KisPageMetadataCoordinator::PublicationInfo page;
            KisSurfaceEpochState surface;
            KisPageVersion current;
            if (!currentRoot.resolve(key, &current) || !m_metadata.queryPublication(current, {}, &page)
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
            semanticTargets.push_back(target);
        }

        diagnostic.next(Phase::CommitTransitionInputs,
                        quint64(owned.proofs.size() + publicationChanges.size()));
        ownerLock.unlock();
        registeredPages = decltype(preparedCommit.directoryKeys){registeredPages.get_allocator()};
        const quint64 publicationTransitionCount = quint64(owned.proofs.size() + publicationChanges.size());
        diagnostic.next(Phase::CommitEpochPrepare, pageWork);
        KisImageEpochCommitResult candidateFailure;
        auto &rootCandidate = preparedCommit.epoch;
        rootCandidate = m_epochs.prepareCommit(transaction, owned, &candidateFailure, bool(retainedAfter), m_derivedExtentSurface);
        diagnostic.next(Phase::CommitTransitionPreflight, publicationTransitionCount);
        auto &publication = preparedCommit.metadata;
        if (repair && repairBase == currentRoot.epoch() && !changesSurfaceDefault) {
            publication = std::move(*repair); repair.reset();
            m_metadata.revalidateAcceptance(publication);
        } else {
            repair.reset();
            const bool detach = !disableDirectDetachment && rootCandidate.mayDetachPreviousRoot();
            if constexpr (std::is_same_v<Input, KisPageOperationPublication>) {
                publication = m_metadata.prepareAcceptance(transaction, owned.proofs.rows, nullptr,
                    prospectiveEpoch, detach, publicationChanges.data(), qsizetype(publicationChanges.size()), &currentRoot);
            } else try {
                KisPagePublicationChanges inputs{KisMutationStorageAllocator<KisPagePublicationChange>(&m_budget)};
                inputs.reserve(owned.proofs.size());
                for (const auto &entry : owned.proofs)
                    inputs.emplace_back(KisPagePublicationChange::Replacement{&entry.second, transaction.id});
                publication = m_metadata.prepareAcceptance(transaction, std::move(inputs), nullptr,
                    prospectiveEpoch, detach, publicationChanges.data(), qsizetype(publicationChanges.size()), &currentRoot);
            } catch (const std::bad_alloc &) {
                // This independent input needs real acceptance storage. A
                // refused preparation has not installed pixels, fees or root;
                // the original transaction remains available to cancel/retry.
                return {};
            }
        }
        preparedCommit.beginBackingChanges();
        const bool detachesBacking = publication.hasDetachedBacking();
        if (detachesBacking) rootCandidate.requireDetachedPreviousRoot();
        diagnostic.next(Phase::CommitPublishOwnerWait, pageWork);
        ownerLock.relock();
        diagnostic.next(Phase::CommitPublicationRevalidate, pageWork);
        const bool rootChanged = !(m_epochs.captureCommittedRoot().epoch() == currentRoot.epoch());
        const bool directoryChanged = changesSurfaceDefault && m_metadata.pageRegistrationCount() != directoryRevision;
        if (rootChanged || directoryChanged || publication.needsReprepare()) {
            m_statistics.rootRebases += rootChanged;
            m_statistics.directoryRepreparations += directoryChanged;
            m_statistics.metadataRepreparations += publication.needsReprepare();
            continue;
        }
        if (!publication.isValid() || !rootCandidate.isValid()) {
            return {};
        }
        diagnostic.next(Phase::CommitCompletion);
        const KisCompletionTicket commitCompletion = m_readyHostCompletion;
        Q_ASSERT(commitCompletion.isValid()); // Prepared once by Store configuration.

        preparedCommit.owner = this;
        preparedCommit.transaction = transaction;
        preparedCommit.completion = commitCompletion;
        Q_ASSERT(preparedCommit.isValid());
        auto install = [&] {
            diagnostic.next(Phase::CommitRoot, pageWork);
            diagnostic.next(Phase::CommitMetadataApply, publicationTransitionCount);
            return preparedCommit.installRoot(retainedAfter);
        };
        using Acceptance = KisPageOwnerLedger::BackingAcceptance;
        const bool domainsReady = m_owner.synchronizeBackingDomains();
        const auto resources = domainsReady ? publication.prepareCpuResources()
            : KisPageMetadataCoordinator::PreparedPublication::CpuResources{};
        if (domainsReady && !resources.isValid()) return {};
        const auto acceptFees = [&](bool cacheDetached) {
            auto nextTarget = [proof = owned.proofs.begin(), proofEnd = owned.proofs.end(),
                               semantic = semanticTargets.begin(), semanticEnd = semanticTargets.end()]
                              (KisPageVersion *target, KisBackingBudgetClass *before) mutable {
                if (proof != proofEnd) {
                    *target = publicationVersion(*proof);
                    ++proof;
                    *before = KisBackingBudgetClass::ActivePending;
                    return true;
                }
                if (semantic == semanticEnd) return false;
                *target = *semantic++;
                *before = KisBackingBudgetClass::RetainedHistory;
                return true;
            };
            auto reader = publicationBackingReader(publication, currentRoot, std::move(nextTarget),
                                                   cacheDetached ? idleAllowanceLocked() : 0);
            return preparedCommit.acceptFees(m_owner, resources, reader, install);
        };
        auto accepted = domainsReady ? acceptFees(detachesBacking) : Acceptance::FeesRefused;
        if (accepted == Acceptance::FeesRefused && detachesBacking && domainsReady)
            accepted = acceptFees(false);
        if (accepted != Acceptance::FeesRefused) preparedCommit.finishInstallation();
        // Only an admission refusal may downgrade the detachment plan. Once
        // installation was entered, its original metadata/root result owns
        // repair or rejection; never replay it as an optional fee fallback.
        if (accepted == Acceptance::FeesRefused && publication.genericInputsChanged()) {
            ++m_statistics.metadataRepreparations;
            continue;
        }
        if (accepted == Acceptance::FeesRefused && detachesBacking) {
            disableDirectDetachment = true;
            ++m_statistics.detachmentBudgetFallbacks;
            continue;
        }
        if (accepted == Acceptance::FeesRefused) return {};
        const bool installed = accepted == Acceptance::Accepted;
        const bool metadataRejected =
            preparedCommit.result.status == KisImageEpochCommitStatus::Rejected;
        const KisImageEpochCommitResult committed = preparedCommit.result;
        auto metadataCleanup = std::move(preparedCommit.metadataCleanup);
        const auto acceptedRows = preparedCommit.metadata.originalRows();
        if (!committed.isCommitted() && metadataRejected && preparedCommit.metadata.needsAcceptanceRevalidation()) {
            repair.emplace(std::move(preparedCommit.metadata));
            repairBase = currentRoot.epoch();
        }
        if (installed)
            diagnostic.next(Phase::CommitRootPublication, pageWork);
        ownerLock.unlock();
        diagnostic.next(Phase::CommitCleanup, pageWork);
        if (!repair) m_metadata.finishAcceptance(acceptedRows);
        m_disposeMetadataCleanup(m_ownerContext, std::move(metadataCleanup));
        ownerLock.relock();
        if (!committed.isCommitted()) {
            if (!metadataRejected)
                return {};
            ++m_statistics.metadataRepreparations;
            continue;
        }

        // Acceptance consumes every proof at once. Original metadata Values
        // already own history delivery; release input storage outside the gate.
        // Abort instead erases each accepted revocation so refusal can resume.
        for (const auto &entry : owned.proofs) {
            if (!m_owner.releasePublicationIdentity(publicationProof(entry)))
                qFatal("Owned committed proof changed before revocation");
        }
        auto releasedOverlay = m_preparedTransactions.extract(transaction.id.value);
        diagnostic.next(Phase::CommitHistoryCollect, quint64(owned.proofs.size() + semanticTargets.size()));
        m_history.collectAfterEpochChangeLocked();
        diagnostic.next(Phase::CommitProviderRetire, 0);
        processRetirementsUnlocked(ownerLock);
        ++m_committedTransactions;
        if constexpr (std::is_same_v<Input, KisPageOperationPublication>)
            ++m_statistics.canonicalOperationCommits;
        const KisImageEpochCommitTicket ticket{committed.root.epoch(), preparedCommit.completion};
        ownerLock.unlock();
        releasedOverlay = {};
        ownerLock.relock();
        return ticket;
    }
}

KisImageEpochCommitTicket
KisPagePublicationCoordinator::restoreRetainedEpochLocked(const KisRetainedImageEpochSnapshot &retained,
                                                          const KisPageStore *diagnosticOwner,
                                                          QMutexLocker<QMutex> &ownerLock,
    const std::function<bool(const KisImageEpochRootSnapshot &, const KisPageRestorationChanges &)> &prepareAdapter,
    const std::function<void(const KisPageRestorationChanges &)> &completeAdapter)
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
    KisPreparedMutationCommit preparedCommit(m_budget);
    const auto releaseCommit = qScopeGuard([&] {
        if (ownerLock.isLocked()) ownerLock.unlock();
        preparedCommit.release();
        ownerLock.relock();
    });
    // These immutable roots are fixed for this call. Generate their actual
    // difference once; only default-cache directory inputs can grow on retry.
    try {
        if (!retainedRoot.appendRestoreTargets(currentRoot, &preparedCommit.restoreTargets)) return {};
    } catch (const std::bad_alloc &) { return {}; }
    const size_t rootTargetCount = preparedCommit.restoreTargets.size();
    for (;;) {
        const quint64 directoryRevision = m_metadata.pageRegistrationCount();
        const auto discardCommit = qScopeGuard([&] {
            if (ownerLock.isLocked()) ownerLock.unlock();
            preparedCommit.clear();
            ownerLock.relock();
        });
        auto &restoreTargets = preparedCommit.restoreTargets;
        auto &registeredPages = preparedCommit.directoryKeys;
        try {
            restoreTargets.resize(rootTargetCount);
            if (changesDefaults) {
                // The original target prefix is sorted and unique. Directory keys
                // are unique by shard ownership; suffix insertions need no second
                // membership authority or quadratic prefix scan.
                const size_t originalTargets = restoreTargets.size();
                registeredPages = m_metadata.registeredPageKeys();
                for (const KisPageKey &key : registeredPages) {
                    KisPageVersion page;
                    if (!currentRoot.resolve(key, &page)) return {};
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

            preparedCommit.prepareChanges(0, restoreTargets.size(), 0,
                prepareAdapter ? rootTargetCount : 0, adapterPrepared);
            for (const KisPageVersion &version : std::as_const(restoreTargets)) {
                KisPageMetadataCoordinator::PublicationInfo page;
                KisPageVersion current;
                if (!currentRoot.resolve(version.key, &current)
                    || !m_metadata.queryPublication(current, version, &page)) {
                    return {};
                }
                if (!page.target.version.isValid() && version.isDefaultPixel()) {
                    KisSurfaceEpochState surface;
                    if (!m_epochs.surfaceState(version.key.surface, retainedView, &surface)
                        || surface.defaultPixelRevision != version.defaultPixelRevision
                        || !surface.allocationDescriptor().isValid()) {
                        return {};
                    }
                }
            }
        } catch (const std::bad_alloc &) { return {}; }

        ownerLock.unlock();
        registeredPages = decltype(preparedCommit.directoryKeys){registeredPages.get_allocator()};
        KisPageStoreDiagnosticTimer diagnostic(diagnosticOwner,
                                               Phase::RestoreTransitionPreflight,
                                               quint64(restoreTargets.size()));
        const quint64 restoreTransitionCount = quint64(restoreTargets.size());
        // Current/retained roots are fixed across retries. Directory growth
        // can only add virtual-default targets; it cannot add cold allocated
        // wrappers. The caller's original manager gate preserves prepared
        // table identity and warm coverage until the successful delivery.
        if (!adapterPrepared) {
            const KisPageRestorationChanges changes(restoreTargets.data(),
                prepareAdapter ? preparedCommit.workspace.at<void *>(0) : nullptr, rootTargetCount);
            if (prepareAdapter && !prepareAdapter(retainedRoot, changes)) return {};
            adapterPrepared = true;
        }
        auto &publication = preparedCommit.metadata;
        publication = m_metadata.prepareRestoration(currentRoot, nextEpoch, restoreTargets.data(), qsizetype(restoreTargets.size()));
        KisImageEpochCommitResult candidateFailure;
        auto &rootCandidate = preparedCommit.restoreEpoch;
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
        if (publication.needsReprepare() || directoryChanged) {
            m_statistics.restoreMetadataRepreparations += publication.needsReprepare();
            m_statistics.restoreDirectoryRepreparations += directoryChanged;
            continue;
        }
        completion = m_readyHostCompletion;
        Q_ASSERT(completion.isValid()); // Prepared once by Store configuration.

        preparedCommit.beginBackingChanges();
        auto nextTarget = [target = restoreTargets.cbegin(), end = restoreTargets.cend()]
                          (KisPageVersion *version, KisBackingBudgetClass *before) mutable {
            if (target == end) return false;
            *version = *target++;
            *before = KisBackingBudgetClass::RetainedHistory;
            return true;
        };
        auto reader = publicationBackingReader(publication, currentRoot, std::move(nextTarget));
        preparedCommit.owner = this;
        preparedCommit.completion = completion;
        Q_ASSERT(preparedCommit.isValid());
        auto install = [&] { return preparedCommit.installRoot(nullptr); };
        using Acceptance = KisPageOwnerLedger::BackingAcceptance;
        const bool domainsReady = m_owner.synchronizeBackingDomains();
        const auto resources = domainsReady ? publication.prepareCpuResources()
            : KisPageMetadataCoordinator::PreparedPublication::CpuResources{};
        if (domainsReady && !resources.isValid()) return {};
        auto accepted = domainsReady ? preparedCommit.acceptFees(m_owner, resources, reader, install)
                                    : Acceptance::FeesRefused;
        if (accepted != Acceptance::FeesRefused) preparedCommit.finishInstallation();
        if (accepted == Acceptance::FeesRefused) {
            if (publication.genericInputsChanged()) {
                ++m_statistics.restoreMetadataRepreparations;
                continue;
            }
            return {};
        }
        const bool installed = accepted == Acceptance::Accepted;
        const bool metadataRejected =
            preparedCommit.result.status == KisImageEpochCommitStatus::Rejected;
        restored = preparedCommit.result;
        auto metadataCleanup = std::move(preparedCommit.metadataCleanup);
        if (installed) {
            completion = preparedCommit.completion;
        }
        ownerLock.unlock();
        if (installed && completeAdapter) {
            const KisPageRestorationChanges changes(restoreTargets.data(),
                prepareAdapter ? preparedCommit.workspace.at<void *>(0) : nullptr, rootTargetCount);
            completeAdapter(changes);
        }
        m_disposeMetadataCleanup(m_ownerContext, std::move(metadataCleanup));
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
        // Consume the final selection: abort must not shift the remaining
        // transaction directory after each independently accepted transition.
        const auto proof = remainingProofs->crbegin()->second.identity(transaction.id);
        if (!m_owner.ownsPublicationIdentity(proof))
            qFatal("Owned abort source proof changed before metadata transition");
        KisPageTransition transition;
        transition.kind = KisPageTransitionKind::AbortTransaction;
        transition.transaction = transaction.id;
        const KisPageMetadataTransitionResult result = m_metadata.applyOwner(proof.version.key, transition);
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
    m_history.collectAfterEpochChangeLocked();
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
    auto state = std::allocate_shared<KisPagePublicationDelta>(storage, storage, m_epochs.viewValues(), transaction);
    m_preparedTransactions.emplace(transaction.value, state);
    return state;
}

const KisPagePublicationCoordinator::SelectionMap *
KisPagePublicationCoordinator::findProofsLocked(KisPageTransactionId transaction) const
{
    const auto found = m_preparedTransactions.find(transaction.value);
    return found == m_preparedTransactions.cend() || found->second->proofs.empty() ? nullptr : &found->second->proofs;
}

KisPreparedPageProof KisPagePublicationCoordinator::projectProof(const KisPagePublicationIdentity &proof) const
{
    return proof.isValid() ? m_owner.exportPublication(proof)
        : KisPreparedPageProof{};
}

bool KisPagePublicationCoordinator::selectWriteBaseLocked(
    const KisPageTransaction &transaction, const KisPageWriteIntent &intent,
    KisPageWriteSelection *selection, QString *error)
{
    Q_ASSERT(selection);
    Q_ASSERT(!selection->write.source.version.isValid() && !selection->layout
        && !selection->surfaceLayout);
    const auto fail = [&](const QString &reason) { KisPageStoreDetail::setError(error, reason); return false; };
    const auto overlay = KisPageReadView::transactionOverlay(transaction.id);
    // This Store-owner window excludes edits to the original delta throughout
    // selection, including local virtual-default preparation. Do not reacquire
    // the same transaction for proof, removal and surface separately.
    const auto found = m_preparedTransactions.find(transaction.id.value);
    const auto *delta = found == m_preparedTransactions.cend() ? nullptr : found->second.get();
    KisPageVersion base;
    KisVersionSlotId slot;
    if (delta) {
        const auto proof = delta->proofs.find(intent.key);
        if (proof != delta->proofs.end()) {
            base = proof->second.version();
            slot = proof->second.metadataSlot();
        }
    }
    if (!base.isValid() && !m_epochs.resolve(intent.key, overlay, &base))
        return fail(QStringLiteral("write base does not resolve"));
    const bool removed = (intent.flags & quint8(KisPageWriteIntentFlag::SemanticRemoval))
        || (delta && delta->removals.count(intent.key));
    const bool discoverBefore = !removed && intent.inputKind != KisPageWriteInputKind::Semantic;
    using Selected = KisPageMetadataCoordinator::MutationBaseSelection;
    auto selected = m_metadata.selectMutationBase(base, discoverBefore, selection, error, &slot);
    // Root surface facts are needed only for a missing default, a removal or
    // semantic input. Keep one actual value in this selection window; ordinary
    // materialized writes construct none of these format/descriptor values.
    std::optional<KisSurfaceEpochState> surface;
    const auto rootSurface = [&] {
        if (surface) return true;
        surface.emplace();
        return m_epochs.surfaceState(intent.key.surface, overlay, &*surface);
    };
    if (selected == Selected::Absent) {
        // Implicit content is already identified by the view. Only a virgin
        // coordinate needs control/highwater; no empty Value or descriptor
        // is created merely to discard those default bytes in this write.
        KisPageVersion current;
        if (!base.isDefaultPixel() || !rootSurface()
            || !m_epochs.resolve(base.key, {}, &current)
            || !m_metadata.registerImplicitPage(current, error)) return false;
        selected = m_metadata.selectMutationBase(base, discoverBefore, selection, error, &slot);
    }
    if (selected != Selected::Selected) return false;
    if (removed || base.isDefaultPixel() || intent.inputKind == KisPageWriteInputKind::Semantic) {
        if (!m_operational || !rootSurface()) return fail(QStringLiteral("write surface does not resolve"));
        if (delta) for (const auto &change : delta->surfaceChanges) {
            if (change.after.surface == intent.key.surface) { *surface = change.after; break; }
        }
        selection->surfaceLayout = surface->allocationDescriptor();
        if (selection->layout && *selection->layout == *selection->surfaceLayout)
            selection->surfaceLayout.reset();
        else selection->layout.reset();
    } else if (!selection->layout)
        return fail(QStringLiteral("write base descriptor is unavailable"));
    selection->write.transaction = transaction.id;
    selection->write.writeMode = removed || base.isDefaultPixel() ? KisPageWriteMode::DiscardContents : intent.mode;
    if (!selection->descriptor().isValid())
        return fail(QStringLiteral("write identity or layout is unavailable"));
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPreparedPageSet
KisPagePublicationCoordinator::transactionDeltaLocked(KisPageTransactionId transaction) const
{
    KisPreparedPageSet result;
    result.transaction = transaction;
    const auto found = m_preparedTransactions.find(transaction.value);
    if (found != m_preparedTransactions.cend()) {
        result.proofs.reserve(qsizetype(found->second->proofs.size()));
        for (const auto &proof : found->second->proofs) result.proofs.append(projectProof(proof.second.identity(transaction)));
        result.surfaceChanges.reserve(qsizetype(found->second->surfaceChanges.size()));
        for (const auto &change : found->second->surfaceChanges) result.surfaceChanges.append(change);
        result.removedPages.reserve(qsizetype(found->second->removals.size()));
        for (const auto &key : found->second->removals) result.removedPages.append(key);
    }
    return result;
}

KisPageIdleCapacity KisPagePublicationCoordinator::takeIdleCapacityLocked(const KisPageKey &key)
{
    const qint64 now = QDeadlineTimer::current().deadline();
    auto *record = m_metadata.exchangeIdleCapacity(key, nullptr);
    if (!record) return {};
    Q_ASSERT(record->is_linked());
    m_idleCapacity.erase(m_idleCapacity.iterator_to(*record));
    const auto deadline = std::exchange(record->idleDeadline, 0);
    if (deadline <= now) {
        m_retirementQueue.acceptDetachedRecord(*record, record->replica());
        return {};
    }
    return {record->replica(), record->backingNode.second.binding(), record};
    // Ledger retains the original record until retag or retirement.
}

qint64 KisPagePublicationCoordinator::nextIdleDeadlineLocked() const
{
    return m_idleCapacity.empty() ? 0 : m_idleCapacity.front().idleDeadline;
}

void KisPagePublicationCoordinator::collectExpiredIdleCapacityLocked(qint64 now, qsizetype budget)
{
    while (budget-- > 0 && !m_idleCapacity.empty()) {
        auto &record = m_idleCapacity.front();
        if (record.idleDeadline > now) break;
        auto *indexed = m_metadata.exchangeIdleCapacity(record.backingNode.second.key(), nullptr);
        Q_ASSERT(indexed == &record); Q_UNUSED(indexed);
        m_idleCapacity.pop_front();
        record.idleDeadline = 0;
        m_retirementQueue.acceptDetachedRecord(record, record.replica());
    }
}

void KisPagePublicationCoordinator::drainIdleCapacityLocked()
{
    // Close/facade exit takes every original responsibility, independently
    // of future worker admission and of the candidates' eligibility ages.
    collectExpiredIdleCapacityLocked(std::numeric_limits<qint64>::max(), qsizetype(m_idleCapacity.size()));
}

void KisPagePublicationCoordinator::acceptCapacityLocked(const KisReplicaHandle &replica,
                                                         KisPageRetirementRecord *record)
{
    if (!record) {
        m_retirementQueue.acceptDetachedBacking(replica);
        return;
    }
    Q_ASSERT(record->replica() == replica && !record->is_linked());
    auto *previous = m_metadata.exchangeIdleCapacity(replica.version.key, record);
    if (previous) {
        m_idleCapacity.erase(m_idleCapacity.iterator_to(*previous));
        previous->idleDeadline = 0;
        m_retirementQueue.acceptDetachedRecord(*previous, previous->replica());
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
    if (capture.values.capacity() < *versions || capture.removedPages.capacity() < *removals
        || capture.stagedSurfaces.capacity() < *surfaces) return false;
    capture.values.clear(); capture.removedPages.clear(); capture.stagedSurfaces.clear();
    if (state) {
        for (const auto &proof : state->proofs) {
            capture.values.push_back(proof.second.value);
        }
        for (const auto &key : state->removals) capture.removedPages.push_back(key);
        for (const auto &change : state->surfaceChanges) capture.stagedSurfaces.push_back(change.after);
    }
    return true; // One owner cut, using already prepared actual storage only.
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
            auto identity = KisPagePublicationIdentity(value.proof);
            if (native) identity = {native->version(), native->sealedBy(), 0, true, native->valueSlot()};
            changes.push_back({{value.key, identity, value.removal, native}, {}, false, false, false, {}});
        }
        std::sort(changes.begin(), changes.end(), [](const auto &a, const auto &b) {
            return overlayKeyLess(a.replacement.key(), b.replacement.key());
        });
        const KisPagePublicationRows rows(changes.data(), changes.size());
        return preparePagePublicationLocked(transaction, rows, error, storageRefused, canonical, std::move(changes));
    } catch (const std::bad_alloc &) {
        if (storageRefused) *storageRefused = true;
        KisPageStoreDetail::setError(error, QStringLiteral("overlay storage preparation was refused"));
        return {};
    }
}

KisPagePublicationCoordinator::KisPreparedPagePublication
KisPagePublicationCoordinator::preparePagePublicationLocked(
    const KisPageTransaction &transaction, KisPagePublicationRows changes,
    QString *error, bool *storageRefused, bool canonical, KisPagePublicationChanges owned)
{
    if (storageRefused) *storageRefused = false;
    if (!hasActiveTransactionLocked(transaction) || !changes || changes.empty()) {
        KisPageStoreDetail::setError(error, QStringLiteral("overlay update input is invalid"));
        return {};
    }
    try {
        KisPreparedPagePublication result;
        result.data.emplace(KisMutationStorageAllocator<char>::retained(&m_budget));
        auto *candidate = &*result.data;
        candidate->owner = this;
        candidate->transaction = transaction;
        candidate->canonical = canonical;
        if (!owned.empty()) candidate->ownedChanges = std::move(owned);
        candidate->changes = changes;
        size_t proofInsertions = 0, removalInsertions = 0;
        const auto base = m_epochs.root(transaction.baseEpoch);
        if (!canonical) candidate->state = preparedStateLocked(transaction.id);
        else if (hasPreparedDeltaLocked(transaction.id) || isPreparingCommitLocked(transaction.id)) return {};
        auto *state = candidate->state.get();
        KisPageKey previous;
        for (auto &entry : candidate->changes) {
            // Every attempt refreshes dynamic acceptance facts; the producer
            // and original row survive a capacity refusal, candidate tails do not.
            entry.superseded = {};
            entry.supersededAuthority = nullptr;
            entry.supersededValue = nullptr;
            entry.detachesSuperseded = false;
            entry.acceptedIdle = nullptr;
            entry.acceptedOverlayIndex = 0;
            const auto &change = entry.replacement;
            const bool hasProof = change.hasProof();
            auto *native = hasProof ? change.native : nullptr;
            if (!change.key().isValid() || change.removal == hasProof
                || previous == change.key()
                || (hasProof
                    && (!(change.transaction() == transaction.id)
                        || !(change.version().key == change.key())
                        || (!native && !m_owner.ownsPreparedPageProof(projectProof(change.proofValue())))))) {
                KisPageStoreDetail::setError(error, QStringLiteral("overlay update change is invalid"));
                return {};
            }
            previous = change.key();
            Q_ASSERT(!entry.acceptedValue);
            if (state && hasProof) {
                entry.acceptedValue = state->values->pin(change.version(), change.metadataSlot(), native);
                if (!entry.acceptedValue) {
                    KisPageStoreDetail::setError(error, QStringLiteral("overlay final Value is unavailable"));
                    return {};
                }
            }
            if (hasProof && !native) native = m_metadata.nativeWrite(change.version());
            // This is the metadata consumer's original result, not permission
            // to skip validation of an independently exported capability.
            entry.replacement.native = native;
            const auto superseded = [&] {
                if (!state) return KisPagePublicationIdentity{};
                const auto found = state->proofs.find(change.key());
                entry.acceptedOverlayIndex = size_t(found - state->proofs.begin());
                return found == state->proofs.end() ? KisPagePublicationIdentity{} : found->second.identity(transaction.id);
            }();
            if (superseded.isValid()) {
                if ((!superseded.native && !m_owner.ownsPreparedPageProof(projectProof(superseded)))
                    || (hasProof
                        && superseded.version.generation.value
                            >= change.version().generation.value)) {
                    KisPageStoreDetail::setError(error, QStringLiteral("overlay update does not replace its current proof"));
                    return {};
                }
            }
            if (superseded.isValid()) {
                // The original overlay holds this Value until acceptSelection
                // exchanges it into row.acceptedValue. Borrow that same memory,
                // never the incoming writer's earlier initialization source.
                const auto *selected = static_cast<const KisPageValueCell *>(
                    (state->proofs.begin() + entry.acceptedOverlayIndex)->second.value);
                if (native && superseded.native && selected->authorityReplica == kisInlineReplicaSlot(selected->slot)) {
                    entry.supersededValue = selected;
                } else {
                    // This is the actual independent input, not a second
                    // copy for every native row. Reserve once so earlier
                    // borrowed addresses remain stable through whole-D retry.
                    if (candidate->ownedAuthorities.empty())
                        candidate->ownedAuthorities.reserve(candidate->changes.size());
                    candidate->ownedAuthorities.push_back(m_owner.publicationAuthority(superseded));
                    entry.supersededAuthority = &candidate->ownedAuthorities.back();
                }
            }
            if (!canonical && (superseded.isValid() || native)) {
                ++candidate->mutationCount;
                // Metadata owns detachment/reuse eligibility. The ledger's
                // optional idle classification is decided at its real accept
                // cut, independently of whether this version has consumers.
                entry.detachesSuperseded = superseded.isValid();
            }
            const bool removalWasInBase = change.removal && base.containsPage(change.key());
            const bool wasRemoved = state && state->removals.count(change.key()) != 0;
            if (!canonical && hasProof && !superseded.isValid())
                ++proofInsertions;
            if (!canonical && removalWasInBase && !wasRemoved)
                ++removalInsertions;
            entry.superseded = superseded;
            entry.removalWasInBase = removalWasInBase;
            entry.wasRemoved = wasRemoved;
        }

        // A sibling may install while this candidate prepares metadata.
        // Reserve the sum of outstanding additions in the original tables;
        // cancellation drops its claim but does not destroy reusable capacity.
        const auto reserve = [](auto &map, size_t outstanding, size_t additions) {
            if (additions > map.max_size() - map.size()
                || outstanding > map.max_size() - map.size() - additions)
                throw std::bad_alloc();
            const size_t required = map.size() + outstanding + additions;
            if (required > map.capacity()) map.reserve(required);
        };
        if (state) {
            reserve(state->proofs, state->proofInsertions.load(), proofInsertions);
            reserve(state->removals, state->removalInsertions.load(), removalInsertions);
            candidate->proofInsertions = proofInsertions;
            candidate->removalInsertions = removalInsertions;
            state->proofInsertions.fetch_add(candidate->proofInsertions);
            state->removalInsertions.fetch_add(candidate->removalInsertions);
        }
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
    const KisPagePublicationIdentity &proof) noexcept
{
    auto transaction = m_preparedTransactions.find(proof.transaction.value);
    if (transaction == m_preparedTransactions.end())
        qFatal("Owned prepared proof has no transaction record");
    auto &state = *transaction->second;
    auto stored = state.proofs.find(proof.version.key);
    // Abort verified this original capability before its accepted metadata
    // transition. Removing the version also consumes its inline CPU proof;
    // a protected Historical version or exported capability still needs
    // explicit revocation. There is no post-acceptance resource refusal.
    if (stored == state.proofs.end() || !(stored->second.identity(proof.transaction) == proof))
        qFatal("Owned prepared proof changed before revocation");
    if (!m_owner.releasePublicationIdentity(proof)) {
        KisPageMetadataCoordinator::VersionInfo remaining;
        m_metadata.versionSnapshot(proof.version, &remaining);
        if (remaining.version.isValid())
            qFatal("Owned aborted proof survived without its capability");
    }
    state.values->release(stored->second.value);
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
    Q_ASSERT(!result.reserved && !result.descriptor());
    Q_ASSERT(!selected || selected.get() == &descriptor);
    if (!selected) selected = std::allocate_shared<const KisPageAllocationDescriptor>(
        KisMutationStorageAllocator<KisPageAllocationDescriptor>::retained(&m_budget), descriptor);
    result.descriptor() = std::move(selected);
    // This unregistered original result is not visible through metadata yet.
    // Only its successful acceptance publishes a descriptor change. A local
    // preparation/refusal must not invalidate another default/restore candidate.
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
    PreparedDescriptors nodes;
    if (!count) return nodes;
    std::optional<KisMutationStorageAllocator<DescriptorEntry>> storage;
    for (size_t i = 0; i < count; ++i) {
        if (m_metadata.versionDescriptorMatches(changes[i].version, changes[i].descriptor)) continue;
        if (!storage) {
            storage.emplace(KisMutationStorageAllocator<DescriptorEntry>::retained(&m_budget));
            nodes = PreparedDescriptors(PreparedDescriptors::allocator_type(*storage));
            nodes.reserve(count - i);
        }
        std::shared_ptr<const KisPageAllocationDescriptor> value;
        if (sharedLayout && **sharedLayout == changes[i].descriptor) value = *sharedLayout;
        if (!value) {
            const auto same = std::find_if(nodes.cbegin(), nodes.cend(), [&](const auto &entry) {
                return entry.second && *entry.second == changes[i].descriptor;
            });
            if (same != nodes.cend()) value = same->second;
        }
        if (!value) value = std::allocate_shared<const KisPageAllocationDescriptor>(
            KisMutationStorageAllocator<KisPageAllocationDescriptor>(*storage), changes[i].descriptor);
        nodes.emplace_back(changes[i].version, std::move(value));
    }
    if (!nodes.empty()) ++m_statistics.descriptorCapacityPreparations;
    return nodes;
}

void KisPagePublicationCoordinator::discardDescriptorAdditionsLocked(
    PreparedDescriptors *prepared, QMutexLocker<QMutex> &ownerLock)
{
    if (!prepared->hasStorage()) return;
    if (ownerLock.isLocked()) ownerLock.unlock();
    prepared->reset();
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

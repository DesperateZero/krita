/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_PAGE_PUBLICATION_INPUT_P_H
#define KIS_PAGE_PUBLICATION_INPUT_P_H

#include "KisPageStoreTypes.h"
#include "KisPagePublicationRows_p.h"
#include "KisMutationStorage_p.h"
#include "KisPageMetadataCoordinator_p.h"
#include "KisPageRetirementRecord_p.h"
#include <vector>
#include <boost/iterator/iterator_facade.hpp>

struct KisPagePrivateWrite;
struct KisPageRetirementRecord;

// Original publication locator, admitted only after validating a real input.
// The issuer's monotonically unique stamp names its validation fact; the
// generational slot is a checked location. Authority/layout/completion stay
// with their original owners, never duplicated in each transaction selection.
// This is NOT an external capability; full input authenticity is checked
// before conversion and again at every public proof-taking boundary.
struct KisPagePublicationIdentity {
    KisPageVersion version;
    KisPageTransactionId transaction;
    quint64 stamp = 0;
    KisVersionSlotId metadataSlot;
    bool native = false;
    KisPagePublicationIdentity() = default;
    KisPagePublicationIdentity(const KisPreparedPageProof &proof)
        : version(proof.authority.version), transaction(proof.transaction), stamp(proof.providerValidationStamp) {}
    KisPagePublicationIdentity(const KisPageVersion &v, KisPageTransactionId t, quint64 s, bool n,
                            KisVersionSlotId slot = {})
        : version(v), transaction(t), stamp(s), metadataSlot(slot), native(n) {}
    bool isValid() const { return version.isValid() && transaction.isValid() && stamp; }
    bool matches(const KisPreparedPageProof &proof) const
    { return version == proof.authority.version && transaction == proof.transaction && stamp == proof.providerValidationStamp; }
    bool operator==(const KisPagePublicationIdentity &other) const
    { return version == other.version && transaction == other.transaction && stamp == other.stamp && native == other.native; }
};

struct KisPagePublicationChange {
    struct Input {
        KisPageKey key;
        KisPreparedPageProof proof;
        bool removal = false;
    };
    struct Replacement {
        struct Owned {
            KisPageKey key;
            KisPagePublicationIdentity proof;
        };
        // CPU publication borrows the original frozen result. Only a removal
        // or an independently owned external input constructs the value form.
        union Payload {
            Owned owned;
            Payload() {}
        } payload;
        bool removal = false;
        KisPagePrivateWrite *native = nullptr;
        bool borrowed = false;
        const KisPagePublicationIdentity *originalProof = nullptr;
        Replacement() { new (&payload.owned) Owned{}; }
        Replacement(const KisPageKey &key, const KisPagePublicationIdentity &proof,
                    bool removed = false, KisPagePrivateWrite *result = nullptr)
            : removal(removed), native(result)
        { new (&payload.owned) Owned{key, proof}; }
        explicit Replacement(KisPagePrivateWrite *result) : native(result), borrowed(true)
        { Q_ASSERT(result); }
        explicit Replacement(const KisPagePublicationIdentity *proof) : originalProof(proof)
        { Q_ASSERT(proof); new (&payload.owned) Owned{}; }
        const KisPageKey &key() const
        { return borrowed ? native->target.version.key : originalProof ? originalProof->version.key : payload.owned.key; }
        const KisPageVersion &version() const { return borrowed ? native->target.version : originalProof ? originalProof->version : payload.owned.proof.version; }
        KisPageTransactionId transaction() const
        { return borrowed ? native->proof.transaction : originalProof ? originalProof->transaction : payload.owned.proof.transaction; }
        bool hasProof() const
        { return borrowed ? native->proof.isValid() && native->target.version.isValid() : originalProof ? originalProof->isValid() : payload.owned.proof.isValid(); }
        bool nativeProof() const { return borrowed || (originalProof ? originalProof->native : payload.owned.proof.native); }
        KisVersionSlotId metadataSlot() const
        { return borrowed ? native->acceptedSlot : originalProof ? originalProof->metadataSlot : payload.owned.proof.metadataSlot; }
        quint64 validationStamp() const
        { return borrowed ? native->proof.providerValidationStamp : originalProof ? originalProof->stamp : payload.owned.proof.stamp; }
        KisPagePublicationIdentity proofValue() const
        {
            return borrowed ? KisPagePublicationIdentity{native->target.version, native->proof.transaction,
                native->proof.providerValidationStamp, true, native->acceptedSlot} : originalProof ? *originalProof : payload.owned.proof;
        }
    } replacement;
    KisPagePublicationChange() = default;
    KisPagePublicationChange(Replacement replacement, KisPagePublicationIdentity superseded = {},
        bool removalWasInBase = false, bool wasRemoved = false, bool detachesSuperseded = false,
        const KisReplicaHandle *supersededAuthority = nullptr)
        : replacement(std::move(replacement)), superseded(superseded), removalWasInBase(removalWasInBase),
          wasRemoved(wasRemoved), detachesSuperseded(detachesSuperseded), supersededAuthority(supersededAuthority) {}
    KisPagePublicationIdentity superseded;
    bool removalWasInBase = false;
    bool wasRemoved = false;
    // Metadata's detachment plan, revalidated against real consumers at install.
    // It grants neither a budget class change nor optional idle capacity.
    bool detachesSuperseded = false;
    // Native publication borrows its frozen producer's original source.
    // Only a removal/foreign source needs a value in the preparation owner's
    // authority storage. All consumers finish before either source dies.
    const KisReplicaHandle *supersededAuthority = nullptr;
    // Commit output, borrowed until immediate handover under Store admission.
    KisPageRetirementRecord *acceptedIdle = nullptr;
    // Preparation's position is only a hint across unlocked work. Acceptance
    // checks its current key, then records the verified position consumed by
    // replacement without changing the selection table's structure.
    union {
        size_t acceptedOverlayIndex = 0; // Prepared destination's selected position.
        void *canonicalRecord; // Independent input's actual release/authority output.
    };
    KisPagePreparedBackingChange backingPreparation;
    struct MetadataAcceptance {
        KisPagePublicationChange *input = nullptr;
        void *shard = nullptr;
        void *page = nullptr;
        void *prepared = nullptr;
        KisVersionSlotId replacementSlot;
        quint16 shardIndex = 0; // configure() permits at most 4096 shards.
        // Only the first adjunct in a shard group requests native growth.
    } metadataAcceptance;
    // Metadata borrows these original facts until preparation/acceptance
    // finishes. No independently owned detachment list or ordinal remap.
    bool hasMutation() const { return superseded.isValid() || replacement.native; }
    const KisPageVersion &mutationVersion() const
    { return superseded.isValid() ? superseded.version : replacement.version(); }
    quint64 expectedStamp() const { return superseded.native ? superseded.stamp : 0; }
    const KisReplicaHandle *expectedAuthority() const
    { return superseded.native ? supersededAuthority : nullptr; }
};
using KisPagePublicationChanges = std::vector<KisPagePublicationChange,
    KisMutationStorageAllocator<KisPagePublicationChange>>;

inline KisPagePublicationChange &KisPagePublicationRows::Iterator::operator*() const
{
    Q_ASSERT(!indirect || (indirect[index] && indirect[index]->publicationRow));
    return indirect ? *indirect[index]->publicationRow : direct[index];
}
inline KisPagePublicationRows KisPagePublicationRows::subspan(size_t offset) const
{
    Q_ASSERT(offset <= count);
    return indirect ? KisPagePublicationRows(indirect + offset, count - offset)
                    : KisPagePublicationRows(direct + offset, count - offset);
}

// Sort only a phase's adjunct fields; original facts and addresses stay fixed.
template<class T, T KisPagePublicationChange::*Member>
class KisPublicationMemberIterator : public boost::iterator_facade<KisPublicationMemberIterator<T, Member>,
    T, boost::random_access_traversal_tag>
{
public:
    KisPublicationMemberIterator() = default;
    explicit KisPublicationMemberIterator(KisPagePublicationRows::Iterator row) : row(row) {}
private:
    T &dereference() const { return (*row).*Member; }
    bool equal(const KisPublicationMemberIterator &other) const { return row == other.row; }
    void increment() { ++row; }
    void decrement() { --row; }
    void advance(ptrdiff_t n) { row += n; }
    ptrdiff_t distance_to(const KisPublicationMemberIterator &other) const { return other.row - row; }
    KisPagePublicationRows::Iterator row;
    friend class boost::iterator_core_access;
};

#endif

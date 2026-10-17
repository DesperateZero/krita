/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_PAGE_PUBLICATION_INPUT_P_H
#define KIS_PAGE_PUBLICATION_INPUT_P_H

#include "KisPageStoreTypes.h"
#include "KisMutationStorage_p.h"
#include <vector>

struct KisPagePrivateWrite;
struct KisPageRetirementRecord;

// Internal identity of an already validated capability. It is not an external
// capability: full values are projected only from their original live owner.
struct KisPagePublicationProof {
    KisPageVersion version;
    KisPageTransactionId transaction;
    quint64 stamp = 0;
    KisReplicaAllocationIdentity allocation;
    bool native = false;
    KisPagePublicationProof() = default;
    KisPagePublicationProof(const KisPreparedPageProof &proof)
        : version(proof.authority.version), transaction(proof.transaction), stamp(proof.providerValidationStamp),
          allocation(proof.authority.allocationIdentity()) {}
    KisPagePublicationProof(const KisPageVersion &v, KisPageTransactionId t, quint64 s, bool n,
                            const KisReplicaAllocationIdentity &a)
        : version(v), transaction(t), stamp(s), allocation(a), native(n) {}
    bool isValid() const { return version.isValid() && transaction.isValid() && stamp; }
    bool matches(const KisPreparedPageProof &proof) const
    { return version == proof.authority.version && transaction == proof.transaction && stamp == proof.providerValidationStamp
        && allocation == proof.authority.allocationIdentity(); }
    bool operator==(const KisPagePublicationProof &other) const
    { return version == other.version && transaction == other.transaction && stamp == other.stamp
        && allocation == other.allocation && native == other.native; }
};

struct KisPagePublicationChange {
    struct Input {
        KisPageKey key;
        KisPreparedPageProof proof;
        bool removal = false;
    };
    struct Replacement {
        KisPageKey key;
        KisPagePublicationProof proof;
        bool removal = false;
        KisPagePrivateWrite *native = nullptr;
    } replacement;
    KisPagePublicationProof superseded;
    bool removalWasInBase = false;
    bool wasRemoved = false;
    bool releasesCapacity = false;
    KisReplicaHandle supersededAuthority;
    // Commit output, borrowed until immediate handover under Store admission.
    KisPageRetirementRecord *acceptedIdle = nullptr;
    // Borrowed only inside installOverlayLocked's uninterrupted owner gate.
    // Revalidation overwrites it on every attempt; it is never an authority.
    KisPagePublicationProof *acceptedOverlayProof = nullptr;
    // Metadata borrows these original facts until preparation/acceptance
    // finishes. No independently owned detachment list or ordinal remap.
    bool hasMutation() const { return superseded.isValid() || replacement.native; }
    const KisPageVersion &mutationVersion() const
    { return superseded.isValid() ? superseded.version : replacement.proof.version; }
    quint64 expectedStamp() const { return superseded.native ? superseded.stamp : 0; }
    const KisReplicaHandle *expectedAuthority() const
    { return superseded.native ? &supersededAuthority : nullptr; }
};
using KisPagePublicationChanges = std::vector<KisPagePublicationChange,
    KisMutationStorageAllocator<KisPagePublicationChange>>;

#endif

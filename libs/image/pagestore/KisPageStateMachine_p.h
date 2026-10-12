/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_PAGE_STATE_MACHINE_P_H
#define KIS_PAGE_STATE_MACHINE_P_H

#include "KisPageStateMachine.h"
#include "KisMutationStorage_p.h"

#include <boost/container/small_vector.hpp>
#include <type_traits>

// Temporary values, never metadata authority. Publication keeps these same
// values through installation. The enclosing local scope or prepared Data
// retains the storage owner; nested values borrow it, including after a move.
// Inline values belong to that scope/allocation. Spills retain their actual
// allocation charge; protection arrays keep their unbounded capacity.
using KisPageWorkingStorage = KisMutationStorageAllocator<char, false>;
template<class T, size_t InlineCapacity = 0> using KisPageWorkingArray = std::conditional_t<
    InlineCapacity == 0, std::vector<T, KisMutationStorageAllocator<T, false>>,
    boost::container::small_vector<T, InlineCapacity, KisMutationStorageAllocator<T, false>>>;

struct KisPageWorkingReplica
{
    explicit KisPageWorkingReplica(KisPageWorkingStorage storage)
        : readLeases(storage), pendingLastUses(storage) {}
    KisReplicaHandle replica;
    KisReplicaValidity validity = KisReplicaValidity::Allocated;
    KisPageOperationId activeOperation;
    KisPageWorkingArray<KisPageLeaseId> readLeases;
    quint32 pinCount = 0;
    KisPageWorkingArray<KisCompletionTicket> pendingLastUses;
};

struct KisPageWorkingVersion
{
    explicit KisPageWorkingVersion(KisPageWorkingStorage storage)
        : replicas(decltype(replicas)::allocator_type(storage)), capturedReadViews(storage) {}
    KisPageVersion version;
    KisPagePublicationState publication = KisPagePublicationState::Unpublished;
    KisPageWorkingArray<KisPageWorkingReplica, 1> replicas;
    KisReplicaHandle authority;
    KisPageTransactionId preparedBy;
    KisPageWorkingArray<KisImageEpochSnapshotToken> capturedReadViews;
    quint32 nativeReaders = 0;

    bool isVirtualDefault() const
    {
        return version.isDefaultPixel() && replicas.empty() && !authority.isValid()
            && (publication == KisPagePublicationState::Published
                || publication == KisPagePublicationState::Historical);
    }
    KisPageWorkingReplica *findReplica(const KisReplicaHandle &identity)
    {
        const auto found = std::find_if(replicas.begin(), replicas.end(),
            [&](const auto &value) { return value.replica == identity; });
        return found == replicas.end() ? nullptr : &*found;
    }
    const KisPageWorkingReplica *findReplica(const KisReplicaHandle &identity) const
    {
        return const_cast<KisPageWorkingVersion *>(this)->findReplica(identity);
    }
};

struct KisPageWorkingState
{
    explicit KisPageWorkingState(KisPageWorkingStorage storage)
        : versions(decltype(versions)::allocator_type(storage)) {}
    KisPageKey key;
    KisImageEpochId publishedEpoch;
    KisPageGeneration publishedGeneration;
    quint64 publishedDefaultPixelRevision = 0;
    KisPageGeneration nextGeneration;
    KisPageGeneration nativeGeneration;
    KisPageTransactionId nativeTransaction;
    KisPageWorkingArray<KisPageWorkingVersion, 2> versions;
    KisPageWriterStateSnapshot writer;
    KisAuthorityHandoffStateSnapshot authorityHandoff;

    void setHeader(const KisPageStateSnapshot &header)
    {
        key = header.key;
        publishedEpoch = header.publishedEpoch;
        publishedGeneration = header.publishedGeneration;
        publishedDefaultPixelRevision = header.publishedDefaultPixelRevision;
        nextGeneration = header.nextGeneration;
        nativeGeneration = header.nativeGeneration;
        nativeTransaction = header.nativeTransaction;
        writer = header.writer;
        authorityHandoff = header.authorityHandoff;
    }
    KisPageStateSnapshot header() const
    {
        KisPageStateSnapshot result;
        result.key = key;
        result.publishedEpoch = publishedEpoch;
        result.publishedGeneration = publishedGeneration;
        result.publishedDefaultPixelRevision = publishedDefaultPixelRevision;
        result.nextGeneration = nextGeneration;
        result.nativeGeneration = nativeGeneration;
        result.nativeTransaction = nativeTransaction;
        result.writer = writer;
        result.authorityHandoff = authorityHandoff;
        return result;
    }
    KisPageWorkingVersion *findVersion(const KisPageVersion &identity)
    {
        const auto found = std::find_if(versions.begin(), versions.end(),
            [&](const auto &value) { return value.version == identity; });
        return found == versions.end() ? nullptr : &*found;
    }
    KisPageWorkingVersion *publishedVersion()
    {
        const auto found = std::find_if(versions.begin(), versions.end(), [](const auto &value) {
            return value.publication == KisPagePublicationState::Published;
        });
        return found == versions.end() ? nullptr : &*found;
    }
};

struct KisPageWorkingResult
{
    explicit KisPageWorkingResult(KisPageWorkingStorage storage)
        : next(storage), effects(storage) {}
    bool accepted = false;
    KisPageWorkingState next;
    KisPageWorkingArray<KisPageTransitionEffect> effects;
    QString rejectionReason;
};

inline KisPageVersionStateSnapshot kisPageVersionValue(const KisPageStateSnapshot &)
{
    return {};
}
inline KisPageWorkingVersion kisPageVersionValue(const KisPageWorkingState &state)
{
    return KisPageWorkingVersion(state.versions.get_allocator());
}
inline KisPageVersionStateSnapshot kisPageVersionValue(const KisPageVersionStateSnapshot &)
{
    return {};
}
inline KisPageWorkingVersion kisPageVersionValue(const KisPageWorkingVersion &version)
{
    return KisPageWorkingVersion(version.replicas.get_allocator());
}
inline KisReplicaStateSnapshot kisPageReplicaValue(const KisPageVersionStateSnapshot &)
{
    return {};
}
inline KisPageWorkingReplica kisPageReplicaValue(const KisPageWorkingVersion &version)
{
    return KisPageWorkingReplica(version.replicas.get_allocator());
}

#endif // KIS_PAGE_STATE_MACHINE_P_H

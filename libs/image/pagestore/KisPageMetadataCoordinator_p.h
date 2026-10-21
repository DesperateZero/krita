/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_METADATA_COORDINATOR_P_H
#define KIS_PAGE_METADATA_COORDINATOR_P_H

#include "KisPageMetadataArena_p.h"
#include "KisPageMetadataIndex_p.h"
#include "KisPageStateMachine.h"

class KisCpuResidentBinding;

// One selected input, retained by the original operation through comparison,
// resource checkout and acquisition. Layout is either the selected immutable
// version's layout or the actual surface value for a default/semantic input.
struct KisPageWriteSelection {
    KisPageTransition write;
    KisReplicaHandle recoverableBefore;
    std::shared_ptr<KisCpuResidentBinding> binding;
    std::shared_ptr<const KisPageAllocationDescriptor> layout;
    KisPageAllocationDescriptor surfaceLayout;
    const KisPageAllocationDescriptor &descriptor() const
    { return layout ? *layout : surfaceLayout; }
};

// Lives in the original mutation's stable cold page. The shard activity only
// borrows it while reserved; it is not a readable version until acceptance.
struct KisPagePrivateWrite {
    KisReplicaHandle target;
    KisReplicaHandle source;
    struct NativeProof {
        KisPageTransactionId transaction;
        quint64 providerValidationStamp = 0;
        bool isValid() const { return transaction.isValid() && providerValidationStamp; }
    } proof;
    // A value projection, not an ownership test. Real consumers still use
    // the owner's dynamic validate/owns/revoke policy. The original result
    // remains alive and immutable through Sealing/CommitPreparation.
    KisPreparedPageProof proofValue(const KisCompletionTicket &completion) const
    {
        if (!completed || !proof.isValid()) return {};
        KisPreparedPageProof value;
        value.authority = target; value.transaction = proof.transaction;
        value.providerValidationStamp = proof.providerValidationStamp;
        value.producerCompletion = completion;
        return value;
    }
    bool matchesProof(const KisPreparedPageProof &value, const KisCompletionTicket &completion) const
    {
        return proof.isValid() && proof.transaction == value.transaction
            && proof.providerValidationStamp == value.providerValidationStamp
            && target == value.authority && completion == value.producerCompletion;
    }
    // While reserved these belong to the new result. Acceptance transfers
    // them to metadata; direct replacement returns old references here for
    // destruction at the original unlocked scope exit. Identity stays intact.
    std::shared_ptr<const KisPageAllocationDescriptor> descriptor;
    std::shared_ptr<KisCpuResidentBinding> binding;
    // The original page activity retains this directory node until cancel or
    // acceptance. Consumers still take its shard gate and validate the active
    // result; they do not rediscover the node by key inside that same lifetime.
    struct Location {
        void *shard = nullptr;
        void *page = nullptr;
        Location() = default;
        Location(void *s, void *p) : shard(s), page(p) {}
        // A copied private value is not the activity's registered result.
        // In particular it cannot retain a location after the original exits.
        Location(const Location &) noexcept {}
        Location &operator=(const Location &) noexcept { shard = page = nullptr; return *this; }
        Location(Location &&other) noexcept
            : shard(std::exchange(other.shard, nullptr)), page(std::exchange(other.page, nullptr)) {}
        Location &operator=(Location &&other) noexcept
        { shard = std::exchange(other.shard, nullptr); page = std::exchange(other.page, nullptr); return *this; }
    } location;
    // Set by metadata acceptance. This is a generational locator, not another
    // version authority; readers validate the record identity under its gate.
    KisVersionSlotId acceptedSlot;
    bool reserved = false;
    bool completed = false;
    bool freshMetadataRequired = false;
};

struct KisPageNativeReadRef {
    static constexpr quint32 End = std::numeric_limits<quint32>::max();
    KisVersionSlotId version;
    quint32 shard = 0;
    quint32 next = End;
};
static_assert(sizeof(KisPageNativeReadRef) == 16);

/**
 * Compact authoritative records. Public/reference snapshots are projections
 * of these records and never own production metadata. M2 stores them in the
 * shard-owned generational arenas; M3 links and indexes their typed slots
 * while the compact page/activity directory projects public values on demand.
 */
struct KisVersionRecord {
    KisPageVersion version;
    KisVersionSlotId slot;
    KisVersionSlotId previousVersion;
    KisVersionSlotId nextVersion;
    KisVersionSlotId previousHistory;
    KisVersionSlotId nextHistory;
    KisVersionSlotId nextMutable;
    KisReplicaSlotId firstReplica;
    KisReplicaSlotId authorityReplica;
    KisMetadataOverflowSlotId capturedReadViews;
    KisVersionFactsSlotId facts;
    KisPageTransactionId preparedBy;
    quint32 replicaCount = 0;
    KisPagePublicationState publication = KisPagePublicationState::Unpublished;
};

// Immutable layout and the owner's single internal CPU completion capability.
// Generic exported capabilities can coexist and retain their original owner
// records. This slot never copies version/transaction/authority from metadata.
struct KisVersionFactsRecord {
    std::shared_ptr<const KisPageAllocationDescriptor> descriptor;
    quint64 stamp = 0;
    KisPageTransactionId transaction;
    std::shared_ptr<KisCpuResidentBinding> nativeBinding;
    KisReplicaSlotId nativeReplica;
    quint32 nativeReaders = 0;
};
static_assert(sizeof(KisVersionFactsRecord) <= 64);

struct KisReplicaRecord {
    KisReplicaAllocationIdentity identity;
    quint64 layoutRevision = 0;
    quint64 formatId = 0;
    quint64 byteSize = 0;
    KisVersionSlotId ownerVersion;
    KisReplicaSlotId nextReplica;
    KisMetadataOverflowSlotId overflow;
    KisPageOperationId activeOperation;
    QRect validRect;
    QSize pageExtent;
    quint32 rowStride = 0;
    quint32 pinCount = 0;
    KisPageAccessDomain domain = KisPageAccessDomain::Unknown;
    KisReplicaValidity validity = KisReplicaValidity::Allocated;
};

enum class KisMetadataOverflowKind : quint8 {
    CapturedReadView,
    ReadLease,
    PendingLastUse
};

struct KisMetadataOverflowNode {
    KisMetadataOverflowSlotId next;
    KisCompletionTicket completion;
    quint64 value = 0;
    KisMetadataOverflowKind kind = KisMetadataOverflowKind::CapturedReadView;
};

static_assert(sizeof(KisVersionRecord) <= 128);
static_assert(sizeof(KisReplicaRecord) <= 128);
static_assert(sizeof(KisMetadataOverflowNode) <= 64);

template<>
struct KisSlotIdFor<KisVersionRecord> {
    using Type = KisVersionSlotId;
};

template<>
struct KisSlotIdFor<KisVersionFactsRecord> {
    using Type = KisVersionFactsSlotId;
};

template<>
struct KisSlotIdFor<KisReplicaRecord> {
    using Type = KisReplicaSlotId;
};

template<>
struct KisSlotIdFor<KisMetadataOverflowNode> {
    using Type = KisMetadataOverflowSlotId;
};

namespace KisPageStoreDetail { class MetadataBudgetAuthority; }

// A caller-owned terminal value, not another metadata owner. Detached empty
// blocks remain charged until clear() destroys them outside every held owner
// gate. Concatenation and transfer use the blocks' existing intrusive links.
class KRITAIMAGE_EXPORT KisPageMetadataReadCleanup
{
public:
    KisPageMetadataReadCleanup() = default;
    ~KisPageMetadataReadCleanup();
    KisPageMetadataReadCleanup(const KisPageMetadataReadCleanup &) = delete;
    KisPageMetadataReadCleanup &operator=(const KisPageMetadataReadCleanup &) = delete;
    KisPageMetadataReadCleanup(KisPageMetadataReadCleanup &&) noexcept;
    KisPageMetadataReadCleanup &operator=(KisPageMetadataReadCleanup &&) noexcept;
    bool isEmpty() const { return m_bytes == 0; }
    quint64 byteSize() const { return m_bytes; }
    void clear() noexcept;

private:
    using Blocks = KisShardSlotArena<KisMetadataOverflowNode, 16 * 1024>::ReleasedBlocks;
    std::shared_ptr<KisPageStoreDetail::MetadataBudgetAuthority> m_authority;
    quint64 m_bytes = 0;
    Blocks m_blocks;
    friend class KisPageMetadataCoordinator;
};

#endif // KIS_PAGE_METADATA_COORDINATOR_P_H

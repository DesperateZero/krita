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
struct KisPageRetirementRecord;

// Borrowed from the original configured metadata core. Its page directory
// only inserts nodes, so the enclosing Store/operation retains this address.
// This carries a location, never the right to write: acceptance still checks
// the exact generational base and all current shard facts after unlocking.
struct KisPageMutationBaseLocation {
    void *shard = nullptr;
    void *node = nullptr;
    size_t shardIndex = 0;
};

// One selected input, retained by the original operation through comparison,
// resource checkout and acquisition. Layout is either the selected immutable
// version's layout or the actual surface value for a default/semantic input.
// Facts for one actual write. Read leases, completion lists, root epochs and
// transition kinds are not part of native acquisition. Only a real generic
// lease boundary projects the reference-model transition below.
struct KisPageWriteInput {
    KisPageVersion baseVersion, version;
    // The base query already resolves this generational location. It is a
    // hint, not authority: acceptance checks the exact version under its
    // shard gate after every unlocked provider/physical preparation window.
    KisVersionSlotId baseSlot;
    KisPageMutationBaseLocation baseLocation;
    // The accepted target belongs to the original private/request result.
    // Selection retains only the exact source, never another result copy.
    KisReplicaHandle source;
    KisPageOperationId operation;
    KisPageWriterToken writer; // Only the generic lease consumer needs a token.
    KisPageTransactionId transaction;
    KisPageWriteMode writeMode = KisPageWriteMode::PreserveContents;
    KisPageWriteInput() = default;
    explicit KisPageWriteInput(const KisPageTransition &value)
        : baseVersion(value.baseVersion), version(value.version), source(value.source),
          operation(value.operation), writer(value.writer), transaction(value.transaction), writeMode(value.writeMode) {}
    KisPageTransition leaseTransition(const KisReplicaHandle &target,
        KisPageTransitionKind kind = KisPageTransitionKind::AcquireWrite) const
    {
        KisPageTransition value;
        value.kind = kind; value.baseVersion = baseVersion; value.version = version;
        value.source = source; value.target = target; value.operation = operation;
        value.writer = writer; value.transaction = transaction; value.writeMode = writeMode;
        return value;
    }
};

struct KisPageWriteSelection {
    KisPageWriteInput write;
    KisReplicaHandle recoverableBefore;
    std::shared_ptr<KisCpuResidentBinding> binding;
    std::shared_ptr<const KisPageAllocationDescriptor> layout;
    std::optional<KisPageAllocationDescriptor> surfaceLayout;
    const KisPageAllocationDescriptor &descriptor() const
    { Q_ASSERT(layout || surfaceLayout); return layout ? *layout : *surfaceLayout; }
};

// Lives in the original mutation's stable cold page. The shard activity only
// borrows it while reserved; it is not a readable version until acceptance.
struct KisPagePrivateWrite {
    KisReplicaHandle target;
    KisReplicaHandle source;
    // Original target registration follows acquisition through the one fee
    // accept/cancel. The native reservation/session retains its lifetime;
    // ledger still checks exact ownership and takes its physical class claim.
    KisPageRetirementRecord *backing = nullptr;
    struct NativeProof {
        KisPageTransactionId transaction;
        quint64 providerValidationStamp = 0;
        bool isValid() const { return transaction.isValid() && providerValidationStamp; }
    } proof;
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
        using ExactIndex = KisShardSlotIndex<KisPageVersion, KisVersionSlotId>;
        using PhysicalIndex = KisShardSlotIndex<KisReplicaPhysicalSlotIdentity, KisReplicaSlotId>;
        void *shard = nullptr;
        void *page = nullptr;
        size_t shardIndex = 0;
        KisVersionSlotId value;
        ExactIndex::Reservation exact;
        PhysicalIndex::Reservation physical;
        Location() = default;
        Location(void *s, void *p, size_t i) : shard(s), page(p), shardIndex(i) {}
        // A copied private value is not the activity's registered result.
        // In particular it cannot retain a location after the original exits.
        Location(const Location &) noexcept {}
        Location &operator=(const Location &) noexcept
        { Q_ASSERT(!value.isValid()); shard = page = nullptr; shardIndex = 0; return *this; }
        Location(Location &&other) noexcept
            : shard(std::exchange(other.shard, nullptr)), page(std::exchange(other.page, nullptr)),
              shardIndex(std::exchange(other.shardIndex, 0)), value(std::exchange(other.value, {})),
              exact(std::move(other.exact)), physical(std::move(other.physical)) {}
        Location &operator=(Location &&other) noexcept
        { Q_ASSERT(!value.isValid());
          shard = std::exchange(other.shard, nullptr); page = std::exchange(other.page, nullptr);
          shardIndex = std::exchange(other.shardIndex, 0); value = std::exchange(other.value, {});
          exact = std::move(other.exact); physical = std::move(other.physical); return *this; }
    } location;
    // Set by metadata acceptance. This is a generational locator, not another
    // version authority; readers validate the record identity under its gate.
    KisVersionSlotId acceptedSlot;
    bool reserved = false;
    bool completed = false;
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
struct KisPageValueCell {
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
    KisPageTransactionId preparedBy;
    quint32 replicaCount = 0;
    KisPagePublicationState publication = KisPagePublicationState::Unpublished;
    // Layout, readiness and native read retention belong to this value. They
    // have no independent slot identity, allocation, or reservation lifetime.
    struct Native {
        std::shared_ptr<const KisPageAllocationDescriptor> descriptor;
        quint64 stamp = 0;
        KisPageTransactionId transaction;
        std::shared_ptr<KisCpuResidentBinding> nativeBinding;
        KisReplicaSlotId nativeReplica;
        quint32 nativeReaders = 0;
    } native;
};

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

static_assert(sizeof(KisPageValueCell) <= 192);
static_assert(sizeof(KisReplicaRecord) <= 128);
static_assert(sizeof(KisMetadataOverflowNode) <= 64);

template<>
struct KisSlotIdFor<KisPageValueCell> {
    using Type = KisVersionSlotId;
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

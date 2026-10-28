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
#include "KisPageViewValue_p.h"
#include <boost/container/small_vector.hpp>
#include <functional>

class KisCpuResidentBinding;
struct KisPageRetirementRecord;
struct KisPagePrivateWrite;

// Metadata memory belongs to the original edit's row storage. One hold per
// touched shard replaces per-row shared ownership. This is not another edit
// capability, transaction or retention edge; the rows still own their Values.
class KRITAIMAGE_EXPORT KisPageEditValues
{
public:
    static constexpr size_t ReleaseBatch = 16;
    KisPageEditValues() = default;
    ~KisPageEditValues();
    KisPageEditValues(KisPageEditValues &&) noexcept;
    KisPageEditValues &operator=(KisPageEditValues &&) noexcept;
    KisPageEditValues(const KisPageEditValues &) = delete;
    KisPageEditValues &operator=(const KisPageEditValues &) = delete;
    // Up to ReleaseBatch distinct, nonreserved rows from this edit. Caller
    // owns structural exclusion, just as during original-row construction.
    void release(KisPagePrivateWrite **rows, size_t count) noexcept;
    size_t storageBytes() const noexcept;
private:
    using Hold = std::shared_ptr<void>;
    using Holds = boost::container::small_vector<Hold, 4, KisMutationStorageAllocator<Hold>>;
    template<class Shard>
    void retain(const std::shared_ptr<Shard> &shard, const KisMutationStorageAllocator<char> &storage)
    {
        if (!m_shards) m_shards.emplace(Holds::allocator_type(storage));
        const auto before = [](const Hold &a, const void *b) { return std::less<const void *>{}(a.get(), b); };
        const auto at = std::lower_bound(m_shards->begin(), m_shards->end(), shard.get(), before);
        // Convert/copy shared ownership only when this edit first touches the
        // shard; even a temporary per-row shared_ptr would defeat that rule.
        if (at == m_shards->end() || at->get() != shard.get()) m_shards->insert(at, shard);
    }
    std::optional<Holds> m_shards;
    size_t m_values = 0;
    friend class KisPageMetadataCoordinator;
    friend class KisPageStoreReferenceTest;
};

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

struct KisPageReplicaState {
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
    KisReplicaHandle handle(const KisPageVersion &version) const
    {
        KisReplicaHandle result;
        result.provider = identity.provider; result.providerEpoch = identity.providerEpoch;
        result.allocation = identity.allocation; result.version = version; result.domain = domain;
        result.layout.layoutRevision = layoutRevision; result.layout.formatId = formatId;
        result.layout.byteSize = byteSize; result.layout.validRect = validRect;
        result.layout.pageExtent = pageExtent; result.layout.rowStride = rowStride;
        return result;
    }
    void setHandle(const KisReplicaHandle &handle)
    {
        identity = handle.allocationIdentity(); domain = handle.domain;
        layoutRevision = handle.layout.layoutRevision; formatId = handle.layout.formatId;
        byteSize = handle.layout.byteSize; validRect = handle.layout.validRect;
        pageExtent = handle.layout.pageExtent; rowStride = handle.layout.rowStride;
    }

};
// Arena generations occupy 16 bits. The high bit identifies the primary
// replica stored in its Value, without a second allocation or slot lifetime.
inline constexpr quint32 KisInlineReplicaGeneration = quint32(1) << 31;
inline bool kisIsInlineReplica(KisReplicaSlotId slot)
{ return slot.generation & KisInlineReplicaGeneration; }
inline KisReplicaSlotId kisInlineReplicaSlot(KisVersionSlotId value)
{ return {value.index, value.generation | KisInlineReplicaGeneration}; }
inline KisVersionSlotId kisInlineReplicaOwner(KisReplicaSlotId replica)
{ return {replica.index, replica.generation & ~KisInlineReplicaGeneration}; }

/**
 * Compact authoritative records. Public/reference snapshots are projections
 * of these records and never own production metadata. A Value stores its
 * primary placement directly; only actual additional replicas own cold slots.
 * The original edit and accepted views share this same generational Value.
 */
struct KisPageValueState {
    KisPageReplicaState primaryReplica;
    KisVersionSlotId previousVersion;
    KisVersionSlotId nextVersion;
    KisVersionSlotId previousHistory;
    KisVersionSlotId nextHistory;
    KisVersionSlotId nextMutable;
    KisReplicaSlotId firstReplica;
    KisReplicaSlotId authorityReplica;
    KisPageTransactionId preparedBy;
    quint32 replicaCount = 0;
    KisPagePublicationState publication = KisPagePublicationState::Unpublished;
    // The original edit holds memory, not a history/read protection edge.
    // Removing a view may detach the value before its Frame finishes delivery.
    bool frameHeld = false;
    bool indexed = false;
    bool initialIdentity = false;
    // Layout and readiness belong to this value. They
    // have no independent slot identity, allocation, or reservation lifetime.
    struct Native {
        std::shared_ptr<const KisPageAllocationDescriptor> descriptor;
        // Sealing names the original producer, not an exported capability.
        // Internal consumers identify this Value by its checked arena slot.
        KisPageTransactionId sealedBy;
        // Only an actual public export assigns this stamp. Revocation clears
        // both the producer permission and its optional external capability.
        quint64 exportedStamp = 0;
        std::shared_ptr<KisCpuResidentBinding> nativeBinding;
        KisReplicaSlotId nativeReplica;
    } native;
};

struct KisPageValueCell : KisPageViewValue, KisPageValueState {};


// The stable original edit row borrows its one final Value. It never owns a
// second target/layout/binding/proof and cannot be copied into another writer.
struct KisPagePrivateWrite {
    explicit KisPagePrivateWrite(KisPageEditValues &edit) : edit(edit) {}
    KRITAIMAGE_EXPORT ~KisPagePrivateWrite();
    KisPagePrivateWrite(const KisPagePrivateWrite &) = delete;
    KisPagePrivateWrite &operator=(const KisPagePrivateWrite &) = delete;
    KisPageEditValues &edit;
    KisPageValueCell *value = nullptr;
    KisReplicaHandle source;
    KisPageRetirementRecord *backing = nullptr;
    bool hasValue() const { return value != nullptr; }
    const KisPageVersion &version() const { Q_ASSERT(value); return value->version; }
    KisReplicaHandle target() const { return value ? value->primaryReplica.handle(value->version) : KisReplicaHandle{}; }
    auto &descriptor() { Q_ASSERT(value); return value->native.descriptor; }
    const auto &descriptor() const { Q_ASSERT(value); return value->native.descriptor; }
    auto &binding() { Q_ASSERT(value); return value->native.nativeBinding; }
    const auto &binding() const { Q_ASSERT(value); return value->native.nativeBinding; }
    bool isSealed() const { return value && value->native.sealedBy.isValid(); }
    KisPageTransactionId sealedBy() const { return value ? value->native.sealedBy : KisPageTransactionId{}; }
    KisVersionSlotId valueSlot() const { return value ? value->slot : KisVersionSlotId{}; }
    KisVersionSlotId acceptedSlot() const { return value && value->indexed ? value->slot : KisVersionSlotId{}; }
    bool matchesProof(const KisPreparedPageProof &candidate, const KisCompletionTicket &completion) const
    {
        return isSealed() && sealedBy() == candidate.transaction
            && value->native.exportedStamp && value->native.exportedStamp == candidate.providerValidationStamp
            && target() == candidate.authority && completion == candidate.producerCompletion;
    }
    struct Location {
        using ExactIndex = KisShardSlotIndex<KisPageVersion, KisVersionSlotId>;
        using PhysicalIndex = KisShardSlotIndex<KisReplicaPhysicalSlotIdentity, KisReplicaSlotId>;
        void *shard = nullptr; // arena lifetime belongs to the original edit
        void *page = nullptr;
        size_t shardIndex = 0;
        KisVersionSlotId value;
        ExactIndex::Reservation exact;
        PhysicalIndex::Reservation physical;
    } location;
    // Real displaced-value destruction leaves the shard/acceptance gates with
    // the original row. These are terminal references, never the new value.
    std::shared_ptr<const KisPageAllocationDescriptor> releasedDescriptor;
    std::shared_ptr<KisCpuResidentBinding> releasedBinding;
    bool reserved = false;
    bool completed = false;
};

enum class KisMetadataOverflowKind : quint8 {
    ReadLease,
    PendingLastUse
};

struct KisMetadataOverflowNode {
    KisMetadataOverflowSlotId next;
    KisCompletionTicket completion;
    quint64 value = 0;
    KisMetadataOverflowKind kind = KisMetadataOverflowKind::ReadLease;
};

// Identity lifetime facts live on the original Value; no per-capture token chain.
static_assert(sizeof(KisPageValueCell) <= 328);
static_assert(sizeof(KisPageReplicaState) <= 128);
static_assert(sizeof(KisMetadataOverflowNode) <= 64);

template<>
struct KisSlotIdFor<KisPageValueCell> {
    using Type = KisVersionSlotId;
};

template<>
struct KisSlotIdFor<KisPageReplicaState> {
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

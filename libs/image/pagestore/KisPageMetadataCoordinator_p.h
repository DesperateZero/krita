/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_METADATA_COORDINATOR_P_H
#define KIS_PAGE_METADATA_COORDINATOR_P_H

#include "KisCpuResidentBinding_p.h"
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
    KisVersionSlotId value;
};

// One selected input, retained by the original operation through comparison,
// resource checkout and acquisition. Layout is either the selected immutable
// version's layout or the actual surface value for a default/semantic input.
// Facts for one actual write. Read leases, completion lists, root epochs and
// transition kinds are not part of native acquisition. Only a real generic
// lease boundary projects the reference-model transition below.
struct KisPageWriteInput {
    KisPageWriteSource source;
    KisPageVersion version;
    KisPageTransactionId transaction;
    KisPageWriteMode writeMode = KisPageWriteMode::PreserveContents;
    bool baseWasPrepared = false;
    KisPageWriteInput() = default;
    explicit KisPageWriteInput(const KisPageTransition &value)
        : source(value.source), version(value.version), transaction(value.transaction), writeMode(value.writeMode)
    { source.version = value.baseVersion; }
};

struct KisPageWriteSelection {
    KisPageWriteInput write;
    KisPageMutationBaseLocation base;
    std::optional<KisPageWriteSource> recoverableBefore;
    std::shared_ptr<const KisPageAllocationDescriptor> layout;
    std::optional<KisPageAllocationDescriptor> surfaceLayout;
    struct Lease { KisPageOperationId operation; KisPageWriterToken writer; };
    std::optional<Lease> lease;
    const KisPageAllocationDescriptor &descriptor() const
    { Q_ASSERT(layout || surfaceLayout); return layout ? *layout : *surfaceLayout; }
    KisPageTransition leaseTransition(const KisReplicaHandle &target,
        KisPageTransitionKind kind = KisPageTransitionKind::AcquireWrite) const
    {
        Q_ASSERT(lease);
        KisPageTransition value;
        value.kind = kind; value.baseVersion = write.source.version; value.version = write.version;
        value.source = write.source.handle(); value.target = target; value.operation = lease->operation;
        value.writer = lease->writer; value.transaction = write.transaction; value.writeMode = write.writeMode;
        return value;
    }
};

struct KisPageReplicaState {
    KisVersionSlotId ownerVersion;
    KisReplicaSlotId nextReplica;
    KisMetadataOverflowSlotId overflow;
    KisPageOperationId activeOperation;
    quint32 pinCount = 0;
    KisReplicaValidity validity = KisReplicaValidity::Allocated;

    // A value-initialized arena record must not zero the inactive external
    // snapshot. Copy/reset likewise touches only its actually active member.
    KisPageReplicaState() noexcept {}
    KisPageReplicaState(const KisPageReplicaState &other) noexcept { *this = other; }
    KisPageReplicaState &operator=(const KisPageReplicaState &other) noexcept
    {
        if (this == &other) return *this;
        ownerVersion = other.ownerVersion; nextReplica = other.nextReplica;
        overflow = other.overflow; activeOperation = other.activeOperation;
        pinCount = other.pinCount; validity = other.validity;
        m_domain = other.m_domain; m_kind = other.m_kind;
        if (m_kind == Kind::Resource) new (&m_physical.resource) Resource(other.m_physical.resource);
        else if (m_kind == Kind::External) new (&m_physical.external) External(other.m_physical.external);
        return *this;
    }
    // Immutable resource identity, borrowed only while the owning Value is held.
    // It proves no mutable access right or accepted incarnation by itself.
    const KisCpuResidentBinding *nativeResource() const noexcept
    { return m_kind == Kind::Resource ? m_physical.resource.binding : nullptr; }

    KisReplicaAllocationIdentity identity() const
    {
        if (m_kind == Kind::Resource) {
            const auto &r = m_physical.resource;
            const auto &slot = r.binding->m_slot;
            return {slot.provider, slot.providerEpoch, {slot.slot, r.incarnation}};
        }
        return m_kind == Kind::External ? m_physical.external.identity : KisReplicaAllocationIdentity{};
    }

    KisPageAccessDomain domain() const
    {
        return m_kind == Kind::Resource ? m_physical.resource.binding->m_domain : m_domain;
    }

    const KisReplicaLayout &layout() const
    {
        static const KisReplicaLayout empty;
        if (m_kind == Kind::Resource) return m_physical.resource.binding->m_layout;
        return m_kind == Kind::External ? m_physical.external.layout : empty;
    }

    KisReplicaHandle handle(const KisPageVersion &version) const
    {
        const auto allocation = identity();
        return {allocation.provider, allocation.providerEpoch, allocation.allocation, version, domain(), layout()};
    }

    void setHandle(const KisReplicaHandle &handle, const KisCpuResidentBinding *resource = nullptr)
    {
        // Only immutable facts are consumed here, under metadata admission, never
        // by acquiring R under V. The expected incarnation belongs to this Value;
        // a later handoff cannot silently change an old Value's public identity.
        if (resource && resource->m_key == handle.version.key
            && resource->m_slot == handle.physicalSlotIdentity()
            && resource->m_domain == handle.domain && resource->m_layout == handle.layout) {
            new (&m_physical.resource) Resource{resource, handle.allocation.generation};
            m_kind = Kind::Resource;
        } else {
            new (&m_physical.external) External{handle.allocationIdentity(), handle.layout};
            m_domain = handle.domain;
            m_kind = Kind::External;
        }
    }

    void writeSourceTo(KisPageWriteSource &result, const KisPageVersion &version,
        const std::shared_ptr<KisCpuResidentBinding> &binding) const
    {
        if (m_kind == Kind::Resource && binding.get() == m_physical.resource.binding)
            result.setNative(version, binding, m_physical.resource.incarnation);
        else result = KisPageWriteSource(handle(version));
    }
    bool matchesSource(const KisPageWriteSource &source, const KisPageVersion &version) const
    {
        if (m_kind == Kind::Resource && source.binding())
            return source.matchesNative(m_physical.resource.binding,
                m_physical.resource.incarnation, version);
        return source.matches(identity(), domain(), layout(), version);
    }

    void detachResource(const KisPageVersion &version)
    {
        if (m_kind == Kind::Resource) setHandle(handle(version));
    }

private:
    enum class Kind : quint8 { Empty, External, Resource };
    Kind m_kind = Kind::Empty;
    KisPageAccessDomain m_domain = KisPageAccessDomain::Unknown;
    struct External { KisReplicaAllocationIdentity identity; KisReplicaLayout layout; };
    struct Resource { const KisCpuResidentBinding *binding; quint64 incarnation; };
    union Physical {
        Physical() {} // Empty/native records do not initialize an external snapshot.
        External external;
        Resource resource;
    } m_physical;
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
    // Explicit default initialization preserves the inactive replica storage
    // when resetting this mutable part of an already retained Value.
    KisPageValueState() noexcept {}
    KisPageReplicaState primaryReplica;
    KisVersionSlotId previousVersion;
    KisVersionSlotId nextVersion;
    KisVersionSlotId nextMutable;
    KisReplicaSlotId firstReplica;
    KisReplicaSlotId authorityReplica;
    KisPageTransactionId preparedBy;
    quint32 replicaCount = 0;
    // Historical denotes an accepted content value, irrespective of current
    // root selection. Published exists only in a requested view projection.
    KisPagePublicationState publication = KisPagePublicationState::Unpublished;
    // The original edit holds memory, not a history/read protection edge.
    // Removing a view may detach the value before its Frame finishes delivery.
    bool frameHeld = false;
    enum class Residence : quint8 { Private, Head, Indexed, Detached };
    Residence residence = Residence::Private;
    bool hasContent() const noexcept
    { return residence == Residence::Head || residence == Residence::Indexed; }
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

struct KisPageValueCell : KisPageViewValue, KisPageValueState {
    KisPageValueCell() noexcept {} // Arena value initialization must also skip inactive storage.
};

// Borrowed only for one synchronous acceptance under the Store owner gate.
// Original Frame/root/candidate and deferred releases retain these objects.
struct KisPageCpuPublicationResource {
    KisCpuResidentBinding *binding = nullptr;
    const KisPageValueCell *value = nullptr;
    const KisPageReplicaState *replica = nullptr;
};


template<>
struct KisSlotIdFor<KisPageReplicaState> {
    using Type = KisReplicaSlotId;
};

// Generic placement names an arena replica. Native placement names the
// original paid retirement record, whose association changes without rekeying.
// Neither form owns pixels or a second Resource state machine.
struct KisMetadataPhysicalPosition {
    KisReplicaSlotId replica;
    KisPageRetirementRecord *native = nullptr;
    bool isValid() const { return native || replica.isValid(); }
    bool operator==(const KisMetadataPhysicalPosition &other) const
    { return native == other.native && replica == other.replica; }
};

// The stable original edit row borrows its one final Value. It never owns a
// second target/layout/binding/proof and cannot be copied into another writer.
struct KisPagePrivateWrite {
    explicit KisPagePrivateWrite(KisPageEditValues &edit) : edit(edit) {}
    KRITAIMAGE_EXPORT ~KisPagePrivateWrite();
    KisPagePrivateWrite(const KisPagePrivateWrite &) = delete;
    KisPagePrivateWrite &operator=(const KisPagePrivateWrite &) = delete;
    KisPageEditValues &edit;
    KisPageValueCell *value = nullptr;
    KisPageRetirementRecord *backing = nullptr;
    bool hasValue() const { return value != nullptr; }
    const KisPageVersion &version() const { Q_ASSERT(value); return value->version; }
    KisReplicaHandle target() const { return value ? value->primaryReplica.handle(value->version) : KisReplicaHandle{}; }
    // Layout is borrowed from the original physical state. Internal pixel
    // access does not need to materialize a complete public identity handle.
    const KisReplicaLayout &targetLayout() const { Q_ASSERT(value); return value->primaryReplica.layout(); }
    bool setDescriptor(std::shared_ptr<const KisPageAllocationDescriptor> descriptor)
    {
        Q_ASSERT(value);
        if (value->native.descriptor || !descriptor || !descriptor->isValid()) return false;
        value->native.descriptor = std::move(descriptor);
        m_descriptorAdmitted = true;
        return true;
    }
    void exchangeDescriptor(std::shared_ptr<const KisPageAllocationDescriptor> &replacement)
    {
        Q_ASSERT(value);
        m_descriptorAdmitted = replacement && replacement->isValid();
        value->native.descriptor.swap(replacement);
    }
    KisValidatedPageDescriptor descriptorAdmission() const
    {
        return m_descriptorAdmitted && value && value->native.descriptor
            ? KisValidatedPageDescriptor(value->native.descriptor.get()) : KisValidatedPageDescriptor{};
    }
    const auto &descriptor() const { Q_ASSERT(value); return value->native.descriptor; }
    const auto &binding() const { Q_ASSERT(value); return value->native.nativeBinding; }
    KRITAIMAGE_EXPORT std::shared_ptr<KisCpuResidentBinding> exchangeBinding(
        std::shared_ptr<KisCpuResidentBinding> next, const KisReplicaHandle &target);
    bool isSealed() const { return value && value->native.sealedBy.isValid(); }
    KisPageTransactionId sealedBy() const { return value ? value->native.sealedBy : KisPageTransactionId{}; }
    KisVersionSlotId valueSlot() const { return value ? value->slot : KisVersionSlotId{}; }
    KisVersionSlotId acceptedSlot() const { return value && value->hasContent() ? value->slot : KisVersionSlotId{}; }
    bool matchesProof(const KisPreparedPageProof &candidate, const KisCompletionTicket &completion) const
    {
        return isSealed() && sealedBy() == candidate.transaction
            && value->native.exportedStamp && value->native.exportedStamp == candidate.providerValidationStamp
            && target() == candidate.authority && completion == candidate.producerCompletion;
    }
    struct Location {
        using ExactIndex = KisShardSlotIndex<KisPageVersion, KisVersionSlotId>;
        using PhysicalIndex = KisShardSlotIndex<KisReplicaPhysicalSlotIdentity, KisMetadataPhysicalPosition>;
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
private:
    bool m_descriptorAdmitted = false;
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
static_assert(sizeof(KisPageValueCell) <= 312);
static_assert(sizeof(KisPageReplicaState) <= 128);
static_assert(sizeof(KisMetadataOverflowNode) <= 64);

template<>
struct KisSlotIdFor<KisPageValueCell> {
    using Type = KisVersionSlotId;
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

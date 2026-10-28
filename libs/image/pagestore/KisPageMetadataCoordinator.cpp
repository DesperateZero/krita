/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageMetadataCoordinator.h"
#include "KisCpuResidentBinding_p.h"
#include "KisDeferredMetadataCleanup_p.h"
#include "KisPageMetadataCoordinator_p.h"
#include "KisPagePublicationInput_p.h"
#include "KisPageStoreReclamation_p.h"
#include "KisPageWriteCoordinator_p.h"
#include "KisPageStateMachine_p.h"
#include "KisPageCommitPolicy_p.h"

#include <QAtomicInteger>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <limits>
#include <memory>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace KisPageStoreDetail {
class MetadataBudgetAuthority final
{
public:
    explicit MetadataBudgetAuthority(KisBackingBudgetController *budget)
        : m_budget(budget), m_storageOwner(kisMutationStorageOwner(budget)) {}

    void detach() noexcept
    {
        QMutexLocker locker(&m_mutex);
        m_budget = nullptr;
    }

    KisBackingBudgetReservation reserve(const KisBackingBudgetDelta &delta,
                                        QString *error)
    {
        QMutexLocker locker(&m_mutex);
        if (!m_budget) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("metadata backing budget is unavailable"));
            return {};
        }
        return m_budget->reserve(delta, error);
    }

    template<class T>
    KisMutationStorageAllocator<T> storage()
    {
        QMutexLocker locker(&m_mutex);
        if (!m_budget) throw std::bad_alloc();
        return KisMutationStorageAllocator<T>::retained(m_budget);
    }

    void commitReservation(KisBackingBudgetReservation &&reservation,
                           const KisBackingBudgetDelta &installed) noexcept
    {
        QMutexLocker locker(&m_mutex);
        Q_ASSERT(m_budget);
        if (m_budget) {
            m_budget->commitReservation(std::move(reservation), installed);
            const qint64 bytes = installed.bucket(size_t(KisBackingBudgetClass::MetadataArena)).cpuRam;
            Q_ASSERT(bytes >= 0 && m_storageOwner);
            m_storageOwner->retainLiveCharge(quint64(bytes));
        }
    }

    void releaseLive(quint64 bytes) noexcept
    {
        QMutexLocker locker(&m_mutex);
        Q_ASSERT(m_storageOwner);
        m_storageOwner->releaseLiveCharge(bytes);
    }

private:
    QMutex m_mutex;
    KisBackingBudgetController *m_budget = nullptr;
    boost::intrusive_ptr<KisMutationStorageOwner> m_storageOwner;
};
}

namespace
{

using MetadataBudgetAuthority = KisPageStoreDetail::MetadataBudgetAuthority;
using PhysicalSlot = KisReplicaPhysicalSlotIdentity;

KisReplicaHandle replicaHandle(const KisReplicaRecord &record, const KisPageVersion &version)
{
    KisReplicaHandle result;
    result.provider = record.identity.provider;
    result.providerEpoch = record.identity.providerEpoch;
    result.allocation = record.identity.allocation;
    result.version = version;
    result.domain = record.domain;
    result.layout.layoutRevision = record.layoutRevision;
    result.layout.formatId = record.formatId;
    result.layout.pageExtent = record.pageExtent;
    result.layout.validRect = record.validRect;
    result.layout.rowStride = record.rowStride;
    result.layout.byteSize = record.byteSize;
    return result;
}

using VersionArena = KisShardSlotArena<KisVersionRecord, 16 * 1024>;
// Few concurrently live versions per shard: a 16 KiB minimum here would
// replace cheap nodes with mostly empty capacity. Growth remains prepaid.
using VersionFactsArena = KisShardSlotArena<KisVersionFactsRecord, 512>;
using ReplicaArena = KisShardSlotArena<KisReplicaRecord, 32 * 1024>;
using OverflowArena = KisShardSlotArena<KisMetadataOverflowNode, 16 * 1024>;


struct MetadataBudgetRelease {
    std::shared_ptr<MetadataBudgetAuthority> authority;
    quint64 bytes = 0;

    MetadataBudgetRelease() = default;
    MetadataBudgetRelease(std::shared_ptr<MetadataBudgetAuthority> authority,
                          quint64 bytes)
        : authority(std::move(authority)), bytes(bytes) {}
    MetadataBudgetRelease(MetadataBudgetRelease &&other) noexcept
        : authority(std::move(other.authority))
        , bytes(std::exchange(other.bytes, 0)) {}
    MetadataBudgetRelease &operator=(MetadataBudgetRelease &&other) noexcept
    {
        if (this != &other) {
            release();
            authority = std::move(other.authority);
            bytes = std::exchange(other.bytes, 0);
        }
        return *this;
    }
    MetadataBudgetRelease(const MetadataBudgetRelease &) = delete;
    MetadataBudgetRelease &operator=(const MetadataBudgetRelease &) = delete;
    ~MetadataBudgetRelease() { release(); }

    void add(quint64 addition)
    {
        Q_ASSERT(addition <= std::numeric_limits<quint64>::max() - bytes);
        bytes += addition;
    }

    MetadataBudgetRelease take(quint64 amount)
    {
        Q_ASSERT(amount <= bytes);
        bytes -= amount;
        return {authority, amount};
    }

    void release() noexcept
    {
        if (authority && bytes)
            authority->releaseLive(bytes);
        authority.reset();
        bytes = 0;
    }
};

struct MetadataArenas {
    // These are hard ceilings, not reservations. Directory and block storage
    // are prepared lazily outside the shard gate by the same growth carrier.
    static constexpr quint64 VersionBudget = 4 * 1024 * 1024;
    static constexpr quint64 ReplicaBudget = 8 * 1024 * 1024;
    static constexpr quint64 OverflowBudget = 4 * 1024 * 1024;

    explicit MetadataArenas(KisMutationStorageAllocator<char> storage)
        : versions(VersionBudget, storage), facts(VersionBudget, storage), replicas(ReplicaBudget, storage)
        , overflow(OverflowBudget, storage) {}

    VersionArena versions;
    VersionFactsArena facts;
    qsizetype cpuProofs = 0;
    quint64 cpuReadBindings = 0;
    ReplicaArena replicas;
    OverflowArena overflow;

    struct ReleasedBlocks {
        // Members are destroyed in reverse order: payloads first, accounting
        // second, so usage never understates memory that is still allocated.
        MetadataBudgetRelease budgetRelease;
        VersionArena::ReleasedBlocks versions;
        VersionFactsArena::ReleasedBlocks facts;
        ReplicaArena::ReleasedBlocks replicas;
        OverflowArena::ReleasedBlocks overflow;

        ReleasedBlocks() = default;
        ReleasedBlocks(ReleasedBlocks &&) noexcept = default;
        ReleasedBlocks &operator=(ReleasedBlocks &&other) noexcept
        {
            if (this == &other)
                return *this;
            versions = {};
            facts = {};
            replicas = {};
            overflow = {};
            budgetRelease.release();
            versions = std::move(other.versions);
            facts = std::move(other.facts);
            replicas = std::move(other.replicas);
            overflow = std::move(other.overflow);
            budgetRelease = std::move(other.budgetRelease);
            return *this;
        }
        ReleasedBlocks(const ReleasedBlocks &) = delete;
        ReleasedBlocks &operator=(const ReleasedBlocks &) = delete;
    };

    ReleasedBlocks takeEmptyBlocks(MetadataBudgetRelease *budgetCharge,
                                   quint64 minimumBlocksToKeep = 1)
    {
        ReleasedBlocks result;
        result.versions = versions.takeEmptyBlocks(minimumBlocksToKeep);
        result.facts = facts.takeEmptyBlocks(minimumBlocksToKeep);
        result.replicas = replicas.takeEmptyBlocks(minimumBlocksToKeep);
        result.overflow = overflow.takeEmptyBlocks(minimumBlocksToKeep);
        const quint64 bytes = result.versions.byteSize()
            + result.facts.byteSize() + result.replicas.byteSize() + result.overflow.byteSize();
        if (bytes) {
            Q_ASSERT(budgetCharge);
            result.budgetRelease = budgetCharge->take(bytes);
        }
        return result;
    }
};

struct MetadataArenaDemand {
    quint64 versions = 0;
    quint64 replicas = 0;
    quint64 overflow = 0;
};

struct MetadataArenaReservations {
    VersionArena::Reservation versions;
    VersionFactsArena::Reservation facts;
    ReplicaArena::Reservation replicas;
    OverflowArena::Reservation overflow;

    bool reserve(MetadataArenas *arenas, const MetadataArenaDemand &demand)
    {
        Q_ASSERT(arenas);
        if (demand.versions > 0) {
            versions = arenas->versions.reserveSlots(qsizetype(demand.versions));
            if (!versions.isValid())
                return false;
            facts = arenas->facts.reserveSlots(qsizetype(demand.versions));
            if (!facts.isValid()) {
                cancel(arenas);
                return false;
            }
        }
        if (demand.replicas > 0) {
            replicas = arenas->replicas.reserveSlots(qsizetype(demand.replicas));
            if (!replicas.isValid()) {
                cancel(arenas);
                return false;
            }
        }
        if (demand.overflow > 0) {
            overflow = arenas->overflow.reserveSlots(qsizetype(demand.overflow));
            if (!overflow.isValid()) {
                cancel(arenas);
                return false;
            }
        }
        return true;
    }

    void cancel(MetadataArenas *arenas)
    {
        if (!arenas)
            return;
        arenas->versions.cancelReservation(&versions);
        arenas->facts.cancelReservation(&facts);
        arenas->replicas.cancelReservation(&replicas);
        arenas->overflow.cancelReservation(&overflow);
    }
};

struct MetadataArenaGrowthPlan {
    quint64 versionBlocks = 0;
    quint64 replicaBlocks = 0;
    quint64 overflowBlocks = 0;
    quint64 versionDirectory = 0;
    quint64 replicaDirectory = 0;
    quint64 overflowDirectory = 0;
    quint64 factsBlocks = 0;
    quint64 factsDirectory = 0;

    bool isEmpty() const
    {
        return versionBlocks == 0 && replicaBlocks == 0 && overflowBlocks == 0 && factsBlocks == 0;
    }
};

template<class Arena>
quint64 arenaBlocksRequired(const Arena &arena, quint64 slots)
{
    const quint64 freeSlots = arena.availableSlots();
    if (freeSlots >= slots)
        return 0;
    const quint64 missing = slots - freeSlots;
    return (missing + Arena::slotsPerBlock() - 1) / Arena::slotsPerBlock();
}

MetadataArenaGrowthPlan metadataArenaGrowthPlan(const MetadataArenas &arenas,
                                               const MetadataArenaDemand &demand)
{
    MetadataArenaGrowthPlan plan{arenaBlocksRequired(arenas.versions, demand.versions),
        arenaBlocksRequired(arenas.replicas, demand.replicas),
        arenaBlocksRequired(arenas.overflow, demand.overflow)};
    plan.versionDirectory = arenas.versions.directoryCapacityForBlocks(plan.versionBlocks);
    plan.replicaDirectory = arenas.replicas.directoryCapacityForBlocks(plan.replicaBlocks);
    plan.overflowDirectory = arenas.overflow.directoryCapacityForBlocks(plan.overflowBlocks);
    plan.factsBlocks = arenaBlocksRequired(arenas.facts, demand.versions);
    plan.factsDirectory = arenas.facts.directoryCapacityForBlocks(plan.factsBlocks);
    return plan;
}

bool metadataArenaGrowthFitsBudget(const MetadataArenas &arenas, const MetadataArenaGrowthPlan &plan)
{
    return arenas.versions.canAttachBlocks(plan.versionBlocks) && arenas.replicas.canAttachBlocks(plan.replicaBlocks)
        && arenas.overflow.canAttachBlocks(plan.overflowBlocks) && arenas.facts.canAttachBlocks(plan.factsBlocks);
}

bool metadataArenasHaveCapacity(const MetadataArenas &arenas, const MetadataArenaDemand &demand)
{
    return arenas.versions.availableSlots() >= demand.versions && arenas.replicas.availableSlots() >= demand.replicas
        && arenas.overflow.availableSlots() >= demand.overflow && arenas.facts.availableSlots() >= demand.versions;
}

/**
 * Carrier for block payloads allocated outside the shard lock.
 *
 * By itself this is not PreparedPublication: it owns no reserved slots,
 * index capacity, semantic delta, root or publication right. The prepared
 * commit embeds it, attaches only the needed blocks during prepare, and then
 * couples the resulting capacity to explicit slot/index reservations.
 */
struct MetadataArenaGrowth {
    explicit MetadataArenaGrowth(KisPageWorkingStorage storage)
        : versions(storage), facts(storage), replicas(storage), overflow(storage)
        , versionDirectory(storage), factsDirectory(storage), replicaDirectory(storage), overflowDirectory(storage) {}
    // Candidate payloads are destroyed before the reservation that accounts
    // for them when preparation exits early.
    KisBackingBudgetReservation budgetReservation;
    KisPageWorkingArray<VersionArena::PreparedBlock> versions;
    KisPageWorkingArray<VersionFactsArena::PreparedBlock> facts;
    KisPageWorkingArray<ReplicaArena::PreparedBlock> replicas;
    KisPageWorkingArray<OverflowArena::PreparedBlock> overflow;
    VersionArena::PreparedDirectory versionDirectory;
    VersionFactsArena::PreparedDirectory factsDirectory;
    ReplicaArena::PreparedDirectory replicaDirectory;
    OverflowArena::PreparedDirectory overflowDirectory;
    quint64 attachedBlocks = 0;

    quint64 preparedBlockCount() const
    {
        return quint64(versions.size()) + quint64(facts.size()) + quint64(replicas.size()) + quint64(overflow.size());
    }

    template<class Arena, class Blocks>
    static bool prepareBlocks(Blocks *target, quint64 count)
    {
        Q_ASSERT(target);
        try {
            target->reserve(target->size() + size_t(count));
        } catch (const std::bad_alloc &) {
            return false;
        }
        for (quint64 i = 0; i < count; ++i) {
            auto candidate = Arena::prepareBlock();
            if (!candidate.isValid())
                return false;
            target->push_back(std::move(candidate));
        }
        return true;
    }

    static quint64 blockBytes(const MetadataArenaGrowthPlan &plan)
    {
        return plan.versionBlocks * VersionArena::blockByteSize()
            + plan.replicaBlocks * ReplicaArena::blockByteSize()
            + plan.overflowBlocks * OverflowArena::blockByteSize()
            + plan.factsBlocks * VersionFactsArena::blockByteSize();
    }

    bool prepare(const MetadataArenaGrowthPlan &plan,
                 const std::shared_ptr<MetadataBudgetAuthority> &authority,
                 QString *error)
    {
        if (!authority || budgetReservation.isValid()) {
            KisPageStoreDetail::setError(error, QStringLiteral("metadata backing budget is unavailable"));
            return false;
        }
        KisBackingBudgetDelta delta;
        delta.bucket(size_t(KisBackingBudgetClass::MetadataArena)).cpuRam =
            qint64(blockBytes(plan));
        budgetReservation = authority->reserve(delta, error);
        if (!budgetReservation.isValid())
            return false;
        try {
            versionDirectory.prepare(plan.versionDirectory);
            factsDirectory.prepare(plan.factsDirectory);
            replicaDirectory.prepare(plan.replicaDirectory);
            overflowDirectory.prepare(plan.overflowDirectory);
        } catch (const std::bad_alloc &) {
            return false;
        }
        return prepareBlocks<VersionArena>(&versions, plan.versionBlocks)
            && prepareBlocks<VersionFactsArena>(&facts, plan.factsBlocks)
            && prepareBlocks<ReplicaArena>(&replicas, plan.replicaBlocks)
            && prepareBlocks<OverflowArena>(&overflow, plan.overflowBlocks);
    }

    enum class AttachResult {
        Ready,
        NeedsMore,
        Rejected
    };

    template<class Arena, class Blocks>
    static AttachResult attachBlocks(Arena *arena,
                                     quint64 slots,
                                     Blocks *candidates,
                                     typename Arena::PreparedDirectory *directory,
                                     quint64 *attachedBlocks)
    {
        Q_ASSERT(arena);
        Q_ASSERT(candidates);
        if (!arena->installPreparedDirectory(directory, arenaBlocksRequired(*arena, slots)))
            return AttachResult::NeedsMore;
        for (auto &candidate : *candidates) {
            if (arena->availableSlots() >= slots)
                break;
            if (!candidate.isValid())
                continue;
            if (!arena->attachPreparedBlock(&candidate)) {
                return AttachResult::Rejected;
            }
            ++*attachedBlocks;
        }
        return arena->availableSlots() >= slots ? AttachResult::Ready : AttachResult::NeedsMore;
    }

    AttachResult attach(MetadataArenas *arenas,
                        MetadataBudgetRelease *budgetCharge,
                        const MetadataArenaDemand &demand)
    {
        const quint64 versionBytesBefore =
            arenas->versions.statistics().allocatedBytes;
        const quint64 factsBytesBefore = arenas->facts.statistics().allocatedBytes;
        const quint64 replicaBytesBefore =
            arenas->replicas.statistics().allocatedBytes;
        const quint64 overflowBytesBefore =
            arenas->overflow.statistics().allocatedBytes;
        const auto versionResult = attachBlocks(&arenas->versions, demand.versions, &versions,
                                               &versionDirectory, &attachedBlocks);
        auto result = versionResult;
        if (result == AttachResult::Ready) {
            result = attachBlocks(&arenas->facts, demand.versions,
                                  &facts, &factsDirectory, &attachedBlocks);
        }
        if (result == AttachResult::Ready) {
            result = attachBlocks(&arenas->replicas, demand.replicas,
                                  &replicas, &replicaDirectory, &attachedBlocks);
        }
        if (result == AttachResult::Ready) {
            result = attachBlocks(&arenas->overflow, demand.overflow,
                                  &overflow, &overflowDirectory, &attachedBlocks);
        }
        // Destroy unattached candidates before releasing their reservation.
        versions.clear();
        facts.clear();
        replicas.clear();
        overflow.clear();
        if (budgetReservation.isValid()) {
            KisBackingBudgetDelta installed;
            const quint64 attachedBytes =
                arenas->versions.statistics().allocatedBytes - versionBytesBefore
                + arenas->facts.statistics().allocatedBytes - factsBytesBefore
                + arenas->replicas.statistics().allocatedBytes - replicaBytesBefore
                + arenas->overflow.statistics().allocatedBytes - overflowBytesBefore;
            installed.bucket(size_t(KisBackingBudgetClass::MetadataArena)).cpuRam =
                qint64(attachedBytes);
            Q_ASSERT(budgetCharge && budgetCharge->authority);
            budgetCharge->authority->commitReservation(
                std::move(budgetReservation), installed);
            if (attachedBytes) {
                Q_ASSERT(budgetCharge);
                budgetCharge->add(attachedBytes);
            }
        }
        return result;
    }
};

template<class Snapshot>
quint64 overflowCount(const Snapshot &snapshot)
{
    quint64 result = quint64(snapshot.capturedReadViews.size());
    for (const auto &replica : snapshot.replicas) {
        result += quint64(replica.readLeases.size());
        result += quint64(replica.pendingLastUses.size());
    }
    return result;
}

struct MetadataGenericActivity {
    KisPageWriterToken writerToken;
    KisPageOperationId writerOperation;
    KisVersionSlotId writerBaseVersion;
    KisReplicaSlotId writerBaseAuthority;
    KisReplicaSlotId writerTarget;
    KisPageOperationId handoffOperation;
    KisReplicaSlotId handoffSource;
    KisReplicaSlotId handoffTarget;
    KisPageWriteMode writerMode = KisPageWriteMode::PreserveContents;
    KisPageWriterPhase writerPhase = KisPageWriterPhase::None;

    bool isEmpty() const
    { return writerPhase == KisPageWriterPhase::None && !handoffOperation.isValid(); }
};

// The original page is the sole owner of native and publication activity.
// Generic writer/handoff payload remains sparse and is prepared before install.
struct MetadataPageActivityFacts {
    KisPagePrivateWrite *nativeWrite = nullptr;
    const void *publicationClaim = nullptr;
    KisPageTransactionId writerTransaction;
    quint16 preparedPublications = 0;
    bool genericWriter = false;
    bool genericHandoff = false;

    bool hasWriter() const { return nativeWrite || genericWriter; }
    bool isEmpty() const
    { return !hasWriter() && !genericHandoff && !publicationClaim && !preparedPublications; }
};
static_assert(sizeof(MetadataPageActivityFacts) == 32);


struct MetadataPage {
    KisVersionSlotId firstVersion;
    KisVersionSlotId lastVersion;
    KisVersionSlotId firstHistory;
    KisVersionSlotId lastHistory;
    KisVersionSlotId firstMutable;
    KisVersionSlotId lastMutable;
    KisVersionSlotId publishedVersion;
    KisImageEpochId publishedEpoch;
    KisPageGeneration nextGeneration;
    quint64 revision = 1;
    quint32 versionCount = 0;
    quint32 historyCount = 0;
    quint32 mutableCount = 0;
    MetadataPageActivityFacts activity;
    KisPageRetirementRecord *idleCapacity = nullptr; // Non-owning original paid record.

    bool revisionCanAdvance() const
    {
        return revision != std::numeric_limits<quint64>::max();
    }
};

static_assert(sizeof(MetadataPage) == 136);

/**
 * Stack projection of original page facts and sparse generic payload. Public snapshots are never
 * retained here: stable record slots are projected back to value handles only
 * at a state-machine or diagnostic boundary. An entry exists only while a
 * writer, handoff, publication claim, or non-owning idle reference is live.
 */
struct MetadataPageActivity {
    KisPageWriterToken writerToken;
    KisPageOperationId writerOperation;
    KisPageTransactionId writerTransaction;
    KisVersionSlotId writerBaseVersion;
    KisReplicaSlotId writerBaseAuthority;
    KisReplicaSlotId writerTarget;
    KisPageOperationId handoffOperation;
    KisReplicaSlotId handoffSource;
    KisReplicaSlotId handoffTarget;
    const void *publicationClaim = nullptr;
    KisPagePrivateWrite *nativeWrite = nullptr;
    quint16 preparedPublications = 0;
    KisPageWriteMode writerMode = KisPageWriteMode::PreserveContents;
    KisPageWriterPhase writerPhase = KisPageWriterPhase::None;

    bool hasWriter() const
    {
        return nativeWrite || writerPhase != KisPageWriterPhase::None;
    }

    bool hasHandoff() const
    {
        return handoffOperation.isValid();
    }

    bool isEmpty() const
    {
        return !hasWriter() && !hasHandoff() && !publicationClaim && preparedPublications == 0;
    }
};

static_assert(sizeof(MetadataPageActivity) <= 104);

struct MetadataKeyLess {
    bool operator()(const KisPageKey &a, const KisPageKey &b) const
    { return std::tie(a.surface.value, a.page.row, a.page.column) < std::tie(b.surface.value, b.page.row, b.page.column); }
    bool operator()(const KisPageVersion &a, const KisPageVersion &b) const
    { return std::tie(a.key.surface.value, a.key.page.row, a.key.page.column, a.generation.value, a.defaultPixelRevision)
        < std::tie(b.key.surface.value, b.key.page.row, b.key.page.column, b.generation.value, b.defaultPixelRevision); }
};

// Observational only: rebind reports the retained library node capacity. The
// underlying original storage allocator remains the sole charging authority.
template<class T>
struct MetadataMapAllocator : KisMutationStorageAllocator<T, false> {
    using Base = KisMutationStorageAllocator<T, false>;
    using value_type = T;
    template<class U> struct rebind { using other = MetadataMapAllocator<U>; };
    MetadataMapAllocator(KisPageWorkingStorage storage, std::atomic<quint64> *bytes)
        : Base(storage), bytes(bytes) {}
    template<class U> MetadataMapAllocator(const MetadataMapAllocator<U> &other)
        : Base(other), bytes(other.bytes) {}
    T *allocate(size_t count)
    {
        auto *data = Base::allocate(count);
        bytes->fetch_add(kisPageStorageBytes(data, alignof(T)), std::memory_order_relaxed);
        return data;
    }
    void deallocate(T *data, size_t count) noexcept
    {
        const auto capacity = kisPageStorageBytes(data, alignof(T));
        Base::deallocate(data, count);
        bytes->fetch_sub(capacity, std::memory_order_relaxed);
    }
    template<class U> bool operator==(const MetadataMapAllocator<U> &other) const
    { return Base::operator==(other) && bytes == other.bytes; }
    template<class U> bool operator!=(const MetadataMapAllocator<U> &other) const { return !(*this == other); }
    std::atomic<quint64> *bytes;
};
template<class Key, class Value>
using MetadataMap = std::map<Key, Value, MetadataKeyLess, MetadataMapAllocator<std::pair<const Key, Value>>>;

/**
 * One record/index owner per shard. Pages contain only intrusive slot heads,
 * tails and counts; no page owns a list, map, set, hash or shared heap object.
 */
struct ShardRecordStore {
    using ExactIndex = KisShardSlotIndex<KisPageVersion, KisVersionSlotId>;
    using PhysicalIndex = KisShardSlotIndex<PhysicalSlot, KisReplicaSlotId>;
    using HistoryPosition = std::pair<quint64, quint64>;

    explicit ShardRecordStore(MetadataArenas *storage, KisMutationStorageAllocator<char> allocator)
        : arenas(storage), exactVersions(allocator), physicalSlots(allocator)
    {
        Q_ASSERT(arenas);
    }

    MetadataArenas *arenas = nullptr;
    ExactIndex exactVersions;
    PhysicalIndex physicalSlots;

    struct ReservationSet {
        MetadataArenaReservations arenas;
        ExactIndex::Reservation exactVersions;
        PhysicalIndex::Reservation physicalSlots;

        void cancel(ShardRecordStore *owner)
        {
            if (!owner)
                return;
            arenas.cancel(owner->arenas);
            owner->exactVersions.cancelReservation(&exactVersions);
            owner->physicalSlots.cancelReservation(&physicalSlots);
        }
    };

    bool reserve(const MetadataArenaDemand &demand,
                 qsizetype exactInsertions,
                 qsizetype physicalInsertions,
                 ReservationSet *reservation)
    {
        if (!reservation || !hasCapacity(demand) || !reservation->arenas.reserve(arenas, demand)) {
            return false;
        }
        if (exactInsertions > 0) {
            const qsizetype required = exactVersions.requiredCapacity(exactInsertions);
            if (!required || !exactVersions.prepareCapacity(required)) {
                reservation->cancel(this);
                return false;
            }
            reservation->exactVersions = exactVersions.reserveInsertions(exactInsertions);
            if (!reservation->exactVersions.isValid()) {
                reservation->cancel(this);
                return false;
            }
        }
        if (physicalInsertions > 0) {
            const qsizetype required = physicalSlots.requiredCapacity(physicalInsertions);
            if (!required || !physicalSlots.prepareCapacity(required)) {
                reservation->cancel(this);
                return false;
            }
            reservation->physicalSlots = physicalSlots.reserveInsertions(physicalInsertions);
            if (!reservation->physicalSlots.isValid()) {
                reservation->cancel(this);
                return false;
            }
        }
        return true;
    }

    static HistoryPosition position(const KisPageVersion &version)
    {
        return {version.generation.value, version.defaultPixelRevision};
    }

    KisVersionRecord *version(KisVersionSlotId slot)
    {
        return arenas->versions.get(slot);
    }
    const KisVersionRecord *version(KisVersionSlotId slot) const
    {
        return arenas->versions.get(slot);
    }
    KisReplicaRecord *replica(KisReplicaSlotId slot)
    {
        return arenas->replicas.get(slot);
    }
    const KisReplicaRecord *replica(KisReplicaSlotId slot) const
    {
        return arenas->replicas.get(slot);
    }
    KisMetadataOverflowNode *overflow(KisMetadataOverflowSlotId slot)
    {
        return arenas->overflow.get(slot);
    }
    const KisMetadataOverflowNode *overflow(KisMetadataOverflowSlotId slot) const
    {
        return arenas->overflow.get(slot);
    }

    bool findVersion(const KisPageVersion &identity, KisVersionSlotId *slot = nullptr) const
    {
        return exactVersions.findExact(identity, slot);
    }

    bool isPreparedBy(const KisPageVersion &identity, KisPageTransactionId transaction, quint64 stamp = 0,
                      const KisReplicaHandle *authority = nullptr, KisVersionSlotId slot = {}) const
    {
        if (!slot.isValid() && !findVersion(identity, &slot))
            return false;
        const KisVersionRecord *record = version(slot);
        return record && record->version == identity
            && record->publication == KisPagePublicationState::Prepared && record->preparedBy == transaction
            && (!stamp || (arenas->facts.get(record->facts)->stamp == stamp
                && arenas->facts.get(record->facts)->transaction == transaction))
            && (!authority || projectReplica(record->authorityReplica) == *authority);
    }

    bool physicalOwner(const PhysicalSlot &physical, KisPageVersion *identity) const
    {
        KisReplicaSlotId replicaSlot;
        if (!physicalSlots.findExact(physical, &replicaSlot))
            return false;
        const KisReplicaRecord *storedReplica = replica(replicaSlot);
        const KisVersionRecord *storedVersion = storedReplica ? version(storedReplica->ownerVersion) : nullptr;
        if (!storedVersion)
            return false;
        if (identity)
            *identity = storedVersion->version;
        return true;
    }

    bool readLeaseOwner(const MetadataPage &page, KisPageLeaseId lease,
                        KisPageVersion *identity) const
    {
        if (!lease.isValid()) return false;
        for (KisVersionSlotId versionSlot = page.firstVersion; versionSlot.isValid();) {
            const KisVersionRecord *storedVersion = version(versionSlot);
            Q_ASSERT(storedVersion);
            if (!storedVersion) break;
            for (KisReplicaSlotId replicaSlot = storedVersion->firstReplica; replicaSlot.isValid();) {
                const KisReplicaRecord *storedReplica = replica(replicaSlot);
                Q_ASSERT(storedReplica);
                if (!storedReplica) break;
                for (KisMetadataOverflowSlotId slot = storedReplica->overflow; slot.isValid();) {
                    const KisMetadataOverflowNode *node = overflow(slot);
                    Q_ASSERT(node);
                    if (!node) break;
                    if (node->kind == KisMetadataOverflowKind::ReadLease && node->value == lease.value) {
                        if (identity) *identity = storedVersion->version;
                        return true;
                    }
                    slot = node->next;
                }
                replicaSlot = storedReplica->nextReplica;
            }
            versionSlot = storedVersion->nextVersion;
        }
        return false;
    }

    void linkVersion(MetadataPage *page, KisVersionRecord *record)
    {
        Q_ASSERT(page && record && record->slot.isValid());
        record->previousVersion = page->lastVersion;
        record->nextVersion = {};
        if (page->lastVersion.isValid()) {
            KisVersionRecord *previous = version(page->lastVersion);
            Q_ASSERT(previous);
            previous->nextVersion = record->slot;
        } else {
            page->firstVersion = record->slot;
        }
        page->lastVersion = record->slot;
        ++page->versionCount;
    }

    void unlinkVersion(MetadataPage *page, KisVersionRecord *record)
    {
        Q_ASSERT(page && record && page->versionCount > 0);
        if (record->previousVersion.isValid()) {
            KisVersionRecord *previous = version(record->previousVersion);
            Q_ASSERT(previous);
            previous->nextVersion = record->nextVersion;
        } else {
            page->firstVersion = record->nextVersion;
        }
        if (record->nextVersion.isValid()) {
            KisVersionRecord *next = version(record->nextVersion);
            Q_ASSERT(next);
            next->previousVersion = record->previousVersion;
        } else {
            page->lastVersion = record->previousVersion;
        }
        record->previousVersion = {};
        record->nextVersion = {};
        --page->versionCount;
    }

    void linkHistory(MetadataPage *page, KisVersionRecord *record)
    {
        Q_ASSERT(page && record && record->publication == KisPagePublicationState::Historical);
        KisVersionSlotId next;
        if (page->lastHistory.isValid()) {
            const KisVersionRecord *last = version(page->lastHistory);
            Q_ASSERT(last);
            if (!last || !(position(last->version) < position(record->version))) {
                next = page->firstHistory;
                while (next.isValid()) {
                    const KisVersionRecord *candidate = version(next);
                    Q_ASSERT(candidate);
                    if (!candidate || !(position(candidate->version) < position(record->version)))
                        break;
                    next = candidate->nextHistory;
                }
            }
        }
        record->nextHistory = next;
        if (next.isValid()) {
            KisVersionRecord *nextRecord = version(next);
            Q_ASSERT(nextRecord);
            record->previousHistory = nextRecord->previousHistory;
            nextRecord->previousHistory = record->slot;
        } else {
            record->previousHistory = page->lastHistory;
            page->lastHistory = record->slot;
        }
        if (record->previousHistory.isValid()) {
            KisVersionRecord *previous = version(record->previousHistory);
            Q_ASSERT(previous);
            previous->nextHistory = record->slot;
        } else {
            page->firstHistory = record->slot;
        }
        ++page->historyCount;
    }

    void unlinkHistory(MetadataPage *page, KisVersionRecord *record)
    {
        Q_ASSERT(page && record && page->historyCount > 0);
        if (record->previousHistory.isValid()) {
            KisVersionRecord *previous = version(record->previousHistory);
            Q_ASSERT(previous);
            previous->nextHistory = record->nextHistory;
        } else {
            page->firstHistory = record->nextHistory;
        }
        if (record->nextHistory.isValid()) {
            KisVersionRecord *next = version(record->nextHistory);
            Q_ASSERT(next);
            next->previousHistory = record->previousHistory;
        } else {
            page->lastHistory = record->previousHistory;
        }
        record->previousHistory = {};
        record->nextHistory = {};
        --page->historyCount;
    }

    void linkMutable(MetadataPage *page, KisVersionRecord *record)
    {
        Q_ASSERT(page && record);
        record->nextMutable = {};
        if (page->lastMutable.isValid()) {
            KisVersionRecord *previous = version(page->lastMutable);
            Q_ASSERT(previous);
            previous->nextMutable = record->slot;
        } else {
            page->firstMutable = record->slot;
        }
        page->lastMutable = record->slot;
        ++page->mutableCount;
    }

    void unlinkMutable(MetadataPage *page, KisVersionRecord *record)
    {
        Q_ASSERT(page && record && page->mutableCount > 0);
        KisVersionSlotId previousSlot;
        for (KisVersionSlotId slot = page->firstMutable; slot.isValid() && !(slot == record->slot);) {
            const KisVersionRecord *candidate = version(slot);
            Q_ASSERT(candidate);
            if (!candidate)
                break;
            previousSlot = slot;
            slot = candidate->nextMutable;
        }
        if (previousSlot.isValid()) {
            KisVersionRecord *previous = version(previousSlot);
            Q_ASSERT(previous);
            previous->nextMutable = record->nextMutable;
        } else {
            Q_ASSERT(page->firstMutable == record->slot);
            page->firstMutable = record->nextMutable;
        }
        if (!record->nextMutable.isValid()) {
            page->lastMutable = previousSlot;
        }
        record->nextMutable = {};
        --page->mutableCount;
    }

    void indexPublication(MetadataPage *page, KisVersionRecord *record)
    {
        if (record->publication == KisPagePublicationState::Historical) {
            linkHistory(page, record);
        }
        if (record->publication == KisPagePublicationState::Prepared
            || record->publication == KisPagePublicationState::Unpublished) {
            linkMutable(page, record);
        }
    }

    void unindexPublication(MetadataPage *page, KisVersionRecord *record)
    {
        if (record->publication == KisPagePublicationState::Historical) {
            unlinkHistory(page, record);
        }
        if (record->publication == KisPagePublicationState::Prepared
            || record->publication == KisPagePublicationState::Unpublished) {
            unlinkMutable(page, record);
        }
    }

    bool setPublication(MetadataPage *page,
                        const KisPageVersion &identity,
                        KisPagePublicationState publication,
                        KisPageTransactionId preparedBy)
    {
        KisVersionSlotId slot;
        if (!findVersion(identity, &slot))
            return false;
        KisVersionRecord *record = version(slot);
        if (!record)
            return false;
        if (record->publication == publication && record->preparedBy == preparedBy)
            return true;
        unindexPublication(page, record);
        record->publication = publication;
        record->preparedBy = preparedBy;
        indexPublication(page, record);
        return true;
    }

    void unindexPhysical(const KisVersionRecord &record)
    {
        for (KisReplicaSlotId slot = record.firstReplica; slot.isValid();) {
            const KisReplicaRecord *stored = replica(slot);
            Q_ASSERT(stored);
            if (!stored)
                break;
            const bool erased = physicalSlots.eraseExact(
                replicaHandle(*stored, record.version).physicalSlotIdentity(), slot);
            Q_ASSERT(erased);
            Q_UNUSED(erased);
            slot = stored->nextReplica;
        }
    }

    void eraseOverflowChain(KisMetadataOverflowSlotId head)
    {
        for (KisMetadataOverflowSlotId slot = head; slot.isValid();) {
            const KisMetadataOverflowNode *record = overflow(slot);
            Q_ASSERT(record);
            if (!record)
                break;
            const KisMetadataOverflowSlotId next = record->next;
            const bool erased = arenas->overflow.erase(slot);
            Q_ASSERT(erased);
            Q_UNUSED(erased);
            slot = next;
        }
    }

    void eraseChildren(KisVersionRecord *record)
    {
        Q_ASSERT(record);
        eraseOverflowChain(record->capturedReadViews);
        for (KisReplicaSlotId slot = record->firstReplica; slot.isValid();) {
            KisReplicaRecord *stored = replica(slot);
            Q_ASSERT(stored);
            if (!stored)
                break;
            const KisReplicaSlotId next = stored->nextReplica;
            eraseOverflowChain(stored->overflow);
            const bool erased = arenas->replicas.erase(slot);
            Q_ASSERT(erased);
            Q_UNUSED(erased);
            slot = next;
        }
        record->firstReplica = {};
        record->authorityReplica = {};
        record->capturedReadViews = {};
        record->replicaCount = 0;
    }

    template<class Snapshots>
    MetadataArenaDemand batchDemand(const Snapshots &snapshots, bool replaceEveryVersion = false) const
    {
        MetadataArenaDemand result;
        for (const auto &snapshot : snapshots) {
            if (replaceEveryVersion || !exactVersions.findExact(snapshot.version)) {
                ++result.versions;
            }
            result.replicas += quint64(snapshot.replicas.size());
            result.overflow += overflowCount(snapshot);
        }
        return result;
    }

    bool hasCapacity(const MetadataArenaDemand &demand) const
    {
        return metadataArenasHaveCapacity(*arenas, demand);
    }

    void appendOverflow(KisMetadataOverflowSlotId *head,
                        KisMetadataOverflowSlotId *tail,
                        OverflowArena::Reservation *reservation,
                        KisMetadataOverflowKind kind,
                        quint64 value,
                        const KisCompletionTicket &completion = {})
    {
        KisMetadataOverflowNode node;
        node.kind = kind;
        node.value = value;
        node.completion = completion;
        const KisMetadataOverflowSlotId slot = arenas->overflow.emplaceReserved(reservation, node);
        Q_ASSERT(slot.isValid());
        if (!head->isValid())
            *head = slot;
        if (tail->isValid()) {
            auto *previous = arenas->overflow.get(*tail);
            Q_ASSERT(previous);
            previous->next = slot;
        }
        *tail = slot;
    }

    template<class Snapshot>
    void assign(KisVersionRecord *record,
                const Snapshot &snapshot,
                MetadataArenaReservations *reservation)
    {
        Q_ASSERT(record && reservation);
        const KisVersionSlotId slot = record->slot;
        const KisVersionFactsSlotId facts = record->facts;
        auto *native = arenas->facts.get(facts);
        const auto *selected = replica(native->nativeReplica);
        const auto selectedIdentity = selected ? selected->identity : KisReplicaAllocationIdentity{};
        native->nativeReplica = {};
        native->nativeReaders = snapshot.nativeReaders;
        const KisVersionSlotId previousVersion = record->previousVersion;
        const KisVersionSlotId nextVersion = record->nextVersion;
        const KisVersionSlotId previousHistory = record->previousHistory;
        const KisVersionSlotId nextHistory = record->nextHistory;
        const KisVersionSlotId nextMutable = record->nextMutable;
        eraseChildren(record);
        *record = {};
        record->version = snapshot.version;
        record->slot = slot;
        record->facts = facts;
        record->previousVersion = previousVersion;
        record->nextVersion = nextVersion;
        record->previousHistory = previousHistory;
        record->nextHistory = nextHistory;
        record->nextMutable = nextMutable;
        record->publication = snapshot.publication;
        record->preparedBy = snapshot.preparedBy;
        record->replicaCount = quint32(snapshot.replicas.size());

        KisMetadataOverflowSlotId capturedTail;
        for (KisImageEpochSnapshotToken token : snapshot.capturedReadViews) {
            appendOverflow(&record->capturedReadViews,
                           &capturedTail,
                           &reservation->overflow,
                           KisMetadataOverflowKind::CapturedReadView,
                           token.value);
        }

        KisReplicaSlotId replicaTail;
        for (const auto &source : snapshot.replicas) {
            KisReplicaRecord next;
            next.identity = source.replica.allocationIdentity();
            next.layoutRevision = source.replica.layout.layoutRevision;
            next.formatId = source.replica.layout.formatId;
            next.byteSize = source.replica.layout.byteSize;
            next.ownerVersion = record->slot;
            next.activeOperation = source.activeOperation;
            next.validRect = source.replica.layout.validRect;
            next.pageExtent = source.replica.layout.pageExtent;
            next.rowStride = source.replica.layout.rowStride;
            next.pinCount = source.pinCount;
            next.domain = source.replica.domain;
            next.validity = source.validity;
            const KisReplicaSlotId replicaSlot = arenas->replicas.emplaceReserved(&reservation->replicas, next);
            Q_ASSERT(replicaSlot.isValid());
            if (native->nativeBinding && next.identity == selectedIdentity)
                native->nativeReplica = replicaSlot;
            KisReplicaRecord *inserted = arenas->replicas.get(replicaSlot);
            Q_ASSERT(inserted);
            if (!record->firstReplica.isValid())
                record->firstReplica = replicaSlot;
            if (replicaTail.isValid()) {
                KisReplicaRecord *previous = arenas->replicas.get(replicaTail);
                Q_ASSERT(previous);
                previous->nextReplica = replicaSlot;
            }
            replicaTail = replicaSlot;
            if (source.replica == snapshot.authority) {
                record->authorityReplica = replicaSlot;
            }

            KisMetadataOverflowSlotId overflowTail;
            for (KisPageLeaseId lease : source.readLeases) {
                appendOverflow(&inserted->overflow,
                               &overflowTail,
                               &reservation->overflow,
                               KisMetadataOverflowKind::ReadLease,
                               lease.value);
            }
            for (const KisCompletionTicket &completion : source.pendingLastUses) {
                appendOverflow(&inserted->overflow,
                               &overflowTail,
                               &reservation->overflow,
                               KisMetadataOverflowKind::PendingLastUse,
                               0,
                               completion);
            }
        }
        Q_ASSERT(!snapshot.authority.isValid() || record->authorityReplica.isValid());
    }

    template<bool Protection = true, class Version>
    Version project(const KisVersionRecord &record, Version result) const
    {
        result.version = record.version;
        result.publication = record.publication;
        result.preparedBy = record.preparedBy;
        result.nativeReaders = arenas->facts.get(record.facts)->nativeReaders;
        for (KisMetadataOverflowSlotId slot = Protection ? record.capturedReadViews : KisMetadataOverflowSlotId{}; slot.isValid();) {
            const KisMetadataOverflowNode *node = overflow(slot);
            Q_ASSERT(node && node->kind == KisMetadataOverflowKind::CapturedReadView);
            if (!node)
                break;
            result.capturedReadViews.push_back(KisImageEpochSnapshotToken{node->value});
            slot = node->next;
        }
        result.replicas.reserve(qsizetype(record.replicaCount));
        for (KisReplicaSlotId slot = record.firstReplica; slot.isValid();) {
            const KisReplicaRecord *stored = replica(slot);
            Q_ASSERT(stored);
            if (!stored)
                break;
            auto projected = kisPageReplicaValue(result);
            projected.replica = replicaHandle(*stored, record.version);
            projected.validity = stored->validity;
            projected.activeOperation = stored->activeOperation;
            projected.pinCount = stored->pinCount;
            for (KisMetadataOverflowSlotId overflowSlot = Protection ? stored->overflow : KisMetadataOverflowSlotId{}; overflowSlot.isValid();) {
                const KisMetadataOverflowNode *node = overflow(overflowSlot);
                Q_ASSERT(node);
                if (!node)
                    break;
                if (node->kind == KisMetadataOverflowKind::ReadLease) {
                    projected.readLeases.push_back(KisPageLeaseId{node->value});
                } else if (node->kind == KisMetadataOverflowKind::PendingLastUse) {
                    projected.pendingLastUses.push_back(node->completion);
                }
                overflowSlot = node->next;
            }
            if (slot == record.authorityReplica) {
                result.authority = projected.replica;
            }
            result.replicas.push_back(std::move(projected));
            slot = stored->nextReplica;
        }
        return result;
    }

    template<class Version, bool Protection = true>
    bool snapshot(const KisPageVersion &identity, Version *snapshot) const
    {
        KisVersionSlotId slot;
        if (!snapshot || !findVersion(identity, &slot))
            return false;
        const KisVersionRecord *record = version(slot);
        if (!record)
            return false;
        *snapshot = project<Protection>(*record, kisPageVersionValue(*snapshot));
        return true;
    }

    bool findReplicaSlot(KisVersionSlotId owner, const KisReplicaHandle &identity, KisReplicaSlotId *slot) const
    {
        if (!slot)
            return false;
        *slot = {};
        if (!identity.isValid())
            return true;
        const KisVersionRecord *record = version(owner);
        if (!record || !(record->version == identity.version))
            return false;
        for (KisReplicaSlotId current = record->firstReplica; current.isValid();) {
            const KisReplicaRecord *stored = replica(current);
            if (!stored)
                return false;
            if (replicaHandle(*stored, record->version) == identity) {
                *slot = current;
                return true;
            }
            current = stored->nextReplica;
        }
        return false;
    }

    bool finishReadProtection(const KisPageTransition &transition,
                              OverflowArena::ReleasedBlocks *released,
                              KisPageMetadataShardMetrics *metrics,
                              QString *error)
    {
        const bool releaseRead = transition.kind == KisPageTransitionKind::ReleaseRead;
        Q_ASSERT(releaseRead || transition.kind == KisPageTransitionKind::AcknowledgeLastUse);
        KisVersionSlotId versionSlot;
        KisReplicaSlotId replicaSlot;
        if (!findVersion(transition.version, &versionSlot)
            || !findReplicaSlot(versionSlot, transition.target, &replicaSlot)
            || !replicaSlot.isValid()) {
            KisPageStoreDetail::setError(error, QStringLiteral("read protection replica is stale or foreign"));
            return false;
        }
        auto *record = replica(replicaSlot);
        auto *link = &record->overflow;
        KisMetadataOverflowSlotId *leaseLink = nullptr;
        KisMetadataOverflowSlotId leaseSlot;
        KisMetadataOverflowNode *leaseNode = nullptr;
        KisMetadataOverflowNode *tail = nullptr;
        quint64 matches = 0;
        // Validate before touching authoritative links. The append order of
        // pending tickets is observable in the reference snapshot, so moving
        // a ReadLease to PendingLastUse also moves its existing slot to the tail.
        for (auto slot = record->overflow; slot.isValid();) {
            auto *node = overflow(slot);
            ++metrics->readProtectionNodeVisits;
            if (!node || (node->kind != KisMetadataOverflowKind::ReadLease
                          && node->kind != KisMetadataOverflowKind::PendingLastUse)) {
                KisPageStoreDetail::setError(error, QStringLiteral("read protection chain is invalid"));
                return false;
            }
            tail = node;
            if (releaseRead && node->kind == KisMetadataOverflowKind::ReadLease
                && node->value == transition.lease.value) {
                leaseLink = link;
                leaseSlot = slot;
                leaseNode = node;
                if (!transition.completion.isValid()) break;
            } else if (!releaseRead && node->kind == KisMetadataOverflowKind::PendingLastUse
                       && node->completion == transition.completion) {
                ++matches;
            }
            link = &node->next;
            slot = node->next;
        }
        if (releaseRead) {
            if (!leaseNode) {
                KisPageStoreDetail::setError(error, QStringLiteral("read lease is stale or belongs to another replica"));
                return false;
            }
            if (transition.completion.isValid()) {
                if (leaseNode != tail) {
                    *leaseLink = leaseNode->next;
                    tail->next = leaseSlot;
                    leaseNode->next = {};
                }
                leaseNode->kind = KisMetadataOverflowKind::PendingLastUse;
                leaseNode->value = 0;
                leaseNode->completion = transition.completion;
                ++metrics->readProtectionSlotReuses;
            } else {
                *leaseLink = leaseNode->next;
                const bool erased = arenas->overflow.erase(leaseSlot, released, 1);
                Q_ASSERT(erased);
                Q_UNUSED(erased);
                ++metrics->readProtectionSlotReleases;
            }
        } else {
            if (!matches) {
                KisPageStoreDetail::setError(error, QStringLiteral("last-use completion is stale or belongs to another replica"));
                return false;
            }
            link = &record->overflow;
            while (link->isValid()) {
                const auto slot = *link;
                auto *node = overflow(slot);
                ++metrics->readProtectionNodeVisits;
                if (node->kind == KisMetadataOverflowKind::PendingLastUse
                    && node->completion == transition.completion) {
                    *link = node->next;
                    const bool erased = arenas->overflow.erase(slot, released, 1);
                    Q_ASSERT(erased);
                    Q_UNUSED(erased);
                    ++metrics->readProtectionSlotReleases;
                } else {
                    link = &node->next;
                }
            }
        }
        return true;
    }

    KisReplicaHandle projectReplica(KisReplicaSlotId slot) const
    {
        if (!slot.isValid())
            return {};
        const KisReplicaRecord *stored = replica(slot);
        const KisVersionRecord *owner = stored ? version(stored->ownerVersion) : nullptr;
        Q_ASSERT(stored && owner);
        return stored && owner ? replicaHandle(*stored, owner->version) : KisReplicaHandle{};
    }

    template<class Info>
    Info versionInfo(const KisPageVersion &identity, KisPageGeneration publishedGeneration) const
    {
        KisVersionSlotId slot;
        return versionInfo<Info>(findVersion(identity, &slot) ? version(slot) : nullptr, publishedGeneration);
    }

    template<class Info>
    Info versionInfo(const KisVersionRecord *stored, KisPageGeneration publishedGeneration) const
    {
        Info result;
        result.publishedGeneration = publishedGeneration;
        if (!stored) return result;
        result.version = stored->version;
        result.publication = stored->publication;
        result.preparedBy = stored->preparedBy;
        const auto *authority = replica(stored->authorityReplica);
        if (authority) result.authority = replicaHandle(*authority, stored->version);
        result.replicaCount = stored->replicaCount;
        result.captured = stored->capturedReadViews.isValid() || arenas->facts.get(stored->facts)->nativeReaders;
        return result;
    }

    KisPageStateSnapshot
    header(const KisPageKey &key, const MetadataPage &page, const MetadataPageActivity *activity) const
    {
        KisPageStateSnapshot result;
        result.key = key;
        result.publishedEpoch = page.publishedEpoch;
        result.nextGeneration = page.nextGeneration;
        const KisVersionRecord *published = version(page.publishedVersion);
        Q_ASSERT(published && published->publication == KisPagePublicationState::Published);
        if (published) {
            result.publishedGeneration = published->version.generation;
            result.publishedDefaultPixelRevision = published->version.defaultPixelRevision;
        }
        if (!activity)
            return result;
        if (activity->nativeWrite) {
            result.nativeGeneration = activity->nativeWrite->target.version.generation;
            result.nativeTransaction = activity->writerTransaction;
        }
        if (activity->writerPhase != KisPageWriterPhase::None) {
            const KisVersionRecord *base = version(activity->writerBaseVersion);
            Q_ASSERT(base);
            result.writer.token = activity->writerToken;
            result.writer.operation = activity->writerOperation;
            result.writer.transaction = activity->writerTransaction;
            result.writer.baseVersion = base ? base->version : KisPageVersion{};
            result.writer.baseAuthority = projectReplica(activity->writerBaseAuthority);
            result.writer.target = projectReplica(activity->writerTarget);
            result.writer.mode = activity->writerMode;
            result.writer.phase = activity->writerPhase;
        }
        if (activity->hasHandoff()) {
            result.authorityHandoff.operation = activity->handoffOperation;
            result.authorityHandoff.source = projectReplica(activity->handoffSource);
            result.authorityHandoff.target = projectReplica(activity->handoffTarget);
        }
        return result;
    }

    KisPageStateSnapshot
    snapshot(const KisPageKey &key, const MetadataPage &page, const MetadataPageActivity *activity) const
    {
        KisPageStateSnapshot result = header(key, page, activity);
        result.versions.reserve(qsizetype(page.versionCount));
        for (KisVersionSlotId slot = page.firstVersion; slot.isValid();) {
            const KisVersionRecord *record = version(slot);
            Q_ASSERT(record);
            if (!record)
                break;
            result.versions.append(project(*record, KisPageVersionStateSnapshot{}));
            slot = record->nextVersion;
        }
        return result;
    }

    bool assignHeader(MetadataPage *page, MetadataPageActivity *activity, const KisPageStateSnapshot &state) const
    {
        if (!page || !activity)
            return false;
        KisVersionSlotId published;
        const KisPageVersion publishedIdentity{state.key,
                                               state.publishedGeneration,
                                               state.publishedDefaultPixelRevision};
        if (!findVersion(publishedIdentity, &published))
            return false;

        MetadataPageActivity next = *activity;
        next.writerToken = {};
        next.writerOperation = {};
        if (!next.nativeWrite) next.writerTransaction = {};
        next.writerBaseVersion = {};
        next.writerBaseAuthority = {};
        next.writerTarget = {};
        next.writerMode = KisPageWriteMode::PreserveContents;
        next.writerPhase = KisPageWriterPhase::None;
        if (state.writer.phase != KisPageWriterPhase::None) {
            KisVersionSlotId writerVersion;
            if (!findVersion(state.writer.baseVersion, &next.writerBaseVersion)
                || !findVersion(state.writer.target.version, &writerVersion)
                || !findReplicaSlot(next.writerBaseVersion, state.writer.baseAuthority, &next.writerBaseAuthority)
                || !findReplicaSlot(writerVersion, state.writer.target, &next.writerTarget)) {
                return false;
            }
            next.writerToken = state.writer.token;
            next.writerOperation = state.writer.operation;
            next.writerTransaction = state.writer.transaction;
            next.writerMode = state.writer.mode;
            next.writerPhase = state.writer.phase;
        }

        next.handoffOperation = {};
        next.handoffSource = {};
        next.handoffTarget = {};
        if (state.authorityHandoff.operation.isValid()) {
            KisVersionSlotId handoffVersion;
            if (!findVersion(state.authorityHandoff.source.version, &handoffVersion)
                || !findReplicaSlot(handoffVersion, state.authorityHandoff.source, &next.handoffSource)
                || !findReplicaSlot(handoffVersion, state.authorityHandoff.target, &next.handoffTarget)) {
                return false;
            }
            next.handoffOperation = state.authorityHandoff.operation;
        }

        page->publishedVersion = published;
        page->publishedEpoch = state.publishedEpoch;
        page->nextGeneration = state.nextGeneration;
        *activity = next;
        return true;
    }

    template<class Snapshot>
    bool canInstallPhysical(const Snapshot &snapshot,
                            KisVersionSlotId replacing) const
    {
        for (auto candidate = snapshot.replicas.cbegin(); candidate != snapshot.replicas.cend(); ++candidate) {
            const auto &replicaState = *candidate;
            const PhysicalSlot key = replicaState.replica.physicalSlotIdentity();
            if (std::any_of(snapshot.replicas.cbegin(), candidate, [&](const auto &other) {
                           return other.replica.physicalSlotIdentity() == key;
                       })) {
                return false;
            }
            KisReplicaSlotId existing;
            if (!physicalSlots.findExact(key, &existing))
                continue;
            const KisReplicaRecord *stored = replica(existing);
            if (!stored || !(stored->ownerVersion == replacing))
                return false;
        }
        return true;
    }

    template<class Snapshot>
    bool putReserved(MetadataPage *page, const Snapshot &snapshot, ReservationSet *reservation)
    {
        Q_ASSERT(reservation);
        KisVersionSlotId slot;
        const bool existing = findVersion(snapshot.version, &slot);
        if (!existing) {
            slot = arenas->versions.emplaceReserved(&reservation->arenas.versions);
            if (!slot.isValid())
                return false;
            KisVersionRecord *record = version(slot);
            Q_ASSERT(record);
            record->slot = slot;
            record->facts = arenas->facts.emplaceReserved(&reservation->arenas.facts);
            Q_ASSERT(record->facts.isValid());
            linkVersion(page, record);
            assign(record, snapshot, &reservation->arenas);
            const bool indexed = exactVersions.insertReserved(&reservation->exactVersions, snapshot.version, slot);
            Q_ASSERT(indexed);
            Q_UNUSED(indexed);
            indexPublication(page, record);
        } else {
            KisVersionRecord *record = version(slot);
            Q_ASSERT(record);
            const bool publicationChanged =
                record->publication != snapshot.publication || !(record->preparedBy == snapshot.preparedBy);
            unindexPhysical(*record);
            if (publicationChanged)
                unindexPublication(page, record);
            assign(record, snapshot, &reservation->arenas);
            if (publicationChanged)
                indexPublication(page, record);
        }

        const KisVersionRecord *installed = version(slot);
        Q_ASSERT(installed);
        for (KisReplicaSlotId replicaSlot = installed->firstReplica; replicaSlot.isValid();) {
            const KisReplicaRecord *stored = replica(replicaSlot);
            Q_ASSERT(stored);
            const bool indexed = physicalSlots.insertReserved(&reservation->physicalSlots,
                                                              replicaHandle(*stored, snapshot.version).physicalSlotIdentity(),
                                                              replicaSlot);
            Q_ASSERT(indexed);
            Q_UNUSED(indexed);
            replicaSlot = stored->nextReplica;
        }
        return true;
    }

    KisVersionSlotId putNativeReserved(MetadataPage *page, const KisPagePrivateWrite &native, ReservationSet *reservation,
        KisPagePublicationState publication = KisPagePublicationState::Prepared)
    {
        const auto slot = arenas->versions.emplaceReserved(&reservation->arenas.versions);
        const auto facts = arenas->facts.emplaceReserved(&reservation->arenas.facts);
        const auto cpuSlot = arenas->replicas.emplaceReserved(&reservation->arenas.replicas);
        Q_ASSERT(slot.isValid() && facts.isValid() && cpuSlot.isValid());
        auto *record = version(slot);
        record->slot = slot;
        record->facts = facts;
        assignNative(record, replica(cpuSlot), cpuSlot, native, publication);
        const bool exact = exactVersions.insertReserved(&reservation->exactVersions, record->version, slot);
        const bool physical = physicalSlots.insertReserved(&reservation->physicalSlots,
            native.target.physicalSlotIdentity(), cpuSlot);
        Q_ASSERT(exact && physical); Q_UNUSED(exact); Q_UNUSED(physical);
        linkVersion(page, record);
        indexPublication(page, record);
        return slot;
    }

    static void assignNative(KisVersionRecord *record, KisReplicaRecord *cpu,
        KisReplicaSlotId replicaSlot, const KisPagePrivateWrite &native, KisPagePublicationState publication)
    {
        record->version = native.target.version;
        record->firstReplica = record->authorityReplica = replicaSlot;
        record->replicaCount = 1;
        record->publication = publication;
        record->preparedBy = publication == KisPagePublicationState::Prepared
            ? native.proof.transaction : KisPageTransactionId{};
        cpu->identity = native.target.allocationIdentity();
        cpu->layoutRevision = native.target.layout.layoutRevision;
        cpu->formatId = native.target.layout.formatId;
        cpu->byteSize = native.target.layout.byteSize;
        cpu->validRect = native.target.layout.validRect;
        cpu->pageExtent = native.target.layout.pageExtent;
        cpu->rowStride = native.target.layout.rowStride;
        cpu->domain = native.target.domain;
        cpu->validity = KisReplicaValidity::Valid;
        cpu->ownerVersion = record->slot;
    }

    const KisVersionRecord *unusedRecord(const KisPageVersion &identity, KisVersionSlotId slot = {}) const
    {
        if (identity.isDefaultPixel() || (!slot.isValid() && !findVersion(identity, &slot))) return nullptr;
        const auto *record = version(slot);
        if (!record || !(record->version == identity)) return nullptr;
        const auto *facts = arenas->facts.get(record->facts);
        const auto *cpu = replica(record->authorityReplica);
        const bool accepted = record->publication == KisPagePublicationState::Published
            || (record->publication == KisPagePublicationState::Prepared && facts->stamp);
        return accepted && !record->capturedReadViews.isValid() && !facts->nativeReaders
            && record->replicaCount == 1 && cpu && cpu->domain == KisPageAccessDomain::CpuRam
            && cpu->validity == KisReplicaValidity::Valid && !cpu->pinCount
            && !cpu->overflow.isValid() && !cpu->activeOperation.isValid() ? record : nullptr;
    }

    bool canDetachPublished(const KisPageVersion &identity) const
    {
        const auto *record = unusedRecord(identity);
        return record && record->publication == KisPagePublicationState::Published;
    }

    const KisVersionRecord *unusedPrepared(const KisPageVersion &identity) const
    {
        const auto *record = unusedRecord(identity);
        return record && record->publication == KisPagePublicationState::Prepared ? record : nullptr;
    }

    bool canDetachPrepared(const KisPageVersion &identity) const
    {
        return unusedPrepared(identity);
    }

    KisVersionSlotId replacementSlot(const KisPageVersion &identity, KisVersionSlotId slot = {}) const
    {
        const auto *record = unusedRecord(identity, slot);
        return record && arenas->versions.canRebind(record->slot) ? record->slot : KisVersionSlotId{};
    }
    bool canReplaceUnused(const KisPageVersion &identity) const { return replacementSlot(identity).isValid(); }

    KisVersionSlotId replaceUnused(MetadataPage *page, const KisPageVersion &old, KisPagePrivateWrite &native,
        std::shared_ptr<const KisPageAllocationDescriptor> *releasedDescriptor,
        std::shared_ptr<KisCpuResidentBinding> *releasedBinding,
        KisPagePublicationState publication = KisPagePublicationState::Prepared,
        KisVersionSlotId oldSlot = {})
    {
        if (!oldSlot.isValid()) oldSlot = replacementSlot(old);
        auto *record = version(oldSlot);
        Q_ASSERT(record && record->version == old);
        auto *facts = arenas->facts.get(record->facts);
        const auto replicaSlot = record->authorityReplica;
        auto *cpu = replica(replicaSlot);
        const auto oldPhysical = replicaHandle(*cpu, old).physicalSlotIdentity();
        // The externally retainable version identity changes. Its private
        // facts and sole unprotected replica keep their paid storage identity;
        // every consumer reaches them through this version under the shard gate.
        // All-D claims and (for Published) the epoch owner's no-consumer check
        // precede this update. The caller installs the new facts before unlock.
        unindexPublication(page, record);
        unlinkVersion(page, record);
        const auto slot = arenas->versions.rebind(oldSlot);
        Q_ASSERT(slot.isValid());
        releasedDescriptor->swap(facts->descriptor);
        releasedBinding->swap(facts->nativeBinding);
        arenas->cpuProofs -= bool(facts->stamp);
        arenas->cpuReadBindings -= bool(*releasedBinding);
        record->slot = slot;
        assignNative(record, cpu, replicaSlot, native, publication);
        const bool exact = exactVersions.replaceExact(old, oldSlot, record->version, slot);
        const bool physical = physicalSlots.replaceExact(oldPhysical, replicaSlot,
            native.target.physicalSlotIdentity(), replicaSlot);
        Q_ASSERT(exact && physical); Q_UNUSED(exact); Q_UNUSED(physical);
        linkVersion(page, record);
        indexPublication(page, record);
        return slot;
    }

    bool putAcceptedBatch(MetadataPage *page, const KisPageWorkingResult &accepted)
    {
        Q_ASSERT(accepted.accepted);
        if (!accepted.accepted) return false;
        // The original transition result preserves physical uniqueness: inputs
        // come from this canonical index, and every identity-introducing state
        // transition checks physicalSlotInUse before appending/replacing it.
        // Recheck canonical ownership after unlocked growth; no second scratch
        // identity index is needed for the already accepted local sequence.
        const auto &snapshots = accepted.next.versions;
        qsizetype newVersions = 0;
        qsizetype replicaCount = 0;
        for (auto snapshotIt = snapshots.cbegin(); snapshotIt != snapshots.cend(); ++snapshotIt) {
            KisVersionSlotId existing;
            if (!findVersion(snapshotIt->version, &existing))
                ++newVersions;
            if (!canInstallPhysical(*snapshotIt, existing))
                return false;
            replicaCount += snapshotIt->replicas.size();
        }

        ReservationSet reservation;
        if (!reserve(batchDemand(snapshots), newVersions, replicaCount, &reservation))
            return false;

        for (const auto &snapshot : snapshots) {
            const bool stored = putReserved(page, snapshot, &reservation);
            Q_ASSERT(stored);
            Q_UNUSED(stored);
        }
        reservation.cancel(this);
        return true;
    }

    void remove(MetadataPage *page, const KisPageVersion &identity,
                std::shared_ptr<const KisPageAllocationDescriptor> *releasedDescriptor = nullptr,
                std::shared_ptr<KisCpuResidentBinding> *releasedBinding = nullptr,
                KisVersionSlotId slot = {})
    {
        if (!slot.isValid() && !findVersion(identity, &slot))
            return;
        KisVersionRecord *record = version(slot);
        Q_ASSERT(record && record->version == identity);
        unindexPhysical(*record);
        unindexPublication(page, record);
        unlinkVersion(page, record);
        eraseChildren(record);
        const bool indexErased = exactVersions.eraseExact(identity, slot);
        auto *facts = arenas->facts.get(record->facts);
        Q_ASSERT(facts);
        if (releasedDescriptor) releasedDescriptor->swap(facts->descriptor);
        if (facts->nativeBinding) --arenas->cpuReadBindings;
        if (releasedBinding) releasedBinding->swap(facts->nativeBinding);
        if (facts->stamp) --arenas->cpuProofs;
        const bool factsErased = arenas->facts.erase(record->facts);
        const bool recordErased = arenas->versions.erase(slot);
        Q_ASSERT(indexErased && recordErased && factsErased);
        Q_UNUSED(indexErased);
        Q_UNUSED(recordErased);
        Q_UNUSED(factsErased);
    }

    void clear(MetadataPage *page)
    {
        while (page->firstVersion.isValid()) {
            const KisVersionRecord *record = version(page->firstVersion);
            Q_ASSERT(record);
            if (!record)
                break;
            remove(page, record->version);
        }
    }

    template<class Snapshots>
    bool replaceAllReserved(MetadataPage *page,
                            const KisPageKey &pageKey,
                            const Snapshots &snapshots,
                            ReservationSet *reservation)
    {
        if (!reservation)
            return false;

        for (const auto &snapshot : snapshots) {
            for (const auto &replicaState : snapshot.replicas) {
                KisReplicaSlotId existingReplica;
                if (!physicalSlots.findExact(replicaState.replica.physicalSlotIdentity(), &existingReplica))
                    continue;
                const KisReplicaRecord *storedReplica = replica(existingReplica);
                const KisVersionRecord *storedVersion = storedReplica ? version(storedReplica->ownerVersion) : nullptr;
                if (!storedVersion || !(storedVersion->version.key == pageKey)) {
                    return false;
                }
            }
        }

        clear(page);
        for (const auto &snapshot : snapshots) {
            const bool stored = putReserved(page, snapshot, reservation);
            Q_ASSERT(stored);
            Q_UNUSED(stored);
        }
        return true;
    }

    template<class Snapshots>
    bool replaceAll(MetadataPage *page, const KisPageKey &pageKey, const Snapshots &snapshots)
    {
        const MetadataArenaDemand demand = batchDemand(snapshots, true);
        qsizetype replicaCount = 0;
        for (const auto &snapshot : snapshots)
            replicaCount += snapshot.replicas.size();
        ReservationSet reservation;
        if (!reserve(demand, qsizetype(snapshots.size()), replicaCount, &reservation))
            return false;
        const bool replaced = replaceAllReserved(page, pageKey, snapshots, &reservation);
        reservation.cancel(this);
        return replaced;
    }

    template<class Additions>
    bool installPreparedAdditionsReserved(MetadataPage *page,
                                          Additions *additions,
                                          ReservationSet *reservation)
    {
        if (!additions || additions->empty())
            return true;
        for (const auto &addition : *additions) {
            if (findVersion(addition.version) || !canInstallPhysical(addition, {}))
                return false;
        }
        for (const auto &addition : *additions) {
            const bool stored = putReserved(page, addition, reservation);
            Q_ASSERT(stored);
            Q_UNUSED(stored);
        }
        return true;
    }

    template<class Visitor>
    void forPreparedTransaction(const MetadataPage &page, KisPageTransactionId transaction, Visitor visitor) const
    {
        for (KisVersionSlotId slot = page.firstMutable; slot.isValid();) {
            const KisVersionRecord *record = version(slot);
            Q_ASSERT(record);
            if (!record)
                break;
            if (record->publication == KisPagePublicationState::Prepared && record->preparedBy == transaction) {
                visitor(record->version);
            }
            slot = record->nextMutable;
        }
    }
};

struct MetadataShard : KisPageMetadataShardMetrics {
    explicit MetadataShard(std::shared_ptr<MetadataBudgetAuthority> authority,
                           KisMutationStorageAllocator<char> storage)
        : budgetAuthority(std::move(authority))
        , budgetCharge(budgetAuthority, 0)
        , arenas(storage)
        , records(&arenas, storage)
        , pages(MetadataKeyLess{}, decltype(pages)::allocator_type(storage.borrowed(), &mapAllocationBytes))
        , activities(MetadataKeyLess{}, decltype(activities)::allocator_type(storage.borrowed(), &mapAllocationBytes)) {}

    mutable QMutex mutex;
    // Retains the original storage owner beyond all borrowing maps below,
    // including when a prepared candidate outlives the coordinator.
    std::shared_ptr<MetadataBudgetAuthority> budgetAuthority;
    // Declared before arenas so arena payloads are destroyed before their
    // remaining live charge is released.
    MetadataBudgetRelease budgetCharge;
    MetadataArenas arenas;
    ShardRecordStore records;
    std::atomic<quint64> mapAllocationBytes{0};
    MetadataMap<KisPageKey, MetadataPage> pages;
    MetadataMap<KisPageKey, MetadataGenericActivity> activities;
    quint64 acceptedTransitions = 0;
    quint64 rejectedTransitions = 0;
    quint64 preparedPublicationPages = 0;
    quint64 installedPublicationPages = 0;
    quint64 rejectedPublicationInstalls = 0;
    quint64 preparedMutationPages = 0;
    quint64 installedMutationPages = 0;
    quint64 rejectedMutationInstalls = 0;
    quint64 publicationHistoryNodesPrepared = 0;
    quint64 publicationAdditionRecordsPrepared = 0;
    const MetadataPageActivityFacts *activity(const KisPageKey &key) const
    {
        const auto found = pages.find(key);
        return found == pages.cend() || found->second.activity.isEmpty() ? nullptr : &found->second.activity;
    }

    bool canMutate(const KisPageKey &, const MetadataPage &page) const
    { return page.revisionCanAdvance() && !page.activity.publicationClaim; }

    void acceptNativeResult(MetadataPage &page, KisVersionSlotId slot, KisPagePrivateWrite &native)
    {
        const auto *record = records.version(slot);
        auto *facts = arenas.facts.get(record->facts);
        Q_ASSERT(!facts->descriptor && !facts->nativeBinding);
        facts->descriptor.swap(native.descriptor);
        facts->stamp = native.proof.providerValidationStamp;
        facts->transaction = native.proof.transaction;
        facts->nativeBinding.swap(native.binding);
        facts->nativeReplica = record->authorityReplica;
        arenas.cpuReadBindings += bool(facts->nativeBinding);
        Q_ASSERT(page.activity.nativeWrite == &native);
        page.activity.nativeWrite = nullptr;
        page.activity.writerTransaction = {};
        native.reserved = false;
        native.location = {};
        native.acceptedSlot = slot;
    }

    bool validDetachment(const MetadataPage &page, const KisPageVersion &version,
        const KisPagePrivateWrite *native, KisPageTransactionId transaction,
        quint64 stamp, const KisReplicaHandle *authority, KisVersionSlotId slot = {}) const
    {
        const auto &activity = page.activity;
        if (native ? activity.nativeWrite != native || !native->reserved || !native->completed
                        || !native->proof.isValid() || !(native->proof.transaction == transaction)
                   : activity.hasWriter()) return false;
        return (native && version == native->target.version)
            || records.isPreparedBy(version, transaction, stamp, authority, slot);
    }

    void installDetachment(MetadataPage &page, const KisPageVersion &version, KisPagePrivateWrite *native,
        bool releaseCapacity, KisVersionSlotId replacement,
        ShardRecordStore::ReservationSet *reservation,
        std::shared_ptr<const KisPageAllocationDescriptor> *releasedDescriptor,
        std::shared_ptr<KisCpuResidentBinding> *releasedBinding)
    {
        KisVersionSlotId accepted;
        if (replacement.isValid()) {
            accepted = records.replaceUnused(&page, version, *native, releasedDescriptor, releasedBinding,
                KisPagePublicationState::Prepared, replacement);
            ++nativeSlotReplacements;
        } else if (!native || !(version == native->target.version)) {
            if (releaseCapacity) records.remove(&page, version, releasedDescriptor, releasedBinding);
            else {
                const bool updated = records.setPublication(&page, version, KisPagePublicationState::Historical, {});
                Q_ASSERT(updated); Q_UNUSED(updated);
                ++publicationHistoryNodesTransferred;
            }
        }
        if (native) {
            if (!replacement.isValid()) {
                Q_ASSERT(reservation);
                accepted = records.putNativeReserved(&page, *native, reservation);
                ++publicationAdditionRecordsTransferred;
                ++publicationVersionInstalls;
            }
            acceptNativeResult(page, accepted, *native);
        }
        ++page.revision;
        ++installedMutationPages;
    }

    MetadataPageActivity activityImage(const KisPageKey &key, const MetadataPage &page) const
    {
        MetadataPageActivity next;
        const auto &facts = page.activity;
        next.nativeWrite = facts.nativeWrite;
        next.writerTransaction = facts.writerTransaction;
        next.publicationClaim = facts.publicationClaim;
        next.preparedPublications = facts.preparedPublications;
        if (!facts.genericWriter && !facts.genericHandoff) return next;
        const auto found = activities.find(key);
        Q_ASSERT(found != activities.end());
        if (found == activities.end()) return next;
        const auto &g = found->second;
        next.writerToken = g.writerToken;
        next.writerOperation = g.writerOperation;
        next.writerBaseVersion = g.writerBaseVersion;
        next.writerBaseAuthority = g.writerBaseAuthority;
        next.writerTarget = g.writerTarget;
        next.handoffOperation = g.handoffOperation;
        next.handoffSource = g.handoffSource;
        next.handoffTarget = g.handoffTarget;
        next.writerMode = g.writerMode;
        next.writerPhase = g.writerPhase;
        return next;
    }

    void installActivityImage(const KisPageKey &key, MetadataPage *page, const MetadataPageActivity &next)
    {
        auto &facts = page->activity;
        facts.nativeWrite = next.nativeWrite;
        facts.writerTransaction = next.writerTransaction;
        facts.publicationClaim = next.publicationClaim;
        facts.preparedPublications = next.preparedPublications;
        facts.genericWriter = next.writerPhase != KisPageWriterPhase::None;
        facts.genericHandoff = next.hasHandoff();
        if (!facts.genericWriter && !facts.genericHandoff) {
            // A prepared candidate still excludes allocator/free work from
            // its publication gate. Release an empty generic payload at the
            // original last-reservation cleanup, exactly like the old node.
            const auto found = activities.find(key);
            if (found != activities.end()) {
                if (facts.isEmpty()) activities.erase(found);
                else found->second = {};
            }
            return;
        }
        const auto found = activities.find(key);
        if (found == activities.end()) qFatal("Generic metadata activity was not prepared before installation");
        auto &g = found->second;
        g.writerToken = next.writerToken;
        g.writerOperation = next.writerOperation;
        g.writerBaseVersion = next.writerBaseVersion;
        g.writerBaseAuthority = next.writerBaseAuthority;
        g.writerTarget = next.writerTarget;
        g.handoffOperation = next.handoffOperation;
        g.handoffSource = next.handoffSource;
        g.handoffTarget = next.handoffTarget;
        g.writerMode = next.writerMode;
        g.writerPhase = next.writerPhase;
    }

    bool ensureActivityCapacity(const KisPageKey &key,
                                const KisPageStateSnapshot *state = nullptr,
                                QString *error = nullptr)
    {
        if (!state || (state->writer.phase == KisPageWriterPhase::None
                       && !state->authorityHandoff.operation.isValid())) return true;
        try { activities.try_emplace(key); return true; }
        catch (const std::bad_alloc &) {
            KisPageStoreDetail::setError(error, QStringLiteral("metadata generic activity storage preparation was refused"));
            return false;
        }
    }

    bool reservePublicationActivity(MetadataPage &page)
    {
        auto &facts = page.activity;
        if (facts.preparedPublications == std::numeric_limits<quint16>::max()) return false;
        ++facts.preparedPublications;
        return true;
    }

    void releasePublicationActivity(const KisPageKey &key, MetadataPage &page)
    {
        Q_ASSERT(page.activity.preparedPublications);
        if (!page.activity.preparedPublications) return;
        --page.activity.preparedPublications;
        if (page.activity.isEmpty()) activities.erase(key);
    }

    KisPageStateSnapshot header(const KisPageKey &key, const MetadataPage &page) const
    {
        const auto image = activityImage(key, page);
        return records.header(key, page, &image);
    }

    bool installHeader(const KisPageKey &key, MetadataPage *page, const KisPageStateSnapshot &state)
    {
        auto next = activityImage(key, *page);
        if (!records.assignHeader(page, &next, state)) return false;
        installActivityImage(key, page, next);
        return true;
    }

    bool installReservedHeader(const KisPageKey &key, MetadataPage *page, const KisPageStateSnapshot &state)
    { return installHeader(key, page, state); }

    bool assign(const KisPageKey &key, MetadataPage *page, const KisPageStateSnapshot &state)
    {
        if (!page || !records.replaceAll(page, key, state.versions))
            return false;
        const bool installed = installHeader(key, page, state);
        Q_ASSERT(installed);
        return installed;
    }

    KisPageStateSnapshot snapshot(const KisPageKey &key, const MetadataPage &page) const
    {
        ++fullSnapshotExports;
        fullSnapshotVersionInputs += page.versionCount;
        if (kisOnPageStoreReclamationThread()) {
            ++backgroundSnapshotExports;
            backgroundSnapshotVersionInputs += page.versionCount;
        }
        const auto image = activityImage(key, page);
        return records.snapshot(key, page, &image);
    }
};

enum class MetadataGrowthResult {
    Ready,
    Stale,
    Exhausted
};

/**
 * Grow shard arenas without invoking the system allocator under shard->mutex.
 * The caller must declare growth before locker, so any unused candidates are
 * destroyed after the locker releases the mutex on every return path.
 */
template<class Locker, class Revalidate>
MetadataGrowthResult growMetadataArenasOutsideLock(MetadataShard *shard,
                                                   const MetadataArenaDemand &demand,
                                                   MetadataArenaGrowth *growth,
                                                   Locker *locker,
                                                   Revalidate revalidate,
                                                   QString *error)
{
    Q_ASSERT(shard);
    Q_ASSERT(growth);
    Q_ASSERT(locker);
    while (!metadataArenasHaveCapacity(shard->arenas, demand)) {
        const MetadataArenaGrowthPlan plan = metadataArenaGrowthPlan(shard->arenas, demand);
        Q_ASSERT(!plan.isEmpty());
        if (!metadataArenaGrowthFitsBudget(shard->arenas, plan)) {
            ++shard->metadataArenaGrowthFailures;
            return MetadataGrowthResult::Exhausted;
        }
        const quint64 preparedBefore = growth->preparedBlockCount();
        const quint64 attachedBefore = growth->attachedBlocks;
        locker->unlock();
        const bool prepared = growth->prepare(plan, shard->budgetAuthority, error);
        locker->relock();
        ++shard->metadataArenaGrowthBatches;
        shard->metadataArenaBlockCandidatesPrepared += growth->preparedBlockCount() - preparedBefore;
        if (!prepared) {
            ++shard->metadataArenaGrowthFailures;
            return MetadataGrowthResult::Exhausted;
        }
        if (!revalidate()) {
            ++shard->metadataArenaGrowthConflicts;
            return MetadataGrowthResult::Stale;
        }
        const auto attached = growth->attach(&shard->arenas,
                                             &shard->budgetCharge, demand);
        shard->metadataArenaBlocksAttached += growth->attachedBlocks - attachedBefore;
        if (attached == MetadataArenaGrowth::AttachResult::Rejected) {
            ++shard->metadataArenaGrowthFailures;
            return MetadataGrowthResult::Exhausted;
        }
        // Another page in the shard may have consumed the observed free slots
        // while the lock was released. Attach what was prepared, then repeat.
    }
    return MetadataGrowthResult::Ready;
}

template<class Snapshots>
MetadataArenaDemand newMetadataDemand(const Snapshots &snapshots)
{
    MetadataArenaDemand result;
    for (const auto &snapshot : snapshots) {
        ++result.versions;
        result.replicas += quint64(snapshot.replicas.size());
        result.overflow += overflowCount(snapshot);
    }
    return result;
}

bool readableVersionLocked(const MetadataShard &shard, const KisVersionRecord &version)
{
    return version.publication == KisPagePublicationState::Published
        || version.publication == KisPagePublicationState::Historical
        || (version.publication == KisPagePublicationState::Prepared
            && (version.capturedReadViews.isValid() || shard.arenas.facts.get(version.facts)->nativeReaders));
}

const KisVersionRecord *readableVersion(const MetadataShard &shard, const KisPageVersion &identity)
{
    KisVersionSlotId slot;
    if (!(shard.pages.find(identity.key) != shard.pages.end()) || !shard.records.findVersion(identity, &slot)) return nullptr;
    const auto *version = shard.records.version(slot);
    return version && readableVersionLocked(shard, *version)
        ? version : nullptr; // Borrowed only while the caller holds the shard gate.
}

bool cpuReadableReplica(const KisReplicaRecord &state, const KisPageVersion &version)
{
    return state.validity == KisReplicaValidity::Valid && replicaHandle(state, version).isValid()
        && (state.domain == KisPageAccessDomain::CpuRam || state.domain == KisPageAccessDomain::UmaShared);
}

} // namespace

class KisPageMetadataCoordinator::Private
{
public:
    using Storage = KisMutationStorageAllocator<Private>;
    Private(std::shared_ptr<KisBackingBudgetController> standalone, const Storage &allocator)
        : standaloneBudget(std::move(standalone))
        , storage(allocator)
        , shards(allocator)
    {
    }

    ~Private()
    {
        shards.clear();
        if (budgetAuthority) budgetAuthority->detach();
    }

    static void destroy(Private *value) noexcept
    {
        if (!value) return;
        // Keep accounting through the actual free, including when the
        // standalone controller is destroyed with the core.
        auto allocator = value->storage;
        std::allocator_traits<Storage>::destroy(allocator, value);
        allocator.deallocate(value, 1);
    }

    static MetadataShard *shardFor(Private *core, const KisPageKey &key)
    {
        if (!core || !key.isValid())
            return nullptr;
        return core->shards.at(kisStablePageKeyHash(key) % core->shards.size()).get();
    }

    static std::shared_ptr<MetadataShard> sharedShardFor(Private *core, const KisPageKey &key)
    {
        if (!core || !key.isValid())
            return {};
        return core->shards.at(kisStablePageKeyHash(key) % core->shards.size());
    }

    static MetadataShard *registeredShard(Private *core, const KisPagePrivateWrite &native)
    {
        // Only the original registered result carries this location. Copies
        // clear it, and acceptance/cancellation revoke it before releasing the
        // page. This checks coordinator membership without rehashing its key.
        const auto &location = native.location;
        if (!core || !native.reserved || !location.page
            || location.shardIndex >= core->shards.size()) return nullptr;
        auto *shard = core->shards[location.shardIndex].get();
        return location.shard == shard ? shard : nullptr;
    }

    std::shared_ptr<KisBackingBudgetController> standaloneBudget;
    Storage storage;
    std::shared_ptr<MetadataBudgetAuthority> budgetAuthority;
    using Shards = std::vector<std::shared_ptr<MetadataShard>,
        KisMutationStorageAllocator<std::shared_ptr<MetadataShard>>>;
    Shards shards;
    QAtomicInteger<quint64> registeredPages{0};
    // Shared identity prevents a capability surviving destruction from being
    // accepted by another coordinator constructed at the same address.
    std::shared_ptr<const quint8> publicationOwner;
    std::atomic<quint64> cpuProofIssuer{0};
    KisCompletionTicket cpuCompletion;
};

class KisPageMetadataCoordinator::PreparedPublication::Data final : public KisPageReclamationJob
{
public:
    using Storage = KisMutationStorageAllocator<char>;
    template<class T> using Array = KisPageWorkingArray<T>;

    // These bounded views construct only prepared values in Data's paid
    // trailing storage. They neither allocate nor own a second storage fee.
    template<class T> class Payload {
    public:
        Payload(T *values, size_t capacity) : m_values(values), m_capacity(capacity) {}
        ~Payload() { while (!empty()) pop_back(); }
        Payload(const Payload &) = delete;
        Payload &operator=(const Payload &) = delete;
        T *begin() { return m_values; }
        T *end() { return m_values + m_size; }
        const T *begin() const { return m_values; }
        const T *end() const { return m_values + m_size; }
        const T *cbegin() const { return begin(); }
        const T *cend() const { return end(); }
        size_t size() const { return m_size; }
        bool empty() const { return m_size == 0; }
        T &front() { return at(0); }
        T &back() { return at(m_size - 1); }
        T &at(size_t index) { Q_ASSERT(index < m_size); return m_values[index]; }
        const T &at(size_t index) const { Q_ASSERT(index < m_size); return m_values[index]; }
        T &operator[](size_t index) { return at(index); }
        const T &operator[](size_t index) const { return at(index); }
        void push_back(T &&value)
        {
            Q_ASSERT(m_size < m_capacity);
            new (m_values + m_size) T(std::move(value));
            ++m_size;
        }
        void pop_back() { Q_ASSERT(m_size); m_values[--m_size].~T(); }
    private:
        T *m_values;
        size_t m_capacity;
        size_t m_size = 0;
    };

    // The same paid values serve the transition and installation, without a
    // second Qt-to-install copy or independent representation of protection.
    using Version = KisPageWorkingVersion;
    static DataPointer create(const Storage &storage, size_t count, size_t shardCount, size_t genericCount);
    ~Data();
    void invoke() override;
    void dispose() noexcept override { DataDeleter{}(this); }
    qsizetype clearBatch(qsizetype maximumWorkUnits);

    struct Entry {
        // One strong pin per sorted shard run. Its first row (destroyed last)
        // retains it until an arena reservation takes that same pin. All other
        // rows use an empty-owner alias to the original shard: no ref traffic.
        std::shared_ptr<MetadataShard> shardOwner;
        // The directory is append-only through that group's pin. Keep the
        // original location; revalidate mutable facts under the shard mutex.
        MetadataMap<KisPageKey, MetadataPage>::iterator originalPage;
        KisVersionSlotId replacementSlot;
        quint64 revision = 0;
        KisPageVersion version;
        size_t inputOrdinal = 0;
        size_t shardIndex = 0;
        bool activityReservation = false;
        bool releaseUnusedCapacity = false;
        KisPagePrivateWrite *native = nullptr;
        bool reuseNativeSlots = false;
        size_t arenaGrowthIndex = std::numeric_limits<size_t>::max();
        std::shared_ptr<const KisPageAllocationDescriptor> releasedDescriptor;
        std::shared_ptr<KisCpuResidentBinding> releasedReadBinding;
        KisPageKey key() const { return version.key; }
    };
    // Only transitions that actually install a general header/record image
    // have this payload. It lives in the SAME original trailing allocation.
    struct GenericPublication {
        explicit GenericPublication(KisPageWorkingStorage storage)
            : installRecords(decltype(installRecords)::allocator_type(storage)) {}
        KisPageStateSnapshot next;
        KisPageWorkingArray<Version, 2> installRecords;
    };
    struct Publication {
        explicit Publication(KisPageWorkingStorage storage)
            : deltas(decltype(deltas)::allocator_type(storage)) {}
        struct PublicationDelta {
            KisPageVersion version;
            KisPagePublicationState publication;
            KisPageTransactionId preparedBy;
            bool detach = false;
        };
        KisPageWorkingArray<PublicationDelta, 2> deltas;
        GenericPublication *generic = nullptr;
    };
    struct ShardArenaGrowth {
        explicit ShardArenaGrowth(KisPageWorkingStorage storage) : blocks(storage) {}
        std::shared_ptr<MetadataShard> shardOwner;
        MetadataArenaDemand totalDemand;
        MetadataArenaGrowth blocks;
        ShardRecordStore::ReservationSet reservations;
        qsizetype exactInsertions = 0;
        qsizetype physicalInsertions = 0;
        size_t firstEntry = 0;
        size_t entryCount = 0;
        MetadataArenas::ReleasedBlocks releasedBlocks;
    };
    void addArenaDemand(size_t i, const MetadataArenaDemand &demand,
                        qsizetype exactInsertions, qsizetype physicalInsertions)
    {
        auto &entry = entries[i];
        if (arenaGrowth.empty() || arenaGrowth.back().shardOwner != entry.shardOwner) {
            ShardArenaGrowth growth(storage.borrowed());
            growth.firstEntry = i;
            arenaGrowth.push_back(std::move(growth));
            size_t first = i;
            while (first && entries[first - 1].shardOwner == entry.shardOwner) --first;
            auto &pin = entries[first].shardOwner;
            auto *shard = pin.get();
            Q_ASSERT(pin.use_count() > 0);
            arenaGrowth.back().shardOwner = std::move(pin);
            pin = std::shared_ptr<MetadataShard>(std::shared_ptr<MetadataShard>{}, shard);
        }
        auto &growth = arenaGrowth.back();
        // Zero-demand native entries between additions need no reservation,
        // but remain in the same bounded identity revalidation range.
        growth.entryCount = i - growth.firstEntry + 1;
        growth.totalDemand.versions += demand.versions;
        growth.totalDemand.replicas += demand.replicas;
        growth.totalDemand.overflow += demand.overflow;
        growth.exactInsertions += exactInsertions;
        growth.physicalInsertions += physicalInsertions;
        entry.arenaGrowthIndex = arenaGrowth.size() - 1;
    }

    void cancel() noexcept;
    size_t batchEnd(size_t first, size_t limit) const
    {
        size_t end = first + 1;
        while (end < limit && end - first < 16
            && entries[end].shardOwner == entries[first].shardOwner) ++end;
        return end;
    }
    void releaseActivities(size_t first, size_t end) noexcept
    {
        if (std::none_of(entries.begin() + first, entries.begin() + end,
            [](const Entry &entry) { return entry.activityReservation; })) return;
        QMutexLocker locker(&entries[first].shardOwner->mutex);
        for (size_t i = first; i < end; ++i) {
            auto &entry = entries[i];
            if (!entry.activityReservation) continue;
            entry.shardOwner->releasePublicationActivity(entry.key(), entry.originalPage->second);
            entry.activityReservation = false;
        }
    }

    using AllocationUnit = std::max_align_t;
    // This original root outlives every borrowing payload, including deferred
    // worker cleanup. The deleter separately keeps it through Data's free.
    KisMutationStorageAllocator<AllocationUnit> storage;
    size_t allocationUnits;
    std::shared_ptr<DeferredMetadataCleanupStatistics> cleanupStatistics;
    QElapsedTimer cleanupAge;
    std::shared_ptr<const quint8> owner;
    KisPageTransaction transaction;
    KisImageEpochId minimumEpoch;
    PreparationKind kind = PreparationKind::Publication;
    bool installed = false;
    // Reservations own their shard until every borrowing entry/payload dies.
    Payload<ShardArenaGrowth> arenaGrowth;
    Payload<Entry> entries;
    // Publication images belong only to root/lease candidates. Mutation
    // acceptance borrows its original rows and never creates these entries.
    Payload<Publication> publications;
    Payload<GenericPublication> genericPublications;
    void popPublication()
    {
        if (publications.empty()) return;
        if (publications.back().generic) {
            Q_ASSERT(publications.back().generic == &genericPublications.back());
            genericPublications.pop_back();
        }
        publications.pop_back();
    }
    // Page keys and projected versions are already unique. This candidate is
    // immutable before any authority lookup; one sorted paid array avoids
    // buckets and one charged allocation for every copied version identity.
    struct BackingAuthority {
        KisPageVersion version;
        KisReplicaHandle replica;
        bool detached = false;
        KisPageRetirementRecord *acceptedIdle = nullptr;
    };
    Array<BackingAuthority> backingAuthorities;

private:
    Data(const Storage &allocator, size_t units, Entry *entryData, size_t entryCount,
         Publication *publicationData, size_t publicationCount,
         GenericPublication *genericData, size_t genericCount,
         ShardArenaGrowth *growthData, size_t growthCount)
        : storage(allocator), allocationUnits(units), arenaGrowth(growthData, growthCount)
        , entries(entryData, entryCount), publications(publicationData, publicationCount)
        , genericPublications(genericData, genericCount)
        , backingAuthorities(storage.borrowed()) {}

    template<class T> static size_t appendPayload(size_t &bytes, size_t count)
    {
        static_assert(alignof(T) <= alignof(AllocationUnit));
        const size_t maximum = size_t(std::numeric_limits<qint64>::max());
        if (bytes > maximum - (alignof(T) - 1)) throw std::bad_alloc();
        const size_t offset = (bytes + alignof(T) - 1) / alignof(T) * alignof(T);
        if (count > (maximum - offset) / sizeof(T)) throw std::bad_alloc();
        bytes = offset + count * sizeof(T);
        return offset;
    }
};

KisPageMetadataCoordinator::PreparedPublication::DataPointer
KisPageMetadataCoordinator::PreparedPublication::Data::create(
    const Storage &storage, size_t count, size_t shardCount, size_t genericCount)
{
    static_assert(alignof(Data) <= alignof(AllocationUnit));
    const size_t publicationCount = count;
    size_t bytes = sizeof(Data);
    const size_t entryOffset = appendPayload<Entry>(bytes, count);
    const size_t publicationOffset = appendPayload<Publication>(bytes, publicationCount);
    const size_t genericOffset = appendPayload<GenericPublication>(bytes, genericCount);
    const size_t growthOffset = appendPayload<ShardArenaGrowth>(bytes, shardCount);
    const size_t units = (bytes + sizeof(AllocationUnit) - 1) / sizeof(AllocationUnit);
    KisMutationStorageAllocator<AllocationUnit> allocator(storage);
    auto *raw = allocator.allocate(units);
    auto *base = reinterpret_cast<char *>(raw);
    try {
        return DataPointer(new (raw) Data(storage, units,
            reinterpret_cast<Entry *>(base + entryOffset), count,
            reinterpret_cast<Publication *>(base + publicationOffset), publicationCount,
            reinterpret_cast<GenericPublication *>(base + genericOffset), genericCount,
            reinterpret_cast<ShardArenaGrowth *>(base + growthOffset), shardCount));
    } catch (...) { allocator.deallocate(raw, units); throw; }
}

void KisPageMetadataCoordinator::PreparedPublication::DataDeleter::operator()(Data *value) const noexcept
{
    if (!value) return;
    auto allocator = value->storage;
    const size_t units = value->allocationUnits;
    value->~Data();
    allocator.deallocate(reinterpret_cast<Data::AllocationUnit *>(value), units);
}

KisPageMetadataCoordinator::PreparedPublication::Data::~Data()
{
    cancel();
}

void KisPageMetadataCoordinator::PreparedPublication::Data::cancel() noexcept
{
    // Installed candidates still retain their original activity reservations
    // through deferred cleanup. Release them even after semantic ownership
    // has ended; cancelled candidates share this same terminal policy.
    for (size_t first = 0; first < entries.size();) {
        const size_t end = batchEnd(first, entries.size());
        releaseActivities(first, end);
        first = end;
    }
    if (!owner) {
        return;
    }
    for (ShardArenaGrowth &growth : arenaGrowth) {
        if (!growth.shardOwner)
            continue;
        QMutexLocker locker(&growth.shardOwner->mutex);
        growth.reservations.cancel(&growth.shardOwner->records);
        growth.releasedBlocks = growth.shardOwner->arenas.takeEmptyBlocks(
            &growth.shardOwner->budgetCharge);
    }
}

// Storage needed by the original mutation rows: real installation capacity,
// independent external shard lifetimes and references that must leave after
// unlocking. No second per-page semantic input or preparation activity.
class KisPageMetadataCoordinator::PreparedPublication::MutationCapacity
{
public:
    using Unit = std::max_align_t;
    struct Group {
        explicit Group(KisPageWorkingStorage storage) : blocks(storage) {}
        MetadataGrowthResult prepare(KisPagePublicationRows rows, size_t first, size_t end,
                                     KisPageTransactionId transaction, QString *error)
        {
            QMutexLocker lock(&shard->mutex);
            const auto valid = [&] {
                for (size_t i = first; i < end; ++i) {
                    const auto &work = rows[i].metadataAcceptance;
                    const auto &input = *work.input;
                    auto *native = input.replacement.native;
                    auto *page = native ? native->location.shard == shard.get()
                        ? static_cast<MetadataPage *>(native->location.page) : nullptr
                        : static_cast<MetadataPage *>(work.page);
                    if (!page || !shard->canMutate(input.mutationVersion().key, *page)
                        || !shard->validDetachment(*page, input.mutationVersion(), native, transaction,
                            input.expectedStamp(), input.expectedAuthority(), input.superseded.metadataSlot))
                        return false;
                }
                return true;
            };
            if (!valid()) return MetadataGrowthResult::Stale;
            const auto grown = growMetadataArenasOutsideLock(shard.get(), demand, &blocks, &lock, valid, error);
            if (grown != MetadataGrowthResult::Ready) return grown;
            if (!shard->records.reserve(demand, qsizetype(demand.versions), qsizetype(demand.replicas), &reservation))
                return MetadataGrowthResult::Exhausted;
            shard->publicationAdditionRecordsPrepared += demand.versions;
            return MetadataGrowthResult::Ready;
        }
        std::shared_ptr<MetadataShard> shard;
        MetadataArenaDemand demand;
        MetadataArenaGrowth blocks;
        ShardRecordStore::ReservationSet reservation;
        MetadataArenas::ReleasedBlocks released;
    };
    struct ReleasedReferences {
        std::shared_ptr<const KisPageAllocationDescriptor> descriptor;
        std::shared_ptr<KisCpuResidentBinding> binding;
    };
    static MutationCapacityPointer create(const KisMutationStorageAllocator<char> &storage,
                                          size_t count, size_t pinCount, size_t releaseCount)
    {
        static_assert(alignof(Group) <= alignof(Unit));
        size_t bytes = sizeof(MutationCapacity);
        KisMutationWorkspace::append<Group>(bytes, count);
        const auto pinOffset = KisMutationWorkspace::append<std::shared_ptr<MetadataShard>>(bytes, pinCount);
        const auto releaseOffset = KisMutationWorkspace::append<ReleasedReferences>(bytes, releaseCount);
        const size_t units = (bytes + sizeof(Unit) - 1) / sizeof(Unit);
        KisMutationStorageAllocator<Unit> allocator(storage);
        auto *raw = allocator.allocate(units);
        auto *base = reinterpret_cast<char *>(raw);
        return MutationCapacityPointer(new (raw) MutationCapacity(allocator, units,
            reinterpret_cast<std::shared_ptr<MetadataShard> *>(base + pinOffset),
            reinterpret_cast<ReleasedReferences *>(base + releaseOffset)));
    }
    Group &append(std::shared_ptr<MetadataShard> shard)
    {
        auto *group = new (groups() + count) Group(storage.borrowed());
        ++count;
        group->shard = std::move(shard);
        return *group;
    }
    void retainShard(const std::shared_ptr<MetadataShard> &shard)
    {
        new (pins + pinCount++) std::shared_ptr<MetadataShard>(shard);
    }
    ReleasedReferences *appendReleased()
    {
        return new (references + referenceCount++) ReleasedReferences;
    }
    // Only real installation capacity enters this mutable group sequence.
    // External-only groups retain one shard pointer in the same allocation.
    void finishShardLocked(MetadataShard *shard)
    {
        if (finished < count && groups()[finished].shard.get() == shard) {
            auto &group = groups()[finished++];
            group.reservation.cancel(&shard->records);
            group.released = shard->arenas.takeEmptyBlocks(&shard->budgetCharge);
        }
        if (next) next->finishShardLocked(shard);
    }
    void appendCapacity(MutationCapacityPointer additional)
    {
        auto *tail = this;
        while (tail->next) tail = tail->next.get();
        tail->next = std::move(additional);
    }
    void finish()
    {
        while (finished < count) {
            auto &group = groups()[finished];
            QMutexLocker lock(&group.shard->mutex);
            finishShardLocked(group.shard.get());
        }
        if (next) next->finish();
    }
    ~MutationCapacity()
    {
        finish();
        while (referenceCount) references[--referenceCount].~ReleasedReferences();
        while (count) groups()[--count].~Group();
        while (pinCount) pins[--pinCount].~shared_ptr();
    }
    KisMutationStorageAllocator<Unit> storage;
    size_t units;
    MutationCapacityPointer next;
    std::shared_ptr<MetadataShard> *pins;
    ReleasedReferences *references;
    size_t count = 0;
    size_t finished = 0;
    size_t referenceCount = 0;
    size_t pinCount = 0;
private:
    Group *groups()
    {
        constexpr size_t offset = (sizeof(MutationCapacity) + alignof(Group) - 1) & ~(alignof(Group) - 1);
        return reinterpret_cast<Group *>(reinterpret_cast<char *>(this) + offset);
    }
    MutationCapacity(const KisMutationStorageAllocator<Unit> &allocator, size_t size,
                     std::shared_ptr<MetadataShard> *owners, ReleasedReferences *released)
        : storage(allocator), units(size), pins(owners), references(released) {}
};

void KisPageMetadataCoordinator::PreparedPublication::MutationCapacityDeleter::operator()(MutationCapacity *value) const noexcept
{
    if (!value) return;
    auto allocator = value->storage;
    const size_t units = value->units;
    value->~MutationCapacity();
    allocator.deallocate(reinterpret_cast<MutationCapacity::Unit *>(value), units);
}

KisPageMetadataCoordinator::PreparedPublication::PreparedPublication() = default;
KisPageMetadataCoordinator::PreparedPublication::~PreparedPublication() = default;
KisPageMetadataCoordinator::PreparedPublication::PreparedPublication(PreparedPublication &&other) noexcept
    : data(std::move(other.data)), m_mutationCapacity(std::move(other.m_mutationCapacity)),
      m_rows(std::exchange(other.m_rows, {})),
      m_count(std::exchange(other.m_count, 0)), m_transaction(other.m_transaction),
      m_owner(std::move(other.m_owner)), m_failure(other.m_failure) {}
KisPageMetadataCoordinator::PreparedPublication &
KisPageMetadataCoordinator::PreparedPublication::operator=(PreparedPublication &&other) noexcept
{
    if (this != &other) {
        data = std::move(other.data); m_mutationCapacity = std::move(other.m_mutationCapacity);
        m_rows = std::exchange(other.m_rows, {});
        m_count = std::exchange(other.m_count, 0); m_transaction = other.m_transaction;
        m_owner = std::move(other.m_owner); m_failure = other.m_failure;
    }
    return *this;
}
bool KisPageMetadataCoordinator::PreparedPublication::isValid() const
{
    return (m_owner && m_failure != Failure::Capacity) || (data && data->owner);
}

KisReplicaHandle
KisPageMetadataCoordinator::PreparedPublication::backingAuthority(
    const KisPageVersion &version, bool *detached, KisPageRetirementRecord ***idleResult) const
{
    if (detached) *detached = false;
    if (idleResult) *idleResult = nullptr;
    if (!data) return {};
    const auto found = std::lower_bound(data->backingAuthorities.begin(),
        data->backingAuthorities.end(), version, [](const auto &entry, const KisPageVersion &key) {
            return MetadataKeyLess{}(entry.version, key);
        });
    if (found != data->backingAuthorities.end() && found->version == version) {
        if (detached) *detached = found->detached;
        if (idleResult && found->detached) *idleResult = &found->acceptedIdle;
        return found->replica;
    }
    // Recoverable candidates already retain the exact base and target values
    // that will be installed. Their authority is derived from those values,
    // without another paid copy. Unrelated authorities remain indexed.
    if (data->kind == PreparationKind::RecoverableWrite) {
        for (const auto &publication : data->publications)
            for (const auto &record : publication.generic->installRecords)
                if (record.version == version) return record.authority;
    }
    return {};
}

bool KisPageMetadataCoordinator::PreparedPublication::hasDetachedBacking() const
{
    return data && std::any_of(data->backingAuthorities.begin(), data->backingAuthorities.end(),
        [](const auto &entry) { return entry.detached; });
}

KisPageMetadataCoordinator::DeferredPublicationCleanup::DeferredPublicationCleanup() = default;
KisPageMetadataCoordinator::DeferredPublicationCleanup::~DeferredPublicationCleanup() = default;
KisPageMetadataCoordinator::DeferredPublicationCleanup::DeferredPublicationCleanup(
    DeferredPublicationCleanup &&) noexcept = default;
KisPageMetadataCoordinator::DeferredPublicationCleanup &
KisPageMetadataCoordinator::DeferredPublicationCleanup::operator=(DeferredPublicationCleanup &&) noexcept = default;
bool KisPageMetadataCoordinator::DeferredPublicationCleanup::isEmpty() const
{
    return !data;
}

qsizetype KisPageMetadataCoordinator::DeferredPublicationCleanup::pendingWorkUnits() const
{
    return data ? std::max(qsizetype(1), qsizetype(data->entries.size())) : 0;
}
qsizetype KisPageMetadataCoordinator::DeferredPublicationCleanup::clearBatch(qsizetype maximumWorkUnits)
{
    if (!data || maximumWorkUnits <= 0)
        return 0;
    const qsizetype count = data->clearBatch(maximumWorkUnits);
    if (data->entries.empty()) data.reset();
    return count;
}

qsizetype KisPageMetadataCoordinator::PreparedPublication::Data::clearBatch(qsizetype maximumWorkUnits)
{
    if (entries.empty()) return 1;
    const qsizetype count = std::min(maximumWorkUnits, qsizetype(entries.size()));
    const size_t limit = entries.size() - size_t(count);
    while (entries.size() > limit) {
        const size_t end = entries.size();
        size_t first = end - 1;
        while (first > limit && end - first < 16
            && entries[first - 1].shardOwner == entries[end - 1].shardOwner) --first;
        // Destroy paid publication payloads outside the shard gate. The
        // original entries keep each owner alive until activity release.
        for (size_t i = first; i < end; ++i)
            popPublication();
        releaseActivities(first, end);
        while (entries.size() > first) entries.pop_back();
    }
    return count;
}

void KisPageMetadataCoordinator::DeferredPublicationCleanup::dispose(
    const std::shared_ptr<DeferredMetadataCleanupStatistics> &statistics)
{
    if (!data) return;
    const qsizetype units = pendingWorkUnits();
    if (units <= 8) { clearBatch(units); return; }
    QMutexLocker lock(&statistics->idleMutex);
    const qsizetype pending = statistics->pendingUnits.loadRelaxed();
    if (units > 1024 || pending > 1024 - units) {
        lock.unlock();
        clearBatch(units);
        return;
    }
    data->cleanupStatistics = statistics;
    data->cleanupAge.start();
    data->reusable = true;
    data->finishContext = data.get();
    data->finished = [](void *context) {
        auto *candidate = static_cast<PreparedPublication::Data *>(context);
        if (candidate->entries.empty()) candidate->dispose();
    };
    statistics->deferredCandidates.fetchAndAddRelaxed(1);
    const auto candidates = statistics->pendingCandidates.fetchAndAddRelaxed(1) + 1;
    statistics->pendingUnits.fetchAndAddRelaxed(units);
    updateMetadataCleanupMaximum(statistics->peakPendingCandidates, candidates);
    updateMetadataCleanupMaximum(statistics->peakPendingUnits, pending + units);
    lock.unlock();
    // The original candidate is the prepared terminal node. No shared job,
    // callback wrapper or per-pass task allocation follows publication.
    if (kisEnqueuePageStoreReclamation(data.get())) data.release();
    else data->invoke();
}

void KisPageMetadataCoordinator::PreparedPublication::Data::invoke()
{
    const auto statistics = cleanupStatistics;
    do {
        QElapsedTimer timer;
        timer.start();
        qsizetype cleared = 0;
        do {
            // A contiguous shard run is one lock unit. Check the original
            // time bound after each payload destruction, before extending it.
            const size_t end = entries.size();
            size_t first = end;
            do {
                --first;
                popPublication();
                ++cleared;
            } while (first && cleared < 16 && timer.nsecsElapsed() < 1000000
                && entries[first - 1].shardOwner == entries[end - 1].shardOwner);
            releaseActivities(first, end);
            while (entries.size() > first) entries.pop_back();
        } while (!entries.empty() && cleared < 16 && timer.nsecsElapsed() < 1000000);
        const quint64 elapsed = quint64(timer.nsecsElapsed());
        statistics->passes.fetchAndAddRelaxed(1);
        statistics->units.fetchAndAddRelaxed(quint64(cleared));
        statistics->nanoseconds.fetchAndAddRelaxed(elapsed);
        updateMetadataCleanupMaximum(statistics->maximumUnitsPerPass, quint64(cleared));
        updateMetadataCleanupMaximum(statistics->maximumPassNanoseconds, elapsed);
        {
            QMutexLocker lock(&statistics->idleMutex);
            const qsizetype before = statistics->pendingUnits.fetchAndSubRelaxed(cleared);
            Q_ASSERT(before >= cleared);
            if (entries.empty()) {
                const quint64 age = quint64(cleanupAge.nsecsElapsed());
                statistics->completedCandidates.fetchAndAddRelaxed(1);
                statistics->queueNanoseconds.fetchAndAddRelaxed(age);
                updateMetadataCleanupMaximum(statistics->maximumQueueNanoseconds, age);
                const qsizetype candidates = statistics->pendingCandidates.fetchAndSubRelaxed(1);
                Q_ASSERT(candidates > 0);
                if (candidates == 1 && statistics->pendingUnits.loadRelaxed() == 0)
                    statistics->idle.wakeAll();
                return;
            }
        }
        if (!entries.empty() && kisEnqueuePageStoreReclamation(this)) return;
    } while (!entries.empty());
}
KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::preparePublication(const KisPageTransaction &transaction,
                                               KisImageEpochId minimumEpoch,
                                               const KisPageSnapshotArray<KisPageTransition> &transitions,
                                               QString *error) const
{
    return preparePublicationImpl(transaction, minimumEpoch, transitions.constData(), transitions.size(), false, error);
}

KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::prepareRestoration(KisImageEpochId minimumEpoch,
                                               const KisPageSnapshotArray<KisPageTransition> &transitions,
                                               QString *error) const
{
    return preparePublicationImpl({}, minimumEpoch, transitions.constData(), transitions.size(), true, error);
}

KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::preparePublication(const KisPageTransaction &transaction,
                                               KisImageEpochId minimumEpoch,
                                               const PublicationChange *changes, qsizetype count,
                                               QString *error, bool detachUnprotectedCurrent) const
{
    return preparePublicationImpl(transaction, minimumEpoch, nullptr, count, false, error,
                                  PreparationKind::Publication, nullptr, changes,
                                  detachUnprotectedCurrent);
}

KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::prepareRestoration(KisImageEpochId minimumEpoch,
                                               const KisPageVersion *versions, qsizetype count,
                                               QString *error) const
{
    return preparePublicationImpl({}, minimumEpoch, nullptr, count, true, error,
                                  PreparationKind::Publication, versions);
}

KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::prepareMutation(const KisPageTransaction &transaction,
                                            KisPagePublicationRows rows,
                                            QString *error) const
try {
    PreparedPublication result;
    auto *d = m_core.load(std::memory_order_acquire);
    const size_t count = rows.size();
    if (!d || d->shards.empty() || !transaction.isValid() || (count && !rows)) return result;
    if (!count) {
        result.m_transaction = transaction;
        result.m_owner = d->publicationOwner;
        KisPageStoreDetail::setError(error, {});
        return result;
    }
    size_t mutations = 0, releases = 0;
    const auto before = [](const auto &a, const auto &b) {
        if (!a.input || !b.input) return a.input && !b.input;
        return a.shardIndex != b.shardIndex ? a.shardIndex < b.shardIndex
            : MetadataKeyLess{}(a.input->mutationVersion().key, b.input->mutationVersion().key);
    };
    KisPagePublicationChange::MetadataAcceptance *previous = nullptr;
    bool ordered = true;
    for (size_t i = 0; i < count; ++i) {
        auto &input = rows[i];
        input.metadataAcceptance = {};
        if (!input.replacement.key().isValid()
            || (input.superseded.version.key.isValid() && !input.superseded.isValid())) return {};
        if (!input.hasMutation()) continue;
        auto *native = input.replacement.native;
        if (native && (!native->reserved || !native->completed || !native->proof.isValid()
            || !(native->proof.transaction == transaction.id))) return {};
        ++mutations;
        auto &work = input.metadataAcceptance;
        work.input = &input;
        work.shardIndex = native ? native->location.shardIndex
            : kisStablePageKeyHash(input.mutationVersion().key) % d->shards.size();
        work.shard = native ? Private::registeredShard(d, *native) : d->shards[work.shardIndex].get();
        if (!work.shard) return {};
        ordered &= i + 1 == mutations && (!previous || !before(work, *previous));
        previous = &work;
        if (native) {
            input.detachesSuperseded = input.detachesSuperseded && !native->freshMetadataRequired
                && input.expectedStamp() && input.expectedAuthority()
                && !(input.mutationVersion() == native->target.version);
        } else {
            releases += input.detachesSuperseded;
        }
    }
    using It = KisPublicationMemberIterator<KisPagePublicationChange::MetadataAcceptance,
                                           &KisPagePublicationChange::metadataAcceptance>;
    if (!ordered) std::sort(It(rows.begin()), It(rows.end()), before);
    size_t capacityGroups = 0, retainedShards = 0;
    for (size_t first = 0; first < mutations;) {
        auto &head = rows[first].metadataAcceptance;
        size_t end = first + 1;
        while (end < mutations && rows[end].metadataAcceptance.shard == head.shard) ++end;
        bool external = false;
        for (size_t i = first; i < end; ++i) {
            const auto &input = *rows[i].metadataAcceptance.input;
            if (i > first && rows[i - 1].metadataAcceptance.input->mutationVersion().key == input.mutationVersion().key)
                return {};
            external |= !input.replacement.native;
            head.needsStorage |= input.replacement.native && !input.detachesSuperseded;
        }
        if (head.needsStorage) ++capacityGroups;
        else if (external) ++retainedShards;
        first = end;
    }
    auto rollbackPreparationCounts = qScopeGuard([&] {
        for (size_t first = 0; first < mutations;) {
            auto *shard = static_cast<MetadataShard *>(rows[first].metadataAcceptance.shard);
            size_t end = first + 1;
            while (end < mutations && rows[end].metadataAcceptance.shard == shard) ++end;
            const auto externalPrepared = [&](size_t i) {
                const auto &work = rows[i].metadataAcceptance;
                return !work.input->replacement.native && work.page;
            };
            size_t preparedCount = 0;
            for (size_t i = first; i < end; ++i) preparedCount += externalPrepared(i);
            if (preparedCount) {
                QMutexLocker lock(&shard->mutex);
                shard->preparedMutationPages -= preparedCount;
            }
            first = end;
        }
    });
    if (capacityGroups || retainedShards) {
        result.m_mutationCapacity = PreparedPublication::MutationCapacity::create(
            d->budgetAuthority->storage<char>(), capacityGroups, retainedShards, releases);
        for (size_t first = 0; first < mutations;) {
            const auto &head = rows[first].metadataAcceptance;
            size_t end = first + 1;
            while (end < mutations && rows[end].metadataAcceptance.shard == head.shard) ++end;
            const auto &shard = d->shards[head.shardIndex];
            PreparedPublication::MutationCapacity::Group *group = nullptr;
            if (head.needsStorage) {
                group = &result.m_mutationCapacity->append(shard);
                for (size_t i = first; i < end; ++i) {
                    auto &work = rows[i].metadataAcceptance;
                    if (work.input->replacement.native && !work.input->detachesSuperseded) {
                        work.prepared = group;
                        ++group->demand.versions; ++group->demand.replicas;
                    }
                }
            }
            bool retained = group;
            // External directory nodes live until their retained shard dies.
            // Bind each original position once, in bounded preparation gates.
            for (size_t batch = first; batch < end;) {
                while (batch < end && rows[batch].metadataAcceptance.input->replacement.native) ++batch;
                if (batch == end) break;
                const size_t batchEnd = std::min(end, batch + 16);
                QMutexLocker lock(&shard->mutex);
                for (size_t i = batch; i < batchEnd; ++i) {
                    auto &work = rows[i].metadataAcceptance;
                    auto &input = *work.input;
                    if (input.replacement.native) continue;
                    if (!retained) {
                        result.m_mutationCapacity->retainShard(shard);
                        retained = true;
                    }
                    const auto found = shard->pages.find(input.mutationVersion().key);
                    if (found == shard->pages.end()
                        || !shard->canMutate(found->first, found->second)
                        || !shard->validDetachment(found->second, input.mutationVersion(), nullptr,
                            transaction.id, input.expectedStamp(), input.expectedAuthority(), input.superseded.metadataSlot)) return {};
                    input.detachesSuperseded = input.detachesSuperseded
                        && shard->records.canDetachPrepared(input.mutationVersion());
                    if (input.detachesSuperseded)
                        work.prepared = result.m_mutationCapacity->appendReleased();
                    work.page = &found->second;
                    ++shard->preparedMutationPages;
                    ++shard->publicationHistoryNodesPrepared;
                }
                batch = batchEnd;
            }
            if (group) {
                const auto grown = group->prepare(rows, first, end, transaction.id, error);
                if (grown != MetadataGrowthResult::Ready) {
                    result.m_failure = grown == MetadataGrowthResult::Stale
                        ? PreparedPublication::Failure::Conflict : PreparedPublication::Failure::Storage;
                    return result;
                }
            }
            first = end;
        }
    }
    result.m_rows = rows;
    result.m_count = mutations;
    result.m_transaction = transaction;
    result.m_owner = d->publicationOwner;
    rollbackPreparationCounts.dismiss();
    KisPageStoreDetail::setError(error, {});
    return result;
}
catch (const std::bad_alloc &) {
    PreparedPublication refused;
    refused.m_failure = PreparedPublication::Failure::Storage;
    KisPageStoreDetail::setError(error, QStringLiteral("mutation installation storage was refused"));
    return refused;
}

bool KisPageMetadataCoordinator::prepareMutationCapacity(PreparedPublication &prepared, QString *error) const
try {
    auto *d = m_core.load(std::memory_order_acquire);
    const auto fail = [&](PreparedPublication::Failure failure, const QString &message) {
        prepared.m_failure = failure;
        prepared.m_owner.reset();
        prepared.m_rows = {};
        prepared.m_count = 0;
        KisPageStoreDetail::setError(error, message);
        return false;
    };
    if (!prepared.needsMutationCapacity() || !d || prepared.m_owner != d->publicationOwner)
        return fail(PreparedPublication::Failure::Conflict, QStringLiteral("mutation preparation is stale or not awaiting capacity"));
    const auto rows = prepared.m_rows;
    const size_t count = prepared.m_count;
    const auto needsSlot = [](const auto &work) {
        const auto *native = work.input->replacement.native;
        return native && native->freshMetadataRequired && !work.prepared;
    };
    size_t groups = 0;
    for (size_t first = 0; first < count;) {
        size_t end = first + 1;
        while (end < count && rows[end].metadataAcceptance.shard == rows[first].metadataAcceptance.shard) ++end;
        bool needsCapacity = false;
        for (size_t i = first; i < end; ++i) needsCapacity |= needsSlot(rows[i].metadataAcceptance);
        groups += needsCapacity;
        first = end;
    }
    if (groups) {
        auto additional = PreparedPublication::MutationCapacity::create(
            d->budgetAuthority->storage<char>(), groups, 0, 0);
        auto *capacity = additional.get();
        if (prepared.m_mutationCapacity) prepared.m_mutationCapacity->appendCapacity(std::move(additional));
        else prepared.m_mutationCapacity = std::move(additional);
        for (size_t first = 0; first < count;) {
            const auto &head = rows[first].metadataAcceptance;
            size_t end = first + 1;
            while (end < count && rows[end].metadataAcceptance.shard == head.shard) ++end;
            PreparedPublication::MutationCapacity::Group *group = nullptr;
            for (size_t i = first; i < end; ++i) {
                auto &work = rows[i].metadataAcceptance;
                if (!needsSlot(work)) continue;
                if (!group) group = &capacity->append(d->shards[head.shardIndex]);
                work.prepared = group;
                ++group->demand.versions; ++group->demand.replicas;
            }
            if (group) {
                const auto grown = group->prepare(rows, first, end, prepared.m_transaction.id, error);
                if (grown != MetadataGrowthResult::Ready)
                    return fail(grown == MetadataGrowthResult::Stale ? PreparedPublication::Failure::Conflict
                        : PreparedPublication::Failure::Storage, QStringLiteral("mutation capacity repair was refused"));
            }
            first = end;
        }
    }
    prepared.m_failure = PreparedPublication::Failure::None;
    KisPageStoreDetail::setError(error, {});
    return true;
}
catch (const std::bad_alloc &) {
    prepared.m_owner.reset();
    prepared.m_rows = {};
    prepared.m_count = 0;
    prepared.m_failure = PreparedPublication::Failure::Storage;
    KisPageStoreDetail::setError(error, QStringLiteral("mutation capacity repair storage was refused"));
    return false;
}

void KisPageMetadataCoordinator::DeferredPublicationCleanup::forEachDetachedBacking(
    void *context, void (*visit)(void *, const KisReplicaHandle &, KisPageRetirementRecord *)) const
{
    if (!data || !data->installed) return;
    for (const auto &entry : data->backingAuthorities)
        if (entry.detached) visit(context, entry.replica, entry.acceptedIdle);
}

bool KisPageMetadataCoordinator::installMutation(PreparedPublication &&prepared,
    const KisPageTransaction &transaction, QString *error)
{
    // The original D is the sole mutation capability. Root/lease candidates
    // cannot be consumed as a mutation, even when their identities match.
    auto rows = std::exchange(prepared.m_rows, {});
    const size_t count = std::exchange(prepared.m_count, 0);
    auto foreignPublication = std::move(prepared.data);
    auto owner = std::move(prepared.m_owner);
    auto *d = m_core.load(std::memory_order_acquire);
    if ((count && !rows) || !owner || foreignPublication || !d
        || prepared.m_failure == PreparedPublication::Failure::Capacity || owner != d->publicationOwner
        || !(prepared.m_transaction == transaction)) return false;
    const auto batchEnd = [rows](size_t first, size_t limit) {
        size_t end = first + 1;
        while (end < limit && end - first < 16
               && rows[end].metadataAcceptance.shard == rows[first].metadataAcceptance.shard) ++end;
        return end;
    };
    // A complete bounded shard batch has no unlock boundary between checking
    // and installing. Keep its one gate instead of creating publication
    // claims whose only consumer would be this same locked call. Larger or
    // multi-shard input still uses the original all-D claim protocol.
    const bool singleWindow = count && batchEnd(0, count) == count;
    std::optional<QMutexLocker<QMutex>> shardLock;
    size_t claimed = 0;
    bool needsCapacity = false;
    for (size_t first = 0; first < count;) {
        const size_t end = batchEnd(first, count);
        auto *shard = static_cast<MetadataShard *>(rows[first].metadataAcceptance.shard);
        shardLock.emplace(&shard->mutex);
        for (size_t i = first; i < end; ++i) {
            auto &work = rows[i].metadataAcceptance;
            auto &input = *work.input;
            const auto &version = input.mutationVersion();
            auto *native = input.replacement.native;
            // Both sources retain their original location. Only actual shard
            // facts are revalidated after preparation's unlocked window.
            auto *page = native ? native->location.shard == shard
                ? static_cast<MetadataPage *>(native->location.page) : nullptr
                : static_cast<MetadataPage *>(work.page);
            if (!page || !shard->canMutate(version.key, *page)) break;
            if (!shard->validDetachment(*page, version, native, transaction.id,
                                        input.expectedStamp(), input.expectedAuthority(), input.superseded.metadataSlot)) break;
            const bool reuse = native && !work.prepared;
            work.replacementSlot = reuse
                ? shard->records.replacementSlot(version, input.superseded.metadataSlot) : KisVersionSlotId{};
            const bool available = reuse ? work.replacementSlot.isValid()
                : !input.detachesSuperseded || shard->records.canDetachPrepared(version);
            if (!available) {
                if (!needsCapacity && reuse) ++shard->nativeCapacityRetries;
                needsCapacity = true;
                input.detachesSuperseded = false;
                if (native) native->freshMetadataRequired = true;
            }
            work.page = page;
            if (!singleWindow) page->activity.publicationClaim = &rows[0];
            ++claimed;
        }
        if (!singleWindow) shardLock.reset();
        if (claimed != end) break;
        first = end;
    }
    if (claimed != count || needsCapacity) {
        for (size_t first = 0; first < claimed;) {
            const size_t end = batchEnd(first, claimed);
            auto *shard = static_cast<MetadataShard *>(rows[first].metadataAcceptance.shard);
            if (!shardLock) shardLock.emplace(&shard->mutex);
            for (size_t i = first; i < end; ++i) {
                auto &work = rows[i].metadataAcceptance;
                if (!singleWindow) static_cast<MetadataPage *>(work.page)->activity.publicationClaim = nullptr;
                work.replacementSlot = {};
            }
            shardLock.reset();
            first = end;
        }
        shardLock.reset(); // A rejection before the first claim may still hold the gate.
        if (count) {
            auto *shard = static_cast<MetadataShard *>(rows[0].metadataAcceptance.shard);
            QMutexLocker lock(&shard->mutex);
            ++shard->rejectedMutationInstalls;
        }
        const bool retry = claimed == count && needsCapacity;
        if (retry) {
            prepared.m_rows = rows;
            prepared.m_count = count;
            prepared.m_owner = std::move(owner);
            prepared.m_failure = PreparedPublication::Failure::Capacity;
        }
        KisPageStoreDetail::setError(error, QStringLiteral("mutation acceptance lost identity or capacity"));
        return false;
    }
    // Input kind and storage lifetime no longer choose a whole-D protocol.
    // Every row consumes the same claims, disposition and installation policy.
    for (size_t first = 0; first < count;) {
        const size_t end = batchEnd(first, count);
        auto *shard = static_cast<MetadataShard *>(rows[first].metadataAcceptance.shard);
        if (!shardLock) shardLock.emplace(&shard->mutex);
        for (size_t i = first; i < end; ++i) {
            auto &work = rows[i].metadataAcceptance;
            auto &input = *work.input;
            auto *page = static_cast<MetadataPage *>(work.page);
            auto *native = input.replacement.native;
            const auto &version = input.mutationVersion();
            Q_ASSERT(page && (singleWindow || page->activity.publicationClaim == &rows[0]));
            std::shared_ptr<const KisPageAllocationDescriptor> oldDescriptor;
            std::shared_ptr<KisCpuResidentBinding> oldBinding;
            auto *released = native ? nullptr
                : static_cast<PreparedPublication::MutationCapacity::ReleasedReferences *>(work.prepared);
            auto *releasedDescriptor = released ? &released->descriptor : &oldDescriptor;
            auto *releasedBinding = released ? &released->binding : &oldBinding;
            auto *group = input.replacement.native
                ? static_cast<PreparedPublication::MutationCapacity::Group *>(work.prepared) : nullptr;
            auto *reservation = group ? &group->reservation : nullptr;
            shard->installDetachment(*page, version, native, input.detachesSuperseded,
                                     work.replacementSlot, reservation, releasedDescriptor, releasedBinding);
            if (native) {
                native->descriptor.swap(oldDescriptor);
                native->binding.swap(oldBinding);
            }
            if (!singleWindow) page->activity.publicationClaim = nullptr;
            // A pre-existing empty generic payload has a real final free, but
            // requires no native preparation/activity/owned Entry to retain it.
            work.page = page->activity.isEmpty()
                && shard->activities.find(version.key) != shard->activities.end() ? page : nullptr;
            work.replacementSlot = {};
        }
        if (prepared.m_mutationCapacity
            && (end == count || rows[end].metadataAcceptance.shard != shard))
            prepared.m_mutationCapacity->finishShardLocked(shard);
        shardLock.reset();
        first = end;
    }
    if (prepared.m_mutationCapacity) prepared.m_mutationCapacity->finish();
    KisPageStoreDetail::setError(error, {});
    return true;
}

void KisPageMetadataCoordinator::finishMutation(KisPagePublicationRows rows)
{
    for (auto &row : rows) {
        auto &work = row.metadataAcceptance;
        if (!work.page || !work.input) continue;
        auto *shard = static_cast<MetadataShard *>(work.shard);
        QMutexLocker lock(&shard->mutex);
        auto *page = static_cast<MetadataPage *>(std::exchange(work.page, nullptr));
        if (page->activity.isEmpty()) shard->activities.erase(work.input->mutationVersion().key);
    }
}

KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::prepareRecoverableWrite(const KisPageTransaction &transaction,
                                                   const KisPageTransition &transition,
                                                   QString *error) const
{
    return preparePublicationImpl(transaction, {}, &transition, 1, false, error,
        PreparationKind::RecoverableWrite);
}

bool KisPageMetadataCoordinator::installRecoverableWrite(PreparedPublication &&prepared,
                                                        const KisPageTransaction &transaction,
                                                        QString *error,
                                                        DeferredPublicationCleanup *deferredCleanup)
{
    return installPublicationImpl(std::move(prepared), transaction, {}, error,
                                  PreparationKind::RecoverableWrite, deferredCleanup);
}

KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::prepareLeaseWrite(const KisPageTransaction &transaction,
                                               const KisPageTransition &transition,
                                               QString *error) const
{
    return preparePublicationImpl(transaction, {}, &transition, 1, false, error, PreparationKind::PrivateWrite);
}

bool KisPageMetadataCoordinator::installLeaseWrite(PreparedPublication &&prepared,
                                                    const KisPageTransaction &transaction,
                                                    QString *error,
                                                    DeferredPublicationCleanup *deferredCleanup)
{
    return installPublicationImpl(std::move(prepared), transaction, {}, error,
        PreparationKind::PrivateWrite, deferredCleanup);
}

KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::preparePublicationImpl(const KisPageTransaction &transaction,
                                                   KisImageEpochId minimumEpoch,
                                                   const KisPageTransition *transitions, qsizetype count,
                                                   bool restoration,
                                                   QString *error,
                                                   PreparationKind kind,
                                                   const KisPageVersion *versions,
                                                   const PublicationChange *changes,
                                                   bool detachUnprotectedCurrent) const
try {
    const bool mutation = kind != PreparationKind::Publication;
    const bool recoverable = kind == PreparationKind::RecoverableWrite;
    const bool privateWrite = kind == PreparationKind::PrivateWrite;
    const bool writePreparation = recoverable || privateWrite;
    PreparedPublication result;
    auto *d = m_core.load(std::memory_order_acquire);
    if (!d || count < 0 || (count && !transitions && !versions && !changes)
        || (!restoration && !transaction.isValid())
        || (!mutation && (!minimumEpoch.isValid() || minimumEpoch.value <= transaction.baseEpoch.value))) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata publication identity is invalid"));
        return result;
    }
    const size_t mutationCount = size_t(count);
    const auto storageOwner = d->budgetAuthority->storage<char>();
    const size_t bytesPerPage = sizeof(PreparedPublication::Data::Entry)
        + sizeof(PreparedPublication::Data::Publication);
    if (mutationCount > size_t(std::numeric_limits<qint64>::max()) / bytesPerPage)
        throw std::bad_alloc();
    size_t shardCount = 0;
    size_t genericCount = 0;
    const auto needsGeneric = [&](qsizetype i) {
        return versions || (changes ? changes[i].kind : transitions[i].kind) != KisPageTransitionKind::CommitTransaction;
    };
    if (mutationCount) {
        if (count == 1) { shardCount = 1; genericCount = needsGeneric(0); }
        else {
            // configure() bounds shards at4096. This stack capacity census
            // supplies no page authority; the existing preparation below
            // still validates and retains each exact shard/page identity.
            std::bitset<4096> usedShards;
            for (qsizetype i = 0; i < count; ++i) {
                const auto &key = versions ? versions[i].key
                    : changes ? changes[i].version.key : transitions[i].version.key;
                usedShards.set(kisStablePageKeyHash(key) % d->shards.size());
                genericCount += needsGeneric(i);
            }
            shardCount = usedShards.count();
        }
    }
    auto data = PreparedPublication::Data::create(storageOwner, mutationCount, shardCount, genericCount);
    const auto storage = data->storage.borrowed();
    data->owner = d->publicationOwner;
    data->transaction = transaction;
    data->minimumEpoch = minimumEpoch;
    data->kind = kind;
    const KisPageStateMachine stateMachine;
    const auto transitionAt = [&](size_t i, KisPageTransition &input) -> const KisPageTransition & {
        if (versions) {
            input.kind = KisPageTransitionKind::RestoreCommittedVersion;
            input.version = versions[i];
            input.transaction = {};
            input.imageEpoch = minimumEpoch;
        } else if (changes) {
            input.kind = changes[i].kind;
            input.version = changes[i].version;
            input.target = changes[i].target;
            input.imageEpoch = minimumEpoch;
            if (input.kind == KisPageTransitionKind::CommitTransaction)
                input.transaction = transaction.id;
        }
        // Compact production inputs and the Qt oracle consume this same
        // transition policy. Only one complete stack value is needed at a time.
        return versions || changes ? input : transitions[i];
    };
    for (qsizetype i = 0; i < count; ++i) {
        KisPageTransition input;
        const auto &transition = transitionAt(size_t(i), input);
        const bool write = transition.kind == KisPageTransitionKind::CommitTransaction;
        if ((recoverable && (transition.kind != KisPageTransitionKind::AcquireRecoverableWrite
                || !(transition.transaction == transaction.id) || transition.imageEpoch.isValid()))
            || (privateWrite && (transition.kind != KisPageTransitionKind::AcquireWrite
                || !(transition.transaction == transaction.id) || transition.imageEpoch.isValid()))
            || (!mutation
                && ((restoration && transition.kind != KisPageTransitionKind::RestoreCommittedVersion)
                    || (!write && transition.kind != KisPageTransitionKind::ReplaceDefaultPixel
                        && transition.kind != KisPageTransitionKind::RestoreCommittedVersion)
                    || (write && !(transition.transaction == transaction.id))
                    || (!write && transition.transaction.isValid()) || !(transition.imageEpoch == minimumEpoch)))) {
            KisPageStoreDetail::setError(error, QStringLiteral("metadata publication transition is invalid or repeated"));
            return result;
        }
        auto *shard = Private::shardFor(d, transition.version.key);
        if (!shard) return result;
        PreparedPublication::Data::Entry entry;
        // No activity or external call occurs before sorting and pinning the
        // groups below. The coordinator still owns its original directory.
        entry.shardOwner = std::shared_ptr<MetadataShard>(std::shared_ptr<MetadataShard>{}, shard);
        entry.version = transition.version;
        entry.inputOrdinal = size_t(i);
        entry.shardIndex = kisStablePageKeyHash(entry.key()) % d->shards.size();
        data->entries.push_back(std::move(entry));
        data->publications.push_back(PreparedPublication::Data::Publication(storage));
    }
    std::sort(data->entries.begin(), data->entries.end(), [](const auto &a, const auto &b) {
        return a.shardIndex != b.shardIndex ? a.shardIndex < b.shardIndex
            : MetadataKeyLess{}(a.key(), b.key());
    });
    for (size_t i = 1; i < data->entries.size(); ++i) {
        if (data->entries[i - 1].key() == data->entries[i].key()) {
            KisPageStoreDetail::setError(error, QStringLiteral("metadata publication transition is invalid or repeated"));
            return result;
        }
    }
    for (size_t first = 0; first < data->entries.size();) {
        auto &pin = data->entries[first].shardOwner;
        pin = Private::sharedShardFor(d, data->entries[first].key());
        size_t end = first + 1;
        while (end < data->entries.size() && data->entries[end].shardOwner == pin) ++end;
        first = end;
    }
    const auto rollbackPreparationCounts = qScopeGuard([&] {
        if (!data) return;
        for (size_t first = 0; first < data->entries.size();) {
            const size_t end = data->batchEnd(first, data->entries.size());
            QMutexLocker locker(&data->entries[first].shardOwner->mutex);
            for (size_t i = first; i < end; ++i) {
                const auto &entry = data->entries[i];
                if (entry.activityReservation)
                    --(mutation ? entry.shardOwner->preparedMutationPages : entry.shardOwner->preparedPublicationPages);
            }
            first = end;
        }
    });

    // Retain facts in the original paid candidate array, grouped by shard.
    // Each gate covers at most 16 pages; no lock nests another shard gate.
    for (size_t first = 0; first < data->entries.size();) {
        const size_t end = data->batchEnd(first, data->entries.size());
        auto *shard = data->entries[first].shardOwner.get();
        {
            QMutexLocker locker(&shard->mutex);
            for (size_t i = first; i < end; ++i) {
                auto &entry = data->entries[i];
                KisPageTransition input;
                const auto &transition = transitionAt(entry.inputOrdinal, input);
                const bool write = transition.kind == KisPageTransitionKind::CommitTransaction;
                const auto page = shard->pages.find(transition.version.key);
                if (page == shard->pages.cend() || !page->second.revisionCanAdvance()
                    || page->second.activity.publicationClaim) {
                    KisPageStoreDetail::setError(error, QStringLiteral("metadata publication page is unavailable"));
                    return result;
                }
                entry.originalPage = page;
                const auto *activity = &page->second.activity;
                entry.revision = page->second.revision;
                auto &publication = data->publications[i];
                const auto *published = shard->records.version(page->second.publishedVersion);
                Q_ASSERT(published);
                const KisPageVersion current = published->version;
                // This only prepares a disposition. The epoch owner must
                // revalidate its no-consumer fact at atomic installation;
                // this page's exact protection is covered by its revision.
                entry.releaseUnusedCapacity = detachUnprotectedCurrent && write
                    && !(current == transition.version) && shard->records.canDetachPublished(current);
                if (write) {
                    // Borrow only original semantic rows while their shard
                    // is locked. Native input remains owned by the caller;
                    // the existing revision claim covers the later install.
                    if (activity->nativeWrite) {
                        entry.native = activity->nativeWrite;
                        if (!entry.native->completed || !entry.native->proof.isValid()
                            || !(entry.native->target.version == transition.version)
                            || !(entry.native->proof.transaction == transaction.id)) return result;
                    }
                    entry.reuseNativeSlots = entry.native && entry.releaseUnusedCapacity
                        && shard->records.canReplaceUnused(current);
                    KisVersionRecord incoming;
                    const KisVersionRecord *target = nullptr;
                    if (entry.native) {
                        incoming.version = entry.native->target.version;
                        incoming.publication = KisPagePublicationState::Prepared;
                        incoming.preparedBy = transaction.id;
                        target = &incoming;
                    } else {
                        KisVersionSlotId slot;
                        if (shard->records.findVersion(transition.version, &slot))
                            target = shard->records.version(slot);
                    }
                    // The exact native reservation above is consumed by
                    // this conversion; other mutable facts stay in place.
                    const auto failure = kisPageCommitPolicy(page->second.publishedEpoch, KisPageGeneration{}, transition,
                        target, static_cast<const KisVersionRecord *>(published),
                        [&](auto visit) {
                            for (auto slot = page->second.firstMutable; slot.isValid();) {
                                const auto *record = shard->records.version(slot);
                                Q_ASSERT(record);
                                visit(*record);
                                slot = record->nextMutable;
                            }
                        },
                        [&](const auto &record, auto state, auto preparedBy) {
                            const bool incomingNative = entry.native && &record == &incoming;
                            const bool detach = entry.releaseUnusedCapacity && record.version == current;
                            const auto authority = incomingNative ? entry.native->target
                                : shard->records.projectReplica(record.authorityReplica);
                            if (authority.isValid())
                                data->backingAuthorities.push_back({record.version, authority, detach});
                            if (!incomingNative) {
                                publication.deltas.push_back({record.version, state, preparedBy, detach});
                                ++shard->publicationVersionInputs;
                            }
                        });
                    if (!failure.isEmpty()) {
                        KisPageStoreDetail::setError(error, failure);
                        return result;
                    }
                    continue;
                }
                data->genericPublications.push_back(PreparedPublication::Data::GenericPublication(storage));
                publication.generic = &data->genericPublications.back();
                publication.generic->next = shard->header(transition.version.key, page->second);
                auto &facts = publication.generic->installRecords;
                if (writePreparation) facts.reserve(2);
                const auto include = [&](const KisPageVersion &identity) {
                    if (std::any_of(facts.cbegin(), facts.cend(), [&](const auto &v) { return v.version == identity; }))
                        return;
                    KisPageWorkingVersion v(storage);
                    // Private acquisition only changes the base pin and a
                    // new target. Like applyWriteSequence(), it never reads
                    // or replaces existing protection. Recoverable retag
                    // removes a replica and must retain the complete image.
                    const bool found = recoverable ? shard->records.snapshot(identity, &v)
                        : shard->records.snapshot<KisPageWorkingVersion, false>(identity, &v);
                    if (found) {
                        facts.push_back(std::move(v));
                    }
                };
                if (!privateWrite)
                    include({publication.generic->next.key, publication.generic->next.publishedGeneration,
                        publication.generic->next.publishedDefaultPixelRevision});
                if (writePreparation)
                    include(transition.baseVersion);
                include(transition.version);
                if (page->second.firstMutable.isValid()) {
                    const KisVersionRecord *mutableRecord = shard->records.version(page->second.firstMutable);
                    Q_ASSERT(mutableRecord);
                    if (mutableRecord)
                        include(mutableRecord->version);
                }
                if (transition.target.isValid()) {
                    KisPageVersion owner;
                    if (shard->records.physicalOwner(transition.target.physicalSlotIdentity(), &owner))
                        include(owner);
                }
                shard->publicationVersionInputs += quint64(facts.size());


            }
        }
        first = end;
    }
    size_t preparedPolicies = 0;
    size_t recordedPolicies = 0;
    const auto recordPolicyWork = [&](size_t i) {
        auto *shard = data->entries[i].shardOwner.get();
        const auto &publication = data->publications[i];
        if (!writePreparation) {
            for (const auto &delta : publication.deltas)
                shard->publicationHistoryNodesPrepared += !delta.detach
                    && delta.publication == KisPagePublicationState::Historical;
            if (publication.generic)
                for (const auto &record : publication.generic->installRecords)
                    shard->publicationHistoryNodesPrepared += record.publication == KisPagePublicationState::Historical;
        }
        shard->publicationAdditionRecordsPrepared += publication.generic ? publication.generic->installRecords.size()
            - (recoverable ? 1 : 0) : (data->entries[i].native ? 1 : 0);
    };
    const auto recordPreparationWork = qScopeGuard([&] {
        // Includes accepted policy work when a later allocation/revalidation
        // refuses preparation; inputs remain counted at their original read.
        if (!data) return;
        for (size_t first = recordedPolicies; first < preparedPolicies;) {
            const size_t end = data->batchEnd(first, preparedPolicies);
            auto *shard = data->entries[first].shardOwner.get();
            QMutexLocker locker(&shard->mutex);
            for (size_t i = first; i < end; ++i) recordPolicyWork(i);
            first = end;
        }
    });
    for (size_t i = 0; i < data->entries.size(); ++i) {
        auto &entry = data->entries[i];
        auto &publication = data->publications[i];
        if (!publication.generic) {
            ++preparedPolicies;
            continue;
        }
        KisPageTransition input;
        const auto &transition = transitionAt(entry.inputOrdinal, input);
        KisPageWorkingResult step(storage);
        auto &before = step.next;
        before.setHeader(publication.generic->next);
        before.versions = std::move(publication.generic->installRecords);
        const KisPageVersion previous{before.key, before.publishedGeneration, before.publishedDefaultPixelRevision};
        // The expensive deterministic work is outside the shard lock and is
        // reused at installation. Revision revalidation includes leases,
        // Only coordinator-owned state reaches this private capability. Like
        // immediate apply(), reuse its established boundary invariants; all
        // transition-local lifetime/identity guards still run. External state
        // enters through registerPage()'s full validation. The reference test
        // compares every supported publication kind with full apply().
        // Indexed inputs deliberately omit unrelated versions.
        const size_t inputCount = before.versions.size();
        if (!writePreparation)
            publication.deltas.reserve(before.versions.size());
        stateMachine.applyKnownValid(step, transition, writePreparation ? nullptr : &publication,
            writePreparation ? nullptr : +[](void *context, const KisPageVersion &version,
                                        KisPagePublicationState state, KisPageTransactionId preparedBy) {
                static_cast<PreparedPublication::Data::Publication *>(context)->deltas.push_back(
                    {version, state, preparedBy});
            });
        if (!step.accepted) {
            KisPageStoreDetail::setError(error, step.rejectionReason);
            return result;
        }
        if (writePreparation) {
            auto ready = transition;
            ready.kind = KisPageTransitionKind::PrepareWrite;
            stateMachine.applyKnownValid(step, ready);
            if (!step.accepted) {
                KisPageStoreDetail::setError(error, step.rejectionReason);
                return result;
            }
        }
        const auto copiedAuthority = [&](const auto &version) {
            return version.authority.isValid() && (!writePreparation
                || (!(version.version == transition.baseVersion) && !(version.version == transition.version)));
        };
        for (auto &delta : publication.deltas)
            delta.detach = entry.releaseUnusedCapacity && delta.version == previous;
        if (data->backingAuthorities.empty())
            data->backingAuthorities.reserve(size_t(std::count_if(step.next.versions.cbegin(),
                step.next.versions.cend(), copiedAuthority)));
        for (const auto &version : std::as_const(step.next.versions))
            if (copiedAuthority(version)) data->backingAuthorities.push_back(
                {version.version, version.authority, entry.releaseUnusedCapacity && version.version == previous});
        if (privateWrite) {
            // The original base and all of its protection stay in the arena.
            // The next writer identifies its one pin delta; the strict page
            // revision claim makes that decision valid until installation.
            auto &records = step.next.versions;
            records.erase(std::remove_if(records.begin(), records.end(), [&](const auto &record) {
                return !(record.version == transition.version);
            }), records.end());
            Q_ASSERT(records.size() == 1);
            publication.generic->installRecords = std::move(records);
        } else if (recoverable) {
            // Strict revision revalidation covers every replica/reader fact
            // used here. Install only this exact base and the new target, in
            // that order, so the old physical slot is unbound before target
            // insertion. Unrelated history is never replaced. This branch is
            // inaccessible through ordinary immediate owner transitions.
            // Base precedes the appended target in this same working array.
            // Remove unrelated input values cold, then transfer its storage
            // directly to installation rather than allocate a second array.
            auto &records = step.next.versions;
            records.erase(std::remove_if(records.begin(), records.end(), [&](const auto &record) {
                return !(record.version == transition.baseVersion) && !(record.version == transition.version);
            }), records.end());
            Q_ASSERT(records.size() == 2 && records[0].version == transition.baseVersion
                && records[1].version == transition.version);
            publication.generic->installRecords = std::move(records);
        } else {
            for (size_t index = inputCount; index < step.next.versions.size(); ++index) {
                const auto &version = step.next.versions[index];
                if (version.publication == KisPagePublicationState::Prepared
                    || version.publication == KisPagePublicationState::Unpublished) {
                    KisPageStoreDetail::setError(error, QStringLiteral("publication candidate introduced mutable metadata"));
                    return result;
                }
            }
            if (step.next.versions.size() > inputCount) {
                // Transfer the appended values and their paid storage; old
                // replica/protection values can be destroyed during prepare.
                auto &records = step.next.versions;
                records.erase(records.begin(), records.begin() + inputCount);
                publication.generic->installRecords = std::move(records);
            }
        }
        // Ordinary publication only changes publication fields. Recoverable
        // writes replace the strictly revision-bound base record and add T;
        // any intervening reader/pin/last-use change rejects that candidate.
        ++preparedPolicies;
        // Headers never own version records at install.
        publication.generic->next = step.next.header();
        Q_ASSERT(step.effects.empty());
    }
    for (size_t first = 0; first < data->entries.size();) {
        const size_t end = data->batchEnd(first, data->entries.size());
        auto *shard = data->entries[first].shardOwner.get();
        QMutexLocker locker(&shard->mutex);
        for (size_t i = first; i < end; ++i) {
            auto &entry = data->entries[i];
            const auto page = entry.originalPage;
            if (!entry.shardOwner->canMutate(entry.key(), page->second)
                || page->second.revision != entry.revision) {
                KisPageStoreDetail::setError(error, QStringLiteral("metadata changed while preparing publication storage"));
                result.m_failure = PreparedPublication::Failure::Conflict;
                return result;
            }
            auto &publication = data->publications[i];
            auto demand = publication.generic ? shard->records.batchDemand(publication.generic->installRecords)
                                              : MetadataArenaDemand{};
            const bool nativeAddition = !publication.generic && entry.native && !entry.reuseNativeSlots;
            if (nativeAddition) { demand.versions = 1; demand.replicas = 1; }
            recordPolicyWork(i);
            ++recordedPolicies;
            qsizetype physicalInsertions = nativeAddition ? 1 : 0;
            if (publication.generic)
                for (const auto &record : publication.generic->installRecords)
                    physicalInsertions += record.replicas.size();
            if (entry.reuseNativeSlots) continue; // No new record/index capacity or arena cleanup.
            data->addArenaDemand(i, demand,
                (publication.generic ? qsizetype(publication.generic->installRecords.size()) - (recoverable ? 1 : 0)
                                     : (nativeAddition ? 1 : 0)), physicalInsertions);
        }
        first = end;
    }
    // Reuse available shard capacity and grow only its shortfall outside the
    // lock. Reserve the complete capacity before releasing the lock, so other
    // mutations cannot consume it while this publication awaits installation.
    for (auto &growth : data->arenaGrowth) {
        QMutexLocker locker(&growth.shardOwner->mutex);
        const auto revalidate = [&] {
            for (size_t i = growth.firstEntry; i < growth.firstEntry + growth.entryCount; ++i) {
                const auto &entry = data->entries[i];
                const auto page = entry.originalPage;
                if (!growth.shardOwner->canMutate(entry.key(), page->second)
                    || page->second.revision != entry.revision) {
                    return false;
                }
            }
            return true;
        };
        if (!revalidate()) {
            ++growth.shardOwner->metadataArenaGrowthConflicts;
            KisPageStoreDetail::setError(error, QStringLiteral("metadata changed while reserving publication capacity"));
            result.m_failure = PreparedPublication::Failure::Conflict;
            return result;
        }

        const auto grown = growMetadataArenasOutsideLock(growth.shardOwner.get(), growth.totalDemand,
            &growth.blocks, &locker, revalidate, error);
        if (grown != MetadataGrowthResult::Ready) {
            result.m_failure = grown == MetadataGrowthResult::Stale
                ? PreparedPublication::Failure::Conflict : PreparedPublication::Failure::Storage;
            return result;
        }
        if (!growth.shardOwner->records.reserve(growth.totalDemand,
                                              growth.exactInsertions,
                                              growth.physicalInsertions,
                                              &growth.reservations)) {
            ++growth.shardOwner->metadataArenaGrowthFailures;
            result.m_failure = PreparedPublication::Failure::Storage;
            KisPageStoreDetail::setError(error, QStringLiteral("metadata publication capacity reservation failed"));
            return result;
        }
    }

    // Prepare real generic payload before installation, and retain the
    // original page facts through the candidate's last reservation. Native
    for (size_t first = 0; first < data->entries.size();) {
        const size_t end = data->batchEnd(first, data->entries.size());
        QMutexLocker locker(&data->entries[first].shardOwner->mutex);
        for (size_t i = first; i < end; ++i) {
            auto &entry = data->entries[i];
            const auto page = entry.originalPage;
            bool stillValid = entry.shardOwner->canMutate(entry.key(), page->second)
                && page->second.revision == entry.revision;
            const bool genericReady = !stillValid
                || !data->publications[i].generic
                || entry.shardOwner->ensureActivityCapacity(entry.key(), &data->publications[i].generic->next, error);
            if (!stillValid || !genericReady || !entry.shardOwner->reservePublicationActivity(page->second)) {
                KisPageStoreDetail::setError(error, QStringLiteral("metadata changed while reserving publication activity"));
                result.m_failure = !stillValid
                    ? PreparedPublication::Failure::Conflict : PreparedPublication::Failure::Storage;
                return result;
            }
            entry.activityReservation = true;
            ++(mutation ? entry.shardOwner->preparedMutationPages : entry.shardOwner->preparedPublicationPages);
        }
        first = end;
    }
    std::sort(data->backingAuthorities.begin(), data->backingAuthorities.end(),
        [](const auto &a, const auto &b) { return MetadataKeyLess{}(a.version, b.version); });
    // No input is read after preparation. Ordinals already name the original
    // owner's rows, including gaps for changes requiring no metadata mutation.
    result.data = std::move(data);
    KisPageStoreDetail::setError(error, {});
    return result;
}
catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("metadata publication preparation storage was refused"));
    PreparedPublication refused;
    refused.m_failure = PreparedPublication::Failure::Storage;
    return refused;
}

bool KisPageMetadataCoordinator::installPublication(PreparedPublication &&prepared,
                                                    const KisPageTransaction &transaction,
                                                    KisImageEpochId epoch,
                                                    QString *error,
                                                    DeferredPublicationCleanup *deferredCleanup)
{
    return installPublicationImpl(std::move(prepared),
                                  transaction,
                                  epoch,
                                  error,
                                  PreparationKind::Publication,
                                  deferredCleanup);
}

bool KisPageMetadataCoordinator::installPublicationImpl(PreparedPublication &&prepared,
                                                        const KisPageTransaction &transaction,
                                                        KisImageEpochId epoch,
                                                        QString *error,
                                                        PreparationKind kind,
                                                        DeferredPublicationCleanup *deferredCleanup)
{
    const bool mutation = kind != PreparationKind::Publication;
    const bool recoverable = kind == PreparationKind::RecoverableWrite;
    const bool privateWrite = kind == PreparationKind::PrivateWrite;
    // Consume even on rejection: callers must prepare again after a conflict.
    prepared.m_rows = {};
    prepared.m_count = 0;
    prepared.m_owner.reset();
    auto data = std::move(prepared.data);
    const auto recordRejectedInstall = [&] {
        if (!data || data->entries.empty() || !data->entries.front().shardOwner)
            return;
        QMutexLocker locker(&data->entries.front().shardOwner->mutex);
        ++(mutation ? data->entries.front().shardOwner->rejectedMutationInstalls
                    : data->entries.front().shardOwner->rejectedPublicationInstalls);
    };
    const bool canDeferCleanup = deferredCleanup && deferredCleanup->isEmpty();
    const auto transferCleanup = qScopeGuard([&] {
        if (canDeferCleanup && data)
            deferredCleanup->data = std::move(data);
    });
    auto *d = m_core.load(std::memory_order_acquire);
    if ((deferredCleanup && !canDeferCleanup) || !data || !d
        || data->owner != d->publicationOwner || data->kind != kind
        || data->minimumEpoch.isValid() == mutation
        || !(data->transaction == transaction)
        || (!mutation && (!epoch.isValid() || epoch.value < data->minimumEpoch.value))
        || (mutation && epoch.isValid())) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata publication capability is stale or foreign"));
        recordRejectedInstall();
        return false;
    }
    qsizetype claimed = 0;
    // All claims are checked before changing any state. Never hold multiple
    // shard locks; a competing mutation is rejected/deferred while claimed.
    // Claims live only inside this synchronous call, with no provider work,
    // root construction, callback or external completion wait inside the
    // claim interval. Individual shard mutex acquisition can still wait.
    for (size_t first = 0; first < data->entries.size();) {
        const size_t end = data->batchEnd(first, data->entries.size());
        QMutexLocker locker(&data->entries[first].shardOwner->mutex);
        for (size_t i = first; i < end; ++i) {
            auto &entry = data->entries[i];
            auto page = entry.originalPage;
            if (!entry.shardOwner->canMutate(entry.key(), page->second))
                break;
            if (page->second.revision != entry.revision)
                break;
            Q_ASSERT(entry.activityReservation);
            // Keep the existing reservation until this Data is cleaned outside
            // installation. Clearing the transient claim cannot then erase/free
            // an otherwise empty activity node under the publication gate.
            page->second.activity.publicationClaim = data.get();
            ++claimed;
        }
        if (claimed != qsizetype(end)) break;
        first = end;
    }
    if (claimed != qsizetype(data->entries.size())) {
        // No page was edited. Release every claim already acquired.
        for (size_t first = 0; first < size_t(claimed);) {
            const size_t end = data->batchEnd(first, size_t(claimed));
            QMutexLocker locker(&data->entries[first].shardOwner->mutex);
            for (size_t i = first; i < end; ++i)
                data->entries[i].originalPage->second.activity.publicationClaim = nullptr;
            first = end;
        }
        KisPageStoreDetail::setError(error, QStringLiteral("metadata changed after publication preparation"));
        recordRejectedInstall();
        return false;
    }
    Q_ASSERT(data->publications.empty() || data->publications.size() == data->entries.size());
    for (size_t first = 0; first < data->entries.size();) {
        const size_t end = data->batchEnd(first, data->entries.size());
        QMutexLocker locker(&data->entries[first].shardOwner->mutex);
        for (size_t i = first; i < end; ++i) {
            auto &entry = data->entries[i];
            auto *publication = &data->publications[i];
            auto page = entry.originalPage;
            Q_ASSERT(page->second.activity.publicationClaim == data.get());
            quint64 installedHistoryLinks = 0;
            if (!mutation) {
                installedHistoryLinks +=
                    quint64(std::count_if(publication->deltas.cbegin(), publication->deltas.cend(), [](const auto &delta) {
                        return !delta.detach && delta.publication == KisPagePublicationState::Historical;
                    }));
                if (publication->generic)
                    installedHistoryLinks +=
                        quint64(std::count_if(publication->generic->installRecords.cbegin(), publication->generic->installRecords.cend(), [](const auto &addition) {
                            return addition.publication == KisPagePublicationState::Historical;
                        }));
            }
            // Epoch allocation may have skipped an identity on an earlier failed
            // root build. These transitions use the epoch only as a newer tag.
            KisVersionSlotId acceptedNativeSlot;
            if (recoverable) {
                auto &reservation = data->arenaGrowth.at(entry.arenaGrowthIndex).reservations;
                for (const auto &record : publication->generic->installRecords) {
                    const bool installed = entry.shardOwner->records.putReserved(&page->second, record, &reservation);
                    Q_ASSERT(installed);
                    Q_UNUSED(installed);
                }
                const bool headerInstalled = entry.shardOwner->installReservedHeader(entry.key(), &page->second, publication->generic->next);
                Q_ASSERT(headerInstalled);
                Q_UNUSED(headerInstalled);
                ++page->second.revision;
                ++entry.shardOwner->publicationAdditionRecordsTransferred;
                entry.shardOwner->publicationVersionInstalls += publication->generic->installRecords.size();
            } else {
                if (!privateWrite && publication->generic) publication->generic->next.publishedEpoch = epoch;
                if (!publication->generic && entry.native) {
                    auto &records = entry.shardOwner->records;
                    if (entry.reuseNativeSlots) {
                        const auto previous = records.version(page->second.publishedVersion)->version;
                        acceptedNativeSlot = records.replaceUnused(&page->second, previous, *entry.native,
                            &entry.releasedDescriptor, &entry.releasedReadBinding, KisPagePublicationState::Published);
                        ++entry.shardOwner->nativeSlotReplacements;
                    } else {
                        auto &reservation = data->arenaGrowth.at(entry.arenaGrowthIndex).reservations;
                        acceptedNativeSlot = records.putNativeReserved(&page->second, *entry.native, &reservation,
                            KisPagePublicationState::Published);
                        ++entry.shardOwner->publicationAdditionRecordsTransferred;
                    }
                    ++entry.shardOwner->publicationVersionInstalls;
                }
                for (const auto &delta : std::as_const(publication->deltas)) {
                    if (delta.detach) {
                        if (entry.reuseNativeSlots) continue; // The old value was replaced in its original storage.
                        Q_ASSERT(entry.releaseUnusedCapacity
                            && entry.shardOwner->records.canDetachPublished(delta.version));
                        entry.shardOwner->records.remove(&page->second, delta.version,
                            &entry.releasedDescriptor, &entry.releasedReadBinding);
                        continue;
                    }
                    const bool updated = entry.shardOwner->records.setPublication(&page->second,
                                                                             delta.version,
                                                                             delta.publication,
                                                                             delta.preparedBy);
                    Q_ASSERT(updated);
                    Q_UNUSED(updated);
                }
                if (!publication->generic) {
                    if (!acceptedNativeSlot.isValid()) {
                        const bool found = entry.shardOwner->records.findVersion(entry.version, &acceptedNativeSlot);
                        Q_ASSERT(found); Q_UNUSED(found);
                    }
                    // CommitTransaction changes exactly this published head.
                    // Current writer, handoff, next generation and protection
                    // remain authoritative; the full-D revision claim holds.
                    page->second.publishedVersion = acceptedNativeSlot;
                    page->second.publishedEpoch = epoch;
                } else {
                    const auto additionRecords = quint64(publication->generic->installRecords.size());
                    auto &reservation = data->arenaGrowth.at(entry.arenaGrowthIndex).reservations;
                    const bool additionsInstalled = entry.shardOwner->records.installPreparedAdditionsReserved(&page->second,
                                                                                                          &publication->generic->installRecords,
                                                                                                          &reservation);
                    Q_ASSERT(additionsInstalled);
                    Q_UNUSED(additionsInstalled);
                    if (privateWrite && publication->generic->next.writer.baseAuthority.isValid()) {
                        const auto &writer = publication->generic->next.writer;
                        KisVersionSlotId base;
                        KisReplicaSlotId authority;
                        const bool found = entry.shardOwner->records.findVersion(writer.baseVersion, &base)
                            && entry.shardOwner->records.findReplicaSlot(base, writer.baseAuthority, &authority);
                        Q_ASSERT(found); Q_UNUSED(found);
                        // AcquireWrite already checked overflow. No intervening
                        // reader can pass the revision claim above; update only
                        // its pin, preserving every unrelated protection node.
                        ++entry.shardOwner->records.replica(authority)->pinCount;
                    }
                    entry.shardOwner->publicationAdditionRecordsTransferred += additionRecords;
                    const bool headerInstalled = entry.shardOwner->installReservedHeader(entry.key(), &page->second, publication->generic->next);
                    Q_ASSERT(headerInstalled);
                    Q_UNUSED(headerInstalled);
                    entry.shardOwner->publicationVersionInstalls += additionRecords;
                }
                ++page->second.revision;
                entry.shardOwner->publicationVersionInstalls += quint64(publication->deltas.size());
            }
            if (entry.native) {
                auto *native = entry.native;
                if (!acceptedNativeSlot.isValid()) {
                    const bool found = entry.shardOwner->records.findVersion(native->target.version, &acceptedNativeSlot);
                    Q_ASSERT(found); Q_UNUSED(found);
                }
                entry.shardOwner->acceptNativeResult(page->second, acceptedNativeSlot, *native);
            }
            entry.shardOwner->publicationHistoryNodesTransferred += installedHistoryLinks;
            ++(mutation ? entry.shardOwner->installedMutationPages
                        : entry.shardOwner->installedPublicationPages);
            // All page facts were claimed before the first install; after this
            // page's final write no later step can overwrite a new reader/last-use
            // mutation here. Release under the same lock, not a third lock pass.
            page->second.activity.publicationClaim = nullptr;
        }
        first = end;
    }
    // Arena capacity was admitted for the whole shard batch. Reclaim only
    // after every page consumed its slots; per-page reclaim could detach an
    // empty block reserved for a later claimed page.
    for (auto &growth : data->arenaGrowth) {
        QMutexLocker locker(&growth.shardOwner->mutex);
        growth.reservations.cancel(&growth.shardOwner->records);
        growth.releasedBlocks = growth.shardOwner->arenas.takeEmptyBlocks(
            &growth.shardOwner->budgetCharge);
    }
    data->installed = true;
    data->owner.reset();
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPageMetadataCoordinator::KisPageMetadataCoordinator() = default;

KisPageMetadataCoordinator::~KisPageMetadataCoordinator()
{
    Private::destroy(m_core.exchange(nullptr, std::memory_order_acq_rel));
}

void KisPageMetadataCoordinator::attachBackingBudget(
    KisBackingBudgetController &budget)
{
    QMutexLocker locker(&m_configurationMutex);
    Q_ASSERT(!m_core.load(std::memory_order_relaxed));
    m_budget = &budget;
}

void KisPageMetadataCoordinator::attachRetirementDebtOwner(
    void *context,
    PrepareRetirementDebt prepare,
    CommitRetirementEffects commit,
    FinalizeRetirementDebt cancel)
{
    QMutexLocker locker(&m_configurationMutex);
    Q_ASSERT(!m_core.load(std::memory_order_relaxed));
    Q_ASSERT(context && prepare && commit && cancel);
    Q_ASSERT(!m_retirementDebtContext);
    m_retirementDebtContext = context;
    m_prepareRetirementDebt = prepare;
    m_commitRetirementDebt = commit;
    m_cancelRetirementDebt = cancel;
}

KisCpuReadBinding
KisPageMetadataCoordinator::installCpuReadBinding(const KisReplicaHandle &replica,
                                                  std::shared_ptr<KisCpuResidentBinding> binding)
{
    if (!binding || !replica.isValid()
        || (replica.domain != KisPageAccessDomain::CpuRam && replica.domain != KisPageAccessDomain::UmaShared))
        return {};
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), replica.version.key);
    if (!shard)
        return {};
    QMutexLocker locker(&shard->mutex);
    // Provider lookup happened outside this lock. Revalidate the exact
    // replica before publishing a cache entry; never resurrect retired data.
    const auto *version = readableVersion(*shard, replica.version);
    KisReplicaSlotId slot;
    if (!version || !shard->records.findReplicaSlot(version->slot, replica, &slot)
        || !cpuReadableReplica(*shard->records.replica(slot), replica.version))
        return {};
    auto *facts = shard->arenas.facts.get(version->facts);
    if (!facts->nativeBinding) ++shard->arenas.cpuReadBindings;
    facts->nativeBinding.swap(binding);
    facts->nativeReplica = slot;
    KisCpuReadBinding result{replica, facts->nativeBinding};
    locker.unlock(); // The replaced physical object may be destroyed here.
    return result;
}

KisPageRetirementRecord *KisPageMetadataCoordinator::exchangeIdleCapacity(
    const KisPageKey &key, KisPageRetirementRecord *next)
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), key);
    if (!shard) { Q_ASSERT(!next); return nullptr; }
    QMutexLocker lock(&shard->mutex);
    auto page = shard->pages.find(key);
    if (page == shard->pages.end()) {
        if (next) qFatal("Idle index requires the original registered page");
        return nullptr;
    }
    return std::exchange(page->second.idleCapacity, next);
}

namespace {
KisReplicaHandle cpuReadReplicaLocked(const MetadataShard &shard, const KisVersionRecord &version)
{
    // Prefer a ready CPU authority; otherwise require one unambiguous exact
    // CPU replica. This selection never requests materialization.
    const auto *authority = shard.records.replica(version.authorityReplica);
    if (authority && cpuReadableReplica(*authority, version.version)) return replicaHandle(*authority, version.version);
    KisReplicaHandle selected;
    for (auto slot = version.firstReplica; slot.isValid();) {
        const auto *candidate = shard.records.replica(slot);
        slot = candidate->nextReplica;
        if (!cpuReadableReplica(*candidate, version.version)) continue;
        if (selected.isValid()) return {};
        selected = replicaHandle(*candidate, version.version);
    }
    return selected;
}

KisCpuReadBinding cachedCpuReadBindingLocked(const MetadataShard &shard, const KisVersionRecord &version)
{
    const auto *facts = shard.arenas.facts.get(version.facts);
    const auto *replica = shard.records.replica(facts->nativeReplica);
    return replica && cpuReadableReplica(*replica, version.version)
        ? KisCpuReadBinding{replicaHandle(*replica, version.version), facts->nativeBinding} : KisCpuReadBinding{};
}

KisCpuReadBinding selectCpuReadBindingLocked(const MetadataShard &shard, const KisVersionRecord &version)
{
    auto selected = cachedCpuReadBindingLocked(shard, version);
    // Keep one return carrier, including the cold case. An empty binding is
    // still a completed exact replica selection; it must not lose its source.
    if (!selected.binding) selected.replica = cpuReadReplicaLocked(shard, version);
    return selected;
}

void takeCpuReadBindingLocked(MetadataShard &shard, const KisVersionRecord &version,
    const KisReplicaHandle &replica, const KisCpuResidentBinding *expected,
    std::shared_ptr<KisCpuResidentBinding> *released)
{
    auto *facts = shard.arenas.facts.get(version.facts);
    const auto *selected = shard.records.replica(facts->nativeReplica);
    if (facts->nativeBinding && (!selected || replicaHandle(*selected, replica.version) == replica)
        && (!expected || facts->nativeBinding.get() == expected)) {
        released->swap(facts->nativeBinding);
        facts->nativeReplica = {};
        --shard.arenas.cpuReadBindings;
    }
}
}

void KisPageMetadataCoordinator::removeCpuReadBinding(const KisReplicaHandle &replica,
                                                      const KisCpuResidentBinding *expected)
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), replica.version.key);
    if (!shard) return;
    std::shared_ptr<KisCpuResidentBinding> released;
    QMutexLocker locker(&shard->mutex);
    KisVersionSlotId slot;
    if (!shard->records.findVersion(replica.version, &slot)) return;
    takeCpuReadBindingLocked(*shard, *shard->records.version(slot), replica, expected, &released);
} // Any last physical reference leaves after the original shard gate.

KisCpuReadBinding KisPageMetadataCoordinator::cpuReadBinding(const KisPageVersion &version) const
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), version.key);
    if (!shard) return {};
    QMutexLocker locker(&shard->mutex);
    const auto *record = readableVersion(*shard, version);
    return record ? cachedCpuReadBindingLocked(*shard, *record) : KisCpuReadBinding{};
}

KisCpuReadBinding KisPageMetadataCoordinator::selectCpuReadBinding(
    const KisPageVersion &version, const KisCpuReadBinding &stale)
{
    if (stale && !(stale.replica.version == version)) return {};
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), version.key);
    if (!shard) return {};
    std::shared_ptr<KisCpuResidentBinding> released;
    QMutexLocker lock(&shard->mutex);
    KisVersionSlotId slot;
    if (shard->pages.find(version.key) == shard->pages.end()
        || !shard->records.findVersion(version, &slot)) return {};
    const auto *record = shard->records.version(slot);
    if (!record) return {};
    if (stale) takeCpuReadBindingLocked(*shard, *record, stale.replica, stale.binding.get(), &released);
    return readableVersionLocked(*shard, *record)
        ? selectCpuReadBindingLocked(*shard, *record) : KisCpuReadBinding{};
}

size_t KisPageMetadataCoordinator::retainNativeReads(KisPageNativeReadRef *references, size_t &next, size_t count,
    void *context, bool (*select)(void *, quint32, KisPageVersion *, KisVersionSlotId *),
    KisCpuReadBinding *firstBindings, size_t bindingCount)
{
    auto *core = m_core.load(std::memory_order_acquire);
    if (!core || !count || !references || !select || next == KisPageNativeReadRef::End
        || references[next].shard >= core->shards.size()) return 0;
    const auto shardIndex = references[next].shard;
    auto *shard = core->shards[shardIndex].get();
    QMutexLocker lock(&shard->mutex);
    size_t retained = 0;
    for (; retained < count && retained < 16 && next != KisPageNativeReadRef::End; ++retained) {
        auto &reference = references[next];
        KisPageVersion identity;
        KisVersionSlotId slot;
        if (reference.shard != shardIndex || !select(context, quint32(next), &identity, &slot)
            || !identity.isValid() || reference.version.isValid()
            || reference.shard != kisStablePageKeyHash(identity.key) % core->shards.size()) break;
        if (identity.isDefaultPixel()) { next = reference.next; continue; }
        auto page = shard->pages.find(identity.key);
        if (page == shard->pages.end() || !shard->canMutate(identity.key, page->second)
            || (!slot.isValid() && !shard->records.findVersion(identity, &slot))) break;
        const auto *version = shard->records.version(slot);
        if (!version || !(version->version == identity)) break;
        auto *facts = shard->arenas.facts.get(version->facts);
        if (version->publication == KisPagePublicationState::Unpublished
            || version->publication == KisPagePublicationState::Retiring
            || facts->nativeReaders == std::numeric_limits<quint32>::max()) break;
        ++facts->nativeReaders;
        ++page->second.revision;
        reference.version = slot;
        if (next < bindingCount) {
            Q_ASSERT(firstBindings && !firstBindings[next]);
            firstBindings[next] = selectCpuReadBindingLocked(*shard, *version);
        }
        next = reference.next;
    }
    return retained;
}

size_t KisPageMetadataCoordinator::releaseNativeReads(const KisPageNativeReadRef *references,
    size_t &next, size_t count, KisPageKey *revisit)
{
    auto *core = m_core.load(std::memory_order_acquire);
    if (!core || !count || !references || !revisit || next == KisPageNativeReadRef::End
        || references[next].shard >= core->shards.size()) return 0;
    const auto shardIndex = references[next].shard;
    auto *shard = core->shards[shardIndex].get();
    QMutexLocker lock(&shard->mutex);
    size_t released = 0;
    for (; released < count && released < 16 && next != KisPageNativeReadRef::End; ++released) {
        const auto &reference = references[next];
        if (reference.shard != shardIndex) break;
        revisit[released] = {};
        if (!reference.version.isValid()) { next = reference.next; continue; }
        const auto *version = shard->records.version(reference.version);
        if (!version) break;
        auto page = shard->pages.find(version->version.key);
        if (page == shard->pages.end() || !shard->canMutate(version->version.key, page->second)) break;
        auto *facts = shard->arenas.facts.get(version->facts);
        if (!facts->nativeReaders) break;
        if (!--facts->nativeReaders && page->second.firstHistory.isValid()) revisit[released] = version->version.key;
        ++page->second.revision;
        next = reference.next;
    }
    return released;
}

KisPageVersion KisPageMetadataCoordinator::nativeReadVersion(const KisPageNativeReadRef &reference,
    KisCpuReadBinding *binding) const
{
    const auto *core = m_core.load(std::memory_order_acquire);
    if (!core || reference.shard >= core->shards.size() || !reference.version.isValid()) return {};
    const auto *shard = core->shards[reference.shard].get();
    QMutexLocker lock(&shard->mutex);
    const auto *version = shard->records.version(reference.version);
    if (!version) return {};
    const auto *facts = shard->arenas.facts.get(version->facts);
    if (!facts->nativeReaders) return {};
    if (binding) *binding = selectCpuReadBindingLocked(*shard, *version);
    return version->version;
}

void KisPageMetadataCoordinator::nativeReadBindings(const KisPageNativeReadRef *references,
    quint32 *indices, size_t first, size_t count, KisCpuReadBinding *bindings) const
{
    Q_ASSERT(count <= 16);
    const auto *core = m_core.load(std::memory_order_acquire);
    if (!core) return;
    for (size_t i = 0; i < count; ++i) indices[i] = quint32(first + i);
    std::sort(indices, indices + count, [&](quint32 a, quint32 b) { return references[a].shard < references[b].shard; });
    for (size_t i = 0; i < count;) {
        const auto shardIndex = references[indices[i]].shard;
        if (shardIndex >= core->shards.size()) return;
        const auto *shard = core->shards[shardIndex].get();
        QMutexLocker lock(&shard->mutex);
        do {
            const auto index = indices[i++];
            Q_ASSERT(!bindings[index - first]);
            const auto *version = shard->records.version(references[index].version);
            if (version && shard->arenas.facts.get(version->facts)->nativeReaders)
                bindings[index - first] = selectCpuReadBindingLocked(*shard, *version);
        } while (i < count && references[indices[i]].shard == shardIndex);
    }
}

KisReplicaHandle KisPageMetadataCoordinator::cpuReadReplica(const KisPageVersion &version) const
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), version.key);
    if (!shard) return {};
    QMutexLocker locker(&shard->mutex);
    const auto *record = readableVersion(*shard, version);
    return record ? cpuReadReplicaLocked(*shard, *record) : KisReplicaHandle{};
}

bool KisPageMetadataCoordinator::configure(qsizetype shardCount, QString *error)
{
    if (shardCount <= 0 || shardCount > 4096) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata shard count is outside the supported range"));
        return false;
    }

    QMutexLocker locker(&m_configurationMutex);
    if (m_core.load(std::memory_order_relaxed)) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata coordinator is already configured"));
        return false;
    }
    try {
        auto *budget = m_budget;
        std::shared_ptr<KisBackingBudgetController> standalone;
        if (!budget) {
            const auto parent = kisAcquirePageStoreBootstrapBudget(error);
            if (!parent) return false;
            standalone = std::allocate_shared<KisBackingBudgetController>(
                KisMutationStorageAllocator<KisBackingBudgetController>{});
            if (!standalone->configureSharedNonPayloadBudget(parent, error)) return false;
            budget = standalone.get();
        }
        auto storage = Private::Storage::retained(budget);
        auto *raw = storage.allocate(1);
        try { std::allocator_traits<Private::Storage>::construct(storage, raw, std::move(standalone), storage); }
        catch (...) { storage.deallocate(raw, 1); throw; }
        std::unique_ptr<Private, void (*)(Private *)> candidate(raw, &Private::destroy);
        candidate->budgetAuthority = std::allocate_shared<MetadataBudgetAuthority>(storage, budget);
        candidate->shards.reserve(size_t(shardCount));
        for (qsizetype i = 0; i < shardCount; ++i) {
            candidate->shards.push_back(std::allocate_shared<MetadataShard>(
                storage, candidate->budgetAuthority, storage));
        }
        candidate->publicationOwner = std::allocate_shared<const quint8>(storage, 0);
        // Publish the complete original core only after every allocation has
        // succeeded. Refusal destroys the same candidate, including its body.
        m_core.store(candidate.release(), std::memory_order_release);
    } catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata configuration storage budget was refused"));
        return false;
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPageMetadataCoordinator::isOperational() const
{
    return m_core.load(std::memory_order_acquire) != nullptr;
}

qsizetype KisPageMetadataCoordinator::shardCount() const
{
    const auto *d = m_core.load(std::memory_order_acquire);
    return d ? qsizetype(d->shards.size()) : 0;
}

qsizetype KisPageMetadataCoordinator::shardFor(const KisPageKey &key) const
{
    const auto *d = m_core.load(std::memory_order_acquire);
    if (!d || !key.isValid())
        return -1;
    return qsizetype(kisStablePageKeyHash(key) % d->shards.size());
}

bool KisPageMetadataCoordinator::registerPage(const KisPageStateSnapshot &initial, QString *error) try
{
    if (initial.nativeGeneration.isValid() || initial.nativeTransaction.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("native reservations require their original operation"));
        return false;
    }
    KisPageStateMachine stateMachine;
    QString invariantFailure;
    if (!stateMachine.validateInvariants(initial, &invariantFailure)) {
        KisPageStoreDetail::setError(error, invariantFailure);
        return false;
    }

    auto *d = m_core.load(std::memory_order_acquire);
    MetadataShard *shard = Private::shardFor(d, initial.key);
    if (!shard) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata coordinator is not configured"));
        return false;
    }
    const MetadataArenaDemand demand = newMetadataDemand(initial.versions);
    const auto storageOwner = shard->budgetAuthority->storage<char>();
    MetadataArenaGrowth growth(storageOwner.borrowed());
    QMutexLocker locker(&shard->mutex);
    if ((shard->pages.find(initial.key) != shard->pages.end())) {
        KisPageStoreDetail::setError(error, QStringLiteral("page is already registered"));
        return false;
    }
    const auto storage = growMetadataArenasOutsideLock(shard, demand, &growth, &locker, [&] {
        return !(shard->pages.find(initial.key) != shard->pages.end());
    }, error);
    if (storage != MetadataGrowthResult::Ready) {
        KisPageStoreDetail::setError(error,
                 storage == MetadataGrowthResult::Stale
                     ? QStringLiteral("page was registered while preparing metadata storage")
                     : QStringLiteral("metadata arena budget is exhausted"));
        return false;
    }
    decltype(shard->pages) prepared(shard->pages.get_allocator());
    prepared.try_emplace(initial.key);
    if (!shard->ensureActivityCapacity(initial.key, &initial, error)) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata owning capacity budget is exhausted"));
        return false;
    }
    const auto discardActivity = qScopeGuard([&] {
        const auto activity = shard->activities.find(initial.key);
        if (activity != shard->activities.end() && activity->second.isEmpty()) shard->activities.erase(activity);
    });
    auto &page = prepared.begin()->second;
    if (!shard->assign(initial.key, &page, initial)) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata arena budget is exhausted"));
        return false;
    }
    shard->pages.insert(prepared.extract(prepared.begin()));
    d->registeredPages.fetchAndAddRelease(1);
    KisPageStoreDetail::setError(error, {});
    return true;
}
catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("metadata registration storage preparation was refused"));
    return false;
}

quint64 KisPageMetadataCoordinator::pageRegistrationCount() const
{
    const auto *d = m_core.load(std::memory_order_acquire);
    return d ? d->registeredPages.loadAcquire() : 0;
}

bool KisPageMetadataCoordinator::pageSnapshot(const KisPageKey &key, KisPageStateSnapshot *snapshot) const
{
    MetadataShard *shard = Private::shardFor(m_core.load(std::memory_order_acquire), key);
    if (!shard || !snapshot)
        return false;
    QMutexLocker locker(&shard->mutex);
    const auto pageIt = shard->pages.find(key);
    if (pageIt == shard->pages.cend())
        return false;
    *snapshot = shard->snapshot(key, pageIt->second);
    return true;
}

KisPageMetadataCoordinator::MutationBaseSelection
KisPageMetadataCoordinator::selectMutationBase(const KisPageVersion &base, bool discoverBefore,
    KisPageWriteSelection *selection, QString *error, const KisVersionSlotId *knownSlot) const
{
    const auto reject = [&](const QString &reason) {
        KisPageStoreDetail::setError(error, reason);
        return MutationBaseSelection::Rejected;
    };
    auto *core = m_core.load(std::memory_order_acquire);
    if (!core || !base.isValid() || !selection)
        return reject(QStringLiteral("write base identity is invalid"));
    const size_t shardIndex = kisStablePageKeyHash(base.key) % core->shards.size();
    auto *shard = core->shards[shardIndex].get();
    QMutexLocker lock(&shard->mutex);
    const auto page = shard->pages.find(base.key);
    if (page == shard->pages.cend()) {
        KisPageStoreDetail::setError(error, QStringLiteral("write base metadata is absent"));
        return MutationBaseSelection::Absent;
    }
    KisVersionSlotId selectedSlot = knownSlot ? *knownSlot : KisVersionSlotId{};
    const auto *record = selectedSlot.isValid() || shard->records.findVersion(base, &selectedSlot)
        ? shard->records.version(selectedSlot) : nullptr;
    if (!record) {
        KisPageStoreDetail::setError(error, QStringLiteral("write base version is absent"));
        return MutationBaseSelection::Absent;
    }
    if (!(record->version == base))
        return reject(QStringLiteral("write base location selects another version"));
    ++shard->mutationBaseVersionInputs;
    if (kisOnPageStoreReclamationThread()) ++shard->backgroundMutationBaseVersionInputs;
    if (page->second.activity.hasWriter())
        return reject(QStringLiteral("write page already has a writer"));
    const KisPageVersion target{base.key, page->second.nextGeneration};
    if (!target.isValid()) return reject(QStringLiteral("write generation is unavailable"));
    const auto *authority = shard->records.replica(record->authorityReplica);
    const bool virtualDefault = base.isDefaultPixel() && !record->replicaCount && !authority
        && (record->publication == KisPagePublicationState::Published
            || record->publication == KisPagePublicationState::Historical);
    if (!authority && !virtualDefault)
        return reject(QStringLiteral("write base has no exact authority"));

    // There is one output carrier. Actual source and accepted-location facts
    // are created in it, not copied through a second metadata projection.
    Q_ASSERT(!selection->write.baseVersion.isValid() && !selection->layout
        && !selection->binding && !selection->surfaceLayout);
    auto &write = selection->write;
    write.baseVersion = base;
    write.version = target;
    write.baseSlot = selectedSlot;
    write.baseLocation = {shard, &*page, shardIndex};
    if (authority) {
        write.source = replicaHandle(*authority, base);
        if (!write.source.isValid())
            return reject(QStringLiteral("write base has no exact authority"));
    }
    const auto *facts = shard->arenas.facts.get(record->facts);
    selection->layout = facts->descriptor;
    if (authority && facts->nativeReplica == record->authorityReplica)
        selection->binding = facts->nativeBinding;
    if (discoverBefore && !base.isDefaultPixel() && record->replicaCount > 1
        && authority && authority->validity == KisReplicaValidity::Valid
        && !authority->activeOperation.isValid() && !authority->pinCount
        && !authority->overflow.isValid()) {
        const auto &layout = write.source.layout;
        for (auto slot = record->firstReplica; slot.isValid();) {
            const auto *replica = shard->records.replica(slot);
            // Compare the original record. Only the chosen before-image
            // needs a full handle for the later real physical acquisition.
            if (replica->validity == KisReplicaValidity::Valid && !replica->activeOperation.isValid()
                && replica->domain == KisPageAccessDomain::CpuRam
                && replica->layoutRevision == layout.layoutRevision && replica->formatId == layout.formatId
                && replica->pageExtent == layout.pageExtent && replica->validRect == layout.validRect
                && replica->rowStride == layout.rowStride && replica->byteSize == layout.byteSize
                && !(replica->identity.provider == authority->identity.provider
                    && replica->identity.providerEpoch == authority->identity.providerEpoch
                    && replica->identity.allocation.slot == authority->identity.allocation.slot)) {
                selection->recoverableBefore = replicaHandle(*replica, base);
                break;
            }
            slot = replica->nextReplica;
        }
    }
    KisPageStoreDetail::setError(error, {});
    return MutationBaseSelection::Selected;
}

bool KisPageMetadataCoordinator::versionSnapshot(const KisPageVersion &identity, VersionInfo *snapshot,
                                                 ReplicaCandidates *replicas) const
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), identity.key);
    if (!shard || !snapshot) return false;
    for (;;) {
        QMutexLocker lock(&shard->mutex);
        const auto page = shard->pages.find(identity.key);
        if (page == shard->pages.cend()) return false;
        const auto *published = shard->records.version(page->second.publishedVersion);
        Q_ASSERT(published);
        KisVersionSlotId slot;
        const auto *version = shard->records.findVersion(identity, &slot) ? shard->records.version(slot) : nullptr;
        auto next = shard->records.versionInfo<VersionInfo>(version,
            published ? published->version.generation : KisPageGeneration{});
        if (version) {
            if (replicas) {
                if (replicas->capacity() < version->replicaCount) {
                    const auto count = version->replicaCount;
                    lock.unlock();
                    replicas->reserve(count); // Refusal precedes changing the output.
                    continue; // Re-select one complete shard cut after preparation.
                }
                replicas->clear();
                for (auto replicaSlot = version->firstReplica; replicaSlot.isValid();) {
                    const auto *replica = shard->records.replica(replicaSlot);
                    replicas->push_back({replicaHandle(*replica, identity), replica->validity});
                    replicaSlot = replica->nextReplica;
                }
            }
        } else {
            const auto *result = page->second.activity.nativeWrite;
            if (result && result->completed && result->target.version == identity) {
                if (replicas && replicas->capacity() < 1) {
                    lock.unlock(); replicas->reserve(1); continue;
                }
                next.version = identity;
                next.authority = result->target;
                next.publication = KisPagePublicationState::Prepared;
                next.preparedBy = page->second.activity.writerTransaction;
                next.replicaCount = 1;
                if (replicas) { replicas->clear(); replicas->push_back({result->target, KisReplicaValidity::Valid}); }
            } else if (replicas) replicas->clear();
        }
        *snapshot = next;
        return true; // Existing page and missing exact version remain distinct.
    }
}

namespace {
// Caller holds this shard's gate. All descriptor consumers share the same
// exact-version/native-result policy; the returned field is never exported.
std::shared_ptr<const KisPageAllocationDescriptor> *findVersionDescriptorLocked(
    MetadataShard *shard, const KisPageVersion &identity)
{
    KisVersionSlotId slot;
    if (shard->records.findVersion(identity, &slot))
        return &shard->arenas.facts.get(shard->records.version(slot)->facts)->descriptor;
    const auto *activity = shard->activity(identity.key);
    auto *native = activity ? activity->nativeWrite : nullptr;
    return native && native->target.version == identity ? &native->descriptor : nullptr;
}
}

std::shared_ptr<const KisPageAllocationDescriptor>
KisPageMetadataCoordinator::versionDescriptor(const KisPageVersion &identity) const
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), identity.key);
    if (!shard) return {};
    QMutexLocker lock(&shard->mutex);
    const auto *field = findVersionDescriptorLocked(shard, identity);
    return field ? *field : nullptr;
}

const KisPageAllocationDescriptor *
KisPageMetadataCoordinator::borrowVersionDescriptor(const KisPageVersion &identity) const
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), identity.key);
    if (!shard) return nullptr;
    QMutexLocker lock(&shard->mutex);
    const auto *field = findVersionDescriptorLocked(shard, identity);
    return field ? field->get() : nullptr;
}

bool KisPageMetadataCoordinator::versionDescriptorMatches(const KisPageVersion &identity,
    const KisPageAllocationDescriptor &desired) const
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), identity.key);
    if (!shard) return false;
    QMutexLocker lock(&shard->mutex);
    const auto *field = findVersionDescriptorLocked(shard, identity);
    return field && *field && **field == desired;
}

bool KisPageMetadataCoordinator::exchangeVersionDescriptor(const KisPageVersion &identity,
    std::shared_ptr<const KisPageAllocationDescriptor> *prepared)
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), identity.key);
    if (!shard || !prepared) return false;
    QMutexLocker lock(&shard->mutex);
    auto *field = findVersionDescriptorLocked(shard, identity);
    if (!field) return false;
    field->swap(*prepared);
    return true;
}

bool KisPageMetadataCoordinator::bindCpuProofIssuer(quint64 issuer, const KisCompletionTicket &completion)
{
    auto *d = m_core.load(std::memory_order_acquire);
    if (!d || !issuer || !completion.isValid()) return false;
    // Configuration owns this cold boundary; publish immutable completion
    // before the issuer becomes visible to readers.
    if (d->cpuProofIssuer.load())
        return d->cpuProofIssuer.load() == issuer && d->cpuCompletion == completion;
    d->cpuCompletion = completion;
    d->cpuProofIssuer.store(issuer);
    return true;
}

bool KisPageMetadataCoordinator::acceptPrivateTarget(const KisPageWriteInput &write,
    const KisReplicaHandle &source, const KisReplicaHandle &target, KisPagePrivateWrite *native, quint32 limit, QString *error,
    const std::shared_ptr<KisCpuResidentBinding> &before, bool complete)
{
    auto *core = m_core.load(std::memory_order_acquire);
    if (!core || !write.version.key.isValid() || core->shards.empty()) return false;
    const auto &location = write.baseLocation;
    const bool originalShard = location.node && location.shardIndex < core->shards.size()
        && location.shard == core->shards[location.shardIndex].get();
    size_t shardIndex = originalShard ? location.shardIndex
        : kisStablePageKeyHash(write.version.key) % core->shards.size();
    auto *shard = core->shards[shardIndex].get();
    if (!shard || !native || native->reserved || !target.isValid()
        || !(write.baseVersion.key == write.version.key)
        || !(target.version == write.version) || !write.transaction.isValid()
        || !write.operation.isValid()) return false;
    QMutexLocker lock(&shard->mutex);
    KisVersionSlotId baseSlot = write.baseSlot;
    auto *base = shard->records.version(baseSlot);
    MetadataPage *page = nullptr;
    auto *node = static_cast<decltype(shard->pages)::value_type *>(location.node);
    if (originalShard && base && base->version == write.baseVersion
        && node->first == write.version.key) {
        page = &node->second;
    } else {
        // Generic inputs and invalidated locators resolve the current page.
        // An old slot never selects a different base or bypasses its checks.
        const size_t actualShardIndex = originalShard
            ? kisStablePageKeyHash(write.version.key) % core->shards.size() : shardIndex;
        if (shardIndex != actualShardIndex) {
            lock.unlock();
            shardIndex = actualShardIndex;
            shard = core->shards[shardIndex].get();
            QMutexLocker rebound(&shard->mutex);
            lock.swap(rebound);
        }
        const auto found = shard->pages.find(write.version.key);
        if (found == shard->pages.end()) return false;
        page = &found->second;
        base = shard->records.version(baseSlot);
        if (!base || !(base->version == write.baseVersion)) {
            if (!shard->records.findVersion(write.baseVersion, &baseSlot)) return false;
            base = shard->records.version(baseSlot);
        }
    }
    KisPageVersion physicalOwner;
    if (!page->revisionCanAdvance()
        || page->activity.publicationClaim || page->activity.hasWriter()
        || !(write.version.generation == page->nextGeneration)
        || write.version.generation.value <= write.baseVersion.generation.value
        || write.version.generation.value == std::numeric_limits<quint64>::max()) return false;
    const bool occupied = shard->records.physicalOwner(target.physicalSlotIdentity(), &physicalOwner);
    if ((base->publication != KisPagePublicationState::Published
         && base->publication != KisPagePublicationState::Prepared)
        || (base->publication == KisPagePublicationState::Prepared && !(base->preparedBy == write.transaction))) return false;
    Q_ASSERT(page->versionCount > page->historyCount);
    if (page->versionCount <= page->historyCount
        || quint64(page->versionCount) - page->historyCount - 1 >= limit) return false;

    if (before) {
        // Reclaiming an authority needs no new metadata: its independent exact
        // before already exists, and the new private target lives in native.
        // All semantic and identity checks precede the first mutation below.
        if (!occupied || !(physicalOwner == write.baseVersion) || page->activity.genericHandoff) return false;
        const auto oldSlot = base->authorityReplica;
        auto *old = shard->records.replica(oldSlot);
        KisReplicaSlotId sourceSlot;
        if (!shard->records.findReplicaSlot(baseSlot, source, &sourceSlot) || sourceSlot == oldSlot) return false;
        const auto *sourceRecord = shard->records.replica(sourceSlot);
        if (!old || !sourceRecord || old->validity != KisReplicaValidity::Valid
            || sourceRecord->validity != KisReplicaValidity::Valid || old->activeOperation.isValid()
            || sourceRecord->activeOperation.isValid() || old->pinCount || old->overflow.isValid()) return false;
        const auto authority = replicaHandle(*old, base->version);
        if (!(authority.layout == source.layout) || !(authority.layout == target.layout)
            || authority.domain != target.domain
            || !(authority.physicalSlotIdentity() == target.physicalSlotIdentity())
            || authority.physicalSlotIdentity() == source.physicalSlotIdentity()
            || authority.allocation.generation == std::numeric_limits<quint64>::max()
            || target.allocation.generation != authority.allocation.generation + 1) return false;
        auto *link = &base->firstReplica;
        while (!(*link == oldSlot)) {
            auto *previous = shard->records.replica(*link);
            Q_ASSERT(previous); link = &previous->nextReplica;
        }
        *link = old->nextReplica;
        const bool unindexed = shard->records.physicalSlots.eraseExact(authority.physicalSlotIdentity(), oldSlot);
        const bool erased = shard->arenas.replicas.erase(oldSlot);
        Q_ASSERT(unindexed && erased); Q_UNUSED(unindexed); Q_UNUSED(erased);
        --base->replicaCount;
        base->authorityReplica = sourceSlot;
        auto *facts = shard->arenas.facts.get(base->facts);
        shard->arenas.cpuReadBindings -= bool(facts->nativeBinding);
        native->binding.swap(facts->nativeBinding); // Original result releases/reuses the old binding outside this gate.
        facts->nativeBinding = before;
        facts->nativeReplica = sourceSlot;
        ++shard->arenas.cpuReadBindings;
        ++shard->publicationVersionInstalls;
    } else {
        const auto *replica = shard->records.replica(base->authorityReplica);
        if (occupied || (!(shard->records.projectReplica(base->authorityReplica) == source)
                && !(base->version.isDefaultPixel() && !source.isValid()))
            || (!base->version.isDefaultPixel() && (!replica || replica->validity != KisReplicaValidity::Valid))) return false;
    }
    auto &activity = page->activity;
    activity.nativeWrite = native;
    activity.writerTransaction = write.transaction;
    native->target = target;
    native->source = source;
    native->reserved = true;
    native->completed = complete;
    native->location = {shard, page, shardIndex};
    ++shard->nativeWriteReservations;
    ++page->nextGeneration.value;
    ++page->revision;
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPagePrivateWrite *KisPageMetadataCoordinator::nativeWrite(const KisPageVersion &identity) const
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), identity.key);
    if (!shard) return nullptr;
    QMutexLocker lock(&shard->mutex);
    const auto *activity = shard->activity(identity.key);
    auto *native = activity ? activity->nativeWrite : nullptr;
    return native && native->target.version == identity ? native : nullptr;
}

KisPagePrivateWrite *KisPageMetadataCoordinator::nativeWriteForProof(const KisPreparedPageProof &proof) const
{
    auto *d = m_core.load(std::memory_order_acquire);
    const auto *shard = Private::shardFor(d, proof.authority.version.key);
    if (!shard || !proof.isValid() || !d->cpuProofIssuer.load()) return nullptr;
    QMutexLocker lock(&shard->mutex);
    const auto *activity = shard->activity(proof.authority.version.key);
    auto *native = activity ? activity->nativeWrite : nullptr;
    return native && native->reserved && native->completed && native->matchesProof(proof, d->cpuCompletion)
        ? native : nullptr;
}

bool KisPageMetadataCoordinator::completePrivateWrite(KisPagePrivateWrite *native)
{
    if (!native) return false;
    auto *shard = Private::registeredShard(m_core.load(std::memory_order_acquire), *native);
    if (!shard) return false;
    QMutexLocker lock(&shard->mutex);
    const auto &activity = static_cast<MetadataPage *>(native->location.page)->activity;
    if (activity.nativeWrite != native || !native->reserved) return false;
    native->completed = true;
    return true;
}

bool KisPageMetadataCoordinator::replacePrivateWrite(KisPagePrivateWrite *result,
    const KisReplicaHandle &target)
{
    if (!result || !target.isValid() || !(target.version == result->target.version)) return false;
    auto *shard = Private::registeredShard(m_core.load(std::memory_order_acquire), *result);
    if (!shard) return false;
    QMutexLocker lock(&shard->mutex);
    auto *page = static_cast<MetadataPage *>(result->location.page);
    auto *activity = &page->activity;
    KisPageVersion physicalOwner;
    if (!activity || activity->nativeWrite != result || !result->reserved || !result->completed
        || activity->publicationClaim || result->proof.isValid()
        || target.physicalSlotIdentity() == result->target.physicalSlotIdentity()
        || shard->records.physicalOwner(target.physicalSlotIdentity(), &physicalOwner)) return false;
    result->target = target;
    result->completed = true;
    ++page->revision;
    return true;
}

bool KisPageMetadataCoordinator::cancelPrivateResult(KisPagePrivateWrite *native)
{
    if (!native) return false;
    auto *shard = Private::registeredShard(m_core.load(std::memory_order_acquire), *native);
    if (!shard) return false;
    QMutexLocker lock(&shard->mutex);
    auto *page = static_cast<MetadataPage *>(native->location.page);
    if (page->activity.nativeWrite != native || page->activity.publicationClaim) return false;
    if (native->proof.isValid()) { --shard->arenas.cpuProofs; native->proof = {}; }
    page->activity.nativeWrite = nullptr;
    page->activity.writerTransaction = {};
    if (page->activity.isEmpty()) shard->activities.erase(native->target.version.key);
    native->reserved = false;
    native->location = {};
    ++page->revision;
    return true;
}

bool KisPageMetadataCoordinator::acceptPrivateValidation(KisPagePrivateWrite &result,
    KisPageTransactionId transaction, quint64 stamp, quint64 issuer, bool complete)
{
    auto *d = m_core.load(std::memory_order_acquire);
    if (!d || !issuer || d->cpuProofIssuer.load() != issuer || !stamp || !transaction.isValid()
        || !result.reserved || !result.location.shard || !result.location.page) return false;
    // The result's existing reservation retains this exact page and shard.
    // A copied value has no location and cannot complete the registered edit.
    auto *shard = Private::registeredShard(d, result);
    if (!shard) return false;
    QMutexLocker lock(&shard->mutex);
    const auto &activity = static_cast<MetadataPage *>(result.location.page)->activity;
    if (activity.nativeWrite != &result || (!result.completed && !complete)
        || result.proof.isValid() || !(activity.writerTransaction == transaction)) return false;
    result.completed = true;
    result.proof = {transaction, stamp};
    ++shard->arenas.cpuProofs;
    return true;
}

namespace {
// All CPU proof consumers use the same original fact and one shard window.
// This is a synchronous visit, not a retained location or validation cache.
template<class Core, class Visit>
bool visitCpuProof(Core *d, const KisPagePublicationIdentity &proof, quint64 issuer, Visit visit)
{
    auto *shard = Core::shardFor(d, proof.version.key);
    if (!shard || !issuer || d->cpuProofIssuer.load() != issuer || !proof.isValid()) return false;
    QMutexLocker lock(&shard->mutex);
    KisVersionSlotId slot = proof.metadataSlot;
    if (!slot.isValid() && !shard->records.findVersion(proof.version, &slot)) {
        const auto *activity = shard->activity(proof.version.key);
        auto *native = activity ? activity->nativeWrite : nullptr;
        if (!native || !native->reserved || !native->completed
            || !(native->target.version == proof.version)
            || native->proof.providerValidationStamp != proof.stamp
            || !(native->proof.transaction == proof.transaction)) return false;
        return visit(native->target, native->descriptor, native->binding, true,
            native->proof.providerValidationStamp, native->proof.transaction, shard->arenas.cpuProofs);
    }
    const auto *record = shard->records.version(slot);
    if (!record || !(record->version == proof.version)) return false;
    const auto *authority = shard->records.replica(record->authorityReplica);
    auto *facts = shard->arenas.facts.get(record->facts);
    if (!facts || facts->stamp != proof.stamp || !(facts->transaction == proof.transaction)
        || !authority) return false;
    const std::shared_ptr<KisCpuResidentBinding> unbound;
    const auto &binding = facts->nativeReplica == record->authorityReplica ? facts->nativeBinding : unbound;
    return visit(replicaHandle(*authority, record->version), facts->descriptor, binding,
        record->publication == KisPagePublicationState::Prepared && record->preparedBy == proof.transaction,
        facts->stamp, facts->transaction, shard->arenas.cpuProofs);
}
}

bool KisPageMetadataCoordinator::cpuPreparedAuthority(const KisPagePublicationIdentity &identity,
    quint64 issuer, KisReplicaHandle *authority,
    std::shared_ptr<const KisPageAllocationDescriptor> *descriptor,
    std::shared_ptr<KisCpuResidentBinding> *binding) const
{
    if (!authority || !descriptor || !binding) return false;
    *authority = {}; descriptor->reset(); binding->reset();
    return visitCpuProof(m_core.load(std::memory_order_acquire), identity, issuer,
        [&](const auto &original, const auto &layout, const auto &originalBinding,
            bool prepared, auto &, auto &, auto &) {
            if (!prepared || !layout) return false;
            *authority = original; *descriptor = layout; *binding = originalBinding;
            return true;
        });
}

KisPreparedPageProof KisPageMetadataCoordinator::cpuProof(const KisPagePublicationIdentity &proof,
    quint64 issuer) const
{
    auto *d = m_core.load(std::memory_order_acquire);
    KisPreparedPageProof result;
    visitCpuProof(d, proof, issuer, [&](const auto &authority, const auto &, const auto &,
                                      bool, auto &, auto &, auto &) {
        result.authority = authority;
        result.transaction = proof.transaction;
        result.providerValidationStamp = proof.stamp;
        result.producerCompletion = d->cpuCompletion;
        return true;
    });
    return result;
}

bool KisPageMetadataCoordinator::ownsCpuProof(const KisPreparedPageProof &proof, quint64 issuer) const
{
    auto *d = m_core.load(std::memory_order_acquire);
    if (!d || !proof.isValid() || !(d->cpuCompletion == proof.producerCompletion)) return false;
    return visitCpuProof(d, KisPagePublicationIdentity(proof), issuer,
        [&](const auto &authority, const auto &, const auto &, bool, auto &, auto &, auto &) {
            return authority == proof.authority;
        });
}

bool KisPageMetadataCoordinator::revokeCpuProof(const KisPreparedPageProof &proof, quint64 issuer)
{
    auto *d = m_core.load(std::memory_order_acquire);
    if (!d || !proof.isValid() || !(d->cpuCompletion == proof.producerCompletion)) return false;
    return visitCpuProof(d, KisPagePublicationIdentity(proof), issuer,
        [&](const auto &authority, const auto &, const auto &, bool, auto &stamp, auto &transaction, auto &count) {
            if (!(authority == proof.authority)) return false;
            stamp = 0; transaction = {}; --count;
            return true;
        });
}

bool KisPageMetadataCoordinator::revokeCpuProof(const KisPagePublicationIdentity &proof, quint64 issuer)
{
    return visitCpuProof(m_core.load(std::memory_order_acquire), proof, issuer,
        [](const auto &, const auto &, const auto &, bool, auto &stamp, auto &transaction, auto &count) {
            stamp = 0; transaction = {}; --count;
            return true;
        });
}

qsizetype KisPageMetadataCoordinator::cpuProofCount(quint64 issuer) const
{
    const auto *d = m_core.load(std::memory_order_acquire);
    if (!d || !issuer || d->cpuProofIssuer.load() != issuer) return 0;
    qsizetype result = 0;
    for (const auto &shard : d->shards) {
        QMutexLocker lock(&shard->mutex);
        result += shard->arenas.cpuProofs;
    }
    return result;
}

bool KisPageMetadataCoordinator::canAddTransientVersion(const KisPageVersion &target, quint32 limit) const
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), target.key);
    if (!shard)
        return limit > 0;
    QMutexLocker lock(&shard->mutex);
    const auto page = shard->pages.find(target.key);
    if (page == shard->pages.cend())
        return limit > 0;
    if (shard->records.findVersion(target))
        return true;
    quint64 native = 0;
    if (const auto *write = page->second.activity.nativeWrite) {
        if (write->target.version == target) return true;
        native = 1;
    }
    Q_ASSERT(page->second.versionCount > page->second.historyCount);
    if (page->second.versionCount <= page->second.historyCount) return false;
    const quint64 count = quint64(page->second.versionCount) - page->second.historyCount - 1 + native;
    return count < limit;
}

bool KisPageMetadataCoordinator::queryPublication(const KisPageKey &key,
                                                  const KisPageVersion &target,
                                                  PublicationInfo *info) const
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), key);
    if (!shard || !info || (target.isValid() && !(target.key == key)))
        return false;
    QMutexLocker lock(&shard->mutex);
    const auto page = shard->pages.find(key);
    if (page == shard->pages.cend())
        return false;
    const auto *head = shard->records.version(page->second.publishedVersion);
    Q_ASSERT(head);
    *info = {};
    info->current = shard->records.versionInfo<VersionInfo>(head, head->version.generation);
    if (target.isValid())
        info->target = target == head->version ? info->current
            : shard->records.versionInfo<VersionInfo>(target, head->version.generation);
    shard->publicationLookupVersionInputs += quint64(info->current.version.isValid())
        + quint64(info->target.version.isValid() && !(info->target.version == info->current.version));
    return true;
}

bool KisPageMetadataCoordinator::hasHistory(const KisPageKey &key) const
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), key);
    if (!shard) return false;
    QMutexLocker lock(&shard->mutex);
    const auto page = shard->pages.find(key);
    return page != shard->pages.cend() && page->second.historyCount != 0;
}

KisPageMetadataCoordinator::HistorySlice
KisPageMetadataCoordinator::historySlice(const KisPageKey &key, const KisPageVersion &after,
    KisMutationWorkspace::Array<KisPageVersion> &versions, qsizetype budget) const
{
    HistorySlice result;
    versions.clear();
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), key);
    if (!shard || budget <= 0 || !versions.capacity() || (after.isValid() && !(after.key == key)))
        return result;
    QMutexLocker lock(&shard->mutex);
    const auto page = shard->pages.find(key);
    if (page == shard->pages.cend())
        return result;
    result.exists = true;
    result.total = qsizetype(page->second.historyCount);
    KisVersionSlotId nextHint = page->second.firstHistory;
    if (after.isValid()) {
        bool resumedFromHint = false;
        KisVersionSlotId afterSlot;
        if (shard->records.findVersion(after, &afterSlot)) {
            const KisVersionRecord *record = shard->records.version(afterSlot);
            if (record && record->publication == KisPagePublicationState::Historical) {
                nextHint = record->nextHistory;
                resumedFromHint = true;
            }
        }
        if (!resumedFromHint) {
            while (nextHint.isValid()) {
                const KisVersionRecord *record = shard->records.version(nextHint);
                Q_ASSERT(record);
                if (!record || ShardRecordStore::position(after) < ShardRecordStore::position(record->version))
                    break;
                nextHint = record->nextHistory;
            }
        }
    }
    const size_t limit = std::min(versions.capacity(), size_t(qMin(budget, HistorySlice::Limit)));
    while (nextHint.isValid() && versions.size() < limit) {
        const KisVersionRecord *record = shard->records.version(nextHint);
        Q_ASSERT(record);
        if (!record)
            break;
        versions.push_back(record->version);
        result.after = record->version;
        nextHint = record->nextHistory;
    }
    if (!nextHint.isValid()) result.after = {};
    ++shard->historySliceQueries;
    shard->historySliceVersionInputs += quint64(versions.size());
    shard->maximumHistorySliceVersionInputs =
        qMax(shard->maximumHistorySliceVersionInputs, quint64(versions.size()));
    return result;
}

bool KisPageMetadataCoordinator::discardHistory(
    const KisPageKey &key, const KisPageVersion *versions, qsizetype count,
    quint32 reachableMask, quint32 *removedMask)
{
    *removedMask = 0;
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), key);
    if (!shard || count < 0 || count > HistorySlice::Limit) return false;
    for (;;) {
        const auto storageOwner = shard->budgetAuthority->storage<char>();
        // Keep the common retirement span inline without imposing a large
        // stack-probe call on every slice, including empty/protected inputs.
        using Effects = KisPageWorkingArray<KisPageTransitionEffect, 8>;
        Effects prepared{Effects::allocator_type(storageOwner.borrowed())};
        std::array<std::shared_ptr<const KisPageAllocationDescriptor>, HistorySlice::Limit> releasedDescriptors;
        std::array<std::shared_ptr<KisCpuResidentBinding>, HistorySlice::Limit> releasedBindings;
        std::array<KisVersionSlotId, HistorySlice::Limit> selectedSlots;
        MetadataArenas::ReleasedBlocks released;
        quint64 debtCookie = 0;
        bool debtPrepared = false;
        const auto cancelDebt = qScopeGuard([&] {
            if (debtPrepared) m_cancelRetirementDebt(m_retirementDebtContext, debtCookie);
        });
        QMutexLocker lock(&shard->mutex);
        auto page = shard->pages.find(key);
        if (page == shard->pages.end() || !shard->canMutate(key, page->second)) return false;
        quint32 selected = 0;
        size_t replicaCount = 0;
        for (qsizetype i = 0; i < count; ++i) {
            if (reachableMask & (quint32(1) << i)) continue;
            if (!versions[i].isValid() || !(versions[i].key == key)) return false;
            KisVersionSlotId slot;
            if (!shard->records.findVersion(versions[i], &slot)) continue;
            const auto *record = shard->records.version(slot);
            if (record->publication != KisPagePublicationState::Historical
                || record->capturedReadViews.isValid() || shard->arenas.facts.get(record->facts)->nativeReaders
                || (!record->authorityReplica.isValid()
                    && !(record->version.isDefaultPixel() && !record->replicaCount))) continue;
            bool eligible = true;
            for (auto r = record->firstReplica; r.isValid();) {
                const auto *replica = shard->records.replica(r);
                if ((replica->validity != KisReplicaValidity::Valid
                     && replica->validity != KisReplicaValidity::Failed)
                    || replica->overflow.isValid() || replica->pinCount
                    || replica->activeOperation.isValid()) { eligible = false; break; }
                r = replica->nextReplica;
            }
            if (!eligible) continue;
            // Identity enumeration never duplicates a record.
            for (qsizetype j = 0; j < i; ++j)
                if (versions[j] == versions[i]) return false;
            selected |= quint32(1) << i;
            selectedSlots[size_t(i)] = slot;
            replicaCount += record->replicaCount;
        }
        if (!selected) return true;
        prepared.reserve(replicaCount);
        for (qsizetype i = 0; i < count; ++i) {
            if (!(selected & (quint32(1) << i))) continue;
            const auto *record = shard->records.version(selectedSlots[size_t(i)]);
            for (auto r = record->firstReplica; r.isValid();) {
                const auto *replica = shard->records.replica(r);
                prepared.push_back({replicaHandle(*replica, record->version), {}});
                r = replica->nextReplica;
            }
        }
        const quint64 revision = page->second.revision;
        if (!prepared.empty()) {
            if (!m_prepareRetirementDebt) { ++shard->rejectedTransitions; return false; }
            lock.unlock();
            const bool accepted = m_prepareRetirementDebt(m_retirementDebtContext,
                prepared.data(), qsizetype(prepared.size()), &debtCookie, nullptr);
            debtPrepared = accepted;
            lock.relock();
            if (!accepted) { ++shard->rejectedTransitions; return false; }
            page = shard->pages.find(key);
            if (page == shard->pages.end() || !shard->canMutate(key, page->second)
                || page->second.revision != revision) continue;
        }
        // All storage and claims exist. Removal only unlinks original records.
        quint64 removed = 0;
        for (qsizetype i = 0; i < count; ++i) {
            if (!(selected & (quint32(1) << i))) continue;
            shard->records.remove(&page->second, versions[i], &releasedDescriptors[size_t(i)],
                                  &releasedBindings[size_t(i)], selectedSlots[size_t(i)]);
            ++removed;
        }
        ++page->second.revision;
        ++shard->localTransitionSequences;
        shard->localVersionInputs += removed;
        shard->localVersionRemovals += removed;
        if (kisOnPageStoreReclamationThread()) {
            ++shard->backgroundLocalTransitionSequences;
            shard->backgroundLocalVersionInputs += removed;
            shard->backgroundLocalVersionRemovals += removed;
        }
        ++shard->acceptedTransitions;
        *removedMask = selected;
        released = shard->arenas.takeEmptyBlocks(&shard->budgetCharge);
        lock.unlock();
        if (debtPrepared) {
            m_commitRetirementDebt(m_retirementDebtContext, debtCookie,
                                    prepared.data(), qsizetype(prepared.size()));
            debtPrepared = false;
        }
        return true;
    }
}

KisPageMetadataCoordinator::PublicationHeads KisPageMetadataCoordinator::publicationHeads() const
{
    const auto *d = m_core.load(std::memory_order_acquire);
    if (!d)
        return {};
    PublicationHeads result(d->budgetAuthority->storage<KisPageVersion>());
    for (const auto &shard : d->shards) {
        QMutexLocker lock(&shard->mutex);
        result.reserve(result.size() + shard->pages.size());
        for (auto page = shard->pages.cbegin(); page != shard->pages.cend(); ++page) {
            const auto *head = shard->records.version(page->second.publishedVersion);
            Q_ASSERT(head);
            result.push_back(head->version);
        }
        shard->publicationDirectoryHeaders += quint64(shard->pages.size());
    }
    return result;
}

namespace
{
bool directWriteSequence(const KisPageTransition *transitions, qsizetype count)
{
    if (!transitions) return false;
    using K = KisPageTransitionKind;
    if (count == 1)
        return transitions->kind == K::AcquireWrite
            || transitions->kind == K::PrepareWrite || transitions->kind == K::BeginPublish;
    return count == 2 && ((transitions[0].kind == K::AcquireWrite && transitions[1].kind == K::PrepareWrite)
        || (transitions[0].kind == K::BeginPublish && transitions[1].kind == K::PublishWrite));
}

bool localTransition(KisPageTransitionKind kind)
{
    using K = KisPageTransitionKind;
    switch (kind) {
    case K::AcquireWrite:
    case K::PrepareWrite:
    case K::BeginPublish:
    case K::PublishWrite:
    case K::FailWrite:
    case K::CancelWrite:
    case K::RetainCapturedVersion:
    case K::ReleaseCapturedVersion:
    case K::DetachPreparedVersion:
    case K::AbortPreparedVersion:
    case K::AbortTransaction:
    case K::DiscardHistoricalVersions:
    case K::BeginAuthorityHandoff:
    case K::CommitAuthorityHandoff:
    case K::FailAuthorityHandoff:
    case K::ReleaseRead:
    case K::AcquireRead:
    case K::AttachHistoricalDefault:
    case K::MaterializeDefault:
        return true;
    default:
        return false;
    }
}
} // namespace

KisPageMetadataReadCleanup::~KisPageMetadataReadCleanup() { clear(); }

KisPageMetadataReadCleanup::KisPageMetadataReadCleanup(KisPageMetadataReadCleanup &&other) noexcept
    : m_authority(std::move(other.m_authority))
    , m_bytes(std::exchange(other.m_bytes, 0))
    , m_blocks(std::move(other.m_blocks))
{
}

KisPageMetadataReadCleanup &KisPageMetadataReadCleanup::operator=(KisPageMetadataReadCleanup &&other) noexcept
{
    if (this != &other) {
        clear();
        m_authority = std::move(other.m_authority);
        m_bytes = std::exchange(other.m_bytes, 0);
        m_blocks = std::move(other.m_blocks);
    }
    return *this;
}

void KisPageMetadataReadCleanup::clear() noexcept
{
    m_blocks = {};
    const quint64 bytes = std::exchange(m_bytes, 0);
    auto authority = std::move(m_authority);
    if (bytes) authority->releaseLive(bytes);
}

KisPageMetadataTransitionResult KisPageMetadataCoordinator::applyOwner(const KisPageKey &key,
                                                               const KisPageTransition &transition,
                                                               KisPageMetadataReadCleanup *cleanup)
{
    if (transition.kind == KisPageTransitionKind::ReleaseRead)
        return applyReadProtection(key, transition, cleanup);
    if (transition.kind == KisPageTransitionKind::RetainCapturedVersion
        || transition.kind == KisPageTransitionKind::ReleaseCapturedVersion)
        return applyCapturedProtection(key, transition, cleanup);
    if (directWriteSequence(&transition, 1))
        return applyWriteSequence(key, &transition, 1);
    return applyProjectedSequence(key, &transition, 1);
}

KisPageMetadataTransitionResult KisPageMetadataCoordinator::applyOwnerSequence(const KisPageKey &key,
                                                                       const KisPageTransition *transitions,
                                                                       qsizetype count)
{
    if (transitions && count == 1 && transitions->kind == KisPageTransitionKind::ReleaseRead)
        return applyReadProtection(key, *transitions);
    if (transitions && count == 1 && (transitions->kind == KisPageTransitionKind::RetainCapturedVersion
                                    || transitions->kind == KisPageTransitionKind::ReleaseCapturedVersion))
        return applyCapturedProtection(key, *transitions);
    if (directWriteSequence(transitions, count))
        return applyWriteSequence(key, transitions, count);
    return applyProjectedSequence(key, transitions, count);
}

KisPageMetadataTransitionResult KisPageMetadataCoordinator::publishLeaseWrite(const KisPageTransition &write)
{
    auto begin = write;
    begin.kind = KisPageTransitionKind::BeginPublish;
    auto publish = begin;
    publish.kind = KisPageTransitionKind::PublishWrite;
    const KisPageTransition transitions[] = {begin, publish};
    return applyWriteSequence(write.version.key, transitions, 2, true);
}

KisPageMetadataTransitionResult KisPageMetadataCoordinator::applyWriteSequence(
    const KisPageKey &key, const KisPageTransition *transitions, qsizetype count, bool acceptPreparedRetry)
try {
    Q_ASSERT(directWriteSequence(transitions, count));
    KisPageMetadataTransitionResult result;
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), key);
    if (!shard) {
        result.rejectionReason = QStringLiteral("metadata coordinator is not configured");
        return result;
    }
    const auto &write = transitions[0];
    const bool addition = write.kind == KisPageTransitionKind::AcquireWrite;
    for (;;) {
        const auto storageOwner = shard->budgetAuthority->storage<char>();
        const auto storage = storageOwner.borrowed();
        MetadataArenaGrowth growth(storage);
        KisPageWorkingResult step(storage);
        KisPageWorkingArray<KisPageWorkingVersion, 1> additions{
            KisPageWorkingArray<KisPageWorkingVersion, 1>::allocator_type(storage)};
        QMutexLocker lock(&shard->mutex);
        auto page = shard->pages.find(key);
        if (page == shard->pages.end() || !shard->canMutate(key, page->second)
            || page->second.activity.nativeWrite) {
            result.rejectionReason = QStringLiteral("page is missing or reserved for publication");
            return result;
        }
        if (acceptPreparedRetry) {
            KisVersionSlotId acceptedSlot;
            const auto *accepted = shard->records.findVersion(write.version, &acceptedSlot)
                ? shard->records.version(acceptedSlot) : nullptr;
            if (accepted && accepted->publication == KisPagePublicationState::Prepared
                && accepted->preparedBy == write.transaction
                && shard->records.projectReplica(accepted->authorityReplica) == write.target) {
                result.accepted = true;
                return result;
            }
        }
        step.next.setHeader(shard->header(key, page->second));
        const auto include = [&](const KisPageVersion &identity) {
            if (!identity.isValid() || step.next.findVersion(identity)) return;
            KisPageWorkingVersion value(storage);
            // This finite sequence only reads scalar replica facts and changes
            // a new target, base pin, writer or publication. No guard reads
            // leases/last-use/capture and no original overflow is replaced.
            if (shard->records.snapshot<KisPageWorkingVersion, false>(identity, &value))
                step.next.versions.push_back(std::move(value));
        };
        include(step.next.writer.baseVersion);
        include(step.next.writer.target.version);
        include(write.baseVersion);
        include(write.version);
        if (addition && write.target.isValid()) {
            KisPageVersion witness;
            if (shard->records.physicalOwner(write.target.physicalSlotIdentity(), &witness) && witness.key == key)
                include(witness);
        }
        const quint64 inputs = quint64(step.next.versions.size());
        const bool background = kisOnPageStoreReclamationThread();
        ++shard->localTransitionSequences;
        shard->localVersionInputs += inputs;
        if (background) {
            ++shard->backgroundLocalTransitionSequences;
            shard->backgroundLocalVersionInputs += inputs;
        }
        const KisPageStateMachine machine;
        for (qsizetype i = 0; i < count; ++i) {
            machine.applyKnownValid(step, transitions[i]);
            if (!step.accepted) {
                ++shard->rejectedTransitions;
                result.rejectionReason = step.rejectionReason;
                return result;
            }
            Q_ASSERT(step.effects.empty());
        }
        const auto nextHeader = step.next.header();
        if (addition) {
            auto *target = step.next.findVersion(write.version);
            Q_ASSERT(target);
            additions.push_back(std::move(*target));
        }
        const quint64 revision = page->second.revision;
        const auto demand = shard->records.batchDemand(additions);
        const auto ready = growMetadataArenasOutsideLock(shard, demand, &growth, &lock, [&] {
            page = shard->pages.find(key);
            return page != shard->pages.end() && shard->canMutate(key, page->second)
                && page->second.revision == revision;
        }, nullptr);
        if (ready == MetadataGrowthResult::Stale) {
            --shard->localTransitionSequences;
            shard->localVersionInputs -= inputs;
            if (background) {
                --shard->backgroundLocalTransitionSequences;
                shard->backgroundLocalVersionInputs -= inputs;
            }
            continue;
        }
        if (ready != MetadataGrowthResult::Ready
            || !shard->ensureActivityCapacity(key, &nextHeader, &result.rejectionReason)) {
            ++shard->rejectedTransitions;
            result.storageRefused = true;
            result.rejectionReason = QStringLiteral("metadata write delta capacity is exhausted");
            return result;
        }
        auto discardActivity = qScopeGuard([&] {
            const auto activity = shard->activities.find(key);
            if (activity != shard->activities.end() && activity->second.isEmpty()) shard->activities.erase(activity);
        });
        ShardRecordStore::ReservationSet reservation;
        qsizetype replicas = 0;
        for (const auto &value : additions) replicas += qsizetype(value.replicas.size());
        if (!shard->records.reserve(demand, qsizetype(additions.size()), replicas, &reservation)) {
            ++shard->rejectedTransitions;
            result.storageRefused = true;
            result.rejectionReason = QStringLiteral("metadata write delta reservation was refused");
            return result;
        }
        auto cancelReservation = qScopeGuard([&] { reservation.cancel(&shard->records); });
        if (!shard->records.installPreparedAdditionsReserved(&page->second, &additions, &reservation)) {
            ++shard->rejectedTransitions;
            result.rejectionReason = QStringLiteral("metadata index reservation or physical ownership conflict");
            return result;
        }
        // All fallible work precedes this point. The shared policy has
        // decided the values; only these finite sequence's actual fields are
        // installed. Other replicas and every protection node stay in place.
        const bool publication = write.kind == KisPageTransitionKind::BeginPublish && count == 2;
        if ((write.kind == KisPageTransitionKind::AcquireWrite || publication) && write.source.isValid()) {
            const auto *base = step.next.findVersion(write.baseVersion);
            const auto *value = base ? base->findReplica(write.source) : nullptr;
            KisVersionSlotId versionSlot;
            KisReplicaSlotId replicaSlot;
            const bool found = value && shard->records.findVersion(write.baseVersion, &versionSlot)
                && shard->records.findReplicaSlot(versionSlot, write.source, &replicaSlot);
            Q_ASSERT(found); Q_UNUSED(found);
            shard->records.replica(replicaSlot)->pinCount = value->pinCount;
        }
        if (publication) {
            const auto *value = step.next.findVersion(write.version);
            KisVersionSlotId versionSlot;
            KisReplicaSlotId replicaSlot;
            const bool found = value && shard->records.findVersion(write.version, &versionSlot)
                && shard->records.findReplicaSlot(versionSlot, write.target, &replicaSlot);
            Q_ASSERT(found); Q_UNUSED(found);
            shard->records.replica(replicaSlot)->validity = value->findReplica(write.target)->validity;
            shard->records.version(versionSlot)->authorityReplica = replicaSlot;
            const bool installed = shard->records.setPublication(&page->second, write.version,
                value->publication, value->preparedBy);
            Q_ASSERT(installed); Q_UNUSED(installed);
        }
        const bool installed = shard->installHeader(key, &page->second, nextHeader);
        Q_ASSERT(installed); Q_UNUSED(installed);
        discardActivity.dismiss();
        reservation.cancel(&shard->records);
        cancelReservation.dismiss();
        ++page->second.revision;
        shard->localVersionInstalls += quint64(additions.size());
        if (background) shard->backgroundLocalVersionInstalls += quint64(additions.size());
        shard->acceptedTransitions += quint64(count);
        result.accepted = true;
        auto releasedBlocks = shard->arenas.takeEmptyBlocks(&shard->budgetCharge);
        lock.unlock();
        Q_UNUSED(releasedBlocks);
        return result;
    }
} catch (const std::bad_alloc &) {
    KisPageMetadataTransitionResult refused;
    refused.storageRefused = true;
    refused.rejectionReason = QStringLiteral("metadata write delta storage was refused");
    return refused;
}

KisPageMetadataTransitionResult KisPageMetadataCoordinator::applyReadProtection(
    const KisPageKey &key, const KisPageTransition &transition,
    KisPageMetadataReadCleanup *cleanup)
{
    KisPageMetadataTransitionResult result;
    const bool releaseRead = transition.kind == KisPageTransitionKind::ReleaseRead;
    if ((!releaseRead && transition.kind != KisPageTransitionKind::AcknowledgeLastUse)
        || !(transition.version.key == key)
        || (releaseRead ? !transition.lease.isValid() : !transition.completion.isValid())) {
        result.rejectionReason = QStringLiteral("read protection transition identity is invalid");
        return result;
    }
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), key);
    if (!shard) {
        result.rejectionReason = QStringLiteral("metadata coordinator is not configured");
        return result;
    }
    if (cleanup && cleanup->m_authority && cleanup->m_authority != shard->budgetAuthority) {
        result.rejectionReason = QStringLiteral("read protection cleanup belongs to another metadata owner");
        return result;
    }
    // Payloads and their charge leave the shard together, with no allocation.
    // Declared before the lock so payload destruction and budget release occur
    // after unlocking, including when an early return rejects the transition.
    MetadataArenas::ReleasedBlocks released;
    QMutexLocker lock(&shard->mutex);
    auto page = shard->pages.find(key);
    if (page == shard->pages.end() || !shard->canMutate(key, page->second)) {
        result.rejectionReason = QStringLiteral("page is missing or reserved for publication");
        return result;
    }
    ++shard->localTransitionSequences;
    if (kisOnPageStoreReclamationThread()) ++shard->backgroundLocalTransitionSequences;
    if (!shard->records.finishReadProtection(transition, &released.overflow,
                                            shard, &result.rejectionReason)) {
        ++shard->rejectedTransitions;
        return result;
    }
    if (!released.overflow.isEmpty()) {
        released.budgetRelease = shard->budgetCharge.take(released.overflow.byteSize());
        if (cleanup) {
            if (!cleanup->m_authority)
                cleanup->m_authority = std::move(released.budgetRelease.authority);
            cleanup->m_bytes += std::exchange(released.budgetRelease.bytes, 0);
            cleanup->m_blocks.append(std::move(released.overflow));
        }
    }
    ++page->second.revision;
    ++shard->acceptedTransitions;
    ++shard->readProtectionTransitions;
    result.accepted = true;
    result.historyNeedsRevisit = page->second.historyCount != 0
        && (!releaseRead || !transition.completion.isValid());
    return result;
}

KisPageMetadataTransitionResult KisPageMetadataCoordinator::applyCapturedProtection(
    const KisPageKey &key, const KisPageTransition &transition, KisPageMetadataReadCleanup *cleanup)
{
    KisPageMetadataTransitionResult result;
    const bool retain = transition.kind == KisPageTransitionKind::RetainCapturedVersion;
    if ((!retain && transition.kind != KisPageTransitionKind::ReleaseCapturedVersion)
        || !(transition.version.key == key) || !transition.readView.isValid()) {
        result.rejectionReason = QStringLiteral("captured protection identity or cleanup is invalid");
        return result;
    }
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), key);
    if (!shard) {
        result.rejectionReason = QStringLiteral("metadata coordinator is not configured");
        return result;
    }
    if (cleanup && cleanup->m_authority && cleanup->m_authority != shard->budgetAuthority) {
        result.rejectionReason = QStringLiteral("captured protection identity or cleanup is invalid");
        return result;
    }
    for (;;) {
        const auto storageOwner = shard->budgetAuthority->storage<char>();
        MetadataArenaGrowth growth(storageOwner.borrowed());
        MetadataArenas::ReleasedBlocks released;
        QMutexLocker lock(&shard->mutex);
        auto page = shard->pages.find(key);
        KisVersionSlotId versionSlot;
        if (page == shard->pages.end() || !shard->canMutate(key, page->second)) {
            result.rejectionReason = QStringLiteral("captured version is missing or reserved for publication");
            return result;
        }
        ++shard->localTransitionSequences;
        const bool background = kisOnPageStoreReclamationThread();
        if (background) ++shard->backgroundLocalTransitionSequences;
        if (!shard->records.findVersion(transition.version, &versionSlot)) {
            ++shard->rejectedTransitions;
            result.rejectionReason = QStringLiteral("captured version identity is not readable");
            return result;
        }
        auto *version = shard->records.version(versionSlot);
        if (version->publication == KisPagePublicationState::Unpublished
            || version->publication == KisPagePublicationState::Retiring) {
            ++shard->rejectedTransitions;
            result.rejectionReason = QStringLiteral("captured version identity is not readable");
            return result;
        }
        if (retain && (version->publication != KisPagePublicationState::Prepared
                       || !(version->preparedBy == transition.transaction))) {
            ++shard->rejectedTransitions;
            result.rejectionReason = QStringLiteral("captured private version is foreign or already retained");
            return result;
        }
        auto *link = &version->capturedReadViews;
        while (link->isValid()) {
            const auto *node = shard->records.overflow(*link);
            Q_ASSERT(node && node->kind == KisMetadataOverflowKind::CapturedReadView);
            if (node->value == transition.readView.value) break;
            link = &shard->records.overflow(*link)->next;
        }
        if (retain == link->isValid()) {
            ++shard->rejectedTransitions;
            result.rejectionReason = retain ? QStringLiteral("captured private version is foreign or already retained")
                                            : QStringLiteral("captured version token is stale or foreign");
            return result;
        }
        if (retain) {
            const quint64 revision = page->second.revision;
            MetadataArenaDemand demand; demand.overflow = 1;
            const auto storage = growMetadataArenasOutsideLock(shard, demand, &growth, &lock, [&] {
                page = shard->pages.find(key);
                return page != shard->pages.end() && shard->canMutate(key, page->second)
                    && page->second.revision == revision;
            }, &result.rejectionReason);
            if (storage == MetadataGrowthResult::Stale) {
                --shard->localTransitionSequences;
                if (background) --shard->backgroundLocalTransitionSequences;
                continue;
            }
            if (storage != MetadataGrowthResult::Ready) {
                ++shard->rejectedTransitions;
                result.rejectionReason = QStringLiteral("captured protection storage is exhausted");
                return result;
            }
            // Arena values stay stable across growth. The unchanged page
            // revision also keeps this exact append link valid after relock.
            auto reservation = shard->arenas.overflow.reserveSlots(1);
            KisMetadataOverflowNode node;
            node.value = transition.readView.value;
            *link = shard->arenas.overflow.emplaceReserved(&reservation, node);
            Q_ASSERT(link->isValid());
        } else {
            const auto slot = *link;
            *link = shard->records.overflow(slot)->next;
            const bool erased = shard->arenas.overflow.erase(slot, &released.overflow, 1);
            Q_ASSERT(erased); Q_UNUSED(erased);
            if (!released.overflow.isEmpty()) {
                released.budgetRelease = shard->budgetCharge.take(released.overflow.byteSize());
                if (cleanup) {
                    if (!cleanup->m_authority) cleanup->m_authority = std::move(released.budgetRelease.authority);
                    cleanup->m_bytes += std::exchange(released.budgetRelease.bytes, 0);
                    cleanup->m_blocks.append(std::move(released.overflow));
                }
            }
        }
        ++page->second.revision;
        ++shard->acceptedTransitions;
        result.accepted = true;
        result.historyNeedsRevisit = !retain && page->second.historyCount != 0;
        return result;
    }
}

KisPageMetadataTransitionResult KisPageMetadataCoordinator::applyProjectedSequence(
    const KisPageKey &key, const KisPageTransition *transitions, qsizetype count)
try {
    KisPageMetadataTransitionResult result;
    if (count <= 0 || !transitions || std::any_of(transitions, transitions + count, [](const auto &t) {
            return !localTransition(t.kind);
        })) {
        result.rejectionReason = QStringLiteral("owner transition is not a local metadata mutation");
        return result;
    }
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), key);
    if (!shard) {
        result.rejectionReason = QStringLiteral("metadata coordinator is not configured");
        return result;
    }
    for (;;) {
        const auto storageOwner = shard->budgetAuthority->storage<char>();
        const auto workingStorage = storageOwner.borrowed();
        MetadataArenaGrowth growth(workingStorage);
        KisPageWorkingArray<KisPageTransitionEffect> effects(workingStorage);
        KisPageWorkingArray<std::shared_ptr<const KisPageAllocationDescriptor>, 2> releasedDescriptors{
            decltype(releasedDescriptors)::allocator_type(workingStorage)};
        KisPageWorkingArray<std::shared_ptr<KisCpuResidentBinding>, 2> releasedBindings{
            decltype(releasedBindings)::allocator_type(workingStorage)};
        QMutexLocker lock(&shard->mutex);
        auto page = shard->pages.find(key);
        if (page == shard->pages.end() || !shard->canMutate(key, page->second)) {
            result.rejectionReason = QStringLiteral("page is missing or reserved for publication");
            return result;
        }
        KisPageWorkingResult step(workingStorage);
        auto &input = step.next;
        input.setHeader(shard->header(key, page->second));
        const auto add = [&](const KisPageVersion &identity) {
            if (!identity.isValid() || input.findVersion(identity))
                return;
            KisPageWorkingVersion version(workingStorage);
            if (shard->records.snapshot(identity, &version)) {
                input.versions.push_back(std::move(version));
            }
        };
        add(input.writer.baseVersion);
        add(input.writer.target.version);
        for (qsizetype i = 0; i < count; ++i) {
            const auto &transition = transitions[i];
            add(transition.baseVersion);
            add(transition.version);
            for (const auto &version : transition.versions)
                add(version);
            if (transition.kind == KisPageTransitionKind::AcquireRead) {
                KisPageVersion leaseOwner;
                if (shard->records.readLeaseOwner(page->second, transition.lease, &leaseOwner))
                    add(leaseOwner);
            }
            if (transition.kind == KisPageTransitionKind::AbortTransaction) {
                shard->records.forPreparedTransaction(page->second, transition.transaction, add);
            }
            // Allocation slot uniqueness is a PAGE-wide condition, including old
            // history and regardless of allocation generation/domain. Include its
            // exact witness so the shared state machine rejects with the same guard.
            if (transition.target.isValid()) {
                KisPageVersion physicalOwner;
                if (shard->records.physicalOwner(transition.target.physicalSlotIdentity(), &physicalOwner)
                    && physicalOwner.key == key) {
                    add(physicalOwner);
                }
            }
        }
        ++shard->localTransitionSequences;
        shard->localVersionInputs += quint64(input.versions.size());
        const bool background = kisOnPageStoreReclamationThread();
        if (background) {
            ++shard->backgroundLocalTransitionSequences;
            shard->backgroundLocalVersionInputs += quint64(input.versions.size());
        }
        // Installation needs the original identities only to remove records
        // absent from the accepted result. No second replica/protection image
        // is needed through unlocked growth or retirement preparation.
        KisPageWorkingArray<KisPageVersion, 2> inputVersions{
            KisPageWorkingArray<KisPageVersion, 2>::allocator_type(workingStorage)};
        inputVersions.reserve(input.versions.size());
        for (const auto &version : input.versions)
            inputVersions.push_back(version.version);
        auto &next = input;
        const KisPageStateMachine machine;
        for (qsizetype i = 0; i < count; ++i) {
            const auto &transition = transitions[i];
            machine.applyKnownValid(step, transition);
            if (!step.accepted) {
                ++shard->rejectedTransitions;
                result.rejectionReason = step.rejectionReason;
                return result; // no authoritative record has changed
            }
            effects.insert(effects.end(), step.effects.cbegin(), step.effects.cend());
        }
        // Only projected records are installed/removed. Unrelated history and its
        // reader/pin/last-use state are neither copied nor overwritten.
        const quint64 expectedRevision = page->second.revision;
        const auto discardStaleAttempt = [&] {
            Q_ASSERT(shard->localTransitionSequences > 0);
            Q_ASSERT(shard->localVersionInputs >= quint64(inputVersions.size()));
            --shard->localTransitionSequences;
            shard->localVersionInputs -= quint64(inputVersions.size());
            if (background) {
                Q_ASSERT(shard->backgroundLocalTransitionSequences > 0);
                Q_ASSERT(shard->backgroundLocalVersionInputs >= quint64(inputVersions.size()));
                --shard->backgroundLocalTransitionSequences;
                shard->backgroundLocalVersionInputs -= quint64(inputVersions.size());
            }
        };
        quint64 retirementDebtCookie = 0;
        bool retirementDebtPrepared = false;
        const auto cancelRetirementDebt = qScopeGuard([&] {
            if (retirementDebtPrepared && m_cancelRetirementDebt) {
                m_cancelRetirementDebt(m_retirementDebtContext,
                                        retirementDebtCookie);
            }
        });
        if (!effects.empty()) {
            if (!m_prepareRetirementDebt) {
                ++shard->rejectedTransitions;
                result.rejectionReason = QStringLiteral("retirement effect receiver is not configured");
                return result;
            }
            QString debtError;
            lock.unlock();
            const bool debtAccepted = m_prepareRetirementDebt(m_retirementDebtContext,
                                                               effects.data(), qsizetype(effects.size()),
                                                               &retirementDebtCookie,
                                                               &debtError);
            retirementDebtPrepared = debtAccepted;
            lock.relock();
            if (!debtAccepted) {
                ++shard->rejectedTransitions;
                result.storageRefused = true;
                result.rejectionReason = debtError.isEmpty()
                    ? QStringLiteral("retirement debt budget is exhausted") : debtError;
                return result;
            }
            page = shard->pages.find(key);
            if (page == shard->pages.end() || !shard->canMutate(key, page->second)
                || page->second.revision != expectedRevision) {
                discardStaleAttempt();
                continue;
            }
        }
        const MetadataArenaDemand demand = shard->records.batchDemand(next.versions);
        const auto storage = growMetadataArenasOutsideLock(shard, demand, &growth, &lock, [&] {
            page = shard->pages.find(key);
            return page != shard->pages.end() && shard->canMutate(key, page->second)
                && page->second.revision == expectedRevision;
        }, nullptr);
        if (storage == MetadataGrowthResult::Stale) {
            // Local owner transitions are composable against the newest page
            // revision. Arena growth is an implementation detail, so do not leak
            // its unlock window as a transient semantic rejection to callers.
            discardStaleAttempt();
            continue;
        }
        if (storage != MetadataGrowthResult::Ready) {
            ++shard->rejectedTransitions;
            result.storageRefused = true;
            result.rejectionReason = QStringLiteral("metadata arena budget is exhausted");
            return result;
        }
        const auto nextHeader = next.header();
        if (!shard->ensureActivityCapacity(key, &nextHeader, &result.rejectionReason)) {
            result.storageRefused = true;
            result.rejectionReason = QStringLiteral("metadata activity capacity budget is exhausted");
            return result;
        }
        auto discardActivity = qScopeGuard([&] {
            const auto activity = shard->activities.find(key);
            if (activity != shard->activities.end() && activity->second.isEmpty()) shard->activities.erase(activity);
        });
        // The next values already witness retention. No second removal set
        // may allocate after installation starts.
        releasedDescriptors.resize(size_t(inputVersions.size()));
        releasedBindings.resize(size_t(inputVersions.size()));
        if (!shard->records.putAcceptedBatch(&page->second, step)) {
            ++shard->rejectedTransitions;
            result.rejectionReason = QStringLiteral("metadata index reservation or physical ownership conflict");
            return result;
        }
        quint64 removed = 0;
        for (const auto &version : inputVersions) {
            if (!next.findVersion(version)) {
                shard->records.remove(&page->second, version, &releasedDescriptors[size_t(removed)],
                                      &releasedBindings[size_t(removed)]);
                ++removed;
            }
        }
        const bool headerInstalled = shard->installHeader(key, &page->second, nextHeader);
        Q_ASSERT(headerInstalled);
        Q_UNUSED(headerInstalled);
        // installHeader already installed or removed the authoritative
        // activity under the shard gate. The preparation rollback must not
        // revisit that map after this successful path unlocks the gate.
        discardActivity.dismiss();
        ++page->second.revision;
        shard->localVersionInstalls += quint64(next.versions.size());
        shard->localVersionRemovals += removed;
        if (background) {
            shard->backgroundLocalVersionInstalls += quint64(next.versions.size());
            shard->backgroundLocalVersionRemovals += removed;
        }
        shard->acceptedTransitions += quint64(count);
        result.accepted = true;
        auto releasedBlocks = shard->arenas.takeEmptyBlocks(&shard->budgetCharge);
        lock.unlock();
        if (retirementDebtPrepared) {
            m_commitRetirementDebt(m_retirementDebtContext, retirementDebtCookie,
                                    effects.data(), qsizetype(effects.size()));
            retirementDebtPrepared = false;
        }
        Q_UNUSED(releasedBlocks);
        return result;
    }
}
catch (const std::bad_alloc &) {
    KisPageMetadataTransitionResult refused;
    refused.storageRefused = true;
    refused.rejectionReason = QStringLiteral("metadata transition working storage was refused");
    return refused;
}

KisPageSnapshotArray<KisPageKey> KisPageMetadataCoordinator::pageKeys() const
{
    KisPageSnapshotArray<KisPageKey> result;
    visitPageKeys(&result, +[](void *p, const KisPageKey &key) {
        static_cast<KisPageSnapshotArray<KisPageKey> *>(p)->append(key);
    });
    return result;
}

void KisPageMetadataCoordinator::visitPageKeys(
    void *context, void (*visit)(void *, const KisPageKey &), bool historicalOnly) const
{
    QMutexLocker configurationLock(&m_configurationMutex);
    const auto *d = m_core.load(std::memory_order_acquire);
    if (!d) return;
    for (const auto &shard : d->shards) {
        QMutexLocker shardLock(&shard->mutex);
        for (auto page = shard->pages.cbegin(); page != shard->pages.cend(); ++page)
            if (!historicalOnly || page->second.historyCount)
                visit(context, page->first); // Under shard lock: consumer must not reenter metadata.
    }
}

KisPageSnapshotArray<KisReplicaHandle> KisPageMetadataCoordinator::shutdownReplicaHandles() const
{
    std::vector<MetadataShard *, KisMutationStorageAllocator<MetadataShard *>> shards;
    {
        QMutexLocker lock(&m_configurationMutex);
        const auto *d = m_core.load(std::memory_order_acquire);
        if (!d) return {};
        shards.reserve(qsizetype(d->shards.size()));
        for (const auto &shard : d->shards) shards.push_back(shard.get());
    }
    KisPageSnapshotArray<KisReplicaHandle> result;
    for (MetadataShard *shard : std::as_const(shards)) {
        QMutexLocker lock(&shard->mutex);
        for (const auto &entry : std::as_const(shard->pages)) {
            const auto &page = entry.second;
            for (KisVersionSlotId versionSlot = page.firstVersion; versionSlot.isValid();) {
                const KisVersionRecord *version = shard->records.version(versionSlot);
                Q_ASSERT(version);
                if (!version) break;
                for (KisReplicaSlotId replicaSlot = version->firstReplica; replicaSlot.isValid();) {
                    const KisReplicaRecord *replica = shard->records.replica(replicaSlot);
                    Q_ASSERT(replica);
                    if (!replica) break;
                    result.append(replicaHandle(*replica, version->version));
                    replicaSlot = replica->nextReplica;
                }
                versionSlot = version->nextVersion;
            }
        }
    }
    return result;
}

KisPageMetadataTransitionResult KisPageMetadataCoordinator::acknowledgeLastUse(const KisPageVersion &version,
                                                                       const KisReplicaHandle &replica,
                                                                       const KisVerifiedCompletion &completion)
{
    return acknowledgeLastUse(version, replica, completion, nullptr);
}

KisPageMetadataTransitionResult KisPageMetadataCoordinator::acknowledgeLastUse(const KisPageVersion &version,
                                                                       const KisReplicaHandle &replica,
                                                                       const KisVerifiedCompletion &completion,
                                                                       KisPageMetadataReadCleanup *cleanup)
{
    KisPageMetadataTransitionResult result;
    if (!version.isValid() || !replica.isValid() || !(replica.version == version) || !completion.isValid()) {
        result.rejectionReason = QStringLiteral("verified last-use completion is invalid");
        return result;
    }
    KisPageTransition transition;
    transition.kind = KisPageTransitionKind::AcknowledgeLastUse;
    transition.version = version;
    transition.target = replica;
    transition.completion = completion.ticket();
    // Verification stays at this entry point. Consume only this exact replica's
    // existing protection slots; ordinary owner transitions cannot acknowledge.
    return applyReadProtection(version.key, transition, cleanup);
}

bool KisPageMetadataCoordinator::queryLastUsePending(
    const KisReplicaHandle &replica, const KisCompletionTicket &completion, bool *pending) const
{
    if (!replica.isValid() || !completion.isValid() || !pending) return false;
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), replica.version.key);
    if (!shard) return false;
    QMutexLocker lock(&shard->mutex);
    *pending = false;
    KisVersionSlotId versionSlot;
    if (!shard->records.findVersion(replica.version, &versionSlot)) return true;
    const auto *version = shard->records.version(versionSlot);
    if (!version) return false;
    const KisReplicaRecord *record = nullptr;
    for (auto slot = version->firstReplica; slot.isValid();) {
        const auto *candidate = shard->records.replica(slot);
        if (!candidate) return false;
        if (replicaHandle(*candidate, version->version) == replica) {
            record = candidate;
            break;
        }
        slot = candidate->nextReplica;
    }
    if (!record) return true;
    for (auto slot = record->overflow; slot.isValid();) {
        const auto *node = shard->records.overflow(slot);
        if (!node) return false;
        if (node->kind == KisMetadataOverflowKind::PendingLastUse && node->completion == completion) {
            *pending = true;
            return true;
        }
        slot = node->next;
    }
    return true;
}

qsizetype KisPageMetadataCoordinator::pageCount() const
{
    qsizetype count = 0;
    QMutexLocker configurationLocker(&m_configurationMutex);
    const auto *d = m_core.load(std::memory_order_acquire);
    if (!d) return 0;
    for (const std::shared_ptr<MetadataShard> &shard : d->shards) {
        QMutexLocker shardLocker(&shard->mutex);
        count += shard->pages.size();
    }
    return count;
}

KisPageMetadataMetrics KisPageMetadataCoordinator::metrics() const
{
    KisPageMetadataMetrics metrics;
    const auto *d = m_core.load(std::memory_order_acquire);
    if (d)
        for (const auto &shard : d->shards) {
            QMutexLocker lock(&shard->mutex);
            metrics.acceptedTransitions += shard->acceptedTransitions;
            metrics.rejectedTransitions += shard->rejectedTransitions;
            metrics.preparedPublicationPages += shard->preparedPublicationPages;
            metrics.installedPublicationPages += shard->installedPublicationPages;
            metrics.rejectedPublicationInstalls += shard->rejectedPublicationInstalls;
            metrics.preparedMutationPages += shard->preparedMutationPages;
            metrics.installedMutationPages += shard->installedMutationPages;
            metrics.rejectedMutationInstalls += shard->rejectedMutationInstalls;
            metrics.publicationHistoryNodesPrepared += shard->publicationHistoryNodesPrepared;
            metrics.publicationAdditionRecordsPrepared += shard->publicationAdditionRecordsPrepared;
            metrics.localTransitionSequences += shard->localTransitionSequences;
            metrics.localVersionInputs += shard->localVersionInputs;
            metrics.localVersionInstalls += shard->localVersionInstalls;
            metrics.localVersionRemovals += shard->localVersionRemovals;
            metrics.readProtectionTransitions += shard->readProtectionTransitions;
            metrics.readProtectionNodeVisits += shard->readProtectionNodeVisits;
            metrics.readProtectionSlotReuses += shard->readProtectionSlotReuses;
            metrics.readProtectionSlotReleases += shard->readProtectionSlotReleases;
            metrics.nativeWriteReservations += shard->nativeWriteReservations;
            metrics.nativeSlotReplacements += shard->nativeSlotReplacements;
            metrics.nativeCapacityRetries += shard->nativeCapacityRetries;
            metrics.mutationBaseVersionInputs += shard->mutationBaseVersionInputs;
            metrics.backgroundLocalTransitionSequences += shard->backgroundLocalTransitionSequences;
            metrics.backgroundLocalVersionInputs += shard->backgroundLocalVersionInputs;
            metrics.backgroundLocalVersionInstalls += shard->backgroundLocalVersionInstalls;
            metrics.backgroundLocalVersionRemovals += shard->backgroundLocalVersionRemovals;
            metrics.backgroundMutationBaseVersionInputs += shard->backgroundMutationBaseVersionInputs;
            metrics.publicationVersionInputs += shard->publicationVersionInputs;
            metrics.publicationVersionInstalls += shard->publicationVersionInstalls;
            metrics.publicationLookupVersionInputs += shard->publicationLookupVersionInputs;
            metrics.publicationDirectoryHeaders += shard->publicationDirectoryHeaders;
            metrics.publicationHistoryNodesTransferred += shard->publicationHistoryNodesTransferred;
            metrics.publicationAdditionRecordsTransferred += shard->publicationAdditionRecordsTransferred;
            metrics.metadataArenaGrowthBatches += shard->metadataArenaGrowthBatches;
            metrics.metadataArenaBlockCandidatesPrepared += shard->metadataArenaBlockCandidatesPrepared;
            metrics.metadataArenaBlocksAttached += shard->metadataArenaBlocksAttached;
            metrics.metadataArenaGrowthConflicts += shard->metadataArenaGrowthConflicts;
            metrics.metadataArenaGrowthFailures += shard->metadataArenaGrowthFailures;
            metrics.cpuReadBindings += shard->arenas.cpuReadBindings;
            metrics.historySliceQueries += shard->historySliceQueries;
            metrics.historySliceVersionInputs += shard->historySliceVersionInputs;
            metrics.maximumHistorySliceVersionInputs =
                qMax(metrics.maximumHistorySliceVersionInputs, shard->maximumHistorySliceVersionInputs);
            metrics.fullSnapshotExports += shard->fullSnapshotExports;
            metrics.fullSnapshotVersionInputs += shard->fullSnapshotVersionInputs;
            metrics.backgroundSnapshotExports += shard->backgroundSnapshotExports;
            metrics.backgroundSnapshotVersionInputs += shard->backgroundSnapshotVersionInputs;
        }
    return metrics;
}

KisPageMetadataFootprint KisPageMetadataCoordinator::footprint() const
{
    KisPageMetadataFootprint result;
    QMutexLocker configurationLocker(&m_configurationMutex);
    const auto *d = m_core.load(std::memory_order_acquire);
    if (!d) return result;
    for (const std::shared_ptr<MetadataShard> &shard : d->shards) {
        QMutexLocker shardLocker(&shard->mutex);
        result.versionArena += shard->arenas.versions.statistics();
        result.versionFactsArena += shard->arenas.facts.statistics();
        result.replicaArena += shard->arenas.replicas.statistics();
        result.overflowArena += shard->arenas.overflow.statistics();
        result.exactVersionIndex += shard->records.exactVersions.statistics();
        result.physicalSlotIndex += shard->records.physicalSlots.statistics();
        result.owningCapacityBytes += shard->mapAllocationBytes.load(std::memory_order_relaxed)
            + shard->records.exactVersions.allocatedBytes() + shard->records.physicalSlots.allocatedBytes()
            + quint64(sizeof(MetadataShard));
        result.pages += quint64(shard->pages.size());
        for (const auto &entry : shard->pages) result.pageActivities += !entry.second.activity.isEmpty();
        const quint64 pageBytes = quint64(shard->pages.size()) * sizeof(MetadataPage);
        const quint64 activityBytes = quint64(shard->activities.size()) * sizeof(MetadataGenericActivity);
        result.metadataPageBytes += pageBytes;
        result.metadataPageActivityBytes += activityBytes;
        result.trackedPayloadBytes += pageBytes + activityBytes;
        const auto exactIndex = shard->records.exactVersions.statistics();
        const auto physicalIndex = shard->records.physicalSlots.statistics();
        result.trackedPayloadBytes += exactIndex.entries * (sizeof(KisPageVersion) + sizeof(KisVersionSlotId));
        result.trackedPayloadBytes += physicalIndex.entries * (sizeof(PhysicalSlot) + sizeof(KisReplicaSlotId));
        for (const auto &entry : std::as_const(shard->pages)) {
            const auto &page = entry.second;
            result.versions += page.versionCount;
            for (KisVersionSlotId versionSlot = page.firstVersion; versionSlot.isValid();) {
                const KisVersionRecord *version = shard->records.version(versionSlot);
                Q_ASSERT(version);
                if (!version)
                    break;
                result.trackedPayloadBytes += sizeof(KisVersionRecord) + sizeof(KisVersionFactsRecord);
                for (KisMetadataOverflowSlotId overflowSlot = version->capturedReadViews; overflowSlot.isValid();) {
                    const KisMetadataOverflowNode *node = shard->records.overflow(overflowSlot);
                    Q_ASSERT(node);
                    if (!node)
                        break;
                    result.trackedPayloadBytes += sizeof(KisMetadataOverflowNode);
                    overflowSlot = node->next;
                }
                for (KisReplicaSlotId replicaSlot = version->firstReplica; replicaSlot.isValid();) {
                    const KisReplicaRecord *replica = shard->records.replica(replicaSlot);
                    Q_ASSERT(replica);
                    if (!replica)
                        break;
                    ++result.replicas;
                    result.trackedPayloadBytes += sizeof(KisReplicaRecord);
                    for (KisMetadataOverflowSlotId overflowSlot = replica->overflow; overflowSlot.isValid();) {
                        const KisMetadataOverflowNode *node = shard->records.overflow(overflowSlot);
                        Q_ASSERT(node);
                        if (!node)
                            break;
                        result.trackedPayloadBytes += sizeof(KisMetadataOverflowNode);
                        if (node->kind == KisMetadataOverflowKind::ReadLease) {
                            ++result.readLeases;
                        } else if (node->kind == KisMetadataOverflowKind::PendingLastUse) {
                            ++result.pendingLastUses;
                        }
                        overflowSlot = node->next;
                    }
                    replicaSlot = replica->nextReplica;
                }
                versionSlot = version->nextVersion;
            }
        }
    }
    return result;
}

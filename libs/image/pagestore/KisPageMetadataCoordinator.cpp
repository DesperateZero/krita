/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageMetadataCoordinator.h"
#include "KisCpuResidentBinding_p.h"
#include "KisDeferredMetadataCleanup_p.h"
#include "KisPageMetadataCoordinator_p.h"
#include "KisPageStoreReclamation_p.h"
#include "KisPageWriteCoordinator_p.h"
#include "KisPageStateMachine_p.h"

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
            const qint64 bytes = installed.buckets[size_t(KisBackingBudgetClass::MetadataArena)].cpuRam;
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
        delta.buckets[size_t(KisBackingBudgetClass::MetadataArena)].cpuRam =
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
            installed.buckets[size_t(KisBackingBudgetClass::MetadataArena)].cpuRam =
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

    bool revisionCanAdvance() const
    {
        return revision != std::numeric_limits<quint64>::max();
    }
};

static_assert(sizeof(MetadataPage) <= 96);

/**
 * Sparse shard-owned page activity. Public writer/handoff snapshots are never
 * retained here: stable record slots are projected back to value handles only
 * at a state-machine or diagnostic boundary. An entry exists only while a
 * writer, handoff, or synchronous publication claim is live.
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

static_assert(sizeof(MetadataPageActivity) <= 96);

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

    bool isPreparedBy(const KisPageVersion &identity, KisPageTransactionId transaction) const
    {
        KisVersionSlotId slot;
        if (!findVersion(identity, &slot))
            return false;
        const KisVersionRecord *record = version(slot);
        return record && record->publication == KisPagePublicationState::Prepared && record->preparedBy == transaction;
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

    void putNativeReserved(MetadataPage *page, const KisPagePrivateWrite &native, ReservationSet *reservation)
    {
        const auto slot = arenas->versions.emplaceReserved(&reservation->arenas.versions);
        const auto facts = arenas->facts.emplaceReserved(&reservation->arenas.facts);
        const auto cpuSlot = arenas->replicas.emplaceReserved(&reservation->arenas.replicas);
        Q_ASSERT(slot.isValid() && facts.isValid() && cpuSlot.isValid());
        auto *record = version(slot);
        record->slot = slot;
        record->facts = facts;
        assignNative(record, replica(cpuSlot), cpuSlot, native);
        const bool exact = exactVersions.insertReserved(&reservation->exactVersions, record->version, slot);
        const bool physical = physicalSlots.insertReserved(&reservation->physicalSlots,
            native.target.physicalSlotIdentity(), cpuSlot);
        Q_ASSERT(exact && physical); Q_UNUSED(exact); Q_UNUSED(physical);
        linkVersion(page, record);
        linkMutable(page, record);
    }

    static void assignNative(KisVersionRecord *record, KisReplicaRecord *cpu,
        KisReplicaSlotId replicaSlot, const KisPagePrivateWrite &native)
    {
        record->version = native.target.version;
        record->firstReplica = record->authorityReplica = replicaSlot;
        record->replicaCount = 1;
        record->publication = KisPagePublicationState::Prepared;
        record->preparedBy = native.proof.transaction;
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

    bool canReplaceUnused(const KisPageVersion &identity) const
    {
        KisVersionSlotId slot;
        if (!findVersion(identity, &slot)) return false;
        const auto *record = version(slot);
        const auto *facts = arenas->facts.get(record->facts);
        const auto *cpu = replica(record->authorityReplica);
        return record->publication == KisPagePublicationState::Prepared && !identity.isDefaultPixel()
            && !record->capturedReadViews.isValid() && !facts->nativeReaders && facts->stamp
            && record->replicaCount == 1 && cpu && cpu->domain == KisPageAccessDomain::CpuRam
            && cpu->validity == KisReplicaValidity::Valid && !cpu->pinCount
            && !cpu->overflow.isValid() && !cpu->activeOperation.isValid()
            && arenas->versions.canRebind(slot) && arenas->facts.canRebind(record->facts)
            && arenas->replicas.canRebind(record->authorityReplica);
    }

    void replaceUnused(MetadataPage *page, const KisPageVersion &old, KisPagePrivateWrite &native,
        std::shared_ptr<const KisPageAllocationDescriptor> *releasedDescriptor,
        std::shared_ptr<KisCpuResidentBinding> *releasedBinding)
    {
        Q_ASSERT(canReplaceUnused(old));
        KisVersionSlotId oldSlot;
        const bool found = findVersion(old, &oldSlot);
        Q_ASSERT(found); Q_UNUSED(found);
        auto *record = version(oldSlot);
        auto *facts = arenas->facts.get(record->facts);
        auto *cpu = replica(record->authorityReplica);
        const auto oldReplica = record->authorityReplica;
        const auto oldPhysical = replicaHandle(*cpu, old).physicalSlotIdentity();
        // No observer can retain these identities. Relink the new generation
        // at the end without releasing/reacquiring any arena capacity.
        unlinkMutable(page, record);
        unlinkVersion(page, record);
        const auto slot = arenas->versions.rebind(oldSlot);
        const auto factSlot = arenas->facts.rebind(record->facts);
        const auto replicaSlot = arenas->replicas.rebind(oldReplica);
        Q_ASSERT(slot.isValid() && factSlot.isValid() && replicaSlot.isValid());
        releasedDescriptor->swap(facts->descriptor);
        releasedBinding->swap(facts->nativeBinding);
        --arenas->cpuProofs;
        arenas->cpuReadBindings -= bool(*releasedBinding);
        *facts = {};
        *record = {};
        record->slot = slot;
        record->facts = factSlot;
        *cpu = {};
        assignNative(record, cpu, replicaSlot, native);
        const bool exact = exactVersions.replaceExact(old, oldSlot, record->version, slot);
        const bool physical = physicalSlots.replaceExact(oldPhysical, oldReplica,
            native.target.physicalSlotIdentity(), replicaSlot);
        Q_ASSERT(exact && physical); Q_UNUSED(exact); Q_UNUSED(physical);
        linkVersion(page, record);
        linkMutable(page, record);
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
                std::shared_ptr<KisCpuResidentBinding> *releasedBinding = nullptr)
    {
        KisVersionSlotId slot;
        if (!findVersion(identity, &slot))
            return;
        KisVersionRecord *record = version(slot);
        Q_ASSERT(record);
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
    MetadataMap<KisPageKey, MetadataPageActivity> activities;
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
    const MetadataPageActivity *activity(const KisPageKey &key) const
    {
        const auto found = activities.find(key);
        return found == activities.cend() ? nullptr : &found->second;
    }

    bool canMutate(const KisPageKey &key, const MetadataPage &page) const
    {
        const MetadataPageActivity *current = activity(key);
        return page.revisionCanAdvance() && (!current || !current->publicationClaim);
    }

    bool hasWriter(const KisPageKey &key) const
    {
        const MetadataPageActivity *current = activity(key);
        return current && current->hasWriter();
    }

    const void *publicationClaim(const KisPageKey &key) const
    {
        const MetadataPageActivity *current = activity(key);
        return current ? current->publicationClaim : nullptr;
    }

    void updateActivity(const KisPageKey &key, const MetadataPageActivity &next)
    {
        if (next.isEmpty()) {
            activities.erase(key);
        } else {
            const auto current = activities.find(key);
            Q_ASSERT(current != activities.end());
            current->second = next;
        }
    }

    void setPublicationClaim(const KisPageKey &key, const void *claim)
    {
        auto current = activities.find(key);
        Q_ASSERT(current != activities.end());
        if (current == activities.end())
            return;
        current->second.publicationClaim = claim;
        if (current->second.isEmpty())
            activities.erase(current);
    }

    bool ensureActivityCapacity(const KisPageKey &key,
                                const KisPageStateSnapshot *state = nullptr,
                                QString *error = nullptr)
    {
        const bool needsEntry = (activities.find(key) != activities.end())
            || (state && (state->writer.phase != KisPageWriterPhase::None
                          || state->authorityHandoff.operation.isValid()));
        if (!needsEntry)
            return true;
        try {
            activities.try_emplace(key);
            return true;
        } catch (const std::bad_alloc &) {
            KisPageStoreDetail::setError(error, QStringLiteral("metadata activity storage preparation was refused"));
            return false;
        }
    }

    bool reservePublicationActivity(const KisPageKey &key, QString *error)
    {
        try { activities.try_emplace(key); }
        catch (const std::bad_alloc &) {
            KisPageStoreDetail::setError(error, QStringLiteral("metadata publication activity storage preparation was refused"));
            return false;
        }
        MetadataPageActivity next;
        if (const MetadataPageActivity *current = activity(key))
            next = *current;
        if (next.preparedPublications == std::numeric_limits<quint16>::max())
            return false;
        ++next.preparedPublications;
        updateActivity(key, next);
        return true;
    }

    void releasePublicationActivity(const KisPageKey &key)
    {
        auto current = activities.find(key);
        Q_ASSERT(current != activities.end() && current->second.preparedPublications > 0);
        if (current == activities.end() || current->second.preparedPublications == 0)
            return;
        --current->second.preparedPublications;
        if (current->second.isEmpty())
            activities.erase(current);
    }

    KisPageStateSnapshot header(const KisPageKey &key, const MetadataPage &page) const
    {
        return records.header(key, page, activity(key));
    }

    bool installHeader(const KisPageKey &key, MetadataPage *page, const KisPageStateSnapshot &state)
    {
        auto current = activities.find(key);
        MetadataPageActivity next;
        if (current != activities.end())
            next = current->second;
        if (!records.assignHeader(page, &next, state))
            return false;
        if (current != activities.end()) {
            if (next.isEmpty())
                activities.erase(current);
            else
                current->second = next;
        } else if (!next.isEmpty()) {
            qFatal("Metadata activity was not prepared before installation");
        }
        return true;
    }

    bool installReservedHeader(const KisPageKey &key, MetadataPage *page, const KisPageStateSnapshot &state)
    {
        auto current = activities.find(key);
        Q_ASSERT(current != activities.end());
        if (current == activities.end())
            return false;
        MetadataPageActivity next = current->second;
        if (!records.assignHeader(page, &next, state))
            return false;
        if (next.isEmpty())
            activities.erase(current);
        else
            current->second = next;
        return true;
    }

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
        return records.snapshot(key, page, activity(key));
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

const KisVersionRecord *readableVersion(const MetadataShard &shard, const KisPageVersion &identity)
{
    KisVersionSlotId slot;
    if (!(shard.pages.find(identity.key) != shard.pages.end()) || !shard.records.findVersion(identity, &slot)) return nullptr;
    const auto *version = shard.records.version(slot);
    return version && (version->publication == KisPagePublicationState::Published
        || version->publication == KisPagePublicationState::Historical
        || (version->publication == KisPagePublicationState::Prepared
            && (version->capturedReadViews.isValid() || shard.arenas.facts.get(version->facts)->nativeReaders)))
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
    static DataPointer create(const Storage &storage, size_t count, size_t shardCount, bool detachment);
    ~Data();
    void invoke() override;
    void dispose() noexcept override { DataDeleter{}(this); }
    qsizetype clearBatch(qsizetype maximumWorkUnits);

    struct Entry {
        std::shared_ptr<MetadataShard> shardOwner;
        quint64 revision = 0;
        KisPageVersion version;
        size_t inputOrdinal = 0;
        size_t shardIndex = 0;
        bool activityReservation = false;
        bool releaseUnusedCapacity = false;
        bool releasedUnusedCapacity = false;
        KisPagePrivateWrite *native = nullptr;
        // A recoverable CPU preparation installs the changed before and
        // reserves the caller's original result atomically, without a writer
        // version. This pointer owns nothing and expires with preparation.
        KisPagePrivateWrite *privateResult = nullptr;
        bool reuseNativeSlots = false;
        size_t nativeArenaGrowthIndex = std::numeric_limits<size_t>::max();
        std::shared_ptr<const KisPageAllocationDescriptor> releasedDescriptor;
        std::shared_ptr<KisCpuResidentBinding> releasedReadBinding;
        KisPageKey key() const { return version.key; }
    };
    struct Publication {
        explicit Publication(KisPageWorkingStorage storage)
            : deltas(storage), installRecords(decltype(installRecords)::allocator_type(storage)) {}
        KisPageStateSnapshot next;
        struct PublicationDelta {
            KisPageVersion version;
            KisPagePublicationState publication;
            KisPageTransactionId preparedBy;
        };
        Array<PublicationDelta> deltas;
        // Candidate values are contiguous; authoritative ordering is formed
        // by intrusive arena links only after full-batch revalidation.
        KisPageWorkingArray<Version, 2> installRecords;
        MetadataArenaDemand arenaDemand;
        size_t arenaGrowthIndex = std::numeric_limits<size_t>::max();
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
            entry.shardOwner->releasePublicationActivity(entry.key());
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
    bool retryNativeCapacity = false;
    Payload<Entry> entries;
    // Publication images belong only to publication/recoverable candidates.
    // Detachment retains the same claims and terminal policy in entries.
    Payload<Publication> publications;
    Payload<ShardArenaGrowth> arenaGrowth;
    // Page keys and projected versions are already unique. This candidate is
    // immutable before any authority lookup; one sorted paid array avoids
    // buckets and one charged allocation for every copied version identity.
    Array<std::pair<KisPageVersion, KisReplicaHandle>> backingAuthorities;

private:
    Data(const Storage &allocator, size_t units, Entry *entryData, size_t entryCount,
         Publication *publicationData, size_t publicationCount,
         ShardArenaGrowth *growthData, size_t growthCount)
        : storage(allocator), allocationUnits(units), entries(entryData, entryCount)
        , publications(publicationData, publicationCount), arenaGrowth(growthData, growthCount)
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
    const Storage &storage, size_t count, size_t shardCount, bool detachment)
{
    static_assert(alignof(Data) <= alignof(AllocationUnit));
    const size_t publicationCount = detachment ? 0 : count;
    size_t bytes = sizeof(Data);
    const size_t entryOffset = appendPayload<Entry>(bytes, count);
    const size_t publicationOffset = appendPayload<Publication>(bytes, publicationCount);
    const size_t growthOffset = appendPayload<ShardArenaGrowth>(bytes, shardCount);
    const size_t units = (bytes + sizeof(AllocationUnit) - 1) / sizeof(AllocationUnit);
    KisMutationStorageAllocator<AllocationUnit> allocator(storage);
    auto *raw = allocator.allocate(units);
    auto *base = reinterpret_cast<char *>(raw);
    try {
        return DataPointer(new (raw) Data(storage, units,
            reinterpret_cast<Entry *>(base + entryOffset), count,
            reinterpret_cast<Publication *>(base + publicationOffset), publicationCount,
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

KisPageMetadataCoordinator::PreparedPublication::PreparedPublication() = default;
KisPageMetadataCoordinator::PreparedPublication::~PreparedPublication() = default;
KisPageMetadataCoordinator::PreparedPublication::PreparedPublication(PreparedPublication &&) noexcept = default;
KisPageMetadataCoordinator::PreparedPublication &
KisPageMetadataCoordinator::PreparedPublication::operator=(PreparedPublication &&) noexcept = default;
bool KisPageMetadataCoordinator::PreparedPublication::isValid() const
{
    return data && data->owner;
}

KisReplicaHandle
KisPageMetadataCoordinator::PreparedPublication::backingAuthority(
    const KisPageVersion &version) const
{
    if (!data) return {};
    const auto found = std::lower_bound(data->backingAuthorities.cbegin(),
        data->backingAuthorities.cend(), version, [](const auto &entry, const KisPageVersion &key) {
            return MetadataKeyLess{}(entry.first, key);
        });
    if (found != data->backingAuthorities.cend() && found->first == version) return found->second;
    // Recoverable candidates already retain the exact base and target values
    // that will be installed. Their authority is derived from those values,
    // without another paid copy. Unrelated authorities remain indexed.
    if (data->kind == PreparationKind::RecoverableWrite) {
        for (const auto &publication : data->publications)
            for (const auto &record : publication.installRecords)
                if (record.version == version) return record.authority;
    }
    return {};
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

bool KisPageMetadataCoordinator::DeferredPublicationCleanup::needsNativeCapacity() const
{
    return data && data->retryNativeCapacity;
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
            if (!publications.empty()) publications.pop_back();
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
                if (!publications.empty()) publications.pop_back();
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
                                               QString *error) const
{
    return preparePublicationImpl(transaction, minimumEpoch, nullptr, count, false, error,
                                  PreparationKind::Publication, nullptr, changes);
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
                                            const KisPageVersion *versions, qsizetype count,
                                            QString *error) const
{
    return preparePublicationImpl(transaction, {}, nullptr, count, false, error, PreparationKind::Detachment, versions);
}

KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::prepareMutation(const KisPageTransaction &transaction,
                                            const MutationDetachment *versions, qsizetype count,
                                            QString *error) const
{
    return preparePublicationImpl(transaction, {}, nullptr, count, false, error,
        PreparationKind::Detachment, nullptr, nullptr, versions);
}

void KisPageMetadataCoordinator::DeferredPublicationCleanup::forEachReleasedMutation(
    void *context, void (*visit)(void *, size_t)) const
{
    if (!data) return;
    for (const auto &entry : data->entries)
        if (entry.releasedUnusedCapacity) visit(context, entry.inputOrdinal);
}

bool KisPageMetadataCoordinator::installMutation(PreparedPublication &&prepared,
                                                 const KisPageTransaction &transaction,
                                                 QString *error,
                                                 DeferredPublicationCleanup *deferredCleanup)
{
    return installPublicationImpl(std::move(prepared), transaction, {}, error,
                                  PreparationKind::Detachment, deferredCleanup);
}

KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::prepareRecoverableWrite(const KisPageTransaction &transaction,
                                                   const KisPageTransition &transition,
                                                   QString *error, KisPagePrivateWrite *privateResult) const
{
    return preparePublicationImpl(transaction, {}, &transition, 1, false, error,
        PreparationKind::RecoverableWrite, nullptr, nullptr, nullptr, privateResult);
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
                                                   const MutationDetachment *detachments,
                                                   KisPagePrivateWrite *privateResult) const
try {
    const bool mutation = kind != PreparationKind::Publication;
    const bool detachment = kind == PreparationKind::Detachment;
    const bool nativeDetachment = detachment && detachments && std::any_of(detachments, detachments + count,
        [](const auto &input) { return input.native; });
    const bool recoverable = kind == PreparationKind::RecoverableWrite;
    const bool privateWrite = kind == PreparationKind::PrivateWrite;
    const bool writePreparation = recoverable || privateWrite;
    PreparedPublication result;
    auto *d = m_core.load(std::memory_order_acquire);
    if (!d || (privateResult && (!recoverable || count != 1 || privateResult->reserved))
        || count < 0 || (count && !transitions && !versions && !changes && !detachments)
        || (!restoration && !transaction.isValid())
        || (!mutation && (!minimumEpoch.isValid() || minimumEpoch.value <= transaction.baseEpoch.value))) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata publication identity is invalid"));
        return result;
    }
    const auto storageOwner = d->budgetAuthority->storage<char>();
    const size_t bytesPerPage = sizeof(PreparedPublication::Data::Entry)
        + (detachment ? 0 : sizeof(PreparedPublication::Data::Publication));
    if (size_t(count) > size_t(std::numeric_limits<qint64>::max()) / bytesPerPage)
        throw std::bad_alloc();
    size_t shardCount = 0;
    if ((!detachment || nativeDetachment) && count) {
        if (count == 1) shardCount = 1;
        else {
            // configure() bounds shards at4096. This stack capacity census
            // supplies no page authority; the existing preparation below
            // still validates and retains each exact shard/page identity.
            std::bitset<4096> usedShards;
            for (qsizetype i = 0; i < count; ++i) {
                const auto &key = detachments ? detachments[i].version.key : versions ? versions[i].key
                    : changes ? changes[i].version.key : transitions[i].version.key;
                usedShards.set(kisStablePageKeyHash(key) % d->shards.size());
            }
            shardCount = usedShards.count();
        }
    }
    auto data = PreparedPublication::Data::create(storageOwner, size_t(count), shardCount, detachment);
    const auto storage = data->storage.borrowed();
    data->owner = d->publicationOwner;
    data->transaction = transaction;
    data->minimumEpoch = minimumEpoch;
    data->kind = kind;
    const KisPageStateMachine stateMachine;
    const auto transitionAt = [&](size_t i, KisPageTransition &input) -> const KisPageTransition & {
        if (versions || detachments) {
            input.kind = detachment ? KisPageTransitionKind::DetachPreparedVersion
                                    : KisPageTransitionKind::RestoreCommittedVersion;
            input.version = detachments ? detachments[i].version : versions[i];
            input.transaction = detachment ? transaction.id : KisPageTransactionId{};
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
        return versions || changes || detachments ? input : transitions[i];
    };
    for (qsizetype i = 0; i < count; ++i) {
        KisPageTransition input;
        const auto &transition = transitionAt(size_t(i), input);
        const bool write = transition.kind == KisPageTransitionKind::CommitTransaction;
        if ((detachment && !transition.version.isValid())
            || (recoverable && (transition.kind != KisPageTransitionKind::AcquireRecoverableWrite
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
        std::shared_ptr<MetadataShard> shardOwner = Private::sharedShardFor(d, transition.version.key);
        if (!shardOwner)
            return result;
        PreparedPublication::Data::Entry entry;
        entry.shardOwner = std::move(shardOwner);
        entry.version = transition.version;
        entry.inputOrdinal = size_t(i);
        entry.releaseUnusedCapacity = detachments && detachments[i].releaseUnusedCapacity;
        entry.native = detachments ? detachments[i].native : nullptr;
        entry.privateResult = privateResult;
        if (privateResult) privateResult->target = transition.target;
        entry.shardIndex = kisStablePageKeyHash(entry.key()) % d->shards.size();
        data->entries.push_back(std::move(entry));
        if (!detachment)
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
                if (page == shard->pages.cend() || !shard->canMutate(transition.version.key, page->second)) {
                    KisPageStoreDetail::setError(error, QStringLiteral("metadata publication page is unavailable"));
                    return result;
                }
                if (detachment) {
                    // Match DetachPreparedVersion's semantic guards without
                    // cloning all versions/protection lists into a stale image.
                    const auto *activity = shard->activity(entry.key());
                    const bool ownNative = entry.native && activity && activity->nativeWrite == entry.native
                        && entry.native->completed && entry.native->proof.isValid()
                        && entry.native->proof.transaction == transaction.id;
                    if ((shard->hasWriter(entry.key()) && !ownNative)
                        || (entry.native && !ownNative)
                        || ((!entry.native || !(entry.version == entry.native->target.version))
                            && !shard->records.isPreparedBy(entry.version, transaction.id))) {
                        KisPageStoreDetail::setError(error, QStringLiteral("prepared detachment identity is invalid or still writable"));
                        return result;
                    }
                    ++shard->publicationHistoryNodesPrepared;
                } else {
                    entry.revision = page->second.revision;
                    auto &publication = data->publications[i];
                    publication.next = shard->header(transition.version.key, page->second);
                    auto &facts = publication.installRecords;
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
                        include({publication.next.key, publication.next.publishedGeneration,
                            publication.next.publishedDefaultPixelRevision});
                    if (writePreparation)
                        include(transition.baseVersion);
                    include(transition.version);
                    if (write) {
                        const auto *activity = shard->activity(entry.key());
                        if (activity && activity->nativeWrite) {
                            entry.native = activity->nativeWrite;
                            if (!entry.native->completed || !entry.native->proof.isValid()
                                || !(entry.native->target.version == transition.version)
                                || !(entry.native->proof.transaction == transaction.id)) return result;
                            // This candidate owns the eventual reservation to
                            // version conversion; unrelated transitions keep
                            // the reservation visible to the common policy.
                            publication.next.nativeGeneration = {};
                            publication.next.nativeTransaction = {};
                        }
                        // Only this transaction's Prepared versions are relevant.
                        shard->records.forPreparedTransaction(page->second, transaction.id, include);
                    } else {
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
                    }
                    shard->publicationVersionInputs += quint64(facts.size());
                }
                if (entry.native && !detachment) {
                    auto &facts = data->publications[i].installRecords;
                    KisPageWorkingVersion target(storage);
                    target.version = entry.native->target.version;
                    target.publication = KisPagePublicationState::Prepared;
                    target.preparedBy = transaction.id;
                    target.authority = entry.native->target;
                    KisPageWorkingReplica replica(storage);
                    replica.replica = entry.native->target;
                    replica.validity = KisReplicaValidity::Valid;
                    target.replicas.push_back(std::move(replica));
                    facts.push_back(std::move(target));
                }
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
                shard->publicationHistoryNodesPrepared += delta.publication == KisPagePublicationState::Historical;
            for (const auto &record : publication.installRecords)
                shard->publicationHistoryNodesPrepared += record.publication == KisPagePublicationState::Historical;
        }
        shard->publicationAdditionRecordsPrepared += publication.installRecords.size() - (recoverable ? 1 : 0);
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
    for (size_t i = 0; !detachment && i < data->entries.size(); ++i) {
        auto &entry = data->entries[i];
        auto &publication = data->publications[i];
        KisPageTransition input;
        const auto &transition = transitionAt(entry.inputOrdinal, input);
        KisPageWorkingResult step(storage);
        auto &before = step.next;
        before.setHeader(publication.next);
        before.versions = std::move(publication.installRecords);
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
        if (data->backingAuthorities.empty())
            data->backingAuthorities.reserve(size_t(std::count_if(step.next.versions.cbegin(),
                step.next.versions.cend(), copiedAuthority)));
        for (const auto &version : std::as_const(step.next.versions))
            if (copiedAuthority(version)) data->backingAuthorities.emplace_back(version.version, version.authority);
        if (privateWrite) {
            // The original base and all of its protection stay in the arena.
            // The next writer identifies its one pin delta; the strict page
            // revision claim makes that decision valid until installation.
            auto &records = step.next.versions;
            records.erase(std::remove_if(records.begin(), records.end(), [&](const auto &record) {
                return !(record.version == transition.version);
            }), records.end());
            Q_ASSERT(records.size() == 1);
            publication.installRecords = std::move(records);
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
            if (entry.privateResult) {
                // The scope's transaction retains the logical before; the
                // coordinator retains its physical read pin until retag and
                // initialization finish. Only the lease producer requires
                // the synthetic writer's metadata pin and target record.
                auto &base = records[0];
                const auto replica = std::find_if(base.replicas.begin(), base.replicas.end(),
                    [&](const auto &r) { return r.replica == transition.source; });
                Q_ASSERT(replica != base.replicas.end() && replica->pinCount);
                --replica->pinCount;
                records.pop_back();
                step.next.writer = {};
            }
            publication.installRecords = std::move(records);
        } else {
            for (size_t index = inputCount; index < step.next.versions.size(); ++index) {
                const auto &version = step.next.versions[index];
                if (version.publication == KisPagePublicationState::Prepared
                    || version.publication == KisPagePublicationState::Unpublished) {
                    KisPageStoreDetail::setError(error, QStringLiteral("publication candidate introduced mutable metadata"));
                    return result;
                }
            }
            if (entry.native) {
                auto &records = step.next.versions;
                records.erase(std::remove_if(records.begin(), records.end(), [&](const auto &v) {
                    return !(v.version == entry.native->target.version);
                }), records.end());
                publication.installRecords = std::move(records);
                publication.deltas.erase(std::remove_if(publication.deltas.begin(), publication.deltas.end(), [&](const auto &v) {
                    return v.version == entry.native->target.version;
                }), publication.deltas.end());
            } else if (step.next.versions.size() > inputCount) {
                // Transfer the appended values and their paid storage; old
                // replica/protection values can be destroyed during prepare.
                auto &records = step.next.versions;
                records.erase(records.begin(), records.begin() + inputCount);
                publication.installRecords = std::move(records);
            }
        }
        // Ordinary publication only changes publication fields. Recoverable
        // writes replace the strictly revision-bound base record and add T;
        // any intervening reader/pin/last-use change rejects that candidate.
        ++preparedPolicies;
        // Headers never own version records at install.
        publication.next = step.next.header();
        Q_ASSERT(step.effects.empty());
    }
    for (size_t first = 0; (!detachment || nativeDetachment) && first < data->entries.size();) {
        const size_t end = data->batchEnd(first, data->entries.size());
        auto *shard = data->entries[first].shardOwner.get();
        QMutexLocker locker(&shard->mutex);
        for (size_t i = first; i < end; ++i) {
            auto &entry = data->entries[i];
            const auto page = entry.shardOwner->pages.find(entry.key());
            if (page == entry.shardOwner->pages.end() || !entry.shardOwner->canMutate(entry.key(), page->second)
                || (!detachment && page->second.revision != entry.revision)) {
                KisPageStoreDetail::setError(error, QStringLiteral("metadata changed while preparing publication storage"));
                result.m_failure = PreparedPublication::Failure::Conflict;
                return result;
            }
            entry.reuseNativeSlots = detachment && entry.native && entry.releaseUnusedCapacity
                && !entry.native->freshMetadataRequired && entry.shardOwner->records.canReplaceUnused(entry.version);
            MetadataArenaDemand demand;
            if (detachment) {
                if (entry.native && !entry.reuseNativeSlots) { demand.versions = 1; demand.replicas = 1; }
                shard->publicationAdditionRecordsPrepared += demand.versions;
            } else {
                auto &publication = data->publications[i];
                demand = publication.arenaDemand = shard->records.batchDemand(publication.installRecords);
                recordPolicyWork(i);
                ++recordedPolicies;
            }
            if (data->arenaGrowth.empty() || data->arenaGrowth.back().shardOwner != entry.shardOwner) {
                PreparedPublication::Data::ShardArenaGrowth nextGrowth(storage);
                nextGrowth.shardOwner = entry.shardOwner;
                nextGrowth.firstEntry = i;
                data->arenaGrowth.push_back(std::move(nextGrowth));
            }
            auto *shardGrowth = &data->arenaGrowth.back();
            ++shardGrowth->entryCount;
            shardGrowth->totalDemand.versions += demand.versions;
            shardGrowth->totalDemand.replicas += demand.replicas;
            shardGrowth->totalDemand.overflow += demand.overflow;
            if (detachment) {
                entry.nativeArenaGrowthIndex = data->arenaGrowth.size() - 1;
                shardGrowth->exactInsertions += demand.versions;
                shardGrowth->physicalInsertions += demand.replicas;
            } else {
                auto &publication = data->publications[i];
                publication.arenaGrowthIndex = data->arenaGrowth.size() - 1;
                shardGrowth->exactInsertions += qsizetype(publication.installRecords.size()) - (recoverable ? 1 : 0);
                for (const auto &record : publication.installRecords)
                    shardGrowth->physicalInsertions += record.replicas.size();
            }
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
                const auto page = growth.shardOwner->pages.find(entry.key());
                if (page == growth.shardOwner->pages.cend() || !growth.shardOwner->canMutate(entry.key(), page->second)
                    || (!detachment && page->second.revision != entry.revision)) {
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

    // Pre-create one sparse activity entry per page. Claim installation then
    // mutates an existing entry in place and cannot grow/rehash the activity
    // map while the commit gate is held.
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
    for (size_t first = 0; first < data->entries.size();) {
        const size_t end = data->batchEnd(first, data->entries.size());
        QMutexLocker locker(&data->entries[first].shardOwner->mutex);
        for (size_t i = first; i < end; ++i) {
            auto &entry = data->entries[i];
            const auto page = entry.shardOwner->pages.find(entry.key());
            bool stillValid = page != entry.shardOwner->pages.cend() && entry.shardOwner->canMutate(entry.key(), page->second);
            if (stillValid && detachment) {
                const auto *activity = entry.shardOwner->activity(entry.key());
                stillValid = entry.native ? activity && activity->nativeWrite == entry.native : !entry.shardOwner->hasWriter(entry.key());
                stillValid = stillValid && ((entry.native && entry.version == entry.native->target.version)
                    || entry.shardOwner->records.isPreparedBy(entry.version, transaction.id));
            } else if (stillValid) {
                stillValid = page->second.revision == entry.revision;
            }
            if (!stillValid || !entry.shardOwner->reservePublicationActivity(entry.key(), error)) {
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
        [](const auto &a, const auto &b) { return MetadataKeyLess{}(a.first, b.first); });
    // No input is read after preparation. Deliver in the original owner's
    // stable array coordinates without another index or a lookup per page.
    if (detachments)
        for (auto &entry : data->entries)
            entry.inputOrdinal = detachments[entry.inputOrdinal].ownerIndex;
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
    const bool detachment = kind == PreparationKind::Detachment;
    const bool recoverable = kind == PreparationKind::RecoverableWrite;
    const bool privateWrite = kind == PreparationKind::PrivateWrite;
    // Consume even on rejection: callers must prepare again after a conflict.
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
            auto page = entry.shardOwner->pages.find(entry.key());
            if (page == entry.shardOwner->pages.end() || !entry.shardOwner->canMutate(entry.key(), page->second))
                break;
            if (detachment) {
                const auto *activity = entry.shardOwner->activity(entry.key());
                if (entry.native ? !activity || activity->nativeWrite != entry.native
                                    || !entry.native->completed || !entry.native->proof.isValid()
                    : entry.shardOwner->hasWriter(entry.key()))
                    break;
                if ((!entry.native || !(entry.version == entry.native->target.version))
                    && !entry.shardOwner->records.isPreparedBy(entry.version, transaction.id))
                    break;
                if (entry.reuseNativeSlots && !entry.shardOwner->records.canReplaceUnused(entry.version)) {
                    data->retryNativeCapacity = true;
                    ++entry.shardOwner->nativeCapacityRetries;
                    break;
                }
            } else if (page->second.revision != entry.revision)
                break;
            if (entry.privateResult && entry.privateResult->reserved) break;
            Q_ASSERT(entry.activityReservation);
            // Keep the existing reservation until this Data is cleaned outside
            // installation. Clearing the transient claim cannot then erase/free
            // an otherwise empty activity node under the publication gate.
            entry.shardOwner->setPublicationClaim(entry.key(), data.get());
            ++claimed;
        }
        if (claimed != qsizetype(end)) break;
        first = end;
    }
    if (claimed != qsizetype(data->entries.size())) {
        // The original mutation retains the completed pixels. One late reader
        // switches the whole D to prepaid additions for its one cold retry.
        if (data->retryNativeCapacity)
            for (auto &entry : data->entries)
                if (entry.native) entry.native->freshMetadataRequired = true;
        for (size_t first = 0; first < size_t(claimed);) {
            const size_t end = data->batchEnd(first, size_t(claimed));
            QMutexLocker locker(&data->entries[first].shardOwner->mutex);
            for (size_t i = first; i < end; ++i)
                data->entries[i].shardOwner->setPublicationClaim(data->entries[i].key(), nullptr);
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
            auto *publication = data->publications.empty() ? nullptr : &data->publications[i];
            auto page = entry.shardOwner->pages.find(entry.key());
            Q_ASSERT(page != entry.shardOwner->pages.end() && entry.shardOwner->publicationClaim(entry.key()) == data.get());
            quint64 installedHistoryLinks = detachment ? 1 : 0;
            if (!mutation) {
                installedHistoryLinks +=
                    quint64(std::count_if(publication->deltas.cbegin(), publication->deltas.cend(), [](const auto &delta) {
                        return delta.publication == KisPagePublicationState::Historical;
                    }));
                installedHistoryLinks +=
                    quint64(std::count_if(publication->installRecords.cbegin(), publication->installRecords.cend(), [](const auto &addition) {
                        return addition.publication == KisPagePublicationState::Historical;
                    }));
            }
            // Epoch allocation may have skipped an identity on an earlier failed
            // root build. These transitions use the epoch only as a newer tag.
            if (entry.reuseNativeSlots) {
                entry.shardOwner->records.replaceUnused(&page->second, entry.version, *entry.native,
                    &entry.releasedDescriptor, &entry.releasedReadBinding);
                ++entry.shardOwner->nativeSlotReplacements;
                entry.releasedUnusedCapacity = true;
                installedHistoryLinks = 0;
                ++page->second.revision;
            } else if (detachment && (!entry.native || !(entry.version == entry.native->target.version))) {
                // Claims on every page are held before the first edit. Preserve
                // the CURRENT replicas, leases, pins, last-use, captured views and
                // epoch; apply only the already revalidated semantic delta. This
                // is not permission to merge arbitrary stale publication images.
                auto &records = entry.shardOwner->records;
                KisVersionSlotId slot;
                const bool found = records.findVersion(entry.version, &slot);
                Q_ASSERT(found);
                const auto *record = records.version(slot);
                const auto *replica = records.replica(record->authorityReplica);
                // This exact Prepared version was never canonical. Captures
                // and read/operation protection remain authoritative here,
                // after all pages have been claimed. Raw/clone protection is
                // physical and will be checked again on capacity checkout.
                const bool unused = entry.releaseUnusedCapacity && !record->version.isDefaultPixel()
                    && !record->capturedReadViews.isValid()
                    && !entry.shardOwner->arenas.facts.get(record->facts)->nativeReaders && record->replicaCount == 1
                    && replica && replica->domain == KisPageAccessDomain::CpuRam
                    && replica->validity == KisReplicaValidity::Valid && !replica->pinCount
                    && !replica->overflow.isValid() && !replica->activeOperation.isValid()
                    && entry.shardOwner->arenas.facts.get(record->facts)->stamp;
                if (unused) {
                    records.remove(&page->second, entry.version, &entry.releasedDescriptor,
                                   &entry.releasedReadBinding);
                    entry.releasedUnusedCapacity = true;
                    installedHistoryLinks = 0;
                } else {
                    const bool updated = records.setPublication(&page->second, entry.version,
                        KisPagePublicationState::Historical, {});
                    Q_ASSERT(updated);
                    Q_UNUSED(updated);
                }
                ++page->second.revision;
            } else if (detachment) {
                installedHistoryLinks = 0;
                ++page->second.revision;
            } else if (recoverable) {
                auto &reservation = data->arenaGrowth.at(publication->arenaGrowthIndex).reservations;
                for (const auto &record : publication->installRecords) {
                    const bool installed = entry.shardOwner->records.putReserved(&page->second, record, &reservation);
                    Q_ASSERT(installed);
                    Q_UNUSED(installed);
                }
                const bool headerInstalled = entry.shardOwner->installReservedHeader(entry.key(), &page->second, publication->next);
                Q_ASSERT(headerInstalled);
                Q_UNUSED(headerInstalled);
                if (entry.privateResult) {
                    auto &activity = entry.shardOwner->activities.find(entry.key())->second;
                    Q_ASSERT(!activity.nativeWrite);
                    activity.nativeWrite = entry.privateResult;
                    activity.writerTransaction = transaction.id;
                    entry.privateResult->reserved = true;
                    ++entry.shardOwner->nativeWriteReservations;
                }
                ++page->second.revision;
                entry.shardOwner->publicationAdditionRecordsTransferred += !entry.privateResult;
                entry.shardOwner->publicationVersionInstalls += publication->installRecords.size();
            } else {
                if (!privateWrite) publication->next.publishedEpoch = epoch;
                for (const auto &delta : std::as_const(publication->deltas)) {
                    const bool updated = entry.shardOwner->records.setPublication(&page->second,
                                                                             delta.version,
                                                                             delta.publication,
                                                                             delta.preparedBy);
                    Q_ASSERT(updated);
                    Q_UNUSED(updated);
                }
                const auto additionRecords = quint64(publication->installRecords.size());
                auto &reservation = data->arenaGrowth.at(publication->arenaGrowthIndex).reservations;
                const bool additionsInstalled = entry.shardOwner->records.installPreparedAdditionsReserved(&page->second,
                                                                                                      &publication->installRecords,
                                                                                                      &reservation);
                Q_ASSERT(additionsInstalled);
                Q_UNUSED(additionsInstalled);
                if (privateWrite && publication->next.writer.baseAuthority.isValid()) {
                    const auto &writer = publication->next.writer;
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
                const bool headerInstalled = entry.shardOwner->installReservedHeader(entry.key(), &page->second, publication->next);
                Q_ASSERT(headerInstalled);
                Q_UNUSED(headerInstalled);
                ++page->second.revision;
                entry.shardOwner->publicationVersionInstalls += quint64(publication->deltas.size()) + additionRecords;
            }
            if (entry.native) {
                auto *native = entry.native;
                if (detachment && !entry.reuseNativeSlots) {
                    auto &reservation = data->arenaGrowth.at(entry.nativeArenaGrowthIndex).reservations;
                    entry.shardOwner->records.putNativeReserved(&page->second, *native, &reservation);
                    ++entry.shardOwner->publicationAdditionRecordsTransferred;
                    ++entry.shardOwner->publicationVersionInstalls;
                }
                KisVersionSlotId slot;
                const bool found = entry.shardOwner->records.findVersion(native->target.version, &slot);
                Q_ASSERT(found); Q_UNUSED(found);
                const auto *record = entry.shardOwner->records.version(slot);
                auto *facts = entry.shardOwner->arenas.facts.get(record->facts);
                facts->descriptor = native->descriptor;
                facts->stamp = native->proof.providerValidationStamp;
                facts->transaction = native->proof.transaction;
                facts->nativeBinding = native->binding;
                facts->nativeReplica = record->authorityReplica;
                entry.shardOwner->arenas.cpuReadBindings += bool(native->binding);
                auto activity = entry.shardOwner->activities.find(entry.key());
                Q_ASSERT(activity != entry.shardOwner->activities.end() && activity->second.nativeWrite == native);
                activity->second.nativeWrite = nullptr;
                native->reserved = false;
            }
            entry.shardOwner->publicationHistoryNodesTransferred += installedHistoryLinks;
            ++(mutation ? entry.shardOwner->installedMutationPages
                        : entry.shardOwner->installedPublicationPages);
            // All page facts were claimed before the first install; after this
            // page's final write no later step can overwrite a new reader/last-use
            // mutation here. Release under the same lock, not a third lock pass.
            entry.shardOwner->setPublicationClaim(entry.key(), nullptr);
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

void KisPageMetadataCoordinator::removeCpuReadBinding(const KisReplicaHandle &replica,
                                                      const KisCpuResidentBinding *expected)
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), replica.version.key);
    if (!shard)
        return;
    std::shared_ptr<KisCpuResidentBinding> released;
    QMutexLocker locker(&shard->mutex);
    KisVersionSlotId slot;
    if (!shard->records.findVersion(replica.version, &slot)) return;
    auto *facts = shard->arenas.facts.get(shard->records.version(slot)->facts);
    const auto *selected = shard->records.replica(facts->nativeReplica);
    if (facts->nativeBinding && (!selected || replicaHandle(*selected, replica.version) == replica)
        && (!expected || facts->nativeBinding.get() == expected)) {
        released.swap(facts->nativeBinding);
        facts->nativeReplica = {};
        --shard->arenas.cpuReadBindings;
    }
}

KisCpuReadBinding KisPageMetadataCoordinator::cpuReadBinding(const KisPageVersion &version) const
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), version.key);
    if (!shard)
        return {};
    QMutexLocker locker(&shard->mutex);
    const auto *record = readableVersion(*shard, version);
    if (!record) return {};
    const auto *facts = shard->arenas.facts.get(record->facts);
    const auto *selected = shard->records.replica(facts->nativeReplica);
    return selected && cpuReadableReplica(*selected, version)
        ? KisCpuReadBinding{replicaHandle(*selected, version), facts->nativeBinding} : KisCpuReadBinding{};
}

namespace {
template<class Shard>
KisCpuReadBinding nativeReadBindingLocked(const Shard &shard, const KisVersionRecord &version)
{
    const auto *facts = shard.arenas.facts.get(version.facts);
    const auto *replica = shard.records.replica(facts->nativeReplica);
    return replica && cpuReadableReplica(*replica, version.version)
        ? KisCpuReadBinding{replicaHandle(*replica, version.version), facts->nativeBinding} : KisCpuReadBinding{};
}
}

size_t KisPageMetadataCoordinator::retainNativeReads(KisPageNativeReadRef *references, size_t &next, size_t count,
    void *context, bool (*select)(void *, quint32, KisPageVersion *),
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
        if (reference.shard != shardIndex || !select(context, quint32(next), &identity)
            || !identity.isValid() || reference.version.isValid()
            || reference.shard != kisStablePageKeyHash(identity.key) % core->shards.size()) break;
        if (identity.isDefaultPixel()) { next = reference.next; continue; }
        auto page = shard->pages.find(identity.key);
        KisVersionSlotId slot;
        if (page == shard->pages.end() || !shard->canMutate(identity.key, page->second)
            || !shard->records.findVersion(identity, &slot)) break;
        const auto *version = shard->records.version(slot);
        auto *facts = shard->arenas.facts.get(version->facts);
        if (version->publication == KisPagePublicationState::Unpublished
            || version->publication == KisPagePublicationState::Retiring
            || facts->nativeReaders == std::numeric_limits<quint32>::max()) break;
        ++facts->nativeReaders;
        ++page->second.revision;
        reference.version = slot;
        if (next < bindingCount) {
            Q_ASSERT(firstBindings && !firstBindings[next]);
            firstBindings[next] = nativeReadBindingLocked(*shard, *version);
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
    if (binding) *binding = nativeReadBindingLocked(*shard, *version);
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
                bindings[index - first] = nativeReadBindingLocked(*shard, *version);
        } while (i < count && references[indices[i]].shard == shardIndex);
    }
}

KisReplicaHandle KisPageMetadataCoordinator::cpuReadReplica(const KisPageVersion &version) const
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), version.key);
    if (!shard)
        return {};
    QMutexLocker locker(&shard->mutex);
    const auto *state = readableVersion(*shard, version);
    if (!state)
        return {};
    // Cold discovery does not export/copy a diagnostic page snapshot. Prefer
    // a ready CPU authority, otherwise require an unambiguous ready CPU
    // replica. Authority elsewhere alone is not a materialization request.
    const auto *authority = shard->records.replica(state->authorityReplica);
    if (authority && cpuReadableReplica(*authority, version)) return replicaHandle(*authority, version);
    KisReplicaHandle selected;
    for (auto slot = state->firstReplica; slot.isValid();) {
        const auto *candidate = shard->records.replica(slot);
        slot = candidate->nextReplica;
        if (!cpuReadableReplica(*candidate, version)) continue;
        if (selected.isValid())
            return {};
        selected = replicaHandle(*candidate, version);
    }
    return selected;
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

bool KisPageMetadataCoordinator::queryMutationBase(const KisPageVersion &base,
                                                   const KisPageVersion &sealed,
                                                   bool discoverBefore, MutationBaseInfo *info) const
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), base.key);
    if (!shard || !info || (sealed.isValid() && !(sealed.key == base.key)))
        return false;
    QMutexLocker lock(&shard->mutex);
    const auto page = shard->pages.find(base.key);
    if (page == shard->pages.cend())
        return false;
    MutationBaseInfo next;
    next.hasWriter = shard->hasWriter(base.key);
    next.nextGeneration = page->second.nextGeneration;
    const auto *published = shard->records.version(page->second.publishedVersion);
    Q_ASSERT(published);
    const auto selected = sealed.isValid() ? sealed : base;
    KisVersionSlotId selectedSlot;
    const auto *record = shard->records.findVersion(selected, &selectedSlot) ? shard->records.version(selectedSlot) : nullptr;
    next.baseExists = base.isValid() && (base == selected ? bool(record) : shard->records.findVersion(base));
    next.selected = shard->records.versionInfo<VersionInfo>(record, published->version.generation);
    if (record) {
        const auto *facts = shard->arenas.facts.get(record->facts);
        next.descriptor = facts->descriptor;
        if (facts->nativeReplica == record->authorityReplica) next.binding = facts->nativeBinding;
    }
    if (discoverBefore && !next.hasWriter && next.selected.version.isValid() && !selected.isDefaultPixel()
        && next.selected.replicaCount > 1) {
        const auto *version = record;
        const auto *authority = shard->records.replica(version->authorityReplica);
        // Replica overflow contains read leases and pending last uses; captured
        // views belong to the version and do not exclude an independent before.
        // This only rejects known blockers. Install still revalidates the cut.
        if (authority && authority->validity == KisReplicaValidity::Valid
            && !authority->activeOperation.isValid() && !authority->pinCount
            && !authority->overflow.isValid()) {
            for (auto replicaSlot = version->firstReplica; replicaSlot.isValid();) {
                const auto *replica = shard->records.replica(replicaSlot);
                const auto handle = replicaHandle(*replica, selected);
                if (replica->validity == KisReplicaValidity::Valid && !replica->activeOperation.isValid()
                    && handle.domain == KisPageAccessDomain::CpuRam
                    && handle.layout == next.selected.authority.layout
                    && !(handle.physicalSlotIdentity() == next.selected.authority.physicalSlotIdentity())) {
                    next.recoverableBefore = handle;
                    break;
                }
                replicaSlot = replica->nextReplica;
            }
        }
    }
    const quint64 inputs = quint64(next.baseExists)
        + quint64(sealed.isValid() && !(sealed == base) && next.selected.version.isValid());
    shard->mutationBaseVersionInputs += inputs;
    if (kisOnPageStoreReclamationThread())
        shard->backgroundMutationBaseVersionInputs += inputs;
    *info = next;
    return true;
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
            const auto *activity = shard->activity(identity.key);
            const auto *result = activity ? activity->nativeWrite : nullptr;
            if (result && result->completed && result->target.version == identity) {
                if (replicas && replicas->capacity() < 1) {
                    lock.unlock(); replicas->reserve(1); continue;
                }
                next.version = identity;
                next.authority = result->target;
                next.publication = KisPagePublicationState::Prepared;
                next.preparedBy = activity->writerTransaction;
                next.replicaCount = 1;
                if (replicas) { replicas->clear(); replicas->push_back({result->target, KisReplicaValidity::Valid}); }
            } else if (replicas) replicas->clear();
        }
        *snapshot = next;
        return true; // Existing page and missing exact version remain distinct.
    }
}

std::shared_ptr<const KisPageAllocationDescriptor>
KisPageMetadataCoordinator::versionDescriptor(const KisPageVersion &identity) const
{
    const auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), identity.key);
    if (!shard) return {};
    QMutexLocker lock(&shard->mutex);
    KisVersionSlotId slot;
    if (!shard->records.findVersion(identity, &slot)) {
        const auto *activity = shard->activity(identity.key);
        const auto *native = activity ? activity->nativeWrite : nullptr;
        return native && native->target.version == identity ? native->descriptor : nullptr;
    }
    return shard->arenas.facts.get(shard->records.version(slot)->facts)->descriptor;
}

const KisPageAllocationDescriptor *
KisPageMetadataCoordinator::borrowVersionDescriptor(const KisPageVersion &identity) const
{
    const auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), identity.key);
    if (!shard) return nullptr;
    QMutexLocker lock(&shard->mutex);
    KisVersionSlotId slot;
    if (!shard->records.findVersion(identity, &slot)) {
        const auto *activity = shard->activity(identity.key);
        const auto *native = activity ? activity->nativeWrite : nullptr;
        return native && native->target.version == identity ? native->descriptor.get() : nullptr;
    }
    return shard->arenas.facts.get(shard->records.version(slot)->facts)->descriptor.get();
}

bool KisPageMetadataCoordinator::exchangeVersionDescriptor(const KisPageVersion &identity,
    std::shared_ptr<const KisPageAllocationDescriptor> *prepared)
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), identity.key);
    if (!shard || !prepared) return false;
    QMutexLocker lock(&shard->mutex);
    KisVersionSlotId slot;
    if (!shard->records.findVersion(identity, &slot)) {
        const auto *activity = shard->activity(identity.key);
        auto *native = activity ? activity->nativeWrite : nullptr;
        if (!native || !(native->target.version == identity)) return false;
        native->descriptor.swap(*prepared);
        return true;
    }
    shard->arenas.facts.get(shard->records.version(slot)->facts)->descriptor.swap(*prepared);
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

bool KisPageMetadataCoordinator::reservePrivateWrite(const KisPageTransition &write,
    KisPagePrivateWrite *native, quint32 limit, QString *error)
try {
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), write.version.key);
    if (!shard || !native || native->reserved || !write.target.isValid()
        || !(write.target.version == write.version) || !write.transaction.isValid()
        || !write.operation.isValid() || !write.writer.isValid()) return false;
    QMutexLocker lock(&shard->mutex);
    auto page = shard->pages.find(write.version.key);
    KisVersionSlotId baseSlot;
    KisPageVersion physicalOwner;
    if (page == shard->pages.end() || !shard->canMutate(write.version.key, page->second)
        || shard->hasWriter(write.version.key)
        || !shard->records.findVersion(write.baseVersion, &baseSlot)
        || !(write.version.generation == page->second.nextGeneration)
        || write.version.generation.value <= write.baseVersion.generation.value
        || write.version.generation.value == std::numeric_limits<quint64>::max()
        || shard->records.physicalOwner(write.target.physicalSlotIdentity(), &physicalOwner)) return false;
    const auto *base = shard->records.version(baseSlot);
    const auto *replica = shard->records.replica(base->authorityReplica);
    if ((base->publication != KisPagePublicationState::Published
         && base->publication != KisPagePublicationState::Prepared)
        || (base->publication == KisPagePublicationState::Prepared && !(base->preparedBy == write.transaction))
        || (!(shard->records.projectReplica(base->authorityReplica) == write.source)
            && !(base->version.isDefaultPixel() && !write.source.isValid()))
        || (!base->version.isDefaultPixel() && (!replica || replica->validity != KisReplicaValidity::Valid))) return false;
    Q_ASSERT(page->second.versionCount > page->second.historyCount);
    if (page->second.versionCount <= page->second.historyCount) return false;
    const quint64 transient = quint64(page->second.versionCount) - page->second.historyCount - 1;
    if (transient >= limit) return false;
    auto &activity = shard->activities.try_emplace(write.version.key).first->second;
    activity.nativeWrite = native;
    activity.writerTransaction = write.transaction;
    native->target = write.target;
    native->reserved = true;
    ++shard->nativeWriteReservations;
    ++page->second.nextGeneration.value;
    ++page->second.revision;
    KisPageStoreDetail::setError(error, {});
    return true;
} catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("native write activity storage was refused"));
    return false;
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

bool KisPageMetadataCoordinator::completePrivateWrite(KisPagePrivateWrite *native)
{
    if (!native) return false;
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), native->target.version.key);
    if (!shard) return false;
    QMutexLocker lock(&shard->mutex);
    const auto *activity = shard->activity(native->target.version.key);
    if (!activity || activity->nativeWrite != native || !native->reserved) return false;
    native->completed = true;
    return true;
}

bool KisPageMetadataCoordinator::replacePrivateWrite(KisPagePrivateWrite *result,
    const KisReplicaHandle &target)
{
    if (!result || !target.isValid() || !(target.version == result->target.version)) return false;
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), target.version.key);
    if (!shard) return false;
    QMutexLocker lock(&shard->mutex);
    auto *activity = shard->activity(target.version.key);
    KisPageVersion physicalOwner;
    if (!activity || activity->nativeWrite != result || !result->reserved || !result->completed
        || activity->publicationClaim || result->proof.isValid()
        || target.physicalSlotIdentity() == result->target.physicalSlotIdentity()
        || shard->records.physicalOwner(target.physicalSlotIdentity(), &physicalOwner)) return false;
    result->target = target;
    result->completed = true;
    ++shard->pages.find(target.version.key)->second.revision;
    return true;
}

bool KisPageMetadataCoordinator::cancelPrivateResult(KisPagePrivateWrite *native)
{
    if (!native) return false;
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), native->target.version.key);
    if (!shard) return false;
    QMutexLocker lock(&shard->mutex);
    auto activity = shard->activities.find(native->target.version.key);
    if (activity == shard->activities.end() || activity->second.nativeWrite != native
        || activity->second.publicationClaim) return false;
    if (native->proof.isValid()) { --shard->arenas.cpuProofs; native->proof = {}; }
    activity->second.nativeWrite = nullptr;
    if (activity->second.isEmpty()) shard->activities.erase(activity);
    native->reserved = false;
    ++shard->pages.find(native->target.version.key)->second.revision;
    return true;
}

bool KisPageMetadataCoordinator::sealCpuProof(const KisPreparedPageProof &proof, quint64 issuer,
    KisPreparedPageProof *destination)
{
    auto *d = m_core.load(std::memory_order_acquire);
    auto *shard = Private::shardFor(d, proof.authority.version.key);
    if (!shard || !issuer || d->cpuProofIssuer.load() != issuer || !proof.isValid()) return false;
    QMutexLocker lock(&shard->mutex);
    const auto *activity = shard->activity(proof.authority.version.key);
    auto *native = activity ? activity->nativeWrite : nullptr;
    if (!native || !native->completed || native->proof.isValid()
        || !(activity->writerTransaction == proof.transaction) || !(native->target == proof.authority)
        || !(d->cpuCompletion == proof.producerCompletion)) return false;
    native->proof = proof;
    if (destination && destination != &native->proof) *destination = proof;
    ++shard->arenas.cpuProofs;
    return true;
}

bool KisPageMetadataCoordinator::ownsCpuProof(const KisPreparedPageProof &proof, quint64 issuer) const
{
    auto *d = m_core.load(std::memory_order_acquire);
    const auto *shard = Private::shardFor(d, proof.authority.version.key);
    if (!shard || !issuer || d->cpuProofIssuer.load() != issuer || !proof.isValid()) return false;
    QMutexLocker lock(&shard->mutex);
    KisVersionSlotId slot;
    if (!shard->records.findVersion(proof.authority.version, &slot)) {
        const auto *activity = shard->activity(proof.authority.version.key);
        const auto *native = activity ? activity->nativeWrite : nullptr;
        return native && native->completed && native->proof == proof;
    }
    const auto *record = shard->records.version(slot);
    const auto *authority = shard->records.replica(record->authorityReplica);
    const auto *facts = shard->arenas.facts.get(record->facts);
    return facts->stamp == proof.providerValidationStamp && facts->transaction == proof.transaction
        && d->cpuCompletion == proof.producerCompletion && authority
        && replicaHandle(*authority, record->version) == proof.authority;
}

bool KisPageMetadataCoordinator::revokeCpuProof(const KisPreparedPageProof &proof, quint64 issuer)
{
    auto *d = m_core.load(std::memory_order_acquire);
    auto *shard = Private::shardFor(d, proof.authority.version.key);
    if (!shard || !issuer || d->cpuProofIssuer.load() != issuer || !proof.isValid()) return false;
    QMutexLocker lock(&shard->mutex);
    KisVersionSlotId slot;
    if (!shard->records.findVersion(proof.authority.version, &slot)) {
        const auto *activity = shard->activity(proof.authority.version.key);
        auto *native = activity ? activity->nativeWrite : nullptr;
        if (!native || !(native->proof == proof)) return false;
        native->proof = {};
        --shard->arenas.cpuProofs;
        return true;
    }
    const auto *record = shard->records.version(slot);
    const auto *authority = shard->records.replica(record->authorityReplica);
    auto *facts = shard->arenas.facts.get(record->facts);
    if (facts->stamp != proof.providerValidationStamp || !(facts->transaction == proof.transaction)
        || !(d->cpuCompletion == proof.producerCompletion) || !authority
        || !(replicaHandle(*authority, record->version) == proof.authority)) return false;
    facts->stamp = 0;
    --shard->arenas.cpuProofs;
    facts->transaction = {};
    return true;
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
    if (const auto *activity = shard->activity(target.key); activity && activity->nativeWrite) {
        if (activity->nativeWrite->target.version == target) return true;
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

KisPageMetadataCoordinator::HistorySlice
KisPageMetadataCoordinator::historySlice(const KisPageKey &key, const KisPageVersion &after, qsizetype budget) const
{
    HistorySlice result;
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), key);
    if (!shard || budget <= 0 || (after.isValid() && !(after.key == key)))
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
    while (nextHint.isValid() && result.count < qMin(budget, HistorySlice::Limit)) {
        const KisVersionRecord *record = shard->records.version(nextHint);
        Q_ASSERT(record);
        if (!record)
            break;
        result.versions[size_t(result.count++)] = record->version;
        result.after = record->version;
        nextHint = record->nextHistory;
    }
    if (!nextHint.isValid()) result.after = {};
    ++shard->historySliceQueries;
    shard->historySliceVersionInputs += quint64(result.count);
    shard->maximumHistorySliceVersionInputs =
        qMax(shard->maximumHistorySliceVersionInputs, quint64(result.count));
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
        KisPageWorkingArray<KisPageTransitionEffect> prepared(storageOwner.borrowed());
        std::array<std::shared_ptr<const KisPageAllocationDescriptor>, HistorySlice::Limit> releasedDescriptors;
        std::array<std::shared_ptr<KisCpuResidentBinding>, HistorySlice::Limit> releasedBindings;
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
            replicaCount += record->replicaCount;
        }
        if (!selected) return true;
        prepared.reserve(replicaCount);
        for (qsizetype i = 0; i < count; ++i) {
            if (!(selected & (quint32(1) << i))) continue;
            KisVersionSlotId slot;
            shard->records.findVersion(versions[i], &slot);
            const auto *record = shard->records.version(slot);
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
                                  &releasedBindings[size_t(i)]);
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
            || (shard->activity(key) && shard->activity(key)->nativeWrite)) {
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
    void *context, void (*visit)(void *, const KisPageKey &)) const
{
    QMutexLocker configurationLock(&m_configurationMutex);
    const auto *d = m_core.load(std::memory_order_acquire);
    if (!d) return;
    for (const auto &shard : d->shards) {
        QMutexLocker shardLock(&shard->mutex);
        for (auto page = shard->pages.cbegin(); page != shard->pages.cend(); ++page)
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
        result.pageActivities += quint64(shard->activities.size());
        const quint64 pageBytes = quint64(shard->pages.size()) * sizeof(MetadataPage);
        const quint64 activityBytes = quint64(shard->activities.size()) * sizeof(MetadataPageActivity);
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

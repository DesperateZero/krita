/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageMetadataCoordinator.h"
#include "KisCpuResidentBinding_p.h"
#include "KisPageMetadataCoordinator_p.h"
#include "KisPageStoreReclamation_p.h"
#include "KisPageWriteCoordinator_p.h"
#include "KisPageStateMachine_p.h"

#include <QAtomicInteger>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>
#include <QSet>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <utility>
#include <unordered_map>
#include <unordered_set>
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
        : versions(VersionBudget, storage), replicas(ReplicaBudget, storage)
        , overflow(OverflowBudget, storage) {}

    VersionArena versions;
    ReplicaArena replicas;
    OverflowArena overflow;

    struct ReleasedBlocks {
        // Members are destroyed in reverse order: payloads first, accounting
        // second, so usage never understates memory that is still allocated.
        MetadataBudgetRelease budgetRelease;
        VersionArena::ReleasedBlocks versions;
        ReplicaArena::ReleasedBlocks replicas;
        OverflowArena::ReleasedBlocks overflow;

        ReleasedBlocks() = default;
        ReleasedBlocks(ReleasedBlocks &&) noexcept = default;
        ReleasedBlocks &operator=(ReleasedBlocks &&other) noexcept
        {
            if (this == &other)
                return *this;
            versions = {};
            replicas = {};
            overflow = {};
            budgetRelease.release();
            versions = std::move(other.versions);
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
        result.replicas = replicas.takeEmptyBlocks(minimumBlocksToKeep);
        result.overflow = overflow.takeEmptyBlocks(minimumBlocksToKeep);
        const quint64 bytes = result.versions.byteSize()
            + result.replicas.byteSize() + result.overflow.byteSize();
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
    ReplicaArena::Reservation replicas;
    OverflowArena::Reservation overflow;

    bool reserve(MetadataArenas *arenas, const MetadataArenaDemand &demand)
    {
        Q_ASSERT(arenas);
        if (demand.versions > 0) {
            versions = arenas->versions.reserveSlots(qsizetype(demand.versions));
            if (!versions.isValid())
                return false;
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

    bool isEmpty() const
    {
        return versionBlocks == 0 && replicaBlocks == 0 && overflowBlocks == 0;
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
                                               const MetadataArenaDemand &demand,
                                               bool fullDemand = false)
{
    const auto blocks = [fullDemand](const auto &arena, quint64 slots) {
        return fullDemand ? (slots + arena.slotsPerBlock() - 1) / arena.slotsPerBlock()
                          : arenaBlocksRequired(arena, slots);
    };
    MetadataArenaGrowthPlan plan{blocks(arenas.versions, demand.versions),
        blocks(arenas.replicas, demand.replicas), blocks(arenas.overflow, demand.overflow)};
    plan.versionDirectory = arenas.versions.directoryCapacityForBlocks(plan.versionBlocks);
    plan.replicaDirectory = arenas.replicas.directoryCapacityForBlocks(plan.replicaBlocks);
    plan.overflowDirectory = arenas.overflow.directoryCapacityForBlocks(plan.overflowBlocks);
    return plan;
}

bool metadataArenaGrowthFitsBudget(const MetadataArenas &arenas, const MetadataArenaGrowthPlan &plan)
{
    return arenas.versions.canAttachBlocks(plan.versionBlocks) && arenas.replicas.canAttachBlocks(plan.replicaBlocks)
        && arenas.overflow.canAttachBlocks(plan.overflowBlocks);
}

bool metadataArenasHaveCapacity(const MetadataArenas &arenas, const MetadataArenaDemand &demand)
{
    return arenas.versions.availableSlots() >= demand.versions && arenas.replicas.availableSlots() >= demand.replicas
        && arenas.overflow.availableSlots() >= demand.overflow;
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
    explicit MetadataArenaGrowth(KisMutationStorageAllocator<char> storage)
        : versions(storage), replicas(storage), overflow(storage)
        , versionDirectory(storage), replicaDirectory(storage), overflowDirectory(storage) {}
    // Candidate payloads are destroyed before the reservation that accounts
    // for them when preparation exits early.
    KisBackingBudgetReservation budgetReservation;
    std::vector<VersionArena::PreparedBlock, KisMutationStorageAllocator<VersionArena::PreparedBlock>> versions;
    std::vector<ReplicaArena::PreparedBlock, KisMutationStorageAllocator<ReplicaArena::PreparedBlock>> replicas;
    std::vector<OverflowArena::PreparedBlock, KisMutationStorageAllocator<OverflowArena::PreparedBlock>> overflow;
    VersionArena::PreparedDirectory versionDirectory;
    ReplicaArena::PreparedDirectory replicaDirectory;
    OverflowArena::PreparedDirectory overflowDirectory;
    quint64 attachedBlocks = 0;

    quint64 preparedBlockCount() const
    {
        return quint64(versions.size()) + quint64(replicas.size()) + quint64(overflow.size());
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
            + plan.overflowBlocks * OverflowArena::blockByteSize();
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
            replicaDirectory.prepare(plan.replicaDirectory);
            overflowDirectory.prepare(plan.overflowDirectory);
        } catch (const std::bad_alloc &) {
            return false;
        }
        return prepareBlocks<VersionArena>(&versions, plan.versionBlocks)
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
        const quint64 replicaBytesBefore =
            arenas->replicas.statistics().allocatedBytes;
        const quint64 overflowBytesBefore =
            arenas->overflow.statistics().allocatedBytes;
        const auto versionResult = attachBlocks(&arenas->versions, demand.versions, &versions,
                                               &versionDirectory, &attachedBlocks);
        auto result = versionResult;
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
        replicas.clear();
        overflow.clear();
        if (budgetReservation.isValid()) {
            KisBackingBudgetDelta installed;
            const quint64 attachedBytes =
                arenas->versions.statistics().allocatedBytes - versionBytesBefore
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
    quint16 preparedPublications = 0;
    KisPageWriteMode writerMode = KisPageWriteMode::PreserveContents;
    KisPageWriterPhase writerPhase = KisPageWriterPhase::None;

    bool hasWriter() const
    {
        return writerPhase != KisPageWriterPhase::None;
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

enum class MetadataOwnedHash : quint8 {
    ExactVersions,
    PhysicalSlots,
    Pages,
    Activities,
    CpuBindings,
    Count
};

/**
 * Byte admission for shard-owned flat indexes and QHash storage. Qt deliberately
 * hides node/bucket allocation details, so the contract charges a stable
 * upper bound per power-of-two capacity slot before reserve()/insert(). The
 * charge follows retained capacity and is released only with the shard.
 */
class MetadataOwnedCapacity
{
public:
    MetadataOwnedCapacity(std::shared_ptr<MetadataBudgetAuthority> authority,
                          MetadataBudgetRelease *charge)
        : m_authority(std::move(authority)), m_charge(charge) {}

    template<class Allocate>
    bool ensure(MetadataOwnedHash kind, qsizetype required, Allocate allocate,
                QString *error = nullptr)
    {
        if (required <= 0)
            return true;
        const size_t index = size_t(kind);
        if (index >= m_capacities.size())
            return false;
        if (quint64(required) <= m_capacities[index])
            return true;
        const quint64 planned = plannedCapacity(quint64(required));
        if (!planned)
            return false;
        if (planned <= m_capacities[index])
            return true;
        const quint64 unit = bytesPerCapacitySlot(kind);
        const quint64 growth = planned - m_capacities[index];
        if (!unit || growth > quint64(std::numeric_limits<qint64>::max()) / unit) {
            KisPageStoreDetail::setError(error, QStringLiteral("metadata owning capacity overflows"));
            return false;
        }
        const quint64 bytes = growth * unit;
        KisBackingBudgetDelta delta;
        delta.buckets[size_t(KisBackingBudgetClass::MetadataArena)].cpuRam =
            qint64(bytes);
        auto reservation = m_authority->reserve(delta, error);
        if (!reservation.isValid())
            return false;
        if (!allocate(qsizetype(required), planned)) {
            KisPageStoreDetail::setError(error, QStringLiteral("metadata owning capacity allocation failed"));
            return false;
        }
        KisBackingBudgetDelta installed;
        installed.buckets[size_t(KisBackingBudgetClass::MetadataArena)].cpuRam =
            qint64(bytes);
        m_authority->commitReservation(std::move(reservation), installed);
        m_charge->add(bytes);
        m_capacities[index] = planned;
        KisPageStoreDetail::setError(error, {});
        return true;
    }

    quint64 chargedBytes() const
    {
        quint64 result = 0;
        for (size_t i = 0; i < m_capacities.size(); ++i)
            result += m_capacities[i] * bytesPerCapacitySlot(MetadataOwnedHash(i));
        return result;
    }

private:
    static quint64 plannedCapacity(quint64 required)
    {
        quint64 result = 64;
        // QHash keeps one bucket tier of headroom when reserve() lands
        // exactly on a tier boundary (for example, 512 entries use 1024
        // buckets). Its minimum allocated tier is 64.
        while (result <= required) {
            if (result > quint64(std::numeric_limits<qsizetype>::max()) / 2)
                return 0;
            result *= 2;
        }
        return result;
    }

    template<class Key, class Value>
    static constexpr quint64 hashSlotBytes()
    {
        // One key/value node, bucket/link storage, hash/alignment and a full
        // extra slot of slack. This is deliberately an upper-bound contract,
        // not a claim about one Qt build's private node layout.
        return 2 * (quint64(sizeof(Key)) + quint64(sizeof(Value))
                    + 3 * quint64(sizeof(void *)) + quint64(sizeof(size_t)));
    }

    static constexpr quint64 bytesPerCapacitySlot(MetadataOwnedHash kind)
    {
        switch (kind) {
        case MetadataOwnedHash::ExactVersions:
            return KisShardSlotIndex<KisPageVersion, KisVersionSlotId>::bytesPerCapacitySlot();
        case MetadataOwnedHash::PhysicalSlots:
            return KisShardSlotIndex<PhysicalSlot, KisReplicaSlotId>::bytesPerCapacitySlot();
        case MetadataOwnedHash::Pages:
            return hashSlotBytes<KisPageKey, MetadataPage>();
        case MetadataOwnedHash::Activities:
            return hashSlotBytes<KisPageKey, MetadataPageActivity>();
        case MetadataOwnedHash::CpuBindings:
            return hashSlotBytes<KisPageVersion, QSharedPointer<KisCpuReadBindingLink>>();
        case MetadataOwnedHash::Count:
            break;
        }
        return 0;
    }

    std::shared_ptr<MetadataBudgetAuthority> m_authority;
    MetadataBudgetRelease *m_charge = nullptr;
    std::array<quint64, size_t(MetadataOwnedHash::Count)> m_capacities{};
};

/**
 * One record/index owner per shard. Pages contain only intrusive slot heads,
 * tails and counts; no page owns a list, map, set, hash or shared heap object.
 */
struct ShardRecordStore {
    using ExactIndex = KisShardSlotIndex<KisPageVersion, KisVersionSlotId>;
    using PhysicalIndex = KisShardSlotIndex<PhysicalSlot, KisReplicaSlotId>;
    using HistoryPosition = std::pair<quint64, quint64>;

    explicit ShardRecordStore(MetadataArenas *storage,
                              MetadataOwnedCapacity *capacity)
        : arenas(storage), ownedCapacity(capacity)
    {
        Q_ASSERT(arenas && ownedCapacity);
    }

    MetadataArenas *arenas = nullptr;
    MetadataOwnedCapacity *ownedCapacity = nullptr;
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
            if (!required || !ownedCapacity->ensure(
                    MetadataOwnedHash::ExactVersions, required,
                    [&](qsizetype value, quint64 planned) {
                        return exactVersions.prepareCapacity(value)
                            && quint64(exactVersions.statistics().capacity) <= planned;
                    })) {
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
            if (!required || !ownedCapacity->ensure(
                    MetadataOwnedHash::PhysicalSlots, required,
                    [&](qsizetype value, quint64 planned) {
                        return physicalSlots.prepareCapacity(value)
                            && quint64(physicalSlots.statistics().capacity) <= planned;
                    })) {
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
        const KisVersionSlotId previousVersion = record->previousVersion;
        const KisVersionSlotId nextVersion = record->nextVersion;
        const KisVersionSlotId previousHistory = record->previousHistory;
        const KisVersionSlotId nextHistory = record->nextHistory;
        const KisVersionSlotId nextMutable = record->nextMutable;
        eraseChildren(record);
        *record = {};
        record->version = snapshot.version;
        record->slot = slot;
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

    template<class Version>
    Version project(const KisVersionRecord &record, Version result) const
    {
        result.version = record.version;
        result.publication = record.publication;
        result.preparedBy = record.preparedBy;
        for (KisMetadataOverflowSlotId slot = record.capturedReadViews; slot.isValid();) {
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
            for (KisMetadataOverflowSlotId overflowSlot = stored->overflow; overflowSlot.isValid();) {
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

    template<class Version>
    bool snapshot(const KisPageVersion &identity, Version *snapshot) const
    {
        KisVersionSlotId slot;
        if (!snapshot || !findVersion(identity, &slot))
            return false;
        const KisVersionRecord *record = version(slot);
        if (!record)
            return false;
        *snapshot = project(*record, kisPageVersionValue(*snapshot));
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
        Info result;
        result.publishedGeneration = publishedGeneration;
        KisVersionSlotId slot;
        if (!findVersion(identity, &slot)) return result;
        const auto *stored = version(slot);
        Q_ASSERT(stored);
        result.version = stored->version;
        result.publication = stored->publication;
        result.preparedBy = stored->preparedBy;
        result.authority = projectReplica(stored->authorityReplica);
        result.replicaCount = stored->replicaCount;
        result.captured = stored->capturedReadViews.isValid();
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
        if (activity->hasWriter()) {
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
        next.writerTransaction = {};
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

    template<class Snapshots>
    bool putBatch(MetadataPage *page, const Snapshots &snapshots, KisPageWorkingStorage storage)
    {
        qsizetype newVersions = 0;
        qsizetype replicaCount = 0;
        struct PhysicalHash {
            size_t operator()(const PhysicalSlot &value) const noexcept { return qHash(value); }
        };
        std::unordered_set<PhysicalSlot, PhysicalHash, std::equal_to<PhysicalSlot>,
            KisMutationStorageAllocator<PhysicalSlot>> batchPhysicalSlots(0, PhysicalHash{}, {}, storage);
        batchPhysicalSlots.reserve(snapshots.size());
        for (auto snapshotIt = snapshots.cbegin(); snapshotIt != snapshots.cend(); ++snapshotIt) {
            KisVersionSlotId existing;
            if (!findVersion(snapshotIt->version, &existing))
                ++newVersions;
            for (const auto &replica : snapshotIt->replicas) {
                if (!batchPhysicalSlots.insert(replica.replica.physicalSlotIdentity()).second)
                    return false;
            }
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

    void remove(MetadataPage *page, const KisPageVersion &identity)
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
        const bool recordErased = arenas->versions.erase(slot);
        Q_ASSERT(indexErased && recordErased);
        Q_UNUSED(indexErased);
        Q_UNUSED(recordErased);
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
        , ownedCapacity(budgetAuthority, &budgetCharge)
        , arenas(storage)
        , records(&arenas, &ownedCapacity) {}

    mutable QMutex mutex;
    std::shared_ptr<MetadataBudgetAuthority> budgetAuthority;
    // Declared before arenas so arena payloads are destroyed before their
    // remaining live charge is released.
    MetadataBudgetRelease budgetCharge;
    MetadataOwnedCapacity ownedCapacity;
    MetadataArenas arenas;
    ShardRecordStore records;
    QHash<KisPageKey, MetadataPage> pages;
    QHash<KisPageKey, MetadataPageActivity> activities;
    QHash<KisPageVersion, QSharedPointer<KisCpuReadBindingLink>> cpuBindings;
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
        const auto found = activities.constFind(key);
        return found == activities.cend() ? nullptr : &found.value();
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
            activities.remove(key);
        } else {
            activities.insert(key, next);
        }
    }

    void setPublicationClaim(const KisPageKey &key, const void *claim)
    {
        auto current = activities.find(key);
        Q_ASSERT(current != activities.end());
        if (current == activities.end())
            return;
        current->publicationClaim = claim;
        if (current->isEmpty())
            activities.erase(current);
    }

    bool ensureActivityCapacity(const KisPageKey &key,
                                const KisPageStateSnapshot *state = nullptr,
                                QString *error = nullptr)
    {
        const bool needsEntry = activities.contains(key)
            || (state && (state->writer.phase != KisPageWriterPhase::None
                          || state->authorityHandoff.operation.isValid()));
        if (!needsEntry)
            return true;
        const qsizetype required = activities.contains(key)
            ? activities.size() : activities.size() + 1;
        return ownedCapacity.ensure(MetadataOwnedHash::Activities, required,
            [&](qsizetype value, quint64 planned) {
                try {
                    activities.reserve(value);
                } catch (const std::bad_alloc &) {
                    return false;
                }
                return quint64(activities.capacity()) <= planned;
            }, error);
    }

    bool reservePublicationActivity(const KisPageKey &key, QString *error)
    {
        if (!activities.contains(key)
            && !ownedCapacity.ensure(MetadataOwnedHash::Activities,
                                     activities.size() + 1,
                                     [&](qsizetype value, quint64 planned) {
                                         try {
                                             activities.reserve(value);
                                         } catch (const std::bad_alloc &) {
                                             return false;
                                         }
                                         return quint64(activities.capacity()) <= planned;
                                     }, error)) {
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
        Q_ASSERT(current != activities.end() && current->preparedPublications > 0);
        if (current == activities.end() || current->preparedPublications == 0)
            return;
        --current->preparedPublications;
        if (current->isEmpty())
            activities.erase(current);
    }

    void consumePublicationActivity(const KisPageKey &key, const void *claim)
    {
        auto current = activities.find(key);
        Q_ASSERT(current != activities.end() && current->preparedPublications > 0 && !current->publicationClaim
                 && claim);
        if (current == activities.end() || current->preparedPublications == 0 || current->publicationClaim || !claim) {
            return;
        }
        --current->preparedPublications;
        current->publicationClaim = claim;
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
            next = current.value();
        if (!records.assignHeader(page, &next, state))
            return false;
        if (current != activities.end()) {
            if (next.isEmpty())
                activities.erase(current);
            else
                current.value() = next;
        } else if (!next.isEmpty()) {
            activities.insert(key, next);
        }
        return true;
    }

    bool installReservedHeader(const KisPageKey &key, MetadataPage *page, const KisPageStateSnapshot &state)
    {
        auto current = activities.find(key);
        Q_ASSERT(current != activities.end());
        if (current == activities.end())
            return false;
        MetadataPageActivity next = current.value();
        if (!records.assignHeader(page, &next, state))
            return false;
        if (next.isEmpty())
            activities.erase(current);
        else
            current.value() = next;
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
    if (!shard.pages.contains(identity.key) || !shard.records.findVersion(identity, &slot)) return nullptr;
    const auto *version = shard.records.version(slot);
    return version && (version->publication == KisPagePublicationState::Published
        || version->publication == KisPagePublicationState::Historical
        || (version->publication == KisPagePublicationState::Prepared && version->capturedReadViews.isValid()))
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
    Private(std::unique_ptr<KisBackingBudgetController> standalone, const Storage &allocator)
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

    std::unique_ptr<KisBackingBudgetController> standaloneBudget;
    Storage storage;
    std::shared_ptr<MetadataBudgetAuthority> budgetAuthority;
    using Shards = std::vector<std::shared_ptr<MetadataShard>,
        KisMutationStorageAllocator<std::shared_ptr<MetadataShard>>>;
    Shards shards;
    QAtomicInteger<quint64> registeredPages{0};
    // Shared identity prevents a capability surviving destruction from being
    // accepted by another coordinator constructed at the same address.
    std::shared_ptr<const quint8> publicationOwner;
};

class KisPageMetadataCoordinator::PreparedPublication::Data
{
public:
    using Storage = KisMutationStorageAllocator<char>;
    template<class T> using Array = std::vector<T, KisMutationStorageAllocator<T>>;

    // The same paid values serve the transition and installation, without a
    // second Qt-to-install copy or independent representation of protection.
    using Version = KisPageWorkingVersion;
    struct VersionHash {
        size_t operator()(const KisPageVersion &version) const noexcept { return qHash(version); }
    };

    explicit Data(Storage storage)
        : entries(storage), publications(storage), arenaGrowth(storage)
        , backingAuthorities(0, VersionHash{}, std::equal_to<KisPageVersion>{}, storage) {}
    ~Data();

    struct Entry {
        std::shared_ptr<MetadataShard> shardOwner;
        quint64 revision = 0;
        KisPageVersion version;
        bool activityReservation = false;
        KisPageKey key() const { return version.key; }
    };
    struct Publication {
        explicit Publication(Storage storage) : deltas(storage), installRecords(storage) {}
        KisPageStateSnapshot next;
        struct PublicationDelta {
            KisPageVersion version;
            KisPagePublicationState publication;
            KisPageTransactionId preparedBy;
        };
        Array<PublicationDelta> deltas;
        // Candidate values are contiguous; authoritative ordering is formed
        // by intrusive arena links only after full-batch revalidation.
        Array<Version> installRecords;
        MetadataArenaDemand arenaDemand;
        size_t arenaGrowthIndex = std::numeric_limits<size_t>::max();
    };
    struct ShardArenaGrowth {
        explicit ShardArenaGrowth(Storage storage) : blocks(storage) {}
        std::shared_ptr<MetadataShard> shardOwner;
        MetadataArenaDemand totalDemand;
        MetadataArenaGrowth blocks;
        ShardRecordStore::ReservationSet reservations;
        qsizetype exactInsertions = 0;
        qsizetype physicalInsertions = 0;
        MetadataArenas::ReleasedBlocks releasedBlocks;
    };
    void cancel() noexcept;

    std::shared_ptr<const quint8> owner;
    KisPageTransaction transaction;
    KisImageEpochId minimumEpoch;
    PreparationKind kind = PreparationKind::Publication;
    Array<Entry> entries;
    // Publication images belong only to publication/recoverable candidates.
    // Detachment retains the same claims and terminal policy in entries.
    Array<Publication> publications;
    Array<ShardArenaGrowth> arenaGrowth;
    std::unordered_map<KisPageVersion, KisReplicaHandle, VersionHash, std::equal_to<KisPageVersion>,
        KisMutationStorageAllocator<std::pair<const KisPageVersion, KisReplicaHandle>>> backingAuthorities;
};

void KisPageMetadataCoordinator::PreparedPublication::DataDeleter::operator()(Data *value) const noexcept
{
    if (!value) return;
    auto allocator = storage;
    std::allocator_traits<decltype(allocator)>::destroy(allocator, value);
    allocator.deallocate(value, 1);
}

KisPageMetadataCoordinator::PreparedPublication::Data::~Data()
{
    cancel();
}

void KisPageMetadataCoordinator::PreparedPublication::Data::cancel() noexcept
{
    if (!owner) {
        return;
    }
    for (Entry &entry : entries) {
        if (!entry.activityReservation || !entry.shardOwner)
            continue;
        QMutexLocker locker(&entry.shardOwner->mutex);
        entry.shardOwner->releasePublicationActivity(entry.key());
        entry.activityReservation = false;
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
    const auto found = data->backingAuthorities.find(version);
    return found == data->backingAuthorities.end() ? KisReplicaHandle{} : found->second;
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
    if (data->entries.empty()) {
        data = PreparedPublication::DataPointer{};
        return 1;
    }
    const qsizetype count = std::min(maximumWorkUnits, qsizetype(data->entries.size()));
    for (qsizetype i = 0; i < count; ++i) {
        // Release this page's optional publication payload before its shard
        // owner, preserving the same bounded cleanup unit as preparation.
        auto entry = std::move(data->entries.back());
        data->entries.pop_back();
        if (!data->publications.empty()) data->publications.pop_back();
        Q_UNUSED(entry);
    }
    if (data->entries.empty())
        data = PreparedPublication::DataPointer{};
    return count;
}
KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::preparePublication(const KisPageTransaction &transaction,
                                               KisImageEpochId minimumEpoch,
                                               const QVector<KisPageTransition> &transitions,
                                               QString *error) const
{
    return preparePublicationImpl(transaction, minimumEpoch, transitions.constData(), transitions.size(), false, error);
}

KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::prepareRestoration(KisImageEpochId minimumEpoch,
                                               const QVector<KisPageTransition> &transitions,
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
                                                   QString *error) const
{
    return preparePublicationImpl(transaction, {}, &transition, 1, false, error, PreparationKind::RecoverableWrite);
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
KisPageMetadataCoordinator::preparePublicationImpl(const KisPageTransaction &transaction,
                                                   KisImageEpochId minimumEpoch,
                                                   const KisPageTransition *transitions, qsizetype count,
                                                   bool restoration,
                                                   QString *error,
                                                   PreparationKind kind,
                                                   const KisPageVersion *versions,
                                                   const PublicationChange *changes) const
try {
    const bool mutation = kind != PreparationKind::Publication;
    const bool detachment = kind == PreparationKind::Detachment;
    const bool recoverable = kind == PreparationKind::RecoverableWrite;
    PreparedPublication result;
    auto *d = m_core.load(std::memory_order_acquire);
    if (!d || count < 0 || (count && !transitions && !versions && !changes)
        || (!restoration && !transaction.isValid())
        || (!mutation && (!minimumEpoch.isValid() || minimumEpoch.value <= transaction.baseEpoch.value))) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata publication identity is invalid"));
        return result;
    }
    auto storage = d->budgetAuthority->storage<PreparedPublication::Data>();
    PreparedPublication::DataPointer data(nullptr, PreparedPublication::DataDeleter{storage});
    auto *raw = storage.allocate(1);
    try { std::allocator_traits<decltype(storage)>::construct(storage, raw, storage); }
    catch (...) { storage.deallocate(raw, 1); throw; }
    data.reset(raw);
    data->owner = d->publicationOwner;
    data->transaction = transaction;
    data->minimumEpoch = minimumEpoch;
    data->kind = kind;
    data->entries.reserve(size_t(count));
    if (!detachment) data->publications.reserve(size_t(count));
    const KisPageStateMachine stateMachine;
    for (qsizetype i = 0; i < count; ++i) {
        KisPageTransition input;
        if (versions) {
            input.kind = detachment ? KisPageTransitionKind::DetachPreparedVersion
                                    : KisPageTransitionKind::RestoreCommittedVersion;
            input.version = versions[i];
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
        const auto &transition = versions || changes ? input : transitions[i];
        const bool write = transition.kind == KisPageTransitionKind::CommitTransaction;
        if ((detachment && !transition.version.isValid())
            || (recoverable && (transition.kind != KisPageTransitionKind::AcquireRecoverableWrite
                || !(transition.transaction == transaction.id) || transition.imageEpoch.isValid()))
            || (!mutation
                && ((restoration && transition.kind != KisPageTransitionKind::RestoreCommittedVersion)
                    || (!write && transition.kind != KisPageTransitionKind::ReplaceDefaultPixel
                        && transition.kind != KisPageTransitionKind::RestoreCommittedVersion)
                    || (write && !(transition.transaction == transaction.id))
                    || (!write && transition.transaction.isValid()) || !(transition.imageEpoch == minimumEpoch)))
            || std::any_of(data->entries.cbegin(), data->entries.cend(), [&](const auto &entry) {
                return entry.key() == transition.version.key;
            })) {
            KisPageStoreDetail::setError(error, QStringLiteral("metadata publication transition is invalid or repeated"));
            return result;
        }
        std::shared_ptr<MetadataShard> shardOwner = Private::sharedShardFor(d, transition.version.key);
        MetadataShard *shard = shardOwner.get();
        if (!shard)
            return result;
        PreparedPublication::Data::Entry entry;
        entry.shardOwner = std::move(shardOwner);
        entry.version = transition.version;
        KisPageWorkingState before(storage);
        {
            QMutexLocker locker(&shard->mutex);
            const auto page = shard->pages.constFind(transition.version.key);
            if (page == shard->pages.constEnd() || !shard->canMutate(transition.version.key, page.value())) {
                KisPageStoreDetail::setError(error, QStringLiteral("metadata publication page is unavailable"));
                return result;
            }
            if (detachment) {
                // Match DetachPreparedVersion's semantic guards without
                // cloning all versions/protection lists into a stale image.
                if (shard->hasWriter(transition.version.key)
                    || !shard->records.isPreparedBy(transition.version, transaction.id)) {
                    KisPageStoreDetail::setError(error, QStringLiteral("prepared detachment identity is invalid or still writable"));
                    return result;
                }
            } else {
                entry.revision = page->revision;
                before.setHeader(shard->header(transition.version.key, page.value()));
                const auto include = [&](const KisPageVersion &identity) {
                    if (before.findVersion(identity))
                        return;
                    KisPageWorkingVersion v(storage);
                    if (shard->records.snapshot(identity, &v)) {
                        before.versions.push_back(std::move(v));
                    }
                };
                include({before.key, before.publishedGeneration, before.publishedDefaultPixelRevision});
                if (recoverable)
                    include(transition.baseVersion);
                include(transition.version);
                if (write) {
                    // Only this transaction's Prepared versions are relevant.
                    shard->records.forPreparedTransaction(page.value(), transaction.id, include);
                } else {
                    if (page->firstMutable.isValid()) {
                        const KisVersionRecord *mutableRecord = shard->records.version(page->firstMutable);
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
                shard->publicationVersionInputs += quint64(before.versions.size());
            }
        }
        if (detachment) {
            QMutexLocker locker(&entry.shardOwner->mutex);
            ++entry.shardOwner->publicationHistoryNodesPrepared;
            data->entries.push_back(std::move(entry));
            continue;
        }
        PreparedPublication::Data::Publication publication(storage);
        quint64 entryHistoryNodesPrepared = 0;
        quint64 entryAdditionRecordsPrepared = 0;
        const std::shared_ptr<MetadataShard> preparationShard = entry.shardOwner;
        const auto recordEntryPreparationWork = qScopeGuard([&] {
            QMutexLocker locker(&preparationShard->mutex);
            preparationShard->publicationHistoryNodesPrepared +=
                entryHistoryNodesPrepared;
            preparationShard->publicationAdditionRecordsPrepared +=
                entryAdditionRecordsPrepared;
        });
        // The expensive deterministic work is outside the shard lock and is
        // reused at installation. Revision revalidation includes leases,
        // Only coordinator-owned state reaches this private capability. Like
        // immediate apply(), reuse its established boundary invariants; all
        // transition-local lifetime/identity guards still run. External state
        // enters through registerPage()'s full validation. The reference test
        // compares every supported publication kind with full apply().
        // Indexed inputs deliberately omit unrelated versions.
        auto step = stateMachine.applyKnownValid(before, transition);
        if (!step.accepted) {
            KisPageStoreDetail::setError(error, step.rejectionReason);
            return result;
        }
        if (recoverable) {
            auto ready = transition;
            ready.kind = KisPageTransitionKind::PrepareWrite;
            step = stateMachine.applyKnownValid(std::move(step.next), ready);
            if (!step.accepted) {
                KisPageStoreDetail::setError(error, step.rejectionReason);
                return result;
            }
        }
        for (const auto &version : std::as_const(before.versions)) {
            if (version.authority.isValid())
                data->backingAuthorities.insert_or_assign(version.version, version.authority);
        }
        for (const auto &version : std::as_const(step.next.versions)) {
            if (version.authority.isValid())
                data->backingAuthorities.insert_or_assign(version.version, version.authority);
        }
        if (recoverable) {
            // Strict revision revalidation covers every replica/reader fact
            // used here. Install only this exact base and the new target, in
            // that order, so the old physical slot is unbound before target
            // insertion. Unrelated history is never replaced. This branch is
            // inaccessible through ordinary immediate owner transitions.
            publication.installRecords.push_back(std::move(*step.next.findVersion(transition.baseVersion)));
            publication.installRecords.push_back(std::move(*step.next.findVersion(transition.version)));
        } else {
            for (auto &version : step.next.versions) {
                const auto *previous = before.findVersion(version.version);
                if (!previous) {
                    if (version.publication == KisPagePublicationState::Prepared
                        || version.publication == KisPagePublicationState::Unpublished) {
                        KisPageStoreDetail::setError(error, QStringLiteral("publication candidate introduced mutable metadata"));
                        return result;
                    }
                    if (version.publication == KisPagePublicationState::Historical)
                        ++entryHistoryNodesPrepared;
                    publication.installRecords.push_back(std::move(version));
                } else if (previous->publication != version.publication
                           || !(previous->preparedBy == version.preparedBy)) {
                    publication.deltas.push_back({version.version, version.publication, version.preparedBy});
                    if (version.publication == KisPagePublicationState::Historical)
                        ++entryHistoryNodesPrepared;
                }
            }
        }
        // Ordinary publication only changes publication fields. Recoverable
        // writes replace the strictly revision-bound base record and add T;
        // any intervening reader/pin/last-use change rejects that candidate.
        entryAdditionRecordsPrepared += quint64(publication.installRecords.size()) - (recoverable ? 1 : 0);
        // Headers never own version records at install.
        publication.next = step.next.header();
        {
            QMutexLocker locker(&entry.shardOwner->mutex);
            const auto page = entry.shardOwner->pages.find(entry.key());
            if (page == entry.shardOwner->pages.end() || !entry.shardOwner->canMutate(entry.key(), page.value())
                || page->revision != entry.revision) {
                KisPageStoreDetail::setError(error, QStringLiteral("metadata changed while preparing publication storage"));
                result.m_conflicted = true;
                return result;
            }
            publication.arenaDemand = entry.shardOwner->records.batchDemand(publication.installRecords);
        }
        auto shardGrowth = std::find_if(data->arenaGrowth.begin(), data->arenaGrowth.end(), [&](const auto &candidate) {
            return candidate.shardOwner == entry.shardOwner;
        });
        if (shardGrowth == data->arenaGrowth.end()) {
            PreparedPublication::Data::ShardArenaGrowth nextGrowth(storage);
            nextGrowth.shardOwner = entry.shardOwner;
            data->arenaGrowth.push_back(std::move(nextGrowth));
            shardGrowth = std::prev(data->arenaGrowth.end());
        }
        publication.arenaGrowthIndex = size_t(std::distance(data->arenaGrowth.begin(), shardGrowth));
        shardGrowth->totalDemand.versions += publication.arenaDemand.versions;
        shardGrowth->totalDemand.replicas += publication.arenaDemand.replicas;
        shardGrowth->totalDemand.overflow += publication.arenaDemand.overflow;
        shardGrowth->exactInsertions += qsizetype(publication.installRecords.size()) - (recoverable ? 1 : 0);
        for (const auto &record : publication.installRecords)
            shardGrowth->physicalInsertions += record.replicas.size();
        // Publication preserves replicas as history; GC owns their eventual
        // retirement. Recoverable write transfers the backing to its target.
        Q_ASSERT(step.effects.empty());
        data->publications.push_back(std::move(publication));
        data->entries.push_back(std::move(entry));
    }
    // One candidate batch per shard avoids reserving a 16/32 KiB block for
    // every small page. It is still self-sufficient if all observed free slots
    // are consumed before install, and all payload allocation happens here.
    for (auto &growth : data->arenaGrowth) {
        MetadataArenaGrowthPlan plan;
        bool fitsBudget = false;
        {
            QMutexLocker locker(&growth.shardOwner->mutex);
            plan = metadataArenaGrowthPlan(growth.shardOwner->arenas, growth.totalDemand, true);
            fitsBudget = metadataArenaGrowthFitsBudget(growth.shardOwner->arenas, plan);
            if (!fitsBudget)
                ++growth.shardOwner->metadataArenaGrowthFailures;
        }
        if (!fitsBudget) {
            KisPageStoreDetail::setError(error, QStringLiteral("metadata arena budget is exhausted"));
            return result;
        }
        const bool prepared = growth.blocks.prepare(
            plan, growth.shardOwner->budgetAuthority, error);
        {
            QMutexLocker locker(&growth.shardOwner->mutex);
            ++growth.shardOwner->metadataArenaGrowthBatches;
            growth.shardOwner->metadataArenaBlockCandidatesPrepared += growth.blocks.preparedBlockCount();
            if (!prepared)
                ++growth.shardOwner->metadataArenaGrowthFailures;
        }
        if (!prepared) {
            KisPageStoreDetail::setError(error, QStringLiteral("metadata arena storage allocation failed"));
            return result;
        }
    }
    // Attach and reserve the complete shard-local install capacity during
    // preparation. Once these tokens exist, ordinary mutations can use only
    // unreserved arena slots/index capacity; install merely consumes tokens.
    for (auto &growth : data->arenaGrowth) {
        QMutexLocker locker(&growth.shardOwner->mutex);
        const auto revalidate = [&] {
            for (const auto &entry : data->entries) {
                if (entry.shardOwner != growth.shardOwner)
                    continue;
                const auto page = growth.shardOwner->pages.constFind(entry.key());
                if (page == growth.shardOwner->pages.constEnd() || !growth.shardOwner->canMutate(entry.key(), page.value())
                    || page->revision != entry.revision) {
                    return false;
                }
            }
            return true;
        };
        if (!revalidate()) {
            ++growth.shardOwner->metadataArenaGrowthConflicts;
            KisPageStoreDetail::setError(error, QStringLiteral("metadata changed while reserving publication capacity"));
            result.m_conflicted = true;
            return result;
        }

        const quint64 attachedBefore = growth.blocks.attachedBlocks;
        auto attachResult = growth.blocks.attach(
            &growth.shardOwner->arenas, &growth.shardOwner->budgetCharge,
            growth.totalDemand);
        growth.shardOwner->metadataArenaBlocksAttached += growth.blocks.attachedBlocks - attachedBefore;
        if (attachResult == MetadataArenaGrowth::AttachResult::NeedsMore) {
            // A different page can grow this directory during cold allocation.
            // Continue through the original locked revalidate/growth loop;
            // install still receives complete reserved capacity, never growth.
            const auto grown = growMetadataArenasOutsideLock(growth.shardOwner.get(), growth.totalDemand,
                &growth.blocks, &locker, revalidate, error);
            if (grown != MetadataGrowthResult::Ready) {
                result.m_conflicted = grown == MetadataGrowthResult::Stale;
                return result;
            }
            attachResult = MetadataArenaGrowth::AttachResult::Ready;
        }
        if (attachResult != MetadataArenaGrowth::AttachResult::Ready
            || !growth.shardOwner->records.reserve(growth.totalDemand,
                                              growth.exactInsertions,
                                              growth.physicalInsertions,
                                              &growth.reservations)) {
            ++growth.shardOwner->metadataArenaGrowthFailures;
            KisPageStoreDetail::setError(error, QStringLiteral("metadata publication capacity reservation failed"));
            return result;
        }
    }

    // Pre-create one sparse activity entry per page. Claim installation then
    // mutates an existing entry in place and cannot grow/rehash the activity
    // map while the commit gate is held.
    for (auto &entry : data->entries) {
        QMutexLocker locker(&entry.shardOwner->mutex);
        const auto page = entry.shardOwner->pages.constFind(entry.key());
        bool stillValid = page != entry.shardOwner->pages.constEnd() && entry.shardOwner->canMutate(entry.key(), page.value());
        if (stillValid && detachment) {
            stillValid = !entry.shardOwner->hasWriter(entry.key())
                && entry.shardOwner->records.isPreparedBy(entry.version, transaction.id);
        } else if (stillValid) {
            stillValid = page->revision == entry.revision;
        }
        if (!stillValid || !entry.shardOwner->reservePublicationActivity(entry.key(), error)) {
            KisPageStoreDetail::setError(error, QStringLiteral("metadata changed while reserving publication activity"));
            result.m_conflicted = !stillValid;
            return result;
        }
        entry.activityReservation = true;
    }
    result.data = std::move(data);
    for (const auto &entry : result.data->entries) {
        QMutexLocker locker(&entry.shardOwner->mutex);
        ++(mutation ? entry.shardOwner->preparedMutationPages
                    : entry.shardOwner->preparedPublicationPages);
    }
    KisPageStoreDetail::setError(error, {});
    return result;
}
catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("metadata publication preparation storage was refused"));
    return {};
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
    for (auto &entry : data->entries) {
        QMutexLocker locker(&entry.shardOwner->mutex);
        auto page = entry.shardOwner->pages.find(entry.key());
        if (page == entry.shardOwner->pages.end() || !entry.shardOwner->canMutate(entry.key(), page.value()))
            break;
        if (detachment) {
            if (entry.shardOwner->hasWriter(entry.key()))
                break;
            if (!entry.shardOwner->records.isPreparedBy(entry.version, transaction.id))
                break;
        } else if (page->revision != entry.revision)
            break;
        Q_ASSERT(entry.activityReservation);
        entry.shardOwner->consumePublicationActivity(entry.key(), data.get());
        entry.activityReservation = false;
        ++claimed;
    }
    if (claimed != qsizetype(data->entries.size())) {
        for (qsizetype i = 0; i < claimed; ++i) {
            const auto &entry = data->entries.at(i);
            QMutexLocker locker(&entry.shardOwner->mutex);
            entry.shardOwner->setPublicationClaim(entry.key(), nullptr);
        }
        KisPageStoreDetail::setError(error, QStringLiteral("metadata changed after publication preparation"));
        recordRejectedInstall();
        return false;
    }
    Q_ASSERT(detachment ? data->publications.empty() : data->publications.size() == data->entries.size());
    for (size_t i = 0; i < data->entries.size(); ++i) {
        auto &entry = data->entries[i];
        auto *publication = detachment ? nullptr : &data->publications[i];
        QMutexLocker locker(&entry.shardOwner->mutex);
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
        if (detachment) {
            // Claims on every page are held before the first edit. Preserve
            // the CURRENT replicas, leases, pins, last-use, captured views and
            // epoch; apply only the already revalidated semantic delta. This
            // is not permission to merge arbitrary stale publication images.
            const bool updated = entry.shardOwner->records.setPublication(&page.value(),
                                                                     entry.version,
                                                                     KisPagePublicationState::Historical,
                                                                     {});
            Q_ASSERT(updated);
            Q_UNUSED(updated);
            ++page->revision;
        } else if (recoverable) {
            auto &reservation = data->arenaGrowth.at(publication->arenaGrowthIndex).reservations;
            for (const auto &record : publication->installRecords) {
                const bool installed = entry.shardOwner->records.putReserved(&page.value(), record, &reservation);
                Q_ASSERT(installed);
                Q_UNUSED(installed);
            }
            const bool headerInstalled = entry.shardOwner->installReservedHeader(entry.key(), &page.value(), publication->next);
            Q_ASSERT(headerInstalled);
            Q_UNUSED(headerInstalled);
            ++page->revision;
            ++entry.shardOwner->publicationAdditionRecordsTransferred;
            entry.shardOwner->publicationVersionInstalls += 2;
        } else {
            publication->next.publishedEpoch = epoch;
            for (const auto &delta : std::as_const(publication->deltas)) {
                const bool updated = entry.shardOwner->records.setPublication(&page.value(),
                                                                         delta.version,
                                                                         delta.publication,
                                                                         delta.preparedBy);
                Q_ASSERT(updated);
                Q_UNUSED(updated);
            }
            const auto additionRecords = quint64(publication->installRecords.size());
            auto &reservation = data->arenaGrowth.at(publication->arenaGrowthIndex).reservations;
            const bool additionsInstalled = entry.shardOwner->records.installPreparedAdditionsReserved(&page.value(),
                                                                                                  &publication->installRecords,
                                                                                                  &reservation);
            Q_ASSERT(additionsInstalled);
            Q_UNUSED(additionsInstalled);
            entry.shardOwner->publicationAdditionRecordsTransferred += additionRecords;
            const bool headerInstalled = entry.shardOwner->installReservedHeader(entry.key(), &page.value(), publication->next);
            Q_ASSERT(headerInstalled);
            Q_UNUSED(headerInstalled);
            ++page->revision;
            entry.shardOwner->publicationVersionInstalls += quint64(publication->deltas.size()) + additionRecords;
        }
        entry.shardOwner->publicationHistoryNodesTransferred += installedHistoryLinks;
        ++(mutation ? entry.shardOwner->installedMutationPages
                    : entry.shardOwner->installedPublicationPages);
        // All page facts were claimed before the first install; after this
        // page's final write no later step can overwrite a new reader/last-use
        // mutation here. Release under the same lock, not a third lock pass.
        entry.shardOwner->setPublicationClaim(entry.key(), nullptr);
        locker.unlock();
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

QSharedPointer<KisCpuReadBindingLink>
KisPageMetadataCoordinator::installCpuReadBinding(const KisReplicaHandle &replica,
                                                  const QSharedPointer<KisPageReplicaProvider> &provider)
{
    if (!provider || !replica.isValid()
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
    const auto existing = shard->cpuBindings.constFind(replica.version);
    if (existing != shard->cpuBindings.constEnd())
        return existing.value();
    if (!shard->ownedCapacity.ensure(
            MetadataOwnedHash::CpuBindings, shard->cpuBindings.size() + 1,
            [&](qsizetype value, quint64 planned) {
                try {
                    shard->cpuBindings.reserve(value);
                } catch (const std::bad_alloc &) {
                    return false;
                }
                return quint64(shard->cpuBindings.capacity()) <= planned;
            })) {
        return {};
    }
    auto link = QSharedPointer<KisCpuReadBindingLink>::create(replica, provider);
    shard->cpuBindings.insert(replica.version, link);
    return link;
}

void KisPageMetadataCoordinator::removeCpuReadBinding(const KisReplicaHandle &replica,
                                                      const KisCpuReadBindingLink *expected)
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), replica.version.key);
    if (!shard)
        return;
    QMutexLocker locker(&shard->mutex);
    const auto existing = shard->cpuBindings.constFind(replica.version);
    if (existing != shard->cpuBindings.constEnd() && existing.value()->replica == replica
        && (!expected || existing.value().data() == expected))
        shard->cpuBindings.remove(replica.version);
}

QSharedPointer<KisCpuReadBindingLink> KisPageMetadataCoordinator::cpuReadBinding(const KisPageVersion &version) const
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), version.key);
    if (!shard)
        return {};
    QMutexLocker locker(&shard->mutex);
    return shard->cpuBindings.value(version);
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
        std::unique_ptr<KisBackingBudgetController> standalone;
        if (!budget) {
            standalone = std::make_unique<KisBackingBudgetController>();
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

bool KisPageMetadataCoordinator::registerPage(const KisPageStateSnapshot &initial, QString *error)
{
    KisPageStateMachine stateMachine;
    QString invariantFailure;
    if (!stateMachine.validateInvariants(initial, &invariantFailure)) {
        KisPageStoreDetail::setError(error, QStringLiteral("initial page state is invalid: %1").arg(invariantFailure));
        return false;
    }

    auto *d = m_core.load(std::memory_order_acquire);
    MetadataShard *shard = Private::shardFor(d, initial.key);
    if (!shard) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata coordinator is not configured"));
        return false;
    }
    const MetadataArenaDemand demand = newMetadataDemand(initial.versions);
    MetadataArenaGrowth growth(shard->budgetAuthority->storage<char>());
    QMutexLocker locker(&shard->mutex);
    if (shard->pages.contains(initial.key)) {
        KisPageStoreDetail::setError(error, QStringLiteral("page is already registered"));
        return false;
    }
    const auto storage = growMetadataArenasOutsideLock(shard, demand, &growth, &locker, [&] {
        return !shard->pages.contains(initial.key);
    }, error);
    if (storage != MetadataGrowthResult::Ready) {
        KisPageStoreDetail::setError(error,
                 storage == MetadataGrowthResult::Stale
                     ? QStringLiteral("page was registered while preparing metadata storage")
                     : QStringLiteral("metadata arena budget is exhausted"));
        return false;
    }
    if (!shard->ownedCapacity.ensure(
            MetadataOwnedHash::Pages, shard->pages.size() + 1,
            [&](qsizetype value, quint64 planned) {
                try {
                    shard->pages.reserve(value);
                } catch (const std::bad_alloc &) {
                    return false;
                }
                return quint64(shard->pages.capacity()) <= planned;
            }, error)
        || !shard->ensureActivityCapacity(initial.key, &initial, error)) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata owning capacity budget is exhausted"));
        return false;
    }
    MetadataPage page;
    if (!shard->assign(initial.key, &page, initial)) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata arena budget is exhausted"));
        return false;
    }
    shard->pages.insert(initial.key, std::move(page));
    d->registeredPages.fetchAndAddRelease(1);
    KisPageStoreDetail::setError(error, {});
    return true;
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
    const auto pageIt = shard->pages.constFind(key);
    if (pageIt == shard->pages.constEnd())
        return false;
    *snapshot = shard->snapshot(key, pageIt.value());
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
    const auto page = shard->pages.constFind(base.key);
    if (page == shard->pages.cend())
        return false;
    MutationBaseInfo next;
    next.baseExists = base.isValid() && shard->records.findVersion(base);
    next.hasWriter = shard->hasWriter(base.key);
    next.nextGeneration = page->nextGeneration;
    const auto *published = shard->records.version(page->publishedVersion);
    Q_ASSERT(published);
    const auto selected = sealed.isValid() ? sealed : base;
    next.selected = shard->records.versionInfo<VersionInfo>(selected, published->version.generation);
    if (discoverBefore && next.selected.version.isValid() && !selected.isDefaultPixel()
        && next.selected.replicaCount > 1) {
        KisVersionSlotId slot;
        const bool found = shard->records.findVersion(selected, &slot);
        Q_ASSERT(found); Q_UNUSED(found);
        const auto *version = shard->records.version(slot);
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
        const auto page = shard->pages.constFind(identity.key);
        if (page == shard->pages.cend()) return false;
        const auto *published = shard->records.version(page->publishedVersion);
        Q_ASSERT(published);
        auto next = shard->records.versionInfo<VersionInfo>(identity,
            published ? published->version.generation : KisPageGeneration{});
        KisVersionSlotId slot;
        if (shard->records.findVersion(identity, &slot)) {
            const auto *version = shard->records.version(slot);
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
        } else if (replicas) replicas->clear();
        *snapshot = next;
        return true; // Existing page and missing exact version remain distinct.
    }
}

bool KisPageMetadataCoordinator::canAddTransientVersion(const KisPageVersion &target, quint32 limit) const
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), target.key);
    if (!shard)
        return limit > 0;
    QMutexLocker lock(&shard->mutex);
    const auto page = shard->pages.constFind(target.key);
    if (page == shard->pages.cend())
        return limit > 0;
    if (shard->records.findVersion(target))
        return true;
    quint32 count = page->mutableCount;
    for (KisVersionSlotId slot = page->firstVersion; slot.isValid() && count < limit;) {
        const KisVersionRecord *record = shard->records.version(slot);
        Q_ASSERT(record);
        if (!record)
            break;
        if (record->publication == KisPagePublicationState::Retiring)
            ++count;
        slot = record->nextVersion;
    }
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
    const auto page = shard->pages.constFind(key);
    if (page == shard->pages.cend())
        return false;
    const auto *head = shard->records.version(page->publishedVersion);
    Q_ASSERT(head);
    *info = {};
    info->current = shard->records.versionInfo<VersionInfo>(head->version, head->version.generation);
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
    const auto page = shard->pages.constFind(key);
    if (page == shard->pages.cend())
        return result;
    result.exists = true;
    result.total = qsizetype(page->historyCount);
    KisVersionSlotId nextHint = page->firstHistory;
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
        KisPageWorkingArray<KisPageTransitionEffect> prepared(shard->budgetAuthority->storage<char>());
        MetadataArenas::ReleasedBlocks released;
        quint64 debtCookie = 0;
        bool debtPrepared = false;
        const auto cancelDebt = qScopeGuard([&] {
            if (debtPrepared) m_cancelRetirementDebt(m_retirementDebtContext, debtCookie);
        });
        QMutexLocker lock(&shard->mutex);
        auto page = shard->pages.find(key);
        if (page == shard->pages.end() || !shard->canMutate(key, page.value())) return false;
        quint32 selected = 0;
        size_t replicaCount = 0;
        for (qsizetype i = 0; i < count; ++i) {
            if (reachableMask & (quint32(1) << i)) continue;
            if (!versions[i].isValid() || !(versions[i].key == key)) return false;
            KisVersionSlotId slot;
            if (!shard->records.findVersion(versions[i], &slot)) continue;
            const auto *record = shard->records.version(slot);
            if (record->publication != KisPagePublicationState::Historical
                || record->capturedReadViews.isValid()
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
        const quint64 revision = page->revision;
        if (!prepared.empty()) {
            if (!m_prepareRetirementDebt) { ++shard->rejectedTransitions; return false; }
            lock.unlock();
            const bool accepted = m_prepareRetirementDebt(m_retirementDebtContext,
                prepared.data(), qsizetype(prepared.size()), &debtCookie, nullptr);
            debtPrepared = accepted;
            lock.relock();
            if (!accepted) { ++shard->rejectedTransitions; return false; }
            page = shard->pages.find(key);
            if (page == shard->pages.end() || !shard->canMutate(key, page.value())
                || page->revision != revision) continue;
        }
        // All storage and claims exist. Removal only unlinks original records.
        quint64 removed = 0;
        for (qsizetype i = 0; i < count; ++i) {
            if (!(selected & (quint32(1) << i))) continue;
            shard->records.remove(&page.value(), versions[i]);
            ++removed;
        }
        ++page->revision;
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
            const auto *head = shard->records.version(page->publishedVersion);
            Q_ASSERT(head);
            result.push_back(head->version);
        }
        shard->publicationDirectoryHeaders += quint64(shard->pages.size());
    }
    return result;
}

namespace
{
bool localTransition(KisPageTransitionKind kind)
{
    using K = KisPageTransitionKind;
    switch (kind) {
    case K::AcquireWrite:
    case K::AdoptPreparedWrite:
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
    case K::ReplacePrivatePreparedBacking:
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
    return applyOwnerSequence(key, {transition});
}

KisPageMetadataTransitionResult KisPageMetadataCoordinator::applyOwnerSequence(const KisPageKey &key,
                                                                       const QVector<KisPageTransition> &transitions)
{
    if (transitions.isEmpty() || std::any_of(transitions.cbegin(), transitions.cend(), [](const auto &t) {
            return !localTransition(t.kind);
        })) {
        KisPageMetadataTransitionResult result;
        result.rejectionReason = QStringLiteral("owner transition is not a local metadata mutation");
        return result;
    }
    if (transitions.size() == 1 && transitions.first().kind == KisPageTransitionKind::ReleaseRead)
        return applyReadProtection(key, transitions.first());
    if (transitions.size() == 1 && (transitions.first().kind == KisPageTransitionKind::RetainCapturedVersion
                                  || transitions.first().kind == KisPageTransitionKind::ReleaseCapturedVersion))
        return applyCapturedProtection(key, transitions.first());
    return applyProjectedSequence(key, transitions);
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
    if (page == shard->pages.end() || !shard->canMutate(key, page.value())) {
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
    ++page->revision;
    ++shard->acceptedTransitions;
    ++shard->readProtectionTransitions;
    result.accepted = true;
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
        MetadataArenaGrowth growth(shard->budgetAuthority->storage<char>());
        MetadataArenas::ReleasedBlocks released;
        QMutexLocker lock(&shard->mutex);
        auto page = shard->pages.find(key);
        KisVersionSlotId versionSlot;
        if (page == shard->pages.end() || !shard->canMutate(key, page.value())) {
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
            const quint64 revision = page->revision;
            MetadataArenaDemand demand; demand.overflow = 1;
            const auto storage = growMetadataArenasOutsideLock(shard, demand, &growth, &lock, [&] {
                page = shard->pages.find(key);
                return page != shard->pages.end() && shard->canMutate(key, page.value())
                    && page->revision == revision;
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
        ++page->revision;
        ++shard->acceptedTransitions;
        result.accepted = true;
        return result;
    }
}

KisPageMetadataTransitionResult KisPageMetadataCoordinator::applyProjectedSequence(
    const KisPageKey &key, const QVector<KisPageTransition> &transitions)
try {
    KisPageMetadataTransitionResult result;
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), key);
    if (!shard) {
        result.rejectionReason = QStringLiteral("metadata coordinator is not configured");
        return result;
    }
    for (;;) {
        const auto workingStorage = shard->budgetAuthority->storage<char>();
        MetadataArenaGrowth growth(workingStorage);
        KisPageWorkingArray<KisPageTransitionEffect> effects(workingStorage);
        QMutexLocker lock(&shard->mutex);
        auto page = shard->pages.find(key);
        if (page == shard->pages.end() || !shard->canMutate(key, page.value())) {
            result.rejectionReason = QStringLiteral("page is missing or reserved for publication");
            return result;
        }
        KisPageWorkingState input(workingStorage);
        input.setHeader(shard->header(key, page.value()));
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
        for (const auto &transition : transitions) {
            add(transition.baseVersion);
            add(transition.version);
            for (const auto &version : transition.versions)
                add(version);
            if (transition.kind == KisPageTransitionKind::AcquireRead) {
                KisPageVersion leaseOwner;
                if (shard->records.readLeaseOwner(page.value(), transition.lease, &leaseOwner))
                    add(leaseOwner);
            }
            if (transition.kind == KisPageTransitionKind::AbortTransaction) {
                shard->records.forPreparedTransaction(page.value(), transition.transaction, add);
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
        auto next = input;
        const KisPageStateMachine machine;
        for (const auto &transition : transitions) {
            auto step = machine.applyKnownValid(std::move(next), transition);
            if (!step.accepted) {
                ++shard->rejectedTransitions;
                result.rejectionReason = step.rejectionReason;
                return result; // no authoritative record has changed
            }
            next = std::move(step.next);
            effects.insert(effects.end(), step.effects.cbegin(), step.effects.cend());
        }
        // Only projected records are installed/removed. Unrelated history and its
        // reader/pin/last-use state are neither copied nor overwritten.
        const quint64 expectedRevision = page->revision;
        const auto discardStaleAttempt = [&] {
            Q_ASSERT(shard->localTransitionSequences > 0);
            Q_ASSERT(shard->localVersionInputs >= quint64(input.versions.size()));
            --shard->localTransitionSequences;
            shard->localVersionInputs -= quint64(input.versions.size());
            if (background) {
                Q_ASSERT(shard->backgroundLocalTransitionSequences > 0);
                Q_ASSERT(shard->backgroundLocalVersionInputs >= quint64(input.versions.size()));
                --shard->backgroundLocalTransitionSequences;
                shard->backgroundLocalVersionInputs -= quint64(input.versions.size());
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
            if (!m_prepareRetirementDebt(m_retirementDebtContext,
                                          effects.data(), qsizetype(effects.size()),
                                          &retirementDebtCookie,
                                          &debtError)) {
                ++shard->rejectedTransitions;
                result.rejectionReason = debtError.isEmpty()
                    ? QStringLiteral("retirement debt budget is exhausted") : debtError;
                return result;
            }
            retirementDebtPrepared = true;
            lock.relock();
            page = shard->pages.find(key);
            if (page == shard->pages.end() || !shard->canMutate(key, page.value())
                || page->revision != expectedRevision) {
                discardStaleAttempt();
                continue;
            }
        }
        const MetadataArenaDemand demand = shard->records.batchDemand(next.versions);
        const auto storage = growMetadataArenasOutsideLock(shard, demand, &growth, &lock, [&] {
            page = shard->pages.find(key);
            return page != shard->pages.end() && shard->canMutate(key, page.value())
                && page->revision == expectedRevision;
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
            result.rejectionReason = QStringLiteral("metadata arena budget is exhausted");
            return result;
        }
        const auto nextHeader = next.header();
        if (!shard->ensureActivityCapacity(key, &nextHeader, &result.rejectionReason)) {
            result.rejectionReason = QStringLiteral("metadata activity capacity budget is exhausted");
            return result;
        }
        // The next values already witness retention. No second removal set
        // may allocate after installation starts.
        if (!shard->records.putBatch(&page.value(), next.versions, workingStorage)) {
            ++shard->rejectedTransitions;
            result.rejectionReason = QStringLiteral("metadata index reservation or physical ownership conflict");
            return result;
        }
        quint64 removed = 0;
        for (const auto &version : std::as_const(input.versions)) {
            if (!next.findVersion(version.version)) {
                shard->records.remove(&page.value(), version.version);
                ++removed;
            }
        }
        const bool headerInstalled = shard->installHeader(key, &page.value(), nextHeader);
        Q_ASSERT(headerInstalled);
        Q_UNUSED(headerInstalled);
        ++page->revision;
        shard->localVersionInstalls += quint64(next.versions.size());
        shard->localVersionRemovals += removed;
        if (background) {
            shard->backgroundLocalVersionInstalls += quint64(next.versions.size());
            shard->backgroundLocalVersionRemovals += removed;
        }
        shard->acceptedTransitions += quint64(transitions.size());
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
    refused.rejectionReason = QStringLiteral("metadata transition working storage was refused");
    return refused;
}

QVector<KisPageKey> KisPageMetadataCoordinator::pageKeys() const
{
    QVector<KisPageKey> result;
    visitPageKeys(&result, +[](void *p, const KisPageKey &key) {
        static_cast<QVector<KisPageKey> *>(p)->append(key);
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
            visit(context, page.key()); // Under shard lock: consumer must not reenter metadata.
    }
}

QVector<KisReplicaHandle> KisPageMetadataCoordinator::shutdownReplicaHandles() const
{
    QVector<MetadataShard *> shards;
    {
        QMutexLocker lock(&m_configurationMutex);
        const auto *d = m_core.load(std::memory_order_acquire);
        if (!d) return {};
        shards.reserve(qsizetype(d->shards.size()));
        for (const auto &shard : d->shards) shards.append(shard.get());
    }
    QVector<KisReplicaHandle> result;
    for (MetadataShard *shard : std::as_const(shards)) {
        QMutexLocker lock(&shard->mutex);
        for (const MetadataPage &page : std::as_const(shard->pages)) {
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
            metrics.cpuReadBindings += quint64(shard->cpuBindings.size());
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
        result.replicaArena += shard->arenas.replicas.statistics();
        result.overflowArena += shard->arenas.overflow.statistics();
        result.exactVersionIndex += shard->records.exactVersions.statistics();
        result.physicalSlotIndex += shard->records.physicalSlots.statistics();
        result.owningCapacityBytes += shard->ownedCapacity.chargedBytes()
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
        for (const MetadataPage &page : std::as_const(shard->pages)) {
            result.versions += page.versionCount;
            for (KisVersionSlotId versionSlot = page.firstVersion; versionSlot.isValid();) {
                const KisVersionRecord *version = shard->records.version(versionSlot);
                Q_ASSERT(version);
                if (!version)
                    break;
                result.trackedPayloadBytes += sizeof(KisVersionRecord);
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

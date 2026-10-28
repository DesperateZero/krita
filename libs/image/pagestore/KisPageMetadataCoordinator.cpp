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

KisReplicaHandle replicaHandle(const KisPageReplicaState &record, const KisPageVersion &version)
{ return record.handle(version); }

using VersionArena = KisShardSlotArena<KisPageValueCell, 16 * 1024>;
using ReplicaArena = KisShardSlotArena<KisPageReplicaState, 32 * 1024>;
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
    qsizetype sealedCpuValues = 0;
    quint64 cpuReadBindings = 0;
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
                                               const MetadataArenaDemand &demand)
{
    MetadataArenaGrowthPlan plan{arenaBlocksRequired(arenas.versions, demand.versions),
        arenaBlocksRequired(arenas.replicas, demand.replicas),
        arenaBlocksRequired(arenas.overflow, demand.overflow)};
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
    explicit MetadataArenaGrowth(KisPageWorkingStorage storage)
        : versions(storage), replicas(storage), overflow(storage)
        , versionDirectory(storage), replicaDirectory(storage), overflowDirectory(storage) {}
    // Candidate payloads are destroyed before the reservation that accounts
    // for them when preparation exits early.
    KisBackingBudgetReservation budgetReservation;
    KisPageWorkingArray<VersionArena::PreparedBlock> versions;
    KisPageWorkingArray<ReplicaArena::PreparedBlock> replicas;
    KisPageWorkingArray<OverflowArena::PreparedBlock> overflow;
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
        delta.bucket(size_t(KisBackingBudgetClass::MetadataArena)).cpuRam =
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
    KisPageWriteClaim writeClaim;
    bool hasMetadata() const { return publishedEpoch.isValid(); }
    KisVersionSlotId firstVersion;
    KisVersionSlotId lastVersion;
    KisVersionSlotId firstHistory;
    KisVersionSlotId lastHistory;
    KisVersionSlotId firstMutable;
    KisVersionSlotId lastMutable;
    // Before registration this is the initial root's memory-only identity.
    // A valid epoch distinguishes actual content, without a second directory.
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

static_assert(sizeof(MetadataPage) == 176);

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

    KisPageValueCell *version(KisVersionSlotId slot)
    {
        auto *value = arenas->versions.get(slot);
        return value && value->indexed ? value : nullptr;
    }
    const KisPageValueCell *version(KisVersionSlotId slot) const
    {
        const auto *value = arenas->versions.get(slot);
        return value && value->indexed ? value : nullptr;
    }
    KisPageReplicaState *replica(KisReplicaSlotId slot)
    {
        if (!kisIsInlineReplica(slot)) return arenas->replicas.get(slot);
        auto *value = arenas->versions.get(kisInlineReplicaOwner(slot));
        return value && value->primaryReplica.ownerVersion == value->slot ? &value->primaryReplica : nullptr;
    }
    const KisPageReplicaState *replica(KisReplicaSlotId slot) const
    {
        if (!kisIsInlineReplica(slot)) return arenas->replicas.get(slot);
        const auto *value = arenas->versions.get(kisInlineReplicaOwner(slot));
        return value && value->primaryReplica.ownerVersion == value->slot ? &value->primaryReplica : nullptr;
    }
    bool eraseReplica(KisReplicaSlotId slot)
    {
        if (!kisIsInlineReplica(slot)) return arenas->replicas.erase(slot);
        auto *primary = replica(slot);
        if (!primary) return false;
        *primary = {};
        return true;
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

    bool isPreparedBy(const KisPageVersion &identity, KisPageTransactionId transaction, bool nativeValue = false,
                      const KisReplicaHandle *authority = nullptr, KisVersionSlotId slot = {}) const
    {
        if (!slot.isValid() && !findVersion(identity, &slot))
            return false;
        const KisPageValueCell *record = version(slot);
        return record && record->version == identity
            && record->publication == KisPagePublicationState::Prepared && record->preparedBy == transaction
            && (!nativeValue || record->native.sealedBy == transaction)
            && (!authority || projectReplica(record->authorityReplica) == *authority);
    }

    bool physicalOwner(const PhysicalSlot &physical, KisPageVersion *identity) const
    {
        KisReplicaSlotId replicaSlot;
        if (!physicalSlots.findExact(physical, &replicaSlot))
            return false;
        const KisPageReplicaState *storedReplica = replica(replicaSlot);
        const KisPageValueCell *storedVersion = storedReplica ? version(storedReplica->ownerVersion) : nullptr;
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
            const KisPageValueCell *storedVersion = version(versionSlot);
            Q_ASSERT(storedVersion);
            if (!storedVersion) break;
            for (KisReplicaSlotId replicaSlot = storedVersion->firstReplica; replicaSlot.isValid();) {
                const KisPageReplicaState *storedReplica = replica(replicaSlot);
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

    void linkVersion(MetadataPage *page, KisPageValueCell *record)
    {
        Q_ASSERT(page && record && record->slot.isValid());
        record->previousVersion = page->lastVersion;
        record->nextVersion = {};
        if (page->lastVersion.isValid()) {
            KisPageValueCell *previous = version(page->lastVersion);
            Q_ASSERT(previous);
            previous->nextVersion = record->slot;
        } else {
            page->firstVersion = record->slot;
        }
        page->lastVersion = record->slot;
        ++page->versionCount;
    }

    void unlinkVersion(MetadataPage *page, KisPageValueCell *record)
    {
        Q_ASSERT(page && record && page->versionCount > 0);
        if (record->previousVersion.isValid()) {
            KisPageValueCell *previous = version(record->previousVersion);
            Q_ASSERT(previous);
            previous->nextVersion = record->nextVersion;
        } else {
            page->firstVersion = record->nextVersion;
        }
        if (record->nextVersion.isValid()) {
            KisPageValueCell *next = version(record->nextVersion);
            Q_ASSERT(next);
            next->previousVersion = record->previousVersion;
        } else {
            page->lastVersion = record->previousVersion;
        }
        record->previousVersion = {};
        record->nextVersion = {};
        --page->versionCount;
    }

    void linkHistory(MetadataPage *page, KisPageValueCell *record)
    {
        Q_ASSERT(page && record && record->publication == KisPagePublicationState::Historical);
        KisVersionSlotId next;
        if (page->lastHistory.isValid()) {
            const KisPageValueCell *last = version(page->lastHistory);
            Q_ASSERT(last);
            if (!last || !(position(last->version) < position(record->version))) {
                next = page->firstHistory;
                while (next.isValid()) {
                    const KisPageValueCell *candidate = version(next);
                    Q_ASSERT(candidate);
                    if (!candidate || !(position(candidate->version) < position(record->version)))
                        break;
                    next = candidate->nextHistory;
                }
            }
        }
        record->nextHistory = next;
        if (next.isValid()) {
            KisPageValueCell *nextRecord = version(next);
            Q_ASSERT(nextRecord);
            record->previousHistory = nextRecord->previousHistory;
            nextRecord->previousHistory = record->slot;
        } else {
            record->previousHistory = page->lastHistory;
            page->lastHistory = record->slot;
        }
        if (record->previousHistory.isValid()) {
            KisPageValueCell *previous = version(record->previousHistory);
            Q_ASSERT(previous);
            previous->nextHistory = record->slot;
        } else {
            page->firstHistory = record->slot;
        }
        ++page->historyCount;
    }

    void unlinkHistory(MetadataPage *page, KisPageValueCell *record)
    {
        Q_ASSERT(page && record && page->historyCount > 0);
        if (record->previousHistory.isValid()) {
            KisPageValueCell *previous = version(record->previousHistory);
            Q_ASSERT(previous);
            previous->nextHistory = record->nextHistory;
        } else {
            page->firstHistory = record->nextHistory;
        }
        if (record->nextHistory.isValid()) {
            KisPageValueCell *next = version(record->nextHistory);
            Q_ASSERT(next);
            next->previousHistory = record->previousHistory;
        } else {
            page->lastHistory = record->previousHistory;
        }
        record->previousHistory = {};
        record->nextHistory = {};
        --page->historyCount;
    }

    void linkMutable(MetadataPage *page, KisPageValueCell *record)
    {
        Q_ASSERT(page && record);
        record->nextMutable = {};
        if (page->lastMutable.isValid()) {
            KisPageValueCell *previous = version(page->lastMutable);
            Q_ASSERT(previous);
            previous->nextMutable = record->slot;
        } else {
            page->firstMutable = record->slot;
        }
        page->lastMutable = record->slot;
        ++page->mutableCount;
    }

    void unlinkMutable(MetadataPage *page, KisPageValueCell *record)
    {
        Q_ASSERT(page && record && page->mutableCount > 0);
        KisVersionSlotId previousSlot;
        for (KisVersionSlotId slot = page->firstMutable; slot.isValid() && !(slot == record->slot);) {
            const KisPageValueCell *candidate = version(slot);
            Q_ASSERT(candidate);
            if (!candidate)
                break;
            previousSlot = slot;
            slot = candidate->nextMutable;
        }
        if (previousSlot.isValid()) {
            KisPageValueCell *previous = version(previousSlot);
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

    void indexPublication(MetadataPage *page, KisPageValueCell *record)
    {
        if (record->publication == KisPagePublicationState::Historical) {
            linkHistory(page, record);
        }
        if (record->publication == KisPagePublicationState::Prepared
            || record->publication == KisPagePublicationState::Unpublished) {
            linkMutable(page, record);
        }
    }

    void unindexPublication(MetadataPage *page, KisPageValueCell *record)
    {
        if (record->publication == KisPagePublicationState::Historical) {
            unlinkHistory(page, record);
        }
        if (record->publication == KisPagePublicationState::Prepared
            || record->publication == KisPagePublicationState::Unpublished) {
            unlinkMutable(page, record);
        }
    }

    bool setPublication(MetadataPage *page, KisPageValueCell *record,
                        KisPagePublicationState publication, KisPageTransactionId preparedBy)
    {
        if (!record) return false;
        if (record->publication == publication && record->preparedBy == preparedBy) return true;
        unindexPublication(page, record);
        record->publication = publication;
        record->preparedBy = preparedBy;
        indexPublication(page, record);
        return true;
    }
    bool setPublication(MetadataPage *page, const KisPageVersion &identity,
                        KisPagePublicationState publication, KisPageTransactionId preparedBy)
    {
        KisVersionSlotId slot;
        return findVersion(identity, &slot) && setPublication(page, version(slot), publication, preparedBy);
    }

    void unindexPhysical(const KisPageValueCell &record)
    {
        for (KisReplicaSlotId slot = record.firstReplica; slot.isValid();) {
            const KisPageReplicaState *stored = replica(slot);
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

    void eraseChildren(KisPageValueCell *record)
    {
        Q_ASSERT(record);
        eraseOverflowChain(record->capturedReadViews);
        for (KisReplicaSlotId slot = record->firstReplica; slot.isValid();) {
            KisPageReplicaState *stored = replica(slot);
            Q_ASSERT(stored);
            if (!stored)
                break;
            const KisReplicaSlotId next = stored->nextReplica;
            eraseOverflowChain(stored->overflow);
            const bool erased = eraseReplica(slot);
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
    MetadataArenaDemand batchDemand(const Snapshots &snapshots) const
    {
        MetadataArenaDemand result;
        for (const auto &snapshot : snapshots) {
            if (!exactVersions.findExact(snapshot.version)) {
                ++result.versions;
            }
            result.replicas += snapshot.replicas.empty() ? 0 : quint64(snapshot.replicas.size() - 1);
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
    void assign(KisPageValueCell *record,
                const Snapshot &snapshot,
                MetadataArenaReservations *reservation)
    {
        Q_ASSERT(record && reservation);
        auto *native = &record->native;
        const auto *selected = replica(native->nativeReplica);
        const auto selectedIdentity = selected ? selected->identity : KisReplicaAllocationIdentity{};
        native->nativeReplica = {};
        // Cold import may seed the independent oracle count. Existing Values
        // own their live read edges; a prepared snapshot cannot restore an
        // edge already returned while its publication claim was outstanding.
        if (!record->version.isValid()) record->readEdges = snapshot.nativeReaders;
        const KisVersionSlotId previousVersion = record->previousVersion;
        const KisVersionSlotId nextVersion = record->nextVersion;
        const KisVersionSlotId previousHistory = record->previousHistory;
        const KisVersionSlotId nextHistory = record->nextHistory;
        const KisVersionSlotId nextMutable = record->nextMutable;
        eraseChildren(record);
        auto retainedNative = std::move(record->native);
        const bool frameHeld = record->frameHeld;
        const bool indexed = record->indexed;
        // A root can already name this final identity. Reset mutable state,
        // never its immutable header or independent memory references.
        Q_ASSERT(!record->version.isValid() || record->version == snapshot.version);
        static_cast<KisPageValueState &>(*record) = {};
        record->native = std::move(retainedNative);
        record->frameHeld = frameHeld;
        record->indexed = indexed;
        if (!record->version.isValid()) record->version = snapshot.version;
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
            KisPageReplicaState next;
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
            const KisReplicaSlotId replicaSlot = replicaTail.isValid()
                ? arenas->replicas.emplaceReserved(&reservation->replicas, next) : kisInlineReplicaSlot(record->slot);
            if (!replicaTail.isValid()) record->primaryReplica = next;
            Q_ASSERT(replicaSlot.isValid());
            if (native->nativeBinding && next.identity == selectedIdentity)
                native->nativeReplica = replicaSlot;
            KisPageReplicaState *inserted = replica(replicaSlot);
            Q_ASSERT(inserted);
            if (!record->firstReplica.isValid())
                record->firstReplica = replicaSlot;
            if (replicaTail.isValid()) {
                KisPageReplicaState *previous = replica(replicaTail);
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
    Version project(const KisPageValueCell &record, Version result) const
    {
        result.version = record.version;
        result.publication = record.publication;
        result.preparedBy = record.preparedBy;
        result.nativeReaders = record.version.isDefaultPixel() ? 0 : record.readEdges;
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
            const KisPageReplicaState *stored = replica(slot);
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
        const KisPageValueCell *record = version(slot);
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
        const KisPageValueCell *record = version(owner);
        if (!record || !(record->version == identity.version))
            return false;
        for (KisReplicaSlotId current = record->firstReplica; current.isValid();) {
            const KisPageReplicaState *stored = replica(current);
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
        const KisPageReplicaState *stored = replica(slot);
        const KisPageValueCell *owner = stored ? version(stored->ownerVersion) : nullptr;
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
    Info versionInfo(const KisPageValueCell *stored, KisPageGeneration publishedGeneration) const
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
        result.captured = stored->capturedReadViews.isValid() || (!stored->version.isDefaultPixel() && stored->readEdges);
        return result;
    }

    KisPageStateSnapshot
    header(const KisPageKey &key, const MetadataPage &page, const MetadataPageActivity *activity) const
    {
        KisPageStateSnapshot result;
        result.key = key;
        result.publishedEpoch = page.publishedEpoch;
        result.nextGeneration = page.nextGeneration;
        const KisPageValueCell *published = version(page.publishedVersion);
        Q_ASSERT(published && published->publication == KisPagePublicationState::Published);
        if (published) {
            result.publishedGeneration = published->version.generation;
            result.publishedDefaultPixelRevision = published->version.defaultPixelRevision;
        }
        if (!activity)
            return result;
        if (activity->nativeWrite) {
            result.nativeGeneration = activity->nativeWrite->version().generation;
            result.nativeTransaction = activity->writerTransaction;
        }
        if (activity->writerPhase != KisPageWriterPhase::None) {
            const KisPageValueCell *base = version(activity->writerBaseVersion);
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
            const KisPageValueCell *record = version(slot);
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
            const KisPageReplicaState *stored = replica(existing);
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
        auto *initial = !page->hasMetadata() ? arenas->versions.get(page->publishedVersion) : nullptr;
        const bool initialized = !existing && initial && initial->initialIdentity
            && initial->version == snapshot.version;
        if (initialized) slot = initial->slot;
        if (!existing) {
            if (!initialized) slot = arenas->versions.emplaceReserved(&reservation->arenas.versions);
            if (!slot.isValid()) return false;
            KisPageValueCell *record = arenas->versions.get(slot);
            Q_ASSERT(record);
            if (!initialized) record->slot = slot;
            linkVersion(page, record);
            assign(record, snapshot, &reservation->arenas);
            const bool indexed = exactVersions.insertReserved(&reservation->exactVersions, snapshot.version, slot);
            Q_ASSERT(indexed);
            Q_UNUSED(indexed);
            indexPublication(page, record);
        } else {
            auto *record = version(slot);
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

        KisPageValueCell *installed = arenas->versions.get(slot);
        Q_ASSERT(installed);
        if (!installed->indexed) installed->attachLifetime();
        installed->indexed = true;
        for (KisReplicaSlotId replicaSlot = installed->firstReplica; replicaSlot.isValid();) {
            const KisPageReplicaState *stored = replica(replicaSlot);
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

    KisVersionSlotId installNativeValue(MetadataPage *page, KisPagePrivateWrite &native,
        KisPagePublicationState publication = KisPagePublicationState::Prepared)
    {
        const auto slot = native.location.value;
        auto *record = native.value;
        Q_ASSERT(record && arenas->versions.get(slot) == record && !record->indexed
            && record->version == native.version());
        const auto cpuSlot = record->authorityReplica;
        record->publication = publication;
        record->preparedBy = publication == KisPagePublicationState::Prepared
            ? native.sealedBy() : KisPageTransactionId{};
        record->primaryReplica.validity = KisReplicaValidity::Valid;
        const bool exact = exactVersions.insertReserved(&native.location.exact, record->version, slot);
        const bool physical = physicalSlots.insertReserved(&native.location.physical,
            native.target().physicalSlotIdentity(), cpuSlot);
        Q_ASSERT(exact && physical); Q_UNUSED(exact); Q_UNUSED(physical);
        linkVersion(page, record);
        indexPublication(page, record);
        record->indexed = true; // Same cell; the original Frame keeps only memory.
        return slot;
    }

    const KisPageValueCell *unusedRecord(const KisPageVersion &identity, KisVersionSlotId slot = {}) const
    {
        if (identity.isDefaultPixel() || (!slot.isValid() && !findVersion(identity, &slot))) return nullptr;
        const auto *record = version(slot);
        if (!record || !(record->version == identity)) return nullptr;
        const auto *facts = &record->native;
        const auto *cpu = replica(record->authorityReplica);
        const bool accepted = record->publication == KisPagePublicationState::Published
            || (record->publication == KisPagePublicationState::Prepared && facts->sealedBy.isValid());
        return accepted && !record->capturedReadViews.isValid() && !record->readEdges
            && record->replicaCount == 1 && cpu && cpu->domain == KisPageAccessDomain::CpuRam
            && cpu->validity == KisReplicaValidity::Valid && !cpu->pinCount
            && !cpu->overflow.isValid() && !cpu->activeOperation.isValid() ? record : nullptr;
    }

    bool canDetachPublished(const KisPageVersion &identity) const
    {
        const auto *record = unusedRecord(identity);
        return record && record->publication == KisPagePublicationState::Published;
    }

    const KisPageValueCell *unusedPrepared(const KisPageVersion &identity) const
    {
        const auto *record = unusedRecord(identity);
        return record && record->publication == KisPagePublicationState::Prepared ? record : nullptr;
    }

    bool canDetachPrepared(const KisPageVersion &identity) const
    {
        return unusedPrepared(identity);
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
        KisPageValueCell *record = version(slot);
        Q_ASSERT(record && record->version == identity);
        unindexPhysical(*record);
        unindexPublication(page, record);
        unlinkVersion(page, record);
        const auto primary = record->primaryReplica;
        eraseChildren(record);
        if (record->frameHeld) record->primaryReplica = primary;
        const bool indexErased = exactVersions.eraseExact(identity, slot);
        auto *facts = &record->native;
        Q_ASSERT(facts);
        if (releasedDescriptor) releasedDescriptor->swap(facts->descriptor);
        if (facts->nativeBinding) --arenas->cpuReadBindings;
        if (releasedBinding) releasedBinding->swap(facts->nativeBinding);
        if (facts->sealedBy.isValid()) { --arenas->sealedCpuValues; facts->sealedBy = {}; facts->exportedStamp = 0; }
        record->indexed = false;
        const bool recordErased = record->frameHeld || record->readEdges
            || !record->detachLifetime()
            || arenas->versions.erase(slot);
        Q_ASSERT(indexErased && recordErased);
        Q_UNUSED(indexErased);
        Q_UNUSED(recordErased);
    }

    template<class Snapshots>
    MetadataArenaDemand initialDemand(const MetadataPage *page, const Snapshots &snapshots) const
    {
        auto result = batchDemand(snapshots);
        const auto *initial = page ? arenas->versions.get(page->publishedVersion) : nullptr;
        if (initial && initial->initialIdentity && std::any_of(snapshots.begin(), snapshots.end(),
            [&](const auto &snapshot) { return snapshot.version == initial->version; })) --result.versions;
        return result;
    }

    template<class Snapshots>
    bool initialize(MetadataPage *page, const Snapshots &snapshots)
    {
        Q_ASSERT(page && !page->hasMetadata() && !page->firstVersion.isValid());
        // Registration consumes only new identities or the original cells
        // already selected by the initial root. There is no arbitrary whole-
        // page replacement contract, clear pass or reconstruction of survivors.
        qsizetype replicaCount = 0;
        for (const auto &snapshot : snapshots) {
            if (findVersion(snapshot.version)) return false;
            for (const auto &replicaState : snapshot.replicas) {
                if (physicalSlots.findExact(replicaState.replica.physicalSlotIdentity())) return false;
                ++replicaCount;
            }
        }
        const MetadataArenaDemand demand = initialDemand(page, snapshots);
        ReservationSet reservation;
        if (!reserve(demand, qsizetype(snapshots.size()), replicaCount, &reservation)) return false;
        const auto cancel = qScopeGuard([&] { reservation.cancel(this); });
        for (const auto &snapshot : snapshots) {
            const bool stored = putReserved(page, snapshot, &reservation);
            Q_ASSERT(stored); Q_UNUSED(stored);
        }
        return true;
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
            const KisPageValueCell *record = version(slot);
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

struct MetadataShard;
struct MetadataReleasedReferences {
    std::shared_ptr<const KisPageAllocationDescriptor> descriptor;
    std::shared_ptr<KisCpuResidentBinding> binding;
};
struct MetadataAcceptanceReferences : MetadataReleasedReferences {
    KisReplicaHandle targetAuthority;
    KisReplicaHandle source;
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
    { return page.hasMetadata() && page.revisionCanAdvance() && !page.activity.publicationClaim; }

    void acceptNativeResult(MetadataPage &page, KisVersionSlotId slot, KisPagePrivateWrite &native)
    {
        auto *record = records.version(slot);
        auto *facts = &record->native;
        Q_ASSERT(native.value == record && record->frameHeld);
        facts->nativeReplica = record->authorityReplica;
        arenas.cpuReadBindings += bool(facts->nativeBinding);
        Q_ASSERT(page.activity.nativeWrite == &native);
        page.activity.nativeWrite = nullptr;
        page.activity.writerTransaction = {};
        native.reserved = false;
        ++nativeValuesAccepted;
    }

    bool validDetachment(const MetadataPage &page, const KisPageVersion &version,
        const KisPagePrivateWrite *native, KisPageTransactionId transaction,
        bool nativeValue, const KisReplicaHandle *authority, KisVersionSlotId slot = {}) const
    {
        const auto &activity = page.activity;
        if (native ? activity.nativeWrite != native || !native->reserved || !native->completed
                        || !native->isSealed() || !(native->sealedBy() == transaction)
                   : activity.hasWriter()) return false;
        return (native && version == native->version())
            || records.isPreparedBy(version, transaction, nativeValue, authority, slot);
    }

    void installDetachment(MetadataPage &page, const KisPageVersion &version, KisPagePrivateWrite *native,
        bool releaseCapacity, std::shared_ptr<const KisPageAllocationDescriptor> *releasedDescriptor,
        std::shared_ptr<KisCpuResidentBinding> *releasedBinding, KisVersionSlotId sourceSlot = {})
    {
        if (!native || !(version == native->version())) {
            if (releaseCapacity) records.remove(&page, version, releasedDescriptor, releasedBinding, sourceSlot);
            else {
                const bool updated = sourceSlot.isValid()
                    ? records.setPublication(&page, records.version(sourceSlot), KisPagePublicationState::Historical, {})
                    : records.setPublication(&page, version, KisPagePublicationState::Historical, {});
                Q_ASSERT(updated); Q_UNUSED(updated);
                ++publicationHistoryNodesTransferred;
            }
        }
        if (native) {
            const auto accepted = records.installNativeValue(&page, *native);
            ++publicationAdditionRecordsTransferred;
            ++publicationVersionInstalls;
            acceptNativeResult(page, accepted, *native);
        }
        ++page.revision;
        ++installedMutationPages;
    }

    template<class Visit> void visitMutable(MetadataPage &page, Visit visit)
    {
        for (auto slot = page.firstMutable; slot.isValid();) {
            auto *record = records.version(slot);
            Q_ASSERT(record);
            slot = record->nextMutable; // An edit may unlink this original node.
            visit(*record);
        }
    }
    struct IncomingCommit {
        const KisPageVersion &version;
        KisPagePublicationState publication = KisPagePublicationState::Prepared;
        KisPageTransactionId preparedBy;
    };
    bool validPublication(MetadataPage &page, const KisPagePublicationChange &input,
                          const KisPagePublicationChange::MetadataAcceptance &work,
                          KisPageTransactionId transaction, KisImageEpochId epoch) const
    {
        const auto *native = input.replacement.native;
        const auto *published = records.version(page.publishedVersion);
        if (!published || (native ? page.activity.nativeWrite != native || !native->reserved
                || !native->completed || !native->isSealed() || !(native->sealedBy() == transaction)
                : bool(page.activity.nativeWrite))) return false;
        if (input.supersededAuthority && input.supersededAuthority->isValid()
            && !(records.projectReplica(published->authorityReplica) == *input.supersededAuthority)) return false;
        const auto visit = [&](auto apply) {
            for (auto slot = page.firstMutable; slot.isValid();) {
                const auto *record = records.version(slot); Q_ASSERT(record);
                apply(*record); slot = record->nextMutable;
            }
        };
        const KisPageCommitFacts facts{transaction, epoch};
        if (native) {
            const IncomingCommit incoming{native->version(), KisPagePublicationState::Prepared, transaction};
            return kisPageValidateCommit(page.publishedEpoch, KisPageGeneration{}, facts, &incoming, published, visit).isEmpty();
        }
        const auto *target = records.version(work.replacementSlot);
        if (!target || !(target->version == input.replacement.version())) return false;
        if (input.canonicalRecord) {
            const auto *original = static_cast<const MetadataAcceptanceReferences *>(input.canonicalRecord);
            if (!(records.projectReplica(target->authorityReplica) == original->targetAuthority)) return false;
        }
        if (input.replacement.isNativeValue() && !(target->native.sealedBy == transaction)) return false;
        return kisPageValidateCommit(page.publishedEpoch, KisPageGeneration{}, facts, target, published, visit).isEmpty();
    }
    void installPublished(MetadataPage &page, KisPagePublicationChange &input,
        const KisPagePublicationChange::MetadataAcceptance &work, KisPageTransactionId transaction, KisImageEpochId epoch,
        std::shared_ptr<const KisPageAllocationDescriptor> *releasedDescriptor,
        std::shared_ptr<KisCpuResidentBinding> *releasedBinding)
    {
        auto *native = input.replacement.native;
        auto *published = records.version(page.publishedVersion); Q_ASSERT(published);
        const KisPageCommitFacts facts{transaction, epoch};
        const auto edit = [&](auto &record, auto state, auto preparedBy) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(record)>, IncomingCommit>) {
                if (record.slot == page.publishedVersion && input.detachesSuperseded) {
                    records.remove(&page, record.version, releasedDescriptor, releasedBinding, record.slot);
                } else {
                    const bool changed = records.setPublication(&page, &record, state, preparedBy);
                    Q_ASSERT(changed); Q_UNUSED(changed);
                    publicationHistoryNodesTransferred += state == KisPagePublicationState::Historical;
                    ++publicationVersionInstalls;
                }
            }
        };
        const auto visit = [&](auto apply) { visitMutable(page, apply); };
        KisVersionSlotId accepted;
        if (native) {
            const IncomingCommit incoming{native->version(), KisPagePublicationState::Prepared, transaction};
            kisPageApplyCommit(facts, &incoming, published, visit, edit);
            accepted = records.installNativeValue(&page, *native, KisPagePublicationState::Published);
            ++publicationAdditionRecordsTransferred;
            ++publicationVersionInstalls;
            acceptNativeResult(page, accepted, *native);
        } else {
            accepted = work.replacementSlot;
            auto *target = records.version(accepted); Q_ASSERT(target);
            kisPageApplyCommit(facts, target, published, visit, edit);
        }
        page.publishedVersion = accepted;
        page.publishedEpoch = epoch;
        ++page.revision;
        ++installedPublicationPages;
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
        if (!page || !records.initialize(page, state.versions))
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

bool readableVersionLocked(const KisPageValueCell &version)
{
    return version.publication == KisPagePublicationState::Published
        || version.publication == KisPagePublicationState::Historical
        || (version.publication == KisPagePublicationState::Prepared
            && (version.capturedReadViews.isValid() || (!version.version.isDefaultPixel() && version.readEdges)));
}

template<class Shard>
auto readableVersion(Shard &shard, const KisPageVersion &identity)
    -> decltype(shard.records.version(KisVersionSlotId{}))
{
    KisVersionSlotId slot;
    if (!(shard.pages.find(identity.key) != shard.pages.end()) || !shard.records.findVersion(identity, &slot)) return nullptr;
    auto *version = shard.records.version(slot);
    return version && readableVersionLocked(*version)
        ? version : nullptr; // Borrowed only while the caller holds the shard gate.
}

bool cpuReadableReplica(const KisPageReplicaState &state, const KisPageVersion &version)
{
    return state.validity == KisReplicaValidity::Valid && replicaHandle(state, version).isValid()
        && (state.domain == KisPageAccessDomain::CpuRam || state.domain == KisPageAccessDomain::UmaShared);
}

} // namespace

class KisPageMetadataCoordinator::Private final : public KisPageViewValueStorage
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

    KisPageViewValue *pin(const KisPageVersion &, KisVersionSlotId = {},
                         const KisPagePrivateWrite * = nullptr) override;
    KisPageViewValue *prepareInitial(const KisPageVersion &) override;
    void releaseMemory(const KisPageKey &, KisVersionSlotId, KisPageViewValue *) noexcept override;
    void detachLiveValues() noexcept;

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
        // Only the noncopyable original row carries this location. Acceptance
        // or cancellation consumes its reservation. The memory hold remains
        // until Frame cleanup and grants no authority in another coordinator.
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
    std::atomic<bool> attached{true};
};

KisPageViewValue *KisPageMetadataCoordinator::Private::pin(
    const KisPageVersion &identity, KisVersionSlotId slot, const KisPagePrivateWrite *original)
{
    if (!attached.load(std::memory_order_acquire) || !identity.isValid()) return nullptr;
    if (original) {
        // The frozen original row already holds this final Value's memory.
        // Its immutable location proves the owning core without a directory
        // lookup or a second shard gate. This grants no content permission.
        if (!original->value || original->location.shardIndex >= shards.size()
            || original->location.shard != shards[original->location.shardIndex].get()
            || !(original->version() == identity) || !(original->valueSlot() == slot)) return nullptr;
        retain(original->value);
        return original->value;
    }
    auto *shard = shardFor(this, identity.key);
    QMutexLocker lock(&shard->mutex);
    if (!slot.isValid() && !shard->records.findVersion(identity, &slot)) {
        const auto page = shard->pages.find(identity.key);
        if (page == shard->pages.end()) return nullptr;
        const auto *writer = page->second.activity.nativeWrite;
        if (writer && writer->version() == identity) slot = writer->valueSlot();
        else if (!page->second.hasMetadata()) slot = page->second.publishedVersion;
        else return nullptr;
    }
    auto *value = shard->arenas.versions.get(slot);
    if (!value || !(value->version == identity)
        || !(value->indexed || value->frameHeld || value->initialIdentity)) return nullptr;
    retain(value);
    return value;
}

KisPageViewValue *KisPageMetadataCoordinator::Private::prepareInitial(const KisPageVersion &identity)
{
    if (!attached.load(std::memory_order_acquire) || !identity.isValid()) return nullptr;
    auto *shard = shardFor(this, identity.key);
    const auto storageOwner = budgetAuthority->storage<char>();
    MetadataArenaGrowth growth(storageOwner.borrowed());
    decltype(shard->pages) prepared(shard->pages.get_allocator());
    QMutexLocker lock(&shard->mutex);
    auto page = shard->pages.find(identity.key);
    if (page == shard->pages.end()) {
        lock.unlock();
        prepared.try_emplace(identity.key);
        lock.relock();
        auto inserted = shard->pages.insert(prepared.extract(prepared.begin()));
        page = inserted.position;
        // Dispose a losing candidate outside the shard gate.
        if (!inserted.node.empty()) {
            lock.unlock(); inserted.node = {}; lock.relock();
        }
    }
    const auto existing = [&]() -> KisPageValueCell * {
        KisVersionSlotId slot;
        if (!shard->records.findVersion(identity, &slot)) slot = page->second.publishedVersion;
        auto *value = shard->arenas.versions.get(slot);
        return value && value->version == identity ? value : nullptr;
    };
    auto *value = existing();
    if (!value) {
        if (page->second.hasMetadata()) return nullptr;
        const MetadataArenaDemand demand{1, 0, 0};
        const auto ready = growMetadataArenasOutsideLock(shard, demand, &growth, &lock,
            [&] { return !existing() && !page->second.hasMetadata(); }, nullptr);
        value = existing();
        if (ready != MetadataGrowthResult::Ready && !value) return nullptr;
        if (!value) {
            MetadataArenaReservations reservation;
            const auto cancel = qScopeGuard([&] { reservation.cancel(&shard->arenas); });
            if (!reservation.reserve(&shard->arenas, demand)) return nullptr;
            const auto slot = shard->arenas.versions.emplaceReserved(&reservation.versions);
            value = shard->arenas.versions.get(slot);
            Q_ASSERT(value);
            value->version = identity;
            value->slot = slot;
            value->initialIdentity = true;
            page->second.publishedVersion = slot;
        }
    }
    retain(value);
    return value;
}

void KisPageMetadataCoordinator::Private::releaseMemory(
    const KisPageKey &key, KisVersionSlotId slot, KisPageViewValue *) noexcept
{
    MetadataReleasedReferences released;
    auto *shard = shardFor(this, key);
    QMutexLocker lock(&shard->mutex);
    auto *value = shard->arenas.versions.get(slot);
    if (!value || value->indexed || value->frameHeld || value->readEdges
        || value->viewMemoryState.load(std::memory_order_acquire)) return;
    if (value->initialIdentity) {
        const auto page = shard->pages.find(key);
        if (page != shard->pages.end() && !page->second.hasMetadata()
            && page->second.publishedVersion == slot) page->second.publishedVersion = {};
    }
    released.descriptor.swap(value->native.descriptor);
    released.binding.swap(value->native.nativeBinding);
    const bool erased = shard->arenas.versions.erase(slot);
    Q_ASSERT(erased); Q_UNUSED(erased);
}

void KisPageMetadataCoordinator::Private::detachLiveValues() noexcept
{
    // The facade is quiescent and detached. Escaped trees may keep the arena
    // memory, but cannot keep every indexed placement/descriptor alive. Use
    // the same original removal policy and bounded out-of-gate destruction;
    // an orphan Frame still owns its private cell and original final cleanup.
    for (const auto &shard : shards) {
        auto page = shard->pages.begin();
        for (;;) {
            MetadataReleasedReferences released[16];
            QMutexLocker lock(&shard->mutex);
            size_t count = 0;
            while (page != shard->pages.end() && count < 16) {
                const auto slot = page->second.firstVersion;
                if (!slot.isValid()) { ++page; continue; }
                auto *value = shard->records.version(slot);
                Q_ASSERT(value);
                // Facade disposal is quiescent; actual captured capabilities
                // retain the Store facade. Only cold oracle seed counts can
                // remain here, and escaped raw trees grant memory only.
                value->readEdges = 0;
                shard->records.remove(&page->second, value->version,
                    &released[count].descriptor, &released[count].binding, slot);
                ++count;
            }
            if (page == shard->pages.end()) break;
        }
    }
}

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
    static DataPointer create(const Storage &storage, size_t count, size_t shardCount);
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
        quint64 revision = 0;
        KisPageVersion version;
        size_t inputOrdinal = 0;
        size_t shardIndex = 0;
        bool activityReservation = false;
        size_t arenaGrowthIndex = std::numeric_limits<size_t>::max();
        std::shared_ptr<const KisPageAllocationDescriptor> releasedDescriptor;
        std::shared_ptr<KisCpuResidentBinding> releasedReadBinding;
        KisPageKey key() const { return version.key; }
    };
    struct Publication {
        explicit Publication(KisPageWorkingStorage storage)
            : installRecords(decltype(installRecords)::allocator_type(storage)),
              deltas(decltype(deltas)::allocator_type(storage)) {}
        struct PublicationDelta {
            KisPageVersion version;
            KisPagePublicationState publication;
            KisPageTransactionId preparedBy;
        };
        KisPageStateSnapshot next;
        KisPageWorkingArray<Version, 2> installRecords;
        KisPageWorkingArray<PublicationDelta, 2> deltas;
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
        // Existing generic records between additions need no reservation,
        // but remain in the same bounded identity revalidation range.
        growth.entryCount = i - growth.firstEntry + 1;
        growth.totalDemand.versions += demand.versions;
        growth.totalDemand.replicas += demand.replicas;
        growth.totalDemand.overflow += demand.overflow;
        growth.exactInsertions += exactInsertions;
        growth.physicalInsertions += physicalInsertions;
        entry.arenaGrowthIndex = arenaGrowth.size() - 1;
    }

    size_t claimRows();
    void releaseClaims(size_t);
    void installRows(bool mutation, bool recoverable, bool privateWrite, KisImageEpochId);
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
    void popPublication() { if (!publications.empty()) publications.pop_back(); }
    // Page keys and projected versions are already unique. This candidate is
    // immutable before any authority lookup; one sorted paid array avoids
    // buckets and one charged allocation for every copied version identity.
    struct BackingAuthority {
        KisPageVersion version;
        KisReplicaHandle replica;
    };
    Array<BackingAuthority> backingAuthorities;

private:
    Data(const Storage &allocator, size_t units, Entry *entryData, size_t entryCount,
         Publication *publicationData, size_t publicationCount,
         ShardArenaGrowth *growthData, size_t growthCount)
        : storage(allocator), allocationUnits(units), arenaGrowth(growthData, growthCount)
        , entries(entryData, entryCount), publications(publicationData, publicationCount)
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
    const Storage &storage, size_t count, size_t shardCount)
{
    static_assert(alignof(Data) <= alignof(AllocationUnit));
    const size_t publicationCount = count;
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

size_t KisPageMetadataCoordinator::PreparedPublication::Data::claimRows()
{
    qsizetype claimed = 0;
    // All claims are checked before changing any state. Never hold multiple
    // shard locks; a competing mutation is rejected/deferred while claimed.
    // Claims live only inside this synchronous call, with no provider work,
    // root construction, callback or external completion wait inside the
    // claim interval. Individual shard mutex acquisition can still wait.
    for (size_t first = 0; first < entries.size();) {
        const size_t end = batchEnd(first, entries.size());
        QMutexLocker locker(&entries[first].shardOwner->mutex);
        for (size_t i = first; i < end; ++i) {
            auto &entry = entries[i];
            auto page = entry.originalPage;
            if (!entry.shardOwner->canMutate(entry.key(), page->second))
                break;
            if (page->second.revision != entry.revision)
                break;
            Q_ASSERT(entry.activityReservation);
            // Keep the existing reservation until this Data is cleaned outside
            // installation. Clearing the transient claim cannot then erase/free
            // an otherwise empty activity node under the publication gate.
            page->second.activity.publicationClaim = this;
            ++claimed;
        }
        if (claimed != qsizetype(end)) break;
        first = end;
    }
    return size_t(claimed);
}

void KisPageMetadataCoordinator::PreparedPublication::Data::releaseClaims(size_t claimed)
{
    for (size_t first = 0; first < claimed;) {
        const size_t end = batchEnd(first, claimed);
        QMutexLocker locker(&entries[first].shardOwner->mutex);
        for (size_t i = first; i < end; ++i)
            entries[i].originalPage->second.activity.publicationClaim = nullptr;
        first = end;
    }
}

void KisPageMetadataCoordinator::PreparedPublication::Data::installRows(
    bool mutation, bool recoverable, bool privateWrite, KisImageEpochId epoch)
{
    Q_ASSERT(publications.empty() || publications.size() == entries.size());
    for (size_t first = 0; first < entries.size();) {
        const size_t end = batchEnd(first, entries.size());
        QMutexLocker locker(&entries[first].shardOwner->mutex);
        for (size_t i = first; i < end; ++i) {
            auto &entry = entries[i];
            auto *publication = &publications[i];
            auto page = entry.originalPage;
            Q_ASSERT(page->second.activity.publicationClaim == this);
            quint64 installedHistoryLinks = 0;
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
            if (recoverable) {
                auto &reservation = arenaGrowth.at(entry.arenaGrowthIndex).reservations;
                for (const auto &record : publication->installRecords) {
                    const bool installed = entry.shardOwner->records.putReserved(&page->second, record, &reservation);
                    Q_ASSERT(installed);
                    Q_UNUSED(installed);
                }
                const bool headerInstalled = entry.shardOwner->installReservedHeader(entry.key(), &page->second, publication->next);
                Q_ASSERT(headerInstalled);
                Q_UNUSED(headerInstalled);
                ++page->second.revision;
                ++entry.shardOwner->publicationAdditionRecordsTransferred;
                entry.shardOwner->publicationVersionInstalls += publication->installRecords.size();
            } else {
                if (!privateWrite) publication->next.publishedEpoch = epoch;

                for (const auto &delta : std::as_const(publication->deltas)) {
                    const bool updated = entry.shardOwner->records.setPublication(&page->second,
                        delta.version, delta.publication, delta.preparedBy);
                    Q_ASSERT(updated); Q_UNUSED(updated);
                }
                const auto additionRecords = quint64(publication->installRecords.size());
                auto &reservation = arenaGrowth.at(entry.arenaGrowthIndex).reservations;
                const bool additionsInstalled = entry.shardOwner->records.installPreparedAdditionsReserved(
                    &page->second, &publication->installRecords, &reservation);
                Q_ASSERT(additionsInstalled); Q_UNUSED(additionsInstalled);
                if (privateWrite && publication->next.writer.baseAuthority.isValid()) {
                    const auto &writer = publication->next.writer;
                    KisVersionSlotId base;
                    KisReplicaSlotId authority;
                    const bool found = entry.shardOwner->records.findVersion(writer.baseVersion, &base)
                        && entry.shardOwner->records.findReplicaSlot(base, writer.baseAuthority, &authority);
                    Q_ASSERT(found); Q_UNUSED(found);
                    // AcquireWrite checked overflow. The original revision
                    // claim excludes new readers through this single pin edit.
                    ++entry.shardOwner->records.replica(authority)->pinCount;
                }
                entry.shardOwner->publicationAdditionRecordsTransferred += additionRecords;
                const bool headerInstalled = entry.shardOwner->installReservedHeader(
                    entry.key(), &page->second, publication->next);
                Q_ASSERT(headerInstalled); Q_UNUSED(headerInstalled);
                entry.shardOwner->publicationVersionInstalls += additionRecords;
                ++page->second.revision;
                entry.shardOwner->publicationVersionInstalls += quint64(publication->deltas.size());
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
    for (auto &growth : arenaGrowth) {
        QMutexLocker locker(&growth.shardOwner->mutex);
        growth.reservations.cancel(&growth.shardOwner->records);
        growth.releasedBlocks = growth.shardOwner->arenas.takeEmptyBlocks(
            &growth.shardOwner->budgetCharge);
    }
    installed = true;
    owner.reset();
}

// Cold accepted references that must leave after unlocking. Native values
// already own their final storage before pixels; acceptance owns no second
// value allocation, growth group or capacity-repair chain.
class KisPageMetadataCoordinator::PreparedPublication::AcceptanceReferences
{
public:
    using Unit = std::max_align_t;
    using ReleasedReferences = MetadataReleasedReferences;
    using CanonicalReferences = MetadataAcceptanceReferences;
    static AcceptanceReferencesPointer create(const KisMutationStorageAllocator<char> &storage,
                                          size_t pinCount, size_t releaseCount, size_t canonicalCount = 0)
    {
        size_t bytes = sizeof(AcceptanceReferences);
        const auto pinOffset = KisMutationWorkspace::append<std::shared_ptr<MetadataShard>>(bytes, pinCount);
        const auto releaseOffset = KisMutationWorkspace::append<ReleasedReferences>(bytes, releaseCount);
        const auto canonicalOffset = KisMutationWorkspace::append<CanonicalReferences>(bytes, canonicalCount);
        const size_t units = (bytes + sizeof(Unit) - 1) / sizeof(Unit);
        KisMutationStorageAllocator<Unit> allocator(storage);
        auto *raw = allocator.allocate(units);
        auto *base = reinterpret_cast<char *>(raw);
        return AcceptanceReferencesPointer(new (raw) AcceptanceReferences(allocator, units,
            reinterpret_cast<std::shared_ptr<MetadataShard> *>(base + pinOffset),
            reinterpret_cast<ReleasedReferences *>(base + releaseOffset),
            reinterpret_cast<CanonicalReferences *>(base + canonicalOffset)));
    }
    void retainShard(const std::shared_ptr<MetadataShard> &shard)
    {
        new (pins + pinCount++) std::shared_ptr<MetadataShard>(shard);
    }
    ReleasedReferences *appendReleased()
    {
        return new (references + referenceCount++) ReleasedReferences;
    }
    CanonicalReferences *appendCanonical()
    {
        return new (canonicalReferences + canonicalReferenceCount++) CanonicalReferences;
    }
    ~AcceptanceReferences()
    {
        while (referenceCount) references[--referenceCount].~ReleasedReferences();
        while (canonicalReferenceCount) canonicalReferences[--canonicalReferenceCount].~CanonicalReferences();
        while (pinCount) pins[--pinCount].~shared_ptr();
    }
    KisMutationStorageAllocator<Unit> storage;
    size_t units;
    std::shared_ptr<MetadataShard> *pins;
    ReleasedReferences *references;
    CanonicalReferences *canonicalReferences;
    size_t referenceCount = 0;
    size_t canonicalReferenceCount = 0;
    size_t pinCount = 0;
private:
    AcceptanceReferences(const KisMutationStorageAllocator<Unit> &allocator, size_t size,
                     std::shared_ptr<MetadataShard> *owners, ReleasedReferences *released, CanonicalReferences *canonical)
        : storage(allocator), units(size), pins(owners), references(released), canonicalReferences(canonical) {}
};

void KisPageMetadataCoordinator::PreparedPublication::AcceptanceReferencesDeleter::operator()(AcceptanceReferences *value) const noexcept
{
    if (!value) return;
    auto allocator = value->storage;
    const size_t units = value->units;
    value->~AcceptanceReferences();
    allocator.deallocate(reinterpret_cast<AcceptanceReferences::Unit *>(value), units);
}

KisPageMetadataCoordinator::PreparedPublication::PreparedPublication() = default;
KisPageMetadataCoordinator::PreparedPublication::~PreparedPublication() = default;
KisPageMetadataCoordinator::PreparedPublication::PreparedPublication(PreparedPublication &&other) noexcept
    : data(std::move(other.data)), m_acceptanceReferences(std::move(other.m_acceptanceReferences)),
      m_ownedRows(std::move(other.m_ownedRows)), m_rows(std::exchange(other.m_rows, {})),
      m_minimumEpoch(other.m_minimumEpoch),
      m_count(std::exchange(other.m_count, 0)), m_transaction(other.m_transaction),
      m_owner(std::move(other.m_owner)), m_failure(other.m_failure) {}
KisPageMetadataCoordinator::PreparedPublication &
KisPageMetadataCoordinator::PreparedPublication::operator=(PreparedPublication &&other) noexcept
{
    if (this != &other) {
        data = std::move(other.data); m_acceptanceReferences = std::move(other.m_acceptanceReferences);
        m_ownedRows = std::move(other.m_ownedRows);
        m_rows = std::exchange(other.m_rows, {}); m_minimumEpoch = other.m_minimumEpoch;
        m_count = std::exchange(other.m_count, 0); m_transaction = other.m_transaction;
        m_owner = std::move(other.m_owner); m_failure = other.m_failure;
    }
    return *this;
}
bool KisPageMetadataCoordinator::PreparedPublication::isValid() const
{
    return m_failure != Failure::RetentionChanged && (m_owner || (data && data->owner));
}

KisReplicaHandle
KisPageMetadataCoordinator::PreparedPublication::backingAuthority(
    const KisPageVersion &version, bool *detached, KisPageRetirementRecord ***idleResult) const
{
    if (detached) *detached = false;
    if (idleResult) *idleResult = nullptr;
    if (m_minimumEpoch.isValid() && m_rows) {
        const auto found = std::lower_bound(m_rows.begin(), m_rows.end(), version.key,
            [](const auto &row, const auto &key) { return MetadataKeyLess{}(row.replacement.key(), key); });
        if (found != m_rows.end() && found->replacement.key() == version.key && !found->replacement.removal) {
            if (found->supersededAuthority && found->supersededAuthority->version == version) {
                if (detached) *detached = found->detachesSuperseded;
                if (idleResult && found->detachesSuperseded) *idleResult = &found->acceptedIdle;
                return *found->supersededAuthority;
            }
            if (found->replacement.version() == version) {
                if (found->replacement.native) return found->replacement.native->target();
                const auto *original = static_cast<const AcceptanceReferences::CanonicalReferences *>(found->canonicalRecord);
                if (!original) return {};
                // Fees consume this real independent input without entering
                // metadata under the ledger gate. Original slot/stamp/authority
                // are revalidated before installation, then claimed as one D.
                return original->targetAuthority;
            }
        }
    }
    if (!data) return {};
    const auto found = std::lower_bound(data->backingAuthorities.begin(),
        data->backingAuthorities.end(), version, [](const auto &entry, const KisPageVersion &key) {
            return MetadataKeyLess{}(entry.version, key);
        });
    if (found != data->backingAuthorities.end() && found->version == version) {
        return found->replica;
    }
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

KisPageRetirementRecord *KisPageMetadataCoordinator::PreparedPublication::originalBacking(const KisPageVersion &version) const
{
    const auto found = std::lower_bound(m_rows.begin(), m_rows.end(), version.key,
        [](const auto &row, const auto &key) { return MetadataKeyLess{}(row.replacement.key(), key); });
    return found != m_rows.end() && found->replacement.native && found->replacement.version() == version
        ? found->replacement.native->backing : nullptr;
}

bool KisPageMetadataCoordinator::PreparedPublication::genericInputsChanged() const
{
    if (!data) return false;
    for (size_t first = 0; first < data->entries.size();) {
        const size_t end = data->batchEnd(first, data->entries.size());
        QMutexLocker lock(&data->entries[first].shardOwner->mutex);
        for (size_t i = first; i < end; ++i) {
            const auto &entry = data->entries[i];
            if (entry.originalPage->second.revision != entry.revision
                || !entry.shardOwner->canMutate(entry.key(), entry.originalPage->second)) return true;
        }
        first = end;
    }
    return false;
}

bool KisPageMetadataCoordinator::PreparedPublication::hasDetachedBacking() const
{
    return m_minimumEpoch.isValid() && std::any_of(m_rows.begin(), m_rows.end(),
        [](const auto &row) { return row.detachesSuperseded; });
}

void KisPageMetadataCoordinator::PreparedPublication::forEachDetachedBacking(void *context,
    void (*visit)(void *, const KisReplicaHandle &, KisPageRetirementRecord *)) const
{
    if (!m_minimumEpoch.isValid()) return;
    for (const auto &row : m_rows)
        if (row.detachesSuperseded && row.supersededAuthority && row.supersededAuthority->isValid())
            visit(context, *row.supersededAuthority, row.acceptedIdle);
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
KisPageMetadataCoordinator::prepareAcceptance(const KisPageTransaction &transaction,
                                            KisPagePublicationRows rows,
                                            QString *error, KisImageEpochId minimumEpoch, bool detachCurrent,
                                            const PublicationChange *generic, qsizetype genericCount) const
try {
    PreparedPublication result;
    auto *d = m_core.load(std::memory_order_acquire);
    const size_t count = rows.size();
    const bool published = minimumEpoch.isValid();
    result.m_minimumEpoch = minimumEpoch;
    if (!d || d->shards.empty() || !transaction.isValid() || (count && !rows)
        || (published && minimumEpoch.value <= transaction.baseEpoch.value)) return result;
    if (genericCount) {
        auto genericPreparation = preparePublicationImpl(transaction, minimumEpoch, nullptr, genericCount,
            false, error, PreparationKind::Publication, nullptr, generic);
        if (!genericPreparation.isValid()) return genericPreparation;
        result.data = std::move(genericPreparation.data);
    }
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
        if (published) {
            input.superseded = {}; input.supersededAuthority = nullptr;
            input.acceptedIdle = nullptr; input.canonicalRecord = nullptr;
            if (!(input.replacement.key() == input.replacement.version().key) && !input.replacement.removal) return {};
        }
        input.metadataAcceptance = {};
        if (!input.replacement.key().isValid()
            || (input.superseded.version.key.isValid() && !input.superseded.isValid())) return {};
        if (published ? input.replacement.removal : !input.hasMutation()) continue;
        auto *native = input.replacement.native;
        if (native && (!native->reserved || !native->completed || !native->isSealed()
            || !(native->sealedBy() == transaction.id))) return {};
        ++mutations;
        auto &work = input.metadataAcceptance;
        work.input = &input;
        work.shardIndex = native ? native->location.shardIndex
            : kisStablePageKeyHash(input.mutationVersion().key) % d->shards.size();
        work.shard = native ? Private::registeredShard(d, *native) : d->shards[work.shardIndex].get();
        if (!work.shard) return {};
        ordered &= i + 1 == mutations && (!previous || !before(work, *previous));
        previous = &work;
        if (published) {
            input.detachesSuperseded = false;
            releases += !native;
        } else if (native) {
            input.detachesSuperseded = input.detachesSuperseded
                && input.superseded.native && input.expectedAuthority()
                && !(input.mutationVersion() == native->version());
        } else {
            releases += input.detachesSuperseded;
        }
    }
    using It = KisPublicationMemberIterator<KisPagePublicationChange::MetadataAcceptance,
                                           &KisPagePublicationChange::metadataAcceptance>;
    if (!ordered) std::sort(It(rows.begin()), It(rows.end()), before);
    if (published) {
        for (size_t first = 0; first < mutations;) {
            auto *shard = static_cast<MetadataShard *>(rows[first].metadataAcceptance.shard);
            size_t end = first + 1;
            while (end < mutations && end - first < 16
                && rows[end].metadataAcceptance.shard == shard) ++end;
            QMutexLocker lock(&shard->mutex);
            for (size_t i = first; i < end; ++i) {
                auto &work = rows[i].metadataAcceptance;
                auto *native = work.input->replacement.native;
                MetadataPage *page = nullptr;
                if (native) page = static_cast<MetadataPage *>(native->location.page);
                else {
                    const auto found = shard->pages.find(work.input->replacement.key());
                    if (found != shard->pages.end()) page = &found->second;
                    work.replacementSlot = work.input->replacement.metadataSlot();
                    auto *target = shard->records.version(work.replacementSlot);
                    if (!target || !(target->version == work.input->replacement.version())) {
                        if (!shard->records.findVersion(work.input->replacement.version(), &work.replacementSlot)) return {};
                    }
                }
                work.page = page;
                if (!page || !shard->canMutate(work.input->replacement.key(), *page)
                    || !shard->validPublication(*page, *work.input, work, transaction.id, minimumEpoch)) return {};
                const auto *current = shard->records.version(page->publishedVersion); Q_ASSERT(current);
                if (native) {
                    ++shard->preparedPublicationPages;
                    // A writer may be based on an earlier Prepared result. Its
                    // source is then distinct from the old committed authority.
                    // Retain only that genuinely independent canonical output.
                    const bool distinct = native->source.isValid() && !(native->source.version == current->version);
                    work.input->supersededAuthority = distinct ? nullptr : &native->source;
                    releases += distinct;
                }
                work.input->detachesSuperseded = detachCurrent && shard->records.canDetachPublished(current->version);
            }
            first = end;
        }
    }
    size_t retainedShards = 0;
    for (size_t first = 0; first < mutations;) {
        auto &head = rows[first].metadataAcceptance;
        size_t end = first + 1;
        while (end < mutations && rows[end].metadataAcceptance.shard == head.shard) ++end;
        bool external = false;
        for (size_t i = first; i < end; ++i) {
            const auto &input = *rows[i].metadataAcceptance.input;
            if (i > first && rows[i - 1].metadataAcceptance.input->mutationVersion().key == input.mutationVersion().key)
                return {};
            external |= !input.replacement.native || (published && !input.supersededAuthority);
        }
        if (external) ++retainedShards;
        first = end;
    }
    auto rollbackPreparationCounts = qScopeGuard([&] {
        for (size_t first = 0; first < mutations;) {
            auto *shard = static_cast<MetadataShard *>(rows[first].metadataAcceptance.shard);
            size_t end = first + 1;
            while (end < mutations && rows[end].metadataAcceptance.shard == shard) ++end;
            const auto externalPrepared = [&](size_t i) {
                const auto &work = rows[i].metadataAcceptance;
                return !published && !work.input->replacement.native && work.page;
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
    if (retainedShards) {
        result.m_acceptanceReferences = PreparedPublication::AcceptanceReferences::create(
            d->budgetAuthority->storage<char>(), retainedShards, published ? 0 : releases, published ? releases : 0);
        for (size_t first = 0; first < mutations;) {
            const auto &head = rows[first].metadataAcceptance;
            size_t end = first + 1;
            while (end < mutations && rows[end].metadataAcceptance.shard == head.shard) ++end;
            const auto &shard = d->shards[head.shardIndex];
            bool retained = false;
            // External directory nodes live until their retained shard dies.
            // Bind each original position once, in bounded preparation gates.
            for (size_t batch = first; batch < end;) {
                while (batch < end && rows[batch].metadataAcceptance.input->replacement.native
                    && (!published || rows[batch].metadataAcceptance.input->supersededAuthority)) ++batch;
                if (batch == end) break;
                const size_t batchEnd = std::min(end, batch + 16);
                QMutexLocker lock(&shard->mutex);
                for (size_t i = batch; i < batchEnd; ++i) {
                    auto &work = rows[i].metadataAcceptance;
                    auto &input = *work.input;
                    if (input.replacement.native && (!published || input.supersededAuthority)) continue;
                    if (!retained) {
                        result.m_acceptanceReferences->retainShard(shard);
                        retained = true;
                    }
                    MetadataPage *page = published ? static_cast<MetadataPage *>(work.page) : nullptr;
                    if (!published) {
                        const auto found = shard->pages.find(input.mutationVersion().key);
                        if (found != shard->pages.end()) page = &found->second;
                    }
                    if (!page || !shard->canMutate(input.mutationVersion().key, *page)
                        || (published ? !shard->validPublication(*page, input, work, transaction.id, minimumEpoch)
                            : !shard->validDetachment(*page, input.mutationVersion(), nullptr,
                                transaction.id, input.superseded.native, input.expectedAuthority(), input.superseded.metadataSlot))) return {};
                    if (!published) input.detachesSuperseded = input.detachesSuperseded
                        && shard->records.canDetachPrepared(input.mutationVersion());
                    if (published || input.detachesSuperseded) {
                        if (published) {
                            auto *references = result.m_acceptanceReferences->appendCanonical();
                            if (!input.replacement.native)
                                work.prepared = static_cast<MetadataReleasedReferences *>(references);
                            const auto *current = shard->records.version(page->publishedVersion); Q_ASSERT(current);
                            references->source = shard->records.projectReplica(current->authorityReplica);
                            if (!input.replacement.native) {
                                const auto *target = shard->records.version(work.replacementSlot); Q_ASSERT(target);
                                references->targetAuthority = shard->records.projectReplica(target->authorityReplica);
                            }
                            input.supersededAuthority = &references->source;
                            input.canonicalRecord = references;
                        } else work.prepared = result.m_acceptanceReferences->appendReleased();
                    }
                    work.page = page;
                    ++(published ? shard->preparedPublicationPages : shard->preparedMutationPages);
                    if (!published) ++shard->publicationHistoryNodesPrepared;
                }
                batch = batchEnd;
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

KisPageMetadataCoordinator::PreparedPublication KisPageMetadataCoordinator::prepareAcceptance(
    const KisPageTransaction &transaction, KisPagePublicationChanges &&owned, QString *error,
    KisImageEpochId epoch, bool detachCurrent, const PublicationChange *generic, qsizetype genericCount) const
{
    auto result = prepareAcceptance(transaction, KisPagePublicationRows{owned.data(), owned.size()},
                                    error, epoch, detachCurrent, generic, genericCount);
    if (result.isValid()) result.m_ownedRows.emplace(std::move(owned));
    return result;
}

bool KisPageMetadataCoordinator::revalidateAcceptance(PreparedPublication &prepared, QString *error) const
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
    if (!prepared.needsAcceptanceRevalidation() || !d || prepared.m_owner != d->publicationOwner)
        return fail(PreparedPublication::Failure::Conflict, QStringLiteral("mutation preparation is stale or not awaiting retention revalidation"));
    prepared.m_failure = PreparedPublication::Failure::None;
    KisPageStoreDetail::setError(error, {});
    return true;
}
catch (const std::bad_alloc &) {
    prepared.m_owner.reset();
    prepared.m_rows = {};
    prepared.m_count = 0;
    prepared.m_failure = PreparedPublication::Failure::Storage;
    KisPageStoreDetail::setError(error, QStringLiteral("mutation retention revalidation was refused"));
    return false;
}


bool KisPageMetadataCoordinator::installAcceptance(PreparedPublication &&prepared,
    const KisPageTransaction &transaction, QString *error, KisImageEpochId epoch,
    DeferredPublicationCleanup *deferredCleanup, PreparationKind kind)
{
    // The original D is the sole mutation capability. Root/lease candidates
    // cannot be consumed as a mutation, even when their identities match.
    const auto rows = prepared.m_rows;
    const size_t count = std::exchange(prepared.m_count, 0);
    auto owner = std::move(prepared.m_owner);
    auto data = std::move(prepared.data);
    const bool mutation = kind != PreparationKind::Publication;
    const bool published = prepared.m_minimumEpoch.isValid() || (data && data->minimumEpoch.isValid());
    const bool canDeferCleanup = deferredCleanup && deferredCleanup->isEmpty();
    const auto transferCleanup = qScopeGuard([&] {
        if (canDeferCleanup && data) deferredCleanup->data = std::move(data);
    });
    auto *d = m_core.load(std::memory_order_acquire);
    if ((count && !rows) || !d || (!owner && !data)
        || (owner && (owner != d->publicationOwner || !(prepared.m_transaction == transaction)))
        || epoch.isValid() != published || (prepared.m_minimumEpoch.isValid() && epoch.value < prepared.m_minimumEpoch.value)
        || prepared.m_failure == PreparedPublication::Failure::RetentionChanged
        || (deferredCleanup && !canDeferCleanup)
        || (data && (data->owner != d->publicationOwner || data->kind != kind
            || data->minimumEpoch.isValid() == mutation || !(data->transaction == transaction)
            || (published && epoch.value < data->minimumEpoch.value)))) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata acceptance capability is stale or foreign"));
        return false;
    }
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
    const bool singleWindow = count && !data && batchEnd(0, count) == count;
    std::optional<QMutexLocker<QMutex>> shardLock;
    size_t claimed = 0;
    bool retentionChanged = false;
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
            if (published ? !shard->validPublication(*page, input, work, transaction.id, epoch)
                : !shard->validDetachment(*page, version, native, transaction.id,
                    input.superseded.native, input.expectedAuthority(), input.superseded.metadataSlot)) break;
            const auto *current = published ? shard->records.version(page->publishedVersion) : nullptr;
            const auto &sourceVersion = published ? current->version : version;
            const bool available = !input.detachesSuperseded
                || (published ? shard->records.canDetachPublished(sourceVersion)
                              : shard->records.canDetachPrepared(sourceVersion));
            if (!available) {
                if (!retentionChanged) ++shard->nativeRetentionRevalidations;
                retentionChanged = true;
                input.detachesSuperseded = false;
            }
            work.page = page;
            if (!singleWindow) page->activity.publicationClaim = &rows[0];
            ++claimed;
        }
        if (!singleWindow) shardLock.reset();
        if (claimed != end) break;
        first = end;
    }
    if (claimed != count || retentionChanged) {
        for (size_t first = 0; first < claimed;) {
            const size_t end = batchEnd(first, claimed);
            auto *shard = static_cast<MetadataShard *>(rows[first].metadataAcceptance.shard);
            if (!shardLock) shardLock.emplace(&shard->mutex);
            for (size_t i = first; i < end; ++i) {
                auto &work = rows[i].metadataAcceptance;
                if (!singleWindow) static_cast<MetadataPage *>(work.page)->activity.publicationClaim = nullptr;
                if (work.input->replacement.native) work.replacementSlot = {};
            }
            shardLock.reset();
            first = end;
        }
        shardLock.reset(); // A rejection before the first claim may still hold the gate.
        if (count) {
            auto *shard = static_cast<MetadataShard *>(rows[0].metadataAcceptance.shard);
            QMutexLocker lock(&shard->mutex);
            ++(published ? shard->rejectedPublicationInstalls : shard->rejectedMutationInstalls);
        }
        const bool retry = claimed == count && retentionChanged;
        if (retry) {
            prepared.m_rows = rows;
            prepared.m_count = count;
            prepared.m_owner = std::move(owner);
            prepared.data = std::move(data);
            prepared.m_failure = PreparedPublication::Failure::RetentionChanged;
        }
        KisPageStoreDetail::setError(error, QStringLiteral("mutation acceptance lost identity or source retention changed"));
        return false;
    }
    if (data) {
        const size_t genericClaimed = data->claimRows();
        if (genericClaimed != data->entries.size()) {
            data->releaseClaims(genericClaimed);
            for (size_t first = 0; first < count;) {
                const size_t end = batchEnd(first, count);
                auto *shard = static_cast<MetadataShard *>(rows[first].metadataAcceptance.shard);
                QMutexLocker gate(&shard->mutex);
                for (size_t i = first; i < end; ++i)
                    static_cast<MetadataPage *>(rows[i].metadataAcceptance.page)->activity.publicationClaim = nullptr;
                first = end;
            }
            if (!data->entries.empty()) {
                QMutexLocker gate(&data->entries.front().shardOwner->mutex);
                ++(mutation ? data->entries.front().shardOwner->rejectedMutationInstalls
                            : data->entries.front().shardOwner->rejectedPublicationInstalls);
            }
            prepared.m_failure = PreparedPublication::Failure::Conflict;
            KisPageStoreDetail::setError(error, QStringLiteral("generic output changed before complete acceptance"));
            return false;
        }
        // Both subsets are now claimed. No fallible work remains before all
        // original facts and genuine header outputs reach their one accept cut.
        data->installRows(mutation, kind == PreparationKind::RecoverableWrite,
            kind == PreparationKind::PrivateWrite, epoch);
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
            auto *released = published && input.canonicalRecord
                ? static_cast<MetadataReleasedReferences *>(static_cast<MetadataAcceptanceReferences *>(input.canonicalRecord))
                : native ? nullptr : static_cast<MetadataReleasedReferences *>(work.prepared);
            auto *releasedDescriptor = released ? &released->descriptor : &oldDescriptor;
            auto *releasedBinding = released ? &released->binding : &oldBinding;
            if (published) shard->installPublished(*page, input, work, transaction.id, epoch,
                releasedDescriptor, releasedBinding);
            else shard->installDetachment(*page, version, native, input.detachesSuperseded,
                releasedDescriptor, releasedBinding, input.superseded.metadataSlot);
            if (native) {
                native->releasedDescriptor.swap(oldDescriptor);
                native->releasedBinding.swap(oldBinding);
            }
            if (!singleWindow) page->activity.publicationClaim = nullptr;
            // A pre-existing empty generic payload has a real final free, but
            // requires no native preparation/activity/owned Entry to retain it.
            work.page = page->activity.isEmpty()
                && shard->activities.find(version.key) != shard->activities.end() ? page : nullptr;
            work.replacementSlot = {};
        }
        shardLock.reset();
        first = end;
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

void KisPageMetadataCoordinator::finishAcceptance(KisPagePublicationRows rows)
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
    return installAcceptance(std::move(prepared), transaction, error, {}, deferredCleanup,
                             PreparationKind::RecoverableWrite);
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
    return installAcceptance(std::move(prepared), transaction, error, {}, deferredCleanup,
                             PreparationKind::PrivateWrite);
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
    if (!mutation && !restoration) {
        const auto kindAt = [&](qsizetype i) { return changes ? changes[i].kind : transitions[i].kind; };
        size_t commits = 0;
        for (qsizetype i = 0; i < count; ++i) commits += kindAt(i) == KisPageTransitionKind::CommitTransaction;
        if (commits) {
            KisPagePublicationChanges rows{d->budgetAuthority->storage<KisPagePublicationChange>()};
            std::vector<PublicationChange, KisMutationStorageAllocator<PublicationChange>> sparse{
                d->budgetAuthority->storage<PublicationChange>()};
            rows.reserve(commits); sparse.reserve(size_t(count) - commits);
            for (qsizetype i = 0; i < count; ++i) {
                const auto &version = changes ? changes[i].version : transitions[i].version;
                if (kindAt(i) == KisPageTransitionKind::CommitTransaction) {
                    if (!changes && (!(transitions[i].transaction == transaction.id)
                        || !(transitions[i].imageEpoch == minimumEpoch))) return {};
                    auto *native = nativeWrite(version);
                    if (native) rows.emplace_back(KisPagePublicationChange::Replacement{native});
                    else rows.emplace_back(KisPagePublicationChange::Replacement{version.key,
                        KisPagePublicationIdentity{version, transaction.id, 0, false}});
                } else sparse.push_back({kindAt(i), version, changes ? changes[i].target : transitions[i].target});
            }
            std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
                return MetadataKeyLess{}(a.replacement.key(), b.replacement.key());
            });
            return prepareAcceptance(transaction, std::move(rows), error, minimumEpoch,
                detachUnprotectedCurrent, sparse.data(), qsizetype(sparse.size()));
        }
    }
    const size_t mutationCount = size_t(count);
    const auto storageOwner = d->budgetAuthority->storage<char>();
    const size_t bytesPerPage = sizeof(PreparedPublication::Data::Entry)
        + sizeof(PreparedPublication::Data::Publication);
    if (mutationCount > size_t(std::numeric_limits<qint64>::max()) / bytesPerPage)
        throw std::bad_alloc();
    size_t shardCount = 0;
    if (mutationCount) {
        if (count == 1) { shardCount = 1; }
        else {
            // configure() bounds shards at4096. This stack capacity census
            // supplies no page authority; the existing preparation below
            // still validates and retains each exact shard/page identity.
            std::bitset<4096> usedShards;
            for (qsizetype i = 0; i < count; ++i) {
                const auto &key = versions ? versions[i].key
                    : changes ? changes[i].version.key : transitions[i].version.key;
                usedShards.set(kisStablePageKeyHash(key) % d->shards.size());
            }
            shardCount = usedShards.count();
        }
    }
    auto data = PreparedPublication::Data::create(storageOwner, mutationCount, shardCount);
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
                const auto page = shard->pages.find(transition.version.key);
                if (page == shard->pages.cend() || !page->second.revisionCanAdvance()
                    || page->second.activity.publicationClaim) {
                    KisPageStoreDetail::setError(error, QStringLiteral("metadata publication page is unavailable"));
                    return result;
                }
                entry.originalPage = page;
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
                if (page->second.firstMutable.isValid()) {
                    const KisPageValueCell *mutableRecord = shard->records.version(page->second.firstMutable);
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
    for (size_t i = 0; i < data->entries.size(); ++i) {
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
            if (copiedAuthority(version)) data->backingAuthorities.push_back(
                {version.version, version.authority});
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
            if (step.next.versions.size() > inputCount) {
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
            auto demand = shard->records.batchDemand(publication.installRecords);
            recordPolicyWork(i);
            ++recordedPolicies;
            qsizetype physicalInsertions = 0;
            for (const auto &record : publication.installRecords) physicalInsertions += record.replicas.size();
            data->addArenaDemand(i, demand,
                qsizetype(publication.installRecords.size()) - (recoverable ? 1 : 0), physicalInsertions);
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
                || entry.shardOwner->ensureActivityCapacity(entry.key(), &data->publications[i].next, error);
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
    return installAcceptance(std::move(prepared), transaction, error, epoch, deferredCleanup);
}


KisPageMetadataCoordinator::KisPageMetadataCoordinator() = default;

KisPageMetadataCoordinator::~KisPageMetadataCoordinator()
{
    if (auto *core = m_core.exchange(nullptr, std::memory_order_acq_rel)) {
        // Raw facade calls already require quiescence. Escaped immutable roots
        // retain memory only, so detach the live budget while it still exists.
        core->attached.store(false, std::memory_order_release);
        if (m_coreMemory.use_count() > 1) core->detachLiveValues();
        core->budgetAuthority->detach();
    }
    m_coreMemory.reset();
}

std::shared_ptr<KisPageViewValueStorage> KisPageMetadataCoordinator::viewValues() const
{
    QMutexLocker lock(&m_configurationMutex);
    return m_coreMemory;
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
    auto *version = readableVersion(*shard, replica.version);
    KisReplicaSlotId slot;
    if (!version || !shard->records.findReplicaSlot(version->slot, replica, &slot)
        || !cpuReadableReplica(*shard->records.replica(slot), replica.version))
        return {};
    auto *facts = &version->native;
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
KisReplicaHandle cpuReadReplicaLocked(const MetadataShard &shard, const KisPageValueCell &version)
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

KisCpuReadBinding cachedCpuReadBindingLocked(const MetadataShard &shard, const KisPageValueCell &version)
{
    const auto *facts = &version.native;
    const auto *replica = shard.records.replica(facts->nativeReplica);
    return replica && cpuReadableReplica(*replica, version.version)
        ? KisCpuReadBinding{replicaHandle(*replica, version.version), facts->nativeBinding} : KisCpuReadBinding{};
}

KisCpuReadBinding selectCpuReadBindingLocked(const MetadataShard &shard, const KisPageValueCell &version)
{
    auto selected = cachedCpuReadBindingLocked(shard, version);
    // Keep one return carrier, including the cold case. An empty binding is
    // still a completed exact replica selection; it must not lose its source.
    if (!selected.binding) selected.replica = cpuReadReplicaLocked(shard, version);
    return selected;
}

void takeCpuReadBindingLocked(MetadataShard &shard, KisPageValueCell &version,
    const KisReplicaHandle &replica, const KisCpuResidentBinding *expected,
    std::shared_ptr<KisCpuResidentBinding> *released)
{
    auto *facts = &version.native;
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
    auto *record = shard->records.version(slot);
    if (!record) return {};
    if (stale) takeCpuReadBindingLocked(*shard, *record, stale.replica, stale.binding.get(), &released);
    return readableVersionLocked(*record)
        ? selectCpuReadBindingLocked(*shard, *record) : KisCpuReadBinding{};
}

size_t KisPageMetadataCoordinator::retainReadValues(KisPageViewValue *const *values,
    const quint32 *order, size_t &next, size_t count, KisCpuReadBinding *firstBindings, size_t bindingCount)
{
    auto *core = m_core.load(std::memory_order_acquire);
    if (!core || !count) return 0;
    Q_ASSERT(values && order && next != std::numeric_limits<quint32>::max());
    const auto shardOf = [&](const KisPageViewValue *value) {
        Q_ASSERT(value);
        return kisStablePageKeyHash(value->version.key) % core->shards.size();
    };
    const auto shardIndex = shardOf(values[next]);
    auto *shard = core->shards[shardIndex].get();
    QMutexLocker lock(&shard->mutex);
    size_t retained = 0;
    for (; retained < count && retained < 16; ++retained) {
        auto *value = static_cast<KisPageValueCell *>(values[next]);
        if (shardOf(value) != shardIndex) break;
        Q_ASSERT(shard->arenas.versions.get(value->slot) == value);
        if (value->readEdges == std::numeric_limits<quint32>::max()) break;
        if (!value->version.isDefaultPixel()) {
            auto page = shard->pages.find(value->version.key);
            if (page == shard->pages.end() || !shard->canMutate(value->version.key, page->second)
                || value->publication == KisPagePublicationState::Unpublished
                || value->publication == KisPagePublicationState::Retiring) break;
            ++page->second.revision;
            if (next < bindingCount) {
                Q_ASSERT(firstBindings && !firstBindings[next]);
                firstBindings[next] = selectCpuReadBindingLocked(*shard, *value);
            }
        }
        // An unindexed default still needs its identity, but never pins
        // optional materialized pixels. No second memory retain is needed.
        if (!value->readEdges && !value->indexed && !value->frameHeld) value->attachLifetime();
        ++value->readEdges;
        next = order[next];
    }
    return retained;
}

size_t KisPageMetadataCoordinator::releaseReadValues(KisPageViewValue **values,
    const quint32 *order, size_t &next, size_t count, KisPageKey *revisit)
{
    auto *core = m_core.load(std::memory_order_acquire);
    if (!count) return 0;
    Q_ASSERT(core && values && order && revisit && next != std::numeric_limits<quint32>::max());
    const auto shardOf = [&](const KisPageViewValue *value) {
        Q_ASSERT(value);
        return kisStablePageKeyHash(value->version.key) % core->shards.size();
    };
    const auto shardIndex = shardOf(values[next]);
    auto *shard = core->shards[shardIndex].get();
    MetadataReleasedReferences releasedReferences;
    QMutexLocker lock(&shard->mutex);
    size_t released = 0, limit = std::min(count, size_t(16));
    for (; released < limit; ++released) {
        auto *value = static_cast<KisPageValueCell *>(values[next]);
        if (shardOf(value) != shardIndex) break;
        revisit[released] = {};
        Q_ASSERT(value->readEdges && shard->arenas.versions.get(value->slot) == value);
        if (!--value->readEdges) {
            const auto page = shard->pages.find(value->version.key);
            if (page != shard->pages.end() && page->second.firstHistory.isValid())
                revisit[released] = value->version.key;
            if (!value->indexed && !value->frameHeld && value->detachLifetime()) {
                if (value->initialIdentity && page != shard->pages.end() && !page->second.hasMetadata()
                    && page->second.publishedVersion == value->slot) page->second.publishedVersion = {};
                releasedReferences.descriptor.swap(value->native.descriptor);
                releasedReferences.binding.swap(value->native.nativeBinding);
                const bool erased = shard->arenas.versions.erase(value->slot);
                Q_ASSERT(erased); Q_UNUSED(erased);
                // Finish this bounded batch before destroying the one
                // detached payload outside the gate. Ordinary edge returns
                // do not initialize an array of shared-reference holders.
                limit = released + 1;
            }
        }
        // A monotone return is never blocked by a publication claim. Existing
        // candidate snapshots can only be conservative; they cannot write the
        // original read count back. Clear this consumed fact before continuing.
        values[next] = nullptr;
        next = order[next];
    }
    return released;
}

KisCpuReadBinding KisPageMetadataCoordinator::readValueBinding(const KisPageViewValue *value) const
{
    if (!value || value->version.isDefaultPixel()) return {};
    const auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), value->version.key);
    Q_ASSERT(shard);
    QMutexLocker lock(&shard->mutex);
    Q_ASSERT(value->readEdges || value->rootEdges.load(std::memory_order_acquire));
    return selectCpuReadBindingLocked(*shard, *static_cast<const KisPageValueCell *>(value));
}

void KisPageMetadataCoordinator::readValueBindings(KisPageViewValue *const *values,
    quint32 *indices, size_t first, size_t count, KisCpuReadBinding *bindings) const
{
    Q_ASSERT(count <= 16);
    const auto *core = m_core.load(std::memory_order_acquire);
    Q_ASSERT(core);
    quint32 shards[16];
    size_t selected = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto *value = values[first + i];
        if (!value || value->version.isDefaultPixel()) continue;
        indices[selected++] = quint32(first + i);
        shards[i] = quint32(kisStablePageKeyHash(value->version.key) % core->shards.size());
    }
    const auto selectedShard = [&](quint32 i) { return shards[i - first]; };
    std::sort(indices, indices + selected, [&](quint32 a, quint32 b) { return selectedShard(a) < selectedShard(b); });
    for (size_t i = 0; i < selected;) {
        const auto shardIndex = selectedShard(indices[i]);
        const auto *shard = core->shards[shardIndex].get();
        QMutexLocker lock(&shard->mutex);
        do {
            const auto index = indices[i++];
            Q_ASSERT(!bindings[index - first]);
            const auto *value = static_cast<const KisPageValueCell *>(values[index]);
            Q_ASSERT(value->readEdges);
            bindings[index - first] = selectCpuReadBindingLocked(*shard, *value);
        } while (i < selected && selectedShard(indices[i]) == shardIndex);
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
        auto candidate = std::allocate_shared<Private>(storage, std::move(standalone), storage);
        candidate->budgetAuthority = std::allocate_shared<MetadataBudgetAuthority>(storage, budget);
        candidate->shards.reserve(size_t(shardCount));
        for (qsizetype i = 0; i < shardCount; ++i) {
            candidate->shards.push_back(std::allocate_shared<MetadataShard>(
                storage, candidate->budgetAuthority, storage));
        }
        candidate->publicationOwner = std::allocate_shared<const quint8>(storage, 0);
        // Publish the complete original core only after every allocation has
        // succeeded. Refusal destroys the same candidate, including its body.
        m_coreMemory = std::move(candidate);
        m_core.store(m_coreMemory.get(), std::memory_order_release);
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

KisPageWriteClaim *KisPageMetadataCoordinator::findWriteClaim(const KisPageKey &key) const
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), key);
    if (!shard) return nullptr;
    QMutexLocker lock(&shard->mutex);
    const auto found = shard->pages.find(key);
    return found == shard->pages.end() ? nullptr : &found->second.writeClaim;
}

KisPageWriteClaim *KisPageMetadataCoordinator::prepareWriteClaim(const KisPageKey &key, QString *error) try
{
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), key);
    if (!shard) {
        KisPageStoreDetail::setError(error, QStringLiteral("page control is unavailable"));
        return nullptr;
    }
    {
        QMutexLocker lock(&shard->mutex);
        const auto found = shard->pages.find(key);
        if (found != shard->pages.end()) return &found->second.writeClaim;
    }
    // Allocate/charge outside both Store and shard gates. A competing creator
    // can win; only the stable original node is installed, and unused storage
    // is destroyed after the shard gate is released.
    decltype(shard->pages) prepared(shard->pages.get_allocator());
    prepared.try_emplace(key);
    auto node = prepared.extract(prepared.begin());
    QMutexLocker lock(&shard->mutex);
    auto result = shard->pages.insert(std::move(node));
    auto *claim = &result.position->second.writeClaim;
    lock.unlock();
    return claim;
} catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("page control storage budget was refused"));
    return nullptr;
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
    const auto storageOwner = shard->budgetAuthority->storage<char>();
    MetadataArenaGrowth growth(storageOwner.borrowed());
    decltype(shard->pages) prepared(shard->pages.get_allocator());
    QMutexLocker locker(&shard->mutex);
    const auto registered = [&] {
        const auto found = shard->pages.find(initial.key);
        return found != shard->pages.end() && found->second.hasMetadata();
    };
    if (registered()) {
        KisPageStoreDetail::setError(error, QStringLiteral("page is already registered"));
        return false;
    }
    auto found = shard->pages.find(initial.key);
    if (found == shard->pages.end()) {
        locker.unlock();
        prepared.try_emplace(initial.key);
        locker.relock();
        found = shard->pages.find(initial.key);
        if (found != shard->pages.end() && found->second.hasMetadata()) {
            KisPageStoreDetail::setError(error, QStringLiteral("page was registered while preparing page control"));
            locker.unlock();
            return false;
        }
    }
    const auto demand = shard->records.initialDemand(found == shard->pages.end() ? nullptr : &found->second, initial.versions);
    const auto storage = growMetadataArenasOutsideLock(shard, demand, &growth, &locker, [&] {
        return !registered();
    }, error);
    if (storage != MetadataGrowthResult::Ready) {
        KisPageStoreDetail::setError(error,
                 storage == MetadataGrowthResult::Stale
                     ? QStringLiteral("page was registered while preparing metadata storage")
                     : QStringLiteral("metadata arena budget is exhausted"));
        return false;
    }
    found = shard->pages.find(initial.key);
    if (!shard->ensureActivityCapacity(initial.key, &initial, error)) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata owning capacity budget is exhausted"));
        return false;
    }
    const auto discardActivity = qScopeGuard([&] {
        const auto activity = shard->activities.find(initial.key);
        if (activity != shard->activities.end() && activity->second.isEmpty()) shard->activities.erase(activity);
    });
    auto &page = found == shard->pages.end() ? prepared.begin()->second : found->second;
    if (!shard->assign(initial.key, &page, initial)) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata arena budget is exhausted"));
        return false;
    }
    if (found == shard->pages.end()) shard->pages.insert(prepared.extract(prepared.begin()));
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
    if (pageIt == shard->pages.cend() || !pageIt->second.hasMetadata())
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
    if (page == shard->pages.cend() || !page->second.hasMetadata()) {
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
    const auto *facts = &record->native;
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
        if (page == shard->pages.cend() || !page->second.hasMetadata()) return false;
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
            if (result && result->completed && result->version() == identity) {
                if (replicas && replicas->capacity() < 1) {
                    lock.unlock(); replicas->reserve(1); continue;
                }
                next.version = identity;
                next.authority = result->target();
                next.publication = KisPagePublicationState::Prepared;
                next.preparedBy = page->second.activity.writerTransaction;
                next.replicaCount = 1;
                if (replicas) { replicas->clear(); replicas->push_back({result->target(), KisReplicaValidity::Valid}); }
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
        return &shard->records.version(slot)->native.descriptor;
    const auto *activity = shard->activity(identity.key);
    auto *native = activity ? activity->nativeWrite : nullptr;
    return native && native->version() == identity ? &native->descriptor() : nullptr;
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

KisPageEditValues::~KisPageEditValues()
{
    Q_ASSERT(!m_values);
}

KisPageEditValues::KisPageEditValues(KisPageEditValues &&other) noexcept
{
    *this = std::move(other);
}

KisPageEditValues &KisPageEditValues::operator=(KisPageEditValues &&other) noexcept
{
    if (this == &other) return *this;
    Q_ASSERT(!m_values && !other.m_values);
    // Only empty operation capacity moves; original live rows never move to
    // another edit. Preserve the allocator that funded any spilled storage.
    m_shards.reset();
    if (other.m_shards) m_shards.emplace(std::move(*other.m_shards));
    other.m_shards.reset();
    return *this;
}

size_t KisPageEditValues::storageBytes() const noexcept
{
    return m_shards && m_shards->capacity() > Holds::internal_capacity()
        ? kisPageStorageBytes(m_shards->data(), alignof(Hold)) : 0;
}

void KisPageEditValues::release(KisPagePrivateWrite **rows, size_t count) noexcept
{
    Q_ASSERT(count <= ReleaseBatch);
    size_t live = 0;
    for (size_t i = 0; i < count; ++i) {
        Q_ASSERT(rows[i] && &rows[i]->edit == this && !rows[i]->reserved);
        if (rows[i]->value) rows[live++] = rows[i];
    }
    if (live) {
        // Sort bounded borrowed positions only. Neither Values nor their
        // original rows move, and cleanup allocates no second carrier.
        std::sort(rows, rows + live, [](const auto *a, const auto *b) {
            return std::less<void *>{}(a->location.shard, b->location.shard);
        });
        const auto release = [&](MetadataReleasedReferences *released) {
            for (size_t first = 0; first < live;) {
                auto *shard = static_cast<MetadataShard *>(rows[first]->location.shard);
                QMutexLocker lock(&shard->mutex);
                size_t end = first;
                do {
                    auto &row = *rows[end];
                    auto *value = row.value;
                    Q_ASSERT(value->frameHeld);
                    shard->records.exactVersions.cancelReservation(&row.location.exact);
                    shard->records.physicalSlots.cancelReservation(&row.location.physical);
                    value->frameHeld = false;
                    if (!value->indexed) {
                        released[end].descriptor.swap(value->native.descriptor);
                        released[end].binding.swap(value->native.nativeBinding);
                        if (!value->readEdges && value->detachLifetime()) {
                            const bool erased = shard->arenas.versions.erase(row.location.value);
                            Q_ASSERT(erased); Q_UNUSED(erased);
                        }
                    }
                    row.value = nullptr;
                    ++end;
                } while (end < live && rows[end]->location.shard == shard);
                first = end;
            }
            Q_ASSERT(m_values >= live);
            m_values -= live;
        };
        // A true one-row lifetime uses the same policy without initializing
        // a whole batch's terminal references. Destruction leaves every gate.
        if (live == 1) {
            MetadataReleasedReferences released;
            release(&released);
        } else {
            MetadataReleasedReferences released[ReleaseBatch];
            release(released);
        }
    }
    if (!m_values && m_shards) m_shards->clear();
}

KisPagePrivateWrite::~KisPagePrivateWrite()
{
    if (!value) return;
    KisPagePrivateWrite *row = this;
    edit.release(&row, 1);
}

bool KisPageMetadataCoordinator::preparePrivateValue(const KisPageWriteInput &write,
    KisPagePrivateWrite *native, QString *error)
try {
    auto *core = m_core.load(std::memory_order_acquire);
    if (!core || core->shards.empty() || !write.version.isValid() || !write.transaction.isValid()
        || !native || native->reserved || native->location.value.isValid()) return false;
    const size_t index = kisStablePageKeyHash(write.version.key) % core->shards.size();
    auto *shard = core->shards[index].get();
    const auto storage = core->budgetAuthority->storage<char>();
    native->edit.retain(core->shards[index], storage);
    MetadataArenaGrowth growth(storage.borrowed());
    QMutexLocker lock(&shard->mutex);
    const auto found = shard->pages.find(write.version.key);
    if (found == shard->pages.end()) return false;
    auto *page = &found->second;
    const auto valid = [&] {
        KisVersionSlotId base;
        return page->revisionCanAdvance() && !page->activity.hasWriter()
            && !page->activity.publicationClaim && page->nextGeneration == write.version.generation
            && shard->records.findVersion(write.baseVersion, &base);
    };
    if (!valid()) return false;
    const MetadataArenaDemand demand{1, 0, 0};
    if (growMetadataArenasOutsideLock(shard, demand, &growth, &lock, valid, error)
        != MetadataGrowthResult::Ready) return false;
    ShardRecordStore::ReservationSet reservation;
    const auto cancel = qScopeGuard([&] { reservation.cancel(&shard->records); });
    if (!shard->records.reserve(demand, 1, 1, &reservation)) {
        KisPageStoreDetail::setError(error, QStringLiteral("final value storage budget was refused"));
        return false;
    }
    const auto slot = shard->arenas.versions.emplaceReserved(&reservation.arenas.versions);
    const auto replica = kisInlineReplicaSlot(slot);
    Q_ASSERT(slot.isValid());
    auto *value = shard->arenas.versions.get(slot);
    value->slot = slot;
    value->version = write.version;
    value->firstReplica = value->authorityReplica = replica;
    value->replicaCount = 1;
    value->primaryReplica.ownerVersion = slot;
    native->location.shard = shard;
    native->location.page = page;
    native->location.shardIndex = index;
    native->location.value = slot;
    native->value = value;
    value->frameHeld = true;
    value->attachLifetime();
    ++native->edit.m_values;
    native->location.exact = std::move(reservation.exactVersions);
    native->location.physical = std::move(reservation.physicalSlots);
    ++shard->nativeValuesPrepared;
    KisPageStoreDetail::setError(error, {});
    return true;
} catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("final value storage budget was refused"));
    return false;
}

namespace {
// The unpublished cell has not entered a view or either lookup index. Its
// creator owns this one cancellation route, including provider refusal.
void discardPrivateValueLocked(MetadataShard &shard, KisPagePrivateWrite &native)
{
    if (!native.value) return;
    Q_ASSERT(native.value->frameHeld && !native.value->indexed);
    shard.records.exactVersions.cancelReservation(&native.location.exact);
    shard.records.physicalSlots.cancelReservation(&native.location.physical);
    // The original Frame consumes target/terminal identity after cancellation.
    // It destroys this same value outside the owner/shard gates at row cleanup.
    ++shard.nativeValuesCancelled;
}
}

void KisPageMetadataCoordinator::discardPrivateValue(KisPagePrivateWrite *native)
{
    if (!native || !native->location.value.isValid()) return;
    auto *core = m_core.load(std::memory_order_acquire);
    Q_ASSERT(core && !native->reserved && native->location.shardIndex < core->shards.size());
    auto *shard = core->shards[native->location.shardIndex].get();
    Q_ASSERT(native->location.shard == shard);
    QMutexLocker lock(&shard->mutex);
    discardPrivateValueLocked(*shard, *native);
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
    if (!native || !native->location.value.isValid()
        || native->location.shardIndex >= core->shards.size()) return false;
    size_t shardIndex = native->location.shardIndex;
    auto *shard = core->shards[shardIndex].get();
    if (native->location.shard != shard || native->reserved || !target.isValid()
        || !(write.baseVersion.key == write.version.key)
        || !(target.version == write.version) || !write.transaction.isValid()
        || !write.operation.isValid()) return false;
    QMutexLocker lock(&shard->mutex);
    const auto *preparedValue = native->location.shard == shard
        ? shard->arenas.versions.get(native->location.value) : nullptr;
    if (!preparedValue || preparedValue != native->value || preparedValue->indexed
        || !(preparedValue->version == write.version)) return false;
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
        const bool erased = shard->records.eraseReplica(oldSlot);
        Q_ASSERT(unindexed && erased); Q_UNUSED(unindexed); Q_UNUSED(erased);
        --base->replicaCount;
        if (kisIsInlineReplica(oldSlot) && !kisIsInlineReplica(sourceSlot)) {
            // Recovery keeps the original Value and moves its surviving exact
            // authority into the now-empty inline position. Preserve list order
            // and all real lease/completion state; only the locator changes.
            auto *sourceLink = &base->firstReplica;
            while (!(*sourceLink == sourceSlot)) {
                auto *previous = shard->records.replica(*sourceLink);
                Q_ASSERT(previous); sourceLink = &previous->nextReplica;
            }
            const auto inlineSlot = kisInlineReplicaSlot(baseSlot);
            base->primaryReplica = *sourceRecord;
            *sourceLink = inlineSlot;
            const bool rebound = shard->records.physicalSlots.replaceExact(
                source.physicalSlotIdentity(), sourceSlot, source.physicalSlotIdentity(), inlineSlot);
            const bool released = shard->records.eraseReplica(sourceSlot);
            Q_ASSERT(rebound && released); Q_UNUSED(rebound); Q_UNUSED(released);
            sourceSlot = inlineSlot;
        }
        base->authorityReplica = sourceSlot;
        auto *facts = &base->native;
        shard->arenas.cpuReadBindings -= bool(facts->nativeBinding);
        native->binding().swap(facts->nativeBinding); // Original result releases/reuses the old binding outside this gate.
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
    native->value->primaryReplica.setHandle(target);
    native->source = source;
    native->reserved = true;
    native->completed = complete;
    Q_ASSERT(native->location.page == page && native->location.shardIndex == shardIndex);
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
    return native && native->version() == identity ? native : nullptr;
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
    if (!result || !result->hasValue() || !target.isValid() || !(target.version == result->version())) return false;
    auto *shard = Private::registeredShard(m_core.load(std::memory_order_acquire), *result);
    if (!shard) return false;
    QMutexLocker lock(&shard->mutex);
    auto *page = static_cast<MetadataPage *>(result->location.page);
    auto *activity = &page->activity;
    KisPageVersion physicalOwner;
    if (!activity || activity->nativeWrite != result || !result->reserved || !result->completed
        || activity->publicationClaim || result->isSealed()
        || target.physicalSlotIdentity() == result->target().physicalSlotIdentity()
        || shard->records.physicalOwner(target.physicalSlotIdentity(), &physicalOwner)) return false;
    result->value->primaryReplica.setHandle(target);
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
    if (native->isSealed()) {
        --shard->arenas.sealedCpuValues;
        native->value->native.sealedBy = {};
        native->value->native.exportedStamp = 0;
    }
    page->activity.nativeWrite = nullptr;
    page->activity.writerTransaction = {};
    if (page->activity.isEmpty()) shard->activities.erase(native->version().key);
    native->reserved = false;
    discardPrivateValueLocked(*shard, *native);
    ++page->revision;
    return true;
}

bool KisPageMetadataCoordinator::sealPrivateValue(KisPagePrivateWrite &result,
    KisPageTransactionId transaction, quint64 issuer, bool complete)
{
    auto *d = m_core.load(std::memory_order_acquire);
    if (!d || !issuer || d->cpuProofIssuer.load() != issuer || !transaction.isValid()
        || !result.reserved || !result.location.shard || !result.location.page) return false;
    // Provider validation has completed. Only the noncopyable original row
    // may seal this registered edit; no public stamp is issued on this path.
    auto *shard = Private::registeredShard(d, result);
    if (!shard) return false;
    QMutexLocker lock(&shard->mutex);
    const auto &activity = static_cast<MetadataPage *>(result.location.page)->activity;
    if (activity.nativeWrite != &result || (!result.completed && !complete)
        || result.isSealed() || !(activity.writerTransaction == transaction)) return false;
    result.completed = true;
    result.value->native.sealedBy = transaction;
    ++shard->arenas.sealedCpuValues;
    return true;
}

namespace {
// Native consumers use the retained Value slot; exported capabilities use
// the exact index and their actual issued stamp. Both resolve the same cell,
// and an unindexed Frame hold alone never grants permission to a detached cell.
template<class Core, class Visit>
bool visitCpuValue(Core *d, const KisPagePublicationIdentity &identity, quint64 issuer, Visit visit)
{
    auto *shard = Core::shardFor(d, identity.version.key);
    if (!shard || !issuer || d->cpuProofIssuer.load() != issuer || !identity.isValid()) return false;
    QMutexLocker lock(&shard->mutex);
    KisPageValueCell *record = nullptr;
    if (identity.native) {
        record = shard->arenas.versions.get(identity.metadataSlot);
        if (!record || (!record->indexed && !record->frameHeld)) return false;
        // The original row alone established sealedBy; cancel, revoke and
        // detachment clear it. That permission, checked below, is distinct
        // from a Frame's memory hold and needs no second page/writer lookup.
    } else {
        KisVersionSlotId slot = identity.metadataSlot;
        if (!slot.isValid()) shard->records.findVersion(identity.version, &slot);
        record = slot.isValid() ? shard->records.version(slot) : nullptr;
        if (!record) {
            const auto *activity = shard->activity(identity.version.key);
            auto *original = activity ? activity->nativeWrite : nullptr;
            if (!original || !original->reserved || !original->completed
                || !(original->version() == identity.version)
                || (slot.isValid() && !(original->valueSlot() == slot))) return false;
            record = original->value;
        }
    }
    auto &native = record->native;
    if (!(record->version == identity.version) || !(native.sealedBy == identity.transaction)
        || (!identity.native && (!native.exportedStamp || native.exportedStamp != identity.stamp))) return false;
    const bool prepared = !record->indexed || (record->publication == KisPagePublicationState::Prepared
        && record->preparedBy == identity.transaction);
    const auto *authority = !record->indexed ? &record->primaryReplica
        : shard->records.replica(record->authorityReplica);
    if (!authority) return false;
    const std::shared_ptr<KisCpuResidentBinding> unbound;
    const auto &binding = !record->indexed || native.nativeReplica == record->authorityReplica ? native.nativeBinding : unbound;
    return visit(*record, *authority, binding, prepared, shard->arenas.sealedCpuValues);
}
}

bool KisPageMetadataCoordinator::cpuPreparedAuthority(const KisPagePublicationIdentity &identity,
    quint64 issuer, KisReplicaHandle *authority,
    std::shared_ptr<const KisPageAllocationDescriptor> *descriptor,
    std::shared_ptr<KisCpuResidentBinding> *binding) const
{
    if (!authority || !descriptor || !binding) return false;
    *authority = {}; descriptor->reset(); binding->reset();
    return visitCpuValue(m_core.load(std::memory_order_acquire), identity, issuer,
        [&](const auto &value, const auto &replica, const auto &originalBinding, bool prepared, auto &) {
            if (!prepared || !value.native.descriptor) return false;
            *authority = replicaHandle(replica, value.version);
            *descriptor = value.native.descriptor; *binding = originalBinding;
            return true;
        });
}

bool KisPageMetadataCoordinator::ownsCpuValue(const KisPagePublicationIdentity &identity, quint64 issuer) const
{
    return identity.native && visitCpuValue(m_core.load(std::memory_order_acquire), identity, issuer,
        [](const auto &, const auto &, const auto &, bool, auto &) { return true; });
}

KisReplicaHandle KisPageMetadataCoordinator::cpuValueAuthority(const KisPagePublicationIdentity &identity, quint64 issuer) const
{
    KisReplicaHandle result;
    if (identity.native) visitCpuValue(m_core.load(std::memory_order_acquire), identity, issuer,
        [&](const auto &value, const auto &authority, const auto &, bool, auto &) {
            result = replicaHandle(authority, value.version);
            return true;
        });
    return result;
}

KisPreparedPageProof KisPageMetadataCoordinator::cpuProof(const KisPagePublicationIdentity &identity,
    quint64 issuer, quint64 exportStamp) const
{
    auto *d = m_core.load(std::memory_order_acquire);
    KisPreparedPageProof result;
    visitCpuValue(d, identity, issuer, [&](auto &value, const auto &authority, const auto &, bool, auto &) {
        auto &native = value.native;
        if (!native.exportedStamp) {
            if (!identity.native || !exportStamp) return false;
            native.exportedStamp = exportStamp;
        }
        result.authority = replicaHandle(authority, value.version);
        result.transaction = native.sealedBy;
        result.providerValidationStamp = native.exportedStamp;
        result.producerCompletion = d->cpuCompletion;
        return true;
    });
    return result;
}

bool KisPageMetadataCoordinator::ownsCpuProof(const KisPreparedPageProof &proof, quint64 issuer) const
{
    auto *d = m_core.load(std::memory_order_acquire);
    if (!d || !proof.isValid() || !(d->cpuCompletion == proof.producerCompletion)) return false;
    return visitCpuValue(d, KisPagePublicationIdentity(proof), issuer,
        [&](const auto &value, const auto &authority, const auto &, bool, auto &) {
            return replicaHandle(authority, value.version) == proof.authority;
        });
}

bool KisPageMetadataCoordinator::revokeCpuProof(const KisPreparedPageProof &proof, quint64 issuer)
{
    auto *d = m_core.load(std::memory_order_acquire);
    if (!d || !proof.isValid() || !(d->cpuCompletion == proof.producerCompletion)) return false;
    return visitCpuValue(d, KisPagePublicationIdentity(proof), issuer,
        [&](auto &value, const auto &authority, const auto &, bool, auto &count) {
            if (!(replicaHandle(authority, value.version) == proof.authority)) return false;
            auto &native = value.native;
            native.sealedBy = {}; native.exportedStamp = 0; --count;
            return true;
        });
}

bool KisPageMetadataCoordinator::revokeCpuProof(const KisPagePublicationIdentity &identity, quint64 issuer)
{
    return visitCpuValue(m_core.load(std::memory_order_acquire), identity, issuer,
        [](auto &value, const auto &, const auto &, bool, auto &count) {
            auto &native = value.native;
            native.sealedBy = {}; native.exportedStamp = 0; --count;
            return true;
        });
}

qsizetype KisPageMetadataCoordinator::sealedCpuValueCount(quint64 issuer) const
{
    const auto *d = m_core.load(std::memory_order_acquire);
    if (!d || !issuer || d->cpuProofIssuer.load() != issuer) return 0;
    qsizetype result = 0;
    for (const auto &shard : d->shards) {
        QMutexLocker lock(&shard->mutex);
        result += shard->arenas.sealedCpuValues;
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
    if (page == shard->pages.cend() || !page->second.hasMetadata())
        return limit > 0;
    if (shard->records.findVersion(target))
        return true;
    quint64 native = 0;
    if (const auto *write = page->second.activity.nativeWrite) {
        if (write->version() == target) return true;
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
    if (page == shard->pages.cend() || !page->second.hasMetadata())
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
    if (page == shard->pages.cend() || !page->second.hasMetadata())
        return result;
    result.exists = true;
    result.total = qsizetype(page->second.historyCount);
    KisVersionSlotId nextHint = page->second.firstHistory;
    if (after.isValid()) {
        bool resumedFromHint = false;
        KisVersionSlotId afterSlot;
        if (shard->records.findVersion(after, &afterSlot)) {
            const KisPageValueCell *record = shard->records.version(afterSlot);
            if (record && record->publication == KisPagePublicationState::Historical) {
                nextHint = record->nextHistory;
                resumedFromHint = true;
            }
        }
        if (!resumedFromHint) {
            while (nextHint.isValid()) {
                const KisPageValueCell *record = shard->records.version(nextHint);
                Q_ASSERT(record);
                if (!record || ShardRecordStore::position(after) < ShardRecordStore::position(record->version))
                    break;
                nextHint = record->nextHistory;
            }
        }
    }
    const size_t limit = std::min(versions.capacity(), size_t(qMin(budget, HistorySlice::Limit)));
    while (nextHint.isValid() && versions.size() < limit) {
        const KisPageValueCell *record = shard->records.version(nextHint);
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
    quint32 *removedMask)
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
            if (!versions[i].isValid() || !(versions[i].key == key)) return false;
            KisVersionSlotId slot;
            if (!shard->records.findVersion(versions[i], &slot)) continue;
            const auto *record = shard->records.version(slot);
            if (record->publication != KisPagePublicationState::Historical
                || record->rootEdges.load(std::memory_order_acquire)
                || record->capturedReadViews.isValid() || (!record->version.isDefaultPixel() && record->readEdges)
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
            // The original Store gate serializes admission with collection. Check
            // the live fact again after the debt preparation window; a raw root or
            // candidate reference is deliberately insufficient protection. Implicit
            // defaults remain reconstructible from the admitted surface revision.
            bool protectedAgain = false;
            for (qsizetype i = 0; i < count; ++i) {
                if ((selected & (quint32(1) << i))
                    && shard->records.version(selectedSlots[size_t(i)])->rootEdges.load(std::memory_order_acquire)) {
                    protectedAgain = true;
                    break;
                }
            }
            if (protectedAgain) continue;
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
    result.reserve(size_t(d->registeredPages.loadAcquire()));
    for (const auto &shard : d->shards) {
        QMutexLocker lock(&shard->mutex);
        quint64 registered = 0;
        for (auto page = shard->pages.cbegin(); page != shard->pages.cend(); ++page) {
            if (!page->second.hasMetadata()) continue;
            ++registered;
            const auto *head = shard->records.version(page->second.publishedVersion);
            Q_ASSERT(head);
            result.push_back(head->version);
        }
        shard->publicationDirectoryHeaders += quint64(registered);
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
            if (page->second.hasMetadata() && (!historicalOnly || page->second.historyCount))
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
                const KisPageValueCell *version = shard->records.version(versionSlot);
                Q_ASSERT(version);
                if (!version) break;
                for (KisReplicaSlotId replicaSlot = version->firstReplica; replicaSlot.isValid();) {
                    const KisPageReplicaState *replica = shard->records.replica(replicaSlot);
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
    const KisPageReplicaState *record = nullptr;
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
    // Stable controls are never erased. Successful first content registration
    // already maintains the exact content-page count; claim-only nodes do not.
    return qsizetype(pageRegistrationCount());
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
            metrics.nativeValuesPrepared += shard->nativeValuesPrepared;
            metrics.nativeValuesAccepted += shard->nativeValuesAccepted;
            metrics.nativeValuesCancelled += shard->nativeValuesCancelled;
            metrics.nativeRetentionRevalidations += shard->nativeRetentionRevalidations;
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
        result.replicaArena += shard->arenas.replicas.statistics();
        result.overflowArena += shard->arenas.overflow.statistics();
        result.exactVersionIndex += shard->records.exactVersions.statistics();
        result.physicalSlotIndex += shard->records.physicalSlots.statistics();
        result.owningCapacityBytes += shard->mapAllocationBytes.load(std::memory_order_relaxed)
            + shard->records.exactVersions.allocatedBytes() + shard->records.physicalSlots.allocatedBytes()
            + quint64(sizeof(MetadataShard));
        for (const auto &entry : shard->pages) result.pages += entry.second.hasMetadata();
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
                const KisPageValueCell *version = shard->records.version(versionSlot);
                Q_ASSERT(version);
                if (!version)
                    break;
                result.trackedPayloadBytes += sizeof(KisPageValueCell);
                for (KisMetadataOverflowSlotId overflowSlot = version->capturedReadViews; overflowSlot.isValid();) {
                    const KisMetadataOverflowNode *node = shard->records.overflow(overflowSlot);
                    Q_ASSERT(node);
                    if (!node)
                        break;
                    result.trackedPayloadBytes += sizeof(KisMetadataOverflowNode);
                    overflowSlot = node->next;
                }
                for (KisReplicaSlotId replicaSlot = version->firstReplica; replicaSlot.isValid();) {
                    const KisPageReplicaState *replica = shard->records.replica(replicaSlot);
                    Q_ASSERT(replica);
                    if (!replica)
                        break;
                    ++result.replicas;
                    if (!kisIsInlineReplica(replicaSlot)) result.trackedPayloadBytes += sizeof(KisPageReplicaState);
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

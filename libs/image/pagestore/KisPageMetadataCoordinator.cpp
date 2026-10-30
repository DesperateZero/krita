/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageMetadataCoordinator.h"
#include "KisImageEpochReferenceModel.h"
#include "KisCpuResidentBinding_p.h"
#include "KisDeferredMetadataCleanup_p.h"
#include "KisPageMetadataCoordinator_p.h"
#include "KisPagePublicationInput_p.h"
#include "KisPageStoreReclamation_p.h"
#include "KisPageWriteCoordinator_p.h"
#include "KisMutationStorage_p.h"

#include <boost/container/small_vector.hpp>
#include <type_traits>
#include "KisPageCommitPolicy_p.h"

#include <QAtomicInteger>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>

#include <algorithm>
#include <boost/iterator/transform_iterator.hpp>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <limits>
#include <memory>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

std::shared_ptr<KisCpuResidentBinding> KisPagePrivateWrite::exchangeBinding(
    std::shared_ptr<KisCpuResidentBinding> next, const KisReplicaHandle &target)
{
    Q_ASSERT(value);
    next.swap(value->native.nativeBinding);
    value->primaryReplica.setHandle(target, value->native.nativeBinding.get());
    return next;
}

namespace {
using MetadataLocalStorage = KisMutationStorageAllocator<char, false>;
template<class T, size_t InlineCapacity = 0> using MetadataLocalArray = std::conditional_t<
    InlineCapacity == 0, std::vector<T, KisMutationStorageAllocator<T, false>>,
    boost::container::small_vector<T, InlineCapacity, KisMutationStorageAllocator<T, false>>>;
}

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
    explicit MetadataArenaGrowth(MetadataLocalStorage storage)
        : versions(storage), replicas(storage), overflow(storage)
        , versionDirectory(storage), replicaDirectory(storage), overflowDirectory(storage) {}
    // Candidate payloads are destroyed before the reservation that accounts
    // for them when preparation exits early.
    KisBackingBudgetReservation budgetReservation;
    MetadataLocalArray<VersionArena::PreparedBlock> versions;
    MetadataLocalArray<ReplicaArena::PreparedBlock> replicas;
    MetadataLocalArray<OverflowArena::PreparedBlock> overflow;
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
    quint64 result = 0;
    for (const auto &replica : snapshot.replicas) {
        result += quint64(replica.readLeases.size());
        result += quint64(replica.pendingLastUses.size());
    }
    return result;
}

struct MetadataGenericActivity {
    KisPageWriterToken writerToken;
    KisPageOperationId writerOperation;
    KisPageGeneration writerDefaultGeneration;
    quint64 writerDefaultRevision = 0;
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
    bool hasMetadata() const { return registered; }
    KisVersionSlotId firstVersion;
    KisVersionSlotId lastVersion;
    KisVersionSlotId firstMutable;
    KisVersionSlotId lastMutable;
    // Memory-only identity before registration; cleared when the original
    // Value joins the content index. Current selection belongs only to ViewRoot.
    KisVersionSlotId initialIdentity;
    bool registered = false;
    KisPageGeneration nextGeneration;
    quint64 revision = 1;
    quint32 versionCount = 0;
    quint32 committedCount = 0;
    quint32 mutableCount = 0;
    MetadataPageActivityFacts activity;
    KisPageRetirementRecord *idleCapacity = nullptr; // Non-owning original paid record.

    bool revisionCanAdvance() const
    {
        return revision != std::numeric_limits<quint64>::max();
    }
};

// The original claim additionally lends one non-owning adapter location.
static_assert(sizeof(MetadataPage) == 168);

/**
 * Stack projection of original page facts and sparse generic payload. Public snapshots are never
 * retained here: stable record slots are projected back to value handles only
 * at a public diagnostic boundary. An entry exists only while a
 * writer, handoff, publication claim, or non-owning idle reference is live.
 */
struct MetadataPageActivity {
    KisPageWriterToken writerToken;
    KisPageOperationId writerOperation;
    KisPageTransactionId writerTransaction;
    KisPageGeneration writerDefaultGeneration;
    quint64 writerDefaultRevision = 0;
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
    MetadataMapAllocator(MetadataLocalStorage storage, std::atomic<quint64> *bytes)
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

    explicit ShardRecordStore(MetadataArenas *storage, KisMutationStorageAllocator<char> allocator,
                              KisPageViewValueStorage *notificationSource)
        : arenas(storage), exactVersions(allocator), physicalSlots(allocator), notificationSource(notificationSource)
    {
        Q_ASSERT(arenas);
    }

    MetadataArenas *arenas = nullptr;
    ExactIndex exactVersions;
    PhysicalIndex physicalSlots;
    // Borrowed only while this shard is admitted by its original facade.
    // Detach clears it under V before an escaped shard can outlive the core.
    KisPageViewValueStorage *notificationSource = nullptr;

    static KisPagePublicationState storedPublication(KisPagePublicationState state)
    {
        return state == KisPagePublicationState::Published ? KisPagePublicationState::Historical : state;
    }

    void reconsiderHistory(KisPageValueCell *value)
    {
        if (!notificationSource || !value->indexed
            || value->publication != KisPagePublicationState::Historical
            || value->rootEdges.load(std::memory_order_acquire)
            || (!value->version.isDefaultPixel() && value->readEdges)) return;
        KisPageViewValue *original = value;
        notificationSource->requestReclaim(&original, 1);
    }

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
            ++page->committedCount;
        }
        if (record->publication == KisPagePublicationState::Prepared
            || record->publication == KisPagePublicationState::Unpublished) {
            linkMutable(page, record);
        }
    }

    void unindexPublication(MetadataPage *page, KisPageValueCell *record)
    {
        if (record->publication == KisPagePublicationState::Historical) {
            Q_ASSERT(page->committedCount);
            --page->committedCount;
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
        const bool selectedByRoot = publication == KisPagePublicationState::Published;
        publication = storedPublication(publication);
        if (record->publication == publication && record->preparedBy == preparedBy) return true;
        unindexPublication(page, record);
        record->publication = publication;
        record->preparedBy = preparedBy;
        indexPublication(page, record);
        // The incoming cut will retain this explicit content. Its eventual
        // last root edge supplies reclamation; do not queue it before that
        // same cut activates the root. Defaults remain reconstructible caches.
        if (!selectedByRoot || record->version.isDefaultPixel()) reconsiderHistory(record);
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
    void initializeValue(KisPageValueCell *record, const Snapshot &snapshot,
                         MetadataArenaReservations *reservation)
    {
        // Only true new cells or the memory-only initial-root identity enter
        // here. Existing Values are exclusively edited at their original slots.
        Q_ASSERT(record && reservation && !record->indexed && !record->firstReplica.isValid() && !record->frameHeld);
        Q_ASSERT(!record->version.isValid() || record->version == snapshot.version);
        if (!record->version.isValid()) record->version = snapshot.version;
        record->initialIdentity = false;
        record->readEdges = snapshot.readEdges;
        record->publication = storedPublication(snapshot.publication);
        record->preparedBy = snapshot.preparedBy;
        record->replicaCount = quint32(snapshot.replicas.size());

        KisReplicaSlotId replicaTail;
        for (const auto &source : snapshot.replicas) {
            KisPageReplicaState next;
            next.setHandle(source.replica);
            next.ownerVersion = record->slot;
            next.activeOperation = source.activeOperation;
            next.pinCount = source.pinCount;
            next.validity = source.validity;
            const KisReplicaSlotId replicaSlot = replicaTail.isValid()
                ? arenas->replicas.emplaceReserved(&reservation->replicas, next) : kisInlineReplicaSlot(record->slot);
            if (!replicaTail.isValid()) record->primaryReplica = next;
            Q_ASSERT(replicaSlot.isValid());
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

    KisPageVersionStateSnapshot project(const KisPageValueCell &record) const
    {
        KisPageVersionStateSnapshot result;
        result.version = record.version;
        result.publication = record.publication;
        result.preparedBy = record.preparedBy;
        result.readEdges = record.version.isDefaultPixel() ? 0 : record.readEdges;
        result.replicas.reserve(qsizetype(record.replicaCount));
        for (KisReplicaSlotId slot = record.firstReplica; slot.isValid();) {
            const KisPageReplicaState *stored = replica(slot);
            Q_ASSERT(stored);
            if (!stored)
                break;
            KisReplicaStateSnapshot projected;
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

    static bool matchesReplica(const KisPageReplicaState &stored, const KisPageVersion &version,
                               const KisReplicaHandle &identity)
    { return version == identity.version && stored.identity() == identity.allocationIdentity()
        && stored.domain() == identity.domain && stored.layout() == identity.layout; }
    static bool matchesReplica(const KisPageReplicaState &stored, const KisPageVersion &version,
                               const KisPageWriteSource &identity)
    { return stored.matchesSource(identity, version); }
    template<class Identity>
    bool findReplicaSlot(KisVersionSlotId owner, const Identity &identity, KisReplicaSlotId *slot) const
    {
        if (!slot)
            return false;
        *slot = {};
        if (!identity.isValid())
            return true;
        const KisPageValueCell *record = version(owner);
        if (!record || !(record->version == identity.version))
            return false;
        KisReplicaSlotId current;
        if (!physicalSlots.findExact(identity.physicalSlotIdentity(), &current)) return false;
        const auto *stored = replica(current);
        if (!stored || !(stored->ownerVersion == owner) || !matchesReplica(*stored, record->version, identity))
            return false;
        *slot = current;
        return true;
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
        if (!releaseRead || !transition.completion.isValid()) reconsiderHistory(version(versionSlot));
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
    Info versionInfo(const KisPageVersion &identity) const
    {
        KisVersionSlotId slot;
        return versionInfo<Info>(findVersion(identity, &slot) ? version(slot) : nullptr);
    }

    template<class Info>
    Info versionInfo(const KisPageValueCell *stored) const
    {
        Info result;
        if (!stored) return result;
        result.version = stored->version;
        result.publication = stored->publication;
        result.preparedBy = stored->preparedBy;
        const auto *authority = replica(stored->authorityReplica);
        if (authority) result.authority = replicaHandle(*authority, stored->version);
        result.replicaCount = stored->replicaCount;
        result.captured = !stored->version.isDefaultPixel() && stored->readEdges;
        return result;
    }

    KisPageStateSnapshot
    activityHeader(const KisPageKey &key, const MetadataPage &page, const MetadataPageActivity *activity) const
    {
        KisPageStateSnapshot result;
        result.key = key;
        result.nextGeneration = page.nextGeneration;
        if (!activity)
            return result;
        if (activity->nativeWrite) {
            result.nativeGeneration = activity->nativeWrite->version().generation;
            result.nativeTransaction = activity->writerTransaction;
        }
        if (activity->writerPhase != KisPageWriterPhase::None) {
            result.writer.baseAuthority = projectReplica(activity->writerBaseAuthority);
            Q_ASSERT(result.writer.baseAuthority.isValid() || activity->writerDefaultRevision);
            result.writer.token = activity->writerToken;
            result.writer.operation = activity->writerOperation;
            result.writer.transaction = activity->writerTransaction;
            result.writer.baseVersion = result.writer.baseAuthority.isValid()
                ? result.writer.baseAuthority.version
                : KisPageVersion{key, activity->writerDefaultGeneration, activity->writerDefaultRevision};
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
    header(const KisPageKey &key, const MetadataPage &page, const MetadataPageActivity *activity,
           const KisImageEpochRootSnapshot &view) const
    {
        KisPageVersion selected;
        if (!view.resolve(key, &selected)) return {};
        auto result = activityHeader(key, page, activity);
        result.publishedEpoch = view.epoch();
        result.publishedGeneration = selected.generation;
        result.publishedDefaultPixelRevision = selected.defaultPixelRevision;
        return result;
    }

    KisPageStateSnapshot
    snapshot(const KisPageKey &key, const MetadataPage &page, const MetadataPageActivity *activity,
             const KisImageEpochRootSnapshot &view) const
    {
        KisPageStateSnapshot result = header(key, page, activity, view);
        result.versions.reserve(qsizetype(page.versionCount));
        for (KisVersionSlotId slot = page.firstVersion; slot.isValid();) {
            const KisPageValueCell *record = version(slot);
            Q_ASSERT(record);
            if (!record) break;
            result.versions.append(project(*record));
            slot = record->nextVersion;
        }
        const KisPageVersion selected{key, result.publishedGeneration, result.publishedDefaultPixelRevision};
        if (auto *value = result.findVersion(selected)) {
            if (value->publication != KisPagePublicationState::Historical) return {};
            value->publication = KisPagePublicationState::Published;
        }
        else if (selected.isDefaultPixel()) {
            KisPageVersionStateSnapshot implicit;
            implicit.version = selected;
            implicit.publication = KisPagePublicationState::Published;
            result.versions.append(std::move(implicit));
        }
        return result;
    }

    bool assignActivityHeader(MetadataPage *page, MetadataPageActivity *activity, const KisPageStateSnapshot &state) const
    {
        if (!page || !activity) return false;
        MetadataPageActivity next = *activity;
        next.writerToken = {};
        next.writerOperation = {};
        if (!next.nativeWrite) next.writerTransaction = {};
        next.writerDefaultGeneration = {};
        next.writerDefaultRevision = 0;
        next.writerBaseAuthority = {};
        next.writerTarget = {};
        next.writerMode = KisPageWriteMode::PreserveContents;
        next.writerPhase = KisPageWriterPhase::None;
        if (state.writer.phase != KisPageWriterPhase::None) {
            KisVersionSlotId writerVersion, baseVersion;
            if (!findVersion(state.writer.target.version, &writerVersion)
                || (state.writer.baseAuthority.isValid()
                    ? !findVersion(state.writer.baseVersion, &baseVersion)
                        || !findReplicaSlot(baseVersion, state.writer.baseAuthority, &next.writerBaseAuthority)
                    : !state.writer.baseVersion.isDefaultPixel())
                || !findReplicaSlot(writerVersion, state.writer.target, &next.writerTarget)) {
                return false;
            }
            if (!state.writer.baseAuthority.isValid()) {
                next.writerDefaultGeneration = state.writer.baseVersion.generation;
                next.writerDefaultRevision = state.writer.baseVersion.defaultPixelRevision;
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

        page->nextGeneration = state.nextGeneration;
        *activity = next;
        return true;
    }

    template<class Snapshot>
    void finishReservedValue(KisPageValueCell *record, const Snapshot &snapshot,
                             ReservationSet *reservation)
    {
        if (!record->indexed) record->attachLifetime();
        record->indexed = true;
        for (auto slot = record->firstReplica; slot.isValid();) {
            const auto *stored = replica(slot);
            const auto identity = stored->identity();
            const bool indexed = physicalSlots.insertReserved(&reservation->physicalSlots,
                {identity.provider, identity.providerEpoch, identity.allocation.slot}, slot);
            Q_ASSERT(indexed); Q_UNUSED(indexed);
            slot = stored->nextReplica;
        }
        if (snapshot.publication != KisPagePublicationState::Published || record->version.isDefaultPixel())
            reconsiderHistory(record);
    }

    template<class Snapshot>
    KisVersionSlotId insertReserved(MetadataPage *page, const Snapshot &snapshot,
                                    ReservationSet *reservation)
    {
        auto *initial = !page->hasMetadata() ? arenas->versions.get(page->initialIdentity) : nullptr;
        const bool initialized = initial && initial->initialIdentity && initial->version == snapshot.version;
        const auto slot = initialized ? initial->slot
            : arenas->versions.emplaceReserved(&reservation->arenas.versions);
        if (!slot.isValid()) return {};
        auto *record = arenas->versions.get(slot);
        if (!initialized) record->slot = slot;
        linkVersion(page, record);
        initializeValue(record, snapshot, &reservation->arenas);
        const bool indexed = exactVersions.insertReserved(&reservation->exactVersions, snapshot.version, slot);
        Q_ASSERT(indexed); Q_UNUSED(indexed);
        indexPublication(page, record);
        finishReservedValue(record, snapshot, reservation);
        return slot;
    }

    // Consume already validated original replica slots. No survivor is
    // projected/recreated: leases, pending uses and other placements stay put.
    KisVersionSlotId insertEmptyReserved(MetadataPage *page, const KisPageVersion &identity,
        KisPagePublicationState state, KisPageTransactionId preparedBy, ReservationSet *reservation)
    {
        struct EmptyValue {
            const KisPageVersion &version;
            KisPagePublicationState publication;
            KisPageTransactionId preparedBy;
            KisReplicaHandle authority;
            quint32 readEdges = 0;
            std::array<KisReplicaStateSnapshot, 0> replicas;
        };
        return insertReserved(page, EmptyValue{identity, state, preparedBy, {}, 0, {}}, reservation);
    }

    KisReplicaSlotId appendSingleReplicaReserved(KisPageValueCell *record, const KisReplicaHandle &handle,
        ReservationSet *reservation)
    {
        Q_ASSERT(record && record->replicaCount == 0 && !record->firstReplica.isValid());
        const auto slot = kisInlineReplicaSlot(record->slot);
        record->primaryReplica.setHandle(handle);
        record->primaryReplica.ownerVersion = record->slot;
        record->firstReplica = slot; record->replicaCount = 1;
        const bool indexed = physicalSlots.insertReserved(&reservation->physicalSlots, handle.physicalSlotIdentity(), slot);
        Q_ASSERT(indexed); Q_UNUSED(indexed);
        return slot;
    }

    KisReplicaSlotId replaceHandoffAuthority(KisPageValueCell *base,
        KisReplicaSlotId oldSlot, KisReplicaSlotId sourceSlot)
    {
        auto *old = replica(oldSlot);
        const auto oldIdentity = old->identity();
        const KisReplicaPhysicalSlotIdentity oldPhysical{oldIdentity.provider,
            oldIdentity.providerEpoch, oldIdentity.allocation.slot};
        auto *link = &base->firstReplica;
        while (!(*link == oldSlot)) link = &replica(*link)->nextReplica;
        *link = old->nextReplica;
        const bool unindexed = physicalSlots.eraseExact(oldPhysical, oldSlot);
        const bool erased = eraseReplica(oldSlot);
        Q_ASSERT(unindexed && erased); Q_UNUSED(unindexed); Q_UNUSED(erased);
        --base->replicaCount;
        if (base->native.nativeReplica == oldSlot) base->native.nativeReplica = {};
        if (kisIsInlineReplica(oldSlot) && !kisIsInlineReplica(sourceSlot)) {
            auto *source = replica(sourceSlot);
            const auto identity = source->identity();
            const KisReplicaPhysicalSlotIdentity physical{identity.provider, identity.providerEpoch, identity.allocation.slot};
            auto *sourceLink = &base->firstReplica;
            while (!(*sourceLink == sourceSlot)) sourceLink = &replica(*sourceLink)->nextReplica;
            const auto inlineSlot = kisInlineReplicaSlot(base->slot);
            base->primaryReplica = *source;
            *sourceLink = inlineSlot;
            const bool rebound = physicalSlots.replaceExact(physical, sourceSlot, physical, inlineSlot);
            const bool released = eraseReplica(sourceSlot);
            Q_ASSERT(rebound && released); Q_UNUSED(rebound); Q_UNUSED(released);
            if (base->native.nativeReplica == sourceSlot) base->native.nativeReplica = inlineSlot;
            sourceSlot = inlineSlot;
        }
        base->authorityReplica = sourceSlot;
        return sourceSlot;
    }

    KisVersionSlotId installNativeValue(MetadataPage *page, KisPagePrivateWrite &native,
        KisPagePublicationState publication = KisPagePublicationState::Prepared)
    {
        const auto slot = native.location.value;
        auto *record = native.value;
        Q_ASSERT(record && arenas->versions.get(slot) == record && !record->indexed
            && record->version == native.version());
        const auto cpuSlot = record->authorityReplica;
        record->publication = storedPublication(publication);
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
        if (publication != KisPagePublicationState::Published || record->version.isDefaultPixel())
            reconsiderHistory(record);
        return slot;
    }

    const KisPageValueCell *unusedRecord(const KisPageVersion &identity, KisVersionSlotId slot = {}) const
    {
        if (identity.isDefaultPixel() || (!slot.isValid() && !findVersion(identity, &slot))) return nullptr;
        const auto *record = version(slot);
        if (!record || !(record->version == identity)) return nullptr;
        const auto *facts = &record->native;
        const auto *cpu = replica(record->authorityReplica);
        const bool accepted = record->publication == KisPagePublicationState::Historical
            || (record->publication == KisPagePublicationState::Prepared && facts->sealedBy.isValid());
        return accepted && !record->readEdges
            && record->replicaCount == 1 && cpu && cpu->domain() == KisPageAccessDomain::CpuRam
            && cpu->validity == KisReplicaValidity::Valid && !cpu->pinCount
            && !cpu->overflow.isValid() && !cpu->activeOperation.isValid() ? record : nullptr;
    }

    bool canDetachSelected(const KisPageValueCell *selected) const
    {
        return selected && selected->publication == KisPagePublicationState::Historical
            && unusedRecord(selected->version, selected->slot);
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

    void remove(MetadataPage *page, const KisPageVersion &identity,
                std::shared_ptr<const KisPageAllocationDescriptor> *releasedDescriptor = nullptr,
                std::shared_ptr<KisCpuResidentBinding> *releasedBinding = nullptr,
                KisVersionSlotId slot = {})
    {
        if (!slot.isValid() && !findVersion(identity, &slot))
            return;
        KisPageValueCell *record = version(slot);
        Q_ASSERT(record && record->version == identity);
        remove(page, record, releasedDescriptor, releasedBinding);
    }

    void remove(MetadataPage *page, KisPageValueCell *record,
                std::shared_ptr<const KisPageAllocationDescriptor> *releasedDescriptor,
                std::shared_ptr<KisCpuResidentBinding> *releasedBinding)
    {
        const auto slot = record->slot;
        const auto identity = record->version;
        unindexPhysical(*record);
        unindexPublication(page, record);
        unlinkVersion(page, record);
        // Original root/overlay references can outlive the logical cut just
        // like the Frame. Keep terminal identity in this same cell, without
        // keeping the Resource alive after its existing release handover.
        const bool retainIdentity = record->frameHeld
            || (record->viewMemoryState.load(std::memory_order_relaxed) & ~KisPageViewValue::OriginalLifetime);
        auto primary = record->primaryReplica;
        if (retainIdentity) primary.detachResource(record->version);
        eraseChildren(record);
        if (retainIdentity) record->primaryReplica = primary;
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
        const auto *initial = page ? arenas->versions.get(page->initialIdentity) : nullptr;
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
            const auto inserted = insertReserved(page, snapshot, &reservation);
            Q_ASSERT(inserted.isValid()); Q_UNUSED(inserted);
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
                           KisMutationStorageAllocator<char> storage, KisPageViewValueStorage *notificationSource)
        : budgetAuthority(std::move(authority))
        , budgetCharge(budgetAuthority, 0)
        , arenas(storage)
        , records(&arenas, storage, notificationSource)
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
    quint64 publicationCommittedValuesPrepared = 0;
    quint64 publicationAdditionRecordsPrepared = 0;
    const MetadataPageActivityFacts *activity(const KisPageKey &key) const
    {
        const auto found = pages.find(key);
        return found == pages.cend() || found->second.activity.isEmpty() ? nullptr : &found->second.activity;
    }

    // Original terminal memory responsibility, entered only under this V gate.
    // Raw release resolves its generational slot first; a queued batch already
    // holds the exact cell and drops its reference inside the same gate.
    bool eraseReleasedValue(KisPageValueCell *value, MetadataReleasedReferences *released)
    {
        if (!value || value->indexed || value->frameHeld || value->readEdges
            || value->viewMemoryState.load(std::memory_order_acquire)) return false;
        if (value->initialIdentity) {
            const auto page = pages.find(value->version.key);
            if (page != pages.end() && !page->second.hasMetadata()
                && page->second.initialIdentity == value->slot) page->second.initialIdentity = {};
        }
        released->descriptor.swap(value->native.descriptor);
        released->binding.swap(value->native.nativeBinding);
        const bool erased = arenas.versions.erase(value->slot);
        Q_ASSERT(erased);
        return erased;
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

    bool validDetachment(const MetadataPage &page, const KisPagePublicationChange &input,
                         KisPageTransactionId transaction) const
    {
        const auto *native = input.replacement.native;
        const auto &version = input.mutationVersion();
        const auto &activity = page.activity;
        if (native ? activity.nativeWrite != native || !native->reserved || !native->completed
                        || !native->isSealed() || !(native->sealedBy() == transaction)
                   : activity.hasWriter()) return false;
        if (native && version == native->version()) return true;
        if (input.supersededValue) {
            const auto *source = records.version(input.superseded.metadataSlot);
            if (source != input.supersededValue || !(source->authorityReplica == kisInlineReplicaSlot(source->slot)))
                return false;
        }
        return records.isPreparedBy(version, transaction, input.superseded.native,
            input.superseded.native ? input.supersededAuthority : nullptr, input.superseded.metadataSlot);
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
                ++publicationCommittedValuesInstalled;
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
        const auto *published = records.version(input.superseded.metadataSlot);
        if ((published ? !published->indexed
                || published->publication != KisPagePublicationState::Historical
                || !(published->version == input.superseded.version)
                : input.superseded.metadataSlot.isValid() || !input.superseded.version.isDefaultPixel())
            || !(input.superseded.version.key == input.replacement.key())
            || (native ? page.activity.nativeWrite != native || !native->reserved
                || !native->completed || !native->isSealed() || !(native->sealedBy() == transaction)
                : bool(page.activity.nativeWrite))) return false;
        if (input.supersededValue) {
            // Same original Value, not two equal projections of its authority.
            // Store admission excludes placement changes during fee/install.
            if (published != input.supersededValue
                || !(published->authorityReplica == kisInlineReplicaSlot(published->slot))) return false;
        } else if (input.supersededAuthority && input.supersededAuthority->isValid()
            && (!published || !(records.projectReplica(published->authorityReplica) == *input.supersededAuthority))) return false;
        const auto visit = [&](auto apply) {
            for (auto slot = page.firstMutable; slot.isValid();) {
                const auto *record = records.version(slot); Q_ASSERT(record);
                apply(*record); slot = record->nextMutable;
            }
        };
        // The source ViewRoot and its epoch were checked at prepare; the
        // epoch owner validates that same root at the actual install cut.
        // Metadata checks only the original Value and real mutable facts.
        const KisPageCommitFacts facts{transaction, epoch};
        const IncomingCommit implicit{input.superseded.version, KisPagePublicationState::Historical, {}};
        const auto valid = [&](const auto *target) {
            return published
                ? kisPageValidateCommit(KisImageEpochId{}, KisPageGeneration{}, facts, target, published, visit).isEmpty()
                : kisPageValidateCommit(KisImageEpochId{}, KisPageGeneration{}, facts, target, &implicit, visit).isEmpty();
        };
        if (native) {
            const IncomingCommit incoming{native->version(), KisPagePublicationState::Prepared, transaction};
            return valid(&incoming);
        }
        const auto *target = records.version(work.replacementSlot);
        if (!target || !(target->version == input.replacement.version())) return false;
        if (input.canonicalRecord) {
            const auto *original = static_cast<const MetadataAcceptanceReferences *>(input.canonicalRecord);
            if (!(records.projectReplica(target->authorityReplica) == original->targetAuthority)) return false;
        }
        if (input.replacement.isNativeValue() && !(target->native.sealedBy == transaction)) return false;
        return valid(target);
    }
    void installPublished(MetadataPage &page, KisPagePublicationChange &input,
        const KisPagePublicationChange::MetadataAcceptance &work, KisPageTransactionId transaction, KisImageEpochId epoch,
        std::shared_ptr<const KisPageAllocationDescriptor> *releasedDescriptor,
        std::shared_ptr<KisCpuResidentBinding> *releasedBinding)
    {
        auto *native = input.replacement.native;
        auto *published = records.version(input.superseded.metadataSlot);
        const KisPageCommitFacts facts{transaction, epoch};
        const auto edit = [&](auto &record, auto state, auto preparedBy) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(record)>, IncomingCommit>) {
                if (record.slot == input.superseded.metadataSlot && input.detachesSuperseded) {
                    records.remove(&page, record.version, releasedDescriptor, releasedBinding, record.slot);
                } else if (record.publication != ShardRecordStore::storedPublication(state)
                           || !(record.preparedBy == preparedBy)) {
                    const bool changed = records.setPublication(&page, &record, state, preparedBy);
                    Q_ASSERT(changed); Q_UNUSED(changed);
                    publicationCommittedValuesInstalled += ShardRecordStore::storedPublication(state) == KisPagePublicationState::Historical;
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
            ++publicationCommittedValuesInstalled;
            ++publicationAdditionRecordsTransferred;
            ++publicationVersionInstalls;
            acceptNativeResult(page, accepted, *native);
        } else {
            accepted = work.replacementSlot;
            auto *target = records.version(accepted); Q_ASSERT(target);
            kisPageApplyCommit(facts, target, published, visit, edit);
        }
        if (published && published->version.isDefaultPixel()) records.reconsiderHistory(published);
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
        next.writerDefaultGeneration = g.writerDefaultGeneration;
        next.writerDefaultRevision = g.writerDefaultRevision;
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
        g.writerDefaultGeneration = next.writerDefaultGeneration;
        g.writerDefaultRevision = next.writerDefaultRevision;
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

    KisPageStateSnapshot activityHeader(const KisPageKey &key, const MetadataPage &page) const
    {
        const auto image = activityImage(key, page);
        return records.activityHeader(key, page, &image);
    }

    bool installActivityHeader(const KisPageKey &key, MetadataPage *page, const KisPageStateSnapshot &state)
    {
        auto next = activityImage(key, *page);
        if (!records.assignActivityHeader(page, &next, state)) return false;
        installActivityImage(key, page, next);
        return true;
    }

    KisPageStateSnapshot header(const KisPageKey &key, const MetadataPage &page,
                                const KisImageEpochRootSnapshot &view) const
    {
        const auto image = activityImage(key, page);
        return records.header(key, page, &image, view);
    }

    bool assign(const KisPageKey &key, MetadataPage *page, const KisPageStateSnapshot &state)
    {
        if (!page || !records.initialize(page, state.versions))
            return false;
        const bool installed = installActivityHeader(key, page, state);
        Q_ASSERT(installed);
        page->registered = installed;
        page->initialIdentity = {};
        return installed;
    }

    KisPageStateSnapshot snapshot(const KisPageKey &key, const MetadataPage &page,
                                  const KisImageEpochRootSnapshot &view) const
    {
        ++fullSnapshotExports;
        fullSnapshotVersionInputs += page.versionCount;
        if (kisOnPageStoreReclamationThread()) {
            ++backgroundSnapshotExports;
            backgroundSnapshotVersionInputs += page.versionCount;
        }
        const auto image = activityImage(key, page);
        return records.snapshot(key, page, &image, view);
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
    return version.publication == KisPagePublicationState::Historical
        || (version.publication == KisPagePublicationState::Prepared
            && !version.version.isDefaultPixel() && version.readEdges);
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
        && (state.domain() == KisPageAccessDomain::CpuRam || state.domain() == KisPageAccessDomain::UmaShared);
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
    void requestReclaim(KisPageViewValue *const *, size_t count) noexcept override;
    size_t takeReclaims(KisPageViewValue **, size_t capacity) noexcept;
    void clearReclaims() noexcept;
    void releaseReclaims(KisPageViewValue **, size_t count, bool retired = false) noexcept;
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
    QMutex reclaimMutex;
    KisPageViewValue *firstReclaim = nullptr;
    std::atomic<size_t> pendingReclaims{0};
    KisPageViewValue *lastReclaim = nullptr;
    void *reclaimWakeContext = nullptr;
    void (*reclaimWake)(void *) noexcept = nullptr;
};

void KisPageMetadataCoordinator::Private::requestReclaim(
    KisPageViewValue *const *values, size_t count) noexcept
{
    QMutexLocker lock(&reclaimMutex);
    if (!attached.load(std::memory_order_acquire)) return;
    size_t added = 0;
    for (size_t i = 0; i < count; ++i) {
        auto *value = values[i];
        if (value->nextReclaim) continue;
        retain(value);
        value->nextReclaim = value;
        if (lastReclaim) lastReclaim->nextReclaim = value;
        else firstReclaim = value;
        lastReclaim = value;
        ++added;
    }
    const auto previous = pendingReclaims.fetch_add(added, std::memory_order_release);
    // The original empty -> nonempty transition owns automatic delivery.
    // Holding this gate also fences callback disconnection during teardown.
    if (added && !previous && reclaimWake) reclaimWake(reclaimWakeContext);
}

size_t KisPageMetadataCoordinator::Private::takeReclaims(
    KisPageViewValue **values, size_t capacity) noexcept
{
    if (!pendingReclaims.load(std::memory_order_acquire)) return 0;
    QMutexLocker lock(&reclaimMutex);
    size_t count = 0;
    while (firstReclaim && count < capacity) {
        auto *value = firstReclaim;
        auto *next = value->nextReclaim;
        values[count++] = value;
        value->nextReclaim = nullptr;
        firstReclaim = next == value ? nullptr : next;
    }
    if (!firstReclaim) lastReclaim = nullptr;
    pendingReclaims.fetch_sub(count, std::memory_order_release);
    // Each returned pointer owns the queue's original memory hold. A real
    // new change may enqueue it again while this batch is being processed.
    return count;
}

void KisPageMetadataCoordinator::Private::clearReclaims() noexcept
{
    KisPageViewValue *values[32];
    while (const auto count = takeReclaims(values, 32)) releaseReclaims(values, count);
}

void KisPageMetadataCoordinator::Private::releaseReclaims(
    KisPageViewValue **values, size_t count, bool retired) noexcept
{
    size_t first = 0;
    while (first < count) {
        auto *shard = shardFor(this, values[first]->version.key);
        MetadataReleasedReferences released;
        MetadataArenas::ReleasedBlocks blocks;
        QMutexLocker lock(&shard->mutex);
        bool erased = false;
        do {
            auto *value = static_cast<KisPageValueCell *>(values[first++]);
            const auto before = value->viewMemoryState.fetch_sub(1, std::memory_order_acq_rel);
            Q_ASSERT(before & ~KisPageViewValue::OriginalLifetime);
            if (before == 1) erased = shard->eraseReleasedValue(value, &released) || erased;
            // The usual retired cell has already handed out its payload.
            // A rare detached payload finishes this batch for out-of-V release.
            if (released.descriptor || released.binding) break;
        } while (first < count && shardFor(this, values[first]->version.key) == shard);
        // Queue memory is gone before testing which blocks really became empty.
        // This is the original arena policy, once per actual shard run.
        if (retired || erased) blocks = shard->arenas.takeEmptyBlocks(&shard->budgetCharge);
    }
}

size_t KisPageMetadataCoordinator::pendingValueChecks() const
{
    const auto *core = m_core.load(std::memory_order_acquire);
    return core ? core->pendingReclaims.load(std::memory_order_acquire) : 0;
}

void KisPageMetadataCoordinator::setValueReclaimWake(
    void *context, void (*wake)(void *) noexcept)
{
    auto *core = m_core.load(std::memory_order_acquire);
    if (!core) return;
    QMutexLocker lock(&core->reclaimMutex);
    core->reclaimWakeContext = context;
    core->reclaimWake = wake;
}

void KisPageMetadataCoordinator::clearValueChecks()
{
    if (auto *core = m_core.load(std::memory_order_acquire)) core->clearReclaims();
}

KisPageMetadataCoordinator::HistoryCollection KisPageMetadataCoordinator::collectHistoryValues(
    void *cacheOwner, bool (*keepDefault)(void *, const KisPageVersion &))
{
    HistoryCollection result;
    auto *core = m_core.load(std::memory_order_acquire);
    if (!core) return result;
    KisPageViewValue *values[HistoryCollection::Limit];
    result.examined = core->takeReclaims(values, HistoryCollection::Limit);
    const auto release = qScopeGuard([&] {
        core->releaseReclaims(values, result.examined, result.retired != 0);
    });
    // Cache policy belongs to the view owner. Query only actual default
    // candidates, outside V; nondefault collection has no root lookup.
    const size_t count = keepDefault ? size_t(std::partition(values, values + result.examined,
        [&](const auto *value) { return !value->version.isDefaultPixel()
            || !keepDefault(cacheOwner, value->version); }) - values) : result.examined;
    for (size_t first = 0; first < count;) {
        size_t end = first + 1;
        while (end < count && values[end]->version.key == values[first]->version.key) ++end;
        quint32 removed = 0;
        bool accepted = false;
        try { accepted = retireHistoryValues(values + first, end - first, &removed); }
        catch (const std::bad_alloc &) {} // This same original obligation resumes.
        if (!accepted) {
            core->requestReclaim(values + first, end - first);
            result.blocked = true;
        }
        result.retired += qPopulationCount(removed);
        first = end;
    }
    return result;
}

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
        else if (!page->second.hasMetadata()) slot = page->second.initialIdentity;
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
        if (!shard->records.findVersion(identity, &slot)) slot = page->second.initialIdentity;
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
            page->second.initialIdentity = slot;
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
    shard->eraseReleasedValue(shard->arenas.versions.get(slot), &released);
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
        std::shared_ptr<KisCpuResidentBinding> releasedReadBinding;
        KisPageKey key() const { return version.key; }
    };
    static constexpr size_t NoAuthority = std::numeric_limits<size_t>::max();
    struct Publication {
        KisPageCpuPublicationResource resourceAcceptance;
        KisVersionSlotId targetValue;
        KisVersionSlotId displacedDefault;
        size_t beforeAuthority = NoAuthority;
        size_t targetAuthority = NoAuthority;
        bool addition = false;
        struct Lease {
            KisPageVersion base;
            KisPageWriterToken writer;
            KisPageOperationId operation;
            KisPageWriteMode mode;
            KisVersionSlotId baseValue;
            KisReplicaSlotId before;
            KisReplicaSlotId removed;
        };
        std::optional<Lease> lease;
    };
    // The real fee/provider identities are frozen once. The two optional
    // locators bind original plan fields after the single sort and are never
    // used after preparation. No second authority array allocation/owner.
    struct BackingAuthority {
        KisReplicaHandle replica;
        size_t *first = nullptr;
        size_t *second = nullptr;
        bool introduced = false;
    };
    const KisReplicaHandle &authority(size_t index) const
    {
        if (index != NoAuthority) return backingAuthorities[index].replica;
        static const KisReplicaHandle empty;
        return empty;
    }
    void addAuthority(const KisReplicaHandle &replica, size_t &index, bool introduced = false)
    {
        if (!replica.isValid()) return;
        index = backingAuthorities.size();
        backingAuthorities.push_back({replica, &index, nullptr, introduced});
    }
    bool capture(size_t, const KisPageTransition &, const KisImageEpochRootSnapshot *, QString *);
    bool freezeAuthorities(QString *);
    struct ShardArenaGrowth {
        explicit ShardArenaGrowth(MetadataLocalStorage storage) : blocks(storage) {}
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

    size_t claimRows(std::optional<QMutexLocker<QMutex>> *held);
    void releaseClaims(size_t);
    void installRows(bool mutation, bool recoverable, bool privateWrite, KisImageEpochId,
                     std::optional<QMutexLocker<QMutex>> *held);
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
    // Actual cold effects belong to root/lease candidates. Native acceptance
    // borrows its original rows and never creates these entries.
    Payload<Publication> publications;
    void popPublication() { if (!publications.empty()) publications.pop_back(); }
    Payload<BackingAuthority> backingAuthorities;

private:
    Data(const Storage &allocator, size_t units, Entry *entryData, size_t entryCount,
         Publication *publicationData, size_t publicationCount,
         ShardArenaGrowth *growthData, size_t growthCount, BackingAuthority *authorityData, size_t authorityCount)
        : storage(allocator), allocationUnits(units), arenaGrowth(growthData, growthCount)
        , entries(entryData, entryCount), publications(publicationData, publicationCount)
        , backingAuthorities(authorityData, authorityCount) {}

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
    if (count > size_t(std::numeric_limits<qint64>::max()) / 2) throw std::bad_alloc();
    const size_t authorityOffset = appendPayload<BackingAuthority>(bytes, 2 * count);
    const size_t units = (bytes + sizeof(AllocationUnit) - 1) / sizeof(AllocationUnit);
    KisMutationStorageAllocator<AllocationUnit> allocator(storage);
    auto *raw = allocator.allocate(units);
    auto *base = reinterpret_cast<char *>(raw);
    try {
        return DataPointer(new (raw) Data(storage, units,
            reinterpret_cast<Entry *>(base + entryOffset), count,
            reinterpret_cast<Publication *>(base + publicationOffset), publicationCount,
            reinterpret_cast<ShardArenaGrowth *>(base + growthOffset), shardCount,
            reinterpret_cast<BackingAuthority *>(base + authorityOffset), 2 * count));
    } catch (...) { allocator.deallocate(raw, units); throw; }
}

bool KisPageMetadataCoordinator::PreparedPublication::Data::capture(
    size_t i, const KisPageTransition &input, const KisImageEpochRootSnapshot *root, QString *error)
{
    auto &entry = entries[i]; auto &output = publications[i];
    auto &shard = *entry.shardOwner; auto &records = shard.records;
    const auto pageIt = shard.pages.find(entry.key());
    const auto reject = [&](const QString &reason) { KisPageStoreDetail::setError(error, reason); return false; };
    if (pageIt == shard.pages.end() || !pageIt->second.revisionCanAdvance()
        || pageIt->second.activity.publicationClaim)
        return reject(QStringLiteral("metadata publication page is unavailable"));
    entry.originalPage = pageIt;
    auto &page = pageIt->second; entry.revision = page.revision;
    if (page.activity.nativeWrite)
        return reject(QStringLiteral("page has an unaccepted native write reservation"));
    if (kind != PreparationKind::Publication) {
        if (page.activity.hasWriter()) return reject(QStringLiteral("page already has a reserved writer"));
        if (!input.writer.isValid() || !input.operation.isValid() || !input.transaction.isValid()
            || !input.baseVersion.isValid() || !input.version.isValid() || !input.target.isValid()
            || !(input.baseVersion.key == entry.key()) || !(input.target.version == input.version))
            return reject(QStringLiteral("write reservation identity is incomplete or targets another page"));
        auto &lease = output.lease.emplace();
        lease.base = input.baseVersion; lease.writer = input.writer; lease.operation = input.operation;
        lease.mode = input.writeMode;
        records.findVersion(input.baseVersion, &lease.baseValue);
        const auto *base = records.version(lease.baseValue);
        shard.publicationVersionInputs += bool(base);
        const bool virtualBase = input.baseVersion.isDefaultPixel() && input.source == KisReplicaHandle{}
            && (!base || (!base->replicaCount && !base->authorityReplica.isValid()))
            && input.writeMode == KisPageWriteMode::DiscardContents;
        if (base ? base->publication != KisPagePublicationState::Historical
                && !(base->publication == KisPagePublicationState::Prepared && base->preparedBy == input.transaction)
            : !virtualBase)
            return reject(QStringLiteral("write base is not visible to the transaction"));
        if (!virtualBase && (!base || !records.findReplicaSlot(lease.baseValue, input.source, &lease.before)))
            return reject(QStringLiteral("write base authority is not a valid exact-generation replica"));
        const auto *before = records.replica(lease.before);
        const bool recoverable = kind == PreparationKind::RecoverableWrite;
        if ((!recoverable && base && !(base->authorityReplica == lease.before))
            || (!virtualBase && (!before || before->validity != KisReplicaValidity::Valid)))
            return reject(QStringLiteral("write base authority is not a valid exact-generation replica"));
        if (!(input.version.generation == page.nextGeneration)
            || input.version.generation.value <= input.baseVersion.generation.value
            || input.version.generation.value == std::numeric_limits<quint64>::max()
            || records.findVersion(input.version))
            return reject(QStringLiteral("write generation is not the next free generation"));
        if (before && before->pinCount == std::numeric_limits<quint32>::max())
            return reject(QStringLiteral("write before-image pin count is exhausted"));
        KisReplicaSlotId occupiedSlot;
        const bool occupied = records.physicalSlots.findExact(input.target.physicalSlotIdentity(), &occupiedSlot);
        if (recoverable) {
            const auto oldSlot = base ? base->authorityReplica : KisReplicaSlotId{};
            const auto *old = records.replica(oldSlot);
            if (!old || !before || oldSlot == lease.before || page.activity.genericHandoff
                || old->validity != KisReplicaValidity::Valid || old->activeOperation.isValid()
                || old->pinCount || old->overflow.isValid() || before->activeOperation.isValid()
                || !(old->layout() == before->layout()) || !(old->layout() == input.target.layout)
                || old->domain() != input.target.domain || !occupied || !(occupiedSlot == oldSlot)
                || old->identity().allocation.generation == std::numeric_limits<quint64>::max()
                || input.target.allocation.generation != old->identity().allocation.generation + 1)
                return reject(QStringLiteral("recoverable write has no idle exact source and independent before"));
            lease.removed = oldSlot;
        } else if (occupied) return reject(QStringLiteral("write allocation token is already in use"));
        addAuthority(input.source, output.beforeAuthority);
        addAuthority(input.target, output.targetAuthority, true);
        output.addition = true;
    } else {
        KisPageVersion selected;
        if (!root || !root->resolve(entry.key(), &selected))
            return reject(QStringLiteral("historical restore has no published source"));
        if (page.activity.hasWriter() || page.mutableCount)
            return reject(QStringLiteral("view publication conflicts with mutable page state"));
        KisVersionSlotId selectedSlot;
        records.findVersion(selected, &selectedSlot);
        const auto *current = records.version(selectedSlot);
        if (!current && !selected.isDefaultPixel())
            return reject(QStringLiteral("historical restore has no published source"));
        records.findVersion(input.version, &output.targetValue);
        const auto *target = records.version(output.targetValue);
        shard.publicationVersionInputs += bool(current) + bool(target && target != current);
        KisReplicaHandle targetAuthority;
        if (!input.version.isValid()) return reject(QStringLiteral("view publication target is invalid"));
        if (input.kind == KisPageTransitionKind::ReplaceDefaultPixel) {
            if (!input.version.isDefaultPixel() || !selected.isDefaultPixel()
                || (input.target.isValid() && !(input.target.version == input.version))
                || (target && target->publication != KisPagePublicationState::Historical))
                return reject(QStringLiteral("default replacement identity or source is invalid"));
            targetAuthority = target ? records.projectReplica(target->authorityReplica) : input.target;
            if (target && !(targetAuthority == input.target))
                return reject(QStringLiteral("default replacement authority is mismatched"));
            if (input.target.isValid()) {
                KisReplicaSlotId occupiedSlot;
                const bool occupied = records.physicalSlots.findExact(input.target.physicalSlotIdentity(), &occupiedSlot);
                if (occupied && (!target || !(occupiedSlot == target->authorityReplica)))
                    return reject(QStringLiteral("default replacement allocation token is already in use"));
            }
        } else {
            if (!target && !input.version.isDefaultPixel())
                return reject(QStringLiteral("historical restore target is unavailable"));
            if (target) {
                const bool virtualDefault = target->version.isDefaultPixel()
                    && !target->authorityReplica.isValid() && !target->replicaCount;
                if (target->publication != KisPagePublicationState::Historical
                    || (!target->authorityReplica.isValid() && !virtualDefault))
                    return reject(QStringLiteral("historical restore target is unavailable"));
                targetAuthority = records.projectReplica(target->authorityReplica);
            }
        }
        output.addition = !target && !(selected == input.version)
            && (targetAuthority.isValid() || input.version.generation.value != 1);
        // Same identity is one frozen input, consumed by both real readers.
        addAuthority(current ? records.projectReplica(current->authorityReplica) : KisReplicaHandle{}, output.beforeAuthority);
        if (target == current && target && output.beforeAuthority != NoAuthority) {
            output.targetAuthority = output.beforeAuthority;
            backingAuthorities[output.beforeAuthority].second = &output.targetAuthority;
        } else addAuthority(targetAuthority, output.targetAuthority, output.addition);
        if (selected.isDefaultPixel() && !(selected == input.version)) output.displacedDefault = selectedSlot;
    }
    if (output.addition) {
        // An original initial identity can already be named by a root. It is
        // filled in place; no second Value or arena slot is needed.
        const auto *initial = !page.hasMetadata() ? shard.arenas.versions.get(page.initialIdentity) : nullptr;
        if (initial && initial->initialIdentity && initial->version == entry.version)
            output.targetValue = initial->slot;
        addArenaDemand(i, {quint64(!output.targetValue.isValid()), 0, 0}, 1,
            qsizetype(authority(output.targetAuthority).isValid()));
        ++shard.publicationAdditionRecordsPrepared;
        if (kind == PreparationKind::Publication) ++shard.publicationCommittedValuesPrepared;
    }
    return true;
}

bool KisPageMetadataCoordinator::PreparedPublication::Data::freezeAuthorities(QString *error)
{
    const auto introduced = std::count_if(backingAuthorities.begin(), backingAuthorities.end(),
        [](const auto &row) { return row.introduced; });
    if (introduced > 1) {
        // Validate only actual newly introduced physical targets, using the
        // same candidate storage. Existing aliases/unchanged inputs need no
        // second set/index. A duplicate must fail before any row is installed.
        std::sort(backingAuthorities.begin(), backingAuthorities.end(), [](const auto &a, const auto &b) {
            if (a.introduced != b.introduced) return a.introduced > b.introduced;
            const auto &x = a.replica; const auto &y = b.replica;
            return std::tie(x.provider.value, x.providerEpoch.value, x.allocation.slot)
                < std::tie(y.provider.value, y.providerEpoch.value, y.allocation.slot);
        });
        for (size_t i = 1; i < size_t(introduced); ++i)
            if (backingAuthorities[i-1].replica.physicalSlotIdentity() == backingAuthorities[i].replica.physicalSlotIdentity()) {
                KisPageStoreDetail::setError(error, QStringLiteral("publication introduces a repeated physical allocation"));
                return false;
            }
    }
    std::sort(backingAuthorities.begin(), backingAuthorities.end(), [](const auto &a, const auto &b) {
        return MetadataKeyLess{}(a.replica.version, b.replica.version);
    });
    for (size_t i = 0; i < backingAuthorities.size(); ++i) {
        auto &row = backingAuthorities[i];
        *row.first = i;
        if (row.second) *row.second = i;
    }
    return true;
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

size_t KisPageMetadataCoordinator::PreparedPublication::Data::claimRows(
    std::optional<QMutexLocker<QMutex>> *held)
{
    qsizetype claimed = 0;
    std::optional<QMutexLocker<QMutex>> local;
    auto &gate = held ? *held : local;
    Q_ASSERT(!held || (!entries.empty() && batchEnd(0, entries.size()) == entries.size()));
    // All claims are checked before changing any state. Never hold multiple
    // shard locks; a competing mutation is rejected/deferred while claimed.
    // Claims live only inside this synchronous call, with no provider work,
    // root construction, callback or external completion wait inside the
    // claim interval. Individual shard mutex acquisition can still wait.
    for (size_t first = 0; first < entries.size();) {
        const size_t end = batchEnd(first, entries.size());
        gate.emplace(&entries[first].shardOwner->mutex);
        for (size_t i = first; i < end; ++i) {
            auto &entry = entries[i];
            auto page = entry.originalPage;
            if (!entry.shardOwner->canMutate(entry.key(), page->second))
                break;
            if (page->second.revision != entry.revision)
                break;
            const auto &publication = publications[i];
            if (publication.addition) {
                if (entry.shardOwner->records.findVersion(entry.version)) break;
                const auto &target = authority(publication.targetAuthority);
                if (target.isValid()) {
                    KisReplicaSlotId occupiedSlot;
                    const bool occupied = entry.shardOwner->records.physicalSlots.findExact(target.physicalSlotIdentity(), &occupiedSlot);
                    const auto expected = publication.lease ? publication.lease->removed : KisReplicaSlotId{};
                    if (occupied ? !(occupiedSlot == expected) : expected.isValid()) break;
                }
                if (publication.targetValue.isValid()
                    && (page->second.hasMetadata() || !(page->second.initialIdentity == publication.targetValue))) break;
            }
            Q_ASSERT(entry.activityReservation || (!publication.addition && !publication.lease));
            // Only actual metadata edits retain activity storage through their
            // deferred cleanup. A pure view neither changes nor erases it.
            if (!held) page->second.activity.publicationClaim = this;
            ++claimed;
        }
        if (!held || claimed != qsizetype(end)) gate.reset();
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
    bool mutation, bool recoverable, bool privateWrite, KisImageEpochId epoch,
    std::optional<QMutexLocker<QMutex>> *held)
{
    Q_UNUSED(epoch);
    std::optional<QMutexLocker<QMutex>> local;
    auto &gate = held ? *held : local;
    Q_ASSERT(publications.size() == entries.size());
    // A new Value has at most its one actual replica. This borrowed adapter
    // feeds the canonical record insertion, never a copied working image.
    struct Replica {
        const KisReplicaHandle &replica;
        KisReplicaValidity validity;
        KisPageOperationId activeOperation;
        quint32 pinCount = 0;
        std::array<KisPageLeaseId, 0> readLeases;
        std::array<KisCompletionTicket, 0> pendingLastUses;
    };
    struct ReplicaRange {
        const Replica *data;
        size_t count;
        bool empty() const { return !count; }
        size_t size() const { return count; }
        const Replica *begin() const { return data; }
        const Replica *end() const { return data + count; }
    };
    struct Addition {
        const KisPageVersion &version;
        KisPagePublicationState publication;
        KisPageTransactionId preparedBy;
        const KisReplicaHandle &authority;
        quint32 readEdges;
        ReplicaRange replicas;
    };
    for (size_t first = 0; first < entries.size();) {
        const size_t end = batchEnd(first, entries.size());
        if (!gate) gate.emplace(&entries[first].shardOwner->mutex);
        for (size_t i = first; i < end; ++i) {
            auto &entry = entries[i]; auto &publication = publications[i];
            auto &shard = *entry.shardOwner; auto &page = entry.originalPage->second;
            Q_ASSERT(held || page.activity.publicationClaim == this);
            if (auto *lease = publication.lease ? &*publication.lease : nullptr) {
                if (recoverable) {
                    auto *base = shard.records.version(lease->baseValue);
                    const bool removedBinding = base->native.nativeReplica == lease->removed;
                    lease->before = shard.records.replaceHandoffAuthority(base, lease->removed, lease->before);
                    if (removedBinding) {
                        entry.releasedReadBinding = std::move(base->native.nativeBinding);
                        --shard.arenas.cpuReadBindings;
                    }
                    ++shard.publicationVersionInstalls;
                }
                if (lease->before.isValid()) ++shard.records.replica(lease->before)->pinCount;
            }
            if (publication.addition) {
                auto &reservation = arenaGrowth[entry.arenaGrowthIndex].reservations;
                const auto &target = authority(publication.targetAuthority);
                const Replica replica{target, mutation ? KisReplicaValidity::Allocated : KisReplicaValidity::Valid, {}, 0, {}, {}};
                const KisReplicaHandle empty;
                const Addition addition{entry.version,
                    mutation ? KisPagePublicationState::Unpublished : KisPagePublicationState::Historical,
                    {}, mutation ? empty : target, 0, {&replica, size_t(target.isValid())}};
                publication.targetValue = shard.records.insertReserved(&page, addition, &reservation);
                Q_ASSERT(publication.targetValue.isValid());
                ++shard.publicationAdditionRecordsTransferred;
                ++shard.publicationVersionInstalls;
                if (!mutation) ++shard.publicationCommittedValuesInstalled;
            }
            if (publication.lease) {
                Q_ASSERT(privateWrite || recoverable);
                const auto &lease = *publication.lease;
                auto image = shard.activityImage(entry.key(), page);
                image.writerToken = lease.writer; image.writerOperation = lease.operation;
                image.writerTransaction = transaction.id; image.writerMode = lease.mode;
                image.writerPhase = KisPageWriterPhase::Writable;
                image.writerBaseAuthority = lease.before;
                image.writerDefaultGeneration = lease.before.isValid() ? KisPageGeneration{} : lease.base.generation;
                image.writerDefaultRevision = lease.before.isValid() ? 0 : lease.base.defaultPixelRevision;
                image.writerTarget = kisInlineReplicaSlot(publication.targetValue);
                page.nextGeneration = {entry.version.generation.value + 1};
                shard.installActivityImage(entry.key(), &page, image);
            }
            if (auto *displaced = shard.records.version(publication.displacedDefault))
                shard.records.reconsiderHistory(displaced);
            // ViewRoot owns pure visibility. Metadata revision tracks only
            // actual new cells/writer state; a cache reclaim hint is not a write.
            if (publication.addition || publication.lease) ++page.revision;
            ++(mutation ? shard.installedMutationPages : shard.installedPublicationPages);
            if (!held) page.activity.publicationClaim = nullptr;
        }
        if (!held) gate.reset();
        first = end;
    }
    for (auto &growth : arenaGrowth) {
        if (!gate) gate.emplace(&growth.shardOwner->mutex);
        growth.reservations.cancel(&growth.shardOwner->records);
        growth.releasedBlocks = growth.shardOwner->arenas.takeEmptyBlocks(&growth.shardOwner->budgetCharge);
        if (!held) gate.reset();
    }
    gate.reset();
    installed = true;
    owner.reset();
}

bool KisPageMetadataCoordinator::PreparedPublication::CpuResources::before(
    const KisPageCpuPublicationResource &a, const KisPageCpuPublicationResource &b)
{
    // Immutable logical key orders gates; address distinguishes actual
    // Resources even when two providers expose identical external identities.
    const auto &ka = a.binding->m_key, &kb = b.binding->m_key;
    if (ka.surface.value != kb.surface.value) return ka.surface.value < kb.surface.value;
    if (ka.page.row != kb.page.row) return ka.page.row < kb.page.row;
    if (ka.page.column != kb.page.column) return ka.page.column < kb.page.column;
    return std::less<KisCpuResidentBinding *>{}(a.binding, b.binding);
}

KisPageMetadataCoordinator::PreparedPublication::CpuResources
KisPageMetadataCoordinator::PreparedPublication::prepareCpuResources(QString *error)
{
    CpuResources result;
    using Resource = KisPageCpuPublicationResource;
    using It = KisPublicationMemberIterator<Resource, &KisPagePublicationChange::resourceAcceptance>;
    It nativeBegin(m_rows.begin());
    auto *cold = data && !data->publications.empty() ? data->publications.begin() : nullptr;
    const auto column = +[](Data::Publication &row) -> Resource & { return row.resourceAcceptance; };
    auto coldBegin = boost::make_transform_iterator(cold, column);
    size_t nativeTotal = 0, coldTotal = 0;
    bool nativeOrdered = true, coldOrdered = true;
    const auto append = [&](Resource resource, auto begin, size_t &count, bool &ordered) {
        if (!resource.binding) return;
        if (ordered && count && CpuResources::before(resource, begin[ptrdiff_t(count - 1)])) ordered = false;
        begin[ptrdiff_t(count++)] = resource;
    };
    const auto capture = [](const ShardRecordStore &records, const KisPageValueCell *value) {
        Resource resource;
        if (value && value->native.nativeBinding && value->native.nativeReplica == value->authorityReplica) {
            const auto *replica = records.replica(value->native.nativeReplica);
            if (replica) resource = {value->native.nativeBinding.get(), value, replica};
        }
        return resource;
    };
    // Store admission excludes authority/placement edits here. Original
    // candidates retain the Values and any displaced references through the
    // synchronous installer. Read pins only change the Resource gate state.
    // Compact only actual Resources into the original borrowed columns. The
    // replacement/metadata inputs are untouched; unused tails are not read.
    for (auto &row : m_rows) {
        if (row.replacement.removal) continue;
        Resource resource;
        if (const auto *native = row.replacement.native) {
            // The private result already owns its final primary replica.
            if (!native->hasValue()) {
                KisPageStoreDetail::setError(error, QStringLiteral("private Value missing before Resource preparation"));
                return {};
            }
            if (native->binding()) resource = {
                native->binding().get(), native->value, &native->value->primaryReplica};
        } else {
            auto &work = row.metadataAcceptance;
            auto *shard = static_cast<MetadataShard *>(work.shard);
            if (!shard) continue;
            QMutexLocker lock(&shard->mutex);
            resource = capture(shard->records, shard->records.version(work.replacementSlot));
        }
        append(resource, nativeBegin, nativeTotal, nativeOrdered);
    }
    if (data) for (size_t i = 0; i < data->publications.size(); ++i) {
        auto &publication = data->publications[i];
        const auto &entry = data->entries[i];
        QMutexLocker lock(&entry.shardOwner->mutex);
        append(capture(entry.shardOwner->records,
            entry.shardOwner->records.version(publication.targetValue)),
            coldBegin, coldTotal, coldOrdered);
    }
    // Canonical rows already have key order. Arbitrary cold/public inputs
    // retain a bounded sort, with the SAME global order as native acceptance.
    if (!nativeOrdered) std::sort(nativeBegin, nativeBegin + ptrdiff_t(nativeTotal), CpuResources::before);
    if (!coldOrdered) std::sort(coldBegin, coldBegin + ptrdiff_t(coldTotal), CpuResources::before);
    result.m_rows = m_rows;
    result.m_cold = data.get();
    result.m_nativeCount = nativeTotal;
    result.m_coldCount = coldTotal;
    result.m_valid = true;
    return result;
}

bool KisPageMetadataCoordinator::PreparedPublication::CpuResources::accept(
    void *context, bool (*accept)(void *), QString *error) const
{
    if (!m_valid || !accept) return false;
    using Resource = KisPageCpuPublicationResource;
    using It = KisPublicationMemberIterator<Resource, &KisPagePublicationChange::resourceAcceptance>;
    It nativeBegin(m_rows.begin());
    auto *cold = m_cold && !m_cold->publications.empty() ? m_cold->publications.begin() : nullptr;
    const auto column = +[](Data::Publication &row) -> Resource & { return row.resourceAcceptance; };
    auto coldBegin = boost::make_transform_iterator(cold, column);
    const auto nativeTotal = m_nativeCount, coldTotal = m_coldCount;
    if (!nativeTotal && !coldTotal) return accept(context);

    size_t nativeCount = 0, coldTaken = 0;
    KisCpuResidentBinding *last = nullptr;
    // Merge the two original columns, rather than allocating a third array
    // or taking two differently ordered sets of Resource gates.
    const auto unlock = qScopeGuard([&] {
        KisCpuResidentBinding *released = nullptr;
        while (nativeCount || coldTaken) {
            const bool fromNative = !coldTaken || (nativeCount
                && !before(nativeBegin[ptrdiff_t(nativeCount - 1)], coldBegin[ptrdiff_t(coldTaken - 1)]));
            const auto &resource = fromNative ? *(nativeBegin + ptrdiff_t(--nativeCount))
                : *(coldBegin + ptrdiff_t(--coldTaken));
            if (resource.binding != released) {
                resource.binding->m_mutex.unlock();
                released = resource.binding;
            }
        }
    });
    while (nativeCount < nativeTotal || coldTaken < coldTotal) {
        const bool fromNative = coldTaken == coldTotal || (nativeCount < nativeTotal
            && !before(coldBegin[ptrdiff_t(coldTaken)], nativeBegin[ptrdiff_t(nativeCount)]));
        const auto &resource = fromNative ? *(nativeBegin + ptrdiff_t(nativeCount++))
            : *(coldBegin + ptrdiff_t(coldTaken++));
        if (resource.binding != last) {
            resource.binding->m_mutex.lock();
            last = resource.binding;
        }
        if (resource.binding->m_retired
            || !(resource.binding->allocationIdentityLocked() == resource.replica->identity())
            || !resource.binding->matchesVersionLocked(resource.value->version)
            || resource.binding->m_domain != resource.replica->domain()
            || !(resource.binding->m_layout == resource.replica->layout())) {
            KisPageStoreDetail::setError(error, QStringLiteral("CPU Resource changed before publication acceptance"));
            return false;
        }
    }
    return accept(context);
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
            if (found->superseded.version == version) {
                const auto source = found->supersededReplica();
                if (source.version == version) {
                    if (detached) *detached = found->detachesSuperseded;
                    if (idleResult && found->detachesSuperseded) *idleResult = &found->acceptedIdle;
                    return source;
                }
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
            return MetadataKeyLess{}(entry.replica.version, key);
        });
    if (found != data->backingAuthorities.end() && found->replica.version == version)
        return found->replica;
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
    for (const auto &row : m_rows) if (row.detachesSuperseded) {
        const auto source = row.supersededReplica();
        if (source.isValid()) visit(context, source, row.acceptedIdle);
    }
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
                                               const KisImageEpochRootSnapshot &currentRoot,
                                               KisImageEpochId minimumEpoch,
                                               const KisPageSnapshotArray<KisPageTransition> &transitions,
                                               QString *error) const
{
    return preparePublicationImpl(transaction, minimumEpoch, transitions.constData(), transitions.size(), false, error,
        PreparationKind::Publication, nullptr, nullptr, false, &currentRoot);
}

KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::prepareRestoration(const KisImageEpochRootSnapshot &currentRoot, KisImageEpochId minimumEpoch,
                                               const KisPageSnapshotArray<KisPageTransition> &transitions,
                                               QString *error) const
{
    return preparePublicationImpl({}, minimumEpoch, transitions.constData(), transitions.size(), true, error,
        PreparationKind::Publication, nullptr, nullptr, false, &currentRoot);
}

KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::preparePublication(const KisPageTransaction &transaction,
                                               const KisImageEpochRootSnapshot &currentRoot,
                                               KisImageEpochId minimumEpoch,
                                               const PublicationChange *changes, qsizetype count,
                                               QString *error, bool detachUnprotectedCurrent) const
{
    return preparePublicationImpl(transaction, minimumEpoch, nullptr, count, false, error,
                                  PreparationKind::Publication, nullptr, changes,
                                  detachUnprotectedCurrent, &currentRoot);
}

KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::prepareRestoration(const KisImageEpochRootSnapshot &currentRoot, KisImageEpochId minimumEpoch,
                                               const KisPageVersion *versions, qsizetype count,
                                               QString *error) const
{
    return preparePublicationImpl({}, minimumEpoch, nullptr, count, true, error,
                                  PreparationKind::Publication, versions, nullptr, false, &currentRoot);
}

KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::prepareAcceptance(const KisPageTransaction &transaction,
                                            KisPagePublicationRows rows,
                                            QString *error, KisImageEpochId minimumEpoch, bool detachCurrent,
                                            const PublicationChange *generic, qsizetype genericCount,
                                            const KisImageEpochRootSnapshot *currentRoot) const
try {
    PreparedPublication result;
    auto *d = m_core.load(std::memory_order_acquire);
    const size_t count = rows.size();
    const bool published = minimumEpoch.isValid();
    result.m_minimumEpoch = minimumEpoch;
    if (!d || d->shards.empty() || !transaction.isValid() || (count && !rows)
        || (published && (!currentRoot || !currentRoot->isValid()
            || minimumEpoch.value <= currentRoot->epoch().value
            || minimumEpoch.value <= transaction.baseEpoch.value))) return result;
    if (genericCount) {
        auto genericPreparation = preparePublicationImpl(transaction, minimumEpoch, nullptr, genericCount,
            false, error, PreparationKind::Publication, nullptr, generic, false, currentRoot);
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
            input.superseded = {}; input.supersededAuthority = nullptr; input.supersededValue = nullptr;
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
                && input.superseded.native && input.hasSupersededAuthority()
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
                KisPageVersion selected;
                KisPageViewValue *value = nullptr;
                if (currentRoot->resolveVersion(work.input->replacement.key(), &selected, &value)
                    == KisImageEpochRootSnapshot::ResolutionKind::Missing) return {};
                if (value) {
                    // Explicit roots already name the original Value. Verify
                    // its owning arena directly instead of looking up its key.
                    if (shard->arenas.versions.get(value->slot) != value) return {};
                    work.input->superseded.metadataSlot = value->slot;
                } else if (!shard->records.findVersion(selected, &work.input->superseded.metadataSlot)
                    && !selected.isDefaultPixel()) return {};
                work.input->superseded.version = selected;
                if (!page || !shard->canMutate(work.input->replacement.key(), *page)
                    || !shard->validPublication(*page, *work.input, work, transaction.id, minimumEpoch)) return {};
                const auto *current = shard->records.version(work.input->superseded.metadataSlot);
                if (native) {
                    ++shard->preparedPublicationPages;
                    // The old committed Value, rather than the writer's pixel
                    // source, owns this fee/terminal fact. The original root
                    // retains its memory through installation and cleanup.
                    if (value && current && current->authorityReplica == kisInlineReplicaSlot(value->slot)) {
                        work.input->supersededValue = current;
                    } else if (!current || !current->authorityReplica.isValid()) {
                        static const KisReplicaHandle empty;
                        work.input->supersededAuthority = &empty;
                    } else ++releases; // Real non-inline/default-cache authority.
                }
                work.input->detachesSuperseded = detachCurrent && current && shard->records.canDetachSelected(current);
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
            external |= !input.replacement.native || (published && !input.hasSupersededAuthority());
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
                    && (!published || rows[batch].metadataAcceptance.input->hasSupersededAuthority())) ++batch;
                if (batch == end) break;
                const size_t batchEnd = std::min(end, batch + 16);
                QMutexLocker lock(&shard->mutex);
                for (size_t i = batch; i < batchEnd; ++i) {
                    auto &work = rows[i].metadataAcceptance;
                    auto &input = *work.input;
                    if (input.replacement.native && (!published || input.hasSupersededAuthority())) continue;
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
                            : !shard->validDetachment(*page, input, transaction.id))) return {};
                    if (!published) input.detachesSuperseded = input.detachesSuperseded
                        && shard->records.canDetachPrepared(input.mutationVersion());
                    if (published || input.detachesSuperseded) {
                        if (published) {
                            auto *references = result.m_acceptanceReferences->appendCanonical();
                            if (!input.replacement.native)
                                work.prepared = static_cast<MetadataReleasedReferences *>(references);
                            const auto *current = shard->records.version(work.input->superseded.metadataSlot);
                            if (current) references->source = shard->records.projectReplica(current->authorityReplica);
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
                    if (!published) ++shard->publicationCommittedValuesPrepared;
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
    KisImageEpochId epoch, bool detachCurrent, const PublicationChange *generic, qsizetype genericCount,
                                            const KisImageEpochRootSnapshot *currentRoot) const
{
    auto result = prepareAcceptance(transaction, KisPagePublicationRows{owned.data(), owned.size()},
                                    error, epoch, detachCurrent, generic, genericCount, currentRoot);
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
                : !shard->validDetachment(*page, input, transaction.id)) break;
            const auto *current = published ? shard->records.version(work.input->superseded.metadataSlot) : nullptr;
            const auto &sourceVersion = published ? input.superseded.version : version;
            const bool available = !input.detachesSuperseded
                || (published ? shard->records.canDetachSelected(current)
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
        // A pure cold batch in one bounded shard has no unlock between its
        // final guards and edits. Keep that gate; no temporary claims needed.
        const bool coldSingleWindow = !count && !data->entries.empty()
            && data->batchEnd(0, data->entries.size()) == data->entries.size();
        const size_t genericClaimed = data->claimRows(coldSingleWindow ? &shardLock : nullptr);
        if (genericClaimed != data->entries.size()) {
            if (!coldSingleWindow) data->releaseClaims(genericClaimed);
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
            kind == PreparationKind::PrivateWrite, epoch, coldSingleWindow ? &shardLock : nullptr);
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
                                                   bool detachUnprotectedCurrent,
                                                   const KisImageEpochRootSnapshot *currentRoot) const
try {
    const bool mutation = kind != PreparationKind::Publication;
    const bool recoverable = kind == PreparationKind::RecoverableWrite;
    const bool privateWrite = kind == PreparationKind::PrivateWrite;
    PreparedPublication result;
    auto *d = m_core.load(std::memory_order_acquire);
    if (!d || count < 0 || (count && !transitions && !versions && !changes)
        || (!restoration && !transaction.isValid())
        || (!mutation && (!currentRoot || !currentRoot->isValid() || !minimumEpoch.isValid()
            || minimumEpoch.value <= currentRoot->epoch().value
            || minimumEpoch.value <= transaction.baseEpoch.value))) {
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
                detachUnprotectedCurrent, sparse.data(), qsizetype(sparse.size()), currentRoot);
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
    data->owner = d->publicationOwner;
    data->transaction = transaction;
    data->minimumEpoch = minimumEpoch;
    data->kind = kind;
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
        // Decode only the actual cold input. The independent oracle is not
        // used by production preparation; no input lives beyond this call.
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
        data->publications.push_back(PreparedPublication::Data::Publication{});
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
    size_t countedPrepared = 0;
    const auto rollbackPreparationCounts = qScopeGuard([&] {
        if (!data) return;
        for (size_t first = 0; first < countedPrepared;) {
            const size_t end = data->batchEnd(first, countedPrepared);
            QMutexLocker locker(&data->entries[first].shardOwner->mutex);
            for (size_t i = first; i < end; ++i)
                --(mutation ? data->entries[i].shardOwner->preparedMutationPages
                            : data->entries[i].shardOwner->preparedPublicationPages);
            first = end;
        }
    });

    // Capture original positions and actual effects under one bounded shard
    // window. Existing Values, replicas and protection never enter a working
    // image; only genuine new identities contribute arena/index demand.
    for (size_t first = 0; first < data->entries.size();) {
        const size_t end = data->batchEnd(first, data->entries.size());
        QMutexLocker locker(&data->entries[first].shardOwner->mutex);
        for (size_t i = first; i < end; ++i) {
            KisPageTransition input;
            const auto &transition = transitionAt(data->entries[i].inputOrdinal, input);
            if (!data->capture(i, transition, currentRoot, error)) return result;
        }
        first = end;
    }
    if (!data->freezeAuthorities(error)) return result;
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

    // Only real edits need generic/activity storage. Pure view observations
    // retain their original shard/dynamic guards without creating write activity.
    for (size_t first = 0; first < data->entries.size();) {
        const size_t end = data->batchEnd(first, data->entries.size());
        QMutexLocker locker(&data->entries[first].shardOwner->mutex);
        for (size_t i = first; i < end; ++i) {
            auto &entry = data->entries[i];
            const auto page = entry.originalPage;
            bool stillValid = entry.shardOwner->canMutate(entry.key(), page->second)
                && page->second.revision == entry.revision;
            bool genericReady = true;
            if (stillValid && data->publications[i].lease) {
                try { entry.shardOwner->activities.try_emplace(entry.key()); }
                catch (const std::bad_alloc &) { genericReady = false; }
            }
            const bool editsMetadata = data->publications[i].addition || data->publications[i].lease.has_value();
            if (!stillValid || !genericReady
                || (editsMetadata && !entry.shardOwner->reservePublicationActivity(page->second))) {
                KisPageStoreDetail::setError(error, QStringLiteral("metadata changed while reserving publication activity"));
                result.m_failure = !stillValid
                    ? PreparedPublication::Failure::Conflict : PreparedPublication::Failure::Storage;
                return result;
            }
            entry.activityReservation = editsMetadata;
            ++(mutation ? entry.shardOwner->preparedMutationPages : entry.shardOwner->preparedPublicationPages);
            ++countedPrepared;
        }
        first = end;
    }
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
        {
            QMutexLocker notificationLock(&core->reclaimMutex);
            core->attached.store(false, std::memory_order_release);
        }
        for (const auto &shard : core->shards) {
            QMutexLocker lock(&shard->mutex);
            shard->records.notificationSource = nullptr;
        }
        core->clearReclaims();
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

void KisPageMetadataCoordinator::attachRetirementOwner(
    void *context, AcceptRetirement accept, DeliverRetirement deliver)
{
    QMutexLocker locker(&m_configurationMutex);
    Q_ASSERT(!m_core.load(std::memory_order_relaxed));
    Q_ASSERT(context && accept && deliver);
    Q_ASSERT(!m_retirementContext);
    m_retirementContext = context;
    m_acceptRetirement = accept;
    m_deliverRetirement = deliver;
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
    if (facts->nativeBinding && (facts->nativeBinding.get() != binding.get() || !(facts->nativeReplica == slot))) {
        if (auto *old = shard->records.replica(facts->nativeReplica)) old->detachResource(version->version);
    }
    if (!facts->nativeBinding) ++shard->arenas.cpuReadBindings;
    facts->nativeBinding.swap(binding);
    facts->nativeReplica = slot;
    shard->records.replica(slot)->setHandle(replica, facts->nativeBinding.get());
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
    auto *selected = shard.records.replica(facts->nativeReplica);
    if (facts->nativeBinding && (!selected || replicaHandle(*selected, replica.version) == replica)
        && (!expected || facts->nativeBinding.get() == expected)) {
        if (selected) selected->detachResource(version.version);
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
    const quint32 *order, size_t &next, size_t count)
{
    auto *core = m_core.load(std::memory_order_acquire);
    if (!count) return 0;
    Q_ASSERT(core && values && order && next != std::numeric_limits<quint32>::max());
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
        Q_ASSERT(value->readEdges && shard->arenas.versions.get(value->slot) == value);
        if (!--value->readEdges) {
            shard->records.reconsiderHistory(value);
            if (!value->indexed && !value->frameHeld && value->detachLifetime()) {
                const auto page = value->initialIdentity ? shard->pages.find(value->version.key) : shard->pages.end();
                if (value->initialIdentity && page != shard->pages.end() && !page->second.hasMetadata()
                    && page->second.initialIdentity == value->slot) page->second.initialIdentity = {};
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
                storage, candidate->budgetAuthority, storage, candidate.get()));
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

bool KisPageMetadataCoordinator::registerImplicitPage(const KisPageVersion &currentDefault, QString *error) try
{
    auto *core = m_core.load(std::memory_order_acquire);
    auto *shard = Private::shardFor(core, currentDefault.key);
    const auto fail = [&](const QString &reason) { KisPageStoreDetail::setError(error, reason); return false; };
    if (!shard || !currentDefault.isValid() || !currentDefault.isDefaultPixel()
        || currentDefault.generation.value == std::numeric_limits<quint64>::max())
        return fail(QStringLiteral("implicit page identity is invalid"));
    decltype(shard->pages) prepared(shard->pages.get_allocator());
    QMutexLocker lock(&shard->mutex);
    auto page = shard->pages.find(currentDefault.key);
    if (page == shard->pages.end()) {
        lock.unlock();
        prepared.try_emplace(currentDefault.key);
        lock.relock();
        page = shard->pages.find(currentDefault.key);
    }
    if (page != shard->pages.end() && page->second.hasMetadata()) {
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    auto &control = page == shard->pages.end() ? prepared.begin()->second : page->second;
    // A real initial manifest cell must be filled by initialization. This
    // runtime path cannot discard or reinterpret its outstanding root edge.
    if (control.initialIdentity.isValid())
        return fail(QStringLiteral("implicit page conflicts with pending initialization"));
    control.nextGeneration = KisPageGeneration{currentDefault.generation.value + 1};
    control.registered = true;
    if (page == shard->pages.end()) shard->pages.insert(prepared.extract(prepared.begin()));
    core->registeredPages.fetchAndAddRelease(1);
    KisPageStoreDetail::setError(error, {});
    return true;
} catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("implicit page control storage was refused"));
    return false;
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

bool KisPageMetadataCoordinator::pageSnapshot(const KisPageKey &key, const KisImageEpochRootSnapshot &view,
                                            KisPageStateSnapshot *snapshot) const
{
    MetadataShard *shard = Private::shardFor(m_core.load(std::memory_order_acquire), key);
    if (!shard || !snapshot)
        return false;
    QMutexLocker locker(&shard->mutex);
    const auto pageIt = shard->pages.find(key);
    if (pageIt == shard->pages.cend() || !pageIt->second.hasMetadata())
        return false;
    auto observed = shard->snapshot(key, pageIt->second, view);
    if (!observed.key.isValid()) return false;
    *snapshot = std::move(observed);
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
    if (!record && !base.isDefaultPixel()) {
        KisPageStoreDetail::setError(error, QStringLiteral("write base version is absent"));
        return MutationBaseSelection::Absent;
    }
    if (record && !(record->version == base))
        return reject(QStringLiteral("write base location selects another version"));
    ++shard->mutationBaseVersionInputs;
    if (kisOnPageStoreReclamationThread()) ++shard->backgroundMutationBaseVersionInputs;
    if (page->second.activity.hasWriter())
        return reject(QStringLiteral("write page already has a writer"));
    const KisPageVersion target{base.key, page->second.nextGeneration};
    if (!target.isValid()) return reject(QStringLiteral("write generation is unavailable"));
    const auto *authority = record ? shard->records.replica(record->authorityReplica) : nullptr;
    const bool virtualDefault = base.isDefaultPixel() && (!record
        || (!record->replicaCount && !authority && record->publication == KisPagePublicationState::Historical));
    if (!authority && !virtualDefault)
        return reject(QStringLiteral("write base has no exact authority"));

    // There is one output carrier. Actual source and accepted-location facts
    // are created in it, not copied through a second metadata projection.
    Q_ASSERT(!selection->write.source.version.isValid() && !selection->layout
        && !selection->surfaceLayout);
    auto &write = selection->write;
    write.source.version = base;
    write.version = target;
    write.baseWasPrepared = record && record->publication == KisPagePublicationState::Prepared;
    selection->base = {shard, &*page, shardIndex, selectedSlot};
    if (record) selection->layout = record->native.descriptor;
    if (authority) {
        const auto binding = record->native.nativeReplica == record->authorityReplica
            ? record->native.nativeBinding : std::shared_ptr<KisCpuResidentBinding>{};
        write.source = authority->writeSource(base, binding);
        if (!write.source.isValid())
            return reject(QStringLiteral("write base has no exact authority"));
    }
    if (discoverBefore && !base.isDefaultPixel() && record && record->replicaCount > 1
        && authority && authority->validity == KisReplicaValidity::Valid
        && !authority->activeOperation.isValid() && !authority->pinCount
        && !authority->overflow.isValid()) {
        const auto &layout = write.source.layout();
        const auto sourceIdentity = authority->identity();
        for (auto slot = record->firstReplica; slot.isValid();) {
            const auto *replica = shard->records.replica(slot);
            // Compare the original record. Only the chosen before-image
            // needs a full handle for the later real physical acquisition.
            if (replica->validity == KisReplicaValidity::Valid && !replica->activeOperation.isValid()
                && replica->domain() == KisPageAccessDomain::CpuRam && replica->layout() == layout) {
                const auto candidateIdentity = replica->identity();
                if (!(candidateIdentity.provider == sourceIdentity.provider
                    && candidateIdentity.providerEpoch == sourceIdentity.providerEpoch
                    && candidateIdentity.allocation.slot == sourceIdentity.allocation.slot)) {
                    selection->recoverableBefore.emplace(replica->writeSource(base, {}));
                    break;
                }
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
        KisVersionSlotId slot;
        const auto *version = shard->records.findVersion(identity, &slot) ? shard->records.version(slot) : nullptr;
        auto next = shard->records.versionInfo<VersionInfo>(version);
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
                        const bool last = !value->readEdges && value->detachLifetime();
                        if (!last) value->primaryReplica.detachResource(value->version);
                        released[end].descriptor.swap(value->native.descriptor);
                        released[end].binding.swap(value->native.nativeBinding);
                        if (last) {
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

bool KisPageMetadataCoordinator::preparePrivateValue(const KisPageWriteSelection &selection,
    KisPagePrivateWrite *native, QString *error)
try {
    const auto &write = selection.write;
    const auto &location = selection.base;
    auto *core = m_core.load(std::memory_order_acquire);
    if (!core || core->shards.empty() || !write.version.isValid() || !write.transaction.isValid()
        || !native || native->reserved || native->location.value.isValid()
        || !location.node || location.shardIndex >= core->shards.size()
        || location.shard != core->shards[location.shardIndex].get()) return false;
    const size_t index = location.shardIndex;
    auto *shard = core->shards[index].get();
    const auto storage = core->budgetAuthority->storage<char>();
    native->edit.retain(core->shards[index], storage);
    MetadataArenaGrowth growth(storage.borrowed());
    QMutexLocker lock(&shard->mutex);
    auto *node = static_cast<decltype(shard->pages)::value_type *>(location.node);
    if (!(node->first == write.version.key) || !(write.source.version.key == write.version.key)) return false;
    auto *page = &node->second;
    const auto valid = [&] {
        const auto *base = shard->records.version(location.value);
        return page->revisionCanAdvance() && !page->activity.hasWriter()
            && !page->activity.publicationClaim && page->nextGeneration == write.version.generation
            && (base ? base->version == write.source.version
                : !write.baseWasPrepared && write.source.version.isDefaultPixel() && !write.source.isValid());
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

bool KisPageMetadataCoordinator::acceptPrivateTarget(const KisPageWriteSelection &selection,
    const KisPageWriteSource &source, const KisReplicaHandle &target, KisPagePrivateWrite *native, quint32 limit, QString *error,
    const std::shared_ptr<KisCpuResidentBinding> &before, bool complete,
    std::shared_ptr<KisCpuResidentBinding> *targetBinding)
{
    const auto &write = selection.write;
    auto *core = m_core.load(std::memory_order_acquire);
    if (!core || !write.version.key.isValid() || core->shards.empty()) return false;
    const auto &location = selection.base;
    if (!native || !native->location.value.isValid()
        || native->location.shardIndex >= core->shards.size()
        || !location.node || location.shardIndex != native->location.shardIndex) return false;
    const size_t shardIndex = native->location.shardIndex;
    auto *shard = core->shards[shardIndex].get();
    if (native->location.shard != shard || location.shard != shard || native->reserved || !target.isValid()
        || !(write.source.version.key == write.version.key)
        || !(target.version == write.version) || !write.transaction.isValid()) return false;
    QMutexLocker lock(&shard->mutex);
    const auto *preparedValue = native->location.shard == shard
        ? shard->arenas.versions.get(native->location.value) : nullptr;
    if (!preparedValue || preparedValue != native->value || preparedValue->indexed
        || !(preparedValue->version == write.version)) return false;
    const KisVersionSlotId baseSlot = location.value;
    auto *base = shard->records.version(baseSlot);
    auto *node = static_cast<decltype(shard->pages)::value_type *>(location.node);
    const bool originalSource = base ? base->version == write.source.version
        : !write.baseWasPrepared && write.source.version.isDefaultPixel() && !source.isValid();
    if (!originalSource || !(node->first == write.version.key)
        || &node->second != native->location.page) return false;
    auto *page = &node->second;
    KisPageVersion physicalOwner;
    if (!page->revisionCanAdvance()
        || page->activity.publicationClaim || page->activity.hasWriter()
        || !(write.version.generation == page->nextGeneration)
        || write.version.generation.value <= write.source.version.generation.value
        || write.version.generation.value == std::numeric_limits<quint64>::max()) return false;
    const bool occupied = shard->records.physicalOwner(target.physicalSlotIdentity(), &physicalOwner);
    if (base ? (write.baseWasPrepared
        ? base->publication != KisPagePublicationState::Prepared || !(base->preparedBy == write.transaction)
        : base->publication != KisPagePublicationState::Historical)
        : write.baseWasPrepared || !write.source.version.isDefaultPixel() || source.isValid()) return false;
    Q_ASSERT(page->versionCount >= page->committedCount);
    if (quint64(page->versionCount) - page->committedCount >= limit) return false;

    if (before) {
        // Reclaiming an authority needs no new metadata: its independent exact
        // before already exists, and the new private target lives in native.
        // All semantic and identity checks precede the first mutation below.
        if (!base || !occupied || !(physicalOwner == write.source.version) || page->activity.genericHandoff) return false;
        const auto oldSlot = base->authorityReplica;
        auto *old = shard->records.replica(oldSlot);
        KisReplicaSlotId sourceSlot;
        if (!shard->records.findReplicaSlot(baseSlot, source, &sourceSlot) || sourceSlot == oldSlot) return false;
        const auto *sourceRecord = shard->records.replica(sourceSlot);
        if (!old || !sourceRecord || old->validity != KisReplicaValidity::Valid
            || sourceRecord->validity != KisReplicaValidity::Valid || old->activeOperation.isValid()
            || sourceRecord->activeOperation.isValid() || old->pinCount || old->overflow.isValid()) return false;
        const auto authorityIdentity = old->identity();
        const auto &authorityLayout = old->layout();
        const KisReplicaPhysicalSlotIdentity authoritySlot{authorityIdentity.provider,
            authorityIdentity.providerEpoch, authorityIdentity.allocation.slot};
        if (!(authorityLayout == source.layout()) || !(authorityLayout == target.layout)
            || old->domain() != target.domain
            || !(authoritySlot == target.physicalSlotIdentity())
            || authoritySlot == source.physicalSlotIdentity()
            || authorityIdentity.allocation.generation == std::numeric_limits<quint64>::max()
            || target.allocation.generation != authorityIdentity.allocation.generation + 1) return false;
        const bool cachedAuthority = base->native.nativeReplica == oldSlot;
        sourceSlot = shard->records.replaceHandoffAuthority(base, oldSlot, sourceSlot);
        auto *facts = &base->native;
        shard->arenas.cpuReadBindings -= bool(facts->nativeBinding);
        // The surviving source may have been the old cached placement. It
        // must stop borrowing that binding before the original owner moves.
        shard->records.replica(sourceSlot)->detachResource(base->version);
        if (cachedAuthority) {
            facts->nativeBinding = native->exchangeBinding(std::move(facts->nativeBinding), target);
        } else if (facts->nativeBinding != before) {
            // A cached independent before/other placement is not the retagged
            // authority. Return any displaced cache through the original row;
            // the actual physical writer supplies target binding after commit.
            Q_ASSERT(!native->releasedBinding);
            native->releasedBinding = std::move(facts->nativeBinding);
        }
        if (facts->nativeBinding != before) facts->nativeBinding = before;
        facts->nativeReplica = sourceSlot;
        shard->records.replica(sourceSlot)->setHandle(source.handle(), before.get());
        ++shard->arenas.cpuReadBindings;
        ++shard->publicationVersionInstalls;
    } else {
        const auto authority = base ? base->authorityReplica : KisReplicaSlotId{};
        const auto *replica = shard->records.replica(authority);
        if (occupied || (!(replica && replica->matchesSource(source, base->version))
                && !(write.source.version.isDefaultPixel() && !source.isValid()))
            || (!write.source.version.isDefaultPixel() && (!replica || replica->validity != KisReplicaValidity::Valid))) return false;
    }
    auto &activity = page->activity;
    activity.nativeWrite = native;
    activity.writerTransaction = write.transaction;
    if (targetBinding) *targetBinding = native->exchangeBinding(std::move(*targetBinding), target);
    else native->value->primaryReplica.setHandle(target, native->binding().get());
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
    const KisReplicaHandle &target, std::shared_ptr<KisCpuResidentBinding> *targetBinding)
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
    if (targetBinding) *targetBinding = result->exchangeBinding(std::move(*targetBinding), target);
    else result->value->primaryReplica.setHandle(target, result->binding().get());
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

bool KisPageMetadataCoordinator::freezePrivateValues(KisPagePublicationRows rows,
    KisPageTransactionId transaction, quint64 issuer, bool complete)
{
    auto *core = m_core.load(std::memory_order_acquire);
    if (!core || !issuer || core->cpuProofIssuer.load() != issuer || !transaction.isValid()
        || (rows.size() && !rows)) return false;
    size_t count = 0;
    for (auto &row : rows) {
        auto &work = row.metadataAcceptance;
        work = {};
        if (row.replacement.removal) continue;
        auto *value = row.replacement.native;
        auto *shard = value ? Private::registeredShard(core, *value) : nullptr;
        if (!shard || !value->reserved || !value->location.page) return false;
        work.input = &row;
        work.shard = shard;
        work.page = value->location.page;
        work.shardIndex = quint16(value->location.shardIndex);
        ++count;
    }
    using It = KisPublicationMemberIterator<KisPagePublicationChange::MetadataAcceptance,
                                           &KisPagePublicationChange::metadataAcceptance>;
    std::sort(It(rows.begin()), It(rows.end()), [](const auto &a, const auto &b) {
        if (bool(a.input) != bool(b.input)) return bool(a.input);
        return a.shardIndex < b.shardIndex;
    });
    for (size_t first = 0; first < count;) {
        auto *shard = static_cast<MetadataShard *>(rows[first].metadataAcceptance.shard);
        size_t end = first + 1;
        while (end < count && end - first < 16 && rows[end].metadataAcceptance.shard == shard) ++end;
        QMutexLocker lock(&shard->mutex);
        for (size_t i = first; i < end; ++i) {
            const auto &work = rows[i].metadataAcceptance;
            auto &value = *work.input->replacement.native;
            const auto &activity = static_cast<MetadataPage *>(work.page)->activity;
            if (activity.nativeWrite != &value || (!value.completed && !complete)
                || !(activity.writerTransaction == transaction)
                || (value.isSealed() && !(value.sealedBy() == transaction))) return false;
        }
        for (size_t i = first; i < end; ++i) {
            auto &value = *rows[i].metadataAcceptance.input->replacement.native;
            value.completed = true;
            if (!value.isSealed()) {
                value.value->native.sealedBy = transaction;
                ++shard->arenas.sealedCpuValues;
            }
        }
        first = end;
    }
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
    Q_ASSERT(page->second.versionCount >= page->second.committedCount);
    const quint64 count = quint64(page->second.versionCount) - page->second.committedCount + native;
    return count < limit;
}

bool KisPageMetadataCoordinator::queryPublication(const KisPageVersion &current,
                                                  const KisPageVersion &target,
                                                  PublicationInfo *info) const
{
    const auto &key = current.key;
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), key);
    if (!shard || !info || !current.isValid() || (target.isValid() && !(target.key == key)))
        return false;
    QMutexLocker lock(&shard->mutex);
    const auto page = shard->pages.find(key);
    if (page == shard->pages.cend() || !page->second.hasMetadata())
        return false;
    KisVersionSlotId selected;
    const auto *head = shard->records.findVersion(current, &selected) ? shard->records.version(selected) : nullptr;
    if (!head && !current.isDefaultPixel()) return false;
    *info = {};
    if (head) info->current = shard->records.versionInfo<VersionInfo>(head);
    else {
        // The caller selected this exact default from its actual source root;
        // observing or replacing it does not require an optional cache Value.
        info->current.version = current;
        info->current.publication = KisPagePublicationState::Historical;
    }
    if (target.isValid())
        info->target = target == current ? info->current
            : shard->records.versionInfo<VersionInfo>(target);
    shard->publicationLookupVersionInputs += quint64(info->current.version.isValid())
        + quint64(info->target.version.isValid() && !(info->target.version == info->current.version));
    return true;
}

bool KisPageMetadataCoordinator::retireHistoryValues(
    KisPageViewValue *const *values, size_t count, quint32 *removedMask)
{
    *removedMask = 0;
    if (!count) return true;
    Q_ASSERT(values && count <= HistoryCollection::Limit);
    const auto key = values[0]->version.key;
    auto *shard = Private::shardFor(m_core.load(std::memory_order_acquire), key);
    Q_ASSERT(shard);
    for (;;) {
        const auto storageOwner = shard->budgetAuthority->storage<char>();
        // Keep the common retirement span inline without imposing a large
        // stack-probe call on every batch, including empty/protected inputs.
        using Effects = MetadataLocalArray<KisPageTransitionEffect, 8>;
        Effects prepared{Effects::allocator_type(storageOwner.borrowed())};
        std::array<std::shared_ptr<const KisPageAllocationDescriptor>, HistoryCollection::Limit> releasedDescriptors;
        std::array<std::shared_ptr<KisCpuResidentBinding>, HistoryCollection::Limit> releasedBindings;
        QMutexLocker lock(&shard->mutex);
        auto page = shard->pages.find(key);
        if (page == shard->pages.end() || !page->second.hasMetadata()) return true;
        quint32 selected = 0;
        size_t replicaCount = 0;
        for (size_t i = 0; i < count; ++i) {
            Q_ASSERT(values[i] && values[i]->version.key == key);
            const auto *record = static_cast<const KisPageValueCell *>(values[i]);
            Q_ASSERT(shard->arenas.versions.get(record->slot) == record);
            if (!record->indexed || record->publication != KisPagePublicationState::Historical
                || record->rootEdges.load(std::memory_order_acquire)
                || (!record->version.isDefaultPixel() && record->readEdges)
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
            selected |= quint32(1) << i;
            replicaCount += record->replicaCount;
        }
        if (!selected) return true;
        if (!shard->canMutate(key, page->second)) return false;
        prepared.reserve(replicaCount);
        for (size_t i = 0; i < count; ++i) {
            if (!(selected & (quint32(1) << i))) continue;
            const auto *record = static_cast<const KisPageValueCell *>(values[i]);
            for (auto r = record->firstReplica; r.isValid();) {
                const auto *replica = shard->records.replica(r);
                prepared.push_back({replicaHandle(*replica, record->version), {}});
                r = replica->nextReplica;
            }
        }
        const quint64 revision = page->second.revision;
        bool stale = false;
        auto install = [&] {
            if (!lock.isLocked()) lock.relock();
            page = shard->pages.find(key);
            if (page == shard->pages.end() || !shard->canMutate(key, page->second)
                || page->second.revision != revision) {
                stale = true;
                lock.unlock();
                return false;
            }
            // A root admission can change retention without changing the page
            // revision. Recheck the original live edge at the actual accept cut.
            for (size_t i = 0; i < count; ++i) {
                if ((selected & (quint32(1) << i))
                    && values[i]->rootEdges.load(std::memory_order_acquire)) {
                    stale = true;
                    lock.unlock();
                    return false;
                }
            }
            // All storage and claims exist. Removal only unlinks original records.
            quint64 removed = 0;
            for (size_t i = 0; i < count; ++i) {
                if (!(selected & (quint32(1) << i))) continue;
                auto *value = static_cast<KisPageValueCell *>(values[i]);
                shard->records.remove(&page->second, value, &releasedDescriptors[i], &releasedBindings[i]);
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
            lock.unlock();
            return true;
        };
        bool accepted;
        if (prepared.empty()) accepted = install();
        else {
            lock.unlock();
            accepted = m_acceptRetirement && m_acceptRetirement(m_retirementContext,
                prepared.data(), qsizetype(prepared.size()), &install,
                [](void *p) { return (*static_cast<decltype(install) *>(p))(); }, nullptr);
        }
        if (!accepted) {
            if (stale) continue;
            lock.relock();
            ++shard->rejectedTransitions;
            return false;
        }
        if (!prepared.empty())
            m_deliverRetirement(m_retirementContext, prepared.data(), qsizetype(prepared.size()));
        return true;
    }
}

KisPageMetadataCoordinator::RegisteredKeys KisPageMetadataCoordinator::registeredPageKeys() const
{
    const auto *d = m_core.load(std::memory_order_acquire);
    if (!d)
        return {};
    RegisteredKeys result(d->budgetAuthority->storage<KisPageKey>());
    result.reserve(size_t(d->registeredPages.loadAcquire()));
    for (const auto &shard : d->shards) {
        QMutexLocker lock(&shard->mutex);
        quint64 registered = 0;
        for (auto page = shard->pages.cbegin(); page != shard->pages.cend(); ++page) {
            if (!page->second.hasMetadata()) continue;
            ++registered;
            result.push_back(page->first);
        }
        shard->publicationDirectoryHeaders += quint64(registered);
    }
    return result;
}

namespace
{
// Synchronous local mutations borrow the original cells. Only changed scalars,
// actual new identities and actual protection edits survive until acceptance.
// No surviving Value/replica/protection chain is projected or reinstalled.
class MetadataLocalMutation final
{
    static constexpr qsizetype Missing = -1;
    struct Value {
        const KisPageVersion *identity;
        KisPageValueCell *original = nullptr;
        KisVersionSlotId installed;
        KisPagePublicationState publication = KisPagePublicationState::Historical;
        KisPageTransactionId preparedBy;
        qsizetype authority = Missing;
        bool authorityLoaded = false;
        bool removed = false;
        bool synthetic = false;
        bool changed = false;
    };
    struct Replica {
        qsizetype owner;
        KisReplicaSlotId slot;
        const KisReplicaHandle *introduced = nullptr;
        KisPageOperationId operation;
        quint32 pins = 0;
        KisReplicaValidity validity = KisReplicaValidity::Allocated;
    };
    struct Protection {
        qsizetype replica;
        KisMetadataOverflowSlotId slot;
        quint64 lease;
        const KisCompletionTicket *completion = nullptr;
        size_t appendOrder = 0;
        bool removed = false;
    };
public:
    MetadataLocalMutation(MetadataShard &owner, MetadataPage &page, const KisPageKey &key,
                          MetadataLocalStorage storage)
        : shard(owner), page(page), key(key), image(owner.activityImage(key, page))
        , nextGeneration(page.nextGeneration)
        , values(decltype(values)::allocator_type(storage))
        , replicas(decltype(replicas)::allocator_type(storage))
        , protections(decltype(protections)::allocator_type(storage))
        , appends(decltype(appends)::allocator_type(storage))
        , effects(decltype(effects)::allocator_type(storage))
        , released(decltype(released)::allocator_type(storage)) {}

    void capture(const KisPageTransition *input, qsizetype count)
    {
        if (image.writerPhase != KisPageWriterPhase::None) loadWriter();
        for (qsizetype i = 0; i < count; ++i) {
            findValue(input[i].baseVersion); findValue(input[i].version);
            for (const auto &version : input[i].versions) findValue(version);
            if (input[i].kind == KisPageTransitionKind::AbortTransaction)
                shard.records.forPreparedTransaction(page, input[i].transaction,
                    [&](const auto &version) { findValue(version); });
        }
    }

    bool apply(const KisPageTransition &t, QString *error)
    {
        const auto reject = [&](const QString &reason) {
            KisPageStoreDetail::setError(error, reason); return false;
        };
        using K = KisPageTransitionKind;
        const bool writing = t.kind == K::AcquireWrite || t.kind == K::PrepareWrite
            || t.kind == K::BeginPublish || t.kind == K::PublishWrite || t.kind == K::FailWrite
            || t.kind == K::CancelWrite || t.kind == K::AbortTransaction
            || t.kind == K::AbortPreparedVersion || t.kind == K::DetachPreparedVersion;
        if (writing && image.nativeWrite)
            return reject(QStringLiteral("page has an unaccepted native write reservation"));
        switch (t.kind) {
        case K::AcquireWrite: {
            if (image.writerPhase != KisPageWriterPhase::None)
                return reject(QStringLiteral("page already has a reserved writer"));
            if (!t.writer.isValid() || !t.operation.isValid() || !t.transaction.isValid()
                || !t.baseVersion.isValid() || !t.version.isValid() || !t.target.isValid())
                return reject(QStringLiteral("write reservation identity is incomplete"));
            if (!(t.baseVersion.key == key) || !(t.version.key == key) || !(t.target.version == t.version))
                return reject(QStringLiteral("write reservation targets another page"));
            auto base = findValue(t.baseVersion);
            if (base == Missing && t.baseVersion.isDefaultPixel() && !t.source.isValid())
                base = addValue(t.baseVersion, KisPagePublicationState::Historical, true);
            if (base == Missing || (values[base].publication != KisPagePublicationState::Historical
                && !(values[base].publication == KisPagePublicationState::Prepared
                     && values[base].preparedBy == t.transaction)))
                return reject(QStringLiteral("write base is not visible to the transaction"));
            const auto before = findReplica(base, t.source);
            const bool virtualBase = virtualDefault(base) && !t.source.isValid()
                && t.writeMode == KisPageWriteMode::DiscardContents;
            if (!authorityMatches(base, t.source)
                || (!virtualBase && (before == Missing || replicas[before].validity != KisReplicaValidity::Valid)))
                return reject(QStringLiteral("write base authority is not a valid exact-generation replica"));
            if (!(t.version.generation == nextGeneration)
                || t.version.generation.value <= t.baseVersion.generation.value || findValue(t.version) != Missing)
                return reject(QStringLiteral("write generation is not the next free generation"));
            if (nextGeneration.value == std::numeric_limits<quint64>::max())
                return reject(QStringLiteral("page generation space is exhausted"));
            if (before != Missing && replicas[before].pins == std::numeric_limits<quint32>::max())
                return reject(QStringLiteral("write before-image pin count is exhausted"));
            if (!physicalAvailable(t.target, error, QStringLiteral("write allocation token is already in use"))) return false;
            const auto targetValue = addValue(t.version, KisPagePublicationState::Unpublished);
            writerTarget = addReplica(targetValue, t.target, KisReplicaValidity::Allocated);
            writerBase = before;
            if (before != Missing) ++replicas[before].pins;
            image.writerToken = t.writer; image.writerOperation = t.operation;
            image.writerTransaction = t.transaction; image.writerMode = t.writeMode;
            image.writerDefaultGeneration = before == Missing ? t.baseVersion.generation : KisPageGeneration{};
            image.writerDefaultRevision = before == Missing ? t.baseVersion.defaultPixelRevision : 0;
            image.writerPhase = KisPageWriterPhase::Reserved;
            ++nextGeneration.value;
            break;
        }
        case K::PrepareWrite:
        case K::BeginPublish: {
            const bool prepare = t.kind == K::PrepareWrite;
            if (image.writerPhase != (prepare ? KisPageWriterPhase::Reserved : KisPageWriterPhase::Writable)
                || !matchesWriter(t))
                return reject(prepare ? QStringLiteral("write preparation does not match the reservation")
                                      : QStringLiteral("publish request is stale or mismatched"));
            const auto &target = replicas[writerTarget];
            if (target.operation.isValid() || (prepare
                    ? target.validity == KisReplicaValidity::Failed || target.validity == KisReplicaValidity::Retiring
                    : !publishable(target)))
                return reject(prepare ? QStringLiteral("write target is not ready for exclusive access")
                                      : QStringLiteral("write replica cannot begin publishing"));
            image.writerPhase = prepare ? KisPageWriterPhase::Writable : KisPageWriterPhase::Publishing;
            break;
        }
        case K::PublishWrite:
        case K::FailWrite: {
            const bool publish = t.kind == K::PublishWrite;
            if ((image.writerPhase != KisPageWriterPhase::Publishing
                 && image.writerPhase != KisPageWriterPhase::CancelPending) || !matchesWriter(t))
                return reject(publish ? QStringLiteral("publish completion is stale or mismatched")
                                      : QStringLiteral("write failure completion is stale or mismatched"));
            const auto target = writerTarget;
            const auto value = replicas[target].owner;
            if (publish ? !publishable(replicas[target])
                        : replicas[target].operation.isValid() || protectedReplica(target) || replicas[target].pins)
                return reject(publish ? QStringLiteral("write replica cannot complete publishing")
                                      : QStringLiteral("failed write still has in-flight state"));
            if (!releaseBefore())
                return reject(publish ? QStringLiteral("write before-image retention was lost")
                                      : QStringLiteral("failed write still has in-flight state"));
            if (!publish || image.writerPhase == KisPageWriterPhase::CancelPending) {
                effects.push_back({handle(target), {}});
                values[value].removed = true;
            } else {
                replicas[target].validity = KisReplicaValidity::Valid;
                values[value].authority = target; values[value].authorityLoaded = true;
                values[value].publication = KisPagePublicationState::Prepared;
                values[value].preparedBy = image.writerTransaction;
            }
            clearWriter();
            break;
        }
        case K::CancelWrite: {
            if (!matchesWriter(t)) return reject(QStringLiteral("write cancellation is stale or mismatched"));
            if (image.writerPhase == KisPageWriterPhase::Publishing) {
                image.writerPhase = KisPageWriterPhase::CancelPending; break;
            }
            if (image.writerPhase == KisPageWriterPhase::CancelPending)
                return reject(QStringLiteral("write cancellation is already pending"));
            const auto value = replicas[writerTarget].owner;
            if (!idle(value, false)) return reject(QStringLiteral("write generation still has in-flight users"));
            if (!releaseBefore()) return reject(QStringLiteral("write before-image retention was lost"));
            retire(value); clearWriter();
            break;
        }
        case K::AcquireRead:
        case K::ReleaseRead: {
            const bool acquire = t.kind == K::AcquireRead;
            if (!t.lease.isValid()) return reject(QStringLiteral("read lease identity is invalid"));
            const auto value = findValue(t.version);
            if (acquire && (value == Missing || values[value].publication == KisPagePublicationState::Unpublished
                            || values[value].publication == KisPagePublicationState::Retiring))
                return reject(QStringLiteral("requested page version is not readable"));
            if (acquire && values[value].publication == KisPagePublicationState::Prepared
                && !(values[value].preparedBy == t.transaction))
                return reject(QStringLiteral("prepared page is private to another transaction"));
            const auto target = value == Missing ? Missing : findReplica(value, t.target);
            if (acquire) {
                if (target == Missing || replicas[target].validity != KisReplicaValidity::Valid)
                    return reject(QStringLiteral("requested replica is not valid"));
                if (leaseExists(t.lease)) return reject(QStringLiteral("read lease identity is already active"));
                KisMetadataOverflowSlotId reusable;
                for (auto &p : protections) if (p.replica == target && p.removed && p.slot.isValid()) {
                    reusable = std::exchange(p.slot, KisMetadataOverflowSlotId{}); break;
                }
                protections.push_back({target, reusable, t.lease.value});
                appendProtection(qsizetype(protections.size() - 1));
            } else {
                const auto node = target == Missing ? Missing : findLease(target, t.lease);
                if (node == Missing) return reject(QStringLiteral("read lease is stale or belongs to another replica"));
                if (t.completion.isValid()) {
                    protections[node].completion = &t.completion;
                    appendProtection(node);
                } else protections[node].removed = true;
            }
            break;
        }
        case K::AttachHistoricalDefault: {
            if (!t.version.isValid() || !(t.version.key == key) || !t.version.isDefaultPixel()
                || (t.target.isValid() && !(t.target.version == t.version))
                || t.imageEpoch.isValid() || findValue(t.version) != Missing)
                return reject(QStringLiteral("historical default attachment identity is invalid"));
            if (!physicalAvailable(t.target, error, QStringLiteral("historical default allocation token is already in use"))) return false;
            const auto value = addValue(t.version, KisPagePublicationState::Historical);
            if (t.target.isValid()) {
                values[value].authority = addReplica(value, t.target, KisReplicaValidity::Valid);
                values[value].authorityLoaded = true;
            }
            break;
        }
        case K::MaterializeDefault: {
            const auto value = findValue(t.version);
            if (value == Missing || !virtualDefault(value) || !t.target.isValid()
                || !(t.target.version == t.version) || t.imageEpoch.isValid())
                return reject(QStringLiteral("virtual default materialization is invalid"));
            if (!physicalAvailable(t.target, error, QStringLiteral("default allocation token is already in use"))) return false;
            values[value].synthetic = false;
            const auto target = addReplica(value, t.target, KisReplicaValidity::Valid);
            values[value].authority = target; values[value].authorityLoaded = true;
            break;
        }
        case K::BeginAuthorityHandoff: {
            if (image.handoffOperation.isValid() || !t.operation.isValid())
                return reject(QStringLiteral("authority handoff is already active or invalid"));
            const auto value = findValue(t.version);
            const auto source = value == Missing ? Missing : findReplica(value, t.source);
            const auto target = value == Missing ? Missing : findReplica(value, t.target);
            if (source == Missing || target == Missing || !authorityMatches(value, t.source)
                || replicas[source].validity != KisReplicaValidity::Valid
                || replicas[target].validity != KisReplicaValidity::Valid || source == target)
                return reject(QStringLiteral("authority handoff endpoints are invalid"));
            if (replicas[source].pins == std::numeric_limits<quint32>::max()
                || replicas[target].pins == std::numeric_limits<quint32>::max())
                return reject(QStringLiteral("authority handoff pin count is exhausted"));
            ++replicas[source].pins; ++replicas[target].pins;
            image.handoffOperation = t.operation; handoffSource = source; handoffTarget = target;
            break;
        }
        case K::CommitAuthorityHandoff:
        case K::FailAuthorityHandoff: {
            const bool commit = t.kind == K::CommitAuthorityHandoff;
            loadHandoff();
            if (!image.handoffOperation.isValid() || !(image.handoffOperation == t.operation)
                || handoffSource == Missing || handoffTarget == Missing
                || !matches(handoffSource, t.source) || !matches(handoffTarget, t.target)
                || !(*values[replicas[handoffSource].owner].identity == t.version))
                return reject(commit ? QStringLiteral("authority handoff completion is stale or mismatched")
                                     : QStringLiteral("authority handoff failure is stale or mismatched"));
            const auto value = replicas[handoffSource].owner;
            if (!replicas[handoffSource].pins || !replicas[handoffTarget].pins
                || (commit ? replicas[handoffTarget].validity != KisReplicaValidity::Valid
                           : authority(value) != handoffSource))
                return reject(commit ? QStringLiteral("authority handoff lost a protected endpoint")
                                     : QStringLiteral("failed authority handoff lost a protected endpoint"));
            --replicas[handoffSource].pins; --replicas[handoffTarget].pins;
            if (commit) { values[value].authority = handoffTarget; values[value].authorityLoaded = true; }
            image.handoffOperation = {}; image.handoffSource = {}; image.handoffTarget = {};
            handoffSource = handoffTarget = Missing;
            break;
        }
        case K::DetachPreparedVersion: {
            const auto value = findValue(t.version);
            if (!t.transaction.isValid() || t.operation.isValid() || value == Missing
                || values[value].publication != KisPagePublicationState::Prepared
                || !(values[value].preparedBy == t.transaction) || image.writerPhase != KisPageWriterPhase::None)
                return reject(QStringLiteral("prepared detachment identity is invalid or still writable"));
            values[value].publication = KisPagePublicationState::Historical;
            values[value].preparedBy = {};
            break;
        }
        case K::DiscardHistoricalVersions: {
            if (t.versions.empty() || t.transaction.isValid() || t.operation.isValid())
                return reject(QStringLiteral("historical discard identity is invalid"));
            for (auto it = t.versions.cbegin(); it != t.versions.cend(); ++it) {
                if (!it->isValid() || !(it->key == key) || std::find(t.versions.cbegin(), it, *it) != it)
                    return reject(QStringLiteral("historical discard set is invalid"));
                const auto value = findValue(*it);
                if (value == Missing || values[value].publication != KisPagePublicationState::Historical
                    || readEdges(value) || (authority(value) == Missing && !virtualDefault(value))
                    || (values[value].original && values[value].original->rootEdges.load(std::memory_order_acquire)))
                    return reject(QStringLiteral("historical discard target is unavailable"));
                if (!idle(value, true)) return reject(QStringLiteral("historical discard target is still in use"));
            }
            for (qsizetype i = qsizetype(values.size()); i-- > 0;)
                if (!values[i].removed && std::find(t.versions.cbegin(), t.versions.cend(), *values[i].identity) != t.versions.cend())
                    retire(i);
            break;
        }
        case K::AbortPreparedVersion:
        case K::AbortTransaction: {
            const bool exact = t.kind == K::AbortPreparedVersion;
            if (!t.transaction.isValid() || (exact && (!t.version.isValid() || !(t.version.key == key))))
                return reject(QStringLiteral("transaction identity is invalid"));
            if (image.writerPhase != KisPageWriterPhase::None && image.writerTransaction == t.transaction)
                return reject(QStringLiteral("transaction still owns a write lease"));
            bool found = false;
            for (qsizetype i = qsizetype(values.size()); i-- > 0;) {
                if (values[i].removed || values[i].publication != KisPagePublicationState::Prepared
                    || !(values[i].preparedBy == t.transaction) || (exact && !(*values[i].identity == t.version))) continue;
                if (readEdges(i)) {
                    values[i].publication = KisPagePublicationState::Historical;
                    values[i].preparedBy = {};
                } else {
                    if (!idle(i, false)) return reject(QStringLiteral("prepared page still has in-flight users"));
                    retire(i);
                }
                found = true;
            }
            if (!found) return reject(QStringLiteral("transaction has no prepared page in this record"));
            break;
        }
        default: return reject(QStringLiteral("owner transition is not a local metadata mutation"));
        }
        return true;
    }

    MetadataArenaDemand demand() const
    {
        MetadataArenaDemand out;
        for (const auto &v : values) out.versions += !v.original && !v.removed && !v.synthetic;
        for (const auto &p : protections)
            out.overflow += !p.slot.isValid() && !p.removed && !values[replicas[p.replica].owner].removed;
        return out;
    }
    qsizetype physicalInsertions() const
    {
        return std::count_if(replicas.begin(), replicas.end(),
            [&](const auto &r) { return r.introduced && !values[r.owner].removed; });
    }
    quint64 inputs() const { return values.size(); }
    void prepareRelease()
    {
        released.resize(std::count_if(values.begin(), values.end(), [](const auto &v) { return v.original && v.removed; }));
    }
    bool validate(QString *error) const
    {
        for (const auto &v : values) {
            if (v.removed && v.original && (v.original->rootEdges.load(std::memory_order_acquire)
                || (!v.identity->isDefaultPixel() && v.original->readEdges))) {
                KisPageStoreDetail::setError(error, QStringLiteral("removed value gained view retention before acceptance"));
                return false;
            }
        }
        for (const auto &r : replicas) if (r.introduced && !values[r.owner].removed) {
            KisReplicaSlotId occupied;
            if (shard.records.physicalSlots.findExact(r.introduced->physicalSlotIdentity(), &occupied)
                && !removedOwner(occupied)) {
                KisPageStoreDetail::setError(error, QStringLiteral("metadata physical ownership changed before acceptance"));
                return false;
            }
        }
        return true;
    }
    void install(ShardRecordStore::ReservationSet &reservation)
    {
        size_t releasedCount = 0;
        for (auto &v : values) if (v.original && v.removed) {
            auto &out = released[releasedCount++];
            shard.records.remove(&page, v.original, &out.descriptor, &out.binding);
        }
        for (auto &v : values) if (!v.removed && !v.original && !v.synthetic) {
            v.installed = shard.records.insertEmptyReserved(&page, *v.identity, v.publication, v.preparedBy, &reservation);
            Q_ASSERT(v.installed.isValid());
            ++shard.localVersionInstalls;
            if (kisOnPageStoreReclamationThread()) ++shard.backgroundLocalVersionInstalls;
        }
        for (auto &r : replicas) {
            if (values[r.owner].removed) continue;
            auto *v = cell(r.owner);
            if (r.introduced) r.slot = shard.records.appendSingleReplicaReserved(v, *r.introduced, &reservation);
            auto *stored = shard.records.replica(r.slot); Q_ASSERT(stored);
            if (stored->pinCount != r.pins || !(stored->activeOperation == r.operation) || stored->validity != r.validity) {
                stored->pinCount = r.pins; stored->activeOperation = r.operation; stored->validity = r.validity;
                values[r.owner].changed = true;
            }
        }
        // Unlink only real modified nodes. Every unchanged original chain and
        // node keeps its identity, capacity and relative order.
        for (qsizetype i = 0; i < qsizetype(replicas.size()); ++i) {
            if (values[replicas[i].owner].removed) continue;
            if (std::none_of(protections.begin(), protections.end(), [&](const auto &p) { return p.replica == i && p.slot.isValid(); })) continue;
            auto *link = &shard.records.replica(replicas[i].slot)->overflow;
            while (link->isValid()) {
                const auto slot = *link; auto *node = shard.records.overflow(slot);
                const auto p = std::find_if(protections.begin(), protections.end(), [&](const auto &p) { return p.slot == slot; });
                if (p == protections.end()) { link = &node->next; continue; }
                *link = node->next; node->next = {};
                values[replicas[i].owner].changed = true;
                if (p->removed) { const bool erased = shard.arenas.overflow.erase(slot); Q_ASSERT(erased); Q_UNUSED(erased); }
            }
        }
        for (size_t order = 0; order < appends.size(); ++order) {
            auto &p = protections[appends[order]];
            if (p.removed || p.appendOrder != order || values[replicas[p.replica].owner].removed) continue;
            KisMetadataOverflowNode node;
            node.kind = p.completion ? KisMetadataOverflowKind::PendingLastUse : KisMetadataOverflowKind::ReadLease;
            node.value = p.completion ? 0 : p.lease;
            if (p.completion) node.completion = *p.completion;
            if (!p.slot.isValid()) p.slot = shard.arenas.overflow.emplaceReserved(&reservation.arenas.overflow, node);
            else *shard.records.overflow(p.slot) = node;
            Q_ASSERT(p.slot.isValid());
            auto *link = &shard.records.replica(replicas[p.replica].slot)->overflow;
            while (link->isValid()) link = &shard.records.overflow(*link)->next;
            *link = p.slot;
            values[replicas[p.replica].owner].changed = true;
        }
        for (qsizetype i = 0; i < qsizetype(values.size()); ++i) {
            auto &v = values[i];
            if (v.removed || v.synthetic) continue;
            auto *stored = cell(i);
            const auto selected = v.authority == Missing ? KisReplicaSlotId{} : replicas[v.authority].slot;
            if (v.authorityLoaded && !(stored->authorityReplica == selected)) {
                stored->authorityReplica = selected; v.changed = true;
            }
            if (stored->publication != v.publication || !(stored->preparedBy == v.preparedBy)) {
                const bool set = shard.records.setPublication(&page, stored, v.publication, v.preparedBy);
                Q_ASSERT(set); Q_UNUSED(set);
            } else if (v.changed) shard.records.reconsiderHistory(stored);
        }
        if (image.writerPhase != KisPageWriterPhase::None) {
            image.writerBaseAuthority = writerBase == Missing ? KisReplicaSlotId{} : replicas[writerBase].slot;
            image.writerTarget = replicas[writerTarget].slot;
        }
        if (image.handoffOperation.isValid() && handoffSource != Missing) {
            image.handoffSource = replicas[handoffSource].slot;
            image.handoffTarget = replicas[handoffTarget].slot;
        }
        image.preparedPublications = page.activity.preparedPublications;
        shard.installActivityImage(key, &page, image);
        page.nextGeneration = nextGeneration;
        shard.localVersionRemovals += releasedCount;
        if (kisOnPageStoreReclamationThread()) shard.backgroundLocalVersionRemovals += releasedCount;
    }
    bool hasActivity() const { return image.writerPhase != KisPageWriterPhase::None || image.handoffOperation.isValid(); }
private:
    MetadataShard &shard;
    MetadataPage &page;
    const KisPageKey &key;
    MetadataPageActivity image;
    KisPageGeneration nextGeneration;
    MetadataLocalArray<Value, 2> values;
    MetadataLocalArray<Replica, 2> replicas;
    MetadataLocalArray<Protection, 2> protections;
    MetadataLocalArray<qsizetype, 2> appends;
public:
    MetadataLocalArray<KisPageTransitionEffect, 2> effects;
private:
    MetadataLocalArray<MetadataReleasedReferences, 2> released;
    qsizetype writerBase = Missing, writerTarget = Missing;
    qsizetype handoffSource = Missing, handoffTarget = Missing;

    KisPageValueCell *cell(qsizetype i) const
    { return values[i].original ? values[i].original : shard.records.version(values[i].installed); }
    quint32 readEdges(qsizetype i) const
    { return values[i].original && !values[i].identity->isDefaultPixel() ? values[i].original->readEdges : 0; }
    qsizetype findValue(const KisPageVersion &identity)
    {
        if (!identity.isValid() || !(identity.key == key)) return Missing;
        for (qsizetype i = qsizetype(values.size()); i-- > 0;)
            if (*values[i].identity == identity) return values[i].removed ? Missing : i;
        KisVersionSlotId slot;
        if (!shard.records.findVersion(identity, &slot)) return Missing;
        auto *stored = shard.records.version(slot);
        values.push_back({&stored->version, stored, slot, stored->publication, stored->preparedBy});
        return qsizetype(values.size() - 1);
    }
    qsizetype addValue(const KisPageVersion &identity, KisPagePublicationState state, bool synthetic = false)
    {
        values.push_back({&identity, nullptr, {}, state, {}, Missing, true, false, synthetic});
        return qsizetype(values.size() - 1);
    }
    qsizetype originalReplica(qsizetype owner, KisReplicaSlotId slot)
    {
        if (!slot.isValid()) return Missing;
        for (qsizetype i = 0; i < qsizetype(replicas.size()); ++i)
            if (replicas[i].slot == slot) return i;
        const auto *stored = shard.records.replica(slot); Q_ASSERT(stored);
        replicas.push_back({owner, slot, nullptr, stored->activeOperation, stored->pinCount, stored->validity});
        return qsizetype(replicas.size() - 1);
    }
    qsizetype addReplica(qsizetype owner, const KisReplicaHandle &handle, KisReplicaValidity validity)
    {
        replicas.push_back({owner, {}, &handle, {}, 0, validity});
        return qsizetype(replicas.size() - 1);
    }
    qsizetype findReplica(qsizetype owner, const KisReplicaHandle &handle)
    {
        if (!handle.isValid()) return Missing;
        for (qsizetype i = 0; i < qsizetype(replicas.size()); ++i)
            if (replicas[i].owner == owner && matches(i, handle)) return i;
        auto *stored = values[owner].original;
        KisReplicaSlotId slot;
        return stored && shard.records.findReplicaSlot(stored->slot, handle, &slot) ? originalReplica(owner, slot) : Missing;
    }
    qsizetype authority(qsizetype value)
    {
        if (!values[value].authorityLoaded) {
            const auto slot = values[value].original->authorityReplica;
            const auto index = originalReplica(value, slot);
            values[value].authority = index; values[value].authorityLoaded = true;
        }
        return values[value].authority;
    }
    bool authorityMatches(qsizetype value, const KisReplicaHandle &handle)
    {
        const auto r = authority(value);
        return r == Missing ? handle == KisReplicaHandle{} : matches(r, handle);
    }
    bool virtualDefault(qsizetype value) const
    {
        if (!values[value].identity->isDefaultPixel()
            || (values[value].publication != KisPagePublicationState::Historical
                && values[value].publication != KisPagePublicationState::Published)) return false;
        if (values[value].original && values[value].original->replicaCount) return false;
        return std::none_of(replicas.begin(), replicas.end(), [&](const auto &r) { return r.owner == value && r.introduced; });
    }
    bool matches(qsizetype r, const KisReplicaHandle &handle) const
    {
        const auto &record = replicas[r];
        return record.introduced ? *record.introduced == handle
            : ShardRecordStore::matchesReplica(*shard.records.replica(record.slot), *values[record.owner].identity, handle);
    }
    KisReplicaHandle handle(qsizetype r) const
    { return replicas[r].introduced ? *replicas[r].introduced : shard.records.projectReplica(replicas[r].slot); }
    void loadWriter()
    {
        if (image.writerPhase == KisPageWriterPhase::None || writerTarget != Missing) return;
        const auto *target = shard.records.replica(image.writerTarget);
        Q_ASSERT(target);
        if (image.writerBaseAuthority.isValid()) {
            const auto *base = shard.records.replica(image.writerBaseAuthority); Q_ASSERT(base);
            writerBase = originalReplica(findValue(shard.records.version(base->ownerVersion)->version), image.writerBaseAuthority);
        }
        writerTarget = originalReplica(findValue(shard.records.version(target->ownerVersion)->version), image.writerTarget);
    }
    bool matchesWriter(const KisPageTransition &t)
    {
        if (image.writerPhase == KisPageWriterPhase::None) return false;
        loadWriter();
        return image.writerToken == t.writer && image.writerOperation == t.operation
            && image.writerTransaction == t.transaction && image.writerMode == t.writeMode
            && (writerBase == Missing ? t.source == KisReplicaHandle{}
                    && t.baseVersion == KisPageVersion{key, image.writerDefaultGeneration, image.writerDefaultRevision}
                : matches(writerBase, t.source) && *values[replicas[writerBase].owner].identity == t.baseVersion)
            && matches(writerTarget, t.target) && *values[replicas[writerTarget].owner].identity == t.version;
    }
    bool releaseBefore()
    {
        if (writerBase == Missing) return image.writerDefaultRevision && image.writerMode == KisPageWriteMode::DiscardContents;
        const auto value = replicas[writerBase].owner;
        if (authority(value) != writerBase || !replicas[writerBase].pins) return false;
        --replicas[writerBase].pins;
        return true;
    }
    void clearWriter()
    {
        image.writerToken = {}; image.writerOperation = {}; image.writerTransaction = {};
        image.writerDefaultGeneration = {}; image.writerDefaultRevision = 0;
        image.writerBaseAuthority = {}; image.writerTarget = {};
        image.writerMode = KisPageWriteMode::PreserveContents; image.writerPhase = KisPageWriterPhase::None;
        writerBase = writerTarget = Missing;
    }
    void loadHandoff()
    {
        if (!image.handoffOperation.isValid() || handoffSource != Missing) return;
        const auto *source = shard.records.replica(image.handoffSource); Q_ASSERT(source);
        const auto value = findValue(shard.records.version(source->ownerVersion)->version);
        handoffSource = originalReplica(value, image.handoffSource);
        handoffTarget = originalReplica(value, image.handoffTarget);
    }
    static bool publishable(const Replica &r)
    { return !r.operation.isValid() && (r.validity == KisReplicaValidity::Allocated || r.validity == KisReplicaValidity::Valid); }
    template<class Visit> void forReplicas(qsizetype value, const Visit &visit)
    {
        if (const auto *original = values[value].original)
            for (auto slot = original->firstReplica; slot.isValid();) {
                const auto next = shard.records.replica(slot)->nextReplica;
                visit(originalReplica(value, slot)); slot = next;
            }
        for (qsizetype i = 0; i < qsizetype(replicas.size()); ++i)
            if (replicas[i].owner == value && replicas[i].introduced) visit(i);
    }
    bool idle(qsizetype value, bool validity)
    {
        bool ready = true;
        forReplicas(value, [&](qsizetype r) {
            const auto &replica = replicas[r];
            if (replica.operation.isValid() || replica.pins || protectedReplica(r)
                || (validity && replica.validity != KisReplicaValidity::Valid && replica.validity != KisReplicaValidity::Failed)) ready = false;
        });
        return ready;
    }
    void retire(qsizetype value)
    {
        forReplicas(value, [&](qsizetype r) { effects.push_back({handle(r), {}}); });
        values[value].removed = true;
    }
    bool removedOwner(KisReplicaSlotId slot) const
    {
        const auto *stored = shard.records.replica(slot);
        return stored && std::any_of(values.begin(), values.end(),
            [&](const auto &v) { return v.original && v.original->slot == stored->ownerVersion && v.removed; });
    }
    bool physicalAvailable(const KisReplicaHandle &target, QString *error, const QString &localReason) const
    {
        if (!target.isValid()) return true;
        const auto key = target.physicalSlotIdentity();
        for (const auto &r : replicas)
            if (r.introduced && !values[r.owner].removed && r.introduced->physicalSlotIdentity() == key) {
                KisPageStoreDetail::setError(error, localReason); return false;
            }
        KisReplicaSlotId slot;
        if (!shard.records.physicalSlots.findExact(key, &slot) || removedOwner(slot)) return true;
        const auto *r = shard.records.replica(slot); const auto *v = shard.records.version(r->ownerVersion);
        KisPageStoreDetail::setError(error, v->version.key == this->key ? localReason
            : QStringLiteral("metadata index reservation or physical ownership conflict"));
        return false;
    }
    qsizetype editedProtection(KisMetadataOverflowSlotId slot) const
    {
        for (qsizetype i = 0; i < qsizetype(protections.size()); ++i)
            if (protections[i].slot == slot) return i;
        return Missing;
    }
    qsizetype findLease(qsizetype replica, KisPageLeaseId lease)
    {
        for (qsizetype i = 0; i < qsizetype(protections.size()); ++i)
            if (protections[i].replica == replica && !protections[i].removed && !protections[i].completion
                && protections[i].lease == lease.value) return i;
        const auto *r = replicas[replica].introduced ? nullptr : shard.records.replica(replicas[replica].slot);
        for (auto slot = r ? r->overflow : KisMetadataOverflowSlotId{}; slot.isValid();) {
            const auto *node = shard.records.overflow(slot);
            if (node->kind == KisMetadataOverflowKind::ReadLease && node->value == lease.value) {
                if (editedProtection(slot) != Missing) return Missing;
                protections.push_back({replica, slot, lease.value});
                return qsizetype(protections.size() - 1);
            }
            slot = node->next;
        }
        return Missing;
    }
    bool leaseExists(KisPageLeaseId lease) const
    {
        for (const auto &p : protections)
            if (!p.removed && !p.completion && p.lease == lease.value && !values[replicas[p.replica].owner].removed) return true;
        for (auto value = page.firstVersion; value.isValid();) {
            const auto *v = shard.records.version(value);
            for (auto replica = v->firstReplica; replica.isValid();) {
                const auto *r = shard.records.replica(replica);
                if (!removedOwner(replica)) for (auto slot = r->overflow; slot.isValid();) {
                    const auto *node = shard.records.overflow(slot);
                    if (node->kind == KisMetadataOverflowKind::ReadLease && node->value == lease.value
                        && editedProtection(slot) == Missing) return true;
                    slot = node->next;
                }
                replica = r->nextReplica;
            }
            value = v->nextVersion;
        }
        return false;
    }
    bool protectedReplica(qsizetype replica) const
    {
        for (const auto &p : protections) if (p.replica == replica && !p.removed) return true;
        const auto *r = replicas[replica].introduced ? nullptr : shard.records.replica(replicas[replica].slot);
        for (auto slot = r ? r->overflow : KisMetadataOverflowSlotId{}; slot.isValid();) {
            const auto *node = shard.records.overflow(slot);
            if (editedProtection(slot) == Missing) return true;
            slot = node->next;
        }
        return false;
    }
    void appendProtection(qsizetype p)
    {
        protections[p].appendOrder = appends.size(); appends.push_back(p);
    }
};

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
    return applyLocalSequence(key, &transition, 1);
}

KisPageMetadataTransitionResult KisPageMetadataCoordinator::applyOwnerSequence(const KisPageKey &key,
                                                                       const KisPageTransition *transitions,
                                                                       qsizetype count)
{
    if (transitions && count == 1 && transitions->kind == KisPageTransitionKind::ReleaseRead)
        return applyReadProtection(key, *transitions);
    return applyLocalSequence(key, transitions, count);
}

KisPageMetadataTransitionResult KisPageMetadataCoordinator::publishLeaseWrite(const KisPageTransition &write)
{
    auto begin = write;
    begin.kind = KisPageTransitionKind::BeginPublish;
    auto publish = begin;
    publish.kind = KisPageTransitionKind::PublishWrite;
    const KisPageTransition transitions[] = {begin, publish};
    return applyLocalSequence(write.version.key, transitions, 2, true);
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
    return result;
}

KisPageMetadataTransitionResult KisPageMetadataCoordinator::applyLocalSequence(
    const KisPageKey &key, const KisPageTransition *transitions, qsizetype count, bool acceptPreparedRetry)
try {
    KisPageMetadataTransitionResult result;
    if (count <= 0 || !transitions || std::any_of(transitions, transitions + count,
            [](const auto &t) { return !localTransition(t.kind); })) {
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
        MetadataArenaGrowth growth(storageOwner.borrowed());
        std::optional<MetadataLocalMutation> plan;
        MetadataArenas::ReleasedBlocks releasedBlocks;
        QMutexLocker lock(&shard->mutex);
        auto page = shard->pages.find(key);
        if (page == shard->pages.end() || !shard->canMutate(key, page->second)) {
            result.rejectionReason = QStringLiteral("page is missing or reserved for publication");
            return result;
        }
        if (acceptPreparedRetry) {
            const auto &write = transitions[0];
            KisVersionSlotId slot;
            const auto *value = shard->records.findVersion(write.version, &slot) ? shard->records.version(slot) : nullptr;
            const auto *authority = value ? shard->records.replica(value->authorityReplica) : nullptr;
            if (value && value->publication == KisPagePublicationState::Prepared && value->preparedBy == write.transaction
                && authority && ShardRecordStore::matchesReplica(*authority, value->version, write.target)) {
                result.accepted = true;
                return result;
            }
        }
        const auto revision = page->second.revision;
        plan.emplace(*shard, page->second, key, storageOwner.borrowed());
        plan->capture(transitions, count);
        const bool background = kisOnPageStoreReclamationThread();
        ++shard->localTransitionSequences;
        if (background) ++shard->backgroundLocalTransitionSequences;
        for (qsizetype i = 0; i < count; ++i) {
            if (!plan->apply(transitions[i], &result.rejectionReason)) {
                shard->localVersionInputs += plan->inputs();
                if (background) shard->backgroundLocalVersionInputs += plan->inputs();
                ++shard->rejectedTransitions;
                return result;
            }
        }
        const auto inputs = plan->inputs();
        shard->localVersionInputs += inputs;
        if (background) shard->backgroundLocalVersionInputs += inputs;
        const auto discardStale = [&] {
            --shard->localTransitionSequences; shard->localVersionInputs -= inputs;
            if (background) { --shard->backgroundLocalTransitionSequences; shard->backgroundLocalVersionInputs -= inputs; }
        };
        const auto demand = plan->demand();
        const auto ready = shard->records.hasCapacity(demand) ? MetadataGrowthResult::Ready
            : growMetadataArenasOutsideLock(shard, demand, &growth, &lock, [&] {
                return shard->canMutate(key, page->second) && page->second.revision == revision;
            }, nullptr);
        if (ready == MetadataGrowthResult::Stale) { discardStale(); continue; }
        if (ready != MetadataGrowthResult::Ready) {
            ++shard->rejectedTransitions; result.storageRefused = true;
            result.rejectionReason = QStringLiteral("metadata arena budget is exhausted");
            return result;
        }
        // Only a real surviving writer/handoff needs the original sparse
        // activity payload. No full activity-header projection or relookup.
        if (plan->hasActivity()) shard->activities.try_emplace(key);
        const bool retirement = !plan->effects.empty();
        bool activityRetained = false;
        auto cleanupActivity = qScopeGuard([&] {
            if (!lock.isLocked()) lock.relock();
            if (activityRetained) shard->releasePublicationActivity(key, page->second);
            else {
                const auto activity = shard->activities.find(key);
                if (activity != shard->activities.end() && activity->second.isEmpty()) shard->activities.erase(activity);
            }
        });
        if (retirement && !shard->reservePublicationActivity(page->second)) {
            result.storageRefused = true;
            result.rejectionReason = QStringLiteral("metadata activity reservation is exhausted");
            return result;
        }
        activityRetained = retirement;
        plan->prepareRelease();
        ShardRecordStore::ReservationSet reservation;
        if (!shard->records.reserve(demand, qsizetype(demand.versions), plan->physicalInsertions(), &reservation)) {
            ++shard->rejectedTransitions;
            result.storageRefused = true;
            result.rejectionReason = QStringLiteral("metadata index reservation or physical ownership conflict");
            return result;
        }
        auto cancelReservation = qScopeGuard([&] {
            if (!lock.isLocked()) lock.relock();
            reservation.cancel(&shard->records);
        });
        bool stale = false;
        auto install = [&] {
            if (!lock.isLocked()) lock.relock();
            if (!shard->canMutate(key, page->second) || page->second.revision != revision) {
                stale = true;
                if (retirement) lock.unlock();
                return false;
            }
            if (!plan->validate(&result.rejectionReason)) {
                if (retirement) lock.unlock();
                return false;
            }
            plan->install(reservation);
            ++page->second.revision;
            shard->acceptedTransitions += quint64(count);
            result.accepted = true;
            if (retirement) lock.unlock();
            return true;
        };
        QString debtError;
        bool accepted;
        if (!retirement) accepted = install();
        else {
            lock.unlock();
            accepted = m_acceptRetirement && m_acceptRetirement(m_retirementContext,
                plan->effects.data(), qsizetype(plan->effects.size()), &install,
                [](void *p) { return (*static_cast<decltype(install) *>(p))(); }, &debtError);
        }
        if (!lock.isLocked()) lock.relock();
        if (!accepted) {
            if (stale) { discardStale(); continue; }
            ++shard->rejectedTransitions;
            if (result.rejectionReason.isEmpty()) {
                result.storageRefused = true;
                result.rejectionReason = debtError.isEmpty()
                    ? QStringLiteral("retirement acceptance receiver is absent or refused") : debtError;
            }
            return result;
        }
        reservation.cancel(&shard->records);
        cancelReservation.dismiss();
        if (activityRetained) shard->releasePublicationActivity(key, page->second);
        cleanupActivity.dismiss();
        releasedBlocks = shard->arenas.takeEmptyBlocks(&shard->budgetCharge);
        lock.unlock();
        if (retirement) m_deliverRetirement(m_retirementContext, plan->effects.data(), qsizetype(plan->effects.size()));
        return result;
    }
} catch (const std::bad_alloc &) {
    KisPageMetadataTransitionResult refused;
    refused.storageRefused = true;
    refused.rejectionReason = QStringLiteral("metadata local mutation storage was refused");
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
            if (page->second.hasMetadata())
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
            metrics.publicationCommittedValuesPrepared += shard->publicationCommittedValuesPrepared;
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
            metrics.publicationCommittedValuesInstalled += shard->publicationCommittedValuesInstalled;
            metrics.publicationAdditionRecordsTransferred += shard->publicationAdditionRecordsTransferred;
            metrics.metadataArenaGrowthBatches += shard->metadataArenaGrowthBatches;
            metrics.metadataArenaBlockCandidatesPrepared += shard->metadataArenaBlockCandidatesPrepared;
            metrics.metadataArenaBlocksAttached += shard->metadataArenaBlocksAttached;
            metrics.metadataArenaGrowthConflicts += shard->metadataArenaGrowthConflicts;
            metrics.metadataArenaGrowthFailures += shard->metadataArenaGrowthFailures;
            metrics.cpuReadBindings += shard->arenas.cpuReadBindings;
            metrics.fullSnapshotExports += shard->fullSnapshotExports;
            metrics.fullSnapshotVersionInputs += shard->fullSnapshotVersionInputs;
            metrics.backgroundSnapshotExports += shard->backgroundSnapshotExports;
            metrics.backgroundSnapshotVersionInputs += shard->backgroundSnapshotVersionInputs;
        }
    return metrics;
}

KisPageMetadataFootprint KisPageMetadataCoordinator::footprint(const KisImageEpochRootSnapshot *view) const
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
            result.committedValues += page.committedCount;
            if (view && page.hasMetadata()) {
                KisPageVersion selected;
                KisVersionSlotId slot;
                if (view->resolve(entry.first, &selected) && shard->records.findVersion(selected, &slot)) {
                    const auto *value = shard->records.version(slot);
                    result.selectedCommittedValues += value->publication == KisPagePublicationState::Historical;
                }
            }
            for (KisVersionSlotId versionSlot = page.firstVersion; versionSlot.isValid();) {
                const KisPageValueCell *version = shard->records.version(versionSlot);
                Q_ASSERT(version);
                if (!version)
                    break;
                result.trackedPayloadBytes += sizeof(KisPageValueCell);
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

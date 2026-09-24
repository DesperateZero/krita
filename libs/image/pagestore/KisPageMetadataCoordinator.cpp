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

#include <QAtomicInteger>
#include <QElapsedTimer>
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
#include <vector>

namespace
{

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

class MetadataBudgetAuthority final
{
public:
    explicit MetadataBudgetAuthority(KisBackingBudgetController *budget)
        : m_budget(budget) {}

    void attach(KisBackingBudgetController *budget)
    {
        QMutexLocker locker(&m_mutex);
        Q_ASSERT(budget);
        m_budget = budget;
    }

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

    void commitReservation(KisBackingBudgetReservation &&reservation,
                           const KisBackingBudgetDelta &installed) noexcept
    {
        QMutexLocker locker(&m_mutex);
        Q_ASSERT(m_budget);
        if (m_budget)
            m_budget->commitReservation(std::move(reservation), installed);
    }

    void releaseLive(quint64 bytes) noexcept
    {
        QMutexLocker locker(&m_mutex);
        if (m_budget) {
            m_budget->releaseLive(KisBackingBudgetClass::MetadataArena,
                                  KisPageAccessDomain::CpuRam, bytes);
        }
    }

private:
    QMutex m_mutex;
    KisBackingBudgetController *m_budget = nullptr;
};

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
    // These are hard ceilings, not reservations. Directory storage is the
    // only eagerly reserved metadata; all block payloads remain lazy.
    static constexpr quint64 VersionBudget = 4 * 1024 * 1024;
    static constexpr quint64 ReplicaBudget = 8 * 1024 * 1024;
    static constexpr quint64 OverflowBudget = 4 * 1024 * 1024;

    VersionArena versions{VersionBudget};
    ReplicaArena replicas{ReplicaBudget};
    OverflowArena overflow{OverflowBudget};

    static constexpr quint64 maximumDirectoryBytes()
    {
        return VersionArena::directoryBytesForLimit(VersionBudget)
            + ReplicaArena::directoryBytesForLimit(ReplicaBudget)
            + OverflowArena::directoryBytesForLimit(OverflowBudget);
    }

    quint64 allocatedDirectoryBytes() const
    {
        return versions.allocatedDirectoryBytes()
            + replicas.allocatedDirectoryBytes()
            + overflow.allocatedDirectoryBytes();
    }

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

MetadataArenaGrowthPlan metadataArenaGrowthPlan(const MetadataArenas &arenas, const MetadataArenaDemand &demand)
{
    return {arenaBlocksRequired(arenas.versions, demand.versions),
            arenaBlocksRequired(arenas.replicas, demand.replicas),
            arenaBlocksRequired(arenas.overflow, demand.overflow)};
}

MetadataArenaGrowthPlan fullMetadataArenaGrowthPlan(const MetadataArenaDemand &demand)
{
    const auto blocks = [](quint64 slots, quint64 slotsPerBlock) {
        return (slots + slotsPerBlock - 1) / slotsPerBlock;
    };
    return {blocks(demand.versions, VersionArena::slotsPerBlock()),
            blocks(demand.replicas, ReplicaArena::slotsPerBlock()),
            blocks(demand.overflow, OverflowArena::slotsPerBlock())};
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
    // Candidate payloads are destroyed before the reservation that accounts
    // for them when preparation exits early.
    KisBackingBudgetReservation budgetReservation;
    std::vector<VersionArena::PreparedBlock> versions;
    std::vector<ReplicaArena::PreparedBlock> replicas;
    std::vector<OverflowArena::PreparedBlock> overflow;
    quint64 attachedBlocks = 0;

    quint64 preparedBlockCount() const
    {
        return quint64(versions.size()) + quint64(replicas.size()) + quint64(overflow.size());
    }

    template<class Arena>
    static bool prepareBlocks(std::vector<typename Arena::PreparedBlock> *target, quint64 count)
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
        return prepareBlocks<VersionArena>(&versions, plan.versionBlocks)
            && prepareBlocks<ReplicaArena>(&replicas, plan.replicaBlocks)
            && prepareBlocks<OverflowArena>(&overflow, plan.overflowBlocks);
    }

    enum class AttachResult {
        Ready,
        NeedsMore,
        Rejected
    };

    template<class Arena>
    static AttachResult attachBlocks(Arena *arena,
                                     quint64 slots,
                                     std::vector<typename Arena::PreparedBlock> *candidates,
                                     quint64 *attachedBlocks)
    {
        Q_ASSERT(arena);
        Q_ASSERT(candidates);
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
        const auto versionResult = attachBlocks(&arenas->versions, demand.versions, &versions, &attachedBlocks);
        auto result = versionResult;
        if (result == AttachResult::Ready) {
            result = attachBlocks(&arenas->replicas, demand.replicas,
                                  &replicas, &attachedBlocks);
        }
        if (result == AttachResult::Ready) {
            result = attachBlocks(&arenas->overflow, demand.overflow,
                                  &overflow, &attachedBlocks);
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

quint64 overflowCount(const KisPageVersionStateSnapshot &snapshot)
{
    quint64 result = quint64(snapshot.capturedReadViews.size());
    for (const KisReplicaStateSnapshot &replica : snapshot.replicas) {
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

/**
 * One record/index owner per shard. Pages contain only intrusive slot heads,
 * tails and counts; no page owns a list, map, set, hash or shared heap object.
 */
struct ShardRecordStore {
    using ExactIndex = KisShardSlotIndex<KisPageVersion, KisVersionSlotId>;
    using PhysicalIndex = KisShardSlotIndex<PhysicalSlot, KisReplicaSlotId>;
    using HistoryPosition = std::pair<quint64, quint64>;

    explicit ShardRecordStore(MetadataArenas *storage)
        : arenas(storage)
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
            reservation->exactVersions = exactVersions.reserveInsertions(exactInsertions);
            if (!reservation->exactVersions.isValid()) {
                reservation->cancel(this);
                return false;
            }
        }
        if (physicalInsertions > 0) {
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

    void assign(KisVersionRecord *record,
                const KisPageVersionStateSnapshot &snapshot,
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
        for (const KisReplicaStateSnapshot &source : snapshot.replicas) {
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

    KisPageVersionStateSnapshot project(const KisVersionRecord &record) const
    {
        KisPageVersionStateSnapshot result;
        result.version = record.version;
        result.publication = record.publication;
        result.preparedBy = record.preparedBy;
        for (KisMetadataOverflowSlotId slot = record.capturedReadViews; slot.isValid();) {
            const KisMetadataOverflowNode *node = overflow(slot);
            Q_ASSERT(node && node->kind == KisMetadataOverflowKind::CapturedReadView);
            if (!node)
                break;
            result.capturedReadViews.append(KisImageEpochSnapshotToken{node->value});
            slot = node->next;
        }
        result.replicas.reserve(qsizetype(record.replicaCount));
        for (KisReplicaSlotId slot = record.firstReplica; slot.isValid();) {
            const KisReplicaRecord *stored = replica(slot);
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
                    projected.readLeases.append(KisPageLeaseId{node->value});
                } else if (node->kind == KisMetadataOverflowKind::PendingLastUse) {
                    projected.pendingLastUses.append(node->completion);
                }
                overflowSlot = node->next;
            }
            if (slot == record.authorityReplica) {
                result.authority = projected.replica;
            }
            result.replicas.append(std::move(projected));
            slot = stored->nextReplica;
        }
        return result;
    }

    bool snapshot(const KisPageVersion &identity, KisPageVersionStateSnapshot *snapshot) const
    {
        KisVersionSlotId slot;
        if (!snapshot || !findVersion(identity, &slot))
            return false;
        const KisVersionRecord *record = version(slot);
        if (!record)
            return false;
        *snapshot = project(*record);
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

    KisReplicaHandle projectReplica(KisReplicaSlotId slot) const
    {
        if (!slot.isValid())
            return {};
        const KisReplicaRecord *stored = replica(slot);
        const KisVersionRecord *owner = stored ? version(stored->ownerVersion) : nullptr;
        Q_ASSERT(stored && owner);
        return stored && owner ? replicaHandle(*stored, owner->version) : KisReplicaHandle{};
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
            result.versions.append(project(*record));
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

    bool canInstallPhysical(const KisPageVersionStateSnapshot &snapshot,
                            KisVersionSlotId replacing,
                            QSet<PhysicalSlot> *batch = nullptr) const
    {
        QSet<PhysicalSlot> local;
        QSet<PhysicalSlot> *seen = batch;
        if (!seen && snapshot.replicas.size() > 1) seen = &local;
        for (const KisReplicaStateSnapshot &replicaState : snapshot.replicas) {
            const PhysicalSlot key = replicaState.replica.physicalSlotIdentity();
            if (seen && seen->contains(key)) return false;
            if (seen) seen->insert(key);
            KisReplicaSlotId existing;
            if (!physicalSlots.findExact(key, &existing))
                continue;
            const KisReplicaRecord *stored = replica(existing);
            if (!stored || !(stored->ownerVersion == replacing))
                return false;
        }
        return true;
    }

    bool putReserved(MetadataPage *page, const KisPageVersionStateSnapshot &snapshot, ReservationSet *reservation)
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
    bool putBatch(MetadataPage *page, const Snapshots &snapshots)
    {
        qsizetype newVersions = 0;
        qsizetype replicaCount = 0;
        QSet<PhysicalSlot> batchPhysicalSlots;
        batchPhysicalSlots.reserve(snapshots.size());
        for (auto snapshotIt = snapshots.cbegin(); snapshotIt != snapshots.cend(); ++snapshotIt) {
            KisVersionSlotId existing;
            if (!findVersion(snapshotIt->version, &existing))
                ++newVersions;
            if (!canInstallPhysical(*snapshotIt, existing, &batchPhysicalSlots))
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

    bool installPreparedAdditionsReserved(MetadataPage *page,
                                          std::vector<KisPageVersionStateSnapshot> *additions,
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
                           quint64 directoryBytes)
        : budgetAuthority(std::move(authority))
        , budgetCharge(budgetAuthority, directoryBytes) {}

    mutable QMutex mutex;
    std::shared_ptr<MetadataBudgetAuthority> budgetAuthority;
    // Declared before arenas so arena payloads are destroyed before their
    // remaining live charge is released.
    MetadataBudgetRelease budgetCharge;
    MetadataArenas arenas;
    ShardRecordStore records{&arenas};
    QHash<KisPageKey, MetadataPage> pages;
    QHash<KisPageKey, MetadataPageActivity> activities;
    QHash<KisPageVersion, QSharedPointer<KisCpuReadBindingLink>> cpuBindings;
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

    bool reservePublicationActivity(const KisPageKey &key)
    {
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

bool readableVersion(const MetadataShard &shard, const KisPageVersion &version, KisPageVersionStateSnapshot *snapshot)
{
    const auto page = shard.pages.constFind(version.key);
    if (page == shard.pages.constEnd() || !shard.records.snapshot(version, snapshot))
        return false;
    return snapshot->publication == KisPagePublicationState::Published
        || snapshot->publication == KisPagePublicationState::Historical
        || (snapshot->publication == KisPagePublicationState::Prepared && !snapshot->capturedReadViews.isEmpty());
}

bool cpuReadableReplica(const KisReplicaStateSnapshot &state)
{
    return state.validity == KisReplicaValidity::Valid && state.replica.isValid()
        && (state.replica.domain == KisPageAccessDomain::CpuRam
            || state.replica.domain == KisPageAccessDomain::UmaShared);
}

} // namespace

class KisPageMetadataCoordinator::Private
{
public:
    Private()
        : budgetAuthority(
            std::make_shared<MetadataBudgetAuthority>(&standaloneBudget)) {}

    ~Private()
    {
        operational.store(false, std::memory_order_release);
        shards.clear();
        directoryCharge.release();
        budgetAuthority->detach();
    }

    MetadataShard *shardFor(const KisPageKey &key) const
    {
        // configure() publishes an immutable shard directory once. Readers
        // acquire that publication, never a process-wide configuration gate.
        // Destruction still requires owner quiescence; this is not an RCU
        // lifetime or permission to reconfigure the directory in place.
        if (!operational.load(std::memory_order_acquire) || !key.isValid())
            return nullptr;
        return shards.at(kisStablePageKeyHash(key) % shards.size()).get();
    }

    std::shared_ptr<MetadataShard> sharedShardFor(const KisPageKey &key) const
    {
        if (!operational.load(std::memory_order_acquire) || !key.isValid())
            return {};
        return shards.at(kisStablePageKeyHash(key) % shards.size());
    }

    mutable QMutex configurationMutex;
    std::atomic<bool> operational{false};
    KisBackingBudgetController standaloneBudget;
    std::shared_ptr<MetadataBudgetAuthority> budgetAuthority;
    // Declared before shards so the directory allocation is destroyed before
    // its final live charge is released.
    MetadataBudgetRelease directoryCharge;
    std::vector<std::shared_ptr<MetadataShard>> shards;
    QAtomicInteger<quint64> acceptedTransitions{0};
    QAtomicInteger<quint64> rejectedTransitions{0};
    QAtomicInteger<quint64> transitionDecisionNanoseconds{0};
    std::atomic<quint64> maximumTransitionDecisionNanoseconds{0};
    QAtomicInteger<quint64> preparedPublicationPages{0};
    QAtomicInteger<quint64> installedPublicationPages{0};
    QAtomicInteger<quint64> rejectedPublicationInstalls{0};
    QAtomicInteger<quint64> preparedMutationPages{0};
    QAtomicInteger<quint64> installedMutationPages{0};
    QAtomicInteger<quint64> rejectedMutationInstalls{0};
    QAtomicInteger<quint64> publicationHistoryNodesPrepared{0};
    QAtomicInteger<quint64> publicationAdditionRecordsPrepared{0};
    QAtomicInteger<quint64> registeredPages{0};
    void *retirementDebtContext = nullptr;
    PrepareRetirementDebt prepareRetirementDebt = nullptr;
    FinalizeRetirementDebt commitRetirementDebt = nullptr;
    FinalizeRetirementDebt cancelRetirementDebt = nullptr;
    // Shared identity prevents a capability surviving destruction from being
    // accepted by another coordinator constructed at the same address.
    const std::shared_ptr<const quint8> publicationOwner = std::make_shared<const quint8>(0);
};

class KisPageMetadataCoordinator::PreparedPublication::Data
{
public:
    ~Data();

    struct Entry {
        std::shared_ptr<MetadataShard> shardOwner;
        quint64 revision = 0;
        KisPageStateSnapshot next;
        struct PublicationDelta {
            KisPageVersion version;
            KisPagePublicationState publication;
            KisPageTransactionId preparedBy;
        };
        QVector<PublicationDelta> deltas;
        // Candidate values are contiguous; authoritative ordering is formed
        // by intrusive arena links only after full-batch revalidation.
        std::vector<KisPageVersionStateSnapshot> additions;
        MetadataArenaDemand arenaDemand;
        size_t arenaGrowthIndex = std::numeric_limits<size_t>::max();
        bool activityReservation = false;
        // Mutation detachment changes only publication/preparedBy, not the
        // page snapshot. Reader/pin/last-use churn must remain live at install.
        KisPageVersion detachedVersion;
        KisPageKey key() const
        {
            return detachedVersion.isValid() ? detachedVersion.key : next.key;
        }
    };
    struct ShardArenaGrowth {
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
    std::vector<Entry> entries;
    std::vector<ShardArenaGrowth> arenaGrowth;
    QVector<KisPageTransitionEffect> effects;
    QHash<KisPageVersion, KisReplicaHandle> backingAuthorities;
};

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

const QVector<KisPageTransitionEffect> &
KisPageMetadataCoordinator::PreparedPublication::retirementEffects() const
{
    static const QVector<KisPageTransitionEffect> empty;
    return data ? data->effects : empty;
}

KisReplicaHandle
KisPageMetadataCoordinator::PreparedPublication::backingAuthority(
    const KisPageVersion &version) const
{
    return data ? data->backingAuthorities.value(version) : KisReplicaHandle{};
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
        data.reset();
        return 1;
    }
    const qsizetype count = std::min(maximumWorkUnits, qsizetype(data->entries.size()));
    for (qsizetype i = 0; i < count; ++i) {
        // Destroy the heavy per-page payload before moving to the next unit.
        // removeLast() then sees an already moved-from entry.
        auto entry = std::move(data->entries.back());
        data->entries.pop_back();
        Q_UNUSED(entry);
    }
    if (data->entries.empty())
        data.reset();
    return count;
}
KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::preparePublication(const KisPageTransaction &transaction,
                                               KisImageEpochId minimumEpoch,
                                               const QVector<KisPageTransition> &transitions,
                                               QString *error) const
{
    return preparePublicationImpl(transaction, minimumEpoch, transitions, false, error);
}

KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::prepareRestoration(KisImageEpochId minimumEpoch,
                                               const QVector<KisPageTransition> &transitions,
                                               QString *error) const
{
    return preparePublicationImpl({}, minimumEpoch, transitions, true, error);
}

KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::prepareMutation(const KisPageTransaction &transaction,
                                            const QVector<KisPageTransition> &transitions,
                                            QString *error) const
{
    return preparePublicationImpl(transaction, {}, transitions, false, error, true);
}

bool KisPageMetadataCoordinator::installMutation(PreparedPublication &&prepared,
                                                 const KisPageTransaction &transaction,
                                                 QString *error,
                                                 DeferredPublicationCleanup *deferredCleanup)
{
    return installPublicationImpl(std::move(prepared), transaction, {}, nullptr, error, true, deferredCleanup);
}

KisPageMetadataCoordinator::PreparedPublication
KisPageMetadataCoordinator::preparePublicationImpl(const KisPageTransaction &transaction,
                                                   KisImageEpochId minimumEpoch,
                                                   const QVector<KisPageTransition> &transitions,
                                                   bool restoration,
                                                   QString *error,
                                                   bool mutation) const
{
    PreparedPublication result;
    if (!isOperational() || (!restoration && !transaction.isValid())
        || (!mutation && (!minimumEpoch.isValid() || minimumEpoch.value <= transaction.baseEpoch.value))) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata publication identity is invalid"));
        return result;
    }
    auto data = std::make_unique<PreparedPublication::Data>();
    quint64 historyNodesPrepared = 0;
    quint64 additionRecordsPrepared = 0;
    const auto recordPreparationWork = qScopeGuard([&] {
        // Include completed work from a later-rejected batch, but fold once
        // outside shard gates instead of adding an atomic per changed page.
        if (historyNodesPrepared)
            d->publicationHistoryNodesPrepared.fetchAndAddRelaxed(historyNodesPrepared);
        if (additionRecordsPrepared)
            d->publicationAdditionRecordsPrepared.fetchAndAddRelaxed(additionRecordsPrepared);
    });
    data->owner = d->publicationOwner;
    data->transaction = transaction;
    data->minimumEpoch = minimumEpoch;
    data->entries.reserve(transitions.size());
    QSet<KisPageKey> seen;
    const KisPageStateMachine stateMachine;
    for (const KisPageTransition &transition : transitions) {
        const bool write = transition.kind == KisPageTransitionKind::CommitTransaction;
        const bool invalidMutation = mutation
            && (transition.kind != KisPageTransitionKind::DetachPreparedVersion
                || !(transition.transaction == transaction.id) || transition.imageEpoch.isValid()
                || transition.operation.isValid() || !transition.version.isValid());
        if (invalidMutation
            || (!mutation
                && ((restoration && transition.kind != KisPageTransitionKind::RestoreCommittedVersion)
                    || (!write && transition.kind != KisPageTransitionKind::ReplaceDefaultPixel
                        && transition.kind != KisPageTransitionKind::RestoreCommittedVersion)
                    || (write && !(transition.transaction == transaction.id))
                    || (!write && transition.transaction.isValid()) || !(transition.imageEpoch == minimumEpoch)))
            || seen.contains(transition.version.key)) {
            KisPageStoreDetail::setError(error, QStringLiteral("metadata publication transition is invalid or repeated"));
            return result;
        }
        seen.insert(transition.version.key);
        std::shared_ptr<MetadataShard> shardOwner = d->sharedShardFor(transition.version.key);
        MetadataShard *shard = shardOwner.get();
        if (!shard)
            return result;
        PreparedPublication::Data::Entry entry;
        entry.shardOwner = std::move(shardOwner);
        KisPageStateSnapshot before;
        {
            QMutexLocker locker(&shard->mutex);
            const auto page = shard->pages.constFind(transition.version.key);
            if (page == shard->pages.constEnd() || !shard->canMutate(transition.version.key, page.value())) {
                KisPageStoreDetail::setError(error, QStringLiteral("metadata publication page is unavailable"));
                return result;
            }
            if (mutation) {
                // Match DetachPreparedVersion's semantic guards without
                // cloning all versions/protection lists into a stale image.
                KisPageVersionStateSnapshot version;
                if (shard->hasWriter(transition.version.key) || !shard->records.snapshot(transition.version, &version)
                    || version.publication != KisPagePublicationState::Prepared
                    || !(version.preparedBy == transaction.id)) {
                    KisPageStoreDetail::setError(error, QStringLiteral("prepared detachment identity is invalid or still writable"));
                    return result;
                }
                entry.detachedVersion = transition.version;
            } else {
                entry.revision = page->revision;
                before = shard->header(transition.version.key, page.value());
                QSet<KisPageVersion> included;
                const auto include = [&](const KisPageVersion &identity) {
                    if (included.contains(identity))
                        return;
                    KisPageVersionStateSnapshot v;
                    if (shard->records.snapshot(identity, &v)) {
                        included.insert(identity);
                        before.versions.append(std::move(v));
                    }
                };
                include({before.key, before.publishedGeneration, before.publishedDefaultPixelRevision});
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
        if (mutation) {
            ++historyNodesPrepared;
            data->entries.push_back(std::move(entry));
            continue;
        }
        // The expensive deterministic work is outside the shard lock and is
        // reused at installation. Revision revalidation includes leases,
        // Only coordinator-owned state reaches this private capability. Like
        // immediate apply(), reuse its established boundary invariants; all
        // transition-local lifetime/identity guards still run. External state
        // enters through registerPage()'s full validation. The reference test
        // compares every supported publication kind with full apply().
        // Indexed inputs deliberately omit unrelated versions.
        KisPageTransitionResult step = stateMachine.applyKnownValid(before, transition);
        if (!step.accepted) {
            KisPageStoreDetail::setError(error, step.rejectionReason);
            return result;
        }
        for (const KisPageVersionStateSnapshot &version : std::as_const(before.versions)) {
            if (version.authority.isValid())
                data->backingAuthorities.insert(version.version, version.authority);
        }
        for (const KisPageVersionStateSnapshot &version : std::as_const(step.next.versions)) {
            if (version.authority.isValid())
                data->backingAuthorities.insert(version.version, version.authority);
        }
        QHash<KisPageVersion, const KisPageVersionStateSnapshot *> previous;
        for (const auto &version : std::as_const(before.versions))
            previous.insert(version.version, &version);
        for (const auto &version : std::as_const(step.next.versions)) {
            const auto found = previous.constFind(version.version);
            if (found == previous.cend()) {
                if (version.publication == KisPagePublicationState::Prepared
                    || version.publication == KisPagePublicationState::Unpublished) {
                    KisPageStoreDetail::setError(error, QStringLiteral("publication candidate introduced mutable metadata"));
                    return result;
                }
                entry.additions.push_back(version);
                if (version.publication == KisPagePublicationState::Historical)
                    ++historyNodesPrepared;
            } else if (found.value()->publication != version.publication
                       || !(found.value()->preparedBy == version.preparedBy)) {
                entry.deltas.append({version.version, version.publication, version.preparedBy});
                if (version.publication == KisPagePublicationState::Historical)
                    ++historyNodesPrepared;
            }
        }
        // Installation must not replace dynamic protection/replica lists.
        // Strict page revision is still required; this is not stale merge.
        additionRecordsPrepared += quint64(entry.additions.size());
        // Headers never own version records at install.
        step.next.versions.clear();
        entry.next = std::move(step.next);
        {
            QMutexLocker locker(&entry.shardOwner->mutex);
            const auto page = entry.shardOwner->pages.find(entry.key());
            if (page == entry.shardOwner->pages.end() || !entry.shardOwner->canMutate(entry.key(), page.value())
                || page->revision != entry.revision) {
                KisPageStoreDetail::setError(error, QStringLiteral("metadata changed while preparing publication storage"));
                result.m_conflicted = true;
                return result;
            }
            entry.arenaDemand = entry.shardOwner->records.batchDemand(entry.additions);
        }
        auto shardGrowth = std::find_if(data->arenaGrowth.begin(), data->arenaGrowth.end(), [&](const auto &candidate) {
            return candidate.shardOwner == entry.shardOwner;
        });
        if (shardGrowth == data->arenaGrowth.end()) {
            PreparedPublication::Data::ShardArenaGrowth nextGrowth;
            nextGrowth.shardOwner = entry.shardOwner;
            data->arenaGrowth.push_back(std::move(nextGrowth));
            shardGrowth = std::prev(data->arenaGrowth.end());
        }
        entry.arenaGrowthIndex = size_t(std::distance(data->arenaGrowth.begin(), shardGrowth));
        shardGrowth->totalDemand.versions += entry.arenaDemand.versions;
        shardGrowth->totalDemand.replicas += entry.arenaDemand.replicas;
        shardGrowth->totalDemand.overflow += entry.arenaDemand.overflow;
        shardGrowth->exactInsertions += qsizetype(entry.additions.size());
        for (const auto &record : entry.additions)
            shardGrowth->physicalInsertions += record.replicas.size();
        data->effects += step.effects;
        data->entries.push_back(std::move(entry));
    }
    // One candidate batch per shard avoids reserving a 16/32 KiB block for
    // every small page. It is still self-sufficient if all observed free slots
    // are consumed before install, and all payload allocation happens here.
    for (auto &growth : data->arenaGrowth) {
        const MetadataArenaGrowthPlan plan = fullMetadataArenaGrowthPlan(growth.totalDemand);
        bool fitsBudget = false;
        {
            QMutexLocker locker(&growth.shardOwner->mutex);
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
            KisPageStoreDetail::setError(error, QStringLiteral("metadata arena block allocation failed"));
            return result;
        }
    }
    // Attach and reserve the complete shard-local install capacity during
    // preparation. Once these tokens exist, ordinary mutations can use only
    // unreserved arena slots/index capacity; install merely consumes tokens.
    for (auto &growth : data->arenaGrowth) {
        QMutexLocker locker(&growth.shardOwner->mutex);
        for (const auto &entry : data->entries) {
            if (entry.shardOwner != growth.shardOwner)
                continue;
            const auto page = growth.shardOwner->pages.constFind(entry.key());
            if (page == growth.shardOwner->pages.constEnd() || !growth.shardOwner->canMutate(entry.key(), page.value())
                || page->revision != entry.revision) {
                ++growth.shardOwner->metadataArenaGrowthConflicts;
                KisPageStoreDetail::setError(error, QStringLiteral("metadata changed while reserving publication capacity"));
                result.m_conflicted = true;
                return result;
            }
        }

        const quint64 attachedBefore = growth.blocks.attachedBlocks;
        const auto attachResult = growth.blocks.attach(
            &growth.shardOwner->arenas, &growth.shardOwner->budgetCharge,
            growth.totalDemand);
        growth.shardOwner->metadataArenaBlocksAttached += growth.blocks.attachedBlocks - attachedBefore;
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
        if (stillValid && mutation) {
            KisPageVersionStateSnapshot version;
            stillValid = !entry.shardOwner->hasWriter(entry.key())
                && entry.shardOwner->records.snapshot(entry.detachedVersion, &version)
                && version.publication == KisPagePublicationState::Prepared && version.preparedBy == transaction.id;
        } else if (stillValid) {
            stillValid = page->revision == entry.revision;
        }
        if (!stillValid || !entry.shardOwner->reservePublicationActivity(entry.key())) {
            KisPageStoreDetail::setError(error, QStringLiteral("metadata changed while reserving publication activity"));
            result.m_conflicted = !stillValid;
            return result;
        }
        entry.activityReservation = true;
    }
    result.data = std::move(data);
    (mutation ? d->preparedMutationPages : d->preparedPublicationPages).fetchAndAddRelaxed(quint64(transitions.size()));
    KisPageStoreDetail::setError(error, {});
    return result;
}

bool KisPageMetadataCoordinator::installPublication(PreparedPublication &&prepared,
                                                    const KisPageTransaction &transaction,
                                                    KisImageEpochId epoch,
                                                    QVector<KisPageTransitionEffect> *effects,
                                                    QString *error,
                                                    DeferredPublicationCleanup *deferredCleanup)
{
    return installPublicationImpl(std::move(prepared),
                                  transaction,
                                  epoch,
                                  effects,
                                  error,
                                  false,
                                  deferredCleanup);
}

bool KisPageMetadataCoordinator::installPublicationImpl(PreparedPublication &&prepared,
                                                        const KisPageTransaction &transaction,
                                                        KisImageEpochId epoch,
                                                        QVector<KisPageTransitionEffect> *effects,
                                                        QString *error,
                                                        bool mutation,
                                                        DeferredPublicationCleanup *deferredCleanup)
{
    // Consume even on rejection: callers must prepare again after a conflict.
    auto data = std::move(prepared.data);
    const bool canDeferCleanup = deferredCleanup && deferredCleanup->isEmpty();
    const auto transferCleanup = qScopeGuard([&] {
        if (canDeferCleanup && data)
            deferredCleanup->data = std::move(data);
    });
    if ((deferredCleanup && !canDeferCleanup) || !data || data->owner != d->publicationOwner
        || data->minimumEpoch.isValid() == mutation
        || !(data->transaction == transaction)
        || (!mutation && (!epoch.isValid() || epoch.value < data->minimumEpoch.value))
        || (mutation && epoch.isValid())) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata publication capability is stale or foreign"));
        (mutation ? d->rejectedMutationInstalls : d->rejectedPublicationInstalls).fetchAndAddRelaxed(1);
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
        if (mutation) {
            if (entry.shardOwner->hasWriter(entry.key()))
                break;
            if (!entry.shardOwner->records.isPreparedBy(entry.detachedVersion, transaction.id))
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
        (mutation ? d->rejectedMutationInstalls : d->rejectedPublicationInstalls).fetchAndAddRelaxed(1);
        return false;
    }
    for (auto &entry : data->entries) {
        QMutexLocker locker(&entry.shardOwner->mutex);
        auto page = entry.shardOwner->pages.find(entry.key());
        Q_ASSERT(page != entry.shardOwner->pages.end() && entry.shardOwner->publicationClaim(entry.key()) == data.get());
        quint64 installedHistoryLinks = mutation ? 1 : 0;
        if (!mutation) {
            installedHistoryLinks +=
                quint64(std::count_if(entry.deltas.cbegin(), entry.deltas.cend(), [](const auto &delta) {
                    return delta.publication == KisPagePublicationState::Historical;
                }));
            installedHistoryLinks +=
                quint64(std::count_if(entry.additions.cbegin(), entry.additions.cend(), [](const auto &addition) {
                    return addition.publication == KisPagePublicationState::Historical;
                }));
        }
        // Epoch allocation may have skipped an identity on an earlier failed
        // root build. These transitions use the epoch only as a newer tag.
        if (mutation) {
            // Claims on every page are held before the first edit. Preserve
            // the CURRENT replicas, leases, pins, last-use, captured views and
            // epoch; apply only the already revalidated semantic delta. This
            // is not permission to merge arbitrary stale publication images.
            const bool updated = entry.shardOwner->records.setPublication(&page.value(),
                                                                     entry.detachedVersion,
                                                                     KisPagePublicationState::Historical,
                                                                     {});
            Q_ASSERT(updated);
            Q_UNUSED(updated);
            ++page->revision;
        } else {
            entry.next.publishedEpoch = epoch;
            for (const auto &delta : std::as_const(entry.deltas)) {
                const bool updated = entry.shardOwner->records.setPublication(&page.value(),
                                                                         delta.version,
                                                                         delta.publication,
                                                                         delta.preparedBy);
                Q_ASSERT(updated);
                Q_UNUSED(updated);
            }
            const auto additionRecords = quint64(entry.additions.size());
            auto &reservation = data->arenaGrowth.at(entry.arenaGrowthIndex).reservations;
            const bool additionsInstalled = entry.shardOwner->records.installPreparedAdditionsReserved(&page.value(),
                                                                                                  &entry.additions,
                                                                                                  &reservation);
            Q_ASSERT(additionsInstalled);
            Q_UNUSED(additionsInstalled);
            entry.shardOwner->publicationAdditionRecordsTransferred += additionRecords;
            const bool headerInstalled = entry.shardOwner->installReservedHeader(entry.key(), &page.value(), entry.next);
            Q_ASSERT(headerInstalled);
            Q_UNUSED(headerInstalled);
            ++page->revision;
            entry.shardOwner->publicationVersionInstalls += quint64(entry.deltas.size()) + additionRecords;
        }
        entry.shardOwner->publicationHistoryNodesTransferred += installedHistoryLinks;
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
    (mutation ? d->installedMutationPages : d->installedPublicationPages)
        .fetchAndAddRelaxed(quint64(data->entries.size()));
    if (effects)
        *effects = std::move(data->effects);
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPageMetadataCoordinator::KisPageMetadataCoordinator()
    : d(new Private)
{
}

KisPageMetadataCoordinator::~KisPageMetadataCoordinator() = default;

void KisPageMetadataCoordinator::attachBackingBudget(
    KisBackingBudgetController &budget)
{
    QMutexLocker locker(&d->configurationMutex);
    Q_ASSERT(!d->operational.load(std::memory_order_relaxed));
    Q_ASSERT(d->shards.empty());
    d->budgetAuthority->attach(&budget);
}

void KisPageMetadataCoordinator::attachRetirementDebtOwner(
    void *context,
    PrepareRetirementDebt prepare,
    FinalizeRetirementDebt commit,
    FinalizeRetirementDebt cancel)
{
    QMutexLocker locker(&d->configurationMutex);
    Q_ASSERT(!d->operational.load(std::memory_order_relaxed));
    Q_ASSERT(context && prepare && commit && cancel);
    Q_ASSERT(!d->retirementDebtContext);
    d->retirementDebtContext = context;
    d->prepareRetirementDebt = prepare;
    d->commitRetirementDebt = commit;
    d->cancelRetirementDebt = cancel;
}

QSharedPointer<KisCpuReadBindingLink>
KisPageMetadataCoordinator::installCpuReadBinding(const KisReplicaHandle &replica,
                                                  const QSharedPointer<KisPageReplicaProvider> &provider)
{
    if (!provider || !replica.isValid()
        || (replica.domain != KisPageAccessDomain::CpuRam && replica.domain != KisPageAccessDomain::UmaShared))
        return {};
    auto *shard = d->shardFor(replica.version.key);
    if (!shard)
        return {};
    QMutexLocker locker(&shard->mutex);
    // Provider lookup happened outside this lock. Revalidate the exact
    // replica before publishing a cache entry; never resurrect retired data.
    KisPageVersionStateSnapshot version;
    if (!readableVersion(*shard, replica.version, &version)
        || std::none_of(version.replicas.cbegin(), version.replicas.cend(), [&](const auto &state) {
               return state.replica == replica && cpuReadableReplica(state);
           }))
        return {};
    const auto existing = shard->cpuBindings.constFind(replica.version);
    if (existing != shard->cpuBindings.constEnd())
        return existing.value();
    auto link = QSharedPointer<KisCpuReadBindingLink>::create(replica, provider);
    shard->cpuBindings.insert(replica.version, link);
    return link;
}

void KisPageMetadataCoordinator::removeCpuReadBinding(const KisReplicaHandle &replica)
{
    auto *shard = d->shardFor(replica.version.key);
    if (!shard)
        return;
    QMutexLocker locker(&shard->mutex);
    const auto existing = shard->cpuBindings.constFind(replica.version);
    if (existing != shard->cpuBindings.constEnd() && existing.value()->replica == replica)
        shard->cpuBindings.remove(replica.version);
}

QSharedPointer<KisCpuReadBindingLink> KisPageMetadataCoordinator::cpuReadBinding(const KisPageVersion &version) const
{
    auto *shard = d->shardFor(version.key);
    if (!shard)
        return {};
    QMutexLocker locker(&shard->mutex);
    return shard->cpuBindings.value(version);
}

KisReplicaHandle KisPageMetadataCoordinator::cpuReadReplica(const KisPageVersion &version) const
{
    auto *shard = d->shardFor(version.key);
    if (!shard)
        return {};
    QMutexLocker locker(&shard->mutex);
    KisPageVersionStateSnapshot state;
    if (!readableVersion(*shard, version, &state))
        return {};
    // Cold discovery does not export/copy a diagnostic page snapshot. Prefer
    // a ready CPU authority, otherwise require an unambiguous ready CPU
    // replica. Authority elsewhere alone is not a materialization request.
    const auto *authority = state.findReplica(state.authority);
    if (authority && cpuReadableReplica(*authority)) return authority->replica;
    KisReplicaHandle selected;
    for (const auto &candidate : state.replicas) {
        if (!cpuReadableReplica(candidate))
            continue;
        if (selected.isValid())
            return {};
        selected = candidate.replica;
    }
    return selected;
}

bool KisPageMetadataCoordinator::configure(qsizetype shardCount, QString *error)
{
    if (shardCount <= 0 || shardCount > 4096) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata shard count is outside the supported range"));
        return false;
    }

    QMutexLocker locker(&d->configurationMutex);
    if (d->operational.load(std::memory_order_relaxed)) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata coordinator is already configured"));
        return false;
    }
    const quint64 count = quint64(shardCount);
    const quint64 perShardMaximum = MetadataArenas::maximumDirectoryBytes();
    const quint64 shardPointers = count * quint64(sizeof(std::shared_ptr<MetadataShard>));
    if (perShardMaximum > (quint64(std::numeric_limits<qint64>::max()) - shardPointers) / count) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata directory budget overflows"));
        return false;
    }
    KisBackingBudgetDelta directoryReservation;
    directoryReservation.buckets[size_t(KisBackingBudgetClass::MetadataArena)].cpuRam =
        qint64(perShardMaximum * count + shardPointers);
    auto budget = d->budgetAuthority->reserve(directoryReservation, error);
    if (!budget.isValid())
        return false;

    try {
        d->shards.reserve(size_t(shardCount));
        for (qsizetype i = 0; i < shardCount; ++i) {
            d->shards.push_back(std::make_shared<MetadataShard>(
                d->budgetAuthority, quint64(0)));
        }
    } catch (const std::bad_alloc &) {
        d->shards.clear();
        KisPageStoreDetail::setError(error, QStringLiteral("metadata shard directory allocation failed"));
        return false;
    }
    quint64 actualBytes = quint64(d->shards.capacity())
        * quint64(sizeof(std::shared_ptr<MetadataShard>));
    for (const auto &shard : d->shards)
        actualBytes += shard->arenas.allocatedDirectoryBytes();
    if (actualBytes > quint64(directoryReservation
            .buckets[size_t(KisBackingBudgetClass::MetadataArena)].cpuRam)) {
        d->shards.clear();
        KisPageStoreDetail::setError(error, QStringLiteral("metadata directory allocation exceeded its reservation"));
        return false;
    }
    KisBackingBudgetDelta installed;
    installed.buckets[size_t(KisBackingBudgetClass::MetadataArena)].cpuRam =
        qint64(actualBytes);
    d->budgetAuthority->commitReservation(std::move(budget), installed);
    const quint64 pointerBytes = quint64(d->shards.capacity())
        * quint64(sizeof(std::shared_ptr<MetadataShard>));
    d->directoryCharge = MetadataBudgetRelease(d->budgetAuthority, pointerBytes);
    for (const auto &shard : d->shards)
        shard->budgetCharge.add(shard->arenas.allocatedDirectoryBytes());
    d->operational.store(true, std::memory_order_release);
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPageMetadataCoordinator::isOperational() const
{
    return d->operational.load(std::memory_order_acquire);
}

qsizetype KisPageMetadataCoordinator::shardCount() const
{
    return isOperational() ? qsizetype(d->shards.size()) : 0;
}

qsizetype KisPageMetadataCoordinator::shardFor(const KisPageKey &key) const
{
    if (!isOperational() || !key.isValid())
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

    MetadataShard *shard = d->shardFor(initial.key);
    if (!shard) {
        KisPageStoreDetail::setError(error, QStringLiteral("metadata coordinator is not configured"));
        return false;
    }
    const MetadataArenaDemand demand = newMetadataDemand(initial.versions);
    MetadataArenaGrowth growth;
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
    return d->registeredPages.loadAcquire();
}

bool KisPageMetadataCoordinator::pageSnapshot(const KisPageKey &key, KisPageStateSnapshot *snapshot) const
{
    MetadataShard *shard = d->shardFor(key);
    if (!shard || !snapshot)
        return false;
    QMutexLocker locker(&shard->mutex);
    const auto pageIt = shard->pages.constFind(key);
    if (pageIt == shard->pages.constEnd())
        return false;
    *snapshot = shard->snapshot(key, pageIt.value());
    return true;
}

bool KisPageMetadataCoordinator::mutationBaseSnapshot(const KisPageVersion &base,
                                                      const KisPageVersion &sealed,
                                                      KisPageStateSnapshot *snapshot) const
{
    return projectVersionPair(base, sealed, snapshot, true);
}

bool KisPageMetadataCoordinator::projectVersionPair(const KisPageVersion &base,
                                                    const KisPageVersion &sealed,
                                                    KisPageStateSnapshot *snapshot,
                                                    bool countMutationInput) const
{
    auto *shard = d->shardFor(base.key);
    if (!shard || !snapshot || (sealed.isValid() && !(sealed.key == base.key)))
        return false;
    QMutexLocker lock(&shard->mutex);
    const auto page = shard->pages.constFind(base.key);
    if (page == shard->pages.cend())
        return false;
    *snapshot = shard->header(base.key, page.value());
    for (const auto &identity : {base, sealed}) {
        if (!identity.isValid() || (identity == sealed && sealed == base && !snapshot->versions.isEmpty()))
            continue;
        KisPageVersionStateSnapshot version;
        if (shard->records.snapshot(identity, &version)) {
            snapshot->versions.append(std::move(version));
        }
    }
    if (countMutationInput) {
        shard->mutationBaseVersionInputs += quint64(snapshot->versions.size());
        if (kisOnPageStoreReclamationThread())
            shard->backgroundMutationBaseVersionInputs += quint64(snapshot->versions.size());
    }
    return true;
}

bool KisPageMetadataCoordinator::versionSnapshot(const KisPageVersion &version, KisPageStateSnapshot *snapshot) const
{
    return projectVersionPair(version, {}, snapshot, false);
}

bool KisPageMetadataCoordinator::canAddTransientVersion(const KisPageVersion &target, quint32 limit) const
{
    auto *shard = d->shardFor(target.key);
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

bool KisPageMetadataCoordinator::publicationSnapshot(const KisPageKey &key,
                                                     const KisPageVersion &target,
                                                     KisPageStateSnapshot *snapshot) const
{
    auto *shard = d->shardFor(key);
    if (!shard || !snapshot || (target.isValid() && !(target.key == key)))
        return false;
    QMutexLocker lock(&shard->mutex);
    const auto page = shard->pages.constFind(key);
    if (page == shard->pages.cend())
        return false;
    *snapshot = shard->header(key, page.value());
    const KisPageVersion current{key, snapshot->publishedGeneration, snapshot->publishedDefaultPixelRevision};
    KisPageVersionStateSnapshot version;
    if (shard->records.snapshot(current, &version)) {
        snapshot->versions.append(std::move(version));
    }
    if (target.isValid() && !(target == current)) {
        if (shard->records.snapshot(target, &version)) {
            snapshot->versions.append(std::move(version));
        }
    }
    shard->publicationLookupVersionInputs += quint64(snapshot->versions.size());
    return true;
}

KisPageMetadataCoordinator::HistorySlice
KisPageMetadataCoordinator::historySlice(const KisPageKey &key, const KisPageVersion &after, qsizetype budget) const
{
    HistorySlice result;
    auto *shard = d->shardFor(key);
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
    while (nextHint.isValid() && result.versions.size() < budget) {
        const KisVersionRecord *record = shard->records.version(nextHint);
        Q_ASSERT(record);
        if (!record)
            break;
        result.versions.append(shard->records.project(*record));
        result.after = record->version;
        nextHint = record->nextHistory;
    }
    if (!nextHint.isValid()) result.after = {};
    ++shard->historySliceQueries;
    shard->historySliceVersionInputs += quint64(result.versions.size());
    shard->maximumHistorySliceVersionInputs =
        qMax(shard->maximumHistorySliceVersionInputs, quint64(result.versions.size()));
    return result;
}

QVector<KisPageStateSnapshot> KisPageMetadataCoordinator::publicationHeaders() const
{
    if (!d->operational.load(std::memory_order_acquire))
        return {};
    QVector<KisPageStateSnapshot> result;
    for (const auto &shard : d->shards) {
        QMutexLocker lock(&shard->mutex);
        result.reserve(result.size() + shard->pages.size());
        for (auto page = shard->pages.cbegin(); page != shard->pages.cend(); ++page)
            result.append(shard->header(page.key(), page.value()));
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

KisPageTransitionResult KisPageMetadataCoordinator::applyOwner(const KisPageKey &key,
                                                               const KisPageTransition &transition)
{
    return applyOwnerSequence(key, {transition});
}

KisPageTransitionResult KisPageMetadataCoordinator::applyOwnerSequence(const KisPageKey &key,
                                                                       const QVector<KisPageTransition> &transitions)
{
    if (transitions.isEmpty() || std::any_of(transitions.cbegin(), transitions.cend(), [](const auto &t) {
            return !localTransition(t.kind);
        })) {
        KisPageTransitionResult result;
        result.rejectionReason = QStringLiteral("owner transition is not a local metadata mutation");
        return result;
    }
    return applyProjectedSequence(key, transitions);
}

KisPageTransitionResult KisPageMetadataCoordinator::applyProjectedSequence(
    const KisPageKey &key, const QVector<KisPageTransition> &transitions)
{
    KisPageTransitionResult result;
    auto *shard = d->shardFor(key);
    if (!shard) {
        result.rejectionReason = QStringLiteral("metadata coordinator is not configured");
        return result;
    }
    for (;;) {
        MetadataArenaGrowth growth;
        QMutexLocker lock(&shard->mutex);
        auto page = shard->pages.find(key);
        if (page == shard->pages.end() || !shard->canMutate(key, page.value())) {
            result.rejectionReason = QStringLiteral("page is missing or reserved for publication");
            return result;
        }
        auto input = shard->header(key, page.value());
        QSet<KisPageVersion> included;
        const auto add = [&](const KisPageVersion &identity) {
            if (!identity.isValid() || included.contains(identity))
                return;
            included.insert(identity);
            KisPageVersionStateSnapshot version;
            if (shard->records.snapshot(identity, &version)) {
                input.versions.append(std::move(version));
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
        {
            QElapsedTimer timer;
            timer.start();
            const auto recordTime = qScopeGuard([&] {
                const auto elapsed = quint64(qMax<qint64>(0, timer.nsecsElapsed()));
                d->transitionDecisionNanoseconds.fetchAndAddRelaxed(elapsed);
                quint64 maximum = d->maximumTransitionDecisionNanoseconds.load(std::memory_order_relaxed);
                while (maximum < elapsed
                       && !d->maximumTransitionDecisionNanoseconds.compare_exchange_weak(maximum,
                                                                                         elapsed,
                                                                                         std::memory_order_relaxed,
                                                                                         std::memory_order_relaxed)) { }
            });
            const KisPageStateMachine machine;
            for (const auto &transition : transitions) {
                auto step = machine.applyKnownValid(next, transition);
                if (!step.accepted) {
                    d->rejectedTransitions.fetchAndAddRelaxed(1);
                    result.rejectionReason = step.rejectionReason;
                    result.effects.clear();
                    return result; // no authoritative record has changed
                }
                next = std::move(step.next);
                result.effects += step.effects;
            }
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
            result.effects.clear();
        };
        quint64 retirementDebtCookie = 0;
        bool retirementDebtPrepared = false;
        const auto cancelRetirementDebt = qScopeGuard([&] {
            if (retirementDebtPrepared && d->cancelRetirementDebt) {
                d->cancelRetirementDebt(d->retirementDebtContext,
                                        retirementDebtCookie);
            }
        });
        if (!result.effects.isEmpty() && d->prepareRetirementDebt) {
            QString debtError;
            lock.unlock();
            if (!d->prepareRetirementDebt(d->retirementDebtContext,
                                          result.effects,
                                          &retirementDebtCookie,
                                          &debtError)) {
                d->rejectedTransitions.fetchAndAddRelaxed(1);
                result.rejectionReason = debtError.isEmpty()
                    ? QStringLiteral("retirement debt budget is exhausted") : debtError;
                result.effects.clear();
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
            d->rejectedTransitions.fetchAndAddRelaxed(1);
            result.rejectionReason = QStringLiteral("metadata arena budget is exhausted");
            result.effects.clear();
            return result;
        }
        if (!shard->records.putBatch(&page.value(), next.versions)) {
            d->rejectedTransitions.fetchAndAddRelaxed(1);
            result.rejectionReason = QStringLiteral("metadata index reservation or physical ownership conflict");
            result.effects.clear();
            return result;
        }
        QSet<KisPageVersion> retained;
        for (const auto &version : std::as_const(next.versions)) {
            retained.insert(version.version);
        }
        quint64 removed = 0;
        for (const auto &version : std::as_const(input.versions)) {
            if (!retained.contains(version.version)) {
                shard->records.remove(&page.value(), version.version);
                ++removed;
            }
        }
        const bool headerInstalled = shard->installHeader(key, &page.value(), next);
        Q_ASSERT(headerInstalled);
        Q_UNUSED(headerInstalled);
        ++page->revision;
        shard->localVersionInstalls += quint64(next.versions.size());
        shard->localVersionRemovals += removed;
        if (background) {
            shard->backgroundLocalVersionInstalls += quint64(next.versions.size());
            shard->backgroundLocalVersionRemovals += removed;
        }
        d->acceptedTransitions.fetchAndAddRelaxed(quint64(transitions.size()));
        result.accepted = true;
        auto releasedBlocks = shard->arenas.takeEmptyBlocks(&shard->budgetCharge);
        lock.unlock();
        if (retirementDebtPrepared) {
            d->commitRetirementDebt(d->retirementDebtContext,
                                    retirementDebtCookie);
            retirementDebtPrepared = false;
        }
        Q_UNUSED(releasedBlocks);
        return result;
    }
}

QVector<KisPageKey> KisPageMetadataCoordinator::pageKeys() const
{
    QVector<MetadataShard *> shards;
    {
        QMutexLocker configurationLock(&d->configurationMutex);
        if (!d->operational) return {};
        for (const auto &shard : d->shards) shards.append(shard.get());
    }
    QVector<KisPageKey> result;
    for (MetadataShard *shard : std::as_const(shards)) {
        QMutexLocker shardLock(&shard->mutex);
        result.reserve(result.size() + shard->pages.size());
        for (auto page = shard->pages.cbegin(); page != shard->pages.cend(); ++page)
            result.append(page.key());
    }
    return result;
}

QVector<KisReplicaHandle> KisPageMetadataCoordinator::shutdownReplicaHandles() const
{
    QVector<MetadataShard *> shards;
    {
        QMutexLocker lock(&d->configurationMutex);
        if (!d->operational) return {};
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

KisPageTransitionResult KisPageMetadataCoordinator::acknowledgeLastUse(const KisPageVersion &version,
                                                                       const KisReplicaHandle &replica,
                                                                       const KisVerifiedCompletion &completion)
{
    KisPageTransitionResult result;
    if (!version.isValid() || !replica.isValid() || !(replica.version == version) || !completion.isValid()) {
        result.rejectionReason = QStringLiteral("verified last-use completion is invalid");
        return result;
    }
    KisPageTransition transition;
    transition.kind = KisPageTransitionKind::AcknowledgeLastUse;
    transition.version = version;
    transition.target = replica;
    transition.completion = completion.ticket();
    // Verification stays at this entry point. Only the exact version is
    // projected and installed; unrelated historical readers are untouched.
    return applyProjectedSequence(version.key, {transition});
}

qsizetype KisPageMetadataCoordinator::pageCount() const
{
    qsizetype count = 0;
    QMutexLocker configurationLocker(&d->configurationMutex);
    for (const std::shared_ptr<MetadataShard> &shard : d->shards) {
        QMutexLocker shardLocker(&shard->mutex);
        count += shard->pages.size();
    }
    return count;
}

KisPageMetadataMetrics KisPageMetadataCoordinator::metrics() const
{
    KisPageMetadataMetrics metrics;
    metrics.acceptedTransitions = d->acceptedTransitions.loadRelaxed();
    metrics.rejectedTransitions = d->rejectedTransitions.loadRelaxed();
    metrics.transitionDecisionNanoseconds = d->transitionDecisionNanoseconds.loadRelaxed();
    metrics.maximumTransitionDecisionNanoseconds =
        d->maximumTransitionDecisionNanoseconds.load(std::memory_order_relaxed);
    metrics.preparedPublicationPages = d->preparedPublicationPages.loadRelaxed();
    metrics.installedPublicationPages = d->installedPublicationPages.loadRelaxed();
    metrics.rejectedPublicationInstalls = d->rejectedPublicationInstalls.loadRelaxed();
    metrics.preparedMutationPages = d->preparedMutationPages.loadRelaxed();
    metrics.installedMutationPages = d->installedMutationPages.loadRelaxed();
    metrics.rejectedMutationInstalls = d->rejectedMutationInstalls.loadRelaxed();
    metrics.publicationHistoryNodesPrepared = d->publicationHistoryNodesPrepared.loadRelaxed();
    metrics.publicationAdditionRecordsPrepared = d->publicationAdditionRecordsPrepared.loadRelaxed();
    if (d->operational.load(std::memory_order_acquire))
        for (const auto &shard : d->shards) {
            QMutexLocker lock(&shard->mutex);
            metrics.localTransitionSequences += shard->localTransitionSequences;
            metrics.localVersionInputs += shard->localVersionInputs;
            metrics.localVersionInstalls += shard->localVersionInstalls;
            metrics.localVersionRemovals += shard->localVersionRemovals;
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
    QMutexLocker configurationLocker(&d->configurationMutex);
    for (const std::shared_ptr<MetadataShard> &shard : d->shards) {
        QMutexLocker shardLocker(&shard->mutex);
        result.versionArena += shard->arenas.versions.statistics();
        result.replicaArena += shard->arenas.replicas.statistics();
        result.overflowArena += shard->arenas.overflow.statistics();
        result.exactVersionIndex += shard->records.exactVersions.statistics();
        result.physicalSlotIndex += shard->records.physicalSlots.statistics();
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

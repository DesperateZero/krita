/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_METADATA_ARENA_P_H
#define KIS_PAGE_METADATA_ARENA_P_H

#include <QHashFunctions>
#include <QtGlobal>

#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "KisPageMetadataCoordinator.h"
#include "KisPageMetadataReservation_p.h"
#include "KisMutationStorage_p.h"

template<class Tag>
struct KisGenerationalSlotId {
    quint32 index = 0;
    quint32 generation = 0;

    bool isValid() const
    {
        return index != 0 && generation != 0;
    }
};

template<class Tag>
inline bool operator==(KisGenerationalSlotId<Tag> lhs, KisGenerationalSlotId<Tag> rhs)
{
    return lhs.index == rhs.index && lhs.generation == rhs.generation;
}

template<class Tag>
inline size_t qHash(KisGenerationalSlotId<Tag> slot, size_t seed = 0) noexcept
{
    return qHash((quint64(slot.generation) << 32) | slot.index, seed);
}

struct KisVersionSlotTag;
struct KisReplicaSlotTag;
struct KisMetadataOverflowSlotTag;

using KisVersionSlotId = KisGenerationalSlotId<KisVersionSlotTag>;
using KisReplicaSlotId = KisGenerationalSlotId<KisReplicaSlotTag>;
using KisMetadataOverflowSlotId = KisGenerationalSlotId<KisMetadataOverflowSlotTag>;

static_assert(sizeof(KisVersionSlotId) == 8);
static_assert(sizeof(KisReplicaSlotId) == 8);
static_assert(sizeof(KisMetadataOverflowSlotId) == 8);
static_assert(std::is_trivially_copyable_v<KisVersionSlotId>);
static_assert(std::is_trivially_copyable_v<KisReplicaSlotId>);
static_assert(std::is_trivially_copyable_v<KisMetadataOverflowSlotId>);

/**
 * Maps an arena value to its public typed slot id. Private record headers may
 * specialize this trait before instantiating KisShardSlotArena. Keeping the
 * tag out of the arena template preserves the two-argument form frozen by the
 * BR1 design while tiny tests can use the value type itself as the tag.
 */
template<class T>
struct KisSlotIdFor {
    using Type = KisGenerationalSlotId<T>;
};

/**
 * A shard-local, externally synchronized stable-slot arena.
 *
 * The arena never acquires a mutex. Its owner must serialize attach/emplace/
 * erase/release and must keep the same lock held while dereferencing get().
 * Block payload allocation is performed by prepareBlock(), which is designed
 * to run before taking that owner lock. Empty payloads are detached into a
 * ReleasedBlocks value and can therefore be destroyed after releasing it.
 */
template<class T, qsizetype BlockBytes>
class KisShardSlotArena
{
    static_assert(BlockBytes > 0);
    static_assert(BlockBytes > qsizetype(sizeof(void *)));
    static_assert(std::is_nothrow_destructible_v<T>);

    static constexpr quint32 InvalidOffset = std::numeric_limits<quint32>::max();
    static constexpr quint16 MaximumGeneration = std::numeric_limits<quint16>::max();

    struct Slot {
        enum class State : quint8 { Free, Occupied, Quarantined } state = State::Free;
        alignas(T) unsigned char storage[sizeof(T)];
        quint32 nextFree = InvalidOffset;
        quint16 generation = 1;

        T *value()
        {
            return std::launder(reinterpret_cast<T *>(storage));
        }

        const T *value() const
        {
            return std::launder(reinterpret_cast<const T *>(storage));
        }
    };

public:
    using SlotId = typename KisSlotIdFor<T>::Type;

private:
    static constexpr quint32 SlotsPerBlock =
        quint32((BlockBytes - qsizetype(sizeof(void *))) / qsizetype(sizeof(Slot)));
    static_assert(SlotsPerBlock > 0, "the arena block must contain at least one slot");

    struct Block {
        Slot slots[SlotsPerBlock];
        std::unique_ptr<Block> nextReleased;

        Block()
        {
            for (quint32 i = 0; i < SlotsPerBlock; ++i) {
                slots[i].nextFree = i + 1 < SlotsPerBlock ? i + 1 : InvalidOffset;
            }
        }
    };

    static_assert(sizeof(Block) <= size_t(BlockBytes));

    enum class DirectoryState : quint8 {
        Active,
        Released,
        Quarantined
    };

    struct DirectoryEntry {
        std::unique_ptr<Block> block;
        quint32 freeHead = InvalidOffset;
        quint32 usedSlots = 0;
        quint32 freeSlots = 0;
        quint32 quarantinedSlots = 0;
        quint16 blockGeneration = 1;
        DirectoryState state = DirectoryState::Released;
    };
    using Directory = std::vector<DirectoryEntry, KisMutationStorageAllocator<DirectoryEntry>>;
    static_assert(std::is_nothrow_move_constructible_v<DirectoryEntry>);

public:
    using Statistics = KisPageMetadataArenaStatistics;

    using Reservation = KisPageMetadataReservation<KisShardSlotArena>;

    class PreparedDirectory
    {
        friend class KisShardSlotArena;
    public:
        explicit PreparedDirectory(KisMutationStorageAllocator<char> storage = KisMutationStorageAllocator<char>())
            : m_entries(storage) {}
        PreparedDirectory(PreparedDirectory &&) noexcept = default;
        PreparedDirectory &operator=(PreparedDirectory &&) noexcept = default;
        PreparedDirectory(const PreparedDirectory &) = delete;
        PreparedDirectory &operator=(const PreparedDirectory &) = delete;
        void prepare(quint64 capacity)
        {
            // Reuse/free the previous empty directory only outside the owner
            // gate. Entries in an installed replacement were moved, not copied.
            if (!capacity || capacity > m_entries.capacity())
                m_entries = Directory(m_entries.get_allocator());
            else m_entries.clear();
            m_entries.reserve(size_t(capacity));
        }
    private:
        Directory m_entries;
    };

    class PreparedBlock
    {
        friend class KisShardSlotArena;

    public:
        PreparedBlock() = default;
        PreparedBlock(PreparedBlock &&) noexcept = default;
        PreparedBlock &operator=(PreparedBlock &&) noexcept = default;
        PreparedBlock(const PreparedBlock &) = delete;
        PreparedBlock &operator=(const PreparedBlock &) = delete;

        bool isValid() const
        {
            return bool(m_block);
        }
    private:
        explicit PreparedBlock(std::unique_ptr<Block> block)
            : m_block(std::move(block))
        {
        }

        std::unique_ptr<Block> m_block;
    };

    class ReleasedBlocks
    {
        friend class KisShardSlotArena;

    public:
        ReleasedBlocks() = default;
        ReleasedBlocks(ReleasedBlocks &&other) noexcept
            : m_blocks(std::move(other.m_blocks))
            , m_tail(std::exchange(other.m_tail, nullptr))
            , m_blockCount(std::exchange(other.m_blockCount, 0))
        {
        }
        ReleasedBlocks &operator=(ReleasedBlocks &&other) noexcept
        {
            if (this != &other) {
                clear();
                m_blocks = std::move(other.m_blocks);
                m_tail = std::exchange(other.m_tail, nullptr);
                m_blockCount = std::exchange(other.m_blockCount, 0);
            }
            return *this;
        }
        ReleasedBlocks(const ReleasedBlocks &) = delete;
        ReleasedBlocks &operator=(const ReleasedBlocks &) = delete;
        ~ReleasedBlocks() { clear(); }

        bool isEmpty() const
        {
            return !m_blocks;
        }
        qsizetype blockCount() const
        {
            return m_blockCount;
        }
        quint64 byteSize() const
        {
            return quint64(m_blockCount) * quint64(BlockBytes);
        }

        void append(ReleasedBlocks &&other) noexcept
        {
            if (this == &other || other.isEmpty()) return;
            if (isEmpty()) {
                *this = std::move(other);
                return;
            }
            other.m_tail->nextReleased = std::move(m_blocks);
            m_blocks = std::move(other.m_blocks);
            m_blockCount += std::exchange(other.m_blockCount, 0);
            other.m_tail = nullptr;
        }

    private:
        void clear() noexcept
        {
            // A long acknowledgement can empty many blocks. Do not destroy
            // their intrusive ownership chain recursively on the caller stack.
            while (m_blocks) {
                auto next = std::move(m_blocks->nextReleased);
                m_blocks = std::move(next);
            }
            m_blockCount = 0;
            m_tail = nullptr;
        }

        std::unique_ptr<Block> m_blocks;
        Block *m_tail = nullptr;
        qsizetype m_blockCount = 0;
    };

    explicit KisShardSlotArena(quint64 maximumBytes = std::numeric_limits<quint64>::max(),
                              KisMutationStorageAllocator<char> storage = KisMutationStorageAllocator<char>())
        : m_maximumBytes(maximumBytes)
        , m_directory(storage)
    {}

    ~KisShardSlotArena()
    {
        destroyActiveValues();
    }

    KisShardSlotArena(const KisShardSlotArena &) = delete;
    KisShardSlotArena &operator=(const KisShardSlotArena &) = delete;
    KisShardSlotArena(KisShardSlotArena &&) = delete;
    KisShardSlotArena &operator=(KisShardSlotArena &&) = delete;

    static constexpr quint32 slotsPerBlock()
    {
        return SlotsPerBlock;
    }

    static constexpr quint64 blockByteSize()
    {
        return quint64(BlockBytes);
    }

    bool canAttachBlocks(quint64 count) const
    {
        if (m_closed || count == 0)
            return !m_closed;
        if (count > m_maximumBytes / quint64(BlockBytes))
            return false;
        if (newDirectoryEntries(count) > std::numeric_limits<quint32>::max() / SlotsPerBlock - m_directory.size())
            return false;
        const quint64 bytes = count * quint64(BlockBytes);
        return m_statistics.allocatedBytes <= m_maximumBytes - bytes;
    }

    static PreparedBlock prepareBlock() noexcept
    {
        return PreparedBlock(std::unique_ptr<Block>(new (std::nothrow) Block()));
    }

    quint64 directoryCapacityForBlocks(quint64 count) const
    {
        count = newDirectoryEntries(count);
        if (count <= m_directory.capacity() - m_directory.size()) return 0;
        const quint64 maximum = std::numeric_limits<quint32>::max() / SlotsPerBlock;
        const quint64 required = std::min(maximum, quint64(m_directory.size()) + count);
        return std::max(required, std::min(maximum, quint64(m_directory.capacity()) * 2));
    }

    bool installPreparedDirectory(PreparedDirectory *prepared, quint64 count) noexcept
    {
        Q_ASSERT(!prepared || prepared->m_entries.get_allocator() == m_directory.get_allocator());
        count = newDirectoryEntries(count);
        if (count <= m_directory.capacity() - m_directory.size()) return true;
        if (!prepared || !prepared->m_entries.empty()
            || prepared->m_entries.capacity() < m_directory.size()
            || count > prepared->m_entries.capacity() - m_directory.size()) return false;
        for (auto &entry : m_directory)
            prepared->m_entries.emplace_back(std::move(entry));
        m_directory.swap(prepared->m_entries);
        return true;
    }

    Reservation reserveSlots(qsizetype count)
    {
        if (count <= 0 || quint64(count) > availableSlots()) {
            if (count > 0)
                ++m_statistics.rejectedReservations;
            return {};
        }
        m_outstandingReservations += quint64(count);
        m_statistics.outstandingReservations = m_outstandingReservations;
        ++m_statistics.reservationBatches;
        return Reservation(this, count);
    }

    void cancelReservation(Reservation *reservation)
    {
        if (!reservation || reservation->m_owner != this)
            return;
        Q_ASSERT(m_outstandingReservations >= quint64(reservation->m_remaining));
        m_outstandingReservations -= quint64(reservation->m_remaining);
        m_statistics.outstandingReservations = m_outstandingReservations;
        reservation->m_remaining = 0;
        reservation->m_owner = nullptr;
    }

    quint64 availableSlots() const
    {
        Q_ASSERT(m_statistics.freeSlots >= m_outstandingReservations);
        return m_statistics.freeSlots - m_outstandingReservations;
    }

    bool attachPreparedBlock(PreparedBlock *prepared)
    {
        if (!prepared || !prepared->m_block || m_closed || quint64(BlockBytes) > m_maximumBytes
            || m_statistics.allocatedBytes > m_maximumBytes - quint64(BlockBytes)) {
            ++m_statistics.rejectedBlockAttaches;
            return false;
        }

        qsizetype directoryIndex = -1;
        for (qsizetype i = 0; i < qsizetype(m_directory.size()); ++i) {
            DirectoryEntry &entry = m_directory[size_t(i)];
            if (entry.state != DirectoryState::Released)
                continue;
            if (entry.blockGeneration + 1 >= MaximumGeneration) {
                entry.state = DirectoryState::Quarantined;
                ++m_statistics.quarantinedBlocks;
                continue;
            }
            ++entry.blockGeneration;
            directoryIndex = i;
            break;
        }

        if (directoryIndex < 0) {
            const quint64 nextIndex = quint64(m_directory.size());
            const quint64 lastFlatIndex = (nextIndex + 1) * quint64(SlotsPerBlock);
            if (lastFlatIndex > std::numeric_limits<quint32>::max()
                || m_directory.size() == m_directory.capacity()) {
                ++m_statistics.rejectedBlockAttaches;
                return false;
            }
            m_directory.emplace_back();
            directoryIndex = qsizetype(m_directory.size() - 1);
        }

        DirectoryEntry &entry = m_directory[size_t(directoryIndex)];
        entry.block = std::move(prepared->m_block);
        entry.freeHead = 0;
        entry.usedSlots = 0;
        entry.freeSlots = SlotsPerBlock;
        entry.quarantinedSlots = 0;
        entry.state = DirectoryState::Active;
        ++m_statistics.activeBlocks;
        ++m_statistics.attachedBlocks;
        m_statistics.allocatedBytes += quint64(BlockBytes);
        m_statistics.freeSlots += SlotsPerBlock;
        m_statistics.directoryEntries = quint64(m_directory.size());
        m_freeBlockHint = quint32(directoryIndex);
        return true;
    }

    template<class... Args>
    SlotId emplace(Args &&...args)
    {
        if (m_closed || availableSlots() == 0 || m_directory.empty()) {
            return {};
        }

        const quint32 directorySize = quint32(m_directory.size());
        for (quint32 step = 0; step < directorySize; ++step) {
            const quint32 directoryIndex = (m_freeBlockHint + step) % directorySize;
            DirectoryEntry &entry = m_directory[directoryIndex];
            if (entry.state != DirectoryState::Active || entry.freeHead == InvalidOffset) {
                continue;
            }

            const quint32 offset = entry.freeHead;
            Slot &slot = entry.block->slots[offset];
            Q_ASSERT(slot.state == Slot::State::Free);
            new (slot.storage) T(std::forward<Args>(args)...);
            entry.freeHead = slot.nextFree;
            slot.nextFree = InvalidOffset;
            slot.state = Slot::State::Occupied;
            ++entry.usedSlots;
            --entry.freeSlots;
            ++m_statistics.usedSlots;
            --m_statistics.freeSlots;
            m_statistics.highWaterSlots = std::max(m_statistics.highWaterSlots, m_statistics.usedSlots);
            m_freeBlockHint = directoryIndex;
            return makeId(directoryIndex, offset, entry, slot);
        }
        return {};
    }

    template<class... Args>
    SlotId emplaceReserved(Reservation *reservation, Args &&...args)
    {
        if (!reservation || reservation->m_owner != this || reservation->m_remaining <= 0
            || m_outstandingReservations == 0) {
            return {};
        }
        --reservation->m_remaining;
        --m_outstandingReservations;
        m_statistics.outstandingReservations = m_outstandingReservations;
        if (reservation->m_remaining == 0)
            reservation->m_owner = nullptr;
        const SlotId result = emplace(std::forward<Args>(args)...);
        Q_ASSERT(result.isValid());
        return result;
    }

    T *get(SlotId id)
    {
        const ResolvedSlot resolved = resolve(id);
        return resolved.slot ? resolved.slot->value() : nullptr;
    }

    const T *get(SlotId id) const
    {
        const ConstResolvedSlot resolved = resolve(id);
        return resolved.slot ? resolved.slot->value() : nullptr;
    }

    bool erase(SlotId id, ReleasedBlocks *released = nullptr,
               quint64 minimumActiveBlocksToKeep = 0)
    {
        ResolvedSlot resolved = resolve(id);
        if (!resolved.slot)
            return false;

        Slot &slot = *resolved.slot;
        DirectoryEntry &entry = *resolved.entry;
        slot.value()->~T();
        slot.state = Slot::State::Free;
        --entry.usedSlots;
        --m_statistics.usedSlots;

        if (slot.generation + 1 >= MaximumGeneration) {
            slot.generation = MaximumGeneration;
            slot.state = Slot::State::Quarantined;
            ++entry.quarantinedSlots;
            ++m_statistics.quarantinedSlots;
        } else {
            ++slot.generation;
            slot.nextFree = entry.freeHead;
            entry.freeHead = resolved.offset;
            ++entry.freeSlots;
            ++m_statistics.freeSlots;
            m_freeBlockHint = resolved.directoryIndex;
        }
        // Optional local reclamation: examine only the block containing this
        // slot. Existing reservations keep their admitted capacity; the normal
        // reservation terminal path reclaims it after the final reservation.
        if (released && entry.usedSlots == 0 && m_outstandingReservations == 0
            && m_statistics.activeBlocks > minimumActiveBlocksToKeep) {
            detachEntry(entry, released);
        }
        return true;
    }

    ReleasedBlocks takeEmptyBlocks(quint64 minimumActiveBlocksToKeep = 0)
    {
        ReleasedBlocks released;
        if (m_outstandingReservations != 0 || m_statistics.activeBlocks <= minimumActiveBlocksToKeep) {
            return released;
        }
        for (DirectoryEntry &entry : m_directory) {
            if (m_statistics.activeBlocks <= minimumActiveBlocksToKeep)
                break;
            if (entry.state != DirectoryState::Active || entry.usedSlots != 0) {
                continue;
            }
            detachEntry(entry, &released);
        }
        return released;
    }

    void close()
    {
        m_closed = true;
    }

    bool isClosed() const
    {
        return m_closed;
    }

    ReleasedBlocks drain()
    {
        m_closed = true;
        ReleasedBlocks released;
        for (DirectoryEntry &entry : m_directory) {
            if (entry.state != DirectoryState::Active)
                continue;
            for (Slot &slot : entry.block->slots) {
                if (slot.state != Slot::State::Occupied)
                    continue;
                slot.value()->~T();
                slot.state = Slot::State::Free;
                --m_statistics.usedSlots;
            }
            entry.usedSlots = 0;
            detachEntry(entry, &released);
        }
        return released;
    }

    Statistics statistics() const
    {
        return m_statistics;
    }

private:
    quint64 newDirectoryEntries(quint64 count) const
    {
        for (const auto &entry : m_directory) {
            if (!count) break;
            if (entry.state == DirectoryState::Released
                && entry.blockGeneration + 1 < MaximumGeneration) --count;
        }
        return count;
    }

    struct ResolvedSlot {
        DirectoryEntry *entry = nullptr;
        Slot *slot = nullptr;
        quint32 directoryIndex = 0;
        quint32 offset = 0;
    };

    struct ConstResolvedSlot {
        const DirectoryEntry *entry = nullptr;
        const Slot *slot = nullptr;
        quint32 directoryIndex = 0;
        quint32 offset = 0;
    };

    static quint32 packedGeneration(quint16 blockGeneration, quint16 slotGeneration)
    {
        return (quint32(blockGeneration) << 16) | slotGeneration;
    }

    SlotId makeId(quint32 directoryIndex, quint32 offset, const DirectoryEntry &entry, const Slot &slot) const
    {
        const quint64 flatIndex = quint64(directoryIndex) * SlotsPerBlock + offset + 1;
        Q_ASSERT(flatIndex <= std::numeric_limits<quint32>::max());
        return {quint32(flatIndex), packedGeneration(entry.blockGeneration, slot.generation)};
    }

    ResolvedSlot resolve(SlotId id)
    {
        const ConstResolvedSlot resolved = std::as_const(*this).resolve(id);
        return {const_cast<DirectoryEntry *>(resolved.entry),
                const_cast<Slot *>(resolved.slot),
                resolved.directoryIndex,
                resolved.offset};
    }

    ConstResolvedSlot resolve(SlotId id) const
    {
        if (!id.isValid())
            return {};
        const quint32 flat = id.index - 1;
        const quint32 directoryIndex = flat / SlotsPerBlock;
        const quint32 offset = flat % SlotsPerBlock;
        if (directoryIndex >= m_directory.size())
            return {};
        const DirectoryEntry &entry = m_directory[directoryIndex];
        if (entry.state != DirectoryState::Active || !entry.block)
            return {};
        const Slot &slot = entry.block->slots[offset];
        if (slot.state != Slot::State::Occupied
            || id.generation != packedGeneration(entry.blockGeneration, slot.generation)) {
            return {};
        }
        return {&entry, &slot, directoryIndex, offset};
    }

    void detachEntry(DirectoryEntry &entry, ReleasedBlocks *released)
    {
        Q_ASSERT(released && entry.state == DirectoryState::Active && entry.usedSlots == 0);
        if (released->isEmpty()) released->m_tail = entry.block.get();
        entry.block->nextReleased = std::move(released->m_blocks);
        released->m_blocks = std::move(entry.block);
        ++released->m_blockCount;
        --m_statistics.activeBlocks;
        m_statistics.allocatedBytes -= quint64(BlockBytes);
        m_statistics.freeSlots -= entry.freeSlots;
        m_statistics.quarantinedSlots -= entry.quarantinedSlots;
        m_statistics.releasedBytes += quint64(BlockBytes);
        entry.freeHead = InvalidOffset;
        entry.freeSlots = 0;
        entry.quarantinedSlots = 0;
        entry.state = DirectoryState::Released;
    }

    void destroyActiveValues() noexcept
    {
        for (DirectoryEntry &entry : m_directory) {
            if (entry.state != DirectoryState::Active || !entry.block)
                continue;
            for (Slot &slot : entry.block->slots) {
                if (slot.state == Slot::State::Occupied)
                    slot.value()->~T();
            }
        }
    }

    const quint64 m_maximumBytes;
    Directory m_directory;
    Statistics m_statistics;
    quint64 m_outstandingReservations = 0;
    quint32 m_freeBlockHint = 0;
    bool m_closed = false;
};

#endif // KIS_PAGE_METADATA_ARENA_P_H

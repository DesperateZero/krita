/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_METADATA_INDEX_P_H
#define KIS_PAGE_METADATA_INDEX_P_H

#include <QHashFunctions>

#include <algorithm>
#include <limits>
#include <new>
#include <memory>
#include <type_traits>

#include "KisPageMetadataCoordinator.h"
#include "KisPageMetadataReservation_p.h"

/**
 * Shard-local flat slot index. The owner supplies synchronization.
 *
 * prepareCapacity() constructs every bucket before any authoritative edit.
 * Tokens cover concurrent candidates at a maximum 1/2 load. Installation and
 * backward-shift erasure only copy trivial identities into existing buckets;
 * neither allocates, rehashes nor leaves tombstones for later insertions.
 */
template<class Key, class SlotId>
class KisShardSlotIndex
{
    struct Entry {
        Key key{};
        SlotId slot{}; // invalid denotes an empty bucket
    };
    static_assert(std::is_trivially_copyable_v<Entry>);
    static_assert(std::is_nothrow_default_constructible_v<Entry>);

public:
    using Reservation = KisPageMetadataReservation<KisShardSlotIndex>;
    using Statistics = KisPageMetadataIndexStatistics;

    explicit KisShardSlotIndex(KisMutationStorageAllocator<char> storage = KisMutationStorageAllocator<char>{}) : m_entries(storage) {}
    KisShardSlotIndex(const KisShardSlotIndex &) = delete;
    KisShardSlotIndex &operator=(const KisShardSlotIndex &) = delete;

    qsizetype requiredCapacity(qsizetype count) const
    {
        const qsizetype maximum = std::numeric_limits<qsizetype>::max();
        if (count <= 0 || m_outstandingReservations > maximum - m_size
            || count > maximum - m_size - m_outstandingReservations) {
            return 0;
        }
        return m_size + m_outstandingReservations + count;
    }

    bool prepareCapacity(qsizetype required)
    {
        if (required <= 0)
            return false;
        if (required <= m_capacity) return true;
        qsizetype capacity = 64;
        const qsizetype maximum = std::numeric_limits<qsizetype>::max() / 2 / sizeof(Entry);
        while (capacity <= required) {
            if (capacity > maximum / 2) {
                ++m_rejectedReservations;
                return false;
            }
            capacity *= 2;
        }
        decltype(m_entries) entries(m_entries.get_allocator());
        try {
            entries.resize(size_t(2 * capacity));
        } catch (const std::bad_alloc &) {
            ++m_rejectedReservations;
            return false;
        }
        const size_t mask = size_t(2 * capacity - 1);
        for (qsizetype i = 0; i < 2 * m_capacity; ++i) {
            if (!m_entries[i].slot.isValid()) continue;
            size_t bucket = hash(m_entries[i].key) & mask;
            while (entries[bucket].slot.isValid()) bucket = (bucket + 1) & mask;
            entries[bucket] = m_entries[i];
        }
        m_entries.swap(entries);
        m_capacity = capacity;
        return true;
    }

    Reservation reserveInsertions(qsizetype count)
    {
        const qsizetype required = requiredCapacity(count);
        if (required <= 0) {
            if (count > 0)
                ++m_rejectedReservations;
            return {};
        }
        // Owning capacity is prepared and budgeted by MetadataShard before a
        // reservation token can exist. Installation only consumes constructed buckets.
        if (m_capacity < required) {
            ++m_rejectedReservations;
            return {};
        }
        m_outstandingReservations += count;
        ++m_reservationBatches;
        return Reservation(this, count);
    }

    bool insertReserved(Reservation *reservation, const Key &key, SlotId slot)
    {
        if (!reservation || reservation->m_owner != this || reservation->m_remaining <= 0 || !slot.isValid())
            return false;
        const size_t bucket = findBucket(key);
        if (m_entries[bucket].slot.isValid()) return false;
        m_entries[bucket] = Entry{key, slot};
        ++m_size;
        --reservation->m_remaining;
        --m_outstandingReservations;
        if (reservation->m_remaining == 0) {
            reservation->m_owner = nullptr;
        }
        m_highWaterEntries = std::max(m_highWaterEntries, quint64(m_size));
        return true;
    }

    void cancelReservation(Reservation *reservation)
    {
        if (!reservation || reservation->m_owner != this)
            return;
        Q_ASSERT(m_outstandingReservations >= reservation->m_remaining);
        m_outstandingReservations -= reservation->m_remaining;
        reservation->m_remaining = 0;
        reservation->m_owner = nullptr;
    }

    bool findExact(const Key &key, SlotId *slot = nullptr) const
    {
        if (!m_size) return false;
        const Entry &entry = m_entries[findBucket(key)];
        if (!entry.slot.isValid()) return false;
        if (slot) *slot = entry.slot;
        return true;
    }

    bool eraseExact(const Key &key, SlotId expected = {})
    {
        if (!m_size) return false;
        size_t hole = findBucket(key);
        if (!m_entries[hole].slot.isValid()
            || (expected.isValid() && !(m_entries[hole].slot == expected))) return false;
        const size_t mask = size_t(2 * m_capacity - 1);
        // Move a displaced entry back only if its probe crosses the hole.
        // Modular distances handle clusters wrapping around the array end.
        for (size_t next = (hole + 1) & mask; m_entries[next].slot.isValid(); next = (next + 1) & mask) {
            const size_t home = hash(m_entries[next].key) & mask;
            if (((next - home) & mask) >= ((hole - home) & mask)) {
                m_entries[hole] = m_entries[next];
                hole = next;
            }
        }
        m_entries[hole] = {};
        --m_size;
        return true;
    }

    qsizetype size() const
    {
        return m_size;
    }

    quint64 allocatedBytes() const noexcept { return kisPageStorageBytes(m_entries.data(), alignof(Entry)); }

    Statistics statistics() const
    {
        return {quint64(m_size),
                quint64(m_capacity),
                quint64(m_outstandingReservations),
                m_highWaterEntries,
                m_reservationBatches,
                m_rejectedReservations};
    }

private:
    size_t hash(const Key &key) const
    {
        using ::qHash;
        return qHash(key, m_seed);
    }

    size_t findBucket(const Key &key) const
    {
        const size_t mask = size_t(2 * m_capacity - 1);
        size_t bucket = hash(key) & mask;
        while (m_entries[bucket].slot.isValid() && !(m_entries[bucket].key == key))
            bucket = (bucket + 1) & mask;
        return bucket;
    }

    std::vector<Entry, KisMutationStorageAllocator<Entry>> m_entries;
    const size_t m_seed = QHashSeed::globalSeed();
    qsizetype m_capacity = 0;
    qsizetype m_size = 0;
    qsizetype m_outstandingReservations = 0;
    quint64 m_highWaterEntries = 0;
    quint64 m_reservationBatches = 0;
    quint64 m_rejectedReservations = 0;
};

#endif // KIS_PAGE_METADATA_INDEX_P_H

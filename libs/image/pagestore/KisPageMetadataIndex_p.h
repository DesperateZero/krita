/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_METADATA_INDEX_P_H
#define KIS_PAGE_METADATA_INDEX_P_H

#include <QHash>

#include <algorithm>
#include <limits>
#include <new>

#include "KisPageMetadataCoordinator.h"
#include "KisPageMetadataReservation_p.h"

/**
 * Shard-local slot index with an explicit insertion-capacity capability.
 *
 * The owner supplies synchronization. reserveInsertions() grows the wrapped
 * QHash before authoritative records are edited and accounts capacity against
 * all outstanding tokens. insertReserved() is the only insertion API; this
 * prevents an install path from silently bypassing the reservation protocol.
 */
template<class Key, class SlotId>
class KisShardSlotIndex
{
public:
    using Reservation = KisPageMetadataReservation<KisShardSlotIndex>;

    using Statistics = KisPageMetadataIndexStatistics;

    KisShardSlotIndex() = default;
    KisShardSlotIndex(const KisShardSlotIndex &) = delete;
    KisShardSlotIndex &operator=(const KisShardSlotIndex &) = delete;

    qsizetype requiredCapacity(qsizetype count) const
    {
        const qsizetype maximum = std::numeric_limits<qsizetype>::max();
        if (count <= 0 || m_outstandingReservations > maximum - m_entries.size()
            || count > maximum - m_entries.size() - m_outstandingReservations) {
            return 0;
        }
        return m_entries.size() + m_outstandingReservations + count;
    }

    bool prepareCapacity(qsizetype required)
    {
        if (required <= 0)
            return false;
        try {
            m_entries.reserve(required);
        } catch (const std::bad_alloc &) {
            ++m_rejectedReservations;
            return false;
        }
        return m_entries.capacity() >= required;
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
        // reservation token can exist. Insertion must not grow this QHash.
        if (m_entries.capacity() < required) {
            ++m_rejectedReservations;
            return {};
        }
        m_outstandingReservations += count;
        ++m_reservationBatches;
        return Reservation(this, count);
    }

    bool insertReserved(Reservation *reservation, const Key &key, SlotId slot)
    {
        if (!reservation || reservation->m_owner != this || reservation->m_remaining <= 0 || !slot.isValid()
            || m_entries.contains(key)) {
            return false;
        }
        m_entries.insert(key, slot);
        --reservation->m_remaining;
        --m_outstandingReservations;
        if (reservation->m_remaining == 0) {
            reservation->m_owner = nullptr;
        }
        m_highWaterEntries = std::max(m_highWaterEntries, quint64(m_entries.size()));
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
        const auto found = m_entries.constFind(key);
        if (found == m_entries.cend())
            return false;
        if (slot)
            *slot = found.value();
        return true;
    }

    bool eraseExact(const Key &key, SlotId expected = {})
    {
        const auto found = m_entries.find(key);
        if (found == m_entries.end() || (expected.isValid() && !(found.value() == expected))) {
            return false;
        }
        m_entries.erase(found);
        return true;
    }

    qsizetype size() const
    {
        return m_entries.size();
    }

    Statistics statistics() const
    {
        return {quint64(m_entries.size()),
                quint64(m_entries.capacity()),
                quint64(m_outstandingReservations),
                m_highWaterEntries,
                m_reservationBatches,
                m_rejectedReservations};
    }

private:
    QHash<Key, SlotId> m_entries;
    qsizetype m_outstandingReservations = 0;
    quint64 m_highWaterEntries = 0;
    quint64 m_reservationBatches = 0;
    quint64 m_rejectedReservations = 0;
};

#endif // KIS_PAGE_METADATA_INDEX_P_H

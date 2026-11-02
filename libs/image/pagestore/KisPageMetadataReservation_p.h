/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_METADATA_RESERVATION_P_H
#define KIS_PAGE_METADATA_RESERVATION_P_H

#include <QtGlobal>

#include <utility>

template<class Owner>
class KisPageMetadataReservation
{
    friend Owner;

public:
    KisPageMetadataReservation() = default;
    KisPageMetadataReservation(KisPageMetadataReservation &&other) noexcept
        : m_owner(std::exchange(other.m_owner, nullptr))
        , m_remaining(std::exchange(other.m_remaining, 0))
    {
    }
    KisPageMetadataReservation &operator=(KisPageMetadataReservation &&other) noexcept
    {
        if (this != &other) {
            Q_ASSERT(!m_owner && m_remaining == 0);
            m_owner = std::exchange(other.m_owner, nullptr);
            m_remaining = std::exchange(other.m_remaining, 0);
        }
        return *this;
    }
    KisPageMetadataReservation(const KisPageMetadataReservation &) = delete;
    KisPageMetadataReservation &operator=(const KisPageMetadataReservation &) = delete;

    // Join unused rights of the same original owner; no new capacity or
    // reservation is issued and the owner's outstanding total is unchanged.
    void absorb(KisPageMetadataReservation &&other) noexcept
    {
        if (!other.isValid()) return;
        Q_ASSERT(!m_owner || m_owner == other.m_owner);
        m_owner = std::exchange(other.m_owner, nullptr);
        m_remaining += std::exchange(other.m_remaining, 0);
    }

    bool isValid() const { return m_owner && m_remaining > 0; }
    qsizetype remaining() const { return m_remaining; }
    // Partition one funded reservation without changing the owner's total.
    // Each returned part has exactly one ordinary consume/cancel endpoint.
    KisPageMetadataReservation take(qsizetype count) noexcept
    {
        if (!m_owner || count <= 0 || count > m_remaining) return {};
        auto *owner = m_owner;
        m_remaining -= count;
        if (!m_remaining) m_owner = nullptr;
        return KisPageMetadataReservation(owner, count);
    }


private:
    explicit KisPageMetadataReservation(Owner *owner, qsizetype remaining)
        : m_owner(owner), m_remaining(remaining) {}

    Owner *m_owner = nullptr;
    qsizetype m_remaining = 0;
};

#endif // KIS_PAGE_METADATA_RESERVATION_P_H

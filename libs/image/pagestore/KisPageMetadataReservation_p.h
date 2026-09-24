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

    bool isValid() const { return m_owner && m_remaining > 0; }
    qsizetype remaining() const { return m_remaining; }

private:
    explicit KisPageMetadataReservation(Owner *owner, qsizetype remaining)
        : m_owner(owner), m_remaining(remaining) {}

    Owner *m_owner = nullptr;
    qsizetype m_remaining = 0;
};

#endif // KIS_PAGE_METADATA_RESERVATION_P_H

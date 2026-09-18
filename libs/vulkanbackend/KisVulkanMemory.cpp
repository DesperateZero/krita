/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanMemory.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>

class KisVulkanMemoryBudgetLedger::Private
{
public:
    mutable QMutex mutex;
    quint64 generation = 0;
    quint64 nextReservation = 1;
    KisVulkanMemoryBudget budget;
    QHash<quint64, KisVulkanMemoryReservation> reservations;
    quint64 reservedBytes = 0;
    quint64 evictableBytes = 0;
};

KisVulkanMemoryBudgetLedger::KisVulkanMemoryBudgetLedger()
    : d(new Private)
{
}

KisVulkanMemoryBudgetLedger::~KisVulkanMemoryBudgetLedger() = default;

bool KisVulkanMemoryBudgetLedger::configure(quint64 generation,
                                            const KisVulkanMemoryBudget &budget)
{
    if (generation == 0 || !budget.isValid()) {
        return false;
    }
    QMutexLocker locker(&d->mutex);
    if (d->generation != 0 || !d->reservations.isEmpty()) {
        return false;
    }
    d->generation = generation;
    d->budget = budget;
    return true;
}

KisVulkanMemoryReservation KisVulkanMemoryBudgetLedger::reserve(
    const KisVulkanMemoryReservationRequest &request,
    QString *error)
{
    QMutexLocker locker(&d->mutex);
    if (d->generation == 0 || !d->budget.isValid()) {
        if (error) *error = QStringLiteral("Memory budget ledger is not configured");
        return {};
    }
    if (!request.isValid()) {
        if (error) *error = QStringLiteral("Memory reservation request is invalid");
        return {};
    }
    if (request.bytes > d->budget.hardLimitBytes - d->reservedBytes) {
        if (error) *error = QStringLiteral("Memory reservation exceeds the hard budget");
        return {};
    }

    KisVulkanMemoryReservation reservation;
    reservation.reservationId = d->nextReservation++;
    reservation.ledgerGeneration = d->generation;
    reservation.memoryClass = request.memoryClass;
    reservation.bytes = request.bytes;
    reservation.ownerSession = request.ownerSession;
    reservation.evictable = request.evictable;
    d->reservations.insert(reservation.reservationId, reservation);
    d->reservedBytes += reservation.bytes;
    if (reservation.evictable) {
        d->evictableBytes += reservation.bytes;
    }
    return reservation;
}

bool KisVulkanMemoryBudgetLedger::release(const KisVulkanMemoryReservation &reservation)
{
    if (!reservation.isValid()) {
        return false;
    }
    QMutexLocker locker(&d->mutex);
    if (reservation.ledgerGeneration != d->generation) {
        return false;
    }
    auto it = d->reservations.find(reservation.reservationId);
    if (it == d->reservations.end() ||
        it->bytes != reservation.bytes ||
        it->ownerSession != reservation.ownerSession) {
        return false;
    }
    d->reservedBytes -= it->bytes;
    if (it->evictable) {
        d->evictableBytes -= it->bytes;
    }
    d->reservations.erase(it);
    return true;
}

KisVulkanMemoryUsageSnapshot KisVulkanMemoryBudgetLedger::usage() const
{
    QMutexLocker locker(&d->mutex);
    KisVulkanMemoryUsageSnapshot snapshot;
    snapshot.ledgerGeneration = d->generation;
    snapshot.reservedBytes = d->reservedBytes;
    snapshot.evictableBytes = d->evictableBytes;
    snapshot.highWatermarkBytes = d->budget.highWatermarkBytes;
    snapshot.hardLimitBytes = d->budget.hardLimitBytes;
    return snapshot;
}

bool KisVulkanMemoryBudgetLedger::isConfigured() const
{
    QMutexLocker locker(&d->mutex);
    return d->generation != 0 && d->budget.isValid();
}

/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanCommandArena.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>

class KisVulkanCommandArena::Private
{
public:
    mutable QMutex mutex;
    KisVulkanCommandArenaConfig config;
    QHash<quint64, KisCompletionTicket> inFlight;
    QString lastFailure;
    bool nativePoolReady = false;
};

KisVulkanCommandArena::KisVulkanCommandArena()
    : d(new Private)
{
}

KisVulkanCommandArena::~KisVulkanCommandArena() = default;

bool KisVulkanCommandArena::configure(const KisVulkanCommandArenaConfig &config)
{
    if (!config.isValid()) return false;
    QMutexLocker locker(&d->mutex);
    if (d->config.isValid()) return false;
    d->config = config;
    d->lastFailure = QStringLiteral("Native Vulkan command pool is not installed");
    return true;
}

bool KisVulkanCommandArena::isOperational() const
{
    QMutexLocker locker(&d->mutex);
    return d->config.isValid() && d->nativePoolReady;
}

QString KisVulkanCommandArena::lastFailure() const
{
    QMutexLocker locker(&d->mutex);
    return d->lastFailure;
}

KisVulkanCommandBufferLease KisVulkanCommandArena::acquire(KisVulkanQueueRole queueRole)
{
    QMutexLocker locker(&d->mutex);
    if (!d->config.isValid()) {
        d->lastFailure = QStringLiteral("Command arena is not configured");
    } else if (queueRole != d->config.queueRole) {
        d->lastFailure = QStringLiteral("Command arena queue role mismatch");
    } else if (quint32(d->inFlight.size()) >= d->config.maximumInFlight) {
        d->lastFailure = QStringLiteral("Command arena reached its bounded in-flight capacity");
    } else if (!d->nativePoolReady) {
        d->lastFailure = QStringLiteral("Native Vulkan command pool is not installed");
    }
    return {};
}

bool KisVulkanCommandArena::retire(const KisVulkanCommandBufferLease &lease,
                                   const KisCompletionTicket &completion)
{
    if (!lease.isValid() || !completion.isValid()) return false;
    QMutexLocker locker(&d->mutex);
    if (lease.arenaGeneration != d->config.arenaGeneration ||
        !d->nativePoolReady || d->inFlight.contains(lease.leaseId)) {
        return false;
    }
    d->inFlight.insert(lease.leaseId, completion);
    return true;
}

int KisVulkanCommandArena::collect(const KisCompletionTicket &completedThrough)
{
    if (!completedThrough.isValid()) return 0;
    QMutexLocker locker(&d->mutex);
    int collected = 0;
    for (auto it = d->inFlight.begin(); it != d->inFlight.end();) {
        if (it->source() == completedThrough.source() &&
            it->value() <= completedThrough.value()) {
            it = d->inFlight.erase(it);
            ++collected;
        } else {
            ++it;
        }
    }
    return collected;
}

int KisVulkanCommandArena::inFlightCount() const
{
    QMutexLocker locker(&d->mutex);
    return d->inFlight.size();
}

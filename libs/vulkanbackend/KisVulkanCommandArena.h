/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_COMMAND_ARENA_H
#define KIS_VULKAN_COMMAND_ARENA_H

#include <QScopedPointer>
#include <QString>

#include "KisPageStoreTypes.h"
#include "KisVulkanSubmissionCoordinator.h"

struct KisVulkanCommandArenaConfig
{
    quint64 arenaGeneration = 0;
    quint64 deviceGeneration = 0;
    KisVulkanQueueRole queueRole = KisVulkanQueueRole::Compute;
    qint32 queueFamily = -1;
    quint32 maximumInFlight = 0;

    bool isValid() const
    {
        return arenaGeneration != 0 && deviceGeneration != 0 &&
               queueFamily >= 0 && maximumInFlight != 0;
    }
};

struct KisVulkanCommandBufferLease
{
    quint64 arenaGeneration = 0;
    quint64 leaseId = 0;
    quint64 encodedCommandId = 0;
    KisVulkanQueueRole queueRole = KisVulkanQueueRole::Compute;

    bool isValid() const
    {
        return arenaGeneration != 0 && leaseId != 0 && encodedCommandId != 0;
    }
};

/**
 * Timeline-retired command-pool contract for one queue-family/thread shard.
 * Native command buffers remain private and cannot be represented by a lease
 * until BR2 installs the pool implementation.
 */
class KisVulkanCommandArena
{
public:
    KisVulkanCommandArena();
    ~KisVulkanCommandArena();

    bool configure(const KisVulkanCommandArenaConfig &config);
    bool isOperational() const;
    QString lastFailure() const;
    KisVulkanCommandBufferLease acquire(KisVulkanQueueRole queueRole);
    bool retire(const KisVulkanCommandBufferLease &lease,
                const KisCompletionTicket &completion);
    int collect(const KisCompletionTicket &completedThrough);
    int inFlightCount() const;

private:
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_VULKAN_COMMAND_ARENA_H

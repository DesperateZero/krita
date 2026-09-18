/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_SUBMISSION_COORDINATOR_H
#define KIS_VULKAN_SUBMISSION_COORDINATOR_H

#include <QByteArray>
#include <QScopedPointer>
#include <QSharedPointer>
#include <QString>
#include <QVector>

#include "KisCompletionRegistry.h"
#include "KisPageStoreTypes.h"
#include "KisVulkanFoundationTypes.h"
#include "kritavulkanbackend_export.h"

enum class KisVulkanQueueRole : quint8 {
    Graphics,
    Compute,
    Transfer,
    Sparse,
    Present
};

enum class KisVulkanWorkloadKind : quint8 {
    FoundationProbe,
    PageTransfer,
    PageBinding,
    Stroke,
    Composite,
    Display,
    Present
};

enum class KisVulkanResourceAccess : quint8 {
    Read,
    Write,
    ReadWrite
};

struct KRITAVULKANBACKEND_EXPORT KisVulkanResourceUse
{
    quint64 resourceId = 0;
    quint64 resourceGeneration = 0;
    KisVulkanResourceAccess access = KisVulkanResourceAccess::Read;
    quint64 byteOffset = 0;
    quint64 byteSize = 0;

    bool isValid() const
    {
        return resourceId != 0 && resourceGeneration != 0 && byteSize != 0;
    }
};

struct KRITAVULKANBACKEND_EXPORT KisVulkanCommandPacket
{
    quint64 packetId = 0;
    KisVulkanWorkloadKind workload = KisVulkanWorkloadKind::FoundationProbe;
    quint64 encodedCommandId = 0;
    QVector<KisVulkanResourceUse> resources;

    bool isValid() const
    {
        if (packetId == 0 || encodedCommandId == 0 || resources.isEmpty()) {
            return false;
        }
        for (const KisVulkanResourceUse &resource : resources) {
            if (!resource.isValid()) return false;
        }
        return true;
    }
};

struct KRITAVULKANBACKEND_EXPORT KisVulkanOwnershipTransfer
{
    quint64 resourceId = 0;
    qint32 sourceQueueFamily = -1;
    qint32 destinationQueueFamily = -1;

    bool isValid() const
    {
        return resourceId != 0 && sourceQueueFamily >= 0 && destinationQueueFamily >= 0 &&
               sourceQueueFamily != destinationQueueFamily;
    }
};

struct KRITAVULKANBACKEND_EXPORT KisVulkanPresentPacket
{
    quint64 viewId = 0;
    quint64 swapchainGeneration = 0;
    quint32 imageIndex = 0;
    KisCompletionTicket renderComplete;

    bool isValid() const
    {
        return viewId != 0 && swapchainGeneration != 0 && renderComplete.isValid();
    }
};

struct KRITAVULKANBACKEND_EXPORT KisVulkanSubmissionRequest
{
    quint64 requestId = 0;
    quint64 deviceGeneration = 0;
    quint64 sessionGeneration = 0;
    bool foundationOperation = false;
    KisVulkanQueueRole queueRole = KisVulkanQueueRole::Compute;
    QVector<KisVulkanCommandPacket> commands;
    QVector<KisCompletionTicket> dependencies;
    QVector<KisVulkanOwnershipTransfer> ownershipTransfers;
    bool sparseBind = false;
    bool hasPresent = false;
    KisVulkanPresentPacket present;
    quint64 failureContinuationId = 0;
    QString debugLabel;

    QString validationError() const;
    bool isValid() const { return validationError().isEmpty(); }
};

enum class KisVulkanCoordinatorState : quint8 {
    Unconfigured,
    Configured,
    Operational,
    Draining,
    Failed,
    Shutdown
};

struct KRITAVULKANBACKEND_EXPORT KisVulkanCoordinatorConfig
{
    quint64 coordinatorGeneration = 0;
    quint64 deviceGeneration = 0;
    QByteArray queueTopologyDigest;
    KisVulkanQueueFamilySelection queues;

    bool isValid() const
    {
        return coordinatorGeneration != 0 && deviceGeneration != 0 &&
               !queueTopologyDigest.isEmpty() && queues.isValid() &&
               queueTopologyDigest == queues.stableDigestInput();
    }
};

/**
 * Sole owner of future queue submit/bind/present operations for one device
 * generation. The current foundation implementation validates complete work
 * packets and completion-source ownership, but remains Configured (not
 * Operational) until the native queue executor and validation tests land.
 */
class KRITAVULKANBACKEND_EXPORT KisVulkanSubmissionCoordinator final
{
public:
    KisVulkanSubmissionCoordinator();
    ~KisVulkanSubmissionCoordinator();

    bool configure(const KisVulkanCoordinatorConfig &config,
                   const QSharedPointer<KisCompletionRegistry> &completionRegistry,
                   QString *error = nullptr);

    KisVulkanCoordinatorState state() const;
    bool isOperational() const;
    QString validate(const KisVulkanSubmissionRequest &request) const;
    QString lastFailure() const;

    KisCompletionTicket submit(const KisVulkanSubmissionRequest &request);
    bool isComplete(const KisCompletionTicket &ticket) const;
    void collectCompletedResources();
    KisCompletionTicket beginShutdown();

private:
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_VULKAN_SUBMISSION_COORDINATOR_H

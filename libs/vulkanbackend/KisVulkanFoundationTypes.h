/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_FOUNDATION_TYPES_H
#define KIS_VULKAN_FOUNDATION_TYPES_H

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include "kritavulkanbackend_export.h"

enum class KisVulkanFoundationState : quint8 {
    Uninitialized,
    Probing,
    Probed,
    Validated,
    Draining,
    DeviceLost,
    Failed,
    Shutdown
};

enum class KisVulkanFailureStage : quint8 {
    None,
    Instance,
    PhysicalDevice,
    QueueTopology,
    LogicalDevice,
    Allocator,
    Submission,
    ShaderManifest,
    Pipeline,
    ReplicaProvider,
    SmokeProbe,
    Wsi,
    Shutdown
};

struct KRITAVULKANBACKEND_EXPORT KisVulkanFailure
{
    KisVulkanFailureStage stage = KisVulkanFailureStage::None;
    qint32 nativeCode = 0;
    bool recoverable = false;
    QString summary;
    QString detail;

    bool isFailure() const
    {
        return stage != KisVulkanFailureStage::None && !summary.isEmpty();
    }
};

struct KRITAVULKANBACKEND_EXPORT KisVulkanQueueFamilySelection
{
    qint32 graphicsFamily = -1;
    qint32 computeFamily = -1;
    qint32 transferFamily = -1;
    qint32 sparseFamily = -1;
    qint32 presentFamily = -1;
    quint32 graphicsQueueCount = 0;
    quint32 computeQueueCount = 0;
    quint32 transferQueueCount = 0;
    bool sparseBindingRequired = false;

    bool isValid() const
    {
        if (graphicsFamily < 0 || computeFamily < 0 || transferFamily < 0 ||
            graphicsQueueCount == 0 || computeQueueCount == 0 || transferQueueCount == 0) {
            return false;
        }
        if (sparseBindingRequired && sparseFamily < 0) {
            return false;
        }
        return true;
    }

    QByteArray stableDigestInput() const;
};

struct KRITAVULKANBACKEND_EXPORT KisVulkanDeviceCapabilitySnapshot
{
    quint64 deviceGeneration = 0;
    quint32 apiVersion = 0;
    QByteArray physicalDeviceIdentity;
    QByteArray driverIdentity;
    QByteArray capabilityDigest;
    KisVulkanQueueFamilySelection queues;
    quint64 deviceLocalBudgetBytes = 0;
    quint64 hostVisibleBudgetBytes = 0;
    quint32 maxStorageBufferRange = 0;
    quint32 maxPushConstantBytes = 0;
    bool timelineSemaphore = false;
    bool synchronization2 = false;
    bool memoryBudget = false;
    bool bufferDeviceAddress = false;
    bool descriptorIndexing = false;
    bool hostVisibleDeviceLocal = false;
    bool coherentSharedMemory = false;

    QString validationError() const;
    bool isValid() const { return validationError().isEmpty(); }
};

struct KRITAVULKANBACKEND_EXPORT KisVulkanFoundationEvidence
{
    quint64 serviceId = 0;
    KisVulkanDeviceCapabilitySnapshot capabilities;
    quint64 allocatorGeneration = 0;
    quint64 coordinatorGeneration = 0;
    quint64 replicaProviderGeneration = 0;
    quint64 shaderAbi = 0;
    QByteArray shaderManifestDigest;
    QByteArray queueTopologyDigest;
    QByteArray smokeProbeDigest;
    bool requiredPipelinesPrepared = false;
    bool completionBridgeRegistered = false;
    bool smokeProbePassed = false;

    QString validationError() const;
    bool isValid() const { return validationError().isEmpty(); }
};

#endif // KIS_VULKAN_FOUNDATION_TYPES_H

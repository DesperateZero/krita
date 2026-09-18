/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanFoundationTypes.h"

#include <QCryptographicHash>

QByteArray KisVulkanQueueFamilySelection::stableDigestInput() const
{
    const QByteArray canonical = QByteArray::number(graphicsFamily) + ':' +
        QByteArray::number(computeFamily) + ':' +
        QByteArray::number(transferFamily) + ':' +
        QByteArray::number(sparseFamily) + ':' +
        QByteArray::number(presentFamily) + ':' +
        QByteArray::number(graphicsQueueCount) + ':' +
        QByteArray::number(computeQueueCount) + ':' +
        QByteArray::number(transferQueueCount) + ':' +
        QByteArray::number(sparseBindingRequired ? 1 : 0);
    return QCryptographicHash::hash(canonical, QCryptographicHash::Sha256);
}

QString KisVulkanDeviceCapabilitySnapshot::validationError() const
{
    if (deviceGeneration == 0) {
        return QStringLiteral("Device capability snapshot has no generation");
    }
    if (apiVersion == 0 || physicalDeviceIdentity.isEmpty() || driverIdentity.isEmpty()) {
        return QStringLiteral("Device or driver identity is incomplete");
    }
    if (capabilityDigest.isEmpty() || !queues.isValid()) {
        return QStringLiteral("Capability digest or queue topology is invalid");
    }
    if (!timelineSemaphore || !synchronization2) {
        return QStringLiteral("Required timeline semaphore or synchronization2 support is absent");
    }
    if (deviceLocalBudgetBytes == 0 || maxStorageBufferRange == 0) {
        return QStringLiteral("Required memory budget or storage-buffer limits are absent");
    }
    return {};
}

QString KisVulkanFoundationEvidence::validationError() const
{
    if (serviceId == 0) {
        return QStringLiteral("Foundation evidence has no service identity");
    }
    const QString capabilityError = capabilities.validationError();
    if (!capabilityError.isEmpty()) {
        return capabilityError;
    }
    if (allocatorGeneration == 0 || coordinatorGeneration == 0 ||
        replicaProviderGeneration == 0) {
        return QStringLiteral("Foundation component generations are incomplete");
    }
    if (shaderAbi == 0 || shaderManifestDigest.isEmpty()) {
        return QStringLiteral("Shader ABI evidence is incomplete");
    }
    if (queueTopologyDigest.isEmpty() ||
        queueTopologyDigest != capabilities.queues.stableDigestInput()) {
        return QStringLiteral("Queue topology evidence does not match the selected device");
    }
    if (!requiredPipelinesPrepared) {
        return QStringLiteral("Required foundation pipelines are not prepared");
    }
    if (!completionBridgeRegistered) {
        return QStringLiteral("Completion bridge has not been registered");
    }
    if (!smokeProbePassed || smokeProbeDigest.isEmpty()) {
        return QStringLiteral("Mandatory foundation smoke probe has not passed");
    }
    return {};
}

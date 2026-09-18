/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanDeviceService.h"

#include <QMutex>
#include <QMutexLocker>

class KisVulkanDeviceService::Private
{
public:
    mutable QMutex mutex;
    KisVulkanFoundationState state = KisVulkanFoundationState::Uninitialized;
    KisVulkanDeviceCapabilitySnapshot capabilities;
    KisVulkanFoundationEvidence evidence;
    KisVulkanFailure failure;
};

KisVulkanDeviceService::KisVulkanDeviceService()
    : d(new Private)
{
}

KisVulkanDeviceService::~KisVulkanDeviceService() = default;

KisVulkanFoundationState KisVulkanDeviceService::state() const
{
    QMutexLocker locker(&d->mutex);
    return d->state;
}

KisVulkanDeviceCapabilitySnapshot KisVulkanDeviceService::capabilities() const
{
    QMutexLocker locker(&d->mutex);
    return d->capabilities;
}

KisVulkanFailure KisVulkanDeviceService::failure() const
{
    QMutexLocker locker(&d->mutex);
    return d->failure;
}

bool KisVulkanDeviceService::beginProbe()
{
    QMutexLocker locker(&d->mutex);
    if (d->state != KisVulkanFoundationState::Uninitialized &&
        d->state != KisVulkanFoundationState::Failed) {
        return false;
    }
    d->failure = {};
    d->state = KisVulkanFoundationState::Probing;
    return true;
}

bool KisVulkanDeviceService::acceptProbeResult(
    const KisVulkanDeviceCapabilitySnapshot &snapshot)
{
    QMutexLocker locker(&d->mutex);
    if (d->state != KisVulkanFoundationState::Probing) {
        return false;
    }

    const QString error = snapshot.validationError();
    if (!error.isEmpty()) {
        d->failure.stage = KisVulkanFailureStage::PhysicalDevice;
        d->failure.summary = QStringLiteral("Vulkan device probe did not meet foundation requirements");
        d->failure.detail = error;
        d->state = KisVulkanFoundationState::Failed;
        return false;
    }

    d->capabilities = snapshot;
    d->state = KisVulkanFoundationState::Probed;
    return true;
}

bool KisVulkanDeviceService::acceptFoundationEvidence(
    const KisVulkanFoundationEvidence &evidence)
{
    QMutexLocker locker(&d->mutex);
    if (d->state != KisVulkanFoundationState::Probed) {
        return false;
    }

    const QString error = evidence.validationError();
    if (!error.isEmpty() ||
        evidence.capabilities.deviceGeneration != d->capabilities.deviceGeneration ||
        evidence.capabilities.capabilityDigest != d->capabilities.capabilityDigest) {
        d->failure.stage = KisVulkanFailureStage::SmokeProbe;
        d->failure.summary = QStringLiteral("Vulkan foundation evidence is incomplete or stale");
        d->failure.detail = error.isEmpty()
            ? QStringLiteral("Capability generation/digest changed during initialization")
            : error;
        d->state = KisVulkanFoundationState::Failed;
        return false;
    }

    d->evidence = evidence;
    d->state = KisVulkanFoundationState::Validated;
    return true;
}

KisBackendValidationReport KisVulkanDeviceService::validationReport() const
{
    QMutexLocker locker(&d->mutex);
    KisBackendValidationReport report;
    if (d->state != KisVulkanFoundationState::Validated || !d->evidence.isValid()) {
        report.failureReason = d->failure.isFailure()
            ? d->failure.summary + QStringLiteral(": ") + d->failure.detail
            : QStringLiteral("Vulkan foundation has not produced complete readiness evidence");
        return report;
    }

    report.serviceId = d->evidence.serviceId;
    report.deviceGeneration = d->evidence.capabilities.deviceGeneration;
    report.allocatorGeneration = d->evidence.allocatorGeneration;
    report.coordinatorGeneration = d->evidence.coordinatorGeneration;
    report.replicaProviderGeneration = d->evidence.replicaProviderGeneration;
    report.shaderAbi = d->evidence.shaderAbi;
    report.queueTopologyDigest = d->evidence.queueTopologyDigest;
    report.capabilityDigest = d->evidence.capabilities.capabilityDigest;
    report.shaderManifestDigest = d->evidence.shaderManifestDigest;
    report.smokeProbeDigest = d->evidence.smokeProbeDigest;
    report.deviceReady = true;
    report.queuesReady = true;
    report.shadersReady = true;
    report.coordinatorReady = true;
    report.replicaProviderReady = true;
    return report;
}

bool KisVulkanDeviceService::beginDraining()
{
    QMutexLocker locker(&d->mutex);
    if (d->state != KisVulkanFoundationState::Validated &&
        d->state != KisVulkanFoundationState::DeviceLost) {
        return false;
    }
    d->state = KisVulkanFoundationState::Draining;
    return true;
}

bool KisVulkanDeviceService::markDeviceLost(const QString &reason)
{
    if (reason.isEmpty()) {
        return false;
    }
    QMutexLocker locker(&d->mutex);
    if (d->state == KisVulkanFoundationState::Shutdown ||
        d->state == KisVulkanFoundationState::Uninitialized) {
        return false;
    }
    d->failure.stage = KisVulkanFailureStage::LogicalDevice;
    d->failure.summary = QStringLiteral("Vulkan device generation was lost");
    d->failure.detail = reason;
    d->failure.recoverable = true;
    d->state = KisVulkanFoundationState::DeviceLost;
    return true;
}

bool KisVulkanDeviceService::finishShutdown()
{
    QMutexLocker locker(&d->mutex);
    if (d->state != KisVulkanFoundationState::Draining &&
        d->state != KisVulkanFoundationState::Failed) {
        return false;
    }
    d->state = KisVulkanFoundationState::Shutdown;
    return true;
}

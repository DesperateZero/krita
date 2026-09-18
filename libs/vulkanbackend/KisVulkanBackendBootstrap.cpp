/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanBackendBootstrap.h"

#include "KisVulkanDeviceService.h"
#include "KisVulkanDocumentGpuSession.h"

class KisVulkanBackendBootstrap::Private
{
public:
    KisVulkanDeviceService deviceService;
    quint64 nextSessionGeneration = 1;
};

KisVulkanBackendBootstrap::KisVulkanBackendBootstrap()
    : d(new Private)
{
}

KisVulkanBackendBootstrap::~KisVulkanBackendBootstrap() = default;

KisVulkanBackendInitResult KisVulkanBackendBootstrap::initialize()
{
    KisVulkanBackendInitResult result;
    result.state = KisVulkanBackendState::Disabled;
    result.failure.stage = KisVulkanFailureStage::Instance;
    result.failure.summary = QStringLiteral("No Vulkan device probe implementation is installed");
    result.failure.detail = QStringLiteral(
        "Use the foundation probe to produce immutable capability and component evidence");
    result.failureReason = result.failure.summary + QStringLiteral(": ") + result.failure.detail;
    return result;
}

KisVulkanBackendInitResult KisVulkanBackendBootstrap::initializeFromEvidence(
    const KisVulkanFoundationEvidence &evidence,
    KisBackendServiceRegistry &serviceRegistry)
{
    KisVulkanBackendInitResult result;
    if (!d->deviceService.beginProbe() ||
        !d->deviceService.acceptProbeResult(evidence.capabilities) ||
        !d->deviceService.acceptFoundationEvidence(evidence)) {
        result.state = KisVulkanBackendState::Failed;
        result.failure = d->deviceService.failure();
        result.failureReason = result.failure.isFailure()
            ? result.failure.summary + QStringLiteral(": ") + result.failure.detail
            : QStringLiteral("Foundation evidence was rejected by the device service state machine");
        return result;
    }

    const KisBackendValidationReport report = d->deviceService.validationReport();
    result.readyToken = serviceRegistry.acceptValidatedService(report);
    if (!result.readyToken.isValid()) {
        result.state = KisVulkanBackendState::Failed;
        result.failure.stage = KisVulkanFailureStage::SmokeProbe;
        result.failure.summary = QStringLiteral("Backend service registry rejected readiness evidence");
        result.failureReason = result.failure.summary;
        return result;
    }
    result.state = KisVulkanBackendState::Ready;
    return result;
}

QSharedPointer<KisDocumentGpuSession> KisVulkanBackendBootstrap::createDocumentSession(
    quint64 documentId,
    const KisBackendReadyToken &readyToken)
{
    if (documentId == 0 || !readyToken.isValid() ||
        d->deviceService.state() != KisVulkanFoundationState::Validated) {
        return {};
    }
    const KisBackendValidationReport report = d->deviceService.validationReport();
    if (!report.isComplete() ||
        report.serviceId != readyToken.serviceId() ||
        report.deviceGeneration != readyToken.deviceGeneration() ||
        report.shaderAbi != readyToken.shaderAbi()) {
        return {};
    }
    return QSharedPointer<KisDocumentGpuSession>(
        new KisVulkanDocumentGpuSession(documentId,
                                        readyToken.serviceId(),
                                        readyToken.deviceGeneration(),
                                        d->nextSessionGeneration++));
}

KisVulkanFoundationState KisVulkanBackendBootstrap::foundationState() const
{
    return d->deviceService.state();
}

/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanCanvasAdapter.h"

KisVulkanCanvasAdapter::KisVulkanCanvasAdapter() = default;
KisVulkanCanvasAdapter::~KisVulkanCanvasAdapter() = default;

KisVulkanCanvasActivationDecision KisVulkanCanvasAdapter::evaluate(
    const KisVulkanCanvasActivationEvidence &evidence) const
{
    KisVulkanCanvasActivationDecision decision;
    if (!evidence.readyToken.isValid()) {
        decision.reason = QStringLiteral("Backend readiness token is invalid");
    } else if (!evidence.session ||
               evidence.session->state() != KisAccelerationSessionState::Ready ||
               !evidence.session->acceptsNewWork()) {
        decision.reason = QStringLiteral("Document acceleration session is not ready");
    } else if (!evidence.swapchain.isReady()) {
        decision.reason = QStringLiteral("View has no ready swapchain generation");
    } else if (evidence.session->backendServiceId() != evidence.readyToken.serviceId() ||
               evidence.session->deviceGeneration() != evidence.readyToken.deviceGeneration() ||
               evidence.swapchain.deviceGeneration() != evidence.readyToken.deviceGeneration()) {
        decision.reason = QStringLiteral("Backend, document session, and swapchain generations do not match");
    } else if (!evidence.display.isValid()) {
        decision.reason = evidence.display.validationError();
    } else if (!evidence.requestedFormatSupported) {
        decision.reason = QStringLiteral("Requested document/display format is unsupported");
    } else if (!evidence.fallbackCanvasAvailable) {
        decision.reason = QStringLiteral("Safe CPU/OpenGL fallback canvas is unavailable");
    } else if (!evidence.productGate.isValid()) {
        decision.reason = QStringLiteral("Vulkan MVP product gate has not passed");
    } else {
        decision.allowed = true;
    }
    return decision;
}

bool KisVulkanCanvasAdapter::canReplaceCanvas(
    const KisVulkanCanvasActivationEvidence &evidence) const
{
    return evaluate(evidence).allowed;
}

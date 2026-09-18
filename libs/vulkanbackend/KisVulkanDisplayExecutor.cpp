/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanDisplayExecutor.h"

KisVulkanDisplayExecutor::KisVulkanDisplayExecutor() = default;
KisVulkanDisplayExecutor::~KisVulkanDisplayExecutor() = default;

bool KisVulkanDisplayExecutor::configure(
    quint64 deviceGeneration,
    const QSharedPointer<KisVulkanPipelineRepository> &pipelines)
{
    if (m_deviceGeneration != 0 || deviceGeneration == 0 || !pipelines) {
        return false;
    }
    m_deviceGeneration = deviceGeneration;
    m_pipelines = pipelines;
    return true;
}

KisVulkanDisplayPreparation KisVulkanDisplayExecutor::prepare(
    const KisVulkanDisplayRequest &request,
    const KisVulkanPassBindingSet &bindings,
    const KisVulkanPipelineKey &pipelineKey) const
{
    KisVulkanDisplayPreparation preparation;
    if (m_deviceGeneration == 0 || !m_pipelines) {
        preparation.error = QStringLiteral("Display executor is not configured");
        return preparation;
    }
    preparation.error = request.validationError();
    if (!preparation.error.isEmpty()) return preparation;
    preparation.error = bindings.validationError();
    if (!preparation.error.isEmpty()) return preparation;
    if (bindings.deviceGeneration != m_deviceGeneration) {
        preparation.error = QStringLiteral("Display binding targets a stale device generation");
        return preparation;
    }

    bool hasDisplaySurface = false;
    for (const KisVulkanBoundPage &page : bindings.pages) {
        if (!page.writable &&
            page.version.key.surface == request.display.surface.surface) {
            hasDisplaySurface = true;
            break;
        }
    }
    if (!hasDisplaySurface) {
        preparation.error = QStringLiteral("Display bindings omit the exact DisplaySurface snapshot");
        return preparation;
    }
    if (!pipelineKey.isValid() ||
        pipelineKey.surfaceFormatId != request.display.format.formatId) {
        preparation.status = KisVulkanPassPreparationStatus::Unsupported;
        preparation.error = QStringLiteral("Display pipeline does not support the snapshot format");
        return preparation;
    }

    QString pipelineError;
    preparation.pipeline = m_pipelines->declarePipeline(pipelineKey, &pipelineError);
    if (!preparation.pipeline.isValid()) {
        preparation.status = KisVulkanPassPreparationStatus::Unsupported;
        preparation.error = pipelineError;
        return preparation;
    }
    if (m_pipelines->state(preparation.pipeline) != KisVulkanPipelineState::Prepared) {
        preparation.status = KisVulkanPassPreparationStatus::WaitingForPipeline;
        preparation.error = QStringLiteral("Display pipeline is declared but not prepared");
        return preparation;
    }
    // Readiness tickets become GPU-side submission dependencies; their mere
    // presence must not trigger a host wait or prevent command encoding.
    preparation.status = KisVulkanPassPreparationStatus::ReadyForEncoding;
    preparation.error.clear();
    return preparation;
}

/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanCompositeExecutor.h"

#include <QSet>

KisVulkanCompositeExecutor::KisVulkanCompositeExecutor() = default;
KisVulkanCompositeExecutor::~KisVulkanCompositeExecutor() = default;

bool KisVulkanCompositeExecutor::configure(
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

QString KisVulkanCompositeExecutor::validate(
    const KisCompositeAccessPlan &plan,
    const KisVulkanPassBindingSet &bindings) const
{
    if (m_deviceGeneration == 0 || !m_pipelines) {
        return QStringLiteral("Composite executor is not configured");
    }
    const QString planError = plan.validationError();
    if (!planError.isEmpty()) return planError;
    const QString bindingError = bindings.validationError();
    if (!bindingError.isEmpty()) return bindingError;
    if (bindings.deviceGeneration != m_deviceGeneration) {
        return QStringLiteral("Composite binding targets a stale device generation");
    }

    QSet<quint64> boundSurfaceIds;
    bool hasWritableOutput = false;
    for (const KisVulkanBoundPage &bound : bindings.pages) {
        boundSurfaceIds.insert(bound.version.key.surface.value);
        if (bound.writable && bound.version.key.surface == plan.output.surface) {
            hasWritableOutput = true;
        }
    }
    if (!hasWritableOutput) {
        return QStringLiteral("Composite binding has no writable output surface page");
    }
    for (const KisCompositeDependency &dependency : plan.dependencies) {
        if (!boundSurfaceIds.contains(dependency.surface.surface.value)) {
            return QStringLiteral("Composite binding omits a required dependency surface");
        }
    }
    return {};
}

KisVulkanCompositePreparation KisVulkanCompositeExecutor::prepare(
    const KisCompositeAccessPlan &plan,
    const KisVulkanPassBindingSet &bindings,
    const KisVulkanPipelineKey &pipelineKey) const
{
    KisVulkanCompositePreparation preparation;
    preparation.output = plan.output;
    preparation.error = validate(plan, bindings);
    if (!preparation.error.isEmpty()) {
        return preparation;
    }
    if (!pipelineKey.isValid() ||
        pipelineKey.surfaceFormatId != plan.workingFormat.formatId ||
        pipelineKey.programId != plan.operationId ||
        pipelineKey.shaderSemanticVersion != plan.semanticVersion) {
        preparation.status = KisVulkanPassPreparationStatus::Unsupported;
        preparation.error = QStringLiteral("Composite pipeline key does not match operation semantics or working format");
        return preparation;
    }

    QString declareError;
    preparation.pipeline = m_pipelines->declarePipeline(pipelineKey, &declareError);
    if (!preparation.pipeline.isValid()) {
        preparation.status = KisVulkanPassPreparationStatus::Unsupported;
        preparation.error = declareError;
        return preparation;
    }
    if (m_pipelines->state(preparation.pipeline) != KisVulkanPipelineState::Prepared) {
        preparation.status = KisVulkanPassPreparationStatus::WaitingForPipeline;
        preparation.error = QStringLiteral("Composite pipeline is declared but not prepared");
        return preparation;
    }
    preparation.status = KisVulkanPassPreparationStatus::ReadyForEncoding;
    preparation.error.clear();
    // BR4 records the native command packet. Returning ReadyForEncoding rather
    // than a fake Encoded packet prevents accidental submission today.
    return preparation;
}

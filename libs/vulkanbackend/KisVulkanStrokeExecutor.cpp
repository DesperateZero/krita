/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanStrokeExecutor.h"

KisVulkanStrokeExecutor::KisVulkanStrokeExecutor() = default;
KisVulkanStrokeExecutor::~KisVulkanStrokeExecutor() = default;

bool KisVulkanStrokeExecutor::configure(
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

QString KisVulkanStrokeExecutor::validate(
    const KisPageWorkPlan &plan,
    const KisVulkanPassBindingSet &bindings) const
{
    if (m_deviceGeneration == 0 || !m_pipelines) {
        return QStringLiteral("Stroke executor is not configured");
    }
    if (!plan.isReady()) {
        return plan.error.isEmpty()
            ? QStringLiteral("Stroke page-work plan is not ready")
            : plan.error;
    }
    const QString bindingError = bindings.validationError();
    if (!bindingError.isEmpty()) return bindingError;
    if (bindings.deviceGeneration != m_deviceGeneration) {
        return QStringLiteral("Stroke binding targets a stale device generation");
    }

    for (const KisPageJob &job : plan.pageJobs) {
        bool foundWritableGeneration = false;
        quint64 previousSequence = 0;
        quint32 previousSubSequence = 0;
        for (const KisPageDabRef &dab : job.orderedDabs) {
            if (dab.sequence < previousSequence ||
                (dab.sequence == previousSequence && dab.subSequence < previousSubSequence)) {
                return QStringLiteral("Stroke page job does not preserve dab order");
            }
            previousSequence = dab.sequence;
            previousSubSequence = dab.subSequence;
        }
        for (const KisVulkanBoundPage &bound : bindings.pages) {
            if (bound.writable && bound.version.key == job.target &&
                bound.version.generation == job.pendingGeneration) {
                foundWritableGeneration = true;
                break;
            }
        }
        if (!foundWritableGeneration) {
            return QStringLiteral("Stroke binding omits an exact writable page generation");
        }
    }
    return {};
}

KisVulkanStrokePreparation KisVulkanStrokeExecutor::prepare(
    const KisPageWorkPlan &plan,
    const KisVulkanPassBindingSet &bindings,
    const KisVulkanPipelineKey &pipelineKey) const
{
    KisVulkanStrokePreparation preparation;
    preparation.error = validate(plan, bindings);
    if (!preparation.error.isEmpty()) return preparation;
    if (!pipelineKey.isValid()) {
        preparation.status = KisVulkanPassPreparationStatus::Unsupported;
        preparation.error = pipelineKey.validationError();
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
        preparation.error = QStringLiteral("Stroke pipeline is declared but not prepared");
        return preparation;
    }
    for (const KisPageJob &job : plan.pageJobs) {
        KisPageVersion output;
        output.key = job.target;
        output.generation = job.pendingGeneration;
        preparation.pendingOutputs.append(output);
    }
    preparation.status = KisVulkanPassPreparationStatus::ReadyForEncoding;
    preparation.error.clear();
    return preparation;
}

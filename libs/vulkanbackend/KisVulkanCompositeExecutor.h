/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_COMPOSITE_EXECUTOR_H
#define KIS_VULKAN_COMPOSITE_EXECUTOR_H

#include <QSharedPointer>

#include "KisCompositeTypes.h"
#include "KisVulkanPassTypes.h"
#include "KisVulkanPipelineRepository.h"
#include "KisVulkanSubmissionCoordinator.h"

struct KisVulkanCompositePreparation
{
    KisVulkanPassPreparationStatus status = KisVulkanPassPreparationStatus::Invalid;
    KisSurfaceVersion output;
    KisVulkanPipelineHandle pipeline;
    KisVulkanSubmissionRequest submission;
    QString error;

    bool isReadyForEncoding() const
    {
        return status == KisVulkanPassPreparationStatus::ReadyForEncoding &&
               output.isValid() && pipeline.isValid() && error.isEmpty();
    }
};

/**
 * Specialized bridge from an exact EvalGraphIR composite operation to a
 * Vulkan pass. It does not interpret an authoring graph, plan the evaluation
 * DAG, acquire pages, submit queues, or publish the derived surface.
 */
class KisVulkanCompositeExecutor
{
public:
    KisVulkanCompositeExecutor();
    ~KisVulkanCompositeExecutor();

    bool configure(quint64 deviceGeneration,
                   const QSharedPointer<KisVulkanPipelineRepository> &pipelines);
    QString validate(const KisCompositeAccessPlan &plan,
                     const KisVulkanPassBindingSet &bindings) const;
    KisVulkanCompositePreparation prepare(
        const KisCompositeAccessPlan &plan,
        const KisVulkanPassBindingSet &bindings,
        const KisVulkanPipelineKey &pipelineKey) const;

private:
    quint64 m_deviceGeneration = 0;
    QSharedPointer<KisVulkanPipelineRepository> m_pipelines;
};

#endif // KIS_VULKAN_COMPOSITE_EXECUTOR_H

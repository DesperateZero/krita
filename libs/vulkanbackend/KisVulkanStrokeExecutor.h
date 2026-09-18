/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_STROKE_EXECUTOR_H
#define KIS_VULKAN_STROKE_EXECUTOR_H

#include <QSharedPointer>

#include "KisGpuStrokeTypes.h"
#include "KisVulkanPassTypes.h"
#include "KisVulkanPipelineRepository.h"
#include "KisVulkanSubmissionCoordinator.h"

struct KisVulkanStrokePreparation
{
    KisVulkanPassPreparationStatus status = KisVulkanPassPreparationStatus::Invalid;
    KisVulkanPipelineHandle pipeline;
    QVector<KisPageVersion> pendingOutputs;
    QString error;

    bool isReadyForEncoding() const
    {
        return status == KisVulkanPassPreparationStatus::ReadyForEncoding &&
               pipeline.isValid() && !pendingOutputs.isEmpty() && error.isEmpty();
    }
};

/**
 * Page-batched stroke pass boundary. It validates exact write bindings,
 * ordering plans and prepared pipeline identity; native command recording is
 * deliberately deferred to BR5.
 */
class KisVulkanStrokeExecutor
{
public:
    KisVulkanStrokeExecutor();
    ~KisVulkanStrokeExecutor();

    bool configure(quint64 deviceGeneration,
                   const QSharedPointer<KisVulkanPipelineRepository> &pipelines);
    QString validate(const KisPageWorkPlan &plan,
                     const KisVulkanPassBindingSet &bindings) const;
    KisVulkanStrokePreparation prepare(
        const KisPageWorkPlan &plan,
        const KisVulkanPassBindingSet &bindings,
        const KisVulkanPipelineKey &pipelineKey) const;

private:
    quint64 m_deviceGeneration = 0;
    QSharedPointer<KisVulkanPipelineRepository> m_pipelines;
};

#endif // KIS_VULKAN_STROKE_EXECUTOR_H

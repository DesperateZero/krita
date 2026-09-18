/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_DISPLAY_EXECUTOR_H
#define KIS_VULKAN_DISPLAY_EXECUTOR_H

#include <QByteArray>
#include <QSharedPointer>

#include "KisCompositeTypes.h"
#include "KisVulkanPassTypes.h"
#include "KisVulkanPipelineRepository.h"

struct KisVulkanDisplayRequest
{
    quint64 viewId = 0;
    quint64 viewGeneration = 0;
    KisDisplaySurfaceSnapshot display;
    QByteArray outputColorSpace;
    bool checkerboard = false;
    bool pixelGrid = false;

    QString validationError() const
    {
        if (viewId == 0 || viewGeneration == 0) {
            return QStringLiteral("Display request has no view identity");
        }
        const QString displayError = display.validationError();
        if (!displayError.isEmpty()) return displayError;
        if (outputColorSpace.isEmpty()) {
            return QStringLiteral("Display request has no output color-space identity");
        }
        return {};
    }
};

struct KisVulkanDisplayPreparation
{
    KisVulkanPassPreparationStatus status = KisVulkanPassPreparationStatus::Invalid;
    KisVulkanPipelineHandle pipeline;
    QString error;
};

class KisVulkanDisplayExecutor
{
public:
    KisVulkanDisplayExecutor();
    ~KisVulkanDisplayExecutor();

    bool configure(quint64 deviceGeneration,
                   const QSharedPointer<KisVulkanPipelineRepository> &pipelines);
    KisVulkanDisplayPreparation prepare(
        const KisVulkanDisplayRequest &request,
        const KisVulkanPassBindingSet &bindings,
        const KisVulkanPipelineKey &pipelineKey) const;

private:
    quint64 m_deviceGeneration = 0;
    QSharedPointer<KisVulkanPipelineRepository> m_pipelines;
};

#endif // KIS_VULKAN_DISPLAY_EXECUTOR_H

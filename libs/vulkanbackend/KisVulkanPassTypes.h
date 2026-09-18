/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_PASS_TYPES_H
#define KIS_VULKAN_PASS_TYPES_H

#include <QString>
#include <QVector>

#include "KisPageStoreTypes.h"

struct KisVulkanBoundPage
{
    KisPageVersion version;
    KisGpuPageAccessView access;
    quint64 resourceId = 0;
    quint64 resourceGeneration = 0;
    bool writable = false;

    bool isValid() const
    {
        return version.isValid() && access.isValid() &&
               resourceId != 0 && resourceGeneration != 0;
    }
};

struct KisVulkanPassBindingSet
{
    quint64 operationId = 0;
    quint64 deviceGeneration = 0;
    quint64 sessionGeneration = 0;
    QVector<KisVulkanBoundPage> pages;
    QVector<KisCompletionTicket> readiness;

    QString validationError() const
    {
        if (operationId == 0 || deviceGeneration == 0 || sessionGeneration == 0) {
            return QStringLiteral("Pass binding identity or generation is missing");
        }
        if (pages.isEmpty()) {
            return QStringLiteral("Pass binding set contains no pages");
        }
        for (const KisVulkanBoundPage &page : pages) {
            if (!page.isValid() || page.access.operationId != operationId) {
                return QStringLiteral("Pass binding contains an invalid or foreign page access view");
            }
        }
        for (const KisCompletionTicket &ticket : readiness) {
            if (!ticket.isValid()) {
                return QStringLiteral("Pass binding contains an invalid readiness ticket");
            }
        }
        return {};
    }

    bool isValid() const { return validationError().isEmpty(); }
};

enum class KisVulkanPassPreparationStatus : quint8 {
    Invalid,
    Unsupported,
    WaitingForResources,
    WaitingForPipeline,
    ReadyForEncoding,
    Encoded
};

#endif // KIS_VULKAN_PASS_TYPES_H

/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_PRESENTER_H
#define KIS_VULKAN_PRESENTER_H

#include "KisCompositeTypes.h"
#include "KisVulkanViewAttachment.h"
#include "kritavulkanuiadapter_export.h"

struct KRITAVULKANUIADAPTER_EXPORT KisPresentIntent
{
    KisVulkanViewId viewId;
    quint64 viewGeneration = 0;
    KisDisplaySurfaceSnapshot display;

    QString validationError() const
    {
        if (!viewId.isValid() || viewGeneration == 0) {
            return QStringLiteral("Present intent has no view identity");
        }
        return display.validationError();
    }

    bool isValid() const { return validationError().isEmpty(); }
};

struct KRITAVULKANUIADAPTER_EXPORT KisPresentIntentResult
{
    KisPresentIntent intent;
    QString error;

    bool isValid() const { return error.isEmpty() && intent.isValid(); }
};

/**
 * UI-side selector of immutable display/view intent. Actual acquire,
 * swapchain image selection, graphics submission and present stay in backend
 * WSI/SubmissionCoordinator.
 */
class KRITAVULKANUIADAPTER_EXPORT KisVulkanPresenter
{
public:
    KisVulkanPresenter();
    ~KisVulkanPresenter();

    KisPresentIntentResult buildPresentIntent(
        KisVulkanViewId viewId,
        quint64 viewGeneration,
        const KisDisplaySurfaceSnapshot &display) const;
};

#endif // KIS_VULKAN_PRESENTER_H

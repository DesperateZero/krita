/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanPresenter.h"

KisVulkanPresenter::KisVulkanPresenter() = default;
KisVulkanPresenter::~KisVulkanPresenter() = default;

KisPresentIntentResult KisVulkanPresenter::buildPresentIntent(
    KisVulkanViewId viewId,
    quint64 viewGeneration,
    const KisDisplaySurfaceSnapshot &display) const
{
    KisPresentIntentResult result;
    result.intent.viewId = viewId;
    result.intent.viewGeneration = viewGeneration;
    result.intent.display = display;
    result.error = result.intent.validationError();
    return result;
}

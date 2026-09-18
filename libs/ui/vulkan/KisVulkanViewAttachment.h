/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_VIEW_ATTACHMENT_H
#define KIS_VULKAN_VIEW_ATTACHMENT_H

#include <QScopedPointer>
#include <QSharedPointer>
#include <QString>

#include "KisDocumentGpuSession.h"
#include "KisVulkanWsiService.h"
#include "kritavulkanuiadapter_export.h"

struct KRITAVULKANUIADAPTER_EXPORT KisVulkanViewId
{
    KisVulkanViewHandle backendView;

    bool isValid() const { return backendView.isValid(); }
};

/**
 * Per-canvas UI attachment. It validates document/session ownership and sends
 * immutable native-window intents to the backend WSI service; it never owns a
 * swapchain or canonical pixels.
 */
class KRITAVULKANUIADAPTER_EXPORT KisVulkanViewAttachment
{
public:
    KisVulkanViewAttachment();
    ~KisVulkanViewAttachment();

    bool configure(const QSharedPointer<KisVulkanWsiService> &wsiService);
    bool isOperational() const;
    KisVulkanViewId attach(const KisVulkanViewIntent &intent,
                           const KisBackendReadyToken &readyToken,
                           const QSharedPointer<KisDocumentGpuSession> &session);
    bool update(KisVulkanViewId viewId, const KisVulkanViewIntent &intent);
    KisVulkanViewRetirement detach(KisVulkanViewId viewId);
    QString failureReason() const;

private:
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_VULKAN_VIEW_ATTACHMENT_H

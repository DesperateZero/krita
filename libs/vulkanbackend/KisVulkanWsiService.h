/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_WSI_SERVICE_H
#define KIS_VULKAN_WSI_SERVICE_H

#include <QByteArray>
#include <QScopedPointer>
#include <QSharedPointer>
#include <QSize>
#include <QString>

#include "KisPageStoreTypes.h"
#include "kritavulkanbackend_export.h"

class KisVulkanSubmissionCoordinator;

enum class KisNativeSurfacePlatform : quint8 {
    Unknown,
    Win32,
    Xcb,
    Wayland,
    Metal
};

struct KRITAVULKANBACKEND_EXPORT KisNativeSurfaceDescriptor
{
    KisNativeSurfacePlatform platform = KisNativeSurfacePlatform::Unknown;
    QByteArray nativeWindowToken;
    QByteArray displayOrLayerToken;
    quint64 lifetimeGeneration = 0;
    QSize pixelExtent;

    QString validationError() const
    {
        if (platform == KisNativeSurfacePlatform::Unknown) {
            return QStringLiteral("Native surface platform is unknown");
        }
        if (nativeWindowToken.isEmpty() || lifetimeGeneration == 0) {
            return QStringLiteral("Native surface token or lifetime generation is missing");
        }
        if (!pixelExtent.isValid()) {
            return QStringLiteral("Native surface extent is invalid");
        }
        return {};
    }

    bool isValid() const { return validationError().isEmpty(); }
};

struct KRITAVULKANBACKEND_EXPORT KisVulkanViewHandle
{
    quint64 serviceGeneration = 0;
    quint64 viewId = 0;

    bool isValid() const
    {
        return serviceGeneration != 0 && viewId != 0;
    }
};

struct KRITAVULKANBACKEND_EXPORT KisVulkanViewIntent
{
    quint64 documentId = 0;
    quint64 documentSessionGeneration = 0;
    quint64 viewGeneration = 0;
    KisNativeSurfaceDescriptor surface;
    bool visible = true;
    QByteArray outputColorSpace;

    QString validationError() const
    {
        if (documentId == 0 || documentSessionGeneration == 0 || viewGeneration == 0) {
            return QStringLiteral("View intent identity or generation is missing");
        }
        const QString surfaceError = surface.validationError();
        if (!surfaceError.isEmpty()) return surfaceError;
        if (outputColorSpace.isEmpty()) {
            return QStringLiteral("View intent has no output color-space identity");
        }
        return {};
    }

    bool isValid() const { return validationError().isEmpty(); }
};

enum class KisVulkanWsiSessionState : quint8 {
    IntentRegistered,
    AwaitingNativeSurface,
    AwaitingSwapchain,
    Ready,
    Retiring,
    Failed,
    Closed
};

class KRITAVULKANBACKEND_EXPORT KisVulkanSwapchainSnapshot
{
public:
    KisVulkanViewHandle view() const { return m_view; }
    quint64 deviceGeneration() const { return m_deviceGeneration; }
    quint64 surfaceGeneration() const { return m_surfaceGeneration; }
    quint64 swapchainGeneration() const { return m_swapchainGeneration; }
    QSize pixelExtent() const { return m_pixelExtent; }
    QByteArray surfaceFormat() const { return m_surfaceFormat; }
    QByteArray outputColorSpace() const { return m_outputColorSpace; }
    KisVulkanWsiSessionState state() const { return m_state; }
    QString failureReason() const { return m_failureReason; }

    bool isReady() const
    {
        return m_view.isValid() && m_deviceGeneration != 0 && m_surfaceGeneration != 0 &&
               m_swapchainGeneration != 0 && m_pixelExtent.isValid() &&
               !m_surfaceFormat.isEmpty() && !m_outputColorSpace.isEmpty() &&
               m_state == KisVulkanWsiSessionState::Ready && m_failureReason.isEmpty();
    }

private:
    KisVulkanViewHandle m_view;
    quint64 m_deviceGeneration = 0;
    quint64 m_surfaceGeneration = 0;
    quint64 m_swapchainGeneration = 0;
    QSize m_pixelExtent;
    QByteArray m_surfaceFormat;
    QByteArray m_outputColorSpace;
    KisVulkanWsiSessionState m_state = KisVulkanWsiSessionState::IntentRegistered;
    QString m_failureReason;

    friend class KisVulkanWsiService;
};

struct KRITAVULKANBACKEND_EXPORT KisVulkanViewRetirement
{
    bool accepted = false;
    bool completedImmediately = false;
    KisCompletionTicket completion;
    QString error;

    bool isValid() const
    {
        return accepted && error.isEmpty() &&
               (completedImmediately || completion.isValid());
    }
};

/**
 * Backend owner for per-view surface/swapchain generations. Foundation code
 * can register and version view intents; native VkSurfaceKHR/swapchain creation
 * remains explicitly AwaitingSwapchain until BR6.
 */
class KRITAVULKANBACKEND_EXPORT KisVulkanWsiService
{
public:
    KisVulkanWsiService();
    ~KisVulkanWsiService();

    bool configure(quint64 serviceGeneration,
                   quint64 deviceGeneration,
                   const QSharedPointer<KisVulkanSubmissionCoordinator> &coordinator);
    KisVulkanViewHandle registerViewIntent(const KisVulkanViewIntent &intent,
                                           QString *error = nullptr);
    bool updateViewIntent(const KisVulkanViewHandle &view,
                          const KisVulkanViewIntent &intent,
                          QString *error = nullptr);
    KisVulkanSwapchainSnapshot snapshot(const KisVulkanViewHandle &view) const;
    KisVulkanViewRetirement retireView(const KisVulkanViewHandle &view);
    int viewCount() const;

private:
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_VULKAN_WSI_SERVICE_H

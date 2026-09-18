/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_BACKEND_BOOTSTRAP_H
#define KIS_VULKAN_BACKEND_BOOTSTRAP_H

#include <QSharedPointer>
#include <QScopedPointer>
#include <QString>

#include "KisBackendServiceRegistry.h"
#include "KisDocumentGpuSession.h"
#include "KisVulkanFoundationTypes.h"
#include "kritavulkanbackend_export.h"

enum class KisVulkanBackendState : quint8 {
    Disabled,
    Probing,
    Initializing,
    Ready,
    Draining,
    Failed,
    DeviceLost,
    Shutdown
};

struct KRITAVULKANBACKEND_EXPORT KisVulkanBackendInitResult
{
    KisVulkanBackendState state = KisVulkanBackendState::Disabled;
    KisBackendReadyToken readyToken;
    KisVulkanFailure failure;
    QString failureReason;

    bool isReady() const
    {
        return state == KisVulkanBackendState::Ready && readyToken.isValid();
    }
};

/**
 * Device-service composition root. The normal entry remains Disabled until a
 * native probe exists. Tests/future bootstrap may provide complete immutable
 * foundation evidence; only the core registry can then issue a ready token.
 */
class KRITAVULKANBACKEND_EXPORT KisVulkanBackendBootstrap final
{
public:
    KisVulkanBackendBootstrap();
    ~KisVulkanBackendBootstrap();

    KisVulkanBackendInitResult initialize();
    KisVulkanBackendInitResult initializeFromEvidence(
        const KisVulkanFoundationEvidence &evidence,
        KisBackendServiceRegistry &serviceRegistry);
    QSharedPointer<KisDocumentGpuSession> createDocumentSession(
        quint64 documentId,
        const KisBackendReadyToken &readyToken);

    KisVulkanFoundationState foundationState() const;

private:
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_VULKAN_BACKEND_BOOTSTRAP_H

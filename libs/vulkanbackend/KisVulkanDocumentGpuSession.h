/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_DOCUMENT_GPU_SESSION_H
#define KIS_VULKAN_DOCUMENT_GPU_SESSION_H

#include <QMutex>

#include "KisDocumentGpuSession.h"
#include "kritavulkanbackend_export.h"

/**
 * Document-scoped Vulkan session. It deliberately has no UI object or
 * active-view API and cannot become ready in BR0.
 */
class KRITAVULKANBACKEND_EXPORT KisVulkanDocumentGpuSession final
    : public KisDocumentGpuSession
{
public:
    KisVulkanDocumentGpuSession(quint64 documentId,
                                quint64 backendServiceId,
                                quint64 deviceGeneration,
                                quint64 sessionGeneration);
    ~KisVulkanDocumentGpuSession() override;

    quint64 documentId() const override;
    quint64 backendServiceId() const override;
    quint64 deviceGeneration() const override;
    quint64 sessionGeneration() const override;
    KisAccelerationSessionState state() const override;
    KisAccelerationCapabilities capabilities() const override;
    bool acceptsNewWork() const override;
    QString failureReason() const override;
    KisCompletionTicket beginShutdown() override;

private:
    quint64 m_documentId = 0;
    quint64 m_backendServiceId = 0;
    quint64 m_deviceGeneration = 0;
    quint64 m_sessionGeneration = 0;
    mutable QMutex m_mutex;
    KisAccelerationSessionState m_state = KisAccelerationSessionState::Initializing;
};

#endif // KIS_VULKAN_DOCUMENT_GPU_SESSION_H

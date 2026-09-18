/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanDocumentGpuSession.h"

#include <QMutexLocker>

KisVulkanDocumentGpuSession::KisVulkanDocumentGpuSession(quint64 documentId,
                                                         quint64 backendServiceId,
                                                         quint64 deviceGeneration,
                                                         quint64 sessionGeneration)
    : m_documentId(documentId)
    , m_backendServiceId(backendServiceId)
    , m_deviceGeneration(deviceGeneration)
    , m_sessionGeneration(sessionGeneration)
{
}

KisVulkanDocumentGpuSession::~KisVulkanDocumentGpuSession() = default;

quint64 KisVulkanDocumentGpuSession::documentId() const
{
    return m_documentId;
}

quint64 KisVulkanDocumentGpuSession::backendServiceId() const
{
    return m_backendServiceId;
}

quint64 KisVulkanDocumentGpuSession::deviceGeneration() const
{
    return m_deviceGeneration;
}

quint64 KisVulkanDocumentGpuSession::sessionGeneration() const
{
    return m_sessionGeneration;
}

KisAccelerationSessionState KisVulkanDocumentGpuSession::state() const
{
    QMutexLocker locker(&m_mutex);
    return m_state;
}

KisAccelerationCapabilities KisVulkanDocumentGpuSession::capabilities() const
{
    KisAccelerationCapabilities capabilities;
    capabilities.serviceId = m_backendServiceId;
    capabilities.deviceGeneration = m_deviceGeneration;
    capabilities.sessionGeneration = m_sessionGeneration;
    // Feature mask stays empty until PageStore/provider/executor contracts are
    // attached and verified for this document.
    return capabilities;
}

bool KisVulkanDocumentGpuSession::acceptsNewWork() const
{
    return false;
}

QString KisVulkanDocumentGpuSession::failureReason() const
{
    QMutexLocker locker(&m_mutex);
    return m_state == KisAccelerationSessionState::Initializing
        ? QStringLiteral("Document session awaits PageStore and replica-provider attachment")
        : QString();
}

KisCompletionTicket KisVulkanDocumentGpuSession::beginShutdown()
{
    QMutexLocker locker(&m_mutex);
    if (m_state == KisAccelerationSessionState::Shutdown) {
        return {};
    }
    m_state = KisAccelerationSessionState::Shutdown;
    // No document work can be accepted by this foundation session, therefore
    // no drain ticket is fabricated.
    return {};
}

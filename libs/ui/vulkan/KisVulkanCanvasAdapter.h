/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_CANVAS_ADAPTER_H
#define KIS_VULKAN_CANVAS_ADAPTER_H

#include <QByteArray>
#include <QSharedPointer>

#include <utility>

#include "KisCompositeTypes.h"
#include "KisDocumentGpuSession.h"
#include "KisVulkanWsiService.h"
#include "kritavulkanuiadapter_export.h"

class KisVulkanProductGate;

class KRITAVULKANUIADAPTER_EXPORT KisVulkanProductGateToken
{
public:
    KisVulkanProductGateToken() = default;
    bool isValid() const { return m_gateGeneration != 0 && !m_evidenceDigest.isEmpty(); }

private:
    KisVulkanProductGateToken(quint64 gateGeneration, QByteArray evidenceDigest)
        : m_gateGeneration(gateGeneration)
        , m_evidenceDigest(std::move(evidenceDigest))
    {
    }

    quint64 m_gateGeneration = 0;
    QByteArray m_evidenceDigest;

    friend class KisVulkanProductGate;
};

struct KRITAVULKANUIADAPTER_EXPORT KisVulkanCanvasActivationEvidence
{
    KisBackendReadyToken readyToken;
    QSharedPointer<KisDocumentGpuSession> session;
    KisVulkanSwapchainSnapshot swapchain;
    KisDisplaySurfaceSnapshot display;
    bool requestedFormatSupported = false;
    bool fallbackCanvasAvailable = false;
    KisVulkanProductGateToken productGate;
};

struct KRITAVULKANUIADAPTER_EXPORT KisVulkanCanvasActivationDecision
{
    bool allowed = false;
    QString reason;
};

/** Product activation gate. It has no side effects and cannot be bypassed. */
class KRITAVULKANUIADAPTER_EXPORT KisVulkanCanvasAdapter
{
public:
    KisVulkanCanvasAdapter();
    ~KisVulkanCanvasAdapter();

    KisVulkanCanvasActivationDecision evaluate(
        const KisVulkanCanvasActivationEvidence &evidence) const;
    bool canReplaceCanvas(const KisVulkanCanvasActivationEvidence &evidence) const;
};

#endif // KIS_VULKAN_CANVAS_ADAPTER_H

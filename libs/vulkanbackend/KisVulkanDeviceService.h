/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_DEVICE_SERVICE_H
#define KIS_VULKAN_DEVICE_SERVICE_H

#include <QScopedPointer>

#include "KisBackendServiceRegistry.h"
#include "KisVulkanFoundationTypes.h"
#include "kritavulkanbackend_export.h"

/**
 * Device-generation state and readiness evidence. Native Vulkan ownership is
 * intentionally hidden in Private; callers only receive immutable snapshots.
 */
class KRITAVULKANBACKEND_EXPORT KisVulkanDeviceService
{
public:
    KisVulkanDeviceService();
    ~KisVulkanDeviceService();

    KisVulkanFoundationState state() const;
    KisVulkanDeviceCapabilitySnapshot capabilities() const;
    KisVulkanFailure failure() const;

    bool beginProbe();
    bool acceptProbeResult(const KisVulkanDeviceCapabilitySnapshot &snapshot);
    bool acceptFoundationEvidence(const KisVulkanFoundationEvidence &evidence);

    KisBackendValidationReport validationReport() const;
    bool beginDraining();
    bool markDeviceLost(const QString &reason);
    bool finishShutdown();

private:
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_VULKAN_DEVICE_SERVICE_H

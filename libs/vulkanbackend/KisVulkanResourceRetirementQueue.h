/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_RESOURCE_RETIREMENT_QUEUE_H
#define KIS_VULKAN_RESOURCE_RETIREMENT_QUEUE_H

#include <QByteArray>
#include <QScopedPointer>
#include <QString>
#include <QVector>

#include "KisPageStoreTypes.h"

enum class KisVulkanResourceKind : quint8 {
    CommandBuffer,
    Descriptor,
    Buffer,
    Image,
    ImageView,
    Pipeline,
    BindingTable,
    Swapchain,
    NativeSurface
};

struct KisVulkanRetiredResource
{
    quint64 resourceId = 0;
    quint64 deviceGeneration = 0;
    KisVulkanResourceKind kind = KisVulkanResourceKind::Buffer;
    KisCompletionTicket lastUse;
    QString debugName;

    bool isValid() const
    {
        return resourceId != 0 && deviceGeneration != 0 &&
               lastUse.isValid() && !debugName.isEmpty();
    }
};

class KisVulkanResourceRetirementQueue
{
public:
    KisVulkanResourceRetirementQueue();
    ~KisVulkanResourceRetirementQueue();

    bool enqueue(const KisVulkanRetiredResource &resource);
    QVector<KisVulkanRetiredResource> collectCompleted(
        const KisCompletionTicket &completedThrough);
    int pendingCount() const;
    bool contains(quint64 deviceGeneration, quint64 resourceId) const;

private:
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_VULKAN_RESOURCE_RETIREMENT_QUEUE_H

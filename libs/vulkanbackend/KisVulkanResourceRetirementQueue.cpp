/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanResourceRetirementQueue.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>

class KisVulkanResourceRetirementQueue::Private
{
public:
    mutable QMutex mutex;
    QHash<QByteArray, KisVulkanRetiredResource> pending;
};

namespace {

QByteArray retirementKey(quint64 deviceGeneration, quint64 resourceId)
{
    return QByteArray::number(deviceGeneration) + ':' + QByteArray::number(resourceId);
}

}

KisVulkanResourceRetirementQueue::KisVulkanResourceRetirementQueue()
    : d(new Private)
{
}

KisVulkanResourceRetirementQueue::~KisVulkanResourceRetirementQueue() = default;

bool KisVulkanResourceRetirementQueue::enqueue(const KisVulkanRetiredResource &resource)
{
    if (!resource.isValid()) {
        return false;
    }
    QMutexLocker locker(&d->mutex);
    const QByteArray key = retirementKey(resource.deviceGeneration, resource.resourceId);
    if (d->pending.contains(key)) {
        return false;
    }
    d->pending.insert(key, resource);
    return true;
}

QVector<KisVulkanRetiredResource> KisVulkanResourceRetirementQueue::collectCompleted(
    const KisCompletionTicket &completedThrough)
{
    QVector<KisVulkanRetiredResource> completed;
    if (!completedThrough.isValid()) {
        return completed;
    }

    QMutexLocker locker(&d->mutex);
    for (auto it = d->pending.begin(); it != d->pending.end();) {
        const KisCompletionTicket lastUse = it->lastUse;
        if (lastUse.source() == completedThrough.source() &&
            lastUse.value() <= completedThrough.value()) {
            completed.append(it.value());
            it = d->pending.erase(it);
        } else {
            ++it;
        }
    }
    return completed;
}

int KisVulkanResourceRetirementQueue::pendingCount() const
{
    QMutexLocker locker(&d->mutex);
    return d->pending.size();
}

bool KisVulkanResourceRetirementQueue::contains(quint64 deviceGeneration,
                                                quint64 resourceId) const
{
    QMutexLocker locker(&d->mutex);
    return deviceGeneration != 0 && resourceId != 0 &&
           d->pending.contains(retirementKey(deviceGeneration, resourceId));
}

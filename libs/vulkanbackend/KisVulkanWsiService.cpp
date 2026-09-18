/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanWsiService.h"

#include "KisVulkanSubmissionCoordinator.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>

struct WsiViewRecord
{
    KisVulkanViewIntent intent;
    KisVulkanSwapchainSnapshot snapshot;
};

class KisVulkanWsiService::Private
{
public:
    mutable QMutex mutex;
    quint64 serviceGeneration = 0;
    quint64 deviceGeneration = 0;
    quint64 nextView = 1;
    QSharedPointer<KisVulkanSubmissionCoordinator> coordinator;
    QHash<quint64, WsiViewRecord> views;
};

KisVulkanWsiService::KisVulkanWsiService()
    : d(new Private)
{
}

KisVulkanWsiService::~KisVulkanWsiService() = default;

bool KisVulkanWsiService::configure(quint64 serviceGeneration,
                                    quint64 deviceGeneration,
                                    const QSharedPointer<KisVulkanSubmissionCoordinator> &coordinator)
{
    if (serviceGeneration == 0 || deviceGeneration == 0 || !coordinator) {
        return false;
    }
    QMutexLocker locker(&d->mutex);
    if (d->serviceGeneration != 0 || !d->views.isEmpty()) {
        return false;
    }
    d->serviceGeneration = serviceGeneration;
    d->deviceGeneration = deviceGeneration;
    d->coordinator = coordinator;
    return true;
}

KisVulkanViewHandle KisVulkanWsiService::registerViewIntent(
    const KisVulkanViewIntent &intent,
    QString *error)
{
    const QString intentError = intent.validationError();
    QMutexLocker locker(&d->mutex);
    if (d->serviceGeneration == 0 || !d->coordinator) {
        if (error) *error = QStringLiteral("WSI service is not configured");
        return {};
    }
    if (!intentError.isEmpty()) {
        if (error) *error = intentError;
        return {};
    }

    KisVulkanViewHandle handle{d->serviceGeneration, d->nextView++};
    WsiViewRecord record;
    record.intent = intent;
    record.snapshot.m_view = handle;
    record.snapshot.m_deviceGeneration = d->deviceGeneration;
    record.snapshot.m_surfaceGeneration = intent.surface.lifetimeGeneration;
    record.snapshot.m_pixelExtent = intent.surface.pixelExtent;
    record.snapshot.m_outputColorSpace = intent.outputColorSpace;
    record.snapshot.m_state = KisVulkanWsiSessionState::AwaitingSwapchain;
    record.snapshot.m_failureReason = QStringLiteral(
        "Native surface and swapchain creation are not implemented before BR6");
    d->views.insert(handle.viewId, record);
    return handle;
}

bool KisVulkanWsiService::updateViewIntent(const KisVulkanViewHandle &view,
                                           const KisVulkanViewIntent &intent,
                                           QString *error)
{
    const QString intentError = intent.validationError();
    QMutexLocker locker(&d->mutex);
    if (!view.isValid() || view.serviceGeneration != d->serviceGeneration) {
        if (error) *error = QStringLiteral("View handle is invalid or stale");
        return false;
    }
    if (!intentError.isEmpty()) {
        if (error) *error = intentError;
        return false;
    }
    auto it = d->views.find(view.viewId);
    if (it == d->views.end()) {
        if (error) *error = QStringLiteral("View handle is unknown");
        return false;
    }
    if (intent.documentId != it->intent.documentId ||
        intent.documentSessionGeneration != it->intent.documentSessionGeneration ||
        intent.viewGeneration <= it->intent.viewGeneration ||
        intent.surface.lifetimeGeneration < it->intent.surface.lifetimeGeneration) {
        if (error) *error = QStringLiteral("View update changes document ownership or is not monotonic");
        return false;
    }

    it->intent = intent;
    it->snapshot.m_surfaceGeneration = intent.surface.lifetimeGeneration;
    it->snapshot.m_pixelExtent = intent.surface.pixelExtent;
    it->snapshot.m_outputColorSpace = intent.outputColorSpace;
    it->snapshot.m_swapchainGeneration = 0;
    it->snapshot.m_surfaceFormat.clear();
    it->snapshot.m_state = KisVulkanWsiSessionState::AwaitingSwapchain;
    it->snapshot.m_failureReason = QStringLiteral(
        "Updated view intent awaits a BR6 swapchain generation");
    return true;
}

KisVulkanSwapchainSnapshot KisVulkanWsiService::snapshot(
    const KisVulkanViewHandle &view) const
{
    QMutexLocker locker(&d->mutex);
    if (!view.isValid() || view.serviceGeneration != d->serviceGeneration) {
        KisVulkanSwapchainSnapshot invalid;
        invalid.m_failureReason = QStringLiteral("View handle is invalid or stale");
        invalid.m_state = KisVulkanWsiSessionState::Failed;
        return invalid;
    }
    const auto it = d->views.constFind(view.viewId);
    if (it == d->views.constEnd()) {
        KisVulkanSwapchainSnapshot invalid;
        invalid.m_failureReason = QStringLiteral("View handle is unknown");
        invalid.m_state = KisVulkanWsiSessionState::Failed;
        return invalid;
    }
    return it->snapshot;
}

KisVulkanViewRetirement KisVulkanWsiService::retireView(
    const KisVulkanViewHandle &view)
{
    KisVulkanViewRetirement retirement;
    QMutexLocker locker(&d->mutex);
    if (!view.isValid() || view.serviceGeneration != d->serviceGeneration) {
        retirement.error = QStringLiteral("View handle is invalid or stale");
        return retirement;
    }
    auto it = d->views.find(view.viewId);
    if (it == d->views.end()) {
        retirement.error = QStringLiteral("View handle is unknown");
        return retirement;
    }

    // Foundation records contain no native or in-flight WSI resources yet, so
    // removal is immediate and returns no fabricated completion ticket.
    d->views.erase(it);
    retirement.accepted = true;
    retirement.completedImmediately = true;
    return retirement;
}

int KisVulkanWsiService::viewCount() const
{
    QMutexLocker locker(&d->mutex);
    return d->views.size();
}

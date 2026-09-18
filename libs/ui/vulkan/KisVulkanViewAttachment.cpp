/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanViewAttachment.h"

#include <QMutex>
#include <QMutexLocker>

class KisVulkanViewAttachment::Private
{
public:
    mutable QMutex mutex;
    QSharedPointer<KisVulkanWsiService> wsi;
    QString failureReason;
};

KisVulkanViewAttachment::KisVulkanViewAttachment()
    : d(new Private)
{
}

KisVulkanViewAttachment::~KisVulkanViewAttachment() = default;

bool KisVulkanViewAttachment::configure(
    const QSharedPointer<KisVulkanWsiService> &wsiService)
{
    if (!wsiService) return false;
    QMutexLocker locker(&d->mutex);
    if (d->wsi) return false;
    d->wsi = wsiService;
    return true;
}

bool KisVulkanViewAttachment::isOperational() const
{
    QMutexLocker locker(&d->mutex);
    return bool(d->wsi);
}

KisVulkanViewId KisVulkanViewAttachment::attach(
    const KisVulkanViewIntent &intent,
    const KisBackendReadyToken &readyToken,
    const QSharedPointer<KisDocumentGpuSession> &session)
{
    QMutexLocker locker(&d->mutex);
    d->failureReason.clear();
    if (!d->wsi) {
        d->failureReason = QStringLiteral("View attachment has no backend WSI service");
        return {};
    }
    if (!readyToken.isValid()) {
        d->failureReason = QStringLiteral("View attachment has no valid backend-ready token");
        return {};
    }
    if (!session || session->state() != KisAccelerationSessionState::Ready ||
        !session->acceptsNewWork()) {
        d->failureReason = QStringLiteral("Document acceleration session is not ready");
        return {};
    }
    if (!intent.isValid() || session->documentId() != intent.documentId) {
        d->failureReason = QStringLiteral("View intent does not match the document session");
        return {};
    }
    if (session->backendServiceId() != readyToken.serviceId() ||
        session->deviceGeneration() != readyToken.deviceGeneration() ||
        session->sessionGeneration() != intent.documentSessionGeneration) {
        d->failureReason = QStringLiteral("View intent, session, and backend generations do not match");
        return {};
    }

    QString error;
    const KisVulkanViewHandle backendView = d->wsi->registerViewIntent(intent, &error);
    if (!backendView.isValid()) {
        d->failureReason = error;
        return {};
    }
    return {backendView};
}

bool KisVulkanViewAttachment::update(KisVulkanViewId viewId,
                                     const KisVulkanViewIntent &intent)
{
    QMutexLocker locker(&d->mutex);
    d->failureReason.clear();
    if (!d->wsi || !viewId.isValid()) {
        d->failureReason = QStringLiteral("View attachment or view id is invalid");
        return false;
    }
    if (!d->wsi->updateViewIntent(viewId.backendView, intent, &d->failureReason)) {
        return false;
    }
    return true;
}

KisVulkanViewRetirement KisVulkanViewAttachment::detach(KisVulkanViewId viewId)
{
    QMutexLocker locker(&d->mutex);
    d->failureReason.clear();
    if (!d->wsi || !viewId.isValid()) {
        d->failureReason = QStringLiteral("View attachment or view id is invalid");
        KisVulkanViewRetirement retirement;
        retirement.error = d->failureReason;
        return retirement;
    }
    KisVulkanViewRetirement retirement = d->wsi->retireView(viewId.backendView);
    d->failureReason = retirement.error;
    return retirement;
}

QString KisVulkanViewAttachment::failureReason() const
{
    QMutexLocker locker(&d->mutex);
    return d->failureReason;
}

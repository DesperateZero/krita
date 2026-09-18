/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanSubmissionCoordinator.h"

#include <QMutex>
#include <QMutexLocker>

QString KisVulkanSubmissionRequest::validationError() const
{
    if (requestId == 0 || deviceGeneration == 0 || debugLabel.isEmpty()) {
        return QStringLiteral("Submission request identity, device generation, or label is missing");
    }
    if (!foundationOperation && sessionGeneration == 0) {
        return QStringLiteral("Document submission has no session generation");
    }
    if (commands.isEmpty() && !sparseBind && !hasPresent) {
        return QStringLiteral("Submission request contains no executable work");
    }
    for (const KisVulkanCommandPacket &command : commands) {
        if (!command.isValid()) {
            return QStringLiteral("Submission request contains an invalid command packet");
        }
    }
    for (const KisCompletionTicket &dependency : dependencies) {
        if (!dependency.isValid()) {
            return QStringLiteral("Submission request contains an invalid dependency ticket");
        }
    }
    for (const KisVulkanOwnershipTransfer &transfer : ownershipTransfers) {
        if (!transfer.isValid()) {
            return QStringLiteral("Submission request contains an invalid ownership transfer");
        }
    }
    if (hasPresent && (!present.isValid() || queueRole != KisVulkanQueueRole::Present)) {
        return QStringLiteral("Present packet is invalid or assigned to a non-present queue");
    }
    if (queueRole == KisVulkanQueueRole::Present && !hasPresent) {
        return QStringLiteral("Present queue request has no present packet");
    }
    if (sparseBind && queueRole != KisVulkanQueueRole::Sparse) {
        return QStringLiteral("Sparse bind request is assigned to a non-sparse queue");
    }
    if (failureContinuationId == 0) {
        return QStringLiteral("Submission request has no failure/cancel continuation");
    }
    return {};
}

class KisVulkanSubmissionCoordinator::Private
{
public:
    mutable QMutex mutex;
    KisVulkanCoordinatorState state = KisVulkanCoordinatorState::Unconfigured;
    KisVulkanCoordinatorConfig config;
    QSharedPointer<KisCompletionRegistry> completions;
    quint64 completionSource = 0;
    QString lastFailure;
};

KisVulkanSubmissionCoordinator::KisVulkanSubmissionCoordinator()
    : d(new Private)
{
}

KisVulkanSubmissionCoordinator::~KisVulkanSubmissionCoordinator() = default;

bool KisVulkanSubmissionCoordinator::configure(
    const KisVulkanCoordinatorConfig &config,
    const QSharedPointer<KisCompletionRegistry> &completionRegistry,
    QString *error)
{
    QMutexLocker locker(&d->mutex);
    if (d->state != KisVulkanCoordinatorState::Unconfigured) {
        if (error) *error = QStringLiteral("Submission coordinator is already configured");
        return false;
    }
    if (!config.isValid()) {
        if (error) *error = QStringLiteral("Submission coordinator configuration is invalid");
        return false;
    }
    if (!completionRegistry || !completionRegistry->isOperational()) {
        if (error) *error = QStringLiteral("Submission coordinator has no completion registry");
        return false;
    }

    KisCompletionSourceDescriptor source;
    source.name = QStringLiteral("Vulkan device generation %1 timeline")
                      .arg(config.deviceGeneration);
    source.gpuTimeline = true;
    const quint64 sourceId = completionRegistry->registerSource(source);
    if (sourceId == 0) {
        if (error) *error = QStringLiteral("Could not register Vulkan timeline completion source");
        return false;
    }

    d->config = config;
    d->completions = completionRegistry;
    d->completionSource = sourceId;
    d->state = KisVulkanCoordinatorState::Configured;
    return true;
}

KisVulkanCoordinatorState KisVulkanSubmissionCoordinator::state() const
{
    QMutexLocker locker(&d->mutex);
    return d->state;
}

bool KisVulkanSubmissionCoordinator::isOperational() const
{
    QMutexLocker locker(&d->mutex);
    return d->state == KisVulkanCoordinatorState::Operational;
}

QString KisVulkanSubmissionCoordinator::validate(
    const KisVulkanSubmissionRequest &request) const
{
    QMutexLocker locker(&d->mutex);
    if (d->state == KisVulkanCoordinatorState::Unconfigured ||
        d->state == KisVulkanCoordinatorState::Failed ||
        d->state == KisVulkanCoordinatorState::Shutdown) {
        return QStringLiteral("Submission coordinator is not configured for validation");
    }
    const QString requestError = request.validationError();
    if (!requestError.isEmpty()) {
        return requestError;
    }
    if (request.deviceGeneration != d->config.deviceGeneration) {
        return QStringLiteral("Submission request targets a stale device generation");
    }
    if (request.queueRole == KisVulkanQueueRole::Sparse &&
        d->config.queues.sparseFamily < 0) {
        return QStringLiteral("Selected device has no sparse queue family");
    }
    if (request.queueRole == KisVulkanQueueRole::Present &&
        d->config.queues.presentFamily < 0) {
        return QStringLiteral("Selected device has no present queue family");
    }
    for (const KisCompletionTicket &dependency : request.dependencies) {
        if (!d->completions->isKnownSource(dependency.source())) {
            return QStringLiteral("Submission dependency comes from an unknown completion source");
        }
    }
    return {};
}

QString KisVulkanSubmissionCoordinator::lastFailure() const
{
    QMutexLocker locker(&d->mutex);
    return d->lastFailure;
}

KisCompletionTicket KisVulkanSubmissionCoordinator::submit(
    const KisVulkanSubmissionRequest &request)
{
    const QString validationError = validate(request);
    QMutexLocker locker(&d->mutex);
    if (!validationError.isEmpty()) {
        d->lastFailure = validationError;
        return {};
    }
    if (d->state != KisVulkanCoordinatorState::Operational) {
        d->lastFailure = QStringLiteral(
            "Submission packet is valid, but native Vulkan queue execution is not installed");
        return {};
    }

    // BR2 installs the only native queue executor here. Until then, do not
    // allocate a ticket that could remain pending forever or imply submission.
    d->lastFailure = QStringLiteral("Native Vulkan submission is not implemented");
    return {};
}

bool KisVulkanSubmissionCoordinator::isComplete(const KisCompletionTicket &ticket) const
{
    QMutexLocker locker(&d->mutex);
    if (!ticket.isValid() || !d->completions) {
        return false;
    }
    const KisCompletionStatus status = d->completions->status(ticket);
    return status == KisCompletionStatus::Succeeded ||
           status == KisCompletionStatus::Failed ||
           status == KisCompletionStatus::Cancelled;
}

void KisVulkanSubmissionCoordinator::collectCompletedResources()
{
    // Resource-specific retirement queues consume cached completed timeline
    // values in BR2. This method deliberately performs no wait.
}

KisCompletionTicket KisVulkanSubmissionCoordinator::beginShutdown()
{
    QMutexLocker locker(&d->mutex);
    if (d->state != KisVulkanCoordinatorState::Configured &&
        d->state != KisVulkanCoordinatorState::Operational) {
        return {};
    }
    d->state = KisVulkanCoordinatorState::Draining;
    // No native submissions can exist in the foundation-only implementation.
    d->state = KisVulkanCoordinatorState::Shutdown;
    return {};
}

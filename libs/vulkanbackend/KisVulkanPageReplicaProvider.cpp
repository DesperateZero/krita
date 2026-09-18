/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanPageReplicaProvider.h"

#include <QMutex>
#include <QMutexLocker>

namespace {

KisReplicaOperation failedReplicaOperation(const QString &reason)
{
    KisReplicaOperation operation;
    operation.status = KisPageRequestStatus::Failed;
    operation.error = reason;
    return operation;
}

}

class KisVulkanPageReplicaProvider::Private
{
public:
    mutable QMutex mutex;
    KisVulkanReplicaProviderConfig config;
    QSharedPointer<KisVulkanSubmissionCoordinator> coordinator;
    QSharedPointer<KisVulkanMemoryBudgetLedger> memoryLedger;
    QSharedPointer<KisVulkanPageBindingTable> bindingTable;
};

KisVulkanPageReplicaProvider::KisVulkanPageReplicaProvider()
    : d(new Private)
{
}

KisVulkanPageReplicaProvider::~KisVulkanPageReplicaProvider() = default;

bool KisVulkanPageReplicaProvider::configure(
    const KisVulkanReplicaProviderConfig &config,
    const QSharedPointer<KisVulkanSubmissionCoordinator> &coordinator,
    const QSharedPointer<KisVulkanMemoryBudgetLedger> &memoryLedger,
    const QSharedPointer<KisVulkanPageBindingTable> &bindingTable,
    QString *error)
{
    QMutexLocker locker(&d->mutex);
    if (d->config.isValid()) {
        if (error) *error = QStringLiteral("Replica provider is already configured");
        return false;
    }
    if (!config.isValid() || !coordinator || !memoryLedger || !bindingTable) {
        if (error) *error = QStringLiteral("Replica provider configuration is incomplete");
        return false;
    }
    if (!memoryLedger->isConfigured() || !bindingTable->isOperational()) {
        if (error) *error = QStringLiteral("Replica provider dependencies are not configured");
        return false;
    }
    d->config = config;
    d->coordinator = coordinator;
    d->memoryLedger = memoryLedger;
    d->bindingTable = bindingTable;
    return true;
}

bool KisVulkanPageReplicaProvider::isConfigured() const
{
    QMutexLocker locker(&d->mutex);
    return d->config.isValid();
}

quint64 KisVulkanPageReplicaProvider::providerGeneration() const
{
    QMutexLocker locker(&d->mutex);
    return d->config.providerGeneration;
}

QString KisVulkanPageReplicaProvider::name() const
{
    QMutexLocker locker(&d->mutex);
    return d->config.isValid()
        ? QStringLiteral("Krita Vulkan page replica provider")
        : QStringLiteral("Krita Vulkan page replica provider (unconfigured)");
}

KisReplicaCapabilities KisVulkanPageReplicaProvider::capabilities() const
{
    QMutexLocker locker(&d->mutex);
    return d->config.capabilities;
}

KisReplicaOperation KisVulkanPageReplicaProvider::requestReplica(
    const KisPageVersion &version,
    KisPageAccessDomain domain,
    KisPageAccessMode mode,
    KisPagePriority priority)
{
    Q_UNUSED(mode);
    Q_UNUSED(priority);
    QMutexLocker locker(&d->mutex);
    if (!d->config.isValid()) {
        return failedReplicaOperation(QStringLiteral("Vulkan replica provider is not configured"));
    }
    if (!version.isValid() || !d->config.capabilities.domains.contains(domain)) {
        return failedReplicaOperation(QStringLiteral("Replica request version or access domain is unsupported"));
    }
    return failedReplicaOperation(
        QStringLiteral("Native Vulkan page allocation/materialization is not implemented before BR3"));
}

KisReplicaOperation KisVulkanPageReplicaProvider::prepareWrite(
    const KisPageKey &key,
    KisPageGeneration generation,
    KisPageAccessDomain domain,
    KisPagePriority priority)
{
    Q_UNUSED(priority);
    QMutexLocker locker(&d->mutex);
    if (!d->config.isValid()) {
        return failedReplicaOperation(QStringLiteral("Vulkan replica provider is not configured"));
    }
    if (!key.isValid() || !generation.isValid() ||
        !d->config.capabilities.domains.contains(domain)) {
        return failedReplicaOperation(QStringLiteral("Write reservation identity or domain is invalid"));
    }
    return failedReplicaOperation(
        QStringLiteral("Native Vulkan write allocation is not implemented before BR3"));
}

KisReplicaOperation KisVulkanPageReplicaProvider::transfer(
    const KisReplicaHandle &source,
    const KisReplicaHandle &target,
    KisPagePriority priority)
{
    Q_UNUSED(priority);
    QMutexLocker locker(&d->mutex);
    if (!d->config.isValid()) {
        return failedReplicaOperation(QStringLiteral("Vulkan replica provider is not configured"));
    }
    if (!source.isValid() || !target.isValid() || !(source.version == target.version)) {
        return failedReplicaOperation(QStringLiteral("Replica transfer endpoints do not describe one page generation"));
    }
    return failedReplicaOperation(
        QStringLiteral("Native Vulkan replica transfer is not implemented before BR3"));
}

bool KisVulkanPageReplicaProvider::validate(
    const KisReplicaHandle &replica,
    KisPageGeneration expectedGeneration) const
{
    QMutexLocker locker(&d->mutex);
    return d->config.isValid() && replica.isValid() && expectedGeneration.isValid() &&
           replica.provider == d->config.providerId &&
           replica.version.generation == expectedGeneration &&
           d->config.capabilities.domains.contains(replica.domain);
}

void KisVulkanPageReplicaProvider::retire(const KisReplicaHandle &replica,
                                          const KisCompletionTicket &lastUse)
{
    Q_UNUSED(replica);
    Q_UNUSED(lastUse);
    // BR3 connects replica allocations to the shared resource-retirement queue.
}

KisReplicaMemoryUsage KisVulkanPageReplicaProvider::memoryUsage() const
{
    QMutexLocker locker(&d->mutex);
    KisReplicaMemoryUsage usage;
    if (!d->memoryLedger) return usage;
    const KisVulkanMemoryUsageSnapshot snapshot = d->memoryLedger->usage();
    usage.residentBytes = snapshot.reservedBytes;
    usage.committedBytes = snapshot.reservedBytes;
    usage.budgetBytes = snapshot.hardLimitBytes;
    return usage;
}

/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanPageReplicaProvider.h"

#include <QMutex>
#include <QMutexLocker>

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
    if (!memoryLedger->isConfigured() ||
        !bindingTable->matchesProvider(config.providerId,
                                       config.providerGeneration)) {
        if (error) *error = QStringLiteral("Replica provider dependencies are not configured");
        return false;
    }
    d->config = config;
    d->coordinator = coordinator;
    d->memoryLedger = memoryLedger;
    d->bindingTable = bindingTable;
    return true;
}

QString KisVulkanPageReplicaProvider::name() const
{
    QMutexLocker locker(&d->mutex);
    return d->config.isValid()
        ? QStringLiteral("Krita Vulkan page replica provider")
        : QStringLiteral("Krita Vulkan page replica provider (unconfigured)");
}

KisReplicaProviderId KisVulkanPageReplicaProvider::providerId() const
{
    QMutexLocker locker(&d->mutex);
    return KisReplicaProviderId{d->config.providerId};
}

KisReplicaProviderEpoch KisVulkanPageReplicaProvider::providerEpoch() const
{
    QMutexLocker locker(&d->mutex);
    return KisReplicaProviderEpoch{d->config.providerGeneration};
}

KisReplicaCapabilities KisVulkanPageReplicaProvider::capabilities() const
{
    QMutexLocker locker(&d->mutex);
    KisReplicaCapabilities result = d->config.capabilities;
    // Submission completion is asynchronous even if a caller accidentally
    // sets the generic capability bit in its configuration.
    result.synchronousOperations = false;
    return result;
}

KisReplicaOperation KisVulkanPageReplicaProvider::requestReplica(
    KisPageOperationId operation,
    const KisPageVersion &version,
    const KisPageAllocationDescriptor &descriptor,
    KisPageAccessDomain domain,
    KisPageAccessMode mode,
    KisPagePriority priority)
{
    Q_UNUSED(mode);
    Q_UNUSED(priority);
    QMutexLocker locker(&d->mutex);
    if (!d->config.isValid()) {
        return KisReplicaOperation::failed(operation,
                                      QStringLiteral("Vulkan replica provider is not configured"));
    }
    if (!operation.isValid() || !version.isValid() || !descriptor.isValid() ||
        !d->config.capabilities.domains.contains(domain)) {
        return KisReplicaOperation::failed(
            operation,
            QStringLiteral("Replica request version or access domain is unsupported"));
    }
    return KisReplicaOperation::failed(operation,
        QStringLiteral("Native Vulkan page allocation/materialization is not implemented before BR3"));
}

KisReplicaOperation KisVulkanPageReplicaProvider::prepareWrite(
    KisPageOperationId operation,
    const KisPageVersion &version,
    const KisPageAllocationDescriptor &descriptor,
    KisPageAccessDomain domain,
    KisPageWriteMode mode,
    KisPagePriority priority)
{
    Q_UNUSED(mode);
    Q_UNUSED(priority);
    QMutexLocker locker(&d->mutex);
    if (!d->config.isValid()) {
        return KisReplicaOperation::failed(operation,
                                      QStringLiteral("Vulkan replica provider is not configured"));
    }
    if (!operation.isValid() || !version.isValid() || !descriptor.isValid() ||
        !d->config.capabilities.domains.contains(domain)) {
        return KisReplicaOperation::failed(
            operation,
            QStringLiteral("Write reservation identity or domain is invalid"));
    }
    return KisReplicaOperation::failed(operation,
        QStringLiteral("Native Vulkan write allocation is not implemented before BR3"));
}

KisReplicaOperation KisVulkanPageReplicaProvider::transfer(
    const KisReplicaTransferRequest &request,
    KisPagePriority priority)
{
    Q_UNUSED(priority);
    QMutexLocker locker(&d->mutex);
    if (!d->config.isValid()) {
        return KisReplicaOperation::failed(request.operation,
                                      QStringLiteral("Vulkan replica provider is not configured"));
    }
    if (!request.isSameProviderTransfer() ||
        request.target.provider.value != d->config.providerId ||
        request.target.providerEpoch.value != d->config.providerGeneration ||
        !d->config.capabilities.domains.contains(request.target.domain)) {
        return KisReplicaOperation::failed(
            request.operation,
            QStringLiteral("Replica transfer target or endpoints do not match this provider"));
    }
    return KisReplicaOperation::failed(request.operation,
        QStringLiteral("Native Vulkan replica transfer is not implemented before BR3"));
}

KisReplicaAccess KisVulkanPageReplicaProvider::resolveAccess(
    KisPageLeaseId lease,
    KisPageOperationId operation,
    const KisReplicaHandle &replica,
    KisPageAccessRequirement requirement,
    KisPageAccessMode mode)
{
    Q_UNUSED(lease);
    Q_UNUSED(operation);
    Q_UNUSED(replica);
    Q_UNUSED(requirement);
    Q_UNUSED(mode);
    // BR3 resolves an operation-scoped binding only after PageStore has
    // validated the exact replica and acquired the corresponding lease.
    return {};
}

void KisVulkanPageReplicaProvider::releaseAccess(
    KisReplicaAccess access,
    const KisCompletionTicket &lastUse)
{
    Q_UNUSED(access);
    Q_UNUSED(lastUse);
    // BR3 forwards GPU last use to the shared retirement path.
}

bool KisVulkanPageReplicaProvider::validate(
    const KisReplicaHandle &replica,
    const KisPageAllocationDescriptor &descriptor) const
{
    QMutexLocker locker(&d->mutex);
    return d->config.isValid() && replica.isValid() && descriptor.isValid() &&
           replica.provider.value == d->config.providerId &&
           replica.providerEpoch.value == d->config.providerGeneration &&
           replica.layout.matches(descriptor) &&
           d->config.capabilities.domains.contains(replica.domain);
}

KisReplicaOperation KisVulkanPageReplicaProvider::retire(
    KisPageOperationId operation,
    const KisReplicaHandle &replica,
    const KisCompletionTicket &lastUse)
{
    Q_UNUSED(lastUse);
    QMutexLocker locker(&d->mutex);
    if (!d->config.isValid()) {
        return KisReplicaOperation::failed(operation,
                                      QStringLiteral("Vulkan replica provider is not configured"));
    }
    if (!operation.isValid() || !replica.isValid() ||
        replica.provider.value != d->config.providerId ||
        replica.providerEpoch.value != d->config.providerGeneration ||
        !d->config.capabilities.domains.contains(replica.domain)) {
        return KisReplicaOperation::failed(operation,
                                      QStringLiteral("Replica retirement identity is invalid"));
    }
    // BR3 connects replica allocations to the shared resource-retirement queue
    // and returns a completion under the caller-supplied operation identity.
    return KisReplicaOperation::failed(
        operation,
        QStringLiteral("Native Vulkan replica retirement is not implemented before BR3"));
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

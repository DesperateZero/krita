/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_PAGE_REPLICA_PROVIDER_H
#define KIS_VULKAN_PAGE_REPLICA_PROVIDER_H

#include <QScopedPointer>
#include <QSharedPointer>

#include "KisPageReplicaProvider.h"
#include "KisVulkanMemory.h"
#include "KisVulkanPageBindingTable.h"
#include "KisVulkanSubmissionCoordinator.h"

struct KisVulkanReplicaProviderConfig
{
    quint64 providerId = 0;
    quint64 providerGeneration = 0;
    quint64 deviceGeneration = 0;
    KisReplicaCapabilities capabilities;

    bool isValid() const
    {
        return providerId != 0 && providerGeneration != 0 && deviceGeneration != 0 &&
               capabilities.isValid();
    }
};

/**
 * Vulkan UMA/dGPU replica provider contract. Configuration establishes domain,
 * budget, binding and coordinator ownership; materialization remains disabled
 * until a native allocator/transfer executor is attached in BR3.
 */
class KisVulkanPageReplicaProvider final : public KisPageReplicaProvider
{
public:
    KisVulkanPageReplicaProvider();
    ~KisVulkanPageReplicaProvider() override;

    bool configure(const KisVulkanReplicaProviderConfig &config,
                   const QSharedPointer<KisVulkanSubmissionCoordinator> &coordinator,
                   const QSharedPointer<KisVulkanMemoryBudgetLedger> &memoryLedger,
                   const QSharedPointer<KisVulkanPageBindingTable> &bindingTable,
                   QString *error = nullptr);

    QString name() const override;
    KisReplicaProviderId providerId() const override;
    KisReplicaProviderEpoch providerEpoch() const override;
    KisReplicaCapabilities capabilities() const override;
    KisReplicaOperation requestReplica(KisPageOperationId operation,
                                       const KisPageVersion &version,
                                       const KisPageAllocationDescriptor &descriptor,
                                       KisPageAccessDomain domain,
                                       KisPageAccessMode mode,
                                       KisPagePriority priority) override;
    KisReplicaOperation prepareWrite(KisPageOperationId operation,
                                     const KisPageVersion &version,
                                     const KisPageAllocationDescriptor &descriptor,
                                     KisPageAccessDomain domain,
                                     KisPageWriteMode mode,
                                     KisPagePriority priority) override;
    KisReplicaOperation transfer(const KisReplicaTransferRequest &request,
                                 KisPagePriority priority) override;
    KisReplicaAccess resolveAccess(KisPageLeaseId lease,
                                   KisPageOperationId operation,
                                   const KisReplicaHandle &replica,
                                   KisPageAccessRequirement requirement,
                                   KisPageAccessMode mode) override;
    void releaseAccess(KisReplicaAccess access,
                       const KisCompletionTicket &lastUse) override;
    bool validate(const KisReplicaHandle &replica,
                  const KisPageAllocationDescriptor &descriptor) const override;
    KisReplicaOperation retire(KisPageOperationId operation,
                               const KisReplicaHandle &replica,
                               const KisCompletionTicket &lastUse) override;
    KisReplicaMemoryUsage memoryUsage() const override;

private:
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_VULKAN_PAGE_REPLICA_PROVIDER_H

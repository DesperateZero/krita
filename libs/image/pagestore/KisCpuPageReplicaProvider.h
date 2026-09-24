/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_CPU_PAGE_REPLICA_PROVIDER_H
#define KIS_CPU_PAGE_REPLICA_PROVIDER_H

#include <QScopedPointer>
#include <QSharedPointer>

#include "KisCompletionRegistry.h"
#include "KisPageReplicaProvider.h"

/**
 * BR1 CPU RAM provider. It owns physical byte allocations only; canonical
 * generation, publication, leases and transaction visibility remain owned by
 * KisPageStore. Operations complete synchronously today but still use the
 * shared completion registry, so an asynchronous tiles3 adapter can preserve
 * the same contract.
 */
class KRITAIMAGE_EXPORT KisCpuPageReplicaProvider final
    : public KisPageReplicaProvider
{
public:
    KisCpuPageReplicaProvider();
    ~KisCpuPageReplicaProvider() override;

    bool configure(const KisCpuResidentReplicaProviderConfig &config,
                   const QSharedPointer<KisCompletionRegistry> &completions,
                   QString *error = nullptr);

    QString name() const override;
    KisReplicaProviderId providerId() const override;
    KisReplicaProviderEpoch providerEpoch() const override;
    KisReplicaCapabilities capabilities() const override;

    KisReplicaOperation requestReplica(
        KisPageOperationId operation,
        const KisPageVersion &version,
        const KisPageAllocationDescriptor &descriptor,
        KisPageAccessDomain domain,
        KisPageAccessMode mode,
        KisPagePriority priority) override;
    KisReplicaOperation prepareWrite(
        KisPageOperationId operation,
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
    QSharedPointer<KisCpuResidentBinding> cpuResidentBinding(
        const KisReplicaHandle &replica, KisCpuResidentReadStatus *status = nullptr) const override;

private:
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_CPU_PAGE_REPLICA_PROVIDER_H

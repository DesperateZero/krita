/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_REPLICA_PROVIDER_H
#define KIS_PAGE_REPLICA_PROVIDER_H

#include <QString>
#include <QVector>

#include "KisPageStoreTypes.h"

struct KRITAIMAGE_EXPORT KisReplicaCapabilities
{
    QVector<KisPageAccessDomain> domains;
    bool cpuPointerAccess = false;
    bool gpuAddressAccess = false;
    bool sharedCpuGpuBacking = false;
    bool durable = false;
};

struct KRITAIMAGE_EXPORT KisReplicaHandle
{
    quint64 provider = 0;
    quint64 allocation = 0;
    KisPageVersion version;
    KisPageAccessDomain domain = KisPageAccessDomain::Unknown;

    bool isValid() const
    {
        return provider != 0 && allocation != 0 && version.isValid();
    }
};

struct KRITAIMAGE_EXPORT KisReplicaOperation
{
    KisPageRequestStatus status = KisPageRequestStatus::Unsupported;
    KisReplicaHandle replica;
    KisCompletionTicket completion;
    QString error;
};

struct KRITAIMAGE_EXPORT KisReplicaMemoryUsage
{
    quint64 residentBytes = 0;
    quint64 committedBytes = 0;
    quint64 budgetBytes = 0;
};

/**
 * Backend-neutral physical replica interface. PageStore is the only intended
 * caller; consumers receive leases instead of provider-owned allocations.
 */
class KRITAIMAGE_EXPORT KisPageReplicaProvider
{
public:
    virtual ~KisPageReplicaProvider();

    virtual QString name() const = 0;
    virtual KisReplicaCapabilities capabilities() const = 0;

    virtual KisReplicaOperation requestReplica(const KisPageVersion &version,
                                                KisPageAccessDomain domain,
                                                KisPageAccessMode mode,
                                                KisPagePriority priority) = 0;
    virtual KisReplicaOperation prepareWrite(const KisPageKey &key,
                                              KisPageGeneration generation,
                                              KisPageAccessDomain domain,
                                              KisPagePriority priority) = 0;
    virtual KisReplicaOperation transfer(const KisReplicaHandle &source,
                                          const KisReplicaHandle &target,
                                          KisPagePriority priority) = 0;
    virtual bool validate(const KisReplicaHandle &replica,
                          KisPageGeneration expectedGeneration) const = 0;
    virtual void retire(const KisReplicaHandle &replica,
                        const KisCompletionTicket &lastUse) = 0;
    virtual KisReplicaMemoryUsage memoryUsage() const = 0;
};

#endif // KIS_PAGE_REPLICA_PROVIDER_H

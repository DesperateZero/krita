/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_UNIFIED_SSD_PAGE_STORE_H
#define KIS_UNIFIED_SSD_PAGE_STORE_H

#include <QScopedPointer>

#include "KisCompletionRegistry.h"
#include "KisPageReplicaProvider.h"

struct KRITAIMAGE_EXPORT KisLegacySwapArchiveConfig
{
    KisReplicaProviderId archive;
    KisReplicaProviderEpoch epoch;
    quint64 byteBudget = 0;

    bool isValid() const
    {
        return archive.isValid() && epoch.isValid() && byteBudget != 0;
    }
};

/**
 * Exact-generation BR1 archive seam for evolving KisSwappedDataStore into the
 * single SSD provider. The in-memory legacy-raw archive is operational for
 * identity/failure tests; the KisPageReplicaProvider half deliberately keeps
 * Ssd/durable capabilities disabled until BR3 recovery guarantees exist.
 */
class KRITAIMAGE_EXPORT KisUnifiedSsdPageStore
    : public KisPageReplicaProvider,
      public KisExactGenerationArchive
{
public:
    KisUnifiedSsdPageStore();
    ~KisUnifiedSsdPageStore() override;

    bool configureLegacySwapArchive(
        const KisLegacySwapArchiveConfig &config,
        const QSharedPointer<KisCompletionRegistry> &completions,
        QString *error = nullptr);
    bool isOperational() const override;
    KisReplicaProviderId archiveId() const override;
    KisReplicaProviderEpoch archiveEpoch() const override;
    bool synchronousOperations() const override;
    KisPageArchiveOperation storeExact(
        const KisExactPageArchiveWrite &write,
        KisPagePriority priority) override;
    bool cancelStoreExact(KisPageOperationId operation) override;
    bool loadExact(const KisPageVersion &version,
                   const KisPageAllocationDescriptor &descriptor,
                   QByteArray *bytes,
                   KisReplicaLayout *layout = nullptr) const override;
    bool contains(const KisPageVersion &version) const override;
    bool forget(const KisPageVersion &version) override;
    void injectNextArchiveWriteFailure();

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

#endif // KIS_UNIFIED_SSD_PAGE_STORE_H

/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_UNIFIED_SSD_PAGE_STORE_H
#define KIS_UNIFIED_SSD_PAGE_STORE_H

#include "KisPageReplicaProvider.h"

struct KRITAIMAGE_EXPORT KisSsdPageRecord
{
    KisPageVersion version;
    QByteArray formatId;
    QByteArray codec;
    quint64 compressedSize = 0;
    quint64 uncompressedSize = 0;
    QByteArray checksum;
    quint64 commitSequence = 0;

    bool isValid() const
    {
        return version.isValid() &&
               !formatId.isEmpty() &&
               !codec.isEmpty() &&
               uncompressedSize > 0 &&
               !checksum.isEmpty() &&
               commitSequence != 0;
    }
};

/**
 * Placeholder for the single SSD replica provider that will evolve the
 * existing KisSwappedDataStore. It is not a second Vulkan-specific swap path.
 */
class KRITAIMAGE_EXPORT KisUnifiedSsdPageStore : public KisPageReplicaProvider
{
public:
    KisUnifiedSsdPageStore();
    ~KisUnifiedSsdPageStore() override;

    QString name() const override;
    KisReplicaCapabilities capabilities() const override;
    KisReplicaOperation requestReplica(const KisPageVersion &version,
                                        KisPageAccessDomain domain,
                                        KisPageAccessMode mode,
                                        KisPagePriority priority) override;
    KisReplicaOperation prepareWrite(const KisPageKey &key,
                                      KisPageGeneration generation,
                                      KisPageAccessDomain domain,
                                      KisPagePriority priority) override;
    KisReplicaOperation transfer(const KisReplicaHandle &source,
                                  const KisReplicaHandle &target,
                                  KisPagePriority priority) override;
    bool validate(const KisReplicaHandle &replica,
                  KisPageGeneration expectedGeneration) const override;
    void retire(const KisReplicaHandle &replica,
                const KisCompletionTicket &lastUse) override;
    KisReplicaMemoryUsage memoryUsage() const override;
};

#endif // KIS_UNIFIED_SSD_PAGE_STORE_H

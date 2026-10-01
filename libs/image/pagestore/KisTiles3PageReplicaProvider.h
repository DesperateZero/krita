/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_TILES3_PAGE_REPLICA_PROVIDER_H
#define KIS_TILES3_PAGE_REPLICA_PROVIDER_H

#include <QScopedPointer>
#include <QSharedPointer>

#include "KisCompletionRegistry.h"
#include "KisPageReplicaProvider.h"
#include "tiles3/KisTilePageStoreBridge.h"

class KisTileData;
class KisCpuWriteGuard;
class KisCpuReadGuard;

// Cumulative provider-local logical work, including work later discarded.
// Not physical traffic, not semantic default requests, not process-wide work.
struct KRITAIMAGE_EXPORT KisTiles3PayloadWork
{
    quint64 defaultInitializedPages = 0;
    quint64 defaultInitializedBytes = 0;
    quint64 explicitCopyPages = 0;
    quint64 explicitCopyBytes = 0;
    quint64 nativeDuplicatePages = 0;
    quint64 nativeDuplicateBytes = 0;
    quint64 nativePrecloneHits = 0;
    quint64 nativeForegroundCopyBytes = 0;
    quint64 adoptedPages = 0;
    quint64 adoptedBytes = 0;
    // Complete caller payload copied into fresh backing, not default fill,
    // before-image COW, physical migration, or an immutable alias.
    quint64 payloadInitializedPages = 0;
    quint64 payloadInitializedBytes = 0;
};

/**
 * CPU replica provider backed by Krita's existing KisTileDataStore.
 *
 * The provider deliberately accepts only the native 64x64 tightly-packed
 * tile layout.  It lets PageStore own generation/publication/lease identity
 * while retaining the existing tile allocator, pooler and swapper as the one
 * physical RAM/swap implementation.  Provider ownership uses KisTileData's
 * ref count, not usersCount. Legacy wrappers and immutable source capabilities
 * additionally hold COW-user references, preventing legacy writers from
 * mutating a source after it has been captured for a semantic alias.
 */
class KRITAIMAGE_EXPORT KisTiles3PageReplicaProvider final
    : public KisPageReplicaProvider
{
public:
    KisTiles3PageReplicaProvider();
    ~KisTiles3PageReplicaProvider() override;

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
    KisReplicaOperation prepareSynchronousCpuPayload(
        KisPageOperationId operation, const KisPageVersion &version,
        const KisPageAllocationDescriptor &descriptor, const KisCpuPagePayload &payload,
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
    KisReplicaBackingFootprint backingFootprint(
        const KisReplicaHandle &replica) const override;
    QVector<KisReplicaBackingDomainChange> backingDomainChanges() const override;
    bool mayHaveBackingDomainChanges() const noexcept override;
    void acknowledgeBackingDomainChange(quint64 physicalSlot,
                                        quint64 revision) override;
    bool registerBackingDomainAdmission(
        const QSharedPointer<KisReplicaBackingDomainAdmission> &admission,
        QString *error = nullptr) override;
    KisReplicaOperation prepareSynchronousWriteCopy(
        KisPageOperationId operation,
        const KisReplicaHandle &source,
        const KisPageVersion &targetVersion,
        const KisPageAllocationDescriptor &descriptor,
        KisPagePriority priority) override;
    KisTiles3PayloadWork payloadWork() const;
    QSharedPointer<KisCpuResidentBinding> cpuResidentBinding(
        const KisReplicaHandle &replica, KisCpuResidentReadStatus *status = nullptr) const override;

    // The caller must own a completed immutable/locked legacy producer input.
    // Capturing is a COW-user reference, not a borrowed pointer or RAM pin.
    // The no-swap-in route accepts only allocator-guaranteed alignment (at
    // most pointer alignment); stricter contracts require another route.
    QSharedPointer<const KisPageReplicaSource> captureCompletedTileSource(
        const KisPageAllocationDescriptor &descriptor, KisTileData *tileData,
        QString *error = nullptr);
    QSharedPointer<const KisPageReplicaSource> captureCpuReadSource(
        const KisCpuReadGuard &guard, const KisPageAllocationDescriptor &descriptor,
        QString *error = nullptr);
    bool sourceMatchesReadGuard(const QSharedPointer<const KisPageReplicaSource> &source,
                                const KisCpuReadGuard &guard) const;
    KisTileData *tileDataForCpuReadGuard(const KisCpuReadGuard &guard) const;
    TileLease acquireTileReadCache(const KisCpuReadGuard &guard, TileLease reuse = {}) const;
    TileLease acquireTileReadCache(KisPageLeaseId lease, TileLease reuse = {}) const;
    bool readCacheMatchesVersion(const KisTilePageStoreLease *cache, const KisPageVersion &version) const;
    // Prepared while the writer is active, first repinned only after finish.
    // The KisTile wrapper owns the cached storage's immutable/COW reference.
    TileLease prepareTileReadCache(const KisCpuWriteGuard &guard, TileLease reuse = {}) const;
    KisReplicaOperation prepareSynchronousSource(
        KisPageOperationId operation, const QSharedPointer<const KisPageReplicaSource> &source,
        const KisPageVersion &targetVersion, const KisPageAllocationDescriptor &descriptor,
        KisReplicaSourceUse use, KisPagePriority priority) override;
    bool copySynchronousSourceToCpu(
        const QSharedPointer<const KisPageReplicaSource> &source,
        const KisPageAllocationDescriptor &descriptor, void *destination,
        quint32 rowStride, quint64 byteSize) override;

    KisReplicaHandle adoptInitialTile(
        const KisPageVersion &version,
        const KisPageAllocationDescriptor &descriptor,
        KisTileData *tileData,
        QString *error = nullptr);

    /**
     * Internal cutover bridge. The returned pointer is valid only while the
     * caller owns a matching CPU lease. It is exposed for KisTile wrappers,
     * never as a PageStore consumer API.
     */
    KisTileData *tileDataForLease(KisPageLeaseId lease) const;
    // Narrow legacy adapter: the owner-issued guard, not a raw handle, proves
    // exclusive access and keeps this exact allocation pinned.
    KisTileData *tileDataForCpuWriteGuard(const KisCpuWriteGuard &guard) const;

private:
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_TILES3_PAGE_REPLICA_PROVIDER_H

/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisUnifiedSsdPageStore.h"

#include <QCryptographicHash>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>

#include <memory>
#include <limits>
#include <cstring>

#include "tiles3/swap/kis_swapped_data_store.h"

namespace {

KisReplicaOperation unsupportedSsdOperation(KisPageOperationId operationId)
{
    KisReplicaOperation operation;
    operation.operation = operationId;
    operation.error = QStringLiteral("BR1 SSD replica provider is disabled until BR3 durability work");
    return operation;
}

struct LegacyArchiveEntry
{
    KisReplicaLayout layout;
    QByteArray checksum;
    quint64 legacySwapRecord = 0;
};

}

class KisUnifiedSsdPageStore::Private
{
public:
    mutable QMutex mutex;
    bool failNextWrite = false;
    KisLegacySwapArchiveConfig config;
    std::shared_ptr<KisCompletionRegistry> completions;
    quint64 completionSource = 0;
    quint64 storedBytes = 0;
    std::unique_ptr<KisSwappedDataStore> swapStore;
    QHash<KisPageVersion, LegacyArchiveEntry> records;
};

KisUnifiedSsdPageStore::KisUnifiedSsdPageStore()
    : d(new Private)
{
}
KisUnifiedSsdPageStore::~KisUnifiedSsdPageStore() = default;

bool KisUnifiedSsdPageStore::configureLegacySwapArchive(
    const KisLegacySwapArchiveConfig &config,
    const std::shared_ptr<KisCompletionRegistry> &completions,
    QString *error)
{
    if (!config.isValid() || !completions || !completions->isOperational()) {
        KisPageStoreDetail::setError(error, QStringLiteral("legacy swap archive configuration is invalid"));
        return false;
    }
    QMutexLocker locker(&d->mutex);
    if (d->swapStore) {
        KisPageStoreDetail::setError(error, QStringLiteral("legacy swap archive is already configured"));
        return false;
    }
    const quint64 completionSource = completions->registerSource(KisCompletionDomain::IoOperation);
    if (completionSource == 0) {
        KisPageStoreDetail::setError(error, QStringLiteral("legacy swap completion source registration failed"));
        return false;
    }
    d->config = config;
    d->completions = completions;
    d->completionSource = completionSource;
    d->swapStore = std::make_unique<KisSwappedDataStore>();
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisUnifiedSsdPageStore::isOperational() const
{
    QMutexLocker locker(&d->mutex);
    return bool(d->swapStore);
}

KisReplicaProviderId KisUnifiedSsdPageStore::archiveId() const
{
    QMutexLocker locker(&d->mutex);
    return d->config.archive;
}

KisReplicaProviderEpoch KisUnifiedSsdPageStore::archiveEpoch() const
{
    QMutexLocker locker(&d->mutex);
    return d->config.epoch;
}

bool KisUnifiedSsdPageStore::synchronousOperations() const
{
    return true;
}

KisPageArchiveOperation KisUnifiedSsdPageStore::storeExact(
    const KisExactPageArchiveWrite &write,
    KisPagePriority priority)
{
    Q_UNUSED(priority);
    KisPageArchiveOperation result;
    result.operation = write.operation;
    result.version = write.version;
    const auto fail = [&result](const QString &error) {
        result.status = KisPageRequestStatus::Failed;
        result.error = error;
        return result;
    };
    if (!write.isValid()) {
        return fail(QStringLiteral("exact-generation archive write is invalid"));
    }

    QMutexLocker locker(&d->mutex);
    if (!d->swapStore || d->records.contains(write.version)) {
        return fail(QStringLiteral("exact-generation archive is unavailable or already contains the version"));
    }
    if (d->failNextWrite) {
        d->failNextWrite = false;
        return fail(QStringLiteral("injected legacy swap archive write failure"));
    }
    if (write.sourceLayout.byteSize > d->config.byteBudget -
            qMin(d->storedBytes, d->config.byteBudget)) {
        return fail(QStringLiteral("legacy swap archive budget is exhausted"));
    }
    if (write.sourceLayout.byteSize >
        quint64(std::numeric_limits<qsizetype>::max())) {
        return fail(QStringLiteral("legacy swap archive payload size is unsupported"));
    }

    const auto completion = d->completions->allocatePending(d->completionSource);
    if (!completion.isValid()) return fail(QStringLiteral("legacy swap archive completion preparation failed"));
    auto finishFailure = qScopeGuard([&] {
        d->completions->completePrepared(completion, KisCompletionStatus::Failed);
    });
    QByteArray payload(qsizetype(write.sourceLayout.byteSize), char(0));
    const quint64 rowBytes = write.descriptor.minimumRowBytes();
    const int rowCount = write.descriptor.pageExtent.height();
    for (int row = 0; row < rowCount; ++row) {
        std::memcpy(payload.data() + quint64(row) * write.sourceLayout.rowStride,
                    static_cast<const char *>(write.sourceData) +
                        quint64(row) * write.sourceLayout.rowStride,
                    size_t(rowBytes));
    }
    const quint64 swapRecord = d->swapStore->storeRawRecord(payload);
    if (swapRecord == 0) {
        return fail(QStringLiteral("legacy swap archive payload allocation failed"));
    }

    LegacyArchiveEntry entry;
    entry.layout = write.sourceLayout;
    entry.legacySwapRecord = swapRecord;
    entry.checksum = QCryptographicHash::hash(payload, QCryptographicHash::Sha256);

    d->completions->completePrepared(completion, KisCompletionStatus::Succeeded);
    finishFailure.dismiss();
    d->storedBytes += entry.layout.byteSize;
    d->records.insert(write.version, entry);
    result.status = KisPageRequestStatus::Ready;
    result.completion = completion;
    return result;
}

bool KisUnifiedSsdPageStore::cancelStoreExact(KisPageOperationId operation)
{
    Q_UNUSED(operation);
    // The BR1 legacy-raw adapter is terminal-at-return, so there is no
    // cancellable operation by the time control returns to PageStore.
    return false;
}

bool KisUnifiedSsdPageStore::loadExact(
    const KisPageVersion &version,
    const KisPageAllocationDescriptor &descriptor,
    QByteArray *bytes,
    KisReplicaLayout *layout) const
{
    if (!version.isValid() || !descriptor.isValid() || !bytes) return false;
    QMutexLocker locker(&d->mutex);
    const auto it = d->records.constFind(version);
    QByteArray payload;
    if (!d->swapStore || it == d->records.constEnd() ||
        !it->layout.matches(descriptor) ||
        !d->swapStore->loadRawRecord(it->legacySwapRecord, &payload) ||
        quint64(payload.size()) != it->layout.byteSize ||
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256) != it->checksum) {
        return false;
    }
    *bytes = payload;
    if (layout) *layout = it->layout;
    return true;
}

bool KisUnifiedSsdPageStore::contains(const KisPageVersion &version) const
{
    QMutexLocker locker(&d->mutex);
    return d->swapStore && version.isValid() && d->records.contains(version);
}

bool KisUnifiedSsdPageStore::forget(const KisPageVersion &version)
{
    QMutexLocker locker(&d->mutex);
    auto it = d->records.find(version);
    if (!d->swapStore || it == d->records.end() ||
        !d->swapStore->forgetRawRecord(it->legacySwapRecord)) {
        return false;
    }
    d->storedBytes -= it->layout.byteSize;
    d->records.erase(it);
    return true;
}

void KisUnifiedSsdPageStore::injectNextArchiveWriteFailure()
{
    QMutexLocker locker(&d->mutex);
    d->failNextWrite = true;
}

QString KisUnifiedSsdPageStore::name() const
{
    return QStringLiteral(
        "Krita exact-generation legacy swap archive (SSD replica disabled)");
}

KisReplicaProviderId KisUnifiedSsdPageStore::providerId() const
{
    return archiveId();
}

KisReplicaProviderEpoch KisUnifiedSsdPageStore::providerEpoch() const
{
    return archiveEpoch();
}

KisReplicaCapabilities KisUnifiedSsdPageStore::capabilities() const
{
    // TODO(BR3): advertise Ssd/durable only after atomic index and recovery tests pass.
    return {};
}

KisReplicaOperation KisUnifiedSsdPageStore::requestReplica(KisPageOperationId operation,
                                                            const KisPageVersion &version,
                                                            const KisPageAllocationDescriptor &descriptor,
                                                            KisPageAccessDomain domain,
                                                            KisPageAccessMode mode,
                                                            KisPagePriority priority)
{
    Q_UNUSED(version);
    Q_UNUSED(descriptor);
    Q_UNUSED(domain);
    Q_UNUSED(mode);
    Q_UNUSED(priority);
    // TODO(BR3): schedule checked materialization from the committed segment index.
    return unsupportedSsdOperation(operation);
}

KisReplicaOperation KisUnifiedSsdPageStore::prepareWrite(KisPageOperationId operation,
                                                          const KisPageVersion &version,
                                                          const KisPageAllocationDescriptor &descriptor,
                                                          KisPageAccessDomain domain,
                                                          KisPageWriteMode mode,
                                                          KisPagePriority priority)
{
    Q_UNUSED(version);
    Q_UNUSED(descriptor);
    Q_UNUSED(domain);
    Q_UNUSED(mode);
    Q_UNUSED(priority);
    // TODO(BR3): reserve an append record without changing page authority.
    return unsupportedSsdOperation(operation);
}

KisReplicaOperation KisUnifiedSsdPageStore::transfer(
    const KisReplicaTransferRequest &request,
    KisPagePriority priority)
{
    Q_UNUSED(priority);
    // TODO(BR3): compress, checksum, append, fsync policy, then atomically commit index.
    return unsupportedSsdOperation(request.operation);
}

KisReplicaAccess KisUnifiedSsdPageStore::resolveAccess(
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
    // SSD records are never directly accessible consumer leases. PageStore
    // must materialize them into a CPU/GPU-accessible provider first.
    return {};
}

void KisUnifiedSsdPageStore::releaseAccess(
    KisReplicaAccess access,
    const KisCompletionTicket &lastUse)
{
    Q_UNUSED(access);
    Q_UNUSED(lastUse);
}

bool KisUnifiedSsdPageStore::validate(const KisReplicaHandle &replica,
                                      const KisPageAllocationDescriptor &descriptor) const
{
    Q_UNUSED(replica);
    Q_UNUSED(descriptor);
    // TODO(BR3): verify record identity, generation, size, codec, and checksum.
    return false;
}

KisReplicaOperation KisUnifiedSsdPageStore::retire(
    KisPageOperationId operation,
    const KisReplicaHandle &replica,
    const KisCompletionTicket &lastUse)
{
    Q_UNUSED(replica);
    Q_UNUSED(lastUse);
    // TODO(BR3): mark unreachable in the index; reclaim only through background GC.
    return unsupportedSsdOperation(operation);
}

KisReplicaMemoryUsage KisUnifiedSsdPageStore::memoryUsage() const
{
    QMutexLocker locker(&d->mutex);
    return {0, d->storedBytes, d->config.byteBudget};
}

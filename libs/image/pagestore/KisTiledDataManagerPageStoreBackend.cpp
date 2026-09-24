/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisTiledDataManagerPageStoreBackend.h"

#include "KisPageStoreDiagnostics_p.h"
#include "KisPageStoreIteratorReadScope_p.h"
#include "KisPageWriteCoordinator_p.h"
#include "KisTiles3PageReplicaProvider.h"
#include "kis_image_config.h"
#include "tiles3/kis_tiled_data_manager.h"
#include "tiles3/kis_tile_data.h"
#include "tiles3/kis_tile_data_store.h"

#include <QMutex>
#include <QMutexLocker>
#include <QReadWriteLock>
#include <QSet>
#include <QWaitCondition>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace {

std::atomic<quint64> nextTiles3ProviderId{0x4000000000000000ULL};

qint32 pageCoordinate(qint32 pixel, qint32 pageExtent)
{
    qint32 coordinate = pixel / pageExtent;
    if (pixel % pageExtent < 0) --coordinate;
    return coordinate;
}

struct ProductBackingPolicy
{
    quint64 providerBytes = 0;
    KisPageBackingLimits limits;
};

bool configMiBToBytes(int value, const char *name, quint64 *bytes,
                      QString *error)
{
    constexpr quint64 bytesPerMiB = quint64(1) << 20;
    if (!bytes || value < 0
        || quint64(value) > std::numeric_limits<quint64>::max() / bytesPerMiB) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("PageStore %1 limit is invalid")
                       .arg(QString::fromLatin1(name)));
        return false;
    }
    *bytes = quint64(value) * bytesPerMiB;
    return true;
}

bool deriveProductBackingPolicy(ProductBackingPolicy *policy, QString *error)
{
    if (!policy) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("PageStore backing policy output is missing"));
        return false;
    }
    const KisImageConfig config(true);
    quint64 ramBytes = 0;
    quint64 poolBytes = 0;
    quint64 swapBytes = 0;
    if (!configMiBToBytes(config.tilesHardLimit(), "RAM", &ramBytes, error)
        || !configMiBToBytes(config.poolLimit(), "pool", &poolBytes, error)
        || !configMiBToBytes(config.maxSwapSize(), "swap", &swapBytes, error)) {
        return false;
    }
    if (swapBytes > std::numeric_limits<quint64>::max() - ramBytes
        || poolBytes > std::numeric_limits<quint64>::max() - ramBytes) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("PageStore configured memory limits overflow"));
        return false;
    }
    const quint64 logicalBytes = ramBytes + swapBytes;
    const quint64 internalCpuBytes = ramBytes + poolBytes;
    ProductBackingPolicy result;
    result.providerBytes = logicalBytes;
    result.limits.logicalCurrentBytes = logicalBytes;
    result.limits.retainedHistoryBytes = logicalBytes;
    result.limits.residentCurrentBytes = {ramBytes, 0, 0, swapBytes};
    result.limits.residentHistoryBytes = result.limits.residentCurrentBytes;
    result.limits.activePendingBytes = ramBytes;
    // BR1 has no GPU transfer provider. Rejecting this bucket prevents a
    // finite product configuration from advertising unimplemented capacity.
    result.limits.inFlightReplicaBytes = 0;
    result.limits.retirementDebtBytes = logicalBytes;
    // These allocations are outside the tiles payload allocator. Their class
    // ceilings therefore use the configured RAM total (tiles plus pool). The
    // default zero-sized pool must not disable required default-page reads.
    result.limits.optionalCacheBytes = internalCpuBytes;
    result.limits.metadataArenaBytes = internalCpuBytes;
    result.limits.durableStoreCapacity = swapBytes;
    *policy = result;
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool updateSnapshotDerivedExtent(KisImageEpochSnapshot *snapshot,
                                 KisSurfaceId surfaceId,
                                 QString *error)
{
    if (!snapshot) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 clone snapshot is absent"));
        return false;
    }
    auto surfaceIt = std::find_if(
        snapshot->surfaces.begin(), snapshot->surfaces.end(),
        [surfaceId](const KisSurfaceEpochState &surface) {
            return surface.surface == surfaceId;
        });
    if (surfaceIt == snapshot->surfaces.end()) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 clone surface is absent"));
        return false;
    }

    QRect extent;
    const qint64 width = surfaceIt->logicalPageExtent.width();
    const qint64 height = surfaceIt->logicalPageExtent.height();
    for (const KisPageVersion &version : std::as_const(snapshot->manifest)) {
        if (!(version.key.surface == surfaceId)) continue;
        const qint64 x = qint64(version.key.page.column) * width;
        const qint64 y = qint64(version.key.page.row) * height;
        if (x < std::numeric_limits<int>::min() ||
            y < std::numeric_limits<int>::min() ||
            x + width - 1 > std::numeric_limits<int>::max() ||
            y + height - 1 > std::numeric_limits<int>::max()) {
            KisPageStoreDetail::setError(error, QStringLiteral("tiles3 clone extent exceeds QRect range"));
            return false;
        }
        const QRect pageRect{int(x), int(y), int(width), int(height)};
        extent = extent.isNull() ? pageRect : extent.united(pageRect);
    }
    if (surfaceIt->contentExtent != extent) {
        surfaceIt->contentExtent = extent;
        ++surfaceIt->extentRevision;
    }
    snapshot->defaultPixelRevision = qMax(
        snapshot->defaultPixelRevision, surfaceIt->defaultPixelRevision);
    snapshot->extentRevision = qMax(
        snapshot->extentRevision, surfaceIt->extentRevision);
    KisPageStoreDetail::setError(error, {});
    return true;
}

}

class KisTiledDataManagerPageStoreLease final : public KisTilePageStoreLease
{
public:
    KisTiledDataManagerPageStoreLease(
        KisTiledDataManagerPageStoreBackend *backend,
        KisTileData *tile,
        KisCpuWriteGuard &&guard,
        QSharedPointer<KisTiledDataManagerPageStoreWriteBatch::Private> batch)
        : m_backend(backend)
        , m_tileData(tile)
        , m_batch(std::move(batch))
        , m_native(std::move(guard))
    {}
    KisTiledDataManagerPageStoreLease(
        KisTiledDataManagerPageStoreBackend *backend,
        KisTileData *tile, const KisPageTransaction &transaction,
        bool ownsTransaction, KisPageMutationSession &&mutation, KisCpuWriteGuard &&guard)
        : m_backend(backend), m_tileData(tile),
          m_transaction(ownsTransaction ? transaction : KisPageTransaction{}),
          m_native(std::move(guard)),
          m_ownedMutation(std::move(mutation))
    {
        if (m_transaction.isValid()) m_backend->registerAnonymousLease(this);
    }
    KisTiledDataManagerPageStoreLease(
        KisTiledDataManagerPageStoreBackend *backend,
        KisTileData *tileData,
        KisPageStoreReadPage &&read)
        : m_backend(backend)
        , m_tileData(tileData)
        , m_read(std::move(read))
    {}

    ~KisTiledDataManagerPageStoreLease() override
    {
        if (m_backend) finish();
    }

    KisTileData *tileData() const override { return m_tileData; }
    bool writable() const override { return m_native.isValid(); }
    void markDirty() override { m_dirty = true; }

    bool finish() override
    {
        if (!m_backend) return true;
        if (m_native.isValid() && !m_ownedMutation.isActive()) {
            // Return the actual RAM pin now; the segment retains a parked
            // writer reservation and seals/proves once at its explicit end.
            m_native = {};
            m_batch.reset();
            return complete(true);
        }
        if (m_read.data()) {
            m_read.reset();
            return complete(true);
        }
        if (m_ownedMutation.isActive()) {
            // Compatibility unlock is an existing visibility boundary. Keep
            // it, but perform first-write/prepare/seal through the same native
            // mutation implementation as explicit operation-wide batches.
            m_native = {};
            const bool success = m_dirty ? m_ownedMutation.sealForLegacyUnlock() : m_ownedMutation.cancel();
            m_ownedMutation = {};
            if (!success) {
                if (m_transaction.isValid() && m_backend) m_backend->abortOwnedTransaction(m_transaction);
                return complete(false);
            }
            const bool committed = !m_transaction.isValid() ||
                (m_backend && m_backend->finishOwnedTransaction(m_transaction));
            return complete(committed);
        }
        return complete(false);
    }

    bool cancelForBarrier()
    {
        if (!m_backend || !m_ownedMutation.isActive() || !m_transaction.isValid()) {
            return false;
        }
        m_native = {};
        if (!m_ownedMutation.cancel()) return false;
        m_ownedMutation = {};
        return complete(m_backend->abortOwnedTransaction(m_transaction));
    }

private:
    bool complete(bool result)
    {
        if (m_backend && m_transaction.isValid())
            m_backend->unregisterAnonymousLease(this);
        m_backend = nullptr;
        return result;
    }

    KisTiledDataManagerPageStoreBackend *m_backend = nullptr;
    KisTileData *m_tileData = nullptr;
    KisPageTransaction m_transaction;
    bool m_dirty = false;
    QSharedPointer<KisTiledDataManagerPageStoreWriteBatch::Private> m_batch;
    KisPageStoreReadPage m_read;
    KisCpuWriteGuard m_native;
    KisPageMutationSession m_ownedMutation;
};

class KisTiledDataManagerPageStoreBackend::Private
{
public:
    bool prepareStore(const KisImageEpochSnapshot &initial, QString *error)
    {
        ProductBackingPolicy policy;
        if (!deriveProductBackingPolicy(&policy, error))
            return false;
        const auto completions = QSharedPointer<KisCompletionRegistry>::create();
        provider = QSharedPointer<KisTiles3PageReplicaProvider>::create();
        KisCpuResidentReplicaProviderConfig providerConfig;
        providerConfig.provider =
            KisPageStoreDetail::allocateMonotonicId<KisReplicaProviderId>(
                &nextTiles3ProviderId);
        providerConfig.providerEpoch = KisReplicaProviderEpoch{1};
        providerConfig.budgetBytes = policy.providerBytes;
        store.reset(new KisPageStore);
        history.reset(new KisPageStoreMementoManager);
        return provider->configure(providerConfig, completions, error) &&
               store->configureBackingLimits(policy.limits, error) &&
               store->configure(initial, completions, 64, error) &&
               store->registerReplicaProvider(provider);
    }

    bool finalizeStore(QString *error)
    {
        return store->finalizeInitialization(error) &&
               history->configure(store.get(), error);
    }

    mutable QMutex mutex;
    QMutex transactionMutex;
    QReadWriteLock publicationLock;
    QSharedPointer<KisTiles3PageReplicaProvider> provider;
    std::unique_ptr<KisPageStore> store;
    std::unique_ptr<KisPageStoreMementoManager> history;
    KisSurfaceId surface{1};
    KisPageTransaction anonymousTransaction;
    quint64 anonymousClients = 0;
    bool anonymousFailed = false;
    KisPageStoreHistoryTransaction activeHistory;
    KisMementoSP currentMemento;
    QHash<const KisMemento *, KisPageStoreMemento> mementos;
    QSet<KisTiledDataManagerPageStoreLease *> anonymousLeases;
    QHash<Qt::HANDLE, QWeakPointer<KisTiledDataManagerPageStoreWriteBatch::Private>> cpuMutationBatches;
    // Configuration is published once and the backend/store lifetime is
    // owned by KisTiledDataManager.  Tile lookup is a production hot path,
    // so readers must not serialize on the control-plane mutex merely to
    // discover that PageStore is enabled.
    std::atomic_bool operational{false};
    QByteArray uniformPixel;
    QSharedPointer<const KisPageReplicaSource> uniformSource;
};

class KisTiledDataManagerPageStoreWriteBatch::Private
{
public:
    ~Private()
    {
        // Every borrowed native lease holds this control block. The final
        // release therefore runs only after the public batch and all guards
        // are gone. Never race an "abandoned" flag against a foreign unlock:
        // an unfinished operation is cancelled here, under unique ownership.
        if (backend) {
            failed = true;
            finish();
        }
    }
    bool finishClient(bool succeeded, QString *error = nullptr);
    bool finish(QString *error = nullptr);
    KisTiledDataManagerPageStoreBackend *backend = nullptr;
    KisPageTransaction transaction;
    bool owned = false;
    bool failed = false;
    bool iteratorScope = false;
    quint64 clients = 1;
    Qt::HANDLE thread = QThread::currentThreadId();
    KisPageMutationSession mutation;
    QHash<KisTileData *, QSharedPointer<const KisPageReplicaSource>> sources;
};

class KisTiledDataManagerIteratorWriteScope::Private
{
public:
    std::unique_ptr<KisTiledDataManagerPageStoreWriteBatch> batch;
};

KisTiledDataManagerIteratorWriteScope::KisTiledDataManagerIteratorWriteScope()
    : d(new Private)
{
}

KisTiledDataManagerIteratorWriteScope::~KisTiledDataManagerIteratorWriteScope() = default;

bool KisTiledDataManagerIteratorWriteScope::finish()
{
    return d && d->batch && d->batch->finish();
}

KisTiledDataManagerPageStoreWriteBatch::
KisTiledDataManagerPageStoreWriteBatch(
    KisTiledDataManagerPageStoreBackend *backend,
    const KisPageTransaction &transaction,
    bool owned)
    : d(new Private)
{
    d->backend = backend;
    d->transaction = transaction;
    d->owned = owned;
}

KisTiledDataManagerPageStoreWriteBatch::
KisTiledDataManagerPageStoreWriteBatch(QSharedPointer<Private> shared)
    : d(std::move(shared))
{
}

KisTiledDataManagerPageStoreWriteBatch::
~KisTiledDataManagerPageStoreWriteBatch()
{
    if (!m_clientFinished) cancel();
}

bool KisTiledDataManagerPageStoreWriteBatch::finish(QString *error)
{
    if (m_clientFinished || !d) return false;
    m_clientFinished = true;
    return d->finishClient(true, error);
}

bool KisTiledDataManagerPageStoreWriteBatch::cancel()
{
    if (m_clientFinished || !d) return false;
    m_clientFinished = true;
    return d->finishClient(false);
}

bool KisTiledDataManagerPageStoreWriteBatch::Private::finishClient(
    bool succeeded, QString *error)
{
    KisTiledDataManagerPageStoreBackend *owner = backend;
    if (!owner) return succeeded && !failed;
    bool finalClient = false;
    bool clientSucceeded = false;
    {
        QMutexLocker lock(&owner->d->mutex);
        if (!succeeded) failed = true;
        if (clients == 0) return false;
        finalClient = --clients == 0;
        clientSucceeded = !failed;
    }
    return finalClient ? finish(error) : clientSucceeded;
}

bool KisTiledDataManagerPageStoreWriteBatch::Private::finish(QString *error)
{
    if (!backend) return !failed;
    if (!transaction.isValid() || !mutation.isActive()) return false;
    bool privateCancelled = false;
    if (failed) KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore private mutation batch cancelled"));
    const bool sealed = !failed && (iteratorScope
        ? mutation.sealForLegacyUnlock(error)
        : mutation.seal(error));
    if (!sealed) {
        failed = true;
        if (!mutation.cancel()) return false;
        privateCancelled = true;
    }
    // A confirmed private cancellation contributed no sealed delta. Return
    // this anonymous client normally instead of poisoning siblings/legacy
    // writers that share the transaction. Generic/partly published failures
    // still use the transaction-wide abort path.
    const bool terminal = !owned || (failed && !privateCancelled
        ? backend->abortOwnedTransaction(transaction)
        : backend->finishOwnedTransaction(transaction, privateCancelled ? nullptr : error));
    failed = failed || !terminal;
    // Keep the slot occupied even after its weak pointer expires, until the
    // anonymous client has finished/aborted. Otherwise the originating thread
    // can join that transaction while its last foreign guard is still
    // cancelling it, poisoning the next operation.
    QMutexLocker lock(&backend->d->mutex);
    backend->d->cpuMutationBatches.remove(thread);
    backend = nullptr;
    return !failed;
}

bool KisTiledDataManagerPageStoreWriteBatch::replaceFullTile(
    qint32 column,
    qint32 row,
    KisTileData *tileData,
    bool sparseDefault,
    QString *error)
{
    if (!d->backend || !d->transaction.isValid() || d->failed) {
        KisPageStoreDetail::setError(error, QStringLiteral(
            "tiles3 PageStore write batch is unavailable"));
        return false;
    }
    const KisPageKey key{d->backend->surface(), {column, row}};
    bool staged = d->mutation.reserveLegacyMutationPage(key, error);
    if (staged && sparseDefault) staged = d->mutation.removePage(key, error);
    else if (staged) {
        auto source = d->sources.value(tileData);
        if (!source) {
            const auto overlay = KisPageReadView::transactionOverlay(d->transaction.id);
            KisSurfaceEpochState state;
            if (d->backend->store()->resolveSurfaceState(key.surface, overlay, &state))
                source = d->backend->d->provider->captureCompletedTileSource(state.allocationDescriptor(), tileData, error);
            if (source) d->sources.insert(tileData, source);
        }
        staged = source && d->mutation.aliasPage(key, source, error);
    }
    if (!staged) d->failed = true;
    return staged;
}

KisTiledDataManagerPageStoreBackend::KisTiledDataManagerPageStoreBackend()
    : d(new Private)
{
}

KisTiledDataManagerPageStoreBackend::~KisTiledDataManagerPageStoreBackend()
{
    if (d->history) d->history->close();
    if (d->store) d->store->closeSession();
}

bool KisTiledDataManagerPageStoreBackend::configure(
    quint32 pixelSize,
    const quint8 *defaultPixel,
    QString *error)
{
    if (pixelSize == 0 || !defaultPixel) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore format is invalid"));
        return false;
    }
    QMutexLocker locker(&d->mutex);
    if (d->operational) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore is already configured"));
        return false;
    }

    KisSurfaceEpochState surface;
    surface.surface = d->surface;
    surface.format.formatId = pixelSize;
    surface.format.colorModelId = QByteArrayLiteral("KRITA_RAW_PIXEL");
    surface.format.colorDepthId = QByteArray::number(pixelSize * 8);
    surface.format.profileFingerprint = QByteArrayLiteral("tiles3-datamanager");
    surface.format.channelOrder = QByteArrayLiteral("RAW");
    surface.format.packing = QByteArrayLiteral("interleaved");
    surface.format.defaultPixel = QByteArray(
        reinterpret_cast<const char *>(defaultPixel), qsizetype(pixelSize));
    surface.format.channelCount = 1;
    surface.format.pixelStride = pixelSize;
    surface.format.pixelAlignment = 1;
    surface.format.hasAlpha = false;
    surface.format.alphaSemantic = KisSurfaceAlphaSemantic::Opaque;
    surface.format.endianness = KisSurfaceEndianness::NativeEndian;
    surface.format.codecVersion = 1;
    surface.contentExtent = QRect();
    surface.logicalPageExtent = QSize(KisTileData::WIDTH,
                                      KisTileData::HEIGHT);
    surface.layoutRevision = 1;
    surface.rowAlignment = 1;
    surface.defaultPixelRevision = 1;
    surface.extentRevision = 1;
    if (!surface.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore surface is invalid"));
        return false;
    }

    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = 1;
    initial.defaultPixelRevision = 1;
    initial.extentRevision = 1;
    initial.propertyRevision = 1;
    initial.surfaces = {surface};

    QString failure;
    if (!d->prepareStore(initial, &failure) || !d->finalizeStore(&failure)) {
        KisPageStoreDetail::setError(error, failure.isEmpty()
            ? QStringLiteral("tiles3 PageStore initialization failed")
            : failure);
        return false;
    }
    d->operational = true;
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisTiledDataManagerPageStoreBackend::configureClone(
    const KisTiledDataManagerPageStoreBackend &source,
    QString *error)
{
    QMutexLocker sourceTransactionLocker(&source.d->transactionMutex);
    KisPageStore *sourceStore = nullptr;
    KisPageStoreHistoryTransaction sourceHistory;
    {
        QMutexLocker sourceLocker(&source.d->mutex);
        if (!source.d->operational) {
            KisPageStoreDetail::setError(error, QStringLiteral("tiles3 clone source is unavailable"));
            return false;
        }
        if (source.d->anonymousTransaction.isValid() ||
            source.d->anonymousClients != 0) {
            KisPageStoreDetail::setError(error, QStringLiteral(
                "tiles3 clone source has active anonymous writes"));
            return false;
        }
        sourceStore = source.d->store.get();
        sourceHistory = source.d->activeHistory;
    }

    KisImageEpochSnapshot snapshot = sourceStore->captureCommittedEpoch();
    if (!snapshot.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 clone snapshot is invalid"));
        return false;
    }
    if (sourceHistory.isValid()) {
        const KisPreparedPageSet prepared =
            sourceStore->preparedPages(sourceHistory.transaction);
        if (prepared.isValid()) {
            for (const KisPageKey &removed : prepared.removedPages) {
                for (qsizetype i = snapshot.manifest.size(); i-- > 0;) {
                    if (snapshot.manifest.at(i).key == removed) {
                        snapshot.manifest.removeAt(i);
                    }
                }
            }
            for (const KisPreparedPageProof &proof : prepared.proofs) {
                bool replaced = false;
                for (KisPageVersion &version : snapshot.manifest) {
                    if (version.key == proof.authority.version.key) {
                        version = proof.authority.version;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) snapshot.manifest.append(proof.authority.version);
            }
            for (const KisSurfaceEpochChange &change :
                 prepared.surfaceChanges) {
                for (KisSurfaceEpochState &surface : snapshot.surfaces) {
                    if (surface.surface == change.after.surface) {
                        surface = change.after;
                        break;
                    }
                }
            }
        }
    }
    if (!updateSnapshotDerivedExtent(&snapshot, source.d->surface, error)) {
        return false;
    }

    QMutexLocker locker(&d->mutex);
    if (d->operational || d->store || d->provider || d->history) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 clone target is already configured"));
        return false;
    }
    QString failure;
    if (!d->prepareStore(snapshot, &failure)) {
        KisPageStoreDetail::setError(error, failure.isEmpty()
            ? QStringLiteral("tiles3 clone initialization failed") : failure);
        return false;
    }
    KisPageReadView sourceView;
    if (sourceHistory.isValid()) {
        sourceView = KisPageReadView::transactionOverlay(sourceHistory.transaction.id);
    }
    auto capturedSource = sourceStore->captureReadView(sourceView, &failure);
    if (!capturedSource.isValid()) {
        KisPageStoreDetail::setError(error, failure.isEmpty()
            ? QStringLiteral("tiles3 clone source view is unavailable") : failure);
        return false;
    }
    for (const KisPageVersion &version : snapshot.manifest) {
        KisPageAllocationDescriptor descriptor;
        if (!sourceStore->pageDescriptor(version, &descriptor)) {
            KisPageStoreDetail::setError(error, QStringLiteral(
                "tiles3 clone page descriptor is absent"));
            return false;
        }
        KisPageStoreReadPage read(sourceStore, capturedSource, version.key,
                                  &failure, KisPagePriority::Normal, false);
        KisTileData *tileData = source.tileDataForReadPage(read);
        if (!tileData || !(read.version() == version)) {
            KisPageStoreDetail::setError(error, QStringLiteral(
                "tiles3 clone canonical tile is unavailable"));
            return false;
        }
        const KisReplicaHandle authority = d->provider->adoptInitialTile(
            version, descriptor, tileData, &failure);
        const bool adopted = authority.isValid() &&
            d->store->adoptInitialPage(version, descriptor, authority,
                                       &failure);
        if (!adopted) {
            KisPageStoreDetail::setError(error, failure.isEmpty()
                ? QStringLiteral("tiles3 clone page adoption failed") : failure);
            return false;
        }
    }
    if (!d->finalizeStore(&failure)) {
        KisPageStoreDetail::setError(error, failure.isEmpty()
            ? QStringLiteral("tiles3 clone finalization failed") : failure);
        return false;
    }
    d->operational = true;
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisTiledDataManagerPageStoreBackend::isOperational() const
{
    return d->operational.load(std::memory_order_acquire);
}

KisPageStore *KisTiledDataManagerPageStoreBackend::store() const
{
    return d->operational.load(std::memory_order_acquire)
        ? d->store.get() : nullptr;
}

KisSurfaceId KisTiledDataManagerPageStoreBackend::surface() const
{
    return d->surface;
}

KisTiles3PayloadWork KisTiledDataManagerPageStoreBackend::payloadWork() const
{
    return d->operational.load(std::memory_order_acquire)
        ? d->provider->payloadWork() : KisTiles3PayloadWork{};
}

std::unique_ptr<KisTiledDataManagerPageStoreWriteBatch>
KisTiledDataManagerPageStoreBackend::beginMutationBatch(QString *error)
{
    {
        QMutexLocker lock(&d->mutex);
        if (d->cpuMutationBatches.contains(QThread::currentThreadId())) {
            KisPageStoreDetail::setError(error, QStringLiteral("nested native CPU batch is not supported"));
            return {};
        }
    }
    bool owned = false;
    const KisPageTransaction transaction = writableTransaction(&owned, error);
    if (!transaction.isValid()) return {};
    auto mutation = d->store->beginMutation(transaction, error);
    if (!mutation.isActive()) {
        if (owned) abortOwnedTransaction(transaction);
        return {};
    }
    auto batch = std::unique_ptr<KisTiledDataManagerPageStoreWriteBatch>(
        new KisTiledDataManagerPageStoreWriteBatch(this, transaction, owned));
    batch->d->mutation = std::move(mutation);
    QMutexLocker lock(&d->mutex);
    d->cpuMutationBatches.insert(batch->d->thread, batch->d.toWeakRef());
    return batch;
}

std::unique_ptr<KisTiledDataManagerIteratorWriteScope>
KisTiledDataManagerPageStoreBackend::beginIteratorMutationScope(QString *error)
{
    std::unique_ptr<KisTiledDataManagerPageStoreWriteBatch> batch;
    {
        QMutexLocker lock(&d->mutex);
        auto shared = d->cpuMutationBatches
                          .value(QThread::currentThreadId()).toStrongRef();
        if (shared) {
            // Zero clients means the last scope has started terminal sealing.
            // Keep the map entry as a barrier, but never revive that batch.
            if (!shared->iteratorScope || shared->failed || shared->clients == 0) {
                KisPageStoreDetail::setError(error, QStringLiteral(
                    "iterator cannot join the current native CPU batch"));
                return {};
            }
            ++shared->clients;
            KisPageStoreDetail::setError(error, {});
            batch.reset(new KisTiledDataManagerPageStoreWriteBatch(
                std::move(shared)));
        }
    }

    if (!batch) {
        batch = beginMutationBatch(error);
        if (!batch) return {};
        {
            QMutexLocker lock(&d->mutex);
            batch->d->iteratorScope = true;
        }
    }
    auto scope = std::unique_ptr<KisTiledDataManagerIteratorWriteScope>(
        new KisTiledDataManagerIteratorWriteScope);
    scope->d->batch = std::move(batch);
    return scope;
}

KisPageTransaction KisTiledDataManagerPageStoreBackend::writableTransaction(
    bool *owned,
    QString *error)
{
    QMutexLocker transactionLocker(&d->transactionMutex);
    QMutexLocker locker(&d->mutex);
    if (!d->operational || !owned) {
        KisPageStoreDetail::setError(error, QStringLiteral(
            "tiles3 PageStore write owner is invalid"));
        return {};
    }
    if (d->activeHistory.isValid()) {
        *owned = false;
        KisPageStoreDetail::setError(error, {});
        return d->activeHistory.transaction;
    }
    if (d->anonymousFailed) {
        KisPageStoreDetail::setError(error, QStringLiteral(
            "tiles3 PageStore anonymous transaction is aborting"));
        return {};
    }
    *owned = true;
    if (!d->anonymousTransaction.isValid()) {
        d->anonymousTransaction = d->store->beginCurrentTransaction();
        if (!d->anonymousTransaction.isValid()) {
            KisPageStoreDetail::setError(error, QStringLiteral(
                "tiles3 PageStore transaction failed"));
            return {};
        }
    }
    ++d->anonymousClients;
    KisPageStoreDetail::setError(error, {});
    return d->anonymousTransaction;
}

bool KisTiledDataManagerPageStoreBackend::finishOwnedTransaction(
    const KisPageTransaction &transaction,
    QString *error)
{
    QMutexLocker transactionLocker(&d->transactionMutex);
    QWriteLocker publicationLocker(&d->publicationLock);
    KisPageStore *pageStore = nullptr;
    {
        QMutexLocker locker(&d->mutex);
        if (!d->operational || !transaction.isValid() ||
            !(d->anonymousTransaction == transaction) ||
            d->anonymousClients == 0) {
            KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore commit is invalid"));
            return false;
        }
        pageStore = d->store.get();
        --d->anonymousClients;
        if (d->anonymousClients != 0) {
            const bool healthy = !d->anonymousFailed;
            KisPageStoreDetail::setError(error, healthy
                ? QString()
                : QStringLiteral(
                    "tiles3 PageStore anonymous transaction is aborting"));
            return healthy;
        }
        if (d->anonymousFailed) {
            pageStore->abort(transaction);
            d->anonymousTransaction = {};
            d->anonymousFailed = false;
            KisPageStoreDetail::setError(error, QStringLiteral(
                "tiles3 PageStore anonymous transaction was aborted"));
            return false;
        }
        // The last client is about to publish this transaction while holding
        // transactionMutex. Stop exposing its overlay before releasing the
        // backend mutex: commit() removes the transaction from PageStore, and
        // a concurrent reader must never select that stale overlay in the
        // interval between publication and the cleanup below. New writers are
        // still serialized by transactionMutex until publication completes.
        d->anonymousTransaction = {};
        d->anonymousFailed = false;
    }
    if (!stageDerivedExtent(transaction, error)) {
        pageStore->abort(transaction);
        return false;
    }
    const KisPreparedPageSet prepared = pageStore->preparedPages(transaction);
    if (!prepared.isValid()) {
        pageStore->abort(transaction);
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    const KisImageEpochCommitTicket committed =
        pageStore->commit(transaction, prepared);
    if (!committed.isValid()) pageStore->abort(transaction);
    KisPageStoreDetail::setError(error, committed.isValid()
        ? QString() : QStringLiteral("tiles3 PageStore commit failed"));
    return committed.isValid();
}

bool KisTiledDataManagerPageStoreBackend::stageDerivedExtent(
    const KisPageTransaction &transaction,
    QString *error)
{
    KisPageStore *pageStore = store();
    if (!pageStore || !transaction.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 extent transaction is invalid"));
        return false;
    }
    const KisPreparedPageSet prepared = pageStore->preparedPages(transaction);
    if (!prepared.isValid()) {
        // Empty history transactions are valid legacy operations. There is
        // no changed-page delta and therefore no derived extent work.
        KisPageStoreDetail::setError(error, {});
        return true;
    }

    const auto overlay = KisPageReadView::transactionOverlay(transaction.id);
    KisSurfaceEpochState state;
    if (!pageStore->resolveSurfaceState(d->surface, overlay, &state)) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 extent surface is unavailable"));
        return false;
    }
    QRect extent = state.contentExtent;
    const qint64 width = state.logicalPageExtent.width();
    const qint64 height = state.logicalPageExtent.height();

    auto pageRect = [&](const KisPageKey &key, QRect *rect) {
        const KisLogicalPageId page = key.page;
        const qint64 x = qint64(page.column) * width;
        const qint64 y = qint64(page.row) * height;
        if (x < std::numeric_limits<int>::min() ||
            y < std::numeric_limits<int>::min() ||
            x + width - 1 > std::numeric_limits<int>::max() ||
            y + height - 1 > std::numeric_limits<int>::max()) {
            KisPageStoreDetail::setError(error, QStringLiteral("tiles3 extent exceeds QRect range"));
            return false;
        }
        *rect = QRect{int(x), int(y), int(width), int(height)};
        return true;
    };

    const bool removesSurfacePage = std::any_of(
        prepared.removedPages.constBegin(), prepared.removedPages.constEnd(),
        [this](const KisPageKey &key) { return key.surface == d->surface; });
    if (removesSurfacePage) {
        // Persistent subtree bounds contract along changed key paths; a single
        // boundary removal no longer exports/scans every page in the image.
        if (!pageStore->preparedPageExtent(transaction, d->surface, &extent, error)) return false;
    } else {
        // The hot path is monotonic: K changed pages can only keep or expand
        // the current extent, so no manifest export or all-page scan is needed.
        for (const KisPreparedPageProof &proof : prepared.proofs) {
            if (!(proof.authority.version.key.surface == d->surface)) continue;
            QRect rect;
            if (!pageRect(proof.authority.version.key, &rect)) return false;
            extent = extent.isNull() ? rect : extent.united(rect);
        }
    }
    if (state.contentExtent == extent) {
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    state.contentExtent = extent;
    ++state.extentRevision;
    return pageStore->stageSurfaceMetadata(transaction, state, error);
}

bool KisTiledDataManagerPageStoreBackend::pruneDefaultPreparedPages(
    const KisPageTransaction &transaction,
    QString *error)
{
    KisPageStore *pageStore = store();
    if (!pageStore || !transaction.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral(
            "tiles3 default-page pruning transaction is invalid"));
        return false;
    }
    const KisPreparedPageSet prepared = pageStore->preparedPages(transaction);
    if (!prepared.isValid() || prepared.proofs.isEmpty()) {
        KisPageStoreDetail::setError(error, {});
        return true;
    }

    const auto overlay = KisPageReadView::transactionOverlay(transaction.id);
    KisSurfaceEpochState state;
    auto view = pageStore->captureReadView(overlay, error);
    if (!view.isValid() || !view.resolveSurfaceState(d->surface, &state)) {
        KisPageStoreDetail::setError(error, QStringLiteral(
            "tiles3 default-page pruning surface is unavailable"));
        return false;
    }
    const quint32 pixelStride = state.format.pixelStride;
    const QByteArray defaultPixel = state.format.defaultPixel;
    for (const KisPreparedPageProof &proof : prepared.proofs) {
        if (!(proof.authority.version.key.surface == d->surface)) continue;
        KisPageAllocationDescriptor descriptor;
        if (!pageStore->pageDescriptor(proof.authority.version, &descriptor)) {
            KisPageStoreDetail::setError(error, QStringLiteral(
                "tiles3 default-page descriptor is unavailable"));
            return false;
        }
        KisPageStoreReadPage read(pageStore, view, proof.authority.version.key, error);
        const quint8 *bytes = read.data();
        if (!bytes) return false;
        bool isDefault = pixelStride == quint32(defaultPixel.size());
        for (int y = descriptor.validRect.top();
             isDefault && y <= descriptor.validRect.bottom(); ++y) {
            const quint8 *row = bytes + quint64(y) * read.rowStride();
            for (int x = descriptor.validRect.left();
                 x <= descriptor.validRect.right(); ++x) {
                if (std::memcmp(row + quint64(x) * pixelStride,
                                defaultPixel.constData(), pixelStride) != 0) {
                    isDefault = false;
                    break;
                }
            }
        }
        read.reset();
        if (isDefault &&
            !pageStore->stagePageRemovalIfUnchanged(
                transaction, proof.authority.version, error)) {
            return false;
        }
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisTiledDataManagerPageStoreBackend::abortOwnedTransaction(
    const KisPageTransaction &transaction)
{
    QMutexLocker transactionLocker(&d->transactionMutex);
    QWriteLocker publicationLocker(&d->publicationLock);
    QMutexLocker locker(&d->mutex);
    if (!d->operational || !transaction.isValid() ||
        !(d->anonymousTransaction == transaction) ||
        d->anonymousClients == 0) {
        return false;
    }
    d->anonymousFailed = true;
    --d->anonymousClients;
    if (d->anonymousClients != 0) return true;
    const bool aborted = d->store->abort(transaction);
    d->anonymousTransaction = {};
    d->anonymousFailed = false;
    return aborted;
}

void KisTiledDataManagerPageStoreBackend::registerAnonymousLease(
    KisTiledDataManagerPageStoreLease *lease)
{
    QMutexLocker locker(&d->mutex);
    if (lease) d->anonymousLeases.insert(lease);
}

void KisTiledDataManagerPageStoreBackend::unregisterAnonymousLease(
    KisTiledDataManagerPageStoreLease *lease)
{
    QMutexLocker locker(&d->mutex);
    d->anonymousLeases.remove(lease);
}

bool KisTiledDataManagerPageStoreBackend::cancelAnonymousLeasesForBarrier(
    QString *error)
{
    QVector<KisTiledDataManagerPageStoreLease *> leases;
    {
        QMutexLocker locker(&d->mutex);
        leases.reserve(d->anonymousLeases.size());
        for (KisTiledDataManagerPageStoreLease *lease :
             std::as_const(d->anonymousLeases)) {
            leases.append(lease);
        }
    }
    for (KisTiledDataManagerPageStoreLease *lease : std::as_const(leases)) {
        if (!lease->cancelForBarrier()) {
            KisPageStoreDetail::setError(error, QStringLiteral(
                "tiles3 PageStore write barrier cancellation failed"));
            return false;
        }
    }
    QMutexLocker locker(&d->mutex);
    if (d->anonymousTransaction.isValid() || d->anonymousClients != 0 ||
        !d->anonymousLeases.isEmpty()) {
        KisPageStoreDetail::setError(error, QStringLiteral(
            "tiles3 PageStore write barrier still owns clients"));
        return false;
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

QSharedPointer<const KisPageStoreIteratorReadScope>
KisTiledDataManagerPageStoreBackend::captureIteratorReadScope(
    bool writable, QString *error, QSharedPointer<const KisPageStoreIteratorReadScope> existing) const
{
    if (existing) {
        if (existing->m_store == d->store.get() && existing->m_surface == d->surface &&
            existing->m_beforeOnly == writable && (existing->isValid() || existing->observesLiveLegacyWriter())) {
            KisPageStoreDetail::setError(error, {});
            return existing;
        }
        KisPageStoreDetail::setError(error, QStringLiteral("iterator scope belongs to a different owner or access mode"));
        return QSharedPointer<KisPageStoreIteratorReadScope>::create(); // fail closed
    }
    auto result = QSharedPointer<KisPageStoreIteratorReadScope>::create();
    if (!writable && hasCurrentThreadIteratorWrites()) {
        result->m_store = d->store.get();
        result->m_surface = d->surface;
        KisPageStoreDetail::setError(error, {});
        return result;
    }
    result->m_beforeOnly = writable;
    QReadLocker publicationLocker(&d->publicationLock);
    KisPageReadView current, before;
    bool captureBefore = false;
    {
        QMutexLocker locker(&d->mutex);
        if (!d->operational) {
            KisPageStoreDetail::setError(error, QStringLiteral("iterator PageStore is unavailable"));
            return result; // non-null invalid scope: never silently use legacy
        }
        result->m_store = d->store.get();
        result->m_surface = d->surface;
        if (d->activeHistory.isValid()) {
            current = KisPageReadView::transactionOverlay(d->activeHistory.transaction.id);
            before = KisPageReadView::transactionBase(d->activeHistory.transaction.id);
            captureBefore = true;
        } else if (d->anonymousTransaction.isValid() && !d->anonymousFailed) {
            current = KisPageReadView::transactionOverlay(d->anonymousTransaction.id);
        }
    }
    if (!writable) {
        result->m_current = result->m_store->captureReadView(current, error);
        if (!result->m_current.isValid()) {
            result->m_store = nullptr;
            return result;
        }
    }
    if (captureBefore) {
        result->m_before = result->m_store->captureReadView(before, error);
        if (!result->m_before.isValid()) {
            result->m_store = nullptr;
            return result;
        }
    }
    if (result->isValid()) {
        KisPageStoreDetail::setError(error, {});
    } else {
        result->m_store = nullptr;
    }
    return result;
}

KisCapturedReadView KisTiledDataManagerPageStoreBackend::captureReadView(
    bool oldData, QString *error) const
{
    QReadLocker publicationLocker(&d->publicationLock);
    KisPageStore *pageStore = nullptr;
    KisPageReadView selector;
    {
        QMutexLocker locker(&d->mutex);
        if (!d->operational) {
            KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore is unavailable"));
            return {};
        }
        pageStore = d->store.get();
        if (d->activeHistory.isValid()) {
            selector = oldData
                ? KisPageReadView::transactionBase(d->activeHistory.transaction.id)
                : KisPageReadView::transactionOverlay(d->activeHistory.transaction.id);
        } else if (!oldData && d->anonymousTransaction.isValid() && !d->anonymousFailed) {
            selector = KisPageReadView::transactionOverlay(d->anonymousTransaction.id);
        }
    }
    return pageStore->captureReadView(selector, error);
}

bool KisTiledDataManagerPageStoreBackend::hasCurrentThreadIteratorWrites() const
{
    // The explicit iterator mutation scope is also the single same-thread
    // discovery source for unpublished compatibility bytes.
    QMutexLocker locker(&d->mutex);
    const auto batch = d->cpuMutationBatches
                           .value(QThread::currentThreadId()).toStrongRef();
    return batch && batch->iteratorScope && !batch->failed && batch->clients != 0;
}

bool KisTiledDataManagerPageStoreBackend::readBytes(
    quint8 *data, qint32 x, qint32 y, qint32 width, qint32 height,
    qint32 dataRowStride, QString *error) const
{
    if (!data || width <= 0 || height <= 0) { KisPageStoreDetail::setError(error, {}); return true; }
    KisPageStore *pageStore = store();
    using Phase = KisPageStoreDiagnosticPhase;
    KisPageStoreDiagnosticTimer diagnostic(pageStore, Phase::ReadBytesCapture, 1);
    auto view = captureReadView(false, error);
    KisSurfaceEpochState state;
    if (!view.isValid() || !view.resolveSurfaceState(d->surface, &state)) return false;
    const qint64 tightRow = qint64(width) * state.format.pixelStride;
    const qint64 rowStride = dataRowStride > 0 ? dataRowStride : tightRow;
    if (rowStride < tightRow || quint64(rowStride) > quint64(std::numeric_limits<qsizetype>::max()) / quint64(height) ||
        qint64(x) + width - 1 > std::numeric_limits<qint32>::max() ||
        qint64(y) + height - 1 > std::numeric_limits<qint32>::max()) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore read buffer layout is invalid"));
        return false;
    }
    const qint32 pageWidth = state.logicalPageExtent.width();
    const qint32 pageHeight = state.logicalPageExtent.height();
    // The observer for ReadBytesPage runs after capture, with neither the
    // backend publication gate nor owner gate held. It is also a test seam
    // for publication between pages, not a production scheduling callback.
    for (qint64 dy = 0; dy < height;) {
        const qint64 imageY = qint64(y) + dy;
        const qint32 row = pageCoordinate(qint32(imageY), pageHeight);
        const qint64 localY = imageY - qint64(row) * pageHeight;
        const qint64 rows = std::min<qint64>(pageHeight - localY, height - dy);
        for (qint64 dx = 0; dx < width;) {
            diagnostic.next(Phase::ReadBytesPage, 1);
            const qint64 imageX = qint64(x) + dx;
            const qint32 column = pageCoordinate(qint32(imageX), pageWidth);
            const qint64 localX = imageX - qint64(column) * pageWidth;
            const qint64 columns = std::min<qint64>(pageWidth - localX, width - dx);
            const KisPageKey key{d->surface, {column, row}};
            KisPageStoreReadPage read(pageStore, view, key, error);
            const auto *source = read.data();
            if (!source) return false;
            const qsizetype sourceStride = read.rowStride();
            for (qint64 line = 0; line < rows; ++line) {
                std::memcpy(data + (dy + line) * rowStride + dx * state.format.pixelStride,
                    source + (localY + line) * sourceStride + localX * state.format.pixelStride,
                    size_t(columns * state.format.pixelStride));
            }
            dx += columns;
        }
        dy += rows;
    }
    diagnostic.next(Phase::ReadBytesRelease, 1);
    view = {};
    KisPageStoreDetail::setError(error, {});
    return true;
}

std::unique_ptr<KisTilePageStoreLease>
KisTiledDataManagerPageStoreBackend::acquireTile(
    qint32 column,
    qint32 row,
    bool writable,
    bool oldData)
{
    // A read view that names a transaction must remain valid until PageStore
    // has converted it into an active read capability. Readers share this
    // publication gate; only the final publisher/aborter takes it exclusively.
    QReadLocker publicationLocker(
        writable ? nullptr : &d->publicationLock);
    KisPageStore *pageStore = nullptr;
    QSharedPointer<KisTiles3PageReplicaProvider> provider;
    KisPageReadView readView;
    {
        QMutexLocker locker(&d->mutex);
        if (!d->operational ||
            (writable && oldData)) {
            return {};
        }
        pageStore = d->store.get();
        provider = d->provider;
        if (d->activeHistory.isValid()) {
            readView = oldData
                ? KisPageReadView::transactionBase(d->activeHistory.transaction.id)
                : KisPageReadView::transactionOverlay(d->activeHistory.transaction.id);
        } else if (!oldData && d->anonymousTransaction.isValid() &&
                   !d->anonymousFailed) {
            readView = KisPageReadView::transactionOverlay(d->anonymousTransaction.id);
        }
    }

    const KisPageKey key{d->surface, {column, row}};
    if (!writable) {
        auto view = pageStore->captureReadView(readView);
        publicationLocker.unlock();
        KisPageStoreReadPage read(pageStore, view, key, nullptr,
                                  KisPagePriority::Normal, false);
        KisTileData *tileData = tileDataForReadPage(read);
        if (!tileData) return {};
        return std::make_unique<KisTiledDataManagerPageStoreLease>(
            this, tileData, std::move(read));
    }

    KisPageMutationSession *mutation = nullptr;
    QSharedPointer<KisTiledDataManagerPageStoreWriteBatch::Private> batch;
    {
        QMutexLocker lock(&d->mutex);
        batch = d->cpuMutationBatches.value(QThread::currentThreadId()).toStrongRef();
        if (batch && (batch->failed || batch->clients == 0)) return {};
        mutation = batch ? &batch->mutation : nullptr;
    }
    if (mutation) {
        QString failure;
        if (!mutation->reserveLegacyMutationPage(key, &failure)) {
            qWarning() << "PageStore CPU mutation acquisition failed:" << failure;
            return {};
        }
        auto guard = mutation->beginWrite(key, &failure);
        if (!guard.isValid()) {
            qWarning() << "PageStore CPU mutation acquisition failed:" << failure;
            return {};
        }
        KisTileData *tile = provider->tileDataForCpuWriteGuard(guard);
        if (!tile) return {};
        return std::make_unique<KisTiledDataManagerPageStoreLease>(
            this, tile, std::move(guard), batch);
    }

    bool ownsTransaction = false;
    QString transactionError;
    const KisPageTransaction transaction =
        writableTransaction(&ownsTransaction, &transactionError);
    if (!transaction.isValid()) return {};
    auto native = pageStore->beginMutation(transaction, &transactionError);
    if (!native.reserveLegacyMutationPage(key, &transactionError)) {
        native.cancel();
        if (ownsTransaction) abortOwnedTransaction(transaction);
        return {};
    }
    auto guard = native.beginWrite(key, &transactionError);
    KisTileData *tile = guard.isValid() ? provider->tileDataForCpuWriteGuard(guard) : nullptr;
    if (!tile) {
        guard = {};
        native.cancel();
        if (ownsTransaction) abortOwnedTransaction(transaction);
        return {};
    }
    return std::make_unique<KisTiledDataManagerPageStoreLease>(
        this, tile, transaction, ownsTransaction,
        std::move(native), std::move(guard));
}

KisMementoSP KisTiledDataManagerPageStoreBackend::beginHistory(
    const quint8 *defaultPixel,
    quint32 pixelSize,
    QString *error)
{
    QMutexLocker transactionLocker(&d->transactionMutex);
    QMutexLocker locker(&d->mutex);
    if (!d->operational || !d->history || d->activeHistory.isValid() ||
        d->anonymousTransaction.isValid() || d->anonymousClients != 0) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore history is unavailable"));
        return {};
    }
    const KisPageStoreHistoryTransaction transaction =
        d->history->begin(error);
    if (!transaction.isValid()) {
        return {};
    }
    d->activeHistory = transaction;
    d->currentMemento = new KisMemento(nullptr);
    d->currentMemento->saveOldDefaultPixel(defaultPixel, pixelSize);
    return d->currentMemento;
}

bool KisTiledDataManagerPageStoreBackend::commitHistory(
    const quint8 *defaultPixel,
    quint32 pixelSize,
    QString *error)
{
    QMutexLocker transactionLocker(&d->transactionMutex);
    QWriteLocker publicationLocker(&d->publicationLock);
    QMutexLocker locker(&d->mutex);
    if (!d->activeHistory.isValid()) {
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    d->currentMemento->saveNewDefaultPixel(defaultPixel, pixelSize);
    const KisPageTransaction transaction = d->activeHistory.transaction;
    locker.unlock();
    if (!pruneDefaultPreparedPages(transaction, error) ||
        !stageDerivedExtent(transaction, error)) {
        return false;
    }
    locker.relock();
    const KisPageStoreMemento committed =
        d->history->commit(d->activeHistory, error);
    if (!committed.isValid()) return false;
    d->mementos.insert(d->currentMemento.data(), committed);
    d->activeHistory = {};
    d->currentMemento.clear();
    return true;
}

bool KisTiledDataManagerPageStoreBackend::abortHistory(QString *error)
{
    QMutexLocker transactionLocker(&d->transactionMutex);
    QWriteLocker publicationLocker(&d->publicationLock);
    QMutexLocker locker(&d->mutex);
    if (!d->activeHistory.isValid()) {
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    const bool aborted = d->history &&
                         d->history->abort(d->activeHistory, error);
    if (!aborted) return false;
    d->activeHistory = {};
    d->currentMemento.clear();
    return true;
}

bool KisTiledDataManagerPageStoreBackend::rollback(const KisMementoSP &memento, QString *error)
{
    return restoreHistory(memento, true, error);
}

bool KisTiledDataManagerPageStoreBackend::rollforward(const KisMementoSP &memento, QString *error)
{
    return restoreHistory(memento, false, error);
}

bool KisTiledDataManagerPageStoreBackend::restoreHistory(
    const KisMementoSP &memento, bool before, QString *error)
{
    if (!memento) return false;
    QMutexLocker transactionLocker(&d->transactionMutex);
    QMutexLocker locker(&d->mutex);
    const KisPageStoreMemento stored = d->mementos.value(memento.data());
    if (!stored.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore memento is unknown"));
        return false;
    }
    return (before ? d->history->rollback(stored, error)
                   : d->history->rollforward(stored, error)).isValid();
}

bool KisTiledDataManagerPageStoreBackend::purgeHistory(
    const KisMementoSP &memento,
    const quint8 *defaultPixel,
    quint32 pixelSize,
    QString *error)
{
    if (!memento) return false;
    bool isCurrent = false;
    {
        QMutexLocker locker(&d->mutex);
        isCurrent = d->activeHistory.isValid() &&
                    d->currentMemento == memento;
    }
    if (isCurrent && !commitHistory(defaultPixel, pixelSize, error)) {
        return false;
    }
    QMutexLocker transactionLocker(&d->transactionMutex);
    QMutexLocker locker(&d->mutex);
    const KisPageStoreMemento stored = d->mementos.value(memento.data());
    if (!stored.isValid() || !d->history->purge(stored, error)) return false;
    d->mementos.remove(memento.data());
    return true;
}

bool KisTiledDataManagerPageStoreBackend::hasCurrentHistory() const
{
    QMutexLocker locker(&d->mutex);
    return d->activeHistory.isValid();
}

bool KisTiledDataManagerPageStoreBackend::fillRect(
    const QRect &rect,
    const QByteArray &pixel,
    QString *error, QVector<KisLogicalPageId> *changed)
{
    if (changed) changed->clear();
    QVector<KisLogicalPageId> changedPages;
    if (!isOperational() || rect.isEmpty()) return false;
    auto batch = beginMutationBatch(error);
    if (!batch) return false;
    auto before = captureReadView(false, error);
    KisSurfaceEpochState surfaceState;
    if (!before.resolveSurfaceState(d->surface, &surfaceState) ||
        pixel.size() != qsizetype(surfaceState.format.pixelStride)) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 fill surface is invalid"));
        return false;
    }
    QByteArray rowBytes(KisTileData::WIDTH * pixel.size(), Qt::Uninitialized);
    for (int x = 0; x < KisTileData::WIDTH; ++x)
        memcpy(rowBytes.data() + x * pixel.size(), pixel.constData(), size_t(pixel.size()));
    QSharedPointer<const KisPageReplicaSource> uniform;
    const qint32 firstColumn = pageCoordinate(rect.left(), KisTileData::WIDTH);
    const qint32 lastColumn = pageCoordinate(rect.right(), KisTileData::WIDTH);
    const qint32 firstRow = pageCoordinate(rect.top(), KisTileData::HEIGHT);
    const qint32 lastRow = pageCoordinate(rect.bottom(), KisTileData::HEIGHT);
    for (qint64 row = firstRow; row <= lastRow; ++row) {
        for (qint64 column = firstColumn; column <= lastColumn; ++column) {
            KisPageStoreDiagnosticTimer phase(store(), KisPageStoreDiagnosticPhase::FillPage, 1);
            const KisPageKey key{
                d->surface, {qint32(column), qint32(row)}};
            const QRect pageRect(
                int(column * KisTileData::WIDTH),
                int(row * KisTileData::HEIGHT),
                KisTileData::WIDTH, KisTileData::HEIGHT);
            const QRect affected = rect.intersected(pageRect);
            if (affected == pageRect) {
                KisPageVersion old; if (!before.resolvePageVersion(key, &old)) return false;
                if (pixel == surfaceState.format.defaultPixel) {
                    if (old.isDefaultPixel()) continue;
                    if (!batch->replaceFullTile(key.page.column, key.page.row, nullptr, true, error)) return false;
                } else {
                    if (!uniform) uniform = uniformSourceFor(surfaceState.allocationDescriptor(), pixel, error);
                    if (!uniform) return false;
                    auto read = before.readResidentPage(key);
                    if (d->provider->sourceMatchesReadGuard(uniform, read)) continue;
                    if (!batch->d->mutation.reserveLegacyMutationPage(key, error) ||
                        !batch->d->mutation.aliasPage(key, uniform, error)) return false;
                }
                if (changed) changedPages.append(key.page);
                continue;
            }
            auto lease = acquireTile(key.page.column, key.page.row, true, false);
            if (!lease || !lease->writable() || !lease->tileData()) return false;
            auto *bytes = lease->tileData()->data(); if (!bytes) return false;
            const int x = affected.left() - pageRect.left(), y = affected.top() - pageRect.top();
            for (int row = 0; row < affected.height(); ++row)
                memcpy(bytes + (qsizetype(y + row) * KisTileData::WIDTH + x) * pixel.size(),
                       rowBytes.constData(), size_t(affected.width() * pixel.size()));
            lease->markDirty(); if (!lease->finish()) return false;
            if (changed) changedPages.append(key.page);
        }
    }
    before = {};
    const bool success = batch->finish(error);
    if (success && changed) *changed = std::move(changedPages);
    return success;
}

template<typename Operation>
KisPageStoreWriteOperationResult KisTiledDataManagerPageStoreBackend::runCpuMutationOperation(
    const QSet<KisLogicalPageId> &targets, bool legacyIntent, Operation &&operation,
    QVector<KisLogicalPageId> *changed, QString *error)
{
    using Result = KisPageStoreWriteOperationResult;
    if (changed) changed->clear();
    if (!isOperational()) return Result::Unavailable;
    if (targets.isEmpty()) return Result::Succeeded;
    using Phase = KisPageStoreDiagnosticPhase;
    KisPageStoreDiagnosticTimer phase(store(), Phase::PixelOperationRangePrepare, quint64(targets.size()));
    {
        QMutexLocker lock(&d->mutex);
        const auto slot = d->cpuMutationBatches.constFind(
            QThread::currentThreadId());
        if (slot != d->cpuMutationBatches.cend()) {
            const auto existing = slot.value().toStrongRef();
            if (existing && existing->iteratorScope && !existing->failed
                && existing->clients != 0) {
                KisPageStoreDetail::setError(
                    error, QStringLiteral("pixel operation intersects a legacy writer"));
                return Result::Borrowed;
            }
            KisPageStoreDetail::setError(error, QStringLiteral("pixel operation cannot enter an existing mutation batch"));
            return Result::Failed;
        }
    }
    bool storeBorrowed = false;
    auto range = store()->reserveManagedRange(d->surface, targets, legacyIntent,
                                              &storeBorrowed, error);
    if (!range)
        return storeBorrowed ? Result::Borrowed : Result::Failed;
    auto batch = beginMutationBatch(error);
    if (!batch) return Result::Failed;
    if (!batch->d->mutation.adoptReservation(std::move(range), error))
        return Result::Failed;
    const auto selection = KisPageReadView::transactionOverlay(batch->d->transaction.id);
    KisSurfaceEpochState state;
    if (!store()->resolveSurfaceState(surface(), selection, &state) ||
        state.logicalPageExtent != QSize(KisTileData::WIDTH, KisTileData::HEIGHT) ||
        quint64(state.format.pixelStride) * KisTileData::WIDTH > quint64(std::numeric_limits<qint32>::max()))
        return Result::Failed;

    QSet<KisLogicalPageId> touched;
    phase.next(Phase::PixelOperationBody, 1);
    if (!operation(batch->d->mutation, batch->d->transaction, state, touched, error)) return Result::Failed;
    phase.next(Phase::PixelOperationFinish, quint64(touched.size()));
    if (!batch->finish(error)) return Result::Failed;
    if (changed) *changed = touched.values();
    return Result::Succeeded;
}

KisPageStoreWriteOperationResult KisTiledDataManagerPageStoreBackend::writeOperation(
    const QVector<KisLogicalPageId> &pages, bool legacyIntent, const KisPageStorePixelOperation &operation,
    QVector<KisLogicalPageId> *changed, QString *error)
{
    if (!operation) {
        if (changed) changed->clear();
        KisPageStoreDetail::setError(error, QStringLiteral("pixel operation callback is absent"));
        return KisPageStoreWriteOperationResult::Failed;
    }
    const QSet<KisLogicalPageId> targets(pages.cbegin(), pages.cend());
    return runCpuMutationOperation(targets, legacyIntent,
        [&](KisPageMutationSession &mutation, const KisPageTransaction &, const KisSurfaceEpochState &state,
            QSet<KisLogicalPageId> &touched, QString *error) {
    // Expose only the declared target set and poison after the first failed
    // move. A later successful move cannot hide an earlier acquisition failure.
    class BoundedCursor final : public KisPixelWriteCursor {
    public:
        BoundedCursor(KisPageMutationSession &mutation, KisSurfaceId surface, quint32 pixelSize,
                       const QSet<KisLogicalPageId> &targets)
            : mutation(mutation), surface(surface), pixelSize(pixelSize), targets(targets) {}
        void moveTo(qint32 x, qint32 y) override {
            if (failed) return;
            position = {x,y}; data = nullptr;
            const KisLogicalPageId next{pageCoordinate(x, KisTileData::WIDTH), pageCoordinate(y, KisTileData::HEIGHT)};
            if (!guard.isValid() || !(next == current)) {
                guard = {};
                if (!targets.contains(next)) { failed = true; failure = QStringLiteral("pixel operation left its reserved pages"); return; }
                guard = mutation.beginWrite({surface, next}, &failure);
                if (!guard.isValid()) { failed = true; return; }
                current = next; touched.insert(current);
            }
            const quint64 offset = quint64(qint64(y) - qint64(current.row) * KisTileData::HEIGHT) * guard.rowStride() +
                quint64(qint64(x) - qint64(current.column) * KisTileData::WIDTH) * pixelSize;
            if (guard.rowStride() != pixelSize * KisTileData::WIDTH || offset + pixelSize > guard.byteSize()) {
                failed = true; failure = QStringLiteral("pixel operation layout does not match tiles3"); guard = {}; return;
            }
            data = static_cast<quint8 *>(guard.data()) + offset;
        }
        void release() { data = nullptr; guard = {}; }
        quint8 *rawData() override { return failed ? nullptr : data; }
        qint32 numContiguousColumns(qint32 x) const override {
            return qint32((qint64(pageCoordinate(x, KisTileData::WIDTH)) + 1) * KisTileData::WIDTH - x);
        }
        qint32 numContiguousRows(qint32 y) const override {
            return qint32((qint64(pageCoordinate(y, KisTileData::HEIGHT)) + 1) * KisTileData::HEIGHT - y);
        }
        qint32 rowStride(qint32, qint32) const override { return qint32(pixelSize * KisTileData::WIDTH); }
        qint32 x() const override { return position.x(); }
        qint32 y() const override { return position.y(); }
        KisPageMutationSession &mutation;
        KisSurfaceId surface;
        quint32 pixelSize;
        const QSet<KisLogicalPageId> &targets;
        KisLogicalPageId current;
        QPoint position;
        KisCpuWriteGuard guard;
        quint8 *data = nullptr;
        QSet<KisLogicalPageId> touched;
        bool failed = false;
        QString failure;
    } cursor(mutation, surface(), state.format.pixelStride, targets);
    const bool accepted = operation(&cursor);
    cursor.release();
    if (!accepted || cursor.failed) {
        KisPageStoreDetail::setError(error, cursor.failed ? cursor.failure : QStringLiteral("pixel operation callback failed or used invalid coordinates"));
        return false;
    }
    touched = std::move(cursor.touched);
    return true;
    }, changed, error);
}

KisPageStoreWriteOperationResult KisTiledDataManagerPageStoreBackend::writeBytes(
    const quint8 *data, qint32 x, qint32 y, qint32 width, qint32 height,
    qint32 dataRowStride, bool legacyIntent, QVector<KisLogicalPageId> *changed, QString *error)
{
    using Result = KisPageStoreWriteOperationResult;
    if (changed) changed->clear();
    if (!data || width <= 0 || height <= 0) { KisPageStoreDetail::setError(error, {}); return Result::Succeeded; }
    if (qint64(x) + width - 1 > std::numeric_limits<qint32>::max() ||
        qint64(y) + height - 1 > std::numeric_limits<qint32>::max()) {
        KisPageStoreDetail::setError(error, QStringLiteral("packed write coordinates overflow"));
        return Result::Failed;
    }
    const QRect rect(x, y, width, height);
    QVector<KisLogicalPageId> pages;
    for (qint64 row = pageCoordinate(y, KisTileData::HEIGHT);
         row <= pageCoordinate(rect.bottom(), KisTileData::HEIGHT); ++row)
        for (qint64 col = pageCoordinate(x, KisTileData::WIDTH);
             col <= pageCoordinate(rect.right(), KisTileData::WIDTH); ++col)
            pages.append({qint32(col), qint32(row)});

    const QSet<KisLogicalPageId> targets(pages.cbegin(), pages.cend());
    return runCpuMutationOperation(targets, legacyIntent,
        [&](KisPageMutationSession &mutation, const KisPageTransaction &transaction,
            const KisSurfaceEpochState &state, QSet<KisLogicalPageId> &touched, QString *error) {
        const qint64 pixelStride = state.format.pixelStride;
        const qint64 tightRow = qint64(width) * pixelStride;
        const qint64 sourceStride = dataRowStride > 0 ? dataRowStride : tightRow;
        if (sourceStride < tightRow || quint64(sourceStride) >
            quint64(std::numeric_limits<qsizetype>::max()) / quint64(height)) {
            KisPageStoreDetail::setError(error, QStringLiteral("packed write buffer layout is invalid"));
            return false;
        }
        using Phase = KisPageStoreDiagnosticPhase;
        KisPageStoreDiagnosticTimer phase(store(), Phase::PackedWriteCapture, 1);
        const auto selection = KisPageReadView::transactionOverlay(transaction.id);
        // Reservation precedes capture: a no-op decision must not race another
        // backend writer to the same page. One immutable overlay for the body,
        // never a per-page selection of the changing current head.
        auto before = store()->captureReadView(selection, error);
        if (!before.isValid()) return false;
        for (const auto &page : pages) {
            const KisPageKey key{surface(), page};
            const QRect pageRect(page.column * KisTileData::WIDTH, page.row * KisTileData::HEIGHT,
                                 KisTileData::WIDTH, KisTileData::HEIGHT);
            const QRect affected = rect.intersected(pageRect);
            const qsizetype localX = qint64(affected.x()) - pageRect.x();
            const qsizetype localY = qint64(affected.y()) - pageRect.y();
            const quint8 *input = data + (qint64(affected.y()) - y) * sourceStride +
                                        (qint64(affected.x()) - x) * pixelStride;
            const size_t lineBytes = size_t(affected.width() * pixelStride);
            phase.next(Phase::PackedWriteCompare, 1);
            bool unchanged = true;
            {
                KisPageStoreReadPage read(store(), before, key, error);
                if (!read.data()) return false;
                for (int row = 0; row < affected.height(); ++row) {
                    if (std::memcmp(read.data() + (localY + row) * read.rowStride() + localX * pixelStride,
                                    input + row * sourceStride, lineBytes)) {
                        unchanged = false;
                        break;
                    }
                }
            } // no read pin overlaps writable acquisition
            if (unchanged) continue;
            phase.next(Phase::PackedWritePrepare, 1);
            if (affected == pageRect) {
                // The input rows initialize fresh backing before it is visible
                // even to the swapper. No default-fill then overwrite, and no
                // uninitialized writable capability is handed to the caller.
                const KisCpuPagePayload payload{input, qsizetype(sourceStride),
                    qsizetype((affected.height() - 1) * sourceStride + lineBytes)};
                if (!mutation.overwritePage(key, payload, error)) return false;
                touched.insert(page);
                continue;
            }
            auto write = mutation.beginWrite(key, KisPageWriteMode::PreserveContents, error);
            if (!write.isValid()) return false;
            phase.next(Phase::PackedWriteCopy, 1);
            auto *output = static_cast<quint8 *>(write.data());
            for (int row = 0; row < affected.height(); ++row)
                std::memcpy(output + (localY + row) * write.rowStride() + localX * pixelStride,
                            input + row * sourceStride, lineBytes);
            phase.next(Phase::PackedWriteRelease, 1);
            write = {};
            touched.insert(page);
        }
        phase.next(Phase::PackedWriteRelease, 1);
        before = {};
        return true;
    }, changed, error);
}

QSharedPointer<const KisPageReplicaSource> KisTiledDataManagerPageStoreBackend::uniformSourceFor(
    const KisPageAllocationDescriptor &descriptor, const QByteArray &pixel, QString *error)
{
    {
        QMutexLocker lock(&d->mutex);
        if (d->uniformSource && d->uniformPixel == pixel && d->uniformSource->descriptor() == descriptor)
            return d->uniformSource;
    }
    auto *tile = KisTileDataStore::instance()->createDefaultTileData(pixel.size(),
        reinterpret_cast<const quint8 *>(pixel.constData()));
    if (!tile) return {};
    tile->acquire();
    auto source = d->provider->captureCompletedTileSource(descriptor, tile, error);
    tile->release();
    if (!source) return {};
    QMutexLocker lock(&d->mutex);
    d->uniformPixel = pixel; d->uniformSource = source;
    return source;
}

bool KisTiledDataManagerPageStoreBackend::copyFrom(
    const KisTiledDataManagerPageStoreBackend &source, const QRect &rect,
    bool oldSource, bool rough, QString *error, QVector<KisLogicalPageId> *changed)
{
    if (changed) changed->clear();
    QVector<KisLogicalPageId> changedPages;
    if (!isOperational() || !source.isOperational() || rect.isEmpty()) return false;
    KisPageStoreDiagnosticTimer phase(store(), KisPageStoreDiagnosticPhase::CopyPrepare, 1);
    // Capture both visibility selections once, before any destination write.
    auto sourceView = source.captureReadView(oldSource, error);
    auto targetView = captureReadView(false, error);
    KisSurfaceEpochState sourceState, targetState;
    if (!sourceView.resolveSurfaceState(source.surface(), &sourceState) ||
        !targetView.resolveSurfaceState(surface(), &targetState) ||
        sourceState.format.pixelStride != targetState.format.pixelStride) return false;
    auto batch = beginMutationBatch(error);
    if (!batch) return false;
    const auto pixelSize = qsizetype(targetState.format.pixelStride);
    const qsizetype rowStride = KisTileData::WIDTH * pixelSize;
    for (qint64 row = pageCoordinate(rect.top(), KisTileData::HEIGHT);
         row <= pageCoordinate(rect.bottom(), KisTileData::HEIGHT); ++row) {
        for (qint64 column = pageCoordinate(rect.left(), KisTileData::WIDTH);
             column <= pageCoordinate(rect.right(), KisTileData::WIDTH); ++column) {
            phase.next(KisPageStoreDiagnosticPhase::CopySourcePage, 1);
            const KisPageKey from{source.surface(), {qint32(column), qint32(row)}};
            const KisPageKey to{surface(), from.page};
            const QRect pageRect(int(column * KisTileData::WIDTH), int(row * KisTileData::HEIGHT),
                                 KisTileData::WIDTH, KisTileData::HEIGHT);
            const auto affected = rough ? pageRect : rect.intersected(pageRect);
            KisPageVersion sourceVersion, targetVersion;
            if (!sourceView.resolvePageVersion(from, &sourceVersion) ||
                !targetView.resolvePageVersion(to, &targetVersion)) return false;
            if (source.store() == store() && sourceVersion == targetVersion) continue;
            if (affected == pageRect && sourceVersion.isDefaultPixel()) {
                if (sourceState.format.defaultPixel == targetState.format.defaultPixel) {
                    if (targetVersion.isDefaultPixel()) continue;
                    if (!batch->replaceFullTile(to.page.column, to.page.row, nullptr, true, error)) return false;
                } else {
                    // Use the frozen default value directly; building a second
                    // shared default read buffer would duplicate initialization.
                    auto backing = uniformSourceFor(targetState.allocationDescriptor(), sourceState.format.defaultPixel, error);
                    auto previous = targetView.readResidentPage(to);
                    if (backing && d->provider->sourceMatchesReadGuard(backing, previous)) continue;
                    if (!backing || !batch->d->mutation.reserveLegacyMutationPage(to, error) ||
                        !batch->d->mutation.aliasPage(to, backing, error)) return false;
                }
                if (changed) changedPages.append(to.page);
                continue;
            }
            KisPageStoreReadPage input(source.store(), sourceView, from, error);
            if (!input.data() || input.rowStride() != rowStride) return false;
            if (affected == pageRect) {
                // A full replacement never needs the old destination bytes.
                // Opportunistic resident identity can prove a no-op; a cold
                // target must not be materialized just to compare payloads.
                auto previous = targetView.readResidentPage(to);
                if (!sourceVersion.isDefaultPixel() && !targetVersion.isDefaultPixel() &&
                    previous.isValid() && input.data() == previous.data()) continue;
                QSharedPointer<const KisPageReplicaSource> backing;
                if (input.nativeGuard().isValid())
                    backing = d->provider->captureCpuReadSource(input.nativeGuard(), targetState.allocationDescriptor(), error);
                else if (input.genericLease())
                    backing = d->provider->captureCompletedTileSource(targetState.allocationDescriptor(),
                        source.d->provider->tileDataForLease(input.genericLease()->leaseId()), error);
                if (backing && d->provider->sourceMatchesReadGuard(backing, previous)) continue;
                if (!backing || !batch->d->mutation.reserveLegacyMutationPage(to, error) ||
                    !batch->d->mutation.aliasPage(to, backing, error)) return false;
                if (changed) changedPages.append(to.page);
                continue;
            }
            auto lease = acquireTile(to.page.column, to.page.row, true, false);
            if (!lease || !lease->writable() || !lease->tileData()) return false;
            auto *bytes = lease->tileData()->data(); if (!bytes) return false;
            const qsizetype offset = (qsizetype(affected.top() - pageRect.top()) * KisTileData::WIDTH +
                                       affected.left() - pageRect.left()) * pixelSize;
            for (int y = 0; y < affected.height(); ++y)
                memcpy(bytes + offset + y * rowStride, input.data() + offset + y * rowStride,
                       size_t(affected.width() * pixelSize));
            lease->markDirty(); if (!lease->finish()) return false;
            if (changed) changedPages.append(to.page);
        }
    }
    phase.next(KisPageStoreDiagnosticPhase::CopyFinish, 1);
    sourceView = {}; targetView = {};
    const bool success = batch->finish(error);
    if (success && changed) *changed = std::move(changedPages);
    return success;
}

KisTileData *KisTiledDataManagerPageStoreBackend::tileDataForReadPage(const KisPageStoreReadPage &read) const
{
    return read.nativeGuard().isValid() ? d->provider->tileDataForCpuReadGuard(read.nativeGuard())
        : read.genericLease() ? d->provider->tileDataForLease(read.genericLease()->leaseId()) : nullptr;
}

bool KisTiledDataManagerPageStoreBackend::setDefaultPixel(
    const QByteArray &pixel,
    QString *error)
{
    bool owned = false;
    const KisPageTransaction transaction = writableTransaction(&owned, error);
    KisPageStore *pageStore = store();
    if (!transaction.isValid() || !pageStore) return false;

    const auto overlay = KisPageReadView::transactionOverlay(transaction.id);
    KisSurfaceEpochState state;
    if (!pageStore->resolveSurfaceState(d->surface, overlay, &state) ||
        pixel.size() != qsizetype(state.format.pixelStride)) {
        if (owned) abortOwnedTransaction(transaction);
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore default pixel is invalid"));
        return false;
    }
    if (state.format.defaultPixel == pixel) {
        return !owned || finishOwnedTransaction(transaction, error);
    }
    if (!pageStore->stageSurfaceDefaultPixel(transaction, d->surface, pixel, error)) {
        if (owned) abortOwnedTransaction(transaction);
        return false;
    }
    return !owned || finishOwnedTransaction(transaction, error);
}

bool KisTiledDataManagerPageStoreBackend::clearAll(QString *error)
{
    // A legacy tile caller may retain a raw writable pointer across clear().
    // clearAll is the sequencing barrier that cancels its anonymous
    // mutation before creating the post-clear transaction.
    if (!cancelAnonymousLeasesForBarrier(error)) return false;
    return removePages(allocatedPages(), error);
}

bool KisTiledDataManagerPageStoreBackend::removePages(
    const QVector<KisLogicalPageId> &pages,
    QString *error)
{
    if (pages.isEmpty()) { KisPageStoreDetail::setError(error, {}); return isOperational(); }
    auto batch = beginMutationBatch(error);
    if (!batch) return false;
    for (const KisLogicalPageId &page : pages) {
        if (!batch->replaceFullTile(page.column, page.row, nullptr, true, error))
            return false;
    }
    return batch->finish(error);
}

bool KisTiledDataManagerPageStoreBackend::trimToRect(
    const QRect &rect,
    QString *error)
{
    {
        QMutexLocker lock(&d->mutex);
        if (d->cpuMutationBatches.contains(QThread::currentThreadId())) {
            // Reject before joining the anonymous transaction: cancelling a
            // newly joined client would also poison its still-live parent.
            KisPageStoreDetail::setError(error, QStringLiteral("trim cannot nest inside a native CPU batch"));
            return false;
        }
    }
    auto batch = beginMutationBatch(error);
    KisPageStore *pageStore = store();
    if (!batch || !pageStore) return false;

    struct TrimPage {
        KisLogicalPageId page;
        QRect extent;
        QRect kept;
    };
    QVector<TrimPage> changes;
    const auto pages = allocatedPages();
    for (const auto &page : pages) {
        const qint64 x = qint64(page.column) * KisTileData::WIDTH;
        const qint64 y = qint64(page.row) * KisTileData::HEIGHT;
        if (x < std::numeric_limits<int>::min() || y < std::numeric_limits<int>::min() ||
            x + KisTileData::WIDTH - 1 > std::numeric_limits<int>::max() ||
            y + KisTileData::HEIGHT - 1 > std::numeric_limits<int>::max()) {
            KisPageStoreDetail::setError(error, QStringLiteral("tiles3 trim extent exceeds coordinate range"));
            return false;
        }
        const QRect extent(int(x), int(y), KisTileData::WIDTH, KisTileData::HEIGHT);
        const auto kept = extent.intersected(rect);
        if (kept == extent) continue;
        changes.append({page, extent, kept});
    }
    const auto transaction = batch->d->transaction;
    const auto overlay = KisPageReadView::transactionOverlay(transaction.id);
    KisSurfaceEpochState state;
    if (!pageStore->resolveSurfaceState(d->surface, overlay, &state) ||
        state.logicalPageExtent != QSize(KisTileData::WIDTH, KisTileData::HEIGHT)) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 trim surface is unavailable or incompatible"));
        return false;
    }
    std::sort(changes.begin(), changes.end(), [](const TrimPage &a, const TrimPage &b) {
        return a.page.row == b.page.row ? a.page.column < b.page.column : a.page.row < b.page.row;
    });
    const auto &pixel = state.format.defaultPixel;
    const qsizetype stride = qsizetype(KisTileData::WIDTH) * pixel.size();
    for (const auto &change : changes) {
        if (change.kept.isEmpty()) {
            if (!batch->replaceFullTile(change.page.column, change.page.row, nullptr, true, error)) return false;
            continue;
        }
        auto lease = acquireTile(change.page.column, change.page.row, true, false);
        if (!lease || !lease->writable() || !lease->tileData()) {
            KisPageStoreDetail::setError(error, QStringLiteral("tiles3 trim writable page acquisition failed"));
            return false;
        }
        quint8 *data = lease->tileData()->data();
        if (!data || lease->tileData()->pixelSize() != pixel.size()) {
            KisPageStoreDetail::setError(error, QStringLiteral("tiles3 trim writable page layout is invalid"));
            return false;
        }
        const int left = change.kept.left() - change.extent.left();
        const int top = change.kept.top() - change.extent.top();
        const int right = left + change.kept.width();
        const int bottom = top + change.kept.height();
        const auto fill = [&](int row, int column, int count) {
            quint8 *target = data + row * stride + column * pixel.size();
            if (pixel.size() == 1) memset(target, quint8(pixel[0]), size_t(count));
            else for (int i = 0; i < count; ++i)
                memcpy(target + i * pixel.size(), pixel.constData(), size_t(pixel.size()));
        };
        // All bands of one partial page share a single first-write backing.
        // The lease protects bytes, not the compatibility hash or an old view.
        for (int row = 0; row < KisTileData::HEIGHT; ++row) {
            if (row < top || row >= bottom) fill(row, 0, KisTileData::WIDTH);
            else {
                fill(row, 0, left);
                fill(row, right, KisTileData::WIDTH - right);
            }
        }
        lease->markDirty();
        if (!lease->finish()) {
            KisPageStoreDetail::setError(error, QStringLiteral("tiles3 trim writable page release failed"));
            return false;
        }
    }
    return batch->finish(error);
}

QVector<KisLogicalPageId>
KisTiledDataManagerPageStoreBackend::allocatedPages() const
{
    QReadLocker publicationLocker(&d->publicationLock);
    KisPageStore *pageStore = nullptr;
    KisPageTransaction overlayTransaction;
    {
        QMutexLocker locker(&d->mutex);
        if (!d->operational) return {};
        pageStore = d->store.get();
        overlayTransaction = d->activeHistory.isValid()
            ? d->activeHistory.transaction
            : (!d->anonymousFailed
                ? d->anonymousTransaction : KisPageTransaction());
    }

    QSet<KisPageKey> visiblePages;
    const KisImageEpochSnapshot snapshot = pageStore->captureCommittedEpoch();
    for (const KisPageVersion &version : snapshot.manifest) {
        if (version.key.surface == d->surface) {
            visiblePages.insert(version.key);
        }
    }
    if (overlayTransaction.isValid()) {
        const KisPreparedPageSet prepared =
            pageStore->preparedPages(overlayTransaction);
        if (prepared.isValid()) {
            for (const KisPageKey &key : prepared.removedPages) {
                if (key.surface == d->surface) visiblePages.remove(key);
            }
            for (const KisPreparedPageProof &proof : prepared.proofs) {
                if (proof.authority.version.key.surface == d->surface) {
                    visiblePages.insert(proof.authority.version.key);
                }
            }
        }
    }
    QVector<KisLogicalPageId> pages;
    pages.reserve(visiblePages.size());
    for (const KisPageKey &key : std::as_const(visiblePages)) {
        pages.append(key.page);
    }
    std::sort(pages.begin(), pages.end(),
              [](const KisLogicalPageId &lhs, const KisLogicalPageId &rhs) {
                  return lhs.row != rhs.row ? lhs.row < rhs.row
                                            : lhs.column < rhs.column;
              });
    return pages;
}

bool KisTiledDataManagerPageStoreBackend::pageAllocated(
    qint32 column,
    qint32 row,
    bool oldData) const
{
    QReadLocker publicationLocker(&d->publicationLock);
    KisPageStore *pageStore = nullptr;
    KisPageReadView view;
    {
        QMutexLocker locker(&d->mutex);
        if (!d->operational) return false;
        pageStore = d->store.get();
        if (d->activeHistory.isValid()) {
            view = oldData
                ? KisPageReadView::transactionBase(d->activeHistory.transaction.id)
                : KisPageReadView::transactionOverlay(d->activeHistory.transaction.id);
        } else if (!oldData && d->anonymousTransaction.isValid() &&
                   !d->anonymousFailed) {
            view = KisPageReadView::transactionOverlay(d->anonymousTransaction.id);
        }
    }
    KisPageVersion version;
    return pageStore->resolvePageVersion(
               {d->surface, {column, row}}, view, &version) &&
           version.isValid() && !version.isDefaultPixel();
}

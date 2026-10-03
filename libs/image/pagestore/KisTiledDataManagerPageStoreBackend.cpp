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
#include <QThread>
#include <QScopeGuard>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <map>
#include <utility>

namespace {

template<class Key, class Value>
using StorageMap = std::map<Key, Value, std::less<Key>,
    KisMutationStorageAllocator<std::pair<const Key, Value>>>;

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

void prepareChangedPageOutput(QVector<KisLogicalPageId> *output, const QRect &rect)
{
    if (!output) return;
    const auto columns = quint64(qint64(pageCoordinate(rect.right(), KisTileData::WIDTH)) -
        pageCoordinate(rect.left(), KisTileData::WIDTH) + 1);
    const auto rows = quint64(qint64(pageCoordinate(rect.bottom(), KisTileData::HEIGHT)) -
        pageCoordinate(rect.top(), KisTileData::HEIGHT) + 1);
    if (columns > quint64(std::numeric_limits<qsizetype>::max()) / rows) throw std::bad_alloc();
    output->reserve(qsizetype(columns * rows));
    if (quint64(output->capacity()) < columns * rows) throw std::bad_alloc();
}

bool configMiBToBytes(int value, quint64 *bytes,
                      QString *error)
{
    constexpr quint64 bytesPerMiB = quint64(1) << 20;
    if (!bytes || value < 0
        || quint64(value) > std::numeric_limits<quint64>::max() / bytesPerMiB) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("PageStore configured memory limit is invalid"));
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
    if (!configMiBToBytes(config.tilesHardLimit(), &ramBytes, error)
        || !configMiBToBytes(config.poolLimit(), &poolBytes, error)
        || !configMiBToBytes(config.maxSwapSize(), &swapBytes, error)) {
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

std::shared_ptr<KisBackingBudgetController> acquireProductNonPayloadBudget(
    const ProductBackingPolicy &policy,
    QString *error)
{
    return kisAcquirePageStoreProcessBudget(policy.limits.metadataArenaBytes, error);
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

class KisTiledDataManagerPageStoreLease final : public KisTilePageStoreLease, public KisPageProcessStorageObject
{
public:
    KisTiledDataManagerPageStoreLease(
        KisTiledDataManagerPageStoreBackend *backend,
        KisTileData *tile,
        KisCpuWriteGuard &&guard,
        std::shared_ptr<KisTiledDataManagerPageStoreWriteBatch::Private> batch);
    KisTiledDataManagerPageStoreLease(
        KisTiledDataManagerPageStoreBackend *backend,
        KisTileData *tile, const KisPageTransaction &transaction,
        bool ownsTransaction, KisPageMutationSession &&mutation, KisCpuWriteGuard &&guard,
        TileLease readCache)
        : m_backend(backend), m_tileData(tile),
          m_transaction(ownsTransaction ? transaction : KisPageTransaction{}),
          m_readCache(std::move(readCache)),
          m_native(std::move(guard)),
          m_ownedMutation(std::move(mutation))
    {
        if (m_transaction.isValid()) m_backend->registerAnonymousLease(this);
    }
    ~KisTiledDataManagerPageStoreLease() override
    {
        finish();
    }

    KisTileData *tileData() const override { return m_tileData; }
    bool writable() const override { return m_native.isValid(); }
    void markDirty() override { m_dirty = true; }
    TileLease takeReadCache() override
    {
        Q_ASSERT(!m_backend && !m_native.isValid());
        return std::move(m_readCache);
    }

    bool finishBatchLease();
    bool finish() override
    {
        if (m_batch) return finishBatchLease();
        releaseBarrierPin();
        if (!m_backend) return true;
        if (m_ownedMutation.isActive()) {
            // Compatibility unlock is an existing visibility boundary. Keep
            // it, but perform first-write/prepare/seal through the same native
            // mutation implementation as explicit operation-wide batches.
            m_native = {};
            const bool success = m_dirty ? m_ownedMutation.sealForLegacyUnlock() : m_ownedMutation.cancel();
            m_ownedMutation = {};
            if (!success) {
                m_readCache.reset();
                if (m_transaction.isValid() && m_backend) m_backend->abortOwnedTransaction(m_transaction);
                return complete(false);
            }
            const bool committed = !m_transaction.isValid() ||
                (m_backend && m_backend->finishOwnedTransaction(m_transaction));
            if (!committed || !m_dirty) m_readCache.reset();
            return complete(committed);
        }
        return complete(false);
    }

    bool cancelForBarrier()
    {
        if (!m_backend || !m_ownedMutation.isActive() || !m_transaction.isValid()) {
            return false;
        }
        if (!retainBarrierPin()) return false;
        m_native = {};
        if (!m_ownedMutation.cancel()) return false;
        m_ownedMutation = {};
        m_readCache.reset();
        return complete(m_backend->abortOwnedTransaction(m_transaction));
    }

    bool belongsToCurrentThread() const
    {
        return m_thread == QThread::currentThreadId();
    }

private:
    friend class KisTiledDataManagerPageStoreBackend;
    friend class KisTiledDataManagerPageStoreWriteBatch::Private;
    // Only the cold clear barrier needs a second residency pin. The native
    // claim can then end while the legacy raw pointer remains valid until
    // its original tile unlock. A tile reference alone does not prevent swap.
    bool retainBarrierPin()
    {
        if (!m_barrierPin && m_tileData->tryBlockSwapping()) {
            m_tileData->ref();
            m_barrierPin = true;
        }
        return m_barrierPin;
    }
    void releaseBarrierPin()
    {
        if (std::exchange(m_barrierPin, false)) {
            m_tileData->unblockSwapping();
            m_tileData->deref();
        }
    }
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
    bool m_barrierPin = false;
    Qt::HANDLE m_thread = QThread::currentThreadId();
    KisTiledDataManagerPageStoreLease *m_previous = nullptr;
    KisTiledDataManagerPageStoreLease *m_next = nullptr;
    std::shared_ptr<KisTiledDataManagerPageStoreWriteBatch::Private> m_batch;
    TileLease m_readCache;
    KisCpuWriteGuard m_native;
    KisPageMutationSession m_ownedMutation;
};

class KisTiledDataManagerPageStoreBackend::Private : public KisPageProcessStorageObject
{
public:
    bool prepareStore(const KisImageEpochSnapshot &initial, QString *error)
    try
    {
        ProductBackingPolicy policy;
        if (!deriveProductBackingPolicy(&policy, error))
            return false;
        const auto sharedNonPayloadBudget = acquireProductNonPayloadBudget(policy, error);
        if (!sharedNonPayloadBudget)
            return false;
        KisBackingBudgetController coldStorage;
        if (!coldStorage.configureSharedNonPayloadBudget(sharedNonPayloadBudget, error)) return false;
        const auto storage = KisMutationStorageAllocator<char>::retained(&coldStorage);
        const auto completions = std::allocate_shared<KisCompletionRegistry>(storage, sharedNonPayloadBudget, storage);
        provider = std::allocate_shared<KisTiles3PageReplicaProvider>(storage, storage);
        KisCpuResidentReplicaProviderConfig providerConfig;
        providerConfig.provider =
            KisPageStoreDetail::allocateMonotonicId<KisReplicaProviderId>(
                &nextTiles3ProviderId);
        providerConfig.providerEpoch = KisReplicaProviderEpoch{1};
        providerConfig.budgetBytes = policy.providerBytes;
        store = KisPageStore::prepareStorage(sharedNonPayloadBudget, error);
        if (!store) return false;
        history = std::allocate_shared<KisPageStoreMementoManager>(storage, storage);
        return provider->configure(providerConfig, completions, error, sharedNonPayloadBudget) &&
               store->configureSharedNonPayloadBudget(sharedNonPayloadBudget, error) &&
               store->configureBackingLimits(policy.limits, error) &&
               store->configure(initial, completions, 64, error) &&
               store->configureDerivedPageExtent(surface) &&
               store->registerReplicaProvider(provider);
    }
    catch (const std::bad_alloc &)
    {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore root storage preparation was refused"));
        return false;
    }

    bool finalizeStore(QString *error)
    {
        return store->finalizeInitialization(error) &&
               history->configure(store.get(), error);
    }

    // Caller holds mutex. This only selects metadata: the caller's existing
    // publication protection must keep a transaction alive until read capture
    // or resolution completes. It neither acquires a capability nor a lock.
    KisPageReadView selectReadViewLocked(bool oldData) const
    {
        if (activeHistory.isValid()) {
            return oldData
                ? KisPageReadView::transactionBase(activeHistory.transaction.id)
                : KisPageReadView::transactionOverlay(activeHistory.transaction.id);
        }
        if (!oldData && anonymousTransaction.isValid() && !anonymousFailed) {
            return KisPageReadView::transactionOverlay(anonymousTransaction.id);
        }
        return {};
    }

    mutable QMutex mutex;
    QMutex transactionMutex;
    QReadWriteLock publicationLock;
    std::shared_ptr<KisTiles3PageReplicaProvider> provider;
    KisPageStore::StoragePointer store{nullptr, &KisPageStore::destroyStorage};
    std::shared_ptr<KisPageStoreMementoManager> history;
    KisSurfaceId surface{1};
    KisPageTransaction anonymousTransaction;
    quint64 anonymousClients = 0;
    bool anonymousFailed = false;
    KisPageStoreHistoryTransaction activeHistory;
    // Business closing phase belongs to this existing history owner. Pixel
    // lifetime/failure, entries and claims remain in the original session.
    enum class HistoryMutationPhase { None, Open, Closing, Sealed };
    HistoryMutationPhase historyMutationPhase = HistoryMutationPhase::None;
    KisPageMutationSession historyMutation;
    KisMementoSP currentMemento;
    StorageMap<const KisMemento *, KisPageStoreMemento> mementos;
    KisTiledDataManagerPageStoreLease *anonymousLeases = nullptr;
    StorageMap<Qt::HANDLE, std::weak_ptr<KisTiledDataManagerPageStoreWriteBatch::Private>> cpuMutationBatches;
    // Configuration is published once and the backend/store lifetime is
    // owned by KisTiledDataManager.  Tile lookup is a production hot path,
    // so readers must not serialize on the control-plane mutex merely to
    // discover that PageStore is enabled.
    std::atomic_bool operational{false};
    KisPageByteArray uniformPixel;
    std::shared_ptr<const KisPageReplicaSource> uniformSource;
    const std::shared_ptr<const KisPageStoreIteratorReadScope> failedReadScope =
        std::allocate_shared<KisPageStoreIteratorReadScope>(
            KisMutationStorageAllocator<KisPageStoreIteratorReadScope>{});
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
    bool finishClient(bool succeeded, QString *error = nullptr, bool retainAdmission = false);
    bool finish(QString *error = nullptr, bool retainAdmission = false);
    bool cancelForBarrier(QString *error);
    void unlinkLease(KisTiledDataManagerPageStoreLease *lease)
    {
        if (lease->m_previous) lease->m_previous->m_next = lease->m_next;
        else leases = lease->m_next;
        if (lease->m_next) lease->m_next->m_previous = lease->m_previous;
        lease->m_previous = lease->m_next = nullptr;
    }
    // Live capabilities only, not a second page/write set. Intrusive links
    // allocate nothing; the local gate also covers a foreign final unlock.
    QMutex leaseMutex;
    KisTiledDataManagerPageStoreLease *leases = nullptr;
    KisTiledDataManagerPageStoreBackend *backend = nullptr;
    KisPageTransaction transaction;
    bool owned = false;
    bool failed = false;
    bool iteratorScope = false;
    quint64 clients = 1;
    Qt::HANDLE thread = QThread::currentThreadId();
    KisPageMutationSession mutation;
    StorageMap<KisTileData *, std::shared_ptr<const KisPageReplicaSource>> sources;
};

KisTiledDataManagerPageStoreLease::KisTiledDataManagerPageStoreLease(
    KisTiledDataManagerPageStoreBackend *backend, KisTileData *tile,
    KisCpuWriteGuard &&guard,
    std::shared_ptr<KisTiledDataManagerPageStoreWriteBatch::Private> batch)
    : m_backend(backend), m_tileData(tile), m_batch(std::move(batch)),
      m_native(std::move(guard))
{
    QMutexLocker lock(&m_batch->leaseMutex);
    m_next = m_batch->leases;
    if (m_next) m_next->m_previous = this;
    m_batch->leases = this;
}

bool KisTiledDataManagerPageStoreLease::finishBatchLease()
{
    auto batch = std::move(m_batch);
    {
        QMutexLocker lock(&batch->leaseMutex);
        // Return the actual RAM pin. Outside a clear barrier, the segment
        // keeps its parked writer reservation until its explicit end.
        m_native = {};
        releaseBarrierPin();
        batch->unlinkLease(this);
        m_backend = nullptr;
    }
    // The final shared holder may finish/cancel the abandoned batch. Release
    // it outside the capability gate and after returning the native guard.
    return true;
}

bool KisTiledDataManagerPageStoreWriteBatch::Private::cancelForBarrier(QString *error)
{
    QMutexLocker leaseLock(&leaseMutex);
    if (!backend || !iteratorScope || thread != QThread::currentThreadId()) return false;
    for (auto *lease = leases; lease; lease = lease->m_next) {
        if (!lease->retainBarrierPin()) {
            for (auto *pinned = leases; pinned != lease; pinned = pinned->m_next)
                pinned->releaseBarrierPin();
            KisPageStoreDetail::setError(error, QStringLiteral("clear cannot retain legacy iterator storage"));
            return false;
        }
    }
    for (auto *lease = leases; lease; lease = lease->m_next) lease->m_native = {};
    if (!mutation.cancel()) {
        failed = true;
        KisPageStoreDetail::setError(error, QStringLiteral("clear cannot cancel the iterator mutation"));
        return false;
    }
    if (owned && !backend->abortOwnedTransaction(transaction)) {
        failed = true;
        return false;
    }
    sources.clear();
    QMutexLocker lock(&backend->d->mutex);
    const auto slot = backend->d->cpuMutationBatches.find(thread);
    if (slot != backend->d->cpuMutationBatches.end() && slot->second.lock().get() == this)
        backend->d->cpuMutationBatches.erase(slot);
    backend = nullptr;
    clients = 0;
    // A successful clear supersedes these clients. Their late finish is a
    // successful no-op, and cannot remove or publish a newer thread batch.
    failed = false;
    return true;
}

class KisTiledDataManagerIteratorWriteScope::Private : public KisPageProcessStorageObject
{
public:
    std::unique_ptr<KisTiledDataManagerPageStoreWriteBatch> batch;
};

KisTiledDataManagerIteratorWriteScope::KisTiledDataManagerIteratorWriteScope()
    : d(new Private)
{
}

KisTiledDataManagerIteratorWriteScope::~KisTiledDataManagerIteratorWriteScope() = default;

void *KisTiledDataManagerIteratorWriteScope::operator new(size_t bytes)
{
    return KisPageProcessStorageObject::operator new(bytes);
}
void KisTiledDataManagerIteratorWriteScope::operator delete(void *data) noexcept
{
    KisPageProcessStorageObject::operator delete(data);
}

bool KisTiledDataManagerIteratorWriteScope::finish()
{
    return d && d->batch && d->batch->finish();
}

bool KisTiledDataManagerIteratorWriteScope::isActive() const
{
    if (!d || !d->batch) return false;
    const auto &batch = d->batch->d;
    QMutexLocker lock(&batch->leaseMutex);
    return batch->backend && !batch->failed && batch->clients != 0;
}

KisTiledDataManagerPageStoreWriteBatch::
KisTiledDataManagerPageStoreWriteBatch(
    KisTiledDataManagerPageStoreBackend *backend,
    const KisPageTransaction &transaction,
    bool owned)
    : d(std::allocate_shared<Private>(KisMutationStorageAllocator<Private>{}))
{
    d->backend = backend;
    d->transaction = transaction;
    d->owned = owned;
}

KisTiledDataManagerPageStoreWriteBatch::
KisTiledDataManagerPageStoreWriteBatch(std::shared_ptr<Private> shared)
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

bool KisTiledDataManagerPageStoreWriteBatch::finishForAdapterDelivery(QString *error)
{
    if (m_clientFinished || !d) return false;
    m_clientFinished = true;
    return d->finishClient(true, error, true);
}

bool KisTiledDataManagerPageStoreWriteBatch::cancel()
{
    if (m_clientFinished || !d) return false;
    m_clientFinished = true;
    return d->finishClient(false);
}

bool KisTiledDataManagerPageStoreWriteBatch::Private::finishClient(
    bool succeeded, QString *error, bool retainAdmission)
{
    QMutexLocker leaseLock(&leaseMutex);
    KisTiledDataManagerPageStoreBackend *owner = backend;
    if (!owner) return succeeded && !failed;
    bool finalClient = false;
    bool clientSucceeded = false;
    {
        QMutexLocker lock(&owner->d->mutex);
        if (retainAdmission && (clients != 1 || iteratorScope || leases)) {
            // Managed cursor delivery cannot hand off a batch with escaped
            // legacy clients/leases. Their original final holder cancels it.
            succeeded = false;
            retainAdmission = false;
            KisPageStoreDetail::setError(error, QStringLiteral("adapter delivery has live legacy clients"));
        }
        if (!succeeded) failed = true;
        if (clients == 0) return false;
        finalClient = --clients == 0;
        clientSucceeded = !failed;
    }
    leaseLock.unlock();
    return finalClient ? finish(error, retainAdmission) : clientSucceeded;
}

bool KisTiledDataManagerPageStoreWriteBatch::Private::finish(QString *error, bool retainAdmission)
{
    if (!backend) return !failed;
    if (!transaction.isValid()) return false;
    const bool mutationActive = mutation.isActive();
    if (!mutationActive && !failed) return false;
    bool privateCancelled = false;
    if (failed) KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore private mutation batch cancelled"));
    const bool sealed = mutationActive && !failed && (iteratorScope
        ? mutation.sealForLegacyUnlock(error)
        : retainAdmission ? mutation.sealForAdapterDelivery(error) : mutation.seal(error));
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
    backend->d->cpuMutationBatches.erase(thread);
    backend = nullptr;
    return !failed;
}

bool KisTiledDataManagerPageStoreWriteBatch::replaceFullTile(
    qint32 column,
    qint32 row,
    KisTileData *tileData,
    bool sparseDefault,
    QString *error) try
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
        const auto found = d->sources.find(tileData);
        auto source = found != d->sources.end() ? found->second : std::shared_ptr<const KisPageReplicaSource>{};
        if (!source) {
            const auto overlay = KisPageReadView::transactionOverlay(d->transaction.id);
            KisSurfaceEpochState state;
            if (d->backend->store()->resolveSurfaceState(key.surface, overlay, &state))
                source = d->backend->d->provider->captureCompletedTileSource(state.allocationDescriptor(), tileData, error);
            if (source) d->sources.emplace(tileData, source);
        }
        staged = source && d->mutation.aliasPage(key, source, error);
    }
    if (!staged) d->failed = true;
    return staged;
}
catch (const std::bad_alloc &) {
    d->failed = true;
    KisPageStoreDetail::setError(error, QStringLiteral("tile source directory storage was refused"));
    return false;
}

KisTiledDataManagerPageStoreBackend::KisTiledDataManagerPageStoreBackend()
    : d(new Private)
{
}

KisTiledDataManagerPageStoreBackend::~KisTiledDataManagerPageStoreBackend()
{
    if (d->historyMutationPhase != Private::HistoryMutationPhase::None) {
        d->historyMutation.cancel();
        d->historyMutation = {};
    }
    if (d->history) d->history->close();
    if (d->store) d->store->closeSession();
}

bool KisTiledDataManagerPageStoreBackend::configure(
    quint32 pixelSize,
    const quint8 *defaultPixel,
    QString *error) try
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
    surface.format.defaultPixel = KisPageByteArray(
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
catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("tiles3 format storage preparation was refused"));
    return false;
}

bool KisTiledDataManagerPageStoreBackend::configureClone(
    const KisTiledDataManagerPageStoreBackend &source,
    QString *error) try
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

catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("tiles3 clone storage preparation was refused"));
    return false;
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

std::shared_ptr<std::pmr::memory_resource> KisTiledDataManagerPageStoreBackend::extentStorage() const
{
    return kisPageProcessMemoryResource();
}

KisTiles3PayloadWork KisTiledDataManagerPageStoreBackend::payloadWork() const
{
    return d->operational.load(std::memory_order_acquire)
        ? d->provider->payloadWork() : KisTiles3PayloadWork{};
}

std::unique_ptr<KisTiledDataManagerPageStoreWriteBatch>
KisTiledDataManagerPageStoreBackend::beginMutationBatch(QString *error) try
{
    {
        QMutexLocker lock(&d->mutex);
        if (d->cpuMutationBatches.find(QThread::currentThreadId()) != d->cpuMutationBatches.end()) {
            KisPageStoreDetail::setError(error, QStringLiteral("nested native CPU batch is not supported"));
            return {};
        }
    }
    StorageMap<Qt::HANDLE, std::weak_ptr<KisTiledDataManagerPageStoreWriteBatch::Private>> preparedIndex;
    preparedIndex.emplace(QThread::currentThreadId(), std::weak_ptr<KisTiledDataManagerPageStoreWriteBatch::Private>{});
    auto batch = std::unique_ptr<KisTiledDataManagerPageStoreWriteBatch>(
        new KisTiledDataManagerPageStoreWriteBatch(nullptr, {}, false));
    bool owned = false;
    const KisPageTransaction transaction = writableTransaction(&owned, error);
    if (!transaction.isValid()) return {};
    const auto returnTransaction = qScopeGuard([&] {
        if (owned) abortOwnedTransaction(transaction);
    });
    auto mutation = d->store->beginMutation(transaction, error);
    if (!mutation.isActive()) return {};
    batch->d->backend = this;
    batch->d->transaction = transaction;
    batch->d->owned = std::exchange(owned, false);
    batch->d->mutation = std::move(mutation);
    preparedIndex.begin()->second = batch->d;
    QMutexLocker lock(&d->mutex);
    const auto inserted = d->cpuMutationBatches.insert(preparedIndex.extract(preparedIndex.begin()));
    if (!inserted.inserted) return {};
    return batch;
}
catch (const std::bad_alloc &)
{
    KisPageStoreDetail::setError(error, QStringLiteral("tiles3 mutation batch storage preparation was refused"));
    return {};
}

std::unique_ptr<KisTiledDataManagerIteratorWriteScope>
KisTiledDataManagerPageStoreBackend::beginIteratorMutationScope(QString *error) try
{
    auto scope = std::unique_ptr<KisTiledDataManagerIteratorWriteScope>(
        new KisTiledDataManagerIteratorWriteScope);
    std::unique_ptr<KisTiledDataManagerPageStoreWriteBatch> batch;
    {
        QMutexLocker lock(&d->mutex);
        const auto slot = d->cpuMutationBatches.find(QThread::currentThreadId());
        auto shared = slot != d->cpuMutationBatches.end() ? slot->second.lock() : nullptr;
        if (shared) {
            // Zero clients means the last scope has started terminal sealing.
            // Keep the map entry as a barrier, but never revive that batch.
            if (!shared->iteratorScope || shared->failed || shared->clients == 0) {
                KisPageStoreDetail::setError(error, QStringLiteral(
                    "iterator cannot join the current native CPU batch"));
                return {};
            }
            batch.reset(new KisTiledDataManagerPageStoreWriteBatch(
                std::move(shared)));
            ++batch->d->clients;
            KisPageStoreDetail::setError(error, {});
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
    scope->d->batch = std::move(batch);
    return scope;
}
catch (const std::bad_alloc &)
{
    KisPageStoreDetail::setError(error, QStringLiteral("tiles3 iterator scope storage preparation was refused"));
    return {};
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
        if (d->historyMutationPhase != Private::HistoryMutationPhase::None) {
            KisPageStoreDetail::setError(error, QStringLiteral("history mutation requires its explicit owner"));
            return {};
        }
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
    const KisPreparedPageSet prepared = pageStore->preparedPages(transaction);
    if (!prepared.isValid()) {
        pageStore->abort(transaction);
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    const KisImageEpochCommitTicket committed =
        pageStore->commitAndReleasePublicationLock(transaction, prepared, publicationLocker);
    if (!committed.isValid()) pageStore->abort(transaction);
    KisPageStoreDetail::setError(error, committed.isValid()
        ? QString() : QStringLiteral("tiles3 PageStore commit failed"));
    return committed.isValid();
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
    const auto &defaultPixel = state.format.defaultPixel;
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
    if (lease) {
        lease->m_next = d->anonymousLeases;
        if (d->anonymousLeases) d->anonymousLeases->m_previous = lease;
        d->anonymousLeases = lease;
    }
}

void KisTiledDataManagerPageStoreBackend::unregisterAnonymousLease(
    KisTiledDataManagerPageStoreLease *lease)
{
    QMutexLocker locker(&d->mutex);
    if (lease->m_previous) lease->m_previous->m_next = lease->m_next;
    else d->anonymousLeases = lease->m_next;
    if (lease->m_next) lease->m_next->m_previous = lease->m_previous;
    lease->m_previous = lease->m_next = nullptr;
}

bool KisTiledDataManagerPageStoreBackend::cancelAnonymousLeasesForBarrier(
    QString *error)
{
    {
        QMutexLocker locker(&d->mutex);
        // clear is an externally sequenced operation, not permission to
        // cancel another running worker or a reentrant native operation.
        for (auto it = d->cpuMutationBatches.cbegin(); it != d->cpuMutationBatches.cend(); ++it) {
            auto batch = it->second.lock();
            if (!batch || !batch->iteratorScope || batch->clients == 0 ||
                batch->thread != QThread::currentThreadId()) {
                KisPageStoreDetail::setError(error, QStringLiteral("clear requires the other CPU batches to finish"));
                return false;
            }
        }
        for (auto *lease = d->anonymousLeases; lease; lease = lease->m_next) {
            if (!lease->belongsToCurrentThread()) {
                KisPageStoreDetail::setError(error, QStringLiteral("clear requires the foreign tile writer to finish"));
                return false;
            }
        }
    }
    for (;;) {
        std::shared_ptr<KisTiledDataManagerPageStoreWriteBatch::Private> batch;
        {
            QMutexLocker locker(&d->mutex);
            if (d->cpuMutationBatches.empty()) break;
            batch = d->cpuMutationBatches.begin()->second.lock();
        }
        if (!batch->cancelForBarrier(error)) return false;
    }
    for (;;) {
        KisTiledDataManagerPageStoreLease *lease;
        {
            QMutexLocker locker(&d->mutex);
            lease = d->anonymousLeases;
        }
        if (!lease) break;
        if (!lease->cancelForBarrier()) {
            KisPageStoreDetail::setError(error, QStringLiteral(
                "tiles3 PageStore write barrier cancellation failed"));
            return false;
        }
    }
    QMutexLocker locker(&d->mutex);
    if (d->anonymousTransaction.isValid() || d->anonymousClients != 0 ||
        d->anonymousLeases || d->store->sessionStats().activeCpuWritePages != 0) {
        KisPageStoreDetail::setError(error, QStringLiteral(
            "tiles3 PageStore write barrier still owns clients"));
        return false;
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

std::shared_ptr<const KisPageStoreIteratorReadScope>
KisTiledDataManagerPageStoreBackend::captureIteratorReadScope(
    bool writable, QString *error, std::shared_ptr<const KisPageStoreIteratorReadScope> existing) const try
{
    if (existing) {
        if (existing->m_store == d->store.get() && existing->m_surface == d->surface &&
            existing->m_beforeOnly == writable && (existing->isValid() || existing->observesLiveLegacyWriter())) {
            KisPageStoreDetail::setError(error, {});
            return existing;
        }
        KisPageStoreDetail::setError(error, QStringLiteral("iterator scope belongs to a different owner or access mode"));
        return d->failedReadScope;
    }
    auto result = std::allocate_shared<KisPageStoreIteratorReadScope>(KisMutationStorageAllocator<KisPageStoreIteratorReadScope>{});
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
        current = d->selectReadViewLocked(false);
        captureBefore = d->activeHistory.isValid();
        if (captureBefore) before = d->selectReadViewLocked(true);
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
catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("iterator read scope storage was refused"));
    return d->failedReadScope;
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
        selector = d->selectReadViewLocked(oldData);
    }
    return pageStore->captureReadView(selector, error);
}

bool KisTiledDataManagerPageStoreBackend::preparePagePresence(
    qsizetype count, QVarLengthArray<quint8, 64> *scratch, QString *error) const
{
    KisPageStoreDiagnosticTimer phase(store(),
        KisPageStoreDiagnosticPhase::MutationPresencePreflight, quint64(qMax(qsizetype(0), count)));
    phase.next(KisPageStoreDiagnosticPhase::MutationPresencePrepare, quint64(qMax(qsizetype(0), count)));
    try {
        if (count < 0) throw std::bad_alloc();
        scratch->resize(count);
        if (scratch->size() != count || (count && !scratch->data())) throw std::bad_alloc();
    } catch (const std::bad_alloc &) {
        if (error) *error = QStringLiteral("pixel operation presence storage is unavailable");
        return false;
    }
    phase.next(KisPageStoreDiagnosticPhase::MutationPresencePrepared, quint64(count));
    return true;
}

bool KisTiledDataManagerPageStoreBackend::resolveCurrentPagePresenceInto(
    const QVector<KisLogicalPageId> &pages, quint8 *scratch, qsizetype capacity, QString *error) const
{
    KisPageStoreDiagnosticTimer phase(store(), KisPageStoreDiagnosticPhase::MutationPresencePreflight,
                                      quint64(pages.size()));
    phase.next(KisPageStoreDiagnosticPhase::MutationPresenceResolve, quint64(pages.size()));
    // Keep an overlay selector alive through metadata resolution, without
    // retaining/freeze-copying that overlay or acquiring a physical read pin.
    QReadLocker publicationLocker(&d->publicationLock);
    KisPageReadView selector;
    {
        QMutexLocker lock(&d->mutex);
        if (!d->operational) {
            KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore is unavailable"));
            return false;
        }
        selector = d->selectReadViewLocked(false);
    }
    const bool resolved = d->store->resolvePagePresenceInto(d->surface, pages, selector, scratch, capacity);
    publicationLocker.unlock();
    phase.next(KisPageStoreDiagnosticPhase::MutationPresenceResolved, quint64(pages.size()));
    KisPageStoreDetail::setError(error, resolved ? QString{} : QStringLiteral("tiles3 page presence is unavailable"));
    return resolved;
}

bool KisTiledDataManagerPageStoreBackend::hasCurrentThreadIteratorWrites() const
{
    // The explicit iterator mutation scope is also the single same-thread
    // discovery source for unpublished compatibility bytes.
    QMutexLocker locker(&d->mutex);
    const auto slot = d->cpuMutationBatches.find(QThread::currentThreadId());
    const auto batch = slot != d->cpuMutationBatches.end() ? slot->second.lock() : nullptr;
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

KisTileSP KisTiledDataManagerPageStoreBackend::selectCurrentReadTile(
    KisTiledDataManager &manager, KisTileSP candidate, bool *present)
{
    if (!candidate || !present) return {};
    // Only the original iterator scope authorizes discovery of live legacy
    // bytes. An external read of a persistent history selects its last cut.
    if (hasCurrentThreadIteratorWrites()) {
        if (!*present) candidate->setPageStoreNativeReadReady();
        else if (!candidate->refreshPageStoreData()) return {};
        return candidate;
    }
    const bool indexed = *present;
    const KisPageKey key{d->surface, {candidate->col(), candidate->row()}};
    KisPageVersion version;
    KisSurfaceEpochState defaults;
    {
        QReadLocker publication(&d->publicationLock);
        KisPageReadView selector;
        {
            QMutexLocker lock(&d->mutex);
            if (!d->operational) return {};
            selector = d->selectReadViewLocked(false);
        }
        if (!d->store->resolveTileReadIdentity(key, selector, &version, &defaults)) return {};
    }
    // Hash/default locks precede the tile barrier. Never hold publication read
    // while waiting for that barrier: an existing tile reader can need it.
    KisTileData *defaultData = version.isDefaultPixel()
        ? manager.m_hashTable->refAndFetchDefaultTileData() : nullptr;
    const auto releaseDefault = qScopeGuard([&] { if (defaultData) defaultData->deref(); });
    QMutexLocker lock(&candidate->m_swapBarrierLock);
    if (candidate->m_pageStoreWriteLockCount) return candidate;
    auto &cache = candidate->m_pageStoreLeases[KisTile::ReadLease];
    if (cache && d->provider->readCacheMatchesVersion(cache.get(), version)) {
        candidate->m_pageStoreNativeReadReady.storeRelease(1);
        *present = !version.isDefaultPixel();
        return candidate;
    }
    try {
        const auto install = [&](KisTileData *data, TileLease replacement) -> KisTileSP {
            // A different selection cannot join an old wrapper's nested pin.
            // A transient result has no manager/bridge pointer to outlive; its
            // original COW reference is acquired before releasing the exact
            // read pin, then ordinary detached Tile locks protect its storage.
            KisTileSP result = candidate;
            if (candidate->m_lockCounter) result = new KisTile(key.page.column, key.page.row, data, nullptr);
            else result->replacePageStoreReadCacheLocked(data);
            if (replacement && !replacement->finish()) return {};
            if (!indexed || result != candidate) {
                result->m_pageStoreLeases[KisTile::ReadLease].reset();
                result->m_pageStoreBridge.storeRelease(nullptr);
            } else result->m_pageStoreLeases[KisTile::ReadLease] = std::move(replacement);
            result->m_pageStoreNativeReadReady.storeRelease(1);
            return result;
        };
        const auto selectDefault = [&](const KisSurfaceEpochState &state) -> KisTileSP {
            bool matches = false;
            if (defaultData && defaultData->blockSwapping()) {
                matches = defaultData->pixelSize() == state.format.pixelStride &&
                    std::memcmp(defaultData->data(), state.format.defaultPixel.constData(),
                                size_t(state.format.pixelStride)) == 0;
                defaultData->unblockSwapping();
            }
            if (!matches) {
                if (defaultData) std::exchange(defaultData, nullptr)->deref();
                defaultData = KisTileDataStore::instance()->createDefaultTileData(
                    state.format.pixelStride, reinterpret_cast<const quint8 *>(state.format.defaultPixel.constData()));
                if (!defaultData) return {};
                defaultData->ref();
            }
            *present = false;
            // A protected uniform default already has exactly these bytes;
            // no PageStore default page, provider request or new wrapper.
            if (!cache && candidate->m_tileData == defaultData) {
                if (!indexed) candidate->m_pageStoreBridge.storeRelease(nullptr);
                candidate->m_pageStoreNativeReadReady.storeRelease(1);
                return candidate;
            }
            return install(defaultData, {});
        };
        if (version.isDefaultPixel()) return selectDefault(defaults);

        // A cold/mismatched cache needs an exact capability. Recapture the
        // original selector here so a removed transaction id is never reused.
        // The lock order is the existing tile barrier -> publication read.
        auto view = captureReadView();
        if (!view.isValid() || !view.resolvePageVersion(key, &version)) return {};
        if (version.isDefaultPixel()) {
            if (!view.resolveSurfaceState(key.surface, &defaults)) return {};
            return selectDefault(defaults);
        }
        KisPageStoreReadPage read(d->store.get(), view, key);
        if (!read.data()) return {};
        TileLease reuse;
        if (!candidate->m_lockCounter && cache && !cache->readPinned()) reuse = std::move(cache);
        TileLease replacement = readCacheForPage(read, std::move(reuse));
        if (!replacement || !replacement->tileData()) return {};
        KisTileData *data = replacement->tileData();
        auto result = install(data, std::move(replacement));
        if (result) *present = true;
        return result;
    } catch (const std::bad_alloc &) {
        return {};
    }
}

std::unique_ptr<KisTilePageStoreLease>
KisTiledDataManagerPageStoreBackend::acquireTile(
    qint32 column,
    qint32 row,
    bool writable,
    bool oldData,
    TileLease *readCache) try
{
    // A read view that names a transaction must remain valid until PageStore
    // has converted it into an active read capability. Readers share this
    // publication gate; only the final publisher/aborter takes it exclusively.
    QReadLocker publicationLocker(
        writable ? nullptr : &d->publicationLock);
    KisPageStore *pageStore = nullptr;
    std::shared_ptr<KisTiles3PageReplicaProvider> provider;
    KisPageReadView readView;
    {
        QMutexLocker locker(&d->mutex);
        if (!d->operational ||
            (writable && oldData)) {
            return {};
        }
        pageStore = d->store.get();
        provider = d->provider;
        readView = d->selectReadViewLocked(oldData);
    }

    const KisPageKey key{d->surface, {column, row}};
    if (!writable) {
        auto view = pageStore->captureReadView(readView);
        publicationLocker.unlock();
        KisPageStoreReadPage read(pageStore, view, key, nullptr,
                                  KisPagePriority::Normal, false);
        TileLease reuse;
        if (readCache && *readCache && !(*readCache)->readPinned())
            reuse = std::move(*readCache);
        return readCacheForPage(read, std::move(reuse));
    }

    KisPageMutationSession *mutation = nullptr;
    std::shared_ptr<KisTiledDataManagerPageStoreWriteBatch::Private> batch;
    {
        QMutexLocker lock(&d->mutex);
        const auto slot = d->cpuMutationBatches.find(QThread::currentThreadId());
        batch = slot != d->cpuMutationBatches.end() ? slot->second.lock() : nullptr;
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
    auto returnClient = qScopeGuard([&] {
        if (ownsTransaction) abortOwnedTransaction(transaction);
    });
    auto native = pageStore->beginMutation(transaction, &transactionError);
    if (!native.reserveLegacyMutationPage(key, &transactionError)) {
        native.cancel();
        return {};
    }
    auto guard = native.beginWrite(key, &transactionError);
    KisTileData *tile = guard.isValid() ? provider->tileDataForCpuWriteGuard(guard) : nullptr;
    if (!tile) {
        guard = {};
        native.cancel();
        return {};
    }
    TileLease futureRead;
    if (readCache) {
        TileLease reuse;
        if (*readCache && !(*readCache)->readPinned()) reuse = std::move(*readCache);
        futureRead = provider->prepareTileReadCache(guard, std::move(reuse));
        if (!futureRead) {
            guard = {};
            native.cancel();
            return {};
        }
    }
    auto lease = std::make_unique<KisTiledDataManagerPageStoreLease>(
        this, tile, transaction, ownsTransaction,
        std::move(native), std::move(guard), std::move(futureRead));
    ownsTransaction = false;
    return lease;
}
catch (const std::bad_alloc &) { return {}; }

KisMementoSP KisTiledDataManagerPageStoreBackend::beginHistory(
    const quint8 *defaultPixel,
    quint32 pixelSize,
    QString *error) try
{
    QMutexLocker transactionLocker(&d->transactionMutex);
    QMutexLocker locker(&d->mutex);
    if (!d->operational || !d->history || d->activeHistory.isValid() ||
        d->anonymousTransaction.isValid() || d->anonymousClients != 0) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore history is unavailable"));
        return {};
    }
    KisMementoSP candidate = new KisMemento(nullptr);
    candidate->saveOldDefaultPixel(defaultPixel, pixelSize);
    const auto prepared = d->mementos.emplace(candidate.data(), KisPageStoreMemento{}).first;
    const KisPageStoreHistoryTransaction transaction =
        d->history->begin(error);
    if (!transaction.isValid()) {
        d->mementos.erase(prepared);
        return {};
    }
    d->activeHistory = transaction;
    d->currentMemento = std::move(candidate);
    return d->currentMemento;
}
catch (const std::bad_alloc &)
{
    KisPageStoreDetail::setError(error, QStringLiteral("tiles3 history storage preparation was refused"));
    return {};
}

bool KisTiledDataManagerPageStoreBackend::beginHistoryMutation(
    const KisMementoSP &owner, QString *error)
{
    QMutexLocker transactionLocker(&d->transactionMutex);
    QMutexLocker locker(&d->mutex);
    if (!owner || d->currentMemento != owner || !d->activeHistory.isValid() ||
        !d->operational || !d->cpuMutationBatches.empty() || d->anonymousLeases) {
        KisPageStoreDetail::setError(error, QStringLiteral("history mutation owner is unavailable"));
        return false;
    }
    using Phase = Private::HistoryMutationPhase;
    if (d->historyMutationPhase != Phase::None) {
        const bool active = d->historyMutationPhase == Phase::Open && d->historyMutation.isActive();
        KisPageStoreDetail::setError(error, active ? QString{} : QStringLiteral("history mutation is closing or failed"));
        return active;
    }
    const auto transaction = d->activeHistory.transaction;
    locker.unlock();
    auto mutation = d->store->beginMutation(transaction, error);
    if (!mutation.isActive()) return false;
    locker.relock();
    d->historyMutation = std::move(mutation);
    d->historyMutationPhase = Phase::Open;
    return true;
}

KisPageSnapshotArray<KisLogicalPageId> KisTiledDataManagerPageStoreBackend::historyChangedPages(
    const KisPageTransaction &transaction) const
{
    // Original canonical history delta, not a second touched-page authority.
    // Called only at a checkpoint/terminal boundary, never per dab.
    const auto prepared = d->store->preparedPages(transaction);
    KisPageSnapshotArray<KisLogicalPageId> pages;
    pages.reserve(prepared.proofs.size() + prepared.removedPages.size());
    for (const auto &proof : prepared.proofs)
        if (proof.authority.version.key.surface == d->surface)
            pages.append(proof.authority.version.key.page);
    for (const auto &key : prepared.removedPages)
        if (key.surface == d->surface) pages.append(key.page);
    return pages;
}

KisCapturedReadView KisTiledDataManagerPageStoreBackend::checkpointHistoryMutation(
    const KisMementoSP &owner, QVector<KisLogicalPageId> *changed, QString *error) try
{
    if (changed) changed->clear();
    QMutexLocker transactionLocker(&d->transactionMutex);
    QWriteLocker publicationLocker(&d->publicationLock);
    QMutexLocker locker(&d->mutex);
    if (!owner || d->currentMemento != owner || !d->activeHistory.isValid() ||
        d->historyMutationPhase != Private::HistoryMutationPhase::Open) {
        KisPageStoreDetail::setError(error, QStringLiteral("checkpoint history owner is unavailable"));
        return {};
    }
    const auto transaction = d->activeHistory.transaction;
    locker.unlock();
    auto view = d->historyMutation.checkpointForRead(error);
    if (view.isValid() && changed) *changed = historyChangedPages(transaction);
    return view;
}
catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("checkpoint output storage was refused"));
    return {};
}

bool KisTiledDataManagerPageStoreBackend::commitHistory(
    const quint8 *defaultPixel,
    quint32 pixelSize,
    QString *error, QVector<KisLogicalPageId> *changed) try
{
    if (changed) changed->clear();
    QMutexLocker transactionLocker(&d->transactionMutex);
    QWriteLocker publicationLocker(&d->publicationLock);
    QMutexLocker locker(&d->mutex);
    if (!d->activeHistory.isValid()) {
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    d->currentMemento->saveNewDefaultPixel(defaultPixel, pixelSize);
    const KisPageTransaction transaction = d->activeHistory.transaction;
    using Phase = Private::HistoryMutationPhase;
    const bool persistent = d->historyMutationPhase != Phase::None;
    if (persistent && d->historyMutationPhase != Phase::Sealed) {
        d->historyMutationPhase = Phase::Closing;
        locker.unlock();
        if (!d->historyMutation.sealForLegacyUnlock(error)) return false;
        locker.relock();
        d->historyMutationPhase = Phase::Sealed;
    }
    locker.unlock();
    if (d->store->sessionStats().activeCpuWritePages != 0) {
        KisPageStoreDetail::setError(error, QStringLiteral("history commit requires adapter delivery to finish"));
        return false;
    }
    // Refresh these original delta keys even if a later root commit rejects:
    // the session may already have sealed, so an old compatibility cache must
    // not continue advertising the pre-checkpoint bytes.
    const auto pages = historyChangedPages(transaction);
    if (persistent && changed) *changed = pages;
    if (!pruneDefaultPreparedPages(transaction, error)) {
        return false;
    }
    locker.relock();
    const KisPageStoreMemento committed =
        d->history->commit(d->activeHistory, error);
    if (!committed.isValid()) return false;
    // Preserve the original transaction's undo/redo projection invalidation.
    // The canonical delta includes removed pages; current extent alone would
    // lose the dirty region when an undo removes the last painted pixels.
    QRect dirtyExtent;
    for (const auto &page : pages) {
        const qint64 x = qint64(page.column) * KisTileData::WIDTH;
        const qint64 y = qint64(page.row) * KisTileData::HEIGHT;
        KIS_ASSERT(x >= std::numeric_limits<qint32>::min() &&
                   y >= std::numeric_limits<qint32>::min() &&
                   x + KisTileData::WIDTH - 1 <= std::numeric_limits<qint32>::max() &&
                   y + KisTileData::HEIGHT - 1 <= std::numeric_limits<qint32>::max());
        dirtyExtent |= QRect(int(x), int(y), KisTileData::WIDTH, KisTileData::HEIGHT);
    }
    d->currentMemento->setExtent(dirtyExtent);
    const auto stored = d->mementos.find(d->currentMemento.data());
    KIS_ASSERT(stored != d->mementos.end());
    stored->second = committed;
    d->activeHistory = {};
    d->currentMemento.clear();
    auto completedMutation = std::move(d->historyMutation);
    d->historyMutationPhase = Phase::None;
    locker.unlock();
    completedMutation = {};
    return true;
}
catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("history output storage was refused"));
    return false;
}

bool KisTiledDataManagerPageStoreBackend::abortHistory(QString *error)
{
    return abortHistory(KisMementoSP(), error);
}

bool KisTiledDataManagerPageStoreBackend::prepareHistoryAbort(
    const KisMementoSP &memento, QVector<KisLogicalPageId> *pages, QString *error)
{
    QMutexLocker transactionLocker(&d->transactionMutex);
    QReadLocker publicationLocker(&d->publicationLock);
    QMutexLocker locker(&d->mutex);
    if (!memento || d->currentMemento != memento || !d->activeHistory.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 abort memento is not current"));
        return false;
    }
    if (d->historyMutationPhase == Private::HistoryMutationPhase::None &&
        d->store->sessionStats().activeCpuWritePages != 0) {
        KisPageStoreDetail::setError(error, QStringLiteral("history abort requires adapter delivery to finish"));
        return false;
    }
    const auto snapshot = d->store->captureCommittedEpoch();
    if (!(snapshot.epoch == d->activeHistory.transaction.baseEpoch)) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 abort base changed"));
        return false;
    }
    pages->clear();
    pages->reserve(snapshot.manifest.size());
    for (const auto &version : snapshot.manifest) {
        if (version.key.surface == d->surface && !version.isDefaultPixel())
            pages->append(version.key.page);
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisTiledDataManagerPageStoreBackend::abortHistory(
    const KisMementoSP &memento, QString *error)
{
    QMutexLocker transactionLocker(&d->transactionMutex);
    QWriteLocker publicationLocker(&d->publicationLock);
    QMutexLocker locker(&d->mutex);
    if (memento) {
        if (d->currentMemento != memento || !d->activeHistory.isValid()) {
            KisPageStoreDetail::setError(error, QStringLiteral("tiles3 abort memento is not current"));
            return false;
        }
        const auto committed = d->store->captureReadView();
        if (!committed.isValid() || !(committed.epoch() == d->activeHistory.transaction.baseEpoch)) {
            KisPageStoreDetail::setError(error, QStringLiteral("tiles3 abort base changed"));
            return false;
        }
    }
    if (!d->activeHistory.isValid()) {
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    if (d->historyMutationPhase != Private::HistoryMutationPhase::None) {
        d->historyMutationPhase = Private::HistoryMutationPhase::Closing;
        locker.unlock();
        if (!d->historyMutation.cancel()) {
            KisPageStoreDetail::setError(error, QStringLiteral("history mutation cancellation still owns work"));
            return false;
        }
        locker.relock();
    }
    if (d->store->sessionStats().activeCpuWritePages != 0) {
        KisPageStoreDetail::setError(error, QStringLiteral("history abort requires adapter delivery to finish"));
        return false;
    }
    const bool aborted = d->history &&
                         d->history->abort(d->activeHistory, error);
    if (!aborted) return false;
    d->activeHistory = {};
    d->mementos.erase(d->currentMemento.data());
    d->currentMemento.clear();
    auto cancelledMutation = std::move(d->historyMutation);
    d->historyMutationPhase = Private::HistoryMutationPhase::None;
    locker.unlock();
    cancelledMutation = {};
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
    const auto entry = d->mementos.find(memento.data());
    const KisPageStoreMemento stored = entry != d->mementos.end() ? entry->second : KisPageStoreMemento{};
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
    const auto entry = d->mementos.find(memento.data());
    const KisPageStoreMemento stored = entry != d->mementos.end() ? entry->second : KisPageStoreMemento{};
    if (!stored.isValid() || !d->history->purge(stored, error)) return false;
    d->mementos.erase(memento.data());
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
    QString *error, QVector<KisLogicalPageId> *changed) try
{
    if (changed) changed->clear();
    if (!isOperational() || rect.isEmpty()) return false;
    prepareChangedPageOutput(changed, rect);
    bool success = false;
    auto discardOutput = qScopeGuard([&] { if (changed && !success) changed->clear(); });
    auto batch = beginMutationBatch(error);
    if (!batch) return false;
    auto before = captureReadView(false, error);
    KisSurfaceEpochState surfaceState;
    if (!before.resolveSurfaceState(d->surface, &surfaceState) ||
        pixel.size() != qsizetype(surfaceState.format.pixelStride)) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 fill surface is invalid"));
        return false;
    }
    std::vector<char, KisMutationStorageAllocator<char>> rowBytes(size_t(KisTileData::WIDTH * pixel.size()));
    for (int x = 0; x < KisTileData::WIDTH; ++x)
        memcpy(rowBytes.data() + x * pixel.size(), pixel.constData(), size_t(pixel.size()));
    std::shared_ptr<const KisPageReplicaSource> uniform;
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
                if (changed) changed->append(key.page);
                continue;
            }
            auto lease = acquireTile(key.page.column, key.page.row, true, false);
            if (!lease || !lease->writable() || !lease->tileData()) return false;
            auto *bytes = lease->tileData()->data(); if (!bytes) return false;
            const int x = affected.left() - pageRect.left(), y = affected.top() - pageRect.top();
            for (int row = 0; row < affected.height(); ++row)
                memcpy(bytes + (qsizetype(y + row) * KisTileData::WIDTH + x) * pixel.size(),
                       rowBytes.data(), size_t(affected.width() * pixel.size()));
            lease->markDirty(); if (!lease->finish()) return false;
            if (changed) changed->append(key.page);
        }
    }
    before = {};
    success = batch->finish(error);
    return success;
}
catch (const std::bad_alloc &) {
    if (changed) changed->clear();
    KisPageStoreDetail::setError(error, QStringLiteral("fill storage preparation was refused"));
    return false;
}

template<typename Operation>
KisPageStoreWriteOperationResult KisTiledDataManagerPageStoreBackend::runCpuMutationOperation(
    const KisPageSnapshotArray<KisLogicalPageId> &targets, bool legacyIntent, bool prepareWrites,
    Operation &&operation,
    QVector<KisLogicalPageId> *changed, QString *error, const KisMementoSP &historyOwner,
    OperationDelivery *delivery,
    const std::function<bool(QString *)> &prepareAdapter,
    const AdapterCompletion &completeAdapter)
{
    using Result = KisPageStoreWriteOperationResult;
    if (changed) changed->clear();
    if (delivery && !delivery->isEmpty()) {
        KisPageStoreDetail::setError(error, QStringLiteral("adapter delivery output is already occupied"));
        return Result::Failed;
    }
    if (!isOperational()) return Result::Unavailable;
    if (targets.isEmpty() && !historyOwner) return Result::Succeeded;
    using Phase = KisPageStoreDiagnosticPhase;
    KisPageStoreDiagnosticTimer phase(store(), Phase::PixelOperationPreflight, quint64(targets.size()));
    {
        QMutexLocker lock(&d->mutex);
        const auto slot = d->cpuMutationBatches.find(
            QThread::currentThreadId());
        if (slot != d->cpuMutationBatches.cend()) {
            const auto existing = slot->second.lock();
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
    phase.next(Phase::PixelOperationStoragePrepare, quint64(targets.size()));
    try {
        // Keep this buffer unique until delivery: no Qt detach or growth may
        // occur while recording the result, particularly after publication.
        if (changed) {
            changed->reserve(targets.size());
            // Reserving an empty Qt container can leave it empty on allocator
            // refusal without throwing. Do not enter pixels with no storage.
            if (changed->capacity() < targets.size()) {
                KisPageStoreDetail::setError(error, QStringLiteral("pixel operation changed-page output is unavailable"));
                return Result::Failed;
            }
        }
    } catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("pixel operation changed-page storage is unavailable"));
        return Result::Failed;
    }
    phase.next(Phase::PixelOperationRangePrepare, quint64(targets.size()));
    std::unique_ptr<KisTiledDataManagerPageStoreWriteBatch> batch;
    KisPageMutationExecution execution;
    KisPageTransaction transaction;
    if (historyOwner) {
        QMutexLocker transactionLocker(&d->transactionMutex);
        QMutexLocker locker(&d->mutex);
        if (d->currentMemento != historyOwner || !d->activeHistory.isValid() || legacyIntent ||
            d->historyMutationPhase != Private::HistoryMutationPhase::Open || !d->historyMutation.isActive()) {
            KisPageStoreDetail::setError(error, QStringLiteral("pixel operation history owner is unavailable"));
            return Result::Failed;
        }
        if (targets.isEmpty()) return Result::Succeeded;
        transaction = d->activeHistory.transaction;
        locker.unlock();
        execution = d->historyMutation.borrowExecution(d->surface, targets, error);
    } else {
        bool storeBorrowed = false;
        auto range = store()->reserveManagedRange(d->surface, targets, legacyIntent,
                                                  &storeBorrowed, error);
        if (!range) return storeBorrowed ? Result::Borrowed : Result::Failed;
        phase.next(Phase::PixelOperationRangeReserved, quint64(targets.size()));
        batch = beginMutationBatch(error);
        if (!batch || !batch->d->mutation.adoptReservation(std::move(range), error)) return Result::Failed;
        transaction = batch->d->transaction;
        execution = batch->d->mutation.borrowExecution(d->surface, targets, error);
    }
    if (!execution.isActive()) return Result::Failed;
    const auto selection = KisPageReadView::transactionOverlay(transaction.id);
    KisSurfaceEpochState state;
    if (!store()->resolveSurfaceState(surface(), selection, &state) ||
        state.logicalPageExtent != QSize(KisTileData::WIDTH, KisTileData::HEIGHT) ||
        quint64(state.format.pixelStride) * KisTileData::WIDTH > quint64(std::numeric_limits<qint32>::max()))
        return Result::Failed;

    if (prepareAdapter) {
        phase.next(Phase::MutationAdapterPrepare, quint64(targets.size()));
        bool prepared = false;
        try {
            prepared = prepareAdapter(error);
        } catch (const std::bad_alloc &) {
            KisPageStoreDetail::setError(error, QStringLiteral("adapter storage preparation is unavailable"));
        }
        phase.next(Phase::MutationAdapterPrepared, quint64(targets.size()));
        if (!prepared) {
            // No pixel work began. Return this borrow normally so refusal
            // cannot poison earlier pending work in the same history scope.
            execution.finish(nullptr);
            return Result::Failed;
        }
    }
    if (prepareWrites) {
        const auto preparation = execution.prepareWrites(error);
        if (preparation == KisPageMutationExecution::PreparationResult::Failed) {
            // Preparation never exposed pixels. Return the borrow normally so
            // a persistent history owner can retry after pressure is relieved.
            execution.finish(nullptr);
            return Result::Failed;
        }
    }
    phase.next(Phase::PixelOperationBody, 1);
    if (!operation(execution, transaction, state, error)) return Result::Failed;
    phase.next(Phase::PixelOperationChangedExport, quint64(targets.size()));
    const qsizetype touched = execution.finishPreparedWrites(
        changed, error);
    if (touched < 0) {
        KisPageStoreDetail::setError(error, QStringLiteral("pixel operation changed-page export is unavailable"));
        return Result::Failed;
    }
    // Completion consumes the exact operation delta while it surrounds the
    // original publication. Restore the caller's empty-on-failure contract if
    // publication or prepared adapter installation rejects.
    phase.next(Phase::PixelOperationFinish, quint64(touched));
    // The manager's short completion owns the existing adapter gate. For a
    // standalone operation it surrounds publication and prepared installation;
    // history installs its pending working adapter before returning the borrow.
    const auto complete = [&](const std::function<bool(QString *)> &publish) {
        return completeAdapter ? completeAdapter(publish, error) : publish(error);
    };
    if (historyOwner) {
        if (completeAdapter && !complete([](QString *) { return true; })) {
            if (changed) changed->clear();
            return Result::Failed;
        }
        if (!execution.finish(error)) {
            if (changed) changed->clear();
            return Result::Failed;
        }
    } else {
        if (!execution.finish(error)) {
            if (changed) changed->clear();
            return Result::Failed;
        }
        if (batch && !complete([&](QString *failure) {
                return delivery ? batch->finishForAdapterDelivery(failure)
                                : batch->finish(failure);
            })) {
            if (changed) changed->clear();
            return Result::Failed;
        }
    }
    phase.next(Phase::PixelOperationChangedDeliver, quint64(touched));
    if (delivery && batch) delivery->m_batch = std::move(batch);
    phase.next(Phase::PixelOperationCleanup, quint64(touched));
    return Result::Succeeded;
}

KisPageStoreWriteOperationResult KisTiledDataManagerPageStoreBackend::writeOperation(
    const QVector<KisLogicalPageId> &pages, bool legacyIntent, const KisPageStorePixelOperation &operation,
    QVector<KisLogicalPageId> *changed, QString *error, const KisMementoSP &historyOwner,
    OperationDelivery *delivery,
    const std::function<bool(QString *)> &prepareAdapter,
    const AdapterCompletion &completeAdapter) try
{
    if (!operation) {
        if (changed) changed->clear();
        KisPageStoreDetail::setError(error, QStringLiteral("pixel operation callback is absent"));
        return KisPageStoreWriteOperationResult::Failed;
    }
    KisPageSnapshotArray<KisLogicalPageId> targets(pages.cbegin(), pages.cend());
    std::sort(targets.begin(), targets.end(), [](const auto &a, const auto &b) {
        return std::tie(a.row, a.column) < std::tie(b.row, b.column);
    });
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    return runCpuMutationOperation(targets, legacyIntent, true,
        [&](KisPageMutationExecution &mutation, const KisPageTransaction &, const KisSurfaceEpochState &state,
            QString *error) {
    // Expose only the declared target set and poison after the first failed
    // move. A later successful move cannot hide an earlier acquisition failure.
    class BoundedCursor final : public KisPixelWriteCursor {
    public:
        BoundedCursor(KisPageMutationExecution &mutation, KisSurfaceId surface, quint32 pixelSize,
                       const KisPageSnapshotArray<KisLogicalPageId> &targets)
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
                current = next;
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
        KisPageMutationExecution &mutation;
        KisSurfaceId surface;
        quint32 pixelSize;
        const KisPageSnapshotArray<KisLogicalPageId> &targets;
        KisLogicalPageId current;
        QPoint position;
        KisCpuWriteGuard guard;
        quint8 *data = nullptr;
        bool failed = false;
        QString failure;
    } cursor(mutation, surface(), state.format.pixelStride, targets);
    const bool accepted = operation(&cursor);
    cursor.release();
    if (!accepted || cursor.failed) {
        KisPageStoreDetail::setError(error, cursor.failed ? cursor.failure : QStringLiteral("pixel operation callback failed or used invalid coordinates"));
        return false;
    }
    return true;
    }, changed, error, historyOwner, delivery, prepareAdapter, completeAdapter);
}
catch (const std::bad_alloc &) {
    if (changed) changed->clear();
    KisPageStoreDetail::setError(error, QStringLiteral("pixel operation storage was refused"));
    return KisPageStoreWriteOperationResult::Failed;
}

KisPageStoreWriteOperationResult KisTiledDataManagerPageStoreBackend::writeBytes(
    const quint8 *data, qint32 x, qint32 y, qint32 width, qint32 height,
    qint32 dataRowStride, bool legacyIntent, QVector<KisLogicalPageId> *changed, QString *error,
    OperationDelivery *delivery,
    const std::function<bool(QString *)> &prepareAdapter,
    const AdapterCompletion &completeAdapter) try
{
    using Result = KisPageStoreWriteOperationResult;
    if (changed) changed->clear();
    if (delivery && !delivery->isEmpty()) {
        KisPageStoreDetail::setError(error, QStringLiteral("adapter delivery output is already occupied"));
        return Result::Failed;
    }
    if (!data || width <= 0 || height <= 0) { KisPageStoreDetail::setError(error, {}); return Result::Succeeded; }
    if (qint64(x) + width - 1 > std::numeric_limits<qint32>::max() ||
        qint64(y) + height - 1 > std::numeric_limits<qint32>::max()) {
        KisPageStoreDetail::setError(error, QStringLiteral("packed write coordinates overflow"));
        return Result::Failed;
    }
    const QRect rect(x, y, width, height);
    KisPageSnapshotArray<KisLogicalPageId> pages;
    for (qint64 row = pageCoordinate(y, KisTileData::HEIGHT);
         row <= pageCoordinate(rect.bottom(), KisTileData::HEIGHT); ++row)
        for (qint64 col = pageCoordinate(x, KisTileData::WIDTH);
             col <= pageCoordinate(rect.right(), KisTileData::WIDTH); ++col)
            pages.append({qint32(col), qint32(row)});

    const auto &targets = pages;
    return runCpuMutationOperation(targets, legacyIntent, false,
        [&](KisPageMutationExecution &mutation, const KisPageTransaction &transaction,
            const KisSurfaceEpochState &state, QString *error) {
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
        }
        phase.next(Phase::PackedWriteRelease, 1);
        before = {};
        return true;
    }, changed, error, {}, delivery, prepareAdapter, completeAdapter);
}
catch (const std::bad_alloc &) {
    if (changed) changed->clear();
    KisPageStoreDetail::setError(error, QStringLiteral("packed write storage was refused"));
    return KisPageStoreWriteOperationResult::Failed;
}

std::shared_ptr<const KisPageReplicaSource> KisTiledDataManagerPageStoreBackend::uniformSourceFor(
    const KisPageAllocationDescriptor &descriptor, const QByteArray &pixel, QString *error) try
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
catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("uniform source cache storage was refused"));
    return {};
}

bool KisTiledDataManagerPageStoreBackend::copyFrom(
    const KisTiledDataManagerPageStoreBackend &source,
    const KisCapturedReadView &sourceView, const KisCapturedReadView &targetView,
    const QRect &rect, bool rough, QString *error, QVector<KisLogicalPageId> *changed) try
{
    if (changed) changed->clear();
    if (!isOperational() || !source.isOperational() || rect.isEmpty()) return false;
    prepareChangedPageOutput(changed, rect);
    bool success = false;
    auto discardOutput = qScopeGuard([&] { if (changed && !success) changed->clear(); });
    KisPageStoreDiagnosticTimer phase(store(), KisPageStoreDiagnosticPhase::CopyPrepare, 1);
    // The caller already selected both views for bounds and adapter capacity.
    // Keep those original capabilities for every source and destination page.
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
                    auto backing = uniformSourceFor(targetState.allocationDescriptor(), sourceState.format.defaultPixel.view(), error);
                    auto previous = targetView.readResidentPage(to);
                    if (backing && d->provider->sourceMatchesReadGuard(backing, previous)) continue;
                    if (!backing || !batch->d->mutation.reserveLegacyMutationPage(to, error) ||
                        !batch->d->mutation.aliasPage(to, backing, error)) return false;
                }
                if (changed) changed->append(to.page);
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
                std::shared_ptr<const KisPageReplicaSource> backing;
                if (input.nativeGuard().isValid())
                    backing = d->provider->captureCpuReadSource(input.nativeGuard(), targetState.allocationDescriptor(), error);
                else if (input.genericLease())
                    backing = d->provider->captureCompletedTileSource(targetState.allocationDescriptor(),
                        source.d->provider->tileDataForLease(input.genericLease()->leaseId()), error);
                if (backing && d->provider->sourceMatchesReadGuard(backing, previous)) continue;
                if (!backing || !batch->d->mutation.reserveLegacyMutationPage(to, error) ||
                    !batch->d->mutation.aliasPage(to, backing, error)) return false;
                if (changed) changed->append(to.page);
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
            if (changed) changed->append(to.page);
        }
    }
    phase.next(KisPageStoreDiagnosticPhase::CopyFinish, 1);
    success = batch->finish(error);
    return success;
}
catch (const std::bad_alloc &) {
    if (changed) changed->clear();
    KisPageStoreDetail::setError(error, QStringLiteral("copy storage preparation was refused"));
    return false;
}

KisTileData *KisTiledDataManagerPageStoreBackend::tileDataForReadPage(const KisPageStoreReadPage &read) const
{
    return read.nativeGuard().isValid() ? d->provider->tileDataForCpuReadGuard(read.nativeGuard())
        : read.genericLease() ? d->provider->tileDataForLease(read.genericLease()->leaseId()) : nullptr;
}

TileLease KisTiledDataManagerPageStoreBackend::readCacheForPage(
    const KisPageStoreReadPage &read, TileLease reuse) const
{
    return read.nativeGuard().isValid()
        ? d->provider->acquireTileReadCache(read.nativeGuard(), std::move(reuse))
        : read.genericLease()
            ? d->provider->acquireTileReadCache(read.genericLease()->leaseId(), std::move(reuse))
            : TileLease{};
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
    QString *error) try
{
    {
        QMutexLocker lock(&d->mutex);
        if (d->cpuMutationBatches.find(QThread::currentThreadId()) != d->cpuMutationBatches.end()) {
            // Reject before joining the anonymous transaction: cancelling a
            // newly joined client would also poison its still-live parent.
            KisPageStoreDetail::setError(error, QStringLiteral("trim cannot nest inside a native CPU batch"));
            return false;
        }
    }
    // The manager rebuilds the entire compatibility index after trimming,
    // including retained pages. Completed native delivery still owns its
    // original claims even after leaving the thread/client batch registry.
    if (d->store->sessionStats().activeCpuWritePages != 0) {
        KisPageStoreDetail::setError(error, QStringLiteral("trim requires adapter delivery to finish"));
        return false;
    }
    auto batch = beginMutationBatch(error);
    KisPageStore *pageStore = store();
    if (!batch || !pageStore) return false;

    struct TrimPage {
        KisLogicalPageId page;
        QRect extent;
        QRect kept;
    };
    KisPageSnapshotArray<TrimPage> changes;
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

catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("trim storage preparation was refused"));
    return false;
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

    KisPageKeyStorage visiblePages;
    const KisImageEpochSnapshot snapshot = pageStore->captureCommittedEpoch();
    for (const KisPageVersion &version : snapshot.manifest) {
        if (version.key.surface == d->surface) {
            visiblePages.push_back(version.key);
        }
    }
    if (overlayTransaction.isValid()) {
        const KisPreparedPageSet prepared =
            pageStore->preparedPages(overlayTransaction);
        if (prepared.isValid()) {
            for (const KisPageKey &key : prepared.removedPages) {
                if (key.surface == d->surface) {
                    const auto found = std::find(visiblePages.begin(), visiblePages.end(), key);
                    if (found != visiblePages.end()) visiblePages.erase(found);
                }
            }
            for (const KisPreparedPageProof &proof : prepared.proofs) {
                if (proof.authority.version.key.surface == d->surface) {
                    const auto &key = proof.authority.version.key;
                    if (std::find(visiblePages.cbegin(), visiblePages.cend(), key) == visiblePages.cend())
                        visiblePages.push_back(key);
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
        view = d->selectReadViewLocked(oldData);
    }
    KisPageVersion version;
    return pageStore->resolvePageVersion(
               {d->surface, {column, row}}, view, &version) &&
           version.isValid() && !version.isDefaultPixel();
}

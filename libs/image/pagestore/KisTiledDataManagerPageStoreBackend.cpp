/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisTiledDataManagerPageStoreBackend.h"

#include "KisPageStoreDiagnostics_p.h"
#include "KisPageStoreIteratorReadScope_p.h"
#include "KisPageMetadataCoordinator.h"
#include "KisPageWriteCoordinator_p.h"
#include "KisTiles3PageReplicaProvider.h"
#include "KisStrokeJobFailureContext.h"
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
#include <charconv>
#include <cstring>
#include <limits>
#include <memory>
#include <map>
#include <utility>
#include <boost/intrusive/set.hpp>

namespace {

template<class Key, class Value>
using StorageMap = std::map<Key, Value, std::less<Key>,
    KisMutationStorageAllocator<std::pair<const Key, Value>>>;

// Discovery borrows the original operation, whether stack-bound or retained by
// real compatibility clients. It never owns another operation or paid node.
struct OperationLink : boost::intrusive::set_base_hook<boost::intrusive::optimize_size<true>>
{
    enum class Kind { Managed, Explicit, Iterator, RawUnused, RawExposed };
    explicit OperationLink(Kind kind) : kind(kind) {}
    // Indexed kind changes are protected by the backend gate. Raw exposure
    // changes only unindexed bodies; terminal cleanup never changes kind.
    Kind kind;
    const Qt::HANDLE thread = QThread::currentThreadId();
    struct Key {
        using type = Qt::HANDLE;
        const type &operator()(const OperationLink &link) const { return link.thread; }
    };
};
using OperationIndex = boost::intrusive::set<OperationLink,
    boost::intrusive::key_of_value<OperationLink::Key>,
    boost::intrusive::compare<std::less<Qt::HANDLE>>,
    boost::intrusive::constant_time_size<false>>;

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

}

class KisTiledDataManagerPageStoreLease final : public KisTilePageStoreLease, public KisPageProcessStorageObject
{
public:
    KisTiledDataManagerPageStoreLease(
        KisTiledDataManagerPageStoreBackend *backend,
        KisTileData *tile,
        KisCpuWriteGuard &&guard,
        boost::intrusive_ptr<KisTiledDataManagerPageStoreWriteBatch::Private> batch,
        TileLease readCache);
    ~KisTiledDataManagerPageStoreLease() override
    {
        finish();
    }

    KisTileData *tileData() const override { return m_tileData; }
    bool writable() const override { return m_native.isValid() && belongsToCurrentThread(); }
    void markDirty() override;
    TileLease takeReadCache() override
    {
        Q_ASSERT(!m_backend && !m_native.isValid());
        return std::move(m_readCache);
    }

    bool finish(TileLeaseRelease *afterUnlock = nullptr) override;
    bool cancelForBarrier();

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
    KisTiledDataManagerPageStoreBackend *m_backend = nullptr;
    KisTileData *m_tileData = nullptr;
    bool m_barrierPin = false;
    Qt::HANDLE m_thread = QThread::currentThreadId();
    KisTiledDataManagerPageStoreLease *m_previous = nullptr;
    KisTiledDataManagerPageStoreLease *m_next = nullptr;
    boost::intrusive_ptr<KisTiledDataManagerPageStoreWriteBatch::Private> m_batch;
    TileLease m_readCache;
    KisCpuWriteGuard m_native;
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
        return {};
    }

    // Caller holds mutex; the returned link does not retain an adapter body.
    OperationLink *currentOperation()
    {
        const auto it = operations.find(QThread::currentThreadId());
        return it == operations.end() ? nullptr : &*it;
    }

    mutable QMutex mutex;
    // Composition only; never transferred into Store's orphan/retry records.
    KisTiledDataManager *manager = nullptr;
    QMutex transactionMutex;
    QReadWriteLock publicationLock;
    std::shared_ptr<KisTiles3PageReplicaProvider> provider;
    KisPageStore::StoragePointer store{nullptr, &KisPageStore::destroyStorage};
    std::shared_ptr<KisPageStoreMementoManager> history;
    KisSurfaceId surface{1};
    KisPageStoreHistoryTransaction activeHistory;
    // Business closing phase belongs to this existing history owner. Pixel
    // lifetime/failure, entries and claims remain in the original session.
    enum class HistoryMutationPhase { None, Open, Closing, Sealed };
    HistoryMutationPhase historyMutationPhase = HistoryMutationPhase::None;
    KisPageMutationSession historyMutation;
    KisMementoSP currentMemento;
    StorageMap<const KisMemento *, KisPageStoreMemento> mementos;
    KisTiledDataManagerPageStoreLease *anonymousLeases = nullptr;
    OperationIndex operations;
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

class KisTiledDataManagerPageStoreWriteBatch::Private : public OperationLink, public KisPageProcessStorageObject
{
public:
    Private() : OperationLink(Kind::Explicit) {}
    static boost::intrusive_ptr<Private> create()
    { return boost::intrusive_ptr<Private>(new Private); }
    // The non-owning index remains linked until terminal cleanup detaches it.
    // Never resurrect a last-release body waiting to enter the backend gate.
    // Call under that gate; release the returned reference OUTSIDE the gate.
    static boost::intrusive_ptr<Private> retain(OperationLink *link)
    {
        if (!link || link->kind == Kind::Managed) return {};
        auto *body = static_cast<Private *>(link);
        auto count = body->references.load(std::memory_order_relaxed);
        while (count && !body->references.compare_exchange_weak(
                   count, count + 1, std::memory_order_acquire, std::memory_order_relaxed)) {}
        return count ? boost::intrusive_ptr<Private>(body, false) : boost::intrusive_ptr<Private>{};
    }
    bool rawLease() const { return kind == Kind::RawUnused || kind == Kind::RawExposed; }
    ~Private()
    {
        // Every borrowed native lease holds this original body. The final
        // release therefore runs only after the public batch and all guards
        // are gone. Never race an "abandoned" flag against a foreign unlock:
        // an unfinished operation is cancelled here, under unique ownership.
        if (backend) {
            if (clients != 0) failed = true;
            QString error;
            if (!finish(&error) && rawLease())
                KisStrokeJobFailureContext::reportFailure(error);
            // No client or guard survives this final reference. The
            // original mutation now owns any refused cancel/abort tail.
            if (backend) detachFromBackend();
        }
    }
    bool finishClient(bool succeeded, QString *error = nullptr, bool retainAdmission = false,
        const KisPageMutationSession::AdapterPreparation &prepareAdapter = {},
        const KisPageMutationSession::AdapterInstallation &installAdapter = {});
    bool replaceFullTile(qint32 column, qint32 row, KisTileData *tileData, bool sparseDefault, QString *error);
    bool finish(QString *error = nullptr, bool retainAdmission = false,
        const KisPageMutationSession::AdapterPreparation &prepareAdapter = {},
        const KisPageMutationSession::AdapterInstallation &installAdapter = {});
    bool cancelForBarrier(QString *error);
    void detachFromBackend();
    void unlinkLease(KisTiledDataManagerPageStoreLease *lease)
    {
        if (lease->m_previous) lease->m_previous->m_next = lease->m_next;
        else leases = lease->m_next;
        if (lease->m_next) lease->m_next->m_previous = lease->m_previous;
        lease->m_previous = lease->m_next = nullptr;
    }
    // Live capabilities only, not a second page/write set. Intrusive links
    // allocate nothing; the local gate also covers a foreign final unlock.
    std::atomic<size_t> references{0};
    QMutex leaseMutex;
    KisTiledDataManagerPageStoreLease *leases = nullptr;
    KisTiledDataManagerPageStoreBackend *backend = nullptr;
    bool failed = false;
    bool hasWorkingAdapters = false;
    quint64 clients = 1;
    KisPageMutationSession mutation;
    StorageMap<KisTileData *, std::shared_ptr<const KisPageReplicaSource>> sources;
    // Only actual provisional cold insertions. The original forward key and
    // candidate are retained, with their preallocated inverse GC capability;
    // warm-page identity continues to live solely in mutation's write set.
    std::vector<KisTileHashTable::PreparedTile,
        KisMutationStorageAllocator<KisTileHashTable::PreparedTile>> adapterInsertions;
};

void intrusive_ptr_add_ref(KisTiledDataManagerPageStoreWriteBatch::Private *body) noexcept
{ body->references.fetch_add(1, std::memory_order_relaxed); }
void intrusive_ptr_release(KisTiledDataManagerPageStoreWriteBatch::Private *body) noexcept
{
    if (body->references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete body;
}

// Managed operations own the original Session directly. This borrowed stack
// link expresses adapter nesting/barriers only; it is neither a write set nor
// another cancellation owner. A real legacy pointer creates its extension on
// demand and moves this SAME Session into it before exposing that pointer.
class KisTiledDataManagerPageStoreBackend::OperationFrame : public OperationLink
{
public:
    explicit OperationFrame(KisTiledDataManagerPageStoreBackend &owner)
        : OperationLink(Kind::Managed), owner(owner) {}
    OperationFrame(const OperationFrame &) = delete;
    OperationFrame &operator=(const OperationFrame &) = delete;
    ~OperationFrame()
    {
        if (legacyClient) legacy->finishClient(false);
        leave();
    }
    void leave()
    {
        if (is_linked()) {
            QMutexLocker lock(&owner.d->mutex);
            owner.d->operations.erase(owner.d->operations.iterator_to(*this));
        }
    }
    bool begin(QString *error, KisPageMutationSession *reservation = nullptr,
               KisPageMutationExecution *execution = nullptr)
    {
        {
            QMutexLocker lock(&owner.d->mutex);
            if (!owner.d->operations.insert(*this).second) {
                KisPageStoreDetail::setError(error, QStringLiteral("nested native CPU batch is not supported"));
                return false;
            }
        }
        original = owner.beginFrameSession(error, false, reservation, execution);
        return original.isActive();
    }
    KisPageMutationSession &session() { return legacy ? legacy->mutation : original; }
    bool isPresent() const { return bool(original.d) || bool(legacy); }
    boost::intrusive_ptr<KisTiledDataManagerPageStoreWriteBatch::Private> legacyForPage(
        const KisPageKey &key, QString *error, bool *present = nullptr, KisMutationPageEntry **admitted = nullptr)
    {
        // A real key must be admitted by this SAME Frame before funding a
        // compatibility client. Refusal neither promotes nor revives it.
        auto *entry = session().reserveLegacyMutationPage(key, error, present);
        if (!entry) return {};
        if (admitted) *admitted = entry;
        if (legacy) return legacy;
        using Extension = KisTiledDataManagerPageStoreWriteBatch::Private;
        auto prepared = Extension::create();
        QMutexLocker lock(&owner.d->mutex);
        if (!original.d || !is_linked()) return {};
        // All fallible preparation precedes this same-key, nonallocating
        // replacement. Exactly one original operation stays discoverable.
        owner.d->operations.replace_node(owner.d->operations.iterator_to(*this), *prepared);
        prepared->mutation = std::move(original);
        prepared->backend = &owner;
        legacy = std::move(prepared);
        legacyClient = true;
        return legacy;
    }
    bool finish(OperationDelivery *delivery, const AdapterCompletion &complete, QString *error)
    {
        const auto publish = [&](QString *failure) {
            bool accepted;
            if (legacy) {
                if (!std::exchange(legacyClient, false)) return false;
                accepted = legacy->finishClient(true, failure, true);
            } else accepted = original.sealForAdapterDelivery(failure)
                && owner.finishOwnedTransaction(original, true, failure);
            // Publish ends thread-level operation discovery. Exact page
            // claims stay with this same Frame through adapter delivery, so
            // the completion callback can start a disjoint operation.
            if (accepted) leave();
            return accepted;
        };
        // The callback invokes publish before visiting this borrowed view.
        // Successful delivery keeps the original rows/claims alive; no key
        // or presence projection is needed for the manager's installation.
        const auto changes = session().frozenAdapterChanges();
        const bool accepted = complete ? complete(publish, changes, error) : publish(error);
        if (accepted && delivery) {
            delivery->m_frame = std::move(original);
            delivery->m_legacy = std::move(legacy);
            legacyClient = false;
        }
        return accepted;
    }
private:
    KisTiledDataManagerPageStoreBackend &owner;
    KisPageMutationSession original;
    boost::intrusive_ptr<KisTiledDataManagerPageStoreWriteBatch::Private> legacy;
    bool legacyClient = false;
};

KisTiledDataManagerPageStoreLease::KisTiledDataManagerPageStoreLease(
    KisTiledDataManagerPageStoreBackend *backend, KisTileData *tile,
    KisCpuWriteGuard &&guard,
    boost::intrusive_ptr<KisTiledDataManagerPageStoreWriteBatch::Private> batch,
    TileLease readCache)
    : m_backend(backend), m_tileData(tile), m_batch(std::move(batch)),
      m_readCache(std::move(readCache)),
      m_native(std::move(guard))
{
    QMutexLocker lock(&m_batch->leaseMutex);
    if (m_batch->rawLease()) {
        if (m_batch->mutation.ownsTransaction()) m_backend->registerAnonymousLease(this);
    } else {
        m_next = m_batch->leases;
        if (m_next) m_next->m_previous = this;
        m_batch->leases = this;
    }
}

void KisTiledDataManagerPageStoreLease::markDirty()
{
    if (m_batch && m_batch->kind == KisTiledDataManagerPageStoreWriteBatch::Private::Kind::RawUnused)
        m_batch->kind = KisTiledDataManagerPageStoreWriteBatch::Private::Kind::RawExposed;
}

bool KisTiledDataManagerPageStoreLease::finish(TileLeaseRelease *afterUnlock)
{
    if (!m_batch) { releaseBarrierPin(); return true; }
    auto batch = std::move(m_batch);
    const bool raw = batch->rawLease();
    {
        QMutexLocker lock(&batch->leaseMutex);
        // Return the actual RAM pin. Outside a clear barrier, the segment
        // keeps its parked writer reservation until its explicit end.
        m_native = {};
        releaseBarrierPin();
        if (raw) {
            if (m_backend && batch->mutation.ownsTransaction()) m_backend->unregisterAnonymousLease(this);
            batch->clients = 0;
            if (batch->kind == KisTiledDataManagerPageStoreWriteBatch::Private::Kind::RawUnused)
                m_readCache.reset();
        } else batch->unlinkLease(this);
        m_backend = nullptr;
    }
    // A Tile unlock also holds its swap barrier. Move this existing reference
    // to that caller's stack so final batch cancellation cannot enter an
    // adapter under the barrier. Standalone lease callers remain synchronous.
    if (afterUnlock) {
        Q_ASSERT(!*afterUnlock);
        *afterUnlock = TileLeaseRelease(batch.detach(), [](void *body) {
            intrusive_ptr_release(static_cast<KisTiledDataManagerPageStoreWriteBatch::Private *>(body));
        });
    } else if (raw) {
        const bool success = batch->finish();
        if (!success) m_readCache.reset();
        return success;
    }
    return true;
}

bool KisTiledDataManagerPageStoreLease::cancelForBarrier()
{
    if (!m_backend || !m_batch || !m_batch->rawLease() || !m_batch->mutation.ownsTransaction()) return false;
    QMutexLocker lock(&m_batch->leaseMutex);
    if (!retainBarrierPin()) return false;
    m_native = {};
    if (!m_backend->completeAdapters(*m_batch, false)) return false;
    if (!m_batch->mutation.cancel()) return false;
    if (!m_backend->finishOwnedTransaction(m_batch->mutation, false)) return false;
    m_readCache.reset();
    m_backend->unregisterAnonymousLease(this);
    m_backend = nullptr;
    m_batch->detachFromBackend();
    return true;
}

bool KisTiledDataManagerPageStoreWriteBatch::Private::cancelForBarrier(QString *error)
{
    QMutexLocker leaseLock(&leaseMutex);
    if (!backend || kind != Kind::Iterator || thread != QThread::currentThreadId()) return false;
    for (auto *lease = leases; lease; lease = lease->m_next) {
        if (!lease->retainBarrierPin()) {
            for (auto *pinned = leases; pinned != lease; pinned = pinned->m_next)
                pinned->releaseBarrierPin();
            KisPageStoreDetail::setError(error, QStringLiteral("clear cannot retain legacy iterator storage"));
            return false;
        }
    }
    for (auto *lease = leases; lease; lease = lease->m_next) lease->m_native = {};
    if (!backend->completeAdapters(*this, false)) return false;
    if (!mutation.cancel()) {
        failed = true;
        KisPageStoreDetail::setError(error, QStringLiteral("clear cannot cancel the iterator mutation"));
        return false;
    }
    if (!backend->finishOwnedTransaction(mutation, false, error)) {
        failed = true;
        return false;
    }
    sources.clear();
    detachFromBackend();
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
    if (!d || !d->batch || !d->batch->d) return false;
    const auto &batch = d->batch->d;
    QMutexLocker lock(&batch->leaseMutex);
    return batch->backend && !batch->failed && batch->clients != 0;
}

KisTiledDataManagerPageStoreWriteBatch::
KisTiledDataManagerPageStoreWriteBatch(boost::intrusive_ptr<Private> shared)
    : d(std::move(shared))
{
}

KisTiledDataManagerPageStoreWriteBatch::
~KisTiledDataManagerPageStoreWriteBatch()
{
    if (!m_clientFinished) cancel();
}

bool KisTiledDataManagerPageStoreWriteBatch::finish(QString *error,
    const KisPageMutationSession::AdapterPreparation &prepareAdapter,
    const KisPageMutationSession::AdapterInstallation &installAdapter)
{
    if (m_clientFinished || !d) return false;
    m_clientFinished = true;
    if (prepareAdapter || installAdapter)
        return d->finishClient(true, error, true, prepareAdapter, installAdapter);
    auto completing = std::move(d);
    return completing->finishClient(true, error);
}

bool KisTiledDataManagerPageStoreWriteBatch::cancel()
{
    if (m_clientFinished || !d) return false;
    m_clientFinished = true;
    auto completing = std::move(d);
    return completing->finishClient(false);
}

bool KisTiledDataManagerPageStoreWriteBatch::Private::finishClient(
    bool succeeded, QString *error, bool retainAdmission,
    const KisPageMutationSession::AdapterPreparation &prepareAdapter,
    const KisPageMutationSession::AdapterInstallation &installAdapter)
{
    QMutexLocker leaseLock(&leaseMutex);
    KisTiledDataManagerPageStoreBackend *owner = backend;
    if (!owner) return succeeded && !failed;
    bool finalClient = false;
    bool clientSucceeded = false;
    {
        QMutexLocker lock(&owner->d->mutex);
        if (retainAdmission && (clients != 1 || kind != Kind::Explicit || leases)) {
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
    return finalClient ? finish(error, retainAdmission, prepareAdapter, installAdapter) : clientSucceeded;
}

bool KisTiledDataManagerPageStoreWriteBatch::Private::finish(QString *error, bool retainAdmission,
    const KisPageMutationSession::AdapterPreparation &prepareAdapter,
    const KisPageMutationSession::AdapterInstallation &installAdapter)
{
    if (!backend) return !failed;
    if (!mutation.transaction().isValid()) return false;
    const bool mutationActive = mutation.isActive();
    if (!mutationActive && !failed) return false;
    if (failed) KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore private mutation batch cancelled"));
    const bool unused = kind == Kind::RawUnused;
    const bool releaseWorkingAdmission = hasWorkingAdapters && !retainAdmission;
    retainAdmission = retainAdmission || hasWorkingAdapters;
    const bool sealed = mutationActive && !failed && !unused && (kind != Kind::Explicit
        ? retainAdmission ? mutation.sealForLegacyAdapterDelivery(error) : mutation.sealForLegacyUnlock(error)
        : retainAdmission ? mutation.sealForAdapterDelivery(error, prepareAdapter) : mutation.seal(error));
    if (!sealed) {
        failed = failed || !unused;
        if (!backend->completeAdapters(*this, false)) return false;
        if (!mutation.cancel()) {
            {
                QMutexLocker leaseLock(&leaseMutex);
                // A real borrowed lease still owns this batch and its thread
                // slot. Its final lock-free release will finish cancellation.
                if (leases) return false;
            }
            // Adapter responsibility is complete. Transfer the original
            // scope now, even if the finished facade remains alive. A live
            // native guard retains that same scope until its own last use.
            failed = true;
            sources.clear();
            mutation = {};
            detachFromBackend();
            return false;
        }
    }
    // An owned batch publishes only its own delta. A confirmed cancellation
    // leaves it empty; explicit history keeps its previously sealed segments.
    const bool terminal = backend->finishOwnedTransaction(mutation, sealed || !failed,
                                                        (sealed || !failed) ? error : nullptr);
    failed = failed || !terminal;
    if (sealed && !backend->completeAdapters(*this, terminal, installAdapter)) return false;
    if (releaseWorkingAdmission || (terminal && !retainAdmission)) {
        sources.clear();
        mutation = {}; // Accepted final delivery ends this facade's ownership.
    }
    detachFromBackend();
    return !failed;
}

bool KisTiledDataManagerPageStoreBackend::completeAdapters(
    KisTiledDataManagerPageStoreWriteBatch::Private &batch, bool published,
    const KisPageMutationSession::AdapterInstallation &installAdapter)
{
    if (!batch.hasWorkingAdapters && !(published && installAdapter)) return true;
    auto *manager = d->manager;
    Q_ASSERT(manager);
    // Callers may already own the manager gate. Original page claims and the
    // index/extent's own locks protect this sparse getTile lifecycle; never
    // acquire a second manager gate, or call adapters inside a Store gate.
    KisPageStoreDiagnosticTimer phase(store(), KisPageStoreDiagnosticPhase::MutationIndexRefresh, 1);
    phase.next(KisPageStoreDiagnosticPhase::MutationAdapterInstall, 1);
    if (published && installAdapter) {
        // The caller prepared all final changes before acceptance. This one
        // install also invalidates raw partial-write caches; do not visit D twice.
        installAdapter(batch.mutation.frozenAdapterChanges());
    } else if (!batch.mutation.visitAdapterPages([&](const KisPageVersion &version, void *location) {
        auto *tile = location ? static_cast<KisTile *>(location)
            : manager->m_hashTable->borrowTileForPreparedUpdate(version.key.page.column, version.key.page.row);
        if (tile) {
            QMutexLocker lock(&tile->m_swapBarrierLock);
            // A successfully published raw cache already has this exact
            // identity. Keep it ready instead of forcing a new resolve/pin
            // on every following warm getTile. A reader may have selected the
            // old current during root preparation, so compare the actual cache.
            if (published && !version.isDefaultPixel() &&
                d->provider->readCacheMatchesVersion(tile->m_pageStoreLeases[KisTile::ReadLease].get(), version)) return;
            tile->m_pageStoreNativeReadReady.storeRelease(0);
        }
    })) return false;
    if (!published) {
        for (auto &prepared : batch.adapterInsertions)
            if (manager->m_hashTable->undoPreparedTileInsertion(prepared))
                manager->m_extentManager.notifyTileRemoved(prepared.col(), prepared.row());
    }
    // Dispose original prepared keys/candidates while their claim still
    // protects delivery, even when a finished public scope stays alive.
    batch.adapterInsertions = decltype(batch.adapterInsertions){};
    // Physical cancellation can still refuse. No manager capability follows
    // the original mutation into orphan retirement after this delivery.
    batch.hasWorkingAdapters = false;
    phase.next(KisPageStoreDiagnosticPhase::MutationAdapterInstalled, 1);
    return true;
}

void KisTiledDataManagerPageStoreWriteBatch::Private::detachFromBackend()
{
    // Keep this node until completion or the final reference. An orphan
    // tail then retains its original Store claims/transaction, not an expired
    // backend slot that could never be removed after physical cancellation.
    QMutexLocker lock(&backend->d->mutex);
    if (is_linked()) backend->d->operations.erase(backend->d->operations.iterator_to(*this));
    backend = nullptr;
    clients = 0;
}

bool KisTiledDataManagerPageStoreWriteBatch::replaceFullTile(
    qint32 column, qint32 row, KisTileData *tileData, bool sparseDefault, QString *error)
{
    return d && d->replaceFullTile(column, row, tileData, sparseDefault, error);
}

bool KisTiledDataManagerPageStoreWriteBatch::Private::replaceFullTile(
    qint32 column,
    qint32 row,
    KisTileData *tileData,
    bool sparseDefault,
    QString *error) try
{
    if (!backend || !mutation.transaction().isValid() || failed) {
        KisPageStoreDetail::setError(error, QStringLiteral(
            "tiles3 PageStore write batch is unavailable"));
        return false;
    }
    const KisPageKey key{backend->surface(), {column, row}};
    bool staged = mutation.reserveLegacyMutationPage(key, error);
    if (staged && sparseDefault) staged = mutation.removePage(key, error);
    else if (staged) {
        const auto found = sources.find(tileData);
        auto source = found != sources.end() ? found->second : std::shared_ptr<const KisPageReplicaSource>{};
        if (!source) {
            const auto overlay = KisPageReadView::transactionOverlay(mutation.transaction().id);
            KisSurfaceEpochState state;
            if (backend->store()->resolveSurfaceState(key.surface, overlay, &state))
                source = backend->d->provider->captureCompletedTileSource(state.allocationDescriptor(), tileData, error);
            if (source) sources.emplace(tileData, source);
        }
        staged = source && mutation.aliasPage(key, source, error);
    }
    if (!staged) failed = true;
    return staged;
}
catch (const std::bad_alloc &) {
    failed = true;
    KisPageStoreDetail::setError(error, QStringLiteral("tile source directory storage was refused"));
    return false;
}

KisTiledDataManagerPageStoreBackend::KisTiledDataManagerPageStoreBackend(KisTiledDataManager *manager)
    : d(new Private)
{
    d->manager = manager;
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
    char depth[std::numeric_limits<quint32>::digits10 + 1];
    const auto converted = std::to_chars(depth, depth + sizeof(depth), pixelSize * 8);
    Q_ASSERT(converted.ec == std::errc{});
    surface.format.colorDepthId = KisPageByteArray(depth, converted.ptr - depth);
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
    QString *error, KisPageSnapshotArray<KisPageVersion> *adapterPages) try
{
    if (adapterPages) adapterPages->clear();
    QMutexLocker sourceTransactionLocker(&source.d->transactionMutex);
    KisPageStore *sourceStore = nullptr;
    KisPageStoreHistoryTransaction sourceHistory;
    {
        QMutexLocker sourceLocker(&source.d->mutex);
        if (!source.d->operational) {
            KisPageStoreDetail::setError(error, QStringLiteral("tiles3 clone source is unavailable"));
            return false;
        }
        if (source.d->store->sessionStats().activeTransactions != qsizetype(source.d->activeHistory.isValid())) {
            KisPageStoreDetail::setError(error, QStringLiteral(
                "tiles3 clone source has active anonymous writes"));
            return false;
        }
        sourceStore = source.d->store.get();
        sourceHistory = source.d->activeHistory;
    }

    const auto selector = sourceHistory.isValid()
        ? KisPageReadView::transactionOverlay(sourceHistory.transaction.id) : KisPageReadView{};
    auto capturedSource = sourceStore->captureReadView(selector, error);
    if (!capturedSource.isValid()) return false;
    // The source already maintains derived bounds at publication. Export and
    // replica adoption consume one cut, including a history's retained base.
    auto snapshot = sourceStore->snapshotForClone(capturedSource, error);
    if (!snapshot.epoch.isValid()) return false;

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
    for (const KisPageVersion &version : std::as_const(snapshot.manifest)) {
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
    // Transfer the original initialization output to its remaining consumer.
    // Standalone backends have no compatibility index to construct.
    if (adapterPages) *adapterPages = std::move(snapshot.manifest);
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

KisTileSP KisTiledDataManagerPageStoreBackend::ensureWritablePage(
    qint32 column, qint32 row, QString *error) try
{
    auto *manager = d->manager;
    if (!manager) return {};
    const KisPageKey key{surface(), {column, row}};
    bool present = false;
    KisMutationPageEntry *entry = nullptr;
    boost::intrusive_ptr<KisTiledDataManagerPageStoreWriteBatch::Private> shared;
    OperationFrame *activeFrame = nullptr;
    {
        QMutexLocker lock(&d->mutex);
        if (auto *operation = d->currentOperation()) {
            if (operation->kind == OperationLink::Kind::Managed)
                activeFrame = static_cast<OperationFrame *>(operation);
            else {
                shared = KisTiledDataManagerPageStoreWriteBatch::Private::retain(operation);
                if (!shared || shared->failed || shared->clients == 0) {
                    KisPageStoreDetail::setError(error, QStringLiteral("writable tile cannot revive a finished batch"));
                    return {};
                }
            }
        }
    }
    if (activeFrame) {
        shared = activeFrame->legacyForPage(key, error, &present, &entry);
        if (!shared) return {};
    }
    OperationFrame frame(*this);
    if (!shared) {
        auto range = store()->reserveLegacyPage(key, error);
        if (!range.d || !frame.begin(error, &range)) return {};
    }
    auto &mutation = shared ? shared->mutation : frame.session();
    // Admission precedes the presence decision and all adapter preparation.
    // Reuse private aliases/writes as well as the transaction's sealed pages.
    if (!activeFrame) entry = mutation.reserveLegacyMutationPage(key, error, &present);
    if (!entry) return {};
    const bool provisional = shared && !present;
    KisTile *existing = nullptr;
    auto prepared = manager->m_hashTable->prepareTileUpdate(column, row,
        provisional ? KisTileHashTable::Update::MayRemove : KisTileHashTable::Update::Present, &existing);
    if (!manager->m_extentManager.prepareTileRange(QRect(column, row, 1, 1))) {
        KisPageStoreDetail::setError(error, QStringLiteral("Extent storage preparation was rejected"));
        return {};
    }
    auto *installation = &prepared;
    if (provisional && prepared) {
        // Capacity and inverse capability exist before the alias or working
        // wrapper is accepted. Reuse this same token after failure.
        shared->adapterInsertions.push_back(std::move(prepared));
        installation = &shared->adapterInsertions.back();
    }
    KisTileData *defaultData = manager->m_hashTable->refAndFetchDefaultTileData();
    const auto releaseDefault = qScopeGuard([&] { if (defaultData) defaultData->deref(); });
    if (!defaultData) return {};
    if (!present) {
        if (shared) {
            if (!shared->replaceFullTile(column, row, defaultData, false, error)) return {};
        } else {
            KisSurfaceEpochState state;
            if (!store()->resolveSurfaceState(surface(),
                    KisPageReadView::transactionOverlay(mutation.transaction().id), &state)) return {};
            auto source = d->provider->captureCompletedTileSource(state.allocationDescriptor(), defaultData, error);
            if (!source || !mutation.aliasPage({surface(), {column, row}}, source, error)) return {};
        }
    }
    if (!shared && !frame.finish(nullptr, {}, error)) return {};
    bool created = false;
    KisTileSP tile = existing;
    if (!tile) tile = manager->m_hashTable->installPreparedTile(*installation, created);
    Q_ASSERT(tile);
    if (shared) { shared->hasWorkingAdapters = true; entry->adapterLocation() = tile.data(); }
    tile->setPageStoreBridge(this, false);
    tile->invalidatePageStoreReadCache();
    if (created) manager->m_extentManager.notifyTileAdded(column, row);
    if (!present) tile->setPageStoreNativeReadReady();
    return tile;
}
catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("writable tile preparation was refused"));
    return {};
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

KisPageMutationSession KisTiledDataManagerPageStoreBackend::beginFrameSession(
    QString *error, bool requireIdle, KisPageMutationSession *reservation, KisPageMutationExecution *execution)
{
    bool owned = false;
    const auto transaction = writableTransaction(&owned, error, requireIdle);
    if (!transaction.isValid()) return {};
    auto frame = reservation
        ? d->store->beginMutation(transaction, std::move(*reservation), execution, error)
        : d->store->beginMutation(transaction, error);
    if (!frame.isActive()) {
        if (owned && !d->store->abort(transaction))
            qFatal("Empty owned transaction could not be released");
        return {};
    }
    if (owned) frame.takeTransactionOwnership();
    return frame;
}

std::unique_ptr<KisTiledDataManagerPageStoreWriteBatch>
KisTiledDataManagerPageStoreBackend::beginMutationBatch(QString *error, bool requireIdle) try
{
    {
        QMutexLocker lock(&d->mutex);
        if (d->currentOperation()) {
            KisPageStoreDetail::setError(error, QStringLiteral("nested native CPU batch is not supported"));
            return {};
        }
    }
    auto batch = std::unique_ptr<KisTiledDataManagerPageStoreWriteBatch>(
        new KisTiledDataManagerPageStoreWriteBatch(boost::intrusive_ptr<KisTiledDataManagerPageStoreWriteBatch::Private>{}));
    batch->d = KisTiledDataManagerPageStoreWriteBatch::Private::create();
    batch->d->mutation = beginFrameSession(error, requireIdle);
    if (!batch->d->mutation.isActive()) return {};
    batch->d->backend = this;
    QMutexLocker lock(&d->mutex);
    if (!d->operations.insert(*batch->d).second) return {};
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
    boost::intrusive_ptr<KisTiledDataManagerPageStoreWriteBatch::Private> shared;
    {
        QMutexLocker lock(&d->mutex);
        if (auto *operation = d->currentOperation()) {
            shared = KisTiledDataManagerPageStoreWriteBatch::Private::retain(operation);
            // Zero clients means the last scope has started terminal sealing.
            // Keep the original node as a barrier; never revive that body.
            if (!shared || shared->kind != KisTiledDataManagerPageStoreWriteBatch::Private::Kind::Iterator
                || shared->failed || shared->clients == 0) {
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
            batch->d->kind = KisTiledDataManagerPageStoreWriteBatch::Private::Kind::Iterator;
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
    QString *error, bool requireIdle)
{
    QMutexLocker transactionLocker(&d->transactionMutex);
    QMutexLocker locker(&d->mutex);
    if (!d->operational || !owned) {
        KisPageStoreDetail::setError(error, QStringLiteral(
            "tiles3 PageStore write owner is invalid"));
        return {};
    }
    if (requireIdle && d->store->sessionStats().activeTransactions != 0) {
        KisPageStoreDetail::setError(error, QStringLiteral("tile import requires an idle store"));
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
    *owned = true;
    const auto transaction = d->store->beginCurrentTransaction();
    if (!transaction.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore transaction failed"));
        return {};
    }
    KisPageStoreDetail::setError(error, {});
    return transaction;
}

bool KisTiledDataManagerPageStoreBackend::finishOwnedTransaction(
    KisPageMutationSession &mutation, bool publish,
    QString *error)
{
    if (!mutation.ownsTransaction()) return true;
    QMutexLocker transactionLocker(&d->transactionMutex);
    const bool committed = mutation.finishOwnedTransaction(publish);
    KisPageStoreDetail::setError(error, committed
        ? QString() : QStringLiteral("tiles3 PageStore commit failed"));
    return committed;
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
    boost::intrusive_ptr<KisTiledDataManagerPageStoreWriteBatch::Private> current;
    {
        QMutexLocker locker(&d->mutex);
        // clear is an externally sequenced operation, not permission to
        // cancel another running worker or a reentrant native operation.
        for (auto &operation : d->operations) {
            if (operation.kind == OperationLink::Kind::Managed) {
                KisPageStoreDetail::setError(error, QStringLiteral("clear requires managed operations to finish"));
                return false;
            }
            if (operation.thread != QThread::currentThreadId()) {
                KisPageStoreDetail::setError(error, QStringLiteral("clear requires the other CPU batches to finish"));
                return false;
            }
            // At most one node can match this thread. Its borrowed reference
            // outlives this gate even on refusal or a concurrent final unlock.
            current = KisTiledDataManagerPageStoreWriteBatch::Private::retain(&operation);
            if (!current || current->kind != KisTiledDataManagerPageStoreWriteBatch::Private::Kind::Iterator || current->clients == 0) {
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
    current.reset();
    for (;;) {
        boost::intrusive_ptr<KisTiledDataManagerPageStoreWriteBatch::Private> batch;
        {
            QMutexLocker locker(&d->mutex);
            if (d->operations.empty()) break;
            batch = KisTiledDataManagerPageStoreWriteBatch::Private::retain(&*d->operations.begin());
        }
        if (!batch || !batch->cancelForBarrier(error)) return false;
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
    const auto activity = d->store->sessionStats();
    if (activity.activeTransactions != qsizetype(d->activeHistory.isValid()) ||
        d->anonymousLeases || activity.activeCpuWritePages != 0) {
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

bool KisTiledDataManagerPageStoreBackend::resolveCurrentSurfaceState(
    KisSurfaceEpochState *state, QString *error) const
{
    QReadLocker publicationLocker(&d->publicationLock);
    KisPageReadView selector;
    {
        QMutexLocker locker(&d->mutex);
        if (!d->operational) return false;
        selector = d->selectReadViewLocked(false);
    }
    const bool resolved = d->store->resolveSurfaceState(d->surface, selector, state);
    KisPageStoreDetail::setError(error, resolved ? QString{} : QStringLiteral("tiles3 surface state is unavailable"));
    return resolved;
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

bool KisTiledDataManagerPageStoreBackend::hasCurrentThreadIteratorWrites() const
{
    // The explicit iterator mutation scope is also the single same-thread
    // discovery source for unpublished compatibility bytes.
    boost::intrusive_ptr<KisTiledDataManagerPageStoreWriteBatch::Private> batch;
    QMutexLocker locker(&d->mutex);
    batch = KisTiledDataManagerPageStoreWriteBatch::Private::retain(d->currentOperation());
    return batch && batch->kind == KisTiledDataManagerPageStoreWriteBatch::Private::Kind::Iterator
        && !batch->failed && batch->clients != 0;
}

bool KisTiledDataManagerPageStoreBackend::readBytes(
    quint8 *data, qint32 x, qint32 y, qint32 width, qint32 height,
    qint32 dataRowStride, QString *error) const
{
    if (!data || width <= 0 || height <= 0) { KisPageStoreDetail::setError(error, {}); return true; }
    KisPageStore *pageStore = store();
    using Phase = KisPageStoreDiagnosticPhase;
    KisPageStoreDiagnosticTimer diagnostic(pageStore, Phase::ReadBytesCapture, 1);
    if (qint64(x) + width - 1 > std::numeric_limits<qint32>::max()
        || qint64(y) + height - 1 > std::numeric_limits<qint32>::max()) return false;
    const QRect pages(QPoint(pageCoordinate(x, KisTileData::WIDTH), pageCoordinate(y, KisTileData::HEIGHT)),
        QPoint(pageCoordinate(qint32(qint64(x) + width - 1), KisTileData::WIDTH),
               pageCoordinate(qint32(qint64(y) + height - 1), KisTileData::HEIGHT)));
    QReadLocker publicationLocker(&d->publicationLock);
    KisPageReadView selector;
    {
        QMutexLocker lock(&d->mutex);
        if (!d->operational) return false;
        selector = d->selectReadViewLocked(false);
    }
    KisCapturedReadView view;
    // These short selections die before the exact view, including failures.
    // They grant no pin; acquireRead still validates the physical identity.
    KisCpuReadBinding bindings[16];
    quint32 indices[16];
    view = pageStore->captureReadRange(d->surface, pages, selector, error, bindings, 16);
    publicationLocker.unlock();
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
    size_t ordinal = 0;
    const size_t pageCount = size_t(pages.width()) * size_t(pages.height());
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
            if (ordinal && ordinal % 16 == 0) {
                // Last references must be dropped outside metadata gates.
                for (auto &binding : bindings) binding = {};
                view.selectReadWindow(ordinal, std::min<size_t>(16, pageCount - ordinal), indices, bindings);
            }
            const auto copy = [&](const void *bytes, quint32 sourceStride) {
                // Snapshot the request before copying. A type-erased callback
                // cannot otherwise prove that memcpy leaves its captures alone.
                auto *target = data + dy * rowStride + dx * state.format.pixelStride;
                const auto *source = static_cast<const quint8 *>(bytes)
                    + localY * sourceStride + localX * state.format.pixelStride;
                const auto targetStride = rowStride;
                const auto rowCount = rows;
                const auto bytesPerRow = size_t(columns * state.format.pixelStride);
                for (qint64 line = 0; line < rowCount; ++line) {
                    std::memcpy(target + line * targetStride,
                        source + line * sourceStride, bytesPerRow);
                }
            };
            KisCpuResidentReadStatus status;
            if (!view.visitResidentPage(key, bindings[ordinal % 16], copy, &status)) {
                if (status != KisCpuResidentReadStatus::NonResident
                    && status != KisCpuResidentReadStatus::UnsupportedProvider
                    && status != KisCpuResidentReadStatus::BindingUnavailable) {
                    KisPageStoreDetail::setError(error, QStringLiteral("resident read was rejected"));
                    return false;
                }
                KisPageStoreReadPage read(pageStore, view, key, error, KisPagePriority::Normal, false);
                if (!read.data()) return false;
                copy(read.data(), read.rowStride());
            }
            ++ordinal;
            dx += columns;
        }
        dy += rows;
    }
    diagnostic.next(Phase::ReadBytesRelease, 1);
    for (auto &binding : bindings) binding = {};
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
    Access access,
    TileLease *readCache) try
{
    // A read view that names a transaction must remain valid until PageStore
    // has converted it into an active read capability. Readers share this
    // publication gate; only the final publisher/aborter takes it exclusively.
    const bool selection = access == Access::ReadSelection;
    if (!selection && access != Access::ReadCache && access != Access::Write) return {};
    QReadLocker publicationLocker(selection ? &d->publicationLock : nullptr);
    KisPageStore *pageStore = nullptr;
    std::shared_ptr<KisTiles3PageReplicaProvider> provider;
    KisPageReadView readView;
    {
        QMutexLocker locker(&d->mutex);
        if (!d->operational) {
            return {};
        }
        pageStore = d->store.get();
        provider = d->provider;
        readView = d->selectReadViewLocked(false);
    }

    const KisPageKey key{d->surface, {column, row}};
    if (selection) {
        // Publication invalidates selection, not the original binding hint.
        // Reuse it only after selecting the current Value and successfully
        // pinning the exact allocation incarnation. A concurrent retag or a
        // different Value falls through to the ordinary captured selection.
        if (readCache && *readCache && !(*readCache)->readPinned()) {
            KisPageVersion version;
            KisSurfaceEpochState state;
            if (pageStore->resolveTileReadIdentity(key, readView, &version, &state) &&
                provider->readCacheMatchesVersion(readCache->get(), version) &&
                (*readCache)->repinRead() == KisTilePageStoreLease::ReadPinResult::Ready)
                return std::move(*readCache);
        }
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
    QString transactionError;
    boost::intrusive_ptr<KisTiledDataManagerPageStoreWriteBatch::Private> batch;
    OperationFrame *activeFrame = nullptr;
    {
        QMutexLocker lock(&d->mutex);
        if (auto *operation = d->currentOperation()) {
            if (operation->kind == OperationLink::Kind::Managed)
                activeFrame = static_cast<OperationFrame *>(operation);
            else {
                batch = KisTiledDataManagerPageStoreWriteBatch::Private::retain(operation);
                if (!batch || batch->failed || batch->clients == 0) return {};
            }
        }
        mutation = batch ? &batch->mutation : nullptr;
    }
    if (access == Access::ReadCache) {
        // An idle wrapper is an expected-identity hint, not a fresh selector.
        // Only the original live iterator segment may lend its parked target.
        // Check before beginWrite: a read must never prepare a new generation.
        if (!batch || batch->kind != KisTiledDataManagerPageStoreWriteBatch::Private::Kind::Iterator || !readCache || !*readCache ||
            (*readCache)->readPinned() || !provider->readCacheMatchesVersion(
                readCache->get(), mutation->legacyMutationPageVersion(key))) return {};
    }
    if (activeFrame) {
        batch = activeFrame->legacyForPage(key, &transactionError);
        if (!batch) return {};
        mutation = &batch->mutation;
    }

    bool prepared = false;
    bool accepted = false;
    auto returnClient = qScopeGuard([&] {
        if (batch && prepared && !accepted) {
            QMutexLocker leaseLock(&batch->leaseMutex);
            QMutexLocker lock(&d->mutex);
            batch->failed = true;
        }
    });
    if (!mutation) {
        auto range = pageStore->reserveLegacyPage(key, &transactionError);
        if (!range.d) return {};
        batch = KisTiledDataManagerPageStoreWriteBatch::Private::create();
        batch->mutation = beginFrameSession(&transactionError, false, &range);
        if (!batch->mutation.isActive()) return {};
        batch->backend = this;
        batch->kind = KisTiledDataManagerPageStoreWriteBatch::Private::Kind::RawUnused;
        mutation = &batch->mutation;
    } else if (access == Access::Write && !activeFrame
        && !mutation->reserveLegacyMutationPage(key, &transactionError)) {
        if (batch) qWarning() << "PageStore CPU mutation acquisition failed:" << transactionError;
        return {};
    }
    auto guard = mutation->beginWrite(key, &transactionError);
    prepared = guard.isValid();
    if (!guard.isValid()) {
        if (batch && access == Access::Write)
            qWarning() << "PageStore CPU mutation acquisition failed:" << transactionError;
        return {};
    }
    if (access == Access::ReadCache &&
        !provider->readCacheMatchesVersion(readCache->get(), guard.version())) return {};
    KisTileData *tile = guard.isValid() ? provider->tileDataForCpuWriteGuard(guard) : nullptr;
    if (!tile) return {};
    TileLease futureRead;
    if (readCache) {
        TileLease reuse;
        if (*readCache && !(*readCache)->readPinned()) reuse = std::move(*readCache);
        futureRead = provider->prepareTileReadCache(guard, std::move(reuse));
        if (!futureRead) return {};
    }
    auto lease = std::make_unique<KisTiledDataManagerPageStoreLease>(
        this, tile, std::move(guard), batch, std::move(futureRead));
    if (readCache && d->manager) batch->hasWorkingAdapters = true;
    accepted = true;
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
        d->store->sessionStats().activeTransactions != 0) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore history is unavailable"));
        return {};
    }
    KisMementoSP candidate = KisMemento::createPageStoreMemento();
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
        !d->operational || !d->operations.empty() || d->anonymousLeases) {
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

bool KisTiledDataManagerPageStoreBackend::checkpointHistoryMutation(
    const KisMementoSP &owner, QString *error,
    const KisPageMutationSession::AdapterPreparation &prepareAdapter,
    const KisPageMutationSession::AdapterInstallation &installAdapter) try
{
    QMutexLocker transactionLocker(&d->transactionMutex);
    QWriteLocker publicationLocker(&d->publicationLock);
    QMutexLocker locker(&d->mutex);
    if (!owner || d->currentMemento != owner || !d->activeHistory.isValid() ||
        d->historyMutationPhase != Private::HistoryMutationPhase::Open) {
        KisPageStoreDetail::setError(error, QStringLiteral("checkpoint history owner is unavailable"));
        return {};
    }
    locker.unlock();
    return d->historyMutation.checkpointForAdapterDelivery(error, prepareAdapter, installAdapter);
}
catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("checkpoint adapter storage was refused"));
    return {};
}

bool KisTiledDataManagerPageStoreBackend::commitHistory(
    const quint8 *defaultPixel,
    quint32 pixelSize,
    QString *error,
    const KisPageMutationSession::AdapterPreparation &prepareAdapter,
    const KisPageMutationSession::AdapterInstallation &installAdapter) try
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
    using Phase = Private::HistoryMutationPhase;
    const bool persistent = d->historyMutationPhase != Phase::None;
    if (persistent && d->historyMutationPhase != Phase::Sealed) {
        d->historyMutationPhase = Phase::Closing;
        locker.unlock();
        if (!d->historyMutation.sealForLegacyUnlock(error, prepareAdapter, installAdapter)) return false;
        locker.relock();
        d->historyMutationPhase = Phase::Sealed;
    }
    locker.unlock();
    if (d->store->sessionStats().activeCpuWritePages != 0) {
        KisPageStoreDetail::setError(error, QStringLiteral("history commit requires adapter delivery to finish"));
        return false;
    }
    // Dirty bounds consume the complete history delta, including removals.
    // Adapter delivery already consumed only the just-sealed segment above.
    QRect dirtyExtent;
    if (!d->store->transactionDirtyExtent(transaction, d->surface, &dirtyExtent)) {
        KisPageStoreDetail::setError(error, QStringLiteral("history dirty bounds are unavailable"));
        return false;
    }
    locker.relock();
    const KisPageStoreMemento committed =
        d->history->commit(d->activeHistory, error);
    if (!committed.isValid()) return false;
    // Preserve the original transaction's undo/redo projection invalidation.
    // The canonical delta includes removed pages; current extent alone would
    // lose the dirty region when an undo removes the last painted pixels.
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
    const KisMementoSP &memento, bool before, QString *error,
    const std::function<bool(const KisImageEpochRootSnapshot &, const KisPageRestorationChanges &)> &prepareAdapter,
    const std::function<void(const KisPageRestorationChanges &)> &completeAdapter)
{
    if (!memento) return false;
    QMutexLocker transactionLocker(&d->transactionMutex);
    QWriteLocker publicationLocker(&d->publicationLock);
    QMutexLocker locker(&d->mutex);
    const auto entry = d->mementos.find(memento.data());
    const KisPageStoreMemento stored = entry != d->mementos.end() ? entry->second : KisPageStoreMemento{};
    if (!stored.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore memento is unknown"));
        return false;
    }
    // The transaction gate preserves this memento capability. A Tile reader
    // can hold its swap barrier while entering backend selection; neither
    // this mutex nor publication write may enclose adapter barrier acquisition.
    locker.unlock();
    return d->history->restore(stored, before, error, prepareAdapter,
        [&](const KisPageRestorationChanges &changes) {
            publicationLocker.unlock(); // canonical acceptance already succeeded
            if (completeAdapter) completeAdapter(changes);
        }).isValid();
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
    QString *error, QVector<KisLogicalPageId> *changed,
    OperationDelivery *delivery,
    const std::function<bool(bool, const KisPageMutationSession::AdapterChanges &, QString *)> &prepareAdapter,
    const AdapterCompletion &completeAdapter,
    QVarLengthArray<quint8, 64> *changedPresence) try
{
    if (changed) changed->clear();
    if (changedPresence) changedPresence->clear();
    if (changedPresence && !changed) {
        KisPageStoreDetail::setError(error, QStringLiteral("fill presence requires changed-page output"));
        return false;
    }
    if (delivery && !delivery->isEmpty()) {
        KisPageStoreDetail::setError(error, QStringLiteral("adapter delivery output is already occupied"));
        return false;
    }
    if (!isOperational() || rect.isEmpty()) return false;
    prepareChangedPageOutput(changed, rect);
    bool success = false;
    auto discardOutput = qScopeGuard([&] {
        if (!success) {
            if (changed) changed->clear();
            if (changedPresence) changedPresence->clear();
        }
    });
    const qint32 firstColumn = pageCoordinate(rect.left(), KisTileData::WIDTH);
    const qint32 lastColumn = pageCoordinate(rect.right(), KisTileData::WIDTH);
    const qint32 firstRow = pageCoordinate(rect.top(), KisTileData::HEIGHT);
    const qint32 lastRow = pageCoordinate(rect.bottom(), KisTileData::HEIGHT);
    KisPageSnapshotArray<KisLogicalPageId> targets;
    for (qint64 row = firstRow; row <= lastRow; ++row)
        for (qint64 column = firstColumn; column <= lastColumn; ++column)
            targets.append({qint32(column), qint32(row)});
    const auto recordChange = [&](const KisLogicalPageId &page, bool present) {
        if (!changed) return;
        if (changedPresence) (*changedPresence)[changed->size()] = quint8(present);
        changed->append(page);
    };
    // Projection clears can overlap a managed write at page granularity.
    // Acquire all claims before any page work, where no adapter gate is held;
    // the original batch then owns both semantic and writable page claims.
    auto range = store()->reserveManagedRange(d->surface, targets, false, nullptr, error, false);
    if (!range.d) return false;
    OperationFrame frame(*this);
    if (!frame.begin(error, &range)) return false;
    auto before = captureReadView(false, error);
    KisSurfaceEpochState surfaceState;
    if (!before.resolveSurfaceState(d->surface, &surfaceState) ||
        pixel.size() != qsizetype(surfaceState.format.pixelStride)) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 fill surface is invalid"));
        return false;
    }
    const bool clearsToDefault = pixel == surfaceState.format.defaultPixel;
    if (prepareAdapter && !prepareAdapter(clearsToDefault, frame.session().admittedAdapterPages(), error)) return false;
    if (changedPresence && !preparePagePresence(targets.size(), changedPresence, error)) return false;
    std::vector<char, KisMutationStorageAllocator<char>> rowBytes(size_t(KisTileData::WIDTH * pixel.size()));
    for (int x = 0; x < KisTileData::WIDTH; ++x)
        memcpy(rowBytes.data() + x * pixel.size(), pixel.constData(), size_t(pixel.size()));
    std::shared_ptr<const KisPageReplicaSource> uniform;
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
            if (affected == pageRect || clearsToDefault) {
                KisPageVersion old; if (!before.resolvePageVersion(key, &old)) return false;
                // A partial clear of a sparse default page is also a no-op.
                // The fixed before view is protected by this batch's range claim.
                if (clearsToDefault && old.isDefaultPixel()) continue;
            }
            if (affected == pageRect) {
                if (clearsToDefault) {
                    if (!frame.session().removePage(key, error)) return false;
                } else {
                    if (!uniform) uniform = uniformSourceFor(surfaceState.allocationDescriptor(), pixel, error);
                    if (!uniform) return false;
                    auto read = before.readResidentPage(key);
                    if (d->provider->sourceMatchesReadGuard(uniform, read)) continue;
                    if (!frame.session().reserveLegacyMutationPage(key, error) ||
                        !frame.session().aliasPage(key, uniform, error)) return false;
                }
                recordChange(key.page, !clearsToDefault);
                continue;
            }
            const int x = affected.left() - pageRect.left(), y = affected.top() - pageRect.top();
            auto execution = frame.session().borrowExecution(d->surface, {key.page}, error);
            if (!execution.isActive() || !execution.writeFinalRect(key,
                QRect(x, y, affected.width(), affected.height()),
                {rowBytes.data(), 0, qsizetype(rowBytes.size())}, error)) return false;
            const auto count = execution.finishPreparedWrites(nullptr, error);
            if (count < 0 || !execution.finish(error)) return false;
            if (count) recordChange(key.page, true);
        }
    }
    before = {};
    if (changedPresence) changedPresence->resize(changed->size());
    success = frame.finish(delivery, completeAdapter, error);
    return success;
}
catch (const std::bad_alloc &) {
    if (changed) changed->clear();
    if (changedPresence) changedPresence->clear();
    KisPageStoreDetail::setError(error, QStringLiteral("fill storage preparation was refused"));
    return false;
}

template<typename Operation>
KisPageStoreWriteOperationResult KisTiledDataManagerPageStoreBackend::runCpuMutationOperation(
    const KisPageSnapshotArray<KisLogicalPageId> &targets, bool legacyIntent, bool prepareWrites,
    Operation &&operation,
    QVector<KisLogicalPageId> *changed, QString *error, const KisMementoSP &historyOwner,
    OperationDelivery *delivery,
    const KisPageMutationSession::AdapterPreparation &prepareAdapter,
    const AdapterCompletion &completeAdapter)
{
    using Result = KisPageStoreWriteOperationResult;
    if (changed) changed->clear();
    bool succeeded = false;
    const auto discardOutput = qScopeGuard([&] { if (changed && !succeeded) changed->clear(); });
    if (delivery && !delivery->isEmpty()) {
        KisPageStoreDetail::setError(error, QStringLiteral("adapter delivery output is already occupied"));
        return Result::Failed;
    }
    if (!isOperational()) return Result::Unavailable;
    if (targets.isEmpty() && !historyOwner) return Result::Succeeded;
    using Phase = KisPageStoreDiagnosticPhase;
    KisPageStoreDiagnosticTimer phase(store(), Phase::PixelOperationPreflight, quint64(targets.size()));
    boost::intrusive_ptr<KisTiledDataManagerPageStoreWriteBatch::Private> existing;
    {
        QMutexLocker lock(&d->mutex);
        if (auto *operation = d->currentOperation(); operation && operation->kind != OperationLink::Kind::Managed) {
            existing = KisTiledDataManagerPageStoreWriteBatch::Private::retain(operation);
            if (existing && existing->kind == KisTiledDataManagerPageStoreWriteBatch::Private::Kind::Iterator && !existing->failed
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
    OperationFrame frame(*this);
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
                                                  &storeBorrowed, error, true);
        if (!range.d) return storeBorrowed ? Result::Borrowed : Result::Failed;
        phase.next(Phase::PixelOperationRangeReserved, quint64(targets.size()));
        if (!frame.begin(error, &range, &execution)) return Result::Failed;
        transaction = frame.session().transaction();
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
            prepared = prepareAdapter(KisPageMutationSession::executionAdapterChanges(execution, false), error);
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
    if (historyOwner && completeAdapter && !completeAdapter([](QString *) { return true; },
            KisPageMutationSession::executionAdapterChanges(execution), error))
        return Result::Failed;
    if (!execution.finish(error)) return Result::Failed;
    if (frame.isPresent() && !frame.finish(delivery, completeAdapter, error)) return Result::Failed;
    phase.next(Phase::PixelOperationChangedDeliver, quint64(touched));
    phase.next(Phase::PixelOperationCleanup, quint64(touched));
    succeeded = true;
    return Result::Succeeded;
}

KisPageStoreWriteOperationResult KisTiledDataManagerPageStoreBackend::writeOperation(
    const QVector<KisLogicalPageId> &pages, bool legacyIntent, const KisPageStorePixelOperation &operation,
    QVector<KisLogicalPageId> *changed, QString *error, const KisMementoSP &historyOwner,
    OperationDelivery *delivery,
    const KisPageMutationSession::AdapterPreparation &prepareAdapter,
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
        BoundedCursor(KisPageMutationExecution &mutation, KisSurfaceId surface, quint32 pixelSize)
            : mutation(mutation), surface(surface), pixelSize(pixelSize) {}
        void moveTo(qint32 x, qint32 y) override {
            if (failed) return;
            position = {x,y}; data = nullptr;
            const KisLogicalPageId next{pageCoordinate(x, KisTileData::WIDTH), pageCoordinate(y, KisTileData::HEIGHT)};
            if (!guard.isValid() || !(next == current)) {
                guard = {};
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
        KisLogicalPageId current;
        QPoint position;
        KisCpuWriteGuard guard;
        quint8 *data = nullptr;
        bool failed = false;
        QString failure;
    } cursor(mutation, surface(), state.format.pixelStride);
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
    const KisPageMutationSession::AdapterPreparation &prepareAdapter,
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
        Q_UNUSED(transaction);
        KisPageStoreDiagnosticTimer phase(store(), Phase::PackedWritePrepare, 1);
        for (const auto &page : targets) {
            const KisPageKey key{surface(), page};
            const QRect pageRect(page.column * KisTileData::WIDTH, page.row * KisTileData::HEIGHT,
                                 KisTileData::WIDTH, KisTileData::HEIGHT);
            const QRect affected = rect.intersected(pageRect);
            const qsizetype localX = qint64(affected.x()) - pageRect.x();
            const qsizetype localY = qint64(affected.y()) - pageRect.y();
            const quint8 *input = data + (qint64(affected.y()) - y) * sourceStride +
                                        (qint64(affected.x()) - x) * pixelStride;
            const size_t lineBytes = size_t(affected.width() * pixelStride);
            phase.next(Phase::PackedWritePrepare, 1);
            const KisCpuPagePayload payload{input, qsizetype(sourceStride),
                qsizetype((affected.height() - 1) * sourceStride + lineBytes)};
            if (!mutation.writeFinalRect(key, QRect(localX, localY, affected.width(), affected.height()), payload, error)) return false;
        }
        phase.next(Phase::PackedWriteRelease, 1);
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
    const KisCapturedReadView &sourceView, const QRect &requestedRect,
    const QRect &rect, bool rough, QString *error, QVector<KisLogicalPageId> *changed,
    OperationDelivery *delivery,
    const std::function<bool(bool, const KisPageMutationSession::AdapterChanges &, QString *)> &prepareAdapter,
    const AdapterCompletion &completeAdapter,
    QVarLengthArray<quint8, 64> *changedPresence) try
{
    if (changed) changed->clear();
    if (changedPresence) changedPresence->clear();
    if (changedPresence && !changed) {
        KisPageStoreDetail::setError(error, QStringLiteral("copy presence requires changed-page output"));
        return false;
    }
    if (delivery && !delivery->isEmpty()) {
        KisPageStoreDetail::setError(error, QStringLiteral("adapter delivery output is already occupied"));
        return false;
    }
    if (!isOperational() || !source.isOperational() || rect.isEmpty()) return false;
    prepareChangedPageOutput(changed, rect);
    bool success = false;
    auto discardOutput = qScopeGuard([&] {
        if (!success) {
            if (changed) changed->clear();
            if (changedPresence) changedPresence->clear();
        }
    });
    KisPageStoreDiagnosticTimer phase(store(), KisPageStoreDiagnosticPhase::CopyPrepare, 1);
    // The immutable source selects both geometry and all copied pixels.
    // Destination geometry alone cannot authorize a post-wait no-op.
    KisSurfaceEpochState sourceState, targetState;
    if (!sourceView.resolveSurfaceState(source.surface(), &sourceState)) return false;
    KisPageSnapshotArray<KisLogicalPageId> targets;
    for (qint64 row = pageCoordinate(rect.top(), KisTileData::HEIGHT);
         row <= pageCoordinate(rect.bottom(), KisTileData::HEIGHT); ++row)
        for (qint64 column = pageCoordinate(rect.left(), KisTileData::WIDTH);
             column <= pageCoordinate(rect.right(), KisTileData::WIDTH); ++column)
            targets.append({qint32(column), qint32(row)});
    auto range = store()->reserveManagedRange(d->surface, targets, false, nullptr, error, false);
    if (!range.d) return false;
    auto targetView = captureReadView(false, error);
    if (!targetView.resolveSurfaceState(surface(), &targetState) ||
        sourceState.format.pixelStride != targetState.format.pixelStride) return false;
    const QRect selected = sourceState.format.defaultPixel == targetState.format.defaultPixel
        ? requestedRect.intersected(sourceState.contentExtent.united(targetState.contentExtent)) : requestedRect;
    if (selected != rect) {
        // A changed clip must not silently omit newly required pages.
        // Reject before pixels; do not replay or grow an already admitted range.
        KisPageStoreDetail::setError(error, QStringLiteral("copy target bounds changed before admission"));
        return false;
    }
    OperationFrame frame(*this);
    if (!frame.begin(error, &range)) return false;
    if (prepareAdapter && !prepareAdapter(sourceState.format.defaultPixel == targetState.format.defaultPixel, frame.session().admittedAdapterPages(), error)) return false;
    if (changedPresence && !preparePagePresence(targets.size(), changedPresence, error)) return false;
    const auto recordChange = [&](const KisLogicalPageId &page, bool present) {
        if (!changed) return;
        if (changedPresence) (*changedPresence)[changed->size()] = quint8(present);
        changed->append(page);
    };
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
                    if (!frame.session().removePage(to, error)) return false;
                } else {
                    // Use the frozen default value directly; building a second
                    // shared default read buffer would duplicate initialization.
                    auto backing = uniformSourceFor(targetState.allocationDescriptor(), sourceState.format.defaultPixel.view(), error);
                    auto previous = targetView.readResidentPage(to);
                    if (backing && d->provider->sourceMatchesReadGuard(backing, previous)) continue;
                    if (!backing || !frame.session().reserveLegacyMutationPage(to, error) ||
                        !frame.session().aliasPage(to, backing, error)) return false;
                }
                recordChange(to.page, sourceState.format.defaultPixel != targetState.format.defaultPixel);
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
                if (!backing || !frame.session().reserveLegacyMutationPage(to, error) ||
                    !frame.session().aliasPage(to, backing, error)) return false;
                recordChange(to.page, true);
                continue;
            }
            const qsizetype offset = (qsizetype(affected.top() - pageRect.top()) * KisTileData::WIDTH +
                                       affected.left() - pageRect.left()) * pixelSize;
            auto execution = frame.session().borrowExecution(surface(), {to.page}, error);
            if (!execution.isActive() || !execution.writeFinalRect(to,
                affected.translated(-pageRect.topLeft()),
                {input.data() + offset, rowStride,
                 (affected.height()-1) * rowStride + affected.width() * pixelSize}, error)) return false;
            const auto count = execution.finishPreparedWrites(nullptr, error);
            if (count < 0 || !execution.finish(error)) return false;
            if (count) recordChange(to.page, true);
        }
    }
    phase.next(KisPageStoreDiagnosticPhase::CopyFinish, 1);
    targetView = {};
    if (changedPresence) changedPresence->resize(changed->size());
    success = frame.finish(delivery, completeAdapter, error);
    return success;
}
catch (const std::bad_alloc &) {
    if (changed) changed->clear();
    if (changedPresence) changedPresence->clear();
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
    const auto transaction = writableTransaction(&owned, error);
    KisPageStore *pageStore = store();
    if (!transaction.isValid() || !pageStore) return false;
    auto release = qScopeGuard([&] {
        // This private surface-only transaction exposes no page proof,
        // session, write lease or prepared reader. Abort has no physical
        // cancellation tail, unlike a real pixel mutation.
        if (owned && !pageStore->abort(transaction))
            qFatal("Surface-only owned transaction could not be released");
    });

    const auto overlay = KisPageReadView::transactionOverlay(transaction.id);
    KisSurfaceEpochState state;
    if (!pageStore->resolveSurfaceState(d->surface, overlay, &state) ||
        pixel.size() != qsizetype(state.format.pixelStride)) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore default pixel is invalid"));
        return false;
    }
    if (state.format.defaultPixel != pixel &&
        !pageStore->stageSurfaceDefaultPixel(transaction, d->surface, pixel, error)) {
        return false;
    }
    if (owned) {
        QMutexLocker transactionLocker(&d->transactionMutex);
        if (!pageStore->commitPreparedTransaction(transaction)) {
            KisPageStoreDetail::setError(error, QStringLiteral("tiles3 PageStore default commit failed"));
            return false;
        }
    }
    release.dismiss();
    return true;
}

bool KisTiledDataManagerPageStoreBackend::clearAll(
    QString *error, OperationDelivery *delivery,
    const std::function<bool(QString *)> &prepareAdapter,
    const AdapterCompletion &completeAdapter) try
{
    if (!isOperational() || (delivery && !delivery->isEmpty())) {
        KisPageStoreDetail::setError(error, QStringLiteral("clear is unavailable"));
        return false;
    }
    // A legacy tile caller may retain a raw writable pointer across clear().
    // clearAll is the sequencing barrier that cancels its anonymous
    // mutation before creating the post-clear transaction.
    if (!cancelAnonymousLeasesForBarrier(error)) return false;
    return removePages(allocatedPages(), error, delivery, prepareAdapter, completeAdapter);
}
catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("clear storage preparation was refused"));
    return false;
}

bool KisTiledDataManagerPageStoreBackend::removePages(
    const QVector<KisLogicalPageId> &pages,
    QString *error, OperationDelivery *delivery,
    const std::function<bool(QString *)> &prepareAdapter,
    const AdapterCompletion &completeAdapter) try
{
    if (!isOperational() || (delivery && !delivery->isEmpty())) {
        KisPageStoreDetail::setError(error, QStringLiteral("page removal is unavailable"));
        return false;
    }
    if (pages.isEmpty()) {
        // No canonical work needs a new transaction. A whole-index clear can
        // still have compatibility remnants left by an independent producer.
        if (prepareAdapter && !prepareAdapter(error)) return false;
        const KisPageMutationSession::AdapterChanges changes(nullptr,
            [](const void *, const void *, KisPageMutationSession::AdapterChanges::Visitor) { return true; });
        if (completeAdapter && !completeAdapter([](QString *) { return true; }, changes, error)) return false;
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    OperationFrame frame(*this);
    if (!frame.begin(error)) return false;
    for (const KisLogicalPageId &page : pages) {
        if (!frame.session().reserveLegacyMutationPage({surface(), page}, error)
            || !frame.session().removePage({surface(), page}, error))
            return false;
    }
    if (prepareAdapter && !prepareAdapter(error)) return false;
    return frame.finish(delivery, completeAdapter, error);
}
catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("removal storage preparation was refused"));
    return false;
}

bool KisTiledDataManagerPageStoreBackend::trimToRect(
    const QRect &rect,
    QString *error)
{
    return trimToRect(rect, error, nullptr, {}, {});
}

bool KisTiledDataManagerPageStoreBackend::trimToRect(
    const QRect &rect, QString *error, OperationDelivery *delivery,
    const std::function<bool(const QVector<KisLogicalPageId> &,
        const KisPageMutationSession::AdapterChanges &, QString *)> &prepareAdapter,
    const KisPageMutationSession::AdapterInstallation &completeAdapter) try
{
    if (delivery && !delivery->isEmpty()) {
        KisPageStoreDetail::setError(error, QStringLiteral("adapter delivery output is already occupied"));
        return false;
    }
    // Trim still reconciles the whole index, including adapter-only remnants.
    // Completed native delivery owns its original claims even after leaving
    // the thread/client registry. Original Frame admission rejects nesting.
    if (d->store->sessionStats().activeCpuWritePages != 0) {
        KisPageStoreDetail::setError(error, QStringLiteral("trim requires adapter delivery to finish"));
        return false;
    }
    OperationFrame frame(*this);
    KisPageStore *pageStore = store();
    if (!pageStore || !frame.begin(error)) return false;

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
        if (!rect.contains(extent) &&
            !frame.session().reserveLegacyMutationPage({surface(), page}, error)) return false;
    }
    const auto transaction = frame.session().transaction();
    const auto overlay = KisPageReadView::transactionOverlay(transaction.id);
    KisSurfaceEpochState state;
    if (!pageStore->resolveSurfaceState(d->surface, overlay, &state) ||
        state.logicalPageExtent != QSize(KisTileData::WIDTH, KisTileData::HEIGHT)) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 trim surface is unavailable or incompatible"));
        return false;
    }
    // The original admitted rows are the complete change set. No second key
    // or geometry array, and no single-page Execution owner, survives a band.
    // Claims precede all adapter preparation and the first pixel write.
    const auto admitted = frame.session().admittedAdapterPages();
    if (prepareAdapter && !prepareAdapter(pages, admitted, error)) return false;
    const auto &pixel = state.format.defaultPixel;
    const qsizetype stride = qsizetype(KisTileData::WIDTH) * pixel.size();
    if (!admitted.forEach([&](const KisPageKey &admittedKey, bool) {
        const auto key = admittedKey;
        const QRect extent(key.page.column * KisTileData::WIDTH, key.page.row * KisTileData::HEIGHT,
                           KisTileData::WIDTH, KisTileData::HEIGHT);
        const auto kept = extent.intersected(rect);
        if (kept.isEmpty()) return frame.session().removePage(key, error);
        auto guard = frame.session().beginWrite(key, error);
        if (!guard.isValid()) {
            KisPageStoreDetail::setError(error, QStringLiteral("tiles3 trim writable page acquisition failed"));
            return false;
        }
        auto *data = static_cast<quint8 *>(guard.data());
        if (!data || guard.rowStride() != stride) {
            KisPageStoreDetail::setError(error, QStringLiteral("tiles3 trim writable page layout is invalid"));
            return false;
        }
        const int left = kept.left() - extent.left();
        const int top = kept.top() - extent.top();
        const int right = left + kept.width();
        const int bottom = top + kept.height();
        const auto fill = [&](int row, int column, int count) {
            quint8 *target = data + row * stride + column * pixel.size();
            if (pixel.size() == 1) memset(target, quint8(pixel[0]), size_t(count));
            else for (int i = 0; i < count; ++i)
                memcpy(target + i * pixel.size(), pixel.constData(), size_t(pixel.size()));
        };
        // All bands of one partial page share a single first-write backing.
        // One original native guard protects all bands, with no generic lease
        // record, tile wrapper or second target lookup.
        for (int row = 0; row < KisTileData::HEIGHT; ++row) {
            if (row < top || row >= bottom) fill(row, 0, KisTileData::WIDTH);
            else {
                fill(row, 0, left);
                fill(row, right, KisTileData::WIDTH - right);
            }
        }
        return true; // This guard returns before the next original row.
    })) return false;
    return frame.finish(delivery,
        [&](const auto &publish, const auto &changes, QString *failure) {
            if (!publish(failure)) return false;
            if (completeAdapter) completeAdapter(changes);
            return true;
        }, error);
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
        if (d->activeHistory.isValid()) overlayTransaction = d->activeHistory.transaction;
    }

    return pageStore->allocatedPages(d->surface, overlayTransaction);
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

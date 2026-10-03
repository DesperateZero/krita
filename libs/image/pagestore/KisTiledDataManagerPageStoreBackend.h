/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_TILED_DATA_MANAGER_PAGE_STORE_BACKEND_H
#define KIS_TILED_DATA_MANAGER_PAGE_STORE_BACKEND_H

#include <memory>
#include <QVarLengthArray>
#include <QHash>
#include <QMutex>
#include <QScopedPointer>
#include <QSet>
#include <QVector>
#include <utility>

#include "KisPageStoreMementoManager.h"
#include "KisPageStoreWriteOperation_p.h"
#include "tiles3/KisTilePageStoreBridge.h"
#include "tiles3/kis_tile_data.h"
#include "tiles3/kis_memento.h"

class KisTiles3PageReplicaProvider;
struct KisTiles3PayloadWork;
class KisTiledDataManagerPageStoreBackend;
class KisTiledDataManagerPageStoreLease;
class KisTiledDataManager;
class KisPageStoreIteratorReadScope;
class KisPageStoreReadPage;
class KisTile;
class KisPageStoreWriteReservation;
class KisTiledDataManagerIteratorWriteScope;

/**
 * Opaque RAII anchor for one legacy multi-tile write operation. Keeping the
 * token alive coalesces all anonymous tile leases into one PageStore commit.
 */
class KRITAIMAGE_EXPORT KisTiledDataManagerPageStoreWriteBatch : public KisPageProcessStorageObject
{
public:
    ~KisTiledDataManagerPageStoreWriteBatch();

    KisTiledDataManagerPageStoreWriteBatch(
        const KisTiledDataManagerPageStoreWriteBatch &) = delete;
    KisTiledDataManagerPageStoreWriteBatch &operator=(
        const KisTiledDataManagerPageStoreWriteBatch &) = delete;

    // A failed replacement poisons a private mutation batch. tileData must
    // be a completed/locked immutable legacy input; source COW is retained.
    bool replaceFullTile(qint32 column,
                         qint32 row,
                         KisTileData *tileData,
                         bool sparseDefault,
                         QString *error = nullptr);
    bool finish(QString *error = nullptr);
    // Cancel this operation. If borrowed guards are still live, destruction of
    // their final shared control block completes the cancellation.
    bool cancel();

private:
    class Private;
    friend class KisTiledDataManagerPageStoreBackend;
    friend class KisTiledDataManagerPageStoreLease;
    friend class KisTiledDataManagerIteratorWriteScope;
    KisTiledDataManagerPageStoreWriteBatch(
        KisTiledDataManagerPageStoreBackend *backend,
        const KisPageTransaction &transaction,
        bool owned);
    explicit KisTiledDataManagerPageStoreWriteBatch(
        std::shared_ptr<Private> shared);
    // Backend's managed operation only: retain the sealed session's original
    // admission until this already-existing batch leaves the adapter caller.
    bool finishForAdapterDelivery(QString *error);

    std::shared_ptr<Private> d;
    bool m_clientFinished = false;
};

/**
 * Production compatibility owner for moving KisTiledDataManager onto
 * PageStore without changing the public iterator/tile ABI in one step.
 */
class KRITAIMAGE_EXPORT KisTiledDataManagerPageStoreBackend final
    : public KisTilePageStoreBridge, public KisPageProcessStorageObject
{
public:
    KisTiledDataManagerPageStoreBackend();
    ~KisTiledDataManagerPageStoreBackend() override;

    // A completed standalone operation's original batch. It carries no write
    // access after completion; retaining it only retains the original page
    // admission until operation-local adapter storage has been destroyed.
    class KRITAIMAGE_EXPORT OperationDelivery
    {
    public:
        OperationDelivery() = default;
        OperationDelivery(OperationDelivery &&other) noexcept
            : m_batch(std::exchange(other.m_batch, {})) {}
        OperationDelivery &operator=(OperationDelivery &&other) noexcept
        {
            if (this != &other) m_batch = std::exchange(other.m_batch, {});
            return *this;
        }
        bool isEmpty() const { return !m_batch; }

    private:
        friend class KisTiledDataManagerPageStoreBackend;
        std::unique_ptr<KisTiledDataManagerPageStoreWriteBatch> m_batch;
    };

    // The caller's existing adapter gate surrounds only the original publish
    // closure and prepared compatibility installation, never the pixel body.
    using AdapterCompletion = std::function<bool(
        const std::function<bool(QString *)> &publish, QString *error)>;

    bool configure(quint32 pixelSize,
                   const quint8 *defaultPixel,
                   QString *error = nullptr);
    bool configureClone(
        const KisTiledDataManagerPageStoreBackend &source,
        QString *error = nullptr);
    bool isOperational() const;
    KisPageStore *store() const;
    KisTiles3PayloadWork payloadWork() const;
    KisSurfaceId surface() const;

    // Thread-confined private operation delta. Pixel capability is selected
    // lazily by the canonical mutation session; semantic-only batches need no
    // provider. Release tile leases before finish.
    std::unique_ptr<KisTiledDataManagerPageStoreWriteBatch> beginMutationBatch(
        QString *error = nullptr);
    // Writable iterators on one thread share one native mutation. Each call
    // returns a client scope; the final client publishes after all tile guards
    // have ended.
    std::unique_ptr<KisTiledDataManagerIteratorWriteScope> beginIteratorMutationScope(
        QString *error = nullptr);

    TileLease acquireTile(
        qint32 column, qint32 row, bool writable, bool oldData,
        TileLease *readCache = nullptr) override;

    // Freeze the selected current/oldData visibility boundary under the
    // publication gate, then release that gate for the whole pixel traversal.
    KisCapturedReadView captureReadView(bool oldData = false, QString *error = nullptr) const;
    std::shared_ptr<const KisPageStoreIteratorReadScope> captureIteratorReadScope(
        bool writable, QString *error = nullptr,
        std::shared_ptr<const KisPageStoreIteratorReadScope> existing = {}) const;
    bool hasCurrentThreadIteratorWrites() const;
    // Synchronous operation-private cursor, never backed by compatibility
    // tile wrappers. Reserve the entire target set before executing callback;
    // borrowed targets select compatibility before any pixel work. The callback
    // must not retain pointers/accessor, leave this range or reenter target writes.
    // Optional adapter preparation consumes the original admission before the
    // pixel callback. Completion receives the original publish closure and the
    // exact changed output, and may surround only publish + prepared adapter
    // installation with the caller's existing gate. A standalone delivery then
    // retains that completed batch's admission; history returns its borrow here
    // and produces no delivery value. False/bad_alloc before pixels returns an
    // untouched borrow normally; neither hook may retain writable pointers.
    KisPageStoreWriteOperationResult writeOperation(
        const QVector<KisLogicalPageId> &pages, bool legacyIntent, const KisPageStorePixelOperation &operation,
        QVector<KisLogicalPageId> *changed, QString *error = nullptr,
        const KisMementoSP &historyOwner = {},
        OperationDelivery *delivery = nullptr,
        const std::function<bool(QString *)> &prepareAdapter = {},
        const AdapterCompletion &completeAdapter = {});
    // Production packed read: one fixed view, at most one physical page pin
    // (or real control-path lease) at a time; no compatibility tile wrappers.
    bool readBytes(quint8 *data, qint32 x, qint32 y, qint32 width, qint32 height,
                   qint32 dataRowStride, QString *error = nullptr) const;
    // Fixed-view no-op comparison and private native writes. All targets are
    // reserved before capture; full-page overwrite needs no source copy.
    // Borrowed/Unavailable means no work was started and permits legacy fallback;
    // Failed must never replay. This is cancellable COW, not an in-place permit.
    KisPageStoreWriteOperationResult writeBytes(
        const quint8 *data, qint32 x, qint32 y, qint32 width, qint32 height,
        qint32 dataRowStride, bool legacyIntent, QVector<KisLogicalPageId> *changed,
        QString *error = nullptr,
        OperationDelivery *delivery = nullptr,
        const std::function<bool(QString *)> &prepareAdapter = {},
        const AdapterCompletion &completeAdapter = {});

    KisMementoSP beginHistory(const quint8 *defaultPixel,
                              quint32 pixelSize,
                              QString *error = nullptr);
    // Explicit opt-in by the existing history owner. Its original memento is
    // the capability; no new transaction, TLS writer or pending-page registry.
    // Caller initializes at a quiescent boundary before dispatching writers.
    // Already admitted legacy operations are not converted by this method.
    // Unscoped writes are rejected until this history finishes/aborts. Current
    // external reads observe the last checkpoint, never mutable pending data.
    bool beginHistoryMutation(const KisMementoSP &owner, QString *error = nullptr);
    KisCapturedReadView checkpointHistoryMutation(const KisMementoSP &owner,
        QVector<KisLogicalPageId> *changed, QString *error = nullptr);
    bool commitHistory(const quint8 *defaultPixel,
                       quint32 pixelSize,
                       QString *error = nullptr,
                       QVector<KisLogicalPageId> *changed = nullptr);
    bool abortHistory(QString *error = nullptr);
    bool abortHistory(const KisMementoSP &memento, QString *error = nullptr);
    bool rollback(const KisMementoSP &memento, QString *error = nullptr);
    bool rollforward(const KisMementoSP &memento, QString *error = nullptr);
    bool purgeHistory(const KisMementoSP &memento,
                      const quint8 *defaultPixel,
                      quint32 pixelSize,
                      QString *error = nullptr);
    bool hasCurrentHistory() const;

    // Operation-private semantic/pixel batch. Reserve the complete rectangle
    // before capture or pixels, outside caller tile/manager gates. The same
    // adapter preparation/completion and retained admission as packed writes
    // surround publication and index installation. Optional changed keys are
    // empty on failure and contain only successful deltas.
    bool fillRect(const QRect &rect,
                  const QByteArray &pixel,
                  QString *error = nullptr, QVector<KisLogicalPageId> *changed = nullptr,
                  OperationDelivery *delivery = nullptr,
                  const std::function<bool(QString *)> &prepareAdapter = {},
                  const AdapterCompletion &completeAdapter = {});
    // Geometry and pixels consume the same caller-selected immutable views.
    // Neither view is recaptured after compatibility range preparation.
    bool copyFrom(const KisTiledDataManagerPageStoreBackend &source,
                  const KisCapturedReadView &sourceView, const KisCapturedReadView &targetView,
                  const QRect &rect, bool rough, QString *error = nullptr,
                  QVector<KisLogicalPageId> *changed = nullptr);
    // Borrowed backing: the caller must retain the exact read page throughout
    // cache installation. No implicit head lookup or generic request.
    KisTileData *tileDataForReadPage(const KisPageStoreReadPage &read) const;
    TileLease readCacheForPage(const KisPageStoreReadPage &read, TileLease reuse = {}) const;
    bool setDefaultPixel(const QByteArray &pixel,
                         QString *error = nullptr);
    bool clearAll(QString *error = nullptr);
    bool removePages(const QVector<KisLogicalPageId> &pages,
                     QString *error = nullptr);
    bool trimToRect(const QRect &rect, QString *error = nullptr);
    QVector<KisLogicalPageId> allocatedPages() const;
    bool pageAllocated(qint32 column, qint32 row, bool oldData) const;

private:
    friend class KisTiledDataManagerPageStoreLease;
    friend class KisTiledDataManagerPageStoreWriteBatch;
    friend class KisTiledDataManager;
    std::shared_ptr<std::pmr::memory_resource> extentStorage() const;

    // A new manager read selects current; a retained TileSP keeps its existing
    // nested/fixed read lifetime. A null result never authorizes stale bytes.
    KisSharedPtr<KisTile> selectCurrentReadTile(KisTiledDataManager &manager,
                                              KisSharedPtr<KisTile> candidate, bool *present);

    bool preparePagePresence(qsizetype count, QVarLengthArray<quint8, 64> *scratch, QString *error) const;
    // Operation-private output storage; valid only after whole-query success.
    // Caller prepares capacity before pixels; no allocation or retained view.
    bool resolveCurrentPagePresenceInto(const QVector<KisLogicalPageId> &pages,
                                        quint8 *scratch, qsizetype capacity, QString *error = nullptr) const;
    KisPageTransaction writableTransaction(bool *owned,
                                           QString *error = nullptr);
    bool finishOwnedTransaction(const KisPageTransaction &transaction,
                                QString *error = nullptr);
    bool abortOwnedTransaction(const KisPageTransaction &transaction);
    void registerAnonymousLease(KisTiledDataManagerPageStoreLease *lease);
    void unregisterAnonymousLease(KisTiledDataManagerPageStoreLease *lease);
    template<typename Operation>
    KisPageStoreWriteOperationResult runCpuMutationOperation(
        const KisPageSnapshotArray<KisLogicalPageId> &targets, bool legacyIntent, bool prepareWrites,
        Operation &&operation,
        QVector<KisLogicalPageId> *changed, QString *error,
        const KisMementoSP &historyOwner = {},
        OperationDelivery *delivery = nullptr,
        const std::function<bool(QString *)> &prepareAdapter = {},
        const AdapterCompletion &completeAdapter = {});
    KisPageSnapshotArray<KisLogicalPageId> historyChangedPages(const KisPageTransaction &transaction) const;
    std::shared_ptr<const KisPageReplicaSource> uniformSourceFor(
        const KisPageAllocationDescriptor &descriptor, const QByteArray &pixel, QString *error);
    bool cancelAnonymousLeasesForBarrier(QString *error = nullptr);
    bool pruneDefaultPreparedPages(const KisPageTransaction &transaction,
                                   QString *error = nullptr);
    // Cold compatibility-index preparation for cancellation, not a pixel lease.
    bool prepareHistoryAbort(const KisMementoSP &memento,
                             QVector<KisLogicalPageId> *pages, QString *error);
    bool restoreHistory(const KisMementoSP &memento, bool before, QString *error);
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_TILED_DATA_MANAGER_PAGE_STORE_BACKEND_H

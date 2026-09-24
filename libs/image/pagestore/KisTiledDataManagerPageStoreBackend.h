/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_TILED_DATA_MANAGER_PAGE_STORE_BACKEND_H
#define KIS_TILED_DATA_MANAGER_PAGE_STORE_BACKEND_H

#include <QHash>
#include <QMutex>
#include <QScopedPointer>
#include <QSet>
#include <QVector>

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
class KisPageStoreWriteReservation;
class KisTiledDataManagerIteratorWriteScope;

/**
 * Opaque RAII anchor for one legacy multi-tile write operation. Keeping the
 * token alive coalesces all anonymous tile leases into one PageStore commit.
 */
class KRITAIMAGE_EXPORT KisTiledDataManagerPageStoreWriteBatch
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
    KisTiledDataManagerPageStoreWriteBatch(
        KisTiledDataManagerPageStoreBackend *backend,
        const KisPageTransaction &transaction,
        bool owned);
    explicit KisTiledDataManagerPageStoreWriteBatch(
        QSharedPointer<Private> shared);

    QSharedPointer<Private> d;
    bool m_clientFinished = false;
};

/**
 * Production compatibility owner for moving KisTiledDataManager onto
 * PageStore without changing the public iterator/tile ABI in one step.
 */
class KRITAIMAGE_EXPORT KisTiledDataManagerPageStoreBackend final
    : public KisTilePageStoreBridge
{
public:
    KisTiledDataManagerPageStoreBackend();
    ~KisTiledDataManagerPageStoreBackend() override;

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

    std::unique_ptr<KisTilePageStoreLease> acquireTile(
        qint32 column, qint32 row, bool writable, bool oldData) override;

    // Freeze the selected current/oldData visibility boundary under the
    // publication gate, then release that gate for the whole pixel traversal.
    KisCapturedReadView captureReadView(bool oldData = false, QString *error = nullptr) const;
    QSharedPointer<const KisPageStoreIteratorReadScope> captureIteratorReadScope(
        bool writable, QString *error = nullptr,
        QSharedPointer<const KisPageStoreIteratorReadScope> existing = {}) const;
    bool hasCurrentThreadIteratorWrites() const;
    // Synchronous operation-private cursor, never backed by compatibility
    // tile wrappers. Reserve the entire target set before executing callback;
    // borrowed targets select compatibility before any pixel work. The callback
    // must not retain pointers/accessor, leave this range or reenter target writes.
    KisPageStoreWriteOperationResult writeOperation(
        const QVector<KisLogicalPageId> &pages, bool legacyIntent, const KisPageStorePixelOperation &operation,
        QVector<KisLogicalPageId> *changed, QString *error = nullptr);
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
        QString *error = nullptr);

    KisMementoSP beginHistory(const quint8 *defaultPixel,
                              quint32 pixelSize,
                              QString *error = nullptr);
    bool commitHistory(const quint8 *defaultPixel,
                       quint32 pixelSize,
                       QString *error = nullptr);
    bool abortHistory(QString *error = nullptr);
    bool rollback(const KisMementoSP &memento, QString *error = nullptr);
    bool rollforward(const KisMementoSP &memento, QString *error = nullptr);
    bool purgeHistory(const KisMementoSP &memento,
                      const quint8 *defaultPixel,
                      quint32 pixelSize,
                      QString *error = nullptr);
    bool hasCurrentHistory() const;

    // Operation-private semantic/pixel batch. Optional changed keys are empty
    // on failure and contain only successful deltas, for index/cache refresh;
    // they are not a capability to read outside a newly protected View.
    bool fillRect(const QRect &rect,
                  const QByteArray &pixel,
                  QString *error = nullptr, QVector<KisLogicalPageId> *changed = nullptr);
    bool copyFrom(const KisTiledDataManagerPageStoreBackend &source, const QRect &rect,
                  bool oldSource, bool rough, QString *error = nullptr, QVector<KisLogicalPageId> *changed = nullptr);
    // Borrowed backing: the caller must retain the exact read page throughout
    // cache installation. No implicit head lookup or generic request.
    KisTileData *tileDataForReadPage(const KisPageStoreReadPage &read) const;
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

    KisPageTransaction writableTransaction(bool *owned,
                                           QString *error = nullptr);
    bool finishOwnedTransaction(const KisPageTransaction &transaction,
                                QString *error = nullptr);
    bool abortOwnedTransaction(const KisPageTransaction &transaction);
    void registerAnonymousLease(KisTiledDataManagerPageStoreLease *lease);
    void unregisterAnonymousLease(KisTiledDataManagerPageStoreLease *lease);
    template<typename Operation>
    KisPageStoreWriteOperationResult runCpuMutationOperation(
        const QSet<KisLogicalPageId> &targets, bool legacyIntent, Operation &&operation,
        QVector<KisLogicalPageId> *changed, QString *error);
    QSharedPointer<const KisPageReplicaSource> uniformSourceFor(
        const KisPageAllocationDescriptor &descriptor, const QByteArray &pixel, QString *error);
    bool cancelAnonymousLeasesForBarrier(QString *error = nullptr);
    bool pruneDefaultPreparedPages(const KisPageTransaction &transaction,
                                   QString *error = nullptr);
    bool stageDerivedExtent(const KisPageTransaction &transaction,
                            QString *error = nullptr);
    bool restoreHistory(const KisMementoSP &memento, bool before, QString *error);
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_TILED_DATA_MANAGER_PAGE_STORE_BACKEND_H

/*
 *  SPDX-FileCopyrightText: 2004 C. Boemann <cbo@boemann.dk>
 *  SPDX-FileCopyrightText: 2009 Dmitry Kazakov <dimula73@gmail.com>
 *  SPDX-FileCopyrightText: 2010 Cyrille Berger <cberger@cberger.net>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "pagestore/experimental/KisNativeResultProtocol.h"
#include <memory>
#include <QRect>
#include <QRegion>
#include <QScopeGuard>
#include <QVector>
#include <QVarLengthArray>

#include <utility>
#include <algorithm>
#include <array>
#include <new>
#include <tuple>

#include "kis_tile.h"
#include "kis_tiled_data_manager.h"
#include "kis_tile_data_wrapper.h"
#include "kis_tiled_data_manager_p.h"
#include "kis_memento_manager.h"
#include "swap/kis_legacy_tile_compressor.h"
#include "swap/kis_tile_compressor_factory.h"

#include "kis_paint_device_writer.h"

#include "kis_global.h"
#include "KisStrokeJobFailureContext.h"
#include "pagestore/KisTiledDataManagerPageStoreBackend.h"
#include "pagestore/KisPageStoreDiagnostics_p.h"
#include "pagestore/KisImageEpochReferenceModel.h"
#include "pagestore/KisPageStoreIteratorReadScope_p.h"

namespace {

using PixelOperationResult = KisPageStoreWriteOperationResult;
using OperationDelivery = KisTiledDataManagerPageStoreBackend::OperationDelivery;

namespace {
std::unique_ptr<KisTiledDataManagerPageStoreBackend> preparePageStoreBackend(KisTiledDataManager *manager) noexcept
{
    try { return std::make_unique<KisTiledDataManagerPageStoreBackend>(manager); }
    catch (const std::bad_alloc &) { return {}; }
}
}

// Capacity preparation does not publish tiles or change visible bounds.
bool prepareExtent(KisTiledExtentManager &extent, const QRect &tileRange, QString *error)
{
    if (extent.prepareTileRange(tileRange)) return true;
    if (error) *error = QStringLiteral("Extent storage preparation was rejected");
    return false;
}

QRect completelyCoveredPages(const QRect &pixels, QRect pages)
{
    if (pixels.left() != qint64(pages.left()) * KisTileData::WIDTH) pages.adjust(1, 0, 0, 0);
    if (pixels.top() != qint64(pages.top()) * KisTileData::HEIGHT) pages.adjust(0, 1, 0, 0);
    if (qint64(pixels.right()) + 1 != (qint64(pages.right()) + 1) * KisTileData::WIDTH) pages.adjust(0, 0, -1, 0);
    if (qint64(pixels.bottom()) + 1 != (qint64(pages.bottom()) + 1) * KisTileData::HEIGHT) pages.adjust(0, 0, 0, -1);
    return pages.isEmpty() ? QRect{} : pages;
}

// Operation-local storage for missing wrappers and possible removals. Ordinary
// warm writes need no record. Semantic producers reserve removal GC actions
// here so successful publication cannot be followed by a late allocation.
// Original admission covers preparation through installation. Candidates are
// destroyed outside the manager/install gates. A checkpoint ends admission
// after installation; standalone deliveries may retain it through destruction.
class PreparedTileUpdates
{
public:
    bool prepare(KisTileHashTable &table,
                 const KisPageMutationSession::AdapterChanges &pages,
                 QString *error, const QRect &removable = {})
    {
        try {
            pages.forEachLocated([&](const KisPageKey &key, bool, void *&location) {
                const auto &page = key.page;
                location = preparePage(table, page, !removable.isEmpty() && removable.contains(page.column, page.row)
                    ? KisTileHashTable::Update::MayRemove : KisTileHashTable::Update::Present);
                return true;
            });
            sortPrepared();
            return true;
        } catch (const std::bad_alloc &) {
            if (error) *error = QStringLiteral("Compatibility tile storage preparation was rejected");
            return false;
        }
    }

    bool prepare(KisTileHashTable &table, KisTiledExtentManager &extent, KisSurfaceId surface,
                 const KisImageEpochRootSnapshot &root, const KisPageRestorationChanges &changes)
    {
        size_t next = 0;
        return prepareReconciliation(table, extent,
            [&](const auto &page) { return root.containsPage({surface, page}); },
            [&](const auto &visit) {
                return root.forEachPage(surface, [&](const KisPageVersion &version) {
                    const auto less = [](const KisPageKey &a, const KisPageKey &b) {
                        return std::tie(a.surface.value, a.page.row, a.page.column)
                            < std::tie(b.surface.value, b.page.row, b.page.column);
                    };
                    while (next < changes.size() && less(changes.version(next).key, version.key)) ++next;
                    void **location = next < changes.size() && changes.version(next).key == version.key
                        ? &changes.location(next++) : nullptr;
                    return visit(version.key.page, location);
                });
            });
    }

    bool prepare(KisTileHashTable &table, KisTiledExtentManager &extent,
                 const QVector<KisLogicalPageId> &pages, const QRect &kept, const QRect &keptPages,
                 const KisPageMutationSession::AdapterChanges &changes)
    {
        const auto pageExtent = [](const KisLogicalPageId &page) {
            // The backend checked pixel-coordinate overflow before admission.
            return QRect(page.column * KisTileData::WIDTH, page.row * KisTileData::HEIGHT,
                         KisTileData::WIDTH, KisTileData::HEIGHT);
        };
        return prepareReconciliation(table, extent,
            [&](const auto &page) {
                // Adapter-only entries need not have passed backend admission.
                return keptPages.contains(page.column, page.row) &&
                    std::binary_search(pages.cbegin(), pages.cend(), page, [](const auto &a, const auto &b) {
                        return std::tie(a.row, a.column) < std::tie(b.row, b.column);
                    });
            },
            [&](const auto &visit) {
                // Unchanged members have no Frame row. Changed members bind
                // the same location that final installation will consume.
                // The two disjoint sets cover the target without another map.
                for (const auto &page : pages)
                    if (kept.contains(pageExtent(page)) && !visit(page, nullptr)) return false;
                return changes.forEachLocated([&](const KisPageKey &key, bool, void *&location) {
                    return !kept.intersects(pageExtent(key.page)) || visit(key.page, &location);
                });
            });
    }

    bool prepareClear(KisTileHashTable &table, QString *error)
    {
        try {
            prepareRemovals(table, [](const KisLogicalPageId &) { return false; });
            return true; // Deleting the complete set needs no sort or lookup.
        } catch (const std::bad_alloc &) {
            if (error) *error = QStringLiteral("Clear compatibility preparation was refused");
            return false;
        }
    }

    bool prepare(KisTileHashTable &table, KisTiledExtentManager &extent, KisSurfaceId surface,
                 const KisPageMutationSession::AdapterChanges &changes, QString *error)
    {
        try {
            QRect coverage;
            const bool valid = changes.forEachLocated([&](const KisPageKey &key, bool present, void *&location) {
                if (!(key.surface == surface)) return true;
                const auto page = key.page;
                if (!KisTileHashTable::supportsCoordinates(page.column, page.row)) return false;
                location = preparePage(table, page, present ? KisTileHashTable::Update::Present : KisTileHashTable::Update::Absent);
                if (present) coverage |= QRect(page.column, page.row, 1, 1);
                return true;
            });
            // Filtering the original ordered changes preserves candidate order.
            if (valid && (coverage.isEmpty() || prepareExtent(extent, coverage, error))) return true;
        } catch (const std::bad_alloc &) {}
        if (error) *error = QStringLiteral("Compatibility adapter preparation was refused");
        return false;
    }

    qsizetype installLocked(KisTileHashTable &table, KisTiledExtentManager &extent,
                            KisTiledDataManagerPageStoreBackend *bridge,
                            const KisPageMutationSession::AdapterChanges &changes)
    {
        qsizetype installed = 0;
        changes.forEachLocated([&](const KisPageKey &key, bool present, void *&location) {
            if (key.surface == bridge->surface()) {
                installPage(table, extent, bridge, key.page, present, static_cast<KisTile *>(location));
                ++installed;
            }
            return true;
        });
        return installed;
    }

    // One synchronous manager cut for every ordinary operation. The caller
    // supplies its existing gate; no adapter/Store lock is added here.
    bool completeLocked(QWriteLocker &lock, KisTileHashTable &table, KisTiledExtentManager &extent,
                        KisTiledDataManagerPageStoreBackend *bridge,
                        const std::function<bool(QString *)> &publish,
                        const KisPageMutationSession::AdapterChanges &changes, QString *error)
    {
        if (!publish(error)) {
            lock.unlock();
            return false;
        }
        KisPageStoreDiagnosticTimer installed(bridge->store(),
            KisPageStoreDiagnosticPhase::MutationIndexRefresh, 0);
        installed.next(KisPageStoreDiagnosticPhase::MutationAdapterInstall, 0);
        const auto count = installLocked(table, extent, bridge, changes);
        lock.unlock();
        installed.next(KisPageStoreDiagnosticPhase::MutationAdapterInstalled, quint64(count));
        return true;
    }

    bool prepareRemovals(KisTileHashTable &table, const QVector<KisLogicalPageId> &pages, QString *error)
    {
        try {
            for (const auto &page : pages) preparePage(table, page, KisTileHashTable::Update::Absent);
            return true; // installRemovals consumes these directly, without lookup.
        } catch (const std::bad_alloc &) {
            if (error) *error = QStringLiteral("Removal compatibility preparation was refused");
            return false;
        }
    }

    void installRemovals(KisTileHashTable &table, KisTiledExtentManager &extent)
    {
        for (auto &candidate : m_prepared)
            if (candidate.storage.removesTile() && table.removePreparedTile(candidate.storage))
                extent.notifyTileRemoved(candidate.page.column, candidate.page.row);
    }

    void installLocked(KisTileHashTable &table, KisTiledExtentManager &extent,
                       KisTiledDataManagerPageStoreBackend *bridge, KisSurfaceId surface,
                       const KisPageRestorationChanges &changes)
    {
        installRemovals(table, extent);
        for (size_t i = 0; i < changes.size(); ++i) {
            const auto &version = changes.version(i);
            if (version.key.surface == surface && !version.isDefaultPixel() && changes.location(i))
                installPage(table, extent, bridge, version.key.page, true, static_cast<KisTile *>(changes.location(i)));
        }
        // A null changed location is a real missing candidate proved by the
        // complete root pass. This also installs unchanged backend-only pages.
        installMissing(table, extent, bridge);
    }

    void installReconciled(KisTileHashTable &table, KisTiledExtentManager &extent,
                           KisTiledDataManagerPageStoreBackend *bridge,
                           const KisPageMutationSession::AdapterChanges &changes)
    {
        installRemovals(table, extent);
        changes.forEachLocated([&](const KisPageKey &key, bool present, void *&location) {
            if (present) installPage(table, extent, bridge, key.page, true, static_cast<KisTile *>(location));
            return true;
        });
        installMissing(table, extent, bridge);
    }

private:
    template<class Contains, class ForEach>
    bool prepareReconciliation(KisTileHashTable &table, KisTiledExtentManager &extent,
                               const Contains &contains, const ForEach &forEach)
    {
        Q_ASSERT(m_prepared.isEmpty());
        try {
            QRect coverage;
            qsizetype targetCount = 0;
            // Enumerate the unique target set, proving membership for every
            // page and preparing only missing wrappers. No N-key copy is made.
            if (!forEach([&](const KisLogicalPageId &page, void **location) {
                if (!KisTileHashTable::supportsCoordinates(page.column, page.row)) return false;
                ++targetCount;
                coverage |= QRect(page.column, page.row, 1, 1);
                auto *tile = preparePage(table, page, KisTileHashTable::Update::Present);
                if (location) *location = tile;
                return true;
            })) return false;
            const auto missingCount = m_prepared.size();
            // Equal raw counts alone prove nothing. After the exhaustive pass,
            // H = N - missing proves that H has no extra members. Intent-only
            // wrappers otherwise require the reverse membership scan.
            if (table.numTiles() != targetCount - missingCount)
                prepareRemovals(table, contains);
            sortPrepared();
            return coverage.isEmpty() || extent.prepareTileRange(coverage);
        } catch (const std::bad_alloc &) {
            return false;
        }
    }

    template<class Contains>
    void prepareRemovals(KisTileHashTable &table, const Contains &contains)
    {
        const auto firstRemoval = m_prepared.size();
        {
            KisTileHashTableConstIterator it(&table);
            while (!it.isDone()) {
                const auto tile = it.tile();
                const KisLogicalPageId page{tile->col(), tile->row()};
                if (!contains(page)) appendPrepared(page, {});
                it.next();
            }
        }
        // Iterators release their table locks before preparing map keys/GC.
        for (qsizetype i = firstRemoval; i < m_prepared.size(); ++i) {
            auto &candidate = m_prepared[i];
            candidate.storage = table.prepareTileUpdate(candidate.page.column, candidate.page.row, KisTileHashTable::Update::Absent);
        }
    }

    void installMissing(KisTileHashTable &table, KisTiledExtentManager &extent,
                        KisTiledDataManagerPageStoreBackend *bridge)
    {
        // Missing wrappers need not belong to the canonical version D.
        // Installation above already consumed any overlapping candidates.
        for (auto &candidate : m_prepared)
            if (candidate.storage && !candidate.storage.removesTile()) {
                bool created = false;
                auto tile = table.installPreparedTile(candidate.storage, created);
                finishTileInstallation(extent, bridge, candidate.page, tile.data(), created);
            }
    }

    KisTile *preparePage(KisTileHashTable &table, const KisLogicalPageId &page, KisTileHashTable::Update update)
    {
        KisTile *existing = nullptr;
        auto candidate = table.prepareTileUpdate(page.column, page.row, update, &existing);
        if (candidate) appendPrepared(page, std::move(candidate));
        return existing;
    }

    void appendPrepared(const KisLogicalPageId &page, KisTileHashTable::PreparedTile candidate)
    {
        const auto size = m_prepared.size();
        if (size == m_prepared.capacity()) {
            if (size > std::numeric_limits<qsizetype>::max() / 2) throw std::bad_alloc();
            m_prepared.reserve(size * 2);
        }
        if (m_prepared.capacity() <= size) throw std::bad_alloc();
        m_prepared.append({page, std::move(candidate)});
    }

    void sortPrepared()
    {
        std::sort(m_prepared.begin(), m_prepared.end(), [](const auto &a, const auto &b) {
            return std::make_pair(a.page.row, a.page.column) < std::make_pair(b.page.row, b.page.column);
        });
    }

    void installPage(KisTileHashTable &table, KisTiledExtentManager &extent,
                     KisTiledDataManagerPageStoreBackend *bridge, const KisLogicalPageId &page, bool present, KisTile *known = nullptr)
    {
        const auto prepared = [&] {
            const auto key = std::make_pair(page.row, page.column);
            auto it = std::lower_bound(m_prepared.begin(), m_prepared.end(), key,
                [](const auto &candidate, const auto &key) {
                    return std::make_pair(candidate.page.row, candidate.page.column) < key;
                });
            return it != m_prepared.end() && it->page == page ? it : m_prepared.end();
        };
        if (!present) {
            const auto it = prepared();
            if (it == m_prepared.end()) {
                Q_ASSERT(!table.borrowTileForPreparedUpdate(page.column, page.row));
                return;
            }
            if (table.removePreparedTile(it->storage)) extent.notifyTileRemoved(page.column, page.row);
            return;
        }
        auto *tile = known ? known : table.borrowTileForPreparedUpdate(page.column, page.row);
        KisTileSP installed;
        bool created = false;
        if (!tile) {
            const auto it = prepared();
            if (it != m_prepared.end()) installed = table.installPreparedTile(it->storage, created);
            tile = installed.data();
        }
        finishTileInstallation(extent, bridge, page, tile, created);
    }

    static void finishTileInstallation(KisTiledExtentManager &extent,
        KisTiledDataManagerPageStoreBackend *bridge, const KisLogicalPageId &page, KisTile *tile, bool created)
    {
        Q_ASSERT_X(tile, "PreparedTileUpdates::finishTileInstallation", "admitted tile coverage was lost");
        tile->setPageStoreBridge(bridge, false);
        tile->invalidatePageStoreReadCache();
        if (created) extent.notifyTileAdded(page.column, page.row);
    }

    struct Candidate {
        KisLogicalPageId page;
        KisTileHashTable::PreparedTile storage;
    };
    QVarLengthArray<Candidate, 8> m_prepared;
};

struct DefaultTileDataDeleter
{
    void operator()(KisTileData *tile) const { tile->deref(); }
};
using PreparedDefaultTile = std::unique_ptr<KisTileData, DefaultTileDataDeleter>;

PreparedDefaultTile prepareDefaultTile(qint32 pixelSize, const quint8 *pixel)
{
    auto *tile = KisTileDataStore::instance()->createDefaultTileData(pixelSize, pixel);
    if (tile) tile->ref();
    return PreparedDefaultTile(tile);
}

class ScopedPageStoreWriteBatch
{
public:
    explicit ScopedPageStoreWriteBatch(
        KisTiledDataManagerPageStoreBackend *backend)
        : m_backend(backend && backend->isOperational() ? backend : nullptr)
    {
        if (!m_backend) return;
        m_batch = m_backend->beginMutationBatch(&m_error);
        if (!m_batch) reportFailure();
    }
    ~ScopedPageStoreWriteBatch()
    {
        if (!m_finished) reportFailure();
    }
    bool isValid() const { return !m_backend || bool(m_batch); }
    bool finish(bool succeeded = true,
        const KisPageMutationSession::AdapterPreparation &prepareAdapter = {},
        const KisPageMutationSession::AdapterInstallation &installAdapter = {})
    {
        if (m_finished) return m_succeeded;
        m_finished = true;
        if (!succeeded) {
            if (m_batch) m_batch->cancel();
        } else {
            m_succeeded = !m_backend || (m_batch && m_batch->finish(&m_error, prepareAdapter, installAdapter));
        }
        // Successful cancellation is not successful drawing. The original
        // batch delivers its working adapters before releasing its claims.
        if (!m_succeeded) reportFailure();
        return m_succeeded;
    }

    bool replaceFullTile(qint32 column,
                         qint32 row,
                         KisTileData *tileData,
                         bool sparseDefault)
    {
        return !m_backend ||
            (m_batch && m_batch->replaceFullTile(
                column, row, tileData, sparseDefault, &m_error));
    }

private:
    void reportFailure() const
    {
        KisStrokeJobFailureContext::reportFailure(m_error.isEmpty()
            ? QStringLiteral("Pixel write batch failed") : m_error);
    }
    KisTiledDataManagerPageStoreBackend *m_backend = nullptr;
    std::unique_ptr<KisTiledDataManagerPageStoreWriteBatch> m_batch;
    QString m_error;
    bool m_finished = false;
    bool m_succeeded = false;
};

}


/* The data area is divided into tiles each say 64x64 pixels (defined at compile time)
 * The tiles are laid out in a matrix that can have negative indexes.
 * The matrix grows automatically if needed (a call for writeaccess to a tile
 * outside the current extent)
 * Even though the matrix has grown it may still not contain tiles at specific positions.
 * They are created on demand
 */

KisTiledDataManager::KisTiledDataManager(quint32 pixelSize,
                                         const quint8 *defaultPixel)
    : KisTiledDataManager(pixelSize, defaultPixel,
        qEnvironmentVariableIntValue("KRITA_NATIVE_RESULT_PROTOCOL") == 104
            ? SelectionAuthority::NativeResult : SelectionAuthority::InternalPageStore)
{}

KisTiledDataManager::KisTiledDataManager(quint32 pixelSize,
    const quint8 *defaultPixel, SelectionAuthority authority)
{
    // Budget rejection has the same construction contract as allocation
    // failure: no partially initialized manager may escape to a caller.
    auto defaultTile = prepareDefaultTile(pixelSize, defaultPixel);
    if (!defaultTile) throw std::bad_alloc();

    // Destruction order matters: the table calls back into the memento owner.
    auto mementoManager = std::make_unique<KisMementoManager>();
    auto hashTable = std::make_unique<KisTileHashTable>(mementoManager.get());
    auto pixel = std::make_unique<quint8[]>(pixelSize);
    memcpy(pixel.get(), defaultPixel, pixelSize);
    hashTable->setDefaultTileData(defaultTile.get());
    mementoManager->setDefaultTileData(defaultTile.get());

    auto backend = authority == SelectionAuthority::InternalPageStore ? preparePageStoreBackend(this) : nullptr;
    QString pageStoreError;
    try {
        if (!backend || !backend->configure(pixelSize, defaultPixel, &pageStoreError)
            || !m_extentManager.configureStorage(backend->extentStorage())) backend.reset();
    } catch (const std::bad_alloc &) { backend.reset(); }
    mementoManager->setPageStoreBridge(backend.get());
    m_pixelSize = pixelSize;
    m_defaultPixel = pixel.get(); m_pageStoreBackend = backend.get();
    m_hashTable = hashTable.get(); m_mementoManager = mementoManager.get();
    if (authority == SelectionAuthority::NativeResult)
        m_nativeResult = KisNativeResultProtocol::createProduct(*this, pixelSize, defaultPixel);
    pixel.release(); backend.release(); hashTable.release(); mementoManager.release();
}

KisTiledDataManager::KisTiledDataManager(const KisTiledDataManager &dm)
    : KisShared()
{
    QReadLocker locker(&dm.m_lock);
    // Cloning shares the admitted default backing; it needs no new payload.
    PreparedDefaultTile defaultTile(dm.m_hashTable->refAndFetchDefaultTileData());
    auto mementoManager = std::make_unique<KisMementoManager>();
    mementoManager->setDefaultTileData(defaultTile.get());
    auto pixel = std::make_unique<quint8[]>(dm.m_pixelSize);
    memcpy(pixel.get(), dm.m_defaultPixel, dm.m_pixelSize);

    const bool nativeSource = bool(dm.m_nativeResult);
    const bool canonicalSource = dm.m_pageStoreBackend && dm.m_pageStoreBackend->isOperational();
    auto backend = canonicalSource ? preparePageStoreBackend(this) : nullptr;
    KisPageSnapshotArray<KisPageVersion> clonePages;
    if (canonicalSource) {
        QString pageStoreError;
        if (!backend || !backend->configureClone(*dm.m_pageStoreBackend, &pageStoreError, &clonePages)
            || !m_extentManager.configureStorage(backend->extentStorage())) {
            throw std::bad_alloc();
        }
    }
    mementoManager->setPageStoreBridge(backend.get());
    auto hashTable = (canonicalSource || nativeSource)
        ? std::make_unique<KisTileHashTable>(mementoManager.get())
        : std::make_unique<KisTileHashTable>(*dm.m_hashTable, mementoManager.get());
    if (canonicalSource || nativeSource) {
        hashTable->setDefaultTileData(defaultTile.get());
        // The working source index may contain unsealed pages. Build from
        // the same cut used for canonical initialization and replica adoption;
        // lazy wrappers need no read/cache copy from the source.
        for (const auto &version : std::as_const(clonePages)) {
            const auto &page = version.key.page;
            if (!KisTileHashTable::supportsCoordinates(page.column, page.row)) throw std::bad_alloc();
            bool added = false;
            auto tile = hashTable->getTileLazy(page.column, page.row, added);
            tile->setPageStoreBridge(backend.get(), false);
        }
    }
    // Keep every constructor resource owned until extent preparation succeeds.
    m_hashTable = hashTable.get();
    recalculateExtent();
    m_pixelSize = dm.m_pixelSize;
    m_defaultPixel = pixel.get(); m_pageStoreBackend = backend.get();
    m_hashTable = hashTable.get(); m_mementoManager = mementoManager.get();
    if (nativeSource) {
        m_nativeResult = dm.m_nativeResult->cloneTo(*this);
        if (!m_nativeResult) throw std::bad_alloc();
    }
    pixel.release(); backend.release(); hashTable.release(); mementoManager.release();
}

KisTiledDataManager::~KisTiledDataManager()
{
    /**
     * Here is an  explanation why we use hash table  and The Memento Manager
     * dynamically allocated We need to  destroy them in that very order. The
     * reason is that when hash table destroying all her child tiles they all
     * cry about it  to The Memento Manager using a  pointer.  So The Memento
     * Manager should be alive during  that destruction. We could  use shared
     * pointers instead, but they create too much overhead.
     */
    m_nativeResult.reset();
    m_mementoManager->setPageStoreBridge(nullptr);
    releaseHistoryDefaultTile();
    delete m_hashTable;
    delete m_pageStoreBackend;
    delete m_mementoManager;

    if (m_uniformClearTileData) {
        m_uniformClearTileData->release();
        m_uniformClearTileData = nullptr;
    }

    delete[] m_defaultPixel;
}

KisTileSP KisTiledDataManager::getTile(qint32 col, qint32 row, bool writable)
{
    if (m_nativeResult && writable) {
        QString failure; auto tile = m_nativeResult->writableTile(col, row, &failure);
        if (!tile) KisStrokeJobFailureContext::reportFailure(failure);
        return tile;
    }
    if (!writable) {
        bool present = false;
        return getReadOnlyTileLazy(col, row, present);
    }
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        auto tile = m_hashTable->getExistingTileForPreparedUpdate(col, row);
        if (!tile) {
            QString error;
            tile = m_pageStoreBackend->ensureWritablePage(col, row, &error);
            if (!tile) {
                KisStrokeJobFailureContext::reportFailure(error);
                return {};
            }
        }
        tile->setPageStoreBridge(m_pageStoreBackend, false);
        if (!tile->refreshPageStoreData()) {
            KisStrokeJobFailureContext::reportFailure(QStringLiteral("Cannot select writable tile read"));
            return {};
        }
        return tile;
    }
    bool newTile = false;
    KisTileSP tile = m_hashTable->getTileLazy(col, row, newTile);
    if (newTile) {
        m_extentManager.notifyTileAdded(col, row);
    }
    return tile;
}

KisTileSP KisTiledDataManager::getReadOnlyTileLazy(
    qint32 col, qint32 row, bool &existingTile)
{
    KisTileSP tile = m_hashTable->getReadOnlyTileLazy(col, row, existingTile);
    if (tile && m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        tile->setPageStoreBridge(m_pageStoreBackend, false);
        tile = m_pageStoreBackend->selectCurrentReadTile(*this, tile, &existingTile);
        if (!tile) {
            existingTile = false;
            KisStrokeJobFailureContext::reportFailure(QStringLiteral("Cannot select current tile read"));
        }
    }
    return tile;
}

KisTileSP KisTiledDataManager::getOldTile(
    qint32 col, qint32 row, bool &existingTile)
{
    if (m_nativeResult) {
        if (!m_nativeResult->hasHistory()) return getReadOnlyTileLazy(col, row, existingTile);
        QString failure; auto tile = m_nativeResult->oldTile(col, row, &existingTile, &failure);
        if (!tile) KisStrokeJobFailureContext::reportFailure(failure);
        else tile->setPageStoreBridge(nullptr, true);
        return tile;
    }
    if (!m_pageStoreBackend || !m_pageStoreBackend->isOperational() ||
        !m_pageStoreBackend->hasCurrentHistory()) {
        KisTileSP tile = m_mementoManager->getCommittedTile(
            col, row, existingTile);
        return tile ? tile : getReadOnlyTileLazy(col, row, existingTile);
    }
    try {
        const KisPageKey key{m_pageStoreBackend->surface(), {col, row}};
        const auto view = m_pageStoreBackend->captureReadView(true);
        // Select and protect the original cut before constructing a
        // compatibility wrapper. Failed preparation must not escape as
        // an oldData flag that can select current after history ends.
        KisPageStoreReadPage read(m_pageStoreBackend->store(), view, key,
                                 nullptr, KisPagePriority::Normal, false);
        KisTileData *data = m_pageStoreBackend->tileDataForReadPage(read);
        if (data) {
            KisTileSP oldTile = new KisTile(col, row, data, nullptr);
            // This wrapper is not in the manager's hash/detach lifecycle.
            // Its COW reference keeps the original storage, never the backend.
            oldTile->setPageStoreBridge(nullptr, true);
            existingTile = !read.version().isDefaultPixel();
            return oldTile;
        }
    } catch (const std::bad_alloc &) {
    }
    existingTile = false;
    KisStrokeJobFailureContext::reportFailure(QStringLiteral("Cannot select old tile read"));
    return {};
}

KisMementoSP KisTiledDataManager::getMemento()
{
    QWriteLocker locker(&m_lock);
    return getMementoLocked();
}

KisMementoSP KisTiledDataManager::getMementoLocked()
{
    if (m_nativeResult) return m_nativeResult->beginHistory();
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        auto memento = m_pageStoreBackend->beginHistory(m_defaultPixel, m_pixelSize);
        if (memento) {
            releaseHistoryDefaultTile();
            m_historyDefaultTileData = m_hashTable->refAndFetchDefaultTileData();
        }
        return memento;
    }
    KisMementoSP memento = m_mementoManager->getMemento();
    memento->saveOldDefaultPixel(m_defaultPixel, m_pixelSize);
    return memento;
}

void KisTiledDataManager::commit()
{
    QString error;
    if (!tryCommit(&error)) {
        qWarning() << "Data manager history commit failed:" << error;
    }
}

bool KisTiledDataManager::tryCommit(QString *error)
{
    if (m_nativeResult) return m_nativeResult->commitHistory(error);
    PreparedTileUpdates prepared;
    QWriteLocker locker(&m_lock);
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        const bool committed = m_pageStoreBackend->commitHistory(m_defaultPixel, m_pixelSize, error,
            [&](const auto &changes, QString *failure) {
                return prepared.prepare(*m_hashTable, m_extentManager, m_pageStoreBackend->surface(), changes, failure);
            }, [&](const auto &changes) {
                prepared.installLocked(*m_hashTable, m_extentManager, m_pageStoreBackend, changes);
            });
        if (!committed) return false;
        releaseHistoryDefaultTile();
        return true;
    }
    KisMementoSP memento = m_mementoManager->currentMemento();
    if (memento) memento->saveNewDefaultPixel(m_defaultPixel, m_pixelSize);
    m_mementoManager->commit();
    if (error) error->clear();
    return true;
}

void KisTiledDataManager::releaseHistoryDefaultTile()
{
    if (auto *tile = std::exchange(m_historyDefaultTileData, nullptr)) tile->deref();
}

// These are the original abort candidates, kept by the caller until after its
// manager gate is released. They never become another persistent history owner.
struct KisTiledDataManager::PreparedHistoryAbort
{
    std::unique_ptr<KisTileHashTable> table;
    std::unique_ptr<KisTiledExtentManager> extent;
};

bool KisTiledDataManager::abortPageStoreHistoryLocked(
    const KisMementoSP &memento, PreparedHistoryAbort &prepared, QString *error)
{
    if (!m_historyDefaultTileData) {
        if (error) *error = QStringLiteral("Abort has no retained default backing");
        return false;
    }
    try {
        QVector<KisLogicalPageId> pages;
        if (!m_pageStoreBackend->prepareHistoryAbort(memento, &pages, error)) return false;
        prepared.table = std::make_unique<KisTileHashTable>(m_mementoManager);
        prepared.table->setDefaultTileData(m_historyDefaultTileData);
        prepared.extent = std::make_unique<KisTiledExtentManager>();
        if (!prepared.extent->configureStorage(m_pageStoreBackend->extentStorage())) return false;
        for (const auto &page : pages) {
            bool added = false;
            auto tile = prepared.table->getTileLazy(page.column, page.row, added);
            tile->setPageStoreBridge(m_pageStoreBackend, false);
            if (added) prepared.extent->notifyTileAdded(page.column, page.row);
        }
    } catch (const std::bad_alloc &) {
        if (error) *error = QStringLiteral("Abort compatibility preparation ran out of memory");
        return false;
    }
    if (!m_pageStoreBackend->abortHistory(memento, error)) return false;

    // All adapter storage and the default backing exist before abort.
    auto *oldTable = m_hashTable;
    m_hashTable = prepared.table.release();
    prepared.table.reset(oldTable);
    m_extentManager.takePrepared(*prepared.extent);
    installDefaultPixel(memento->oldDefaultPixel(), m_historyDefaultTileData);
    releaseHistoryDefaultTile();
    return true;
}

bool KisTiledDataManager::tryAbort(KisMementoSP memento, QString *error)
{
    if (m_nativeResult) return m_nativeResult->abortHistory(memento, error);
    if (error) error->clear();
    if (!memento) {
        if (error) *error = QStringLiteral("Cannot abort an invalid memento");
        return false;
    }

    // Declare candidates before the lock so rejected/old tables are destroyed
    // after unlocking. No canonical state changes during compatibility preparation.
    PreparedHistoryAbort prepared;
    QWriteLocker locker(&m_lock);
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        return abortPageStoreHistoryLocked(memento, prepared, error);
    }

    // The legacy-only manager has no canonical abort primitive. Retain its
    // existing rollback protocol; never replay a failed PageStore cancellation.
    if (m_mementoManager->currentMemento() != memento) {
        if (error) *error = QStringLiteral("Legacy abort memento is not current");
        return false;
    }
    auto defaultTile = prepareDefaultTile(m_pixelSize, memento->oldDefaultPixel());
    if (!defaultTile) {
        if (error) *error = QStringLiteral("Legacy abort default preparation failed");
        return false;
    }
    m_mementoManager->rollback(m_hashTable, memento);
    installDefaultPixel(memento->oldDefaultPixel(), defaultTile.get());
    recalculateExtent();
    m_mementoManager->purgeHistory(memento);
    return true;
}

void KisTiledDataManager::rollback(KisMementoSP memento)
{
    restoreHistory(memento, true);
}

void KisTiledDataManager::rollforward(KisMementoSP memento)
{
    restoreHistory(memento, false);
}

void KisTiledDataManager::restoreHistory(KisMementoSP memento, bool before)
{
    if (m_nativeResult) {
        QString failure;
        if (!m_nativeResult->restoreHistory(memento, before, &failure)) KisStrokeJobFailureContext::reportFailure(failure);
        return;
    }
    if (!memento) return;
    if (!tryCommit()) return;
    PreparedTileUpdates preparedTiles;
    PreparedDefaultTile defaultTile;
    PreparedDefaultTile previousDefault;
    QWriteLocker locker(&m_lock);
    const quint8 *pixel = before ? memento->oldDefaultPixel() : memento->newDefaultPixel();
    const bool defaultChanged = pixel && std::memcmp(m_defaultPixel, pixel, m_pixelSize);
    if (defaultChanged) {
        previousDefault.reset(m_hashTable->refAndFetchDefaultTileData());
        defaultTile = prepareDefaultTile(m_pixelSize, pixel);
    }
    // Prepare every fallible default resource before moving either history.
    if (defaultChanged && !defaultTile) return;

    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        const auto surface = m_pageStoreBackend->surface();
        const bool restored = m_pageStoreBackend->restoreHistory(memento, before, nullptr,
            [&](const KisImageEpochRootSnapshot &root, const KisPageRestorationChanges &changes) {
                return preparedTiles.prepare(*m_hashTable, m_extentManager, surface, root, changes);
            }, [&](const KisPageRestorationChanges &changes) {
                preparedTiles.installLocked(*m_hashTable, m_extentManager, m_pageStoreBackend, surface, changes);
                if (defaultChanged) installDefaultPixel(pixel, defaultTile.get());
            });
        if (!restored) return;
    } else {
        if (before) m_mementoManager->rollback(m_hashTable, memento);
        else m_mementoManager->rollforward(m_hashTable, memento);
        if (defaultChanged) installDefaultPixel(pixel, defaultTile.get());
        recalculateExtent();
    }
}

bool KisTiledDataManager::hasCurrentMemento() const
{
    if (m_nativeResult) return m_nativeResult->hasHistory();
    return m_pageStoreBackend && m_pageStoreBackend->isOperational()
        ? m_pageStoreBackend->hasCurrentHistory()
        : m_mementoManager->hasCurrentMemento();
}

void KisTiledDataManager::purgeHistory(KisMementoSP oldestMemento)
{
    if (m_nativeResult) { m_nativeResult->purgeHistory(oldestMemento); return; }
    QWriteLocker locker(&m_lock);
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        m_pageStoreBackend->purgeHistory(
            oldestMemento, m_defaultPixel, m_pixelSize);
        if (!m_pageStoreBackend->hasCurrentHistory()) releaseHistoryDefaultTile();
    } else {
        m_mementoManager->purgeHistory(oldestMemento);
    }
}

void KisTiledDataManager::setDefaultPixel(const quint8 *defaultPixel)
{
    if (m_nativeResult) {
        if (KisStrokeJobFailureContext::currentJobHasFailed()) return;
        QString failure;
        if (!m_nativeResult->setDefaultPixel(QByteArray(reinterpret_cast<const char *>(defaultPixel), m_pixelSize), &failure))
            KisStrokeJobFailureContext::reportFailure(failure);
        return;
    }
    if (KisStrokeJobFailureContext::currentJobHasFailed()) return;
    QWriteLocker locker(&m_lock);
    if (!std::memcmp(m_defaultPixel, defaultPixel, m_pixelSize)) return;
    auto defaultTile = prepareDefaultTile(m_pixelSize, defaultPixel);
    if (!defaultTile) {
        KisStrokeJobFailureContext::reportFailure(QStringLiteral("Default pixel backing allocation failed"));
        return;
    }
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        const QByteArray pixel(reinterpret_cast<const char *>(defaultPixel), m_pixelSize);
        QString error;
        if (!m_pageStoreBackend->setDefaultPixel(pixel, &error)) {
            KisStrokeJobFailureContext::reportFailure(error);
            return;
        }
    }
    installDefaultPixel(defaultPixel, defaultTile.get());
}

void KisTiledDataManager::installDefaultPixel(const quint8 *defaultPixel, KisTileData *tile)
{
    Q_ASSERT(tile);
    m_hashTable->setDefaultTileData(tile);
    m_mementoManager->setDefaultTileData(tile);
    memcpy(m_defaultPixel, defaultPixel, pixelSize());
}

bool KisTiledDataManager::write(KisPaintDeviceWriter &store)
{
    QReadLocker locker(&m_lock);

    bool retval = true;

    if(CURRENT_VERSION == LEGACY_VERSION) {
        char str[80];
        snprintf(str, 80, "%d\n", m_hashTable->numTiles());
        retval = store.write(str, strlen(str));
    }
    else {
        retval = writeTilesHeader(store, m_hashTable->numTiles());
    }


    KisTileHashTableConstIterator iter(m_hashTable);
    KisTileSP tile;

    KisAbstractTileCompressorSP compressor =
        KisTileCompressorFactory::create(CURRENT_VERSION);

    while ((tile = iter.tile())) {
        retval = compressor->writeTile(tile, store);
        if (!retval) {
            warnFile << "Failed to write tile";
            break;
        }
        iter.next();
    }

    return retval;
}
bool KisTiledDataManager::read(QIODevice *stream) try
{
    clear();

    // The original batch owns decoding and cancellation. Its final resources
    // outlive the manager gate; no undo result escapes this file import.
    std::unique_ptr<KisTiledDataManagerPageStoreWriteBatch> batch;
    QWriteLocker locker(&m_lock);
    if (!stream) return false;

    const qint32 maxLineLength = 79; // Legacy magic
    const QByteArray line = stream->readLine(maxLineLength).trimmed();
    if (line.isEmpty()) return false;

    quint32 numTiles;
    qint32 tilesVersion = LEGACY_VERSION;

    if (line[0] == 'V') {
        QList<QByteArray> lineItems = line.split(' ');

        QString keyword = lineItems.takeFirst();
        Q_ASSERT(keyword == "VERSION");

        tilesVersion = lineItems.takeFirst().toInt();

        if (!processTilesHeader(stream, numTiles)) return false;
    } else {
        numTiles = line.toUInt();
    }

    KisAbstractTileCompressorSP compressor =
        KisTileCompressorFactory::create(tilesVersion);

    if (m_nativeResult) {
        // The decoder's real tile locks append to the original new-protocol
        // write set. One all-D cut follows successful decoding of every tile.
        auto scope = m_nativeResult->beginIterator();
        if (!scope) return false;
        for (quint32 i = 0; i < numTiles; ++i)
            if (!compressor->readTile(stream, this)) return false;
        locker.unlock();
        return scope->finish();
    }
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        batch = m_pageStoreBackend->beginMutationBatch(nullptr, true);
        if (!batch) return false;
    } else if (!getMementoLocked()) {
        return false;
    }
    bool readSuccess = true;
    for (quint32 i = 0; i < numTiles; i++) {
        if (!compressor->readTile(stream, this)) readSuccess = false;
    }

    if (batch) return readSuccess && batch->finish();
    m_mementoManager->commit();
    return readSuccess;
}
catch (const std::bad_alloc &)
{
    warnFile << "Tile import storage preparation was refused";
    return false;
}

bool KisTiledDataManager::writeTilesHeader(KisPaintDeviceWriter &store, quint32 numTiles)
{
    QString buffer;

    buffer = QString("VERSION %1\n"
                     "TILEWIDTH %2\n"
                     "TILEHEIGHT %3\n"
                     "PIXELSIZE %4\n"
                     "DATA %5\n")
        .arg(CURRENT_VERSION)
        .arg(KisTileData::WIDTH)
        .arg(KisTileData::HEIGHT)
        .arg(pixelSize())
        .arg(numTiles);

    return store.write(buffer.toLatin1());
}

#define takeOneLine(stream, maxLine, keyword, value)            \
    do {                                                        \
        QByteArray line = stream->readLine(maxLine);            \
        line = line.trimmed();                                  \
        QList<QByteArray> lineItems = line.split(' ');          \
        keyword = lineItems.takeFirst();                        \
        value = lineItems.takeFirst().toInt();                  \
    } while(0)                                                  \


bool KisTiledDataManager::processTilesHeader(QIODevice *stream, quint32 &numTiles)
{
    /**
     * We assume that there is only one version of this header
     * possible. In case we invent something new, it'll be quite easy
     * to modify the behavior
     */

    const qint32 maxLineLength = 25;
    const qint32 totalNumTests = 4;
    bool foundDataMark = false;
    qint32 testsPassed = 0;

    QString keyword;
    qint32 value;

    while(!foundDataMark && stream->canReadLine()) {
        takeOneLine(stream, maxLineLength, keyword, value);

        if (keyword == "TILEWIDTH") {
            if(value != KisTileData::WIDTH)
                goto wrongString;
        }
        else if (keyword == "TILEHEIGHT") {
            if(value != KisTileData::HEIGHT)
                goto wrongString;
        }
        else if (keyword == "PIXELSIZE") {
            if((quint32)value != pixelSize())
                goto wrongString;
        }
        else if (keyword == "DATA") {
            numTiles = value;
            foundDataMark = true;
        }
        else {
            goto wrongString;
        }

        testsPassed++;
    }

    if(testsPassed != totalNumTests) {
        warnTiles << "Not enough fields of tiles header present"
                  << testsPassed << "of" << totalNumTests;
    }

    return testsPassed == totalNumTests;

wrongString:
    warnTiles << "Wrong string in tiles header:" << keyword << value;
    return false;
}

void KisTiledDataManager::purge(const QRect& area)
{
    if (m_nativeResult) {
        QString failure;
        if (!m_nativeResult->purge(area, &failure)) KisStrokeJobFailureContext::reportFailure(failure);
        return;
    }
    // Callers finish their iterators and sequence device writes before purge.
    // Keep that boundary across comparison and removal; a later page claim
    // alone would not validate a predicate observed before concurrent writes.
    QVector<KisLogicalPageId> pages;
    {
        const qint32 tileDataSize = KisTileData::HEIGHT * KisTileData::WIDTH * pixelSize();
        KisTileData *tileData = m_hashTable->refAndFetchDefaultTileData();
        if (!tileData->blockSwapping()) {
            tileData->deref();
            return;
        }
        const auto releaseDefault = qScopeGuard([&] {
            tileData->unblockSwapping();
            tileData->deref();
        });
        const quint8 *defaultData = tileData->data();

        KisTileHashTableConstIterator iter(m_hashTable);
        KisTileSP tile;

        while ((tile = iter.tile())) {
            if (tile->extent().intersects(area)) {
                if (!tile->lockForRead()) return;
                const auto unlock = qScopeGuard([&] { tile->unlockForRead(); });
                const quint8 *data = tile->data();
                if (!data) return;
                if(memcmp(defaultData, data, tileDataSize) == 0) {
                    pages.append({tile->col(), tile->row()});
                }
            }
            iter.next();
        }

    }
    if (pages.isEmpty()) return;
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        OperationDelivery delivery;
        PreparedTileUpdates preparedTiles;
        m_pageStoreBackend->removePages(pages, nullptr, &delivery,
            [&](QString *failure) {
                KisPageStoreDiagnosticTimer phase(m_pageStoreBackend->store(),
                    KisPageStoreDiagnosticPhase::MutationAdapterPrepare, quint64(pages.size()));
                const bool prepared = preparedTiles.prepareRemovals(*m_hashTable, pages, failure);
                phase.next(KisPageStoreDiagnosticPhase::MutationAdapterPrepared, quint64(pages.size()));
                return prepared;
            }, [&](const auto &publish, const auto &, QString *failure) {
                if (!publish(failure)) return false;
                KisPageStoreDiagnosticTimer phase(m_pageStoreBackend->store(),
                    KisPageStoreDiagnosticPhase::MutationIndexRefresh, quint64(pages.size()));
                phase.next(KisPageStoreDiagnosticPhase::MutationAdapterInstall, quint64(pages.size()));
                preparedTiles.installRemovals(*m_hashTable, m_extentManager);
                phase.next(KisPageStoreDiagnosticPhase::MutationAdapterInstalled, quint64(pages.size()));
                return true;
            });
        return;
    }
    for (const auto &page : pages) {
        if (m_hashTable->deleteTile(page.column, page.row)) {
            m_extentManager.notifyTileRemoved(page.column, page.row);
        }
    }
}

quint8* KisTiledDataManager::duplicatePixel(qint32 num, const quint8 *pixel)
{
    const qint32 pixelSize = this->pixelSize();
    /* FIXME:  Make a fun filling here */
    quint8 *dstBuf = new quint8[num * pixelSize];
    quint8 *dstIt = dstBuf;
    for (qint32 i = 0; i < num; i++) {
        memcpy(dstIt, pixel, pixelSize);
        dstIt += pixelSize;
    }
    return dstBuf;
}

void KisTiledDataManager::clear(QRect clearRect, const quint8 *clearPixel)
{
    if (m_nativeResult) {
        if (KisStrokeJobFailureContext::currentJobHasFailed()) return;
        QString failure;
        if (!m_nativeResult->fill(clearRect, QByteArray(reinterpret_cast<const char *>(clearPixel), m_pixelSize), &failure))
            KisStrokeJobFailureContext::reportFailure(failure);
        return;
    }

    if (KisStrokeJobFailureContext::currentJobHasFailed()) return;
    if (clearPixel == 0)
        clearPixel = m_defaultPixel;

    if (clearRect.isEmpty())
        return;

    const qint32 pixelSize = this->pixelSize();

    bool pixelBytesAreDefault = !memcmp(clearPixel, m_defaultPixel, pixelSize);

    bool pixelBytesAreTheSame = true;
    for (qint32 i = 0; i < pixelSize; ++i) {
        if (clearPixel[i] != clearPixel[0]) {
            pixelBytesAreTheSame = false;
            break;
        }
    }

    if (pixelBytesAreDefault) {
        clearRect &= m_extentManager.extent();
        if (clearRect.isEmpty()) return;
    }

    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        if (!KisTileHashTable::supportsCoordinates(xToCol(clearRect.left()), yToRow(clearRect.top())) ||
            !KisTileHashTable::supportsCoordinates(xToCol(clearRect.right()), yToRow(clearRect.bottom()))) {
            KisStrokeJobFailureContext::reportFailure(QStringLiteral("Clear exceeds compatibility index coordinates"));
            return;
        }
        QString error;
        OperationDelivery delivery;
        PreparedTileUpdates preparedTiles;
        const QRect pages(QPoint(xToCol(clearRect.left()), yToRow(clearRect.top())),
                          QPoint(xToCol(clearRect.right()), yToRow(clearRect.bottom())));
        if (!m_pageStoreBackend->fillRect(clearRect,
                QByteArray(reinterpret_cast<const char *>(clearPixel), pixelSize), &error, nullptr, &delivery,
                [&](bool mayRemove, const auto &admitted, QString *failure) {
                    if (!prepareExtent(m_extentManager, pages, failure)) return false;
                    QReadLocker lock(&m_lock);
                    // The producer supplies its admitted default comparison.
                    // Do not repeat the caller's stale pre-wait selection.
                    return preparedTiles.prepare(*m_hashTable, admitted, failure,
                        mayRemove ? completelyCoveredPages(clearRect, pages) : QRect{});
                }, [&](const auto &publish, const auto &changes, QString *failure) {
                    QWriteLocker lock(&m_lock);
                    return preparedTiles.completeLocked(lock, *m_hashTable, m_extentManager,
                        m_pageStoreBackend, publish, changes, failure);
                }))
            KisStrokeJobFailureContext::reportFailure(error);
        return;
    }

    // The fallback still reports failure on an early return or exception,
    // without carrying an always-empty PageStore batch to do so.
    auto reportFailure = qScopeGuard([] {
        KisStrokeJobFailureContext::reportFailure(QStringLiteral("Legacy clear failed"));
    });
    qint32 firstColumn = xToCol(clearRect.left());
    qint32 lastColumn = xToCol(clearRect.right());

    qint32 firstRow = yToRow(clearRect.top());
    qint32 lastRow = yToRow(clearRect.bottom());

    const quint32 rowStride = KisTileData::WIDTH * pixelSize;

    // Generate one row
    quint8 *clearPixelData = 0;
    quint32 maxRunLength = qMin(clearRect.width(), KisTileData::WIDTH);
    clearPixelData = duplicatePixel(maxRunLength, clearPixel);

    KisTileData *td = 0;
    bool reusesUniformClearBacking = false;
    if (!pixelBytesAreDefault &&
        clearRect.width() >= KisTileData::WIDTH &&
        clearRect.height() >= KisTileData::HEIGHT) {
        const QByteArray clearPixelBytes(
            reinterpret_cast<const char *>(clearPixel), pixelSize);
        {
            QMutexLocker uniformLocker(&m_uniformClearMutex);
            reusesUniformClearBacking = m_uniformClearTileData &&
                m_uniformClearPixel == clearPixelBytes;
            if (reusesUniformClearBacking) {
                td = m_uniformClearTileData;
            } else {
                td = KisTileDataStore::instance()->createDefaultTileData(
                    pixelSize, clearPixel);
                if (!td) {
                    delete[] clearPixelData;
                    return;
                }
                if (m_uniformClearTileData) {
                    m_uniformClearTileData->release();
                }
                m_uniformClearTileData = td;
                m_uniformClearTileData->acquire();
                m_uniformClearPixel = clearPixelBytes;
            }
            // Hold the selected backing independently of the cache. Another
            // clear may replace the cache entry as soon as this lock drops.
            td->acquire();
        }
    }

    for (qint32 row = firstRow; row <= lastRow; ++row) {
        for (qint32 column = firstColumn; column <= lastColumn; ++column) {

            QRect tileRect(column*KisTileData::WIDTH, row*KisTileData::HEIGHT,
                           KisTileData::WIDTH, KisTileData::HEIGHT);
            QRect clearTileRect = clearRect & tileRect;

            if (clearTileRect == tileRect) {
                 // Clear whole tile
                 if (reusesUniformClearBacking) {
                     bool existingTile = false;
                     KisTileSP current = m_hashTable->getReadOnlyTileLazy(
                         column, row, existingTile);
                     if (existingTile) {
                         if (!current->lockForRead()) {
                             if (td) td->release();
                             delete[] clearPixelData;
                             return;
                         }
                         KisTileData *currentData = current->tileData();
                         current->unlockForRead();
                         if (!currentData) {
                             if (td) td->release();
                             delete[] clearPixelData;
                             return;
                         }
                         const bool alreadyCleared = currentData == td;
                         if (alreadyCleared) continue;
                     }
                 }
                 const bool wasDeleted =
                     m_hashTable->deleteTile(column, row);

                 if (wasDeleted) {
                     m_extentManager.notifyTileRemoved(column, row);
                 }


                 if (!pixelBytesAreDefault) {
                     KisTileSP clearedTile = KisTileSP(new KisTile(column, row, td, m_mementoManager));
                     m_hashTable->addTile(clearedTile);
                     m_extentManager.notifyTileAdded(column, row);
                 }
            } else {
                const qint32 lineSize = clearTileRect.width() * pixelSize;
                qint32 rowsRemaining = clearTileRect.height();

                KisTileDataWrapper tw(this,
                                      clearTileRect.left(),
                                      clearTileRect.top(),
                                      KisTileDataWrapper::WRITE);
                quint8* tileIt = tw.data();
                if (!tileIt) {
                    if (td) td->release();
                    delete[] clearPixelData;
                    return;
                }

                if (pixelBytesAreTheSame) {
                    while (rowsRemaining > 0) {
                        memset(tileIt, *clearPixelData, lineSize);
                        tileIt += rowStride;
                        rowsRemaining--;
                    }
                } else {
                    while (rowsRemaining > 0) {
                        memcpy(tileIt, clearPixelData, lineSize);
                        tileIt += rowStride;
                        rowsRemaining--;
                    }
                }
            }
        }
    }

    if (td) td->release();
    delete[] clearPixelData;
    reportFailure.dismiss();
}

void KisTiledDataManager::clear(QRect clearRect, quint8 clearValue)
{
    quint8 *buf = new quint8[pixelSize()];
    memset(buf, clearValue, pixelSize());
    clear(clearRect, buf);
    delete[] buf;
}

void KisTiledDataManager::clear(qint32 x, qint32 y, qint32 w, qint32 h, const quint8 *clearPixel)
{
    clear(QRect(x, y, w, h), clearPixel);
}
void KisTiledDataManager::clear(qint32 x, qint32 y, qint32 w, qint32 h, quint8 clearValue)
{
    clear(QRect(x, y, w, h), clearValue);
}

void KisTiledDataManager::clear()
{
    if (m_nativeResult) {
        if (KisStrokeJobFailureContext::currentJobHasFailed()) return;
        QString failure;
        if (!m_nativeResult->clear(&failure)) KisStrokeJobFailureContext::reportFailure(failure);
        return;
    }
    if (KisStrokeJobFailureContext::currentJobHasFailed()) return;
    OperationDelivery delivery;
    PreparedTileUpdates preparedTiles;
    QWriteLocker locker(&m_lock);
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        QString error;
        if (!m_pageStoreBackend->clearAll(&error, &delivery,
            [&](QString *failure) {
                KisPageStoreDiagnosticTimer phase(m_pageStoreBackend->store(),
                    KisPageStoreDiagnosticPhase::MutationAdapterPrepare, 1);
                const bool prepared = preparedTiles.prepareClear(*m_hashTable, failure);
                phase.next(KisPageStoreDiagnosticPhase::MutationAdapterPrepared, 1);
                return prepared;
            }, [&](const auto &publish, const auto &, QString *failure) {
                if (!publish(failure)) return false;
                KisPageStoreDiagnosticTimer phase(m_pageStoreBackend->store(),
                    KisPageStoreDiagnosticPhase::MutationIndexRefresh, 1);
                phase.next(KisPageStoreDiagnosticPhase::MutationAdapterInstall, 1);
                preparedTiles.installRemovals(*m_hashTable, m_extentManager);
                phase.next(KisPageStoreDiagnosticPhase::MutationAdapterInstalled, 1);
                return true;
            })) {
            KisStrokeJobFailureContext::reportFailure(error);
        }
        return;
    }
    m_hashTable->clear();
    m_extentManager.clear();
}

bool KisTiledDataManager::copyNeedsLiveLegacySource(const QRect &rect, bool oldData) const
{
    // The caller rejects unsupported coordinates before mutation. Do not let
    // compatibility classification traverse a range the index cannot hold.
    if (!KisTileHashTable::supportsCoordinates(xToCol(rect.left()), yToRow(rect.top())) ||
        !KisTileHashTable::supportsCoordinates(xToCol(rect.right()), yToRow(rect.bottom()))) return false;
    if (oldData && m_pageStoreBackend->hasCurrentHistory()) return false;
    // No global counter/lock added to tile lock/unlock. Probe only this copy's
    // source range, without creating wrappers, exposing bytes or publishing.
    // Already borrowed legacy writers cannot be revoked or silently read as
    // their sealed before-image. Those operations keep the compatibility path.
    for (qint64 row = yToRow(rect.top()); row <= yToRow(rect.bottom()); ++row)
        for (qint64 column = xToCol(rect.left()); column <= xToCol(rect.right()); ++column) {
            auto tile = m_hashTable->getExistingTile(qint32(column), qint32(row));
            if (tile && tile->hasPageStoreWriteIntent()) return true;
        }
    return false;
}

PixelOperationResult KisTiledDataManager::copyPageStore(
    KisTiledDataManager *source, QRect &rect, bool oldSource, bool rough, QString *error)
{
    const bool nativeTarget = m_pageStoreBackend && m_pageStoreBackend->isOperational();
    const bool nativeSource = source->m_pageStoreBackend && source->m_pageStoreBackend->isOperational();
    if (!nativeTarget || !nativeSource) {
        // Legacy current copies can retain the old empty-space optimization.
        // History-before bounds are not the current working extent.
        if (!nativeTarget && !nativeSource && !oldSource &&
            !memcmp(source->defaultPixel(), m_defaultPixel, pixelSize()))
            rect &= source->extent().united(extent());
        return PixelOperationResult::Unavailable;
    }
    // Only existing wrappers can hold live legacy source intent. Limit this
    // classification, not the native pixel selection, to working bounds.
    const QRect legacyCandidates = rect.intersected(source->m_extentManager.extent());
    if (!legacyCandidates.isEmpty() && source->copyNeedsLiveLegacySource(legacyCandidates, oldSource))
        return PixelOperationResult::Unavailable;

    // Freeze the source before computing bounds, preparing capacity or writing.
    // Working extent may lag a sealed page or include unpublished intent;
    // neither is a reason to clip a fixed source through that separate value.
    const auto sourceView = source->m_pageStoreBackend->captureReadView(oldSource, error);
    KisSurfaceEpochState sourceState, targetState;
    if (!sourceView.resolveSurfaceState(source->m_pageStoreBackend->surface(), &sourceState) ||
        !m_pageStoreBackend->resolveCurrentSurfaceState(&targetState, error)) {
        if (error) *error = QStringLiteral("Copy bounds selection is unavailable");
        return PixelOperationResult::Failed;
    }
    // Equal sparse defaults need work only where the selected geometry has content.
    // Include the destination: copying default pixels may remove its old pages.
    // Different defaults require the caller's entire requested rectangle.
    const QRect requestedRect = rect;
    if (sourceState.format.defaultPixel == targetState.format.defaultPixel)
        rect &= sourceState.contentExtent.united(targetState.contentExtent);
    if (rect.isEmpty()) return PixelOperationResult::Succeeded;
    if (!KisTileHashTable::supportsCoordinates(xToCol(rect.left()), yToRow(rect.top())) ||
        !KisTileHashTable::supportsCoordinates(xToCol(rect.right()), yToRow(rect.bottom()))) {
        if (error) *error = QStringLiteral("Copy exceeds compatibility index coordinates");
        return PixelOperationResult::Failed;
    }
    OperationDelivery delivery;
    PreparedTileUpdates preparedTiles;
    const QRect pages(QPoint(xToCol(rect.left()), yToRow(rect.top())),
                      QPoint(xToCol(rect.right()), yToRow(rect.bottom())));
    if (!m_pageStoreBackend->copyFrom(*source->m_pageStoreBackend, sourceView, requestedRect,
            rect, rough, error, nullptr, &delivery,
            [&](bool mayRemove, const auto &admitted, QString *failure) {
                if (!prepareExtent(m_extentManager, pages, failure)) return false;
                QReadLocker lock(&m_lock);
                return preparedTiles.prepare(*m_hashTable, admitted, failure,
                    mayRemove ? (rough ? pages : completelyCoveredPages(rect, pages)) : QRect{});
            }, [&](const auto &publish, const auto &changes, QString *failure) {
                QWriteLocker lock(&m_lock);
                return preparedTiles.completeLocked(lock, *m_hashTable, m_extentManager,
                    m_pageStoreBackend, publish, changes, failure);
            }))
        return PixelOperationResult::Failed;
    return PixelOperationResult::Succeeded;
}

template<bool useOldSrcData, bool rough>
void KisTiledDataManager::bitBltImpl(KisTiledDataManager *srcDM, const QRect &requestedRect)
{
    if (KisStrokeJobFailureContext::currentJobHasFailed()) return;
    if (requestedRect.isEmpty()) return;
    QRect rect = requestedRect;
    QString error;
    if (m_nativeResult) {
        if (rough) rect = QRect(QPoint(xToCol(rect.left()) * 64, yToRow(rect.top()) * 64),
            QPoint((xToCol(rect.right()) + 1) * 64 - 1, (yToRow(rect.bottom()) + 1) * 64 - 1));
        bool copied = false;
        if (srcDM->m_nativeResult) {
            auto selected = useOldSrcData ? srcDM->m_nativeResult->captureOldData(&error) : srcDM->m_nativeResult->capture(&error);
            copied = selected && m_nativeResult->copyFrom(selected, rect, &error);
        } else {
            // A real external source boundary, not a route through the old
            // destination private/seal/publication pipeline.
            copied = m_nativeResult->mutate(rect, [&](qint32 col, qint32 row, quint8 *pixels, quint32 stride, const QRect &local) {
                bool present;
                auto tile = useOldSrcData ? srcDM->getOldTile(col, row, present) : srcDM->getReadOnlyTileLazy(col, row, present);
                if (!tile || !tile->lockForRead()) return false;
                const auto unpin = qScopeGuard([&] { tile->unlockForRead(); });
                const auto *source = tile->data(); if (!source) return false;
                for (int y = local.top(); y <= local.bottom(); ++y)
                    std::memcpy(pixels + y * stride + local.x() * m_pixelSize,
                        source + (y * 64 + local.x()) * m_pixelSize, local.width() * m_pixelSize);
                return true;
            }, true, {}, &error);
        }
        if (!copied) KisStrokeJobFailureContext::reportFailure(error);
        return;
    }
    const auto copied = copyPageStore(srcDM, rect, useOldSrcData, rough, &error);
    if (copied != PixelOperationResult::Unavailable) {
        if (copied == PixelOperationResult::Failed)
            KisStrokeJobFailureContext::reportFailure(error);
        return;
    }
    if (rect.isEmpty()) return;
    const bool pageStoreTarget = m_pageStoreBackend && m_pageStoreBackend->isOperational();
    ScopedPageStoreWriteBatch pageStoreBatch(m_pageStoreBackend);
    PreparedTileUpdates preparedTiles; // Destroy before the batch releases admission.
    if (!pageStoreBatch.isValid()) return;

    const qint32 pixelSize = this->pixelSize();
    const bool defaultPixelsCoincide =
        !memcmp(srcDM->defaultPixel(), m_defaultPixel, pixelSize);

    const quint32 rowStride = KisTileData::WIDTH * pixelSize;

    qint32 firstColumn = xToCol(rect.left());
    qint32 lastColumn = xToCol(rect.right());

    qint32 firstRow = yToRow(rect.top());
    qint32 lastRow = yToRow(rect.bottom());

    for (qint32 row = firstRow; row <= lastRow; ++row) {
        for (qint32 column = firstColumn; column <= lastColumn; ++column) {

            bool srcTileExists = false;

            KisTileSP srcTile = useOldSrcData ?
                srcDM->getOldTile(column, row, srcTileExists) :
                srcDM->getReadOnlyTileLazy(column, row, srcTileExists);
            if (!srcTile) return;

            QRect tileRect(column*KisTileData::WIDTH, row*KisTileData::HEIGHT,
                           KisTileData::WIDTH, KisTileData::HEIGHT);
            QRect cloneTileRect = rough ? tileRect : rect & tileRect;

            if (cloneTileRect == tileRect) {
                 // Clone whole tile
                 if (!srcTile->lockForRead()) return;
                 const auto unlockSource = qScopeGuard([&] { srcTile->unlockForRead(); });
                 KisTileData *td = srcTile->tileData();
                 if (!td) return;
                 const bool sparseDefault =
                     !srcTileExists && defaultPixelsCoincide;
                 bool dstTileExists = false;
                 KisTileSP dstTile = getReadOnlyTileLazy(
                     column, row, dstTileExists);
                 if (!dstTile) return;
                 bool unchanged = sparseDefault && !dstTileExists;
                 if (!sparseDefault && dstTileExists) {
                     if (!dstTile->lockForRead()) return;
                     const auto unlockDestination = qScopeGuard([&] { dstTile->unlockForRead(); });
                     KisTileData *dstData = dstTile->tileData();
                     if (!dstData) return;
                     unchanged = dstData == td ||
                         memcmp(dstData->data(), td->data(),
                                size_t(rowStride) * KisTileData::HEIGHT) == 0;
                 }
                 if (unchanged) continue;
                 const bool pageStoreReplaced =
                     pageStoreBatch.replaceFullTile(
                         column, row, td, sparseDefault);
                 if (!pageStoreReplaced) return;
                 // PageStore keeps the old index until publication accepts.
                 // The original batch supplies final changes to the installer.
                 if (pageStoreTarget) continue;
                 const bool wasDeleted =
                     m_hashTable->deleteTile(column, row);

                 if (srcTileExists || !defaultPixelsCoincide) {
                     KisTileSP clonedTile = KisTileSP(new KisTile(column, row, td, m_mementoManager));
                     clonedTile->setPageStoreBridge(m_pageStoreBackend, false);

                     m_hashTable->addTile(clonedTile);

                     if (!wasDeleted) {
                         m_extentManager.notifyTileAdded(column, row);
                     }
                 } else if (wasDeleted) {
                     m_extentManager.notifyTileRemoved(column, row);
                 }

            } else {
                const qint32 lineSize = cloneTileRect.width() * pixelSize;
                qint32 rowsRemaining = cloneTileRect.height();

                KisTileDataWrapper tw(this,
                                      cloneTileRect.left(),
                                      cloneTileRect.top(),
                                      KisTileDataWrapper::WRITE);
                if (!tw.isValid() || !srcTile->lockForRead()) return;
                const auto unlockSource = qScopeGuard([&] { srcTile->unlockForRead(); });
                // We suppose that the shift in both tiles is the same
                const quint8 *sourceData = srcTile->data();
                quint8* dstTileIt = tw.data();
                if (!sourceData || !dstTileIt) return;
                const quint8* srcTileIt = sourceData + tw.offset();

                while (rowsRemaining > 0) {
                    memcpy(dstTileIt, srcTileIt, lineSize);
                    srcTileIt += rowStride;
                    dstTileIt += rowStride;
                    rowsRemaining--;
                }

            }
        }
    }
    if (pageStoreTarget) {
        // Source pixels/pins are gone. Keep the original manager gate across
        // preparation, root/overlay acceptance and the prepared index install.
        QWriteLocker lock(&m_lock);
        pageStoreBatch.finish(true,
            [&](const auto &changes, QString *failure) {
                return preparedTiles.prepare(*m_hashTable, m_extentManager,
                    m_pageStoreBackend->surface(), changes, failure);
            }, [&](const auto &changes) {
                preparedTiles.installLocked(*m_hashTable, m_extentManager, m_pageStoreBackend, changes);
            });
    } else pageStoreBatch.finish();
}

void KisTiledDataManager::bitBlt(KisTiledDataManager *srcDM, const QRect &rect)
{
    bitBltImpl<false, false>(srcDM, rect);
}

void KisTiledDataManager::bitBltOldData(KisTiledDataManager *srcDM, const QRect &rect)
{
    bitBltImpl<true, false>(srcDM, rect);
}

void KisTiledDataManager::bitBltRough(KisTiledDataManager *srcDM, const QRect &rect)
{
    bitBltImpl<false, true>(srcDM, rect);
}

void KisTiledDataManager::bitBltRoughOldData(KisTiledDataManager *srcDM, const QRect &rect)
{
    bitBltImpl<true, true>(srcDM, rect);
}

void KisTiledDataManager::setExtent(qint32 x, qint32 y, qint32 w, qint32 h)
{
    setExtent(QRect(x, y, w, h));
}

void KisTiledDataManager::setExtent(QRect newRect)
{
    if (m_nativeResult) {
        if (KisStrokeJobFailureContext::currentJobHasFailed()) return;
        QString failure;
        if (!m_nativeResult->trim(newRect.normalized(), &failure)) KisStrokeJobFailureContext::reportFailure(failure);
        return;
    }

    if (KisStrokeJobFailureContext::currentJobHasFailed()) return;
    KisTiledDataManagerPageStoreBackend::OperationDelivery delivery;
    PreparedTileUpdates prepared;
    QWriteLocker locker(&m_lock);
    QRect oldRect = m_extentManager.extent();
    newRect = newRect.normalized();

    // Do nothing if the desired size is bigger than we currently are:
    // that is handled by the autoextending automatically
    if (newRect.contains(oldRect)) return;

    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        QString error;
        const QRect keptPages = newRect.isEmpty() ? QRect{} :
            QRect(QPoint(xToCol(newRect.left()), yToRow(newRect.top())),
                  QPoint(xToCol(newRect.right()), yToRow(newRect.bottom())));
        if (!m_pageStoreBackend->trimToRect(newRect, &error, &delivery,
            [&](const QVector<KisLogicalPageId> &pages, const auto &changes, QString *failure) {
                if (prepared.prepare(*m_hashTable, m_extentManager, pages, newRect, keptPages, changes)) return true;
                if (failure) *failure = QStringLiteral("Trim compatibility preparation was rejected");
                return false;
            },
            [&](const auto &changes) {
                prepared.installReconciled(*m_hashTable, m_extentManager, m_pageStoreBackend, changes);
            })) {
            KisStrokeJobFailureContext::reportFailure(error);
        }
        return;
    }

    KisTileSP tile;
    QRect tileRect;
    {
        KisTileHashTableIterator iter(m_hashTable);

        while (!iter.isDone()) {
            tile = iter.tile();

            tileRect = tile->extent();
            if (newRect.contains(tileRect)) {
                //do nothing
                iter.next();
            } else if (newRect.intersects(tileRect)) {
                QRect intersection = newRect & tileRect;
                intersection.translate(- tileRect.topLeft());

                const qint32 pixelSize = this->pixelSize();

                if (!tile->lockForWrite()) return;
                quint8* data = tile->data();
                if (!data) {
                    tile->unlockForWrite();
                    return;
                }
                quint8* ptr;

                /* FIXME: make it faster */
                for (int y = 0; y < KisTileData::HEIGHT; y++) {
                    for (int x = 0; x < KisTileData::WIDTH; x++) {
                        if (!intersection.contains(x, y)) {
                            ptr = data + pixelSize * (y * KisTileData::WIDTH + x);
                            memcpy(ptr, m_defaultPixel, pixelSize);
                        }
                    }
                }
                tile->unlockForWrite();
                iter.next();
            } else {
                m_extentManager.notifyTileRemoved(tile->col(), tile->row());
                iter.deleteCurrent();
            }
        }
    }
}

void KisTiledDataManager::recalculateExtent()
{
    QVector<QPoint> indexes;

    {
        KisTileHashTableConstIterator iter(m_hashTable);
        KisTileSP tile;

        while ((tile = iter.tile())) {
            indexes << QPoint(tile->col(), tile->row());
            iter.next();
        }
    }

    m_extentManager.replaceTileStats(indexes);
}

void KisTiledDataManager::extent(qint32 &x, qint32 &y, qint32 &w, qint32 &h) const
{
    QRect rect = extent();
    rect.getRect(&x, &y, &w, &h);
}

QRect KisTiledDataManager::extent() const
{
    QReadLocker locker(&m_lock);
    return m_extentManager.extent();
}

KisRegion KisTiledDataManager::region() const
{
    QReadLocker locker(&m_lock);
    QVector<QRect> rects;

    KisTileHashTableConstIterator iter(m_hashTable);
    KisTileSP tile;

    while ((tile = iter.tile())) {
        rects << tile->extent();
        iter.next();
    }

    return KisRegion(std::move(rects));
}

void KisTiledDataManager::setPixel(qint32 x, qint32 y, const quint8 * data)
{
    if (KisStrokeJobFailureContext::currentJobHasFailed()) return;
    KisTileDataWrapper tw(this, x, y, KisTileDataWrapper::WRITE);
    if (quint8 *destination = tw.data())
        memcpy(destination, data, pixelSize());
    else
        KisStrokeJobFailureContext::reportFailure(QStringLiteral("Pixel write acquisition failed"));
}

void KisTiledDataManager::writeBytes(const quint8 *data,
                                     qint32 x, qint32 y,
                                     qint32 width, qint32 height,
                                     qint32 dataRowStride)
{
    if (KisStrokeJobFailureContext::currentJobHasFailed()) return;
    using Phase = KisPageStoreDiagnosticPhase;
    KisPageStoreDiagnosticTimer diagnostic(m_pageStoreBackend ? m_pageStoreBackend->store() : nullptr,
                                           Phase::WriteBytesOwnerWait, 1);
    // This is the same completed batch, with no writable capability. Release
    // its admission/storage after the manager gate and compatibility refresh.
    OperationDelivery delivery;
    PreparedTileUpdates preparedTiles;
    QWriteLocker locker(&m_lock);
    diagnostic.next(Phase::WriteBytesBatchBegin, 1);
    if (!data || width <= 0 || height <= 0) return;
    // Validate before QRect construction, index traversal or any pixel work.
    // Keep the legacy void API, but never replay a failed native operation.
    const qint64 tightRow = qint64(width) * pixelSize();
    const qint64 sourceStride = dataRowStride > 0 ? dataRowStride : tightRow;
    const bool validLayout = qint64(x) + width - 1 <= std::numeric_limits<qint32>::max() &&
        qint64(y) + height - 1 <= std::numeric_limits<qint32>::max() && sourceStride >= tightRow &&
        quint64(sourceStride) <= quint64(std::numeric_limits<qsizetype>::max()) / quint64(height);
    if (!validLayout) {
        KisStrokeJobFailureContext::reportFailure(QStringLiteral("Invalid packed write layout"));
        return;
    }
    const QRect rect(x, y, width, height);
    if (!KisTileHashTable::supportsCoordinates(xToCol(rect.left()), yToRow(rect.top())) ||
        !KisTileHashTable::supportsCoordinates(xToCol(rect.right()), yToRow(rect.bottom()))) {
        KisStrokeJobFailureContext::reportFailure(QStringLiteral("Packed write exceeds compatibility index coordinates"));
        return;
    }
    if (m_nativeResult) {
        locker.unlock(); QString failure;
        if (!m_nativeResult->write(data, rect, sourceStride, &failure)) KisStrokeJobFailureContext::reportFailure(failure);
        return;
    }
    if (m_pageStoreBackend) {
        QString error;
        const bool legacyIntent = copyNeedsLiveLegacySource(rect, false);
        // Match the operation-private painter route: range reservation and
        // canonical claims protect execution, not the derived tiles3 index.
        // Never wait for a native target or execute pixels while excluding
        // readers of the still-visible before-image with the DataManager lock.
        locker.unlock();
        diagnostic.next(Phase::WriteBytesBody, 1);
        const auto result = m_pageStoreBackend->writeBytes(data, x, y, width, height, dataRowStride,
            legacyIntent, nullptr, &error, &delivery, [&](const auto &admitted, QString *failure) {
                // Original claims prevent abort/restore from replacing this
                // capacity between preparation and the post-pixel delta.
                if (!prepareExtent(m_extentManager,
                    QRect(QPoint(xToCol(rect.left()), yToRow(rect.top())),
                          QPoint(xToCol(rect.right()), yToRow(rect.bottom()))), failure)) return false;
                QReadLocker lock(&m_lock);
                return preparedTiles.prepare(*m_hashTable, admitted, failure);
            }, [&](const auto &publish, const auto &changes, QString *failure) {
                locker.relock();
                return preparedTiles.completeLocked(locker, *m_hashTable, m_extentManager,
                    m_pageStoreBackend, publish, changes, failure);
            });
        if (result == KisPageStoreWriteOperationResult::Succeeded) {
            diagnostic.next(Phase::WriteBytesBatchFinish, 1);
            return;
        }
        if (result != KisPageStoreWriteOperationResult::Unavailable &&
            result != KisPageStoreWriteOperationResult::Borrowed) {
            qWarning() << "PageStore packed write failed:" << error;
            KisStrokeJobFailureContext::reportFailure(error);
            return;
        }
        // Borrowed/Unavailable are decided before work. Existing live legacy
        // pointers retain their original visibility and release boundary.
        locker.relock();
    }
    {
        ScopedPageStoreWriteBatch pageStoreBatch(m_pageStoreBackend);
        if (!pageStoreBatch.isValid()) return;
        diagnostic.next(Phase::WriteBytesBody, 1);
        // Actual bytes reading/writing is done in private header. Child
        // PageStore acquire/resolve/publish timers are inclusive in Body.
        const bool written = writeBytesBody(
            data, x, y, width, height, dataRowStride);
        diagnostic.next(Phase::WriteBytesBatchFinish, 1);
        pageStoreBatch.finish(written);
    }
}

void KisTiledDataManager::readBytes(quint8 *data,
                                    qint32 x, qint32 y,
                                    qint32 width, qint32 height,
                                    qint32 dataRowStride) const
{
    if (m_nativeResult) {
        if (width <= 0 || height <= 0) return;
        QString failure;
        if (!m_nativeResult->readCurrent(data, QRect(x, y, width, height),
            dataRowStride > 0 ? dataRowStride : qint64(width) * m_pixelSize, &failure))
            KisStrokeJobFailureContext::reportFailure(failure);
        return;
    }

    QReadLocker locker(&m_lock);
    if (m_pageStoreBackend) {
        QString error;
        if (m_pageStoreBackend->hasCurrentThreadIteratorWrites()) {
            if (!readBytesBody(data, x, y, width, height, dataRowStride))
                KisStrokeJobFailureContext::reportFailure(QStringLiteral("Packed read failed"));
            return;
        }
        const bool read = m_pageStoreBackend->readBytes(data, x, y, width, height, dataRowStride, &error);
        if (!read) {
            qWarning() << "PageStore packed read failed:" << error;
            KisStrokeJobFailureContext::reportFailure(error);
        }
        return;
    }
    // Actual bytes reading/writing is done in private header
    if (!readBytesBody(data, x, y, width, height, dataRowStride))
        KisStrokeJobFailureContext::reportFailure(QStringLiteral("Packed read failed"));
}

std::shared_ptr<const KisPageStoreIteratorReadScope>
KisTiledDataManager::capturePageStoreReadScope(
    bool writable, std::shared_ptr<const KisPageStoreIteratorReadScope> existing) const
{
    if (m_nativeResult) return m_nativeResult->captureIteratorScope(writable, std::move(existing));
    return m_pageStoreBackend ? m_pageStoreBackend->captureIteratorReadScope(writable, nullptr, std::move(existing))
                              : std::shared_ptr<const KisPageStoreIteratorReadScope>{};
}

std::unique_ptr<KisTiledDataManagerIteratorWriteScope>
KisTiledDataManager::beginIteratorWriteScope()
{
    if (m_nativeResult) {
        QString failure; auto scope = m_nativeResult->beginIterator(&failure);
        if (!scope) KisStrokeJobFailureContext::reportFailure(failure);
        return scope;
    }
    return m_pageStoreBackend
        ? m_pageStoreBackend->beginIteratorMutationScope()
        : std::unique_ptr<KisTiledDataManagerIteratorWriteScope>{};
}

bool KisTiledDataManager::beginStrokeMutation(const KisMementoSP &historyOwner, QString *error)
{
    if (m_nativeResult) return m_nativeResult->beginHistoryMutation(historyOwner, error);
    QWriteLocker lock(&m_lock);
    if (!m_pageStoreBackend || !m_pageStoreBackend->isOperational()) {
        if (error) *error = QStringLiteral("Stroke mutation requires PageStore");
        return false;
    }
    return m_pageStoreBackend->beginHistoryMutation(historyOwner, error);
}

bool KisTiledDataManager::applyStrokePixelOperation(
    const KisMementoSP &historyOwner, const QVector<QRect> &targetRects,
    const std::function<bool(KisPixelWriteCursor *)> &operation, QString *error)
{
    if (!historyOwner) {
        if (error) *error = QStringLiteral("Stroke mutation requires its history owner");
        return false;
    }
    return writePageStoreOperation(targetRects, operation, error, historyOwner)
        == PixelOperationResult::Succeeded;
}

bool KisTiledDataManager::checkpointStrokeMutation(
    const KisMementoSP &historyOwner, QString *error)
{
    if (m_nativeResult) return m_nativeResult->checkpointHistoryMutation(historyOwner, error);
    PreparedTileUpdates prepared;
    QWriteLocker lock(&m_lock);
    if (!m_pageStoreBackend || !m_pageStoreBackend->isOperational()) {
        if (error) *error = QStringLiteral("Stroke checkpoint requires PageStore");
        return false;
    }
    return m_pageStoreBackend->checkpointHistoryMutation(historyOwner, error,
        [&](const auto &changes, QString *failure) {
            return prepared.prepare(*m_hashTable, m_extentManager, m_pageStoreBackend->surface(), changes, failure);
        }, [&](const auto &changes) {
            prepared.installLocked(*m_hashTable, m_extentManager, m_pageStoreBackend, changes);
        });
}

KisPageStoreWriteOperationResult KisTiledDataManager::writePageStoreOperation(
    const QVector<QRect> &targetRects, const KisPageStorePixelOperation &operation, QString *error,
    const KisMementoSP &historyOwner)
{
    using Result = PixelOperationResult;
    if (m_nativeResult) return m_nativeResult->pixelOperation(targetRects, operation, historyOwner, error)
        ? Result::Succeeded : Result::Failed;
    if (!m_pageStoreBackend || !m_pageStoreBackend->isOperational()) return Result::Unavailable;
    // Geometry is derived from the caller's declarations, not a second writer
    // registry. Union page rectangles before touching compatibility tiles so
    // repeated/overlapping dabs do not repeat those lookups or claim empty gaps.
    QRegion pageCoverage;
    for (const QRect &rect : targetRects) {
        if (rect.isEmpty()) continue;
        const QPoint first(xToCol(rect.left()), yToRow(rect.top()));
        const QPoint last(xToCol(rect.right()), yToRow(rect.bottom()));
        if (!KisTileHashTable::supportsCoordinates(first.x(), first.y()) ||
            !KisTileHashTable::supportsCoordinates(last.x(), last.y())) {
            if (error) *error = QStringLiteral("pixel operation exceeds compatibility index coordinates");
            return Result::Failed;
        }
        pageCoverage += QRect(first, last);
    }
    QSet<KisLogicalPageId> targets;
    bool legacyIntent = false;
    {
        QReadLocker lock(&m_lock);
        for (const QRect &pages : pageCoverage) {
            for (qint64 row = pages.top(); row <= pages.bottom(); ++row)
                for (qint64 col = pages.left(); col <= pages.right(); ++col) {
                    auto tile = m_hashTable->getExistingTile(qint32(col), qint32(row));
                    legacyIntent |= tile && tile->hasPageStoreWriteIntent();
                    targets.insert({qint32(col), qint32(row)});
                }
        }
    }
    OperationDelivery delivery;
    PreparedTileUpdates preparedTiles;
    const auto result = m_pageStoreBackend->writeOperation(targets.values(), legacyIntent, operation, nullptr, error,
        historyOwner, &delivery, [&](const auto &admitted, QString *failure) {
            if (!prepareExtent(m_extentManager, pageCoverage.boundingRect(), failure)) return false;
            QReadLocker lock(&m_lock);
            return preparedTiles.prepare(*m_hashTable, admitted, failure);
        }, [&](const auto &publish, const auto &changes, QString *failure) {
            QWriteLocker lock(&m_lock);
            return preparedTiles.completeLocked(lock, *m_hashTable, m_extentManager,
                m_pageStoreBackend, publish, changes, failure);
        });
    return result;
}

QVector<quint8*>
KisTiledDataManager::readPlanarBytes(QVector<qint32> channelSizes,
                                     qint32 x, qint32 y,
                                     qint32 width, qint32 height) const
{
    QReadLocker locker(&m_lock);
    // Actual bytes reading/writing is done in private header
    return readPlanarBytesBody(channelSizes, x, y, width, height);
}


void KisTiledDataManager::writePlanarBytes(QVector<quint8*> planes,
                                           QVector<qint32> channelSizes,
                                           qint32 x, qint32 y,
                                           qint32 width, qint32 height)
{
    if (KisStrokeJobFailureContext::currentJobHasFailed()) return;
    // Empty/missing input is a known no-op, not a request for writable pixels.
    if (width <= 0 || height <= 0) return;
    KIS_SAFE_ASSERT_RECOVER_RETURN(!planes.isEmpty() && planes.size() == channelSizes.size());
    qint64 channelBytes = 0;
    bool anyChannelPresent = false;
    for (int i = 0; i < channelSizes.size(); ++i) {
        const int size = channelSizes[i];
        KIS_SAFE_ASSERT_RECOVER_RETURN(size >= 0);
        channelBytes += size;
        KIS_SAFE_ASSERT_RECOVER_RETURN(quint64(width) * quint64(height) <=
            quint64(std::numeric_limits<qsizetype>::max()) / qMax(1, size));
        anyChannelPresent |= size > 0 && planes[i];
    }
    KIS_SAFE_ASSERT_RECOVER_RETURN(channelBytes <= pixelSize());
    KIS_SAFE_ASSERT_RECOVER_RETURN(qint64(x) + width - 1 <= std::numeric_limits<qint32>::max() &&
                                   qint64(y) + height - 1 <= std::numeric_limits<qint32>::max());
    if (!anyChannelPresent) return;
    if (!KisTileHashTable::supportsCoordinates(xToCol(x), yToRow(y)) ||
        !KisTileHashTable::supportsCoordinates(xToCol(qint32(qint64(x) + width - 1)),
                                               yToRow(qint32(qint64(y) + height - 1)))) {
        qWarning() << "Planar write exceeds the compatibility tile index coordinate range";
        KisStrokeJobFailureContext::reportFailure(QStringLiteral("Planar write exceeds compatibility index coordinates"));
        return;
    }
    if (m_nativeResult) {
        QString failure;
        const bool ok = m_nativeResult->mutate(QRect(x, y, width, height),
            [&](qint32 col, qint32 row, quint8 *data, quint32 stride, const QRect &local) {
                int channelOffset = 0;
                for (int c = 0; c < channelSizes.size(); ++c) {
                    const int bytes = channelSizes[c];
                    if (planes[c] && bytes) for (int dy = local.top(); dy <= local.bottom(); ++dy)
                        for (int dx = local.left(); dx <= local.right(); ++dx) {
                            const qint64 source = (qint64(row * 64 + dy - y) * width + col * 64 + dx - x) * bytes;
                            std::memcpy(data + dy * stride + dx * m_pixelSize + channelOffset, planes[c] + source, bytes);
                        }
                    channelOffset += bytes;
                }
                return true;
            }, false, {}, &failure);
        if (!ok) KisStrokeJobFailureContext::reportFailure(failure);
        return;
    }
    QWriteLocker locker(&m_lock);
    ScopedPageStoreWriteBatch pageStoreBatch(m_pageStoreBackend);
    if (!pageStoreBatch.isValid()) return;
    // Actual bytes reading/writing is done in private header

    bool allChannelsPresent = true;

    Q_FOREACH (const quint8* plane, planes) {
        if (!plane) {
            allChannelsPresent = false;
            break;
        }
    }

    const bool written = allChannelsPresent
        ? writePlanarBytesBody<true>(planes, channelSizes, x, y, width, height)
        : writePlanarBytesBody<false>(planes, channelSizes, x, y, width, height);
    if (!pageStoreBatch.finish(written))
        qWarning() << "PageStore planar write failed";
}

qint32 KisTiledDataManager::numContiguousColumns(qint32 x, qint32 minY, qint32 maxY) const
{
    qint32 numColumns;

    Q_UNUSED(minY);
    Q_UNUSED(maxY);

    if (x >= 0) {
        numColumns = KisTileData::WIDTH - (x % KisTileData::WIDTH);
    } else {
        numColumns = ((-qint64(x) - 1) % KisTileData::WIDTH) + 1;
    }

    return numColumns;
}

qint32 KisTiledDataManager::numContiguousRows(qint32 y, qint32 minX, qint32 maxX) const
{
    qint32 numRows;

    Q_UNUSED(minX);
    Q_UNUSED(maxX);

    if (y >= 0) {
        numRows = KisTileData::HEIGHT - (y % KisTileData::HEIGHT);
    } else {
        numRows = ((-qint64(y) - 1) % KisTileData::HEIGHT) + 1;
    }

    return numRows;
}

qint32 KisTiledDataManager::rowStride(qint32 x, qint32 y) const
{
    Q_UNUSED(x);
    Q_UNUSED(y);

    return KisTileData::WIDTH * pixelSize();
}

void KisTiledDataManager::releaseInternalPools()
{
    KisTileData::releaseInternalPools();
}

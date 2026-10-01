/*
 *  SPDX-FileCopyrightText: 2004 C. Boemann <cbo@boemann.dk>
 *  SPDX-FileCopyrightText: 2009 Dmitry Kazakov <dimula73@gmail.com>
 *  SPDX-FileCopyrightText: 2010 Cyrille Berger <cberger@cberger.net>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QRect>
#include <QRegion>
#include <QScopeGuard>
#include <QVector>
#include <QVarLengthArray>

#include <utility>
#include <algorithm>
#include <array>
#include <new>

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
#include "pagestore/KisPageStoreIteratorReadScope_p.h"

namespace {

using PixelOperationResult = KisPageStoreWriteOperationResult;
using OperationDelivery = KisTiledDataManagerPageStoreBackend::OperationDelivery;

// A result buffer, never a presence cache or page permission. Common brush
// jobs use inline storage; larger declarations allocate once before pixels.
using PagePresenceScratch = QVarLengthArray<quint8, 64>;

// Capacity preparation does not publish tiles or change visible bounds.
bool prepareExtent(KisTiledExtentManager &extent, const QRect &tileRange, QString *error)
{
    if (extent.prepareTileRange(tileRange)) return true;
    if (error) *error = QStringLiteral("Extent storage preparation was rejected");
    return false;
}

// Operation-local storage only for absent wrappers. Warm declared pages do
// not allocate a key record, retain another tile, or grow this collection.
// The caller owns the original page admission through delivery and destroys
// this storage outside the manager/install gates, before releasing admission.
class PreparedTileUpdates
{
public:
    template<class Coverage>
    bool prepare(KisTileHashTable &table, const Coverage &coverage, QString *error)
    {
        try {
            for (const QRect &pages : coverage) {
                for (qint64 row = pages.top(); row <= pages.bottom(); ++row) {
                    for (qint64 col = pages.left(); col <= pages.right(); ++col) {
                        auto candidate = table.prepareMissingTile(qint32(col), qint32(row));
                        if (!candidate) continue;
                        const auto size = m_missing.size();
                        if (size == m_missing.capacity()) {
                            if (size > std::numeric_limits<qsizetype>::max() / 2) throw std::bad_alloc();
                            m_missing.reserve(size * 2);
                        }
                        if (m_missing.capacity() <= size) throw std::bad_alloc();
                        m_missing.append(std::move(candidate));
                    }
                }
            }
            std::sort(m_missing.begin(), m_missing.end(), [](const auto &a, const auto &b) {
                return std::make_pair(a.row(), a.col()) < std::make_pair(b.row(), b.col());
            });
            return true;
        } catch (const std::bad_alloc &) {
            if (error) *error = QStringLiteral("Compatibility tile storage preparation was rejected");
            return false;
        }
    }

    bool install(KisTileHashTable &table, KisTiledExtentManager &extent,
                 KisTilePageStoreBridge *bridge, const QVector<KisLogicalPageId> &changed,
                 QString *error)
    {
        // Both managed pixel operations report actual writes, never removals.
        // Use this operation's changed set while its original admission/borrow
        // is held, rather than re-querying a newer global presence selection.
        for (const auto &page : changed) {
            auto tile = table.getExistingTileForPreparedUpdate(page.column, page.row);
            bool created = false;
            if (!tile) {
                const auto key = std::make_pair(page.row, page.column);
                const auto it = std::lower_bound(m_missing.begin(), m_missing.end(), key,
                    [](const auto &candidate, const auto &key) {
                        return std::make_pair(candidate.row(), candidate.col()) < key;
                    });
                if (it != m_missing.end() && it->row() == page.row && it->col() == page.column)
                    tile = table.installPreparedTile(*it, created);
            }
            if (!tile) {
                if (error) *error = QStringLiteral("Prepared compatibility tile is unavailable");
                return false;
            }
            tile->setPageStoreBridge(bridge, false);
            tile->invalidatePageStoreReadCache();
            if (created) extent.notifyTileAdded(page.column, page.row);
        }
        return true;
    }

private:
    QVarLengthArray<KisTileHashTable::PreparedTile, 8> m_missing;
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
    bool finish(bool succeeded = true)
    {
        if (m_finished) return m_succeeded;
        m_finished = true;
        if (!succeeded) {
            if (m_batch) m_batch->cancel();
        } else {
            m_succeeded = !m_backend || (m_batch && m_batch->finish(&m_error));
        }
        // Successful rollback is not successful drawing. In particular, the
        // compatibility cache still needs invalidating after finish(false).
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

    auto backend = std::make_unique<KisTiledDataManagerPageStoreBackend>();
    QString pageStoreError;
    if (!backend->configure(pixelSize, defaultPixel, &pageStoreError)) {
        backend.reset();
    }
    mementoManager->setPageStoreBridge(backend.get());
    m_pixelSize = pixelSize;
    m_defaultPixel = pixel.release();
    m_pageStoreBackend = backend.release();
    m_hashTable = hashTable.release();
    m_mementoManager = mementoManager.release();
}

KisTiledDataManager::KisTiledDataManager(const KisTiledDataManager &dm)
    : KisShared()
{
    QReadLocker locker(&dm.m_lock);
    // Cloning shares the admitted default backing; it needs no new payload.
    PreparedDefaultTile defaultTile(dm.m_hashTable->refAndFetchDefaultTileData());
    auto mementoManager = std::make_unique<KisMementoManager>();
    mementoManager->setDefaultTileData(defaultTile.get());
    auto hashTable = std::make_unique<KisTileHashTable>(*dm.m_hashTable, mementoManager.get());
    auto pixel = std::make_unique<quint8[]>(dm.m_pixelSize);
    memcpy(pixel.get(), dm.m_defaultPixel, dm.m_pixelSize);

    auto backend = std::make_unique<KisTiledDataManagerPageStoreBackend>();
    QString pageStoreError;
    if (!dm.m_pageStoreBackend ||
        !dm.m_pageStoreBackend->isOperational() ||
        !backend->configureClone(*dm.m_pageStoreBackend, &pageStoreError)) {
        backend = std::make_unique<KisTiledDataManagerPageStoreBackend>();
        if (!backend->configure(dm.m_pixelSize, pixel.get(), &pageStoreError)) {
            backend.reset();
        }
    }
    mementoManager->setPageStoreBridge(backend.get());
    m_pixelSize = dm.m_pixelSize;
    m_defaultPixel = pixel.release();
    m_pageStoreBackend = backend.release();
    m_hashTable = hashTable.release();
    m_mementoManager = mementoManager.release();
    recalculateExtent();
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
    if (!writable) {
        bool present = false;
        return getReadOnlyTileLazy(col, row, present);
    }
    bool newTile = false;
    KisTileSP tile = m_hashTable->getTileLazy(col, row, newTile);
    if (newTile) {
        m_extentManager.notifyTileAdded(col, row);
    }
    // A writable miss remains an intent until mutable pixels are exposed.
    attachPageStoreTile(tile, false, newTile);
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
    if (!m_pageStoreBackend || !m_pageStoreBackend->isOperational() ||
        !m_pageStoreBackend->hasCurrentHistory()) {
        KisTileSP tile = m_mementoManager->getCommittedTile(
            col, row, existingTile);
        return tile ? tile : getReadOnlyTileLazy(col, row, existingTile);
    }
    KisTileSP current =
        m_hashTable->getReadOnlyTileLazy(col, row, existingTile);
    existingTile = m_pageStoreBackend->pageAllocated(col, row, true);
    KisTileSP oldTile = new KisTile(*current, col, row, nullptr);
    attachPageStoreTile(oldTile, true, false);
    return oldTile;
}

void KisTiledDataManager::attachPageStoreTile(
    KisTileSP &tile, bool oldData, bool nativeReady)
{
    if (!tile || !m_pageStoreBackend || !m_pageStoreBackend->isOperational()) return;
    tile->setPageStoreBridge(m_pageStoreBackend, oldData);
    if (nativeReady) tile->setPageStoreNativeReadReady();
    else KIS_SAFE_ASSERT_RECOVER_NOOP(tile->refreshPageStoreData());
}

KisMementoSP KisTiledDataManager::getMemento()
{
    QWriteLocker locker(&m_lock);
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
    QWriteLocker locker(&m_lock);
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        QVector<KisLogicalPageId> changed;
        const bool committed = m_pageStoreBackend->commitHistory(m_defaultPixel, m_pixelSize, error, &changed);
        // A persistent session may already have sealed before root preparation
        // rejects. Invalidate the original history delta in either case.
        refreshPageStorePages(changed);
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

bool KisTiledDataManager::tryAbort(KisMementoSP memento, QString *error)
{
    if (error) error->clear();
    if (!memento) {
        if (error) *error = QStringLiteral("Cannot abort an invalid memento");
        return false;
    }

    // Declare candidates before the lock so rejected/old tables are destroyed
    // after unlocking. No canonical state changes during compatibility preparation.
    std::unique_ptr<KisTileHashTable> preparedTable;
    std::unique_ptr<KisTiledExtentManager> preparedExtent;
    QWriteLocker locker(&m_lock);
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        if (!m_historyDefaultTileData) {
            if (error) *error = QStringLiteral("Abort has no retained default backing");
            return false;
        }
        try {
            QVector<KisLogicalPageId> pages;
            if (!m_pageStoreBackend->prepareHistoryAbort(memento, &pages, error)) return false;
            preparedTable = std::make_unique<KisTileHashTable>(m_mementoManager);
            preparedTable->setDefaultTileData(m_historyDefaultTileData);
            preparedExtent = std::make_unique<KisTiledExtentManager>();
            for (const auto &page : pages) {
                bool added = false;
                auto tile = preparedTable->getTileLazy(page.column, page.row, added);
                tile->setPageStoreBridge(m_pageStoreBackend, false);
                if (added) preparedExtent->notifyTileAdded(page.column, page.row);
            }
        } catch (const std::bad_alloc &) {
            if (error) *error = QStringLiteral("Abort compatibility preparation ran out of memory");
            return false;
        }
        if (!m_pageStoreBackend->abortHistory(memento, error)) return false;

        // All adapter storage and the default backing exist before abort.
        auto *oldTable = m_hashTable;
        m_hashTable = preparedTable.release();
        preparedTable.reset(oldTable);
        m_extentManager.takePrepared(*preparedExtent);
        installDefaultPixel(memento->oldDefaultPixel(), m_historyDefaultTileData);
        releaseHistoryDefaultTile();
        return true;
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
    if (!memento) return;
    if (!tryCommit()) return;
    QWriteLocker locker(&m_lock);
    const quint8 *pixel = before ? memento->oldDefaultPixel() : memento->newDefaultPixel();
    const bool defaultChanged = pixel && std::memcmp(m_defaultPixel, pixel, m_pixelSize);
    auto defaultTile = defaultChanged ? prepareDefaultTile(m_pixelSize, pixel) : PreparedDefaultTile{};
    // Prepare every fallible default resource before moving either history.
    if (defaultChanged && !defaultTile) return;

    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        const bool restored = before ? m_pageStoreBackend->rollback(memento)
                                     : m_pageStoreBackend->rollforward(memento);
        if (!restored) return;
        if (defaultChanged) installDefaultPixel(pixel, defaultTile.get());
        rebuildPageStoreIndex();
    } else {
        if (before) m_mementoManager->rollback(m_hashTable, memento);
        else m_mementoManager->rollforward(m_hashTable, memento);
        if (defaultChanged) installDefaultPixel(pixel, defaultTile.get());
        recalculateExtent();
    }
}

bool KisTiledDataManager::hasCurrentMemento() const
{
    return m_pageStoreBackend && m_pageStoreBackend->isOperational()
        ? m_pageStoreBackend->hasCurrentHistory()
        : m_mementoManager->hasCurrentMemento();
}

void KisTiledDataManager::purgeHistory(KisMementoSP oldestMemento)
{
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
bool KisTiledDataManager::read(QIODevice *stream)
{
    clear();

    QWriteLocker locker(&m_lock);
    const bool pageStoreHistory =
        m_pageStoreBackend && m_pageStoreBackend->isOperational();
    KisMementoSP nothing = pageStoreHistory
        ? m_pageStoreBackend->beginHistory(m_defaultPixel, m_pixelSize)
        : m_mementoManager->getMemento();
    if (!nothing) return false;

    if (!stream) {
        if (pageStoreHistory) m_pageStoreBackend->abortHistory();
        else m_mementoManager->commit();
        return false;
    }

    const qint32 maxLineLength = 79; // Legacy magic
    QByteArray line = stream->readLine(maxLineLength);
    line = line.trimmed();
    if (line.isEmpty()) {
        if (pageStoreHistory) m_pageStoreBackend->abortHistory();
        else m_mementoManager->commit();
        return false;
    }

    quint32 numTiles;
    qint32 tilesVersion = LEGACY_VERSION;

    if (line[0] == 'V') {
        QList<QByteArray> lineItems = line.split(' ');

        QString keyword = lineItems.takeFirst();
        Q_ASSERT(keyword == "VERSION");

        tilesVersion = lineItems.takeFirst().toInt();

        if(!processTilesHeader(stream, numTiles)) {
            if (pageStoreHistory) m_pageStoreBackend->abortHistory();
            else m_mementoManager->commit();
            return false;
        }
    }
    else {
        numTiles = line.toUInt();
    }

    KisAbstractTileCompressorSP compressor =
        KisTileCompressorFactory::create(tilesVersion);

    bool readSuccess = true;
    for (quint32 i = 0; i < numTiles; i++) {
        if (!compressor->readTile(stream, this)) {
            readSuccess = false;
        }
    }

    if (pageStoreHistory) {
        if (!readSuccess ||
            !m_pageStoreBackend->commitHistory(m_defaultPixel, m_pixelSize)) {
            if (m_pageStoreBackend->hasCurrentHistory()) {
                m_pageStoreBackend->abortHistory();
            }
            rebuildPageStoreIndex();
            return false;
        }
    } else {
        m_mementoManager->commit();
    }
    return readSuccess;
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
    QList<KisTileSP> tilesToDelete;
    {
        const qint32 tileDataSize = KisTileData::HEIGHT * KisTileData::WIDTH * pixelSize();
        KisTileData *tileData = m_hashTable->refAndFetchDefaultTileData();
        if (!tileData->blockSwapping()) {
            tileData->deref();
            return;
        }
        const quint8 *defaultData = tileData->data();

        KisTileHashTableConstIterator iter(m_hashTable);
        KisTileSP tile;

        while ((tile = iter.tile())) {
            if (tile->extent().intersects(area)) {
                if (!tile->lockForRead()) {
                    tileData->unblockSwapping();
                    tileData->deref();
                    return;
                }
                if(memcmp(defaultData, tile->data(), tileDataSize) == 0) {
                    tilesToDelete.push_back(tile);
                }
                tile->unlockForRead();
            }
            iter.next();
        }

        tileData->unblockSwapping();
        tileData->deref();
    }
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        QVector<KisLogicalPageId> pages;
        pages.reserve(tilesToDelete.size());
        for (const KisTileSP &tile : std::as_const(tilesToDelete)) {
            pages.append({tile->col(), tile->row()});
        }
        if (!m_pageStoreBackend->removePages(pages)) return;
    }
    Q_FOREACH (KisTileSP tile, tilesToDelete) {
        if (m_hashTable->deleteTile(tile)) {
            m_extentManager.notifyTileRemoved(tile->col(), tile->row());
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
        QVector<KisLogicalPageId> changed;
        QString error;
        PagePresenceScratch present;
        if (!prepareExtent(m_extentManager,
                QRect(QPoint(xToCol(clearRect.left()), yToRow(clearRect.top())),
                      QPoint(xToCol(clearRect.right()), yToRow(clearRect.bottom()))), &error) ||
            !m_pageStoreBackend->preparePagePresence(pageStoreRefreshCapacity(clearRect), &present, &error)) {
            KisStrokeJobFailureContext::reportFailure(error);
            return;
        }
        if (m_pageStoreBackend->fillRect(clearRect,
                QByteArray(reinterpret_cast<const char *>(clearPixel), pixelSize), &error, &changed))
            refreshPageStorePages(changed, present.data(), present.size());
        else
            KisStrokeJobFailureContext::reportFailure(error);
        return;
    }

    ScopedPageStoreWriteBatch pageStoreBatch(m_pageStoreBackend);

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
                         const bool alreadyCleared = current->tileData() == td;
                         current->unlockForRead();
                         if (alreadyCleared) continue;
                     }
                 }
                 if (!pageStoreBatch.replaceFullTile(
                         column, row, td, pixelBytesAreDefault)) {
                     if (td) td->release();
                     delete[] clearPixelData;
                     return;
                 }
                 const bool wasDeleted =
                     m_hashTable->deleteTile(column, row);

                 if (wasDeleted) {
                     m_extentManager.notifyTileRemoved(column, row);
                 }


                 if (!pixelBytesAreDefault) {
                     KisTileSP clearedTile = KisTileSP(new KisTile(column, row, td, m_mementoManager));
                     clearedTile->setPageStoreBridge(
                         m_pageStoreBackend, false);
                     clearedTile->setPageStoreNativeReadReady();
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
    pageStoreBatch.finish();
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
    if (KisStrokeJobFailureContext::currentJobHasFailed()) return;
    QWriteLocker locker(&m_lock);
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        QString error;
        if (!m_pageStoreBackend->clearAll(&error)) {
            KisStrokeJobFailureContext::reportFailure(error);
            return;
        }
    }
    m_hashTable->clear();
    m_extentManager.clear();
}

void KisTiledDataManager::rebuildPageStoreIndex()
{
    if (!m_pageStoreBackend || !m_pageStoreBackend->isOperational()) return;
    const QVector<KisLogicalPageId> pages =
        m_pageStoreBackend->allocatedPages();
    m_hashTable->clear();
    m_extentManager.clear();
    for (const KisLogicalPageId &page : pages) {
        bool newTile = false;
        KisTileSP tile = m_hashTable->getTileLazy(
            page.column, page.row, newTile);
        tile->setPageStoreBridge(m_pageStoreBackend, false);
        if (newTile) m_extentManager.notifyTileAdded(page.column, page.row);
    }
}

void KisTiledDataManager::refreshPageStorePages(const QVector<KisLogicalPageId> &pages)
{
    if (pages.isEmpty()) return;
    PagePresenceScratch present;
    KIS_SAFE_ASSERT_RECOVER_RETURN(m_pageStoreBackend->preparePagePresence(pages.size(), &present, nullptr));
    refreshPageStorePages(pages, present.data(), present.size());
}

qsizetype KisTiledDataManager::pageStoreRefreshCapacity(const QRect &rect) const
{
    if (rect.isEmpty()) return 0;
    const quint64 columns = quint64(qint64(xToCol(rect.right())) - xToCol(rect.left()) + 1);
    const quint64 rows = quint64(qint64(yToRow(rect.bottom())) - yToRow(rect.top()) + 1);
    if (columns > quint64(std::numeric_limits<qsizetype>::max()) / rows) return -1;
    return qsizetype(columns * rows);
}

void KisTiledDataManager::refreshPageStorePages(
    const QVector<KisLogicalPageId> &pages, quint8 *scratch, qsizetype capacity)
{
    if (pages.isEmpty()) return;
    KisPageStoreDiagnosticTimer phase(m_pageStoreBackend->store(),
        KisPageStoreDiagnosticPhase::MutationIndexRefresh, quint64(pages.size()));
    KIS_SAFE_ASSERT_RECOVER_RETURN(m_pageStoreBackend->resolveCurrentPagePresenceInto(pages, scratch, capacity));
    for (qsizetype i = 0; i < pages.size(); ++i)
        refreshPageStorePage(pages[i], scratch[i]);
}

void KisTiledDataManager::refreshPageStorePage(const KisLogicalPageId &page, bool present)
{
    if (!present) {
        if (m_hashTable->deleteTile(page.column, page.row))
            m_extentManager.notifyTileRemoved(page.column, page.row);
        return;
    }
    bool created = false;
    auto tile = m_hashTable->getTileLazy(page.column, page.row, created);
    tile->setPageStoreBridge(m_pageStoreBackend, false);
    tile->invalidatePageStoreReadCache();
    if (created) m_extentManager.notifyTileAdded(page.column, page.row);
}

bool KisTiledDataManager::refreshPageStorePage(
    const KisCapturedReadView &view, const KisLogicalPageId &page)
{
    const KisPageKey key{m_pageStoreBackend->surface(), page};
    KisPageVersion version;
    if (!view.resolvePageVersion(key, &version)) return false;
    if (version.isDefaultPixel()) {
        if (m_hashTable->deleteTile(page.column, page.row))
            m_extentManager.notifyTileRemoved(page.column, page.row);
        return true;
    }
    KisPageStoreReadPage read(m_pageStoreBackend->store(), view, key);
    auto tile = m_hashTable->getExistingTile(page.column, page.row);
    auto cache = m_pageStoreBackend->readCacheForPage(read, tile ? tile->takeIdleReadCache() : TileLease{});
    if (!cache) return false;
    bool created = false;
    if (!tile) tile = m_hashTable->getTileLazy(page.column, page.row, created);
    tile->setPageStoreBridge(m_pageStoreBackend, false);
    tile->installPageStoreReadCache(std::move(cache));
    if (created) m_extentManager.notifyTileAdded(page.column, page.row);
    return true;
}

void KisTiledDataManager::refreshPageStoreIndex(const QRect &rect)
{
    if (!m_pageStoreBackend || !m_pageStoreBackend->isOperational() ||
        rect.isEmpty()) {
        return;
    }
    const qint32 firstColumn = xToCol(rect.left());
    const qint32 lastColumn = xToCol(rect.right());
    const qint32 firstRow = yToRow(rect.top());
    const qint32 lastRow = yToRow(rect.bottom());
    auto view = m_pageStoreBackend->captureReadView();
    KIS_SAFE_ASSERT_RECOVER_RETURN(view.isValid());
    for (qint64 row = firstRow; row <= lastRow; ++row) {
        for (qint64 column = firstColumn; column <= lastColumn; ++column) {
            const KisLogicalPageId page{qint32(column), qint32(row)};
            // Failure rollback still restores bytes for unmigrated raw consumers.
            KIS_SAFE_ASSERT_RECOVER_RETURN(refreshPageStorePage(view, page));
        }
    }
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

    // Select once before computing bounds, preparing capacity or writing.
    // Working extent may lag a sealed page or include unpublished intent;
    // neither is a reason to clip a fixed source through that separate value.
    const auto sourceView = source->m_pageStoreBackend->captureReadView(oldSource, error);
    const auto targetView = m_pageStoreBackend->captureReadView(false, error);
    KisSurfaceEpochState sourceState, targetState;
    if (!sourceView.resolveSurfaceState(source->m_pageStoreBackend->surface(), &sourceState) ||
        !targetView.resolveSurfaceState(m_pageStoreBackend->surface(), &targetState)) {
        if (error) *error = QStringLiteral("Copy bounds selection is unavailable");
        return PixelOperationResult::Failed;
    }
    // Equal sparse defaults need work only where either fixed view has content.
    // Include the destination: copying default pixels may remove its old pages.
    // Different defaults require the caller's entire requested rectangle.
    if (sourceState.format.defaultPixel == targetState.format.defaultPixel)
        rect &= sourceState.contentExtent.united(targetState.contentExtent);
    if (rect.isEmpty()) return PixelOperationResult::Succeeded;
    if (!KisTileHashTable::supportsCoordinates(xToCol(rect.left()), yToRow(rect.top())) ||
        !KisTileHashTable::supportsCoordinates(xToCol(rect.right()), yToRow(rect.bottom()))) {
        if (error) *error = QStringLiteral("Copy exceeds compatibility index coordinates");
        return PixelOperationResult::Failed;
    }
    QVector<KisLogicalPageId> changed;
    PagePresenceScratch present;
    if (!prepareExtent(m_extentManager,
            QRect(QPoint(xToCol(rect.left()), yToRow(rect.top())),
                  QPoint(xToCol(rect.right()), yToRow(rect.bottom()))), error) ||
        !m_pageStoreBackend->preparePagePresence(pageStoreRefreshCapacity(rect), &present, error))
        return PixelOperationResult::Failed;
    if (!m_pageStoreBackend->copyFrom(*source->m_pageStoreBackend, sourceView, targetView,
                                     rect, rough, error, &changed))
        return PixelOperationResult::Failed;
    refreshPageStorePages(changed, present.data(), present.size());
    return PixelOperationResult::Succeeded;
}

template<bool useOldSrcData>
void KisTiledDataManager::bitBltImpl(KisTiledDataManager *srcDM, const QRect &requestedRect)
{
    if (KisStrokeJobFailureContext::currentJobHasFailed()) return;
    if (requestedRect.isEmpty()) return;
    QRect rect = requestedRect;
    QString error;
    const auto copied = copyPageStore(srcDM, rect, useOldSrcData, false, &error);
    if (copied != PixelOperationResult::Unavailable) {
        if (copied == PixelOperationResult::Failed)
            KisStrokeJobFailureContext::reportFailure(error);
        return;
    }
    if (rect.isEmpty()) return;
    ScopedPageStoreWriteBatch pageStoreBatch(m_pageStoreBackend);
    bool published = false;
    const auto restoreCompatibilityState = qScopeGuard([&] {
        if (published) return;
        pageStoreBatch.finish(false);
        refreshPageStoreIndex(rect);
    });

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

            // this is the only variation in the template
            KisTileSP srcTile = useOldSrcData ?
                srcDM->getOldTile(column, row, srcTileExists) :
                srcDM->getReadOnlyTileLazy(column, row, srcTileExists);
            if (!srcTile) return;

            QRect tileRect(column*KisTileData::WIDTH, row*KisTileData::HEIGHT,
                           KisTileData::WIDTH, KisTileData::HEIGHT);
            QRect cloneTileRect = rect & tileRect;

            if (cloneTileRect == tileRect) {
                 // Clone whole tile
                 if (!srcTile->lockForRead()) return;
                 KisTileData *td = srcTile->tileData();
                 const bool sparseDefault =
                     !srcTileExists && defaultPixelsCoincide;
                 bool dstTileExists = false;
                 KisTileSP dstTile = getReadOnlyTileLazy(
                     column, row, dstTileExists);
                 if (!dstTile) {
                     srcTile->unlockForRead();
                     return;
                 }
                 bool unchanged = sparseDefault && !dstTileExists;
                 if (!sparseDefault && dstTileExists) {
                     if (!dstTile->lockForRead()) {
                         srcTile->unlockForRead();
                         return;
                     }
                     KisTileData *dstData = dstTile->tileData();
                     unchanged = dstData == td ||
                         memcmp(dstData->data(), td->data(),
                                size_t(rowStride) * KisTileData::HEIGHT) == 0;
                     dstTile->unlockForRead();
                 }
                 if (unchanged) {
                     srcTile->unlockForRead();
                     continue;
                 }
                 const bool pageStoreReplaced =
                     pageStoreBatch.replaceFullTile(
                         column, row, td, sparseDefault);
                 if (!pageStoreReplaced) {
                     srcTile->unlockForRead();
                     return;
                 }
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
                 srcTile->unlockForRead();

            } else {
                const qint32 lineSize = cloneTileRect.width() * pixelSize;
                qint32 rowsRemaining = cloneTileRect.height();

                KisTileDataWrapper tw(this,
                                      cloneTileRect.left(),
                                      cloneTileRect.top(),
                                      KisTileDataWrapper::WRITE);
                if (!tw.isValid() || !srcTile->lockForRead()) return;
                // We suppose that the shift in both tiles is the same
                const quint8* srcTileIt = srcTile->data() + tw.offset();
                quint8* dstTileIt = tw.data();
                if (!dstTileIt) {
                    srcTile->unlockForRead();
                    return;
                }

                while (rowsRemaining > 0) {
                    memcpy(dstTileIt, srcTileIt, lineSize);
                    srcTileIt += rowStride;
                    dstTileIt += rowStride;
                    rowsRemaining--;
                }

                srcTile->unlockForRead();
            }
        }
    }
    published = pageStoreBatch.finish();
}

template<bool useOldSrcData>
void KisTiledDataManager::bitBltRoughImpl(KisTiledDataManager *srcDM, const QRect &requestedRect)
{
    if (KisStrokeJobFailureContext::currentJobHasFailed()) return;
    if (requestedRect.isEmpty()) return;
    QRect rect = requestedRect;
    QString error;
    const auto copied = copyPageStore(srcDM, rect, useOldSrcData, true, &error);
    if (copied != PixelOperationResult::Unavailable) {
        if (copied == PixelOperationResult::Failed)
            KisStrokeJobFailureContext::reportFailure(error);
        return;
    }
    if (rect.isEmpty()) return;
    ScopedPageStoreWriteBatch pageStoreBatch(m_pageStoreBackend);
    bool published = false;
    const auto restoreCompatibilityState = qScopeGuard([&] {
        if (published) return;
        pageStoreBatch.finish(false);
        refreshPageStoreIndex(rect);
    });

    const qint32 pixelSize = this->pixelSize();
    const bool defaultPixelsCoincide =
        !memcmp(srcDM->defaultPixel(), m_defaultPixel, pixelSize);

    qint32 firstColumn = xToCol(rect.left());
    qint32 lastColumn = xToCol(rect.right());

    qint32 firstRow = yToRow(rect.top());
    qint32 lastRow = yToRow(rect.bottom());

    for (qint32 row = firstRow; row <= lastRow; ++row) {
        for (qint32 column = firstColumn; column <= lastColumn; ++column) {

            /**
             * We are cloning whole tiles here so let's not be so boring
             * to check any borders :)
             */

            bool srcTileExists = false;

            // this is the only variation in the template
            KisTileSP srcTile = useOldSrcData ?
                srcDM->getOldTile(column, row, srcTileExists) :
                srcDM->getReadOnlyTileLazy(column, row, srcTileExists);
            if (!srcTile) return;

            if (!srcTile->lockForRead()) return;
            KisTileData *td = srcTile->tileData();
            const bool sparseDefault =
                !srcTileExists && defaultPixelsCoincide;
            bool dstTileExists = false;
            KisTileSP dstTile = getReadOnlyTileLazy(
                column, row, dstTileExists);
            if (!dstTile) {
                srcTile->unlockForRead();
                return;
            }
            bool unchanged = sparseDefault && !dstTileExists;
            if (!sparseDefault && dstTileExists) {
                if (!dstTile->lockForRead()) {
                    srcTile->unlockForRead();
                    return;
                }
                KisTileData *dstData = dstTile->tileData();
                unchanged = dstData == td ||
                    memcmp(dstData->data(), td->data(),
                           size_t(KisTileData::WIDTH) *
                               KisTileData::HEIGHT * pixelSize) == 0;
                dstTile->unlockForRead();
            }
            if (unchanged) {
                srcTile->unlockForRead();
                continue;
            }
            const bool pageStoreReplaced =
                pageStoreBatch.replaceFullTile(
                    column, row, td, sparseDefault);
            if (!pageStoreReplaced) {
                srcTile->unlockForRead();
                return;
            }

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
            srcTile->unlockForRead();
        }
    }
    published = pageStoreBatch.finish();
}

void KisTiledDataManager::bitBlt(KisTiledDataManager *srcDM, const QRect &rect)
{
    bitBltImpl<false>(srcDM, rect);
}

void KisTiledDataManager::bitBltOldData(KisTiledDataManager *srcDM, const QRect &rect)
{
    bitBltImpl<true>(srcDM, rect);
}

void KisTiledDataManager::bitBltRough(KisTiledDataManager *srcDM, const QRect &rect)
{
    bitBltRoughImpl<false>(srcDM, rect);
}

void KisTiledDataManager::bitBltRoughOldData(KisTiledDataManager *srcDM, const QRect &rect)
{
    bitBltRoughImpl<true>(srcDM, rect);
}

void KisTiledDataManager::setExtent(qint32 x, qint32 y, qint32 w, qint32 h)
{
    setExtent(QRect(x, y, w, h));
}

void KisTiledDataManager::setExtent(QRect newRect)
{
    if (KisStrokeJobFailureContext::currentJobHasFailed()) return;
    QWriteLocker locker(&m_lock);
    QRect oldRect = m_extentManager.extent();
    newRect = newRect.normalized();

    // Do nothing if the desired size is bigger than we currently are:
    // that is handled by the autoextending automatically
    if (newRect.contains(oldRect)) return;

    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        QString error;
        if (m_pageStoreBackend->trimToRect(newRect, &error)) {
            rebuildPageStoreIndex();
        } else {
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
    if (m_pageStoreBackend) {
        QVector<KisLogicalPageId> changed;
        QString error;
        const bool legacyIntent = copyNeedsLiveLegacySource(rect, false);
        // Match the operation-private painter route: range reservation and
        // canonical claims protect execution, not the derived tiles3 index.
        // Never wait for a native target or execute pixels while excluding
        // readers of the still-visible before-image with the DataManager lock.
        locker.unlock();
        diagnostic.next(Phase::WriteBytesBody, 1);
        const auto result = m_pageStoreBackend->writeBytes(data, x, y, width, height, dataRowStride,
            legacyIntent, &changed, &error, &delivery, [&](QString *failure) {
                // Original claims prevent abort/restore from replacing this
                // capacity between preparation and the post-pixel delta.
                if (!prepareExtent(m_extentManager,
                    QRect(QPoint(xToCol(rect.left()), yToRow(rect.top())),
                          QPoint(xToCol(rect.right()), yToRow(rect.bottom()))), failure)) return false;
                QReadLocker lock(&m_lock);
                return preparedTiles.prepare(*m_hashTable,
                    std::array<QRect, 1>{QRect(QPoint(xToCol(rect.left()), yToRow(rect.top())),
                                             QPoint(xToCol(rect.right()), yToRow(rect.bottom())))}, failure);
            }, [&](const std::function<bool(QString *)> &publish, QString *failure) {
                locker.relock();
                if (!publish(failure)) {
                    locker.unlock();
                    return false;
                }
                diagnostic.next(Phase::MutationAdapterInstall, quint64(changed.size()));
                const bool installed = preparedTiles.install(
                    *m_hashTable, m_extentManager, m_pageStoreBackend, changed, failure);
                locker.unlock();
                if (installed)
                    diagnostic.next(Phase::MutationAdapterInstalled, quint64(changed.size()));
                return installed;
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
        if (!pageStoreBatch.finish(written)) {
            // Unpublished COW was cancelled; do not leave compatibility tiles
            // marked ready with bytes that canonical PageStore never exposed.
            refreshPageStoreIndex(QRect(x, y, width, height));
        }
    }
}

void KisTiledDataManager::readBytes(quint8 *data,
                                    qint32 x, qint32 y,
                                    qint32 width, qint32 height,
                                    qint32 dataRowStride) const
{
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

QSharedPointer<const KisPageStoreIteratorReadScope>
KisTiledDataManager::capturePageStoreReadScope(
    bool writable, QSharedPointer<const KisPageStoreIteratorReadScope> existing) const
{
    return m_pageStoreBackend ? m_pageStoreBackend->captureIteratorReadScope(writable, nullptr, std::move(existing))
                              : QSharedPointer<const KisPageStoreIteratorReadScope>{};
}

std::unique_ptr<KisTiledDataManagerIteratorWriteScope>
KisTiledDataManager::beginIteratorWriteScope()
{
    return m_pageStoreBackend
        ? m_pageStoreBackend->beginIteratorMutationScope()
        : std::unique_ptr<KisTiledDataManagerIteratorWriteScope>{};
}

bool KisTiledDataManager::beginStrokeMutation(const KisMementoSP &historyOwner, QString *error)
{
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
    QWriteLocker lock(&m_lock);
    if (!m_pageStoreBackend || !m_pageStoreBackend->isOperational()) {
        if (error) *error = QStringLiteral("Stroke checkpoint requires PageStore");
        return false;
    }
    QVector<KisLogicalPageId> changed;
    auto view = m_pageStoreBackend->checkpointHistoryMutation(historyOwner, &changed, error);
    if (view.isValid()) {
        for (const auto &page : changed) {
            KisPageVersion version;
            if (!view.resolvePageVersion({m_pageStoreBackend->surface(), page}, &version)) {
                if (error) *error = QStringLiteral("Stroke checkpoint page cannot be resolved");
                return false;
            }
            refreshPageStorePage(page, !version.isDefaultPixel());
        }
    }
    return view.isValid();
}

KisPageStoreWriteOperationResult KisTiledDataManager::writePageStoreOperation(
    const QVector<QRect> &targetRects, const KisPageStorePixelOperation &operation, QString *error,
    const KisMementoSP &historyOwner)
{
    using Result = PixelOperationResult;
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
    QVector<KisLogicalPageId> changed;
    OperationDelivery delivery;
    PreparedTileUpdates preparedTiles;
    const auto result = m_pageStoreBackend->writeOperation(targets.values(), legacyIntent, operation, &changed, error,
        historyOwner, &delivery, [&](QString *failure) {
            if (!prepareExtent(m_extentManager, pageCoverage.boundingRect(), failure)) return false;
            QReadLocker lock(&m_lock);
            return preparedTiles.prepare(*m_hashTable, pageCoverage, failure);
        }, [&](const std::function<bool(QString *)> &publish, QString *failure) {
            QWriteLocker lock(&m_lock);
            if (!publish(failure)) return false;
            using Phase = KisPageStoreDiagnosticPhase;
            KisPageStoreDiagnosticTimer diagnostic(m_pageStoreBackend->store(), Phase::MutationIndexRefresh,
                                                   quint64(changed.size()));
            diagnostic.next(Phase::MutationAdapterInstall, quint64(changed.size()));
            const bool installed = preparedTiles.install(
                *m_hashTable, m_extentManager, m_pageStoreBackend, changed, failure);
            lock.unlock();
            if (installed)
                diagnostic.next(Phase::MutationAdapterInstalled, quint64(changed.size()));
            return installed;
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
    if (!pageStoreBatch.finish(written)) {
        refreshPageStoreIndex(QRect(x, y, width, height));
        qWarning() << "PageStore planar write failed";
    }
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

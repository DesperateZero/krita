/*
 *  SPDX-FileCopyrightText: 2004 C. Boemann <cbo@boemann.dk>
 *  SPDX-FileCopyrightText: 2009 Dmitry Kazakov <dimula73@gmail.com>
 *  SPDX-FileCopyrightText: 2010 Cyrille Berger <cberger@cberger.net>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QRect>
#include <QVector>

#include <utility>

#include "kis_tile.h"
#include "kis_tiled_data_manager.h"
#include "kis_tile_data_wrapper.h"
#include "kis_tiled_data_manager_p.h"
#include "kis_memento_manager.h"
#include "swap/kis_legacy_tile_compressor.h"
#include "swap/kis_tile_compressor_factory.h"

#include "kis_paint_device_writer.h"

#include "kis_global.h"
#include "pagestore/KisTiledDataManagerPageStoreBackend.h"
#include "pagestore/KisPageStoreDiagnostics_p.h"
#include "pagestore/KisPageStoreIteratorReadScope_p.h"

namespace {

class ScopedPageStoreWriteBatch
{
public:
    explicit ScopedPageStoreWriteBatch(
        KisTiledDataManagerPageStoreBackend *backend)
        : m_backend(backend && backend->isOperational() ? backend : nullptr)
    {
        if (!m_backend) return;
        m_batch = m_backend->beginMutationBatch(&m_error);
        KIS_SAFE_ASSERT_RECOVER_NOOP(m_batch);
    }
    bool isValid() const { return !m_backend || bool(m_batch); }
    bool finish(bool succeeded = true)
    {
        return !m_backend ? succeeded : m_batch && (succeeded
            ? m_batch->finish(&m_error) : m_batch->cancel());
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
    KisTiledDataManagerPageStoreBackend *m_backend = nullptr;
    std::unique_ptr<KisTiledDataManagerPageStoreWriteBatch> m_batch;
    QString m_error;
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
    m_pageStoreBackend = nullptr;
    /* See comment in destructor for details */
    m_mementoManager = new KisMementoManager();
    m_hashTable = new KisTileHashTable(m_mementoManager);

    m_pixelSize = pixelSize;
    m_defaultPixel = new quint8[m_pixelSize];
    setDefaultPixel(defaultPixel);

    m_pageStoreBackend = new KisTiledDataManagerPageStoreBackend;
    QString pageStoreError;
    if (!m_pageStoreBackend->configure(pixelSize, defaultPixel,
                                       &pageStoreError)) {
        delete m_pageStoreBackend;
        m_pageStoreBackend = nullptr;
    }
    m_mementoManager->setPageStoreBridge(m_pageStoreBackend);
}

KisTiledDataManager::KisTiledDataManager(const KisTiledDataManager &dm)
    : KisShared()
{
    /* See comment in destructor for details */

    /* We do not clone the history of the device, there is no usecase for it */
    m_mementoManager = new KisMementoManager();

    KisTileData *defaultTileData = dm.m_hashTable->refAndFetchDefaultTileData();
    m_mementoManager->setDefaultTileData(defaultTileData);
    defaultTileData->deref();

    m_hashTable = new KisTileHashTable(*dm.m_hashTable, m_mementoManager);

    m_pixelSize = dm.m_pixelSize;
    m_defaultPixel = new quint8[m_pixelSize];
    /**
     * We won't call setDefaultTileData here, as defaultTileDatas
     * has already been made shared in m_hashTable(dm->m_hashTable)
    */
    memcpy(m_defaultPixel, dm.m_defaultPixel, m_pixelSize);
    m_pageStoreBackend = new KisTiledDataManagerPageStoreBackend;
    QString pageStoreError;
    if (!dm.m_pageStoreBackend ||
        !dm.m_pageStoreBackend->isOperational() ||
        !m_pageStoreBackend->configureClone(
            *dm.m_pageStoreBackend, &pageStoreError)) {
        delete m_pageStoreBackend;
        m_pageStoreBackend = new KisTiledDataManagerPageStoreBackend;
        if (!m_pageStoreBackend->configure(m_pixelSize, m_defaultPixel,
                                           &pageStoreError)) {
            delete m_pageStoreBackend;
            m_pageStoreBackend = nullptr;
        }
    }
    m_mementoManager->setPageStoreBridge(m_pageStoreBackend);
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
    bool newTile = false;
    KisTileSP tile = writable
        ? m_hashTable->getTileLazy(col, row, newTile)
        : m_hashTable->getReadOnlyTileLazy(col, row, newTile);
    if (writable && newTile) {
        m_extentManager.notifyTileAdded(col, row);
    }
    // A sparse miss already has immutable default backing; a writable miss
    // remains an intent until pixels are exposed and needs no replica refresh.
    attachPageStoreTile(tile, false, (!writable && !newTile) || (writable && newTile));
    return tile;
}

KisTileSP KisTiledDataManager::getReadOnlyTileLazy(
    qint32 col, qint32 row, bool &existingTile)
{
    KisTileSP tile = m_hashTable->getReadOnlyTileLazy(col, row, existingTile);
    attachPageStoreTile(tile, false, !existingTile);
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
        return m_pageStoreBackend->beginHistory(m_defaultPixel, m_pixelSize);
    }
    KisMementoSP memento = m_mementoManager->getMemento();
    memento->saveOldDefaultPixel(m_defaultPixel, m_pixelSize);
    return memento;
}

void KisTiledDataManager::commit()
{
    QWriteLocker locker(&m_lock);
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        QString error;
        KIS_SAFE_ASSERT_RECOVER(
            m_pageStoreBackend->commitHistory(
                m_defaultPixel, m_pixelSize, &error)) {
            qWarning() << "PageStore history commit failed:" << error;
        }
        return;
    }
    KisMementoSP memento = m_mementoManager->currentMemento();
    if (memento) memento->saveNewDefaultPixel(m_defaultPixel, m_pixelSize);
    m_mementoManager->commit();
}

void KisTiledDataManager::rollback(KisMementoSP memento)
{
    commit();
    QWriteLocker locker(&m_lock);
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        if (m_pageStoreBackend->rollback(memento)) {
            if (memento->oldDefaultPixel() &&
                std::memcmp(m_defaultPixel, memento->oldDefaultPixel(),
                            m_pixelSize)) {
                setDefaultPixelImpl(memento->oldDefaultPixel());
            }
            rebuildPageStoreIndex();
        }
        return;
    }
    m_mementoManager->rollback(m_hashTable, memento);
    const quint8 *defaultPixel = memento->oldDefaultPixel();
    if (std::memcmp(m_defaultPixel, defaultPixel, m_pixelSize)) {
        setDefaultPixelImpl(defaultPixel);
    }
    recalculateExtent();
}

void KisTiledDataManager::rollforward(KisMementoSP memento)
{
    commit();
    QWriteLocker locker(&m_lock);
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        if (m_pageStoreBackend->rollforward(memento)) {
            if (memento->newDefaultPixel() &&
                std::memcmp(m_defaultPixel, memento->newDefaultPixel(),
                            m_pixelSize)) {
                setDefaultPixelImpl(memento->newDefaultPixel());
            }
            rebuildPageStoreIndex();
        }
        return;
    }
    m_mementoManager->rollforward(m_hashTable, memento);
    const quint8 *defaultPixel = memento->newDefaultPixel();
    if (std::memcmp(m_defaultPixel, defaultPixel, m_pixelSize)) {
        setDefaultPixelImpl(defaultPixel);
    }
    recalculateExtent();
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
    } else {
        m_mementoManager->purgeHistory(oldestMemento);
    }
}

void KisTiledDataManager::setDefaultPixel(const quint8 *defaultPixel)
{
    QWriteLocker locker(&m_lock);
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        const QByteArray pixel(reinterpret_cast<const char *>(defaultPixel),
                               m_pixelSize);
        if (!m_pageStoreBackend->setDefaultPixel(pixel)) return;
    }
    setDefaultPixelImpl(defaultPixel);
}

void KisTiledDataManager::setDefaultPixelImpl(const quint8 *defaultPixel)
{
    KisTileData *td = KisTileDataStore::instance()->createDefaultTileData(pixelSize(), defaultPixel);
    m_hashTable->setDefaultTileData(td);
    m_mementoManager->setDefaultTileData(td);

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
            !KisTileHashTable::supportsCoordinates(xToCol(clearRect.right()), yToRow(clearRect.bottom()))) return;
        QVector<KisLogicalPageId> changed;
        if (m_pageStoreBackend->fillRect(clearRect,
                QByteArray(reinterpret_cast<const char *>(clearPixel), pixelSize), nullptr, &changed))
            refreshPageStorePages(changed);
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
    KIS_SAFE_ASSERT_RECOVER_NOOP(pageStoreBatch.finish());
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
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        if (!m_pageStoreBackend->clearAll()) return;
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
    KisPageStoreDiagnosticTimer phase(m_pageStoreBackend->store(),
        KisPageStoreDiagnosticPhase::MutationIndexRefresh, quint64(pages.size()));
    auto view = m_pageStoreBackend->captureReadView();
    KIS_SAFE_ASSERT_RECOVER_RETURN(view.isValid());
    for (const auto &page : pages)
        KIS_SAFE_ASSERT_RECOVER_RETURN(refreshPageStorePage(view, page));
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
    auto *data = m_pageStoreBackend->tileDataForReadPage(read);
    if (!data) return false;
    bool created = false;
    auto tile = m_hashTable->getTileLazy(page.column, page.row, created);
    tile->setPageStoreBridge(m_pageStoreBackend, false);
    tile->installPageStoreReadCache(data);
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

template<bool useOldSrcData>
void KisTiledDataManager::bitBltImpl(KisTiledDataManager *srcDM, const QRect &rect)
{
    if (rect.isEmpty()) return;
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational() &&
        srcDM->m_pageStoreBackend && srcDM->m_pageStoreBackend->isOperational() &&
        !srcDM->copyNeedsLiveLegacySource(rect, useOldSrcData)) {
        if (!KisTileHashTable::supportsCoordinates(xToCol(rect.left()), yToRow(rect.top())) ||
            !KisTileHashTable::supportsCoordinates(xToCol(rect.right()), yToRow(rect.bottom()))) return;
        QVector<KisLogicalPageId> changed;
        if (m_pageStoreBackend->copyFrom(*srcDM->m_pageStoreBackend, rect, useOldSrcData, false, nullptr, &changed))
            refreshPageStorePages(changed);
        return;
    }
    ScopedPageStoreWriteBatch pageStoreBatch(m_pageStoreBackend);

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
    KIS_SAFE_ASSERT_RECOVER_NOOP(pageStoreBatch.finish());
}

template<bool useOldSrcData>
void KisTiledDataManager::bitBltRoughImpl(KisTiledDataManager *srcDM, const QRect &rect)
{
    if (rect.isEmpty()) return;
    if (m_pageStoreBackend && m_pageStoreBackend->isOperational() &&
        srcDM->m_pageStoreBackend && srcDM->m_pageStoreBackend->isOperational() &&
        !srcDM->copyNeedsLiveLegacySource(rect, useOldSrcData)) {
        if (!KisTileHashTable::supportsCoordinates(xToCol(rect.left()), yToRow(rect.top())) ||
            !KisTileHashTable::supportsCoordinates(xToCol(rect.right()), yToRow(rect.bottom()))) return;
        QVector<KisLogicalPageId> changed;
        if (m_pageStoreBackend->copyFrom(*srcDM->m_pageStoreBackend, rect, useOldSrcData, true, nullptr, &changed))
            refreshPageStorePages(changed);
        return;
    }
    ScopedPageStoreWriteBatch pageStoreBatch(m_pageStoreBackend);

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

            if (!srcTile->lockForRead()) return;
            KisTileData *td = srcTile->tileData();
            const bool sparseDefault =
                !srcTileExists && defaultPixelsCoincide;
            bool dstTileExists = false;
            KisTileSP dstTile = getReadOnlyTileLazy(
                column, row, dstTileExists);
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
    KIS_SAFE_ASSERT_RECOVER_NOOP(pageStoreBatch.finish());
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
    QRect oldRect = extent();
    newRect = newRect.normalized();

    // Do nothing if the desired size is bigger than we currently are:
    // that is handled by the autoextending automatically
    if (newRect.contains(oldRect)) return;

    if (m_pageStoreBackend && m_pageStoreBackend->isOperational()) {
        if (m_pageStoreBackend->trimToRect(newRect)) {
            rebuildPageStoreIndex();
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
    return m_extentManager.extent();
}

KisRegion KisTiledDataManager::region() const
{
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
    KisTileDataWrapper tw(this, x, y, KisTileDataWrapper::WRITE);
    if (quint8 *destination = tw.data())
        memcpy(destination, data, pixelSize());
}

void KisTiledDataManager::writeBytes(const quint8 *data,
                                     qint32 x, qint32 y,
                                     qint32 width, qint32 height,
                                     qint32 dataRowStride)
{
    using Phase = KisPageStoreDiagnosticPhase;
    KisPageStoreDiagnosticTimer diagnostic(m_pageStoreBackend ? m_pageStoreBackend->store() : nullptr,
                                           Phase::WriteBytesOwnerWait, 1);
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
    KIS_SAFE_ASSERT_RECOVER_RETURN(validLayout);
    const QRect rect(x, y, width, height);
    KIS_SAFE_ASSERT_RECOVER_RETURN(
        KisTileHashTable::supportsCoordinates(xToCol(rect.left()), yToRow(rect.top())) &&
        KisTileHashTable::supportsCoordinates(xToCol(rect.right()), yToRow(rect.bottom())));
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
            legacyIntent, &changed, &error);
        if (result == KisPageStoreWriteOperationResult::Succeeded) {
            diagnostic.next(Phase::WriteBytesBatchFinish, 1);
            locker.relock();
            refreshPageStorePages(changed);
            return;
        }
        if (result == KisPageStoreWriteOperationResult::Failed) {
            qWarning() << "PageStore packed write failed:" << error;
            KIS_SAFE_ASSERT_RECOVER_RETURN(false);
        }
        // Borrowed/Unavailable are decided before work. Existing live legacy
        // pointers retain their original visibility and release boundary.
        locker.relock();
    }
    {
        ScopedPageStoreWriteBatch pageStoreBatch(m_pageStoreBackend);
        KIS_SAFE_ASSERT_RECOVER_RETURN(pageStoreBatch.isValid());
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
            KIS_SAFE_ASSERT_RECOVER_NOOP(false);
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
            KIS_SAFE_ASSERT_RECOVER_NOOP(
                readBytesBody(data, x, y, width, height, dataRowStride));
            return;
        }
        const bool read = m_pageStoreBackend->readBytes(data, x, y, width, height, dataRowStride, &error);
        if (!read) qWarning() << "PageStore packed read failed:" << error;
        KIS_SAFE_ASSERT_RECOVER_NOOP(read);
        return;
    }
    // Actual bytes reading/writing is done in private header
    KIS_SAFE_ASSERT_RECOVER_NOOP(
        readBytesBody(data, x, y, width, height, dataRowStride));
}

QSharedPointer<const KisPageStoreIteratorReadScope>
KisTiledDataManager::capturePageStoreReadScope(
    bool writable, QSharedPointer<const KisPageStoreIteratorReadScope> existing) const
{
    return m_pageStoreBackend ? m_pageStoreBackend->captureIteratorReadScope(writable, nullptr, std::move(existing))
                              : QSharedPointer<const KisPageStoreIteratorReadScope>{};
}

Qt::HANDLE KisTiledDataManager::registerPageStoreWriteBoundary(
    const void *key, KisPageStoreWriteBoundary boundary)
{
    return m_pageStoreBackend ? m_pageStoreBackend->registerIteratorWriteBoundary(key, boundary) : nullptr;
}
void KisTiledDataManager::unregisterPageStoreWriteBoundary(Qt::HANDLE thread, const void *key)
{
    if (m_pageStoreBackend && thread) m_pageStoreBackend->unregisterIteratorWriteBoundary(thread, key);
}

KisPageStoreWriteOperationResult KisTiledDataManager::writePageStoreOperation(
    const QVector<QRect> &targetRects, const KisPageStorePixelOperation &operation, QString *error)
{
    using Result = KisPageStoreWriteOperationResult;
    if (!m_pageStoreBackend || !m_pageStoreBackend->isOperational()) return Result::Unavailable;
    QSet<KisLogicalPageId> targets;
    bool legacyIntent = false;
    {
        QReadLocker lock(&m_lock);
        for (const auto &rect : targetRects) {
            if (rect.isEmpty()) continue;
            if (!KisTileHashTable::supportsCoordinates(xToCol(rect.left()), yToRow(rect.top())) ||
                !KisTileHashTable::supportsCoordinates(xToCol(rect.right()), yToRow(rect.bottom()))) {
                if (error) *error = QStringLiteral("pixel operation exceeds compatibility index coordinates");
                return Result::Failed;
            }
            for (qint64 row = yToRow(rect.top()); row <= yToRow(rect.bottom()); ++row)
                for (qint64 col = xToCol(rect.left()); col <= xToCol(rect.right()); ++col) {
                    auto tile = m_hashTable->getExistingTile(qint32(col), qint32(row));
                    legacyIntent |= tile && tile->hasPageStoreWriteIntent();
                    targets.insert({qint32(col), qint32(row)});
                }
        }
    }
    QVector<KisLogicalPageId> changed;
    const auto result = m_pageStoreBackend->writeOperation(targets.values(), legacyIntent, operation, &changed, error);
    if (result == Result::Succeeded) {
        QWriteLocker lock(&m_lock);
        refreshPageStorePages(changed);
    }
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
        return;
    }
    QWriteLocker locker(&m_lock);
    ScopedPageStoreWriteBatch pageStoreBatch(m_pageStoreBackend);
    KIS_SAFE_ASSERT_RECOVER_RETURN(pageStoreBatch.isValid());
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

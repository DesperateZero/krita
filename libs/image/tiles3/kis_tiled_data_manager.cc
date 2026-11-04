/*
 *  SPDX-FileCopyrightText: 2004 C. Boemann <cbo@boemann.dk>
 *  SPDX-FileCopyrightText: 2009 Dmitry Kazakov <dimula73@gmail.com>
 *  SPDX-FileCopyrightText: 2010 Cyrille Berger <cberger@cberger.net>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "pagestore/native/KisNativeResultProtocol.h"
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
#include "kis_tile_data_store.h"
#include "kis_tiled_data_manager.h"
#include "kis_tile_data_wrapper.h"
#include "kis_tiled_data_manager_p.h"
#include "kis_memento_manager.h"
#include "swap/kis_legacy_tile_compressor.h"
#include "swap/kis_tile_compressor_factory.h"

#include "kis_paint_device_writer.h"

#include "kis_global.h"
#include "KisStrokeJobFailureContext.h"
#include "pagestore/KisPageStoreIteratorReadScope_p.h"

namespace {

using PixelOperationResult = KisPageStoreWriteOperationResult;
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
        SelectionAuthority::NativeResult)
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

    m_pixelSize = pixelSize;
    m_defaultPixel = pixel.get();
    m_hashTable = hashTable.get(); m_mementoManager = mementoManager.get();
    if (authority == SelectionAuthority::NativeResult)
        m_nativeResult = KisNativeResultProtocol::createProduct(*this, pixelSize, defaultPixel);
    pixel.release(); hashTable.release(); mementoManager.release();
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
    auto hashTable = nativeSource
        ? std::make_unique<KisTileHashTable>(mementoManager.get())
        : std::make_unique<KisTileHashTable>(*dm.m_hashTable, mementoManager.get());
    if (nativeSource) hashTable->setDefaultTileData(defaultTile.get());
    // Keep every constructor resource owned until extent preparation succeeds.
    m_hashTable = hashTable.get();
    recalculateExtent();
    m_pixelSize = dm.m_pixelSize;
    m_defaultPixel = pixel.get();
    m_hashTable = hashTable.get(); m_mementoManager = mementoManager.get();
    if (nativeSource) {
        m_nativeResult = dm.m_nativeResult->cloneTo(*this);
        if (!m_nativeResult) throw std::bad_alloc();
    }
    pixel.release(); hashTable.release(); mementoManager.release();
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
    delete m_hashTable;
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
    if (tile && m_nativeResult) {
        QString failure; tile = m_nativeResult->selectReadTile(tile, &existingTile, &failure);
        if (!tile) { existingTile = false; KisStrokeJobFailureContext::reportFailure(failure); }
        return tile;
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
    KisTileSP tile = m_mementoManager->getCommittedTile(col, row, existingTile);
    return tile ? tile : getReadOnlyTileLazy(col, row, existingTile);

}

KisMementoSP KisTiledDataManager::getMemento()
{
    QWriteLocker locker(&m_lock);
    return getMementoLocked();
}

KisMementoSP KisTiledDataManager::getMementoLocked()
{
    if (m_nativeResult) return m_nativeResult->beginHistory();
    
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
    QWriteLocker locker(&m_lock);
    
    KisMementoSP memento = m_mementoManager->currentMemento();
    if (memento) memento->saveNewDefaultPixel(m_defaultPixel, m_pixelSize);
    m_mementoManager->commit();
    if (error) error->clear();
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
    QWriteLocker locker(&m_lock);
    

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

    
        if (before) m_mementoManager->rollback(m_hashTable, memento);
        else m_mementoManager->rollforward(m_hashTable, memento);
        if (defaultChanged) installDefaultPixel(pixel, defaultTile.get());
        recalculateExtent();
    
}

bool KisTiledDataManager::hasCurrentMemento() const
{
    return m_nativeResult ? m_nativeResult->hasHistory() : m_mementoManager->hasCurrentMemento();
}

void KisTiledDataManager::purgeHistory(KisMementoSP oldestMemento)
{
    if (m_nativeResult) { m_nativeResult->purgeHistory(oldestMemento); return; }
    QWriteLocker locker(&m_lock);
    
        m_mementoManager->purgeHistory(oldestMemento);
    
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
    auto nativeImport = m_nativeResult ? m_nativeResult->beginImport() : nullptr;
    if (m_nativeResult && !nativeImport) return false;
    if (!m_nativeResult) clear();

    // The original batch owns decoding and cancellation. Its final resources
    // outlive the manager gate; no undo result escapes this file import.
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
        for (quint32 i = 0; i < numTiles; ++i)
            if (!compressor->readTile(stream, this)) return false;
        locker.unlock();
        return nativeImport->finish();
    }
    if (!getMementoLocked()) {
        return false;
    }
    bool readSuccess = true;
    for (quint32 i = 0; i < numTiles; i++) {
        if (!compressor->readTile(stream, this)) readSuccess = false;
    }

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

KisTileData *KisTiledDataManager::acquireUniformTile(const QByteArray &pixel, bool *reused)
{
    QMutexLocker lock(&m_uniformClearMutex);
    const bool hit = m_uniformClearTileData && m_uniformClearPixel == pixel;
    if (reused) *reused = hit;
    if (!hit) {
        auto *tile = KisTileDataStore::instance()->createDefaultTileData(
            m_pixelSize, reinterpret_cast<const quint8 *>(pixel.constData()));
        if (!tile) return nullptr;
        tile->acquire();
        if (m_uniformClearTileData) m_uniformClearTileData->release();
        m_uniformClearTileData = tile;
        m_uniformClearPixel = pixel;
    }
    // The caller owns its source independently of later cache replacement.
    m_uniformClearTileData->acquire();
    return m_uniformClearTileData;
}

void KisTiledDataManager::clear(QRect clearRect, const quint8 *clearPixel)
{
    if (m_nativeResult) {
        if (KisStrokeJobFailureContext::currentJobHasFailed()) return;
        QString failure;
        if (!m_nativeResult->fill(clearRect, clearPixel ? QByteArray(reinterpret_cast<const char *>(clearPixel), m_pixelSize) : QByteArray{}, &failure))
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
        td = acquireUniformTile(clearPixelBytes, &reusesUniformClearBacking);
        if (!td) {
            delete[] clearPixelData;
            return;
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
    QWriteLocker locker(&m_lock);
    
    m_hashTable->clear();
    m_extentManager.clear();
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
            auto *liveSource = useOldSrcData && srcDM->m_nativeResult->hasHistory() ? nullptr : srcDM;
            copied = selected && m_nativeResult->copyFrom(selected, rect, liveSource, &error);
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
    if (rect.isEmpty()) return;

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
                 const bool wasDeleted =
                     m_hashTable->deleteTile(column, row);

                 if (srcTileExists || !defaultPixelsCoincide) {
                     KisTileSP clonedTile = KisTileSP(new KisTile(column, row, td, m_mementoManager));

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
    QWriteLocker locker(&m_lock);
    QRect oldRect = m_extentManager.extent();
    newRect = newRect.normalized();

    // Do nothing if the desired size is bigger than we currently are:
    // that is handled by the autoextending automatically
    if (newRect.contains(oldRect)) return;

    

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
        QString failure;
        if (!m_nativeResult->write(data, rect, sourceStride, &failure)) KisStrokeJobFailureContext::reportFailure(failure);
        return;
    }
    
    QWriteLocker locker(&m_lock);
    if (!writeBytesBody(data, x, y, width, height, dataRowStride))
        KisStrokeJobFailureContext::reportFailure(QStringLiteral("Packed write failed"));

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
    
    // Actual bytes reading/writing is done in private header
    if (!readBytesBody(data, x, y, width, height, dataRowStride))
        KisStrokeJobFailureContext::reportFailure(QStringLiteral("Packed read failed"));
}

std::shared_ptr<const KisPageStoreIteratorReadScope>
KisTiledDataManager::capturePageStoreReadScope(
    bool writable, std::shared_ptr<const KisPageStoreIteratorReadScope> existing) const
{
    return m_nativeResult ? m_nativeResult->captureIteratorScope(writable, std::move(existing))
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
    return {};
}

bool KisTiledDataManager::beginStrokeMutation(const KisMementoSP &historyOwner, QString *error)
{
    if (m_nativeResult) return m_nativeResult->beginHistoryMutation(historyOwner, error);
    if (error) *error = QStringLiteral("Stroke mutation requires a result authority");
    return false;
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
    if (error) *error = QStringLiteral("Stroke checkpoint requires a result authority");
    return false;
}

KisPageStoreWriteOperationResult KisTiledDataManager::writePageStoreOperation(
    const QVector<QRect> &targetRects, const KisPageStorePixelOperation &operation, QString *error,
    const KisMementoSP &historyOwner)
{
    if (!m_nativeResult) return PixelOperationResult::Unavailable;
    return m_nativeResult->pixelOperation(targetRects, operation, historyOwner, error)
        ? PixelOperationResult::Succeeded : PixelOperationResult::Failed;
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
    if (!written)
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

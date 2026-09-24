/*
 *  SPDX-FileCopyrightText: 2010 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_tiled_data_manager_test.h"
#include <simpletest.h>

#include <QRandomGenerator>
#include <thread>
#include <QSemaphore>
#include <QScopeGuard>
#include <QRegularExpression>

#include "tiles3/kis_tiled_data_manager.h"
#include "tiles3/kis_tile_data_store.h"
#include "tiles3/kis_hline_iterator.h"
#include "tiles3/kis_vline_iterator.h"
#include "tiles3/kis_random_accessor.h"
#include "kis_wrapped_hline_iterator.h"
#include "kis_wrapped_vline_iterator.h"
#include "kis_repeat_iterators_pixel.h"
#include "pagestore/KisTiledDataManagerPageStoreBackend.h"
#include "pagestore/KisTiles3PageReplicaProvider.h"
#include "KisPageStoreCpuSurfaceOps.h"
#include "pagestore/KisPageStoreDiagnostics_p.h"

#include "tiles_test_utils.h"
#include "config-limit-long-tests.h"
#include "kis_br1_stress_profile.h"

namespace {

class TestableTiledDataManager : public KisTiledDataManager
{
public:
    using KisTiledDataManager::KisTiledDataManager;
    using KisTiledDataManager::purge;
    using KisTiledDataManager::read;
    using KisTiledDataManager::write;
};

}

bool KisTiledDataManagerTest::checkHole(quint8* buffer,
                                        quint8 holeColor, QRect holeRect,
                                        quint8 backgroundColor, QRect backgroundRect)
{
    for(qint32 y = backgroundRect.y(); y <= backgroundRect.bottom(); y++) {
        for(qint32 x = backgroundRect.x(); x <= backgroundRect.right(); x++) {
            quint8 expectedColor = holeRect.contains(x,y) ? holeColor : backgroundColor;

            if(*buffer != expectedColor) {
                qDebug() << "Expected" << expectedColor << "but found" << *buffer;
                return false;
            }

            buffer++;
        }
    }
    return true;
}

bool KisTiledDataManagerTest::checkTilesShared(KisTiledDataManager *srcDM,
                                               KisTiledDataManager *dstDM,
                                               bool takeOldSrc,
                                               bool takeOldDst,
                                               QRect tilesRect)
{
    for(qint32 row = tilesRect.y(); row <= tilesRect.bottom(); row++) {
        for(qint32 col = tilesRect.x(); col <= tilesRect.right(); col++) {
            KisTileSP srcTile = takeOldSrc ? srcDM->getOldTile(col, row)
                : srcDM->getTile(col, row, false);
            KisTileSP dstTile = takeOldDst ? dstDM->getOldTile(col, row)
                : dstDM->getTile(col, row, false);

            if(srcTile->tileData() != dstTile->tileData()) {
                qDebug() << "Expected tile data (" << col << row << ")"
                         << srcTile->extent()
                         << srcTile->tileData()
                         << "but found" << dstTile->tileData();
                qDebug() << "Expected" << srcTile->data()[0] << "but found" << dstTile->data()[0];
                return false;
            }
        }
    }
    return true;
}

bool KisTiledDataManagerTest::checkTilesNotShared(KisTiledDataManager *srcDM,
                                                  KisTiledDataManager *dstDM,
                                                  bool takeOldSrc,
                                                  bool takeOldDst,
                                                  QRect tilesRect)
{
    for(qint32 row = tilesRect.y(); row <= tilesRect.bottom(); row++) {
        for(qint32 col = tilesRect.x(); col <= tilesRect.right(); col++) {
            KisTileSP srcTile = takeOldSrc ? srcDM->getOldTile(col, row)
                : srcDM->getTile(col, row, false);
            KisTileSP dstTile = takeOldDst ? dstDM->getOldTile(col, row)
                : dstDM->getTile(col, row, false);

            if(srcTile->tileData() == dstTile->tileData()) {
                qDebug() << "Expected tiles not be shared:"<< srcTile->extent();
                return false;
            }
        }
    }
    return true;
}

void KisTiledDataManagerTest::testUndoingNewTiles()
{
    // "growing extent bug"

    const QRect nullRect;

    quint8 defaultPixel = 0;
    KisTiledDataManager srcDM(1, &defaultPixel);

    KisTileSP emptyTile = srcDM.getTile(0, 0, false);

    QCOMPARE(srcDM.extent(), nullRect);

    KisMementoSP memento0 = srcDM.getMemento();
    KisTileSP createdTile = srcDM.getTile(0, 0, true);
    srcDM.commit();

    QCOMPARE(srcDM.extent(), QRect(0,0,64,64));

    srcDM.rollback(memento0);
    QCOMPARE(srcDM.extent(), nullRect);
}

void KisTiledDataManagerTest::testPurgedAndEmptyTransactions()
{
    quint8 defaultPixel = 0;
    KisTiledDataManager srcDM(1, &defaultPixel);

    quint8 oddPixel1 = 128;

    QRect rect(0,0,512,512);
    QRect clearRect1(50,50,100,100);
    QRect clearRect2(150,50,100,100);

    quint8 *buffer = new quint8[rect.width()*rect.height()];

    // purged transaction

    KisMementoSP memento0 = srcDM.getMemento();
    srcDM.clear(clearRect1, &oddPixel1);
    srcDM.purgeHistory(memento0);
    memento0 = 0;

    srcDM.readBytes(buffer, rect.x(), rect.y(), rect.width(), rect.height());
    QVERIFY(checkHole(buffer, oddPixel1, clearRect1,
                      defaultPixel, rect));

    // one more purged transaction

    KisMementoSP memento1 = srcDM.getMemento();
    srcDM.clear(clearRect2, &oddPixel1);

    srcDM.readBytes(buffer, rect.x(), rect.y(), rect.width(), rect.height());
    QVERIFY(checkHole(buffer, oddPixel1, clearRect1 | clearRect2,
                      defaultPixel, rect));

    srcDM.purgeHistory(memento1);
    memento1 = 0;

    srcDM.readBytes(buffer, rect.x(), rect.y(), rect.width(), rect.height());
    QVERIFY(checkHole(buffer, oddPixel1, clearRect1 | clearRect2,
                      defaultPixel, rect));

    // empty one

    KisMementoSP memento2 = srcDM.getMemento();
    srcDM.commit();
    srcDM.rollback(memento2);

    srcDM.readBytes(buffer, rect.x(), rect.y(), rect.width(), rect.height());
    QVERIFY(checkHole(buffer, oddPixel1, clearRect1 | clearRect2,
                      defaultPixel, rect));


    // now check that everything works still

    KisMementoSP memento3 = srcDM.getMemento();
    srcDM.setExtent(clearRect2);
    srcDM.commit();

    srcDM.readBytes(buffer, rect.x(), rect.y(), rect.width(), rect.height());
    QVERIFY(checkHole(buffer, oddPixel1, clearRect2,
                      defaultPixel, rect));

    srcDM.rollback(memento3);

    srcDM.readBytes(buffer, rect.x(), rect.y(), rect.width(), rect.height());
    QVERIFY(checkHole(buffer, oddPixel1, clearRect1 | clearRect2,
                      defaultPixel, rect));


}
void KisTiledDataManagerTest::testUnversionedBitBlt()
{
    quint8 defaultPixel = 0;
    KisTiledDataManager srcDM(1, &defaultPixel);
    KisTiledDataManager dstDM(1, &defaultPixel);

    quint8 oddPixel1 = 128;
    quint8 oddPixel2 = 129;

    QRect rect(0,0,512,512);
    QRect cloneRect(81,80,250,250);
    QRect tilesRect(2,2,3,3);

    srcDM.clear(rect, &oddPixel1);
    dstDM.clear(rect, &oddPixel2);

    dstDM.bitBlt(&srcDM, cloneRect);

    quint8 *buffer = new quint8[rect.width()*rect.height()];

    dstDM.readBytes(buffer, rect.x(), rect.y(), rect.width(), rect.height());

    QVERIFY(checkHole(buffer, oddPixel1, cloneRect,
                      oddPixel2, rect));

    delete[] buffer;

    // Test whether tiles became shared
    QVERIFY(checkTilesShared(&srcDM, &dstDM, false, false, tilesRect));
}

void KisTiledDataManagerTest::testVersionedBitBlt()
{
    quint8 defaultPixel = 0;
    KisTiledDataManager srcDM1(1, &defaultPixel);
    KisTiledDataManager srcDM2(1, &defaultPixel);
    KisTiledDataManager dstDM(1, &defaultPixel);

    quint8 oddPixel1 = 128;
    quint8 oddPixel2 = 129;
    quint8 oddPixel3 = 130;

    quint8 oddPixel4 = 131;

    QRect rect(0,0,512,512);
    QRect cloneRect(81,80,250,250);
    QRect tilesRect(2,2,3,3);


    KisMementoSP memento1 = srcDM1.getMemento();
    srcDM1.clear(rect, &oddPixel1);

    srcDM2.clear(rect, &oddPixel2);
    dstDM.clear(rect, &oddPixel3);

    KisMementoSP memento2 = dstDM.getMemento();
    dstDM.bitBlt(&srcDM1, cloneRect);

    QVERIFY(checkTilesShared(&srcDM1, &dstDM, false, false, tilesRect));
    QVERIFY(checkTilesNotShared(&srcDM1, &srcDM1, true, false, tilesRect));
    QVERIFY(checkTilesNotShared(&dstDM, &dstDM, true, false, tilesRect));

    dstDM.commit();
    QVERIFY(checkTilesShared(&dstDM, &dstDM, true, false, tilesRect));

    KisMementoSP memento3 = srcDM2.getMemento();
    srcDM2.clear(rect, &oddPixel4);

    KisMementoSP memento4 = dstDM.getMemento();
    dstDM.bitBlt(&srcDM2, cloneRect);

    QVERIFY(checkTilesShared(&srcDM2, &dstDM, false, false, tilesRect));
    QVERIFY(checkTilesNotShared(&srcDM2, &srcDM2, true, false, tilesRect));
    QVERIFY(checkTilesNotShared(&dstDM, &dstDM, true, false, tilesRect));

    dstDM.commit();
    QVERIFY(checkTilesShared(&dstDM, &dstDM, true, false, tilesRect));

    dstDM.rollback(memento4);
    QVERIFY(checkTilesShared(&srcDM1, &dstDM, false, false, tilesRect));
    QVERIFY(checkTilesShared(&dstDM, &dstDM, true, false, tilesRect));
    QVERIFY(checkTilesNotShared(&srcDM1, &srcDM1, true, false, tilesRect));

    dstDM.rollforward(memento4);
    QVERIFY(checkTilesShared(&srcDM2, &dstDM, false, false, tilesRect));
    QVERIFY(checkTilesShared(&dstDM, &dstDM, true, false, tilesRect));
    QVERIFY(checkTilesNotShared(&srcDM1, &srcDM1, true, false, tilesRect));
}

void KisTiledDataManagerTest::testBitBltOldData()
{
    quint8 defaultPixel = 0;
    KisTiledDataManager srcDM(1, &defaultPixel);
    KisTiledDataManager dstDM(1, &defaultPixel);

    quint8 oddPixel1 = 128;
    quint8 oddPixel2 = 129;

    QRect rect(0,0,512,512);
    QRect cloneRect(81,80,250,250);

    quint8 *buffer = new quint8[rect.width()*rect.height()];

    KisMementoSP memento1 = srcDM.getMemento();
    srcDM.clear(rect, &oddPixel1);
    srcDM.commit();

    dstDM.bitBltOldData(&srcDM, cloneRect);
    dstDM.readBytes(buffer, rect.x(), rect.y(), rect.width(), rect.height());
    QVERIFY(checkHole(buffer, oddPixel1, cloneRect,
                      defaultPixel, rect));

    KisMementoSP memento2 = srcDM.getMemento();
    srcDM.clear(rect, &oddPixel2);
    dstDM.bitBltOldData(&srcDM, cloneRect);
    srcDM.commit();

    dstDM.readBytes(buffer, rect.x(), rect.y(), rect.width(), rect.height());
    QVERIFY(checkHole(buffer, oddPixel1, cloneRect,
                      defaultPixel, rect));

    delete[] buffer;
}

void KisTiledDataManagerTest::testBitBltRough()
{
    quint8 defaultPixel = 0;
    KisTiledDataManager srcDM(1, &defaultPixel);
    KisTiledDataManager dstDM(1, &defaultPixel);

    quint8 oddPixel1 = 128;
    quint8 oddPixel2 = 129;
    quint8 oddPixel3 = 130;

    QRect rect(0,0,512,512);
    QRect cloneRect(81,80,250,250);
    QRect actualCloneRect(64,64,320,320);
    QRect tilesRect(1,1,4,4);

    srcDM.clear(rect, &oddPixel1);
    dstDM.clear(rect, &oddPixel2);

    dstDM.bitBltRough(&srcDM, cloneRect);

    quint8 *buffer = new quint8[rect.width()*rect.height()];

    dstDM.readBytes(buffer, rect.x(), rect.y(), rect.width(), rect.height());

    QVERIFY(checkHole(buffer, oddPixel1, actualCloneRect,
                      oddPixel2, rect));

    // Test whether tiles became shared
    QVERIFY(checkTilesShared(&srcDM, &dstDM, false, false, tilesRect));

    // check bitBltRoughOldData
    KisMementoSP memento1 = srcDM.getMemento();
    srcDM.clear(rect, &oddPixel3);
    dstDM.bitBltRoughOldData(&srcDM, cloneRect);
    srcDM.commit();
    dstDM.readBytes(buffer, rect.x(), rect.y(), rect.width(), rect.height());
    QVERIFY(checkHole(buffer, oddPixel1, actualCloneRect,
                      oddPixel2, rect));

    delete[] buffer;
}

void KisTiledDataManagerTest::testTransactions()
{
    quint8 defaultPixel = 0;
    KisTiledDataManager dm(1, &defaultPixel);

    quint8 oddPixel1 = 128;
    quint8 oddPixel2 = 129;
    quint8 oddPixel3 = 130;

    KisTileSP tile00;
    KisTileSP oldTile00;

    // Create a named transaction: versioning is enabled
    KisMementoSP memento1 = dm.getMemento();
    dm.clear(0, 0, 64, 64, &oddPixel1);

    tile00 = dm.getTile(0, 0, false);
    oldTile00 = dm.getOldTile(0, 0);
    QVERIFY(memoryIsFilled(oddPixel1, tile00->data(), TILESIZE));
    QVERIFY(memoryIsFilled(defaultPixel, oldTile00->data(), TILESIZE));
    tile00 = oldTile00 = 0;

    // Create an anonymous transaction: versioning is disabled
    dm.commit();
    tile00 = dm.getTile(0, 0, false);
    oldTile00 = dm.getOldTile(0, 0);
    QVERIFY(memoryIsFilled(oddPixel1, tile00->data(), TILESIZE));
    QVERIFY(memoryIsFilled(oddPixel1, oldTile00->data(), TILESIZE));
    tile00 = oldTile00 = 0;

    dm.clear(0, 0, 64, 64, &oddPixel2);

    // Versioning is disabled, i said! >:)
    tile00 = dm.getTile(0, 0, false);
    oldTile00 = dm.getOldTile(0, 0);
    QVERIFY(memoryIsFilled(oddPixel2, tile00->data(), TILESIZE));
    QVERIFY(memoryIsFilled(oddPixel2, oldTile00->data(), TILESIZE));
    tile00 = oldTile00 = 0;

    // And the last round: named transaction:
    KisMementoSP memento2 = dm.getMemento();
    dm.clear(0, 0, 64, 64, &oddPixel3);

    tile00 = dm.getTile(0, 0, false);
    oldTile00 = dm.getOldTile(0, 0);
    QVERIFY(memoryIsFilled(oddPixel3, tile00->data(), TILESIZE));
    QVERIFY(memoryIsFilled(oddPixel2, oldTile00->data(), TILESIZE));
    tile00 = oldTile00 = 0;

}

void KisTiledDataManagerTest::testPurgeHistory()
{
    quint8 defaultPixel = 0;
    KisTiledDataManager dm(1, &defaultPixel);

    quint8 oddPixel1 = 128;
    quint8 oddPixel2 = 129;
    quint8 oddPixel3 = 130;
    quint8 oddPixel4 = 131;

    KisMementoSP memento1 = dm.getMemento();
    dm.clear(0, 0, 64, 64, &oddPixel1);
    dm.commit();

    KisMementoSP memento2 = dm.getMemento();
    dm.clear(0, 0, 64, 64, &oddPixel2);

    KisTileSP tile00;
    KisTileSP oldTile00;

    tile00 = dm.getTile(0, 0, false);
    oldTile00 = dm.getOldTile(0, 0);
    QVERIFY(memoryIsFilled(oddPixel2, tile00->data(), TILESIZE));
    QVERIFY(memoryIsFilled(oddPixel1, oldTile00->data(), TILESIZE));
    tile00 = oldTile00 = 0;

    dm.purgeHistory(memento1);

    /**
     * Nothing has changed in the visible state of the data manager
     */

    tile00 = dm.getTile(0, 0, false);
    oldTile00 = dm.getOldTile(0, 0);
    QVERIFY(memoryIsFilled(oddPixel2, tile00->data(), TILESIZE));
    QVERIFY(memoryIsFilled(oddPixel1, oldTile00->data(), TILESIZE));
    tile00 = oldTile00 = 0;

    dm.commit();

    dm.purgeHistory(memento2);

    /**
     * We've removed all the history of the device, so it
     * became "unversioned".
     * NOTE: the return value for getOldTile() when there is no
     * history present is a subject for change
     */

    tile00 = dm.getTile(0, 0, false);
    oldTile00 = dm.getOldTile(0, 0);
    QVERIFY(memoryIsFilled(oddPixel2, tile00->data(), TILESIZE));
    QVERIFY(memoryIsFilled(oddPixel2, oldTile00->data(), TILESIZE));
    tile00 = oldTile00 = 0;

    /**
     * Just test we won't crash when the memento is not
     * present in history anymore
     */

    KisMementoSP memento3 = dm.getMemento();
    dm.clear(0, 0, 64, 64, &oddPixel3);
    dm.commit();

    KisMementoSP memento4 = dm.getMemento();
    dm.clear(0, 0, 64, 64, &oddPixel4);
    dm.commit();

    dm.rollback(memento4);

    dm.purgeHistory(memento3);
    dm.purgeHistory(memento4);
}

void KisTiledDataManagerTest::testUndoSetDefaultPixel()
{
    quint8 defaultPixel = 0;
    KisTiledDataManager dm(1, &defaultPixel);

    quint8 oddPixel1 = 128;
    quint8 oddPixel2 = 129;

    QRect fillRect(0,0,64,64);

    KisTileSP tile00;
    KisTileSP tile10;

    tile00 = dm.getTile(0, 0, false);
    tile10 = dm.getTile(1, 0, false);
    QVERIFY(memoryIsFilled(defaultPixel, tile00->data(), TILESIZE));
    QVERIFY(memoryIsFilled(defaultPixel, tile10->data(), TILESIZE));

    KisMementoSP memento1 = dm.getMemento();
    dm.clear(fillRect, &oddPixel1);
    dm.commit();

    tile00 = dm.getTile(0, 0, false);
    tile10 = dm.getTile(1, 0, false);
    QVERIFY(memoryIsFilled(oddPixel1, tile00->data(), TILESIZE));
    QVERIFY(memoryIsFilled(defaultPixel, tile10->data(), TILESIZE));

    KisMementoSP memento2 = dm.getMemento();
    dm.setDefaultPixel(&oddPixel2);
    dm.commit();

    tile00 = dm.getTile(0, 0, false);
    tile10 = dm.getTile(1, 0, false);
    QVERIFY(memoryIsFilled(oddPixel1, tile00->data(), TILESIZE));
    QVERIFY(memoryIsFilled(oddPixel2, tile10->data(), TILESIZE));

    dm.rollback(memento2);

    tile00 = dm.getTile(0, 0, false);
    tile10 = dm.getTile(1, 0, false);
    QVERIFY(memoryIsFilled(oddPixel1, tile00->data(), TILESIZE));
    QVERIFY(memoryIsFilled(defaultPixel, tile10->data(), TILESIZE));

    dm.rollback(memento1);

    tile00 = dm.getTile(0, 0, false);
    tile10 = dm.getTile(1, 0, false);
    QVERIFY(memoryIsFilled(defaultPixel, tile00->data(), TILESIZE));
    QVERIFY(memoryIsFilled(defaultPixel, tile10->data(), TILESIZE));

    dm.rollforward(memento1);

    tile00 = dm.getTile(0, 0, false);
    tile10 = dm.getTile(1, 0, false);
    QVERIFY(memoryIsFilled(oddPixel1, tile00->data(), TILESIZE));
    QVERIFY(memoryIsFilled(defaultPixel, tile10->data(), TILESIZE));

    dm.rollforward(memento2);

    tile00 = dm.getTile(0, 0, false);
    tile10 = dm.getTile(1, 0, false);
    QVERIFY(memoryIsFilled(oddPixel1, tile00->data(), TILESIZE));
    QVERIFY(memoryIsFilled(oddPixel2, tile10->data(), TILESIZE));
}

void KisTiledDataManagerTest::testPageStoreSaveReopenRoundTrip()
{
    quint8 defaultPixel = 0x19;
    quint8 paintedPixel = 0xa7;
    const QRect paintedRect(-70, -3, 141, 73);
    const QRect sampleRect(-128, -64, 256, 192);
    TestableTiledDataManager source(1, &defaultPixel);
    source.clear(paintedRect, &paintedPixel);

    QByteArray expected(sampleRect.width() * sampleRect.height(), char(0));
    source.readBytes(reinterpret_cast<quint8 *>(expected.data()),
                     sampleRect.x(), sampleRect.y(),
                     sampleRect.width(), sampleRect.height());

    KoStoreFake store;
    KisFakePaintDeviceWriter writer(&store);
    QVERIFY(source.write(writer));
    store.startReading();

    TestableTiledDataManager reopened(1, &defaultPixel);
    QVERIFY(reopened.read(store.device()));
    QCOMPARE(reopened.extent(), source.extent());
    QByteArray actual(expected.size(), char(0));
    reopened.readBytes(reinterpret_cast<quint8 *>(actual.data()),
                       sampleRect.x(), sampleRect.y(),
                       sampleRect.width(), sampleRect.height());
    QCOMPARE(actual, expected);

    const KisMementoSP edit = reopened.getMemento();
    quint8 replacement = 0x44;
    reopened.clear(QRect(-64, 0, 64, 64), &replacement);
    reopened.commit();
    reopened.rollback(edit);
    reopened.readBytes(reinterpret_cast<quint8 *>(actual.data()),
                       sampleRect.x(), sampleRect.y(),
                       sampleRect.width(), sampleRect.height());
    QCOMPARE(actual, expected);
}

void KisTiledDataManagerTest::testPageStoreSparsePurge()
{
    quint8 defaultPixel = 0;
    quint8 paintedPixel = 0x5d;
    TestableTiledDataManager dm(1, &defaultPixel);
    dm.clear(QRect(0, 0, 64, 64), &paintedPixel);
    QCOMPARE(dm.extent(), QRect(0, 0, 64, 64));

    KisTileSP tile = dm.getTile(0, 0, true);
    tile->lockForWrite();
    std::memset(tile->data(), defaultPixel, TILESIZE);
    tile->unlockForWrite();
    QCOMPARE(dm.extent(), QRect(0, 0, 64, 64));

    dm.purge(QRect(0, 0, 64, 64));
    QCOMPARE(dm.extent(), QRect());
    QVERIFY(dm.region().isEmpty());

    bool existingTile = true;
    tile = dm.getReadOnlyTileLazy(0, 0, existingTile);
    QVERIFY(!existingTile);
    tile->lockForRead();
    QVERIFY(memoryIsFilled(defaultPixel, tile->data(), TILESIZE));
    tile->unlockForRead();
}

void KisTiledDataManagerTest::testPageStoreVirtualDefaultReadStaysSparse()
{
    quint8 defaultPixel = 0x2a;
    KisTiledDataManager dm(1, &defaultPixel);
    QVERIFY(dm.m_pageStoreBackend);
    QVERIFY(dm.m_pageStoreBackend->store());

    const KisPageStoreSessionStats before =
        dm.m_pageStoreBackend->store()->sessionStats();
    const qint32 tileDataBefore = KisTileDataStore::instance()->numTiles();

    constexpr qint32 coordinateCount = 0x7fff;
    for (qint32 i = 0; i < coordinateCount; ++i) {
        bool existingTile = true;
        KisTileSP tile = dm.getReadOnlyTileLazy(i, -i, existingTile);
        QVERIFY(tile);
        QVERIFY(!existingTile);
        tile->lockForRead();
        QCOMPARE(tile->data()[0], defaultPixel);
        tile->unlockForRead();
    }

    const KisPageStoreSessionStats after =
        dm.m_pageStoreBackend->store()->sessionStats();
    QCOMPARE(after.registeredPages, before.registeredPages);
    QCOMPARE(after.pageVersions, before.pageVersions);
    QCOMPARE(after.replicas, before.replicas);
    QCOMPARE(after.readRequestsCreated, before.readRequestsCreated);
    QCOMPARE(after.defaultMaterializationRequests,
             before.defaultMaterializationRequests);
    QCOMPARE(after.pendingRequests, qsizetype(0));
    QCOMPARE(after.activeReadLeases, qsizetype(0));
    QCOMPARE(KisTileDataStore::instance()->numTiles(), tileDataBefore);
    QCOMPARE(dm.extent(), QRect());
}

void KisTiledDataManagerTest::testPageStoreCompatibilityBehavior()
{
    quint8 defaultPixel = 0;
    quint8 paintedPixel = 0x35;
    KisTiledDataManager dm(1, &defaultPixel);
    auto *backend = dm.m_pageStoreBackend;
    QVERIFY(backend);

    dm.clear(QRect(0, 0, 64, 64), &paintedPixel);
    bool existingTile = false;
    KisTileSP tile = dm.getReadOnlyTileLazy(0, 0, existingTile);
    QVERIFY(tile);
    QVERIFY(existingTile);
    tile->setPageStoreBridge(nullptr, false);
    tile = dm.getReadOnlyTileLazy(0, 0, existingTile);
    QVERIFY(tile);
    QVERIFY(existingTile);
    tile->lockForRead();
    QCOMPARE(tile->data()[0], paintedPixel);
    tile->unlockForRead();

    KisTileSP writable = dm.getTile(0, 0, true);
    writable->lockForWrite();
    writable->data()[0] = 0x57;
    writable->unlockForWrite();

    tile = dm.getReadOnlyTileLazy(0, 0, existingTile);
    QVERIFY(tile && existingTile);
    tile->lockForRead();
    QCOMPARE(tile->data()[0], quint8(0x57));
    tile->unlockForRead();

    QVERIFY(!backend->acquireTile(0, 0, true, true));

}

void KisTiledDataManagerTest::testPageStorePackedReadCapturesOneView_data()
{
    testPageStoreDefaultLifecycleDoesNotMaterialize_data();
}

void KisTiledDataManagerTest::testPageStorePackedReadCapturesOneView()
{
    QFETCH(int, pixelSize);
    const QByteArray initial(pixelSize, char(0x2a));
    KisTiledDataManager dm(pixelSize, reinterpret_cast<const quint8 *>(initial.constData()));
    auto *store = dm.m_pageStoreBackend->store();
    const QRect painted(-64, -64, 128, 128);
    QByteArray pixels(painted.width() * painted.height() * pixelSize, Qt::Uninitialized);
    for (qsizetype i = 0; i < pixels.size(); ++i) pixels[i] = char(i * 37 % 251);
    dm.writeBytes(reinterpret_cast<const quint8 *>(pixels.constData()), painted.x(), painted.y(),
                  painted.width(), painted.height());
    const QRect area(-70, -65, 150, 140);
    const int stride = area.width() * pixelSize + 13;
    QByteArray output(stride * area.height(), char(0x6d));
    const auto before = store->sessionStats();
    const auto views = store->readScopeStatistics().capturedReadViewsCreated;
    dm.readBytes(reinterpret_cast<quint8 *>(output.data()), area.x(), area.y(), area.width(), area.height(), stride);
    QCOMPARE(store->readScopeStatistics().capturedReadViewsCreated, views + 1);
    QCOMPARE(store->sessionStats().readRequestsCreated, before.readRequestsCreated);
    QCOMPARE(store->sessionStats().registeredPages, before.registeredPages);
    QCOMPARE(store->sessionStats().defaultMaterializationRequests, before.defaultMaterializationRequests);
    for (int y = 0; y < area.height(); ++y) {
        for (int x = 0; x < area.width(); ++x) {
            const QPoint p = area.topLeft() + QPoint(x, y);
            const QByteArray expected = painted.contains(p)
                ? pixels.mid(((p.y() - painted.y()) * painted.width() + p.x() - painted.x()) * pixelSize, pixelSize)
                : initial;
            QCOMPARE(output.mid(y * stride + x * pixelSize, pixelSize), expected);
        }
        QCOMPARE(output.mid(y * stride + area.width() * pixelSize, 13), QByteArray(13, char(0x6d)));
    }
    QVERIFY(store->waitForRetirementIdle());
    QVERIFY(!store->sessionStats().hasOutstandingCapabilities());
}

void KisTiledDataManagerTest::testPageStorePackedReadSurvivesPublication_data()
{
    QTest::addColumn<int>("terminal");
    QTest::newRow("committed-head") << 0;
    QTest::newRow("overlay-commit") << 1;
    QTest::newRow("overlay-abort") << 2;
}

void KisTiledDataManagerTest::testPageStorePackedReadSurvivesPublication()
{
    quint8 initial = 0x2a;
    KisTiledDataManager dm(1, &initial);
    auto *backend = dm.m_pageStoreBackend;
    auto *store = backend->store();
    QFETCH(int, terminal);
    QString error;
    if (terminal) QVERIFY(backend->beginHistory(&initial, 1, &error));
    QVERIFY(backend->fillRect(QRect(0, 0, 128, 64), QByteArray(1, char(0x51)), &error));
    QByteArray bytes(192 * 64, char(0));
    int pages = 0;
    bool mutationSucceeded = false;
    const auto stats = store->sessionStats();
    KisPageStoreDiagnosticRecorder recorder(true, store);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::ReadBytesPage || ++pages != 2) return;
        // Same production backend APIs, without re-entering the manager's
        // read lock. The first page is already copied and its pin released.
        mutationSucceeded = backend->fillRect(QRect(0, 0, 128, 64), QByteArray(1, char(0x71)), &error) &&
            backend->setDefaultPixel(QByteArray(1, char(0x41)), &error);
        if (terminal == 1) mutationSucceeded &= backend->commitHistory(&initial, 1, &error);
        if (terminal == 2) mutationSucceeded &= backend->abortHistory(&error);
    });
    dm.readBytes(reinterpret_cast<quint8 *>(bytes.data()), 0, 0, 192, 64);
    QVERIFY2(mutationSucceeded, qPrintable(error)); QCOMPARE(pages, 3);
    for (int y = 0; y < 64; ++y) {
        QCOMPARE(bytes.mid(y * 192, 128), QByteArray(128, char(0x51)));
        QCOMPARE(bytes.mid(y * 192 + 128, 64), QByteArray(64, char(0x2a)));
    }
    QCOMPARE(store->sessionStats().readRequestsCreated, stats.readRequestsCreated);
    QVERIFY(store->waitForRetirementIdle());
}

void KisTiledDataManagerTest::testPageStoreIteratorFixedView_data()
{
    QTest::addColumn<int>("entry");
    QTest::addColumn<int>("terminal");
    for (int entry = 0; entry < 3; ++entry)
        for (int terminal = 0; terminal < 3; ++terminal)
            QTest::newRow(qPrintable(QStringLiteral("entry%1-terminal%2").arg(entry).arg(terminal))) << entry << terminal;
}

void KisTiledDataManagerTest::testPageStoreIteratorFixedView()
{
    QFETCH(int, entry);
    QFETCH(int, terminal);
    const quint8 background = 0x13;
    KisDataManager dm(1, &background);
    auto *backend = dm.m_pageStoreBackend;
    auto *store = backend->store();
    QString error;
    KisMementoSP history;
    if (terminal) { history = backend->beginHistory(&background, 1, &error); QVERIFY(history); }
    QVERIFY(backend->fillRect(QRect(-64, -64, 384, 384), QByteArray(1, char(0x47)), &error));
    const auto views = store->readScopeStatistics().capturedReadViewsCreated;
    const auto requests = store->sessionStats().readRequestsCreated;
    {
        std::unique_ptr<KisHLineIterator2> h;
        std::unique_ptr<KisVLineIterator2> v;
        std::unique_ptr<KisRandomAccessor2> r;
        if (entry == 0) h = std::make_unique<KisHLineIterator2>(&dm, -64, -64, 448, 0, 0, false, nullptr);
        if (entry == 1) v = std::make_unique<KisVLineIterator2>(&dm, -64, -64, 448, 0, 0, false, nullptr);
        if (entry == 2) { r = std::make_unique<KisRandomAccessor2>(&dm, 0, 0, false, nullptr); r->moveTo(-64, -64); }
        QCOMPARE(store->readScopeStatistics().capturedReadViewsCreated, views + (terminal ? 2 : 1));
        auto current = [&] { return h ? h->rawDataConst() : v ? v->rawDataConst() : r->rawDataConst(); };
        auto before = [&] { return h ? h->oldRawData() : v ? v->oldRawData() : r->oldRawData(); };
        QCOMPARE(*current(), quint8(0x47));
        QVERIFY(backend->fillRect(QRect(-64, -64, 384, 384), QByteArray(1, char(0x71)), &error));
        QVERIFY(backend->setDefaultPixel(QByteArray(1, char(0x29)), &error));
        if (terminal == 1) QVERIFY(backend->commitHistory(&background, 1, &error));
        if (terminal == 2) QVERIFY(backend->abortHistory(&error));
        for (int page = 0; page < 7; ++page) {
            if (page) {
                if (h) QVERIFY(h->nextPixels(64));
                if (v) QVERIFY(v->nextPixels(64));
                if (r) r->moveTo(-64 + page * 64, -64);
            }
            QCOMPARE(*current(), page < 6 ? quint8(0x47) : background);
            QCOMPARE(*before(), terminal ? background : *current());
            if (!terminal) QCOMPARE(before(), current());
        }
        // Revisit an evicted page, then change scanline without recapturing.
        if (h) { h->resetPixelPos(); h->nextRow(); }
        if (v) { v->resetPixelPos(); v->nextColumn(); }
        if (r) r->moveTo(-64, -63);
        QCOMPARE(*current(), quint8(0x47));
        QCOMPARE(*before(), terminal ? background : quint8(0x47));
        QCOMPARE(store->sessionStats().readRequestsCreated, requests);
    }
    if (terminal == 1) QVERIFY(backend->purgeHistory(history, &background, 1, &error));
    QVERIFY(store->waitForRetirementIdle());
    QVERIFY(!store->sessionStats().hasOutstandingCapabilities());
}

void KisTiledDataManagerTest::testPageStoreIteratorLiveWriteCompatibility_data()
{
    QTest::addColumn<int>("entry");
    QTest::addColumn<bool>("history");
    for (int entry = 0; entry < 3; ++entry)
        for (bool history : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("entry%1-history%2").arg(entry).arg(history))) << entry << history;
}

void KisTiledDataManagerTest::testPageStoreIteratorLiveWriteCompatibility()
{
    QFETCH(int, entry);
    QFETCH(bool, history);
    const quint8 background = 0x13;
    KisDataManager dm(1, &background);
    auto *backend = dm.m_pageStoreBackend;
    auto *store = backend->store();
    KisMementoSP memento;
    if (history) memento = dm.getMemento();
    {
        std::unique_ptr<KisHLineIterator2> h;
        std::unique_ptr<KisVLineIterator2> v;
        std::unique_ptr<KisRandomAccessor2> r;
        if (entry == 0) h = std::make_unique<KisHLineIterator2>(&dm, 0, 0, 128, 0, 0, true, nullptr);
        if (entry == 1) v = std::make_unique<KisVLineIterator2>(&dm, 0, 0, 128, 0, 0, true, nullptr);
        if (entry == 2) { r = std::make_unique<KisRandomAccessor2>(&dm, 0, 0, true, nullptr); r->moveTo(0, 0); }
        auto data = [&] { return h ? h->rawData() : v ? v->rawData() : r->rawData(); };
        auto before = [&] { return h ? h->oldRawData() : v ? v->oldRawData() : r->oldRawData(); };
        quint8 *borrowed = data();
        *borrowed = 0x47;
        const auto sealed = store->mutationStatistics().pagesSealed;
        QVERIFY(backend->hasCurrentThreadIteratorWrites());
        QVERIFY(dm.capturePageStoreReadScope(false)->observesLiveLegacyWriter());
        {
            KisHLineIterator2 readH(&dm, 0, 0, 1, 0, 0, false, nullptr);
            KisVLineIterator2 readV(&dm, 0, 0, 1, 0, 0, false, nullptr);
            KisRandomAccessor2 readR(&dm, 0, 0, false, nullptr); readR.moveTo(0, 0);
            QCOMPARE(readH.rawDataConst()[0], quint8(0x47));
            QCOMPARE(readV.rawDataConst()[0], quint8(0x47));
            QCOMPARE(readR.rawDataConst()[0], quint8(0x47));
            quint8 packed = 0;
            dm.readBytes(&packed, 0, 0, 1, 1);
            QCOMPARE(packed, quint8(0x47));
            auto planes = dm.readPlanarBytes({1}, 0, 0, 1, 1);
            QCOMPARE(planes.size(), 1); QCOMPARE(planes[0][0], quint8(0x47)); delete[] planes[0];
            QCOMPARE(data(), borrowed);
            QCOMPARE(store->mutationStatistics().pagesSealed, sealed);
            // A read does not revoke a pointer issued by a still-live writer.
            *borrowed = 0x71;
            QCOMPARE(readH.rawDataConst()[0], quint8(0x71));
            QCOMPARE(before()[0], history ? background : quint8(0x71));
            if (!history) QCOMPARE(before(), borrowed);
        }
        QCOMPARE(data(), borrowed);
    }
    QVERIFY(!backend->hasCurrentThreadIteratorWrites());
    dm.commit();
    quint8 actual = 0; dm.readBytes(&actual, 0, 0, 1, 1);
    QCOMPARE(actual, quint8(0x71));
    if (history) { dm.rollback(memento); dm.readBytes(&actual, 0, 0, 1, 1); QCOMPARE(actual, background); }
}

void KisTiledDataManagerTest::testPageStoreIteratorOtherThreadReadsSealedView()
{
    const quint8 background = 0x13;
    KisDataManager dm(1, &background);
    {
        KisRandomAccessor2 writer(&dm, 0, 0, true, nullptr);
        writer.moveTo(0, 0); writer.rawData()[0] = 0x47;
        bool nativeScope = false;
        quint8 seen = 0;
        std::thread reader([&] {
            const auto scope = dm.capturePageStoreReadScope(false);
            nativeScope = scope && scope->isValid() && !scope->observesLiveLegacyWriter();
            KisPageStoreReadCursor cursor(scope);
            const auto page = cursor.read(0, 0);
            if (page.isValid()) seen = page.current[0];
        });
        reader.join();
        QVERIFY(nativeScope); QCOMPARE(seen, background);
        QCOMPARE(writer.rawData()[0], quint8(0x47));
    }
    quint8 seen = 0; dm.readBytes(&seen, 0, 0, 1, 1); QCOMPARE(seen, quint8(0x47));
    auto *backend = dm.m_pageStoreBackend;
    auto finalWriter = std::make_unique<KisRandomAccessor2>(
        &dm, 0, 0, true, nullptr);
    finalWriter->moveTo(0, 0);
    QVERIFY(finalWriter->rawData());
    QVERIFY(backend->hasCurrentThreadIteratorWrites());
    std::thread finalRelease([writer = std::move(finalWriter)]() mutable {
        writer.reset();
    });
    finalRelease.join();
    QVERIFY(!backend->hasCurrentThreadIteratorWrites());
}

void KisTiledDataManagerTest::testPageStoreReadCursorBounded()
{
    const quint8 background = 0x13;
    KisDataManager dm(1, &background);
    auto *store = dm.m_pageStoreBackend->store();
    const auto initial = store->sessionStats();
    const auto views = store->readScopeStatistics().capturedReadViewsCreated;
    {
        const auto scope = dm.capturePageStoreReadScope(false);
        KisDataManager other(1, &background);
        const auto foreign = other.capturePageStoreReadScope(false, scope);
        QVERIFY(foreign && !foreign->isValid() && !foreign->observesLiveLegacyWriter());
        const auto wrongMode = dm.capturePageStoreReadScope(true, scope);
        QVERIFY(wrongMode && !wrongMode->isValid() && !wrongMode->observesLiveLegacyWriter());
        KisPageStoreReadCursor cursor(scope), peer(scope);
        for (int x = -100; x < 100; ++x) {
            const auto page = cursor.read(x, x);
            QVERIFY(page.isValid()); QCOMPARE(page.current, page.before);
            QCOMPARE(page.current[0], background);
        }
        QVERIFY(peer.read(-100, -100).isValid());
        QVERIFY(cursor.read(-100, -100).isValid());
        // Peers share a logical view only, never a mutable pin cache.
        KisHLineIterator2 h(&dm, 0, 0, 640, 0, 0, false, nullptr, scope);
        KisVLineIterator2 v(&dm, 0, 0, 640, 0, 0, false, nullptr, scope);
        QCOMPARE(h.rawDataConst()[0], background); QCOMPARE(v.rawDataConst()[0], background);
        QCOMPARE(store->readScopeStatistics().capturedReadViewsCreated, views + 1);
    }
    QCOMPARE(store->sessionStats().registeredPages, initial.registeredPages);
    QCOMPARE(store->sessionStats().readRequestsCreated, initial.readRequestsCreated);
    QCOMPARE(store->sessionStats().defaultMaterializationRequests, initial.defaultMaterializationRequests);
    QVERIFY(!store->sessionStats().hasOutstandingCapabilities());
}

void KisTiledDataManagerTest::testPageStoreWrappedIteratorFixedView_data()
{
    QTest::addColumn<bool>("horizontal");
    QTest::newRow("hline-four-peers") << true;
    QTest::newRow("vline-four-peers") << false;
}

void KisTiledDataManagerTest::testPageStoreWrappedIteratorFixedView()
{
    QFETCH(bool, horizontal);
    const quint8 background = 0x13;
    KisDataManager dm(1, &background);
    auto *backend = dm.m_pageStoreBackend;
    auto *store = backend->store();
    QString error;
    const QRect bounds(0, 0, 128, 128), area(-5, -7, 270, 260);
    QVERIFY(backend->fillRect(bounds, QByteArray(1, char(0x47)), &error));
    const auto views = store->readScopeStatistics().capturedReadViewsCreated;
    const auto requests = store->sessionStats().readRequestsCreated;
    {
        const KisWrappedRect split(area, bounds, WRAPAROUND_BOTH);
        QCOMPARE(split.size(), 4);
        std::unique_ptr<KisWrappedHLineIterator> h;
        std::unique_ptr<KisWrappedVLineIterator> v;
        if (horizontal) h = std::make_unique<KisWrappedHLineIterator>(&dm, split, 0, 0, false, nullptr);
        else v = std::make_unique<KisWrappedVLineIterator>(&dm, split, 0, 0, false, nullptr);
        QCOMPARE(store->readScopeStatistics().capturedReadViewsCreated, views + 1);
        QVERIFY(backend->fillRect(bounds, QByteArray(1, char(0x71)), &error));
        for (int line = 0; line < (horizontal ? area.height() : area.width()); ++line) {
            int pixels = 0;
            do {
                QCOMPARE((h ? h->rawDataConst() : v->rawDataConst())[0], quint8(0x47));
                QCOMPARE((h ? h->oldRawData() : v->oldRawData())[0], quint8(0x47));
                ++pixels;
            } while (h ? h->nextPixel() : v->nextPixel());
            QCOMPARE(pixels, horizontal ? area.width() : area.height());
            if (line + 1 < (horizontal ? area.height() : area.width())) {
                if (h) h->nextRow(); else v->nextColumn();
            }
        }
        QCOMPARE(store->sessionStats().readRequestsCreated, requests);
    }
    QVERIFY(store->waitForRetirementIdle());
    QVERIFY(!store->sessionStats().hasOutstandingCapabilities());
}

void KisTiledDataManagerTest::testPageStoreRepeatIteratorFixedView_data()
{
    testPageStoreWrappedIteratorFixedView_data();
}

void KisTiledDataManagerTest::testPageStoreRepeatIteratorFixedView()
{
    QFETCH(bool, horizontal);
    const quint8 background = 0x13;
    KisDataManager dm(1, &background);
    auto *backend = dm.m_pageStoreBackend;
    auto *store = backend->store();
    QString error;
    const QRect bounds(0, 0, 128, 128);
    QVERIFY(backend->fillRect(bounds, QByteArray(1, char(0x47)), &error));
    const auto views = store->readScopeStatistics().capturedReadViewsCreated;
    const auto requests = store->sessionStats().readRequestsCreated;
    {
        std::unique_ptr<KisRepeatHLineIteratorPixelBase<KisHLineIterator2>> h;
        std::unique_ptr<KisRepeatVLineIteratorPixelBase<KisVLineIterator2>> v;
        if (horizontal) h = std::make_unique<KisRepeatHLineIteratorPixelBase<KisHLineIterator2>>(&dm, -2, -2, 140, 0, 0, bounds, nullptr);
        else v = std::make_unique<KisRepeatVLineIteratorPixelBase<KisVLineIterator2>>(&dm, -2, -2, 140, 0, 0, bounds, nullptr);
        QCOMPARE(store->readScopeStatistics().capturedReadViewsCreated, views + 1);
        const auto beforeFill = store->readScopeStatistics().capturedReadViewsCreated;
        QVERIFY(backend->fillRect(bounds, QByteArray(1, char(0x71)), &error));
        QCOMPARE(store->readScopeStatistics().capturedReadViewsCreated, beforeFill + 1);
        for (int line = 0; line < 140; ++line) {
            int pixels = 0;
            do {
                QCOMPARE((h ? h->oldRawData() : v->oldRawData())[0], quint8(0x47));
                ++pixels;
            } while (h ? h->nextPixel() : v->nextPixel());
            QCOMPARE(pixels, 140);
            if (line + 1 < 140) { if (h) h->nextRow(); else v->nextColumn(); }
        }
        // Repeated borders reuse the read View; fill owns a separate no-op/base selection.
        QCOMPARE(store->readScopeStatistics().capturedReadViewsCreated, beforeFill + 1);
        QCOMPARE(store->sessionStats().readRequestsCreated, requests);
    }
    QVERIFY(store->waitForRetirementIdle());
    QVERIFY(!store->sessionStats().hasOutstandingCapabilities());
}

void KisTiledDataManagerTest::testPageStoreIteratorNextPixelsOffsets()
{
    const quint8 background = 0;
    KisDataManager dm(1, &background);
    QByteArray base(64 * 64, Qt::Uninitialized), current(base.size(), Qt::Uninitialized);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
        base[y * 64 + x] = char(x + y * 2); current[y * 64 + x] = char(x + y * 3);
    }
    dm.writeBytes(reinterpret_cast<const quint8 *>(base.constData()), 0, 0, 64, 64);
    const auto memento = dm.getMemento();
    dm.writeBytes(reinterpret_cast<const quint8 *>(current.constData()), 0, 0, 64, 64);
    {
        KisHLineIterator2 h(&dm, 1, 1, 60, 0, 0, false, nullptr);
        KisVLineIterator2 v(&dm, 1, 1, 60, 0, 0, false, nullptr);
        QVERIFY(h.nextPixels(5)); QVERIFY(v.nextPixels(5));
        QCOMPARE(h.rawDataConst()[0], quint8(9)); QCOMPARE(h.oldRawData()[0], quint8(8));
        QCOMPARE(v.rawDataConst()[0], quint8(19)); QCOMPARE(v.oldRawData()[0], quint8(13));
        h.resetPixelPos(); v.resetPixelPos();
        QCOMPARE(h.rawDataConst()[0], quint8(4)); QCOMPARE(v.oldRawData()[0], quint8(3));
    }
    dm.commit(); dm.purgeHistory(memento);
}

void KisTiledDataManagerTest::testPageStorePlanarReadFixedView_data()
{
    testPageStoreDefaultLifecycleDoesNotMaterialize_data();
}

void KisTiledDataManagerTest::testPageStorePlanarReadFixedView()
{
    QFETCH(int, pixelSize);
    QByteArray background(pixelSize, Qt::Uninitialized), painted(pixelSize, Qt::Uninitialized);
    for (int i = 0; i < pixelSize; ++i) { background[i] = char(0x13 + i); painted[i] = char(0x47 + i); }
    KisDataManager dm(pixelSize, reinterpret_cast<const quint8 *>(background.constData()));
    auto *backend = dm.m_pageStoreBackend;
    auto *store = backend->store();
    QString error;
    QVERIFY(backend->fillRect(QRect(-64, -64, 128, 128), painted, &error));
    const QRect area(-70, -65, 150, 140);
    const QVector<qint32> channels = pixelSize == 1 ? QVector<qint32>{1} : QVector<qint32>{1, 0, pixelSize - 2, 1};
    const auto views = store->readScopeStatistics().capturedReadViewsCreated;
    const auto requests = store->sessionStats().readRequestsCreated;
    bool changed = false;
    quint64 mutationViews = 0;
    int pages = 0;
    KisPageStoreDiagnosticRecorder recorder(true, store);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::ReadPlanarPage || ++pages != 2) return;
        const auto beforeMutation = store->readScopeStatistics().capturedReadViewsCreated;
        changed = backend->fillRect(QRect(-64, -64, 128, 128), QByteArray(pixelSize, char(0x71)), &error) &&
            backend->setDefaultPixel(QByteArray(pixelSize, char(0x29)), &error);
        mutationViews += store->readScopeStatistics().capturedReadViewsCreated - beforeMutation;
    });
    auto planes = dm.readPlanarBytes(channels, area.x(), area.y(), area.width(), area.height());
    const auto cleanup = qScopeGuard([&] { for (auto plane : planes) delete[] plane; });
    QCOMPARE(planes.size(), channels.size()); QVERIFY2(changed, qPrintable(error));
    QCOMPARE(mutationViews, quint64(1));
    QCOMPARE(store->readScopeStatistics().capturedReadViewsCreated - mutationViews, views + 1);
    QCOMPARE(store->sessionStats().readRequestsCreated, requests);
    for (int y = 0; y < area.height(); ++y) for (int x = 0; x < area.width(); ++x) {
        const auto p = area.topLeft() + QPoint(x, y);
        const auto expected = QRect(-64, -64, 128, 128).contains(p) ? painted : background;
        int offset = 0;
        for (int channel = 0; channel < channels.size(); ++channel) {
            QCOMPARE(QByteArray(reinterpret_cast<const char *>(planes[channel] + (y * area.width() + x) * channels[channel]), channels[channel]),
                     expected.mid(offset, channels[channel]));
            offset += channels[channel];
        }
    }
    auto empty = dm.readPlanarBytes(channels, 0, 0, 0, 10);
    QCOMPARE(empty.size(), channels.size()); for (auto plane : empty) delete[] plane;
    QCOMPARE(store->readScopeStatistics().capturedReadViewsCreated - mutationViews, views + 1);
}

void KisTiledDataManagerTest::testPageStoreDefaultLifecycleDoesNotMaterialize_data()
{
    QTest::addColumn<int>("pixelSize");
    for (int stride : {1, 4, 8, 16}) {
        QTest::newRow(qPrintable(QString::number(stride))) << stride;
    }
}

void KisTiledDataManagerTest::testPageStorePixelOperation_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("history"); QTest::addColumn<int>("failure");
    for (int bpp : {1, 4, 8, 16}) for (bool history : {false, true}) for (int failure = 0; failure < 4; ++failure)
        QTest::newRow(qPrintable(QStringLiteral("bpp%1-history%2-failure%3").arg(bpp).arg(history).arg(failure)))
            << bpp << history << failure;
}

void KisTiledDataManagerTest::testPageStorePixelOperation()
{
    QFETCH(int, bpp); QFETCH(bool, history); QFETCH(int, failure);
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31)), prior(bpp, char(0x41));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    dm.clear(0, 0, 128, 64, reinterpret_cast<const quint8 *>(initial.constData()));
    auto *backend = dm.m_pageStoreBackend; auto *store = backend->store();
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    if (history) dm.clear(0, 0, 128, 64, reinterpret_cast<const quint8 *>(prior.constData()));
    auto beforeView = backend->captureReadView();
    const auto foreignTx = failure == 3 ? store->beginCurrentTransaction() : KisPageTransaction{};
    auto foreign = failure == 3 ? store->beginMutation(foreignTx) : KisPageMutationSession{};
    auto held = failure == 3 ? foreign.beginWrite({{1}, {1, 0}}) : KisCpuWriteGuard{};
    if (failure == 3) QVERIFY(held.isValid());
    const auto counts = store->mutationStatistics(); const auto requests = store->sessionStats();
    bool hidden = true, called = false; QString error;
    const auto result = dm.writePageStoreOperation({QRect(0, 0, 128, 2)}, [&](KisPixelWriteCursor *cursor) {
        called = true;
        for (const QPoint p : {QPoint(0,0), QPoint(64,0), QPoint(0,1), QPoint(64,1)}) {
            cursor->moveTo(p.x(), p.y());
            if (!cursor->rawData()) return false;
            memset(cursor->rawData(), 0x71 + p.y(), size_t(bpp));
            auto view = backend->captureReadView();
            auto read = view.readResidentPage({{1}, {0,0}});
            hidden &= read.isValid() && static_cast<const quint8 *>(read.data())[0] == quint8(history ? 0x41 : 0x31);
        }
        if (failure == 2) { cursor->moveTo(128,0); hidden &= !cursor->rawData(); cursor->moveTo(0,0); hidden &= !cursor->rawData(); }
        return failure != 1;
    }, &error);
    // A pre-existing foreign claim rejects the entire managed range before
    // the callback can expose even its first pixel.
    QCOMPARE(called, failure != 3);
    QVERIFY(hidden);
    QCOMPARE(result, failure ? KisPageStoreWriteOperationResult::Failed : KisPageStoreWriteOperationResult::Succeeded);
    const auto after = store->mutationStatistics();
    QCOMPARE(after.sessionsCreated - counts.sessionsCreated, quint64(failure == 3 ? 0 : 1));
    QCOMPARE(after.generationsReserved - counts.generationsReserved, quint64(failure == 3 ? 0 : 2));
    QCOMPARE(after.pagesSealed - counts.pagesSealed, quint64(failure ? 0 : 2));
    QCOMPARE(after.pagesCancelled - counts.pagesCancelled, quint64(failure == 3 ? 0 : failure ? 2 : 0));
    QCOMPARE(after.maximumPinnedPagesPerSegment, quint64(failure == 3 ? 0 : 1));
    QCOMPARE(store->sessionStats().writeRequestsCreated, requests.writeRequestsCreated);
    QCOMPARE(store->sessionStats().readRequestsCreated, requests.readRequestsCreated);
    QCOMPARE(store->sessionStats().committedTransactions - requests.committedTransactions, quint64(history || failure ? 0 : 1));
    QByteArray actual(128 * 2 * bpp, Qt::Uninitialized), expected(actual.size(), char(history ? 0x41 : 0x31));
    if (!failure) for (int y = 0; y < 2; ++y) for (int x : {0,64}) memset(expected.data() + (y * 128 + x) * bpp, 0x71 + y, size_t(bpp));
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 128, 2); QCOMPARE(actual, expected);
    auto old = beforeView.readResidentPage({{1}, {0,0}}); QVERIFY(old.isValid());
    QCOMPARE(static_cast<const quint8 *>(old.data())[0], quint8(history ? 0x41 : 0x31));
    old = {}; beforeView = {};
    if (failure == 3) { held = {}; QVERIFY(foreign.cancel()); QVERIFY(store->abort(foreignTx)); }
    if (history) {
        dm.commit(); dm.rollback(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 128, 2); QCOMPARE(actual, QByteArray(actual.size(), char(0x31)));
        dm.rollforward(memento); dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 128, 2); QCOMPARE(actual, expected);
        dm.purgeHistory(memento);
    }
    // Reservations and failed segments cannot poison the next operation.
    QCOMPARE(dm.writePageStoreOperation({QRect(0,0,128,2)}, [](KisPixelWriteCursor *cursor) {
        cursor->moveTo(64,1); if (!cursor->rawData()) return false; *cursor->rawData() = 0x75; return true;
    }), KisPageStoreWriteOperationResult::Succeeded);
}

void KisTiledDataManagerTest::testPageStorePixelOperationBorrowed()
{
    quint8 blank = 0;
    KisDataManager dm(1, &blank);
    bool nestedCalled = false;
    auto enclosing = dm.m_pageStoreBackend->beginMutationBatch(); QVERIFY(enclosing);
    QCOMPARE(dm.writePageStoreOperation({QRect(0,0,128,2)}, [&](KisPixelWriteCursor *) { nestedCalled = true; return true; }),
        KisPageStoreWriteOperationResult::Failed);
    QVERIFY(!nestedCalled); QVERIFY(enclosing->finish()); enclosing.reset();
    auto tile = dm.getTile(0,0,true); tile->lockForWrite();
    auto *pointer = tile->data(); QVERIFY(pointer); *pointer = 0x31;
    bool called = false;
    QCOMPARE(dm.writePageStoreOperation({QRect(0,0,128,2)}, [&](KisPixelWriteCursor *) { called = true; return true; }),
        KisPageStoreWriteOperationResult::Borrowed);
    QVERIFY(!called); QCOMPARE(tile->data(), pointer); *pointer = 0x41; tile->unlockForWrite();
    auto foreignTile = dm.getTile(2,0,true); foreignTile->lockForWrite();
    auto *foreignPointer = foreignTile->data(); QVERIFY(foreignPointer); *foreignPointer = 0x77;
    QCOMPARE(dm.writePageStoreOperation({QRect(0,0,128,2)}, [&](KisPixelWriteCursor *cursor) {
        cursor->moveTo(0,0); if (!cursor->rawData()) return false; *cursor->rawData() = 0x51;
        bool nested = false;
        // A legacy borrower elsewhere in the range must not hide same-thread
        // native reentry (regardless of target-set iteration order).
        const auto result = dm.writePageStoreOperation({QRect(0,0,192,1)}, [&](KisPixelWriteCursor *) { nested = true; return true; });
        return result == KisPageStoreWriteOperationResult::Failed && !nested;
    }), KisPageStoreWriteOperationResult::Succeeded);
    QCOMPARE(foreignTile->data(), foreignPointer); QCOMPARE(*foreignPointer, quint8(0x77)); foreignTile->unlockForWrite();
    quint8 actual = 0; dm.readBytes(&actual,0,0,1,1); QCOMPARE(actual, quint8(0x51));
    dm.readBytes(&actual,64,0,1,1); QCOMPARE(actual, quint8(0));
    dm.readBytes(&actual,128,0,1,1); QCOMPARE(actual, quint8(0x77));
}

void KisTiledDataManagerTest::testPageStoreCancelledPrivateClient_data()
{
    QTest::addColumn<bool>("compatibilityBatch");
    QTest::newRow("operation") << false;
    QTest::newRow("compatibility-batch") << true;
}

void KisTiledDataManagerTest::testPageStoreCancelledPrivateClient()
{
    QFETCH(bool, compatibilityBatch);
    quint8 blank = 0;
    KisDataManager dm(1, &blank);
    auto *backend = dm.m_pageStoreBackend;
    auto foreignTile = dm.getTile(2,0,true); foreignTile->lockForWrite();
    bool locked = true;
    const auto cleanup = qScopeGuard([&] { if (locked) foreignTile->unlockForWrite(); });
    auto *pointer = foreignTile->data(); QVERIFY(pointer); *pointer = 0x77;
    // The external client holds the anonymous transaction open across two
    // distinct operations. Cancellation must remove only the private delta.
    QCOMPARE(dm.writePageStoreOperation({QRect(0,0,1,1)}, [](KisPixelWriteCursor *cursor) {
        cursor->moveTo(0,0); if (!cursor->rawData()) return false; *cursor->rawData() = 0x51; return true;
    }), KisPageStoreWriteOperationResult::Succeeded);
    if (compatibilityBatch) {
        auto batch = backend->beginMutationBatch(); QVERIFY(batch);
        auto lease = backend->acquireTile(1,0,true,false); QVERIFY(lease); QVERIFY(lease->tileData());
        lease->tileData()->data()[0] = 0x62; lease->markDirty(); QVERIFY(lease->finish()); lease.reset();
        QVERIFY(!batch->cancel()); batch.reset();
    } else {
        QCOMPARE(dm.writePageStoreOperation({QRect(64,0,1,1)}, [](KisPixelWriteCursor *cursor) {
            cursor->moveTo(64,0); if (!cursor->rawData()) return false; *cursor->rawData() = 0x62; return false;
        }), KisPageStoreWriteOperationResult::Failed);
    }
    const bool intact = foreignTile->data() == pointer && *pointer == 0x77;
    foreignTile->unlockForWrite(); locked = false;
    QVERIFY(intact);
    quint8 actual[129]; dm.readBytes(actual,0,0,129,1);
    QCOMPARE(actual[0], quint8(0x51)); QCOMPARE(actual[64], quint8(0)); QCOMPARE(actual[128], quint8(0x77));
    QCOMPARE(backend->store()->sessionStats().committedTransactions, quint64(1));
}

void KisTiledDataManagerTest::testPageStorePixelOperationConcurrent_data()
{
    QTest::addColumn<bool>("cancelFirst");
    QTest::addColumn<int>("secondKind");
    // 0: pixel cursor; 1: packed different input; 2: packed input equal to the
    // initial before-image. Kind 2 must compare only after the range wait:
    // publication makes it a real write, cancellation leaves it a no-op.
    for (bool cancel : {false, true}) for (int kind : {0,1,2})
        QTest::newRow(qPrintable(QStringLiteral("cancel%1-kind%2").arg(cancel).arg(kind))) << cancel << kind;
}

void KisTiledDataManagerTest::testPageStorePixelOperationConcurrent()
{
    QFETCH(bool, cancelFirst);
    QFETCH(int, secondKind);
    quint8 blank = 0x13, initial = 0x31;
    KisDataManager dm(1, &blank);
    dm.clear(0,0,128,64, &initial);
    auto *store = dm.m_pageStoreBackend->store();
    const auto counts = store->mutationStatistics();
    QSemaphore firstInside, releaseFirst, secondWaiting;
    using Result = KisPageStoreWriteOperationResult;
    Result firstResult = Result::Failed, secondResult = Result::Failed;
    bool firstPixels = false, secondPixels = false;
    std::thread first([&] {
        firstResult = dm.writePageStoreOperation({QRect(0,0,128,1)}, [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(64,0); if (!cursor->rawData()) return false; *cursor->rawData() = 0x51;
            cursor->moveTo(0,0); if (!cursor->rawData()) return false; *cursor->rawData() = 0x51;
            firstPixels = true; firstInside.release(); releaseFirst.acquire(); return !cancelFirst;
        });
    });
    const bool entered = firstInside.tryAcquire(1,5000);
    if (!entered) { releaseFirst.release(); first.join(); QVERIFY(entered); return; }
    std::thread second([&] {
        KisPageStoreDiagnosticRecorder recorder(true, store);
        recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
            if (phase == KisPageStoreDiagnosticPhase::PixelOperationRangeWait) secondWaiting.release();
        });
        if (secondKind) {
            const quint8 input = secondKind == 2 ? initial : 0x72;
            dm.writeBytes(&input, 0, 0, 1, 1);
            secondResult = KisPageStoreWriteOperationResult::Succeeded;
            secondPixels = true;
        } else secondResult = dm.writePageStoreOperation({QRect(64,0,64,1), QRect(0,0,64,1)}, [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(0,0); if (!cursor->rawData()) return false; *cursor->rawData() = 0x72; secondPixels = true; return true;
        });
    });
    const bool waited = secondWaiting.tryAcquire(1,5000);
    auto view = dm.m_pageStoreBackend->captureReadView();
    auto read = view.readResidentPage({{1},{0,0}});
    const bool hidden = read.isValid() && *static_cast<const quint8 *>(read.data()) == initial;
    read = {}; view = {};
    releaseFirst.release(); first.join(); second.join();
    QVERIFY(waited); QVERIFY(hidden); QVERIFY(firstPixels && secondPixels);
    QCOMPARE(firstResult, cancelFirst ? Result::Failed : Result::Succeeded);
    QCOMPARE(secondResult, Result::Succeeded);
    quint8 actual[128]; dm.readBytes(actual,0,0,128,1);
    QCOMPARE(actual[0], quint8(secondKind == 2 ? initial : 0x72));
    QCOMPARE(actual[64], quint8(cancelFirst ? initial : 0x51));
    QCOMPARE(store->mutationStatistics().generationsReserved - counts.generationsReserved,
             quint64(secondKind == 2 && cancelFirst ? 2 : 3));
}

void KisTiledDataManagerTest::testPageStorePlanarWriteMutation_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("partial"); QTest::addColumn<bool>("history");
    for (int bpp : {1, 4, 8, 16}) for (bool partial : {false, true}) for (bool history : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("%1-partial%2-history%3").arg(bpp).arg(partial).arg(history)))
            << bpp << partial << history;
}

void KisTiledDataManagerTest::testPageStorePlanarWriteMutation()
{
    QFETCH(int, bpp); QFETCH(bool, partial); QFETCH(bool, history);
    const QByteArray blank(bpp, char(0x13));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    const QRect area(-65, -33, 131, 99);
    const QByteArray original(area.width() * area.height() * bpp, char(0x35));
    dm.writeBytes(reinterpret_cast<const quint8 *>(original.constData()), area.x(), area.y(), area.width(), area.height());
    auto *store = dm.m_pageStoreBackend->store(); QVERIFY(store);
    auto beforeView = store->captureReadView();
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    const QVector<qint32> channels = bpp == 1 ? QVector<qint32>{1} : QVector<qint32>{1, 0, bpp - 2, 1};
    QVector<QByteArray> storage;
    QVector<quint8 *> planes;
    QByteArray expected = original;
    int offset = 0;
    for (int channel = 0; channel < channels.size(); ++channel) {
        const int size = channels[channel];
        storage.append(QByteArray(area.width() * area.height() * size, char(0)));
        auto &bytes = storage.last();
        for (int p = 0; p < area.width() * area.height(); ++p) for (int c = 0; c < size; ++c)
            bytes[p * size + c] = char((p * 11 + channel * 17 + c) % 251);
        const bool present = !partial || (channel == 0 && bpp != 1);
        planes.append(present ? reinterpret_cast<quint8 *>(bytes.data()) : nullptr);
        if (present) for (int p = 0; p < area.width() * area.height(); ++p)
            memcpy(expected.data() + p * bpp + offset, bytes.constData() + p * size, size_t(size));
        offset += size;
    }
    const auto counts = store->mutationStatistics(); const auto requests = store->sessionStats();
    dm.writePlanarBytes(planes, channels, area.x(), area.y(), area.width(), area.height());
    const auto after = store->mutationStatistics();
    const bool noOp = partial && bpp == 1;
    QCOMPARE(after.sessionsCreated - counts.sessionsCreated, quint64(noOp ? 0 : 1));
    QCOMPARE(after.generationsReserved - counts.generationsReserved, quint64(noOp ? 0 : 12));
    QCOMPARE(after.pagesSealed - counts.pagesSealed, quint64(noOp ? 0 : 12));
    QCOMPARE(after.writablePinsAcquired - counts.writablePinsAcquired, quint64(noOp ? 0 : 12));
    QCOMPARE(after.maximumPinnedPagesPerSegment, quint64(1));
    QCOMPARE(store->sessionStats().writeRequestsCreated, requests.writeRequestsCreated);
    QCOMPARE(store->sessionStats().committedTransactions - requests.committedTransactions, quint64(history || noOp ? 0 : 1));
    QByteArray actual(original.size(), Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), area.x(), area.y(), area.width(), area.height());
    QCOMPARE(actual, expected);
    // Retained root remains the pre-operation image even after the new root.
    auto old = beforeView.readResidentPage({{1}, {-1, 0}}); QVERIFY(old.isValid());
    QCOMPARE(static_cast<const quint8 *>(old.data())[0], quint8(0x35));
    old = {}; beforeView = {};
    if (history) {
        dm.commit(); dm.rollback(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), area.x(), area.y(), area.width(), area.height());
        QCOMPARE(actual, original);
        dm.rollforward(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), area.x(), area.y(), area.width(), area.height());
        QCOMPARE(actual, expected); dm.purgeHistory(memento);
    }
}

void KisTiledDataManagerTest::testPageStoreTrimMutation_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("history"); QTest::addColumn<QRect>("kept");
    for (int bpp : {1, 4, 8, 16}) for (bool history : {false, true}) {
        const auto prefix = QStringLiteral("%1-history%2-").arg(bpp).arg(history);
        QTest::newRow(qPrintable(prefix + "four-bands")) << bpp << history << QRect(3, 4, 51, 49);
        QTest::newRow(qPrintable(prefix + "multiple-pages")) << bpp << history << QRect(-31, -25, 130, 101);
        QTest::newRow(qPrintable(prefix + "whole-removals")) << bpp << history << QRect(0, 0, 64, 64);
        QTest::newRow(qPrintable(prefix + "no-op")) << bpp << history << QRect(-128, -128, 512, 512);
    }
}

void KisTiledDataManagerTest::testPageStorePlanarWriteCoordinateLimits()
{
    // This product entry still uses the packed-key compatibility tile index.
    // Its range is narrower than canonical PageStore's logical coordinates.
    const int minimum = -0x7ffe * 64, maximum = (0x7ffe + 1) * 64 - 1;
    const QRect areas[] = {QRect(minimum, 0, 67, 3), QRect(maximum - 66, 0, 67, 3),
                           QRect(0, minimum, 3, 67), QRect(0, maximum - 66, 3, 67)};
    for (const auto &area : areas) {
        const QByteArray blank(4, char(0x13));
        KisDataManager dm(4, reinterpret_cast<const quint8 *>(blank.constData()));
        const QVector<qint32> channels{1, 2, 1};
        QVector<QByteArray> storage; QVector<quint8 *> planes;
        QByteArray expected(area.width() * area.height() * 4, char(0));
        int offset = 0;
        for (int channel = 0; channel < channels.size(); ++channel) {
            const int size = channels[channel];
            storage.append(QByteArray(area.width() * area.height() * size, char(0)));
            auto &bytes = storage.last();
            for (int i = 0; i < bytes.size(); ++i) bytes[i] = char((i + channel * 31) % 251);
            planes.append(reinterpret_cast<quint8 *>(bytes.data()));
            for (int p = 0; p < area.width() * area.height(); ++p)
                memcpy(expected.data() + p * 4 + offset, bytes.constData() + p * size, size_t(size));
            offset += size;
        }
        dm.writePlanarBytes(planes, channels, area.x(), area.y(), area.width(), area.height());
        auto *store = dm.m_pageStoreBackend->store();
        const auto stats = store->mutationStatistics();
        QCOMPARE(stats.generationsReserved, quint64(2)); QCOMPARE(stats.maximumPinnedPagesPerSegment, quint64(1));
        QByteArray actual; QString error;
        QVERIFY2(KisPageStoreCpuSurfaceOps::readRect(store, {1}, {}, area, &actual, 0, &error), qPrintable(error));
        QCOMPARE(actual, expected);
    }
    const int intMin = std::numeric_limits<int>::min(), intMax = std::numeric_limits<int>::max();
    const QRect rejected[] = {QRect(intMin, 0, 67, 3), QRect(intMax - 66, 0, 67, 3),
                              QRect(0, intMin, 3, 67), QRect(0, intMax - 66, 3, 67)};
    for (const auto &area : rejected) {
        if (KisTileHashTable::supportsCoordinates(area.x() / 64, area.y() / 64)) continue;
        const quint8 blank = 0x13;
        KisDataManager dm(1, &blank); quint8 unused = 0;
        QTest::ignoreMessage(QtWarningMsg, "Planar write exceeds the compatibility tile index coordinate range");
        dm.writePlanarBytes({&unused}, {1}, area.x(), area.y(), area.width(), area.height());
        const auto stats = dm.m_pageStoreBackend->store()->sessionStats();
        QCOMPARE(stats.registeredPages, qsizetype(0)); QCOMPARE(stats.committedTransactions, quint64(0));
        QCOMPARE(dm.m_pageStoreBackend->store()->mutationStatistics().sessionsCreated, quint64(0));
        QCOMPARE(dm.extent(), QRect());
    }
}

void KisTiledDataManagerTest::testPageStorePlanarWriteNoOpInputs()
{
    const quint8 blank[4] = {0x13, 0x13, 0x13, 0x13};
    KisDataManager dm(4, blank);
    auto *store = dm.m_pageStoreBackend->store();
    const auto before = store->sessionStats();
    const auto mutations = store->mutationStatistics();
    const auto views = store->readScopeStatistics();
    quint8 byte = 0;
    dm.writePlanarBytes({nullptr, nullptr}, {1, 3}, -65, -33, 131, 99);
    dm.writePlanarBytes({&byte, nullptr}, {0, 4}, -65, -33, 131, 99);
    dm.writePlanarBytes({}, {}, 0, 0, 0, 99);
    dm.writePlanarBytes({}, {}, 0, 0, 99, -1);
    const auto after = store->sessionStats();
    QCOMPARE(after.registeredPages, before.registeredPages);
    QCOMPARE(after.committedTransactions, before.committedTransactions);
    QCOMPARE(after.writeRequestsCreated, before.writeRequestsCreated);
    QCOMPARE(store->mutationStatistics().sessionsCreated, mutations.sessionsCreated);
    QCOMPARE(store->readScopeStatistics().capturedReadViewsCreated, views.capturedReadViewsCreated);
    QCOMPARE(dm.extent(), QRect());
}

void KisTiledDataManagerTest::testPageStoreTrimMutation()
{
    QFETCH(int, bpp); QFETCH(bool, history); QFETCH(QRect, kept);
    const QByteArray blank(bpp, char(0x13)), color(bpp, char(0x47));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    const QRect area(-64, -64, 256, 192);
    dm.clear(area.x(), area.y(), area.width(), area.height(), reinterpret_cast<const quint8 *>(color.constData()));
    auto *store = dm.m_pageStoreBackend->store(); QVERIFY(store);
    auto beforeView = store->captureReadView();
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    const auto counts = store->mutationStatistics(); const auto requests = store->sessionStats();
    const auto payload = dm.m_pageStoreBackend->payloadWork();
    quint64 partialPages = 0;
    for (int y = area.top(); y <= area.bottom(); y += 64) for (int x = area.left(); x <= area.right(); x += 64) {
        const QRect page(x, y, 64, 64); const QRect overlap = page.intersected(kept);
        if (!overlap.isEmpty() && overlap != page) ++partialPages;
    }
    dm.setExtent(kept);
    const auto after = store->mutationStatistics();
    QCOMPARE(after.sessionsCreated - counts.sessionsCreated, quint64(partialPages ? 1 : 0));
    QCOMPARE(after.generationsReserved - counts.generationsReserved, partialPages);
    QCOMPARE(after.pagesSealed - counts.pagesSealed, partialPages);
    QCOMPARE(after.writablePinsAcquired - counts.writablePinsAcquired, partialPages);
    QCOMPARE(after.maximumPinnedPagesPerSegment, quint64(partialPages ? 1 : 0));
    QCOMPARE(dm.m_pageStoreBackend->payloadWork().nativeDuplicatePages - payload.nativeDuplicatePages, partialPages);
    QCOMPARE(store->sessionStats().writeRequestsCreated, requests.writeRequestsCreated);
    const bool noOp = kept.contains(area);
    QCOMPARE(store->sessionStats().committedTransactions - requests.committedTransactions, quint64(history || noOp ? 0 : 1));
    QByteArray expected(area.width() * area.height() * bpp, char(0x13));
    for (int y = 0; y < area.height(); ++y) for (int x = 0; x < area.width(); ++x)
        if (kept.contains(area.topLeft() + QPoint(x, y))) memset(expected.data() + (y * area.width() + x) * bpp, 0x47, size_t(bpp));
    QByteArray actual(expected.size(), Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), area.x(), area.y(), area.width(), area.height());
    QCOMPARE(actual, expected);
    auto old = beforeView.readResidentPage({{1}, {0, 0}}); QVERIFY(old.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(old.data()), int(old.byteSize())), QByteArray(int(old.byteSize()), char(0x47)));
    old = {}; beforeView = {};
    if (history) {
        dm.commit(); dm.rollback(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), area.x(), area.y(), area.width(), area.height());
        QCOMPARE(actual, QByteArray(actual.size(), char(0x47)));
        dm.rollforward(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), area.x(), area.y(), area.width(), area.height());
        QCOMPARE(actual, expected); dm.purgeHistory(memento);
    }
}

void KisTiledDataManagerTest::testPageStoreRegionWriteFailure_data()
{
    QTest::addColumn<bool>("history"); QTest::addColumn<bool>("planar");
    QTest::addColumn<bool>("mixed");
    for (bool history : {false, true}) for (bool planar : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("history%1-planar%2").arg(history).arg(planar))) << history << planar << false;
    for (bool history : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("history%1-mixed-trim").arg(history))) << history << false << true;
}

void KisTiledDataManagerTest::testPageStoreRegionWriteFailure()
{
    QFETCH(bool, history); QFETCH(bool, planar); QFETCH(bool, mixed);
    const quint8 blank = 0x13, initial = 0x31, staged = 0x41;
    KisDataManager dm(1, &blank);
    const int width = mixed ? 192 : 128;
    dm.clear(0, 0, width, 64, &initial);
    auto *backend = dm.m_pageStoreBackend; auto *store = backend->store();
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    if (history) dm.clear(0, 0, width, 64, &staged);
    const auto otherTx = store->beginCurrentTransaction();
    auto other = store->beginMutation(otherTx); QVERIFY(other.isActive());
    auto held = other.beginWrite({{1}, {mixed ? 2 : 1, 0}}); QVERIFY(held.isValid());
    // The real production operation can modify page zero, but must fail on
    // page one without cancelling the foreign writer or exposing page zero.
    const auto before = store->mutationStatistics();
    const auto commits = store->sessionStats().committedTransactions;
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression("^PageStore CPU mutation acquisition failed:.*legacy mutation cannot reenter its page.*$"));
    if (planar) {
        QByteArray input(128 * 64, char(0x71));
        QTest::ignoreMessage(QtWarningMsg, "PageStore planar write failed");
        dm.writePlanarBytes({reinterpret_cast<quint8 *>(input.data())}, {1}, 0, 0, 128, 64);
    } else {
        QString error;
        QVERIFY(!backend->trimToRect(QRect(mixed ? 65 : 1, 1, 126, 62), &error)); QVERIFY(!error.isEmpty());
    }
    QCOMPARE(store->mutationStatistics().generationsReserved - before.generationsReserved, quint64(1));
    QCOMPARE(store->mutationStatistics().pagesCancelled - before.pagesCancelled, quint64(1));
    QCOMPARE(store->mutationStatistics().removalsCancelled - before.removalsCancelled, quint64(mixed ? 1 : 0));
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(1)); // foreign claim intact
    QCOMPARE(store->sessionStats().committedTransactions, commits);
    QByteArray actual(width * 64, Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, width, 64);
    QCOMPARE(actual, QByteArray(actual.size(), char(history ? staged : initial)));
    QCOMPARE(static_cast<const quint8 *>(held.data())[0], initial);
    held = {}; QVERIFY(other.cancel()); QVERIFY(store->abort(otherTx));
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(0));
    if (history) {
        dm.commit(); dm.rollback(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, width, 64);
        QCOMPARE(actual, QByteArray(actual.size(), char(initial)));
        dm.rollforward(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, width, 64);
        QCOMPARE(actual, QByteArray(actual.size(), char(staged))); dm.purgeHistory(memento);
    }
}

void KisTiledDataManagerTest::testPageStoreSemanticRemovalFailure_data()
{
    QTest::addColumn<bool>("history"); QTest::addColumn<bool>("clearAll");
    for (bool history : {false, true}) for (bool all : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("history%1-clearAll%2").arg(history).arg(all))) << history << all;
}

void KisTiledDataManagerTest::testPageStoreSemanticRemovalFailure()
{
    QFETCH(bool, history); QFETCH(bool, clearAll);
    const quint8 blank = 0x13, initial = 0x31, staged = 0x41;
    KisDataManager dm(1, &blank); dm.clear(0, 0, 128, 64, &initial);
    auto *backend = dm.m_pageStoreBackend; auto *store = backend->store();
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    if (history) dm.clear(0, 0, 128, 64, &staged);
    auto beforeView = backend->captureReadView(); QVERIFY(beforeView.isValid());
    const auto tx = store->beginCurrentTransaction(); auto other = store->beginMutation(tx);
    auto held = other.beginWrite({{1}, {1, 0}}); QVERIFY(held.isValid());
    const auto before = store->mutationStatistics(); const auto commits = store->sessionStats().committedTransactions;
    QString error;
    QVERIFY(!(clearAll ? backend->clearAll(&error) : backend->removePages({{0, 0}, {1, 0}}, &error)));
    QVERIFY(!error.isEmpty());
    const auto after = store->mutationStatistics();
    QCOMPARE(after.removalsCancelled - before.removalsCancelled, quint64(1));
    QCOMPARE(after.generationsReserved, before.generationsReserved);
    QCOMPARE(after.sessionsCreated, before.sessionsCreated);
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(1));
    QCOMPARE(store->sessionStats().committedTransactions, commits);
    QByteArray actual(128 * 64, Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 128, 64);
    QCOMPARE(actual, QByteArray(actual.size(), char(history ? staged : initial)));
    held = {}; QVERIFY(other.cancel()); QVERIFY(store->abort(tx));
    QVERIFY(clearAll ? backend->clearAll(&error) : backend->removePages({{0, 0}, {1, 0}}, &error));
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 128, 64);
    QCOMPARE(actual, QByteArray(actual.size(), char(blank)));
    auto old = beforeView.readResidentPage({{1}, {0, 0}}); QVERIFY(old.isValid());
    QCOMPARE(static_cast<const quint8 *>(old.data())[0], history ? staged : initial);
    if (history) {
        dm.commit(); dm.rollback(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 128, 64);
        QCOMPARE(actual, QByteArray(actual.size(), char(initial)));
        dm.rollforward(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 128, 64);
        QCOMPARE(actual, QByteArray(actual.size(), char(blank))); dm.purgeHistory(memento);
    }
}

void KisTiledDataManagerTest::testPageStoreAliasRegionFailure_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("history");
    QTest::addColumn<bool>("copy"); QTest::addColumn<bool>("removal");
    for (int bpp : {1, 4, 8, 16}) for (bool history : {false, true})
        for (bool copy : {false, true}) for (bool removal : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-history%2-copy%3-removal%4")
                .arg(bpp).arg(history).arg(copy).arg(removal))) << bpp << history << copy << removal;
}

void KisTiledDataManagerTest::testPageStoreAliasRegionFailure()
{
    QFETCH(int, bpp); QFETCH(bool, history); QFETCH(bool, copy); QFETCH(bool, removal);
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31)), staged(bpp, char(0x41));
    const QByteArray changedPixel(bpp, char(removal ? 0x13 : 0x71));
    const auto pixel = [](const QByteArray &a) { return reinterpret_cast<const quint8 *>(a.constData()); };
    KisDataManager dm(bpp, pixel(blank)), source(bpp, pixel(blank));
    dm.clear(0, 0, 192, 64, pixel(initial));
    if (!removal) source.clear(0, 0, 192, 64, pixel(changedPixel));
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    if (history) dm.clear(0, 0, 192, 64, pixel(staged));
    auto *backend = dm.m_pageStoreBackend; auto *store = backend->store();
    auto old = backend->captureReadView(); QVERIFY(old.isValid());
    const auto tx = store->beginCurrentTransaction(); auto other = store->beginMutation(tx);
    auto held = other.beginWrite({{1}, {2, 0}}); QVERIFY(held.isValid());
    const auto before = store->mutationStatistics(); const auto commits = store->sessionStats().committedTransactions;
    const QRect area(1, 0, 128, 64); // partial pixel page, semantic page, failing partial page
    QVector<KisLogicalPageId> changes{{99, 99}};
    QString error;
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression("^PageStore CPU mutation acquisition failed:.*legacy mutation cannot reenter its page.*$"));
    QVERIFY(!(copy ? backend->copyFrom(*source.m_pageStoreBackend, area, false, false, &error, &changes)
                  : backend->fillRect(area, changedPixel, &error, &changes)));
    QVERIFY(changes.isEmpty());
    const auto after = store->mutationStatistics();
    QCOMPARE(after.generationsReserved - before.generationsReserved, quint64(1));
    QCOMPARE(after.pagesCancelled - before.pagesCancelled, quint64(1));
    QCOMPARE(after.removalsCancelled - before.removalsCancelled, quint64(removal ? 1 : 0));
    QCOMPARE(after.aliasGenerationsReserved, before.aliasGenerationsReserved); // staged only, no adoption
    QCOMPARE(store->sessionStats().committedTransactions, commits);
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(1)); // foreign writer not cancelled
    QByteArray actual(192 * 64 * bpp, Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 192, 64);
    QCOMPARE(actual, QByteArray(actual.size(), char(history ? 0x41 : 0x31)));
    held = {}; QVERIFY(other.cancel()); QVERIFY(store->abort(tx));
    const auto requests = store->sessionStats();
    KisPageStoreDiagnosticRecorder recorder(true, store);
    if (copy) dm.KisTiledDataManager::bitBlt(&source, area);
    else dm.clear(area.x(), area.y(), area.width(), area.height(), pixel(changedPixel));
    // Only the three actually changed pages refresh the compatibility index;
    // native cache refresh must not manufacture generic read requests.
    const auto &refresh = recorder.metrics()[size_t(KisPageStoreDiagnosticPhase::MutationIndexRefresh)];
    QCOMPARE(refresh.intervals, quint64(1)); QCOMPARE(refresh.workItems, quint64(3));
    QCOMPARE(store->sessionStats().readRequestsCreated, requests.readRequestsCreated);
    QCOMPARE(store->sessionStats().writeRequestsCreated, requests.writeRequestsCreated);
    QByteArray expected(actual.size(), char(history ? 0x41 : 0x31));
    for (int y = 0; y < 64; ++y) memset(expected.data() + (y * 192 + 1) * bpp,
        quint8(changedPixel[0]), size_t(128 * bpp));
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 192, 64); QCOMPARE(actual, expected);
    for (int column = 0; column < 3; ++column) {
        auto previous = old.readResidentPage({{1}, {column, 0}}); QVERIFY(previous.isValid());
        QCOMPARE(static_cast<const quint8 *>(previous.data())[0], quint8(history ? 0x41 : 0x31));
    }
    old = {};
    if (history) {
        dm.commit(); dm.rollback(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 192, 64);
        QCOMPARE(actual, QByteArray(actual.size(), char(0x31)));
        dm.rollforward(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 192, 64); QCOMPARE(actual, expected);
        dm.purgeHistory(memento);
    }
}

void KisTiledDataManagerTest::testPageStoreCopyFixedView_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("oldSource"); QTest::addColumn<bool>("rough");
    for (int bpp : {1, 4, 8, 16}) for (bool oldSource : {false, true}) for (bool rough : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("bpp%1-old%2-rough%3").arg(bpp).arg(oldSource).arg(rough)))
            << bpp << oldSource << rough;
}

void KisTiledDataManagerTest::testPageStoreCopyFixedView()
{
    QFETCH(int, bpp); QFETCH(bool, oldSource); QFETCH(bool, rough);
    const QByteArray blank(bpp, char(0x13)), original(bpp, char(0x31)), staged(bpp, char(0x41)), target(bpp, char(0x47));
    const auto pixel = [](const QByteArray &a) { return reinterpret_cast<const quint8 *>(a.constData()); };
    KisDataManager source(bpp, pixel(blank)), dm(bpp, pixel(blank));
    source.clear(0, 0, 128, 64, pixel(original));
    const auto history = source.getMemento(); source.clear(0, 0, 128, 64, pixel(staged));
    dm.clear(0, 0, 256, 64, pixel(target));
    auto *store = dm.m_pageStoreBackend->store();
    const auto requests = store->sessionStats();
    const auto sourceRequests = source.m_pageStoreBackend->store()->sessionStats();
    int visits = 0; bool injected = false; QString error;
    KisPageStoreDiagnosticRecorder recorder(true, store);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::CopySourcePage || ++visits != 2) return;
        injected = source.m_pageStoreBackend->fillRect(QRect(0, 0, 256, 64), QByteArray(bpp, char(0x61)), &error) &&
            source.m_pageStoreBackend->setDefaultPixel(QByteArray(bpp, char(0x29)), &error);
    });
    const QRect area(1, 0, 192, 64);
    if (rough) {
        if (oldSource) dm.KisTiledDataManager::bitBltRoughOldData(&source, area);
        else dm.KisTiledDataManager::bitBltRough(&source, area);
    } else {
        if (oldSource) dm.KisTiledDataManager::bitBltOldData(&source, area);
        else dm.KisTiledDataManager::bitBlt(&source, area);
    }
    QVERIFY2(injected, qPrintable(error)); QCOMPARE(visits, 4);
    QCOMPARE(store->sessionStats().committedTransactions - requests.committedTransactions, quint64(1));
    QCOMPARE(store->sessionStats().readRequestsCreated, requests.readRequestsCreated);
    QCOMPARE(store->sessionStats().writeRequestsCreated, requests.writeRequestsCreated);
    QCOMPARE(source.m_pageStoreBackend->store()->sessionStats().readRequestsCreated, sourceRequests.readRequestsCreated);
    QByteArray actual(256 * 64 * bpp, Qt::Uninitialized), expected(actual.size(), Qt::Uninitialized);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 256; ++x) {
        const quint8 value = (!rough && !area.contains(x, y)) ? 0x47 : x < 128 ? (oldSource ? 0x31 : 0x41) : 0x13;
        memset(expected.data() + (y * 256 + x) * bpp, value, size_t(bpp));
    }
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 256, 64); QCOMPARE(actual, expected);
    // A later source mutation and history purge cannot mutate a retained alias.
    source.commit(); source.purgeHistory(history); source.clear();
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 256, 64); QCOMPARE(actual, expected);
}

void KisTiledDataManagerTest::testPageStoreCopyLiveWriteCompatibility_data()
{
    QTest::addColumn<bool>("history"); QTest::addColumn<bool>("oldSource");
    QTest::addColumn<bool>("rough"); QTest::addColumn<bool>("exposed");
    for (bool history : {false, true}) for (bool oldSource : {false, true})
        for (bool rough : {false, true}) for (bool exposed : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("history%1-old%2-rough%3-exposed%4")
                .arg(history).arg(oldSource).arg(rough).arg(exposed))) << history << oldSource << rough << exposed;
}

void KisTiledDataManagerTest::testPageStoreCopyLiveWriteCompatibility()
{
    QFETCH(bool, history); QFETCH(bool, oldSource); QFETCH(bool, rough); QFETCH(bool, exposed);
    const quint8 blank = 0, initial = 0x31;
    KisTiledDataManager source(1, &blank), target(1, &blank);
    const QRect area(0, 0, 64, 64); source.clear(area, &initial);
    const auto memento = history ? source.getMemento() : KisMementoSP{};
    {
        auto tile = source.getTile(0, 0, true); tile->lockForWrite();
        const auto unlock = qScopeGuard([&] { tile->unlockForWrite(); });
        if (exposed) tile->data()[0] = 0x71;
        const auto views = source.m_pageStoreBackend->store()->readScopeStatistics().capturedReadViewsCreated;
        if (rough) {
            if (oldSource) target.bitBltRoughOldData(&source, area); else target.bitBltRough(&source, area);
        } else {
            if (oldSource) target.bitBltOldData(&source, area); else target.bitBlt(&source, area);
        }
        const bool immutableBefore = history && oldSource;
        QCOMPARE(source.m_pageStoreBackend->store()->readScopeStatistics().capturedReadViewsCreated - views,
                 quint64(immutableBefore ? 1 : 0));
        auto result = target.getTile(0, 0, false); result->lockForRead();
        const auto unlockResult = qScopeGuard([&] { result->unlockForRead(); });
        QCOMPARE(result->data()[0], quint8(immutableBefore || !exposed ? 0x31 : 0x71));
        if (!immutableBefore) QCOMPARE(result->tileData(), tile->tileData());
        // This is explicitly a legacy live-source observation, not a claim
        // that an exposed mutable producer has become an immutable source.
    }
    if (history) { source.commit(); source.purgeHistory(memento); }
}

void KisTiledDataManagerTest::testPageStoreCompatibilityCopyFailure_data()
{
    QTest::addColumn<bool>("rough");
    QTest::addColumn<int>("failurePoint");
    for (bool rough : {false, true}) {
        for (KisSwapInFailurePoint point : {KisSwapInFailurePoint::Mapping,
                                            KisSwapInFailurePoint::Allocation,
                                            KisSwapInFailurePoint::Decompression}) {
            QTest::newRow(qPrintable(QStringLiteral("rough%1-failure%2").arg(rough).arg(int(point))))
                << rough << int(point);
        }
    }
}

void KisTiledDataManagerTest::testPageStoreCompatibilityCopyFailure()
{
    QFETCH(bool, rough);
    QFETCH(int, failurePoint);
    const quint8 blank = 0;
    const quint8 firstSource = 0x61;
    const quint8 secondSource = 0x62;
    const quint8 initialTarget = 0x31;
    KisTiledDataManager source(1, &blank);
    KisTiledDataManager target(1, &blank);

    // Exercise the production compatibility path with a legacy source. Keep
    // its two physical backings distinct so only the second page is swapped.
    source.m_mementoManager->setPageStoreBridge(nullptr);
    delete source.m_pageStoreBackend;
    source.m_pageStoreBackend = nullptr;
    source.clear(0, 0, 64, 64, &firstSource);
    source.clear(64, 0, 64, 64, &secondSource);
    target.clear(0, 0, 128, 64, &initialTarget);

    bool exists = false;
    const auto secondTile = source.getReadOnlyTileLazy(1, 0, exists);
    QVERIFY(exists);
    KisTileData *secondData = secondTile->tileData();
    QVERIFY(secondData->ref());
    const auto release = qScopeGuard([&] {
        secondData->deref();
    });
    auto *tileStore = KisTileDataStore::instance();
    QVERIFY(tileStore->trySwapTileData(secondData));
    QVERIFY(!secondData->isResident());

    const auto committed = target.m_pageStoreBackend->store()->sessionStats().committedTransactions;
    tileStore->testingFailNextSwapIn(KisSwapInFailurePoint(failurePoint));
    const QRect area = rough ? QRect(0, 0, 128, 64) : QRect(1, 0, 126, 64);
    if (rough)
        target.bitBltRough(&source, area);
    else
        target.bitBlt(&source, area);

    QCOMPARE(target.m_pageStoreBackend->store()->sessionStats().committedTransactions, committed);
    QVERIFY(!secondData->isResident());
    const auto firstCached = target.m_hashTable->getExistingTile(0, 0);
    QVERIFY(firstCached);
    QVERIFY(firstCached->tileData()->data());
    QCOMPARE(firstCached->tileData()->data()[rough ? 0 : 1], initialTarget);
    QByteArray actual(128 * 64, Qt::Uninitialized);
    target.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 128, 64);
    QCOMPARE(actual, QByteArray(actual.size(), char(initialTarget)));
}

void KisTiledDataManagerTest::testPageStoreDefaultLifecycleDoesNotMaterialize()
{
    QFETCH(int, pixelSize);
    const QByteArray defaultPixel(pixelSize, char(0x2a));
    const QByteArray paintedPixel(pixelSize, char(0x5d));
    KisTiledDataManager dm(pixelSize,
        reinterpret_cast<const quint8 *>(defaultPixel.constData()));
    KisPageStore *store = dm.m_pageStoreBackend->store();
    QVERIFY(store);
    const auto initial = store->sessionStats();
    const QRect rect(-64, -64, 256, 256);
    for (int cycle = 0; cycle < 4; ++cycle) {
        const KisMementoSP edit = dm.getMemento();
        dm.clear(rect, reinterpret_cast<const quint8 *>(paintedPixel.constData()));
        dm.commit();
        QCOMPARE(dm.extent(), rect);
        const auto written = store->sessionStats();
        dm.rollback(edit);
        QCOMPARE(dm.extent(), QRect());
        QCOMPARE(store->sessionStats().defaultMaterializationRequests,
                 written.defaultMaterializationRequests);
        dm.rollforward(edit);
        QCOMPARE(dm.extent(), rect);
        dm.purgeHistory(edit);

        // Both a trim and a clear-all must publish absence, not new pixels.
        dm.setExtent(QRect(0, 0, 64, 64));
        QCOMPARE(dm.extent(), QRect(0, 0, 64, 64));
        dm.clear();
        QCOMPARE(dm.extent(), QRect());
        bool exists = true;
        KisTileSP tile = dm.getReadOnlyTileLazy(-1, -1, exists);
        QVERIFY(!exists);
        tile->lockForRead();
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(tile->data()), pixelSize),
                 defaultPixel);
        tile->unlockForRead();
        QCOMPARE(store->sessionStats().defaultMaterializationRequests,
                 initial.defaultMaterializationRequests);
    }
    QVERIFY(store->waitForRetirementIdle());
    QCOMPARE(store->sessionStats().replicas, qsizetype(0));
}

void KisTiledDataManagerTest::testPageStoreHistoryClearStagesAbsence_data()
{
    testPageStoreDefaultLifecycleDoesNotMaterialize_data();
}

void KisTiledDataManagerTest::testPageStoreHistoryClearStagesAbsence()
{
    QFETCH(int, pixelSize);
    const QByteArray defaultPixel(pixelSize, char(0x2a));
    const QByteArray paintedPixel(pixelSize, char(0x5d));
    const QByteArray changedPixel(pixelSize, char(0x6e));
    KisTiledDataManager dm(pixelSize,
        reinterpret_cast<const quint8 *>(defaultPixel.constData()));
    KisPageStore *store = dm.m_pageStoreBackend->store();
    QVERIFY(store);
    const QRect initialRect(0, 0, 128, 128);
    dm.clear(initialRect,
             reinterpret_cast<const quint8 *>(paintedPixel.constData()));
    const KisMementoSP edit = dm.getMemento();
    // Include both committed pages and a newly prepared page in clear-all.
    dm.clear(QRect(256, 0, 64, 64),
             reinterpret_cast<const quint8 *>(paintedPixel.constData()));
    const auto beforeClear = store->sessionStats();
    dm.clear();
    const auto afterClear = store->sessionStats();
    QCOMPARE(afterClear.writeRequestsCreated, beforeClear.writeRequestsCreated);
    QCOMPARE(afterClear.defaultMaterializationRequests,
             beforeClear.defaultMaterializationRequests);
    QCOMPARE(dm.extent(), QRect());

    QByteArray readBack(pixelSize, char(0));
    dm.readBytes(reinterpret_cast<quint8 *>(readBack.data()), 1, 1, 1, 1);
    QCOMPARE(readBack, defaultPixel);
    dm.writeBytes(reinterpret_cast<const quint8 *>(changedPixel.constData()),
                   1, 1, 1, 1);
    dm.commit();
    QCOMPARE(dm.extent(), QRect(0, 0, 64, 64));
    dm.readBytes(reinterpret_cast<quint8 *>(readBack.data()), 1, 1, 1, 1);
    QCOMPARE(readBack, changedPixel);
    dm.readBytes(reinterpret_cast<quint8 *>(readBack.data()), 2, 1, 1, 1);
    QCOMPARE(readBack, defaultPixel);
    dm.rollback(edit);
    QCOMPARE(dm.extent(), initialRect);
    dm.readBytes(reinterpret_cast<quint8 *>(readBack.data()), 1, 1, 1, 1);
    QCOMPARE(readBack, paintedPixel);
    dm.rollforward(edit);
    QCOMPARE(dm.extent(), QRect(0, 0, 64, 64));
    dm.readBytes(reinterpret_cast<quint8 *>(readBack.data()), 1, 1, 1, 1);
    QCOMPARE(readBack, changedPixel);
    dm.purgeHistory(edit);
}

void KisTiledDataManagerTest::testPageStoreFixedPageWork_data()
{
    QTest::addColumn<int>("totalPages");
    QTest::addColumn<int>("changedPages");
    QTest::addColumn<int>("retainedRoots");
    QTest::addColumn<int>("pixelSize");
    QTest::addColumn<QString>("mode");
    // Orthogonal N/K/H sweeps, plus wide-format real writes. This is a
    // deterministic diagnostic fixture, not a substitute for paired M/E.
    for (const QString &mode : {QStringLiteral("alias"), QStringLiteral("noop"),
                               QStringLiteral("identical"), QStringLiteral("real")}) {
        for (int n : {64, 1024, 4096}) {
            for (int k : {1, 16}) {
                for (int h : {0, 4}) {
                    const QByteArray tag = QString("%1-N%2-K%3-H%4-B4").arg(mode).arg(n).arg(k).arg(h).toLatin1();
                    QTest::newRow(tag.constData()) << n << k << h << 4 << mode;
                }
            }
        }
    }
    for (int bpp : {8, 16}) {
        for (int h : {0, 4}) {
            const QByteArray tag = QString("real-N1024-K16-H%1-B%2").arg(h).arg(bpp).toLatin1();
            QTest::newRow(tag.constData()) << 1024 << 16 << h << bpp << QStringLiteral("real");
        }
    }
}

void KisTiledDataManagerTest::testPageStoreFixedPageWork()
{
    QFETCH(int, totalPages);
    QFETCH(int, changedPages);
    QFETCH(int, retainedRoots);
    QFETCH(int, pixelSize);
    QFETCH(QString, mode);
    const QByteArray loopsOption = qgetenv("KIS_BR1_FIXED_LOOPS");
    const int loops = loopsOption.isEmpty() ? 4 : loopsOption.toInt();
    QVERIFY(loops > 0 && loops <= 262144);
    const bool stageProfile = qEnvironmentVariableIntValue("KIS_BR1_STAGE_PROFILE") != 0;
    const QByteArray sampleOption = qgetenv("KIS_BR1_STAGE_SAMPLE_PERIOD");
    const int samplePeriod = sampleOption.isEmpty() ? 1 : sampleOption.toInt();
    const int sampleOffset = qEnvironmentVariableIntValue("KIS_BR1_STAGE_SAMPLE_OFFSET");
    QVERIFY(samplePeriod > 0 && sampleOffset >= 0 && sampleOffset < samplePeriod);
    const QByteArray defaultPixel(pixelSize, char(0));
    const QByteArray initialPixel(pixelSize, char(0x11));
    KisTiledDataManager dm(pixelSize, reinterpret_cast<const quint8 *>(defaultPixel.constData()));
    KisPageStore *store = dm.m_pageStoreBackend->store();
    QVERIFY(store);
    const QRect allPages(0, 0, totalPages * 64, 64);
    const QRect changedRect(0, 0, changedPages * 64, 64);
    dm.clear(allPages, reinterpret_cast<const quint8 *>(initialPixel.constData()));
    QVector<KisRetainedImageEpochSnapshot> retained;
    for (int h = 0; h < retainedRoots; ++h) {
        const QByteArray pixel(pixelSize, char(0x20 + h));
        dm.clear(QRect((totalPages - 1) * 64, 0, 64, 64),
                 reinterpret_cast<const quint8 *>(pixel.constData()));
        retained.append(store->captureRetainedEpoch());
        QVERIFY(retained.back().isValid());
    }
    // Separate a known uniform-cache hit from byte-identical content whose
    // backing cache was displaced by setup. Do not call the latter zero work.
    const auto beforeWarmup = store->sessionStats();
    QElapsedTimer warmupTimer;
    warmupTimer.start();
    if (mode == "noop") {
        dm.clear(changedRect, reinterpret_cast<const quint8 *>(initialPixel.constData()));
    }
    const qint64 warmupNanoseconds = warmupTimer.nsecsElapsed();
    const auto before = store->sessionStats();
    QCOMPARE(before.registeredPages, qsizetype(totalPages));
    const auto payloadBefore = dm.m_pageStoreBackend->payloadWork();
    const auto mutationBefore = store->mutationStatistics();
    const QByteArray first(pixelSize * changedPages * 64, char(0x40));
    const QByteArray second(pixelSize * changedPages * 64, char(0x41));
    KisPageStoreDiagnosticRecorder stages(stageProfile, store);
    QElapsedTimer operationTimer;
    operationTimer.start();
    for (int iteration = 0; iteration < loops; ++iteration) {
        if (stageProfile) {
            QVERIFY(stages.setRecording(iteration % samplePeriod == sampleOffset));
        }
        const QByteArray &bytes = iteration % 2 ? second : first;
        if (mode == "real") {
            // One changed scanline per page, not full-page alias or identical
            // write-back. Keep the other 63 rows exact through COW.
            dm.writeBytes(reinterpret_cast<const quint8 *>(bytes.constData()),
                          0, 0, changedPages * 64, 1);
        } else {
            const QByteArray &pixel = (mode == "noop" || mode == "identical") ? initialPixel : bytes;
            dm.clear(changedRect, reinterpret_cast<const quint8 *>(pixel.constData()));
        }
    }
    const qint64 operationNanoseconds = operationTimer.nsecsElapsed();
    const auto payloadAfter = dm.m_pageStoreBackend->payloadWork();
    if (stageProfile) QVERIFY(stages.setRecording(false));
    const QJsonArray stageMetrics = kisBr1StageMetrics(stages);
    if (stageProfile) {
        QCOMPARE(stages.metrics()[size_t(KisPageStoreDiagnosticPhase::ManifestExport)].intervals, quint64(0));
    }
    const auto after = store->sessionStats();
    const auto mutationAfter = store->mutationStatistics();
    const bool identical = mode == "noop" || mode == "identical";
    const quint64 expectedChanged = identical ? 0 : quint64(loops * changedPages);
    const quint64 defaultPages = payloadAfter.defaultInitializedPages - payloadBefore.defaultInitializedPages;
    const quint64 defaultBytes = payloadAfter.defaultInitializedBytes - payloadBefore.defaultInitializedBytes;
    const quint64 copiedPages = payloadAfter.explicitCopyPages - payloadBefore.explicitCopyPages;
    const quint64 copiedBytes = payloadAfter.explicitCopyBytes - payloadBefore.explicitCopyBytes;
    const quint64 nativePages = payloadAfter.nativeDuplicatePages - payloadBefore.nativeDuplicatePages;
    const quint64 nativeBytes = payloadAfter.nativeDuplicateBytes - payloadBefore.nativeDuplicateBytes;
    const quint64 precloneHits = payloadAfter.nativePrecloneHits - payloadBefore.nativePrecloneHits;
    const quint64 nativeForegroundBytes = payloadAfter.nativeForegroundCopyBytes - payloadBefore.nativeForegroundCopyBytes;
    const quint64 adoptedPages = payloadAfter.adoptedPages - payloadBefore.adoptedPages;
    const quint64 adoptedBytes = payloadAfter.adoptedBytes - payloadBefore.adoptedBytes;
    const quint64 tileBytes = quint64(64 * 64 * pixelSize);
    const quint64 realPages = mode == "real" ? expectedChanged : 0;
    QCOMPARE(defaultPages, quint64(0));
    QCOMPARE(copiedPages, defaultPages);
    QCOMPARE(nativePages, realPages);
    QCOMPARE(defaultBytes, defaultPages * tileBytes);
    QCOMPARE(copiedBytes, copiedPages * tileBytes);
    QCOMPARE(nativeBytes, nativePages * tileBytes);
    QVERIFY(precloneHits <= nativePages);
    QCOMPARE(nativeForegroundBytes, (nativePages - precloneHits) * tileBytes);
    const quint64 adoptions = after.synchronousHostWrites - before.synchronousHostWrites;
    const quint64 commits = after.committedTransactions - before.committedTransactions;
    QCOMPARE(adoptedPages, adoptions);
    QCOMPARE(adoptedBytes, adoptions * tileBytes);
    if (mode == "identical") {
        // Known incomplete content-property optimization: one cold redundant
        // adoption per page is allowed here and reported, not qualified Pass.
        QVERIFY(adoptions <= quint64(changedPages));
        QVERIFY(commits <= 1);
    } else {
        QCOMPARE(adoptions, mode == "alias" ? expectedChanged : quint64(0));
        QCOMPARE(commits, mode == "noop" ? quint64(0) : quint64(loops));
    }
    QCOMPARE(after.writeRequestsCreated - before.writeRequestsCreated,
             quint64(0));
    const quint64 nativeReserved = mutationAfter.generationsReserved - mutationBefore.generationsReserved;
    QCOMPARE(nativeReserved, realPages);
    QCOMPARE(mutationAfter.pagesSealed - mutationBefore.pagesSealed, nativeReserved);
    QCOMPARE(mutationAfter.pagesCancelled, mutationBefore.pagesCancelled);
    QCOMPARE(after.defaultMaterializationRequests, before.defaultMaterializationRequests);
    QCOMPARE(dm.extent(), allPages);
    QByteArray pixel(pixelSize, char(0));
    for (int column : {0, changedPages - 1}) {
        dm.readBytes(reinterpret_cast<quint8 *>(pixel.data()), column * 64, 0, 1, 1);
        QCOMPARE(pixel, identical ? initialPixel : QByteArray(pixelSize, char(loops % 2 ? 0x40 : 0x41)));
        dm.readBytes(reinterpret_cast<quint8 *>(pixel.data()), column * 64, 1, 1, 1);
        QCOMPARE(pixel, mode == "alias" ? QByteArray(pixelSize, char(loops % 2 ? 0x40 : 0x41)) : initialPixel);
    }
    dm.readBytes(reinterpret_cast<quint8 *>(pixel.data()), changedPages * 64, 0, 1, 1);
    QCOMPARE(pixel, initialPixel);
    // Retained epochs see the original exact bytes even after the real writes.
    for (const auto &snapshot : std::as_const(retained)) {
        KisPageReadView view;
        view.kind = KisPageReadViewKind::CommittedEpoch;
        view.epoch = snapshot.snapshot.epoch;
        view.retention = snapshot.token;
        const KisPageKey key{dm.m_pageStoreBackend->surface(), {0, 0}};
        auto request = store->acquireRead(key, view,
            {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer}, KisPagePriority::Interactive);
        QVERIFY(request.isValid());
        auto lease = store->resolve(request, request.readiness);
        QVERIFY(lease.isValid());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(lease.cpuData()), pixelSize), initialPixel);
        store->release(std::move(lease));
    }
    QElapsedTimer releaseTimer;
    releaseTimer.start();
    for (const auto &snapshot : std::as_const(retained)) {
        QVERIFY(store->releaseSnapshot(snapshot.token));
    }
    const qint64 releaseNanoseconds = releaseTimer.nsecsElapsed();
    const auto drained = store->sessionStats();
    if (stageProfile || !loopsOption.isEmpty()) {
        qInfo().noquote() << "BR1_FIXED" << QJsonDocument(QJsonObject{
            {"case", QTest::currentDataTag()}, {"mode", mode}, {"N", totalPages},
            {"K", changedPages}, {"H", retainedRoots}, {"bpp", pixelSize}, {"loops", loops},
            {"logical_changed_pages", double(expectedChanged)},
            {"submitted_page_visits", double(loops * changedPages)},
            {"canonical_changed_pages", double(adoptions + nativeReserved + after.writeRequestsCreated - before.writeRequestsCreated)},
            {"native_generations_reserved", double(nativeReserved)},
            {"native_pages_sealed", double(mutationAfter.pagesSealed - mutationBefore.pagesSealed)},
            {"native_guards_released", double(mutationAfter.guardsReleased - mutationBefore.guardsReleased)},
            {"scope", "single target manager; setup, correctness reads and retained-root release outside operation"},
            {"operation_ms", double(operationNanoseconds) / 1e6},
            {"known_noop_warmup_ms", double(warmupNanoseconds) / 1e6},
            {"known_noop_warmup_adoptions", double(before.synchronousHostWrites - beforeWarmup.synchronousHostWrites)},
            {"release_retained_ms", double(releaseNanoseconds) / 1e6},
            {"stage_profile", stageProfile}, {"cpu_clock_available", stages.threadCpuClockAvailable()},
            {"stage_sample_period", samplePeriod}, {"stage_sample_offset", sampleOffset},
            {"sampled_operations", stageProfile && loops > sampleOffset ? (loops - 1 - sampleOffset) / samplePeriod + 1 : 0},
            {"adoptions", double(after.synchronousHostWrites - before.synchronousHostWrites)},
            {"write_requests", double(after.writeRequestsCreated - before.writeRequestsCreated)},
            {"commits", double(after.committedTransactions - before.committedTransactions)},
            {"payload_scope", "target provider operation interval; logical bytes, not hardware traffic; background preclone work not measured"},
            {"native_write_copy", true},
            {"default_initialized_pages", double(defaultPages)},
            {"default_initialized_bytes", double(defaultBytes)},
            {"explicit_copy_pages", double(copiedPages)},
            {"explicit_copy_bytes", double(copiedBytes)},
            {"native_duplicate_pages", double(nativePages)},
            {"native_duplicate_bytes", double(nativeBytes)},
            {"native_preclone_hits", double(precloneHits)},
            {"native_foreground_copy_bytes", double(nativeForegroundBytes)},
            {"adopted_pages", double(adoptedPages)},
            {"adopted_bytes", double(adoptedBytes)},
            {"live_versions_before", double(before.pageVersions)},
            {"live_versions_after", double(after.pageVersions)},
            {"live_versions_after_release", double(drained.pageVersions)},
            {"phases", stageMetrics}
        }).toJson(QJsonDocument::Compact);
    }
}

void KisTiledDataManagerTest::testPageStoreNativeIteratorEntryPoints_data()
{
    QTest::addColumn<int>("entry");
    QTest::newRow("hline") << 0;
    QTest::newRow("vline") << 1;
    QTest::newRow("random-accessor") << 2;
}

void KisTiledDataManagerTest::testPageStorePackedWriteNative_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("history");
    QTest::addColumn<bool>("partial"); QTest::addColumn<bool>("noOp");
    for (int bpp : {1, 4, 8, 16}) for (bool history : {false, true})
        for (bool partial : {false, true}) for (bool noOp : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-history%2-partial%3-noop%4")
                .arg(bpp).arg(history).arg(partial).arg(noOp))) << bpp << history << partial << noOp;
}

void KisTiledDataManagerTest::testPageStorePackedWriteNative()
{
    QFETCH(int, bpp); QFETCH(bool, history); QFETCH(bool, partial); QFETCH(bool, noOp);
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31)), prior(bpp, char(0x41));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    const QRect whole(-64,-64,192,128);
    dm.clear(whole.x(), whole.y(), whole.width(), whole.height(), reinterpret_cast<const quint8 *>(initial.constData()));
    auto *backend = dm.m_pageStoreBackend; auto *store = backend->store();
    auto memento = history ? dm.getMemento() : KisMementoSP{};
    if (history) dm.clear(whole.x(), whole.y(), whole.width(), whole.height(), reinterpret_cast<const quint8 *>(prior.constData()));
    auto old = backend->captureReadView(); QVERIFY(old.isValid());
    const QRect area = partial ? QRect(-63,-63,190,126) : whole;
    const int stride = area.width() * bpp + 17;
    const char before = history ? 0x41 : 0x31;
    QByteArray input(stride * area.height(), char(0xee));
    for (int y = 0; y < area.height(); ++y)
        memset(input.data() + y * stride, noOp ? before : 0x71, size_t(area.width() * bpp));
    const auto requests = store->sessionStats(); const auto count = store->mutationStatistics();
    const auto views = store->readScopeStatistics(); const auto payload = backend->payloadWork();
    KisPageStoreDiagnosticRecorder recorder(true, store);
    bool hidden = true; int copies = 0;
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::PackedWritePrepare) return;
        ++copies;
        auto view = backend->captureReadView(); auto read = view.readResidentPage({{1},{-1,-1}});
        hidden &= read.isValid() && static_cast<const char *>(read.data())[0] == before;
    });
    dm.writeBytes(reinterpret_cast<const quint8 *>(input.constData()), area.x(), area.y(), area.width(), area.height(), stride);
    QVERIFY(hidden);
    const auto after = store->mutationStatistics(); const auto work = backend->payloadWork();
    const quint64 changed = noOp ? 0 : 6;
    QCOMPARE(copies, int(changed));
    // A byte-identical input is rejected by the preflight comparison before
    // creating a mutation session; changed input owns exactly one batch.
    QCOMPARE(after.sessionsCreated - count.sessionsCreated, quint64(noOp ? 0 : 1));
    QCOMPARE(after.generationsReserved - count.generationsReserved, changed);
    QCOMPARE(after.pagesSealed - count.pagesSealed, changed);
    QCOMPARE(after.writablePinsAcquired - count.writablePinsAcquired, changed);
    QCOMPARE(after.writablePinsReleased - count.writablePinsReleased, changed);
    QCOMPARE(store->sessionStats().readRequestsCreated, requests.readRequestsCreated);
    QCOMPARE(store->sessionStats().writeRequestsCreated, requests.writeRequestsCreated);
    QCOMPARE(store->sessionStats().committedTransactions - requests.committedTransactions, quint64(history || noOp ? 0 : 1));
    // One body View plus one derived-index refresh for nonempty changes;
    // test-only captures at Prepare are counted separately, never per-page body Views.
    QCOMPARE(store->readScopeStatistics().capturedReadViewsCreated - views.capturedReadViewsCreated,
             quint64(1 + (!noOp ? 1 : 0) + copies));
    QCOMPARE(work.nativeDuplicatePages - payload.nativeDuplicatePages, partial ? changed : quint64(0));
    QCOMPARE(work.explicitCopyPages, payload.explicitCopyPages);
    QCOMPARE(work.defaultInitializedPages,payload.defaultInitializedPages);
    QCOMPARE(work.payloadInitializedPages-payload.payloadInitializedPages,partial ? quint64(0) : changed);
    QCOMPARE(work.payloadInitializedBytes-payload.payloadInitializedBytes,partial ? quint64(0) : changed*64*64*bpp);
    QByteArray expected(whole.width() * whole.height() * bpp, before), actual(expected.size(), Qt::Uninitialized);
    if (!noOp) for (int y = area.top(); y <= area.bottom(); ++y)
        memset(expected.data() + ((y - whole.y()) * whole.width() + area.x() - whole.x()) * bpp,
               0x71, size_t(area.width() * bpp));
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), whole.x(), whole.y(), whole.width(), whole.height());
    QCOMPARE(actual, expected);
    auto oldRead = old.readResidentPage({{1},{-1,-1}}); QVERIFY(oldRead.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(oldRead.data()), int(oldRead.byteSize())), QByteArray(64*64*bpp, before));
    oldRead = {}; old = {};
    if (history) {
        dm.commit(); dm.rollback(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), whole.x(), whole.y(), whole.width(), whole.height());
        QCOMPARE(actual, QByteArray(actual.size(), char(0x31)));
        dm.rollforward(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), whole.x(), whole.y(), whole.width(), whole.height());
        QCOMPARE(actual, expected); dm.purgeHistory(memento);
    }
}

void KisTiledDataManagerTest::testPageStorePackedWritePreclaimExcludesLateWriter_data()
{
    QTest::addColumn<bool>("history"); QTest::addColumn<bool>("partial");
    for (bool history : {false, true}) for (bool partial : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("history%1-partial%2").arg(history).arg(partial))) << history << partial;
}

void KisTiledDataManagerTest::testPageStorePackedWritePreclaimExcludesLateWriter()
{
    QFETCH(bool, history); QFETCH(bool, partial);
    quint8 blank = 0x13, initial = 0x31, prior = 0x41;
    KisDataManager dm(1, &blank); dm.clear(0,0,128,64,&initial);
    auto *backend = dm.m_pageStoreBackend; auto *store = backend->store();
    auto memento = history ? dm.getMemento() : KisMementoSP{};
    if (history) dm.clear(0,0,128,64,&prior);
    const auto foreignTx = store->beginCurrentTransaction();
    auto foreign = store->beginMutation(foreignTx); QVERIFY(foreign.isActive());
    KisCpuWriteGuard held;
    const auto cleanup = qScopeGuard([&] { held = {}; foreign.cancel(); store->abort(foreignTx); });
    int prepared = 0;
    KisPageStoreDiagnosticRecorder recorder(true, store);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase == KisPageStoreDiagnosticPhase::PackedWritePrepare && ++prepared == 2)
            held = foreign.beginWrite({{1},{1,0}});
    });
    QByteArray input(128 * (partial ? 1 : 64), char(0x71));
    QVector<KisLogicalPageId> changed; QString error;
    const auto before = store->mutationStatistics();
    QCOMPARE(backend->writeBytes(reinterpret_cast<const quint8 *>(input.constData()),0,0,128,partial ? 1 : 64,0,false,&changed,&error),
             KisPageStoreWriteOperationResult::Succeeded);
    // The competing writer arrives after range admission, so it cannot
    // acquire the second page even while the first page is being prepared.
    QVERIFY(!held.isValid()); QCOMPARE(prepared, 2); QCOMPARE(changed.size(), 2); QVERIFY(error.isEmpty());
    QCOMPARE(store->mutationStatistics().pagesCancelled - before.pagesCancelled, quint64(0));
    QByteArray actual(input.size(), Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()),0,0,128,partial ? 1 : 64);
    QCOMPARE(actual, input);
    held = {}; QVERIFY(foreign.cancel()); QVERIFY(store->abort(foreignTx));
    // The rejected foreign segment cannot roll back the completed operation.
    dm.writeBytes(reinterpret_cast<const quint8 *>(input.constData()),0,0,128,partial ? 1 : 64);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()),0,0,128,partial ? 1 : 64); QCOMPARE(actual, input);
    if (history) {
        dm.commit(); dm.rollback(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()),0,0,128,partial ? 1 : 64);
        QCOMPARE(actual, QByteArray(actual.size(), char(initial))); dm.purgeHistory(memento);
    }
}

void KisTiledDataManagerTest::testPageStorePackedWriteBorrowedAndInvalid()
{
    quint8 blank = 0x13, next = 0x71;
    KisDataManager dm(1, &blank);
    auto *backend = dm.m_pageStoreBackend; auto *store = backend->store();
    QVector<KisLogicalPageId> changed; QString error;
    auto tile = dm.getTile(0,0,true); tile->lockForWrite();
    const auto cleanup = qScopeGuard([&] { tile->unlockForWrite(); });
    auto *pointer = tile->data(); QVERIFY(pointer); *pointer = 0x31;
    const auto count = store->mutationStatistics();
    QCOMPARE(backend->writeBytes(&next,0,0,1,1,0,false,&changed,&error), KisPageStoreWriteOperationResult::Borrowed);
    QCOMPARE(store->mutationStatistics().sessionsCreated, count.sessionsCreated);
    dm.writeBytes(&next,0,0,1,1);
    QCOMPARE(tile->data(), pointer); QCOMPARE(*pointer, next);
    const auto requests = store->sessionStats();
    QCOMPARE(backend->writeBytes(&next,128,0,2,1,1,false,&changed,&error), KisPageStoreWriteOperationResult::Failed);
    QVERIFY(!error.isEmpty()); QVERIFY(changed.isEmpty());
    QCOMPARE(backend->writeBytes(&next,std::numeric_limits<qint32>::max(),0,2,1,0,false,&changed,&error), KisPageStoreWriteOperationResult::Failed);
    QCOMPARE(backend->writeBytes(nullptr,0,0,1,1,0,false,&changed,&error), KisPageStoreWriteOperationResult::Succeeded);
    QCOMPARE(backend->writeBytes(&next,0,0,0,1,0,false,&changed,&error), KisPageStoreWriteOperationResult::Succeeded);
    QCOMPARE(store->sessionStats().committedTransactions, requests.committedTransactions);
    QCOMPARE(tile->data(), pointer); QCOMPARE(*pointer, next);
}

void KisTiledDataManagerTest::testPageStorePackedWriteBodyAllowsReader()
{
    quint8 blank = 0x13, initial = 0x31, input = 0x71, observed = 0;
    KisDataManager dm(1, &blank); dm.clear(0,0,64,64,&initial);
    QSemaphore start, finished;
    std::thread reader([&] { start.acquire(); dm.readBytes(&observed,0,0,1,1); finished.release(); });
    bool entered = false, completedInside = false;
    const auto cleanup = qScopeGuard([&] { if (!entered) start.release(); if (reader.joinable()) reader.join(); });
    KisPageStoreDiagnosticRecorder recorder(true, dm.m_pageStoreBackend->store());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::PackedWriteCopy || entered) return;
        entered = true; start.release(); completedInside = finished.tryAcquire(1,2000);
    });
    dm.writeBytes(&input,0,0,1,1);
    if (!entered) start.release();
    reader.join();
    QVERIFY(entered); QVERIFY(completedInside); QCOMPARE(observed, initial);
    dm.readBytes(&observed,0,0,1,1); QCOMPARE(observed, input);
}

void KisTiledDataManagerTest::testPageStoreWriteBytesPinWorkingSet_data()
{
    QTest::addColumn<int>("columns"); QTest::addColumn<int>("bpp");
    for (int columns : {1, 16, 128}) for (int bpp : {1, 4})
        QTest::newRow(qPrintable(QStringLiteral("columns%1-bpp%2").arg(columns).arg(bpp))) << columns << bpp;
}

void KisTiledDataManagerTest::testPageStoreWriteBytesPinWorkingSet()
{
    QFETCH(int, columns); QFETCH(int, bpp);
    const QByteArray blank(bpp, char(0x13));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    auto *store = dm.m_pageStoreBackend->store(); QVERIFY(store);
    const auto before = store->sessionStats();
    const auto mutationBefore = store->mutationStatistics();
    const QRect area(-64, -64, columns * 64, 128);
    const QByteArray input(area.width() * area.height() * bpp, char(0x47));
    const auto memento = dm.getMemento();
    dm.writeBytes(reinterpret_cast<const quint8 *>(input.constData()), area.x(), area.y(), area.width(), area.height());
    dm.commit();
    const auto after = store->mutationStatistics();
    QCOMPARE(store->sessionStats().writeRequestsCreated, before.writeRequestsCreated);
    QCOMPARE(after.generationsReserved - mutationBefore.generationsReserved, quint64(columns * 2));
    QCOMPARE(after.writablePinsAcquired - mutationBefore.writablePinsAcquired, quint64(columns * 2));
    QCOMPARE(after.writablePinsReleased - mutationBefore.writablePinsReleased, quint64(columns * 2));
    QCOMPARE(after.maximumPinnedPagesPerSegment, quint64(1));
    QByteArray actual(input.size(), Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), area.x(), area.y(), area.width(), area.height());
    QCOMPARE(actual, input);
    dm.rollback(memento);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), area.x(), area.y(), area.width(), area.height());
    QCOMPARE(actual, QByteArray(input.size(), char(0x13)));
    dm.rollforward(memento);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), area.x(), area.y(), area.width(), area.height());
    QCOMPARE(actual, input);
    dm.purgeHistory(memento);
}

void KisTiledDataManagerTest::testPageStoreNativeIteratorEntryPoints()
{
    QFETCH(int, entry);
    const quint8 defaultPixel = 0x13;
    KisDataManager dm(1, &defaultPixel);
    auto *store = dm.m_pageStoreBackend->store();
    const auto before = store->sessionStats();
    const auto mutations = store->mutationStatistics();
    const auto memento = dm.getMemento();
    const QRect area(-65, -65, 131, 131);
    if (entry == 0) {
        KisHLineIterator2 it(&dm, area.x(), area.y(), area.width(), 0, 0, true, nullptr);
        for (int y = 0; y < area.height(); ++y) {
            do { QCOMPARE(it.oldRawData()[0], defaultPixel); it.rawData()[0] = 0x47; } while (it.nextPixel());
            if (y + 1 < area.height()) it.nextRow();
        }
    } else if (entry == 1) {
        KisVLineIterator2 it(&dm, area.x(), area.y(), area.height(), 0, 0, true, nullptr);
        for (int x = 0; x < area.width(); ++x) {
            do { QCOMPARE(it.oldRawData()[0], defaultPixel); it.rawData()[0] = 0x47; } while (it.nextPixel());
            if (x + 1 < area.width()) it.nextColumn();
        }
    } else {
        KisRandomAccessor2 it(&dm, 0, 0, true, nullptr);
        for (int y = area.top(); y <= area.bottom(); ++y) {
            for (int x = area.left(); x <= area.right(); ++x) {
                it.moveTo(x, y);
                QCOMPARE(it.oldRawData()[0], defaultPixel);
                it.rawData()[0] = 0x47;
            }
        }
    }
    dm.commit();
    const auto after = store->sessionStats();
    QCOMPARE(after.writeRequestsCreated, before.writeRequestsCreated);
    QVERIFY(store->mutationStatistics().pagesSealed > mutations.pagesSealed);
    QByteArray actual(area.width() * area.height(), 0);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), area.x(), area.y(), area.width(), area.height());
    QCOMPARE(actual, QByteArray(actual.size(), char(0x47)));
    dm.rollback(memento);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), area.x(), area.y(), area.width(), area.height());
    QCOMPARE(actual, QByteArray(actual.size(), char(defaultPixel)));
    dm.rollforward(memento);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), area.x(), area.y(), area.width(), area.height());
    QCOMPARE(actual, QByteArray(actual.size(), char(0x47)));
}

void KisTiledDataManagerTest::testPageStoreResidentReadReusesNativeGuard()
{
    quint8 defaultPixel = 0;
    quint8 paintedPixel = 0x63;
    KisTiledDataManager dm(1, &defaultPixel);
    dm.clear(QRect(0, 0, 64, 64), &paintedPixel);

    bool existingTile = false;
    KisTileSP tile = dm.getReadOnlyTileLazy(0, 0, existingTile);
    QVERIFY(tile);
    QVERIFY(existingTile);

    const KisPageStoreSessionStats before =
        dm.m_pageStoreBackend->store()->sessionStats();
    constexpr qint32 lockCount = 4096;
    for (qint32 i = 0; i < lockCount; ++i) {
        tile->lockForRead();
        QCOMPARE(tile->data()[0], paintedPixel);
        tile->unlockForRead();
    }
    const KisPageStoreSessionStats after =
        dm.m_pageStoreBackend->store()->sessionStats();

    QCOMPARE(after.readRequestsCreated, before.readRequestsCreated);
    QCOMPARE(after.defaultMaterializationRequests,
             before.defaultMaterializationRequests);
    QCOMPARE(after.pendingRequests, qsizetype(0));
    QCOMPARE(after.activeReadLeases, qsizetype(0));
}

void KisTiledDataManagerTest::testPageStoreWriteIntentDoesNotAllocateUntilDataExposure()
{
    quint8 defaultPixel = 0;
    KisTiledDataManager dm(1, &defaultPixel);
    QVERIFY(dm.m_pageStoreBackend);
    QVERIFY(dm.m_pageStoreBackend->store());

    const KisPageStoreSessionStats before =
        dm.m_pageStoreBackend->store()->sessionStats();
    const auto mutationBefore = dm.m_pageStoreBackend->store()->mutationStatistics();
    KisTileSP tile = dm.getTile(7, -3, true);
    QVERIFY(tile);
    tile->lockForWrite();
    tile->unlockForWrite();
    const KisPageStoreSessionStats afterIntent =
        dm.m_pageStoreBackend->store()->sessionStats();

    QCOMPARE(afterIntent.registeredPages, before.registeredPages);
    QCOMPARE(afterIntent.pageVersions, before.pageVersions);
    QCOMPARE(afterIntent.replicas, before.replicas);
    QCOMPARE(afterIntent.readRequestsCreated, before.readRequestsCreated);
    QCOMPARE(afterIntent.writeRequestsCreated, before.writeRequestsCreated);
    QCOMPARE(afterIntent.defaultMaterializationRequests,
             before.defaultMaterializationRequests);

    tile->lockForWrite();
    tile->data()[0] = 0x71;
    tile->unlockForWrite();
    const KisPageStoreSessionStats afterWrite =
        dm.m_pageStoreBackend->store()->sessionStats();
    QCOMPARE(afterWrite.writeRequestsCreated,
             before.writeRequestsCreated);
    QCOMPARE(afterWrite.defaultMaterializationRequests,
             before.defaultMaterializationRequests);
    const auto mutationAfter = dm.m_pageStoreBackend->store()->mutationStatistics();
    QCOMPARE(mutationAfter.generationsReserved - mutationBefore.generationsReserved, quint64(1));
    QCOMPARE(mutationAfter.pagesSealed - mutationBefore.pagesSealed, quint64(1));
    QCOMPARE(afterWrite.pendingRequests, qsizetype(0));
    QCOMPARE(afterWrite.activeWriteLeases, qsizetype(0));

    bool existingTile = false;
    KisTileSP readTile = dm.getReadOnlyTileLazy(7, -3, existingTile);
    QVERIFY(existingTile);
    readTile->lockForRead();
    QCOMPARE(readTile->data()[0], quint8(0x71));
    readTile->unlockForRead();
}

void KisTiledDataManagerTest::testPageStoreWholeTileFillUsesSynchronousAdoption()
{
    quint8 defaultPixel = 0;
    quint8 fillPixel = 0x4d;
    KisTiledDataManager dm(1, &defaultPixel);
    KisPageStore *store = dm.m_pageStoreBackend->store();
    QVERIFY(store);
    const KisPageStoreSessionStats before = store->sessionStats();

    dm.clear(QRect(0, 0, 128, 128), &fillPixel);
    const KisPageStoreSessionStats filled = store->sessionStats();
    QCOMPARE(filled.synchronousHostWrites,
             before.synchronousHostWrites + 4);
    QCOMPARE(filled.writeRequestsCreated, before.writeRequestsCreated);
    QCOMPARE(filled.readRequestsCreated, before.readRequestsCreated);
    QCOMPARE(filled.registeredPages, before.registeredPages + 4);
    QCOMPARE(filled.committedTransactions,
             before.committedTransactions + 1);

    // Repeating the same uniform full-page fill reuses the immutable shared
    // backing. It is a physical and logical no-op: no PageStore generation,
    // proof, transaction commit, or provider adoption is allowed.
    dm.clear(QRect(0, 0, 128, 128), &fillPixel);
    const KisPageStoreSessionStats repeated = store->sessionStats();
    QCOMPARE(repeated.synchronousHostWrites,
             filled.synchronousHostWrites);
    QCOMPARE(repeated.committedTransactions,
             filled.committedTransactions);
    QCOMPARE(repeated.registeredPages, filled.registeredPages);

    quint8 bytes[4] = {};
    dm.readBytes(bytes, 0, 0, 2, 2);
    for (quint8 byte : bytes) QCOMPARE(byte, fillPixel);

    dm.clear(QRect(0, 0, 128, 128), &defaultPixel);
    const KisPageStoreSessionStats cleared = store->sessionStats();
    QCOMPARE(cleared.synchronousHostWrites, repeated.synchronousHostWrites);
    QCOMPARE(cleared.writeRequestsCreated, repeated.writeRequestsCreated);
    QCOMPARE(cleared.committedTransactions,
             repeated.committedTransactions + 1);
    QCOMPARE(cleared.registeredPages, qsizetype(4));
    QCOMPARE(dm.extent(), QRect());
}

void KisTiledDataManagerTest::testPageStoreBulkNoOpAndBitBltBatching()
{
    constexpr qint32 side = 128;
    quint8 defaultPixel = 0;
    quint8 fillPixel = 0x31;
    KisTiledDataManager dm(1, &defaultPixel);
    KisPageStore *store = dm.m_pageStoreBackend->store();
    QVERIFY(store);

    dm.clear(QRect(0, 0, side, side), &fillPixel);
    QByteArray bytes(side * side, Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(bytes.data()),
                 0, 0, side, side);
    const KisPageStoreSessionStats beforeNoOp = store->sessionStats();
    const auto mutationBeforeNoOp = store->mutationStatistics();
    dm.writeBytes(reinterpret_cast<const quint8 *>(bytes.constData()),
                  0, 0, side, side);
    const KisPageStoreSessionStats afterNoOp = store->sessionStats();
    QCOMPARE(store->mutationStatistics().generationsReserved, mutationBeforeNoOp.generationsReserved);
    QCOMPARE(afterNoOp.writeRequestsCreated,
             beforeNoOp.writeRequestsCreated);
    QCOMPARE(afterNoOp.synchronousHostWrites,
             beforeNoOp.synchronousHostWrites);
    QCOMPARE(afterNoOp.committedTransactions,
             beforeNoOp.committedTransactions);

    bytes[0] = char(0x72);
    dm.writeBytes(reinterpret_cast<const quint8 *>(bytes.constData()),
                  0, 0, side, side);
    const KisPageStoreSessionStats afterOnePageChange =
        store->sessionStats();
    QCOMPARE(afterOnePageChange.writeRequestsCreated,
             afterNoOp.writeRequestsCreated);
    const auto mutationAfterChange = store->mutationStatistics();
    QCOMPARE(mutationAfterChange.generationsReserved - mutationBeforeNoOp.generationsReserved, quint64(1));
    QCOMPARE(mutationAfterChange.pagesSealed - mutationBeforeNoOp.pagesSealed, quint64(1));
    QCOMPARE(afterOnePageChange.committedTransactions,
             afterNoOp.committedTransactions + 1);

    quint8 sourceDefault = 0x5a;
    KisTiledDataManager source(1, &sourceDefault);
    const KisPageStoreSessionStats beforeBitBlt = store->sessionStats();
    dm.bitBlt(&source, QRect(0, 0, side, side));
    const KisPageStoreSessionStats afterBitBlt = store->sessionStats();
    QCOMPARE(afterBitBlt.synchronousHostWrites,
             beforeBitBlt.synchronousHostWrites + 4);
    QCOMPARE(afterBitBlt.committedTransactions,
             beforeBitBlt.committedTransactions + 1);

    dm.bitBlt(&source, QRect(0, 0, side, side));
    const KisPageStoreSessionStats afterRepeatedBitBlt =
        store->sessionStats();
    QCOMPARE(afterRepeatedBitBlt.synchronousHostWrites,
             afterBitBlt.synchronousHostWrites);
    QCOMPARE(afterRepeatedBitBlt.committedTransactions,
             afterBitBlt.committedTransactions);
}

void KisTiledDataManagerTest::testIteratorWriteScope_data()
{
    QTest::addColumn<int>("entry");
    QTest::addColumn<int>("failurePoint");
    for (int entry = 0; entry < 3; ++entry) {
        for (KisSwapInFailurePoint point : {
                 KisSwapInFailurePoint::Mapping,
                 KisSwapInFailurePoint::Allocation,
                 KisSwapInFailurePoint::Decompression}) {
            QTest::newRow(qPrintable(QStringLiteral("entry%1-failure%2")
                              .arg(entry).arg(int(point))))
                << entry << int(point);
        }
    }
}

void KisTiledDataManagerTest::testIteratorWriteScope()
{
    QFETCH(int, entry);
    QFETCH(int, failurePoint);
    const quint8 blank = 0;
    const quint8 first = 0x51;
    const quint8 second = 0x62;
    KisDataManager dm(1, &blank);
    auto *backend = dm.m_pageStoreBackend;
    const bool horizontal = entry == 0;
    const qint32 secondColumn = horizontal ? 0 : entry == 1 ? 1 : 0;
    const qint32 secondRow = horizontal ? 1 : 0;
    dm.clear(0, 0, 64, 64, &first);
    dm.clear(secondColumn * 64, secondRow * 64, 64, 64, &second);

    bool existing = false;
    const auto secondTile = dm.getReadOnlyTileLazy(
        secondColumn, secondRow, existing);
    QVERIFY(existing);
    KisTileData *secondData = secondTile->tileData();
    QVERIFY(secondData->ref());
    const auto release = qScopeGuard([&] { secondData->deref(); });
    auto *tileStore = KisTileDataStore::instance();
    QVERIFY(tileStore->trySwapTileData(secondData));
    QVERIFY(!secondData->isResident());

    tileStore->testingFailNextSwapIn(
        KisSwapInFailurePoint(failurePoint));
    if (entry == 0) {
        KisHLineIterator2 writer(&dm, 0, 63, 1, 0, 0, true, nullptr);
        QVERIFY(writer.rawData());
        QVERIFY(backend->hasCurrentThreadIteratorWrites());
        writer.nextRow();
        QVERIFY(!writer.rawData());
        QVERIFY(backend->hasCurrentThreadIteratorWrites());
    } else if (entry == 1) {
        KisVLineIterator2 writer(&dm, 63, 0, 1, 0, 0, true, nullptr);
        QVERIFY(writer.rawData());
        QVERIFY(backend->hasCurrentThreadIteratorWrites());
        writer.nextColumn();
        QVERIFY(!writer.rawData());
        QVERIFY(backend->hasCurrentThreadIteratorWrites());
    } else {
        KisRandomAccessor2 writer(&dm, 0, 0, true, nullptr);
        QVERIFY(backend->hasCurrentThreadIteratorWrites());
        writer.moveTo(0, 0);
        QVERIFY(!writer.rawData());
        QVERIFY(backend->hasCurrentThreadIteratorWrites());
    }
    QVERIFY(!backend->hasCurrentThreadIteratorWrites());
    QVERIFY(!secondData->isResident());
}

void KisTiledDataManagerTest::testIteratorNestedWriteScope_data()
{
    QTest::addColumn<bool>("history");
    QTest::addColumn<bool>("horizontalFirst");
    for (bool history : {false, true}) {
        for (bool horizontalFirst : {false, true}) {
            QTest::newRow(qPrintable(QStringLiteral("history%1-horizontal-first%2")
                              .arg(history).arg(horizontalFirst)))
                << history << horizontalFirst;
        }
    }
}

void KisTiledDataManagerTest::testIteratorNestedWriteScope()
{
    QFETCH(bool, history);
    QFETCH(bool, horizontalFirst);
    const quint8 blank = 0;
    KisDataManager dm(1, &blank);
    auto *backend = dm.m_pageStoreBackend;
    auto *store = backend->store();
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    const auto before = store->sessionStats();
    const auto mutations = store->mutationStatistics();

    auto horizontal = std::make_unique<KisHLineIterator2>(
        &dm, 0, 0, 1, 0, 0, true, nullptr);
    auto random = std::make_unique<KisRandomAccessor2>(
        &dm, 0, 0, true, nullptr);
    random->moveTo(64, 0);
    QVERIFY(horizontal->rawData());
    QVERIFY(random->rawData());
    horizontal->rawData()[0] = 0x51;
    random->rawData()[0] = 0x62;
    QVERIFY(backend->hasCurrentThreadIteratorWrites());
    QCOMPARE(store->mutationStatistics().sessionsCreated - mutations.sessionsCreated,
             quint64(1));

    if (horizontalFirst) horizontal.reset();
    else random.reset();
    QVERIFY(backend->hasCurrentThreadIteratorWrites());
    QCOMPARE(store->mutationStatistics().pagesSealed, mutations.pagesSealed);
    QCOMPARE(store->sessionStats().committedTransactions,
             before.committedTransactions);
    if (horizontalFirst) QCOMPARE(random->rawData()[0], quint8(0x62));
    else QCOMPARE(horizontal->rawData()[0], quint8(0x51));

    horizontal.reset();
    random.reset();
    QVERIFY(!backend->hasCurrentThreadIteratorWrites());
    QCOMPARE(store->mutationStatistics().generationsReserved -
                 mutations.generationsReserved,
             quint64(2));
    QCOMPARE(store->mutationStatistics().pagesSealed - mutations.pagesSealed,
             quint64(2));
    QCOMPARE(store->sessionStats().committedTransactions -
                 before.committedTransactions,
             quint64(history ? 0 : 1));

    quint8 actual[2] = {};
    dm.readBytes(actual, 0, 0, 1, 1);
    dm.readBytes(actual + 1, 64, 0, 1, 1);
    QCOMPARE(actual[0], quint8(0x51));
    QCOMPARE(actual[1], quint8(0x62));
    if (history) {
        dm.commit();
        dm.rollback(memento);
        dm.readBytes(actual, 0, 0, 1, 1);
        dm.readBytes(actual + 1, 64, 0, 1, 1);
        QCOMPARE(actual[0], blank);
        QCOMPARE(actual[1], blank);
    }
}

void KisTiledDataManagerTest::testIteratorSwapInFailureDropsPointers_data()
{
    QTest::addColumn<int>("failurePoint");
    QTest::newRow("mapping")
        << int(KisSwapInFailurePoint::Mapping);
    QTest::newRow("allocation")
        << int(KisSwapInFailurePoint::Allocation);
    QTest::newRow("decompression")
        << int(KisSwapInFailurePoint::Decompression);
}

void KisTiledDataManagerTest::testIteratorSwapInFailureDropsPointers()
{
    QFETCH(int, failurePoint);
    const quint8 defaultPixel = 0;
    const quint8 value = 0x56;
    const quint8 horizontalValue = 0x57;
    const quint8 verticalValue = 0x58;
    KisDataManager dm(1, &defaultPixel);
    dm.m_mementoManager->setPageStoreBridge(nullptr);
    delete dm.m_pageStoreBackend;
    dm.m_pageStoreBackend = nullptr;
    dm.clear(0, 0, 128, 128, &value);
    dm.setPixel(64, 0, &horizontalValue);
    dm.setPixel(0, 64, &verticalValue);

    bool existing = false;
    auto tile = dm.getReadOnlyTileLazy(0, 0, existing);
    QVERIFY(existing);
    auto *tileData = tile->tileData();
    QVERIFY(tileData->ref());
    const auto release = qScopeGuard([&] { tileData->deref(); });
    auto *store = KisTileDataStore::instance();
    QVERIFY(store->trySwapTileData(tileData));
    QVERIFY(!tileData->isResident());

    const auto inject = [&] {
        store->testingFailNextSwapIn(
            KisSwapInFailurePoint(failurePoint));
    };
    inject();
    {
        KisHLineIterator2 iterator(
            &dm, 0, 0, 1, 0, 0, false, nullptr);
        QVERIFY(!iterator.rawDataConst());
    }
    QVERIFY(!tileData->isResident());

    inject();
    {
        KisVLineIterator2 iterator(
            &dm, 0, 0, 1, 0, 0, false, nullptr);
        QVERIFY(!iterator.rawDataConst());
    }
    QVERIFY(!tileData->isResident());

    inject();
    {
        KisRandomAccessor2 accessor(
            &dm, 0, 0, false, nullptr);
        accessor.moveTo(0, 0);
        QVERIFY(!accessor.rawDataConst());
        QVERIFY(!accessor.oldRawData());
    }
    QVERIFY(!tileData->isResident());

    QVERIFY(tile->lockForRead());
    QCOMPARE(tile->data()[0], value);
    tile->unlockForRead();

    bool horizontalExists = false;
    auto horizontalTile = dm.getReadOnlyTileLazy(
        1, 0, horizontalExists);
    QVERIFY(horizontalExists);
    auto *horizontalData = horizontalTile->tileData();
    QVERIFY(horizontalData != tileData);
    QVERIFY(horizontalData->ref());
    const auto releaseHorizontal = qScopeGuard(
        [&] { horizontalData->deref(); });
    QVERIFY(store->trySwapTileData(horizontalData));
    QVERIFY(!horizontalData->isResident());

    inject();
    QVERIFY(!horizontalTile->lockForRead());
    QVERIFY(!horizontalData->isResident());
    inject();
    {
        KisHLineIterator2 iterator(
            &dm, 0, 0, 65, 0, 0, false, nullptr);
        QVERIFY(!iterator.rawDataConst());
    }
    QVERIFY(!horizontalData->isResident());
    QVERIFY(tile->lockForRead());
    QCOMPARE(tile->data()[0], value);
    tile->unlockForRead();

    bool verticalExists = false;
    auto verticalTile = dm.getReadOnlyTileLazy(
        0, 1, verticalExists);
    QVERIFY(verticalExists);
    auto *verticalData = verticalTile->tileData();
    QVERIFY(verticalData != tileData);
    QVERIFY(verticalData != horizontalData);
    QVERIFY(verticalData->ref());
    const auto releaseVertical = qScopeGuard(
        [&] { verticalData->deref(); });
    QVERIFY(store->trySwapTileData(verticalData));
    QVERIFY(!verticalData->isResident());

    inject();
    {
        KisVLineIterator2 iterator(
            &dm, 0, 0, 65, 0, 0, false, nullptr);
        QVERIFY(!iterator.rawDataConst());
    }
    QVERIFY(!verticalData->isResident());
    QVERIFY(tile->lockForRead());
    QCOMPARE(tile->data()[0], value);
    tile->unlockForRead();

    inject();
    {
        KisHLineIterator2 iterator(
            &dm, 0, 63, 1, 0, 0, false, nullptr);
        QVERIFY(iterator.rawDataConst());
        iterator.nextRow();
        QVERIFY(!iterator.rawDataConst());
        QVERIFY(!iterator.oldRawData());
    }
    QVERIFY(!verticalData->isResident());

    inject();
    {
        KisVLineIterator2 iterator(
            &dm, 63, 0, 1, 0, 0, false, nullptr);
        QVERIFY(iterator.rawDataConst());
        iterator.nextColumn();
        QVERIFY(!iterator.rawDataConst());
        QVERIFY(!iterator.oldRawData());
    }
    QVERIFY(!horizontalData->isResident());

    {
        KisRandomAccessor2 accessor(
            &dm, 0, 0, false, nullptr);
        accessor.moveTo(0, 0);
        QVERIFY(accessor.rawDataConst());
        QCOMPARE(*accessor.rawDataConst(), value);

        inject();
        accessor.moveTo(64, 0);
        QVERIFY(!accessor.rawDataConst());
        QVERIFY(!accessor.oldRawData());
        QVERIFY(!horizontalData->isResident());

        accessor.moveTo(0, 0);
        QVERIFY(accessor.rawDataConst());
        QCOMPARE(*accessor.rawDataConst(), value);
    }

    QVERIFY(horizontalTile->lockForRead());
    QCOMPARE(horizontalTile->data()[0], horizontalValue);
    horizontalTile->unlockForRead();
}

//#include <valgrind/callgrind.h>

void KisTiledDataManagerTest::benchmarkReadOnlyTileLazy()
{
    quint8 defaultPixel = 0;
    KisTiledDataManager dm(1, &defaultPixel);

    /*
     * See KisTileHashTableTraits2 for more details
     */

    const qint32 numTilesToTest = 0x7fff;

    //CALLGRIND_START_INSTRUMENTATION;

    QBENCHMARK_ONCE {
        for(qint32 i = 0; i < numTilesToTest; i++) {
            KisTileSP tile = dm.getTile(i, i, false);
        }
    }

    //CALLGRIND_STOP_INSTRUMENTATION;
}

class KisSimpleClass : public KisShared
{
    qint64 m_int;
public:
    KisSimpleClass() {
        Q_UNUSED(m_int);
    }
};

typedef KisSharedPtr<KisSimpleClass> KisSimpleClassSP;
void KisTiledDataManagerTest::benchmarkSharedPointers()
{
    const qint32 numIterations = 2 * 1000000;

    //CALLGRIND_START_INSTRUMENTATION;

    QBENCHMARK_ONCE {
        for(qint32 i = 0; i < numIterations; i++) {
            KisSimpleClassSP pointer = new KisSimpleClass;
            pointer = 0;
        }
    }

    //CALLGRIND_STOP_INSTRUMENTATION;
}

void KisTiledDataManagerTest::benchmarkCOWImpl()
{
    const int pixelSize = 8;
    quint8 defaultPixel[pixelSize];
    memset(defaultPixel, 1, pixelSize);

    KisTiledDataManager dm(pixelSize, defaultPixel);


    KisMementoSP memento1 = dm.getMemento();

    /**
     * Imagine a regular image of 4096x2048 pixels
     * (64x32 tiles)
     */
    for (int i = 0; i < 32; i++) {
        for (int j = 0; j < 64; j++) {
            KisTileSP tile = dm.getTile(j, i, true);
            tile->lockForWrite();
            tile->unlockForWrite();
        }
    }

    dm.commit();

    QTest::qSleep(200);

    KisMementoSP memento2 = dm.getMemento();
    QTest::qSleep(200);
    QBENCHMARK_ONCE {

        for (int i = 0; i < 32; i++) {
            for (int j = 0; j < 64; j++) {
                KisTileSP tile = dm.getTile(j, i, true);
                tile->lockForWrite();
                tile->unlockForWrite();
            }
        }

    }
    dm.commit();
}

void KisTiledDataManagerTest::benchmarkCOWNoPooler()
{
    KisTileDataStore::instance()->testingSuspendPooler();
    QTest::qSleep(200);

    benchmarkCOWImpl();

    KisTileDataStore::instance()->testingResumePooler();
    QTest::qSleep(200);
}

void KisTiledDataManagerTest::benchmarkCOWWithPooler()
{
    benchmarkCOWImpl();
}

/******************* Stress job ***********************/

#ifdef LIMIT_LONG_TESTS
#define NUM_CYCLES 10000
#else
#define NUM_CYCLES 100000
#endif

#define NUM_TYPES 12

#define TILE_DIMENSION 64


/**
 * The data manager has partial guarantees of reentrancy. That is
 * you can call any arbitrary number of methods concurrently as long
 * as their access areas do not intersect.
 *
 * Though the rule can be quite tricky -- some of the methods always
 * use entire image as their access area, so they cannot be called
 * concurrently in any circumstances.
 * The examples are: clear(), commit(), rollback() and etc...
 */

#define run_exclusive(lock, _i) for(_i = 0, (lock).lockForWrite(); _i < 1; _i++, (lock).unlock())
#define run_concurrent(lock, _i) for(_i = 0, (lock).lockForRead(); _i < 1; _i++, (lock).unlock())
//#define run_exclusive(lock, _i) while(0)
//#define run_concurrent(lock, _i) while(0)


class KisStressJob : public QRunnable
{
public:
    KisStressJob(KisTiledDataManager &dataManager, QRect rect, QReadWriteLock &_lock, int worker, KisPageStore *store)
        : m_accessRect(rect), dm(dataManager), lock(_lock), m_worker(worker), m_store(store)
    {
    }

    void run() override {
        bool fixedSeed = false;
        const quint32 seed = qEnvironmentVariable("KIS_BR1_SEED").toUInt(&fixedSeed);
        const quint32 workerSeed = fixedSeed
            ? seed + quint32(m_worker) * 0x9e3779b9U
            : quint32(QTime::currentTime().msec());
        QRandomGenerator rng(workerSeed);
        const bool stageProfile = qEnvironmentVariableIntValue("KIS_BR1_STAGE_PROFILE") != 0;
        KisPageStoreDiagnosticRecorder stages(stageProfile, m_store);
        KisBr1StressProfile profile(lock, m_worker);
        // The wrapper measures test-level lock waiting separately from the
        // operation body. Worker-time totals are not additive wall time.
        KisBr1StressProfile &profiledLock = profile;
        for(qint32 i = 0; i < NUM_CYCLES; i++) {
            qint32 type = rng.bounded(NUM_TYPES);
            profile.operation(type);

            qint32 t;

            switch(type) {
            case 0:
                run_concurrent(profiledLock,t) {
                    quint8 *buf;
                    buf = new quint8[dm.pixelSize()];
                    memcpy(buf, dm.defaultPixel(), dm.pixelSize());
                    dm.setDefaultPixel(buf);
                    delete[] buf;
                }
                break;
            case 1:
            case 2:
                run_concurrent(profiledLock,t) {
                    KisTileSP tile;

                    tile = dm.getTile(m_accessRect.x() / TILE_DIMENSION,
                                      m_accessRect.y() / TILE_DIMENSION, false);
                    tile->lockForRead();
                    tile->unlockForRead();
                    tile = dm.getTile(m_accessRect.x() / TILE_DIMENSION,
                                      m_accessRect.y() / TILE_DIMENSION, true);
                    tile->lockForWrite();
                    tile->unlockForWrite();

                    tile = dm.getOldTile(m_accessRect.x() / TILE_DIMENSION,
                                         m_accessRect.y() / TILE_DIMENSION);
                    tile->lockForRead();
                    tile->unlockForRead();
                }
                break;
            case 3:
                run_concurrent(profiledLock,t) {
                    QRect newRect = dm.extent();
		    Q_UNUSED(newRect);
                }
                break;
            case 4:
                run_concurrent(profiledLock,t) {
                    dm.clear(m_accessRect.x(), m_accessRect.y(),
                             m_accessRect.width(), m_accessRect.height(), 4);
                }
                break;
            case 5:
                run_concurrent(profiledLock,t) {
                    quint8 *buf;

                    buf = new quint8[m_accessRect.width() * m_accessRect.height() *
                                     dm.pixelSize()];
                    profile.phase(KisBr1StressProfile::ReadBytes, [&] {
                        dm.readBytes(buf, m_accessRect.x(), m_accessRect.y(),
                                     m_accessRect.width(), m_accessRect.height());
                    });
                    profile.phase(KisBr1StressProfile::WriteBytes, [&] {
                        dm.writeBytes(buf, m_accessRect.x(), m_accessRect.y(),
                                      m_accessRect.width(), m_accessRect.height());
                    });
                    delete[] buf;
                    }
                break;
            case 6:
                run_concurrent(profiledLock,t) {
                    quint8 oddPixel = 13;
                    KisTiledDataManager srcDM(1, &oddPixel);
                    dm.bitBlt(&srcDM, m_accessRect);
                }
                break;
            case 7:
            case 8:
                run_exclusive(profiledLock,t) {
                    profile.phase(KisBr1StressProfile::HistoryBegin, [&] { m_memento = dm.getMemento(); });
                    profile.phase(KisBr1StressProfile::HistoryClear, [&] {
                        dm.clear(m_accessRect.x(), m_accessRect.y(),
                                 m_accessRect.width(), m_accessRect.height(), 2);
                    });
                    profile.phase(KisBr1StressProfile::HistoryCommit, [&] { dm.commit(); });
                    profile.phase(KisBr1StressProfile::Rollback, [&] { dm.rollback(m_memento); });
                    profile.phase(KisBr1StressProfile::Rollforward, [&] { dm.rollforward(m_memento); });
                    profile.phase(KisBr1StressProfile::Purge, [&] { dm.purgeHistory(m_memento); });
                    m_memento = 0;
                }
                break;
            case 9:
                run_exclusive(profiledLock,t) {
                    bool b = dm.hasCurrentMemento();
                    Q_UNUSED(b);
                }
                break;
            case 10:
                run_exclusive(profiledLock,t) {
                    dm.clear();
                }
                break;
            case 11:
                run_exclusive(profiledLock,t) {
                    dm.setExtent(m_accessRect);
                }
                break;
            }
        }
        profile.report(workerSeed);
        if (stageProfile) {
            qInfo().noquote() << "BR1_STAGES" << QJsonDocument(QJsonObject{
                {"worker", m_worker}, {"seed", double(workerSeed)},
                {"scope", "target_manager_only; excludes temporary source managers"},
                {"cpu_clock_available", stages.threadCpuClockAvailable()},
                {"phases", kisBr1StageMetrics(stages)}
            }).toJson(QJsonDocument::Compact);
        }
    }

private:
    KisMementoSP m_memento;
    QRect m_accessRect;
    KisTiledDataManager &dm;
    QReadWriteLock &lock;
    int m_worker;
    KisPageStore *m_store;
};

void KisTiledDataManagerTest::stressTest()
{
    QTest::failOnWarning(QRegularExpression(QStringLiteral(".*SAFE ASSERT.*")));
    quint8 defaultPixel = 0;
    KisTiledDataManager dm(1, &defaultPixel);
    QReadWriteLock lock;

#ifdef LIMIT_LONG_TESTS
    const int numThreads = 8;
    const int numWorkers = 8;
#else
    const int numThreads = 16;
    const int numWorkers = 48;
#endif

    QThreadPool pool;
    const int diagnosticThreads = qEnvironmentVariableIntValue("KIS_BR1_THREADS");
    pool.setMaxThreadCount(diagnosticThreads > 0 ? diagnosticThreads : numThreads);

    QRect accessRect(0,0,512,512);
    for(qint32 i = 0; i < numWorkers; i++) {
        KisStressJob *job = new KisStressJob(dm, accessRect, lock, i, dm.m_pageStoreBackend->store());
        pool.start(job);
        accessRect.translate(512, 0);
    }
    pool.waitForDone();
    if (qEnvironmentVariableIntValue("KIS_BR1_PROFILE") && dm.m_pageStoreBackend) {
        // Counters are captured once after workers finish, outside timed
        // operation bodies. There is no diagnostic lock in the pixel path.
        const KisPageStoreSessionStats stats = dm.m_pageStoreBackend->store()->sessionStats();
        qInfo().noquote() << "BR1_COUNTERS" << QJsonDocument(QJsonObject{
            {"threads", pool.maxThreadCount()},
            {"read_requests", double(stats.readRequestsCreated)},
            {"write_requests", double(stats.writeRequestsCreated)},
            {"default_materializations", double(stats.defaultMaterializationRequests)},
            {"synchronous_adoptions", double(stats.synchronousHostWrites)},
            {"commits", double(stats.committedTransactions)},
            {"registered_pages", double(stats.registeredPages)},
            {"versions", double(stats.pageVersions)},
            {"replicas", double(stats.replicas)}
        }).toJson(QJsonDocument::Compact);
    }
}

template <typename Func>
void applyToRect(const QRect &rc, Func func) {
    for (int y = rc.y(); y < rc.y() + rc.height(); y += KisTileData::HEIGHT) {
        for (int x = rc.x(); x < rc.x() + rc.width(); x += KisTileData::WIDTH) {
            const int col = x / KisTileData::WIDTH;
            const int row = y / KisTileData::HEIGHT;

            func(col, row);
        }
    }
}


class LazyCopyingStressJob : public QRunnable
{
public:
    LazyCopyingStressJob(KisTiledDataManager &dataManager,
                         const QRect &rect,
                         QReadWriteLock &dmExclusiveLock,
                         QReadWriteLock &tileExclusiveLock,
                         int numCycles,
                         bool isWriter)
        : m_accessRect(rect),
          dm(dataManager),
          m_dmExclusiveLock(dmExclusiveLock),
          m_tileExclusiveLock(tileExclusiveLock),
          m_numCycles(numCycles),
          m_isWriter(isWriter)
    {
    }

    void run() override {
        for(qint32 i = 0; i < m_numCycles; i++) {

            //const int epoch = i % 100;
            int t;

            if (m_isWriter && 0) {

            } else {
                const bool shouldClear = i % 5 <= 1; // 40% of requests are clears
                const bool shouldWrite = i % 5 <= 3; // other 40% of requests are writes

                run_concurrent(m_dmExclusiveLock, t) {
                    if (shouldClear) {
                        QWriteLocker locker(&m_tileExclusiveLock);
                        dm.clear(m_accessRect, 4);
                    } else {
                        auto readFunc = [this] (int col, int row) {
                            KisTileSP tile = dm.getTile(col, row, false);
                            tile->lockForRead();
                            tile->unlockForRead();
                        };

                        auto writeFunc = [this] (int col, int row) {
                            KisTileSP tile = dm.getTile(col, row, true);
                            tile->lockForWrite();
                            tile->unlockForWrite();
                        };

                        auto readOldFunc = [this] (int col, int row) {
                            KisTileSP tile = dm.getOldTile(col, row);
                            tile->lockForRead();
                            tile->unlockForRead();
                        };

                        applyToRect(m_accessRect, readFunc);
                        if (shouldWrite) {
                            QReadLocker locker(&m_tileExclusiveLock);
                            applyToRect(m_accessRect, writeFunc);
                        }
                        applyToRect(m_accessRect, readOldFunc);
                    }
                }
            }
        }
    }

private:
    KisMementoSP m_memento;
    QRect m_accessRect;
    KisTiledDataManager &dm;
    QReadWriteLock &m_dmExclusiveLock;
    QReadWriteLock &m_tileExclusiveLock;
    const int m_numCycles;
    const bool m_isWriter;
};

void KisTiledDataManagerTest::stressTestLazyCopying()
{
    quint8 defaultPixel = 0;
    KisTiledDataManager dm(1, &defaultPixel);
    QReadWriteLock dmLock;
    QReadWriteLock tileLock;

#ifdef LIMIT_LONG_TESTS
    const int numCycles = 10000;
    const int numThreads = 8;
    const int numWorkers = 8;
#else
    const int numThreads = 16;
    const int numWorkers = 32;
    const int numCycles = 100000;
#endif

    QThreadPool pool;
    pool.setMaxThreadCount(numThreads);

    const QRect accessRect(0,0,512,256);
    for(qint32 i = 0; i < numWorkers; i++) {
        const bool isWriter = i == 0;
        LazyCopyingStressJob *job = new LazyCopyingStressJob(dm, accessRect,
                                                             dmLock, tileLock,
                                                             numCycles, isWriter);
        pool.start(job);
    }
    pool.waitForDone();
}

void KisTiledDataManagerTest::stressTestExtentsColumn()
{
    KisTiledExtentManager::Data column;

    struct Job : public QRunnable
    {
        Job(KisTiledExtentManager::Data &column, int index, int numCycles)
            : m_column(column), m_index(index), m_numCycles(numCycles) {}

        void run() override {
            for(qint32 i = 0; i < m_numCycles; i++) {
                if (!m_isCreated) {
                    m_column.add(m_index);
                    KIS_SAFE_ASSERT_RECOVER_NOOP(m_column.max() >= m_index);
                    KIS_SAFE_ASSERT_RECOVER_NOOP(m_column.min() <= m_index);
                } else {
                    m_column.remove(m_index);
                }

                m_isCreated = !m_isCreated;
            }
        }

        KisTiledExtentManager::Data &m_column;
        const int m_index;
        const int m_numCycles;
        bool m_isCreated = false;
    };


#ifdef LIMIT_LONG_TESTS
    const int numThreads = 8;
    const int numWorkers = 32;
    const int numCycles = 10000;
#else
    const int numThreads = 16;
    const int numWorkers = 32;
    const int numCycles = 100000;
#endif

    QThreadPool pool;
    pool.setMaxThreadCount(numThreads);

    for(qint32 i = 0; i < numWorkers; i++) {
        const int index = 18 + i / 13;
        pool.start(new Job(column, index, numCycles));
    }
    pool.waitForDone();

    QVERIFY(column.isEmpty());
    QVERIFY(column.max() < column.min()); // really empty :)
}

void KisTiledDataManagerTest::benchmarkQRegion()
{
    QVector<QRect> rects;

    int poison = 0;
    for (int y = 0; y < 8000; y += 64) {
        for (int x = 0; x < 8000; x += 64) {
            if (poison++ % 7 == 0) continue;
            rects << QRect(x, y, 64, 64);
        }
    }

    std::random_device randomDevice;
    std::mt19937 generator(randomDevice());
    std::shuffle(rects.begin(), rects.end(), generator);

    QElapsedTimer timer;
    timer.start();

    QRegion region;

    Q_FOREACH (const QRect &rc, rects) {
        region += rc;
    }

    qDebug() << "compressed rects:" << ppVar(rects.size()) << "-->" << ppVar(region.rectCount());
    qDebug() << "compression time:" << timer.elapsed() << "ms";
}

#include "KisRegion.h"
void KisTiledDataManagerTest::benchmarkKisRegion()
{
    QVector<QRect> rects;

    int poison = 0;

    for (int y = 0; y < 8000; y += 64) {
        for (int x = 0; x < 8000; x += 64) {
            if (poison++ % 7 == 0) continue;
            rects << QRect(x, y, 64, 64);
        }
    }

    std::random_device randomDevice;
    std::mt19937 generator(randomDevice());
    std::shuffle(rects.begin(), rects.end(), generator);

    QElapsedTimer timer;
    timer.start();

    auto endIt = KisRegion::mergeSparseRects(rects.begin(), rects.end());

    qDebug() << "compressed rects:" << ppVar(rects.size()) << "-->" << ppVar(std::distance(rects.begin(), endIt));
    qDebug() << "compression time:" << timer.elapsed() << "ms";
}

inline bool findPoint (const QPoint &pt, const QVector<QRect> &rects)
{
    for (auto it = rects.begin(); it != rects.end(); ++it) {
        if (it->contains(pt)) return true;
    }

    return false;
}

void KisTiledDataManagerTest::benchmarkOverlappedKisRegion()
{
    QVector<QRect> rects;

    int poison = 0;
    for (int y = 0; y < 8000; y += 13) {
        for (int x = 0; x < 8000; x += 17) {
            if (poison++ % 7 == 0) continue;
            rects << QRect(x, y, 13 + (poison % 17) * 7, 17 + (poison % 13) * 7);
        }
    }

    const int originalSize = rects.size();
    QVector<QRect> originalRects = rects;

    std::random_device randomDevice;
    std::mt19937 generator(randomDevice());
    std::shuffle(rects.begin(), rects.end(), generator);

    QElapsedTimer timer;
    timer.start();

#if 0
    // speed reference: executes for about 150 seconds! (150000ms)
    QRegion region;
    Q_FOREACH (const QRect &rc, rects) {
        region += rc;
    }
#endif

    KisRegion::approximateOverlappingRects(rects, 64);

    qDebug() << "deoverlapped rects:" << ppVar(originalSize) << "-->" << ppVar(rects.size());
    qDebug() << "deoverlapping time:" << timer.restart() << "ms";

    KisRegion region(rects);

    qDebug() << "compressed rects:" << ppVar(region.rects().size());
    qDebug() << "compression time:" << timer.restart() << "ms";

    for (auto it1 = rects.begin(); it1 != rects.end(); ++it1) {
        for (auto it2 = std::next(it1); it2 != rects.end(); ++it2) {
            QVERIFY(!it1->intersects(*it2));
        }
    }


#if 0
    /// very slow sanity check for invariant: "all source rects are
    /// represented in the deoverlapped set of rects"

    QVector<QRect> compressedRects = region.rects();
    int i = 0;
    Q_FOREACH(const QRect &rc, originalRects) {
        if (i % 1000 == 0) {
            qDebug() << ppVar(i);
        }

        for (int y = rc.y(); y <= rc.bottom(); ++y) {
            for (int x = rc.x(); x <= rc.right(); ++x) {
                QVERIFY(findPoint(QPoint(x, y), compressedRects));
            }
        }
        i++;
    }
#endif
}

SIMPLE_TEST_MAIN(KisTiledDataManagerTest)

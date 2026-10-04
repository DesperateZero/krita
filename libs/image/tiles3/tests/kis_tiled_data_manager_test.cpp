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
#include <KConfig>

#include "tiles3/kis_tiled_data_manager.h"
#include "tiles3/kis_tile_data_store.h"
#include "tiles3/kis_tile_data_wrapper.h"
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
#include "pagestore/KisPageStoreIteratorReadScope_p.h"
#include "pagestore/KisPageStoreReclamation_p.h"
#include "pagestore/KisPageWriteCoordinator_p.h"

#include "tiles_test_utils.h"
#include "kis_image_config.h"
#include "kis_paint_device.h"
#include <KoColorSpaceRegistry.h>
#include "kis_tile_data_store_test_access.h"
#include "config-limit-long-tests.h"
#include "kis_br1_stress_profile.h"
#include "KisStrokeJobFailureContext.h"

void KisTiledDataManagerTest::initTestCase()
{
    // KConfig 6.7 lazily updates its global filename after test mode changes.
    // Complete both initial construction and that transition on the main
    // thread before any stress worker opens its thread-local configuration.
    KConfig initialConfig;
    KConfig testModeConfig;
}

namespace {

class TestableTiledDataManager : public KisTiledDataManager
{
public:
    using KisTiledDataManager::KisTiledDataManager;
    using KisTiledDataManager::purge;
    using KisTiledDataManager::read;
    using KisTiledDataManager::write;
};

// Own the configuration and every filler through failure/early-test exits.
class DefaultBudgetScope
{
public:
    DefaultBudgetScope()
        : config(false), hard(config.memoryHardLimitPercent()),
          soft(config.memorySoftLimitPercent()), pool(config.memoryPoolLimitPercent()),
          swap(config.maxSwapSize())
    {
        config.setMemoryHardLimitPercent(100.0 * 8.5 / KisImageConfig::totalRAM());
        config.setMemorySoftLimitPercent(0);
        config.setMemoryPoolLimitPercent(0);
        config.setMaxSwapSize(0);
        kisDrainPageStoreReclamation();
        KisTileDataStoreTestAccess::rereadConfig();
    }
    ~DefaultBudgetScope()
    {
        release();
        // Managers created in this scope have already exited, but their
        // charged terminal cleanup may still hold the old process policy.
        kisDrainPageStoreReclamation();
        config.setMemoryHardLimitPercent(hard);
        config.setMemorySoftLimitPercent(soft);
        config.setMemoryPoolLimitPercent(pool);
        config.setMaxSwapSize(swap);
        KisTileDataStoreTestAccess::rereadConfig();
    }
    void saturate(int bpp, const quint8 *pixel)
    {
        while (auto *tile = KisTileDataStore::instance()->createDefaultTileData(bpp, pixel)) {
            tile->ref();
            const bool locked = tile->blockSwapping();
            Q_ASSERT(locked);
            Q_UNUSED(locked);
            fillers.append(tile);
        }
    }
    void release()
    {
        for (auto *tile : std::as_const(fillers)) {
            tile->unblockSwapping();
            tile->deref();
        }
        fillers.clear();
    }
    void setBudgetMiB(qreal mib)
    {
        config.setMemoryHardLimitPercent(100.0 * mib / KisImageConfig::totalRAM());
        KisTileDataStoreTestAccess::rereadConfig();
    }
private:
    KisImageConfig config;
    qreal hard, soft, pool;
    int swap;
    QVector<KisTileData *> fillers;
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

void KisTiledDataManagerTest::testExtentGrowthReusesNegativeCapacity()
{
    KisTiledExtentManager extent;
    constexpr int tiles = 257;
    for (int i = 1; i <= tiles; ++i) {
        extent.notifyTileAdded(-2 * i, -3 * i);
        QCOMPARE(extent.extent(), QRect(QPoint(-2 * i * 64, -3 * i * 64), QPoint(-65, -129)));
        // Capacity is amortized by coordinate span, not insertion count.
        QVERIFY(extent.m_colsData.m_capacity <= 2 * (256 + 2 * i));
        QVERIFY(extent.m_rowsData.m_capacity <= 2 * (256 + 3 * i));
    }
    extent.notifyTileAdded(-2, -3);
    extent.notifyTileRemoved(-2, -3); // preserve the remaining original count
    for (int i = tiles; i >= 1; --i) {
        extent.notifyTileRemoved(-2 * i, -3 * i);
        QCOMPARE(extent.extent(), i == 1 ? QRect{} :
                 QRect(QPoint(-2 * (i - 1) * 64, -3 * (i - 1) * 64), QPoint(-65, -129)));
    }
    extent.notifyTileAdded(800, 1200);
    QCOMPARE(extent.extent(), QRect(800 * 64, 1200 * 64, 64, 64));
    extent.notifyTileRemoved(800, 1200);
    QVERIFY(extent.extent().isEmpty());
}

void KisTiledDataManagerTest::testPageStoreHistoryDirtyExtent()
{
    const quint8 blank = 0;
    const quint8 paint = 0x81;
    const QRect page(-64, 64, 64, 64);
    KisTiledDataManager dm(1, &blank);
    auto painted = dm.getMemento();
    dm.clear(page, &paint);
    QVERIFY(dm.tryCommit());
    QCOMPARE(painted->extent(), page);
    dm.rollback(painted);
    QVERIFY(dm.extent().isEmpty());
    QCOMPARE(painted->extent(), page);
    dm.rollforward(painted);
    QCOMPARE(dm.extent(), page);

    auto removed = dm.getMemento();
    dm.clear(page, &blank);
    QVERIFY(dm.tryCommit());
    QVERIFY(dm.extent().isEmpty());
    QCOMPARE(removed->extent(), page); // removal must invalidate the old pixels
    dm.rollback(removed);
    QCOMPARE(dm.extent(), page);
    dm.rollforward(removed);
    QVERIFY(dm.extent().isEmpty());
    auto empty = dm.getMemento();
    QVERIFY(dm.tryCommit());
    QVERIFY(empty->extent().isEmpty());
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

void KisTiledDataManagerTest::testDefaultBudgetFailure_data()
{
    QTest::addColumn<int>("bpp");
    for (int bpp : {1, 4, 8, 16}) QTest::newRow(qPrintable(QString::number(bpp))) << bpp;
}

void KisTiledDataManagerTest::testPageStoreAbortUnderBudgetPressure_data()
{
    QTest::addColumn<int>("bpp");
    for (int bpp : {1, 4, 8, 16}) QTest::newRow(qPrintable(QString::number(bpp))) << bpp;
}

void KisTiledDataManagerTest::testPageStoreAbortUnderBudgetPressure()
{
    QFETCH(int, bpp);
    DefaultBudgetScope budget;
    const QByteArray oldPixel(bpp, char(0x13)), newPixel(bpp, char(0x57));
    auto bytes = [](const QByteArray &p) { return reinterpret_cast<const quint8 *>(p.constData()); };
    KisTiledDataManager manager(bpp, bytes(oldPixel));
    const auto memento = manager.getMemento();
    QVERIFY(memento);
    manager.setDefaultPixel(bytes(newPixel));
    auto *store = manager.m_pageStoreBackend->store();
    const auto commits = store->sessionStats().committedTransactions;
    budget.saturate(bpp, bytes(oldPixel));
    QString error;
    QVERIFY2(manager.tryAbort(memento, &error), qPrintable(error));
    QVERIFY(!manager.hasCurrentMemento());
    QCOMPARE(store->sessionStats().committedTransactions, commits);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(manager.defaultPixel()), bpp), oldPixel);
    KisSurfaceEpochState state;
    QVERIFY(store->resolveSurfaceState(manager.m_pageStoreBackend->surface(), {}, &state));
    QCOMPARE(state.format.defaultPixel, oldPixel);
    QVERIFY(!manager.tryAbort(memento, &error)); // stale capability never aborts a later stroke
    budget.release();
    const auto next = manager.getMemento();
    QVERIFY(next);
    QVERIFY(!manager.tryAbort(memento, &error));
    QVERIFY(manager.hasCurrentMemento());
    QVERIFY(manager.tryAbort(next, &error));
}

void KisTiledDataManagerTest::testDefaultBudgetFailure()
{
    QFETCH(int, bpp);
    DefaultBudgetScope budget;
    const QByteArray oldPixel(bpp, char(0x13)), newPixel(bpp, char(0x57));
    const auto *oldBytes = reinterpret_cast<const quint8 *>(oldPixel.constData());
    const auto *newBytes = reinterpret_cast<const quint8 *>(newPixel.constData());
    KisTiledDataManager manager(bpp, oldBytes);
    QVERIFY(manager.m_pageStoreBackend && manager.m_pageStoreBackend->isOperational());
    auto agrees = [&](const QByteArray &expected) {
        KisSurfaceEpochState state;
        return QByteArray(reinterpret_cast<const char *>(manager.defaultPixel()), bpp) == expected
            && manager.m_pageStoreBackend->store()->resolveSurfaceState(
                manager.m_pageStoreBackend->surface(), {}, &state)
            && state.format.defaultPixel == expected;
    };
    auto reads = [&](KisTiledDataManager &source, const QByteArray &expected) {
        QByteArray result(bpp, char(0xff));
        source.readBytes(reinterpret_cast<quint8 *>(result.data()), 4096, 4096, 1, 1);
        return result == expected;
    };
    budget.saturate(bpp, oldBytes);
    {
        KisStrokeJobFailureContext failure(true);
        manager.setDefaultPixel(newBytes);
        QVERIFY(failure.failed());
    }
    QVERIFY(agrees(oldPixel));
    manager.setDefaultPixel(oldBytes); // no-op still works at the hard limit
    QVERIFY(agrees(oldPixel));
    {
        KisTiledDataManager clone(manager); // reuse admitted default backing
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(clone.defaultPixel()), bpp), oldPixel);
    }
    budget.release();
    QVERIFY(reads(manager, oldPixel));
    auto memento = manager.getMemento();
    QVERIFY(memento);
    manager.setDefaultPixel(newBytes);
    manager.commit();
    QVERIFY(agrees(newPixel));
    QVERIFY(reads(manager, newPixel));

    budget.saturate(bpp, oldBytes);
    manager.rollback(memento);
    QVERIFY(agrees(newPixel)); // failed undo never moves canonical history
    budget.release();
    manager.rollback(memento);
    QVERIFY(agrees(oldPixel));
    QVERIFY(reads(manager, oldPixel));

    budget.saturate(bpp, oldBytes);
    manager.rollforward(memento);
    QVERIFY(agrees(oldPixel));
    budget.release();
    manager.rollforward(memento);
    QVERIFY(agrees(newPixel));
    QVERIFY(reads(manager, newPixel));
    manager.clear();
    QVERIFY(reads(manager, newPixel));
    KisTiledDataManager clone(manager);
    QVERIFY(reads(clone, newPixel));
}

void KisTiledDataManagerTest::testConstructorBudgetFailure()
{
    DefaultBudgetScope budget;
    const quint8 pixel[4] = {0x12, 0x34, 0x56, 0x78};
    {
        KisTiledDataManager existing(4, pixel);
        budget.saturate(4, pixel);
        const qint64 full = KisTileDataStore::instance()->memoryMetric();
        for (int i = 0; i < 3; ++i) {
            QVERIFY_EXCEPTION_THROWN(KisTiledDataManager(4, pixel), std::bad_alloc);
            QCOMPARE(KisTileDataStore::instance()->memoryMetric(), full);
        }
        KisTiledDataManager clone(existing);
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(clone.defaultPixel()), 4),
                 QByteArray(reinterpret_cast<const char *>(pixel), 4));
        QCOMPARE(KisTileDataStore::instance()->memoryMetric(), full);
        budget.release();
        KisTiledDataManager retry(4, pixel);
        KisTiledDataManager retryCopy(retry);
        quint8 result[4]{};
        retryCopy.readBytes(result, 0, 0, 1, 1);
        QVERIFY(!std::memcmp(result, pixel, 4));
    }
    budget.setBudgetMiB(0);
    QVERIFY_EXCEPTION_THROWN(KisTiledDataManager(4, pixel), std::bad_alloc);
    budget.setBudgetMiB(1.1);
    const QByteArray largePixel(512, char(0x42)); // a 2 MiB tile cannot fit
    QVERIFY_EXCEPTION_THROWN(KisTiledDataManager(512,
        reinterpret_cast<const quint8 *>(largePixel.constData())), std::bad_alloc);
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

void KisTiledDataManagerTest::testPageStoreCommitCleanupAllowsReader_data()
{
    QTest::addColumn<int>("bpp");
    QTest::addColumn<bool>("packed");
    QTest::addColumn<bool>("warm");
    for (int bpp : {1, 4, 8, 16})
        for (bool packed : {false, true})
            for (bool warm : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("b%1-packed%2-warm%3")
                    .arg(bpp).arg(packed).arg(warm))) << bpp << packed << warm;
}

void KisTiledDataManagerTest::testPageStoreCommitCleanupAllowsReader()
{
    QFETCH(int, bpp); QFETCH(bool, packed); QFETCH(bool, warm);
    const QByteArray blank(bpp, 0), old(bpp, char(0x31)), value(bpp, char(0x61));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    if (warm) dm.writeBytes(reinterpret_cast<const quint8 *>(old.constData()), 0, 0, 1, 1);
    auto *backend = dm.m_pageStoreBackend;
    auto *store = backend->store();
    auto before = backend->captureReadView(); QVERIFY(before.isValid());
    struct Observation {
        QSemaphore finished;
        std::thread reader;
        bool completedInPhase = false;
        bool coherent = false;
    } observations[1];
    int seen[1] = {};
    int publications = 0;
    bool rootPublished = false;
    auto join = qScopeGuard([&] {
        for (auto &observation : observations)
            if (observation.reader.joinable()) observation.reader.join();
    });
    KisPageStoreDiagnosticRecorder recorder(true, store);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        using Phase = KisPageStoreDiagnosticPhase;
        if (phase == Phase::CommitRootPublication) {
            ++publications;
            rootPublished = true;
            return;
        }
        if (phase != Phase::CommitCleanup) return;
        // Rejected metadata installs also dispose their prepared candidates.
        // Only accepted root cleanup must release the publication gate.
        if (!std::exchange(rootPublished, false) || seen[0]++) return;
        constexpr int index = 0;
        auto &observation = observations[index];
        observation.reader = std::thread([&, index] {
            auto view = backend->captureReadView();
            KisSurfaceEpochState state;
            KisPageStoreReadPage page(store, view, {backend->surface(), {0, 0}});
            observations[index].coherent = view.isValid() && page.data() &&
                QByteArray(reinterpret_cast<const char *>(page.data()), bpp) == value &&
                view.resolveSurfaceState(backend->surface(), &state) &&
                state.contentExtent == QRect(0, 0, 64, 64);
            observations[index].finished.release();
        });
        // Do not join here: on regression the reader is blocked by the outer
        // publication gate until this observer and the commit return.
        observation.completedInPhase = observation.finished.tryAcquire(1, 5000);
    });
    bool succeeded = false;
    if (packed) {
        KisStrokeJobFailureContext failure(true);
        dm.writeBytes(reinterpret_cast<const quint8 *>(value.constData()), 0, 0, 1, 1);
        succeeded = !failure.failed();
    } else {
        succeeded = dm.writePageStoreOperation({QRect(0, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(0, 0);
            if (!cursor->rawData()) return false;
            std::memcpy(cursor->rawData(), value.constData(), size_t(bpp));
            return true;
        }) == KisPageStoreWriteOperationResult::Succeeded;
    }
    recorder.setPhaseObserver({});
    for (auto &observation : observations)
        if (observation.reader.joinable()) observation.reader.join();
    QVERIFY(succeeded);
    QCOMPARE(publications, 1);
    QCOMPARE(seen[0], 1);
    QVERIFY2(observations[0].completedInPhase, "successful commit cleanup blocked reader");
    QVERIFY(observations[0].coherent);
    KisPageStoreReadPage oldPage(store, before, {backend->surface(), {0, 0}});
    QVERIFY(oldPage.data());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(oldPage.data()), bpp), warm ? old : blank);
    auto after = backend->captureReadView(); QVERIFY(after.isValid());
    KisPageStoreReadPage newPage(store, after, {backend->surface(), {0, 0}});
    QVERIFY(newPage.data());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(newPage.data()), bpp), value);
    QCOMPARE(store->sessionStats().activeTransactions, qsizetype(0));
}

void KisTiledDataManagerTest::testPageStoreOverlayExtent_data()
{
    testPageStoreCommitCleanupAllowsReader_data();
}

void KisTiledDataManagerTest::testPageStoreOverlayExtent()
{
    QFETCH(int, bpp); QFETCH(bool, packed); QFETCH(bool, warm);
    const QByteArray blank(bpp, 0), old(bpp, char(0x31)), value(bpp, char(0x61));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    if (warm) dm.writeBytes(reinterpret_cast<const quint8 *>(old.constData()), 0, 0, 1, 1);
    auto *backend = dm.m_pageStoreBackend;
    auto *store = backend->store();
    auto before = backend->captureReadView();
    bool coherent = false;
    int seen = 0;
    KisPageStoreDiagnosticRecorder recorder(true, store);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::MutationSealCleanup) return;
        ++seen;
        auto view = backend->captureReadView();
        auto page = view.readResidentPage({backend->surface(), {2, 0}});
        KisSurfaceEpochState state;
        coherent = page.isValid() && QByteArray(static_cast<const char *>(page.data()), bpp) == value &&
            view.resolveSurfaceState(backend->surface(), &state) &&
            state.contentExtent == (warm ? QRect(0, 0, 192, 64) : QRect(128, 0, 64, 64));
    });
    if (packed) {
        KisStrokeJobFailureContext failure(true);
        dm.writeBytes(reinterpret_cast<const quint8 *>(value.constData()), 128, 0, 1, 1);
        QVERIFY(!failure.failed());
    } else {
        QCOMPARE(dm.writePageStoreOperation({QRect(128, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(128, 0);
            if (!cursor->rawData()) return false;
            std::memcpy(cursor->rawData(), value.constData(), size_t(bpp));
            return true;
        }), KisPageStoreWriteOperationResult::Succeeded);
    }
    recorder.setPhaseObserver({});
    QCOMPARE(seen, 1);
    QVERIFY(coherent);
    KisSurfaceEpochState state;
    QVERIFY(before.resolveSurfaceState(backend->surface(), &state));
    QCOMPARE(state.contentExtent, warm ? QRect(0, 0, 64, 64) : QRect());
    auto oldPage = before.readResidentPage({backend->surface(), {2, 0}});
    QVERIFY(oldPage.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(oldPage.data()), bpp), blank);
}

void KisTiledDataManagerTest::testPageStoreOverlayExtentSibling_data()
{
    QTest::addColumn<int>("bpp");
    QTest::addColumn<bool>("removal");
    for (int bpp : {1, 4, 8, 16}) for (bool removal : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("b%1-removal%2").arg(bpp).arg(removal))) << bpp << removal;
}

void KisTiledDataManagerTest::testPageStoreOverlayExtentSibling()
{
    QFETCH(int, bpp); QFETCH(bool, removal);
    QByteArray blank(bpp, 0), old(bpp, char(0x31));
    KisTiledDataManagerPageStoreBackend backend;
    QString error;
    QVERIFY(backend.configure(bpp, reinterpret_cast<const quint8 *>(blank.constData()), &error));
    QVERIFY(backend.fillRect(QRect(-128, 0, 1, 1), old, &error));
    QVERIFY(backend.fillRect(QRect(128, 0, 1, 1), old, &error));
    auto *store = backend.store();
    auto tx = store->beginCurrentTransaction();
    const auto selection = KisPageReadView::transactionOverlay(tx.id);
    auto before = store->captureReadView(selection);
    auto segment = store->beginMutation(tx, &error);
    if (removal) QVERIFY(segment.removePage({backend.surface(), {-2, 0}}, &error));
    else {
        auto write = segment.beginWrite({backend.surface(), {-4, 0}}, KisPageWriteMode::PreserveContents, &error);
        QVERIFY(write.isValid()); std::memset(write.data(), 0x61, size_t(write.byteSize()));
    }
    bool nested = false, siblingSucceeded = false;
    KisPageStoreDiagnosticRecorder recorder(true, store);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::MutationSealMetadataPrepare || nested) return;
        nested = true;
        // Publish a disjoint sibling after the first candidate was prepared.
        auto sibling = store->beginMutation(tx, &error);
        {
            auto write = sibling.beginWrite({backend.surface(), {4, 0}}, KisPageWriteMode::PreserveContents, &error);
            if (!write.isValid()) return;
            std::memset(write.data(), 0x71, size_t(write.byteSize()));
        }
        siblingSucceeded = sibling.seal(&error);
    });
    QVERIFY2(segment.seal(&error), qPrintable(error));
    recorder.setPhaseObserver({});
    QVERIFY(nested && siblingSucceeded);
    auto after = store->captureReadView(selection);
    KisSurfaceEpochState state;
    QVERIFY(after.resolveSurfaceState(backend.surface(), &state));
    QCOMPARE(state.contentExtent, removal ? QRect(128, 0, 192, 64) : QRect(-256, 0, 576, 64));
    QVERIFY(before.resolveSurfaceState(backend.surface(), &state));
    QCOMPARE(state.contentExtent, QRect(-128, 0, 320, 64));
    for (int column : {-4, -2, 2, 4}) {
        auto page = after.readResidentPage({backend.surface(), {column, 0}});
        QVERIFY(page.isValid());
        const quint8 expected = column == 4 ? 0x71 : column == 2 ? 0x31 :
            column == -2 ? (removal ? 0 : 0x31) : (removal ? 0 : 0x61);
        QCOMPARE(static_cast<const quint8 *>(page.data())[0], expected);
    }
    QVERIFY(store->commit(tx, store->preparedPages(tx)).isValid());
}

void KisTiledDataManagerTest::testPageStoreOverlayExtentRangeFailure()
{
    const quint8 blank = 0;
    KisTiledDataManagerPageStoreBackend backend;
    QString error;
    QVERIFY(backend.configure(1, &blank, &error));
    auto *store = backend.store();
    auto tx = store->beginCurrentTransaction();
    const auto selection = KisPageReadView::transactionOverlay(tx.id);
    const int left = std::numeric_limits<int>::min() / 64;
    const int right = std::numeric_limits<int>::max() / 64;
    auto fill = [&](int column) {
        auto segment = store->beginMutation(tx, &error);
        {
            auto write = segment.beginWrite({backend.surface(), {column, 0}}, KisPageWriteMode::PreserveContents, &error);
            if (!write.isValid()) return false;
            static_cast<quint8 *>(write.data())[0] = 0x61;
        }
        return segment.seal(&error);
    };
    QVERIFY(fill(left));
    auto before = store->captureReadView(selection);
    QVERIFY(!fill(right));
    QVERIFY(error.contains(QStringLiteral("QRect range")));
    auto after = store->captureReadView(selection);
    for (const auto *view : {&before, &after}) {
        KisSurfaceEpochState state;
        QVERIFY(view->resolveSurfaceState(backend.surface(), &state));
        QCOMPARE(state.contentExtent, QRect(std::numeric_limits<int>::min(), 0, 64, 64));
        auto old = view->readResidentPage({backend.surface(), {left, 0}});
        auto missing = view->readResidentPage({backend.surface(), {right, 0}});
        QVERIFY(old.isValid() && missing.isValid());
        QCOMPARE(static_cast<const quint8 *>(old.data())[0], quint8(0x61));
        QCOMPARE(static_cast<const quint8 *>(missing.data())[0], quint8(0));
    }
    QVERIFY(store->commit(tx, store->preparedPages(tx)).isValid());
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

void KisTiledDataManagerTest::testPageStoreHistoryMutation_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("repeats"); QTest::addColumn<bool>("abort");
    for (int bpp : {1, 4, 8, 16}) for (int repeats : {1, 10, 1000}) for (bool abort : {false, true})
        QTest::newRow(qPrintable(QString("B%1-D%2-abort%3").arg(bpp).arg(repeats).arg(abort))) << bpp << repeats << abort;
}

void KisTiledDataManagerTest::testPageStoreHistoryMutation()
{
    QFETCH(int, bpp); QFETCH(int, repeats); QFETCH(bool, abort);
    const QByteArray initial(bpp, char(0x31));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(initial.constData()));
    dm.writeBytes(reinterpret_cast<const quint8 *>(initial.constData()), 0, 0, 1, 1);
    auto *backend = dm.m_pageStoreBackend; auto *store = backend->store();
    const auto memento = dm.getMemento(); QVERIFY(memento);
    const auto baseline = store->mutationStatistics();
    QString error; QVERIFY2(dm.beginStrokeMutation(memento, &error), qPrintable(error));
    QVERIFY(dm.beginStrokeMutation(memento)); // same owner, same original session
    const auto captures = store->readScopeStatistics().capturedReadViewsCreated;
    const QVector<QRect> targets{QRect(0, 0, 1, 1), QRect(65 * 64, 0, 1, 1)};
    for (int i = 0; i < repeats; ++i) {
        bool beforeCorrect = true;
        const auto result = dm.writePageStoreOperation(targets, [&](KisPixelWriteCursor *cursor) {
            for (int page : {0, 65}) {
                cursor->moveTo(page * 64, 0); if (!cursor->rawData()) return false;
                const quint8 expected = i ? (page ? 0x52 : 0x51) : 0x31;
                beforeCorrect &= QByteArray(reinterpret_cast<const char *>(cursor->rawData()), bpp) == QByteArray(bpp, char(expected));
                std::memset(cursor->rawData(), page ? 0x52 : 0x51, size_t(bpp));
            }
            return true;
        }, &error, memento);
        QCOMPARE(result, KisPageStoreWriteOperationResult::Succeeded); QVERIFY(beforeCorrect);
    }
    QCOMPARE(store->mutationStatistics().operationSessionsCreated - baseline.operationSessionsCreated, quint64(1));
    QCOMPARE(store->mutationStatistics().generationsReserved - baseline.generationsReserved, quint64(2));
    QCOMPARE(store->mutationStatistics().pagesSealed - baseline.pagesSealed, quint64(0));
    QCOMPARE(store->readScopeStatistics().capturedReadViewsCreated, captures);
    // Ordinary exact reads remain on the last complete cut. Prime a wrapper
    // with that old view to prove checkpoint invalidates it without a payload read.
    bool present = false; auto tile = dm.getReadOnlyTileLazy(0, 0, present); QVERIFY(tile);
    QVERIFY(tile->lockForRead()); QCOMPARE(tile->data()[0], quint8(0x31)); tile->unlockForRead();
    QVERIFY2(dm.checkpointStrokeMutation(memento, &error), qPrintable(error));
    auto frozen = backend->captureReadView(); QVERIFY(frozen.isValid());
    KisSurfaceEpochState frozenState;
    QVERIFY(frozen.resolveSurfaceState(backend->surface(), &frozenState));
    QCOMPARE(frozenState.contentExtent, QRect(0, 0, 66 * 64, 64));
    for (int page : {0, 65}) {
        auto read = frozen.readResidentPage({backend->surface(), {page, 0}}); QVERIFY(read.isValid());
        QCOMPARE(QByteArray(static_cast<const char *>(read.data()), bpp), QByteArray(bpp, char(page ? 0x52 : 0x51)));
    }
    QVERIFY(tile->lockForRead()); QCOMPARE(tile->data()[0], quint8(0x51)); tile->unlockForRead();
    QCOMPARE(dm.writePageStoreOperation({QRect(0, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
        cursor->moveTo(0, 0); if (!cursor->rawData()) return false;
        std::memset(cursor->rawData(), 0x71, size_t(bpp)); return true;
    }, &error, memento), KisPageStoreWriteOperationResult::Succeeded);
    if (abort) QVERIFY2(dm.tryAbort(memento, &error), qPrintable(error));
    else QVERIFY2(dm.tryCommit(&error), qPrintable(error));
    QByteArray pixel(bpp, char(0)); dm.readBytes(reinterpret_cast<quint8 *>(pixel.data()), 0, 0, 1, 1);
    QCOMPARE(pixel, QByteArray(bpp, char(abort ? 0x31 : 0x71)));
    auto old = frozen.readResidentPage({backend->surface(), {0, 0}}); QVERIFY(old.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(old.data()), bpp), QByteArray(bpp, char(0x51)));
    QCOMPARE(store->mutationStatistics().operationSessionsCreated - baseline.operationSessionsCreated, quint64(1));
    QCOMPARE(store->mutationStatistics().generationsReserved - baseline.generationsReserved, quint64(3));
    // Three cold generations each require one preflight pin. Warm callbacks
    // reuse admitted backing; preparation must not add one pin per dab.
    const quint64 expectedPins = quint64(repeats * 2 + 1) + 3;
    QCOMPARE(store->mutationStatistics().writablePinsAcquired - baseline.writablePinsAcquired, expectedPins);
    QCOMPARE(store->mutationStatistics().writablePinsReleased - baseline.writablePinsReleased, expectedPins);
    if (!abort) {
        dm.rollback(memento); dm.readBytes(reinterpret_cast<quint8 *>(pixel.data()), 0, 0, 1, 1); QCOMPARE(pixel, initial);
        dm.rollforward(memento); dm.readBytes(reinterpret_cast<quint8 *>(pixel.data()), 0, 0, 1, 1); QCOMPARE(pixel, QByteArray(bpp, char(0x71)));
        dm.purgeHistory(memento);
    }
}

void KisTiledDataManagerTest::testPageStoreHistoryMutationFailure_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("cut");
    for (int bpp : {1, 4, 8, 16}) for (bool cut : {false, true})
        QTest::newRow(qPrintable(QString("B%1-cut%2").arg(bpp).arg(cut))) << bpp << cut;
}

void KisTiledDataManagerTest::testPageStoreHistoryMutationFailure()
{
    QFETCH(int, bpp); QFETCH(bool, cut);
    QByteArray initial(bpp, char(0x31)); KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(initial.constData()));
    const auto owner = dm.getMemento(); QVERIFY(dm.beginStrokeMutation(owner));
    const auto write = [&](quint8 value, bool succeed) {
        return dm.writePageStoreOperation({QRect(0, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(0, 0); if (!cursor->rawData()) return false;
            std::memset(cursor->rawData(), value, size_t(bpp)); return succeed;
        }, nullptr, owner);
    };
    QCOMPARE(write(0x51, true), KisPageStoreWriteOperationResult::Succeeded);
    KisCapturedReadView frozen; if (cut) { QVERIFY(dm.checkpointStrokeMutation(owner)); frozen = dm.m_pageStoreBackend->captureReadView(); QVERIFY(frozen.isValid()); }
    QCOMPARE(write(0x71, false), KisPageStoreWriteOperationResult::Failed);
    QCOMPARE(write(0x91, true), KisPageStoreWriteOperationResult::Failed);
    QVERIFY(!dm.tryCommit()); QVERIFY(!dm.tryCommit()); QVERIFY(!dm.beginStrokeMutation(owner));
    QVERIFY(dm.tryAbort(owner)); QByteArray actual(bpp, 0);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1); QCOMPARE(actual, initial);
    if (cut) {
        auto read = frozen.readResidentPage({dm.m_pageStoreBackend->surface(), {0, 0}}); QVERIFY(read.isValid());
        QCOMPARE(QByteArray(static_cast<const char *>(read.data()), bpp), QByteArray(bpp, char(0x51)));
    }
}

void KisTiledDataManagerTest::testPageStoreHistoryMutationClosing_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("abort");
    for (int bpp : {1, 4, 8, 16}) for (bool abort : {false, true})
        QTest::newRow(qPrintable(QString("B%1-abort%2").arg(bpp).arg(abort))) << bpp << abort;
}

void KisTiledDataManagerTest::testPageStoreHistoryMutationClosing()
{
    QFETCH(int, bpp); QFETCH(bool, abort);
    QByteArray initial(bpp, char(0x31)); KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(initial.constData()));
    const auto owner = dm.getMemento(); QVERIFY(dm.beginStrokeMutation(owner));
    QSemaphore entered, release; bool wrote = false;
    std::thread worker([&] {
        wrote = dm.writePageStoreOperation({QRect(0, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(0, 0); if (!cursor->rawData()) { entered.release(); return false; }
            std::memset(cursor->rawData(), 0x71, size_t(bpp)); entered.release(); release.acquire(); return true;
        }, nullptr, owner) == KisPageStoreWriteOperationResult::Succeeded;
    });
    const auto cleanup = qScopeGuard([&] { release.release(); if (worker.joinable()) worker.join(); });
    QVERIFY(entered.tryAcquire(1, 5000));
    QVERIFY(!dm.checkpointStrokeMutation(owner)); // nonterminal refusal
    QVERIFY(!(abort ? dm.tryAbort(owner) : dm.tryCommit()));
    QVERIFY(!dm.beginStrokeMutation(owner));
    int calls = 0;
    QCOMPARE(dm.writePageStoreOperation({}, [&](KisPixelWriteCursor *) { ++calls; return true; }, nullptr, owner), KisPageStoreWriteOperationResult::Failed);
    QCOMPARE(calls, 0);
    release.release(); worker.join(); QCOMPARE(wrote, !abort);
    QVERIFY(abort ? dm.tryAbort(owner) : dm.tryCommit());
    QByteArray actual(bpp, 0); dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1);
    QCOMPARE(actual, QByteArray(bpp, char(abort ? 0x31 : 0x71)));
    if (!abort) dm.purgeHistory(owner);
}

void KisTiledDataManagerTest::testPageStoreHistoryMutationOwner_data()
{
    QTest::addColumn<int>("bpp"); for (int bpp : {1, 4, 8, 16}) QTest::newRow(qPrintable(QString::number(bpp))) << bpp;
}

void KisTiledDataManagerTest::testPageStoreHistoryMutationOwner()
{
    QFETCH(int, bpp); QByteArray initial(bpp, char(0x31));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(initial.constData())), other(bpp, reinterpret_cast<const quint8 *>(initial.constData()));
    const auto owner = dm.getMemento(), foreign = other.getMemento();
    QVERIFY(!dm.beginStrokeMutation({})); QVERIFY(!dm.beginStrokeMutation(foreign)); QVERIFY(dm.beginStrokeMutation(owner));
    int calls = 0; const auto body = [&](KisPixelWriteCursor *) { ++calls; return true; };
    for (const auto &rects : {QVector<QRect>{}, QVector<QRect>{QRect(0, 0, 1, 1)}})
        QCOMPARE(dm.writePageStoreOperation(rects, body, nullptr, foreign), KisPageStoreWriteOperationResult::Failed);
    QCOMPARE(dm.writePageStoreOperation({QRect(0, 0, 1, 1)}, body), KisPageStoreWriteOperationResult::Failed);
    QVERIFY(!dm.checkpointStrokeMutation(foreign)); QCOMPARE(calls, 0);
    QVERIFY(dm.tryCommit()); const auto next = dm.getMemento(); QVERIFY(next); QVERIFY(dm.beginStrokeMutation(next));
    QCOMPARE(dm.writePageStoreOperation({}, body, nullptr, owner), KisPageStoreWriteOperationResult::Failed);
    QVERIFY(!dm.checkpointStrokeMutation(owner)); QCOMPARE(calls, 0);
    QVERIFY(dm.tryAbort(next)); dm.purgeHistory(owner); QVERIFY(other.tryAbort(foreign));
}

void KisTiledDataManagerTest::testPageStoreMetadataRefresh_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("history"); QTest::addColumn<bool>("heldReader");
    for (int bpp : {1, 4, 8, 16}) for (bool history : {false, true}) for (bool held : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("bpp%1-history%2-held%3").arg(bpp).arg(history).arg(held)))
            << bpp << history << held;
}

void KisTiledDataManagerTest::testPageStoreMetadataRefresh()
{
    QFETCH(int, bpp); QFETCH(bool, history); QFETCH(bool, heldReader);
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    dm.clear(0, 0, 64, 64, reinterpret_cast<const quint8 *>(initial.constData()));
    auto tile = dm.getTile(0, 0, false); QVERIFY(tile);
    auto *store = dm.m_pageStoreBackend->store();
    auto memento = history ? dm.getMemento() : KisMementoSP{};
    bool pinned = heldReader && tile->lockForRead();
    const auto release = qScopeGuard([&] { if (pinned) tile->unlockForRead(); });
    QCOMPARE(pinned, heldReader);
    const auto views = store->readScopeStatistics();
    const auto requests = store->sessionStats();
    QString error;
    const auto result = dm.writePageStoreOperation({QRect(0, 0, 128, 1)}, [&](KisPixelWriteCursor *cursor) {
        for (int column : {0, 1}) {
            cursor->moveTo(column * 64, 0);
            if (!cursor->rawData()) return false;
            memset(cursor->rawData(), 0x71 + column, size_t(bpp));
        }
        return true;
    }, &error);
    QCOMPARE(result, KisPageStoreWriteOperationResult::Succeeded);
    // Compatibility existence/cache invalidation is not an exact-read cut.
    QCOMPARE(store->readScopeStatistics().capturedReadViewsCreated, views.capturedReadViewsCreated);
    QCOMPARE(store->sessionStats().readRequestsCreated, requests.readRequestsCreated);
    QVERIFY(dm.m_hashTable->getExistingTile(1, 0));
    QCOMPARE(dm.extent(), QRect(0, 0, 128, 64));
    if (pinned) {
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(tile->data()), bpp), initial);
        tile->unlockForRead(); pinned = false;
    }
    // An already retained wrapper must resolve current on its next real read;
    // no getTile() call is allowed to hide a missed invalidation here.
    QVERIFY(tile->lockForRead());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(tile->data()), bpp), QByteArray(bpp, char(0x71)));
    tile->unlockForRead();
    const auto warmed = store->readScopeStatistics();
    for (int i = 0; i < 1000; ++i) {
        QVERIFY(tile->lockForRead()); tile->unlockForRead();
    }
    QCOMPARE(store->readScopeStatistics().capturedReadViewsCreated, warmed.capturedReadViewsCreated);
    auto added = dm.getTile(1, 0, false); QVERIFY(added->lockForRead());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(added->data()), bpp), QByteArray(bpp, char(0x72)));
    added->unlockForRead();
    if (history) {
        dm.commit(); dm.rollback(memento);
        QByteArray actual(bpp, Qt::Uninitialized);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1); QCOMPARE(actual, initial);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 64, 0, 1, 1); QCOMPARE(actual, blank);
        dm.rollforward(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1); QCOMPARE(actual, QByteArray(bpp, char(0x71)));
        dm.purgeHistory(memento);
    }
}

void KisTiledDataManagerTest::testExtentStoragePreparation()
{
    KisTiledExtentManager extent;
    extent.notifyTileAdded(0, 0);
    const QRect original = extent.extent();
    for (const QRect range : {QRect(), QRect(-1, -1, 256, 256), QRect(-257, 600, 515, 2),
                              QRect(-32766, -32766, 65533, 65533)}) {
        QVERIFY(extent.prepareTileRange(range));
        QCOMPARE(extent.extent(), original);
        if (range.isEmpty()) continue;
        for (const QPoint point : {range.topLeft(), range.bottomRight()}) {
            extent.notifyTileAdded(point.x(), point.y());
            const QRect tile(point.x() * 64, point.y() * 64, 64, 64);
            QVERIFY(extent.extent().contains(tile));
            extent.notifyTileRemoved(point.x(), point.y());
            QCOMPARE(extent.extent(), original);
        }
    }
    auto *cols = extent.m_colsData.m_buffer.get();
    auto *rows = extent.m_rowsData.m_buffer.get();
    const auto colCapacity = extent.m_colsData.m_capacity, rowCapacity = extent.m_rowsData.m_capacity;
    for (int i = 0; i < 1000; ++i) QVERIFY(extent.prepareTileRange(QRect(-1000, -1000, 2001, 2001)));
    QCOMPARE(extent.m_colsData.m_buffer.get(), cols); QCOMPARE(extent.m_rowsData.m_buffer.get(), rows);
    // A valid first axis cannot leak an active geometry change when the
    // second axis cannot be represented. No enormous allocation is attempted.
    QVERIFY(!extent.prepareTileRange(QRect(100000, std::numeric_limits<qint32>::max(), 1, 1)));
    QVERIFY(!extent.prepareTileRange(QRect(std::numeric_limits<qint32>::min(), 100000, 1, 1)));
    QCOMPARE(extent.m_colsData.m_buffer.get(), cols); QCOMPARE(extent.m_rowsData.m_buffer.get(), rows);
    QCOMPARE(extent.m_colsData.m_capacity, colCapacity); QCOMPARE(extent.m_rowsData.m_capacity, rowCapacity);
    QCOMPARE(extent.extent(), original);
    extent.notifyTileRemoved(0, 0); QCOMPARE(extent.extent(), QRect());

    // Storage contains no earlier snapshot: installation copies all current
    // counts, including a same-geometry edit after allocating the candidate.
    KisTiledExtentManager::Data data;
    data.add(3);
    auto growth = data.planGrowth(-500, 500); growth.allocate();
    data.add(3); data.add(7);
    QVERIFY(data.canInstall(growth, -500, 500)); data.install(growth, -500, 500);
    data.remove(3); QCOMPARE(data.min(), qint32(3));
    data.remove(3); QCOMPARE(data.min(), qint32(7));
    data.remove(7); QVERIFY(data.isEmpty());
    auto stale = data.planGrowth(-2000, 2000); stale.allocate();
    data.add(10000);
    QVERIFY(!data.canInstall(stale, -2000, 2000));
    data.prepare(-2000, 2000); data.add(-2000);
    QCOMPARE(data.min(), qint32(-2000)); QCOMPARE(data.max(), qint32(10000));
    data.remove(-2000); data.remove(10000); QVERIFY(data.isEmpty());
}

void KisTiledDataManagerTest::testExtentStorageConcurrent()
{
    KisTiledExtentManager extent;
    extent.notifyTileAdded(0, 0);
    std::atomic<bool> good{true};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 8; ++worker) workers.emplace_back([&, worker] {
        const int col = (worker % 2 ? -1 : 1) * (400 + worker * 700);
        const int row = -col + 13;
        for (int i = 0; i < 1000; ++i) {
            if (!extent.prepareTileRange(QRect(col, row, 1, 1))) { good = false; return; }
            extent.notifyTileAdded(col, row);
            extent.notifyTileRemoved(col, row);
        }
    });
    for (auto &worker : workers) worker.join();
    QVERIFY(good.load()); QCOMPARE(extent.extent(), QRect(0, 0, 64, 64));
    extent.notifyTileRemoved(0, 0); QCOMPARE(extent.extent(), QRect());
}

void KisTiledDataManagerTest::testPageStoreExtentStorage_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("sign"); QTest::addColumn<int>("operation");
    for (int bpp : {1, 4, 8, 16}) for (int sign : {-1, 1}) for (int operation = 0; operation < 6; ++operation)
        QTest::newRow(qPrintable(QStringLiteral("B%1-sign%2-op%3").arg(bpp).arg(sign).arg(operation)))
            << bpp << sign << operation;
}

void KisTiledDataManagerTest::testPageStoreExtentStorage()
{
    QFETCH(int, bpp); QFETCH(int, sign); QFETCH(int, operation);
    using Phase = KisPageStoreDiagnosticPhase;
    const QByteArray blank(bpp, char(0x13)), paint(bpp, char(0x71));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    KisDataManagerSP source = new KisDataManager(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    const int col = sign * 700, row = -sign * 900;
    const QRect area(col * 64 + 2, row * 64 + 3, 3, 2);
    if (operation == 3 || operation == 4)
        source->clear(area.x(), area.y(), area.width(), area.height(), reinterpret_cast<const quint8 *>(paint.constData()));
    auto *backend = dm.m_pageStoreBackend;
    auto old = backend->captureReadView(); QVERIFY(old.isValid());
    auto owner = dm.getMemento();
    if (operation == 5) QVERIFY(dm.beginStrokeMutation(owner));
    int observed = 0; bool ready = true, extentUnchanged = true;
    const auto verifyPrepared = [&] {
        ++observed;
        ready &= dm.m_extentManager.m_colsData.covers(col, col) && dm.m_extentManager.m_rowsData.covers(row, row);
        extentUnchanged &= dm.extent().isEmpty();
    };
    KisPageStoreDiagnosticRecorder recorder(true, backend->store());
    recorder.setPhaseObserver([&](Phase phase) {
        if (phase == Phase::PixelOperationBody || phase == Phase::CopySourcePage ||
            (operation == 2 && phase == Phase::MutationPresencePrepared)) verifyPrepared();
    });
    QString error;
    {
        KisStrokeJobFailureContext failure(true);
        if (operation == 0 || operation == 5) {
            QCOMPARE(dm.writePageStoreOperation({area, area}, [&](KisPixelWriteCursor *cursor) {
                verifyPrepared();
                for (int y = area.top(); y <= area.bottom(); ++y) for (int x = area.left(); x <= area.right(); ++x) {
                    cursor->moveTo(x, y); if (!cursor->rawData()) return false;
                    std::memset(cursor->rawData(), 0x71, size_t(bpp));
                }
                return true;
            }, &error, operation == 5 ? owner : KisMementoSP{}), KisPageStoreWriteOperationResult::Succeeded);
        } else if (operation == 1) {
            QByteArray packed(area.width() * area.height() * bpp, char(0x71));
            dm.writeBytes(reinterpret_cast<const quint8 *>(packed.constData()), area.x(), area.y(), area.width(), area.height());
        } else if (operation == 2) dm.clear(area.x(), area.y(), area.width(), area.height(), reinterpret_cast<const quint8 *>(paint.constData()));
        else if (operation == 3) dm.bitBlt(source, area);
        else dm.bitBltRough(source, area);
        QVERIFY2(!failure.failed(), qPrintable(failure.error()));
    }
    recorder.setPhaseObserver({});
    QVERIFY(observed > 0); QVERIFY(ready); QVERIFY(extentUnchanged);
    const QRect tile(col * 64, row * 64, 64, 64);
    QCOMPARE(dm.extent(), tile); QVERIFY(dm.tryCommit());
    auto current = backend->captureReadView(); QVERIFY(current.isValid());
    KisPageStoreReadPage actual(backend->store(), current, {backend->surface(), {col, row}});
    KisPageStoreReadPage before(backend->store(), old, {backend->surface(), {col, row}});
    QVERIFY(actual.data()); QVERIFY(before.data());
    QByteArray expected(64 * 64 * bpp, char(0x13));
    for (int y = 3; y < 5; ++y) for (int x = 2; x < 5; ++x)
        std::memset(expected.data() + (y * 64 + x) * bpp, 0x71, size_t(bpp));
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(actual.data()), expected.size()), expected);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(before.data()), expected.size()), QByteArray(expected.size(), char(0x13)));
    dm.rollback(owner); QCOMPARE(dm.extent(), QRect());
    dm.rollforward(owner); QCOMPARE(dm.extent(), tile);
    dm.clear(tile.x(), tile.y(), tile.width(), tile.height(), reinterpret_cast<const quint8 *>(blank.constData()));
    QCOMPARE(dm.extent(), QRect()); dm.purgeHistory(owner);
}

void KisTiledDataManagerTest::testPageStorePresenceStorage_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("pages"); QTest::addColumn<int>("operation");
    for (int bpp : {1, 4, 8, 16}) for (int pages : {1, 64, 65, 257}) for (int operation = 0; operation < 6; ++operation)
        QTest::newRow(qPrintable(QStringLiteral("B%1-K%2-op%3").arg(bpp).arg(pages).arg(operation)))
            << bpp << pages << operation;
}

void KisTiledDataManagerTest::testPageStorePresenceStorage()
{
    QFETCH(int, bpp); QFETCH(int, pages); QFETCH(int, operation);
    using Phase = KisPageStoreDiagnosticPhase;
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31)), painted(bpp, char(0x71));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    KisDataManagerSP source = new KisDataManager(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    const QRect area(-128, -64, 64 * pages, 64);
    const QRect copyRect(area.x() + 3, area.y() + 2, area.width() - 6, 3);
    if (operation >= 2 && operation <= 4) dm.clear(area.x(), area.y(), area.width(), area.height(), reinterpret_cast<const quint8 *>(initial.constData()));
    if (operation == 3 || operation == 4)
        for (int i = 0; i < pages; i += 2)
            source->clear(area.x() + 64 * i, area.y(), 64, 64, reinterpret_cast<const quint8 *>(painted.constData()));
    auto *backend = dm.m_pageStoreBackend; auto *store = backend->store();
    auto old = backend->captureReadView(); QVERIFY(old.isValid());
    auto owner = dm.getMemento();
    if (operation == 5) QVERIFY(dm.beginStrokeMutation(owner));
    QByteArray packed(area.width() * bpp, char(0x13));
    for (int i = 0; i < pages; i += 2) std::memset(packed.data() + 64 * i * bpp, 0x71, 64 * bpp);
    const bool needsPresence = operation >= 2 && operation <= 4;
    bool readyBeforePixels = !needsPresence, resolvedWithoutCapture = true;
    quint64 captures = 0, requests = 0;
    KisPageStoreDiagnosticRecorder recorder(true, store);
    recorder.setPhaseObserver([&](Phase phase) {
        if (phase == Phase::MutationPresencePrepared) readyBeforePixels = true;
        if (phase == Phase::MutationPresenceResolve) {
            captures = store->readScopeStatistics().capturedReadViewsCreated;
            requests = store->sessionStats().readRequestsCreated;
        } else if (phase == Phase::MutationPresenceResolved) {
            resolvedWithoutCapture &= captures == store->readScopeStatistics().capturedReadViewsCreated &&
                                      requests == store->sessionStats().readRequestsCreated;
        }
    });
    QString error;
    {
        KisStrokeJobFailureContext failure(true);
        if (operation == 0 || operation == 5) {
            QCOMPARE(dm.writePageStoreOperation({area, area}, [&](KisPixelWriteCursor *cursor) {
                if (!readyBeforePixels) return false;
                for (int i = 0; i < pages; i += 2) {
                    cursor->moveTo(area.x() + 64 * i + 3, area.y() + 2);
                    if (!cursor->rawData()) return false;
                    std::memset(cursor->rawData(), 0x71, size_t(bpp));
                }
                return true;
            }, &error, operation == 5 ? owner : KisMementoSP{}), KisPageStoreWriteOperationResult::Succeeded);
        } else if (operation == 1) dm.writeBytes(reinterpret_cast<const quint8 *>(packed.constData()), area.x(), area.y(), area.width(), 1);
        else if (operation == 2) dm.clear(area.x(), area.y(), area.width(), area.height(), reinterpret_cast<const quint8 *>(blank.constData()));
        else if (operation == 3) dm.bitBlt(source, copyRect);
        else dm.bitBltRough(source, copyRect);
        QVERIFY2(!failure.failed(), qPrintable(failure.error()));
    }
    recorder.setPhaseObserver({});
    const auto metrics = recorder.metrics();
    QCOMPARE(metrics[size_t(Phase::MutationPresencePrepare)].intervals, quint64(needsPresence ? 1 : 0));
    QCOMPARE(metrics[size_t(Phase::MutationPresencePrepare)].workItems, quint64(needsPresence ? pages : 0));
    QCOMPARE(metrics[size_t(Phase::MutationPresenceResolve)].intervals, quint64(needsPresence ? 1 : 0));
    QVERIFY(resolvedWithoutCapture);
    QVERIFY(dm.tryCommit());
    auto current = backend->captureReadView(); QVERIFY(current.isValid());
    const auto expectedPage = [&](int i) {
        QByteArray result(64 * 64 * bpp, char(operation >= 2 && operation <= 4 ? 0x31 : 0x13));
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
            int value = -1;
            if ((operation == 0 || operation == 5) && i % 2 == 0 && x == 3 && y == 2) value = 0x71;
            if (operation == 1 && y == 0 && i % 2 == 0) value = 0x71;
            if (operation == 2) value = 0x13;
            if (operation == 4 || (operation == 3 && copyRect.contains(area.x() + 64 * i + x, area.y() + y)))
                value = i % 2 == 0 ? 0x71 : 0x13;
            if (value >= 0) std::memset(result.data() + (y * 64 + x) * bpp, value, size_t(bpp));
        }
        return result;
    };
    for (int i = 0; i < pages; ++i) {
        const KisLogicalPageId page{i - 2, -1};
        const QByteArray expected = expectedPage(i);
        KisPageStoreReadPage read(store, current, {backend->surface(), page}); QVERIFY(read.data());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(read.data()), expected.size()), expected);
        KisPageStoreReadPage before(store, old, {backend->surface(), page}); QVERIFY(before.data());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(before.data()), expected.size()),
                 QByteArray(expected.size(), char(operation >= 2 && operation <= 4 ? 0x31 : 0x13)));
        const bool present = expected != QByteArray(expected.size(), char(0x13));
        QCOMPARE(bool(dm.m_hashTable->getExistingTile(page.column, page.row)), present);
    }
    const QRect expectedExtent = operation == 2 ? QRect{} :
        operation == 3 ? area : QRect(area.x(), area.y(), 64 * (pages % 2 ? pages : pages - 1), 64);
    QCOMPARE(dm.extent(), expectedExtent);
    current = {}; old = {};
    dm.rollback(owner);
    QCOMPARE(dm.extent(), operation >= 2 && operation <= 4 ? area : QRect{});
    dm.rollforward(owner); QCOMPARE(dm.extent(), expectedExtent);
    dm.purgeHistory(owner);
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
    QCOMPARE(after.maximumPinnedPagesPerExecution, quint64(failure == 3 ? 0 : 1));
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

void KisTiledDataManagerTest::testPageStorePixelOperationPreflightBudgetPressure()
{
    DefaultBudgetScope budget;
    quint8 blank = 0x13;
    KisDataManager dm(1, &blank);
    auto *backend = dm.m_pageStoreBackend;
    auto *store = backend->store();
    const auto owner = dm.getMemento();
    QVERIFY(owner);
    QVERIFY(dm.beginStrokeMutation(owner));
    using Result = KisPageStoreWriteOperationResult;

    QCOMPARE(backend->writeOperation({{0, 0}}, false, [](KisPixelWriteCursor *cursor) {
        cursor->moveTo(0, 0);
        if (!cursor->rawData()) return false;
        *cursor->rawData() = 0x31;
        return true;
    }, nullptr, nullptr, owner), Result::Succeeded);

    budget.saturate(1, &blank);
    bool called = false;
    QString error;
    QCOMPARE(backend->writeOperation({{0, 0}, {1, 0}}, false,
        [&](KisPixelWriteCursor *cursor) {
            called = true;
            cursor->moveTo(0, 0);
            if (!cursor->rawData()) return false;
            *cursor->rawData() = 0x51;
            cursor->moveTo(64, 0);
            if (!cursor->rawData()) return false;
            *cursor->rawData() = 0x52;
            return true;
        }, nullptr, &error, owner), Result::Failed);
    QVERIFY(!called);
    QVERIFY(!error.isEmpty());

    budget.release();
    QCOMPARE(backend->writeOperation({{0, 0}, {1, 0}}, false,
        [&](KisPixelWriteCursor *cursor) {
            called = true;
            cursor->moveTo(0, 0);
            if (!cursor->rawData()) return false;
            *cursor->rawData() = 0x51;
            cursor->moveTo(64, 0);
            if (!cursor->rawData()) return false;
            *cursor->rawData() = 0x52;
            return true;
        }, nullptr, &error, owner), Result::Succeeded);
    QVERIFY(called);
    QVERIFY(dm.tryCommit());

    auto view = backend->captureReadView();
    QVERIFY(view.isValid());
    KisPageStoreReadPage first(store, view, {backend->surface(), {0, 0}});
    KisPageStoreReadPage second(store, view, {backend->surface(), {1, 0}});
    QVERIFY(first.data());
    QVERIFY(second.data());
    QCOMPARE(static_cast<const quint8 *>(first.data())[0], quint8(0x51));
    QCOMPARE(static_cast<const quint8 *>(second.data())[0], quint8(0x52));
    first = {};
    second = {};
    view = {};
    dm.purgeHistory(owner);
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(0));
}

void KisTiledDataManagerTest::testPageStoreChangedStorage_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("pages");
    QTest::addColumn<bool>("persistent"); QTest::addColumn<int>("outcome");
    for (int bpp : {1, 4, 8, 16}) for (int pages : {1, 9, 65, 257})
        for (bool persistent : {false, true}) for (int outcome = 0; outcome < 4; ++outcome)
            QTest::newRow(qPrintable(QStringLiteral("B%1-K%2-owner%3-outcome%4")
                .arg(bpp).arg(pages).arg(persistent).arg(outcome))) << bpp << pages << persistent << outcome;
}

void KisTiledDataManagerTest::testPageStoreChangedStorage()
{
    QFETCH(int, bpp); QFETCH(int, pages); QFETCH(bool, persistent); QFETCH(int, outcome);
    const QByteArray blank(bpp, char(0x13));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    auto *backend = dm.m_pageStoreBackend; auto *store = backend->store();
    auto old = backend->captureReadView(); QVERIFY(old.isValid());
    auto owner = persistent ? dm.getMemento() : KisMementoSP{};
    if (persistent) QVERIFY(dm.beginStrokeMutation(owner));
    const auto mutationBefore = store->mutationStatistics();
    QVector<KisLogicalPageId> targets;
    QSet<KisLogicalPageId> expectedChanged;
    for (int i = 0; i < pages; ++i) {
        const KisLogicalPageId page{2 * i - pages, i % 3 - 1};
        targets.append(page); targets.append(page); // declaration duplicates do not duplicate output
        if (outcome != 2 && (outcome != 1 || i % 3 == 0)) expectedChanged.insert(page);
    }
    // A shared previous output must neither detach during delivery nor change its peer.
    QVector<KisLogicalPageId> changed{{-999, -999}};
    const auto peer = changed;
    const size_t metadata = size_t(KisBackingBudgetClass::MetadataArena);
    quint64 beforeScratch = 0, preparedBytes = 0;
    int prepares = 0, ranges = 0, exports = 0, deliveries = 0;
    KisPageStoreDiagnosticRecorder recorder(true, store);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase == KisPageStoreDiagnosticPhase::PixelOperationStoragePrepare) {
            ++prepares; beforeScratch = store->backingUsage().buckets[metadata].live.cpuRam;
        } else if (phase == KisPageStoreDiagnosticPhase::PixelOperationRangePrepare) {
            ++ranges; preparedBytes = store->backingUsage().buckets[metadata].live.cpuRam - beforeScratch;
        } else if (phase == KisPageStoreDiagnosticPhase::PixelOperationChangedExport) ++exports;
        else if (phase == KisPageStoreDiagnosticPhase::PixelOperationChangedDeliver) ++deliveries;
    });
    QString error; int bodies = 0;
    const auto result = backend->writeOperation(targets, false, [&](KisPixelWriteCursor *cursor) {
        ++bodies;
        for (int pass = 0; pass < 3; ++pass) for (int i = pages - 1; i >= 0; --i) {
            const auto &page = targets[2 * i];
            if (!expectedChanged.contains(page)) continue;
            cursor->moveTo(page.column * 64 + 3, page.row * 64 + 2 + pass);
            if (!cursor->rawData()) return false;
            std::memset(cursor->rawData(), 0x61 + pass, size_t(bpp));
        }
        return outcome != 3;
    }, &changed, &error, owner);
    QCOMPARE(bodies, 1); QCOMPARE(prepares, 1); QCOMPARE(ranges, 1);
    QCOMPARE(preparedBytes, quint64(0)); // output precedes the original execution/range storage
    QCOMPARE(result, outcome == 3 ? KisPageStoreWriteOperationResult::Failed : KisPageStoreWriteOperationResult::Succeeded);
    QCOMPARE(exports, outcome == 3 ? 0 : 1); QCOMPARE(deliveries, exports);
    QCOMPARE(peer, QVector<KisLogicalPageId>({{-999, -999}}));
    if (outcome == 3) expectedChanged.clear();
    QCOMPARE(changed.size(), expectedChanged.size());
    QVERIFY(QSet<KisLogicalPageId>(changed.cbegin(), changed.cend()) == expectedChanged);
    if (persistent) {
        auto hidden = backend->captureReadView(); QVERIFY(hidden.isValid());
        KisPageStoreReadPage read(store, hidden, {backend->surface(), targets.front()});
        QVERIFY(read.data()); QCOMPARE(quint8(read.data()[2 * read.rowStride() + 3 * bpp]), quint8(0x13));
        if (outcome == 3) QVERIFY(dm.tryAbort(owner));
        else QVERIFY(dm.tryCommit());
    }
    QCOMPARE(store->mutationStatistics().pagesSealed - mutationBefore.pagesSealed,
             quint64(expectedChanged.size()));
    auto current = backend->captureReadView(); QVERIFY(current.isValid());
    for (int i = 0; i < pages; ++i) {
        const auto &page = targets[2 * i];
        QByteArray expected(64 * 64 * bpp, char(0x13));
        if (expectedChanged.contains(page)) for (int pass = 0; pass < 3; ++pass)
            std::memset(expected.data() + ((2 + pass) * 64 + 3) * bpp, 0x61 + pass, size_t(bpp));
        KisPageStoreReadPage read(store, current, {backend->surface(), page}); QVERIFY(read.data());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(read.data()), expected.size()), expected);
        KisPageStoreReadPage previous(store, old, {backend->surface(), page}); QVERIFY(previous.data());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(previous.data()), expected.size()), QByteArray(expected.size(), char(0x13)));
    }
    current = {}; old = {};
    if (persistent && outcome != 3) dm.purgeHistory(owner);
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(0));
    QCOMPARE(store->backingUsage().buckets[metadata].reserved.cpuRam, quint64(0));
    // No touched bit/failed claim poisons the next standalone operation.
    QCOMPARE(backend->writeOperation({targets.front()}, false, [](KisPixelWriteCursor *) { return true; }, nullptr),
             KisPageStoreWriteOperationResult::Succeeded);
}

void KisTiledDataManagerTest::testPageStoreChangedStorageBudget()
{
    // The complete declared range exceeds the real product-derived metadata
    // ceiling. No allocator interposition or test-only budget setter.
    DefaultBudgetScope limits;
    for (bool persistent : {false, true}) {
        quint8 blank = 0x13; KisDataManager dm(1, &blank);
        auto *backend = dm.m_pageStoreBackend; auto *store = backend->store();
        auto owner = persistent ? dm.getMemento() : KisMementoSP{};
        if (persistent) QVERIFY(dm.beginStrokeMutation(owner));
        using Result = KisPageStoreWriteOperationResult;
        QCOMPARE(backend->writeOperation({{0, 0}}, false, [](KisPixelWriteCursor *cursor) {
            cursor->moveTo(0, 0); if (!cursor->rawData()) return false;
            *cursor->rawData() = 0x47; return true;
        }, nullptr, nullptr, owner), Result::Succeeded);
        QVector<KisLogicalPageId> pages; pages.reserve(262145);
        for (int i = 0; i < 262145; ++i) pages.append({i, 0});
        const auto stats = store->mutationStatistics();
        const auto sessions = store->sessionStats();
        const size_t metadata = size_t(KisBackingBudgetClass::MetadataArena);
        const auto usage = store->backingUsage().buckets[metadata];
        bool called = false; QVector<KisLogicalPageId> changed{{-1, -1}}; QString error;
        QCOMPARE(backend->writeOperation(pages, false, [&](KisPixelWriteCursor *) {
            called = true; return true;
        }, &changed, &error, owner), Result::Failed);
        QVERIFY(!called); QVERIFY(!error.isEmpty()); QVERIFY(changed.isEmpty());
        QCOMPARE(store->mutationStatistics().generationsReserved, stats.generationsReserved);
        QCOMPARE(store->sessionStats().activeTransactions, sessions.activeTransactions);
        QVERIFY(store->backingUsage().buckets[metadata].live.cpuRam <= quint64(8 * 1024 * 1024));
        QCOMPARE(store->backingUsage().buckets[metadata].reserved.cpuRam, usage.reserved.cpuRam);
        // Prior pending bytes survive a rejected preparation, and the original
        // owner remains usable. A small retry consumes the inline bitmap.
        QCOMPARE(backend->writeOperation({{0, 0}}, false, [](KisPixelWriteCursor *cursor) {
            cursor->moveTo(0, 1); if (!cursor->rawData()) return false;
            *cursor->rawData() = 0x61; return true;
        }, &changed, &error, owner), Result::Succeeded);
        if (persistent) QVERIFY(dm.tryCommit());
        auto view = backend->captureReadView(); auto read = view.readResidentPage({backend->surface(), {0, 0}});
        QVERIFY(read.isValid());
        QCOMPARE(static_cast<const quint8 *>(read.data())[0], quint8(0x47));
        QCOMPARE(static_cast<const quint8 *>(read.data())[read.rowStride()], quint8(0x61));
        read = {}; view = {}; if (persistent) dm.purgeHistory(owner);
    }
}

void KisTiledDataManagerTest::testPageStorePackedChangedStorage_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("pages"); QTest::addColumn<bool>("partial");
    for (int bpp : {1, 4, 8, 16}) for (int pages : {1, 9, 65, 257}) for (bool partial : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("B%1-K%2-partial%3").arg(bpp).arg(pages).arg(partial))) << bpp << pages << partial;
}

void KisTiledDataManagerTest::testPageStorePackedChangedStorage()
{
    QFETCH(int, bpp); QFETCH(int, pages); QFETCH(bool, partial);
    const QByteArray blank(bpp, char(0x13));
    KisTiledDataManagerPageStoreBackend backend; QVERIFY(backend.configure(bpp, reinterpret_cast<const quint8 *>(blank.constData())));
    const int width = pages * 64, height = partial ? 1 : 64, stride = width * bpp + 17;
    QByteArray input(stride * height, char(0x13));
    QSet<KisLogicalPageId> expectedChanged;
    for (int page = 0; page < pages; page += 3) {
        expectedChanged.insert({page - 1, -1});
        for (int row = 0; row < height; ++row) std::memset(input.data() + row * stride + page * 64 * bpp, 0x71, size_t(64 * bpp));
    }
    QVector<KisLogicalPageId> changed{{999, 999}}; QString error;
    using Result = KisPageStoreWriteOperationResult;
    QCOMPARE(backend.writeBytes(reinterpret_cast<const quint8 *>(input.constData()), -64, -64, width, height, stride, false, &changed, &error), Result::Succeeded);
    QCOMPARE(changed.size(), expectedChanged.size());
    QVERIFY(QSet<KisLogicalPageId>(changed.cbegin(), changed.cend()) == expectedChanged);
    auto view = backend.captureReadView(); QVERIFY(view.isValid());
    for (int page = 0; page < pages; ++page) {
        QByteArray expected(64 * 64 * bpp, char(0x13));
        if (page % 3 == 0) std::memset(expected.data(), 0x71, size_t(height * 64 * bpp));
        KisPageStoreReadPage read(backend.store(), view, {backend.surface(), {page - 1, -1}}); QVERIFY(read.data());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(read.data()), expected.size()), expected);
    }
    const auto counts = backend.store()->mutationStatistics();
    QCOMPARE(backend.writeBytes(reinterpret_cast<const quint8 *>(input.constData()), -64, -64, width, height, stride, false, &changed, &error), Result::Succeeded);
    QVERIFY(changed.isEmpty());
    QCOMPARE(backend.store()->mutationStatistics().generationsReserved, counts.generationsReserved);
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
    {
        KisRandomAccessor2 iterator(&dm, 0, 0, true, nullptr);
        iterator.moveTo(0, 0);
        auto *iteratorPointer = iterator.rawData();
        QVERIFY(iteratorPointer);
        *iteratorPointer = 0x21;
        bool iteratorCalled = false;
        QCOMPARE(dm.writePageStoreOperation(
                     {QRect(0,0,128,2)},
                     [&](KisPixelWriteCursor *) {
                         iteratorCalled = true;
                         return true;
                     }),
                 KisPageStoreWriteOperationResult::Borrowed);
        QVERIFY(!iteratorCalled);
        QCOMPARE(iterator.rawData(), iteratorPointer);
    }
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

void KisTiledDataManagerTest::testPageStoreAdapterPreparationRefusal_data()
{
    QTest::addColumn<bool>("persistent"); QTest::addColumn<bool>("throws");
    for (bool persistent : {false, true}) for (bool throws : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("persistent%1-throws%2").arg(persistent).arg(throws)))
            << persistent << throws;
}

void KisTiledDataManagerTest::testPageStoreAdapterPreparationRefusal()
{
    QFETCH(bool, persistent); QFETCH(bool, throws);
    const quint8 blank = 0; KisDataManager dm(1, &blank);
    auto *backend = dm.m_pageStoreBackend; auto *store = backend->store();
    auto history = persistent ? dm.getMemento() : KisMementoSP{};
    if (history) QVERIFY(dm.beginStrokeMutation(history));
    QString error;
    auto paint = [](KisPixelWriteCursor *cursor) {
        cursor->moveTo(0, 0); if (!cursor->rawData()) return false;
        *cursor->rawData() = 0x41; return true;
    };
    QCOMPARE(dm.writePageStoreOperation({QRect(0, 0, 1, 1)}, paint, &error, history),
             KisPageStoreWriteOperationResult::Succeeded);
    bool prepared = false, pixels = false;
    QVector<KisLogicalPageId> changed;
    QCOMPARE(backend->writeOperation({{1, 0}}, false,
        [&](KisPixelWriteCursor *) { pixels = true; return true; }, &changed, &error, history,
        nullptr, [&](QString *failure) {
            prepared = store->sessionStats().activeCpuWritePages == (persistent ? 2 : 1);
            if (throws) throw std::bad_alloc();
            *failure = QStringLiteral("test preparation refusal"); return false;
        }), KisPageStoreWriteOperationResult::Failed);
    QVERIFY(prepared); QVERIFY(!pixels); QVERIFY(changed.isEmpty()); QVERIFY(!error.isEmpty());
    // A refused cold preparation must leave the already-painted segment usable.
    QCOMPARE(dm.writePageStoreOperation({QRect(0, 0, 1, 1)}, paint, &error, history),
             KisPageStoreWriteOperationResult::Succeeded);
    if (history) QVERIFY2(dm.tryCommit(&error), qPrintable(error));
    quint8 actual = 0; dm.readBytes(&actual, 0, 0, 1, 1); QCOMPARE(actual, quint8(0x41));
    QCOMPARE(dm.m_hashTable->numTiles(), 1);
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(0));
}

void KisTiledDataManagerTest::testPageStoreAdapterAfterAbort_data()
{
    QTest::addColumn<bool>("packed");
    QTest::newRow("cursor") << false;
    QTest::newRow("packed") << true;
}

void KisTiledDataManagerTest::testPageStoreAdapterAfterAbort()
{
    QFETCH(bool, packed);
    const quint8 blank = 0; KisDataManager dm(1, &blank);
    const auto history = dm.getMemento(); QVERIFY(history);
    auto *backend = dm.m_pageStoreBackend; auto *store = backend->store();
    bool aborted = false, prepared = false; QString error;
    KisPageStoreDiagnosticRecorder recorder(true, store);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase == KisPageStoreDiagnosticPhase::PixelOperationRangePrepare && !aborted)
            aborted = dm.tryAbort(history, &error);
        if (phase == KisPageStoreDiagnosticPhase::MutationAdapterPrepared) {
            // Abort replaced both original adapter structures before admission.
            // The new extent capacity must now be ready while counts stay empty.
            prepared = aborted && store->sessionStats().activeCpuWritePages == 1 &&
                dm.m_extentManager.m_colsData.m_capacity > 256 && dm.extent().isEmpty();
        }
    });
    const quint8 value = 0x59;
    if (packed) {
        KisStrokeJobFailureContext failure(true);
        dm.writeBytes(&value, 300 * 64, 0, 1, 1); QVERIFY(!failure.failed());
    } else {
        QCOMPARE(dm.writePageStoreOperation({QRect(300 * 64, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(300 * 64, 0); if (!cursor->rawData()) return false;
            *cursor->rawData() = value; return true;
        }, &error), KisPageStoreWriteOperationResult::Succeeded);
    }
    recorder.setPhaseObserver({});
    QVERIFY(aborted); QVERIFY(prepared); QCOMPARE(dm.m_hashTable->numTiles(), 1);
    QCOMPARE(dm.extent(), QRect(300 * 64, 0, 64, 64));
    quint8 actual = 0; dm.readBytes(&actual, 300 * 64, 0, 1, 1); QCOMPARE(actual, value);
}

void KisTiledDataManagerTest::testPageStoreRestoreLateAdmission()
{
    const quint8 blank = 0; KisDataManager dm(1, &blank);
    auto *backend = dm.m_pageStoreBackend; auto *store = backend->store();
    const auto retained = store->captureRetainedEpoch(); QVERIFY(retained.isValid());
    QSemaphore claimed, proceed;
    std::thread worker;
    bool observed = false, admittedBeforeTransaction = false, finished = false;
    KisPageStoreDiagnosticRecorder recorder(true, store);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::RestorePublishOwnerWait || observed) return;
        observed = true;
        worker = std::thread([&] {
            bool paused = false;
            KisPageStoreDiagnosticRecorder other(true, store);
            other.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
                if (phase == KisPageStoreDiagnosticPhase::PixelOperationRangeReserved && !paused) {
                    paused = true; claimed.release(); proceed.acquire();
                }
            });
            QVector<KisLogicalPageId> changed; QString error;
            finished = backend->writeOperation({{0, 0}}, false, [](KisPixelWriteCursor *) { return true; },
                &changed, &error) == KisPageStoreWriteOperationResult::Succeeded;
        });
        if (claimed.tryAcquire(1, 5000)) {
            const auto stats = store->sessionStats();
            admittedBeforeTransaction = stats.activeCpuWritePages == 1 && stats.activeTransactions == 0;
        }
    });
    const auto restored = store->restoreRetainedEpoch(retained);
    recorder.setPhaseObserver({});
    proceed.release();
    if (worker.joinable()) worker.join();
    QVERIFY(observed); QVERIFY(admittedBeforeTransaction); QVERIFY(!restored.isValid()); QVERIFY(finished);
    QVERIFY(store->restoreRetainedEpoch(retained).isValid());
}

void KisTiledDataManagerTest::testPageStoreAdapterBarrier_data()
{
    QTest::addColumn<int>("barrier");
    for (int barrier : {0, 1, 2, 3, 4})
        QTest::newRow(qPrintable(QStringLiteral("barrier%1").arg(barrier))) << barrier;
}

void KisTiledDataManagerTest::testPageStoreAdapterBarrier()
{
    QFETCH(int, barrier);
    const quint8 blank = 0; KisDataManager dm(1, &blank);
    auto *backend = dm.m_pageStoreBackend; auto *store = backend->store();
    const auto retained = store->captureRetainedEpoch(); QVERIFY(retained.isValid());
    const auto history = barrier == 2 || barrier == 3 ? dm.getMemento() : KisMementoSP{};
    bool observed = false, rejected = false; QString error;
    KisPageStoreDiagnosticRecorder recorder(true, store);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        const auto deliveryPhase = KisPageStoreDiagnosticPhase::MutationAdapterInstalled;
        if (phase != deliveryPhase || observed) return;
        observed = true;
        if (barrier == 0) {
            KisStrokeJobFailureContext failure(true); dm.clear(); rejected = failure.failed();
        } else if (barrier == 1) rejected = !store->restoreRetainedEpoch(retained).isValid();
        else if (barrier == 2) rejected = !dm.tryCommit(&error);
        else if (barrier == 3) rejected = !dm.tryAbort(history, &error);
        else { KisStrokeJobFailureContext failure(true); dm.setExtent(QRect(0, 0, 0, 0)); rejected = failure.failed(); }
    });
    QCOMPARE(dm.writePageStoreOperation({QRect(0, 0, 1, 1)}, [](KisPixelWriteCursor *cursor) {
        cursor->moveTo(0, 0); if (!cursor->rawData()) return false;
        *cursor->rawData() = 0x59; return true;
    }, &error), KisPageStoreWriteOperationResult::Succeeded);
    recorder.setPhaseObserver({});
    QVERIFY(observed); QVERIFY(rejected);
    QCOMPARE(dm.m_hashTable->numTiles(), 1);
    quint8 actual = 0; dm.readBytes(&actual, 0, 0, 1, 1); QCOMPARE(actual, quint8(0x59));
    if (barrier == 0) { dm.clear(); QCOMPARE(dm.m_hashTable->numTiles(), 0); }
    else if (barrier == 1) QVERIFY(store->restoreRetainedEpoch(retained).isValid());
    else if (barrier == 2) QVERIFY2(dm.tryCommit(&error), qPrintable(error));
    else if (barrier == 3) { QVERIFY2(dm.tryAbort(history, &error), qPrintable(error)); QCOMPARE(dm.m_hashTable->numTiles(), 0); }
    else { dm.setExtent(QRect(0, 0, 0, 0)); QCOMPARE(dm.m_hashTable->numTiles(), 0); }
}

void KisTiledDataManagerTest::testPageStoreAdapterDelivery_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("mode"); QTest::addColumn<int>("outcome");
    // Cursor, packed, persistent history; real write, no-op, rejected work.
    for (int bpp : {1, 4, 8, 16}) for (int mode : {0, 1, 2}) for (int outcome : {0, 1, 2})
        QTest::newRow(qPrintable(QStringLiteral("bpp%1-mode%2-outcome%3").arg(bpp).arg(mode).arg(outcome)))
            << bpp << mode << outcome;
}

void KisTiledDataManagerTest::testPageStoreAdapterDelivery()
{
    QFETCH(int, bpp); QFETCH(int, mode); QFETCH(int, outcome);
    const QByteArray blank(bpp, char(0x13));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    auto *backend = dm.m_pageStoreBackend; auto *store = backend->store();
    auto before = backend->captureReadView(); QVERIFY(before.isValid());
    auto history = mode == 2 ? dm.getMemento() : KisMementoSP{};
    if (history) QVERIFY(dm.beginStrokeMutation(history));
    bool observed = false, retained = false, independent = false, checkpointRejected = false;
    KisPageStoreDiagnosticRecorder recorder(true, store);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        const auto deliveryPhase = KisPageStoreDiagnosticPhase::MutationAdapterInstalled;
        if (phase != deliveryPhase || observed) return;
        observed = true;
        retained = store->sessionStats().activeCpuWritePages == 2;
        QString error; bool called = false; QVector<KisLogicalPageId> changed;
        const auto blocked = backend->writeOperation({{0, 0}}, false,
            [&](KisPixelWriteCursor *) { called = true; return true; }, &changed, &error, history);
        retained &= blocked == KisPageStoreWriteOperationResult::Failed && !called;
        const auto unrelated = backend->writeOperation({{10, 0}}, false,
            [&](KisPixelWriteCursor *) { called = true; return true; }, &changed, &error, history);
        independent = unrelated == KisPageStoreWriteOperationResult::Succeeded && called;
        if (history) checkpointRejected = !backend->checkpointHistoryMutation(history, nullptr, &error).isValid();
    });
    QString error; bool success;
    if (mode == 1) {
        const QByteArray input(65 * bpp, char(outcome == 1 ? 0x13 : 0x71));
        KisStrokeJobFailureContext failures(true);
        dm.writeBytes(reinterpret_cast<const quint8 *>(input.constData()), 0, 0, 65, 1, outcome == 2 ? 1 : 0);
        success = !failures.failed();
    } else {
        success = dm.writePageStoreOperation({QRect(0, 0, 65, 1)}, [&](KisPixelWriteCursor *cursor) {
            if (outcome == 1) return true;
            for (int x : {0, 64}) {
                cursor->moveTo(x, 0); if (!cursor->rawData()) return false;
                std::memset(cursor->rawData(), 0x71, size_t(bpp));
            }
            return outcome != 2;
        }, &error, history) == KisPageStoreWriteOperationResult::Succeeded;
    }
    recorder.setPhaseObserver({});
    QCOMPARE(success, outcome != 2);
    QCOMPARE(observed, outcome != 2);
    if (observed) { QVERIFY(retained); QVERIFY(independent); if (history) QVERIFY(checkpointRejected); }
    if (history) {
        if (outcome == 2) QVERIFY(dm.tryAbort(history, &error));
        else { QVERIFY(dm.checkpointStrokeMutation(history, &error)); QVERIFY(dm.tryCommit(&error)); }
    }
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(0));
    QCOMPARE(dm.m_hashTable->numTiles(), outcome == 0 ? 2 : 0);
    QCOMPARE(dm.extent(), outcome == 0 ? QRect(0, 0, 128, 64) : QRect());
    QByteArray pixel(bpp, Qt::Uninitialized);
    for (int x : {0, 64}) {
        dm.readBytes(reinterpret_cast<quint8 *>(pixel.data()), x, 0, 1, 1);
        QCOMPARE(pixel, QByteArray(bpp, char(outcome == 0 ? 0x71 : 0x13)));
        KisPageStoreReadPage old(store, before, {backend->surface(), {x / 64, 0}});
        QVERIFY(old.data()); QCOMPARE(QByteArray(reinterpret_cast<const char *>(old.data()), bpp), blank);
    }
    if (history && outcome != 2) {
        dm.rollback(history); dm.readBytes(reinterpret_cast<quint8 *>(pixel.data()), 0, 0, 1, 1); QCOMPARE(pixel, blank);
        dm.rollforward(history); dm.readBytes(reinterpret_cast<quint8 *>(pixel.data()), 0, 0, 1, 1);
        QCOMPARE(pixel, QByteArray(bpp, char(outcome == 0 ? 0x71 : 0x13)));
    }
}

void KisTiledDataManagerTest::testPageStoreAdapterDeliveryLifetime_data()
{
    QTest::addColumn<bool>("packed");
    QTest::newRow("cursor") << false;
    QTest::newRow("packed") << true;
}

void KisTiledDataManagerTest::testPageStoreAdapterDeliveryLifetime()
{
    QFETCH(bool, packed);
    const quint8 blank = 0x13, value = 0x71;
    KisDataManager dm(1, &blank);
    auto *backend = dm.m_pageStoreBackend; auto *store = backend->store();
    KisTiledDataManagerPageStoreBackend::OperationDelivery delivery;
    QVector<KisLogicalPageId> changed;
    QString error;
    const auto write = [&](bool &called) {
        if (packed) return backend->writeBytes(&value, 0, 0, 1, 1, 0, false, &changed, &error, &delivery);
        return backend->writeOperation({{0, 0}}, false, [&](KisPixelWriteCursor *cursor) {
            called = true; cursor->moveTo(0, 0); if (!cursor->rawData()) return false;
            *cursor->rawData() = value; return true;
        }, &changed, &error, {}, &delivery);
    };
    bool called = false;
    QCOMPARE(write(called), KisPageStoreWriteOperationResult::Succeeded);
    QVERIFY(!delivery.isEmpty());
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(1));
    QCOMPARE(store->sessionStats().committedTransactions, quint64(1));
    QVERIFY(!store->closeSession(&error)); // The original retained claim still gates close.
    called = false;
    QCOMPARE(write(called), KisPageStoreWriteOperationResult::Failed);
    QVERIFY(!called); QVERIFY(!delivery.isEmpty());
    if (packed) {
        QCOMPARE(backend->writeBytes(nullptr, 0, 0, 0, 0, 0, false, &changed, &error, &delivery),
                 KisPageStoreWriteOperationResult::Failed);
        QVERIFY(!delivery.isEmpty());
    }
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(1));
    // A different worker really waits until delivery drops the same original
    // admission; ending the transaction alone must not wake it through.
    QSemaphore waiting, done;
    bool admitted = false;
    std::thread waiter([&] {
        KisPageStoreDiagnosticRecorder recorder(true, store);
        recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
            if (phase == KisPageStoreDiagnosticPhase::PixelOperationRangeWait) waiting.release();
        });
        QString failure; QVector<KisLogicalPageId> changed;
        admitted = backend->writeOperation({{0, 0}}, false,
            [](KisPixelWriteCursor *) { return true; }, &changed, &failure)
            == KisPageStoreWriteOperationResult::Succeeded;
        done.release();
    });
    const bool blocked = waiting.tryAcquire(1, 5000);
    const bool premature = done.tryAcquire();
    delivery = {};
    waiter.join();
    QVERIFY(blocked); QVERIFY(!premature); QVERIFY(admitted);
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(0));
    QVERIFY(write(called) == KisPageStoreWriteOperationResult::Succeeded);
    std::thread cleanup([held = std::move(delivery)]() mutable { held = {}; });
    cleanup.join();
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(0));
    auto view = backend->captureReadView(); QVERIFY(view.isValid());
    KisPageStoreReadPage read(store, view, {backend->surface(), {0, 0}});
    QVERIFY(read.data()); QCOMPARE(read.data()[0], value);
    read.reset(); view = {};
    QVERIFY2(store->closeSession(&error), qPrintable(error));
}

void KisTiledDataManagerTest::testPageStoreAdapterDeliveryLegacyConflict_data()
{
    QTest::addColumn<int>("mode"); QTest::addColumn<int>("legacy");
    // Native cursor/packed/history delivery versus raw/planar/borrowed packed.
    for (int mode : {0, 1, 2}) for (int legacy : {0, 1, 2})
        QTest::newRow(qPrintable(QStringLiteral("mode%1-legacy%2").arg(mode).arg(legacy))) << mode << legacy;
}

void KisTiledDataManagerTest::testPageStoreClearRangeAdmission_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("partial");
    QTest::addColumn<bool>("remove"); QTest::addColumn<bool>("history");
    for (int bpp : {1, 4, 8, 16}) for (bool partial : {false, true})
        for (bool remove : {false, true}) for (bool history : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-partial%2-remove%3-history%4")
                .arg(bpp).arg(partial).arg(remove).arg(history))) << bpp << partial << remove << history;
}

void KisTiledDataManagerTest::testPageStoreClearRangeAdmission()
{
    QFETCH(int, bpp); QFETCH(bool, partial); QFETCH(bool, remove); QFETCH(bool, history);
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31)), next(bpp, char(0x71));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    dm.clear(0, 0, 192, 64, reinterpret_cast<const quint8 *>(initial.constData()));
    auto *backend = dm.m_pageStoreBackend; auto *store = backend->store();
    auto before = backend->captureReadView(); QVERIFY(before.isValid());
    auto memento = history ? dm.getMemento() : KisMementoSP{};
    KisTiledDataManagerPageStoreBackend::OperationDelivery delivery;
    QVector<KisLogicalPageId> changed; QString error; int calls = 0;
    QCOMPARE(backend->writeOperation({{0, 0}}, false, [&](KisPixelWriteCursor *cursor) {
        ++calls; cursor->moveTo(0, 0); if (!cursor->rawData()) return false;
        std::memset(cursor->rawData(), 0x61, size_t(bpp)); return true;
    }, &changed, &error, {}, &delivery), KisPageStoreWriteOperationResult::Succeeded);
    QVERIFY(!delivery.isEmpty());
    QSemaphore waiting, done;
    bool succeeded = false, lateExcluded = false;
    const QRect area = partial ? QRect(1, 1, 126, 62) : QRect(0, 0, 128, 64);
    const QByteArray value = remove ? blank : next;
    std::thread clearer([&] {
        KisPageStoreDiagnosticRecorder recorder(true, store);
        recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
            if (phase == KisPageStoreDiagnosticPhase::PixelOperationRangeWait) waiting.release();
            if (phase == KisPageStoreDiagnosticPhase::MutationAdapterInstall && !lateExcluded) {
                const auto tx = store->beginCurrentTransaction();
                auto late = store->beginMutation(tx);
                const bool active = late.isActive();
                auto guard = late.beginWrite({backend->surface(), {1, 0}});
                lateExcluded = active && !guard.isValid();
                guard = {}; late.cancel(); store->abort(tx);
            }
        });
        KisStrokeJobFailureContext failure(true);
        dm.clear(area.x(), area.y(), area.width(), area.height(),
                 reinterpret_cast<const quint8 *>(value.constData()));
        succeeded = !failure.failed(); done.release();
    });
    const auto join = qScopeGuard([&] { delivery = {}; if (clearer.joinable()) clearer.join(); });
    const bool blocked = waiting.tryAcquire(1, 500);
    const bool premature = done.tryAcquire();
    // Admission protects only the overlapping pages. An unrelated operation
    // must finish while the original managed delivery remains held.
    int independentCalls = 0;
    const auto independent = dm.writePageStoreOperation({QRect(128, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
        ++independentCalls; cursor->moveTo(128, 0); if (!cursor->rawData()) return false;
        std::memset(cursor->rawData(), 0x51, size_t(bpp)); return true;
    }, &error);
    delivery = {}; clearer.join();
    QVERIFY(blocked); QVERIFY(!premature); QVERIFY(succeeded); QVERIFY(lateExcluded);
    QCOMPARE(independent, KisPageStoreWriteOperationResult::Succeeded);
    QCOMPARE(calls, 1); QCOMPARE(independentCalls, 1);
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(0));
    QByteArray expected(192 * 64 * bpp, char(0x31)), actual(expected.size(), Qt::Uninitialized);
    std::memset(expected.data(), partial ? 0x61 : value[0], size_t(bpp));
    std::memset(expected.data() + 128 * bpp, 0x51, size_t(bpp));
    for (int y = area.top(); y <= area.bottom(); ++y)
        std::memset(expected.data() + (y * 192 + area.x()) * bpp, value[0], size_t(area.width() * bpp));
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 192, 64);
    QCOMPARE(actual, expected);
    // Fixed before-images survive clear and the earlier accepted write.
    auto old = before.readResidentPage({backend->surface(), {0, 0}}); QVERIFY(old.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(old.data()), int(old.byteSize())), QByteArray(64 * 64 * bpp, char(0x31)));
    old = {}; before = {};
    if (history) {
        QVERIFY(dm.tryCommit(&error)); dm.rollback(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 192, 64);
        QCOMPARE(actual, QByteArray(actual.size(), char(0x31)));
        dm.rollforward(memento); dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 192, 64);
        QCOMPARE(actual, expected); dm.purgeHistory(memento);
    }
}

void KisTiledDataManagerTest::testPageStoreAdapterDeliveryLegacyConflict()
{
    QFETCH(int, mode); QFETCH(int, legacy);
    const quint8 blank = 0x13, initial = 0x31, value = 0x71;
    KisDataManager dm(1, &blank);
    dm.clear(0, 0, 128, 64, &initial);
    auto *backend = dm.m_pageStoreBackend; auto *store = backend->store();
    auto before = backend->captureReadView(); QVERIFY(before.isValid());
    auto history = mode == 2 ? dm.getMemento() : KisMementoSP{};
    if (history) QVERIFY(dm.beginStrokeMutation(history));
    KisTiledDataManagerPageStoreBackend::OperationDelivery delivery;
    QVector<KisLogicalPageId> changed;
    QString error;
    const auto result = mode == 1
        ? backend->writeBytes(&value, 64, 0, 1, 1, 0, false, &changed, &error, &delivery)
        : backend->writeOperation({{1, 0}}, false, [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(64, 0); if (!cursor->rawData()) return false;
            *cursor->rawData() = value; return true;
          }, &changed, &error, history, &delivery);
    QCOMPARE(result, KisPageStoreWriteOperationResult::Succeeded);
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(1));

    // Retain the real original capability at the manager-delivery boundary.
    // The legacy caller holds its real tile/manager gates. It must reject
    // before we release the claim, otherwise an actual manager refresh would
    // need the very gates that the waiting legacy caller is holding.
    QSemaphore done;
    bool rejected = false, intentLocked = true;
    std::thread writer([&] {
        KisStrokeJobFailureContext failures(true);
        QByteArray input(65, char(0x72));
        KisTileSP intent;
        if (legacy == 2) {
            intent = dm.getTile(1, 0, true);
            intentLocked = intent->lockForWrite();
        }
        if (legacy == 0) dm.setPixel(64, 0, reinterpret_cast<const quint8 *>(input.constData()));
        else if (legacy == 1) dm.writePlanarBytes({reinterpret_cast<quint8 *>(input.data())}, {1}, 0, 0, 65, 1);
        else if (intentLocked) dm.writeBytes(reinterpret_cast<const quint8 *>(input.constData()), 0, 0, 65, 1);
        if (intent && intentLocked) intent->unlockForWrite();
        rejected = failures.failed();
        done.release();
    });
    const bool returnedWhileClaimed = done.tryAcquire(1, 5000);
    // On regression, releasing the direct backend capability lets the test
    // clean up its worker and report failure instead of hanging the suite.
    if (!returnedWhileClaimed) {
        delivery = {};
    }
    writer.join();
    QVERIFY(returnedWhileClaimed); QVERIFY(intentLocked); QVERIFY(rejected);
    {
        QWriteLocker lock(&dm.m_lock);
        dm.refreshPageStorePage({1, 0}, true);
    }
    delivery = {};
    if (history) QVERIFY(dm.tryCommit(&error));
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(0));
    quint8 actual[65]; dm.readBytes(actual, 0, 0, 65, 1);
    for (int x = 0; x < 64; ++x) QCOMPARE(actual[x], initial);
    QCOMPARE(actual[64], value);
    KisPageStoreReadPage old(store, before, {backend->surface(), {1, 0}});
    QVERIFY(old.data()); QCOMPARE(old.data()[0], initial);
    old.reset(); before = {};
    // Rejection did not leave a failed job, claim or lock attached to retries.
    quint8 next = 0x73;
    { KisStrokeJobFailureContext failures(true); dm.setPixel(64, 0, &next); QVERIFY(!failures.failed()); }
    dm.readBytes(actual, 64, 0, 1, 1); QCOMPARE(actual[0], next);
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
    // Cursor preflight prepares both declared pages, then cancels the second
    // callback's untouched page. Packed input prepares only its actual write.
    const quint64 secondWrites = secondKind == 2 && cancelFirst ? 0 : 1;
    const auto after = store->mutationStatistics();
    QCOMPARE(after.generationsReserved - counts.generationsReserved,
             quint64(2) + (secondKind == 0 ? 2 : secondWrites));
    QCOMPARE(after.pagesSealed - counts.pagesSealed, (cancelFirst ? 0 : 2) + secondWrites);
    QCOMPARE(after.pagesCancelled - counts.pagesCancelled, quint64(cancelFirst ? 2 : 0) + (secondKind == 0 ? 1 : 0));
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(0));
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
    QCOMPARE(after.maximumPinnedPagesPerExecution, quint64(1));
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
        QCOMPARE(stats.generationsReserved, quint64(2)); QCOMPARE(stats.maximumPinnedPagesPerExecution, quint64(1));
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
    QCOMPARE(after.maximumPinnedPagesPerExecution, quint64(partialPages ? 1 : 0));
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

void KisTiledDataManagerTest::testVoidDrawingFailure_data()
{
    QTest::addColumn<int>("entry");
    QTest::addColumn<bool>("history");
    const QStringList names{"packed", "planar", "fill", "clear", "copy", "rough-copy", "trim", "pixel", "legacy-source-copy"};
    for (int entry = 0; entry < names.size(); ++entry)
        for (bool history : {false, true})
            QTest::newRow(qPrintable(names[entry] + QString("-history%1").arg(history))) << entry << history;
}

void KisTiledDataManagerTest::testVoidDrawingFailure()
{
    QFETCH(int, entry);
    QFETCH(bool, history);
    const quint8 blank = 0x13, initial = 0x31, staged = 0x41, replacement = 0x71;
    KisTiledDataManager dm(1, &blank), source(1, &blank);
    if (entry == 8) {
        source.m_mementoManager->setPageStoreBridge(nullptr);
        delete source.m_pageStoreBackend;
        source.m_pageStoreBackend = nullptr;
    }
    dm.clear(0, 0, 128, 64, &initial);
    source.clear(0, 0, 128, 64, &replacement);
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    if (history) dm.clear(0, 0, 128, 64, &staged);
    auto *store = dm.m_pageStoreBackend->store();
    const auto foreign = store->beginCurrentTransaction();
    auto mutation = store->beginMutation(foreign);
    auto held = mutation.beginWrite({{1}, {1, 0}});
    QVERIFY(held.isValid());
    const auto before = store->mutationStatistics();
    const auto commits = store->sessionStats().committedTransactions;
    {
        KisStrokeJobFailureContext failure(true);
        const QRect area(1, 0, 126, 64);
        QByteArray input(128 * 64, char(replacement));
        auto *bytes = reinterpret_cast<quint8 *>(input.data());
        switch (entry) {
        case 0: dm.writeBytes(bytes, 0, 0, 128, 64); break;
        case 1: dm.writePlanarBytes({bytes}, {1}, 0, 0, 128, 64); break;
        case 2: dm.clear(area.x(), area.y(), area.width(), area.height(), &replacement); break;
        case 3: dm.clear(); break;
        case 4: dm.bitBlt(&source, area); break;
        case 5: dm.bitBltRough(&source, area); break;
        case 6: dm.setExtent(area); break;
        case 7: dm.setPixel(65, 1, &replacement); break;
        case 8: dm.bitBlt(&source, area); break;
        }
        QVERIFY(failure.failed());
        // Later void calls in the same failed job cannot publish more pixels.
        dm.clear(0, 0, 64, 64, &replacement);
        dm.setDefaultPixel(&replacement);
        QCOMPARE(*dm.defaultPixel(), blank);
    }
    if (entry == 1 || entry == 4 || entry == 8)
        QCOMPARE(store->mutationStatistics().pagesCancelled - before.pagesCancelled, quint64(1));
    if (entry == 2 || entry == 3 || entry == 6) {
        // Clear's whole range and whole-index barriers reject a foreign claim
        // before any page work; planar/copy retain their partial rollback test.
        QCOMPARE(store->mutationStatistics().generationsReserved, before.generationsReserved);
        QCOMPARE(store->mutationStatistics().pagesCancelled, before.pagesCancelled);
        QCOMPARE(store->mutationStatistics().removalsCancelled, before.removalsCancelled);
    }
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(1));
    QCOMPARE(store->sessionStats().committedTransactions, commits);
    QByteArray actual(128 * 64, Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 128, 64);
    QCOMPARE(actual, QByteArray(actual.size(), char(history ? staged : initial)));
    held = {};
    QVERIFY(mutation.cancel());
    QVERIFY(store->abort(foreign));
    if (history) QVERIFY(dm.tryAbort(memento));
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 128, 64);
    QCOMPARE(actual, QByteArray(actual.size(), char(initial)));
    // A new job can write normally; the TLS bridge retains no failure state.
    KisStrokeJobFailureContext next(true);
    dm.clear(0, 0, 128, 64, &replacement);
    QVERIFY(!next.failed());
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 128, 64);
    QCOMPARE(actual, QByteArray(actual.size(), char(replacement)));
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
    // Planar still exercises partial-work rollback on page one. Trim rebuilds
    // the complete compatibility index and must reject the foreign claim
    // before touching any page, including a mixed removal/partial trim.
    const auto before = store->mutationStatistics();
    const auto commits = store->sessionStats().committedTransactions;
    if (planar) {
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("^PageStore CPU mutation acquisition failed:.*legacy mutation cannot reenter its page.*$"));
        QByteArray input(128 * 64, char(0x71));
        QTest::ignoreMessage(QtWarningMsg, "PageStore planar write failed");
        dm.writePlanarBytes({reinterpret_cast<quint8 *>(input.data())}, {1}, 0, 0, 128, 64);
    } else {
        QString error;
        QVERIFY(!backend->trimToRect(QRect(mixed ? 65 : 1, 1, 126, 62), &error)); QVERIFY(!error.isEmpty());
    }
    QCOMPARE(store->mutationStatistics().generationsReserved - before.generationsReserved, quint64(planar ? 1 : 0));
    QCOMPARE(store->mutationStatistics().pagesCancelled - before.pagesCancelled, quint64(planar ? 1 : 0));
    QCOMPARE(store->mutationStatistics().removalsCancelled, before.removalsCancelled);
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

void KisTiledDataManagerTest::testIteratorRejectedWriteAdmission_data()
{
    QTest::addColumn<int>("entry");
    QTest::addColumn<int>("bpp");
    for (int entry = 0; entry < 3; ++entry) for (int bpp : {1, 4, 8, 16})
        QTest::newRow(qPrintable(QString("entry%1-bpp%2").arg(entry).arg(bpp))) << entry << bpp;
}

void KisTiledDataManagerTest::testIteratorRejectedWriteAdmission()
{
    QFETCH(int, entry);
    QFETCH(int, bpp);
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31));
    auto bytes = [](const QByteArray &p) { return reinterpret_cast<const quint8 *>(p.constData()); };
    KisDataManager dm(bpp, bytes(blank));
    dm.clear(0, 0, 128, 64, bytes(initial));
    const auto memento = dm.getMemento();
    QVERIFY(memento);
    auto *store = dm.m_pageStoreBackend->store();
    auto before = dm.m_pageStoreBackend->captureReadView();
    QVERIFY(before.isValid());
    const auto foreign = store->beginCurrentTransaction();
    auto mutation = store->beginMutation(foreign);
    auto held = mutation.beginWrite({{1}, {1, 0}});
    QVERIFY(held.isValid());
    // A resident compatibility tile can grant the intent lock, but must never
    // return its before-image as writable bytes when admission is rejected.
    if (entry == 0) {
        KisHLineIterator2 writer(&dm, 65, 1, 1, 0, 0, true, nullptr);
        QVERIFY(!writer.rawData());
        QVERIFY(!writer.oldRawData());
    } else if (entry == 1) {
        KisVLineIterator2 writer(&dm, 65, 1, 1, 0, 0, true, nullptr);
        QVERIFY(!writer.rawData());
        QVERIFY(!writer.oldRawData());
    } else {
        KisRandomAccessor2 writer(&dm, 0, 0, true, nullptr);
        writer.moveTo(65, 1);
        QVERIFY(!writer.rawData());
        QVERIFY(!writer.oldRawData());
    }
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(1));
    auto old = before.readResidentPage({{1}, {1, 0}});
    QVERIFY(old.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(old.data()) + (64 + 1) * bpp, bpp), initial);
    old = {};
    held = {};
    QVERIFY(mutation.cancel());
    QVERIFY(store->abort(foreign));
    QVERIFY(dm.tryAbort(memento));
    QByteArray actual(bpp, Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 65, 1, 1, 1);
    QCOMPARE(actual, initial);
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
    // Direct removePages still tests cancellation after a prepared removal;
    // clearAll is a whole-index barrier and now refuses before that work.
    QCOMPARE(after.removalsCancelled - before.removalsCancelled, quint64(clearAll ? 0 : 1));
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
    if (copy)
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("^PageStore CPU mutation acquisition failed:.*legacy mutation cannot reenter its page.*$"));
    QVERIFY(!(copy ? backend->copyFrom(*source.m_pageStoreBackend, source.m_pageStoreBackend->captureReadView(),
                                     backend->captureReadView(), area, false, &error, &changes)
                  : backend->fillRect(area, changedPixel, &error, &changes)));
    QVERIFY(changes.isEmpty());
    const auto after = store->mutationStatistics();
    QCOMPARE(after.generationsReserved - before.generationsReserved, quint64(copy ? 1 : 0));
    QCOMPARE(after.pagesCancelled - before.pagesCancelled, quint64(copy ? 1 : 0));
    QCOMPARE(after.removalsCancelled - before.removalsCancelled, quint64(copy && removal ? 1 : 0));
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
    QVERIFY(dm.m_pageStoreBackend && dm.m_pageStoreBackend->isOperational());
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
    // One operation-start body View; successful index refresh is metadata-only.
    // Test-only captures at Prepare remain counted separately.
    QCOMPARE(store->readScopeStatistics().capturedReadViewsCreated - views.capturedReadViewsCreated,
             quint64(1 + copies));
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
    QCOMPARE(after.maximumPinnedPagesPerExecution, quint64(1));
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

void KisTiledDataManagerTest::testPageStoreCopyBoundsSelection_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("rough"); QTest::addColumn<int>("mode");
    QTest::addColumn<bool>("native");
    // Frozen current, different sparse defaults, history-before, empty source.
    for (int bpp : {1, 4, 8, 16}) for (bool rough : {false, true}) for (int mode : {0, 1, 2, 3})
        for (bool native : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("B%1-rough%2-mode%3-native%4").arg(bpp).arg(rough).arg(mode).arg(native)))
                << bpp << rough << mode << native;
}

void KisTiledDataManagerTest::testPageStoreCopyBoundsSelection()
{
    QFETCH(int, bpp); QFETCH(bool, rough); QFETCH(int, mode);
    QFETCH(bool, native);
    const QByteArray blank(bpp, char(0x13)), targetDefault(bpp, char(mode == 1 ? 0x31 : 0x13));
    const QByteArray first(bpp, char(0x41)), replaced(bpp, char(0x71)), added(bpp, char(0x72));
    KisDataManager source(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    KisDataManager target(bpp, reinterpret_cast<const quint8 *>(targetDefault.constData()));
    if (!native) {
        // Exercise the original manager implementation with both bridges off,
        // before any tile or history can retain a backend capability.
        for (auto *manager : {&source, &target}) {
            manager->m_mementoManager->setPageStoreBridge(nullptr);
            delete manager->m_pageStoreBackend;
            manager->m_pageStoreBackend = nullptr;
        }
    }
    if (mode == 0 || mode == 2)
        source.writeBytes(reinterpret_cast<const quint8 *>(first.constData()), -128, 0, 1, 1);
    if (mode != 1)
        target.writeBytes(reinterpret_cast<const quint8 *>(first.constData()), 192, 0, 1, 1);
    const auto history = mode == 2 ? source.getMemento() : KisMementoSP{};
    const auto changeSource = [&] {
        source.writeBytes(reinterpret_cast<const quint8 *>(replaced.constData()), -128, 0, 1, 1);
        source.writeBytes(reinterpret_cast<const quint8 *>(added.constData()), 576, 0, 1, 1);
    };
    if (history) changeSource();
    auto *sourceStore = native ? source.m_pageStoreBackend->store() : nullptr;
    auto *targetStore = native ? target.m_pageStoreBackend->store() : nullptr;
    const auto sourceCaptures = native ? sourceStore->readScopeStatistics().capturedReadViewsCreated : 0;
    const auto targetCaptures = native ? targetStore->readScopeStatistics().capturedReadViewsCreated : 0;
    bool prepared = false, selectedBeforePreparation = false;
    using Phase = KisPageStoreDiagnosticPhase;
    KisPageStoreDiagnosticRecorder recorder(native, targetStore);
    recorder.setPhaseObserver([&](Phase phase) {
        if (!native || phase != Phase::MutationPresencePrepared || prepared) return;
        prepared = true;
        selectedBeforePreparation = sourceStore->readScopeStatistics().capturedReadViewsCreated == sourceCaptures + 1 &&
            targetStore->readScopeStatistics().capturedReadViewsCreated == targetCaptures + 1;
        // A real source write after geometry selection cannot change copied
        // pixels or silently enlarge the selected source extent.
        if (mode == 0) changeSource();
    });
    const QRect request = mode == 1 ? QRect(-65, 1, 130, 2) : QRect(-4096, -4096, 8192, 8192);
    {
        KisStrokeJobFailureContext failure(true);
        if (history && rough) target.KisTiledDataManager::bitBltRoughOldData(&source, request);
        else if (history) target.KisTiledDataManager::bitBltOldData(&source, request);
        else if (rough) target.KisTiledDataManager::bitBltRough(&source, request);
        else target.KisTiledDataManager::bitBlt(&source, request);
        QVERIFY2(!failure.failed(), qPrintable(failure.error()));
    }
    recorder.setPhaseObserver({});
    if (native) {
        QVERIFY(prepared); QVERIFY(selectedBeforePreparation);
        const auto metrics = recorder.metrics();
        const quint64 selectedPages = mode == 1 ? 4 : mode == 3 ? 1 : 6;
        QCOMPARE(metrics[size_t(Phase::MutationPresencePrepare)].workItems, selectedPages);
        QCOMPARE(metrics[size_t(Phase::CopySourcePage)].workItems, selectedPages);
        QCOMPARE(targetStore->readScopeStatistics().capturedReadViewsCreated, targetCaptures + 1);
    }
    auto pixel = [&](int x, int y) {
        QByteArray result(bpp, Qt::Uninitialized);
        target.readBytes(reinterpret_cast<quint8 *>(result.data()), x, y, 1, 1);
        return result;
    };
    if (mode == 1) {
        QCOMPARE(pixel(-65, 1), blank);
        QCOMPARE(pixel(-65, 0), rough ? blank : targetDefault);
        QCOMPARE(pixel(999, 1), targetDefault);
    } else {
        QCOMPARE(pixel(-128, 0), mode == 3 ? blank : first);
        QCOMPARE(pixel(192, 0), blank); // destination-only content must be removed
        QCOMPARE(pixel(576, 0), blank); // outside the original fixed source
        QCOMPARE(target.extent(), mode == 3 ? QRect() : QRect(-128, 0, 64, 64));
    }
    if (history) { QVERIFY(source.tryCommit()); source.purgeHistory(history); }
}

void KisTiledDataManagerTest::testPageStoreClonePublishedBounds_data()
{
    QTest::addColumn<bool>("rough"); QTest::addColumn<bool>("packed");
    QTest::addColumn<bool>("warm"); QTest::addColumn<bool>("offset");
    QTest::addColumn<bool>("atSeal");
    for (bool rough : {false, true}) for (bool packed : {false, true})
        for (bool warm : {false, true}) for (bool offset : {false, true}) for (bool atSeal : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("rough%1-packed%2-warm%3-offset%4-seal%5")
                .arg(rough).arg(packed).arg(warm).arg(offset).arg(atSeal)))
                << rough << packed << warm << offset << atSeal;
}

void KisTiledDataManagerTest::testPageStoreClonePublishedBounds()
{
    QFETCH(bool, rough); QFETCH(bool, packed); QFETCH(bool, warm);
    QFETCH(bool, offset); QFETCH(bool, atSeal);
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP source = new KisPaintDevice(cs);
    if (offset) source->moveTo(19, -11);
    auto dm = source->dataManager();
    auto *backend = dm->m_pageStoreBackend; QVERIFY(backend);
    const QByteArray initial(4, char(0x31)), value(4, char(0x61));
    if (warm) dm->writeBytes(reinterpret_cast<const quint8 *>(initial.constData()), 0, 0, 1, 1);
    const auto before = backend->captureReadView(); QVERIFY(before.isValid());
    const QPoint local(-65, 2), global = local + QPoint(source->x(), source->y());
    const QRect requested(global - QPoint(3, 3), QSize(8, 8));
    QByteArray cloned(4, char(0));
    bool observed = false, sourcePublished = false, cloneSucceeded = false;
    KisPageStoreDiagnosticRecorder recorder(true, backend->store());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        const auto wanted = atSeal ? KisPageStoreDiagnosticPhase::MutationSealCleanup
                                  : KisPageStoreDiagnosticPhase::PixelOperationCleanup;
        if (phase != wanted || observed) return;
        observed = true;
        const auto current = backend->captureReadView();
        auto read = current.readResidentPage({backend->surface(), {-2, 0}});
        sourcePublished = read.isValid() &&
            static_cast<const quint8 *>(read.data())[2 * read.rowStride() + 63 * 4] == 0x61;
        KisPaintDeviceSP target = new KisPaintDevice(cs);
        KisStrokeJobFailureContext failures(true);
        if (rough) target->makeCloneFromRough(source, requested);
        else target->makeCloneFrom(source, requested);
        target->readBytes(reinterpret_cast<quint8 *>(cloned.data()), global.x(), global.y(), 1, 1);
        cloneSucceeded = !failures.failed();
    });
    if (packed) {
        KisStrokeJobFailureContext failures(true);
        dm->writeBytes(reinterpret_cast<const quint8 *>(value.constData()), local.x(), local.y(), 1, 1);
        QVERIFY(!failures.failed());
    } else {
        QCOMPARE(dm->writePageStoreOperation({QRect(local, QSize(1, 1))}, [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(local.x(), local.y()); if (!cursor->rawData()) return false;
            std::memcpy(cursor->rawData(), value.constData(), 4); return true;
        }), KisPageStoreWriteOperationResult::Succeeded);
    }
    recorder.setPhaseObserver({});
    QVERIFY(observed); QVERIFY(sourcePublished); QVERIFY(cloneSucceeded);
    QCOMPARE(cloned, value);
    auto old = before.readResidentPage({backend->surface(), {-2, 0}});
    QVERIFY(old.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(old.data()) + 2 * old.rowStride() + 63 * 4, 4), QByteArray(4, char(0)));
}

void KisTiledDataManagerTest::testPageStoreFreshReadSelection_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("packed");
    QTest::addColumn<bool>("warm"); QTest::addColumn<bool>("hold"); QTest::addColumn<bool>("clear");
    for (int bpp : {1, 4, 8, 16}) for (bool packed : {false, true})
        for (bool warm : {false, true}) for (bool hold : {false, true}) for (bool clear : {false, true}) {
            if (packed && clear) continue;
            QTest::newRow(qPrintable(QString("B%1-P%2-W%3-H%4-C%5").arg(bpp).arg(packed).arg(warm).arg(hold).arg(clear)))
                << bpp << packed << warm << hold << clear;
        }
}

void KisTiledDataManagerTest::testPageStoreFreshReadSelection()
{
    QFETCH(int, bpp); QFETCH(bool, packed); QFETCH(bool, warm); QFETCH(bool, hold); QFETCH(bool, clear);
    const QByteArray blank(bpp, 0), old(bpp, char(0x31)), value(bpp, char(0x61));
    KisTiledDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    if (warm) dm.writeBytes(reinterpret_cast<const quint8 *>(old.constData()), 0, 0, 1, 1);
    auto *backend = dm.m_pageStoreBackend;
    const auto before = backend->captureReadView();
    auto retained = dm.getTile(0, 0, false); QVERIFY(retained); QVERIFY(retained->lockForRead());
    bool held = true;
    const auto release = qScopeGuard([&] { if (held) retained->unlockForRead(); });
    const quint8 *pointer = retained->data();
    if (!hold) { retained->unlockForRead(); held = false; }
    const auto pixel = [&](const KisTileSP &tile) {
        if (!tile || !tile->lockForRead()) return QByteArray{};
        const QByteArray result(reinterpret_cast<const char *>(tile->data()), bpp);
        tile->unlockForRead();
        return result;
    };
    bool coherent = true;
    const auto observe = [&] {
        bool present = false;
        const auto lazy = dm.getReadOnlyTileLazy(0, 0, present);
        coherent &= present && pixel(lazy) == value && pixel(dm.getTile(0, 0, false)) == value;
        if (held) coherent &= QByteArray(reinterpret_cast<const char *>(pointer), bpp) == (warm ? old : blank);
    };
    int sealed = 0, installed = 0;
    bool publicationBlocked = false, publicationCoherent = false;
    QSemaphore publicationReady, publicationDone;
    std::thread publicationReader;
    KisPageStoreDiagnosticRecorder recorder(true, backend->store());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase == KisPageStoreDiagnosticPhase::MutationSealCleanup) {
            ++sealed;
            observe();
            publicationReader = std::thread([&] {
                const auto view = backend->captureReadView();
                KisSurfaceEpochState state;
                const bool canonical = view.isValid() &&
                    view.resolveSurfaceState(backend->surface(), &state) &&
                    state.contentExtent == QRect(0, 0, 64, 64) &&
                    pixel(dm.getTile(0, 0, false)) == value;
                publicationReady.release();
                publicationCoherent = canonical && dm.extent() == state.contentExtent;
                publicationDone.release();
            });
            QVERIFY(publicationReady.tryAcquire(1, 5000));
            publicationBlocked = !publicationDone.tryAcquire(1, 50);
        }
        if (phase == KisPageStoreDiagnosticPhase::MutationAdapterInstalled) {
            ++installed;
            if (publicationBlocked) QVERIFY(publicationDone.tryAcquire(1, 5000));
            observe();
        }
    });
    if (packed || clear) {
        KisStrokeJobFailureContext failure(true);
        if (clear) dm.clear(0, 0, 1, 1, reinterpret_cast<const quint8 *>(value.constData()));
        else dm.writeBytes(reinterpret_cast<const quint8 *>(value.constData()), 0, 0, 1, 1);
        QVERIFY(!failure.failed());
    } else {
        QCOMPARE(dm.writePageStoreOperation({QRect(0, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(0, 0); if (!cursor->rawData()) return false;
            std::memcpy(cursor->rawData(), value.constData(), size_t(bpp)); return true;
        }), KisPageStoreWriteOperationResult::Succeeded);
    }
    recorder.setPhaseObserver({});
    if (publicationReader.joinable()) publicationReader.join();
    QCOMPARE(sealed, 1); QCOMPARE(installed, 1);
    QVERIFY(publicationBlocked); QVERIFY(publicationCoherent);
    std::thread independentReader(observe); independentReader.join();
    QVERIFY(coherent);
    if (held) { retained->unlockForRead(); held = false; }
    QCOMPARE(pixel(dm.getTile(0, 0, false)), value);
    auto fixed = before.readResidentPage({backend->surface(), {0, 0}}); QVERIFY(fixed.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(fixed.data()), bpp), warm ? old : blank);
}

void KisTiledDataManagerTest::testPageStoreFreshReadWarmPath_data()
{
    QTest::addColumn<int>("bpp");
    for (int bpp : {1, 4, 8, 16}) QTest::newRow(qPrintable(QString::number(bpp))) << bpp;
}

void KisTiledDataManagerTest::testPageStoreFreshReadWarmPath()
{
    QFETCH(int, bpp);
    const QByteArray blank(bpp, char(0x13)), value(bpp, char(0x61));
    KisTiledDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    dm.writeBytes(reinterpret_cast<const quint8 *>(value.constData()), 0, 0, 1, 1);
    auto warm = dm.getTile(0, 0, false); QVERIFY(warm);
    auto *store = dm.m_pageStoreBackend->store();
    const auto before = store->sessionStats();
    const auto captures = store->readScopeStatistics().capturedReadViewsCreated;
    for (int i = 0; i < 1000; ++i) {
        bool present = false;
        auto tile = dm.getReadOnlyTileLazy(0, 0, present); QVERIFY(present); QCOMPARE(tile, warm);
        QVERIFY(tile->lockForRead());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(tile->data()), bpp), value);
        tile->unlockForRead();
        QCOMPARE(dm.getTile(0, 0, false), warm);
        auto missing = dm.getReadOnlyTileLazy(3, -2, present); QVERIFY(!present); QVERIFY(missing);
        QVERIFY(missing->lockForRead());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(missing->data()), bpp), blank);
        missing->unlockForRead();
    }
    QCOMPARE(store->readScopeStatistics().capturedReadViewsCreated, captures);
    QCOMPARE(store->sessionStats().readRequestsCreated, before.readRequestsCreated);
    QCOMPARE(store->sessionStats().defaultMaterializationRequests, before.defaultMaterializationRequests);
    QCOMPARE(store->sessionStats().registeredPages, before.registeredPages);
}

void KisTiledDataManagerTest::testPageStoreFreshReadOutlivesManager()
{
    const quint8 blank = 0, old = 0x31, value = 0x61;
    KisTileSP fresh;
    {
        KisTiledDataManager dm(1, &blank);
        dm.writeBytes(&old, 0, 0, 1, 1);
        auto retained = dm.getTile(0, 0, false); QVERIFY(retained); QVERIFY(retained->lockForRead());
        const auto release = qScopeGuard([&] { retained->unlockForRead(); });
        dm.writeBytes(&value, 0, 0, 1, 1);
        fresh = dm.getTile(0, 0, false); QVERIFY(fresh); QVERIFY(fresh != retained);
        QVERIFY(fresh->lockForRead()); QCOMPARE(fresh->data()[0], value); fresh->unlockForRead();
    }
    kisDrainPageStoreReclamation();
    QVERIFY(fresh->lockForRead()); QCOMPARE(fresh->data()[0], value); fresh->unlockForRead();
    // This is an ordinary detached wrapper: later private edits must neither
    // consult a dead bridge nor keep using the earlier provider read cache.
    QVERIFY(fresh->lockForWrite()); fresh->data()[0] = 0x72; fresh->unlockForWrite();
    QVERIFY(fresh->lockForRead()); QCOMPARE(fresh->data()[0], quint8(0x72)); fresh->unlockForRead();
}

void KisTiledDataManagerTest::testPageStoreFreshReadDefaultRevision_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("intent");
    for (int bpp : {1, 4, 8, 16}) for (bool intent : {false, true})
        QTest::newRow(qPrintable(QString("B%1-intent%2").arg(bpp).arg(intent))) << bpp << intent;
}

void KisTiledDataManagerTest::testPageStoreFreshReadDefaultRevision()
{
    QFETCH(int, bpp); QFETCH(bool, intent);
    const QByteArray blank(bpp, char(0x13)), next(bpp, char(0x31));
    KisTiledDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    if (intent) { auto tile = dm.getTile(2, 0, true); QVERIFY(tile->lockForWrite()); tile->unlockForWrite(); }
    auto old = dm.getTile(2, 0, false); QVERIFY(old); QVERIFY(old->lockForRead());
    const auto release = qScopeGuard([&] { old->unlockForRead(); });
    const auto *oldPointer = old->data();
    auto *store = dm.m_pageStoreBackend->store();
    const auto before = store->sessionStats();
    for (const auto &value : {next, blank, next}) {
        dm.setDefaultPixel(reinterpret_cast<const quint8 *>(value.constData()));
        bool present = true;
        auto fresh = dm.getReadOnlyTileLazy(2, 0, present); QVERIFY(fresh); QVERIFY(!present);
        QVERIFY(fresh->lockForRead());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(fresh->data()), bpp), value);
        fresh->unlockForRead();
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(oldPointer), bpp), blank);
    }
    QCOMPARE(store->sessionStats().registeredPages, before.registeredPages);
    QCOMPARE(store->sessionStats().defaultMaterializationRequests, before.defaultMaterializationRequests);
}

void KisTiledDataManagerTest::testPageStoreFreshReadFailure()
{
    const quint8 blank = 0, value = 0x61;
    KisTiledDataManager dm(1, &blank);
    dm.writeBytes(&value, 0, 0, 1, 1);
    auto retained = dm.getTile(0, 0, false); QVERIFY(retained);
    QVERIFY(dm.m_pageStoreBackend->store()->closeSession());
    KisStrokeJobFailureContext failure(true);
    bool present = true;
    QVERIFY(!dm.getReadOnlyTileLazy(0, 0, present)); QVERIFY(!present);
    QVERIFY(!dm.getTile(0, 0, false));
    KisTileDataWrapper read(&dm, 0, 0, KisTileDataWrapper::READ);
    QVERIFY(!read.data()); QVERIFY(failure.failed());
    // Failure of a new selection does not revoke the old wrapper's storage.
    QVERIFY(retained->lockForRead()); QCOMPARE(retained->data()[0], value); retained->unlockForRead();
}

void KisTiledDataManagerTest::testPageStoreFreshReadDefaultPressure()
{
    DefaultBudgetScope budget;
    const QByteArray blank(4, char(0x13)), next(4, char(0x31));
    KisTiledDataManager dm(4, reinterpret_cast<const quint8 *>(blank.constData()));
    auto old = dm.getTile(0, 0, false); QVERIFY(old); QVERIFY(old->lockForRead());
    const auto release = qScopeGuard([&] { old->unlockForRead(); });
    const quint8 *pointer = old->data();
    auto *backend = dm.m_pageStoreBackend;
    // Reproduce the default metadata/compatibility boundary without silently
    // updating the old hash default. The new value is already canonical.
    QVERIFY(backend->setDefaultPixel(next));
    const auto before = backend->store()->sessionStats();
    budget.saturate(4, reinterpret_cast<const quint8 *>(blank.constData()));
    {
        KisStrokeJobFailureContext failure(true);
        bool present = true;
        QVERIFY(!dm.getReadOnlyTileLazy(0, 0, present)); QVERIFY(!present); QVERIFY(failure.failed());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(pointer), 4), blank);
    }
    budget.release();
    bool present = true;
    auto fresh = dm.getReadOnlyTileLazy(0, 0, present); QVERIFY(fresh); QVERIFY(!present);
    QVERIFY(fresh->lockForRead());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(fresh->data()), 4), next);
    fresh->unlockForRead();
    QCOMPARE(backend->store()->sessionStats().registeredPages, before.registeredPages);
    QCOMPARE(backend->store()->sessionStats().defaultMaterializationRequests, before.defaultMaterializationRequests);
}

void KisTiledDataManagerTest::testPageStoreIdentityCacheLifetime_data()
{
    QTest::addColumn<int>("bpp");
    QTest::addColumn<int>("boundary");
    for (int bpp : {1, 4, 8, 16}) {
        for (int boundary = 0; boundary != 4; ++boundary)
            QTest::newRow(qPrintable(QString("bpp%1-boundary%2").arg(bpp).arg(boundary))) << bpp << boundary;
    }
}

void KisTiledDataManagerTest::testPageStoreIdentityCacheLifetime()
{
    QFETCH(int, bpp);
    QFETCH(int, boundary);
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31));
    KisTiledDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    // The packed path refreshes a canonical cache via a native read guard;
    // uniform clear instead uses an immutable TileData fast path.
    dm.writeBytes(reinterpret_cast<const quint8 *>(initial.constData()), 0, 0, 1, 1);
    auto *store = dm.m_pageStoreBackend->store();
    auto tile = dm.getTile(0, 0, false);
    QVERIFY(tile);
    const auto requests = store->sessionStats().readRequestsCreated;
    const auto captures = store->readScopeStatistics().capturedReadViewsCreated;
    for (int i = 0; i != 1000; ++i) {
        QVERIFY(tile->lockForRead());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(tile->data()), bpp), initial);
        QVERIFY(tile->lockForRead());
        tile->unlockForRead();
        tile->unlockForRead();
    }
    QCOMPARE(store->sessionStats().readRequestsCreated, requests);
    QCOMPARE(store->readScopeStatistics().capturedReadViewsCreated, captures);
    QVERIFY(tile->lockForRead());
    QVERIFY(!KisTileDataStore::instance()->trySwapTileData(tile->tileData()));
    tile->unlockForRead();
    QVERIFY(KisTileDataStore::instance()->trySwapTileData(tile->tileData()));
    QVERIFY(tile->lockForRead());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(tile->data()), bpp), initial);
    tile->unlockForRead();

    if (boundary == 0) {
        for (int i = 0; i != 10; ++i) {
            QVERIFY(tile->lockForWrite());
            auto *bytes = tile->tryWriteData(); QVERIFY(bytes);
            memset(bytes, 0x60 + i, size_t(bpp));
            tile->unlockForWrite();
            const auto before = store->readScopeStatistics().capturedReadViewsCreated;
            QVERIFY(tile->lockForRead());
            QCOMPARE(QByteArray(reinterpret_cast<const char *>(tile->data()), bpp), QByteArray(bpp, char(0x60 + i)));
            tile->unlockForRead();
            QCOMPARE(store->readScopeStatistics().capturedReadViewsCreated, before);
            QCOMPARE(store->sessionStats().readRequestsCreated, requests);
        }
    } else if (boundary == 1 || boundary == 2) {
        const QByteArray replacement(bpp, char(0x52));
        if (boundary == 1) dm.clear(); // detached
        else {
            // Replacing the hash entry invokes notifyDeadWithoutDetaching.
            // Normal canonical clear refreshes the same wrapper in place,
            // so it does not exercise this lifetime boundary by itself.
            KisTileSP newTile = new KisTile(*tile, 0, 0, dm.m_mementoManager);
            newTile->setPageStoreBridge(dm.m_pageStoreBackend, false);
            dm.m_hashTable->addTile(newTile);
            dm.clear(QRect(0, 0, 64, 64), reinterpret_cast<const quint8 *>(replacement.constData()));
        }
        QVERIFY(tile->lockForRead());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(tile->data()), bpp), initial);
        tile->unlockForRead();
        QVERIFY(tile->lockForWrite());
        auto *bytes = tile->tryWriteData(); QVERIFY(bytes);
        memset(bytes, 0x76, size_t(bpp));
        tile->unlockForWrite();
        QByteArray actual(bpp, 0);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1);
        QCOMPARE(actual, boundary == 1 ? blank : replacement);
    } else {
        const auto history = dm.getMemento(); QVERIFY(history);
        auto old = dm.getOldTile(0, 0); QVERIFY(old);
        const QByteArray replacement(bpp, char(0x67));
        dm.writeBytes(reinterpret_cast<const quint8 *>(replacement.constData()), 0, 0, 1, 1);
        dm.commit();
        dm.purgeHistory(history);
        kisDrainPageStoreReclamation();
        QVERIFY(old->lockForRead());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(old->data()), bpp), initial);
        old->unlockForRead();
        // The old wrapper must not reselect the newly published current page.
        QVERIFY(old->lockForRead());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(old->data()), bpp), initial);
        old->unlockForRead();
    }
    QCOMPARE(store->sessionStats().activeReadLeases, qsizetype(0));
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

void KisTiledDataManagerTest::testPageStoreNestedTileCapabilityLifetime_data()
{
    QTest::addColumn<bool>("history");
    QTest::addColumn<bool>("writeFirst");
    for (bool history : {false, true}) {
        for (bool writeFirst : {false, true}) {
            QTest::newRow(qPrintable(QStringLiteral("history%1-write-first%2")
                              .arg(history).arg(writeFirst)))
                << history << writeFirst;
        }
    }
}

void KisTiledDataManagerTest::testPageStoreNestedTileCapabilityLifetime()
{
    QFETCH(bool, history);
    QFETCH(bool, writeFirst);
    const quint8 blank = 0;
    const quint8 initial = 0x31;
    KisTiledDataManager dm(1, &blank);
    dm.clear(QRect(0, 0, 64, 64), &initial);
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    auto *store = dm.m_pageStoreBackend->store();
    const auto sessions = store->sessionStats();
    const auto mutations = store->mutationStatistics();
    KisTileSP tile = dm.getTile(0, 0, true);
    QVERIFY(tile);

    quint8 *outerRead = nullptr;
    if (writeFirst) {
        QVERIFY(tile->lockForWrite());
        QVERIFY(tile->lockForRead());
    } else {
        QVERIFY(tile->lockForRead());
        outerRead = tile->data();
        QVERIFY(outerRead);
        QCOMPARE(*outerRead, initial);
        QVERIFY(tile->lockForWrite());
    }
    quint8 *write = tile->tryWriteData();
    QVERIFY(write);
    *write = 0x62;

    if (writeFirst) {
        tile->unlockForRead();
        QCOMPARE(*write, quint8(0x62));
        tile->unlockForWrite();
    } else {
        tile->unlockForWrite();
        QCOMPARE(*outerRead, initial);
        QCOMPARE(*write, quint8(0x62));
        tile->unlockForRead();
    }

    QCOMPARE(store->mutationStatistics().generationsReserved -
                 mutations.generationsReserved,
             quint64(1));
    QCOMPARE(store->mutationStatistics().pagesSealed - mutations.pagesSealed,
             quint64(1));
    QCOMPARE(store->sessionStats().committedTransactions -
                 sessions.committedTransactions,
             quint64(history ? 0 : 1));
    quint8 actual = 0;
    dm.readBytes(&actual, 0, 0, 1, 1);
    QCOMPARE(actual, quint8(0x62));
    if (history) {
        dm.commit();
        dm.rollback(memento);
        dm.readBytes(&actual, 0, 0, 1, 1);
        QCOMPARE(actual, initial);
        dm.rollforward(memento);
        dm.readBytes(&actual, 0, 0, 1, 1);
        QCOMPARE(actual, quint8(0x62));
        dm.purgeHistory(memento);
    }
}

void KisTiledDataManagerTest::testPageStoreClearBarrierCancelsLegacyWriter()
{
    const quint8 blank = 0;
    const quint8 initial = 0x31;
    KisTiledDataManager dm(1, &blank);
    dm.clear(QRect(0, 0, 64, 64), &initial);
    auto *store = dm.m_pageStoreBackend->store();
    const auto sessions = store->sessionStats();
    KisTileSP writer = dm.getTile(0, 0, true);
    QVERIFY(writer->lockForWrite());
    quint8 *borrowed = writer->tryWriteData();
    QVERIFY(borrowed);
    *borrowed = 0x62;

    // The void legacy clear is a sequencing barrier. It cancels the live
    // compatibility mutation before publishing the removal; later writes
    // through the detached raw pointer cannot republish that transaction.
    dm.clear();
    QVERIFY(!KisTileDataStore::instance()->trySwapTileData(writer->tileData()));
    *borrowed = 0x73;
    writer->unlockForWrite();

    quint8 actual = 0xff;
    dm.readBytes(&actual, 0, 0, 1, 1);
    QCOMPARE(actual, blank);
    QCOMPARE(store->sessionStats().activeTransactions, qsizetype(0));
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(0));
    QCOMPARE(store->sessionStats().committedTransactions -
                 sessions.committedTransactions,
             quint64(1));
}

void KisTiledDataManagerTest::testIteratorClearBarrier_data()
{
    QTest::addColumn<int>("entry");
    QTest::addColumn<int>("bpp");
    QTest::addColumn<bool>("history");
    for (int entry = 0; entry < 3; ++entry)
        for (int bpp : {1, 4, 8, 16})
            for (bool history : {false, true})
                QTest::newRow(qPrintable(QString("entry%1-bpp%2-history%3").arg(entry).arg(bpp).arg(history)))
                    << entry << bpp << history;
}

void KisTiledDataManagerTest::testIteratorClearBarrier()
{
    QFETCH(int, entry);
    QFETCH(int, bpp);
    QFETCH(bool, history);
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    dm.clear(0, 0, 128, 64, reinterpret_cast<const quint8 *>(initial.constData()));
    auto *backend = dm.m_pageStoreBackend;
    auto *store = backend->store();
    auto before = backend->captureReadView();
    QVERIFY(before.isValid());
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    if (history) QVERIFY(memento);

    std::unique_ptr<KisHLineIterator2> horizontal;
    std::unique_ptr<KisVLineIterator2> vertical;
    std::unique_ptr<KisRandomAccessor2> random;
    quint8 *borrowed = nullptr;
    if (entry == 0) {
        horizontal = std::make_unique<KisHLineIterator2>(&dm, 1, 1, 1, 0, 0, true, nullptr);
        borrowed = horizontal->rawData();
    } else if (entry == 1) {
        vertical = std::make_unique<KisVLineIterator2>(&dm, 1, 1, 1, 0, 0, true, nullptr);
        borrowed = vertical->rawData();
    } else {
        random = std::make_unique<KisRandomAccessor2>(&dm, 0, 0, true, nullptr);
        random->moveTo(1, 1);
        borrowed = random->rawData();
    }
    QVERIFY(borrowed);
    memset(borrowed, 0x62, bpp);
    auto oldTile = dm.getTile(0, 0, true);
    // A second live iterator shares the original batch on another page.
    auto sibling = std::make_unique<KisRandomAccessor2>(&dm, 0, 0, true, nullptr);
    sibling->moveTo(65, 1);
    QVERIFY(sibling->rawData());
    memset(sibling->rawData(), 0x63, bpp);
    auto siblingTile = dm.getTile(1, 0, true);

    KisStrokeJobFailureContext errors(true);
    dm.clear();
    QVERIFY(!KisStrokeJobFailureContext::currentJobHasFailed());
    QCOMPARE(dm.extent(), QRect());
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(0));
    QCOMPARE(store->sessionStats().activeTransactions, qsizetype(history ? 1 : 0));
    QVERIFY(!KisTileDataStore::instance()->trySwapTileData(oldTile->tileData()));
    QVERIFY(!KisTileDataStore::instance()->trySwapTileData(siblingTile->tileData()));
    memset(borrowed, 0x73, bpp);
    memset(sibling->rawData(), 0x74, bpp);

    // The new scope starts before the old scopes are destroyed. Late old
    // clients must not erase this batch's thread slot or publish their bytes.
    auto replacement = std::make_unique<KisRandomAccessor2>(&dm, 0, 0, true, nullptr);
    replacement->moveTo(1, 1);
    QVERIFY(replacement->rawData());
    memset(replacement->rawData(), 0x45, bpp);
    sibling->moveTo(129, 1); // revoked scope cannot acquire a new current tile
    QVERIFY(!sibling->rawData());
    QVERIFY(!sibling->oldRawData());
    sibling.reset();
    horizontal.reset(); vertical.reset(); random.reset();
    QVERIFY(KisTileDataStore::instance()->trySwapTileData(oldTile->tileData()));
    QVERIFY(KisTileDataStore::instance()->trySwapTileData(siblingTile->tileData()));
    QVERIFY(backend->hasCurrentThreadIteratorWrites());
    replacement->moveTo(65, 1);
    QVERIFY(replacement->rawData());
    memset(replacement->rawData(), 0x46, bpp);
    replacement.reset();
    QVERIFY(!backend->hasCurrentThreadIteratorWrites());

    QByteArray actual(bpp, Qt::Uninitialized);
    auto readPixel = [&](int x, int y) {
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), x, y, 1, 1);
        return actual;
    };
    QCOMPARE(readPixel(1, 1), QByteArray(bpp, char(0x45)));
    QCOMPARE(readPixel(65, 1), QByteArray(bpp, char(0x46)));
    QCOMPARE(readPixel(2, 1), blank);
    QCOMPARE(readPixel(129, 1), blank);
    auto old = before.readResidentPage({{1}, {0, 0}});
    QVERIFY(old.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(old.data()) + 65 * bpp, bpp), initial);
    if (history) {
        QVERIFY(dm.tryCommit());
        dm.rollback(memento);
        QCOMPARE(readPixel(1, 1), initial);
        QCOMPARE(readPixel(65, 1), initial);
        dm.rollforward(memento);
        QCOMPARE(readPixel(1, 1), QByteArray(bpp, char(0x45)));
        QCOMPARE(readPixel(65, 1), QByteArray(bpp, char(0x46)));
        QCOMPARE(readPixel(2, 1), blank);
        dm.purgeHistory(memento);
    }
    QCOMPARE(store->sessionStats().activeTransactions, qsizetype(0));
    QCOMPARE(store->sessionStats().activeCpuWritePages, qsizetype(0));
}

void KisTiledDataManagerTest::testIteratorClearRejectsForeignBatch()
{
    const quint8 blank = 0, initial = 0x31;
    KisDataManager dm(1, &blank);
    dm.clear(0, 0, 128, 64, &initial);
    QSemaphore ready, release;
    bool foreignAcquired = false;
    std::thread worker([&] {
        KisRandomAccessor2 writer(&dm, 0, 0, true, nullptr);
        writer.moveTo(65, 1);
        foreignAcquired = writer.rawData();
        if (foreignAcquired) *writer.rawData() = 0x64;
        ready.release();
        release.acquire();
    });
    bool joined = false;
    const auto join = qScopeGuard([&] { if (!joined) { release.release(); worker.join(); } });
    ready.acquire();
    QVERIFY(foreignAcquired);
    auto local = std::make_unique<KisRandomAccessor2>(&dm, 0, 0, true, nullptr);
    local->moveTo(1, 1);
    QVERIFY(local->rawData());
    *local->rawData() = 0x63;
    QString error;
    QVERIFY(!dm.m_pageStoreBackend->clearAll(&error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(dm.m_pageStoreBackend->store()->sessionStats().activeCpuWritePages, qsizetype(2));
    QCOMPARE(*local->rawData(), quint8(0x63));
    local->moveTo(2, 1);
    QVERIFY(local->rawData());
    *local->rawData() = 0x65;
    local.reset();
    release.release();
    worker.join();
    joined = true;
    quint8 actual = 0;
    dm.readBytes(&actual, 1, 1, 1, 1);
    QCOMPARE(actual, quint8(0x63));
    dm.readBytes(&actual, 2, 1, 1, 1);
    QCOMPARE(actual, quint8(0x65));
    dm.readBytes(&actual, 65, 1, 1, 1);
    QCOMPARE(actual, quint8(0x64));
    dm.clear();
    QCOMPARE(dm.extent(), QRect());
    QCOMPARE(dm.m_pageStoreBackend->store()->sessionStats().activeTransactions, qsizetype(0));
}

void KisTiledDataManagerTest::testClearRejectsNativeBatch()
{
    const quint8 blank = 0, initial = 0x31;
    KisDataManager dm(1, &blank);
    dm.clear(0, 0, 64, 64, &initial);
    auto batch = dm.m_pageStoreBackend->beginMutationBatch();
    QVERIFY(batch);
    QString error;
    QVERIFY(!dm.m_pageStoreBackend->clearAll(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(batch->finish());
    quint8 actual = 0;
    dm.readBytes(&actual, 0, 0, 1, 1);
    QCOMPARE(actual, initial);
}

void KisTiledDataManagerTest::testClearRetainsBareLease_data()
{
    QTest::addColumn<bool>("iteratorBatch");
    QTest::newRow("standalone") << false;
    QTest::newRow("iterator") << true;
}

void KisTiledDataManagerTest::testClearRetainsBareLease()
{
    QFETCH(bool, iteratorBatch);
    const quint8 blank = 0;
    KisTiledDataManagerPageStoreBackend backend;
    QVERIFY(backend.configure(1, &blank));
    auto scope = iteratorBatch ? backend.beginIteratorMutationScope() : nullptr;
    if (iteratorBatch) QVERIFY(scope);
    // No compatibility Tile owns this backing. Cancellation must retain both
    // a residency pin and a reference until the actual lease is returned.
    auto lease = backend.acquireTile(0, 0, true, false);
    QVERIFY(lease && lease->writable());
    auto *tile = lease->tileData();
    auto *borrowed = tile->data();
    QVERIFY(borrowed);
    *borrowed = 0x62;
    QVERIFY(backend.clearAll());
    QVERIFY(!KisTileDataStore::instance()->trySwapTileData(tile));
    *borrowed = 0x73;
    if (scope) {
        QVERIFY(!scope->isActive());
        QVERIFY(scope->finish());
    }
    QVERIFY(lease->finish());
    lease.reset();
    QCOMPARE(backend.store()->sessionStats().activeTransactions, qsizetype(0));
    QCOMPARE(backend.store()->sessionStats().activeCpuWritePages, qsizetype(0));
    quint8 actual = 0xff;
    QVERIFY(backend.readBytes(&actual, 0, 0, 1, 1, 1));
    QCOMPARE(actual, blank);
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

    KisPageStore *store = dm.m_pageStoreBackend->store();
    QVERIFY(store->waitForRetirementIdle());
    const KisPageStoreSessionStats settled = store->sessionStats();
    QVERIFY(!settled.hasOutstandingCapabilities());
    QCOMPARE(settled.pendingHistoricalPages, qsizetype(0));
    QCOMPARE(settled.deferredHistoricalPages, qsizetype(0));
    QCOMPARE(settled.activeHistoryScans, qsizetype(0));
    QCOMPARE(settled.scheduledReclamationJobs, qsizetype(0));

    const KisPageBackingUsage usage = store->backingUsage();
    for (const KisPageBackingUsageBucket &bucket : usage.buckets) {
        QCOMPARE(bucket.reserved.cpuRam, quint64(0));
        QCOMPARE(bucket.reserved.umaShared, quint64(0));
        QCOMPARE(bucket.reserved.discreteVram, quint64(0));
        QCOMPARE(bucket.reserved.ssd, quint64(0));
    }
    const auto transientLive = [&usage](KisBackingBudgetClass kind) {
        return usage.buckets[size_t(kind)].live;
    };
    for (const KisBackingBudgetClass kind : {
             KisBackingBudgetClass::ActivePending,
             KisBackingBudgetClass::InFlight,
             KisBackingBudgetClass::RetirementDebt}) {
        const KisPageDomainBytes live = transientLive(kind);
        QCOMPARE(live.cpuRam, quint64(0));
        QCOMPARE(live.umaShared, quint64(0));
        QCOMPARE(live.discreteVram, quint64(0));
        QCOMPARE(live.ssd, quint64(0));
    }

    if (qEnvironmentVariableIntValue("KIS_BR1_PROFILE") && dm.m_pageStoreBackend) {
        // Counters are captured once after workers finish, outside timed
        // operation bodies. There is no diagnostic lock in the pixel path.
        const KisPageStoreSessionStats stats = store->sessionStats();
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

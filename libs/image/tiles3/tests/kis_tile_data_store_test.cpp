/*
 *  SPDX-FileCopyrightText: 2010 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_tile_data_store_test.h"
#include <simpletest.h>
#include <QScopeGuard>

#include "kis_debug.h"

#include "kis_image_config.h"

#include "tiles3/kis_tiled_data_manager.h"
#include "tiles3/kis_tile.h"
#include "tiles_test_utils.h"

#include "tiles3/kis_tile_data_store.h"
#include "tiles3/kis_tile_data_store_iterators.h"


void KisTileDataStoreTest::testClockIterator()
{
    KisTileDataStore *store = KisTileDataStore::instance();
    store->debugClear();

    const qint32 pixelSize = 1;
    quint8 defaultPixel = 128;

    QList<KisTileData*> tileDataList;
    KisTileData *item;

    item = new KisTileData(pixelSize, &defaultPixel, store, false);
    store->registerTileData(item);
    tileDataList.append(item);
    item = new KisTileData(pixelSize, &defaultPixel, store, false);
    store->registerTileData(item);
    tileDataList.append(item);
    item = new KisTileData(pixelSize, &defaultPixel, store, false);
    store->registerTileData(item);
    tileDataList.append(item);


    /// First, full cycle!
    KisTileDataStoreClockIterator *iter = store->beginClockIteration();

    QVERIFY(iter->hasNext());
    item = iter->next();
    QCOMPARE(item, tileDataList[0]);

    QVERIFY(iter->hasNext());
    item = iter->next();
    QCOMPARE(item, tileDataList[2]);

    QVERIFY(iter->hasNext());
    item = iter->next();
    QCOMPARE(item, tileDataList[1]);

    QVERIFY(!iter->hasNext());

    store->endIteration(iter);


    /// Second, iterate until the second item!
    iter = store->beginClockIteration();

    QVERIFY(iter->hasNext());
    item = iter->next();
    QCOMPARE(item, tileDataList[0]);

    store->endIteration(iter);


    /// Third, check the position restored!
    iter = store->beginClockIteration();

    QVERIFY(iter->hasNext());
    item = iter->next();
    QCOMPARE(item, tileDataList[2]);

    QVERIFY(iter->hasNext());
    item = iter->next();
    QCOMPARE(item, tileDataList[1]);

    QVERIFY(iter->hasNext());
    item = iter->next();
    QCOMPARE(item, tileDataList[0]);

    QVERIFY(!iter->hasNext());

    store->endIteration(iter);


    /// By this moment clock index has been set
    /// onto the last item.
    /// Let's try remove it and see what will happen...

    store->freeTileData(tileDataList[0]);

    iter = store->beginClockIteration();

    QVERIFY(iter->hasNext());
    item = iter->next();
    QCOMPARE(item, tileDataList[2]);

    QVERIFY(iter->hasNext());
    item = iter->next();
    QCOMPARE(item, tileDataList[1]);

    QVERIFY(!iter->hasNext());

    store->endIteration(iter);

    store->freeTileData(tileDataList[2]);
    store->freeTileData(tileDataList[1]);
}

void KisTileDataStoreTest::testLeaks()
{
    KisTileDataStore::instance()->debugClear();

    QCOMPARE(KisTileDataStore::instance()->numTiles(), 0);

    const qint32 pixelSize = 1;
    quint8 defaultPixel = 128;
    KisTiledDataManager *dm = new KisTiledDataManager(pixelSize, &defaultPixel);

    KisTileSP tile = dm->getTile(0, 0, true);
    QVERIFY(tile->lockForWrite());
    tile->unlockForWrite();

    tile = 0;

    delete dm;

    QCOMPARE(KisTileDataStore::instance()->numTiles(), 0);
}

#define COLUMN2COLOR(col) (col%255)

void KisTileDataStoreTest::testSwapping()
{
    KisImageConfig config(false);
    const qreal oldHard = config.memoryHardLimitPercent();
    const qreal oldSoft = config.memorySoftLimitPercent();
    const auto restore = qScopeGuard([&] {
        config.setMemoryHardLimitPercent(oldHard);
        config.setMemorySoftLimitPercent(oldSoft);
        KisTileDataStore::instance()->testingRereadConfig();
    });
    config.setMemoryHardLimitPercent(100.0 / KisImageConfig::totalRAM());
    config.setMemorySoftLimitPercent(0);

    KisTileDataStore::instance()->debugClear();
    KisTileDataStore::instance()->testingRereadConfig();



    const qint32 pixelSize = 1;
    quint8 defaultPixel = 128;
    KisTiledDataManager dm(pixelSize, &defaultPixel);

    for(qint32 col = 0; col < 1000; col++) {
        KisTileSP tile = dm.getTile(col, 0, true);
        QVERIFY(tile->lockForWrite());

        KisTileData *td = tile->tileData();
        QVERIFY(memoryIsFilled(defaultPixel, td->data(), TILESIZE));

        memset(td->data(), COLUMN2COLOR(col), TILESIZE);
        QVERIFY(memoryIsFilled(COLUMN2COLOR(col), td->data(), TILESIZE));

        tile->unlockForWrite();
    }

    //KisTileDataStore::instance()->debugSwapAll();

    for(qint32 col = 0; col < 1000; col++) {
        KisTileSP tile = dm.getTile(col, 0, true);
        QVERIFY(tile->lockForRead());

        KisTileData *td = tile->tileData();
        QVERIFY(memoryIsFilled(COLUMN2COLOR(col), td->data(), TILESIZE));
        tile->unlockForRead();
    }
}

void KisTileDataStoreTest::testTileLockPropagatesSwapInFailure_data()
{
    QTest::addColumn<int>("failurePoint");
    QTest::newRow("mapping")
        << int(KisSwapInFailurePoint::Mapping);
    QTest::newRow("allocation")
        << int(KisSwapInFailurePoint::Allocation);
    QTest::newRow("decompression")
        << int(KisSwapInFailurePoint::Decompression);
}

void KisTileDataStoreTest::testTileLockPropagatesSwapInFailure()
{
    QFETCH(int, failurePoint);
    auto *store = KisTileDataStore::instance();
    const quint8 initial = 0x45;
    auto *tileData = store->createDefaultTileData(1, &initial);
    {
        KisTile tile(0, 0, tileData, nullptr);
        QVERIFY(store->trySwapTileData(tileData));
        QVERIFY(!tileData->isResident());

        store->testingFailNextSwapIn(
            KisSwapInFailurePoint(failurePoint));
        QVERIFY(!tile.lockForRead());
        QVERIFY(!tileData->isResident());

        store->testingFailNextSwapIn(
            KisSwapInFailurePoint(failurePoint));
        QVERIFY(!tile.lockForWrite());
        QVERIFY(!tileData->isResident());

        QVERIFY(tile.lockForRead());
        QCOMPARE(tile.data()[0], initial);
        tile.unlockForRead();
    }
}

void KisTileDataStoreTest::testResidentHardAdmission()
{
    KisImageConfig config(false);
    const qreal oldHard = config.memoryHardLimitPercent();
    const qreal oldSoft = config.memorySoftLimitPercent();
    const qreal oldPool = config.memoryPoolLimitPercent();
    const auto restore = qScopeGuard([&] {
        config.setMemoryHardLimitPercent(oldHard);
        config.setMemorySoftLimitPercent(oldSoft);
        config.setMemoryPoolLimitPercent(oldPool);
        KisTileDataStore::instance()->testingRereadConfig();
    });
    config.setMemoryHardLimitPercent(1.1 * 100.0 / KisImageConfig::totalRAM());
    config.setMemorySoftLimitPercent(0);
    config.setMemoryPoolLimitPercent(0);

    KisTileDataStore *store = KisTileDataStore::instance();
    store->debugClear();
    store->testingRereadConfig();
    const quint64 hardBytes = quint64(KisImageConfig(true).tilesHardLimit()) << 20;
    constexpr qint32 pixelSize = 4;
    const quint64 tileBytes = quint64(pixelSize) * KisTileData::WIDTH * KisTileData::HEIGHT;
    QVERIFY(hardBytes >= tileBytes);
    const int maximumTiles = int(hardBytes / tileBytes);
    quint8 pixel[pixelSize]{};
    QVector<KisTileData *> pinned;
    pinned.reserve(maximumTiles);
    for (int i = 0; i < maximumTiles; ++i) {
        KisTileData *tile = store->createDefaultTileData(pixelSize, pixel);
        QVERIFY(tile);
        QVERIFY(tile->ref());
        QVERIFY(tile->blockSwapping());
        pinned.append(tile);
    }
    QCOMPARE(quint64(store->memoryMetric()) * KisTileData::WIDTH * KisTileData::HEIGHT,
             quint64(maximumTiles) * tileBytes);
    QVERIFY(!store->createDefaultTileData(pixelSize, pixel));
    for (KisTileData *tile : std::as_const(pinned)) {
        tile->unblockSwapping();
        tile->deref();
    }
    QCOMPARE(store->memoryMetric(), qint64(0));
}

SIMPLE_TEST_MAIN(KisTileDataStoreTest)

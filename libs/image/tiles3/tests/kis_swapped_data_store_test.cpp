/*
 *  SPDX-FileCopyrightText: 2010 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_swapped_data_store_test.h"
#include <simpletest.h>

#include <QRandomGenerator>

#include <utility>

#include "kis_debug.h"

#include "kis_image_config.h"

#include "tiles3/kis_tile_data.h"
#include "tiles_test_utils.h"

#include "tiles3/kis_tile_data_store.h"


#define COLUMN2COLOR(col) (col%255)

void KisSwappedDataStoreTest::testRoundTrip()
{
    const qint32 pixelSize = 1;
    const quint8 defaultPixel = 128;
    const qint32 NUM_TILES = 10000;

    KisImageConfig config(false);
    config.setMaxSwapSize(4);
    config.setSwapSlabSize(1);
    config.setSwapWindowSize(1);


    KisSwappedDataStore store;

    QList<KisTileData*> tileDataList;
    for(qint32 i = 0; i < NUM_TILES; i++)
        tileDataList.append(new KisTileData(pixelSize, &defaultPixel, KisTileDataStore::instance()));

    for(qint32 i = 0; i < NUM_TILES; i++) {
        KisTileData *td = tileDataList[i];
        QVERIFY(memoryIsFilled(defaultPixel, td->data(), TILESIZE));

        memset(td->data(), COLUMN2COLOR(i), TILESIZE);
        QVERIFY(memoryIsFilled(COLUMN2COLOR(i), td->data(), TILESIZE));

        // FIXME: take a lock of the tile data
        QVERIFY(store.trySwapOutTileData(td));
    }

    store.debugStatistics();

    for(qint32 i = 0; i < NUM_TILES; i++) {
        KisTileData *td = tileDataList[i];
        QVERIFY(!td->data());
        // TODO: check num clones

        // FIXME: take a lock of the tile data
        QVERIFY(store.swapInTileData(td));
        QVERIFY(memoryIsFilled(COLUMN2COLOR(i), td->data(), TILESIZE));
    }

    store.debugStatistics();

    for(qint32 i = 0; i < NUM_TILES; i++)
        delete tileDataList[i];
}

void KisSwappedDataStoreTest::processTileData(qint32 column, KisTileData *td, KisSwappedDataStore &store)
{
    if(td->data()) {
        memset(td->data(), COLUMN2COLOR(column), TILESIZE);
        QVERIFY(memoryIsFilled(COLUMN2COLOR(column), td->data(), TILESIZE));

        // FIXME: take a lock of the tile data
        QVERIFY(store.trySwapOutTileData(td));
    }
    else {
        // TODO: check num clones
        // FIXME: take a lock of the tile data
        QVERIFY(store.swapInTileData(td));
        QVERIFY(memoryIsFilled(COLUMN2COLOR(column), td->data(), TILESIZE));
    }
}

void KisSwappedDataStoreTest::testRandomAccess()
{
    QRandomGenerator rng(10);
    const qint32 pixelSize = 1;
    const quint8 defaultPixel = 128;
    const qint32 NUM_CYCLES = 50000;
    const qint32 NUM_TILES = 10000;

    KisImageConfig config(false);
    config.setMaxSwapSize(40);
    config.setSwapSlabSize(1);
    config.setSwapWindowSize(1);


    KisSwappedDataStore store;

    QList<KisTileData*> tileDataList;
    for(qint32 i = 0; i < NUM_TILES; i++)
        tileDataList.append(new KisTileData(pixelSize, &defaultPixel, KisTileDataStore::instance()));

    for(qint32 i = 0; i < NUM_CYCLES; i++) {
        if(!(i%5000))
            dbgKrita << i << "of" << NUM_CYCLES;

        qint32 col = rng.bounded(NUM_TILES);

        KisTileData *td = tileDataList[col];
        processTileData(col, td, store);
    }

    store.debugStatistics();

    for(qint32 i = 0; i < NUM_TILES; i++)
        delete tileDataList[i];
}

void KisSwappedDataStoreTest::testCapacityFailurePreservesResidentTile()
{
    KisImageConfig config(false);
    config.setMaxSwapSize(1);
    config.setSwapSlabSize(1);
    config.setSwapWindowSize(1);

    KisSwappedDataStore store;
    const qint32 pixelSize = 16;
    const quint8 defaultPixel[16] = {};
    QRandomGenerator rng(0x5a17);
    QList<KisTileData *> swapped;
    bool rejected = false;
    for (int i = 0; i < 32; ++i) {
        auto *tile = new KisTileData(
            pixelSize, defaultPixel, KisTileDataStore::instance());
        const qsizetype bytes = qsizetype(pixelSize)
            * KisTileData::WIDTH * KisTileData::HEIGHT;
        for (qsizetype byte = 0; byte < bytes; ++byte)
            tile->data()[byte] = quint8(rng.generate());
        if (!store.trySwapOutTileData(tile)) {
            rejected = true;
            QVERIFY(tile->data());
            delete tile;
            break;
        }
        swapped.append(tile);
    }
    QVERIFY(rejected);
    for (KisTileData *tile : std::as_const(swapped))
        delete tile;
}

void KisSwappedDataStoreTest::testReadFailurePreservesSwappedTile_data()
{
    QTest::addColumn<int>("failurePoint");
    QTest::newRow("mapping")
        << int(KisSwapInFailurePoint::Mapping);
    QTest::newRow("allocation")
        << int(KisSwapInFailurePoint::Allocation);
    QTest::newRow("decompression")
        << int(KisSwapInFailurePoint::Decompression);
}

void KisSwappedDataStoreTest::testReadFailurePreservesSwappedTile()
{
    QFETCH(int, failurePoint);
    KisImageConfig config(false);
    config.setMaxSwapSize(1);
    config.setSwapSlabSize(1);
    config.setSwapWindowSize(1);

    KisSwappedDataStore store;
    const quint8 defaultPixel = 0x37;
    auto *tile = new KisTileData(
        1, &defaultPixel, KisTileDataStore::instance());
    std::memset(tile->data(), 0x6d, TILESIZE);
    QVERIFY(store.trySwapOutTileData(tile));
    QVERIFY(!tile->data());
    const quint64 chunkBegin = tile->swapChunk().begin();
    const quint64 chunkSize = tile->swapChunk().size();
    const qint64 swapBytes = store.totalSwapMemoryUsed();
    const quint64 chunks = store.numTiles();

    store.testingFailNextSwapIn(
        KisSwapInFailurePoint(failurePoint));
    QVERIFY(!store.swapInTileData(tile));
    QVERIFY(!tile->data());
    QCOMPARE(tile->swapChunk().begin(), chunkBegin);
    QCOMPARE(tile->swapChunk().size(), chunkSize);
    QCOMPARE(store.totalSwapMemoryUsed(), swapBytes);
    QCOMPARE(store.numTiles(), chunks);

    QVERIFY(store.swapInTileData(tile));
    QVERIFY(memoryIsFilled(0x6d, tile->data(), TILESIZE));
    QCOMPARE(store.totalSwapMemoryUsed(), qint64(0));
    QCOMPARE(store.numTiles(), quint64(0));
    delete tile;
}

SIMPLE_TEST_MAIN(KisSwappedDataStoreTest)

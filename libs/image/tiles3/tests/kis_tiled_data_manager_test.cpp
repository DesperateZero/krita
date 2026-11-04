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
#include <type_traits>

#include "tiles3/kis_tiled_data_manager.h"
#include "tiles3/kis_tile_data_store.h"
#include "tiles3/kis_tile_data_wrapper.h"
#include "tiles3/swap/kis_legacy_tile_compressor.h"
#include "tiles3/swap/kis_tile_compressor_2.h"
#include "tiles3/kis_hline_iterator.h"
#include "tiles3/kis_vline_iterator.h"
#include "tiles3/kis_random_accessor.h"
#include "kis_wrapped_hline_iterator.h"
#include "kis_wrapped_vline_iterator.h"
#include "kis_repeat_iterators_pixel.h"
#include "pagestore/native/KisNativeResultDiagnostics_p.h"
#include "pagestore/KisTiles3PageReplicaProvider.h"
#include "KisPageStoreCpuSurfaceOps.h"
#include "KisPageStoreStoragePressure.h"
#include "pagestore/KisPageStoreDiagnostics_p.h"
#include "pagestore/KisPageStoreIteratorReadScope_p.h"
#include "pagestore/KisPageStoreReclamation_p.h"
#include "pagestore/KisPageWriteCoordinator_p.h"

#include "tiles_test_utils.h"
#include "kis_image_config.h"
#include "kis_paint_device.h"
#include "kis_painter.h"
#include <KoCompositeOpRegistry.h>
#include <KoColorSpaceRegistry.h>
#include "kis_tile_data_store_test_access.h"
#include "config-limit-long-tests.h"
#include "kis_br1_stress_profile.h"
#include "KisStrokeJobFailureContext.h"

// Exercise both template implementations without changing the production
// manager configuration or building a second copy of kritaimage.
#define KisTileHashTable Cp2AlternateTileHashTable
#define KisTileHashTableIterator Cp2AlternateTileHashTableIterator
#define KisTileHashTableConstIterator Cp2AlternateTileHashTableConstIterator
#ifdef USE_LOCK_FREE_HASH_TABLE
#include "tiles3/kis_tile_hash_table.h"
#else
#include "tiles3/kis_tile_hash_table2.h"
#endif
#undef KisTileHashTable
#undef KisTileHashTableIterator
#undef KisTileHashTableConstIterator

void KisTiledDataManagerTest::initTestCase()
{
    // KConfig 6.7 lazily updates its global filename after test mode changes.
    // Complete both initial construction and that transition on the main
    // thread before any stress worker opens its thread-local configuration.
    KConfig initialConfig;
    KConfig testModeConfig;
}

namespace {

class CloneFaultTile : public KisTile
{
public:
    using KisTile::KisTile;
    CloneFaultTile(const CloneFaultTile &source, KisMementoManager *manager)
        : KisTile(accepted(source), manager), copied(true) {}
    KisSharedPtr<CloneFaultTile> next() const
    { return static_cast<CloneFaultTile *>(KisTile::next().data()); }
    void setNext(KisSharedPtr<CloneFaultTile> tile)
    { KisTile::setNext(tile.data()); }
    void notifyDetachedFromDataManager()
    {
        if (copied) ++detached;
        KisTile::notifyDetachedFromDataManager();
    }
    static inline int remaining = -1;
    static inline int detached = 0;
private:
    static const CloneFaultTile &accepted(const CloneFaultTile &source)
    {
        if (remaining == 0) throw std::bad_alloc();
        if (remaining > 0) --remaining;
        return source;
    }
    bool copied = false;
};

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

void KisTiledDataManagerTest::testPageStoreWritablePresence_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("mode");
    for (int bpp : {1, 4, 8, 16}) for (int mode = 0; mode < 3; ++mode)
        QTest::newRow(qPrintable(QString("%1-mode%2").arg(bpp).arg(mode))) << bpp << mode;
}

void KisTiledDataManagerTest::testPageStoreWritablePresence()
{
    QFETCH(int, bpp); QFETCH(int, mode);
    const QByteArray blank(bpp, char(0x13)), other(bpp, char(0x29));
    auto bytes = [](const QByteArray &p) { return reinterpret_cast<const quint8 *>(p.constData()); };
    KisTiledDataManager dm(bpp, bytes(blank));
    auto batch = mode ? dm.beginIteratorWriteScope() : nullptr;
    QVERIFY(!mode || batch);
    auto tile = dm.getTile(0, 0, true); QVERIFY(tile);
    QCOMPARE(dm.getTile(0, 0, true), tile);
    QVERIFY(tile->lockForRead());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(tile->data()), bpp), blank);
    tile->unlockForRead();
    if (mode == 2) {
        QVERIFY(tile->lockForWrite()); tile->data()[0] = 0x57; tile->unlockForWrite();
        QCOMPARE(dm.getTile(0, 0, true), tile); // Reacquisition preserves private bytes.
    }
    if (batch) { QVERIFY(batch->finish()); batch.reset(); }
    auto selected = dm.m_nativeResult->capture();
    QVERIFY(selected);
    QVERIFY(!selected.versionAt(0, 0).isDefaultPixel());
    selected = {};
    QCOMPARE(dm.extent(), QRect(0, 0, 64, 64));
    KisTiledDataManager clone(dm);
    dm.setDefaultPixel(bytes(other));
    QByteArray expected = blank, actual(bpp, 0);
    if (mode == 2) expected[0] = char(0x57);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1); QCOMPARE(actual, expected);
    clone.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1); QCOMPARE(actual, expected);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 63, 63, 1, 1); QCOMPARE(actual, blank);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 128, 0, 1, 1); QCOMPARE(actual, other);
    tile = {}; dm.clear();
    QVERIFY(dm.extent().isEmpty());
    QCOMPARE(KisNativeResultTestAccess::state(*dm.m_nativeResult).activeResults, quint64(0));

}

void KisTiledDataManagerTest::testPageStoreKnownWriterRefusal_data()
{
    QTest::addColumn<int>("mode");
    QTest::newRow("standalone") << 0;
    QTest::newRow("original-managed-frame") << 1;
    QTest::newRow("real-legacy-scope") << 2;
}

void KisTiledDataManagerTest::testPageStoreKnownWriterRefusal()
{
const quint8 blank=0x13;QString error;KisTiledDataManager dm(1,&blank);
    auto &protocol=*dm.m_nativeResult;auto &bridge=KisNativeResultTestAccess::tiles(protocol);

    QFETCH(int,mode);auto holder=protocol.prepareProducer(QRect(64,0,64,64));QVERIFY(holder);
    QCOMPARE(dm.m_hashTable->numTiles(),0);bool refused=false;
    const auto probe=[&]{
        auto read=bridge.acquireTile(1,0,KisTilePageStoreBridge::Access::ReadCache);
        const bool oldReadable=read&&read->tileData()&&read->tileData()->data()[0]==blank;
        read.reset();const bool rawRefused=!bridge.acquireTile(1,0,KisTilePageStoreBridge::Access::Write);
        KisStrokeJobFailureContext failure(true);auto tile=dm.getTile(1,0,true);
        refused=oldReadable&&rawRefused&&!tile&&failure.failed();return true;
    };
    auto scope=mode==2?dm.beginIteratorWriteScope():nullptr;QVERIFY(mode!=2||scope);
    if(mode==1){QVERIFY(protocol.pixelOperation({QRect(0,0,1,1)},[&](KisPixelWriteCursor *){return probe();},{}));}else probe();
    QVERIFY(refused);QCOMPARE(dm.m_hashTable->numTiles(),0);
    if(scope){QVERIFY(scope->finish());scope.reset();}protocol.cancel(holder);
    auto tile=dm.getTile(1,0,true);QVERIFY(tile&&tile->lockForRead());QCOMPARE(tile->data()[0],blank);tile->unlockForRead();
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
}

void KisTiledDataManagerTest::testPageStoreWritablePresenceRefusal()
{
const quint8 blank=0x13;QString error;KisTiledDataManager dm(1,&blank);
    auto &protocol=*dm.m_nativeResult;auto &bridge=KisNativeResultTestAccess::tiles(protocol);

    const quint8 color=0x57;auto prepared=protocol.prepareWrite(&color,QRect(0,0,1,1),1);QVERIFY(prepared);
    const auto epoch=KisNativeResultTestAccess::state(protocol).acceptedEpoch;
    {KisStrokeJobFailureContext failure(true);QVERIFY(!dm.getTile(0,0,true));QVERIFY(failure.failed());}
    QCOMPARE(dm.m_hashTable->numTiles(),0);QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch,epoch);
    QVERIFY(protocol.accept(prepared));auto tile=dm.getTile(0,0,true);QVERIFY(tile&&tile->lockForRead());QCOMPARE(tile->data()[0],color);tile->unlockForRead();
    auto scope=dm.beginIteratorWriteScope();QVERIFY(scope);auto lease=bridge.acquireTile(1,0,KisTilePageStoreBridge::Access::Write);QVERIFY(lease);
    lease->tileData()->data()[0]=color;lease->markDirty();QVERIFY(lease->finish());lease.reset();
    {KisStrokeJobFailureContext failure(true);QVERIFY(!dm.getTile(0x7fff,0,true));QVERIFY(failure.failed());}
    QVERIFY(scope->finish());scope.reset();quint8 value=0;dm.readBytes(&value,64,0,1,1);QCOMPARE(value,color);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
}

void KisTiledDataManagerTest::testPageStoreCancelledIteratorAdapters_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("existing"); QTest::addColumn<bool>("write");
    QTest::addColumn<int>("pressure");
    for (int bpp : {1, 4, 8, 16}) for (bool existing : {false, true}) for (bool write : {false, true})
        for (int pressure : {0, 1, 2}) {
            if (pressure && !write) continue;
            QTest::newRow(qPrintable(QString("%1-existing%2-write%3-pressure%4").arg(bpp).arg(existing).arg(write).arg(pressure)))
                << bpp << existing << write << pressure;
        }
}

void KisTiledDataManagerTest::testPageStoreCancelledIteratorAdapters()
{
    QFETCH(int, bpp); QFETCH(bool, existing); QFETCH(bool, write); QFETCH(int, pressure);
    const QByteArray blank(bpp, char(0x13)), color(bpp, char(0x57)), other(bpp, char(0x29));
    auto bytes = [](const QByteArray &p) { return reinterpret_cast<const quint8 *>(p.constData()); };
    KisTiledDataManager dm(bpp, bytes(blank));
    if (existing) dm.writeBytes(bytes(color), 0, 0, 1, 1);
    const QRect beforeExtent = dm.extent();
    auto scope = dm.beginIteratorWriteScope(); QVERIFY(scope);
    auto tile = dm.getTile(0, 0, true); QVERIFY(tile);
    if (write) {
        QVERIFY(tile->lockForWrite());
        std::memset(tile->data(), 0x71, size_t(bpp));
        tile->unlockForWrite();
    }
    auto &protocol=*dm.m_nativeResult;auto &bridge=KisNativeResultTestAccess::tiles(protocol);
    bool accepted=false;
    if (pressure) {
        kisDrainPageStoreReclamation();
        auto process = kisAcquirePageStoreBootstrapBudget(); QVERIFY(process);
        const auto used = [&] {
            const auto metadata = process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
            return metadata.live.cpuRam + metadata.reserved.cpuRam;
        };
        size_t count = 0;
        void *filler = nullptr;
        const auto free = qScopeGuard([&] { if (filler) freeTestProcessStoragePressure(filler, count, 1); });
        const auto fill = [&] {
            count = size_t(kisPageProcessStorageLimit() - used());
            filler = allocateTestProcessStoragePressure(count, 1);
        };
        if (pressure == 1) {
            fill(); QCOMPARE(used(), kisPageProcessStorageLimit());
            TileLease cache;
            auto refused = bridge.acquireTile(0, 0, KisTilePageStoreBridge::Access::Write, &cache);
            QVERIFY(!refused); QVERIFY(!cache);
            freeTestProcessStoragePressure(std::exchange(filler, nullptr), count, 1);
            QVERIFY(!scope->finish());
        } else {
            KisPageStoreDiagnosticRecorder recorder(true);
            bool injected = false;
            recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
                if (phase == KisPageStoreDiagnosticPhase::NativeAcceptPrepared && !injected) {
                    injected = true; fill();
                }
            });
            accepted = scope->finish();
            recorder.setPhaseObserver({});
            if (filler) freeTestProcessStoragePressure(std::exchange(filler, nullptr), count, 1);
            QVERIFY(injected); QVERIFY(accepted);
        }
    }
    scope.reset(); // Abandonment or real refusal cancels the original private delta.
    // Under hard pressure the original scope may transfer refused cleanup
    // to its existing orphan task. Require automatic progress after relief,
    // before any retry operation can accidentally drive that cleanup for it.
    if (pressure)
        QTRY_COMPARE_WITH_TIMEOUT(KisNativeResultTestAccess::state(protocol).activeResults, quint64(0), 5000);
    const QRect cancelledExtent = dm.extent();
    QByteArray actual(bpp, 0); dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1);
    const QByteArray cancelledPixel = actual;
    auto retry = dm.getTile(0, 0, true); QVERIFY(retry);
    QVERIFY(retry->lockForRead());
    const QByteArray retryPixel(reinterpret_cast<const char *>(retry->data()), bpp);
    retry->unlockForRead();
    const auto version=protocol.capture().versionAt(0,0);
    const bool retryPresent = !version.isDefaultPixel();
    dm.setDefaultPixel(bytes(other));
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1);
    qInfo() << "cancel/retry" << cancelledExtent << cancelledPixel.toHex() << retryPresent << actual.toHex()
            << "tile" << retryPixel.toHex();
    QCOMPARE(cancelledExtent, accepted ? QRect(0,0,64,64) : beforeExtent);
    QCOMPARE(cancelledPixel, accepted ? QByteArray(bpp,char(0x71)) : existing ? color : blank);
    QCOMPARE(retryPixel, accepted ? QByteArray(bpp,char(0x71)) : existing ? color : blank);
    QVERIFY(retryPresent);
    QCOMPARE(actual, accepted ? QByteArray(bpp,char(0x71)) : existing ? color : blank);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults, quint64(0));

}

void KisTiledDataManagerTest::testPageStoreOwnedCancellationUnderPressure_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("ending");
    for (int bpp : {1, 4, 8, 16}) for (int ending : {0, 1, 2, 3})
        QTest::newRow(qPrintable(QString("bpp%1-ending%2").arg(bpp).arg(ending))) << bpp << ending;
}

void KisTiledDataManagerTest::testPageStoreOwnedCancellationUnderPressure()
{
    QFETCH(int, bpp); QFETCH(int, ending);
    const bool iterator = ending != 0;
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31));
    auto bytes = [](const QByteArray &p) { return reinterpret_cast<const quint8 *>(p.constData()); };
    KisTiledDataManager dm(bpp, bytes(blank));
    dm.writeBytes(bytes(initial), 0, 0, 1, 1);
    auto &protocol=*dm.m_nativeResult;
    bool accepted=false;
    auto scope = iterator ? dm.beginIteratorWriteScope() : nullptr;
    QVERIFY(!iterator || scope);
    auto tile = dm.getTile(0, 0, true); QVERIFY(tile && tile->lockForWrite());
    std::memset(tile->data(), 0x71, size_t(bpp));
    if (iterator) tile->unlockForWrite();
    kisDrainPageStoreReclamation();
    auto process = kisAcquirePageStoreBootstrapBudget(); QVERIFY(process);
    size_t count = 0; void *filler = nullptr;
    const auto fill = [&] {
        const auto usage = process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        count = size_t(kisPageProcessStorageLimit() - usage.live.cpuRam - usage.reserved.cpuRam);
        filler = allocateTestProcessStoragePressure(count, 1);
    };
    const auto free = qScopeGuard([&] { if (filler) freeTestProcessStoragePressure(filler, count, 1); });
    if (ending == 2) {
        bool injected = false;
        KisPageStoreDiagnosticRecorder recorder(true);
        recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
            if (phase == KisPageStoreDiagnosticPhase::NativeAcceptPrepared && !injected) {
                injected = true; fill();
            }
        });
        accepted = scope->finish();
        recorder.setPhaseObserver({});
        QVERIFY(injected); QVERIFY(accepted);
        scope.reset();
    } else {
        fill();
        const auto full = process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        QCOMPARE(full.live.cpuRam + full.reserved.cpuRam, kisPageProcessStorageLimit());
        KisStrokeJobFailureContext failure(true);
        if (ending == 3) accepted=scope->finish(); // Keep the finished facade.
        else if (iterator) scope.reset();
        else {tile->unlockForWrite();accepted=!failure.failed();}
    }
    freeTestProcessStoragePressure(std::exchange(filler,nullptr),count,1);
    QTRY_COMPARE_WITH_TIMEOUT(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0),5000);
    auto next = dm.beginIteratorWriteScope(); QVERIFY(next);
    auto again = dm.getTile(0, 0, true); QVERIFY(again && again->lockForWrite());
    again->unlockForWrite(); next.reset();
    QByteArray actual(bpp, 0); dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1);
    QCOMPARE(actual, accepted?QByteArray(bpp,char(0x71)):initial);
}

void KisTiledDataManagerTest::testPageStoreFinishedFacadeWithNativeLease()
{
const quint8 blank=0x13;QString error;KisTiledDataManager dm(1,&blank);
    auto &protocol=*dm.m_nativeResult;auto &bridge=KisNativeResultTestAccess::tiles(protocol);

    auto scope=dm.beginIteratorWriteScope();QVERIFY(scope);
    auto lease=bridge.acquireTile(0,0,KisTilePageStoreBridge::Access::Write);QVERIFY(lease);
    lease->tileData()->data()[0]=0x71;lease->markDirty();
    QVERIFY(scope->finish());QVERIFY(!scope->isActive()); // Original borrow owns deferred normal completion.
    QVERIFY(!protocol.prepareProducer(QRect(0,0,64,64)));
    QVERIFY(lease->finish());lease.reset();
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
    auto next=dm.beginIteratorWriteScope();QVERIFY(next);
    auto retry=bridge.acquireTile(0,0,KisTilePageStoreBridge::Access::Write);QVERIFY(retry);QCOMPARE(retry->tileData()->data()[0],quint8(0x71));
    QVERIFY(retry->finish());retry.reset();QVERIFY(next->finish());
}

void KisTiledDataManagerTest::testPageStoreSurfaceOnlyOwnerRefusal()
{
const quint8 blank=0x13;QString error;KisTiledDataManager dm(1,&blank);
    auto &protocol=*dm.m_nativeResult;auto &bridge=KisNativeResultTestAccess::tiles(protocol);

    QVERIFY(!protocol.setDefaultPixel(QByteArray(2,char(0x31))));QCOMPARE(dm.defaultPixel()[0],blank);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
    auto blocker=protocol.prepareProducer(QRect(0,0,1,1));QVERIFY(blocker);
    QVERIFY(!protocol.setDefaultPixel(QByteArray(1,char(0x31))));protocol.cancel(blocker);QCOMPARE(dm.defaultPixel()[0],blank);
    kisDrainPageStoreReclamation();auto process=kisAcquirePageStoreBootstrapBudget();QVERIFY(process);
    size_t count=0;void *filler=nullptr;const auto free=qScopeGuard([&]{if(filler)freeTestProcessStoragePressure(filler,count,1);});
    bool visited=false;KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](auto phase){if(visited||phase!=KisPageStoreDiagnosticPhase::NativeAcceptPrepared)return;
        visited=true;const auto usage=process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        count=size_t(kisPageProcessStorageLimit()-usage.live.cpuRam-usage.reserved.cpuRam);filler=allocateTestProcessStoragePressure(count,1);});
    QVERIFY(protocol.setDefaultPixel(QByteArray(1,char(0x31))));recorder.setPhaseObserver({});QVERIFY(visited&&filler);
    freeTestProcessStoragePressure(std::exchange(filler,nullptr),count,1);
    QCOMPARE(dm.defaultPixel()[0],quint8(0x31));QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
}

void KisTiledDataManagerTest::testPageStoreFinalRawUnlockAllowsAdapter_data()
{
    QTest::addColumn<bool>("readLast"); QTest::addColumn<bool>("foreignUnlock"); QTest::addColumn<bool>("abandonScope");
    for (bool readLast : {false, true}) for (bool foreignUnlock : {false, true})
        for (bool abandonScope : {false, true})
            QTest::newRow(qPrintable(QString("read%1-foreign%2-cancel%3").arg(readLast).arg(foreignUnlock).arg(abandonScope)))
                << readLast << foreignUnlock << abandonScope;
}

void KisTiledDataManagerTest::testPageStoreFinalRawUnlockAllowsAdapter()
{
    QFETCH(bool, readLast); QFETCH(bool, foreignUnlock); QFETCH(bool, abandonScope);
    const quint8 blank = 0x13, initial = 0x31, color = 0x57;
    KisTiledDataManager dm(1, &blank);
    dm.writeBytes(&initial, 0, 0, 1, 1);
    auto &protocol=*dm.m_nativeResult;
    auto scope = abandonScope ? dm.beginIteratorWriteScope() : nullptr;
    QVERIFY(!abandonScope || scope);
    auto tile = dm.getTile(0, 0, true); QVERIFY(tile && tile->lockForWrite());
    tile->data()[0] = 0x71;
    bool secondSucceeded = false;
    std::thread second([&] {
        secondSucceeded = dm.writePageStoreOperation({QRect(64, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(64, 0);
            if (!cursor->rawData()) return false;
            cursor->rawData()[0] = color;
            return true;
        }) == KisPageStoreWriteOperationResult::Succeeded;
    });
    second.join();
    if (readLast) QVERIFY(tile->lockForRead());
    if (readLast) tile->unlockForWrite();
    scope.reset(); // The original last raw holder must deliver cancellation.
    bool observed = false, adapterEntered = false;
    QSemaphore entered;
    std::thread adapter;
    const auto unlock = [&] {
        KisPageStoreDiagnosticRecorder recorder(true);
        recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
            const auto boundary = abandonScope ? KisPageStoreDiagnosticPhase::NativeCancelPrepared
                                               : KisPageStoreDiagnosticPhase::NativeAcceptPrepared;
            if (phase != boundary || observed) return;
            observed = true;
            adapter = std::thread([&] { tile->invalidatePageStoreReadCache(); entered.release(); });
            adapterEntered = entered.tryAcquire(1, 1000);
        });
        if (readLast) tile->unlockForRead(); else tile->unlockForWrite();
        recorder.setPhaseObserver({});
    };
    if (foreignUnlock) { std::thread last(unlock); last.join(); } else unlock();
    if (adapter.joinable()) adapter.join();
    QVERIFY(secondSucceeded); QVERIFY(observed); QVERIFY(adapterEntered);
    quint8 actual[65] = {};
    dm.readBytes(actual, 0, 0, 65, 1);
    QCOMPARE(actual[0], abandonScope ? initial : quint8(0x71)); QCOMPARE(actual[64], color);
    auto retry = dm.getTile(0, 0, true); QVERIFY(retry && retry->lockForRead());
    QCOMPARE(retry->data()[0], abandonScope ? initial : quint8(0x71)); retry->unlockForRead();
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults, quint64(0));

    dm.writeBytes(&color, 128, 0, 1, 1);
    dm.readBytes(actual, 128, 0, 1, 1); QCOMPARE(actual[0], color);
}

void KisTiledDataManagerTest::testPageStoreFinalAdapterCannotRevive()
{
const quint8 blank=0x13;QString error;KisTiledDataManager dm(1,&blank);
    auto &protocol=*dm.m_nativeResult;auto &bridge=KisNativeResultTestAccess::tiles(protocol);

    const quint8 initial=0x31;dm.writeBytes(&initial,0,0,1,1);auto scope=dm.beginIteratorWriteScope();QVERIFY(scope);
    auto tile=dm.getTile(0,0,true);QVERIFY(tile&&tile->lockForWrite());tile->data()[0]=0x71;scope.reset();
    QSemaphore entered,resume;bool observed=false;
    std::thread last([&]{KisPageStoreDiagnosticRecorder recorder(true);recorder.setPhaseObserver([&](auto phase){
        if(phase!=KisPageStoreDiagnosticPhase::NativeCancelPrepared||observed)return;observed=true;entered.release();resume.acquire();});tile->unlockForWrite();});
    const auto join=qScopeGuard([&]{resume.release();if(last.joinable())last.join();});QVERIFY(entered.tryAcquire(1,5000));
    QVERIFY(!protocol.prepareProducer(QRect(0,0,1,1))); // cancelled original still owns its exact claim
    auto next=dm.beginIteratorWriteScope();QVERIFY(next); // detached registry does not globally ban unrelated work
    auto other=dm.getTile(1,0,true);QVERIFY(other&&other->lockForWrite());other->data()[0]=0x57;other->unlockForWrite();QVERIFY(next->finish());next.reset();
    resume.release();last.join();QVERIFY(observed);quint8 values[65]={};dm.readBytes(values,0,0,65,1);
    QCOMPARE(values[0],initial);QCOMPARE(values[64],quint8(0x57));
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
    next=dm.beginIteratorWriteScope();QVERIFY(next);auto retry=dm.getTile(0,0,true);QVERIFY(retry&&retry->lockForWrite());retry->data()[0]=0x57;
    retry->unlockForWrite();QVERIFY(next->finish());next.reset();dm.readBytes(values,0,0,1,1);QCOMPARE(values[0],quint8(0x57));
}

void KisTiledDataManagerTest::testPageStoreUnscopedLeaseCompletion_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("history"); QTest::addColumn<bool>("dirty");
    for (int bpp : {1, 4, 8, 16}) for (bool history : {false, true}) for (bool dirty : {false, true})
        QTest::newRow(qPrintable(QString("bpp%1-history%2-dirty%3").arg(bpp).arg(history).arg(dirty)))
            << bpp << history << dirty;
}

void KisTiledDataManagerTest::testPageStoreUnscopedLeaseCompletion()
{
    QFETCH(int, bpp); QFETCH(bool, history); QFETCH(bool, dirty);
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31)), color(bpp, char(0x57));
    auto bytes = [](const QByteArray &p) { return reinterpret_cast<const quint8 *>(p.constData()); };
    KisTiledDataManager dm(bpp, bytes(blank));
    dm.writeBytes(bytes(initial), 0, 0, 1, 1);
    auto &protocol=*dm.m_nativeResult;auto &bridge=KisNativeResultTestAccess::tiles(protocol);
    auto before = protocol.capture();
    auto exact = before.readPage(0,0); QVERIFY(exact.data());
    auto memento = history ? dm.getMemento() : KisMementoSP{};
    auto lease = bridge.acquireTile(0, 0, KisTilePageStoreBridge::Access::Write); QVERIFY(lease);
    std::memcpy(lease->tileData()->data(), color.constData(), size_t(bpp));
    if (dirty) lease->markDirty();
    // A raw lease is not a registered iterator scope. A separate operation
    // can complete into the original history/anonymous transaction alongside it.
    QCOMPARE(dm.writePageStoreOperation({QRect(64, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
        cursor->moveTo(64, 0); if (!cursor->rawData()) return false;
        std::memcpy(cursor->rawData(), color.constData(), size_t(bpp)); return true;
    }), KisPageStoreWriteOperationResult::Succeeded);
    QVERIFY(lease->finish());
    // Direct finish remains synchronous even while the finished lease object lives.
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults, quint64(0));

    QByteArray actual(bpp, 0);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1);
    QCOMPARE(actual, dirty ? color : initial);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 64, 0, 1, 1); QCOMPARE(actual, color);
    QVERIFY(lease->finish()); lease.reset();
    if (history) {
        QVERIFY(dm.tryCommit());
        dm.rollback(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1); QCOMPARE(actual, initial);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 64, 0, 1, 1); QCOMPARE(actual, blank);
        dm.rollforward(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1); QCOMPARE(actual, dirty ? color : initial);
    }
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(exact.data()), bpp), initial);
}

void KisTiledDataManagerTest::testPageStoreDisjointOwnerPublication_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("warm"); QTest::addColumn<int>("ending");
    for (int bpp : {1, 4, 8, 16}) for (bool warm : {false, true}) for (int ending : {0, 1, 2})
        QTest::newRow(qPrintable(QString("bpp%1-warm%2-ending%3").arg(bpp).arg(warm).arg(ending)))
            << bpp << warm << ending;
}

void KisTiledDataManagerTest::testPageStoreDisjointOwnerPublication()
{
    QFETCH(int, bpp); QFETCH(bool, warm); QFETCH(int, ending);
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31)), first(bpp, char(0x57)), second(bpp, char(0x71));
    auto bytes = [](const QByteArray &p) { return reinterpret_cast<const quint8 *>(p.constData()); };
    KisTiledDataManager dm(bpp, bytes(blank));
    if (warm) dm.writeBytes(bytes(initial), 0, 0, 1, 1);
    auto &protocol=*dm.m_nativeResult;
    auto scope = dm.beginIteratorWriteScope(); QVERIFY(scope);
    auto tile = dm.getTile(0, 0, true); QVERIFY(tile && tile->lockForWrite());
    std::memcpy(tile->data(), first.constData(), size_t(bpp)); tile->unlockForWrite();
    bool siblingAccepted = false;
    std::thread sibling([&] {
        siblingAccepted = dm.writePageStoreOperation({QRect(64, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(64, 0); if (!cursor->rawData()) return false;
            std::memcpy(cursor->rawData(), second.constData(), size_t(bpp)); return true;
        }) == KisPageStoreWriteOperationResult::Succeeded;
    });
    sibling.join();
    bool finished = false, injected = false;
    if (ending == 0) finished = scope->finish();
    else if (ending == 2) {
        kisDrainPageStoreReclamation();
        auto process = kisAcquirePageStoreBootstrapBudget(); QVERIFY(process);
        size_t count = 0; void *filler = nullptr;
        const auto free = qScopeGuard([&] { if (filler) freeTestProcessStoragePressure(filler, count, 1); });
        KisPageStoreDiagnosticRecorder recorder(true);
        recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
            if (phase != KisPageStoreDiagnosticPhase::NativeAcceptPrepared || injected) return;
            injected = true;
            const auto usage = process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
            count = size_t(kisPageProcessStorageLimit() - usage.live.cpuRam - usage.reserved.cpuRam);
            filler = allocateTestProcessStoragePressure(count, 1);
        });
        finished = scope->finish();
        recorder.setPhaseObserver({});
    }
    if (ending != 1) {
        // The finished facade can remain alive without retaining the original
        // page admission or blocking the next operation on that page.
        if (ending == 2) {
            // Existing paid capacity may complete acceptance at the hard
            // limit. If preparation refuses instead, relief must let the
            // original orphan finish without a new operation or facade exit.
            QTRY_COMPARE_WITH_TIMEOUT(KisNativeResultTestAccess::state(protocol).activeResults, quint64(0), 5000);
        } else {
            QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults, quint64(0));
        }
        auto next = dm.beginIteratorWriteScope(); QVERIFY(next);
        auto again = dm.getTile(0, 0, true); QVERIFY(again && again->lockForWrite());
        again->unlockForWrite(); next.reset();
    }
    scope.reset();
    QByteArray a(bpp, 0), b(bpp, 0);
    dm.readBytes(reinterpret_cast<quint8 *>(a.data()), 0, 0, 1, 1);
    dm.readBytes(reinterpret_cast<quint8 *>(b.data()), 64, 0, 1, 1);
    qInfo() << "disjoint owners" << siblingAccepted << finished << injected << a.toHex() << b.toHex();
    QVERIFY(siblingAccepted);
    QCOMPARE(finished, ending != 1);
    QCOMPARE(injected, ending == 2);
    QCOMPARE(a, finished ? first : warm ? initial : blank);
    // No shared history was requested. An already successful independent
    // operation cannot be rolled back by this iterator's later refusal.
    QCOMPARE(b, second);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults, quint64(0));
}

void KisTiledDataManagerTest::testPageStoreDerivedExtentRebase_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("interior-add-after-shrink") << 0;
    QTest::newRow("shrink-after-exterior-add") << 1;
    QTest::newRow("two-disjoint-removals") << 2;
    QTest::newRow("default-metadata-conflict") << 3;
}

void KisTiledDataManagerTest::testPageStoreDerivedExtentRebase()
{
QFETCH(int,scenario);const quint8 blank=0x13;KisTiledDataManager dm(1,&blank);auto &p=*dm.m_nativeResult;
    QVERIFY(p.fill(QRect(0,0,1,1),QByteArray(1,char(0x31))));QVERIFY(p.fill(QRect(128,0,1,1),QByteArray(1,char(0x31))));
    auto before=p.capture();QVERIFY(before);bool visited=false,sibling=false;KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](auto phase){if(visited||phase!=KisPageStoreDiagnosticPhase::NativeAcceptPrepared)return;visited=true;
        if(scenario==0)sibling=p.remove(QRect(128,0,64,64));
        else if(scenario==1)sibling=p.fill(QRect(256,0,1,1),QByteArray(1,char(0x57)));
        else if(scenario==2)sibling=p.remove(QRect(0,0,64,64));
        else sibling=p.setDefaultPixel(QByteArray(1,char(0x71)));
    });
    const bool accepted=scenario==0?p.fill(QRect(64,0,1,1),QByteArray(1,char(0x57))):scenario==3?p.setDefaultPixel(QByteArray(1,char(0x57))):p.remove(QRect(128,0,64,64));
    recorder.setPhaseObserver({});QVERIFY(visited&&accepted);QCOMPARE(sibling,scenario!=3);
    QCOMPARE(dm.extent(),scenario==0?QRect(0,0,128,64):scenario==1?QRect(0,0,320,64):scenario==2?QRect():QRect(0,0,192,64));
    QCOMPARE(dm.defaultPixel()[0],quint8(scenario==3?0x57:blank));
    for(int x:{0,2}){auto old=before.readPage(x,0);QVERIFY(old.data());QCOMPARE(old.data()[0],quint8(0x31));}
}

void KisTiledDataManagerTest::testPageStoreUnpublishedReadRefusal_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("warm"); QTest::addColumn<bool>("packed");
    for (int bpp : {1, 4, 8, 16}) for (bool warm : {false, true}) for (bool packed : {false, true})
        QTest::newRow(qPrintable(QString("bpp%1-warm%2-packed%3").arg(bpp).arg(warm).arg(packed)))
            << bpp << warm << packed;
}

void KisTiledDataManagerTest::testPageStoreUnpublishedReadRefusal()
{
    QFETCH(int, bpp); QFETCH(bool, warm); QFETCH(bool, packed);
    const QByteArray blank(bpp, char(0x13)), old(bpp, char(0x31)), value(bpp, char(0x57));
    auto bytes = [](const QByteArray &p) { return reinterpret_cast<const quint8 *>(p.constData()); };
    KisTiledDataManager dm(bpp, bytes(blank));
    if (warm) dm.writeBytes(bytes(old), 0, 0, 1, 1);
    auto &protocol=*dm.m_nativeResult;
    auto before = protocol.capture(); QVERIFY(before);
    const QRect extent = dm.extent();
    kisDrainPageStoreReclamation();
    auto process = kisAcquirePageStoreBootstrapBudget(); QVERIFY(process);
    size_t count = 0; void *filler = nullptr;
    const auto free = qScopeGuard([&] { if (filler) freeTestProcessStoragePressure(filler, count, 1); });
    QByteArray during;
    bool injected = false;
    KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase == KisPageStoreDiagnosticPhase::NativeCancelPrepared) {
            // Publication preparation has already refused. Release injected
            // pressure before admitting a NEW read capability: its storage
            // must not depend on how much commit scratch was just freed.
            // The transaction is still unpublished and rollback has not run.
            if (filler) freeTestProcessStoragePressure(std::exchange(filler, nullptr), count, 1);
            std::thread reader([&] {
                auto tile = dm.getTile(0, 0, false);
                if (!tile || !tile->lockForRead()) return;
                during = QByteArray(reinterpret_cast<const char *>(tile->data()), bpp);
                tile->unlockForRead();
            });
            reader.join();
        }
        if (phase != KisPageStoreDiagnosticPhase::PackedWritePrepare || injected) return;
        injected = true;
        const auto usage = process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        count = size_t(kisPageProcessStorageLimit() - usage.live.cpuRam - usage.reserved.cpuRam);
        filler = allocateTestProcessStoragePressure(count, 1);
    });
    bool rejected = false;
    if (packed) {
        KisStrokeJobFailureContext failure(true);
        dm.writeBytes(bytes(value), 0, 0, 1, 1); rejected = failure.failed();
    } else {
        rejected = dm.writePageStoreOperation({QRect(0, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(0, 0); if (!cursor->rawData()) return false;
            std::memcpy(cursor->rawData(), value.constData(), size_t(bpp)); return true;
        }) == KisPageStoreWriteOperationResult::Failed;
    }
    recorder.setPhaseObserver({});
    if (filler) freeTestProcessStoragePressure(std::exchange(filler, nullptr), count, 1);
    QVERIFY(injected && rejected);
    const QByteArray expected = warm ? old : blank;
    QCOMPARE(during, expected);
    QByteArray after(bpp, 0); dm.readBytes(reinterpret_cast<quint8 *>(after.data()), 0, 0, 1, 1);
    QCOMPARE(after, expected); QCOMPARE(dm.extent(), extent);
    auto exact = before.readPage(0,0); QVERIFY(exact.data());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(exact.data()), bpp), expected);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults, quint64(0));
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

void KisTiledDataManagerTest::testLegacyClearFailure()
{
    DefaultBudgetScope budget;
    const QByteArray blank(4, char(0x13)), color(4, char(0x57));
    auto bytes = [](const QByteArray &p) { return reinterpret_cast<const quint8 *>(p.constData()); };
    KisTiledDataManager dm(4, bytes(blank), KisTiledDataManager::SelectionAuthority::ExternalProtocol);
    kisDrainPageStoreReclamation();
    budget.saturate(4, bytes(blank));
    {
        KisStrokeJobFailureContext failure(true);
        dm.clear(QRect(0, 0, 64, 64), bytes(color));
        QVERIFY(failure.failed()); QVERIFY(dm.extent().isEmpty());
    }
    budget.release();
    {
        KisStrokeJobFailureContext failure(true);
        dm.clear(QRect(0, 0, 64, 64), bytes(color));
        QVERIFY(!failure.failed());
    }
    QByteArray actual(4, 0); dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1);
    QCOMPARE(actual, color); QCOMPARE(dm.extent(), QRect(0, 0, 64, 64));
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
    budget.saturate(bpp, bytes(oldPixel));
    QString error;
    QVERIFY2(manager.tryAbort(memento, &error), qPrintable(error));
    QVERIFY(!manager.hasCurrentMemento());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(manager.defaultPixel()), bpp), oldPixel);
    QByteArray selected(bpp, 0);
    manager.readBytes(reinterpret_cast<quint8 *>(selected.data()), 0, 0, 1, 1);
    QCOMPARE(selected, oldPixel);
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
    QVERIFY(manager.m_nativeResult);
    auto agrees = [&](const QByteArray &expected) {
        auto view = manager.m_nativeResult->capture();
        QByteArray selected(bpp, 0);
        return QByteArray(reinterpret_cast<const char *>(manager.defaultPixel()), bpp) == expected
            && view.read(reinterpret_cast<quint8 *>(selected.data()), QRect(65536, 65536, 1, 1), bpp)
            && selected == expected;
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
    QVERIFY(agrees(oldPixel)); // restore consumes the already retained default backing
    budget.release();
    manager.rollback(memento);
    QVERIFY(agrees(oldPixel));
    QVERIFY(reads(manager, oldPixel));

    budget.saturate(bpp, oldBytes);
    manager.rollforward(memento);
    QVERIFY(agrees(newPixel));
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

void KisTiledDataManagerTest::testPageStoreCloneActiveWriteRefusal_data()
{
    QTest::addColumn<int>("bpp");
    QTest::addColumn<bool>("exposed");
    for (int bpp : {1, 4, 8, 16}) for (bool exposed : {false, true})
        QTest::newRow(qPrintable(QString("bpp%1-exposed%2").arg(bpp).arg(exposed)))
            << bpp << exposed;
}

void KisTiledDataManagerTest::testPageStoreCloneActiveWriteRefusal()
{
    QFETCH(int, bpp);
    QFETCH(bool, exposed);
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31));
    KisTiledDataManager source(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    source.writeBytes(reinterpret_cast<const quint8 *>(initial.constData()), 0, 0, 1, 1);
    auto *backend = source.m_nativeResult.get();
    QVERIFY(backend && true);
    const auto before = backend->capture();
    auto exact = before.readPage(0, 0);
    QVERIFY(bool(exact.data()));
    auto tile = source.getTile(0, 0, true);
    QVERIFY(tile && tile->lockForWrite());
    bool locked = true;
    const auto unlock = qScopeGuard([&] { if (locked) tile->unlockForWrite(); });
    if (exposed) {
        auto *pixels = tile->tryWriteData();
        QVERIFY(pixels);
        std::memset(pixels, 0x71, size_t(bpp));
    }
    bool refused = false;
    try { KisTiledDataManager clone(source); }
    catch (const std::bad_alloc &) { refused = true; }
    tile->unlockForWrite();
    locked = false;
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(exact.data()), bpp), initial);
    QCOMPARE(KisNativeResultTestAccess::state(*backend).activeResults, quint64(0));
    QCOMPARE(refused, exposed);
    KisTiledDataManager retry(source);
    QByteArray actual(bpp, Qt::Uninitialized);
    retry.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1);
    QCOMPARE(actual, exposed ? QByteArray(bpp, char(0x71)) : initial);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(exact.data()), bpp), initial);
}

void KisTiledDataManagerTest::testPageStoreCloneHistoryBase_data()
{
    QTest::addColumn<int>("bpp");
    for (int bpp : {1, 4, 8, 16}) QTest::newRow(qPrintable(QString::number(bpp))) << bpp;
}

void KisTiledDataManagerTest::testPageStoreCloneHistoryBase()
{
QFETCH(int,bpp);const QByteArray initial(bpp,char(0x31)),value(bpp,char(0x71));
    const auto bytes=[](const QByteArray &b){return reinterpret_cast<const quint8 *>(b.constData());};
    KisTiledDataManager source(bpp,bytes(initial));source.writeBytes(bytes(initial),0,0,1,1);
    auto &protocol=*source.m_nativeResult;auto history=source.getMemento();QVERIFY(history);
    QVERIFY(protocol.write(bytes(value),QRect(0,0,1,1),bpp));
    auto before=protocol.captureOldData();QVERIFY(before);auto old=before.readPage(0,0);QVERIFY(old.data());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(old.data()),bpp),initial);
    KisTiledDataManager clone(source);QByteArray actual(bpp,0);clone.readBytes(reinterpret_cast<quint8 *>(actual.data()),0,0,1,1);QCOMPARE(actual,value);
    QVERIFY(source.tryAbort(history));source.readBytes(reinterpret_cast<quint8 *>(actual.data()),0,0,1,1);QCOMPARE(actual,initial);
    clone.readBytes(reinterpret_cast<quint8 *>(actual.data()),0,0,1,1);QCOMPARE(actual,value);
}

void KisTiledDataManagerTest::testPageStoreCloneFixedView_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("count"); QTest::addColumn<int>("changes");
    for (int bpp : {1, 4, 8, 16}) for (int count : {0, 1, 17, 257}) {
        QVector<int> deltas{0}; if (count) deltas.append(1); if (count > 1) deltas.append(count);
        for (int changes : deltas)
            QTest::newRow(qPrintable(QString("B%1-N%2-D%3").arg(bpp).arg(count).arg(changes))) << bpp << count << changes;
    }
}

void KisTiledDataManagerTest::testPageStoreCloneFixedView()
{
    QFETCH(int, bpp); QFETCH(int, count); QFETCH(int, changes);
    const QByteArray blank(bpp, char(0x13)), paint(bpp, char(0x51)), laterDefault(bpp, char(0x29));
    const auto bytes = [](const QByteArray &value) { return reinterpret_cast<const quint8 *>(value.constData()); };
    KisTiledDataManager source(bpp, bytes(blank));
    if (count) { const QByteArray initial(count * 64 * bpp, char(0x31)); source.writeBytes(bytes(initial), 0, 64, count * 64, 1); }
    const auto history = source.getMemento(); QVERIFY(history);
    for (int i = 0; i < changes; ++i) {
        const int column = i % 3 == 0 ? -1 - i : i;
        source.clear(QRect(column * 64, 64, 64, 64), bytes(i % 3 == 1 ? blank : paint));
    }
    source.setDefaultPixel(bytes(laterDefault));
    auto *backend = source.m_nativeResult.get();
    const auto selected = backend->capture(); QVERIFY(bool(selected));
    const auto selectedExtent = source.extent();
    const auto selectedRegion = source.region();
    const auto selectedTiles = source.m_hashTable->numTiles();
    KisTiledDataManager clone(source);
    QCOMPARE(clone.region(), selectedRegion);
    QCOMPARE(clone.extent(), selectedExtent);
    QCOMPARE(clone.m_hashTable->numTiles(), selectedTiles);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(clone.defaultPixel()), bpp), laterDefault);
    const auto cloned = clone.m_nativeResult->capture(); QVERIFY(bool(cloned));
    for (int column = -count - 1; column <= count; ++column) {
        auto from = selected.readPage(column, 1), to = cloned.readPage(column, 1);
        QVERIFY(from.data() && to.data());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(to.data()), bpp), QByteArray(reinterpret_cast<const char *>(from.data()), bpp));
    }
    QVERIFY(source.tryAbort(history)); source.clear();
    QCOMPARE(clone.region(), selectedRegion);
    const auto implicit = cloned.readPage(count + 1, 1); QVERIFY(implicit.data());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(implicit.data()), bpp), laterDefault);
}

void KisTiledDataManagerTest::testPageStoreClonePendingPresence_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("cold");
    for (int bpp : {1, 4, 8, 16}) for (bool cold : {false, true})
        QTest::newRow(qPrintable(QString("B%1-cold%2").arg(bpp).arg(cold))) << bpp << cold;
}

void KisTiledDataManagerTest::testPageStoreClonePendingPresence()
{
    QFETCH(int, bpp); QFETCH(bool, cold);
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31));
    KisTiledDataManager source(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    source.writeBytes(reinterpret_cast<const quint8 *>(initial.constData()), 0, 0, 1, 1);
    const auto owner = source.getMemento(); QVERIFY(source.beginStrokeMutation(owner));
    const int x = cold ? 64 : 0;
    QCOMPARE(source.writePageStoreOperation({QRect(x, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
        cursor->moveTo(x, 0); if (!cursor->rawData()) return false;
        std::memset(cursor->rawData(), 0x71, size_t(bpp)); return true;
    }, nullptr, owner), KisPageStoreWriteOperationResult::Succeeded);
    KisTiledDataManager clone(source);
    QVERIFY(clone.m_nativeResult);
    QCOMPARE(clone.m_hashTable->numTiles(), 1);
    QCOMPARE(clone.extent(), QRect(0, 0, 64, 64));
    QVERIFY(source.tryAbort(owner));
    QByteArray pixel(bpp, Qt::Uninitialized);
    clone.readBytes(reinterpret_cast<quint8 *>(pixel.data()), 0, 0, 1, 1); QCOMPARE(pixel, initial);
    clone.readBytes(reinterpret_cast<quint8 *>(pixel.data()), 64, 0, 1, 1); QCOMPARE(pixel, blank);
}

void KisTiledDataManagerTest::testPageStoreCloneSnapshotRefusal_data()
{
    testPageStoreCloneHistoryBase_data();
}

void KisTiledDataManagerTest::testPageStoreCloneSnapshotRefusal()
{
    QFETCH(int, bpp);
    const QByteArray initial(bpp, char(0x31));
    KisTiledDataManager source(bpp, reinterpret_cast<const quint8 *>(initial.constData()));
    source.writeBytes(reinterpret_cast<const quint8 *>(initial.constData()), 0, 0, 1, 1);
    auto *protocol = source.m_nativeResult.get(); kisDrainPageStoreReclamation();
    const auto retained = KisNativeResultTestAccess::state(*protocol).logicalViews;
    auto process = kisAcquirePageStoreBootstrapBudget(); QVERIFY(process);
    size_t count = 0; void *filler = nullptr; bool injected = false;
    const auto cleanup = qScopeGuard([&] { if (filler) freeTestProcessStoragePressure(filler, count, 1); });
    KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::NativeCloneCaptured || injected) return;
        injected = true;
        const auto usage = process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        count = size_t(kisPageProcessStorageLimit() - usage.live.cpuRam - usage.reserved.cpuRam);
        filler = allocateTestProcessStoragePressure(count, 1);
    });
    bool refused = false;
    try { KisTiledDataManager clone(source); } catch (const std::bad_alloc &) { refused = true; }
    recorder.setPhaseObserver({});
    QVERIFY(injected && filler);
    freeTestProcessStoragePressure(std::exchange(filler, nullptr), count, 1);
    QVERIFY(refused);
    QTRY_COMPARE_WITH_TIMEOUT(KisNativeResultTestAccess::state(*protocol).logicalViews, retained, 3000);
    KisTiledDataManager retry(source);
    QByteArray pixel(bpp, Qt::Uninitialized);
    retry.readBytes(reinterpret_cast<quint8 *>(pixel.data()), 0, 0, 1, 1); QCOMPARE(pixel, initial);
}

void KisTiledDataManagerTest::testTileCloneRejectedWrite_data()
{
    QTest::addColumn<int>("bpp");
    QTest::addColumn<int>("entry");
    QTest::addColumn<bool>("batch");
    QTest::addColumn<bool>("pressure");
    for (int bpp : {1, 4, 8, 16}) for (int entry = 0; entry < 4; ++entry)
        for (bool batch : {false, true}) for (bool pressure : {false, true})
            QTest::newRow(qPrintable(QString("bpp%1-entry%2-batch%3-pressure%4")
                .arg(bpp).arg(entry).arg(batch).arg(pressure))) << bpp << entry << batch << pressure;
}

void KisTiledDataManagerTest::testTileCloneRejectedWrite()
{
    QFETCH(int, bpp); QFETCH(int, entry); QFETCH(bool, batch); QFETCH(bool, pressure);
    DefaultBudgetScope budget;
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31));
    KisTiledDataManager source(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    source.writeBytes(reinterpret_cast<const quint8 *>(initial.constData()), 0, 0, 1, 1);
    auto &protocol=*source.m_nativeResult;
    const auto before = protocol.capture();
    auto exact = before.readPage(0,0);
    QVERIFY(exact.data());
    auto tile = source.getTile(0, 0, true);
    QVERIFY(tile);
    auto scope = batch ? source.beginIteratorWriteScope() : nullptr;
    QVERIFY(!batch || (scope && scope->isActive()));
    auto held=pressure?KisNativeResultProtocol::Prepared{}:protocol.prepareProducer(QRect(0,0,1,1));
    QVERIFY(pressure||held);const auto releaseForeign=qScopeGuard([&]{protocol.cancel(held);});
    QVERIFY(tile->lockForWrite());
    if (pressure) budget.saturate(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    {
        const auto unlock = qScopeGuard([&] { tile->unlockForWrite(); });
        if (entry == 0) QVERIFY_THROWS_EXCEPTION(std::bad_alloc, KisTile copy(*tile, 2, 3, nullptr));
        if (entry == 1) QVERIFY_THROWS_EXCEPTION(std::bad_alloc, KisTile copy(*tile, nullptr));
        if (entry == 2) QVERIFY_THROWS_EXCEPTION(std::bad_alloc, KisTile copy(*tile));
        if (entry == 3) {
            // The explicit target and unexposed raw intent cannot modify the
            // selected current result. A clone may retain that exact cut.
            KisTiledDataManager clone(source);QByteArray actual(bpp,0);
            clone.readBytes(reinterpret_cast<quint8 *>(actual.data()),0,0,1,1);QCOMPARE(actual,initial);
            QVERIFY(!tile->tryWriteData());
        }
    }
    scope.reset();
    budget.release();
    protocol.cancel(held);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(exact.data()), bpp), initial);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults, quint64(0));
    QVERIFY(!tile->hasPageStoreWriteIntent());
    KisTiledDataManager retry(source);
    QByteArray actual(bpp, Qt::Uninitialized);
    retry.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1);
    QCOMPARE(actual, initial);
}

void KisTiledDataManagerTest::testHashCloneFailureCleanup()
{
    const quint8 pixel = 0x31;
    auto *data = KisTileDataStore::instance()->createDefaultTileData(1, &pixel);
    QVERIFY(data);
    data->ref();
    const auto release = qScopeGuard([&] { data->deref(); });
    const auto check = [&](auto *implementation) {
        using Table = std::remove_pointer_t<decltype(implementation)>;
        Table source(nullptr);
        source.setDefaultTileData(data);
        for (int col : {0, 1, 32, 33, 64}) {
            bool created = false;
            QVERIFY(source.getTileLazy(col, 0, created));
            QVERIFY(created);
        }
        const int users = data->numUsers();
        CloneFaultTile::remaining = 2;
        CloneFaultTile::detached = 0;
        QVERIFY_THROWS_EXCEPTION(std::bad_alloc, Table(source, nullptr));
        QCOMPARE(CloneFaultTile::detached, 2);
        QCOMPARE(data->numUsers(), users);
        CloneFaultTile::remaining = -1;
        {
            Table retry(source, nullptr);
            QCOMPARE(retry.numTiles(), source.numTiles());
            QCOMPARE(data->numUsers(), users + 6);
        }
        QCOMPARE(CloneFaultTile::detached, 7);
        QCOMPARE(data->numUsers(), users);
    };
    check(static_cast<KisTileHashTableTraits<CloneFaultTile> *>(nullptr));
    check(static_cast<KisTileHashTableTraits2<CloneFaultTile> *>(nullptr));
}

void KisTiledDataManagerTest::testCloneCanonicalOrLegacySource_data()
{
    QTest::addColumn<int>("bpp");
    QTest::addColumn<bool>("native");
    for (int bpp : {1, 4, 8, 16}) for (bool native : {false, true})
        QTest::newRow(qPrintable(QString("bpp%1-native%2").arg(bpp).arg(native))) << bpp << native;
}

void KisTiledDataManagerTest::testCloneCanonicalOrLegacySource()
{
    QFETCH(int, bpp); QFETCH(bool, native);
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31));
    KisTiledDataManager source(bpp, reinterpret_cast<const quint8 *>(blank.constData()),
        native ? KisTiledDataManager::SelectionAuthority::NativeResult
               : KisTiledDataManager::SelectionAuthority::ExternalProtocol);
    source.writeBytes(reinterpret_cast<const quint8 *>(initial.constData()), -65, 2, 1, 1);
    KisTiledDataManager clone(source);
    QByteArray actual(bpp, Qt::Uninitialized);
    clone.readBytes(reinterpret_cast<quint8 *>(actual.data()), -65, 2, 1, 1);
    QCOMPARE(actual, initial);
    QCOMPARE(clone.extent(), source.extent());
    QCOMPARE(bool(clone.m_nativeResult), native);
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

void KisTiledDataManagerTest::testPageStoreImportFailure_data()
{
    QTest::addColumn<int>("bpp");
    QTest::addColumn<int>("failure");
    for (int bpp : {1, 4, 8, 16})
        for (int failure = 0; failure < 7; ++failure)
            QTest::newRow(qPrintable(QString("%1-%2").arg(bpp).arg(failure))) << bpp << failure;
}

void KisTiledDataManagerTest::testPageStoreImportFailure()
{
    QFETCH(int, bpp); QFETCH(int, failure);
    const QByteArray blank(bpp, char(0x19)), painted(bpp, char(0x57));
    auto bytes = [](const QByteArray &p) { return reinterpret_cast<const quint8 *>(p.constData()); };
    KisTiledDataManager source(bpp, bytes(blank));
    source.clear(QRect(0, 0, 64, 64), bytes(painted));
    KoStoreFake output; KisFakePaintDeviceWriter writer(&output);
    QVERIFY(source.write(writer)); output.startReading();
    QByteArray data = output.device()->readAll();
    QVERIFY(data.contains("DATA 1\n"));
    data.replace("DATA 1\n", "DATA 2\n");
    const auto badHeader = data.size();
    data += "invalid\n"; // The first real compressed tile still decodes fully.
    if (failure == 1) data.clear();
    if (failure == 2) data = "VERSION 2\nTILEWIDTH 32\n";
    class ImportStream : public QBuffer {
    public:
        std::function<void()> beforeBadHeader;
        qint64 badHeader = -1;
    protected:
        qint64 readLineData(char *data, qint64 length) override
        {
            if (pos() == badHeader && beforeBadHeader) {
                auto callback = std::exchange(beforeBadHeader, {});
                callback();
            }
            return QBuffer::readLineData(data, length);
        }
    } stream;
    stream.setData(data); QVERIFY(stream.open(QIODevice::ReadOnly));
    KisTiledDataManager dm(bpp, bytes(blank));
    dm.clear(QRect(9 * 64, 0, 64, 64), bytes(painted));
    KisTileSP held;
    bool admitted = false, locked = false, exposed = false, allocationException = false;
    const auto unlock = qScopeGuard([&] { if (locked) held->unlockForWrite(); });
    if (failure >= 4) {
        stream.badHeader = badHeader;
        stream.beforeBadHeader = [&] {
            if (failure == 6) {
                allocationException = true;
                throw std::bad_alloc(); // Stream-side failure after a real decoded page.
            }
            held = dm.getTile(0, 0, true);
            if (failure == 5) {
                locked = held && held->lockForWrite();
                exposed = locked && held->tryWriteData();
                return;
            }
            admitted = dm.writePageStoreOperation({QRect(0, 0, 64, 64)},
                [](KisPixelWriteCursor *) { return true; }) == KisPageStoreWriteOperationResult::Succeeded;
        };
    }
    QVERIFY(!dm.read(failure == 0 ? nullptr : &stream));
    QCOMPARE(allocationException, failure == 6);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(dm.defaultPixel()), bpp), blank);
    if (failure == 4) {
        QVERIFY(held); QVERIFY(!admitted);
    }
    QVERIFY(!dm.hasCurrentMemento());
    if (failure == 5) {
        QVERIFY(locked); QVERIFY(exposed);
        // The real raw guard retains the original cancelled batch until its
        // final unlock, without leaving a private undo result to commit.
        held->unlockForWrite(); locked = false;
    }
    kisDrainPageStoreReclamation();
    QCOMPARE(KisNativeResultTestAccess::state(*dm.m_nativeResult).activeResults, quint64(0));
    QCOMPARE(KisNativeResultTestAccess::state(*dm.m_nativeResult).logicalViews, quint64(0));
    QCOMPARE(dm.m_hashTable->numTiles(), 0);
    QVERIFY(dm.extent().isEmpty());
    const auto next = dm.getMemento(); QVERIFY(next);
    QVERIFY(dm.tryAbort(next));
    QVERIFY(!dm.hasCurrentMemento());
}

void KisTiledDataManagerTest::testPageStoreImportRetention_data()
{
    QTest::addColumn<int>("bpp");
    for (int bpp : {1, 4, 8, 16}) QTest::newRow(qPrintable(QString::number(bpp))) << bpp;
}

void KisTiledDataManagerTest::testPageStoreImportRetention()
{
    QFETCH(int, bpp);
    const QByteArray blank(bpp, char(0x19)), paint(bpp, char(0x57));
    const auto bytes = [](const QByteArray &value) { return reinterpret_cast<const quint8 *>(value.constData()); };
    KisTiledDataManager source(bpp, bytes(blank));
    source.clear(QRect(0, 0, 64, 64), bytes(paint));
    KoStoreFake output; KisFakePaintDeviceWriter writer(&output);
    QVERIFY(source.write(writer)); output.startReading();
    const QByteArray data = output.device()->readAll();
    KisTiledDataManager target(bpp, bytes(blank));
    auto *protocol = target.m_nativeResult.get();
    kisDrainPageStoreReclamation();
    const auto retained = KisNativeResultTestAccess::state(*protocol).logicalViews;
    QVector<qsizetype> counts;
    for (int i = 0; i < 4; ++i) {
        QBuffer stream; stream.setData(data); QVERIFY(stream.open(QIODevice::ReadOnly));
        QVERIFY(target.read(&stream));
        QByteArray pixel(bpp, Qt::Uninitialized);
        target.readBytes(reinterpret_cast<quint8 *>(pixel.data()), 0, 0, 1, 1);
        QCOMPARE(pixel, paint);
        target.clear();
        kisDrainPageStoreReclamation();
        QVERIFY(!target.hasCurrentMemento());
        QCOMPARE(KisNativeResultTestAccess::state(*protocol).activeResults, quint64(0));
        counts.append(KisNativeResultTestAccess::state(*protocol).logicalViews);
        QCOMPARE(counts.last(), retained);
    }
    qInfo() << "Import retention after each read/clear/drain:" << counts << "baseline:" << retained;
    QCOMPARE(counts.last(), retained);
}

void KisTiledDataManagerTest::testPageStoreImportAdmission_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("history");
    for (int bpp : {1, 4, 8, 16}) for (bool history : {false, true})
        QTest::newRow(qPrintable(QString("B%1-history%2").arg(bpp).arg(history))) << bpp << history;
}

void KisTiledDataManagerTest::testPageStoreImportAdmission()
{
    QFETCH(int, bpp); QFETCH(bool, history);
    const QByteArray blank(bpp, char(0x19));
    KisTiledDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    auto *protocol = dm.m_nativeResult.get();
    const auto owner = history ? dm.getMemento() : KisMementoSP{};
    auto transaction = history ? KisNativeResultProtocol::Prepared{} : protocol->prepareProducer(QRect(0, 0, 64, 64));
    QVERIFY(history ? bool(owner) : bool(transaction));
    // A valid empty file must still honour the original idle-store admission.
    QByteArray data = QString("VERSION 2\nTILEWIDTH 64\nTILEHEIGHT 64\nPIXELSIZE %1\nDATA 0\n").arg(bpp).toLatin1();
    QBuffer stream(&data); QVERIFY(stream.open(QIODevice::ReadOnly));
    QVERIFY(!dm.read(&stream));
    QCOMPARE(dm.hasCurrentMemento(), history);
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).activeResults, quint64(history ? 0 : 1));
    if (history) QVERIFY(dm.tryAbort(owner));
    else protocol->cancel(transaction);
    QVERIFY(stream.seek(0)); QVERIFY(dm.read(&stream));
    kisDrainPageStoreReclamation();
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).activeResults, quint64(0));
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).logicalViews, quint64(0));
}

void KisTiledDataManagerTest::testPageStoreImportPublicationCapacity_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("funded");
    for (int bpp : {1, 4, 8, 16}) for (bool funded : {false, true})
        QTest::newRow(qPrintable(QString("B%1-funded%2").arg(bpp).arg(funded))) << bpp << funded;
}

void KisTiledDataManagerTest::testPageStoreImportPublicationCapacity()
{
    QFETCH(int, bpp); QFETCH(bool, funded);
    const QByteArray blank(bpp, char(0x19)), paint(bpp, char(0x57));
    const auto bytes = [](const QByteArray &p) { return reinterpret_cast<const quint8 *>(p.constData()); };
    KisTiledDataManager source(bpp, bytes(blank));
    source.clear(QRect(0, 0, 17 * 64, 64), bytes(paint));
    KoStoreFake output; KisFakePaintDeviceWriter writer(&output);
    QVERIFY(source.write(writer)); output.startReading();
    QByteArray data = output.device()->readAll();
    QBuffer stream(&data); QVERIFY(stream.open(QIODevice::ReadOnly));
    KisTiledDataManager dm(bpp, bytes(blank)); auto *protocol = dm.m_nativeResult.get();
    kisDrainPageStoreReclamation(); auto process = kisAcquirePageStoreBootstrapBudget(); QVERIFY(process);
    size_t count = 0; void *filler = nullptr; bool injected = false;
    const auto free = qScopeGuard([&] { if (filler) freeTestProcessStoragePressure(filler, count, 1); });
    const auto saturate = [&] {
        injected = true;
        const auto usage = process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        count = size_t(kisPageProcessStorageLimit() - usage.live.cpuRam - usage.reserved.cpuRam);
        filler = allocateTestProcessStoragePressure(count, 1);
    };
    KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        // Once decoded inputs and their original capacity are funded, the
        // final publication must not request a second allocation lifetime.
        if (phase != KisPageStoreDiagnosticPhase::NativeAcceptPrepared || !stream.atEnd() || injected) return;
        saturate();
    });
    if (!funded) saturate();
    QCOMPARE(dm.read(&stream), funded);
    recorder.setPhaseObserver({});
    if (filler) freeTestProcessStoragePressure(std::exchange(filler, nullptr), count, 1);
    QVERIFY(injected); QVERIFY(!dm.hasCurrentMemento());
    kisDrainPageStoreReclamation();
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).activeResults, quint64(0));
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).logicalViews, quint64(0));
    QCOMPARE(dm.m_hashTable->numTiles(), funded ? 17 : 0);
    QCOMPARE(dm.extent(), funded ? QRect(0, 0, 17 * 64, 64) : QRect());
    QVERIFY(stream.seek(0)); QVERIFY(dm.read(&stream));
    QCOMPARE(dm.m_hashTable->numTiles(), 17);
    QByteArray pixel(bpp, 0); dm.readBytes(reinterpret_cast<quint8 *>(pixel.data()), 16 * 64, 0, 1, 1);
    QCOMPARE(pixel, paint);
    dm.clear(); kisDrainPageStoreReclamation();
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).logicalViews, quint64(0));
}

void KisTiledDataManagerTest::testPageStoreImportStreaming_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("pages");
    for (int bpp : {1, 4, 8, 16}) for (int pages : {17, 257})
        QTest::newRow(qPrintable(QString("B%1-N%2").arg(bpp).arg(pages))) << bpp << pages;
}

void KisTiledDataManagerTest::testPageStoreImportStreaming()
{
    QFETCH(int, bpp); QFETCH(int, pages);
    const QByteArray blank(bpp, char(0x19)), paint(bpp, char(0x57));
    const auto bytes = [](const QByteArray &p) { return reinterpret_cast<const quint8 *>(p.constData()); };
    KisTiledDataManager source(bpp, bytes(blank));
    source.clear(QRect(0, 0, pages * 64, 64), bytes(paint));
    KoStoreFake output; KisFakePaintDeviceWriter writer(&output);
    QVERIFY(source.write(writer)); output.startReading();
    class StreamingInput : public QBuffer {
    public:
        std::function<void(const QByteArray &)> tileHeader;
    protected:
        qint64 readLineData(char *data, qint64 length) override
        {
            const auto size = QBuffer::readLineData(data, length);
            if (size > 0 && tileHeader) tileHeader(QByteArray(data, size));
            return size;
        }
    } stream;
    stream.setData(output.device()->readAll()); QVERIFY(stream.open(QIODevice::ReadOnly));
    KisTiledDataManager dm(bpp, bytes(blank)); auto *protocol = dm.m_nativeResult.get();
    QPoint previous; bool hadPrevious = false, swapped = true; int swapCount = 0;
    stream.tileHeader = [&](const QByteArray &header) {
        const auto fields = header.trimmed().split(',');
        if (fields.size() != 4) return;
        if (hadPrevious) {
            const auto tile = dm.getTile(previous.x(), previous.y(), true);
            auto *backing = tile ? tile->tileData() : nullptr;
            // Force the previous decoded page out before reading the next
            // one. An unsealed writer may retain identity, never a RAM pin.
            const bool released = backing && KisTileDataStore::instance()->trySwapTileData(backing)
                && !backing->isResident();
            swapped = swapped && released;
            if (released) ++swapCount;
        }
        previous = QPoint(fields[0].toInt() / 64, fields[1].toInt() / 64);
        hadPrevious = true;
    };
    QVERIFY(dm.read(&stream)); QVERIFY(swapped); QCOMPARE(swapCount, pages - 1);
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).activeResults, quint64(0));
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).logicalViews, quint64(0));
    QCOMPARE(dm.m_hashTable->numTiles(), pages);
    QByteArray pixels(pages * 64 * bpp, 0);
    dm.readBytes(reinterpret_cast<quint8 *>(pixels.data()), 0, 0, pages * 64, 1);
    QCOMPARE(pixels, QByteArray(pixels.size(), char(0x57)));
    qInfo() << "Import streaming pages:" << pages << "bpp:" << bpp
            << "forced swaps:" << swapCount;
    dm.clear(); kisDrainPageStoreReclamation();
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).activeResults, quint64(0));
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).logicalViews, quint64(0));
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

void KisTiledDataManagerTest::testPageStorePurgePreparedDelivery_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("history"); QTest::addColumn<int>("count");
    for (int bpp : {1, 4, 8, 16}) for (bool history : {false, true}) for (int count : {0, 1, 17})
        QTest::newRow(qPrintable(QString("bpp%1-history%2-count%3").arg(bpp).arg(history).arg(count)))
            << bpp << history << count;
}

void KisTiledDataManagerTest::testPageStorePurgePreparedDelivery()
{
    QFETCH(int, bpp); QFETCH(bool, history); QFETCH(int, count);
    const QByteArray blank(bpp, char(0x13)), color(bpp, char(0x71)), nextDefault(bpp, char(0x29));
    const auto bytes = [](const QByteArray &v) { return reinterpret_cast<const quint8 *>(v.constData()); };
    TestableTiledDataManager dm(bpp, bytes(blank));
    const QByteArray input((count + 2) * 64 * 64 * bpp, char(0x13));
    dm.writeBytes(bytes(input), -64, 0, (count + 2) * 64, 64);
    // The last candidate differs only at its last pixel; page -1 is out of area.
    dm.writeBytes(bytes(color), count * 64 + 63, 63, 1, 1);
    auto &protocol=*dm.m_nativeResult;
    auto *originalTable = dm.m_hashTable;
    const auto retained = protocol.capture(); QVERIFY(retained);
    auto reader = dm.getTile(0, 0, false); QVERIFY(reader && reader->lockForRead());
    const auto unlock = qScopeGuard([&] { reader->unlockForRead(); });
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    if (history) QVERIFY(memento);
    bool installed = false, held = false;
    KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::NativeAdapterInstalled) return;
        installed = true; held = KisNativeResultTestAccess::state(protocol).activeResults == 1;
    });
    // One pixel of the first page still selects the entire tile for comparison.
    dm.purge(QRect(63, 0, count * 64 + 1, 64));
    recorder.setPhaseObserver({});
    QCOMPARE(installed, count != 0); QCOMPARE(held, count != 0);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults, quint64(0));
    QCOMPARE(dm.m_hashTable, originalTable); QCOMPARE(dm.m_hashTable->numTiles(), 2);
    bool exists = true; dm.getReadOnlyTileLazy(0, 0, exists); QCOMPARE(exists, count == 0);
    dm.getReadOnlyTileLazy(-1, 0, exists); QVERIFY(exists);
    dm.getReadOnlyTileLazy(count, 0, exists); QVERIFY(exists);
    QCOMPARE(reader->data()[0], quint8(0x13));
    auto old = retained.readPage(0,0); QVERIFY(old.data());
    QCOMPARE(old.data()[0], quint8(0x13));
    if (history) {
        QVERIFY(dm.tryCommit()); dm.rollback(memento);
        QCOMPARE(dm.m_hashTable->numTiles(), count + 2);
        dm.rollforward(memento); QCOMPARE(dm.m_hashTable->numTiles(), 2);
        dm.purgeHistory(memento);
    }
    dm.setDefaultPixel(bytes(nextDefault));
    QByteArray actual(bpp, 0);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1);
    QCOMPARE(actual, count ? nextDefault : blank);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), -64, 0, 1, 1); QCOMPARE(actual, blank);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), count * 64 + 63, 63, 1, 1); QCOMPARE(actual, color);
}

void KisTiledDataManagerTest::testPageStorePurgePublicationRefusal_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("history"); QTest::addColumn<bool>("root");
    for (int bpp : {1, 4, 8, 16}) for (bool history : {false, true}) for (bool root : {false, true}) {
        if (history && root) continue; // Borrowed removal seals; history owns root publication.
        QTest::newRow(qPrintable(QString("bpp%1-history%2-root%3").arg(bpp).arg(history).arg(root)))
            << bpp << history << root;
    }
}

void KisTiledDataManagerTest::testPageStorePurgePublicationRefusal()
{
QFETCH(int,bpp);QFETCH(bool,history);QFETCH(bool,root);
    const QByteArray blank(bpp,char(0x13)),input(17*64*64*bpp,char(0x13));
    TestableTiledDataManager dm(bpp,reinterpret_cast<const quint8 *>(blank.constData()));
    dm.writeBytes(reinterpret_cast<const quint8 *>(input.constData()),0,0,17*64,64);auto &protocol=*dm.m_nativeResult;
    const auto memento=history?dm.getMemento():KisMementoSP{};QVERIFY(!history||memento);const auto extent=dm.extent();
    kisDrainPageStoreReclamation();auto process=kisAcquirePageStoreBootstrapBudget();QVERIFY(process);
    size_t count=0;void *filler=nullptr;const auto free=qScopeGuard([&]{if(filler)freeTestProcessStoragePressure(filler,count,1);});
    const auto fill=[&]{const auto usage=process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        count=size_t(kisPageProcessStorageLimit()-usage.live.cpuRam-usage.reserved.cpuRam);filler=allocateTestProcessStoragePressure(count,1);};
    bool installed=false,injected=false;KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](auto phase){if(phase==KisPageStoreDiagnosticPhase::NativeAdapterInstalled)installed=true;
        if(root&&!injected&&phase==KisPageStoreDiagnosticPhase::NativeAcceptPrepared){injected=true;fill();}});
    if(!root){fill();injected=true;}
    KisStrokeJobFailureContext failure(true);dm.purge(extent);recorder.setPhaseObserver({});
    freeTestProcessStoragePressure(std::exchange(filler,nullptr),count,1);QVERIFY(injected);QVERIFY(!root||installed);QCOMPARE(failure.failed(),!installed);
    // With a retained history, already paid removal capacity may complete
    // even before the funded marker. Cold/no-history still proves refusal.
    if(!root&&!history)QVERIFY(!installed);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
    QCOMPARE(dm.m_hashTable->numTiles(),installed?0:17);QCOMPARE(dm.extent(),installed?QRect():extent);
    dm.purge(extent);QCOMPARE(dm.m_hashTable->numTiles(),0);QVERIFY(dm.extent().isEmpty());
    if(history){QVERIFY(dm.tryCommit());dm.rollback(memento);QCOMPARE(dm.m_hashTable->numTiles(),17);dm.purgeHistory(memento);}
}

void KisTiledDataManagerTest::testPageStoreVirtualDefaultReadStaysSparse()
{
    quint8 defaultPixel = 0x2a;
    KisTiledDataManager dm(1, &defaultPixel);
    const auto before = KisNativeResultTestAccess::state(*dm.m_nativeResult);
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

    const auto after = KisNativeResultTestAccess::state(*dm.m_nativeResult);
    QCOMPARE(after.pageSlots, before.pageSlots);
    QCOMPARE(after.historyEdges, before.historyEdges);
    QCOMPARE(after.physicalResources, before.physicalResources);
    QCOMPARE(after.logicalViews, before.logicalViews);
    QCOMPARE(after.acceptedEpoch, before.acceptedEpoch);
    QCOMPARE(after.activeResults, quint64(0));

    QCOMPARE(KisTileDataStore::instance()->numTiles(), tileDataBefore);
    QCOMPARE(dm.extent(), QRect());
}

void KisTiledDataManagerTest::testPageStorePartialDefaultClearStaysSparse_data()
{
    QTest::addColumn<int>("bpp");
    QTest::addColumn<bool>("history");
    for (int bpp : {1, 4, 8, 16}) for (bool history : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("bpp%1-history%2").arg(bpp).arg(history))) << bpp << history;
}

void KisTiledDataManagerTest::testPageStorePartialDefaultClearStaysSparse()
{
    QFETCH(int, bpp); QFETCH(bool, history);
    const QByteArray blank(bpp, char(0x13)), color(bpp, char(0x47));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    dm.clear(0, 0, 64, 64, reinterpret_cast<const quint8 *>(color.constData()));
    dm.clear(128, 0, 64, 64, reinterpret_cast<const quint8 *>(color.constData()));
    auto *backend = dm.m_nativeResult.get();
    auto frozen = backend->capture(); QVERIFY(bool(frozen));
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    const auto before = KisNativeResultTestAccess::state(*backend);
    const QRect hole(65, 1, 62, 62);
    dm.clear(hole.x(), hole.y(), hole.width(), hole.height(), reinterpret_cast<const quint8 *>(blank.constData()));
    QCOMPARE(KisNativeResultTestAccess::state(*backend).physicalResources, before.physicalResources);
    QCOMPARE(KisNativeResultTestAccess::state(*backend).acceptedEpoch, before.acceptedEpoch);
    QCOMPARE(dm.extent(), QRect(0, 0, 192, 64));
    auto current = backend->capture(); auto version = current.versionAt(1, 0);
    QVERIFY(version.isDefaultPixel()); current = {};

    // A different fill must still materialize the hole, preserve its border,
    // and leave the captured default page and surrounding painted pages intact.
    dm.clear(hole.x(), hole.y(), hole.width(), hole.height(), reinterpret_cast<const quint8 *>(color.constData()));
    QCOMPARE(KisNativeResultTestAccess::state(*backend).acceptedEpoch, before.acceptedEpoch + 1);
    QByteArray expected(192 * 64 * bpp, char(0x13)), actual(expected.size(), Qt::Uninitialized);
    for (int y = 0; y < 64; ++y) {
        std::memset(expected.data() + y * 192 * bpp, 0x47, size_t(64 * bpp));
        std::memset(expected.data() + (y * 192 + 128) * bpp, 0x47, size_t(64 * bpp));
        if (y >= hole.top() && y <= hole.bottom())
            std::memset(expected.data() + (y * 192 + hole.x()) * bpp, 0x47, size_t(hole.width() * bpp));
    }
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 192, 64); QCOMPARE(actual, expected);
    QVERIFY(frozen.versionAt(1, 0).isDefaultPixel());
    if (history) {
        dm.commit(); dm.rollback(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 65, 1, 62, 62);
        QCOMPARE(actual.left(62 * 62 * bpp), QByteArray(62 * 62 * bpp, char(0x13)));
        dm.rollforward(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 192, 64); QCOMPARE(actual, expected);
    }
    frozen = {};
    kisDrainPageStoreReclamation();
    QCOMPARE(KisNativeResultTestAccess::state(*backend).activeResults, quint64(0));
}

void KisTiledDataManagerTest::testPageStoreCompatibilityBehavior()
{
    quint8 defaultPixel = 0;
    quint8 paintedPixel = 0x35;
    KisTiledDataManager dm(1, &defaultPixel);
    auto *backend = dm.m_nativeResult.get();
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
QFETCH(int,bpp);QFETCH(bool,packed);QFETCH(bool,warm);
    const QByteArray blank(bpp,0),old(bpp,char(0x31)),value(bpp,char(0x61));
    KisDataManager dm(bpp,reinterpret_cast<const quint8 *>(blank.constData()));auto &protocol=*dm.m_nativeResult;
    if(warm)dm.writeBytes(reinterpret_cast<const quint8 *>(old.constData()),0,0,1,1);auto before=protocol.capture();QVERIFY(before);
    QSemaphore finished;std::thread reader;bool inPhase=false,coherent=false;int seen=0;
    const auto join=qScopeGuard([&]{if(reader.joinable())reader.join();});KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](auto phase){if(phase!=KisPageStoreDiagnosticPhase::NativeAdapterInstalled||seen++)return;
        reader=std::thread([&]{auto view=protocol.capture();auto page=view.readPage(0,0);
            coherent=view&&page.data()&&QByteArray(reinterpret_cast<const char *>(page.data()),bpp)==value&&dm.extent()==QRect(0,0,64,64);finished.release();});
        inPhase=finished.tryAcquire(1,5000);
    });
    bool succeeded=false;
    if(packed){KisStrokeJobFailureContext failure(true);dm.writeBytes(reinterpret_cast<const quint8 *>(value.constData()),0,0,1,1);succeeded=!failure.failed();}
    else succeeded=dm.writePageStoreOperation({QRect(0,0,1,1)},[&](KisPixelWriteCursor *c){c->moveTo(0,0);auto *p=c->rawData();if(!p)return false;std::memcpy(p,value.constData(),bpp);return true;})==KisPageStoreWriteOperationResult::Succeeded;
    recorder.setPhaseObserver({});if(reader.joinable())reader.join();QVERIFY(succeeded);QCOMPARE(seen,1);QVERIFY(inPhase&&coherent);
    auto prior=before.readPage(0,0);QVERIFY(prior.data());QCOMPARE(QByteArray(reinterpret_cast<const char *>(prior.data()),bpp),warm?old:blank);
    auto now=protocol.capture().readPage(0,0);QVERIFY(now.data());QCOMPARE(QByteArray(reinterpret_cast<const char *>(now.data()),bpp),value);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
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
    auto &protocol=*dm.m_nativeResult;
    auto before = protocol.capture();
    bool coherent = true;
    int sealed = 0, committed = 0;
    KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        const bool published = phase == KisPageStoreDiagnosticPhase::NativeAdapterInstalled;
        // The anonymous operation now accepts directly into canonical state.
        // Observe the old cut before acceptance, not after destination cleanup.
        if (!published && phase != KisPageStoreDiagnosticPhase::NativeAcceptPrepared) return;
        if (published) ++committed; else ++sealed;
        auto view = protocol.capture();
        auto page = view.readPage(2,0);
        coherent &= bool(page.data()) && QByteArray(reinterpret_cast<const char *>(page.data()), bpp) == (published ? value : blank) &&
            dm.extent() == (published ? (warm ? QRect(0, 0, 192, 64) : QRect(128, 0, 64, 64)) :
                                                (warm ? QRect(0, 0, 64, 64) : QRect()));
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
    QCOMPARE(sealed, 1); QCOMPARE(committed, 1);
    QVERIFY(coherent);
    auto oldPage = before.readPage(2,0);
    QVERIFY(oldPage.data());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(oldPage.data()), bpp), blank);
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
QFETCH(int,bpp);QFETCH(bool,removal);QByteArray blank(bpp,0),old(bpp,char(0x31));
    KisTiledDataManager dm(bpp,reinterpret_cast<const quint8 *>(blank.constData()));auto &p=*dm.m_nativeResult;
    QVERIFY(p.fill(QRect(-128,0,1,1),old));QVERIFY(p.fill(QRect(128,0,1,1),old));auto before=p.capture();QVERIFY(before);
    bool entered=false,sibling=false;KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](auto phase){if(entered||phase!=KisPageStoreDiagnosticPhase::NativeAcceptPrepared)return;entered=true;sibling=p.fill(QRect(256,0,1,1),QByteArray(bpp,char(0x71)));});
    QVERIFY(removal?p.remove(QRect(-128,0,64,64)):p.fill(QRect(-256,0,1,1),QByteArray(bpp,char(0x61))));recorder.setPhaseObserver({});QVERIFY(entered&&sibling);
    QCOMPARE(dm.extent(),removal?QRect(128,0,192,64):QRect(-256,0,576,64));
    auto after=p.capture();for(int x:{-4,-2,2,4}){auto page=after.readPage(x,0);QVERIFY(page.data());
        QCOMPARE(page.data()[0],quint8(x==4?0x71:x==2?0x31:x==-2?(removal?0:0x31):(removal?0:0x61)));}
    for(int x:{-2,2}){auto page=before.readPage(x,0);QVERIFY(page.data());QCOMPARE(page.data()[0],quint8(0x31));}
}

void KisTiledDataManagerTest::testPageStoreOverlayExtentRangeFailure()
{
const quint8 blank=0;KisTiledDataManager dm(1,&blank);auto &p=*dm.m_nativeResult;QString error;
    const int left=-0x7ffe,right=0x7fff;QVERIFY(KisTileHashTable::supportsCoordinates(left,0));QVERIFY(!KisTileHashTable::supportsCoordinates(right,0));
    QVERIFY(p.fill(QRect(left*64,0,1,1),QByteArray(1,char(0x61)),&error));auto before=p.capture();QVERIFY(before);
    QVERIFY(!p.fill(QRect(right*64,0,1,1),QByteArray(1,char(0x61)),&error));QCOMPARE(dm.extent(),QRect(left*64,0,64,64));
    auto after=p.capture();for(const auto *view:{&before,&after}){auto prior=view->readPage(left,0);auto absent=view->readPage(right,0);
        QVERIFY(prior.data());QVERIFY(!absent.data());QCOMPARE(prior.data()[0],quint8(0x61));}
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
    const QRect painted(-64, -64, 128, 128);
    QByteArray pixels(painted.width() * painted.height() * pixelSize, Qt::Uninitialized);
    for (qsizetype i = 0; i < pixels.size(); ++i) pixels[i] = char(i * 37 % 251);
    dm.writeBytes(reinterpret_cast<const quint8 *>(pixels.constData()), painted.x(), painted.y(),
                  painted.width(), painted.height());
    const QRect area(-70, -65, 150, 140);
    const int stride = area.width() * pixelSize + 13;
    QByteArray output(stride * area.height(), char(0x6d));
    dm.readBytes(reinterpret_cast<quint8 *>(output.data()), area.x(), area.y(), area.width(), area.height(), stride);
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
    kisDrainPageStoreReclamation();
}

void KisTiledDataManagerTest::testPreparedWarmBorrowBothTables()
{
    const auto check = [&](auto *type) {
        using Table = std::remove_pointer_t<decltype(type)>;
        const quint8 initial = 0x2a;
        auto *data = KisTileDataStore::instance()->createDefaultTileData(1, &initial);
        Table table(nullptr);
        table.setDefaultTileData(data);
        for (int i = 0; i < 32; ++i) {
            // Same low hash bits exercise chained lookup as the table grows.
            const int col = i * 1024;
            auto cold = table.prepareTileUpdate(col, 0);
            QVERIFY(cold);
            bool created = false;
            auto installed = table.installPreparedTile(cold, created);
            QVERIFY(installed && created);
            KisTile *preparedLocation = nullptr;
            QVERIFY(!table.prepareTileUpdate(col, 0, Table::Update::Present, &preparedLocation));
            QCOMPARE(preparedLocation, installed.data());
            auto *borrowed = table.borrowTileForPreparedUpdate(col, 0);
            QCOMPARE(borrowed, installed.data());
            borrowed->invalidatePageStoreReadCache();
        }
        QCOMPARE(table.numTiles(), qsizetype(32));
    };
    check(static_cast<KisTileHashTableTraits<KisTile> *>(nullptr));
    check(static_cast<KisTileHashTableTraits2<KisTile> *>(nullptr));
}

void KisTiledDataManagerTest::testPageStorePackedReadSurvivesPublication_data()
{
    QTest::addColumn<int>("terminal");
    QTest::addColumn<int>("count"); QTest::addColumn<int>("publishAt");
    for (int terminal = 0; terminal < 3; ++terminal)
        for (int count : {3,16,17,33}) for (int at : {2,17}) {
            if (at > count) continue;
            QTest::newRow(qPrintable(QStringLiteral("terminal%1-k%2-at%3").arg(terminal).arg(count).arg(at)))
                << terminal << count << at;
        }
}

void KisTiledDataManagerTest::testPageStorePackedReadSurvivesPublication()
{
QFETCH(int,terminal);QFETCH(int,count);QFETCH(int,publishAt);
    const quint8 initial=0x2a;KisTiledDataManager dm(1,&initial);auto &p=*dm.m_nativeResult;QString error;
    const int width=count*64,presentWidth=width-64;KisMementoSP history;
    if(terminal){history=dm.getMemento();QVERIFY(history);}QVERIFY(p.fill(QRect(0,0,presentWidth,64),QByteArray(1,char(0x51))));
    QByteArray bytes(width*64,0);int pages=0;bool readyInRead=false,mutationSucceeded=false;std::atomic<bool> accepted{false};
    QSemaphore prepared;std::thread writer;const auto join=qScopeGuard([&]{if(writer.joinable())writer.join();});
    KisPageStoreDiagnosticRecorder recorder(true);recorder.setPhaseObserver([&](auto phase){
        if(phase!=KisPageStoreDiagnosticPhase::ReadBytesPage||++pages!=publishAt)return;
        writer=std::thread([&]{bool announced=false;KisPageStoreDiagnosticRecorder writing(true);writing.setPhaseObserver([&](auto stage){
            if(!announced&&stage==KisPageStoreDiagnosticPhase::NativeAcceptPrepared){announced=true;prepared.release();}});
            mutationSucceeded=p.fill(QRect(0,0,presentWidth,64),QByteArray(1,char(0x71)),&error)&&p.setDefaultPixel(QByteArray(1,char(0x41)),&error);
            if(terminal==1)mutationSucceeded&=dm.tryCommit(&error);if(terminal==2)mutationSucceeded&=dm.tryAbort(history,&error);accepted=true;
        });readyInRead=prepared.tryAcquire(1,5000)&&!accepted.load();
    });
    dm.readBytes(reinterpret_cast<quint8 *>(bytes.data()),0,0,width,64);recorder.setPhaseObserver({});if(writer.joinable())writer.join();
    QVERIFY(readyInRead);QVERIFY2(mutationSucceeded,qPrintable(error));QCOMPARE(pages,count);
    for(int y=0;y<64;++y){QCOMPARE(bytes.mid(y*width,presentWidth),QByteArray(presentWidth,char(0x51)));QCOMPARE(bytes.mid(y*width+presentWidth,64),QByteArray(64,char(0x2a)));}
    if(terminal==1)dm.purgeHistory(history);kisDrainPageStoreReclamation();
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
    auto *backend = dm.m_nativeResult.get();
    QString error;
    KisMementoSP history;
    if (terminal) { history = backend->beginHistory(&error); QVERIFY(history); }
    QVERIFY(backend->fill(QRect(-64, -64, 384, 384), QByteArray(1, char(0x47)), &error));
    {
        std::unique_ptr<KisHLineIterator2> h;
        std::unique_ptr<KisVLineIterator2> v;
        std::unique_ptr<KisRandomAccessor2> r;
        if (entry == 0) h = std::make_unique<KisHLineIterator2>(&dm, -64, -64, 448, 0, 0, false, nullptr);
        if (entry == 1) v = std::make_unique<KisVLineIterator2>(&dm, -64, -64, 448, 0, 0, false, nullptr);
        if (entry == 2) { r = std::make_unique<KisRandomAccessor2>(&dm, 0, 0, false, nullptr); r->moveTo(-64, -64); }
        auto current = [&] { return h ? h->rawDataConst() : v ? v->rawDataConst() : r->rawDataConst(); };
        auto before = [&] { return h ? h->oldRawData() : v ? v->oldRawData() : r->oldRawData(); };
        QCOMPARE(*current(), quint8(0x47));
        QVERIFY(backend->fill(QRect(-64, -64, 384, 384), QByteArray(1, char(0x71)), &error));
        QVERIFY(backend->setDefaultPixel(QByteArray(1, char(0x29)), &error));
        if (terminal == 1) QVERIFY(backend->commitHistory(&error));
        if (terminal == 2) QVERIFY(backend->abortHistory(history, &error));
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
    }
    if (terminal == 1) backend->purgeHistory(history);
    kisDrainPageStoreReclamation();
    QCOMPARE(KisNativeResultTestAccess::state(*dm.m_nativeResult).activeResults, quint64(0));

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
    auto *backend = dm.m_nativeResult.get();
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
        QVERIFY(dm.capturePageStoreReadScope(false)->observesLiveLegacyWriter());
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
            // A read does not revoke a pointer issued by a still-live writer.
            *borrowed = 0x71;
            QCOMPARE(readH.rawDataConst()[0], quint8(0x71));
            QCOMPARE(before()[0], history ? background : quint8(0x71));
            if (!history) QCOMPARE(before(), borrowed);
        }
        QCOMPARE(data(), borrowed);
    }
    QVERIFY(!dm.capturePageStoreReadScope(false)->observesLiveLegacyWriter());
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
    auto *backend = dm.m_nativeResult.get();
    auto finalWriter = std::make_unique<KisRandomAccessor2>(
        &dm, 0, 0, true, nullptr);
    finalWriter->moveTo(0, 0);
    QVERIFY(finalWriter->rawData());
    QVERIFY(dm.capturePageStoreReadScope(false)->observesLiveLegacyWriter());
    std::thread finalRelease([writer = std::move(finalWriter)]() mutable {
        writer.reset();
    });
    finalRelease.join();
    QVERIFY(!dm.capturePageStoreReadScope(false)->observesLiveLegacyWriter());
}

void KisTiledDataManagerTest::testPageStoreReadCursorBounded()
{
    const quint8 background = 0x13;
    KisDataManager dm(1, &background);
    const auto initial = KisNativeResultTestAccess::state(*dm.m_nativeResult);
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
    }
    const auto final = KisNativeResultTestAccess::state(*dm.m_nativeResult);
    QCOMPARE(final.pageSlots, initial.pageSlots);
    QCOMPARE(final.physicalResources, initial.physicalResources);
    QCOMPARE(final.logicalViews, initial.logicalViews);

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
    auto *backend = dm.m_nativeResult.get();
    QString error;
    const QRect bounds(0, 0, 128, 128), area(-5, -7, 270, 260);
    QVERIFY(backend->fill(bounds, QByteArray(1, char(0x47)), &error));
    {
        const KisWrappedRect split(area, bounds, WRAPAROUND_BOTH);
        QCOMPARE(split.size(), 4);
        std::unique_ptr<KisWrappedHLineIterator> h;
        std::unique_ptr<KisWrappedVLineIterator> v;
        if (horizontal) h = std::make_unique<KisWrappedHLineIterator>(&dm, split, 0, 0, false, nullptr);
        else v = std::make_unique<KisWrappedVLineIterator>(&dm, split, 0, 0, false, nullptr);
        QVERIFY(backend->fill(bounds, QByteArray(1, char(0x71)), &error));
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
    }
    kisDrainPageStoreReclamation();
    QCOMPARE(KisNativeResultTestAccess::state(*dm.m_nativeResult).activeResults, quint64(0));

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
    auto *backend = dm.m_nativeResult.get();
    QString error;
    const QRect bounds(0, 0, 128, 128);
    QVERIFY(backend->fill(bounds, QByteArray(1, char(0x47)), &error));
    {
        std::unique_ptr<KisRepeatHLineIteratorPixelBase<KisHLineIterator2>> h;
        std::unique_ptr<KisRepeatVLineIteratorPixelBase<KisVLineIterator2>> v;
        if (horizontal) h = std::make_unique<KisRepeatHLineIteratorPixelBase<KisHLineIterator2>>(&dm, -2, -2, 140, 0, 0, bounds, nullptr);
        else v = std::make_unique<KisRepeatVLineIteratorPixelBase<KisVLineIterator2>>(&dm, -2, -2, 140, 0, 0, bounds, nullptr);
        QVERIFY(backend->fill(bounds, QByteArray(1, char(0x71)), &error));
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
    }
    kisDrainPageStoreReclamation();
    QCOMPARE(KisNativeResultTestAccess::state(*dm.m_nativeResult).activeResults, quint64(0));

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
    auto &protocol=*dm.m_nativeResult;
    QString error;
    QVERIFY(protocol.fill(QRect(-64, -64, 128, 128), painted, &error));
    const QRect area(-70, -65, 150, 140);
    const QVector<qint32> channels = pixelSize == 1 ? QVector<qint32>{1} : QVector<qint32>{1, 0, pixelSize - 2, 1};
    const auto views=KisNativeResultTestAccess::state(protocol).logicalViews;
    bool changed=false,readyInRead=false;std::atomic<bool> accepted{false};int pages=0;QSemaphore prepared;std::thread writer;
    const auto join=qScopeGuard([&]{if(writer.joinable())writer.join();});KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](auto phase){if(phase!=KisPageStoreDiagnosticPhase::ReadPlanarPage||++pages!=2)return;
        writer=std::thread([&]{bool announced=false;KisPageStoreDiagnosticRecorder writing(true);writing.setPhaseObserver([&](auto stage){
            if(!announced&&stage==KisPageStoreDiagnosticPhase::NativeAcceptPrepared){announced=true;prepared.release();}});
            changed=protocol.fill(QRect(-64,-64,128,128),QByteArray(pixelSize,char(0x71)),&error)&&protocol.setDefaultPixel(QByteArray(pixelSize,char(0x29)),&error);accepted=true;
        });readyInRead=prepared.tryAcquire(1,5000)&&!accepted.load();
    });
    auto planes = dm.readPlanarBytes(channels, area.x(), area.y(), area.width(), area.height());
    recorder.setPhaseObserver({});if(writer.joinable())writer.join();QVERIFY(readyInRead);
    const auto cleanup = qScopeGuard([&] { for (auto plane : planes) delete[] plane; });
    QCOMPARE(planes.size(), channels.size()); QVERIFY2(changed, qPrintable(error));
    QCOMPARE(KisNativeResultTestAccess::state(protocol).logicalViews,views);
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
    QCOMPARE(KisNativeResultTestAccess::state(protocol).logicalViews,views);
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
    auto *backend = dm.m_nativeResult.get();
    const auto memento = dm.getMemento(); QVERIFY(memento);
    QString error; QVERIFY2(dm.beginStrokeMutation(memento, &error), qPrintable(error));
    QVERIFY(dm.beginStrokeMutation(memento)); // same owner, same original session
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
    // Ordinary exact reads remain on the last complete cut. Prime a wrapper
    // with that old view to prove checkpoint invalidates it without a payload read.
    bool present = false; auto tile = dm.getReadOnlyTileLazy(0, 0, present); QVERIFY(tile);
    QVERIFY(tile->lockForRead()); QCOMPARE(tile->data()[0], quint8(0x31)); tile->unlockForRead();
    QVERIFY2(dm.checkpointStrokeMutation(memento, &error), qPrintable(error));
    auto frozen = backend->capture(); QVERIFY(bool(frozen));
    QCOMPARE(dm.extent(), QRect(0, 0, 66 * 64, 64));
    for (int page : {0, 65}) {
        auto read = frozen.readPage(page, 0); QVERIFY(bool(read.data()));
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(read.data()), bpp), QByteArray(bpp, char(page ? 0x52 : 0x51)));
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
    auto old = frozen.readPage(0, 0); QVERIFY(bool(old.data()));
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(old.data()), bpp), QByteArray(bpp, char(0x51)));
    if (!abort) {
        dm.rollback(memento); dm.readBytes(reinterpret_cast<quint8 *>(pixel.data()), 0, 0, 1, 1); QCOMPARE(pixel, initial);
        dm.rollforward(memento); dm.readBytes(reinterpret_cast<quint8 *>(pixel.data()), 0, 0, 1, 1); QCOMPARE(pixel, QByteArray(bpp, char(0x71)));
        dm.purgeHistory(memento);
    }
}

void KisTiledDataManagerTest::testNativeHistoryConcurrentRows()
{
    const quint8 blank = 0x13;
    KisTiledDataManager dm(1, &blank);
    const auto history = dm.getMemento(); QVERIFY(dm.beginStrokeMutation(history));
    const auto result = dm.writePageStoreOperation({QRect(0, 0, 128, 1)}, [](KisPixelWriteCursor *c) {
        c->moveTo(0, 0); if (!c->rawData()) return false; *c->rawData() = 0x31;
        c->moveTo(64, 0); if (!c->rawData()) return false; *c->rawData() = 0x32;
        return true;
    }, nullptr, history);
    QCOMPARE(result, KisPageStoreWriteOperationResult::Succeeded);
    QSemaphore entered, release;
    std::array<bool, 3> success{};
    std::array<std::thread, 3> workers;
    const auto join = qScopeGuard([&] { release.release(3); for (auto &t : workers) if (t.joinable()) t.join(); });
    for (size_t i = 0; i < workers.size(); ++i) workers[i] = std::thread([&, i] {
        success[i] = dm.writePageStoreOperation({QRect(int(i) * 64, 0, 1, 1)}, [&](KisPixelWriteCursor *c) {
            c->moveTo(int(i) * 64, 0); auto *p = c->rawData();
            entered.release(); release.acquire();
            if (!p) return false;
            *p = quint8(0x51 + i); return true;
        }, nullptr, history) == KisPageStoreWriteOperationResult::Succeeded;
    });
    QVERIFY(entered.tryAcquire(3, 5000));
    QVERIFY(!dm.checkpointStrokeMutation(history));
    quint8 before[129]; dm.readBytes(before, 0, 0, 129, 1);
    for (int x : {0, 64, 128}) QCOMPARE(before[x], blank);
    release.release(3); for (auto &t : workers) t.join();
    for (const bool ok : success) QVERIFY(ok);
    QVERIFY(dm.checkpointStrokeMutation(history));
    quint8 after[129]; dm.readBytes(after, 0, 0, 129, 1);
    for (int i = 0; i < 3; ++i) QCOMPARE(after[i * 64], quint8(0x51 + i));
    QVERIFY(dm.tryAbort(history));
    dm.readBytes(after, 0, 0, 129, 1);
    for (int x : {0, 64, 128}) QCOMPARE(after[x], blank);
    QCOMPARE(KisNativeResultTestAccess::state(*dm.m_nativeResult).activeResults, quint64(0));
}

void KisTiledDataManagerTest::testNativeHistoryCheckpointRetry()
{
    const quint8 blank = 0x13;
    KisTiledDataManager dm(1, &blank);
    const auto history = dm.getMemento(); QVERIFY(dm.beginStrokeMutation(history));
    int producerCalls = 0;
    QCOMPARE(dm.writePageStoreOperation({QRect(0, 0, 1, 1)}, [&](KisPixelWriteCursor *c) {
        ++producerCalls; c->moveTo(0, 0); auto *p = c->rawData();
        if (!p) return false; *p = 0x71; return true;
    }, nullptr, history), KisPageStoreWriteOperationResult::Succeeded);
    kisDrainPageStoreReclamation();
    auto process = kisAcquirePageStoreBootstrapBudget(); QVERIFY(process);
    const auto used = process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
    const size_t count = size_t(kisPageProcessStorageLimit() - used.live.cpuRam - used.reserved.cpuRam);
    void *filler = allocateTestProcessStoragePressure(count, 1); QVERIFY(filler);
    const auto free = qScopeGuard([&] { if (filler) freeTestProcessStoragePressure(filler, count, 1); });
    QVERIFY(!dm.checkpointStrokeMutation(history));
    freeTestProcessStoragePressure(std::exchange(filler, nullptr), count, 1);
    quint8 actual = 0; dm.readBytes(&actual, 0, 0, 1, 1); QCOMPARE(actual, blank);
    QVERIFY(dm.checkpointStrokeMutation(history)); QCOMPARE(producerCalls, 1);
    dm.readBytes(&actual, 0, 0, 1, 1); QCOMPARE(actual, quint8(0x71));
    QVERIFY(dm.tryCommit()); dm.rollback(history);
    dm.readBytes(&actual, 0, 0, 1, 1); QCOMPARE(actual, blank);
    dm.purgeHistory(history);
    QCOMPARE(KisNativeResultTestAccess::state(*dm.m_nativeResult).activeResults, quint64(0));
}

void KisTiledDataManagerTest::testPageStoreHistoryMutationFailure_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("cut");
    for (int bpp : {1, 4, 8, 16}) for (bool cut : {false, true})
        QTest::newRow(qPrintable(QString("B%1-cut%2").arg(bpp).arg(cut))) << bpp << cut;
}

void KisTiledDataManagerTest::testPageStoreHistorySegmentDelivery_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("direct"); QTest::addColumn<bool>("terminal");
    for (int bpp : {1, 4, 8, 16}) for (bool direct : {false, true}) for (bool terminal : {false, true})
        QTest::newRow(qPrintable(QString("B%1-direct%2-final%3").arg(bpp).arg(direct).arg(terminal)))
            << bpp << direct << terminal;
}

void KisTiledDataManagerTest::testPageStoreHistorySegmentDelivery()
{
    QFETCH(int, bpp); QFETCH(bool, direct); QFETCH(bool, terminal);
    const QByteArray blank(bpp, char(0x31));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    dm.writeBytes(reinterpret_cast<const quint8 *>(blank.constData()), 0, 0, 1, 1);
    dm.writeBytes(reinterpret_cast<const quint8 *>(blank.constData()), 100 * 64, 0, 1, 1);
    auto &protocol=*dm.m_nativeResult;
    auto owner = dm.getMemento(); QVERIFY(dm.beginStrokeMutation(owner));
    const auto write = [&](int first, int last) {
        const auto paint = [&](KisPixelWriteCursor *cursor) {
            for (int col = first; col <= last; ++col) {
                cursor->moveTo(col * 64, 0); if (!cursor->rawData()) return false;
                std::memset(cursor->rawData(), 0x71, size_t(bpp));
            }
            return true;
        };
        const QVector<QRect> rects{QRect(first*64,0,(last-first)*64+1,1),QRect(99*64,64,1,1)};
        return direct ? (protocol.pixelOperation(rects,paint,owner)?KisPageStoreWriteOperationResult::Succeeded:KisPageStoreWriteOperationResult::Failed)
                      : dm.writePageStoreOperation(rects,paint,nullptr,owner);
    };
    QCOMPARE(write(0, 1), KisPageStoreWriteOperationResult::Succeeded);
    QCOMPARE(dm.m_hashTable->numTiles(),2);
    auto tile = dm.getTile(0, 0, false); QVERIFY(tile && tile->lockForRead());
    QCOMPARE(tile->data()[0], quint8(0x31)); tile->unlockForRead();
    const auto install = size_t(KisPageStoreDiagnosticPhase::NativeAdapterInstalled);
    KisPageStoreDiagnosticRecorder recorder(true);
    const auto captures = KisNativeResultTestAccess::state(protocol).logicalViews;
    QVERIFY(terminal ? dm.tryCommit() : dm.checkpointStrokeMutation(owner));
    QCOMPARE(KisNativeResultTestAccess::state(protocol).logicalViews, captures + quint64(terminal)); // redo owns one real after-view
    QCOMPARE(recorder.metrics()[install].workItems, quint64(2));
    QCOMPARE(dm.m_hashTable->numTiles(), 3);
    QVERIFY(tile->lockForRead()); QCOMPARE(tile->data()[0], quint8(0x71)); tile->unlockForRead();
    if (!terminal) {
        QVERIFY(dm.checkpointStrokeMutation(owner));
        QCOMPARE(recorder.metrics()[install].workItems, quint64(2)); // Empty retry does not revisit history D.
        QCOMPARE(write(2, 2), KisPageStoreWriteOperationResult::Succeeded);
        const auto installed = recorder.metrics()[install].workItems;
        QVERIFY(dm.checkpointStrokeMutation(owner));
        QCOMPARE(recorder.metrics()[install].workItems - installed, quint64(1));
        QVERIFY(dm.tryCommit());
    }
    QCOMPARE(owner->extent(), QRect(0, 0, (terminal ? 2 : 3) * 64, 64));
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults, quint64(0));
    dm.rollback(owner);
    bool exists = true; dm.getReadOnlyTileLazy(1, 0, exists); QVERIFY(!exists);
    dm.rollforward(owner); dm.getReadOnlyTileLazy(1, 0, exists); QVERIFY(exists);
    dm.purgeHistory(owner);
}

void KisTiledDataManagerTest::testPageStoreHistorySegmentRefusal_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("failure");
    for (int bpp : {1, 4, 8, 16}) for (int failure : {0, 1, 2})
        QTest::newRow(qPrintable(QString("B%1-failure%2").arg(bpp).arg(failure))) << bpp << failure;
}

void KisTiledDataManagerTest::testPageStoreHistorySegmentRefusal()
{
QFETCH(int,bpp);QFETCH(int,failure);const QByteArray blank(bpp,char(0x31));
    KisDataManager dm(bpp,reinterpret_cast<const quint8 *>(blank.constData()));auto &protocol=*dm.m_nativeResult;
    auto owner=dm.getMemento();QVERIFY(dm.beginStrokeMutation(owner));int calls=0;
    QCOMPARE(dm.writePageStoreOperation({QRect(0,0,65,1)},[&](KisPixelWriteCursor *c){++calls;for(int x:{0,64}){c->moveTo(x,0);auto *p=c->rawData();if(!p)return false;std::memset(p,0x71,bpp);}return true;},nullptr,owner),KisPageStoreWriteOperationResult::Succeeded);
    kisDrainPageStoreReclamation();auto process=kisAcquirePageStoreBootstrapBudget();QVERIFY(process);void *filler=nullptr;size_t count=0;
    const auto free=qScopeGuard([&]{if(filler)freeTestProcessStoragePressure(filler,count,1);});
    const auto fill=[&]{const auto used=process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];count=size_t(kisPageProcessStorageLimit()-used.live.cpuRam-used.reserved.cpuRam);filler=allocateTestProcessStoragePressure(count,1);};
    QString error;bool injected=false;int installs=0;KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](auto phase){if(phase==KisPageStoreDiagnosticPhase::NativeAdapterInstalled)++installs;
        if(failure==2&&!injected&&phase==KisPageStoreDiagnosticPhase::NativeAcceptPrepared){injected=true;fill();}});
    if(failure==0){fill();injected=true;QVERIFY(!dm.checkpointStrokeMutation(owner,&error));QCOMPARE(installs,0);}
    else if(failure==1){QVERIFY(dm.checkpointStrokeMutation(owner,&error));fill();injected=true;
        const auto views=KisNativeResultTestAccess::state(protocol).logicalViews;QVERIFY(dm.checkpointStrokeMutation(owner,&error));
        QCOMPARE(KisNativeResultTestAccess::state(protocol).logicalViews,views);QVERIFY(!protocol.capture());}
    else {QVERIFY(dm.checkpointStrokeMutation(owner,&error));QCOMPARE(installs,1);}
    recorder.setPhaseObserver({});QVERIFY(injected);freeTestProcessStoragePressure(std::exchange(filler,nullptr),count,1);
    quint8 pixel=0;dm.readBytes(&pixel,0,0,1,1);QCOMPARE(pixel,quint8(failure?0x71:0x31));
    QVERIFY(dm.checkpointStrokeMutation(owner,&error));QCOMPARE(calls,1);QVERIFY(dm.tryCommit());dm.rollback(owner);dm.purgeHistory(owner);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
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
    KisNativeResultProtocol::Snapshot frozen; if (cut) { QVERIFY(dm.checkpointStrokeMutation(owner)); frozen = dm.m_nativeResult->capture(); QVERIFY(bool(frozen)); }
    QCOMPARE(write(0x71, false), KisPageStoreWriteOperationResult::Failed);
    QCOMPARE(write(0x91, true), KisPageStoreWriteOperationResult::Failed);
    QVERIFY(!dm.tryCommit()); QVERIFY(!dm.tryCommit()); QVERIFY(!dm.beginStrokeMutation(owner));
    QVERIFY(dm.tryAbort(owner)); QByteArray actual(bpp, 0);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1); QCOMPARE(actual, initial);
    if (cut) {
        auto read = frozen.readPage(0, 0); QVERIFY(bool(read.data()));
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(read.data()), bpp), QByteArray(bpp, char(0x51)));
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
    auto &protocol=*dm.m_nativeResult;
    auto memento = history ? dm.getMemento() : KisMementoSP{};
    bool pinned = heldReader && tile->lockForRead();
    const auto release = qScopeGuard([&] { if (pinned) tile->unlockForRead(); });
    QCOMPARE(pinned, heldReader);
    const auto views = KisNativeResultTestAccess::state(protocol).logicalViews;
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
    QCOMPARE(KisNativeResultTestAccess::state(protocol).logicalViews, views);
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
    const auto warmed=KisNativeResultTestAccess::state(protocol).logicalViews;
    for (int i = 0; i < 1000; ++i) {
        QVERIFY(tile->lockForRead()); tile->unlockForRead();
    }
    QCOMPARE(KisNativeResultTestAccess::state(protocol).logicalViews, warmed);
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
    auto &protocol=*dm.m_nativeResult;
    auto old = protocol.capture(); QVERIFY(old);
    auto owner = dm.getMemento();
    if (operation == 5) QVERIFY(dm.beginStrokeMutation(owner));
    int observed = 0; bool ready = true, extentUnchanged = true;
    const auto verifyPrepared = [&] {
        ++observed;
        ready &= dm.m_extentManager.m_colsData.covers(col, col) && dm.m_extentManager.m_rowsData.covers(row, row);
        extentUnchanged &= dm.extent().isEmpty();
    };
    KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](Phase phase) {
        if (phase == Phase::NativeAcceptPrepared) verifyPrepared();
    });
    QString error;
    {
        KisStrokeJobFailureContext failure(true);
        if (operation == 0 || operation == 5) {
            QCOMPARE(dm.writePageStoreOperation({area, area}, [&](KisPixelWriteCursor *cursor) {
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
    if(operation==5)QVERIFY(dm.checkpointStrokeMutation(owner));
    recorder.setPhaseObserver({});
    QVERIFY(observed > 0); QVERIFY(ready); QVERIFY(extentUnchanged);
    const QRect tile(col * 64, row * 64, 64, 64);
    QCOMPARE(dm.extent(), tile); QVERIFY(dm.tryCommit());
    auto current = protocol.capture(); QVERIFY(current);
    auto actual=current.readPage(col,row);
    auto before=old.readPage(col,row);
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
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31)), painted(bpp, char(0x71));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    KisDataManagerSP source = new KisDataManager(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    const QRect area(-128, -64, 64 * pages, 64);
    const QRect copyRect(area.x() + 3, area.y() + 2, area.width() - 6, 3);
    if (operation >= 2 && operation <= 4) dm.clear(area.x(), area.y(), area.width(), area.height(), reinterpret_cast<const quint8 *>(initial.constData()));
    if (operation == 3 || operation == 4)
        for (int i = 0; i < pages; i += 2)
            source->clear(area.x() + 64 * i, area.y(), 64, 64, reinterpret_cast<const quint8 *>(painted.constData()));
    auto &protocol=*dm.m_nativeResult;
    auto old = protocol.capture(); QVERIFY(old);
    auto owner = dm.getMemento();
    if (operation == 5) QVERIFY(dm.beginStrokeMutation(owner));
    QByteArray packed(area.width() * bpp, char(0x13));
    for (int i = 0; i < pages; i += 2) std::memset(packed.data() + 64 * i * bpp, 0x71, 64 * bpp);
    const auto views=KisNativeResultTestAccess::state(protocol).logicalViews;
    QString error;
    {
        KisStrokeJobFailureContext failure(true);
        if (operation == 0 || operation == 5) {
            QCOMPARE(dm.writePageStoreOperation({area, area}, [&](KisPixelWriteCursor *cursor) {
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
    QCOMPARE(KisNativeResultTestAccess::state(protocol).logicalViews,views);
    QVERIFY(dm.tryCommit());
    auto current = protocol.capture(); QVERIFY(current);
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
        auto read=current.readPage(page.column,page.row); QVERIFY(read.data());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(read.data()), expected.size()), expected);
        auto before=old.readPage(page.column,page.row); QVERIFY(before.data());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(before.data()), expected.size()),
                 QByteArray(expected.size(), char(operation >= 2 && operation <= 4 ? 0x31 : 0x13)));
        // Packed writes establish every addressed page, including default
        // bytes. Cursor declarations alone still do not create untouched pages.
        const bool present = operation == 1 || expected != QByteArray(expected.size(), char(0x13));
        QCOMPARE(bool(dm.m_hashTable->getExistingTile(page.column, page.row)), present);
    }
    const QRect expectedExtent = operation == 2 ? QRect{} :
        operation == 1 || operation == 3 ? area : QRect(area.x(), area.y(), 64 * (pages % 2 ? pages : pages - 1), 64);
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
    auto &protocol=*dm.m_nativeResult;
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    if (history) dm.clear(0, 0, 128, 64, reinterpret_cast<const quint8 *>(prior.constData()));
    auto beforeView = protocol.capture();
    auto foreign=failure==3?protocol.prepareProducer(QRect(64,0,64,64)):KisNativeResultProtocol::Prepared{};
    if(failure==3)QVERIFY(foreign);
    const auto epoch=KisNativeResultTestAccess::state(protocol).acceptedEpoch;
    bool hidden = true, called = false; QString error;
    const auto result = dm.writePageStoreOperation({QRect(0, 0, 128, 2)}, [&](KisPixelWriteCursor *cursor) {
        called = true;
        for (const QPoint p : {QPoint(0,0), QPoint(64,0), QPoint(0,1), QPoint(64,1)}) {
            cursor->moveTo(p.x(), p.y());
            if (!cursor->rawData()) return false;
            memset(cursor->rawData(), 0x71 + p.y(), size_t(bpp));
            auto view = protocol.capture();
            auto read = view.readPage(0,0);
            hidden &= read.data() && static_cast<const quint8 *>(read.data())[0] == quint8(history ? 0x41 : 0x31);
        }
        if (failure == 2) { cursor->moveTo(128,0); hidden &= !cursor->rawData(); cursor->moveTo(0,0); hidden &= !cursor->rawData(); }
        return failure != 1;
    }, &error);
    // A pre-existing foreign claim rejects the entire managed range before
    // the callback can expose even its first pixel.
    QCOMPARE(called, failure != 3);
    QVERIFY(hidden);
    QCOMPARE(result, failure ? KisPageStoreWriteOperationResult::Failed : KisPageStoreWriteOperationResult::Succeeded);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch-epoch,quint64(failure?0:1));
    QByteArray actual(128 * 2 * bpp, Qt::Uninitialized), expected(actual.size(), char(history ? 0x41 : 0x31));
    if (!failure) for (int y = 0; y < 2; ++y) for (int x : {0,64}) memset(expected.data() + (y * 128 + x) * bpp, 0x71 + y, size_t(bpp));
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 128, 2); QCOMPARE(actual, expected);
    auto old = beforeView.readPage(0,0); QVERIFY(old.data());
    QCOMPARE(static_cast<const quint8 *>(old.data())[0], quint8(history ? 0x41 : 0x31));
    old = {}; beforeView = {};
    if(failure==3)protocol.cancel(foreign);
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

void KisTiledDataManagerTest::testPageStorePixelOperationCanonicalRange()
{
    const quint8 blank = 0x13;
    KisDataManager dm(1, &blank);
    auto &protocol=*dm.m_nativeResult;
    using Result = KisPageStoreWriteOperationResult;
    const auto before=KisNativeResultTestAccess::state(protocol);
    // Admission claims the range; physical results are prepared only on access.
    // An invalid access latches failure without materializing untouched pages.
    quint64 prepared = 0;
    QCOMPARE(dm.writePageStoreOperation({QRect(-64, -64, 9 * 64, 8 * 64)},
        [&](KisPixelWriteCursor *cursor) {
            prepared = KisNativeResultTestAccess::state(protocol).physicalResources;
            cursor->moveTo(-65, -64);
            if (cursor->rawData()) return false;
            cursor->moveTo(-64, -64);
            return !cursor->rawData();
        }), Result::Failed);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).physicalResources, prepared);
    QCOMPARE(prepared - before.physicalResources, quint64(0));
    // Reversed and duplicate rectangles cross both the inline index and
    // touched bitmap boundaries. Search every accepted page in reverse order.
    QCOMPARE(dm.writePageStoreOperation({QRect(0, 0, 8 * 64, 7 * 64),
                                         QRect(-64, -64, 9 * 64, 8 * 64),
                                         QRect(-64, -64, 64, 64)},
        [](KisPixelWriteCursor *cursor) {
            for (int row = 6; row >= -1; --row) for (int column = 7; column >= -1; --column) {
                cursor->moveTo(column * 64, row * 64);
                if (!cursor->rawData()) return false;
                *cursor->rawData() = quint8(0x40 + row + column);
            }
            return true;
        }), Result::Succeeded);
    for (int row = -1; row <= 6; ++row) for (int column = -1; column <= 7; ++column) {
        quint8 pixel = 0;
        dm.readBytes(&pixel, column * 64, row * 64, 1, 1);
        QCOMPARE(pixel, quint8(0x40 + row + column));
    }
    QCOMPARE(KisNativeResultTestAccess::state(protocol).physicalResources - prepared, quint64(72));
}

void KisTiledDataManagerTest::testPageStoreClearCompositePressure()
{
    // Match the original sequence under real process payload exhaustion.
    // Full clear can release old pages before the subsequent allocation;
    // partial clear cannot. A refused fusion must retain both behaviors.
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
    for (bool partial : {false, true}) {
        QByteArray reference; bool referenceFailed = false;
        for (bool fused : {false, true}) {
            DefaultBudgetScope budget;
            KisPaintDeviceSP dst = new KisPaintDevice(cs), src = new KisPaintDevice(cs);
            const QRect storage(0, 0, 128, 64);
            const QRect clear = partial ? QRect(7, 9, 91, 37) : storage;
            const KoColor initial(QColor(73, 91, 117, 255), cs), input(QColor(133, 19, 61, 173), cs);
            dst->fill(storage, initial); src->fill(storage, input);
            auto &protocol=*dst->dataManager()->m_nativeResult;
            kisDrainPageStoreReclamation();
            budget.saturate(cs->pixelSize(), initial.data());
            bool failed;
            {
                KisStrokeJobFailureContext failure(true);
                KisPainter painter(dst); painter.setCompositeOpId(COMPOSITE_OVER);
                if (fused) painter.bitBltAfterClear(clear, clear.topLeft(), src, clear);
                else { dst->clear(clear); painter.bitBlt(clear.topLeft(), src, clear); }
                painter.end(); failed = failure.failed();
            }
            QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
            kisDrainPageStoreReclamation();
            const auto usage=protocol.usage();
            for(size_t i=0;i<usage.buckets.size();++i)if(i!=size_t(KisBackingBudgetClass::RetirementDebt))
                QCOMPARE(usage.buckets[i].reserved.cpuRam,quint64(0));
            QCOMPARE(usage.buckets[size_t(KisBackingBudgetClass::RetirementDebt)].reserved.cpuRam,
                     KisNativeResultTestAccess::state(protocol).physicalResources*4096*cs->pixelSize());
            budget.release();
            QByteArray actual(storage.width() * storage.height() * cs->pixelSize(), Qt::Uninitialized);
            dst->readBytes(reinterpret_cast<quint8 *>(actual.data()), storage);
            qInfo() << "clear-composite pressure" << "partial" << partial << "fused" << fused << "failed" << failed;
            if (!fused) { reference = actual; referenceFailed = failed; }
            else { QCOMPARE(failed, referenceFailed); QCOMPARE(actual, reference); }
            // After the pressure is released the original owner is usable.
            KisStrokeJobFailureContext retry(true);
            KisPainter painter(dst); painter.setCompositeOpId(COMPOSITE_OVER);
            painter.bitBltAfterClear(clear, clear.topLeft(), src, clear); painter.end();
            QVERIFY2(!retry.failed(), qPrintable(retry.error()));
        }
        kisDrainPageStoreReclamation();
    }
}

void KisTiledDataManagerTest::testPageStorePixelOperationPreflightBudgetPressure()
{
DefaultBudgetScope budget;const quint8 blank=0x13;KisDataManager dm(1,&blank);auto &protocol=*dm.m_nativeResult;
    auto history=dm.getMemento();QVERIFY(history);QVERIFY(dm.beginStrokeMutation(history));
    using Result=KisPageStoreWriteOperationResult;QString error;
    const auto paint=[&](quint8 first,quint8 second,bool *called){return dm.writePageStoreOperation({QRect(0,0,65,1)},[&](KisPixelWriteCursor *cursor){
        if(called)*called=true;for(int x:{0,64}){cursor->moveTo(x,0);auto *p=cursor->rawData();if(!p)return false;*p=x?second:first;}return true;
    },&error,history);};
    QCOMPARE(dm.writePageStoreOperation({QRect(0,0,1,1)},[](KisPixelWriteCursor *c){c->moveTo(0,0);if(!c->rawData())return false;*c->rawData()=0x31;return true;},&error,history),Result::Succeeded);
    budget.saturate(1,&blank);bool called=false;QCOMPARE(paint(0x51,0x52,&called),Result::Failed);
    // Payload is prepared at the actual access. Once the callback has changed
    // a pending row, failure must latch the whole segment; it cannot be replayed.
    QVERIFY(called);QVERIFY(!error.isEmpty());QVERIFY(!dm.tryCommit(&error));
    quint8 actual=0;dm.readBytes(&actual,0,0,1,1);QCOMPARE(actual,blank);
    QVERIFY(dm.tryAbort(history));budget.release();
    history=dm.getMemento();QVERIFY(dm.beginStrokeMutation(history));called=false;
    QCOMPARE(paint(0x51,0x52,&called),Result::Succeeded);QVERIFY(called);QVERIFY(dm.tryCommit());
    dm.readBytes(&actual,0,0,1,1);QCOMPARE(actual,quint8(0x51));dm.readBytes(&actual,64,0,1,1);QCOMPARE(actual,quint8(0x52));
    dm.rollback(history);dm.readBytes(&actual,0,0,1,1);QCOMPARE(actual,blank);dm.rollforward(history);dm.purgeHistory(history);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
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
QFETCH(int,bpp);QFETCH(int,pages);QFETCH(bool,persistent);QFETCH(int,outcome);
    const QByteArray blank(bpp,char(0x13));KisDataManager dm(bpp,reinterpret_cast<const quint8 *>(blank.constData()));
    auto &protocol=*dm.m_nativeResult;auto old=protocol.capture();QVERIFY(old);
    auto history=persistent?dm.getMemento():KisMementoSP{};if(history)QVERIFY(dm.beginStrokeMutation(history));
    QVector<QRect> targets;QSet<KisLogicalPageId> changed;
    for(int i=0;i<pages;++i){const KisLogicalPageId page{2*i-pages,i%3-1};QRect rect(page.column*64,page.row*64,64,64);
        targets.append(rect);targets.append(rect);if(outcome!=2&&(outcome!=1||i%3==0))changed.insert(page);}
    QString error;int bodies=0;KisPageStoreDiagnosticRecorder recorder(true);
    const auto result=dm.writePageStoreOperation(targets,[&](KisPixelWriteCursor *c){++bodies;
        for(int pass=0;pass<3;++pass)for(int i=pages-1;i>=0;--i){const KisLogicalPageId page{2*i-pages,i%3-1};if(!changed.contains(page))continue;
            c->moveTo(page.column*64+3,page.row*64+2+pass);auto *p=c->rawData();if(!p)return false;std::memset(p,0x61+pass,size_t(bpp));}
        return outcome!=3;
    },&error,history);
    QCOMPARE(bodies,1);QCOMPARE(result,outcome==3?KisPageStoreWriteOperationResult::Failed:KisPageStoreWriteOperationResult::Succeeded);
    if(outcome==3)changed.clear();
    if(persistent){
        auto hidden=protocol.capture();auto read=hidden.readPage(-pages,-1);QVERIFY(read.data());QCOMPARE(read.data()[2*64*bpp+3*bpp],quint8(0x13));
        if(outcome==3)QVERIFY(dm.tryAbort(history));else{
            QCOMPARE(dm.writePageStoreOperation(targets,[](KisPixelWriteCursor *){return true;},&error,history),KisPageStoreWriteOperationResult::Succeeded);
            QVERIFY2(dm.tryCommit(&error),qPrintable(error));
        }
    }
    QCOMPARE(recorder.metrics()[size_t(KisPageStoreDiagnosticPhase::NativeAdapterInstalled)].workItems,quint64(changed.size()));
    auto current=protocol.capture();QVERIFY(current);
    for(int i=0;i<pages;++i){const KisLogicalPageId page{2*i-pages,i%3-1};QByteArray expected(4096*bpp,char(0x13));
        if(changed.contains(page))for(int pass=0;pass<3;++pass)std::memset(expected.data()+((2+pass)*64+3)*bpp,0x61+pass,size_t(bpp));
        auto read=current.readPage(page.column,page.row);QVERIFY(read.data());QCOMPARE(QByteArray(reinterpret_cast<const char *>(read.data()),expected.size()),expected);
        auto previous=old.readPage(page.column,page.row);QVERIFY(previous.data());QCOMPARE(QByteArray(reinterpret_cast<const char *>(previous.data()),expected.size()),QByteArray(expected.size(),char(0x13)));
        QCOMPARE(bool(dm.m_hashTable->getExistingTile(page.column,page.row)),changed.contains(page));
    }
    current={};old={};if(persistent&&outcome!=3)dm.purgeHistory(history);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
    QCOMPARE(protocol.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].reserved.cpuRam,quint64(0));
    QCOMPARE(dm.writePageStoreOperation({targets.front()},[](KisPixelWriteCursor *){return true;}),KisPageStoreWriteOperationResult::Succeeded);
}

void KisTiledDataManagerTest::testPageStoreChangedStorageBudget()
{
DefaultBudgetScope limits;using Result=KisPageStoreWriteOperationResult;
    for(bool persistent:{false,true}){const quint8 blank=0x13;KisDataManager dm(1,&blank);auto &protocol=*dm.m_nativeResult;
        auto history=persistent?dm.getMemento():KisMementoSP{};if(history)QVERIFY(dm.beginStrokeMutation(history));QString error;
        const auto small=[&](int y,quint8 value){return dm.writePageStoreOperation({QRect(0,y,1,1)},[&](KisPixelWriteCursor *c){c->moveTo(0,y);auto *p=c->rawData();if(!p)return false;*p=value;return true;},&error,history);};
        QCOMPARE(small(0,0x47),Result::Succeeded);const auto before=KisNativeResultTestAccess::state(protocol);
        const auto usage=protocol.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];bool called=false;
        // 262656 valid tile positions; metadata, not unsupported coordinates,
        // must reject the declared range before the producer executes.
        QCOMPARE(dm.writePageStoreOperation({QRect(0,0,513*64,512*64)},[&](KisPixelWriteCursor *){called=true;return true;},&error,history),Result::Failed);
        QVERIFY(!called);QVERIFY(!error.isEmpty());QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,before.activeResults);
        const auto after=protocol.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];QVERIFY(after.live.cpuRam<=quint64(8*1024*1024));QCOMPARE(after.reserved.cpuRam,usage.reserved.cpuRam);
        QCOMPARE(small(1,0x61),Result::Succeeded);if(history)QVERIFY(dm.tryCommit());
        auto view=protocol.capture();auto read=view.readPage(0,0);QVERIFY(read.data());QCOMPARE(read.data()[0],quint8(0x47));QCOMPARE(read.data()[64],quint8(0x61));
        read={};view={};if(history)dm.purgeHistory(history);
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
QFETCH(int,bpp);QFETCH(int,pages);QFETCH(bool,partial);const QByteArray blank(bpp,char(0x13));
    KisDataManager dm(bpp,reinterpret_cast<const quint8 *>(blank.constData()));auto &protocol=*dm.m_nativeResult;
    const int width=pages*64,height=partial?1:64,stride=width*bpp+17;QByteArray input(stride*height,char(0x13));
    for(int page=0;page<pages;page+=3)for(int row=0;row<height;++row)std::memset(input.data()+row*stride+page*64*bpp,0x71,size_t(64*bpp));
    QString error;QVERIFY(protocol.write(reinterpret_cast<const quint8 *>(input.constData()),QRect(-64,-64,width,height),stride,&error));
    auto view=protocol.capture();QVERIFY(view);
    for(int page=0;page<pages;++page){QByteArray expected(4096*bpp,char(0x13));if(page%3==0)std::memset(expected.data(),0x71,size_t(height*64*bpp));
        auto read=view.readPage(page-1,-1);QVERIFY(read.data());QCOMPARE(QByteArray(reinterpret_cast<const char *>(read.data()),expected.size()),expected);QVERIFY(dm.m_hashTable->getExistingTile(page-1,-1));}
    const auto before=KisNativeResultTestAccess::state(protocol);
    KisPageStoreDiagnosticRecorder recorder(true);
    QVERIFY(protocol.write(reinterpret_cast<const quint8 *>(input.constData()),QRect(-64,-64,width,height),stride,&error));
    const auto after=KisNativeResultTestAccess::state(protocol);
    QCOMPARE(after.acceptedEpoch,before.acceptedEpoch);
    QCOMPARE(after.physicalResources,before.physicalResources);
    QCOMPARE(recorder.metrics()[size_t(KisPageStoreDiagnosticPhase::NativeAdapterInstalled)].workItems,quint64(0));
}

void KisTiledDataManagerTest::testPageStorePixelOperationBorrowed()
{
    quint8 blank = 0;
    KisDataManager dm(1, &blank);
    bool nestedCalled = false;
    auto &protocol=*dm.m_nativeResult;auto enclosing=protocol.prepareProducer(QRect(0,0,64,64));QVERIFY(enclosing);
    QCOMPARE(dm.writePageStoreOperation({QRect(0,0,128,2)},[&](KisPixelWriteCursor *){nestedCalled=true;return true;}),KisPageStoreWriteOperationResult::Failed);
    QVERIFY(!nestedCalled);protocol.cancel(enclosing);
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
                 KisPageStoreWriteOperationResult::Succeeded);
        QVERIFY(iteratorCalled);
        QCOMPARE(iterator.rawData(), iteratorPointer);
    }
    auto tile = dm.getTile(0,0,true); tile->lockForWrite();
    auto *pointer = tile->data(); QVERIFY(pointer); *pointer = 0x31;
    bool called = false;
    QCOMPARE(dm.writePageStoreOperation({QRect(0,0,128,2)}, [&](KisPixelWriteCursor *) { called = true; return true; }),
        KisPageStoreWriteOperationResult::Succeeded);
    QVERIFY(called); QCOMPARE(tile->data(), pointer); *pointer = 0x41; tile->unlockForWrite();
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
    auto &protocol=*dm.m_nativeResult;
    auto foreignTile = dm.getTile(2,0,true); foreignTile->lockForWrite();
    const auto beforeWrites=KisNativeResultTestAccess::state(protocol).acceptedEpoch;
    bool locked = true;
    const auto cleanup = qScopeGuard([&] { if (locked) foreignTile->unlockForWrite(); });
    auto *pointer = foreignTile->data(); QVERIFY(pointer); *pointer = 0x77;
    // This raw writer is independent of the following operations. A completed
    // operation must already be canonical before another one can fail.
    QCOMPARE(dm.writePageStoreOperation({QRect(0,0,1,1)}, [](KisPixelWriteCursor *cursor) {
        cursor->moveTo(0,0); if (!cursor->rawData()) return false; *cursor->rawData() = 0x51; return true;
    }), KisPageStoreWriteOperationResult::Succeeded);
    auto published = protocol.capture();
    auto completed = published.readPage(0,0);
    QVERIFY(completed.data());
    QCOMPARE(static_cast<const quint8 *>(completed.data())[0], quint8(0x51));
    if (compatibilityBatch) {
        auto batch=protocol.beginIterator();QVERIFY(batch);
        auto lease = KisNativeResultTestAccess::tiles(protocol).acquireTile(1,0,KisTilePageStoreBridge::Access::Write); QVERIFY(lease); QVERIFY(lease->tileData());
        lease->tileData()->data()[0] = 0x62; lease->markDirty(); QVERIFY(lease->finish()); lease.reset();
        batch.reset();
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
    QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch-beforeWrites, quint64(2));
}

void KisTiledDataManagerTest::testPageStoreAdapterPreparationRefusal_data()
{
    QTest::addColumn<bool>("persistent"); QTest::addColumn<bool>("funded");
    QTest::addColumn<bool>("copy");
    for (bool persistent : {false, true}) for (bool funded : {false, true}) for (bool copy : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("persistent%1-funded%2-copy%3").arg(persistent).arg(funded).arg(copy)))
            << persistent << funded << copy;
}

void KisTiledDataManagerTest::testPageStoreAdapterPreparationRefusal()
{
    QFETCH(bool,persistent);QFETCH(bool,funded);QFETCH(bool,copy);
    const quint8 blank=0,value=0x72;KisDataManager dm(1,&blank);KisTiledDataManager source(1,&blank);
    source.clear(QRect(64,0,64,64),&value);
    auto &protocol=*dm.m_nativeResult;
    auto history=persistent?dm.getMemento():KisMementoSP{};
    if(history)QVERIFY(dm.beginStrokeMutation(history));
    QString error;
    auto paint=[](KisPixelWriteCursor *cursor){cursor->moveTo(0,0);if(!cursor->rawData())return false;*cursor->rawData()=0x41;return true;};
    QCOMPARE(dm.writePageStoreOperation({QRect(0,0,1,1)},paint,&error,history),KisPageStoreWriteOperationResult::Succeeded);
    kisDrainPageStoreReclamation();auto process=kisAcquirePageStoreBootstrapBudget();
    size_t count=0;void *filler=nullptr;bool injected=false,pixels=false;
    const auto release=qScopeGuard([&]{if(filler)freeTestProcessStoragePressure(filler,count,1);});
    const auto saturate=[&]{injected=true;const auto used=process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        count=size_t(kisPageProcessStorageLimit()-used.live.cpuRam-used.reserved.cpuRam);filler=allocateTestProcessStoragePressure(count,1);};
    KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](auto phase){if(funded&&!injected&&phase==KisPageStoreDiagnosticPhase::NativeAcceptPrepared)saturate();});
    if(!funded)saturate();
    bool succeeded=false;
    if(copy){KisStrokeJobFailureContext failure(true);dm.KisTiledDataManager::bitBlt(&source,QRect(64,0,64,64));succeeded=!failure.failed();}
    else {
        succeeded=dm.writePageStoreOperation({QRect(64,0,1,1)},[&](KisPixelWriteCursor *cursor){pixels=true;cursor->moveTo(64,0);if(!cursor->rawData())return false;*cursor->rawData()=value;return true;},&error,history)==KisPageStoreWriteOperationResult::Succeeded;
        if(succeeded&&history)QVERIFY(dm.checkpointStrokeMutation(history,&error));
    }
    recorder.setPhaseObserver({});if(filler)freeTestProcessStoragePressure(std::exchange(filler,nullptr),count,1);
    // Public semantic copy has no explicit owner while a stroke is open.
    QCOMPARE(succeeded,funded&&!(persistent&&copy));
    if(!(funded&&persistent&&copy))QVERIFY(injected);
    if(!funded&&!copy)QVERIFY(!pixels);
    QCOMPARE(dm.writePageStoreOperation({QRect(0,0,1,1)},paint,&error,history),KisPageStoreWriteOperationResult::Succeeded);
    if(history)QVERIFY2(dm.tryCommit(&error),qPrintable(error));
    quint8 actual=0;dm.readBytes(&actual,0,0,1,1);QCOMPARE(actual,quint8(0x41));
    dm.readBytes(&actual,64,0,1,1);QCOMPARE(actual,quint8(succeeded?value:blank));
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
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
    auto &protocol=*dm.m_nativeResult;
    bool aborted = false, prepared = false; QString error;
    KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase == (packed?KisPageStoreDiagnosticPhase::PackedWritePrepare:KisPageStoreDiagnosticPhase::PixelOperationRangePrepare) && !aborted)
            aborted = dm.tryAbort(history, &error);
        if (phase == KisPageStoreDiagnosticPhase::NativeAcceptPrepared) {
            // Abort replaced both original adapter structures before admission.
            // The new extent capacity must now be ready while counts stay empty.
            prepared = aborted && KisNativeResultTestAccess::state(protocol).activeResults == 1 &&
                dm.m_extentManager.m_colsData.m_capacity > 256 && dm.extent().isEmpty();
        }
    });
    if(packed)aborted=dm.tryAbort(history,&error);
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

void KisTiledDataManagerTest::testPageStoreRestoreSparseDelivery_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("pages"); QTest::addColumn<bool>("defaults"); QTest::addColumn<bool>("intent");
    for (int bpp : {1, 4, 8, 16}) for (int pages : {17, 257}) for (bool defaults : {false, true}) for (bool intent : {false, true})
        QTest::newRow(qPrintable(QString("B%1-N%2-default%3-intent%4").arg(bpp).arg(pages).arg(defaults).arg(intent))) << bpp << pages << defaults << intent;
}

void KisTiledDataManagerTest::testPageStoreRestoreSparseDelivery()
{
    QFETCH(int, bpp); QFETCH(int, pages); QFETCH(bool, defaults); QFETCH(bool, intent);
    const QByteArray blank(bpp, char(0x11)), base(bpp, char(0x31)), first(bpp, char(0x51)), second(bpp, char(0x71)), other(bpp, char(0x21));
    const auto bytes = [](const QByteArray &data) { return reinterpret_cast<const quint8 *>(data.constData()); };
    KisTiledDataManager dm(bpp, bytes(blank));
    dm.clear(QRect(0, 0, pages * 64, 64), bytes(base));
    const auto m1 = dm.getMemento();
    dm.clear(QRect(0, 0, 64, 64), bytes(first));
    dm.clear(QRect(64, 0, 64, 64), bytes(blank));
    dm.clear(QRect((pages + 300) * 64, 0, 64, 64), bytes(first));
    if (defaults) dm.setDefaultPixel(bytes(other));
    QVERIFY(dm.tryCommit());
    const auto m2 = dm.getMemento();
    dm.clear(QRect(128, 0, 64, 64), bytes(second)); QVERIFY(dm.tryCommit());
    KisTileSP unmaterialized = intent ? dm.getTile(-600, 0, true) : KisTileSP{};
    bool present = false;
    auto unchanged = dm.getReadOnlyTileLazy(3, 0, present); QVERIFY(present);
    auto held = dm.getReadOnlyTileLazy(2, 0, present); QVERIFY(present); QVERIFY(held->lockForRead());
    const auto unlock = qScopeGuard([&] { held->unlockForRead(); });
    auto &protocol=*dm.m_nativeResult;KisNativeResultProtocol::ReadAccess late;
    bool injected=false;KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](auto phase){if(!defaults||injected||phase!=KisPageStoreDiagnosticPhase::NativeAcceptPrepared)return;
        injected=true;auto snapshot=protocol.capture();late=snapshot.readPage(-700,0);
    });
    dm.rollback(m1);
    recorder.setPhaseObserver({});
    QCOMPARE(recorder.metrics()[size_t(KisPageStoreDiagnosticPhase::ManifestExport)].intervals, quint64(0));
    QCOMPARE(dm.m_hashTable->numTiles(), pages);
    QCOMPARE(dm.extent(), QRect(0, 0, pages * 64, 64));
    QCOMPARE(dm.m_hashTable->getExistingTileForPreparedUpdate(3, 0), unchanged);
    QVERIFY(!dm.m_hashTable->getExistingTileForPreparedUpdate(-600, 0));
    if (unmaterialized) {
        QVERIFY(unmaterialized->lockForRead());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(unmaterialized->data()), bpp), defaults ? other : blank);
        unmaterialized->unlockForRead();
    }
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(held->data()), bpp), second);
    const auto check = [&](int col, const QByteArray &expected) {
        QByteArray actual(bpp, 0); dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), col * 64, 0, 1, 1);
        QCOMPARE(actual, expected);
    };
    check(0, base); check(1, base); check(2, base); check(pages + 300, blank); check(-700, blank);
    if (defaults) {
        QVERIFY(injected); QVERIFY(late.data());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(late.data()), bpp), other);
        late={};
    }
    auto restored = dm.m_hashTable->getExistingTileForPreparedUpdate(0, 0);
    dm.rollback(m1); // same content, new epoch: no rebuild or wrapper invalidation
    QCOMPARE(dm.m_hashTable->getExistingTileForPreparedUpdate(0, 0), restored);
    dm.rollforward(m2);
    check(0, first); check(1, defaults ? other : blank); check(2, second); check(pages + 300, first);
    QCOMPARE(dm.m_hashTable->getExistingTileForPreparedUpdate(3, 0), unchanged);
    dm.rollback(m1);
    dm.clear(QRect(128, 0, 1, 1), bytes(first)); check(2, first);
    dm.purgeHistory(m2); dm.purgeHistory(m1);
}

void KisTiledDataManagerTest::testPageStoreRestoreDeliveryRefusal()
{
const quint8 blank=0x11,value=0x51;KisTiledDataManager dm(1,&blank);auto &protocol=*dm.m_nativeResult;
    dm.clear(QRect(0,0,128,64),&value);const auto history=dm.getMemento();dm.clear(QRect(0,0,64,64),&blank);QVERIFY(dm.tryCommit());
    const auto epoch=KisNativeResultTestAccess::state(protocol).acceptedEpoch;
    auto claimed=protocol.prepareProducer(QRect(0,0,64,64));QVERIFY(claimed);QString error;
    QVERIFY(!protocol.restoreHistory(history,true,&error));QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch,epoch);
    QCOMPARE(dm.m_hashTable->numTiles(),1);QVERIFY(!dm.m_hashTable->getExistingTileForPreparedUpdate(0,0));QCOMPARE(dm.extent(),QRect(64,0,64,64));
    protocol.cancel(claimed);QVERIFY(protocol.restoreHistory(history,true,&error));QCOMPARE(dm.m_hashTable->numTiles(),2);QCOMPARE(dm.extent(),QRect(0,0,128,64));
    dm.purgeHistory(history);
}

void KisTiledDataManagerTest::testPageStoreRestoreDeliveryAllowsTileReader()
{
    const quint8 blank = 0x11, initial = 0x31, changed = 0x51;
    KisTiledDataManager dm(1, &blank);
    dm.clear(QRect(0, 0, 64, 64), &initial);
    const auto history = dm.getMemento();
    dm.clear(QRect(0, 0, 64, 64), &changed); QVERIFY(dm.tryCommit());
    auto tile = dm.m_hashTable->getExistingTileForPreparedUpdate(0, 0); QVERIFY(tile);
    struct ReaderBridge : KisTilePageStoreBridge {
        KisTilePageStoreBridge *backend = nullptr;
        QSemaphore entered;
        quint8 observed = 0;
        bool finished = false;
        TileLease acquireTile(qint32 col, qint32 row, Access access, TileLease *cache) override {
            // Called by the real Tile read while its swap barrier is held.
            entered.release();
            auto lease = backend->acquireTile(col, row, access, cache);
            if (lease && lease->tileData()) {
                observed = lease->tileData()->data()[0]; finished = lease->finish();
            }
            // Refuse the outer pin after testing real backend selection; the
            // manager can then reconnect its normal bridge to an idle tile.
            return {};
        }
    } bridge;
    bridge.backend=&KisNativeResultTestAccess::tiles(*dm.m_nativeResult);
    tile->setPageStoreBridge(&bridge, false); tile->invalidatePageStoreReadCache();
    QSemaphore startReader;
    std::atomic<bool> readRequested{false};
    bool entered = false, refused = false;
    // Allocate the witness before the funded installation window. The phase
    // callback only releases this already existing reader.
    std::thread reader([&] {
        startReader.acquire();
        if (readRequested.load()) refused = !tile->lockForRead();
    });
    const auto joinReader = qScopeGuard([&] {
        startReader.release();
        if (reader.joinable()) reader.join();
    });
    KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::NativeSelectionInstalled || readRequested.exchange(true)) return;
        startReader.release();
        entered = bridge.entered.tryAcquire(1, 5000);
    });
    dm.rollback(history);
    recorder.setPhaseObserver({}); if (reader.joinable()) reader.join();
    QVERIFY(entered); QVERIFY(refused); QVERIFY(bridge.finished); QCOMPARE(bridge.observed, initial);
    dm.purgeHistory(history);
}

void KisTiledDataManagerTest::testPageStoreRestoreReconcilesBackendPages()
{
    const quint8 blank = 0x11, initial = 0x31, changed = 0x51;
    KisTiledDataManager dm(1, &blank);
    dm.clear(QRect(0, 0, 64, 64), &initial);
    // Native direct and manager consumers install through the same original
    // adapter cut; there is no canonical-only page hidden from its table.
    QVERIFY(dm.m_nativeResult->fill(QRect(5 * 64, 0, 64, 64), QByteArray(1, char(initial))));
    const auto history = dm.getMemento();
    dm.clear(QRect(0, 0, 64, 64), &changed);
    QVERIFY(dm.tryCommit());
    auto intent = dm.getTile(9, 0, true); QVERIFY(intent);
    // Page 5 already exists; the unmaterialized page 9 must be removed.
    QCOMPARE(dm.m_hashTable->numTiles(), 3);
    dm.rollback(history);
    QCOMPARE(dm.m_hashTable->numTiles(), 2);
    QVERIFY(dm.m_hashTable->getExistingTileForPreparedUpdate(5, 0));
    QVERIFY(!dm.m_hashTable->getExistingTileForPreparedUpdate(9, 0));
    QCOMPARE(dm.extent(), QRect(0, 0, 6 * 64, 64));
    quint8 actual = 0; dm.readBytes(&actual, 5 * 64, 0, 1, 1); QCOMPARE(actual, initial);
    dm.purgeHistory(history);
}

void KisTiledDataManagerTest::testPageStoreRestoreLateAdmission()
{
const quint8 blank=0,initial=0x31,changed=0x71;KisDataManager dm(1,&blank);auto &protocol=*dm.m_nativeResult;
    QVERIFY(protocol.fill(QRect(0,0,64,64),QByteArray(1,char(initial))));auto retained=protocol.capture();QVERIFY(retained);
    QVERIFY(protocol.fill(QRect(0,0,64,64),QByteArray(1,char(changed))));
    auto existing=protocol.prepareProducer(QRect(128,0,64,64));QVERIFY(existing);QVERIFY(!protocol.restore(retained));protocol.cancel(existing);
    bool observed=false,refused=false;KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](auto phase){if(observed||phase!=KisPageStoreDiagnosticPhase::NativeAcceptPrepared)return;observed=true;
        auto late=protocol.prepareProducer(QRect(192,0,64,64));refused=!late;
    });
    QVERIFY(protocol.restore(retained));recorder.setPhaseObserver({});QVERIFY(observed);QVERIFY(refused);
    quint8 actual=0;dm.readBytes(&actual,0,0,1,1);QCOMPARE(actual,initial);QCOMPARE(dm.extent(),QRect(0,0,64,64));
    QVERIFY(protocol.fill(QRect(192,0,64,64),QByteArray(1,char(changed))));QVERIFY(protocol.restore(retained));
    dm.readBytes(&actual,192,0,1,1);QCOMPARE(actual,blank);QCOMPARE(dm.extent(),QRect(0,0,64,64));
}

void KisTiledDataManagerTest::testPageStoreAdapterBarrier_data()
{
    QTest::addColumn<int>("barrier");
    QTest::addColumn<bool>("copy");
    for (int barrier : {0, 1, 2, 3, 4}) for (bool copy : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("barrier%1-copy%2").arg(barrier).arg(copy))) << barrier << copy;
}

void KisTiledDataManagerTest::testPageStoreAdapterBarrier()
{
    QFETCH(int, barrier); QFETCH(bool, copy);
    const quint8 blank = 0; KisDataManager dm(1, &blank);
    KisDataManager source(1, &blank);
    const quint8 value = 0x59;
    if (copy) source.clear(0, 0, 64, 64, &value);
    auto &protocol=*dm.m_nativeResult;
    const auto retained = protocol.capture(); QVERIFY(bool(retained));
    const auto history = barrier == 2 || barrier == 3 ? dm.getMemento() : KisMementoSP{};
    bool observed = false, rejected = false; QString error;
    KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        const auto deliveryPhase = KisPageStoreDiagnosticPhase::NativeAdapterInstalled;
        if (phase != deliveryPhase || observed) return;
        observed = true;
        if (barrier == 0) {
            KisStrokeJobFailureContext failure(true); dm.clear(); rejected = failure.failed();
        } else if (barrier == 1) rejected = !protocol.restore(retained);
        else if (barrier == 2) rejected = !dm.tryCommit(&error);
        else if (barrier == 3) rejected = !dm.tryAbort(history, &error);
        else { KisStrokeJobFailureContext failure(true); dm.setExtent(QRect(0, 0, 0, 0)); rejected = failure.failed(); }
    });
    if (copy) {
        KisStrokeJobFailureContext failure(true);
        dm.KisTiledDataManager::bitBlt(&source, QRect(0, 0, 64, 64));
        QVERIFY2(!failure.failed(), qPrintable(failure.error()));
    } else {
        QCOMPARE(dm.writePageStoreOperation({QRect(0, 0, 1, 1)}, [](KisPixelWriteCursor *cursor) {
            cursor->moveTo(0, 0); if (!cursor->rawData()) return false;
            *cursor->rawData() = 0x59; return true;
        }, &error), KisPageStoreWriteOperationResult::Succeeded);
    }
    recorder.setPhaseObserver({});
    QVERIFY(observed); QVERIFY(rejected);
    QCOMPARE(dm.m_hashTable->numTiles(), 1);
    quint8 actual = 0; dm.readBytes(&actual, 0, 0, 1, 1); QCOMPARE(actual, quint8(0x59));
    if (barrier == 0) { dm.clear(); QCOMPARE(dm.m_hashTable->numTiles(), 0); }
    else if (barrier == 1) QVERIFY(protocol.restore(retained));
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
    auto &protocol=*dm.m_nativeResult;
    auto before = protocol.capture(); QVERIFY(bool(before));
    auto history = mode == 2 ? dm.getMemento() : KisMementoSP{};
    if (history) QVERIFY(dm.beginStrokeMutation(history));
    bool observed = false, retained = false, independent = false, checkpointRejected = false;
    KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        const auto deliveryPhase = KisPageStoreDiagnosticPhase::NativeAdapterInstalled;
        if (phase != deliveryPhase || observed) return;
        observed = true;
        retained=KisNativeResultTestAccess::state(protocol).activeResults>=1;
        QString error;bool called=false;
        const auto blocked=dm.writePageStoreOperation({QRect(0,0,1,1)},[&](KisPixelWriteCursor *){called=true;return true;},&error,history);
        retained&=blocked==KisPageStoreWriteOperationResult::Failed&&!called;called=false;
        const auto unrelated=dm.writePageStoreOperation({QRect(640,0,1,1)},[&](KisPixelWriteCursor *){called=true;return true;},&error,history);
        independent=history?(unrelated==KisPageStoreWriteOperationResult::Failed&&!called):(unrelated==KisPageStoreWriteOperationResult::Succeeded&&called);
        if(history)checkpointRejected=!dm.checkpointStrokeMutation(history,&error);
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
    QCOMPARE(success,outcome!=2);
    if(history){if(outcome==2)QVERIFY(dm.tryAbort(history,&error));else QVERIFY(dm.checkpointStrokeMutation(history,&error));}
    recorder.setPhaseObserver({});
    QCOMPARE(observed,outcome==0||(mode==1&&outcome==1));
    if(observed){QVERIFY(retained);QVERIFY(independent);if(history)QVERIFY(checkpointRejected);}
    if(history&&outcome!=2)QVERIFY(dm.tryCommit(&error));
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
    const bool establishesPages = outcome == 0 || (mode == 1 && outcome == 1);
    QCOMPARE(dm.m_hashTable->numTiles(), establishesPages ? 2 : 0);
    QCOMPARE(dm.extent(), establishesPages ? QRect(0, 0, 128, 64) : QRect());
    QByteArray pixel(bpp, Qt::Uninitialized);
    for (int x : {0, 64}) {
        dm.readBytes(reinterpret_cast<quint8 *>(pixel.data()), x, 0, 1, 1);
        QCOMPARE(pixel, QByteArray(bpp, char(outcome == 0 ? 0x71 : 0x13)));
        auto old=before.readPage(x/64,0);
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
QFETCH(bool,packed);const quint8 blank=0x13,value=0x71;KisDataManager dm(1,&blank);auto &protocol=*dm.m_nativeResult;
    QSemaphore waiting,done;std::thread waiter;bool observed=false,blocked=false,admitted=false,sameRefused=false;
    KisNativeResultProtocol::Snapshot retained;QString error;
    const auto join=qScopeGuard([&]{if(waiter.joinable())waiter.join();});
    KisPageStoreDiagnosticRecorder recorder(true);recorder.setPhaseObserver([&](auto phase){
        if(observed||phase!=KisPageStoreDiagnosticPhase::NativeAdapterInstalled)return;observed=true;retained=protocol.capture();
        bool called=false;sameRefused=dm.writePageStoreOperation({QRect(0,0,1,1)},[&](KisPixelWriteCursor *){called=true;return true;})==KisPageStoreWriteOperationResult::Failed&&!called;
        waiter=std::thread([&]{KisPageStoreDiagnosticRecorder other(true);other.setPhaseObserver([&](auto stage){if(stage==KisPageStoreDiagnosticPhase::PixelOperationRangeWait)waiting.release();});
            admitted=dm.writePageStoreOperation({QRect(0,0,1,1)},[](KisPixelWriteCursor *c){c->moveTo(0,0);auto *p=c->rawData();if(!p)return false;*p=0x73;return true;})==KisPageStoreWriteOperationResult::Succeeded;done.release();
        });blocked=waiting.tryAcquire(1,5000)&&!done.tryAcquire();
    });
    if(packed){KisStrokeJobFailureContext failure(true);dm.writeBytes(&value,0,0,1,1);QVERIFY2(!failure.failed(),qPrintable(failure.error()));}
    else QCOMPARE(dm.writePageStoreOperation({QRect(0,0,1,1)},[](KisPixelWriteCursor *c){c->moveTo(0,0);if(!c->rawData())return false;*c->rawData()=0x71;return true;},&error),KisPageStoreWriteOperationResult::Succeeded);
    recorder.setPhaseObserver({});if(waiter.joinable())waiter.join();QVERIFY(observed);QVERIFY(blocked);QVERIFY(admitted);QVERIFY(sameRefused);
    auto old=retained.readPage(0,0);QVERIFY(old.data());QCOMPARE(old.data()[0],value);quint8 actual=0;dm.readBytes(&actual,0,0,1,1);QCOMPARE(actual,quint8(0x73));
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));old={};retained={};QVERIFY(protocol.close());
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
    auto &protocol=*dm.m_nativeResult;
    auto before=protocol.capture();QVERIFY(bool(before));
    auto memento=history?dm.getMemento():KisMementoSP{};
    QString error;const QByteArray first(bpp,char(0x61));
    auto prepared=protocol.prepareWrite(reinterpret_cast<const quint8 *>(first.constData()),QRect(0,0,1,1),bpp,&error);
    QVERIFY2(bool(prepared),qPrintable(error));
    QSemaphore waiting, done;
    bool succeeded = false, lateExcluded = false;
    const QRect area = partial ? QRect(1, 1, 126, 62) : QRect(0, 0, 128, 64);
    const QByteArray value = remove ? blank : next;
    std::thread clearer([&] {
        KisPageStoreDiagnosticRecorder recorder(true);
        recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
            if (phase == KisPageStoreDiagnosticPhase::PixelOperationRangeWait) waiting.release();
            if(phase==KisPageStoreDiagnosticPhase::NativeAcceptPrepared&&!lateExcluded){
                auto late=protocol.prepareProducer(QRect(64,0,1,1));lateExcluded=!late;
            }
        });
        KisStrokeJobFailureContext failure(true);
        dm.clear(area.x(), area.y(), area.width(), area.height(),
                 reinterpret_cast<const quint8 *>(value.constData()));
        succeeded = !failure.failed(); done.release();
    });
    const auto join = qScopeGuard([&] { protocol.cancel(prepared); if (clearer.joinable()) clearer.join(); });
    const bool blocked = waiting.tryAcquire(1, 500);
    const bool premature = done.tryAcquire();
    // Admission protects only the overlapping pages. An unrelated operation
    // must finish while the original ready result remains claimed.
    int independentCalls = 0;
    const auto independent = dm.writePageStoreOperation({QRect(128, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
        ++independentCalls; cursor->moveTo(128, 0); if (!cursor->rawData()) return false;
        std::memset(cursor->rawData(), 0x51, size_t(bpp)); return true;
    }, &error);
    const bool accepted=protocol.accept(prepared,&error); clearer.join();
    QVERIFY2(accepted,qPrintable(error));
    QVERIFY(blocked); QVERIFY(!premature); QVERIFY(succeeded); QVERIFY(lateExcluded);
    QCOMPARE(independent, KisPageStoreWriteOperationResult::Succeeded);
    QCOMPARE(independentCalls, 1);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
    QByteArray expected(192 * 64 * bpp, char(0x31)), actual(expected.size(), Qt::Uninitialized);
    std::memset(expected.data(), partial ? 0x61 : value[0], size_t(bpp));
    std::memset(expected.data() + 128 * bpp, 0x51, size_t(bpp));
    for (int y = area.top(); y <= area.bottom(); ++y)
        std::memset(expected.data() + (y * 192 + area.x()) * bpp, value[0], size_t(area.width() * bpp));
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 192, 64);
    QCOMPARE(actual, expected);
    // Fixed before-images survive clear and the earlier accepted write.
    auto old = before.readPage(0,0); QVERIFY(old.data());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(old.data()), 64*64*bpp), QByteArray(64 * 64 * bpp, char(0x31)));
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
    QFETCH(int,mode);QFETCH(int,legacy);
    const quint8 blank=0x13,initial=0x31,value=0x71;
    KisDataManager dm(1,&blank);dm.clear(0,0,128,64,&initial);
    auto &protocol=*dm.m_nativeResult;auto before=protocol.capture();QVERIFY(bool(before));
    auto history=mode==2?dm.getMemento():KisMementoSP{};if(history)QVERIFY(dm.beginStrokeMutation(history));
    QSemaphore reached;std::thread writer;bool observed=false,settled=false,rejected=false,intentLocked=false;QString error;
    const auto join=qScopeGuard([&]{if(writer.joinable())writer.join();});
    KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](auto phase){
        if(observed||phase!=KisPageStoreDiagnosticPhase::NativeAdapterInstalled)return;observed=true;
        writer=std::thread([&]{KisStrokeJobFailureContext failure(true);QByteArray input(65,char(0x72));
            KisPageStoreDiagnosticRecorder nested(true);nested.setPhaseObserver([&](auto p){if(p==KisPageStoreDiagnosticPhase::PixelOperationRangeWait)reached.release();});
            KisTileSP intent;if(legacy==2){intent=dm.getTile(1,0,true);intentLocked=intent&&intent->lockForWrite();}
            if(legacy==0)dm.setPixel(64,0,reinterpret_cast<const quint8 *>(input.constData()));
            else if(legacy==1)dm.writePlanarBytes({reinterpret_cast<quint8 *>(input.data())},{1},0,0,65,1);
            else if(intentLocked)dm.writeBytes(reinterpret_cast<const quint8 *>(input.constData()),0,0,65,1);
            if(intent&&intentLocked)intent->unlockForWrite();rejected=failure.failed()||(legacy==2&&!intentLocked);reached.release();
        });
        settled=reached.tryAcquire(1,5000);
    });
    if(mode==1){KisStrokeJobFailureContext failure(true);dm.writeBytes(&value,64,0,1,1);QVERIFY(!failure.failed());}
    else QCOMPARE(dm.writePageStoreOperation({QRect(64,0,1,1)},[&](KisPixelWriteCursor *cursor){cursor->moveTo(64,0);if(!cursor->rawData())return false;*cursor->rawData()=value;return true;},&error,history),KisPageStoreWriteOperationResult::Succeeded);
    if(history)QVERIFY(dm.checkpointStrokeMutation(history,&error));
    recorder.setPhaseObserver({});if(writer.joinable())writer.join();QVERIFY(observed);QVERIFY(settled);
    const bool queuedPacked=legacy==2&&mode!=2;
    QCOMPARE(rejected,!queuedPacked);
    if(queuedPacked)QVERIFY(intentLocked);
    if(history)QVERIFY(dm.tryCommit(&error));
    quint8 actual[65];dm.readBytes(actual,0,0,65,1);
    for(int x=0;x<64;++x)QCOMPARE(actual[x],quint8(queuedPacked?0x72:initial));
    QCOMPARE(actual[64],quint8(queuedPacked?0x72:value));
    auto old=before.readPage(1,0);QVERIFY(old.data());QCOMPARE(old.data()[0],initial);old={};before={};
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
    quint8 next=0x73;{KisStrokeJobFailureContext failure(true);dm.setPixel(64,0,&next);QVERIFY(!failure.failed());}
    dm.readBytes(actual,64,0,1,1);QCOMPARE(actual[0],next);
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
    auto *protocol = dm.m_nativeResult.get();
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
        KisPageStoreDiagnosticRecorder recorder(true);
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
    auto view = protocol->capture();
    auto read = view.readPage(0, 0);
    const bool hidden = read.data() && *static_cast<const quint8 *>(read.data()) == initial;
    read = {}; view = {};
    releaseFirst.release(); first.join(); second.join();
    QVERIFY(waited); QVERIFY(hidden); QVERIFY(firstPixels && secondPixels);
    QCOMPARE(firstResult, cancelFirst ? Result::Failed : Result::Succeeded);
    QCOMPARE(secondResult, Result::Succeeded);
    quint8 actual[128]; dm.readBytes(actual,0,0,128,1);
    QCOMPARE(actual[0], quint8(secondKind == 2 ? initial : 0x72));
    QCOMPARE(actual[64], quint8(cancelFirst ? initial : 0x51));
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).activeResults, quint64(0));
}

void KisTiledDataManagerTest::testNativeRangeWaitIsAtomicAndCloses()
{
    const quint8 blank = 0x13;
    KisTiledDataManager dm(1, &blank);
    auto &protocol = *dm.m_nativeResult;
    QSemaphore firstInside, releaseFirst, secondWaiting, secondDone;
    using Result = KisPageStoreWriteOperationResult;
    Result firstResult = Result::Failed, secondResult = Result::Succeeded;
    int secondCalls = 0;
    std::thread first([&] {
        firstResult = dm.writePageStoreOperation({QRect(64, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(64, 0); if (!cursor->rawData()) return false;
            *cursor->rawData() = 0x51;
            firstInside.release(); releaseFirst.acquire(); return true;
        });
    });
    const bool entered = firstInside.tryAcquire(1, 5000);
    if (!entered) { releaseFirst.release(); first.join(); QVERIFY(entered); return; }
    std::thread second([&] {
        KisPageStoreDiagnosticRecorder recorder(true);
        recorder.setPhaseObserver([&](auto phase) {
            if (phase == KisPageStoreDiagnosticPhase::PixelOperationRangeWait) secondWaiting.release();
        });
        secondResult = dm.writePageStoreOperation({QRect(0, 0, 1, 1), QRect(64, 0, 1, 1)},
            [&](KisPixelWriteCursor *) { ++secondCalls; return true; });
        secondDone.release();
    });
    const bool waited = secondWaiting.tryAcquire(1, 5000);
    // The waiting range includes page 0, but cannot hold it hostage while
    // another page is busy. A truly independent result must still complete.
    const auto independent = dm.writePageStoreOperation({QRect(0, 0, 1, 1)}, [](KisPixelWriteCursor *cursor) {
        cursor->moveTo(0, 0); if (!cursor->rawData()) return false;
        *cursor->rawData() = 0x72; return true;
    });
    const bool deferredClose = !protocol.close();
    const bool wokeOnClose = secondDone.tryAcquire(1, 5000);
    releaseFirst.release(); first.join(); second.join();
    QVERIFY(waited); QCOMPARE(independent, Result::Succeeded);
    QVERIFY(deferredClose); QVERIFY(wokeOnClose);
    QCOMPARE(secondCalls, 0); QCOMPARE(secondResult, Result::Failed); QCOMPARE(firstResult, Result::Failed);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults, quint64(0));
    QVERIFY(protocol.close());
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
    auto *protocol = dm.m_nativeResult.get(); QVERIFY(protocol);
    auto beforeView = protocol->capture();
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
    const auto before = KisNativeResultTestAccess::state(*protocol);
    dm.writePlanarBytes(planes, channels, area.x(), area.y(), area.width(), area.height());
    const auto after = KisNativeResultTestAccess::state(*protocol);
    const bool noOp = partial && bpp == 1;
    QCOMPARE(after.acceptedEpoch - before.acceptedEpoch, quint64(noOp ? 0 : 1));
    QCOMPARE(after.activeResults, quint64(0));
    QByteArray actual(original.size(), Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), area.x(), area.y(), area.width(), area.height());
    QCOMPARE(actual, expected);
    // Retained root remains the pre-operation image even after the new root.
    auto old = beforeView.readPage(-1, 0); QVERIFY(old.data());
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
        QCOMPARE(KisNativeResultTestAccess::state(*dm.m_nativeResult).activeResults, quint64(0));
        QByteArray actual(expected.size(), 0);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), area.x(), area.y(), area.width(), area.height());
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
        const auto stats = KisNativeResultTestAccess::state(*dm.m_nativeResult);
        QCOMPARE(stats.pageSlots, quint64(0)); QCOMPARE(stats.acceptedEpoch, quint64(1));
        QCOMPARE(stats.activeResults, quint64(0));
        QCOMPARE(dm.extent(), QRect());
    }
}

void KisTiledDataManagerTest::testPageStorePlanarWriteNoOpInputs()
{
    const quint8 blank[4] = {0x13, 0x13, 0x13, 0x13};
    KisDataManager dm(4, blank);
    const auto before = KisNativeResultTestAccess::state(*dm.m_nativeResult);
    quint8 byte = 0;
    dm.writePlanarBytes({nullptr, nullptr}, {1, 3}, -65, -33, 131, 99);
    dm.writePlanarBytes({&byte, nullptr}, {0, 4}, -65, -33, 131, 99);
    dm.writePlanarBytes({}, {}, 0, 0, 0, 99);
    dm.writePlanarBytes({}, {}, 0, 0, 99, -1);
    const auto after = KisNativeResultTestAccess::state(*dm.m_nativeResult);
    QCOMPARE(after.pageSlots, before.pageSlots);
    QCOMPARE(after.acceptedEpoch, before.acceptedEpoch);
    QCOMPARE(after.activeResults, before.activeResults);
    QCOMPARE(after.physicalResources, before.physicalResources);
    QCOMPARE(after.logicalViews, before.logicalViews);
    QCOMPARE(dm.extent(), QRect());
}

void KisTiledDataManagerTest::testPageStoreTrimMutation()
{
    QFETCH(int, bpp); QFETCH(bool, history); QFETCH(QRect, kept);
    const QByteArray blank(bpp, char(0x13)), color(bpp, char(0x47));
    KisDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    const QRect area(-64, -64, 256, 192);
    dm.clear(area.x(), area.y(), area.width(), area.height(), reinterpret_cast<const quint8 *>(color.constData()));
    auto &protocol=*dm.m_nativeResult;
    auto beforeView = protocol.capture();
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    const auto epoch=KisNativeResultTestAccess::state(protocol).acceptedEpoch;
    quint64 partialPages = 0;
    for (int y = area.top(); y <= area.bottom(); y += 64) for (int x = area.left(); x <= area.right(); x += 64) {
        const QRect page(x, y, 64, 64); const QRect overlap = page.intersected(kept);
        if (!overlap.isEmpty() && overlap != page) ++partialPages;
    }
    dm.setExtent(kept);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch-epoch,quint64(!kept.contains(area)));
    auto selected=protocol.capture();quint64 changedKept=0;
    for(int y=area.top();y<=area.bottom();y+=64)for(int x=area.left();x<=area.right();x+=64){
        const QRect tile(x,y,64,64);if(tile.intersects(kept)&&!(selected.versionAt(x/64,y/64)==beforeView.versionAt(x/64,y/64)))++changedKept;
    }
    QCOMPARE(changedKept,partialPages);selected={};
    QByteArray expected(area.width() * area.height() * bpp, char(0x13));
    for (int y = 0; y < area.height(); ++y) for (int x = 0; x < area.width(); ++x)
        if (kept.contains(area.topLeft() + QPoint(x, y))) memset(expected.data() + (y * area.width() + x) * bpp, 0x47, size_t(bpp));
    QByteArray actual(expected.size(), Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), area.x(), area.y(), area.width(), area.height());
    QCOMPARE(actual, expected);
    auto old = beforeView.readPage(0,0); QVERIFY(old.data());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(old.data()), 4096*bpp), QByteArray(4096*bpp, char(0x47)));
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

void KisTiledDataManagerTest::testPageStoreTrimPreparedDelivery_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("pages");
    QTest::addColumn<bool>("history"); QTest::addColumn<bool>("changed");
    for (int bpp : {1, 4, 8, 16}) for (int pages : {17, 257})
        for (bool history : {false, true}) for (bool changed : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2-history%3-changed%4").arg(bpp).arg(pages).arg(history).arg(changed)))
                << bpp << pages << history << changed;
}

void KisTiledDataManagerTest::testPageStoreTrimPreparedDelivery()
{
    QFETCH(int, bpp); QFETCH(int, pages); QFETCH(bool, history); QFETCH(bool, changed);
    const QByteArray blank(bpp, char(0x13)), color(bpp, char(0x57));
    auto bytes = [](const QByteArray &p) { return reinterpret_cast<const quint8 *>(p.constData()); };
    KisTiledDataManager dm(bpp, bytes(blank));
    for (int i = 0; i < pages; ++i) dm.clear(QRect(2 * i * 64, 0, 64, 64), bytes(color));
    auto &protocol=*dm.m_nativeResult;
    QVERIFY(protocol.fill(QRect(3 * 64, 0, 64, 64), color)); // Direct producer, inside the retained range.
    // A public explicit default intent is a real additional selection.
    // Direct native pages already have their adapter in the same cut.
    auto extra=dm.getTile(2*pages+3,0,true);QVERIFY(extra);
    QCOMPARE(dm.m_hashTable->numTiles(),pages+2);
    auto unchanged = dm.m_hashTable->getExistingTileForPreparedUpdate(2, 0); QVERIFY(unchanged);
    auto held = dm.getTile(0, 0, false); QVERIFY(held && held->lockForRead());
    bool locked = true;
    const auto unlock = qScopeGuard([&] { if (locked) held->unlockForRead(); });
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    const QRect kept = changed ? QRect(1, 0, (2 * pages - 3) * 64 - 1, 64)
                               : QRect(0, 0, (2 * pages - 1) * 64, 64);
    KisPageStoreDiagnosticRecorder recorder(true);
    {
        KisStrokeJobFailureContext failure(true);
        dm.setExtent(kept); QVERIFY2(!failure.failed(), qPrintable(failure.error()));
    }
    QCOMPARE(recorder.metrics()[size_t(KisPageStoreDiagnosticPhase::ManifestExport)].intervals, quint64(0));
    QCOMPARE(dm.m_hashTable->numTiles(), pages + (changed ? 0 : 1));
    QVERIFY(!dm.m_hashTable->getExistingTileForPreparedUpdate(2 * pages + 3, 0));
    QVERIFY(dm.m_hashTable->getExistingTileForPreparedUpdate(3, 0));
    QCOMPARE(dm.m_hashTable->getExistingTileForPreparedUpdate(2, 0), unchanged);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(held->data()), bpp), color);
    held->unlockForRead(); locked = false;
    QByteArray actual(bpp, 0);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1);
    QCOMPARE(actual, changed ? blank : color);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 3 * 64, 0, 1, 1); QCOMPARE(actual, color);
    if (history) {
        QVERIFY(dm.tryCommit()); dm.rollback(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1); QCOMPARE(actual, color);
        dm.rollforward(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1); QCOMPARE(actual, changed ? blank : color);
        dm.purgeHistory(memento);
    }
}

void KisTiledDataManagerTest::testPageStoreTrimOriginalRows_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("history");
    for (int bpp : {1, 4, 8, 16}) for (bool history : {false, true})
        QTest::newRow(qPrintable(QString("%1-history%2").arg(bpp).arg(history))) << bpp << history;
}

void KisTiledDataManagerTest::testPageStoreTrimOriginalRows()
{
    QFETCH(int, bpp); QFETCH(bool, history);
    const QByteArray blank(bpp, char(0x13)), color(bpp, char(0x57));
    auto bytes = [](const QByteArray &p) { return reinterpret_cast<const quint8 *>(p.constData()); };
    KisTiledDataManager dm(bpp, bytes(blank));
    auto &protocol=*dm.m_nativeResult;
    // Direct and manager-created partial/unchanged/removal rows share one
    // adapter cut. Each actual changed row is installed once.
    QVERIFY(protocol.fill(QRect(0, 0, 128, 64), color));
    dm.clear(QRect(128, 0, 128, 64), bytes(color));
    QVERIFY(dm.m_hashTable->getExistingTileForPreparedUpdate(0, 0));
    QVERIFY(dm.m_hashTable->getExistingTileForPreparedUpdate(1, 0));
    auto warm = dm.m_hashTable->getExistingTileForPreparedUpdate(2, 0); QVERIFY(warm);
    auto before = protocol.capture(); QVERIFY(bool(before));
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    KisPageStoreDiagnosticRecorder recorder(true);
    {
        KisStrokeJobFailureContext failure(true);
        dm.setExtent(QRect(1, 0, 129, 64));
        QVERIFY2(!failure.failed(), qPrintable(failure.error()));
    }
    QCOMPARE(recorder.metrics()[size_t(KisPageStoreDiagnosticPhase::NativeAdapterInstalled)].workItems,quint64(3));
    auto selected=protocol.capture();QVERIFY(!(selected.versionAt(0,0)==before.versionAt(0,0)));QVERIFY(!(selected.versionAt(2,0)==before.versionAt(2,0)));
    QCOMPARE(selected.versionAt(1,0),before.versionAt(1,0));selected={};
    QCOMPARE(dm.m_hashTable->numTiles(), 3);
    QCOMPARE(dm.m_hashTable->getExistingTileForPreparedUpdate(2, 0), warm);
    QVERIFY(!dm.m_hashTable->getExistingTileForPreparedUpdate(3, 0));
    QByteArray actual(256 * bpp, Qt::Uninitialized), expected(256 * bpp, char(0x13));
    memset(expected.data() + bpp, 0x57, size_t(129 * bpp));
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 256, 1);
    QCOMPARE(actual, expected);
    auto old = before.readPage(0,0); QVERIFY(old.data());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(old.data()), 4096*bpp),
             QByteArray(4096*bpp, char(0x57)));
    old = {}; before = {};
    if (history) {
        QVERIFY(dm.tryCommit()); dm.rollback(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 256, 1);
        QCOMPARE(actual, QByteArray(actual.size(), char(0x57)));
        dm.rollforward(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 256, 1);
        QCOMPARE(actual, expected); dm.purgeHistory(memento);
    }
}

void KisTiledDataManagerTest::testPageStoreTrimPreparationRefusal()
{
    const quint8 blank=0x13,color=0x57;KisTiledDataManager dm(1,&blank);auto &protocol=*dm.m_nativeResult;
    dm.clear(QRect(-64,0,128,64),&color);auto before=protocol.capture();QVERIFY(before);
    QVERIFY(!protocol.fill(QRect(0x7fff*64,0,64,64),QByteArray(1,char(color))));
    auto unchanged=dm.m_hashTable->getExistingTileForPreparedUpdate(-1,0);QVERIFY(unchanged);
    const auto epoch=KisNativeResultTestAccess::state(protocol).acceptedEpoch;kisDrainPageStoreReclamation();
    auto process=kisAcquirePageStoreBootstrapBudget();const auto used=process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
    const auto bytes=size_t(kisPageProcessStorageLimit()-used.live.cpuRam-used.reserved.cpuRam);void *filler=allocateTestProcessStoragePressure(bytes,1);
    const auto release=qScopeGuard([&]{if(filler)freeTestProcessStoragePressure(filler,bytes,1);});QVERIFY(filler);
    QString error;QVERIFY(!protocol.trim(QRect(1,1,62,62),&error));QVERIFY(!error.isEmpty());
    QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch,epoch);QCOMPARE(dm.m_hashTable->getExistingTileForPreparedUpdate(-1,0),unchanged);
    QCOMPARE(dm.extent(),QRect(-64,0,128,64));freeTestProcessStoragePressure(std::exchange(filler,nullptr),bytes,1);
    QVERIFY(protocol.trim(QRect(0,0,64,64)));QCOMPARE(dm.extent(),QRect(0,0,64,64));
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
    KisTiledDataManager dm(1,&blank),source(1,&blank,entry==8?KisTiledDataManager::SelectionAuthority::ExternalProtocol:KisTiledDataManager::SelectionAuthority::NativeResult);
    dm.clear(0, 0, 128, 64, &initial);
    source.clear(0, 0, 128, 64, &replacement);
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    if (history) dm.clear(0, 0, 128, 64, &staged);
    auto &protocol=*dm.m_nativeResult;auto held=protocol.prepareProducer(QRect(64,0,64,64));QVERIFY(held);
    const auto epoch=KisNativeResultTestAccess::state(protocol).acceptedEpoch;
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
        case 8: dm.bitBlt(&source,area); break;
        }
        QVERIFY(failure.failed());
        // Later void calls in the same failed job cannot publish more pixels.
        dm.clear(0, 0, 64, 64, &replacement);
        dm.setDefaultPixel(&replacement);
        QCOMPARE(*dm.defaultPixel(), blank);
    }
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(1));
    QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch,epoch);
    QByteArray actual(128 * 64, Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 128, 64);
    QCOMPARE(actual, QByteArray(actual.size(), char(history ? staged : initial)));
    protocol.cancel(held);
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
    auto &protocol=*dm.m_nativeResult;
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    if (history) dm.clear(0, 0, width, 64, &staged);
    auto held=protocol.prepareProducer(QRect((mixed?2:1)*64,0,64,64));QVERIFY(held);
    const auto epoch=KisNativeResultTestAccess::state(protocol).acceptedEpoch;
    if (planar) {
        QByteArray input(128 * 64, char(0x71));
        KisStrokeJobFailureContext failure(true);
        dm.writePlanarBytes({reinterpret_cast<quint8 *>(input.data())}, {1}, 0, 0, 128, 64);
        QVERIFY(failure.failed());
    } else {
        QString error;
        QVERIFY(!protocol.trim(QRect(mixed ? 65 : 1, 1, 126, 62), &error)); QVERIFY(!error.isEmpty());
    }
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(1));
    QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch,epoch);
    QByteArray actual(width * 64, Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, width, 64);
    QCOMPARE(actual, QByteArray(actual.size(), char(history ? staged : initial)));
    protocol.cancel(held);QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
    if (history) {
        dm.commit(); dm.rollback(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, width, 64);
        QCOMPARE(actual, QByteArray(actual.size(), char(initial)));
        dm.rollforward(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, width, 64);
        QCOMPARE(actual, QByteArray(actual.size(), char(staged))); dm.purgeHistory(memento);
    }
}

void KisTiledDataManagerTest::testTileRawAccessRejectedWrite_data()
{
    QTest::addColumn<int>("bpp");
    QTest::addColumn<int>("entry");
    QTest::addColumn<bool>("batch");
    QTest::addColumn<bool>("pressure");
    const QStringList entries{"data", "tryWriteData", "tileData", "setData"};
    for (int bpp : {1, 4, 8, 16})
        for (int entry = 0; entry < entries.size(); ++entry)
            for (bool batch : {false, true})
                for (bool pressure : {false, true})
                    QTest::newRow(qPrintable(QString("bpp%1-%2-batch%3-pressure%4")
                        .arg(bpp).arg(entries[entry]).arg(batch).arg(pressure)))
                        << bpp << entry << batch << pressure;
}

void KisTiledDataManagerTest::testTileRawAccessRejectedWrite()
{
    QFETCH(int, bpp);
    QFETCH(int, entry);
    QFETCH(bool, batch);
    QFETCH(bool, pressure);
    DefaultBudgetScope budget;
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31));
    const QByteArray replacement(bpp * KisTileData::WIDTH * KisTileData::HEIGHT, char(0x71));
    const auto bytes = [](const QByteArray &value) {
        return reinterpret_cast<const quint8 *>(value.constData());
    };
    KisTiledDataManager dm(bpp, bytes(blank));
    dm.writeBytes(bytes(initial), 0, 0, 1, 1);
    auto &protocol=*dm.m_nativeResult;
    auto before = protocol.capture();
    QVERIFY(bool(before));
    auto tile = dm.getTile(0, 0, true);
    QVERIFY(tile);
    auto exact = before.readPage(0,0);
    QVERIFY(exact.data());
    const QByteArray original(reinterpret_cast<const char *>(exact.data()), replacement.size());
    const auto transactions = KisNativeResultTestAccess::state(protocol).acceptedEpoch;
    auto scope = batch ? dm.beginIteratorWriteScope() : nullptr;
    QVERIFY(!batch || (scope && scope->isActive()));

    auto held=pressure?KisNativeResultProtocol::Prepared{}:protocol.prepareProducer(QRect(0,0,64,64));QVERIFY(pressure||held);
    const auto releaseForeign=qScopeGuard([&]{protocol.cancel(held);});
    QVERIFY(tile->lockForWrite());
    if (pressure) budget.saturate(bpp, bytes(blank));

    bool rejected = false;
    {
        const auto unlock = qScopeGuard([&] { tile->unlockForWrite(); });
        switch (entry) {
        case 0: rejected = !tile->data(); break;
        case 1: rejected = !tile->tryWriteData(); break;
        case 2: rejected = !tile->tileData(); break;
        case 3:
            tile->setData(bytes(replacement));
            rejected = QByteArray(reinterpret_cast<const char *>(exact.data()), original.size()) == original;
            break;
        }
    }
    scope.reset();
    budget.release();
    protocol.cancel(held);
    QVERIFY2(rejected, "Rejected canonical write exposed or modified its before-image");
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(exact.data()), original.size()), original);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch, transactions);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));


    // The refused raw access leaves no pin/claim behind. A later operation
    // writes its own generation while the original exact reader stays valid.
    QVERIFY(tile->lockForWrite());
    {
        const auto unlock = qScopeGuard([&] { tile->unlockForWrite(); });
        auto *output = tile->tryWriteData();
        QVERIFY(output);
        std::memcpy(output, replacement.constData(), size_t(bpp));
    }
    QByteArray actual(bpp, Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1);
    QCOMPARE(actual, replacement.left(bpp));
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(exact.data()), original.size()), original);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
}

void KisTiledDataManagerTest::testTileCompressionRejectedWrite_data()
{
    QTest::addColumn<int>("bpp");
    QTest::addColumn<bool>("legacy");
    QTest::addColumn<bool>("pressure");
    for (int bpp : {1, 4, 8, 16})
        for (bool legacy : {false, true})
            for (bool pressure : {false, true})
                QTest::newRow(qPrintable(QString("bpp%1-legacy%2-pressure%3")
                    .arg(bpp).arg(legacy).arg(pressure))) << bpp << legacy << pressure;
}

void KisTiledDataManagerTest::testTileCompressionRejectedWrite()
{
    QFETCH(int, bpp);
    QFETCH(bool, legacy);
    QFETCH(bool, pressure);
    DefaultBudgetScope budget;
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31));
    const auto bytes = [](const QByteArray &value) {
        return reinterpret_cast<const quint8 *>(value.constData());
    };
    KisTiledDataManager dm(bpp, bytes(blank));
    dm.writeBytes(bytes(initial), 0, 0, 1, 1);
    auto tile = dm.getTile(0, 0, true);
    QVERIFY(tile);
    std::unique_ptr<KisAbstractTileCompressor> compressor = legacy
        ? std::unique_ptr<KisAbstractTileCompressor>(new KisLegacyTileCompressor())
        : std::unique_ptr<KisAbstractTileCompressor>(new KisTileCompressor2());
    KoStoreFake input;
    KisFakePaintDeviceWriter inputWriter(&input);
    QVERIFY(compressor->writeTile(tile, inputWriter));
    input.startReading();
    auto &protocol=*dm.m_nativeResult;
    auto before = protocol.capture();
    auto exact = before.readPage(0,0);
    QVERIFY(exact.data());
    const QByteArray original(reinterpret_cast<const char *>(exact.data()),
        bpp * KisTileData::WIDTH * KisTileData::HEIGHT);
    const auto transactions = KisNativeResultTestAccess::state(protocol).acceptedEpoch;
    auto held=pressure?KisNativeResultProtocol::Prepared{}:protocol.prepareProducer(QRect(0,0,64,64));QVERIFY(pressure||held);
    const auto releaseForeign=qScopeGuard([&]{protocol.cancel(held);});
    QVERIFY(tile->lockForWrite());
    if (pressure) budget.saturate(bpp, bytes(blank));

    // An outer intent makes the nested serializer a raw write consumer too.
    // Both codecs must return refusal and release only their own nested lock.
    {
        const auto unlock = qScopeGuard([&] { tile->unlockForWrite(); });
        KoStoreFake refused;
        KisFakePaintDeviceWriter refusedWriter(&refused);
        QVERIFY(!compressor->writeTile(tile, refusedWriter));
        QVERIFY(!compressor->readTile(input.device(), &dm));
        QVERIFY(tile->hasPageStoreWriteIntent());
    }
    budget.release();
    protocol.cancel(held);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(exact.data()), original.size()), original);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch, transactions);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
    QVERIFY(!tile->hasPageStoreWriteIntent());

    input.startReading();
    QVERIFY(compressor->readTile(input.device(), &dm));
    QByteArray actual(bpp, Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1);
    QCOMPARE(actual, initial);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(exact.data()), original.size()), original);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
}

void KisTiledDataManagerTest::testPageStoreParkedBatchReadCapability_data()
{
    QTest::addColumn<int>("bpp");
    QTest::addColumn<bool>("history");
    for (int bpp : {1, 4, 8, 16})
        for (bool history : {false, true})
            QTest::newRow(qPrintable(QString("bpp%1-history%2").arg(bpp).arg(history)))
                << bpp << history;
}

void KisTiledDataManagerTest::testPageStoreParkedBatchReadCapability()
{
    QFETCH(int, bpp);
    QFETCH(bool, history);
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31));
    const QByteArray staged(bpp, char(0x71)), final(bpp, char(0x72));
    const auto bytes = [](const QByteArray &value) {
        return reinterpret_cast<const quint8 *>(value.constData());
    };
    KisTiledDataManager dm(bpp, bytes(blank));
    dm.writeBytes(bytes(initial), 0, 0, 1, 1);
    auto &protocol=*dm.m_nativeResult;
    auto before = protocol.capture();
    auto exact = before.readPage(0,0);
    QVERIFY(exact.data());
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    const auto epoch=KisNativeResultTestAccess::state(protocol).acceptedEpoch;
    auto scope = dm.beginIteratorWriteScope();
    QVERIFY(scope && scope->isActive());
    auto tile = dm.getTile(0, 0, true);
    QVERIFY(tile && tile->lockForWrite());
    {
        const auto unlock = qScopeGuard([&] { tile->unlockForWrite(); });
        auto *data = tile->tryWriteData();
        QVERIFY(data);
        std::memcpy(data, staged.constData(), size_t(bpp));
    }

    // A foreign read selects the accepted before-image, while the original
    // iterator can still reacquire its private target. Neither leaks the other.
    bool foreignLocked=false;QByteArray foreignBytes;
    std::thread foreign([&] {
        foreignLocked = tile->lockForRead();
        if(foreignLocked){foreignBytes=QByteArray(reinterpret_cast<const char *>(tile->data()),bpp);tile->unlockForRead();}
    });
    foreign.join();
    QVERIFY(foreignLocked);QCOMPARE(foreignBytes,initial);

    const auto captures=KisNativeResultTestAccess::state(protocol).logicalViews;
    QVERIFY(tile->lockForRead());
    {
        const auto unlock = qScopeGuard([&] { tile->unlockForRead(); });
        auto *live = tile->data();
        QVERIFY(live);
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(live), bpp), staged);
        QVERIFY(tile->lockForWrite());
        {
            const auto unlockWrite = qScopeGuard([&] { tile->unlockForWrite(); });
            QCOMPARE(tile->tryWriteData(), live);
            std::memcpy(live, final.constData(), size_t(bpp));
        }
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(live), bpp), final);
    }
    QCOMPARE(KisNativeResultTestAccess::state(protocol).logicalViews,captures);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(exact.data()), bpp), initial);
    QVERIFY(scope->finish());
    scope.reset();
    QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch,epoch+1);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
    QByteArray actual(bpp, Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1);
    QCOMPARE(actual, final);
    if (history) {
        dm.commit();
        dm.rollback(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1);
        QCOMPARE(actual, initial);
        dm.rollforward(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1);
        QCOMPARE(actual, final);
        dm.purgeHistory(memento);
    }
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(exact.data()), bpp), initial);
}

void KisTiledDataManagerTest::testPageStoreBatchCachePreparationFailure()
{
    const quint8 blank = 0x13, initial = 0x31;
    KisTiledDataManager dm(1, &blank);
    dm.writeBytes(&initial, 0, 0, 1, 1);
    auto &protocol=*dm.m_nativeResult;auto &bridge=KisNativeResultTestAccess::tiles(protocol);
    auto before = protocol.capture();
    auto exact = before.readPage(0,0);
    QVERIFY(exact.data());
    kisDrainPageStoreReclamation();
    auto scope = dm.beginIteratorWriteScope();
    QVERIFY(scope && scope->isActive());
    auto warm = bridge.acquireTile(0, 0, KisTilePageStoreBridge::Access::Write);
    QVERIFY(warm && warm->writable());
    warm->tileData()->data()[0] = 0x71;
    warm->markDirty();
    QVERIFY(warm->finish());
    warm.reset();
    const auto resources=KisNativeResultTestAccess::state(protocol).physicalResources;

    // Global pressure needs a global cleanup boundary, including detached
    // root/task storage whose final free is outside this store's idle count.
    kisDrainPageStoreReclamation();
    auto process = kisAcquirePageStoreBootstrapBudget();
    QVERIFY(process);
    const auto used = [&] {
        const auto metadata = process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        return metadata.live.cpuRam + metadata.reserved.cpuRam;
    };
    const size_t bytes = size_t(kisPageProcessStorageLimit() - used());
    void *filler = allocateTestProcessStoragePressure(bytes, 1);
    const auto free = qScopeGuard([&] {
        if (filler) freeTestProcessStoragePressure(filler, bytes, 1);
    });
    QCOMPARE(used(), kisPageProcessStorageLimit());
    TileLease cache;
    auto refused = bridge.acquireTile(0, 0, KisTilePageStoreBridge::Access::Write, &cache);
    QVERIFY(!refused);
    QVERIFY(!cache);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).physicalResources,resources);
    QCOMPARE(static_cast<const quint8 *>(exact.data())[0], initial);
    freeTestProcessStoragePressure(std::exchange(filler, nullptr), bytes, 1);
    // Once the target was prepared, failure to transfer its capability must
    // cancel the original segment. Freeing capacity cannot silently publish it.
    QVERIFY(!scope->finish());
    scope.reset();
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
    quint8 current = 0;
    dm.readBytes(&current, 0, 0, 1, 1);
    QCOMPARE(current, initial);
    const quint8 recovered = 0x72;
    dm.writeBytes(&recovered, 0, 0, 1, 1);
    dm.readBytes(&current, 0, 0, 1, 1);
    QCOMPARE(current, recovered);
    QCOMPARE(static_cast<const quint8 *>(exact.data())[0], initial);
}

void KisTiledDataManagerTest::testPageStoreTileSelectionFailure_data()
{
    QTest::addColumn<int>("bpp");
    QTest::addColumn<bool>("allocated");
    QTest::addColumn<bool>("abort");
    QTest::addColumn<bool>("writable");
    for (int bpp : {1, 4, 8, 16})
        for (bool allocated : {false, true})
            for (bool abort : {false, true})
                for (bool writable : {false, true})
                    QTest::newRow(qPrintable(QString("bpp%1-allocated%2-abort%3-writable%4")
                        .arg(bpp).arg(allocated).arg(abort).arg(writable)))
                        << bpp << allocated << abort << writable;
}

void KisTiledDataManagerTest::testPageStoreTileSelectionFailure()
{
    QFETCH(int, bpp);
    QFETCH(bool, allocated);
    QFETCH(bool, abort);
    QFETCH(bool, writable);
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31)), changed(bpp, char(0x72));
    const auto bytes = [](const QByteArray &value) {
        return reinterpret_cast<const quint8 *>(value.constData());
    };
    KisTiledDataManager dm(bpp, bytes(allocated ? blank : initial));
    if (allocated) dm.writeBytes(bytes(initial), 0, 0, 1, 1);
    const auto memento = dm.getMemento();
    QVERIFY(memento);
    dm.writeBytes(bytes(changed), 0, 0, 1, 1);
    auto &protocol=*dm.m_nativeResult;
    kisDrainPageStoreReclamation();
    // Other stores and detached root jobs share the process capacity too.
    // Drain their physical frees before computing an exactly full filler.
    kisDrainPageStoreReclamation();
    auto process = kisAcquirePageStoreBootstrapBudget();
    QVERIFY(process);
    const auto used = [&] {
        const auto metadata = process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        return metadata.live.cpuRam + metadata.reserved.cpuRam;
    };
    const size_t capacity = size_t(kisPageProcessStorageLimit() - used());
    void *filler = allocateTestProcessStoragePressure(capacity, 1);
    const auto free = qScopeGuard([&] {
        if (filler) freeTestProcessStoragePressure(filler, capacity, 1);
    });
    QCOMPARE(used(), kisPageProcessStorageLimit());
    bool present = true;
    bool reported = false;
    KisTileSP escaped;
    {
        KisStrokeJobFailureContext failure(true);
        escaped = writable ? dm.getTile(0, 0, true) : dm.getOldTile(0, 0, present);
        reported = failure.failed();
    }
    freeTestProcessStoragePressure(std::exchange(filler, nullptr), capacity, 1);

    // Existing writable intent and retained old backing require no new
    // budgeted metadata capability. Exact selection remains valid under pressure.
    bool oldPresent = !allocated;
    auto old = dm.getOldTile(0, 0, oldPresent);
    QVERIFY(old);
    QCOMPARE(oldPresent, allocated);
    QVERIFY(old->lockForRead());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(old->data()), bpp), initial);
    old->unlockForRead();
    auto current = dm.getTile(0, 0, true);
    QVERIFY(current);
    QVERIFY(current->lockForRead());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(current->data()), bpp), changed);
    current->unlockForRead();
    if (abort) QVERIFY(dm.tryAbort(memento));
    else QVERIFY(dm.tryCommit());
    dm.purgeHistory(memento);
    kisDrainPageStoreReclamation();
    QVERIFY(old->lockForRead());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(old->data()), bpp), initial);
    old->unlockForRead();
    QByteArray actual(bpp, Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1);
    QCOMPARE(actual, abort ? initial : changed);

    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
    if (!writable && escaped) {
        QVERIFY(escaped->lockForRead());
        const QByteArray rebound(reinterpret_cast<const char *>(escaped->data()), bpp);
        escaped->unlockForRead();
        if (!abort) QCOMPARE(rebound, initial);
    }
    QVERIFY(escaped);QVERIFY(!reported);
    if(!writable)QCOMPARE(present,allocated); // Retained backing needs no new metadata capability.
}

void KisTiledDataManagerTest::testPageStoreOldTileOwnerLifetime_data()
{
    QTest::addColumn<int>("bpp");
    for (int bpp : {1, 4, 8, 16})
        QTest::newRow(qPrintable(QString("bpp%1").arg(bpp))) << bpp;
}

void KisTiledDataManagerTest::testPageStoreOldTileOwnerLifetime()
{
    QFETCH(int, bpp);
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31)), changed(bpp, char(0x72));
    auto dm = std::make_unique<KisTiledDataManager>(bpp,
        reinterpret_cast<const quint8 *>(blank.constData()));
    dm->writeBytes(reinterpret_cast<const quint8 *>(initial.constData()), 0, 0, 1, 1);
    const auto memento = dm->getMemento();
    QVERIFY(memento);
    dm->writeBytes(reinterpret_cast<const quint8 *>(changed.constData()), 0, 0, 1, 1);
    auto old = dm->getOldTile(0, 0);
    QVERIFY(old);
    QVERIFY(dm->tryCommit());
    dm->purgeHistory(memento);
    dm.reset();
    kisDrainPageStoreReclamation();
    // A G0 wrapper keeps its immutable storage, not a borrowed manager.
    // Writer refusal must happen before creating an intent or returning data.
    QVERIFY(old->lockForRead());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(old->data()), bpp), initial);
    old->unlockForRead();
    const bool writable = old->lockForWrite();
    if (writable) old->unlockForWrite();
    QVERIFY(!writable);
    QVERIFY(old->lockForRead());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(old->data()), bpp), initial);
    old->unlockForRead();
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
    auto &protocol=*dm.m_nativeResult;auto before=protocol.capture();QVERIFY(before);
    auto held=protocol.prepareProducer(QRect(64,0,64,64));QVERIFY(held);
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
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(1));
    auto old = before.readPage(1,0);
    QVERIFY(old.data());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(old.data()) + (64 + 1) * bpp, bpp), initial);
    old = {};
    protocol.cancel(held);
    QVERIFY(dm.tryAbort(memento));
    QByteArray actual(bpp, Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 65, 1, 1, 1);
    QCOMPARE(actual, initial);
}

void KisTiledDataManagerTest::testPageStoreClearPreparedDelivery_data()
{
    QTest::addColumn<bool>("history");
    QTest::addColumn<bool>("remnants");
    QTest::addColumn<int>("count");
    for (bool history : {false, true}) for (bool remnants : {false, true}) for (int count : {0, 1, 17})
        QTest::newRow(qPrintable(QStringLiteral("history%1-remnants%2-count%3")
            .arg(history).arg(remnants).arg(count))) << history << remnants << count;
}

void KisTiledDataManagerTest::testPageStoreClearPreparedDelivery()
{
    QFETCH(bool,history);QFETCH(bool,remnants);QFETCH(int,count);const quint8 blank=0x13,initial=0x71;KisTiledDataManager dm(1,&blank);
    dm.clear(0,0,count*64,64,&initial);auto &protocol=*dm.m_nativeResult;auto *table=dm.m_hashTable;auto retained=protocol.capture();QVERIFY(retained);
    KisTileSP reader;if(count){reader=dm.getTile(0,0,false);QVERIFY(reader&&reader->lockForRead());}const auto unlock=qScopeGuard([&]{if(reader)reader->unlockForRead();});
    if(remnants&&count)QVERIFY(protocol.remove(QRect(0,0,count*64,64)));
    const auto owner=history?dm.getMemento():KisMementoSP{};if(history)QVERIFY(owner);
    const auto extent=dm.extent();const auto epoch=KisNativeResultTestAccess::state(protocol).acceptedEpoch;
    auto conflict=protocol.prepareProducer(QRect((count+2)*64,0,64,64));QVERIFY(conflict);QString error;QVERIFY(!protocol.clear(&error));
    QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch,epoch);QCOMPARE(dm.m_hashTable,table);QCOMPARE(dm.extent(),extent);
    protocol.cancel(conflict);bool installed=false,held=false;KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](auto phase){if(phase!=KisPageStoreDiagnosticPhase::NativeAdapterInstalled)return;installed=true;held=KisNativeResultTestAccess::state(protocol).activeResults==1;});
    QVERIFY(protocol.clear(&error));recorder.setPhaseObserver({});QCOMPARE(installed,bool(count&&!remnants));if(installed)QVERIFY(held);
    QCOMPARE(dm.m_hashTable,table);QCOMPARE(dm.m_hashTable->numTiles(),0);QCOMPARE(dm.extent(),QRect());QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
    if(reader)QCOMPARE(reader->data()[0],initial);if(count){auto exact=retained.readPage(0,0);QVERIFY(exact.data());QCOMPARE(exact.data()[0],initial);}
    if(history){QVERIFY(dm.tryCommit());dm.rollback(owner);quint8 actual=0;dm.readBytes(&actual,0,0,1,1);QCOMPARE(actual,count&&!remnants?initial:blank);dm.purgeHistory(owner);}
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
    auto &protocol=*dm.m_nativeResult;
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    if (history) dm.clear(0, 0, 128, 64, &staged);
    auto beforeView = protocol.capture(); QVERIFY(bool(beforeView));
    auto held=protocol.prepareProducer(QRect(64,0,64,64));QVERIFY(held);const auto epoch=KisNativeResultTestAccess::state(protocol).acceptedEpoch;
    QString error;
    QVERIFY(!(clearAll ? protocol.clear(&error) : protocol.remove(QRect(0,0,128,64),&error)));
    QVERIFY(!error.isEmpty());
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(1));QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch,epoch);
    QByteArray actual(128 * 64, Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 128, 64);
    QCOMPARE(actual, QByteArray(actual.size(), char(history ? staged : initial)));
    protocol.cancel(held);
    QVERIFY(clearAll ? protocol.clear(&error) : protocol.remove(QRect(0,0,128,64),&error));
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 128, 64);
    QCOMPARE(actual, QByteArray(actual.size(), char(blank)));
    auto old = beforeView.readPage(0,0); QVERIFY(old.data());
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
    auto &protocol=*dm.m_nativeResult;
    auto old = protocol.capture(); QVERIFY(bool(old));
    auto held=protocol.prepareProducer(QRect(128,0,64,64));QVERIFY(held);const auto epoch=KisNativeResultTestAccess::state(protocol).acceptedEpoch;
    const QRect area(1, 0, 128, 64); // partial pixel page, semantic page, failing partial page
    QString error;auto sourceView=source.m_nativeResult->capture();QVERIFY(sourceView);
    QVERIFY(!(copy?protocol.copyFrom(sourceView,area,&error):protocol.fill(area,changedPixel,&error)));
    QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch,epoch);QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(1));
    QByteArray actual(192 * 64 * bpp, Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 192, 64);
    QCOMPARE(actual, QByteArray(actual.size(), char(history ? 0x41 : 0x31)));
    protocol.cancel(held);KisPageStoreDiagnosticRecorder recorder(true);
    if (copy) dm.KisTiledDataManager::bitBlt(&source, area);
    else dm.clear(area.x(), area.y(), area.width(), area.height(), pixel(changedPixel));
    const auto &installed=recorder.metrics()[size_t(KisPageStoreDiagnosticPhase::NativeAdapterInstalled)];
    QCOMPARE(installed.intervals,quint64(1));QCOMPARE(installed.workItems,quint64(3));
    QByteArray expected(actual.size(), char(history ? 0x41 : 0x31));
    for (int y = 0; y < 64; ++y) memset(expected.data() + (y * 192 + 1) * bpp,
        quint8(changedPixel[0]), size_t(128 * bpp));
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 192, 64); QCOMPARE(actual, expected);
    for (int column = 0; column < 3; ++column) {
        auto previous = old.readPage(column,0); QVERIFY(previous.data());
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
    auto &protocol=*dm.m_nativeResult;
    const auto epoch=KisNativeResultTestAccess::state(protocol).acceptedEpoch;
    int visits = 0; bool injected = false; QString error;
    KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::CopySourcePage || ++visits != 1) return;
        injected = source.m_nativeResult->fill(QRect(0, 0, 256, 64), QByteArray(bpp, char(0x61)), &error) &&
            source.m_nativeResult->setDefaultPixel(QByteArray(bpp, char(0x29)), &error);
    });
    const QRect area(1, 0, 192, 64);
    if (rough) {
        if (oldSource) dm.KisTiledDataManager::bitBltRoughOldData(&source, area);
        else dm.KisTiledDataManager::bitBltRough(&source, area);
    } else {
        if (oldSource) dm.KisTiledDataManager::bitBltOldData(&source, area);
        else dm.KisTiledDataManager::bitBlt(&source, area);
    }
    QVERIFY2(injected, qPrintable(error)); QCOMPARE(visits, 1);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch,epoch+1);
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

void KisTiledDataManagerTest::testPageStoreCopyAfterRangeWait_data()
{
    QTest::addColumn<bool>("cancelFirst");
    QTest::addColumn<bool>("expands");
    QTest::addColumn<bool>("outsideRequest");
    for (bool cancel : {false, true}) for (bool expands : {false, true}) for (bool outside : {false, true}) {
        if (!expands && outside) continue;
        QTest::newRow(qPrintable(QStringLiteral("cancel%1-expands%2-outside%3").arg(cancel).arg(expands).arg(outside)))
            << cancel << expands << outside;
    }
}

void KisTiledDataManagerTest::testPageStoreCopyAfterRangeWait()
{
    QFETCH(bool, cancelFirst); QFETCH(bool, expands); QFETCH(bool, outsideRequest);
    const quint8 blank = 0, initial = 0x31;
    KisTiledDataManager source(1, &blank), target(1, &blank);
    source.clear(QRect(0, 0, 64, 64), &initial);
    target.bitBlt(&source, QRect(0, 0, 64, 64)); // The old destination is the same retained alias.
    auto &protocol=*target.m_nativeResult;
    QSemaphore entered, proceed, waiting;
    auto firstResult = KisPageStoreWriteOperationResult::Failed;
    bool copied = false;
    std::thread first([&] {
        firstResult = target.writePageStoreOperation({QRect(0, 0, expands ? 65 : 1, 1)}, [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(0, 0);
            if (!cursor->rawData()) return false;
            *cursor->rawData() = 0x71;
            if (expands) {
                cursor->moveTo(64, 0); if (!cursor->rawData()) return false;
                *cursor->rawData() = 0x72;
            }
            entered.release(); proceed.acquire(); return !cancelFirst;
        });
    });
    const bool started = entered.tryAcquire(1, 5000);
    if (!started) { proceed.release(); first.join(); QVERIFY(started); return; }
    std::thread second([&] {
        KisPageStoreDiagnosticRecorder recorder(true);
        recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
            if (phase == KisPageStoreDiagnosticPhase::PixelOperationRangeWait) waiting.release();
        });
        KisStrokeJobFailureContext failure(true);
        target.bitBlt(&source, QRect(0, 0, outsideRequest ? 64 : 128, 64));
        copied = !failure.failed();
    });
    const bool blocked = waiting.tryAcquire(1, 5000);
    proceed.release(); first.join(); second.join();
    QVERIFY(blocked); QCOMPARE(copied, cancelFirst || !expands || outsideRequest);
    QCOMPARE(firstResult, cancelFirst ? KisPageStoreWriteOperationResult::Failed : KisPageStoreWriteOperationResult::Succeeded);
    quint8 actual = 0; target.readBytes(&actual, 0, 0, 1, 1);
    // Stale aliases cannot prove a post-wait no-op. Expanded bounds refuse
    // before pixels, rather than silently omitting the newly required page.
    QCOMPARE(actual, quint8(copied ? initial : 0x71));
    target.readBytes(&actual, 64, 0, 1, 1);
    QCOMPARE(actual, quint8(!cancelFirst && expands ? 0x72 : blank));
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
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
        if (rough) {
            if (oldSource) target.bitBltRoughOldData(&source, area); else target.bitBltRough(&source, area);
        } else {
            if (oldSource) target.bitBltOldData(&source, area); else target.bitBlt(&source, area);
        }
        const bool immutableBefore = history && oldSource;
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
    KisTiledDataManager source(1,&blank,KisTiledDataManager::SelectionAuthority::ExternalProtocol);
    KisTiledDataManager target(1, &blank);

    // Exercise the production compatibility path with a legacy source. Keep
    // its two physical backings distinct so only the second page is swapped.
    source.clear(0, 0, 64, 64, &firstSource);
    source.clear(64, 0, 64, 64, &secondSource);
    target.clear(0, 0, 128, 64, &initialTarget);
    const auto original = target.m_hashTable->getExistingTile(0, 0);

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

    const auto committed = KisNativeResultTestAccess::state(*target.m_nativeResult).acceptedEpoch;
    tileStore->testingFailNextSwapIn(KisSwapInFailurePoint(failurePoint));
    const QRect area = rough ? QRect(0, 0, 128, 64) : QRect(1, 0, 126, 64);
    if (rough)
        target.bitBltRough(&source, area);
    else
        target.bitBlt(&source, area);

    QCOMPARE(KisNativeResultTestAccess::state(*target.m_nativeResult).acceptedEpoch, committed);
    QVERIFY(!secondData->isResident());
    const auto firstCached = target.m_hashTable->getExistingTile(0, 0);
    QCOMPARE(firstCached, original);
    // Cancellation invalidates selection without replacing the wrapper or
    // revoking borrowed pointers. A real read reacquires the accepted version.
    QVERIFY(firstCached->lockForRead());
    {
        const auto unlock = qScopeGuard([&] { firstCached->unlockForRead(); });
        QVERIFY(firstCached->data());
        QCOMPARE(firstCached->data()[rough ? 0 : 1], initialTarget);
    }
    QByteArray actual(128 * 64, Qt::Uninitialized);
    target.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 128, 64);
    QCOMPARE(actual, QByteArray(actual.size(), char(initialTarget)));
}

void KisTiledDataManagerTest::testPageStoreCompatibilityCopyPublicationFailure_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("rough");
    for (int bpp : {1, 4, 8, 16}) for (bool rough : {false, true})
        QTest::newRow(qPrintable(QString("B%1-rough%2").arg(bpp).arg(rough))) << bpp << rough;
}

void KisTiledDataManagerTest::testPageStoreCompatibilityCopyPublicationFailure()
{
    QFETCH(int, bpp); QFETCH(bool, rough);
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31)), paint(bpp, char(0x71));
    const auto bytes = [](const QByteArray &p) { return reinterpret_cast<const quint8 *>(p.constData()); };
    KisTiledDataManager source(bpp,bytes(blank),KisTiledDataManager::SelectionAuthority::ExternalProtocol),target(bpp,bytes(blank));
    source.clear(QRect(0, 0, 128, 64), bytes(paint));
    target.clear(QRect(0, 0, 64, 64), bytes(initial));
    target.clear(QRect(128, 0, 64, 64), bytes(initial));
    const auto warm = target.getTile(0, 0, false), removed = target.getTile(2, 0, false);
    QVERIFY(warm); QVERIFY(removed);
    auto &protocol=*target.m_nativeResult;
    const auto committed = KisNativeResultTestAccess::state(protocol).acceptedEpoch;
    const QRect area(0, 0, 192, 64); QCOMPARE(target.extent(), area);
    kisDrainPageStoreReclamation(); auto process = kisAcquirePageStoreBootstrapBudget(); QVERIFY(process);
    size_t count = 0; void *filler = nullptr; bool injected = false, unchangedBeforeRoot = false;
    const auto free = qScopeGuard([&] { if (filler) freeTestProcessStoragePressure(filler, count, 1); });
    KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::NativeAcceptPrepared || injected) return;
        injected = true;
        unchangedBeforeRoot = target.m_hashTable->getExistingTileForPreparedUpdate(0, 0) == warm &&
            !target.m_hashTable->getExistingTileForPreparedUpdate(1, 0) &&
            target.m_hashTable->getExistingTileForPreparedUpdate(2, 0) == removed;
        const auto usage = process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        count = size_t(kisPageProcessStorageLimit() - usage.live.cpuRam - usage.reserved.cpuRam);
        filler = allocateTestProcessStoragePressure(count, 1);
    });
    bool failed = false;
    {
        KisStrokeJobFailureContext failure(true);
        if (rough) target.bitBltRough(&source, area); else target.bitBlt(&source, area);
        failed = failure.failed();
    }
    recorder.setPhaseObserver({});
    if (filler) freeTestProcessStoragePressure(std::exchange(filler, nullptr), count, 1);
    QVERIFY(injected);QVERIFY(!failed);QVERIFY(unchangedBeforeRoot);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch,committed+1);
    QCOMPARE(target.m_hashTable->getExistingTileForPreparedUpdate(0,0),warm);
    QVERIFY(target.m_hashTable->getExistingTileForPreparedUpdate(1,0));QVERIFY(!target.m_hashTable->getExistingTileForPreparedUpdate(2,0));
    QByteArray actual(192*bpp,0),expected(actual.size(),char(0x13));std::memset(expected.data(),0x71,size_t(128*bpp));
    target.readBytes(reinterpret_cast<quint8 *>(actual.data()),0,0,192,1);QCOMPARE(actual,expected);
    kisDrainPageStoreReclamation();QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
    if (rough) target.bitBltRough(&source, area); else target.bitBlt(&source, area);
    std::memset(expected.data(), 0x71, size_t(128 * bpp));
    std::memset(expected.data() + 128 * bpp, 0x13, size_t(64 * bpp));
    target.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 192, 1); QCOMPARE(actual, expected);
    QCOMPARE(target.extent(), QRect(0, 0, 128, 64));
}

void KisTiledDataManagerTest::testPageStoreCompatibilityCopySegmentFailure_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("rough"); QTest::addColumn<bool>("history");
    for (int bpp : {1, 4, 8, 16}) for (bool rough : {false, true}) for (bool history : {false, true})
        QTest::newRow(qPrintable(QString("B%1-rough%2-history%3").arg(bpp).arg(rough).arg(history)))
            << bpp << rough << history;
}

void KisTiledDataManagerTest::testPageStoreCompatibilityCopySegmentFailure()
{
    QFETCH(int, bpp); QFETCH(bool, rough); QFETCH(bool, history);
    const QByteArray blank(bpp, char(0x13)), initial(bpp, char(0x31)), paint(bpp, char(0x71));
    const auto bytes = [](const QByteArray &p) { return reinterpret_cast<const quint8 *>(p.constData()); };
    KisTiledDataManager source(bpp,bytes(blank),KisTiledDataManager::SelectionAuthority::ExternalProtocol),target(bpp,bytes(blank));
    source.clear(QRect(0, 0, 128, 64), bytes(paint));
    source.clear(QRect(256, 0, 64, 64), bytes(paint));
    for (int column : {0, 2, 4}) target.clear(QRect(column * 64, 0, 64, 64), bytes(initial));
    const auto original = target.getTile(0, 0, false);
    const auto owner = history ? target.getMemento() : KisMementoSP{};
    if (history) { QVERIFY(owner); target.clear(QRect(384, 0, 64, 64), bytes(paint)); }
    auto &protocol=*target.m_nativeResult;
    const auto committed = KisNativeResultTestAccess::state(protocol).acceptedEpoch;
    const auto read = [&] {
        QByteArray result(448 * bpp, 0);
        target.readBytes(reinterpret_cast<quint8 *>(result.data()), 0, 0, 448, 1);
        return result;
    };
    const QByteArray before = read();
    const QRect extent = target.extent(), area(1, 0, 318, 64);
    kisDrainPageStoreReclamation(); auto process = kisAcquirePageStoreBootstrapBudget(); QVERIFY(process);
    size_t count = 0; void *filler = nullptr; bool injected = false;
    const auto free = qScopeGuard([&] { if (filler) freeTestProcessStoragePressure(filler, count, 1); });
    int visits=0;KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::PackedWritePrepare || ++visits!=3 || injected) return;
        injected = true;
        const auto usage = process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        count = size_t(kisPageProcessStorageLimit() - usage.live.cpuRam - usage.reserved.cpuRam);
        filler = allocateTestProcessStoragePressure(count, 1);
    });
    {
        KisStrokeJobFailureContext failure(true);
        if (rough) target.bitBltRough(&source, area); else target.bitBlt(&source, area);
        QVERIFY(failure.failed());
    }
    recorder.setPhaseObserver({});
    if (filler) freeTestProcessStoragePressure(std::exchange(filler, nullptr), count, 1);
    QVERIFY(injected); QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch, committed);
    QCOMPARE(target.m_hashTable->getExistingTile(0, 0), original);
    QVERIFY(!target.m_hashTable->getExistingTile(1, 0));
    QCOMPARE(target.extent(), extent); QCOMPARE(read(), before);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
    if (rough) target.bitBltRough(&source, area); else target.bitBlt(&source, area);
    QByteArray expected = before;
    const int begin = rough ? 0 : 1, end = rough ? 320 : 319;
    for (int x = begin; x < end; ++x)
        std::memset(expected.data() + x * bpp, x < 128 || x >= 256 ? 0x71 : 0x13, size_t(bpp));
    QCOMPARE(read(), expected);
    if (history) {
        QVERIFY(target.tryCommit()); target.rollback(owner);
        QByteArray undo = before; std::memset(undo.data() + 384 * bpp, 0x13, size_t(64 * bpp));
        QCOMPARE(read(), undo); target.rollforward(owner); QCOMPARE(read(), expected);
        target.purgeHistory(owner);
    }
    kisDrainPageStoreReclamation(); QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
}

void KisTiledDataManagerTest::testPageStoreDefaultLifecycleDoesNotMaterialize()
{
    QFETCH(int, pixelSize);
    const QByteArray defaultPixel(pixelSize, char(0x2a));
    const QByteArray paintedPixel(pixelSize, char(0x5d));
    KisTiledDataManager dm(pixelSize,
        reinterpret_cast<const quint8 *>(defaultPixel.constData()));
    auto *protocol = dm.m_nativeResult.get(); QVERIFY(protocol);
    const QRect rect(-64, -64, 256, 256);
    for (int cycle = 0; cycle < 4; ++cycle) {
        const KisMementoSP edit = dm.getMemento();
        dm.clear(rect, reinterpret_cast<const quint8 *>(paintedPixel.constData()));
        dm.commit();
        QCOMPARE(dm.extent(), rect);
        const auto written = KisNativeResultTestAccess::state(*protocol);
        dm.rollback(edit);
        QCOMPARE(dm.extent(), QRect());
        QVERIFY(KisNativeResultTestAccess::state(*protocol).physicalResources <= written.physicalResources);
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
        QCOMPARE(KisNativeResultTestAccess::state(*protocol).activeResults, quint64(0));
    }
    QVERIFY(protocol->close());
    kisDrainPageStoreReclamation();
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).physicalResources, quint64(0));
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
    auto *protocol = dm.m_nativeResult.get(); QVERIFY(protocol);
    const QRect initialRect(0, 0, 128, 128);
    dm.clear(initialRect,
             reinterpret_cast<const quint8 *>(paintedPixel.constData()));
    const KisMementoSP edit = dm.getMemento();
    // Include both committed pages and a newly prepared page in clear-all.
    dm.clear(QRect(256, 0, 64, 64),
             reinterpret_cast<const quint8 *>(paintedPixel.constData()));
    const auto beforeClear = KisNativeResultTestAccess::state(*protocol);
    dm.clear();
    QVERIFY(KisNativeResultTestAccess::state(*protocol).physicalResources <= beforeClear.physicalResources);
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
    auto &protocol=*dm.m_nativeResult;
    const QRect allPages(0, 0, totalPages * 64, 64);
    const QRect changedRect(0, 0, changedPages * 64, 64);
    dm.clear(allPages, reinterpret_cast<const quint8 *>(initialPixel.constData()));
    QVector<KisNativeResultProtocol::Snapshot> retained;
    for (int h = 0; h < retainedRoots; ++h) {
        const QByteArray pixel(pixelSize, char(0x20 + h));
        dm.clear(QRect((totalPages - 1) * 64, 0, 64, 64),
                 reinterpret_cast<const quint8 *>(pixel.constData()));
        retained.append(protocol.capture());
        QVERIFY(bool(retained.back()));
    }
    // Separate a known uniform-cache hit from byte-identical content whose
    // backing cache was displaced by setup. Do not call the latter zero work.
    const auto beforeWarmup = KisNativeResultTestAccess::state(protocol);
    QElapsedTimer warmupTimer;
    warmupTimer.start();
    if (mode == "noop") {
        dm.clear(changedRect, reinterpret_cast<const quint8 *>(initialPixel.constData()));
    }
    const qint64 warmupNanoseconds = warmupTimer.nsecsElapsed();
    const auto before = KisNativeResultTestAccess::state(protocol);
    QCOMPARE(before.pageSlots, quint64(totalPages));
    const QByteArray first(pixelSize * changedPages * 64, char(0x40));
    const QByteArray second(pixelSize * changedPages * 64, char(0x41));
    KisPageStoreDiagnosticRecorder stages(stageProfile);
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
    if (stageProfile) QVERIFY(stages.setRecording(false));
    const QJsonArray stageMetrics = kisBr1StageMetrics(stages);
    if (stageProfile) {
        QCOMPARE(stages.metrics()[size_t(KisPageStoreDiagnosticPhase::ManifestExport)].intervals, quint64(0));
    }
    const auto after = KisNativeResultTestAccess::state(protocol);
    const bool identical=mode=="noop"||mode=="identical";
    const quint64 expectedChanged=identical?0:quint64(loops*changedPages);
    const auto cuts=after.acceptedEpoch-before.acceptedEpoch;
    if(mode=="identical")QVERIFY(cuts<=1);else QCOMPARE(cuts,quint64(mode=="noop"?0:loops));
    QCOMPARE(after.activeResults,quint64(0));
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
    for(const auto &snapshot:std::as_const(retained)){auto old=snapshot.readPage(0,0);QVERIFY(old.data());QCOMPARE(old.data()[0],quint8(0x11));}
    QElapsedTimer releaseTimer;
    releaseTimer.start();
    retained.clear();
    kisDrainPageStoreReclamation();
    const qint64 releaseNanoseconds = releaseTimer.nsecsElapsed();
    const auto drained = KisNativeResultTestAccess::state(protocol);
    QCOMPARE(drained.logicalViews, quint64(0));
    if (stageProfile || !loopsOption.isEmpty()) {
        qInfo().noquote()<<"BR1_FIXED_NATIVE"<<QJsonDocument(QJsonObject{
        {"case",QTest::currentDataTag()},{"mode",mode},{"N",totalPages},{"K",changedPages},{"H",retainedRoots},{"bpp",pixelSize},{"loops",loops},
        {"logical_changed_pages",double(expectedChanged)},{"submitted_page_visits",double(loops*changedPages)},
        {"accepted_cuts",double(cuts)},{"current_physical_resources",double(after.physicalResources)},
        {"scope","diagnostic fixture only: setup/read/retained-release outside operation; not whole-price evidence"},
        {"operation_ms",double(operationNanoseconds)/1e6},{"known_noop_warmup_ms",double(warmupNanoseconds)/1e6},
        {"known_noop_warmup_cuts",double(before.acceptedEpoch-beforeWarmup.acceptedEpoch)},
        {"release_retained_ms",double(releaseNanoseconds)/1e6},{"stage_profile",stageProfile},{"cpu_clock_available",stages.threadCpuClockAvailable()},
        {"stage_sample_period",samplePeriod},{"stage_sample_offset",sampleOffset},{"stages",stageMetrics}
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

void KisTiledDataManagerTest::testPageStoreExplicitDefaultHistory_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("mode"); QTest::addColumn<bool>("persistent");
    for (int bpp : {1, 4, 8, 16}) for (int mode = 0; mode < 5; ++mode)
        for (bool persistent : {false, true}) {
            if (persistent && mode >= 3) continue; // Public clear/getTile have no persistent owner API.
            QTest::newRow(qPrintable(QString("%1-mode%2-persistent%3").arg(bpp).arg(mode).arg(persistent)))
                << bpp << mode << persistent;
        }
}

void KisTiledDataManagerTest::testPageStoreExplicitDefaultHistory()
{
    QFETCH(int, bpp); QFETCH(int, mode); QFETCH(bool, persistent);
    const QByteArray blank(bpp, char(0x13)), color(bpp, char(0x57)), other(bpp, char(0x29));
    auto bytes = [](const QByteArray &p) { return reinterpret_cast<const quint8 *>(p.constData()); };
    const auto exercise = [&](bool native, QStringList &observations) {
        KisTiledDataManager dm(bpp, bytes(blank), native ? KisTiledDataManager::SelectionAuthority::NativeResult : KisTiledDataManager::SelectionAuthority::ExternalProtocol);
        if (mode == 2 || mode == 3) dm.clear(QRect(0, 0, 64, 64), bytes(color));
        const auto memento = dm.getMemento(); QVERIFY(memento);
        const auto report = [&] {
            QByteArray actual(2 * bpp, 0);
            dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 1, 1);
            dm.readBytes(reinterpret_cast<quint8 *>(actual.data()) + bpp, 128, 0, 1, 1);
            const auto rect = dm.extent();
            observations.append(QString("%1,%2,%3,%4:%5").arg(rect.x()).arg(rect.y())
                .arg(rect.width()).arg(rect.height()).arg(QString::fromLatin1(actual.toHex())));
        };
        const int side = mode == 0 ? 1 : 64;
        if (mode == 4) QVERIFY(dm.getTile(0, 0, true));
        else if (mode == 3) dm.clear(QRect(0, 0, 64, 64), bytes(blank));
        else if (native && persistent) {
            QVERIFY(dm.beginStrokeMutation(memento));
            QVERIFY(dm.applyStrokePixelOperation(memento, {QRect(0, 0, side, side)}, [&](KisPixelWriteCursor *cursor) {
                for (int y = 0; y < side; ++y) {
                    cursor->moveTo(0, y); if (!cursor->rawData()) return false;
                    std::memset(cursor->rawData(), 0x13, size_t(side * bpp));
                }
                return true;
            }));
            QVERIFY(dm.checkpointStrokeMutation(memento));
        } else {
            const QByteArray input(side * side * bpp, char(0x13));
            dm.writeBytes(bytes(input), 0, 0, side, side);
        }
        report();
        QVERIFY(dm.tryCommit());
        report(); dm.rollback(memento); report(); dm.rollforward(memento); report();
        dm.setDefaultPixel(bytes(other)); report();
        dm.purgeHistory(memento);
    };
    QStringList legacy, native;
    exercise(false, legacy); exercise(true, native);
    // These public results also have an independent, identically compiled
    // upstream probe. The modified legacy fallback alone is not its oracle.
    QStringList expected;
    for (int stage = 0; stage < 5; ++stage) {
        const bool coloredBefore = mode == 2 || mode == 3;
        const bool present = stage == 2 ? coloredBefore : mode != 3;
        const QByteArray inside = stage == 2 && coloredBefore ? color :
            stage == 4 && mode == 3 ? other : blank;
        const QByteArray outside = stage == 4 ? other : blank;
        expected.append(QString("0,0,%1,%1:%2").arg(present ? 64 : 0)
            .arg(QString::fromLatin1(QByteArray(inside + outside).toHex())));
    }
    QCOMPARE(legacy, expected); QCOMPARE(native, expected);
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
    auto *backend = dm.m_nativeResult.get();
    auto memento = history ? dm.getMemento() : KisMementoSP{};
    if (history) dm.clear(whole.x(), whole.y(), whole.width(), whole.height(), reinterpret_cast<const quint8 *>(prior.constData()));
    auto old = backend->capture(); QVERIFY(old);
    const QRect area = partial ? QRect(-63,-63,190,126) : whole;
    const int stride = area.width() * bpp + 17;
    const char before = history ? 0x41 : 0x31;
    QByteArray input(stride * area.height(), char(0xee));
    for (int y = 0; y < area.height(); ++y)
        memset(input.data() + y * stride, noOp ? before : 0x71, size_t(area.width() * bpp));
    const auto beforeState = KisNativeResultTestAccess::state(*backend);
    KisPageStoreDiagnosticRecorder recorder(true);
    bool hidden = true; int copies = 0;
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::PackedWritePrepare) return;
        ++copies;
        auto view = backend->capture(); auto read = view.readPage(-1,-1);
        hidden &= read.data() && reinterpret_cast<const char *>(read.data())[0] == before;
    });
    dm.writeBytes(reinterpret_cast<const quint8 *>(input.constData()), area.x(), area.y(), area.width(), area.height(), stride);
    QVERIFY(hidden);
    const auto after = KisNativeResultTestAccess::state(*backend);
    QCOMPARE(copies, noOp ? 0 : 6);
    QCOMPARE(after.activeResults, quint64(0));
    QCOMPARE(after.acceptedEpoch - beforeState.acceptedEpoch, quint64(noOp ? 0 : 1));
    QByteArray expected(whole.width() * whole.height() * bpp, before), actual(expected.size(), Qt::Uninitialized);
    if (!noOp) for (int y = area.top(); y <= area.bottom(); ++y)
        memset(expected.data() + ((y - whole.y()) * whole.width() + area.x() - whole.x()) * bpp,
               0x71, size_t(area.width() * bpp));
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), whole.x(), whole.y(), whole.width(), whole.height());
    QCOMPARE(actual, expected);
    auto oldRead = old.readPage(-1,-1); QVERIFY(oldRead.data());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(oldRead.data()), 64*64*bpp), QByteArray(64*64*bpp, before));
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
    auto *backend = dm.m_nativeResult.get();
    auto memento = history ? dm.getMemento() : KisMementoSP{};
    if (history) dm.clear(0,0,128,64,&prior);
    KisNativeResultProtocol::Prepared held;
    int prepared = 0;
    KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase == KisPageStoreDiagnosticPhase::PackedWritePrepare && ++prepared == 2)
            held = backend->prepareProducer(QRect(64,0,64,64));
    });
    QByteArray input(128 * (partial ? 1 : 64), char(0x71));
    QString error;
    QVERIFY2(backend->write(reinterpret_cast<const quint8 *>(input.constData()), QRect(0,0,128,partial ? 1 : 64),128,&error),qPrintable(error));
    // The competing writer arrives after range admission, so it cannot
    // acquire the second page even while the first page is being prepared.
    QVERIFY(!held); QCOMPARE(prepared, 2); QVERIFY(error.isEmpty());
    QByteArray actual(input.size(), Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()),0,0,128,partial ? 1 : 64);
    QCOMPARE(actual, input);
    held = {};
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
    auto tile = dm.getTile(0,0,true); QVERIFY(tile && tile->lockForWrite());
    const auto cleanup = qScopeGuard([&] { tile->unlockForWrite(); });
    auto *pointer = tile->data(); QVERIFY(pointer); *pointer = 0x31;
    const auto before = KisNativeResultTestAccess::state(*dm.m_nativeResult);
    { KisStrokeJobFailureContext failure(true); dm.writeBytes(&next,0,0,1,1); QVERIFY(!failure.failed()); }
    QCOMPARE(tile->data(), pointer); QCOMPARE(*pointer, next);
    QCOMPARE(KisNativeResultTestAccess::state(*dm.m_nativeResult).activeResults, before.activeResults);
    for (int mode = 0; mode < 2; ++mode) {
        KisStrokeJobFailureContext failure(true);
        if (mode == 0) dm.writeBytes(&next,128,0,2,1,1);
        else dm.writeBytes(&next,std::numeric_limits<qint32>::max(),0,2,1);
        QVERIFY(failure.failed());
    }
    { KisStrokeJobFailureContext failure(true); dm.writeBytes(nullptr,0,0,1,1); dm.writeBytes(&next,0,0,0,1); QVERIFY(!failure.failed()); }
    QCOMPARE(KisNativeResultTestAccess::state(*dm.m_nativeResult).acceptedEpoch, before.acceptedEpoch);
    QCOMPARE(tile->data(), pointer); QCOMPARE(*pointer, next);
}

void KisTiledDataManagerTest::testPageStoreCompatibilityWriteFailure_data()
{
    QTest::addColumn<bool>("planar");
    QTest::addColumn<bool>("history");
    QTest::addColumn<int>("failurePoint");
    for (bool planar : {false, true}) for (bool history : {false, true})
        for (auto point : {KisSwapInFailurePoint::Mapping, KisSwapInFailurePoint::Allocation,
                           KisSwapInFailurePoint::Decompression})
            QTest::newRow(qPrintable(QStringLiteral("planar%1-history%2-failure%3")
                .arg(planar).arg(history).arg(int(point)))) << planar << history << int(point);
}

void KisTiledDataManagerTest::testPageStoreCompatibilityWriteFailure()
{
    QFETCH(bool, planar);
    QFETCH(bool, history);
    QFETCH(int, failurePoint);
    const quint8 blank = 0, initial = 0x31, warm = 0x32, last = 0x33;
    KisTiledDataManager dm(1, &blank);
    dm.clear(0, 0, 64, 64, &initial);
    dm.clear(64, 0, 64, 64, &warm);
    dm.clear(192, 0, 64, 64, &last);
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    auto borrowed = dm.getTile(0, 0, true);
    QVERIFY(borrowed && borrowed->lockForWrite());
    bool locked = true;
    const auto unlock = qScopeGuard([&] { if (locked) borrowed->unlockForWrite(); });
    auto *pointer = borrowed->data(); QVERIFY(pointer); pointer[0] = 0x41;
    bool exists = false;
    const auto swapped = dm.getReadOnlyTileLazy(3, 0, exists); QVERIFY(exists);
    auto *backing = swapped->tileData(); QVERIFY(backing->ref());
    const auto unref = qScopeGuard([&] { backing->deref(); });
    auto *tileStore = KisTileDataStore::instance();
    QVERIFY(tileStore->trySwapTileData(backing)); QVERIFY(!backing->isResident());
    QByteArray input(256, char(0x71));
    const auto write = [&] {
        auto *bytes = reinterpret_cast<quint8 *>(input.data());
        if (planar) dm.writePlanarBytes({bytes}, {1}, 0, 0, 256, 1);
        else dm.writeBytes(bytes, 0, 0, 256, 1);
    };
    const auto committed = KisNativeResultTestAccess::state(*dm.m_nativeResult).acceptedEpoch;
    tileStore->testingFailNextSwapIn(KisSwapInFailurePoint(failurePoint));
    {
        KisStrokeJobFailureContext failure(true);
        write(); QVERIFY(failure.failed());
    }
    QCOMPARE(KisNativeResultTestAccess::state(*dm.m_nativeResult).acceptedEpoch, committed);
    // The earlier raw capability remains its own owner. This call cancels
    // its warm/cold private writes without revoking that live pointer.
    QCOMPARE(borrowed->data(), pointer); QCOMPARE(pointer[0], quint8(0x71));
    QVERIFY(!dm.m_hashTable->getExistingTile(2, 0));
    auto cached = dm.m_hashTable->getExistingTile(1, 0); QVERIFY(cached);
    QVERIFY(cached->lockForRead());
    QCOMPARE(cached->data()[0], warm); cached->unlockForRead();
    QByteArray actual(256, Qt::Uninitialized);
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 256, 1);
    QCOMPARE(actual, QByteArray(64, char(initial)) + QByteArray(64, char(warm)) +
                     QByteArray(64, char(blank)) + QByteArray(64, char(last)));
    borrowed->unlockForWrite(); locked = false;
    // A fresh operation can reuse all cancelled claims, including the hole.
    write(); dm.commit();
    dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 256, 1);
    QCOMPARE(actual, input);
    if (history) {
        dm.rollback(memento);
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 256, 1);
        QCOMPARE(actual, QByteArray(64, char(initial)) + QByteArray(64, char(warm)) +
                         QByteArray(64, char(blank)) + QByteArray(64, char(last)));
        dm.purgeHistory(memento);
    }
}

void KisTiledDataManagerTest::testPageStorePackedWriteBodyAllowsReader()
{
    quint8 blank = 0x13, initial = 0x31, input = 0x71, observed = 0;
    KisDataManager dm(1, &blank); dm.clear(0,0,64,64,&initial);
    QSemaphore start, finished;
    std::thread reader([&] { start.acquire(); dm.readBytes(&observed,0,0,1,1); finished.release(); });
    bool entered = false, completedInside = false;
    const auto cleanup = qScopeGuard([&] { if (!entered) start.release(); if (reader.joinable()) reader.join(); });
    KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::PackedWritePrepare || entered) return;
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
    auto *protocol = dm.m_nativeResult.get(); QVERIFY(protocol);
    const QRect area(-64, -64, columns * 64, 128);
    const QByteArray input(area.width() * area.height() * bpp, char(0x47));
    const auto memento = dm.getMemento();
    dm.writeBytes(reinterpret_cast<const quint8 *>(input.constData()), area.x(), area.y(), area.width(), area.height());
    dm.commit();
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).activeResults, quint64(0));
    // A complete packed result must retain no RAM pins between operations.
    // Prove against every real output backing, not a removed guard counter.
    for (int y = -1; y <= 0; ++y) for (int x = -1; x < columns - 1; ++x) {
        bool present = false; auto tile = dm.getReadOnlyTileLazy(x,y,present); QVERIFY(tile && present);
        QVERIFY(KisTileDataStore::instance()->trySwapTileData(tile->tileData()));
    }
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
    const auto before = KisNativeResultTestAccess::state(*dm.m_nativeResult);
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
    const auto after = KisNativeResultTestAccess::state(*dm.m_nativeResult);
    QCOMPARE(after.activeResults, quint64(0));
    QVERIFY(after.acceptedEpoch > before.acceptedEpoch);
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

    const auto before = KisNativeResultTestAccess::state(*dm.m_nativeResult);
    constexpr qint32 lockCount = 4096;
    for (qint32 i = 0; i < lockCount; ++i) {
        tile->lockForRead();
        QCOMPARE(tile->data()[0], paintedPixel);
        tile->unlockForRead();
    }
    const auto after = KisNativeResultTestAccess::state(*dm.m_nativeResult);

    QCOMPARE(after.pageSlots, before.pageSlots);
    QCOMPARE(after.physicalResources, before.physicalResources);
    QCOMPARE(after.acceptedEpoch, before.acceptedEpoch);
    QCOMPARE(after.activeResults, quint64(0));
    QVERIFY(KisTileDataStore::instance()->trySwapTileData(tile->tileData()));
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
    const auto authority = native ? KisTiledDataManager::SelectionAuthority::NativeResult
                                  : KisTiledDataManager::SelectionAuthority::ExternalProtocol;
    KisTiledDataManager source(bpp, reinterpret_cast<const quint8 *>(blank.constData()), authority);
    KisTiledDataManager target(bpp, reinterpret_cast<const quint8 *>(targetDefault.constData()), authority);
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
    const auto sourceViews = native ? KisNativeResultTestAccess::state(*source.m_nativeResult).logicalViews : 0;
    bool prepared = false, selectedBeforePreparation = false;
    using Phase = KisPageStoreDiagnosticPhase;
    KisPageStoreDiagnosticRecorder recorder(native);
    recorder.setPhaseObserver([&](Phase phase) {
        if (!native || phase != Phase::CopySourcePage || prepared) return;
        prepared = true;
        selectedBeforePreparation = KisNativeResultTestAccess::state(*source.m_nativeResult).logicalViews >= sourceViews + (history ? 0 : 1);
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
        QCOMPARE(metrics[size_t(Phase::MutationPresencePrepare)].workItems, quint64(0));
        QCOMPARE(metrics[size_t(Phase::CopySourcePage)].workItems, selectedPages);
        QCOMPARE(KisNativeResultTestAccess::state(*source.m_nativeResult).logicalViews, sourceViews);
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
    auto *backend = dm->m_nativeResult.get(); QVERIFY(backend);
    const QByteArray initial(4, char(0x31)), value(4, char(0x61));
    if (warm) dm->writeBytes(reinterpret_cast<const quint8 *>(initial.constData()), 0, 0, 1, 1);
    const auto before = backend->capture(); QVERIFY(bool(before));
    const QPoint local(-65, 2), global = local + QPoint(source->x(), source->y());
    const QRect requested(global - QPoint(3, 3), QSize(8, 8));
    QByteArray cloned(4, char(0));
    bool observed = false, sourceCoherent = false, cloneSucceeded = false;
    KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        const auto wanted = atSeal ? KisPageStoreDiagnosticPhase::NativeAcceptPrepared
                                  : KisPageStoreDiagnosticPhase::NativeAdapterInstalled;
        if (phase != wanted || observed) return;
        observed = true;
        const auto current = backend->capture();
        auto read = current.readPage(-2, 0);
        sourceCoherent = read.data() &&
            static_cast<const quint8 *>(read.data())[2 * read.rowStride() + 63 * 4] == (atSeal ? 0 : 0x61);
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
    QVERIFY(observed); QVERIFY(sourceCoherent); QVERIFY(cloneSucceeded);
    QCOMPARE(cloned, atSeal ? QByteArray(4, char(0)) : value);
    if (atSeal) {
        // The first clone selected the old current while publication was
        // private. A separate clone after operation success selects the new one.
        KisPaintDeviceSP target = new KisPaintDevice(cs);
        KisStrokeJobFailureContext failures(true);
        if (rough) target->makeCloneFromRough(source, requested);
        else target->makeCloneFrom(source, requested);
        target->readBytes(reinterpret_cast<quint8 *>(cloned.data()), global.x(), global.y(), 1, 1);
        QVERIFY(!failures.failed()); QCOMPARE(cloned, value);
    }
    auto old = before.readPage(-2, 0);
    QVERIFY(old.data());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(old.data()) + 2 * old.rowStride() + 63 * 4, 4), QByteArray(4, char(0)));
}

void KisTiledDataManagerTest::testPageStoreFreshReadSelection_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("operation");
    QTest::addColumn<bool>("warm"); QTest::addColumn<bool>("hold");
    // Cursor, packed, clear, partial copy and full-page alias copy.
    for (int bpp : {1, 4, 8, 16}) for (int operation = 0; operation < 5; ++operation)
        for (bool warm : {false, true}) for (bool hold : {false, true})
            QTest::newRow(qPrintable(QString("B%1-op%2-W%3-H%4").arg(bpp).arg(operation).arg(warm).arg(hold)))
                << bpp << operation << warm << hold;
}

void KisTiledDataManagerTest::testPageStoreFreshReadSelection()
{
    QFETCH(int, bpp); QFETCH(int, operation); QFETCH(bool, warm); QFETCH(bool, hold);
    const QByteArray blank(bpp, 0), old(bpp, char(0x31)), value(bpp, char(0x61));
    KisTiledDataManager dm(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    KisTiledDataManager source(bpp, reinterpret_cast<const quint8 *>(blank.constData()));
    if (operation >= 3) source.clear(QRect(0, 0, 64, 64), reinterpret_cast<const quint8 *>(value.constData()));
    if (warm) dm.writeBytes(reinterpret_cast<const quint8 *>(old.constData()), 0, 0, 1, 1);
    auto *backend = dm.m_nativeResult.get();
    const auto before = backend->capture();
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
    const auto observe = [&](const QByteArray &expected, bool expectedPresence) {
        bool present = false;
        const auto lazy = dm.getReadOnlyTileLazy(0, 0, present);
        coherent &= present == expectedPresence && pixel(lazy) == expected && pixel(dm.getTile(0, 0, false)) == expected;
        if (held) coherent &= QByteArray(reinterpret_cast<const char *>(pointer), bpp) == (warm ? old : blank);
    };
    int sealed = 0, committed = 0, installed = 0, presenceResolutions = 0;
    bool publicationBlocked = false, publicationCoherent = false;
    QSemaphore publicationStart, publicationReady, publicationDone;
    std::atomic<bool> publicationRequested{false};
    std::thread publicationReader([&] {
        publicationStart.acquire();
        if (!publicationRequested.load()) return;
        const auto view = backend->capture();
        auto selected = view.readPage(0, 0);
        const bool canonical = selected.data() &&
            QByteArray(reinterpret_cast<const char *>(selected.data()), bpp) == value &&
            pixel(dm.getTile(0, 0, false)) == value;
        publicationReady.release();
        publicationCoherent = canonical && dm.extent() == QRect(0, 0, 64, 64);
        publicationDone.release();
    });
    const auto joinPublication = qScopeGuard([&] {
        publicationStart.release();
        if (publicationReader.joinable()) publicationReader.join();
    });
    KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase == KisPageStoreDiagnosticPhase::MutationPresenceResolve) ++presenceResolutions;
        if (phase == KisPageStoreDiagnosticPhase::NativeAcceptPrepared) {
            ++sealed;
            observe(warm ? old : blank, warm);
        }
        if (phase == KisPageStoreDiagnosticPhase::NativeSelectionInstalled) {
            ++committed;
            publicationRequested.store(true);
            publicationStart.release();
            QVERIFY(publicationReady.tryAcquire(1, 5000));
            publicationBlocked = !publicationDone.tryAcquire(1, 50);
        }
        if (phase == KisPageStoreDiagnosticPhase::NativeAdapterInstalled) {
            ++installed;
            if (publicationBlocked) QVERIFY(publicationDone.tryAcquire(1, 5000));
            observe(value, true);
        }
    });
    if (operation != 0) {
        KisStrokeJobFailureContext failure(true);
        if (operation == 1) dm.writeBytes(reinterpret_cast<const quint8 *>(value.constData()), 0, 0, 1, 1);
        else if (operation == 2) dm.clear(0, 0, 1, 1, reinterpret_cast<const quint8 *>(value.constData()));
        else if (operation == 3) dm.bitBlt(&source, QRect(0, 0, 1, 1));
        else dm.bitBltRough(&source, QRect(0, 0, 1, 1));
        QVERIFY(!failure.failed());
    } else {
        QCOMPARE(dm.writePageStoreOperation({QRect(0, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(0, 0); if (!cursor->rawData()) return false;
            std::memcpy(cursor->rawData(), value.constData(), size_t(bpp)); return true;
        }), KisPageStoreWriteOperationResult::Succeeded);
    }
    recorder.setPhaseObserver({});
    if (publicationReader.joinable()) publicationReader.join();
    QCOMPARE(sealed, 1); QCOMPARE(committed, 1); QCOMPARE(installed, 1);
    QCOMPARE(presenceResolutions, 0); // The admitted producer delivers its final presence.
    QVERIFY(publicationBlocked); QVERIFY(publicationCoherent);
    std::thread independentReader([&] { observe(value, true); }); independentReader.join();
    QVERIFY(coherent);
    if (held) { retained->unlockForRead(); held = false; }
    QCOMPARE(pixel(dm.getTile(0, 0, false)), value);
    auto fixed = before.readPage(0, 0); QVERIFY(fixed.data());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(fixed.data()), bpp), warm ? old : blank);
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
    auto *protocol = dm.m_nativeResult.get();
    const auto before = KisNativeResultTestAccess::state(*protocol);
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
    const auto after = KisNativeResultTestAccess::state(*protocol);
    QCOMPARE(after.pageSlots, before.pageSlots);
    QCOMPARE(after.physicalResources, before.physicalResources);
    QCOMPARE(after.acceptedEpoch, before.acceptedEpoch);
    QCOMPARE(after.logicalViews, before.logicalViews);
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
    auto *protocol = dm.m_nativeResult.get();
    const auto before = KisNativeResultTestAccess::state(*protocol);
    for (const auto &value : {next, blank, next}) {
        dm.setDefaultPixel(reinterpret_cast<const quint8 *>(value.constData()));
        bool present = true;
        auto fresh = dm.getReadOnlyTileLazy(2, 0, present); QVERIFY(fresh); QCOMPARE(present, intent);
        QVERIFY(fresh->lockForRead());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(fresh->data()), bpp), intent ? blank : value);
        fresh->unlockForRead();
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(oldPointer), bpp), blank);
    }
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).pageSlots, before.pageSlots);
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).physicalResources, before.physicalResources);
}

void KisTiledDataManagerTest::testPageStoreFreshReadFailure()
{
    const quint8 blank = 0, value = 0x61;
    KisTiledDataManager dm(1, &blank);
    dm.writeBytes(&value, 0, 0, 1, 1);
    auto retained = dm.getTile(0, 0, false); QVERIFY(retained);
    QVERIFY(dm.m_nativeResult->close());
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
    auto *backend = dm.m_nativeResult.get();
    // Native acceptance updates metadata and adapter at one cut. Its uniform
    // backing may still need first RAM materialization; hard pressure may
    // refuse that new pointer, without touching the already held old pointer.
    QVERIFY(backend->setDefaultPixel(next));
    const auto before = KisNativeResultTestAccess::state(*backend);
    budget.saturate(4, reinterpret_cast<const quint8 *>(blank.constData()));
    {
        KisStrokeJobFailureContext failure(true);
        bool present = true;
        QVERIFY(!dm.getReadOnlyTileLazy(0, 0, present));
        QVERIFY(!present); QVERIFY(failure.failed());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(pointer), 4), blank);
    }
    budget.release();
    bool present = true;
    auto fresh = dm.getReadOnlyTileLazy(0, 0, present); QVERIFY(fresh); QVERIFY(!present);
    QVERIFY(fresh->lockForRead());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(fresh->data()), 4), next);
    fresh->unlockForRead();
    QCOMPARE(KisNativeResultTestAccess::state(*backend).pageSlots, before.pageSlots);
    QCOMPARE(KisNativeResultTestAccess::state(*backend).physicalResources, before.physicalResources);
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
    auto *protocol = dm.m_nativeResult.get();
    auto tile = dm.getTile(0, 0, false);
    QVERIFY(tile);
    const auto requests = KisNativeResultTestAccess::state(*protocol).activeResults;
    const auto captures = KisNativeResultTestAccess::state(*protocol).logicalViews;
    for (int i = 0; i != 1000; ++i) {
        QVERIFY(tile->lockForRead());
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(tile->data()), bpp), initial);
        QVERIFY(tile->lockForRead());
        tile->unlockForRead();
        tile->unlockForRead();
    }
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).activeResults, requests);
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).logicalViews, captures);
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
            const auto before = KisNativeResultTestAccess::state(*protocol).logicalViews;
            QVERIFY(tile->lockForRead());
            QCOMPARE(QByteArray(reinterpret_cast<const char *>(tile->data()), bpp), QByteArray(bpp, char(0x60 + i)));
            tile->unlockForRead();
            QCOMPARE(KisNativeResultTestAccess::state(*protocol).logicalViews, before);
            QCOMPARE(KisNativeResultTestAccess::state(*protocol).activeResults, requests);
        }
    } else if (boundary == 1 || boundary == 2) {
        const QByteArray replacement(bpp, char(0x52));
        if (boundary == 1) dm.clear(); // detached
        else {
            // Replacing the hash entry invokes notifyDeadWithoutDetaching.
            // Normal canonical clear refreshes the same wrapper in place,
            // so it does not exercise this lifetime boundary by itself.
            KisTileSP newTile = new KisTile(*tile, 0, 0, dm.m_mementoManager);
            newTile->setPageStoreBridge(dm.m_mementoManager->pageStoreBridge(), false);
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
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).activeResults, quint64(0));
}

void KisTiledDataManagerTest::testPageStoreWritablePresencePrecedesDataExposure()
{
    quint8 blank = 0;
    KisTiledDataManager dm(1,&blank);
    auto &protocol = *dm.m_nativeResult;
    const auto before = KisNativeResultTestAccess::state(protocol);
    auto tile = dm.getTile(7,-3,true); QVERIFY(tile);
    QCOMPARE(dm.extent(),QRect(7*64,-3*64,64,64));
    const auto presence = KisNativeResultTestAccess::state(protocol);
    QCOMPARE(presence.pageSlots,before.pageSlots+1);
    QCOMPARE(presence.physicalResources,before.physicalResources+1);
    QVERIFY(tile->lockForWrite()); tile->unlockForWrite();
    QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch,presence.acceptedEpoch);
    QVERIFY(tile->lockForWrite()); QVERIFY(tile->data()); tile->data()[0]=0x71; tile->unlockForWrite();
    QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch,presence.acceptedEpoch+1);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
    bool present=false;auto read=dm.getReadOnlyTileLazy(7,-3,present);QVERIFY(read && present && read->lockForRead());
    QCOMPARE(read->data()[0],quint8(0x71));read->unlockForRead();
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
    auto *protocol = dm.m_nativeResult.get();
    const auto before = KisNativeResultTestAccess::state(*protocol);
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

    QCOMPARE(KisNativeResultTestAccess::state(*protocol).activeResults, quint64(0));
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).acceptedEpoch, before.acceptedEpoch + 1);
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

void KisTiledDataManagerTest::testPageStoreReadLastReleasesWriteIntentPin()
{
    const quint8 blank = 0, initial = 0x31;
    KisTiledDataManager dm(1, &blank);
    dm.clear(QRect(0, 0, 64, 64), &initial);
    KisTileSP tile = dm.getTile(0, 0, true);
    QVERIFY(tile);

    // An unexposed writer can finish before a nested reader. That reader owns
    // the final unlock of the native pin originally acquired by write intent.
    QVERIFY(tile->lockForWrite());
    QVERIFY(tile->lockForRead());
    tile->unlockForWrite();
    tile->unlockForRead();

    QVERIFY(tile->lockForRead());
    const auto unlockRead = qScopeGuard([&] { tile->unlockForRead(); });
    KisTileData *before = tile->tileData();
    QVERIFY(before);
    quint8 *bytes = before->data();
    QVERIFY(bytes);
    QCOMPARE(*bytes, initial);

    QVERIFY(tile->lockForWrite());
    const auto unlockWrite = qScopeGuard([&] { tile->unlockForWrite(); });
    auto *write = tile->tryWriteData();
    QVERIFY(write);
    QVERIFY(write != bytes);
    *write = 0x62;

    // Retention of the old identity alone is insufficient: the outstanding
    // reader's actual storage must remain pinned after the new writer resolves.
    QVERIFY(!KisTileDataStore::instance()->trySwapTileData(before));
    QCOMPARE(before->data(), bytes);
    QCOMPARE(*bytes, initial);
    QCOMPARE(*write, quint8(0x62));
}

void KisTiledDataManagerTest::testPageStoreClearBarrierCancelsLegacyWriter()
{
    const quint8 blank = 0;
    const quint8 initial = 0x31;
    KisTiledDataManager dm(1, &blank);
    dm.clear(QRect(0, 0, 64, 64), &initial);
    auto *protocol = dm.m_nativeResult.get();
    const auto before = KisNativeResultTestAccess::state(*protocol);
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
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).activeResults, quint64(0));
    QCOMPARE(KisNativeResultTestAccess::state(*protocol).acceptedEpoch, before.acceptedEpoch + 1);
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
    auto *backend = dm.m_nativeResult.get();
    auto before = backend->capture();
    QVERIFY(bool(before));
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
    QVERIFY(KisNativeResultTestAccess::state(*backend).activeResults == 1);
    replacement->moveTo(65, 1);
    QVERIFY(replacement->rawData());
    memset(replacement->rawData(), 0x46, bpp);
    replacement.reset();
    QCOMPARE(KisNativeResultTestAccess::state(*backend).activeResults, quint64(0));

    QByteArray actual(bpp, Qt::Uninitialized);
    auto readPixel = [&](int x, int y) {
        dm.readBytes(reinterpret_cast<quint8 *>(actual.data()), x, y, 1, 1);
        return actual;
    };
    QCOMPARE(readPixel(1, 1), QByteArray(bpp, char(0x45)));
    QCOMPARE(readPixel(65, 1), QByteArray(bpp, char(0x46)));
    QCOMPARE(readPixel(2, 1), blank);
    QCOMPARE(readPixel(129, 1), blank);
    auto old = before.readPage(0, 0);
    QVERIFY(old.data());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(old.data()) + 65 * bpp, bpp), initial);
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
    QVERIFY(!dm.m_nativeResult->clear(&error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(KisNativeResultTestAccess::state(*dm.m_nativeResult).activeResults,quint64(2));
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
    QCOMPARE(KisNativeResultTestAccess::state(*dm.m_nativeResult).activeResults,quint64(0));
}

void KisTiledDataManagerTest::testClearRejectsNativeBatch()
{
    const quint8 blank = 0, initial = 0x31;
    KisDataManager dm(1, &blank);
    dm.clear(0, 0, 64, 64, &initial);
    auto batch=dm.m_nativeResult->prepareProducer(QRect(0,0,64,64));
    QVERIFY(batch);
    QString error;
    QVERIFY(!dm.m_nativeResult->clear(&error));
    QVERIFY(!error.isEmpty());
    dm.m_nativeResult->cancel(batch);
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
    KisDataManager dm(1,&blank);auto &backend=*dm.m_nativeResult;
    auto scope = iteratorBatch ? backend.beginIterator() : nullptr;
    if (iteratorBatch) QVERIFY(scope);
    // No compatibility Tile owns this backing. Cancellation must retain both
    // a residency pin and a reference until the actual lease is returned.
    auto lease = KisNativeResultTestAccess::tiles(backend).acquireTile(0, 0, KisTilePageStoreBridge::Access::Write);
    QVERIFY(lease && lease->writable());
    auto *tile = lease->tileData();
    auto *borrowed = tile->data();
    QVERIFY(borrowed);
    *borrowed = 0x62;
    QVERIFY(backend.clear());
    QVERIFY(!KisTileDataStore::instance()->trySwapTileData(tile));
    *borrowed = 0x73;
    if (scope) {
        QVERIFY(!scope->isActive());
        QVERIFY(scope->finish());
    }
    QVERIFY(lease->finish());
    lease.reset();
    QCOMPARE(KisNativeResultTestAccess::state(backend).activeResults,quint64(0));
    QCOMPARE(KisNativeResultTestAccess::state(backend).activeResults,quint64(0));
    quint8 actual = 0xff;
    QVERIFY(backend.readCurrent(&actual,QRect(0,0,1,1),1));
    QCOMPARE(actual, blank);
}

void KisTiledDataManagerTest::testPageStoreWholeTileFillUsesSynchronousAdoption()
{
    quint8 blank=0, color=0x4d, changed=0x71;
    KisTiledDataManager dm(1,&blank);auto &protocol=*dm.m_nativeResult;
    const auto before=KisNativeResultTestAccess::state(protocol);
    dm.clear(QRect(0,0,128,128),&color);
    const auto filled=KisNativeResultTestAccess::state(protocol);
    QCOMPARE(filled.pageSlots,before.pageSlots+4);
    QCOMPARE(filled.physicalResources,before.physicalResources+1);
    QCOMPARE(filled.acceptedEpoch,before.acceptedEpoch+1);
    dm.clear(QRect(0,0,128,128),&color);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).physicalResources,filled.physicalResources);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch,filled.acceptedEpoch);
    // A later operation adopts the same physical source at a different page.
    // Logical generations may multiply; physical registration and its funded
    // retirement credit must not. This is independent of one-batch aliasing.
    dm.clear(QRect(128,0,64,64),&color);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).physicalResources,filled.physicalResources);
    QCOMPARE(protocol.usage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].reserved.cpuRam,quint64(4096));
    // A shared source is not tied to its first page's logical birth key.
    auto old=protocol.capture();QVERIFY(old);
    dm.writeBytes(&changed,65,65,1,1);
    quint8 byte=0;dm.readBytes(&byte,65,65,1,1);QCOMPARE(byte,changed);
    dm.readBytes(&byte,1,1,1,1);QCOMPARE(byte,color);
    QVERIFY(old.read(&byte,QRect(65,65,1,1),1));QCOMPARE(byte,color);old={};
    dm.clear(QRect(0,0,192,128),&blank);QCOMPARE(dm.extent(),QRect());
    QCOMPARE(KisNativeResultTestAccess::state(protocol).activeResults,quint64(0));
    // Two cold disjoint results may discover the same uniform source before
    // either result is visible. Physical birth still has one registration.
    KisTiledDataManager concurrent(1,&blank);
    QSemaphore ready,start;
    bool left=false,right=false;
    const auto fill=[&](int x,bool &ok){ready.release();start.acquire();KisStrokeJobFailureContext failure(true);
        concurrent.clear(QRect(x,0,64,64),&color);ok=!failure.failed();};
    std::thread a([&]{fill(0,left);}),b([&]{fill(64,right);});
    ready.acquire(2);start.release(2);a.join();b.join();QVERIFY(left);QVERIFY(right);
    auto &shared=*concurrent.m_nativeResult;
    QCOMPARE(KisNativeResultTestAccess::state(shared).physicalResources,quint64(1));
    QCOMPARE(shared.usage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].reserved.cpuRam,quint64(4096));
    concurrent.writeBytes(&changed,65,0,1,1);
    concurrent.readBytes(&byte,1,0,1,1);QCOMPARE(byte,color);
    concurrent.readBytes(&byte,65,0,1,1);QCOMPARE(byte,changed);
}

void KisTiledDataManagerTest::testPageStoreBulkNoOpAndBitBltBatching()
{
    constexpr int side=128;quint8 blank=0,color=0x31;
    KisTiledDataManager dm(1,&blank);auto &protocol=*dm.m_nativeResult;
    dm.clear(QRect(0,0,side,side),&color);
    QByteArray bytes(side*side,Qt::Uninitialized);dm.readBytes(reinterpret_cast<quint8 *>(bytes.data()),0,0,side,side);
    const auto before=KisNativeResultTestAccess::state(protocol);
    dm.writeBytes(reinterpret_cast<const quint8 *>(bytes.constData()),0,0,side,side);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch,before.acceptedEpoch);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).physicalResources,before.physicalResources);
    bytes[0]=char(0x72);dm.writeBytes(reinterpret_cast<const quint8 *>(bytes.constData()),0,0,side,side);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch,before.acceptedEpoch+1);
    quint8 sourceDefault=0x5a;KisTiledDataManager source(1,&sourceDefault);
    dm.bitBlt(&source,QRect(0,0,side,side));
    const auto copied=KisNativeResultTestAccess::state(protocol);
    QCOMPARE(copied.acceptedEpoch,before.acceptedEpoch+2);
    dm.bitBlt(&source,QRect(0,0,side,side));
    QCOMPARE(KisNativeResultTestAccess::state(protocol).acceptedEpoch,copied.acceptedEpoch);
    QCOMPARE(KisNativeResultTestAccess::state(protocol).physicalResources,copied.physicalResources);
    dm.readBytes(reinterpret_cast<quint8 *>(bytes.data()),0,0,side,side);QCOMPARE(bytes,QByteArray(bytes.size(),char(sourceDefault)));
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
    auto *backend = dm.m_nativeResult.get();
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
        QVERIFY(KisNativeResultTestAccess::state(*backend).activeResults == 1);
        writer.nextRow();
        QVERIFY(!writer.rawData());
        QVERIFY(KisNativeResultTestAccess::state(*backend).activeResults == 1);
    } else if (entry == 1) {
        KisVLineIterator2 writer(&dm, 63, 0, 1, 0, 0, true, nullptr);
        QVERIFY(writer.rawData());
        QVERIFY(KisNativeResultTestAccess::state(*backend).activeResults == 1);
        writer.nextColumn();
        QVERIFY(!writer.rawData());
        QVERIFY(KisNativeResultTestAccess::state(*backend).activeResults == 1);
    } else {
        KisRandomAccessor2 writer(&dm, 0, 0, true, nullptr);
        QVERIFY(KisNativeResultTestAccess::state(*backend).activeResults == 1);
        writer.moveTo(0, 0);
        QVERIFY(!writer.rawData());
        QVERIFY(KisNativeResultTestAccess::state(*backend).activeResults == 1);
    }
    QCOMPARE(KisNativeResultTestAccess::state(*backend).activeResults, quint64(0));
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
    auto *backend = dm.m_nativeResult.get();
    const auto memento = history ? dm.getMemento() : KisMementoSP{};
    const auto before = KisNativeResultTestAccess::state(*backend);

    auto horizontal = std::make_unique<KisHLineIterator2>(
        &dm, 0, 0, 1, 0, 0, true, nullptr);
    auto random = std::make_unique<KisRandomAccessor2>(
        &dm, 0, 0, true, nullptr);
    random->moveTo(64, 0);
    QVERIFY(horizontal->rawData());
    QVERIFY(random->rawData());
    horizontal->rawData()[0] = 0x51;
    random->rawData()[0] = 0x62;
    QVERIFY(KisNativeResultTestAccess::state(*backend).activeResults == 1);
    if (horizontalFirst) horizontal.reset();
    else random.reset();
    QVERIFY(KisNativeResultTestAccess::state(*backend).activeResults == 1);
    QCOMPARE(KisNativeResultTestAccess::state(*backend).acceptedEpoch, before.acceptedEpoch);
    if (horizontalFirst) QCOMPARE(random->rawData()[0], quint8(0x62));
    else QCOMPARE(horizontal->rawData()[0], quint8(0x51));

    horizontal.reset();
    random.reset();
    QCOMPARE(KisNativeResultTestAccess::state(*backend).activeResults, quint64(0));
    QCOMPARE(KisNativeResultTestAccess::state(*backend).acceptedEpoch, before.acceptedEpoch + 1);
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
        // Native read cursors pin only the visited tile, never the whole line.
        QVERIFY(iterator.rawDataConst());
        QVERIFY(!iterator.nextPixels(64));
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
        // Native read cursors pin only the visited tile, never the whole line.
        QVERIFY(iterator.rawDataConst());
        QVERIFY(!iterator.nextPixels(64));
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
    KisStressJob(KisTiledDataManager &dataManager, QRect rect, QReadWriteLock &_lock, int worker)
        : m_accessRect(rect), dm(dataManager), lock(_lock), m_worker(worker)
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
        KisPageStoreDiagnosticRecorder stages(stageProfile);
        KisBr1StressProfile profile(lock, m_worker);
        // The wrapper measures test-level lock waiting separately from the
        // operation body. Worker-time totals are not additive wall time.
        KisBr1StressProfile &profiledLock = profile;
        for(qint32 i = 0; i < NUM_CYCLES; i++) {
            qint32 type = rng.bounded(NUM_TYPES);
            profile.operation(type);
            KisStrokeJobFailureContext failure(true);
            const auto requireSuccess = [&](const char *operation, bool result = true) {
                if (!result || failure.failed()) {
                    qFatal("BR1 stress first failure: seed=%u worker=%d cycle=%d type=%d operation=%s result=%d error=%s",
                           workerSeed, m_worker, i, type, operation, int(result),
                           qPrintable(failure.error()));
                }
            };

            qint32 t;

            switch(type) {
            case 0:
                run_concurrent(profiledLock,t) {
                    quint8 *buf;
                    buf = new quint8[dm.pixelSize()];
                    memcpy(buf, dm.defaultPixel(), dm.pixelSize());
                    dm.setDefaultPixel(buf);
                    requireSuccess("setDefaultPixel");
                    delete[] buf;
                }
                break;
            case 1:
            case 2:
                run_concurrent(profiledLock,t) {
                    KisTileSP tile;

                    tile = dm.getTile(m_accessRect.x() / TILE_DIMENSION,
                                      m_accessRect.y() / TILE_DIMENSION, false);
                    requireSuccess("getTile(read)", bool(tile));
                    tile->lockForRead();
                    tile->unlockForRead();
                    requireSuccess("tileRead");
                    tile = dm.getTile(m_accessRect.x() / TILE_DIMENSION,
                                      m_accessRect.y() / TILE_DIMENSION, true);
                    requireSuccess("getTile(write)", bool(tile));
                    tile->lockForWrite();
                    tile->unlockForWrite();
                    requireSuccess("tileWrite");

                    tile = dm.getOldTile(m_accessRect.x() / TILE_DIMENSION,
                                         m_accessRect.y() / TILE_DIMENSION);
                    requireSuccess("getOldTile", bool(tile));
                    tile->lockForRead();
                    tile->unlockForRead();
                    requireSuccess("oldTileRead");
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
                    requireSuccess("readBytes");
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
                    requireSuccess("getMemento", bool(m_memento));
                    profile.phase(KisBr1StressProfile::HistoryClear, [&] {
                        dm.clear(m_accessRect.x(), m_accessRect.y(),
                                 m_accessRect.width(), m_accessRect.height(), 2);
                    });
                    requireSuccess("historyClear");
                    profile.phase(KisBr1StressProfile::HistoryCommit, [&] { dm.commit(); });
                    requireSuccess("commit");
                    profile.phase(KisBr1StressProfile::Rollback, [&] { dm.rollback(m_memento); });
                    requireSuccess("rollback");
                    profile.phase(KisBr1StressProfile::Rollforward, [&] { dm.rollforward(m_memento); });
                    requireSuccess("rollforward");
                    profile.phase(KisBr1StressProfile::Purge, [&] { dm.purgeHistory(m_memento); });
                    requireSuccess("purgeHistory");
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
            requireSuccess("operation complete");
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
        KisStressJob *job = new KisStressJob(dm, accessRect, lock, i);
        pool.start(job);
        accessRect.translate(512, 0);
    }
    pool.waitForDone();

    kisDrainPageStoreReclamation();auto &protocol=*dm.m_nativeResult;
    const auto settled=KisNativeResultTestAccess::state(protocol);
    QCOMPARE(settled.activeResults,quint64(0));QCOMPARE(settled.historyEdges,quint64(0));QCOMPARE(settled.domainMoves,quint64(0));
    const auto usage=protocol.usage();
    for(size_t i=0;i<usage.buckets.size();++i){const auto &bucket=usage.buckets[i];
        QCOMPARE(bucket.reserved.cpuRam,i==size_t(KisBackingBudgetClass::RetirementDebt)?settled.physicalResources*4096:quint64(0));
        QCOMPARE(bucket.reserved.umaShared,quint64(0));QCOMPARE(bucket.reserved.discreteVram,quint64(0));QCOMPARE(bucket.reserved.ssd,quint64(0));
    }
    for(const auto kind:{KisBackingBudgetClass::ActivePending,KisBackingBudgetClass::InFlight,KisBackingBudgetClass::RetirementDebt}){
        const auto live=usage.buckets[size_t(kind)].live;QCOMPARE(live.cpuRam,quint64(0));QCOMPARE(live.umaShared,quint64(0));QCOMPARE(live.discreteVram,quint64(0));QCOMPARE(live.ssd,quint64(0));
    }
    if(qEnvironmentVariableIntValue("KIS_BR1_PROFILE"))qInfo().noquote()<<"BR1_NATIVE_STATE"<<QJsonDocument(QJsonObject{
        {"threads",pool.maxThreadCount()},{"accepted_epoch",double(settled.acceptedEpoch)},{"page_slots",double(settled.pageSlots)},
        {"physical_resources",double(settled.physicalResources)},{"logical_views",double(settled.logicalViews)}
    }).toJson(QJsonDocument::Compact);
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

/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <simpletest.h>
#include <testing_timed_default_bounds.h>
#include <KoColorSpaceRegistry.h>
#include <KoColor.h>
#include <atomic>
#include <QScopeGuard>
#include <KoCompositeOpRegistry.h>

#include "kis_image.h"
#include "kis_paint_layer.h"
#include "kis_paint_device.h"
#include "kis_datamanager.h"
#include "kis_iterator_ng.h"
#include "kis_painter.h"
#include "kis_fixed_paint_device.h"
#include "KisPixelWriteCursor.h"
#include "KisStrokeJobFailureContext.h"
#include "tiles3/kis_tile.h"
#include "tiles3/kis_tile_data_store.h"
#include "tiles3/tests/kis_tile_data_store_test_access.h"
#include "kis_transaction.h"
#include "kis_undo_stores.h"
#include "kis_resources_snapshot.h"
#include "kis_raster_keyframe_channel.h"
#include "KisAnimAutoKey.h"
#include "KisGlobalResourcesInterface.h"
#include "kis_paintop_preset.h"
#include "kis_paintop_settings.h"
#include "kis_paintop_factory.h"
#include "kis_paintop_registry.h"
#include "KoResourceLoadResult.h"
#include "strokes/kis_painter_based_stroke_strategy.h"

namespace {
struct CompletionState {
    KisPaintDeviceSP target;
    std::atomic<bool> painted{false};
    std::atomic<bool> destroyed{false};
    std::atomic<int> finishes{0};
    std::atomic<int> cancellations{0};
    std::atomic<int> paintJobs{0};
    std::atomic<bool> failureContract{false};
};

// Exercise the production strategy's transaction and scheduler boundary with
// deterministic pixels. Brush kernels and their performance are separate tests.
class CompletionStrategy : public KisPainterBasedStrokeStrategy
{
public:
    struct PaintData : KisStrokeJobData {};
    struct FailureData : KisStrokeJobData {
        FailureData(KisPaintDeviceSP source, int entry) : source(source), entry(entry) {}
        KisPaintDeviceSP source;
        int entry;
    };
    CompletionStrategy(KisResourcesSnapshotSP resources, CompletionState &state, bool indirect)
        : KisPainterBasedStrokeStrategy(QLatin1String("completion-test"),
              kundo2_noi18n("Completion test"), resources, QVector<KisFreehandStrokeInfo*>()), state(state)
    {
        setSupportsIndirectPainting(indirect);
        enableJob(JOB_DOSTROKE);
    }
    ~CompletionStrategy() override { state.destroyed = true; }
    void doStrokeCallback(KisStrokeJobData *data) override {
        if (dynamic_cast<PaintData *>(data)) {
            state.target = targetDevice();
            state.target->fill(QRect(0, 0, 64, 64), KoColor(Qt::red, state.target->colorSpace()));
            ++state.paintJobs;
            state.painted = true;
        } else if (auto *failure = dynamic_cast<FailureData *>(data)) {
            if (failure->entry == 0) {
                // A result-bearing callback can reject just this operation.
                // It must not silently opt into whole-stroke cancellation.
                const bool accepted = state.target->applyPixelOperation(QRect(0, 0, 1, 1),
                    [&](KisPixelWriteCursor *cursor) {
                        cursor->moveTo(0, 0);
                        if (auto *pixel = cursor->rawData())
                            state.target->colorSpace()->fromQColor(Qt::blue, pixel);
                        return false;
                    });
                QColor actual;
                state.target->pixel(0, 0, &actual);
                state.failureContract = !accepted && actual == QColor(Qt::red) &&
                    !KisStrokeJobFailureContext::currentJobHasFailed();
            } else {
                KisPainter painter(state.target);
                if (failure->entry == 2) painter.setCompositeOpId(COMPOSITE_COPY);
                if (failure->entry == 3) {
                    KisFixedPaintDeviceSP selection(new KisFixedPaintDevice(KoColorSpaceRegistry::instance()->alpha8()));
                    selection->setRect(QRect(0, 0, 128, 64));
                    if (!selection->initialize(255)) return;
                    painter.bitBltWithFixedSelection(1, 0, failure->source, selection, 1, 0, 1, 0, 126, 64);
                } else {
                    painter.bitBlt(1, 0, failure->source, 1, 0, 126, 64);
                }
                bool callbackExecuted = false;
                const bool subsequent = state.target->applyPixelOperation(QRect(0, 0, 1, 1),
                    [&](KisPixelWriteCursor *) { callbackExecuted = true; return true; });
                state.failureContract = KisStrokeJobFailureContext::currentJobHasFailed() &&
                    !subsequent && !callbackExecuted;
            }
        } else {
            KisPainterBasedStrokeStrategy::doStrokeCallback(data);
        }
    }
    void finishStrokeCallback() override {
        ++state.finishes;
        KisPainterBasedStrokeStrategy::finishStrokeCallback();
    }
    void cancelStrokeCallback() override {
        KisPainterBasedStrokeStrategy::cancelStrokeCallback();
        ++state.cancellations;
    }
    CompletionState &state;
};

class WashSettings : public KisPaintOpSettings
{
public:
    WashSettings() : KisPaintOpSettings(KisGlobalResourcesInterface::instance()) {
        setProperty("paintop", "completion-test-no-kernel");
    }
    bool paintIncremental() override { return false; }
    KisPaintOpSettingsSP clone() const override { return new WashSettings(*this); }
    void setPaintOpSize(qreal) override {}
    qreal paintOpSize() const override { return 1.0; }
    void setPaintOpAngle(qreal) override {}
    qreal paintOpAngle() const override { return 0.0; }
};

class CompletionFactory : public KisPaintOpFactory
{
public:
    QString id() const override { return "completion-test-no-kernel"; }
    QString name() const override { return id(); }
    QString category() const override { return id(); }
    bool lodSizeThresholdSupported() const override { return false; }
    KisPaintOp *createOp(KisPaintOpSettingsSP, KisPainter *, KisNodeSP, KisImageSP) override { return nullptr; }
    KisPaintOpSettingsSP createSettings(KisResourcesInterfaceSP) override { return new WashSettings(); }
    KisPaintOpConfigWidget *createConfigWidget(QWidget *, KisResourcesInterfaceSP, KoCanvasResourcesInterfaceSP) override { return nullptr; }
    QList<KoResourceLoadResult> prepareLinkedResources(KisPaintOpSettingsSP, KisResourcesInterfaceSP) override { return {}; }
    QList<KoResourceLoadResult> prepareEmbeddedResources(KisPaintOpSettingsSP, KisResourcesInterfaceSP) override { return {}; }
};

struct AutoKeyModeGuard {
    const KisAutoKey::Mode previous = KisAutoKey::activeMode();
    explicit AutoKeyModeGuard(bool enabled) {
        KisAutoKey::testingSetActiveMode(enabled ? KisAutoKey::DUPLICATE : KisAutoKey::NONE);
    }
    ~AutoKeyModeGuard() { KisAutoKey::testingSetActiveMode(previous); }
};
}

class KisPainterStrokeCompletionTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { KisPaintOpRegistry::instance()->add(new CompletionFactory()); }
    void testCompletion_data();
    void testCompletion();
    void testRejectedBegin();
    void testPixelFailure_data();
    void testPixelFailure();
};

void KisPainterStrokeCompletionTest::testCompletion_data()
{
    QTest::addColumn<bool>("indirect");
    QTest::addColumn<bool>("autoKey");
    QTest::addColumn<int>("endpoint");
    for (bool indirect : {false, true}) {
        for (bool autoKey : {false, true}) {
            for (int endpoint = 0; endpoint < 3; ++endpoint) {
                QTest::newRow(qPrintable(QString("%1-autokey%2-endpoint%3")
                    .arg(indirect ? "indirect" : "direct").arg(autoKey).arg(endpoint)))
                    << indirect << autoKey << endpoint;
            }
        }
    }
}

void KisPainterStrokeCompletionTest::testCompletion()
{
    QFETCH(bool, indirect);
    QFETCH(bool, autoKey);
    QFETCH(int, endpoint); // 0: success, 1: rejected finish, 2: rejected user cancel
    AutoKeyModeGuard mode(autoKey);
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
    auto *store = new KisSurrogateUndoStore();
    KisImageSP image = new KisImage(store, 128, 64, cs, "stroke completion");
    KisPaintLayerSP layer = new KisPaintLayer(image, "paint", OPACITY_OPAQUE_U8);
    image->addNode(layer);
    image->waitForDone();
    auto device = layer->paintDevice();
    auto *bounds = new TestUtil::TestingTimedDefaultBounds();
    device->setDefaultBounds(bounds);
    if (autoKey) device->createKeyframeChannel(KisKeyframeChannel::Raster);
    device->fill(QRect(0, 0, 128, 64), KoColor(Qt::white, cs));
    if (autoKey) bounds->testingSetTime(10);

    KisResourcesSnapshotSP resources = new KisResourcesSnapshot(image, layer);
    if (indirect) {
        KisPaintOpPresetSP preset(new KisPaintOpPreset());
        preset->setSettings(new WashSettings());
        resources->setBrush(preset);
    }
    CompletionState state;
    const auto id = image->startStroke(new CompletionStrategy(resources, state, indirect));
    image->addJob(id, new CompletionStrategy::PaintData());
    QTRY_VERIFY_WITH_TIMEOUT(state.painted.load(), 5000);
    if (autoKey) QVERIFY(device->keyframeChannel()->keyframeAt(10));
    if (indirect) QVERIFY(layer->hasTemporaryTarget());

    KisHLineIteratorSP writer;
    if (endpoint != 0) {
        writer = state.target->createHLineIteratorNG(64, 0, 1);
        cs->fromQColor(Qt::blue, writer->rawData());
    }
    if (endpoint == 2) image->cancelStroke(id);
    else image->endStroke(id);

    if (endpoint != 0) {
        QTRY_VERIFY_WITH_TIMEOUT(state.cancellations.load() >= 2, 5000);
        QVERIFY(!state.destroyed);
        QVERIFY(!store->presentCommand());
        QVERIFY(state.target->dataManager()->hasCurrentMemento());
        if (autoKey) QVERIFY(device->keyframeChannel()->keyframeAt(10));
        if (indirect) QVERIFY(layer->hasTemporaryTarget());
        writer.clear(); // no further stroke input should be needed to finish cleanup
    }
    QTRY_VERIFY_WITH_TIMEOUT(state.destroyed.load(), 5000);
    image->waitForDone();
    QVERIFY(!layer->hasTemporaryTarget());
    QVERIFY(!state.target->dataManager()->hasCurrentMemento());
    QCOMPARE(state.finishes.load(), endpoint == 2 ? 0 : 1);
    QColor actual;
    device->pixel(0, 0, &actual);
    QCOMPARE(actual, QColor(endpoint == 0 ? Qt::red : Qt::white));
    device->pixel(64, 0, &actual); QCOMPARE(actual, QColor(Qt::white));
    if (endpoint == 0) {
        QVERIFY(store->presentCommand());
        store->undo(); image->waitForDone();
        device->pixel(0, 0, &actual); QCOMPARE(actual, QColor(Qt::white));
        if (autoKey) QVERIFY(!device->keyframeChannel()->keyframeAt(10));
        store->redo(); image->waitForDone();
        device->pixel(0, 0, &actual); QCOMPARE(actual, QColor(Qt::red));
        if (autoKey) QVERIFY(device->keyframeChannel()->keyframeAt(10));
    } else {
        QVERIFY(!store->presentCommand());
        if (autoKey) QVERIFY(!device->keyframeChannel()->keyframeAt(10));
    }
}

void KisPainterStrokeCompletionTest::testRejectedBegin()
{
    AutoKeyModeGuard mode(false);
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
    auto *store = new KisSurrogateUndoStore();
    KisImageSP image = new KisImage(store, 128, 64, cs, "rejected stroke begin");
    KisPaintLayerSP layer = new KisPaintLayer(image, "paint", OPACITY_OPAQUE_U8);
    image->addNode(layer);
    image->waitForDone();
    auto device = layer->paintDevice();
    device->fill(QRect(0, 0, 64, 64), KoColor(Qt::white, cs));
    KisTransaction owner(device);
    device->fill(QRect(0, 0, 64, 64), KoColor(Qt::blue, cs));
    CompletionState state;
    KisResourcesSnapshotSP resources = new KisResourcesSnapshot(image, layer);
    const auto id = image->startStroke(new CompletionStrategy(resources, state, false));
    image->addJob(id, new CompletionStrategy::PaintData());
    image->endStroke(id);
    image->waitForDone();
    QVERIFY(state.destroyed);
    QVERIFY(!state.painted);
    QCOMPARE(state.finishes.load(), 0);
    QCOMPARE(state.cancellations.load(), 1);
    QVERIFY(!store->presentCommand());
    QVERIFY(device->dataManager()->hasCurrentMemento());
    QColor actual;
    device->pixel(0, 0, &actual); QCOMPARE(actual, QColor(Qt::blue));
    QVERIFY(owner.tryRevert());
    device->pixel(0, 0, &actual); QCOMPARE(actual, QColor(Qt::white));
}

void KisPainterStrokeCompletionTest::testPixelFailure_data()
{
    QTest::addColumn<bool>("indirect");
    QTest::addColumn<int>("entry");
    QTest::addColumn<int>("failurePoint");
    for (bool indirect : {false, true}) {
        QTest::newRow(qPrintable(QString("handled-indirect%1").arg(indirect))) << indirect << 0 << 0;
        for (int entry = 1; entry <= 3; ++entry)
            for (auto point : {KisSwapInFailurePoint::Mapping, KisSwapInFailurePoint::Allocation,
                               KisSwapInFailurePoint::Decompression})
                QTest::newRow(qPrintable(QString("entry%1-indirect%2-swap%3").arg(entry).arg(indirect).arg(int(point))))
                    << indirect << entry << int(point);
    }
}

void KisPainterStrokeCompletionTest::testPixelFailure()
{
    QFETCH(bool, indirect);
    QFETCH(int, entry);
    QFETCH(int, failurePoint);
    AutoKeyModeGuard mode(false);
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
    auto *store = new KisSurrogateUndoStore();
    KisImageSP image = new KisImage(store, 128, 64, cs, "pixel failure");
    KisPaintLayerSP layer = new KisPaintLayer(image, "paint", OPACITY_OPAQUE_U8);
    image->addNode(layer);
    image->waitForDone();
    auto device = layer->paintDevice();
    device->fill(QRect(0, 0, 128, 64), KoColor(Qt::white, cs));
    KisPaintDeviceSP source(new KisPaintDevice(cs));
    source->fill(QRect(0, 0, 64, 64), KoColor(Qt::blue, cs));
    source->fill(QRect(64, 0, 64, 64), KoColor(Qt::green, cs));
    bool exists = false;
    const auto secondTile = source->dataManager()->getReadOnlyTileLazy(1, 0, exists);
    QVERIFY(exists);
    auto *secondData = secondTile->tileData();
    QVERIFY(secondData->ref());
    const auto release = qScopeGuard([&] { secondData->deref(); });
    KisResourcesSnapshotSP resources = new KisResourcesSnapshot(image, layer);
    if (indirect) {
        KisPaintOpPresetSP preset(new KisPaintOpPreset());
        preset->setSettings(new WashSettings());
        resources->setBrush(preset);
    }
    CompletionState state;
    const auto id = image->startStroke(new CompletionStrategy(resources, state, indirect));
    image->addJob(id, new CompletionStrategy::PaintData());
    QTRY_VERIFY_WITH_TIMEOUT(state.painted.load(), 5000);
    if (entry != 0) {
        QVERIFY(KisTileDataStore::instance()->trySwapTileData(secondData));
        QVERIFY(!secondData->isResident());
        KisTileDataStoreTestAccess::failNextSwapIn(KisSwapInFailurePoint(failurePoint));
    }
    image->addJob(id, new CompletionStrategy::FailureData(source, entry));
    image->addJob(id, new CompletionStrategy::PaintData());
    image->endStroke(id);
    image->waitForDone();
    QVERIFY(state.destroyed);
    QVERIFY(state.failureContract);
    QCOMPARE(state.paintJobs.load(), entry == 0 ? 2 : 1);
    QCOMPARE(state.finishes.load(), entry == 0 ? 1 : 0);
    QCOMPARE(state.cancellations.load(), entry == 0 ? 0 : 1);
    QVERIFY(!layer->hasTemporaryTarget());
    QVERIFY(!state.target->dataManager()->hasCurrentMemento());
    QCOMPARE(bool(store->presentCommand()), entry == 0);
    QColor actual;
    for (const auto &point : {QPoint(0, 0), QPoint(1, 0), QPoint(63, 0), QPoint(64, 0), QPoint(126, 63)}) {
        device->pixel(point.x(), point.y(), &actual);
        QCOMPARE(actual, QColor(entry == 0 && point.x() < 64 ? Qt::red : Qt::white));
    }
    if (entry == 0) {
        store->undo(); image->waitForDone();
        device->pixel(0, 0, &actual); QCOMPARE(actual, QColor(Qt::white));
    } else {
        QVERIFY(!secondData->isResident()); // no fallback replay consumed the injected source failure
    }
}

SIMPLE_TEST_MAIN(KisPainterStrokeCompletionTest)
#include "KisPainterStrokeCompletionTest.moc"

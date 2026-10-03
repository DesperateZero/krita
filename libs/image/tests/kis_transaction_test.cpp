/*
 *  SPDX-FileCopyrightText: 2007 Boudewijn Rempt <boud@valdyas.org>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <memory>
#include "kis_transaction_test.h"
#include <simpletest.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>

#include <kundo2qstack.h>

#include "kis_types.h"
#include "kis_transform_worker.h"
#include "kis_paint_device.h"
#include "kis_transaction.h"
#include "kis_surrogate_undo_adapter.h"
#include "kis_image.h"
#include "kis_paint_device_debug_utils.h"
#include "kistest.h"
#include "kis_iterator_ng.h"
#include "kis_datamanager.h"
#include "KisTransactionWrapperFactory.h"
#include "kis_undo_stores.h"
#include "kis_post_execution_undo_adapter.h"
#include "kis_painter.h"
#include <QRegularExpression>
#include <thread>
#include "pagestore/KisPageStore.h"
#include "pagestore/KisPageStoreDiagnostics_p.h"
#include "pagestore/KisPageStoreIteratorReadScope_p.h"
#include "kis_pixel_selection.h"
#include "kis_raster_keyframe_channel.h"
#include "kis_paint_device_frames_interface.h"
#include "testing_timed_default_bounds.h"
#include <KoColor.h>

namespace {
class StrokeMappingBounds final : public TestUtil::TestingTimedDefaultBounds
{
public:
    QRect bounds() const override { return rect; }
    bool wrapAroundMode() const override { return wrapped; }
    WrapAroundAxis wrapAroundModeAxis() const override { return axis; }
    QRect rect{0, 0, 257, 193};
    bool wrapped = true;
    WrapAroundAxis axis = WRAPAROUND_BOTH;
};
}

void KisTransactionTest::testStrokeMappingContext_data()
{
    QTest::addColumn<int>("change"); QTest::addColumn<bool>("cancel"); QTest::addColumn<bool>("cut");
    const char *names[] = {"offset-x", "offset-y", "support", "mode", "axis", "border-origin", "border-size", "equivalent-bounds", "inactive-wrap", "pre-admission-offset"};
    for (int change = 0; change < 10; ++change) for (bool cancel : {false, true}) for (bool cut : {false, true})
        QTest::newRow(qPrintable(QString("%1-cancel%2-cut%3").arg(names[change]).arg(cancel).arg(cut))) << change << cancel << cut;
}

void KisTransactionTest::testStrokeMappingContext()
{
    QFETCH(int, change); QFETCH(bool, cancel); QFETCH(bool, cut);
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);
    auto *bounds = new StrokeMappingBounds(); dev->setDefaultBounds(bounds);
    dev->moveTo(QPoint(13, 11)); dev->fill(QRect(13, 11, 1, 1), KoColor(Qt::white, cs));
    dev->setSupportsWraparoundMode(change != 8);
    const auto manager = dev->dataManager(); KisTransaction transaction(dev);
    if (change == 9) dev->moveTo(QPoint(19, 23)); // legal before explicit admission
    const QPoint admittedOffset(dev->x(), dev->y());
    QString error; QVERIFY2(transaction.beginStrokeMutation(&error), qPrintable(error));
    const auto context = dev->captureWriteContext(); QVERIFY(context.matches(dev.data()));
    QVERIFY(!KisPaintDevice::WriteContext().matches(dev.data()));
    int calls = 0;
    const auto write = [&](KisPixelWriteCursor *cursor) {
        ++calls; cursor->moveTo(0, 0); if (!cursor->rawData()) return false;
        cs->fromQColor(Qt::black, cursor->rawData()); return true;
    };
    QVERIFY(transaction.applyStrokePixelOperation(dev, {QRect(0, 0, 1, 1)}, write));
    std::shared_ptr<const KisPageStoreIteratorReadScope> frozen;
    if (cut) { QVERIFY(transaction.checkpointStrokeMutation()); frozen = manager->capturePageStoreReadScope(false); QVERIFY(frozen && frozen->isValid()); }
    switch (change) {
    case 0: dev->moveTo(admittedOffset + QPoint(1, 0)); break;
    case 1: dev->moveTo(admittedOffset + QPoint(0, -1)); break;
    case 2: dev->setSupportsWraparoundMode(false); break;
    case 3: bounds->wrapped = false; break;
    case 4: bounds->axis = WRAPAROUND_HORIZONTAL; break;
    case 5: bounds->rect.translate(1, -1); break;
    case 6: bounds->rect.setWidth(513); break;
    case 7: bounds = new StrokeMappingBounds(); dev->setDefaultBounds(bounds); break;
    case 8: bounds->wrapped = false; bounds->axis = WRAPAROUND_VERTICAL; bounds->rect.translate(7, 9); break;
    case 9: break;
    }
    KisSurrogateUndoAdapter undo;
    const bool changedMapping = change < 7;
    QCOMPARE(context.matches(dev.data()), !changedMapping);
    if (changedMapping) {
        QVERIFY(!transaction.applyStrokePixelOperation(dev, {QRect(0, 0, 1, 1)}, write, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(!transaction.applyStrokePixelOperation(dev, {}, write));
        QVERIFY(!dev->applyPixelOperation(QRect(admittedOffset, QSize(1, 1)), write, &transaction));
        QCOMPARE(calls, 1);
        QVERIFY(!transaction.beginStrokeMutation()); // cannot rebind via repeated opt-in
        QVERIFY(!transaction.checkpointStrokeMutation()); QVERIFY(!transaction.tryCommit(&undo));
    } else {
        QVERIFY(transaction.applyStrokePixelOperation(dev, {QRect(0, 0, 1, 1)}, write)); QCOMPARE(calls, 2);
    }
    if (cancel) {
        // Cancellation must still work with the changed context; it uses the
        // original manager and restores the transaction's original offset.
        QVERIFY2(transaction.tryRevert(&error), qPrintable(error));
        QCOMPARE(QPoint(dev->x(), dev->y()), QPoint(13, 11));
    } else {
        dev->moveTo(admittedOffset); dev->setSupportsWraparoundMode(change != 8);
        bounds->wrapped = true; bounds->axis = WRAPAROUND_BOTH; bounds->rect = QRect(0, 0, 257, 193);
        QVERIFY(transaction.applyStrokePixelOperation(dev, {QRect(0, 0, 1, 1)}, write));
        QVERIFY2(transaction.tryCommit(&undo, &error), qPrintable(error));
    }
    const auto check = [&](Qt::GlobalColor color) {
        KoColor pixel(cs); manager->readBytes(pixel.data(), 0, 0, 1, 1);
        return pixel == KoColor(color, cs);
    };
    QVERIFY(check(cancel ? Qt::white : Qt::black));
    if (!cancel) { undo.undo(); QVERIFY(check(Qt::white)); undo.redo(); QVERIFY(check(Qt::black)); }
    if (frozen) { auto page = frozen->readPage(0, 0, false); QVERIFY(page.data()); QCOMPARE(QByteArray(reinterpret_cast<const char *>(page.data()), 4), QByteArray(reinterpret_cast<const char *>(KoColor(Qt::black, cs).data()), 4)); }
}

void KisTransactionTest::testOrdinaryTransactionMove()
{
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8(); KisPaintDeviceSP dev = new KisPaintDevice(cs);
    dev->fill(QRect(0, 0, 1, 1), KoColor(Qt::white, cs));
    KisTransaction transaction(dev); dev->moveTo(QPoint(71, -39));
    KisSurrogateUndoAdapter undo; QVERIFY(transaction.tryCommit(&undo));
    QCOMPARE(QPoint(dev->x(), dev->y()), QPoint(71, -39));
    undo.undo(); QCOMPARE(QPoint(dev->x(), dev->y()), QPoint());
    undo.redo(); QCOMPARE(QPoint(dev->x(), dev->y()), QPoint(71, -39));
}

void KisTransactionTest::testStrokeMutationOwner_data()
{
    QTest::addColumn<bool>("abort"); QTest::addColumn<int>("repeats");
    for (bool abort : {false, true}) for (int repeats : {1, 10, 1000})
        QTest::newRow(qPrintable(QString("abort%1-D%2").arg(abort).arg(repeats))) << abort << repeats;
}

void KisTransactionTest::testStrokeMutationOwner()
{
    QFETCH(bool, abort); QFETCH(int, repeats);
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs), other = new KisPaintDevice(cs);
    dev->fill(QRect(0, 0, 1, 1), KoColor(Qt::white, cs));
    KisTransaction transaction(dev); QString error;
    QVERIFY2(transaction.beginStrokeMutation(&error), qPrintable(error));
    int calls = 0;
    QVERIFY(!transaction.applyStrokePixelOperation(other, {QRect(0, 0, 1, 1)}, [&](KisPixelWriteCursor *) { ++calls; return true; }));
    QCOMPARE(calls, 0);
    bool succeeded = true;
    std::thread worker([&] {
        for (int i = 0; i < repeats && succeeded; ++i)
            succeeded = transaction.applyStrokePixelOperation(dev, {QRect(0, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
                cursor->moveTo(0, 0); if (!cursor->rawData()) return false;
                cs->fromQColor(Qt::black, cursor->rawData()); return true;
            });
    });
    worker.join(); QVERIFY(succeeded);
    QVERIFY2(transaction.checkpointStrokeMutation(&error), qPrintable(error));
    const auto frozen = dev->dataManager()->capturePageStoreReadScope(false); QVERIFY(frozen && frozen->isValid());
    KoColor pixel(cs); dev->pixel(0, 0, &pixel); QCOMPARE(pixel, KoColor(Qt::black, cs));
    std::thread second([&] {
        succeeded = transaction.applyStrokePixelOperation(dev, {QRect(0, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(0, 0); if (!cursor->rawData()) return false;
            cs->fromQColor(Qt::red, cursor->rawData()); return true;
        });
    });
    second.join(); QVERIFY(succeeded);
    KisSurrogateUndoAdapter adapter;
    if (abort) QVERIFY2(transaction.tryRevert(&error), qPrintable(error));
    else QVERIFY2(transaction.tryCommit(&adapter, &error), qPrintable(error));
    dev->pixel(0, 0, &pixel); QCOMPARE(pixel, KoColor(abort ? Qt::white : Qt::red, cs));
    if (!abort) {
        adapter.undo(); dev->pixel(0, 0, &pixel); QCOMPARE(pixel, KoColor(Qt::white, cs));
        adapter.redo(); dev->pixel(0, 0, &pixel); QCOMPARE(pixel, KoColor(Qt::red, cs));
    }
    auto old = frozen->readPage(0, 0, false); QVERIFY(old.data());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(old.data()), cs->pixelSize()), QByteArray(reinterpret_cast<const char *>(KoColor(Qt::black, cs).data()), cs->pixelSize()));
    QVERIFY(!transaction.applyStrokePixelOperation(dev, {QRect(0, 0, 1, 1)}, [&](KisPixelWriteCursor *) { ++calls; return true; }));
    QCOMPARE(calls, 0);
}

void KisTransactionTest::testStrokeMutationDestruction()
{
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8(); KisPaintDeviceSP dev = new KisPaintDevice(cs);
    dev->fill(QRect(0, 0, 1, 1), KoColor(Qt::white, cs));
    {
        KisTransaction transaction(dev); QVERIFY(transaction.beginStrokeMutation());
        QVERIFY(transaction.applyStrokePixelOperation(dev, {QRect(0, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(0, 0); if (!cursor->rawData()) return false; cs->fromQColor(Qt::red, cursor->rawData()); return true;
        }));
        // Unfinished explicit stroke scope must abort, not purge/commit.
    }
    KoColor pixel(cs); dev->pixel(0, 0, &pixel); QCOMPARE(pixel, KoColor(Qt::white, cs));
    KisTransaction next(dev); QVERIFY(next.hasMemento()); QVERIFY(next.tryRevert());
}

void KisTransactionTest::testStrokeMutationFailure()
{
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8(); KisPaintDeviceSP dev = new KisPaintDevice(cs);
    dev->fill(QRect(0, 0, 1, 1), KoColor(Qt::white, cs));
    KisTransaction transaction(dev); QVERIFY(transaction.beginStrokeMutation());
    QVERIFY(!transaction.applyStrokePixelOperation(dev, {QRect(0, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
        cursor->moveTo(0, 0); if (!cursor->rawData()) return false; cs->fromQColor(Qt::red, cursor->rawData()); return false;
    }));
    KisSurrogateUndoAdapter adapter; QVERIFY(!transaction.tryCommit(&adapter)); QVERIFY(transaction.tryRevert());
    KoColor pixel(cs); dev->pixel(0, 0, &pixel); QCOMPARE(pixel, KoColor(Qt::white, cs));
}

void KisTransactionTest::testStrokeMutationContext_data()
{
    QTest::addColumn<int>("context");
    QTest::newRow("time") << 0;
    QTest::newRow("frame") << 1;
    QTest::newRow("lod") << 2;
}

void KisTransactionTest::testStrokeMutationContext()
{
    QFETCH(int, context);
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);
    auto *bounds = new TestUtil::TestingTimedDefaultBounds();
    dev->setDefaultBounds(bounds);
    if (context == 1) dev->createKeyframeChannel(KisKeyframeChannel::Raster)->addKeyframe(10);
    dev->fill(QRect(0, 0, 1, 1), KoColor(Qt::white, cs));
    KisTransaction transaction(dev);
    QString error;
    int calls = 0;
    const auto write = [&](KisPixelWriteCursor *cursor) {
        ++calls;
        cursor->moveTo(0, 0);
        if (!cursor->rawData()) return false;
        cs->fromQColor(Qt::red, cursor->rawData());
        return true;
    };
    QVERIFY(!transaction.checkpointStrokeMutation(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!transaction.applyStrokePixelOperation(dev, {QRect(0, 0, 1, 1)}, write));
    QCOMPARE(calls, 0);
    QVERIFY2(transaction.beginStrokeMutation(&error), qPrintable(error));
    QVERIFY(transaction.applyStrokePixelOperation(dev, {QRect(0, 0, 1, 1)}, write));
    QVERIFY(transaction.checkpointStrokeMutation());
    const auto frozen = dev->dataManager()->capturePageStoreReadScope(false); QVERIFY(frozen && frozen->isValid());
    if (context == 2) bounds->testingSetLod(1);
    else bounds->testingSetTime(10);
    QVERIFY(!transaction.applyStrokePixelOperation(dev, {QRect(0, 0, 1, 1)}, write));
    QVERIFY(!transaction.applyStrokePixelOperation(dev, {}, write));
    QCOMPARE(calls, 1);
    QVERIFY(!transaction.beginStrokeMutation());
    QVERIFY(!transaction.checkpointStrokeMutation());
    QVERIFY(frozen->isValid()); // Rejection preserves the existing immutable reader.
    KisSurrogateUndoAdapter adapter;
    QVERIFY(!transaction.tryCommit(&adapter));
    bounds->testingSetLod(0);
    bounds->testingSetTime(0);
    QVERIFY(transaction.applyStrokePixelOperation(dev, {QRect(0, 0, 1, 1)}, write));
    QCOMPARE(calls, 2);
    QVERIFY2(transaction.tryCommit(&adapter, &error), qPrintable(error));
    KoColor pixel(cs);
    dev->pixel(0, 0, &pixel); QCOMPARE(pixel, KoColor(Qt::red, cs));
    adapter.undo(); dev->pixel(0, 0, &pixel); QCOMPARE(pixel, KoColor(Qt::white, cs));
    adapter.redo(); dev->pixel(0, 0, &pixel); QCOMPARE(pixel, KoColor(Qt::red, cs));
}

void KisTransactionTest::testRejectedCommitIsRetryable_data()
{
    QTest::addColumn<int>("endpoint");
    QTest::newRow("undo-adapter") << 0;
    QTest::newRow("post-execution-adapter") << 1;
    QTest::newRow("take-command") << 2;
}

void KisTransactionTest::testRejectedCommitIsRetryable()
{
    QFETCH(int, endpoint);
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);
    dev->fill(QRect(0, 0, 128, 64), KoColor(Qt::white, cs));

    int endCommands = 0;
    int endRedos = 0;
    struct EndCommand : KUndo2Command {
        explicit EndCommand(int &redos) : redos(redos) {}
        void redo() override { ++redos; }
        int &redos;
    };
    struct Factory : KisTransactionWrapperFactory {
        Factory(int &commands, int &redos) : commands(commands), redos(redos) {}
        KUndo2Command *createBeginTransactionCommand(KisPaintDeviceSP) override { return nullptr; }
        KUndo2Command *createEndTransactionCommand() override {
            ++commands;
            return new EndCommand(redos);
        }
        int &commands;
        int &redos;
    };
    KisSurrogateUndoAdapter adapter;
    auto *postStore = new KisSurrogateUndoStore();
    KisImageSP image = new KisImage(postStore, 128, 64, cs, "commit retry");
    KisTransaction transaction(dev, nullptr, -1, new Factory(endCommands, endRedos));
    auto *command = static_cast<KisTransactionData *>(transaction.undoCommand());
    dev->fill(QRect(0, 0, 64, 64), KoColor(Qt::black, cs));

    // Page zero is sealed; page one still owns a real writer. Completion must
    // reject this state, not commit the first page and register a partial undo.
    auto writer = dev->createHLineIteratorNG(64, 0, 1);
    cs->fromQColor(Qt::red, writer->rawData());
    KUndo2Command *taken = nullptr;
    QString error;
    const auto finish = [&] {
        if (endpoint == 0) return transaction.tryCommit(&adapter, &error);
        if (endpoint == 1) return transaction.tryCommit(image->postExecutionUndoAdapter(), &error);
        return transaction.tryEndAndTake(taken, &error);
    };
    for (int attempt = 0; attempt < 2; ++attempt) {
        QVERIFY(!finish());
        QVERIFY(!error.isEmpty());
        QCOMPARE(transaction.undoCommand(), command);
        QVERIFY(!command->isTransactionFinished());
        QVERIFY(dev->dataManager()->hasCurrentMemento());
        QVERIFY(!adapter.presentCommand());
        QVERIFY(!postStore->presentCommand());
        QVERIFY(!taken);
        QCOMPARE(endCommands, 0);
        QCOMPARE(endRedos, 0);
    }

    writer.clear();
    QVERIFY2(finish(), qPrintable(error));
    QVERIFY(error.isEmpty());
    QVERIFY(!transaction.undoCommand());
    QVERIFY(command->isTransactionFinished());
    QVERIFY(!dev->dataManager()->hasCurrentMemento());
    QCOMPARE(endCommands, 1);
    QCOMPARE(endRedos, 1);
    // Repeating the data endpoint is idempotent and does not rerun wrappers.
    QVERIFY(command->tryEndTransaction(&error));
    QCOMPARE(endCommands, 1);
    QCOMPARE(endRedos, 1);

    std::unique_ptr<KUndo2Command> takenOwner(taken);
    if (taken) taken->redo();
    if (endpoint == 0) adapter.undo();
    else if (endpoint == 1) { postStore->undo(); image->waitForDone(); }
    else taken->undo();
    QColor actual;
    dev->pixel(0, 0, &actual); QCOMPARE(actual, QColor(Qt::white));
    dev->pixel(64, 0, &actual); QCOMPARE(actual, QColor(Qt::white));
    if (endpoint == 0) adapter.redo();
    else if (endpoint == 1) { postStore->redo(); image->waitForDone(); }
    else taken->redo();
    dev->pixel(0, 0, &actual); QCOMPARE(actual, QColor(Qt::black));
    dev->pixel(64, 0, &actual); QCOMPARE(actual, QColor(Qt::red));
}

void KisTransactionTest::testPainterRetainsRejectedTransaction()
{
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);
    dev->fill(QRect(0, 0, 128, 64), KoColor(Qt::white, cs));
    KisSurrogateUndoAdapter adapter;
    KisPainter painter(dev);
    painter.beginTransaction();
    auto *transaction = painter.takeTransaction();
    painter.putTransaction(transaction);
    dev->fill(QRect(0, 0, 64, 64), KoColor(Qt::black, cs));
    auto writer = dev->createHLineIteratorNG(64, 0, 1);
    cs->fromQColor(Qt::red, writer->rawData());

    QTest::ignoreMessage(QtWarningMsg, QRegularExpression("^Painter transaction completion failed:.*$"));
    painter.endTransaction(&adapter);
    QVERIFY(!adapter.presentCommand());
    auto *retained = painter.takeTransaction();
    QCOMPARE(retained, transaction);
    painter.putTransaction(retained);
    QVERIFY(dev->dataManager()->hasCurrentMemento());

    writer.clear();
    painter.endTransaction(&adapter);
    QVERIFY(adapter.presentCommand());
    QVERIFY(!dev->dataManager()->hasCurrentMemento());
    adapter.undo();
    QColor actual;
    dev->pixel(0, 0, &actual); QCOMPARE(actual, QColor(Qt::white));
    dev->pixel(64, 0, &actual); QCOMPARE(actual, QColor(Qt::white));
}

void KisTransactionTest::testUndo()
{
    KisSurrogateUndoAdapter undoAdapter;
    const KoColorSpace * cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);

    quint8* pixel = new quint8[cs->pixelSize()];
    cs->fromQColor(Qt::white, pixel);
    dev->fill(0, 0, 512, 512, pixel);

    cs->fromQColor(Qt::black, pixel);
    dev->fill(512, 0, 512, 512, pixel);

    QColor c1, c2;
    dev->pixel(5, 5, &c1);
    dev->pixel(517, 5, &c2);

    QVERIFY(c1 == Qt::white);
    QVERIFY(c2 == Qt::black);

    KisTransaction transaction(kundo2_noi18n("mirror"), dev, 0);
    KisTransformWorker::mirrorX(dev);
    transaction.commit(&undoAdapter);

    dev->pixel(5, 5, &c1);
    dev->pixel(517, 5, &c2);

    QVERIFY(c1 == Qt::black);
    QVERIFY(c2 == Qt::white);

    undoAdapter.undo();

    dev->pixel(5, 5, &c1);
    dev->pixel(517, 5, &c2);

    QVERIFY(c1 == Qt::white);
    QVERIFY(c2 == Qt::black);

}

void KisTransactionTest::testRedo()
{
    KisSurrogateUndoAdapter undoAdapter;

    const KoColorSpace * cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);

    quint8* pixel = new quint8[cs->pixelSize()];
    cs->fromQColor(Qt::white, pixel);
    dev->fill(0, 0, 512, 512, pixel);

    cs->fromQColor(Qt::black, pixel);
    dev->fill(512, 0, 512, 512, pixel);

    QColor c1, c2;
    dev->pixel(5, 5, &c1);
    dev->pixel(517, 5, &c2);

    QVERIFY(c1 == Qt::white);
    QVERIFY(c2 == Qt::black);

    KisTransaction transaction(kundo2_noi18n("mirror"), dev, 0);
    KisTransformWorker::mirrorX(dev);
    transaction.commit(&undoAdapter);

    dev->pixel(5, 5, &c1);
    dev->pixel(517, 5, &c2);

    QVERIFY(c1 == Qt::black);
    QVERIFY(c2 == Qt::white);


    undoAdapter.undo();

    dev->pixel(5, 5, &c1);
    dev->pixel(517, 5, &c2);

    QVERIFY(c1 == Qt::white);
    QVERIFY(c2 == Qt::black);

    undoAdapter.redo();

    dev->pixel(5, 5, &c1);
    dev->pixel(517, 5, &c2);

    QVERIFY(c1 == Qt::black);
    QVERIFY(c2 == Qt::white);
}

void KisTransactionTest::testDeviceMove()
{
    KisSurrogateUndoAdapter undoAdapter;

    const KoColorSpace * cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);

    QCOMPARE(dev->x(), 0);
    QCOMPARE(dev->y(), 0);

    KisTransaction t1(kundo2_noi18n("move1"), dev, 0);
    dev->moveTo(10,20);
    t1.commit(&undoAdapter);

    QCOMPARE(dev->x(), 10);
    QCOMPARE(dev->y(), 20);

    KisTransaction t2(kundo2_noi18n("move2"), dev, 0);
    dev->moveTo(7,11);
    t2.commit(&undoAdapter);

    QCOMPARE(dev->x(),  7);
    QCOMPARE(dev->y(), 11);

    undoAdapter.undo();

    QCOMPARE(dev->x(), 10);
    QCOMPARE(dev->y(), 20);

    undoAdapter.undo();

    QCOMPARE(dev->x(), 0);
    QCOMPARE(dev->y(), 0);

    undoAdapter.redo();

    QCOMPARE(dev->x(), 10);
    QCOMPARE(dev->y(), 20);

    undoAdapter.redo();

    QCOMPARE(dev->x(),  7);
    QCOMPARE(dev->y(), 11);
}

#include "kis_keyframe_channel.h"
void KisTransactionTest::testUndoWithUnswitchedFrames()
{
    KisSurrogateUndoAdapter undoAdapter;
    const QRect imageRect(0,0,100,100);


    const KoColorSpace * cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);

    TestUtil::TestingTimedDefaultBounds *bounds = new TestUtil::TestingTimedDefaultBounds();
    dev->setDefaultBounds(bounds);

    KisRasterKeyframeChannel *channel = dev->createKeyframeChannel(KisKeyframeChannel::Raster);
    QVERIFY(channel);

    KisPaintDeviceFramesInterface *i = dev->framesInterface();
    QVERIFY(i);

    QCOMPARE(i->frames().size(), 1);


    dev->fill(QRect(10,10,20,20), KoColor(Qt::white, cs));

    KIS_DUMP_DEVICE_2(dev, imageRect, "00_f0_w20", "dd");
    QCOMPARE(dev->exactBounds(), QRect(10,10,20,20));


    // add keyframe at position 10
    channel->addKeyframe(10);

    // add keyframe at position 11
    channel->addKeyframe(11);

    // add keyframe at position 12
    channel->addKeyframe(12);

    KIS_DUMP_DEVICE_2(dev, imageRect, "01_f0_b20", "dd");
    QCOMPARE(dev->exactBounds(), QRect(10,10,20,20));

    {
        KisTransaction transaction(kundo2_noi18n("first_stroke"), dev, 0);

        dev->clear();
        dev->fill(QRect(40,40,21,21), KoColor(Qt::red, cs));

        transaction.commit(&undoAdapter);

        KIS_DUMP_DEVICE_2(dev, imageRect, "02_f0_b21_stroke", "dd");
        QCOMPARE(dev->exactBounds(), QRect(40,40,21,21));
    }

    // switch to frame 10
    bounds->testingSetTime(10);

    KIS_DUMP_DEVICE_2(dev, imageRect, "03_f10_b0_switched", "dd");
    QVERIFY(dev->exactBounds().isEmpty());

    {
        KisTransaction transaction(kundo2_noi18n("second_stroke"), dev, 0);

        dev->fill(QRect(60,60,22,22), KoColor(Qt::green, cs));

        transaction.commit(&undoAdapter);

        KIS_DUMP_DEVICE_2(dev, imageRect, "04_f10_b22_stroke", "dd");
        QCOMPARE(dev->exactBounds(), QRect(60,60,22,22));
    }

    undoAdapter.undo();

    KIS_DUMP_DEVICE_2(dev, imageRect, "05_f10_b0_undone", "dd");
    QVERIFY(dev->exactBounds().isEmpty());

    bounds->testingSetTime(0);
    KIS_DUMP_DEVICE_2(dev, imageRect, "06_f0_b21_undone", "dd");
    QCOMPARE(dev->exactBounds(), QRect(40,40,21,21));

    bounds->testingSetTime(10);
    QVERIFY(dev->exactBounds().isEmpty());

    undoAdapter.undo();

    KIS_DUMP_DEVICE_2(dev, imageRect, "07_f10_b0_undone_x2", "dd");
    QVERIFY(dev->exactBounds().isEmpty());

    bounds->testingSetTime(0);
    KIS_DUMP_DEVICE_2(dev, imageRect, "08_f0_b20_undone_x2", "dd");
    QCOMPARE(dev->exactBounds(), QRect(10,10,20,20));

    {
        KisTransaction transaction(kundo2_noi18n("third_move"), dev, 0);

        dev->moveTo(17,17);

        transaction.commit(&undoAdapter);

        KIS_DUMP_DEVICE_2(dev, imageRect, "09_f0_o27_move", "dd");
        QCOMPARE(dev->exactBounds(), QRect(27,27,20,20));
    }

    bounds->testingSetTime(10);
    QVERIFY(dev->exactBounds().isEmpty());

    undoAdapter.undo();

    KIS_DUMP_DEVICE_2(dev, imageRect, "10_f10_b0_undone_x3", "dd");
    QVERIFY(dev->exactBounds().isEmpty());

    bounds->testingSetTime(0);
    KIS_DUMP_DEVICE_2(dev, imageRect, "11_f0_b20_undone_x3", "dd");
    QCOMPARE(dev->exactBounds(), QRect(10,10,20,20));
}

#include "KisTransactionWrapperFactory.h"

void KisTransactionTest::testTransactionWrapperFactory()
{
    KisSurrogateUndoAdapter undoAdapter;

    const KoColorSpace * cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);

    enum CommandState {
        Inexistent,
        Created,
        Redone,
        Undone,
        Destroyed
    };
    Q_ENUMS(CommandState)

    struct StateTrackingUndoCommand : public KUndo2Command
    {
        StateTrackingUndoCommand(CommandState &state)
            : m_state(state)
        {
            KIS_ASSERT(m_state == Inexistent);
            m_state = Created;
        }

        ~StateTrackingUndoCommand() override {
            m_state = Destroyed;
        }

        void redo() override {
            m_state = Redone;
        }

        void undo() override {
            m_state = Undone;
        }

        CommandState &m_state;
    };

    struct Factory : public KisTransactionWrapperFactory {
        Factory(CommandState &beginState, CommandState &endState)
            : m_beginState(beginState), m_endState(endState)
        {
        }

        KUndo2Command* createBeginTransactionCommand(KisPaintDeviceSP device) override {
            Q_UNUSED(device);
            return new StateTrackingUndoCommand(m_beginState);
        }

        KUndo2Command* createEndTransactionCommand() override {
            return new StateTrackingUndoCommand(m_endState);
        }

        CommandState &m_beginState;
        CommandState &m_endState;
    };

    CommandState beginState = Inexistent;
    CommandState endState = Inexistent;

    KisTransaction transaction(dev, 0, -1, new Factory(beginState, endState));

    QCOMPARE(beginState, Redone);
    QCOMPARE(endState, Inexistent);

    transaction.commit(&undoAdapter);

    QCOMPARE(beginState, Redone);
    QCOMPARE(endState, Redone);

    undoAdapter.undo();

    QCOMPARE(beginState, Undone);
    QCOMPARE(endState, Undone);

    undoAdapter.redo();

    QCOMPARE(beginState, Redone);
    QCOMPARE(endState, Redone);
}

#include "KisInterstrokeDataTransactionWrapperFactory.h"
#include "KisInterstrokeDataFactory.h"
#include "KisInterstrokeData.h"

struct TestInterstrokeData : public KisInterstrokeData
{
    TestInterstrokeData(KisPaintDeviceSP device,
                        int _typeId, int _value)
        : KisInterstrokeData(device),
          typeId(_typeId), value(_value)
    {
    }

    void beginTransaction() override {
        ENTER_FUNCTION() << ppVar(value);
        m_savedValue = value;
    }

    KUndo2Command* endTransaction() override {

        ENTER_FUNCTION() << ppVar(value) << ppVar(m_savedValue);
        struct SimpleIntCommand : public KUndo2Command
        {
            SimpleIntCommand(int *pointer, int newValue, int oldValue)
                : m_pointer(pointer),
                  m_newValue(newValue),
                  m_oldValue(oldValue)
            {
            }

            void redo() override {
                if (m_firstRedo) {
                    m_firstRedo = false;
                    return;
                }

                *m_pointer = m_newValue;
            }

            void undo() override {
                *m_pointer = m_oldValue;
            }

        private:
            bool m_firstRedo {false};
            int *m_pointer {0};
            int m_newValue {0};
            int m_oldValue {0};
        };

        return new SimpleIntCommand(&value, value, m_savedValue);
    }

    int typeId {0};
    int value {0};

    int m_savedValue {0};
};

struct TestInterstrokeDataFactory : public KisInterstrokeDataFactory
{
    TestInterstrokeDataFactory(int typeId)
        : m_typeId(typeId)
    {
    }

    bool isCompatible(KisInterstrokeData *_data) override {
        TestInterstrokeData *data = dynamic_cast<TestInterstrokeData*>(_data);
        return data && data->typeId == m_typeId;
    }

    KisInterstrokeData * create(KisPaintDeviceSP device) override {
        Q_UNUSED(device);
        return new TestInterstrokeData(device, m_typeId, 0);
    }

    int m_typeId {0};
};

TestInterstrokeData* resolveType(KisInterstrokeDataSP data) {
    TestInterstrokeData *result = dynamic_cast<TestInterstrokeData*>(data.data());
    KIS_ASSERT(result);
    return result;
}

void KisTransactionTest::testInterstrokeData()
{
    KisSurrogateUndoAdapter undoAdapter;

    const KoColorSpace * cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);

    KisTransaction transaction1(dev, 0, -1,
                                new KisInterstrokeDataTransactionWrapperFactory(
                                    new TestInterstrokeDataFactory(13)));

    QVERIFY(dev->interstrokeData());
    QCOMPARE(resolveType(dev->interstrokeData())->typeId, 13);
    QCOMPARE(resolveType(dev->interstrokeData())->value, 0);

    resolveType(dev->interstrokeData())->value = 10;

    transaction1.commit(&undoAdapter);

    QVERIFY(dev->interstrokeData());
    QCOMPARE(resolveType(dev->interstrokeData())->typeId, 13);
    QCOMPARE(resolveType(dev->interstrokeData())->value, 10);

    KisInterstrokeDataSP firstData = dev->interstrokeData();

    KisTransaction transaction2(dev, 0, -1,
                                new KisInterstrokeDataTransactionWrapperFactory(
                                    new TestInterstrokeDataFactory(13)));

    QVERIFY(dev->interstrokeData());
    QCOMPARE(dev->interstrokeData(), firstData);
    QCOMPARE(resolveType(dev->interstrokeData())->typeId, 13);
    QCOMPARE(resolveType(dev->interstrokeData())->value, 10);

    resolveType(dev->interstrokeData())->value = 20;

    transaction2.commit(&undoAdapter);

    QVERIFY(dev->interstrokeData());
    QCOMPARE(dev->interstrokeData(), firstData);
    QCOMPARE(resolveType(dev->interstrokeData())->typeId, 13);
    QCOMPARE(resolveType(dev->interstrokeData())->value, 20);

    KisTransaction transaction3(dev, 0, -1,
                                new KisInterstrokeDataTransactionWrapperFactory(
                                    new TestInterstrokeDataFactory(17)));

    QVERIFY(dev->interstrokeData());
    QVERIFY(dev->interstrokeData() != firstData);
    QCOMPARE(resolveType(dev->interstrokeData())->typeId, 17);
    QCOMPARE(resolveType(dev->interstrokeData())->value, 0);

    resolveType(dev->interstrokeData())->value = 30;

    transaction3.commit(&undoAdapter);

    QVERIFY(dev->interstrokeData());
    QVERIFY(dev->interstrokeData() != firstData);
    QCOMPARE(resolveType(dev->interstrokeData())->typeId, 17);
    QCOMPARE(resolveType(dev->interstrokeData())->value, 30);


    KisTransaction transaction4(dev);
    QVERIFY(!dev->interstrokeData());
    transaction4.commit(&undoAdapter);
    QVERIFY(!dev->interstrokeData());

    undoAdapter.undo();

    QVERIFY(dev->interstrokeData());
    QVERIFY(dev->interstrokeData() != firstData);
    QCOMPARE(resolveType(dev->interstrokeData())->typeId, 17);
    QCOMPARE(resolveType(dev->interstrokeData())->value, 30);

    undoAdapter.undo();

    QVERIFY(dev->interstrokeData());
    QCOMPARE(dev->interstrokeData(), firstData);
    QCOMPARE(resolveType(dev->interstrokeData())->typeId, 13);
    QCOMPARE(resolveType(dev->interstrokeData())->value, 20);

    undoAdapter.undo();

    QVERIFY(dev->interstrokeData());
    QCOMPARE(dev->interstrokeData(), firstData);
    QCOMPARE(resolveType(dev->interstrokeData())->typeId, 13);
    QCOMPARE(resolveType(dev->interstrokeData())->value, 10);

    undoAdapter.undo();

    QVERIFY(!dev->interstrokeData());
}

void KisTransactionTest::testInterstrokeDataWithUnswitchedFrames()
{
    KisSurrogateUndoAdapter undoAdapter;
    const QRect imageRect(0,0,100,100);


    const KoColorSpace * cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);

    TestUtil::TestingTimedDefaultBounds *bounds = new TestUtil::TestingTimedDefaultBounds();
    dev->setDefaultBounds(bounds);

    KisRasterKeyframeChannel *channel = dev->createKeyframeChannel(KisKeyframeChannel::Raster);
    QVERIFY(channel);

    KisPaintDeviceFramesInterface *i = dev->framesInterface();
    QVERIFY(i);

    QCOMPARE(i->frames().size(), 1);


    dev->fill(QRect(10,10,20,20), KoColor(Qt::white, cs));

    KIS_DUMP_DEVICE_2(dev, imageRect, "00_f0_w20", "dd");
    QCOMPARE(dev->exactBounds(), QRect(10,10,20,20));


    // add keyframe at position 10
    channel->addKeyframe(10);

    // add keyframe at position 11
    channel->addKeyframe(11);

    // add keyframe at position 12
    channel->addKeyframe(12);

    QVERIFY(!dev->interstrokeData());

    {
        KisTransaction transaction(dev, 0, -1,
                new KisInterstrokeDataTransactionWrapperFactory(
                    new TestInterstrokeDataFactory(17)));

        QVERIFY(dev->interstrokeData());
        QCOMPARE(resolveType(dev->interstrokeData())->typeId, 17);
        QCOMPARE(resolveType(dev->interstrokeData())->value, 0);

        resolveType(dev->interstrokeData())->value = 30;
        transaction.commit(&undoAdapter);
    }

    // switch to frame 10
    bounds->testingSetTime(10);
    QVERIFY(!dev->interstrokeData());

    {
        KisTransaction transaction(dev, 0, -1,
                new KisInterstrokeDataTransactionWrapperFactory(
                    new TestInterstrokeDataFactory(18)));

        QVERIFY(dev->interstrokeData());
        QCOMPARE(resolveType(dev->interstrokeData())->typeId, 18);
        QCOMPARE(resolveType(dev->interstrokeData())->value, 0);

        resolveType(dev->interstrokeData())->value = 40;
        transaction.commit(&undoAdapter);
    }

    QVERIFY(dev->interstrokeData());

    undoAdapter.undo();

    QVERIFY(!dev->interstrokeData());

    // switch to frame 0
    bounds->testingSetTime(0);

    QVERIFY(dev->interstrokeData());
    QCOMPARE(resolveType(dev->interstrokeData())->typeId, 17);
    QCOMPARE(resolveType(dev->interstrokeData())->value, 30);

    // switch back to frame 10
    bounds->testingSetTime(10);

    QVERIFY(!dev->interstrokeData());

    undoAdapter.undo();

    QVERIFY(!dev->interstrokeData());

    // switch to frame 0
    bounds->testingSetTime(0);

    QVERIFY(!dev->interstrokeData());
}

void KisTransactionTest::testDirectAbort_data()
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<bool>("suppressUpdates");
    for (int mode : {0, 1, 2, 3}) for (bool suppressed : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("mode%1-suppressed%2").arg(mode).arg(suppressed)))
            << mode << suppressed;
}

void KisTransactionTest::testDirectAbort()
{
    QFETCH(int, mode);
    QFETCH(bool, suppressUpdates);
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);
    dev->fill(QRect(0, 0, 64, 64), KoColor(Qt::red, cs));
    dev->fill(QRect(128, 0, 64, 64), KoColor(Qt::blue, cs));
    const QRect originalBounds = dev->exactBounds();
    const auto originalRegion = dev->region();
    const KoColor originalDefault = dev->defaultPixel();
    KisTransaction transaction(dev, nullptr, -1, nullptr,
        suppressUpdates ? KisTransaction::SuppressUpdates : KisTransaction::None);
    auto *command = static_cast<KisTransactionData *>(transaction.undoCommand());

    if (mode != 0) {
        dev->moveTo(7, 11);
        dev->clear();
        if (mode != 1) dev->fill(QRect(71, 11, 64, 64), KoColor(Qt::green, cs));
        if (mode == 3) dev->setDefaultPixel(KoColor(Qt::yellow, cs));
    }
    // Force the caches to contain the in-transaction state, even when updates
    // are suppressed. A captured reader must keep that exact state after abort.
    (void) dev->exactBounds();
    (void) dev->region();
    const int readX = mode == 0 ? 0 : 71;
    const int readY = mode == 0 ? 0 : 11;
    auto reader = dev->createHLineConstIteratorNG(readX, readY, 1);
    const QByteArray frozen(reinterpret_cast<const char *>(reader->rawDataConst()), cs->pixelSize());
    KisPageStoreDiagnosticRecorder recorder(true);
    QString error;
    QVERIFY2(command->tryAbortTransaction(&error), qPrintable(error));
    QVERIFY(command->tryAbortTransaction(&error));
    QVERIFY(!command->tryEndTransaction(&error));
    QVERIFY(!dev->dataManager()->hasCurrentMemento());
    QCOMPARE(QPoint(dev->x(), dev->y()), QPoint());
    QCOMPARE(dev->defaultPixel(), originalDefault);
    QCOMPARE(dev->exactBounds(), originalBounds);
    QCOMPARE(dev->region(), originalRegion);
    QColor actual;
    dev->pixel(0, 0, &actual); QCOMPARE(actual, QColor(Qt::red));
    dev->pixel(128, 0, &actual); QCOMPARE(actual, QColor(Qt::blue));
    dev->pixel(64, 0, &actual); QCOMPARE(actual.alpha(), 0);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(reader->rawDataConst()), cs->pixelSize()), frozen);
    QVERIFY(transaction.tryRevert(&error));
    QVERIFY(!transaction.undoCommand());
    QCOMPARE(recorder.metrics()[size_t(KisPageStoreDiagnosticPhase::CommitRootPublication)].intervals, quint64(0));
}

void KisTransactionTest::testAbortRejectedBegin()
{
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);
    dev->fill(QRect(0, 0, 64, 64), KoColor(Qt::white, cs));
    KisTransaction owner(dev);
    QVERIFY(owner.hasMemento());
    dev->fill(QRect(0, 0, 64, 64), KoColor(Qt::red, cs));
    struct RejectedFactory : KisTransactionWrapperFactory {
        int &calls;
        explicit RejectedFactory(int &calls) : calls(calls) {}
        KUndo2Command *createBeginTransactionCommand(KisPaintDeviceSP) override {
            ++calls;
            return nullptr;
        }
        KUndo2Command *createEndTransactionCommand() override {
            ++calls;
            return nullptr;
        }
    };
    int auxiliaryCalls = 0;
    KisTransaction rejected(dev, nullptr, -1, new RejectedFactory(auxiliaryCalls));
    QVERIFY(!rejected.hasMemento());
    QString error;
    QVERIFY(!rejected.tryEnd(&error));
    QVERIFY2(rejected.tryRevert(&error), qPrintable(error));
    QCOMPARE(auxiliaryCalls, 0);
    QVERIFY(dev->dataManager()->hasCurrentMemento());
    QColor actual;
    dev->pixel(0, 0, &actual); QCOMPARE(actual, QColor(Qt::red));
    QVERIFY2(owner.tryRevert(&error), qPrintable(error));
    dev->pixel(0, 0, &actual); QCOMPARE(actual, QColor(Qt::white));
}

void KisTransactionTest::testAbortWithLiveWriter()
{
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);
    dev->fill(QRect(0, 0, 128, 64), KoColor(Qt::white, cs));
    KisTransaction transaction(dev);
    auto *command = static_cast<KisTransactionData *>(transaction.undoCommand());
    dev->fill(QRect(0, 0, 64, 64), KoColor(Qt::black, cs));
    auto writer = dev->createHLineIteratorNG(64, 0, 1);
    cs->fromQColor(Qt::red, writer->rawData());
    QString error;
    QVERIFY(!transaction.tryRevert(&error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(transaction.undoCommand(), command);
    QVERIFY(dev->dataManager()->hasCurrentMemento());
    QVERIFY(!command->tryEndTransaction(&error));
    writer.clear();
    QVERIFY2(transaction.tryRevert(&error), qPrintable(error));
    QVERIFY(!dev->dataManager()->hasCurrentMemento());
    QColor actual;
    dev->pixel(0, 0, &actual); QCOMPARE(actual, QColor(Qt::white));
    dev->pixel(64, 0, &actual); QCOMPARE(actual, QColor(Qt::white));
    // Cancellation freed the old history admission; the next stroke is usable.
    KisSurrogateUndoAdapter adapter;
    KisTransaction next(dev);
    dev->fill(QRect(0, 0, 1, 1), KoColor(Qt::green, cs));
    QVERIFY(next.tryCommit(&adapter, &error));
    adapter.undo();
    dev->pixel(0, 0, &actual); QCOMPARE(actual, QColor(Qt::white));
}

void KisTransactionTest::testAbortInterstrokeData_data()
{
    QTest::addColumn<int>("nextType");
    QTest::newRow("continued") << 17;
    QTest::newRow("replacement") << 18;
    QTest::newRow("cleared") << -1;
}

void KisTransactionTest::testAbortInterstrokeData()
{
    QFETCH(int, nextType);
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);
    KisSurrogateUndoAdapter adapter;
    {
        KisTransaction initial(dev, nullptr, -1,
            new KisInterstrokeDataTransactionWrapperFactory(new TestInterstrokeDataFactory(17)));
        resolveType(dev->interstrokeData())->value = 10;
        initial.commit(&adapter);
    }
    const auto original = dev->interstrokeData();
    KisTransaction transaction(dev, nullptr, -1, nextType < 0 ? nullptr :
        new KisInterstrokeDataTransactionWrapperFactory(new TestInterstrokeDataFactory(nextType)));
    if (dev->interstrokeData()) resolveType(dev->interstrokeData())->value = 37;
    dev->fill(QRect(0, 0, 64, 64), KoColor(Qt::blue, cs));
    QString error;
    QVERIFY2(transaction.tryRevert(&error), qPrintable(error));
    QCOMPARE(dev->interstrokeData(), original);
    QCOMPARE(resolveType(dev->interstrokeData())->value, 10);
    QVERIFY(dev->exactBounds().isEmpty());
}

void KisTransactionTest::testAbortWithUnswitchedFrame()
{
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);
    auto *bounds = new TestUtil::TestingTimedDefaultBounds();
    dev->setDefaultBounds(bounds);
    auto *channel = dev->createKeyframeChannel(KisKeyframeChannel::Raster);
    channel->addKeyframe(10);
    dev->fill(QRect(0, 0, 64, 64), KoColor(Qt::red, cs));
    KisTransaction transaction(dev);
    dev->fill(QRect(0, 0, 64, 64), KoColor(Qt::blue, cs));
    dev->moveTo(7, 11);
    bounds->testingSetTime(10);
    dev->fill(QRect(0, 0, 64, 64), KoColor(Qt::green, cs));
    QString error;
    QVERIFY2(transaction.tryRevert(&error), qPrintable(error));
    bounds->testingSetTime(10);
    QColor actual;
    dev->pixel(0, 0, &actual); QCOMPARE(actual, QColor(Qt::green));
    bounds->testingSetTime(0);
    QCOMPARE(QPoint(dev->x(), dev->y()), QPoint());
    dev->pixel(0, 0, &actual); QCOMPARE(actual, QColor(Qt::red));
}

void KisTransactionTest::testAbortSelectionCache()
{
    KisPixelSelectionSP selection = new KisPixelSelection();
    selection->select(QRect(0, 0, 64, 64));
    selection->recalculateOutlineCache();
    const auto outline = selection->outlineCache();
    KisSelectionTransaction transaction(selection);
    selection->clear();
    selection->select(QRect(128, 0, 64, 64));
    selection->recalculateOutlineCache();
    QString error;
    QVERIFY2(transaction.tryRevert(&error), qPrintable(error));
    QVERIFY(selection->outlineCacheValid());
    QCOMPARE(selection->outlineCache(), outline);
    QCOMPARE(selection->selectedExactRect(), QRect(0, 0, 64, 64));
}

SIMPLE_TEST_MAIN(KisTransactionTest)

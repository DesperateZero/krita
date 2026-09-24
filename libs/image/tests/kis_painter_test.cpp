/*
 *  SPDX-FileCopyrightText: 2007 Sven Langkamp <sven.langkamp@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_painter_test.h"
#include <simpletest.h>

#include <kis_debug.h>
#include <QRect>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QRegularExpression>
#include <cmath>
#include <limits>
#include <QtXml>

#include <KoChannelInfo.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>

#include "kis_datamanager.h"
#include "kis_types.h"
#include "kis_paint_device.h"
#include "kis_painter.h"
#include "kis_pixel_selection.h"
#include "kis_fill_painter.h"
#include <kis_fixed_paint_device.h>
#include <testutil.h>
#include <kis_iterator_ng.h>
#include "kis_random_accessor_ng.h"
#include <testimage.h>
#include "pagestore/KisPageStoreDiagnostics_p.h"
#include "kis_default_bounds_base.h"

namespace {
class PainterWrapBounds final : public KisDefaultBoundsBase
{
public:
    QRect bounds() const override { return QRect(0, 0, 128, 96); }
    bool wrapAroundMode() const override { return true; }
    WrapAroundAxis wrapAroundModeAxis() const override { return WRAPAROUND_BOTH; }
    int currentLevelOfDetail() const override { return 0; }
    int currentTime() const override { return 0; }
    bool externalFrameActive() const override { return false; }
    void *sourceCookie() const override { return nullptr; }
};
}

void KisPainterTest::testPageStoreBitBltReadBoundary_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("scenario");
    QTest::addColumn<bool>("oldSource"); QTest::addColumn<bool>("masked");
    for (int bpp : {1, 4, 8, 16}) for (int scenario = 0; scenario < 4; ++scenario)
        for (bool oldSource : {false, true}) for (bool masked : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-scene%2-old%3-mask%4").arg(bpp).arg(scenario).arg(oldSource).arg(masked)))
                << bpp << scenario << oldSource << masked;
}

void KisPainterTest::testPageStoreBitBltReadBoundary()
{
    QFETCH(int, bpp); QFETCH(int, scenario); QFETCH(bool, oldSource); QFETCH(bool, masked);
    const auto registry = KoColorSpaceRegistry::instance();
    const KoColorSpace *cs = bpp == 1 ? registry->alpha8() : registry->colorSpace(
        "RGBA", bpp == 4 ? "U8" : bpp == 8 ? "U16" : "F32", "");
    QVERIFY(cs); QCOMPARE(cs->pixelSize(), quint32(bpp));
    const QRect sourceStorage(-160, -96, 576, 256), sourceRect(-101, -23, 275, 71);
    const QPoint destination(41, 53); const QRect destinationRect(destination, sourceRect.size());
    const bool history = scenario != 0, implicit = scenario == 2, wrapped = scenario == 3;
    KisPaintDeviceSP src = new KisPaintDevice(cs), dst = new KisPaintDevice(cs);
    // Offset and wrap mappings stay in the public accessor abstraction.
    src->moveTo(QPoint(-17, 9)); dst->moveTo(QPoint(13, -11));
    if (wrapped) { src->setDefaultBounds(new PainterWrapBounds); src->setSupportsWraparoundMode(true); }
    KoColor before(Qt::red, cs), current(Qt::green, cs), later(Qt::yellow, cs), background(Qt::blue, cs);
    before.setOpacity(quint8(73)); current.setOpacity(quint8(191)); later.setOpacity(quint8(233));
    if (implicit) src->setDefaultPixel(before);
    else src->fill(sourceStorage, before);
    KisPainter historyOwner(src);
    if (history) historyOwner.beginTransaction();
    const auto endHistory = qScopeGuard([&] { if (history) historyOwner.deleteTransaction(); });
    if (history) {
        if (implicit) src->setDefaultPixel(current);
        else src->fill(sourceStorage, current);
    }
    dst->fill(destinationRect.adjusted(-1, -1, 1, 1), background);
    KisSelectionSP selection;
    KisMementoSP maskHistory;
    if (masked) {
        selection = new KisSelection();
        selection->pixelSelection()->select(destinationRect, 67);
        maskHistory = selection->pixelSelection()->dataManager()->getMemento();
        selection->pixelSelection()->select(destinationRect, 201);
    }
    const auto endMaskHistory = qScopeGuard([&] {
        if (masked) selection->pixelSelection()->dataManager()->commit();
    });
    const KoColor &selectedSource = oldSource && history ? before : history ? current : before;
    const int pixels = sourceRect.width() * sourceRect.height();
    QByteArray expected(pixels * bpp, Qt::Uninitialized), sourceBytes(expected.size(), Qt::Uninitialized);
    QByteArray maskBytes(pixels, char(201));
    for (int p = 0; p < pixels; ++p) {
        memcpy(expected.data() + p * bpp, background.data(), size_t(bpp));
        memcpy(sourceBytes.data() + p * bpp, selectedSource.data(), size_t(bpp));
    }
    KoCompositeOp::ParameterInfo params;
    params.dstRowStart = reinterpret_cast<quint8 *>(expected.data()); params.dstRowStride = sourceRect.width() * bpp;
    params.srcRowStart = reinterpret_cast<const quint8 *>(sourceBytes.constData()); params.srcRowStride = params.dstRowStride;
    params.maskRowStart = masked ? reinterpret_cast<const quint8 *>(maskBytes.constData()) : nullptr;
    params.maskRowStride = masked ? sourceRect.width() : 0;
    params.rows = sourceRect.height(); params.cols = sourceRect.width(); params.opacity = params.flow = 1.0f;
    cs->bitBlt(cs, params, cs->compositeOp(COMPOSITE_COPY),
        KoColorConversionTransformation::internalRenderingIntent(), KoColorConversionTransformation::internalConversionFlags());

    bool injected = false;
    KisPageStoreDiagnosticRecorder recorder(true); // product spans multiple stores
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        // The source accessor has already captured its fixed view when the
        // destination enters its single managed write body.
        if (phase != KisPageStoreDiagnosticPhase::PixelOperationBody || injected) return;
        injected = true; // guard the source mutation's own diagnostics
        if (implicit) src->setDefaultPixel(later);
        else src->fill(sourceStorage, later);
    });
    KisPainter painter(dst, selection); painter.setCompositeOpId(COMPOSITE_COPY);
    if (oldSource) painter.bitBltOldData(destination, src, sourceRect);
    else painter.bitBlt(destination, src, sourceRect);
    painter.end();
    QVERIFY(injected);
    QByteArray actual(expected.size(), Qt::Uninitialized);
    dst->readBytes(reinterpret_cast<quint8 *>(actual.data()), destinationRect);
    if (bpp == 16) {
        // The full-buffer oracle and tile-sized product calls may take different
        // vector/tail paths. Compare finite F32 values within one float epsilon;
        // integer formats still require byte identity. Source/mask epoch changes
        // in this fixture are many orders of magnitude larger than this bound.
        for (qsizetype i = 0; i < actual.size(); i += sizeof(float)) {
            float a = 0, e = 0;
            memcpy(&a, actual.constData() + i, sizeof(float)); memcpy(&e, expected.constData() + i, sizeof(float));
            QVERIFY(std::isfinite(a) && std::isfinite(e));
            const double tolerance = std::numeric_limits<float>::epsilon() * qMax(1.0, std::abs(double(e)));
            QVERIFY2(std::abs(double(a) - e) <= tolerance,
                qPrintable(QStringLiteral("F32 component %1: actual=%2 expected=%3 tolerance=%4")
                    .arg(i / sizeof(float)).arg(double(a), 0, 'g', 10).arg(double(e), 0, 'g', 10).arg(tolerance)));
        }
    } else {
        QCOMPARE(actual, expected);
    }
    // The source mutation really happened, but the traversal never chased the
    // new head for later tiles. Keep this separate from writer atomicity.
    auto fresh = src->createRandomConstAccessorNG(); fresh->moveTo(sourceRect.x(), sourceRect.y());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(fresh->rawDataConst()), bpp),
             QByteArray(reinterpret_cast<const char *>(later.data()), bpp));
    if (masked) {
        selection->pixelSelection()->select(destinationRect, 13);
        auto freshMask = selection->projection()->createRandomConstAccessorNG(); freshMask->moveTo(destination.x(), destination.y());
        QCOMPARE(*freshMask->rawDataConst(), quint8(13));
    }
}

void KisPainterTest::testPageStoreBitBltLiveSource_data()
{
    QTest::addColumn<bool>("history"); QTest::addColumn<bool>("oldSource");
    for (bool history : {false, true}) for (bool oldSource : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("history%1-old%2").arg(history).arg(oldSource))) << history << oldSource;
}

void KisPainterTest::testPageStoreBitBltLiveSource()
{
    QFETCH(bool, history); QFETCH(bool, oldSource);
    const auto cs = KoColorSpaceRegistry::instance()->alpha8();
    KisPaintDeviceSP src = new KisPaintDevice(cs), dst = new KisPaintDevice(cs);
    const quint8 value = 0x31; src->fill(QRect(0, 0, 64, 64), KoColor(&value, cs));
    KisPainter owner(src); if (history) owner.beginTransaction();
    const auto cleanup = qScopeGuard([&] { if (history) owner.deleteTransaction(); });
    auto writer = src->createRandomAccessorNG(); writer->moveTo(0, 0);
    auto *pointer = writer->rawData(); QVERIFY(pointer); *pointer = 0x55;
    KisPainter painter(dst); painter.setCompositeOpId(COMPOSITE_COPY);
    if (oldSource) painter.bitBltOldData(QPoint(9, 7), src, QRect(0, 0, 8, 1));
    else painter.bitBlt(QPoint(9, 7), src, QRect(0, 0, 8, 1));
    painter.end();
    quint8 pixel = 0; dst->readBytes(&pixel, 9, 7, 1, 1);
    QCOMPARE(pixel, quint8(history && oldSource ? 0x31 : 0x55));
    QCOMPARE(writer->rawData(), pointer); *pointer = 0x66; writer.clear();
    src->readBytes(&pixel, 0, 0, 1, 1); QCOMPARE(pixel, quint8(0x66));
}

void KisPainterTest::testPageStoreBitBltSelfCopy_data()
{
    testPageStoreBitBltLiveSource_data();
}

void KisPainterTest::testPageStoreBitBltSelfCopy()
{
    QFETCH(bool, history); QFETCH(bool, oldSource);
    const auto cs = KoColorSpaceRegistry::instance()->alpha8();
    KisPaintDeviceSP device = new KisPaintDevice(cs);
    QByteArray before(384, Qt::Uninitialized);
    for (int i = 0; i < before.size(); ++i) before[i] = char((17 * i + 23) % 251);
    device->writeBytes(reinterpret_cast<const quint8 *>(before.constData()), 0, 0, before.size(), 1);
    KisPainter owner(device); if (history) owner.beginTransaction();
    const auto cleanup = qScopeGuard([&] { if (history) owner.deleteTransaction(); });
    QByteArray current = before;
    if (history) {
        for (int i = 0; i < current.size(); ++i) current[i] = char((29 * i + 37) % 251);
        device->writeBytes(reinterpret_cast<const quint8 *>(current.constData()), 0, 0, current.size(), 1);
    }
    QByteArray expected = current;
    const QByteArray input = history && oldSource ? before : current;
    memcpy(expected.data() + 37, input.constData(), 300);
    KisPainter painter(device); painter.setCompositeOpId(COMPOSITE_COPY);
    if (oldSource) painter.bitBltOldData(QPoint(37, 0), device, QRect(0, 0, 300, 1));
    else painter.bitBlt(QPoint(37, 0), device, QRect(0, 0, 300, 1));
    painter.end();
    QByteArray actual(expected.size(), Qt::Uninitialized);
    device->readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, actual.size(), 1);
    QCOMPARE(actual, expected);
}

void KisPainterTest::testPageStoreBitBltWriteOperation_data()
{
    QTest::addColumn<bool>("history"); QTest::addColumn<bool>("wrapped"); QTest::addColumn<bool>("borrowed");
    for (bool history : {false,true}) for (bool wrapped : {false,true}) for (bool borrowed : {false,true})
        QTest::newRow(qPrintable(QStringLiteral("history%1-wrap%2-borrow%3").arg(history).arg(wrapped).arg(borrowed)))
            << history << wrapped << borrowed;
}

void KisPainterTest::testPageStoreBitBltWriteOperation()
{
    QFETCH(bool, history); QFETCH(bool, wrapped); QFETCH(bool, borrowed);
    const auto cs = KoColorSpaceRegistry::instance()->alpha8();
    KisPaintDeviceSP src = new KisPaintDevice(cs), dst = new KisPaintDevice(cs);
    const QRect storage(-64,-64,384,256), source(-7,-5,139,91);
    const QPoint target(97,77), offset(13,-11);
    dst->moveTo(offset);
    const quint8 initial = 9;
    dst->fill(storage.translated(offset), KoColor(&initial, cs));
    if (wrapped) { dst->setDefaultBounds(new PainterWrapBounds); dst->setSupportsWraparoundMode(true); }
    QByteArray input(source.width() * source.height(), Qt::Uninitialized);
    for (int i = 0; i < input.size(); ++i) input[i] = char((i * 23 + 17) % 251);
    src->writeBytes(reinterpret_cast<const quint8 *>(input.constData()), source);
    KisPainter historyOwner(dst); if (history) historyOwner.beginTransaction();
    const auto cleanup = qScopeGuard([&] { if (history) historyOwner.deleteTransaction(); });
    KisRandomAccessorSP external;
    quint8 *pointer = nullptr;
    if (borrowed) { external = dst->createRandomAccessorNG(); external->moveTo(target.x(),target.y()); pointer = external->rawData(); QVERIFY(pointer); *pointer = 0x55; }
    bool bodyObserved = false, hidden = true;
    KisPageStoreDiagnosticRecorder recorder(true);
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::PixelOperationBody || bodyObserved || borrowed) return;
        bodyObserved = true;
        auto view = dst->createRandomConstAccessorNG(); view->moveTo(target.x(),target.y());
        hidden &= view->rawDataConst() && *view->rawDataConst() == initial;
    });
    KisPainter painter(dst); painter.setCompositeOpId(COMPOSITE_COPY);
    painter.bitBlt(target, src, source); painter.end();
    QCOMPARE(bodyObserved, !borrowed); QVERIFY(hidden);
    const auto seals = recorder.metrics()[size_t(KisPageStoreDiagnosticPhase::MutationSealPrivatePublish)].intervals;
    QCOMPARE(recorder.metrics()[size_t(KisPageStoreDiagnosticPhase::PixelOperationBody)].intervals, quint64(borrowed ? 0 : 1));
    if (!borrowed) QCOMPARE(seals, quint64(1));
    else {
        // The compatibility copy joins the accessor's single native mutation.
        // Nothing publishes while its raw pointer is still live; the final
        // iterator client seals the whole shared transaction exactly once.
        QCOMPARE(seals, quint64(0));
        QCOMPARE(external->rawData(), pointer);
        external.clear();
        QCOMPARE(recorder.metrics()[size_t(
                     KisPageStoreDiagnosticPhase::MutationSealPrivatePublish)].intervals,
                 quint64(1));
    }
    QByteArray expected(storage.width() * storage.height(), char(initial));
    for (int y = 0; y < source.height(); ++y) for (int x = 0; x < source.width(); ++x) {
        const int dx = (wrapped ? (target.x() + x) % 128 : target.x() + x) - offset.x();
        const int dy = (wrapped ? (target.y() + y) % 96 : target.y() + y) - offset.y();
        expected[(dy - storage.y()) * storage.width() + dx - storage.x()] = input[y * source.width() + x];
    }
    QByteArray actual(expected.size(), Qt::Uninitialized);
    dst->dataManager()->readBytes(reinterpret_cast<quint8 *>(actual.data()), storage.x(),storage.y(),storage.width(),storage.height());
    QCOMPARE(actual, expected);
}

void KisPainterTest::testPageStorePixelOperationCoordinates()
{
    const int low = std::numeric_limits<int>::min(), high = std::numeric_limits<int>::max();
    KisPaintDeviceSP device = new KisPaintDevice(KoColorSpaceRegistry::instance()->alpha8());
    bool called = false;
    device->moveTo(QPoint(low,0));
    QVERIFY(!device->applyPixelOperation(QRect(high,0,1,1), [&](KisPixelWriteCursor *) { called = true; return true; }));
    QVERIFY(!called);
    device->moveTo(QPoint(high,0));
    QVERIFY(!device->applyPixelOperation(QRect(low,0,1,1), [&](KisPixelWriteCursor *) { called = true; return true; }));
    QVERIFY(!called);
    QVERIFY(device->applyPixelOperation(QRect(high,0,1,1), [&](KisPixelWriteCursor *cursor) {
        cursor->moveTo(high,0); if (!cursor->rawData()) return false; *cursor->rawData() = 0x55; return true;
    }));
    bool rejected = false;
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression("^PageStore pixel operation failed:.*"));
    QVERIFY(!device->applyPixelOperation(QRect(high,0,1,1), [&](KisPixelWriteCursor *cursor) {
        cursor->moveTo(high,0); if (!cursor->rawData()) return false; *cursor->rawData() = 0x66;
        cursor->moveTo(low,0); rejected = !cursor->rawData(); return true; // even a careless caller cannot hide poison
    }));
    QVERIFY(rejected);
    quint8 actual = 0; device->dataManager()->readBytes(&actual,0,0,1,1); QCOMPARE(actual, quint8(0x55));
}

void KisPainterTest::allCsApplicator(void (KisPainterTest::* funcPtr)(const KoColorSpace*cs))
{
    qDebug() << qAppName();

    QList<const KoColorSpace*> colorspaces = KoColorSpaceRegistry::instance()->allColorSpaces(KoColorSpaceRegistry::AllColorSpaces, KoColorSpaceRegistry::OnlyDefaultProfile);

    Q_FOREACH (const KoColorSpace* cs, colorspaces) {

        QString csId = cs->id();
        // ALL THESE COLORSPACES ARE BROKEN: WE NEED UNITTESTS FOR COLORSPACES!
        if (csId.startsWith("KS")) continue;
        if (csId.startsWith("Xyz")) continue;
        if (csId.startsWith('Y')) continue;
        if (csId.contains("AF")) continue;
        if (csId == "GRAYU16") continue; // No point in testing bounds with a cs without alpha
        if (csId == "GRAYU8") continue; // No point in testing bounds with a cs without alpha

        dbgKrita << "Testing with cs" << csId;

        if (cs && cs->compositeOp(COMPOSITE_OVER) != 0) {
            (this->*funcPtr)(cs);
        } else {
            dbgKrita << "Cannot bitBlt for cs" << csId;
        }
    }
}

void KisPainterTest::testSimpleBlt(const KoColorSpace * cs)
{

    KisPaintDeviceSP dst = new KisPaintDevice(cs);
    KisPaintDeviceSP src = new KisPaintDevice(cs);
    KoColor c(Qt::red, cs);
    c.setOpacity(quint8(128));
    src->fill(20, 20, 20, 20, c.data());

    QCOMPARE(src->exactBounds(), QRect(20, 20, 20, 20));

    const KoCompositeOp* op;

    {
        op = cs->compositeOp(COMPOSITE_OVER);
        KisPainter painter(dst);
        painter.setCompositeOpId(op);
        painter.bitBlt(50, 50, src, 20, 20, 20, 20);
        painter.end();
        QCOMPARE(dst->exactBounds(), QRect(50,50,20,20));
    }

    dst->clear();

    {
        op = cs->compositeOp(COMPOSITE_COPY);
        KisPainter painter(dst);
        painter.setCompositeOpId(op);
        painter.bitBlt(50, 50, src, 20, 20, 20, 20);
        painter.end();
        QCOMPARE(dst->exactBounds(), QRect(50,50,20,20));
    }
}

void KisPainterTest::testSimpleBlt()
{
    allCsApplicator(&KisPainterTest::testSimpleBlt);
}

/*

Note: the bltSelection tests assume the following geometry:

0,0               0,30
  +---------+------+
  |  10,10  |      |
  |    +----+      |
  |    |####|      |
  |    |####|      |
  +----+----+      |
  |       20,20    |
  |                |
  |                |
  +----------------+
                  30,30
 */
void KisPainterTest::testPaintDeviceBltSelection(const KoColorSpace * cs)
{

    KisPaintDeviceSP dst = new KisPaintDevice(cs);

    KisPaintDeviceSP src = new KisPaintDevice(cs);
    KoColor c(Qt::red, cs);
    c.setOpacity(quint8(128));
    src->fill(0, 0, 20, 20, c.data());

    QCOMPARE(src->exactBounds(), QRect(0, 0, 20, 20));

    KisSelectionSP selection = new KisSelection();
    selection->pixelSelection()->select(QRect(10, 10, 20, 20));
    selection->updateProjection();
    QCOMPARE(selection->selectedExactRect(), QRect(10, 10, 20, 20));

    KisPainter painter(dst);
    painter.setSelection(selection);

    painter.bitBlt(0, 0, src, 0, 0, 30, 30);
    painter.end();

    QImage image = dst->convertToQImage(0);
    image.save("blt_Selection_" + cs->name() + ".png");

    QCOMPARE(dst->exactBounds(), QRect(10, 10, 10, 10));

    const KoCompositeOp* op = cs->compositeOp(COMPOSITE_SUBTRACT);
    if (op->id() == COMPOSITE_SUBTRACT) {

        KisPaintDeviceSP dst2 = new KisPaintDevice(cs);
        KisPainter painter2(dst2);
        painter2.setSelection(selection);
        painter2.setCompositeOpId(op);
        painter2.bitBlt(0, 0, src, 0, 0, 30, 30);
        painter2.end();

        QCOMPARE(dst2->exactBounds(), QRect(10, 10, 10, 10));
    }
}

void KisPainterTest::testPaintDeviceBltSelection()
{
    allCsApplicator(&KisPainterTest::testPaintDeviceBltSelection);
}

void KisPainterTest::testPaintDeviceBltSelectionIrregular(const KoColorSpace * cs)
{

    KisPaintDeviceSP dst = new KisPaintDevice(cs);
    KisPaintDeviceSP src = new KisPaintDevice(cs);
    KisFillPainter gc(src);
    gc.fillRect(0, 0, 20, 20, KoColor(Qt::red, cs));
    gc.end();

    QCOMPARE(src->exactBounds(), QRect(0, 0, 20, 20));

    KisSelectionSP sel = new KisSelection();

    KisPixelSelectionSP psel = sel->pixelSelection();
    psel->select(QRect(10, 15, 20, 15));
    psel->select(QRect(15, 10, 15, 5));

    QCOMPARE(psel->selectedExactRect(), QRect(10, 10, 20, 20));
    QCOMPARE(TestUtil::alphaDevicePixel(psel, 13, 13), MIN_SELECTED);

    KisPainter painter(dst);
    painter.setSelection(sel);
    painter.bitBlt(0, 0, src, 0, 0, 30, 30);
    painter.end();

    QImage image = dst->convertToQImage(0);
    image.save("blt_Selection_irregular" + cs->name() + ".png");

    QCOMPARE(dst->exactBounds(), QRect(10, 10, 10, 10));
    Q_FOREACH (KoChannelInfo * channel, cs->channels()) {
        // Only compare alpha if there actually is an alpha channel in
        // this colorspace
        if (channel->channelType() == KoChannelInfo::ALPHA) {
            QColor c;

            dst->pixel(13, 13, &c);

            QCOMPARE((int) c.alpha(), (int) OPACITY_TRANSPARENT_U8);
        }
    }
}


void KisPainterTest::testPaintDeviceBltSelectionIrregular()
{
    allCsApplicator(&KisPainterTest::testPaintDeviceBltSelectionIrregular);
}

void KisPainterTest::testPaintDeviceBltSelectionInverted(const KoColorSpace * cs)
{

    KisPaintDeviceSP dst = new KisPaintDevice(cs);
    KisPaintDeviceSP src = new KisPaintDevice(cs);
    KisFillPainter gc(src);
    gc.fillRect(0, 0, 30, 30, KoColor(Qt::red, cs));
    gc.end();
    QCOMPARE(src->exactBounds(), QRect(0, 0, 30, 30));

    KisSelectionSP sel = new KisSelection();
    KisPixelSelectionSP psel = sel->pixelSelection();
    psel->select(QRect(10, 10, 20, 20));
    psel->invert();
    sel->updateProjection();

    KisPainter painter(dst);
    painter.setSelection(sel);
    painter.bitBlt(0, 0, src, 0, 0, 30, 30);
    painter.end();
    QCOMPARE(dst->exactBounds(), QRect(0, 0, 30, 30));
}

void KisPainterTest::testPaintDeviceBltSelectionInverted()
{
    allCsApplicator(&KisPainterTest::testPaintDeviceBltSelectionInverted);
}


void KisPainterTest::testSelectionBltSelection()
{
    KisPixelSelectionSP src = new KisPixelSelection();
    src->select(QRect(0, 0, 20, 20));
    QCOMPARE(src->selectedExactRect(), QRect(0, 0, 20, 20));

    KisSelectionSP sel = new KisSelection();

    KisPixelSelectionSP Selection = sel->pixelSelection();
    Selection->select(QRect(10, 10, 20, 20));
    QCOMPARE(Selection->selectedExactRect(), QRect(10, 10, 20, 20));

    sel->updateProjection();
    KisPixelSelectionSP dst = new KisPixelSelection();
    KisPainter painter(dst);
    painter.setSelection(sel);
    painter.bitBlt(0, 0, src, 0, 0, 30, 30);
    painter.end();

    QCOMPARE(dst->selectedExactRect(), QRect(10, 10, 10, 10));

    KisSequentialConstIterator it(dst, QRect(10, 10, 10, 10));
    while (it.nextPixel()) {
        // These are selections, so only one channel and it should
        // be totally selected
        QCOMPARE(it.oldRawData()[0], MAX_SELECTED);
    }
}

/*

Test with non-square selection

0,0               0,30
  +-----------+------+
  |    13,13  |      |
  |      x +--+      |
  |     +--+##|      |
  |     |#####|      |
  +-----+-----+      |
  |         20,20    |
  |                  |
  |                  |
  +------------------+
                  30,30
 */
void KisPainterTest::testSelectionBltSelectionIrregular()
{

    KisPaintDeviceSP dev = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());

    KisPixelSelectionSP src = new KisPixelSelection();
    src->select(QRect(0, 0, 20, 20));
    QCOMPARE(src->selectedExactRect(), QRect(0, 0, 20, 20));

    KisSelectionSP sel = new KisSelection();

    KisPixelSelectionSP Selection = sel->pixelSelection();
    Selection->select(QRect(10, 15, 20, 15));
    Selection->select(QRect(15, 10, 15, 5));
    QCOMPARE(Selection->selectedExactRect(), QRect(10, 10, 20, 20));
    QCOMPARE(TestUtil::alphaDevicePixel(Selection, 13, 13), MIN_SELECTED);

    sel->updateProjection();

    KisPixelSelectionSP dst = new KisPixelSelection();
    KisPainter painter(dst);
    painter.setSelection(sel);
    painter.bitBlt(0, 0, src, 0, 0, 30, 30);
    painter.end();

    QCOMPARE(dst->selectedExactRect(), QRect(10, 10, 10, 10));
    QCOMPARE(TestUtil::alphaDevicePixel(dst, 13, 13), MIN_SELECTED);
}

void KisPainterTest::testSelectionBitBltFixedSelection()
{
    const KoColorSpace* cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dst = new KisPaintDevice(cs);

    KisPaintDeviceSP src = new KisPaintDevice(cs);
    KoColor c(Qt::red, cs);
    c.setOpacity(quint8(128));
    src->fill(0, 0, 20, 20, c.data());

    QCOMPARE(src->exactBounds(), QRect(0, 0, 20, 20));

    KisFixedPaintDeviceSP fixedSelection = new KisFixedPaintDevice(cs);
    fixedSelection->setRect(QRect(0, 0, 20, 20));
    fixedSelection->initialize();
    KoColor fill(Qt::white, cs);
    fixedSelection->fill(5, 5, 10, 10, fill.data());
    fixedSelection->convertTo(KoColorSpaceRegistry::instance()->alpha8());

    KisPainter painter(dst);

    painter.bitBltWithFixedSelection(0, 0, src, fixedSelection, 20, 20);
    painter.end();

    QCOMPARE(dst->exactBounds(), QRect(5, 5, 10, 10));
    /*
dbgKrita << "canary1.5";
    dst->clear();
    painter.begin(dst);

    painter.bitBltWithFixedSelection(0, 0, src, fixedSelection, 10, 20);
    painter.end();
dbgKrita << "canary2";
    QCOMPARE(dst->exactBounds(), QRect(5, 5, 5, 10));

    dst->clear();
    painter.begin(dst);

    painter.bitBltWithFixedSelection(0, 0, src, fixedSelection, 5, 5, 5, 5, 10, 20);
    painter.end();
dbgKrita << "canary3";
    QCOMPARE(dst->exactBounds(), QRect(5, 5, 5, 10));

    dst->clear();
    painter.begin(dst);

    painter.bitBltWithFixedSelection(5, 5, src, fixedSelection, 10, 20);
    painter.end();
dbgKrita << "canary4";
    QCOMPARE(dst->exactBounds(), QRect(10, 10, 5, 10));
    */
}

void KisPainterTest::testSelectionBitBltEraseCompositeOp()
{
    const KoColorSpace* cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dst = new KisPaintDevice(cs);
    KoColor c(Qt::red, cs);
    dst->fill(0, 0, 150, 150, c.data());

    KisPaintDeviceSP src = new KisPaintDevice(cs);
    KoColor c2(Qt::black, cs);
    src->fill(50, 50, 50, 50, c2.data());

    KisSelectionSP sel = new KisSelection();
    KisPixelSelectionSP selection = sel->pixelSelection();
    selection->select(QRect(25, 25, 100, 100));
    sel->updateProjection();

    const KoCompositeOp* op = cs->compositeOp(COMPOSITE_ERASE);
    KisPainter painter(dst);
    painter.setSelection(sel);
    painter.setCompositeOpId(op);
    painter.bitBlt(0, 0, src, 0, 0, 150, 150);
    painter.end();

    //dst->convertToQImage(0).save("result.png");

    QRect erasedRect(50, 50, 50, 50);
    KisSequentialConstIterator it(dst, QRect(0, 0, 150, 150));
    while (it.nextPixel()) {
        if(!erasedRect.contains(it.x(), it.y())) {
             QVERIFY(memcmp(it.oldRawData(), c.data(), cs->pixelSize()) == 0);
        }
    }

}

void KisPainterTest::testSimpleAlphaCopy()
{
    KisPaintDeviceSP src = new KisPaintDevice(KoColorSpaceRegistry::instance()->alpha8());
    KisPaintDeviceSP dst = new KisPaintDevice(KoColorSpaceRegistry::instance()->alpha8());
    quint8 p = 128;
    src->fill(0, 0, 100, 100, &p);
    QVERIFY(src->exactBounds() == QRect(0, 0, 100, 100));
    KisPainter gc(dst);
    gc.setCompositeOpId(KoColorSpaceRegistry::instance()->alpha8()->compositeOp(COMPOSITE_COPY));
    gc.bitBlt(QPoint(0, 0), src, src->exactBounds());
    gc.end();
    QCOMPARE(dst->exactBounds(), QRect(0, 0, 100, 100));

}

void KisPainterTest::checkPerformance()
{
    KisPaintDeviceSP src = new KisPaintDevice(KoColorSpaceRegistry::instance()->alpha8());
    KisPaintDeviceSP dst = new KisPaintDevice(KoColorSpaceRegistry::instance()->alpha8());
    quint8 p = 128;
    src->fill(0, 0, 10000, 5000, &p);
    KisSelectionSP sel = new KisSelection();
    sel->pixelSelection()->select(QRect(0, 0, 10000, 5000), 128);
    sel->updateProjection();

    QElapsedTimer t;
    t.start();
    for (int i = 0; i < 10; ++i) {
        KisPainter gc(dst);
        gc.bitBlt(0, 0, src, 0, 0, 10000, 5000);
    }

    t.restart();
    for (int i = 0; i < 10; ++i) {
        KisPainter gc(dst, sel);
        gc.bitBlt(0, 0, src, 0, 0, 10000, 5000);
    }
}

void KisPainterTest::testBitBltOldData()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->alpha8();

    KisPaintDeviceSP src = new KisPaintDevice(cs);
    KisPaintDeviceSP dst = new KisPaintDevice(cs);

    quint8 defaultPixel = 0;
    quint8 p1 = 128;
    quint8 p2 = 129;
    quint8 p3 = 130;
    KoColor defaultColor(&defaultPixel, cs);
    KoColor color1(&p1, cs);
    KoColor color2(&p2, cs);
    KoColor color3(&p3, cs);
    QRect fillRect(0,0,5000,5000);

    src->fill(fillRect, color1);

    KisPainter srcGc(src);
    srcGc.beginTransaction();
    src->fill(fillRect, color2);

    KisPainter dstGc(dst);
    dstGc.bitBltOldData(QPoint(), src, fillRect);

    QVERIFY(TestUtil::checkAlphaDeviceFilledWithPixel(dst, fillRect, p1));

    dstGc.end();
    srcGc.deleteTransaction();
}

#include "kis_paint_device_debug_utils.h"
#include "KisRenderedDab.h"

void testMassiveBltFixedImpl(int numRects, bool varyOpacity = false, bool useSelection = false)
{
    const KoColorSpace* cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dst = new KisPaintDevice(cs);

    QList<QColor> colors;
    colors << Qt::red;
    colors << Qt::green;
    colors << Qt::blue;

    QRect devicesRect;
    QList<KisRenderedDab> devices;

    for (int i = 0; i < numRects; i++) {
        const QRect rc(10 + i * 10, 10 + i * 10, 30, 30);
        KisFixedPaintDeviceSP dev = new KisFixedPaintDevice(cs);
        dev->setRect(rc);
        dev->initialize();
        dev->fill(rc, KoColor(colors[i % 3], cs));
        dev->fill(kisGrowRect(rc, -5), KoColor(Qt::white, cs));

        KisRenderedDab dab;
        dab.device = dev;
        dab.offset = dev->bounds().topLeft();
        dab.opacity = varyOpacity ? qreal(1 + i) / numRects : 1.0;
        dab.flow = 1.0;

        devices << dab;
        devicesRect |= rc;
    }

    KisSelectionSP selection;

    if (useSelection) {
        selection = new KisSelection();
        selection->pixelSelection()->select(kisGrowRect(devicesRect, -7));
    }

    const QString opacityPostfix = varyOpacity ? "_varyop" : "";
    const QString selectionPostfix = useSelection ? "_sel" : "";

    const QRect fullRect = kisGrowRect(devicesRect, 10);

    {
        KisPainter painter(dst);
        painter.setSelection(selection);
        painter.bltFixed(fullRect, devices);
        painter.end();
        QVERIFY(TestUtil::checkQImage(dst->convertToQImage(0, fullRect),
                                      "kispainter_test",
                                      "massive_bitblt",
                                      QString("full_update_%1%2%3")
                                          .arg(numRects)
                                          .arg(opacityPostfix)
                                          .arg(selectionPostfix), 2, 2));
    }

    dst->clear();

    {
        KisPainter painter(dst);
        painter.setSelection(selection);

        for (int i = fullRect.x(); i <= fullRect.center().x(); i += 10) {
            const QRect rc(i, fullRect.y(), 10, fullRect.height());
            painter.bltFixed(rc, devices);
        }

        painter.end();

        QVERIFY(TestUtil::checkQImage(dst->convertToQImage(0, fullRect),
                                      "kispainter_test",
                                      "massive_bitblt",
                                      QString("partial_update_%1%2%3")
                                          .arg(numRects)
                                          .arg(opacityPostfix)
                                          .arg(selectionPostfix), 2, 2));

    }
}

void KisPainterTest::testMassiveBltFixedSingleTile()
{
    testMassiveBltFixedImpl(3);
}

void KisPainterTest::testMassiveBltFixedMultiTile()
{
    testMassiveBltFixedImpl(6);
}

void KisPainterTest::testMassiveBltFixedMultiTileWithOpacity()
{
    testMassiveBltFixedImpl(6, true);
}

void KisPainterTest::testMassiveBltFixedMultiTileWithSelection()
{
    testMassiveBltFixedImpl(6, false, true);
}

void KisPainterTest::testMassiveBltFixedCornerCases()
{
    const KoColorSpace* cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dst = new KisPaintDevice(cs);

    QList<KisRenderedDab> devices;

    QVERIFY(dst->extent().isEmpty());

    {
        // empty devices, shouldn't crash
        KisPainter painter(dst);
        painter.bltFixed(QRect(60,60,20,20), devices);
        painter.end();
    }

    QVERIFY(dst->extent().isEmpty());

    const QRect rc(10,10,20,20);
    KisFixedPaintDeviceSP dev = new KisFixedPaintDevice(cs);
    dev->setRect(rc);
    dev->initialize();
    dev->fill(rc, KoColor(Qt::white, cs));

    devices.append(KisRenderedDab(dev));

    {
        // rect outside the devices bounds, shouldn't crash
        KisPainter painter(dst);
        painter.bltFixed(QRect(60,60,20,20), devices);
        painter.end();
    }

    QVERIFY(dst->extent().isEmpty());
}


#include "kis_lod_transform.h"

inline QRect extentifyRect(const QRect &rc)
{
    return KisLodTransform::alignedRect(rc, 6);
}

void testOptimizedCopyingImpl(const QRect &srcRect,
                              const QRect &dstRect,
                              const QRect &srcCopyRect,
                              const QPoint &dstPt,
                              const QRect &expectedDstBounds)
{
    const QRect expectedDstExtent = extentifyRect(expectedDstBounds);

    const KoColorSpace* cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP src = new KisPaintDevice(cs);
    KisPaintDeviceSP dst = new KisPaintDevice(cs);

    const KoColor color1(Qt::red, cs);
    const KoColor color2(Qt::blue, cs);

    src->fill(srcRect, color1);
    dst->fill(dstRect, color2);

    KisPainter::copyAreaOptimized(dstPt, src, dst, srcCopyRect);

    //KIS_DUMP_DEVICE_2(dst, QRect(0,0,5000,5000), "dst", "dd");

    QCOMPARE(dst->exactBounds(), expectedDstBounds);
    QCOMPARE(dst->extent(), expectedDstExtent);
}

void KisPainterTest::testOptimizedCopying()
{
    const QRect srcRect(1000, 1000, 1000, 1000);
    const QRect srcCopyRect(0, 0, 5000, 5000);


    testOptimizedCopyingImpl(srcRect, QRect(6000, 500, 1000,1000),
                             srcCopyRect, srcCopyRect.topLeft(),
                             QRect(1000, 500, 6000, 1500));

    testOptimizedCopyingImpl(srcRect, QRect(4500, 1500, 1000, 1000),
                             srcCopyRect, srcCopyRect.topLeft(),
                             QRect(1000, 1000, 4500, 1500));

    testOptimizedCopyingImpl(srcRect, QRect(2500, 2500, 1000, 1000),
                             srcCopyRect, srcCopyRect.topLeft(),
                             srcRect);

    testOptimizedCopyingImpl(srcRect, QRect(1200, 1200, 600, 1600),
                             srcCopyRect, srcCopyRect.topLeft(),
                             srcRect);

    testOptimizedCopyingImpl(srcRect, QRect(1200, 1200, 600, 600),
                             srcCopyRect, srcCopyRect.topLeft(),
                             srcRect);

}

KISTEST_MAIN(KisPainterTest)

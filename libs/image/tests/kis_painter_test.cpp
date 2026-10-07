/*
 *  SPDX-FileCopyrightText: 2007 Sven Langkamp <sven.langkamp@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_painter_test.h"
#include <simpletest.h>

#include <kis_debug.h>
#include <QRect>
#include <QRegion>
#include <brushengine/kis_paintop_utils.h>
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
#include <colorspaces/KoAlphaColorSpace.h>

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
#include "kis_transaction.h"
#include "kis_surrogate_undo_adapter.h"
#include "KisRenderedDab.h"
#include "KisStrokeJobFailureContext.h"
#include <QSemaphore>
#include <thread>
#include <vector>

namespace {
class PainterWrapBounds : public KisDefaultBoundsBase
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
class PainterFootprintBounds final : public PainterWrapBounds
{
public:
    PainterFootprintBounds(QRect rect, WrapAroundAxis axis) : m_rect(rect), m_axis(axis) {}
    QRect bounds() const override { return m_rect; }
    WrapAroundAxis wrapAroundModeAxis() const override { return m_axis; }
private:
    const QRect m_rect;
    const WrapAroundAxis m_axis;
};
}






namespace {
// Counterfactual implementation: retain the pre-cursor read/compose/writeBytes
// behavior as an independent oracle, including wrap's first-period write rule.
void legacyFixedBlit(KisPaintDeviceSP target, KisFixedPaintDeviceSP source,
                     QRect sourceRect, QPoint destination, KisSelectionSP selection,
                     KoCompositeOp::ParameterInfo params, const QString &composite)
{
    const QRect rect(destination, sourceRect.size());
    const auto *cs = target->colorSpace();
    const int bpp = cs->pixelSize();
    QByteArray pixels(rect.width() * rect.height() * bpp, Qt::Uninitialized);
    target->readBytes(reinterpret_cast<quint8 *>(pixels.data()), rect);
    QByteArray mask;
    if (selection) {
        mask.resize(rect.width() * rect.height());
        selection->projection()->readBytes(reinterpret_cast<quint8 *>(mask.data()), rect);
    }
    const auto bounds = source->bounds();
    params.dstRowStart = reinterpret_cast<quint8 *>(pixels.data()); params.dstRowStride = rect.width() * bpp;
    params.srcRowStart = source->constData() + ((sourceRect.y() - bounds.y()) * bounds.width() + sourceRect.x() - bounds.x()) * source->pixelSize();
    params.srcRowStride = bounds.width() * source->pixelSize();
    params.maskRowStart = selection ? reinterpret_cast<const quint8 *>(mask.constData()) : nullptr;
    params.maskRowStride = selection ? rect.width() : 0; params.rows = rect.height(); params.cols = rect.width();
    cs->bitBlt(source->colorSpace(), params, cs->compositeOp(composite, source->colorSpace()),
              KoColorConversionTransformation::internalRenderingIntent(), KoColorConversionTransformation::internalConversionFlags());
    target->writeBytes(reinterpret_cast<const quint8 *>(pixels.constData()), rect);
}

void legacyMirroredBlits(KisPaintDeviceSP target, KisPaintDeviceSP source, QRect rect,
                         QPoint center, int mirrors, KisSelectionSP selection,
                         KoCompositeOp::ParameterInfo params)
{
    if (!mirrors) return;
    KisFixedPaintDeviceSP fixed = new KisFixedPaintDevice(source->colorSpace());
    fixed->setRect(QRect(QPoint(), rect.size())); fixed->initialize(); source->readBytes(fixed->data(), rect);
    const int mx = 2 * center.x() - rect.x() - rect.width();
    const int my = 2 * center.y() - rect.y() - rect.height();
    const auto blit = [&](QPoint at) { legacyFixedBlit(target, fixed, fixed->bounds(), at, selection, params, COMPOSITE_OVER); };
    if (mirrors & 1) { fixed->mirror(true, false); blit({mx, rect.y()}); }
    if (mirrors & 2) {
        fixed->mirror(false, true); blit({mirrors & 1 ? mx : rect.x(), my});
        if (mirrors & 1) { fixed->mirror(true, false); blit({rect.x(), my}); }
    }
}

bool sameFixedPixels(const QByteArray &a, const QByteArray &b, int bpp)
{
    if (a.size() != b.size()) return false;
    if (bpp != 16) return a == b;
    const auto *x = reinterpret_cast<const float *>(a.constData());
    const auto *y = reinterpret_cast<const float *>(b.constData());
    for (int i = 0; i < a.size() / int(sizeof(float)); ++i)
        if (!std::isfinite(x[i]) || !std::isfinite(y[i]) || std::abs(x[i] - y[i]) > 0.000005f) return false;
    return true;
}
}


void KisPainterTest::testAlphaDarkenSpanInvariant_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("masked"); QTest::addColumn<double>("average"); QTest::addColumn<double>("flow");
    for (int bpp : {8, 16}) for (bool mask : {false, true}) for (double average : {0.31, 0.81}) for (double flow : {0.71, 1.0})
        QTest::newRow(qPrintable(QString("B%1-mask%2-average%3-flow%4").arg(bpp).arg(mask).arg(average).arg(flow))) << bpp << mask << average << flow;
}

void KisPainterTest::testAlphaDarkenSpanInvariant()
{
    QFETCH(int, bpp); QFETCH(bool, masked); QFETCH(double, average); QFETCH(double, flow);
    const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", bpp == 8 ? "U16" : "F32", ""); QVERIFY(cs);
    constexpr int width = 193, rows = 3;
    QByteArray source(width * rows * bpp, Qt::Uninitialized), initial(source.size(), Qt::Uninitialized), mask(width * rows, Qt::Uninitialized);
    for (int y = 0; y < rows; ++y) for (int x = 0; x < width; ++x) {
        const int i = y * width + x;
        KoColor src(QColor(x * 17 % 256, y * 83 % 256, (x * 7 + y * 11) % 256, x % 5 ? (x + y * 13) % 256 : 0), cs);
        KoColor dst(QColor(x * 13 % 256, 53, 83, x % 7 ? 117 : 0), cs);
        std::memcpy(source.data() + i * bpp, src.data(), size_t(bpp)); std::memcpy(initial.data() + i * bpp, dst.data(), size_t(bpp));
        mask[i] = char(i * 17 % 256);
    }
    const auto render = [&](QByteArray &pixels, int offset, int chunk) {
        KoCompositeOp::ParameterInfo params; params.setOpacityAndAverage(0.63, average); params.flow = flow;
        for (int repeat = 0; repeat < 2; ++repeat) for (int y = 0; y < rows; ++y) for (int x = 0; x < width; x += chunk) {
            const int i = y * width + x;
            params.dstRowStart = reinterpret_cast<quint8 *>(pixels.data()) + offset + i * bpp; params.dstRowStride = width * bpp;
            params.srcRowStart = reinterpret_cast<const quint8 *>(source.constData()) + i * bpp; params.srcRowStride = width * bpp;
            params.maskRowStart = masked ? reinterpret_cast<const quint8 *>(mask.constData()) + i : nullptr; params.maskRowStride = width;
            params.rows = 1; params.cols = qMin(chunk, width - x);
            cs->bitBlt(cs, params, cs->compositeOp(COMPOSITE_ALPHA_DARKEN), KoColorConversionTransformation::internalRenderingIntent(), KoColorConversionTransformation::internalConversionFlags());
        }
    };
    QByteArray wide(initial.constData(), initial.size()); render(wide, 0, width); QVERIFY(wide != initial);
    for (int alignment : {0, 1, 3}) for (int chunk : {1, 3, 17, 64}) {
        const int prefix = (4 + alignment) * bpp;
        QByteArray actual(prefix + initial.size() + 4 * bpp, char(0x39)); std::memcpy(actual.data() + prefix, initial.constData(), size_t(initial.size()));
        render(actual, prefix, chunk);
        QVERIFY2(sameFixedPixels(actual.mid(prefix, initial.size()), wide, bpp), qPrintable(QString("alignment%1-chunk%2").arg(alignment).arg(chunk)));
        QCOMPARE(actual.left(prefix), QByteArray(prefix, char(0x39))); QCOMPARE(actual.right(4 * bpp), QByteArray(4 * bpp, char(0x39)));
    }
}

void KisPainterTest::testFixedCursor_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("mode"); QTest::addColumn<bool>("masked");
    QTest::addColumn<int>("wrapMode"); QTest::addColumn<QString>("composite");
    for (int bpp : {1, 4, 8, 16}) for (int mode : {0, 1, 2}) for (bool masked : {false, true})
        for (int wrap : {0, 1, 2, 3}) for (const QString composite : {COMPOSITE_OVER, COMPOSITE_COPY, COMPOSITE_ALPHA_DARKEN})
            QTest::newRow(qPrintable(QString("B%1-mode%2-mask%3-wrap%4-%5").arg(bpp).arg(mode).arg(masked).arg(wrap).arg(composite)))
                << bpp << mode << masked << wrap << composite;
}

void KisPainterTest::testFixedCursor()
{
    QFETCH(int, bpp); QFETCH(int, mode); QFETCH(bool, masked); QFETCH(int, wrapMode); QFETCH(QString, composite);
    auto *registry = KoColorSpaceRegistry::instance();
    const auto *cs = bpp == 1 ? registry->alpha8() : registry->colorSpace("RGBA", bpp == 4 ? "U8" : bpp == 8 ? "U16" : "F32", "");
    QVERIFY(cs);
    const QRect window(-128, -128, 384, 448), sourceRect(-2, 13, 189, 137);
    const QPoint at(-23, 77), offset(13, -11);
    KisPaintDeviceSP actual = new KisPaintDevice(cs), reference = new KisPaintDevice(cs);
    for (auto dev : {actual, reference}) {
        dev->moveTo(offset); dev->fill(window.translated(offset), KoColor(QColor(27, 53, 83, 117), cs));
        if (wrapMode) {
            dev->setDefaultBounds(new PainterFootprintBounds(QRect(0, 0, 128, 96), wrapMode == 1 ? WRAPAROUND_HORIZONTAL : wrapMode == 2 ? WRAPAROUND_VERTICAL : WRAPAROUND_BOTH));
            dev->setSupportsWraparoundMode(true);
        }
    }
    const auto bytes = [&](KisPaintDeviceSP dev) {
        QByteArray result(window.width() * window.height() * bpp, Qt::Uninitialized);
        dev->dataManager()->readBytes(reinterpret_cast<quint8 *>(result.data()), window.x(), window.y(), window.width(), window.height()); return result;
    };
    const QByteArray before = bytes(actual);
    KisFixedPaintDeviceSP fixed = new KisFixedPaintDevice(cs); fixed->setRect(QRect(-9, 7, 209, 155)); QVERIFY(fixed->initialize());
    for (int y = 0; y < fixed->bounds().height(); ++y) for (int x = 0; x < fixed->bounds().width(); ++x) {
        KoColor color(QColor((x * 17) % 256, (y * 19) % 256, (x * 7 + y * 11) % 256, x % 5 ? (x + y * 13) % 256 : 0), cs);
        std::memcpy(fixed->data() + (y * fixed->bounds().width() + x) * bpp, color.data(), size_t(bpp));
    }
    KisSelectionSP selection;
    if (masked) {
        selection = new KisSelection(); QByteArray mask(sourceRect.width() * sourceRect.height(), Qt::Uninitialized);
        for (int i = 0; i < mask.size(); ++i) mask[i] = char((i * 17) % 256);
        selection->pixelSelection()->writeBytes(reinterpret_cast<const quint8 *>(mask.constData()), QRect(at, sourceRect.size()));
    }
    KoCompositeOp::ParameterInfo params; params.setOpacityAndAverage(0.63, 0.81); params.flow = 0.71;
    if (bpp != 1) { params.channelFlags = QBitArray(cs->channelCount(), true); params.channelFlags.setBit(0, false); }
    KisPainter painter(actual, selection); painter.setCompositeOpId(composite); painter.setOpacityF(params.opacity);
    painter.setAverageOpacity(*params.lastOpacity); painter.setFlow(params.flow); painter.setChannelFlags(params.channelFlags);
    KisTransaction owner(actual); if (mode) QVERIFY(owner.beginStrokeMutation());
    painter.setStrokeMutationOwner(mode ? &owner : nullptr);
    KisStrokeJobFailureContext failure(true);
    for (int repeat = 0; repeat < 2; ++repeat) {
        painter.bltFixed(at.x(), at.y(), fixed, sourceRect.x(), sourceRect.y(), sourceRect.width(), sourceRect.height());
        QVERIFY(!failure.failed()); legacyFixedBlit(reference, fixed, sourceRect, at, selection, params, composite);
    }
    if (mode) QVERIFY(owner.checkpointStrokeMutation());
    const QByteArray expected = bytes(reference); QVERIFY(expected != before);
    const QByteArray result = bytes(actual);
    if (!sameFixedPixels(result, expected, bpp)) {
        int count = 0, first = -1, maxDifference = 0;
        for (int i = 0; i < result.size(); ++i) if (result[i] != expected[i]) {
            if (first < 0) first = i;
            ++count; maxDifference = qMax(maxDifference, qAbs(int(quint8(result[i])) - int(quint8(expected[i]))));
        }
        qDebug() << "fixed difference" << count << maxDifference << first << result.mid(first - first % bpp, bpp).toHex() << expected.mid(first - first % bpp, bpp).toHex();
    }
    QVERIFY2(sameFixedPixels(result, expected, bpp), "fixed-source output differs from old read/compose/write oracle");
    QVERIFY(!painter.takeDirtyRegion().isEmpty());
    KisSurrogateUndoAdapter undo;
    if (mode == 2) { QVERIFY(owner.tryRevert()); QCOMPARE(bytes(actual), before); }
    else { QVERIFY(owner.tryCommit(&undo)); undo.undo(); QCOMPARE(bytes(actual), before); undo.redo(); QVERIFY(sameFixedPixels(bytes(actual), expected, bpp)); }
}

void KisPainterTest::testFixedCursorReuse_data()
{
    QTest::addColumn<int>("repeats"); for (int n : {1, 10, 1000}) QTest::newRow(qPrintable(QString::number(n))) << n;
}

void KisPainterTest::testFixedCursorReuse()
{
    QFETCH(int, repeats);
    const auto *cs = KoColorSpaceRegistry::instance()->alpha8(); KisPaintDeviceSP dev = new KisPaintDevice(cs);
    KisFixedPaintDeviceSP source = new KisFixedPaintDevice(cs); source->setRect(QRect(-7, 9, 128, 1)); QVERIFY(source->initialize(71));
    KisTransaction owner(dev); QVERIFY(owner.beginStrokeMutation()); KisPainter painter(dev); painter.setCompositeOpId(COMPOSITE_COPY);
    painter.setStrokeMutationOwner(&owner);
    KisStrokeJobFailureContext failure(true); KisPageStoreDiagnosticRecorder recorder(true);
    const auto count = [&](auto phase) { return recorder.metrics()[size_t(phase)].intervals; };
    for (int i = 0; i < repeats; ++i) painter.bltFixed(0, 0, source, -7, 9, 128, 1);
    QVERIFY(!failure.failed()); QCOMPARE(count(KisPageStoreDiagnosticPhase::WriteProviderPrepare), quint64(2));
    QCOMPARE(count(KisPageStoreDiagnosticPhase::PixelOperationBody), quint64(repeats));
    QCOMPARE(count(KisPageStoreDiagnosticPhase::MutationSealInputs), quint64(0));
    QVERIFY(owner.checkpointStrokeMutation());
    auto frozen = dev->createRandomConstAccessorNG(); frozen->moveTo(0, 0); QCOMPARE(*frozen->rawDataConst(), quint8(71));
    QVERIFY(source->initialize(93)); painter.bltFixed(0, 0, source, -7, 9, 128, 1); QVERIFY(!failure.failed());
    QCOMPARE(count(KisPageStoreDiagnosticPhase::WriteProviderPrepare), quint64(4)); QCOMPARE(*frozen->rawDataConst(), quint8(71));
    frozen.clear(); QVERIFY(owner.tryRevert());
}

void KisPainterTest::testSharpnessMirrorOwner_data()
{
    QTest::addColumn<int>("wrapMode"); QTest::addColumn<int>("mirrors"); QTest::addColumn<bool>("masked");
    for (int wrap : {0, 1, 2, 3}) for (int mirrors : {0, 1, 2, 3}) for (bool masked : {false, true})
        QTest::newRow(qPrintable(QString("wrap%1-mirror%2-mask%3").arg(wrap).arg(mirrors).arg(masked))) << wrap << mirrors << masked;
}

void KisPainterTest::testSharpnessMirrorOwner()
{
    QFETCH(int, wrapMode); QFETCH(int, mirrors); QFETCH(bool, masked);
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8(); const QRect window(-256, -256, 768, 640); const QPoint center(-17, 23);
    KisPaintDeviceSP actual = new KisPaintDevice(cs), reference = new KisPaintDevice(cs), source = new KisPaintDevice(cs);
    for (auto dev : {actual, reference}) {
        dev->moveTo(QPoint(13, -11));
        if (wrapMode) { dev->setDefaultBounds(new PainterFootprintBounds(QRect(0, 0, 128, 96), wrapMode == 1 ? WRAPAROUND_HORIZONTAL : wrapMode == 2 ? WRAPAROUND_VERTICAL : WRAPAROUND_BOTH)); dev->setSupportsWraparoundMode(true); }
    }
    KisPainter line(source); line.setPaintColor(KoColor(QColor(73, 139, 197, 211), cs)); line.drawDDALine(QPointF(-91, 63), QPointF(142, 123));
    const QRect rect = source->extent();
    KisSelectionSP selection; if (masked) { selection = new KisSelection(); selection->pixelSelection()->select(QRect(-140, -90, 270, 270), 179); }
    KisPainter painter(actual, selection), oracle(reference, selection);
    painter.setMirrorInformation(center, mirrors & 1, mirrors & 2); painter.setOpacityF(0.63); oracle.setOpacityF(0.63);
    KisTransaction owner(actual); QVERIFY(owner.beginStrokeMutation()); KisStrokeJobFailureContext failure(true);
    painter.setStrokeMutationOwner(&owner);
    KoCompositeOp::ParameterInfo params; params.opacity = 0.63;
    for (int i = 0; i < 2; ++i) {
        painter.bitBlt(rect.x(), rect.y(), source, rect.x(), rect.y(), rect.width(), rect.height());
        painter.renderMirrorMask(rect, source); QVERIFY(!failure.failed());
        oracle.bitBlt(rect.x(), rect.y(), source, rect.x(), rect.y(), rect.width(), rect.height());
        legacyMirroredBlits(reference, source, rect, center, mirrors, selection, params);
    }
    QVERIFY(owner.checkpointStrokeMutation());
    QByteArray pixels(window.width() * window.height() * 4, Qt::Uninitialized), expected(pixels.size(), Qt::Uninitialized);
    actual->readBytes(reinterpret_cast<quint8 *>(pixels.data()), window); reference->readBytes(reinterpret_cast<quint8 *>(expected.data()), window);
    QVERIFY(pixels != QByteArray(pixels.size(), char(0))); QCOMPARE(pixels, expected);
    KisSurrogateUndoAdapter undo; QVERIFY(owner.tryCommit(&undo)); undo.undo(); actual->readBytes(reinterpret_cast<quint8 *>(pixels.data()), window); QCOMPARE(pixels, QByteArray(pixels.size(), char(0)));
    undo.redo(); actual->readBytes(reinterpret_cast<quint8 *>(pixels.data()), window); QCOMPARE(pixels, expected);
}

void KisPainterTest::testFixedCursorOwnerRejection()
{
    const auto *cs = KoColorSpaceRegistry::instance()->alpha8(); KisPaintDeviceSP target = new KisPaintDevice(cs), other = new KisPaintDevice(cs);
    KisFixedPaintDeviceSP fixed = new KisFixedPaintDevice(cs); fixed->setRect(QRect(0, 0, 65, 1)); QVERIFY(fixed->initialize(71));
    const quint8 input = 71; target->fill(QRect(0, 0, 65, 1), KoColor(&input, cs));
    KisTransaction owner(target); QVERIFY(owner.beginStrokeMutation()); KisPainter painter(target), wrong(other);
    painter.setStrokeMutationOwner(&owner); wrong.setStrokeMutationOwner(&owner);
    for (int kind = 0; kind < 4; ++kind) {
        KisStrokeJobFailureContext failure(true); KisPageStoreDiagnosticRecorder recorder(true);
        if (kind == 0) wrong.bltFixed(0, 0, fixed, 0, 0, 65, 1);
        if (kind == 1) wrong.bitBlt(0, 0, target, 0, 0, 65, 1);
        if (kind == 2) painter.bitBlt(0, 0, target, 0, 0, 65, 1);
        if (kind == 3) painter.renderMirrorMask(QRect(0, 0, 65, 1), target);
        QVERIFY2(failure.failed(), qPrintable(QString::number(kind))); QCOMPARE(recorder.metrics()[size_t(KisPageStoreDiagnosticPhase::PixelOperationBody)].intervals, quint64(0));
    }
    QVERIFY(owner.tryRevert());
    // The source view is not represented by shifting the entire fixed device
    // outside qint32. A valid destination at the coordinate edge still works.
    const int edge = std::numeric_limits<int>::min() + 4;
    other->moveTo(QPoint(edge, 0)); // keep manager-local tiles inside the compatibility index range
    KisTransaction edgeOwner(other); QVERIFY(edgeOwner.beginStrokeMutation());
    wrong.setStrokeMutationOwner(&edgeOwner);
    {
        KisStrokeJobFailureContext failure(true);
        wrong.bltFixed(edge, 0, fixed, 64, 0, 1, 1); QVERIFY(!failure.failed());
    }
    QVERIFY(edgeOwner.checkpointStrokeMutation()); KoColor pixel(cs); other->pixel(edge, 0, &pixel); QCOMPARE(*pixel.data(), input);
    QVERIFY(edgeOwner.tryRevert());
}

void KisPainterTest::testCompositeOverTransparentLanes_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("masked"); QTest::addColumn<bool>("opacity");
    QTest::addColumn<bool>("mixedDestination"); QTest::addColumn<int>("alignment");
    for (int bpp : {4, 8, 16}) for (bool masked : {false, true}) for (bool opacity : {false, true})
        for (bool mixedDestination : {false, true}) for (int alignment : {0, 1, 3})
            QTest::newRow(qPrintable(QString("B%1-mask%2-opacity%3-mixed%4-offset%5").arg(bpp).arg(masked).arg(opacity).arg(mixedDestination).arg(alignment)))
                << bpp << masked << opacity << mixedDestination << alignment;
}

void KisPainterTest::testCompositeOverTransparentLanes()
{
    QFETCH(int, bpp); QFETCH(bool, masked); QFETCH(bool, opacity); QFETCH(bool, mixedDestination); QFETCH(int, alignment);
    const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", bpp == 4 ? "U8" : bpp == 8 ? "U16" : "F32", "");
    QVERIFY(cs);
    constexpr int width = 97, rows = 2;
    const int srcStride = (width + 7) * bpp, dstStride = (width + 5) * bpp;
    QByteArray src(srcStride * rows + 16 * bpp, char(0x53));
    QByteArray dst(dstStride * rows + 16 * bpp, char(0x39));
    QByteArray mask(width * rows, char(255));
    auto *sp = reinterpret_cast<quint8 *>(src.data()) + (3 - alignment) * bpp;
    auto *dp = reinterpret_cast<quint8 *>(dst.data()) + alignment * bpp;
    for (int y = 0; y < rows; ++y) for (int x = 0; x < width; ++x) {
        const KoColor source(QColor(193, 113, 71, x % 3 ? 179 : 0), cs);
        const KoColor destination(QColor(29, 59, 89, mixedDestination && x % 5 ? 113 : 0), cs);
        std::memcpy(sp + y * srcStride + x * bpp, source.data(), size_t(bpp));
        std::memcpy(dp + y * dstStride + x * bpp, destination.data(), size_t(bpp));
        if (x % 5 == 1) mask[y * width + x] = 0;
    }
    const QByteArray before(dst.constData(), dst.size());
    // The wide, mixed-alpha rows exercise SIMD, including its scalar alignment
    // prefix/tail. Transparent source/mask lanes must preserve every dst byte.
    KoCompositeOp::ParameterInfo params;
    params.dstRowStart = dp; params.dstRowStride = dstStride;
    params.srcRowStart = sp; params.srcRowStride = srcStride;
    params.maskRowStart = masked ? reinterpret_cast<const quint8 *>(mask.constData()) : nullptr;
    params.maskRowStride = masked ? width : 0; params.rows = rows; params.cols = width;
    params.setOpacityAndAverage(opacity ? 0.63 : 1.0, 1.0); params.flow = 1.0;
    cs->bitBlt(cs, params, cs->compositeOp(COMPOSITE_OVER), KoColorConversionTransformation::internalRenderingIntent(), KoColorConversionTransformation::internalConversionFlags());
    QByteArray transparentOnly = dst;
    for (int y = 0; y < rows; ++y) for (int x = 0; x < width; ++x) {
        const int index = alignment * bpp + y * dstStride + x * bpp;
        if (x % 3 == 0 || (masked && x % 5 == 1)) {
            QCOMPARE(dst.mid(index, bpp), before.mid(index, bpp));
        } else {
            QVERIFY(dst.mid(index, bpp) != before.mid(index, bpp));
            // Restore only the allowed-to-change pixels. This also verifies
            // that no padding, prefix or tail bytes were touched.
            std::memcpy(transparentOnly.data() + index, before.constData() + index, size_t(bpp));
        }
    }
    QCOMPARE(transparentOnly, before);
}

void KisPainterTest::testMappedWritePartition_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("wrapMode"); QTest::addColumn<int>("mirrorMode");
    for (int bpp : {1, 4, 8, 16}) for (int wrapMode : {0, 1, 2, 3}) for (int mirrorMode : {0, 1, 2, 3})
        QTest::newRow(qPrintable(QString("B%1-wrap%2-mirror%3").arg(bpp).arg(wrapMode).arg(mirrorMode)))
            << bpp << wrapMode << mirrorMode;
}

void KisPainterTest::testMappedWritePartition()
{
    QFETCH(int, bpp); QFETCH(int, wrapMode); QFETCH(int, mirrorMode);
    const auto registry = KoColorSpaceRegistry::instance();
    const auto *cs = bpp == 1 ? registry->alpha8() : registry->colorSpace("RGBA", bpp == 4 ? "U8" : bpp == 8 ? "U16" : "F32", "");
    QVERIFY(cs);
    KisPaintDeviceSP dev = new KisPaintDevice(cs);
    const QPoint offset(13, -11); dev->moveTo(offset);
    const auto axis = wrapMode == 1 ? WRAPAROUND_HORIZONTAL : wrapMode == 2 ? WRAPAROUND_VERTICAL : WRAPAROUND_BOTH;
    if (wrapMode) { dev->setDefaultBounds(new PainterFootprintBounds(QRect(0, 0, 257, 193), axis)); dev->setSupportsWraparoundMode(true); }
    const KoColor initial(QColor(31, 57, 79, 113), cs); dev->setDefaultPixel(initial);
    KisPainter painter(dev); painter.setMirrorInformation(QPointF(-17, 23), true, true);
    QList<KisRenderedDab> dabs;
    for (int i = 0; i < 2; ++i) {
        KisFixedPaintDeviceSP fixed = new KisFixedPaintDevice(cs);
        fixed->setRect(QRect(0, 0, 180 - 10 * i, 100 + 30 * i)); fixed->initialize();
        fixed->fill(fixed->bounds(), KoColor(i ? QColor(151, 29, 199, 137) : QColor(71, 179, 41, 191), cs));
        KisRenderedDab dab(fixed); dab.offset = i ? QPoint(69, 33) : QPoint(7, 17);
        dab.opacity = i ? 0.63 : 0.79; dab.averageOpacity = i ? 0.41 : 0.87; dab.flow = i ? 0.81 : 0.71;
        if (mirrorMode & 1) painter.mirrorDab(Qt::Horizontal, &dab);
        if (mirrorMode & 2) painter.mirrorDab(Qt::Vertical, &dab);
        dabs.append(dab);
    }
    const QRegion original = QRegion(QRect(15, 21, 215, 137)) - QRegion(QRect(90, 60, 30, 20));
    QVector<QRect> input;
    for (QRect rc : original) {
        if (mirrorMode & 1) painter.mirrorRect(Qt::Horizontal, &rc);
        if (mirrorMode & 2) painter.mirrorRect(Qt::Vertical, &rc);
        input.append(rc);
    }
    input.append(input.first()); // repeated coverage is not a repeated paint
    QRegion coverage; for (const QRect &rc : input) coverage += rc;
    const auto mod = [](int value, int period) { const int r = value % period; return r < 0 ? r + period : r; };
    const auto map = [&](QPoint p) {
        return QPoint((wrapMode && wrapMode != 2 ? mod(p.x(), 257) : p.x()) - offset.x(),
                      (wrapMode && wrapMode != 1 ? mod(p.y(), 193) : p.y()) - offset.y());
    };
    const auto page = [](int value) { return int(value >= 0 ? qint64(value) / 64 : -((-qint64(value) + 63) / 64)); };
    QVector<QVector<QRect>> jobs;
    for (int patchSize : {128, 320}) {
        QVERIFY(dev->partitionWriteRects(input, patchSize, &jobs)); QVERIFY(!jobs.isEmpty());
        QRegion returned; QHash<QPair<int, int>, int> pageOwners;
        for (int i = 0; i < jobs.size(); ++i) for (const QRect &rc : jobs[i]) {
            QVERIFY((returned & QRegion(rc)).isEmpty()); returned += rc;
            for (int y = rc.top(); y <= rc.bottom(); ++y) for (int x = rc.left(); x <= rc.right(); ++x) {
                const QPoint p = map({x, y}); const QPair<int, int> key(page(p.x()), page(p.y()));
                auto it = pageOwners.find(key);
                if (it == pageOwners.end()) pageOwners.insert(key, i); else QCOMPARE(*it, i);
            }
        }
        QCOMPARE(returned, coverage);
    }
    QVERIFY(dev->partitionWriteRects(input, 128, &jobs)); QVERIFY(jobs.size() > 1);
    QRect storage;
    for (const QRect &rc : coverage) for (int y = rc.top(); y <= rc.bottom(); ++y) for (int x = rc.left(); x <= rc.right(); ++x)
        storage |= QRect(map({x, y}), QSize(1, 1));
    storage.adjust(-2, -2, 2, 2);
    QByteArray before(storage.width() * storage.height() * bpp, Qt::Uninitialized);
    for (int i = 0; i < before.size(); i += bpp) std::memcpy(before.data() + i, initial.data(), size_t(bpp));
    QByteArray expected = before;
    // Independent per-pixel mapping and ordered, non-commutative compositing.
    for (const auto &dab : dabs) {
        const QRect rc = dab.realBounds();
        for (int y = rc.top(); y <= rc.bottom(); ++y) for (int x = rc.left(); x <= rc.right(); ++x) {
            if (!coverage.contains(QPoint(x, y))) continue;
            const QPoint p = map({x, y});
            KoCompositeOp::ParameterInfo params;
            params.dstRowStart = reinterpret_cast<quint8 *>(expected.data()) + ((p.y() - storage.y()) * storage.width() + p.x() - storage.x()) * bpp;
            params.dstRowStride = bpp;
            params.srcRowStart = dab.device->constData() + ((y - rc.y()) * rc.width() + x - rc.x()) * bpp;
            params.srcRowStride = bpp; params.maskRowStart = nullptr; params.maskRowStride = 0; params.rows = 1; params.cols = 1;
            params.setOpacityAndAverage(dab.opacity, dab.averageOpacity); params.flow = dab.flow;
            cs->bitBlt(cs, params, cs->compositeOp(COMPOSITE_OVER), KoColorConversionTransformation::internalRenderingIntent(), KoColorConversionTransformation::internalConversionFlags());
        }
    }
    KisTransaction owner(dev); QVERIFY(owner.beginStrokeMutation());
    painter.setStrokeMutationOwner(&owner);
    KisStrokeJobFailureContext failure(true);
    // Deliberately reverse independent jobs. Pixels within a job retain dab order.
    for (int i = jobs.size() - 1; i >= 0; --i) painter.bltFixed(jobs[i], dabs);
    QVERIFY(!failure.failed()); QVERIFY(owner.checkpointStrokeMutation());
    QByteArray actual(before.size(), Qt::Uninitialized);
    const auto read = [&] { dev->dataManager()->readBytes(reinterpret_cast<quint8 *>(actual.data()), storage.x(), storage.y(), storage.width(), storage.height()); };
    const auto agrees = [&](const QByteArray &a, const QByteArray &b) {
        if (bpp != 16) return a == b;
        const auto *ap = reinterpret_cast<const float *>(a.constData()); const auto *bp = reinterpret_cast<const float *>(b.constData());
        for (int i = 0; i < a.size() / int(sizeof(float)); ++i) if (!std::isfinite(ap[i]) || std::abs(ap[i] - bp[i]) > 0.000005f) return false;
        return true;
    };
    read(); QVERIFY(agrees(actual, expected));
    KisSurrogateUndoAdapter undo; QVERIFY(owner.tryCommit(&undo));
    undo.undo(); read(); QVERIFY(agrees(actual, before));
    undo.redo(); read(); QVERIFY(agrees(actual, expected));
}

void KisPainterTest::testMappedWritePartitionParallel_data()
{
    QTest::addColumn<bool>("wrapped"); QTest::addColumn<bool>("mirrored");
    for (bool wrapped : {false, true}) for (bool mirrored : {false, true})
        QTest::newRow(qPrintable(QString("wrap%1-mirror%2").arg(wrapped).arg(mirrored))) << wrapped << mirrored;
}

void KisPainterTest::testMappedWritePartitionParallel()
{
    QFETCH(bool, wrapped); QFETCH(bool, mirrored);
    const auto *cs = KoColorSpaceRegistry::instance()->alpha8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs); const QPoint offset(13, 11); dev->moveTo(offset);
    if (wrapped) { dev->setDefaultBounds(new PainterFootprintBounds(QRect(0, 0, 257, 193), WRAPAROUND_BOTH)); dev->setSupportsWraparoundMode(true); }
    KisPainter painter(dev); painter.setCompositeOpId(COMPOSITE_COPY); painter.setMirrorInformation(QPointF(-17, 23), true, true);
    KisFixedPaintDeviceSP fixed = new KisFixedPaintDevice(cs); fixed->setRect(QRect(0, 0, 240, 160)); fixed->initialize();
    const quint8 value = 71; fixed->fill(fixed->bounds(), KoColor(&value, cs));
    KisRenderedDab dab(fixed); dab.offset = QPoint(3, 5); QRect rect = dab.realBounds();
    if (mirrored) { painter.mirrorDab(Qt::Horizontal, &dab); painter.mirrorDab(Qt::Vertical, &dab); painter.mirrorRect(Qt::Horizontal, &rect); painter.mirrorRect(Qt::Vertical, &rect); }
    QList<KisRenderedDab> dabs{dab}; QVector<QVector<QRect>> jobs;
    QVERIFY(dev->partitionWriteRects({rect}, 128, &jobs)); QVERIFY(jobs.size() > 1);
    KisTransaction owner(dev); QVERIFY(owner.beginStrokeMutation());
    painter.setStrokeMutationOwner(&owner);
    QSemaphore entered, release; std::vector<std::thread> workers; std::vector<int> success(jobs.size());
    const auto cleanup = qScopeGuard([&] { release.release(jobs.size()); for (auto &worker : workers) if (worker.joinable()) worker.join(); });
    for (int i = 0; i < jobs.size(); ++i) workers.emplace_back([&, i] {
        KisStrokeJobFailureContext failure(true); KisPageStoreDiagnosticRecorder recorder(true);
        recorder.setPhaseObserver([&](auto phase) { if (phase == KisPageStoreDiagnosticPhase::PixelOperationBody) { entered.release(); release.acquire(); } });
        painter.bltFixed(std::as_const(jobs)[i], dabs); success[i] = !failure.failed();
    });
    QVERIFY(entered.tryAcquire(jobs.size(), 5000)); QVERIFY(!owner.checkpointStrokeMutation());
    release.release(jobs.size()); for (auto &worker : workers) worker.join();
    for (int ok : success) QVERIFY(ok);
    QVERIFY(owner.checkpointStrokeMutation());
    QByteArray actual(rect.width() * rect.height(), Qt::Uninitialized);
    dev->readBytes(reinterpret_cast<quint8 *>(actual.data()), rect); QCOMPARE(actual, QByteArray(actual.size(), char(value)));
    QVERIFY(owner.tryRevert()); dev->readBytes(reinterpret_cast<quint8 *>(actual.data()), rect); QCOMPARE(actual, QByteArray(actual.size(), char(0)));
}

void KisPainterTest::testMappedWritePartitionRejection()
{
    KisPaintDeviceSP dev = new KisPaintDevice(KoColorSpaceRegistry::instance()->alpha8());
    QVector<QVector<QRect>> jobs{{QRect(1, 2, 3, 4)}}; const auto before = jobs;
    for (int patchSize : {-64, 0, 63, 127}) { QVERIFY(!dev->partitionWriteRects({QRect(0, 0, 10, 10)}, patchSize, &jobs)); QCOMPARE(jobs, before); }
    QVERIFY(!dev->partitionWriteRects({}, 128, nullptr));
    dev->moveTo(QPoint(13, 11));
    QVERIFY(!dev->partitionWriteRects({QRect(std::numeric_limits<int>::min(), 0, 10, 10)}, 128, &jobs)); QCOMPARE(jobs, before);
    QVERIFY(dev->partitionWriteRects({}, 128, &jobs)); QVERIFY(jobs.isEmpty());
    // Counterfactual: two old world-aligned jobs share the local page at x=128.
    int patchSize = 0;
    const auto old = KisPaintOpUtils::splitDabsIntoRects({QRect(0, 0, 256, 64)}, 2, 128, 1.0, &patchSize);
    QCOMPARE(patchSize, 128); QCOMPARE(old.size(), 2);
    QVERIFY(dev->partitionWriteRects(old, patchSize, &jobs)); QCOMPARE(jobs.size(), 6);
    const int oldLeftPage = (old[0].right() - dev->x()) / 64;
    const int oldRightPage = (old[1].left() - dev->x()) / 64;
    QCOMPARE(oldLeftPage, oldRightPage);
}

void KisPainterTest::testMultiDabSparseFootprint_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("wrapMode");
    QTest::addColumn<bool>("mirrored"); QTest::addColumn<bool>("persistent");
    for (int bpp : {1, 4, 8, 16}) for (int wrapMode : {0, 1, 2, 3})
        for (bool mirrored : {false, true}) for (bool persistent : {false, true})
            QTest::newRow(qPrintable(QString("B%1-wrap%2-mirror%3-owner%4").arg(bpp).arg(wrapMode).arg(mirrored).arg(persistent)))
                << bpp << wrapMode << mirrored << persistent;
}

void KisPainterTest::testMultiDabSparseFootprint()
{
    QFETCH(int, bpp); QFETCH(int, wrapMode); QFETCH(bool, mirrored); QFETCH(bool, persistent);
    const auto registry = KoColorSpaceRegistry::instance();
    const auto *cs = bpp == 1 ? registry->alpha8() : registry->colorSpace("RGBA", bpp == 4 ? "U8" : bpp == 8 ? "U16" : "F32", "");
    QVERIFY(cs);
    KisPaintDeviceSP dev = new KisPaintDevice(cs);
    const QPoint offset(-13, 11); dev->moveTo(offset);
    const auto axis = wrapMode == 1 ? WRAPAROUND_HORIZONTAL : wrapMode == 2 ? WRAPAROUND_VERTICAL : WRAPAROUND_BOTH;
    if (wrapMode) { dev->setDefaultBounds(new PainterFootprintBounds(QRect(0, 0, 128, 96), axis)); dev->setSupportsWraparoundMode(true); }
    const KoColor initial(QColor(33, 55, 77, 13), cs); dev->setDefaultPixel(initial);
    KisPainter painter(dev); painter.setCompositeOpId(COMPOSITE_COPY);
    painter.setMirrorInformation(QPointF(-17, 23), true, true);
    QList<KisRenderedDab> dabs;
    QRect apply;
    for (int i = 0; i < 2; ++i) {
        KisFixedPaintDeviceSP fixed = new KisFixedPaintDevice(cs);
        fixed->setRect(QRect(0, 0, 5 + 2 * i, 4 + i)); fixed->initialize();
        const KoColor color(QColor(90 + 71 * i, 33, 170 - 30 * i, bpp == 1 ? 71 + 66 * i : 255), cs);
        fixed->fill(fixed->bounds(), color);
        KisRenderedDab dab(fixed); dab.offset = i ? QPoint(270, -170) : QPoint(-50, 83);
        apply |= dab.realBounds(); dabs.append(dab);
    }
    apply.adjust(1, 1, -1, -1);
    if (mirrored) {
        for (auto &dab : dabs) { painter.mirrorDab(Qt::Horizontal, &dab); painter.mirrorDab(Qt::Vertical, &dab); }
        painter.mirrorRect(Qt::Horizontal, &apply); painter.mirrorRect(Qt::Vertical, &apply);
    }
    dabs.append(dabs.first()); // repeated declaration must not enlarge admission
    const auto mod = [](int value, int period) { const int r = value % period; return r < 0 ? r + period : r; };
    const auto page = [](int value) { return int(value >= 0 ? qint64(value) / 64 : -((-qint64(value) + 63) / 64)); };
    QVector<QPair<QPoint, QByteArray>> writes;
    QSet<QPair<int, int>> pages;
    QRect window;
    for (const auto &dab : std::as_const(dabs)) {
        const QRect rc = apply & dab.realBounds();
        for (int y = rc.top(); y <= rc.bottom(); ++y) for (int x = rc.left(); x <= rc.right(); ++x) {
            const int px = (wrapMode && wrapMode != 2 ? mod(x, 128) : x) - offset.x();
            const int py = (wrapMode && wrapMode != 1 ? mod(y, 96) : y) - offset.y();
            const QPoint point(px, py);
            const int src = ((y - dab.realBounds().y()) * dab.realBounds().width() + x - dab.realBounds().x()) * bpp;
            writes.append({point, QByteArray(reinterpret_cast<const char *>(dab.device->constData() + src), bpp)});
            pages.insert({page(px), page(py)}); window |= QRect(point, QSize(1, 1));
        }
    }
    QVERIFY(!pages.isEmpty()); window.adjust(-2, -2, 2, 2);
    QByteArray before(window.width() * window.height() * bpp, Qt::Uninitialized);
    for (int i = 0; i < before.size(); i += bpp) std::memcpy(before.data() + i, initial.data(), size_t(bpp));
    QByteArray expected = before;
    for (const auto &write : std::as_const(writes)) {
        const int index = ((write.first.y() - window.y()) * window.width() + write.first.x() - window.x()) * bpp;
        std::memcpy(expected.data() + index, write.second.constData(), size_t(bpp));
    }
    // Positive control: the former enclosing-rectangle declaration claims
    // more pages for this same input, even when its callback writes nothing.
    {
        KisPageStoreDiagnosticRecorder recorder(true); int calls = 0;
        QVERIFY(dev->applyPixelOperation(apply, [&](KisPixelWriteCursor *) { ++calls; return true; }));
        QCOMPARE(calls, 1);
        const auto &range = recorder.metrics()[size_t(KisPageStoreDiagnosticPhase::PixelOperationRangePrepare)];
        QCOMPARE(range.intervals, quint64(1)); QVERIFY(range.workItems > quint64(pages.size()));
    }
    KisTransaction owner(dev); if (persistent) QVERIFY(owner.beginStrokeMutation());
    painter.setStrokeMutationOwner(persistent ? &owner : nullptr);
    {
        KisStrokeJobFailureContext failure(true); KisPageStoreDiagnosticRecorder recorder(true);
        painter.bltFixed(apply, dabs);
        QVERIFY(!failure.failed());
        const auto &range = recorder.metrics()[size_t(KisPageStoreDiagnosticPhase::PixelOperationRangePrepare)];
        QCOMPARE(range.intervals, quint64(1)); QCOMPARE(range.workItems, quint64(pages.size()));
    }
    if (persistent) QVERIFY(owner.checkpointStrokeMutation());
    QByteArray actual(before.size(), Qt::Uninitialized);
    const auto read = [&] { dev->dataManager()->readBytes(reinterpret_cast<quint8 *>(actual.data()), window.x(), window.y(), window.width(), window.height()); };
    read(); QCOMPARE(actual, expected);
    KisSurrogateUndoAdapter adapter; QVERIFY(owner.tryCommit(&adapter));
    adapter.undo(); read(); QCOMPARE(actual, before);
    adapter.redo(); read(); QCOMPARE(actual, expected);
}

void KisPainterTest::testMultiDabSparseConcurrent_data()
{
    QTest::addColumn<bool>("wrapped"); QTest::addColumn<bool>("mirrored");
    for (bool wrapped : {false, true}) for (bool mirrored : {false, true})
        QTest::newRow(qPrintable(QString("wrap%1-mirror%2").arg(wrapped).arg(mirrored))) << wrapped << mirrored;
}

void KisPainterTest::testMultiDabSparseConcurrent()
{
    QFETCH(bool, wrapped); QFETCH(bool, mirrored);
    const auto *cs = KoColorSpaceRegistry::instance()->alpha8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs); const QPoint offset(13, 11); dev->moveTo(offset);
    if (wrapped) { dev->setDefaultBounds(new PainterFootprintBounds(QRect(0, 0, 512, 128), WRAPAROUND_BOTH)); dev->setSupportsWraparoundMode(true); }
    KisPainter painter(dev); painter.setCompositeOpId(COMPOSITE_COPY); painter.setMirrorInformation(QPointF(), true, false);
    QList<KisRenderedDab> dabs[2]; QRect rects[2];
    for (int i = 0; i < 2; ++i) for (int x : {i * 64, i * 64 + 192}) {
        KisFixedPaintDeviceSP fixed = new KisFixedPaintDevice(cs); fixed->setRect(QRect(0, 0, 1, 1)); fixed->initialize();
        const quint8 value = 71 + 10 * i; fixed->fill(fixed->bounds(), KoColor(&value, cs));
        KisRenderedDab dab(fixed); dab.offset = offset + QPoint(x, 0);
        if (mirrored) painter.mirrorDab(Qt::Horizontal, &dab);
        rects[i] |= dab.realBounds(); dabs[i].append(dab);
    }
    QVERIFY(rects[0].intersects(rects[1])); // enclosing boxes conflict; the actual pages do not
    KisTransaction owner(dev); QVERIFY(owner.beginStrokeMutation());
    painter.setStrokeMutationOwner(&owner);
    QSemaphore entered, release; std::thread workers[2]; bool success[2] = {false, false}; quint64 pageCounts[2] = {};
    const auto cleanup = qScopeGuard([&] { release.release(2); for (auto &w : workers) if (w.joinable()) w.join(); });
    for (int i = 0; i < 2; ++i) workers[i] = std::thread([&, i] {
        KisStrokeJobFailureContext failure(true); KisPageStoreDiagnosticRecorder recorder(true);
        recorder.setPhaseObserver([&](auto phase) { if (phase == KisPageStoreDiagnosticPhase::PixelOperationBody) { entered.release(); release.acquire(); } });
        painter.bltFixed(rects[i], dabs[i]);
        success[i] = !failure.failed();
        pageCounts[i] = recorder.metrics()[size_t(KisPageStoreDiagnosticPhase::PixelOperationRangePrepare)].workItems;
    });
    QVERIFY(entered.tryAcquire(2, 5000)); QVERIFY(!owner.checkpointStrokeMutation());
    release.release(2); for (auto &w : workers) w.join();
    QVERIFY(success[0] && success[1]); QCOMPARE(pageCounts[0], quint64(2)); QCOMPARE(pageCounts[1], quint64(2));
    QVERIFY(owner.checkpointStrokeMutation());
    for (int i = 0; i < 2; ++i) for (const auto &dab : std::as_const(dabs[i])) {
        quint8 actual = 0; dev->readBytes(&actual, dab.offset.x(), dab.offset.y(), 1, 1); QCOMPARE(actual, quint8(71 + 10 * i));
    }
    QVERIFY(owner.tryRevert());
    for (const auto &queue : dabs) for (const auto &dab : queue) {
        quint8 actual = 255; dev->readBytes(&actual, dab.offset.x(), dab.offset.y(), 1, 1); QCOMPARE(actual, quint8(0));
    }
}

void KisPainterTest::testSparsePixelOperationBounds_data()
{
    QTest::addColumn<bool>("wrapped"); QTest::newRow("plain") << false; QTest::newRow("wrapped") << true;
}

void KisPainterTest::testSparsePixelOperationBounds()
{
    QFETCH(bool, wrapped);
    const auto *cs = KoColorSpaceRegistry::instance()->alpha8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs); const QPoint offset(13, 11); dev->moveTo(offset);
    if (wrapped) { dev->setDefaultBounds(new PainterFootprintBounds(QRect(0, 0, 512, 128), WRAPAROUND_BOTH)); dev->setSupportsWraparoundMode(true); }
    const quint8 initial = 31; dev->setDefaultPixel(KoColor(&initial, cs));
    KisTransaction owner(dev); QVERIFY(owner.beginStrokeMutation()); int calls = 0; bool firstWritten = false, gapRejected = false;
    const QVector<QRect> rects{QRect(offset, QSize(1, 1)), QRect(offset + QPoint(128, 0), QSize(1, 1)), QRect()};
    const bool result = dev->applyPixelOperation(rects, [&](KisPixelWriteCursor *cursor) {
        ++calls; cursor->moveTo(offset.x(), offset.y());
        if (auto *data = cursor->rawData()) { *data = 71; firstWritten = true; }
        cursor->moveTo(offset.x() + 64, offset.y()); gapRejected = !cursor->rawData();
        return true; // a failed cursor cannot be hidden by the callback's result
    }, &owner);
    QVERIFY(!result); QCOMPARE(calls, 1); QVERIFY(firstWritten && gapRejected);
    QVERIFY(!owner.tryEnd()); QVERIFY(owner.tryRevert());
    QByteArray actual(129, 0); dev->dataManager()->readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 129, 1);
    QCOMPARE(actual, QByteArray(129, char(initial)));
}

void KisPainterTest::testMultiDabOperation_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("mode");
    QTest::addColumn<bool>("masked"); QTest::addColumn<bool>("wrapped");
    for (int bpp : {1, 4, 8, 16}) for (int mode : {0, 1, 2})
        for (bool masked : {false, true}) for (bool wrapped : {false, true})
            QTest::newRow(qPrintable(QString("B%1-mode%2-mask%3-wrap%4").arg(bpp).arg(mode).arg(masked).arg(wrapped)))
                << bpp << mode << masked << wrapped;
}

void KisPainterTest::testMultiDabOperation()
{
    QFETCH(int, bpp); QFETCH(int, mode); QFETCH(bool, masked); QFETCH(bool, wrapped);
    const auto registry = KoColorSpaceRegistry::instance();
    const auto *cs = bpp == 1 ? registry->alpha8() : registry->colorSpace("RGBA", bpp == 4 ? "U8" : bpp == 8 ? "U16" : "F32", "");
    QVERIFY(cs);
    KisPaintDeviceSP dev = new KisPaintDevice(cs);
    const QPoint offset(13, -11);
    const QRect storage(-64, -64, 320, 256), target(97, 77, 73, 47);
    dev->moveTo(offset);
    const KoColor initial(QColor(21, 47, 79, 113), cs);
    dev->fill(storage.translated(offset), initial);
    QByteArray before(storage.width() * storage.height() * bpp, Qt::Uninitialized);
    dev->dataManager()->readBytes(reinterpret_cast<quint8 *>(before.data()), storage.x(), storage.y(), storage.width(), storage.height());
    QByteArray expected = before;
    if (wrapped) { dev->setDefaultBounds(new PainterWrapBounds); dev->setSupportsWraparoundMode(true); }
    KisSelectionSP selection;
    if (masked) {
        selection = new KisSelection();
        QByteArray values(target.width() * target.height(), Qt::Uninitialized);
        for (int y = 0; y < target.height(); ++y) for (int x = 0; x < target.width(); ++x)
            values[y * target.width() + x] = char((x * 13 + y * 17) % 256);
        selection->pixelSelection()->writeBytes(reinterpret_cast<const quint8 *>(values.constData()), target);
    }
    QList<KisRenderedDab> dabs;
    for (int i = 0; i < 2; ++i) {
        KisFixedPaintDeviceSP fixed = new KisFixedPaintDevice(cs);
        const QRect bounds(-7 + i * 3, 9, 45 + i * 8, 35 - i * 8);
        fixed->setRect(bounds); fixed->initialize();
        fixed->fill(bounds, KoColor(i ? QColor(137, 63, 201, 179) : QColor(203, 145, 29, 211), cs));
        KisRenderedDab dab(fixed); dab.offset = target.topLeft() + QPoint(i ? 21 : -4, i ? 11 : 3);
        dab.opacity = i ? 0.47 : 0.63; dab.averageOpacity = i ? 0.81 : 0.37; dab.flow = i ? 0.91 : 0.76;
        dabs.append(dab);
    }
    QBitArray channels;
    if (bpp != 1) { channels = QBitArray(cs->channelCount(), true); channels.setBit(0, false); }
    // Independent scalar oracle: compose the same ordered immutable inputs
    // into the expected physical coordinates, without any writer/iterator API.
    const auto reference = [&] {
        for (const auto &dab : dabs) {
            const auto rect = target & dab.realBounds();
            for (int y = rect.top(); y <= rect.bottom(); ++y) for (int x = rect.left(); x <= rect.right(); ++x) {
                const int px = (wrapped ? x % 128 : x) - offset.x();
                const int py = (wrapped ? y % 96 : y) - offset.y();
                KoCompositeOp::ParameterInfo params;
                params.dstRowStart = reinterpret_cast<quint8 *>(expected.data()) + ((py - storage.y()) * storage.width() + px - storage.x()) * bpp;
                params.dstRowStride = bpp;
                params.srcRowStart = dab.device->constData() + ((y - dab.offset.y()) * dab.device->bounds().width() + x - dab.offset.x()) * bpp;
                params.srcRowStride = bpp;
                const quint8 mask = quint8(((x - target.x()) * 13 + (y - target.y()) * 17) % 256);
                params.maskRowStart = masked ? &mask : nullptr; params.maskRowStride = masked ? 1 : 0;
                params.rows = 1; params.cols = 1; params.channelFlags = channels;
                params.setOpacityAndAverage(dab.opacity, dab.averageOpacity); params.flow = dab.flow;
                cs->bitBlt(cs, params, cs->compositeOp(COMPOSITE_OVER), KoColorConversionTransformation::internalRenderingIntent(), KoColorConversionTransformation::internalConversionFlags());
            }
        }
    };
    KisTransaction owner(dev); if (mode) QVERIFY(owner.beginStrokeMutation());
    KisPainter painter(dev, selection); painter.setChannelFlags(channels);
    painter.setStrokeMutationOwner(mode ? &owner : nullptr);
    KisStrokeJobFailureContext failure(true);
    for (int i = 0; i < 2; ++i) { painter.bltFixed(target, dabs); reference(); }
    QVERIFY(!failure.failed());
    if (mode) QVERIFY(owner.checkpointStrokeMutation());
    const QByteArray frozenExpected = expected;
    auto frozen = dev->createRandomConstAccessorNG();
    painter.bltFixed(target, dabs); reference();
    QVERIFY(!failure.failed());
    if (mode) QVERIFY(owner.checkpointStrokeMutation());
    const auto agrees = [&](const QByteArray &actual, const QByteArray &wanted) {
        if (bpp != 16) return actual == wanted;
        const auto *a = reinterpret_cast<const float *>(actual.constData());
        const auto *b = reinterpret_cast<const float *>(wanted.constData());
        for (int i = 0; i < actual.size() / int(sizeof(float)); ++i)
            if (!std::isfinite(a[i]) || std::abs(a[i] - b[i]) > 0.000005f) return false;
        return true;
    };
    QByteArray actual(expected.size(), Qt::Uninitialized);
    dev->dataManager()->readBytes(reinterpret_cast<quint8 *>(actual.data()), storage.x(), storage.y(), storage.width(), storage.height());
    QVERIFY(agrees(actual, expected));
    const QPoint point = target.topLeft() + QPoint(27, 17);
    frozen->moveTo(point.x(), point.y()); QVERIFY(frozen->rawDataConst());
    const int px = (wrapped ? point.x() % 128 : point.x()) - offset.x();
    const int py = (wrapped ? point.y() % 96 : point.y()) - offset.y();
    const auto oldPixel = frozenExpected.mid(((py - storage.y()) * storage.width() + px - storage.x()) * bpp, bpp);
    QVERIFY(agrees(QByteArray(reinterpret_cast<const char *>(frozen->rawDataConst()), bpp), oldPixel));
    frozen.clear();
    KisSurrogateUndoAdapter adapter;
    if (mode == 2) QVERIFY(owner.tryRevert()); else QVERIFY(owner.tryCommit(&adapter));
    dev->dataManager()->readBytes(reinterpret_cast<quint8 *>(actual.data()), storage.x(), storage.y(), storage.width(), storage.height());
    QVERIFY(agrees(actual, mode == 2 ? before : expected));
    if (mode != 2) {
        adapter.undo(); dev->dataManager()->readBytes(reinterpret_cast<quint8 *>(actual.data()), storage.x(), storage.y(), storage.width(), storage.height()); QVERIFY(agrees(actual, before));
        adapter.redo(); dev->dataManager()->readBytes(reinterpret_cast<quint8 *>(actual.data()), storage.x(), storage.y(), storage.width(), storage.height()); QVERIFY(agrees(actual, expected));
    }
}

void KisPainterTest::testMultiDabReuse_data()
{
    QTest::addColumn<int>("repeats"); for (int n : {1, 10, 1000}) QTest::newRow(qPrintable(QString::number(n))) << n;
}

void KisPainterTest::testMultiDabReuse()
{
    QFETCH(int, repeats);
    const auto *cs = KoColorSpaceRegistry::instance()->alpha8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs); const quint8 initial = 31, input = 71;
    dev->fill(QRect(0, 0, 128, 64), KoColor(&initial, cs));
    KisFixedPaintDeviceSP fixed = new KisFixedPaintDevice(cs); fixed->setRect(QRect(0, 0, 1, 1)); fixed->initialize(); fixed->fill(fixed->bounds(), KoColor(&input, cs));
    KisRenderedDab first(fixed), second(fixed); second.offset = {65, 0};
    KisTransaction owner(dev); QVERIFY(owner.beginStrokeMutation()); KisPainter painter(dev); painter.setCompositeOpId(COMPOSITE_COPY);
    painter.setStrokeMutationOwner(&owner);
    KisStrokeJobFailureContext failure(true); KisPageStoreDiagnosticRecorder recorder(true);
    const auto intervals = [&](KisPageStoreDiagnosticPhase phase) { return recorder.metrics()[size_t(phase)].intervals; };
    painter.bltFixed(QRect(0, 0, 66, 1), {first, second}); QVERIFY(!failure.failed());
    const auto preparations = intervals(KisPageStoreDiagnosticPhase::WriteProviderPrepare); QCOMPARE(preparations, quint64(2));
    for (int i = 1; i < repeats; ++i) painter.bltFixed(QRect(0, 0, 66, 1), {first, second});
    QVERIFY(!failure.failed());
    QCOMPARE(intervals(KisPageStoreDiagnosticPhase::PixelOperationBody), quint64(repeats));
    QCOMPARE(intervals(KisPageStoreDiagnosticPhase::MutationSealPrivatePublish), quint64(0));
    QCOMPARE(intervals(KisPageStoreDiagnosticPhase::WriteProviderPrepare), preparations);
    QVERIFY(owner.checkpointStrokeMutation());
    auto old = dev->createRandomConstAccessorNG();
    painter.bltFixed(QRect(0, 0, 66, 1), {first, second}); QVERIFY(!failure.failed());
    QCOMPARE(intervals(KisPageStoreDiagnosticPhase::WriteProviderPrepare), preparations * 2);
    old.clear(); QVERIFY(owner.tryRevert());
    quint8 actual = 0; dev->readBytes(&actual, 0, 0, 1, 1); QCOMPARE(actual, initial);
}

void KisPainterTest::testMultiDabConcurrent()
{
    const auto *cs = KoColorSpaceRegistry::instance()->alpha8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs); KisTransaction owner(dev); QVERIFY(owner.beginStrokeMutation());
    KisPainter painter(dev); painter.setCompositeOpId(COMPOSITE_COPY);
    painter.setStrokeMutationOwner(&owner);
    const quint8 input = 71; KisFixedPaintDeviceSP fixed = new KisFixedPaintDevice(cs);
    fixed->setRect(QRect(0, 0, 192, 1)); fixed->initialize(); fixed->fill(fixed->bounds(), KoColor(&input, cs));
    KisRenderedDab dab(fixed); QSemaphore entered, release; bool success[2] = {false, false};
    std::thread workers[2];
    const auto cleanup = qScopeGuard([&] { release.release(2); for (auto &worker : workers) if (worker.joinable()) worker.join(); });
    for (int i = 0; i < 2; ++i) workers[i] = std::thread([&, i] {
        KisStrokeJobFailureContext failure(true); KisPageStoreDiagnosticRecorder recorder(true);
        recorder.setPhaseObserver([&](auto phase) { if (phase == KisPageStoreDiagnosticPhase::PixelOperationBody) { entered.release(); release.acquire(); } });
        painter.bltFixed(QRect(i * 128, 0, 64, 1), {dab}); success[i] = !failure.failed();
    });
    QVERIFY(entered.tryAcquire(2, 5000)); QVERIFY(!owner.checkpointStrokeMutation());
    release.release(2); for (auto &worker : workers) worker.join(); QVERIFY(success[0] && success[1]);
    QVERIFY(owner.checkpointStrokeMutation());
    QByteArray actual(192, 0); dev->readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 192, 1);
    QCOMPARE(actual, QByteArray(64, char(input)) + QByteArray(64, char(0)) + QByteArray(64, char(input)));
    QVERIFY(owner.tryRevert());
}

void KisPainterTest::testFixedCursorCancellation()
{
    // A test-owned color space lets the actual composite kernel pause after
    // changing the first page, without installing a production-only test hook
    // or relying on a diagnostic phase that this write plan does not enter.
    const auto *alpha = static_cast<const KoAlphaColorSpace *>(KoColorSpaceRegistry::instance()->alpha8());
    std::unique_ptr<KoAlphaColorSpace> colorSpace(static_cast<KoAlphaColorSpace *>(alpha->clone()));
    const auto *cs = colorSpace.get();
    QSemaphore firstWrite, release;
    class PausingCopy final : public KoCompositeOp {
    public:
        PausingCopy(const KoColorSpace *cs, QSemaphore &entered, QSemaphore &resume)
            : KoCompositeOp(cs, QStringLiteral("test-pause-copy")),
              copy(cs->compositeOp(COMPOSITE_COPY)), entered(entered), resume(resume) {}
        void composite(const ParameterInfo &params) const override {
            copy->composite(params);
            if (++calls == 1) {
                firstPixel = *params.dstRowStart;
                entered.release();
                resume.acquire();
            }
        }
        mutable int calls = 0;
        mutable quint8 firstPixel = 0;
    private:
        const KoCompositeOp *copy;
        QSemaphore &entered;
        QSemaphore &resume;
    };
    auto *operation = new PausingCopy(cs, firstWrite, release);
    colorSpace->addCompositeOp(operation); // owned by this local color space
    KisPaintDeviceSP dev = new KisPaintDevice(cs);
    QCOMPARE(dev->colorSpace(), cs);
    const quint8 initial = 31, input = 71;
    dev->fill(QRect(0, 0, 128, 64), KoColor(&initial, cs));
    KisFixedPaintDeviceSP fixed = new KisFixedPaintDevice(cs);
    fixed->setRect(QRect(0, 0, 66, 1)); fixed->initialize();
    fixed->fill(fixed->bounds(), KoColor(&input, cs));
    KisRenderedDab first(fixed), second(fixed); second.offset = {65, 0};
    KisTransaction owner(dev); QVERIFY(owner.beginStrokeMutation());
    KisPainter painter(dev); painter.setCompositeOpId(operation->id());
    painter.setStrokeMutationOwner(&owner);
    bool failed = false;
    std::thread worker([&] {
        KisStrokeJobFailureContext failure(true);
        painter.bltFixed(0, 0, fixed, 0, 0, 66, 1);
        failed = failure.failed();
    });
    const auto cleanup = qScopeGuard([&] { release.release(); if (worker.joinable()) worker.join(); });
    QVERIFY(firstWrite.tryAcquire(1, 5000));
    QCOMPARE(operation->firstPixel, input);
    QVERIFY(!owner.tryRevert()); // cancellation is recorded; the live borrow still owns its guard
    release.release(); worker.join();
    QVERIFY(failed);
    QCOMPARE(operation->calls, 1); // second page must never enter the kernel after cancellation
    QVERIFY(owner.tryRevert());
    QByteArray actual(128, 0); dev->readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 128, 1);
    QCOMPARE(actual, QByteArray(128, char(initial)));
}

void KisPainterTest::testMultiDabCancellation()
{
    // A test-owned color space lets the actual composite kernel pause after
    // changing the first page, without installing a production-only test hook
    // or relying on a diagnostic phase that this write plan does not enter.
    const auto *alpha = static_cast<const KoAlphaColorSpace *>(KoColorSpaceRegistry::instance()->alpha8());
    std::unique_ptr<KoAlphaColorSpace> colorSpace(static_cast<KoAlphaColorSpace *>(alpha->clone()));
    const auto *cs = colorSpace.get();
    QSemaphore firstWrite, release;
    class PausingCopy final : public KoCompositeOp {
    public:
        PausingCopy(const KoColorSpace *cs, QSemaphore &entered, QSemaphore &resume)
            : KoCompositeOp(cs, QStringLiteral("test-pause-copy")),
              copy(cs->compositeOp(COMPOSITE_COPY)), entered(entered), resume(resume) {}
        void composite(const ParameterInfo &params) const override {
            copy->composite(params);
            if (++calls == 1) {
                firstPixel = *params.dstRowStart;
                entered.release();
                resume.acquire();
            }
        }
        mutable int calls = 0;
        mutable quint8 firstPixel = 0;
    private:
        const KoCompositeOp *copy;
        QSemaphore &entered;
        QSemaphore &resume;
    };
    auto *operation = new PausingCopy(cs, firstWrite, release);
    colorSpace->addCompositeOp(operation); // owned by this local color space
    KisPaintDeviceSP dev = new KisPaintDevice(cs);
    QCOMPARE(dev->colorSpace(), cs);
    const quint8 initial = 31, input = 71;
    dev->fill(QRect(0, 0, 128, 64), KoColor(&initial, cs));
    KisFixedPaintDeviceSP fixed = new KisFixedPaintDevice(cs);
    fixed->setRect(QRect(0, 0, 1, 1)); fixed->initialize();
    fixed->fill(fixed->bounds(), KoColor(&input, cs));
    KisRenderedDab first(fixed), second(fixed); second.offset = {65, 0};
    KisTransaction owner(dev); QVERIFY(owner.beginStrokeMutation());
    KisPainter painter(dev); painter.setCompositeOpId(operation->id());
    painter.setStrokeMutationOwner(&owner);
    bool failed = false;
    std::thread worker([&] {
        KisStrokeJobFailureContext failure(true);
        painter.bltFixed(QRect(0, 0, 66, 1), {first, second});
        failed = failure.failed();
    });
    const auto cleanup = qScopeGuard([&] { release.release(); if (worker.joinable()) worker.join(); });
    QVERIFY(firstWrite.tryAcquire(1, 5000));
    QCOMPARE(operation->firstPixel, input);
    QVERIFY(!owner.tryRevert()); // cancellation is recorded; the live borrow still owns its guard
    release.release(); worker.join();
    QVERIFY(failed);
    QCOMPARE(operation->calls, 1); // second page must never enter the kernel after cancellation
    QVERIFY(owner.tryRevert());
    QByteArray actual(128, 0); dev->readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, 128, 1);
    QCOMPARE(actual, QByteArray(128, char(initial)));
}

void KisPainterTest::testFixedCursorSnapshots_data()
{
    QTest::addColumn<bool>("deviceSource"); QTest::newRow("fixed-mask") << false; QTest::newRow("device-source-and-mask") << true;
}

void KisPainterTest::testFixedCursorSnapshots()
{
    QFETCH(bool, deviceSource);
    const auto *cs = KoColorSpaceRegistry::instance()->alpha8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);
    const QRect bounds(0, 0, 65, 1);
    KisSelectionSP selection = new KisSelection();
    selection->pixelSelection()->select(bounds, 255);
    KisFixedPaintDeviceSP fixed = new KisFixedPaintDevice(cs);
    fixed->setRect(bounds); fixed->initialize();
    const quint8 input = 71;
    fixed->fill(bounds, KoColor(&input, cs));
    KisPaintDeviceSP source = new KisPaintDevice(cs); source->fill(bounds, KoColor(&input, cs));
    KisTransaction owner(dev); QVERIFY(owner.beginStrokeMutation());
    KisPainter painter(dev, selection); painter.setCompositeOpId(COMPOSITE_COPY);
    painter.setStrokeMutationOwner(&owner);
    KisStrokeJobFailureContext failure(true);
    KisPageStoreDiagnosticRecorder recorder(true);
    int changes = 0;
    recorder.setPhaseObserver([&](auto phase) {
        if (phase == KisPageStoreDiagnosticPhase::PixelOperationBody && !changes++) {
            // Selection is independent of the destination owner. The already
            // captured mask must stay exact when its producer changes later.
            selection->pixelSelection()->clear();
            if (deviceSource) source->clear();
        }
    });
    if (deviceSource) painter.bitBlt(0, 0, source, 0, 0, bounds.width(), bounds.height());
    else painter.bltFixed(0, 0, fixed, 0, 0, bounds.width(), bounds.height());
    recorder.setPhaseObserver({});
    QVERIFY(!failure.failed()); QCOMPARE(changes, 1);
    QVERIFY(selection->selectedRect().isEmpty());
    QVERIFY(owner.checkpointStrokeMutation());
    QByteArray actual(bounds.width(), 0);
    dev->readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, bounds.width(), 1);
    QCOMPARE(actual, QByteArray(bounds.width(), char(input)));
    QVERIFY(owner.tryRevert());
}

void KisPainterTest::testMultiDabSelectionSnapshot()
{
    const auto *cs = KoColorSpaceRegistry::instance()->alpha8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);
    const QRect bounds(0, 0, 65, 1);
    KisSelectionSP selection = new KisSelection();
    selection->pixelSelection()->select(bounds, 255);
    KisFixedPaintDeviceSP fixed = new KisFixedPaintDevice(cs);
    fixed->setRect(bounds); fixed->initialize();
    const quint8 input = 71;
    fixed->fill(bounds, KoColor(&input, cs));
    KisTransaction owner(dev); QVERIFY(owner.beginStrokeMutation());
    KisPainter painter(dev, selection); painter.setCompositeOpId(COMPOSITE_COPY);
    painter.setStrokeMutationOwner(&owner);
    KisStrokeJobFailureContext failure(true);
    KisPageStoreDiagnosticRecorder recorder(true);
    int changes = 0;
    recorder.setPhaseObserver([&](auto phase) {
        if (phase == KisPageStoreDiagnosticPhase::PixelOperationBody && !changes++) {
            // Selection is independent of the destination owner. The already
            // captured mask must stay exact when its producer changes later.
            selection->pixelSelection()->clear();
        }
    });
    painter.bltFixed(bounds, {KisRenderedDab(fixed)});
    recorder.setPhaseObserver({});
    QVERIFY(!failure.failed()); QCOMPARE(changes, 1);
    QVERIFY(selection->selectedRect().isEmpty());
    QVERIFY(owner.checkpointStrokeMutation());
    QByteArray actual(bounds.width(), 0);
    dev->readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, bounds.width(), 1);
    QCOMPARE(actual, QByteArray(bounds.width(), char(input)));
    QVERIFY(owner.tryRevert());
}

void KisPainterTest::testMultiDabOwnerRejection()
{
    const auto *cs = KoColorSpaceRegistry::instance()->alpha8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs), other = new KisPaintDevice(cs);
    KisTransaction owner(dev); QVERIFY(owner.beginStrokeMutation()); int calls = 0;
    const auto body = [&](KisPixelWriteCursor *) { ++calls; return true; };
    QVERIFY(!other->applyPixelOperation(QRect(), body, &owner));
    QVERIFY(!other->applyPixelOperation(QVector<QRect>{}, body, &owner));
    QVERIFY(!other->applyPixelOperation(QVector<QRect>{QRect(0, 0, 1, 1), QRect(128, 0, 1, 1)}, body, &owner));
    QVERIFY(!other->applyPixelOperation(QRect(0, 0, 1, 1), body, &owner));
    QCOMPARE(calls, 0);
    KisFixedPaintDeviceSP fixed = new KisFixedPaintDevice(cs); fixed->setRect(QRect(0, 0, 1, 1)); fixed->initialize();
    {
        KisPainter painter(other); KisStrokeJobFailureContext failure(true);
        painter.setStrokeMutationOwner(&owner);
        painter.bltFixed(QRect(0, 0, 1, 1), {KisRenderedDab(fixed)}); QVERIFY(failure.failed());
    }
    QVERIFY(owner.tryRevert());
    QVERIFY(!dev->applyPixelOperation(QRect(), body, &owner)); QCOMPARE(calls, 0);
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

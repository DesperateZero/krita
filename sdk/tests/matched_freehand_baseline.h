/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef MATCHED_FREEHAND_BASELINE_H
#define MATCHED_FREEHAND_BASELINE_H

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <KoCanvasResourceProvider.h>
#include <KoColorSpace.h>
#include <KoCompositeOpRegistry.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>
#include <brushengine/kis_paint_information.h>
#include "KisAsynchronousStrokeUpdateHelper.h"
#include "kis_image.h"
#include "kis_group_layer.h"
#include "kis_node.h"
#include "kis_paint_device.h"
#include "kis_resources_snapshot.h"
#include "kis_undo_stores.h"
#include "stroke_testing_utils.h"
#include "strokes/freehand_stroke.h"
#include "strokes/KisFreehandStrokeInfo.h"

#include <atomic>
#include <memory>

namespace TestUtil {

// This fixture is copied unchanged into the fixed upstream test build.
// It measures the ordinary CPU strategy with updates, not canvas visibility
// or an opt-in persistent strategy. Keep its inputs and endpoints identical.
inline void runMatchedFreehandBaseline()
{
    struct Probe {
        std::atomic<int> segments {0};
        std::atomic<int> updates {0};
        std::atomic<int> finish {0};
        std::atomic<int> destroyed {0};
    } probe;

    class Strategy final : public FreehandStrokeStrategy {
    public:
        Strategy(KisResourcesSnapshotSP resources, Probe &probe)
            : FreehandStrokeStrategy(resources, new KisFreehandStrokeInfo(),
                                     kundo2_noi18n("Matched CPU baseline"))
            , m_probe(probe)
        {
        }
        ~Strategy() override { ++m_probe.destroyed; }
        void doStrokeCallback(KisStrokeJobData *data) override
        {
            FreehandStrokeStrategy::doStrokeCallback(data);
            if (dynamic_cast<FreehandStrokeStrategy::Data *>(data)) ++m_probe.segments;
            if (dynamic_cast<KisAsynchronousStrokeUpdateHelper::UpdateData *>(data)) ++m_probe.updates;
        }
        void finishStrokeCallback() override
        {
            FreehandStrokeStrategy::finishStrokeCallback();
            ++m_probe.finish;
        }
    private:
        Probe &m_probe;
    };

    QElapsedTimer lifecycle;
    lifecycle.start();
    auto *undoStore = new KisSurrogateUndoStore(); // owned by image
    KisImageSP image = utils::createImage(undoStore, QSize(512, 512));
    image->setWorkingThreadsLimit(4);
    KisNodeSP node = image->rootLayer()->firstChild();
    std::unique_ptr<KoCanvasResourceProvider> manager(
        utils::createResourceManager(image, node, "autobrush_300px.kpp"));
    KisPaintOpPresetSP preset = manager->resource(
        KoCanvasResource::CurrentPaintOpPreset).value<KisPaintOpPresetSP>();
    QVERIFY(preset && preset->settings());
    QCOMPARE(preset->paintOp().id(), QString("paintbrush"));
    preset->settings()->setProperty("PaintOpAction", 1); // Buildup
    preset->settings()->setPaintOpSize(48);
    QVERIFY(preset->settings()->paintIncremental());
    KisResourcesSnapshotSP resources = new KisResourcesSnapshot(image, node, manager.get());
    QVERIFY(!resources->needsIndirectPainting());
    QVERIFY(!resources->needsMaskingBrushRendering());
    QCOMPARE(resources->compositeOpId(), QString(COMPOSITE_OVER));
    QCOMPARE(image->colorSpace()->pixelSize(), quint32(4));
    image->waitForDone();

    const auto projection = [&] {
        return image->projection()->convertToQImage(nullptr, image->bounds());
    };
    const auto digest = [](const QImage &input) {
        const QImage rgba = input.convertToFormat(QImage::Format_RGBA8888);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (int row = 0; row < rgba.height(); ++row) {
            hash.addData(QByteArrayView(reinterpret_cast<const char *>(rgba.constScanLine(row)),
                                        rgba.width() * 4));
        }
        return QString::fromLatin1(hash.result().toHex());
    };
    const QImage blank = projection();
    QVERIFY(!undoStore->presentCommand());
    std::atomic<int> projectionUpdates {0};
    const auto connection = QObject::connect(image.data(), &KisImage::sigImageUpdated,
        image.data(), [&](const QRect &) { ++projectionUpdates; }, Qt::DirectConnection);
    const qint64 setupNs = lifecycle.nsecsElapsed();

    QElapsedTimer stroke;
    stroke.start();
    const KisStrokeId id = image->startStroke(new Strategy(resources, probe));
    for (int segment = 0; segment < 8; ++segment) {
        const int y = 64 + segment * 48;
        const bool reverse = segment % 2;
        const KisPaintInformation from(QPointF(reverse ? 448 : 64, y), 0.5);
        const KisPaintInformation to(QPointF(reverse ? 64 : 448, y), 1.0);
        image->addJob(id, new FreehandStrokeStrategy::Data(0, from, to));
        if (reverse) image->addJob(id, new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
    }
    QElapsedTimer penUp;
    penUp.start();
    image->endStroke(id);
    image->waitForDone();
    const qint64 penUpNs = penUp.nsecsElapsed();
    const qint64 strokeNs = stroke.nsecsElapsed();
    QCOMPARE(probe.segments.load(), 8);
    QCOMPARE(probe.updates.load(), 4);
    QCOMPARE(probe.finish.load(), 1);
    QCOMPARE(probe.destroyed.load(), 1);
    QVERIFY(projectionUpdates.load() > 0);
    QVERIFY(undoStore->presentCommand());
    const QImage painted = projection();
    QVERIFY(painted != blank);
    const QImage layer = node->paintDevice()->convertToQImage(nullptr, image->bounds());
    QCOMPARE(digest(painted), digest(layer));
    QObject::disconnect(connection);

    QElapsedTimer undo;
    undo.start();
    undoStore->undo();
    image->waitForDone();
    const qint64 undoNs = undo.nsecsElapsed();
    QVERIFY(!undoStore->presentCommand()); // all eight segments formed one command
    QVERIFY(node->paintDevice()->exactBounds().isEmpty());
    const QImage undone = projection();
    QCOMPARE(digest(undone), digest(blank));
    QElapsedTimer redo;
    redo.start();
    undoStore->redo();
    image->waitForDone();
    const qint64 redoNs = redo.nsecsElapsed();
    QVERIFY(undoStore->presentCommand());
    QCOMPARE(digest(projection()), digest(painted));

    const QString blankHash = digest(blank);
    const QString paintedHash = digest(painted);
    KisImageWSP releasedImage(image);
    QElapsedTimer teardown;
    teardown.start();
    resources.clear();
    preset.clear();
    manager.reset();
    node.clear();
    image.clear(); // original scheduler/undo-store destructors finish here
    const qint64 teardownNs = teardown.nsecsElapsed();
    // KisImage disconnects signals in its destructor, before QObject::destroyed.
    // Its original weak-reference validity proves final strong-owner release.
    const bool imageDestroyed = !releasedImage.isValid();
    QVERIFY(imageDestroyed);
    const qint64 totalNs = lifecycle.nsecsElapsed();
    const QJsonObject record {
        {"schema", 1}, {"workload", "rgba8-buildup-over-512-8-lines-v1"},
        {"strategy", "ordinary-cpu-default"}, {"canvas_visible_endpoint", false},
        {"workers", 4}, {"segment_callbacks", probe.segments.load()},
        {"update_callbacks", probe.updates.load()}, {"projection_updates", projectionUpdates.load()},
        {"strategy_destroyed", true}, {"image_destroyed", imageDestroyed},
        {"blank_sha256", blankHash}, {"painted_sha256", paintedHash},
        {"setup_ns", setupNs}, {"stroke_to_projection_ready_ns", strokeNs},
        {"pen_up_to_projection_ready_ns", penUpNs}, {"undo_drain_ns", undoNs},
        {"redo_drain_ns", redoNs}, {"owner_teardown_ns", teardownNs},
        {"total_fixture_ns", totalNs}
    };
    qInfo().noquote() << "BR1_CP0_JSON" << QJsonDocument(record).toJson(QJsonDocument::Compact);
}

} // namespace TestUtil
#endif

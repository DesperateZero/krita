/*
 *  SPDX-FileCopyrightText: 2011 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "freehand_stroke_test.h"

#include <simpletest.h>
#include <KoCompositeOpRegistry.h>
#include <KoColor.h>
#include "stroke_testing_utils.h"
#include "strokes/freehand_stroke.h"
#include "strokes/KisFreehandStrokeInfo.h"
#include "strokes/KisMaskedFreehandStrokePainter.h"
#include "KisAsynchronousStrokeUpdateHelper.h"
#include "kis_resources_snapshot.h"
#include "kis_paintop_preset.h"
#include "kis_paintop_settings.h"
#include "kis_image.h"
#include "kis_painter.h"
#include "kis_paint_device.h"
#include "kis_datamanager.h"
#include "kis_image_config.h"
#include "kis_random_accessor_ng.h"
#include "kis_transaction.h"
#include "kis_undo_stores.h"
#include "KisStrokeJobFailureContext.h"
#include "KisRunnableStrokeJobData.h"
#include "KisRunnableStrokeJobsInterface.h"
#include "KisRunnableStrokeJobUtils.h"
#include <brushengine/kis_paint_information.h>
#include "tiles3/kis_tile_data.h"
#include "tiles3/kis_tile_data_store.h"
#include "tiles3/tests/kis_tile_data_store_test_access.h"

#include "testui.h"

#include <atomic>
#include <memory>

namespace {

enum class PersistentFailurePoint {
    None,
    Admission,
    Pixel,
    Checkpoint,
    Finish
};

enum class PersistentWorkload {
    Basic,
    TinyBudget
};

class TinyTileBudget
{
public:
    explicit TinyTileBudget(int hardMegabytes = 8, int poolMegabytes = 0)
        : m_oldHard(m_config.memoryHardLimitPercent())
        , m_oldSoft(m_config.memorySoftLimitPercent())
        , m_oldPool(m_config.memoryPoolLimitPercent())
        , m_oldSwap(m_config.maxSwapSize())
    {
        const qreal configuredMegabytes = hardMegabytes + poolMegabytes + 0.5;
        m_config.setMemoryHardLimitPercent(
            100.0 * configuredMegabytes / KisImageConfig::totalRAM());
        m_config.setMemorySoftLimitPercent(0);
        m_config.setMemoryPoolLimitPercent(
            100.0 * poolMegabytes / configuredMegabytes);
        m_config.setMaxSwapSize(0);
        KisTileDataStoreTestAccess::rereadConfig();
        KisTileDataStoreTestAccess::clear();
    }

    ~TinyTileBudget()
    {
        m_config.setMemoryHardLimitPercent(m_oldHard);
        m_config.setMemorySoftLimitPercent(m_oldSoft);
        m_config.setMemoryPoolLimitPercent(m_oldPool);
        m_config.setMaxSwapSize(m_oldSwap);
        KisTileDataStoreTestAccess::rereadConfig();
    }

    quint64 hardBytes() const
    {
        return quint64(KisImageConfig(true).tilesHardLimit()) << 20;
    }

    quint64 nonPayloadBytes() const
    {
        const KisImageConfig config(true);
        return quint64(config.tilesHardLimit() + config.poolLimit()) << 20;
    }

    static quint64 residentBytes()
    {
        return quint64(qMax<qint64>(0, KisTileDataStore::instance()->memoryMetric()))
            * KisTileData::WIDTH * KisTileData::HEIGHT;
    }

private:
    KisImageConfig m_config{false};
    qreal m_oldHard;
    qreal m_oldSoft;
    qreal m_oldPool;
    int m_oldSwap;
};

struct PersistentStrokeProbe
{
    std::atomic<int> visibleChecks {0};
    std::atomic<int> visiblePixels {0};
    std::atomic<int> recursiveProducerSteps {0};
    std::atomic<int> finishCallbacks {0};
    std::atomic<int> cancelCallbacks {0};
    std::atomic<int> failureTriggers {0};
    std::atomic<int> postFailureJobs {0};
    std::atomic<bool> admissionBlockerReady {false};
    std::atomic<bool> pixelFailureReported {false};
    std::atomic<bool> finishObservedDrainedProducer {false};
    std::atomic<int> slowReaderAcquired {0};
    std::atomic<int> slowReaderChecks {0};
    std::atomic<int> slowReaderStable {0};
    std::atomic<int> slowReaderReleased {0};
    std::atomic<quint64> peakResidentBytes {0};
};

class ProbedFreehandStrokeStrategy : public FreehandStrokeStrategy
{
public:
    class ProbeData : public KisStrokeJobData
    {
    public:
        explicit ProbeData(const QPoint &point)
            : point(point)
        {
        }

        QPoint point;
    };

    class RecursiveProducerData : public KisStrokeJobData
    {
    public:
        explicit RecursiveProducerData(int steps)
            : steps(steps)
        {
        }

        int steps = 0;
    };

    class ChangeContextData : public KisStrokeJobData {};
    class PixelFailureData : public KisStrokeJobData {};
    class CheckpointFailureData : public KisStrokeJobData {};
    class PostFailureData : public KisStrokeJobData {};

    class HoldSlowReaderData : public KisStrokeJobData
    {
    public:
        explicit HoldSlowReaderData(const QPoint &point)
            : point(point)
        {
        }

        QPoint point;
    };

    class CheckSlowReaderData : public KisStrokeJobData {};
    class ReleaseSlowReaderData : public KisStrokeJobData {};

    ProbedFreehandStrokeStrategy(KisResourcesSnapshotSP resources,
                                 KisFreehandStrokeInfo *strokeInfo,
                                 PersistentStrokeProbe *probe)
        : FreehandStrokeStrategy(resources, strokeInfo, kundo2_noi18n("Freehand Stroke"))
        , m_probe(probe)
    {
        setUsesPersistentStrokeMutation(true);
    }

    void doStrokeCallback(KisStrokeJobData *data) override
    {
        if (auto *probeData = dynamic_cast<ProbeData *>(data)) {
            QColor pixel;
            targetDevice()->pixel(probeData->point.x(), probeData->point.y(), &pixel);
            ++m_probe->visibleChecks;
            if (pixel.alpha() != 0) ++m_probe->visiblePixels;
            return;
        }
        if (auto *producerData = dynamic_cast<RecursiveProducerData *>(data)) {
            enqueueProducerStep(producerData->steps);
            return;
        }
        if (dynamic_cast<ChangeContextData *>(data)) {
            targetDevice()->moveTo(7, 11);
            ++m_probe->failureTriggers;
            return;
        }
        if (dynamic_cast<PixelFailureData *>(data)) {
            ++m_probe->failureTriggers;
            m_probe->pixelFailureReported = KisStrokeJobFailureContext::reportFailure(
                QStringLiteral("Injected painter pixel failure after a real brush update"));
            return;
        }
        if (dynamic_cast<CheckpointFailureData *>(data)) {
            QVector<KisRunnableStrokeJobData *> jobs;
            maskedPainter(0)->doAsynchronousUpdate(jobs);
            KritaUtils::addJobSequential(jobs, [this] {
                targetDevice()->moveTo(7, 11);
                ++m_probe->failureTriggers;
                checkpointStrokeMutationBeforeDirtyPublish();
            });
            runnableJobsInterface()->addRunnableJobs(jobs);
            return;
        }
        if (dynamic_cast<PostFailureData *>(data)) {
            ++m_probe->postFailureJobs;
            return;
        }
        if (auto *readerData = dynamic_cast<HoldSlowReaderData *>(data)) {
            m_slowReader = targetDevice()->createRandomConstAccessorNG();
            m_slowReader->moveTo(readerData->point.x(), readerData->point.y());
            const quint8 *pixel = m_slowReader->rawDataConst();
            if (pixel) {
                m_slowPixel = QByteArray(reinterpret_cast<const char *>(pixel),
                                         targetDevice()->pixelSize());
                ++m_probe->slowReaderAcquired;
            }
            sampleResidentBytes();
            return;
        }
        if (dynamic_cast<CheckSlowReaderData *>(data)) {
            ++m_probe->slowReaderChecks;
            const quint8 *pixel = m_slowReader ? m_slowReader->rawDataConst() : nullptr;
            if (pixel && QByteArray(reinterpret_cast<const char *>(pixel),
                                    targetDevice()->pixelSize()) == m_slowPixel) {
                ++m_probe->slowReaderStable;
            }
            sampleResidentBytes();
            return;
        }
        if (dynamic_cast<ReleaseSlowReaderData *>(data)) {
            m_slowReader.clear();
            m_slowPixel.clear();
            ++m_probe->slowReaderReleased;
            sampleResidentBytes();
            return;
        }
        FreehandStrokeStrategy::doStrokeCallback(data);
    }

    void finishStrokeCallback() override
    {
        m_probe->finishObservedDrainedProducer =
            m_probe->recursiveProducerSteps.load() == ProducerSteps;
        ++m_probe->finishCallbacks;
        FreehandStrokeStrategy::finishStrokeCallback();
    }

    void cancelStrokeCallback() override
    {
        ++m_probe->cancelCallbacks;
        FreehandStrokeStrategy::cancelStrokeCallback();
    }

private:
    void enqueueProducerStep(int remaining)
    {
        QVector<KisRunnableStrokeJobData *> jobs;
        KritaUtils::addJobSequential(jobs, [this, remaining] {
            ++m_probe->recursiveProducerSteps;
            if (remaining > 1) enqueueProducerStep(remaining - 1);
        });
        runnableJobsInterface()->addRunnableJobs(jobs);
    }

    void sampleResidentBytes()
    {
        const quint64 value = TinyTileBudget::residentBytes();
        quint64 peak = m_probe->peakResidentBytes.load();
        while (peak < value &&
               !m_probe->peakResidentBytes.compare_exchange_weak(peak, value)) {
        }
    }

public:
    static constexpr int ProducerSteps = 3;

private:
    PersistentStrokeProbe *m_probe;
    KisRandomConstAccessorSP m_slowReader;
    QByteArray m_slowPixel;
};

}

class FreehandStrokeTester : public utils::StrokeTester
{
public:
    FreehandStrokeTester(const QString &presetFilename, bool useLod = false,
                         bool persistentMutation = false,
                         PersistentFailurePoint failurePoint = PersistentFailurePoint::None,
                         PersistentWorkload workload = PersistentWorkload::Basic)
        : StrokeTester(useLod ? "freehand-lod" : "freehand",
                       workload == PersistentWorkload::TinyBudget
                           ? QSize(1024, 1024) : QSize(500, 500),
                       presetFilename),
          m_useLod(useLod),
          m_flipLineDirection(false),
          m_persistentMutation(persistentMutation),
          m_failurePoint(failurePoint),
          m_workload(workload)
    {
        setBaseFuzziness(3);
    }

    void setFlipLineDirection(bool value) {
        m_flipLineDirection = value;
        setNumIterations(2);
    }

    void setPaintColor(const QColor &color) {
        m_paintColor.reset(new QColor(color));
    }

    const PersistentStrokeProbe &persistentProbe() const { return m_persistentProbe; }
    int undoRedoChecks() const { return m_undoRedoChecks; }
    int failureChecks() const { return m_failureChecks; }

protected:
    using utils::StrokeTester::initImage;
    void initImage(KisImageWSP image, KisNodeSP activeNode) override {
        if (m_useLod) {
            image->setLodPreferences(KisLodPreferences(1));
        }
        Q_UNUSED(activeNode);
    }

    void beforeCheckingResult(KisImageWSP image, KisNodeSP activeNode) override {
        Q_UNUSED(image);
        Q_UNUSED(activeNode);

        if (m_useLod) {
            //image->testingSetLevelOfDetailsEnabled(true);
        }
    }

    void iterationEndedCallback(KisImageWSP image, KisNodeSP activeNode, int iteration) override
    {
        Q_UNUSED(iteration);
        if (!m_persistentMutation) return;

        auto *undoStore = dynamic_cast<KisSurrogateUndoStore *>(image->undoStore());
        QVERIFY(undoStore);

        if (m_failurePoint != PersistentFailurePoint::None) {
            if (m_admissionBlocker) {
                QString error;
                QVERIFY2(m_admissionBlocker->tryRevert(&error), qPrintable(error));
                m_admissionBlocker.reset();
            }
            QVERIFY(!undoStore->presentCommand());
            QCOMPARE(activeNode->paintDevice()->x(), 0);
            QCOMPARE(activeNode->paintDevice()->y(), 0);
            QVERIFY(activeNode->paintDevice()->exactBounds().isEmpty());
            QVERIFY(!activeNode->paintDevice()->dataManager()->hasCurrentMemento());
            ++m_failureChecks;
            return;
        }

        if (m_persistentProbe.cancelCallbacks.load() > 0) {
            QVERIFY(!undoStore->presentCommand());
            QVERIFY(activeNode->paintDevice()->exactBounds().isEmpty());
            QVERIFY(!activeNode->paintDevice()->dataManager()->hasCurrentMemento());
            return;
        }

        QVERIFY(image->lastExecutedCommand());

        const QImage painted = activeNode->paintDevice()->convertToQImage(
            nullptr, 0, 0, image->width(), image->height());
        QVERIFY(!painted.isNull());

        undoStore->undo();
        image->waitForDone();
        QVERIFY(!undoStore->presentCommand());
        QVERIFY(activeNode->paintDevice()->exactBounds().isEmpty());

        undoStore->redo();
        image->waitForDone();
        QVERIFY(undoStore->presentCommand());
        const QImage redone = activeNode->paintDevice()->convertToQImage(
            nullptr, 0, 0, image->width(), image->height());
        QCOMPARE(redone, painted);
        ++m_undoRedoChecks;
    }

    void modifyResourceManager(KoCanvasResourceProvider *manager,
                               KisImageWSP image) override
    {
        modifyResourceManager(manager, image, 0);
    }


    void modifyResourceManager(KoCanvasResourceProvider *manager,
                               KisImageWSP image,
                               int iteration) override {

        if (m_persistentMutation) {
            const KisPaintOpPresetSP preset =
                manager->resource(KoCanvasResource::CurrentPaintOpPreset)
                    .value<KisPaintOpPresetSP>();
            QVERIFY(preset && preset->settings());
            preset->settings()->setProperty("PaintOpAction", 1);
            QVERIFY(preset->settings()->paintIncremental());
        }

        if (m_paintColor && iteration > 0) {
            QVariant i;
            i.setValue(KoColor(*m_paintColor, image->colorSpace()));
            manager->setResource(KoCanvasResource::ForegroundColor, i);
        }
    }

    KisStrokeStrategy* createStroke(KisResourcesSnapshotSP resources,
                                    KisImageWSP image) override {
        Q_UNUSED(image);

        if (m_failurePoint == PersistentFailurePoint::Admission) {
            m_admissionBlocker.reset(new KisTransaction(resources->currentNode()->paintDevice()));
            m_persistentProbe.admissionBlockerReady = m_admissionBlocker->hasMemento();
        }

        KisFreehandStrokeInfo *strokeInfo = new KisFreehandStrokeInfo();

        std::unique_ptr<FreehandStrokeStrategy> stroke(m_persistentMutation
            ? static_cast<FreehandStrokeStrategy *>(
                new ProbedFreehandStrokeStrategy(
                    resources, strokeInfo, &m_persistentProbe))
            : new FreehandStrokeStrategy(resources, strokeInfo, kundo2_noi18n("Freehand Stroke")));

        return stroke.release();
    }

    void addPaintingJobs(KisImageWSP image,
                                 KisResourcesSnapshotSP resources) override
    {
        addPaintingJobs(image, resources, 0);
    }

    void addPaintingJobs(KisImageWSP image, KisResourcesSnapshotSP resources, int iteration) override {
        Q_UNUSED(resources);

        KisPaintInformation pi1;
        KisPaintInformation pi2;

        if (!iteration) {
            pi1 = KisPaintInformation(QPointF(200, 200));
            pi2 = KisPaintInformation(QPointF(300, 300));
        } else {
            pi1 = KisPaintInformation(QPointF(200, 300));
            pi2 = KisPaintInformation(QPointF(300, 200));
        }

        if (m_failurePoint == PersistentFailurePoint::Admission) {
            image->addJob(strokeId(), new FreehandStrokeStrategy::Data(0, pi1, pi2));
            image->addJob(strokeId(), new ProbedFreehandStrokeStrategy::PostFailureData());
            return;
        }
        if (m_failurePoint == PersistentFailurePoint::Pixel) {
            image->addJob(strokeId(), new FreehandStrokeStrategy::Data(0, pi1, pi2));
            image->addJob(strokeId(), new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
            image->addJob(strokeId(), new ProbedFreehandStrokeStrategy::PixelFailureData());
            image->addJob(strokeId(), new ProbedFreehandStrokeStrategy::PostFailureData());
            return;
        }
        if (m_failurePoint == PersistentFailurePoint::Checkpoint) {
            image->addJob(strokeId(), new FreehandStrokeStrategy::Data(0, pi1, pi2));
            image->addJob(strokeId(), new ProbedFreehandStrokeStrategy::CheckpointFailureData());
            image->addJob(strokeId(), new ProbedFreehandStrokeStrategy::PostFailureData());
            return;
        }
        if (m_failurePoint == PersistentFailurePoint::Finish) {
            image->addJob(strokeId(), new FreehandStrokeStrategy::Data(0, pi1, pi2));
            image->addJob(strokeId(), new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
            image->addJob(strokeId(), new ProbedFreehandStrokeStrategy::ChangeContextData());
            return;
        }

        if (m_workload == PersistentWorkload::TinyBudget) {
            image->addJob(strokeId(), new FreehandStrokeStrategy::Data(0, pi1, pi2));
            image->addJob(strokeId(), new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
            image->addJob(strokeId(),
                          new ProbedFreehandStrokeStrategy::HoldSlowReaderData(QPoint(250, 250)));

            for (int coordinate : {128, 384, 640, 896}) {
                image->addJob(strokeId(), new FreehandStrokeStrategy::Data(
                    0, KisPaintInformation(QPointF(64, coordinate)),
                    KisPaintInformation(QPointF(960, coordinate))));
                image->addJob(strokeId(), new FreehandStrokeStrategy::Data(
                    0, KisPaintInformation(QPointF(coordinate, 64)),
                    KisPaintInformation(QPointF(coordinate, 960))));
            }
            image->addJob(strokeId(), new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
            image->addJob(strokeId(), new ProbedFreehandStrokeStrategy::CheckSlowReaderData());
            image->addJob(strokeId(), new ProbedFreehandStrokeStrategy::ReleaseSlowReaderData());
            image->addJob(strokeId(), new ProbedFreehandStrokeStrategy::RecursiveProducerData(
                ProbedFreehandStrokeStrategy::ProducerSteps));
            return;
        }

        image->addJob(strokeId(), new FreehandStrokeStrategy::Data(0, pi1, pi2));
        image->addJob(strokeId(), new KisAsynchronousStrokeUpdateHelper::UpdateData(true));

        if (!m_persistentMutation) return;

        image->addJob(strokeId(), new ProbedFreehandStrokeStrategy::ProbeData(QPoint(250, 250)));

        const KisPaintInformation pi3(QPointF(100, 400));
        const KisPaintInformation pi4(QPointF(200, 400));
        image->addJob(strokeId(), new FreehandStrokeStrategy::Data(0, pi3, pi4));
        image->addJob(strokeId(), new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
        image->addJob(strokeId(), new ProbedFreehandStrokeStrategy::ProbeData(QPoint(150, 400)));
        image->addJob(strokeId(), new ProbedFreehandStrokeStrategy::RecursiveProducerData(
            ProbedFreehandStrokeStrategy::ProducerSteps));
    }

private:
    bool m_useLod;
    bool m_flipLineDirection;
    bool m_persistentMutation;
    PersistentFailurePoint m_failurePoint;
    PersistentWorkload m_workload;
    PersistentStrokeProbe m_persistentProbe;
    int m_undoRedoChecks = 0;
    int m_failureChecks = 0;
    std::unique_ptr<KisTransaction> m_admissionBlocker;
    QScopedPointer<QColor> m_paintColor;
};

void FreehandStrokeTest::testAutoBrushStroke()
{
    FreehandStrokeTester tester("autobrush_300px.kpp");
    tester.test();
}

void FreehandStrokeTest::testPersistentAutoBrushStroke()
{
    FreehandStrokeTester tester("autobrush_300px.kpp", false, true);
    tester.testSimpleStrokeNoVerification();
    QCOMPARE(tester.persistentProbe().visibleChecks.load(), 2);
    QCOMPARE(tester.persistentProbe().visiblePixels.load(), 2);
    QCOMPARE(tester.persistentProbe().recursiveProducerSteps.load(),
             ProbedFreehandStrokeStrategy::ProducerSteps);
    QVERIFY(tester.persistentProbe().finishObservedDrainedProducer.load());
    QCOMPARE(tester.persistentProbe().finishCallbacks.load(), 1);
    QCOMPARE(tester.persistentProbe().cancelCallbacks.load(), 0);
    QCOMPARE(tester.undoRedoChecks(), 1);
}

void FreehandStrokeTest::testPersistentAutoBrushStrokeCancelled()
{
    FreehandStrokeTester tester("autobrush_300px.kpp", false, true);
    tester.testSimpleStrokeCancelled();
    QCOMPARE(tester.persistentProbe().finishCallbacks.load(), 0);
    QVERIFY(tester.persistentProbe().cancelCallbacks.load() >= 1);
}

void FreehandStrokeTest::testPersistentAutoBrushStrokeFailure_data()
{
    QTest::addColumn<int>("failurePoint");
    QTest::newRow("admission") << int(PersistentFailurePoint::Admission);
    QTest::newRow("pixel") << int(PersistentFailurePoint::Pixel);
    QTest::newRow("checkpoint") << int(PersistentFailurePoint::Checkpoint);
    QTest::newRow("finish") << int(PersistentFailurePoint::Finish);
}

void FreehandStrokeTest::testPersistentAutoBrushStrokeFailure()
{
    QFETCH(int, failurePoint);
    const auto point = PersistentFailurePoint(failurePoint);
    FreehandStrokeTester tester("autobrush_300px.kpp", false, true, point);
    tester.testSimpleStrokeNoVerification();

    QCOMPARE(tester.persistentProbe().failureTriggers.load(),
             point == PersistentFailurePoint::Admission ? 0 : 1);
    QCOMPARE(tester.persistentProbe().admissionBlockerReady.load(),
             point == PersistentFailurePoint::Admission);
    QCOMPARE(tester.persistentProbe().postFailureJobs.load(), 0);
    QCOMPARE(tester.persistentProbe().pixelFailureReported.load(),
             point == PersistentFailurePoint::Pixel);
    QCOMPARE(tester.persistentProbe().finishCallbacks.load(),
             point == PersistentFailurePoint::Finish ? 1 : 0);
    QCOMPARE(tester.persistentProbe().cancelCallbacks.load(), 1);
    QCOMPARE(tester.undoRedoChecks(), 0);
    QCOMPARE(tester.failureChecks(), 1);
}

void FreehandStrokeTest::testPersistentAutoBrushStrokePressure()
{
    TinyTileBudget budget(24, 0);
    QCOMPARE(budget.hardBytes(), quint64(24 * 1024 * 1024));
    QCOMPARE(budget.nonPayloadBytes(), quint64(24 * 1024 * 1024));

    FreehandStrokeTester tester("autobrush_300px.kpp", false, true,
                                PersistentFailurePoint::None,
                                PersistentWorkload::TinyBudget);
    tester.testSimpleStrokeNoVerification();

    QCOMPARE(tester.persistentProbe().slowReaderAcquired.load(), 1);
    QCOMPARE(tester.persistentProbe().slowReaderChecks.load(), 1);
    QCOMPARE(tester.persistentProbe().slowReaderStable.load(), 1);
    QCOMPARE(tester.persistentProbe().slowReaderReleased.load(), 1);
    QCOMPARE(tester.persistentProbe().recursiveProducerSteps.load(),
             ProbedFreehandStrokeStrategy::ProducerSteps);
    QVERIFY(tester.persistentProbe().finishObservedDrainedProducer.load());
    QCOMPARE(tester.persistentProbe().finishCallbacks.load(), 1);
    QCOMPARE(tester.persistentProbe().cancelCallbacks.load(), 0);
    QCOMPARE(tester.undoRedoChecks(), 1);
    QVERIFY(tester.persistentProbe().peakResidentBytes.load() >=
            budget.hardBytes() / 2);
    QVERIFY(tester.persistentProbe().peakResidentBytes.load() <= budget.hardBytes());
    QCOMPARE(TinyTileBudget::residentBytes(), quint64(0));
}

void FreehandStrokeTest::testPersistentAutoBrushStrokePressureCancelled()
{
    TinyTileBudget budget;
    QCOMPARE(budget.hardBytes(), quint64(8 * 1024 * 1024));

    FreehandStrokeTester tester("autobrush_300px.kpp", false, true,
                                PersistentFailurePoint::None,
                                PersistentWorkload::Basic);
    tester.testSimpleStrokeCancelled();

    QCOMPARE(tester.persistentProbe().finishCallbacks.load(), 0);
    QVERIFY(tester.persistentProbe().cancelCallbacks.load() >= 1);
    QCOMPARE(TinyTileBudget::residentBytes(), quint64(0));
}

void FreehandStrokeTest::testHatchingStroke()
{
    FreehandStrokeTester tester("hatching_30px.kpp");
    tester.test();
}

void FreehandStrokeTest::testColorSmudgeStroke()
{
    FreehandStrokeTester tester("colorsmudge_predefined.kpp");
    tester.test();
}

void FreehandStrokeTest::testAutoTextured17()
{
    FreehandStrokeTester tester("auto_textured_17.kpp");
    tester.test();
}

void FreehandStrokeTest::testAutoTextured38()
{
    FreehandStrokeTester tester("auto_textured_38.kpp");
    tester.test();
}

void FreehandStrokeTest::testMixDullCompositing()
{
    FreehandStrokeTester tester("Mix_dull.kpp");
    tester.setFlipLineDirection(true);
    tester.setPaintColor(Qt::red);
    tester.test();
}

void FreehandStrokeTest::testAutoBrushStrokeLod()
{
    FreehandStrokeTester tester("Basic_tip_default.kpp", true);
    tester.testSimpleStroke();
}

void FreehandStrokeTest::testPredefinedBrushStrokeLod()
{
    FreehandStrokeTester tester("testing_predefined_lod_spc13.kpp", true);
    //FreehandStrokeTester tester("testing_predefined_lod.kpp", true);
    tester.testSimpleStroke();
}

KISTEST_MAIN(FreehandStrokeTest)

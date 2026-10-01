/*
 *  SPDX-FileCopyrightText: 2017 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisDabRenderingQueueTest.h"

#include <simpletest.h>
#include "kis_datamanager.h"
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>

#include <../KisDabRenderingQueue.h>
#include <../KisRenderedDab.h>
#include <../KisDabRenderingJob.h>

struct SurrogateCacheInterface : public KisDabRenderingQueue::CacheInterface
{
    void getDabType(bool hasDabInCache,
                    KisDabCacheUtils::DabRenderingResources *resources,
                    const KisDabCacheUtils::DabRequestInfo &request,
                    /* out */
                    KisDabCacheUtils::DabGenerationInfo *di,
                    bool *shouldUseCache) override
    {
        Q_UNUSED(resources);
        Q_UNUSED(request);

        if (!hasDabInCache || typeOverride == KisDabRenderingJob::Dab) {
            di->needsPostprocessing = false;
            *shouldUseCache = false;
        } else if (typeOverride == KisDabRenderingJob::Copy) {
            di->needsPostprocessing = false;
            *shouldUseCache = true;
        } else if (typeOverride == KisDabRenderingJob::Postprocess) {
            di->needsPostprocessing = true;
            *shouldUseCache = true;
        }

        di->info = request.info;
    }

    bool hasSeparateOriginal(KisDabCacheUtils::DabRenderingResources *resources) const override {
        Q_UNUSED(resources);
        return typeOverride == KisDabRenderingJob::Postprocess;
    }

    KisDabRenderingJob::JobType typeOverride = KisDabRenderingJob::Dab;
};

#include <kis_mask_generator.h>
#include "kis_auto_brush.h"

KisDabCacheUtils::DabRenderingResources *testResourcesFactory()
{
    KisDabCacheUtils::DabRenderingResources *resources =
        new KisDabCacheUtils::DabRenderingResources();

    KisCircleMaskGenerator* circle = new KisCircleMaskGenerator(10, 1.0, 1.0, 1.0, 2, false);
    KisBrushSP brush(new KisAutoBrush(circle, 0.0, 0.0));
    resources->brush = brush;

    return resources;
}

void KisDabRenderingQueueTest::testCachedDabs()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();

    SurrogateCacheInterface *cacheInterface = new SurrogateCacheInterface();

    KisDabRenderingQueue queue(cs, testResourcesFactory);
    queue.setCacheInterface(cacheInterface);

    KoColor color;
    QPointF pos1(10,10);
    QPointF pos2(20,20);
    KisDabShape shape;
    KisPaintInformation pi1(pos1);
    KisPaintInformation pi2(pos2);

    KisDabCacheUtils::DabRequestInfo request1(color, pos1, shape, pi1, 1.0);
    KisDabCacheUtils::DabRequestInfo request2(color, pos2, shape, pi2, 1.0);

    cacheInterface->typeOverride = KisDabRenderingJob::Dab;
    KisDabRenderingJobSP job0 = queue.addDab(request1, OPACITY_OPAQUE_F, OPACITY_OPAQUE_F);

    QVERIFY(job0);
    QCOMPARE(job0->seqNo, 0);
    QCOMPARE(job0->generationInfo.info.pos(), request1.info.pos());
    QCOMPARE(job0->type, KisDabRenderingJob::Dab);
    QVERIFY(!job0->originalDevice);
    QVERIFY(!job0->postprocessedDevice);

    cacheInterface->typeOverride = KisDabRenderingJob::Dab;
    KisDabRenderingJobSP job1 = queue.addDab(request2, OPACITY_OPAQUE_F, OPACITY_OPAQUE_F);

    QVERIFY(job1);
    QCOMPARE(job1->seqNo, 1);
    QCOMPARE(job1->generationInfo.info.pos(), request2.info.pos());
    QCOMPARE(job1->type, KisDabRenderingJob::Dab);
    QVERIFY(!job1->originalDevice);
    QVERIFY(!job1->postprocessedDevice);

    cacheInterface->typeOverride = KisDabRenderingJob::Copy;
    KisDabRenderingJobSP job2 = queue.addDab(request2, OPACITY_OPAQUE_F, OPACITY_OPAQUE_F);
    QVERIFY(!job2);

    cacheInterface->typeOverride = KisDabRenderingJob::Copy;
    KisDabRenderingJobSP job3 = queue.addDab(request2, OPACITY_OPAQUE_F, OPACITY_OPAQUE_F);
    QVERIFY(!job3);

    // we only added the dabs, but we haven't completed them yet
    QVERIFY(!queue.hasPreparedDabs());
    QCOMPARE(queue.testingGetQueueSize(), 4);

    QList<KisDabRenderingJobSP > jobs;
    QList<KisRenderedDab> renderedDabs;


    {
        // we've completed job0
        job0->originalDevice = new KisFixedPaintDevice(cs);
        job0->postprocessedDevice = job0->originalDevice;

        jobs = queue.notifyJobFinished(job0->seqNo);
        QVERIFY(jobs.isEmpty());

        // now we should have at least one job in prepared state
        QVERIFY(queue.hasPreparedDabs());

        // take the prepared dabs
        renderedDabs = queue.takeReadyDabs();
        QCOMPARE(renderedDabs.size(), 1);

        // the list should be empty again
        QVERIFY(!queue.hasPreparedDabs());
        QCOMPARE(queue.testingGetQueueSize(), 3);
    }

    {
        // we've completed job1
        job1->originalDevice = new KisFixedPaintDevice(cs);
        job1->postprocessedDevice = job1->originalDevice;

        jobs = queue.notifyJobFinished(job1->seqNo);
        QVERIFY(jobs.isEmpty());

        // now we should have at least one job in prepared state
        QVERIFY(queue.hasPreparedDabs());

        // take the prepared dabs
        renderedDabs = queue.takeReadyDabs();
        QCOMPARE(renderedDabs.size(), 3);

        // since they are copies, they should be the same
        QCOMPARE(renderedDabs[1].device, renderedDabs[0].device);
        QCOMPARE(renderedDabs[2].device, renderedDabs[0].device);

        // the list should be empty again
        QVERIFY(!queue.hasPreparedDabs());

        // we delete all the painted jobs except the latest 'dab' job
        QCOMPARE(queue.testingGetQueueSize(), 1);
    }

    {
        // add one more cached job and take it
        cacheInterface->typeOverride = KisDabRenderingJob::Copy;
        KisDabRenderingJobSP job = queue.addDab(request2, OPACITY_OPAQUE_F, OPACITY_OPAQUE_F);
        QVERIFY(!job);

        // now we should have at least one job in prepared state
        QVERIFY(queue.hasPreparedDabs());

        // take the prepared dabs
        renderedDabs = queue.takeReadyDabs();
        QCOMPARE(renderedDabs.size(), 1);

        // the list should be empty again
        QVERIFY(!queue.hasPreparedDabs());

        // we delete all the painted jobs except the latest 'dab' job
        QCOMPARE(queue.testingGetQueueSize(), 1);
    }

    {
        // add a 'dab' job and complete it

        cacheInterface->typeOverride = KisDabRenderingJob::Dab;
        KisDabRenderingJobSP job = queue.addDab(request1, OPACITY_OPAQUE_F, OPACITY_OPAQUE_F);

        QVERIFY(job);
        QCOMPARE(job->seqNo, 5);
        QCOMPARE(job->generationInfo.info.pos(), request1.info.pos());
        QCOMPARE(job->type, KisDabRenderingJob::Dab);
        QVERIFY(!job->originalDevice);
        QVERIFY(!job->postprocessedDevice);

        // now the queue can be cleared from the completed dabs!
        QCOMPARE(queue.testingGetQueueSize(), 1);

        job->originalDevice = new KisFixedPaintDevice(cs);
        job->postprocessedDevice = job->originalDevice;

        jobs = queue.notifyJobFinished(job->seqNo);
        QVERIFY(jobs.isEmpty());

        // now we should have at least one job in prepared state
        QVERIFY(queue.hasPreparedDabs());

        // take the prepared dabs
        renderedDabs = queue.takeReadyDabs();
        QCOMPARE(renderedDabs.size(), 1);

        // the list should be empty again
        QVERIFY(!queue.hasPreparedDabs());

        // we do not delete the queue of jobs until the next 'dab'
        // job arrives
        QCOMPARE(queue.testingGetQueueSize(), 1);
    }

}

void KisDabRenderingQueueTest::testPostprocessedDabs()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();

    SurrogateCacheInterface *cacheInterface = new SurrogateCacheInterface();

    KisDabRenderingQueue queue(cs, testResourcesFactory);
    queue.setCacheInterface(cacheInterface);

    KoColor color;
    QPointF pos1(10,10);
    QPointF pos2(20,20);
    KisDabShape shape;
    KisPaintInformation pi1(pos1);
    KisPaintInformation pi2(pos2);

    KisDabCacheUtils::DabRequestInfo request1(color, pos1, shape, pi1, 1.0);
    KisDabCacheUtils::DabRequestInfo request2(color, pos2, shape, pi2, 1.0);

    cacheInterface->typeOverride = KisDabRenderingJob::Dab;
    KisDabRenderingJobSP job0 = queue.addDab(request1, OPACITY_OPAQUE_F, OPACITY_OPAQUE_F);

    QVERIFY(job0);
    QCOMPARE(job0->seqNo, 0);
    QCOMPARE(job0->generationInfo.info.pos(), request1.info.pos());
    QCOMPARE(job0->type, KisDabRenderingJob::Dab);
    QVERIFY(!job0->originalDevice);
    QVERIFY(!job0->postprocessedDevice);

    cacheInterface->typeOverride = KisDabRenderingJob::Dab;
    KisDabRenderingJobSP job1 = queue.addDab(request2, OPACITY_OPAQUE_F, OPACITY_OPAQUE_F);

    QVERIFY(job1);
    QCOMPARE(job1->seqNo, 1);
    QCOMPARE(job1->generationInfo.info.pos(), request2.info.pos());
    QCOMPARE(job1->type, KisDabRenderingJob::Dab);
    QVERIFY(!job1->originalDevice);
    QVERIFY(!job1->postprocessedDevice);

    cacheInterface->typeOverride = KisDabRenderingJob::Postprocess;
    KisDabRenderingJobSP job2 = queue.addDab(request2, OPACITY_OPAQUE_F, OPACITY_OPAQUE_F);
    QVERIFY(!job2);

    cacheInterface->typeOverride = KisDabRenderingJob::Postprocess;
    KisDabRenderingJobSP job3 = queue.addDab(request2, OPACITY_OPAQUE_F, OPACITY_OPAQUE_F);
    QVERIFY(!job3);

    // we only added the dabs, but we haven't completed them yet
    QVERIFY(!queue.hasPreparedDabs());
    QCOMPARE(queue.testingGetQueueSize(), 4);

    QList<KisDabRenderingJobSP > jobs;
    QList<KisRenderedDab> renderedDabs;


    {
        // we've completed job0
        job0->originalDevice = new KisFixedPaintDevice(cs);
        job0->postprocessedDevice = job0->originalDevice;

        jobs = queue.notifyJobFinished(job0->seqNo);
        QVERIFY(jobs.isEmpty());

        // now we should have at least one job in prepared state
        QVERIFY(queue.hasPreparedDabs());

        // take the prepared dabs
        renderedDabs = queue.takeReadyDabs();
        QCOMPARE(renderedDabs.size(), 1);

        // the list should be empty again
        QVERIFY(!queue.hasPreparedDabs());
        QCOMPARE(queue.testingGetQueueSize(), 3);
    }

    {
        // we've completed job1
        job1->originalDevice = new KisFixedPaintDevice(cs);
        job1->postprocessedDevice = job1->originalDevice;

        jobs = queue.notifyJobFinished(job1->seqNo);
        QCOMPARE(jobs.size(), 2);

        QCOMPARE(jobs[0]->seqNo, 2);
        QCOMPARE(jobs[1]->seqNo, 3);

        QVERIFY(jobs[0]->originalDevice);
        QVERIFY(!jobs[0]->postprocessedDevice);

        QVERIFY(jobs[1]->originalDevice);
        QVERIFY(!jobs[1]->postprocessedDevice);

        // pretend we have created a postprocessed device
        jobs[0]->postprocessedDevice = new KisFixedPaintDevice(cs);
        jobs[1]->postprocessedDevice = new KisFixedPaintDevice(cs);

        // now we should have at least one job in prepared state
        QVERIFY(queue.hasPreparedDabs());

        // take the prepared dabs
        renderedDabs = queue.takeReadyDabs();
        QCOMPARE(renderedDabs.size(), 1);

        // the list should be empty again
        QVERIFY(!queue.hasPreparedDabs());


        // return back two postprocessed dabs
        QList<KisDabRenderingJobSP > emptyJobs;
        emptyJobs = queue.notifyJobFinished(jobs[0]->seqNo);
        QVERIFY(emptyJobs.isEmpty());

        emptyJobs = queue.notifyJobFinished(jobs[1]->seqNo);
        QVERIFY(emptyJobs.isEmpty());


        // now we should have at least one job in prepared state
        QVERIFY(queue.hasPreparedDabs());

        // take the prepared dabs
        renderedDabs = queue.takeReadyDabs();
        QCOMPARE(renderedDabs.size(), 2);

        // the list should be empty again
        QVERIFY(!queue.hasPreparedDabs());

        // we delete all the painted jobs except the latest 'dab' job
        QCOMPARE(queue.testingGetQueueSize(), 1);
    }

    {
        // add one more postprocessed job and take it
        cacheInterface->typeOverride = KisDabRenderingJob::Postprocess;
        KisDabRenderingJobSP job = queue.addDab(request2, OPACITY_OPAQUE_F, OPACITY_OPAQUE_F);

        QVERIFY(job);
        QCOMPARE(job->seqNo, 4);
        QCOMPARE(job->generationInfo.info.pos(), request2.info.pos());
        ENTER_FUNCTION() << ppVar(job->type);

        QCOMPARE(job->type, KisDabRenderingJob::Postprocess);
        QVERIFY(job->originalDevice);
        QVERIFY(!job->postprocessedDevice);

        // the list should still be empty
        QVERIFY(!queue.hasPreparedDabs());

        // pretend we have created a postprocessed device
        job->postprocessedDevice = new KisFixedPaintDevice(cs);

        // return back the postprocessed dab
        QList<KisDabRenderingJobSP > emptyJobs;
        emptyJobs = queue.notifyJobFinished(job->seqNo);
        QVERIFY(emptyJobs.isEmpty());

        // now we should have at least one job in prepared state
        QVERIFY(queue.hasPreparedDabs());

        // take the prepared dabs
        renderedDabs = queue.takeReadyDabs();
        QCOMPARE(renderedDabs.size(), 1);

        // the list should be empty again
        QVERIFY(!queue.hasPreparedDabs());

        // we delete all the painted jobs except the latest 'dab' job
        QCOMPARE(queue.testingGetQueueSize(), 1);
    }

    {
        // add a 'dab' job and complete it. That will clear the queue!

        cacheInterface->typeOverride = KisDabRenderingJob::Dab;
        KisDabRenderingJobSP job = queue.addDab(request1, OPACITY_OPAQUE_F, OPACITY_OPAQUE_F);

        QVERIFY(job);
        QCOMPARE(job->seqNo, 5);
        QCOMPARE(job->generationInfo.info.pos(), request1.info.pos());
        QCOMPARE(job->type, KisDabRenderingJob::Dab);
        QVERIFY(!job->originalDevice);
        QVERIFY(!job->postprocessedDevice);

        // now the queue can be cleared from the completed dabs!
        QCOMPARE(queue.testingGetQueueSize(), 1);

        job->originalDevice = new KisFixedPaintDevice(cs);
        job->postprocessedDevice = job->originalDevice;

        jobs = queue.notifyJobFinished(job->seqNo);
        QVERIFY(jobs.isEmpty());

        // now we should have at least one job in prepared state
        QVERIFY(queue.hasPreparedDabs());

        // take the prepared dabs
        renderedDabs = queue.takeReadyDabs();
        QCOMPARE(renderedDabs.size(), 1);

        // the list should be empty again
        QVERIFY(!queue.hasPreparedDabs());

        // we do not delete the queue of jobs until the next 'dab'
        // job arrives
        QCOMPARE(queue.testingGetQueueSize(), 1);
    }

}

#include <../KisDabRenderingQueueCache.h>

void KisDabRenderingQueueTest::testRunningJobs()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();

    KisDabRenderingQueueCache *cacheInterface = new KisDabRenderingQueueCache();
    // we do *not* initialize any options yet!

    KisDabRenderingQueue queue(cs, testResourcesFactory);
    queue.setCacheInterface(cacheInterface);


    KoColor color(Qt::red, cs);
    QPointF pos1(10,10);
    QPointF pos2(20,20);
    KisDabShape shape;
    KisPaintInformation pi1(pos1);
    KisPaintInformation pi2(pos2);

    KisDabCacheUtils::DabRequestInfo request1(color, pos1, shape, pi1, 1.0);
    KisDabCacheUtils::DabRequestInfo request2(color, pos2, shape, pi2, 1.0);

    KisDabRenderingJobSP job0 = queue.addDab(request1, OPACITY_OPAQUE_F, OPACITY_OPAQUE_F);

    QVERIFY(job0);
    QCOMPARE(job0->seqNo, 0);
    QCOMPARE(job0->generationInfo.info.pos(), request1.info.pos());
    QCOMPARE(job0->type, KisDabRenderingJob::Dab);

    QVERIFY(!job0->originalDevice);
    QVERIFY(!job0->postprocessedDevice);

    KisDabRenderingJobRunner runner(job0, &queue, 0);
    runner.run();

    QVERIFY(job0->originalDevice);
    QVERIFY(job0->postprocessedDevice);
    QCOMPARE(job0->originalDevice, job0->postprocessedDevice);

    QVERIFY(!job0->originalDevice->bounds().isEmpty());

    KisDabRenderingJobSP job1 = queue.addDab(request2, OPACITY_OPAQUE_F, OPACITY_OPAQUE_F);
    QVERIFY(!job1);

    QList<KisRenderedDab> renderedDabs = queue.takeReadyDabs();
    QCOMPARE(renderedDabs.size(), 2);

    // we did the caching
    QVERIFY(renderedDabs[0].device == renderedDabs[1].device);

    QCOMPARE(renderedDabs[0].offset, QPoint(5,5));
    QCOMPARE(renderedDabs[1].offset, QPoint(15,15));
}

#include "../KisDabRenderingExecutor.h"
#include "KisFakeRunnableStrokeJobsExecutor.h"

void KisDabRenderingQueueTest::testExecutor()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();

    QScopedPointer<KisRunnableStrokeJobsInterface> runner(new KisFakeRunnableStrokeJobsExecutor());

    KisDabRenderingExecutor executor(cs, testResourcesFactory, runner.data());

    KoColor color(Qt::red, cs);
    QPointF pos1(10,10);
    QPointF pos2(20,20);
    KisDabShape shape;
    KisPaintInformation pi1(pos1);
    KisPaintInformation pi2(pos2);

    KisDabCacheUtils::DabRequestInfo request1(color, pos1, shape, pi1, 1.0);
    KisDabCacheUtils::DabRequestInfo request2(color, pos2, shape, pi2, 1.0);

    executor.addDab(request1, 0.5, 0.25);
    executor.addDab(request2, 0.125, 1.0);

    QList<KisRenderedDab> renderedDabs = executor.takeReadyDabs();
    QCOMPARE(renderedDabs.size(), 2);

    // we did the caching
    QVERIFY(renderedDabs[0].device == renderedDabs[1].device);

    QCOMPARE(renderedDabs[0].offset, QPoint(5,5));
    QCOMPARE(renderedDabs[1].offset, QPoint(15,15));

    QCOMPARE(renderedDabs[0].opacity, 0.5);
    QCOMPARE(renderedDabs[0].flow, 0.25);
    QCOMPARE(renderedDabs[1].opacity, 0.125);
    QCOMPARE(renderedDabs[1].flow, 1.0);

}


#include "../kis_brushop.h"
#include "kis_image.h"
#include "kis_node.h"
#include "../KisBrushOpSettings.h"
#include "kis_brush_option.h"
#include "kis_painter.h"
#include "kis_paint_device.h"
#include "kis_transaction.h"
#include "kis_surrogate_undo_adapter.h"
#include "kis_default_bounds_base.h"
#include "KisLocalStrokeResources.h"
#include "KisSharpnessOptionData.h"
#include "kis_pixel_selection.h"
#include "kis_distance_information.h"
#include <KoCompositeOpRegistry.h>
#include "KisRunnableStrokeJobData.h"
#include "KisStrokeJobFailureContext.h"
#include "pagestore/KisPageStoreDiagnostics_p.h"
#include <QScopeGuard>
#include <QSemaphore>
#include <thread>
#include <vector>

namespace {
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


class PartitionBrushOp final : public KisBrushOp
{
public:
    using KisBrushOp::KisBrushOp;
    using KisBrushOp::paintAt;
};
class PartitionBrushBounds final : public KisDefaultBoundsBase
{
public:
    explicit PartitionBrushBounds(WrapAroundAxis axis) : axis(axis) {}
    QRect bounds() const override { return rect; }
    bool wrapAroundMode() const override { return wrapped; }
    WrapAroundAxis wrapAroundModeAxis() const override { return axis; }
    int currentLevelOfDetail() const override { return 0; }
    int currentTime() const override { return time; }
    bool externalFrameActive() const override { return false; }
    void *sourceCookie() const override { return nullptr; }
    WrapAroundAxis axis;
    QRect rect{0, 0, 257, 193};
    bool wrapped = true;
    int time = 0;
};
}

void KisDabRenderingQueueTest::testBrushPagePartition_data()
{
    QTest::addColumn<int>("wrapMode"); QTest::addColumn<int>("mirrorMode");
    for (int wrapMode : {0, 1, 2, 3}) for (int mirrorMode : {0, 1, 2, 3})
        QTest::newRow(qPrintable(QString("wrap%1-mirror%2").arg(wrapMode).arg(mirrorMode))) << wrapMode << mirrorMode;
}

void KisDabRenderingQueueTest::testBrushPagePartition()
{
    QFETCH(int, wrapMode); QFETCH(int, mirrorMode);
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
    const QRect window(-384, -384, 1024, 768);
    QByteArray reference;
    for (bool parallel : {false, true}) {
        KisPaintDeviceSP dev = new KisPaintDevice(cs);
        // Reference uses a different storage alignment. Both images must have
        // identical canvas pixels, independently of partition and worker order.
        dev->moveTo(parallel ? QPoint(13, -11) : QPoint());
        const auto axis = wrapMode == 1 ? WRAPAROUND_HORIZONTAL : wrapMode == 2 ? WRAPAROUND_VERTICAL : WRAPAROUND_BOTH;
        if (wrapMode) { dev->setDefaultBounds(new PartitionBrushBounds(axis)); dev->setSupportsWraparoundMode(true); }
        KisPainter painter(dev); painter.setPaintColor(KoColor(QColor(71, 137, 191, 213), cs));
        painter.setMirrorInformation(QPointF(-17, 23), mirrorMode & 1, mirrorMode & 2);
        KisFakeRunnableStrokeJobsExecutor rendering;
        painter.setRunnableStrokeJobsInterface(&rendering);
        KisResourcesInterfaceSP resources(new KisLocalStrokeResources());
        KisPaintOpSettingsSP settings(new KisBrushOpSettings(resources));
        KisBrushOptionProperties brushOption;
        brushOption.setBrush(KisBrushSP(new KisAutoBrush(new KisCircleMaskGenerator(280, 1.0, 1.0, 1.0, 2, false), 0.0, 0.0)));
        brushOption.writeOptionSetting(settings.data());
        PartitionBrushOp op(settings, &painter, nullptr, nullptr);
        KisTransaction owner(dev);
        if (parallel) {
            QVERIFY(owner.beginStrokeMutation());
            painter.setStrokeMutationOwner(&owner);
        }
        KisStrokeJobFailureContext outerFailure(true);
        op.paintAt(KisPaintInformation(QPointF(80, 85), 1.0));
        op.paintAt(KisPaintInformation(QPointF(210, 140), 1.0));
        QVector<KisRunnableStrokeJobData *> jobs;
        op.doAsynchronousUpdate(jobs); QVERIFY(!jobs.isEmpty()); QVERIFY(!outerFailure.failed());
        const auto deleteJobs = qScopeGuard([&] { qDeleteAll(jobs); });
        int wave = 0, paintingWaves = 0;
        for (int begin = 0; begin < jobs.size();) {
            if (jobs[begin]->sequentiality() != KisStrokeJobData::CONCURRENT) {
                jobs[begin++]->run(); QVERIFY(!outerFailure.failed()); continue;
            }
            int end = begin;
            while (end < jobs.size() && jobs[end]->sequentiality() == KisStrokeJobData::CONCURRENT) ++end;
            const bool painting = (wave++ % 2) == 0;
            if (painting) ++paintingWaves;
            if (!parallel) {
                for (int i = begin; i < end; ++i) jobs[i]->run();
                QVERIFY(!outerFailure.failed());
            } else {
                QSemaphore entered, release;
                std::vector<std::thread> workers; std::vector<int> success(end - begin);
                const auto cleanup = qScopeGuard([&] { release.release(end - begin); for (auto &worker : workers) if (worker.joinable()) worker.join(); });
                for (int i = begin; i < end; ++i) workers.emplace_back([&, i] {
                    KisStrokeJobFailureContext failure(true); KisPageStoreDiagnosticRecorder recorder(true);
                    recorder.setPhaseObserver([&](auto phase) { if (painting && phase == KisPageStoreDiagnosticPhase::PixelOperationBody) { entered.release(); release.acquire(); } });
                    std::as_const(jobs)[i]->run(); success[i - begin] = !failure.failed();
                });
                if (painting) { QVERIFY(end - begin > 1); QVERIFY(entered.tryAcquire(end - begin, 5000)); }
                release.release(end - begin); for (auto &worker : workers) worker.join();
                for (int ok : success) QVERIFY(ok);
            }
            begin = end;
        }
        QCOMPARE(paintingWaves, mirrorMode == 3 ? 4 : mirrorMode ? 2 : 1);
        QVector<KisRunnableStrokeJobData *> drained;
        op.doAsynchronousUpdate(drained); QVERIFY(drained.isEmpty());
        if (parallel) QVERIFY(owner.checkpointStrokeMutation());
        QByteArray actual(window.width() * window.height() * cs->pixelSize(), Qt::Uninitialized);
        dev->readBytes(reinterpret_cast<quint8 *>(actual.data()), window);
        QVERIFY(actual != QByteArray(actual.size(), char(0)));
        if (parallel) {
            if (actual != reference) {
                int count = 0, maximum = 0, first = -1;
                for (int i = 0; i < actual.size(); ++i) if (actual[i] != reference[i]) {
                    if (first < 0) first = i;
                    ++count; maximum = qMax(maximum, qAbs(int(quint8(actual[i])) - int(quint8(reference[i]))));
                }
                qDebug() << "pixel difference" << count << maximum << first << actual.mid(first, 16).toHex() << reference.mid(first, 16).toHex();
            }
            QCOMPARE(actual, reference);
        } else reference = actual;
        painter.setStrokeMutationOwner(nullptr);
        KisSurrogateUndoAdapter undo; QVERIFY(owner.tryCommit(&undo));
        undo.undo(); dev->readBytes(reinterpret_cast<quint8 *>(actual.data()), window); QCOMPARE(actual, QByteArray(actual.size(), char(0)));
        undo.redo(); dev->readBytes(reinterpret_cast<quint8 *>(actual.data()), window); QCOMPARE(actual, reference);
    }
}


void KisDabRenderingQueueTest::testSharpnessLine_data()
{
    QTest::addColumn<int>("wrapMode"); QTest::addColumn<int>("mirrors"); QTest::addColumn<bool>("masked");
    for (int wrap : {0, 1, 2, 3}) for (int mirror : {0, 1, 2, 3}) for (bool mask : {false, true})
        QTest::newRow(qPrintable(QString("wrap%1-mirror%2-mask%3").arg(wrap).arg(mirror).arg(mask))) << wrap << mirror << mask;
}

void KisDabRenderingQueueTest::testSharpnessLine()
{
    QFETCH(int, wrapMode); QFETCH(int, mirrors); QFETCH(bool, masked);
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8(); const QRect window(-384, -384, 1024, 768); const QPoint center(-17, 23);
    KisPaintDeviceSP actual = new KisPaintDevice(cs), reference = new KisPaintDevice(cs);
    for (auto dev : {actual, reference}) {
        dev->moveTo(QPoint(13, -11));
        if (wrapMode) { dev->setDefaultBounds(new PartitionBrushBounds(wrapMode == 1 ? WRAPAROUND_HORIZONTAL : wrapMode == 2 ? WRAPAROUND_VERTICAL : WRAPAROUND_BOTH)); dev->setSupportsWraparoundMode(true); }
    }
    KisSelectionSP selection;
    if (masked) { selection = new KisSelection(); selection->pixelSelection()->select(QRect(-140, -90, 270, 270), 179); }
    KisPainter painter(actual, selection), oracle(reference, selection);
    painter.setPaintColor(KoColor(QColor(73, 139, 197, 211), cs)); painter.setMirrorInformation(center, mirrors & 1, mirrors & 2);
    painter.setOpacityF(0.63); oracle.setOpacityF(0.63);
    KisFakeRunnableStrokeJobsExecutor rendering; painter.setRunnableStrokeJobsInterface(&rendering);
    KisResourcesInterfaceSP resources(new KisLocalStrokeResources()); KisPaintOpSettingsSP settings(new KisBrushOpSettings(resources));
    KisBrushOptionProperties brushOption;
    KisBrushSP brush(new KisAutoBrush(new KisCircleMaskGenerator(1, 1.0, 1.0, 1.0, 2, false), 0.0, 0.0));
    QCOMPARE(brush->width(), 1); QCOMPARE(brush->height(), 1); brushOption.setBrush(brush); brushOption.writeOptionSetting(settings.data());
    KisSharpnessOptionData sharpness; sharpness.isChecked = true; sharpness.write(settings.data());
    PartitionBrushOp op(settings, &painter, nullptr, nullptr);
    KisTransaction owner(actual); QVERIFY(owner.beginStrokeMutation());
    painter.setStrokeMutationOwner(&owner);
    KisStrokeJobFailureContext failure(true); KisDistanceInformation distance;
    KisPaintDeviceSP cache = reference->createCompositionSourceDevice();
    const QPointF points[] = {QPointF(-91, 63), QPointF(142, 123), QPointF(-171, -67)};
    for (int i = 0; i < 2; ++i) {
        op.paintLine(KisPaintInformation(points[i], 1.0), KisPaintInformation(points[i + 1], 1.0), &distance);
        QVERIFY(!failure.failed());
        cache->clear(); KisPainter line(cache); line.setPaintColor(painter.paintColor()); line.drawDDALine(points[i], points[i + 1]);
        const QRect rect = cache->extent();
        oracle.bitBlt(rect.x(), rect.y(), cache, rect.x(), rect.y(), rect.width(), rect.height());
        KoCompositeOp::ParameterInfo params; params.opacity = 0.63;
        legacyMirroredBlits(reference, cache, rect, center, mirrors, selection, params);
    }
    QVector<KisRunnableStrokeJobData *> jobs; op.doAsynchronousUpdate(jobs);
    const auto cleanup = qScopeGuard([&] { qDeleteAll(jobs); });
    QVERIFY(jobs.isEmpty()); // Prove the 1px synchronous branch was exercised.
    QVERIFY(!painter.takeDirtyRegion().isEmpty());
    QVERIFY(owner.checkpointStrokeMutation());
    QByteArray pixels(window.width() * window.height() * 4, Qt::Uninitialized), expected(pixels.size(), Qt::Uninitialized);
    actual->readBytes(reinterpret_cast<quint8 *>(pixels.data()), window); reference->readBytes(reinterpret_cast<quint8 *>(expected.data()), window);
    QVERIFY(pixels != QByteArray(pixels.size(), char(0))); QCOMPARE(pixels, expected);
    painter.setStrokeMutationOwner(nullptr);
    KisSurrogateUndoAdapter undo; QVERIFY(owner.tryCommit(&undo)); undo.undo(); actual->readBytes(reinterpret_cast<quint8 *>(pixels.data()), window); QCOMPARE(pixels, QByteArray(pixels.size(), char(0)));
    undo.redo(); actual->readBytes(reinterpret_cast<quint8 *>(pixels.data()), window); QCOMPARE(pixels, expected);
}


void KisDabRenderingQueueTest::testBrushQueuedContext_data()
{
    QTest::addColumn<int>("change"); QTest::addColumn<int>("mirrors"); QTest::addColumn<bool>("late");
    const char *names[] = {"stable", "offset", "support", "mode", "axis", "border", "time", "painter-target", "manager", "equivalent-bounds", "inactive-wrap"};
    for (int change = 0; change < 11; ++change) for (int mirrors : {0, 3})
        QTest::newRow(qPrintable(QString("%1-mirror%2").arg(names[change]).arg(mirrors))) << change << mirrors << false;
    for (int change : {0, 1, 6, 9}) for (int mirrors : {0, 3})
        QTest::newRow(qPrintable(QString("before-completion-%1-mirror%2").arg(names[change]).arg(mirrors))) << change << mirrors << true;
}

void KisDabRenderingQueueTest::testBrushQueuedContext()
{
    QFETCH(int, change); QFETCH(int, mirrors); QFETCH(bool, late);
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP dev = new KisPaintDevice(cs);
    KisPaintDeviceSP other = new KisPaintDevice(change == 8 ? KoColorSpaceRegistry::instance()->rgb16() : cs);
    auto *bounds = new PartitionBrushBounds(WRAPAROUND_BOTH); dev->setDefaultBounds(bounds);
    dev->setSupportsWraparoundMode(change != 10); dev->moveTo(QPoint(13, -11));
    KisPainter painter(dev); painter.setPaintColor(KoColor(Qt::red, cs));
    painter.setMirrorInformation(QPointF(-17, 23), mirrors & 1, mirrors & 2);
    KisFakeRunnableStrokeJobsExecutor rendering; painter.setRunnableStrokeJobsInterface(&rendering);
    KisResourcesInterfaceSP resources(new KisLocalStrokeResources()); KisPaintOpSettingsSP settings(new KisBrushOpSettings(resources));
    KisBrushOptionProperties brushOption;
    brushOption.setBrush(KisBrushSP(new KisAutoBrush(new KisCircleMaskGenerator(280, 1.0, 1.0, 1.0, 2, false), 0.0, 0.0)));
    brushOption.writeOptionSetting(settings.data()); PartitionBrushOp op(settings, &painter, nullptr, nullptr);
    KisTransaction transaction(dev);
    op.paintAt(KisPaintInformation(QPointF(80, 85), 1.0));
    QVector<KisRunnableStrokeJobData *> jobs; op.doAsynchronousUpdate(jobs); QVERIFY(!jobs.isEmpty());
    const auto cleanup = qScopeGuard([&] { qDeleteAll(jobs); });
    if (late) {
        KisStrokeJobFailureContext failure(true);
        int written = 0; KisPageStoreDiagnosticRecorder recorder(true);
        recorder.setPhaseObserver([&](auto phase) { if (phase == KisPageStoreDiagnosticPhase::PixelOperationBody) ++written; });
        for (int i = 0; i + 1 < jobs.size(); ++i) jobs[i]->run();
        QVERIFY(!failure.failed()); QVERIFY(written > 0);
        QVERIFY(!dev->exactBounds().isEmpty());
        // Multi-dab workers defer dirty reporting to the final sequential job.
        QVERIFY(painter.takeDirtyRegion().isEmpty());
    }
    switch (change) {
    case 1: dev->moveTo(QPoint(14, -11)); break;
    case 2: dev->setSupportsWraparoundMode(false); break;
    case 3: bounds->wrapped = false; break;
    case 4: bounds->axis = WRAPAROUND_HORIZONTAL; break;
    case 5: bounds->rect.translate(1, 0); break;
    case 6: bounds->time = 7; break;
    case 7: painter.begin(other); break;
    case 8: {
        const auto originalManager = dev->dataManager();
        other->setDefaultBounds(bounds); other->setSupportsWraparoundMode(true);
        other->moveTo(QPoint(13, -11));
        dev->prepareClone(other);
        QVERIFY(dev->dataManager() != originalManager);
        break;
    }
    case 9: dev->setDefaultBounds(new PartitionBrushBounds(WRAPAROUND_BOTH)); break;
    case 10: bounds->axis = WRAPAROUND_VERTICAL; bounds->rect.translate(7, 9); break;
    }
    const bool changed = change >= 1 && change <= 8;
    int bodies = 0;
    {
        KisStrokeJobFailureContext failure(true); KisPageStoreDiagnosticRecorder recorder(true);
        recorder.setPhaseObserver([&](auto phase) { if (phase == KisPageStoreDiagnosticPhase::PixelOperationBody) ++bodies; });
        // Execute the already-created real jobs, including the final reporting job.
        for (int i = late ? jobs.size() - 1 : 0; i < jobs.size(); ++i) jobs[i]->run();
        QCOMPARE(failure.failed(), changed);
    }
    if (changed) { QCOMPARE(bodies, 0); QVERIFY(painter.takeDirtyRegion().isEmpty()); }
    else { QVERIFY(late ? bodies == 0 : bodies > 0); QVERIFY(!painter.takeDirtyRegion().isEmpty()); }
    QVERIFY(transaction.tryRevert());
    if (changed) { QVERIFY(dev->exactBounds().isEmpty()); QVERIFY(other->exactBounds().isEmpty()); }
}

SIMPLE_TEST_MAIN(KisDabRenderingQueueTest)

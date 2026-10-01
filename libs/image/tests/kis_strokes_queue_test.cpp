/*
 *  SPDX-FileCopyrightText: 2011 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_strokes_queue_test.h"
#include <simpletest.h>
#include <QScopeGuard>
#include <QSemaphore>
#include <atomic>
#include "KisRunnableBasedStrokeStrategy.h"
#include "KisRunnableStrokeJobData.h"
#include "pagestore/KisTiledDataManagerPageStoreBackend.h"
#include "pagestore/KisPageStore.h"

#include "kistest.h"

#include "scheduler_utils.h"
#include "kis_strokes_queue.h"
#include "kis_updater_context.h"
#include "kis_update_job_item.h"
#include "kis_merge_walker.h"


namespace {
class CheckpointStrokeStrategy : public KisRunnableBasedStrokeStrategy
{
public:
    CheckpointStrokeStrategy() : KisRunnableBasedStrokeStrategy(QLatin1String("checkpoint"))
    {
        enableJob(JOB_DOSTROKE, true, KisStrokeJobData::CONCURRENT);
        enableJob(JOB_FINISH);
        enableJob(JOB_CANCEL);
    }
    using KisStrokeStrategy::addCheckpointJob;
    using KisStrokeStrategy::addMutatedJob;
    void finishStrokeCallback() override { if (finish) finish(); }
    void cancelStrokeCallback() override { if (cancel) cancel(); }
    std::function<void()> finish, cancel;
};
}

void KisStrokesQueueTest::testCheckpointCapturesAdmittedJobs_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("barrier"); QTest::addColumn<bool>("ended");
    for (int bpp : {1, 4, 8, 16}) for (bool barrier : {false, true}) for (bool ended : {false, true})
        QTest::newRow(qPrintable(QString("B%1-barrier%2-ended%3").arg(bpp).arg(barrier).arg(ended)))
            << bpp << barrier << ended;
}

void KisStrokesQueueTest::testCheckpointCapturesAdmittedJobs()
{
    QFETCH(int, bpp); QFETCH(bool, barrier); QFETCH(bool, ended);
    KisTiledDataManagerPageStoreBackend backend;
    const QByteArray initial(bpp, char(0x2a)); QString error;
    QVERIFY2(backend.configure(bpp, reinterpret_cast<const quint8 *>(initial.constData()), &error), qPrintable(error));
    auto *store = backend.store(); const auto baseline = store->mutationStatistics();
    const auto tx = store->beginCurrentTransaction(); auto scope = store->beginMutation(tx);
    QVERIFY(scope.isActive());
    KisCapturedReadView frozen;
    KisStrokesQueue queue; KisUpdaterContext context(2);
    auto *strategy = new CheckpointStrokeStrategy;
    const auto id = queue.startStroke(strategy);
    std::atomic<int> failures{0}, cuts{0}, finished{0}, after{0};
    QSemaphore entered, release, requested;
    const auto cleanup = qScopeGuard([&] {
        release.release(4); context.waitForDone();
        if (!id.isNull() && !id.toStrongRef()->isEnded()) queue.endStroke(id);
        for (int i = 0; i < 32 && !queue.isEmpty(); ++i) { queue.processQueue(context, false); context.waitForDone(); }
        scope.cancel(); store->abort(tx); frozen = {}; scope = {};
    });
    const auto write = [&](int x, quint8 value) {
        auto execution = scope.borrowExecution(backend.surface(), {{x, 0}});
        auto guard = execution.beginWrite({backend.surface(), {x, 0}});
        if (!guard.isValid()) { ++failures; return; }
        std::memset(guard.data(), value, size_t(bpp)); guard = {};
        if (!execution.finish()) ++failures;
    };
    strategy->finish = [&] { ++finished; };
    queue.addJob(id, new KisRunnableStrokeJobData([&] {
        auto execution = scope.borrowExecution(backend.surface(), {{0, 0}});
        auto guard = execution.beginWrite({backend.surface(), {0, 0}});
        if (guard.isValid()) std::memset(guard.data(), 0x51, size_t(bpp)); else ++failures;
        const bool accepted = strategy->addCheckpointJob(new KisRunnableStrokeJobData([&] {
            if (cuts.fetch_add(1) || after.load()) ++failures;
            frozen = scope.checkpointForRead();
            if (!frozen.isValid()) { ++failures; return; }
            for (int x = 0; x != 5; ++x) {
                auto read = frozen.readResidentPage({backend.surface(), {x, 0}});
                if (!read.isValid() || QByteArray(static_cast<const char *>(read.data()), bpp) != QByteArray(bpp, char(0x51 + x)))
                    ++failures;
            }
        }, barrier ? KisStrokeJobData::BARRIER : KisStrokeJobData::SEQUENTIAL));
        if (!accepted) ++failures;
        requested.release(); entered.release(); release.acquire();
        guard = {}; if (!execution.finish()) ++failures;
    }, KisStrokeJobData::CONCURRENT));
    queue.addJob(id, new KisRunnableStrokeJobData([&] {
        auto execution = scope.borrowExecution(backend.surface(), {{1, 0}});
        auto guard = execution.beginWrite({backend.surface(), {1, 0}});
        if (guard.isValid()) std::memset(guard.data(), 0x52, size_t(bpp)); else ++failures;
        entered.release(); release.acquire(); guard = {};
        if (!execution.finish()) ++failures;
        // These are produced after the checkpoint request and recurse through
        // the real queue; both must still precede its exact view.
        strategy->addMutatedJob(new KisRunnableStrokeJobData([&] {
            write(3, 0x54);
            strategy->addMutatedJob(new KisRunnableStrokeJobData([&] { write(4, 0x55); }));
        }));
    }, KisStrokeJobData::CONCURRENT));
    queue.addJob(id, new KisRunnableStrokeJobData([&] { write(2, 0x53); }));
    if (ended) queue.endStroke(id);
    queue.processQueue(context, false);
    QVERIFY(entered.tryAcquire(2, 5000)); QVERIFY(requested.tryAcquire(1, 5000));
    QCOMPARE(cuts.load(), 0); QCOMPARE(finished.load(), 0);
    if (!ended) {
        queue.addJob(id, new KisRunnableStrokeJobData([&] {
            if (cuts.load() != 1 || !frozen.isValid()) ++failures;
            write(0, 0x71); ++after;
            auto old = frozen.readResidentPage({backend.surface(), {0, 0}});
            if (!old.isValid() || QByteArray(static_cast<const char *>(old.data()), bpp) != QByteArray(bpp, char(0x51))) ++failures;
        }, KisStrokeJobData::CONCURRENT));
        queue.endStroke(id);
    }
    queue.processQueue(context, false); // a full worker set cannot dispatch the cut
    QCOMPARE(cuts.load(), 0);
    release.release(2); context.waitForDone();
    for (int i = 0; i < 32 && !queue.isEmpty(); ++i) { queue.processQueue(context, false); context.waitForDone(); }
    QVERIFY(queue.isEmpty()); QCOMPARE(failures.load(), 0); QCOMPARE(cuts.load(), 1);
    QCOMPARE(finished.load(), 1); QCOMPARE(after.load(), ended ? 0 : 1);
    QVERIFY(scope.isActive()); QVERIFY(scope.seal());
    QVERIFY(store->commit(tx, store->preparedPages(tx)).isValid());
    const auto stats = store->mutationStatistics();
    QCOMPARE(stats.operationSessionsCreated - baseline.operationSessionsCreated, quint64(1));
    QCOMPARE(stats.generationsReserved - baseline.generationsReserved, quint64(ended ? 5 : 6));
    QCOMPARE(stats.writablePinsAcquired - baseline.writablePinsAcquired, quint64(ended ? 5 : 6));
    QCOMPARE(stats.writablePinsReleased - baseline.writablePinsReleased, quint64(ended ? 5 : 6));
    frozen = {}; scope = {}; QVERIFY(store->closeSession());
}

void KisStrokesQueueTest::testCheckpointCancellation_data()
{
    QTest::addColumn<bool>("fail"); QTest::addColumn<bool>("ended");
    for (bool fail : {false, true}) for (bool ended : {false, true})
        QTest::newRow(qPrintable(QString("fail%1-ended%2").arg(fail).arg(ended))) << fail << ended;
}

void KisStrokesQueueTest::testCheckpointCancellation()
{
    QFETCH(bool, fail); QFETCH(bool, ended);
    KisStrokesQueue queue; KisUpdaterContext context(2);
    auto *strategy = new CheckpointStrokeStrategy; const auto id = queue.startStroke(strategy);
    QSemaphore requested, release;
    std::atomic<int> cuts{0}, children{0}, cancelled{0}, finished{0}, failures{0};
    const auto cleanup = qScopeGuard([&] {
        release.release(2); context.waitForDone();
        if (!id.isNull() && !id.toStrongRef()->isEnded()) queue.endStroke(id);
        for (int i = 0; i < 16 && !queue.isEmpty(); ++i) { queue.processQueue(context, false); context.waitForDone(); }
    });
    strategy->finish = [&] { ++finished; };
    strategy->cancel = [&] { if (children.load() != 1) ++failures; ++cancelled; };
    queue.addJob(id, new KisRunnableStrokeJobData([&] {
        if (!strategy->addCheckpointJob(new KisRunnableStrokeJobData([&] { ++cuts; }))) ++failures;
        requested.release(); release.acquire();
        auto *required = new KisRunnableStrokeJobData([&] { ++children; }); required->setCancellable(false);
        strategy->addMutatedJob(required);
        if (strategy->addCheckpointJob(new KisRunnableStrokeJobData([&] { ++cuts; }))) ++failures;
    }, KisStrokeJobData::CONCURRENT));
    if (ended) queue.endStroke(id);
    queue.processQueue(context, false); QVERIFY(requested.tryAcquire(1, 5000));
    if (fail) queue.failStroke(id, QStringLiteral("checkpoint cancellation test")); else QVERIFY(queue.cancelStroke(id));
    if (fail && !ended) queue.endStroke(id);
    release.release(); context.waitForDone();
    for (int i = 0; i < 16 && !queue.isEmpty(); ++i) { queue.processQueue(context, false); context.waitForDone(); }
    QVERIFY(queue.isEmpty()); QCOMPARE(cuts.load(), 0); QCOMPARE(children.load(), 1);
    QCOMPARE(cancelled.load(), 1); QCOMPARE(finished.load(), 0); QCOMPARE(failures.load(), 0);
}

void KisStrokesQueueTest::testCheckpointRejectsRetainedFinishedStroke()
{
    KisStrokesQueue queue; KisUpdaterContext context(1);
    const auto id = queue.startStroke(new CheckpointStrokeStrategy);
    const auto retained = id.toStrongRef(); // like an already completed LoD buddy
    queue.endStroke(id);
    for (int i = 0; i < 4 && !queue.isEmpty(); ++i) { queue.processQueue(context, false); context.waitForDone(); }
    QVERIFY(queue.isEmpty()); QVERIFY(!id.isNull());
    int destroyed = 0;
    struct CountedData : KisStrokeJobData {
        CountedData(int &count) : count(count) {} ~CountedData() override { ++count; } int &count;
    };
    QVERIFY(!queue.addCheckpointJob(id, new CountedData(destroyed)));
    QVERIFY(!queue.addCheckpointJob({}, new CountedData(destroyed)));
    QCOMPARE(destroyed, 2); QVERIFY(!retained->hasJobs());
}

void KisStrokesQueueTest::testSequentialJobs()
{
    KisStrokesQueue queue;
    KisStrokeId id = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("tri_"), false));
    queue.addJob(id, new KisStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.addJob(id, new KisStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.addJob(id, new KisStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id);

    KisTestableUpdaterContext context(2);
    QVector<KisUpdateJobItem*> jobs;

    queue.processQueue(context, false);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "tri_init");
    VERIFY_EMPTY(jobs[1]);

    context.clear();
    queue.processQueue(context, false);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "tri_dab");
    COMPARE_NAME(jobs[1], "tri_dab");

    context.clear();
    queue.processQueue(context, false);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "tri_dab");
    VERIFY_EMPTY(jobs[1]);

    context.clear();
    queue.processQueue(context, false);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "tri_finish");
    VERIFY_EMPTY(jobs[1]);
}

void KisStrokesQueueTest::testConcurrentSequentialBarrier()
{
    KisStrokesQueue queue;
    KisStrokeId id = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("tri_"), false));
    queue.addJob(id, new KisStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.addJob(id, new KisStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id);

    // make the number of threads higher
    KisTestableUpdaterContext context(3);
    QVector<KisUpdateJobItem*> jobs;

    queue.processQueue(context, false);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "tri_init");
    VERIFY_EMPTY(jobs[1]);

    context.clear();
    queue.processQueue(context, false);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "tri_dab");
    COMPARE_NAME(jobs[1], "tri_dab");

    context.clear();
    queue.processQueue(context, false);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "tri_finish");
    VERIFY_EMPTY(jobs[1]);
}

void KisStrokesQueueTest::testExclusiveStrokes()
{
    KisStrokesQueue queue;
    KisStrokeId id = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("excl_"), true));
    queue.addJob(id, new KisStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.addJob(id, new KisStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.addJob(id, new KisStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id);

    // well, this walker is not initialized... but who cares?
    KisBaseRectsWalkerSP walker = new KisMergeWalker(QRect());

    KisTestableUpdaterContext context(2);
    QVector<KisUpdateJobItem*> jobs;

    context.addMergeJob(walker);
    queue.processQueue(context, false);

    jobs = context.getJobs();
    COMPARE_WALKER(jobs[0], walker);
    VERIFY_EMPTY(jobs[1]);
    QCOMPARE(queue.needsExclusiveAccess(), true);

    context.clear();
    queue.processQueue(context, false);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "excl_init");
    VERIFY_EMPTY(jobs[1]);
    QCOMPARE(queue.needsExclusiveAccess(), true);

    context.clear();
    queue.processQueue(context, false);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "excl_dab");
    COMPARE_NAME(jobs[1], "excl_dab");
    QCOMPARE(queue.needsExclusiveAccess(), true);

    context.clear();
    context.addMergeJob(walker);
    queue.processQueue(context, false);

    COMPARE_WALKER(jobs[0], walker);
    VERIFY_EMPTY(jobs[1]);
    QCOMPARE(queue.needsExclusiveAccess(), true);

    context.clear();
    queue.processQueue(context, false);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "excl_dab");
    VERIFY_EMPTY(jobs[1]);
    QCOMPARE(queue.needsExclusiveAccess(), true);

    context.clear();
    queue.processQueue(context, false);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "excl_finish");
    VERIFY_EMPTY(jobs[1]);
    QCOMPARE(queue.needsExclusiveAccess(), true);

    context.clear();
    queue.processQueue(context, false);

    QCOMPARE(queue.needsExclusiveAccess(), false);
}

void KisStrokesQueueTest::testBarrierStrokeJobs()
{
    KisStrokesQueue queue;
    KisStrokeId id = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("nor_"), false));
    queue.addJob(id, new KisStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.addJob(id, new KisStrokeJobData(KisStrokeJobData::BARRIER));
    queue.addJob(id, new KisStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id);

    // yes, this walker is not initialized again... but who cares?
    KisBaseRectsWalkerSP walker = new KisMergeWalker(QRect());
    bool externalJobsPending = false;

    KisTestableUpdaterContext context(3);
    QVector<KisUpdateJobItem*> jobs;

    queue.processQueue(context, externalJobsPending);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "nor_init");
    VERIFY_EMPTY(jobs[1]);
    VERIFY_EMPTY(jobs[2]);

    context.clear();

    queue.processQueue(context, externalJobsPending);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "nor_dab");
    VERIFY_EMPTY(jobs[1]);
    VERIFY_EMPTY(jobs[2]);

    // Now some updates has come...
    context.addMergeJob(walker);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "nor_dab");
    COMPARE_WALKER(jobs[1], walker);
    VERIFY_EMPTY(jobs[2]);

    // No difference for the queue
    queue.processQueue(context, externalJobsPending);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "nor_dab");
    COMPARE_WALKER(jobs[1], walker);
    VERIFY_EMPTY(jobs[2]);

    // Even more updates has come...
    externalJobsPending = true;

    // Still no difference for the queue
    queue.processQueue(context, externalJobsPending);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "nor_dab");
    COMPARE_WALKER(jobs[1], walker);
    VERIFY_EMPTY(jobs[2]);

    // Now clear the context
    context.clear();

    // And still no difference for the queue
    queue.processQueue(context, externalJobsPending);

    jobs = context.getJobs();
    VERIFY_EMPTY(jobs[0]);
    VERIFY_EMPTY(jobs[1]);
    VERIFY_EMPTY(jobs[2]);

    // Process the last update...
    context.addMergeJob(walker);
    externalJobsPending = false;

    // Yep, the queue is still waiting
    queue.processQueue(context, externalJobsPending);

    jobs = context.getJobs();
    COMPARE_WALKER(jobs[0], walker);
    VERIFY_EMPTY(jobs[1]);
    VERIFY_EMPTY(jobs[2]);

    context.clear();

    // Finally, we can do our work. Barrier job is executed alone
    queue.processQueue(context, externalJobsPending);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "nor_dab");
    VERIFY_EMPTY(jobs[1]);
    VERIFY_EMPTY(jobs[2]);

    // Barrier job has finished
    context.clear();

    jobs = context.getJobs();
    VERIFY_EMPTY(jobs[0]);
    VERIFY_EMPTY(jobs[1]);
    VERIFY_EMPTY(jobs[2]);

    // fetch the last (concurrent) one
    queue.processQueue(context, externalJobsPending);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "nor_dab");
    VERIFY_EMPTY(jobs[1]);
    VERIFY_EMPTY(jobs[2]);

    context.clear();

    // finish the stroke
    queue.processQueue(context, externalJobsPending);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "nor_finish");
    VERIFY_EMPTY(jobs[1]);
    VERIFY_EMPTY(jobs[2]);

    context.clear();
}

void KisStrokesQueueTest::testStrokesOverlapping()
{
    KisStrokesQueue queue;
    KisStrokeId id = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("1_"), false, true));
    queue.addJob(id, 0);

    // comment out this line to catch an assert
    queue.endStroke(id);

    id = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("2_"), false, true));
    queue.addJob(id, 0);
    queue.endStroke(id);

    // uncomment this line to catch an assert
    // queue.addJob(id, 0);

    KisTestableUpdaterContext context(2);
    QVector<KisUpdateJobItem*> jobs;

    queue.processQueue(context, false);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "1_dab");
    VERIFY_EMPTY(jobs[1]);

    context.clear();
    queue.processQueue(context, false);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "2_dab");
    VERIFY_EMPTY(jobs[1]);
}

void KisStrokesQueueTest::testImmediateCancel()
{
    KisStrokesQueue queue;
    KisTestableUpdaterContext context(2);

    KisStrokeId id = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("1_"), false, false));
    queue.cancelStroke(id);

    // this should not crash
    queue.processQueue(context, false);
}

void KisStrokesQueueTest::testOpenedStrokeCounter()
{
    KisStrokesQueue queue;

    QVERIFY(!queue.hasOpenedStrokes());
    KisStrokeId id0 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("0")));
    QVERIFY(queue.hasOpenedStrokes());
    KisStrokeId id1 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("1")));
    QVERIFY(queue.hasOpenedStrokes());
    queue.endStroke(id0);
    QVERIFY(queue.hasOpenedStrokes());
    queue.endStroke(id1);
    QVERIFY(!queue.hasOpenedStrokes());

    KisTestableUpdaterContext context(2);
    queue.processQueue(context, false); context.clear();
    queue.processQueue(context, false); context.clear();
    queue.processQueue(context, false); context.clear();
    queue.processQueue(context, false); context.clear();
}

void KisStrokesQueueTest::testAsyncCancelWhileOpenedStroke()
{
    KisStrokesQueue queue;
    KisStrokeId id = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("nor_"), false));
    queue.addJob(id, 0);
    queue.addJob(id, 0);
    queue.addJob(id, 0);

    // no async cancelling until the stroke is ended by the owner
    QVERIFY(!queue.tryCancelCurrentStrokeAsync());

    queue.endStroke(id);

    QVERIFY(queue.tryCancelCurrentStrokeAsync());

    bool externalJobsPending = false;

    KisTestableUpdaterContext context(3);
    QVector<KisUpdateJobItem*> jobs;

    queue.processQueue(context, externalJobsPending);

    // no? really?
    jobs = context.getJobs();
    VERIFY_EMPTY(jobs[0]);
    VERIFY_EMPTY(jobs[1]);
    VERIFY_EMPTY(jobs[2]);
}

void KisStrokesQueueTest::testWorkerFailureKeepsInputHandle()
{
    KisStrokesQueue queue;
    KisTestableUpdaterContext context(2);
    KisStrokeId id = queue.startStroke(new KisTestingStrokeStrategy());
    queue.processQueue(context, false); context.clear(); // init
    queue.failStroke(id, QStringLiteral("injected worker failure"));
    queue.processQueue(context, false); context.clear(); // cancel
    queue.processQueue(context, false);
    QVERIFY(!id.isNull());
    QVERIFY(queue.hasOpenedStrokes());
    queue.addJob(id, new KisStrokeJobData());
    QVERIFY(!id.toStrongRef()->hasJobs());
    queue.endStroke(id);
    QVERIFY(!queue.hasOpenedStrokes());
    queue.cancelStroke(id); // do not decrement the counter a second time
    QVERIFY(!queue.hasOpenedStrokes());
    queue.processQueue(context, false);
    QVERIFY(queue.isEmpty());
    QVERIFY(id.isNull());
}

struct KisStrokesQueueTest::LodStrokesQueueTester {

    LodStrokesQueueTester(bool real = false)
        : fakeContext(2),
          realContext(2),
          context(!real ? fakeContext : realContext)
    {
        queue.setSuspendResumeUpdatesStrokeStrategyFactory(
            []() {
                KisSuspendResumePair suspend(
                    new KisTestingStrokeStrategy(QLatin1String("susp_u_"), false, true, true),
                    QList<KisStrokeJobData*>());

                KisSuspendResumePair resume(
                    new KisTestingStrokeStrategy(QLatin1String("resu_u_"), false, true, true),
                    QList<KisStrokeJobData*>());

                return std::make_pair(suspend, resume);
            });

        queue.setLod0ToNStrokeStrategyFactory(
            [] (bool forgettable) {
                Q_UNUSED(forgettable);
                return KisLodSyncPair(
                    new KisTestingStrokeStrategy(QLatin1String("sync_u_"), false, true, true),
                    QList<KisStrokeJobData*>());
            });
    }

    KisStrokesQueue queue;

    KisTestableUpdaterContext fakeContext;
    KisUpdaterContext realContext;
    KisUpdaterContext &context;
    QVector<KisUpdateJobItem*> jobs;

    void processQueueNoAdd() {
        if (&context != &fakeContext) return;

        fakeContext.clear();

        jobs = fakeContext.getJobs();
        VERIFY_EMPTY(jobs[0]);
        VERIFY_EMPTY(jobs[1]);
    }

    void processQueueNoContextClear() {
        queue.processQueue(context, false);

        if (&context == &realContext) {
            context.waitForDone();
        }
    }

    void processQueue() {
        processQueueNoAdd();
        queue.processQueue(context, false);

        if (&context == &realContext) {
            context.waitForDone();
        }
    }

    void checkNothing() {
        KIS_ASSERT(&context == &fakeContext);

        jobs = fakeContext.getJobs();
        VERIFY_EMPTY(jobs[0]);
        VERIFY_EMPTY(jobs[1]);
    }

    void checkJobs(const QStringList &list) {
        KIS_ASSERT(&context == &fakeContext);

        jobs = fakeContext.getJobs();

        for (int i = 0; i < 2; i++) {
            if (list.size() <= i) {
                VERIFY_EMPTY(jobs[i]);
            } else {
                QVERIFY(jobs[i]->isRunning());
                COMPARE_NAME(jobs[i], list[i]);
            }
        }

        QCOMPARE(queue.needsExclusiveAccess(), false);
    }

    void checkOnlyJob(const QString &name) {
        KIS_ASSERT(&context == &fakeContext);

        jobs = fakeContext.getJobs();
        COMPARE_NAME(jobs[0], name);
        VERIFY_EMPTY(jobs[1]);
        QCOMPARE(queue.needsExclusiveAccess(), false);
    }

    void checkOnlyExecutedJob(const QString &name) {
        realContext.waitForDone();
        QVERIFY(!globalExecutedDabs.isEmpty());
        QCOMPARE(globalExecutedDabs[0], name);

        QCOMPARE(globalExecutedDabs.size(), 1);
        globalExecutedDabs.clear();
    }

    void checkExecutedJobs(const QStringList &list) {
        realContext.waitForDone();

        QCOMPARE(globalExecutedDabs, list);
        globalExecutedDabs.clear();
    }

    void checkNothingExecuted() {
        realContext.waitForDone();
        QVERIFY(globalExecutedDabs.isEmpty());
    }
};

void KisStrokesQueueTest::testWorkerFailureLod_data()
{
    QTest::addColumn<bool>("failBuddy");
    QTest::newRow("lod0-to-lodn") << false;
    QTest::newRow("lodn-to-lod0") << true;
}

void KisStrokesQueueTest::testWorkerFailureLod()
{
    QFETCH(bool, failBuddy);
    LodStrokesQueueTester t;
    t.queue.setLodPreferences(KisLodPreferences(2));
    t.processQueue();
    KisStrokeId id = t.queue.startStroke(new KisTestingStrokeStrategy());
    auto stroke = id.toStrongRef();
    auto buddy = stroke->lodBuddy();
    QVERIFY(buddy);
    t.queue.failStroke(failBuddy ? KisStrokeId(buddy) : id, QStringLiteral("injected LoD failure"));
    t.queue.addJob(id, new KisTestingStrokeJobData());
    QVERIFY(!stroke->hasJobs());
    QVERIFY(!buddy->hasJobs());
    QVERIFY(t.queue.hasOpenedStrokes());
    t.queue.endStroke(id);
    QVERIFY(!t.queue.hasOpenedStrokes());
    for (int i = 0; i < 20 && !t.queue.isEmpty(); ++i) t.processQueue();
    t.processQueueNoAdd();
    QVERIFY(t.queue.isEmpty());
}


void KisStrokesQueueTest::testWorkerFailureAfterLodBuddyFinished()
{
    LodStrokesQueueTester t;
    t.queue.setLodPreferences(KisLodPreferences(2));
    t.processQueue();
    KisStrokeId id = t.queue.startStroke(new KisTestingStrokeStrategy());
    auto stroke = id.toStrongRef();
    auto buddy = stroke->lodBuddy();
    QVERIFY(buddy);
    t.queue.endStroke(id);
    for (int i = 0; i < 30 && !stroke->isInitialized(); ++i) t.processQueue();
    QVERIFY(stroke->isInitialized());
    QVERIFY(buddy->isEnded());
    QVERIFY(!buddy->hasJobs());
    t.queue.failStroke(id, QStringLiteral("failure after LoDN completion"));
    QVERIFY(!buddy->hasJobs()); // retained by LoD0, but no longer schedulable
    for (int i = 0; i < 30 && !t.queue.isEmpty(); ++i) t.processQueue();
    t.processQueueNoAdd();
    QVERIFY(t.queue.isEmpty());
}

void KisStrokesQueueTest::testStrokesLevelOfDetail()
{
    LodStrokesQueueTester t;
    KisStrokesQueue &queue = t.queue;

    // create a stroke with LOD0 + LOD2
    queue.setLodPreferences(KisLodPreferences(2));

    // process sync-lodn-planes stroke
    t.processQueue();
    t.checkOnlyJob("sync_u_init");

    KisStrokeId id2 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("lod_"), false, true));
    queue.addJob(id2, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id2);

    // create a update with LOD == 0 (default one)
    // well, this walker is not initialized... but who cares?
    KisBaseRectsWalkerSP walker = new KisMergeWalker(QRect());

    KisTestableUpdaterContext context(2);
    QVector<KisUpdateJobItem*> jobs;

    context.addMergeJob(walker);
    queue.processQueue(context, false);

    jobs = context.getJobs();
    COMPARE_WALKER(jobs[0], walker);
    VERIFY_EMPTY(jobs[1]);
    QCOMPARE(queue.needsExclusiveAccess(), false);

    context.clear();

    jobs = context.getJobs();
    VERIFY_EMPTY(jobs[0]);
    VERIFY_EMPTY(jobs[1]);

    context.clear();
    queue.processQueue(context, false);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "clone2_lod_dab");
    VERIFY_EMPTY(jobs[1]);
    QCOMPARE(queue.needsExclusiveAccess(), false);

    // walker of a different LOD must not be allowed
    QCOMPARE(context.isJobAllowed(walker), false);

    context.clear();
    context.addMergeJob(walker);
    queue.processQueue(context, false);

    jobs = context.getJobs();
    COMPARE_WALKER(jobs[0], walker);
    COMPARE_NAME(jobs[1], "susp_u_init");
    QCOMPARE(queue.needsExclusiveAccess(), false);

    context.clear();
    queue.processQueue(context, false);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "lod_dab");
    VERIFY_EMPTY(jobs[1]);
    QCOMPARE(queue.needsExclusiveAccess(), false);

    context.clear();
    queue.processQueue(context, false);

    jobs = context.getJobs();
    COMPARE_NAME(jobs[0], "resu_u_init");
    VERIFY_EMPTY(jobs[1]);
    QCOMPARE(queue.needsExclusiveAccess(), false);

    context.clear();
}

void KisStrokesQueueTest::testStrokeWithMixedLodJobs()
{
    LodStrokesQueueTester t;
    KisStrokesQueue &queue = t.queue;

    // create a stroke with LOD0 + LOD2
    queue.setLodPreferences(KisLodPreferences(2));

    // process sync-lodn-planes stroke
    t.processQueue();
    t.checkOnlyJob("sync_u_init");

    KisStrokeId id2 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("lod_"), false, true, false, false, true));

    KisStrokeJobData *data = 0;
    data = new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT,
                                       KisStrokeJobData::NORMAL,
                                       false, "job0_l0");
    queue.addJob(id2, data);

    data = new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT,
                                       KisStrokeJobData::NORMAL,
                                       false, "job1_l0");
    queue.addJob(id2, data);

    data = new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT,
                                       KisStrokeJobData::NORMAL,
                                       false, "job2_l0");
    queue.addJob(id2, data);

    data = new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT,
                                       KisStrokeJobData::NORMAL,
                                       false, "job3_l2");
    data->setLevelOfDetailOverride(2);
    queue.addJob(id2, data);

    data = new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT,
                                       KisStrokeJobData::NORMAL,
                                       false, "job4_l0");
    queue.addJob(id2, data);

    queue.endStroke(id2);

    t.processQueue();
    t.checkJobs({"lod_dab_job0_l0", "lod_dab_job1_l0"});
    QCOMPARE(t.context.currentLevelOfDetail(), 0);

    t.processQueue();
    t.checkOnlyJob("lod_dab_job2_l0");
    QCOMPARE(t.context.currentLevelOfDetail(), 0);

    t.processQueue();
    t.checkOnlyJob("lod_dab_job3_l2");
    QCOMPARE(t.context.currentLevelOfDetail(), 2);

    t.processQueue();
    t.checkOnlyJob("lod_dab_job4_l0");
    QCOMPARE(t.context.currentLevelOfDetail(), 0);
}

void KisStrokesQueueTest::testMultipleLevelOfDetailStrokes()
{
    LodStrokesQueueTester t;
    KisStrokesQueue &queue = t.queue;

    // create a stroke with LOD0 + LOD2
    queue.setLodPreferences(KisLodPreferences(2));

    KisStrokeId id1 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("lod1_"), false, true));
    queue.addJob(id1, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id1);

    KisStrokeId id2 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("lod2_"), false, true));
    queue.addJob(id2, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id2);

    t.processQueue();
    t.checkOnlyJob("sync_u_init");

    t.processQueue();
    t.checkOnlyJob("clone2_lod1_dab");

    t.processQueue();
    t.checkOnlyJob("clone2_lod2_dab");

    t.processQueue();
    t.checkOnlyJob("susp_u_init");

    t.processQueue();
    t.checkOnlyJob("lod1_dab");

    t.processQueue();
    t.checkOnlyJob("lod2_dab");

    t.processQueue();
    t.checkOnlyJob("resu_u_init");
}

void KisStrokesQueueTest::testMultipleLevelOfDetailAfterLegacy()
{
    LodStrokesQueueTester t;
    KisStrokesQueue &queue = t.queue;

    // create a stroke with LOD0 + LOD2
    queue.setLodPreferences(KisLodPreferences(2));

    KisStrokeId id0 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("leg0_"), false, true, false, false, true));
    queue.addJob(id0, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id0);

    KisStrokeId id1 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("lod1_"), false, true));
    queue.addJob(id1, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id1);

    KisStrokeId id2 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("lod2_"), false, true));
    queue.addJob(id2, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id2);

    t.processQueue();
    t.checkOnlyJob("sync_u_init");

    t.processQueue();
    t.checkOnlyJob("leg0_dab");

    t.processQueue();
    t.checkOnlyJob("sync_u_init");

    t.processQueue();
    t.checkOnlyJob("clone2_lod1_dab");

    t.processQueue();
    t.checkOnlyJob("clone2_lod2_dab");

    t.processQueue();
    t.checkOnlyJob("susp_u_init");

    t.processQueue();
    t.checkOnlyJob("lod1_dab");

    t.processQueue();
    t.checkOnlyJob("lod2_dab");

    t.processQueue();
    t.checkOnlyJob("resu_u_init");

}

void KisStrokesQueueTest::testMultipleLevelOfDetailMixedLegacy()
{
    LodStrokesQueueTester t;
    KisStrokesQueue &queue = t.queue;

    // create a stroke with LOD0 + LOD2
    queue.setLodPreferences(KisLodPreferences(2));

    KisStrokeId id0 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("lod0_"), false, true));
    queue.addJob(id0, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id0);

    KisStrokeId id1 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("leg1_"), false, true, false, false, true));
    queue.addJob(id1, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id1);

    KisStrokeId id2 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("lod2_"), false, true));
    queue.addJob(id2, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id2);

    t.processQueue();
    t.checkOnlyJob("sync_u_init");

    t.processQueue();
    t.checkOnlyJob("clone2_lod0_dab");

    t.processQueue();
    t.checkOnlyJob("susp_u_init");

    t.processQueue();
    t.checkOnlyJob("lod0_dab");

    t.processQueue();
    t.checkOnlyJob("resu_u_init");

    t.processQueue();
    t.checkOnlyJob("leg1_dab");

    t.processQueue();
    t.checkOnlyJob("sync_u_init");

    t.processQueue();
    t.checkOnlyJob("clone2_lod2_dab");

    t.processQueue();
    t.checkOnlyJob("susp_u_init");

    t.processQueue();
    t.checkOnlyJob("lod2_dab");

    t.processQueue();
    t.checkOnlyJob("resu_u_init");
}

void KisStrokesQueueTest::testCancelBetweenLodNStrokes()
{
    LodStrokesQueueTester t;
    KisStrokesQueue &queue = t.queue;

    // create a stroke with LOD0 + LOD2
    queue.setLodPreferences(KisLodPreferences(2));

    KisStrokeId idneg1 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("legneg1_"), false, true, false, false, true));
    queue.addJob(idneg1, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(idneg1);

    KisStrokeId id0 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("lod0_"), false, true));
    queue.addJob(id0, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id0);

    // cancelling the strokes should force synchronization of lod planes
    queue.tryCancelCurrentStrokeAsync();

    KisStrokeId id2 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("lod2_"), false, true));
    queue.addJob(id2, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id2);

    t.processQueue();
    t.checkOnlyJob("sync_u_init");

    t.processQueue();
    t.checkOnlyJob("clone2_lod2_dab");

    t.processQueue();
    t.checkOnlyJob("susp_u_init");

    t.processQueue();
    t.checkOnlyJob("lod2_dab");

    t.processQueue();
    t.checkOnlyJob("resu_u_init");
}

void KisStrokesQueueTest::testUFOVisitBetweenLodNStrokes()
{
    LodStrokesQueueTester t;
    KisStrokesQueue &queue = t.queue;

    // create a stroke with LOD0 + LOD2
    queue.setLodPreferences(KisLodPreferences(2));

    KisStrokeId idneg1 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("legneg1_"), false, true, false, false, true));
    queue.addJob(idneg1, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(idneg1);

    KisStrokeId id0 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("lod0_"), false, true));
    queue.addJob(id0, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id0);

    // force synchronization of lod planes
    queue.notifyUFOChangedImage();

    KisStrokeId id2 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("lod2_"), false, true));
    queue.addJob(id2, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id2);

    t.processQueue();
    t.checkOnlyJob("sync_u_init");

    t.processQueue();
    t.checkOnlyJob("legneg1_dab");

    t.processQueue();
    t.checkOnlyJob("sync_u_init");

    t.processQueue();
    t.checkOnlyJob("clone2_lod0_dab");

    t.processQueue();
    t.checkOnlyJob("susp_u_init");

    t.processQueue();
    t.checkOnlyJob("lod0_dab");

    t.processQueue();
    t.checkOnlyJob("resu_u_init");

    t.processQueue();
    t.checkOnlyJob("sync_u_init");

    t.processQueue();
    t.checkOnlyJob("clone2_lod2_dab");

    t.processQueue();
    t.checkOnlyJob("susp_u_init");

    t.processQueue();
    t.checkOnlyJob("lod2_dab");

    t.processQueue();
    t.checkOnlyJob("resu_u_init");

}

#include <kundo2command.h>
#include <kis_post_execution_undo_adapter.h>
struct TestUndoCommand : public KUndo2Command
{
    TestUndoCommand(const QString &text) : KUndo2Command(kundo2_noi18n(text)) {}

    void undo() override {
        ENTER_FUNCTION();
        undoCount++;
    }

    void redo() override {
        ENTER_FUNCTION();
        redoCount++;
    }

    int undoCount = 0;
    int redoCount = 0;
};

void KisStrokesQueueTest::testLodUndoBase()
{
    LodStrokesQueueTester t;
    KisStrokesQueue &queue = t.queue;

    // create a stroke with LOD0 + LOD2
    queue.setLodPreferences(KisLodPreferences(2));

    // process sync-lodn-planes stroke
    t.processQueue();
    t.checkOnlyJob("sync_u_init");

    KisStrokeId id1 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("str1_"), false, true));
    queue.addJob(id1, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id1);

    KisStrokeId id2 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("str2_"), false, true));
    queue.addJob(id2, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id2);

    t.processQueue();
    t.checkOnlyJob("clone2_str1_dab");


    QSharedPointer<TestUndoCommand> undoStr1(new TestUndoCommand("str1_undo"));
    queue.lodNPostExecutionUndoAdapter()->addCommand(undoStr1);

    t.processQueue();
    t.checkOnlyJob("clone2_str2_dab");

    QSharedPointer<TestUndoCommand> undoStr2(new TestUndoCommand("str2_undo"));
    queue.lodNPostExecutionUndoAdapter()->addCommand(undoStr2);

    t.processQueue();
    t.checkOnlyJob("susp_u_init");

    t.processQueue();
    t.checkOnlyJob("str1_dab");

    t.processQueue();
    t.checkOnlyJob("str2_dab");

    t.processQueue();
    t.checkOnlyJob("resu_u_init");
}

void KisStrokesQueueTest::testLodUndoBase2()
{
    LodStrokesQueueTester t(true);
    KisStrokesQueue &queue = t.queue;

    // create a stroke with LOD0 + LOD2
    queue.setLodPreferences(KisLodPreferences(2));
    KisStrokeId id1 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("str1_"), false, true, false, true));
    queue.addJob(id1, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id1);

    KisStrokeId id2 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("str2_"), false, true, false, true));
    queue.addJob(id2, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.endStroke(id2);

    t.processQueue();
    t.checkOnlyExecutedJob("sync_u_init");

    t.processQueue();
    t.checkOnlyExecutedJob("clone2_str1_dab");

    QSharedPointer<TestUndoCommand> undoStr1(new TestUndoCommand("str1_undo"));
    queue.lodNPostExecutionUndoAdapter()->addCommand(undoStr1);

    t.processQueue();
    t.checkOnlyExecutedJob("clone2_str2_dab");

    QSharedPointer<TestUndoCommand> undoStr2(new TestUndoCommand("str2_undo"));
    queue.lodNPostExecutionUndoAdapter()->addCommand(undoStr2);

    t.processQueue();
    t.checkOnlyExecutedJob("susp_u_init");

    queue.tryUndoLastStrokeAsync();
    t.processQueue();

    while (queue.currentStrokeName() == kundo2_noi18n("str2_undo")) {
        //queue.debugPrintStrokes();
        t.processQueue();
    }

    QCOMPARE(undoStr2->undoCount, 1);

    t.checkOnlyExecutedJob("str1_dab");

    t.processQueue();
    t.checkOnlyExecutedJob("str2_cancel");

    t.processQueue();
    t.checkOnlyExecutedJob("resu_u_init");
}

void KisStrokesQueueTest::testMutatedJobs()
{
    QSemaphore entered;
    QSemaphore completed;
    class HeldDabStrategy : public KisMutatableDabStrategy {
    public:
        HeldDabStrategy(KisStrokeStrategy *owner, QSemaphore &entered, QSemaphore &completed)
            : KisMutatableDabStrategy(QStringLiteral("str1_dab"), owner),
              entered(entered), completed(completed) {}
        void run(KisStrokeJobData *data) override {
            KisMutatableDabStrategy::run(data);
            entered.release();
            completed.acquire();
        }
    private:
        QSemaphore &entered;
        QSemaphore &completed;
    };
    class HeldStrokeStrategy : public KisTestingStrokeStrategy {
    public:
        HeldStrokeStrategy(QSemaphore &entered, QSemaphore &completed)
            : KisTestingStrokeStrategy(QLatin1String("str1_"), false, true, false, true),
              entered(entered), completed(completed) {}
        KisStrokeJobStrategy *createDabStrategy() override {
            return new HeldDabStrategy(this, entered, completed);
        }
    private:
        QSemaphore &entered;
        QSemaphore &completed;
    };
    LodStrokesQueueTester t(true);
    KisStrokesQueue &queue = t.queue;
    const auto cleanup = qScopeGuard([&] {
        completed.release(8);
        t.realContext.waitForDone();
    });
    const auto processBatch = [&](int expected) {
        // Keep both real workers occupied until this dispatch pass returns.
        // Otherwise a fast job may free its slot and let the third mutated
        // job run in the same pass; a two-thread limit is not a batch limit.
        queue.processQueue(t.context, false);
        if (!entered.tryAcquire(expected, 5000)) return false;
        completed.release(expected);
        t.realContext.waitForDone();
        return true;
    };

    KisStrokeId id1 = queue.startStroke(new HeldStrokeStrategy(entered, completed));

    queue.addJob(id1,
                 new KisTestingStrokeJobData(
                     KisStrokeJobData::CONCURRENT,
                     KisStrokeJobData::NORMAL,
                     true, "1"));

    queue.addJob(id1,
                 new KisTestingStrokeJobData(
                     KisStrokeJobData::SEQUENTIAL,
                     KisStrokeJobData::NORMAL,
                     false, "2"));

    queue.endStroke(id1);

    QVERIFY(processBatch(1));
    t.checkOnlyExecutedJob("str1_dab_1");

    QVERIFY(processBatch(2));

    QStringList refList;
    refList << "str1_dab_mutated" << "str1_dab_mutated";
    t.checkExecutedJobs(refList);

    QVERIFY(processBatch(1));
    t.checkOnlyExecutedJob("str1_dab_mutated");

    QVERIFY(processBatch(1));
    t.checkOnlyExecutedJob("str1_dab_2");

    t.processQueue();
    t.checkNothingExecuted();
}

QString sequentialityToString(KisStrokeJobData::Sequentiality seq) {
    QString result = "<unknown>";

    switch (seq) {
    case KisStrokeJobData::SEQUENTIAL:
        result = "SEQUENTIAL";
        break;
    case KisStrokeJobData::UNIQUELY_CONCURRENT:
        result = "UNIQUELY_CONCURRENT";
        break;
    case KisStrokeJobData::BARRIER:
        result = "BARRIER";
        break;
    case KisStrokeJobData::CONCURRENT:
        result = "CONCURRENT";
        break;
    }

    return result;
}

void KisStrokesQueueTest::checkJobsOverlapping(LodStrokesQueueTester &t,
                                               KisStrokeId id,
                                               KisStrokeJobData::Sequentiality first,
                                               KisStrokeJobData::Sequentiality second,
                                               bool allowed)
{
    t.queue.addJob(id, new KisTestingStrokeJobData(first,
                                                   KisStrokeJobData::NORMAL, false, "first"));
    t.processQueue();
    t.checkJobs({"str1_dab_first"});

    t.queue.addJob(id, new KisTestingStrokeJobData(second,
                                                   KisStrokeJobData::NORMAL, false, "second"));

    qDebug() << QString("  test %1 after %2 allowed: %3 ")
                .arg(sequentialityToString(second), 24)
                .arg(sequentialityToString(first), 24)
                .arg(allowed);

    if (allowed) {
        t.processQueueNoContextClear();
        t.checkJobs({"str1_dab_first", "str1_dab_second"});
    } else {
        t.processQueueNoContextClear();
        t.checkJobs({"str1_dab_first"});

        t.processQueue();
        t.checkJobs({"str1_dab_second"});
    }

    t.processQueueNoAdd();
    t.checkNothing();
}

void KisStrokesQueueTest::testUniquelyConcurrentJobs()
{
    LodStrokesQueueTester t;
    KisStrokesQueue &queue = t.queue;

    KisStrokeId id1 = queue.startStroke(new KisTestingStrokeStrategy(QLatin1String("str1_"), false, true));
    queue.addJob(id1, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
    queue.addJob(id1, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));

    { // manual test
        t.processQueue();
        t.checkJobs({"str1_dab", "str1_dab"});

        queue.addJob(id1, new KisTestingStrokeJobData(KisStrokeJobData::CONCURRENT));
        t.processQueue();
        t.checkJobs({"str1_dab"});

        queue.addJob(id1, new KisTestingStrokeJobData(KisStrokeJobData::UNIQUELY_CONCURRENT,
                                                  KisStrokeJobData::NORMAL, false, "ucon"));
        t.processQueueNoContextClear();
        t.checkJobs({"str1_dab", "str1_dab_ucon"});

        t.processQueueNoAdd();
        t.checkNothing();
    }

    // Test various cases of overlapping

    checkJobsOverlapping(t, id1, KisStrokeJobData::UNIQUELY_CONCURRENT, KisStrokeJobData::CONCURRENT, true);
    checkJobsOverlapping(t, id1, KisStrokeJobData::UNIQUELY_CONCURRENT, KisStrokeJobData::UNIQUELY_CONCURRENT, false);
    checkJobsOverlapping(t, id1, KisStrokeJobData::UNIQUELY_CONCURRENT, KisStrokeJobData::SEQUENTIAL, false);
    checkJobsOverlapping(t, id1, KisStrokeJobData::UNIQUELY_CONCURRENT, KisStrokeJobData::BARRIER, false);

    checkJobsOverlapping(t, id1, KisStrokeJobData::CONCURRENT, KisStrokeJobData::UNIQUELY_CONCURRENT , true);
    checkJobsOverlapping(t, id1, KisStrokeJobData::UNIQUELY_CONCURRENT, KisStrokeJobData::UNIQUELY_CONCURRENT, false);
    checkJobsOverlapping(t, id1, KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::UNIQUELY_CONCURRENT, false);
    checkJobsOverlapping(t, id1, KisStrokeJobData::BARRIER, KisStrokeJobData::UNIQUELY_CONCURRENT, false);

    queue.endStroke(id1);
}


KISTEST_MAIN(KisStrokesQueueTest)

/*
 *  SPDX-FileCopyrightText: 2011 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_stroke_test.h"
#include <simpletest.h>
#include <QScopeGuard>

#include "kis_stroke.h"
#include "scheduler_utils.h"

void KisStrokeTest::testCheckpointOrdering_data()
{
    QTest::addColumn<bool>("barrier");
    QTest::addColumn<bool>("ended");
    for (bool barrier : {false, true}) for (bool ended : {false, true})
        QTest::newRow(qPrintable(QString("barrier%1-ended%2").arg(barrier).arg(ended))) << barrier << ended;
}

void KisStrokeTest::testCheckpointOrdering()
{
    QFETCH(bool, barrier); QFETCH(bool, ended);
    KisStroke stroke(new KisTestingStrokeStrategy());
    auto &queue = stroke.testingGetQueue();
    const auto cleanup = qScopeGuard([&] {
        if (!stroke.isEnded()) stroke.endStroke();
        while (auto *job = stroke.popOneJob()) delete job;
    });
    delete stroke.popOneJob(); // init
    stroke.addJob(new KisStrokeJobData(KisStrokeJobData::CONCURRENT));
    auto *before = queue.back();
    if (ended) stroke.endStroke();
    auto *finish = ended ? queue.back() : nullptr;
    QVERIFY(stroke.addCheckpointJob(new KisStrokeJobData(
        barrier ? KisStrokeJobData::BARRIER : KisStrokeJobData::SEQUENTIAL)));
    auto *cut = queue[1];
    QCOMPARE(queue[0], before);
    if (ended) QCOMPARE(queue[2], finish);
    else stroke.addJob(new KisStrokeJobData(KisStrokeJobData::CONCURRENT));
    auto *after = queue.back();
    stroke.addMutatedJobs({new KisStrokeJobData(KisStrokeJobData::CONCURRENT)});
    QCOMPARE(queue[1], before);
    QCOMPARE(queue[2], cut);
    QCOMPARE(queue[3], after);
    // Another cut includes input admitted since the first cut, without moving
    // or replacing that first checkpoint or the finish marker.
    QVERIFY(stroke.addCheckpointJob(new KisStrokeJobData));
    QCOMPARE(queue[2], cut);
    if (ended) QCOMPARE(queue.back(), finish);
    else QCOMPARE(queue[3], after);
}

void KisStrokeTest::testCheckpointRejection_data()
{
    QTest::addColumn<int>("reason");
    for (int reason = 0; reason < 8; ++reason)
        QTest::newRow(qPrintable(QString::number(reason))) << reason;
}

void KisStrokeTest::testCheckpointRejection()
{
    QFETCH(int, reason);
    KisStroke stroke(new KisTestingStrokeStrategy());
    auto &queue = stroke.testingGetQueue();
    const auto cleanup = qScopeGuard([&] {
        if (!stroke.isEnded()) stroke.endStroke();
        while (auto *job = stroke.popOneJob()) delete job;
    });
    delete stroke.popOneJob();
    if (reason == 4) stroke.cancelStroke();
    if (reason == 5) stroke.failStroke();
    if (reason == 6) { stroke.endStroke(); delete stroke.popOneJob(); }
    int destroyed = 0;
    struct CountedData : KisStrokeJobData {
        CountedData(Sequentiality seq, int &count) : KisStrokeJobData(seq), count(count) {}
        ~CountedData() override { ++count; }
        int &count;
    };
    auto *data = reason == 7 ? nullptr : new CountedData(
        reason == 0 ? KisStrokeJobData::CONCURRENT : reason == 1
            ? KisStrokeJobData::UNIQUELY_CONCURRENT : KisStrokeJobData::SEQUENTIAL,
        destroyed);
    if (reason == 2) data->setCancellable(false);
    if (reason == 3) data->setLevelOfDetailOverride(2);
    const int queued = queue.size();
    QVERIFY(!stroke.addCheckpointJob(data));
    QCOMPARE(queue.size(), queued);
    QCOMPARE(destroyed, reason == 7 ? 0 : 1);
}

void KisStrokeTest::testRegularStroke()
{
    KisStroke stroke(new KisTestingStrokeStrategy());
    QQueue<KisStrokeJob*> &queue = stroke.testingGetQueue();

    QCOMPARE(queue.size(), 1);
    SCOMPARE(getJobName(queue[0]), "init");
    QCOMPARE(stroke.isEnded(), false);

    stroke.addJob(0);

    QCOMPARE(queue.size(), 2);
    SCOMPARE(getJobName(queue[0]), "init");
    SCOMPARE(getJobName(queue[1]), "dab");
    QCOMPARE(stroke.isEnded(), false);

    stroke.endStroke();

    QCOMPARE(queue.size(), 3);
    SCOMPARE(getJobName(queue[0]), "init");
    SCOMPARE(getJobName(queue[1]), "dab");
    SCOMPARE(getJobName(queue[2]), "finish");
    QCOMPARE(stroke.isEnded(), true);

    // uncomment this line to catch an assert:
    // stroke.addJob(0);

    KisStrokeJob* job;

    job = stroke.popOneJob();
    delete job;
    QCOMPARE(queue.size(), 2);
    SCOMPARE(getJobName(queue[0]), "dab");
    SCOMPARE(getJobName(queue[1]), "finish");

    job = stroke.popOneJob();
    delete job;
    QCOMPARE(queue.size(), 1);
    SCOMPARE(getJobName(queue[0]), "finish");

    job = stroke.popOneJob();
    delete job;
    QCOMPARE(queue.size(), 0);

    job = stroke.popOneJob();
    QCOMPARE(job, (KisStrokeJob*)0);
}

void KisStrokeTest::testCancelStrokeCase1()
{
    KisStroke stroke(new KisTestingStrokeStrategy());
    QQueue<KisStrokeJob*> &queue = stroke.testingGetQueue();

    stroke.addJob(0);

    // "not initialized, has jobs"

    QCOMPARE(queue.size(), 2);
    SCOMPARE(getJobName(queue[0]), "init");
    SCOMPARE(getJobName(queue[1]), "dab");
    QCOMPARE(stroke.isEnded(), false);

    stroke.cancelStroke();

    QCOMPARE(queue.size(), 0);
    QCOMPARE(stroke.isEnded(), true);

    stroke.clearQueueOnCancel();
}

void KisStrokeTest::testCancelStrokeCase2and3()
{
    KisStroke stroke(new KisTestingStrokeStrategy());
    QQueue<KisStrokeJob*> &queue = stroke.testingGetQueue();

    stroke.addJob(0);
    delete stroke.popOneJob();

    // "initialized, has jobs"

    QCOMPARE(queue.size(), 1);
    SCOMPARE(getJobName(queue[0]), "dab");
    QCOMPARE(stroke.isEnded(), false);

    stroke.cancelStroke();

    QCOMPARE(queue.size(), 1);
    SCOMPARE(getJobName(queue[0]), "cancel");
    QCOMPARE(stroke.isEnded(), true);

    stroke.clearQueueOnCancel();
}

void KisStrokeTest::testCancelStrokeCase5()
{
    KisStroke stroke(new KisTestingStrokeStrategy());
    QQueue<KisStrokeJob*> &queue = stroke.testingGetQueue();

    // initialized, no jobs, not finished

    stroke.addJob(0);
    delete stroke.popOneJob(); // init
    delete stroke.popOneJob(); // dab

    QCOMPARE(stroke.isEnded(), false);

    stroke.cancelStroke();
    QCOMPARE(queue.size(), 1);
    SCOMPARE(getJobName(queue[0]), "cancel");
    QCOMPARE(stroke.isEnded(), true);

    delete stroke.popOneJob(); // cancel
}

void KisStrokeTest::testCancelStrokeCase4()
{
    KisStroke stroke(new KisTestingStrokeStrategy());
    QQueue<KisStrokeJob*> &queue = stroke.testingGetQueue();

    stroke.addJob(0);
    stroke.endStroke();
    delete stroke.popOneJob(); // init
    delete stroke.popOneJob(); // dab
    delete stroke.popOneJob(); // finish

    QCOMPARE(stroke.isEnded(), true);

    stroke.cancelStroke();
    QCOMPARE(queue.size(), 0);
    QCOMPARE(stroke.isEnded(), true);
}

void KisStrokeTest::testCancelStrokeCase6()
{
    KisStroke stroke(new KisTestingStrokeStrategy());
    QQueue<KisStrokeJob*> &queue = stroke.testingGetQueue();

    stroke.addJob(0);
    delete stroke.popOneJob();

    // "initialized, has jobs"

    QCOMPARE(queue.size(), 1);
    SCOMPARE(getJobName(queue[0]), "dab");
    QCOMPARE(stroke.isEnded(), false);

    // "cancelled"

    stroke.cancelStroke();

    QCOMPARE(queue.size(), 1);
    SCOMPARE(getJobName(queue[0]), "cancel");
    QCOMPARE(stroke.isEnded(), true);

    int seqNo = cancelSeqNo(queue.head());

    // try cancel once more...

    stroke.cancelStroke();

    QCOMPARE(queue.size(), 1);
    SCOMPARE(getJobName(queue[0]), "cancel");
    QCOMPARE(stroke.isEnded(), true);
    QCOMPARE(cancelSeqNo(queue.head()), seqNo);

    stroke.clearQueueOnCancel();
}

void KisStrokeTest::testWorkerFailure()
{
    KisStroke stroke(new KisTestingStrokeStrategy());
    auto &queue = stroke.testingGetQueue();
    delete stroke.popOneJob(); // init
    stroke.addJob(new KisStrokeJobData());
    auto *mandatory = new KisStrokeJobData();
    mandatory->setCancellable(false);
    stroke.addJob(mandatory);
    QVERIFY(stroke.failStroke());
    QVERIFY(!stroke.failStroke()); // exactly one cancellation
    QVERIFY(!stroke.isEnded()); // the input owner may still submit/end
    QCOMPARE(queue.size(), 2);
    QCOMPARE(getJobName(queue[0]), QString("dab"));
    QCOMPARE(getJobName(queue[1]), QString("cancel"));

    stroke.addJob(new KisStrokeJobData());
    auto *lateInput = new KisStrokeJobData();
    lateInput->setCancellable(false);
    stroke.addJob(lateInput);
    stroke.addMutatedJobs({new KisStrokeJobData()});
    QCOMPARE(queue.size(), 2);
    auto *requiredChild = new KisStrokeJobData();
    requiredChild->setCancellable(false);
    stroke.addMutatedJobs({requiredChild});
    QCOMPARE(queue.size(), 3);
    stroke.endStroke();
    QCOMPARE(queue.size(), 3); // no finish on a failed stroke
    while (auto *job = stroke.popOneJob()) delete job;
}

void KisStrokeTest::testFailureAfterFinish()
{
    KisStroke stroke(new KisTestingStrokeStrategy());
    stroke.endStroke();
    delete stroke.popOneJob(); // init
    delete stroke.popOneJob(); // finish now executing
    QVERIFY(!stroke.hasJobs());
    QVERIFY(stroke.failStroke());
    QCOMPARE(getJobName(stroke.testingGetQueue().head()), QString("cancel"));
    delete stroke.popOneJob();
}

void KisStrokeTest::testFailureBeforeExplicitInit()
{
    struct Strategy : KisTestingStrokeStrategy {
        Strategy() { setNeedsExplicitCancel(true); }
    };
    KisStroke stroke(new Strategy());
    QVERIFY(stroke.failStroke());
    QCOMPARE(stroke.numJobs(), 1);
    QCOMPARE(getJobName(stroke.testingGetQueue().head()), QString("cancel"));
    stroke.endStroke();
    delete stroke.popOneJob();
}

void KisStrokeTest::testCancellationRetry()
{
    KisStroke stroke(new KisTestingStrokeStrategy());
    delete stroke.popOneJob();
    stroke.failStroke();
    delete stroke.popOneJob();
    for (int attempt = 0; attempt < 12; ++attempt) {
        const int delay = stroke.retryCancellation();
        QCOMPARE(delay, qMin(100, 1 << qMin(attempt, 7)));
        QVERIFY(stroke.cancellationRetryDelay() <= delay);
        QCOMPARE(stroke.numJobs(), 1);
        QVERIFY(!stroke.testingGetQueue().head()->isCancellable());
        stroke.cancelStroke(); // user cancel must keep pending cleanup
        QCOMPARE(stroke.numJobs(), 1);
        delete stroke.popOneJob();
    }
}

SIMPLE_TEST_MAIN(KisStrokeTest)

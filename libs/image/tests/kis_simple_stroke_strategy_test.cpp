/*
 *  SPDX-FileCopyrightText: 2011 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_simple_stroke_strategy_test.h"
#include <simpletest.h>

#include "kis_image.h"
#include "kis_simple_stroke_strategy.h"
#include "kis_update_scheduler.h"
#include "KisStrokeJobFailureContext.h"
#include <atomic>
#include <thread>


class TestingSimpleStrokeStrategy : public KisSimpleStrokeStrategy
{
public:
    TestingSimpleStrokeStrategy()
        : KisSimpleStrokeStrategy(QLatin1String("TestingSimpleStrokeStrategy")),
          m_stageCounter(0)
    {
        enableJob(KisSimpleStrokeStrategy::JOB_INIT);
        enableJob(KisSimpleStrokeStrategy::JOB_FINISH);
        enableJob(KisSimpleStrokeStrategy::JOB_CANCEL);
        enableJob(KisSimpleStrokeStrategy::JOB_DOSTROKE);
    }

    ~TestingSimpleStrokeStrategy() override {
        QCOMPARE(m_stageCounter, 3);
    }

    void initStrokeCallback() override {
        QCOMPARE(m_stageCounter, 0);
        m_stageCounter++;
    }

    void finishStrokeCallback() override {
        QCOMPARE(m_stageCounter, 2);
        m_stageCounter++;
    }

    void cancelStrokeCallback() override {
        QCOMPARE(m_stageCounter, 2);
        m_stageCounter++;
    }

    void doStrokeCallback(KisStrokeJobData *data) override {
        Q_UNUSED(data);

        QCOMPARE(m_stageCounter, 1);
        m_stageCounter++;
    }

private:
    int m_stageCounter;
};

void KisSimpleStrokeStrategyTest::testFinish()
{
    KisImageSP image = new KisImage(0, 100, 100, 0, "test executor image");

    KisStrokeId id = image->startStroke(new TestingSimpleStrokeStrategy());
    image->addJob(id, 0);
    image->endStroke(id);
    image->waitForDone();
}

void KisSimpleStrokeStrategyTest::testCancel()
{
    KisImageSP image = new KisImage(0, 100, 100, 0, "test executor image");

    KisStrokeId id = image->startStroke(new TestingSimpleStrokeStrategy());
    image->addJob(id, 0);

    /**
     * We add a delay to ensure the job is finished before the cancel
     * is requested, the cancel job will abort it otherwise
     */
    QTest::qSleep(500);

    image->cancelStroke(id);
    image->waitForDone();
}

void KisSimpleStrokeStrategyTest::testFailureCleanupProgress_data()
{
    QTest::addColumn<bool>("failFinish");
    QTest::addColumn<int>("waitMode");
    for (bool finish : {false, true}) {
        for (int mode = 0; mode < 3; ++mode) {
            QTest::newRow(qPrintable(QString("%1-wait%2").arg(finish ? "finish" : "dab").arg(mode)))
                << finish << mode;
        }
    }
}

void KisSimpleStrokeStrategyTest::testFailureCleanupProgress()
{
    QFETCH(bool, failFinish);
    QFETCH(int, waitMode);
    struct State {
        std::atomic<int> attempts{0};
        std::atomic<int> dabs{0};
        std::atomic<int> finishes{0};
        std::atomic<bool> destroyed{false};
    } state;
    struct Strategy : KisSimpleStrokeStrategy {
        Strategy(State &state, bool failFinish)
            : KisSimpleStrokeStrategy(QLatin1String("failure-progress")), state(state), failFinish(failFinish)
        {
            enableJob(JOB_DOSTROKE);
            enableJob(JOB_FINISH);
            enableJob(JOB_CANCEL);
        }
        ~Strategy() override { state.destroyed = true; }
        void doStrokeCallback(KisStrokeJobData *) override {
            ++state.dabs;
            if (!failFinish) requestStrokeFailure(QStringLiteral("injected dab failure"));
        }
        void finishStrokeCallback() override {
            ++state.finishes;
            requestStrokeFailure(QStringLiteral("injected finish failure"));
        }
        void cancelStrokeCallback() override {
            if (++state.attempts < 5) retryStrokeCancellation();
        }
        State &state;
        bool failFinish;
    };

    KisUpdateScheduler scheduler(nullptr);
    const auto id = scheduler.startStroke(new Strategy(state, failFinish));
    scheduler.addJob(id, new KisStrokeJobData());
    scheduler.endStroke(id);
    if (waitMode == 0) {
        // No more scheduler input: only the queued cleanup wakeup can progress.
        QTRY_VERIFY_WITH_TIMEOUT(state.destroyed.load(), 5000);
    } else if (waitMode == 1) {
        scheduler.waitForDone(); // no GUI event pumping
    } else {
        scheduler.barrierLock();
        scheduler.unlock();
    }
    scheduler.waitForDone();
    QCOMPARE(state.attempts.load(), 5);
    QCOMPARE(state.dabs.load(), 1);
    QCOMPARE(state.finishes.load(), failFinish ? 1 : 0);
    QVERIFY(state.destroyed);
    QVERIFY(scheduler.isIdle());
}

void KisSimpleStrokeStrategyTest::testJobFailureReporting_data()
{
    QTest::addColumn<bool>("enabled");
    QTest::addColumn<bool>("cancellable");
    for (bool enabled : {false, true}) for (bool cancellable : {false, true})
        QTest::newRow(qPrintable(QString("enabled%1-cancellable%2").arg(enabled).arg(cancellable)))
            << enabled << cancellable;
}

void KisSimpleStrokeStrategyTest::testJobFailureReporting()
{
    QFETCH(bool, enabled);
    QFETCH(bool, cancellable);
    struct State { int jobs = 0; int finishes = 0; int cancels = 0; bool reported = false; } state;
    struct Strategy : KisSimpleStrokeStrategy {
        Strategy(State &state, bool enabled)
            : KisSimpleStrokeStrategy(QLatin1String("job-failure")), state(state)
        {
            setSupportsJobFailureReporting(enabled);
            enableJob(JOB_DOSTROKE);
            enableJob(JOB_FINISH);
            enableJob(JOB_CANCEL);
        }
        void doStrokeCallback(KisStrokeJobData *) override {
            if (++state.jobs == 1)
                state.reported = KisStrokeJobFailureContext::reportFailure(QStringLiteral("pixel failure"));
        }
        void finishStrokeCallback() override { ++state.finishes; }
        void cancelStrokeCallback() override {
            QVERIFY(!KisStrokeJobFailureContext::currentJobHasFailed());
            ++state.cancels;
        }
        State &state;
    };
    KisUpdateScheduler scheduler(nullptr);
    scheduler.immediateLockForReadOnly();
    const auto id = scheduler.startStroke(new Strategy(state, enabled));
    auto *job = new KisStrokeJobData();
    if (!cancellable) job->setCancellable(false);
    scheduler.addJob(id, job);
    scheduler.addJob(id, new KisStrokeJobData());
    scheduler.endStroke(id);
    scheduler.unlock();
    scheduler.waitForDone();
    const bool failed = enabled && cancellable;
    QCOMPARE(state.reported, failed);
    QCOMPARE(state.jobs, failed ? 1 : 2);
    QCOMPARE(state.finishes, failed ? 0 : 1);
    QCOMPARE(state.cancels, failed ? 1 : 0);
}

void KisSimpleStrokeStrategyTest::testFailureContextIsolation()
{
    QVERIFY(!KisStrokeJobFailureContext::reportFailure(QStringLiteral("outside")));
    {
        KisStrokeJobFailureContext outer(true);
        QVERIFY(KisStrokeJobFailureContext::reportFailure(QStringLiteral("first")));
        QVERIFY(KisStrokeJobFailureContext::reportFailure(QStringLiteral("later")));
        QCOMPARE(outer.error(), QStringLiteral("first"));
        {
            KisStrokeJobFailureContext disabled(false);
            QVERIFY(!KisStrokeJobFailureContext::currentJobHasFailed());
            QVERIFY(!KisStrokeJobFailureContext::reportFailure(QStringLiteral("disabled")));
            KisStrokeJobFailureContext inner(true);
            QVERIFY(!inner.failed());
            QVERIFY(KisStrokeJobFailureContext::reportFailure(QStringLiteral("inner")));
            QCOMPARE(inner.error(), QStringLiteral("inner"));
        }
        QVERIFY(KisStrokeJobFailureContext::currentJobHasFailed());
        bool threadIsolated = false;
        std::thread worker([&] {
            threadIsolated = !KisStrokeJobFailureContext::currentJobHasFailed() &&
                !KisStrokeJobFailureContext::reportFailure(QStringLiteral("other thread"));
        });
        worker.join();
        QVERIFY(threadIsolated);
        QCOMPARE(outer.error(), QStringLiteral("first"));
    }
    QVERIFY(!KisStrokeJobFailureContext::currentJobHasFailed());
}

SIMPLE_TEST_MAIN(KisSimpleStrokeStrategyTest)

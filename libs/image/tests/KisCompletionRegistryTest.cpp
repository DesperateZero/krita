/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <QTest>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSemaphore>
#include <QScopeGuard>
#include <array>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <memory>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

#include "KisCompletionRegistry.h"
#include "KisPageWriteCoordinator_p.h"
#include "pagestore/KisPageStoreStoragePressure.h"

namespace {
quint64 addSource(KisCompletionRegistry &registry)
{
    const KisCompletionDomain source = KisCompletionDomain::HostLogical;
    return registry.registerSource(source);
}

KisCompletionStatus resultFor(quint64 i)
{
    return i % 3 == 0 ? KisCompletionStatus::Succeeded :
           i % 3 == 1 ? KisCompletionStatus::Failed : KisCompletionStatus::Cancelled;
}
}

class KisCompletionRegistryTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void identityAndTerminalValidation();
    void pendingGapsAndTerminalBoundaries();
    void duplicateTerminalPreservesStatusAndWaiters_data();
    void duplicateTerminalPreservesStatusAndWaiters();
    void shuffledCompletionMatchesOracle();
    void concurrentAllocationAndCompletion();
    void compactStorage();
    void preparedCompletionsAtCapacity_data();
    void preparedCompletionsAtCapacity();
    void registryStorageReturnsToProcessOwner();
    void terminalReadiness_data();
    void terminalReadiness();
    void readinessCancellation();
    void cancelledPendingSubscriptionsDoNotAccumulate();
    void readinessStorageAfterBurst_data();
    void readinessStorageAfterBurst();
    void readinessResubscriptionAfterCancel();
    void readinessCancellationRacesRegistration_data();
    void readinessCancellationRacesRegistration();
    void registryDestructionRacesCancellation();
    void terminalCleanupPrecedesCallback();
    void concurrentReadinessRegistration();
    void dispatchedReadinessCanOutliveCancellation();
    void benchmark_data();
    void benchmark();
};

void KisCompletionRegistryTest::terminalReadiness_data()
{
    QTest::addColumn<int>("terminal");
    QTest::addColumn<bool>("terminalFirst");
    for (auto status : {KisCompletionStatus::Succeeded, KisCompletionStatus::Failed,
                        KisCompletionStatus::Cancelled}) for (bool first : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("status%1-first%2").arg(int(status)).arg(first)))
            << int(status) << first;
}

void KisCompletionRegistryTest::terminalReadiness()
{
    QFETCH(int, terminal); QFETCH(bool, terminalFirst);
    KisCompletionRegistry registry;
    const auto source = addSource(registry);
    const auto ticket = registry.allocatePending(source);
    const auto status = KisCompletionStatus(terminal);
    if (terminalFirst) QVERIFY(registry.complete(ticket, status));
    int calls = 0;
    bool terminalVisible = false;
    KisPageReadinessSubscription subscription;
    const auto result = registry.watchTerminal(ticket, [&] {
        ++calls;
        // This reentrant query also proves the registry gate was dropped.
        terminalVisible = registry.status(ticket) == status;
    }, &subscription);
    QCOMPARE(result, terminalFirst ? KisPageReadinessStatus::Ready : KisPageReadinessStatus::Waiting);
    QCOMPARE(calls, 0);
    QCOMPARE(registry.sourceStatistics(source).readinessWaiters, quint64(!terminalFirst));
    if (!terminalFirst) {
        QVERIFY(registry.complete(ticket, status));
        QCOMPARE(calls, 1);
        QVERIFY(terminalVisible);
    }
    QCOMPARE(registry.sourceStatistics(source).readinessWaiters, quint64(0));
    QCOMPARE(registry.sourceStatistics(source).readinessSignals, quint64(0));
    QCOMPARE(registry.sourceStatistics(source).readinessCapacity, quint64(0));
    QVERIFY(!registry.complete(ticket, status));
    subscription.reset();
}

void KisCompletionRegistryTest::readinessCancellation()
{
    KisCompletionRegistry registry, foreign;
    const auto source = addSource(registry);
    const auto ticket = registry.allocatePending(source);
    const auto alien = foreign.allocatePending(addSource(foreign));
    int calls = 0;
    KisPageReadinessSubscription first, second;
    const auto schedule = [&] { ++calls; };
    QCOMPARE(registry.watchTerminal(alien, schedule, &first), KisPageReadinessStatus::Unavailable);
    QCOMPARE(registry.watchTerminal({}, schedule, &first), KisPageReadinessStatus::Unavailable);
    QCOMPARE(registry.watchTerminal(ticket, {}, &first), KisPageReadinessStatus::Unavailable);
    QCOMPARE(registry.watchTerminal(ticket, schedule, nullptr), KisPageReadinessStatus::Unavailable);
    QCOMPARE(registry.watchTerminal(ticket, schedule, &first), KisPageReadinessStatus::Waiting);
    QCOMPARE(registry.watchTerminal(ticket, schedule, &second), KisPageReadinessStatus::Waiting);
    second = std::move(first); // Cancels only the formerly second subscription.
    QVERIFY(!first.isValid());
    QCOMPARE(registry.sourceStatistics(source).readinessWaiters, quint64(1));
    second.reset(); second.reset();
    QCOMPARE(registry.sourceStatistics(source).readinessWaiters, quint64(0));
    QVERIFY(registry.complete(ticket, KisCompletionStatus::Succeeded));
    QCOMPARE(calls, 0);
    QCOMPARE(registry.sourceStatistics(source).storageRecords, quint64(1));
}

void KisCompletionRegistryTest::cancelledPendingSubscriptionsDoNotAccumulate()
{
    KisCompletionRegistry registry;
    const auto source = addSource(registry);
    int calls = 0;
    for (int iteration = 0; iteration < 256; ++iteration) {
        const auto ticket = registry.allocatePending(source);
        KisPageReadinessSubscription first, second;
        QCOMPARE(registry.watchTerminal(ticket, [&] { ++calls; }, &first), KisPageReadinessStatus::Waiting);
        QCOMPARE(registry.watchTerminal(ticket, [&] { ++calls; }, &second), KisPageReadinessStatus::Waiting);
        first.reset();
        QCOMPARE(registry.sourceStatistics(source).readinessWaiters, quint64(1));
        second.reset();
        QCOMPARE(registry.status(ticket), KisCompletionStatus::Pending);
        QCOMPARE(registry.sourceStatistics(source).readinessWaiters, quint64(0));
        QCOMPARE(registry.sourceStatistics(source).storageRecords, quint64(iteration + 1));
        QCOMPARE(registry.sourceStatistics(source).readinessSignals, quint64(0));
        QCOMPARE(registry.sourceStatistics(source).readinessCapacity, quint64(0));
    }
    QCOMPARE(registry.sourceStatistics(source).pendingTickets, quint64(256));
    QCOMPARE(calls, 0);
}

void KisCompletionRegistryTest::readinessStorageAfterBurst_data()
{
    QTest::addColumn<bool>("complete");
    QTest::newRow("cancel-pending") << false;
    QTest::newRow("complete") << true;
}

void KisCompletionRegistryTest::readinessStorageAfterBurst()
{
    QFETCH(bool, complete);
    KisCompletionRegistry registry;
    const auto source = addSource(registry);
    std::vector<KisCompletionTicket> tickets;
    std::vector<KisPageReadinessSubscription> subscriptions(257);
    int calls = 0;
    for (auto &subscription : subscriptions) {
        tickets.push_back(registry.allocatePending(source));
        QCOMPARE(registry.watchTerminal(tickets.back(), [&] { ++calls; }, &subscription), KisPageReadinessStatus::Waiting);
    }
    QCOMPARE(registry.sourceStatistics(source).readinessSignals, quint64(257));
    QVERIFY(registry.sourceStatistics(source).readinessCapacity >= 257);
    for (size_t i = 0; i < subscriptions.size(); ++i) {
        if (complete) QVERIFY(registry.complete(tickets[i], resultFor(i)));
        else subscriptions[i].reset();
    }
    const auto stats = registry.sourceStatistics(source);
    QCOMPARE(stats.readinessWaiters, quint64(0));
    QCOMPARE(stats.readinessSignals, quint64(0));
    QCOMPARE(stats.readinessCapacity, quint64(0));
    QCOMPARE(stats.pendingTickets, complete ? quint64(0) : quint64(257));
    QCOMPARE(calls, complete ? 257 : 0);
    // Reuse the same source after returning the notification table capacity.
    KisPageReadinessSubscription again;
    const auto next = registry.allocatePending(source);
    QCOMPARE(registry.watchTerminal(next, [&] { ++calls; }, &again), KisPageReadinessStatus::Waiting);
    again.reset();
    QCOMPARE(registry.sourceStatistics(source).readinessCapacity, quint64(0));
}

void KisCompletionRegistryTest::readinessResubscriptionAfterCancel()
{
    KisCompletionRegistry registry;
    const auto source = addSource(registry);
    const auto ticket = registry.allocatePending(source);
    bool destroyed = false;
    struct Capture {
        KisCompletionRegistry *registry;
        KisCompletionTicket ticket;
        bool *destroyed;
        ~Capture() { *destroyed = registry->status(ticket) == KisCompletionStatus::Pending; }
    };
    auto capture = std::make_shared<Capture>();
    capture->registry = &registry; capture->ticket = ticket; capture->destroyed = &destroyed;
    std::weak_ptr<Capture> weak = capture;
    KisPageReadinessSubscription old, replacement;
    QCOMPARE(registry.watchTerminal(ticket, [capture] {}, &old), KisPageReadinessStatus::Waiting);
    capture.reset(); old.reset();
    QVERIFY(destroyed && weak.expired()); // Capture destruction can reenter the registry.
    QCOMPARE(registry.sourceStatistics(source).readinessCapacity, quint64(0));
    int calls = 0;
    QCOMPARE(registry.watchTerminal(ticket, [&] { ++calls; }, &replacement), KisPageReadinessStatus::Waiting);
    old.reset();
    QCOMPARE(registry.sourceStatistics(source).readinessWaiters, quint64(1));
    QVERIFY(registry.complete(ticket, KisCompletionStatus::Succeeded));
    QCOMPARE(calls, 1);
    QCOMPARE(registry.sourceStatistics(source).readinessCapacity, quint64(0));
}

void KisCompletionRegistryTest::readinessCancellationRacesRegistration_data()
{
    QTest::addColumn<int>("order");
    QTest::newRow("cancel-first") << 0;
    QTest::newRow("register-first") << 1;
    QTest::newRow("concurrent") << 2;
}

void KisCompletionRegistryTest::readinessCancellationRacesRegistration()
{
    QFETCH(int, order);
    KisCompletionRegistry registry;
    const auto source = addSource(registry);
    for (int iteration = 0; iteration < 128; ++iteration) {
        const auto ticket = registry.allocatePending(source);
        std::atomic<int> oldCalls{0}, newCalls{0};
        KisPageReadinessSubscription old, replacement;
        QCOMPARE(registry.watchTerminal(ticket, [&] { ++oldCalls; }, &old), KisPageReadinessStatus::Waiting);
        QSemaphore start, ordered;
        auto result = KisPageReadinessStatus::Unavailable;
        std::thread cancelling([&] {
            start.acquire();
            if (order == 1) ordered.acquire();
            old.reset();
            if (order == 0) ordered.release();
        });
        std::thread registering([&] {
            start.acquire();
            if (order == 0) ordered.acquire();
            result = registry.watchTerminal(ticket, [&] { ++newCalls; }, &replacement);
            if (order == 1) ordered.release();
        });
        start.release(2); cancelling.join(); registering.join();
        QCOMPARE(result, KisPageReadinessStatus::Waiting);
        QCOMPARE(registry.sourceStatistics(source).readinessSignals, quint64(1));
        QCOMPARE(registry.sourceStatistics(source).readinessWaiters, quint64(1));
        QVERIFY(registry.complete(ticket, KisCompletionStatus::Succeeded));
        QCOMPARE(oldCalls.load(), 0); QCOMPARE(newCalls.load(), 1);
        QCOMPARE(registry.sourceStatistics(source).readinessCapacity, quint64(0));
    }
}

void KisCompletionRegistryTest::registryDestructionRacesCancellation()
{
    for (int iteration = 0; iteration < 256; ++iteration) {
        auto registry = std::make_unique<KisCompletionRegistry>();
        const auto ticket = registry->allocatePending(addSource(*registry));
        auto payload = std::make_shared<int>(iteration);
        std::weak_ptr<int> weak = payload;
        KisPageReadinessSubscription first, second;
        QCOMPARE(registry->watchTerminal(ticket, [payload] {}, &first), KisPageReadinessStatus::Waiting);
        // Exercise both the last-subscriber cleanup's weak owner promotion and
        // an un-cancelled sibling losing its signal when the facade is destroyed.
        if (iteration % 2)
            QCOMPARE(registry->watchTerminal(ticket, [payload] {}, &second), KisPageReadinessStatus::Waiting);
        payload.reset();
        QSemaphore start;
        std::thread cancelling([&] { start.acquire(); first.reset(); });
        start.release(); registry.reset(); cancelling.join();
        QVERIFY(weak.expired());
        QVERIFY(!second.isValid()); second.reset();
    }
}

void KisCompletionRegistryTest::terminalCleanupPrecedesCallback()
{
    KisCompletionRegistry registry;
    const auto source = addSource(registry);
    const auto ticket = registry.allocatePending(source);
    QSemaphore entered, resume;
    std::atomic<int> calls{0};
    KisPageReadinessSubscription subscription;
    QCOMPARE(registry.watchTerminal(ticket, [&] {
        entered.release(); resume.acquire(); ++calls;
    }, &subscription), KisPageReadinessStatus::Waiting);
    std::thread completing([&] { registry.complete(ticket, KisCompletionStatus::Succeeded); });
    entered.acquire();
    const auto stats = registry.sourceStatistics(source);
    subscription.reset(); // The callback was taken; cancellation never waits for it.
    resume.release(); completing.join();
    QCOMPARE(stats.readinessSignals, quint64(0));
    QCOMPARE(stats.readinessCapacity, quint64(0));
    QCOMPARE(calls.load(), 1);
}

void KisCompletionRegistryTest::concurrentReadinessRegistration()
{
    KisCompletionRegistry registry;
    const auto source = addSource(registry);
    for (int iteration = 0; iteration < 128; ++iteration) {
        const auto ticket = registry.allocatePending(source);
        std::atomic<int> calls{0}; std::atomic<bool> valid{true};
        QSemaphore start;
        KisPageReadinessSubscription subscription;
        auto result = KisPageReadinessStatus::Unavailable;
        std::thread registration([&] {
            start.acquire();
            result = registry.watchTerminal(ticket, [&] {
                if (registry.status(ticket) != KisCompletionStatus::Succeeded) valid = false;
                ++calls;
            }, &subscription);
        });
        std::thread completion([&] {
            start.acquire();
            if (!registry.complete(ticket, KisCompletionStatus::Succeeded)) valid = false;
        });
        start.release(2); registration.join(); completion.join();
        QVERIFY(valid.load());
        QVERIFY(result == KisPageReadinessStatus::Ready || result == KisPageReadinessStatus::Waiting);
        QCOMPARE(calls.load(), result == KisPageReadinessStatus::Ready ? 0 : 1);
        QCOMPARE(registry.sourceStatistics(source).readinessWaiters, quint64(0));
    }
}

void KisCompletionRegistryTest::dispatchedReadinessCanOutliveCancellation()
{
    KisPageReadinessSignal signal;
    QSemaphore entered, resume;
    std::atomic<int> calls{0};
    auto subscription = signal.subscribe([&] {
        entered.release(); resume.acquire(); ++calls;
    });
    std::thread notifying([&] { signal.notify(); });
    entered.acquire();
    subscription.reset(); // Cannot retract an already dispatched callback.
    resume.release(); notifying.join();
    QCOMPARE(calls.load(), 1);
    QCOMPARE(signal.subscriberCount(), qsizetype(0));
    signal.notify();
    QCOMPARE(calls.load(), 1);
}

void KisCompletionRegistryTest::identityAndTerminalValidation()
{
    KisCompletionRegistry registry, foreign;
    const quint64 source = addSource(registry);
    QVERIFY(source != 0);
    const auto ticket = registry.allocatePending(source);
    const auto alien = foreign.allocatePending(addSource(foreign));
    QVERIFY(!registry.sourceStatistics(0).knownSource);
    QVERIFY(!registry.allocatePending(0).isValid());
    QCOMPARE(registry.status({}), KisCompletionStatus::Unknown);
    QCOMPARE(registry.status(alien), KisCompletionStatus::Unknown);
    QVERIFY(!registry.complete(alien, KisCompletionStatus::Succeeded));
    QVERIFY(!registry.verifyTerminal(ticket).isValid());
    for (auto invalid : {KisCompletionStatus::Unknown, KisCompletionStatus::Pending,
                         static_cast<KisCompletionStatus>(255)}) {
        QVERIFY(!registry.complete(ticket, invalid));
        QCOMPARE(registry.status(ticket), KisCompletionStatus::Pending);
    }
    QVERIFY(registry.complete(ticket, KisCompletionStatus::Cancelled));
    for (auto terminal : {KisCompletionStatus::Succeeded, KisCompletionStatus::Failed,
                          KisCompletionStatus::Cancelled}) QVERIFY(!registry.complete(ticket, terminal));
    QCOMPARE(registry.verifyTerminal(ticket).status(), KisCompletionStatus::Cancelled);
    QCOMPARE(registry.sourceStatistics(source).terminalTickets, quint64(1));
}

void KisCompletionRegistryTest::pendingGapsAndTerminalBoundaries()
{
    KisCompletionRegistry registry;
    const auto source = addSource(registry);
    std::vector<KisCompletionTicket> tickets;
    for (int i = 0; i < 9; ++i) tickets.push_back(registry.allocatePending(source));
    // Two neighbours must not imply that the gap between them completed.
    for (int i : {0, 2, 3, 5, 8}) QVERIFY(registry.complete(tickets[i], KisCompletionStatus::Succeeded));
    for (int i : {1, 4, 6, 7}) QCOMPARE(registry.status(tickets[i]), KisCompletionStatus::Pending);
    QVERIFY(registry.complete(tickets[1], KisCompletionStatus::Succeeded));
    QVERIFY(registry.complete(tickets[4], KisCompletionStatus::Failed));
    QVERIFY(registry.complete(tickets[6], KisCompletionStatus::Cancelled));
    QVERIFY(registry.complete(tickets[7], KisCompletionStatus::Succeeded));
    for (int i = 0; i < 9; ++i) {
        const auto expected = i == 4 ? KisCompletionStatus::Failed :
                              i == 6 ? KisCompletionStatus::Cancelled : KisCompletionStatus::Succeeded;
        QCOMPARE(registry.status(tickets[i]), expected);
        QVERIFY(!registry.complete(tickets[i], KisCompletionStatus::Succeeded));
    }
    QCOMPARE(registry.sourceStatistics(source).pendingTickets, quint64(0));
}

void KisCompletionRegistryTest::duplicateTerminalPreservesStatusAndWaiters_data()
{
    QTest::addColumn<int>("shape");
    QTest::addColumn<int>("terminal");
    QTest::addColumn<int>("duplicate");
    for (int shape = 0; shape < 6; ++shape)
        for (int terminal = int(KisCompletionStatus::Succeeded); terminal <= int(KisCompletionStatus::Cancelled); ++terminal)
            for (int duplicate = int(KisCompletionStatus::Succeeded); duplicate <= int(KisCompletionStatus::Cancelled); ++duplicate)
                QTest::newRow(qPrintable(QStringLiteral("shape%1-terminal%2-duplicate%3")
                    .arg(shape).arg(terminal).arg(duplicate))) << shape << terminal << duplicate;
}

void KisCompletionRegistryTest::duplicateTerminalPreservesStatusAndWaiters()
{
    QFETCH(int, shape); QFETCH(int, terminal); QFETCH(int, duplicate);
    KisCompletionRegistry registry;
    const auto source = addSource(registry);
    std::vector<KisCompletionTicket> tickets;
    std::vector<KisCompletionStatus> expected(9, KisCompletionStatus::Pending);
    for (int i = 0; i < 9; ++i) tickets.push_back(registry.allocatePending(source));
    const auto complete = [&](int index, KisCompletionStatus status) {
        if (!registry.complete(tickets[index], status)) return false;
        expected[index] = status;
        return true;
    };
    // Prefix, isolated range, extend-left, prepend-right, bridge and a prefix
    // carrying an earlier non-success record exercise distinct range updates.
    const int target = shape == 0 ? 0 : shape == 1 ? 2 : shape == 5 ? 1 : 3;
    if (shape == 2 || shape == 4) QVERIFY(complete(2, KisCompletionStatus::Succeeded));
    if (shape == 3 || shape == 4) QVERIFY(complete(4, KisCompletionStatus::Succeeded));
    if (shape == 5) QVERIFY(complete(0, KisCompletionStatus::Failed));
    int targetCalls = 0, pendingCalls = 0;
    KisCompletionStatus seen = KisCompletionStatus::Unknown;
    KisPageReadinessSubscription targetSubscription, pendingSubscription;
    QCOMPARE(registry.watchTerminal(tickets[target], [&] {
        ++targetCalls; seen = registry.verifyTerminal(tickets[target]).status();
    }, &targetSubscription), KisPageReadinessStatus::Waiting);
    QCOMPARE(registry.watchTerminal(tickets[8], [&] { ++pendingCalls; }, &pendingSubscription),
             KisPageReadinessStatus::Waiting);
    QVERIFY(complete(target, KisCompletionStatus(terminal)));
    const auto before = registry.sourceStatistics(source);
    QVERIFY(!registry.complete(tickets[target], KisCompletionStatus(duplicate)));
    const auto after = registry.sourceStatistics(source);
    QCOMPARE(after.terminalTickets, before.terminalTickets);
    QCOMPARE(after.storageRecords, before.storageRecords);
    QCOMPARE(after.readinessWaiters, quint64(1));
    QCOMPARE(targetCalls, 1);
    QCOMPARE(pendingCalls, 0);
    QCOMPARE(seen, KisCompletionStatus(terminal));
    for (int i = 0; i < 9; ++i) QCOMPARE(registry.status(tickets[i]), expected[i]);
    QVERIFY(complete(8, KisCompletionStatus::Cancelled));
    QCOMPARE(pendingCalls, 1);
    QCOMPARE(registry.sourceStatistics(source).readinessCapacity, quint64(0));
}

void KisCompletionRegistryTest::shuffledCompletionMatchesOracle()
{
    KisCompletionRegistry registry;
    const auto source = addSource(registry);
    constexpr int count = 2048;
    std::vector<KisCompletionTicket> tickets;
    std::vector<KisCompletionStatus> oracle(count, KisCompletionStatus::Pending);
    std::vector<int> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 rng(606137);
    std::shuffle(order.begin(), order.end(), rng);
    for (int i = 0; i < count; ++i) tickets.push_back(registry.allocatePending(source));
    for (int step = 0; step < count; ++step) {
        const int i = order[step];
        const auto status = resultFor(quint64(i / 13));
        QVERIFY(registry.complete(tickets[i], status));
        oracle[i] = status;
        for (int probe = 0; probe < count; ++probe) QCOMPARE(registry.status(tickets[probe]), oracle[probe]);
        const auto stats = registry.sourceStatistics(source);
        QCOMPARE(stats.terminalTickets, quint64(step + 1));
        QCOMPARE(stats.pendingTickets, quint64(count - step - 1));
    }
}

void KisCompletionRegistryTest::concurrentAllocationAndCompletion()
{
    KisCompletionRegistry registry;
    const auto source = addSource(registry);
    std::atomic<bool> valid{true};
    std::vector<KisCompletionTicket> tickets[4];
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) workers.emplace_back([&, worker] {
        tickets[worker].reserve(4096);
        for (int i = 0; i < 4096; ++i) {
            const auto ticket = registry.allocatePending(source);
            const auto terminal = resultFor(ticket.value() / 19);
            if (!ticket.isValid() || registry.status(ticket) != KisCompletionStatus::Pending ||
                !registry.complete(ticket, terminal) || registry.status(ticket) != terminal ||
                registry.complete(ticket, terminal)) valid.store(false);
            tickets[worker].push_back(ticket);
        }
    });
    for (auto &worker : workers) worker.join();
    QVERIFY(valid.load());
    for (const auto &batch : tickets) for (const auto &ticket : batch)
        QCOMPARE(registry.status(ticket), resultFor(ticket.value() / 19));
    const auto statistics = registry.sourceStatistics(source);
    QCOMPARE(statistics.allocatedTickets, quint64(4 * 4096));
    QCOMPARE(statistics.pendingTickets, quint64(0));
}

void KisCompletionRegistryTest::compactStorage()
{
    KisCompletionRegistry registry;
    const auto source = addSource(registry);
    const auto gap = registry.allocatePending(source);
    KisCompletionTicket first, last;
    for (int i = 0; i < 100000; ++i) {
        last = registry.allocatePending(source);
        if (i == 0) first = last;
        QVERIFY(registry.complete(last, KisCompletionStatus::Succeeded));
    }
    const auto stats = registry.sourceStatistics(source);
    QCOMPARE(stats.allocatedTickets, quint64(100001));
    QCOMPARE(stats.pendingTickets, quint64(1));
    QCOMPARE(stats.terminalTickets, quint64(100000));
    QCOMPARE(stats.storageRecords, quint64(2)); // Gap preparation and the terminal range.
    QCOMPARE(registry.status(gap), KisCompletionStatus::Pending);
    QVERIFY(registry.complete(gap, KisCompletionStatus::Succeeded));
    QCOMPARE(registry.sourceStatistics(source).storageRecords, quint64(1));
    QCOMPARE(registry.status(first), KisCompletionStatus::Succeeded);
    QCOMPARE(registry.status(last), KisCompletionStatus::Succeeded);
    QVERIFY(!registry.complete(first, KisCompletionStatus::Failed));
    const auto pendingSource = addSource(registry);
    for (int i = 0; i < 100000; ++i) QVERIFY(registry.allocatePending(pendingSource).isValid());
    QCOMPARE(registry.sourceStatistics(pendingSource).storageRecords, quint64(100000));
    QCOMPARE(registry.sourceStatistics(pendingSource).pendingTickets, quint64(100000));
}

void KisCompletionRegistryTest::preparedCompletionsAtCapacity_data()
{
    QTest::addColumn<int>("terminal");
    for (const auto status : {KisCompletionStatus::Succeeded, KisCompletionStatus::Failed, KisCompletionStatus::Cancelled})
        QTest::newRow(qPrintable(QString::number(int(status)))) << int(status);
}

void KisCompletionRegistryTest::preparedCompletionsAtCapacity()
{
    QFETCH(int, terminal);
    const auto status = KisCompletionStatus(terminal);
    KisPageBackingLimits limits; limits.metadataArenaBytes = 64 * 1024;
    auto process = std::make_shared<KisBackingBudgetController>(limits);
    KisCompletionRegistry registry(process);
    const auto source = addSource(registry);
    std::array<KisCompletionTicket, 5> tickets;
    for (auto &ticket : tickets) { ticket = registry.allocatePending(source); QVERIFY(ticket.isValid()); }
    int calls = 0;
    KisPageReadinessSubscription first, bridge;
    for (const auto index : {0, 3}) {
        KisPageReadinessCallback callback([&, index] {
            QCOMPARE(registry.status(tickets[index]), status);
            ++calls;
        }, process.get());
        QCOMPARE(registry.watchTerminal(tickets[index], std::move(callback), index ? &bridge : &first),
                 KisPageReadinessStatus::Waiting);
    }
    auto warm = process->reserve({}, nullptr); QVERIFY(warm.isValid()); warm.release();
    const auto live = [&] { return process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - live());
    void *filler = allocateTestStoragePressure(process.get(), fillerBytes, 1);
    const auto release = qScopeGuard([&] { freeTestStoragePressure(process.get(), filler, fillerBytes, 1); });
    QCOMPARE(live(), limits.metadataArenaBytes);
    QVERIFY(!registry.allocatePending(source).isValid());
    QCOMPARE(registry.registerSource(KisCompletionDomain::CpuJob), quint64(0));
    QCOMPARE(registry.sourceStatistics(source).allocatedTickets, quint64(5));
    QCOMPARE(registry.sourceStatistics(source).readinessWaiters, quint64(2));
    // Isolated ranges, a bridge, then closing the gap: every terminal kind
    // completes from the original nodes while admission is actually refused.
    for (const auto index : {2, 4, 3}) registry.completePrepared(tickets[index], status);
    registry.completePrepared(tickets[1], KisCompletionStatus::Succeeded);
    registry.completePrepared(tickets[0], status);
    QCOMPARE(calls, 2);
    QCOMPARE(registry.sourceStatistics(source).pendingTickets, quint64(0));
    QCOMPARE(registry.sourceStatistics(source).terminalTickets, quint64(5));
    QVERIFY(live() < limits.metadataArenaBytes);
    for (const auto &ticket : tickets) QVERIFY(!registry.complete(ticket, KisCompletionStatus::Cancelled));
    freeTestStoragePressure(process.get(), std::exchange(filler, nullptr), fillerBytes, 1);
    const auto next = registry.allocatePending(source);
    QCOMPARE(next.value(), quint64(6)); // Refusal issued no hidden identity.
    QVERIFY(registry.complete(next, KisCompletionStatus::Succeeded));
    QCOMPARE(registry.registerSource(KisCompletionDomain::CpuJob), source + 1);
}

void KisCompletionRegistryTest::registryStorageReturnsToProcessOwner()
{
    KisPageBackingLimits limits; limits.metadataArenaBytes = 64 * 1024;
    auto process = std::make_shared<KisBackingBudgetController>(limits);
    const auto live = [&] { return process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    // Measure the parent cache after the same real source/subscription workload.
    const auto exercise = [&](bool retain) {
        auto registry = std::make_unique<KisCompletionRegistry>(process);
        const auto source = addSource(*registry);
        const auto ticket = registry->allocatePending(source);
        KisPageReadinessSubscription subscription;
        KisPageReadinessCallback callback([] {}, process.get());
        if (registry->watchTerminal(ticket, std::move(callback), &subscription) != KisPageReadinessStatus::Waiting)
            return false;
        const auto before = live();
        registry.reset();
        if (live() >= before || subscription.isValid()) return false;
        const auto weakTail = live();
        subscription.reset();
        return !retain || live() < weakTail;
    };
    QVERIFY(exercise(false));
    const auto baseline = live();
    QVERIFY(exercise(true));
    QCOMPARE(live(), baseline);
    for (const auto &bucket : process->usage().buckets) QCOMPARE(bucket.reserved.cpuRam, quint64(0));
}

void KisCompletionRegistryTest::benchmark_data()
{
    QTest::addColumn<QString>("pattern");
    for (const char *name : {"success", "pending-gap", "sparse-failure", "alternating"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

void KisCompletionRegistryTest::benchmark()
{
    QFETCH(QString, pattern);
    bool ok = true;
    const auto text = qgetenv("KIS_COMPLETION_LOOPS");
    const quint64 loops = text.isEmpty() ? 1024 : text.toULongLong(&ok);
    QVERIFY(ok && loops > 0 && loops <= 16777216);
    KisCompletionRegistry registry;
    const auto source = addSource(registry);
    const auto gap = pattern == "pending-gap" ? registry.allocatePending(source) : KisCompletionTicket{};
    const bool alternate = pattern == "alternating", sparse = pattern == "sparse-failure";
    bool valid = true;
    KisCompletionTicket first, last;
    const auto cpuStart = std::clock();
    QElapsedTimer timer;
    timer.start();
    for (quint64 i = 0; i < loops; ++i) {
        const auto ticket = registry.allocatePending(source);
        const auto result = (alternate && (i & 1)) || (sparse && (i % 1024 == 0)) ?
            KisCompletionStatus::Failed : KisCompletionStatus::Succeeded;
        valid &= registry.complete(ticket, result) && registry.status(ticket) == result;
        if (i == 0) first = ticket;
        last = ticket;
    }
    const auto wallNs = timer.nsecsElapsed();
    const auto cpuEnd = std::clock();
    QVERIFY(valid);
    const auto stats = registry.sourceStatistics(source);
    QCOMPARE(stats.terminalTickets, loops);
    QCOMPARE(stats.pendingTickets, quint64(gap.isValid()));
    QVERIFY(!registry.complete(first, KisCompletionStatus::Cancelled));
    if (gap.isValid()) QCOMPARE(registry.status(gap), KisCompletionStatus::Pending);
    QVERIFY(registry.verifyTerminal(last).isValid());
    QJsonObject record;
    record["pattern"] = pattern;
    record["completed_operations"] = qint64(loops);
    record["wall_ns"] = wallNs;
    record["process_cpu_ns"] = qint64(double(cpuEnd - cpuStart) * 1e9 / CLOCKS_PER_SEC);
    record["allocated_tickets"] = qint64(stats.allocatedTickets);
    record["pending_tickets"] = qint64(stats.pendingTickets);
    record["terminal_tickets"] = qint64(stats.terminalTickets);
    record["storage_records"] = qint64(stats.storageRecords);
    record["scope"] = "completion-registry-lifecycle; allocate-complete-status; no provider/root/drain";
    record["correctness"] = "Pass";
    qInfo().noquote() << "COMPLETION_BASELINE" << QJsonDocument(record).toJson(QJsonDocument::Compact);
}

QTEST_GUILESS_MAIN(KisCompletionRegistryTest)
#include "KisCompletionRegistryTest.moc"

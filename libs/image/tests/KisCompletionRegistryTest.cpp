/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <QTest>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

#include "KisCompletionRegistry.h"

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
    void shuffledCompletionMatchesOracle();
    void concurrentAllocationAndCompletion();
    void compactStorage();
    void benchmark_data();
    void benchmark();
};

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
    QCOMPARE(stats.storageRecords, quint64(1));
    QCOMPARE(registry.status(gap), KisCompletionStatus::Pending);
    QVERIFY(registry.complete(gap, KisCompletionStatus::Succeeded));
    QCOMPARE(registry.sourceStatistics(source).storageRecords, quint64(1));
    QCOMPARE(registry.status(first), KisCompletionStatus::Succeeded);
    QCOMPARE(registry.status(last), KisCompletionStatus::Succeeded);
    QVERIFY(!registry.complete(first, KisCompletionStatus::Failed));
    const auto pendingSource = addSource(registry);
    for (int i = 0; i < 100000; ++i) QVERIFY(registry.allocatePending(pendingSource).isValid());
    QCOMPARE(registry.sourceStatistics(pendingSource).storageRecords, quint64(0));
    QCOMPARE(registry.sourceStatistics(pendingSource).pendingTickets, quint64(100000));
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

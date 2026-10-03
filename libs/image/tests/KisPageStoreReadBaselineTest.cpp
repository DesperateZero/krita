/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// Common fixture for archived D8 and newer libraries. The held-view controls
// use D8 guard symbols; do not load this executable against D6/D7. Native
// tiles3 controls use their separate unchanged executable for older versions.
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <ctime>
#include <memory>

#include "KisCpuPageReplicaProvider.h"
#include "KisPageMetadataCoordinator.h"
#include "KisPageStoreDiagnostics_p.h"
#include "KisPageStoreCpuAccessSession.h"
#include "KisPageStoreCpuSurfaceOps.h"
#include "KisPageStoreRandomAccessor.h"
#include "KisTiles3PageReplicaProvider.h"

namespace {
bool configureStore(KisPageStore &store, int pages, QString *error, bool tiles3 = false, int bpp = 1)
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    std::shared_ptr<KisPageReplicaProvider> provider;
    if (tiles3) {
        auto native = std::make_shared<KisTiles3PageReplicaProvider>();
        KisCpuResidentReplicaProviderConfig config;
        config.provider = {175}; config.providerEpoch = {1}; config.budgetBytes = 128 * 1024 * 1024;
        if (!native->configure(config, completions, error)) return false;
        provider = native;
    } else {
        auto memory = std::make_shared<KisCpuPageReplicaProvider>();
        KisCpuResidentReplicaProviderConfig config;
        config.provider = {175}; config.providerEpoch = {1}; config.budgetBytes = 128 * 1024 * 1024;
        if (!memory->configure(config, completions, error)) return false;
        provider = memory;
    }
    KisSurfaceEpochState surface;
    surface.surface = {1};
    auto &format = surface.format;
    format.formatId = 1;
    format.colorModelId = "ALPHA";
    format.colorDepthId = "U8";
    format.profileFingerprint = "read-baseline";
    format.channelOrder = "A";
    format.packing = "interleaved";
    format.channelCount = 1;
    format.pixelStride = bpp;
    format.pixelAlignment = 1;
    format.defaultPixel = QByteArray(bpp, char(0));
    format.hasAlpha = true;
    format.alphaSemantic = KisSurfaceAlphaSemantic::SelectionMask;
    format.endianness = KisSurfaceEndianness::NativeEndian;
    format.codecVersion = 1;
    surface.contentExtent = QRect(0, 0, pages * 64, 64);
    surface.logicalPageExtent = QSize(64, 64);
    surface.layoutRevision = surface.rowAlignment = surface.defaultPixelRevision = surface.extentRevision = 1;
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.surfaces = {surface};
    return store.configure(initial, completions, 4, error) && store.registerReplicaProvider(provider) &&
        store.finalizeInitialization(error);
}

bool fill(KisPageStore &store, int pages, quint8 value, QString *error, int bpp = 1)
{
    const auto transaction = store.beginCurrentTransaction();
    return transaction.isValid() && KisPageStoreCpuSurfaceOps::fillRect(
        &store, {1}, transaction, QRect(0, 0, pages * 64, 64), QByteArray(bpp, char(value)), error) &&
        store.commit(transaction, store.preparedPages(transaction)).isValid();
}
}

class KisPageStoreReadBaselineTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void sessionIsAnImmutableOperation();
    void benchmark_data();
    void benchmark();
    void coldLifecycle_data();
    void coldLifecycle();
};

void KisPageStoreReadBaselineTest::sessionIsAnImmutableOperation()
{
    static_assert(sizeof(KisPageStore) == sizeof(void *), "frozen D6 facade ABI");
    KisPageStore store;
    QString error;
    QVERIFY(configureStore(store, 2, &error));
    QVERIFY(fill(store, 2, 0x21, &error));
    KisPageStoreCpuAccessSession session;
    QVERIFY(session.beginRead(&store, {1}, {}, KisPagePriority::Normal, &error));
    const auto first = session.readPage(0, 0, &error);
    QVERIFY(first.isValid());
    QCOMPARE(first.data[0], quint8(0x21));
    QVERIFY(fill(store, 2, 0x45, &error));
    const auto second = session.readPage(1, 0, &error);
    QVERIFY(second.isValid());
    // The old implementation follows the newly published root on the second
    // page. Keep this negative before result; it is not a performance sample.
    QCOMPARE(second.data[0], quint8(0x21));
    QVERIFY(session.finish(&error));
}

void KisPageStoreReadBaselineTest::benchmark_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<int>("pages");
    QTest::addColumn<int>("workPages");
    QTest::addColumn<int>("historyDepth");
    QTest::addColumn<bool>("tiles3");
    QTest::addColumn<int>("bpp");
    const auto row = [](const char *mode, int n, int k, int historyDepth, bool tiles3 = false, int bpp = 1) {
        QString name = QStringLiteral("%1-n%2-k%3-h%4").arg(QString::fromLatin1(mode)).arg(n).arg(k).arg(historyDepth);
        if (tiles3 || bpp != 1) name += QStringLiteral("-%1-b%2").arg(tiles3 ? "tiles3" : "cpu").arg(bpp);
        QTest::newRow(name.toLatin1().constData()) << QString::fromLatin1(mode) << n << k << historyDepth << tiles3 << bpp;
    };
    row("session", 16, 1, false);
    row("session", 256, 1, false);
    row("session", 256, 16, false);
    row("session", 256, 1, true);
    row("accessor", 256, 16, false);
    row("rect", 256, 16, false);
    for (bool tiles3 : {false, true}) for (int bpp : {4, 8, 16}) {
        row("session", 256, 1, false, tiles3, bpp);
        row("rect", 256, 16, false, tiles3, bpp);
    }
    row("guard", 16, 1, false);
    row("guard", 256, 1, false);
    row("guard", 256, 16, false);
    row("guard", 256, 1, true);
    // Fixed-K control curves: N and retained history vary independently.
    // Setup is outside the measured interval; each history generation changes
    // only K pages so the fixture itself does not require O(N*H) storage.
    row("guard", 16, 1, 32);
    row("guard", 1024, 1, 0);
    row("guard", 1024, 1, 32);
    row("guard", 1024, 16, 32);
    for (bool tiles3 : {false, true}) for (int bpp : {4, 16}) row("guard", 256, 16, false, tiles3, bpp);
}

void KisPageStoreReadBaselineTest::benchmark()
{
    QFETCH(QString, mode);
    QFETCH(int, pages);
    QFETCH(int, workPages);
    QFETCH(int, historyDepth);
    QFETCH(bool, tiles3);
    QFETCH(int, bpp);
    bool validDuration = true;
    const auto durationText = qgetenv("KIS_READ_BASELINE_MS");
    const qint64 durationMs = durationText.isEmpty() ? 0 : durationText.toLongLong(&validDuration);
    QVERIFY(validDuration && durationMs >= 0 && durationMs <= 10000);
    QElapsedTimer setup;
    setup.start();
    KisPageStore store;
    QString error;
    QVERIFY2(configureStore(store, pages, &error, tiles3, bpp), qPrintable(error));
    QVERIFY2(fill(store, pages, 0x21, &error, bpp), qPrintable(error));
    QVector<KisRetainedImageEpochSnapshot> retained;
    quint8 expected = 0x21;
    for (int i = 0; i < historyDepth; ++i) {
        auto snapshot = store.captureRetainedEpoch();
        QVERIFY(snapshot.isValid());
        retained.append(snapshot);
        expected = quint8(0x22 + (i % 0xc0));
        QVERIFY(fill(store, workPages, expected, &error, bpp));
    }
    KisCapturedReadView guardView;
    if (mode == "guard") {
        guardView = store.captureReadView();
        QVERIFY(guardView.isValid());
    }
    quint64 controlPageResolutions = 0;
    bool measureControlWork = false;
    const auto operation = [&]() -> bool {
        if (mode == "guard") {
            for (int i = 0; i < workPages; ++i) {
                auto guard = guardView.tryReadResidentPage({{1}, {i, 0}});
                if (!guard.isValid() || static_cast<const quint8 *>(guard.data())[0] != expected) return false;
                if (measureControlWork) ++controlPageResolutions;
            }
            return true;
        }
        if (mode == "rect") {
            QByteArray result;
            return KisPageStoreCpuSurfaceOps::readRect(&store, {1}, {}, QRect(0, 0, workPages * 64, 64), &result, 0, &error) &&
                result.size() == workPages * 4096 * bpp && result.front() == char(expected) && result.back() == char(expected);
        }
        if (mode == "accessor") {
            KisPageStoreRandomAccessor accessor(&store, {1}, KisPageReadView{});
            if (!accessor.isValid()) return false;
            for (int i = 0; i < workPages; ++i) {
                accessor.moveTo(i * 64, 0);
                const auto bytes = accessor.rawDataConst();
                if (!bytes || bytes[0] != expected) return false;
            }
            return accessor.finish(&error);
        }
        KisPageStoreCpuAccessSession session;
        if (!session.beginRead(&store, {1}, {}, KisPagePriority::Normal, &error)) return false;
        for (int i = 0; i < workPages; ++i) {
            const auto span = session.readPage(i, 0, &error);
            if (!span.isValid() || span.data[0] != expected) return false;
        }
        return session.finish(&error);
    };
    for (int i = 0; i < 4; ++i) QVERIFY(operation());
    const auto setupNs = setup.nsecsElapsed();
    const auto before = store.sessionStats();
    const auto metadataBefore = kisPageStoreMetadataMetrics(store);
    bool correct = true;
    qint64 completed = 0;
    measureControlWork = true;
    const auto cpuStart = std::clock();
    QElapsedTimer timer;
    timer.start();
    do {
        correct = operation() && correct;
        ++completed;
    } while (durationMs > 0 ? (completed % 16 != 0 || timer.nsecsElapsed() < durationMs * 1000000) : completed < 4);
    const auto wallNs = timer.nsecsElapsed();
    const auto cpuNs = qint64(double(std::clock() - cpuStart) * 1e9 / CLOCKS_PER_SEC);
    QVERIFY2(correct, qPrintable(error));
    const auto after = store.sessionStats();
    const auto metadataAfter = kisPageStoreMetadataMetrics(store);
    // The common binary freezes the expected path per library instead of
    // weakening a structural assertion to accept any request count.
    bool validRequests = true;
    const auto requestText = qgetenv("KIS_READ_BASELINE_REQUESTS_PER_PAGE");
    const int requestsPerPage = requestText.isEmpty() ? 0 : requestText.toInt(&validRequests);
    QVERIFY(validRequests && (requestsPerPage == 0 || requestsPerPage == 1));
    QCOMPARE(after.readRequestsCreated - before.readRequestsCreated,
             quint64(completed) * quint64(workPages) * quint64(requestsPerPage));
    QCOMPARE(after.retainedSnapshots, qsizetype(historyDepth) + qsizetype(mode == "guard" ? 1 : 0));
    QCOMPARE(after.activeReadLeases, qsizetype(0));
    QCOMPARE(after.defaultMaterializationRequests, before.defaultMaterializationRequests);
    if (mode == "guard" && !tiles3) {
        QCOMPARE(controlPageResolutions,
                 quint64(completed) * quint64(workPages));
        QCOMPARE(metadataAfter.cpuReadBindings, metadataBefore.cpuReadBindings);
        QCOMPARE(metadataAfter.fullSnapshotExports, metadataBefore.fullSnapshotExports);
        QCOMPARE(metadataAfter.localTransitionSequences, metadataBefore.localTransitionSequences);
        QCOMPARE(metadataAfter.localVersionInputs, metadataBefore.localVersionInputs);
    }
    QElapsedTimer drain;
    drain.start();
    guardView = {};
    for (const auto &snapshot : std::as_const(retained))
        QVERIFY(store.releaseSnapshot(snapshot.token));
    QVERIFY(store.closeSession(&error));
    const auto drainNs = drain.nsecsElapsed();
    QJsonObject result;
    result["fixture"] = QString::fromLatin1(QTest::currentDataTag());
    result["scope"] = mode == "guard"
        ? "held-captured-view; native-guard; CpuRam; payload-and-binding-warm; capture-release-close-separate"
        : "lifecycle-inclusive; read-session; CpuRam; payload-and-binding-warm; H-release-and-close-separate";
    result["expected_requests_per_page"] = requestsPerPage;
    result["measurement"] = durationMs ? "completed-throughput" : "correctness-short-run";
    result["completed_operations"] = completed;
    result["wall_ns"] = wallNs;
    result["process_cpu_ns"] = cpuNs;
    result["setup_ns"] = setupNs;
    result["history_release_close_ns"] = drainNs;
    result["read_requests"] = qint64(after.readRequestsCreated - before.readRequestsCreated);
    result["N"] = pages;
    result["K"] = workPages;
    result["H"] = historyDepth;
    result["control_page_resolutions"] = mode == "guard"
        ? qint64(controlPageResolutions) : qint64(completed) * workPages;
    result["control_generic_requests"] = qint64(after.readRequestsCreated - before.readRequestsCreated);
    result["control_default_materializations"] = qint64(after.defaultMaterializationRequests - before.defaultMaterializationRequests);
    result["control_metadata_transitions"] = qint64(metadataAfter.localTransitionSequences - metadataBefore.localTransitionSequences);
    result["BPP"] = bpp;
    result["provider"] = tiles3 ? "tiles3" : "cpu";
    result["endpoint"] = mode == "guard" ? "GuardReleased-view-retained" : "CpuReadable-and-session-released";
    result["threads"] = 1;
    result["in_flight"] = 1;
    result["correctness"] = "Pass";
    qInfo().noquote() << "READ_BASELINE" << QJsonDocument(result).toJson(QJsonDocument::Compact);
}

void KisPageStoreReadBaselineTest::coldLifecycle_data()
{
    QTest::addColumn<bool>("tiles3");
    QTest::addColumn<int>("bpp");
    QTest::addColumn<int>("workPages");
    for (bool tiles3 : {false, true}) for (int bpp : {4, 16}) for (int k : {0, 1, 16}) {
        const auto name = QStringLiteral("cycle-n64-k%1-%2-b%3").arg(k).arg(tiles3 ? "tiles3" : "cpu").arg(bpp).toLatin1();
        QTest::newRow(name.constData()) << tiles3 << bpp << k;
    }
}

void KisPageStoreReadBaselineTest::coldLifecycle()
{
    QFETCH(bool, tiles3); QFETCH(int, bpp); QFETCH(int, workPages);
    bool valid = true;
    const auto durationText = qgetenv("KIS_READ_BASELINE_MS");
    const qint64 durationMs = durationText.isEmpty() ? 0 : durationText.toLongLong(&valid);
    QVERIFY(valid && durationMs >= 0 && durationMs <= 10000);
    bool validCycles = true;
    const auto cyclesText = qgetenv("KIS_READ_BASELINE_CYCLES");
    const qint64 cycles = cyclesText.isEmpty() ? 2 : cyclesText.toLongLong(&validCycles);
    QVERIFY(validCycles && cycles > 0 && cycles <= 4096 && (cyclesText.isEmpty() || durationMs == 0));
    const bool profile = qEnvironmentVariableIntValue("KIS_COLD_READ_PHASES") == 1;
    const auto cpuStart = std::clock();
    QElapsedTimer total; total.start();
    auto storeOwner = std::make_unique<KisPageStore>();
    auto &store = *storeOwner;
    QString error;
    QVERIFY(configureStore(store, 64, &error, tiles3, bpp));
    QVERIFY(fill(store, 64, 0x21, &error, bpp));
    const auto setupNs = total.nsecsElapsed();
    qint64 completed = 0, prepareNs = 0, readNs = 0;
    quint64 readRequests = 0;
    QElapsedTimer timer; timer.start();
    const auto operationCpuStart = std::clock();
    do {
        // Every cycle publishes a fresh generation. No warming of that
        // generation's binding: this is write -> first native read -> release.
        const quint8 expected = completed % 2 ? 0x21 : 0x45;
        QElapsedTimer phase;
        if (profile) phase.start();
        QVERIFY2(fill(store, 64, expected, &error, bpp), qPrintable(error));
        if (profile) prepareNs += phase.nsecsElapsed();
        const auto beforeRead = store.sessionStats().readRequestsCreated;
        if (profile) phase.start();
        if (workPages) {
            KisPageStoreCpuAccessSession session;
            QVERIFY(session.beginRead(&store, {1}, {}, KisPagePriority::Normal, &error));
            for (int i = 0; i < workPages; ++i) {
                const auto span = session.readPage(i, 0, &error);
                QVERIFY2(span.isValid(), qPrintable(error));
                QCOMPARE(span.data[0], expected);
            }
            QVERIFY(session.finish(&error));
        }
        if (profile) readNs += phase.nsecsElapsed();
        readRequests += store.sessionStats().readRequestsCreated - beforeRead;
        ++completed;
    } while (durationMs ? timer.nsecsElapsed() < durationMs * 1000000 : completed < cycles);
    const auto wallNs = timer.nsecsElapsed();
    const auto cpuNs = qint64(double(std::clock() - operationCpuStart) * 1e9 / CLOCKS_PER_SEC);
    QCOMPARE(readRequests, quint64(0));
    QElapsedTimer close; close.start();
    QVERIFY(store.closeSession(&error));
    const auto closeNs = close.nsecsElapsed();
    const auto configureToCloseNs = total.nsecsElapsed();
    QElapsedTimer destroy; destroy.start();
    storeOwner.reset(); // Includes provider/ledger container destruction.
    const auto destroyNs = destroy.nsecsElapsed();
    const auto lifecycleNs = total.nsecsElapsed();
    const auto lifecycleCpuNs = qint64(double(std::clock() - cpuStart) * 1e9 / CLOCKS_PER_SEC);
    QJsonObject result;
    result["fixture"] = QString::fromLatin1(QTest::currentDataTag());
    result["measurement"] = durationMs ? "completed-throughput" : cyclesText.isEmpty() ? "correctness-short-run" : "fixed-work-lifecycle";
    result["scope"] = "generation-cycle; fill64-publish-first-read-release; CpuRam; payload-ready-binding-cold; setup-close-destroy-separate";
    result["provider"] = tiles3 ? "tiles3" : "cpu";
    result["N"] = 64; result["K"] = workPages; result["BPP"] = bpp;
    result["threads"] = 1; result["in_flight"] = 1;
    result["completed_operations"] = completed;
    result["wall_ns"] = wallNs; result["process_cpu_ns"] = cpuNs;
    result["setup_ns"] = setupNs; result["close_ns"] = closeNs;
    result["configure_to_close_ns"] = configureToCloseNs;
    result["destroy_ns"] = destroyNs;
    result["lifecycle_ns"] = lifecycleNs;
    result["lifecycle_process_cpu_ns"] = lifecycleCpuNs;
    result["lifecycle_endpoint"] = "Facade-and-provider-containers-destroyed";
    result["read_phase_requests"] = qint64(readRequests);
    result["phase_profile"] = profile;
    if (profile) { result["prepare_publish_ns"] = prepareNs; result["first_read_scope_ns"] = readNs; }
    result["phase_scope"] = "inclusive diagnostic intervals, not binding/lock self or single-operation tails";
    result["endpoint"] = "Published-CpuReadable-and-session-released";
    result["background_drain"] = "NotMeasured";
    result["correctness"] = "Pass";
    qInfo().noquote() << "READ_COLD" << QJsonDocument(result).toJson(QJsonDocument::Compact);
}

QTEST_GUILESS_MAIN(KisPageStoreReadBaselineTest)
#include "KisPageStoreReadBaselineTest.moc"

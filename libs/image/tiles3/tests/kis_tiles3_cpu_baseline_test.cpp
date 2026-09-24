/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// Keep this fixture source-identical on the fixed upstream and PageStore trees.
// Only public tiles3 APIs are used; no PageStore emulation on the upstream side.
#include <simpletest.h>

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>

#include <atomic>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>

#if defined(Q_OS_UNIX)
#include <sys/resource.h>
#include <time.h>
#endif

#include "tiles3/kis_tiled_data_manager.h"

namespace {

qint64 processCpuNs()
{
    const auto ticks = std::clock();
    return ticks == std::clock_t(-1) ? -1 : qint64(double(ticks) * 1e9 / CLOCKS_PER_SEC);
}

qint64 processPeakRssBytes()
{
#if defined(Q_OS_UNIX)
    rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) return -1;
#if defined(Q_OS_MACOS)
    return usage.ru_maxrss;
#else
    return qint64(usage.ru_maxrss) * 1024;
#endif
#else
    return -1;
#endif
}

qint64 threadCpuNs()
{
#if defined(Q_OS_UNIX) && defined(CLOCK_THREAD_CPUTIME_ID)
    timespec stamp;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &stamp) != 0) {
        qFatal("Cannot read thread CPU clock");
    }
    return qint64(stamp.tv_sec) * 1000000000LL + stamp.tv_nsec;
#else
    return -1;
#endif
}

struct ReadPin
{
    explicit ReadPin(KisTileSP value) : tile(value)
    {
        tile->lockForRead();
        bytes = tile->data();
    }
    ~ReadPin() { tile->unlockForRead(); }
    KisTileSP tile;
    const quint8 *bytes;
};

} // namespace

class KisTiles3CpuBaselineTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testSharedBaseline_data();
    void testSharedBaseline();
};

void KisTiles3CpuBaselineTest::testSharedBaseline_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<int>("totalPages");
    QTest::addColumn<int>("workPages");
    QTest::addColumn<int>("pixelBytes");
    const auto row = [](const QString &mode, int n, int k, int bpp) {
        const QByteArray tag = QStringLiteral("%1-n%2-k%3-b%4")
            .arg(mode).arg(n).arg(k).arg(bpp).toLatin1();
        QTest::newRow(tag.constData()) << mode << n << k << bpp;
    };
    const QStringList modes = {"guard", "lookup", "blank", "intent",
                               "alias", "read", "write", "kernel"};
    for (int bpp : {4, 8, 16}) {
        for (const QString &mode : modes) row(mode, 64, 1, bpp);
    }
    for (const QString &mode : {QString("guard"), QString("lookup"),
                              QString("alias"), QString("read"),
                              QString("write"), QString("kernel")}) {
        row(mode, 1024, 16, 4);
    }
    row("alias", 4096, 1, 4);
    row("write", 4096, 1, 4);
}

void KisTiles3CpuBaselineTest::testSharedBaseline()
{
    QFETCH(QString, mode);
    QFETCH(int, totalPages);
    QFETCH(int, workPages);
    QFETCH(int, pixelBytes);
    const QByteArray loopsText = qgetenv("KIS_CPU_BASELINE_LOOPS");
    bool ok = true;
    const qint64 loops = loopsText.isEmpty() ? 4 : loopsText.toLongLong(&ok);
    QVERIFY(ok && loops > 0 && loops <= 100000000);
    const QByteArray durationText = qgetenv("KIS_CPU_BASELINE_RATE_MS");
    const qint64 rateMs = durationText.isEmpty() ? 0 : durationText.toLongLong(&ok);
    QVERIFY(ok && rateMs >= 0 && rateMs <= 10000);
    QVERIFY(workPages <= totalPages);

    const int tileBytes = 64 * 64 * pixelBytes;
    const QRect all(0, 0, totalPages * 64, 64);
    const QRect work(0, 0, workPages * 64, 64);
    const QByteArray zero(pixelBytes, char(0));
    QElapsedTimer setupTimer;
    setupTimer.start();
    auto dm = std::make_unique<KisTiledDataManager>(
        pixelBytes, reinterpret_cast<const quint8 *>(zero.constData()));
    if (mode != "blank") {
        // Real writes, not one shared uniform tile: resident working-set bytes
        // and distinct backings are comparable even for the pre-bound kernel.
        const QByteArray initial(qsizetype(totalPages) * tileBytes, char(0x11));
        dm->writeBytes(reinterpret_cast<const quint8 *>(initial.constData()),
                       0, 0, all.width(), all.height());
        dm->commit();
    }
    std::unique_ptr<KisTiledDataManager> firstSource;
    std::unique_ptr<KisTiledDataManager> secondSource;
    if (mode == "alias") {
        firstSource = std::make_unique<KisTiledDataManager>(
            pixelBytes, reinterpret_cast<const quint8 *>(zero.constData()));
        secondSource = std::make_unique<KisTiledDataManager>(
            pixelBytes, reinterpret_cast<const quint8 *>(zero.constData()));
        const QByteArray firstPixel(pixelBytes, char(0x40));
        const QByteArray secondPixel(pixelBytes, char(0x41));
        firstSource->clear(work, reinterpret_cast<const quint8 *>(firstPixel.constData()));
        secondSource->clear(work, reinterpret_cast<const quint8 *>(secondPixel.constData()));
        firstSource->commit();
        secondSource->commit();
    }

    QVector<KisTileSP> cached;
    // Pin once before timing, so lookup/guard are warm resident management.
    // Do not retain blank wrappers: blank lookup includes its native lifecycle.
    if (mode != "blank") {
        cached.reserve(totalPages);
        for (int page = 0; page < totalPages; ++page) {
            KisTileSP tile = dm->getTile(page, 0, false);
            { ReadPin warm(tile); QVERIFY(warm.bytes); }
            if (mode == "guard" || mode == "kernel") cached.append(tile);
        }
    }
    std::vector<std::unique_ptr<ReadPin>> kernelPins;
    if (mode == "kernel") {
        for (int page = 0; page < workPages; ++page) {
            kernelPins.emplace_back(std::make_unique<ReadPin>(cached[page]));
        }
    }
    const QByteArray firstScanline(workPages * 64 * pixelBytes, char(0x40));
    const QByteArray secondScanline(workPages * 64 * pixelBytes, char(0x41));
    QByteArray output(workPages * tileBytes, char(0));
    quint8 *outputBytes = reinterpret_cast<quint8 *>(output.data());
    const qint64 setupNs = setupTimer.nsecsElapsed();

    // Select the operation before timing; identical harness on both trees.
    enum Mode { Guard, Lookup, Blank, Intent, Alias, Read, Write, Kernel };
    const Mode selected = Mode(QStringList({"guard", "lookup", "blank", "intent",
                                           "alias", "read", "write", "kernel"}).indexOf(mode));
    quintptr pointerWitness = 0;
    const auto operation = [&](qint64 iteration) {
        if (selected <= Intent) {
            for (int page = 0; page < workPages; ++page) {
                const int column = int((iteration * workPages + page) % totalPages);
                KisTileSP tile = selected == Guard ? cached[column] :
                    dm->getTile(column, 0, selected == Intent);
                if (selected == Intent) {
                    tile->lockForWrite();
                    // No writable pointer exposure and no pixel modification.
                    pointerWitness ^= quintptr(tile.data());
                    tile->unlockForWrite();
                } else {
                    tile->lockForRead();
                    pointerWitness ^= quintptr(tile->data());
                    tile->unlockForRead();
                }
            }
        } else if (selected == Alias) {
            dm->bitBltRough(iteration % 2 ? secondSource.get() : firstSource.get(), work);
        } else if (selected == Read) {
            dm->readBytes(outputBytes, 0, 0, work.width(), 1);
        } else if (selected == Write) {
            const QByteArray &bytes = iteration % 2 ? secondScanline : firstScanline;
            dm->writeBytes(reinterpret_cast<const quint8 *>(bytes.constData()),
                           0, 0, work.width(), 1);
        } else {
            for (int page = 0; page < workPages; ++page) {
                std::memcpy(outputBytes + page * tileBytes, kernelPins[page]->bytes, tileBytes);
            }
        }
        // Compiler-only barrier; no hardware fence, timer, allocation or log
        // in the pixel loop. Keeps repeated resident copies observable.
        std::atomic_signal_fence(std::memory_order_seq_cst);
    };
    QElapsedTimer timer;
    const qint64 processCpuBegin = processCpuNs();
    const qint64 cpuBegin = threadCpuNs();
    timer.start();
    qint64 completed = 0;
    if (rateMs) {
        // Separate completed-work throughput experiment. Operations are not
        // merged: a deadline check every 64 completions only amortizes clocks.
        do {
            for (int j = 0; j < 64; ++j) operation(completed++);
        } while (timer.nsecsElapsed() < rateMs * 1000000LL);
    } else {
        for (; completed < loops; ++completed) operation(completed);
    }
    const qint64 wallNs = timer.nsecsElapsed();
    const qint64 cpuEnd = threadCpuNs();
    const qint64 processCpuEnd = processCpuNs();
    const qint64 cpuNs = cpuBegin < 0 || cpuEnd < 0 ? -1 : cpuEnd - cpuBegin;

    // Correctness and release/destruction are outside this operation endpoint;
    // neither is silently claimed as a complete retirement/background drain.
    kernelPins.clear();
    QByteArray actual(workPages * tileBytes, char(0));
    dm->readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, work.width(), 64);
    QByteArray expected(actual.size(), mode == "blank" ? char(0) : char(0x11));
    const char last = completed % 2 ? char(0x40) : char(0x41);
    if (mode == "alias") expected.fill(last);
    if (mode == "write") std::memset(expected.data(), last, size_t(work.width()) * pixelBytes);
    QCOMPARE(actual, expected);
    QCOMPARE(dm->extent(), mode == "blank" ? QRect() : all);
    if (mode == "read") QCOMPARE(output.left(work.width() * pixelBytes), QByteArray(work.width() * pixelBytes, char(0x11)));
    if (mode == "kernel") QCOMPARE(output, QByteArray(output.size(), char(0x11)));
    if (mode == "alias") {
        firstSource->readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, work.width(), 64);
        QCOMPARE(actual, QByteArray(actual.size(), char(0x40)));
        secondSource->readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, work.width(), 64);
        QCOMPARE(actual, QByteArray(actual.size(), char(0x41)));
    }
    cached.clear();
    QElapsedTimer destroyTimer;
    destroyTimer.start();
    dm.reset();
    firstSource.reset();
    secondSource.reset();
    const qint64 managerDestroyNs = destroyTimer.nsecsElapsed();

    QJsonObject result;
    result["schema"] = 1;
    result["fixture"] = QString::fromLatin1(QTest::currentDataTag());
    result["mode"] = mode;
    result["measurement"] = rateMs ? "completed-throughput" : "fixed-work-amortized-time";
    result["evidence_class"] = selected <= Intent ? "M" : selected == Kernel ? "K" : "E";
    result["metric_scope"] = selected <= Intent ? "lifecycle-inclusive" : selected == Kernel ? "resident-kernel" : "endpoint";
    result["endpoint"] = selected <= Intent ? "GuardReleased" : selected == Kernel ? "CopyCompletePrebound" : "CpuReadable";
    result["executor"] = "CPU";
    result["access_path"] = "native-CpuRam";
    result["total_pages"] = totalPages;
    result["initial_allocated_pages_expected"] = mode == "blank" ? 0 : totalPages;
    result["work_pages"] = workPages;
    result["retained_history_roots"] = 0;
    result["pixel_bytes"] = pixelBytes;
    result["threads"] = 1;
    result["in_flight"] = 1;
    result["completed_operations"] = completed;
    result["wall_ns"] = wallNs;
    result["thread_cpu_ns"] = cpuNs;
    result["process_cpu_ns"] = processCpuBegin < 0 || processCpuEnd < 0 ? -1 : processCpuEnd - processCpuBegin;
    result["process_peak_rss_bytes"] = processPeakRssBytes();
    result["logical_work_bytes_per_operation"] = selected == Read || selected == Write ?
        work.width() * pixelBytes : selected == Kernel ? workPages * tileBytes : 0;
    result["setup_ns"] = setupNs;
    result["manager_destroy_ns"] = managerDestroyNs;
    result["background_drain"] = "NotMeasured";
    result["per_event_tail"] = "NotMeasured";
    result["rate_check_interval"] = rateMs ? 64 : 0;
    result["pointer_witness"] = QString::number(pointerWitness, 16);
    result["correctness"] = "Pass";
    qInfo().noquote() << "CPU_BASELINE" << QJsonDocument(result).toJson(QJsonDocument::Compact);
}

SIMPLE_TEST_MAIN(KisTiles3CpuBaselineTest)
#include "kis_tiles3_cpu_baseline_test.moc"

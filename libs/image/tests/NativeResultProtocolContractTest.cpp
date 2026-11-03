#include "kis_datamanager.h"
#include "tiles3/kis_hline_iterator.h"
#include "pagestore/KisPageStoreIteratorReadScope_p.h"
#include "kis_paint_device_writer.h"
#include <QBuffer>
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pagestore/experimental/KisNativeResultProtocol.h"
#include "tiles3/kis_tiled_data_manager.h"
#include "pagestore/KisPageStoreReclamation_p.h"
#include <QApplication>
#include "KisStrokeJobFailureContext.h"
#include <QByteArray>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <random>

static QString why;
static void require(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "FAIL %s: %s\n", message, qPrintable(why)); std::abort(); }
}
static const quint8 *data(const QByteArray &v) { return reinterpret_cast<const quint8 *>(v.constData()); }
static KisPageAllocationDescriptor descriptor(int bpp = 4) {
    KisSurfaceEpochState s;
    s.surface = {1}; s.logicalPageExtent = QSize(64, 64);
    s.layoutRevision = s.rowAlignment = s.defaultPixelRevision = s.extentRevision = 1;
    auto &f = s.format;
    f.formatId = 104; f.colorModelId = "RAW"; f.colorDepthId = "U8";
    f.profileFingerprint = "native104"; f.channelOrder = "raw"; f.packing = "interleaved";
    f.defaultPixel = QByteArray(bpp, 0); f.channelCount = bpp; f.pixelStride = bpp; f.pixelAlignment = 1;
    f.hasAlpha = false; f.alphaSemantic = KisSurfaceAlphaSemantic::Opaque;
    f.endianness = KisSurfaceEndianness::NativeEndian; f.codecVersion = 1;
    return s.allocationDescriptor();
}
struct Fixture {
    KisPageAllocationDescriptor desc = descriptor();
    KisTiledDataManager adapter{4, data(desc.format.defaultPixel), KisTiledDataManager::SelectionAuthority::ExternalProtocol};
    KisNativeResultProtocol protocol;
    explicit Fixture(const KisPageBackingLimits &limits = {}) : protocol(adapter, desc, limits) {}
    QRect rect{0, 0, 256, 256};
    QByteArray a{256 * 256 * 4, char(0x19)}, b{a.size(), char(0x53)}, c{a.size(), char(0x6d)};
    void write(const QByteArray &v) { require(protocol.write(data(v), rect, 1024, &why), "write"); }
    void current(const QByteArray &want) {
        QByteArray out(want.size(), 0);
        require(protocol.readCurrent(reinterpret_cast<quint8 *>(out.data()), rect, 1024, &why), "adapter read");
        require(out == want, "adapter pixels");
    }
    void historical(const KisNativeResultProtocol::Snapshot &s, const QByteArray &want) {
        QByteArray out(want.size(), 0);
        require(protocol.read(s, reinterpret_cast<quint8 *>(out.data()), rect, 1024, &why), "snapshot read");
        require(out == want, "snapshot pixels");
    }
};

static void publicContracts() {
    qputenv("KRITA_NATIVE_RESULT_PROTOCOL", "104");
    const QByteArray zero(4, 0), blue(4, char(0x35)), green(4, char(0x57));
    const QRect rect(-64, 0, 192, 64);
    const auto expect = [&](KisTiledDataManager &m, const QByteArray &want, const QRect &where) {
        QByteArray bytes(where.width() * where.height() * 4, 0);
        m.readBytes(reinterpret_cast<quint8 *>(bytes.data()), where.x(), where.y(), where.width(), where.height());
        require(bytes == want, "public pixels");
    };
    {
        KisDataManager m(4, data(zero));
        m.KisTiledDataManager::clear(rect, data(blue));
        const QByteArray blueImage(rect.width() * rect.height() * 4, char(0x35));
        auto fixed = m.capturePageStoreReadScope(false); require(fixed && fixed->isValid(), "fixed public selection");
        auto history = m.getMemento(); require(bool(history), "public history");
        require(m.getMemento() == history, "one history owner");
        {
            KisHLineIterator2 writer(&m, -64, 0, 192, 0, 0, true, nullptr);
            for (int x = 0; x < 192; ++x) {
                require(writer.rawData() && writer.oldRawData(), "native iterator actual pointers");
                require(writer.oldRawData()[0] == 0x35, "native before selection");
                std::memcpy(writer.rawData(), green.constData(), 4);
                if (x != 191) require(writer.nextPixel(), "native iterator next page");
            }
        }
        require(fixed->readPage(-1, 0, false).data()[0] == 0x35, "captured read remains old");
        QByteArray expected = blueImage; std::memset(expected.data(), 0x57, 192 * 4);
        expect(m, expected, rect);
        require(m.tryCommit(&why), "public commit");
        m.rollback(history); expect(m, blueImage, rect);
        m.rollforward(history); expect(m, expected, rect);
        KisTiledDataManagerSP clone(new KisTiledDataManager(m)); expect(*clone, expected, rect);
        clone->clear(rect, data(green)); expect(m, expected, rect);
        m.bitBlt(clone, rect); expect(m, QByteArray(expected.size(), char(0x57)), rect);
        m.setExtent(QRect(-32, 0, 64, 32));
        expect(m, QByteArray(64 * 32 * 4, char(0x57)), QRect(-32, 0, 64, 32));
        expect(m, QByteArray(32 * 32 * 4, 0), QRect(-64, 0, 32, 32));
        m.purgeHistory(history);
        std::fprintf(stderr, "PUBLIC_HISTORY_COPY_OK\n");
    }
    {
        KisDataManager m(4, data(zero));
        auto scope = m.beginIteratorWriteScope(); require(scope && scope->isActive(), "explicit iterator scope");
        auto tile = m.getTile(0, 0, true); require(tile && tile->lockForWrite(), "missing intent");
        std::memset(tile->data(), 0x35, 16384);
        m.clear(); require(!scope->isActive(), "clear revokes original iterator");
        tile->unlockForWrite(); tile.clear();
        require(!scope->finish(), "revoked iterator never accepts");
        expect(m, QByteArray(16384, 0), QRect(0, 0, 64, 64));
        auto newer = m.beginIteratorWriteScope(); require(newer && newer->isActive(), "new scope after barrier");
        auto replacement = m.getTile(0, 0, true); require(replacement && replacement->lockForWrite(), "new write after barrier");
        std::memset(replacement->data(), 0x57, 16384); replacement->unlockForWrite(); replacement.clear();
        require(newer->finish(), "new scope accepts");
        expect(m, QByteArray(16384, char(0x57)), QRect(0, 0, 64, 64));
        std::fprintf(stderr, "PUBLIC_BARRIER_OK\n");
    }
    {
        KisDataManager m(4, data(zero));
        m.KisTiledDataManager::clear(QRect(0, 0, 64, 64), data(blue));
        struct Writer final : KisPaintDeviceWriter {
            QByteArray bytes;
            bool write(const QByteArray &b) override { bytes += b; return true; }
            bool write(const char *p, qint64 n) override { bytes.append(p, n); return true; }
        } writer;
        require(m.write(writer), "public serialize");
        KisDataManager imported(4, data(zero));
        QBuffer input(&writer.bytes); require(input.open(QIODevice::ReadOnly), "import input");
        require(imported.read(&input), "public native import");
        expect(imported, QByteArray(16384, char(0x35)), QRect(0, 0, 64, 64));
        QByteArray damaged = writer.bytes; damaged.replace("DATA 1\n", "DATA 2\n"); damaged += "invalid\n";
        QBuffer broken(&damaged); require(broken.open(QIODevice::ReadOnly), "broken import input");
        require(!imported.read(&broken), "partial decoder failure cancels original write set");
        expect(imported, QByteArray(16384, 0), QRect(0, 0, 64, 64));
    }
    {
        KisDataManager m(4, data(zero));
        auto h = m.getMemento(); require(h && m.beginStrokeMutation(h, &why), "native stroke owner");
        auto result = m.writePageStoreOperation({QRect(0, 0, 128, 1)}, [&](KisPixelWriteCursor *cursor) {
            for (int x = 0; x < 128; ++x) {
                cursor->moveTo(x, 0); if (!cursor->rawData()) return false;
                std::memcpy(cursor->rawData(), blue.constData(), 4);
            }
            return true;
        }, &why, h);
        require(result == KisPageStoreWriteOperationResult::Succeeded, "native pixel operation");
        require(m.checkpointStrokeMutation(h, &why) && m.tryCommit(&why), "native checkpoint commit");
        expect(m, QByteArray(128 * 4, char(0x35)), QRect(0, 0, 128, 1));
        m.rollback(h); expect(m, QByteArray(128 * 4, 0), QRect(0, 0, 128, 1));
        m.rollforward(h); expect(m, QByteArray(128 * 4, char(0x35)), QRect(0, 0, 128, 1));
        result = m.writePageStoreOperation({QRect(0, 0, 1, 1)}, [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(0, 0); if (!cursor->rawData()) return false;
            std::memcpy(cursor->rawData(), green.constData(), 4); return false;
        }, &why);
        require(result == KisPageStoreWriteOperationResult::Failed, "pixel callback failure does not replay");
        expect(m, QByteArray(128 * 4, char(0x35)), QRect(0, 0, 128, 1));
    }
    qunsetenv("KRITA_NATIVE_RESULT_PROTOCOL");
    std::fprintf(stderr, "NATIVE_PUBLIC_CONTRACTS_OK\n");
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    publicContracts();
    {
        Fixture f; f.write(f.a); auto initial = f.protocol.capture(&why); require(bool(initial), "capture");
        for (int i = 0; i < 16; ++i) { const auto &v = i % 2 ? f.c : f.b; f.write(v); f.current(v); }
        auto final = f.protocol.capture(&why); f.historical(initial, f.a);
        require(f.protocol.restore(initial, &why), "undo"); f.current(f.a);
        require(f.protocol.restore(final, &why), "redo"); f.current(f.c);
        auto n = f.protocol.counts();
        std::fprintf(stderr, "H1_CONTRACT fresh=%llu reuse=%llu results=%llu retained=%llu\n",
            n.fresh, n.handoff, n.results, n.retainedEdges);
    }
    {
        Fixture f; f.write(f.a);
        KisNativeResultProtocol::Faults fail; fail.rejectAfterTargets = 7;
        require(!f.protocol.prepareWrite(data(f.b), f.rect, 1024, &why, &fail), "producer failure"); f.current(f.a);
        auto ready = f.protocol.prepareWrite(data(f.b), f.rect, 1024, &why); require(bool(ready), "prepare");
        fail = {}; fail.rejectAccept = true;
        require(!f.protocol.accept(ready, &why, &fail), "final reject"); f.current(f.a);
        require(f.protocol.accept(ready, &why), "Ready retry"); f.current(f.b);
        ready = f.protocol.prepareWrite(data(f.c), f.rect, 1024, &why);
        auto late = f.protocol.capture(&why);
        require(f.protocol.accept(ready, &why), "late before planning"); f.historical(late, f.b);
        ready = f.protocol.prepareWrite(data(f.a), f.rect, 1024, &why); require(bool(ready), "cancel prepare");
        f.protocol.cancel(ready); f.current(f.c);
    }
    {
        Fixture f; auto empty = f.protocol.capture(&why); f.write(f.a);
        require(f.protocol.restore(empty, &why), "restore absence"); f.current(QByteArray(f.a.size(), 0));
    }
    {
        Fixture f; f.write(f.a); f.write(f.b);
        // Destination-domain positive admission retains the source until swap
        // commits: a full idle allowance must reject without changing pixels.
        require(!f.protocol.swapIdleForTest(&why), "domain positive peak refusal");
        f.current(f.b);
    }
    {
        KisPageBackingLimits limits; limits.idleCapacityBytes = 512 * 1024;
        Fixture f(limits); f.write(f.a); f.write(f.b);
        require(f.protocol.swapIdleForTest(&why), "real SSD swap");
        f.write(f.c); f.current(f.c);
        auto held = f.protocol.capture(&why);
        require(f.protocol.close(&why), "close with snapshot"); f.historical(held, f.c);
    }
    {
        Fixture f; f.write(f.a);
        auto raw = f.adapter.getTile(0, 0, true);
        require(raw->lockForWrite(), "raw lock");
        auto *pixels = raw->data(); require(pixels != nullptr, "raw pending target");
        std::memset(pixels, 0x53, 64 * 64 * 4); raw->unlockForWrite();
        QByteArray expected = f.a;
        for (int y = 0; y < 64; ++y) std::memset(expected.data() + y * 1024, 0x53, 256);
        f.current(expected);
        auto producer = f.protocol.prepareProducer(f.rect, &why); require(bool(producer), "custom producer");
        require(!f.protocol.accept(producer, &why), "pending cannot publish");
        std::thread completion([&] {
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
                auto *p = f.protocol.producerPage(producer, x, y); require(p != nullptr, "producer row");
                std::memset(p, 0x6d, 64 * 64 * 4);
            }
            require(f.protocol.finishProducer(producer, true, &why), "producer completion");
        }); completion.join();
        require(f.protocol.accept(producer, &why), "completed same protocol"); f.current(f.c);
        producer = f.protocol.prepareProducer(f.rect, &why);
        require(bool(producer), "failed producer preparation");
        require(!f.protocol.finishProducer(producer, false, &why), "producer failure cancellation");
        f.current(f.c);
    }
    {
        Fixture f; f.write(f.a);
        auto producer = f.protocol.prepareProducer(f.rect, &why);
        auto completion = f.protocol.deferProducer(producer, &why); require(bool(completion), "real pending completion");
        require(completion.ticket().isValid(), "issued actual completion ticket");
        auto *target = f.protocol.producerPage(producer, 0, 0); require(target != nullptr, "in-flight target pin");
        f.protocol.cancel(producer);
        require(!f.protocol.close(&why), "close retains real producer tail");
        const auto inFlight = f.protocol.usage().buckets[size_t(KisBackingBudgetClass::InFlight)].live.cpuRam;
        require(inFlight == 16 * 64 * 64 * 4, "cancel did not prematurely refund in-flight pixels");
        std::memset(target, 0x53, 64 * 64 * 4); // real original pin still usable
        require(completion.finish(false), "actual producer terminal failure");
        require(f.protocol.close(&why), "closed after actual terminal");
        const auto usage = f.protocol.usage();
        for (size_t i = 0; i < size_t(KisBackingBudgetClass::MetadataArena); ++i) {
            const auto &b = usage.buckets[i];
            require(!b.live.cpuRam && !b.live.ssd && !b.reserved.cpuRam && !b.reserved.ssd, "terminal payload and credits refunded");
        }
    }
    {
        Fixture f; f.write(f.a);
        auto range = f.protocol.captureRange(QRect(0, 0, 64, 64), &why); require(bool(range), "range capture");
        f.write(f.b);
        require(f.protocol.alias(range, QRect(0, 0, 64, 64), QPoint(64, 0), &why), "alias from retained range");
        QByteArray expected = f.b;
        for (int y = 0; y < 64; ++y) std::memset(expected.data() + y * 1024 + 256, 0x19, 256);
        f.current(expected);
        auto p = f.protocol.prepareWrite(data(f.c), f.rect, 1024, &why); require(bool(p), "private prepare");
        auto cap = f.protocol.capturePrepared(p, &why); require(bool(cap), "private capability");
        f.protocol.cancel(p); f.historical(cap, f.c); f.current(expected);
        KisTiledDataManager branchAdapter(4, data(f.desc.format.defaultPixel), KisTiledDataManager::SelectionAuthority::ExternalProtocol);
        auto branch = f.protocol.cloneTo(branchAdapter, &why); require(bool(branch), "real native alias branch");
        f.write(f.a);
        QByteArray copied(expected.size(), 0);
        require(branch->readCurrent(reinterpret_cast<quint8 *>(copied.data()), f.rect, 1024, &why), "branch read");
        require(copied == expected, "branch isolation");
        require(branch->write(data(f.c), f.rect, 1024, &why), "branch COW write"); f.current(f.a);
        require(f.protocol.remove(QRect(0, 0, 64, 64), &why), "remove selection");
        expected = f.a;
        for (int y = 0; y < 64; ++y) std::memset(expected.data() + y * 1024, 0, 256);
        f.current(expected); f.historical(cap, f.c);
    }
    {
        Fixture f; auto old = f.protocol.capture(&why);
        require(f.protocol.setDefaultPixel(QByteArray(4, char(0x53)), &why), "real default change");
        f.current(f.b); f.historical(old, QByteArray(f.a.size(), 0));
        require(f.protocol.restore(old, &why), "restore default"); f.current(QByteArray(f.a.size(), 0));
        auto raw = f.adapter.getTile(-1, -1, true);
        require(raw->lockForWrite(), "absent raw lock");
        require(raw->data() != nullptr, "absent raw same protocol"); std::memset(raw->data(), 0x19, 64 * 64 * 4);
        raw->unlockForWrite();
        QByteArray read(64 * 64 * 4, 0);
        require(f.protocol.readCurrent(reinterpret_cast<quint8 *>(read.data()), QRect(-64, -64, 64, 64), 256, &why), "negative raw read");
        require(read == QByteArray(read.size(), char(0x19)), "negative raw pixels");
    }
    {
        Fixture f; f.write(f.a);
        auto p = f.protocol.prepareWrite(data(f.b), f.rect, 1024, &why); require(bool(p), "all-D preparation");
        for (int i = 0; i < 16; ++i) {
            KisNativeResultProtocol::Faults fault; fault.rejectAcceptAt = i;
            require(!f.protocol.accept(p, &why, &fault), "each final target refusal"); f.current(f.a);
        }
        KisNativeResultProtocol::Snapshot late;
        KisNativeResultProtocol::Faults lateCapture; lateCapture.beforeAccept = [&] { late = f.protocol.capture(&why); };
        require(!f.protocol.accept(p, &why, &lateCapture), "late capture changes real fee plan"); f.current(f.a);
        require(f.protocol.accept(p, &why), "replan reuses Ready pixels"); f.current(f.b); f.historical(late, f.a);
    }
    {
        KisNativeResultProtocol::Snapshot held;
        QByteArray want(256 * 256 * 4, char(0x19));
        { Fixture f; f.write(f.a); held = f.protocol.capture(&why); }
        QByteArray out(want.size(), 0);
        require(held.read(reinterpret_cast<quint8 *>(out.data()), QRect(0, 0, 256, 256), 1024, &why), "snapshot outlives facade and adapter");
        require(out == want, "surviving snapshot pixels"); held = {};
    }
    {
        Fixture f; f.rect = QRect(0, 0, 256, 256);
        QByteArray model(f.a.size(), 0); std::array<bool, 16> present{}; char def = 0;
        struct Saved { KisNativeResultProtocol::Snapshot view; QByteArray bytes; std::array<bool, 16> present; char def; };
        std::vector<Saved> saved;
        std::mt19937 random(104);
        for (int step = 0; step < 256; ++step) {
            const int action = random() % 7, page = random() % 16, x = (page % 4) * 64, y = (page / 4) * 64;
            const char value = char(1 + random() % 126);
            auto fill = [&](char v) { for (int dy = 0; dy < 64; ++dy) std::memset(model.data() + (y + dy) * 1024 + x * 4, v, 256); };
            if (action == 0 || saved.empty()) {
                QByteArray patch(64 * 64 * 4, value);
                require(f.protocol.write(data(patch), QRect(x, y, 64, 64), 256, &why), "random real write"); fill(value); present[page] = true;
            } else if (action == 1) {
                saved.push_back({f.protocol.capture(&why), model, present, def}); require(bool(saved.back().view), "random capture");
            } else if (action == 2) {
                const auto &s = saved[random() % saved.size()]; require(f.protocol.restore(s.view, &why), "random restore");
                model = s.bytes; present = s.present; def = s.def;
            } else if (action == 3) {
                require(f.protocol.remove(QRect(x, y, 64, 64), &why), "random remove"); fill(def); present[page] = false;
            } else if (action == 4) {
                require(f.protocol.setDefaultPixel(QByteArray(4, value), &why), "random default"); def = value;
                for (int i = 0; i < 16; ++i) if (!present[i]) for (int dy = 0; dy < 64; ++dy)
                    std::memset(model.data() + ((i / 4) * 64 + dy) * 1024 + (i % 4) * 256, def, 256);
            } else if (action == 5) {
                const auto &s = saved[random() % saved.size()]; const int src = random() % 16;
                require(f.protocol.alias(s.view, QRect((src % 4) * 64, (src / 4) * 64, 64, 64), QPoint(x, y), &why), "random alias");
                for (int dy = 0; dy < 64; ++dy) std::memcpy(model.data() + (y + dy) * 1024 + x * 4,
                    s.bytes.constData() + ((src / 4) * 64 + dy) * 1024 + (src % 4) * 256, 256);
                present[page] = s.present[src] || s.def != def;
            } else if (!saved.empty()) saved.erase(saved.begin() + random() % saved.size());
            if (step % 13 == 0) saved.push_back({f.protocol.capture(&why), model, present, def});
            f.current(model);
            for (const auto &s : saved) f.historical(s.view, s.bytes);
        }
        std::fprintf(stderr, "NATIVE104_RANDOM_TRANSITIONS 256\n");
    }
    for (int bpp : {1, 4, 8, 16}) {
        auto desc = descriptor(bpp);
        KisTiledDataManager adapter(bpp, data(desc.format.defaultPixel), KisTiledDataManager::SelectionAuthority::ExternalProtocol);
        KisNativeResultProtocol protocol(adapter, desc);
        QByteArray all(128 * 128 * bpp, char(0x19)), patch(65 * 63 * bpp, char(0x53)), out(all.size(), 0);
        require(protocol.write(data(all), QRect(0, 0, 128, 128), 128 * bpp, &why), "format full");
        require(protocol.write(data(patch), QRect(31, 33, 65, 63), 65 * bpp, &why), "partial format copy");
        for (int y = 0; y < 63; ++y) std::memset(all.data() + ((y + 33) * 128 + 31) * bpp, 0x53, 65 * bpp);
        require(protocol.readCurrent(reinterpret_cast<quint8 *>(out.data()), QRect(0, 0, 128, 128), 128 * bpp, &why), "format adapter read");
        require(out == all, "partial copy independent bytes");
        const auto old = protocol.counts(); require(protocol.write(data(all), QRect(0, 0, 128, 128), 128 * bpp, &why), "no-op");
        const auto now = protocol.counts(); require(old.results == now.results && old.fresh == now.fresh && old.handoff == now.handoff, "no-op has no result services");
    }
    {
        auto desc = descriptor();
        KisTiledDataManager adapter(4, data(desc.format.defaultPixel), KisTiledDataManager::SelectionAuthority::ExternalProtocol);
        KisNativeResultProtocol protocol(adapter, desc);
        QByteArray a(512 * 512 * 4, char(0x19)), b(a.size(), char(0x53)), c(a.size(), char(0x6d));
        require(protocol.write(data(a), QRect(0, 0, 512, 512), 2048, &why), "pressure setup");
        auto held = protocol.capture(&why);
        for (int i = 0; i < 16; ++i) require(protocol.write(data(i % 2 ? b : c), QRect(0, 0, 512, 512), 2048, &why), "64-page pressure write");
#ifdef NR104_CENSUS
        const auto counts = protocol.counts(); require(counts.fresh == 864 && counts.handoff == 224, "real pressure F/H counts");
#endif
    }
    {
        KisPageBackingLimits limits; limits.retainedHistoryBytes = 0;
        Fixture f(limits); f.write(f.a); auto held = f.protocol.capture(&why);
        auto p = f.protocol.prepareWrite(data(f.b), f.rect, 1024, &why); require(bool(p), "Ready before history admission");
        require(!f.protocol.accept(p, &why), "real retained class refusal"); f.current(f.a);
        held = {}; require(f.protocol.accept(p, &why), "retry after actual retained consumer release"); f.current(f.b);
    }
    {
        KisPageBackingLimits limits; limits.activePendingBytes = 8 * 64 * 64 * 4; limits.idleCapacityBytes = 0;
        Fixture f(limits);
        require(!f.protocol.write(data(f.a), f.rect, 1024, &why), "real partial factory budget failure"); f.current(QByteArray(f.a.size(), 0));
        require(f.protocol.close(&why), "budget failure terminal");
        const auto usage = f.protocol.usage();
        for (size_t i = 0; i < size_t(KisBackingBudgetClass::MetadataArena); ++i) {
            const auto &b = usage.buckets[i];
            require(!b.live.cpuRam && !b.reserved.cpuRam, "factory failure has no unfunded or leaked payload");
        }
    }
    {
        Fixture f; f.write(f.a);
        auto raw = f.adapter.getTile(0, 0, false); require(raw->lockForRead(), "late real raw read pin");
        const auto *old = raw->data(); require(old && old[0] == 0x19, "old physical bytes");
        f.write(f.b); f.current(f.b); f.write(f.c); f.current(f.c);
        require(old[0] == 0x19, "late read survived new current and failed handoff");
        require(f.protocol.usage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].live.cpuRam >= 64 * 64 * 4, "actual pin retains funded debt");
        raw->unlockForRead(); raw.clear(); kisDrainPageStoreReclamation();
        require(!f.protocol.usage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].live.cpuRam, "real pin release permits provider retirement");
    }
    {
        KisPageBackingLimits limits; limits.retainedHistoryBytes = 0;
        Fixture f(limits); f.write(f.a); auto held = f.protocol.capture(&why);
        KisStrokeJobFailureContext failure(true);
        auto raw = f.adapter.getTile(0, 0, true); require(raw->lockForWrite(), "raw refused lock");
        std::memset(raw->data(), 0x53, 64 * 64 * 4); raw->unlockForWrite(); raw.clear();
        require(failure.failed(), "raw final rejection reaches actual job context"); f.current(f.a);
    }
    kisDrainPageStoreReclamation();
    std::fprintf(stderr, "NATIVE104_CONTRACT_OK\n");
}

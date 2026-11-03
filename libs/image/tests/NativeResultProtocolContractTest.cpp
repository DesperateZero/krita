#ifdef NR104_INSTALL_AUDIT
#include <dlfcn.h>
#endif
#include <atomic>
#include <vector>
#include "kis_datamanager.h"
#include "tiles3/kis_hline_iterator.h"
#include "pagestore/KisPageStoreIteratorReadScope_p.h"
#include "kis_paint_device_writer.h"
#include <QBuffer>
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pagestore/native/KisNativeResultProtocol.h"
#include "pagestore/native/KisNativeResultDiagnostics_p.h"
#include "tiles3/kis_tiled_data_manager.h"
#include "pagestore/KisPageStoreReclamation_p.h"
#include "pagestore/KisPageStoreProductPolicy_p.h"
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

class NativeResultProtocolContractTest {
public:
    static void publicContracts();
    static void concurrentContracts();
    static void pressureIdentityContracts();
    static void freshControlContracts();
    static void stoppedRuntimeContract();
};
void NativeResultProtocolContractTest::publicContracts() {
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
        auto oldRaw = m.getTile(-1, 0, false); require(oldRaw && oldRaw->lockForRead(), "old public raw read");
        const auto *oldBytes = oldRaw->data();
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
        auto freshRaw = m.getTile(-1, 0, false); require(freshRaw && freshRaw->lockForRead(), "fresh public raw read");
        require(freshRaw->data()[0] == 0x57 && oldBytes[0] == 0x35, "fresh selection separates old raw pin");
        freshRaw->unlockForRead(); freshRaw.clear(); oldRaw->unlockForRead(); oldRaw.clear();
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
        require(scope->finish(), "revoked iterator finishes as a successful no-op");
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
        auto failedOwner = m.getMemento(); require(m.beginStrokeMutation(failedOwner, &why), "failed stroke owns original history");
        const auto changed = [&](KisPixelWriteCursor *cursor) {
            cursor->moveTo(0, 0); if (!cursor->rawData()) return false;
            std::memcpy(cursor->rawData(), green.constData(), 4); return true;
        };
        require(m.writePageStoreOperation({QRect(0, 0, 1, 1)}, changed, &why, failedOwner) == KisPageStoreWriteOperationResult::Succeeded, "first actual stroke result");
        require(m.writePageStoreOperation({QRect(0, 0, 1, 1)}, [](KisPixelWriteCursor *) { return false; }, &why, failedOwner) == KisPageStoreWriteOperationResult::Failed, "failed stroke callback");
        int replay = 0;
        require(m.writePageStoreOperation({}, [&](KisPixelWriteCursor *) { ++replay; return true; }, &why, failedOwner) == KisPageStoreWriteOperationResult::Failed && !replay,
            "failed original history cannot accept later producer");
        require(!m.tryCommit(&why) && !m.beginStrokeMutation(failedOwner, &why), "failure poisons commit and reopen");
        require(m.tryAbort(failedOwner, &why), "actual abort restores original before");
        expect(m, QByteArray(128 * 4, char(0x35)), QRect(0, 0, 128, 1));
    }
    std::fprintf(stderr, "NATIVE_PUBLIC_CONTRACTS_OK\n");
}

static void requireDrained(KisNativeResultProtocol &protocol) {
    kisDrainPageStoreReclamation();
    const auto state = KisNativeResultTestAccess::state(protocol);
    require(!state.activeResults && !state.domainMoves && !state.terminalRegistrations && !state.reservedEdges,
        "no outstanding original work or terminal registrations");
    const auto usage = protocol.usage();
    for (size_t i = 0; i < size_t(KisBackingBudgetClass::Count); ++i) {
        const auto &bucket = usage.buckets[i];
        require(!bucket.reserved.cpuRam && !bucket.reserved.ssd && !bucket.reserved.discreteVram && !bucket.reserved.umaShared,
            "all category reservations returned");
        if (i == size_t(KisBackingBudgetClass::ActivePending) || i == size_t(KisBackingBudgetClass::InFlight) ||
            i == size_t(KisBackingBudgetClass::RetirementDebt))
            require(!bucket.live.cpuRam && !bucket.live.ssd && !bucket.live.discreteVram && !bucket.live.umaShared,
                "no transient or retirement live debt");
    }
}

void NativeResultProtocolContractTest::concurrentContracts() {
    {
        Fixture f; std::atomic<bool> failed{false}; std::vector<std::thread> threads;
        for (int worker = 0; worker < 4; ++worker) threads.emplace_back([&, worker] {
            QString error;
            const QRect area(worker * 128, 0, 128, 64);
            for (int i = 0; i < 32; ++i) {
                QByteArray bytes(128 * 64 * 4, char(worker + i + 1));
                auto pending = f.protocol.prepareWrite(data(bytes), area, 128 * 4, &error);
                if (!pending) { std::fprintf(stderr, "CONCURRENT_PREP %s\n", qPrintable(error)); failed = true; return; }
                bool accepted = false;
                for (int attempt = 0; attempt < 64 && !accepted; ++attempt) {
                    accepted = f.protocol.accept(pending, &error);
                    if (!accepted) std::this_thread::yield();
                }
                if (!accepted) { std::fprintf(stderr, "CONCURRENT_ACCEPT %s\n", qPrintable(error)); failed = true; return; }
                QByteArray out(bytes.size(), 0);
                if (!f.protocol.readCurrent(reinterpret_cast<quint8 *>(out.data()), area, 512, &error) || out != bytes) { failed = true; return; }
            }
        });
        for (auto &thread : threads) thread.join();
        require(!failed, "disjoint writers and actual capacity growth");
        require(f.protocol.close(&why), "concurrent owner closes"); requireDrained(f.protocol);
        require(!KisNativeResultTestAccess::state(f.protocol).physicalResources, "concurrent physical release");
    }
    {
        Fixture f; f.rect = QRect(0, 0, 64 * 80, 64);
        f.a = QByteArray(64 * 80 * 64 * 4, char(0x19)); f.b = QByteArray(f.a.size(), char(0x53)); f.c = QByteArray(f.a.size(), char(0x6d));
        require(f.protocol.write(data(f.a), f.rect, f.rect.width() * 4, &why), "many pinned pages initial");
        auto view = f.protocol.capture(&why);
        std::vector<KisNativeResultProtocol::ReadAccess> pins;
        for (int x = 0; x < 80; ++x) { pins.push_back(view.readPage(x, 0, &why)); require(pins.back().data(), "actual late pin"); }
        view = {};
        require(f.protocol.write(data(f.b), f.rect, f.rect.width() * 4, &why), "many pins second write");
        require(f.protocol.write(data(f.c), f.rect, f.rect.width() * 4, &why), "many pins third write");
        kisDrainPageStoreReclamation();
        require(KisNativeResultTestAccess::state(f.protocol).terminalRegistrations >= 64, "actual deferred set exceeds one bounded pass");
        std::vector<std::thread> release;
        for (int worker = 0; worker < 4; ++worker) release.emplace_back([&, worker] {
            for (int x = worker; x < 80; x += 4) pins[x] = {};
        });
        for (auto &thread : release) thread.join();
        require(f.protocol.close(&why), "many terminals close"); requireDrained(f.protocol);
        require(!KisNativeResultTestAccess::state(f.protocol).physicalResources, "all notified physical resources retired");
    }
    {
        KisNativeResultProtocol::ReadAccess pin;
        {
            auto f = std::make_unique<Fixture>(); f->write(f->a);
            auto view = f->protocol.capture(&why); pin = view.readPage(0, 0, &why); view = {};
            require(f->protocol.close(&why), "facade closes before raw capability");
            require(pin.data() && pin.data()[0] == 0x19, "late capability remains readable");
        }
        require(pin.data()[0] == 0x19, "capability survives actual adapter destruction"); pin = {};
        kisDrainPageStoreReclamation();
    }
    {
        Fixture f; f.write(f.a);
        auto pending = f.protocol.prepareProducer(QRect(0, 0, 64, 64), &why);
        auto done = f.protocol.deferProducer(pending, &why); require(bool(done), "async original producer");
        require(!f.protocol.close(&why), "pending producer owns funded close tail");
        f.protocol.cancel(pending); require(done.finish(false), "real completion cancels late original work");
        require(f.protocol.close(&why), "async late close completion"); requireDrained(f.protocol);
    }
    const auto layout = KisNativeResultTestAccess::layout();
    std::fprintf(stderr, "NATIVE_LAYOUT page=%zu resource=%zu body=%zu row=%zu edge=%zu view=%zu\n",
        layout.page, layout.resource, layout.body, layout.row, layout.retentionEdge, layout.view);
    std::fprintf(stderr, "NATIVE_CONCURRENT_TERMINAL_OK\n");
}

void NativeResultProtocolContractTest::pressureIdentityContracts() {
    const QRect one(0, 0, 64, 64);
    const QByteArray a(16384, char(0x19)), b(16384, char(0x53));
    {
        Fixture f; f.write(f.a);
        std::array<KisMementoSP, 4> owners; std::vector<std::thread> threads;
        for (size_t i = 0; i < owners.size(); ++i) threads.emplace_back([&, i] { owners[i] = f.protocol.beginHistory(); });
        for (auto &thread : threads) thread.join();
        for (const auto &owner : owners) require(owner && owner == owners[0], "concurrent public history returns one owner");
        require(f.protocol.commitHistory(&why), "one concurrent history commit");
        owners = {}; require(f.protocol.close(&why), "concurrent history close"); requireDrained(f.protocol);
    }
    {
        KisPageBackingLimits limits; limits.maxTransientVersionsPerPage = 1;
        Fixture f(limits);
        require(f.protocol.write(data(a), one, 256, &why), "transient seed");
        auto p = f.protocol.prepareWrite(data(b), one, 256, &why); require(bool(p), "one original unpublished result");
        auto retained = f.protocol.capturePrepared(p, &why); require(bool(retained), "actual unpublished consumer");
        f.protocol.cancel(p);
        require(!f.protocol.prepareWrite(data(b), one, 256, &why), "retained unpublished result enforces transient bound");
        retained = {};
        require(f.protocol.write(data(b), one, 256, &why), "last actual unpublished consumer releases allowance");
        require(f.protocol.close(&why), "transient close"); requireDrained(f.protocol);
    }
    {
        KisPageBackingLimits limits; limits.metadataArenaBytes = 512 * 1024;
        Fixture f(limits); f.write(f.a);
        // The original operation row capacity alone exceeds this allowance.
        require(!f.protocol.prepareProducer(QRect(0, 0, 4096, 4096), &why), "real cold storage admission refusal");
        f.current(f.a);
        require(!KisNativeResultTestAccess::state(f.protocol).activeResults && !KisNativeResultTestAccess::state(f.protocol).reservedEdges, "growth refusal returns original owner and edge credits");
        f.write(f.b); f.current(f.b);
    }
    {
        Fixture source, destination;
        require(source.protocol.write(data(a), one, 256, &why), "cross-provider source");
        auto view = source.protocol.capture(&why);
        require(source.protocol.alias(view, one, QPoint(64, 0), &why), "source stable alias");
        view = source.protocol.capture(&why);
        require(!destination.protocol.restore(view, &why), "foreign owner cannot restore authority");
        require(!destination.protocol.alias(view, one, QPoint(0, 0), &why), "foreign capability cannot become native selection");
        require(destination.protocol.copyFrom(view, QRect(0, 0, 128, 64), &why), "foreign real provider import");
        require(KisNativeResultTestAccess::state(destination.protocol).physicalResources == 1, "foreign alias topology charged once");
        require(destination.protocol.write(data(b), one, 256, &why), "destination independent COW");
        QByteArray read(16384, 0);
        require(view.read(reinterpret_cast<quint8 *>(read.data()), one, 256, &why) && read == a, "cross-provider source isolation");
        auto desc = descriptor(8);
        KisTiledDataManager adapter(8, data(desc.format.defaultPixel), KisTiledDataManager::SelectionAuthority::ExternalProtocol);
        KisNativeResultProtocol mismatch(adapter, desc);
        require(!mismatch.copyFrom(view, one, &why), "incompatible stride refused before import");
        require(!source.protocol.write(data(a), QRect(std::numeric_limits<int>::max()-63, 0, 64, 64), 256, &why), "unsupported public coordinate rejected");
    }
    {
        KisPageStoreProductPolicy policy; require(kisDerivePageStoreProductPolicy(&policy, &why), "actual product policy");
        auto parent = kisAcquireProductNonPayloadBudget(policy, &why);
        const auto before = parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
        {
            Fixture first; first.write(first.a);
            const auto afterFirst = parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
            Fixture second; second.write(second.a);
            require(kisAcquireProductNonPayloadBudget(policy, &why) == parent, "two native documents use shared product parent");
            require(afterFirst > before && parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam > afterFirst,
                "both actual native documents charge shared parent");
            KisBackingBudgetDelta oversized; oversized.bucket(KisBackingBudgetClass::MetadataArena).cpuRam = qint64(policy.limits.metadataArenaBytes);
            require(!parent->reserve(oversized, &why).isValid(), "combined product metadata ceiling enforced");
        }
        kisDrainPageStoreReclamation();
    }
    // Fixed K=1; only the surrounding current/history population changes.
    // Diagnostics verify real touched work, not a time or instruction claim.
    for (int n : {1, 64, 256}) for (int h : {0, 8}) {
        Fixture f;
        QByteArray image(n * 16384, char(0x19));
        require(f.protocol.write(data(image), QRect(0, 0, n * 64, 64), n * 256, &why), "scaled population setup");
        std::vector<KisNativeResultProtocol::Snapshot> views;
        for (int i = 0; i < h; ++i) {
            views.push_back(f.protocol.capture(&why));
            require(f.protocol.write(data(i % 2 ? a : b), one, 256, &why), "scaled real retained generation");
        }
        const auto before = KisNativeResultTestAccess::counts(f.protocol);
        require(f.protocol.write(data(b), QRect(1, 1, 1, 1), 4, &why), "fixed K original result");
        const auto after = KisNativeResultTestAccess::counts(f.protocol);
#ifdef NR104_CENSUS
        require(after.results - before.results <= 1 && after.adapterInstalled - before.adapterInstalled <= 1,
            "fixed K never installs unrelated population");
#endif
        std::fprintf(stderr, "NATIVE_SCALE K=1 N=%d H=%d slots=%llu liveViews=%llu results=%llu installs=%llu\n",
            n, h, KisNativeResultTestAccess::state(f.protocol).pageSlots, KisNativeResultTestAccess::state(f.protocol).logicalViews,
            after.results-before.results, after.adapterInstalled-before.adapterInstalled);
    }
    std::fprintf(stderr, "NATIVE_PRESSURE_IDENTITY_OK\n");
}

void NativeResultProtocolContractTest::freshControlContracts() {
    const auto previous = qgetenv("KRITA_NATIVE_RESULT_FORCE_FRESH");
    qputenv("KRITA_NATIVE_RESULT_FORCE_FRESH", "1");
    for (int k : {1, 16}) {
        Fixture f; f.write(f.a);
        const auto before = KisNativeResultTestAccess::counts(f.protocol);
        QByteArray patch(64 * k * 4, char(0x35));
        for (int i = 0; i < 4; ++i) {
            patch.fill(char(0x35 + i));
            require(f.protocol.write(data(patch), QRect(0, 0, 64 * k, 1), patch.size(), &why), "protected fresh partial write");
            QByteArray read(patch.size(), 0);
            require(f.protocol.readCurrent(reinterpret_cast<quint8 *>(read.data()), QRect(0, 0, 64 * k, 1), patch.size(), &why) && read == patch, "protected fresh visible bytes");
        }
        const auto after = KisNativeResultTestAccess::counts(f.protocol);
#ifdef NR104_CENSUS
        require(after.fresh - before.fresh == quint64(4 * k) && after.handoff == before.handoff, "protected native fresh never silently uses handoff");
#endif
        require(f.protocol.close(&why), "protected fresh close"); requireDrained(f.protocol);
        std::fprintf(stderr, "NATIVE_FRESH_CONTROL K=%d fresh=%llu handoff=%llu\n", k, after.fresh-before.fresh, after.handoff-before.handoff);
    }
    if (previous.isNull()) qunsetenv("KRITA_NATIVE_RESULT_FORCE_FRESH"); else qputenv("KRITA_NATIVE_RESULT_FORCE_FRESH", previous);
}

void NativeResultProtocolContractTest::stoppedRuntimeContract() {
    KisNativeResultProtocol::ReadAccess late;
    {
        auto f = std::make_unique<Fixture>(); f->write(f->a);
        auto view = f->protocol.capture(&why); late = view.readPage(0, 0, &why); view = {};
        require(f->protocol.close(&why), "stop owner closes with real pin");
        kisDrainPageStoreReclamation();
    }
    kisStopPageStoreReclamation();
    require(late.data() && late.data()[0] == 0x19, "late real pin after permanent stop");
    late = {};
    std::fprintf(stderr, "NATIVE_STOP_LATE_RELEASE_OK\n");
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
#ifdef NR104_INSTALL_AUDIT
    struct Counts { size_t allocations, bytes, frees; };
    auto start = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "auditStart"));
    auto stop = reinterpret_cast<Counts (*)()>(dlsym(RTLD_DEFAULT, "auditStop"));
    require(start && stop, "real allocation interposer loaded");
    start(); void *allocation = std::malloc(173);
    asm volatile("" : "+r"(allocation) : : "memory"); std::free(allocation);
    const auto control = stop();
    require(control.allocations > 0 && control.frees > 0, "real allocator positive control");
    std::fprintf(stderr, "NATIVE_INSTALL_POSITIVE_CONTROL_OK\n");
#endif
    NativeResultProtocolContractTest::publicContracts();
    NativeResultProtocolContractTest::concurrentContracts();
    NativeResultProtocolContractTest::pressureIdentityContracts();
    NativeResultProtocolContractTest::freshControlContracts();
    {
        Fixture f; f.write(f.a); auto initial = f.protocol.capture(&why); require(bool(initial), "capture");
        for (int i = 0; i < 16; ++i) { const auto &v = i % 2 ? f.c : f.b; f.write(v); f.current(v); }
        auto final = f.protocol.capture(&why); f.historical(initial, f.a);
        require(f.protocol.restore(initial, &why), "undo"); f.current(f.a);
        require(f.protocol.restore(final, &why), "redo"); f.current(f.c);
        auto n = KisNativeResultTestAccess::counts(f.protocol);
        std::fprintf(stderr, "H1_CONTRACT fresh=%llu reuse=%llu results=%llu retained=%llu\n",
            n.fresh, n.handoff, n.results, n.retainedEdges);
    }
    {
        Fixture f; f.write(f.a);
        KisNativeResultTestAccess::Faults fail; fail.rejectAfterTargets = 7;
        require(!KisNativeResultTestAccess::prepareWrite(f.protocol, data(f.b), f.rect, 1024, &why, &fail), "producer failure"); f.current(f.a);
        auto ready = f.protocol.prepareWrite(data(f.b), f.rect, 1024, &why); require(bool(ready), "prepare");
        fail = {}; fail.rejectAccept = true;
        require(!KisNativeResultTestAccess::accept(f.protocol, ready, &why, &fail), "final reject"); f.current(f.a);
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
        require(!KisNativeResultTestAccess::swapIdle(f.protocol, &why), "domain positive peak refusal");
        f.current(f.b);
    }
    {
        KisPageBackingLimits limits; limits.idleCapacityBytes = 512 * 1024;
        Fixture f(limits); f.write(f.a); f.write(f.b);
        require(KisNativeResultTestAccess::swapIdle(f.protocol, &why), "real SSD swap");
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
            KisNativeResultTestAccess::Faults fault; fault.rejectAcceptAt = i;
            require(!KisNativeResultTestAccess::accept(f.protocol, p, &why, &fault), "each final target refusal"); f.current(f.a);
        }
        KisNativeResultProtocol::Snapshot late;
        KisNativeResultTestAccess::Faults lateCapture; lateCapture.beforeAccept = [&] { late = f.protocol.capture(&why); };
        require(!KisNativeResultTestAccess::accept(f.protocol, p, &why, &lateCapture), "late capture changes real fee plan"); f.current(f.a);
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
        const auto old = KisNativeResultTestAccess::counts(protocol); require(protocol.write(data(all), QRect(0, 0, 128, 128), 128 * bpp, &why), "no-op");
        const auto now = KisNativeResultTestAccess::counts(protocol); require(old.results == now.results && old.fresh == now.fresh && old.handoff == now.handoff, "no-op has no result services");
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
        const auto counts = KisNativeResultTestAccess::counts(protocol); require(counts.fresh == 864 && counts.handoff == 224, "real pressure F/H counts");
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
    NativeResultProtocolContractTest::stoppedRuntimeContract();
    std::fprintf(stderr, "NATIVE104_CONTRACT_OK\n");
}

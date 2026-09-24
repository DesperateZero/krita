/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <QTest>
#include <QScopeGuard>
#include <QSemaphore>
#include <atomic>
#include <thread>

#include "KisCpuResidentBinding_p.h"
#include "KisTiles3PageReplicaProvider.h"
#include "tiles3/kis_tile_data.h"
#include "tiles3/kis_tile_data_store.h"
#include "tiles3/kis_tile_data_store_iterators.h"

namespace {
const KisPageAccessRequirement cpu{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
KisPageKey key(int x) { return {{1}, {x, 0}}; }
KisPageAllocationDescriptor descriptor(int bpp)
{
    KisPageAllocationDescriptor d;
    d.layoutRevision = d.rowAlignment = 1;
    d.pageExtent = QSize(64, 64); d.validRect = QRect(0, 0, 64, 64);
    auto &f = d.format;
    f.formatId = 200 + bpp; f.colorModelId = "RGBA"; f.colorDepthId = "U8";
    f.profileFingerprint = "physical-claim-test"; f.channelOrder = "test";
    f.packing = "interleaved"; f.defaultPixel = QByteArray(bpp, char(0x31));
    f.channelCount = f.pixelStride = bpp; f.pixelAlignment = 1;
    f.hasAlpha = true; f.alphaSemantic = KisSurfaceAlphaSemantic::Premultiplied;
    f.endianness = KisSurfaceEndianness::NativeEndian; f.codecVersion = 1;
    return d;
}

// This fixture has no PageStore roots: it tests physical ownership only.
// Two accounted refs: one allocation record and one resident binding.
struct Fixture
{
    QSharedPointer<KisCompletionRegistry> completions = QSharedPointer<KisCompletionRegistry>::create();
    QSharedPointer<KisTiles3PageReplicaProvider> provider = QSharedPointer<KisTiles3PageReplicaProvider>::create();
    QSharedPointer<KisCpuResidentBinding> binding;
    KisReplicaHandle replica;
    KisPageAllocationDescriptor desc;
    KisTileData *tile = nullptr;
    QString error;
    bool init(int bpp = 4)
    {
        desc = descriptor(bpp);
        KisCpuResidentReplicaProviderConfig config;
        config.provider = {200}; config.providerEpoch = {1}; config.budgetBytes = 16 * 1024 * 1024;
        if (!provider->configure(config, completions, &error)) return false;
        auto allocation = provider->requestReplica({1}, {key(0), {1}}, desc, cpu.domain,
                                                   KisPageAccessMode::Read, KisPagePriority::Normal);
        if (!allocation.isValid()) return false;
        replica = allocation.replica;
        auto access = provider->resolveAccess({2}, {3}, replica, cpu, KisPageAccessMode::Read);
        if (!access.isValid()) return false;
        tile = provider->tileDataForLease({2});
        provider->releaseAccess(std::move(access), {});
        binding = provider->cpuResidentBinding(replica);
        return tile && binding;
    }
    bool swap()
    {
        return KisTileDataStore::instance()->trySwapTileData(tile);
    }
};

}

class KisPageStorePhysicalClaimTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void sourceConsumersRespectWriter_data();
    void sourceConsumersRespectWriter();
    void parkedWriterReservation_data();
    void parkedWriterReservation();
    void revokedWriterReservation_data();
    void revokedWriterReservation();
    void parkedWriterConcurrentConsumers();
};

void KisPageStorePhysicalClaimTest::parkedWriterReservation_data()
{
    QTest::addColumn<int>("bpp");
    for (int bpp : {1, 4, 8, 16}) QTest::newRow(qPrintable(QString::number(bpp))) << bpp;
}

void KisPageStorePhysicalClaimTest::parkedWriterReservation()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp));
    QVERIFY(f.binding->acquireRead(true));
    QVERIFY(!KisCpuWriteBindingReservation::acquire(f.binding).isValid());
    f.binding->releaseRead();
    auto writer = KisCpuWriteBindingReservation::acquire(f.binding); QVERIFY(writer.isValid());
    QVERIFY(!KisCpuWriteBindingReservation::acquire(f.binding).isValid());
    auto *bytes = static_cast<quint8 *>(writer.pinResident()); QVERIFY(bytes);
    memset(bytes, 0x73, size_t(f.desc.minimumByteSize()));
    QVERIFY(!f.swap());
    writer.unpin();
    // A parked reservation still excludes every binding consumer, but no
    // longer owns the physical swap barrier. No byte pointer may be reused.
    QVERIFY(!f.binding->acquireRead(true));
    QVERIFY(!f.binding->acquireWrite());
    QVERIFY(!f.binding->retire());
    QVERIFY(!f.provider->resolveAccess({10}, {11}, f.replica, cpu, KisPageAccessMode::Read).isValid());
    QVERIFY(!f.provider->prepareSynchronousWriteCopy({12}, f.replica, {key(0), {2}}, f.desc, KisPagePriority::Normal).isValid());
    QVERIFY(f.swap());
    KisCpuResidentReadStatus status;
    QVERIFY(!writer.pinResident(&status));
    QCOMPARE(status, KisCpuResidentReadStatus::NonResident);
    auto moved = std::move(writer); QVERIFY(!writer.isValid());
    QVERIFY(!writer.materialize());
    bytes = static_cast<quint8 *>(moved.materialize()); QVERIFY(bytes);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(bytes), int(f.desc.minimumByteSize())),
             QByteArray(int(f.desc.minimumByteSize()), char(0x73)));
    writer = std::move(moved); QVERIFY(!moved.isValid());
    writer.reset();
    const auto *read = static_cast<const quint8 *>(f.binding->acquireRead(true)); QVERIFY(read);
    QCOMPARE(read[0], quint8(0x73));
    f.binding->releaseRead();
    QVERIFY(f.provider->retire({20}, f.replica, {}).isValid());
}

void KisPageStorePhysicalClaimTest::revokedWriterReservation_data()
{
    QTest::addColumn<bool>("pinned");
    QTest::newRow("parked") << false;
    QTest::newRow("pinned") << true;
}

void KisPageStorePhysicalClaimTest::revokedWriterReservation()
{
    QFETCH(bool, pinned);
    Fixture f; QVERIFY(f.init());
    auto writer = KisCpuWriteBindingReservation::acquire(f.binding); QVERIFY(writer.isValid());
    auto *bytes = pinned ? static_cast<quint8 *>(writer.pinResident()) : nullptr;
    if (pinned) { QVERIFY(bytes); bytes[0] = 0x76; }
    f.provider.clear(); // revoke may not free a borrowed pointer/token's backing
    if (pinned) QCOMPARE(bytes[0], quint8(0x76));
    writer.unpin();
    KisCpuResidentReadStatus status;
    QVERIFY(!writer.pinResident(&status)); QCOMPARE(status, KisCpuResidentReadStatus::Retired);
    QVERIFY(!writer.materialize());
    writer.reset();
    QVERIFY(!f.binding->acquireRead(true));
}

void KisPageStorePhysicalClaimTest::parkedWriterConcurrentConsumers()
{
    Fixture f; QVERIFY(f.init());
    auto writer = KisCpuWriteBindingReservation::acquire(f.binding); QVERIFY(writer.isValid());
    QSemaphore inspect, inspected;
    std::atomic<bool> stop{false}, failed{false};
    std::thread consumer([&] {
        for (;;) {
            inspect.acquire();
            if (stop.load()) break;
            KisCpuResidentReadStatus status;
            const void *read = f.binding->acquireRead(true, &status, true);
            if (read) { failed = true; f.binding->releaseRead(); }
            if (status != KisCpuResidentReadStatus::Busy) failed = true;
            auto generic = f.provider->resolveAccess({20}, {21}, f.replica, cpu, KisPageAccessMode::Read);
            if (generic.isValid()) { failed = true; f.provider->releaseAccess(std::move(generic), {}); }
            if (f.provider->prepareSynchronousWriteCopy({22}, f.replica, {key(0), {2}}, f.desc,
                    KisPagePriority::Normal).isValid()) failed = true;
            if (f.binding->retire()) failed = true;
            inspected.release();
        }
    });
    const auto join = qScopeGuard([&] {
        stop = true; inspect.release();
        if (consumer.joinable()) consumer.join();
    });
    for (int i = 0; i < 64; ++i) {
        auto *bytes = static_cast<quint8 *>(writer.pinResident()); QVERIFY(bytes);
        bytes[0] = quint8(0x40 + i);
        inspect.release(); QVERIFY(inspected.tryAcquire(1, 5000));
        writer.unpin();
        inspect.release(); QVERIFY(inspected.tryAcquire(1, 5000));
    }
    stop = true; inspect.release(); consumer.join();
    QVERIFY(!failed.load());
    writer.reset();
    const auto *read = static_cast<const quint8 *>(f.binding->acquireRead(true)); QVERIFY(read);
    QCOMPARE(read[0], quint8(0x7f));
    f.binding->releaseRead();
}

void KisPageStorePhysicalClaimTest::sourceConsumersRespectWriter_data()
{
    QTest::addColumn<int>("writerMode");
    QTest::addColumn<bool>("preclone");
    QTest::newRow("generic") << 0 << false;
    QTest::newRow("native") << 1 << false;
    QTest::newRow("native-preclone") << 1 << true;
    QTest::newRow("parked") << 3 << false;
    QTest::newRow("parked-preclone") << 3 << true;
}

void KisPageStorePhysicalClaimTest::sourceConsumersRespectWriter()
{
    QFETCH(int, writerMode); QFETCH(bool, preclone);
    Fixture f; QVERIFY(f.init());
    const auto target = f.provider->prepareWrite({10}, {key(0), {2}}, f.desc, cpu.domain,
                                                KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
    QVERIFY(target.isValid());
    if (preclone) {
        QVERIFY(f.tile->blockSwapping());
        f.tile->m_clonesStack.push(new KisTileData(*f.tile, false));
        f.tile->unblockSwapping();
    }
    KisReplicaAccess generic = writerMode == 0
        ? f.provider->resolveAccess({11}, {12}, f.replica, cpu, KisPageAccessMode::Write)
        : KisReplicaAccess{};
    void *data = nullptr;
    KisCpuWriteBindingReservation parked;
    if (writerMode == 0) {
        data = generic.cpuWriteData;
    } else if (writerMode == 3) {
        parked = KisCpuWriteBindingReservation::acquire(f.binding);
        QVERIFY(parked.pinResident());
        parked.unpin();
    } else {
        data = f.binding->acquireWrite();
    }
    QVERIFY(writerMode == 3 ? parked.isValid() : data != nullptr);
    bool held = true;
    auto releaseWriter = [&] {
        if (!held) return;
        if (writerMode == 0) f.provider->releaseAccess(std::move(generic), {});
        else if (writerMode == 3) parked.reset();
        else f.binding->releaseWrite();
        held = false;
    };
    const auto release = qScopeGuard(releaseWriter);
    const auto before = f.provider->payloadWork();
    const auto deniedCopy = f.provider->prepareSynchronousWriteCopy({20}, f.replica,
        {key(0), {2}}, f.desc, KisPagePriority::Normal);
    QVERIFY2(!deniedCopy.isValid(), "Native source-copy must not bypass an active binding writer");
    QCOMPARE(f.provider->payloadWork().nativeDuplicatePages, before.nativeDuplicatePages);
    if (preclone) QCOMPARE(f.tile->m_clonesStack.size(), 1);

    KisReplicaTransferRequest transfer;
    transfer.operation = {21}; transfer.source = f.replica; transfer.target = target.replica; transfer.descriptor = f.desc;
    transfer.kind = KisReplicaTransferKind::WriteGenerationInitialization;
    QVERIFY(transfer.isSameProviderTransfer());
    QVERIFY(!f.provider->transfer(transfer, KisPagePriority::Normal).isValid());
    QVERIFY(!f.provider->retire({22}, f.replica, {}).isValid());
    QVERIFY(!f.provider->resolveAccess({23}, {24}, f.replica, cpu, KisPageAccessMode::Read).isValid());
    releaseWriter();

    const auto copy = f.provider->prepareSynchronousWriteCopy({25}, f.replica,
        {key(0), {2}}, f.desc, KisPagePriority::Normal);
    QVERIFY(copy.isValid());
    QCOMPARE(f.provider->payloadWork().nativeDuplicatePages, before.nativeDuplicatePages + 1);
    QCOMPARE(f.provider->payloadWork().nativePrecloneHits, before.nativePrecloneHits + (preclone ? 1 : 0));
    auto copiedBinding = f.provider->cpuResidentBinding(copy.replica); QVERIFY(copiedBinding);
    const auto *bytes = static_cast<const quint8 *>(copiedBinding->acquireRead(false)); QVERIFY(bytes);
    const bool correct = bytes[0] == 0x31 && bytes[copy.replica.layout.byteSize - 1] == 0x31;
    copiedBinding->releaseRead(); QVERIFY(correct);
    transfer.operation = {26}; QVERIFY(f.provider->transfer(transfer, KisPagePriority::Normal).isValid());
    QVERIFY(f.provider->retire({27}, copy.replica, {}).isValid());
    QVERIFY(f.provider->retire({28}, f.replica, {}).isValid());
    QVERIFY(f.provider->retire({29}, target.replica, {}).isValid());
}

QTEST_GUILESS_MAIN(KisPageStorePhysicalClaimTest)
#include "KisPageStorePhysicalClaimTest.moc"

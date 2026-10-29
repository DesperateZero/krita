#include "pagestore/KisPageOwnerLedgerTestAccess.h"
/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <QTest>
#include <QScopeGuard>
#include <QSemaphore>
#include <QElapsedTimer>
#include <atomic>
#include <thread>
#include <memory>
#include <cstring>

#include "KisCpuResidentBinding_p.h"
#include "KisTiles3PageReplicaProvider.h"
#include "KisPageOwnerLedger.h"
#include "KisPageWriteCoordinator_p.h"
#include "pagestore/KisPageStoreStoragePressure.h"
#include "KisPageRetirementRecord_p.h"
#include "KisPageRetirementQueue_p.h"
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
// The stable resident binding owns the sole TileData ref when idle.
struct Fixture
{
    std::shared_ptr<KisCompletionRegistry> completions = std::make_shared<KisCompletionRegistry>();
    std::shared_ptr<KisTiles3PageReplicaProvider> provider = std::make_shared<KisTiles3PageReplicaProvider>();
    std::shared_ptr<KisCpuResidentBinding> binding;
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

quint64 retirementStorageBytes(KisBackingBudgetController &budget)
{
    const auto live = [&] {
        return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    // Warm the retained accounting cache, then measure the complete real node.
    { auto warm = kisPreparePageRetirementRecord(&budget); }
    const quint64 before = live();
    auto record = kisPreparePageRetirementRecord(&budget);
    return live() - before;
}

}

class KisPageStorePhysicalClaimTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void freshResourceDelivery_data();
    void freshResourceDelivery();
    void freshCopyRequiresOriginalSource_data();
    void freshCopyRequiresOriginalSource();
    void terminalConsumptionWaitsForClassAcceptance();
    void unfundedRetirementPreservesCharge_data();
    void unfundedRetirementPreservesCharge();
    void mappedFinalReferenceCannotBeRetained_data();
    void mappedFinalReferenceCannotBeRetained();
    void residentBindingStorageFollowsLastHandle_data();
    void residentBindingStorageFollowsLastHandle();
    void retirementReadiness_data();
    void retirementReadiness();
    void tileReadCacheLifetime_data();
    void tileReadCacheLifetime();
    void sourceConsumersRespectWriter_data();
    void sourceConsumersRespectWriter();
    void parkedWriterReservation_data();
    void parkedWriterReservation();
    void revokedWriterReservation_data();
    void revokedWriterReservation();
    void parkedWriterConcurrentConsumers();
    void cachedBindingRequiresExactIdentity_data();
    void cachedBindingRequiresExactIdentity();
    void physicalBackingHandoffGuards_data();
    void physicalBackingHandoffGuards();
    void physicalBackingHandoffLifecycle_data();
    void physicalBackingHandoffLifecycle();
    void physicalBackingHandoffConcurrentConsumers();
    void physicalBackingHandoffRetainsProvider();
    void handoffExcludesNewOwner_data()
    {
        QTest::addColumn<bool>("commit");
        QTest::newRow("cancel") << false; QTest::newRow("commit") << true;
    }
    void handoffExcludesNewOwner();
    void physicalBackingHandoffRejectsWrongIdentity_data();
    void physicalBackingHandoffRejectsWrongIdentity();
    void backingHandoffAccounting_data();
    void backingHandoffAccounting();
    void backingHandoffAccountingGuards_data();
    void backingHandoffAccountingGuards();
    void backingHandoffAccountingAcrossIndexGrowth();
    void backingHandoffReusesFundedPreparation_data();
    void backingHandoffReusesFundedPreparation();
    void handoffDebtFollowsTerminal_data();
    void handoffDebtFollowsTerminal();
    void sharedHandoffDebt_data();
    void sharedHandoffDebt();
    void ledgerAliasMembershipAtCapacity_data()
    {
        QTest::addColumn<bool>("reverse");
        QTest::newRow("middle-account-owner") << false;
        QTest::newRow("head-account-owner") << true;
    }
    void ledgerAliasMembershipAtCapacity();
    void wideAliasClassCapacity();
    void sharedCacheClassificationAndDebt_data()
    {
        QTest::addColumn<int>("bpp");
        QTest::addColumn<bool>("alias");
        for (int bpp : {1, 4, 8, 16}) for (bool alias : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-alias%2").arg(bpp).arg(alias))) << bpp << alias;
    }
    void sharedCacheClassificationAndDebt();
    void sharedOrphanRetainsOnlyDebt();
    void optionalCapacityCheckout_data();
    void optionalCapacityCheckout();
};

void KisPageStorePhysicalClaimTest::freshResourceDelivery_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("kind"); QTest::addColumn<bool>("external");
    for (int bpp : {1, 4, 8, 16}) for (int kind = 0; kind != 5; ++kind) for (bool external : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("bpp%1-kind%2-external%3").arg(bpp).arg(kind).arg(external)))
            << bpp << kind << external;
}

void KisPageStorePhysicalClaimTest::freshResourceDelivery()
{
    QFETCH(int, bpp); QFETCH(int, kind); QFETCH(bool, external);
    Fixture f; QVERIFY(f.init(bpp));
    auto *initial = f.binding->acquireWrite(f.replica.allocationIdentity()); QVERIFY(initial);
    std::memset(initial, 0x55, size_t(f.desc.minimumByteSize())); f.binding->releaseWrite();
    std::shared_ptr<const KisPageReplicaSource> source;
    if (kind >= 3) { source = f.provider->captureCompletedTileSource(f.desc, f.tile); QVERIFY(source); }
    const QByteArray bytes(qsizetype(f.desc.minimumByteSize()), char(0x69));
    const KisCpuPagePayload payload{bytes.constData(), qsizetype(f.desc.minimumRowBytes()), bytes.size()};
    const KisPageVersion version{key(0), {2}};
    // Native creation needs no fabricated public operation or ready ticket.
    const KisCpuReplicaInput input{KisCpuReplicaInput::Kind(kind), external ? KisPageOperationId{10} : KisPageOperationId{},
        version, f.desc, KisPageWriteMode::PreserveContents, KisPagePriority::Normal,
        f.replica, f.binding, &payload, source};
    auto result = external ? f.provider->KisPageReplicaProvider::prepareCpuReplica(input)
                           : f.provider->prepareCpuReplica(input);
    QCOMPARE(result.nativeReady(), !external);
    QCOMPARE(bool(result.externalOperation()), external);
    if (external) {
        QVERIFY(result.externalOperation()->isValid());
        QCOMPARE(result.externalOperation()->operation, input.operation);
        QVERIFY(f.completions->verifyTerminal(result.externalOperation()->completion).succeeded());
    }
    const auto target = result.replica(version); QVERIFY(target.isValid()); QCOMPARE(target.version, version);
    auto binding = external ? f.provider->cpuResidentBinding(target) : result.takeNativeBinding();
    QVERIFY(binding); QVERIFY(binding->matchesHandle(target));
    QCOMPARE(binding.get(), f.provider->cpuResidentBinding(target).get());
    if (!external) { QVERIFY(!result.nativeReady()); QVERIFY(!result.replica(version).isValid()); }
    const auto *data = static_cast<const char *>(binding->acquireRead(target.allocationIdentity(), false));
    QVERIFY(data);
    const auto release = qScopeGuard([&] { if (data) binding->releaseRead(); });
    QCOMPARE(QByteArray(data, bytes.size()), QByteArray(bytes.size(), kind == 0 ? char(0x31) : kind == 2 ? char(0x69) : char(0x55)));
    QCOMPARE(data == reinterpret_cast<const char *>(f.tile->data()), kind == 4);
    binding->releaseRead(); data = nullptr;
    QVERIFY(f.provider->retire({11}, target, {}).isValid());
    QVERIFY(!binding->acquireRead(target.allocationIdentity(), false));
    // The public adapter still consumes replay identity, even when its pixel
    // implementation is shared with the operation-free native factory.
    if (external) QVERIFY(!f.provider->KisPageReplicaProvider::prepareCpuReplica(input).externalOperation()->isValid());
}

void KisPageStorePhysicalClaimTest::freshCopyRequiresOriginalSource_data()
{
    QTest::addColumn<int>("failure");
    for (int i = 0; i != 4; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
}

void KisPageStorePhysicalClaimTest::freshCopyRequiresOriginalSource()
{
    QFETCH(int, failure);
    Fixture f, foreign; QVERIFY(f.init()); QVERIFY(foreign.init());
    const KisPageVersion version{key(0), {2}};
    auto source = f.replica;
    auto binding = failure == 0 ? foreign.binding : f.binding;
    std::shared_ptr<const KisPageReplicaSource> initialization;
    // The foreign provider deliberately has identical exported ids/slots.
    if (failure == 1) ++source.version.generation.value;
    if (failure == 2) QVERIFY(f.binding->acquireWrite(f.replica.allocationIdentity()));
    const auto release = qScopeGuard([&] { if (failure == 2) f.binding->releaseWrite(); });
    if (failure == 3) QVERIFY(f.provider->retire({12}, f.replica, {}).isValid());
    const auto before = f.provider->payloadWork();
    auto result = f.provider->prepareCpuReplica({KisCpuReplicaInput::Kind::Copy, {}, version, f.desc,
        KisPageWriteMode::PreserveContents, KisPagePriority::Normal, source, binding, nullptr, initialization});
    QVERIFY(!result.nativeReady()); QVERIFY(!result.replica(version).isValid()); QVERIFY(!result.error().isEmpty());
    QCOMPARE(f.provider->payloadWork().nativeDuplicatePages, before.nativeDuplicatePages);
}

void KisPageStorePhysicalClaimTest::mappedFinalReferenceCannotBeRetained_data()
{
    QTest::addColumn<int>("bpp");
    for (int bpp : {1, 4, 8, 16})
        QTest::newRow(qPrintable(QString::number(bpp))) << bpp;
}

void KisPageStorePhysicalClaimTest::mappedFinalReferenceCannotBeRetained()
{
    QFETCH(int, bpp);
    auto *store = KisTileDataStore::instance();
    const auto before = store->memoryMetric();
    const quint8 pixel[16] = {0x31};
    auto *tile = store->createDefaultTileData(bpp, pixel);
    QVERIFY(tile); QVERIFY(tile->ref()); // Initial cold ownership still works.
    QVERIFY(tile->tryRef()); QCOMPARE(tile->m_refCount.loadAcquire(), 2);
    QVERIFY(tile->deref());
    auto *iterator = store->beginIteration();
    std::thread releaser([&] { tile->deref(); });
    auto release = qScopeGuard([&] {
        if (iterator) store->endIteration(iterator);
        if (releaser.joinable()) releaser.join();
    });
    QElapsedTimer timer; timer.start();
    while (tile->m_refCount.loadAcquire() != 0 && timer.elapsed() < 5000)
        std::this_thread::yield();
    const int terminalCount = tile->m_refCount.loadAcquire();
    const bool retained = tile->tryRef();
    const int after = tile->m_refCount.loadAcquire();
    // The original final free owns this node even if a broken map pin revives
    // its count. Never dereference that pointer after releasing the barrier.
    store->endIteration(iterator); iterator = nullptr;
    releaser.join();
    QCOMPARE(terminalCount, 0); QVERIFY(!retained); QCOMPARE(after, 0);
    QCOMPARE(store->memoryMetric(), before);
}

void KisPageStorePhysicalClaimTest::residentBindingStorageFollowsLastHandle_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("outcome");
    for (int bpp : {1, 4, 8, 16}) for (int outcome = 0; outcome < 3; ++outcome)
        QTest::newRow(qPrintable(QStringLiteral("bpp%1-outcome%2").arg(bpp).arg(outcome))) << bpp << outcome;
}

void KisPageStorePhysicalClaimTest::residentBindingStorageFollowsLastHandle()
{
    QFETCH(int, bpp); QFETCH(int, outcome);
    KisPageBackingLimits limits; limits.metadataArenaBytes = 64 * 1024;
    auto process = std::make_shared<KisBackingBudgetController>(limits);
    auto first = process->reserve({}, nullptr), second = process->reserve({}, nullptr);
    QVERIFY(first.isValid() && second.isValid()); first.release(); second.release();
    const auto live = [&] { return process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const auto baseline = live();
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<KisTiles3PageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig config;
    config.provider = {200}; config.providerEpoch = {1}; config.budgetBytes = 1024 * 1024;
    const auto desc = descriptor(bpp);
    QString error;
    QVERIFY2(provider->configure(config, completions, &error, process), qPrintable(error));
    const auto allocation = provider->requestReplica({1}, {key(0), {1}}, desc, cpu.domain,
        KisPageAccessMode::Read, KisPagePriority::Normal);
    QVERIFY2(allocation.isValid(), qPrintable(allocation.error));
    auto binding = provider->cpuResidentBinding(allocation.replica); QVERIFY(binding);
    std::weak_ptr<KisCpuResidentBinding> weak(binding);
    bool pinned = false;
    const auto unpin = qScopeGuard([&] { if (pinned) binding->releaseRead(); });
    const void *bytes = nullptr;
    if (outcome == 2) {
        bytes = binding->acquireRead(allocation.replica.allocationIdentity(), true); QVERIFY(bytes);
        pinned = true;
    } else if (outcome == 0) {
        const auto retired = provider->retire({2}, allocation.replica, {});
        QVERIFY2(retired.isValid(), qPrintable(retired.error));
        QVERIFY(completions->verifyTerminal(retired.completion).succeeded());
        QVERIFY(!provider->cpuResidentBinding(allocation.replica));
    }
    provider.reset(); // Its controller exits; the original allocation stays charged.
    KisCpuResidentReadStatus status;
    QVERIFY(!binding->acquireRead(allocation.replica.allocationIdentity(), true, &status));
    QCOMPARE(status, KisCpuResidentReadStatus::Retired);
    if (pinned) {
        QCOMPARE(QByteArray(static_cast<const char *>(bytes), int(desc.minimumByteSize())),
                 QByteArray(int(desc.minimumByteSize()), char(0x31)));
        binding->releaseRead(); pinned = false;
    }
    const auto tail = live(); QVERIFY(tail > baseline + sizeof(KisCpuResidentBinding));
    binding.reset(); QVERIFY(weak.expired());
    QCOMPARE(live(), tail); // allocate_shared storage is still held by the weak control block.
    weak.reset();
    QCOMPARE(live(), baseline);
    for (const auto &bucket : process->usage().buckets) QCOMPARE(bucket.reserved.cpuRam, quint64(0));
}

void KisPageStorePhysicalClaimTest::retirementReadiness_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("mode"); QTest::addColumn<bool>("revoke");
    for (int bpp : {1, 4, 8, 16}) for (int mode = 0; mode < 3; ++mode) for (bool revoke : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("bpp%1-mode%2-revoke%3").arg(bpp).arg(mode).arg(revoke)))
            << bpp << mode << revoke;
}

void KisPageStorePhysicalClaimTest::retirementReadiness()
{
    QFETCH(int, bpp); QFETCH(int, mode); QFETCH(bool, revoke);
    Fixture f; QVERIFY(f.init(bpp));
    const auto expected = f.replica.allocationIdentity();
    int calls = 0;
    KisPageReadinessSubscription subscription;
    const auto schedule = [&] { ++calls; };
    QCOMPARE(f.binding->watchRetirementReadiness(expected, schedule, &subscription),
             KisPageReadinessStatus::Ready);
    QVERIFY(!subscription.isValid());
    KisCpuWriteBindingReservation writer;
    if (mode == 0) {
        QVERIFY(f.binding->acquireRead(expected, true));
        QVERIFY(f.binding->acquireRead(expected, true));
    } else if (mode == 1) {
        QVERIFY(f.binding->acquireWrite(expected));
    } else {
        writer = KisCpuWriteBindingReservation::acquire(f.binding, expected);
        QVERIFY(writer.isValid()); QVERIFY(writer.pinResident());
    }
    auto cleanup = qScopeGuard([&] {
        if (mode == 0) { f.binding->releaseRead(); f.binding->releaseRead(); }
        else if (mode == 1) f.binding->releaseWrite();
        else writer.reset();
    });
    auto wrong = expected; ++wrong.allocation.generation;
    QCOMPARE(f.binding->watchRetirementReadiness(wrong, schedule, &subscription),
             KisPageReadinessStatus::Unavailable);
    QCOMPARE(f.binding->watchRetirementReadiness(expected, schedule, &subscription),
             KisPageReadinessStatus::Waiting);
    QCOMPARE(calls, 0);
    if (mode == 2) { writer.unpin(); QCOMPARE(calls, 0); }
    if (revoke) { f.binding->revoke(); QCOMPARE(calls, 1); }
    if (mode == 0) {
        f.binding->releaseRead(); QCOMPARE(calls, int(revoke));
        f.binding->releaseRead();
    } else if (mode == 1) f.binding->releaseWrite();
    else writer.reset();
    cleanup.dismiss();
    QCOMPARE(calls, 1);
    QCOMPARE(f.binding->watchRetirementReadiness(expected, schedule, &subscription),
             revoke ? KisPageReadinessStatus::Unavailable : KisPageReadinessStatus::Ready);
    if (!revoke) QVERIFY(f.provider->retire({100}, f.replica, {}).isValid());
}

void KisPageStorePhysicalClaimTest::tileReadCacheLifetime_data()
{
    QTest::addColumn<int>("bpp");
    QTest::addColumn<int>("terminal");
    for (int bpp : {1, 4, 8, 16}) {
        for (int terminal = 0; terminal != 3; ++terminal)
            QTest::newRow(qPrintable(QString("bpp%1-terminal%2").arg(bpp).arg(terminal))) << bpp << terminal;
    }
}

void KisPageStorePhysicalClaimTest::tileReadCacheLifetime()
{
    QFETCH(int, bpp);
    QFETCH(int, terminal);
    Fixture f; QVERIFY(f.init(bpp));
    QVERIFY(!f.provider->acquireTileReadCache(KisPageLeaseId{999}));
    auto access = f.provider->resolveAccess({110}, {111}, f.replica, cpu, KisPageAccessMode::Read);
    QVERIFY(access.isValid());
    auto cache = f.provider->acquireTileReadCache(KisPageLeaseId{110});
    QVERIFY(cache && cache->readPinned());
    QCOMPARE(cache->tileData(), f.tile);
    f.provider->releaseAccess(std::move(access), {});
    QVERIFY(!f.swap());
    auto handoff = KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc);
    QVERIFY(!handoff.isValid()); // Already pinned: no candidate is retained for a later retry.
    QVERIFY(!handoff.tryClaim());
    QVERIFY(!f.provider->retire({112}, f.replica, {}).isValid());
    QVERIFY(cache->finish());
    QVERIFY(cache->finish());
    QVERIFY(!cache->readPinned());
    QVERIFY(!cache->tileData());
    QVERIFY(f.swap());
    QCOMPARE(cache->repinRead(), KisTilePageStoreLease::ReadPinResult::Ready);
    QCOMPARE(cache->tileData()->data()[0], quint8(0x31));
    QVERIFY(cache->finish());

    auto writer = KisCpuWriteBindingReservation::acquire(f.binding, f.replica.allocationIdentity());
    QVERIFY(writer.isValid());
    QCOMPARE(cache->repinRead(), KisTilePageStoreLease::ReadPinResult::Unavailable);
    writer.reset();
    auto *sameCache = cache.get();
    auto secondAccess = f.provider->resolveAccess({113}, {114}, f.replica, cpu, KisPageAccessMode::Read);
    QVERIFY(secondAccess.isValid());
    cache = f.provider->acquireTileReadCache(KisPageLeaseId{113}, std::move(cache));
    QCOMPARE(cache.get(), sameCache);
    QVERIFY(cache->readPinned());
    f.provider->releaseAccess(std::move(secondAccess), {});
    QVERIFY(cache->finish());

    if (terminal == 0) {
        // No legacy wrapper/storage ref in this primitive fixture: the idle
        // candidate itself neither pins RAM nor claims a canonical version.
        handoff = KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc);
        QVERIFY(handoff.tryClaim());
        auto target = handoff.target();
        auto reserved = handoff.commit();
        QVERIFY(reserved.isValid());
        static_cast<quint8 *>(reserved.materialize())[0] = 0x77;
        reserved.reset(); handoff.reset();
        QCOMPARE(cache->repinRead(), KisTilePageStoreLease::ReadPinResult::Stale);
        QVERIFY(!cache->tileData());
        QVERIFY(f.provider->retire({115}, target, {}).isValid());
    } else {
        handoff.reset();
        if (terminal == 1) QVERIFY(f.provider->retire({116}, f.replica, {}).isValid());
        else f.provider.reset();
        QCOMPARE(cache->repinRead(), KisTilePageStoreLease::ReadPinResult::Retired);
        QVERIFY(!cache->tileData());
    }
}

void KisPageStorePhysicalClaimTest::handoffExcludesNewOwner()
{
    QFETCH(bool, commit);
    Fixture f; QVERIFY(f.init());
    KisPageOwnerLedger owner, other;
    QVERIFY(owner.configure(f.completions)); QVERIFY(owner.registerProvider(f.provider));
    QVERIFY(other.configure(f.completions));
    auto candidate = KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc);
    QVERIFY(candidate.tryClaim());
    QVERIFY(!other.registerProvider(f.provider, &f.error));
    QVERIFY(f.error.contains(QStringLiteral("handoff")));
    auto current = f.replica;
    if (commit) {
        current = candidate.target();
        auto writer = candidate.commit(); QVERIFY(writer.isValid()); writer.reset();
    }
    candidate.reset();
    QVERIFY2(other.registerProvider(f.provider, &f.error), qPrintable(f.error));
    auto denied = KisCpuBackingHandoff::prepare(f.provider, current, {key(0), {3}}, f.desc);
    QVERIFY(denied.isValid()); QVERIFY(!denied.tryClaim()); denied.reset();
    QVERIFY(f.provider->validate(current, f.desc));
    QVERIFY(f.provider->retire({99}, current, {}).isValid());
}

void KisPageStorePhysicalClaimTest::handoffDebtFollowsTerminal_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("swapped"); QTest::addColumn<int>("finish");
    for (int bpp : {1, 4, 8, 16}) for (bool swapped : {false, true}) for (int finish = 0; finish < 4; ++finish)
        QTest::newRow(qPrintable(QString("bpp%1-ssd%2-terminal%3").arg(bpp).arg(swapped).arg(finish))) << bpp << swapped << finish;
}

void KisPageStorePhysicalClaimTest::handoffDebtFollowsTerminal()
{
    QFETCH(int, bpp); QFETCH(bool, swapped); QFETCH(int, finish);
    Fixture f; QVERIFY(f.init(bpp)); const quint64 bytes = f.replica.layout.byteSize;
    KisPageBackingLimits limits; limits.retirementDebtBytes = limits.activePendingBytes = limits.durableStoreCapacity = bytes;
    KisBackingBudgetController budget(limits); KisPageOwnerLedger ledger;
    QVERIFY(ledger.configure(f.completions)); ledger.attachBackingBudget(budget); QVERIFY(ledger.registerProvider(f.provider));
    KisBackingBudgetDelta initial; initial.bucket(size_t(KisBackingBudgetClass::Current)).cpuRam = bytes;
    auto reservation = budget.reserve(initial, &f.error); QVERIFY(ledger.registerBacking(f.replica, reservation, KisBackingBudgetClass::Current));
    auto target = f.replica;
    // The second handoff must reuse the original headroom at an exact one-page
    // limit, not demand another retirement or pending reservation.
    for (quint64 generation : {2u, 3u}) {
        auto physical = KisCpuBackingHandoff::prepare(f.provider, target, {key(0), {generation}}, f.desc);
        const auto targetReplica = physical.target();
        auto accounting = ledger.prepareBackingHandoff(target, targetReplica, &f.error);
        QVERIFY2(accounting.isValid(), qPrintable(f.error)); QVERIFY(physical.tryClaim());
        auto writer = physical.commit(); QVERIFY(writer.isValid());
        QVERIFY(ledger.commitBackingHandoff(std::move(accounting))); target = targetReplica;
        auto *data = static_cast<quint8 *>(writer.pinResident()); QVERIFY(data); data[0] = 0x75; writer.reset();
    }
    const size_t debt = size_t(KisBackingBudgetClass::RetirementDebt);
    if (swapped) QVERIFY(f.swap());
    QVERIFY(ledger.synchronizeBackingDomains(&f.error));
    auto usage = budget.usage();
    QCOMPARE(usage.buckets[debt].reserved.cpuRam, swapped ? 0u : bytes);
    QCOMPARE(usage.buckets[debt].reserved.ssd, swapped ? bytes : 0u);
    KisBackingBudgetDelta pressure; pressure.bucket(debt).cpuRam = 1;
    QVERIFY(!budget.reserve(pressure, &f.error).isValid());
    if (finish == 0) {
        QVERIFY2(ledger.reclassifyBacking(target, KisBackingBudgetClass::RetirementDebt, &f.error), qPrintable(f.error));
    } else if (finish < 3) {
        KisPageTransitionEffect effect; effect.replica = target;
        if (finish == 2) {
            bool entered = false;
            QVERIFY(!ledger.acceptRetirementDebt(&effect, 1, &entered,
                [](void *p) { *static_cast<bool *>(p) = true; return false; }, &f.error));
            QVERIFY(entered);
            QCOMPARE(ledger.backingClass(target), KisBackingBudgetClass::ActivePending);
            QVERIFY(!budget.reserve(pressure, &f.error).isValid());
            // Cancellation keeps original headroom through a real domain move.
            if (swapped) {
                auto access = f.provider->resolveAccess({610}, {611}, target, cpu, KisPageAccessMode::Read);
                QVERIFY(access.isValid()); QCOMPARE(static_cast<const quint8 *>(access.cpuReadData)[0], quint8(0x75));
                f.provider->releaseAccess(std::move(access), {}); QVERIFY(f.swap());
            }
        }
        QVERIFY2(ledger.acceptRetirementDebt(&effect, 1, nullptr,
            [](void *) { return true; }, &f.error), qPrintable(f.error));
    } else {
        QVERIFY2(KisPageOwnerLedgerTestAccess::changeClasses(ledger,
            {{target, KisBackingBudgetClass::ActivePending, KisBackingBudgetClass::Current}}, &f.error), qPrintable(f.error));
        usage = budget.usage(); QCOMPARE(usage.buckets[debt].reserved.cpuRam, 0u); QCOMPARE(usage.buckets[debt].reserved.ssd, 0u);
        auto available = budget.reserve(pressure, &f.error); QVERIFY(available.isValid()); available.release();
        QVERIFY(ledger.reclassifyBacking(target, KisBackingBudgetClass::RetirementDebt, &f.error));
    }
    QCOMPARE(ledger.backingClass(target), KisBackingBudgetClass::RetirementDebt);
    const auto terminalBytes = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    QVERIFY(f.provider->retire({612}, target, {}).isValid()); KisPageOwnerLedgerTestAccess::withdrawRegistration(ledger, target);
    const quint64 retainedCache = terminalBytes - retirementStorageBytes(budget);
    const auto finalUsage = budget.usage();
    for (size_t i = 0; i < finalUsage.buckets.size(); ++i) {
        const auto &bucket = finalUsage.buckets[i];
        QCOMPARE(bucket.live.cpuRam, i == size_t(KisBackingBudgetClass::MetadataArena) ? retainedCache : quint64(0)); QCOMPARE(bucket.live.ssd, 0u);
        QCOMPARE(bucket.reserved.cpuRam, 0u); QCOMPARE(bucket.reserved.ssd, 0u);
    }
}

void KisPageStorePhysicalClaimTest::terminalConsumptionWaitsForClassAcceptance()
{
    Fixture f; QVERIFY(f.init());
    KisBackingBudgetController budget; KisPageOwnerLedger owner;
    QVERIFY(owner.configure(f.completions)); owner.attachBackingBudget(budget);
    QVERIFY(owner.registerProvider(f.provider));
    KisBackingBudgetDelta delta;
    delta.bucket(size_t(KisBackingBudgetClass::Current)).cpuRam = f.replica.layout.byteSize;
    auto fee = budget.reserve(delta, &f.error);
    QVERIFY(owner.registerBacking(f.replica, fee, KisBackingBudgetClass::Current));
    auto source = f.provider->captureCompletedTileSource(f.desc, f.tile); QVERIFY(source);
    auto aliasOperation = owner.nextOperationId();
    while (aliasOperation.value <= 3) aliasOperation = owner.nextOperationId(); // Fixture used ids 1–3.
    const auto alias = f.provider->prepareSynchronousSource(aliasOperation, source, {key(1), {1}}, f.desc,
        KisReplicaSourceUse::ImmutableAlias, KisPagePriority::Normal);
    QVERIFY(alias.isValid()); source.reset();
    auto aliasFee = budget.reserve(delta, &f.error);
    QVERIFY(owner.registerBacking(alias.replica, aliasFee, KisBackingBudgetClass::Current));
    QVERIFY(owner.reclassifyBacking(f.replica, KisBackingBudgetClass::RetirementDebt));
    auto record = owner.takeRetirementRecord(f.replica); QVERIFY(record);
    KisPageMetadataCoordinator metadata; QVERIFY(metadata.configure(1));
    QAtomicInt references{1};
    KisPageRetirementQueue queue(owner, metadata, budget, references, &references,
        [](void *p) { static_cast<QAtomicInt *>(p)->deref(); });
    QSemaphore entered, resume, retiring, finished;
    bool accepted = false, retired = false;
    QString acceptanceError;
    std::thread acceptance([&] {
        accepted = KisPageOwnerLedgerTestAccess::acceptChanges(owner,
            {{alias.replica, KisBackingBudgetClass::Current, KisBackingBudgetClass::RetainedHistory}}, [&] {
                entered.release(); resume.acquire(); return true;
            }, &acceptanceError) == KisPageOwnerLedger::BackingAcceptance::Accepted;
    });
    std::thread worker;
    const auto join = qScopeGuard([&] {
        resume.release();
        if (acceptance.joinable()) acceptance.join();
        if (worker.joinable()) worker.join();
    });
    QVERIFY2(entered.tryAcquire(1, 5000), "class acceptance did not reach installation");
    worker = std::thread([&] { retiring.release(); retired = queue.retireRecord(*record); finished.release(); });
    QVERIFY(retiring.tryAcquire(1, 5000));
    const bool finishedInside = finished.tryAcquire(1, 20);
    resume.release(); acceptance.join(); worker.join();
    QVERIFY(!finishedInside);
    QVERIFY2(accepted, qPrintable(acceptanceError));
    QVERIFY(retired);
    QCOMPARE(owner.backingClass(f.replica), KisBackingBudgetClass::Count);
    QCOMPARE(owner.backingClass(alias.replica), KisBackingBudgetClass::RetainedHistory);
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::RetainedHistory)].live.cpuRam,
             f.replica.layout.byteSize);
    QCOMPARE(owner.providerOperationCount(), qsizetype(0));
    auto last = owner.takeRetirementRecord(alias.replica); QVERIFY(last);
    QVERIFY(queue.retireRecord(*last));
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].live.cpuRam, quint64(0));
}

void KisPageStorePhysicalClaimTest::sharedHandoffDebt_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("swapped");
    QTest::addColumn<bool>("keepAlias"); QTest::addColumn<bool>("reverse");
    for (int bpp : {1, 4, 8, 16}) for (bool swapped : {false, true})
        for (bool keep : {false, true}) for (bool reverse : {false, true})
            QTest::newRow(qPrintable(QString("bpp%1-ssd%2-keep%3-reverse%4").arg(bpp).arg(swapped).arg(keep).arg(reverse)))
                << bpp << swapped << keep << reverse;
}

void KisPageStorePhysicalClaimTest::sharedHandoffDebt()
{
    QFETCH(int, bpp); QFETCH(bool, swapped); QFETCH(bool, keepAlias); QFETCH(bool, reverse);
    Fixture f; QVERIFY(f.init(bpp)); const quint64 bytes = f.replica.layout.byteSize;
    KisPageBackingLimits limits; limits.retirementDebtBytes = limits.durableStoreCapacity = bytes;
    KisBackingBudgetController budget(limits); KisPageOwnerLedger ledger;
    QVERIFY(ledger.configure(f.completions)); ledger.attachBackingBudget(budget); QVERIFY(ledger.registerProvider(f.provider));
    KisBackingBudgetDelta initial; initial.bucket(size_t(KisBackingBudgetClass::Current)).cpuRam = bytes;
    auto reservation = budget.reserve(initial, &f.error); QVERIFY(ledger.registerBacking(f.replica, reservation, KisBackingBudgetClass::Current));
    auto physical = KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc);
    const auto targetReplica = physical.target();
    const auto target = targetReplica; auto accounting = ledger.prepareBackingHandoff(f.replica, target, &f.error);
    QVERIFY(accounting.isValid()); QVERIFY(physical.tryClaim()); auto writer = physical.commit(); QVERIFY(writer.isValid());
    QVERIFY(ledger.commitBackingHandoff(std::move(accounting))); writer.reset(); physical.reset();
    auto source = f.provider->captureCompletedTileSource(f.desc, f.tile); QVERIFY(source);
    auto alias = f.provider->prepareSynchronousSource({620}, source, {key(1), {1}}, f.desc, KisReplicaSourceUse::ImmutableAlias, KisPagePriority::Normal);
    QVERIFY(alias.isValid()); source.reset(); reservation = budget.reserve(initial, &f.error);
    QVERIFY(ledger.registerBacking(alias.replica, reservation, KisBackingBudgetClass::Current));
    if (swapped) QVERIFY(f.swap());
    QVector<KisPageTransitionEffect> effects; KisPageTransitionEffect effect; effect.replica = target; effects.append(effect);
    if (!keepAlias) { effect.replica = alias.replica; effects.append(effect); }
    if (reverse) std::reverse(effects.begin(), effects.end());
    bool entered = false;
    QVERIFY(!ledger.acceptRetirementDebt(effects.constData(), effects.size(), &entered,
        [](void *p) { *static_cast<bool *>(p) = true; return false; }, &f.error));
    QVERIFY(entered);
    QVERIFY2(ledger.acceptRetirementDebt(effects.constData(), effects.size(), nullptr,
        [](void *) { return true; }, &f.error), qPrintable(f.error));
    const auto firstNodeBytes = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    QVERIFY(f.provider->retire({621}, target, {}).isValid()); KisPageOwnerLedgerTestAccess::withdrawRegistration(ledger, target);
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
             firstNodeBytes); // Physical account holds the original allocation until the last alias.
    if (keepAlias) {
        QCOMPARE(ledger.backingClass(alias.replica), KisBackingBudgetClass::Current);
        const auto usage = budget.usage(); const auto &debt = usage.buckets[size_t(KisBackingBudgetClass::RetirementDebt)];
        QCOMPARE(debt.reserved.cpuRam, 0u); QCOMPARE(debt.reserved.ssd, 0u);
        QVERIFY(ledger.reclassifyBacking(alias.replica, KisBackingBudgetClass::RetirementDebt));
    }
    const auto terminalBytes = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    QVERIFY(f.provider->retire({622}, alias.replica, {}).isValid()); KisPageOwnerLedgerTestAccess::withdrawRegistration(ledger, alias.replica);
    const quint64 retainedCache = terminalBytes - 2 * retirementStorageBytes(budget);
    const auto finalUsage = budget.usage();
    for (size_t i = 0; i < finalUsage.buckets.size(); ++i) {
        const auto &bucket = finalUsage.buckets[i];
        QCOMPARE(bucket.live.cpuRam, i == size_t(KisBackingBudgetClass::MetadataArena) ? retainedCache : quint64(0)); QCOMPARE(bucket.live.ssd, 0u);
        QCOMPARE(bucket.reserved.cpuRam, 0u); QCOMPARE(bucket.reserved.ssd, 0u);
    }
}

void KisPageStorePhysicalClaimTest::sharedCacheClassificationAndDebt()
{
    QFETCH(int, bpp); QFETCH(bool, alias);
    Fixture f; QVERIFY(f.init(bpp));
    const quint64 bytes = f.replica.layout.byteSize;
    const size_t metadata = size_t(KisBackingBudgetClass::MetadataArena);
    const size_t cache = size_t(KisBackingBudgetClass::OptionalCache);
    const size_t pending = size_t(KisBackingBudgetClass::ActivePending);
    const size_t debt = size_t(KisBackingBudgetClass::RetirementDebt);
    auto parent = std::make_shared<KisBackingBudgetController>();
    {
        std::array<KisBackingBudgetReservation, 16> slots;
        for (auto &slot : slots) { slot = parent->reserve({}, nullptr); QVERIFY(slot.isValid()); }
    }
    const auto parentControl = parent->usage().buckets[metadata].live.cpuRam;
    {
        KisPageBackingLimits limits; limits.retirementDebtBytes = bytes;
        KisBackingBudgetController budget(limits);
        QVERIFY(budget.configureSharedNonPayloadBudget(parent));
        KisPageOwnerLedger ledger;
        QVERIFY(ledger.configure(f.completions)); ledger.attachBackingBudget(budget);
        QVERIFY(ledger.registerProvider(f.provider));
        KisBackingBudgetDelta initial;
        initial.bucket(cache).cpuRam = qint64(bytes);
        if (!alias) initial.bucket(debt).cpuRam = qint64(bytes);
        auto reservation = budget.reserve(initial, &f.error);
        QVERIFY(reservation.isValid());
        QVERIFY2(ledger.registerBacking(f.replica, reservation, KisBackingBudgetClass::OptionalCache,
                                       &f.error), qPrintable(f.error));
        // registerBacking uses commitRetaining: only the caller's already paid
        // debt remains. No parent reservation may follow that payload token.
        QCOMPARE(reservation.isValid(), !alias);
        QCOMPARE(parent->usage().buckets[metadata].reserved.cpuRam, quint64(0));
        QVector<KisReplicaHandle> replicas{f.replica};
        if (alias) {
            auto source = f.provider->captureCompletedTileSource(f.desc, f.tile); QVERIFY(source);
            auto result = f.provider->prepareSynchronousSource({730}, source, {key(1), {1}},
                f.desc, KisReplicaSourceUse::ImmutableAlias, KisPagePriority::Normal);
            QVERIFY(result.isValid()); source.reset();
            auto aliasReservation = budget.reserve({}, &f.error);
            QVERIFY(ledger.registerBacking(result.replica, aliasReservation, KisBackingBudgetClass::OptionalCache));
            replicas.append(result.replica);
        }
        QCOMPARE(budget.usage().buckets[cache].live.cpuRam, bytes);
        const auto parentLessChildStorage = [&] {
            return parent->usage().buckets[metadata].live.cpuRam - budget.usage().buckets[metadata].live.cpuRam;
        };
        const quint64 before = parentLessChildStorage();
        const auto accept = [&](bool commit) {
            KisPageSnapshotArray<KisBackingClassChange> changes;
            for (const auto &replica : replicas)
                changes.append({replica, KisBackingBudgetClass::OptionalCache, KisBackingBudgetClass::ActivePending});
            return KisPageOwnerLedgerTestAccess::changeClasses(ledger, std::move(changes), &f.error, commit);
        };
        QVERIFY2(accept(false), qPrintable(f.error));
        QCOMPARE(budget.usage().buckets[cache].live.cpuRam, bytes);
        QCOMPARE(parentLessChildStorage(), before);
        QVERIFY2(accept(true), qPrintable(f.error));
        QCOMPARE(budget.usage().buckets[cache].live.cpuRam, quint64(0));
        QCOMPARE(budget.usage().buckets[pending].live.cpuRam, bytes);
        QCOMPARE(parentLessChildStorage(), before - bytes); // One physical, even with two logical members.
        QCOMPARE(budget.usage().buckets[debt].reserved.cpuRam, alias ? quint64(0) : bytes);
        for (const auto &replica : replicas) {
            if (alias) QVERIFY(ledger.reclassifyBacking(replica, KisBackingBudgetClass::RetirementDebt));
            else QVERIFY(ledger.reclassifyBacking(replica, KisBackingBudgetClass::RetirementDebt, reservation));
        }
        QVERIFY(!reservation.isValid());
        QCOMPARE(budget.usage().buckets[debt].live.cpuRam, bytes);
        QCOMPARE(budget.usage().buckets[debt].reserved.cpuRam, quint64(0));
        quint64 retireOperation = 731;
        for (const auto &replica : replicas) {
            QVERIFY(f.provider->retire({retireOperation++}, replica, {}).isValid());
            KisPageOwnerLedgerTestAccess::withdrawRegistration(ledger, replica);
        }
        QCOMPARE(budget.usage().buckets[debt].live.cpuRam, quint64(0));
        QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
    }
    QCOMPARE(parent->usage().buckets[metadata].live.cpuRam, parentControl);
    QCOMPARE(parent->usage().buckets[metadata].reserved.cpuRam, quint64(0));
}

void KisPageStorePhysicalClaimTest::unfundedRetirementPreservesCharge_data()
{
    QTest::addColumn<int>("bpp");
    QTest::addColumn<int>("classification");
    QTest::addColumn<bool>("close");
    for (int bpp : {1, 4, 8, 16}) for (int classification = 0; classification < 4; ++classification)
        for (bool close : {false, true})
            QTest::newRow(qPrintable(QString("bpp%1-class%2-close%3")
                .arg(bpp).arg(classification).arg(close))) << bpp << classification << close;
}

void KisPageStorePhysicalClaimTest::unfundedRetirementPreservesCharge()
{
    QFETCH(int, bpp); QFETCH(int, classification); QFETCH(bool, close);
    Fixture f; QVERIFY(f.init(bpp));
    const quint64 bytes = f.replica.layout.byteSize;
    const auto original = classification == 0 ? KisBackingBudgetClass::Current
        : classification == 1 ? KisBackingBudgetClass::ActivePending
        : classification == 2 ? KisBackingBudgetClass::OptionalCache : KisBackingBudgetClass::IdleCapacity;
    const auto debt = size_t(KisBackingBudgetClass::RetirementDebt);
    const auto metadataClass = size_t(KisBackingBudgetClass::MetadataArena);
    auto parent = std::make_shared<KisBackingBudgetController>();
    KisPageBackingLimits limits; limits.retirementDebtBytes = bytes; limits.optionalCacheBytes = bytes;
    KisBackingBudgetController budget(limits);
    QVERIFY(budget.configureSharedNonPayloadBudget(parent));
    KisPageOwnerLedger ledger;
    QVERIFY(ledger.configure(f.completions)); ledger.attachBackingBudget(budget);
    QVERIFY(ledger.registerProvider(f.provider));
    auto terminalStorage = kisPreparePageRetirementRecord(&budget);
    auto *originalRecord = terminalStorage.get();
    {
        KisBackingBudgetDelta initial;
        initial.bucket(size_t(original)).cpuRam = qint64(bytes);
        initial.bucket(debt).cpuRam = qint64(bytes);
        auto preparation = budget.reserve(initial, &f.error); QVERIFY(preparation.isValid());
        QVERIFY(ledger.registerBacking(f.replica, preparation, original, &f.error, &terminalStorage));
        QVERIFY(preparation.isValid()); // Only provider/adoption failure is prepaid here.
    }
    QCOMPARE(budget.usage().buckets[debt].reserved.cpuRam, quint64(0));
    KisBackingBudgetDelta competing; competing.bucket(debt).cpuRam = qint64(bytes);
    auto blocker = budget.reserve(competing, &f.error); QVERIFY(blocker.isValid());
    KisPageMetadataCoordinator metadata; QVERIFY(metadata.configure(1));
    QAtomicInt references{1};
    KisPageRetirementQueue queue(ledger, metadata, budget, references, &references,
        [](void *p) { static_cast<QAtomicInt *>(p)->deref(); });
    queue.prepareTask();
    std::shared_ptr<KisPageRetirementWait> delayedCallback;
    const auto cleanup = qScopeGuard([&] {
        delayedCallback.reset();
        queue.beginClose(); queue.waitForIdle(); blocker.release();
        auto remaining = queue.takeForClose();
        for (auto &record : remaining) queue.retireRecord(record);
        remaining.clear_and_dispose(KisPageRetirementRecordDeleter{});
        kisDrainPageStoreReclamation();
    });
    const auto sharedPayloadCharge = [&] {
        return parent->usage().buckets[metadataClass].live.cpuRam
            - budget.usage().buckets[metadataClass].live.cpuRam;
    };
    const auto beforeShared = sharedPayloadCharge();
    queue.retireOrDefer(f.replica, f.provider, {}, {});
    queue.waitForIdle();
    QVERIFY(!queue.isDrained());
    QCOMPARE(queue.snapshot().pendingBytes, bytes);
    QCOMPARE(ledger.backingClass(f.replica), original);
    QCOMPARE(budget.usage().buckets[size_t(original)].live.cpuRam, bytes);
    QCOMPARE(budget.usage().buckets[debt].live.cpuRam, quint64(0));
    QCOMPARE(budget.usage().buckets[debt].reserved.cpuRam, bytes); // The competing owner only.
    QCOMPARE(sharedPayloadCharge(), beforeShared);
    if ((original == KisBackingBudgetClass::OptionalCache || original == KisBackingBudgetClass::IdleCapacity)) {
        KisBackingBudgetDelta anotherCache; anotherCache.bucket(size_t(original)).cpuRam = qint64(bytes);
        QVERIFY(!budget.reserve(anotherCache, &f.error).isValid()); // Queued is still charged to Q.
    }
    if (close) {
        // A producer may have locked the weak callback just before close
        // stops dispatch. Storage may outlive disarm; its budget permission
        // must not. Keep that exact original wait alive across the handoff.
        delayedCallback = originalRecord->wait;
        QVERIFY(delayedCallback);
        queue.beginClose(); queue.waitForIdle();
        QCOMPARE(budget.usage().waitingRequests, quint32(0));
        QCOMPARE(budget.usage().grantedRequests, quint32(0));
        auto records = queue.takeForClose(); QCOMPARE(records.size(), size_t(1));
        QVERIFY(!queue.retireRecord(records.front()));
        QCOMPARE(ledger.backingClass(f.replica), original);
        blocker.release();
        QVERIFY(queue.retireRecord(records.front()));
        records.clear_and_dispose(KisPageRetirementRecordDeleter{});
        delayedCallback.reset();
    } else {
        blocker.release(); // Original budget notification progresses without another write.
        QTRY_VERIFY_WITH_TIMEOUT(queue.isDrained(), 5000);
        queue.waitForIdle();
    }
    QCOMPARE(budget.usage().buckets[size_t(original)].live.cpuRam, quint64(0));
    QCOMPARE(budget.usage().buckets[debt].live.cpuRam, quint64(0));
    QCOMPARE(budget.usage().buckets[debt].reserved.cpuRam, quint64(0));
    QCOMPARE(sharedPayloadCharge(), beforeShared - ((original == KisBackingBudgetClass::OptionalCache || original == KisBackingBudgetClass::IdleCapacity) ? bytes : 0));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStorePhysicalClaimTest::sharedOrphanRetainsOnlyDebt()
{
    Fixture f; QVERIFY(f.init());
    const qint64 bytes = qint64(f.replica.layout.byteSize);
    const size_t metadata = size_t(KisBackingBudgetClass::MetadataArena);
    const size_t debt = size_t(KisBackingBudgetClass::RetirementDebt);
    auto parent = std::make_shared<KisBackingBudgetController>();
    KisBackingBudgetController budget;
    QVERIFY(budget.configureSharedNonPayloadBudget(parent));
    KisPageOwnerLedger ledger;
    QVERIFY(ledger.configure(f.completions)); ledger.attachBackingBudget(budget);
    QVERIFY(ledger.registerProvider(f.provider));
    KisBackingBudgetDelta initial;
    initial.bucket(size_t(KisBackingBudgetClass::OptionalCache)).cpuRam = bytes;
    initial.bucket(debt).cpuRam = bytes;
    auto reservation = budget.reserve(initial, &f.error); QVERIFY(reservation.isValid());
    QCOMPARE(parent->usage().buckets[metadata].reserved.cpuRam, quint64(bytes));
    QVERIFY(ledger.retainRetirementDebtReservation(f.replica, reservation, &f.error));
    QVERIFY(reservation.isValid());
    QCOMPARE(parent->usage().buckets[metadata].reserved.cpuRam, quint64(0));
    QCOMPARE(budget.usage().buckets[debt].reserved.cpuRam, quint64(bytes));
    QVERIFY(ledger.registerBacking(f.replica, reservation, KisBackingBudgetClass::RetirementDebt));
    QVERIFY(!reservation.isValid());
    QVERIFY(f.provider->retire({732}, f.replica, {}).isValid()); KisPageOwnerLedgerTestAccess::withdrawRegistration(ledger, f.replica);
    QCOMPARE(budget.usage().buckets[debt].live.cpuRam, quint64(0));
    QCOMPARE(budget.usage().buckets[debt].reserved.cpuRam, quint64(0));
}

void KisPageStorePhysicalClaimTest::ledgerAliasMembershipAtCapacity()
{
    QFETCH(bool, reverse);
    Fixture f; QVERIFY(f.init());
    KisPageBackingLimits limits; limits.metadataArenaBytes = 64 * 1024;
    KisBackingBudgetController budget(limits);
    KisPageOwnerLedger ledger; QVERIFY(ledger.configure(f.completions)); ledger.attachBackingBudget(budget);
    QVERIFY(ledger.registerProvider(f.provider));
    const auto live = [&] { return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    std::array<KisBackingBudgetReservation, 3> warm;
    for (auto &reservation : warm) { reservation = budget.reserve({}, &f.error); QVERIFY(reservation.isValid()); }
    for (auto &reservation : warm) reservation.release();
    const auto recordBytes = retirementStorageBytes(budget);
    const auto baseline = live();
    std::array<KisReplicaHandle, 3> replicas{f.replica};
    std::array<KisPageRetirementRecordPointer, 3> records;
    std::array<KisBackingBudgetReservation, 3> reservations;
    KisBackingBudgetDelta initial;
    initial.bucket(size_t(KisBackingBudgetClass::Current)).cpuRam = qint64(f.replica.layout.byteSize);
    auto source = f.provider->captureCompletedTileSource(f.desc, f.tile); QVERIFY(source);
    const auto committed = f.provider->memoryUsage().committedBytes;
    for (size_t i = 0; i < records.size(); ++i) {
        records[i] = kisPreparePageRetirementRecord(&budget);
        reservations[i] = budget.reserve(initial, &f.error); QVERIFY(reservations[i].isValid());
        if (i) {
            auto result = f.provider->prepareSynchronousSource({700 + i}, source, {key(int(i)), {1}},
                f.desc, KisReplicaSourceUse::ImmutableAlias, KisPagePriority::Normal);
            QVERIFY(result.isValid()); replicas[i] = result.replica;
        }
    }
    source.reset();
    QCOMPARE(f.provider->memoryUsage().committedBytes, committed);
    std::array<void *, 3> fillers{};
    std::array<size_t, 3> fillerBytes{};
    const auto release = qScopeGuard([&] {
        for (size_t i = 0; i < fillers.size(); ++i)
            freeTestStoragePressure(&budget, fillers[i], fillerBytes[i], 1);
    });
    for (size_t i = 0; i < replicas.size(); ++i) {
        fillerBytes[i] = size_t(limits.metadataArenaBytes - live());
        fillers[i] = allocateTestStoragePressure(&budget, fillerBytes[i], 1);
        QCOMPARE(live(), limits.metadataArenaBytes);
        QVERIFY2(ledger.registerBacking(replicas[i], reservations[i], KisBackingBudgetClass::Current,
            &f.error, &records[i]), qPrintable(f.error));
        QVERIFY(!records[i]);
        QCOMPARE(live(), limits.metadataArenaBytes); // Shared aliases never prepare a throwaway physical node.
        QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::Current)].live.cpuRam,
                 f.replica.layout.byteSize); // One charge, including every new foreground alias.
    }
    // Roll back the original member while its physical endpoint still serves
    // real aliases. Same-resource retry reuses the endpoint at hard full; a
    // different resource cannot overwrite that still-live account.
    auto original = ledger.takeRetirementRecord(replicas[0]); QVERIFY(original);
    auto *identity = original.get();
    QVERIFY(KisPageOwnerLedgerTestAccess::withdrawRegistration(ledger, *original));
    QVERIFY(original->physical.is_linked());
    const auto unrelated = f.provider->requestReplica({709}, {key(8), {1}}, f.desc,
        cpu.domain, KisPageAccessMode::Read, KisPagePriority::Normal);
    QVERIFY(unrelated.isValid());
    auto retry = budget.reserve(initial, &f.error); QVERIFY(retry.isValid());
    QVERIFY(!ledger.registerBacking(unrelated.replica, retry, KisBackingBudgetClass::Current,
        &f.error, &original));
    QCOMPARE(original.get(), identity); QVERIFY(retry.isValid());
    QVERIFY(f.provider->retire({710}, unrelated.replica, {}).isValid());
    QCOMPARE(live(), limits.metadataArenaBytes);
    QVERIFY2(ledger.registerBacking(replicas[0], retry, KisBackingBudgetClass::Current,
        &f.error, &original), qPrintable(f.error));
    QVERIFY(!original); QCOMPARE(live(), limits.metadataArenaBytes);
    for (const auto &replica : replicas)
        QVERIFY(ledger.reclassifyBacking(replica, KisBackingBudgetClass::RetirementDebt));
    const std::array<size_t, 3> order = reverse ? std::array<size_t, 3>{2, 0, 1}
                                             : std::array<size_t, 3>{1, 0, 2};
    for (size_t n = 0; n < order.size(); ++n) {
        const auto i = order[n];
        const auto before = live();
        QVERIFY(f.provider->retire({720 + n}, replicas[i], {}).isValid());
        KisPageOwnerLedgerTestAccess::withdrawRegistration(ledger, replicas[i]);
        QCOMPARE(live(), before - (n == 2 ? 2 * recordBytes : i == 0 ? 0 : recordBytes));
        for (size_t j = n + 1; j < order.size(); ++j) {
            const auto &survivor = replicas[order[j]];
            QCOMPARE(ledger.backingClass(survivor), KisBackingBudgetClass::RetirementDebt);
            QVERIFY(f.provider->validate(survivor, f.desc));
        }
    }
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].live.cpuRam, quint64(0));
    for (size_t i = 0; i < fillers.size(); ++i)
        freeTestStoragePressure(&budget, std::exchange(fillers[i], nullptr), fillerBytes[i], 1);
    QCOMPARE(live(), baseline);
}

void KisPageStorePhysicalClaimTest::wideAliasClassCapacity()
{
    using Class = KisBackingBudgetClass;
    Fixture f; QVERIFY(f.init());
    KisPageBackingLimits limits; limits.metadataArenaBytes = 2 * 1024 * 1024;
    KisBackingBudgetController budget(limits);
    KisPageOwnerLedger ledger; QVERIFY(ledger.configure(f.completions)); ledger.attachBackingBudget(budget);
    QVERIFY(ledger.registerProvider(f.provider));
    { auto warm = kisPreparePageRetirementRecord(&budget); }
    std::array<KisBackingBudgetReservation, 4> warm;
    for (auto &fee : warm) { fee = budget.reserve({}, &f.error); QVERIFY(fee.isValid()); }
    for (auto &fee : warm) fee.release();
    const auto live = [&] { return budget.usage().buckets[size_t(Class::MetadataArena)].live.cpuRam; };
    const auto baseline = live();
    const auto bytes = f.replica.layout.byteSize;
    KisBackingBudgetDelta delta; delta.bucket(size_t(Class::Current)).cpuRam = qint64(bytes);
    std::array<KisReplicaHandle, 256> replicas;
    std::array<Class, 256> classes;
    classes.fill(Class::Current);
    auto source = f.provider->captureCompletedTileSource(f.desc, f.tile); QVERIFY(source);
    KisPageRetirementRecord *anchor = nullptr;
    KisPageRetirementRecordPointer last;
    KisBackingBudgetReservation lastFee;
    for (size_t i = 0; i < replicas.size(); ++i) {
        auto record = kisPreparePageRetirementRecord(&budget);
        if (!i) { anchor = record.get(); replicas[i] = f.replica; }
        else {
            auto result = f.provider->prepareSynchronousSource({1000 + i}, source, {key(int(i)), {1}},
                f.desc, KisReplicaSourceUse::ImmutableAlias, KisPagePriority::Normal);
            QVERIFY(result.isValid()); replicas[i] = result.replica;
        }
        auto fee = budget.reserve(delta, &f.error); QVERIFY(fee.isValid());
        if (i + 1 == replicas.size()) { last = std::move(record); lastFee = std::move(fee); }
        else QVERIFY2(ledger.registerBacking(replicas[i], fee, Class::Current, &f.error, &record), qPrintable(f.error));
    }
    QVERIFY(!anchor->physical.classStorage); // 255 real members need no wide storage.
    QVERIFY(!ledger.reclassifyBacking(replicas[0], Class::Count));
    QCOMPARE(ledger.backingClass(replicas[0]), Class::Current);
    const auto beforeWide = live();
    size_t fillerBytes = size_t(limits.metadataArenaBytes - live());
    void *filler = allocateTestStoragePressure(&budget, fillerBytes, 1);
    const auto cleanup = qScopeGuard([&] { freeTestStoragePressure(&budget, filler, fillerBytes, 1); });
    auto *original = last.get();
    QVERIFY(!ledger.registerBacking(replicas.back(), lastFee, Class::Current, &f.error, &last));
    QCOMPARE(last.get(), original); QVERIFY(lastFee.isValid());
    QCOMPARE(ledger.backingClass(replicas.back()), Class::Count);
    QCOMPARE(budget.usage().buckets[size_t(Class::Current)].live.cpuRam, bytes);
    QVERIFY(!anchor->physical.classStorage);
    freeTestStoragePressure(&budget, std::exchange(filler, nullptr), fillerBytes, 1);
    QVERIFY2(ledger.registerBacking(replicas.back(), lastFee, Class::Current, &f.error, &last), qPrintable(f.error));
    QVERIFY(anchor->physical.classStorage);
    const auto storage = live(); QVERIFY(storage > beforeWide);
    qInfo() << "BR1_WIDE_CLASS_CAPACITY_BYTES" << storage - beforeWide;
    const std::array<Class, 8> priority{Class::Current, Class::ActivePending, Class::InFlight,
        Class::RetainedHistory, Class::OptionalCache, Class::IdleCapacity, Class::MetadataArena, Class::RetirementDebt};
    const auto checkCharge = [&] {
        Class expected = Class::Count;
        for (const auto cls : priority) if (std::find(classes.begin(), classes.end(), cls) != classes.end()) {
            expected = cls; break;
        }
        const auto usage = budget.usage();
        for (const auto cls : priority) {
            const quint64 expectedBytes = (cls == Class::MetadataArena ? storage : 0) + (cls == expected ? bytes : 0);
            if (!QTest::qCompare(usage.buckets[size_t(cls)].live.cpuRam, expectedBytes,
                "actual charge", "member oracle", __FILE__, __LINE__)) return false;
            if (!QTest::qCompare(usage.buckets[size_t(cls)].reserved.cpuRam, quint64(0),
                "reserved charge", "zero after acceptance", __FILE__, __LINE__)) return false;
        }
        return true;
    };
    // Uniform 256-member groups exceed one byte in each of the eight classes.
    // Check every intermediate destination, including the final priority edge.
    for (const auto cls : priority) for (size_t i = 0; i < replicas.size(); ++i) {
        QVERIFY2(ledger.reclassifyBacking(replicas[i], cls, &f.error), qPrintable(f.error));
        classes[i] = cls; QVERIFY(checkCharge());
    }
    // Independent member oracle: mixed distributions and priority crossings.
    for (size_t shift = 0; shift < priority.size(); ++shift) {
        for (size_t i = 0; i < replicas.size(); ++i) {
            const auto cls = priority[(i + shift) % priority.size()];
            QVERIFY2(ledger.reclassifyBacking(replicas[i], cls, &f.error), qPrintable(f.error));
            classes[i] = cls; QVERIFY(checkCharge());
        }
    }
    for (size_t i = 0; i < replicas.size(); ++i) {
        QVERIFY(ledger.reclassifyBacking(replicas[i], Class::Current)); classes[i] = Class::Current;
    }
    QVERIFY(checkCharge());
    QVERIFY(ledger.reclassifyBacking(replicas.back(), Class::RetirementDebt));
    QVERIFY(f.provider->retire({2000}, replicas.back(), {}).isValid());
    QVERIFY(KisPageOwnerLedgerTestAccess::withdrawRegistration(ledger, replicas.back()));
    QVERIFY(anchor->physical.classCounts != KisPageClassCounts::Wide);
    last = kisPreparePageRetirementRecord(&budget);
    const auto replacement = f.provider->prepareSynchronousSource({2001}, source, {key(255), {2}},
        f.desc, KisReplicaSourceUse::ImmutableAlias, KisPagePriority::Normal);
    QVERIFY(replacement.isValid()); replicas.back() = replacement.replica;
    lastFee = budget.reserve(delta, &f.error); QVERIFY(lastFee.isValid());
    fillerBytes = size_t(limits.metadataArenaBytes - live());
    filler = allocateTestStoragePressure(&budget, fillerBytes, 1);
    QVERIFY2(ledger.registerBacking(replicas.back(), lastFee, Class::Current, &f.error, &last), qPrintable(f.error));
    QCOMPARE(live(), limits.metadataArenaBytes); // Reuse paid cold capacity; no refill at 255 -> 256.
    freeTestStoragePressure(&budget, std::exchange(filler, nullptr), fillerBytes, 1);
    QCOMPARE(live(), storage);
    for (const auto &replica : replicas) QVERIFY(ledger.reclassifyBacking(replica, Class::RetirementDebt));
    source.reset();
    for (size_t i = 0; i < replicas.size(); ++i) {
        QVERIFY(f.provider->retire({3000 + i}, replicas[i], {}).isValid());
        QVERIFY(KisPageOwnerLedgerTestAccess::withdrawRegistration(ledger, replicas[i]));
    }
    QCOMPARE(live(), baseline); // The original alias account tail and wide capacity both finish once.
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
    QCOMPARE(budget.usage().buckets[size_t(Class::RetirementDebt)].live.cpuRam, quint64(0));
}

void KisPageStorePhysicalClaimTest::optionalCapacityCheckout_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("outcome");
    for (int bpp : {1, 4, 8, 16}) for (int outcome = 0; outcome < 6; ++outcome)
        QTest::newRow(qPrintable(QString("b%1-outcome%2").arg(bpp).arg(outcome))) << bpp << outcome;
}

void KisPageStorePhysicalClaimTest::optionalCapacityCheckout()
{
    QFETCH(int, bpp); QFETCH(int, outcome);
    Fixture f; QVERIFY(f.init(bpp));
    const quint64 bytes = f.replica.layout.byteSize;
    const auto cache = KisBackingBudgetClass::OptionalCache;
    const auto pending = KisBackingBudgetClass::ActivePending;
    const auto debt = KisBackingBudgetClass::RetirementDebt;
    const auto arena = KisBackingBudgetClass::MetadataArena;
    KisPageBackingLimits parentLimits; parentLimits.metadataArenaBytes = 1024 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(parentLimits);
    {
        std::array<KisBackingBudgetReservation, 8> slots;
        for (auto &slot : slots) { slot = parent->reserve({}, nullptr); QVERIFY(slot.isValid()); }
    }
    KisPageBackingLimits limits;
    limits.optionalCacheBytes = limits.activePendingBytes = limits.retirementDebtBytes = bytes;
    KisBackingBudgetController budget(limits), sibling;
    QVERIFY(budget.configureSharedNonPayloadBudget(parent));
    QVERIFY(sibling.configureSharedNonPayloadBudget(parent));
    // Prepare real sibling request storage before computing its pressure.
    { auto a = sibling.reserve({}, nullptr), b = sibling.reserve({}, nullptr); QVERIFY(a.isValid() && b.isValid()); }
    KisPageOwnerLedger ledger;
    QVERIFY(ledger.configure(f.completions)); ledger.attachBackingBudget(budget);
    QVERIFY(ledger.registerProvider(f.provider));
    KisBackingBudgetDelta initial; initial.bucket(size_t(cache)).cpuRam = bytes;
    auto registration = budget.reserve(initial, &f.error);
    QVERIFY(ledger.registerBacking(f.replica, registration, cache, &f.error));
    registration.release();
    KisBackingBudgetDelta debtDemand; debtDemand.bucket(size_t(debt)).cpuRam = bytes;
    auto debtBlocker = budget.reserve(debtDemand, &f.error); QVERIFY(debtBlocker.isValid());
    KisBackingBudgetReservation pressure;
    KisPageMetadataCoordinator metadata; QVERIFY(metadata.configure(1));
    QAtomicInt references{1};
    KisPageRetirementQueue queue(ledger, metadata, budget, references, &references,
        [](void *p) { static_cast<QAtomicInt *>(p)->deref(); });
    queue.prepareTask();
    auto current = f.replica;
    bool pinned = false;
    const auto cleanup = qScopeGuard([&] {
        if (pinned) f.binding->releaseRead();
        pressure.release(); debtBlocker.release();
        queue.beginClose(); queue.waitForIdle();
        auto remaining = queue.takeForClose();
        if (remaining.empty() && ledger.backingClass(current) != KisBackingBudgetClass::Count) {
            queue.retireOrDefer(current, f.provider, {}, {});
            remaining = queue.takeForClose();
        }
        for (auto &record : remaining) queue.retireRecord(record);
        remaining.clear_and_dispose(KisPageRetirementRecordDeleter{});
        kisDrainPageStoreReclamation();
    });
    const auto live = [&](KisBackingBudgetClass c) { return budget.usage().buckets[size_t(c)].live.cpuRam; };
    const auto sharedPayload = [&] {
        return parent->usage().buckets[size_t(arena)].live.cpuRam
            - live(arena) - sibling.usage().buckets[size_t(arena)].live.cpuRam;
    };
    // The parent's own request storage is still charged. Compare the actual
    // payload delta without pretending all parent-only storage is payload.
    const quint64 originalParent = sharedPayload();
    QVERIFY(originalParent >= bytes);
    const auto physicalIdentity = current.physicalSlotIdentity();
    for (quint64 generation = 2; generation <= 17; ++generation) {
        const auto old = current;
        auto physical = KisCpuBackingHandoff::prepare(f.provider, old, {key(0), {generation}}, f.desc);
        const auto targetReplica = physical.target();
        QVERIFY(physical.isValid());
        auto prepared = ledger.prepareBackingHandoff(old, targetReplica, &f.error);
        QVERIFY2(prepared.isValid(), qPrintable(f.error));
        QCOMPARE(live(cache), bytes); QCOMPARE(live(pending), quint64(0));
        QCOMPARE(sharedPayload(), originalParent); // Negative parent fee is not yet consumed.
        QCOMPARE(budget.usage().buckets[size_t(debt)].reserved.cpuRam, bytes); // Only blocker.
        if (outcome == 1) {
            prepared.release();
            QCOMPARE(ledger.backingClass(old), cache);
            QCOMPARE(budget.usage().buckets[size_t(pending)].reserved.cpuRam, quint64(0));
            QVERIFY(f.provider->validate(old, f.desc));
            prepared = ledger.prepareBackingHandoff(old, targetReplica, &f.error);
            QVERIFY2(prepared.isValid(), qPrintable(f.error));
        }
        if (outcome == 2) {
            pinned = f.binding->acquireRead(old.allocationIdentity(), true) != nullptr;
            QVERIFY(pinned); QVERIFY(!physical.tryClaim());
            QCOMPARE(ledger.backingClass(old), cache);
            QVERIFY(f.provider->validate(old, f.desc));
            f.binding->releaseRead(); pinned = false;
        }
        QVERIFY(physical.tryClaim());
        auto writer = physical.commit(); QVERIFY(writer.isValid());
        QVERIFY(ledger.commitBackingHandoff(std::move(prepared)));
        current = targetReplica;
        QVERIFY(current.physicalSlotIdentity() == physicalIdentity);
        QCOMPARE(ledger.backingClass(old), KisBackingBudgetClass::Count);
        QVERIFY(!f.provider->validate(old, f.desc));
        QVERIFY(!ledger.prepareBackingHandoff(old, current).isValid());
        KisPageOwnerLedgerTestAccess::withdrawRegistration(ledger, old); // Stale finalization cannot release the new incarnation.
        QCOMPARE(ledger.backingClass(current), pending);
        QCOMPARE(live(cache), quint64(0)); QCOMPARE(live(pending), bytes);
        QCOMPARE(sharedPayload(), originalParent - bytes);
        QCOMPARE(budget.usage().buckets[size_t(debt)].reserved.cpuRam, bytes);
        auto *data = static_cast<quint8 *>(writer.pinResident()); QVERIFY(data);
        data[0] = quint8(generation); data[bytes - 1] = quint8(generation + 1);
        writer.reset(); physical.reset();
        if (generation == 17 && outcome >= 3) {
            if (outcome == 3) {
                const auto usage = parent->usage().buckets[size_t(arena)];
                KisBackingBudgetDelta demand;
                demand.bucket(size_t(arena)).cpuRam = parentLimits.metadataArenaBytes
                    - usage.live.cpuRam - usage.reserved.cpuRam - bytes + 1;
                pressure = sibling.reserve(demand, &f.error); QVERIFY2(pressure.isValid(), qPrintable(f.error));
            } else if (outcome == 4) {
                pressure = budget.reserve(initial, &f.error); QVERIFY(pressure.isValid());
            }
            if (outcome != 5) QVERIFY(!ledger.reclassifyBacking(current, cache, &f.error));
            QCOMPARE(ledger.backingClass(current), pending);
            break; // Cancel/refused optional return keeps the actual Pending charge.
        }
        QVERIFY(ledger.reclassifyBacking(current, KisBackingBudgetClass::Current, &f.error));
        QVERIFY(ledger.reclassifyBacking(current, cache, &f.error));
        QCOMPARE(live(cache), bytes); QCOMPARE(sharedPayload(), originalParent);
    }
    auto read = f.provider->resolveAccess({300}, {301}, current, cpu, KisPageAccessMode::Read);
    QVERIFY(read.isValid());
    QCOMPARE(static_cast<const quint8 *>(read.cpuReadData)[0], quint8(17));
    QCOMPARE(static_cast<const quint8 *>(read.cpuReadData)[bytes - 1], quint8(18));
    f.provider->releaseAccess(std::move(read), {});
    const auto lastClass = ledger.backingClass(current);
    queue.retireOrDefer(current, f.provider, {}, {});
    queue.waitForIdle();
    QVERIFY(!queue.isDrained()); QCOMPARE(queue.snapshot().pendingBytes, bytes);
    QCOMPARE(ledger.backingClass(current), lastClass); QCOMPARE(live(lastClass), bytes);
    QCOMPARE(f.provider->memoryUsage().committedBytes, bytes);
    pressure.release(); debtBlocker.release();
    QTRY_VERIFY_WITH_TIMEOUT(queue.isDrained(), 5000);
    queue.waitForIdle();
    QCOMPARE(ledger.backingClass(current), KisBackingBudgetClass::Count);
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
    QCOMPARE(sharedPayload(), originalParent - bytes);
    for (auto c : {cache, pending, debt}) {
        QCOMPARE(live(c), quint64(0));
        QCOMPARE(budget.usage().buckets[size_t(c)].reserved.cpuRam, quint64(0));
    }
}

void KisPageStorePhysicalClaimTest::backingHandoffAccounting_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("sourceClass");
    QTest::addColumn<bool>("shared");
    QTest::addColumn<bool>("cancelWrite");
    for (int bpp : {1, 4, 8, 16}) for (auto source : {KisBackingBudgetClass::Current,
        KisBackingBudgetClass::ActivePending, KisBackingBudgetClass::OptionalCache, KisBackingBudgetClass::IdleCapacity})
        for (bool shared : {false, true}) for (bool cancel : {false, true})
            QTest::newRow(qPrintable(QString("bpp%1-source%2-shared%3-cancel%4").arg(bpp).arg(int(source)).arg(shared).arg(cancel)))
                << bpp << int(source) << shared << cancel;
}

void KisPageStorePhysicalClaimTest::backingHandoffAccounting()
{
    QFETCH(int, bpp); QFETCH(int, sourceClass); QFETCH(bool, shared); QFETCH(bool, cancelWrite);
    Fixture f; QVERIFY(f.init(bpp));
    auto parent = std::make_shared<KisBackingBudgetController>();
    KisBackingBudgetController budget;
    if (shared) QVERIFY(budget.configureSharedNonPayloadBudget(parent, &f.error));
    KisPageOwnerLedger ledger;
    QVERIFY(ledger.configure(f.completions)); ledger.attachBackingBudget(budget);
    QVERIFY(ledger.registerProvider(f.provider));
    const auto initialClass = KisBackingBudgetClass(sourceClass);
    const bool optional = initialClass == KisBackingBudgetClass::OptionalCache || initialClass == KisBackingBudgetClass::IdleCapacity;
    const quint64 bytes = f.replica.layout.byteSize;
    KisBackingBudgetDelta initial; initial.bucket(size_t(initialClass)).cpuRam = bytes;
    auto backing = budget.reserve(initial, &f.error);
    auto terminal = kisPreparePageRetirementRecord(&budget);
    auto *original = terminal.get();
    QVERIFY(ledger.registerBacking(f.replica, backing, initialClass, &f.error, &terminal));
    QVERIFY(!original->backingNode.second.nativeBinding); // Generic registration owns no CPU capability yet.
    auto physical = KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc);
    const auto targetReplica = physical.target();
    QVERIFY(physical.isValid()); const auto target = targetReplica;
    {
        auto cancelled = ledger.prepareBackingHandoff(f.replica, target, &f.error);
        QVERIFY2(cancelled.isValid(), qPrintable(f.error));
        QCOMPARE(ledger.backingClass(f.replica), initialClass);
        QCOMPARE(ledger.backingClass(target), KisBackingBudgetClass::Count);
        QVERIFY(!ledger.prepareBackingHandoff(f.replica, target).isValid());
        QVERIFY(!ledger.reclassifyBacking(f.replica, KisBackingBudgetClass::RetirementDebt));
        QVERIFY(!f.swap()); // The accounting claim also excludes domain changes.
        auto moved = std::move(cancelled); QVERIFY(!cancelled.isValid());
        KisBackingHandoffReservation assigned; assigned = std::move(moved);
        QVERIFY(!moved.isValid()); QVERIFY(assigned.isValid());
    }
    for (const auto &bucket : budget.usage().buckets) QCOMPARE(bucket.reserved.cpuRam, 0u);
    const auto parentBefore = parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    auto prepared = ledger.prepareBackingHandoff(f.replica, target, &f.error);
    QVERIFY2(prepared.isValid(), qPrintable(f.error));
    QCOMPARE(budget.usage().buckets[size_t(initialClass)].live.cpuRam, bytes);
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].reserved.cpuRam, optional ? quint64(0) : bytes);
    if (optional) {
        QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::ActivePending)].reserved.cpuRam, bytes);
        QCOMPARE(parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam, parentBefore);
    }
    QVERIFY(physical.tryClaim());
    auto writer = physical.commit(); QVERIFY(writer.isValid());
    QVERIFY(!original->backingNode.second.nativeBinding); // Cancelled preparations did not install it.
    QVERIFY(ledger.commitBackingHandoff(std::move(prepared), writer.binding()));
    QCOMPARE(original->backingNode.second.nativeBinding, f.binding);
    QVERIFY(!prepared.isValid());
    QCOMPARE(parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
        parentBefore - (shared && optional ? bytes : 0));
    QVERIFY(!ledger.commitBackingHandoff(std::move(prepared)));
    QCOMPARE(ledger.backingClass(f.replica), KisBackingBudgetClass::Count);
    QCOMPARE(ledger.backingClass(target), KisBackingBudgetClass::ActivePending);
    auto forged = target; ++forged.version.generation.value;
    QCOMPARE(ledger.backingClass(forged), KisBackingBudgetClass::Count);
    KisPageOwnerLedgerTestAccess::withdrawRegistration(ledger, f.replica); // Late old-generation release is harmless.
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::ActivePending)].live.cpuRam, bytes);
    auto *data = static_cast<quint8 *>(writer.pinResident()); QVERIFY(data);
    data[0] = 0x75; data[bytes - 1] = 0x76; writer.reset();
    physical.reset();
    // Representative identity must follow the retag for swap/restore accounting.
    QVERIFY(f.swap());
    QVERIFY(ledger.synchronizeBackingDomains(&f.error));
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::ActivePending)].live.ssd, bytes);
    auto read = f.provider->resolveAccess({300}, {301}, target, cpu, KisPageAccessMode::Read);
    QVERIFY(read.isValid());
    QCOMPARE(static_cast<const quint8 *>(read.cpuReadData)[0], quint8(0x75));
    QCOMPARE(static_cast<const quint8 *>(read.cpuReadData)[bytes - 1], quint8(0x76));
    f.provider->releaseAccess(std::move(read), {});
    QVERIFY(ledger.synchronizeBackingDomains(&f.error));
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::ActivePending)].live.cpuRam, bytes);
    if (cancelWrite) {
        QVERIFY(ledger.reclassifyBacking(target, KisBackingBudgetClass::RetirementDebt, &f.error));
    } else {
        QVERIFY(ledger.reclassifyBacking(target, KisBackingBudgetClass::Current, &f.error));
        QVERIFY(ledger.reclassifyBacking(target, KisBackingBudgetClass::RetirementDebt, &f.error));
    }
    const auto terminalBytes = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    QVERIFY(f.provider->retire({302}, target, {}).isValid());
    KisPageOwnerLedgerTestAccess::withdrawRegistration(ledger, target);
    const quint64 retainedCache = terminalBytes - retirementStorageBytes(budget);
    const auto finalUsage = budget.usage();
    for (size_t i = 0; i < finalUsage.buckets.size(); ++i) {
        const auto &bucket = finalUsage.buckets[i];
        QCOMPARE(bucket.live.cpuRam, i == size_t(KisBackingBudgetClass::MetadataArena) ? retainedCache : quint64(0)); QCOMPARE(bucket.live.ssd, 0u);
        QCOMPARE(bucket.reserved.cpuRam, 0u); QCOMPARE(bucket.reserved.ssd, 0u);
    }
}

void KisPageStorePhysicalClaimTest::backingHandoffAccountingGuards_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("guard");
    QTest::addColumn<bool>("optional");
    const char *names[] = {"provider", "epoch", "slot", "generation", "logical-generation",
        "key", "layout", "domain", "invalid-source", "forged-source", "history-class",
        "pending-budget", "debt-budget", "handoff-claim", "swapped", "alias"};
    for (int bpp : {1, 4, 8, 16}) for (int i = 0; i < int(std::size(names)); ++i) {
        QTest::newRow(qPrintable(QString("bpp%1-%2").arg(bpp).arg(names[i]))) << bpp << i << false;
        // History is still forbidden; Optional's debt independence is tested
        // by the complete checkout/return/cancellation cases above.
        if (i != 10 && i != 12)
            QTest::newRow(qPrintable(QString("bpp%1-%2-optional").arg(bpp).arg(names[i]))) << bpp << i << true;
    }
}

void KisPageStorePhysicalClaimTest::backingHandoffAccountingGuards()
{
    QFETCH(int, bpp); QFETCH(int, guard); QFETCH(bool, optional);
    Fixture f; QVERIFY(f.init(bpp));
    KisPageBackingLimits limits;
    if (guard == 11) limits.activePendingBytes = f.replica.layout.byteSize;
    if (guard == 12) limits.retirementDebtBytes = f.replica.layout.byteSize;
    KisBackingBudgetController budget(limits);
    KisPageOwnerLedger ledger;
    QVERIFY(ledger.configure(f.completions)); ledger.attachBackingBudget(budget);
    QVERIFY(ledger.registerProvider(f.provider));
    auto initialClass = guard == 10 ? KisBackingBudgetClass::RetainedHistory
        : optional ? KisBackingBudgetClass::OptionalCache : KisBackingBudgetClass::Current;
    KisBackingBudgetDelta initial;
    initial.bucket(size_t(initialClass)).cpuRam = f.replica.layout.byteSize;
    auto backing = budget.reserve(initial, &f.error);
    QVERIFY(ledger.registerBacking(f.replica, backing, initialClass, &f.error));
    KisBackingBudgetReservation budgetBlocker;
    if (guard == 11 || guard == 12) {
        KisBackingBudgetDelta pressure;
        pressure.bucket(size_t(guard == 11 ? KisBackingBudgetClass::ActivePending
                                            : KisBackingBudgetClass::RetirementDebt)).cpuRam = f.replica.layout.byteSize;
        budgetBlocker = budget.reserve(pressure, &f.error);
        QVERIFY(budgetBlocker.isValid());
    }
    auto physical = KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc);
    const auto targetReplica = physical.target();
    QVERIFY(physical.isValid());
    auto source = f.replica; auto target = targetReplica;
    switch (guard) {
    case 0: ++target.provider.value; break;
    case 1: ++target.providerEpoch.value; break;
    case 2: ++target.allocation.slot; break;
    case 3: ++target.allocation.generation; break;
    case 4: target.version = source.version; break;
    case 5: target.version.key = key(1); break;
    case 6: ++target.layout.byteSize; break;
    case 7: target.domain = KisPageAccessDomain::Ssd; break;
    case 8: source = {}; break;
    case 9: source.version.generation.value = 2; target.version.generation.value = 3; break;
    }
    auto existingHandoff = guard == 13 ? ledger.prepareBackingHandoff(source, target, &f.error)
                                      : KisBackingHandoffReservation{};
    if (guard == 13) QVERIFY(existingHandoff.isValid());
    if (guard == 14) QVERIFY(f.swap());
    KisReplicaOperation alias;
    if (guard == 15) {
        auto captured = f.provider->captureCompletedTileSource(f.desc, f.tile);
        QVERIFY(captured);
        alias = f.provider->prepareSynchronousSource({310}, captured, {key(1), {1}},
            f.desc, KisReplicaSourceUse::ImmutableAlias, KisPagePriority::Normal);
        QVERIFY(alias.isValid());
        auto aliasBudget = budget.reserve(initial, &f.error);
        QVERIFY(ledger.registerBacking(alias.replica, aliasBudget, initialClass, &f.error));
    }
    QVERIFY(!ledger.prepareBackingHandoff(source, target, &f.error).isValid());
    QVERIFY(!f.error.isEmpty());
    QCOMPARE(ledger.backingClass(f.replica), initialClass);
    QCOMPARE(ledger.backingClass(targetReplica), KisBackingBudgetClass::Count);
    existingHandoff.release();
    if (alias.isValid()) {
        QVERIFY(ledger.reclassifyBacking(alias.replica, KisBackingBudgetClass::RetirementDebt));
        QVERIFY(f.provider->retire({311}, alias.replica, {}).isValid());
        KisPageOwnerLedgerTestAccess::withdrawRegistration(ledger, alias.replica);
    }
    if (guard == 14) {
        auto read = f.provider->resolveAccess({312}, {313}, f.replica, cpu, KisPageAccessMode::Read);
        QVERIFY(read.isValid()); f.provider->releaseAccess(std::move(read), {});
    }
    if (guard == 10) QVERIFY(ledger.reclassifyBacking(f.replica, KisBackingBudgetClass::Current));
    budgetBlocker.release();
    // Every rejection leaves reusable claims and budget; no half rekey.
    auto retry = ledger.prepareBackingHandoff(f.replica, targetReplica, &f.error);
    QVERIFY2(retry.isValid(), qPrintable(f.error)); retry.release();
    for (const auto &bucket : budget.usage().buckets) QCOMPARE(bucket.reserved.cpuRam, 0u);
    QVERIFY(ledger.reclassifyBacking(f.replica, KisBackingBudgetClass::RetirementDebt));
    QVERIFY(f.provider->retire({314}, f.replica, {}).isValid());
    KisPageOwnerLedgerTestAccess::withdrawRegistration(ledger, f.replica);
}

void KisPageStorePhysicalClaimTest::backingHandoffAccountingAcrossIndexGrowth()
{
    Fixture f; QVERIFY(f.init());
    KisBackingBudgetController budget; KisPageOwnerLedger ledger;
    QVERIFY(ledger.configure(f.completions)); ledger.attachBackingBudget(budget);
    QVERIFY(ledger.registerProvider(f.provider));
    const auto registerCurrent = [&](const KisReplicaHandle &handle) {
        KisBackingBudgetDelta d; d.bucket(size_t(KisBackingBudgetClass::Current)).cpuRam = handle.layout.byteSize;
        auto r = budget.reserve(d, &f.error);
        return ledger.registerBacking(handle, r, KisBackingBudgetClass::Current, &f.error);
    };
    QVERIFY(registerCurrent(f.replica));
    auto old = f.replica;
    QVector<KisReplicaHandle> others;
    quint64 nextOperation = 400;
    int nextKey = 1;
    for (int generation = 2; generation <= 4; ++generation) {
        auto physical = KisCpuBackingHandoff::prepare(f.provider, old, {key(0), {quint64(generation)}}, f.desc);
        const auto targetReplica = physical.target();
        auto prepared = ledger.prepareBackingHandoff(old, targetReplica, &f.error);
        QVERIFY2(prepared.isValid(), qPrintable(f.error));
        if (generation > 2) {
            // Original headroom makes this an identity-only claim. Releasing
            // it must not clear that headroom or leave an occupied cookie.
            const auto reserved = budget.usage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].reserved.cpuRam;
            prepared.release();
            QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].reserved.cpuRam, reserved);
            prepared = ledger.prepareBackingHandoff(old, targetReplica, &f.error);
            QVERIFY2(prepared.isValid(), qPrintable(f.error));
        }
        // Other map insertions preserve the claimed source node. Removing
        // its old successor requires commit to compute its hint afresh.
        for (int i = 0; i < 129; ++i) {
            auto operation = f.provider->requestReplica({nextOperation++},
                {key(nextKey++), {1}}, f.desc, cpu.domain, KisPageAccessMode::Read, KisPagePriority::Normal);
            QVERIFY(operation.isValid()); QVERIFY(registerCurrent(operation.replica));
            others.append(operation.replica);
        }
        const auto neighbour = others.takeFirst();
        QVERIFY(ledger.reclassifyBacking(neighbour, KisBackingBudgetClass::RetirementDebt));
        QVERIFY(f.provider->retire({quint64(800 + generation)}, neighbour, {}).isValid());
        KisPageOwnerLedgerTestAccess::withdrawRegistration(ledger, neighbour);
        QVERIFY(physical.tryClaim()); auto writer = physical.commit();
        QVERIFY(writer.isValid());
        QVERIFY(ledger.commitBackingHandoff(std::move(prepared)));
        QVERIFY(!ledger.commitBackingHandoff(std::move(prepared)));
        QCOMPARE(ledger.backingClass(old), KisBackingBudgetClass::Count);
        old = targetReplica;
        writer.reset();
        QCOMPARE(ledger.backingClass(old), KisBackingBudgetClass::ActivePending);
    }
    others.append(old);
    quint64 retirementId = 900, retainedCache = 0;
    for (const auto &handle : std::as_const(others)) {
        QVERIFY(ledger.reclassifyBacking(handle, KisBackingBudgetClass::RetirementDebt));
        const auto terminalBytes = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
        QVERIFY(f.provider->retire({retirementId++}, handle, {}).isValid()); KisPageOwnerLedgerTestAccess::withdrawRegistration(ledger, handle);
        retainedCache = terminalBytes - retirementStorageBytes(budget);
        QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam, retainedCache);
    }
    const auto finalUsage = budget.usage();
    for (size_t i = 0; i < finalUsage.buckets.size(); ++i) {
        const auto &bucket = finalUsage.buckets[i];
        QCOMPARE(bucket.live.cpuRam, i == size_t(KisBackingBudgetClass::MetadataArena) ? retainedCache : quint64(0)); QCOMPARE(bucket.reserved.cpuRam, 0u);
    }
}

void KisPageStorePhysicalClaimTest::backingHandoffReusesFundedPreparation_data()
{
    QTest::addColumn<bool>("idleCut");
    QTest::newRow("handoff-only") << false;
    QTest::newRow("interleaved-idle-classification") << true;
}

void KisPageStorePhysicalClaimTest::backingHandoffReusesFundedPreparation()
{
    QFETCH(bool, idleCut);
    Fixture f; QVERIFY(f.init());
    KisPageBackingLimits limits;
    limits.metadataArenaBytes = 1024 * 1024;
    KisBackingBudgetController budget(limits);
    KisPageOwnerLedger ledger;
    QVERIFY(ledger.configure(f.completions)); ledger.attachBackingBudget(budget);
    QVERIFY(ledger.registerProvider(f.provider));
    KisBackingBudgetDelta initial;
    initial.bucket(size_t(KisBackingBudgetClass::Current)).cpuRam = f.replica.layout.byteSize;
    auto backing = budget.reserve(initial, &f.error);
    QVERIFY(ledger.registerBacking(f.replica, backing, KisBackingBudgetClass::Current, &f.error));
    auto source = f.replica;
    for (quint64 generation = 2; generation <= 4; ++generation) {
        if (idleCut && generation > 2) {
            const auto change = KisPageSnapshotArray<KisBackingClassChange>{
                {source, KisBackingBudgetClass::ActivePending, KisBackingBudgetClass::IdleCapacity}};
            QVERIFY2(KisPageOwnerLedgerTestAccess::changeClasses(ledger, change, &f.error, false), qPrintable(f.error));
            QCOMPARE(ledger.backingClass(source), KisBackingBudgetClass::ActivePending);
            QVERIFY2(KisPageOwnerLedgerTestAccess::changeClasses(ledger, change, &f.error), qPrintable(f.error));
            QCOMPARE(ledger.backingClass(source), KisBackingBudgetClass::IdleCapacity);
        }
        auto physical = KisCpuBackingHandoff::prepare(f.provider, source, {key(0), {generation}}, f.desc);
        const auto targetReplica = physical.target();
        QVERIFY(physical.isValid());
        KisBackingBudgetReservation pressure;
        if (generation > 2) {
            // Already funded preparation must work without additional metadata
            // headroom. Reusing storage must still acquire the new identity.
            {
                auto first = budget.reserve({}, &f.error);
                auto second = budget.reserve({}, &f.error);
                QVERIFY(first.isValid()); QVERIFY(second.isValid());
            }
            const auto usage = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
            KisBackingBudgetDelta remaining;
            remaining.bucket(size_t(KisBackingBudgetClass::MetadataArena)).cpuRam =
                limits.metadataArenaBytes - usage.live.cpuRam - usage.reserved.cpuRam;
            pressure = budget.reserve(remaining, &f.error);
            QVERIFY2(pressure.isValid(), qPrintable(f.error));
        }
        KisBackingHandoffReservation prepared;
        try {
            prepared = ledger.prepareBackingHandoff(source, targetReplica, &f.error);
        } catch (const std::bad_alloc &) {
            QFAIL("funded handoff preparation requested additional metadata storage");
        }
        QVERIFY2(prepared.isValid(), qPrintable(f.error));
        QVERIFY(!ledger.prepareBackingHandoff(source, targetReplica, &f.error).isValid());
        if (generation == 3) {
            // Cancellation releases the new claim. A retry must neither keep
            // the detached old key live nor skip the new class/physical claim.
            prepared.release();
            prepared = ledger.prepareBackingHandoff(source, targetReplica, &f.error);
            QVERIFY2(prepared.isValid(), qPrintable(f.error));
        }
        QVERIFY(physical.tryClaim());
        auto writer = physical.commit(); QVERIFY(writer.isValid());
        QVERIFY(ledger.commitBackingHandoff(std::move(prepared)));
        QCOMPARE(ledger.backingClass(source), KisBackingBudgetClass::Count);
        source = targetReplica;
        QCOMPARE(ledger.backingClass(source), KisBackingBudgetClass::ActivePending);
        writer.reset();
        pressure.release();
    }
    // The same slot also serves ordinary classification. Its empty storage
    // remains charged until the original ledger's final destruction.
    QVERIFY(ledger.reclassifyBacking(source, KisBackingBudgetClass::RetirementDebt, &f.error));
    QVERIFY(f.provider->retire({900}, source, {}).isValid());
    KisPageOwnerLedgerTestAccess::withdrawRegistration(ledger, source);
    for (const auto &bucket : budget.usage().buckets) QCOMPARE(bucket.reserved.cpuRam, 0u);
}

void KisPageStorePhysicalClaimTest::cachedBindingRequiresExactIdentity_data()
{
    QTest::addColumn<int>("bpp");
    QTest::addColumn<int>("mismatch");
    QTest::addColumn<int>("held");
    for (int bpp : {1, 4, 8, 16}) for (int mismatch = 0; mismatch < 5; ++mismatch)
        for (int held = 0; held < 3; ++held)
            QTest::newRow(qPrintable(QString("bpp%1-identity%2-held%3").arg(bpp).arg(mismatch).arg(held)))
                << bpp << mismatch << held;
}

void KisPageStorePhysicalClaimTest::cachedBindingRequiresExactIdentity()
{
    QFETCH(int, bpp); QFETCH(int, mismatch); QFETCH(int, held);
    Fixture f; QVERIFY(f.init(bpp));
    const auto exact = f.replica.allocationIdentity();
    auto wrongHandle = f.replica;
    switch (mismatch) {
    case 0: ++wrongHandle.provider.value; break;
    case 1: ++wrongHandle.providerEpoch.value; break;
    case 2: ++wrongHandle.allocation.slot; break;
    case 3: ++wrongHandle.allocation.generation; break;
    case 4: wrongHandle.allocation = {}; break;
    }
    const auto wrong = wrongHandle.allocationIdentity();
    const void *read = held == 1 ? f.binding->acquireRead(exact, true) : nullptr;
    auto writer = held == 2 ? KisCpuWriteBindingReservation::acquire(f.binding, exact)
                            : KisCpuWriteBindingReservation{};
    const auto cleanup = qScopeGuard([&] { if (read) f.binding->releaseRead(); });
    if (held == 1) QVERIFY(read);
    if (held == 2) QVERIFY(writer.isValid());
    // A cached pointer to the binding bypasses provider lookup. Every new
    // access still has to match the physical identity at the pin gate.
    for (bool resident : {true, false}) {
        KisCpuResidentReadStatus status;
        QVERIFY(!f.binding->acquireRead(wrong, resident, &status));
        QCOMPARE(status, KisCpuResidentReadStatus::InvalidIdentity);
    }
    QVERIFY(!f.binding->acquireWrite(wrong));
    QVERIFY(!KisCpuWriteBindingReservation::acquire(f.binding, wrong).isValid());
    QVERIFY(!f.binding->retire(wrong));
    QVERIFY(!f.binding->acquireAccess({40}, {41}, wrongHandle, cpu, KisPageAccessMode::Read).isValid());
    QVERIFY(!f.binding->acquireAccess({42}, {43}, wrongHandle, cpu, KisPageAccessMode::Write).isValid());
    writer.reset();
    if (read) { f.binding->releaseRead(); read = nullptr; }
    // Rejections must neither acquire a hidden pin nor revoke the live
    // allocation. Correct access and retirement still succeed afterwards.
    auto *bytes = static_cast<quint8 *>(f.binding->acquireWrite(exact));
    QVERIFY(bytes);
    QCOMPARE(bytes[0], quint8(0x31));
    bytes[0] = 0x73;
    f.binding->releaseWrite();
    read = f.binding->acquireRead(exact, true);
    QVERIFY(read);
    QCOMPARE(static_cast<const quint8 *>(read)[0], quint8(0x73));
    f.binding->releaseRead(); read = nullptr;
    QVERIFY(f.provider->retire({50}, f.replica, {}).isValid());
}

void KisPageStorePhysicalClaimTest::parkedWriterReservation_data()
{
    QTest::addColumn<int>("bpp");
    for (int bpp : {1, 4, 8, 16}) QTest::newRow(qPrintable(QString::number(bpp))) << bpp;
}

void KisPageStorePhysicalClaimTest::parkedWriterReservation()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp));
    QVERIFY(f.binding->acquireRead(f.replica.allocationIdentity(), true));
    QVERIFY(!KisCpuWriteBindingReservation::acquire(f.binding, f.replica.allocationIdentity()).isValid());
    f.binding->releaseRead();
    auto writer = KisCpuWriteBindingReservation::acquire(f.binding, f.replica.allocationIdentity()); QVERIFY(writer.isValid());
    QVERIFY(!KisCpuWriteBindingReservation::acquire(f.binding, f.replica.allocationIdentity()).isValid());
    auto *bytes = static_cast<quint8 *>(writer.pinResident()); QVERIFY(bytes);
    memset(bytes, 0x73, size_t(f.desc.minimumByteSize()));
    QVERIFY(!f.swap());
    writer.unpin();
    // A parked reservation still excludes every binding consumer, but no
    // longer owns the physical swap barrier. No byte pointer may be reused.
    QVERIFY(!f.binding->acquireRead(f.replica.allocationIdentity(), true));
    QVERIFY(!f.binding->acquireWrite(f.replica.allocationIdentity()));
    QVERIFY(!f.binding->retire(f.replica.allocationIdentity()));
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
    const auto *read = static_cast<const quint8 *>(f.binding->acquireRead(f.replica.allocationIdentity(), true)); QVERIFY(read);
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
    auto writer = KisCpuWriteBindingReservation::acquire(f.binding, f.replica.allocationIdentity()); QVERIFY(writer.isValid());
    auto *bytes = pinned ? static_cast<quint8 *>(writer.pinResident()) : nullptr;
    if (pinned) { QVERIFY(bytes); bytes[0] = 0x76; }
    f.provider.reset(); // revoke may not free a borrowed pointer/token's backing
    if (pinned) QCOMPARE(bytes[0], quint8(0x76));
    writer.unpin();
    KisCpuResidentReadStatus status;
    QVERIFY(!writer.pinResident(&status)); QCOMPARE(status, KisCpuResidentReadStatus::Retired);
    QVERIFY(!writer.materialize());
    writer.reset();
    QVERIFY(!f.binding->acquireRead(f.replica.allocationIdentity(), true));
}

void KisPageStorePhysicalClaimTest::parkedWriterConcurrentConsumers()
{
    Fixture f; QVERIFY(f.init());
    auto writer = KisCpuWriteBindingReservation::acquire(f.binding, f.replica.allocationIdentity()); QVERIFY(writer.isValid());
    QSemaphore inspect, inspected;
    std::atomic<bool> stop{false}, failed{false};
    std::thread consumer([&] {
        for (;;) {
            inspect.acquire();
            if (stop.load()) break;
            KisCpuResidentReadStatus status;
            const void *read = f.binding->acquireRead(f.replica.allocationIdentity(), true, &status, true);
            if (read) { failed = true; f.binding->releaseRead(); }
            if (status != KisCpuResidentReadStatus::Busy) failed = true;
            auto generic = f.provider->resolveAccess({20}, {21}, f.replica, cpu, KisPageAccessMode::Read);
            if (generic.isValid()) { failed = true; f.provider->releaseAccess(std::move(generic), {}); }
            if (f.provider->prepareSynchronousWriteCopy({22}, f.replica, {key(0), {2}}, f.desc,
                    KisPagePriority::Normal).isValid()) failed = true;
            if (f.binding->retire(f.replica.allocationIdentity())) failed = true;
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
    const auto *read = static_cast<const quint8 *>(f.binding->acquireRead(f.replica.allocationIdentity(), true)); QVERIFY(read);
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
        parked = KisCpuWriteBindingReservation::acquire(f.binding, f.replica.allocationIdentity());
        QVERIFY(parked.pinResident());
        parked.unpin();
    } else {
        data = f.binding->acquireWrite(f.replica.allocationIdentity());
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
    const auto *bytes = static_cast<const quint8 *>(copiedBinding->acquireRead(copy.replica.allocationIdentity(), false)); QVERIFY(bytes);
    const bool correct = bytes[0] == 0x31 && bytes[copy.replica.layout.byteSize - 1] == 0x31;
    copiedBinding->releaseRead(); QVERIFY(correct);
    transfer.operation = {26}; QVERIFY(f.provider->transfer(transfer, KisPagePriority::Normal).isValid());
    QVERIFY(f.provider->retire({27}, copy.replica, {}).isValid());
    QVERIFY(f.provider->retire({28}, f.replica, {}).isValid());
    QVERIFY(f.provider->retire({29}, target.replica, {}).isValid());
}


void KisPageStorePhysicalClaimTest::physicalBackingHandoffGuards_data()
{
    QTest::addColumn<int>("bpp");
    QTest::addColumn<int>("blocker");
    const char *names[] = {"none", "native-read", "native-write", "parked-writer",
        "generic-read", "generic-write", "raw-pin", "external-ref", "legacy-user",
        "immutable-source", "alias", "foreign-alias", "preclone", "swapped", "pooler-iteration"};
    for (int bpp : {1, 4, 8, 16}) for (int i = 0; i < int(std::size(names)); ++i)
        QTest::newRow(qPrintable(QString("bpp%1-%2").arg(bpp).arg(names[i]))) << bpp << i;
}

void KisPageStorePhysicalClaimTest::physicalBackingHandoffGuards()
{
    QFETCH(int, bpp); QFETCH(int, blocker);
    Fixture f; QVERIFY(f.init(bpp));
    auto candidate = KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc);
    QVERIFY(candidate.isValid());
    QCOMPARE(candidate.target().allocation.slot, f.replica.allocation.slot);
    QCOMPARE(candidate.target().allocation.generation, f.replica.allocation.generation + 1);
    const void *read = blocker == 1 ? f.binding->acquireRead(f.replica.allocationIdentity(), true) : nullptr;
    void *write = blocker == 2 ? f.binding->acquireWrite(f.replica.allocationIdentity()) : nullptr;
    auto parked = blocker == 3 ? KisCpuWriteBindingReservation::acquire(f.binding, f.replica.allocationIdentity())
                               : KisCpuWriteBindingReservation{};
    auto generic = blocker == 4 || blocker == 5
        ? f.provider->resolveAccess({60}, {61}, f.replica, cpu,
            blocker == 4 ? KisPageAccessMode::Read : KisPageAccessMode::Write) : KisReplicaAccess{};
    bool rawPin = blocker == 6 && f.tile->tryBlockSwapping();
    if (blocker == 7) f.tile->ref();
    if (blocker == 8) f.tile->acquire();
    std::shared_ptr<const KisPageReplicaSource> source;
    if (blocker == 9 || blocker == 10)
        source = f.provider->captureCompletedTileSource(f.desc, f.tile);
    auto alias = blocker == 10 ? f.provider->prepareSynchronousSource({62}, source, {key(1), {1}},
        f.desc, KisReplicaSourceUse::ImmutableAlias, KisPagePriority::Normal) : KisReplicaOperation{};
    if (blocker == 10) source.reset(); // The alias alone must exclude transfer.
    std::shared_ptr<KisTiles3PageReplicaProvider> foreign;
    KisReplicaHandle foreignAlias;
    if (blocker == 11) {
        foreign = std::make_shared<KisTiles3PageReplicaProvider>();
        QVERIFY(foreign->configure({{201}, {1}, 16 * 1024 * 1024}, f.completions));
        foreignAlias = foreign->adoptInitialTile({key(1), {1}}, f.desc, f.tile);
    }
    if (blocker == 12) {
        QVERIFY(f.tile->blockSwapping());
        f.tile->m_clonesStack.push(new KisTileData(*f.tile, false));
        f.tile->unblockSwapping();
    }
    if (blocker == 13) QVERIFY(f.swap());
    auto *iteration = blocker == 14 ? KisTileDataStore::instance()->beginIteration() : nullptr;
    const auto release = qScopeGuard([&] {
        candidate.reset();
        if (iteration) KisTileDataStore::instance()->endIteration(iteration);
        if (read) f.binding->releaseRead();
        if (write) f.binding->releaseWrite();
        if (rawPin) f.tile->unblockSwapping();
        if (blocker == 7) f.tile->deref();
        if (blocker == 8) f.tile->release();
    });
    if (blocker == 1) QVERIFY(read);
    if (blocker == 2) QVERIFY(write);
    if (blocker == 3) QVERIFY(parked.isValid());
    if (blocker == 4 || blocker == 5) QVERIFY(generic.isValid());
    if (blocker == 6) QVERIFY(rawPin);
    if (blocker == 9) QVERIFY(source);
    if (blocker == 10) QVERIFY(alias.isValid());
    if (blocker == 11) QVERIFY(foreignAlias.isValid());
    // A new candidate rejects an already known native blocker. The candidate
    // prepared before the blocker must still reject at the final claim below.
    if (blocker >= 1 && blocker <= 3)
        QVERIFY(!KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc).isValid());
    QCOMPARE(candidate.tryClaim(), blocker == 0);
    candidate.reset();
    QVERIFY(f.provider->validate(f.replica, f.desc)); // No retag on rejection/cancel.
    if (generic.isValid()) f.provider->releaseAccess(std::move(generic), {});
    parked.reset(); source.reset();
    if (alias.isValid()) QVERIFY(f.provider->retire({63}, alias.replica, {}).isValid());
    foreign.reset();
    if (blocker == 12) {
        KisTileData *clone = nullptr;
        QVERIFY(f.tile->m_clonesStack.pop(clone)); delete clone;
    }
    if (iteration) { KisTileDataStore::instance()->endIteration(iteration); iteration = nullptr; }
    if (read) { f.binding->releaseRead(); read = nullptr; }
    if (write) { f.binding->releaseWrite(); write = nullptr; }
    if (rawPin) { f.tile->unblockSwapping(); rawPin = false; }
    if (blocker == 7) { f.tile->deref(); blocker = 0; }
    if (blocker == 8) { f.tile->release(); blocker = 0; }
    // A swapped source is not faulted in merely to create a handoff hit.
    if (blocker == 13) {
        KisCpuResidentReadStatus status;
        QVERIFY(!f.binding->acquireRead(f.replica.allocationIdentity(), true, &status));
        QCOMPARE(status, KisCpuResidentReadStatus::NonResident);
        QVERIFY(f.binding->acquireRead(f.replica.allocationIdentity(), false));
        f.binding->releaseRead();
    }
    auto retry = KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc);
    QVERIFY(retry.tryClaim()); retry.reset();
    QVERIFY(f.binding->acquireRead(f.replica.allocationIdentity(), true));
    f.binding->releaseRead();
}

void KisPageStorePhysicalClaimTest::physicalBackingHandoffLifecycle_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("outcome");
    for (int bpp : {1, 4, 8, 16}) for (int outcome = 0; outcome < 4; ++outcome)
        QTest::newRow(qPrintable(QString("bpp%1-outcome%2").arg(bpp).arg(outcome))) << bpp << outcome;
}

void KisPageStorePhysicalClaimTest::physicalBackingHandoffLifecycle()
{
    QFETCH(int, bpp); QFETCH(int, outcome);
    Fixture f; QVERIFY(f.init(bpp));
    const auto beforeWork = f.provider->payloadWork();
    const auto beforeMemory = f.provider->memoryUsage();
    const void *original = f.binding->acquireRead(f.replica.allocationIdentity(), true); QVERIFY(original);
    f.binding->releaseRead();
    auto first = KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc);
    auto stale = KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc);
    QVERIFY(first.isValid()); QVERIFY(stale.isValid());
    std::unique_ptr<Fixture> other;
    KisCpuBackingHandoff replacement;
    if (outcome == 3) {
        other = std::make_unique<Fixture>(); QVERIFY(other->init(bpp));
        replacement = KisCpuBackingHandoff::prepare(other->provider, other->replica, {key(0), {2}}, other->desc);
        QVERIFY(replacement.isValid());
    }
    QVERIFY(!first.commit().isValid()); // Preparation alone grants nothing.
    QVERIFY(first.isValid()); QVERIFY(first.tryClaim()); QVERIFY(!first.tryClaim());
    QVERIFY(!stale.tryClaim());
    auto claimed = std::move(first); QVERIFY(!first.isValid()); QVERIFY(!first.isClaimed());
    QVERIFY(!first.commit().isValid()); QVERIFY(!first.target().isValid());
    KisCpuResidentReadStatus status;
    QVERIFY(!f.binding->acquireRead(f.replica.allocationIdentity(), true, &status));
    QCOMPARE(status, KisCpuResidentReadStatus::Busy);
    QVERIFY(!KisCpuWriteBindingReservation::acquire(f.binding, f.replica.allocationIdentity()).isValid());
    bool busy = false;
    QVERIFY(!f.tile->tryBlockSwapping(&busy)); QVERIFY(busy);
    QVERIFY(!f.provider->resolveAccess({70}, {71}, f.replica, cpu, KisPageAccessMode::Read).isValid());
    QVERIFY(!f.provider->prepareSynchronousWriteCopy({72}, f.replica, {key(0), {2}}, f.desc,
                                                   KisPagePriority::Normal).isValid());
    QVERIFY(!f.provider->retire({73}, f.replica, {}).isValid());
    if (outcome == 0) {
        claimed.reset(); QVERIFY(!claimed.target().isValid());
        QVERIFY(f.provider->validate(f.replica, f.desc));
        QVERIFY(stale.tryClaim()); stale.reset();
        const auto *bytes = static_cast<const quint8 *>(f.binding->acquireRead(f.replica.allocationIdentity(), true));
        QVERIFY(bytes); QCOMPARE(QByteArray(reinterpret_cast<const char *>(bytes), int(f.desc.minimumByteSize())),
                                 QByteArray(int(f.desc.minimumByteSize()), char(0x31)));
        f.binding->releaseRead(); return;
    }
    if (outcome == 2) { first = std::move(claimed); claimed = std::move(first); }
    if (outcome == 3) {
        // Move assignment first cancels the destination's previous claim.
        QVERIFY(replacement.tryClaim());
        replacement = std::move(claimed);
        QVERIFY(other->binding->acquireRead(other->replica.allocationIdentity(), true));
        other->binding->releaseRead();
        claimed = std::move(replacement);
    }
    const auto target = claimed.target();
    auto writer = claimed.commit(); QVERIFY(writer.isValid());
    QVERIFY(!claimed.isValid()); QVERIFY(!claimed.isClaimed()); QVERIFY(!claimed.commit().isValid());
    QVERIFY(!claimed.target().isValid()); QVERIFY(stale.target() == target);
    QVERIFY(!stale.tryClaim()); stale.reset();
    QVERIFY(!f.provider->validate(f.replica, f.desc));
    QVERIFY(!f.provider->cpuResidentBinding(f.replica));
    QCOMPARE(f.provider->cpuResidentBinding(target), f.binding);
    QVERIFY(f.provider->validate(target, f.desc));
    QVERIFY(!f.binding->acquireRead(f.replica.allocationIdentity(), true, &status));
    QCOMPARE(status, KisCpuResidentReadStatus::InvalidIdentity);
    QVERIFY(!f.binding->acquireWrite(f.replica.allocationIdentity()));
    QVERIFY(!f.binding->retire(f.replica.allocationIdentity()));
    auto *bytes = static_cast<quint8 *>(writer.pinResident()); QVERIFY(bytes); QCOMPARE(bytes, original);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(bytes), int(f.desc.minimumByteSize())),
             QByteArray(int(f.desc.minimumByteSize()), char(0x31)));
    memset(bytes, 0x79, size_t(f.desc.minimumByteSize()));
    claimed.reset(); // Consumed candidate must not release the returned writer.
    QVERIFY(!f.binding->acquireRead(target.allocationIdentity(), true));
    QVERIFY(!f.swap());
    writer.unpin(); QVERIFY(f.swap());
    QVERIFY(!writer.pinResident(&status)); QCOMPARE(status, KisCpuResidentReadStatus::NonResident);
    bytes = static_cast<quint8 *>(writer.materialize()); QVERIFY(bytes);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(bytes), int(f.desc.minimumByteSize())),
             QByteArray(int(f.desc.minimumByteSize()), char(0x79)));
    writer.reset();
    auto again = KisCpuBackingHandoff::prepare(f.provider, target, {key(0), {3}}, f.desc);
    QVERIFY(again.tryClaim()); const auto newest = again.target();
    auto nextWriter = again.commit(); QVERIFY(nextWriter.isValid()); nextWriter.reset(); again.reset();
    QCOMPARE(newest.allocation.generation, f.replica.allocation.generation + 2);
    QVERIFY(!f.binding->acquireRead(target.allocationIdentity(), true));
    QVERIFY(f.binding->acquireRead(newest.allocationIdentity(), true)); f.binding->releaseRead();
    QCOMPARE(f.provider->payloadWork().defaultInitializedPages, beforeWork.defaultInitializedPages);
    QCOMPARE(f.provider->payloadWork().nativeDuplicatePages, beforeWork.nativeDuplicatePages);
    QCOMPARE(f.provider->payloadWork().explicitCopyPages, beforeWork.explicitCopyPages);
    QCOMPARE(f.provider->memoryUsage().committedBytes, beforeMemory.committedBytes);
    QVERIFY(f.provider->retire({90}, newest, {}).isValid());
}

void KisPageStorePhysicalClaimTest::physicalBackingHandoffConcurrentConsumers()
{
    Fixture f; QVERIFY(f.init());
    auto candidate = KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc);
    QSemaphore inspect, inspected;
    std::atomic<bool> stop{false}, failed{false};
    std::thread reader([&] {
        for (;;) {
            inspect.acquire(); if (stop.load()) break;
            KisCpuResidentReadStatus status;
            const void *p = f.binding->acquireRead(f.replica.allocationIdentity(), true, &status);
            if (p) { failed = true; f.binding->releaseRead(); }
            if (status != KisCpuResidentReadStatus::Busy && status != KisCpuResidentReadStatus::InvalidIdentity)
                failed = true;
            auto access = f.provider->resolveAccess({80}, {81}, f.replica, cpu, KisPageAccessMode::Read);
            if (access.isValid()) { failed = true; f.provider->releaseAccess(std::move(access), {}); }
            if (f.binding->retire(f.replica.allocationIdentity())) failed = true;
            inspected.release();
        }
    });
    const auto join = qScopeGuard([&] { stop = true; inspect.release(); reader.join(); });
    QVERIFY(candidate.tryClaim());
    inspect.release(); QVERIFY(inspected.tryAcquire(1, 5000));
    auto writer = candidate.commit(); QVERIFY(writer.isValid());
    inspect.release(); QVERIFY(inspected.tryAcquire(1, 5000));
    writer.reset();
    inspect.release(); QVERIFY(inspected.tryAcquire(1, 5000));
    QVERIFY(!failed.load());
}

void KisPageStorePhysicalClaimTest::physicalBackingHandoffRetainsProvider()
{
    Fixture f; QVERIFY(f.init());
    std::weak_ptr<KisTiles3PageReplicaProvider> weak = f.provider;
    auto candidate = KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc);
    f.provider.reset();
    QVERIFY(!weak.expired()); QVERIFY(candidate.tryClaim());
    auto writer = candidate.commit(); QVERIFY(writer.isValid());
    auto *bytes = static_cast<quint8 *>(writer.pinResident()); QVERIFY(bytes); bytes[0] = 0x69;
    QVERIFY(!weak.expired());
    candidate.reset(); QVERIFY(weak.expired()); // Revocation is deferred past consumption.
    QCOMPARE(bytes[0], quint8(0x69)); // Existing resident holder is still alive.
    writer.unpin();
    KisCpuResidentReadStatus status;
    QVERIFY(!writer.pinResident(&status)); QCOMPARE(status, KisCpuResidentReadStatus::Retired);
    writer.reset();
}


void KisPageStorePhysicalClaimTest::physicalBackingHandoffRejectsWrongIdentity_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("mismatch");
    for (int bpp : {1, 4, 8, 16}) for (int mismatch = 0; mismatch < 18; ++mismatch)
        QTest::newRow(qPrintable(QString("bpp%1-mismatch%2").arg(bpp).arg(mismatch))) << bpp << mismatch;
}
void KisPageStorePhysicalClaimTest::physicalBackingHandoffRejectsWrongIdentity()
{
    QFETCH(int, bpp); QFETCH(int, mismatch);
    Fixture f; QVERIFY(f.init(bpp));
    auto source = f.replica; KisPageVersion target{key(0), {2}}; auto desc = f.desc;
    switch (mismatch) {
    case 0: ++source.provider.value; break;
    case 1: ++source.providerEpoch.value; break;
    case 2: ++source.allocation.slot; break;
    case 3: ++source.allocation.generation; break;
    case 4: ++source.version.generation.value; break;
    case 5: ++source.layout.formatId; break;
    case 6: source.domain = KisPageAccessDomain::Ssd; break;
    case 7: target = {}; break;
    case 8: target = source.version; break;
    case 9: target.key = key(1); break;
    case 10: ++desc.format.formatId; break;
    case 11: ++source.layout.layoutRevision; break;
    case 12: source.layout.pageExtent.setWidth(63); break;
    case 13: source.layout.validRect.setWidth(63); break;
    case 14: ++source.layout.rowStride; break;
    case 15: ++source.layout.byteSize; break;
    case 16: ++source.version.defaultPixelRevision; break;
    case 17: source.version.key = key(1); break;
    }
    auto rejected = KisCpuBackingHandoff::prepare(f.provider, source, target, desc);
    QVERIFY(!rejected.isValid()); QVERIFY(!rejected.tryClaim()); QVERIFY(!rejected.commit().isValid());
    // Native consumers already hold a binding and bypass the provider index.
    // That direct path must still reject every forged stable/logical field.
    rejected = KisCpuBackingHandoff::prepare(f.provider, source, target, desc, f.binding);
    QVERIFY(!rejected.isValid()); QVERIFY(!rejected.target().isValid());
    QVERIFY(!rejected.tryClaim()); QVERIFY(!rejected.commit().isValid());
    auto valid = KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc);
    QVERIFY(valid.tryClaim()); valid.reset();
    QVERIFY(f.provider->retire({95}, f.replica, {}).isValid());
    auto retired = KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc);
    QVERIFY(!retired.isValid());
}

QTEST_GUILESS_MAIN(KisPageStorePhysicalClaimTest)
#include "KisPageStorePhysicalClaimTest.moc"

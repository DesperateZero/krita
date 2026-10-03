/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <QTest>
#include <QScopeGuard>
#include <QSemaphore>
#include <atomic>
#include <thread>
#include <memory>

#include "KisCpuResidentBinding_p.h"
#include "KisTiles3PageReplicaProvider.h"
#include "KisPageOwnerLedger.h"
#include "KisPageWriteCoordinator_p.h"
#include "KisPageRetirementRecord_p.h"
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

quint64 retirementStorageBytes(KisBackingBudgetController &budget, bool lastPhysical = true)
{
    const auto live = [&] {
        return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    // Warm the retained accounting cache, then measure the complete real node.
    { auto warm = kisPreparePageRetirementRecord(&budget); }
    const quint64 before = live();
    auto record = kisPreparePageRetirementRecord(&budget);
    if (!lastPhysical) record->physicalStorage = {};
    return live() - before;
}

}

class KisPageStorePhysicalClaimTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
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
    void handoffDebtFollowsTerminal_data();
    void handoffDebtFollowsTerminal();
    void sharedHandoffDebt_data();
    void sharedHandoffDebt();
    void ledgerAliasMembershipAtCapacity_data()
    {
        QTest::addColumn<bool>("reverse");
        QTest::newRow("middle-representative-last") << false;
        QTest::newRow("head-representative-last") << true;
    }
    void ledgerAliasMembershipAtCapacity();
};

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
    KisBackingBudgetDelta initial; initial.buckets[size_t(KisBackingBudgetClass::Current)].cpuRam = bytes;
    auto reservation = budget.reserve(initial, &f.error); QVERIFY(ledger.registerBacking(f.replica, reservation, KisBackingBudgetClass::Current));
    auto target = f.replica;
    // The second handoff must reuse the original headroom at an exact one-page
    // limit, not demand another retirement or pending reservation.
    for (quint64 generation : {2u, 3u}) {
        auto physical = KisCpuBackingHandoff::prepare(f.provider, target, {key(0), {generation}}, f.desc);
        auto accounting = ledger.prepareBackingHandoff(target, physical.target(), &f.error);
        QVERIFY2(accounting.isValid(), qPrintable(f.error)); QVERIFY(physical.tryClaim());
        auto writer = physical.commit(); QVERIFY(writer.isValid());
        QVERIFY(ledger.commitBackingHandoff(std::move(accounting))); target = physical.target();
        auto *data = static_cast<quint8 *>(writer.pinResident()); QVERIFY(data); data[0] = 0x75; writer.reset();
    }
    const size_t debt = size_t(KisBackingBudgetClass::RetirementDebt);
    if (swapped) QVERIFY(f.swap());
    QVERIFY(ledger.synchronizeBackingDomains(&f.error));
    auto usage = budget.usage();
    QCOMPARE(usage.buckets[debt].reserved.cpuRam, swapped ? 0u : bytes);
    QCOMPARE(usage.buckets[debt].reserved.ssd, swapped ? bytes : 0u);
    KisBackingBudgetDelta pressure; pressure.buckets[debt].cpuRam = 1;
    QVERIFY(!budget.reserve(pressure, &f.error).isValid());
    if (finish == 0) {
        QVERIFY2(ledger.reclassifyBacking(target, KisBackingBudgetClass::RetirementDebt, &f.error), qPrintable(f.error));
    } else if (finish < 3) {
        KisPageTransitionEffect effect; effect.replica = target; quint64 cookie = 0;
        QVERIFY2(ledger.prepareRetirementDebt(&effect, 1, &cookie, &f.error), qPrintable(f.error));
        if (finish == 2) {
            ledger.cancelRetirementDebt(cookie);
            QCOMPARE(ledger.backingClass(target), KisBackingBudgetClass::ActivePending);
            QVERIFY(!budget.reserve(pressure, &f.error).isValid());
            // A cancelled preparation must leave the same headroom able to
            // follow another actual swap-in and swap-out, without new quota.
            if (swapped) {
                auto access = f.provider->resolveAccess({610}, {611}, target, cpu, KisPageAccessMode::Read);
                QVERIFY(access.isValid()); QCOMPARE(static_cast<const quint8 *>(access.cpuReadData)[0], quint8(0x75));
                f.provider->releaseAccess(std::move(access), {}); QVERIFY(f.swap());
            }
            QVERIFY2(ledger.prepareRetirementDebt(&effect, 1, &cookie, &f.error), qPrintable(f.error));
        }
        ledger.commitRetirementDebt(cookie);
    } else {
        auto publication = ledger.prepareBackingChanges({{target, KisBackingBudgetClass::ActivePending, KisBackingBudgetClass::Current}}, {}, &f.error);
        QVERIFY2(publication.isValid(), qPrintable(f.error)); ledger.commitBackingChanges(std::move(publication));
        usage = budget.usage(); QCOMPARE(usage.buckets[debt].reserved.cpuRam, 0u); QCOMPARE(usage.buckets[debt].reserved.ssd, 0u);
        auto available = budget.reserve(pressure, &f.error); QVERIFY(available.isValid()); available.release();
        QVERIFY(ledger.reclassifyBacking(target, KisBackingBudgetClass::RetirementDebt, &f.error));
    }
    QCOMPARE(ledger.backingClass(target), KisBackingBudgetClass::RetirementDebt);
    const auto terminalBytes = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    QVERIFY(f.provider->retire({612}, target, {}).isValid()); ledger.releaseRetiredBacking(target);
    const quint64 retainedCache = terminalBytes - retirementStorageBytes(budget);
    const auto finalUsage = budget.usage();
    for (size_t i = 0; i < finalUsage.buckets.size(); ++i) {
        const auto &bucket = finalUsage.buckets[i];
        QCOMPARE(bucket.live.cpuRam, i == size_t(KisBackingBudgetClass::MetadataArena) ? retainedCache : quint64(0)); QCOMPARE(bucket.live.ssd, 0u);
        QCOMPARE(bucket.reserved.cpuRam, 0u); QCOMPARE(bucket.reserved.ssd, 0u);
    }
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
    KisBackingBudgetDelta initial; initial.buckets[size_t(KisBackingBudgetClass::Current)].cpuRam = bytes;
    auto reservation = budget.reserve(initial, &f.error); QVERIFY(ledger.registerBacking(f.replica, reservation, KisBackingBudgetClass::Current));
    auto physical = KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc);
    const auto target = physical.target(); auto accounting = ledger.prepareBackingHandoff(f.replica, target, &f.error);
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
    quint64 cookie = 0; QVERIFY2(ledger.prepareRetirementDebt(effects.constData(), effects.size(), &cookie, &f.error), qPrintable(f.error));
    ledger.cancelRetirementDebt(cookie);
    QVERIFY2(ledger.prepareRetirementDebt(effects.constData(), effects.size(), &cookie, &f.error), qPrintable(f.error)); ledger.commitRetirementDebt(cookie);
    const auto firstNodeBytes = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    QVERIFY(f.provider->retire({621}, target, {}).isValid()); ledger.releaseRetiredBacking(target);
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
             firstNodeBytes - retirementStorageBytes(budget, false));
    if (keepAlias) {
        QCOMPARE(ledger.backingClass(alias.replica), KisBackingBudgetClass::Current);
        const auto usage = budget.usage(); const auto &debt = usage.buckets[size_t(KisBackingBudgetClass::RetirementDebt)];
        QCOMPARE(debt.reserved.cpuRam, 0u); QCOMPARE(debt.reserved.ssd, 0u);
        QVERIFY(ledger.reclassifyBacking(alias.replica, KisBackingBudgetClass::RetirementDebt));
    }
    const auto terminalBytes = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    QVERIFY(f.provider->retire({622}, alias.replica, {}).isValid()); ledger.releaseRetiredBacking(alias.replica);
    const quint64 retainedCache = terminalBytes - retirementStorageBytes(budget);
    const auto finalUsage = budget.usage();
    for (size_t i = 0; i < finalUsage.buckets.size(); ++i) {
        const auto &bucket = finalUsage.buckets[i];
        QCOMPARE(bucket.live.cpuRam, i == size_t(KisBackingBudgetClass::MetadataArena) ? retainedCache : quint64(0)); QCOMPARE(bucket.live.ssd, 0u);
        QCOMPARE(bucket.reserved.cpuRam, 0u); QCOMPARE(bucket.reserved.ssd, 0u);
    }
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
    const auto allNodeBytes = retirementStorageBytes(budget);
    const auto logicalNodeBytes = retirementStorageBytes(budget, false);
    const auto baseline = live();
    const auto physicalNodeBytes = allNodeBytes - logicalNodeBytes;
    std::array<KisReplicaHandle, 3> replicas{f.replica};
    std::array<KisPageRetirementRecordPointer, 3> records;
    std::array<KisBackingBudgetReservation, 3> reservations;
    KisBackingBudgetDelta initial;
    initial.buckets[size_t(KisBackingBudgetClass::Current)].cpuRam = qint64(f.replica.layout.byteSize);
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
            kisFreeMutationStorage(&budget, fillers[i], fillerBytes[i], 1);
    });
    for (size_t i = 0; i < replicas.size(); ++i) {
        fillerBytes[i] = size_t(limits.metadataArenaBytes - live());
        fillers[i] = kisAllocateMutationStorage(&budget, fillerBytes[i], 1);
        QCOMPARE(live(), limits.metadataArenaBytes);
        QVERIFY2(ledger.registerBacking(replicas[i], reservations[i], KisBackingBudgetClass::Current,
            &f.error, &records[i]), qPrintable(f.error));
        QVERIFY(!records[i]);
        QCOMPARE(live(), limits.metadataArenaBytes - (i ? physicalNodeBytes : 0));
        QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::Current)].live.cpuRam,
                 f.replica.layout.byteSize); // One charge, including every new foreground alias.
    }
    for (const auto &replica : replicas)
        QVERIFY(ledger.reclassifyBacking(replica, KisBackingBudgetClass::RetirementDebt));
    const std::array<size_t, 3> order = reverse ? std::array<size_t, 3>{2, 0, 1}
                                             : std::array<size_t, 3>{1, 0, 2};
    for (size_t n = 0; n < order.size(); ++n) {
        const auto i = order[n];
        const auto before = live();
        QVERIFY(f.provider->retire({710 + n}, replicas[i], {}).isValid());
        ledger.releaseRetiredBacking(replicas[i]);
        QCOMPARE(live(), before - (n == 2 ? allNodeBytes : logicalNodeBytes));
        for (size_t j = n + 1; j < order.size(); ++j) {
            const auto &survivor = replicas[order[j]];
            QCOMPARE(ledger.backingClass(survivor), KisBackingBudgetClass::RetirementDebt);
            QVERIFY(f.provider->validate(survivor, f.desc));
        }
    }
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].live.cpuRam, quint64(0));
    for (size_t i = 0; i < fillers.size(); ++i)
        kisFreeMutationStorage(&budget, std::exchange(fillers[i], nullptr), fillerBytes[i], 1);
    QCOMPARE(live(), baseline);
}

void KisPageStorePhysicalClaimTest::backingHandoffAccounting_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("pending");
    QTest::addColumn<bool>("cancelWrite");
    for (int bpp : {1, 4, 8, 16}) for (bool pending : {false, true})
        for (bool cancel : {false, true})
            QTest::newRow(qPrintable(QString("bpp%1-pending%2-cancel%3").arg(bpp).arg(pending).arg(cancel)))
                << bpp << pending << cancel;
}

void KisPageStorePhysicalClaimTest::backingHandoffAccounting()
{
    QFETCH(int, bpp); QFETCH(bool, pending); QFETCH(bool, cancelWrite);
    Fixture f; QVERIFY(f.init(bpp));
    KisBackingBudgetController budget;
    KisPageOwnerLedger ledger;
    QVERIFY(ledger.configure(f.completions)); ledger.attachBackingBudget(budget);
    QVERIFY(ledger.registerProvider(f.provider));
    const auto initialClass = pending ? KisBackingBudgetClass::ActivePending : KisBackingBudgetClass::Current;
    const quint64 bytes = f.replica.layout.byteSize;
    KisBackingBudgetDelta initial; initial.buckets[size_t(initialClass)].cpuRam = bytes;
    auto backing = budget.reserve(initial, &f.error);
    QVERIFY(ledger.registerBacking(f.replica, backing, initialClass, &f.error));
    auto physical = KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc);
    QVERIFY(physical.isValid()); const auto target = physical.target();
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
    auto prepared = ledger.prepareBackingHandoff(f.replica, target, &f.error);
    QVERIFY2(prepared.isValid(), qPrintable(f.error));
    QCOMPARE(budget.usage().buckets[size_t(initialClass)].live.cpuRam, bytes);
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].reserved.cpuRam, bytes);
    QVERIFY(physical.tryClaim());
    auto writer = physical.commit(); QVERIFY(writer.isValid());
    QVERIFY(ledger.commitBackingHandoff(std::move(prepared)));
    QVERIFY(!prepared.isValid());
    QVERIFY(!ledger.commitBackingHandoff(std::move(prepared)));
    QCOMPARE(ledger.backingClass(f.replica), KisBackingBudgetClass::Count);
    QCOMPARE(ledger.backingClass(target), KisBackingBudgetClass::ActivePending);
    auto forged = target; ++forged.version.generation.value;
    QCOMPARE(ledger.backingClass(forged), KisBackingBudgetClass::Count);
    ledger.releaseRetiredBacking(f.replica); // Late old-generation release is harmless.
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
    ledger.releaseRetiredBacking(target);
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
    const char *names[] = {"provider", "epoch", "slot", "generation", "logical-generation",
        "key", "layout", "domain", "invalid-source", "forged-source", "history-class",
        "pending-budget", "debt-budget", "class-claim", "swapped", "alias"};
    for (int bpp : {1, 4, 8, 16}) for (int i = 0; i < int(std::size(names)); ++i)
        QTest::newRow(qPrintable(QString("bpp%1-%2").arg(bpp).arg(names[i]))) << bpp << i;
}

void KisPageStorePhysicalClaimTest::backingHandoffAccountingGuards()
{
    QFETCH(int, bpp); QFETCH(int, guard);
    Fixture f; QVERIFY(f.init(bpp));
    KisPageBackingLimits limits;
    if (guard == 11) limits.activePendingBytes = f.replica.layout.byteSize;
    if (guard == 12) limits.retirementDebtBytes = f.replica.layout.byteSize;
    KisBackingBudgetController budget(limits);
    KisPageOwnerLedger ledger;
    QVERIFY(ledger.configure(f.completions)); ledger.attachBackingBudget(budget);
    QVERIFY(ledger.registerProvider(f.provider));
    auto initialClass = guard == 10 ? KisBackingBudgetClass::RetainedHistory : KisBackingBudgetClass::Current;
    KisBackingBudgetDelta initial;
    initial.buckets[size_t(initialClass)].cpuRam = f.replica.layout.byteSize;
    auto backing = budget.reserve(initial, &f.error);
    QVERIFY(ledger.registerBacking(f.replica, backing, initialClass, &f.error));
    KisBackingBudgetReservation budgetBlocker;
    if (guard == 11 || guard == 12) {
        KisBackingBudgetDelta pressure;
        pressure.buckets[size_t(guard == 11 ? KisBackingBudgetClass::ActivePending
                                            : KisBackingBudgetClass::RetirementDebt)].cpuRam = f.replica.layout.byteSize;
        budgetBlocker = budget.reserve(pressure, &f.error);
        QVERIFY(budgetBlocker.isValid());
    }
    auto physical = KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc);
    QVERIFY(physical.isValid());
    auto source = f.replica; auto target = physical.target();
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
    auto classClaim = guard == 13 ? ledger.prepareBackingChanges(
        {{f.replica, initialClass, KisBackingBudgetClass::RetainedHistory}}, {}) : KisBackingClassChangeReservation{};
    if (guard == 13) QVERIFY(classClaim.isValid());
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
    QCOMPARE(ledger.backingClass(physical.target()), KisBackingBudgetClass::Count);
    classClaim.release();
    if (alias.isValid()) {
        QVERIFY(ledger.reclassifyBacking(alias.replica, KisBackingBudgetClass::RetirementDebt));
        QVERIFY(f.provider->retire({311}, alias.replica, {}).isValid());
        ledger.releaseRetiredBacking(alias.replica);
    }
    if (guard == 14) {
        auto read = f.provider->resolveAccess({312}, {313}, f.replica, cpu, KisPageAccessMode::Read);
        QVERIFY(read.isValid()); f.provider->releaseAccess(std::move(read), {});
    }
    if (guard == 10) QVERIFY(ledger.reclassifyBacking(f.replica, KisBackingBudgetClass::Current));
    budgetBlocker.release();
    // Every rejection leaves reusable claims and budget; no half rekey.
    auto retry = ledger.prepareBackingHandoff(f.replica, physical.target(), &f.error);
    QVERIFY2(retry.isValid(), qPrintable(f.error)); retry.release();
    for (const auto &bucket : budget.usage().buckets) QCOMPARE(bucket.reserved.cpuRam, 0u);
    QVERIFY(ledger.reclassifyBacking(f.replica, KisBackingBudgetClass::RetirementDebt));
    QVERIFY(f.provider->retire({314}, f.replica, {}).isValid());
    ledger.releaseRetiredBacking(f.replica);
}

void KisPageStorePhysicalClaimTest::backingHandoffAccountingAcrossIndexGrowth()
{
    Fixture f; QVERIFY(f.init());
    KisBackingBudgetController budget; KisPageOwnerLedger ledger;
    QVERIFY(ledger.configure(f.completions)); ledger.attachBackingBudget(budget);
    QVERIFY(ledger.registerProvider(f.provider));
    const auto registerCurrent = [&](const KisReplicaHandle &handle) {
        KisBackingBudgetDelta d; d.buckets[size_t(KisBackingBudgetClass::Current)].cpuRam = handle.layout.byteSize;
        auto r = budget.reserve(d, &f.error);
        return ledger.registerBacking(handle, r, KisBackingBudgetClass::Current, &f.error);
    };
    QVERIFY(registerCurrent(f.replica));
    auto old = f.replica;
    QVector<KisReplicaHandle> others;
    for (int generation = 2; generation <= 3; ++generation) {
        auto physical = KisCpuBackingHandoff::prepare(f.provider, old, {key(0), {quint64(generation)}}, f.desc);
        auto prepared = ledger.prepareBackingHandoff(old, physical.target(), &f.error);
        QVERIFY2(prepared.isValid(), qPrintable(f.error));
        // Grow and rehash the same index after preparation. No saved iterator
        // or full-table shadow copy may be used during commit.
        for (int i = 0; i < 129; ++i) {
            auto operation = f.provider->requestReplica({quint64(400 + others.size())},
                {key(others.size() + 1), {1}}, f.desc, cpu.domain, KisPageAccessMode::Read, KisPagePriority::Normal);
            QVERIFY(operation.isValid()); QVERIFY(registerCurrent(operation.replica));
            others.append(operation.replica);
        }
        QVERIFY(physical.tryClaim()); auto writer = physical.commit();
        QVERIFY(writer.isValid());
        QVERIFY(ledger.commitBackingHandoff(std::move(prepared)));
        QCOMPARE(ledger.backingClass(old), KisBackingBudgetClass::Count);
        old = physical.target();
        writer.reset();
        QCOMPARE(ledger.backingClass(old), KisBackingBudgetClass::ActivePending);
    }
    others.append(old);
    quint64 retirementId = 900, retainedCache = 0;
    for (const auto &handle : std::as_const(others)) {
        QVERIFY(ledger.reclassifyBacking(handle, KisBackingBudgetClass::RetirementDebt));
        const auto terminalBytes = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
        QVERIFY(f.provider->retire({retirementId++}, handle, {}).isValid()); ledger.releaseRetiredBacking(handle);
        retainedCache = terminalBytes - retirementStorageBytes(budget);
        QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam, retainedCache);
    }
    const auto finalUsage = budget.usage();
    for (size_t i = 0; i < finalUsage.buckets.size(); ++i) {
        const auto &bucket = finalUsage.buckets[i];
        QCOMPARE(bucket.live.cpuRam, i == size_t(KisBackingBudgetClass::MetadataArena) ? retainedCache : quint64(0)); QCOMPARE(bucket.reserved.cpuRam, 0u);
    }
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
    QVERIFY(!first.commit().isValid());
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
        claimed.reset();
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
    for (int bpp : {1, 4, 8, 16}) for (int mismatch = 0; mismatch < 11; ++mismatch)
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
    }
    auto rejected = KisCpuBackingHandoff::prepare(f.provider, source, target, desc);
    QVERIFY(!rejected.isValid()); QVERIFY(!rejected.tryClaim()); QVERIFY(!rejected.commit().isValid());
    auto valid = KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc);
    QVERIFY(valid.tryClaim()); valid.reset();
    QVERIFY(f.provider->retire({95}, f.replica, {}).isValid());
    auto retired = KisCpuBackingHandoff::prepare(f.provider, f.replica, {key(0), {2}}, f.desc);
    QVERIFY(!retired.isValid());
}

QTEST_GUILESS_MAIN(KisPageStorePhysicalClaimTest)
#include "KisPageStorePhysicalClaimTest.moc"

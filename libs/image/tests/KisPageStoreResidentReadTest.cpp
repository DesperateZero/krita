/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#include "KisCpuPageReplicaProvider.h"
#include "KisCpuResidentBinding_p.h"
#include "KisPageMetadataCoordinator.h"
#include "KisPageStoreCpuAccessSession.h"
#include "KisPageStoreCpuSurfaceOps.h"
#include "KisPageStoreDiagnostics_p.h"
#include "KisPageStoreReclamation_p.h"
#include "KisTiles3PageReplicaProvider.h"
#include "tiles3/kis_tile_data.h"
#include "tiles3/kis_tile_data_store.h"
#include "tiles3/kis_tile_data_store_iterators.h"

namespace {
const KisPageAccessRequirement cpu{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
KisPageKey key(int x) { return {{1}, {x, 0}}; }

KisPageAllocationDescriptor descriptor(int bpp = 4)
{
    KisPageAllocationDescriptor d;
    d.layoutRevision = d.rowAlignment = 1;
    d.pageExtent = QSize(64, 64);
    d.validRect = QRect(0, 0, 64, 64);
    auto &f = d.format;
    f.formatId = 180 + bpp;
    f.colorModelId = "RGBA";
    f.colorDepthId = "U8";
    f.profileFingerprint = "resident-read-test";
    f.channelOrder = "test-channels";
    f.packing = "interleaved";
    f.defaultPixel = QByteArray(bpp, char(0));
    f.channelCount = f.pixelStride = bpp;
    f.pixelAlignment = 1;
    f.hasAlpha = true;
    f.alphaSemantic = KisSurfaceAlphaSemantic::Premultiplied;
    f.endianness = KisSurfaceEndianness::NativeEndian;
    f.codecVersion = 1;
    return d;
}

// Preserve the generic provider contract while deliberately not exporting a
// native record. Tests must not mistake this software fallback for swap-in.
class GenericOnlyProvider final : public KisPageReplicaProvider
{
public:
    explicit GenericOnlyProvider(QSharedPointer<KisPageReplicaProvider> value) : p(std::move(value)) {}
    QString name() const override { return p->name(); }
    KisReplicaProviderId providerId() const override { return p->providerId(); }
    KisReplicaProviderEpoch providerEpoch() const override { return p->providerEpoch(); }
    KisReplicaCapabilities capabilities() const override
    {
        auto result = p->capabilities();
        result.nativeCpuMutation = false;
        result.synchronousWriteCopy = false;
        return result;
    }
    KisReplicaOperation requestReplica(KisPageOperationId o, const KisPageVersion &v,
        const KisPageAllocationDescriptor &d, KisPageAccessDomain domain, KisPageAccessMode m, KisPagePriority pri) override
    { return p->requestReplica(o, v, d, domain, m, pri); }
    KisReplicaOperation prepareWrite(KisPageOperationId o, const KisPageVersion &v,
        const KisPageAllocationDescriptor &d, KisPageAccessDomain domain, KisPageWriteMode m, KisPagePriority pri) override
    { return p->prepareWrite(o, v, d, domain, m, pri); }
    KisReplicaOperation transfer(const KisReplicaTransferRequest &r, KisPagePriority pri) override
    { return p->transfer(r, pri); }
    KisReplicaAccess resolveAccess(KisPageLeaseId l, KisPageOperationId o, const KisReplicaHandle &r,
        KisPageAccessRequirement a, KisPageAccessMode m) override { return p->resolveAccess(l, o, r, a, m); }
    void releaseAccess(KisReplicaAccess a, const KisCompletionTicket &t) override { p->releaseAccess(std::move(a), t); }
    bool validate(const KisReplicaHandle &r, const KisPageAllocationDescriptor &d) const override { return p->validate(r, d); }
    KisReplicaOperation retire(KisPageOperationId o, const KisReplicaHandle &r, const KisCompletionTicket &t) override
    { return p->retire(o, r, t); }
    KisReplicaMemoryUsage memoryUsage() const override { return p->memoryUsage(); }
private:
    QSharedPointer<KisPageReplicaProvider> p;
};

class BlockingReadProvider final : public KisPageReplicaProvider
{
public:
    explicit BlockingReadProvider(QSharedPointer<KisPageReplicaProvider> value)
        : p(std::move(value))
    {
    }

    QString name() const override { return p->name(); }
    KisReplicaProviderId providerId() const override { return p->providerId(); }
    KisReplicaProviderEpoch providerEpoch() const override { return p->providerEpoch(); }
    KisReplicaCapabilities capabilities() const override { return p->capabilities(); }
    KisReplicaOperation requestReplica(KisPageOperationId operation,
        const KisPageVersion &version, const KisPageAllocationDescriptor &allocation,
        KisPageAccessDomain domain, KisPageAccessMode mode,
        KisPagePriority priority) override
    {
        {
            std::unique_lock lock(mutex);
            ++blockedRequests;
            changed.notify_all();
            changed.wait(lock, [this] { return released; });
        }
        return p->requestReplica(operation, version, allocation, domain, mode, priority);
    }
    KisReplicaOperation prepareWrite(KisPageOperationId operation,
        const KisPageVersion &version, const KisPageAllocationDescriptor &allocation,
        KisPageAccessDomain domain, KisPageWriteMode mode,
        KisPagePriority priority) override
    { return p->prepareWrite(operation, version, allocation, domain, mode, priority); }
    KisReplicaOperation transfer(const KisReplicaTransferRequest &request,
        KisPagePriority priority) override
    { return p->transfer(request, priority); }
    KisReplicaAccess resolveAccess(KisPageLeaseId lease, KisPageOperationId operation,
        const KisReplicaHandle &replica, KisPageAccessRequirement access,
        KisPageAccessMode mode) override
    { return p->resolveAccess(lease, operation, replica, access, mode); }
    void releaseAccess(KisReplicaAccess access,
        const KisCompletionTicket &completion) override
    { p->releaseAccess(std::move(access), completion); }
    bool validate(const KisReplicaHandle &replica,
        const KisPageAllocationDescriptor &allocation) const override
    { return p->validate(replica, allocation); }
    KisReplicaOperation retire(KisPageOperationId operation,
        const KisReplicaHandle &replica,
        const KisCompletionTicket &completion) override
    { return p->retire(operation, replica, completion); }
    KisReplicaMemoryUsage memoryUsage() const override { return p->memoryUsage(); }

    bool waitForBlockedRequests(int count)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(10), [this, count] {
            return blockedRequests >= count;
        });
    }
    int blockedRequestCount() const
    {
        std::lock_guard lock(mutex);
        return blockedRequests;
    }
    void releaseAll()
    {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }

private:
    QSharedPointer<KisPageReplicaProvider> p;
    mutable std::mutex mutex;
    std::condition_variable changed;
    int blockedRequests = 0;
    bool released = false;
};

QSharedPointer<KisPageReplicaProvider> makeProvider(bool tiles3,
    const QSharedPointer<KisCompletionRegistry> &completions, QString *error)
{
    if (tiles3) {
        auto p = QSharedPointer<KisTiles3PageReplicaProvider>::create();
        KisCpuResidentReplicaProviderConfig c;
        c.provider = {180}; c.providerEpoch = {1}; c.budgetBytes = 64 * 1024 * 1024;
        return p->configure(c, completions, error) ? p : QSharedPointer<KisPageReplicaProvider>{};
    }
    auto p = QSharedPointer<KisCpuPageReplicaProvider>::create();
    KisCpuResidentReplicaProviderConfig c;
    c.provider = {180}; c.providerEpoch = {1}; c.budgetBytes = 64 * 1024 * 1024;
    return p->configure(c, completions, error) ? p : QSharedPointer<KisPageReplicaProvider>{};
}

struct Fixture
{
    std::unique_ptr<KisPageStore> store = std::make_unique<KisPageStore>();
    QSharedPointer<KisCompletionRegistry> completions = QSharedPointer<KisCompletionRegistry>::create();
    QSharedPointer<KisPageReplicaProvider> provider;
    QString error;
    int bpp = 4;
    bool init(bool tiles3, int pixelSize = 4, bool genericOnly = false)
    {
        bpp = pixelSize;
        provider = makeProvider(tiles3, completions, &error);
        if (!provider) return false;
        if (genericOnly) provider = QSharedPointer<GenericOnlyProvider>::create(provider);
        const auto d = descriptor(bpp);
        KisSurfaceEpochState s;
        s.surface = {1}; s.format = d.format;
        s.logicalPageExtent = d.pageExtent;
        s.contentExtent = QRect(0, 0, 256 * 64, 64);
        s.layoutRevision = s.rowAlignment = s.defaultPixelRevision = s.extentRevision = 1;
        KisImageEpochSnapshot initial;
        initial.epoch = {1};
        initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
        initial.surfaces = {s};
        return store->configure(initial, completions, 4, &error) && store->registerReplicaProvider(provider) &&
            store->finalizeInitialization(&error);
    }
    bool fill(int pages, quint8 value)
    {
        const auto tx = store->beginCurrentTransaction();
        return tx.isValid() && KisPageStoreCpuSurfaceOps::fillRect(store.get(), {1}, tx,
            QRect(0, 0, pages * 64, 64), QByteArray(bpp, char(value)), &error) &&
            store->commit(tx, store->preparedPages(tx)).isValid();
    }
};

void providerRows()
{
    QTest::addColumn<bool>("tiles3");
    QTest::newRow("cpu-ram") << false;
    QTest::newRow("tiles3") << true;
}
}

class KisPageStoreResidentReadTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void guardLifetimeAndHistory_data();
    void guardLifetimeAndHistory();
    void providerAccessTransferRetire_data() { providerRows(); }
    void providerAccessTransferRetire();
    void sessionCacheAndFallback_data() { providerRows(); }
    void sessionCacheAndFallback();
    void concurrentCommitAndRead_data() { providerRows(); }
    void concurrentCommitAndRead();
    void concurrentPinAndRetire_data() { providerRows(); }
    void concurrentPinAndRetire();
    void abortKeepsNativeBeforeImage_data() { providerRows(); }
    void abortKeepsNativeBeforeImage();
    void providerRevocationKeepsExistingPin_data() { providerRows(); }
    void providerRevocationKeepsExistingPin();
    void swapMissDoesNotMaterialize();
    void unsupportedProviderKeepsGenericPath();
    void lazyBindingsFollowNativeReaders_data() { providerRows(); }
    void lazyBindingsFollowNativeReaders();
    void coldDefaultBindingAfterMaterialization_data() { providerRows(); }
    void coldDefaultBindingAfterMaterialization();
    void sharedDefaultIsExactAndBounded_data() { providerRows(); }
    void sharedDefaultIsExactAndBounded();
    void defaultMaterializationAdmissionIsBounded_data()
    {
        QTest::addColumn<int>("distinctPages");
        QTest::newRow("capacity-backpressure") << 65;
        QTest::newRow("same-page-deduplication") << 1;
    }
    void defaultMaterializationAdmissionIsBounded();
    void defaultIdentityCannotImportArbitraryPayload();
};

void KisPageStoreResidentReadTest::defaultMaterializationAdmissionIsBounded()
{
    QFETCH(int, distinctPages);
    constexpr int preparationBudget = 64;
    constexpr int requestCount = preparationBudget + 1;
    const int expectedActive = std::min(preparationBudget, distinctPages);

    auto completions = QSharedPointer<KisCompletionRegistry>::create();
    QString error;
    const auto backing = makeProvider(false, completions, &error);
    QVERIFY2(backing, qPrintable(error));
    const auto provider = QSharedPointer<BlockingReadProvider>::create(backing);

    const auto allocation = descriptor(4);
    KisSurfaceEpochState surface;
    surface.surface = {1};
    surface.format = allocation.format;
    surface.logicalPageExtent = allocation.pageExtent;
    surface.contentExtent = QRect(0, 0, requestCount * 64, 64);
    surface.layoutRevision = surface.rowAlignment =
        surface.defaultPixelRevision = surface.extentRevision = 1;
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision =
        initial.extentRevision = initial.propertyRevision = 1;
    initial.surfaces = {surface};

    KisPageStore store;
    QVERIFY(store.configure(initial, completions, 4, &error));
    QVERIFY(store.registerReplicaProvider(provider));
    QVERIFY(store.finalizeInitialization(&error));

    std::atomic<bool> valid{true};
    std::array<KisReadRequest, requestCount> requests;
    std::vector<std::thread> readers;
    readers.reserve(requestCount);
    for (int i = 0; i < requestCount; ++i) {
        readers.emplace_back([&, i] {
            auto &request = requests[size_t(i)];
            request = store.acquireRead(key(i % distinctPages), {}, cpu, KisPagePriority::Normal);
            if (!request.isValid()) {
                valid.store(false);
                return;
            }
            KisReadLease lease = store.resolve(request, request.readiness);
            if (!lease.isValid()) {
                valid.store(false);
                return;
            }
            store.release(std::move(lease));
        });
    }

    const bool filledBudget = provider->waitForBlockedRequests(expectedActive);
    bool waiterObserved = false;
    KisPageStoreSessionStats saturated;
    if (filledBudget) {
        for (int attempt = 0; attempt < 10000; ++attempt) {
            saturated = store.sessionStats();
            if (saturated.defaultPreparationWaits != 0) {
                waiterObserved = true;
                break;
            }
            std::this_thread::yield();
        }
    }
    const int blockedAtSaturation = provider->blockedRequestCount();
    const bool closeRejectedWhileActive = !store.closeSession(&error);
    provider->releaseAll();
    for (auto &reader : readers) reader.join();

    QVERIFY(filledBudget);
    QCOMPARE(blockedAtSaturation, expectedActive);
    QVERIFY(waiterObserved);
    QCOMPARE(saturated.activeDefaultPreparations, qsizetype(expectedActive));
    QCOMPARE(saturated.peakDefaultPreparations, qsizetype(expectedActive));
    QCOMPARE(saturated.defaultMaterializationRequests, quint64(expectedActive));
    QVERIFY(closeRejectedWhileActive);
    QVERIFY(valid.load());

    QSet<quint64> identities;
    for (const auto &request : requests) {
        QVERIFY(!identities.contains(request.id.value));
        identities.insert(request.id.value);
        QVERIFY(!store.resolve(request, request.readiness).isValid());
        QVERIFY(!store.cancel(request));
    }
    const auto late = store.acquireRead(key(0), {}, cpu, KisPagePriority::Normal);
    QVERIFY(late.isValid());
    QVERIFY(!identities.contains(late.id.value));
    auto forged = late;
    ++forged.version.generation.value;
    QVERIFY(!store.cancel(forged));
    QVERIFY(!store.resolve(forged, forged.readiness).isValid());
    QVERIFY(store.cancel(late));
    QVERIFY(!store.cancel(late));

    const auto drained = store.sessionStats();
    QCOMPARE(drained.activeDefaultPreparations, qsizetype(0));
    QCOMPARE(drained.peakDefaultPreparations, qsizetype(expectedActive));
    QVERIFY(drained.defaultPreparationWaits >= 1);
    QCOMPARE(drained.defaultMaterializationRequests, quint64(distinctPages));
    QVERIFY2(store.closeSession(&error), qPrintable(error));
    QCOMPARE(backing->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreResidentReadTest::defaultIdentityCannotImportArbitraryPayload()
{
    Fixture f; QVERIFY(f.init(true));
    auto initial = f.store->captureCommittedEpoch();
    const KisPageVersion semanticDefault{key(0), {1}, 1};
    KisPageStore imported;
    auto invalid = initial;
    invalid.manifest = {semanticDefault};
    QVERIFY(!imported.configure(invalid, f.completions, 4, &f.error));
    QVERIFY(f.error.contains(QStringLiteral("surface metadata")));
    QVERIFY(imported.configure(initial, f.completions, 4, &f.error));
    QVERIFY(imported.registerReplicaProvider(f.provider));
    QVERIFY(!imported.adoptInitialPageBytes(semanticDefault, descriptor(), QByteArray(64 * 64 * 4, char(0x77)), &f.error));
    const auto allocation = f.provider->requestReplica({990}, semanticDefault, descriptor(), cpu.domain,
        KisPageAccessMode::Read, KisPagePriority::Normal);
    QVERIFY(allocation.isValid());
    QVERIFY(!imported.adoptInitialPage(semanticDefault, descriptor(), allocation.replica, &f.error));
    QVERIFY(f.provider->retire({991}, allocation.replica, {}).isValid());
    QVERIFY(imported.finalizeInitialization());
    auto view = imported.captureReadView();
    auto guard = view.readResidentPage(key(0));
    QVERIFY(guard.isValid()); QCOMPARE(static_cast<const quint8 *>(guard.data())[0], quint8(0));
    guard = {}; view = {};
    QVERIFY(imported.closeSession());
    QVERIFY(f.store->closeSession());
}

void KisPageStoreResidentReadTest::lazyBindingsFollowNativeReaders()
{
    QFETCH(bool, tiles3);
    Fixture f; QVERIFY(f.init(tiles3));
    for (int i = 0; i < 8; ++i) QVERIFY(f.fill(3, quint8(0x31 + i)));
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(kisPageStoreMetadataMetrics(*f.store).cpuReadBindings, quint64(0));
    const auto request = f.store->acquireRead(key(0), {}, cpu, KisPagePriority::Normal);
    auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
    f.store->release(std::move(lease));
    QCOMPARE(kisPageStoreMetadataMetrics(*f.store).cpuReadBindings, quint64(0));
    auto old = f.store->captureReadView();
    QVERIFY(f.fill(3, 0x59));
    // Native write sessions now actually read their oldData pages. The
    // retained old view keeps those provider-specific demand bindings live:
    // tiles3 exposes each page, while the CPU provider reuses one binding.
    const quint64 oldBindings = tiles3 ? 3 : 1;
    QCOMPARE(kisPageStoreMetadataMetrics(*f.store).cpuReadBindings, oldBindings);
    auto before = old.tryReadResidentPage(key(0)); QVERIFY(before.isValid());
    QCOMPARE(static_cast<const quint8 *>(before.data())[0], quint8(0x38));
    QCOMPARE(kisPageStoreMetadataMetrics(*f.store).cpuReadBindings, qMax(oldBindings, quint64(1)));
    auto current = f.store->captureReadView();
    // Several cold readers race to install the same exact cache entry.
    std::atomic<int> ready{0}; std::atomic<bool> go{false}, correct{true};
    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) readers.emplace_back([&] {
        ready.fetch_add(1);
        while (!go.load()) std::this_thread::yield();
        for (int j = 0; j < 64; ++j) {
            auto guard = current.tryReadResidentPage(key(0));
            // Native try may legitimately see a busy local gate.
            if (guard.isValid() && static_cast<const quint8 *>(guard.data())[0] != 0x59) correct.store(false);
        }
    });
    while (ready.load() != 8) std::this_thread::yield();
    go.store(true);
    for (auto &reader : readers) reader.join();
    QVERIFY(correct.load());
    auto now = current.tryReadResidentPage(key(0)); QVERIFY(now.isValid());
    QCOMPARE(kisPageStoreMetadataMetrics(*f.store).cpuReadBindings, qMax(oldBindings, quint64(1)) + 1);
    old = {}; before = {};
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(kisPageStoreMetadataMetrics(*f.store).cpuReadBindings, quint64(1));
    now = {}; current = {};
    QVERIFY(f.store->closeSession(&f.error));
    QCOMPARE(kisPageStoreMetadataMetrics(*f.store).cpuReadBindings, quint64(0));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreResidentReadTest::coldDefaultBindingAfterMaterialization()
{
    QFETCH(bool, tiles3);
    Fixture f; QVERIFY(f.init(tiles3));
    auto view = f.store->captureReadView();
    KisCpuResidentReadStatus status;
    auto shared = view.tryReadResidentPage(key(0), &status);
    QVERIFY(shared.isValid());
    QCOMPARE(status, KisCpuResidentReadStatus::Ready);
    QCOMPARE(kisPageStoreMetadataMetrics(*f.store).cpuReadBindings, quint64(0));
    const auto request = f.store->acquireReadInView(key(0), view, cpu, KisPagePriority::Normal);
    auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
    f.store->release(std::move(lease));
    QCOMPARE(kisPageStoreMetadataMetrics(*f.store).cpuReadBindings, quint64(0));
    auto guard = view.tryReadResidentPage(key(0), &status);
    QVERIFY(guard.isValid()); QCOMPARE(status, KisCpuResidentReadStatus::Ready);
    QCOMPARE(static_cast<const quint8 *>(guard.data())[0], quint8(0));
    QCOMPARE(guard.data(), shared.data());
    QCOMPARE(kisPageStoreMetadataMetrics(*f.store).cpuReadBindings, quint64(0));
    shared = {}; guard = {}; view = {}; QVERIFY(f.store->closeSession(&f.error));
    QCOMPARE(kisPageStoreMetadataMetrics(*f.store).cpuReadBindings, quint64(0));
}

void KisPageStoreResidentReadTest::sharedDefaultIsExactAndBounded()
{
    QFETCH(bool, tiles3);
    for (int bpp : {1, 4, 8, 16}) {
        Fixture f; QVERIFY(f.init(tiles3, bpp));
        auto oldView = f.store->captureReadView();
        auto first = oldView.tryReadResidentPage(key(-9));
        auto second = oldView.tryReadResidentPage(key(100));
        QVERIFY(first.isValid()); QVERIFY(second.isValid());
        QCOMPARE(first.data(), second.data());
        QCOMPARE(first.version().key, key(-9));
        QCOMPARE(second.version().key, key(100));
        QCOMPARE(first.version().defaultPixelRevision, quint64(1));
        QCOMPARE(first.rowStride(), quint32(64 * bpp));
        QCOMPARE(first.byteSize(), quint64(64 * 64 * bpp));
        auto otherScope = f.store->captureReadView();
        auto same = otherScope.tryReadResidentPage(key(7));
        QCOMPARE(same.data(), first.data());
        same = {}; otherScope = {}; second = {}; oldView = {};
        for (int revision = 2; revision <= 70; ++revision) {
            KisSurfaceEpochState surface;
            QVERIFY(f.store->resolveSurfaceState({1}, {}, &surface));
            surface.defaultPixelRevision = quint64(revision);
            surface.format.defaultPixel = QByteArray(bpp, char(revision));
            const auto tx = f.store->beginCurrentTransaction();
            QVERIFY(f.store->stageSurfaceMetadata(tx, surface));
            QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
            auto current = f.store->captureReadView();
            auto currentDefault = current.tryReadResidentPage(key(-9));
            QVERIFY(currentDefault.isValid());
            QCOMPARE(currentDefault.version().defaultPixelRevision, quint64(revision));
            QCOMPARE(static_cast<const quint8 *>(currentDefault.data())[0], quint8(revision));
            QCOMPARE(static_cast<const quint8 *>(first.data())[0], quint8(0));
        }
        const auto stats = f.store->sessionStats();
        QCOMPARE(stats.readRequestsCreated, quint64(0));
        QCOMPARE(stats.defaultMaterializationRequests, quint64(0));
        QCOMPARE(stats.registeredPages, qsizetype(0));
        QCOMPARE(stats.cachedDefaultReadBuffers, qsizetype(64));
        QVERIFY(stats.cachedDefaultReadBytes <= 8 * 1024 * 1024);
        QCOMPARE(stats.liveDefaultReadBytes, quint64(65 * 64 * 64 * bpp));
        QCOMPARE(stats.defaultReadBuffersCreated, quint64(70));
        QCOMPARE(stats.defaultReadInitializedBytes, quint64(70 * 64 * 64 * bpp));
        QCOMPARE(stats.defaultReadCacheEvictions, quint64(6));
        QCOMPARE(stats.defaultReadCacheOversizeBypasses, quint64(0));
        QCOMPARE(stats.activeDefaultPreparations, qsizetype(0));
        QCOMPARE(stats.peakDefaultPreparations, qsizetype(0));
        QCOMPARE(stats.defaultPreparationWaits, quint64(0));
        QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
        QVERIFY(!f.store->closeSession()); // old semantic guard still protects its root
        auto survivor = std::move(first);
        QVERIFY(!first.isValid());
        QCOMPARE(static_cast<const quint8 *>(survivor.data())[0], quint8(0));
        survivor = {};
        QVERIFY(f.store->closeSession());
        QCOMPARE(f.store->sessionStats().cachedDefaultReadBytes, quint64(0));
        QCOMPARE(f.store->sessionStats().liveDefaultReadBytes, quint64(0));
    }
}

void KisPageStoreResidentReadTest::guardLifetimeAndHistory_data()
{
    QTest::addColumn<bool>("tiles3");
    QTest::addColumn<int>("bpp");
    for (bool tiles3 : {false, true}) for (int bpp : {1, 4, 8, 16}) {
        const auto name = QStringLiteral("%1-bpp%2").arg(tiles3 ? "tiles3" : "cpu").arg(bpp).toLatin1();
        QTest::newRow(name.constData()) << tiles3 << bpp;
    }
}

void KisPageStoreResidentReadTest::guardLifetimeAndHistory()
{
    static_assert(!std::is_copy_constructible_v<KisCpuReadGuard>);
    static_assert(std::is_nothrow_move_constructible_v<KisCpuReadGuard>);
    static_assert(sizeof(KisPageStore) == sizeof(void *));
    QFETCH(bool, tiles3); QFETCH(int, bpp);
    Fixture f;
    QVERIFY2(f.init(tiles3, bpp), qPrintable(f.error));
    QVERIFY(f.fill(2, 0x31));
    auto view = f.store->captureReadView();
    KisCpuResidentReadStatus status;
    auto old = view.tryReadResidentPage(key(0), &status);
    QVERIFY(old.isValid()); QCOMPARE(status, KisCpuResidentReadStatus::Ready);
    QCOMPARE(old.rowStride(), quint32(64 * bpp));
    QCOMPARE(old.byteSize(), quint64(4096 * bpp));
    KisPageVersion oldVersion;
    QVERIFY(view.resolvePageVersion(key(0), &oldVersion));
    QCOMPARE(old.version(), oldVersion);
    const auto retained = f.store->captureRetainedEpoch();
    KisPageReadView selector;
    selector.kind = KisPageReadViewKind::ExactVersion;
    selector.exactVersion = oldVersion;
    selector.epoch = retained.snapshot.epoch;
    selector.retention = retained.token;
    auto exact = f.store->captureReadView(selector);
    QVERIFY(exact.isValid());
    QVERIFY(!exact.tryReadResidentPage(key(1), &status).isValid());
    QCOMPARE(status, KisCpuResidentReadStatus::InvalidIdentity);
    QVERIFY(exact.tryReadResidentPage(key(0)).isValid());
    QVERIFY(f.store->releaseSnapshot(retained.token));
    exact = {};
    view = {};
    const auto bytes = f.provider->memoryUsage().committedBytes;
    QVERIFY(f.fill(2, 0x52));
    QCOMPARE(f.provider->memoryUsage().committedBytes, bytes * 2);
    QCOMPARE(static_cast<const quint8 *>(old.data())[0], quint8(0x31));
    QVERIFY(!f.store->closeSession(&f.error));
    auto moved = std::move(old);
    QVERIFY(!old.isValid()); QVERIFY(moved.isValid());
    old = std::move(moved);
    QVERIFY(!moved.isValid());
    old = {};
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.provider->memoryUsage().committedBytes, bytes); // unpin before GC
    auto fresh = f.store->captureReadView();
    auto survivor = fresh.tryReadResidentPage(key(0));
    fresh = {};
    const QWeakPointer<KisPageReplicaProvider> weak(f.provider);
    f.provider.clear(); f.store.reset();
    QVERIFY(!weak.isNull());
    QCOMPARE(static_cast<const quint8 *>(survivor.data())[0], quint8(0x52));
    survivor = {};
    kisDrainPageStoreReclamation();
    QVERIFY(weak.isNull());
}

void KisPageStoreResidentReadTest::providerAccessTransferRetire()
{
    QFETCH(bool, tiles3);
    auto completions = QSharedPointer<KisCompletionRegistry>::create();
    QString error;
    auto p = makeProvider(tiles3, completions, &error);
    QVERIFY2(p, qPrintable(error));
    const auto d = descriptor();
    const auto a = p->requestReplica({100}, {key(0), {1}}, d, cpu.domain, KisPageAccessMode::Read, KisPagePriority::Normal);
    const auto b = p->prepareWrite({101}, {key(0), {2}}, d, cpu.domain, KisPageWriteMode::PreserveContents, KisPagePriority::Normal);
    QVERIFY(a.isValid()); QVERIFY(b.isValid());
    auto binding = p->cpuResidentBinding(a.replica);
    auto target = p->cpuResidentBinding(b.replica);
    QVERIFY(binding); QVERIFY(target);
    KisCpuResidentReadStatus status;
    QVERIFY(binding->acquireRead(true, &status)); QCOMPARE(status, KisCpuResidentReadStatus::Ready);
    QVERIFY(binding->acquireRead(true));
    auto generic = p->resolveAccess({200}, {201}, a.replica, cpu, KisPageAccessMode::Read);
    QVERIFY(generic.isValid());
    QVERIFY(!p->resolveAccess({202}, {203}, a.replica, cpu, KisPageAccessMode::Write).isValid());
    QVERIFY(!p->retire({204}, a.replica, {}).isValid());
    QVERIFY(target->acquireRead(true));
    KisReplicaTransferRequest transfer;
    transfer.operation = {205}; transfer.source = a.replica; transfer.target = b.replica;
    transfer.descriptor = d; transfer.kind = KisReplicaTransferKind::WriteGenerationInitialization;
    QVERIFY(!p->transfer(transfer, KisPagePriority::Normal).isValid());
    target->releaseRead(); transfer.operation = {206};
    QVERIFY(p->transfer(transfer, KisPagePriority::Normal).isValid());
    binding->releaseRead();
    QVERIFY(!p->retire({207}, a.replica, {}).isValid());
    binding->releaseRead();
    QVERIFY(!p->retire({208}, a.replica, {}).isValid()); // generic reader remains
    p->releaseAccess(std::move(generic), {});
    auto writer = p->resolveAccess({209}, {210}, a.replica, cpu, KisPageAccessMode::Write);
    QVERIFY(writer.isValid());
    QVERIFY(!binding->acquireRead(true, &status)); QCOMPARE(status, KisCpuResidentReadStatus::Busy);
    p->releaseAccess(std::move(writer), {});
    auto stale = a.replica; ++stale.providerEpoch.value;
    QVERIFY(!p->cpuResidentBinding(stale, &status)); QCOMPARE(status, KisCpuResidentReadStatus::InvalidIdentity);
    const KisCompletionDomain source = KisCompletionDomain::CpuJob;
    const auto lastUse = completions->allocatePending(completions->registerSource(source));
    QVERIFY(lastUse.isValid());
    QVERIFY(!p->retire({211}, a.replica, lastUse).isValid());
    QVERIFY(completions->complete(lastUse, KisCompletionStatus::Succeeded));
    QVERIFY(p->retire({213}, a.replica, lastUse).isValid());
    QVERIFY(!binding->acquireRead(true, &status)); QCOMPARE(status, KisCpuResidentReadStatus::Retired);
    QVERIFY(p->retire({212}, b.replica, {}).isValid());
    QCOMPARE(p->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreResidentReadTest::sessionCacheAndFallback()
{
    QFETCH(bool, tiles3);
    Fixture f; QVERIFY(f.init(tiles3)); QVERIFY(f.fill(256, 0x42));
    const auto before = f.store->sessionStats();
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    KisPageStoreCpuAccessSession session;
    QVERIFY(session.beginRead(f.store.get(), {1}, {}, KisPagePriority::Normal, &f.error));
    std::vector<KisCpuPageReadSpan> spans;
    for (int i = 0; i < 256; ++i) {
        spans.push_back(session.readPage(i, 0, &f.error));
        QVERIFY2(spans.back().isValid(), qPrintable(f.error));
    }
    for (int i = 255; i >= 0; --i) {
        QCOMPARE(spans[size_t(i)].data[0], quint8(0x42));
        QCOMPARE(session.readPage(i, 0).data, spans[size_t(i)].data);
    }
    QCOMPARE(f.store->sessionStats().readRequestsCreated, before.readRequestsCreated);
    QCOMPARE(f.store->sessionStats().activeReadLeases, qsizetype(0));
    QCOMPARE(recorder.metrics()[size_t(KisPageStoreDiagnosticPhase::ManifestExport)].intervals, quint64(0));
    QVERIFY(!f.store->closeSession(&f.error)); // native root still outstanding
    auto view = f.store->captureReadView();
    KisCpuResidentReadStatus status;
    QVERIFY(view.tryReadResidentPage(key(-9), &status).isValid());
    QCOMPARE(status, KisCpuResidentReadStatus::Ready);
    const auto blank = session.readPage(-9, 0, &f.error);
    QVERIFY(blank.isValid()); QCOMPARE(blank.data[0], quint8(0));
    QCOMPARE(f.store->sessionStats().readRequestsCreated, before.readRequestsCreated);
    view = {};
    QVERIFY(session.finish(&f.error));
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreResidentReadTest::concurrentCommitAndRead()
{
    QFETCH(bool, tiles3);
    Fixture f; QVERIFY(f.init(tiles3)); QVERIFY(f.fill(2, 0x31));
    std::atomic<bool> ok{true};
    std::thread writer([&] {
        for (int i = 0; i < 64; ++i) if (!f.fill(2, quint8(0x40 + (i & 1)))) ok = false;
    });
    for (int i = 0; i < 256; ++i) {
        KisPageStoreCpuAccessSession session;
        QString error;
        if (!session.beginRead(f.store.get(), {1}, {}, KisPagePriority::Normal, &error)) { ok = false; break; }
        const auto a = session.readPage(0, 0, &error), b = session.readPage(1, 0, &error);
        if (!a.isValid() || !b.isValid() || a.data[0] != b.data[0]) ok = false;
        if (!session.finish(&error)) ok = false;
    }
    writer.join();
    QVERIFY(ok.load());
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(2 * 4096 * f.bpp));
    QVERIFY(f.store->closeSession(&f.error));
}

void KisPageStoreResidentReadTest::concurrentPinAndRetire()
{
    QFETCH(bool, tiles3);
    auto completions = QSharedPointer<KisCompletionRegistry>::create();
    QString error; auto p = makeProvider(tiles3, completions, &error); QVERIFY(p);
    const auto a = p->requestReplica({100}, {key(0), {1}}, descriptor(), cpu.domain,
                                    KisPageAccessMode::Read, KisPagePriority::Normal);
    QVERIFY(a.isValid()); auto binding = p->cpuResidentBinding(a.replica); QVERIFY(binding);
    std::atomic<bool> pinned{false}, release{false}, ok{true};
    std::thread reader([&] {
        const void *data = binding->acquireRead(true);
        if (!data) ok = false;
        pinned = true;
        while (!release.load()) std::this_thread::yield();
        if (data) {
            if (*static_cast<const quint8 *>(data) != 0) ok = false;
            binding->releaseRead();
        }
    });
    while (!pinned.load()) std::this_thread::yield();
    for (quint64 i = 200; i < 264; ++i) if (p->retire({i}, a.replica, {}).isValid()) ok = false;
    release = true; reader.join();
    QVERIFY(ok.load());
    QVERIFY(p->retire({300}, a.replica, {}).isValid());
    QVERIFY(!binding->acquireRead(true));
}

void KisPageStoreResidentReadTest::swapMissDoesNotMaterialize()
{
    Fixture f; QVERIFY(f.init(true)); QVERIFY(f.fill(1, 0x51));
    auto p = qSharedPointerDynamicCast<KisTiles3PageReplicaProvider>(f.provider); QVERIFY(p);
    auto view = f.store->captureReadView();
    const auto request = f.store->acquireReadInView(key(0), view, cpu, KisPagePriority::Normal);
    auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
    KisTileData *tile = p->tileDataForLease(lease.leaseId()); QVERIFY(tile);
    f.store->release(std::move(lease));
    auto guard = view.tryReadResidentPage(key(0)); QVERIFY(guard.isValid());
    auto *tiles = KisTileDataStore::instance();
    const auto swap = [&] {
        auto *it = tiles->beginIteration();
        const bool result = it->trySwapOut(tile);
        tiles->endIteration(it);
        return result;
    };
    QVERIFY(!swap()); // physical pin, even though generic read map is empty
    guard = {};
    QVERIFY(swap());
    const auto before = f.store->sessionStats();
    KisCpuResidentReadStatus status;
    QVERIFY(!view.tryReadResidentPage(key(0), &status).isValid());
    QCOMPARE(status, KisCpuResidentReadStatus::NonResident);
    QCOMPARE(f.store->sessionStats().readRequestsCreated, before.readRequestsCreated);
    KisPageStoreCpuAccessSession session;
    QVERIFY(session.beginRead(f.store.get(), {1}, {}, KisPagePriority::Normal, &f.error));
    auto loaded = session.readPage(0, 0, &f.error);
    QVERIFY2(loaded.isValid(), qPrintable(f.error)); QCOMPARE(loaded.data[0], quint8(0x51));
    QCOMPARE(f.store->sessionStats().readRequestsCreated, before.readRequestsCreated + 1);
    QVERIFY(session.finish(&f.error));
    guard = view.tryReadResidentPage(key(0), &status);
    QVERIFY(guard.isValid()); QCOMPARE(status, KisCpuResidentReadStatus::Ready);
    guard = {}; view = {}; QVERIFY(f.store->closeSession(&f.error));
}

void KisPageStoreResidentReadTest::abortKeepsNativeBeforeImage()
{
    QFETCH(bool, tiles3);
    Fixture f; QVERIFY(f.init(tiles3)); QVERIFY(f.fill(1, 0x32));
    auto view = f.store->captureReadView();
    auto before = view.tryReadResidentPage(key(0)); QVERIFY(before.isValid());
    const auto bytes = f.provider->memoryUsage().committedBytes;
    const auto tx = f.store->beginCurrentTransaction();
    const auto request = f.store->acquireWrite(tx, key(0), cpu,
        KisPageWriteMode::PreserveContents, KisPagePriority::Normal);
    auto writer = f.store->resolve(request, request.readiness); QVERIFY(writer.isValid());
    static_cast<quint8 *>(writer.cpuData())[0] = 0x73;
    QCOMPARE(static_cast<const quint8 *>(before.data())[0], quint8(0x32));
    QVERIFY(!f.store->abort(tx));
    f.store->cancel(std::move(writer));
    QVERIFY(f.store->abort(tx));
    QCOMPARE(static_cast<const quint8 *>(before.data())[0], quint8(0x32));
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.provider->memoryUsage().committedBytes, bytes);
    before = {}; view = {};
    QVERIFY(f.store->closeSession(&f.error));
}

void KisPageStoreResidentReadTest::providerRevocationKeepsExistingPin()
{
    QFETCH(bool, tiles3);
    auto completions = QSharedPointer<KisCompletionRegistry>::create();
    QString error; auto p = makeProvider(tiles3, completions, &error); QVERIFY(p);
    const auto a = p->requestReplica({100}, {key(0), {1}}, descriptor(), cpu.domain,
                                    KisPageAccessMode::Read, KisPagePriority::Normal);
    QVERIFY(a.isValid());
    auto binding = p->cpuResidentBinding(a.replica); QVERIFY(binding);
    const auto *data = static_cast<const quint8 *>(binding->acquireRead(true)); QVERIFY(data);
    p.clear();
    QCOMPARE(data[0], quint8(0));
    KisCpuResidentReadStatus status;
    QVERIFY(!binding->acquireRead(true, &status)); QCOMPARE(status, KisCpuResidentReadStatus::Retired);
    binding->releaseRead();
    QVERIFY(!binding->acquireRead(true));
}

void KisPageStoreResidentReadTest::unsupportedProviderKeepsGenericPath()
{
    Fixture f; QVERIFY(f.init(false, 4, true)); QVERIFY(f.fill(1, 0x71));
    KisCpuResidentReadStatus status;
    QVERIFY(!KisCapturedReadView{}.tryReadResidentPage(key(0), &status).isValid());
    QCOMPARE(status, KisCpuResidentReadStatus::InvalidView);
    auto view = f.store->captureReadView();
    QVERIFY(!view.tryReadResidentPage(key(0), &status).isValid());
    QCOMPARE(status, KisCpuResidentReadStatus::UnsupportedProvider);
    QVERIFY(!view.tryReadResidentPage({{999}, {0, 0}}, &status).isValid());
    QCOMPARE(status, KisCpuResidentReadStatus::InvalidIdentity);
    const auto before = f.store->sessionStats();
    KisPageStoreCpuAccessSession session;
    QVERIFY(session.beginRead(f.store.get(), {1}, {}, KisPagePriority::Normal, &f.error));
    const auto span = session.readPage(0, 0, &f.error);
    QVERIFY(span.isValid()); QCOMPARE(span.data[0], quint8(0x71));
    QCOMPARE(f.store->sessionStats().readRequestsCreated, before.readRequestsCreated + 1);
    QVERIFY(session.finish(&f.error)); view = {};
    QVERIFY(f.store->closeSession(&f.error));
}

QTEST_GUILESS_MAIN(KisPageStoreResidentReadTest)
#include "KisPageStoreResidentReadTest.moc"

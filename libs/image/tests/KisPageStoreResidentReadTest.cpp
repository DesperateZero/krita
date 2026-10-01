/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>
#include <QScopeGuard>
#include <QSemaphore>
#include <new>
#include <optional>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>
#include <functional>
#include <cstring>

#include "KisCpuPageReplicaProvider.h"
#include "KisCpuResidentBinding_p.h"
#include "KisPageMetadataCoordinator.h"
#include "KisPageReadCoordinator_p.h"
#include "KisPageWriteCoordinator_p.h"
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
class GenericOnlyProvider : public KisPageReplicaProvider
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
        result.synchronousSourceAdoption = false;
        result.synchronousCpuPayload = false;
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
    KisReplicaBackingFootprint backingFootprint(const KisReplicaHandle &r) const override
    { return p->backingFootprint(r); }
    QVector<KisReplicaBackingDomainChange> backingDomainChanges() const override
    { return p->backingDomainChanges(); }
    bool mayHaveBackingDomainChanges() const noexcept override
    { return p->mayHaveBackingDomainChanges(); }
    void acknowledgeBackingDomainChange(quint64 slot, quint64 revision) override
    { p->acknowledgeBackingDomainChange(slot, revision); }
    bool registerBackingDomainAdmission(
        const QSharedPointer<KisReplicaBackingDomainAdmission> &admission, QString *error) override
    { return p->registerBackingDomainAdmission(admission, error); }
protected:
    QSharedPointer<KisPageReplicaProvider> p;
};

// Exercise the real coordinator while another caller grows both indexes or
// a provider rejects before creating a pin. The backing provider is unchanged.
class ReadResolveProbeProvider final : public GenericOnlyProvider
{
public:
    using GenericOnlyProvider::GenericOnlyProvider;
    enum Behavior { Normal, ThrowBeforePin, BlockAfterPin };
    std::atomic<Behavior> next{Normal};
    QSemaphore entered, resume;
    KisReplicaAccess resolveAccess(KisPageLeaseId lease, KisPageOperationId operation,
        const KisReplicaHandle &replica, KisPageAccessRequirement access, KisPageAccessMode mode) override
    {
        const auto behavior = mode == KisPageAccessMode::Read ? next.exchange(Normal) : Normal;
        if (behavior == ThrowBeforePin) throw std::bad_alloc();
        auto result = p->resolveAccess(lease, operation, replica, access, mode);
        if (behavior == BlockAfterPin) {
            entered.release();
            resume.acquire();
        }
        return result;
    }
};

class StorageFailureBinding final : public KisCpuResidentBinding
{
public:
    explicit StorageFailureBinding(const KisReplicaHandle &handle) : KisCpuResidentBinding(handle) {}
    bool reject = true;
    int pins = 0;
    int unpins = 0;
protected:
    void *pinStorage(bool, KisCpuResidentReadStatus *) override
    {
        if (std::exchange(reject, false)) throw std::bad_alloc();
        ++pins;
        return bytes.data();
    }
    void unpinStorage() override { ++unpins; }
    void releaseStorage() override {}
private:
    std::array<quint8, 64 * 64 * 4> bytes{};
};

// Generic writes stay unchanged; native reads exercise the real captured-view
// retry loop with controlled stale/negative provider binding results.
class ReadBindingProbeProvider final : public GenericOnlyProvider
{
public:
    using GenericOnlyProvider::GenericOnlyProvider;
    mutable int bindingCalls = 0;
    std::function<QSharedPointer<KisCpuResidentBinding>(const KisReplicaHandle &,
        KisCpuResidentReadStatus *)> bindingProbe;
    QSharedPointer<KisCpuResidentBinding> cpuResidentBinding(
        const KisReplicaHandle &replica, KisCpuResidentReadStatus *status = nullptr) const override
    {
        ++bindingCalls;
        return bindingProbe ? bindingProbe(replica, status) : p->cpuResidentBinding(replica, status);
    }
    QSharedPointer<KisCpuResidentBinding> originalBinding(const KisReplicaHandle &r,
                                                         KisCpuResidentReadStatus *status = nullptr)
    { return p->cpuResidentBinding(r, status); }
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
    bool resolveProbe = false;
    bool init(bool tiles3, int pixelSize = 4, bool genericOnly = false, bool readProbe = false)
    {
        bpp = pixelSize;
        provider = makeProvider(tiles3, completions, &error);
        if (!provider) return false;
        if (resolveProbe) provider = QSharedPointer<ReadResolveProbeProvider>::create(provider);
        else if (readProbe) provider = QSharedPointer<ReadBindingProbeProvider>::create(provider);
        else if (genericOnly) provider = QSharedPointer<GenericOnlyProvider>::create(provider);
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
    void capturedReadRetriesStaleBinding_data();
    void capturedReadRetriesStaleBinding();
    void retainedVersionRediscoveryAfterRetag_data();
    void retainedVersionRediscoveryAfterRetag();
    void bindingStorageFailureUnlocks_data();
    void bindingStorageFailureUnlocks();
    void resolveStorageBeforePin_data();
    void resolveStorageBeforePin();
    void resolveSurvivesConcurrentIndexGrowth_data();
    void resolveSurvivesConcurrentIndexGrowth();
};

void KisPageStoreResidentReadTest::bindingStorageFailureUnlocks_data()
{
    QTest::addColumn<int>("path");
    QTest::newRow("generic-read") << 0;
    QTest::newRow("native-try") << 1;
    QTest::newRow("native-wait") << 2;
    QTest::newRow("generic-write") << 3;
}

void KisPageStoreResidentReadTest::bindingStorageFailureUnlocks()
{
    QFETCH(int, path);
    auto completions = QSharedPointer<KisCompletionRegistry>::create();
    QString error; auto provider = makeProvider(false, completions, &error); QVERIFY(provider);
    auto op = provider->requestReplica({900}, {key(0), {1}}, descriptor(), cpu.domain,
        KisPageAccessMode::Read, KisPagePriority::Normal); QVERIFY(op.isValid());
    auto binding = QSharedPointer<StorageFailureBinding>::create(op.replica);
    KisCpuBindingLeaseMap leases;
    const auto mode = path == 3 ? KisPageAccessMode::Write : KisPageAccessMode::Read;
    bool rejected = false;
    try {
        if (path == 1 || path == 2)
            binding->acquireRead(op.replica.allocationIdentity(), true, nullptr, path == 2);
        else
            kisAcquireCpuBindingAccess(leases, binding, {901}, {902}, op.replica, cpu, mode);
    } catch (const std::bad_alloc &) { rejected = true; }
    QVERIFY(rejected); QVERIFY(leases.empty());
    QCOMPARE(binding->pins, 0);
    // Nonblocking retry detects a leaked binding gate without hanging the test.
    QVERIFY(binding->acquireRead(op.replica.allocationIdentity(), true));
    binding->releaseRead();
    auto access = kisAcquireCpuBindingAccess(leases, binding, {901}, {902}, op.replica, cpu, mode);
    QVERIFY(access.isValid()); QCOMPARE(leases.size(), size_t(1));
    kisReleaseCpuBindingAccess(leases, binding, access); QVERIFY(leases.empty());
    QCOMPARE(binding->pins, 2); QCOMPARE(binding->unpins, 2);
    QVERIFY(binding->retire(op.replica.allocationIdentity()));
    QVERIFY(provider->retire({903}, op.replica, {}).isValid());
}

void KisPageStoreResidentReadTest::resolveStorageBeforePin_data()
{
    QTest::addColumn<bool>("tiles3");
    QTest::addColumn<int>("bpp");
    QTest::addColumn<bool>("retry");
    for (bool tiles3 : {false, true}) for (int bpp : {1, 4, 8, 16})
        for (bool retry : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-bpp%2-%3")
                .arg(tiles3 ? "tiles3" : "cpu").arg(bpp).arg(retry ? "retry" : "cancel")))
                << tiles3 << bpp << retry;
}

void KisPageStoreResidentReadTest::resolveStorageBeforePin()
{
    QFETCH(bool, tiles3); QFETCH(int, bpp); QFETCH(bool, retry);
    Fixture f; f.resolveProbe = true;
    QVERIFY(f.init(tiles3, bpp)); QVERIFY(f.fill(1, 0x58));
    const auto provider = qSharedPointerDynamicCast<ReadResolveProbeProvider>(f.provider);
    const auto request = f.store->acquireRead(key(0), {}, cpu, KisPagePriority::Normal);
    QVERIFY(request.isValid());
    provider->next = ReadResolveProbeProvider::ThrowBeforePin;
    QVERIFY(!f.store->resolve(request, request.readiness).isValid());
    QCOMPARE(f.store->sessionStats().pendingRequests, qsizetype(1));
    QCOMPARE(f.store->sessionStats().activeReadLeases, qsizetype(0));
    QVERIFY(!f.store->closeSession(&f.error));
    if (retry) {
        auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
        QCOMPARE(static_cast<const quint8 *>(lease.cpuData())[0], quint8(0x58));
        QCOMPARE(f.store->sessionStats().pendingRequests, qsizetype(0));
        QCOMPARE(f.store->sessionStats().activeReadLeases, qsizetype(1));
        f.store->release(std::move(lease));
    } else {
        QVERIFY(f.store->cancel(request));
    }
    QVERIFY(!f.store->resolve(request, request.readiness).isValid());
    QVERIFY(!f.store->cancel(request));
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    QCOMPARE(f.store->sessionStats().activeReadLeases, qsizetype(0));
    QCOMPARE(f.store->sessionStats().pendingRequests, qsizetype(0));
}

void KisPageStoreResidentReadTest::resolveSurvivesConcurrentIndexGrowth_data()
{
    QTest::addColumn<bool>("tiles3");
    QTest::addColumn<int>("bpp");
    for (bool tiles3 : {false, true}) for (int bpp : {1, 4, 8, 16})
        QTest::newRow(qPrintable(QStringLiteral("%1-bpp%2").arg(tiles3 ? "tiles3" : "cpu").arg(bpp)))
            << tiles3 << bpp;
}

void KisPageStoreResidentReadTest::resolveSurvivesConcurrentIndexGrowth()
{
    QFETCH(bool, tiles3); QFETCH(int, bpp);
    Fixture f; f.resolveProbe = true;
    QVERIFY(f.init(tiles3, bpp)); QVERIFY(f.fill(1, 0x6b));
    const auto provider = qSharedPointerDynamicCast<ReadResolveProbeProvider>(f.provider);
    const auto request = f.store->acquireRead(key(0), {}, cpu, KisPagePriority::Normal);
    QVERIFY(request.isValid());
    provider->next = ReadResolveProbeProvider::BlockAfterPin;
    std::optional<KisReadLease> target;
    std::thread reader([&] { target.emplace(f.store->resolve(request, request.readiness)); });
    const bool entered = provider->entered.tryAcquire(1, 10000);
    bool correct = entered;
    std::vector<KisReadLease> others;
    if (entered) {
        for (int i = 0; i < 257; ++i) {
            const auto other = f.store->acquireRead(key(0), {}, cpu, KisPagePriority::Normal);
            auto lease = f.store->resolve(other, other.readiness);
            correct = correct && other.isValid() && lease.isValid();
            if (lease.isValid()) others.push_back(std::move(lease));
        }
        const auto blocked = f.store->sessionStats();
        correct = correct && blocked.pendingRequests == 1 && blocked.activeReadLeases == 257;
        correct = !f.store->cancel(request) && correct;
        correct = !f.store->closeSession(&f.error) && correct;
    }
    provider->resume.release();
    reader.join();
    QVERIFY(correct); QVERIFY(target && target->isValid());
    QCOMPARE(f.store->sessionStats().pendingRequests, qsizetype(0));
    QCOMPARE(f.store->sessionStats().activeReadLeases, qsizetype(258));
    QCOMPARE(static_cast<const quint8 *>(target->cpuData())[0], quint8(0x6b));
    for (auto &lease : others) {
        QCOMPARE(static_cast<const quint8 *>(lease.cpuData())[0], quint8(0x6b));
        f.store->release(std::move(lease));
    }
    f.store->release(std::move(*target));
    QVERIFY(!f.store->resolve(request, request.readiness).isValid());
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
}

void KisPageStoreResidentReadTest::capturedReadRetriesStaleBinding_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("mode");
    QTest::addColumn<bool>("wait"); QTest::addColumn<bool>("reportStatus");
    for (int bpp : {1, 4, 8, 16}) for (int mode = 0; mode < 8; ++mode)
        for (bool wait : {false, true}) for (bool report : {false, true})
            QTest::newRow(qPrintable(QString("bpp%1-mode%2-wait%3-status%4").arg(bpp).arg(mode).arg(wait).arg(report)))
                << bpp << mode << wait << report;
}

void KisPageStoreResidentReadTest::capturedReadRetriesStaleBinding()
{
    QFETCH(int, bpp); QFETCH(int, mode); QFETCH(bool, wait); QFETCH(bool, reportStatus);
    Fixture f; QVERIFY2(f.init(true, bpp, false, true), qPrintable(f.error));
    QVERIFY2(f.fill(1, 0x31), qPrintable(f.error));
    auto provider = qSharedPointerDynamicCast<ReadBindingProbeProvider>(f.provider); QVERIFY(provider);
    auto retained = f.store->captureReadView();
    KisPageVersion oldVersion; QVERIFY(retained.resolvePageVersion(key(0), &oldVersion));
    // Freeze G, then publish another current generation. Every retry below
    // must still select G, even when its failed cached candidate is discarded.
    // The reference surface accessor also reads oldData and would warm G's
    // native cache before fault injection. Use the generic discard write
    // contract to establish the new current without reading G.
    const auto tx = f.store->beginCurrentTransaction(); QVERIFY(tx.isValid());
    const auto request = f.store->acquireWrite(tx, key(0), cpu,
        KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
    QVERIFY(request.isValid());
    auto writer = f.store->resolve(request, request.readiness); QVERIFY(writer.isValid());
    std::memset(writer.cpuData(), 0x72, size_t(writer.byteSize()));
    QVERIFY(f.store->publishHostWrite(std::move(writer)).isValid());
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    auto wrong = provider->prepareWrite({1000000}, {key(9), {1}}, descriptor(bpp), cpu.domain,
        KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
    QVERIFY(wrong.isValid());
    auto wrongBinding = provider->originalBinding(wrong.replica); QVERIFY(wrongBinding);
    int remaining = mode <= 2 ? 1 : 100;
    QSharedPointer<KisCpuResidentBinding> busyBinding;
    bool busyPinned = false;
    const auto cleanup = qScopeGuard([&] {
        provider->bindingProbe = {};
        if (busyPinned) busyBinding->releaseWrite();
    });
    provider->bindingProbe = [&](const KisReplicaHandle &replica, KisCpuResidentReadStatus *status) {
        if (remaining-- > 0) {
            if (mode == 0 || mode == 3) return wrongBinding;
            if (mode == 6) {
                busyBinding = provider->originalBinding(replica, status);
                if (!busyBinding || !busyBinding->acquireWrite(replica.allocationIdentity())) return QSharedPointer<KisCpuResidentBinding>{};
                busyPinned = true;
                return busyBinding;
            }
            if (status) *status = mode == 2 || mode == 5 ? KisCpuResidentReadStatus::Retired
                : mode == 7 ? KisCpuResidentReadStatus::BindingUnavailable : KisCpuResidentReadStatus::InvalidIdentity;
            return QSharedPointer<KisCpuResidentBinding>{};
        }
        return provider->originalBinding(replica, status);
    };
    const int before = provider->bindingCalls;
    KisCpuResidentReadStatus status{};
    auto guard = wait ? retained.readResidentPage(key(0), reportStatus ? &status : nullptr)
                      : retained.tryReadResidentPage(key(0), reportStatus ? &status : nullptr);
    if (mode <= 2) {
        QVERIFY(guard.isValid()); QCOMPARE(guard.version(), oldVersion);
        QCOMPARE(static_cast<const quint8 *>(guard.data())[0], quint8(0x31));
        QCOMPARE(provider->bindingCalls - before, 2);
        if (reportStatus) QCOMPARE(status, KisCpuResidentReadStatus::Ready);
        guard = {};
        const int warmed = provider->bindingCalls;
        for (int i = 0; i < 32; ++i) {
            auto read = retained.readResidentPage(key(0)); QVERIFY(read.isValid());
            QCOMPARE(read.version(), oldVersion);
            QCOMPARE(static_cast<const quint8 *>(read.data())[0], quint8(0x31));
        }
        QCOMPARE(provider->bindingCalls, warmed); // Warm hits have no new lookup.
    } else {
        QVERIFY(!guard.isValid());
        QCOMPARE(provider->bindingCalls - before, mode <= 5 ? 3 : 1);
        if (reportStatus) QCOMPARE(status, mode == 5 ? KisCpuResidentReadStatus::Retired
            : mode == 6 ? KisCpuResidentReadStatus::Busy
            : mode == 7 ? KisCpuResidentReadStatus::BindingUnavailable : KisCpuResidentReadStatus::InvalidIdentity);
    }
    if (busyPinned) {
        busyBinding->releaseWrite();
        busyPinned = false;
        provider->bindingProbe = {};
        auto read = retained.readResidentPage(key(0)); QVERIFY(read.isValid());
        QCOMPARE(read.version(), oldVersion);
        QCOMPARE(provider->bindingCalls - before, 1); // Busy did not evict.
    }
    provider->bindingProbe = {};
    QVERIFY(provider->retire({1000001}, wrong.replica, {}).isValid());
    retained = {};
    auto current = f.store->captureReadView(); auto now = current.readResidentPage(key(0));
    QVERIFY(now.isValid()); QVERIFY(!(now.version() == oldVersion));
    QCOMPARE(static_cast<const quint8 *>(now.data())[0], quint8(0x72));
}

void KisPageStoreResidentReadTest::retainedVersionRediscoveryAfterRetag_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("warm");
    QTest::addColumn<bool>("publish"); QTest::addColumn<bool>("crossProvider");
    for (int bpp : {1, 4, 8, 16}) for (bool warm : {false, true})
        for (bool publish : {false, true}) for (bool cross : {false, true})
            QTest::newRow(qPrintable(QString("bpp%1-warm%2-publish%3-cross%4").arg(bpp).arg(warm).arg(publish).arg(cross)))
                << bpp << warm << publish << cross;
}

void KisPageStoreResidentReadTest::retainedVersionRediscoveryAfterRetag()
{
    QFETCH(int, bpp); QFETCH(bool, warm); QFETCH(bool, publish); QFETCH(bool, crossProvider);
    auto completions = QSharedPointer<KisCompletionRegistry>::create();
    auto provider = QSharedPointer<KisTiles3PageReplicaProvider>::create();
    auto beforeProvider = crossProvider ? QSharedPointer<KisTiles3PageReplicaProvider>::create() : provider;
    QVERIFY(provider->configure({{180}, {1}, 16 * 1024 * 1024}, completions));
    if (crossProvider) QVERIFY(beforeProvider->configure({{181}, {1}, 16 * 1024 * 1024}, completions));
    auto desc = descriptor(bpp); desc.format.defaultPixel.fill(char(0x31));
    const KisPageVersion version{key(0), {1}};
    // Both independent exact replicas pre-exist the candidate. Handoff itself
    // must not allocate or copy a before payload to manufacture eligibility.
    auto a = provider->requestReplica({1}, version, desc, cpu.domain, KisPageAccessMode::Read, KisPagePriority::Normal);
    auto b = beforeProvider->requestReplica({2}, version, desc, cpu.domain, KisPageAccessMode::Read, KisPagePriority::Normal);
    QVERIFY(a.isValid()); QVERIFY(b.isValid());
    KisPageBackingLimits limits; limits.retirementDebtBytes = a.replica.layout.byteSize;
    KisBackingBudgetController budget(limits); KisPageOwnerLedger owner;
    QVERIFY(owner.configure(completions)); owner.attachBackingBudget(budget);
    QVERIFY(owner.registerProvider(provider));
    if (crossProvider) QVERIFY(owner.registerProvider(beforeProvider));
    for (const auto &replica : {a.replica, b.replica}) {
        KisBackingBudgetDelta delta; delta.buckets[size_t(KisBackingBudgetClass::Current)].cpuRam = replica.layout.byteSize;
        auto r = budget.reserve(delta, nullptr); QVERIFY(owner.registerBacking(replica, r, KisBackingBudgetClass::Current));
    }
    KisPageMetadataCoordinator metadata;
    metadata.attachRetirementDebtOwner(&owner,
        [](void *context, const QVector<KisPageTransitionEffect> &effects, quint64 *cookie, QString *error) {
            return static_cast<KisPageOwnerLedger *>(context)->prepareRetirementDebt(effects, cookie, error);
        },
        [](void *context, quint64 cookie) noexcept { static_cast<KisPageOwnerLedger *>(context)->commitRetirementDebt(cookie); },
        [](void *context, quint64 cookie) noexcept { static_cast<KisPageOwnerLedger *>(context)->cancelRetirementDebt(cookie); });
    QVERIFY(metadata.configure(4));
    KisPageStateSnapshot page; page.key = version.key; page.publishedEpoch = {1};
    page.publishedGeneration = {1}; page.nextGeneration = {2};
    KisPageVersionStateSnapshot old;
    old.version = version; old.publication = KisPagePublicationState::Published; old.authority = a.replica;
    old.replicas = {{a.replica, KisReplicaValidity::Valid, {}, {}, 0, {}},
                    {b.replica, KisReplicaValidity::Valid, {}, {}, 0, {}}};
    page.versions = {old}; QVERIFY(metadata.registerPage(page));
    auto stale = KisPageReadCoordinator::discoverCpuReadBinding(metadata, owner, version); QVERIFY(stale);
    QSharedPointer<KisCpuResidentBinding> binding;
    if (warm) { binding = stale->resolve(); QVERIFY(binding); }
    auto physical = KisCpuBackingHandoff::prepare(provider, a.replica, {key(0), {2}}, desc); QVERIFY(physical.isValid());
    const auto target = physical.target();
    const KisPageTransaction tx{{71}, {1}};
    KisPageTransition write; write.kind = KisPageTransitionKind::AcquireRecoverableWrite;
    write.baseVersion = version; write.version = target.version;
    write.source = b.replica; write.target = target; write.transaction = tx.id;
    write.operation = {72}; write.writer = {73};
    auto prepared = metadata.prepareRecoverableWrite(tx, write); QVERIFY(prepared.isValid());
    auto accounting = owner.prepareBackingHandoff(a.replica, target); QVERIFY(accounting.isValid());
    KisPageMetadataCoordinator::DeferredPublicationCleanup cleanup;
    QVERIFY(physical.tryClaim());
    QVERIFY(metadata.installRecoverableWrite(std::move(prepared), tx, nullptr, &cleanup));
    auto writer = physical.commit(); QVERIFY(writer.isValid());
    QVERIFY(owner.commitBackingHandoff(std::move(accounting)));
    while (!cleanup.isEmpty()) QVERIFY(cleanup.clearBatch(1) > 0);
    auto *bytes = static_cast<quint8 *>(writer.pinResident()); QVERIFY(bytes);
    std::memset(bytes, 0x72, size_t(target.layout.byteSize)); writer.reset();
    KisCpuResidentReadStatus observed{};
    binding = stale->resolve(&observed);
    if (binding) QVERIFY(!binding->acquireRead(a.replica.allocationIdentity(), true, &observed));
    QCOMPARE(observed, KisCpuResidentReadStatus::InvalidIdentity);
    if (publish) {
        auto finish = write; finish.kind = KisPageTransitionKind::BeginPublish;
        QVERIFY(metadata.applyOwner(version.key, finish).accepted);
        finish.kind = KisPageTransitionKind::PublishWrite; QVERIFY(metadata.applyOwner(version.key, finish).accepted);
        finish.kind = KisPageTransitionKind::CommitTransaction; finish.imageEpoch = {2};
        auto publication = metadata.preparePublication(tx, {2}, {finish});
        QVERIFY(publication.isValid());
        QVERIFY(metadata.installPublication(std::move(publication), tx, {2}, nullptr));
    } else {
        auto cancel = write; cancel.kind = KisPageTransitionKind::CancelWrite;
        QVERIFY(metadata.applyOwner(version.key, cancel).accepted);
    }
    // Production captured reads call this exact cold selection with the
    // version resolved once from their retained root, never the latest key.
    auto selected = KisPageReadCoordinator::discoverCpuReadBinding(metadata, owner, version, stale);
    QVERIFY(selected); QCOMPARE(selected->replica, b.replica);
    auto beforeBinding = selected->resolve(); QVERIFY(beforeBinding);
    const auto *before = static_cast<const quint8 *>(beforeBinding->acquireRead(b.replica.allocationIdentity(), true));
    QVERIFY(before); const bool beforeExact = before[0] == 0x31 && before[b.replica.layout.byteSize - 1] == 0x31;
    beforeBinding->releaseRead(); QVERIFY(beforeExact);
    if (publish) {
        auto current = KisPageReadCoordinator::discoverCpuReadBinding(metadata, owner, target.version);
        QVERIFY(current); QCOMPARE(current->replica, target);
        auto currentBinding = current->resolve(); QVERIFY(currentBinding);
        const auto *after = static_cast<const quint8 *>(currentBinding->acquireRead(target.allocationIdentity(), true));
        QVERIFY(after); const bool afterExact = after[0] == 0x72 && after[target.layout.byteSize - 1] == 0x72;
        currentBinding->releaseRead(); QVERIFY(afterExact);
    }
    // A late reader of A cannot evict B; even a replacement with the same B
    // handle survives eviction requested by an older B cache object.
    QCOMPARE(KisPageReadCoordinator::discoverCpuReadBinding(metadata, owner, version, stale), selected);
    metadata.removeCpuReadBinding(b.replica);
    auto newer = metadata.installCpuReadBinding(b.replica, beforeProvider); QVERIFY(newer); QVERIFY(newer != selected);
    QSharedPointer<KisCpuReadBindingLink> concurrent;
    std::thread late([&] { concurrent = KisPageReadCoordinator::discoverCpuReadBinding(metadata, owner, version, selected); });
    late.join(); QCOMPARE(concurrent, newer); QCOMPARE(metadata.cpuReadBinding(version), newer);
    QVERIFY(!KisPageReadCoordinator::discoverCpuReadBinding(metadata, owner, target.version, stale));
    QCOMPARE(metadata.cpuReadBinding(version), newer);
    QCOMPARE(metadata.metrics().fullSnapshotExports, quint64(0));
    QVERIFY(owner.reclassifyBacking(target, KisBackingBudgetClass::RetirementDebt));
    QVERIFY(provider->retire({100}, target, {}).isValid()); owner.releaseRetiredBacking(target);
    QVERIFY(owner.reclassifyBacking(b.replica, KisBackingBudgetClass::RetirementDebt));
    QVERIFY(beforeProvider->retire({101}, b.replica, {}).isValid()); owner.releaseRetiredBacking(b.replica);
    for (const auto &bucket : budget.usage().buckets) {
        QCOMPARE(bucket.live.cpuRam, 0u); QCOMPARE(bucket.reserved.cpuRam, 0u);
    }
}

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
    QVERIFY(binding->acquireRead(a.replica.allocationIdentity(), true, &status)); QCOMPARE(status, KisCpuResidentReadStatus::Ready);
    QVERIFY(binding->acquireRead(a.replica.allocationIdentity(), true));
    auto generic = p->resolveAccess({200}, {201}, a.replica, cpu, KisPageAccessMode::Read);
    QVERIFY(generic.isValid());
    QVERIFY(!p->resolveAccess({202}, {203}, a.replica, cpu, KisPageAccessMode::Write).isValid());
    QVERIFY(!p->retire({204}, a.replica, {}).isValid());
    QVERIFY(target->acquireRead(b.replica.allocationIdentity(), true));
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
    QVERIFY(!binding->acquireRead(a.replica.allocationIdentity(), true, &status)); QCOMPARE(status, KisCpuResidentReadStatus::Busy);
    p->releaseAccess(std::move(writer), {});
    auto stale = a.replica; ++stale.providerEpoch.value;
    QVERIFY(!p->cpuResidentBinding(stale, &status)); QCOMPARE(status, KisCpuResidentReadStatus::InvalidIdentity);
    const KisCompletionDomain source = KisCompletionDomain::CpuJob;
    const auto lastUse = completions->allocatePending(completions->registerSource(source));
    QVERIFY(lastUse.isValid());
    QVERIFY(!p->retire({211}, a.replica, lastUse).isValid());
    QVERIFY(completions->complete(lastUse, KisCompletionStatus::Succeeded));
    QVERIFY(p->retire({213}, a.replica, lastUse).isValid());
    QVERIFY(!binding->acquireRead(a.replica.allocationIdentity(), true, &status)); QCOMPARE(status, KisCpuResidentReadStatus::Retired);
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
        const void *data = binding->acquireRead(a.replica.allocationIdentity(), true);
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
    QVERIFY(!binding->acquireRead(a.replica.allocationIdentity(), true));
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
        return tiles->trySwapTileData(tile);
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
    const auto *data = static_cast<const quint8 *>(binding->acquireRead(a.replica.allocationIdentity(), true)); QVERIFY(data);
    p.clear();
    QCOMPARE(data[0], quint8(0));
    KisCpuResidentReadStatus status;
    QVERIFY(!binding->acquireRead(a.replica.allocationIdentity(), true, &status)); QCOMPARE(status, KisCpuResidentReadStatus::Retired);
    binding->releaseRead();
    QVERIFY(!binding->acquireRead(a.replica.allocationIdentity(), true));
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

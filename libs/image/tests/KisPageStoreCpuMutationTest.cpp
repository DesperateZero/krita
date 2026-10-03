/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <QTest>
#include <QSemaphore>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <limits>
#include <new>
#include <optional>
#include <vector>
#include "KisCpuResidentBinding_p.h"
#include "KisPageStoreDiagnostics_p.h"
#include "KisPageMetadataCoordinator.h"
#include "KisPageMetadataCoordinator_p.h"
#include "KisPageOwnerLedger.h"
#include "KisImageEpochReferenceModel.h"
#include "KisPageStoreReclamation_p.h"
#include "KisPageRetirementQueue_p.h"
#include "KisPageReadCoordinator_p.h"
#include "KisPageHistoryCollector_p.h"
#include "KisPagePublicationCoordinator_p.h"
#include "KisPageStoreCpuSurfaceOps.h"
#include "KisPageWriteCoordinator_p.h"
#include "KisTiles3PageReplicaProvider.h"
#include "KisTiledDataManagerPageStoreBackend.h"
#include "KisPageStoreIteratorReadScope_p.h"
#include "tiles3/kis_tile_data.h"
#include "tiles3/kis_tile_data_store.h"
#include "tiles3/kis_tile_data_store_iterators.h"
#include "tiles3/KisTiledExtentManager.h"
#include "tiles3/tests/kis_tile_data_store_test_access.h"

namespace {
const KisPageAccessRequirement cpu{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
KisPageKey key(int x = 0) { return {{1}, {x, 0}}; }

// Faults are injected at the provider contract, never by editing owner state.
class TestProvider final : public KisPageReplicaProvider
{
public:
    std::shared_ptr<KisTiles3PageReplicaProvider> p = std::make_shared<KisTiles3PageReplicaProvider>();
    mutable int validationsUntilFailure = -1;
    bool rejectWrite = false;
    bool returnExistingWriteReplica = false;
    bool returnExistingRequestReplica = false;
    std::atomic<bool> rejectRetire{false};
    std::atomic<bool> rejectNativeBinding{false};
    KisReplicaHandle rejectBindingFor;
    bool rejectReadAccess = false;
    std::atomic<int> readAccessCalls{0};
    int writeAccessMismatch = 0;
    KisReplicaHandle mismatchedWriteReplica;
    bool disableNativeMutation = false;
    bool disableCpuPayload = false;
    bool disableSynchronousWriteCopy = false;
    bool disableBackgroundRetirement = false;
    bool mismatchTransferOperation = false;
    std::atomic<int> retireCalls{0};
    KisCompletionTicket deferredRetirement;
    std::function<void()> beforeRetire;
    std::function<void()> beforeCapabilities;
    std::function<void(const KisReplicaHandle &, const KisReplicaOperation &)> afterRetire;
    std::function<void()> beforePrepareWrite;
    std::function<void()> beforeTransfer;
    std::function<void(const KisReplicaHandle &)> beforeBinding;
    std::function<void(const KisReplicaHandle &)> beforeWriteResolve;
    std::function<void(const KisReplicaHandle &)> afterWriteResolve;
    std::function<void()> beforeCpuPayload;
    std::function<void()> beforeRequestReplica;
    std::function<void()> beforeAdopt;
    std::function<void()> beforeRelease;
    std::function<void()> afterDomainSnapshot;
    bool forceDomainJournalProbe = false;
    mutable std::atomic<int> domainJournalCalls{0};
    std::function<void()> afterBackingFootprint;
    bool forwardDomainAdmission = true;
    bool rejectAdopt = false;
    std::function<void()> beforeValidate;
    std::function<void()> beforeDomainAdmission;
    std::atomic<int> domainAdmissionCalls{0};
    bool rejectDomainAdmission = false;
    std::shared_ptr<KisReplicaBackingDomainAdmission> *domainAdmissionCapture = nullptr;
    KisReplicaHandle lastTarget;
    QString name() const override { return p->name(); }
    KisReplicaProviderId providerId() const override { return p->providerId(); }
    KisReplicaProviderEpoch providerEpoch() const override { return p->providerEpoch(); }
    KisReplicaCapabilities capabilities() const override
    { if (beforeCapabilities) beforeCapabilities();
      auto c = p->capabilities(); if (disableNativeMutation) { c.nativeCpuMutation = false; c.synchronousSourceAdoption = false; }
      if (disableCpuPayload) c.synchronousCpuPayload = false;
      if (disableSynchronousWriteCopy) c.synchronousWriteCopy = false;
      if (disableBackgroundRetirement) c.backgroundRetirement = false;
      return c; }
    KisReplicaOperation requestReplica(KisPageOperationId o, const KisPageVersion &v,
        const KisPageAllocationDescriptor &d, KisPageAccessDomain domain, KisPageAccessMode m, KisPagePriority pri) override
    { if (beforeRequestReplica) beforeRequestReplica();
      if (returnExistingRequestReplica) {
          KisReplicaOperation reused; reused.status = KisPageRequestStatus::Ready;
          reused.operation = o; reused.replica = lastTarget; return reused;
      }
      return p->requestReplica(o, v, d, domain, m, pri); }
    KisReplicaOperation prepareWrite(KisPageOperationId o, const KisPageVersion &v,
        const KisPageAllocationDescriptor &d, KisPageAccessDomain domain, KisPageWriteMode m, KisPagePriority pri) override
    { if (beforePrepareWrite) beforePrepareWrite(); if (rejectWrite) return {};
      if (returnExistingWriteReplica) {
          KisReplicaOperation reused; reused.status = KisPageRequestStatus::Ready;
          reused.operation = o; reused.replica = lastTarget; return reused;
      }
      auto r = p->prepareWrite(o, v, d, domain, m, pri); lastTarget = r.replica; return r; }
    KisReplicaOperation prepareSynchronousWriteCopy(KisPageOperationId o, const KisReplicaHandle &s,
        const KisPageVersion &v, const KisPageAllocationDescriptor &d, KisPagePriority pri) override
    { if (rejectWrite) return {}; auto r = p->prepareSynchronousWriteCopy(o, s, v, d, pri); lastTarget = r.replica; return r; }
    KisReplicaOperation prepareSynchronousCpuPayload(KisPageOperationId o, const KisPageVersion &v,
        const KisPageAllocationDescriptor &d, const KisCpuPagePayload &payload, KisPagePriority pri) override
    { if (beforeCpuPayload) beforeCpuPayload(); if (rejectWrite) return {};
      auto r = p->prepareSynchronousCpuPayload(o,v,d,payload,pri); lastTarget = r.replica; return r; }
    KisReplicaOperation prepareSynchronousSource(KisPageOperationId o,
        const std::shared_ptr<const KisPageReplicaSource> &s, const KisPageVersion &v,
        const KisPageAllocationDescriptor &d, KisReplicaSourceUse use, KisPagePriority pri) override
    {
        if (beforeAdopt) beforeAdopt(); if (rejectAdopt) return {};
        auto r = p->prepareSynchronousSource(o, s, v, d, use, pri); lastTarget = r.replica; return r;
    }
    bool copySynchronousSourceToCpu(const std::shared_ptr<const KisPageReplicaSource> &s,
        const KisPageAllocationDescriptor &d, void *target, quint32 stride, quint64 bytes) override
    { return p->copySynchronousSourceToCpu(s, d, target, stride, bytes); }
    std::shared_ptr<KisCpuResidentBinding> cpuResidentBinding(const KisReplicaHandle &r,
        KisCpuResidentReadStatus *status = nullptr) const override
    { if (beforeBinding) beforeBinding(r);
      return rejectNativeBinding || r == rejectBindingFor ? std::shared_ptr<KisCpuResidentBinding>{} : p->cpuResidentBinding(r, status); }
    KisReplicaOperation transfer(const KisReplicaTransferRequest &r, KisPagePriority pri) override
    { if (beforeTransfer) beforeTransfer(); auto result = p->transfer(r, pri);
      if (mismatchTransferOperation) ++result.operation.value; return result; }
    KisReplicaAccess resolveAccess(KisPageLeaseId l, KisPageOperationId o, const KisReplicaHandle &r,
        KisPageAccessRequirement a, KisPageAccessMode m) override
    {
        if (rejectReadAccess && m == KisPageAccessMode::Read) return {};
        if (m == KisPageAccessMode::Write && beforeWriteResolve) beforeWriteResolve(r);
        auto resolvedReplica = r;
        if (m == KisPageAccessMode::Write) {
            if (writeAccessMismatch == 1) m = KisPageAccessMode::Read;
            if (writeAccessMismatch == 2) resolvedReplica = mismatchedWriteReplica;
            if (writeAccessMismatch == 3) l.value += 100000;
            if (writeAccessMismatch == 4) o.value += 100000;
        }
        if (m == KisPageAccessMode::Read) ++readAccessCalls;
        auto access = p->resolveAccess(l, o, resolvedReplica, a, m);
        if (access.isValid() && m == KisPageAccessMode::Write && afterWriteResolve) afterWriteResolve(r);
        return access;
    }
    void releaseAccess(KisReplicaAccess a, const KisCompletionTicket &t) override
    { if (beforeRelease) beforeRelease(); p->releaseAccess(std::move(a), t); }
    bool validate(const KisReplicaHandle &r, const KisPageAllocationDescriptor &d) const override
    {
        if (beforeValidate) beforeValidate();
        if (validationsUntilFailure == 0) return false;
        if (validationsUntilFailure > 0) --validationsUntilFailure;
        return p->validate(r, d);
    }
    KisReplicaOperation retire(KisPageOperationId o, const KisReplicaHandle &r, const KisCompletionTicket &t) override
    {
        ++retireCalls; if (beforeRetire) beforeRetire();
        auto result = rejectRetire ? KisReplicaOperation{} : p->retire(o, r, t);
        // Delay acknowledgement, not physical safety: the wrapped provider
        // still checks pins/last-use before accepting retirement.
        if (result.isValid() && deferredRetirement.isValid()) {
            result.completion = deferredRetirement;
            result.status = KisPageRequestStatus::Pending;
        }
        if (afterRetire) afterRetire(r, result);
        return result;
    }
    KisReplicaMemoryUsage memoryUsage() const override { return p->memoryUsage(); }
    KisReplicaBackingFootprint backingFootprint(
        const KisReplicaHandle &r) const override
    {
        auto result = p->backingFootprint(r);
        if (afterBackingFootprint) afterBackingFootprint();
        return result;
    }
    KisReplicaBackingDomainChanges backingDomainChanges(KisBackingBudgetController *budget = nullptr) const override
    {
        ++domainJournalCalls;
        auto result = p->backingDomainChanges(budget);
        if (afterDomainSnapshot) afterDomainSnapshot();
        return result;
    }
    bool mayHaveBackingDomainChanges() const noexcept override
    {
        return forceDomainJournalProbe || p->mayHaveBackingDomainChanges();
    }
    void acknowledgeBackingDomainChange(quint64 slot, quint64 revision) override
    { p->acknowledgeBackingDomainChange(slot, revision); }
    bool registerBackingDomainAdmission(
        const std::shared_ptr<KisReplicaBackingDomainAdmission> &admission,
        QString *error) override
    {
        ++domainAdmissionCalls;
        if (domainAdmissionCapture) *domainAdmissionCapture = admission;
        if (beforeDomainAdmission) beforeDomainAdmission();
        if (rejectDomainAdmission) {
            KisPageStoreDetail::setError(error, QStringLiteral("test provider refused domain admission"));
            return false;
        }
        if (forwardDomainAdmission)
            return p->registerBackingDomainAdmission(admission, error);
        KisPageStoreDetail::setError(error, {});
        return bool(admission);
    }
};

struct Fixture
{
    std::unique_ptr<KisPageStore> store = std::make_unique<KisPageStore>();
    std::shared_ptr<KisCompletionRegistry> completions = std::make_shared<KisCompletionRegistry>();
    std::shared_ptr<TestProvider> provider = std::make_shared<TestProvider>();
    std::shared_ptr<KisBackingBudgetController> providerProcessBudget;
    QString error;
    int bpp = 4;
    QVector<KisReplicaHandle> initialReplicas;
    KisPageAllocationDescriptor importedDescriptor;
    bool init(int pixelSize = 4, int importedReplicas = 0, bool deferAdoption = false)
    {
        bpp = pixelSize;
        KisCpuResidentReplicaProviderConfig config;
        config.provider = {190}; config.providerEpoch = {1}; config.budgetBytes = 64 * 1024 * 1024;
        if (!provider->p->configure(config, completions, &error, providerProcessBudget)) return false;
        KisSurfaceEpochState s;
        s.surface = {1}; s.logicalPageExtent = QSize(64, 64);
        s.contentExtent = QRect(0, 0, 256, 64);
        s.layoutRevision = s.rowAlignment = s.defaultPixelRevision = s.extentRevision = 1;
        auto &f = s.format;
        f.formatId = 190 + bpp; f.colorModelId = "RGBA"; f.colorDepthId = "U8";
        f.profileFingerprint = "cpu-mutation-test"; f.channelOrder = "test-channels";
        f.packing = "interleaved"; f.defaultPixel = QByteArray(bpp, char(0x2a));
        f.channelCount = f.pixelStride = bpp; f.pixelAlignment = 1; f.hasAlpha = true;
        f.alphaSemantic = KisSurfaceAlphaSemantic::Premultiplied;
        f.endianness = KisSurfaceEndianness::NativeEndian; f.codecVersion = 1;
        KisImageEpochSnapshot initial; initial.epoch = {1};
        initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
        initial.surfaces = {s};
        const KisPageVersion imported{key(), {1}};
        if (importedReplicas) initial.manifest = {imported};
        if (!store->configure(initial, completions, 4, &error) || !store->registerReplicaProvider(provider)) return false;
        if (importedReplicas) {
            KisPageOwnerLedger identities;
            if (!identities.configure(completions, &error)) return false;
            const auto descriptor = s.allocationDescriptor();
            importedDescriptor = descriptor;
            for (int i = 0; i < importedReplicas; ++i) {
                const auto ready = provider->p->prepareWrite(identities.nextOperationId(), imported,
                    descriptor, cpu.domain, KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
                if (!ready.isValid() || ready.status != KisPageRequestStatus::Ready) return false;
                const auto binding = provider->p->cpuResidentBinding(ready.replica);
                void *bytes = binding ? binding->acquireWrite(ready.replica.allocationIdentity()) : nullptr;
                if (!bytes) return false;
                std::memset(bytes, 0x31, size_t(ready.replica.layout.byteSize));
                binding->releaseWrite();
                initialReplicas.append(ready.replica);
            }
            if (deferAdoption) return true;
            if (!store->adoptInitialPage(imported, descriptor, initialReplicas.first(), initialReplicas.mid(1), &error))
                return false;
        }
        return store->finalizeInitialization(&error);
    }
    bool fill(quint8 value, bool drain = true)
    {
        const auto tx = store->beginCurrentTransaction();
        return KisPageStoreCpuSurfaceOps::fillRect(store.get(), {1}, tx, QRect(0, 0, 128, 64),
            QByteArray(bpp, char(value)), &error) && store->commit(tx, store->preparedPages(tx)).isValid() &&
            (!drain || store->waitForRetirementIdle());
    }
    bool setDefault(quint8 value)
    {
        KisSurfaceEpochState surface;
        if (!store->resolveSurfaceState({1}, {}, &surface)) return false;
        surface.format.defaultPixel = QByteArray(bpp, char(value));
        ++surface.defaultPixelRevision;
        const auto tx = store->beginCurrentTransaction();
        return store->stageSurfaceMetadata(tx, surface, &error) &&
            store->commit(tx, store->preparedPages(tx)).isValid();
    }
    KisPageMutationSession begin(const KisPageTransaction &tx)
    { return store->beginMutation(tx, &error); }
    bool remove(const KisPageTransaction &tx, const KisPageKey &page)
    { return KisPageStoreCpuSurfaceOps::removePage(store.get(), tx, page, &error); }
    QByteArray pixel(const KisPageReadView &view = {}, int x = 0, int offset = 0)
    {
        const auto request = store->acquireRead(key(x), view, cpu, KisPagePriority::Normal);
        auto lease = store->resolve(request, request.readiness);
        if (!lease.isValid()) return {};
        QByteArray result(static_cast<const char *>(lease.cpuData()) + offset, bpp);
        store->release(std::move(lease)); return result;
    }
};
// Compose the original owners against a real imported provider replica. The
// test owns this budget from construction; Store initialization limits remain
// frozen. Capacity is occupied/released through the actual storage allocator.
struct ReadTerminalFixture
{
    Fixture physical;
    QMutex mutex;
    QAtomicInt references{1};
    qsizetype activeCalls = 0;
    bool operational = true;
    bool closing = false;
    bool background = true;
    explicit ReadTerminalFixture(quint64 metadataBytes = 64 * 1024,
                                 quint64 retirementBytes = 64 * 64 * 4)
        : limits([metadataBytes, retirementBytes] {
        KisPageBackingLimits result;
        result.metadataArenaBytes = metadataBytes;
        result.retirementDebtBytes = retirementBytes;
        return result;
    }()) {}
    KisPageBackingLimits limits;
    KisBackingBudgetController budget{limits};
    KisPageOwnerLedger owner;
    KisPageMetadataCoordinator metadata;
    KisImageEpochReferenceModel epochs;
    static void releaseLifetime(void *value) { static_cast<QAtomicInt *>(value)->deref(); }
    KisPageRetirementQueue retirement{owner, metadata, budget, references, &references, releaseLifetime};
    KisPageHistoryCollector history{metadata, epochs, retirement, mutex, references,
        operational, closing, background, &references, releaseLifetime,
        +[](void *, const KisPageVersion &) {}};
    KisPageReadCoordinator read{metadata, epochs, owner, budget, history, retirement,
        physical.completions, mutex, activeCalls, operational, background,
        references, &references, releaseLifetime};
    KisCompletionTicket ready;
    KisReplicaHandle replica;
    KisPageRetirementRecord *originalRetirementRecord = nullptr;
    QString error;
    std::function<void()> afterDebtCommit;

    bool init(KisPageCapturedRelease *capture = nullptr, bool historical = false)
    {
        if (!physical.init(4, historical ? 0 : 1) || !owner.configure(physical.completions, &error)
            || !owner.registerProvider(physical.provider)) return false;
        owner.attachBackingBudget(budget);
        if (!metadata.configure(1, &error)) return false;
        KisSurfaceEpochState surface;
        if (!physical.store->resolveSurfaceState({1}, {}, &surface)) return false;
        KisPageBackingPreparation backing;
        if (historical) {
            KisPageWriteCoordinator coordinator(metadata, epochs, budget, owner);
            backing = coordinator.reserveBacking(surface.allocationDescriptor(), cpu.domain,
                KisBackingBudgetClass::RetainedHistory, &error);
            if (!backing.reservation.isValid()) return false;
            const auto result = physical.provider->prepareWrite(owner.nextOperationId(), {key(), {1}},
                surface.allocationDescriptor(), cpu.domain, KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
            if (!result.isValid()) return false;
            replica = result.replica;
        } else {
            replica = physical.initialReplicas.first();
        }
        KisImageEpochSnapshot initial; initial.epoch = {1};
        initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
        initial.surfaces = {surface};
        if (!historical) initial.manifest = {replica.version};
        if (!epochs.initialize(initial, &error)) return false;
        if (capture) {
            capture->versions.push_back(replica.version);
            capture->token = epochs.retainSnapshot(initial.epoch).token;
            if (!capture->token.isValid()) return false;
            capture->retained = 1;
            capture->capturedScope = true;
        }
        KisReplicaStateSnapshot resident; resident.replica = replica;
        resident.validity = KisReplicaValidity::Valid;
        KisPageVersionStateSnapshot version; version.version = replica.version;
        version.publication = KisPagePublicationState::Published;
        version.authority = replica; version.replicas = {resident};
        if (capture) version.capturedReadViews = {capture->token};
        KisPageStateSnapshot page; page.key = key(); page.publishedEpoch = {1};
        page.publishedGeneration = replica.version.generation;
        page.publishedDefaultPixelRevision = replica.version.defaultPixelRevision;
        page.nextGeneration = {2}; page.versions = {version};
        if (historical) {
            page.versions[0].publication = KisPagePublicationState::Historical;
            KisPageVersionStateSnapshot current;
            current.version = {key(), {2}, surface.defaultPixelRevision};
            current.publication = KisPagePublicationState::Published;
            page.versions.prepend(current);
            page.publishedGeneration = {2};
            page.nextGeneration = {3};
            page.publishedDefaultPixelRevision = surface.defaultPixelRevision;
            originalRetirementRecord = backing.retirement.get();
            if (!owner.registerBacking(replica, backing.reservation,
                KisBackingBudgetClass::RetainedHistory, &error, &backing.retirement)) return false;
        }
        if (!metadata.registerPage(page, &error)) return false;
        retirement.prepareTask(); history.prepareTask(budget); read.prepareTask();
        ready = physical.completions->allocatePending(
            physical.completions->registerSource(KisCompletionDomain::HostLogical));
        return ready.isValid() && physical.completions->complete(ready, KisCompletionStatus::Succeeded);
    }
    ~ReadTerminalFixture()
    {
        read.stopAutomaticWakeups(); read.waitForIdle();
        retirement.stopAutomaticWakeups(); retirement.waitForIdle();
        history.stopAutomaticWakeups();
        QMutexLocker lock(&mutex); operational = false; history.waitForIdleLocked();
        Q_ASSERT(references.loadAcquire() == 1);
    }
    KisReadRequest acquire()
    {
        KisPageReadCleanup cleanup(read); QMutexLocker lock(&mutex);
        if (!cleanup.prepareRequestLocked(lock, &error)) return {};
        return read.registerRequestLocked(replica.version, replica, cpu, ready, {}, cleanup);
    }
    KisReadLease resolve(const KisReadRequest &request)
    {
        KisPageReadCleanup cleanup(read); QMutexLocker lock(&mutex);
        return read.resolveLocked(request, request.readiness, lock, cleanup);
    }
    void release(KisReadLease lease, const KisCompletionTicket &ticket = {})
    {
        KisPageReadCleanup cleanup(read); QMutexLocker lock(&mutex);
        read.releaseLocked(std::move(lease), ticket, lock, cleanup);
    }
    bool cancel(const KisReadRequest &request)
    {
        KisPageReadCleanup cleanup(read); QMutexLocker lock(&mutex);
        return read.cancelLocked(request, lock, cleanup);
    }
    KisPageReadCoordinatorSnapshot snapshot()
    { QMutexLocker lock(&mutex); return read.snapshotLocked(); }
    quint64 live() const
    { return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; }
};
void pixelRows()
{
    QTest::addColumn<int>("bpp");
    for (int bpp : {1, 4, 8, 16}) QTest::newRow(qPrintable(QString::number(bpp))) << bpp;
}
}

class KisPageStoreCpuMutationTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void epochNativeConfigurationResumes_data()
    {
        QTest::addColumn<int>("kind");
        for (int i = 0; i < 3; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void epochNativeConfigurationResumes();
    void publicationDescriptorStorageAtCapacity();
    void overlayStorageAtCapacity_data()
    {
        QTest::addColumn<int>("refusal");
        QTest::newRow("candidate-refusal") << 0;
        QTest::newRow("surface-refusal") << 1;
        QTest::newRow("prepared-install-and-history") << 2;
    }
    void overlayStorageAtCapacity();
    void initialReplicaImportFailure_data();
    void initialReplicaImportFailure();
    void productionRecoverableHandoff_data();
    void productionRecoverableHandoff();
    void productionHandoffFallback_data();
    void productionHandoffFallback();
    void productionHandoffConcurrentConsumers_data();
    void productionHandoffConcurrentConsumers();
    void supersededOverlayUsesHistoricalLifetime_data();
    void supersededOverlayUsesHistoricalLifetime();
    void consumedGenericLeaseCanRetryAbort_data()
    {
        QTest::addColumn<int>("terminal");
        QTest::newRow("cancel") << 0;
        QTest::newRow("invalid-completion") << 1;
        QTest::newRow("proof-rejected-after-publish") << 2;
    }
    void consumedGenericLeaseCanRetryAbort();
    void partialCancellationRetiresBeforeRetry_data()
    {
        QTest::addColumn<int>("route");
        QTest::addColumn<int>("bpp");
        QTest::addColumn<int>("pageCount");
        for (int bpp : {1, 4, 8, 16}) {
            for (int count : {2, 17, 65})
                QTest::newRow(qPrintable(QStringLiteral("native-b%1-pages%2").arg(bpp).arg(count))) << 0 << bpp << count;
            QTest::newRow(qPrintable(QStringLiteral("generic-b%1").arg(bpp))) << 1 << bpp << 2;
            QTest::newRow(qPrintable(QStringLiteral("sealed-b%1").arg(bpp))) << 2 << bpp << 2;
        }
    }
    void partialCancellationRetiresBeforeRetry();
    void partialPreparationRollbackPumpsForegroundRetirement();
    void backingDomainRejectsStaleSnapshot();
    void cleanDomainJournalSkipsSnapshots();
    void dirtyDomainJournalRetainsUnregistered_data()
    {
        QTest::addColumn<int>("bpp");
        for (int bpp : {1, 4, 8, 16}) QTest::newRow(qPrintable(QString::number(bpp))) << bpp;
    }
    void dirtyDomainJournalRetainsUnregistered();
    void domainProviderSnapshotSurvivesRegistration();
    void ledgerProviderRegistrationPreparedBeforeAdmission();
    void domainJournalSynchronizationRejectsAtCapacity_data()
    {
        QTest::addColumn<bool>("snapshotFits");
        QTest::newRow("snapshot-refused") << false;
        QTest::newRow("probe-refused") << true;
    }
    void domainJournalSynchronizationRejectsAtCapacity();
    void domainClaimPreparationRejectsBeforePhysicalMove();
    void domainJournalCommitsAtCapacity();
    void metadataArenaBudgetTracksAllocator();
    void storeRootStorageRefusal_data()
    {
        QTest::addColumn<bool>("firstAllocation");
        QTest::newRow("first-allocation") << true;
        QTest::newRow("candidate-one-byte-short") << false;
    }
    void storeRootStorageRefusal();
    void storeRootStorageRetained_data()
    {
        QTest::addColumn<bool>("captured");
        QTest::newRow("cold-facade") << false;
        QTest::newRow("late-captured-root") << true;
    }
    void storeRootStorageRetained();
    void domainAdmissionStorageRetained_data()
    {
        QTest::addColumn<bool>("keepReservation");
        QTest::newRow("last-strong-admission") << false;
        QTest::newRow("late-reservation") << true;
    }
    void domainAdmissionStorageRetained();
    void domainReservationControlRetained();
    void finiteBackingLimitMatrix();
    void swapInFailurePreservesDomainState_data()
    {
        QTest::addColumn<int>("failurePoint");
        QTest::newRow("mapping")
            << int(KisSwapInFailurePoint::Mapping);
        QTest::newRow("allocation")
            << int(KisSwapInFailurePoint::Allocation);
        QTest::newRow("decompression")
            << int(KisSwapInFailurePoint::Decompression);
    }
    void swapInFailurePreservesDomainState();
    void retiredEpochAdmissionAndPhysicalReaders_data()
    {
        QTest::addColumn<int>("bpp"); QTest::addColumn<int>("protection");
        for (int b : {1, 4, 8, 16}) for (int p = 0; p < 6; ++p)
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-protection%2").arg(b).arg(p))) << b << p;
    }
    void retiredEpochAdmissionAndPhysicalReaders();
    void historyReachabilityIgnoresRetiredRootBacklog_data()
    {
        QTest::addColumn<int>("bpp"); QTest::addColumn<int>("history"); QTest::addColumn<bool>("keepOldest");
        for (int b : {1, 4, 8, 16}) for (int h : {65, 257}) for (bool k : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-history%2-keep%3").arg(b).arg(h).arg(k))) << b << h << k;
    }
    void historyReachabilityIgnoresRetiredRootBacklog();
    void longHistoryUsesVersionBudget_data()
    {
        QTest::addColumn<int>("bpp"); QTest::addColumn<int>("history");
        for (int bpp : {1, 4, 8, 16}) for (int history : {65, 257})
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-history%2").arg(bpp).arg(history))) << bpp << history;
    }
    void longHistoryUsesVersionBudget();
    void multiPageHistorySharesRootBudget_data() { pixelRows(); }
    void multiPageHistorySharesRootBudget();
    void historyRootSliceRevalidatesChangedKey_data()
    {
        QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("changedKey");
        for (int bpp : {1, 4, 8, 16}) for (bool changed : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-changed%2").arg(bpp).arg(changed))) << bpp << changed;
    }
    void historyRootSliceRevalidatesChangedKey();
    void historySweepRevalidatesAndRevisits_data() { pixelRows(); }
    void historySweepRevalidatesAndRevisits();
    void nativeMetadataWorkIgnoresRetainedHistory_data()
    {
        QTest::addColumn<int>("bpp"); QTest::addColumn<int>("history");
        for (int bpp : {1, 4, 8, 16}) for (int h : {0, 7, 63})
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-history%2").arg(bpp).arg(h))) << bpp << h;
    }
    void nativeMetadataWorkIgnoresRetainedHistory();
    void publicationMetadataWorkIgnoresRetainedHistory_data() { nativeMetadataWorkIgnoresRetainedHistory_data(); }
    void publicationMetadataWorkIgnoresRetainedHistory();
    void restorationDefaultIsCandidateOnly_data()
    {
        QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("rejectFirst");
        for (int bpp : {1, 4, 8, 16}) for (bool reject : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-reject%2").arg(bpp).arg(reject))) << bpp << reject;
    }
    void restorationDefaultIsCandidateOnly();
    void repeatedGuardAndSegment_data() { pixelRows(); }
    void repeatedGuardAndSegment();
    void mutationStorageBudgetRejection_data() { pixelRows(); }
    void mutationStorageBudgetRejection();
    void executionWarmDuringColdPreparation_data() { pixelRows(); }
    void executionWarmDuringColdPreparation();
    void executionLocalCountersAndCancellation_data() { pixelRows(); }
    void executionLocalCountersAndCancellation();
    void executionBorrowWorkers_data() { pixelRows(); }
    void executionBorrowWorkers();
    void executionBorrowExclusionAndFailure_data() { pixelRows(); }
    void executionBorrowExclusionAndFailure();
    void executionBorrowStorageRejection_data() { pixelRows(); }
    void executionBorrowStorageRejection();
    void executionBorrowLateGuard_data() { pixelRows(); }
    void executionBorrowLateGuard();
    void mutationScopeAdmissionBudgetRejection_data() { pixelRows(); }
    void mutationScopeAdmissionBudgetRejection();
    void abandonedMutationsRetainOriginalTerminalLinks_data() { pixelRows(); }
    void abandonedMutationsRetainOriginalTerminalLinks();
    void mutationStorageGrowthKeepsGuards_data() { pixelRows(); }
    void mutationStorageGrowthKeepsGuards();
    void mutationColdHolesAreReusedDuringSeal_data() { pixelRows(); }
    void mutationColdHolesAreReusedDuringSeal();
    void cancelAndClaims();
    void facadeLifetime();
    void defaultAndRemoval_data() { pixelRows(); }
    void defaultAndRemoval();
    void metadataPresenceUsesCanonicalSelector_data() { pixelRows(); }
    void metadataPresenceUsesCanonicalSelector();
    void checkpointRecyclesStorageAndSemanticPages_data() { pixelRows(); }
    void checkpointRecyclesStorageAndSemanticPages();
    void checkpointKeepsOriginalScope_data()
    {
        QTest::addColumn<int>("bpp"); QTest::addColumn<int>("repeats"); QTest::addColumn<bool>("abort");
        for (int bpp : {1, 4, 8, 16}) for (int repeats : {1, 10, 1000}) for (bool abort : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-repeats%2-abort%3").arg(bpp).arg(repeats).arg(abort))) << bpp << repeats << abort;
    }
    void checkpointKeepsOriginalScope();
    void checkpointRejectsLiveBorrow_data()
    {
        QTest::addColumn<int>("bpp"); QTest::addColumn<int>("borrow");
        for (int bpp : {1, 4, 8, 16}) for (int borrow : {0, 1, 2})
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-borrow%2").arg(bpp).arg(borrow))) << bpp << borrow;
    }
    void checkpointRejectsLiveBorrow();
    void checkpointFailurePreservesFrozenViews_data() { pixelRows(); }
    void checkpointFailurePreservesFrozenViews();
    void checkpointCaptureRefusalKeepsAcceptedSegment();
    void checkpointDoesNotWaitForColdPreparation_data() { pixelRows(); }
    void checkpointDoesNotWaitForColdPreparation();
    void failedSealKeepsEarlierSegment();
    void failedSealKeepsEarlierSegment_data()
    {
        QTest::addColumn<int>("successfulProofs");
        QTest::newRow("first-proof") << 0;
        QTest::newRow("second-proof") << 1;
    }
    void allocationFailureCancelsSegment();
    void rejectedGenericReplicaCannotRetireAnotherPage();
    void threadConfinement();
    void abandonedCompatibilityBatch();
    void concurrentBatchAbandonment();
    void nestedCpuBatchDoesNotAbortOuter();
    void failedProductionBatchCancelsUnpublished_data()
    {
        QTest::addColumn<bool>("history");
        QTest::newRow("owned") << false; QTest::newRow("history") << true;
    }
    void failedProductionBatchCancelsUnpublished();
    void invalidAliasCancelsSemanticBatch_data()
    {
        QTest::addColumn<bool>("history"); QTest::addColumn<bool>("explicitFinish");
        for (bool history : {false, true}) for (bool finish : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("history%1-finish%2").arg(history).arg(finish))) << history << finish;
    }
    void invalidAliasCancelsSemanticBatch();
    void semanticOnlyMutation_data() { pixelRows(); }
    void semanticOnlyMutation();
    void aliasMutation_data();
    void aliasMutation();
    void aliasBatchFailure_data()
    {
        QTest::addColumn<int>("bpp"); QTest::addColumn<int>("failure");
        for (int bpp : {1, 4, 8, 16}) for (int failure = 0; failure < 3; ++failure)
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-failure%2").arg(bpp).arg(failure))) << bpp << failure;
    }
    void aliasBatchFailure();
    void immutableAliasSource_data() { pixelRows(); }
    void immutableAliasSource();
    void mixedSemanticAtomicity_data();
    void mixedSemanticAtomicity();
    void concurrentMixedSemanticCapture();
    void semanticRewriteReusesPending_data();
    void semanticRewriteReusesPending();
    void virtualDefaultOnlyAllocatesTarget_data()
    {
        QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("generic");
        for (int bpp : {1, 4, 8, 16}) for (bool generic : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-generic%2").arg(bpp).arg(generic))) << bpp << generic;
    }
    void virtualDefaultOnlyAllocatesTarget();
    void rejectedRetirementsPreserveDebtAndBudget();
    void retirementRecordRefusedBeforePhysicalResult();
    void ledgerBackingNodesRefusedBeforePhysicalResult_data()
    {
        QTest::addColumn<bool>("physicalNode");
        QTest::newRow("backing-node") << false;
        QTest::newRow("physical-node") << true;
    }
    void ledgerBackingNodesRefusedBeforePhysicalResult();
    void ledgerBackingRegistrationExcludesDomainMovement();
    void retirementRecordTransfersAtCapacity_data()
    {
        QTest::addColumn<bool>("orphan");
        QTest::addColumn<bool>("automatic");
        QTest::addColumn<bool>("waitForBudget");
        QTest::newRow("registered") << false << false << false;
        QTest::newRow("rejected-unregistered") << true << false << false;
        QTest::newRow("registered-automatic") << false << true << false;
        QTest::newRow("rejected-unregistered-automatic") << true << true << false;
        QTest::newRow("registered-automatic-budget-release") << false << true << true;
    }
    void retirementRecordTransfersAtCapacity();
    void historyRefusalPreservesOriginalWork_data()
    {
        QTest::addColumn<int>("refusal");
        QTest::newRow("work-storage") << 0;
        QTest::newRow("detach-debt") << 1;
        QTest::newRow("effect-transfer-at-capacity") << 2;
        QTest::newRow("effect-preparation-storage") << 3;
        QTest::newRow("ledger-preparation-storage") << 4;
    }
    void historyRefusalPreservesOriginalWork();
    void historyDetachRequiresRetirementOwner_data()
    {
        QTest::addColumn<int>("missing");
        QTest::newRow("debt-owner") << 0;
        QTest::newRow("registered-backing") << 1;
        QTest::newRow("original-retirement-record") << 2;
    }
    void historyDetachRequiresRetirementOwner();
    void genericEffectsTransferAtCapacity_data()
    {
        QTest::addColumn<bool>("background");
        QTest::addColumn<bool>("receiver");
        QTest::newRow("foreground") << false << true;
        QTest::newRow("background") << true << true;
        QTest::newRow("missing-receiver") << false << false;
    }
    void genericEffectsTransferAtCapacity();
    void retirementPreparationRollsBackAtCapacity();
    void synchronousProviderResultsAtCapacity();
    void providerCompletionPreparationRejectsBeforePayload_data()
    {
        QTest::addColumn<bool>("prepareFirst");
        QTest::newRow("operation-node-refused") << false;
        QTest::newRow("retirement-node-refused") << true;
    }
    void providerCompletionPreparationRejectsBeforePayload();
    void providerRetirementUsesPreparedCompletion();
    void providerAdoptionPreparationRejectsBeforePayload_data()
    {
        QTest::addColumn<int>("node");
        QTest::newRow("allocation-node") << 0;
        QTest::newRow("payload-node") << 1;
        QTest::newRow("slot-node") << 2;
        QTest::newRow("binding-control") << 3;
    }
    void providerAdoptionPreparationRejectsBeforePayload();
    void providerLeasePreparationPreservesOriginalPin();
    void providerMemoryUsageWaitsForOriginalTransition();
    void residencyObserverRegistrationRejectsRecreatedIdentity();
    void writeTransferRejectsMismatchedOperation();
    void historyPreparationRevalidatesProtection();
    void retirementResultBindsAtCapacity_data()
    {
        QTest::addColumn<bool>("close");
        QTest::newRow("automatic") << false;
        QTest::newRow("close") << true;
    }
    void retirementResultBindsAtCapacity();
    void budgetWaitersReserveInOrder();
    void budgetWaiterStateStorageIsChargedAndReleased();
    void budgetWaiterCancellationAndLifetime();
    void budgetWaiterQueuedCancellation_data()
    {
        QTest::addColumn<bool>("granted");
        QTest::newRow("cancel-before-grant") << false;
        QTest::newRow("cancel-after-notification-taken") << true;
    }
    void budgetWaiterQueuedCancellation();
    void budgetWaiterSignedDurableTransfer();
    void budgetWaiterCommitReleasesUnusedCapacity();
    void budgetReservationRecyclingPreservesHeldGrants();
    void preparedReclamationWakeCancellation();
    void terminalReclamationStorageAtCapacity();
    void retirementBudgetWaitersProgress_data()
    {
        QTest::addColumn<int>("bpp"); QTest::addColumn<int>("pages"); QTest::addColumn<bool>("closeWaiting");
        for (int bpp : {1, 4, 8, 16}) for (int pages : {20, 40}) for (bool close : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-pages%2-close%3").arg(bpp).arg(pages).arg(close)))
                << bpp << pages << close;
    }
    void retirementBudgetWaitersProgress();
    void reclamationDeadlinesCancelAndDoNotOccupyWorker();
    void autonomousRetirementRetries_data() { pixelRows(); }
    void autonomousRetirementRetries();
    void noSignalPinRetirementRetries();
    void retirementRetryCloseAndLifetime_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("close-refused-resumes") << 0;
        QTest::newRow("close-takes-debt") << 1;
        QTest::newRow("destroy-dormant-deadline") << 2;
        QTest::newRow("destroy-with-active-and-queued-retry") << 3;
    }
    void retirementRetryCloseAndLifetime();
    void closeRetriesDetachedReplicas();
    void closePollsDetachedRetirement_data()
    {
        QTest::addColumn<bool>("background");
        QTest::newRow("shutdown") << false;
        QTest::newRow("background") << true;
    }
    void closePollsDetachedRetirement();
    void physicalPinDelaysDetachedRetirement_data()
    {
        QTest::addColumn<bool>("generic");
        QTest::newRow("native-pin") << false;
        QTest::newRow("provider-lease") << true;
    }
    void physicalPinDelaysDetachedRetirement();
    void retirementReleaseBeforeRegistration();
    void completionWakesDetachedRetirement_data() { pixelRows(); }
    void completionWakesDetachedRetirement();
    void retirementWakeCloseAndOwnerLifetime_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("close-rejected-rearms") << 0;
        QTest::newRow("destroy-dormant-waiter") << 1;
        QTest::newRow("destroy-queued-wakeup") << 2;
    }
    void retirementWakeCloseAndOwnerLifetime();
    void nativeSessionDoesNotMaterializeDefaultBeforeImage();
    void busyIsNotAMaterializationFallback();
    void defaultMaterializedDuringWritePreparation_data()
    {
        QTest::addColumn<bool>("generic");
        QTest::newRow("native") << false; QTest::newRow("generic") << true;
    }
    void defaultMaterializedDuringWritePreparation();
    void staleFirstWriteKeepsCurrentDefault_data()
    {
        QTest::addColumn<int>("route");
        QTest::newRow("native") << 0; QTest::newRow("generic") << 1; QTest::newRow("alias") << 2;
    }
    void staleFirstWriteKeepsCurrentDefault();
    void rejectedAllocationRetirementIsNotLost_data()
    {
        QTest::addColumn<bool>("delayed");
        QTest::newRow("terminal") << false;
        QTest::newRow("pending-operation") << true;
    }
    void rejectedAllocationRetirementIsNotLost();
    void compatibilityFinalUnlockCanTransferThreads();
    void discardDoesNotCopySource();
    void completePayload_data();
    void completePayload();
    void invalidCompletePayload_data();
    void invalidCompletePayload();
    void completePayloadPreparationClaims();
    void completePayloadProviderFailure_data();
    void completePayloadProviderFailure();
    void accessSessionUsesOneNativeSegment();
    void cursorWriteLifetime_data();
    void cursorWriteLifetime();
    void cursorReadRetainsViewNotPins_data();
    void cursorReadRetainsViewNotPins();
    void accessorFailedMoveDropsCachedPointer();
    void surfaceRectPageWorkingSet_data();
    void surfaceRectPageWorkingSet();
    void commitValidatesOriginalDelta();
    void publicationInputRefusalKeepsOriginalTransaction_data()
    {
        QTest::addColumn<bool>("late");
        QTest::addColumn<bool>("advance");
        QTest::newRow("descriptor-reference-array") << false << false;
        QTest::newRow("backing-array-after-candidates") << true << false;
        QTest::newRow("continue-after-refusal") << true << true;
    }
    void publicationInputRefusalKeepsOriginalTransaction();
    void restorationUsesOriginalTargets_data()
    {
        QTest::addColumn<bool>("delta");
        QTest::newRow("full-root") << false;
        QTest::newRow("duplicate-delta") << true;
    }
    void restorationUsesOriginalTargets();
    void commitPreparationClaimsAndLateCapture();
    void publicationCleanupUsesBoundedBackgroundPasses();
    void hostLogicalCompletionIsPreparedOnce();
    void commitRevalidatesReadLeaseRevision();
    void publicationWhileCapturedReadersChange();
    void commitRebasesAfterDisjointPublication();
    void commitRejectsConflictingPublicationAtomically();
    void genericWriteLifetimeBlocksCommit();
    void genericResolveStorageFailure_data();
    void genericResolveStorageFailure();
    void genericResolveConcurrentIndexGrowth_data();
    void genericResolveConcurrentIndexGrowth();
    void genericPreparationStorageFailure_data();
    void genericPreparationStorageFailure();
    void genericInitializationFailureRetainsAbortDebt_data() { pixelRows(); }
    void genericInitializationFailureRetainsAbortDebt();
    void genericResolveRejectsMismatchedAccess_data();
    void genericResolveRejectsMismatchedAccess();
    void genericResolveValidatesRequestIdentity_data() { pixelRows(); }
    void genericResolveValidatesRequestIdentity();
    void genericPreparationProtectsTransactionBeforeRequest();
    void genericRemovalRemainsAbsentUntilSeal_data();
    void genericRemovalRemainsAbsentUntilSeal();
    void aliasRemovalRemainsAbsentUntilSeal_data();
    void aliasRemovalRemainsAbsentUntilSeal();
    void indexedWriteClaimsIgnoreUnrelatedRequests_data();
    void indexedWriteClaimsIgnoreUnrelatedRequests();
    void concurrentWritePreparationClaims_data();
    void concurrentWritePreparationClaims();
    void guardReleaseParksPendingBacking_data();
    void guardReleaseParksPendingBacking();
    void executionPreparationReusesParkedBacking_data() { pixelRows(); }
    void executionPreparationReusesParkedBacking();
    void historyCollectionReleasesReadBindingCache_data() { pixelRows(); }
    void historyCollectionReleasesReadBindingCache();
    void pinFootprintTracksGuards_data();
    void pinFootprintTracksGuards();
    void mutablePreparationDoesNotAliasSource_data();
    void mutablePreparationDoesNotAliasSource();
    void failedCommitPreparationReleasesClaim();
    void restoreProtectsSourceAndRevalidatesLease();
    void restoreRejectsNewTransaction();
    void restoreRejectsChangedHead();
    void restoreIgnoresDetachedRetirement_data();
    void restoreIgnoresDetachedRetirement();
    void concurrentDisjointCommitsKeepEveryPage();
    void defaultPublicationRevalidatesNewlyRegisteredPage();
    void defaultPublicationRevalidatesDescriptorStorage();
    void restorationRevalidatesNewlyRegisteredPage();
    void firstRetirementRunsOffThreadAndCloseDrains();
    void blockedRetirementDoesNotBlockPublication();
    void retainedHistoryReclaimsInBackground();
    void lastUseDefersBackgroundHistory();
    void automaticLastUse_data();
    void automaticLastUse();
    void lastUseBoundedBurst_data();
    void lastUseBoundedBurst();
    void lastUseCloseAndLifetime_data();
    void lastUseCloseAndLifetime();
    void lastUseManualCompatibility();
    void lastUseAtMetadataCapacity_data();
    void lastUseAtMetadataCapacity();
    void capturedReleaseKeepsOriginalFacts_data()
    {
        QTest::addColumn<int>("exit");
        QTest::newRow("automatic") << 0;
        QTest::newRow("cancel-close") << 1;
        QTest::newRow("stop-and-drain") << 2;
    }
    void capturedReleaseKeepsOriginalFacts();
    void captureStorageRefusalPrecedesProtection();
    void readStorageRefusalPreservesOriginalRequest_data()
    {
        QTest::addColumn<bool>("resolve");
        QTest::newRow("request-before-protection") << false;
        QTest::newRow("lease-before-physical-pin") << true;
    }
    void readStorageRefusalPreservesOriginalRequest();
    void readProtectionBlockCleanup_data();
    void readProtectionBlockCleanup();
    void oldTransactionBaseReleaseReclaimsDeferredHistory();
    void capturedOverlayFreezesSealedDelta_data()
    {
        QTest::addColumn<int>("terminal");
        QTest::newRow("commit") << 0;
        QTest::newRow("abort") << 1;
        QTest::newRow("removal") << 2;
    }
    void capturedOverlayFreezesSealedDelta();
    void capturedOverlayDefaultIdentitySurvivesAbortAndRestore();
    void capturedOverlayOutlivesFacade();
    void capturedCutStorageRefusalAndLifetime();
    void capturedOverlayRevalidatesCommitPreparation();
    void capturedOverlayGenericSessionKeepsExactVersion();
    void sharedReadFailureReleasesOnlyExpiredPins_data()
    {
        QTest::addColumn<bool>("cursor");
        QTest::newRow("operation") << false; QTest::newRow("cursor") << true;
    }
    void sharedReadFailureReleasesOnlyExpiredPins();
    void capturedOverlayConcurrentSegments();
    void capturedOverlayDoesNotReviveUnsealedRemoval_data()
    {
        QTest::addColumn<bool>("seal");
        QTest::newRow("seal") << true;
        QTest::newRow("cancel") << false;
    }
    void capturedOverlayDoesNotReviveUnsealedRemoval();
    void conditionalRemovalRejectsChangedOverlay();
    void sealPreparationClaimsAndSibling_data()
    {
        QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("fail");
        for (int bpp : {1, 4, 8, 16}) for (bool fail : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-fail%2").arg(bpp).arg(fail))) << bpp << fail;
    }
    void sealPreparationClaimsAndSibling();
    void overlayStorageSurvivesSiblingCandidates_data()
    {
        QTest::addColumn<int>("bpp"); QTest::addColumn<int>("pages"); QTest::addColumn<int>("mode");
        for (int bpp : {1, 4, 8, 16}) for (int pages : {1, 9, 65}) for (int mode = 0; mode < 4; ++mode)
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-K%2-mode%3").arg(bpp).arg(pages).arg(mode)))
                << bpp << pages << mode;
    }
    void overlayStorageSurvivesSiblingCandidates();
    void overlayStorageIsNotASemanticDelta_data()
    {
        QTest::addColumn<int>("bpp"); QTest::addColumn<int>("mode");
        for (int bpp : {1, 4, 8, 16}) for (int mode = 0; mode < 3; ++mode)
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-mode%2").arg(bpp).arg(mode))) << bpp << mode;
    }
    void overlayStorageIsNotASemanticDelta();
    void sealPreservesReaderChanges_data()
    {
        QTest::addColumn<int>("bpp"); QTest::addColumn<int>("reader"); QTest::addColumn<bool>("removal");
        for (int bpp : {1, 4, 8, 16}) for (int reader = 0; reader < 4; ++reader) for (bool removal : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-reader%2-removal%3").arg(bpp).arg(reader).arg(removal)))
                << bpp << reader << removal;
    }
    void sealPreservesReaderChanges();
    void blockedSealValidationAllowsParallelWork();
    void processStorageRejectsAndRetains();
    // Permanent application stop is deliberately the final method.
    void runtimeStopsAtCapacity();
};

void KisPageStoreCpuMutationTest::epochNativeConfigurationResumes()
{
    QFETCH(int, kind);
    kisDrainPageStoreReclamation();
    KisPageBackingLimits limits; limits.metadataArenaBytes = 2 * 1024 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    KisPageBackingLimits registryLimits; registryLimits.metadataArenaBytes = 64 * 1024;
    auto registryBudget = std::make_shared<KisBackingBudgetController>(registryLimits);
    auto completions = std::make_shared<KisCompletionRegistry>(registryBudget);
    const auto registryLive = [&] { return registryBudget->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    quint64 sourceCost = 0;
    if (kind == 2) {
        const auto before = registryLive();
        QCOMPARE(completions->registerSource(KisCompletionDomain::HostLogical), quint64(1));
        sourceCost = registryLive() - before;
        QVERIFY(sourceCost > 0);
    }
    KisPageStore store;
    QString error;
    QVERIFY(store.configureSharedNonPayloadBudget(parent, &error));
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    KisSurfaceEpochState surface;
    surface.surface = {1}; surface.logicalPageExtent = QSize(64, 64);
    surface.layoutRevision = surface.rowAlignment = surface.defaultPixelRevision = surface.extentRevision = 1;
    auto &format = surface.format;
    format.formatId = 191; format.colorModelId = "RGBA"; format.colorDepthId = "U8";
    format.profileFingerprint = "epoch-config-test"; format.channelOrder = "test-channels";
    format.packing = "interleaved"; format.defaultPixel = QByteArray(4, char(0x2a));
    format.channelCount = format.pixelStride = 4; format.pixelAlignment = 1; format.hasAlpha = true;
    format.alphaSemantic = KisSurfaceAlphaSemantic::Premultiplied;
    format.endianness = KisSurfaceEndianness::NativeEndian; format.codecVersion = 1;
    initial.surfaces = {surface};
    auto *pressure = kind == 0 ? parent.get() : registryBudget.get();
    const quint64 maximum = kind == 0 ? limits.metadataArenaBytes : registryLimits.metadataArenaBytes;
    const quint64 used = pressure->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    const size_t bytes = size_t(maximum - used - (kind == 0 ? 8192 : sourceCost));
    void *filler = kisAllocateMutationStorage(pressure, bytes, 1);
    const auto free = qScopeGuard([&] { if (filler) kisFreeMutationStorage(pressure, filler, bytes, 1); });
    QVERIFY(!store.configure(initial, completions, 4, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!store.sessionStats().configured);
    QCOMPARE(store.sessionStats().immutableRoots, qsizetype(0));
    if (kind == 2) {
        QVERIFY(completions->sourceStatistics(2).knownSource);
        QCOMPARE(completions->sourceStatistics(2).allocatedTickets, quint64(0));
        auto foreign = std::make_shared<KisCompletionRegistry>();
        QVERIFY(!store.configure(initial, foreign, 4, &error));
    }
    kisFreeMutationStorage(pressure, filler, bytes, 1); filler = nullptr;
    kisDrainPageStoreReclamation();
    QVERIFY2(store.configure(initial, completions, 4, &error), qPrintable(error));
    QVERIFY(store.sessionStats().configured);
    QCOMPARE(store.sessionStats().immutableRoots, qsizetype(1));
    QVERIFY2(store.finalizeInitialization(&error), qPrintable(error));
    if (kind == 2) {
        QCOMPARE(completions->sourceStatistics(2).allocatedTickets, quint64(1));
        QCOMPARE(completions->sourceStatistics(2).terminalTickets, quint64(1));
        QVERIFY(!completions->sourceStatistics(3).knownSource);
    }
}

void KisPageStoreCpuMutationTest::sealPreparationClaimsAndSibling()
{
    QFETCH(int, bpp); QFETCH(bool, fail);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    auto prior = f.begin(tx);
    for (int x = 0; x < 3; ++x) {
        auto w = prior.beginWrite(key(x)); QVERIFY(w.isValid());
        memset(w.data(), 0x40 + x, size_t(w.byteSize()));
    }
    QVERIFY(prior.seal());
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay; overlay.transaction = tx.id;
    auto old = f.store->captureReadView(overlay); auto input = old.readResidentPage(key());
    KisSurfaceEpochState state; QVERIFY(old.resolveSurfaceState({1}, &state));
    auto source = f.provider->p->captureCpuReadSource(input, state.allocationDescriptor()); QVERIFY(source);
    auto segment = f.begin(tx); QVERIFY(segment.removePage(key()));
    auto w = segment.beginWrite(key(1)); QVERIFY(w.isValid());
    static_cast<quint8 *>(w.data())[0] = 0x71; w = {};
    QVERIFY(segment.aliasPage(key(2), source));
    bool observed = false, claimsHeld = false, siblingSealed = false, oldComplete = false, cleanupUnlocked = false;
    f.provider->beforeValidate = [&] {
        if (observed) return;
        observed = true;
        auto view = f.store->captureReadView(overlay);
        oldComplete = view.isValid();
        for (int x = 0; x < 3; ++x) {
            auto read = view.readResidentPage(key(x));
            oldComplete = oldComplete && read.isValid() && static_cast<const quint8 *>(read.data())[0] == 0x40 + x;
        }
        auto contender = f.begin(tx);
        const auto generic = f.store->acquireWrite(tx, key(), cpu, KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
        claimsHeld = contender.isActive() && !contender.beginWrite(key()).isValid() && contender.cancel() &&
            !generic.isValid() && !f.remove(tx, key(1)) &&
            !f.store->stageSurfaceMetadata(tx, state) && !f.store->abort(tx) &&
            !f.store->commit(tx, f.store->preparedPages(tx)).isValid() && !f.store->closeSession();
        // A sibling changes the same overlay map while the first seal is in
        // provider validation. Installing the first must merge, not overwrite.
        auto sibling = f.begin(tx); auto other = sibling.beginWrite(key(3));
        if (other.isValid()) { static_cast<quint8 *>(other.data())[0] = 0x73; other = {}; siblingSealed = sibling.seal(); }
        if (fail) f.provider->validationsUntilFailure = 0;
    };
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::MutationSealCleanup || !observed || !siblingSealed) return;
        // Cleanup is after complete overlay installation but before releasing
        // transaction/key claims, and must not retain the owner gate.
        auto view = f.store->captureReadView(overlay);
        auto read = view.readResidentPage(key(1));
        cleanupUnlocked = read.isValid() && static_cast<const quint8 *>(read.data())[0] == 0x71;
    });
    QCOMPARE(segment.seal(&f.error), !fail);
    f.provider->beforeValidate = {}; f.provider->validationsUntilFailure = -1;
    QVERIFY(observed); QVERIFY(claimsHeld); QVERIFY(siblingSealed); QVERIFY(oldComplete);
    QCOMPARE(cleanupUnlocked, !fail);
    QVERIFY(!segment.isActive());
    QCOMPARE(quint8(f.pixel(overlay, 0)[0]), quint8(fail ? 0x40 : 0x2a));
    QCOMPARE(quint8(f.pixel(overlay, 1)[0]), quint8(fail ? 0x41 : 0x71));
    QCOMPARE(quint8(f.pixel(overlay, 2)[0]), quint8(fail ? 0x42 : 0x40));
    QCOMPARE(quint8(f.pixel(overlay, 3)[0]), quint8(0x73));
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    for (int x = 0; x < 3; ++x) {
        auto read = old.readResidentPage(key(x)); QVERIFY(read.isValid());
        QCOMPARE(static_cast<const quint8 *>(read.data())[0], quint8(0x40 + x));
    }
    input = {}; old = {}; source.reset(); QVERIFY(f.store->closeSession());
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::overlayStorageSurvivesSiblingCandidates()
{
    QFETCH(int, bpp); QFETCH(int, pages); QFETCH(int, mode);
    Fixture f; QVERIFY(f.init(bpp));
    const auto fill = [&](const KisPageTransaction &tx, int first, int count, quint8 value) {
        auto segment = f.begin(tx);
        for (int x = first; x < first + count; ++x) {
            auto w = segment.beginWrite(key(x));
            if (!w.isValid()) return false;
            std::memset(w.data(), value, size_t(w.byteSize()));
        }
        return segment.seal(&f.error);
    };
    auto base = f.store->beginCurrentTransaction();
    QVERIFY(fill(base, 0, pages, 0x31));
    QVERIFY(f.store->commit(base, f.store->preparedPages(base)).isValid());
    const auto tx = f.store->beginCurrentTransaction();
    const auto overlay = KisPageReadView::transactionOverlay(tx.id);
    if (mode == 1 || mode == 2) QVERIFY(fill(tx, 0, pages, 0x41));
    if (mode == 3) {
        auto removal = f.begin(tx);
        for (int x = 0; x < pages; ++x) QVERIFY(removal.removePage(key(x)));
        QVERIFY(removal.seal());
    }
    auto old = f.store->captureReadView(overlay); QVERIFY(old.isValid());
    auto segment = f.begin(tx);
    for (int x = 0; x < pages; ++x) {
        if (mode == 2) QVERIFY(segment.removePage(key(x)));
        else {
            auto w = segment.beginWrite(key(x)); QVERIFY(w.isValid());
            std::memset(w.data(), 0x71, size_t(w.byteSize()));
        }
    }
    bool observed = false, siblingSealed = false;
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::MutationSealMetadataPrepare || observed) return;
        observed = true;
        // This runs after the outer candidate owns its real nodes/buckets.
        // Force the same authoritative transaction maps to grow meanwhile.
        siblingSealed = fill(tx, pages, pages + 33, 0x53);
    });
    QVERIFY2(segment.seal(&f.error), qPrintable(f.error));
    recorder.setPhaseObserver({});
    QVERIFY(observed); QVERIFY(siblingSealed);
    const quint8 previous = mode == 3 ? 0x2a : mode == 0 ? 0x31 : 0x41;
    for (int x = 0; x < pages; ++x) {
        QCOMPARE(quint8(f.pixel(overlay, x)[0]), quint8(mode == 2 ? 0x2a : 0x71));
        auto read = old.readResidentPage(key(x)); QVERIFY(read.isValid());
        QCOMPARE(static_cast<const quint8 *>(read.data())[0], previous);
    }
    for (int x = pages; x < pages * 2 + 33; ++x)
        QCOMPARE(quint8(f.pixel(overlay, x)[0]), quint8(0x53));
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    old = {}; QVERIFY(f.store->closeSession());
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::overlayStorageIsNotASemanticDelta()
{
    QFETCH(int, bpp); QFETCH(int, mode);
    Fixture f; QVERIFY(f.init(bpp));
    const auto tx = f.store->beginCurrentTransaction();
    const auto overlay = KisPageReadView::transactionOverlay(tx.id);
    KisCapturedReadView old;
    if (mode == 2) {
        auto segment = f.begin(tx); auto write = segment.beginWrite(key()); QVERIFY(write.isValid());
        std::memset(write.data(), 0x71, size_t(write.byteSize())); write = {};
        QVERIFY(segment.seal()); old = f.store->captureReadView(overlay); QVERIFY(old.isValid());
    }
    if (mode == 1) {
        KisPageVersion observed; QVERIFY(f.store->resolvePageVersion(key(), overlay, &observed));
        QVERIFY(f.store->stagePageRemovalIfUnchanged(tx, observed, &f.error));
    } else {
        auto segment = f.begin(tx); QVERIFY(segment.removePage(key())); QVERIFY(segment.seal());
    }
    const auto delta = f.store->preparedPages(tx);
    QVERIFY(delta.proofs.isEmpty()); QVERIFY(delta.removedPages.isEmpty()); QVERIFY(delta.surfaceChanges.isEmpty());
    // A storage record must not turn the formerly absent delta into a commit.
    QVERIFY(!f.store->commit(tx, delta).isValid());
    QCOMPARE(f.pixel(overlay), QByteArray(bpp, char(0x2a)));
    QVERIFY(f.store->abort(tx));
    if (old.isValid()) {
        auto read = old.readResidentPage(key()); QVERIFY(read.isValid());
        QCOMPARE(static_cast<const quint8 *>(read.data())[0], quint8(0x71));
    }
    old = {}; QVERIFY(f.store->closeSession());
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::sealPreservesReaderChanges()
{
    QFETCH(int, bpp); QFETCH(int, reader); QFETCH(bool, removal);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    auto prior = f.begin(tx);
    for (int x : {0, 1}) { auto w = prior.beginWrite(key(x)); QVERIFY(w.isValid()); static_cast<quint8 *>(w.data())[0] = 0x41; }
    QVERIFY(prior.seal());
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay; overlay.transaction = tx.id;
    auto old = f.store->captureReadView(overlay);
    KisCapturedReadView late;
    std::unique_ptr<KisReadLease> lease;
    const auto acquire = [&] {
        const auto request = f.store->acquireRead(key(1), overlay, cpu, KisPagePriority::Normal);
        lease = std::make_unique<KisReadLease>(f.store->resolve(request, request.readiness));
    };
    if (reader == 2) { acquire(); QVERIFY(lease->isValid()); }
    auto segment = f.begin(tx);
    if (removal) QVERIFY(segment.removePage(key()));
    else { auto w = segment.beginWrite(key()); QVERIFY(w.isValid()); static_cast<quint8 *>(w.data())[0] = 0x71; }
    auto w = segment.beginWrite(key(1)); QVERIFY(w.isValid()); static_cast<quint8 *>(w.data())[0] = 0x71; w = {};
    const auto before = f.store->mutationStatistics();
    int attempts = 0, validations = 0; bool proofPhase = false, prepareUnlocked = false;
    f.provider->beforeValidate = [&] { if (proofPhase) ++validations; };
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase == KisPageStoreDiagnosticPhase::MutationSealProofPrepare) proofPhase = true;
        if (phase == KisPageStoreDiagnosticPhase::MutationSealMetadataPrepare) {
            proofPhase = false; prepareUnlocked = f.store->isOperational();
        }
        if (phase != KisPageStoreDiagnosticPhase::MutationSealPublishOwnerWait) return;
        if (++attempts != 1) return;
        if (reader == 0) late = f.store->captureReadView(overlay);
        else if (reader == 1) acquire();
        else if (reader == 2) { f.store->release(std::move(*lease)); lease.reset(); }
        else {
            for (int i = 0; i < 8; ++i) { auto transient = f.store->captureReadView(overlay); QVERIFY(transient.isValid()); }
            late = f.store->captureReadView(overlay);
        }
    });
    const bool success = segment.seal(&f.error);
    f.provider->beforeValidate = {};
    QVERIFY2(success, qPrintable(f.error)); QVERIFY(prepareUnlocked);
    QCOMPARE(attempts, 1); // read churn does not repeat metadata preparation
    QCOMPARE(validations, removal ? 1 : 2);
    const auto after = f.store->mutationStatistics();
    QCOMPARE(after.sealMetadataPreparations - before.sealMetadataPreparations, quint64(attempts));
    QCOMPARE(after.sealMetadataRejections - before.sealMetadataRejections, quint64(0));
    if (reader == 0 || reader == 3) {
        auto read = late.readResidentPage(key(1)); QVERIFY(read.isValid());
        QCOMPARE(static_cast<const quint8 *>(read.data())[0], quint8(0x41));
    }
    if (reader == 1) { QVERIFY(lease && lease->isValid()); QCOMPARE(static_cast<const quint8 *>(lease->cpuData())[0], quint8(0x41)); }
    QCOMPARE(quint8(f.pixel(overlay, 0)[0]), quint8(success ? (removal ? 0x2a : 0x71) : 0x41));
    QCOMPARE(quint8(f.pixel(overlay, 1)[0]), quint8(success ? 0x71 : 0x41));
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    for (int x : {0, 1}) {
        auto read = old.readResidentPage(key(x)); QVERIFY(read.isValid());
        QCOMPARE(static_cast<const quint8 *>(read.data())[0], quint8(0x41));
    }
    if (lease) { f.store->release(std::move(*lease)); lease.reset(); }
    late = {}; old = {}; QVERIFY(f.store->closeSession());
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::blockedSealValidationAllowsParallelWork()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay; overlay.transaction = tx.id;
    QSemaphore entered, resume; std::atomic<int> validations{0};
    f.provider->beforeValidate = [&] { if (++validations == 1) { entered.release(); resume.acquire(); } };
    bool sealed = false; QString error;
    std::thread writer([&] {
        auto segment = f.store->beginMutation(tx, &error);
        for (int x : {0, 1}) {
            auto w = segment.beginWrite(key(x), KisPageWriteMode::PreserveContents, &error);
            if (!w.isValid()) return;
            static_cast<quint8 *>(w.data())[0] = 0x71;
        }
        sealed = segment.seal(&error);
    });
    const auto join = qScopeGuard([&] { resume.release(); if (writer.joinable()) writer.join(); });
    QVERIFY(entered.tryAcquire(1, 5000));
    auto old = f.store->captureReadView(overlay); QVERIFY(old.isValid());
    for (int x : {0, 1}) { auto read = old.readResidentPage(key(x)); QVERIFY(read.isValid()); QCOMPARE(static_cast<const quint8 *>(read.data())[0], quint8(0x31)); }
    QVERIFY(!f.store->abort(tx)); QVERIFY(!f.store->closeSession());
    const auto independent = f.store->beginCurrentTransaction(); auto other = f.begin(independent);
    auto w = other.beginWrite(key(2)); QVERIFY(w.isValid()); static_cast<quint8 *>(w.data())[0] = 0x72; w = {};
    QVERIFY(other.seal()); QVERIFY(f.store->commit(independent, f.store->preparedPages(independent)).isValid());
    resume.release(); writer.join(); f.provider->beforeValidate = {};
    QVERIFY2(sealed, qPrintable(error));
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QCOMPARE(quint8(f.pixel({}, 0)[0]), quint8(0x71)); QCOMPARE(quint8(f.pixel({}, 1)[0]), quint8(0x71));
    QCOMPARE(quint8(f.pixel({}, 2)[0]), quint8(0x72));
    old = {}; QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::conditionalRemovalRejectsChangedOverlay()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay;
    overlay.transaction = tx.id;
    auto view = f.store->captureReadView(overlay); KisPageVersion before;
    QVERIFY(view.resolvePageVersion(key(), &before));
    auto segment = f.begin(tx); auto write = segment.beginWrite(key());
    QVERIFY(write.isValid()); const auto pending = write.version();
    static_cast<quint8 *>(write.data())[0] = 0x71;
    QVERIFY(!f.store->stagePageRemovalIfUnchanged(tx, before, &f.error));
    write = {}; QVERIFY(segment.seal());
    QVERIFY(!f.store->stagePageRemovalIfUnchanged(tx, before, &f.error));
    KisPageVersion current; QVERIFY(f.store->resolvePageVersion(key(), overlay, &current));
    QCOMPARE(current, pending);
    QVERIFY(!f.store->stagePageRemovalIfUnchanged(tx, {}, &f.error));
    QVERIFY(f.store->stagePageRemovalIfUnchanged(tx, current, &f.error));
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x2a));
    view = {}; QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::capturedOverlayDoesNotReviveUnsealedRemoval()
{
    QFETCH(bool, seal);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    QVERIFY(f.remove(tx, key()));
    auto segment = f.begin(tx); auto write = segment.beginWrite(key());
    QVERIFY2(write.isValid(), qPrintable(f.error));
    static_cast<quint8 *>(write.data())[0] = 0x71;
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay;
    overlay.transaction = tx.id;
    auto view = f.store->captureReadView(overlay); QVERIFY(view.isValid());
    auto blank = view.readResidentPage(key()); QVERIFY(blank.isValid());
    QVERIFY(blank.version().isDefaultPixel());
    QCOMPARE(static_cast<const quint8 *>(blank.data())[0], quint8(0x2a));
    write = {};
    if (seal) QVERIFY(segment.seal()); else QVERIFY(segment.cancel());
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QCOMPARE(quint8(f.pixel()[0]), quint8(seal ? 0x71 : 0x2a));
    QCOMPARE(static_cast<const quint8 *>(blank.data())[0], quint8(0x2a));
    blank = {}; view = {};
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::capturedOverlayConcurrentSegments()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay;
    overlay.transaction = tx.id;
    std::atomic<bool> done{false}, failed{false};
    QSemaphore started;
    std::thread reader([&] {
        started.release();
        for (int n = 0; n < 128 || !done.load(); ++n) {
            auto view = f.store->captureReadView(overlay);
            auto a = view.readResidentPage(key());
            std::this_thread::yield();
            auto b = view.readResidentPage(key(1));
            if (!view.isValid() || !a.isValid() || !b.isValid() ||
                static_cast<const quint8 *>(a.data())[0] != static_cast<const quint8 *>(b.data())[0])
                failed.store(true);
        }
    });
    const auto join = qScopeGuard([&] { done.store(true); if (reader.joinable()) reader.join(); });
    started.acquire();
    for (int n = 0; n < 64; ++n) {
        auto segment = f.begin(tx);
        for (int x : {0, 1}) {
            auto write = segment.beginWrite(key(x)); QVERIFY(write.isValid());
            static_cast<quint8 *>(write.data())[0] = 0x40 + n;
        }
        QVERIFY(segment.seal());
    }
    done.store(true); reader.join(); QVERIFY(!failed.load());
    QVERIFY(f.store->abort(tx)); QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::capturedOverlayFreezesSealedDelta()
{
    QFETCH(int, terminal);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    auto segment = f.begin(tx);
    for (int x : {0, 1}) {
        auto write = segment.beginWrite(key(x)); QVERIFY(write.isValid());
        static_cast<quint8 *>(write.data())[0] = 0x51 + x;
    }
    QVERIFY(segment.seal());
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay;
    overlay.transaction = tx.id;
    auto captured = f.store->captureReadView(overlay, &f.error);
    auto independent = f.store->captureReadView(overlay, &f.error);
    QVERIFY2(captured.isValid() && independent.isValid(), qPrintable(f.error));
    QVERIFY(captured.isTransactionOverlay()); QCOMPARE(captured.epoch(), tx.baseEpoch);
    const auto stats = f.store->sessionStats();
    KisPageStoreCpuAccessSession reader;
    QVERIFY(reader.beginRead(f.store.get(), {1}, overlay, KisPagePriority::Normal, &f.error));
    auto first = reader.readPage(0, 0, &f.error);
    QVERIFY2(first.isValid(), qPrintable(f.error)); QCOMPARE(first.data[0], quint8(0x51));
    segment = f.begin(tx);
    auto pending = segment.beginWrite(key(1)); QVERIFY(pending.isValid());
    static_cast<quint8 *>(pending.data())[0] = 0x72;
    // Capturing while an unpublished writer is active must expose the last
    // sealed delta, never the writable allocation currently being modified.
    auto duringWrite = f.store->captureReadView(overlay, &f.error);
    auto sealed = duringWrite.readResidentPage(key(1)); QVERIFY(sealed.isValid());
    QCOMPARE(static_cast<const quint8 *>(sealed.data())[0], quint8(0x52));
    pending = {}; QVERIFY(segment.seal());
    if (terminal == 2) QVERIFY(f.remove(tx, key(1)));
    auto second = reader.readPage(1, 0, &f.error);
    QVERIFY2(second.isValid(), qPrintable(f.error)); QCOMPARE(second.data[0], quint8(0x52));
    QCOMPARE(f.store->sessionStats().readRequestsCreated, stats.readRequestsCreated);
    // Generic control reads use the same frozen exact identity, including
    // while the transaction aborts and this access lease is still alive.
    const auto request = f.store->acquireReadInView(key(1), captured, cpu, KisPagePriority::Normal);
    QVERIFY2(request.isValid(), qPrintable(request.error));
    auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
    if (terminal == 1) QVERIFY(f.store->abort(tx));
    else QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QCOMPARE(static_cast<const quint8 *>(lease.cpuData())[0], quint8(0x52));
    f.store->release(std::move(lease));
    captured = {}; sealed = {}; duringWrite = {};
    QVERIFY(reader.finish(&f.error));
    QVERIFY(!f.store->closeSession());
    auto remaining = independent.readResidentPage(key(1)); QVERIFY(remaining.isValid());
    QCOMPARE(static_cast<const quint8 *>(remaining.data())[0], quint8(0x52));
    remaining = {}; independent = {};
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(quint8(f.pixel({}, 1)[0]), quint8(terminal == 0 ? 0x72 : terminal == 1 ? 0x31 : 0x2a));
    QVERIFY(f.store->closeSession());
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::capturedOverlayDefaultIdentitySurvivesAbortAndRestore()
{
    Fixture f; QVERIFY(f.init());
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x2a)); // materialized old default must advance too
    auto initial = f.store->captureRetainedEpoch(); QVERIFY(initial.isValid());
    auto tx = f.store->beginCurrentTransaction();
    QVERIFY(f.store->stageSurfaceDefaultPixel(tx, {1}, QByteArray(4, char(0x51)), &f.error));
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay;
    overlay.transaction = tx.id;
    auto first = f.store->captureReadView(overlay); QVERIFY(first.isValid());
    KisSurfaceEpochState firstState; QVERIFY(first.resolveSurfaceState({1}, &firstState));
    QCOMPARE(firstState.defaultPixelRevision, quint64(2));
    auto native = first.readResidentPage(key(4)); QVERIFY(native.isValid());
    QVERIFY(f.store->abort(tx));
    tx = f.store->beginCurrentTransaction();
    auto reused = firstState; reused.format.defaultPixel.fill(char(0x71));
    QVERIFY(!f.store->stageSurfaceMetadata(tx, reused, &f.error));
    QVERIFY(f.store->stageSurfaceDefaultPixel(tx, {1}, reused.format.defaultPixel, &f.error));
    overlay.transaction = tx.id;
    auto second = f.store->captureReadView(overlay); QVERIFY(second.isValid());
    KisSurfaceEpochState secondState; QVERIFY(second.resolveSurfaceState({1}, &secondState));
    QCOMPARE(secondState.defaultPixelRevision, quint64(3));
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QVERIFY(f.store->restoreRetainedEpoch(initial).isValid());
    tx = f.store->beginCurrentTransaction();
    QVERIFY(f.store->stageSurfaceDefaultPixel(tx, {1}, QByteArray(4, char(0x41)), &f.error));
    overlay.transaction = tx.id;
    auto third = f.store->captureReadView(overlay); QVERIFY(third.isValid());
    // A -> B -> A within one transaction still uses a fresh identity.
    QVERIFY(f.store->stageSurfaceDefaultPixel(tx, {1}, QByteArray(4, char(0x2a)), &f.error));
    QVERIFY(f.store->stageSurfaceDefaultPixel(tx, {1}, QByteArray(4, char(0x2a)), &f.error));
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    KisSurfaceEpochState current; QVERIFY(f.store->resolveSurfaceState({1}, {}, &current));
    QCOMPARE(current.defaultPixelRevision, quint64(5));
    for (auto *view : {&first, &second, &third}) {
        KisSurfaceEpochState state; QVERIFY(view->resolveSurfaceState({1}, &state));
        auto span = view->readResidentPage(key(4)); QVERIFY(span.isValid());
        QCOMPARE(QByteArray(static_cast<const char *>(span.data()), 4), state.format.defaultPixel);
        const auto request = f.store->acquireReadInView(key(5), *view, cpu, KisPagePriority::Normal);
        QVERIFY2(request.isValid(), qPrintable(request.error));
        auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
        QCOMPARE(QByteArray(static_cast<const char *>(lease.cpuData()), 4), state.format.defaultPixel);
        f.store->release(std::move(lease));
    }
    native = {}; first = {}; second = {}; third = {};
    QVERIFY(f.store->releaseSnapshot(initial.token));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::capturedOverlayOutlivesFacade()
{
    Fixture f; QVERIFY(f.init());
    const auto tx = f.store->beginCurrentTransaction();
    auto segment = f.begin(tx);
    auto write = segment.beginWrite(key()); QVERIFY(write.isValid());
    static_cast<quint8 *>(write.data())[0] = 0x71;
    write = {}; QVERIFY(segment.seal()); segment = {};
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay;
    overlay.transaction = tx.id;
    auto captured = f.store->captureReadView(overlay); QVERIFY(captured.isValid());
    QVERIFY(f.store->abort(tx));
    f.store.reset();
    auto guard = captured.readResidentPage(key()); QVERIFY(guard.isValid());
    captured = {};
    QCOMPARE(static_cast<const quint8 *>(guard.data())[0], quint8(0x71));
    guard = {};
    kisDrainPageStoreReclamation();
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::capturedCutStorageRefusalAndLifetime()
{
    KisPageBackingLimits limits; limits.metadataArenaBytes = 4 * 1024 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    std::array<KisBackingBudgetReservation, 8> slots;
    for (auto &slot : slots) { slot = parent->reserve({}, nullptr); QVERIFY(slot.isValid()); }
    for (auto &slot : slots) slot.release();
    const auto live = [&] {
        return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const auto baseline = live();
    Fixture f;
    QVERIFY(f.store->configureSharedNonPayloadBudget(parent, &f.error));
    QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    auto segment = f.begin(tx);
    auto write = segment.beginWrite(key()); QVERIFY(write.isValid());
    static_cast<quint8 *>(write.data())[0] = 0x71;
    write = {}; QVERIFY(segment.seal()); segment = {};
    QVERIFY(f.remove(tx, key(1)));
    KisSurfaceEpochState surface;
    QVERIFY(f.store->resolveSurfaceState({1}, {}, &surface));
    ++surface.defaultPixelRevision; surface.format.defaultPixel = QByteArray(4, char(0x65));
    QVERIFY(f.store->stageSurfaceMetadata(tx, surface));
    const auto overlay = KisPageReadView::transactionOverlay(tx.id);
    auto captured = f.store->captureReadView(overlay); QVERIFY(captured.isValid());
    const auto snapshots = f.store->sessionStats().retainedSnapshots;
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - live());
    void *filler = kisAllocateMutationStorage(parent.get(), fillerBytes, 1);
    {
        const auto freeFiller = qScopeGuard([&] { kisFreeMutationStorage(parent.get(), filler, fillerBytes, 1); });
        QCOMPARE(live(), limits.metadataArenaBytes);
        // The real per-cut default map node must be paid before buffer creation.
        QVERIFY(!captured.readResidentPage(key(1)).isValid());
        QVERIFY(!f.store->captureReadView(overlay).isValid());
        QCOMPARE(f.store->sessionStats().retainedSnapshots, snapshots);
    }
    auto defaultGuard = captured.readResidentPage(key(1)); QVERIFY(defaultGuard.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(defaultGuard.data()), 4), QByteArray(4, char(0x65)));
    ++surface.defaultPixelRevision; surface.format.defaultPixel = QByteArray(4, char(0x75));
    QVERIFY(f.store->stageSurfaceMetadata(tx, surface));
    auto later = f.store->captureReadView(overlay); QVERIFY(later.isValid());
    auto laterGuard = later.readResidentPage(key(1)); QVERIFY(laterGuard.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(laterGuard.data()), 4), QByteArray(4, char(0x75)));
    laterGuard = {}; later = {};
    QVERIFY(f.store->abort(tx)); f.store.reset();
    auto physicalGuard = captured.readResidentPage(key()); QVERIFY(physicalGuard.isValid());
    captured = {};
    QCOMPARE(static_cast<const quint8 *>(physicalGuard.data())[0], quint8(0x71));
    QCOMPARE(QByteArray(static_cast<const char *>(defaultGuard.data()), 4), QByteArray(4, char(0x65)));
    QVERIFY(live() > baseline);
    physicalGuard = {}; defaultGuard = {};
    f.completions.reset(); f.provider.reset();
    kisDrainPageStoreReclamation();
    QTRY_COMPARE(live(), baseline);
}

void KisPageStoreCpuMutationTest::capturedOverlayRevalidatesCommitPreparation()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    auto segment = f.begin(tx); auto write = segment.beginWrite(key());
    QVERIFY(write.isValid()); static_cast<quint8 *>(write.data())[0] = 0x71;
    write = {}; QVERIFY(segment.seal());
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay;
    overlay.transaction = tx.id;
    KisCapturedReadView late;
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase == KisPageStoreDiagnosticPhase::CommitPublishOwnerWait && !late.isValid())
            late = f.store->captureReadView(overlay);
    });
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QVERIFY(late.isValid());
    QVERIFY(f.fill(0x41));
    auto guard = late.readResidentPage(key()); QVERIFY(guard.isValid());
    QCOMPARE(static_cast<const quint8 *>(guard.data())[0], quint8(0x71));
    guard = {}; late = {};
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::capturedOverlayGenericSessionKeepsExactVersion()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    auto segment = f.begin(tx); auto write = segment.beginWrite(key());
    QVERIFY(write.isValid()); static_cast<quint8 *>(write.data())[0] = 0x71;
    write = {}; QVERIFY(segment.seal());
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay;
    overlay.transaction = tx.id;
    f.provider->rejectNativeBinding = true;
    KisPageStoreCpuAccessSession reader;
    QVERIFY(reader.beginRead(f.store.get(), {1}, overlay, KisPagePriority::Normal, &f.error));
    auto captured = f.store->captureReadView(overlay, &f.error);
    QVERIFY(captured.isValid());
    QVERIFY(f.store->abort(tx));
    const auto before = f.store->sessionStats().readRequestsCreated;
    const auto span = reader.readPage(0, 0, &f.error); QVERIFY2(span.isValid(), qPrintable(f.error));
    QCOMPARE(span.data[0], quint8(0x71));
    QCOMPARE(f.store->sessionStats().readRequestsCreated, before + 1);
    {
        KisPageStoreReadPage page(f.store.get(), captured, key(), &f.error);
        QVERIFY2(page.data(), qPrintable(f.error));
        QCOMPARE(page.data()[0], quint8(0x71));
        QCOMPARE(page.version(), span.version);
        const auto pointer = page.data();
        KisPageStoreReadPage moved(std::move(page));
        QVERIFY(!page.data()); QCOMPARE(moved.data(), pointer);
        page = std::move(moved);
        QVERIFY(!moved.data()); QCOMPARE(page.data(), pointer);
        QCOMPARE(f.store->sessionStats().readRequestsCreated, before + 2);
    }
    captured = {};
    QVERIFY(reader.finish(&f.error));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::sharedReadFailureReleasesOnlyExpiredPins()
{
    QFETCH(bool, cursor);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    f.provider->rejectNativeBinding = true;
    KisPageStoreCpuAccessSession reader(cursor ? KisCpuAccessLifetime::CursorPage : KisCpuAccessLifetime::OperationSpans);
    QVERIFY(reader.beginRead(f.store.get(), {1}, {}, KisPagePriority::Interactive, &f.error));
    f.provider->rejectReadAccess = true;
    QVERIFY(!reader.readPage(0, 0, &f.error).isValid());
    QCOMPARE(f.store->sessionStats().pendingRequests, qsizetype(0));
    QCOMPARE(f.store->sessionStats().activeReadLeases, qsizetype(0));
    f.provider->rejectReadAccess = false;
    const auto first = reader.readPage(0, 0, &f.error); QVERIFY2(first.isValid(), qPrintable(f.error));
    QCOMPARE(first.byteSize, quint64(64 * 64 * f.bpp));
    QCOMPARE(first.rowStride, quint32(64 * f.bpp));
    QCOMPARE(first.data[0], quint8(0x31));
    const auto readRequests = f.store->sessionStats().readRequestsCreated;
    const auto repeated = reader.readPage(0, 0, &f.error);
    QCOMPARE(repeated.data, first.data);
    QCOMPARE(f.store->sessionStats().readRequestsCreated, readRequests);
    f.provider->rejectReadAccess = true;
    QVERIFY(!reader.readPage(1, 0, &f.error).isValid());
    QCOMPARE(f.store->sessionStats().pendingRequests, qsizetype(0));
    QCOMPARE(f.store->sessionStats().activeReadLeases, qsizetype(cursor ? 0 : 1));
    if (!cursor) QCOMPARE(first.data[0], quint8(0x31));
    f.provider->rejectReadAccess = false;
    f.provider->rejectNativeBinding = false;
    QVERIFY(f.fill(0x57));
    f.provider->rejectNativeBinding = true;
    const auto second = reader.readPage(1, 0, &f.error); QVERIFY2(second.isValid(), qPrintable(f.error));
    QCOMPARE(second.data[0], quint8(0x31)); // cache miss still uses the captured root
    QCOMPARE(f.store->sessionStats().activeReadLeases, qsizetype(cursor ? 1 : 2));
    QVERIFY(reader.finish(&f.error));
    QCOMPARE(f.store->sessionStats().activeReadLeases, qsizetype(0));
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
}

void KisPageStoreCpuMutationTest::retiredEpochAdmissionAndPhysicalReaders()
{
    QFETCH(int, bpp); QFETCH(int, protection);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x35));
    const auto old = f.store->captureCommittedEpoch();
    auto retained = protection == 1 ? f.store->captureRetainedEpoch() : KisRetainedImageEpochSnapshot{};
    const auto base = protection == 2 ? f.store->beginCurrentTransaction() : KisPageTransaction{};
    auto view = (protection == 3 || protection == 4) ? f.store->captureReadView({}) : KisCapturedReadView{};
    auto guard = protection == 4 ? view.readResidentPage(key()) : KisCpuReadGuard{};
    if (protection == 4) { QVERIFY(guard.isValid()); view = {}; } // Guard itself retains the View.
    auto lease = [&] {
        if (protection != 5) return KisReadLease();
        const auto request = f.store->acquireRead(key(), {}, cpu, KisPagePriority::Normal);
        return f.store->resolve(request, request.readiness);
    }();
    if (protection == 5) QVERIFY(lease.isValid());
    QSemaphore entered, resume;
    kisSchedulePageStoreReclamation([&] { entered.release(); resume.acquire(); });
    auto unblock = qScopeGuard([&] { resume.release(); kisDrainPageStoreReclamation(); });
    QVERIFY(entered.tryAcquire(1, 10000));
    QVERIFY(f.fill(0x65, false));
    QVERIFY(f.store->sessionStats().immutableRoots >= 2); // Physical collection is blocked.
    auto adopted = f.store->beginTransaction(old.epoch);
    const bool logical = protection >= 1 && protection <= 4;
    QCOMPARE(adopted.isValid(), logical);
    KisCapturedReadView adoptedView;
    if (logical) {
        KisPageReadView selector; selector.kind = KisPageReadViewKind::TransactionBaseEpoch; selector.transaction = adopted.id;
        adoptedView = f.store->captureReadView(selector); QVERIFY(adoptedView.isValid());
        auto read = adoptedView.readResidentPage(key()); QVERIFY(read.isValid());
        QCOMPARE(QByteArray(static_cast<const char *>(read.data()), bpp), QByteArray(bpp, char(0x35)));
        if (retained.isValid()) QVERIFY(f.store->releaseSnapshot(retained.token));
        if (base.isValid()) QVERIFY(f.store->abort(base));
        view = {}; guard = {};
        QVERIFY(f.store->abort(adopted));
        // The captured transaction base outlives the aborted transaction.
        read = {}; read = adoptedView.readResidentPage(key()); QVERIFY(read.isValid());
        QCOMPARE(QByteArray(static_cast<const char *>(read.data()), bpp), QByteArray(bpp, char(0x35)));
        read = {}; adoptedView = {};
    }
    QVERIFY(!f.store->beginTransaction(old.epoch).isValid());
    QCOMPARE(f.pixel(), QByteArray(bpp, char(0x65)));
    resume.release(); unblock.dismiss(); QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().immutableRoots, qsizetype(1));
    if (lease.isValid()) {
        // A generic lease protects exact physical bytes, not admission to all
        // pages of an old epoch. Logical root retirement must not free them.
        QCOMPARE(QByteArray(static_cast<const char *>(lease.cpuData()), bpp), QByteArray(bpp, char(0x35)));
        QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(3));
        QVERIFY(!f.store->closeSession());
        f.store->release(std::move(lease)); QVERIFY(f.store->waitForRetirementIdle());
    }
    QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(2));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::historyReachabilityIgnoresRetiredRootBacklog()
{
    QFETCH(int, bpp); QFETCH(int, history); QFETCH(bool, keepOldest);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x35));
    // Outlive the cleanup guard even if an assertion returns while the worker
    // still owns the callback; cleanup drains before releasing captures.
    std::atomic<bool> reclaimedWithRootBacklog{false};
    QSemaphore entered, resume;
    kisSchedulePageStoreReclamation([&] { entered.release(); resume.acquire(); });
    auto unblock = qScopeGuard([&] { resume.release(); kisDrainPageStoreReclamation(); f.provider->beforeRetire = {}; });
    QVERIFY(entered.tryAcquire(1, 10000));
    auto oldest = f.store->captureReadView({}); QVERIFY(oldest.isValid());
    for (int i = 0; i < history; ++i) {
        const auto tx = f.store->beginCurrentTransaction(); auto mutation = f.begin(tx);
        auto write = mutation.beginWrite(key()); QVERIFY(write.isValid());
        std::memset(write.data(), 0x71, size_t(bpp)); write = {}; QVERIFY(mutation.seal());
        QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    }
    if (!keepOldest) oldest = {};
    QCOMPARE(f.store->sessionStats().immutableRoots, qsizetype(history + 1));
    const auto before = f.store->readScopeStatistics();
    f.provider->beforeRetire = [&] {
        // Provider retirement runs outside owner. Prove history collection is
        // already effective before physical root registry cleanup catches up.
        if (f.store->sessionStats().immutableRoots > (keepOldest ? 2 : 1)) reclaimedWithRootBacklog = true;
    };
    resume.release(); QVERIFY(f.store->waitForRetirementIdle());
    f.provider->beforeRetire = {}; unblock.dismiss();
    QVERIFY(reclaimedWithRootBacklog.load());
    const auto after = f.store->readScopeStatistics();
    const auto refreshes = after.historyReachabilityRefreshes - before.historyReachabilityRefreshes;
    const quint64 slices = quint64((history + 31) / 32);
    QVERIFY(refreshes >= slices && refreshes <= 2 * slices);
    QVERIFY(after.maximumHistoryRootsPerPass > 0 && after.maximumHistoryRootsPerPass <= 32);
    QCOMPARE(after.historyReachabilityRootsVisited - before.historyReachabilityRootsVisited,
             quint64(keepOldest ? 2 : 1) * refreshes);
    QCOMPARE(f.store->sessionStats().immutableRoots, qsizetype(keepOldest ? 2 : 1));
    QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(keepOldest ? 3 : 2));
    QCOMPARE(f.pixel(), QByteArray(bpp, char(0x71)));
    if (keepOldest) {
        auto read = oldest.readResidentPage(key()); QVERIFY(read.isValid());
        QCOMPARE(QByteArray(static_cast<const char *>(read.data()), bpp), QByteArray(bpp, char(0x35)));
        read = {}; oldest = {}; QVERIFY(f.store->waitForRetirementIdle());
    }
    QCOMPARE(f.store->sessionStats().immutableRoots, qsizetype(1));
    QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(2));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::longHistoryUsesVersionBudget()
{
    QFETCH(int, bpp); QFETCH(int, history);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x35));
    QSemaphore entered, resume;
    kisSchedulePageStoreReclamation([&] { entered.release(); resume.acquire(); });
    auto unblock = qScopeGuard([&] { resume.release(); kisDrainPageStoreReclamation(); });
    QVERIFY(entered.tryAcquire(1, 10000));
    std::vector<KisCapturedReadView> views;
    views.push_back(f.store->captureReadView({}));
    for (int i = 0; i < history; ++i) {
        const auto tx = f.store->beginCurrentTransaction(); auto mutation = f.begin(tx);
        auto write = mutation.beginWrite(key(0)); QVERIFY(write.isValid());
        std::memset(write.data(), 0x60 + i % 0x30, size_t(bpp)); write = {}; QVERIFY(mutation.seal());
        QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
        views.push_back(f.store->captureReadView({})); QVERIFY(views.back().isValid());
    }
    const auto before = f.store->readScopeStatistics();
    const auto metadataBefore = kisPageStoreMetadataMetrics(*f.store);
    resume.release(); unblock.dismiss();
    QVERIFY(f.store->waitForRetirementIdle());
    const auto retained = f.store->readScopeStatistics();
    const auto metadataRetained = kisPageStoreMetadataMetrics(*f.store);
    const auto refreshes = retained.historyReachabilityRefreshes - before.historyReachabilityRefreshes;
    const quint64 slices = quint64((history + 31) / 32);
    QVERIFY(refreshes >= slices && refreshes <= 2 * slices);
    QVERIFY(retained.maximumHistoryRootsPerPass > 0 && retained.maximumHistoryRootsPerPass <= 32);
    QCOMPARE(retained.historyReachabilityRootsVisited - before.historyReachabilityRootsVisited,
             quint64(history + 1) * refreshes);
    // Candidate storage is bounded to 32 versions, so each group visits the
    // protected roots independently; no history-sized reachability cache.
    QCOMPARE(metadataRetained.historySliceVersionInputs - metadataBefore.historySliceVersionInputs,
             quint64(history) * (refreshes / slices));
    QCOMPARE(metadataRetained.fullSnapshotExports, metadataBefore.fullSnapshotExports);
    QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(history + 2));
    QCOMPARE(f.store->sessionStats().activeHistoryScans, qsizetype(0));
    QCOMPARE(f.store->sessionStats().cachedHistoryReachableVersions, quint64(0));
    auto held = std::move(views[size_t(history / 2)]);
    views.clear(); QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(3));
    auto pin = held.readResidentPage(key(0)); QVERIFY(pin.isValid()); held = {};
    QCOMPARE(QByteArray(static_cast<const char *>(pin.data()), bpp), QByteArray(bpp, char(0x60 + (history / 2 - 1) % 0x30)));
    QVERIFY(f.store->waitForRetirementIdle()); QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(3));
    QVERIFY(!f.store->closeSession()); pin = {};
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(2));
    const auto after = f.store->readScopeStatistics();
    const auto metadataAfter = kisPageStoreMetadataMetrics(*f.store);
    QVERIFY(after.maximumHistoryVersionsPerPass > 0 && after.maximumHistoryVersionsPerPass <= 512);
    QVERIFY(metadataAfter.maximumHistorySliceVersionInputs > 0 && metadataAfter.maximumHistorySliceVersionInputs <= 32);
    QCOMPARE(after.foregroundHistoricalGcPagesVisited, quint64(0));
    QCOMPARE(metadataAfter.fullSnapshotExports, metadataBefore.fullSnapshotExports);
    QCOMPARE(metadataAfter.backgroundLocalVersionRemovals - metadataBefore.backgroundLocalVersionRemovals, quint64(history));
    QCOMPARE(f.store->sessionStats().pendingHistoricalPages, qsizetype(0));
    QCOMPARE(f.store->sessionStats().deferredHistoricalPages, qsizetype(0));
    qInfo() << "history budget" << history << "retained refreshes" << refreshes
            << "slice maximum" << metadataAfter.maximumHistorySliceVersionInputs
            << "removed" << metadataAfter.backgroundLocalVersionRemovals - metadataBefore.backgroundLocalVersionRemovals;
    QVERIFY(f.store->closeSession()); QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::multiPageHistorySharesRootBudget()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp));
    const auto fill = [&](quint8 value) {
        const auto tx = f.store->beginCurrentTransaction();
        return KisPageStoreCpuSurfaceOps::fillRect(f.store.get(), {1}, tx,
            QRect(0, 0, 8 * 64, 64), QByteArray(bpp, char(value)), &f.error) &&
            f.store->commit(tx, f.store->preparedPages(tx)).isValid();
    };
    QVERIFY(fill(0x31)); QVERIFY(f.store->waitForRetirementIdle());
    QSemaphore entered, resume;
    kisSchedulePageStoreReclamation([&] { entered.release(); resume.acquire(); });
    auto unblock = qScopeGuard([&] { resume.release(); kisDrainPageStoreReclamation(); });
    QVERIFY(entered.tryAcquire(1, 10000));
    std::vector<KisCapturedReadView> views;
    for (int i = 0; i < 33; ++i) {
        views.push_back(f.store->captureReadView({})); QVERIFY(views.back().isValid());
        QVERIFY(fill(quint8(0x50 + i)));
    }
    resume.release(); unblock.dismiss(); QVERIFY(f.store->waitForRetirementIdle());
    const auto stats = f.store->readScopeStatistics();
    QVERIFY(stats.maximumHistoryRootsPerPass > 0 && stats.maximumHistoryRootsPerPass <= 32);
    QCOMPARE(stats.historyReachabilityRestarts, quint64(0));
    for (int column = 0; column < 8; ++column) {
        auto before = views.front().readResidentPage(key(column)); QVERIFY(before.isValid());
        QCOMPARE(static_cast<const quint8 *>(before.data())[0], quint8(0x31));
        QCOMPARE(quint8(f.pixel({}, column)[0]), quint8(0x70));
    }
    views.clear(); QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().activeHistoryScans, qsizetype(0));
    QCOMPARE(f.store->sessionStats().cachedHistoryReachableVersions, quint64(0));
    QCOMPARE(f.store->sessionStats().pendingHistoricalPages, qsizetype(0));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(8 * 64 * 64 * bpp));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::historyRootSliceRevalidatesChangedKey()
{
    QFETCH(int, bpp); QFETCH(bool, changedKey);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    QSemaphore entered, resume, firstSlice, continueScanning;
    kisSchedulePageStoreReclamation([&] { entered.release(); resume.acquire(); });
    auto unblock = qScopeGuard([&] {
        resume.release(); continueScanning.release(); kisDrainPageStoreReclamation();
    });
    QVERIFY(entered.tryAcquire(1, 10000));
    const auto write = [&](int column, quint8 value) {
        const auto tx = f.store->beginCurrentTransaction(); auto mutation = f.begin(tx);
        auto guard = mutation.beginWrite(key(column)); if (!guard.isValid()) return false;
        std::memset(guard.data(), value, size_t(bpp)); guard = {};
        return mutation.seal() && f.store->commit(tx, f.store->preparedPages(tx)).isValid();
    };
    std::vector<KisCapturedReadView> views;
    for (int i = 0; i < 65; ++i) {
        views.push_back(f.store->captureReadView({})); QVERIFY(views.back().isValid());
        QVERIFY(write(0, quint8(0x40 + i)));
    }
    // This barrier is after the first bounded collector pass but before its
    // continuation. Publish while a real root cursor is partially scanned.
    kisSchedulePageStoreReclamation([&] { firstSlice.release(); continueScanning.acquire(); });
    resume.release(); QVERIFY(firstSlice.tryAcquire(1, 10000));
    QVERIFY(f.store->sessionStats().activeHistoryScans > 0);
    const auto before = f.store->readScopeStatistics();
    QVERIFY(write(changedKey ? 0 : 1, 0xc3));
    continueScanning.release(); unblock.dismiss(); QVERIFY(f.store->waitForRetirementIdle());
    const auto after = f.store->readScopeStatistics();
    if (changedKey) QVERIFY(after.historyReachabilityRestarts > before.historyReachabilityRestarts);
    else QCOMPARE(after.historyReachabilityRestarts, before.historyReachabilityRestarts);
    QVERIFY(after.maximumHistoryRootsPerPass <= 32);
    auto old = views.front().readResidentPage(key(0)); QVERIFY(old.isValid());
    QCOMPARE(static_cast<const quint8 *>(old.data())[0], quint8(0x31));
    old = {}; views.clear(); QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().activeHistoryScans, qsizetype(0));
    QCOMPARE(f.store->sessionStats().cachedHistoryReachableVersions, quint64(0));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::historySweepRevalidatesAndRevisits()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x35));
    const auto oldest = f.store->captureRetainedEpoch(); QVERIFY(oldest.isValid());
    QSemaphore initialEntered, initialResume, retiring, continueRetiring;
    std::atomic<bool> parked{false}; // outlives cleanup's worker drain on an early assertion failure
    kisSchedulePageStoreReclamation([&] { initialEntered.release(); initialResume.acquire(); });
    auto cleanup = qScopeGuard([&] {
        initialResume.release(); continueRetiring.release(); kisDrainPageStoreReclamation(); f.provider->beforeRetire = {};
    });
    QVERIFY(initialEntered.tryAcquire(1, 10000));
    f.provider->beforeRetire = [&] {
        if (!parked.exchange(true)) { retiring.release(); continueRetiring.acquire(); }
    };
    for (int i = 0; i < 100; ++i) {
        const auto tx = f.store->beginCurrentTransaction(); auto mutation = f.begin(tx);
        auto write = mutation.beginWrite(key(0)); QVERIFY(write.isValid());
        std::memset(write.data(), 0x60 + i % 0x30, size_t(bpp)); write = {}; QVERIFY(mutation.seal());
        QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    }
    initialResume.release(); QVERIFY(retiring.tryAcquire(1, 10000));
    QVERIFY(f.store->sessionStats().activeHistoryScans > 0);
    QVERIFY(f.store->sessionStats().cachedHistoryReachableVersions > 0);
    // The history cursor has passed the oldest (protected) generation. Restore
    // it and supersede it again while physical retirement is parked outside
    // owner. This creates history BELOW the cursor and changes root reachability.
    QVERIFY(f.store->restoreRetainedEpoch(oldest).isValid());
    const auto tx = f.store->beginCurrentTransaction(); auto mutation = f.begin(tx);
    auto write = mutation.beginWrite(key(0)); QVERIFY(write.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(write.data()), bpp), QByteArray(bpp, char(0x35)));
    std::memset(write.data(), 0xe7, size_t(bpp)); write = {}; QVERIFY(mutation.seal());
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QVERIFY(f.store->releaseSnapshot(oldest.token));
    continueRetiring.release(); QVERIFY(f.store->waitForRetirementIdle());
    f.provider->beforeRetire = {}; cleanup.dismiss();
    QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(2));
    QCOMPARE(f.store->sessionStats().activeHistoryScans, qsizetype(0));
    QCOMPARE(f.store->sessionStats().cachedHistoryReachableVersions, quint64(0));
    QCOMPARE(f.store->sessionStats().pendingHistoricalPages, qsizetype(0));
    QCOMPARE(f.store->sessionStats().deferredHistoricalPages, qsizetype(0));
    QVERIFY(f.store->readScopeStatistics().historyReachabilityRefreshes >= 2);
    auto view = f.store->captureReadView({}); auto read = view.readResidentPage(key(0)); QVERIFY(read.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(read.data()), bpp), QByteArray(bpp, char(0xe7)));
    read = {}; view = {}; QVERIFY(f.store->closeSession());
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::retainedHistoryReclaimsInBackground()
{
    Fixture f; QVERIFY(f.init());
    constexpr int pages = 96;
    const auto fill = [&](char value) {
        const auto tx = f.store->beginCurrentTransaction();
        return KisPageStoreCpuSurfaceOps::fillRect(f.store.get(), {1}, tx,
            QRect(0, 0, pages * 64, 64), QByteArray(f.bpp, value), &f.error) &&
            f.store->commit(tx, f.store->preparedPages(tx)).isValid() && f.store->waitForRetirementIdle();
    };
    QVERIFY(fill(0x31));
    auto view = f.store->captureReadView();
    QVERIFY(fill(0x77));
    auto old = view.readResidentPage(key(95));
    QVERIFY(old.isValid());
    QCOMPARE(static_cast<const quint8 *>(old.data())[0], quint8(0x31));
    QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(2 * pages));
    const auto idle = f.store->readScopeStatistics();
    for (int i = 0; i < 64; ++i) {
        auto current = f.store->captureReadView();
        QVERIFY(current.isValid());
    }
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->readScopeStatistics().historicalGcPagesVisited, idle.historicalGcPagesVisited);
    // Rejected shutdown must unfreeze scheduling, including pending history.
    QVERIFY(!f.store->closeSession());
    old = {};
    const auto before = f.store->readScopeStatistics();
    view = {};
    QCOMPARE(f.store->readScopeStatistics().foregroundHistoricalGcPagesVisited,
             before.foregroundHistoricalGcPagesVisited);
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(pages));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(pages * 64 * 64 * f.bpp));
    const auto stats = f.store->sessionStats();
    QCOMPARE(stats.pendingHistoricalPages, qsizetype(0));
    QCOMPARE(stats.deferredHistoricalPages, qsizetype(0));
    QCOMPARE(stats.scheduledReclamationJobs, qsizetype(0));
    QVERIFY(stats.backgroundRetirementPasses > 0 && stats.maximumReplicasPerRetirementPass <= 32);
    const auto after = f.store->readScopeStatistics();
    QVERIFY(after.historicalGcPagesVisited >= before.historicalGcPagesVisited + pages);
    QVERIFY(after.maximumHistoryPagesPerPass > 0 && after.maximumHistoryPagesPerPass <= 16);
    QCOMPARE(after.foregroundHistoricalGcPagesVisited, quint64(0));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::lastUseDefersBackgroundHistory()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto request = f.store->acquireRead(key(), {}, cpu, KisPagePriority::Normal);
    auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
    const KisCompletionDomain source = KisCompletionDomain::HostLogical;
    const auto completion = f.completions->allocatePending(f.completions->registerSource(source));
    QVERIFY(completion.isValid());
    QVERIFY(f.fill(0x77));
    f.store->release(std::move(lease), completion);
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(3));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(3 * 64 * 64 * f.bpp));
    QVERIFY(!f.store->closeSession());
    QVERIFY(f.completions->complete(completion, KisCompletionStatus::Succeeded));
    QTRY_COMPARE(f.store->sessionStats().pendingLastUses, qsizetype(0));
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(2));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(2 * 64 * 64 * f.bpp));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::automaticLastUse_data()
{
    QTest::addColumn<int>("bpp");
    QTest::addColumn<int>("terminal");
    QTest::addColumn<int>("order");
    for (int bpp : {1, 4, 8, 16})
        for (int terminal = 0; terminal < 3; ++terminal)
            for (int order = 0; order < 3; ++order)
                QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(bpp).arg(terminal).arg(order)))
                    << bpp << terminal << order;
}

void KisPageStoreCpuMutationTest::automaticLastUse()
{
    QFETCH(int, bpp); QFETCH(int, terminal); QFETCH(int, order);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const auto request = f.store->acquireRead(key(), {}, cpu, KisPagePriority::Normal);
    auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
    const auto source = f.completions->registerSource(KisCompletionDomain::HostLogical);
    const auto completion = f.completions->allocatePending(source);
    const auto status = terminal == 0 ? KisCompletionStatus::Succeeded :
        terminal == 1 ? KisCompletionStatus::Failed : KisCompletionStatus::Cancelled;
    QVERIFY(f.fill(0x77));
    bool completed = false;
    if (order == 1) completed = f.completions->complete(completion, status);
    if (order == 2) f.provider->beforeRelease = [&] {
        completed = f.completions->complete(completion, status);
    };
    f.store->release(std::move(lease), completion);
    f.provider->beforeRelease = {};
    if (order == 0) {
        QCOMPARE(f.store->sessionStats().pendingLastUses, qsizetype(1));
        completed = f.completions->complete(completion, status);
    }
    QVERIFY(completed);
    QVERIFY(f.store->waitForRetirementIdle());
    const auto stats = f.store->sessionStats();
    QCOMPARE(stats.pendingLastUses, qsizetype(0));
    QCOMPARE(stats.activeReadLeases, qsizetype(0));
    QCOMPARE(stats.pageVersions, qsizetype(2));
    QCOMPARE(stats.scheduledReclamationJobs, qsizetype(0));
    if (order == 0) QVERIFY(stats.lastUseAcknowledgePasses > 0);
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(2 * 64 * 64 * bpp));
    QCOMPARE(f.completions->sourceStatistics(source).readinessSignals, quint64(0));
    QCOMPARE(f.pixel(), QByteArray(bpp, char(0x77)));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::lastUseBoundedBurst_data()
{
    QTest::addColumn<bool>("sharedTicket");
    QTest::newRow("independent-tickets") << false;
    QTest::newRow("same-ticket-many-leases") << true;
}

void KisPageStoreCpuMutationTest::lastUseBoundedBurst()
{
    QFETCH(bool, sharedTicket);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto source = f.completions->registerSource(KisCompletionDomain::HostLogical);
    QVector<KisCompletionTicket> tickets;
    constexpr int count = 129;
    for (int i = 0; i < count; ++i) {
        const auto request = f.store->acquireRead(key(), {}, cpu, KisPagePriority::Normal);
        auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
        if (!sharedTicket || tickets.isEmpty()) tickets.append(f.completions->allocatePending(source));
        f.store->release(std::move(lease), tickets.back());
    }
    QCOMPARE(f.store->sessionStats().pendingLastUses, qsizetype(count));
    QCOMPARE(f.completions->sourceStatistics(source).readinessWaiters, quint64(count));
    QVERIFY(f.fill(0x77));
    QSemaphore entered, resume;
    bool blocked = true;
    auto unblock = qScopeGuard([&] { if (blocked) resume.release(); });
    kisSchedulePageStoreReclamation([&] { entered.release(); resume.acquire(); });
    entered.acquire();
    for (const auto &ticket : tickets) QVERIFY(f.completions->complete(ticket, KisCompletionStatus::Succeeded));
    QVERIFY(f.store->sessionStats().scheduledReclamationJobs > 0);
    resume.release(); blocked = false;
    QVERIFY(f.store->waitForRetirementIdle());
    const auto stats = f.store->sessionStats();
    QCOMPARE(stats.pendingLastUses, qsizetype(0));
    QCOMPARE(stats.activeReadLeases, qsizetype(0));
    QVERIFY(stats.lastUseAcknowledgePasses >= 5);
    QVERIFY(stats.maximumLastUsesPerPass > 0 && stats.maximumLastUsesPerPass <= 32);
    QCOMPARE(stats.pageVersions, qsizetype(2));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(2 * 64 * 64 * f.bpp));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::lastUseCloseAndLifetime_data()
{
    QTest::addColumn<int>("mode");
    QTest::newRow("close-queued-event") << 0;
    QTest::newRow("destroy-pending") << 1;
    QTest::newRow("destroy-queued-event") << 2;
}

void KisPageStoreCpuMutationTest::lastUseCloseAndLifetime()
{
    QFETCH(int, mode);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto source = f.completions->registerSource(KisCompletionDomain::HostLogical);
    const auto completion = f.completions->allocatePending(source);
    const auto request = f.store->acquireRead(key(), {}, cpu, KisPagePriority::Normal);
    auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
    f.store->release(std::move(lease), completion);
    QCOMPARE(f.completions->sourceStatistics(source).readinessWaiters, quint64(1));
    QSemaphore entered, resume;
    bool blocked = true;
    auto unblock = qScopeGuard([&] { if (blocked) resume.release(); });
    kisSchedulePageStoreReclamation([&] { entered.release(); resume.acquire(); });
    entered.acquire();
    if (mode != 1) QVERIFY(f.completions->complete(completion, KisCompletionStatus::Succeeded));
    if (mode == 0) {
        bool closed = false;
        std::thread closing([&] { closed = f.store->closeSession(); });
        resume.release(); blocked = false;
        closing.join();
        QVERIFY(closed);
        QCOMPARE(f.store->sessionStats().pendingLastUses, qsizetype(0));
        QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
    } else {
        std::weak_ptr<TestProvider> weak = f.provider;
        f.provider.reset(); f.store.reset();
        QCOMPARE(f.completions->sourceStatistics(source).readinessWaiters, quint64(0));
        if (mode == 1) QVERIFY(f.completions->complete(completion, KisCompletionStatus::Succeeded));
        resume.release(); blocked = false;
        kisDrainPageStoreReclamation();
        QTRY_VERIFY(weak.expired());
    }
}

void KisPageStoreCpuMutationTest::lastUseManualCompatibility()
{
    Fixture f; f.provider->disableBackgroundRetirement = true;
    QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto source = f.completions->registerSource(KisCompletionDomain::HostLogical);
    const auto completion = f.completions->allocatePending(source);
    for (int i = 0; i < 2; ++i) {
        const auto request = f.store->acquireRead(key(), {}, cpu, KisPagePriority::Normal);
        auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
        f.store->release(std::move(lease), completion);
    }
    QCOMPARE(f.store->sessionStats().pendingLastUses, qsizetype(2));
    QCOMPARE(f.completions->sourceStatistics(source).readinessWaiters, quint64(0));
    QVERIFY(f.completions->complete(completion, KisCompletionStatus::Succeeded));
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().pendingLastUses, qsizetype(2));
    QVERIFY(f.store->acknowledgeLastUse(f.completions->verifyTerminal(completion)));
    QCOMPARE(f.store->sessionStats().pendingLastUses, qsizetype(0));
    QVERIFY(!f.store->acknowledgeLastUse(f.completions->verifyTerminal(completion)));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::lastUseAtMetadataCapacity_data()
{
    QTest::addColumn<int>("terminal");
    QTest::addColumn<int>("exit");
    for (int terminal = 0; terminal < 3; ++terminal)
        for (int exit = 0; exit < 2; ++exit)
            QTest::newRow(qPrintable(QStringLiteral("terminal-%1-exit-%2").arg(terminal).arg(exit)))
                << terminal << exit;
    QTest::newRow("stop-pending-retry") << 0 << 2;
}

void KisPageStoreCpuMutationTest::lastUseAtMetadataCapacity()
{
    QFETCH(int, terminal); QFETCH(int, exit);
    ReadTerminalFixture f; QVERIFY2(f.init(), qPrintable(f.error));
    const auto request = f.acquire(); auto lease = f.resolve(request); QVERIFY(lease.isValid());
    const auto source = f.physical.completions->registerSource(KisCompletionDomain::HostLogical);
    const auto ticket = f.physical.completions->allocatePending(source); QVERIFY(ticket.isValid());
    const size_t fillerBytes = size_t(f.limits.metadataArenaBytes - f.live());
    void *filler = kisAllocateMutationStorage(&f.budget, fillerBytes, 1);
    const auto free = qScopeGuard([&] { kisFreeMutationStorage(&f.budget, filler, fillerBytes, 1); });
    f.release(std::move(lease), ticket);
    QCOMPARE(f.physical.completions->sourceStatistics(source).readinessWaiters, quint64(0));
    QCOMPARE(f.snapshot().pendingLastUses, qsizetype(1));
    QTRY_VERIFY_WITH_TIMEOUT(f.snapshot().lastUsePasses >= 3, 5000);
    QCOMPARE(f.live(), f.limits.metadataArenaBytes);
    QCOMPARE(f.snapshot().pendingLastUses, qsizetype(1));
    if (exit == 2) {
        f.read.stopAutomaticWakeups(); f.read.waitForIdle();
        QVERIFY(f.physical.completions->complete(ticket, KisCompletionStatus::Succeeded));
        kisDrainPageStoreReclamation();
        QCOMPARE(f.snapshot().pendingLastUses, qsizetype(1));
        QCOMPARE(f.references.loadAcquire(), 1);
        KisPageReadCleanup cleanup(f.read); QMutexLocker lock(&f.mutex);
        f.read.acknowledgeCompletedLastUsesLocked(lock, cleanup);
    } else {
        if (exit == 1) {
            { QMutexLocker lock(&f.mutex); f.read.beginCloseLocked(); }
            f.read.waitForIdle();
            QCOMPARE(f.snapshot().pendingLastUses, qsizetype(1));
            { QMutexLocker lock(&f.mutex); f.read.cancelCloseLocked(); }
        }
        const auto status = terminal == 0 ? KisCompletionStatus::Succeeded
            : terminal == 1 ? KisCompletionStatus::Failed : KisCompletionStatus::Cancelled;
        QVERIFY(f.physical.completions->complete(ticket, status)); // No new request or explicit ack.
        QTRY_COMPARE_WITH_TIMEOUT(f.snapshot().pendingLastUses, qsizetype(0), 5000);
    }
    f.read.waitForIdle();
    // Hard-full history can still have a future retry. Its idle contract
    // excludes future deadlines; freeze dispatch before observing exit pins.
    f.history.stopAutomaticWakeups();
    { QMutexLocker lock(&f.mutex); f.history.waitForIdleLocked(); }
    f.retirement.stopAutomaticWakeups();
    f.retirement.waitForIdle();
    kisDrainPageStoreReclamation(); // Includes finished callbacks returning the original lifetime pins.
    const auto stats = f.snapshot();
    QCOMPARE(stats.pendingRequests, qsizetype(0));
    QCOMPARE(stats.activeLeases, qsizetype(0));
    QCOMPARE(f.activeCalls, qsizetype(0));
    QVERIFY(stats.maximumLastUsesPerPass <= 32);
    KisPageStateSnapshot page;
    QVERIFY(f.metadata.pageSnapshot(key(), &page));
    const auto *version = page.findVersion(f.replica.version); QVERIFY(version);
    const auto *resident = version->findReplica(f.replica); QVERIFY(resident);
    QVERIFY(resident->pendingLastUses.isEmpty());
    QVERIFY(resident->readLeases.isEmpty()); QCOMPARE(resident->pinCount, quint32(0));
    QCOMPARE(f.references.loadAcquire(), 1);
    QVERIFY(f.live() < f.limits.metadataArenaBytes); // Original record/control storage was actually freed.
}

void KisPageStoreCpuMutationTest::readStorageRefusalPreservesOriginalRequest()
{
    QFETCH(bool, resolve);
    ReadTerminalFixture f; QVERIFY2(f.init(), qPrintable(f.error));
    const auto original = f.acquire();
    QVERIFY(original.isValid());
    const size_t fillerBytes = size_t(f.limits.metadataArenaBytes - f.live());
    void *filler = kisAllocateMutationStorage(&f.budget, fillerBytes, 1);
    const auto free = qScopeGuard([&] { kisFreeMutationStorage(&f.budget, filler, fillerBytes, 1); });
    if (resolve) {
        QVERIFY(!f.resolve(original).isValid());
        QCOMPARE(f.physical.provider->readAccessCalls.load(), 0);
    } else {
        QVERIFY(!f.acquire().isValid());
    }
    QCOMPARE(f.snapshot().pendingRequests, qsizetype(1));
    QCOMPARE(f.snapshot().activeLeases, qsizetype(0));
    QCOMPARE(f.live(), f.limits.metadataArenaBytes);
    if (resolve) {
        kisFreeMutationStorage(&f.budget, std::exchange(filler, nullptr), fillerBytes, 1);
        auto lease = f.resolve(original); QVERIFY(lease.isValid());
        QCOMPARE(f.physical.provider->readAccessCalls.load(), 1);
        f.release(std::move(lease));
    } else {
        QVERIFY(f.cancel(original));
        const auto retry = f.acquire();
        QVERIFY(retry.isValid()); QVERIFY(f.cancel(retry));
    }
    f.read.waitForIdle();
    { QMutexLocker lock(&f.mutex); f.history.waitForIdleLocked(); }
    QCOMPARE(f.snapshot().pendingRequests, qsizetype(0));
    QCOMPARE(f.snapshot().activeLeases, qsizetype(0));
    QCOMPARE(f.activeCalls, qsizetype(0));
    QCOMPARE(f.references.loadAcquire(), 1);
}

void KisPageStoreCpuMutationTest::capturedReleaseKeepsOriginalFacts()
{
    QFETCH(int, exit);
    ReadTerminalFixture f;
    auto release = KisPageCapturedRelease::prepare(f.budget);
    QVERIFY2(f.init(release.get()), qPrintable(f.error));
    const quint64 storageBytes = sizeof(KisPageCapturedRelease) +
        release->versions.capacity() * sizeof(KisPageVersion);
    const size_t fillerBytes = size_t(f.limits.metadataArenaBytes - f.live());
    void *filler = kisAllocateMutationStorage(&f.budget, fillerBytes, 1);
    const auto free = qScopeGuard([&] { kisFreeMutationStorage(&f.budget, filler, fillerBytes, 1); });
    { QMutexLocker lock(&f.mutex); f.operational = false; }
    QVERIFY(!f.read.releaseCapturedView(std::move(release)));
    QTRY_VERIFY_WITH_TIMEOUT(f.snapshot().lastUsePasses >= 3, 5000);
    QCOMPARE(f.snapshot().pendingCapturedReleases, qsizetype(1));
    QCOMPARE(f.live(), f.limits.metadataArenaBytes);
    KisPageStateSnapshot page; QVERIFY(f.metadata.pageSnapshot(key(), &page));
    QVERIFY(page.findVersion(f.replica.version)->capturedReadViews.isEmpty());
    // Metadata protection already left. A retry of ReleaseCapturedVersion
    // would reject its stale token forever; only snapshot/handoff may continue.
    QCOMPARE(f.epochs.retainedSnapshotCount(), qsizetype(1));
    if (exit == 1) {
        { QMutexLocker lock(&f.mutex); f.read.beginCloseLocked(); }
        f.read.waitForIdle();
        { QMutexLocker lock(&f.mutex); f.operational = true; f.read.cancelCloseLocked(); }
    } else if (exit == 2) {
        f.read.stopAutomaticWakeups(); f.read.waitForIdle();
        { QMutexLocker lock(&f.mutex); f.operational = true; }
        kisDrainPageStoreReclamation();
        QCOMPARE(f.snapshot().pendingCapturedReleases, qsizetype(1));
        { QMutexLocker lock(&f.mutex); f.read.retryCapturedReleasesLocked(lock, true); }
    } else {
        { QMutexLocker lock(&f.mutex); f.operational = true; }
        // No new capture, completion, manual release or explicit wake.
    }
    QTRY_COMPARE_WITH_TIMEOUT(f.snapshot().pendingCapturedReleases, qsizetype(0), 5000);
    f.read.waitForIdle();
    { QMutexLocker lock(&f.mutex); f.history.waitForIdleLocked(); }
    QCOMPARE(f.epochs.retainedSnapshotCount(), qsizetype(0));
    QCOMPARE(f.snapshot().capturedViewReleases, quint64(1));
    QCOMPARE(f.activeCalls, qsizetype(0));
    kisDrainPageStoreReclamation(); // Idle precedes the executor's final lifetime callback.
    QCOMPARE(f.references.loadAcquire(), 1);
    QCOMPARE(f.live(), f.limits.metadataArenaBytes - storageBytes);
}

void KisPageStoreCpuMutationTest::captureStorageRefusalPrecedesProtection()
{
    Fixture f;
    KisPageBackingLimits limits; limits.metadataArenaBytes = 128 * 1024;
    QVERIFY(f.store->configureBackingLimits(limits));
    QVERIFY2(f.init(), qPrintable(f.error));
    const auto before = f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    std::vector<KisCapturedReadView> held;
    for (int i = 0; i < 2048; ++i) {
        auto view = f.store->captureReadView({}, &f.error);
        if (!view.isValid()) break;
        held.push_back(std::move(view));
    }
    QVERIFY(!held.empty() && held.size() < 2048);
    const auto snapshots = f.store->sessionStats().retainedSnapshots;
    QCOMPARE(snapshots, qsizetype(held.size()));
    QVERIFY(!f.store->captureReadView({}, &f.error).isValid());
    QCOMPARE(f.store->sessionStats().retainedSnapshots, snapshots);
    QVERIFY(!f.store->closeSession());
    held.pop_back();
    auto recovered = f.store->captureReadView(); QVERIFY(recovered.isValid());
    recovered = {}; held.clear();
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().retainedSnapshots, qsizetype(0));
    QCOMPARE(f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam, before);
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
}

void KisPageStoreCpuMutationTest::readProtectionBlockCleanup_data()
{
    QTest::addColumn<int>("route");
    QTest::newRow("release") << 0;
    QTest::newRow("cancel") << 1;
    QTest::newRow("failed-resolve-cancel") << 2;
    QTest::newRow("acknowledge") << 3;
    QTest::newRow("automatic-acknowledge") << 4;
    QTest::newRow("close-acknowledge") << 5;
    QTest::newRow("abort-prepared-requests") << 6;
}

void KisPageStoreCpuMutationTest::readProtectionBlockCleanup()
{
    QFETCH(int, route);
    Fixture f;
    f.provider->disableBackgroundRetirement = route != 4;
    QVERIFY(f.init());
    QVERIFY(f.fill(0x31));
    if (route == 4) {
        // This checks cleanup after storage admission. Prepare the cold
        // last-use wake before measuring the release interval's budget.
        const auto source = f.completions->registerSource(KisCompletionDomain::HostLogical);
        const auto ticket = f.completions->allocatePending(source);
        const auto request = f.store->acquireRead(key(), {}, cpu, KisPagePriority::Normal);
        auto lease = f.store->resolve(request, request.readiness);
        QVERIFY(lease.isValid());
        f.store->release(std::move(lease), ticket);
        QVERIFY(f.completions->complete(ticket, KisCompletionStatus::Succeeded));
        QVERIFY(f.store->waitForRetirementIdle());
        QTRY_COMPARE(f.store->sessionStats().pendingLastUses, qsizetype(0));
    }
    if (route == 2) {
        // Measure released protection blocks independently of the retained
        // accounting owner and reservation-slot cache prepared by resolve.
        const auto request = f.store->acquireRead(key(), {}, cpu, KisPagePriority::Normal);
        QVERIFY(request.isValid());
        f.provider->rejectReadAccess = true;
        QVERIFY(!f.store->resolve(request, request.readiness).isValid());
        f.provider->rejectReadAccess = false;
        QVERIFY(f.store->waitForRetirementIdle());
    }
    KisPageTransaction transaction;
    KisPageReadView view;
    if (route == 6) {
        transaction = f.store->beginCurrentTransaction();
        QVERIFY(KisPageStoreCpuSurfaceOps::fillRect(f.store.get(), {1}, transaction,
            QRect(0, 0, 64, 64), QByteArray(f.bpp, char(0x77)), &f.error));
        view.kind = KisPageReadViewKind::TransactionOverlay;
        view.transaction = transaction.id;
    }
    using Arena = KisShardSlotArena<KisMetadataOverflowNode, 16 * 1024>;
    const int count = int(3 * Arena::slotsPerBlock() + 7);
    QVector<KisReadRequest> requests;
    std::vector<KisReadLease> leases;
    for (int i = 0; i < count; ++i) {
        const auto request = f.store->acquireRead(key(), view, cpu, KisPagePriority::Normal);
        QVERIFY(request.isValid());
        if (route == 1 || route == 2 || route == 6) requests.append(request);
        else {
            auto lease = f.store->resolve(request, request.readiness);
            QVERIFY(lease.isValid());
            leases.push_back(std::move(lease));
        }
    }
    const auto charged = [&] {
        return f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const quint64 before = charged();
    if (route == 0) {
        for (auto &lease : leases) f.store->release(std::move(lease));
    } else if (route == 1) {
        for (const auto &request : requests) QVERIFY(f.store->cancel(request));
    } else if (route == 2) {
        f.provider->rejectReadAccess = true;
        for (const auto &request : requests) QVERIFY(!f.store->resolve(request, request.readiness).isValid());
        f.provider->rejectReadAccess = false;
    } else if (route == 6) {
        QVERIFY(f.store->abort(transaction));
    } else {
        const auto ticket = f.completions->allocatePending(
            f.completions->registerSource(KisCompletionDomain::HostLogical));
        for (auto &lease : leases) f.store->release(std::move(lease), ticket);
        QCOMPARE(f.store->sessionStats().pendingLastUses, qsizetype(count));
        if (route == 4) {
            QVERIFY(charged() > before); // Actual automatic callback/subscriber storage.
            QCOMPARE(f.completions->sourceStatistics(ticket.source()).readinessWaiters, quint64(count));
        } else QCOMPARE(charged(), before);
        QVERIFY(f.completions->complete(ticket, KisCompletionStatus::Succeeded));
        if (route == 3) QVERIFY(f.store->acknowledgeLastUse(f.completions->verifyTerminal(ticket)));
        else if (route == 5) QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    }
    QVERIFY(f.store->waitForRetirementIdle());
    const auto stats = f.store->sessionStats();
    QCOMPARE(stats.pendingRequests, qsizetype(0));
    QCOMPARE(stats.activeReadLeases, qsizetype(0));
    QCOMPARE(stats.pendingLastUses, qsizetype(0));
    QCOMPARE(stats.activeProviderCalls, qsizetype(0));
    QCOMPARE(stats.scheduledReclamationJobs, qsizetype(0));
    QVERIFY(charged() <= before - 3 * Arena::blockByteSize());
    if (route != 5) QCOMPARE(f.pixel(), QByteArray(f.bpp, char(0x31)));
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
}

void KisPageStoreCpuMutationTest::oldTransactionBaseReleaseReclaimsDeferredHistory()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    auto view = f.store->captureReadView();
    const auto oldTransaction = f.store->beginCurrentTransaction();
    QVERIFY(oldTransaction.isValid());
    QVERIFY(f.fill(0x77));
    view = {}; // the old transaction still owns the same root
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(4));
    QVERIFY(f.store->abort(oldTransaction));
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(2));
    QCOMPARE(f.store->sessionStats().deferredHistoricalPages, qsizetype(0));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(2 * 64 * 64 * f.bpp));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::firstRetirementRunsOffThreadAndCloseDrains()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    std::atomic<bool> wrongThread{false};
    f.provider->beforeRetire = [&] {
        if (!kisOnPageStoreReclamationThread()) wrongThread = true;
        // Proves provider work has neither the owner nor queue gate held.
        f.store->sessionStats();
    };
    QVERIFY(f.fill(0x77));
    QVERIFY(!wrongThread.load());
    QCOMPARE(f.provider->retireCalls.load(), 2);
    QCOMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(0));
    f.provider->beforeRetire = {};
    // Close also handles not-yet-started first retirements and waits any pass
    // already in flight before enumerating shutdown replicas.
    QVERIFY(f.fill(0x88, false));
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
    QVERIFY(!f.store->sessionStats().hasOutstandingCapabilities());
}

void KisPageStoreCpuMutationTest::blockedRetirementDoesNotBlockPublication()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    QSemaphore entered, resume;
    std::atomic<int> calls{0};
    f.provider->beforeRetire = [&] {
        if (++calls == 1) { entered.release(); resume.acquire(); }
        f.store->isOperational();
    };
    auto unblock = qScopeGuard([&] { resume.release(); f.store->waitForRetirementIdle(); });
    QVERIFY(f.fill(0x77, false));
    QVERIFY(entered.tryAcquire(1, 5000));
    // Publication returned while physical retirement is still blocked. Both
    // queued and in-flight replicas remain counted as debt, including bytes.
    const auto stats = f.store->sessionStats();
    QCOMPARE(stats.pendingRetiredReplicas, qsizetype(2));
    QCOMPARE(stats.activeRetirementReplicas, qsizetype(2));
    QCOMPARE(stats.pendingRetiredBytes, quint64(2 * 64 * 64 * f.bpp));
    QCOMPARE(stats.peakRetiredReplicas, qsizetype(2));
    QCOMPARE(stats.peakRetiredBytes, quint64(2 * 64 * 64 * f.bpp));
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x77));
    resume.release();
    QVERIFY(f.store->waitForRetirementIdle());
    f.provider->beforeRetire = {};
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::commitValidatesOriginalDelta()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    auto mutation = f.begin(tx);
    for (int x : {0, 2}) {
        auto write = mutation.beginWrite(key(x)); QVERIFY(write.isValid());
        static_cast<quint8 *>(write.data())[0] = 0x72;
    }
    QVERIFY(mutation.seal());
    QVERIFY(f.remove(tx, key(1)));
    QVERIFY(f.store->stageSurfaceDefaultPixel(tx, {1}, QByteArray(f.bpp, char(0x44)), &f.error));
    const auto original = f.store->preparedPages(tx);
    QCOMPARE(original.proofs.size(), qsizetype(2));
    QCOMPARE(original.surfaceChanges.size(), qsizetype(1));
    QCOMPARE(original.removedPages.size(), qsizetype(1));
    const auto preparations = f.store->publicationStatistics().preparedMutationCommits;
    const auto reject = [&](const KisPreparedPageSet &supplied) {
        QVERIFY(!f.store->commit(tx, supplied).isValid());
        QCOMPARE(f.store->sessionStats().activeTransactions, qsizetype(1));
        QCOMPARE(f.store->sessionStats().sealedPreparedProofs, qsizetype(2));
        QCOMPARE(f.store->publicationStatistics().preparedMutationCommits, preparations);
    };
    auto supplied = original;
    supplied.proofs[1] = supplied.proofs[0]; reject(supplied);
    supplied = original; supplied.proofs.removeLast(); reject(supplied);
    supplied = original; ++supplied.proofs[0].providerValidationStamp; reject(supplied);
    supplied = original; supplied.surfaceChanges.append(original.surfaceChanges.first()); reject(supplied);
    supplied = original; supplied.surfaceChanges.clear(); reject(supplied);
    supplied = original;
    supplied.surfaceChanges[0].after.contentExtent.adjust(0, 0, 64, 0);
    ++supplied.surfaceChanges[0].after.extentRevision;
    QVERIFY(supplied.isValid()); reject(supplied);
    supplied = original; supplied.removedPages[0] = key(3);
    QVERIFY(supplied.isValid()); reject(supplied);
    supplied = original; supplied.removedPages.append(original.removedPages.first()); reject(supplied);
    // Exact ownership is independent of the caller's export order. Every
    // refused input above left these original facts and proofs available.
    supplied = original; std::reverse(supplied.proofs.begin(), supplied.proofs.end());
    QVERIFY(f.store->commit(tx, supplied).isValid());
    QCOMPARE(quint8(f.pixel({}, 0)[0]), quint8(0x72));
    QCOMPARE(f.pixel({}, 1), QByteArray(f.bpp, char(0x44)));
    QCOMPARE(quint8(f.pixel({}, 2)[0]), quint8(0x72));
    QCOMPARE(f.store->sessionStats().sealedPreparedProofs, qsizetype(0));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::publicationInputRefusalKeepsOriginalTransaction()
{
    QFETCH(bool, late);
    QFETCH(bool, advance);
    KisPageBackingLimits limits; limits.metadataArenaBytes = 4 * 1024 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    Fixture f; f.provider->disableBackgroundRetirement = true;
    QVERIFY(f.store->configureSharedNonPayloadBudget(parent, &f.error));
    QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    auto mutation = f.begin(tx);
    for (int x : {0, 2}) {
        auto write = mutation.beginWrite(key(x)); QVERIFY(write.isValid());
        static_cast<quint8 *>(write.data())[0] = 0x72;
    }
    QVERIFY(mutation.seal()); mutation = {};
    QVERIFY(f.remove(tx, key(1)));
    QVERIFY(f.store->stageSurfaceDefaultPixel(tx, {1}, QByteArray(f.bpp, char(0x44)), &f.error));
    const auto original = f.store->preparedPages(tx);
    const auto preparations = f.store->publicationStatistics().preparedMutationCommits;
    const auto live = [&] { return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const auto before = live();
    size_t fillerBytes = 0;
    void *filler = nullptr;
    const auto fillCapacity = [&] {
        fillerBytes = size_t(limits.metadataArenaBytes - live());
        filler = kisAllocateMutationStorage(parent.get(), fillerBytes, 1);
        QCOMPARE(live(), limits.metadataArenaBytes);
    };
    if (!late) fillCapacity();
    bool validated = false;
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        validated |= phase == KisPageStoreDiagnosticPhase::CommitProviderValidation;
        if (late && !filler && phase == KisPageStoreDiagnosticPhase::CommitCompletion)
            fillCapacity(); // Metadata and epoch candidates already belong to the original Data.
    });
    {
        const auto freeFiller = qScopeGuard([&] { kisFreeMutationStorage(parent.get(), filler, fillerBytes, 1); });
        QVERIFY(!f.store->commit(tx, original).isValid());
        QCOMPARE(validated, late);
        QVERIFY(filler);
        QCOMPARE(f.store->sessionStats().activeTransactions, qsizetype(1));
        QCOMPARE(f.store->sessionStats().sealedPreparedProofs, qsizetype(2));
        QCOMPARE(f.store->publicationStatistics().preparedMutationCommits, preparations);
        if (late) QVERIFY(live() < limits.metadataArenaBytes); // Real prepared candidate capacity was freed.
        else QCOMPARE(live(), limits.metadataArenaBytes);
    }
    if (!late) QCOMPARE(live(), before);
    filler = nullptr; late = false; // Retry has the released capacity, with no new blocker.
    auto continuation = f.begin(tx); QVERIFY(continuation.isActive()); QVERIFY(continuation.cancel());
    auto retry = original;
    if (advance) {
        auto next = f.begin(tx);
        auto write = next.beginWrite(key(0)); QVERIFY(write.isValid());
        static_cast<quint8 *>(write.data())[0] = 0x73;
        write = {}; QVERIFY(next.seal());
        retry = f.store->preparedPages(tx);
    }
    QVERIFY(f.store->commit(tx, retry).isValid()); QVERIFY(validated);
    QCOMPARE(quint8(f.pixel({}, 0)[0]), quint8(advance ? 0x73 : 0x72));
    QCOMPARE(f.pixel({}, 1), QByteArray(f.bpp, char(0x44)));
    QCOMPARE(quint8(f.pixel({}, 2)[0]), quint8(0x72));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::restorationUsesOriginalTargets()
{
    QFETCH(bool, delta);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto retained = f.store->captureRetainedEpoch(); QVERIFY(retained.isValid());
    QVERIFY(f.fill(0x72)); QVERIFY(f.setDefault(0x55));
    for (int x : {2, 3}) QCOMPARE(f.pixel({}, x), QByteArray(f.bpp, char(0x55)));
    const auto before = f.store->publicationStatistics();
    int attempts = 0;
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase == KisPageStoreDiagnosticPhase::RestorePublishOwnerWait && ++attempts == 1)
            QCOMPARE(f.pixel({}, 4), QByteArray(f.bpp, char(0x55))); // New directory key, outside the original prefix.
    });
    const KisPageKeyStorage changed{key(1), {}, key(0), key(1), key(0)};
    const auto restored = delta ? f.store->restoreRetainedEpochDelta(retained, changed)
                               : f.store->restoreRetainedEpoch(retained);
    QVERIFY(restored.isValid()); QCOMPARE(attempts, 2);
    QCOMPARE(f.store->publicationStatistics().restoreDirectoryRepreparations,
             before.restoreDirectoryRepreparations + 1);
    for (int x : {0, 1}) QCOMPARE(f.pixel({}, x), QByteArray(f.bpp, char(0x31)));
    for (int x : {2, 3, 4}) QCOMPARE(f.pixel({}, x), QByteArray(f.bpp, char(0x2a)));
    QVERIFY(f.store->releaseSnapshot(retained.token)); QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::commitPreparationClaimsAndLateCapture()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    auto segment = f.begin(tx);
    auto write = segment.beginWrite(key()); QVERIFY(write.isValid());
    static_cast<quint8 *>(write.data())[0] = 0x72;
    write = {}; QVERIFY(segment.seal());
    const auto prepared = f.store->preparedPages(tx);
    KisCapturedReadView oldView;
    KisCpuReadGuard oldGuard;
    int attempts = 0;
    bool claimsHeld = false;
    bool otherTransactionWorks = false;
    bool prepareUnlocked = false;
    bool cleanupUnlocked = false;
    bool providerValidationUnlocked = false;
    bool validationObserved = false;
    f.provider->beforeValidate = [&] {
        if (validationObserved) return;
        validationObserved = true;
        auto probe = f.store->captureReadView();
        providerValidationUnlocked = probe.isValid();
    };
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase == KisPageStoreDiagnosticPhase::CommitEpochPrepare) {
            // This takes owner. It would deadlock if root construction still
            // ran inside the owner critical section.
            prepareUnlocked = f.store->isOperational();
        }
        if (phase == KisPageStoreDiagnosticPhase::CommitCleanup) {
            cleanupUnlocked = f.store->isOperational();
        }
        if (phase != KisPageStoreDiagnosticPhase::CommitPublishOwnerWait || ++attempts != 1) return;
        oldView = f.store->captureReadView();
        oldGuard = oldView.readResidentPage(key());
        const auto generic = f.store->acquireWrite(tx, key(3), cpu,
            KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
        claimsHeld = !f.store->abort(tx) && !f.begin(tx).isActive() && !generic.isValid() &&
            !f.remove(tx, key()) &&
            !f.store->commit(tx, prepared).isValid() && !f.store->closeSession();
        KisSurfaceEpochState surface;
        claimsHeld = claimsHeld && f.store->resolveSurfaceState({1}, {}, &surface) &&
            !f.store->stageSurfaceMetadata(tx, surface);
        const auto other = f.store->beginCurrentTransaction();
        auto mutation = f.begin(other);
        auto otherWrite = mutation.beginWrite(key(3));
        otherTransactionWorks = otherWrite.isValid();
        otherWrite = {};
        otherTransactionWorks = otherTransactionWorks && mutation.cancel() && f.store->abort(other);
    });
    const auto committed = f.store->commit(tx, prepared);
    f.provider->beforeValidate = {};
    QVERIFY(committed.isValid()); QVERIFY(claimsHeld); QVERIFY(otherTransactionWorks); QVERIFY(prepareUnlocked);
    QVERIFY(cleanupUnlocked);
    QVERIFY(providerValidationUnlocked);
    QVERIFY(attempts >= 1);
    QCOMPARE(f.store->publicationStatistics().activeCommitPreparations, qsizetype(0));
    QVERIFY(oldGuard.isValid()); QCOMPARE(static_cast<const quint8 *>(oldGuard.data())[0], quint8(0x31));
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x72));
    oldGuard = {}; oldView = {};
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::publicationCleanupUsesBoundedBackgroundPasses()
{
    Fixture f; QVERIFY(f.init());
    constexpr int pages = 17;
    const auto tx = f.store->beginCurrentTransaction(); QVERIFY(tx.isValid());
    {
        auto segment = f.begin(tx);
        for (int x = 0; x < pages; ++x) {
            auto write = segment.beginWrite(key(x), &f.error);
            QVERIFY2(write.isValid(), qPrintable(f.error));
            static_cast<quint8 *>(write.data())[0] = quint8(0x20 + x);
        }
        QVERIFY2(segment.seal(&f.error), qPrintable(f.error));
    }

    QSemaphore entered, resume;
    kisSchedulePageStoreReclamation([&] { entered.release(); resume.acquire(); });
    auto unblock = qScopeGuard([&] { resume.release(); kisDrainPageStoreReclamation(); });
    QVERIFY(entered.tryAcquire(1, 10000));

    {
        auto segment = f.begin(tx);
        for (int x = 0; x < pages; ++x) {
            auto write = segment.beginWrite(key(x), &f.error);
            QVERIFY2(write.isValid(), qPrintable(f.error));
            static_cast<quint8 *>(write.data())[0] = quint8(0x60 + x);
        }
        QVERIFY2(segment.seal(&f.error), qPrintable(f.error));
    }
    auto statistics = f.store->publicationStatistics();
    QCOMPARE(statistics.metadataCleanupDeferredCandidates, quint64(1));
    QCOMPARE(statistics.metadataCleanupPendingCandidates, qsizetype(1));
    QCOMPARE(statistics.metadataCleanupPendingUnits, qsizetype(pages));
    QCOMPARE(statistics.metadataCleanupPasses, quint64(0));

    const auto committed = f.store->commit(tx, f.store->preparedPages(tx));
    QVERIFY(committed.isValid());
    statistics = f.store->publicationStatistics();
    QCOMPARE(statistics.metadataCleanupDeferredCandidates, quint64(2));
    QCOMPARE(statistics.metadataCleanupPendingCandidates, qsizetype(2));
    QCOMPARE(statistics.metadataCleanupPeakPendingCandidates, qsizetype(2));
    QCOMPARE(statistics.metadataCleanupPendingUnits, qsizetype(2 * pages));
    QCOMPARE(statistics.metadataCleanupPeakPendingUnits, qsizetype(2 * pages));
    QCOMPARE(quint8(f.pixel({}, pages - 1)[0]), quint8(0x60 + pages - 1));

    resume.release(); unblock.dismiss(); kisDrainPageStoreReclamation();
    statistics = f.store->publicationStatistics();
    QCOMPARE(statistics.metadataCleanupPendingCandidates, qsizetype(0));
    QCOMPARE(statistics.metadataCleanupCompletedCandidates, quint64(2));
    QCOMPARE(statistics.metadataCleanupPendingUnits, qsizetype(0));
    QCOMPARE(statistics.metadataCleanupUnits, quint64(2 * pages));
    QVERIFY(statistics.metadataCleanupPasses >= 4);
    QVERIFY(statistics.metadataCleanupMaximumUnitsPerPass > 0);
    QVERIFY(statistics.metadataCleanupMaximumUnitsPerPass <= 16);
    QVERIFY(statistics.metadataCleanupNanoseconds >= statistics.metadataCleanupMaximumPassNanoseconds);
    QVERIFY(statistics.metadataCleanupQueueNanoseconds >= statistics.metadataCleanupMaximumQueueNanoseconds);
    QVERIFY(statistics.metadataCleanupMaximumQueueNanoseconds > 0);
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::hostLogicalCompletionIsPreparedOnce()
{
    Fixture f; QVERIFY(f.init());
    quint64 hostSource = 0;
    for (quint64 source = 1; source <= 8; ++source) {
        const auto statistics = f.completions->sourceStatistics(source);
        if (statistics.knownSource && statistics.allocatedTickets == 1 &&
            statistics.terminalTickets == 1 && statistics.pendingTickets == 0) {
            hostSource = source;
            break;
        }
    }
    QVERIFY(hostSource != 0);
    const auto prepared = f.completions->sourceStatistics(hostSource);
    QCOMPARE(prepared.allocatedTickets, quint64(1));
    QCOMPARE(prepared.terminalTickets, quint64(1));
    QCOMPARE(prepared.pendingTickets, quint64(0));
    QCOMPARE(prepared.storageRecords, quint64(1));

    QVERIFY2(f.fill(0x31), qPrintable(f.error));
    QVERIFY2(f.fill(0x61), qPrintable(f.error));
    QVERIFY2(f.setDefault(0x45), qPrintable(f.error));
    const auto after = f.completions->sourceStatistics(hostSource);
    QCOMPARE(after.allocatedTickets, prepared.allocatedTickets);
    QCOMPARE(after.terminalTickets, prepared.terminalTickets);
    QCOMPARE(after.pendingTickets, quint64(0));
    QCOMPARE(after.storageRecords, prepared.storageRecords);
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::commitRevalidatesReadLeaseRevision()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    auto retained = f.store->captureReadView(); // select non-fused publication
    const auto tx = f.store->beginCurrentTransaction();
    auto mutation = f.begin(tx); auto write = mutation.beginWrite(key());
    QVERIFY(write.isValid()); static_cast<quint8 *>(write.data())[0] = 0x73;
    write = {}; QVERIFY(mutation.seal());
    std::unique_ptr<KisReadLease> concurrentRead;
    int attempts = 0;
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::CommitPublishOwnerWait || ++attempts != 1) return;
        const auto request = f.store->acquireRead(key(), {}, cpu, KisPagePriority::Normal);
        concurrentRead = std::make_unique<KisReadLease>(f.store->resolve(request, request.readiness));
    });
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QCOMPARE(attempts, 2); QVERIFY(concurrentRead && concurrentRead->isValid());
    QCOMPARE(f.store->publicationStatistics().metadataRepreparations, quint64(1));
    QCOMPARE(static_cast<const quint8 *>(concurrentRead->cpuData())[0], quint8(0x31));
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x73));
    f.store->release(std::move(*concurrentRead)); concurrentRead.reset(); retained = {};
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::publicationWhileCapturedReadersChange()
{
    Fixture f; QVERIFY(f.init());
    constexpr int pages = 16;
    const auto fill = [&](quint8 value) {
        const auto tx = f.store->beginCurrentTransaction();
        auto mutation = f.begin(tx);
        for (int x = 0; x < pages; ++x) {
            auto write = mutation.beginWrite(key(x));
            if (!write.isValid()) return false;
            static_cast<quint8 *>(write.data())[0] = value;
        }
        return mutation.seal() && f.store->commit(tx, f.store->preparedPages(tx)).isValid();
    };
    QVERIFY(fill(0x31));
    const auto before = f.store->captureRetainedEpoch(); QVERIFY(before.isValid());
    std::atomic<bool> stop{false}, coherent{true};
    std::atomic<int> captures{0};
    std::thread reader([&] {
        while (!stop.load()) {
            auto view = f.store->captureReadView();
            auto first = view.readResidentPage(key());
            auto last = view.readResidentPage(key(pages - 1));
            if (!first.isValid() || !last.isValid()
                || *static_cast<const quint8 *>(first.data()) != *static_cast<const quint8 *>(last.data()))
                coherent = false;
            ++captures;
        }
    });
    auto join = qScopeGuard([&] { stop = true; reader.join(); });
    while (captures.load() == 0) std::this_thread::yield();
    // Exercise both optimistic preparation loops, including retained-history
    // collection racing a candidate before it reaches installation.
    for (int i = 0; i < 64; ++i) {
        QVERIFY(fill(quint8(0x40 + i)));
        QVERIFY(f.store->restoreRetainedEpoch(before).isValid());
    }
    QVERIFY(coherent.load());
    stop = true; reader.join(); join.dismiss();
    QVERIFY(f.store->releaseSnapshot(before.token));
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
}

void KisPageStoreCpuMutationTest::commitRebasesAfterDisjointPublication()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto first = f.store->beginCurrentTransaction();
    const auto second = f.store->beginCurrentTransaction();
    for (const auto &tx : {first, second}) {
        auto mutation = f.begin(tx);
        auto write = mutation.beginWrite(key(tx.id == first.id ? 0 : 1));
        QVERIFY(write.isValid()); static_cast<quint8 *>(write.data())[0] = tx.id == first.id ? 0x51 : 0x52;
        write = {}; QVERIFY(mutation.seal());
    }
    int attempts = 0;
    KisImageEpochCommitTicket secondCommit;
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase == KisPageStoreDiagnosticPhase::CommitPublishOwnerWait && ++attempts == 1)
            secondCommit = f.store->commit(second, f.store->preparedPages(second));
    });
    const auto firstCommit = f.store->commit(first, f.store->preparedPages(first));
    QVERIFY(secondCommit.isValid()); QVERIFY(firstCommit.isValid()); QCOMPARE(attempts, 2);
    QCOMPARE(f.store->publicationStatistics().rootRebases, quint64(1));
    QVERIFY(firstCommit.epoch.value > secondCommit.epoch.value);
    QCOMPARE(quint8(f.pixel({}, 0)[0]), quint8(0x51)); QCOMPARE(quint8(f.pixel({}, 1)[0]), quint8(0x52));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::commitRejectsConflictingPublicationAtomically()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto first = f.store->beginCurrentTransaction();
    const auto second = f.store->beginCurrentTransaction();
    for (const auto &tx : {first, second}) {
        auto mutation = f.begin(tx);
        for (int x = 0; x < (tx.id == first.id ? 2 : 1); ++x) {
            auto write = mutation.beginWrite(key(x)); QVERIFY(write.isValid());
            static_cast<quint8 *>(write.data())[0] = tx.id == first.id ? 0x51 : 0x52;
        }
        QVERIFY(mutation.seal());
    }
    bool injected = false;
    KisImageEpochCommitTicket secondCommit;
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase == KisPageStoreDiagnosticPhase::CommitPublishOwnerWait && !injected) {
            injected = true;
            secondCommit = f.store->commit(second, f.store->preparedPages(second));
        }
    });
    QVERIFY(!f.store->commit(first, f.store->preparedPages(first)).isValid());
    QVERIFY(secondCommit.isValid());
    QCOMPARE(quint8(f.pixel({}, 0)[0]), quint8(0x52)); QCOMPARE(quint8(f.pixel({}, 1)[0]), quint8(0x31));
    QVERIFY(f.store->abort(first)); QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::genericResolveStorageFailure_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("recoverable"); QTest::addColumn<int>("terminal");
    for (int bpp : {1, 4, 8, 16}) for (bool recoverable : {false, true}) for (int terminal = 0; terminal < 3; ++terminal)
        QTest::newRow(qPrintable(QStringLiteral("bpp%1-recoverable%2-terminal%3").arg(bpp).arg(recoverable).arg(terminal)))
            << bpp << recoverable << terminal;
}

void KisPageStoreCpuMutationTest::genericResolveStorageFailure()
{
    QFETCH(int, bpp); QFETCH(bool, recoverable); QFETCH(int, terminal);
    Fixture f; QVERIFY(f.init(bpp, recoverable ? 2 : 1));
    auto before = f.store->captureReadView(); QVERIFY(before.isValid());
    const auto tx = f.store->beginCurrentTransaction();
    const auto request = f.store->acquireWrite(tx, key(), cpu, KisPageWriteMode::PreserveContents, KisPagePriority::Normal);
    QVERIFY(request.isValid());
    QCOMPARE(f.provider->lastTarget.isValid(), !recoverable);
    bool injected = false;
    f.provider->beforeWriteResolve = [&](const KisReplicaHandle &) { injected = true; throw std::bad_alloc(); };
    QVERIFY(!f.store->resolve(request, request.readiness).isValid());
    f.provider->beforeWriteResolve = {}; QVERIFY(injected);
    QCOMPARE(f.store->sessionStats().pendingRequests, qsizetype(1));
    QCOMPARE(f.store->sessionStats().activeWriteLeases, qsizetype(0));
    QCOMPARE(f.store->sessionStats().activeControlWritePages, qsizetype(1));
    QVERIFY(!f.store->commit(tx, {}).isValid()); QVERIFY(!f.store->closeSession(&f.error));
    QCOMPARE(f.pixel(), QByteArray(bpp, char(0x31)));
    if (terminal == 0) {
        auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
        QCOMPARE(static_cast<const quint8 *>(lease.cpuData())[0], quint8(0x31));
        std::memset(lease.cpuData(), 0x72, size_t(lease.byteSize()));
        QVERIFY(f.store->publishHostWrite(std::move(lease)).isValid());
        QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    } else {
        if (terminal == 1) QVERIFY(f.store->cancel(request));
        QVERIFY(f.store->abort(tx));
    }
    QVERIFY(!f.store->resolve(request, request.readiness).isValid()); QVERIFY(!f.store->cancel(request));
    QCOMPARE(f.store->sessionStats().pendingRequests, qsizetype(0));
    QCOMPARE(f.store->sessionStats().activeWriteLeases, qsizetype(0));
    QCOMPARE(f.store->sessionStats().activeControlWritePages, qsizetype(0));
    QCOMPARE(f.pixel(), QByteArray(bpp, char(terminal == 0 ? 0x72 : 0x31)));
    auto old = before.readResidentPage(key()); QVERIFY(old.isValid());
    QCOMPARE(static_cast<const quint8 *>(old.data())[0], quint8(0x31));
    old = {}; before = {};
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::genericResolveConcurrentIndexGrowth_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("recoverable");
    for (int bpp : {1, 4, 8, 16}) for (bool recoverable : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("bpp%1-recoverable%2").arg(bpp).arg(recoverable))) << bpp << recoverable;
}

void KisPageStoreCpuMutationTest::genericResolveConcurrentIndexGrowth()
{
    QFETCH(int, bpp); QFETCH(bool, recoverable);
    Fixture f; QVERIFY(f.init(bpp, recoverable ? 2 : 1));
    const auto tx = f.store->beginCurrentTransaction();
    const auto request = f.store->acquireWrite(tx, key(), cpu, KisPageWriteMode::PreserveContents, KisPagePriority::Normal);
    QVERIFY(request.isValid()); QCOMPARE(f.provider->lastTarget.isValid(), !recoverable);
    QSemaphore entered, resume;
    f.provider->afterWriteResolve = [&](const KisReplicaHandle &replica) {
        if (replica.version.key == key()) { entered.release(); resume.acquire(); }
    };
    std::optional<KisWriteLease> target;
    std::thread writer([&] { target.emplace(f.store->resolve(request, request.readiness)); });
    const bool started = entered.tryAcquire(1, 10000);
    bool correct = started;
    std::vector<KisWriteRequest> requests;
    std::vector<KisWriteLease> leases;
    if (started) {
        for (int i = 1; i <= 257; ++i) {
            const auto next = f.store->acquireWrite(tx, key(i), cpu, KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
            correct = next.isValid() && correct; requests.push_back(next);
        }
        correct = (f.store->sessionStats().pendingRequests == 258) && correct;
        for (const auto &next : requests) {
            auto lease = f.store->resolve(next, next.readiness);
            correct = lease.isValid() && correct; if (lease.isValid()) leases.push_back(std::move(lease));
        }
        const auto stats = f.store->sessionStats();
        correct = (stats.pendingRequests == 1 && stats.activeWriteLeases == 257 && stats.activeControlWritePages == 258) && correct;
        correct = !f.store->cancel(request) && correct;
        correct = !f.store->abort(tx) && correct;
        correct = !f.store->commit(tx, {}).isValid() && correct;
        correct = !f.store->closeSession(&f.error) && correct;
    }
    resume.release(); writer.join(); f.provider->afterWriteResolve = {};
    QVERIFY(correct); QVERIFY(target && target->isValid());
    QCOMPARE(f.store->sessionStats().pendingRequests, qsizetype(0));
    QCOMPARE(f.store->sessionStats().activeWriteLeases, qsizetype(258));
    QCOMPARE(static_cast<const quint8 *>(target->cpuData())[0], quint8(0x31));
    for (auto &lease : leases) {
        QCOMPARE(static_cast<const quint8 *>(lease.cpuData())[0], quint8(0x2a));
        f.store->cancel(std::move(lease));
    }
    std::memset(target->cpuData(), 0x64, size_t(target->byteSize()));
    QVERIFY(f.store->publishHostWrite(std::move(*target)).isValid());
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QCOMPARE(f.pixel(), QByteArray(bpp, char(0x64)));
    QCOMPARE(f.store->sessionStats().activeControlWritePages, qsizetype(0));
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::genericPreparationStorageFailure_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("point");
    for (int bpp : {1, 4, 8, 16}) for (int point = 0; point < 3; ++point)
        QTest::newRow(qPrintable(QStringLiteral("bpp%1-point%2").arg(bpp).arg(point))) << bpp << point;
}

void KisPageStoreCpuMutationTest::genericPreparationStorageFailure()
{
    QFETCH(int, bpp); QFETCH(int, point);
    Fixture f; QVERIFY(f.init(bpp, point == 0 ? 2 : 1));
    f.provider->disableSynchronousWriteCopy = true;
    bool injected = false;
    const auto fail = [&] { injected = true; throw std::bad_alloc(); };
    if (point == 0) f.provider->beforeBinding = [&](const KisReplicaHandle &) { fail(); };
    else if (point == 1) f.provider->beforePrepareWrite = fail;
    else f.provider->beforeTransfer = fail;
    const auto tx = f.store->beginCurrentTransaction();
    const auto request = f.store->acquireWrite(tx, key(), cpu, KisPageWriteMode::PreserveContents, KisPagePriority::Normal);
    f.provider->beforeBinding = {}; f.provider->beforePrepareWrite = {}; f.provider->beforeTransfer = {};
    QVERIFY(injected); QVERIFY(!request.isValid()); QCOMPARE(request.status, KisPageRequestStatus::Failed);
    QCOMPARE(f.store->sessionStats().pendingRequests, qsizetype(0));
    QCOMPARE(f.store->sessionStats().activeWriteLeases, qsizetype(0));
    QCOMPARE(f.store->sessionStats().activeControlWritePages, qsizetype(0));
    QCOMPARE(f.pixel(), QByteArray(bpp, char(0x31)));
    const auto retry = f.store->acquireWrite(tx, key(), cpu, KisPageWriteMode::PreserveContents, KisPagePriority::Normal);
    QVERIFY(retry.isValid()); auto lease = f.store->resolve(retry, retry.readiness); QVERIFY(lease.isValid());
    QCOMPARE(static_cast<const quint8 *>(lease.cpuData())[0], quint8(0x31));
    f.store->cancel(std::move(lease)); QVERIFY(f.store->abort(tx));
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::genericInitializationFailureRetainsAbortDebt()
{
    QFETCH(int, bpp);
    Fixture f; KisPageBackingLimits limits; limits.retirementDebtBytes = 2 * 64 * 64 * bpp;
    QVERIFY(f.store->configureBackingLimits(limits)); QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    f.provider->disableSynchronousWriteCopy = true;
    const auto tx = f.store->beginCurrentTransaction();
    bool injected = false, debtReady = false;
    QString debtFailure;
    f.provider->beforeTransfer = [&] {
        // Fill retirement capacity only after this request obtained its writer.
        // Otherwise admission correctly rejects it before initialization.
        f.provider->disableSynchronousWriteCopy = false;
        const auto other = f.store->beginCurrentTransaction();
        auto mutation = f.begin(other); auto guard = mutation.beginWrite(key(1), &debtFailure);
        const bool valid = guard.isValid(); guard = {};
        f.provider->rejectRetire = true;
        const bool cancelled = mutation.cancel();
        const bool aborted = f.store->abort(other);
        const bool idle = f.store->waitForRetirementIdle();
        debtReady = valid && cancelled && aborted && idle;
        if (!debtReady) debtFailure += QStringLiteral(" valid=%1 cancel=%2 abort=%3 idle=%4")
            .arg(valid).arg(cancelled).arg(aborted).arg(idle);
        f.provider->disableSynchronousWriteCopy = true;
        injected = true; throw std::bad_alloc();
    };
    const auto request = f.store->acquireWrite(tx, key(), cpu, KisPageWriteMode::PreserveContents, KisPagePriority::Normal);
    f.provider->beforeTransfer = {};
    QVERIFY(injected); QVERIFY2(debtReady, qPrintable(debtFailure)); QVERIFY(!request.isValid()); QCOMPARE(request.status, KisPageRequestStatus::Failed);
    QCOMPARE(f.store->sessionStats().pendingRequests, qsizetype(1));
    QCOMPARE(f.store->sessionStats().activeControlWritePages, qsizetype(1));
    // acquireWrite returned its fallback reservation while preserving the
    // original pending record. Fill that newly available slot before abort.
    f.provider->disableSynchronousWriteCopy = false;
    const auto blocker = f.store->beginCurrentTransaction();
    auto blocked = f.begin(blocker); auto blockGuard = blocked.beginWrite(key(2));
    QVERIFY(blockGuard.isValid()); blockGuard = {};
    QVERIFY(blocked.cancel()); QVERIFY(f.store->abort(blocker)); QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].live.cpuRam,
             limits.retirementDebtBytes);
    QVERIFY(!f.store->abort(tx)); QVERIFY(!f.store->closeSession(&f.error));
    f.provider->rejectRetire = false;
    f.store->processRetirements(64); QVERIFY(f.store->waitForRetirementIdle());
    QVERIFY(f.store->abort(tx));
    QCOMPARE(f.store->sessionStats().pendingRequests, qsizetype(0));
    QCOMPARE(f.store->sessionStats().activeControlWritePages, qsizetype(0));
    QCOMPARE(f.pixel(), QByteArray(bpp, char(0x31)));
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
}

void KisPageStoreCpuMutationTest::genericResolveRejectsMismatchedAccess_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("mismatch");
    for (int bpp : {1, 4, 8, 16}) for (int mismatch = 1; mismatch <= 4; ++mismatch)
        QTest::newRow(qPrintable(QStringLiteral("bpp%1-mismatch%2").arg(bpp).arg(mismatch))) << bpp << mismatch;
}

void KisPageStoreCpuMutationTest::genericResolveRejectsMismatchedAccess()
{
    QFETCH(int, bpp); QFETCH(int, mismatch);
    Fixture f; QVERIFY(f.init(bpp, 1));
    const auto tx = f.store->beginCurrentTransaction();
    const auto request = f.store->acquireWrite(tx, key(), cpu, KisPageWriteMode::PreserveContents, KisPagePriority::Normal);
    QVERIFY(request.isValid());
    // A different admitted writer supplies a real releasable pin. Its request
    // remains the sole logical owner even when the provider returns it here.
    const auto foreign = f.store->acquireWrite(tx, key(9), cpu, KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
    QVERIFY(foreign.isValid()); f.provider->mismatchedWriteReplica = f.provider->lastTarget;
    int releases = 0;
    f.provider->beforeRelease = [&] { ++releases; QCOMPARE(f.store->sessionStats().pendingRequests, qsizetype(2)); };
    f.provider->writeAccessMismatch = mismatch;
    QVERIFY(!f.store->resolve(request, request.readiness).isValid());
    f.provider->writeAccessMismatch = 0; f.provider->beforeRelease = {};
    QCOMPARE(releases, 1);
    QCOMPARE(f.store->sessionStats().pendingRequests, qsizetype(2));
    QCOMPARE(f.store->sessionStats().activeWriteLeases, qsizetype(0));
    auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
    QCOMPARE(static_cast<const quint8 *>(lease.cpuData())[0], quint8(0x31));
    f.store->cancel(std::move(lease)); QVERIFY(f.store->cancel(foreign)); QVERIFY(f.store->abort(tx));
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::genericResolveValidatesRequestIdentity()
{
    QFETCH(int, bpp); Fixture f; QVERIFY(f.init(bpp, 1));
    const auto tx = f.store->beginCurrentTransaction();
    const auto request = f.store->acquireWrite(tx, key(), cpu, KisPageWriteMode::PreserveContents, KisPagePriority::Normal);
    QVERIFY(request.isValid());
    const auto source = f.completions->registerSource(KisCompletionDomain::HostLogical);
    const auto other = f.completions->allocatePending(source);
    QVERIFY(f.completions->complete(other, KisCompletionStatus::Succeeded));
    for (int field = 0; field < 7; ++field) {
        auto changed = request;
        if (field == 0) ++changed.transaction.value;
        if (field == 1) ++changed.baseVersion.defaultPixelRevision;
        if (field == 2) ++changed.writeVersion.generation.value;
        if (field == 3) ++changed.writer.value;
        if (field == 4) changed.access = {KisPageAccessDomain::DiscreteVram, KisPageAccessKind::GpuBinding};
        if (field == 5) changed.mode = KisPageWriteMode::DiscardContents;
        if (field == 6) changed.readiness = other;
        QVERIFY(changed.isValid());
        QVERIFY(!f.store->resolve(changed, request.readiness).isValid());
        QVERIFY(!f.store->cancel(changed));
        QCOMPARE(f.store->sessionStats().pendingRequests, qsizetype(1));
        QCOMPARE(f.store->sessionStats().activeWriteLeases, qsizetype(0));
    }
    auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
    f.store->cancel(std::move(lease)); QVERIFY(f.store->abort(tx));
    QCOMPARE(f.pixel(), QByteArray(bpp, char(0x31)));
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
}

void KisPageStoreCpuMutationTest::genericWriteLifetimeBlocksCommit()
{
    Fixture f; QVERIFY(f.init());
    for (int terminal = 0; terminal < 4; ++terminal) {
        const auto tx = f.store->beginCurrentTransaction();
        auto mutation = f.begin(tx); auto first = mutation.beginWrite(key());
        QVERIFY(first.isValid()); first = {}; QVERIFY(mutation.seal());
        const auto request = f.store->acquireWrite(tx, key(1), cpu,
            KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
        QVERIFY(request.isValid()); QVERIFY(!f.store->commit(tx, f.store->preparedPages(tx)).isValid());
        QCOMPARE(f.store->sessionStats().activeControlWritePages, qsizetype(1));
        if (terminal == 0) {
            QVERIFY(f.store->cancel(request));
        } else if (terminal == 1) {
            QVERIFY(f.store->abort(tx));
            QCOMPARE(f.store->sessionStats().activeControlWritePages, qsizetype(0));
            continue;
        } else {
            auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
            QCOMPARE(f.store->sessionStats().activeControlWritePages, qsizetype(1));
            QVERIFY(!f.store->stageSurfaceDefaultPixel(tx, {1}, QByteArray(f.bpp, char(0x46))));
            bool heldThroughRelease = false;
            f.provider->beforeRelease = [&] {
                auto competitor = f.begin(tx);
                auto denied = competitor.beginWrite(key(1));
                heldThroughRelease = !denied.isValid() && competitor.cancel() &&
                    f.store->sessionStats().activeControlWritePages == 1 &&
                    !f.store->stageSurfaceDefaultPixel(tx, {1}, QByteArray(f.bpp, char(0x46)));
            };
            QVERIFY(!f.store->commit(tx, f.store->preparedPages(tx)).isValid());
            if (terminal == 2) f.store->cancel(std::move(lease));
            else QVERIFY(f.store->publishHostWrite(std::move(lease)).isValid());
            f.provider->beforeRelease = {};
            QVERIFY(heldThroughRelease);
        }
        QCOMPARE(f.store->sessionStats().activeControlWritePages, qsizetype(0));
        QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    }
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::genericPreparationProtectsTransactionBeforeRequest()
{
    Fixture f; QVERIFY(f.init());
    const auto tx = f.store->beginCurrentTransaction();
    auto mutation = f.begin(tx); auto write = mutation.beginWrite(key());
    QVERIFY(write.isValid()); write = {}; QVERIFY(mutation.seal());
    bool checked = false;
    bool protectedTransaction = false;
    f.provider->beforePrepareWrite = [&] {
        checked = true;
        auto competitor = f.begin(tx);
        auto denied = competitor.beginWrite(key(1));
        protectedTransaction = f.store->sessionStats().pendingRequests == 1 &&
            f.store->sessionStats().activeControlWritePages == 1 && !denied.isValid() && competitor.cancel() &&
            !f.remove(tx, key(1)) &&
            !f.store->commit(tx, f.store->preparedPages(tx)).isValid() && !f.store->abort(tx);
    };
    const auto request = f.store->acquireWrite(tx, key(1), cpu,
        KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
    QVERIFY(request.isValid()); QVERIFY(checked); QVERIFY(protectedTransaction);
    f.provider->beforePrepareWrite = {};
    QVERIFY(f.store->cancel(request));
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::genericRemovalRemainsAbsentUntilSeal_data()
{
    QTest::addColumn<int>("terminal");
    QTest::newRow("request-cancel") << 0;
    QTest::newRow("lease-cancel") << 1;
    QTest::newRow("publish-success") << 2;
    QTest::newRow("publish-failed-completion") << 3;
    QTest::newRow("allocation-failure") << 4;
    QTest::newRow("transaction-abort") << 5;
    QTest::newRow("publish-invalid-completion") << 6;
}

void KisPageStoreCpuMutationTest::genericRemovalRemainsAbsentUntilSeal()
{
    QFETCH(int, terminal);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    QVERIFY(f.remove(tx, key()));
    QVERIFY(f.store->stageSurfaceDefaultPixel(tx, {1}, QByteArray(f.bpp, char(0x46)), &f.error));
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay; overlay.transaction = tx.id;
    KisCapturedReadView during;
    bool blockedConflicts = false;
    f.provider->beforePrepareWrite = [&] {
        during = f.store->captureReadView(overlay, &f.error);
        auto competitor = f.begin(tx);
        auto denied = competitor.beginWrite(key());
        const auto duplicate = f.store->acquireWrite(tx, key(), cpu, KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
        blockedConflicts = !denied.isValid() && competitor.cancel() && !duplicate.isValid() &&
            !f.remove(tx, key()) && f.store->sessionStats().activeControlWritePages == 1;
    };
    f.provider->rejectWrite = terminal == 4;
    const auto request = f.store->acquireWrite(tx, key(), cpu, KisPageWriteMode::PreserveContents, KisPagePriority::Normal);
    f.provider->beforePrepareWrite = {};
    f.provider->rejectWrite = false;
    QVERIFY(blockedConflicts); QVERIFY2(during.isValid(), qPrintable(f.error));
    {
        auto read = during.readResidentPage(key()); QVERIFY(read.isValid());
        QVERIFY(read.version().isDefaultPixel());
        QCOMPARE(static_cast<const quint8 *>(read.data())[0], quint8(0x46));
    }
    QCOMPARE(request.isValid(), terminal != 4);
    const QByteArray defaultPixel(f.bpp, char(0x46));
    QByteArray changedPixel = defaultPixel; changedPixel[0] = char(0x72);
    QCOMPARE(f.pixel(overlay), defaultPixel);
    if (terminal == 0) QVERIFY(f.store->cancel(request));
    else if (terminal == 5) QVERIFY(f.store->abort(tx));
    else if (terminal != 4) {
        auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
        static_cast<quint8 *>(lease.cpuData())[0] = 0x72;
        QCOMPARE(f.pixel(overlay), defaultPixel);
        if (terminal == 1) f.store->cancel(std::move(lease));
        else if (terminal == 2) QVERIFY(f.store->publishHostWrite(std::move(lease)).isValid());
        else {
            KisCompletionTicket completion;
            if (terminal == 3) {
                const KisCompletionDomain source = KisCompletionDomain::HostLogical;
                completion = f.completions->allocatePending(f.completions->registerSource(source));
                QVERIFY(completion.isValid());
                QVERIFY(f.completions->complete(completion, KisCompletionStatus::Failed));
            }
            QVERIFY(!f.store->publish(std::move(lease), completion).isValid());
        }
    }
    QCOMPARE(f.store->sessionStats().activeControlWritePages, qsizetype(0));
    if (terminal != 5) {
        QCOMPARE(f.pixel(overlay), terminal == 2 ? changedPixel : defaultPixel);
        if (terminal == 2) {
            QCOMPARE(f.pixel(overlay, 0, 1), defaultPixel); // no copy of removed pixels
            QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
        } else QVERIFY(f.store->abort(tx));
    }
    {
        auto read = during.readResidentPage(key()); QVERIFY(read.isValid());
        QCOMPARE(static_cast<const quint8 *>(read.data())[0], quint8(0x46));
    }
    during = {};
    QCOMPARE(f.pixel(), terminal == 2 ? changedPixel : QByteArray(f.bpp, char(0x31)));
    QVERIFY(f.store->closeSession(&f.error));
}

void KisPageStoreCpuMutationTest::aliasRemovalRemainsAbsentUntilSeal_data()
{
    QTest::addColumn<bool>("reject");
    QTest::addColumn<bool>("changedDefault");
    for (bool reject : {false, true}) for (bool changedDefault : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("reject%1-newDefault%2").arg(reject).arg(changedDefault))) << reject << changedDefault;
}

void KisPageStoreCpuMutationTest::aliasRemovalRemainsAbsentUntilSeal()
{
    QFETCH(bool, reject);
    QFETCH(bool, changedDefault);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    KisSurfaceEpochState surface;
    QVERIFY(f.store->resolveSurfaceState({1}, {}, &surface));
    const auto sourceRequest = f.store->acquireRead(key(1), {}, cpu, KisPagePriority::Normal);
    auto source = f.store->resolve(sourceRequest, sourceRequest.readiness); QVERIFY(source.isValid());
    auto *tile = f.provider->p->tileDataForLease(source.leaseId()); QVERIFY(tile);
    const auto tx = f.store->beginCurrentTransaction();
    QVERIFY(f.remove(tx, key()));
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay; overlay.transaction = tx.id;
    if (changedDefault) QVERIFY(f.store->stageSurfaceDefaultPixel(tx, {1}, QByteArray(f.bpp, char(0x46)), &f.error));
    QVERIFY(f.store->resolveSurfaceState({1}, overlay, &surface));
    auto input = f.provider->p->captureCompletedTileSource(surface.allocationDescriptor(), tile, &f.error);
    QVERIFY(input);
    f.store->release(std::move(source));
    auto mutation = f.begin(tx);
    QVERIFY(mutation.aliasPage(key(), input, &f.error));
    KisCapturedReadView during;
    bool blocked = false;
    f.provider->beforeAdopt = [&] {
        during = f.store->captureReadView(overlay, &f.error);
        auto competitor = f.begin(tx);
        auto denied = competitor.beginWrite(key());
        blocked = !denied.isValid() && competitor.cancel() &&
            !f.remove(tx, key()) && f.store->sessionStats().activeCpuWritePages == 1;
    };
    f.provider->rejectAdopt = reject;
    QCOMPARE(mutation.seal(&f.error), !reject);
    f.provider->beforeAdopt = {};
    input.reset();
    QVERIFY(blocked); QVERIFY(during.isValid());
    QCOMPARE(f.store->sessionStats().activeControlWritePages, qsizetype(0));
    QCOMPARE(f.store->sessionStats().activeCpuWritePages, qsizetype(0));
    {
        auto read = during.readResidentPage(key()); QVERIFY(read.isValid());
        QVERIFY(read.version().isDefaultPixel());
        QCOMPARE(static_cast<const quint8 *>(read.data())[0], changedDefault ? quint8(0x46) : quint8(0x2a));
    }
    QCOMPARE(f.pixel(overlay), QByteArray(f.bpp, reject ? (changedDefault ? char(0x46) : char(0x2a)) : char(0x31)));
    if (reject) QVERIFY(f.store->abort(tx));
    else QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    during = {}; QVERIFY(f.store->closeSession(&f.error));
}

void KisPageStoreCpuMutationTest::indexedWriteClaimsIgnoreUnrelatedRequests_data()
{
    QTest::addColumn<int>("reads");
    QTest::addColumn<int>("writes");
    for (int reads : {0, 32, 256}) for (int writes : {0, 32})
        QTest::newRow(qPrintable(QStringLiteral("reads%1-writes%2").arg(reads).arg(writes))) << reads << writes;
}

void KisPageStoreCpuMutationTest::indexedWriteClaimsIgnoreUnrelatedRequests()
{
    QFETCH(int, reads); QFETCH(int, writes);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    QVector<KisReadRequest> pendingReads;
    for (int i = 0; i < reads; ++i) {
        const auto r = f.store->acquireRead(key(1), {}, cpu, KisPagePriority::Normal);
        QVERIFY(r.isValid()); pendingReads.append(r);
    }
    const auto background = f.store->beginCurrentTransaction();
    for (int i = 0; i < writes; ++i) {
        const auto r = f.store->acquireWrite(background, key(i + 2), cpu,
            KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
        QVERIFY(r.isValid());
    }
    const auto before = f.store->sessionStats();
    QCOMPARE(before.activeControlWritePages, qsizetype(writes));
    const auto tx = f.store->beginCurrentTransaction();
    auto segment = f.begin(tx);
    KisPageVersion version;
    for (int i = 0; i < 16; ++i) {
        auto write = segment.beginWrite(key()); QVERIFY(write.isValid());
        if (!i) version = write.version();
        QCOMPARE(write.version(), version);
        static_cast<quint8 *>(write.data())[0] = 0x71;
    }
    QVERIFY(f.remove(tx, key(1)));
    const auto after = f.store->sessionStats();
    QCOMPARE(after.activeCpuWritePages, qsizetype(1));
    QCOMPARE(after.activeControlWritePages, before.activeControlWritePages);
    QCOMPARE(after.writeRequestsCreated, before.writeRequestsCreated);
    QVERIFY(segment.seal()); QVERIFY(f.store->abort(tx));
    for (const auto &r : pendingReads) QVERIFY(f.store->cancel(r));
    QVERIFY(f.store->abort(background));
    QCOMPARE(f.store->sessionStats().activeControlWritePages, qsizetype(0));
    QCOMPARE(f.store->sessionStats().activeCpuWritePages, qsizetype(0));
    QVERIFY(f.store->closeSession(&f.error));
}

void KisPageStoreCpuMutationTest::concurrentWritePreparationClaims_data()
{
    QTest::addColumn<int>("phase");
    QTest::addColumn<bool>("fail");
    for (int phase : {0, 1, 2}) for (bool fail : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("phase%1-fail%2").arg(phase).arg(fail))) << phase << fail;
}

void KisPageStoreCpuMutationTest::concurrentWritePreparationClaims()
{
    QFETCH(int, phase); QFETCH(bool, fail);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    const auto unrelated = f.store->beginCurrentTransaction();
    const auto target = key(phase == 0 ? 2 : 0);
    std::shared_ptr<const KisPageReplicaSource> input;
    if (phase == 2) {
        KisSurfaceEpochState surface;
        QVERIFY(f.store->resolveSurfaceState({1}, {}, &surface));
        const auto request = f.store->acquireRead(key(1), {}, cpu, KisPagePriority::Normal);
        auto source = f.store->resolve(request, request.readiness); QVERIFY(source.isValid());
        auto *tile = f.provider->p->tileDataForLease(source.leaseId()); QVERIFY(tile);
        input = f.provider->p->captureCompletedTileSource(surface.allocationDescriptor(), tile);
        QVERIFY(input);
        f.store->release(std::move(source));
    }
    QSemaphore entered, resume;
    const auto pause = [&] { entered.release(); resume.acquire(); };
    if (phase != 2) f.provider->beforePrepareWrite = pause;
    else f.provider->beforeAdopt = pause;
    KisWriteRequest request;
    bool adopted = false;
    std::thread worker([&] {
        if (phase == 2) {
            auto mutation = f.begin(tx);
            adopted = mutation.isActive() && mutation.aliasPage(target, input) && mutation.seal();
        } else {
            request = f.store->acquireWrite(tx, target, cpu, KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
        }
    });
    const auto join = qScopeGuard([&] {
        resume.release();
        if (worker.joinable()) worker.join();
    });
    QVERIFY(entered.tryAcquire(1, 5000));
    const auto stats = f.store->sessionStats();
    QCOMPARE(stats.activeControlWritePages, qsizetype(phase == 2 ? 0 : 1));
    QCOMPARE(stats.activeCpuWritePages, qsizetype(phase == 2 ? 1 : 0));
    QCOMPARE(stats.pendingRequests, qsizetype(phase == 2 ? 0 : 1));
    for (const auto &competitorTx : {tx, unrelated}) {
        auto competitor = f.begin(competitorTx);
        QVERIFY(competitor.isActive());
        auto denied = competitor.beginWrite(target); QVERIFY(!denied.isValid());
        QVERIFY(competitor.cancel());
        QVERIFY(!f.store->acquireWrite(competitorTx, target, cpu,
            KisPageWriteMode::DiscardContents, KisPagePriority::Normal).isValid());
        QVERIFY(!f.remove(competitorTx, target));
    }
    QVERIFY(!f.store->stageSurfaceDefaultPixel(tx, {1}, QByteArray(f.bpp, char(0x46))));
    QVERIFY(!f.store->abort(tx));
    QVERIFY(!f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QVERIFY(!f.store->closeSession());
    // A stalled provider must not hold owner or serialize different PageKeys.
    auto segment = f.begin(unrelated);
    auto write = segment.beginWrite(key(1)); QVERIFY(write.isValid());
    static_cast<quint8 *>(write.data())[0] = 0x72;
    write = {}; QVERIFY(segment.seal());
    QVERIFY(f.store->commit(unrelated, f.store->preparedPages(unrelated)).isValid());
    // Publish fault configuration before the semaphore handoff. The provider
    // reads it only after resuming; no unsynchronized test flag mutation.
    f.provider->rejectWrite = fail;
    f.provider->rejectAdopt = fail;
    resume.release(); worker.join();
    f.provider->beforeRequestReplica = {};
    f.provider->beforePrepareWrite = {};
    f.provider->beforeAdopt = {};
    f.provider->rejectWrite = f.provider->rejectAdopt = false;
    if (phase == 2) {
        QCOMPARE(adopted, !fail);
        input.reset();
    } else {
        QCOMPARE(request.isValid(), !fail);
        if (request.isValid()) {
            QCOMPARE(f.store->sessionStats().activeControlWritePages, qsizetype(1));
            QVERIFY(!f.store->stageSurfaceDefaultPixel(tx, {1}, QByteArray(f.bpp, char(0x46))));
            QVERIFY(f.store->cancel(request));
        }
    }
    QCOMPARE(f.store->sessionStats().activeControlWritePages, qsizetype(0));
    // The exact same PageKey can be claimed again after failure/cancel/seal.
    QVERIFY(f.store->stageSurfaceDefaultPixel(tx, {1}, QByteArray(f.bpp, char(0x46))));
    auto retry = f.begin(tx);
    auto retryWrite = retry.beginWrite(target); QVERIFY(retryWrite.isValid());
    retryWrite = {}; QVERIFY(retry.seal());
    QVERIFY(f.store->abort(tx));
    QVERIFY(f.store->closeSession());
    QVERIFY(!f.store->sessionStats().hasOutstandingCapabilities());
}

void KisPageStoreCpuMutationTest::executionPreparationReusesParkedBacking()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    const auto baseline = f.store->mutationStatistics();
    auto mutation = f.begin(tx);
    auto execution = mutation.borrowExecution({1}, {{0, 0}});
    QCOMPARE(execution.prepareWrites(&f.error), KisPageMutationExecution::PreparationResult::Ready);
    auto guard = execution.beginWrite(key()); QVERIFY(guard.isValid());
    const auto version = guard.version();
    auto *tile = f.provider->p->tileDataForCpuWriteGuard(guard); QVERIFY(tile);
    QVERIFY(tile->ref());
    const auto releaseTile = qScopeGuard([&] { tile->deref(); });
    std::memset(guard.data(), 0x71, size_t(guard.byteSize()));
    guard = {};
    QVERIFY(execution.finish());
    QVERIFY(KisTileDataStore::instance()->trySwapTileData(tile));

    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    const auto work = [&](KisPageStoreDiagnosticPhase phase) {
        return recorder.metrics()[size_t(phase)].workItems;
    };
    f.provider->rejectWrite = true; // existing backing needs no new allocation
    execution = mutation.borrowExecution({1}, {{0, 0}});
    QCOMPARE(execution.prepareWrites(&f.error), KisPageMutationExecution::PreparationResult::Ready);
    QCOMPARE(work(KisPageStoreDiagnosticPhase::WriteWritablePin), quint64(0));
    QCOMPARE(work(KisPageStoreDiagnosticPhase::WritePendingMaterialize), quint64(0));
    guard = execution.beginWrite(key()); QVERIFY2(guard.isValid(), qPrintable(f.error));
    QCOMPARE(work(KisPageStoreDiagnosticPhase::WriteWritablePin), quint64(1));
    QCOMPARE(work(KisPageStoreDiagnosticPhase::WritePendingMaterialize), quint64(1));
    QCOMPARE(guard.version(), version);
    QCOMPARE(QByteArray(static_cast<const char *>(guard.data()), int(guard.byteSize())),
             QByteArray(int(guard.byteSize()), char(0x71)));
    std::memset(guard.data(), 0x72, size_t(bpp));
    guard = {};
    QVERIFY(execution.finish());
    f.provider->rejectWrite = false;
    QVERIFY(mutation.seal());
    const auto after = f.store->mutationStatistics();
    QCOMPARE(after.generationsReserved - baseline.generationsReserved, quint64(1));
    QCOMPARE(after.writablePinsAcquired - baseline.writablePinsAcquired, quint64(3));
    QCOMPARE(after.writablePinsReleased - baseline.writablePinsReleased, quint64(3));
    QCOMPARE(after.maximumPinnedPagesPerExecution, quint64(1));
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QCOMPARE(f.pixel(), QByteArray(bpp, char(0x72)));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::historyCollectionReleasesReadBindingCache()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    for (int i = 0; i < 33; ++i) {
        auto view = f.store->captureReadView();
        auto guard = view.readResidentPage(key()); QVERIFY(guard.isValid());
        QCOMPARE(kisPageStoreMetadataMetrics(*f.store).cpuReadBindings, quint64(1));
        guard = {}; view = {};
        QVERIFY(f.fill(quint8(0x32 + i)));
        QCOMPARE(kisPageStoreMetadataMetrics(*f.store).cpuReadBindings, quint64(0));
        QCOMPARE(f.pixel(), QByteArray(bpp, char(0x32 + i)));
    }
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::guardReleaseParksPendingBacking_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("seal"); QTest::addColumn<bool>("borrow");
    for (int bpp : {1, 4, 8, 16}) for (bool seal : {false, true}) for (bool borrow : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("bpp%1-seal%2-borrow%3").arg(bpp).arg(seal).arg(borrow))) << bpp << seal << borrow;
}

void KisPageStoreCpuMutationTest::guardReleaseParksPendingBacking()
{
    QFETCH(int, bpp); QFETCH(bool, seal); QFETCH(bool, borrow);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    auto beforeView = f.store->captureReadView();
    const auto tx = f.store->beginCurrentTransaction();
    const auto stats = f.store->mutationStatistics();
    const auto requests = f.store->sessionStats();
    const auto work = f.provider->p->payloadWork();
    auto segment = f.begin(tx);
    auto execution = borrow ? segment.borrowExecution({1}, {{0, 0}}) : KisPageMutationExecution{};
    const auto acquire = [&] { return borrow ? execution.beginWrite(key()) : segment.beginWrite(key()); };
    auto guard = acquire(); QVERIFY(guard.isValid());
    const auto version = guard.version();
    auto *tile = f.provider->p->tileDataForCpuWriteGuard(guard); QVERIFY(tile);
    // Test-only lifetime ref: keep the storage identity while asking tiles3
    // to swap it. This reference grants no access to mutable bytes.
    QVERIFY(tile->ref());
    const auto releaseTile = qScopeGuard([&] { tile->deref(); });
    const auto swap = [&] {
        return KisTileDataStore::instance()->trySwapTileData(tile);
    };
    memset(guard.data(), 0x71, size_t(guard.byteSize()));
    QVERIFY(!swap());
    guard = {};
    QVERIFY(swap());
    auto binding = f.provider->cpuResidentBinding(f.provider->lastTarget); QVERIFY(binding);
    QVERIFY(!binding->acquireRead(f.provider->lastTarget.allocationIdentity(), true)); QVERIFY(!binding->acquireWrite(f.provider->lastTarget.allocationIdentity())); QVERIFY(!binding->retire(f.provider->lastTarget.allocationIdentity()));
    auto old = beforeView.readResidentPage(key()); QVERIFY(old.isValid());
    QCOMPARE(static_cast<const quint8 *>(old.data())[0], quint8(0x31));
    int restored = 0;
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::WritePendingMaterialize) return;
        ++restored;
        // A real pending-page swap-in must run outside the owner gate.
        QVERIFY(f.store->isOperational());
    });
    guard = acquire(); QVERIFY2(guard.isValid(), qPrintable(f.error));
    QCOMPARE(restored, 1); QCOMPARE(guard.version(), version);
    QCOMPARE(QByteArray(static_cast<const char *>(guard.data()), int(guard.byteSize())),
             QByteArray(int(guard.byteSize()), char(0x71)));
    guard = {}; QVERIFY(swap());
    // seal/cancel needs no pixel pin or eager swap-in of parked pages.
    if (borrow) QVERIFY(execution.finish());
    if (seal) QVERIFY(segment.seal()); else QVERIFY(segment.cancel());
    QCOMPARE(restored, 1);
    const auto after = f.store->mutationStatistics();
    QCOMPARE(after.generationsReserved - stats.generationsReserved, quint64(1));
    QCOMPARE(after.writablePinsAcquired - stats.writablePinsAcquired, quint64(2));
    QCOMPARE(after.writablePinsReleased - stats.writablePinsReleased, quint64(2));
    QCOMPARE(after.pendingWriteMaterializations - stats.pendingWriteMaterializations, quint64(1));
    QCOMPARE(f.provider->p->payloadWork().nativeDuplicatePages - work.nativeDuplicatePages, quint64(1));
    QCOMPARE(f.store->sessionStats().writeRequestsCreated, requests.writeRequestsCreated);
    if (seal) QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    else QVERIFY(f.store->abort(tx));
    QCOMPARE(f.pixel(), QByteArray(bpp, seal ? char(0x71) : char(0x31)));
    old = {}; beforeView = {};
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::pinFootprintTracksGuards_data()
{
    QTest::addColumn<bool>("retainGuards");
    QTest::addColumn<bool>("aliasFirst");
    QTest::newRow("sequential") << false << false;
    QTest::newRow("borrowed-spans") << true << false;
    QTest::newRow("alias-sequential") << false << true;
    QTest::newRow("alias-borrowed-spans") << true << true;
}

void KisPageStoreCpuMutationTest::pinFootprintTracksGuards()
{
    QFETCH(bool, retainGuards);
    QFETCH(bool, aliasFirst);
    Fixture f; QVERIFY(f.init());
    if (aliasFirst) QVERIFY(f.fill(0x31));
    const auto initialStats = f.store->mutationStatistics();
    const auto tx = f.store->beginCurrentTransaction();
    auto segment = f.begin(tx);
    std::vector<KisCpuWriteGuard> guards;
    constexpr int pages = 64;
    std::weak_ptr<const KisPageReplicaSource> sourceLifetime;
    if (aliasFirst) {
        auto view = f.store->captureReadView();
        auto read = view.readResidentPage(key()); QVERIFY(read.isValid());
        KisSurfaceEpochState surface; QVERIFY(view.resolveSurfaceState({1}, &surface));
        auto source = f.provider->p->captureCpuReadSource(read, surface.allocationDescriptor());
        QVERIFY(source);
        sourceLifetime = source;
        for (int i = 0; i < pages; ++i) QVERIFY(segment.aliasPage(key(i), source));
    }
    if (aliasFirst) QVERIFY(!sourceLifetime.expired()); // only entries retain the input
    for (int i = 0; i < pages; ++i) {
        auto guard = segment.beginWrite(key(i)); QVERIFY(guard.isValid());
        if (aliasFirst) QCOMPARE(static_cast<const quint8 *>(guard.data())[0], quint8(0x31));
        static_cast<quint8 *>(guard.data())[0] = quint8(i + 1);
        if (retainGuards) guards.emplace_back(std::move(guard));
    }
    QVERIFY(sourceLifetime.expired()); // consumed source cannot survive as a stale second owner
    if (retainGuards) {
        QVERIFY(!segment.seal());
        for (int i = 0; i < pages; ++i) {
            QCOMPARE(static_cast<const quint8 *>(guards[size_t(i)].data())[0], quint8(i + 1));
            QCOMPARE(guards[size_t(i)].version().key, key(i));
            QVERIFY(!segment.beginWrite(key(i)).isValid());
            QVERIFY(!segment.removePage(key(i)));
        }
        QVERIFY(!segment.cancel());
        guards.clear();
    }
    QVERIFY(segment.seal());
    const auto stats = f.store->mutationStatistics();
    QCOMPARE(stats.writablePinsAcquired - initialStats.writablePinsAcquired, quint64(pages));
    QCOMPARE(stats.writablePinsReleased - initialStats.writablePinsReleased, quint64(pages));
    QCOMPARE(stats.maximumPinnedPagesPerExecution, quint64(retainGuards ? pages : 1));
    QCOMPARE(stats.pendingWriteMaterializations, quint64(0));
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    for (int i = 0; i < pages; ++i) QCOMPARE(quint8(f.pixel({}, i)[0]), quint8(i + 1));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::mutablePreparationDoesNotAliasSource_data()
{
    QTest::addColumn<bool>("native");
    QTest::newRow("native") << true;
    QTest::newRow("generic") << false;
}

void KisPageStoreCpuMutationTest::mutablePreparationDoesNotAliasSource()
{
    QFETCH(bool, native);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    KisSurfaceEpochState surface;
    QVERIFY(f.store->resolveSurfaceState({1}, {}, &surface));
    const auto request = f.store->acquireRead(key(), {}, cpu, KisPagePriority::Normal);
    auto source = f.store->resolve(request, request.readiness); QVERIFY(source.isValid());
    auto *tile = f.provider->p->tileDataForLease(source.leaseId()); QVERIFY(tile);
    auto input = f.provider->p->captureCompletedTileSource(surface.allocationDescriptor(), tile);
    QVERIFY(input);
    f.store->release(std::move(source));
    const auto tx = f.store->beginCurrentTransaction();
    const auto work = f.provider->p->payloadWork();
    if (native) {
        auto segment = f.begin(tx);
        auto guard = segment.beginWrite(key(2)); QVERIFY(guard.isValid());
        QCOMPARE(static_cast<const quint8 *>(guard.data())[0], quint8(0x2a));
        static_cast<quint8 *>(guard.data())[0] = 0x72;
        guard = {}; QVERIFY(segment.seal());
    } else {
        const auto write = f.store->acquireWrite(tx, key(2), cpu, KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
        auto lease = f.store->resolve(write, write.readiness); QVERIFY(lease.isValid());
        QCOMPARE(static_cast<const quint8 *>(lease.cpuData())[0], quint8(0x2a));
        static_cast<quint8 *>(lease.cpuData())[0] = 0x72;
        QVERIFY(f.store->publishHostWrite(std::move(lease)).isValid());
    }
    QCOMPARE(f.provider->p->payloadWork().adoptedPages, work.adoptedPages);
    QCOMPARE(f.pixel(), QByteArray(f.bpp, char(0x31)));
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QCOMPARE(quint8(f.pixel({}, 2)[0]), quint8(0x72));
    QCOMPARE(f.pixel(), QByteArray(f.bpp, char(0x31)));
    input.reset();
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::failedCommitPreparationReleasesClaim()
{
    Fixture f; QVERIFY(f.init());
    const auto tx = f.store->beginCurrentTransaction();
    auto mutation = f.begin(tx); auto write = mutation.beginWrite(key());
    QVERIFY(write.isValid()); write = {}; QVERIFY(mutation.seal());
    f.provider->validationsUntilFailure = 0;
    QVERIFY(!f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    f.provider->validationsUntilFailure = -1;
    auto retryMutation = f.begin(tx); QVERIFY(retryMutation.isActive()); QVERIFY(retryMutation.cancel());
    QVERIFY(f.store->abort(tx)); QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::restoreProtectsSourceAndRevalidatesLease()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto retained = f.store->captureRetainedEpoch();
    QVERIFY(f.fill(0x72));
    std::unique_ptr<KisReadLease> reader;
    bool released = false;
    int attempts = 0;
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::RestorePublishOwnerWait || ++attempts != 1) return;
        released = f.store->releaseSnapshot(retained.token);
        const auto request = f.store->acquireRead(key(), {}, cpu, KisPagePriority::Normal);
        reader = std::make_unique<KisReadLease>(f.store->resolve(request, request.readiness));
    });
    QVERIFY(f.store->restoreRetainedEpoch(retained).isValid());
    QVERIFY(released); QCOMPARE(attempts, 2); QVERIFY(reader && reader->isValid());
    QCOMPARE(f.store->publicationStatistics().restoreMetadataRepreparations, quint64(1));
    QCOMPARE(static_cast<const quint8 *>(reader->cpuData())[0], quint8(0x72));
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x31));
    f.store->release(std::move(*reader)); reader.reset();
    QCOMPARE(f.store->sessionStats().retainedSnapshots, qsizetype(0));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::restoreRejectsNewTransaction()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto retained = f.store->captureRetainedEpoch();
    QVERIFY(f.fill(0x72));
    KisPageTransaction concurrent;
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase == KisPageStoreDiagnosticPhase::RestorePublishOwnerWait)
            concurrent = f.store->beginCurrentTransaction();
    });
    QVERIFY(!f.store->restoreRetainedEpoch(retained).isValid());
    QVERIFY(concurrent.isValid()); QCOMPARE(quint8(f.pixel()[0]), quint8(0x72));
    QVERIFY(f.store->abort(concurrent)); QVERIFY(f.store->releaseSnapshot(retained.token));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::restoreRejectsChangedHead()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto first = f.store->captureRetainedEpoch();
    QVERIFY(f.fill(0x52)); const auto second = f.store->captureRetainedEpoch();
    QVERIFY(f.fill(0x72));
    KisImageEpochCommitTicket concurrent;
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase == KisPageStoreDiagnosticPhase::RestorePublishOwnerWait)
            concurrent = f.store->restoreRetainedEpoch(second);
    });
    QVERIFY(!f.store->restoreRetainedEpoch(first).isValid()); QVERIFY(concurrent.isValid());
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x52));
    QVERIFY(f.store->releaseSnapshot(first.token)); QVERIFY(f.store->releaseSnapshot(second.token));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::restoreIgnoresDetachedRetirement_data()
{
    QTest::addColumn<bool>("blank");
    QTest::addColumn<bool>("delta");
    for (bool blank : {false, true}) for (bool delta : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("blank%1-delta%2").arg(blank).arg(delta))) << blank << delta;
}

void KisPageStoreCpuMutationTest::restoreIgnoresDetachedRetirement()
{
    QFETCH(bool, blank); QFETCH(bool, delta);
    Fixture f;
    // This test assigns the explicit processRetirements call both detached
    // records. Completing their ticket must not race an automatic pass.
    f.provider->disableBackgroundRetirement = true;
    QVERIFY(f.init());
    if (!blank) QVERIFY(f.fill(0x31));
    const auto retained = f.store->captureRetainedEpoch(); QVERIFY(retained.isValid());
    KisPageStoreMementoManager history; QVERIFY(history.configure(f.store.get()));
    KisPageStoreMemento memento;
    if (delta) {
        const auto historyTx = history.begin(); QVERIFY(historyTx.isValid());
        QVERIFY(KisPageStoreCpuSurfaceOps::fillRect(f.store.get(), {1}, historyTx.transaction,
            QRect(0, 0, 128, 64), QByteArray(f.bpp, char(0x51)), &f.error));
        memento = history.commit(historyTx); QVERIFY(memento.isValid());
        QVERIFY(f.store->waitForRetirementIdle());
    }
    QVERIFY(f.fill(0x41));
    const KisCompletionDomain source = KisCompletionDomain::CpuJob;
    const auto ticket = f.completions->allocatePending(f.completions->registerSource(source));
    QVERIFY(ticket.isValid());
    // No retirement worker is active when changing the test provider config.
    f.provider->deferredRetirement = ticket;
    auto unblock = qScopeGuard([&] {
        f.completions->complete(ticket, KisCompletionStatus::Succeeded);
        f.store->waitForRetirementIdle();
        f.store->processRetirements(100);
    });
    QVERIFY(f.fill(0x71));
    QCOMPARE(f.store->sessionStats().providerOperations, qsizetype(2));
    QCOMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(2));
    const int calls = f.provider->retireCalls.load();
    auto beforeRestore = f.store->captureReadView(); QVERIFY(beforeRestore.isValid());
    const auto restored = delta ? history.rollback(memento, &f.error)
                                : f.store->restoreRetainedEpoch(retained);
    QVERIFY2(restored.isValid(), "Detached retirement must not reject history restoration");
    QCOMPARE(f.pixel(), QByteArray(f.bpp, char(blank ? 0x2a : 0x31)));
    auto old = beforeRestore.readResidentPage(key()); QVERIFY(old.isValid());
    QCOMPARE(static_cast<const quint8 *>(old.data())[0], quint8(0x71));
    // Restoration schedules history work, which may itself poll the pending
    // retirement queue. Quiesce that work while acknowledgement is still
    // pending, so the explicit drain below deterministically owns both items.
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().providerOperations, qsizetype(2));
    QVERIFY(f.completions->complete(ticket, KisCompletionStatus::Succeeded));
    QCOMPARE(f.store->processRetirements(100).retired, qsizetype(2));
    QCOMPARE(f.provider->retireCalls.load(), calls); // Poll, never reissue retire.
    QCOMPARE(f.store->sessionStats().providerOperations, qsizetype(0));
    old = {}; beforeRestore = {};
    QVERIFY(f.store->releaseSnapshot(retained.token));
    QVERIFY(history.close());
    QVERIFY(f.store->waitForRetirementIdle());
    QVERIFY(f.store->closeSession());
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::concurrentDisjointCommitsKeepEveryPage()
{
    Fixture f; QVERIFY(f.init());
    // Prepare sequentially: the instrumented fault provider's counters are
    // thread-confined. Concurrent commit retires no old physical replicas in
    // this blank fixture, and exercises the actual optimistic publish loop.
    constexpr int count = 12;
    std::array<KisPageTransaction, count> transactions;
    std::array<KisPreparedPageSet, count> prepared;
    std::array<KisImageEpochCommitTicket, count> committed;
    for (int i = 0; i < count; ++i) {
        transactions[i] = f.store->beginCurrentTransaction();
        auto mutation = f.begin(transactions[i]); auto write = mutation.beginWrite(key(i));
        QVERIFY(write.isValid()); static_cast<quint8 *>(write.data())[0] = quint8(i + 1);
        write = {}; QVERIFY(mutation.seal()); prepared[i] = f.store->preparedPages(transactions[i]);
    }
    std::atomic<int> ready{0}; std::atomic<bool> go{false};
    std::array<std::thread, count> threads;
    for (int i = 0; i < count; ++i) threads[i] = std::thread([&, i] {
        ++ready;
        while (!go.load()) std::this_thread::yield();
        committed[i] = f.store->commit(transactions[i], prepared[i]);
    });
    while (ready.load() != count) std::this_thread::yield();
    go = true;
    for (auto &thread : threads) thread.join();
    for (int i = 0; i < count; ++i) {
        QVERIFY(committed[i].isValid()); QCOMPARE(quint8(f.pixel({}, i)[0]), quint8(i + 1));
    }
    QCOMPARE(f.store->publicationStatistics().activeCommitPreparations, qsizetype(0));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::defaultPublicationRevalidatesNewlyRegisteredPage()
{
    Fixture f; QVERIFY(f.init());
    std::unique_ptr<KisReadLease> lateReader;
    bool injected = false;
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::CommitPublishOwnerWait || injected) return;
        injected = true;
        const auto request = f.store->acquireRead(key(3), {}, cpu, KisPagePriority::Normal);
        lateReader = std::make_unique<KisReadLease>(f.store->resolve(request, request.readiness));
    });
    QVERIFY(f.setDefault(0x55));
    QVERIFY(lateReader && lateReader->isValid());
    QCOMPARE(f.store->publicationStatistics().directoryRepreparations, quint64(1));
    QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(2));
    QCOMPARE(static_cast<const quint8 *>(lateReader->cpuData())[0], quint8(0x2a));
    QCOMPARE(quint8(f.pixel({}, 3)[0]), quint8(0x55));
    f.store->release(std::move(*lateReader)); lateReader.reset();
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::defaultPublicationRevalidatesDescriptorStorage()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    QCOMPARE(f.pixel({}, 3), QByteArray(f.bpp, char(0x2a))); // register a virtual default page
    const auto before = f.store->publicationStatistics();
    bool injected = false; int attempts = 0;
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::CommitPublishOwnerWait) return;
        ++attempts;
        if (injected) return;
        injected = true;
        const auto other = f.store->beginCurrentTransaction(); QVERIFY(other.isValid());
        auto mutation = f.begin(other); auto write = mutation.beginWrite(key(0)); QVERIFY(write.isValid());
        static_cast<quint8 *>(write.data())[0] = 0x71; write = {};
        QVERIFY(mutation.cancel()); QVERIFY(f.store->abort(other));
    });
    QVERIFY(f.setDefault(0x55)); QCOMPARE(attempts, 2);
    const auto after = f.store->publicationStatistics();
    QCOMPARE(after.descriptorRepreparations - before.descriptorRepreparations, quint64(1));
    QCOMPARE(after.descriptorCapacityPreparations - before.descriptorCapacityPreparations, quint64(3));
    QCOMPARE(after.descriptorInstallations - before.descriptorInstallations, quint64(1));
    QCOMPARE(after.directoryRepreparations, before.directoryRepreparations);
    QCOMPARE(f.pixel({}, 3), QByteArray(f.bpp, char(0x55)));
    QCOMPARE(f.pixel(), QByteArray(f.bpp, char(0x31)));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::restorationRevalidatesNewlyRegisteredPage()
{
    Fixture f; QVERIFY(f.init());
    const auto before = f.store->captureRetainedEpoch();
    QVERIFY(f.setDefault(0x55));
    std::unique_ptr<KisReadLease> lateReader;
    bool injected = false;
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::RestorePublishOwnerWait || injected) return;
        injected = true;
        const auto request = f.store->acquireRead(key(3), {}, cpu, KisPagePriority::Normal);
        lateReader = std::make_unique<KisReadLease>(f.store->resolve(request, request.readiness));
    });
    QVERIFY(f.store->restoreRetainedEpoch(before).isValid());
    QVERIFY(lateReader && lateReader->isValid());
    QCOMPARE(f.store->publicationStatistics().restoreDirectoryRepreparations, quint64(1));
    QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(2));
    QCOMPARE(static_cast<const quint8 *>(lateReader->cpuData())[0], quint8(0x55));
    QCOMPARE(quint8(f.pixel({}, 3)[0]), quint8(0x2a));
    f.store->release(std::move(*lateReader)); lateReader.reset();
    QVERIFY(f.store->releaseSnapshot(before.token)); QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::compatibilityFinalUnlockCanTransferThreads()
{
    KisTiledDataManagerPageStoreBackend backend;
    const quint8 defaultPixel = 0x2a;
    QVERIFY(backend.configure(1, &defaultPixel));
    auto lease = backend.acquireTile(0, 0, true, false);
    QVERIFY(lease);
    lease->tileData()->data()[0] = 0x55;
    lease->markDirty();
    QVERIFY(!backend.store()->closeSession());
    bool finished = false;
    std::thread releaser([lease = std::move(lease), &finished]() mutable {
        finished = lease->finish();
        lease.reset();
    });
    releaser.join();
    QVERIFY(finished);
    QCOMPARE(backend.store()->sessionStats().activeTransactions, qsizetype(0));
    QCOMPARE(backend.store()->sessionStats().writeRequestsCreated, quint64(0));
    auto read = backend.acquireTile(0, 0, false, false);
    QVERIFY(read); QCOMPARE(read->tileData()->data()[0], quint8(0x55));
    QVERIFY(read->finish()); read.reset();
    QVERIFY(backend.store()->closeSession());
}

void KisPageStoreCpuMutationTest::rejectedAllocationRetirementIsNotLost()
{
    QFETCH(bool, delayed);
    Fixture f; QVERIFY(f.init());
    const auto source = f.completions->registerSource(KisCompletionDomain::CpuJob);
    const auto ticket = delayed ? f.completions->allocatePending(source) : KisCompletionTicket{};
    f.provider->deferredRetirement = ticket;
    f.provider->rejectNativeBinding = true;
    f.provider->rejectRetire = true;
    const auto tx = f.store->beginCurrentTransaction();
    auto segment = f.begin(tx);
    QVERIFY(!segment.beginWrite(key(), &f.error).isValid());
    QVERIFY(segment.cancel());
    QVERIFY(f.store->abort(tx));
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(1));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(64 * 64 * f.bpp));
    f.provider->rejectRetire = false;
    // This rejected provider result owns only an orphan reservation. No new
    // edit or explicit processing may be needed after the transient rejection.
    if (delayed) {
        QTRY_COMPARE(f.completions->sourceStatistics(source).readinessWaiters, quint64(1));
        QCOMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(1));
        const auto calls = f.provider->retireCalls.load();
        QVERIFY(f.completions->complete(ticket, KisCompletionStatus::Succeeded));
        QVERIFY(f.store->waitForRetirementIdle());
        QCOMPARE(f.provider->retireCalls.load(), calls);
    }
    QTRY_COMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(0));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::defaultMaterializedDuringWritePreparation()
{
    QFETCH(bool, generic);
    Fixture f; QVERIFY(f.init());
    std::unique_ptr<KisReadLease> concurrentBefore;
    f.provider->beforePrepareWrite = [&] {
        const auto read = f.store->acquireRead(key(), {}, cpu, KisPagePriority::Normal);
        if (read.isValid()) concurrentBefore = std::make_unique<KisReadLease>(f.store->resolve(read, read.readiness));
    };
    const auto tx = f.store->beginCurrentTransaction();
    auto segment = generic ? KisPageMutationSession{} : f.begin(tx);
    auto guard = generic ? KisCpuWriteGuard{} : segment.beginWrite(key(), &f.error);
    const auto request = generic ? f.store->acquireWrite(tx, key(), cpu, KisPageWriteMode::PreserveContents,
                                                        KisPagePriority::Normal) : KisWriteRequest{};
    if (generic) QVERIFY2(request.isValid(), qPrintable(request.error));
    auto lease = generic ? f.store->resolve(request, request.readiness) : KisWriteLease{};
    void *data = generic ? lease.cpuData() : guard.data(); QVERIFY2(data, qPrintable(f.error));
    QVERIFY(concurrentBefore && concurrentBefore->isValid());
    static_cast<quint8 *>(data)[0] = 0x77;
    QCOMPARE(static_cast<const quint8 *>(concurrentBefore->cpuData())[0], quint8(0x2a));
    QCOMPARE(f.provider->p->payloadWork().nativeDuplicatePages, quint64(0));
    guard = {};
    if (generic) QVERIFY(f.store->publishHostWrite(std::move(lease)).isValid());
    else QVERIFY2(segment.seal(&f.error), qPrintable(f.error));
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QCOMPARE(static_cast<const quint8 *>(concurrentBefore->cpuData())[0], quint8(0x2a));
    f.store->release(std::move(*concurrentBefore));
    concurrentBefore.reset();
    f.provider->beforePrepareWrite = {};
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x77));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::nativeSessionDoesNotMaterializeDefaultBeforeImage()
{
    for (int bpp : {1, 4, 8, 16}) {
        Fixture f; QVERIFY(f.init(bpp));
        const auto tx = f.store->beginCurrentTransaction();
        KisPageStoreCpuAccessSession session;
        QVERIFY(session.beginWrite(f.store.get(), {1}, tx, KisPageWriteMode::PreserveContents,
                                   KisPagePriority::Interactive, &f.error));
        const auto span = session.writePage(0, 0, &f.error);
        QVERIFY2(span.isValid(), qPrintable(f.error));
        QCOMPARE(span.oldData[0], quint8(0x2a));
        span.data[0] = 0x77;
        QCOMPARE(span.oldData[0], quint8(0x2a));
        QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(64 * 64 * bpp));
        QCOMPARE(f.provider->p->payloadWork().defaultInitializedPages, quint64(1));
        QCOMPARE(f.store->sessionStats().readRequestsCreated, quint64(0));
        QCOMPARE(f.store->sessionStats().writeRequestsCreated, quint64(0));
        QCOMPARE(f.store->sessionStats().defaultMaterializationRequests, quint64(0));
        QVERIFY(session.finish(&f.error));
        QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
        QCOMPARE(quint8(f.pixel()[0]), quint8(0x77));
        QVERIFY(f.store->closeSession());
    }
}

void KisPageStoreCpuMutationTest::busyIsNotAMaterializationFallback()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto version = f.provider->lastTarget.version;
    auto binding = f.provider->cpuResidentBinding(f.provider->lastTarget);
    QVERIFY(binding); QVERIFY(binding->acquireWrite(f.provider->lastTarget.allocationIdentity()));
    KisPageStoreCpuAccessSession session;
    QVERIFY(session.beginRead(f.store.get(), {1}, {}, KisPagePriority::Normal, &f.error));
    const auto before = f.store->sessionStats();
    QVERIFY(!session.readPage(version.key.page.column, version.key.page.row, &f.error).isValid());
    QVERIFY(f.error.contains(QStringLiteral("busy")));
    QCOMPARE(f.store->sessionStats().readRequestsCreated, before.readRequestsCreated);
    QCOMPARE(f.store->sessionStats().defaultMaterializationRequests, before.defaultMaterializationRequests);
    binding->releaseWrite();
    const auto span = session.readPage(version.key.page.column, version.key.page.row, &f.error);
    QVERIFY2(span.isValid(), qPrintable(f.error));
    QCOMPARE(span.data[0], quint8(0x31));
    QVERIFY(session.finish());
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::rejectedRetirementsPreserveDebtAndBudget()
{
    Fixture f;
    // Exercise the explicit-only provider contract. Autonomous providers are
    // separately tested without assuming a timer cannot run between assertions.
    f.provider->disableBackgroundRetirement = true;
    QVERIFY(f.init());
    const auto fill = [&](quint8 value) {
        const auto tx = f.store->beginCurrentTransaction();
        return KisPageStoreCpuSurfaceOps::fillRect(f.store.get(), {1}, tx,
            QRect(0, 0, 20 * 64, 64), QByteArray(f.bpp, char(value)), &f.error) &&
            f.store->commit(tx, f.store->preparedPages(tx)).isValid();
    };
    QVERIFY(fill(0x31));
    f.provider->rejectRetire = true;
    QVERIFY(fill(0x77));
    QVERIFY(f.store->waitForRetirementIdle());
    const quint64 tileBytes = 64 * 64 * f.bpp;
    const auto initialDebt = f.store->sessionStats();
    QCOMPARE(initialDebt.pendingRetiredReplicas, qsizetype(20));
    QCOMPARE(initialDebt.activeRetirementReplicas, qsizetype(0));
    QCOMPARE(initialDebt.pendingRetiredBytes, 20 * tileBytes);
    QCOMPARE(initialDebt.peakRetiredReplicas, qsizetype(20));
    QCOMPARE(initialDebt.peakRetiredBytes, 20 * tileBytes);
    QVERIFY(initialDebt.retirementRetryRequeues >= 20);
    QCOMPARE(f.provider->memoryUsage().committedBytes, 40 * tileBytes);
    const int attempts = f.provider->retireCalls.load();
    QCOMPARE(f.store->processRetirements(0).attempted, qsizetype(0));
    QCOMPARE(f.store->processRetirements(-1).attempted, qsizetype(0));
    const auto blocked = f.store->processRetirements(3);
    QCOMPARE(blocked.attempted, qsizetype(3));
    QCOMPARE(blocked.retired, qsizetype(0));
    QCOMPARE(blocked.pending, qsizetype(20));
    QCOMPARE(f.provider->retireCalls.load(), attempts + 3);
    f.provider->rejectRetire = false;
    const auto partial = f.store->processRetirements(3);
    QCOMPARE(partial.attempted, qsizetype(3));
    QCOMPARE(partial.retired, qsizetype(3));
    QCOMPARE(partial.pending, qsizetype(17));
    QCOMPARE(f.store->sessionStats().pendingRetiredBytes, 17 * tileBytes);
    const auto drained = f.store->processRetirements(100);
    QCOMPARE(drained.attempted, qsizetype(17));
    QCOMPARE(drained.retired, qsizetype(17));
    QCOMPARE(drained.pending, qsizetype(0));
    const auto drainedStats = f.store->sessionStats();
    QCOMPARE(drainedStats.activeRetirementReplicas, qsizetype(0));
    QCOMPARE(drainedStats.pendingRetiredBytes, quint64(0));
    QCOMPARE(f.provider->memoryUsage().committedBytes, 20 * tileBytes);
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x77));
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::budgetWaitersReserveInOrder()
{
    KisPageBackingLimits limits;
    limits.activePendingBytes = 10; limits.optionalCacheBytes = 4;
    KisBackingBudgetController budget(limits);
    const auto delta = [](KisBackingBudgetClass cls, qint64 bytes) {
        KisBackingBudgetDelta d; d.buckets[size_t(cls)].cpuRam = bytes; return d;
    };
    const auto active = KisBackingBudgetClass::ActivePending;
    const auto cache = KisBackingBudgetClass::OptionalCache;
    auto held = budget.reserve(delta(active, 10), nullptr); QVERIFY(held.isValid());
    budget.commitReservation(std::move(held), delta(active, 10));
    auto cacheHeld = budget.reserve(delta(cache, 4), nullptr); QVERIFY(cacheHeld.isValid());
    auto count = std::make_shared<std::atomic<int>>(0);
    auto order = std::make_shared<std::array<int, 3>>();
    const auto notified = [count, order](int id) {
        return [count, order, id] { (*order)[size_t(count->load())] = id; ++*count; };
    };
    KisBackingBudgetWaiter large, small, independent;
    QCOMPARE(budget.waitForChange(delta(active, 8), notified(0), &large), KisPageReadinessStatus::Waiting);
    QCOMPARE(budget.waitForChange(delta(active, 2), notified(1), &small), KisPageReadinessStatus::Waiting);
    QCOMPARE(budget.waitForChange(delta(cache, 4), notified(2), &independent), KisPageReadinessStatus::Waiting);
    budget.releaseLive(active, KisPageAccessDomain::CpuRam, 2);
    QVERIFY(!budget.reserve(delta(active, 2), nullptr).isValid()); // No bypass of earlier larger demand.
    cacheHeld.release();
    QTRY_COMPARE_WITH_TIMEOUT(count->load(), 1, 5000);
    QCOMPARE((*order)[0], 2); // Unrelated capacity remains usable.
    QVERIFY(!large.take().isValid()); QVERIFY(!small.take().isValid());
    auto cacheGrant = independent.take(); QVERIFY(cacheGrant.isValid());
    QVERIFY(!independent.take().isValid()); cacheGrant.release();
    budget.releaseLive(active, KisPageAccessDomain::CpuRam, 8);
    QTRY_COMPARE_WITH_TIMEOUT(count->load(), 3, 5000);
    QCOMPARE((*order)[1], 0); QCOMPARE((*order)[2], 1);
    QCOMPARE(budget.usage().buckets[size_t(active)].reserved.cpuRam, quint64(10));
    auto bigGrant = large.take(); auto smallGrant = small.take();
    QVERIFY(bigGrant.isValid() && smallGrant.isValid());
    QVERIFY(!budget.reserve(delta(active, 1), nullptr).isValid());
    bigGrant.release(); smallGrant.release();
    QCOMPARE(budget.usage().waitingRequests, quint32(0));
    QCOMPARE(budget.usage().grantedRequests, quint32(0));
    QCOMPARE(budget.usage().buckets[size_t(active)].reserved.cpuRam, quint64(0));
}

void KisPageStoreCpuMutationTest::budgetWaiterStateStorageIsChargedAndReleased()
{
    KisBackingBudgetDelta demand;
    demand.buckets[size_t(KisBackingBudgetClass::ActivePending)].cpuRam = 1;

    KisPageBackingLimits tinyLimits;
    tinyLimits.activePendingBytes = 1;
    tinyLimits.metadataArenaBytes = 1;
    KisBackingBudgetController tiny(tinyLimits);
    KisBackingBudgetWaiter rejected;
    QString error;
    QCOMPARE(tiny.waitForChange(demand, [] {}, &rejected, &error),
             KisPageReadinessStatus::Unavailable);
    QVERIFY(error.contains(QStringLiteral("exceeds capacity")));
    QVERIFY(!rejected.isValid());
    QCOMPARE(tiny.usage().buckets[
                 size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
             quint64(0));
    QCOMPARE(tiny.usage().buckets[
                 size_t(KisBackingBudgetClass::MetadataArena)].reserved.cpuRam,
             quint64(0));

    KisPageBackingLimits parentLimits;
    parentLimits.metadataArenaBytes = 16 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(parentLimits);
    // Prepare the parent's own two-slot accounting storage. It remains
    // charged while the caller deliberately retains this process controller.
    const auto warmParent = [](const auto &value) {
        auto first = value->reserve({}, nullptr);
        auto second = value->reserve({}, nullptr);
        return first.isValid() && second.isValid();
    };
    QVERIFY(warmParent(parent));
    const quint64 emptyParent = parent->usage().buckets[
        size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    QVERIFY(emptyParent > 0);

    quint64 registrationCost = 0;
    quint64 persistentControlCost = 0;
    quint64 requestStateCost = 0;
    quint64 slotStorageCost = 0;

    {
        KisPageBackingLimits childLimits;
        childLimits.activePendingBytes = 1;
        KisBackingBudgetController child(childLimits);
        QVERIFY2(child.configureSharedNonPayloadBudget(parent, &error), qPrintable(error));
        const quint64 registrationBytes = parent->usage().buckets[
            size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
        QVERIFY(registrationBytes > 0);
        registrationCost = registrationBytes;
        auto initialSlot = child.reserve({}, &error);
        QVERIFY(initialSlot.isValid());
        initialSlot.release();
        slotStorageCost = child.usage().buckets[
            size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
        QVERIFY(slotStorageCost > 0);

        KisBackingBudgetWaiter waiter;
        QCOMPARE(child.waitForChange(demand, [] {}, &waiter, &error),
                 KisPageReadinessStatus::Ready);
        QVERIFY(waiter.isValid());
        const quint64 childControlBytes = child.usage().buckets[
            size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
        QVERIFY(childControlBytes > 0);
        QCOMPARE(parent->usage().buckets[
                     size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
                 registrationBytes + childControlBytes);

        auto grant = waiter.take();
        QVERIFY(grant.isValid());
        QVERIFY(!waiter.isValid());
        const quint64 childPersistentControlBytes = child.usage().buckets[
            size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
        QVERIFY(childPersistentControlBytes > 0
                && childPersistentControlBytes < childControlBytes);
        const quint64 childStateBytes = childControlBytes - childPersistentControlBytes;
        persistentControlCost = childPersistentControlBytes;
        requestStateCost = childStateBytes;
        QCOMPARE(parent->usage().buckets[
                     size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
                 registrationBytes + childPersistentControlBytes);
        grant.release();

        auto held = child.reserve(demand, &error);
        QVERIFY2(held.isValid(), qPrintable(error));
        child.commitReservation(std::move(held), demand);
        QCOMPARE(child.waitForChange(demand, [] {}, &waiter, &error),
                 KisPageReadinessStatus::Waiting);
        QVERIFY(waiter.isValid());
        QCOMPARE(parent->usage().buckets[
                     size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
                 registrationBytes + childPersistentControlBytes + childStateBytes);
        waiter.reset();
        QCOMPARE(child.usage().buckets[
                     size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
                 childPersistentControlBytes);
        QCOMPARE(parent->usage().buckets[
                     size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
                 registrationBytes + childPersistentControlBytes);
        child.releaseLive(KisBackingBudgetClass::ActivePending,
                          KisPageAccessDomain::CpuRam, 1);
    }
    QCOMPARE(parent->usage().buckets[
                 size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
             emptyParent);

    KisBackingBudgetWaiter dormant;
    {
        KisBackingBudgetController child;
        QVERIFY2(child.configureSharedNonPayloadBudget(parent, &error), qPrintable(error));
        QCOMPARE(child.waitForChange(demand, [] {}, &dormant, &error),
                 KisPageReadinessStatus::Ready);
        QVERIFY(dormant.isValid());
        QVERIFY(parent->usage().buckets[
                    size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam > 0);
    }
    QVERIFY(!dormant.isValid());
    QVERIFY(!dormant.take().isValid());
    // Request storage is gone; the real inert context and its accounting
    // lifetime remain charged until this last handle physically releases it.
    QVERIFY(parent->usage().buckets[
                size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam > emptyParent);
    dormant.reset();
    QCOMPARE(parent->usage().buckets[
                 size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
             emptyParent);

    // Local empty-use preflight passes, but the parent allows context+wake
    // and rejects the request state. The uninstalled candidate must cancel
    // its owning wake callback before dropping its last external reference.
    KisPageBackingLimits constrainedLimits;
    constrainedLimits.metadataArenaBytes = registrationCost
        + persistentControlCost + requestStateCost - 1;
    auto constrainedParent = std::make_shared<KisBackingBudgetController>(constrainedLimits);
    QVERIFY(warmParent(constrainedParent));
    {
        KisBackingBudgetController child;
        QVERIFY2(child.configureSharedNonPayloadBudget(constrainedParent, &error), qPrintable(error));
        QCOMPARE(child.waitForChange(demand, [] {}, &rejected, &error),
                 KisPageReadinessStatus::Unavailable);
        QVERIFY(error.contains(QStringLiteral("allocation failed")));
        QVERIFY(!rejected.isValid());
        const auto childUsage = child.usage();
        QCOMPARE(childUsage.buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
                 slotStorageCost + sizeof(KisMutationStorageOwner));
        QCOMPARE(childUsage.buckets[size_t(KisBackingBudgetClass::MetadataArena)].reserved.cpuRam,
                 quint64(0));
        QCOMPARE(constrainedParent->usage().buckets[
                     size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
                 registrationCost + slotStorageCost + sizeof(KisMutationStorageOwner));
    }
    QCOMPARE(constrainedParent->usage().buckets[
                 size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
             emptyParent);

    // Concurrent first registration may prepare several contexts. Only the
    // installed context and each actual request may retain a live charge.
    KisPageBackingLimits concurrentParentLimits;
    concurrentParentLimits.metadataArenaBytes = 1024 * 1024;
    auto concurrentParent = std::make_shared<KisBackingBudgetController>(concurrentParentLimits);
    quint64 concurrentRootBytes = 0;
    {
        KisPageBackingLimits childLimits;
        childLimits.activePendingBytes = 1;
        KisBackingBudgetController child(childLimits);
        QVERIFY2(child.configureSharedNonPayloadBudget(concurrentParent, &error), qPrintable(error));
        auto held = child.reserve(demand, &error);
        QVERIFY(held.isValid());
        constexpr size_t contenders = 8;
        std::array<KisBackingBudgetWaiter, contenders> waiters;
        std::array<KisPageReadinessStatus, contenders> statuses;
        std::array<std::thread, contenders> threads;
        QSemaphore start;
        for (size_t i = 0; i < contenders; ++i) {
            threads[i] = std::thread([&, i] {
                start.acquire();
                statuses[i] = child.waitForChange(demand, [] {}, &waiters[i]);
            });
        }
        start.release(int(contenders));
        for (auto &thread : threads) thread.join();
        for (const auto status : statuses) QCOMPARE(status, KisPageReadinessStatus::Waiting);
        const auto childBytes = child.usage().buckets[
            size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
        const auto contextBytes = persistentControlCost - slotStorageCost;
        const auto slotsBytes = childBytes - contextBytes - contenders * requestStateCost;
        QVERIFY(slotsBytes >= (contenders + 1) * slotStorageCost);
        QVERIFY(slotsBytes <= 32 * slotStorageCost);
        QCOMPARE(slotsBytes % slotStorageCost, quint64(0));
        concurrentRootBytes = concurrentParent->usage().buckets[
            size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam
            - childBytes - (registrationCost - emptyParent);
        QVERIFY(concurrentRootBytes >= slotStorageCost);
        QVERIFY(concurrentRootBytes <= 32 * slotStorageCost);
        for (auto &waiter : waiters) waiter.reset();
        QCOMPARE(child.usage().waitingRequests, quint32(0));
    }
    QTRY_COMPARE(concurrentParent->usage().buckets[
                 size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
             concurrentRootBytes);
}

void KisPageStoreCpuMutationTest::budgetWaiterCancellationAndLifetime()
{
    KisPageBackingLimits limits; limits.activePendingBytes = 1;
    auto budget = std::make_unique<KisBackingBudgetController>(limits);
    KisBackingBudgetDelta delta; delta.buckets[size_t(KisBackingBudgetClass::ActivePending)].cpuRam = 1;
    auto held = budget->reserve(delta, nullptr); QVERIFY(held.isValid());
    auto calls = std::make_shared<std::atomic<int>>(0);
    std::array<KisBackingBudgetWaiter, 32> waiting;
    for (auto &waiter : waiting)
        QCOMPARE(budget->waitForChange(delta, [calls] { ++*calls; }, &waiter), KisPageReadinessStatus::Waiting);
    KisBackingBudgetWaiter excess;
    QCOMPARE(budget->waitForChange(delta, [calls] { ++*calls; }, &excess), KisPageReadinessStatus::Unavailable);
    QVERIFY(!budget->configureLimits(limits, nullptr));
    auto moved = std::move(waiting[0]); QVERIFY(!waiting[0].isValid()); QVERIFY(moved.isValid());
    moved.reset();
    QCOMPARE(budget->waitForChange(delta, [calls] { ++*calls; }, &excess), KisPageReadinessStatus::Waiting);
    for (auto &waiter : waiting) waiter.reset();
    QCOMPARE(budget->usage().waitingRequests, quint32(1));
    held.release();
    QTRY_COMPARE_WITH_TIMEOUT(calls->load(), 1, 5000);
    QCOMPARE(budget->usage().grantedRequests, quint32(1));
    budget.reset(); // A dormant granted handle must not retain/dereference the controller.
    QVERIFY(!excess.isValid()); QVERIFY(!excess.take().isValid()); excess.reset();
}

void KisPageStoreCpuMutationTest::budgetWaiterQueuedCancellation()
{
    QFETCH(bool, granted);
    KisPageBackingLimits limits; limits.activePendingBytes = 1;
    auto budget = std::make_unique<KisBackingBudgetController>(limits);
    KisBackingBudgetDelta demand; demand.buckets[size_t(KisBackingBudgetClass::ActivePending)].cpuRam = 1;
    auto held = budget->reserve(demand, nullptr); QVERIFY(held.isValid());
    const auto entered = std::make_shared<QSemaphore>();
    const auto proceed = std::make_shared<QSemaphore>();
    const auto finished = std::make_shared<QSemaphore>();
    const auto calls = std::make_shared<std::atomic<int>>(0);
    auto consumer = std::make_shared<int>(1);
    const std::weak_ptr<int> weakConsumer = consumer;
    const auto cleanup = qScopeGuard([&] { proceed->release(); kisDrainPageStoreReclamation(); });
    if (!granted) {
        kisSchedulePageStoreReclamation([entered, proceed] { entered->release(); proceed->acquire(); });
        QVERIFY(entered->tryAcquire(1, 5000));
    }
    KisBackingBudgetWaiter waiter;
    QCOMPARE(budget->waitForChange(demand, [granted, entered, proceed, weakConsumer, calls] {
        if (granted) { entered->release(); proceed->acquire(); }
        // A copied callback has no authority after consumer incarnation dies.
        if (weakConsumer.lock()) ++*calls;
    }, &waiter), KisPageReadinessStatus::Waiting);
    held.release();
    if (granted) QVERIFY(entered->tryAcquire(1, 5000));
    waiter.reset(); // Also proves callbacks do not retain either controller gate.
    QCOMPARE(budget->usage().waitingRequests, quint32(0));
    QCOMPARE(budget->usage().grantedRequests, quint32(0));
    QCOMPARE(budget->usage().buckets[size_t(KisBackingBudgetClass::ActivePending)].reserved.cpuRam, quint64(0));
    consumer.reset(); budget.reset();
    proceed->release();
    kisSchedulePageStoreReclamation([finished] { finished->release(); });
    QVERIFY(finished->tryAcquire(1, 5000));
    QCOMPARE(calls->load(), 0);
}

void KisPageStoreCpuMutationTest::budgetWaiterSignedDurableTransfer()
{
    KisPageBackingLimits limits; limits.durableStoreCapacity = 10;
    limits.residentHistoryBytes.ssd = 10;
    KisBackingBudgetController budget(limits);
    const auto current = size_t(KisBackingBudgetClass::Current);
    const auto history = size_t(KisBackingBudgetClass::RetainedHistory);
    KisBackingBudgetDelta initial; initial.buckets[current].ssd = 10;
    auto backing = budget.reserve(initial, nullptr); QVERIFY(backing.isValid());
    budget.commitReservation(std::move(backing), initial);
    KisBackingBudgetDelta move; move.buckets[current].ssd = -10; move.buckets[history].ssd = 10;
    auto calls = std::make_shared<std::atomic<int>>(0);
    KisBackingBudgetWaiter waiter;
    QCOMPARE(budget.waitForChange(move, [calls] { ++*calls; }, &waiter), KisPageReadinessStatus::Ready);
    auto grant = waiter.take(); QVERIFY(grant.isValid());
    budget.commitReservation(std::move(grant), move);
    QCOMPARE(budget.usage().buckets[history].live.ssd, quint64(10));
    QCOMPARE(budget.usage().buckets[current].live.ssd, quint64(0));
    QCOMPARE(calls->load(), 0); // Ready never invokes user code inline.
    KisBackingBudgetDelta impossible; impossible.buckets[history].ssd = 11;
    QCOMPARE(budget.waitForChange(impossible, [calls] { ++*calls; }, &waiter), KisPageReadinessStatus::Unavailable);
    QVERIFY(!waiter.isValid());
    budget.releaseLive(KisBackingBudgetClass::RetainedHistory, KisPageAccessDomain::Ssd, 10);
}

void KisPageStoreCpuMutationTest::budgetWaiterCommitReleasesUnusedCapacity()
{
    KisPageBackingLimits limits; limits.activePendingBytes = 10;
    KisBackingBudgetController budget(limits);
    const auto cls = KisBackingBudgetClass::ActivePending;
    KisBackingBudgetDelta ten; ten.buckets[size_t(cls)].cpuRam = 10;
    KisBackingBudgetDelta four; four.buckets[size_t(cls)].cpuRam = 4;
    KisBackingBudgetDelta six; six.buckets[size_t(cls)].cpuRam = 6;
    auto reserved = budget.reserve(ten, nullptr); QVERIFY(reserved.isValid());
    auto calls = std::make_shared<std::atomic<int>>(0);
    KisBackingBudgetWaiter waiter;
    QCOMPARE(budget.waitForChange(six, [calls] { ++*calls; }, &waiter), KisPageReadinessStatus::Waiting);
    budget.commitReservation(std::move(reserved), four);
    QTRY_COMPARE_WITH_TIMEOUT(calls->load(), 1, 5000);
    auto granted = waiter.take(); QVERIFY(granted.isValid());
    QCOMPARE(budget.usage().buckets[size_t(cls)].live.cpuRam, quint64(4));
    QCOMPARE(budget.usage().buckets[size_t(cls)].reserved.cpuRam, quint64(6));
    granted.release(); budget.releaseLive(cls, KisPageAccessDomain::CpuRam, 4);
}

void KisPageStoreCpuMutationTest::budgetReservationRecyclingPreservesHeldGrants()
{
    constexpr size_t count = 128;
    const auto active = KisBackingBudgetClass::ActivePending;
    KisPageBackingLimits limits;
    limits.activePendingBytes = count;
    KisBackingBudgetController budget(limits);
    KisBackingBudgetDelta demand;
    demand.buckets[size_t(active)].cpuRam = 1;
    std::array<KisBackingBudgetReservation, count> held;
    quint64 oneSlotBytes = 0;
    for (auto &reservation : held) {
        reservation = budget.reserve(demand, nullptr);
        QVERIFY(reservation.isValid());
        if (!oneSlotBytes) oneSlotBytes = budget.usage().buckets[
            size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    }
    QVERIFY(oneSlotBytes > 0);
    const auto storageBytes = count * oneSlotBytes;
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
             storageBytes);
    for (size_t parity = 0; parity < 2; ++parity) {
        for (size_t i = parity; i < count; i += 2) held[i].release();
        QCOMPARE(budget.usage().buckets[size_t(active)].reserved.cpuRam, quint64(count / 2));
        for (size_t i = parity; i < count; i += 2) {
            held[i] = budget.reserve(demand, nullptr);
            QVERIFY(held[i].isValid());
        }
        QCOMPARE(budget.usage().buckets[size_t(active)].reserved.cpuRam, quint64(count));
        QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
                 storageBytes);
        QVERIFY(!budget.reserve(demand, nullptr).isValid());
    }
    // A move assignment releases one slot while preserving the transferred
    // grant and all its neighbours. Commit and reuse that released capacity.
    held[0] = std::move(held[1]);
    QVERIFY(!held[1].isValid());
    QCOMPARE(budget.usage().buckets[size_t(active)].reserved.cpuRam, quint64(count - 1));
    budget.commitReservation(std::move(held[0]), demand);
    QCOMPARE(budget.usage().buckets[size_t(active)].live.cpuRam, quint64(1));
    QCOMPARE(budget.usage().buckets[size_t(active)].reserved.cpuRam, quint64(count - 2));
    budget.releaseLive(active, KisPageAccessDomain::CpuRam, 1);
    for (auto &reservation : held) reservation.release();
    QCOMPARE(budget.usage().buckets[size_t(active)].reserved.cpuRam, quint64(0));
    auto reused = budget.reserve(demand, nullptr);
    QVERIFY(reused.isValid());
    reused.release();
    QCOMPARE(budget.usage().buckets[size_t(active)].live.cpuRam, quint64(0));
    QCOMPARE(budget.usage().buckets[size_t(active)].reserved.cpuRam, quint64(0));

    // Metadata is exactly full after one slot. Growth must account both old
    // and new arrays, reject without touching a held grant, and reuse the
    // existing slot after release without needing any spare metadata bytes.
    KisPageBackingLimits constrained;
    constrained.activePendingBytes = 2;
    constrained.metadataArenaBytes = oneSlotBytes;
    KisBackingBudgetController bounded(constrained);
    auto first = bounded.reserve(demand, nullptr);
    QVERIFY(first.isValid());
    QString error;
    QVERIFY(!bounded.reserve(demand, &error).isValid());
    QVERIFY(error.contains(QStringLiteral("storage exceeds")));
    QCOMPARE(bounded.usage().buckets[size_t(active)].reserved.cpuRam, quint64(1));
    QCOMPARE(bounded.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
             oneSlotBytes);
    QCOMPARE(bounded.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].reserved.cpuRam,
             quint64(0));
    first.release();
    auto again = bounded.reserve(demand, &error);
    QVERIFY2(again.isValid(), qPrintable(error));
    bounded.commitReservation(std::move(again), demand);
    QCOMPARE(bounded.usage().buckets[size_t(active)].live.cpuRam, quint64(1));
    bounded.releaseLive(active, KisPageAccessDomain::CpuRam, 1);

    // Two stores charge their real slot arrays to the same process owner.
    // Growing either store cannot bypass the process cap; teardown returns
    // its array charge and the child registration, while a deliberately
    // retained root keeps its own array accounted until its destruction.
    KisPageBackingLimits processLimits;
    processLimits.metadataArenaBytes = oneSlotBytes * 4;
    auto process = std::make_shared<KisBackingBudgetController>(processLimits);
    quint64 rootBytes = 0;
    {
        KisBackingBudgetController left;
        KisBackingBudgetController right;
        QVERIFY(left.configureSharedNonPayloadBudget(process));
        QVERIFY(right.configureSharedNonPayloadBudget(process));
        auto leftGrant = left.reserve(demand, &error);
        auto rightGrant = right.reserve(demand, &error);
        QVERIFY2(leftGrant.isValid() && rightGrant.isValid(), qPrintable(error));
        const auto before = process->usage().buckets[
            size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
        QVERIFY(before >= 3 * oneSlotBytes && before <= processLimits.metadataArenaBytes);
        QVERIFY(!left.reserve(demand, &error).isValid());
        QVERIFY(error.contains(QStringLiteral("process budget")));
        QCOMPARE(process->usage().buckets[
                     size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam, before);
        QCOMPARE(process->usage().buckets[
                     size_t(KisBackingBudgetClass::MetadataArena)].reserved.cpuRam, quint64(0));
        leftGrant.release(); rightGrant.release();
        auto reusable = right.reserve(demand, &error);
        QVERIFY2(reusable.isValid(), qPrintable(error));
        rootBytes = oneSlotBytes;
    }
    QCOMPARE(process->usage().buckets[
                 size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam, rootBytes);
    const std::weak_ptr<KisBackingBudgetController> rootLifetime(process);
    process.reset();
    QVERIFY(rootLifetime.expired());

    // A retained slot cache is irreducible until this controller exits.
    // A demand which fits a fresh controller must be permanently refused,
    // not queued forever, when a past concurrency peak leaves insufficient
    // metadata for that demand plus the actual cached control storage.
    KisPageBackingLimits cachedLimits;
    cachedLimits.metadataArenaBytes = 64 * oneSlotBytes;
    KisBackingBudgetDelta metadataDemand;
    metadataDemand.buckets[size_t(KisBackingBudgetClass::MetadataArena)].cpuRam =
        qint64(cachedLimits.metadataArenaBytes - 8 * oneSlotBytes);
    {
        KisBackingBudgetController fresh(cachedLimits);
        KisBackingBudgetWaiter ready;
        QCOMPARE(fresh.waitForChange(metadataDemand, [] {}, &ready, &error),
                 KisPageReadinessStatus::Ready);
        auto grant = ready.take();
        QVERIFY(grant.isValid());
    }
    KisBackingBudgetController cached(cachedLimits);
    std::array<KisBackingBudgetReservation, 32> peak;
    for (auto &grant : peak) {
        grant = cached.reserve(demand, &error);
        QVERIFY2(grant.isValid(), qPrintable(error));
    }
    for (auto &grant : peak) grant.release();
    const auto chargedCache = cached.usage().buckets[
        size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    KisBackingBudgetWaiter impossible;
    QCOMPARE(cached.waitForChange(metadataDemand, [] {}, &impossible, &error),
             KisPageReadinessStatus::Unavailable);
    QVERIFY(error.contains(QStringLiteral("exceeds capacity")));
    QVERIFY(!impossible.isValid());
    QCOMPARE(cached.usage().waitingRequests, quint32(0));
    QCOMPARE(cached.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
             chargedCache);
}

void KisPageStoreCpuMutationTest::terminalReclamationStorageAtCapacity()
{
    quint64 fullBytes = 0;
    {
        Fixture probe; QVERIFY(probe.init());
        auto view = probe.store->captureReadView(); QVERIFY(view.isValid());
        const auto tx = probe.store->beginCurrentTransaction();
        auto scope = probe.begin(tx); QVERIFY(scope.isActive());
        fullBytes = probe.store->backingUsage()
            .buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
        QVERIFY(scope.cancel()); scope = {}; view = {};
        QVERIFY(probe.store->abort(tx));
        QVERIFY(probe.store->closeSession());
    }
    Fixture f;
    KisPageBackingLimits limits;
    limits.metadataArenaBytes = fullBytes;
    QVERIFY(f.store->configureBackingLimits(limits));
    QVERIFY2(f.init(), qPrintable(f.error));
    auto view = f.store->captureReadView(); QVERIFY(view.isValid());
    const auto tx = f.store->beginCurrentTransaction();
    auto scope = f.begin(tx); QVERIFY(scope.isActive());
    QCOMPARE(f.store->backingUsage()
        .buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam, fullBytes);
    QSemaphore entered, resume;
    kisSchedulePageStoreReclamation([&] { entered.release(); resume.acquire(); });
    const auto unblock = qScopeGuard([&] { resume.release(); kisDrainPageStoreReclamation(); });
    QVERIFY(entered.tryAcquire(1, 5000));
    const std::weak_ptr<TestProvider> lifetime(f.provider);
    f.provider.reset(); f.store.reset();
    QVERIFY(!lifetime.expired());
    QVERIFY(scope.cancel()); scope = {}; view = {};
    QVERIFY(!lifetime.expired()); // Queued maintenance/terminal still owns the root.
    resume.release();
    kisDrainPageStoreReclamation();
    QVERIFY(lifetime.expired());
}

void KisPageStoreCpuMutationTest::preparedReclamationWakeCancellation()
{
    KisPageBackingLimits limits;
    limits.metadataArenaBytes = 4096;
    KisBackingBudgetController budget(limits);
    const auto metadataLive = [&] {
        return budget.usage().buckets[
            size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    auto preparation = budget.reserve({}, nullptr);
    QVERIFY(preparation.isValid());
    preparation.release();
    const quint64 retainedSlotBytes = metadataLive();
    QVERIFY(retainedSlotBytes > 0);
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto payload = std::make_shared<int>(42); std::weak_ptr<int> weak = payload;
    KisPageReadinessCallback firstDispatch([calls, payload] { ++*calls; }, &budget);
    const quint64 firstCaptureBytes = firstDispatch.storageBytes();
    auto wake = kisPreparePageStoreReclamationWake(std::move(firstDispatch), &budget);
    const quint64 wakeBytes = metadataLive();
    QVERIFY(wakeBytes > 0);
    payload.reset(); QVERIFY(wake.isValid()); QVERIFY(!weak.expired());
    auto moved = std::move(wake); QVERIFY(!wake.isValid());
    moved.reset(); QVERIFY(weak.expired());
    const quint64 retainedControlBytes = retainedSlotBytes + sizeof(KisMutationStorageOwner);
    QCOMPARE(metadataLive(), retainedControlBytes);
    QCOMPARE(calls->load(), 0);
    KisPageReadinessCallback readyDispatch([calls] { ++*calls; }, &budget);
    const quint64 readyBytes = wakeBytes - firstCaptureBytes + readyDispatch.storageBytes();
    auto ready = kisPreparePageStoreReclamationWake(std::move(readyDispatch), &budget);
    QCOMPARE(metadataLive(), readyBytes);
    ready.notify(); QTRY_COMPARE_WITH_TIMEOUT(calls->load(), 1, 5000);
    ready.notify(); QTRY_COMPARE_WITH_TIMEOUT(calls->load(), 2, 5000);
    ready.reset(); ready.notify(); QCOMPARE(calls->load(), 2);
    QTRY_COMPARE(metadataLive(), retainedControlBytes);

    auto entered = std::make_shared<QSemaphore>();
    auto resume = std::make_shared<QSemaphore>();
    auto finished = std::make_shared<QSemaphore>();
    auto retained = std::make_shared<int>(7);
    std::weak_ptr<int> retainedWeak = retained;
    KisPageReadinessCallback inFlightDispatch([entered, resume, finished, retained] {
        entered->release(); resume->acquire(); finished->release();
    }, &budget);
    const quint64 inFlightBytes = wakeBytes - firstCaptureBytes + inFlightDispatch.storageBytes();
    auto inFlight = kisPreparePageStoreReclamationWake(std::move(inFlightDispatch), &budget);
    retained.reset();
    QCOMPARE(metadataLive(), inFlightBytes);
    inFlight.notify();
    QVERIFY(entered->tryAcquire(1, 5000));
    inFlight.reset();
    QCOMPARE(metadataLive(), inFlightBytes); // Monitor still owns the real wake and capture.
    QVERIFY(!retainedWeak.expired());
    resume->release();
    QVERIFY(finished->tryAcquire(1, 5000));
    QTRY_VERIFY_WITH_TIMEOUT(retainedWeak.expired(), 5000);
    QTRY_COMPARE(metadataLive(), retainedControlBytes);
}

void KisPageStoreCpuMutationTest::retirementRecordRefusedBeforePhysicalResult()
{
    Fixture f; QVERIFY(f.init());
    KisSurfaceEpochState surface; QVERIFY(f.store->resolveSurfaceState({1}, {}, &surface));
    KisPageBackingLimits limits; limits.metadataArenaBytes = 64 * 1024;
    KisBackingBudgetController budget(limits);
    KisPageOwnerLedger owner; QVERIFY(owner.configure(f.completions));
    owner.attachBackingBudget(budget); QVERIFY(owner.registerProvider(f.provider));
    KisPageMetadataCoordinator metadata; QVERIFY(metadata.configure(1));
    KisImageEpochReferenceModel epochs;
    KisPageWriteCoordinator coordinator(metadata, epochs, budget, owner);
    auto first = budget.reserve({}, nullptr), second = budget.reserve({}, nullptr);
    QVERIFY(first.isValid() && second.isValid()); first.release(); second.release();
    auto accounting = KisMutationStorageAllocator<char>::retained(&budget);
    const auto live = [&] { return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const quint64 baseline = live();
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - baseline - sizeof(KisPageRetirementRecord) + 1);
    void *filler = kisAllocateMutationStorage(&budget, fillerBytes, 1);
    const auto release = qScopeGuard([&] { kisFreeMutationStorage(&budget, filler, fillerBytes, 1); });
    int physicalCalls = 0; f.provider->beforePrepareWrite = [&] { ++physicalCalls; };
    QString error;
    auto rejected = coordinator.reserveBacking(surface.allocationDescriptor(), KisPageAccessDomain::CpuRam,
        KisBackingBudgetClass::Current, &error);
    QVERIFY(!rejected.reservation.isValid() && !rejected.retirement);
    QVERIFY2(error.contains(QStringLiteral("retirement record budget")), qPrintable(error));
    QCOMPARE(physicalCalls, 0);
    QCOMPARE(live(), baseline + fillerBytes);
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::Current)].reserved.cpuRam, quint64(0));
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].reserved.cpuRam, quint64(0));
    kisFreeMutationStorage(&budget, std::exchange(filler, nullptr), fillerBytes, 1);
    {
        auto prepared = coordinator.reserveBacking(surface.allocationDescriptor(), KisPageAccessDomain::CpuRam,
            KisBackingBudgetClass::Current, &error);
        QVERIFY2(prepared.reservation.isValid() && prepared.retirement, qPrintable(error));
        QVERIFY(live() > baseline + sizeof(KisPageRetirementRecord)); // Includes the prepaid operation node.
    }
    QCOMPARE(live(), baseline);
}

void KisPageStoreCpuMutationTest::ledgerBackingNodesRefusedBeforePhysicalResult()
{
    QFETCH(bool, physicalNode);
    Fixture f; QVERIFY(f.init(4, 1));
    KisSurfaceEpochState surface; QVERIFY(f.store->resolveSurfaceState({1}, {}, &surface));
    KisPageBackingLimits limits; limits.metadataArenaBytes = 64 * 1024;
    KisBackingBudgetController budget(limits);
    KisPageOwnerLedger owner; QVERIFY(owner.configure(f.completions));
    owner.attachBackingBudget(budget); QVERIFY(owner.registerProvider(f.provider));
    KisPageMetadataCoordinator metadata; QVERIFY(metadata.configure(1));
    KisImageEpochReferenceModel epochs;
    KisPageWriteCoordinator coordinator(metadata, epochs, budget, owner);
    const auto live = [&] { return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    auto warmFirst = budget.reserve({}, nullptr), warmSecond = budget.reserve({}, nullptr);
    QVERIFY(warmFirst.isValid() && warmSecond.isValid()); warmFirst.release(); warmSecond.release();
    { auto warm = kisPreparePageRetirementRecord(&budget); }
    const quint64 baseline = live();
    quint64 allBytes = 0, physicalBytes = 0, backingBytes = 0;
    {
        auto probe = kisPreparePageRetirementRecord(&budget);
        allBytes = live() - baseline;
        probe->physicalStorage = {};
        physicalBytes = baseline + allBytes - live();
        const auto before = live();
        probe->backingStorage = {};
        backingBytes = before - live();
    }
    QCOMPARE(live(), baseline);
    QVERIFY(physicalBytes && backingBytes);
    const quint64 room = physicalNode ? allBytes - 1 : allBytes - physicalBytes - backingBytes;
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - baseline - room);
    void *filler = kisAllocateMutationStorage(&budget, fillerBytes, 1);
    const auto release = qScopeGuard([&] { kisFreeMutationStorage(&budget, filler, fillerBytes, 1); });
    int physicalCalls = 0; f.provider->beforePrepareWrite = [&] { ++physicalCalls; };
    QString error;
    auto rejected = coordinator.reserveBacking(surface.allocationDescriptor(), cpu.domain,
        KisBackingBudgetClass::Current, &error);
    QVERIFY(!rejected.reservation.isValid() && !rejected.retirement);
    QVERIFY2(error.contains(QStringLiteral("retirement record budget")), qPrintable(error));
    QCOMPARE(physicalCalls, 0);
    QCOMPARE(live(), baseline + fillerBytes); // Every partially prepared actual node was freed.
    for (const auto &bucket : budget.usage().buckets) QCOMPARE(bucket.reserved.cpuRam, quint64(0));

    // Direct adoption already has a valid physical result. A real node refusal
    // leaves that exact result, its caller reservation and the ledger intact.
    const auto original = f.initialReplicas.first();
    KisBackingBudgetDelta delta;
    delta.buckets[size_t(KisBackingBudgetClass::Current)].cpuRam = qint64(original.layout.byteSize);
    auto reservation = budget.reserve(delta, &error); QVERIFY(reservation.isValid());
    const auto committed = f.provider->memoryUsage().committedBytes;
    QVERIFY(!owner.registerBacking(original, reservation, KisBackingBudgetClass::Current, &error));
    QVERIFY(reservation.isValid());
    QCOMPARE(owner.backingClass(original), KisBackingBudgetClass::Count);
    QVERIFY(f.provider->validate(original, surface.allocationDescriptor()));
    QCOMPARE(f.provider->memoryUsage().committedBytes, committed);
    QCOMPARE(live(), baseline + fillerBytes);
    kisFreeMutationStorage(&budget, std::exchange(filler, nullptr), fillerBytes, 1);
    QVERIFY2(owner.registerBacking(original, reservation, KisBackingBudgetClass::Current, &error), qPrintable(error));
    QCOMPARE(physicalCalls, 0); // Resume registration; never recreate the provider result.
    auto record = owner.takeRetirementRecord(original);
    QVERIFY(record && record->backingStorage.empty() && record->physicalStorage.empty());
    owner.releaseRetiredBacking(original);
    // Initial-adoption rollback hands the original terminal record back to its
    // caller. Refilling its consumed installation nodes is cold and fallible.
    auto *identity = record.get();
    const size_t refillBytes = size_t(limits.metadataArenaBytes - live());
    void *refill = kisAllocateMutationStorage(&budget, refillBytes, 1);
    const auto releaseRefill = qScopeGuard([&] { kisFreeMutationStorage(&budget, refill, refillBytes, 1); });
    auto refusedRefill = coordinator.reserveBacking(surface.allocationDescriptor(), cpu.domain,
        KisBackingBudgetClass::Current, &error, {}, &record);
    QVERIFY(!refusedRefill.reservation.isValid()); QCOMPARE(record.get(), identity);
    QCOMPARE(live(), limits.metadataArenaBytes);
    QVERIFY(f.provider->validate(original, surface.allocationDescriptor()));
    kisFreeMutationStorage(&budget, std::exchange(refill, nullptr), refillBytes, 1);
    auto retry = coordinator.reserveBacking(surface.allocationDescriptor(), cpu.domain,
        KisBackingBudgetClass::Current, &error, {}, &record);
    QVERIFY2(retry.reservation.isValid(), qPrintable(error)); QCOMPARE(retry.retirement.get(), identity);
    QVERIFY(owner.registerBacking(original, retry.reservation, KisBackingBudgetClass::Current, &error, &retry.retirement));
    record = owner.takeRetirementRecord(original); QCOMPARE(record.get(), identity);
    owner.releaseRetiredBacking(original); retry.reservation.release();
    record.reset();
    QCOMPARE(live(), baseline);
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::ledgerBackingRegistrationExcludesDomainMovement()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    KisSurfaceEpochState surface; QVERIFY(f.store->resolveSurfaceState({1}, {}, &surface));
    const auto original = f.provider->lastTarget;
    auto view = f.store->captureReadView(); auto read = view.readResidentPage(original.version.key);
    QVERIFY(read.isValid()); auto *tile = f.provider->p->tileDataForCpuReadGuard(read); QVERIFY(tile && tile->ref());
    const auto releaseTile = qScopeGuard([&] { tile->deref(); });
    read = {}; view = {};
    auto source = f.provider->p->captureCompletedTileSource(surface.allocationDescriptor(), tile); QVERIFY(source);
    KisPageBackingLimits limits; limits.metadataArenaBytes = 64 * 1024;
    KisBackingBudgetController budget(limits);
    KisPageOwnerLedger owner; QVERIFY(owner.configure(f.completions)); owner.attachBackingBudget(budget);
    QVERIFY(owner.registerProvider(f.provider));
    KisBackingBudgetDelta delta;
    delta.buckets[size_t(KisBackingBudgetClass::Current)].cpuRam = qint64(original.layout.byteSize);
    auto reservation = budget.reserve(delta, &f.error), aliasReservation = budget.reserve(delta, &f.error);
    QVERIFY(reservation.isValid() && aliasReservation.isValid());
    auto record = kisPreparePageRetirementRecord(&budget), aliasRecord = kisPreparePageRetirementRecord(&budget);
    const auto alias = f.provider->prepareSynchronousSource(owner.nextOperationId(), source, {key(1), {1}},
        surface.allocationDescriptor(), KisReplicaSourceUse::ImmutableAlias, KisPagePriority::Normal);
    QVERIFY(alias.isValid()); source.reset();
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - budget.usage()
        .buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam);
    void *filler = kisAllocateMutationStorage(&budget, fillerBytes, 1);
    const auto release = qScopeGuard([&] { kisFreeMutationStorage(&budget, filler, fillerBytes, 1); });
    QSemaphore entered, resume;
    std::atomic<int> footprints{0};
    f.provider->afterBackingFootprint = [&] {
        if (footprints.fetch_add(1) == 1) { entered.release(); resume.acquire(); }
    };
    bool accepted = false; QString registrationError;
    std::thread registration([&] {
        accepted = owner.registerBacking(original, reservation, KisBackingBudgetClass::Current,
            &registrationError, &record);
    });
    const auto join = qScopeGuard([&] {
        resume.release(); if (registration.joinable()) registration.join();
        f.provider->afterBackingFootprint = {};
    });
    QVERIFY(entered.tryAcquire(1, 5000)); // Second observation: original scoped marker is active.
    QCOMPARE(owner.backingClass(original), KisBackingBudgetClass::Count);
    QVERIFY(!KisTileDataStore::instance()->trySwapTileData(tile)); QVERIFY(tile->isResident());
    QVERIFY(!owner.registerBacking(alias.replica, aliasReservation, KisBackingBudgetClass::Current,
        &f.error, &aliasRecord));
    QVERIFY(f.error.contains(QStringLiteral("busy")));
    QVERIFY(aliasReservation.isValid() && aliasRecord && !aliasRecord->backingStorage.empty());
    resume.release(); registration.join(); f.provider->afterBackingFootprint = {};
    QVERIFY2(accepted, qPrintable(registrationError)); QVERIFY(!record);
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
             limits.metadataArenaBytes);
    QVERIFY2(owner.registerBacking(alias.replica, aliasReservation, KisBackingBudgetClass::Current,
        &f.error, &aliasRecord), qPrintable(f.error)); // Marker returned; same physical result and nodes.
    kisFreeMutationStorage(&budget, std::exchange(filler, nullptr), fillerBytes, 1);
    QVERIFY(KisTileDataStore::instance()->trySwapTileData(tile));
    QVERIFY(owner.synchronizeBackingDomains(&f.error));
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::Current)].live.ssd, original.layout.byteSize);
    owner.releaseRetiredBacking(original);
    QVERIFY(f.provider->retire(owner.nextOperationId(), alias.replica, {}).isValid());
    owner.releaseRetiredBacking(alias.replica);
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::retirementRecordTransfersAtCapacity()
{
    QFETCH(bool, orphan);
    QFETCH(bool, automatic);
    QFETCH(bool, waitForBudget);
    Fixture f; QVERIFY(f.init());
    KisSurfaceEpochState surface; QVERIFY(f.store->resolveSurfaceState({1}, {}, &surface));
    KisPageBackingLimits limits; limits.metadataArenaBytes = 64 * 1024;
    if (waitForBudget) limits.retirementDebtBytes = surface.allocationDescriptor().minimumByteSize();
    KisBackingBudgetController budget(limits);
    KisPageOwnerLedger owner; QVERIFY(owner.configure(f.completions));
    owner.attachBackingBudget(budget); QVERIFY(owner.registerProvider(f.provider));
    KisPageMetadataCoordinator metadata; QVERIFY(metadata.configure(1));
    KisImageEpochReferenceModel epochs;
    KisPageWriteCoordinator coordinator(metadata, epochs, budget, owner);
    QAtomicInt references{1};
    KisPageRetirementQueue queue(owner, metadata, budget, references, &references,
        [](void *p) { static_cast<QAtomicInt *>(p)->deref(); });
    queue.prepareTask();
    if (!automatic) queue.stopAutomaticWakeups();
    QString error;
    const auto live = [&] { return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const auto beforeRecord = live();
    quint64 recordBytes = 0, installedBytes = 0;
    {
        auto probe = kisPreparePageRetirementRecord(&budget); recordBytes = live() - beforeRecord;
        const auto before = live(); probe->backingStorage = {}; probe->physicalStorage = {};
        installedBytes = before - live();
    }
    QCOMPARE(live(), beforeRecord);
    auto prepared = coordinator.reserveBacking(surface.allocationDescriptor(), KisPageAccessDomain::CpuRam,
        KisBackingBudgetClass::Current, &error);
    QVERIFY2(prepared.reservation.isValid() && prepared.retirement, qPrintable(error));
    auto *identity = prepared.retirement.get();
    const auto result = f.provider->prepareWrite(owner.nextOperationId(), {key(5), {1}},
        surface.allocationDescriptor(), KisPageAccessDomain::CpuRam,
        KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
    QVERIFY(result.isValid());
    // All actual installation nodes were prepared before the provider result.
    // Registration and the same-node retirement handoff must work at hard full.
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - live());
    void *filler = kisAllocateMutationStorage(&budget, fillerBytes, 1);
    const auto freeFiller = qScopeGuard([&] { kisFreeMutationStorage(&budget, filler, fillerBytes, 1); });
    KisBackingBudgetReservation blockingDebt;
    if (!orphan) {
        QVERIFY(owner.registerBacking(result.replica, prepared.reservation,
            KisBackingBudgetClass::Current, &error, &prepared.retirement));
        QVERIFY(!prepared.retirement);
        if (waitForBudget) {
            prepared.reservation.release();
            KisBackingBudgetDelta delta;
            delta.buckets[size_t(KisBackingBudgetClass::RetirementDebt)].cpuRam =
                qint64(surface.allocationDescriptor().minimumByteSize());
            blockingDebt = budget.reserve(delta, &error);
            QVERIFY2(blockingDebt.isValid(), qPrintable(error));
        } else {
            QVERIFY(owner.reclassifyBacking(result.replica, KisBackingBudgetClass::RetirementDebt,
                prepared.reservation, &error));
        }
    }
    f.provider->rejectRetire = true;
    std::atomic<int> permissionQueries{0};
    f.provider->beforeCapabilities = [&] { ++permissionQueries; };
    const auto cleanup = qScopeGuard([&] {
        f.provider->beforeCapabilities = {};
        blockingDebt.release();
        f.provider->rejectRetire = false;
        queue.beginClose(); queue.waitForIdle();
        auto records = queue.takeForClose();
        for (auto &record : records) queue.retireRecord(record);
        kisDrainPageStoreReclamation();
    });
    QCOMPARE(live(), limits.metadataArenaBytes);
    queue.retireOrDefer(result.replica, f.provider, {}, std::move(prepared));
    queue.waitForIdle();
    QCOMPARE(queue.snapshot().pendingReplicas, qsizetype(1));
    QCOMPARE(live(), limits.metadataArenaBytes);
    if (automatic) {
        // Subscriber/Wait preparation has no capacity. The original admitted
        // timer must continue retrying without another process call or input.
        QTRY_VERIFY_WITH_TIMEOUT(queue.snapshot().retryWakeups >= 3, 5000);
        QCOMPARE(permissionQueries.load(), orphan ? 1 : 0);
        QCOMPARE(live(), limits.metadataArenaBytes);
        QCOMPARE(queue.snapshot().pendingReplicas, qsizetype(1));
        QCOMPARE(budget.usage().waitingRequests, quint32(0));
        if (waitForBudget) {
            QCOMPARE(owner.backingClass(result.replica), KisBackingBudgetClass::Current);
            blockingDebt.release(); // Actual capacity release, with no new request/input.
        }
        f.provider->rejectRetire = false;
        QTRY_VERIFY_WITH_TIMEOUT(queue.isDrained(), 5000);
        queue.waitForIdle(); kisDrainPageStoreReclamation();
        QCOMPARE(permissionQueries.load(), orphan ? 1 : 0);
        QCOMPARE(live(), limits.metadataArenaBytes - recordBytes);
        QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::Current)].live.cpuRam, quint64(0));
        QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].live.cpuRam, quint64(0));
        QCOMPARE(references.loadAcquire(), 1);
        return;
    }
    const auto retry = queue.process(1);
    QCOMPARE(retry.attempted, qsizetype(1)); QCOMPARE(retry.retired, qsizetype(0));
    QCOMPARE(live(), limits.metadataArenaBytes);
    queue.beginClose(); queue.waitForIdle();
    auto records = queue.takeForClose();
    QCOMPARE(records.size(), size_t(1));
    QCOMPARE(&records.front(), identity); // Original node, including orphan reservation.
    QCOMPARE(live(), limits.metadataArenaBytes);
    f.provider->rejectRetire = false;
    QVERIFY(queue.retireRecord(records.front()));
    QCOMPARE(live(), limits.metadataArenaBytes - (orphan ? 0 : installedBytes)); // Only real inert storage remains.
    records.clear_and_dispose(KisPageRetirementRecordDeleter{});
    QCOMPARE(live(), limits.metadataArenaBytes - recordBytes);
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::Current)].live.cpuRam, quint64(0));
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].live.cpuRam, quint64(0));
    QVERIFY(queue.isDrained());
    kisDrainPageStoreReclamation();
    QCOMPARE(permissionQueries.load(), orphan ? 1 : 0);
    QCOMPARE(references.loadAcquire(), 1);
}

void KisPageStoreCpuMutationTest::publicationDescriptorStorageAtCapacity()
{
    KisPageBackingLimits processLimits;
    processLimits.metadataArenaBytes = 512 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(processLimits);
    std::array<KisBackingBudgetReservation, 16> warm;
    KisBackingBudgetDelta delta;
    delta.buckets[size_t(KisBackingBudgetClass::MetadataArena)].cpuRam = 1;
    for (auto &reservation : warm) {
        reservation = parent->reserve(delta, nullptr);
        QVERIFY(reservation.isValid());
    }
    for (auto &reservation : warm) reservation.release();
    const auto parentBaseline = parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    auto fixture = std::make_unique<ReadTerminalFixture>(256 * 1024);
    auto &f = *fixture;
    QVERIFY(f.budget.configureSharedNonPayloadBudget(parent));
    f.background = false;
    f.metadata.attachBackingBudget(f.budget);
    QVERIFY2(f.init(), qPrintable(f.error));
    auto publication = std::make_unique<KisPagePublicationCoordinator>(f.epochs, f.metadata, f.owner,
        f.budget, f.retirement, f.history, f.ready, f.activeCalls, f.operational, f.background, &f,
        +[](void *, KisPageTransactionId) { return false; },
        +[](void *, const KisPageKey &) { return false; },
        +[](void *, KisPageTransactionId, QMutexLocker<QMutex> &, QString *) { return true; },
        +[](void *, KisPageTransactionId) {},
        +[](void *, KisPageMetadataCoordinator::DeferredPublicationCleanup) {},
        +[](void *) { return true; },
        +[](void *, KisPageTransactionId, KisPageReadCleanup &) { return true; });
    KisSurfaceEpochState surface;
    QVERIFY(f.epochs.root({1}).surfaceState({1}, &surface));
    const auto descriptor = surface.allocationDescriptor();
    using Commit = KisPagePublicationCoordinator::KisPreparedMutationCommit;
    std::optional<Commit> facade;
    std::optional<KisPagePublicationCoordinator::DescriptorMap> tail;
    {
        QMutexLocker lock(&f.mutex);
        auto first = publication->prepareDescriptorLocked({key(1), {2}}, descriptor);
        auto second = publication->prepareDescriptorLocked({key(2), {2}}, descriptor);
        QVERIFY(!first.empty() && !second.empty());
        QVERIFY(publication->m_descriptors.empty());
        const auto beforeFull = f.live();
        const auto fillerBytes = size_t(f.limits.metadataArenaBytes - beforeFull);
        void *filler = kisAllocateMutationStorage(&f.budget, fillerBytes, 1);
        const auto cleanupFiller = qScopeGuard([&] {
            if (filler) kisFreeMutationStorage(&f.budget, filler, fillerBytes, 1);
        });
        QCOMPARE(f.live(), f.limits.metadataArenaBytes);
        bool refused = false;
        try { auto candidate = publication->prepareDescriptorLocked({key(3), {2}}, descriptor); }
        catch (const std::bad_alloc &) { refused = true; }
        QVERIFY(refused);
        const auto registrations = f.metadata.pageRegistrationCount();
        const KisPageVersion implicit{key(3), {1}, surface.defaultPixelRevision};
        QVERIFY(!publication->ensureVirtualDefaultLocked(implicit, surface, &f.error));
        QCOMPARE(f.metadata.pageRegistrationCount(), registrations);
        QVERIFY(publication->m_descriptors.empty());
        refused = false;
        try { facade.emplace(f.budget); }
        catch (const std::bad_alloc &) { refused = true; }
        QVERIFY(refused);
        QVERIFY(!facade);
        publication->installDescriptorAdditionsLocked(&second);
        publication->installDescriptorAdditionsLocked(&first);
        QCOMPARE(publication->m_descriptors.size(), size_t(2));
        QVERIFY(first.empty() && second.empty());
        QCOMPARE(f.live(), f.limits.metadataArenaBytes);
        auto unchanged = publication->prepareDescriptorLocked({key(1), {2}}, descriptor);
        QVERIFY(unchanged.empty()); // Existing identical policy requires no node.
        f.operational = false;
        QVERIFY(!publication->prepareDefaultRevisionsLocked({surface}, &f.error));
        QVERIFY(publication->m_defaultRevisionHighWater.empty());
        kisFreeMutationStorage(&f.budget, std::exchange(filler, nullptr), fillerBytes, 1);
        QCOMPARE(f.live(), beforeFull);
        auto duplicate = surface;
        QVERIFY(!publication->prepareDefaultRevisionsLocked({surface, duplicate}, &f.error));
        QVERIFY(publication->m_defaultRevisionHighWater.empty());
        duplicate.surface = {2};
        QVERIFY(publication->prepareDefaultRevisionsLocked({surface, duplicate}, &f.error));
        QCOMPARE(publication->m_defaultRevisionHighWater.size(), size_t(2));
        QVERIFY(publication->ensureVirtualDefaultLocked(implicit, surface, &f.error));
        QCOMPARE(f.metadata.pageRegistrationCount(), registrations + 1);
        QCOMPARE(*publication->m_descriptors.at(implicit), descriptor);
        QCOMPARE(publication->m_descriptors.at(implicit).get(), publication->m_descriptors.at({key(1), {2}}).get());
        facade.emplace(f.budget);
        tail.emplace(publication->prepareDescriptorLocked({key(4), {2}}, descriptor));
        lock.unlock();
    } // Empty candidate maps also release their retained allocator references.
    publication.reset(); fixture.reset();
    const auto parentLive = [&] {
        return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    QVERIFY(parentLive() > parentBaseline);
    const auto withFacade = parentLive();
    facade.reset();
    QVERIFY(parentLive() < withFacade);
    QVERIFY(parentLive() > parentBaseline); // Inert descriptor node still owns actual capacity.
    tail.reset();
    QCOMPARE(parentLive(), parentBaseline);
}

void KisPageStoreCpuMutationTest::overlayStorageAtCapacity()
{
    QFETCH(int, refusal);
    KisPageBackingLimits processLimits;
    processLimits.metadataArenaBytes = 512 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(processLimits);
    // Keep the parent's reusable reservation slots in their original owner;
    // only this child's actual storage must vanish after its final tail.
    std::array<KisBackingBudgetReservation, 16> warm;
    KisBackingBudgetDelta warmDelta;
    warmDelta.buckets[size_t(KisBackingBudgetClass::MetadataArena)].cpuRam = 1;
    for (auto &reservation : warm) {
        reservation = parent->reserve(warmDelta, nullptr);
        QVERIFY(reservation.isValid());
    }
    for (auto &reservation : warm) reservation.release();
    const auto parentBaseline = parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    auto fixture = std::make_unique<ReadTerminalFixture>(256 * 1024, 4 * 64 * 64 * 4);
    auto &f = *fixture;
    QVERIFY(f.budget.configureSharedNonPayloadBudget(parent));
    f.background = false; // Explicit execution fixes the hard-capacity cut.
    f.metadata.attachBackingBudget(f.budget);
    f.metadata.attachRetirementDebtOwner(&f,
        +[](void *p, const KisPageTransitionEffect *effects, qsizetype count, quint64 *cookie, QString *error) {
            return static_cast<ReadTerminalFixture *>(p)->owner.prepareRetirementDebt(effects, count, cookie, error);
        }, +[](void *p, quint64 cookie, const KisPageTransitionEffect *effects, qsizetype count) noexcept {
            auto &fixture = *static_cast<ReadTerminalFixture *>(p);
            fixture.owner.commitRetirementDebt(cookie);
            for (qsizetype i = 0; i < count; ++i) fixture.retirement.acceptEffect(effects[i]);
        }, +[](void *p, quint64 cookie) noexcept { static_cast<ReadTerminalFixture *>(p)->owner.cancelRetirementDebt(cookie); });
    QVERIFY2(f.init(), qPrintable(f.error));
    auto publication = std::make_unique<KisPagePublicationCoordinator>(f.epochs, f.metadata, f.owner,
        f.budget, f.retirement, f.history, f.ready, f.activeCalls, f.operational, f.background, &f,
        +[](void *, KisPageTransactionId) { return false; },
        +[](void *, const KisPageKey &) { return false; },
        +[](void *, KisPageTransactionId, QMutexLocker<QMutex> &, QString *) { return true; },
        +[](void *, KisPageTransactionId) {},
        +[](void *, KisPageMetadataCoordinator::DeferredPublicationCleanup) {},
        +[](void *) { return true; },
        +[](void *, KisPageTransactionId, KisPageReadCleanup &) { return true; });
    f.operational = false;
    QVERIFY(publication->configureDerivedExtentLocked({1}));
    f.operational = true;
    const auto tx = f.epochs.beginTransaction({1}, &f.error);
    QVERIFY(tx.isValid());
    KisSurfaceEpochState surface;
    QVERIFY(f.epochs.root({1}).surfaceState({1}, &surface));
    std::array<KisPreparedPageProof, 2> proofs;
    KisPageWriteCoordinator write(f.metadata, f.epochs, f.budget, f.owner);
    for (size_t i = 0; i < proofs.size(); ++i) {
        const KisPageVersion version{key(int(i + 1)), {2}};
        auto backing = write.reserveBacking(surface.allocationDescriptor(), cpu.domain,
            KisBackingBudgetClass::ActivePending, &f.error, version);
        QVERIFY2(backing.reservation.isValid(), qPrintable(f.error));
        const auto result = f.physical.provider->prepareWrite(f.owner.nextOperationId(), version,
            surface.allocationDescriptor(), cpu.domain, KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
        QVERIFY(result.isValid());
        QVERIFY(f.owner.registerBacking(result.replica, backing.reservation,
            KisBackingBudgetClass::ActivePending, &f.error, &backing.retirement));
        KisPageVersionStateSnapshot implicit;
        implicit.version = {version.key, {1}, 1}; implicit.publication = KisPagePublicationState::Published;
        KisPageVersionStateSnapshot prepared;
        prepared.version = version; prepared.publication = KisPagePublicationState::Prepared;
        prepared.preparedBy = tx.id; prepared.authority = result.replica;
        KisReplicaStateSnapshot resident;
        resident.replica = result.replica; resident.validity = KisReplicaValidity::Valid;
        prepared.replicas = {resident};
        KisPageStateSnapshot page;
        page.key = version.key; page.publishedEpoch = {1}; page.publishedGeneration = {1};
        page.publishedDefaultPixelRevision = 1; page.nextGeneration = {3}; page.versions = {implicit, prepared};
        QVERIFY2(f.metadata.registerPage(page, &f.error), qPrintable(f.error));
        QVERIFY2(f.owner.sealPreparedPage(f.metadata, version, tx.id, surface.allocationDescriptor(),
            result.completion, &proofs[i], &f.error), qPrintable(f.error));
    }
    using Overlay = KisPagePublicationCoordinator::KisPreparedOverlayUpdate;
    using Change = KisPagePublicationCoordinator::OverlayChange;
    QMutexLocker lock(&f.mutex);
    const Change first{key(1), proofs[0], false}, second{key(2), proofs[1], false};
    // Two original candidates coexist: bucket preparation includes both
    // outstanding insertions, while the visible authority is still empty.
    auto a = publication->prepareOverlayUpdateLocked(tx, &first, 1, &f.error);
    auto b = publication->prepareOverlayUpdateLocked(tx, &second, 1, &f.error);
    QVERIFY2(a.isValid() && b.isValid(), qPrintable(f.error));
    auto state = publication->m_preparedTransactions.at(tx.id.value);
    QCOMPARE(state->proofInsertions.load(), size_t(2));
    QVERIFY(state->proofs.empty());
    const Change baseRemoval{key(), {}, true};
    auto cancelled = publication->prepareOverlayUpdateLocked(tx, &baseRemoval, 1, &f.error);
    QVERIFY(cancelled.isValid());
    QCOMPARE(state->removalInsertions.load(), size_t(1));
    QVERIFY(state->removals.empty());
    const auto beforeCancellation = f.live();
    lock.unlock(); cancelled = {}; lock.relock();
    QCOMPARE(state->removalInsertions.load(), size_t(0));
    QVERIFY(state->removals.empty());
    QVERIFY(f.live() < beforeCancellation);
    QVERIFY(a.prepare(&f.error)); QVERIFY(a.prepareSurfaceLocked(&f.error));
    void *filler = nullptr;
    size_t fillerBytes = 0;
    const auto fill = [&] {
        fillerBytes = size_t(f.limits.metadataArenaBytes - f.live());
        filler = kisAllocateMutationStorage(&f.budget, fillerBytes, 1);
        QCOMPARE(f.live(), f.limits.metadataArenaBytes);
    };
    const auto freeFiller = [&] {
        kisFreeMutationStorage(&f.budget, std::exchange(filler, nullptr), fillerBytes, 1);
    };
    const auto cleanupFiller = qScopeGuard([&] { if (filler) freeFiller(); });
    KisPageMetadataCoordinator::DeferredPublicationCleanup cleanup;
    fill();
    QVERIFY2(a.tryInstallLocked(&cleanup, &f.error), qPrintable(f.error));
    a.collectRetirementsLocked();
    QVERIFY(b.prepare(&f.error)); QVERIFY(b.prepareSurfaceLocked(&f.error));
    QVERIFY2(b.tryInstallLocked(&cleanup, &f.error), qPrintable(f.error));
    b.collectRetirementsLocked();
    QCOMPARE(state->proofs.size(), size_t(2));
    QCOMPARE(state->proofInsertions.load(), size_t(0));
    freeFiller();
    lock.unlock(); a = {}; b = {}; lock.relock();

    const std::array<Change, 3> removals{{{key(2), {}, true}, {key(1), {}, true}, {key(), {}, true}}};
    Overlay removal;
    if (refusal == 0) {
        const auto before = f.live();
        fill();
        removal = publication->prepareOverlayUpdateLocked(tx, removals.data(), removals.size(), &f.error);
        QVERIFY(!removal.isValid());
        QCOMPARE(f.live(), f.limits.metadataArenaBytes);
        QCOMPARE(state->proofs.size(), size_t(2));
        for (const auto &proof : proofs) QVERIFY(f.owner.ownsPreparedPageProof(proof));
        freeFiller(); QCOMPARE(f.live(), before);
    }
    removal = publication->prepareOverlayUpdateLocked(tx, removals.data(), removals.size(), &f.error);
    QVERIFY2(removal.isValid(), qPrintable(f.error));
    lock.unlock(); const bool prepared = removal.prepare(&f.error); lock.relock();
    QVERIFY2(prepared, qPrintable(f.error));
    if (refusal == 1) {
        fill();
        QVERIFY(!removal.prepareSurfaceLocked(&f.error));
        QCOMPARE(state->proofs.size(), size_t(2));
        for (const auto &proof : proofs) {
            QVERIFY(f.owner.ownsPreparedPageProof(proof));
            KisPageMetadataCoordinator::VersionInfo selected;
            QVERIFY(f.metadata.versionSnapshot(proof.authority.version, &selected));
            QCOMPARE(selected.publication, KisPagePublicationState::Prepared);
            QCOMPARE(selected.preparedBy, tx.id);
        }
        freeFiller(); // The same candidate may retry before its acceptance.
    }
    QVERIFY2(removal.prepareSurfaceLocked(&f.error), qPrintable(f.error));
    fill();
    QVERIFY2(removal.tryInstallLocked(&cleanup, &f.error), qPrintable(f.error));
    const auto beforeCollection = f.budget.usage().backpressureCount;
    removal.collectRetirementsLocked(); // Original History retains refused keys at hard capacity.
    QVERIFY(f.budget.usage().backpressureCount > beforeCollection);
    QCOMPARE(f.history.snapshotLocked().pendingPages, qsizetype(0)); // No key node could be allocated.
    QVERIFY(state->proofs.empty());
    QCOMPARE(state->removals.size(), size_t(1));
    QVERIFY(state->removals.count(key()));
    QCOMPARE(state->surfaceChanges.front().after.contentExtent, QRect());
    for (const auto &proof : proofs) QVERIFY(!f.owner.ownsPreparedPageProof(proof));
    for (const auto &proof : proofs) {
        KisPageMetadataCoordinator::VersionInfo historical;
        QVERIFY(f.metadata.versionSnapshot(proof.authority.version, &historical));
        QCOMPARE(historical.publication, KisPagePublicationState::Historical);
    }
    QCOMPARE(f.physical.provider->retireCalls.load(), 0);
    freeFiller();
    lock.unlock(); cleanup = {}; lock.relock();
    f.history.collectUnreachableLocked(nullptr, 0); // Resume the original scan without replaying installation.
    lock.unlock();
    f.retirement.process(std::numeric_limits<qsizetype>::max());
    f.retirement.waitForIdle(); // Registered provider permission retains its original worker dispatch.
    lock.relock();
    QCOMPARE(f.physical.provider->retireCalls.load(), 2);
    QVERIFY(f.retirement.isDrained());
    QCOMPARE(f.owner.providerOperationCount(), qsizetype(0));
    // Installed candidate/state tails contain only storage; destroying the
    // publication facade first cannot release their actual capacity early.
    const auto withFacade = f.live();
    publication.reset();
    QVERIFY(f.live() < withFacade); // Only the original transaction index node.
    QVERIFY(f.epochs.abort(tx));
    lock.unlock(); state.reset(); fixture.reset();
    const auto parentLive = [&] {
        return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    QVERIFY(parentLive() > parentBaseline); // Controller is gone; the candidate still owns actual storage.
    removal = {};
    QCOMPARE(parentLive(), parentBaseline);
    QCOMPARE(parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].reserved.cpuRam, quint64(0));
}

void KisPageStoreCpuMutationTest::historyRefusalPreservesOriginalWork()
{
    QFETCH(int, refusal);
    ReadTerminalFixture f(256 * 1024);
    // Limits were fixed before constructing the original controller.
    f.metadata.attachBackingBudget(f.budget);
    f.metadata.attachRetirementDebtOwner(&f,
        +[](void *p, const KisPageTransitionEffect *effects, qsizetype count, quint64 *cookie, QString *error) {
            return static_cast<ReadTerminalFixture *>(p)->owner.prepareRetirementDebt(effects, count, cookie, error);
        }, +[](void *p, quint64 cookie, const KisPageTransitionEffect *effects, qsizetype count) noexcept {
            auto &fixture = *static_cast<ReadTerminalFixture *>(p);
            fixture.owner.commitRetirementDebt(cookie);
            if (fixture.afterDebtCommit) fixture.afterDebtCommit();
            for (qsizetype i = 0; i < count; ++i) fixture.retirement.acceptEffect(effects[i]);
        }, +[](void *p, quint64 cookie) noexcept { static_cast<ReadTerminalFixture *>(p)->owner.cancelRetirementDebt(cookie); });
    QVERIFY2(f.init(nullptr, true), qPrintable(f.error));
    const auto stats = [&] { QMutexLocker lock(&f.mutex); return f.history.snapshotLocked(); };
    const quint64 before = f.live();
    void *filler = nullptr;
    size_t fillerBytes = 0;
    KisBackingBudgetReservation blockingDebt;
    std::atomic<int> capabilityCalls{0};
    if (refusal == 0 || refusal == 3 || refusal == 4) {
        QMutexLocker lock(&f.mutex);
        if (refusal != 0) {
            const auto k = key();
            f.history.collectUnreachableLocked(&k, 1); // Prepare Work before filling capacity.
        }
        fillerBytes = size_t(f.limits.metadataArenaBytes - f.live())
            - (refusal == 4 ? sizeof(KisPageTransitionEffect) : 0);
        filler = kisAllocateMutationStorage(&f.budget, fillerBytes, 1);
    } else if (refusal == 1) {
        KisBackingBudgetDelta debt;
        debt.buckets[size_t(KisBackingBudgetClass::RetirementDebt)].cpuRam =
            qint64(f.limits.retirementDebtBytes);
        blockingDebt = f.budget.reserve(debt, &f.error);
        QVERIFY2(blockingDebt.isValid(), qPrintable(f.error));
    } else {
        f.physical.provider->beforeCapabilities = [&] { ++capabilityCalls; };
        f.afterDebtCommit = [&] {
            if (!filler) {
                fillerBytes = size_t(f.limits.metadataArenaBytes - f.live());
                filler = kisAllocateMutationStorage(&f.budget, fillerBytes, 1);
            }
            QCOMPARE(f.live(), f.limits.metadataArenaBytes);
        };
    }
    const auto release = qScopeGuard([&] {
        f.history.stopAutomaticWakeups();
        { QMutexLocker lock(&f.mutex); f.history.waitForIdleLocked();
          f.physical.provider->beforeCapabilities = {}; f.afterDebtCommit = {}; }
        blockingDebt.release();
        kisFreeMutationStorage(&f.budget, filler, fillerBytes, 1);
    });
    { QMutexLocker lock(&f.mutex); const auto k = key(); f.history.collectUnreachableLocked(&k, 1); }
    if (refusal != 2) QTRY_VERIFY_WITH_TIMEOUT(stats().retryWakeups >= 3, 5000);
    else QTRY_COMPARE_WITH_TIMEOUT(f.physical.provider->retireCalls.load(), 1, 5000);
    KisPageStateSnapshot page;
    QVERIFY(f.metadata.pageSnapshot(key(), &page));
    QCOMPARE(bool(page.findVersion(f.replica.version)), refusal != 2);
    if (refusal == 2) {
        QCOMPARE(capabilityCalls.load(), 0);
    } else QCOMPARE(f.physical.provider->retireCalls.load(), 0);
    blockingDebt.release();
    { QMutexLocker lock(&f.mutex); f.physical.provider->beforeCapabilities = {}; f.afterDebtCommit = {}; }
    kisFreeMutationStorage(&f.budget, std::exchange(filler, nullptr), fillerBytes, 1);
    // No new request, collect call, completion or explicit wake after capacity release.
    QTRY_COMPARE_WITH_TIMEOUT(stats().pendingPages, qsizetype(0), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(f.physical.provider->retireCalls.load(), 1, 5000);
    f.retirement.waitForIdle();
    { QMutexLocker lock(&f.mutex); f.history.waitForIdleLocked(); }
    QVERIFY(f.retirement.isDrained());
    QVERIFY(f.metadata.pageSnapshot(key(), &page));
    QVERIFY(!page.findVersion(f.replica.version));
    QCOMPARE(stats().activeScans, qsizetype(0));
    QVERIFY(!stats().retryScheduled);
    QCOMPARE(f.owner.providerOperationCount(), qsizetype(0));
    QVERIFY(f.live() < before); // The real key/retirement/operation storage was freed.
    QCOMPARE(f.references.loadAcquire(), 1);
}

void KisPageStoreCpuMutationTest::historyDetachRequiresRetirementOwner()
{
    QFETCH(int, missing);
    ReadTerminalFixture f(256 * 1024);
    f.metadata.attachBackingBudget(f.budget);
    if (missing) f.metadata.attachRetirementDebtOwner(&f,
        +[](void *p, const KisPageTransitionEffect *effects, qsizetype count, quint64 *cookie, QString *error) {
            return static_cast<ReadTerminalFixture *>(p)->owner.prepareRetirementDebt(effects, count, cookie, error);
        }, +[](void *p, quint64 cookie, const KisPageTransitionEffect *effects, qsizetype count) noexcept {
            auto &f = *static_cast<ReadTerminalFixture *>(p); f.owner.commitRetirementDebt(cookie);
            for (qsizetype i = 0; i < count; ++i) f.retirement.acceptEffect(effects[i]);
        }, +[](void *p, quint64 cookie) noexcept { static_cast<ReadTerminalFixture *>(p)->owner.cancelRetirementDebt(cookie); });
    QVERIFY2(f.init(nullptr, true), qPrintable(f.error));
    KisPageRetirementRecordPointer retained;
    if (missing == 1) f.owner.releaseRetiredBacking(f.replica);
    else if (missing == 2) {
        retained = f.owner.takeRetirementRecord(f.replica);
        QVERIFY(retained);
    }
    const auto stats = [&] { QMutexLocker lock(&f.mutex); return f.history.snapshotLocked(); };
    { QMutexLocker lock(&f.mutex); const auto k = key(); f.history.collectUnreachableLocked(&k, 1); }
    QTRY_VERIFY_WITH_TIMEOUT(stats().retryWakeups >= 3, 5000);
    f.history.stopAutomaticWakeups();
    { QMutexLocker lock(&f.mutex); f.history.waitForIdleLocked(); }
    KisPageStateSnapshot page;
    QVERIFY(f.metadata.pageSnapshot(key(), &page));
    QVERIFY(page.findVersion(f.replica.version));
    QCOMPARE(f.physical.provider->retireCalls.load(), 0);
    QCOMPARE(f.owner.providerOperationCount(), qsizetype(0));
    QCOMPARE(f.retirement.snapshot().pendingReplicas, qsizetype(0));
    QCOMPARE(stats().pendingPages, qsizetype(1));
}

void KisPageStoreCpuMutationTest::genericEffectsTransferAtCapacity()
{
    QFETCH(bool, background);
    QFETCH(bool, receiver);
    ReadTerminalFixture f(256 * 1024);
    f.physical.provider->disableBackgroundRetirement = !background;
    f.physical.provider->rejectRetire = true;
    f.metadata.attachBackingBudget(f.budget);
    if (receiver) f.metadata.attachRetirementDebtOwner(&f,
        +[](void *p, const KisPageTransitionEffect *effects, qsizetype count, quint64 *cookie, QString *error) {
            return static_cast<ReadTerminalFixture *>(p)->owner.prepareRetirementDebt(effects, count, cookie, error);
        }, +[](void *p, quint64 cookie, const KisPageTransitionEffect *effects, qsizetype count) noexcept {
            auto &f = *static_cast<ReadTerminalFixture *>(p);
            f.owner.commitRetirementDebt(cookie);
            f.afterDebtCommit();
            for (qsizetype i = 0; i < count; ++i) f.retirement.acceptEffect(effects[i]);
        }, +[](void *p, quint64 cookie) noexcept { static_cast<ReadTerminalFixture *>(p)->owner.cancelRetirementDebt(cookie); });
    QVERIFY2(f.init(nullptr, true), qPrintable(f.error));
    QVERIFY(f.originalRetirementRecord);
    void *filler = nullptr;
    size_t fillerBytes = 0;
    bool filledAtCommit = false;
    f.afterDebtCommit = [&] {
        fillerBytes = size_t(f.limits.metadataArenaBytes - f.live());
        filler = kisAllocateMutationStorage(&f.budget, fillerBytes, 1);
        filledAtCommit = f.live() == f.limits.metadataArenaBytes;
    };
    const auto cleanup = qScopeGuard([&] {
        f.retirement.beginClose(); f.retirement.waitForIdle();
        f.afterDebtCommit = {};
        kisFreeMutationStorage(&f.budget, filler, fillerBytes, 1);
    });
    KisPageTransition discard;
    discard.kind = KisPageTransitionKind::DiscardHistoricalVersions;
    discard.versions = {f.replica.version};
    KisPageMetadataTransitionResult result;
    { QMutexLocker lock(&f.mutex); result = f.metadata.applyOwner(key(), discard); }
    QCOMPARE(result.accepted, receiver);
    KisPageStateSnapshot page; QVERIFY(f.metadata.pageSnapshot(key(), &page));
    QCOMPARE(bool(page.findVersion(f.replica.version)), !receiver);
    if (!receiver) {
        QVERIFY(!result.rejectionReason.isEmpty());
        QCOMPARE(f.owner.backingClass(f.replica), KisBackingBudgetClass::RetainedHistory);
        QCOMPARE(f.retirement.snapshot().pendingReplicas, qsizetype(0));
        QCOMPARE(f.physical.provider->retireCalls.load(), 0);
        QVERIFY(!filledAtCommit);
        return;
    }
    QVERIFY(filledAtCommit); // Capacity cannot refuse the committed record transfer.
    QCOMPARE(f.owner.backingClass(f.replica), KisBackingBudgetClass::RetirementDebt);
    QVERIFY(!f.owner.takeRetirementRecord(f.replica)); // Queue has the sole original record.
    if (background) {
        QTRY_VERIFY_WITH_TIMEOUT(f.physical.provider->retireCalls.load() > 0, 5000);
    } else {
        QCOMPARE(f.physical.provider->retireCalls.load(), 0);
        QCOMPARE(f.retirement.process(0).attempted, qsizetype(0));
        QCOMPARE(f.physical.provider->retireCalls.load(), 0);
        QCOMPARE(f.retirement.process(1).attempted, qsizetype(1));
        QCOMPARE(f.physical.provider->retireCalls.load(), 1);
        QVERIFY(!f.retirement.snapshot().jobScheduled);
    }
    f.retirement.beginClose(); f.retirement.waitForIdle();
    auto records = f.retirement.takeForClose();
    QCOMPARE(records.size(), size_t(1));
    QCOMPARE(&records.front(), f.originalRetirementRecord);
    QCOMPARE(records.front().backgroundRetirement, background);
    QCOMPARE(records.front().replica, f.replica);
    QVERIFY(!records.front().lastUse.isValid());
    f.physical.provider->rejectRetire = false;
    const auto attempts = f.physical.provider->retireCalls.load();
    QVERIFY(f.retirement.retireRecord(records.front())); // Original prepared operation still works at capacity.
    QCOMPARE(f.physical.provider->retireCalls.load(), attempts + 1);
    records.clear_and_dispose(KisPageRetirementRecordDeleter{});
    QVERIFY(f.retirement.isDrained());
    QCOMPARE(f.owner.backingClass(f.replica), KisBackingBudgetClass::Count);
    QCOMPARE(f.owner.providerOperationCount(), qsizetype(0));
    QCOMPARE(f.physical.provider->memoryUsage().committedBytes, quint64(0));
    QCOMPARE(f.budget.usage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].live.cpuRam, quint64(0));
    QCOMPARE(f.references.loadAcquire(), 1);
}

void KisPageStoreCpuMutationTest::synchronousProviderResultsAtCapacity()
{
    Fixture f; QVERIFY(f.init());
    KisSurfaceEpochState surface; QVERIFY(f.store->resolveSurfaceState({1}, {}, &surface));
    auto descriptor = surface.allocationDescriptor();
    descriptor.initialization = KisPageInitialization::DefaultPixel;
    KisPageBackingLimits limits; limits.metadataArenaBytes = 64 * 1024;
    KisBackingBudgetController budget(limits);
    KisPageOwnerLedger owner; QVERIFY(owner.configure(f.completions));
    owner.attachBackingBudget(budget); QVERIFY(owner.registerProvider(f.provider));
    const auto allocation = f.provider->requestReplica(owner.nextOperationId(), {key(), {1}},
        descriptor, cpu.domain, KisPageAccessMode::Read, KisPagePriority::Normal);
    const auto write = f.provider->prepareWrite(owner.nextOperationId(), {key(), {2}},
        descriptor, cpu.domain, KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
    QVERIFY(allocation.isValid() && write.isValid());
    KisReplicaTransferRequest request{owner.nextOperationId(), allocation.replica, write.replica,
        descriptor, KisReplicaTransferKind::WriteGenerationInitialization};
    int transfers = 0; f.provider->beforeTransfer = [&] { ++transfers; };
    const auto transfer = f.provider->transfer(request, KisPagePriority::Normal);
    QVERIFY(transfer.isValid());

    // Establish the real accounting owner and reusable reservation capacity
    // before filling the arena; their cold storage is part of the live total.
    auto accounting = KisMutationStorageAllocator<char>::retained(&budget);
    auto first = budget.reserve({}, nullptr), second = budget.reserve({}, nullptr);
    QVERIFY(first.isValid() && second.isValid()); first.release(); second.release();
    const auto live = [&] { return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - live());
    void *filler = kisAllocateMutationStorage(&budget, fillerBytes, 1);
    const auto release = qScopeGuard([&] { kisFreeMutationStorage(&budget, filler, fillerBytes, 1); });
    QCOMPARE(live(), limits.metadataArenaBytes);
    // This is the exact charged insertion used by the old synchronous path.
    // Refusal after physical success must not require replay or retain a node.
    QVERIFY(!owner.bindProviderOperation(allocation.operation, allocation, &f.error));
    QVERIFY(f.error.contains(QStringLiteral("storage was refused")));
    for (const auto &result : {allocation, write, transfer}) {
        QVERIFY2(owner.verifyTerminalProviderResult(result.operation, result, &f.error).succeeded(), qPrintable(f.error));
        QCOMPARE(owner.providerOperationCount(), qsizetype(0));
        QCOMPARE(owner.publicationBlockingOperationCount(), qsizetype(0));
        QCOMPARE(live(), limits.metadataArenaBytes);
    }
    QCOMPARE(transfers, 1);

    KisPageMetadataCoordinator metadata; QVERIFY(metadata.configure(1));
    QAtomicInt references{1};
    KisPageRetirementQueue queue(owner, metadata, budget, references, &references,
        [](void *p) { static_cast<QAtomicInt *>(p)->deref(); });
    // No original record exists for these foreign handles. The synchronous
    // fallback must also finish at capacity without an unowned operation.
    queue.retireOrDefer(allocation.replica, f.provider, {}, {});
    queue.retireOrDefer(write.replica, f.provider, {}, {});
    QCOMPARE(f.provider->retireCalls.load(), 2);
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
    QCOMPARE(owner.providerOperationCount(), qsizetype(0));
    QCOMPARE(references.loadAcquire(), 1);
    QCOMPARE(live(), limits.metadataArenaBytes);
}

void KisPageStoreCpuMutationTest::providerCompletionPreparationRejectsBeforePayload()
{
    QFETCH(bool, prepareFirst);
    KisPageBackingLimits limits; limits.metadataArenaBytes = 64 * 1024;
    auto process = std::make_shared<KisBackingBudgetController>(limits);
    Fixture f; f.completions = std::make_shared<KisCompletionRegistry>(process);
    QVERIFY(f.init());
    KisSurfaceEpochState surface; QVERIFY(f.store->resolveSurfaceState({1}, {}, &surface));
    KisPageOwnerLedger identities; QVERIFY(identities.configure(f.completions));
    const auto prepare = [&] {
        return f.provider->prepareWrite(identities.nextOperationId(), {key(), {1}},
            surface.allocationDescriptor(), cpu.domain, KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
    };
    const auto warmAllocation = prepare(); QVERIFY(warmAllocation.isValid());
    const auto source = warmAllocation.completion.source();
    QVERIFY(f.provider->retire(identities.nextOperationId(), warmAllocation.replica, {}).isValid());
    auto first = process->reserve({}, nullptr), second = process->reserve({}, nullptr);
    QVERIFY(first.isValid() && second.isValid()); first.release(); second.release();
    const auto live = [&] { return process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const auto before = live();
    const auto probe = f.completions->allocatePending(source); QVERIFY(probe.isValid());
    const auto nodeBytes = live() - before; QVERIFY(nodeBytes > 0);
    QVERIFY(f.completions->complete(probe, KisCompletionStatus::Succeeded));
    QCOMPARE(live(), before);
    const auto admitted = f.completions->sourceStatistics(source);
    const auto work = f.provider->p->payloadWork();
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - live() - (prepareFirst ? nodeBytes : 0));
    void *filler = kisAllocateMutationStorage(process.get(), fillerBytes, 1);
    const auto release = qScopeGuard([&] { kisFreeMutationStorage(process.get(), filler, fillerBytes, 1); });
    const auto rejected = prepare();
    QVERIFY(!rejected.isValid());
    QCOMPARE(f.completions->sourceStatistics(source).allocatedTickets, admitted.allocatedTickets + quint64(prepareFirst));
    QCOMPARE(f.completions->sourceStatistics(source).pendingTickets, quint64(0));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
    QCOMPARE(f.provider->p->payloadWork().defaultInitializedPages, work.defaultInitializedPages);
    kisFreeMutationStorage(process.get(), std::exchange(filler, nullptr), fillerBytes, 1);
    const auto retry = prepare(); QVERIFY2(retry.isValid(), qPrintable(retry.error));
    QVERIFY(f.provider->retire(identities.nextOperationId(), retry.replica, {}).isValid());
    QCOMPARE(f.completions->sourceStatistics(source).pendingTickets, quint64(0));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::providerRetirementUsesPreparedCompletion()
{
    KisPageBackingLimits limits; limits.metadataArenaBytes = 64 * 1024;
    auto process = std::make_shared<KisBackingBudgetController>(limits);
    Fixture f; f.completions = std::make_shared<KisCompletionRegistry>(process);
    QVERIFY(f.init());
    KisSurfaceEpochState surface; QVERIFY(f.store->resolveSurfaceState({1}, {}, &surface));
    KisPageOwnerLedger identities; QVERIFY(identities.configure(f.completions));
    const auto allocation = f.provider->prepareWrite(identities.nextOperationId(), {key(), {1}},
        surface.allocationDescriptor(), cpu.domain, KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
    QVERIFY(allocation.isValid());
    const auto source = allocation.completion.source();
    const auto later = f.provider->prepareWrite(identities.nextOperationId(), {key(1), {1}},
        surface.allocationDescriptor(), cpu.domain, KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
    QVERIFY(later.isValid());
    auto first = process->reserve({}, nullptr), second = process->reserve({}, nullptr);
    QVERIFY(first.isValid() && second.isValid()); first.release(); second.release();
    const auto live = [&] { return process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const auto admitted = f.completions->sourceStatistics(source);
    QCOMPARE(admitted.pendingTickets, quint64(2)); // Both original retirement tickets.
    auto access = f.provider->resolveAccess(identities.nextLeaseId(), identities.nextOperationId(),
        later.replica, cpu, KisPageAccessMode::Read);
    QVERIFY(access.isValid());
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - live());
    void *filler = kisAllocateMutationStorage(process.get(), fillerBytes, 1);
    const auto release = qScopeGuard([&] { kisFreeMutationStorage(process.get(), filler, fillerBytes, 1); });
    QCOMPARE(live(), limits.metadataArenaBytes);
    const auto busy = f.provider->retire(identities.nextOperationId(), later.replica, {});
    QVERIFY(!busy.isValid());
    QCOMPARE(f.completions->sourceStatistics(source).allocatedTickets, admitted.allocatedTickets);
    QCOMPARE(f.completions->sourceStatistics(source).pendingTickets, quint64(2));
    f.provider->releaseAccess(std::move(access), {});
    // A refusal preserved the original private ticket. Out-of-order physical
    // retirement succeeds while the shared arena has no remaining capacity.
    const auto retired = f.provider->retire(identities.nextOperationId(), later.replica, {});
    QVERIFY2(retired.isValid(), qPrintable(retired.error));
    QVERIFY(f.completions->verifyTerminal(retired.completion).succeeded());
    QCOMPARE(f.completions->sourceStatistics(source).allocatedTickets, admitted.allocatedTickets);
    QCOMPARE(f.completions->sourceStatistics(source).pendingTickets, quint64(1));
    QCOMPARE(f.provider->memoryUsage().committedBytes, allocation.replica.layout.byteSize);
    QVERIFY(live() < limits.metadataArenaBytes);
    QVERIFY(f.provider->retire(identities.nextOperationId(), allocation.replica, {}).isValid());
    QCOMPARE(f.provider->retireCalls.load(), 3); // One refused call, two physical frees.
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
    QCOMPARE(f.completions->sourceStatistics(source).pendingTickets, quint64(0));
    kisFreeMutationStorage(process.get(), std::exchange(filler, nullptr), fillerBytes, 1);
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::providerAdoptionPreparationRejectsBeforePayload()
{
    QFETCH(int, node);
    KisPageBackingLimits limits; limits.metadataArenaBytes = 64 * 1024;
    auto process = std::make_shared<KisBackingBudgetController>(limits);
    Fixture f; f.providerProcessBudget = process; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    auto view = f.store->captureReadView(); auto read = view.readResidentPage(key()); QVERIFY(read.isValid());
    KisSurfaceEpochState surface; QVERIFY(view.resolveSurfaceState({1}, &surface));
    const auto descriptor = surface.allocationDescriptor();
    auto *tile = f.provider->p->tileDataForCpuReadGuard(read); QVERIFY(tile && tile->ref());
    const auto releaseTile = qScopeGuard([&] { tile->deref(); });
    const auto source = f.provider->p->captureCpuReadSource(read, descriptor); QVERIFY(source);
    read = {}; view = {};
    KisPageOwnerLedger identities; QVERIFY(identities.configure(f.completions));
    const auto fresh = [&] { return f.provider->prepareWrite(identities.nextOperationId(), {key(2), {1}},
        descriptor, cpu.domain, KisPageWriteMode::DiscardContents, KisPagePriority::Normal); };
    const auto alias = [&] { return f.provider->p->prepareSynchronousSource(identities.nextOperationId(), source,
        {key(3), {1}}, descriptor, KisReplicaSourceUse::ImmutableAlias, KisPagePriority::Normal); };
    const auto retire = [&](const KisReplicaOperation &result) {
        return f.provider->retire(identities.nextOperationId(), result.replica, {}).isValid();
    };
    const auto warmedFresh = fresh(); QVERIFY(warmedFresh.isValid()); QVERIFY(retire(warmedFresh));
    const auto warmedAlias = alias(); QVERIFY(warmedAlias.isValid()); QVERIFY(retire(warmedAlias));
    auto first = process->reserve({}, nullptr), second = process->reserve({}, nullptr);
    QVERIFY(first.isValid() && second.isValid()); first.release(); second.release();
    const auto live = [&] { return process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const auto baseline = live();
    const auto measuredAlias = alias(); QVERIFY(measuredAlias.isValid());
    const quint64 aliasBytes = live() - baseline;
    auto aliasBinding = f.provider->p->cpuResidentBinding(measuredAlias.replica); QVERIFY(aliasBinding);
    QVERIFY(retire(measuredAlias));
    const quint64 bindingBytes = live() - baseline; QVERIFY(bindingBytes > sizeof(KisCpuResidentBinding));
    const quint64 allocationBytes = aliasBytes - bindingBytes; QVERIFY(allocationBytes > 0);
    aliasBinding.reset(); QCOMPARE(live(), baseline);
    const auto measuredFresh = fresh(); QVERIFY(measuredFresh.isValid());
    const quint64 freshBytes = live() - baseline; QVERIFY(freshBytes > allocationBytes);
    QVERIFY(retire(measuredFresh)); QCOMPARE(live(), baseline);
    const quint64 room = node == 0 ? 0 : node == 1 ? allocationBytes :
        node == 2 ? freshBytes - bindingBytes - 1 : freshBytes - 1;
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - live() - room);
    void *filler = kisAllocateMutationStorage(process.get(), fillerBytes, 1);
    const auto release = qScopeGuard([&] { kisFreeMutationStorage(process.get(), filler, fillerBytes, 1); });
    const auto work = f.provider->p->payloadWork(); const auto usage = f.provider->memoryUsage();
    const auto original = f.completions->sourceStatistics(measuredFresh.completion.source());
    const auto rejected = fresh(); QVERIFY(!rejected.isValid()); QVERIFY(rejected.error.contains(QStringLiteral("storage")));
    QCOMPARE(live(), baseline + fillerBytes); // Every partially prepared actual node returned.
    QCOMPARE(f.provider->p->payloadWork().defaultInitializedPages, work.defaultInitializedPages);
    QCOMPARE(f.provider->memoryUsage().committedBytes, usage.committedBytes);
    QCOMPARE(f.completions->sourceStatistics(measuredFresh.completion.source()).pendingTickets, original.pendingTickets);
    if (!node) {
        QVERIFY(!alias().isValid());
        QVERIFY(!f.provider->p->adoptInitialTile({key(3), {1}}, descriptor, tile, &f.error).isValid());
        QCOMPARE(live(), baseline + fillerBytes);
        QCOMPARE(f.provider->p->payloadWork().adoptedPages, work.adoptedPages);
        QCOMPARE(f.provider->memoryUsage().committedBytes, usage.committedBytes);
    }
    kisFreeMutationStorage(process.get(), std::exchange(filler, nullptr), fillerBytes, 1);
    const auto retry = fresh(); QVERIFY2(retry.isValid(), qPrintable(retry.error)); QVERIFY(retire(retry));
    const auto aliasRetry = alias(); QVERIFY(aliasRetry.isValid());
    const auto initialRetry = f.provider->p->adoptInitialTile({key(3), {1}}, descriptor, tile, &f.error);
    QVERIFY(initialRetry.isValid());
    QCOMPARE(f.provider->p->backingFootprint(initialRetry).physicalSlot,
        f.provider->p->backingFootprint(aliasRetry.replica).physicalSlot);
    QCOMPARE(f.provider->memoryUsage().committedBytes, usage.committedBytes);
    QVERIFY(retire(aliasRetry)); QCOMPARE(f.provider->memoryUsage().committedBytes, usage.committedBytes);
    QVERIFY(f.provider->retire(identities.nextOperationId(), initialRetry, {}).isValid());
    QCOMPARE(live(), baseline); QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::providerLeasePreparationPreservesOriginalPin()
{
    KisPageBackingLimits limits; limits.metadataArenaBytes = 64 * 1024;
    auto process = std::make_shared<KisBackingBudgetController>(limits);
    Fixture f; f.providerProcessBudget = process; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto handle = f.provider->lastTarget;
    KisPageOwnerLedger identities; QVERIFY(identities.configure(f.completions));
    auto access = f.provider->resolveAccess(identities.nextLeaseId(), identities.nextOperationId(), handle, cpu, KisPageAccessMode::Read);
    QVERIFY(access.isValid());
    auto first = process->reserve({}, nullptr), second = process->reserve({}, nullptr);
    QVERIFY(first.isValid() && second.isValid()); first.release(); second.release();
    const auto before = process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - before);
    void *filler = kisAllocateMutationStorage(process.get(), fillerBytes, 1);
    const auto release = qScopeGuard([&] { kisFreeMutationStorage(process.get(), filler, fillerBytes, 1); });
    const auto refusedLease = identities.nextLeaseId();
    QVERIFY(!f.provider->resolveAccess(refusedLease, identities.nextOperationId(), handle, cpu, KisPageAccessMode::Read).isValid());
    QCOMPARE(*static_cast<const quint8 *>(access.cpuReadData), quint8(0x31));
    QVERIFY(!f.provider->retire(identities.nextOperationId(), handle, {}).isValid());
    f.provider->releaseAccess(std::move(access), {});
    QVERIFY(process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam < limits.metadataArenaBytes);
    // Refusal did not occupy its original lease identity; the actual freed node grants it.
    auto retry = f.provider->resolveAccess(refusedLease, identities.nextOperationId(), handle, cpu, KisPageAccessMode::Read);
    QVERIFY(retry.isValid()); f.provider->releaseAccess(std::move(retry), {});
    kisFreeMutationStorage(process.get(), std::exchange(filler, nullptr), fillerBytes, 1);
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::residencyObserverRegistrationRejectsRecreatedIdentity()
{
    struct Observer final : KisTileDataResidencyObserver {
        struct Terminal final : KisTileDataResidencyTransition {
            explicit Terminal(Observer *value) : owner(value) {}
            ~Terminal() override { if (!completed) ++owner->cancelled; }
            void commit(quint64) noexcept override { completed = true; ++owner->committed; }
            Observer *owner;
            bool completed = false;
        };
        std::shared_ptr<KisTileDataResidencyTransition> prepareResidencyChange(
            KisTileData *, const KisTileDataResidencyState &, bool, QString *) override
        {
            auto result = std::make_shared<Terminal>(this);
            if (++prepared == 1) { entered.release(); resume.acquire(); }
            return result;
        }
        std::atomic<int> prepared{0}, committed{0}, cancelled{0};
        QSemaphore entered, resume;
    };
    auto observer = std::make_shared<Observer>();
    auto *tiles = KisTileDataStore::instance();
    const quint8 pixel[4] = {0x17, 0x17, 0x17, 0x17};
    auto *tile = tiles->createDefaultTileData(4, pixel); QVERIFY(tile && tile->ref());
    const auto releaseTile = qScopeGuard([&] { tile->deref(); });
    const auto unregister = qScopeGuard([&] { tiles->unregisterResidencyObserver(tile, observer); });
    KisTileDataResidencyState original, replacement;
    QVERIFY(tiles->registerResidencyObserver(tile, observer, &original));
    bool swapped = false;
    std::thread swap([&] { swapped = tiles->trySwapTileData(tile); });
    const bool entered = observer->entered.tryAcquire(1, 5000);
    tiles->unregisterResidencyObserver(tile, observer);
    const bool registered = tiles->registerResidencyObserver(tile, observer, &replacement);
    observer->resume.release(); swap.join();
    QVERIFY(entered && registered);
    QCOMPARE(replacement.resident, original.resident); QCOMPARE(replacement.revision, original.revision);
    QVERIFY(swapped && !tile->isResident());
    QCOMPARE(observer->prepared.load(), 2);
    QCOMPARE(observer->committed.load(), 1);
    QCOMPARE(observer->cancelled.load(), 1);
    QVERIFY(tile->blockSwapping()); tile->unblockSwapping();
}

void KisPageStoreCpuMutationTest::providerMemoryUsageWaitsForOriginalTransition()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto handle = f.provider->lastTarget;
    auto view = f.store->captureReadView(); auto read = view.readResidentPage(handle.version.key);
    auto *tile = f.provider->p->tileDataForCpuReadGuard(read); QVERIFY(tile && tile->ref());
    const auto releaseTile = qScopeGuard([&] { tile->deref(); }); read = {}; view = {};
    const auto before = f.provider->memoryUsage();
    struct PausedObserver final : KisTileDataResidencyObserver {
        QSemaphore entered, resume;
        struct Terminal final : KisTileDataResidencyTransition { void commit(quint64) noexcept override {} };
        std::shared_ptr<KisTileDataResidencyTransition> prepareResidencyChange(
            KisTileData *, const KisTileDataResidencyState &, bool, QString *) override {
            auto terminal = std::make_shared<Terminal>();
            entered.release(); resume.acquire(); return terminal;
        }
    };
    auto paused = std::make_shared<PausedObserver>(); auto *tiles = KisTileDataStore::instance();
    QVERIFY(tiles->registerResidencyObserver(tile, paused));
    const auto unregister = qScopeGuard([&] { tiles->unregisterResidencyObserver(tile, paused); });
    bool swapped = false; KisReplicaMemoryUsage after; QSemaphore started, finished;
    std::thread swap([&] { swapped = tiles->trySwapTileData(tile); }); std::thread usage;
    const auto join = qScopeGuard([&] { paused->resume.release(); if (swap.joinable()) swap.join(); if (usage.joinable()) usage.join(); });
    QVERIFY(paused->entered.tryAcquire(1, 2000));
    usage = std::thread([&] { started.release(); after = f.provider->memoryUsage(); finished.release(); });
    QVERIFY(started.tryAcquire(1, 2000)); QVERIFY(!finished.tryAcquire(1, 20));
    paused->resume.release(); swap.join(); usage.join(); QVERIFY(swapped);
    QCOMPARE(after.residentBytes, before.residentBytes - handle.layout.byteSize);
    QCOMPARE(after.committedBytes, before.committedBytes);
    tiles->unregisterResidencyObserver(tile, paused);
    QVERIFY(tile->blockSwapping()); tile->unblockSwapping(); QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::writeTransferRejectsMismatchedOperation()
{
    Fixture f; QVERIFY(f.init(4, 1));
    KisSurfaceEpochState surface; QVERIFY(f.store->resolveSurfaceState({1}, {}, &surface));
    const auto descriptor = surface.allocationDescriptor();
    KisPageOwnerLedger owner; QVERIFY(owner.configure(f.completions));
    QVERIFY(owner.registerProvider(f.provider));
    KisBackingBudgetController budget;
    KisPageMetadataCoordinator metadata; KisImageEpochReferenceModel epochs;
    KisPageWriteCoordinator coordinator(metadata, epochs, budget, owner);
    const auto write = f.provider->prepareWrite(owner.nextOperationId(), {key(), {2}},
        descriptor, cpu.domain, KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
    QVERIFY(write.isValid());
    KisPageWriteIntent intent; intent.inputKind = KisPageWriteInputKind::MutableGuard;
    intent.mode = KisPageWriteMode::PreserveContents;
    int transfers = 0; f.provider->beforeTransfer = [&] { ++transfers; };
    f.provider->mismatchTransferOperation = true;
    const auto completion = coordinator.initializeFreshReplica(intent, false, f.initialReplicas.first(),
        write.replica, descriptor, f.provider, KisPagePriority::Normal, write.completion, &f.error);
    QVERIFY(!completion.isValid());
    QVERIFY(f.error.contains(QStringLiteral("mismatched")));
    QCOMPARE(transfers, 1);
    QCOMPARE(owner.providerOperationCount(), qsizetype(0));
    QVERIFY(f.provider->retire(owner.nextOperationId(), write.replica, {}).isValid());
    QVERIFY(f.store->closeSession());
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::retirementPreparationRollsBackAtCapacity()
{
    int refused = 0;
    int accepted = 0;
    // Sweep real allocator capacity through scratch maps, slot growth and
    // persistent claim-node insertion. Each sample starts with a cold ledger.
    for (size_t headroom = 0; headroom <= 4096; headroom += 64) {
        ReadTerminalFixture f(256 * 1024, 2 * 64 * 64 * 4);
        f.metadata.attachBackingBudget(f.budget);
        QVERIFY2(f.init(nullptr, true), qPrintable(f.error));
        KisSurfaceEpochState surface;
        QVERIFY(f.physical.store->resolveSurfaceState({1}, {}, &surface));
        KisPageWriteCoordinator write(f.metadata, f.epochs, f.budget, f.owner);
        auto backing = write.reserveBacking(surface.allocationDescriptor(), cpu.domain,
                                             KisBackingBudgetClass::RetainedHistory, &f.error);
        QVERIFY2(backing.reservation.isValid(), qPrintable(f.error));
        const auto replica = f.physical.provider->prepareWrite(f.owner.nextOperationId(),
            {key(), {3}}, surface.allocationDescriptor(), cpu.domain,
            KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
        QVERIFY(replica.isValid());
        QVERIFY(f.owner.registerBacking(replica.replica, backing.reservation,
            KisBackingBudgetClass::RetainedHistory, &f.error, &backing.retirement));
        // Initial adoption is complete; its caller-owned fallback is no
        // longer live input to this independent class-change preparation.
        backing.reservation.release();
        const std::array<KisPageTransitionEffect, 2> effects{{{f.replica, {}}, {replica.replica, {}}}};
        const quint64 originalDebt = f.budget.usage()
            .buckets[size_t(KisBackingBudgetClass::RetirementDebt)].reserved.cpuRam;
        const size_t fillerBytes = size_t(f.limits.metadataArenaBytes - f.live()) - headroom;
        void *filler = kisAllocateMutationStorage(&f.budget, fillerBytes, 1);
        const auto release = qScopeGuard([&] { kisFreeMutationStorage(&f.budget, filler, fillerBytes, 1); });
        quint64 cookie = 0;
        const bool prepared = f.owner.prepareRetirementDebt(effects.data(), 2, &cookie, &f.error);
        if (prepared) { ++accepted; f.owner.cancelRetirementDebt(cookie); }
        else { ++refused; QCOMPARE(cookie, quint64(0)); }
        for (const auto &effect : effects) {
            QCOMPARE(f.owner.backingClass(effect.replica), KisBackingBudgetClass::RetainedHistory);
        }
        QCOMPARE(f.budget.usage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].reserved.cpuRam,
                 originalDebt);
        kisFreeMutationStorage(&f.budget, std::exchange(filler, nullptr), fillerBytes, 1);
        // A failed preparation must leave neither a key claim nor a physical
        // claim behind; the same two original backings can immediately retry.
        QVERIFY2(f.owner.prepareRetirementDebt(effects.data(), 2, &cookie, &f.error), qPrintable(f.error));
        f.owner.commitRetirementDebt(cookie);
        for (const auto &effect : effects) {
            QCOMPARE(f.owner.backingClass(effect.replica), KisBackingBudgetClass::RetirementDebt);
        }
    }
    QVERIFY(refused > 0);
    QVERIFY(accepted > 0);
}

void KisPageStoreCpuMutationTest::historyPreparationRevalidatesProtection()
{
    ReadTerminalFixture f(256 * 1024);
    f.metadata.attachBackingBudget(f.budget);
    KisPageTransition read;
    bool installed = false;
    struct Context { ReadTerminalFixture *fixture; KisPageTransition *read; bool *installed; } context{&f, &read, &installed};
    f.metadata.attachRetirementDebtOwner(&context,
        +[](void *p, const KisPageTransitionEffect *effects, qsizetype count, quint64 *cookie, QString *error) {
            auto &c = *static_cast<Context *>(p);
            if (!c.fixture->owner.prepareRetirementDebt(effects, count, cookie, error)) return false;
            // Change the real page revision during the existing shard-unlock
            // window, after Debt preparation and before authoritative removal.
            if (!*c.installed)
                *c.installed = c.fixture->metadata.applyOwner(c.read->version.key, *c.read).accepted;
            return true;
        }, +[](void *p, quint64 cookie, const KisPageTransitionEffect *effects, qsizetype count) noexcept {
            auto &f = *static_cast<Context *>(p)->fixture; f.owner.commitRetirementDebt(cookie);
            for (qsizetype i = 0; i < count; ++i) f.retirement.acceptEffect(effects[i]);
        },
        +[](void *p, quint64 cookie) noexcept { static_cast<Context *>(p)->fixture->owner.cancelRetirementDebt(cookie); });
    QVERIFY2(f.init(nullptr, true), qPrintable(f.error));
    read.kind = KisPageTransitionKind::AcquireRead;
    read.version = f.replica.version;
    read.target = f.replica;
    read.lease = f.owner.nextLeaseId();
    const auto detach = qScopeGuard([&] {
        f.history.stopAutomaticWakeups();
        QMutexLocker lock(&f.mutex); f.history.waitForIdleLocked();
    });
    { QMutexLocker lock(&f.mutex); const auto k = key(); f.history.collectUnreachableLocked(&k, 1); }
    QTRY_VERIFY_WITH_TIMEOUT(([&] { QMutexLocker lock(&f.mutex); return f.history.snapshotLocked().deferredPages == 1; }()), 5000);
    QVERIFY(installed);
    QCOMPARE(f.physical.provider->retireCalls.load(), 0);
    QCOMPARE(f.owner.backingClass(f.replica), KisBackingBudgetClass::RetainedHistory);
    QCOMPARE(f.budget.usage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].reserved.cpuRam, quint64(0));
    KisPageStateSnapshot page;
    QVERIFY(f.metadata.pageSnapshot(key(), &page));
    QVERIFY(page.findVersion(f.replica.version)->replicas.first().readLeases.contains(read.lease));
    read.kind = KisPageTransitionKind::ReleaseRead;
    { QMutexLocker lock(&f.mutex);
      QVERIFY(f.metadata.applyOwner(key(), read).accepted);
      const auto k = key(); f.history.collectUnreachableLocked(&k, 1); }
    QTRY_COMPARE_WITH_TIMEOUT(f.physical.provider->retireCalls.load(), 1, 5000);
    f.retirement.waitForIdle();
    { QMutexLocker lock(&f.mutex); f.history.waitForIdleLocked(); }
    QVERIFY(f.retirement.isDrained());
    QCOMPARE(f.references.loadAcquire(), 1);
}

void KisPageStoreCpuMutationTest::retirementResultBindsAtCapacity()
{
    QFETCH(bool, close);
    ReadTerminalFixture f(256 * 1024);
    f.metadata.attachBackingBudget(f.budget);
    f.metadata.attachRetirementDebtOwner(&f,
        +[](void *p, const KisPageTransitionEffect *effects, qsizetype count, quint64 *cookie, QString *error) {
            return static_cast<ReadTerminalFixture *>(p)->owner.prepareRetirementDebt(effects, count, cookie, error);
        }, +[](void *p, quint64 cookie, const KisPageTransitionEffect *effects, qsizetype count) noexcept {
            auto &f = *static_cast<ReadTerminalFixture *>(p); f.owner.commitRetirementDebt(cookie);
            for (qsizetype i = 0; i < count; ++i) f.retirement.acceptEffect(effects[i]);
        }, +[](void *p, quint64 cookie) noexcept { static_cast<ReadTerminalFixture *>(p)->owner.cancelRetirementDebt(cookie); });
    QVERIFY2(f.init(nullptr, true), qPrintable(f.error));
    const auto ticket = f.physical.completions->allocatePending(
        f.physical.completions->registerSource(KisCompletionDomain::CpuJob));
    QVERIFY(ticket.isValid());
    f.physical.provider->deferredRetirement = ticket;
    void *filler = nullptr;
    size_t fillerBytes = 0;
    f.physical.provider->afterRetire = [&](const KisReplicaHandle &, const KisReplicaOperation &result) {
        if (!result.isValid()) return;
        fillerBytes = size_t(f.limits.metadataArenaBytes - f.live());
        filler = kisAllocateMutationStorage(&f.budget, fillerBytes, 1);
    };
    const auto cleanup = qScopeGuard([&] {
        f.physical.provider->afterRetire = {};
        f.physical.completions->complete(ticket, KisCompletionStatus::Succeeded);
        kisFreeMutationStorage(&f.budget, filler, fillerBytes, 1);
    });
    { QMutexLocker lock(&f.mutex); const auto k = key(); f.history.collectUnreachableLocked(&k, 1); }
    QTRY_COMPARE_WITH_TIMEOUT(f.owner.providerOperationCount(), qsizetype(1), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(f.retirement.snapshot().retryWakeups >= 3, 5000);
    f.retirement.waitForIdle();
    QCOMPARE(f.live(), f.limits.metadataArenaBytes);
    QCOMPARE(f.physical.provider->retireCalls.load(), 1);
    QCOMPARE(f.owner.publicationBlockingOperationCount(), qsizetype(0));
    QVERIFY(!f.retirement.isDrained());
    KisPageRetirementRecords records;
    if (close) {
        f.retirement.beginClose();
        f.retirement.waitForIdle();
        records = f.retirement.takeForClose();
        QCOMPARE(records.size(), size_t(1));
    }
    QVERIFY(f.physical.completions->complete(ticket, KisCompletionStatus::Succeeded));
    if (close) {
        QVERIFY(f.retirement.retireRecord(records.front()));
        records.clear_and_dispose(KisPageRetirementRecordDeleter{});
    } else {
        QTRY_VERIFY_WITH_TIMEOUT(f.retirement.isDrained(), 5000);
        f.retirement.waitForIdle();
    }
    QCOMPARE(f.physical.provider->retireCalls.load(), 1);
    QCOMPARE(f.owner.providerOperationCount(), qsizetype(0));
    QCOMPARE(f.physical.provider->memoryUsage().committedBytes, quint64(0));
    QVERIFY(f.live() < f.limits.metadataArenaBytes);
}

void KisPageStoreCpuMutationTest::retirementBudgetWaitersProgress()
{
    QFETCH(int, bpp); QFETCH(int, pages); QFETCH(bool, closeWaiting);
    Fixture f; QVERIFY(f.init(bpp));
    const quint64 bytes = quint64(64 * 64 * bpp);
    KisPageBackingLimits limits; limits.retirementDebtBytes = bytes;
    KisBackingBudgetController budget(limits);
    KisPageOwnerLedger owner; QVERIFY(owner.configure(f.completions));
    owner.attachBackingBudget(budget); QVERIFY(owner.registerProvider(f.provider));
    KisPageMetadataCoordinator metadata; QVERIFY(metadata.configure(1));
    QAtomicInt references{1};
    KisPageRetirementQueue queue(owner, metadata, budget, references, &references,
        [](void *p) { static_cast<QAtomicInt *>(p)->deref(); });
    queue.prepareTask();
    const auto cleanup = qScopeGuard([&] {
        queue.beginClose(); queue.waitForIdle();
        auto records = queue.takeForClose();
        // Normal success empties these. On assertion failure, provider ownership
        // and fixture teardown remain alive until queued notifications are drained.
        for (auto &record : records) queue.retireRecord(record);
        kisDrainPageStoreReclamation();
    });
    KisSurfaceEpochState surface; QVERIFY(f.store->resolveSurfaceState({1}, {}, &surface));
    KisBackingBudgetDelta debt; debt.buckets[size_t(KisBackingBudgetClass::RetirementDebt)].cpuRam = qint64(bytes);
    auto block = budget.reserve(debt, nullptr); QVERIFY(block.isValid());
    for (int i = 0; i < pages; ++i) {
        const auto result = f.provider->prepareWrite(owner.nextOperationId(), {key(i), {1}},
            surface.allocationDescriptor(), KisPageAccessDomain::CpuRam,
            KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
        QVERIFY(result.isValid());
        KisBackingBudgetDelta current; current.buckets[size_t(KisBackingBudgetClass::Current)].cpuRam = qint64(bytes);
        auto charge = budget.reserve(current, nullptr); QVERIFY(charge.isValid());
        QVERIFY(owner.registerBacking(result.replica, charge, KisBackingBudgetClass::Current));
        queue.retireOrDefer(result.replica, f.provider, {}, {});
    }
    QCOMPARE(budget.usage().waitingRequests, quint32(qMin(32, pages)));
    QCOMPARE(f.provider->retireCalls.load(), 0);
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::Current)].live.cpuRam, pages * bytes);
    if (pages <= 32) QVERIFY(!queue.snapshot().retryScheduled);
    if (closeWaiting) {
        queue.beginClose(); queue.waitForIdle();
        QCOMPARE(budget.usage().waitingRequests, quint32(0));
        block.release();
        auto records = queue.takeForClose(); QCOMPARE(records.size(), qsizetype(pages));
        for (auto &record : records) QVERIFY(queue.retireRecord(record));
    } else {
        block.release();
        QTRY_VERIFY_WITH_TIMEOUT(queue.isDrained(), 10000);
    }
    queue.waitForIdle();
    QCOMPARE(f.provider->retireCalls.load(), pages);
    QCOMPARE(budget.usage().waitingRequests, quint32(0));
    QCOMPARE(budget.usage().grantedRequests, quint32(0));
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].live.cpuRam, quint64(0));
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::Current)].live.cpuRam, quint64(0));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
    QTRY_COMPARE_WITH_TIMEOUT(references.loadAcquire(), 1, 5000);
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::reclamationDeadlinesCancelAndDoNotOccupyWorker()
{
    const auto calls = std::make_shared<std::atomic<int>>(0);
    auto captured = std::make_shared<int>(1);
    const std::weak_ptr<int> weak = captured;
    KisPageReclamationDelay cancelled(KisPageReadinessCallback([captured, calls] { ++*calls; }));
    QVERIFY(cancelled.arm(60000));
    captured.reset();
    QVERIFY(!weak.expired());
    auto moved = std::move(cancelled);
    QVERIFY(!cancelled.isValid()); QVERIFY(moved.isValid());
    moved.reset();
    QVERIFY(weak.expired()); // The actual queued node/captures were removed now.
    QCOMPARE(calls->load(), 0);

    QSemaphore dispatched, immediate;
    std::atomic<bool> wrongThread{false};
    KisPageReclamationDelay future(KisPageReadinessCallback([calls] { ++*calls; }));
    QVERIFY(future.arm(60000));
    kisSchedulePageStoreReclamation([&] { immediate.release(); });
    // A future deadline must not put the sole reclamation worker to sleep.
    QVERIFY(immediate.tryAcquire(1, 5000));
    future.reset();
    KisPageReclamationDelay near(KisPageReadinessCallback([&] {
        wrongThread = kisOnPageStoreReclamationThread();
        kisSchedulePageStoreReclamation([&] { dispatched.release(); });
    }));
    QVERIFY(near.arm(1));
    QVERIFY(dispatched.tryAcquire(1, 5000));
    near.reset(); kisDrainPageStoreReclamation();
    QVERIFY(!wrongThread.load()); QCOMPARE(calls->load(), 0);
}

void KisPageStoreCpuMutationTest::autonomousRetirementRetries()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp));
    const auto fill = [&](quint8 value) {
        const auto tx = f.store->beginCurrentTransaction();
        return KisPageStoreCpuSurfaceOps::fillRect(f.store.get(), {1}, tx,
            QRect(0, 0, 129 * 64, 64), QByteArray(bpp, char(value)), &f.error) &&
            f.store->commit(tx, f.store->preparedPages(tx)).isValid();
    };
    QVERIFY(fill(0x31)); f.provider->rejectRetire = true; QVERIFY(fill(0x77));
    QTRY_COMPARE(f.store->sessionStats().nextRetirementRetryDelayMs, 100);
    const auto pending = f.store->sessionStats();
    QCOMPARE(pending.pendingRetiredReplicas, qsizetype(129));
    QCOMPARE(pending.pendingRetiredBytes, quint64(129 * 64 * 64 * bpp));
    QVERIFY(pending.retirementRetryWakeups > 0);
    QVERIFY(pending.scheduledRetirementRetries <= 1);
    QVERIFY(pending.maximumReplicasPerRetirementRetryWake <= 32);
    QVERIFY(pending.maximumReplicasPerRetirementPass <= 32);
    // An indefinitely rejecting store cannot starve another store's work.
    Fixture other; QVERIFY(other.init(bpp));
    QVERIFY(other.fill(0x15)); QVERIFY(other.fill(0x53));
    QCOMPARE(other.store->sessionStats().pendingRetiredReplicas, qsizetype(0));
    QVERIFY(other.store->closeSession());
    f.provider->rejectRetire = false;
    QTRY_COMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(0));
    const auto drained = f.store->sessionStats();
    QCOMPARE(drained.delayedRetiredReplicas, qsizetype(0));
    QCOMPARE(drained.scheduledRetirementRetries, qsizetype(0));
    QCOMPARE(drained.pendingRetiredBytes, quint64(0));
    QVERIFY(drained.maximumReplicasPerRetirementRetryWake <= 32);
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(129 * 64 * 64 * bpp));
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x77));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::noSignalPinRetirementRetries()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto replica = f.provider->lastTarget;
    const auto binding = f.provider->cpuResidentBinding(replica);
    QVERIFY(binding); QVERIFY(binding->acquireRead(replica.allocationIdentity(), true));
    bool pinned = true;
    auto unpin = qScopeGuard([&] { if (pinned) binding->releaseRead(); });
    f.provider->rejectBindingFor = replica; // provider has no subscribable binding
    QVERIFY(f.fill(0x77));
    QTRY_VERIFY(f.store->sessionStats().retirementRetryWakeups > 0);
    QCOMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(1));
    binding->releaseRead(); pinned = false;
    QTRY_COMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(0));
    QCOMPARE(f.store->sessionStats().scheduledRetirementRetries, qsizetype(0));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::retirementRetryCloseAndLifetime()
{
    QFETCH(int, mode);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    QSemaphore blocked, resume;
    std::atomic<int> callsUntilBlock{0};
    f.provider->beforeRetire = [&] {
        const int remaining = callsUntilBlock.load();
        if (remaining > 0 && callsUntilBlock.fetch_sub(1) == 1) {
            blocked.release(); resume.acquire();
        }
    };
    auto unblock = qScopeGuard([&] {
        f.store.reset(); // stop future callbacks before the fixture captures die
        resume.release(); kisDrainPageStoreReclamation();
        if (f.provider) f.provider->beforeRetire = {};
    });
    f.provider->rejectRetire = true; QVERIFY(f.fill(0x77));
    QTRY_COMPARE(f.store->sessionStats().nextRetirementRetryDelayMs, 100);
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(2));
    if (mode == 0) {
        auto view = f.store->captureReadView({}); QVERIFY(view.isValid());
        QVERIFY(!f.store->closeSession());
        f.provider->rejectRetire = false;
        QTRY_COMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(0));
        view = {}; QVERIFY(f.store->closeSession());
    } else if (mode == 1) {
        QVERIFY(!f.store->closeSession());
        const auto closed = f.store->sessionStats();
        QCOMPARE(closed.pendingShutdownReplicas, qsizetype(4));
        QCOMPARE(closed.scheduledRetirementRetries, qsizetype(0));
        QCOMPARE(closed.pendingRetiredReplicas, qsizetype(0));
        f.provider->rejectRetire = false;
        QVERIFY(f.store->closeSession());
        kisDrainPageStoreReclamation();
        QCOMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(0));
    } else if (mode == 2) {
        const std::weak_ptr<TestProvider> weak = f.provider;
        f.store.reset(); f.provider.reset();
        kisDrainPageStoreReclamation();
        QVERIFY(weak.expired()); // A dormant deadline cannot retain the owner.
    } else {
        // The first rejected record rearms the queue's deadline; block the
        // second until that deadline has dispatched behind the active worker.
        callsUntilBlock = 2;
        QVERIFY(blocked.tryAcquire(1, 5000));
        QSemaphore laterDeadline;
        KisPageReclamationDelay later(KisPageReadinessCallback([&] { laterDeadline.release(); }));
        QVERIFY(later.arm(200));
        QVERIFY(laterDeadline.tryAcquire(1, 5000));
        later.reset();
        const auto calls = f.provider->retireCalls.load();
        const std::weak_ptr<TestProvider> weak = f.provider;
        f.store.reset();
        resume.release(); kisDrainPageStoreReclamation();
        QCOMPARE(f.provider->retireCalls.load(), calls); // No recurring continuation.
        f.provider->beforeRetire = {};
        f.provider.reset();
        QVERIFY(weak.expired());
    }
}

void KisPageStoreCpuMutationTest::closeRetriesDetachedReplicas()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    f.provider->rejectRetire = true;
    QVERIFY(f.fill(0x77));
    QCOMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(2));
    QVERIFY(!f.store->closeSession(&f.error));
    QCOMPARE(f.error, QStringLiteral("PageStore shutdown retirement is pending"));
    const auto failedClose = f.store->sessionStats();
    QCOMPARE(failedClose.pendingShutdownReplicas, qsizetype(4));
    QCOMPARE(failedClose.pendingRetiredReplicas, qsizetype(0));
    QCOMPARE(failedClose.retirementCloseDrainedReplicas, quint64(2));
    f.provider->rejectRetire = false;
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    QVERIFY(!f.store->sessionStats().hasOutstandingCapabilities());
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::closePollsDetachedRetirement()
{
    QFETCH(bool, background);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const KisCompletionDomain source = KisCompletionDomain::CpuJob;
    const auto ticket = f.completions->allocatePending(f.completions->registerSource(source));
    QVERIFY(ticket.isValid());
    f.provider->deferredRetirement = ticket;
    auto unblock = qScopeGuard([&] {
        f.completions->complete(ticket, KisCompletionStatus::Succeeded);
        f.store->waitForRetirementIdle();
        f.store->processRetirements(100);
    });
    if (background) {
        QVERIFY(f.fill(0x71));
        QTRY_VERIFY(f.completions->sourceStatistics(ticket.source()).readinessWaiters > 0);
    }
    QVERIFY(!f.store->closeSession(&f.error));
    QVERIFY(f.store->sessionStats().hasOutstandingCapabilities());
    QVERIFY(f.store->sessionStats().providerOperations > 0);
    // Shutdown takes the original retirement records and drops their waits;
    // the underlying operation remains pending, but notification storage does not.
    QCOMPARE(f.completions->status(ticket), KisCompletionStatus::Pending);
    const auto notifications = f.completions->sourceStatistics(ticket.source());
    QCOMPARE(notifications.readinessWaiters, quint64(0));
    QCOMPARE(notifications.readinessSignals, quint64(0));
    QCOMPARE(notifications.readinessCapacity, quint64(0));
    QVERIFY(f.completions->complete(ticket, KisCompletionStatus::Succeeded));
    // close itself must poll the saved operation. A manually drained queue
    // must not be required, especially after the store became non-operational.
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    QCOMPARE(f.provider->retireCalls.load(), background ? 4 : 2);
    QCOMPARE(f.store->sessionStats().providerOperations, qsizetype(0));
    QVERIFY(!f.store->sessionStats().hasOutstandingCapabilities());
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::physicalPinDelaysDetachedRetirement()
{
    QFETCH(bool, generic);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto pinnedIdentity = f.provider->lastTarget.allocationIdentity();
    auto binding = f.provider->cpuResidentBinding(f.provider->lastTarget);
    QVERIFY(binding);
    KisPageOwnerLedger identities;
    QVERIFY(identities.configure(f.completions, &f.error));
    auto access = generic ? f.provider->resolveAccess(identities.nextLeaseId(), identities.nextOperationId(),
        f.provider->lastTarget, cpu, KisPageAccessMode::Read) : KisReplicaAccess{};
    const auto *bytes = static_cast<const char *>(generic ? access.cpuReadData
        : binding->acquireRead(pinnedIdentity, true));
    QVERIFY(bytes);
    QCOMPARE(bytes[0], char(0x31));
    QVERIFY(f.fill(0x77));
    QCOMPARE(bytes[0], char(0x31));
    QCOMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(1));
    const auto blocked = f.store->processRetirements(1);
    QCOMPARE(blocked.retired, qsizetype(0));
    QCOMPARE(blocked.pending, qsizetype(1));
    if (generic) f.provider->releaseAccess(std::move(access), {});
    else binding->releaseRead();
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(0));
    QCOMPARE(f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].live.cpuRam,
             quint64(0));
    QVERIFY(!binding->acquireRead(pinnedIdentity, true));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::retirementReleaseBeforeRegistration()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto replica = f.provider->lastTarget;
    const auto binding = f.provider->cpuResidentBinding(replica);
    QVERIFY(binding); QVERIFY(binding->acquireRead(replica.allocationIdentity(), true));
    std::atomic<bool> released{false};
    f.provider->afterRetire = [&](const auto &retired, const auto &result) {
        if (retired == replica && !result.isValid() && !released.exchange(true)) binding->releaseRead();
    };
    auto unpin = qScopeGuard([&] { if (!released.exchange(true)) binding->releaseRead(); });
    QVERIFY(f.fill(0x77));
    QVERIFY(released.load());
    QCOMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(0));
    QCOMPARE(f.provider->retireCalls.load(), 3); // Two old pages, one failed pin check.
    f.provider->afterRetire = {};
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::completionWakesDetachedRetirement()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const auto source = f.completions->registerSource(KisCompletionDomain::CpuJob);
    const auto ticket = f.completions->allocatePending(source);
    f.provider->deferredRetirement = ticket;
    auto complete = qScopeGuard([&] {
        f.completions->complete(ticket, KisCompletionStatus::Succeeded);
        f.store->waitForRetirementIdle();
    });
    QVERIFY(f.fill(0x77));
    QCOMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(2));
    QCOMPARE(f.completions->sourceStatistics(source).readinessWaiters, quint64(2));
    QCOMPARE(f.store->sessionStats().scheduledRetirementRetries, qsizetype(0));
    QCOMPARE(f.store->sessionStats().retirementRetryWakeups, quint64(0));
    const auto attempts = f.provider->retireCalls.load();
    QVERIFY(f.completions->complete(ticket, KisCompletionStatus::Succeeded));
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(0));
    QCOMPARE(f.store->sessionStats().providerOperations, qsizetype(0));
    QCOMPARE(f.completions->sourceStatistics(source).readinessWaiters, quint64(0));
    QCOMPARE(f.provider->retireCalls.load(), attempts);
    QCOMPARE(f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].live.cpuRam,
             quint64(0));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::retirementWakeCloseAndOwnerLifetime()
{
    QFETCH(int, mode);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto replica = f.provider->lastTarget;
    auto binding = f.provider->cpuResidentBinding(replica);
    QVERIFY(binding); QVERIFY(binding->acquireRead(replica.allocationIdentity(), true));
    bool pinned = true;
    auto unpin = qScopeGuard([&] { if (pinned) binding->releaseRead(); });
    QVERIFY(f.fill(0x77));
    QCOMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(1));
    if (mode == 0) {
        // An actual retained view makes close reject before taking ownership
        // of shutdown records. Resumption must rearm the old pending record.
        auto view = f.store->captureReadView({});
        QVERIFY(view.isValid());
        QVERIFY(!f.store->closeSession());
        QVERIFY(f.store->waitForRetirementIdle());
        binding->releaseRead(); pinned = false;
        QVERIFY(f.store->waitForRetirementIdle());
        QCOMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(0));
        view = {};
        QVERIFY(f.store->closeSession());
        return;
    }
    QSemaphore entered, resume;
    kisSchedulePageStoreReclamation([&] { entered.release(); resume.acquire(); });
    entered.acquire();
    auto unblock = qScopeGuard([&] { resume.release(); kisDrainPageStoreReclamation(); });
    const std::weak_ptr<TestProvider> provider = f.provider;
    if (mode == 2) { binding->releaseRead(); pinned = false; }
    f.store.reset(); f.provider.reset();
    QVERIFY(!provider.expired()); // Destruction/wakeup is still behind the barrier.
    resume.release(); unblock.dismiss();
    kisDrainPageStoreReclamation();
    QVERIFY(provider.expired()); // Neither dormant nor dispatched waits cycle.
    if (pinned) { binding->releaseRead(); pinned = false; }
}

void KisPageStoreCpuMutationTest::virtualDefaultOnlyAllocatesTarget()
{
    QFETCH(int, bpp);
    QFETCH(bool, generic);
    Fixture f; QVERIFY(f.init(bpp));
    const auto tx = f.store->beginCurrentTransaction();
    const auto before = f.provider->p->payloadWork();
    auto segment = generic ? KisPageMutationSession{} : f.begin(tx);
    auto guard = generic ? KisCpuWriteGuard{} : segment.beginWrite(key(), &f.error);
    const auto request = generic ? f.store->acquireWrite(tx, key(), cpu, KisPageWriteMode::PreserveContents,
                                                        KisPagePriority::Normal) : KisWriteRequest{};
    if (generic) QVERIFY2(request.isValid(), qPrintable(request.error));
    auto lease = generic ? f.store->resolve(request, request.readiness) : KisWriteLease{};
    void *data = generic ? lease.cpuData() : guard.data(); QVERIFY2(data, qPrintable(f.error));
    const auto prepared = f.provider->p->payloadWork();
    QCOMPARE(prepared.defaultInitializedPages, before.defaultInitializedPages + 1);
    QCOMPARE(prepared.nativeDuplicatePages, before.nativeDuplicatePages);
    QCOMPARE(prepared.explicitCopyBytes, before.explicitCopyBytes);
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(64 * 64 * bpp));
    QCOMPARE(QByteArray(static_cast<const char *>(data), 64 * 64 * bpp),
             QByteArray(64 * 64 * bpp, char(0x2a)));
    static_cast<char *>(data)[0] = char(0x77);
    // A reader of the still-current default must not see the pending bytes,
    // nor invalidate a writer whose before-image is a logical default.
    const auto read = f.store->acquireRead(key(), {}, cpu, KisPagePriority::Normal);
    QVERIFY2(read.isValid(), qPrintable(read.error));
    auto beforeLease = f.store->resolve(read, read.readiness);
    QVERIFY(beforeLease.isValid());
    QCOMPARE(static_cast<const char *>(beforeLease.cpuData())[0], char(0x2a));
    guard = {};
    if (generic) QVERIFY(f.store->publishHostWrite(std::move(lease)).isValid());
    else QVERIFY(segment.seal());
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QCOMPARE(static_cast<const char *>(beforeLease.cpuData())[0], char(0x2a));
    f.store->release(std::move(beforeLease));
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x77));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::discardDoesNotCopySource()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    auto beforeView = f.store->captureReadView();
    auto before = beforeView.tryReadResidentPage(key()); QVERIFY(before.isValid());
    const auto tx = f.store->beginCurrentTransaction();
    const auto payload = f.provider->p->payloadWork();
    auto segment = f.begin(tx);
    auto guard = segment.beginWrite(key(), KisPageWriteMode::DiscardContents, &f.error);
    QVERIFY2(guard.isValid(), qPrintable(f.error));
    const auto version = guard.version();
    memset(guard.data(), 0x77, size_t(guard.byteSize()));
    guard = {};
    guard = segment.beginWrite(key(), KisPageWriteMode::PreserveContents);
    QVERIFY(guard.isValid());
    QCOMPARE(guard.version(), version);
    QCOMPARE(static_cast<const char *>(guard.data())[0], char(0x77));
    guard = {};
    QCOMPARE(f.provider->p->payloadWork().nativeDuplicatePages, payload.nativeDuplicatePages);
    QCOMPARE(f.provider->p->payloadWork().explicitCopyBytes, payload.explicitCopyBytes);
    QVERIFY(segment.seal());
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QCOMPARE(static_cast<const char *>(before.data())[0], char(0x31));
    before = {}; beforeView = {};
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x77));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::completePayload_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("baseKind");
    QTest::addColumn<bool>("fallback"); QTest::addColumn<bool>("commit");
    for (int bpp : {1,4,8,16}) for (int base : {0,1,2}) for (bool fallback : {false,true}) for (bool commit : {false,true})
        QTest::newRow(qPrintable(QStringLiteral("bpp%1-base%2-fallback%3-commit%4").arg(bpp).arg(base).arg(fallback).arg(commit)))
            << bpp << base << fallback << commit;
}

void KisPageStoreCpuMutationTest::completePayload()
{
    QFETCH(int,bpp); QFETCH(int,baseKind); QFETCH(bool,fallback); QFETCH(bool,commit);
    Fixture f; QVERIFY(f.init(bpp)); if (baseKind) QVERIFY(f.fill(0x31));
    f.provider->disableCpuPayload = fallback;
    auto before = f.store->captureReadView();
    const auto tx = f.store->beginCurrentTransaction(); auto segment = f.begin(tx);
    if (baseKind == 2) QVERIFY(segment.removePage(key()));
    const int stride = 64*bpp+17;
    QByteArray input(63*stride+64*bpp, char(0xee)), expected(64*64*bpp, Qt::Uninitialized);
    for (int row=0;row<64;++row) {
        memset(input.data()+row*stride, 0x50+row%32, size_t(64*bpp));
        memset(expected.data()+row*64*bpp, 0x50+row%32, size_t(64*bpp));
    }
    const auto work = f.provider->p->payloadWork(); const auto counts = f.store->mutationStatistics();
    const auto requests = f.store->sessionStats();
    QVERIFY2(segment.overwritePage(key(), {input.constData(),stride,input.size()}, &f.error), qPrintable(f.error));
    input.fill(char(0x77)); // provider must not retain/alias borrowed host memory
    auto guard = segment.beginWrite(key()); QVERIFY(guard.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(guard.data()), int(guard.byteSize())), expected);
    const auto version = guard.version();
    auto *tile = f.provider->p->tileDataForCpuWriteGuard(guard); QVERIFY(tile); QVERIFY(tile->ref());
    const auto releaseTile = qScopeGuard([&] { tile->deref(); });
    guard = {};
    auto *tileStore = KisTileDataStore::instance();
    const bool swapped = tileStore->trySwapTileData(tile); QVERIFY(swapped);
    guard = segment.beginWrite(key()); QVERIFY(guard.isValid()); QCOMPARE(guard.version(),version);
    QCOMPARE(QByteArray(static_cast<const char *>(guard.data()), int(guard.byteSize())),expected); guard = {};
    QVERIFY(segment.removePage(key()));
    QVERIFY(segment.overwritePage(key(), {input.constData(),stride,input.size()}));
    guard = segment.beginWrite(key()); QVERIFY(guard.isValid()); QCOMPARE(guard.version(),version);
    QCOMPARE(QByteArray(static_cast<const char *>(guard.data()),int(guard.byteSize())),QByteArray(64*64*bpp,char(0x77)));
    guard = {};
    auto old = before.readResidentPage(key()); QVERIFY(old.isValid());
    QCOMPARE(static_cast<const quint8 *>(old.data())[0], quint8(baseKind ? 0x31 : 0x2a)); old = {};
    if (commit) { QVERIFY(segment.seal()); QVERIFY(f.store->commit(tx,f.store->preparedPages(tx)).isValid()); }
    else { QVERIFY(segment.cancel()); QVERIFY(f.store->abort(tx)); }
    const auto after = f.store->mutationStatistics(); const auto payload = f.provider->p->payloadWork();
    QCOMPARE(after.generationsReserved-counts.generationsReserved,quint64(1));
    QCOMPARE(after.pendingDefaultResetBytes,counts.pendingDefaultResetBytes);
    QCOMPARE(after.pendingPayloadCopyBytes-counts.pendingPayloadCopyBytes,quint64(64*64*bpp*(fallback ? 2 : 1)));
    QCOMPARE(payload.payloadInitializedPages-work.payloadInitializedPages,quint64(fallback ? 0 : 1));
    QCOMPARE(payload.payloadInitializedBytes-work.payloadInitializedBytes,quint64(fallback ? 0 : 64*64*bpp));
    QCOMPARE(payload.defaultInitializedPages-work.defaultInitializedPages,quint64(fallback ? 1 : 0));
    QCOMPARE(payload.nativeDuplicatePages,work.nativeDuplicatePages); QCOMPARE(payload.explicitCopyBytes,work.explicitCopyBytes);
    QCOMPARE(f.store->sessionStats().readRequestsCreated,requests.readRequestsCreated);
    QCOMPARE(f.store->sessionStats().writeRequestsCreated,requests.writeRequestsCreated);
    QCOMPARE(f.pixel(),QByteArray(bpp,char(commit ? 0x77 : baseKind ? 0x31 : 0x2a)));
    before = {}; QVERIFY(f.store->closeSession()); QCOMPARE(f.provider->memoryUsage().committedBytes,quint64(0));
}

void KisPageStoreCpuMutationTest::invalidCompletePayload_data()
{
    QTest::addColumn<bool>("pending"); QTest::addColumn<int>("fault");
    for (bool pending : {false,true}) for (int fault=0;fault<7;++fault)
        QTest::newRow(qPrintable(QStringLiteral("pending%1-fault%2").arg(pending).arg(fault))) << pending << fault;
}

void KisPageStoreCpuMutationTest::invalidCompletePayload()
{
    QFETCH(bool,pending); QFETCH(int,fault);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction(); auto segment = f.begin(tx);
    QByteArray input(64*64*f.bpp,char(0x77));
    KisCpuPagePayload payload{input.constData(),64*f.bpp,input.size()};
    if (pending) QVERIFY(segment.overwritePage(key(),payload));
    switch (fault) {
    case 0: payload.data=nullptr; break;
    case 1: payload.rowStride=0; break;
    case 2: --payload.rowStride; break;
    case 3: payload.byteSize=-1; break;
    case 4: --payload.byteSize; break;
    case 5: payload.rowStride=std::numeric_limits<qsizetype>::max(); break;
    case 6: payload.data=reinterpret_cast<const void *>(std::numeric_limits<quintptr>::max()-3); break;
    }
    KisSurfaceEpochState state; QVERIFY(f.store->resolveSurfaceState({1},{},&state));
    QVERIFY(!payload.isValidFor(state.allocationDescriptor()));
    QVERIFY(!KisTileDataStore::instance()->createTileDataFromRows(f.bpp,
        static_cast<const quint8 *>(payload.data),payload.rowStride,payload.byteSize));
    const auto work = f.provider->p->payloadWork();
    QVERIFY(!segment.overwritePage(key(),payload,&f.error)); QVERIFY(!f.error.isEmpty());
    QVERIFY(!segment.seal()); QVERIFY(f.store->abort(tx));
    QCOMPARE(f.provider->p->payloadWork().payloadInitializedPages,work.payloadInitializedPages);
    QCOMPARE(f.pixel(),QByteArray(f.bpp,char(0x31))); QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::completePayloadPreparationClaims()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    QByteArray input(64*64*f.bpp,char(0x77));
    QSemaphore entered, release; std::atomic<bool> first{true};
    f.provider->beforeCpuPayload = [&] { if (first.exchange(false)) { entered.release(); release.acquire(); } };
    bool success=false;
    std::thread worker([&] {
        auto segment=f.begin(tx);
        success=segment.overwritePage(key(),{input.constData(),64*f.bpp,input.size()}) && segment.seal() &&
            f.store->commit(tx,f.store->preparedPages(tx)).isValid();
    });
    const auto join = qScopeGuard([&] { release.release(); if (worker.joinable()) worker.join(); });
    QVERIFY(entered.tryAcquire(1,5000)); QVERIFY(!f.store->abort(tx)); QVERIFY(!f.store->closeSession());
    auto old=f.store->captureReadView(); auto read=old.readResidentPage(key()); QVERIFY(read.isValid());
    QCOMPARE(static_cast<const quint8 *>(read.data())[0],quint8(0x31)); read={};
    const auto other=f.store->beginCurrentTransaction();
    auto sibling=f.store->beginMutation(other); QVERIFY(sibling.isActive());
    QVERIFY(sibling.overwritePage(key(2),{input.constData(),64*f.bpp,input.size()}));
    QVERIFY(sibling.seal()); QVERIFY(f.store->commit(other,f.store->preparedPages(other)).isValid());
    release.release(); worker.join(); QVERIFY(success);
    read=old.readResidentPage(key()); QVERIFY(read.isValid()); QCOMPARE(static_cast<const quint8 *>(read.data())[0],quint8(0x31));
    read={}; old={}; QCOMPARE(f.pixel(),QByteArray(f.bpp,char(0x77))); QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::completePayloadProviderFailure_data()
{
    QTest::addColumn<int>("fault"); for (int fault=0;fault<3;++fault) QTest::newRow(qPrintable(QString::number(fault))) << fault;
}

void KisPageStoreCpuMutationTest::completePayloadProviderFailure()
{
    QFETCH(int,fault);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx=f.store->beginCurrentTransaction(); auto segment=f.begin(tx);
    QByteArray input(64*64*f.bpp,char(0x77));
    f.provider->rejectWrite=fault==0; f.provider->rejectNativeBinding=fault==1;
    if (fault==2) f.provider->validationsUntilFailure=0;
    const auto work=f.provider->p->payloadWork();
    const bool prepared=segment.overwritePage(key(),{input.constData(),64*f.bpp,input.size()},&f.error);
    // Provider validation is a seal boundary, not part of synchronous allocation.
    QCOMPARE(prepared,fault==2);
    QVERIFY(!segment.seal()); QVERIFY(f.store->abort(tx));
    f.provider->rejectWrite=f.provider->rejectNativeBinding=false; f.provider->validationsUntilFailure=-1;
    QCOMPARE(f.provider->p->payloadWork().payloadInitializedPages-work.payloadInitializedPages,quint64(fault ? 1 : 0));
    QCOMPARE(f.provider->p->payloadWork().defaultInitializedPages,work.defaultInitializedPages);
    QCOMPARE(f.pixel(),QByteArray(f.bpp,char(0x31))); QVERIFY(f.store->closeSession());
    QCOMPARE(f.provider->memoryUsage().committedBytes,quint64(0));
}

void KisPageStoreCpuMutationTest::accessSessionUsesOneNativeSegment()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    const auto counts = f.store->sessionStats();
    const auto mutationCounts = f.store->mutationStatistics();
    KisPageStoreCpuAccessSession session;
    QVERIFY(session.beginWrite(f.store.get(), {1}, tx, KisPageWriteMode::PreserveContents,
                               KisPagePriority::Interactive, &f.error));
    const auto first = session.writePage(0, 0, &f.error); QVERIFY2(first.isValid(), qPrintable(f.error));
    first.data[0] = 0x77;
    const auto same = session.writePage(0, 0); QVERIFY(same.isValid());
    QCOMPARE(same.data, first.data); QCOMPARE(same.version, first.version);
    QCOMPARE(same.oldData[0], quint8(0x31));
    const auto second = session.writePage(1, 0); QVERIFY(second.isValid());
    second.data[0] = 0x55;
    // Growing the contiguous record cache must not revoke earlier byte spans.
    for (qint32 column = 2; column < 4; ++column) {
        const auto extra = session.writePage(column, 0);
        QVERIFY(extra.isValid());
        extra.data[0] = quint8(0x55 + column);
    }
    // Default OperationSpans must not inherit cursor invalidation semantics.
    QCOMPARE(first.data[0], quint8(0x77)); QCOMPARE(first.oldData[0], quint8(0x31));
    QCOMPARE(f.store->sessionStats().readRequestsCreated, counts.readRequestsCreated);
    QCOMPARE(f.store->sessionStats().writeRequestsCreated, counts.writeRequestsCreated);
    QVERIFY(!f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QVERIFY(session.finish(&f.error));
    const auto sealed = f.store->mutationStatistics();
    QCOMPARE(sealed.generationsReserved - mutationCounts.generationsReserved, quint64(4));
    QCOMPARE(sealed.pagesSealed - mutationCounts.pagesSealed, quint64(4));
    QCOMPARE(sealed.sessionsCreated - mutationCounts.sessionsCreated, quint64(1));
    QCOMPARE(sealed.maximumPinnedPagesPerExecution, quint64(4));
    QCOMPARE(f.pixel(), QByteArray(4, char(0x31))); // seal is not epoch publication
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x77));
    QCOMPARE(quint8(f.pixel({}, 1)[0]), quint8(0x55));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::cursorWriteLifetime_data()
{
    QTest::addColumn<bool>("native"); QTest::addColumn<bool>("seal");
    for (bool native : {false, true}) for (bool seal : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(native).arg(seal))) << native << seal;
}

void KisPageStoreCpuMutationTest::cursorWriteLifetime()
{
    QFETCH(bool, native); QFETCH(bool, seal);
    Fixture f; f.provider->disableNativeMutation = !native;
    f.provider->rejectNativeBinding = !native;
    QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay; overlay.transaction = tx.id;
    KisPageVersion before; QVERIFY(f.store->resolvePageVersion(key(), overlay, &before));
    const auto stats = f.store->mutationStatistics();
    const auto requests = f.store->sessionStats();
    KisPageStoreCpuAccessSession cursor(KisCpuAccessLifetime::CursorPage);
    QVERIFY(cursor.beginWrite(f.store.get(), {1}, tx, KisPageWriteMode::PreserveContents,
                               KisPagePriority::Normal, &f.error));
    auto a = cursor.writePage(0, 0); QVERIFY(a.isValid());
    const auto pending = a.version; a.data[0] = 0x71;
    auto b = cursor.writePage(1, 0); QVERIFY(b.isValid()); b.data[0] = 0x72;
    a = cursor.writePage(0, 0); QVERIFY(a.isValid());
    QCOMPARE(a.version, pending); QCOMPARE(a.data[0], quint8(0x71));
    QCOMPARE(a.oldData[0], quint8(0x31));
    QCOMPARE(f.store->sessionStats().activeReadLeases, qsizetype(native ? 0 : 1));
    QCOMPARE(f.store->sessionStats().activeWriteLeases, qsizetype(native ? 0 : 2));
    KisPageVersion visible; QVERIFY(f.store->resolvePageVersion(key(), overlay, &visible));
    QCOMPARE(visible, before); // Moving/parking never seals a partial segment.
    QVERIFY(!f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QVERIFY(!f.store->abort(tx));
    if (seal) QVERIFY(cursor.finish(&f.error)); else cursor.cancel();
    QCOMPARE(f.store->sessionStats().activeReadLeases, qsizetype(0));
    QCOMPARE(f.store->sessionStats().activeWriteLeases, qsizetype(0));
    const auto after = f.store->mutationStatistics();
    if (native) {
        QCOMPARE(after.generationsReserved - stats.generationsReserved, quint64(2));
        QCOMPARE(after.writablePinsAcquired - stats.writablePinsAcquired, quint64(3));
        QCOMPARE(after.writablePinsReleased - stats.writablePinsReleased, quint64(3));
        QCOMPARE(after.maximumPinnedPagesPerExecution, quint64(1));
        QCOMPARE(f.store->sessionStats().writeRequestsCreated, requests.writeRequestsCreated);
    } else {
        QCOMPARE(f.store->sessionStats().writeRequestsCreated - requests.writeRequestsCreated, quint64(2));
    }
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x31));
    if (seal) QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    else QVERIFY(f.store->abort(tx));
    QCOMPARE(quint8(f.pixel()[0]), quint8(seal ? 0x71 : 0x31));
    QCOMPARE(quint8(f.pixel({}, 1)[0]), quint8(seal ? 0x72 : 0x31));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::cursorReadRetainsViewNotPins_data()
{
    QTest::addColumn<bool>("native");
    QTest::newRow("native-and-cold") << true; QTest::newRow("generic") << false;
}

void KisPageStoreCpuMutationTest::cursorReadRetainsViewNotPins()
{
    QFETCH(bool, native);
    Fixture f; f.provider->disableNativeMutation = !native; f.provider->rejectNativeBinding = !native;
    QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto request = f.store->acquireRead(key(), {}, cpu, KisPagePriority::Normal);
    auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
    auto *tile = f.provider->p->tileDataForLease(lease.leaseId()); QVERIFY(tile); QVERIFY(tile->ref());
    const auto releaseTile = qScopeGuard([&] { tile->deref(); });
    f.store->release(std::move(lease));
    const auto swap = [&] {
        return KisTileDataStore::instance()->trySwapTileData(tile);
    };
    KisPageStoreCpuAccessSession cursor(KisCpuAccessLifetime::CursorPage);
    QVERIFY(cursor.beginRead(f.store.get(), {1}, {}, KisPagePriority::Normal));
    auto a = cursor.readPage(0, 0); QVERIFY(a.isValid()); const auto oldVersion = a.version;
    QCOMPARE(a.data[0], quint8(0x31)); QVERIFY(!swap());
    QVERIFY(f.fill(0x71)); QVERIFY(f.setDefault(0x61));
    auto b = cursor.readPage(1, 0); QVERIFY(b.isValid()); QCOMPARE(b.data[0], quint8(0x31));
    QVERIFY(swap()); // Root retention alone does not hold the previous RAM pin.
    a = cursor.readPage(0, 0); QVERIFY(a.isValid()); QCOMPARE(a.version, oldVersion);
    QCOMPARE(a.data[0], quint8(0x31));
    QCOMPARE(f.store->sessionStats().activeReadLeases, qsizetype(1)); // actual cold fallback
    auto blank = cursor.readPage(2, 0); QVERIFY(blank.isValid()); QCOMPARE(blank.data[0], quint8(0x2a));
    QVERIFY(f.store->sessionStats().activeReadLeases <= 1);
    QVERIFY(cursor.finish()); QCOMPARE(f.store->sessionStats().activeReadLeases, qsizetype(0));
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x71));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::accessorFailedMoveDropsCachedPointer()
{
    Fixture f; QVERIFY(f.init());
    const auto tx = f.store->beginCurrentTransaction();
    KisPageStoreRandomAccessor accessor(f.store.get(), {1}, tx, KisPageWriteMode::PreserveContents);
    QVERIFY(accessor.isValid());
    accessor.moveTo(0, 0); QVERIFY(accessor.rawData()); accessor.rawData()[0] = 0x71;
    for (int y = 0; y < 64; ++y) { accessor.moveTo(1, y); QVERIFY(accessor.rawData()); }
    f.provider->rejectWrite = true;
    accessor.moveTo(64, 0); QVERIFY(!accessor.rawData()); QVERIFY(!accessor.oldRawData());
    QVERIFY(!accessor.error().isEmpty());
    f.provider->rejectWrite = false;
    // Allocation failure poisons the native segment. Going back must neither
    // expose a stale cached pointer nor replay its work on the generic route.
    accessor.moveTo(0, 0); QVERIFY(!accessor.rawData()); QVERIFY(!accessor.oldRawData());
    QVERIFY(!accessor.finish()); QVERIFY(!accessor.rawData());
    const auto stats = f.store->mutationStatistics();
    QCOMPARE(stats.writablePinsAcquired, quint64(1)); QCOMPARE(stats.maximumPinnedPagesPerExecution, quint64(1));
    QCOMPARE(f.store->sessionStats().writeRequestsCreated, quint64(0));
    QVERIFY(f.store->abort(tx)); QCOMPARE(quint8(f.pixel()[0]), quint8(0x2a));
    const auto retry = f.store->beginCurrentTransaction();
    KisPageStoreRandomAccessor next(f.store.get(), {1}, retry, KisPageWriteMode::PreserveContents);
    next.moveTo(0, 0); QVERIFY(next.rawData()); next.rawData()[0] = 0x72;
    QVERIFY(next.finish()); QVERIFY(f.store->commit(retry, f.store->preparedPages(retry)).isValid());
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x72)); QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::surfaceRectPageWorkingSet_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("native"); QTest::addColumn<QRect>("rect");
    for (int bpp : {1, 4, 16}) for (bool native : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(bpp).arg(native)))
            << bpp << native << QRect(-3, -2, 134, 69);
    QTest::newRow("minimum-coordinate") << 4 << true << QRect(std::numeric_limits<int>::min(), 0, 67, 3);
    QTest::newRow("maximum-coordinate") << 4 << true << QRect(std::numeric_limits<int>::max() - 66, 0, 67, 3);
}

void KisPageStoreCpuMutationTest::surfaceRectPageWorkingSet()
{
    QFETCH(int, bpp); QFETCH(bool, native); QFETCH(QRect, rect);
    Fixture f; f.provider->disableNativeMutation = !native; f.provider->rejectNativeBinding = !native;
    QVERIFY(f.init(bpp));
    const int stride = rect.width() * bpp + 13;
    QByteArray input(stride * rect.height(), char(0xee));
    for (int y = 0; y < rect.height(); ++y) for (int x = 0; x < rect.width() * bpp; ++x)
        input[y * stride + x] = char((x * 7 + y * 13) % 251);
    const auto tx = f.store->beginCurrentTransaction();
    QVERIFY2(KisPageStoreCpuSurfaceOps::writeRect(f.store.get(), {1}, tx, rect, input, stride, &f.error), qPrintable(f.error));
    const auto stats = f.store->mutationStatistics();
    const auto page = [](int x) { return x / 64 - (x % 64 < 0 ? 1 : 0); };
    const quint64 pages = quint64(page(rect.right()) - page(rect.left()) + 1) *
                          quint64(page(rect.bottom()) - page(rect.top()) + 1);
    if (native) {
        QCOMPARE(stats.generationsReserved, pages); QCOMPARE(stats.pagesSealed, pages);
        QCOMPARE(stats.writablePinsAcquired, pages); QCOMPARE(stats.writablePinsReleased, pages);
        QCOMPARE(stats.maximumPinnedPagesPerExecution, quint64(1));
        QCOMPARE(f.store->sessionStats().writeRequestsCreated, quint64(0));
    } else QCOMPARE(f.store->sessionStats().writeRequestsCreated, pages);
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QByteArray output; const int outStride = rect.width() * bpp + 17;
    QVERIFY2(KisPageStoreCpuSurfaceOps::readRect(f.store.get(), {1}, {}, rect, &output, outStride, &f.error), qPrintable(f.error));
    for (int y = 0; y < rect.height(); ++y) {
        QCOMPARE(output.mid(y * outStride, rect.width() * bpp), input.mid(y * stride, rect.width() * bpp));
        QCOMPARE(output.mid(y * outStride + rect.width() * bpp, 17), QByteArray(17, char(0)));
    }
    QCOMPARE(f.store->sessionStats().activeReadLeases, qsizetype(0));
    // In-place overlapping region copy must read a complete protected source,
    // not bytes already overwritten by the destination's traversal.
    if (rect.left() == -3) {
        const auto copyTx = f.store->beginCurrentTransaction();
        const QPoint destination(1, 1);
        QVERIFY(KisPageStoreCpuSurfaceOps::copyRect(f.store.get(), {1}, {}, rect,
            {1}, copyTx, destination, &f.error));
        QVERIFY(f.store->commit(copyTx, f.store->preparedPages(copyTx)).isValid());
        QByteArray copied;
        QVERIFY(KisPageStoreCpuSurfaceOps::readRect(f.store.get(), {1}, {}, QRect(destination, rect.size()),
            &copied, outStride, &f.error));
        QCOMPARE(copied, output);
        // Pixels outside both writes still come from the shared default.
        QByteArray untouched;
        QVERIFY(KisPageStoreCpuSurfaceOps::readRect(f.store.get(), {1}, {}, QRect(-4, -3, 1, 1), &untouched, 0, &f.error));
        QCOMPARE(untouched, QByteArray(bpp, char(0x2a)));
    }
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::nativeMetadataWorkIgnoresRetainedHistory()
{
    QFETCH(int, bpp); QFETCH(int, history);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x35));
    auto original = f.store->captureReadView({}); QVERIFY(original.isValid());
    const auto tx = f.store->beginCurrentTransaction(); QVERIFY(tx.isValid());
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay; overlay.transaction = tx.id;
    std::vector<KisCapturedReadView> retained;
    for (int i = 0; i < history; ++i) {
        auto segment = f.begin(tx); auto write = segment.beginWrite(key(0)); QVERIFY(write.isValid());
        std::memset(write.data(), 0x60 + i % 0x30, size_t(bpp)); write = {};
        QVERIFY(segment.seal()); retained.push_back(f.store->captureReadView(overlay)); QVERIFY(retained.back().isValid());
    }
    const auto foregroundMetrics = [&] {
        auto metrics = kisPageStoreMetadataMetrics(*f.store);
        metrics.localTransitionSequences -= metrics.backgroundLocalTransitionSequences;
        metrics.localVersionInputs -= metrics.backgroundLocalVersionInputs;
        metrics.localVersionInstalls -= metrics.backgroundLocalVersionInstalls;
        metrics.mutationBaseVersionInputs -= metrics.backgroundMutationBaseVersionInputs;
        return metrics;
    };
    const auto before = foregroundMetrics();
    const auto mutationBefore = f.store->mutationStatistics();
    auto segment = f.begin(tx);
    auto write = segment.beginWrite(key(0)); QVERIFY(write.isValid());
    std::memset(write.data(), 0xd3, size_t(bpp)); write = {};
    write = segment.beginWrite(key(0)); QVERIFY(write.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(write.data()), bpp), QByteArray(bpp, char(0xd3))); write = {};
    QVERIFY(segment.seal());
    const auto after = foregroundMetrics();
    QCOMPARE(after.localTransitionSequences - before.localTransitionSequences, quint64(2));
    QVERIFY(after.localVersionInputs - before.localVersionInputs <= 4);
    QVERIFY(after.localVersionInstalls - before.localVersionInstalls <= 4);
    QVERIFY(after.mutationBaseVersionInputs - before.mutationBaseVersionInputs <= 5);
    QCOMPARE(after.fullSnapshotExports - after.backgroundSnapshotExports,
             before.fullSnapshotExports - before.backgroundSnapshotExports);
    QCOMPARE(f.store->mutationStatistics().generationsReserved - mutationBefore.generationsReserved, quint64(1));
    qInfo() << "native metadata history" << history << "inputs" << after.localVersionInputs - before.localVersionInputs
            << "installs" << after.localVersionInstalls - before.localVersionInstalls
            << "exact snapshot inputs" << after.mutationBaseVersionInputs - before.mutationBaseVersionInputs;
    auto current = f.store->captureReadView(overlay); QVERIFY(current.isValid());
    const auto check = [&](const KisCapturedReadView &view, quint8 expected) {
        auto read = view.readResidentPage(key(0)); QVERIFY(read.isValid());
        QCOMPARE(QByteArray(static_cast<const char *>(read.data()), bpp), QByteArray(bpp, char(expected)));
        QCOMPARE(QByteArray(static_cast<const char *>(read.data()) + bpp, bpp), QByteArray(bpp, char(0x35)));
    };
    check(original, 0x35); check(current, 0xd3);
    for (int i = 0; i < history; ++i) check(retained[size_t(i)], quint8(0x60 + i % 0x30));
    const auto expectedBase = f.store->preparedPages(tx).proofs.first().authority.version;
    const auto genericBefore = foregroundMetrics();
    const auto request = f.store->acquireWrite(tx, key(), cpu, KisPageWriteMode::PreserveContents, KisPagePriority::Normal);
    QVERIFY2(request.isValid(), qPrintable(request.error)); QCOMPARE(request.baseVersion, expectedBase);
    const auto genericAfter = foregroundMetrics();
    QVERIFY(genericAfter.mutationBaseVersionInputs - genericBefore.mutationBaseVersionInputs <= 2);
    QCOMPARE(genericAfter.fullSnapshotExports - genericAfter.backgroundSnapshotExports,
             genericBefore.fullSnapshotExports - genericBefore.backgroundSnapshotExports);
    auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(lease.cpuData()), bpp), QByteArray(bpp, char(0xd3)));
    f.store->cancel(std::move(lease));
    QVERIFY(f.store->abort(tx));
    check(current, 0xd3);
    for (int i = 0; i < history; ++i) check(retained[size_t(i)], quint8(0x60 + i % 0x30));
    retained.clear(); current = {}; original = {};
    QVERIFY(f.store->waitForRetirementIdle()); QVERIFY(f.store->closeSession());
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::publicationMetadataWorkIgnoresRetainedHistory()
{
    QFETCH(int, bpp); QFETCH(int, history);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x35));
    const auto initial = f.store->captureRetainedEpoch(); QVERIFY(initial.isValid());
    std::vector<KisCapturedReadView> retained;
    for (int i = 0; i < history; ++i) {
        const auto tx = f.store->beginCurrentTransaction();
        auto mutation = f.begin(tx); auto write = mutation.beginWrite(key(0)); QVERIFY(write.isValid());
        std::memset(write.data(), 0x60 + i % 0x30, size_t(bpp)); write = {};
        QVERIFY(mutation.seal()); QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
        retained.push_back(f.store->captureReadView({})); QVERIFY(retained.back().isValid());
    }
    const auto tx = f.store->beginCurrentTransaction();
    auto mutation = f.begin(tx); auto write = mutation.beginWrite(key(0)); QVERIFY(write.isValid());
    std::memset(write.data(), 0xd3, size_t(bpp)); write = {}; QVERIFY(mutation.seal());
    const auto before = kisPageStoreMetadataMetrics(*f.store);
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    const auto committed = kisPageStoreMetadataMetrics(*f.store);
    QCOMPARE(committed.publicationVersionInputs - before.publicationVersionInputs, quint64(2));
    QCOMPARE(committed.publicationVersionInstalls - before.publicationVersionInstalls, quint64(2));
    QCOMPARE(committed.publicationHistoryNodesPrepared - before.publicationHistoryNodesPrepared, quint64(1));
    QCOMPARE(committed.publicationHistoryNodesTransferred - before.publicationHistoryNodesTransferred, quint64(1));
    QCOMPARE(committed.publicationAdditionRecordsPrepared, before.publicationAdditionRecordsPrepared);
    QCOMPARE(committed.publicationAdditionRecordsTransferred, before.publicationAdditionRecordsTransferred);
    QCOMPARE(committed.fullSnapshotExports - committed.backgroundSnapshotExports,
             before.fullSnapshotExports - before.backgroundSnapshotExports);
    auto current = f.store->captureReadView({}); QVERIFY(current.isValid());
    QVERIFY(f.store->restoreRetainedEpoch(initial).isValid());
    const auto restored = kisPageStoreMetadataMetrics(*f.store);
    QCOMPARE(restored.publicationVersionInputs - committed.publicationVersionInputs, quint64(3));
    QCOMPARE(restored.publicationVersionInstalls - committed.publicationVersionInstalls, quint64(2));
    QCOMPARE(restored.publicationHistoryNodesPrepared - committed.publicationHistoryNodesPrepared, quint64(1));
    QCOMPARE(restored.publicationHistoryNodesTransferred - committed.publicationHistoryNodesTransferred, quint64(1));
    QCOMPARE(restored.publicationAdditionRecordsPrepared, committed.publicationAdditionRecordsPrepared);
    QCOMPARE(restored.publicationAdditionRecordsTransferred, committed.publicationAdditionRecordsTransferred);
    QCOMPARE(restored.publicationLookupVersionInputs - committed.publicationLookupVersionInputs, quint64(3));
    QCOMPARE(restored.publicationDirectoryHeaders - committed.publicationDirectoryHeaders, quint64(2));
    QCOMPARE(restored.fullSnapshotExports - restored.backgroundSnapshotExports,
             committed.fullSnapshotExports - committed.backgroundSnapshotExports);
    qInfo() << "publication metadata history" << history << "commit inputs/installs"
            << committed.publicationVersionInputs - before.publicationVersionInputs
            << committed.publicationVersionInstalls - before.publicationVersionInstalls
            << "restore inputs/installs" << restored.publicationVersionInputs - committed.publicationVersionInputs
            << restored.publicationVersionInstalls - committed.publicationVersionInstalls;
    const auto check = [&](const KisCapturedReadView &view, quint8 expected) {
        auto read = view.readResidentPage(key(0)); QVERIFY(read.isValid());
        QCOMPARE(QByteArray(static_cast<const char *>(read.data()), bpp), QByteArray(bpp, char(expected)));
        QCOMPARE(QByteArray(static_cast<const char *>(read.data()) + bpp, bpp), QByteArray(bpp, char(0x35)));
    };
    auto restoredView = f.store->captureReadView({}); check(restoredView, 0x35); check(current, 0xd3);
    for (int i = 0; i < history; ++i) check(retained[size_t(i)], quint8(0x60 + i % 0x30));
    retained.clear(); current = {}; restoredView = {};
    QVERIFY(f.store->releaseSnapshot(initial.token));
    QVERIFY(f.store->waitForRetirementIdle()); QVERIFY(f.store->closeSession());
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::restorationDefaultIsCandidateOnly()
{
    QFETCH(int, bpp); QFETCH(bool, rejectFirst);
    Fixture f; QVERIFY(f.init(bpp));
    const auto initial = f.store->captureRetainedEpoch(); QVERIFY(initial.isValid());
    QVERIFY(f.setDefault(0x55));
    QCOMPARE(f.pixel({}, 3), QByteArray(bpp, char(0x55))); // only NEW default is registered
    auto current = f.store->captureReadView({}); QVERIFY(current.isValid());
    const auto versions = f.store->sessionStats().pageVersions; QCOMPARE(versions, qsizetype(1));
    KisPageTransaction interfering; int attempts = 0;
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase != KisPageStoreDiagnosticPhase::RestorePublishOwnerWait) return;
        ++attempts;
        QCOMPARE(f.store->sessionStats().pageVersions, versions);
        if (rejectFirst && attempts == 1) interfering = f.store->beginCurrentTransaction();
    });
    auto metrics = kisPageStoreMetadataMetrics(*f.store);
    auto publication = f.store->publicationStatistics();
    if (rejectFirst) {
        QVERIFY(!f.store->restoreRetainedEpoch(initial).isValid()); QVERIFY(interfering.isValid());
        QCOMPARE(f.store->sessionStats().pageVersions, versions);
        const auto rejected = kisPageStoreMetadataMetrics(*f.store);
        QCOMPARE(rejected.publicationVersionInstalls, metrics.publicationVersionInstalls);
        QCOMPARE(rejected.publicationAdditionRecordsPrepared - metrics.publicationAdditionRecordsPrepared, quint64(1));
        QCOMPARE(rejected.publicationAdditionRecordsTransferred, metrics.publicationAdditionRecordsTransferred);
        QVERIFY(f.store->abort(interfering));
        metrics = kisPageStoreMetadataMetrics(*f.store);
        publication = f.store->publicationStatistics();
    }
    QVERIFY(f.store->restoreRetainedEpoch(initial).isValid()); QCOMPARE(attempts, rejectFirst ? 2 : 1);
    const auto restored = kisPageStoreMetadataMetrics(*f.store);
    QCOMPARE(restored.publicationVersionInputs - metrics.publicationVersionInputs, quint64(1));
    QCOMPARE(restored.publicationVersionInstalls - metrics.publicationVersionInstalls, quint64(2));
    QCOMPARE(restored.publicationAdditionRecordsPrepared - metrics.publicationAdditionRecordsPrepared, quint64(1));
    QCOMPARE(restored.publicationAdditionRecordsTransferred - metrics.publicationAdditionRecordsTransferred, quint64(1));
    const auto published = f.store->publicationStatistics();
    QCOMPARE(published.descriptorCapacityPreparations - publication.descriptorCapacityPreparations, quint64(1));
    QCOMPARE(published.descriptorInstallations - publication.descriptorInstallations, quint64(1));
    QCOMPARE(restored.fullSnapshotExports - restored.backgroundSnapshotExports,
             metrics.fullSnapshotExports - metrics.backgroundSnapshotExports);
    QCOMPARE(f.store->sessionStats().pageVersions, versions + 1);
    QCOMPARE(f.pixel({}, 3), QByteArray(bpp, char(0x2a))); // descriptor installed before new root is exposed
    auto old = current.readResidentPage(key(3)); QVERIFY(old.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(old.data()), bpp), QByteArray(bpp, char(0x55)));
    old = {}; current = {}; QVERIFY(f.store->releaseSnapshot(initial.token));
    QVERIFY(f.store->waitForRetirementIdle()); QVERIFY(f.store->closeSession());
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::repeatedGuardAndSegment()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    auto before = f.store->captureReadView();
    auto oldGuard = before.tryReadResidentPage(key()); QVERIFY(oldGuard.isValid());
    const auto tx = f.store->beginCurrentTransaction();
    const auto stats = f.store->sessionStats();
    const auto mutationStats = f.store->mutationStatistics();
    const auto payload = f.provider->p->payloadWork();
    auto scope = f.begin(tx); QVERIFY2(scope.isActive(), qPrintable(f.error));
    KisPageVersion version;
    for (int i = 0; i < 16; ++i) {
        auto guard = scope.beginWrite(key(), &f.error); QVERIFY2(guard.isValid(), qPrintable(f.error));
        if (!i) version = guard.version();
        QVERIFY(guard.version() == version);
        static_cast<char *>(guard.data())[i * bpp] = char(0x55);
        QVERIFY(!scope.beginWrite(key()).isValid());
        QVERIFY(!scope.seal()); QVERIFY(!scope.cancel());
    }
    // Pixel pins are returned between guards, but the unpublished writer
    // reservation still excludes foreign read/write/retire.
    const auto binding = f.provider->cpuResidentBinding(f.provider->lastTarget);
    QVERIFY(binding); QVERIFY(!binding->acquireRead(f.provider->lastTarget.allocationIdentity(), false));
    QVERIFY(!binding->acquireWrite(f.provider->lastTarget.allocationIdentity())); QVERIFY(!binding->retire(f.provider->lastTarget.allocationIdentity()));
    QCOMPARE(f.provider->p->payloadWork().nativeDuplicatePages - payload.nativeDuplicatePages, quint64(1));
    QCOMPARE(f.store->sessionStats().writeRequestsCreated, stats.writeRequestsCreated);
    QCOMPARE(f.store->sessionStats().readRequestsCreated, stats.readRequestsCreated);
    QCOMPARE(f.store->mutationStatistics().generationsReserved - mutationStats.generationsReserved, quint64(1));
    QCOMPARE(f.store->mutationStatistics().pagesSealed - mutationStats.pagesSealed, quint64(0));
    QVERIFY(!f.store->commit(tx, f.store->preparedPages(tx)).isValid()); QVERIFY(!f.store->abort(tx));
    QVERIFY2(scope.seal(&f.error), qPrintable(f.error));
    QCOMPARE(f.store->mutationStatistics().pagesSealed - mutationStats.pagesSealed, quint64(1));
    QCOMPARE(f.store->mutationStatistics().guardsReleased - mutationStats.guardsReleased, quint64(16));
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay; overlay.transaction = tx.id;
    QCOMPARE(quint8(f.pixel(overlay)[0]), quint8(0x55));
    QCOMPARE(f.pixel(), QByteArray(bpp, char(0x31)));
    auto second = f.begin(tx); auto next = second.beginWrite(key()); QVERIFY(next.isValid());
    QVERIFY(!(next.version() == version));
    static_cast<char *>(next.data())[0] = char(0x6a); next = {};
    QVERIFY(second.seal()); QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x6a));
    QCOMPARE(static_cast<const quint8 *>(oldGuard.data())[0], quint8(0x31));
    oldGuard = {}; before = {};
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::mutationStorageBudgetRejection()
{
    QFETCH(int, bpp);
    quint64 directoryBytes = 0;
    quint64 transactionBytes = 0, scopeBytes = 0;
    {
        Fixture probe; QVERIFY(probe.init(bpp));
        directoryBytes = probe.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
        const auto tx = probe.store->beginCurrentTransaction();
        transactionBytes = probe.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam - directoryBytes;
        QVERIFY(transactionBytes > 0);
        auto scope = probe.begin(tx);
        QVERIFY(scope.isActive());
        scopeBytes = probe.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam - directoryBytes - transactionBytes;
        QVERIFY(scopeBytes > 0);
    }
    Fixture f;
    KisPageBackingLimits limits; limits.metadataArenaBytes = directoryBytes + transactionBytes + scopeBytes;
    QVERIFY(f.store->configureBackingLimits(limits)); QVERIFY(f.init(bpp));
    const auto tx = f.store->beginCurrentTransaction();
    auto mutation = f.begin(tx); QVERIFY(mutation.isActive());
    QVERIFY(mutation.removePage(key(0), &f.error));
    QVERIFY(!mutation.removePage(key(1), &f.error));
    QVERIFY(!f.error.isEmpty());
    QVERIFY(mutation.cancel());
    QCOMPARE(f.store->sessionStats().activeCpuWritePages, qsizetype(0));
    QCOMPARE(f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
             directoryBytes + transactionBytes + scopeBytes);
    mutation = {};
    QCOMPARE(f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam, directoryBytes + transactionBytes);
    QCOMPARE(f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].reserved.cpuRam,
             quint64(0));
    // The failed second claim cannot prevent a subsequent inline operation.
    auto retry = f.begin(tx); QVERIFY(retry.removePage(key(1), &f.error));
    QVERIFY(retry.cancel()); QVERIFY(f.store->abort(tx));
    retry = {}; QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam, directoryBytes);
    QVERIFY(f.store->closeSession(&f.error));
}

void KisPageStoreCpuMutationTest::executionWarmDuringColdPreparation()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction(); auto scope = f.begin(tx);
    const auto before = f.store->mutationStatistics();
    KisPageVersion versions[2];
    for (int i = 0; i < 2; ++i) {
        auto guard = scope.beginWrite(key(i)); QVERIFY(guard.isValid());
        versions[i] = guard.version();
    }
    QSemaphore warmReady, coldEntered, releaseCold, warmDone;
    std::atomic<bool> coldHeld{false};
    bool warmOK = false, coldOK = false, progressedWhileCold = false;
    f.provider->beforePrepareWrite = [&] {
        coldHeld = true; coldEntered.release();
        releaseCold.acquire(); coldHeld = false;
    };
    std::thread warm([&] {
        auto execution = scope.borrowExecution({1}, {{0, 0}, {1, 0}});
        warmReady.release();
        const bool entered = coldEntered.tryAcquire(1, 5000);
        warmOK = entered && execution.isActive();
        for (int i = 0; i < 1000 && warmOK; ++i) {
            auto guard = execution.beginWrite(key(i % 2));
            warmOK = guard.isValid() && guard.version() == versions[i % 2] && coldHeld;
            if (guard.isValid()) static_cast<quint8 *>(guard.data())[0] = quint8(0x50 + i % 2);
        }
        // A guard can return from another thread while the borrowing worker
        // remains alive. No structural gate or movable directory is consulted.
        auto late = warmOK ? execution.beginWrite(key()) : KisCpuWriteGuard{};
        warmOK = warmOK && late.isValid();
        std::thread returner([guard = std::move(late)]() mutable { guard = {}; });
        returner.join();
        progressedWhileCold = warmOK && coldHeld;
        warmDone.release();
        warmOK = execution.finish() && warmOK; // cold structural return may wait
    });
    const bool ready = warmReady.tryAcquire(1, 5000);
    std::thread cold([&] {
        QSet<KisLogicalPageId> pages;
        for (int i = 2; i <= 65; ++i) pages.insert({i, 0});
        auto execution = scope.borrowExecution({1}, pages);
        auto guard = execution.beginWrite(key(65));
        coldOK = guard.isValid(); guard = {};
        coldOK = execution.finish() && coldOK;
    });
    const bool independent = warmDone.tryAcquire(1, 5000);
    releaseCold.release();
    warm.join(); cold.join();
    f.provider->beforePrepareWrite = {};
    QVERIFY(ready); QVERIFY(independent); QVERIFY(progressedWhileCold); QVERIFY(warmOK); QVERIFY(coldOK);
    QVERIFY(scope.seal());
    const auto after = f.store->mutationStatistics();
    QCOMPARE(after.generationsReserved - before.generationsReserved, quint64(3));
    QCOMPARE(after.writablePinsAcquired - before.writablePinsAcquired, quint64(1004));
    QCOMPARE(after.writablePinsReleased - before.writablePinsReleased, quint64(1004));
    QCOMPARE(after.guardsReleased - before.guardsReleased, quint64(1004));
    QCOMPARE(after.maximumPinnedPagesPerExecution, quint64(1));
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x50));
    QCOMPARE(quint8(f.pixel({}, 1)[0]), quint8(0x51));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::executionLocalCountersAndCancellation()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction(); auto scope = f.begin(tx);
    const auto before = f.store->mutationStatistics();
    auto execution = scope.borrowExecution({1}, {{2, 0}, {0, 0}, {1, 0}, {0, 0}});
    QVERIFY(execution.isActive()); // Unordered duplicate inputs retain set semantics.
    auto first = execution.beginWrite(key()); auto second = execution.beginWrite(key(1));
    QVERIFY(first.isValid()); QVERIFY(second.isValid());
    const auto version = first.version();
    QVERIFY(!execution.beginWrite(key()).isValid());
    first = {};
    first = execution.beginWrite(key()); QVERIFY(first.isValid()); QCOMPARE(first.version(), version);
    // Cancellation races only with the original atomic lifecycle state.
    bool cancelled = true;
    std::thread cancel([&] { cancelled = scope.cancel(); }); cancel.join();
    QVERIFY(!cancelled); QVERIFY(!scope.isActive());
    QVERIFY(!execution.beginWrite(key(2)).isValid());
    QVERIFY(!execution.finish()); // live guards still retain the binding
    first = {}; second = {};
    QVERIFY(!execution.finish()); QVERIFY(scope.cancel()); QVERIFY(f.store->abort(tx));
    const auto after = f.store->mutationStatistics();
    QCOMPARE(after.writablePinsAcquired - before.writablePinsAcquired, quint64(3));
    QCOMPARE(after.writablePinsReleased - before.writablePinsReleased, quint64(3));
    QCOMPARE(after.guardsReleased - before.guardsReleased, quint64(3));
    QCOMPARE(after.maximumPinnedPagesPerExecution, quint64(2));
    QCOMPARE(f.pixel(), QByteArray(bpp, char(0x31)));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::executionBorrowWorkers()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    auto old = f.store->captureReadView();
    const auto tx = f.store->beginCurrentTransaction();
    auto scope = f.begin(tx); QVERIFY(scope.isActive());
    const auto stats = f.store->mutationStatistics();
    KisPageVersion firstVersion;
    QSemaphore firstEntered, secondEntered, firstDone;
    std::atomic<bool> firstHolding{false};
    bool firstOK = false, secondOK = false, simultaneous = false;
    std::thread first([&] {
        QString error;
        auto execution = scope.borrowExecution({1}, {{0, 0}}, &error);
        auto guard = execution.beginWrite(key(), &error);
        const bool valid = guard.isValid();
        if (valid) { firstVersion = guard.version(); firstHolding = true; }
        firstEntered.release();
        const bool other = secondEntered.tryAcquire(1, 5000);
        if (valid && other) std::memset(guard.data(), 0x51, size_t(guard.byteSize()));
        firstHolding = false; guard = {};
        firstOK = valid && other && execution.finish(&error);
        firstDone.release();
    });
    const bool entered = firstEntered.tryAcquire(1, 5000);
    std::thread second([&] {
        QString error; QSet<KisLogicalPageId> pages;
        for (int i = 1; i <= 65; ++i) pages.insert({i, 0});
        auto execution = scope.borrowExecution({1}, pages, &error);
        // New hot/cold blocks and the original index grow while the first
        // worker still owns its exact slot, pin and writable byte address.
        auto guard = execution.beginWrite(key(65), &error);
        simultaneous = guard.isValid() && firstHolding;
        if (guard.isValid()) std::memset(guard.data(), 0x62, size_t(guard.byteSize()));
        secondEntered.release();
        const bool other = firstDone.tryAcquire(1, 5000);
        guard = {};
        secondOK = execution.isActive() && other && execution.finish(&error);
    });
    first.join(); second.join();
    QVERIFY(entered); QVERIFY(firstOK); QVERIFY(secondOK); QVERIFY(simultaneous);
    QCOMPARE(f.store->mutationStatistics().generationsReserved - stats.generationsReserved, quint64(2));
    QCOMPARE(f.store->mutationStatistics().pagesSealed - stats.pagesSealed, quint64(0));
    bool resumed = false;
    std::thread successor([&] {
        auto execution = scope.borrowExecution({1}, {{0, 0}});
        resumed = execution.isActive();
        for (int i = 0; i < 1000 && resumed; ++i) {
            auto guard = execution.beginWrite(key());
            resumed = guard.isValid() && guard.version() == firstVersion;
            if (resumed) static_cast<quint8 *>(guard.data())[0] = 0x73;
        }
        resumed = resumed && execution.finish();
    });
    successor.join(); QVERIFY(resumed);
    QCOMPARE(f.store->mutationStatistics().generationsReserved - stats.generationsReserved, quint64(2));
    QVERIFY(scope.seal(&f.error));
    QCOMPARE(f.store->mutationStatistics().guardsReleased - stats.guardsReleased, quint64(1002));
    QCOMPARE(f.store->mutationStatistics().maximumPinnedPagesPerExecution, quint64(1));
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x73));
    QCOMPARE(f.pixel({}, 65), QByteArray(bpp, char(0x62)));
    auto previous = old.readResidentPage(key()); QVERIFY(previous.isValid());
    QCOMPARE(static_cast<const quint8 *>(previous.data())[0], quint8(0x31));
    previous = {}; old = {}; scope = {};
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::executionBorrowExclusionAndFailure()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction(); auto scope = f.begin(tx);
    auto first = scope.borrowExecution({1}, {{0, 0}}); QVERIFY(first.isActive());
    const auto claims = f.store->sessionStats().activeCpuWritePages;
    QVERIFY(!scope.borrowExecution({99}, {{0, 0}}).isActive());
    QVERIFY(scope.isActive());
    QVERIFY(!scope.borrowExecution({1}, {{0, 0}, {2, 0}}).isActive());
    QCOMPARE(f.store->sessionStats().activeCpuWritePages, claims);
    QVERIFY(!scope.beginWrite(key(2)).isValid()); QVERIFY(!scope.removePage(key(2)));
    QVERIFY(!scope.seal());
    bool wrongWorkerRejected = false;
    std::thread wrong([&] {
        wrongWorkerRejected = !first.beginWrite(key()).isValid() && !first.finish();
    });
    wrong.join(); QVERIFY(wrongWorkerRejected); QVERIFY(first.isActive());
    auto guard = first.beginWrite(key()); QVERIFY(guard.isValid());
    QVERIFY(!first.finish()); QVERIFY(!first.beginWrite(key()).isValid());
    guard = {}; QVERIFY(first.finish()); QVERIFY(!first.isActive());
    auto other = f.begin(tx); QVERIFY(other.removePage(key(3)));
    QVERIFY(!scope.borrowExecution({1}, {{1, 0}, {3, 0}}).isActive());
    QVERIFY(scope.isActive());
    QCOMPARE(f.store->sessionStats().activeCpuWritePages, claims + 1);
    QVERIFY(other.cancel());
    auto retry = scope.borrowExecution({1}, {{1, 0}, {3, 0}}); QVERIFY(retry.isActive());
    auto sibling = scope.borrowExecution({1}, {{0, 0}}); QVERIFY(sibling.isActive());
    retry = {}; // abandoned execution irrevocably fails the original scope
    QVERIFY(!scope.isActive()); QVERIFY(!sibling.beginWrite(key()).isValid());
    QVERIFY(!sibling.finish()); QVERIFY(!scope.seal()); QVERIFY(scope.cancel());
    QVERIFY(f.store->abort(tx)); QCOMPARE(f.pixel(), QByteArray(bpp, char(0x31)));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::executionBorrowStorageRejection()
{
    QFETCH(int, bpp);
    quint64 base = 0, scopeBytes = 0, executionBytes = 0;
    {
        Fixture p; QVERIFY(p.init(bpp));
        auto usage = [&] { return p.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
        base = usage(); const auto tx = p.store->beginCurrentTransaction(); auto scope = p.begin(tx);
        scopeBytes = usage() - base;
        auto execution = scope.borrowExecution({1}, {{0, 0}}); QVERIFY(execution.isActive());
        executionBytes = usage() - base - scopeBytes; QVERIFY(executionBytes > 0);
        QVERIFY(execution.finish()); QCOMPARE(usage(), base + scopeBytes);
        QVERIFY(scope.cancel()); scope = {}; QVERIFY(p.store->abort(tx)); QVERIFY(p.store->closeSession());
    }
    for (bool admitRecord : {false, true}) {
        Fixture f; KisPageBackingLimits limits;
        limits.metadataArenaBytes = base + scopeBytes + (admitRecord ? executionBytes : 0);
        QVERIFY(f.store->configureBackingLimits(limits)); QVERIFY(f.init(bpp));
        const auto tx = f.store->beginCurrentTransaction(); auto scope = f.begin(tx); QVERIFY(scope.isActive());
        if (admitRecord) {
            auto first = scope.borrowExecution({1}, {{0, 0}}); QVERIFY(first.isActive()); QVERIFY(first.finish());
        }
        QSet<KisLogicalPageId> pages;
        for (int i = 1; i <= 65; ++i) pages.insert({i, 0});
        auto rejected = scope.borrowExecution({1}, pages, &f.error);
        QVERIFY(!rejected.isActive()); QVERIFY(f.error.contains(QStringLiteral("storage")));
        QVERIFY(scope.isActive()); QCOMPARE(f.store->sessionStats().activeCpuWritePages, qsizetype(admitRecord ? 1 : 0));
        QCOMPARE(f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam, base + scopeBytes);
        if (admitRecord) {
            auto retry = scope.borrowExecution({1}, {{0, 0}}); QVERIFY(retry.isActive()); QVERIFY(retry.finish());
        }
        QVERIFY(scope.cancel()); scope = {}; QVERIFY(f.store->abort(tx)); QVERIFY(f.store->closeSession());
    }
}

void KisPageStoreCpuMutationTest::executionBorrowLateGuard()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const auto committedBefore = f.provider->memoryUsage().committedBytes;
    auto old = f.store->captureReadView();
    const auto tx = f.store->beginCurrentTransaction(); auto scope = f.begin(tx);
    auto execution = scope.borrowExecution({1}, {{0, 0}}); QVERIFY(execution.isActive());
    auto guard = execution.beginWrite(key()); QVERIFY(guard.isValid());
    std::memset(guard.data(), 0x55, size_t(guard.byteSize()));
    QVERIFY(!scope.cancel()); // records cancellation, does not discard the live borrow
    QVERIFY(!scope.isActive()); QVERIFY(!scope.borrowExecution({1}, {{1, 0}}).isActive());
    execution = {}; scope = {};
    QVERIFY(!f.store->closeSession());
    f.store.reset();
    // Aliasing guard ownership retains the original borrow, scope and owner.
    QVERIFY(guard.isValid()); QCOMPARE(static_cast<const quint8 *>(guard.data())[0], quint8(0x55));
    std::thread release([guard = std::move(guard)]() mutable { guard = {}; });
    release.join();
    auto previous = old.readResidentPage(key()); QVERIFY(previous.isValid());
    QCOMPARE(static_cast<const quint8 *>(previous.data())[0], quint8(0x31));
    previous = {}; old = {};
    kisDrainPageStoreReclamation();
    // The externally retained provider keeps the committed allocations; the
    // abandoned private target must retire, just as in facadeLifetime().
    QCOMPARE(f.provider->memoryUsage().committedBytes, committedBefore);
}

void KisPageStoreCpuMutationTest::mutationScopeAdmissionBudgetRejection()
{
    QFETCH(int, bpp);
    quint64 baseBytes = 0, transactionBytes = 0, scopeBytes = 0;
    {
        Fixture probe; QVERIFY(probe.init(bpp));
        baseBytes = probe.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
        const auto tx = probe.store->beginCurrentTransaction();
        transactionBytes = probe.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam - baseBytes;
        QVERIFY(transactionBytes > 0);
        auto scope = probe.begin(tx); QVERIFY(scope.isActive());
        scopeBytes = probe.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam - baseBytes - transactionBytes;
        QVERIFY(scopeBytes > 0);
        QVERIFY(scope.cancel()); scope = {};
        QCOMPARE(probe.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam, baseBytes + transactionBytes);
        QVERIFY(probe.store->abort(tx)); QVERIFY(probe.store->closeSession());
    }
    for (int availableScopes : {0, 5}) {
        Fixture f;
        KisPageBackingLimits limits;
        limits.metadataArenaBytes = baseBytes + (availableScopes
            ? quint64(availableScopes) * (transactionBytes + scopeBytes) : transactionBytes);
        QVERIFY(f.store->configureBackingLimits(limits)); QVERIFY(f.init(bpp));
        std::vector<KisPageTransaction> transactions;
        std::vector<KisPageMutationSession> scopes;
        const int admitted = availableScopes ? 4 : 0;
        for (int i = 0; i < admitted; ++i) {
            transactions.push_back(f.store->beginCurrentTransaction());
            scopes.push_back(f.begin(transactions.back()));
            QVERIFY(scopes.back().isActive());
        }
        const auto rejectedTransaction = f.store->beginCurrentTransaction();
        QVERIFY(rejectedTransaction.isValid());
        auto rejected = f.store->beginMutation(rejectedTransaction, &f.error);
        QVERIFY(!rejected.isActive()); QVERIFY(!f.error.isEmpty());
        // With room for five scope/control blocks, the fifth transaction is
        // rejected by actual activity-table growth, after inert scope prepare.
        if (availableScopes) QVERIFY(f.error.contains(QStringLiteral("admission")));
        QCOMPARE(f.store->mutationStatistics().operationSessionsCreated, quint64(admitted));
        QCOMPARE(f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
                 baseBytes + quint64(admitted) * (transactionBytes + scopeBytes) + transactionBytes);
        QVERIFY(f.store->abort(rejectedTransaction)); // no phantom activity
        for (auto &scope : scopes) QVERIFY(scope.cancel());
        scopes.clear();
        for (const auto &transaction : transactions) QVERIFY(f.store->abort(transaction));
        QVERIFY(f.store->waitForRetirementIdle());
        QCOMPARE(f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam, baseBytes);
        QCOMPARE(f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].reserved.cpuRam, quint64(0));
        QVERIFY(f.store->closeSession(&f.error));
    }
}

void KisPageStoreCpuMutationTest::abandonedMutationsRetainOriginalTerminalLinks()
{
    QFETCH(int, bpp);
    Fixture f; KisPageBackingLimits limits; limits.retirementDebtBytes = 64 * 64 * bpp;
    QVERIFY(f.store->configureBackingLimits(limits)); QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const auto first = f.store->beginCurrentTransaction();
    const auto second = f.store->beginCurrentTransaction();
    auto a = f.begin(first); auto b = f.begin(second);
    for (int i = 0; i < 4; ++i) {
        auto guard = (i < 2 ? a : b).beginWrite(key(i)); QVERIFY(guard.isValid());
        static_cast<char *>(guard.data())[0] = char(0x71 + i);
    }
    f.provider->rejectRetire = true;
    a = {}; b = {}; // both original scopes must survive their last shared owner
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().activeCpuWritePages, qsizetype(3));
    QVERIFY(!f.store->abort(first)); QVERIFY(!f.store->abort(second));
    QCOMPARE(f.store->sessionStats().activeCpuWritePages, qsizetype(3));
    QVERIFY(!f.store->closeSession(&f.error));
    f.provider->rejectRetire = false;
    bool firstDone = false, secondDone = false;
    for (int i = 0; i < 8 && !(firstDone && secondDone); ++i) {
        f.store->processRetirements(64); QVERIFY(f.store->waitForRetirementIdle());
        if (!firstDone) firstDone = f.store->abort(first);
        if (!secondDone) secondDone = f.store->abort(second);
    }
    QVERIFY(firstDone); QVERIFY(secondDone);
    QCOMPARE(f.store->sessionStats().activeCpuWritePages, qsizetype(0));
    QCOMPARE(f.store->mutationStatistics().pagesCancelled, quint64(4));
    QCOMPARE(f.pixel(), QByteArray(bpp, char(0x31)));
    QVERIFY(f.store->closeSession(&f.error));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::mutationColdHolesAreReusedDuringSeal()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    auto before = f.store->captureReadView();
    auto read = before.readResidentPage(key()); QVERIFY(read.isValid());
    KisSurfaceEpochState surface; QVERIFY(before.resolveSurfaceState({1}, &surface));
    auto source = f.provider->p->captureCpuReadSource(read, surface.allocationDescriptor());
    QVERIFY(source);
    const auto tx = f.store->beginCurrentTransaction();
    auto segment = f.begin(tx);
    for (int i = 1; i <= 17; ++i) {
        auto guard = segment.beginWrite(key(i)); QVERIFY(guard.isValid());
        std::memset(guard.data(), 0x40 + i, size_t(guard.byteSize()));
    }
    const auto removed = [](int i) { return i <= 9 || i == 13 || i == 17; };
    for (int i = 1; i <= 17; ++i)
        if (removed(i)) QVERIFY(segment.removePage(key(i)));
    for (int i = 100; i < 112; ++i) QVERIFY(segment.aliasPage(key(i), source));
    const auto chargedBeforeSeal = f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    bool observed = false, returnedBeforeAlias = false;
    f.provider->beforeAdopt = [&] {
        if (!observed) {
            observed = true;
            returnedBeforeAlias = f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam < chargedBeforeSeal;
        }
    };
    // seal discards scattered cold slots first, then adopts aliases into those
    // holes. Neither live count nor the last allocated index identifies them.
    QVERIFY2(segment.seal(&f.error), qPrintable(f.error));
    f.provider->beforeAdopt = {};
    QVERIFY(observed && returnedBeforeAlias);
    const auto overlay = KisPageReadView::transactionOverlay(tx.id);
    for (int i = 1; i <= 17; ++i)
        QCOMPARE(f.pixel(overlay, i), QByteArray(bpp, char(removed(i) ? 0x2a : 0x40 + i)));
    for (int i = 100; i < 112; ++i)
        QCOMPARE(f.pixel(overlay, i), QByteArray(bpp, char(0x31)));
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QCOMPARE(static_cast<const quint8 *>(read.data())[0], quint8(0x31));
    source.reset(); read = {}; before = {};
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
}

void KisPageStoreCpuMutationTest::mutationStorageGrowthKeepsGuards()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp));
    const auto tx = f.store->beginCurrentTransaction();
    auto mutation = f.begin(tx);
    // Keep both inline and overflow cold records exposed while both stable
    // directories and the key index grow. Release after the facade is gone.
    auto first = mutation.beginWrite(key(0)); QVERIFY(first.isValid());
    auto second = mutation.beginWrite(key(1)); QVERIFY(second.isValid());
    void *const firstData = first.data(); void *const secondData = second.data();
    const auto firstVersion = first.version(); const auto secondVersion = second.version();
    for (int i = 2; i < 65; ++i) {
        auto next = mutation.beginWrite(key(i), &f.error); QVERIFY2(next.isValid(), qPrintable(f.error));
        std::memset(next.data(), i, size_t(next.byteSize()));
    }
    QCOMPARE(first.data(), firstData); QCOMPARE(second.data(), secondData);
    QVERIFY(first.version() == firstVersion); QVERIFY(second.version() == secondVersion);
    std::memset(first.data(), 0x41, size_t(first.byteSize()));
    std::memset(second.data(), 0x42, size_t(second.byteSize()));
    QVERIFY(!mutation.seal()); QVERIFY(!mutation.cancel());
    mutation = {}; // The guards keep the original owner and charged storage alive.
    first = {};
    QCOMPARE(static_cast<const quint8 *>(second.data())[0], quint8(0x42));
    second = {};
    QVERIFY(f.store->abort(tx));
    QVERIFY(f.store->closeSession(&f.error));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
    QCOMPARE(f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].reserved.cpuRam,
             quint64(0));
}

void KisPageStoreCpuMutationTest::cancelAndClaims()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    const auto payload = f.provider->p->payloadWork();
    const auto mutationStats = f.store->mutationStatistics();
    { auto intent = f.begin(tx); QVERIFY(intent.isActive()); QVERIFY(intent.seal()); }
    QCOMPARE(f.provider->p->payloadWork().nativeDuplicatePages, payload.nativeDuplicatePages);
    QCOMPARE(f.store->mutationStatistics().generationsReserved, mutationStats.generationsReserved);
    auto first = f.begin(tx); auto guard = first.beginWrite(key()); QVERIFY(guard.isValid());
    static_cast<char *>(guard.data())[0] = char(0x55); guard = {};
    QVERIFY(!f.store->acquireWrite(tx, key(), cpu, KisPageWriteMode::PreserveContents, KisPagePriority::Normal).isValid());
    QVERIFY(!f.remove(tx, key()));
    KisSurfaceEpochState surface; QVERIFY(f.store->resolveSurfaceState({1}, {}, &surface));
    QVERIFY(!f.store->stageSurfaceMetadata(tx, surface));
    auto competing = f.begin(tx); QVERIFY(!competing.beginWrite(key()).isValid()); QVERIFY(competing.cancel());
    auto separate = f.begin(tx); auto other = separate.beginWrite(key(1)); QVERIFY(other.isValid());
    other = {}; QVERIFY(separate.seal());
    QVERIFY(!f.store->closeSession());
    QVERIFY(first.cancel()); QCOMPARE(f.pixel(), QByteArray(4, char(0x31)));
    QVERIFY(f.store->abort(tx)); QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::facadeLifetime()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto committedBefore = f.provider->memoryUsage().committedBytes;
    const auto tx = f.store->beginCurrentTransaction();
    KisCpuWriteGuard guard;
    { auto scope = f.begin(tx); guard = scope.beginWrite(key()); QVERIFY(guard.isValid()); }
    QVERIFY(!f.store->abort(tx));
    static_cast<char *>(guard.data())[0] = char(0x55);
    f.store.reset(); // retained private owner/provider must outlive the pointer
    QCOMPARE(static_cast<const quint8 *>(guard.data())[0], quint8(0x55));
    guard = {};
    // This externally held provider still owns the committed allocations;
    // only the abandoned pending target is retired here. closeSession() is
    // the explicit whole-owner drain in the other tests.
    kisDrainPageStoreReclamation();
    QCOMPARE(f.provider->memoryUsage().committedBytes, committedBefore);
}

void KisPageStoreCpuMutationTest::defaultAndRemoval()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp));
    auto tx = f.store->beginCurrentTransaction();
    KisSurfaceEpochState surface; QVERIFY(f.store->resolveSurfaceState({1}, {}, &surface));
    surface.format.defaultPixel.fill(char(0x4c)); ++surface.defaultPixelRevision;
    QVERIFY(f.store->stageSurfaceMetadata(tx, surface));
    auto fresh = f.begin(tx); auto guard = fresh.beginWrite(key(), &f.error);
    QVERIFY2(guard.isValid(), qPrintable(f.error));
    QCOMPARE(QByteArray(static_cast<const char *>(guard.data()), bpp), QByteArray(bpp, char(0x4c)));
    static_cast<char *>(guard.data())[0] = char(0x55); guard = {};
    QVERIFY(fresh.seal()); QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    tx = f.store->beginCurrentTransaction();
    QVERIFY(f.remove(tx, key()));
    auto cancelled = f.begin(tx); guard = cancelled.beginWrite(key()); QVERIFY(guard.isValid());
    guard = {}; QVERIFY(cancelled.cancel());
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay; overlay.transaction = tx.id;
    QCOMPARE(f.pixel(overlay), QByteArray(bpp, char(0x4c)));
    auto revive = f.begin(tx); guard = revive.beginWrite(key()); QVERIFY(guard.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(guard.data()), bpp), QByteArray(bpp, char(0x4c)));
    static_cast<char *>(guard.data())[0] = char(0x6a); guard = {};
    QVERIFY(revive.seal()); QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x6a));
    QCOMPARE(f.pixel({}, 0, bpp), QByteArray(bpp, char(0x4c)));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::metadataPresenceUsesCanonicalSelector()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const QVector<KisLogicalPageId> pages{{0, 0}, {1, 0}, {2, 0}};
    QVector<quint8> present;
    int bindings = 0;
    f.provider->beforeBinding = [&](const KisReplicaHandle &) { ++bindings; };
    const auto captures = f.store->readScopeStatistics().capturedReadViewsCreated;
    const auto requests = f.store->sessionStats().readRequestsCreated;
    const auto query = [&](const KisPageReadView &view, const QVector<quint8> &expected) {
        const QVector<quint8> oldValues(present.cbegin(), present.cend());
        const auto sharedPrevious = present;
        QVERIFY(f.store->resolvePagePresence({1}, pages, view, &present));
        QCOMPARE(present, expected);
        QCOMPARE(sharedPrevious, oldValues);
        QCOMPARE(f.store->readScopeStatistics().capturedReadViewsCreated, captures);
        QCOMPARE(f.store->sessionStats().readRequestsCreated, requests);
    };
    query({}, {1, 1, 0});
    const auto tx = f.store->beginCurrentTransaction();
    const auto overlay = KisPageReadView::transactionOverlay(tx.id);
    KisPageReadView base;
    base.kind = KisPageReadViewKind::TransactionBaseEpoch; base.transaction = tx.id;
    QVERIFY(f.remove(tx, key(0)));
    auto scope = f.begin(tx); auto guard = scope.beginWrite(key(2));
    QVERIFY(guard.isValid());
    static_cast<quint8 *>(guard.data())[0] = 0x71;
    const int bindingsAfterWrite = bindings;
    query(overlay, {0, 1, 0}); // unsealed pending never becomes a readable page
    query(base, {1, 1, 0});
    query({}, {1, 1, 0});
    QCOMPARE(bindings, bindingsAfterWrite);
    guard = {}; QVERIFY(scope.seal());
    const int bindingsAfterSeal = bindings;
    query(overlay, {0, 1, 1});
    query(base, {1, 1, 0});
    query({}, {1, 1, 0});
    QCOMPARE(bindings, bindingsAfterSeal);
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QVERIFY(f.setDefault(0x4c));
    const int bindingsAfterCommit = bindings;
    query({}, {0, 1, 1}); // an absent page stays absent after changing default
    QCOMPARE(bindings, bindingsAfterCommit);

    const QVector<quint8> sentinel{9, 8};
    present = sentinel;
    QVERIFY(!f.store->resolvePagePresence({1}, pages, overlay, &present));
    QCOMPARE(present, sentinel); // ended selector cannot partially replace output
    QVERIFY(!f.store->resolvePagePresence({1}, {}, overlay, &present));
    QCOMPARE(present, sentinel); // empty batches still validate the selector
    QVERIFY(!f.store->resolvePagePresence({999}, pages, {}, &present));
    QCOMPARE(present, sentinel);
    QVERIFY(!f.store->resolvePagePresence({1}, pages, {}, nullptr));
    QVERIFY(f.store->resolvePagePresence({1}, {}, {}, &present));
    QVERIFY(present.isEmpty());
    query({}, {0, 1, 1});
    QVERIFY(f.store->closeSession()); // saved classifications retain no view/pin
    QCOMPARE(present, QVector<quint8>({0, 1, 1}));
}

void KisPageStoreCpuMutationTest::checkpointRecyclesStorageAndSemanticPages()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction(); auto scope = f.begin(tx);
    const auto arena = [&] { return f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const auto emptyBytes = arena();
    QSet<KisLogicalPageId> pages;
    for (int x = 0; x < 65; ++x) pages.insert({x, 0});
    // Isolate declared-range storage from the new metadata protection needed
    // by a frozen pixel view: an unwritten range contributes no sealed pages.
    auto declared = scope.borrowExecution({1}, pages); QVERIFY(declared.isActive());
    QVERIFY(declared.finish());
    const auto declaredBytes = arena(); QVERIFY(declaredBytes > emptyBytes);
    auto emptyCut = scope.checkpointForRead(); QVERIFY(emptyCut.isValid());
    QVERIFY(arena() < declaredBytes); emptyCut = {};
    KisCapturedReadView latest;
    for (int cycle = 0; cycle < 3; ++cycle) {
        auto execution = scope.borrowExecution({1}, pages); QVERIFY(execution.isActive());
        for (int x = 0; x < 65; ++x) {
            auto guard = execution.beginWrite(key(x)); QVERIFY(guard.isValid());
            std::memset(guard.data(), 0x51 + cycle, size_t(bpp));
        }
        QVERIFY(execution.finish());
        const auto pendingBytes = arena(); QVERIFY(pendingBytes > emptyBytes);
        auto checkpoint = scope.checkpointForRead(); QVERIFY(checkpoint.isValid());
        // A frozen pixel view adds real metadata protection in this same
        // budget class, so total arena bytes need not decrease at this cut.
        // The declared-only check above isolates actual range storage return.
        if (latest.isValid()) {
            auto previous = latest.readResidentPage(key(64)); QVERIFY(previous.isValid());
            QCOMPARE(static_cast<const quint8 *>(previous.data())[0], quint8(0x50 + cycle));
        }
        latest = std::move(checkpoint);
        QVERIFY(f.store->waitForRetirementIdle());
    }
    QVERIFY(scope.removePage(key()));
    auto absent = scope.checkpointForRead(); QVERIFY(absent.isValid());
    KisPageVersion removed; QVERIFY(absent.resolvePageVersion(key(), &removed)); QVERIFY(removed.isDefaultPixel());
    auto guard = scope.beginWrite(key()); QVERIFY(guard.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(guard.data()), bpp), QByteArray(bpp, char(0x2a)));
    std::memset(guard.data(), 0x73, size_t(bpp)); guard = {};
    auto revived = scope.checkpointForRead(); QVERIFY(revived.isValid());
    QVERIFY(scope.cancel()); QVERIFY(f.store->abort(tx));
    for (auto *view : {&latest, &absent, &revived}) {
        auto read = view->readResidentPage(key()); QVERIFY(read.isValid());
        const quint8 expected = view == &latest ? 0x53 : view == &absent ? 0x2a : 0x73;
        QCOMPARE(QByteArray(static_cast<const char *>(read.data()), bpp), QByteArray(bpp, char(expected)));
    }
    latest = {}; absent = {}; revived = {}; scope = {};
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::checkpointKeepsOriginalScope()
{
    QFETCH(int, bpp); QFETCH(int, repeats); QFETCH(bool, abort);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const auto baseline = f.store->mutationStatistics();
    const auto tx = f.store->beginCurrentTransaction(); auto scope = f.begin(tx);
    const QSet<KisLogicalPageId> pages{{0, 0}, {2, 0}, {65, 0}};
    bool wrote = true; KisPageVersion firstVersion;
    std::thread firstWorker([&] {
        auto execution = scope.borrowExecution({1}, pages);
        wrote = execution.isActive();
        for (int i = 0; wrote && i < repeats; ++i) for (int x : {0, 2, 65}) {
            auto guard = execution.beginWrite(key(x));
            if (!guard.isValid()) { wrote = false; break; }
            if (x == 0) {
                if (!i) firstVersion = guard.version();
                else wrote &= guard.version() == firstVersion;
            }
            std::memset(guard.data(), 0x51 + x % 8, size_t(bpp));
        }
        wrote &= execution.finish();
    });
    firstWorker.join(); QVERIFY(wrote);
    KisCapturedReadView first;
    std::thread checkpointWorker([&] { first = scope.checkpointForRead(&f.error); });
    checkpointWorker.join(); QVERIFY2(first.isValid(), qPrintable(f.error));
    QVERIFY(scope.isActive()); QVERIFY(first.isTransactionOverlay()); QCOMPARE(first.epoch(), tx.baseEpoch);
    // The same original activity keeps the transaction uncommittable while
    // the business scope is open, even though this segment has no page claim.
    QVERIFY(!f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QVERIFY(!f.store->abort(tx));
    for (int x : {0, 2, 65}) {
        auto read = first.readResidentPage(key(x)); QVERIFY(read.isValid());
        QCOMPARE(QByteArray(static_cast<const char *>(read.data()), bpp), QByteArray(bpp, char(0x51 + x % 8)));
    }
    KisPageVersion secondVersion;
    std::thread secondWorker([&] {
        auto execution = scope.borrowExecution({1}, {{0, 0}});
        wrote = execution.isActive();
        for (int i = 0; wrote && i < repeats; ++i) {
            auto guard = execution.beginWrite(key());
            if (!guard.isValid()) { wrote = false; break; }
            if (!i) {
                secondVersion = guard.version();
                wrote &= static_cast<const quint8 *>(guard.data())[0] == 0x51;
            } else wrote &= guard.version() == secondVersion;
            std::memset(guard.data(), 0x71, size_t(bpp));
        }
        wrote &= execution.finish();
    });
    secondWorker.join(); QVERIFY(wrote); QVERIFY(!(firstVersion == secondVersion));
    auto second = scope.checkpointForRead(); QVERIFY(second.isValid());
    auto empty = scope.checkpointForRead(); QVERIFY(empty.isValid());
    KisPageVersion untouchedFirst, untouchedSecond;
    QVERIFY(first.resolvePageVersion(key(65), &untouchedFirst));
    QVERIFY(second.resolvePageVersion(key(65), &untouchedSecond)); QCOMPARE(untouchedFirst, untouchedSecond);
    const auto stats = f.store->mutationStatistics();
    QCOMPARE(stats.operationSessionsCreated - baseline.operationSessionsCreated, quint64(1));
    QCOMPARE(stats.generationsReserved - baseline.generationsReserved, quint64(4));
    QCOMPARE(stats.pagesSealed - baseline.pagesSealed, quint64(4));
    QCOMPARE(stats.writablePinsAcquired - baseline.writablePinsAcquired, quint64(repeats * 4));
    QCOMPARE(stats.writablePinsReleased - baseline.writablePinsReleased, quint64(repeats * 4));
    if (abort) { QVERIFY(scope.cancel()); QVERIFY(f.store->abort(tx)); }
    else { QVERIFY(scope.seal()); QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid()); }
    QCOMPARE(f.store->mutationStatistics().writablePinsAcquired - baseline.writablePinsAcquired, quint64(repeats * 4));
    QCOMPARE(f.store->mutationStatistics().writablePinsReleased - baseline.writablePinsReleased, quint64(repeats * 4));
    for (auto *view : {&first, &second, &empty}) {
        auto read = view->readResidentPage(key()); QVERIFY(read.isValid());
        QCOMPARE(QByteArray(static_cast<const char *>(read.data()), bpp), QByteArray(bpp, char(view == &first ? 0x51 : 0x71)));
    }
    QCOMPARE(f.pixel(), QByteArray(bpp, char(abort ? 0x31 : 0x71)));
    QCOMPARE(f.pixel({}, 65), QByteArray(bpp, char(abort ? 0x2a : 0x52)));
    first = {}; second = {}; empty = {}; scope = {};
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::checkpointRejectsLiveBorrow()
{
    QFETCH(int, bpp); QFETCH(int, borrow);
    Fixture f; QVERIFY(f.init(bpp));
    const auto tx = f.store->beginCurrentTransaction(); auto scope = f.begin(tx);
    auto execution = borrow ? scope.borrowExecution({1}, {{0, 0}}) : KisPageMutationExecution{};
    auto guard = borrow == 1 ? KisCpuWriteGuard{} : borrow ? execution.beginWrite(key()) : scope.beginWrite(key());
    if (borrow != 1) QVERIFY(guard.isValid());
    const auto captures = f.store->readScopeStatistics().capturedReadViewsCreated;
    const auto sealed = f.store->mutationStatistics().pagesSealed;
    QVERIFY(!scope.checkpointForRead().isValid()); QVERIFY(scope.isActive());
    QCOMPARE(f.store->readScopeStatistics().capturedReadViewsCreated, captures);
    QCOMPARE(f.store->mutationStatistics().pagesSealed, sealed);
    if (borrow == 1) guard = execution.beginWrite(key());
    QVERIFY(guard.isValid()); std::memset(guard.data(), 0x67, size_t(bpp)); guard = {};
    if (borrow) QVERIFY(execution.finish());
    auto frozen = scope.checkpointForRead(); QVERIFY(frozen.isValid());
    QVERIFY(scope.cancel()); QVERIFY(f.store->abort(tx));
    auto old = frozen.readResidentPage(key()); QVERIFY(old.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(old.data()), bpp), QByteArray(bpp, char(0x67)));
    old = {}; frozen = {}; QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::checkpointFailurePreservesFrozenViews()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction(); auto scope = f.begin(tx);
    auto guard = scope.beginWrite(key()); QVERIFY(guard.isValid());
    std::memset(guard.data(), 0x51, size_t(bpp)); guard = {};
    auto frozen = scope.checkpointForRead(); QVERIFY(frozen.isValid());
    for (int x : {0, 2}) {
        guard = scope.beginWrite(key(x)); QVERIFY(guard.isValid());
        std::memset(guard.data(), 0x71, size_t(bpp)); guard = {};
    }
    f.provider->validationsUntilFailure = 1;
    QVERIFY(!scope.checkpointForRead(&f.error).isValid()); QVERIFY(!f.error.isEmpty());
    QVERIFY(!scope.isActive());
    f.provider->validationsUntilFailure = -1;
    QCOMPARE(f.pixel(KisPageReadView::transactionOverlay(tx.id)), QByteArray(bpp, char(0x51)));
    QCOMPARE(f.pixel(KisPageReadView::transactionOverlay(tx.id), 2), QByteArray(bpp, char(0x2a)));
    QVERIFY(scope.cancel()); QVERIFY(f.store->abort(tx));
    auto old = frozen.readResidentPage(key()); QVERIFY(old.isValid());
    QCOMPARE(QByteArray(static_cast<const char *>(old.data()), bpp), QByteArray(bpp, char(0x51)));
    QCOMPARE(f.pixel(), QByteArray(bpp, char(0x31)));
    old = {}; frozen = {}; QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::checkpointCaptureRefusalKeepsAcceptedSegment()
{
    KisPageBackingLimits limits; limits.metadataArenaBytes = 4 * 1024 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    Fixture f;
    QVERIFY(f.store->configureSharedNonPayloadBudget(parent, &f.error));
    QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction(); auto scope = f.begin(tx);
    auto guard = scope.beginWrite(key()); QVERIFY(guard.isValid());
    static_cast<quint8 *>(guard.data())[0] = 0x71; guard = {};
    auto accepted = scope.checkpointForRead(); QVERIFY(accepted.isValid());
    const auto sealed = f.store->mutationStatistics().pagesSealed;
    int providerPreparations = 0;
    const auto observePreparation = [&] { ++providerPreparations; };
    f.provider->beforePrepareWrite = observePreparation;
    f.provider->beforeCpuPayload = observePreparation;
    f.provider->beforeAdopt = observePreparation;
    f.provider->beforeTransfer = observePreparation;
    const auto live = parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - live);
    void *filler = kisAllocateMutationStorage(parent.get(), fillerBytes, 1);
    {
        const auto release = qScopeGuard([&] { kisFreeMutationStorage(parent.get(), filler, fillerBytes, 1); });
        for (int attempt = 0; attempt < 3; ++attempt) {
            QVERIFY(!scope.checkpointForRead(&f.error).isValid());
            QVERIFY(!f.error.isEmpty()); QVERIFY(scope.isActive());
            QCOMPARE(f.store->sessionStats().activeProviderCalls, qsizetype(0));
            QCOMPARE(f.store->mutationStatistics().pagesSealed, sealed);
        }
    }
    auto retry = scope.checkpointForRead(&f.error); QVERIFY2(retry.isValid(), qPrintable(f.error));
    QCOMPARE(f.store->mutationStatistics().pagesSealed, sealed);
    QCOMPARE(providerPreparations, 0);
    auto read = retry.readResidentPage(key()); QVERIFY(read.isValid());
    QCOMPARE(static_cast<const quint8 *>(read.data())[0], quint8(0x71));
    read = {}; retry = {}; QVERIFY(scope.seal());
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    read = accepted.readResidentPage(key()); QVERIFY(read.isValid());
    QCOMPARE(static_cast<const quint8 *>(read.data())[0], quint8(0x71));
    read = {}; accepted = {}; QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::checkpointDoesNotWaitForColdPreparation()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp));
    const auto tx = f.store->beginCurrentTransaction(); auto scope = f.begin(tx);
    QSemaphore preparing, resume, returned;
    f.provider->beforePrepareWrite = [&] { preparing.release(); resume.acquire(); };
    bool wrote = false;
    std::thread worker([&] {
        auto execution = scope.borrowExecution({1}, {{0, 0}});
        auto guard = execution.beginWrite(key());
        wrote = guard.isValid(); guard = {}; wrote &= execution.finish();
    });
    const bool entered = preparing.tryAcquire(1, 5000);
    bool refused = false;
    std::thread checkpoint([&] { refused = !scope.checkpointForRead().isValid(); returned.release(); });
    const bool nonblocking = returned.tryAcquire(1, 2000);
    resume.release(); worker.join(); checkpoint.join();
    QVERIFY(entered); QVERIFY(nonblocking); QVERIFY(refused); QVERIFY(wrote);
    f.provider->beforePrepareWrite = {};
    auto frozen = scope.checkpointForRead(); QVERIFY(frozen.isValid());
    QVERIFY(scope.cancel()); QVERIFY(f.store->abort(tx)); frozen = {};
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::failedSealKeepsEarlierSegment()
{
    QFETCH(int, successfulProofs);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    auto first = f.begin(tx); auto guard = first.beginWrite(key()); QVERIFY(guard.isValid());
    static_cast<char *>(guard.data())[0] = char(0x55); guard = {}; QVERIFY(first.seal());
    const auto proofs = f.store->preparedPages(tx);
    auto second = f.begin(tx);
    for (int i : {0, 1}) {
        guard = second.beginWrite(key(i)); QVERIFY(guard.isValid());
        static_cast<char *>(guard.data())[0] = char(0x6a); guard = {};
    }
    f.provider->validationsUntilFailure = successfulProofs;
    QVERIFY(!second.seal(&f.error)); QVERIFY(!second.isActive());
    f.provider->validationsUntilFailure = -1;
    const auto restored = f.store->preparedPages(tx);
    QCOMPARE(restored.proofs.size(), proofs.proofs.size());
    QVERIFY(restored.proofs[0].authority.version == proofs.proofs[0].authority.version);
    QCOMPARE(f.store->mutationStatistics().pagesCancelled, quint64(2));
    QVERIFY(f.store->commit(tx, restored).isValid());
    QCOMPARE(quint8(f.pixel()[0]), quint8(0x55));
    QCOMPARE(f.pixel({}, 1), QByteArray(4, char(0x31)));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::allocationFailureCancelsSegment()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto bytes = f.provider->memoryUsage().committedBytes;
    const auto tx = f.store->beginCurrentTransaction(); auto scope = f.begin(tx);
    auto guard = scope.beginWrite(key()); QVERIFY(guard.isValid());
    static_cast<char *>(guard.data())[0] = char(0x55); guard = {};
    f.provider->rejectWrite = true;
    QVERIFY(!scope.beginWrite(key(1), &f.error).isValid());
    QVERIFY(!scope.seal());
    QCOMPARE(f.store->mutationStatistics().pagesCancelled, quint64(1));
    QCOMPARE(f.pixel(), QByteArray(4, char(0x31)));
    QCOMPARE(f.pixel({}, 1), QByteArray(4, char(0x31)));
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.provider->memoryUsage().committedBytes, bytes);
    QVERIFY(f.store->abort(tx)); QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::rejectedGenericReplicaCannotRetireAnotherPage()
{
    Fixture f; QVERIFY(f.init());
    const auto seeded = f.store->beginCurrentTransaction();
    auto mutation = f.begin(seeded);
    auto guard = mutation.beginWrite(key(1)); QVERIFY(guard.isValid());
    std::memset(guard.data(), 0x41, size_t(guard.byteSize()));
    guard = {}; QVERIFY(mutation.seal(&f.error));
    QVERIFY(f.store->commit(seeded, f.store->preparedPages(seeded)).isValid());
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.provider->lastTarget.version.key, key(1));
    const auto bytes = f.provider->memoryUsage().committedBytes;
    const auto retires = f.provider->retireCalls.load();

    // A faulty provider presents the live page-1 identity for a page-0 write.
    // Rejection must not register or retire that unrelated physical backing.
    f.provider->returnExistingWriteReplica = true;
    const auto tx = f.store->beginCurrentTransaction();
    const auto request = f.store->acquireWrite(tx, key(), cpu,
        KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
    QVERIFY(!request.isValid());
    f.provider->returnExistingWriteReplica = false;
    QVERIFY(f.store->abort(tx));
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.provider->retireCalls.load(), retires);
    QCOMPARE(f.provider->memoryUsage().committedBytes, bytes);
    QCOMPARE(f.pixel({}, 1), QByteArray(f.bpp, char(0x41)));

    // The same faulty identity on a virtual-default read must not register or
    // retire page 1. Once the fault is gone, page 0 must still materialize.
    f.provider->returnExistingRequestReplica = true;
    QVERIFY(f.pixel().isEmpty());
    f.provider->returnExistingRequestReplica = false;
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.provider->retireCalls.load(), retires);
    QCOMPARE(f.provider->memoryUsage().committedBytes, bytes);
    QCOMPARE(f.pixel({}, 1), QByteArray(f.bpp, char(0x41)));
    QCOMPARE(f.pixel(), QByteArray(f.bpp, char(0x2a)));
    const auto bytesAfterDefault = f.provider->memoryUsage().committedBytes;

    // Checkpoint byte import has the same physical-identity admission rule.
    auto initial = f.store->captureCommittedEpoch();
    KisPageVersion importedVersion = f.provider->lastTarget.version;
    importedVersion.key = key();
    initial.manifest = {importedVersion};
    KisSurfaceEpochState surface;
    QVERIFY(f.store->resolveSurfaceState({1}, {}, &surface));
    const auto descriptor = surface.allocationDescriptor();
    const QByteArray importedBytes(qsizetype(descriptor.minimumByteSize()), char(0x27));
    KisPageStore imported;
    QVERIFY2(imported.configure(initial, f.completions, 4, &f.error), qPrintable(f.error));
    QVERIFY(imported.registerReplicaProvider(f.provider));
    f.provider->returnExistingRequestReplica = true;
    QVERIFY(!imported.adoptInitialPageBytes(importedVersion, descriptor, importedBytes, &f.error));
    f.provider->returnExistingRequestReplica = false;
    QCOMPARE(f.provider->retireCalls.load(), retires);
    QCOMPARE(f.provider->memoryUsage().committedBytes, bytesAfterDefault);
    QCOMPARE(f.pixel({}, 1), QByteArray(f.bpp, char(0x41)));
    QVERIFY2(imported.adoptInitialPageBytes(importedVersion, descriptor, importedBytes, &f.error), qPrintable(f.error));
    QVERIFY2(imported.finalizeInitialization(&f.error), qPrintable(f.error));
    const auto importedRequest = imported.acquireRead(key(), {}, cpu, KisPagePriority::Normal);
    auto importedLease = imported.resolve(importedRequest, importedRequest.readiness);
    QVERIFY(importedLease.isValid());
    QCOMPARE(static_cast<const quint8 *>(importedLease.cpuData())[0], quint8(0x27));
    imported.release(std::move(importedLease));
    QVERIFY(imported.closeSession());
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
}

void KisPageStoreCpuMutationTest::threadConfinement()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction(); auto scope = f.begin(tx);
    bool accepted = true, sealed = true, removed = true;
    std::thread other([&] {
        accepted = scope.beginWrite(key()).isValid(); sealed = scope.seal(); removed = scope.removePage(key());
    });
    other.join(); QVERIFY(!accepted); QVERIFY(!sealed); QVERIFY(!removed); QVERIFY(scope.isActive());
    QVERIFY(scope.cancel()); QVERIFY(f.store->abort(tx)); QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::abandonedCompatibilityBatch()
{
    KisTiledDataManagerPageStoreBackend backend;
    const quint8 defaultPixel = 0x2a;
    QVERIFY(backend.configure(1, &defaultPixel));
    auto batch = backend.beginMutationBatch(); QVERIFY(batch);
    auto lease = backend.acquireTile(0, 0, true, false); QVERIFY(lease);
    lease->tileData()->data()[0] = 0x55;
    lease->markDirty();
    batch.reset(); // the lease keeps deferred cancellation alive
    QVERIFY(!backend.store()->closeSession());
    QVERIFY(lease->finish()); lease.reset();
    QCOMPARE(backend.store()->mutationStatistics().pagesCancelled, quint64(1));
    QCOMPARE(backend.store()->sessionStats().activeTransactions, qsizetype(0));
    auto read = backend.acquireTile(0, 0, false, false); QVERIFY(read);
    QCOMPARE(read->tileData()->data()[0], defaultPixel); QVERIFY(read->finish()); read.reset();
    batch = backend.beginMutationBatch(); QVERIFY(batch);
    lease = backend.acquireTile(0, 0, true, false); QVERIFY(lease);
    lease->tileData()->data()[0] = 0x6a; lease->markDirty(); QVERIFY(lease->finish()); lease.reset();
    QVERIFY(batch->finish()); batch.reset();
    read = backend.acquireTile(0, 0, false, false); QVERIFY(read);
    QCOMPARE(read->tileData()->data()[0], quint8(0x6a)); QVERIFY(read->finish()); read.reset();
    QVERIFY(backend.store()->closeSession());
}

void KisPageStoreCpuMutationTest::nestedCpuBatchDoesNotAbortOuter()
{
    KisTiledDataManagerPageStoreBackend backend; const quint8 blank = 0x2a;
    QVERIFY(backend.configure(1, &blank));
    QVERIFY(backend.fillRect(QRect(0, 0, 64, 64), QByteArray(1, char(0x31))));
    auto outer = backend.beginMutationBatch(); QVERIFY(outer);
    auto lease = backend.acquireTile(0, 0, true, false); QVERIFY(lease);
    lease->tileData()->data()[0] = 0x71; lease->markDirty(); QVERIFY(lease->finish()); lease.reset();
    QString error;
    QVERIFY(!backend.beginMutationBatch(&error)); QVERIFY(!error.isEmpty());
    QVERIFY(!backend.trimToRect(QRect(2, 2, 60, 60), &error));
    // A rejected nested operation must not join/poison the outer anonymous
    // transaction or consume its client count and writer reservations.
    QCOMPARE(backend.store()->sessionStats().activeTransactions, qsizetype(1));
    QVERIFY2(outer->finish(&error), qPrintable(error)); outer.reset();
    auto read = backend.acquireTile(0, 0, false, false); QVERIFY(read);
    QCOMPARE(read->tileData()->data()[0], quint8(0x71));
    QVERIFY(read->finish()); read.reset(); QVERIFY(backend.store()->closeSession());
}

void KisPageStoreCpuMutationTest::concurrentBatchAbandonment()
{
    KisTiledDataManagerPageStoreBackend backend; const quint8 blank = 0x2a;
    QVERIFY(backend.configure(1, &blank));
    quint8 previous = 0x31;
    QVERIFY(backend.fillRect(QRect(0, 0, 128, 64), QByteArray(1, char(previous))));
    for (int round = 0; round < 128; ++round) {
        const quint8 next = quint8(0x41 + round % 128);
        auto batch = backend.beginMutationBatch(); QVERIFY2(batch, qPrintable(QString::number(round)));
        auto first = backend.acquireTile(0, 0, true, false); QVERIFY(first);
        first->tileData()->data()[0] = next; first->markDirty(); QVERIFY(first->finish()); first.reset();
        auto last = backend.acquireTile(1, 0, true, false); QVERIFY(last);
        last->tileData()->data()[0] = next; last->markDirty();
        QSemaphore start;
        bool released = false;
        std::thread other([&] {
            start.acquire();
            if (round % 3 == 0) std::this_thread::yield();
            released = last->finish(); last.reset();
        });
        start.release();
        if (round % 3 == 1) std::this_thread::yield();
        batch.reset(); // may overlap the last foreign-thread unlock
        // Try to reuse the originating thread before the foreign finalizer
        // returns. Its registry slot must remain occupied until cancellation
        // has also dropped the anonymous transaction client.
        std::unique_ptr<KisTiledDataManagerPageStoreWriteBatch> following;
        QString error;
        QElapsedTimer wait; wait.start();
        do {
            following = backend.beginMutationBatch(&error);
            if (following || error != QStringLiteral("nested native CPU batch is not supported")) break;
            std::this_thread::yield();
        } while (wait.elapsed() < 5000);
        const bool finished = following && following->finish(&error);
        following.reset();
        other.join(); QVERIFY(released); QVERIFY2(finished, qPrintable(error));
        const auto stats = backend.store()->sessionStats();
        QCOMPARE(stats.activeTransactions, qsizetype(0)); QCOMPARE(stats.activeCpuWritePages, qsizetype(0));
        quint8 bytes[65] = {};
        QVERIFY(backend.readBytes(bytes, 0, 0, 65, 1, 65));
        // If every guard ended before the terminal attempt, normal seal is
        // legal; otherwise both pages must be cancelled. Never a half batch.
        QVERIFY(bytes[0] == previous || bytes[0] == next); QCOMPARE(bytes[0], bytes[64]);
        previous = bytes[0];
    }
    // Reusing the backend each round also checks its weak registry/key claims,
    // not just the canonical owner whose mutation destructor could clean up.
    QVERIFY(backend.store()->closeSession());
}

void KisPageStoreCpuMutationTest::failedProductionBatchCancelsUnpublished()
{
    QFETCH(bool, history);
    KisTiledDataManagerPageStoreBackend backend; const quint8 blank = 0x2a;
    QVERIFY(backend.configure(1, &blank));
    QVERIFY(backend.fillRect(QRect(0, 0, 64, 64), QByteArray(1, char(0x31))));
    KisMementoSP memento;
    if (history) {
        memento = backend.beginHistory(&blank, 1); QVERIFY(memento);
        QVERIFY(backend.fillRect(QRect(0, 0, 64, 64), QByteArray(1, char(0x41))));
    }
    const auto stats = backend.store()->mutationStatistics();
    auto batch = backend.beginMutationBatch(); QVERIFY(batch);
    auto first = backend.acquireTile(0, 0, true, false); QVERIFY(first);
    first->tileData()->data()[0] = 0x71; first->markDirty();
    auto second = backend.acquireTile(1, 0, true, false); QVERIFY(second);
    second->tileData()->data()[0] = 0x72; second->markDirty();
    QVERIFY(!batch->cancel()); // still borrowed pointers
    QCOMPARE(first->tileData()->data()[0], quint8(0x71));
    QVERIFY(first->finish()); first.reset(); QVERIFY(second->finish()); second.reset();
    QVERIFY(!batch->cancel()); batch.reset();
    const auto after = backend.store()->sessionStats();
    QCOMPARE(after.activeCpuWritePages, qsizetype(0));
    QCOMPARE(after.activeControlWritePages, qsizetype(0));
    QCOMPARE(backend.store()->mutationStatistics().pagesCancelled - stats.pagesCancelled, quint64(2));
    auto read = backend.acquireTile(0, 0, false, false); QVERIFY(read);
    QCOMPARE(read->tileData()->data()[0], quint8(history ? 0x41 : 0x31));
    QVERIFY(read->finish()); read.reset();
    read = backend.acquireTile(1, 0, false, false); QVERIFY(read);
    QCOMPARE(read->tileData()->data()[0], blank); QVERIFY(read->finish()); read.reset();
    if (history) {
        QVERIFY(backend.commitHistory(&blank, 1)); QVERIFY(backend.rollback(memento));
        read = backend.acquireTile(0, 0, false, false); QVERIFY(read);
        QCOMPARE(read->tileData()->data()[0], quint8(0x31)); QVERIFY(read->finish()); read.reset();
        QVERIFY(backend.rollforward(memento)); QVERIFY(backend.purgeHistory(memento, &blank, 1));
    }
    QVERIFY(backend.store()->closeSession());
}

void KisPageStoreCpuMutationTest::invalidAliasCancelsSemanticBatch()
{
    QFETCH(bool, history); QFETCH(bool, explicitFinish);
    KisTiledDataManagerPageStoreBackend backend; const quint8 blank = 0x2a;
    QVERIFY(backend.configure(1, &blank));
    QVERIFY(backend.fillRect(QRect(0, 0, 64, 64), QByteArray(1, char(0x31))));
    KisMementoSP memento;
    if (history) {
        memento = backend.beginHistory(&blank, 1); QVERIFY(memento);
        QVERIFY(backend.fillRect(QRect(0, 0, 64, 64), QByteArray(1, char(0x41))));
    }
    auto source = backend.acquireTile(0, 0, false, false); QVERIFY(source);
    const auto stats = backend.store()->mutationStatistics();
    auto batch = backend.beginMutationBatch(); QVERIFY(batch);
    QVERIFY(batch->replaceFullTile(0, 0, nullptr, true));
    // Invalid alias input must cancel the entire batch, not implicitly seal
    // the preceding removal. Valid completed inputs now use source capability.
    QVERIFY(!batch->replaceFullTile(1, 0, nullptr, false));
    QVERIFY(source->finish()); source.reset();
    if (explicitFinish) QVERIFY(!batch->finish());
    batch.reset();
    QCOMPARE(backend.store()->mutationStatistics().removalsCancelled - stats.removalsCancelled, quint64(1));
    QCOMPARE(backend.store()->sessionStats().activeCpuWritePages, qsizetype(0));
    auto read = backend.acquireTile(0, 0, false, false); QVERIFY(read);
    QCOMPARE(read->tileData()->data()[0], quint8(history ? 0x41 : 0x31));
    QVERIFY(read->finish()); read.reset();
    if (history) {
        QVERIFY(backend.commitHistory(&blank, 1)); QVERIFY(backend.rollback(memento));
        read = backend.acquireTile(0, 0, false, false); QVERIFY(read);
        QCOMPARE(read->tileData()->data()[0], quint8(0x31)); QVERIFY(read->finish()); read.reset();
        QVERIFY(backend.rollforward(memento)); QVERIFY(backend.purgeHistory(memento, &blank, 1));
    }
    QVERIFY(backend.store()->closeSession());
}

void KisPageStoreCpuMutationTest::immutableAliasSource()
{
    QFETCH(int, bpp);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    auto view = f.store->captureReadView(); auto read = view.readResidentPage(key()); QVERIFY(read.isValid());
    KisSurfaceEpochState state; QVERIFY(view.resolveSurfaceState({1}, &state));
    const auto descriptor = state.allocationDescriptor();
    auto *tile = f.provider->p->tileDataForCpuReadGuard(read); QVERIFY(tile); QVERIFY(tile->ref());
    const auto release = qScopeGuard([&] { tile->deref(); });
    const auto source = f.provider->p->captureCpuReadSource(read, descriptor); QVERIFY(source);
    QVERIFY(!f.provider->p->captureCpuReadSource({}, descriptor));
    auto badDescriptor = descriptor; badDescriptor.format.pixelAlignment = 2 * alignof(void *);
    QVERIFY(!f.provider->p->captureCompletedTileSource(badDescriptor, tile));
    auto foreign = std::make_shared<KisTiles3PageReplicaProvider>();
    QVERIFY(foreign->configure({f.provider->providerId(), f.provider->providerEpoch(), 1024 * 1024}, f.completions));
    // Matching public IDs are not a minting capability (including a provider
    // instance recreated with the same numeric IDs).
    const auto rejected = foreign->prepareSynchronousSource({700}, source, {key(3), {2}}, descriptor,
        KisReplicaSourceUse::ImmutableAlias, KisPagePriority::Normal);
    QVERIFY(!rejected.isValid()); QCOMPARE(foreign->memoryUsage().committedBytes, quint64(0));
    QByteArray untouched(64 * 64 * bpp, char(0x19));
    QVERIFY(!foreign->copySynchronousSourceToCpu(source, descriptor, untouched.data(), 64 * bpp, untouched.size()));
    QCOMPARE(untouched, QByteArray(untouched.size(), char(0x19)));
    const auto imported = foreign->captureCpuReadSource(read, descriptor); QVERIFY(imported);
    const auto adopted = foreign->prepareSynchronousSource({700}, imported, {key(3), {2}}, descriptor,
        KisReplicaSourceUse::ImmutableAlias, KisPagePriority::Normal);
    QVERIFY(adopted.isValid());
    QVERIFY(!foreign->prepareSynchronousSource({700}, imported, {key(3), {2}}, descriptor,
        KisReplicaSourceUse::ImmutableAlias, KisPagePriority::Normal).isValid());
    const auto secondAlias = foreign->prepareSynchronousSource(
        {701}, imported, {key(4), {2}}, descriptor,
        KisReplicaSourceUse::ImmutableAlias, KisPagePriority::Normal);
    QVERIFY(secondAlias.isValid());
    const auto firstFootprint = foreign->backingFootprint(adopted.replica);
    const auto secondFootprint = foreign->backingFootprint(secondAlias.replica);
    QVERIFY(firstFootprint.isValid());
    QCOMPARE(firstFootprint.physicalSlot, secondFootprint.physicalSlot);
    QCOMPARE(foreign->memoryUsage().committedBytes, descriptor.minimumByteSize());
    QVERIFY(foreign->retire({702}, adopted.replica, {}).isValid());
    QCOMPARE(foreign->memoryUsage().committedBytes, descriptor.minimumByteSize());
    QVERIFY(foreign->retire({703}, secondAlias.replica, {}).isValid());
    QCOMPARE(foreign->memoryUsage().committedBytes, quint64(0));
    read = {}; view = {};
    auto *tileStore = KisTileDataStore::instance();
    const bool swapped = tileStore->trySwapTileData(tile);
    QVERIFY(swapped); QVERIFY(!tile->data()); // COW user reference is not a RAM pin
    const auto domainChanges = f.provider->p->backingDomainChanges();
    QCOMPARE(domainChanges.size(), 1);
    QCOMPARE(domainChanges.front().physicalSlot, firstFootprint.physicalSlot);
    QCOMPARE(domainChanges.front().domain, KisPageAccessDomain::Ssd);
    QCOMPARE(domainChanges.front().bytes, descriptor.minimumByteSize());
    // backingUsage() is a pure ledger observation. The swap token has already
    // installed the physical move, so this accessor performs no domain rescan.
    const auto coldUsage = f.store->backingUsage();
    QCOMPARE(coldUsage.buckets[size_t(KisBackingBudgetClass::Current)].live.cpuRam,
             descriptor.minimumByteSize());
    QCOMPARE(coldUsage.buckets[size_t(KisBackingBudgetClass::Current)].live.ssd,
             descriptor.minimumByteSize());
    const auto tx = f.store->beginCurrentTransaction(); auto segment = f.store->beginMutation(tx);
    const auto requests = f.store->sessionStats(); const auto work = f.provider->p->payloadWork();
    QVERIFY(segment.aliasPage(key(2), source));
    QVERIFY(segment.aliasPage(key(3), source)); QVERIFY(!tile->data());
    QVERIFY(segment.seal()); QVERIFY(!tile->data()); // no address check or eager source materialization
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QCOMPARE(f.store->sessionStats().readRequestsCreated, requests.readRequestsCreated);
    QCOMPARE(f.provider->p->payloadWork().nativeDuplicatePages, work.nativeDuplicatePages);
    QCOMPARE(f.provider->p->payloadWork().explicitCopyPages, work.explicitCopyPages);
    QCOMPARE(f.provider->p->payloadWork().adoptedPages - work.adoptedPages, quint64(2));
    QVERIFY(f.provider->p->backingDomainChanges().empty());
    QCOMPARE(f.pixel({}, 2), QByteArray(bpp, char(0x31)));
    QCOMPARE(f.pixel({}, 3), QByteArray(bpp, char(0x31)));
    const auto residentUsage = f.store->backingUsage();
    QCOMPARE(residentUsage.buckets[size_t(KisBackingBudgetClass::Current)].live.cpuRam,
             2 * descriptor.minimumByteSize());
    QCOMPARE(residentUsage.buckets[size_t(KisBackingBudgetClass::Current)].live.ssd,
             quint64(0));
    const auto residentChanges = f.provider->p->backingDomainChanges();
    QCOMPARE(residentChanges.size(), 1);
    QCOMPARE(residentChanges.front().domain, KisPageAccessDomain::CpuRam);
    // Complete overwrite of a private alias must not initialize from it.
    const auto overwriteTx = f.store->beginCurrentTransaction(); auto overwrite = f.begin(overwriteTx);
    QVERIFY(overwrite.aliasPage(key(2), source));
    const auto beforeWrite = f.provider->p->payloadWork();
    QByteArray fullPayload(64*64*bpp,char(0x72));
    QVERIFY(overwrite.overwritePage(key(2),{fullPayload.constData(),64*bpp,fullPayload.size()}));
    QVERIFY(overwrite.seal());
    QVERIFY(f.store->commit(overwriteTx, f.store->preparedPages(overwriteTx)).isValid());
    QCOMPARE(f.provider->p->payloadWork().nativeDuplicatePages, beforeWrite.nativeDuplicatePages);
    QCOMPARE(f.provider->p->payloadWork().explicitCopyPages, beforeWrite.explicitCopyPages);
    QCOMPARE(f.pixel({}, 2), QByteArray(bpp, char(0x72)));
    QCOMPARE(f.pixel({}, 3), QByteArray(bpp, char(0x31)));
    QVERIFY(f.fill(0x55)); QCOMPARE(f.pixel({}, 3), QByteArray(bpp, char(0x31)));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::aliasBatchFailure()
{
    QFETCH(int, bpp); QFETCH(int, failure);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    auto prior = f.begin(tx);
    for (int i = 0; i < 3; ++i) {
        auto w = prior.beginWrite(key(i)); QVERIFY(w.isValid()); memset(w.data(), 0x41, size_t(w.byteSize()));
    }
    QVERIFY(prior.seal());
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay; overlay.transaction = tx.id;
    auto old = f.store->captureReadView(overlay);
    KisSurfaceEpochState state; QVERIFY(old.resolveSurfaceState({1}, &state));
    const QByteArray pixel(bpp, char(0x61));
    auto *tile = KisTileDataStore::instance()->createDefaultTileData(bpp,
        reinterpret_cast<const quint8 *>(pixel.constData()));
    tile->acquire(); const auto cleanup = qScopeGuard([&] { tile->release(); });
    auto source = f.provider->p->captureCompletedTileSource(state.allocationDescriptor(), tile);
    auto segment = f.begin(tx);
    QVERIFY(segment.aliasPage(key(0), source)); QVERIFY(segment.aliasPage(key(1), source));
    auto w = segment.beginWrite(key(2)); QVERIFY(w.isValid()); static_cast<quint8 *>(w.data())[0] = 0x72; w = {};
    int preparations = 0;
    f.provider->beforeAdopt = [&] {
        auto captured = f.store->captureReadView(overlay);
        for (int i = 0; i < 3; ++i) {
            auto span = captured.readResidentPage(key(i)); QVERIFY(span.isValid());
            QCOMPARE(static_cast<const quint8 *>(span.data())[0], quint8(0x41));
        }
        if (++preparations == 2 && failure == 0) f.provider->rejectAdopt = true;
    };
    if (failure) f.provider->validationsUntilFailure = failure - 1;
    QVERIFY(!segment.seal(&f.error)); QVERIFY(!segment.isActive());
    f.provider->rejectAdopt = false; f.provider->validationsUntilFailure = -1; f.provider->beforeAdopt = {};
    QCOMPARE(preparations, 2);
    auto current = f.store->captureReadView(overlay);
    for (int i = 0; i < 3; ++i) {
        auto span = current.readResidentPage(key(i)); QVERIFY(span.isValid());
        QCOMPARE(static_cast<const quint8 *>(span.data())[0], quint8(0x41));
        span = old.readResidentPage(key(i)); QVERIFY(span.isValid());
        QCOMPARE(static_cast<const quint8 *>(span.data())[0], quint8(0x41));
    }
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    old = {}; current = {}; source.reset(); QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::aliasMutation_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("sequence"); QTest::addColumn<int>("terminal");
    for (int bpp : {1, 4, 8, 16}) for (int sequence = 0; sequence < 8; ++sequence)
        for (int terminal = 0; terminal < 3; ++terminal)
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-sequence%2-terminal%3").arg(bpp).arg(sequence).arg(terminal)))
                << bpp << sequence << terminal;
}

void KisPageStoreCpuMutationTest::aliasMutation()
{
    QFETCH(int, bpp); QFETCH(int, sequence); QFETCH(int, terminal);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    auto prior = f.begin(tx); auto w = prior.beginWrite(key()); QVERIFY(w.isValid());
    memset(w.data(), 0x41, size_t(w.byteSize())); w = {}; QVERIFY(prior.seal());
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay; overlay.transaction = tx.id;
    auto old = f.store->captureReadView(overlay); QVERIFY(old.isValid());
    KisSurfaceEpochState surface; QVERIFY(old.resolveSurfaceState({1}, &surface));
    const QByteArray input(bpp, char(0x61));
    auto *tile = KisTileDataStore::instance()->createDefaultTileData(bpp,
        reinterpret_cast<const quint8 *>(input.constData()));
    QVERIFY(tile); tile->acquire();
    const auto release = qScopeGuard([&] { tile->release(); });
    auto source = f.provider->p->captureCompletedTileSource(surface.allocationDescriptor(), tile, &f.error);
    QVERIFY2(source, qPrintable(f.error));
    const auto stats = f.store->mutationStatistics(); const auto work = f.provider->p->payloadWork();
    auto segment = f.begin(tx); QVERIFY(segment.isActive());
    KisPageVersion pending;
    const bool writeFirst = sequence == 2 || sequence == 3 || sequence == 6 || sequence == 7;
    if (writeFirst) {
        w = segment.beginWrite(key()); QVERIFY(w.isValid()); pending = w.version();
        static_cast<quint8 *>(w.data())[0] = 0x52;
        QVERIFY(!segment.aliasPage(key(), source)); // never revoke a borrowed pointer
        w = {};
    }
    if (sequence == 4 || sequence == 7) QVERIFY(segment.removePage(key()));
    QVERIFY2(segment.aliasPage(key(), source, &f.error), qPrintable(f.error));
    // Replacing the same private alias never allocates or reserves generation.
    QVERIFY(segment.aliasPage(key(), source));
    QCOMPARE(f.store->mutationStatistics().generationsReserved - stats.generationsReserved, quint64(writeFirst));
    QCOMPARE(f.provider->p->payloadWork().adoptedPages, work.adoptedPages);
    if (sequence == 1 || sequence == 3) {
        w = segment.beginWrite(key(), &f.error); QVERIFY2(w.isValid(), qPrintable(f.error));
        if (pending.isValid()) QCOMPARE(w.version(), pending);
        pending = w.version();
        QCOMPARE(QByteArray(static_cast<const char *>(w.data()), bpp), input);
        static_cast<quint8 *>(w.data())[0] = 0x72; w = {};
        w = segment.beginWrite(key()); QVERIFY(w.isValid()); QCOMPARE(w.version(), pending);
        QCOMPARE(static_cast<const quint8 *>(w.data())[0], quint8(0x72)); w = {};
    }
    const bool removed = sequence == 5 || sequence == 6;
    if (removed) QVERIFY(segment.removePage(key()));
    auto during = f.store->captureReadView(overlay); QVERIFY(during.isValid());
    auto before = during.readResidentPage(key()); QVERIFY(before.isValid());
    QCOMPARE(static_cast<const quint8 *>(before.data())[0], quint8(0x41));
    f.provider->beforeAdopt = [&] {
        auto capture = f.store->captureReadView(overlay);
        auto span = capture.readResidentPage(key());
        QVERIFY(span.isValid()); QCOMPARE(static_cast<const quint8 *>(span.data())[0], quint8(0x41));
    };
    const bool failureExpected = terminal == 2 && !removed;
    if (failureExpected) f.provider->validationsUntilFailure = 0;
    if (terminal == 1) QVERIFY(segment.cancel());
    else QCOMPARE(segment.seal(&f.error), !failureExpected);
    f.provider->validationsUntilFailure = -1; f.provider->beforeAdopt = {};
    if (terminal == 0 && !removed && pending.isValid()) {
        KisPageVersion version; QVERIFY(f.store->resolvePageVersion(key(), overlay, &version));
        QCOMPARE(version, pending); // write -> alias preserves the pending generation
    }
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    const auto expected = terminal == 1 || failureExpected ? 0x41 : removed ? 0x2a :
        (sequence == 1 || sequence == 3) ? 0x72 : 0x61;
    QCOMPARE(quint8(f.pixel()[0]), quint8(expected));
    QCOMPARE(static_cast<const quint8 *>(before.data())[0], quint8(0x41));
    auto original = old.readResidentPage(key()); QVERIFY(original.isValid());
    QCOMPARE(static_cast<const quint8 *>(original.data())[0], quint8(0x41));
    QVERIFY(tile->blockSwapping()); QCOMPARE(tile->data()[0], quint8(0x61)); tile->unblockSwapping();
    original = {}; before = {}; during = {}; old = {}; source.reset();
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::semanticOnlyMutation()
{
    QFETCH(int, bpp); Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    auto prior = f.begin(tx);
    for (int x : {0, 2}) {
        auto w = prior.beginWrite(key(x)); QVERIFY(w.isValid());
        memset(w.data(), 0x41, size_t(w.byteSize()));
    }
    QVERIFY(prior.seal());
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay; overlay.transaction = tx.id;
    auto before = f.store->captureReadView(overlay); QVERIFY(before.isValid());
    const auto request = f.store->acquireRead(key(), overlay, cpu, KisPagePriority::Normal);
    auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
    f.provider->disableNativeMutation = true;
    auto unavailablePixels = f.begin(tx);
    QVERIFY(unavailablePixels.isActive()); // semantics are not a CPU capability
    QVERIFY(!unavailablePixels.beginWrite(key()).isValid());
    QVERIFY(unavailablePixels.cancel());
    const auto stats = f.store->mutationStatistics();
    const auto payload = f.provider->p->payloadWork();
    const auto registered = f.store->sessionStats().registeredPages;
    auto delta = f.store->beginMutation(tx); QVERIFY(delta.isActive());
    for (int x : {0, 2, 99, 0}) QVERIFY(delta.removePage(key(x), &f.error));
    QCOMPARE(f.store->sessionStats().activeCpuWritePages, qsizetype(3));
    QVERIFY(!f.store->abort(tx));
    QVERIFY(!f.store->stageSurfaceDefaultPixel(tx, {1}, QByteArray(bpp, char(0x55))));
    QVERIFY(!f.remove(tx, key()));
    const auto otherTx = f.store->beginCurrentTransaction();
    auto other = f.store->beginMutation(otherTx); QVERIFY(!other.removePage(key()));
    QVERIFY(other.cancel()); QVERIFY(f.store->abort(otherTx));
    auto during = f.store->captureReadView(overlay); QVERIFY(during.isValid());
    for (int x : {0, 2}) {
        auto r = during.readResidentPage(key(x)); QVERIFY(r.isValid());
        QCOMPARE(QByteArray(static_cast<const char *>(r.data()), bpp), QByteArray(bpp, char(0x41)));
    }
    QVERIFY2(delta.seal(&f.error), qPrintable(f.error));
    QCOMPARE(f.store->sessionStats().registeredPages, registered); // blank removal did not materialize metadata
    const auto after = f.store->mutationStatistics();
    QCOMPARE(after.sessionsCreated, stats.sessionsCreated);
    QCOMPARE(after.operationSessionsCreated - stats.operationSessionsCreated, quint64(3));
    // The primary delta, the rejected same-transaction contender above and
    // the rejected foreign delta are all real operation sessions.
    QCOMPARE(after.generationsReserved, stats.generationsReserved);
    QCOMPARE(after.writablePinsAcquired, stats.writablePinsAcquired);
    QCOMPARE(after.removalsSealed - stats.removalsSealed, quint64(3));
    QCOMPARE(f.provider->p->payloadWork().nativeDuplicateBytes, payload.nativeDuplicateBytes);
    QCOMPARE(f.provider->p->payloadWork().defaultInitializedBytes, payload.defaultInitializedBytes);
    QCOMPARE(f.provider->p->payloadWork().adoptedBytes, payload.adoptedBytes);
    auto sealed = f.store->captureReadView(overlay); QVERIFY(sealed.isValid());
    for (int x : {0, 2}) {
        auto r = sealed.readResidentPage(key(x)); QVERIFY(r.isValid()); QVERIFY(r.version().isDefaultPixel());
        QCOMPARE(QByteArray(static_cast<const char *>(r.data()), bpp), QByteArray(bpp, char(0x2a)));
    }
    // Read capabilities and a previously sealed version survive detachment.
    QCOMPARE(QByteArray(static_cast<const char *>(lease.cpuData()), bpp), QByteArray(bpp, char(0x41)));
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    for (auto *view : {&before, &during}) {
        auto r = view->readResidentPage(key(2)); QVERIFY(r.isValid());
        QCOMPARE(QByteArray(static_cast<const char *>(r.data()), bpp), QByteArray(bpp, char(0x41)));
    }
    f.store->release(std::move(lease)); before = {}; during = {}; sealed = {};
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::mixedSemanticAtomicity_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("terminal");
    for (int bpp : {1, 4, 8, 16}) for (int terminal : {0, 1, 2, 3})
        QTest::newRow(qPrintable(QStringLiteral("bpp%1-terminal%2").arg(bpp).arg(terminal))) << bpp << terminal;
}

void KisPageStoreCpuMutationTest::mixedSemanticAtomicity()
{
    QFETCH(int, bpp); QFETCH(int, terminal);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    auto prior = f.begin(tx);
    for (int x : {0, 1, 3}) {
        auto w = prior.beginWrite(key(x)); QVERIFY(w.isValid()); memset(w.data(), 0x41, size_t(w.byteSize()));
    }
    QVERIFY(prior.seal());
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay; overlay.transaction = tx.id;
    auto before = f.store->captureReadView(overlay); QVERIFY(before.isValid());
    auto delta = f.begin(tx); QVERIFY(delta.removePage(key(0))); QVERIFY(delta.removePage(key(3)));
    KisCapturedReadView preparing;
    f.provider->beforePrepareWrite = [&] { preparing = f.store->captureReadView(overlay); };
    for (int x : {1, 2}) {
        auto w = delta.beginWrite(key(x)); QVERIFY(w.isValid()); memset(w.data(), 0x70 + x, size_t(w.byteSize()));
    }
    f.provider->beforePrepareWrite = {};
    QVERIFY(preparing.isValid()); // captured inside actual unlocked provider preparation
    auto during = f.store->captureReadView(overlay); QVERIFY(during.isValid());
    for (int x : {0, 1, 3}) QCOMPARE(f.pixel(overlay, x), QByteArray(bpp, char(0x41)));
    if (terminal == 1) QVERIFY(delta.cancel());
    else {
        if (terminal >= 2) f.provider->validationsUntilFailure = terminal - 2;
        QCOMPARE(delta.seal(&f.error), terminal == 0);
        f.provider->validationsUntilFailure = -1;
    }
    QCOMPARE(f.store->sessionStats().activeCpuWritePages, qsizetype(0));
    auto after = f.store->captureReadView(overlay); QVERIFY(after.isValid());
    for (int x : {0, 1, 2, 3}) {
        const char expected = terminal == 0 ? (x == 1 ? char(0x71) : x == 2 ? char(0x72) : char(0x2a))
                                           : (x == 2 ? char(0x2a) : char(0x41));
        auto r = after.readResidentPage(key(x)); QVERIFY(r.isValid());
        QCOMPARE(QByteArray(static_cast<const char *>(r.data()), bpp), QByteArray(bpp, expected));
    }
    // Failed seals retain *all* earlier proofs, including a page not in base.
    const auto prepared = f.store->preparedPages(tx);
    QCOMPARE(prepared.proofs.size(), terminal == 0 ? qsizetype(2) : qsizetype(3));
    QCOMPARE(prepared.removedPages.size(), terminal == 0 ? qsizetype(1) : qsizetype(0));
    QVERIFY(f.store->commit(tx, prepared).isValid());
    for (auto *view : {&before, &during, &preparing}) for (int x : {0, 1, 3}) {
        auto r = view->readResidentPage(key(x)); QVERIFY(r.isValid());
        QCOMPARE(QByteArray(static_cast<const char *>(r.data()), bpp), QByteArray(bpp, char(0x41)));
    }
    before = {}; during = {}; preparing = {}; after = {}; QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::concurrentMixedSemanticCapture()
{
    Fixture f; QVERIFY(f.init(1)); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay; overlay.transaction = tx.id;
    QSemaphore start; std::atomic<bool> done{false}; bool success = true;
    std::thread writer([&] {
        start.acquire();
        for (int round = 0; round < 128 && success; ++round) {
            auto delta = f.begin(tx);
            success = delta.isActive() && delta.removePage(key(round % 2));
            if (success) {
                auto w = delta.beginWrite(key(1 - round % 2)); success = w.isValid();
                if (success) static_cast<quint8 *>(w.data())[0] = quint8(0x41 + round % 32);
            }
            success = success && delta.seal();
            std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });
    bool whole = true; int captures = 0; start.release();
    do {
        auto view = f.store->captureReadView(overlay);
        auto a = view.readResidentPage(key()); auto b = view.readResidentPage(key(1));
        if (!a.isValid() || !b.isValid()) whole = false;
        else {
            const quint8 x = *static_cast<const quint8 *>(a.data()), y = *static_cast<const quint8 *>(b.data());
            whole &= (x == 0x31 && y == 0x31) || (x == 0x2a && y >= 0x41 && y < 0x61) ||
                     (y == 0x2a && x >= 0x41 && x < 0x61);
        }
        ++captures; std::this_thread::yield();
    } while (!done.load(std::memory_order_acquire));
    writer.join(); QVERIFY(success); QVERIFY(whole); QVERIFY(captures > 0);
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::semanticRewriteReusesPending_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("writeFirst"); QTest::addColumn<bool>("removeLast");
    for (int bpp : {1, 4, 8, 16}) for (bool first : {false, true}) for (bool last : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("bpp%1-writeFirst%2-removeLast%3").arg(bpp).arg(first).arg(last))) << bpp << first << last;
}

void KisPageStoreCpuMutationTest::semanticRewriteReusesPending()
{
    QFETCH(int, bpp); QFETCH(bool, writeFirst); QFETCH(bool, removeLast);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    QVERIFY(f.store->stageSurfaceDefaultPixel(tx, {1}, QByteArray(bpp, char(0x46))));
    auto old = f.store->captureReadView();
    const auto before = f.store->mutationStatistics();
    auto delta = f.begin(tx); KisPageVersion version; void *pointer = nullptr;
    if (writeFirst) {
        auto w = delta.beginWrite(key()); QVERIFY(w.isValid()); version = w.version(); pointer = w.data();
        QVERIFY(!delta.removePage(key())); // never revoke a borrowed pointer
        memset(w.data(), 0x66, size_t(w.byteSize()));
    }
    for (int i = 0; i < 2; ++i) {
        QVERIFY(delta.removePage(key())); QVERIFY(delta.removePage(key()));
        auto w = delta.beginWrite(key()); QVERIFY2(w.isValid(), qPrintable(f.error));
        if (version.isValid()) { QCOMPARE(w.version(), version); QCOMPARE(w.data(), pointer); }
        version = w.version(); pointer = w.data();
        QCOMPARE(QByteArray(static_cast<const char *>(w.data()), qsizetype(w.byteSize())), QByteArray(64 * 64 * bpp, char(0x46)));
        static_cast<quint8 *>(w.data())[0] = 0x72;
    }
    if (removeLast) QVERIFY(delta.removePage(key()));
    QVERIFY2(delta.seal(&f.error), qPrintable(f.error));
    const auto after = f.store->mutationStatistics();
    QCOMPARE(after.generationsReserved - before.generationsReserved, quint64(1));
    QCOMPARE(after.pagesSealed - before.pagesSealed, quint64(removeLast ? 0 : 1));
    QCOMPARE(after.pagesCancelled - before.pagesCancelled, quint64(removeLast ? 1 : 0));
    QCOMPARE(after.pendingDefaultResetBytes - before.pendingDefaultResetBytes, quint64((writeFirst ? 2 : 1) * 64 * 64 * bpp));
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QCOMPARE(quint8(f.pixel()[0]), quint8(removeLast ? 0x46 : 0x72));
    auto r = old.readResidentPage(key()); QVERIFY(r.isValid());
    QCOMPARE(static_cast<const quint8 *>(r.data())[0], quint8(0x31));
    r = {}; old = {}; QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::supersededOverlayUsesHistoricalLifetime_data()
{
    QTest::addColumn<int>("route"); QTest::addColumn<int>("reader"); QTest::addColumn<int>("terminal");
    for (int route = 0; route < 4; ++route) for (int reader = 0; reader < 7; ++reader)
        for (int terminal = 0; terminal < 3; ++terminal)
            QTest::newRow(qPrintable(QStringLiteral("route%1-reader%2-terminal%3").arg(route).arg(reader).arg(terminal)))
                << route << reader << terminal;
}

void KisPageStoreCpuMutationTest::supersededOverlayUsesHistoricalLifetime()
{
    QFETCH(int, route); QFETCH(int, reader); QFETCH(int, terminal);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    const auto write = [&](bool generic, quint8 value, bool fail) {
        if (generic) {
            const auto request = f.store->acquireWrite(tx, key(), cpu, KisPageWriteMode::PreserveContents, KisPagePriority::Normal);
            auto lease = f.store->resolve(request, request.readiness);
            if (!lease.isValid()) return false;
            std::memset(lease.cpuData(), value, size_t(lease.byteSize()));
            if (fail) f.provider->validationsUntilFailure = 0;
            return f.store->publishHostWrite(std::move(lease)).isValid();
        }
        auto segment = f.begin(tx); auto guard = segment.beginWrite(key());
        if (!guard.isValid()) return false;
        std::memset(guard.data(), value, size_t(guard.byteSize())); guard = {};
        if (fail) f.provider->validationsUntilFailure = 0;
        return segment.seal(&f.error);
    };
    QVERIFY(write(route & 1, 0x41, false));
    KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay; overlay.transaction = tx.id;
    KisPageVersion oldVersion; QVERIFY(f.store->resolvePageVersion(key(), overlay, &oldVersion));
    KisCapturedReadView captured;
    KisCpuReadGuard guard;
    KisReadRequest request;
    std::unique_ptr<KisReadLease> lease;
    KisCompletionTicket lastUse;
    if (reader == 0 || reader == 4) {
        captured = f.store->captureReadView(overlay); QVERIFY(captured.isValid());
        if (reader == 4) { guard = captured.readResidentPage(key()); QVERIFY(guard.isValid()); }
    } else {
        request = f.store->acquireRead(key(), overlay, cpu, KisPagePriority::Normal); QVERIFY(request.isValid());
        if (reader == 1 || reader == 3) {
            lease = std::make_unique<KisReadLease>(f.store->resolve(request, request.readiness));
            QVERIFY(lease->isValid());
        }
        if (reader == 3) {
            QCOMPARE(static_cast<const quint8 *>(lease->cpuData())[0], quint8(0x41));
            const KisCompletionDomain source = KisCompletionDomain::HostLogical;
            lastUse = f.completions->allocatePending(f.completions->registerSource(source)); QVERIFY(lastUse.isValid());
            f.store->release(std::move(*lease), lastUse);
        }
    }
    QCOMPARE(write(route & 2, 0x72, terminal == 2), terminal != 2);
    f.provider->validationsUntilFailure = -1;
    QCOMPARE(f.pixel(overlay), QByteArray(f.bpp, char(terminal == 2 ? 0x41 : 0x72)));
    const auto proofs = f.store->preparedPages(tx).proofs;
    QCOMPARE(proofs.size(), qsizetype(1));
    QCOMPARE(proofs.first().authority.version == oldVersion, terminal == 2);
    if (terminal == 1) QVERIFY(f.store->abort(tx)); // detached readers no longer belong to the transaction
    else QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QVERIFY(f.store->waitForRetirementIdle());
    if (reader == 0) { guard = captured.readResidentPage(key()); QVERIFY(guard.isValid()); }
    if (reader == 2) {
        lease = std::make_unique<KisReadLease>(f.store->resolve(request, request.readiness));
        QVERIFY(lease->isValid());
    } else if (reader == 5) {
        QVERIFY(f.store->cancel(request));
    } else if (reader == 6) {
        f.provider->rejectReadAccess = true;
        QVERIFY(!f.store->resolve(request, request.readiness).isValid());
        f.provider->rejectReadAccess = false;
        QVERIFY(!f.store->cancel(request));
    }
    if (guard.isValid()) {
        QCOMPARE(guard.version(), oldVersion);
        QCOMPARE(static_cast<const quint8 *>(guard.data())[0], quint8(0x41));
    }
    if (lease && lease->isValid()) {
        QCOMPARE(lease->version(), oldVersion);
        QCOMPARE(static_cast<const quint8 *>(lease->cpuData())[0], quint8(0x41));
        f.store->release(std::move(*lease));
    }
    guard = {}; captured = {};
    if (lastUse.isValid()) {
        QCOMPARE(f.store->sessionStats().pendingLastUses, qsizetype(1));
        QVERIFY(!f.store->closeSession());
        QVERIFY(f.completions->complete(lastUse, KisCompletionStatus::Succeeded));
        QTRY_COMPARE(f.store->sessionStats().pendingLastUses, qsizetype(0));
    }
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(2));
    QCOMPARE(f.store->sessionStats().sealedPreparedProofs, qsizetype(0));
    QCOMPARE(f.pixel(), QByteArray(f.bpp, char(terminal == 1 ? 0x31 : terminal == 2 ? 0x41 : 0x72)));
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
}

void KisPageStoreCpuMutationTest::consumedGenericLeaseCanRetryAbort()
{
    QFETCH(int, terminal);
    Fixture f;
    KisPageBackingLimits limits;
    limits.retirementDebtBytes = 64 * 64 * f.bpp;
    QVERIFY(f.store->configureBackingLimits(limits));
    QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    const auto request = f.store->acquireWrite(tx, key(), cpu, KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
    auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
    const auto other = f.store->beginCurrentTransaction();
    auto mutation = f.begin(other);
    auto guard = mutation.beginWrite(key(1)); QVERIFY(guard.isValid()); guard = {};
    f.provider->rejectRetire = true;
    QVERIFY(mutation.cancel()); QVERIFY(f.store->abort(other));
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].live.cpuRam,
             limits.retirementDebtBytes);
    if (terminal == 0) f.store->cancel(std::move(lease));
    else if (terminal == 1) QVERIFY(!f.store->publish(std::move(lease), {}).isValid());
    else {
        f.provider->validationsUntilFailure = 0;
        QVERIFY(!f.store->publishHostWrite(std::move(lease)).isValid());
        f.provider->validationsUntilFailure = -1;
    }
    QCOMPARE(f.store->sessionStats().activeControlWritePages, qsizetype(1));
    QVERIFY(!f.store->abort(tx));
    auto blocked = f.begin(tx);
    QVERIFY(!blocked.beginWrite(key()).isValid()); QVERIFY(blocked.cancel());
    f.provider->rejectRetire = false;
    f.store->processRetirements(64); QVERIFY(f.store->waitForRetirementIdle());
    QVERIFY(f.store->abort(tx));
    QCOMPARE(f.store->sessionStats().activeControlWritePages, qsizetype(0));
    QCOMPARE(f.pixel(), QByteArray(f.bpp, char(0x31)));
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
}

void KisPageStoreCpuMutationTest::partialPreparationRollbackPumpsForegroundRetirement()
{
    Fixture f;
    KisPageBackingLimits limits; limits.retirementDebtBytes = 64 * 64 * f.bpp;
    QVERIFY(f.store->configureBackingLimits(limits));
    f.provider->disableBackgroundRetirement = true;
    QVERIFY(f.init());
    const auto tx = f.store->beginCurrentTransaction();
    auto mutation = f.begin(tx);
    auto execution = mutation.borrowExecution({1}, {{0, 0}, {1, 0}});
    QVERIFY(execution.isActive());
    QCOMPARE(execution.prepareWrites(&f.error), KisPageMutationExecution::PreparationResult::Ready);
    f.provider->rejectRetire = true;
    // The first page transfers its record; the second cannot reserve Debt.
    QCOMPARE(execution.finishPreparedWrites(nullptr, &f.error), qsizetype(-1));
    QVERIFY(!f.error.isEmpty());
    QCOMPARE(f.provider->retireCalls.load(), 1); // Foreground first attempt ran on refusal exit.
    QCOMPARE(f.store->mutationStatistics().pagesCancelled, quint64(1));
    QCOMPARE(f.store->sessionStats().activeCpuWritePages, qsizetype(2)); // Original borrow retains both logical claims.
    QCOMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(1));
    QCOMPARE(f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].live.cpuRam,
             limits.retirementDebtBytes);
    QVERIFY(!execution.finish(&f.error));
    f.provider->rejectRetire = false;
    f.store->processRetirements(1);
    QCOMPARE(f.provider->retireCalls.load(), 2);
    QVERIFY(mutation.cancel()); // Continues the untouched second page, without replaying the first.
    QCOMPARE(f.provider->retireCalls.load(), 3);
    QCOMPARE(f.store->mutationStatistics().pagesCancelled, quint64(2));
    QVERIFY(f.store->abort(tx));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
    QCOMPARE(f.pixel(), QByteArray(f.bpp, char(0x2a)));
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::partialCancellationRetiresBeforeRetry()
{
    QFETCH(int, route);
    QFETCH(int, bpp);
    QFETCH(int, pageCount);
    Fixture f;
    KisPageBackingLimits limits;
    limits.retirementDebtBytes = 64 * 64 * bpp;
    QVERIFY(f.store->configureBackingLimits(limits));
    QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    auto mutation = route == 0 ? f.begin(tx) : KisPageMutationSession{};
    for (int i = 0; i < pageCount; ++i) {
        if (route == 0) {
            auto guard = mutation.beginWrite(key(i)); QVERIFY(guard.isValid());
        } else {
            const auto request = f.store->acquireWrite(tx, key(i), cpu, KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
            QVERIFY(request.isValid());
            if (route == 2) {
                auto lease = f.store->resolve(request, request.readiness); QVERIFY(lease.isValid());
                QVERIFY(f.store->publishHostWrite(std::move(lease)).isValid());
            }
        }
    }
    f.provider->rejectRetire = true;
    QVERIFY(!(route == 0 ? mutation.cancel() : f.store->abort(tx)));
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->backingUsage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].live.cpuRam,
             limits.retirementDebtBytes);
    KisPageMutationSession reclaimed;
    if (route == 0) {
        QCOMPARE(f.store->sessionStats().activeCpuWritePages, qsizetype(pageCount - 1));
        reclaimed = f.begin(tx);
        QVERIFY(reclaimed.removePage(key(0), &f.error));
        auto blocked = f.begin(tx);
        QVERIFY(!blocked.removePage(key(1), &f.error));
        QVERIFY(blocked.cancel());
    }
    f.provider->rejectRetire = false;
    f.store->processRetirements(64); QVERIFY(f.store->waitForRetirementIdle());
    if (route == 0) {
        bool done = false;
        for (int attempt = 0; attempt < pageCount; ++attempt) {
            f.provider->rejectRetire = true;
            done = mutation.cancel();
            QVERIFY(f.store->waitForRetirementIdle());
            f.provider->rejectRetire = false;
            f.store->processRetirements(64);
            QVERIFY(f.store->waitForRetirementIdle());
            if (done) break;
        }
        QVERIFY(done);
        // Retrying the original cancellation cannot release a new writer of
        // its already-completed key.
        auto blocked = f.begin(tx);
        QVERIFY(!blocked.removePage(key(0), &f.error));
        QVERIFY(blocked.cancel());
        QVERIFY(reclaimed.cancel());
        QCOMPARE(f.store->mutationStatistics().pagesCancelled, quint64(pageCount));
    }
    QVERIFY(f.store->abort(tx));
    QCOMPARE(f.pixel(), QByteArray(f.bpp, char(0x31)));
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
}

void KisPageStoreCpuMutationTest::cleanDomainJournalSkipsSnapshots()
{
    Fixture f;
    QVERIFY2(f.init(), qPrintable(f.error));
    KisBackingBudgetController budget;
    KisPageOwnerLedger ledger;
    QVERIFY(ledger.configure(f.completions, &f.error));
    ledger.attachBackingBudget(budget);
    QVERIFY(ledger.registerProvider(f.provider, &f.error));
    QVERIFY(!f.provider->mayHaveBackingDomainChanges());
    const auto before = f.provider->domainJournalCalls.load();
    for (int i = 0; i < 1000; ++i)
        QVERIFY2(ledger.synchronizeBackingDomains(&f.error), qPrintable(f.error));
    QCOMPARE(f.provider->domainJournalCalls.load(), before);

    // The base contract is conservative for providers that cannot expose a
    // marker. Their journals must still be read on every synchronization.
    QVERIFY(f.provider->KisPageReplicaProvider::mayHaveBackingDomainChanges());
    f.provider->forceDomainJournalProbe = true;
    QVERIFY(ledger.synchronizeBackingDomains(&f.error));
    QCOMPARE(f.provider->domainJournalCalls.load(), before + 1);
    f.provider->forceDomainJournalProbe = false;

    KisPageOwnerLedger missingBudget;
    QVERIFY(missingBudget.configure(f.completions, &f.error));
    QVERIFY(missingBudget.registerProvider(f.provider, &f.error));
    QVERIFY(!missingBudget.synchronizeBackingDomains(&f.error));
    QVERIFY(f.error.contains(QStringLiteral("budget")));
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::dirtyDomainJournalRetainsUnregistered()
{
    QFETCH(int, bpp);
    Fixture f;
    QVERIFY2(f.init(bpp), qPrintable(f.error));
    QVERIFY2(f.fill(0x31), qPrintable(f.error));
    const auto handle = f.provider->lastTarget;
    auto view = f.store->captureReadView();
    auto read = view.readResidentPage(handle.version.key);
    QVERIFY(read.isValid());
    auto *tile = f.provider->p->tileDataForCpuReadGuard(read);
    QVERIFY(tile && tile->ref());
    const auto release = qScopeGuard([&] { tile->deref(); });
    read = {}; view = {};

    KisBackingBudgetController budget;
    KisPageOwnerLedger ledger;
    QVERIFY(ledger.configure(f.completions, &f.error));
    ledger.attachBackingBudget(budget);
    QVERIFY(ledger.registerProvider(f.provider, &f.error));
    QVERIFY(KisTileDataStore::instance()->trySwapTileData(tile));
    QVERIFY(f.provider->mayHaveBackingDomainChanges());
    const auto cold = f.provider->backingFootprint(handle);
    QCOMPARE(cold.domain, KisPageAccessDomain::Ssd);
    QVERIFY(ledger.synchronizeBackingDomains(&f.error));
    // The new owner has not registered this backing. It cannot acknowledge
    // the observation, nor remember it as clean merely because sync returned.
    QVERIFY(f.provider->mayHaveBackingDomainChanges());
    f.provider->acknowledgeBackingDomainChange(cold.physicalSlot, cold.revision - 1);
    QVERIFY(f.provider->mayHaveBackingDomainChanges());
    KisBackingBudgetDelta delta;
    delta.buckets[size_t(KisBackingBudgetClass::Current)].ssd = cold.bytes;
    auto reservation = budget.reserve(delta, &f.error);
    QVERIFY(reservation.isValid());
    QVERIFY2(ledger.registerBacking(handle, reservation,
        KisBackingBudgetClass::Current, &f.error), qPrintable(f.error));
    QVERIFY(ledger.synchronizeBackingDomains(&f.error));
    QVERIFY(!f.provider->mayHaveBackingDomainChanges());

    QVERIFY(tile->blockSwapping()); tile->unblockSwapping();
    QVERIFY(f.provider->mayHaveBackingDomainChanges());
    f.provider->acknowledgeBackingDomainChange(cold.physicalSlot, cold.revision);
    QVERIFY(f.provider->mayHaveBackingDomainChanges());
    QVERIFY(ledger.synchronizeBackingDomains(&f.error));
    QVERIFY(!f.provider->mayHaveBackingDomainChanges());
    const auto usage = budget.usage().buckets[size_t(KisBackingBudgetClass::Current)].live;
    QCOMPARE(usage.cpuRam, cold.bytes);
    QCOMPARE(usage.ssd, quint64(0));
    ledger.releaseRetiredBacking(handle);
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::ledgerProviderRegistrationPreparedBeforeAdmission()
{
    Fixture f; QVERIFY(f.init());
    KisPageBackingLimits limits; limits.metadataArenaBytes = 64 * 1024;
    KisBackingBudgetController budget(limits);
    auto ledger = std::make_unique<KisPageOwnerLedger>();
    QVERIFY(ledger->configure(f.completions)); ledger->attachBackingBudget(budget);
    auto first = budget.reserve({}, nullptr), second = budget.reserve({}, nullptr);
    QVERIFY(first.isValid() && second.isValid()); first.release(); second.release();
    const auto live = [&] { return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const auto baseline = live();
    size_t fillerBytes = size_t(limits.metadataArenaBytes - baseline);
    void *filler = kisAllocateMutationStorage(&budget, fillerBytes, 1);
    const auto cleanup = qScopeGuard([&] {
        f.provider->beforeDomainAdmission = {};
        kisFreeMutationStorage(&budget, filler, fillerBytes, 1);
    });
    const auto calls = f.provider->domainAdmissionCalls.load();
    QVERIFY(!ledger->registerProvider(f.provider, &f.error));
    QVERIFY(f.error.contains(QStringLiteral("registration storage")));
    QCOMPARE(f.provider->domainAdmissionCalls.load(), calls);
    QVERIFY(!ledger->provider(f.provider->providerId(), f.provider->providerEpoch()));
    QCOMPARE(live(), limits.metadataArenaBytes);
    kisFreeMutationStorage(&budget, std::exchange(filler, nullptr), fillerBytes, 1);

    bool pendingHidden = false, duplicateRejected = false;
    f.provider->beforeDomainAdmission = [&] {
        pendingHidden = !ledger->provider(f.provider->providerId(), f.provider->providerEpoch())
            && !ledger->providerFor(cpu);
        duplicateRejected = !ledger->registerProvider(f.provider);
    };
    f.provider->rejectDomainAdmission = true;
    QVERIFY(!ledger->registerProvider(f.provider, &f.error));
    QVERIFY(pendingHidden && duplicateRejected);
    QCOMPARE(live(), baseline); // The pending node was unlinked and freed.
    f.provider->rejectDomainAdmission = false;
    f.provider->beforeDomainAdmission = [] { throw std::bad_alloc(); };
    QVERIFY(!ledger->registerProvider(f.provider, &f.error));
    QCOMPARE(live(), baseline);
    QVERIFY(!ledger->providerFor(cpu));

    quint64 nodeBytes = 0;
    f.provider->beforeDomainAdmission = [&] {
        nodeBytes = live() - baseline;
        pendingHidden = !ledger->providerFor(cpu);
        fillerBytes = size_t(limits.metadataArenaBytes - live());
        filler = kisAllocateMutationStorage(&budget, fillerBytes, 1);
    };
    QVERIFY2(ledger->registerProvider(f.provider, &f.error), qPrintable(f.error));
    QVERIFY(nodeBytes > 0 && pendingHidden);
    QCOMPARE(live(), limits.metadataArenaBytes);
    // Registration accepted the same prepared node. Routing needs no list
    // allocation even with no remaining MetadataArena capacity.
    QVERIFY(ledger->providerFor(cpu) == f.provider);
    QVERIFY(ledger->provider(f.provider->providerId(), f.provider->providerEpoch()) == f.provider);
    QVERIFY(!ledger->registerProvider(f.provider));
    QCOMPARE(live(), limits.metadataArenaBytes);
    f.provider->beforeDomainAdmission = {};
    ledger.reset();
    QCOMPARE(live(), limits.metadataArenaBytes - nodeBytes);
    kisFreeMutationStorage(&budget, std::exchange(filler, nullptr), fillerBytes, 1);
    QCOMPARE(live(), baseline);
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::domainProviderSnapshotSurvivesRegistration()
{
    Fixture f;
    QVERIFY2(f.init(), qPrintable(f.error));
    KisBackingBudgetController budget;
    KisPageOwnerLedger ledger;
    QVERIFY(ledger.configure(f.completions, &f.error));
    ledger.attachBackingBudget(budget);
    QVERIFY(ledger.registerProvider(f.provider, &f.error));
    auto second = std::make_shared<TestProvider>();
    QVERIFY(second->p->configure({{191}, {1}, 1024 * 1024}, f.completions, &f.error));
    second->forceDomainJournalProbe = true;
    f.provider->forceDomainJournalProbe = true;
    bool registered = false;
    f.provider->beforeCapabilities = [&] { registered = ledger.registerProvider(second, &f.error); };
    QVERIFY(ledger.providerFor(cpu) == f.provider); // Later acceptance is outside this route cut.
    QVERIFY(registered);
    f.provider->beforeCapabilities = {};
    QVERIFY(!ledger.providerFor(cpu)); // The next cut sees both and rejects ambiguity.
    auto third = std::make_shared<TestProvider>();
    QVERIFY(third->p->configure({{192}, {1}, 1024 * 1024}, f.completions, &f.error));
    third->forceDomainJournalProbe = true;
    registered = false;
    f.provider->afterDomainSnapshot = [&] { registered = ledger.registerProvider(third, &f.error); };
    QVERIFY(ledger.synchronizeBackingDomains(&f.error));
    QVERIFY(registered);
    QCOMPARE(second->domainJournalCalls.load(), 1);
    QCOMPARE(third->domainJournalCalls.load(), 0);
    f.provider->afterDomainSnapshot = {};
    QVERIFY(ledger.synchronizeBackingDomains(&f.error));
    QCOMPARE(second->domainJournalCalls.load(), 2);
    QCOMPARE(third->domainJournalCalls.load(), 1);
    QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::domainJournalSynchronizationRejectsAtCapacity()
{
    QFETCH(bool, snapshotFits);
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto handle = f.provider->lastTarget;
    auto view = f.store->captureReadView(); auto read = view.readResidentPage(handle.version.key);
    auto *tile = f.provider->p->tileDataForCpuReadGuard(read); QVERIFY(tile && tile->ref());
    const auto releaseTile = qScopeGuard([&] { tile->deref(); }); read = {}; view = {};
    KisPageBackingLimits limits; limits.metadataArenaBytes = 64 * 1024;
    KisBackingBudgetController budget(limits);
    KisPageOwnerLedger ledger; QVERIFY(ledger.configure(f.completions)); ledger.attachBackingBudget(budget);
    QVERIFY(ledger.registerProvider(f.provider));
    KisBackingBudgetDelta delta; delta.buckets[size_t(KisBackingBudgetClass::Current)].cpuRam = handle.layout.byteSize;
    auto reservation = budget.reserve(delta, &f.error); QVERIFY(reservation.isValid());
    QVERIFY(ledger.registerBacking(handle, reservation, KisBackingBudgetClass::Current, &f.error));
    QVERIFY(KisTileDataStore::instance()->trySwapTileData(tile));
    const auto cold = f.provider->backingFootprint(handle);
    auto first = budget.reserve({}, nullptr), second = budget.reserve({}, nullptr);
    QVERIFY(first.isValid() && second.isValid()); first.release(); second.release();
    { const auto warm = f.provider->backingDomainChanges(&budget); QCOMPARE(warm.size(), size_t(1)); }
    const auto live = [&] { return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const auto before = live();
    quint64 snapshotBytes = 0;
    { const auto snapshot = f.provider->backingDomainChanges(&budget); snapshotBytes = live() - before; }
    QVERIFY(snapshotBytes > 0); QCOMPARE(live(), before);
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - live() - (snapshotFits ? snapshotBytes : 0));
    void *filler = kisAllocateMutationStorage(&budget, fillerBytes, 1);
    const auto release = qScopeGuard([&] { kisFreeMutationStorage(&budget, filler, fillerBytes, 1); });
    int captured = 0; f.provider->afterDomainSnapshot = [&] { ++captured; };
    const auto clearHook = qScopeGuard([&] { f.provider->afterDomainSnapshot = {}; });
    QVERIFY(!ledger.synchronizeBackingDomains(&f.error));
    QVERIFY(f.error.contains(QStringLiteral("storage")));
    QCOMPARE(captured, int(snapshotFits)); // Actual snapshot allocation vs actual probe allocation.
    QVERIFY(f.provider->mayHaveBackingDomainChanges());
    QCOMPARE(f.provider->backingFootprint(handle).revision, cold.revision);
    QVERIFY(!tile->isResident());
    kisFreeMutationStorage(&budget, std::exchange(filler, nullptr), fillerBytes, 1);
    QVERIFY2(ledger.synchronizeBackingDomains(&f.error), qPrintable(f.error));
    QVERIFY(!f.provider->mayHaveBackingDomainChanges());
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::Current)].live.ssd, handle.layout.byteSize);
    QVERIFY(tile->blockSwapping()); tile->unblockSwapping();
    QVERIFY(ledger.synchronizeBackingDomains(&f.error));
    ledger.releaseRetiredBacking(handle); QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::domainClaimPreparationRejectsBeforePhysicalMove()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto handle = f.provider->lastTarget;
    auto view = f.store->captureReadView(); auto read = view.readResidentPage(handle.version.key);
    auto *tile = f.provider->p->tileDataForCpuReadGuard(read); QVERIFY(tile && tile->ref());
    const auto releaseTile = qScopeGuard([&] { tile->deref(); }); read = {}; view = {};
    KisPageBackingLimits limits; limits.metadataArenaBytes = 64 * 1024;
    KisBackingBudgetController budget(limits);
    KisPageOwnerLedger ledger; QVERIFY(ledger.configure(f.completions)); ledger.attachBackingBudget(budget);
    QVERIFY(ledger.registerProvider(f.provider));
    KisBackingBudgetDelta delta; delta.buckets[size_t(KisBackingBudgetClass::Current)].cpuRam = handle.layout.byteSize;
    auto reservation = budget.reserve(delta, &f.error); QVERIFY(reservation.isValid());
    QVERIFY(ledger.registerBacking(handle, reservation, KisBackingBudgetClass::Current, &f.error));
    const auto original = f.provider->backingFootprint(handle);
    auto first = budget.reserve({}, nullptr), second = budget.reserve({}, nullptr);
    QVERIFY(first.isValid() && second.isValid()); first.release(); second.release();
    const auto live = [&] { return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - live());
    void *filler = kisAllocateMutationStorage(&budget, fillerBytes, 1);
    const auto release = qScopeGuard([&] { kisFreeMutationStorage(&budget, filler, fillerBytes, 1); });
    QVERIFY(!KisTileDataStore::instance()->trySwapTileData(tile));
    QVERIFY(tile->isResident()); QCOMPARE(f.provider->backingFootprint(handle).revision, original.revision);
    QVERIFY(!f.provider->mayHaveBackingDomainChanges());
    for (const auto &bucket : budget.usage().buckets) QCOMPARE(bucket.reserved.ssd, quint64(0));
    kisFreeMutationStorage(&budget, std::exchange(filler, nullptr), fillerBytes, 1);
    QVERIFY(KisTileDataStore::instance()->trySwapTileData(tile)); // No abandoned original transition/claim.
    QVERIFY(ledger.synchronizeBackingDomains(&f.error));
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::Current)].live.ssd, handle.layout.byteSize);
    QVERIFY(tile->blockSwapping()); tile->unblockSwapping();
    QVERIFY(ledger.synchronizeBackingDomains(&f.error));
    ledger.releaseRetiredBacking(handle); QVERIFY(f.store->closeSession());
}

void KisPageStoreCpuMutationTest::domainJournalCommitsAtCapacity()
{
    KisPageBackingLimits limits; limits.metadataArenaBytes = 64 * 1024;
    auto process = std::make_shared<KisBackingBudgetController>(limits);
    Fixture f; f.providerProcessBudget = process; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto handle = f.provider->lastTarget;
    auto view = f.store->captureReadView(); auto read = view.readResidentPage(handle.version.key);
    auto *tile = f.provider->p->tileDataForCpuReadGuard(read); QVERIFY(tile && tile->ref());
    const auto releaseTile = qScopeGuard([&] { tile->deref(); }); read = {}; view = {};
    struct CapacityObserver final : KisTileDataResidencyObserver {
        KisBackingBudgetController *budget;
        quint64 limit;
        size_t bytes = 0;
        void *filler = nullptr;
        bool committedAtCapacity = false;
        CapacityObserver(KisBackingBudgetController *owner, quint64 cap) : budget(owner), limit(cap) {}
        ~CapacityObserver() override { release(); }
        void release() { kisFreeMutationStorage(budget, std::exchange(filler, nullptr), bytes, 1); }
        struct Terminal final : KisTileDataResidencyTransition {
            CapacityObserver *owner;
            explicit Terminal(CapacityObserver *value) : owner(value) {}
            void commit(quint64) noexcept override {
                owner->committedAtCapacity = owner->budget->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam == owner->limit;
            }
        };
        std::shared_ptr<KisTileDataResidencyTransition> prepareResidencyChange(
            KisTileData *, const KisTileDataResidencyState &, bool, QString *) override try {
            auto terminal = std::make_shared<Terminal>(this);
            bytes = size_t(limit - budget->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam);
            filler = kisAllocateMutationStorage(budget, bytes, 1);
            return terminal;
        }
        catch (const std::bad_alloc &) { return {}; }
    };
    auto first = process->reserve({}, nullptr), second = process->reserve({}, nullptr);
    QVERIFY(first.isValid() && second.isValid()); first.release(); second.release();
    auto observer = std::make_shared<CapacityObserver>(process.get(), limits.metadataArenaBytes);
    auto *tiles = KisTileDataStore::instance();
    QVERIFY(tiles->registerResidencyObserver(tile, observer));
    const auto unregister = qScopeGuard([&] { tiles->unregisterResidencyObserver(tile, observer); });
    QVERIFY(tiles->trySwapTileData(tile));
    QVERIFY(observer->committedAtCapacity); QVERIFY(!tile->isResident());
    QVERIFY(f.provider->mayHaveBackingDomainChanges());
    QCOMPARE(f.provider->backingFootprint(handle).domain, KisPageAccessDomain::Ssd);
    KisBackingBudgetController budget;
    auto later = std::make_unique<KisPageOwnerLedger>(); QVERIFY(later->configure(f.completions)); later->attachBackingBudget(budget);
    // The accepted Transition is now charged and has already freed here.
    // Fill that returned capacity at the actual later registration boundary.
    const auto returnedBytes = size_t(limits.metadataArenaBytes -
        process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam);
    void *returned = kisAllocateMutationStorage(process.get(), returnedBytes, 1);
    auto releaseReturned = qScopeGuard([&] { kisFreeMutationStorage(process.get(), returned, returnedBytes, 1); });
    QVERIFY(!later->registerProvider(f.provider, &f.error)); // Actual admission-vector growth refused.
    kisFreeMutationStorage(process.get(), std::exchange(returned, nullptr), returnedBytes, 1);
    observer->release(); tiles->unregisterResidencyObserver(tile, observer);
    QVERIFY(later->registerProvider(f.provider, &f.error)); // Original pending registration was returned.
    auto cold = f.provider->backingDomainChanges(); QCOMPARE(cold.size(), size_t(1));
    QVERIFY(tile->blockSwapping()); tile->unblockSwapping();
    f.provider->acknowledgeBackingDomainChange(cold.front().physicalSlot, cold.front().revision);
    QVERIFY(f.provider->mayHaveBackingDomainChanges()); // Old ack cannot erase a newer original payload revision.
    QVERIFY(f.store->closeSession());
    later.reset(); f.store.reset(); f.provider.reset();
    const auto tail = process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    cold = KisReplicaBackingDomainChanges{}; // Snapshot storage survives its original provider accounting facade.
    QVERIFY(process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam < tail);
}

void KisPageStoreCpuMutationTest::backingDomainRejectsStaleSnapshot()
{
    Fixture f;
    QVERIFY2(f.init(), qPrintable(f.error));
    QVERIFY2(f.fill(0x31), qPrintable(f.error));
    const KisReplicaHandle handle = f.provider->lastTarget;
    QVERIFY(handle.isValid());

    auto view = f.store->captureReadView();
    auto read = view.readResidentPage(handle.version.key);
    QVERIFY(read.isValid());
    auto *tile = f.provider->p->tileDataForCpuReadGuard(read);
    QVERIFY(tile && tile->ref());
    read = {};
    view = {};

    KisBackingBudgetController budget;
    KisPageOwnerLedger ledger;
    QVERIFY(ledger.configure(f.completions, &f.error));
    ledger.attachBackingBudget(budget);
    QVERIFY(ledger.registerProvider(f.provider, &f.error));
    const auto initialFootprint = f.provider->backingFootprint(handle);
    QVERIFY(initialFootprint.isValid());
    KisBackingBudgetDelta delta;
    delta.buckets[size_t(KisBackingBudgetClass::Current)].cpuRam =
        handle.layout.byteSize;
    auto reservation = budget.reserve(delta, &f.error);
    QVERIFY2(reservation.isValid(), qPrintable(f.error));
    QVERIFY2(ledger.registerBacking(handle, reservation,
                                    KisBackingBudgetClass::Current, &f.error),
             qPrintable(f.error));

    auto *tileStore = KisTileDataStore::instance();
    QVector<KisBackingClassChange> preparedClassChange{
        {handle, KisBackingBudgetClass::Current,
         KisBackingBudgetClass::RetainedHistory}};
    auto classReservation = ledger.prepareBackingChanges(
        std::move(preparedClassChange), {}, &f.error);
    QVERIFY2(classReservation.isValid(), qPrintable(f.error));
    QVERIFY(!tileStore->trySwapTileData(tile));
    QVERIFY(tile->isResident());
    QVERIFY(f.provider->backingDomainChanges().empty());
    classReservation.release();

    KisBackingBudgetController racingBudget;
    KisPageOwnerLedger racingLedger;
    QVERIFY(racingLedger.configure(f.completions, &f.error));
    racingLedger.attachBackingBudget(racingBudget);
    QVERIFY(racingLedger.registerProvider(f.provider, &f.error));
    auto racingReservation = racingBudget.reserve(delta, &f.error);
    QVERIFY2(racingReservation.isValid(), qPrintable(f.error));
    QSemaphore footprintReady;
    QSemaphore resumeRegistration;
    std::atomic<bool> pauseFootprintOnce{true};
    f.provider->afterBackingFootprint = [&] {
        if (pauseFootprintOnce.exchange(false)) {
            footprintReady.release();
            resumeRegistration.acquire();
        }
    };
    bool racingRegistrationSucceeded = true;
    QString racingRegistrationError;
    std::thread racingRegistration([&] {
        racingRegistrationSucceeded = racingLedger.registerBacking(
            handle, racingReservation, KisBackingBudgetClass::Current,
            &racingRegistrationError);
    });
    const auto joinRacingRegistration = qScopeGuard([&] {
        resumeRegistration.release();
        if (racingRegistration.joinable()) racingRegistration.join();
        f.provider->afterBackingFootprint = {};
    });
    QVERIFY(footprintReady.tryAcquire(1, 5000));
    QVERIFY(tileStore->trySwapTileData(tile));
    resumeRegistration.release();
    racingRegistration.join();
    f.provider->afterBackingFootprint = {};
    QVERIFY(!racingRegistrationSucceeded);
    QVERIFY(racingRegistrationError.contains(QStringLiteral("reserved")));
    racingReservation.release();

    KisBackingBudgetDelta racingSsdDelta;
    racingSsdDelta.buckets[size_t(KisBackingBudgetClass::Current)].ssd =
        handle.layout.byteSize;
    auto racingSsdReservation = racingBudget.reserve(
        racingSsdDelta, &f.error);
    QVERIFY2(racingSsdReservation.isValid(), qPrintable(f.error));
    QVERIFY2(racingLedger.registerBacking(
                 handle, racingSsdReservation, KisBackingBudgetClass::Current,
                 &f.error),
             qPrintable(f.error));
    const auto coldChanges = f.provider->backingDomainChanges();
    QCOMPARE(coldChanges.size(), 1);
    QCOMPARE(kisCompareBackingRevision(coldChanges.front().revision,
                                       initialFootprint.revision),
             KisBackingRevisionOrder::Newer);

    QSemaphore snapshotReady;
    QSemaphore resumeOldSnapshot;
    std::atomic<bool> pauseOnce{true};
    f.provider->afterDomainSnapshot = [&] {
        if (pauseOnce.exchange(false)) {
            snapshotReady.release();
            resumeOldSnapshot.acquire();
        }
    };
    bool oldSyncSucceeded = false;
    QString oldSyncError;
    std::thread oldSync([&] {
        oldSyncSucceeded = ledger.synchronizeBackingDomains(&oldSyncError);
    });
    const auto joinOldSync = qScopeGuard([&] {
        resumeOldSnapshot.release();
        if (oldSync.joinable()) oldSync.join();
        f.provider->afterDomainSnapshot = {};
    });
    QVERIFY(snapshotReady.tryAcquire(1, 5000));

    QVERIFY(tile->blockSwapping());
    QVERIFY(tile->data());
    tile->unblockSwapping();
    const auto residentChanges = f.provider->backingDomainChanges();
    QCOMPARE(residentChanges.size(), 1);
    QCOMPARE(residentChanges.front().domain, KisPageAccessDomain::CpuRam);
    QCOMPARE(kisCompareBackingRevision(residentChanges.front().revision,
                                       coldChanges.front().revision),
             KisBackingRevisionOrder::Newer);
    QVERIFY2(ledger.synchronizeBackingDomains(&f.error), qPrintable(f.error));

    resumeOldSnapshot.release();
    oldSync.join();
    QVERIFY2(oldSyncSucceeded, qPrintable(oldSyncError));
    f.provider->afterDomainSnapshot = {};

    const auto actual = f.provider->backingFootprint(handle);
    QVERIFY(actual.isValid());
    QCOMPARE(actual.domain, KisPageAccessDomain::CpuRam);
    QCOMPARE(actual.revision, residentChanges.front().revision);
    auto usage = budget.usage()
        .buckets[size_t(KisBackingBudgetClass::Current)].live;
    QCOMPARE(usage.cpuRam, handle.layout.byteSize);
    QCOMPARE(usage.ssd, quint64(0));
    QVERIFY(f.provider->backingDomainChanges().empty());

    QVERIFY2(ledger.synchronizeBackingDomains(&f.error), qPrintable(f.error));
    usage = budget.usage()
        .buckets[size_t(KisBackingBudgetClass::Current)].live;
    QCOMPARE(usage.cpuRam, handle.layout.byteSize);
    QCOMPARE(usage.ssd, quint64(0));

    // The journal is verification only. A mutable owner that was not
    // registered with the provider cannot approve an already completed move.
    KisBackingBudgetController unadmittedBudget;
    KisPageOwnerLedger unadmittedLedger;
    QVERIFY(unadmittedLedger.configure(f.completions, &f.error));
    unadmittedLedger.attachBackingBudget(unadmittedBudget);
    f.provider->forwardDomainAdmission = false;
    QVERIFY(unadmittedLedger.registerProvider(f.provider, &f.error));
    f.provider->forwardDomainAdmission = true;
    auto unadmittedReservation = unadmittedBudget.reserve(delta, &f.error);
    QVERIFY2(unadmittedReservation.isValid(), qPrintable(f.error));
    QVERIFY2(unadmittedLedger.registerBacking(
                 handle, unadmittedReservation,
                 KisBackingBudgetClass::Current, &f.error),
             qPrintable(f.error));
    QVERIFY(tileStore->trySwapTileData(tile));
    QVERIFY(!unadmittedLedger.synchronizeBackingDomains(&f.error));
    QVERIFY(f.error.contains(QStringLiteral("unadmitted")));
    unadmittedLedger.releaseRetiredBacking(handle);
    QVERIFY2(racingLedger.synchronizeBackingDomains(&f.error),
             qPrintable(f.error));
    QVERIFY(tile->blockSwapping());
    tile->unblockSwapping();
    QVERIFY2(ledger.synchronizeBackingDomains(&f.error), qPrintable(f.error));

    racingLedger.releaseRetiredBacking(handle);
    ledger.releaseRetiredBacking(handle);
    tile->deref();
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
}

void KisPageStoreCpuMutationTest::storeRootStorageRefusal()
{
    QFETCH(bool, firstAllocation);
    KisPageBackingLimits limits;
    limits.metadataArenaBytes = 64 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    auto storage = KisMutationStorageAllocator<char>::retained(parent.get());
    std::array<KisBackingBudgetReservation, 8> warm;
    for (auto &slot : warm) { slot = parent->reserve({}, nullptr); QVERIFY(slot.isValid()); }
    for (auto &slot : warm) slot.release();
    const auto live = [&] {
        return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const auto baseline = live();
    quint64 required = 0;
    {
        QString error;
        auto measured = KisPageStore::prepareStorage(parent, &error);
        QVERIFY2(measured, qPrintable(error));
        required = live() - baseline;
        QVERIFY(required > sizeof(KisPageStore));
        QVERIFY(!measured->isOperational());
    }
    QCOMPARE(live(), baseline);
    qInfo() << "BR1_STORE_ROOT_STORAGE_BYTES" << required;
    const quint64 headroom = firstAllocation ? 1 : required - 1;
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - baseline - headroom);
    auto *filler = storage.allocate(fillerBytes);
    auto releaseFiller = qScopeGuard([&] { storage.deallocate(filler, fillerBytes); });
    const auto filled = live();
    for (int attempt = 0; attempt < 3; ++attempt) {
        QString error;
        auto rejected = KisPageStore::prepareStorage(parent, &error);
        QVERIFY(!rejected);
        QVERIFY(!error.isEmpty());
        QCOMPARE(live(), filled);
        for (const auto &bucket : parent->usage().buckets)
            QCOMPARE(bucket.reserved.cpuRam, quint64(0));
    }
    releaseFiller.dismiss();
    storage.deallocate(filler, fillerBytes);
    QCOMPARE(live(), baseline);
    auto retried = KisPageStore::prepareStorage(parent, nullptr);
    QVERIFY(retried);
    QCOMPARE(live(), baseline + required);
    retried.reset();
    QCOMPARE(live(), baseline);
}

void KisPageStoreCpuMutationTest::storeRootStorageRetained()
{
    QFETCH(bool, captured);
    KisPageBackingLimits limits;
    limits.metadataArenaBytes = 1024 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    auto storage = KisMutationStorageAllocator<char>::retained(parent.get());
    std::array<KisBackingBudgetReservation, 8> warm;
    for (auto &slot : warm) { slot = parent->reserve({}, nullptr); QVERIFY(slot.isValid()); }
    for (auto &slot : warm) slot.release();
    const auto live = [&] {
        return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const auto baseline = live();
    QString error;
    auto store = KisPageStore::prepareStorage(parent, &error);
    QVERIFY2(store, qPrintable(error));
    KisCapturedReadView view;
    if (captured) {
        QVERIFY(store->configureSharedNonPayloadBudget(parent, &error));
        QVERIFY(store->configureBackingLimits(limits, &error));
        auto completions = std::make_shared<KisCompletionRegistry>();
        KisImageEpochSnapshot initial;
        initial.epoch = {1};
        initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
        QVERIFY2(store->configure(initial, completions, 1, &error), qPrintable(error));
        QVERIFY2(store->finalizeInitialization(&error), qPrintable(error));
        view = store->captureReadView();
        QVERIFY(view.isValid());
        store.reset();
    }
    const std::weak_ptr<KisBackingBudgetController> lifetime(parent);
    parent.reset();
    QVERIFY(!lifetime.expired());
    parent = lifetime.lock();
    QVERIFY(parent);
    QVERIFY(live() > baseline);
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - live());
    auto *filler = storage.allocate(fillerBytes);
    auto releaseFiller = qScopeGuard([&] { storage.deallocate(filler, fillerBytes); });
    QCOMPARE(live(), limits.metadataArenaBytes);
    if (captured) view = {};
    else store.reset();
    kisDrainPageStoreReclamation();
    QCOMPARE(live(), baseline + fillerBytes);
    for (const auto &bucket : parent->usage().buckets)
        QCOMPARE(bucket.reserved.cpuRam, quint64(0));
    releaseFiller.dismiss();
    storage.deallocate(filler, fillerBytes);
    QCOMPARE(live(), baseline);
    parent.reset();
    QVERIFY(lifetime.expired());
}

void KisPageStoreCpuMutationTest::domainAdmissionStorageRetained()
{
    QFETCH(bool, keepReservation);
    KisPageBackingLimits limits;
    limits.metadataArenaBytes = 1024 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    auto storage = KisMutationStorageAllocator<char>::retained(parent.get());
    std::array<KisBackingBudgetReservation, 8> warm;
    for (auto &slot : warm) { slot = parent->reserve({}, nullptr); QVERIFY(slot.isValid()); }
    for (auto &slot : warm) slot.release();
    const auto live = [&] {
        return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const auto baseline = live();
    QString error;
    auto store = KisPageStore::prepareStorage(parent, &error);
    QVERIFY2(store, qPrintable(error));
    QVERIFY(store->configureSharedNonPayloadBudget(parent, &error));
    QVERIFY(store->configureBackingLimits(limits, &error));
    auto completions = std::make_shared<KisCompletionRegistry>();
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    QVERIFY2(store->configure(initial, completions, 1, &error), qPrintable(error));
    auto provider = std::make_shared<TestProvider>();
    KisCpuResidentReplicaProviderConfig config;
    config.provider = {190}; config.providerEpoch = {1}; config.budgetBytes = 64 * 1024 * 1024;
    QVERIFY2(provider->p->configure(config, completions, &error), qPrintable(error));
    std::shared_ptr<KisReplicaBackingDomainAdmission> admission;
    provider->domainAdmissionCapture = &admission;
    QVERIFY(store->registerReplicaProvider(provider));
    QVERIFY2(store->finalizeInitialization(&error), qPrintable(error));
    QVERIFY(admission);
    std::weak_ptr<KisReplicaBackingDomainAdmission> weak(admission);
    const auto prepare = [&] {
        return admission->prepare({{190}, {1}, 1}, 4096,
                                  KisPageAccessDomain::CpuRam, 1, KisPageAccessDomain::Ssd, &error);
    };
    // This owner has no physical record. Its receipt aliases the paid root,
    // keeping the same strong lifetime without return storage or admission.
    const auto before = live();
    auto reservation = prepare(); QVERIFY2(reservation, qPrintable(error));
    QCOMPARE(live(), before);
    qInfo() << "BR1_DOMAIN_NOOP_STORAGE_BYTES" << live() - before;
    size_t fillerBytes = size_t(limits.metadataArenaBytes - live());
    auto *filler = storage.allocate(fillerBytes);
    auto releaseFiller = qScopeGuard([&] { if (filler) storage.deallocate(filler, fillerBytes); });
    const auto filled = live();
    for (int attempt = 0; attempt < 3; ++attempt) {
        QVERIFY2(prepare(), qPrintable(error)); QVERIFY(error.isEmpty()); QCOMPARE(live(), filled);
        for (const auto &bucket : parent->usage().buckets) QCOMPARE(bucket.reserved.cpuRam, quint64(0));
    }
    storage.deallocate(std::exchange(filler, nullptr), fillerBytes);
    QCOMPARE(live(), before);
    if (!keepReservation) reservation.reset();
    const std::weak_ptr<KisBackingBudgetController> lifetime(parent);
    parent.reset(); QVERIFY(!lifetime.expired());
    parent = lifetime.lock(); QVERIFY(parent);
    store.reset(); kisDrainPageStoreReclamation();
    QVERIFY(!weak.expired());
    fillerBytes = size_t(limits.metadataArenaBytes - live());
    filler = storage.allocate(fillerBytes);
    QCOMPARE(live(), limits.metadataArenaBytes);
    // A detached authority rejects before allocation even at full capacity.
    QVERIFY(!prepare()); QCOMPARE(live(), limits.metadataArenaBytes);
    admission.reset();
    QCOMPARE(weak.expired(), !keepReservation);
    reservation.reset();
    QVERIFY(weak.expired());
    QVERIFY(live() > baseline + fillerBytes); // Provider and test still hold the paid weak control.
    provider.reset();
    QVERIFY(live() > baseline + fillerBytes);
    weak.reset();
    QCOMPARE(live(), baseline + fillerBytes);
    storage.deallocate(std::exchange(filler, nullptr), fillerBytes);
    QCOMPARE(live(), baseline);
    parent.reset(); QVERIFY(lifetime.expired());
}

void KisPageStoreCpuMutationTest::domainReservationControlRetained()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto handle = f.provider->lastTarget;
    const auto footprint = f.provider->backingFootprint(handle);
    QVERIFY(footprint.isValid());
    KisPageBackingLimits limits; limits.metadataArenaBytes = 1024 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    auto storage = KisMutationStorageAllocator<char>::retained(parent.get());
    std::array<KisBackingBudgetReservation, 8> warm;
    for (auto &slot : warm) { slot = parent->reserve({}, nullptr); QVERIFY(slot.isValid()); }
    for (auto &slot : warm) slot.release();
    const auto live = [&] {
        return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const auto baseline = live();
    std::unique_ptr<KisPageOwnerLedger> ledger;
    {
        KisBackingBudgetController cold;
        QVERIFY(cold.configureSharedNonPayloadBudget(parent, &f.error));
        // Exercise the same private constructor as product Store composition.
        ledger.reset(new KisPageOwnerLedger(KisMutationStorageAllocator<KisPageOwnerLedger>::retained(&cold)));
    }
    qInfo() << "BR1_LEDGER_CONTROL_STORAGE_BYTES" << live() - baseline;
    auto budget = std::make_unique<KisBackingBudgetController>(limits);
    QVERIFY(budget->configureSharedNonPayloadBudget(parent, &f.error));
    QVERIFY(ledger->configure(f.completions)); ledger->attachBackingBudget(*budget);
    std::shared_ptr<KisReplicaBackingDomainAdmission> admission;
    f.provider->domainAdmissionCapture = &admission;
    QVERIFY(ledger->registerProvider(f.provider)); QVERIFY(admission);
    KisBackingBudgetDelta delta;
    delta.buckets[size_t(KisBackingBudgetClass::Current)].cpuRam = handle.layout.byteSize;
    auto charge = budget->reserve(delta, &f.error); QVERIFY(charge.isValid());
    QVERIFY(ledger->registerBacking(handle, charge, KisBackingBudgetClass::Current, &f.error));
    const auto prepare = [&] {
        return admission->prepare({handle.provider, handle.providerEpoch, footprint.physicalSlot}, footprint.bytes,
                                  footprint.domain, footprint.revision, KisPageAccessDomain::Ssd, &f.error);
    };
    auto reservation = prepare(); QVERIFY2(reservation, qPrintable(f.error)); reservation.reset();
    const auto before = live();
    reservation = prepare(); QVERIFY2(reservation, qPrintable(f.error));
    const quint64 required = live() - before;
    QVERIFY(required > 0);
    qInfo() << "BR1_DOMAIN_CLAIM_STORAGE_BYTES" << required;
    reservation.reset(); QCOMPARE(live(), before);
    size_t fillerBytes = size_t(limits.metadataArenaBytes - live() - required + 1);
    auto *filler = storage.allocate(fillerBytes);
    const auto releaseFiller = qScopeGuard([&] { if (filler) storage.deallocate(filler, fillerBytes); });
    const auto filled = live();
    for (int attempt = 0; attempt < 3; ++attempt) {
        QVERIFY(!prepare()); QVERIFY(!f.error.isEmpty()); QCOMPARE(live(), filled);
        QCOMPARE(f.provider->backingFootprint(handle).revision, footprint.revision);
        QCOMPARE(f.provider->backingFootprint(handle).domain, footprint.domain);
        for (const auto &bucket : parent->usage().buckets) QCOMPARE(bucket.reserved.ssd, quint64(0));
    }
    storage.deallocate(std::exchange(filler, nullptr), fillerBytes);
    reservation = prepare(); QVERIFY2(reservation, qPrintable(f.error));
    QCOMPARE(live(), before + required);
    std::weak_ptr<KisReplicaBackingDomainReservation> weak(reservation);
    reservation.reset(); // Cancel the real claim, leaving only return/control storage.
    QVERIFY(weak.expired());
    const quint64 controlBytes = live() - before;
    QVERIFY(controlBytes > sizeof(reservation));
    qInfo() << "BR1_DOMAIN_RESERVATION_STORAGE_BYTES" << controlBytes;
    ledger->releaseRetiredBacking(handle);
    ledger.reset(); admission.reset(); budget.reset();
    f.store.reset(); kisDrainPageStoreReclamation(); f.provider.reset();
    const std::weak_ptr<KisBackingBudgetController> lifetime(parent);
    parent.reset(); QVERIFY(!lifetime.expired());
    parent = lifetime.lock(); QVERIFY(parent);
    fillerBytes = size_t(limits.metadataArenaBytes - live());
    filler = storage.allocate(fillerBytes);
    QCOMPARE(live(), limits.metadataArenaBytes);
    weak.reset();
    QCOMPARE(live(), baseline + fillerBytes);
    storage.deallocate(std::exchange(filler, nullptr), fillerBytes);
    QCOMPARE(live(), baseline);
    parent.reset(); QVERIFY(lifetime.expired());
}

void KisPageStoreCpuMutationTest::metadataArenaBudgetTracksAllocator()
{
    // Even an empty Store owns finite shard/arena directories. A zero limit
    // must reject configuration before any metadata allocation escapes.
    {
        Fixture f;
        KisPageBackingLimits limits;
        limits.metadataArenaBytes = 0;
        QVERIFY(f.store->configureBackingLimits(limits, &f.error));
        QVERIFY(!f.init());
        QVERIFY(f.error.contains(QStringLiteral("budget")));
        const auto usage = f.store->backingUsage()
            .buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        QCOMPARE(usage.live.cpuRam, quint64(0));
        QCOMPARE(usage.reserved.cpuRam, quint64(0));
    }

    quint64 directoryBytes = 0;
    quint64 scopeBytes = 0;
    {
        Fixture probe;
        QVERIFY2(probe.init(), qPrintable(probe.error));
        directoryBytes = probe.store->backingUsage()
            .buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
        QVERIFY(directoryBytes > 0);
        const auto tx = probe.store->beginCurrentTransaction();
        auto scope = probe.begin(tx); QVERIFY(scope.isActive());
        scopeBytes = probe.store->backingUsage()
            .buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam - directoryBytes;
        QVERIFY(scopeBytes > 0);
        QVERIFY(scope.cancel()); scope = {};
        QVERIFY(probe.store->abort(tx));
        QVERIFY2(probe.store->closeSession(&probe.error), qPrintable(probe.error));
    }

    // Admit the real session/control block, but leave no room for the first
    // metadata arena block. Test that allocation boundary rather than the
    // earlier (separately tested) scope-admission rejection.
    {
        Fixture f;
        KisPageBackingLimits limits;
        limits.metadataArenaBytes = directoryBytes + scopeBytes;
        QVERIFY(f.store->configureBackingLimits(limits, &f.error));
        QVERIFY2(f.init(), qPrintable(f.error));
        const auto tx = f.store->beginCurrentTransaction();
        auto mutation = f.begin(tx);
        QVERIFY(mutation.isActive());
        auto guard = mutation.beginWrite(
            key(), KisPageWriteMode::DiscardContents, &f.error);
        QVERIFY(!guard.isValid());
        QVERIFY(f.error.contains(QStringLiteral("budget")));
        QVERIFY(mutation.cancel());
        mutation = {}; // scope/control-block charge lasts through actual free
        QVERIFY(f.store->abort(tx));
        QVERIFY(f.store->waitForRetirementIdle()); // The original finished record frees in collection.
        const auto usage = f.store->backingUsage()
            .buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        QCOMPARE(usage.live.cpuRam, directoryBytes);
        QCOMPARE(usage.reserved.cpuRam, quint64(0));
        QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    }

    // A successful write converts real block reservations into live usage.
    {
        Fixture f;
        QVERIFY2(f.init(), qPrintable(f.error));
        const quint64 before = f.store->backingUsage()
            .buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
        QVERIFY2(f.fill(0x31), qPrintable(f.error));
        const auto usage = f.store->backingUsage()
            .buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        QVERIFY(usage.live.cpuRam > before);
        QCOMPARE(usage.reserved.cpuRam, quint64(0));
        QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    }
}

void KisPageStoreCpuMutationTest::finiteBackingLimitMatrix()
{
    constexpr quint64 pageBytes = 64 * 64 * 4;

    // Current: a two-page publication is rejected atomically at a one-page
    // logical/resident limit, then abort releases every Pending/Debt escrow.
    {
        Fixture f;
        KisPageBackingLimits limits;
        limits.logicalCurrentBytes = pageBytes;
        limits.residentCurrentBytes.cpuRam = pageBytes;
        QVERIFY(f.store->configureBackingLimits(limits));
        QVERIFY(f.init());
        const auto tx = f.store->beginCurrentTransaction();
        auto mutation = f.begin(tx);
        for (int x : {0, 1}) {
            auto guard = mutation.beginWrite(key(x), KisPageWriteMode::DiscardContents, &f.error);
            QVERIFY2(guard.isValid(), qPrintable(f.error));
            static_cast<quint8 *>(guard.data())[0] = quint8(0x50 + x);
        }
        QVERIFY2(mutation.seal(&f.error), qPrintable(f.error));
        QVERIFY(!f.store->commit(tx, f.store->preparedPages(tx)).isValid());
        QVERIFY(f.store->abort(tx));
        QVERIFY(f.store->waitForRetirementIdle());
        const auto usage = f.store->backingUsage();
        QCOMPARE(usage.buckets[size_t(KisBackingBudgetClass::Current)].live.cpuRam, quint64(0));
        QCOMPARE(usage.buckets[size_t(KisBackingBudgetClass::ActivePending)].live.cpuRam, quint64(0));
        QCOMPARE(usage.buckets[size_t(KisBackingBudgetClass::RetirementDebt)].live.cpuRam, quint64(0));
        QVERIFY(usage.backpressureCount > 0);
        QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    }

    // Pending: the second request cannot escape when the first request owns
    // the only pending slot; cancelling the first leaves a closable Store.
    {
        Fixture f;
        KisPageBackingLimits limits;
        limits.activePendingBytes = pageBytes;
        QVERIFY(f.store->configureBackingLimits(limits));
        QVERIFY(f.init());
        const auto tx = f.store->beginCurrentTransaction();
        const auto first = f.store->acquireWrite(
            tx, key(), cpu, KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
        QVERIFY2(first.isValid(), qPrintable(first.error));
        const auto second = f.store->acquireWrite(
            tx, key(1), cpu, KisPageWriteMode::DiscardContents, KisPagePriority::Normal);
        QVERIFY(!second.isValid());
        QVERIFY(f.store->cancel(first));
        QVERIFY(f.store->abort(tx));
        QVERIFY(f.store->waitForRetirementIdle());
        QCOMPARE(f.store->backingUsage()
                     .buckets[size_t(KisBackingBudgetClass::ActivePending)].live.cpuRam,
                 quint64(0));
        QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    }

    // Optional cache: a default-page buffer larger than the hard limit is a
    // clean ResourceUnavailable result, not an unbudgeted allocation.
    {
        Fixture f;
        KisPageBackingLimits limits;
        limits.optionalCacheBytes = pageBytes - 1;
        QVERIFY(f.store->configureBackingLimits(limits));
        QVERIFY(f.init());
        auto view = f.store->captureReadView();
        KisCpuResidentReadStatus status = KisCpuResidentReadStatus::Ready;
        QVERIFY(!view.readResidentPage(key(), &status).isValid());
        QCOMPARE(status, KisCpuResidentReadStatus::ResourceUnavailable);
        const auto usage = f.store->backingUsage();
        QCOMPARE(usage.buckets[size_t(KisBackingBudgetClass::OptionalCache)].live.cpuRam,
                 quint64(0));
        QVERIFY(usage.backpressureCount > 0);
        view = {};
        QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    }

    // A domain move reserves its destination domain but has zero logical
    // growth. An exactly full Current bucket must therefore still swap out.
    {
        Fixture f;
        KisPageBackingLimits limits;
        limits.logicalCurrentBytes = pageBytes;
        limits.residentCurrentBytes.cpuRam = pageBytes;
        limits.residentCurrentBytes.ssd = pageBytes;
        limits.durableStoreCapacity = pageBytes;
        QVERIFY(f.store->configureBackingLimits(limits));
        QVERIFY(f.init());
        const auto tx = f.store->beginCurrentTransaction();
        auto mutation = f.store->beginMutation(tx, &f.error);
        auto guard = mutation.beginWrite(
            key(), KisPageWriteMode::DiscardContents, &f.error);
        QVERIFY2(guard.isValid(), qPrintable(f.error));
        std::memset(guard.data(), 0x31, size_t(guard.byteSize()));
        guard = {};
        QVERIFY2(mutation.seal(&f.error), qPrintable(f.error));
        QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
        QVERIFY(f.store->waitForRetirementIdle());

        auto view = f.store->captureReadView();
        auto read = view.readResidentPage(key());
        QVERIFY(read.isValid());
        auto *tile = f.provider->p->tileDataForCpuReadGuard(read);
        QVERIFY(tile && tile->ref());
        read = {};
        view = {};
        QVERIFY(KisTileDataStore::instance()->trySwapTileData(tile));
        QVERIFY(!tile->isResident());
        const auto usage = f.store->backingUsage()
            .buckets[size_t(KisBackingBudgetClass::Current)].live;
        QCOMPARE(usage.cpuRam, quint64(0));
        QCOMPARE(usage.ssd, pageBytes);
        tile->deref();
        QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    }

    // SSD: target space is admitted before tiles3 changes residency. A zero
    // durable limit keeps the RAM source valid and does not poison unrelated
    // owner admission with a post-facto journal entry.
    {
        Fixture f;
        KisPageBackingLimits limits;
        limits.durableStoreCapacity = 0;
        QVERIFY(f.store->configureBackingLimits(limits));
        QVERIFY(f.init());
        QVERIFY(f.fill(0x31));
        auto view = f.store->captureReadView();
        auto read = view.readResidentPage(key());
        QVERIFY(read.isValid());
        auto *tile = f.provider->p->tileDataForCpuReadGuard(read);
        QVERIFY(tile && tile->ref());
        const auto release = qScopeGuard([&] { tile->deref(); });
        read = {};
        view = {};
        auto *tileStore = KisTileDataStore::instance();
        const bool swapped = tileStore->trySwapTileData(tile);
        QVERIFY(!swapped);
        QVERIFY(tile->isResident());
        QVERIFY(f.store->backingUsage().backpressureCount > 0);
        QVERIFY(f.provider->backingDomainChanges().empty());
        const auto domainUsage = f.store->backingUsage()
            .buckets[size_t(KisBackingBudgetClass::Current)].live;
        QCOMPARE(domainUsage.cpuRam, 2 * pageBytes);
        QCOMPARE(domainUsage.ssd, quint64(0));

        const auto retryTx = f.store->beginCurrentTransaction();
        auto retry = f.store->beginMutation(retryTx);
        auto guard = retry.beginWrite(key(), KisPageWriteMode::DiscardContents);
        QVERIFY(guard.isValid());
        guard = {};
        QVERIFY(retry.cancel());
        QVERIFY(f.store->abort(retryTx));
        QVERIFY2(f.store->closeSession(), "finite SSD recovery did not drain");
    }

    // RAM: after a current page moves to SSD, another current page may use the
    // only resident slot. Faulting the first page back then fails before the
    // swap chunk is consumed, so the valid SSD source remains intact.
    {
        Fixture f;
        KisPageBackingLimits limits;
        limits.residentCurrentBytes.cpuRam = pageBytes;
        limits.durableStoreCapacity = pageBytes;
        QVERIFY(f.store->configureBackingLimits(limits));
        QVERIFY(f.init());
        const auto writePage = [&](const KisPageKey &page, quint8 value) {
            const auto tx = f.store->beginCurrentTransaction();
            auto mutation = f.store->beginMutation(tx, &f.error);
            auto guard = mutation.beginWrite(
                page, KisPageWriteMode::DiscardContents, &f.error);
            if (!guard.isValid()) return false;
            std::memset(guard.data(), value, size_t(guard.byteSize()));
            guard = {};
            return mutation.seal(&f.error)
                && f.store->commit(tx, f.store->preparedPages(tx)).isValid()
                && f.store->waitForRetirementIdle();
        };
        QVERIFY2(writePage(key(), 0x31), qPrintable(f.error));
        auto view = f.store->captureReadView();
        auto read = view.readResidentPage(key());
        QVERIFY(read.isValid());
        auto *tile = f.provider->p->tileDataForCpuReadGuard(read);
        QVERIFY(tile && tile->ref());
        const auto release = qScopeGuard([&] { tile->deref(); });
        read = {};
        view = {};
        QVERIFY(KisTileDataStore::instance()->trySwapTileData(tile));
        QVERIFY(!tile->isResident());
        QVERIFY2(writePage(key(1), 0x51), qPrintable(f.error));

        QVERIFY(!tile->blockSwapping());
        QVERIFY(!tile->isResident());
        const auto usage = f.store->backingUsage()
            .buckets[size_t(KisBackingBudgetClass::Current)].live;
        QCOMPARE(usage.cpuRam, pageBytes);
        QCOMPARE(usage.ssd, pageBytes);
        QVERIFY(f.store->backingUsage().backpressureCount > 0);
        QVERIFY(f.provider->backingDomainChanges().empty());
        QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    }
}

void KisPageStoreCpuMutationTest::swapInFailurePreservesDomainState()
{
    QFETCH(int, failurePoint);
    constexpr quint64 pageBytes = 64 * 64 * 4;
    Fixture f;
    KisPageBackingLimits limits;
    limits.logicalCurrentBytes = pageBytes;
    limits.residentCurrentBytes.cpuRam = pageBytes;
    limits.residentCurrentBytes.ssd = pageBytes;
    limits.durableStoreCapacity = pageBytes;
    QVERIFY(f.store->configureBackingLimits(limits));
    QVERIFY2(f.init(), qPrintable(f.error));

    const auto tx = f.store->beginCurrentTransaction();
    auto mutation = f.store->beginMutation(tx, &f.error);
    auto guard = mutation.beginWrite(
        key(), KisPageWriteMode::DiscardContents, &f.error);
    QVERIFY2(guard.isValid(), qPrintable(f.error));
    std::memset(guard.data(), 0x63, size_t(guard.byteSize()));
    guard = {};
    QVERIFY2(mutation.seal(&f.error), qPrintable(f.error));
    QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    QVERIFY(f.store->waitForRetirementIdle());

    auto view = f.store->captureReadView();
    auto read = view.readResidentPage(key());
    QVERIFY(read.isValid());
    const KisReplicaHandle replica = f.provider->lastTarget;
    QVERIFY(replica.isValid());
    auto *tile = f.provider->p->tileDataForCpuReadGuard(read);
    QVERIFY(tile && tile->ref());
    const auto release = qScopeGuard([&] { tile->deref(); });
    read = {};
    view = {};
    auto *tileStore = KisTileDataStore::instance();
    QVERIFY(tileStore->trySwapTileData(tile));
    QVERIFY(!tile->isResident());

    const auto footprint = f.provider->backingFootprint(replica);
    QCOMPARE(footprint.domain, KisPageAccessDomain::Ssd);
    QVERIFY(footprint.revision != 0);
    const auto swapChanges = f.provider->backingDomainChanges();
    QVERIFY(!swapChanges.empty());
    for (const auto &change : swapChanges) {
        f.provider->acknowledgeBackingDomainChange(
            change.physicalSlot, change.revision);
    }
    QVERIFY(f.provider->backingDomainChanges().empty());
    const quint64 chunkBegin = tile->swapChunk().begin();
    const quint64 chunkSize = tile->swapChunk().size();
    const auto usageBefore = f.store->backingUsage();
    KisTileDataStoreTestAccess::failNextSwapIn(
        KisSwapInFailurePoint(failurePoint));
    QVERIFY(!tile->blockSwapping());
    QVERIFY(!tile->isResident());
    QCOMPARE(tile->swapChunk().begin(), chunkBegin);
    QCOMPARE(tile->swapChunk().size(), chunkSize);
    const auto footprintAfter = f.provider->backingFootprint(replica);
    QCOMPARE(footprintAfter.domain, footprint.domain);
    QCOMPARE(footprintAfter.revision, footprint.revision);
    QCOMPARE(footprintAfter.physicalSlot, footprint.physicalSlot);
    QVERIFY(f.provider->backingDomainChanges().empty());

    const auto usageAfter = f.store->backingUsage();
    const auto currentBefore =
        usageBefore.buckets[size_t(KisBackingBudgetClass::Current)];
    const auto currentAfter =
        usageAfter.buckets[size_t(KisBackingBudgetClass::Current)];
    QCOMPARE(currentAfter.live.cpuRam, currentBefore.live.cpuRam);
    QCOMPARE(currentAfter.live.ssd, currentBefore.live.ssd);
    QCOMPARE(currentAfter.reserved.cpuRam, quint64(0));
    QCOMPARE(currentAfter.reserved.ssd, quint64(0));

    QVERIFY(tile->blockSwapping());
    QCOMPARE(tile->data()[0], quint8(0x63));
    tile->unblockSwapping();
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
}

void KisPageStoreCpuMutationTest::staleFirstWriteKeepsCurrentDefault()
{
    QFETCH(int, route);
    Fixture f; QVERIFY(f.init());
    const auto tx = f.store->beginCurrentTransaction();
    QVERIFY(f.setDefault(0x57));
    QCOMPARE(f.store->sessionStats().registeredPages, qsizetype(0));
    auto segment = route == 1 ? KisPageMutationSession{} : f.begin(tx);
    if (route == 1) {
        const auto request = f.store->acquireWrite(tx, key(), cpu, KisPageWriteMode::PreserveContents, KisPagePriority::Normal);
        QVERIFY(!request.isValid());
        QVERIFY(request.error.contains(QStringLiteral("write base is not visible")));
    } else if (route == 0) {
        QVERIFY(!segment.beginWrite(key()).isValid());
        QVERIFY(segment.cancel());
    } else {
        KisPageReadView overlay; overlay.kind = KisPageReadViewKind::TransactionOverlay; overlay.transaction = tx.id;
        KisSurfaceEpochState surface; QVERIFY(f.store->resolveSurfaceState({1}, overlay, &surface));
        auto *tile = KisTileDataStore::instance()->createDefaultTileData(f.bpp,
            reinterpret_cast<const quint8 *>(surface.format.defaultPixel.constData()));
        tile->acquire(); const auto cleanup = qScopeGuard([&] { tile->release(); });
        auto source = f.provider->p->captureCompletedTileSource(surface.allocationDescriptor(), tile); QVERIFY(source);
        QVERIFY(segment.aliasPage(key(), source)); QVERIFY(!segment.seal());
        QVERIFY(segment.cancel());
    }
    // A stale first writer obeys the same rejection as an already registered
    // page: its old default must not become Published to bypass the conflict.
    QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(2));
    QCOMPARE(f.store->sessionStats().activeControlWritePages, qsizetype(0));
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
    QVERIFY(f.store->abort(tx));
    QCOMPARE(f.pixel(), QByteArray(f.bpp, char(0x57)));
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
}

void KisPageStoreCpuMutationTest::initialReplicaImportFailure_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("fault");
    for (int bpp : {1, 4, 8, 16}) for (int fault = 0; fault < 5; ++fault)
        QTest::newRow(qPrintable(QString("%1-fault%2").arg(bpp).arg(fault))) << bpp << fault;
}

void KisPageStoreCpuMutationTest::initialReplicaImportFailure()
{
    QFETCH(int, bpp); QFETCH(int, fault);
    Fixture f;
    if (fault == 4) {
        KisPageBackingLimits limits;
        limits.residentCurrentBytes.cpuRam = 64 * 64 * bpp;
        QVERIFY(f.store->configureBackingLimits(limits));
    }
    QVERIFY2(f.init(bpp, 2, true), qPrintable(f.error));
    const auto authority = f.initialReplicas.first();
    const auto descriptor = f.importedDescriptor;
    auto alternate = f.initialReplicas[1];
    if (fault == 0) alternate = authority;
    if (fault == 1) ++alternate.version.generation.value;
    if (fault == 2) ++alternate.providerEpoch.value;
    if (fault == 3) ++alternate.layout.rowStride;
    QVERIFY(!f.store->adoptInitialPage(authority.version, descriptor, authority, {alternate}, &f.error));
    QVERIFY(!f.store->isOperational());
    QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(0));
    const auto usage = f.store->backingUsage();
    QCOMPARE(usage.buckets[size_t(KisBackingBudgetClass::Current)].live.cpuRam, quint64(0));
    for (const auto &bucket : usage.buckets) QCOMPARE(bucket.reserved.cpuRam, quint64(0));
    QVERIFY(f.provider->validate(authority, descriptor));
    QVERIFY(f.provider->validate(f.initialReplicas[1], descriptor));
    const QVector<KisReplicaHandle> alternates = fault == 4 ? QVector<KisReplicaHandle>{} : f.initialReplicas.mid(1);
    QVERIFY2(f.store->adoptInitialPage(authority.version, descriptor, authority, alternates, &f.error), qPrintable(f.error));
    QVERIFY2(f.store->finalizeInitialization(&f.error), qPrintable(f.error));
    QCOMPARE(f.pixel(), QByteArray(bpp, char(0x31)));
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    if (fault == 4) {
        // Rejected import never took ownership of this caller-held replica.
        KisPageOwnerLedger identities; QVERIFY(identities.configure(f.completions));
        QVERIFY(f.provider->retire(identities.nextOperationId(), f.initialReplicas[1], {}).isValid());
    }
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::productionRecoverableHandoff_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<int>("route"); QTest::addColumn<int>("terminal");
    for (int bpp : {1, 4, 8, 16}) for (int route : {0, 1, 2, 3, 4}) for (int terminal : {0, 1, 2, 3})
        QTest::newRow(qPrintable(QString("%1-route%2-terminal%3").arg(bpp).arg(route).arg(terminal)))
            << bpp << route << terminal;
}

void KisPageStoreCpuMutationTest::productionRecoverableHandoff()
{
    QFETCH(int, bpp); QFETCH(int, route); QFETCH(int, terminal);
    Fixture f;
    KisPageBackingLimits limits;
    limits.activePendingBytes = limits.retirementDebtBytes = 64 * 64 * bpp;
    QVERIFY(f.store->configureBackingLimits(limits));
    QVERIFY2(f.init(bpp, 2), qPrintable(f.error));
    const auto original = f.initialReplicas.first();
    KisSurfaceEpochState surface; QVERIFY(f.store->resolveSurfaceState({1}, {}, &surface));
    const auto descriptor = surface.allocationDescriptor();
    const auto initialBytes = f.provider->memoryUsage().committedBytes;
    auto old = f.store->captureReadView();
    auto read = old.readResidentPage(key()); QVERIFY(read.isValid());
    const void *allocationAddress = read.data(); read = {}; // warm stale cache, no physical pin
    auto before = f.store->captureRetainedEpoch(); QVERIFY(before.isValid());
    const auto tx = f.store->beginCurrentTransaction();
    KisReplicaHandle expected = original;
    ++expected.allocation.generation; ++expected.version.generation.value;
    // Any Fresh allocation/copy would now fail. The existing alternate was
    // imported before the transaction, never manufactured by this write.
    f.provider->rejectWrite = true;
    KisPageMutationSession mutation;
    std::optional<KisWriteLease> lease;
    if (route == 2 || route == 3) {
        const auto request = f.store->acquireWrite(tx, key(), cpu,
            route == 3 ? KisPageWriteMode::DiscardContents : KisPageWriteMode::PreserveContents, KisPagePriority::Normal);
        QVERIFY2(request.isValid(), qPrintable(request.error));
        lease.emplace(f.store->resolve(request, request.readiness)); QVERIFY(lease->isValid());
        QCOMPARE(lease->cpuData(), allocationAddress);
        QCOMPARE(static_cast<const quint8 *>(lease->cpuData())[bpp], quint8(route == 3 ? 0x2a : 0x31));
        std::memset(lease->cpuData(), 0x73, size_t(bpp));
    } else {
        mutation = f.begin(tx);
        if (route == 1) {
            const QByteArray bytes(qsizetype(descriptor.minimumByteSize()), char(0x73));
            KisCpuPagePayload payload{bytes.constData(), 64 * bpp, bytes.size()};
            QVERIFY2(mutation.overwritePage(key(), payload, &f.error), qPrintable(f.error));
        } else {
            auto write = mutation.beginWrite(key(), route == 4 ? KisPageWriteMode::DiscardContents : KisPageWriteMode::PreserveContents, &f.error); QVERIFY2(write.isValid(), qPrintable(f.error));
            QCOMPARE(write.data(), allocationAddress);
            QCOMPARE(static_cast<const quint8 *>(write.data())[bpp], quint8(route == 4 ? 0x2a : 0x31));
            std::memset(write.data(), 0x73, size_t(bpp));
        }
    }
    QVERIFY(!f.provider->validate(original, descriptor));
    QVERIFY(f.provider->validate(expected, descriptor));
    QCOMPARE(f.provider->memoryUsage().committedBytes, initialBytes);
    read = old.readResidentPage(key()); QVERIFY(read.isValid());
    QCOMPARE(read.version(), original.version);
    QCOMPARE(static_cast<const quint8 *>(read.data())[0], quint8(0x31));
    QVERIFY(read.data() != allocationAddress); read = {};
    QCOMPARE(f.pixel(), QByteArray(bpp, char(0x31)));
    f.provider->rejectWrite = false;
    if (terminal == 1) {
        if (route == 2 || route == 3) f.store->cancel(std::move(*lease));
        else QVERIFY(mutation.cancel());
        QVERIFY(f.store->abort(tx));
    } else {
        if (route == 2 || route == 3) QVERIFY(f.store->publishHostWrite(std::move(*lease)).isValid());
        else QVERIFY2(mutation.seal(&f.error), qPrintable(f.error));
        if (terminal == 2) QVERIFY(f.store->abort(tx));
        else QVERIFY(f.store->commit(tx, f.store->preparedPages(tx)).isValid());
    }
    const bool committed = terminal == 0 || terminal == 3;
    QCOMPARE(f.pixel(), QByteArray(bpp, char(committed ? 0x73 : 0x31)));
    QCOMPARE(f.pixel({}, 0, bpp), QByteArray(bpp, char(!committed ? 0x31 : route == 1 ? 0x73 : route >= 3 ? 0x2a : 0x31)));
    if (terminal == 3) {
        auto after = f.store->captureRetainedEpoch(); QVERIFY(after.isValid());
        QVERIFY(f.store->restoreRetainedEpoch(before).isValid());
        QCOMPARE(f.pixel(), QByteArray(bpp, char(0x31)));
        QVERIFY(f.store->restoreRetainedEpoch(after).isValid());
        QCOMPARE(f.pixel(), QByteArray(bpp, char(0x73)));
        QVERIFY(f.store->releaseSnapshot(after.token));
    }
    read = old.readResidentPage(key()); QVERIFY(read.isValid());
    QCOMPARE(static_cast<const quint8 *>(read.data())[0], quint8(0x31)); read = {}; old = {};
    QVERIFY(f.store->releaseSnapshot(before.token));
    mutation = {};
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
    const auto usage = f.store->backingUsage();
    for (size_t i = 0; i < usage.buckets.size(); ++i) {
        // Closing retires payloads. The still-live Store continues to own
        // charged metadata directories/arena capacity until its destruction.
        if (i != size_t(KisBackingBudgetClass::MetadataArena))
            QCOMPARE(usage.buckets[i].live.cpuRam, quint64(0));
        QCOMPARE(usage.buckets[i].reserved.cpuRam, quint64(0));
    }
}

void KisPageStoreCpuMutationTest::productionHandoffFallback_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("generic"); QTest::addColumn<int>("blocker");
    for (int bpp : {1, 4, 8, 16}) for (bool generic : {false, true}) for (int blocker = 0; blocker < 8; ++blocker)
        QTest::newRow(qPrintable(QString("%1-generic%2-blocker%3").arg(bpp).arg(generic).arg(blocker)))
            << bpp << generic << blocker;
}

void KisPageStoreCpuMutationTest::productionHandoffFallback()
{
    QFETCH(int, bpp); QFETCH(bool, generic); QFETCH(int, blocker);
    Fixture f;
    if (blocker == 6) {
        KisPageBackingLimits limits; limits.maxTransientVersionsPerPage = 0;
        QVERIFY(f.store->configureBackingLimits(limits));
    }
    QVERIFY2(f.init(bpp, blocker == 0 ? 1 : 2), qPrintable(f.error));
    const auto source = f.initialReplicas.first();
    KisSurfaceEpochState surface; QVERIFY(f.store->resolveSurfaceState({1}, {}, &surface));
    auto old = f.store->captureReadView();
    KisCpuReadGuard pin;
    std::optional<KisReadLease> lease;
    KisCompletionTicket lastUse;
    KisTileData *raw = nullptr;
    const auto releaseRaw = qScopeGuard([&] { if (raw) raw->deref(); });
    if (blocker == 1 || blocker == 4) {
        pin = old.readResidentPage(key()); QVERIFY(pin.isValid());
        if (blocker == 4) {
            raw = f.provider->p->tileDataForCpuReadGuard(pin); QVERIFY(raw); raw->ref(); pin = {};
        }
    }
    if (blocker == 2 || blocker == 3) {
        auto request = f.store->acquireRead(key(), {}, cpu, KisPagePriority::Normal);
        lease.emplace(f.store->resolve(request, request.readiness)); QVERIFY(lease->isValid());
        if (blocker == 3) {
            lastUse = f.completions->allocatePending(f.completions->registerSource(KisCompletionDomain::HostLogical));
            f.store->release(std::move(*lease), lastUse); lease.reset();
        }
    }
    KisPageOwnerLedger otherOwner;
    if (blocker == 7) {
        QVERIFY(otherOwner.configure(f.completions, &f.error));
        QVERIFY(otherOwner.registerProvider(f.provider, &f.error));
    }
    if (blocker == 5) f.provider->rejectBindingFor = f.initialReplicas[1];
    const auto originalBytes = f.provider->memoryUsage().committedBytes;
    const auto tx = f.store->beginCurrentTransaction();
    if (generic) {
        auto request = f.store->acquireWrite(tx, key(), cpu,
            KisPageWriteMode::PreserveContents, KisPagePriority::Normal);
        QCOMPARE(request.isValid(), blocker != 6);
        if (request.isValid()) {
            QCOMPARE(f.provider->memoryUsage().committedBytes, originalBytes + 64 * 64 * bpp);
            QVERIFY(f.store->cancel(request));
        }
    } else {
        auto mutation = f.begin(tx);
        auto write = mutation.beginWrite(key(), &f.error);
        QCOMPARE(write.isValid(), blocker != 6);
        if (write.isValid()) {
            QCOMPARE(static_cast<const quint8 *>(write.data())[0], quint8(0x31));
            QCOMPARE(f.provider->memoryUsage().committedBytes, originalBytes + 64 * 64 * bpp);
            write = {};
        }
        QVERIFY(mutation.cancel());
    }
    QVERIFY(f.provider->validate(source, surface.allocationDescriptor()));
    QVERIFY(f.store->abort(tx));
    f.provider->rejectBindingFor = {};
    pin = {}; if (lease && lease->isValid()) f.store->release(std::move(*lease));
    if (lastUse.isValid()) {
        QVERIFY(f.completions->complete(lastUse, KisCompletionStatus::Succeeded));
        QTRY_COMPARE(f.store->sessionStats().pendingLastUses, qsizetype(0));
    }
    if (raw) { raw->deref(); raw = nullptr; }
    auto read = old.readResidentPage(key()); QVERIFY(read.isValid());
    QCOMPARE(static_cast<const quint8 *>(read.data())[0], quint8(0x31)); read = {}; old = {};
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::productionHandoffConcurrentConsumers_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("generic");
    QTest::addColumn<int>("consumer"); QTest::addColumn<bool>("afterInstall");
    for (int bpp : {1, 4, 8, 16}) for (bool generic : {false, true})
        for (int consumer = 0; consumer < 9; ++consumer) for (bool after : {false, true})
            QTest::newRow(qPrintable(QString("%1-generic%2-consumer%3-after%4")
                .arg(bpp).arg(generic).arg(consumer).arg(after))) << bpp << generic << consumer << after;
}

void KisPageStoreCpuMutationTest::productionHandoffConcurrentConsumers()
{
    QFETCH(int, bpp); QFETCH(bool, generic); QFETCH(int, consumer); QFETCH(bool, afterInstall);
    Fixture f; QVERIFY2(f.init(bpp, 2), qPrintable(f.error));
    const auto source = f.initialReplicas[0];
    const auto before = f.initialReplicas[1];
    auto old = f.store->captureReadView(); QVERIFY(old.isValid());
    auto warm = old.readResidentPage(key()); QVERIFY(warm.isValid());
    const void *originalAddress = warm.data(); warm = {};
    const auto initialBytes = f.provider->memoryUsage().committedBytes;
    QSemaphore start, ready, release;
    std::atomic<bool> stop{false}, workerOk{true};
    std::thread worker([&] {
        start.acquire();
        KisCpuReadGuard pin;
        std::optional<KisReadLease> lease;
        KisCompletionTicket lastUse;
        KisCapturedReadView captured;
        std::shared_ptr<KisCpuResidentBinding> beforeBinding;
        bool beforePinned = false;
        KisPageOwnerLedger otherOwner;
        if (!stop.load()) {
            if (consumer == 0 || consumer == 4 || consumer == 8) {
                pin = old.readResidentPage(key());
                workerOk = pin.isValid() && pin.version() == source.version
                    && static_cast<const quint8 *>(pin.data())[0] == 0x31;
                if ((consumer == 4 || consumer == 8) && workerOk) {
                    auto *raw = f.provider->p->tileDataForCpuReadGuard(pin);
                    if (!raw) workerOk = false;
                    else {
                        raw->ref(); pin = {};
                        // Before accounting prepare A may move to SSD. Once
                        // prepared, its class claim rejects the move; after
                        // install B remains protected by the before pin.
                        const bool swapped = KisTileDataStore::instance()->trySwapTileData(raw);
                        workerOk = swapped == (consumer == 8 && !afterInstall);
                        raw->deref();
                    }
                }
            } else if (consumer >= 1 && consumer <= 3) {
                auto request = f.store->acquireReadInView(key(), old, cpu, KisPagePriority::Normal);
                if (!request.isValid()) workerOk = false;
                else {
                    lease.emplace(f.store->resolve(request, request.readiness));
                    workerOk = lease->isValid() && lease->version() == source.version
                        && static_cast<const quint8 *>(lease->cpuData())[0] == 0x31;
                    if (!lease->isValid()) f.store->cancel(request);
                    else if (consumer != 1) {
                        if (consumer == 3)
                            lastUse = f.completions->allocatePending(
                                f.completions->registerSource(KisCompletionDomain::HostLogical));
                        f.store->release(std::move(*lease), lastUse); lease.reset();
                    }
                }
            } else if (consumer == 5) {
                workerOk = otherOwner.configure(f.completions) && otherOwner.registerProvider(f.provider);
            } else if (consumer == 6) {
                captured = f.store->captureReadView();
                KisPageVersion version;
                workerOk = captured.resolvePageVersion(key(), &version) && version == source.version;
            } else {
                beforeBinding = f.provider->cpuResidentBinding(before);
                const void *bytes = beforeBinding ? beforeBinding->acquireRead(before.allocationIdentity(), true) : nullptr;
                beforePinned = bytes;
                workerOk = bytes && static_cast<const quint8 *>(bytes)[0] == 0x31;
            }
        }
        ready.release(); release.acquire();
        pin = {};
        if (lease && lease->isValid()) f.store->release(std::move(*lease));
        if (lastUse.isValid()) {
            workerOk = workerOk && f.completions->complete(lastUse, KisCompletionStatus::Succeeded);
            f.store->acknowledgeLastUse(f.completions->verifyTerminal(lastUse));
            workerOk = workerOk && f.store->sessionStats().pendingLastUses == 0;
        }
        if (beforePinned) beforeBinding->releaseRead();
    });
    auto stopWorker = qScopeGuard([&] {
        stop = true; start.release(); release.release(); worker.join();
    });
    bool observed = false, consumerReady = false;
    int installed = 0;
    KisPageStoreDiagnosticRecorder recorder(true, f.store.get());
    recorder.setPhaseObserver([&](KisPageStoreDiagnosticPhase phase) {
        if (phase == KisPageStoreDiagnosticPhase::RecoverableInstalled) ++installed;
        const auto trigger = afterInstall ? KisPageStoreDiagnosticPhase::RecoverableCleanup
                                          : consumer == 8 ? KisPageStoreDiagnosticPhase::RecoverablePrepare
                                                          : KisPageStoreDiagnosticPhase::RecoverablePrepared;
        if (phase != trigger || observed) return;
        observed = true; start.release(); consumerReady = ready.tryAcquire(1, 5000);
    });
    const auto tx = f.store->beginCurrentTransaction();
    auto mutation = generic ? KisPageMutationSession{} : f.begin(tx);
    std::optional<KisWriteLease> writeLease;
    KisCpuWriteGuard guard;
    if (generic) {
        const auto request = f.store->acquireWrite(tx, key(), cpu, KisPageWriteMode::PreserveContents, KisPagePriority::Normal);
        QVERIFY2(request.isValid(), qPrintable(request.error));
        writeLease.emplace(f.store->resolve(request, request.readiness)); QVERIFY(writeLease->isValid());
    } else {
        guard = mutation.beginWrite(key(), &f.error); QVERIFY2(guard.isValid(), qPrintable(f.error));
    }
    recorder.setPhaseObserver({});
    QVERIFY(observed); QVERIFY(consumerReady); QVERIFY(workerOk.load());
    const bool handoff = afterInstall || consumer == 4 || consumer == 6 || consumer == 7;
    QCOMPARE(installed, handoff ? 1 : 0);
    void *bytes = generic ? writeLease->cpuData() : guard.data();
    QCOMPARE(bytes == originalAddress, handoff);
    QCOMPARE(f.provider->validate(source, f.importedDescriptor), !handoff);
    QCOMPARE(f.provider->memoryUsage().committedBytes, initialBytes + (handoff ? 0 : 4096 * bpp));
    QCOMPARE(static_cast<const quint8 *>(bytes)[0], quint8(0x31));
    std::memset(bytes, 0x73, size_t(bpp));
    guard = {};
    if (generic) f.store->cancel(std::move(*writeLease));
    else QVERIFY(mutation.cancel());
    QVERIFY(f.store->abort(tx));
    release.release(); worker.join(); stopWorker.dismiss();
    QVERIFY(workerOk.load());
    auto read = old.readResidentPage(key());
    if (!read.isValid()) {
        // The pre-install swap case is allowed to leave G nonresident.
        KisPageStoreReadPage materialized(f.store.get(), old, key(), &f.error);
        QVERIFY2(materialized.data(), qPrintable(f.error));
        QCOMPARE(materialized.data()[0], quint8(0x31));
    } else QCOMPARE(static_cast<const quint8 *>(read.data())[0], quint8(0x31));
    read = {}; old = {}; mutation = {};
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreCpuMutationTest::processStorageRejectsAndRetains()
{
    kisDrainPageStoreReclamation();
    QString error;
    auto process = kisAcquirePageStoreBootstrapBudget(&error); QVERIFY2(process, qPrintable(error));
    const auto limit = kisPageProcessStorageLimit();
    const auto live = [&] { return process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    KisBackingBudgetReservation warm[16];
    for (auto &reservation : warm) { reservation = process->reserve({}, nullptr); QVERIFY(reservation.isValid()); }
    for (auto &reservation : warm) reservation.release();
    const auto resource = kisPageProcessMemoryResource();
    const auto baseline = live();
    {
        Fixture f;
        f.completions = std::allocate_shared<KisCompletionRegistry>(KisMutationStorageAllocator<KisCompletionRegistry>{}, process);
        f.providerProcessBudget = process;
        QVERIFY(f.store->configureSharedNonPayloadBudget(process, &error));
        QVERIFY2(f.init(), qPrintable(f.error)); QVERIFY(f.fill(0x31));
        auto view = f.store->captureReadView(); QVERIFY(view.isValid());
        auto guard = view.readResidentPage(key()); QVERIFY(guard.isValid());
        auto defaultGuard = view.readResidentPage(key(3)); QVERIFY(defaultGuard.isValid());
        KisPageByteArray bytes("abcd"), shared = bytes;
        KisPageStateSnapshot oracle;
        oracle.key = key(3); oracle.publishedEpoch = {1};
        oracle.publishedGeneration = {1}; oracle.nextGeneration = {2};
        oracle.publishedDefaultPixelRevision = 1;
        oracle.versions.push_back({{oracle.key, {1}, 1}, KisPagePublicationState::Published, {}, {}, {}, {}});
        QVERIFY(KisPageStateMachine().validateInvariants(oracle));
        KisPageTransition restore;
        restore.kind = KisPageTransitionKind::RestoreCommittedVersion;
        restore.imageEpoch = {2}; restore.version = {oracle.key, {1}, 2};
        KisTiledExtentManager extent; QVERIFY(extent.configureStorage(resource));
        QVERIFY(extent.prepareTileRange(QRect(0, 0, 1, 1)));
        extent.notifyTileAdded(0, 0); const auto oldExtent = extent.extent();
        struct Value { quint64 value = 42; };
        auto value = std::allocate_shared<Value>(KisMutationStorageAllocator<Value>{});
        std::weak_ptr<Value> weak(value);
        const auto before = live();
        const auto fillerBytes = size_t(limit - before);
        void *filler = kisAllocatePageProcessStorage(fillerBytes, 1);
        auto free = qScopeGuard([&] { if (filler) kisFreePageProcessStorage(filler, fillerBytes, 1); });
        QCOMPARE(live(), limit);
        for (int attempt = 0; attempt < 3; ++attempt) {
            QVERIFY_EXCEPTION_THROWN(shared.fill('z'), std::bad_alloc);
            QVERIFY_EXCEPTION_THROWN(std::allocate_shared<KisTiles3PageReplicaProvider>(
                KisMutationStorageAllocator<KisTiles3PageReplicaProvider>{}), std::bad_alloc);
            const auto refused = KisPageStateMachine().apply(oracle, restore);
            QVERIFY(!refused.accepted);
            QCOMPARE(refused.next.versions.size(), oracle.versions.size());
            QCOMPARE(refused.next.versions.constData(), oracle.versions.constData());
            QCOMPARE(refused.next.publishedEpoch, oracle.publishedEpoch);
            QVERIFY(refused.effects.isEmpty()); QVERIFY(refused.rejectionReason.contains(QStringLiteral("storage")));
            QVERIFY(!extent.prepareTileRange(QRect(100000, 100000, 1, 1)));
            QCOMPARE(extent.extent(), oldExtent);
            QVERIFY(!f.store->closeSession(&error));
            QCOMPARE(live(), limit);
            QCOMPARE(bytes.constData()[0], 'a'); QCOMPARE(shared.constData()[0], 'a');
            QCOMPARE(static_cast<const quint8 *>(guard.data())[0], quint8(0x31));
        }
        // The same already accepted values survive refusal; no pixel work is
        // replayed. Removing capacity lets their original preparation proceed.
        kisFreePageProcessStorage(std::exchange(filler, nullptr), fillerBytes, 1);
        shared.fill('z'); QCOMPARE(bytes.constData()[0], 'a'); QCOMPARE(shared.constData()[0], 'z');
        QVERIFY(KisPageStateMachine().apply(oracle, restore).accepted);
        QVERIFY(extent.prepareTileRange(QRect(100000, 100000, 1, 1)));
        value.reset(); QVERIFY(weak.expired()); const auto weakBytes = live();
        weak.reset(); QVERIFY(live() < weakBytes);
        view = {}; guard = {}; defaultGuard = {};
        f.provider->rejectRetire = true;
        const auto retainedBytes = f.provider->memoryUsage().committedBytes;
        for (int attempt = 0; attempt < 3; ++attempt) QVERIFY(!f.store->closeSession(&error));
        QCOMPARE(f.provider->memoryUsage().committedBytes, retainedBytes);
        f.provider->rejectRetire = false;
        QVERIFY2(f.store->closeSession(&error), qPrintable(error));
        QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(0));
        f.store.reset(); f.provider.reset(); f.completions.reset();
    }
    kisDrainPageStoreReclamation();
    QCOMPARE(live(), baseline);
    std::weak_ptr<KisBackingBudgetController> weak(process);
    const auto fixedBefore = kisPageProcessStorageBytes();
    process.reset(); QVERIFY(weak.expired());
    // The last public weak control remains a real fixed allocation until free.
    QCOMPARE(kisPageProcessStorageBytes(), fixedBefore);
    weak.reset();
    QVERIFY(kisPageProcessStorageBytes() < fixedBefore);
}

void KisPageStoreCpuMutationTest::runtimeStopsAtCapacity()
{
    kisDrainPageStoreReclamation();
    auto process = kisAcquirePageStoreBootstrapBudget(); QVERIFY(process);
    auto warm = process->reserve({}, nullptr); QVERIFY(warm.isValid()); warm.release();
    std::atomic<int> calls{0};
    KisPageReclamationDelay late(KisPageReadinessCallback([&] { ++calls; }));
    QVERIFY(late.isValid()); QVERIFY(late.arm(60000));
    const auto live = [&] { return process->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const auto before = kisPageProcessStorageBytes();
    const auto fillerBytes = size_t(kisPageProcessStorageLimit() - live());
    void *filler = kisAllocatePageProcessStorage(fillerBytes, 1);
    auto free = qScopeGuard([&] { kisFreePageProcessStorage(filler, fillerBytes, 1); });
    kisStopPageStoreReclamation();
    QCOMPARE(calls.load(), 0); QVERIFY(!late.arm(1)); QVERIFY(!late.takeReady());
#ifdef Q_OS_DARWIN
    QVERIFY(kisPageProcessStorageBytes() + 2 * 512 * 1024 <= before + fillerBytes);
#endif
    const auto stopped = live(); late.reset(); QVERIFY(live() < stopped);
    kisStopPageStoreReclamation(); // The same permanent endpoint is idempotent.
    qInfo() << "BR1_FIXED_RUNTIME_STOP_BYTES" << before << kisPageProcessStorageBytes() - fillerBytes;
}

QTEST_GUILESS_MAIN(KisPageStoreCpuMutationTest)
#include "KisPageStoreCpuMutationTest.moc"

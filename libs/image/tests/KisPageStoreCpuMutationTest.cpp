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
#include "KisCpuResidentBinding_p.h"
#include "KisPageStoreDiagnostics_p.h"
#include "KisPageMetadataCoordinator.h"
#include "KisPageOwnerLedger.h"
#include "KisPageStoreReclamation_p.h"
#include "KisPageStoreCpuSurfaceOps.h"
#include "KisPageWriteCoordinator_p.h"
#include "KisTiles3PageReplicaProvider.h"
#include "KisTiledDataManagerPageStoreBackend.h"
#include "KisPageStoreIteratorReadScope_p.h"
#include "tiles3/kis_tile_data.h"
#include "tiles3/kis_tile_data_store.h"
#include "tiles3/kis_tile_data_store_iterators.h"

namespace {
const KisPageAccessRequirement cpu{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
KisPageKey key(int x = 0) { return {{1}, {x, 0}}; }

// Faults are injected at the provider contract, never by editing owner state.
class TestProvider final : public KisPageReplicaProvider
{
public:
    QSharedPointer<KisTiles3PageReplicaProvider> p = QSharedPointer<KisTiles3PageReplicaProvider>::create();
    mutable int validationsUntilFailure = -1;
    bool rejectWrite = false;
    bool returnExistingWriteReplica = false;
    bool returnExistingRequestReplica = false;
    std::atomic<bool> rejectRetire{false};
    bool rejectNativeBinding = false;
    bool rejectReadAccess = false;
    bool disableNativeMutation = false;
    bool disableCpuPayload = false;
    std::atomic<int> retireCalls{0};
    KisCompletionTicket deferredRetirement;
    std::function<void()> beforeRetire;
    std::function<void()> beforePrepareWrite;
    std::function<void()> beforeCpuPayload;
    std::function<void()> beforeRequestReplica;
    std::function<void()> beforeAdopt;
    std::function<void()> beforeRelease;
    std::function<void()> afterDomainSnapshot;
    std::function<void()> afterBackingFootprint;
    bool forwardDomainAdmission = true;
    bool rejectAdopt = false;
    std::function<void()> beforeValidate;
    KisReplicaHandle lastTarget;
    QString name() const override { return p->name(); }
    KisReplicaProviderId providerId() const override { return p->providerId(); }
    KisReplicaProviderEpoch providerEpoch() const override { return p->providerEpoch(); }
    KisReplicaCapabilities capabilities() const override
    { auto c = p->capabilities(); if (disableNativeMutation) { c.nativeCpuMutation = false; c.synchronousSourceAdoption = false; }
      if (disableCpuPayload) c.synchronousCpuPayload = false; return c; }
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
        const QSharedPointer<const KisPageReplicaSource> &s, const KisPageVersion &v,
        const KisPageAllocationDescriptor &d, KisReplicaSourceUse use, KisPagePriority pri) override
    {
        if (beforeAdopt) beforeAdopt(); if (rejectAdopt) return {};
        auto r = p->prepareSynchronousSource(o, s, v, d, use, pri); lastTarget = r.replica; return r;
    }
    bool copySynchronousSourceToCpu(const QSharedPointer<const KisPageReplicaSource> &s,
        const KisPageAllocationDescriptor &d, void *target, quint32 stride, quint64 bytes) override
    { return p->copySynchronousSourceToCpu(s, d, target, stride, bytes); }
    QSharedPointer<KisCpuResidentBinding> cpuResidentBinding(const KisReplicaHandle &r,
        KisCpuResidentReadStatus *status = nullptr) const override
    { return rejectNativeBinding ? QSharedPointer<KisCpuResidentBinding>{} : p->cpuResidentBinding(r, status); }
    KisReplicaOperation transfer(const KisReplicaTransferRequest &r, KisPagePriority pri) override
    { return p->transfer(r, pri); }
    KisReplicaAccess resolveAccess(KisPageLeaseId l, KisPageOperationId o, const KisReplicaHandle &r,
        KisPageAccessRequirement a, KisPageAccessMode m) override
    { return rejectReadAccess && m == KisPageAccessMode::Read ? KisReplicaAccess{} : p->resolveAccess(l, o, r, a, m); }
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
    QVector<KisReplicaBackingDomainChange> backingDomainChanges() const override
    {
        auto result = p->backingDomainChanges();
        if (afterDomainSnapshot) afterDomainSnapshot();
        return result;
    }
    void acknowledgeBackingDomainChange(quint64 slot, quint64 revision) override
    { p->acknowledgeBackingDomainChange(slot, revision); }
    bool registerBackingDomainAdmission(
        const QSharedPointer<KisReplicaBackingDomainAdmission> &admission,
        QString *error) override
    {
        if (forwardDomainAdmission)
            return p->registerBackingDomainAdmission(admission, error);
        KisPageStoreDetail::setError(error, {});
        return bool(admission);
    }
};

struct Fixture
{
    std::unique_ptr<KisPageStore> store = std::make_unique<KisPageStore>();
    QSharedPointer<KisCompletionRegistry> completions = QSharedPointer<KisCompletionRegistry>::create();
    QSharedPointer<TestProvider> provider = QSharedPointer<TestProvider>::create();
    QString error;
    int bpp = 4;
    bool init(int pixelSize = 4)
    {
        bpp = pixelSize;
        KisCpuResidentReplicaProviderConfig config;
        config.provider = {190}; config.providerEpoch = {1}; config.budgetBytes = 64 * 1024 * 1024;
        if (!provider->p->configure(config, completions, &error)) return false;
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
        return store->configure(initial, completions, 4, &error) &&
            store->registerReplicaProvider(provider) && store->finalizeInitialization(&error);
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
        QTest::newRow("native-session") << 0;
        QTest::newRow("generic-requests") << 1;
        QTest::newRow("sealed-proofs") << 2;
    }
    void partialCancellationRetiresBeforeRetry();
    void backingDomainRejectsStaleSnapshot();
    void metadataArenaBudgetTracksAllocator();
    void finiteBackingLimitMatrix();
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
    void cancelAndClaims();
    void facadeLifetime();
    void defaultAndRemoval_data() { pixelRows(); }
    void defaultAndRemoval();
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
    void closeRetriesDetachedReplicas();
    void closePollsDetachedRetirement_data()
    {
        QTest::addColumn<bool>("background");
        QTest::newRow("shutdown") << false;
        QTest::newRow("background") << true;
    }
    void closePollsDetachedRetirement();
    void physicalPinDelaysDetachedRetirement();
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
    void commitPreparationClaimsAndLateCapture();
    void publicationCleanupUsesBoundedBackgroundPasses();
    void hostLogicalCompletionIsPreparedOnce();
    void commitRevalidatesReadLeaseRevision();
    void publicationWhileCapturedReadersChange();
    void commitRebasesAfterDisjointPublication();
    void commitRejectsConflictingPublicationAtomically();
    void genericWriteLifetimeBlocksCommit();
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
    void sealPreservesReaderChanges_data()
    {
        QTest::addColumn<int>("bpp"); QTest::addColumn<int>("reader"); QTest::addColumn<bool>("removal");
        for (int bpp : {1, 4, 8, 16}) for (int reader = 0; reader < 4; ++reader) for (bool removal : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-reader%2-removal%3").arg(bpp).arg(reader).arg(removal)))
                << bpp << reader << removal;
    }
    void sealPreservesReaderChanges();
    void blockedSealValidationAllowsParallelWork();
};

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
    input = {}; old = {}; source.clear(); QVERIFY(f.store->closeSession());
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
    QVERIFY(refreshes >= 1 && refreshes <= 2);
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
    QVERIFY(refreshes >= 1 && refreshes <= 2); // one sweep plus a coalesced retention rescan, not one per slice
    QCOMPARE(retained.historyReachabilityRootsVisited - before.historyReachabilityRootsVisited,
             quint64(history + 1) * refreshes);
    QCOMPARE(metadataRetained.historySliceVersionInputs - metadataBefore.historySliceVersionInputs, quint64(history) * refreshes);
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
    QVERIFY(f.store->acknowledgeLastUse(f.completions->verifyTerminal(completion)));
    QVERIFY(f.store->waitForRetirementIdle());
    QCOMPARE(f.store->sessionStats().pageVersions, qsizetype(2));
    QCOMPARE(f.provider->memoryUsage().committedBytes, quint64(2 * 64 * 64 * f.bpp));
    QVERIFY(f.store->closeSession());
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
    input.clear();
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
    QSharedPointer<const KisPageReplicaSource> input;
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
        input.clear();
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

void KisPageStoreCpuMutationTest::guardReleaseParksPendingBacking_data()
{
    QTest::addColumn<int>("bpp"); QTest::addColumn<bool>("seal");
    for (int bpp : {1, 4, 8, 16}) for (bool seal : {false, true})
        QTest::newRow(qPrintable(QStringLiteral("bpp%1-seal%2").arg(bpp).arg(seal))) << bpp << seal;
}

void KisPageStoreCpuMutationTest::guardReleaseParksPendingBacking()
{
    QFETCH(int, bpp); QFETCH(bool, seal);
    Fixture f; QVERIFY(f.init(bpp)); QVERIFY(f.fill(0x31));
    auto beforeView = f.store->captureReadView();
    const auto tx = f.store->beginCurrentTransaction();
    const auto stats = f.store->mutationStatistics();
    const auto requests = f.store->sessionStats();
    const auto work = f.provider->p->payloadWork();
    auto segment = f.begin(tx);
    auto guard = segment.beginWrite(key()); QVERIFY(guard.isValid());
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
    QVERIFY(!binding->acquireRead(true)); QVERIFY(!binding->acquireWrite()); QVERIFY(!binding->retire());
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
    guard = segment.beginWrite(key()); QVERIFY2(guard.isValid(), qPrintable(f.error));
    QCOMPARE(restored, 1); QCOMPARE(guard.version(), version);
    QCOMPARE(QByteArray(static_cast<const char *>(guard.data()), int(guard.byteSize())),
             QByteArray(int(guard.byteSize()), char(0x71)));
    guard = {}; QVERIFY(swap());
    // seal/cancel needs no pixel pin or eager swap-in of parked pages.
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
    QWeakPointer<const KisPageReplicaSource> sourceLifetime;
    if (aliasFirst) {
        auto view = f.store->captureReadView();
        auto read = view.readResidentPage(key()); QVERIFY(read.isValid());
        KisSurfaceEpochState surface; QVERIFY(view.resolveSurfaceState({1}, &surface));
        auto source = f.provider->p->captureCpuReadSource(read, surface.allocationDescriptor());
        QVERIFY(source);
        sourceLifetime = source.toWeakRef();
        for (int i = 0; i < pages; ++i) QVERIFY(segment.aliasPage(key(i), source));
    }
    if (aliasFirst) QVERIFY(!sourceLifetime.isNull()); // only entries retain the input
    for (int i = 0; i < pages; ++i) {
        auto guard = segment.beginWrite(key(i)); QVERIFY(guard.isValid());
        if (aliasFirst) QCOMPARE(static_cast<const quint8 *>(guard.data())[0], quint8(0x31));
        static_cast<quint8 *>(guard.data())[0] = quint8(i + 1);
        if (retainGuards) guards.emplace_back(std::move(guard));
    }
    QVERIFY(sourceLifetime.isNull()); // consumed source cannot survive as a stale second owner
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
    QCOMPARE(stats.maximumPinnedPagesPerSegment, quint64(retainGuards ? pages : 1));
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
    input.clear();
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
    Fixture f; QVERIFY(f.init());
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
    QCOMPARE(after.descriptorCapacityPreparations - before.descriptorCapacityPreparations, quint64(2));
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
    Fixture f; QVERIFY(f.init());
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
    const auto progress = f.store->processRetirements(1);
    QCOMPARE(progress.retired, qsizetype(1));
    QCOMPARE(progress.pending, qsizetype(0));
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
    QVERIFY(binding); QVERIFY(binding->acquireWrite());
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
    Fixture f; QVERIFY(f.init());
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

void KisPageStoreCpuMutationTest::closeRetriesDetachedReplicas()
{
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    f.provider->rejectRetire = true;
    QVERIFY(f.fill(0x77));
    QCOMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(2));
    QVERIFY(!f.store->closeSession(&f.error));
    QVERIFY(f.error.contains(QStringLiteral("retirement failed")));
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
    if (background) QVERIFY(f.fill(0x71));
    QVERIFY(!f.store->closeSession(&f.error));
    QVERIFY(f.store->sessionStats().hasOutstandingCapabilities());
    QVERIFY(f.store->sessionStats().providerOperations > 0);
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
    Fixture f; QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    auto binding = f.provider->cpuResidentBinding(f.provider->lastTarget);
    QVERIFY(binding);
    const auto *bytes = static_cast<const char *>(binding->acquireRead(true));
    QVERIFY(bytes);
    QCOMPARE(bytes[0], char(0x31));
    QVERIFY(f.fill(0x77));
    QCOMPARE(bytes[0], char(0x31));
    QCOMPARE(f.store->sessionStats().pendingRetiredReplicas, qsizetype(1));
    const auto blocked = f.store->processRetirements(1);
    QCOMPARE(blocked.retired, qsizetype(0));
    QCOMPARE(blocked.pending, qsizetype(1));
    binding->releaseRead();
    const auto drained = f.store->processRetirements(1);
    QCOMPARE(drained.retired, qsizetype(1));
    QCOMPARE(drained.pending, qsizetype(0));
    QVERIFY(!binding->acquireRead(true));
    QVERIFY(f.store->closeSession());
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
    QCOMPARE(sealed.maximumPinnedPagesPerSegment, quint64(4));
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
        QCOMPARE(after.maximumPinnedPagesPerSegment, quint64(1));
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
    QCOMPARE(stats.writablePinsAcquired, quint64(1)); QCOMPARE(stats.maximumPinnedPagesPerSegment, quint64(1));
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
        QCOMPARE(stats.maximumPinnedPagesPerSegment, quint64(1));
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
    QVERIFY(binding); QVERIFY(!binding->acquireRead(false));
    QVERIFY(!binding->acquireWrite()); QVERIFY(!binding->retire());
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
    auto foreign = QSharedPointer<KisTiles3PageReplicaProvider>::create();
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
    QVERIFY(f.provider->p->backingDomainChanges().isEmpty());
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
    old = {}; current = {}; source.clear(); QVERIFY(f.store->closeSession());
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
    original = {}; before = {}; during = {}; old = {}; source.clear();
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
        QVERIFY(f.store->acknowledgeLastUse(f.completions->verifyTerminal(lastUse)));
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

void KisPageStoreCpuMutationTest::partialCancellationRetiresBeforeRetry()
{
    QFETCH(int, route);
    Fixture f;
    KisPageBackingLimits limits;
    limits.retirementDebtBytes = 64 * 64 * f.bpp;
    QVERIFY(f.store->configureBackingLimits(limits));
    QVERIFY(f.init()); QVERIFY(f.fill(0x31));
    const auto tx = f.store->beginCurrentTransaction();
    auto mutation = route == 0 ? f.begin(tx) : KisPageMutationSession{};
    for (int i = 0; i < 2; ++i) {
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
    f.provider->rejectRetire = false;
    f.store->processRetirements(64); QVERIFY(f.store->waitForRetirementIdle());
    if (route == 0) QVERIFY(mutation.cancel());
    QVERIFY(f.store->abort(tx));
    QCOMPARE(f.pixel(), QByteArray(f.bpp, char(0x31)));
    QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
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
    QVERIFY(f.provider->backingDomainChanges().isEmpty());
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
    QVERIFY(f.provider->backingDomainChanges().isEmpty());

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
    {
        Fixture probe;
        QVERIFY2(probe.init(), qPrintable(probe.error));
        directoryBytes = probe.store->backingUsage()
            .buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
        QVERIFY(directoryBytes > 0);
        QVERIFY2(probe.store->closeSession(&probe.error), qPrintable(probe.error));
    }

    // Directory-only capacity configures cleanly, then rejects the first
    // arena block before installing a page and releases the failed prepare.
    {
        Fixture f;
        KisPageBackingLimits limits;
        limits.metadataArenaBytes = directoryBytes;
        QVERIFY(f.store->configureBackingLimits(limits, &f.error));
        QVERIFY2(f.init(), qPrintable(f.error));
        const auto tx = f.store->beginCurrentTransaction();
        auto mutation = f.begin(tx);
        auto guard = mutation.beginWrite(
            key(), KisPageWriteMode::DiscardContents, &f.error);
        QVERIFY(!guard.isValid());
        QVERIFY(f.error.contains(QStringLiteral("budget")));
        QVERIFY(mutation.cancel());
        QVERIFY(f.store->abort(tx));
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
        QVERIFY(f.provider->backingDomainChanges().isEmpty());
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
        QVERIFY(f.provider->backingDomainChanges().isEmpty());
        QVERIFY2(f.store->closeSession(&f.error), qPrintable(f.error));
    }
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

QTEST_GUILESS_MAIN(KisPageStoreCpuMutationTest)
#include "KisPageStoreCpuMutationTest.moc"

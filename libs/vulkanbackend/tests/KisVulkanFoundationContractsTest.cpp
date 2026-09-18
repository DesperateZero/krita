/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QCryptographicHash>
#include <QTest>

#include <utility>

#include "KisBackendServiceRegistry.h"
#include "KisCompletionRegistry.h"
#include "KisSurfaceRegistry.h"
#include "KisSurfaceCodecRegistry.h"
#include "KisVulkanMemory.h"
#include "KisVulkanDeviceService.h"
#include "KisVulkanPipelineRepository.h"
#include "KisVulkanShaderCatalog.h"
#include "KisVulkanSubmissionCoordinator.h"
#include "KisVulkanWsiService.h"

class KisVulkanFoundationContractsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void completionRegistryRejectsConflictingTerminalState();
    void surfaceRegistryAllocatesStableIdentity();
    void codecRegistryRejectsAmbiguousClaims();
    void backendRegistryRevokesExactGeneration();
    void deviceServiceRequiresCompleteFoundationEvidence();
    void shaderCatalogSealsOnlyCompleteAssets();
    void memoryLedgerEnforcesHardBudget();
    void pipelineRepositoryEnforcesStateOrder();
    void coordinatorAndWsiValidateGenerations();
};

void KisVulkanFoundationContractsTest::completionRegistryRejectsConflictingTerminalState()
{
    KisCompletionRegistry registry;
    KisCompletionSourceDescriptor source;
    source.name = QStringLiteral("test timeline");
    source.gpuTimeline = true;
    const quint64 sourceId = registry.registerSource(source);
    QVERIFY(sourceId != 0);

    const KisCompletionTicket first = registry.allocatePending(sourceId);
    const KisCompletionTicket second = registry.allocatePending(sourceId);
    QVERIFY(first.isValid());
    QCOMPARE(second.value(), first.value() + 1);
    QVERIFY(registry.complete(first, KisCompletionStatus::Succeeded));
    QVERIFY(!registry.complete(first, KisCompletionStatus::Failed));
    QCOMPARE(registry.status(first), KisCompletionStatus::Succeeded);
}

void KisVulkanFoundationContractsTest::surfaceRegistryAllocatesStableIdentity()
{
    KisSurfaceRegistry registry;
    KisSurfaceDescriptor descriptor;
    descriptor.debugName = QStringLiteral("canonical test surface");
    descriptor.ownerDocument = 7;
    descriptor.pixelExtent = QSize(512, 512);
    descriptor.logicalPageExtent = QSize(64, 64);
    descriptor.format.formatId = 1;
    descriptor.format.colorModelId = "RGBA";
    descriptor.format.colorDepthId = "U8";
    descriptor.format.profileFingerprint = "test-profile";
    descriptor.format.channelOrder = "RGBA";
    descriptor.format.packing = "interleaved";
    descriptor.format.defaultPixel = QByteArray(4, 0);
    descriptor.format.channelCount = 4;
    descriptor.format.pixelStride = 4;
    descriptor.format.pixelAlignment = 4;
    descriptor.format.hasAlpha = true;
    descriptor.format.alphaSemantic = KisSurfaceAlphaSemantic::Premultiplied;
    descriptor.format.endianness = KisSurfaceEndianness::NativeEndian;
    descriptor.format.codecVersion = 1;

    const KisSurfaceId first = registry.registerSurface(descriptor);
    const KisSurfaceId second = registry.registerSurface(descriptor);
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    QVERIFY(first.value != second.value);
    QCOMPARE(registry.surfaceCount(), 2);
    QVERIFY(registry.unregisterSurface(first));
    QVERIFY(!registry.contains(first));
}

namespace {

class TestSurfaceCodec : public KisSurfaceCodec
{
public:
    TestSurfaceCodec(QByteArray codecId, quint32 codecVersion, quint64 formatId)
        : m_id(std::move(codecId))
        , m_version(codecVersion)
        , m_formatId(formatId)
    {
    }

    QByteArray id() const override { return m_id; }
    quint32 version() const override { return m_version; }
    bool supports(const KisSurfaceFormat &format) const override
    {
        return format.formatId == m_formatId;
    }

private:
    QByteArray m_id;
    quint32 m_version = 0;
    quint64 m_formatId = 0;
};

}

void KisVulkanFoundationContractsTest::codecRegistryRejectsAmbiguousClaims()
{
    KisSurfaceCodecRegistry registry;
    QVERIFY(registry.registerCodec(QSharedPointer<TestSurfaceCodec>::create("codec-a", 1, 9)));
    QVERIFY(!registry.registerCodec(QSharedPointer<TestSurfaceCodec>::create("codec-a", 1, 9)));
    QVERIFY(registry.registerCodec(QSharedPointer<TestSurfaceCodec>::create("codec-b", 1, 9)));

    KisSurfaceFormat format;
    format.formatId = 9;
    format.colorModelId = "RGBA";
    format.colorDepthId = "U8";
    format.profileFingerprint = "profile";
    format.channelOrder = "RGBA";
    format.packing = "interleaved";
    format.channelCount = 4;
    format.pixelStride = 4;
    format.pixelAlignment = 4;
    format.alphaSemantic = KisSurfaceAlphaSemantic::Premultiplied;
    format.endianness = KisSurfaceEndianness::NativeEndian;
    format.codecVersion = 1;
    QVERIFY(!registry.codecFor(format));
}

void KisVulkanFoundationContractsTest::backendRegistryRevokesExactGeneration()
{
    KisBackendServiceRegistry registry;
    KisBackendValidationReport report;
    report.serviceId = 3;
    report.deviceGeneration = 5;
    report.allocatorGeneration = 2;
    report.coordinatorGeneration = 4;
    report.replicaProviderGeneration = 6;
    report.shaderAbi = 7;
    report.queueTopologyDigest = "queue";
    report.capabilityDigest = "capability";
    report.shaderManifestDigest = "shader-manifest";
    report.smokeProbeDigest = "smoke-probe";
    report.deviceReady = true;
    report.queuesReady = true;
    report.shadersReady = true;
    report.coordinatorReady = true;
    report.replicaProviderReady = true;

    const KisBackendReadyToken token = registry.acceptValidatedService(report);
    QVERIFY(token.isValid());
    QVERIFY(registry.isCurrent(token));
    QVERIFY(registry.revokeService(3, 5, QStringLiteral("device lost")));
    QVERIFY(!registry.isCurrent(token));
    QCOMPARE(registry.revocationReason(3), QStringLiteral("device lost"));

    report.deviceGeneration = 6;
    const KisBackendReadyToken recovered = registry.acceptValidatedService(report);
    QVERIFY(recovered.isValid());
    QVERIFY(registry.isCurrent(recovered));
    QVERIFY(!registry.isCurrent(token));
}

void KisVulkanFoundationContractsTest::deviceServiceRequiresCompleteFoundationEvidence()
{
    KisVulkanDeviceCapabilitySnapshot capabilities;
    capabilities.deviceGeneration = 1;
    capabilities.apiVersion = 1;
    capabilities.physicalDeviceIdentity = "device";
    capabilities.driverIdentity = "driver";
    capabilities.capabilityDigest = "capability";
    capabilities.deviceLocalBudgetBytes = 1024;
    capabilities.hostVisibleBudgetBytes = 1024;
    capabilities.maxStorageBufferRange = 1024;
    capabilities.maxPushConstantBytes = 128;
    capabilities.timelineSemaphore = true;
    capabilities.synchronization2 = true;
    capabilities.queues.graphicsFamily = 0;
    capabilities.queues.computeFamily = 0;
    capabilities.queues.transferFamily = 0;
    capabilities.queues.graphicsQueueCount = 1;
    capabilities.queues.computeQueueCount = 1;
    capabilities.queues.transferQueueCount = 1;

    KisVulkanFoundationEvidence evidence;
    evidence.serviceId = 1;
    evidence.capabilities = capabilities;
    evidence.allocatorGeneration = 1;
    evidence.coordinatorGeneration = 1;
    evidence.replicaProviderGeneration = 1;
    evidence.shaderAbi = 1;
    evidence.shaderManifestDigest = "manifest";
    evidence.queueTopologyDigest = capabilities.queues.stableDigestInput();
    evidence.requiredPipelinesPrepared = true;
    evidence.completionBridgeRegistered = true;
    evidence.smokeProbePassed = true;

    KisVulkanDeviceService incomplete;
    QVERIFY(incomplete.beginProbe());
    QVERIFY(incomplete.acceptProbeResult(capabilities));
    QVERIFY(!incomplete.acceptFoundationEvidence(evidence));
    QCOMPARE(incomplete.state(), KisVulkanFoundationState::Failed);

    evidence.smokeProbeDigest = "smoke";
    KisVulkanDeviceService complete;
    QVERIFY(complete.beginProbe());
    QVERIFY(complete.acceptProbeResult(capabilities));
    QVERIFY(complete.acceptFoundationEvidence(evidence));
    QVERIFY(complete.validationReport().isComplete());
}

void KisVulkanFoundationContractsTest::shaderCatalogSealsOnlyCompleteAssets()
{
    KisVulkanShaderCatalog catalog;
    KisVulkanShaderAsset asset;
    asset.logicalId = QStringLiteral("foundation.buffer_checksum");
    asset.semanticVersion = 1;
    asset.passAbiVersion = 1;
    asset.sourceDependencyHash = "source";
    asset.reflectionDigest = "reflection";
    asset.spirv = QByteArray("\x03\x02\x23\x07", 4);
    asset.spirvHash = QCryptographicHash::hash(asset.spirv, QCryptographicHash::Sha256);
    asset.requiredForFoundation = true;

    QString error;
    QVERIFY(catalog.registerAsset(asset, &error));
    QVERIFY(catalog.seal(1, &error));
    QCOMPARE(catalog.state(), KisVulkanShaderCatalogState::Sealed);
    QVERIFY(!catalog.manifestDigest().isEmpty());
}

void KisVulkanFoundationContractsTest::memoryLedgerEnforcesHardBudget()
{
    KisVulkanMemoryBudgetLedger ledger;
    KisVulkanMemoryBudget budget{1024, 768, 512};
    QVERIFY(ledger.configure(1, budget));

    KisVulkanMemoryReservationRequest request;
    request.bytes = 800;
    request.alignment = 64;
    request.ownerSession = 9;
    request.debugName = QStringLiteral("test allocation");
    const KisVulkanMemoryReservation first = ledger.reserve(request);
    QVERIFY(first.isValid());
    request.bytes = 300;
    QVERIFY(!ledger.reserve(request).isValid());
    QVERIFY(ledger.release(first));
    QCOMPARE(ledger.usage().reservedBytes, quint64(0));
}

void KisVulkanFoundationContractsTest::pipelineRepositoryEnforcesStateOrder()
{
    KisVulkanPipelineRepository repository;
    QVERIFY(repository.configure(1, 2, QByteArrayLiteral("manifest")));
    KisVulkanPipelineKey key;
    key.shaderId = QStringLiteral("stroke.round");
    key.shaderSemanticVersion = 1;
    key.passAbiVersion = 1;
    key.surfaceFormatId = 4;
    key.alphaSemantic = 1;
    key.workingColorClass = "linear";
    key.programId = "round-normal";
    key.deviceFeatureClass = "baseline";
    key.specializationDigest = "default";

    const KisVulkanPipelineHandle handle = repository.declarePipeline(key);
    QVERIFY(handle.isValid());
    QVERIFY(!repository.markPrepared(handle));
    QVERIFY(repository.beginPreparation(handle));
    QVERIFY(repository.markPrepared(handle));
    QVERIFY(repository.isPrepared(key));
}

void KisVulkanFoundationContractsTest::coordinatorAndWsiValidateGenerations()
{
    auto completions = QSharedPointer<KisCompletionRegistry>::create();
    auto coordinator = QSharedPointer<KisVulkanSubmissionCoordinator>::create();
    KisVulkanCoordinatorConfig config;
    config.coordinatorGeneration = 1;
    config.deviceGeneration = 2;
    config.queues.graphicsFamily = 0;
    config.queues.computeFamily = 0;
    config.queues.transferFamily = 0;
    config.queues.presentFamily = 0;
    config.queues.graphicsQueueCount = 1;
    config.queues.computeQueueCount = 1;
    config.queues.transferQueueCount = 1;
    config.queueTopologyDigest = config.queues.stableDigestInput();
    QVERIFY(coordinator->configure(config, completions));
    QCOMPARE(coordinator->state(), KisVulkanCoordinatorState::Configured);

    KisVulkanWsiService wsi;
    QVERIFY(wsi.configure(4, 2, coordinator));
    KisVulkanViewIntent intent;
    intent.documentId = 10;
    intent.documentSessionGeneration = 11;
    intent.viewGeneration = 1;
    intent.surface.platform = KisNativeSurfacePlatform::Metal;
    intent.surface.nativeWindowToken = "window";
    intent.surface.displayOrLayerToken = "layer";
    intent.surface.lifetimeGeneration = 1;
    intent.surface.pixelExtent = QSize(800, 600);
    intent.outputColorSpace = "display-p3";

    const KisVulkanViewHandle view = wsi.registerViewIntent(intent);
    QVERIFY(view.isValid());
    QVERIFY(!wsi.snapshot(view).isReady());
    intent.viewGeneration = 2;
    intent.surface.lifetimeGeneration = 2;
    QVERIFY(wsi.updateViewIntent(view, intent));
    const KisVulkanViewRetirement retirement = wsi.retireView(view);
    QVERIFY(retirement.isValid());
    QCOMPARE(wsi.viewCount(), 0);
}

QTEST_GUILESS_MAIN(KisVulkanFoundationContractsTest)

#include "KisVulkanFoundationContractsTest.moc"

/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>

#include "KisDerivedSurfaceCache.h"
#include "KisDocumentGpuSession.h"
#include "KisEvaluationGraphLowerer.h"
#include "KisEvaluationPlanner.h"
#include "KisPageStateMachine.h"
#include "KisPageStore.h"
#include "KisUnifiedSsdPageStore.h"

class KisPageStoreFoundationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void pageStoreFailsClosedBeforeBr1();
    void stateMachineRejectsTransitionsBeforeBr1();
    void ssdProviderDoesNotAdvertiseDurabilityBeforeBr3();
    void evaluationGraphContractIsNotLayerBound();
    void evaluationGraphRejectsCycles();
    void evaluationCapabilityRequiresExactOperation();
    void evaluationFoundationFailsClosedBeforeBr4();
};

void KisPageStoreFoundationTest::pageStoreFailsClosedBeforeBr1()
{
    KisPageStore store;
    QVERIFY(!store.isOperational());

    KisPageRange range;
    range.pages.append({KisSurfaceId{1}, KisLogicalPageId{0, 0}});
    range.exactGeneration = KisPageGeneration{1};

    const KisReadRequest read = store.acquireRead(
        range, KisPageAccessDomain::CpuRam, KisPagePriority::Interactive);
    QCOMPARE(read.status, KisPageRequestStatus::Unsupported);
    QCOMPARE(read.range.pages.size(), 1);
    QVERIFY(!read.readiness.isValid());
    QVERIFY(!read.error.isEmpty());

    const KisWriteRequest write = store.acquireWrite(
        range,
        KisPageAccessDomain::CpuRam,
        KisPageWriteMode::PreserveContents,
        KisPagePriority::Interactive);
    QCOMPARE(write.status, KisPageRequestStatus::Unsupported);
    QCOMPARE(write.range.pages.size(), 1);
    QVERIFY(!write.readiness.isValid());
    QVERIFY(!write.error.isEmpty());

    QVERIFY(!store.resolve(read, {}).isValid());
    QVERIFY(!store.resolve(write, {}).isValid());
    QVERIFY(!store.beginTransaction(KisImageEpochId{1}).isValid());
    QVERIFY(!store.captureCommittedEpoch().isValid());
}

void KisPageStoreFoundationTest::stateMachineRejectsTransitionsBeforeBr1()
{
    KisPageStateMachine stateMachine;
    KisPageStateSnapshot current;
    current.version.key.surface = KisSurfaceId{4};
    current.version.key.page = KisLogicalPageId{2, 3};
    current.version.generation = KisPageGeneration{7};

    KisPageTransition transition;
    transition.kind = KisPageTransitionKind::AcquireRead;
    transition.version = current.version;
    transition.operationId = 1;

    const KisPageTransitionResult result = stateMachine.apply(current, transition);
    QVERIFY(!result.accepted);
    QVERIFY(!result.rejectionReason.isEmpty());
    QVERIFY(result.next.version == current.version);

    QString invariantFailure;
    QVERIFY(!stateMachine.validateInvariants(current, &invariantFailure));
    QVERIFY(!invariantFailure.isEmpty());
}

void KisPageStoreFoundationTest::ssdProviderDoesNotAdvertiseDurabilityBeforeBr3()
{
    KisUnifiedSsdPageStore provider;
    const KisReplicaCapabilities capabilities = provider.capabilities();
    QVERIFY(!capabilities.durable);
    QVERIFY(!capabilities.domains.contains(KisPageAccessDomain::Ssd));

    KisPageVersion version;
    version.key.surface = KisSurfaceId{1};
    version.generation = KisPageGeneration{1};
    const KisReplicaOperation operation = provider.requestReplica(
        version,
        KisPageAccessDomain::Ssd,
        KisPageAccessMode::Read,
        KisPagePriority::Background);
    QCOMPARE(operation.status, KisPageRequestStatus::Unsupported);
    QVERIFY(!operation.replica.isValid());
    QVERIFY(!operation.error.isEmpty());
}

void KisPageStoreFoundationTest::evaluationGraphContractIsNotLayerBound()
{
    const KisEvaluationValueType imageType{
        "org.krita.value.image-surface", 1, KisEvaluationStorageClass::Surface};
    const KisEvaluationValueType maskType{
        "org.krita.value.mask-surface", 1, KisEvaluationStorageClass::Surface};

    KisEvaluationGraphSnapshot graph;
    graph.epoch = KisEvaluationEpochId{1};
    graph.documentId = 7;
    graph.graphRevision = 3;
    graph.context.contextVersion = 1;
    graph.context.workingSpaceId = "RGBA-U8-test";
    graph.context.policyDigest = "test-policy";

    KisEvaluationNodeDesc generator;
    generator.id = KisEvaluationNodeId{11};
    generator.operationId = "org.krita.test.generator";
    generator.semanticVersion = 1;
    generator.outputs.append({1, "image", imageType, 17,
                              KisEvaluationValueId{101}, false});
    graph.nodes.append(generator);
    graph.requestedOutputs.append({generator.id, 1});
    graph.exact = true;

    QVERIFY(graph.isValid());
    QVERIFY(graph.mutationRoots.isEmpty());

    KisEvaluationNodeDesc consumer;
    consumer.id = KisEvaluationNodeId{12};
    consumer.operationId = "org.krita.test.consumer";
    consumer.semanticVersion = 1;
    consumer.inputs.append({1, "image", imageType, 17, {}, false});
    consumer.outputs.append({2, "result", imageType, 17,
                             KisEvaluationValueId{102}, false});
    graph.nodes.append(consumer);
    graph.edges.append(KisEvaluationEdgeDesc{{generator.id, 1}, {consumer.id, 1}});
    graph.requestedOutputs = {{consumer.id, 2}};
    QVERIFY(graph.isValid());

    graph.nodes[1].inputs[0].type = maskType;
    QVERIFY(!graph.isValid());
    graph.nodes[1].inputs[0].type = imageType;
    QVERIFY(graph.isValid());
    graph.nodes[1].inputs[0].surfaceFormatId = 18;
    QVERIFY(!graph.isValid());
    graph.nodes[1].inputs[0].surfaceFormatId = 17;
    QVERIFY(graph.isValid());

    graph.edges.append(KisEvaluationEdgeDesc{{consumer.id, 2}, {consumer.id, 1}});
    QVERIFY(!graph.isValid());

    KisAuthoringGraphSnapshot authoring;
    authoring.schemaId = KisLayerStackLowerer::sourceSchemaId();
    authoring.schemaVersion = 1;
    authoring.documentId = graph.documentId;
    authoring.graphRevision = graph.graphRevision;
    authoring.immutablePayload = "immutable-layer-tree-snapshot";
    KisEvaluationValueVersion writableInput;
    writableInput.value = KisEvaluationValueId{301};
    writableInput.generation = 5;
    writableInput.type = imageType;
    writableInput.surface = {KisSurfaceId{9}, KisSurfaceGeneration{5}};
    authoring.canonicalInputs.append(writableInput);
    authoring.mutationRoots.append(writableInput.value);
    authoring.exact = true;
    QVERIFY(authoring.isValid());
    authoring.mutationRoots = {KisEvaluationValueId{302}};
    QVERIFY(!authoring.isValid());
    authoring.mutationRoots = {writableInput.value};

    KisLayerStackLowerer lowerer;
    QVERIFY(lowerer.supports(authoring));
}

void KisPageStoreFoundationTest::evaluationGraphRejectsCycles()
{
    const KisEvaluationValueType imageType{
        "org.krita.value.image-surface", 1, KisEvaluationStorageClass::Surface};

    KisEvaluationGraphSnapshot graph;
    graph.epoch = KisEvaluationEpochId{2};
    graph.documentId = 7;
    graph.graphRevision = 4;
    graph.context.contextVersion = 1;
    graph.context.workingSpaceId = "RGBA-U8-test";
    graph.context.policyDigest = "test-policy";

    KisEvaluationNodeDesc first;
    first.id = KisEvaluationNodeId{21};
    first.operationId = "org.krita.test.first";
    first.semanticVersion = 1;
    first.inputs.append({1, "input", imageType, 17, {}, false});
    first.outputs.append({2, "output", imageType, 17,
                          KisEvaluationValueId{201}, false});

    KisEvaluationNodeDesc second = first;
    second.id = KisEvaluationNodeId{22};
    second.operationId = "org.krita.test.second";
    second.outputs[0].producedValue = KisEvaluationValueId{202};

    graph.nodes = {first, second};
    graph.edges = {
        KisEvaluationEdgeDesc{{first.id, 2}, {second.id, 1}},
        KisEvaluationEdgeDesc{{second.id, 2}, {first.id, 1}}
    };
    graph.requestedOutputs = {{first.id, 2}};
    graph.exact = true;

    QVERIFY(!graph.isValid());
    QVERIFY(graph.validationError().contains(QStringLiteral("cycle")));
}

void KisPageStoreFoundationTest::evaluationCapabilityRequiresExactOperation()
{
    const KisEvaluationValueType imageType{
        "org.krita.value.image-surface", 1, KisEvaluationStorageClass::Surface};
    const KisEvaluationValueType maskType{
        "org.krita.value.mask-surface", 1, KisEvaluationStorageClass::Surface};

    KisAccelerationCapabilities capabilities;
    KisEvaluationOperationCapability operation;
    operation.operationId = "org.krita.test.operation";
    operation.semanticVersion = 3;
    operation.inputPorts = {{imageType, 17}};
    operation.outputPorts = {{imageType, 17}};
    capabilities.evaluationOperations.append(operation);

    QVERIFY(!capabilities.supportsEvaluationOperation(
        operation.operationId, 3, operation.inputPorts, operation.outputPorts));
    capabilities.featureMask |= quint64(1) << quint8(KisAccelerationFeature::GpuEvaluation);
    QVERIFY(capabilities.supportsEvaluationOperation(
        operation.operationId, 3, operation.inputPorts, operation.outputPorts));
    QVERIFY(!capabilities.supportsEvaluationOperation(
        operation.operationId, 2, operation.inputPorts, operation.outputPorts));
    QVERIFY(!capabilities.supportsEvaluationOperation(
        operation.operationId,
        3,
        {{maskType, 17}},
        operation.outputPorts));
    QVERIFY(!capabilities.supportsEvaluationOperation(
        operation.operationId, 3, {{imageType, 18}}, operation.outputPorts));
}

void KisPageStoreFoundationTest::evaluationFoundationFailsClosedBeforeBr4()
{
    KisAuthoringGraphSnapshot authoring;
    authoring.schemaId = KisLayerStackLowerer::sourceSchemaId();
    authoring.schemaVersion = 1;
    authoring.documentId = 2;
    authoring.graphRevision = 1;
    authoring.immutablePayload = "layer-tree";
    authoring.exact = true;

    KisEvaluationContext context;
    context.contextVersion = 1;
    context.workingSpaceId = "RGBA-U8-test";
    context.policyDigest = "test-policy";

    KisLayerStackLowerer lowerer;
    QString error;
    QVERIFY(!lowerer.lower(authoring, context, &error).isValid());
    QVERIFY(!error.isEmpty());

    KisEvaluationPlanner planner;
    QVERIFY(!planner.plan({}).isValid());

    KisDerivedSurfaceCache cache;
    QVERIFY(!cache.isOperational());
    KisDerivedSurfaceCacheKey key;
    QVERIFY(!cache.lookup(key).has_value());
    QVERIFY(!cache.publish(key, {}));
}

QTEST_GUILESS_MAIN(KisPageStoreFoundationTest)

#include "KisPageStoreFoundationTest.moc"

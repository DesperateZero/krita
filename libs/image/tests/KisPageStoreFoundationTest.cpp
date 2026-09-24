/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>

#include <type_traits>

#include "KisCompletionRegistry.h"
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
    void layoutBaselineDoesNotGrow();
    void pageStoreFailsClosedAtTiles3Boundary();
    void requestDescriptorsRejectAmbiguousSelectors();
    void accessRequirementSeparatesUmaConsumers();
    void leaseCapabilitiesAreMoveOnly();
    void stateMachineCommitsPreparedGeneration();
    void stateMachineAdoptsSynchronousPreparedWriteAtomically();
    void stateMachineSeparatesCommitAndIdleHistoryRetirement();
    void stateMachineCommitsLatestTransactionGeneration();
    void stateMachineDiscardsOnlyIdleHistoricalVersions();
    void stateMachineRestoresHistoricalCommittedGeneration();
    void stateMachineVirtualDefaultRemainsExactWithoutAllocation();
    void stateMachineVirtualDefaultWriterHasLogicalBeforeImage();
    void stateMachineHistoricalDefaultDoesNotChangeWriter();
    void stateMachineCancelPreservesPublishedGeneration();
    void stateMachineRejectsStaleCompletionAndAba();
    void stateMachineLeaseBlocksRetirement();
    void stateMachineHandoffAndFailurePreserveAuthority();
    void stateMachineAbortDropsOnlyPreparedGeneration();
    void stateMachineCapturedVersionRetention();
    void stateMachineRandomSequencePreservesInvariants();
    void ssdProviderDoesNotAdvertiseDurabilityBeforeBr3();
    void evaluationGraphContractIsNotLayerBound();
    void evaluationGraphRejectsCycles();
    void evaluationCapabilityRequiresExactOperation();
    void evaluationFoundationFailsClosedBeforeBr4();
};

namespace {

KisPageKey testPageKey()
{
    return {KisSurfaceId{4}, KisLogicalPageId{2, 3}};
}

KisPageVersion testPageVersion(quint64 generation)
{
    return {testPageKey(), KisPageGeneration{generation}};
}

KisPageAllocationDescriptor testAllocationDescriptor()
{
    KisPageAllocationDescriptor descriptor;
    descriptor.layoutRevision = 1;
    descriptor.format.formatId = 1;
    descriptor.format.colorModelId = "RGBA";
    descriptor.format.colorDepthId = "U8";
    descriptor.format.profileFingerprint = "foundation-test";
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
    descriptor.pageExtent = QSize(64, 16);
    descriptor.validRect = QRect(QPoint(0, 0), descriptor.pageExtent);
    descriptor.rowAlignment = 4;
    return descriptor;
}

KisReplicaHandle testReplica(quint64 generation,
                             quint64 provider,
                             quint64 providerEpoch,
                             quint64 slot,
                             quint64 allocationGeneration)
{
    KisReplicaHandle replica;
    replica.provider = KisReplicaProviderId{provider};
    replica.providerEpoch = KisReplicaProviderEpoch{providerEpoch};
    replica.allocation = KisReplicaAllocationToken{slot, allocationGeneration};
    replica.version = testPageVersion(generation);
    replica.domain = KisPageAccessDomain::CpuRam;
    replica.layout = {1, 1, QSize(64, 16), QRect(0, 0, 64, 16), 256, 4096};
    return replica;
}

KisPreparedPageProof testPreparedProof(
    quint64 generation,
    KisPageTransactionId transaction,
    const KisCompletionTicket &completion)
{
    const KisReplicaHandle authority =
        testReplica(generation, 1, 1, generation, 1);
    KisPreparedPageProof proof;
    proof.transaction = transaction;
    proof.authority = authority;
    proof.producerCompletion = completion;
    proof.providerValidationStamp = generation;
    return proof;
}

KisPageStateSnapshot initialPageState()
{
    const KisPageVersion publishedVersion = testPageVersion(1);
    const KisReplicaHandle authority = testReplica(1, 1, 1, 1, 1);

    KisPageVersionStateSnapshot version;
    version.version = publishedVersion;
    version.publication = KisPagePublicationState::Published;
    version.replicas.append({authority, KisReplicaValidity::Valid, {}, {}, 0, {}});
    version.authority = authority;

    KisPageStateSnapshot state;
    state.key = testPageKey();
    state.publishedEpoch = KisImageEpochId{1};
    state.publishedGeneration = KisPageGeneration{1};
    state.nextGeneration = KisPageGeneration{2};
    state.versions.append(version);
    return state;
}

KisPageTransition writeTransition(KisPageTransitionKind kind,
                                  quint64 generation,
                                  quint64 transaction = 1,
                                  quint64 operation = 10,
                                  quint64 writer = 20)
{
    KisPageTransition transition;
    transition.kind = kind;
    transition.baseVersion = testPageVersion(generation - 1);
    transition.version = testPageVersion(generation);
    transition.source = testReplica(generation - 1, 1, 1, generation - 1, 1);
    transition.target = testReplica(generation, 1, 1, generation, 1);
    transition.operation = KisPageOperationId{operation};
    transition.writer = KisPageWriterToken{writer};
    transition.transaction = KisPageTransactionId{transaction};
    return transition;
}

KisPageStateSnapshot applyAccepted(const KisPageStateMachine &stateMachine,
                                   const KisPageStateSnapshot &state,
                                   const KisPageTransition &transition)
{
    const KisPageTransitionResult result = stateMachine.apply(state, transition);
    if (!result.accepted) {
        qWarning() << "unexpected transition rejection" << result.rejectionReason;
    }
    return result.next;
}

}

void KisPageStoreFoundationTest::layoutBaselineDoesNotGrow()
{
    // BR1 M0 arm64/Qt6 baselines from design 68.  The planned compact records
    // are allowed to shrink these values, but the pre-refactor representations
    // must not grow while the slot-arena replacement is being prepared.
    qInfo().nospace()
        << "BR1 layout baseline: KisReplicaHandle=" << sizeof(KisReplicaHandle)
        << "/" << alignof(KisReplicaHandle)
        << " KisPageTransition=" << sizeof(KisPageTransition)
        << "/" << alignof(KisPageTransition)
        << " KisPageAllocationDescriptor=" << sizeof(KisPageAllocationDescriptor)
        << "/" << alignof(KisPageAllocationDescriptor)
        << " KisPreparedPageProof=" << sizeof(KisPreparedPageProof)
        << "/" << alignof(KisPreparedPageProof)
        << " KisCpuWriteGuard=" << sizeof(KisCpuWriteGuard)
        << "/" << alignof(KisCpuWriteGuard);

    QVERIFY2(sizeof(KisReplicaHandle) <= 128,
             "KisReplicaHandle exceeded the BR1 M0 128-byte baseline");
    QVERIFY2(sizeof(KisPageTransition) <= 440,
             "KisPageTransition exceeded the BR1 M0 440-byte baseline");
    QVERIFY2(sizeof(KisPageAllocationDescriptor) <= 216,
             "KisPageAllocationDescriptor exceeded the BR1 M0 216-byte baseline");
    QVERIFY2(sizeof(KisPreparedPageProof) <= 176,
             "KisPreparedPageProof exceeded the BR1 M0 176-byte baseline");
    QVERIFY2(sizeof(KisCpuWriteGuard) <= 200,
             "KisCpuWriteGuard exceeded the BR1 M0 200-byte baseline");
}

void KisPageStoreFoundationTest::pageStoreFailsClosedAtTiles3Boundary()
{
    KisPageStore store;
    QVERIFY(!store.isOperational());

    const KisPageKey key{KisSurfaceId{1}, KisLogicalPageId{0, 0}};
    KisPageReadView view;

    const KisReadRequest read = store.acquireRead(
        key,
        view,
        {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
        KisPagePriority::Interactive);
    QCOMPARE(read.status, KisPageRequestStatus::Unsupported);
    QVERIFY(!read.id.isValid());
    QVERIFY(!read.version.isValid());
    QVERIFY(!read.readiness.isValid());
    QVERIFY(!read.error.isEmpty());

    const KisPageTransaction transaction{KisPageTransactionId{1}, KisImageEpochId{1}};
    const KisWriteRequest write = store.acquireWrite(
        transaction,
        key,
        {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
        KisPageWriteMode::PreserveContents,
        KisPagePriority::Interactive);
    QCOMPARE(write.status, KisPageRequestStatus::Unsupported);
    QVERIFY(!write.id.isValid());
    QVERIFY(!write.writer.isValid());
    QVERIFY(!write.readiness.isValid());
    QVERIFY(!write.error.isEmpty());

    QVERIFY(!store.resolve(read, {}).isValid());
    QVERIFY(!store.resolve(write, {}).isValid());
    QVERIFY(!store.beginTransaction(KisImageEpochId{1}).isValid());
    QVERIFY(!store.captureCommittedEpoch().isValid());
}

void KisPageStoreFoundationTest::requestDescriptorsRejectAmbiguousSelectors()
{
    const KisPageKey key = testPageKey();
    KisPageReadView view;
    QVERIFY(view.isValidFor(key));

    view.kind = KisPageReadViewKind::CommittedEpoch;
    view.epoch = KisImageEpochId{3};
    QVERIFY(!view.isValidFor(key));
    view.retention = KisImageEpochSnapshotToken{8};
    QVERIFY(view.isValidFor(key));
    view.exactVersion = testPageVersion(1);
    QVERIFY(!view.isValidFor(key));

    view = {};
    view.kind = KisPageReadViewKind::ExactVersion;
    view.epoch = KisImageEpochId{3};
    view.retention = KisImageEpochSnapshotToken{8};
    view.exactVersion = testPageVersion(1);
    QVERIFY(view.isValidFor(key));
    view.exactVersion.key.page.column++;
    QVERIFY(!view.isValidFor(key));

    KisPreparedPageSet prepared;
    prepared.transaction = KisPageTransactionId{7};
    KisCompletionRegistry completions;
    const KisCompletionDomain source = KisCompletionDomain::HostLogical;
    const quint64 completionSource = completions.registerSource(source);
    const KisCompletionTicket ready = completions.allocatePending(completionSource);
    prepared.proofs = {testPreparedProof(1, prepared.transaction, ready),
                       testPreparedProof(2, prepared.transaction, ready)};
    QVERIFY(!prepared.isValid());
    prepared.proofs = {testPreparedProof(2, prepared.transaction, ready)};
    QVERIFY(prepared.isValid());

    KisImageEpochSnapshot snapshot;
    snapshot.epoch = KisImageEpochId{9};
    snapshot.graphRevision = 1;
    snapshot.defaultPixelRevision = 1;
    snapshot.extentRevision = 1;
    snapshot.propertyRevision = 1;
    snapshot.manifest = {testPageVersion(1), testPageVersion(2)};
    QVERIFY(!snapshot.isValid());
}

void KisPageStoreFoundationTest::accessRequirementSeparatesUmaConsumers()
{
    const KisPageAccessRequirement cpuRam{
        KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
    const KisPageAccessRequirement umaCpu{
        KisPageAccessDomain::UmaShared, KisPageAccessKind::CpuPointer};
    const KisPageAccessRequirement umaGpu{
        KisPageAccessDomain::UmaShared, KisPageAccessKind::GpuBinding};
    const KisPageAccessRequirement vramGpu{
        KisPageAccessDomain::DiscreteVram, KisPageAccessKind::GpuBinding};
    const KisPageAccessRequirement directSsd{
        KisPageAccessDomain::Ssd, KisPageAccessKind::CpuPointer};

    QVERIFY(cpuRam.isValid());
    QVERIFY(umaCpu.isValid());
    QVERIFY(umaGpu.isValid());
    QVERIFY(vramGpu.isValid());
    QVERIFY(!directSsd.isValid());

    KisReplicaCapabilities capabilities;
    capabilities.domains = {KisPageAccessDomain::UmaShared};
    capabilities.consumerAccess = {umaCpu, umaGpu};
    QVERIFY(capabilities.isValid());
    QVERIFY(capabilities.supports(umaCpu));
    QVERIFY(capabilities.supports(umaGpu));
    QVERIFY(!capabilities.supports(cpuRam));
    capabilities.synchronousWriteCopy = true;
    QVERIFY(!capabilities.isValid());
    capabilities.synchronousOperations = true;
    QVERIFY(capabilities.isValid());

    KisReplicaCapabilities durableStorage;
    durableStorage.domains = {KisPageAccessDomain::Ssd};
    QVERIFY(durableStorage.isValid());
    QVERIFY(!durableStorage.supports(directSsd));
    durableStorage.synchronousOperations = true;
    durableStorage.synchronousWriteCopy = true;
    QVERIFY(!durableStorage.isValid());

    KisReplicaCapabilities inaccessibleRam;
    inaccessibleRam.domains = {KisPageAccessDomain::CpuRam};
    QVERIFY(!inaccessibleRam.isValid());

    quint8 byte = 0;
    KisReplicaAccess resolved;
    resolved.lease = KisPageLeaseId{1};
    resolved.operation = KisPageOperationId{2};
    resolved.replica = testReplica(1, 5, 7, 9, 1);
    resolved.replica.domain = KisPageAccessDomain::UmaShared;
    resolved.cpuReadData = &byte;
    QVERIFY(resolved.isValid(umaCpu));

    resolved.cpuReadData = nullptr;
    resolved.gpuAccess.providerId = 5;
    resolved.gpuAccess.providerEpoch = 7;
    resolved.gpuAccess.operationId = 2;
    resolved.gpuAccess.bindingTableGeneration = 1;
    resolved.gpuAccess.bindingIndex = 3;
    resolved.gpuAccess.byteSize = 4096;
    QVERIFY(resolved.isValid(umaGpu));
    resolved.gpuAccess.bindingIndex = 0;
    QVERIFY(resolved.isValid(umaGpu));
    resolved.gpuAccess.byteSize--;
    QVERIFY(!resolved.isValid(umaGpu));
    resolved.gpuAccess.byteSize++;
    resolved.gpuAccess.providerEpoch = 8;
    QVERIFY(!resolved.isValid());

    KisReplicaTransferRequest transfer;
    transfer.operation = KisPageOperationId{11};
    transfer.source = testReplica(1, 1, 1, 1, 1);
    transfer.target = testReplica(1, 1, 1, 2, 1);
    transfer.descriptor = testAllocationDescriptor();
    QVERIFY(transfer.isValid());
    QVERIFY(transfer.isSameProviderTransfer());
    KisReplicaTransferRequest crossProvider = transfer;
    crossProvider.target = testReplica(1, 2, 1, 2, 1);
    QVERIFY(crossProvider.isValid());
    QVERIFY(!crossProvider.isSameProviderTransfer());
    transfer.target.version = testPageVersion(2);
    QVERIFY(!transfer.isValid());
    transfer.kind = KisReplicaTransferKind::WriteGenerationInitialization;
    QVERIFY(transfer.isValid());
    transfer.target.version.key.page.row++;
    QVERIFY(!transfer.isValid());
}

void KisPageStoreFoundationTest::leaseCapabilitiesAreMoveOnly()
{
    QVERIFY(!std::is_copy_constructible_v<KisReadLease>);
    QVERIFY(!std::is_copy_assignable_v<KisReadLease>);
    QVERIFY(std::is_move_constructible_v<KisReadLease>);
    QVERIFY(!std::is_move_assignable_v<KisReadLease>);
    QVERIFY(!std::is_copy_constructible_v<KisWriteLease>);
    QVERIFY(!std::is_copy_assignable_v<KisWriteLease>);
    QVERIFY(std::is_move_constructible_v<KisWriteLease>);
    QVERIFY(!std::is_move_assignable_v<KisWriteLease>);
    QVERIFY(!std::is_copy_constructible_v<KisReplicaAccess>);
    QVERIFY(std::is_move_constructible_v<KisReplicaAccess>);
    QVERIFY(!std::is_move_assignable_v<KisReplicaAccess>);
}

void KisPageStoreFoundationTest::stateMachineCommitsPreparedGeneration()
{
    const KisPageStateMachine stateMachine;
    KisPageStateSnapshot state = initialPageState();

    QString invariantFailure;
    QVERIFY2(stateMachine.validateInvariants(state, &invariantFailure),
             qPrintable(invariantFailure));

    const KisPageTransition acquire = writeTransition(KisPageTransitionKind::AcquireWrite, 2);
    state = applyAccepted(stateMachine, state, acquire);
    QVERIFY(state.writer.isValid());
    QCOMPARE(state.versions.first().replicas.first().pinCount, quint32(1));
    QCOMPARE(state.publishedGeneration.value, quint64(1));

    KisPageTransition retireWriterTarget;
    retireWriterTarget.kind = KisPageTransitionKind::BeginRetire;
    retireWriterTarget.version = testPageVersion(2);
    retireWriterTarget.target = testReplica(2, 1, 1, 2, 1);
    retireWriterTarget.operation = KisPageOperationId{99};
    QVERIFY(!stateMachine.apply(state, retireWriterTarget).accepted);

    const KisPageTransitionResult conflict = stateMachine.apply(state, acquire);
    QVERIFY(!conflict.accepted);
    QVERIFY(conflict.rejectionReason.contains(QStringLiteral("writer")));
    QCOMPARE(conflict.next.publishedGeneration.value, quint64(1));

    state = applyAccepted(stateMachine, state,
                          writeTransition(KisPageTransitionKind::PrepareWrite, 2));
    state = applyAccepted(stateMachine, state,
                          writeTransition(KisPageTransitionKind::BeginPublish, 2));
    state = applyAccepted(stateMachine, state,
                          writeTransition(KisPageTransitionKind::PublishWrite, 2));
    QVERIFY(!state.writer.isValid());
    QCOMPARE(state.versions.first().replicas.first().pinCount, quint32(0));
    QCOMPARE(state.publishedGeneration.value, quint64(1));

    KisPageTransition preparedRead;
    preparedRead.kind = KisPageTransitionKind::AcquireRead;
    preparedRead.version = testPageVersion(2);
    preparedRead.target = testReplica(2, 1, 1, 2, 1);
    preparedRead.lease = KisPageLeaseId{77};
    QVERIFY(!stateMachine.apply(state, preparedRead).accepted);
    preparedRead.transaction = KisPageTransactionId{1};
    state = applyAccepted(stateMachine, state, preparedRead);
    preparedRead.kind = KisPageTransitionKind::ReleaseRead;
    state = applyAccepted(stateMachine, state, preparedRead);

    KisPageTransition commit;
    commit.kind = KisPageTransitionKind::CommitTransaction;
    commit.imageEpoch = KisImageEpochId{2};
    commit.version = testPageVersion(2);
    commit.transaction = KisPageTransactionId{1};
    state = applyAccepted(stateMachine, state, commit);

    QCOMPARE(state.publishedGeneration.value, quint64(2));
    QVERIFY2(stateMachine.validateInvariants(state, &invariantFailure),
             qPrintable(invariantFailure));
}

void KisPageStoreFoundationTest::stateMachineAdoptsSynchronousPreparedWriteAtomically()
{
    const KisPageStateMachine stateMachine;
    const KisPageStateSnapshot before = initialPageState();
    KisPageTransition adopt = writeTransition(
        KisPageTransitionKind::AdoptPreparedWrite, 2);
    adopt.writeMode = KisPageWriteMode::DiscardContents;

    const KisPageTransitionResult adopted = stateMachine.apply(before, adopt);
    QVERIFY2(adopted.accepted, qPrintable(adopted.rejectionReason));
    QCOMPARE(adopted.next.publishedGeneration.value, quint64(1));
    QCOMPARE(adopted.next.nextGeneration.value, quint64(3));
    QVERIFY(!adopted.next.writer.isValid());
    QCOMPARE(adopted.next.versions.size(), qsizetype(2));
    const KisPageVersionStateSnapshot &prepared = adopted.next.versions.last();
    QCOMPARE(prepared.version, testPageVersion(2));
    QCOMPARE(prepared.publication, KisPagePublicationState::Prepared);
    QCOMPARE(prepared.preparedBy, KisPageTransactionId{1});
    QCOMPARE(prepared.authority, adopt.target);
    QCOMPARE(prepared.replicas.size(), qsizetype(1));
    QCOMPARE(prepared.replicas.first().validity,
             KisReplicaValidity::Valid);

    QString invariantFailure;
    QVERIFY2(stateMachine.validateInvariants(
                 adopted.next, &invariantFailure),
             qPrintable(invariantFailure));

    KisPageTransition invalidMode = adopt;
    invalidMode.writeMode = KisPageWriteMode::PreserveContents;
    const KisPageTransitionResult rejected =
        stateMachine.apply(before, invalidMode);
    QVERIFY(!rejected.accepted);
    QCOMPARE(rejected.next.publishedGeneration,
             before.publishedGeneration);
    QCOMPARE(rejected.next.nextGeneration, before.nextGeneration);
    QCOMPARE(rejected.next.versions.size(), before.versions.size());
}

void KisPageStoreFoundationTest::
stateMachineSeparatesCommitAndIdleHistoryRetirement()
{
    const KisPageStateMachine stateMachine;
    KisPageTransition adopt = writeTransition(
        KisPageTransitionKind::AdoptPreparedWrite, 2);
    adopt.writeMode = KisPageWriteMode::DiscardContents;
    const KisPageStateSnapshot prepared =
        applyAccepted(stateMachine, initialPageState(), adopt);

    KisPageTransition commit;
    commit.kind = KisPageTransitionKind::CommitTransaction;
    commit.imageEpoch = KisImageEpochId{2};
    commit.version = testPageVersion(2);
    commit.transaction = KisPageTransactionId{1};
    const KisPageTransitionResult committed =
        stateMachine.apply(prepared, commit);
    QVERIFY2(committed.accepted, qPrintable(committed.rejectionReason));
    QCOMPARE(committed.next.publishedGeneration, KisPageGeneration{2});
    QCOMPARE(committed.next.versions.size(), qsizetype(2));
    QVERIFY(committed.effects.isEmpty());
    KisPageTransition discard;
    discard.kind = KisPageTransitionKind::DiscardHistoricalVersions;
    discard.versions = {testPageVersion(1)};
    const auto retired = stateMachine.apply(committed.next, discard);
    QVERIFY(retired.accepted);
    QCOMPARE(retired.next.versions.size(), qsizetype(1));
    QCOMPARE(retired.next.versions.first().version, testPageVersion(2));
    QCOMPARE(retired.effects.size(), qsizetype(1));
    QCOMPARE(retired.effects.first().replica.version, testPageVersion(1));

    KisPageStateSnapshot leased = initialPageState();
    KisPageTransition acquireRead;
    acquireRead.kind = KisPageTransitionKind::AcquireRead;
    acquireRead.version = testPageVersion(1);
    acquireRead.target = testReplica(1, 1, 1, 1, 1);
    acquireRead.lease = KisPageLeaseId{77};
    leased = applyAccepted(stateMachine, leased, acquireRead);
    leased = applyAccepted(stateMachine, leased, adopt);
    const KisPageTransitionResult published = stateMachine.apply(leased, commit);
    QVERIFY(published.accepted);
    const KisPageTransitionResult rejected = stateMachine.apply(published.next, discard);
    QVERIFY(!rejected.accepted);
    QCOMPARE(rejected.next.publishedGeneration, KisPageGeneration{2});
}

void KisPageStoreFoundationTest::stateMachineVirtualDefaultWriterHasLogicalBeforeImage()
{
    const KisPageStateMachine machine;
    auto state = initialPageState();
    state.publishedDefaultPixelRevision = 7;
    state.versions[0].version.defaultPixelRevision = 7;
    state.versions[0].replicas.clear();
    state.versions[0].authority = {};
    QVERIFY(machine.validateInvariants(state));
    auto acquire = writeTransition(KisPageTransitionKind::AcquireWrite, 2);
    acquire.source = {};
    acquire.baseVersion = state.versions[0].version;
    acquire.writeMode = KisPageWriteMode::DiscardContents;
    auto invalid = acquire;
    invalid.writeMode = KisPageWriteMode::PreserveContents;
    QVERIFY(!machine.apply(state, invalid).accepted);
    invalid = acquire;
    invalid.baseVersion.defaultPixelRevision = 8;
    QVERIFY(!machine.apply(state, invalid).accepted);
    auto missingNonDefault = writeTransition(KisPageTransitionKind::AcquireWrite, 2);
    missingNonDefault.source = {};
    missingNonDefault.writeMode = KisPageWriteMode::DiscardContents;
    QVERIFY(!machine.apply(initialPageState(), missingNonDefault).accepted);
    const auto reserved = applyAccepted(machine, state, acquire);
    QVERIFY(reserved.writer.isValid());
    QVERIFY(!reserved.writer.baseAuthority.isValid());
    QVERIFY(reserved.versions.first().isVirtualDefault());
    QCOMPARE(reserved.versions.first().version.defaultPixelRevision, quint64(7));
    auto step = acquire;
    step.kind = KisPageTransitionKind::PrepareWrite;
    const auto writable = applyAccepted(machine, reserved, step);
    KisPageTransition materialize;
    materialize.kind = KisPageTransitionKind::MaterializeDefault;
    materialize.version = acquire.baseVersion;
    materialize.target = testReplica(1, 1, 1, 4, 1);
    materialize.target.version = acquire.baseVersion;
    const auto readableBefore = applyAccepted(machine, writable, materialize);
    QVERIFY(!readableBefore.versions.first().isVirtualDefault());
    QVERIFY(!readableBefore.writer.baseAuthority.isValid());
    step.kind = KisPageTransitionKind::CancelWrite;
    QVERIFY(machine.apply(readableBefore, step).accepted);
    step.kind = KisPageTransitionKind::BeginPublish;
    const auto readablePublishing = applyAccepted(machine, readableBefore, step);
    step.kind = KisPageTransitionKind::FailWrite;
    QVERIFY(machine.apply(readablePublishing, step).accepted);
    step.kind = KisPageTransitionKind::PublishWrite;
    QVERIFY(machine.apply(readablePublishing, step).accepted);
    step.kind = KisPageTransitionKind::CancelWrite;
    const auto cancelled = machine.apply(writable, step);
    QVERIFY(cancelled.accepted);
    QCOMPARE(cancelled.next.versions.size(), qsizetype(1));
    QCOMPARE(cancelled.effects.size(), qsizetype(1));
    QVERIFY(cancelled.next.versions.first().isVirtualDefault());
    step.kind = KisPageTransitionKind::BeginPublish;
    const auto publishing = applyAccepted(machine, writable, step);
    step.kind = KisPageTransitionKind::FailWrite;
    const auto failed = machine.apply(publishing, step);
    QVERIFY(failed.accepted);
    QVERIFY(failed.next.versions.first().isVirtualDefault());
    QCOMPARE(failed.next.versions.size(), qsizetype(1));
    step.kind = KisPageTransitionKind::PublishWrite;
    const auto prepared = applyAccepted(machine, publishing, step);
    QVERIFY(!prepared.writer.isValid());
    QVERIFY(prepared.versions.first().isVirtualDefault());
    QCOMPARE(prepared.versions.last().publication, KisPagePublicationState::Prepared);
    step.kind = KisPageTransitionKind::CommitTransaction;
    step.imageEpoch = {2};
    const auto committed = applyAccepted(machine, prepared, step);
    QCOMPARE(committed.publishedGeneration.value, quint64(2));
    QVERIFY(committed.versions.first().isVirtualDefault());
    QVERIFY(machine.validateInvariants(committed));
}

void KisPageStoreFoundationTest::stateMachineCommitsLatestTransactionGeneration()
{
    const KisPageStateMachine stateMachine;
    KisPageStateSnapshot state = initialPageState();

    state = applyAccepted(stateMachine, state,
                          writeTransition(KisPageTransitionKind::AcquireWrite, 2));
    state = applyAccepted(stateMachine, state,
                          writeTransition(KisPageTransitionKind::PrepareWrite, 2));
    state = applyAccepted(stateMachine, state,
                          writeTransition(KisPageTransitionKind::BeginPublish, 2));
    state = applyAccepted(stateMachine, state,
                          writeTransition(KisPageTransitionKind::PublishWrite, 2));

    KisPageTransition third = writeTransition(KisPageTransitionKind::AcquireWrite,
                                              3,
                                              1,
                                              11,
                                              21);
    state = applyAccepted(stateMachine, state, third);
    third.kind = KisPageTransitionKind::PrepareWrite;
    state = applyAccepted(stateMachine, state, third);
    third.kind = KisPageTransitionKind::BeginPublish;
    state = applyAccepted(stateMachine, state, third);
    third.kind = KisPageTransitionKind::PublishWrite;
    state = applyAccepted(stateMachine, state, third);

    KisPageTransition discardPrepared;
    discardPrepared.kind = KisPageTransitionKind::DiscardHistoricalVersions;
    discardPrepared.versions = {testPageVersion(3)};
    QVERIFY(!stateMachine.apply(state, discardPrepared).accepted);
    KisPageTransition detach;
    detach.kind = KisPageTransitionKind::DetachPreparedVersion;
    detach.transaction = {1};
    detach.version = testPageVersion(2);
    state = applyAccepted(stateMachine, state, detach);
    discardPrepared.versions = {testPageVersion(2)};
    const KisPageTransitionResult discarded =
        stateMachine.apply(state, discardPrepared);
    QVERIFY2(discarded.accepted, qPrintable(discarded.rejectionReason));
    QCOMPARE(discarded.effects.size(), 1);
    QCOMPARE(discarded.effects.first().replica.version,
             testPageVersion(2));
    state = discarded.next;

    KisPageTransition commit;
    commit.kind = KisPageTransitionKind::CommitTransaction;
    commit.imageEpoch = KisImageEpochId{2};
    commit.version = testPageVersion(2);
    commit.transaction = KisPageTransactionId{1};
    QVERIFY(!stateMachine.apply(state, commit).accepted);

    commit.version = testPageVersion(3);
    state = applyAccepted(stateMachine, state, commit);
    QCOMPARE(state.publishedGeneration.value, quint64(3));

    int historicalCount = 0;
    for (const KisPageVersionStateSnapshot &version : state.versions) {
        if (version.publication == KisPagePublicationState::Historical) {
            historicalCount++;
        }
        QVERIFY(!version.preparedBy.isValid());
    }
    QCOMPARE(historicalCount, 1);
}

void KisPageStoreFoundationTest::
stateMachineDiscardsOnlyIdleHistoricalVersions()
{
    const KisPageStateMachine stateMachine;
    KisPageStateSnapshot state = initialPageState();

    for (quint64 generation = 2; generation <= 3; ++generation) {
        KisPageTransition write = writeTransition(
            KisPageTransitionKind::AcquireWrite,
            generation,
            1,
            10 + generation,
            20 + generation);
        state = applyAccepted(stateMachine, state, write);
        write.kind = KisPageTransitionKind::PrepareWrite;
        state = applyAccepted(stateMachine, state, write);
        write.kind = KisPageTransitionKind::BeginPublish;
        state = applyAccepted(stateMachine, state, write);
        write.kind = KisPageTransitionKind::PublishWrite;
        state = applyAccepted(stateMachine, state, write);
    }

    KisPageTransition commit;
    commit.kind = KisPageTransitionKind::CommitTransaction;
    commit.imageEpoch = KisImageEpochId{2};
    commit.version = testPageVersion(3);
    commit.transaction = KisPageTransactionId{1};
    state = applyAccepted(stateMachine, state, commit);

    KisPageTransition invalidDiscard;
    invalidDiscard.kind = KisPageTransitionKind::DiscardHistoricalVersions;
    invalidDiscard.versions = {testPageVersion(3)};
    QVERIFY(!stateMachine.apply(state, invalidDiscard).accepted);

    KisPageTransition discard;
    discard.kind = KisPageTransitionKind::DiscardHistoricalVersions;
    discard.versions = {testPageVersion(1), testPageVersion(2)};
    const KisPageTransitionResult result = stateMachine.apply(state, discard);
    QVERIFY2(result.accepted, qPrintable(result.rejectionReason));
    QCOMPARE(result.next.versions.size(), 1);
    QCOMPARE(result.next.versions.first().version, testPageVersion(3));
    QCOMPARE(result.effects.size(), 2);
    for (const KisPageTransitionEffect &effect : result.effects) {
        QVERIFY(effect.replica.version == testPageVersion(1) ||
                effect.replica.version == testPageVersion(2));
    }

    QString invariantFailure;
    QVERIFY2(stateMachine.validateInvariants(result.next, &invariantFailure),
             qPrintable(invariantFailure));
}

void KisPageStoreFoundationTest::stateMachineRestoresHistoricalCommittedGeneration()
{
    const KisPageStateMachine stateMachine;
    KisPageStateSnapshot state = initialPageState();
    state = applyAccepted(stateMachine, state,
                          writeTransition(KisPageTransitionKind::AcquireWrite, 2));
    state = applyAccepted(stateMachine, state,
                          writeTransition(KisPageTransitionKind::PrepareWrite, 2));
    state = applyAccepted(stateMachine, state,
                          writeTransition(KisPageTransitionKind::BeginPublish, 2));
    state = applyAccepted(stateMachine, state,
                          writeTransition(KisPageTransitionKind::PublishWrite, 2));

    KisPageTransition commit;
    commit.kind = KisPageTransitionKind::CommitTransaction;
    commit.imageEpoch = KisImageEpochId{2};
    commit.version = testPageVersion(2);
    commit.transaction = KisPageTransactionId{1};
    state = applyAccepted(stateMachine, state, commit);

    KisPageTransition restore;
    restore.kind = KisPageTransitionKind::RestoreCommittedVersion;
    restore.imageEpoch = KisImageEpochId{3};
    restore.version = testPageVersion(1);
    state = applyAccepted(stateMachine, state, restore);
    QCOMPARE(state.publishedEpoch.value, quint64(3));
    QCOMPARE(state.publishedGeneration.value, quint64(1));
    QCOMPARE(state.nextGeneration.value, quint64(3));

    restore.imageEpoch = KisImageEpochId{4};
    restore.version = testPageVersion(2);
    state = applyAccepted(stateMachine, state, restore);
    QCOMPARE(state.publishedEpoch.value, quint64(4));
    QCOMPARE(state.publishedGeneration.value, quint64(2));
    QCOMPARE(state.nextGeneration.value, quint64(3));
}

void KisPageStoreFoundationTest::stateMachineVirtualDefaultRemainsExactWithoutAllocation()
{
    const KisPageStateMachine machine;
    KisPageStateSnapshot state = initialPageState();
    const auto highWater = state.nextGeneration;
    KisPageTransition restore;
    restore.kind = KisPageTransitionKind::RestoreCommittedVersion;
    restore.version = {state.key, KisPageGeneration{1}, 7};
    restore.imageEpoch = KisImageEpochId{state.publishedEpoch.value + 1};
    state = applyAccepted(machine, state, restore);
    QVERIFY(state.versions.last().isVirtualDefault());
    QCOMPARE(state.nextGeneration, highWater);
    QCOMPARE(state.publishedDefaultPixelRevision, quint64(7));
    QString error;
    QVERIFY2(machine.validateInvariants(state, &error), qPrintable(error));

    KisPageStateSnapshot invalid = state;
    invalid.versions.last().version.defaultPixelRevision = 0;
    invalid.publishedDefaultPixelRevision = 0;
    QVERIFY(!machine.validateInvariants(invalid));
    KisPageTransition materialize;
    materialize.kind = KisPageTransitionKind::MaterializeDefault;
    materialize.version = restore.version;
    materialize.target = state.versions.first().authority;
    // Another version's allocation cannot be used as the default's bytes.
    QVERIFY(!machine.apply(state, materialize).accepted);
    materialize.target.version = restore.version;
    QVERIFY(!machine.apply(state, materialize).accepted); // same physical slot
    materialize.target.allocation.slot += 100;
    state = applyAccepted(machine, state, materialize);
    QVERIFY(!state.versions.last().isVirtualDefault());
    QCOMPARE(state.nextGeneration, highWater);
    QVERIFY(!machine.apply(state, materialize).accepted); // duplicate completion

    restore.version.defaultPixelRevision = 8;
    restore.imageEpoch.value++;
    state = applyAccepted(machine, state, restore);
    QVERIFY(state.versions.last().isVirtualDefault());
    QCOMPARE(state.nextGeneration, highWater);
    restore.version.defaultPixelRevision = 9;
    restore.imageEpoch.value++;
    state = applyAccepted(machine, state, restore);
    KisPageTransition discard;
    discard.kind = KisPageTransitionKind::DiscardHistoricalVersions;
    discard.versions = {{state.key, KisPageGeneration{1}, 8}};
    const auto result = machine.apply(state, discard);
    QVERIFY2(result.accepted, qPrintable(result.rejectionReason));
    QVERIFY(result.effects.isEmpty());
}

void KisPageStoreFoundationTest::stateMachineHistoricalDefaultDoesNotChangeWriter()
{
    const KisPageStateMachine machine;
    for (int phase = 0; phase < 3; ++phase) {
        auto state = applyAccepted(machine, initialPageState(), writeTransition(KisPageTransitionKind::AcquireWrite, 2));
        if (phase) state = applyAccepted(machine, state, writeTransition(KisPageTransitionKind::PrepareWrite, 2));
        if (phase == 2) {
            state = applyAccepted(machine, state, writeTransition(KisPageTransitionKind::BeginPublish, 2));
            state = applyAccepted(machine, state, writeTransition(KisPageTransitionKind::PublishWrite, 2));
        }
        const auto writer = state.writer;
        const auto nextGeneration = state.nextGeneration;
        const auto published = state.publishedGeneration;
        KisPageTransition attach;
        attach.kind = KisPageTransitionKind::AttachHistoricalDefault;
        attach.version = {state.key, KisPageGeneration{1}, 7};
        // Neither the before authority nor the writer target can supply the
        // new default's bytes; a new allocation generation at that slot is
        // not permission to reuse a still-live physical identity.
        for (quint64 slot : {1ULL, 2ULL}) {
            attach.target = testReplica(1, 1, 1, slot, 2);
            attach.target.version = attach.version;
            QVERIFY(!machine.apply(state, attach).accepted);
        }
        attach.target = {};
        state = applyAccepted(machine, state, attach);
        QVERIFY(state.versions.last().isVirtualDefault());
        QVERIFY(!machine.apply(state, attach).accepted);
        KisPageTransition materialize;
        materialize.kind = KisPageTransitionKind::MaterializeDefault;
        materialize.version = attach.version;
        materialize.target = testReplica(1, 1, 1, 99, 1);
        materialize.target.version = attach.version;
        state = applyAccepted(machine, state, materialize);
        KisPageTransition read;
        read.kind = KisPageTransitionKind::AcquireRead;
        read.version = attach.version; read.target = materialize.target; read.lease = {77};
        state = applyAccepted(machine, state, read);
        read.kind = KisPageTransitionKind::ReleaseRead;
        state = applyAccepted(machine, state, read);
        QCOMPARE(state.writer.token, writer.token);
        QCOMPARE(state.writer.phase, writer.phase);
        QCOMPARE(state.writer.target, writer.target);
        QCOMPARE(state.writer.baseAuthority, writer.baseAuthority);
        QCOMPARE(state.publishedGeneration, published);
        QCOMPARE(state.nextGeneration, nextGeneration);
        QVERIFY(machine.validateInvariants(state));
    }
}

void KisPageStoreFoundationTest::stateMachineCancelPreservesPublishedGeneration()
{
    KisPageStateMachine stateMachine;
    KisPageStateSnapshot state = initialPageState();
    state = applyAccepted(stateMachine, state,
                          writeTransition(KisPageTransitionKind::AcquireWrite, 2));
    state = applyAccepted(stateMachine, state,
                          writeTransition(KisPageTransitionKind::PrepareWrite, 2));
    state = applyAccepted(stateMachine, state,
                          writeTransition(KisPageTransitionKind::CancelWrite, 2));

    QString invariantFailure;
    QVERIFY2(stateMachine.validateInvariants(state, &invariantFailure),
             qPrintable(invariantFailure));
    QCOMPARE(state.publishedGeneration.value, quint64(1));
    QCOMPARE(state.nextGeneration.value, quint64(3));
    QCOMPARE(state.versions.size(), 1);
    QVERIFY(!state.writer.isValid());

    KisPageTransition pending = writeTransition(KisPageTransitionKind::AcquireWrite,
                                                3,
                                                2,
                                                12,
                                                22);
    pending.baseVersion = testPageVersion(1);
    pending.source = testReplica(1, 1, 1, 1, 1);
    state = applyAccepted(stateMachine, state, pending);
    pending.kind = KisPageTransitionKind::PrepareWrite;
    state = applyAccepted(stateMachine, state, pending);
    pending.kind = KisPageTransitionKind::BeginPublish;
    state = applyAccepted(stateMachine, state, pending);
    pending.kind = KisPageTransitionKind::CancelWrite;
    state = applyAccepted(stateMachine, state, pending);
    QCOMPARE(state.writer.phase, KisPageWriterPhase::CancelPending);
    QCOMPARE(state.publishedGeneration.value, quint64(1));

    pending.kind = KisPageTransitionKind::PublishWrite;
    state = applyAccepted(stateMachine, state, pending);
    QVERIFY(!state.writer.isValid());
    QCOMPARE(state.publishedGeneration.value, quint64(1));
    QCOMPARE(state.nextGeneration.value, quint64(4));
    QCOMPARE(state.versions.size(), 1);

    KisPageTransition failed = writeTransition(KisPageTransitionKind::AcquireWrite,
                                               4,
                                               3,
                                               13,
                                               23);
    failed.baseVersion = testPageVersion(1);
    failed.source = testReplica(1, 1, 1, 1, 1);
    state = applyAccepted(stateMachine, state, failed);
    failed.kind = KisPageTransitionKind::PrepareWrite;
    state = applyAccepted(stateMachine, state, failed);
    failed.kind = KisPageTransitionKind::BeginPublish;
    state = applyAccepted(stateMachine, state, failed);
    failed.kind = KisPageTransitionKind::FailWrite;
    state = applyAccepted(stateMachine, state, failed);
    QCOMPARE(state.publishedGeneration.value, quint64(1));
    QCOMPARE(state.nextGeneration.value, quint64(5));
    QCOMPARE(state.versions.size(), 1);
}

void KisPageStoreFoundationTest::stateMachineRejectsStaleCompletionAndAba()
{
    const KisPageStateMachine stateMachine;
    KisPageStateSnapshot state = initialPageState();
    const KisReplicaHandle source = testReplica(1, 1, 1, 1, 1);
    const KisReplicaHandle target = testReplica(1, 2, 7, 9, 3);

    KisPageTransition begin;
    begin.kind = KisPageTransitionKind::BeginMaterialize;
    begin.version = testPageVersion(1);
    begin.source = source;
    begin.target = target;
    begin.operation = KisPageOperationId{31};

    KisPageTransition aliasedSlot = begin;
    aliasedSlot.target = testReplica(1, 1, 1, 1, 2);
    QVERIFY(!stateMachine.apply(state, aliasedSlot).accepted);

    state = applyAccepted(stateMachine, state, begin);

    KisPageTransition duplicateOperation = begin;
    duplicateOperation.target = testReplica(1, 3, 1, 10, 1);
    QVERIFY(!stateMachine.apply(state, duplicateOperation).accepted);

    KisPageTransition stale = begin;
    stale.kind = KisPageTransitionKind::CompleteMaterialize;
    stale.operation = KisPageOperationId{30};
    KisPageTransitionResult result = stateMachine.apply(state, stale);
    QVERIFY(!result.accepted);
    QCOMPARE(result.next.publishedGeneration.value, quint64(1));

    stale = begin;
    stale.kind = KisPageTransitionKind::CompleteMaterialize;
    stale.target.providerEpoch = KisReplicaProviderEpoch{8};
    result = stateMachine.apply(state, stale);
    QVERIFY(!result.accepted);

    KisPageTransition complete = begin;
    complete.kind = KisPageTransitionKind::CompleteMaterialize;
    state = applyAccepted(stateMachine, state, complete);

    KisPageTransition retire;
    retire.kind = KisPageTransitionKind::BeginRetire;
    retire.version = testPageVersion(1);
    retire.target = target;
    retire.operation = KisPageOperationId{32};
    state = applyAccepted(stateMachine, state, retire);

    KisPageTransition abaCompletion = retire;
    abaCompletion.kind = KisPageTransitionKind::CompleteRetire;
    abaCompletion.target.allocation.generation++;
    result = stateMachine.apply(state, abaCompletion);
    QVERIFY(!result.accepted);

    retire.kind = KisPageTransitionKind::CompleteRetire;
    state = applyAccepted(stateMachine, state, retire);
    QCOMPARE(state.versions.first().replicas.size(), 1);
}

void KisPageStoreFoundationTest::stateMachineLeaseBlocksRetirement()
{
    const KisPageStateMachine stateMachine;
    KisPageStateSnapshot state = initialPageState();
    const KisReplicaHandle source = testReplica(1, 1, 1, 1, 1);
    const KisReplicaHandle target = testReplica(1, 2, 1, 2, 1);

    KisPageTransition materialize;
    materialize.kind = KisPageTransitionKind::BeginMaterialize;
    materialize.version = testPageVersion(1);
    materialize.source = source;
    materialize.target = target;
    materialize.operation = KisPageOperationId{41};
    state = applyAccepted(stateMachine, state, materialize);
    materialize.kind = KisPageTransitionKind::CompleteMaterialize;
    state = applyAccepted(stateMachine, state, materialize);

    KisPageTransition acquireRead;
    acquireRead.kind = KisPageTransitionKind::AcquireRead;
    acquireRead.version = testPageVersion(1);
    acquireRead.target = target;
    acquireRead.lease = KisPageLeaseId{51};
    state = applyAccepted(stateMachine, state, acquireRead);

    KisPageTransition retire;
    retire.kind = KisPageTransitionKind::BeginRetire;
    retire.version = testPageVersion(1);
    retire.target = target;
    retire.operation = KisPageOperationId{42};
    const KisPageTransitionResult blocked = stateMachine.apply(state, retire);
    QVERIFY(!blocked.accepted);

    KisCompletionRegistry completions;
    const KisCompletionDomain sourceDescriptor = KisCompletionDomain::CpuJob;
    const quint64 completionSource = completions.registerSource(sourceDescriptor);
    const KisCompletionTicket lastUse =
        completions.allocatePending(completionSource);

    acquireRead.kind = KisPageTransitionKind::ReleaseRead;
    acquireRead.completion = lastUse;
    state = applyAccepted(stateMachine, state, acquireRead);
    QVERIFY(!stateMachine.apply(state, retire).accepted);

    QVERIFY(completions.complete(lastUse, KisCompletionStatus::Succeeded));
    KisPageTransition acknowledge;
    acknowledge.kind = KisPageTransitionKind::AcknowledgeLastUse;
    acknowledge.version = testPageVersion(1);
    acknowledge.target = target;
    acknowledge.completion =
        completions.verifyTerminal(lastUse).ticket();
    state = applyAccepted(stateMachine, state, acknowledge);
    state = applyAccepted(stateMachine, state, retire);
    retire.kind = KisPageTransitionKind::CompleteRetire;
    state = applyAccepted(stateMachine, state, retire);

    QString invariantFailure;
    QVERIFY2(stateMachine.validateInvariants(state, &invariantFailure),
             qPrintable(invariantFailure));
}

void KisPageStoreFoundationTest::stateMachineHandoffAndFailurePreserveAuthority()
{
    const KisPageStateMachine stateMachine;
    KisPageStateSnapshot state = initialPageState();
    const KisReplicaHandle source = testReplica(1, 1, 1, 1, 1);
    const KisReplicaHandle failedTarget = testReplica(1, 2, 1, 2, 1);

    KisPageTransition materialize;
    materialize.kind = KisPageTransitionKind::BeginMaterialize;
    materialize.version = testPageVersion(1);
    materialize.source = source;
    materialize.target = failedTarget;
    materialize.operation = KisPageOperationId{61};
    state = applyAccepted(stateMachine, state, materialize);
    materialize.kind = KisPageTransitionKind::FailMaterialize;
    state = applyAccepted(stateMachine, state, materialize);

    QVERIFY(state.versions.first().authority == source);
    QCOMPARE(state.versions.first().replicas.first().pinCount, quint32(0));
    QCOMPARE(state.versions.first().replicas.last().validity, KisReplicaValidity::Failed);

    KisPageTransition retireFailed;
    retireFailed.kind = KisPageTransitionKind::BeginRetire;
    retireFailed.version = testPageVersion(1);
    retireFailed.target = failedTarget;
    retireFailed.operation = KisPageOperationId{62};
    state = applyAccepted(stateMachine, state, retireFailed);
    retireFailed.kind = KisPageTransitionKind::CompleteRetire;
    state = applyAccepted(stateMachine, state, retireFailed);

    const KisReplicaHandle replacement = testReplica(1, 3, 4, 3, 1);
    materialize.kind = KisPageTransitionKind::BeginMaterialize;
    materialize.target = replacement;
    materialize.operation = KisPageOperationId{63};
    state = applyAccepted(stateMachine, state, materialize);
    materialize.kind = KisPageTransitionKind::CompleteMaterialize;
    state = applyAccepted(stateMachine, state, materialize);

    KisPageTransition handoff;
    handoff.kind = KisPageTransitionKind::BeginAuthorityHandoff;
    handoff.version = testPageVersion(1);
    handoff.source = source;
    handoff.target = replacement;
    handoff.operation = KisPageOperationId{64};
    state = applyAccepted(stateMachine, state, handoff);
    QVERIFY(state.versions.first().authority == source);

    handoff.kind = KisPageTransitionKind::FailAuthorityHandoff;
    state = applyAccepted(stateMachine, state, handoff);
    QVERIFY(state.versions.first().authority == source);
    QCOMPARE(state.versions.first().replicas.first().pinCount, quint32(0));
    QCOMPARE(state.versions.first().replicas.last().pinCount, quint32(0));

    handoff.kind = KisPageTransitionKind::BeginAuthorityHandoff;
    handoff.operation = KisPageOperationId{65};
    state = applyAccepted(stateMachine, state, handoff);
    handoff.kind = KisPageTransitionKind::CommitAuthorityHandoff;
    state = applyAccepted(stateMachine, state, handoff);
    QVERIFY(state.versions.first().authority == replacement);

    KisPageTransition retireSource;
    retireSource.kind = KisPageTransitionKind::BeginRetire;
    retireSource.version = testPageVersion(1);
    retireSource.target = source;
    retireSource.operation = KisPageOperationId{66};
    state = applyAccepted(stateMachine, state, retireSource);
    retireSource.kind = KisPageTransitionKind::FailRetire;
    state = applyAccepted(stateMachine, state, retireSource);
    QCOMPARE(state.versions.first().replicas.first().validity,
             KisReplicaValidity::Failed);

    retireSource.kind = KisPageTransitionKind::BeginRetire;
    retireSource.operation = KisPageOperationId{67};
    state = applyAccepted(stateMachine, state, retireSource);
    retireSource.kind = KisPageTransitionKind::CompleteRetire;
    state = applyAccepted(stateMachine, state, retireSource);

    QString invariantFailure;
    QVERIFY2(stateMachine.validateInvariants(state, &invariantFailure),
             qPrintable(invariantFailure));
    QCOMPARE(state.versions.first().replicas.size(), 1);
}

void KisPageStoreFoundationTest::stateMachineAbortDropsOnlyPreparedGeneration()
{
    const KisPageStateMachine stateMachine;
    KisPageStateSnapshot state = initialPageState();
    state = applyAccepted(stateMachine, state,
                          writeTransition(KisPageTransitionKind::AcquireWrite, 2));
    state = applyAccepted(stateMachine, state,
                          writeTransition(KisPageTransitionKind::PrepareWrite, 2));
    state = applyAccepted(stateMachine, state,
                          writeTransition(KisPageTransitionKind::BeginPublish, 2));
    state = applyAccepted(stateMachine, state,
                          writeTransition(KisPageTransitionKind::PublishWrite, 2));

    KisPageTransition abort;
    abort.kind = KisPageTransitionKind::AbortTransaction;
    abort.transaction = KisPageTransactionId{1};
    state = applyAccepted(stateMachine, state, abort);

    QString invariantFailure;
    QVERIFY2(stateMachine.validateInvariants(state, &invariantFailure),
             qPrintable(invariantFailure));
    QCOMPARE(state.publishedGeneration.value, quint64(1));
    QCOMPARE(state.nextGeneration.value, quint64(3));
    QCOMPARE(state.versions.size(), 1);
}

void KisPageStoreFoundationTest::stateMachineCapturedVersionRetention()
{
    const KisPageStateMachine sm;
    auto state = initialPageState();
    for (auto kind : {KisPageTransitionKind::AcquireWrite, KisPageTransitionKind::PrepareWrite,
                      KisPageTransitionKind::BeginPublish, KisPageTransitionKind::PublishWrite})
        state = applyAccepted(sm, state, writeTransition(kind, 2));
    KisPageTransition retain;
    retain.kind = KisPageTransitionKind::RetainCapturedVersion;
    retain.version = testPageVersion(2); retain.transaction = {1}; retain.readView = {71};
    auto foreign = retain; foreign.transaction = {2};
    QVERIFY(!sm.apply(state, foreign).accepted);
    foreign = retain; foreign.version = testPageVersion(1);
    QVERIFY(!sm.apply(state, foreign).accepted);
    auto result = sm.apply(state, retain); QVERIFY(result.accepted); state = result.next;
    QVERIFY(!sm.apply(state, retain).accepted);
    retain.readView = {72}; result = sm.apply(state, retain); QVERIFY(result.accepted); state = result.next;
    KisPageTransition discard;
    discard.kind = KisPageTransitionKind::DiscardHistoricalVersions;
    discard.versions = {testPageVersion(2)};
    QVERIFY(!sm.apply(state, discard).accepted);
    KisPageTransition abort; abort.kind = KisPageTransitionKind::AbortTransaction; abort.transaction = {1};
    result = sm.apply(state, abort); QVERIFY(result.accepted); QVERIFY(result.effects.isEmpty()); state = result.next;
    QCOMPARE(state.versions.last().publication, KisPagePublicationState::Historical);
    QVERIFY(!state.versions.last().preparedBy.isValid());
    QVERIFY(!sm.apply(state, discard).accepted);
    KisPageTransition release = retain; release.kind = KisPageTransitionKind::ReleaseCapturedVersion;
    release.readView = {70}; QVERIFY(!sm.apply(state, release).accepted);
    for (quint64 token : {71, 72}) {
        release.readView = {token}; result = sm.apply(state, release); QVERIFY(result.accepted); state = result.next;
        QVERIFY(!sm.apply(state, release).accepted);
        if (token == 71) QVERIFY(!sm.apply(state, discard).accepted);
    }
    result = sm.apply(state, discard); QVERIFY(result.accepted); QCOMPARE(result.effects.size(), 1);
    QVERIFY(sm.validateInvariants(result.next));
}

void KisPageStoreFoundationTest::stateMachineRandomSequencePreservesInvariants()
{
    const KisPageStateMachine stateMachine;
    KisPageStateSnapshot state = initialPageState();
    quint64 nextOperation = 100;
    quint64 nextWriter = 1000;
    quint64 nextTransaction = 2000;
    quint64 nextLease = 3000;
    quint32 seed = 0x4b525431U;

    auto nextRandom = [&seed]() {
        seed = seed * 1664525U + 1013904223U;
        return seed;
    };

    for (int i = 0; i < 512; ++i) {
        QString invariantFailure;
        QVERIFY2(stateMachine.validateInvariants(state, &invariantFailure),
                 qPrintable(invariantFailure));

        const KisPageVersion published = testPageVersion(state.publishedGeneration.value);
        const KisPageVersionStateSnapshot *publishedState = nullptr;
        for (const KisPageVersionStateSnapshot &candidate : state.versions) {
            if (candidate.version == published) {
                publishedState = &candidate;
                break;
            }
        }
        QVERIFY(publishedState);
        const KisReplicaHandle authority = publishedState->authority;

        if ((nextRandom() & 3U) == 0U) {
            KisPageTransition read;
            read.kind = KisPageTransitionKind::AcquireRead;
            read.version = published;
            read.target = authority;
            read.lease = KisPageLeaseId{nextLease++};
            state = applyAccepted(stateMachine, state, read);

            KisPageTransition staleRelease = read;
            staleRelease.kind = KisPageTransitionKind::ReleaseRead;
            staleRelease.lease = KisPageLeaseId{nextLease++};
            const KisPageTransitionResult rejected = stateMachine.apply(state, staleRelease);
            QVERIFY(!rejected.accepted);

            read.kind = KisPageTransitionKind::ReleaseRead;
            state = applyAccepted(stateMachine, state, read);
            continue;
        }

        const quint64 generation = state.nextGeneration.value;
        const quint64 operation = nextOperation++;
        const quint64 writer = nextWriter++;
        const quint64 transaction = nextTransaction++;
        KisPageTransition acquire = writeTransition(KisPageTransitionKind::AcquireWrite,
                                                    generation,
                                                    transaction,
                                                    operation,
                                                    writer);
        acquire.baseVersion = published;
        acquire.source = authority;
        acquire.target = testReplica(generation, 1, 1, generation, 1);
        state = applyAccepted(stateMachine, state, acquire);

        KisPageTransition stale = acquire;
        stale.kind = KisPageTransitionKind::PrepareWrite;
        stale.writer = KisPageWriterToken{writer + 100000};
        const KisPageTransitionResult rejected = stateMachine.apply(state, stale);
        QVERIFY(!rejected.accepted);

        KisPageTransition prepare = acquire;
        prepare.kind = KisPageTransitionKind::PrepareWrite;
        state = applyAccepted(stateMachine, state, prepare);

        if ((nextRandom() & 1U) == 0U) {
            KisPageTransition cancel = acquire;
            cancel.kind = KisPageTransitionKind::CancelWrite;
            state = applyAccepted(stateMachine, state, cancel);
        } else {
            KisPageTransition publish = acquire;
            publish.kind = KisPageTransitionKind::BeginPublish;
            state = applyAccepted(stateMachine, state, publish);
            publish.kind = KisPageTransitionKind::PublishWrite;
            state = applyAccepted(stateMachine, state, publish);

            KisPageTransition commit;
            commit.kind = KisPageTransitionKind::CommitTransaction;
            commit.imageEpoch = KisImageEpochId{state.publishedEpoch.value + 1};
            commit.version = testPageVersion(generation);
            commit.transaction = KisPageTransactionId{transaction};
            state = applyAccepted(stateMachine, state, commit);
        }
    }

    QString invariantFailure;
    QVERIFY2(stateMachine.validateInvariants(state, &invariantFailure),
             qPrintable(invariantFailure));
}

void KisPageStoreFoundationTest::ssdProviderDoesNotAdvertiseDurabilityBeforeBr3()
{
    KisUnifiedSsdPageStore provider;
    const KisReplicaCapabilities capabilities = provider.capabilities();
    QVERIFY(!capabilities.domains.contains(KisPageAccessDomain::Ssd));

    KisPageVersion version;
    version.key.surface = KisSurfaceId{1};
    version.generation = KisPageGeneration{1};
    const KisReplicaOperation operation = provider.requestReplica(
        KisPageOperationId{1},
        version,
        testAllocationDescriptor(),
        KisPageAccessDomain::Ssd,
        KisPageAccessMode::Read,
        KisPagePriority::Background);
    QCOMPARE(operation.status, KisPageRequestStatus::Unsupported);
    QCOMPARE(operation.operation.value, quint64(1));
    QVERIFY(!operation.replica.isValid());
    QVERIFY(!operation.error.isEmpty());

    const KisReplicaOperation retirement = provider.retire(
        KisPageOperationId{2}, {}, {});
    QCOMPARE(retirement.status, KisPageRequestStatus::Unsupported);
    QCOMPARE(retirement.operation.value, quint64(2));
    QVERIFY(!retirement.error.isEmpty());
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
    graph.surfaceSnapshot.token = KisImageEpochSnapshotToken{1};
    graph.surfaceSnapshot.snapshot.epoch = KisImageEpochId{1};
    graph.surfaceSnapshot.snapshot.graphRevision = graph.graphRevision;
    graph.surfaceSnapshot.snapshot.defaultPixelRevision = 1;
    graph.surfaceSnapshot.snapshot.extentRevision = 1;
    graph.surfaceSnapshot.snapshot.propertyRevision = 1;
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
    graph.surfaceSnapshot.token = KisImageEpochSnapshotToken{1};
    graph.surfaceSnapshot.snapshot.epoch = KisImageEpochId{1};
    graph.surfaceSnapshot.snapshot.graphRevision = graph.graphRevision;
    graph.surfaceSnapshot.snapshot.defaultPixelRevision = 1;
    graph.surfaceSnapshot.snapshot.extentRevision = 1;
    graph.surfaceSnapshot.snapshot.propertyRevision = 1;
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

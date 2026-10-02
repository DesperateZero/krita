/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageStateMachine_p.h"

#include <QSet>

#include <algorithm>
#include <limits>

namespace {

template<class State>
bool physicalSlotInUse(const State &state,
                       const KisReplicaHandle &target,
                       const typename decltype(state.versions)::value_type *ignoredVersion = nullptr)
{
    for (const auto &version : state.versions) {
        if (&version == ignoredVersion) continue;
        for (const auto &replica : version.replicas) {
            if (replica.replica.physicalSlotIdentity() == target.physicalSlotIdentity()) return true;
        }
    }
    return false;
}

template<class Version>
bool logicalDefaultBeforeImage(const Version *base,
                              const KisPageWriterStateSnapshot &writer)
{
    // A reader may materialize this immutable default after the writer started.
    // Its new replica does not replace the writer's logical before-image claim.
    return base && base->version == writer.baseVersion &&
           base->version.isDefaultPixel() && !writer.baseAuthority.isValid() &&
           writer.mode == KisPageWriteMode::DiscardContents;
}

template<class State>
bool releaseWriterBeforeImage(State &state,
                              const KisPageWriterStateSnapshot &writer)
{
    auto *base = state.findVersion(writer.baseVersion);
    auto *authority = base ? base->findReplica(writer.baseAuthority) : nullptr;
    if (!base || (!logicalDefaultBeforeImage(base, writer) &&
                  (!(base->authority == writer.baseAuthority) || !authority ||
                   authority->pinCount == 0)))
        return false;
    if (authority) --authority->pinCount;
    return true;
}

template<class Versions>
void removeVersion(Versions *versions,
                   const KisPageVersion &identity)
{
    const auto found = std::find_if(versions->begin(), versions->end(),
        [&](const auto &candidate) { return candidate.version == identity; });
    if (found != versions->end()) versions->erase(found);
}

bool isWriterEmpty(const KisPageWriterStateSnapshot &writer)
{
    return writer.phase == KisPageWriterPhase::None && !writer.token.isValid() &&
           !writer.operation.isValid() && !writer.transaction.isValid() &&
           !writer.baseVersion.isValid() && !writer.baseAuthority.isValid() &&
           !writer.target.isValid();
}

bool isHandoffEmpty(const KisAuthorityHandoffStateSnapshot &handoff)
{
    return !handoff.operation.isValid() && !handoff.source.isValid() &&
           !handoff.target.isValid();
}

bool matchesWriter(const KisPageWriterStateSnapshot &writer,
                   const KisPageTransition &transition)
{
    return writer.isValid() && writer.token == transition.writer &&
           writer.operation == transition.operation &&
           writer.transaction == transition.transaction &&
           writer.baseVersion == transition.baseVersion &&
           writer.baseAuthority == transition.source &&
           writer.target.version == transition.version &&
           writer.target == transition.target && writer.mode == transition.writeMode;
}

bool matchesHandoff(const KisAuthorityHandoffStateSnapshot &handoff,
                    const KisPageTransition &transition)
{
    return handoff.isValid() && handoff.operation == transition.operation &&
           handoff.source == transition.source && handoff.target == transition.target &&
           handoff.source.version == transition.version;
}

template<class Replica>
bool canPublishWriteReplica(const Replica *replica)
{
    return replica && !replica->activeOperation.isValid() &&
           (replica->validity == KisReplicaValidity::Allocated ||
            replica->validity == KisReplicaValidity::Valid);
}

template<class State>
bool hasMutableVersion(const State &state)
{
    return std::any_of(state.versions.cbegin(), state.versions.cend(), [](const auto &version) {
        return version.publication == KisPagePublicationState::Prepared ||
               version.publication == KisPagePublicationState::Unpublished;
    });
}

template<class Version>
bool isVisibleToTransaction(const Version *version,
                            KisPageTransactionId transaction)
{
    return version && (version->publication == KisPagePublicationState::Published ||
                       (version->publication == KisPagePublicationState::Prepared &&
                        version->preparedBy == transaction));
}

template<class Version, class Effects>
bool appendIdleReplicaRetirements(const Version &version, Effects *effects)
{
    for (const auto &replica : version.replicas) {
        if (replica.activeOperation.isValid() || !replica.readLeases.empty() ||
            !replica.pendingLastUses.empty() || replica.pinCount != 0)
            return false;
    }
    for (const auto &replica : version.replicas)
        effects->push_back({replica.replica, {}});
    return true;
}

template<class Values, class Value>
bool contains(const Values &values, const Value &value)
{
    return std::find(values.cbegin(), values.cend(), value) != values.cend();
}

template<class Values, class Value>
bool eraseOne(Values &values, const Value &value)
{
    const auto found = std::find(values.begin(), values.end(), value);
    if (found == values.end()) return false;
    values.erase(found);
    return true;
}

template<class Values, class Value>
qsizetype eraseAll(Values &values, const Value &value)
{
    const auto before = values.size();
    values.erase(std::remove(values.begin(), values.end(), value), values.end());
    return qsizetype(before - values.size());
}

template<class Version>
void appendReplica(Version &version, const KisReplicaHandle &handle,
                   KisReplicaValidity validity, KisPageOperationId operation = {})
{
    auto value = kisPageReplicaValue(version);
    value.replica = handle;
    value.validity = validity;
    value.activeOperation = operation;
    version.replicas.push_back(std::move(value));
}

KisPageTransitionResult transitionResult(const KisPageStateSnapshot &current)
{
    KisPageTransitionResult result;
    result.next = current;
    return result;
}
KisPageWorkingResult transitionResult(KisPageWorkingState &current)
{
    return KisPageWorkingResult(std::move(current));
}

template<class State, class Validate>
auto applyPolicy(State current, const KisPageTransition &transition,
                 bool validateBoundaryInvariants, Validate validate)
{
    auto result = transitionResult(current);
    using Version = typename decltype(current.versions)::value_type;
    using Replica = typename decltype(std::declval<Version>().replicas)::value_type;

    if (validateBoundaryInvariants) {
        QString currentFailure;
        if (!validate(current, &currentFailure)) {
            result.rejectionReason = QStringLiteral("invalid current state: %1").arg(currentFailure);
            return result;
        }
    }

    auto reject = [&result, &current, validateBoundaryInvariants](const QString &reason) {
        result.accepted = false;
        // The public oracle returns the original snapshot on rejection. The
        // private consumed work has no consumer until the whole sequence accepts.
        if (validateBoundaryInvariants) result.next = current;
        result.effects.clear();
        result.rejectionReason = reason;
        return std::move(result);
    };

    auto requireVersion = [&result](const KisPageVersion &version)
        -> Version * {
        return result.next.findVersion(version);
    };

    switch (transition.kind) {
    case KisPageTransitionKind::RetainCapturedVersion:
    case KisPageTransitionKind::ReleaseCapturedVersion: {
        auto *version = requireVersion(transition.version);
        if (!version || !transition.readView.isValid() ||
            version->publication == KisPagePublicationState::Unpublished ||
            version->publication == KisPagePublicationState::Retiring) {
            return reject(QStringLiteral("captured version identity is not readable"));
        }
        if (transition.kind == KisPageTransitionKind::RetainCapturedVersion) {
            if (version->publication != KisPagePublicationState::Prepared ||
                !(version->preparedBy == transition.transaction) ||
                contains(version->capturedReadViews, transition.readView)) {
                return reject(QStringLiteral("captured private version is foreign or already retained"));
            }
            version->capturedReadViews.push_back(transition.readView);
        } else if (!eraseOne(version->capturedReadViews, transition.readView)) {
            return reject(QStringLiteral("captured version token is stale or foreign"));
        }
        break;
    }
    case KisPageTransitionKind::AcquireRead: {
        if (!transition.lease.isValid()) {
            return reject(QStringLiteral("read lease identity is invalid"));
        }
        Version *version = requireVersion(transition.version);
        if (!version || version->publication == KisPagePublicationState::Unpublished ||
            version->publication == KisPagePublicationState::Retiring) {
            return reject(QStringLiteral("requested page version is not readable"));
        }
        if (version->publication == KisPagePublicationState::Prepared &&
            !(version->preparedBy == transition.transaction)) {
            return reject(QStringLiteral("prepared page is private to another transaction"));
        }
        Replica *replica = version->findReplica(transition.target);
        if (!replica || replica->validity != KisReplicaValidity::Valid) {
            return reject(QStringLiteral("requested replica is not valid"));
        }
        for (const Version &candidateVersion : result.next.versions) {
            for (const Replica &candidateReplica : candidateVersion.replicas) {
                if (contains(candidateReplica.readLeases, transition.lease)) {
                    return reject(QStringLiteral("read lease identity is already active"));
                }
            }
        }
        replica->readLeases.push_back(transition.lease);
        break;
    }
    case KisPageTransitionKind::ReleaseRead: {
        if (!transition.lease.isValid()) {
            return reject(QStringLiteral("read lease identity is invalid"));
        }
        Version *version = requireVersion(transition.version);
        Replica *replica = version
            ? version->findReplica(transition.target) : nullptr;
        if (!replica || !eraseOne(replica->readLeases, transition.lease)) {
            return reject(QStringLiteral("read lease is stale or belongs to another replica"));
        }
        if (transition.completion.isValid()) {
            replica->pendingLastUses.push_back(transition.completion);
        }
        break;
    }
    case KisPageTransitionKind::AcknowledgeLastUse: {
        if (!transition.completion.isValid()) {
            return reject(QStringLiteral("last-use completion is invalid"));
        }
        Version *version = requireVersion(transition.version);
        Replica *replica = version
            ? version->findReplica(transition.target) : nullptr;
        if (!replica || eraseAll(replica->pendingLastUses, transition.completion) == 0) {
            return reject(QStringLiteral("last-use completion is stale or belongs to another replica"));
        }
        break;
    }
    case KisPageTransitionKind::AcquireWrite:
    case KisPageTransitionKind::AcquireRecoverableWrite: {
        const bool recoverable = transition.kind == KisPageTransitionKind::AcquireRecoverableWrite;
        if (!isWriterEmpty(result.next.writer)) {
            return reject(QStringLiteral("page already has a reserved writer"));
        }
        if (!transition.writer.isValid() || !transition.operation.isValid() ||
            !transition.transaction.isValid() || !transition.baseVersion.isValid() ||
            !transition.version.isValid() ||
            !transition.target.isValid()) {
            return reject(QStringLiteral("write reservation identity is incomplete"));
        }
        if (!(transition.baseVersion.key == result.next.key) ||
            !(transition.version.key == result.next.key) ||
            !(transition.target.version == transition.version)) {
            return reject(QStringLiteral("write reservation targets another page"));
        }
        Version *base =
            result.next.findVersion(transition.baseVersion);
        if (!isVisibleToTransaction(base, transition.transaction)) {
            return reject(QStringLiteral("write base is not visible to the transaction"));
        }
        Replica *baseAuthority = base->findReplica(transition.source);
        const bool virtualBase = base->isVirtualDefault() &&
            !transition.source.isValid() &&
            transition.writeMode == KisPageWriteMode::DiscardContents;
        if ((!recoverable && !(base->authority == transition.source)) ||
            (!virtualBase && (!baseAuthority || baseAuthority->validity != KisReplicaValidity::Valid))) {
            return reject(QStringLiteral("write base authority is not a valid exact-generation replica"));
        }
        if (transition.version.generation.value != result.next.nextGeneration.value ||
            transition.version.generation.value <= transition.baseVersion.generation.value ||
            result.next.findVersion(transition.version)) {
            return reject(QStringLiteral("write generation is not the next free generation"));
        }
        if (result.next.nextGeneration.value == std::numeric_limits<quint64>::max()) {
            return reject(QStringLiteral("page generation space is exhausted"));
        }
        if (baseAuthority && baseAuthority->pinCount == std::numeric_limits<quint32>::max()) {
            return reject(QStringLiteral("write before-image pin count is exhausted"));
        }
        if (recoverable) {
            const auto *old = base->findReplica(base->authority);
            if (!old || !baseAuthority || old == baseAuthority ||
                !isHandoffEmpty(result.next.authorityHandoff) ||
                old->validity != KisReplicaValidity::Valid || old->activeOperation.isValid() ||
                old->pinCount || !old->readLeases.empty() || !old->pendingLastUses.empty() ||
                baseAuthority->activeOperation.isValid() ||
                !(old->replica.layout == baseAuthority->replica.layout) ||
                !(old->replica.layout == transition.target.layout) ||
                old->replica.domain != transition.target.domain ||
                !(old->replica.physicalSlotIdentity() == transition.target.physicalSlotIdentity()) ||
                old->replica.physicalSlotIdentity() == transition.source.physicalSlotIdentity() ||
                old->replica.allocation.generation == std::numeric_limits<quint64>::max() ||
                transition.target.allocation.generation != old->replica.allocation.generation + 1) {
                return reject(QStringLiteral("recoverable write has no idle exact source and independent before"));
            }
            // This changes only logical ownership, not bytes or a provider
            // tag. A production prepared install is paired with an already
            // acquired physical claim; the reference model proves no such
            // physical permission. Ordinary AcquireWrite keeps its collision
            // guard unchanged.
            const auto oldHandle = old->replica;
            base->replicas.erase(std::find_if(base->replicas.begin(), base->replicas.end(),
                [&](const auto &candidate) { return candidate.replica == oldHandle; }));
            base->authority = transition.source;
            baseAuthority = base->findReplica(transition.source);
        }
        if (physicalSlotInUse(result.next, transition.target))
            return reject(QStringLiteral("write allocation token is already in use"));

        auto writeVersion = kisPageVersionValue(result.next);
        writeVersion.version = transition.version;
        writeVersion.publication = KisPagePublicationState::Unpublished;
        appendReplica(writeVersion, transition.target, KisReplicaValidity::Allocated, {});
        // A virtual default has no bytes to pin. The writer reservation keeps
        // its exact logical before-version alive; only the target is physical.
        if (baseAuthority) baseAuthority->pinCount++;
        result.next.versions.push_back(std::move(writeVersion));
        result.next.writer.token = transition.writer;
        result.next.writer.operation = transition.operation;
        result.next.writer.transaction = transition.transaction;
        result.next.writer.baseVersion = transition.baseVersion;
        result.next.writer.baseAuthority = transition.source;
        result.next.writer.target = transition.target;
        result.next.writer.mode = transition.writeMode;
        result.next.writer.phase = KisPageWriterPhase::Reserved;
        result.next.nextGeneration.value++;
        break;
    }
    case KisPageTransitionKind::AdoptPreparedWrite: {
        if (!isWriterEmpty(result.next.writer)) {
            return reject(QStringLiteral("page already has a reserved writer"));
        }
        if (!transition.writer.isValid() || !transition.operation.isValid() ||
            !transition.transaction.isValid() ||
            !transition.baseVersion.isValid() ||
            !transition.version.isValid() ||
            !transition.target.isValid() ||
            transition.writeMode != KisPageWriteMode::DiscardContents) {
            return reject(QStringLiteral(
                "prepared write adoption identity is incomplete"));
        }
        if (!(transition.baseVersion.key == result.next.key) ||
            !(transition.version.key == result.next.key) ||
            !(transition.target.version == transition.version)) {
            return reject(QStringLiteral(
                "prepared write adoption targets another page"));
        }
        const Version *base =
            result.next.findVersion(transition.baseVersion);
        if (!isVisibleToTransaction(base, transition.transaction)) {
            return reject(QStringLiteral(
                "prepared write adoption base is not transaction-visible"));
        }
        const Replica *baseAuthority =
            base->findReplica(transition.source);
        if (!(base->authority == transition.source) ||
            (!base->isVirtualDefault() &&
             (!baseAuthority || baseAuthority->validity != KisReplicaValidity::Valid))) {
            return reject(QStringLiteral(
                "prepared write adoption base authority is invalid"));
        }
        if (transition.version.generation.value !=
                result.next.nextGeneration.value ||
            transition.version.generation.value <=
                transition.baseVersion.generation.value ||
            result.next.findVersion(transition.version) ||
            result.next.nextGeneration.value ==
                std::numeric_limits<quint64>::max()) {
            return reject(QStringLiteral(
                "prepared write adoption generation is not the next free generation"));
        }
        if (physicalSlotInUse(result.next, transition.target))
            return reject(QStringLiteral(
                "prepared write adoption token is already in use"));

        auto writeVersion = kisPageVersionValue(result.next);
        writeVersion.version = transition.version;
        writeVersion.publication = KisPagePublicationState::Prepared;
        appendReplica(writeVersion, transition.target, KisReplicaValidity::Valid, {});
        writeVersion.authority = transition.target;
        writeVersion.preparedBy = transition.transaction;
        result.next.versions.push_back(std::move(writeVersion));
        result.next.nextGeneration.value++;
        break;
    }
    case KisPageTransitionKind::PrepareWrite: {
        KisPageWriterStateSnapshot &writer = result.next.writer;
        if (writer.phase != KisPageWriterPhase::Reserved ||
            !matchesWriter(writer, transition)) {
            return reject(QStringLiteral("write preparation does not match the reservation"));
        }
        Version *version = requireVersion(writer.target.version);
        Replica *replica = version ? version->findReplica(writer.target) : nullptr;
        if (!replica || replica->validity == KisReplicaValidity::Failed ||
            replica->validity == KisReplicaValidity::Retiring ||
            replica->activeOperation.isValid()) {
            return reject(QStringLiteral("write target is not ready for exclusive access"));
        }
        writer.phase = KisPageWriterPhase::Writable;
        break;
    }
    case KisPageTransitionKind::BeginPublish: {
        KisPageWriterStateSnapshot &writer = result.next.writer;
        if (writer.phase != KisPageWriterPhase::Writable ||
            !matchesWriter(writer, transition)) {
            return reject(QStringLiteral("publish request is stale or mismatched"));
        }
        Version *version = requireVersion(writer.target.version);
        Replica *replica = version ? version->findReplica(writer.target) : nullptr;
        if (!canPublishWriteReplica(replica)) {
            return reject(QStringLiteral("write replica cannot begin publishing"));
        }
        writer.phase = KisPageWriterPhase::Publishing;
        break;
    }
    case KisPageTransitionKind::PublishWrite: {
        KisPageWriterStateSnapshot &writer = result.next.writer;
        if ((writer.phase != KisPageWriterPhase::Publishing &&
             writer.phase != KisPageWriterPhase::CancelPending) ||
            !matchesWriter(writer, transition)) {
            return reject(QStringLiteral("publish completion is stale or mismatched"));
        }
        Version *version = requireVersion(writer.target.version);
        Replica *replica = version ? version->findReplica(writer.target) : nullptr;
        if (!canPublishWriteReplica(replica)) {
            return reject(QStringLiteral("write replica cannot complete publishing"));
        }
        if (!releaseWriterBeforeImage(result.next, writer)) {
            return reject(QStringLiteral("write before-image retention was lost"));
        }

        if (writer.phase == KisPageWriterPhase::CancelPending) {
            const KisPageVersion writeVersion = writer.target.version;
            result.effects.push_back({writer.target, {}});
            writer = {};
            removeVersion(&result.next.versions, writeVersion);
            break;
        }

        replica->validity = KisReplicaValidity::Valid;
        version->authority = writer.target;
        version->publication = KisPagePublicationState::Prepared;
        version->preparedBy = writer.transaction;
        writer = {};
        break;
    }
    case KisPageTransitionKind::FailWrite: {
        const KisPageWriterStateSnapshot writer = result.next.writer;
        if ((writer.phase != KisPageWriterPhase::Publishing &&
             writer.phase != KisPageWriterPhase::CancelPending) ||
            !matchesWriter(writer, transition)) {
            return reject(QStringLiteral("write failure completion is stale or mismatched"));
        }
        Version *version = requireVersion(writer.target.version);
        Replica *target = version
            ? version->findReplica(writer.target) : nullptr;
        if (!version || !target || target->activeOperation.isValid() ||
            !target->readLeases.empty() ||
            !target->pendingLastUses.empty() || target->pinCount != 0 ||
            !releaseWriterBeforeImage(result.next, writer)) {
            return reject(QStringLiteral("failed write still has in-flight state"));
        }
        result.effects.push_back({writer.target, {}});
        result.next.writer = {};
        removeVersion(&result.next.versions, writer.target.version);
        break;
    }
    case KisPageTransitionKind::CancelWrite: {
        const KisPageWriterStateSnapshot writer = result.next.writer;
        if (!matchesWriter(writer, transition)) {
            return reject(QStringLiteral("write cancellation is stale or mismatched"));
        }
        Version *version = requireVersion(writer.target.version);
        if (!version) {
            return reject(QStringLiteral("reserved write generation is missing"));
        }
        if (writer.phase == KisPageWriterPhase::Publishing) {
            result.next.writer.phase = KisPageWriterPhase::CancelPending;
            break;
        }
        if (writer.phase == KisPageWriterPhase::CancelPending) {
            return reject(QStringLiteral("write cancellation is already pending"));
        }
        if (!appendIdleReplicaRetirements(*version, &result.effects))
            return reject(QStringLiteral("write generation still has in-flight users"));
        if (!releaseWriterBeforeImage(result.next, writer)) {
            return reject(QStringLiteral("write before-image retention was lost"));
        }
        removeVersion(&result.next.versions, writer.target.version);
        result.next.writer = {};
        break;
    }
    case KisPageTransitionKind::BeginMaterialize: {
        if (!transition.operation.isValid() || !transition.source.isValid() ||
            !transition.target.isValid() || !(transition.source.version == transition.version) ||
            !(transition.target.version == transition.version) ||
            transition.source == transition.target) {
            return reject(QStringLiteral("materialization identity is incomplete"));
        }
        Version *version = requireVersion(transition.version);
        Replica *source = version
            ? version->findReplica(transition.source) : nullptr;
        if (!source || source->validity != KisReplicaValidity::Valid ||
            version->findReplica(transition.target)) {
            return reject(QStringLiteral("materialization source or target is invalid"));
        }
        if (source->pinCount == std::numeric_limits<quint32>::max()) {
            return reject(QStringLiteral("materialization source pin count is exhausted"));
        }
        if (physicalSlotInUse(result.next, transition.target))
            return reject(QStringLiteral("materialization allocation token is already in use"));
        source->pinCount++;
        appendReplica(*version, transition.target, KisReplicaValidity::Materializing, transition.operation);
        break;
    }
    case KisPageTransitionKind::CompleteMaterialize:
    case KisPageTransitionKind::FailMaterialize: {
        Version *version = requireVersion(transition.version);
        Replica *source = version
            ? version->findReplica(transition.source) : nullptr;
        Replica *target = version
            ? version->findReplica(transition.target) : nullptr;
        if (!transition.operation.isValid() || !source || !target ||
            source->pinCount == 0 ||
            target->validity != KisReplicaValidity::Materializing ||
            !(target->activeOperation == transition.operation)) {
            return reject(QStringLiteral("materialization completion is stale or mismatched"));
        }
        source->pinCount--;
        target->activeOperation = {};
        target->validity = transition.kind == KisPageTransitionKind::CompleteMaterialize
            ? KisReplicaValidity::Valid : KisReplicaValidity::Failed;
        break;
    }
    case KisPageTransitionKind::BeginAuthorityHandoff: {
        if (!isHandoffEmpty(result.next.authorityHandoff) ||
            !transition.operation.isValid()) {
            return reject(QStringLiteral("authority handoff is already active or invalid"));
        }
        Version *version = requireVersion(transition.version);
        Replica *source = version
            ? version->findReplica(transition.source) : nullptr;
        Replica *target = version
            ? version->findReplica(transition.target) : nullptr;
        if (!version || !source || !target ||
            !(version->authority == transition.source) ||
            source->validity != KisReplicaValidity::Valid ||
            target->validity != KisReplicaValidity::Valid ||
            transition.source == transition.target) {
            return reject(QStringLiteral("authority handoff endpoints are invalid"));
        }
        if (source->pinCount == std::numeric_limits<quint32>::max() ||
            target->pinCount == std::numeric_limits<quint32>::max()) {
            return reject(QStringLiteral("authority handoff pin count is exhausted"));
        }
        source->pinCount++;
        target->pinCount++;
        result.next.authorityHandoff = {transition.operation,
                                        transition.source,
                                        transition.target};
        break;
    }
    case KisPageTransitionKind::CommitAuthorityHandoff:
    case KisPageTransitionKind::FailAuthorityHandoff: {
        const bool commit = transition.kind == KisPageTransitionKind::CommitAuthorityHandoff;
        const KisAuthorityHandoffStateSnapshot handoff = result.next.authorityHandoff;
        if (!matchesHandoff(handoff, transition)) {
            return reject(commit
                ? QStringLiteral("authority handoff completion is stale or mismatched")
                : QStringLiteral("authority handoff failure is stale or mismatched"));
        }
        Version *version = requireVersion(transition.version);
        Replica *source = version
            ? version->findReplica(handoff.source) : nullptr;
        Replica *target = version
            ? version->findReplica(handoff.target) : nullptr;
        if (!version || !source || !target || source->pinCount == 0 ||
            target->pinCount == 0 || (commit
                ? target->validity != KisReplicaValidity::Valid
                : !(version->authority == handoff.source))) {
            return reject(commit
                ? QStringLiteral("authority handoff lost a protected endpoint")
                : QStringLiteral("failed authority handoff lost a protected endpoint"));
        }
        source->pinCount--;
        target->pinCount--;
        if (commit) version->authority = handoff.target;
        result.next.authorityHandoff = {};
        break;
    }
    case KisPageTransitionKind::BeginRetire: {
        if (!transition.operation.isValid() || !transition.target.isValid()) {
            return reject(QStringLiteral("retirement identity is incomplete"));
        }
        Version *version = requireVersion(transition.version);
        Replica *target = version
            ? version->findReplica(transition.target) : nullptr;
        if (!version || !target || version->authority == transition.target ||
            (result.next.writer.isValid() &&
             result.next.writer.target == transition.target) ||
            (target->validity != KisReplicaValidity::Valid &&
             target->validity != KisReplicaValidity::Failed) ||
            !target->readLeases.empty() || target->pinCount != 0 ||
            !target->pendingLastUses.empty() || target->activeOperation.isValid()) {
            return reject(QStringLiteral("replica is not eligible for retirement"));
        }
        target->validity = KisReplicaValidity::Retiring;
        target->activeOperation = transition.operation;
        break;
    }
    case KisPageTransitionKind::CompleteRetire: {
        Version *version = requireVersion(transition.version);
        if (!version) return reject(QStringLiteral("retired page version is missing"));
        bool removed = false;
        for (auto it = version->replicas.begin(); it != version->replicas.end(); ++it) {
            const Replica &target = *it;
            if (target.replica == transition.target &&
                target.validity == KisReplicaValidity::Retiring &&
                target.activeOperation == transition.operation &&
                target.readLeases.empty() &&
                target.pendingLastUses.empty() && target.pinCount == 0) {
                version->replicas.erase(it);
                removed = true;
                break;
            }
        }
        if (!removed) {
            return reject(QStringLiteral("retirement completion is stale or mismatched"));
        }
        break;
    }
    case KisPageTransitionKind::FailRetire: {
        Version *version = requireVersion(transition.version);
        Replica *target = version
            ? version->findReplica(transition.target) : nullptr;
        if (!transition.operation.isValid() || !target ||
            target->validity != KisReplicaValidity::Retiring ||
            !(target->activeOperation == transition.operation) ||
            !target->readLeases.empty() ||
            !target->pendingLastUses.empty() || target->pinCount != 0) {
            return reject(QStringLiteral("retirement failure is stale or mismatched"));
        }
        target->validity = KisReplicaValidity::Failed;
        target->activeOperation = {};
        break;
    }
    case KisPageTransitionKind::CommitTransaction: {
        if (!transition.transaction.isValid() || !transition.imageEpoch.isValid() ||
            transition.imageEpoch.value <= result.next.publishedEpoch.value) {
            return reject(QStringLiteral("transaction identity is invalid"));
        }
        Version *prepared = requireVersion(transition.version);
        if (!prepared || prepared->publication != KisPagePublicationState::Prepared ||
            !(prepared->preparedBy == transition.transaction)) {
            return reject(QStringLiteral("transaction page is not prepared"));
        }
        for (const Version &candidate : result.next.versions) {
            if (candidate.publication == KisPagePublicationState::Prepared &&
                candidate.preparedBy == transition.transaction &&
                candidate.version.generation.value >
                    prepared->version.generation.value) {
                return reject(QStringLiteral("transaction commit does not select its latest prepared generation"));
            }
        }
        Version *published = result.next.publishedVersion();
        if (!published || prepared->version.generation.value <=
                              published->version.generation.value) {
            return reject(QStringLiteral("transaction would not advance the published generation"));
        }
        published->publication = KisPagePublicationState::Historical;
        for (Version &candidate : result.next.versions) {
            if (candidate.publication == KisPagePublicationState::Prepared &&
                candidate.preparedBy == transition.transaction &&
                !(candidate.version == prepared->version)) {
                candidate.publication = KisPagePublicationState::Historical;
                candidate.preparedBy = {};
            }
        }
        prepared->publication = KisPagePublicationState::Published;
        prepared->preparedBy = {};
        result.next.publishedEpoch = transition.imageEpoch;
        result.next.publishedGeneration = prepared->version.generation;
        result.next.publishedDefaultPixelRevision =
            prepared->version.defaultPixelRevision;

        break;
    }
    case KisPageTransitionKind::AttachHistoricalDefault: {
        if (!transition.version.isValid() ||
            !transition.version.isDefaultPixel() ||
            (transition.target.isValid() &&
             !(transition.target.version == transition.version)) ||
            transition.imageEpoch.isValid() ||
            result.next.findVersion(transition.version)) {
            return reject(QStringLiteral(
                "historical default attachment identity is invalid"));
        }
        // An independently identified immutable default may be needed by a
        // captured removal/default view while another version is being written
        // or is Prepared. This appends history only: it must neither change
        // the writer/head nor alias any existing (including mutable) allocation.
        if (physicalSlotInUse(result.next, transition.target))
            return reject(QStringLiteral(
                "historical default allocation token is already in use"));
        auto historical = kisPageVersionValue(result.next);
        historical.version = transition.version;
        historical.publication = KisPagePublicationState::Historical;
        if (transition.target.isValid()) {
            appendReplica(historical, transition.target, KisReplicaValidity::Valid, {});
        }
        historical.authority = transition.target;
        result.next.versions.push_back(std::move(historical));
        break;
    }
    case KisPageTransitionKind::MaterializeDefault: {
        Version *version = requireVersion(transition.version);
        if (!version || !version->isVirtualDefault() ||
            !transition.target.isValid() ||
            !(transition.target.version == transition.version) ||
            transition.imageEpoch.isValid()) {
            return reject(QStringLiteral("virtual default materialization is invalid"));
        }
        if (physicalSlotInUse(result.next, transition.target))
            return reject(QStringLiteral("default allocation token is already in use"));
        appendReplica(*version, transition.target, KisReplicaValidity::Valid, {});
        version->authority = transition.target;
        break;
    }
    case KisPageTransitionKind::ReplaceDefaultPixel: {
        Version *existingReplacement =
            result.next.findVersion(transition.version);
        if (!transition.imageEpoch.isValid() ||
            transition.imageEpoch.value <= result.next.publishedEpoch.value ||
            !transition.version.isValid() ||
            !transition.version.isDefaultPixel() ||
            (transition.target.isValid() &&
             !(transition.target.version == transition.version)) ||
            result.next.writer.isValid() ||
            (existingReplacement &&
             (existingReplacement->publication !=
                  KisPagePublicationState::Historical ||
              !(existingReplacement->authority == transition.target)))) {
            return reject(QStringLiteral("default replacement identity is invalid"));
        }
        if (hasMutableVersion(result.next))
            return reject(QStringLiteral("default replacement conflicts with mutable page state"));
        if (physicalSlotInUse(result.next, transition.target, existingReplacement))
            return reject(QStringLiteral(
                "default replacement allocation token is already in use"));
        Version *published = result.next.publishedVersion();
        if (!published || !published->version.isDefaultPixel()) {
            return reject(QStringLiteral(
                "default replacement source is not an implicit default"));
        }
        published->publication = KisPagePublicationState::Historical;
        if (existingReplacement) {
            existingReplacement->publication = KisPagePublicationState::Published;
        } else {
            auto replacement = kisPageVersionValue(result.next);
            replacement.version = transition.version;
            replacement.publication = KisPagePublicationState::Published;
            if (transition.target.isValid()) {
                appendReplica(replacement, transition.target, KisReplicaValidity::Valid, {});
            }
            replacement.authority = transition.target;
            result.next.versions.push_back(std::move(replacement));
        }
        result.next.publishedEpoch = transition.imageEpoch;
        result.next.publishedGeneration = transition.version.generation;
        result.next.publishedDefaultPixelRevision =
            transition.version.defaultPixelRevision;
        break;
    }
    case KisPageTransitionKind::RestoreCommittedVersion: {
        if (!transition.imageEpoch.isValid() ||
            transition.imageEpoch.value <= result.next.publishedEpoch.value ||
            transition.transaction.isValid() || result.next.writer.isValid()) {
            return reject(QStringLiteral("historical restore identity is invalid"));
        }
        if (hasMutableVersion(result.next))
            return reject(QStringLiteral("historical restore conflicts with mutable page state"));
        // An absent page is recoverable from its exact default revision, not
        // from a newly allocated/fill-produced pixel replica. Keep the page's
        // nextGeneration high-water mark intact when returning to absence.
        if (transition.version.isValid() && transition.version.isDefaultPixel() &&
            transition.version.key == result.next.key &&
            !result.next.findVersion(transition.version)) {
            auto implicit = kisPageVersionValue(result.next);
            implicit.version = transition.version;
            implicit.publication = KisPagePublicationState::Historical;
            result.next.versions.push_back(std::move(implicit));
        }
        Version *target = requireVersion(transition.version);
        if (!target ||
            (target->publication != KisPagePublicationState::Historical &&
             target->publication != KisPagePublicationState::Published) ||
            (!target->authority.isValid() && !target->isVirtualDefault())) {
            return reject(QStringLiteral("historical restore target is unavailable"));
        }
        Version *published = result.next.publishedVersion();
        if (!published) {
            return reject(QStringLiteral("historical restore has no published source"));
        }
        if (!(published->version == target->version)) {
            published->publication = KisPagePublicationState::Historical;
            target->publication = KisPagePublicationState::Published;
        }
        result.next.publishedEpoch = transition.imageEpoch;
        result.next.publishedGeneration = target->version.generation;
        result.next.publishedDefaultPixelRevision =
            target->version.defaultPixelRevision;
        break;
    }
    case KisPageTransitionKind::DiscardHistoricalVersions: {
        if (transition.versions.empty() || transition.transaction.isValid() ||
            transition.operation.isValid()) {
            return reject(QStringLiteral(
                "historical discard identity is invalid"));
        }
        for (auto it = transition.versions.cbegin(); it != transition.versions.cend(); ++it) {
            const auto &version = *it;
            if (!version.isValid() || !(version.key == result.next.key) ||
                std::find(transition.versions.cbegin(), it, version) != it) {
                return reject(QStringLiteral(
                    "historical discard set is invalid"));
            }
            const Version *candidate =
                result.next.findVersion(version);
            if (!candidate ||
                candidate->publication != KisPagePublicationState::Historical ||
                !candidate->capturedReadViews.empty() ||
                (!candidate->authority.isValid() && !candidate->isVirtualDefault())) {
                return reject(QStringLiteral(
                    "historical discard target is unavailable"));
            }
            for (const Replica &replica : candidate->replicas) {
                if ((replica.validity != KisReplicaValidity::Valid &&
                     replica.validity != KisReplicaValidity::Failed) ||
                    !replica.readLeases.empty() || replica.pinCount != 0 ||
                    !replica.pendingLastUses.empty() ||
                    replica.activeOperation.isValid()) {
                    return reject(QStringLiteral(
                        "historical discard target is still in use"));
                }
            }
        }
        for (qsizetype i = result.next.versions.size(); i > 0; --i) {
            const Version &candidate =
                result.next.versions.at(i - 1);
            if (!contains(transition.versions, candidate.version)) continue;
            for (const Replica &replica : candidate.replicas) {
                result.effects.push_back({replica.replica, {}});
            }
            result.next.versions.erase(result.next.versions.begin() + i - 1);
        }
        break;
    }
    case KisPageTransitionKind::ReplacePrivatePreparedBacking: {
        auto *version = result.next.findVersion(transition.version);
        if (!transition.transaction.isValid() || !version ||
            version->publication != KisPagePublicationState::Prepared ||
            !(version->preparedBy == transition.transaction) || result.next.writer.isValid() ||
            !version->capturedReadViews.empty() || version->replicas.size() != 1 ||
            !(version->authority == transition.source) || !transition.target.isValid() ||
            !(transition.target.version == version->version))
            return reject(QStringLiteral("private backing replacement identity is invalid"));
        const auto &old = version->replicas.front();
        if (old.activeOperation.isValid() || !old.readLeases.empty() || old.pinCount ||
            !old.pendingLastUses.empty() || old.validity != KisReplicaValidity::Valid)
            return reject(QStringLiteral("private backing replacement still has consumers"));
        if (physicalSlotInUse(result.next, transition.target))
            return reject(QStringLiteral("private replacement allocation is already used"));
        result.effects.push_back({old.replica, {}});
        version->replicas.clear();
        appendReplica(*version, transition.target, KisReplicaValidity::Valid);
        version->authority = transition.target;
        break;
    }
    case KisPageTransitionKind::DetachPreparedVersion: {
        auto *version = result.next.findVersion(transition.version);
        if (!transition.transaction.isValid() || transition.operation.isValid() ||
            !version || version->publication != KisPagePublicationState::Prepared ||
            !(version->preparedBy == transition.transaction) || result.next.writer.isValid()) {
            return reject(QStringLiteral("prepared detachment identity is invalid or still writable"));
        }
        version->publication = KisPagePublicationState::Historical;
        version->preparedBy = {};
        break;
    }
    case KisPageTransitionKind::AbortPreparedVersion:
    case KisPageTransitionKind::AbortTransaction: {
        const bool exact = transition.kind == KisPageTransitionKind::AbortPreparedVersion;
        if (!transition.transaction.isValid() ||
            (exact && (!transition.version.isValid() ||
                       !(transition.version.key == result.next.key)))) {
            return reject(QStringLiteral("transaction identity is invalid"));
        }
        if (result.next.writer.isValid() &&
            result.next.writer.transaction == transition.transaction) {
            return reject(QStringLiteral("transaction still owns a write lease"));
        }
        bool found = false;
        for (qsizetype i = result.next.versions.size(); i > 0; --i) {
            Version &version = result.next.versions[i - 1];
            if (version.publication == KisPagePublicationState::Prepared &&
                version.preparedBy == transition.transaction &&
                (!exact || version.version == transition.version)) {
                // Abort removes transaction visibility, not an issued read
                // capability. Retained sealed bytes become detached history;
                // the owner collects them after view/pin/last-use release.
                if (!version.capturedReadViews.empty()) {
                    version.publication = KisPagePublicationState::Historical;
                    version.preparedBy = {};
                    found = true;
                    continue;
                }
                if (!appendIdleReplicaRetirements(version, &result.effects))
                    return reject(QStringLiteral("prepared page still has in-flight users"));
                result.next.versions.erase(result.next.versions.begin() + i - 1);
                found = true;
            }
        }
        if (!found) {
            return reject(QStringLiteral("transaction has no prepared page in this record"));
        }
        break;
    }
    }

    if (validateBoundaryInvariants) {
        QString nextFailure;
        if (!validate(result.next, &nextFailure)) {
            return reject(QStringLiteral("transition violates invariant: %1").arg(nextFailure));
        }
    }
    for (const KisPageTransitionEffect &effect : result.effects) {
        if (!effect.isValid()) {
            return reject(QStringLiteral("transition emitted an invalid side effect"));
        }
    }

    result.accepted = true;
    result.rejectionReason.clear();
    return result;
}

} // namespace

KisPageTransitionResult KisPageStateMachine::apply(
    const KisPageStateSnapshot &current, const KisPageTransition &transition) const
{
    return applyPolicy(current, transition, true,
        [this](const KisPageStateSnapshot &state, QString *reason) { return validateInvariants(state, reason); });
}

KisPageWorkingResult KisPageStateMachine::applyKnownValid(
    KisPageWorkingState current, const KisPageTransition &transition) const
{
    return applyPolicy(std::move(current), transition, false,
        [](const KisPageWorkingState &, QString *) { return true; });
}

bool KisPageStateMachine::validateInvariants(const KisPageStateSnapshot &state,
                                             QString *failureReason) const
{
    auto fail = [failureReason](const QString &reason) {
        if (failureReason) *failureReason = reason;
        return false;
    };

    if (!state.key.isValid()) return fail(QStringLiteral("page key is invalid"));
    if (!state.publishedEpoch.isValid()) {
        return fail(QStringLiteral("published epoch cache tag is invalid"));
    }
    if (!state.publishedGeneration.isValid()) {
        return fail(QStringLiteral("published generation is invalid"));
    }
    if (!state.nextGeneration.isValid()) {
        return fail(QStringLiteral("next generation is invalid"));
    }
    if (state.versions.isEmpty()) return fail(QStringLiteral("page has no versions"));

    int publishedCount = 0;
    int unpublishedCount = 0;
    KisPageVersion unpublishedVersion;
    quint64 maximumGeneration = 0;
    QSet<KisPageVersion> versionIdentities;
    QSet<quint64> leases;
    QSet<quint64> operations;
    QSet<KisReplicaPhysicalSlotIdentity> allocations;

    for (const KisPageVersionStateSnapshot &version : state.versions) {
        if (!version.version.isValid() || !(version.version.key == state.key)) {
            return fail(QStringLiteral("version belongs to another page"));
        }
        if (versionIdentities.contains(version.version))
            return fail(QStringLiteral("page version identity is duplicated"));
        versionIdentities.insert(version.version);
        maximumGeneration = qMax(maximumGeneration, version.version.generation.value);

        if (version.publication == KisPagePublicationState::Published) {
            publishedCount++;
            if (!(version.version.generation == state.publishedGeneration) ||
                version.version.defaultPixelRevision !=
                    state.publishedDefaultPixelRevision) {
                return fail(QStringLiteral("published cache does not match the published version"));
            }
        }
        if (version.publication == KisPagePublicationState::Unpublished) {
            if (!version.capturedReadViews.isEmpty()) return fail(QStringLiteral("unpublished bytes cannot belong to a captured view"));
            unpublishedCount++;
            unpublishedVersion = version.version;
        }
        if (version.publication == KisPagePublicationState::Prepared) {
            if (!version.preparedBy.isValid()) {
                return fail(QStringLiteral("prepared version has no transaction"));
            }
            // Prepared is version-local transaction visibility, not an active
            // writer reservation. Optimistic transactions may have independent
            // sealed versions here; the epoch owner revalidates their base/root
            // conflicts at commit. Writer/slot uniqueness is checked separately.
        } else if (version.preparedBy.isValid()) {
            return fail(QStringLiteral("non-prepared version retains a transaction"));
        }
        QSet<quint64> viewTokens;
        for (const auto token : version.capturedReadViews) {
            if (!token.isValid() || viewTokens.contains(token.value) ||
                version.publication == KisPagePublicationState::Retiring)
                return fail(QStringLiteral("captured version protection is invalid or duplicated"));
            viewTokens.insert(token.value);
        }

        for (const KisReplicaStateSnapshot &replica : version.replicas) {
            if (!replica.replica.isValid() ||
                !(replica.replica.version == version.version)) {
                return fail(QStringLiteral("replica identity does not match its page version"));
            }
            const bool operationRequired =
                replica.validity == KisReplicaValidity::Materializing ||
                replica.validity == KisReplicaValidity::Retiring;
            if (operationRequired != replica.activeOperation.isValid()) {
                return fail(QStringLiteral("replica operation state is inconsistent"));
            }
            if (replica.activeOperation.isValid()) {
                if (operations.contains(replica.activeOperation.value))
                    return fail(QStringLiteral("active operation identity is duplicated"));
                operations.insert(replica.activeOperation.value);
            }
            if (!replica.readLeases.isEmpty() &&
                replica.validity != KisReplicaValidity::Valid) {
                return fail(QStringLiteral("read lease references a non-valid replica"));
            }
            if (!replica.pendingLastUses.isEmpty() &&
                replica.validity != KisReplicaValidity::Valid) {
                return fail(QStringLiteral("last-use completion references a non-valid replica"));
            }
            for (KisPageLeaseId lease : replica.readLeases) {
                if (!lease.isValid())
                    return fail(QStringLiteral("read lease identity is invalid"));
                if (leases.contains(lease.value))
                    return fail(QStringLiteral("read lease identity is duplicated"));
                leases.insert(lease.value);
            }
            for (const KisCompletionTicket &lastUse : replica.pendingLastUses) {
                if (!lastUse.isValid()) {
                    return fail(QStringLiteral("last-use completion is invalid"));
                }
            }
            const auto physical = replica.replica.physicalSlotIdentity();
            if (allocations.contains(physical))
                return fail(QStringLiteral("replica physical allocation slot is aliased"));
            allocations.insert(physical);
        }

        const bool needsAuthority =
            version.publication == KisPagePublicationState::Prepared ||
            version.publication == KisPagePublicationState::Published ||
            version.publication == KisPagePublicationState::Historical;
        if (needsAuthority && !version.isVirtualDefault()) {
            if (!version.authority.isValid()) {
                return fail(QStringLiteral("recoverable version has no authority"));
            }
            const KisReplicaStateSnapshot *authority = version.findReplica(version.authority);
            if (!authority || authority->validity != KisReplicaValidity::Valid) {
                return fail(QStringLiteral("authority is not a valid exact-generation replica"));
            }
        } else if (version.authority.isValid()) {
            return fail(QStringLiteral("unpublished or retiring version exposes authority"));
        }
    }

    if (publishedCount != 1) {
        return fail(QStringLiteral("page must have exactly one published version"));
    }
    if (state.nextGeneration.value <= maximumGeneration) {
        return fail(QStringLiteral("next generation does not advance the version chain"));
    }

    if (state.writer.phase == KisPageWriterPhase::None) {
        if (!isWriterEmpty(state.writer)) {
            return fail(QStringLiteral("inactive writer retains capabilities"));
        }
        if (unpublishedCount != 0) {
            return fail(QStringLiteral("unpublished generation has no writer"));
        }
    } else {
        if (!state.writer.isValid() || !(state.writer.target.version.key == state.key)) {
            return fail(QStringLiteral("writer reservation is invalid"));
        }
        if (unpublishedCount != 1 || !(unpublishedVersion == state.writer.target.version)) {
            return fail(QStringLiteral("writer does not uniquely own the unpublished generation"));
        }
        if (operations.contains(state.writer.operation.value))
            return fail(QStringLiteral("writer operation identity is duplicated"));
        operations.insert(state.writer.operation.value);
        const KisPageVersionStateSnapshot *base = state.findVersion(state.writer.baseVersion);
        const KisPageVersionStateSnapshot *write = state.findVersion(state.writer.target.version);
        const KisReplicaStateSnapshot *target = write
            ? write->findReplica(state.writer.target) : nullptr;
        const KisReplicaStateSnapshot *baseAuthority = base
            ? base->findReplica(state.writer.baseAuthority) : nullptr;
        if (!base || !write || !target ||
            (!logicalDefaultBeforeImage(base, state.writer) &&
             (!(base->authority == state.writer.baseAuthority) || !baseAuthority ||
              baseAuthority->validity != KisReplicaValidity::Valid || baseAuthority->pinCount == 0)) ||
            (target->validity != KisReplicaValidity::Allocated &&
             target->validity != KisReplicaValidity::Valid) ||
            target->activeOperation.isValid() ||
            write->publication != KisPagePublicationState::Unpublished ||
            write->authority.isValid() ||
            write->version.generation.value <= base->version.generation.value) {
            return fail(QStringLiteral("writer does not own one unpublished generation"));
        }
    }

    if (isHandoffEmpty(state.authorityHandoff)) {
        // No handoff is active.
    } else if (!state.authorityHandoff.isValid()) {
        return fail(QStringLiteral("authority handoff identity is incomplete"));
    } else {
        if (operations.contains(state.authorityHandoff.operation.value))
            return fail(QStringLiteral("authority handoff operation identity is duplicated"));
        const KisPageVersionStateSnapshot *version =
            state.findVersion(state.authorityHandoff.source.version);
        const KisReplicaStateSnapshot *source = version
            ? version->findReplica(state.authorityHandoff.source) : nullptr;
        const KisReplicaStateSnapshot *target = version
            ? version->findReplica(state.authorityHandoff.target) : nullptr;
        if (!version || !source || !target || source->pinCount == 0 ||
            target->pinCount == 0 ||
            !(version->authority == state.authorityHandoff.source) ||
            source->validity != KisReplicaValidity::Valid ||
            target->validity != KisReplicaValidity::Valid) {
            return fail(QStringLiteral("authority handoff endpoints are not protected"));
        }
    }

    if (failureReason) failureReason->clear();
    return true;
}

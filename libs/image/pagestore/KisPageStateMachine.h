/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_STATE_MACHINE_H
#define KIS_PAGE_STATE_MACHINE_H

#include <QString>
#include <QVector>

#include "KisPageStoreTypes.h"

enum class KisPagePublicationState : quint8 {
    Unpublished,
    Prepared,
    Published,
    Historical,
    Retiring
};

enum class KisReplicaValidity : quint8 {
    Allocated,
    Materializing,
    Valid,
    Failed,
    Retiring
};

struct KRITAIMAGE_EXPORT KisReplicaStateSnapshot
{
    KisReplicaHandle replica;
    KisReplicaValidity validity = KisReplicaValidity::Allocated;
    KisPageOperationId activeOperation;
    QVector<KisPageLeaseId> readLeases;
    quint32 pinCount = 0;
    QVector<KisCompletionTicket> pendingLastUses;
};

struct KRITAIMAGE_EXPORT KisPageVersionStateSnapshot
{
    KisPageVersion version;
    KisPagePublicationState publication = KisPagePublicationState::Unpublished;
    QVector<KisReplicaStateSnapshot> replicas;
    KisReplicaHandle authority;
    // Per-version owner. Different transactions may retain independent sealed
    // versions of this page; only the active writer is page-exclusive.
    KisPageTransactionId preparedBy;
    // Logical captured-view protection, independent of replica pins and
    // execution last-use. A sealed private version may outlive its transaction.
    QVector<KisImageEpochSnapshotToken> capturedReadViews;

    /** Exact bytes are defined by the retained surface default revision. */
    bool isVirtualDefault() const
    {
        return version.isDefaultPixel() && replicas.isEmpty() &&
               !authority.isValid() &&
               (publication == KisPagePublicationState::Published ||
                publication == KisPagePublicationState::Historical);
    }

    KisReplicaStateSnapshot *findReplica(const KisReplicaHandle &replica)
    {
        for (KisReplicaStateSnapshot &candidate : replicas) {
            if (candidate.replica == replica) return &candidate;
        }
        return nullptr;
    }

    const KisReplicaStateSnapshot *findReplica(const KisReplicaHandle &replica) const
    {
        return const_cast<KisPageVersionStateSnapshot *>(this)->findReplica(replica);
    }
};

enum class KisPageWriterPhase : quint8 {
    None,
    Reserved,
    Writable,
    Publishing,
    CancelPending
};

struct KRITAIMAGE_EXPORT KisPageWriterStateSnapshot
{
    KisPageWriterToken token;
    KisPageOperationId operation;
    KisPageTransactionId transaction;
    KisPageVersion baseVersion;
    KisReplicaHandle baseAuthority;
    KisReplicaHandle target;
    KisPageWriteMode mode = KisPageWriteMode::PreserveContents;
    KisPageWriterPhase phase = KisPageWriterPhase::None;

    bool isValid() const
    {
        return token.isValid() && operation.isValid() && transaction.isValid() &&
               baseVersion.isValid() && target.isValid() &&
               baseVersion.key == target.version.key &&
               target.version.generation.value > baseVersion.generation.value &&
               ((baseAuthority.isValid() && baseAuthority.version == baseVersion) ||
                (!baseAuthority.isValid() && baseVersion.isDefaultPixel() &&
                 mode == KisPageWriteMode::DiscardContents)) &&
               phase != KisPageWriterPhase::None;
    }
};

struct KRITAIMAGE_EXPORT KisAuthorityHandoffStateSnapshot
{
    KisPageOperationId operation;
    KisReplicaHandle source;
    KisReplicaHandle target;

    bool isValid() const
    {
        return operation.isValid() && source.isValid() && target.isValid() &&
               source.version == target.version && !(source == target);
    }
};

/**
 * Deterministic state for one logical PageKey. The committed ImageEpoch root
 * remains the visibility linearization point for a multi-page transaction;
 * publishedGeneration is a generation-qualified cache of that committed view.
 */
struct KRITAIMAGE_EXPORT KisPageStateSnapshot
{
    KisPageKey key;
    KisImageEpochId publishedEpoch;
    KisPageGeneration publishedGeneration;
    quint64 publishedDefaultPixelRevision = 0;
    KisPageGeneration nextGeneration;
    QVector<KisPageVersionStateSnapshot> versions;
    KisPageWriterStateSnapshot writer;
    KisAuthorityHandoffStateSnapshot authorityHandoff;

    KisPageVersionStateSnapshot *findVersion(const KisPageVersion &version)
    {
        for (KisPageVersionStateSnapshot &candidate : versions) {
            if (candidate.version == version) return &candidate;
        }
        return nullptr;
    }

    const KisPageVersionStateSnapshot *findVersion(const KisPageVersion &version) const
    {
        return const_cast<KisPageStateSnapshot *>(this)->findVersion(version);
    }

    KisPageVersionStateSnapshot *publishedVersion()
    {
        for (KisPageVersionStateSnapshot &candidate : versions) {
            if (candidate.publication == KisPagePublicationState::Published)
                return &candidate;
        }
        return nullptr;
    }

    const KisPageVersionStateSnapshot *publishedVersion() const
    {
        return const_cast<KisPageStateSnapshot *>(this)->publishedVersion();
    }
};

enum class KisPageTransitionKind : quint8 {
    AcquireRead,
    ReleaseRead,
    AcknowledgeLastUse,
    AcquireWrite,
    AdoptPreparedWrite,
    PrepareWrite,
    BeginPublish,
    PublishWrite,
    FailWrite,
    CancelWrite,
    BeginMaterialize,
    CompleteMaterialize,
    FailMaterialize,
    BeginAuthorityHandoff,
    CommitAuthorityHandoff,
    FailAuthorityHandoff,
    BeginRetire,
    CompleteRetire,
    FailRetire,
    CommitTransaction,
    AttachHistoricalDefault = 21, // 20 was the retired fused commit transition
    MaterializeDefault,
    ReplaceDefaultPixel,
    RestoreCommittedVersion,
    DiscardHistoricalVersions = 26, // keep values after retired transition 25 stable
    AbortTransaction,
    // Owner rollback of a not-yet-exposed mutation seal. Unlike aborting the
    // whole transaction, earlier Prepared generations remain intact.
    AbortPreparedVersion,
    RetainCapturedVersion,
    ReleaseCapturedVersion,
    // Drop sealed-overlay membership, not outstanding pixel/read capabilities.
    // Reclamation follows ordinary historical reachability/pin/last-use rules.
    DetachPreparedVersion,
    // Owner-only replacement of this segment's not-yet-exposed Prepared
    // target, after returning its guards. Keeps the pending generation.
    ReplacePrivatePreparedBacking,
    // Logical half of a recoverable backing transfer. source is the existing
    // independent exact before replica; target retags the old authority's
    // physical slot for a newer logical version. Production must prepare all
    // storage and hold the provider's physical claim before installing this.
    AcquireRecoverableWrite
};

struct KRITAIMAGE_EXPORT KisPageTransition
{
    KisPageTransitionKind kind = KisPageTransitionKind::AcquireRead;
    KisPageVersion baseVersion;
    KisPageVersion version;
    KisReplicaHandle source;
    KisReplicaHandle target;
    KisPageOperationId operation;
    KisPageWriterToken writer;
    KisPageLeaseId lease;
    KisPageTransactionId transaction;
    KisPageWriteMode writeMode = KisPageWriteMode::PreserveContents;
    KisCompletionTicket completion;
    KisImageEpochId imageEpoch;
    QVector<KisPageVersion> versions;
    KisImageEpochSnapshotToken readView;
};

struct KRITAIMAGE_EXPORT KisPageTransitionEffect
{
    KisReplicaHandle replica;
    KisCompletionTicket lastUse;

    bool isValid() const { return replica.isValid(); }
};

struct KRITAIMAGE_EXPORT KisPageTransitionResult
{
    bool accepted = false;
    KisPageStateSnapshot next;
    QVector<KisPageTransitionEffect> effects;
    QString rejectionReason;
};

/**
 * Shared deterministic transition function for reference and production
 * providers. BR1 must implement it before PageStore can become operational.
 */
struct KisPageWorkingState;
struct KisPageWorkingResult;
class KisPageMetadataCoordinator;

class KRITAIMAGE_EXPORT KisPageStateMachine
{
public:
    KisPageTransitionResult apply(const KisPageStateSnapshot &current,
                                  const KisPageTransition &transition) const;
    bool validateInvariants(const KisPageStateSnapshot &state,
                            QString *failureReason = nullptr) const;

private:
    // Only indexed coordinator-owned values may omit the full boundary scans.
    // Both entry points use the same transition-local policy. The working value
    // is consumed; on rejection the coordinator discards it without installation.
    KisPageWorkingResult applyKnownValid(
        KisPageWorkingState current,
        const KisPageTransition &transition) const;
    friend class KisPageMetadataCoordinator;
};

#endif // KIS_PAGE_STATE_MACHINE_H

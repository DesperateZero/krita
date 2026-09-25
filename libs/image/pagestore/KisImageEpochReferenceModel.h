/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_IMAGE_EPOCH_REFERENCE_MODEL_H
#define KIS_IMAGE_EPOCH_REFERENCE_MODEL_H

#include <QScopedPointer>
#include <QSet>
#include <QSharedPointer>
#include <QString>
#include <QVector>
#include "KisPageStoreTypes.h"

class KisImageEpochPageRoot;

enum class KisPageTransactionState : quint8 {
    Invalid,
    Open,
    Prepared,
    Committed,
    Aborted
};

/**
 * Immutable document visibility root backed by a structurally shared,
 * balanced persistent page tree. manifest() is an explicit cold-path export
 * for serialization/reference tests; production lookup and commit use the
 * root directly.
 */
class KRITAIMAGE_EXPORT KisImageEpochRootSnapshot
{
public:
    bool isValid() const;
    KisImageEpochId epoch() const { return m_epoch; }
    KisImageEpochId previousEpoch() const { return m_previousEpoch; }
    quint64 commitSequence() const { return m_commitSequence; }
    quint64 graphRevision() const { return m_graphRevision; }
    quint64 defaultPixelRevision() const { return m_defaultPixelRevision; }
    quint64 extentRevision() const { return m_extentRevision; }
    quint64 propertyRevision() const { return m_propertyRevision; }
    QVector<KisPageVersion> manifest() const;
    qsizetype pageCount() const;
    qint32 pageTreeHeight() const;
    bool containsPage(const KisPageKey &key, KisPageVersion *version = nullptr) const;
    QVector<KisSurfaceEpochState> surfaces() const { return m_surfaces; }
    KisImageEpochSnapshot snapshot() const;
    bool resolve(const KisPageKey &key, KisPageVersion *version) const;
    bool surfaceState(KisSurfaceId surface, KisSurfaceEpochState *state) const;

    // Read-only derived index query. Applies only this delta to a temporary
    // structurally shared tree; never exports/scans the complete manifest.
    // Does not publish or grant transaction authority.
    bool contentExtentAfterDelta(KisSurfaceId surface,
                                 QSize pageExtent,
                                 const KisPreparedPageSet &delta,
                                 QRect *extent) const;

private:
    bool validate() const;
    // Issued only after complete construction-time validation by the model.
    bool m_validated = false;
    KisImageEpochId m_epoch;
    KisImageEpochId m_previousEpoch;
    quint64 m_commitSequence = 0;
    quint64 m_graphRevision = 0;
    quint64 m_defaultPixelRevision = 0;
    quint64 m_extentRevision = 0;
    quint64 m_propertyRevision = 0;
    QSharedPointer<const KisImageEpochPageRoot> m_pageRoot;
    QVector<KisSurfaceEpochState> m_surfaces;

    friend class KisImageEpochReferenceModel;
};

struct KRITAIMAGE_EXPORT KisPageTransactionSnapshot {
    KisPageTransaction transaction;
    KisPageTransactionState state = KisPageTransactionState::Invalid;
    QVector<KisPageVersion> changes;
    QVector<KisSurfaceEpochChange> surfaceChanges;
    QVector<KisPageKey> removedPages;

    bool isActive() const
    {
        return state == KisPageTransactionState::Open ||
               state == KisPageTransactionState::Prepared;
    }

    bool isValid() const
    {
        return transaction.isValid() && state != KisPageTransactionState::Invalid
            && (state != KisPageTransactionState::Prepared || !changes.isEmpty()
                || !surfaceChanges.isEmpty() || !removedPages.isEmpty());
    }
};

enum class KisImageEpochCommitStatus : quint8 {
    Rejected,
    Committed,
    Conflict
};

struct KRITAIMAGE_EXPORT KisImageEpochCommitResult {
    KisImageEpochCommitStatus status = KisImageEpochCommitStatus::Rejected;
    KisImageEpochRootSnapshot root;
    QString error;

    bool isCommitted() const
    {
        return status == KisImageEpochCommitStatus::Committed && root.isValid();
    }
};

/**
 * Single-owner reference implementation of transaction visibility. It keeps
 * historical immutable roots so save/read snapshots never observe per-page
 * commit progress. Physical page transitions remain owned by
 * KisPageStateMachine and are intentionally not performed here.
 */
class KRITAIMAGE_EXPORT KisImageEpochReferenceModel
{
public:
    KisImageEpochReferenceModel();
    ~KisImageEpochReferenceModel();
    KisImageEpochReferenceModel(const KisImageEpochReferenceModel &) = delete;
    KisImageEpochReferenceModel &operator=(const KisImageEpochReferenceModel &) = delete;

    bool initialize(const KisImageEpochSnapshot &initial, QString *error = nullptr);

    // Admission and protection acquisition are atomic under the epoch gate.
    // A base must be current or still protected by a snapshot/active transaction;
    // a queued retired root and a raw immutable root copy grant no admission.
    KisPageTransaction beginTransaction(KisImageEpochId baseEpoch, QString *error = nullptr);
    bool prepare(const KisPreparedPageSet &preparedPages, QString *error = nullptr);
    KisImageEpochCommitResult commit(const KisPageTransaction &transaction);
    bool abort(const KisPageTransaction &transaction, QString *error = nullptr);

    KisImageEpochRootSnapshot captureCommittedRoot() const;
    // Full manifest export for checkpoint/serialization callers.
    KisRetainedImageEpochSnapshot captureRetainedSnapshot();
    // Structurally shared root capability for history/control-plane use.
    KisRetainedImageEpochSnapshot captureRetainedRoot();
    // Same admission rule as beginTransaction; cannot revive retired epochs.
    KisRetainedImageEpochSnapshot retainSnapshot(KisImageEpochId epoch);
    bool validateRetainedSnapshot(const KisRetainedImageEpochSnapshot &retained) const;
    bool
    releaseSnapshot(KisImageEpochSnapshotToken token, QString *error = nullptr, bool *rootBecameUnretained = nullptr);
    qsizetype retainedSnapshotCount() const;
    qsizetype rootCount() const; // Physical registry, including retired roots awaiting collection.
    qsizetype activeTransactionCount() const;
    // Retirement links live in already-created registry records. Enqueue at
    // publication/final release does not allocate a queue node or dedup entry.
    // Budgets count examined queue entries, including still-protected roots,
    // not only successful removals. Negative selects the explicit full drain.
    qsizetype collectFinishedTransactions(qsizetype budget = -1);
    qsizetype collectUnretainedRoots(qsizetype budget = -1);
    bool hasCollectionWork() const;
    // Current + distinct snapshot/transaction-protected roots only. Optional
    // counter is unique roots probed in this call (zero for empty keys), not
    // elapsed time, tree nodes, or snapshot-token count. Deferred garbage does
    // not enlarge logical reachability. Physical pins/last-use are separate.
    QSet<KisPageVersion> reachablePageVersions(const QVector<KisPageKey> &registeredKeys,
                                               quint64 *visitedRoots = nullptr) const;
    // Structural metadata only, and only while admitted. Holding the returned
    // copy does not retain pixel versions or authorize later epoch admission.
    KisImageEpochRootSnapshot root(KisImageEpochId epoch) const;
    KisPageTransactionSnapshot transaction(KisPageTransactionId id) const;
    bool resolve(const KisPageKey &key, const KisPageReadView &view, KisPageVersion *version) const;
    bool surfaceState(KisSurfaceId surface, const KisPageReadView &view, KisSurfaceEpochState *state) const;

private:
    class Private;
    KisRetainedImageEpochSnapshot retainRootLocked(const KisImageEpochRootSnapshot &,
                                                    bool completeManifest);
    KisRetainedImageEpochSnapshot captureCurrentRetainedRootLocked(bool completeManifest);
    // Owner-only, single-use candidate. It reserves an unpublished index slot
    // so install performs no root-tree construction or root-index allocation.
    // Neither a pixel pin nor permission to mutate backing. The owner still
    // protects provider/metadata/retention facts across prepare and install.
    class PreparedRootReservation
    {
    public:
        PreparedRootReservation() = default;
        ~PreparedRootReservation();
        PreparedRootReservation(PreparedRootReservation &&) noexcept = default;
        PreparedRootReservation &operator=(PreparedRootReservation &&other) noexcept;
        PreparedRootReservation(const PreparedRootReservation &) = delete;
        PreparedRootReservation &operator=(const PreparedRootReservation &) = delete;
        bool isValid() const { return m_owner && m_root.isValid(); }

    private:
        void cancel();
        QSharedPointer<Private> m_owner;
        KisImageEpochRootSnapshot m_root;
        friend class KisImageEpochReferenceModel;
    };

    class PreparedCommit : public PreparedRootReservation
    {
    public:
        PreparedCommit() = default;
        PreparedCommit(PreparedCommit &&) noexcept = default;
        PreparedCommit &operator=(PreparedCommit &&) noexcept = default;
        PreparedCommit(const PreparedCommit &) = delete;
        PreparedCommit &operator=(const PreparedCommit &) = delete;

    private:
        KisPageTransactionId m_transaction;
        quint64 m_revision = 0;
        friend class KisImageEpochReferenceModel;
    };

    PreparedCommit prepareCommit(const KisPageTransaction &transaction, KisImageEpochCommitResult *failure);
    using InstallMetadataFunction = bool (*)(void *, KisImageEpochId);
    bool installReservedRoot(PreparedRootReservation &candidate, void *context,
                             InstallMetadataFunction installMetadata,
                             KisImageEpochCommitResult *result);
    KisImageEpochCommitResult
    installCommit(PreparedCommit &&candidate, void *context, InstallMetadataFunction installMetadata);
    PreparedRootReservation prepareRestore(const KisRetainedImageEpochSnapshot &retained,
                                           KisImageEpochCommitResult *failure);
    KisImageEpochCommitResult
    installRestore(PreparedRootReservation &&candidate, void *context,
                   InstallMetadataFunction installMetadata);
    KisImageEpochRootSnapshot retainedRoot(KisImageEpochSnapshotToken token, KisImageEpochId epoch) const;
    // Only the canonical owner may install metadata here. Called after every
    // fallible root check, before visibility switches, under the epoch mutex.
    // Must not reenter this model, call providers, wait, or partially mutate on
    // failure. Not a public extension callback.
    friend class KisPageStore;
    friend class KisPagePublicationCoordinator;
    friend class KisPageStoreReferenceTest;
    // A cancelled candidate can safely outlive this facade.
    QSharedPointer<Private> d;
};

#endif // KIS_IMAGE_EPOCH_REFERENCE_MODEL_H

/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_PUBLICATION_COORDINATOR_P_H
#define KIS_PAGE_PUBLICATION_COORDINATOR_P_H

#include "KisImageEpochReferenceModel.h"
#include "KisPageHistoryCollector_p.h"
#include "KisPageMetadataCoordinator.h"
#include "KisPageOwnerLedger.h"
#include "KisPageRetirementQueue_p.h"
#include "KisPageStore.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QVector>

#include <memory>
#include <atomic>
#include <unordered_map>
#include <unordered_set>

struct KisPagePublicationCoordinatorSnapshot {
    qsizetype preparedProofs = 0;
    qsizetype preparedSurfaceChanges = 0;
    qsizetype stagedPageRemovals = 0;
    quint64 committedTransactions = 0;
};

class KisPageReadCleanup;

/**
 * Owner-gated transaction publication state.
 *
 * The original transaction delta is the only queryable overlay. Prepared
 * overlay/commit aggregates carry actual installation storage, not another
 * selection authority. All callers reach that delta through this service.
 */
class KisPagePublicationCoordinator final
{
    friend class KisPageWriteCoordinator;
    struct PageHash {
        size_t operator()(const KisPageKey &key) const noexcept { return qHash(key, seed); }
        size_t seed = QHashSeed::globalSeed();
    };
    using ProofMap = std::unordered_map<KisPageKey, KisPreparedPageProof, PageHash>;
    using RemovalSet = std::unordered_set<KisPageKey, PageHash>;
    struct DescriptorHash {
        size_t operator()(const KisPageVersion &version) const noexcept { return qHash(version, seed); }
        size_t seed = QHashSeed::globalSeed();
    };
    using DescriptorMap = std::unordered_map<KisPageVersion, KisPageAllocationDescriptor, DescriptorHash>;
    struct PreparedDescriptorChange {
        KisPageVersion version;
        KisPageAllocationDescriptor descriptor;
    };
public:
    struct OverlayChange {
        KisPageKey key;
        KisPreparedPageProof proof;
        bool removal = false;
    };

    class KisPreparedOverlayUpdate final
    {
    public:
        KisPreparedOverlayUpdate();
        ~KisPreparedOverlayUpdate();
        KisPreparedOverlayUpdate(KisPreparedOverlayUpdate &&) noexcept;
        KisPreparedOverlayUpdate &operator=(KisPreparedOverlayUpdate &&) noexcept;
        KisPreparedOverlayUpdate(const KisPreparedOverlayUpdate &) = delete;
        KisPreparedOverlayUpdate &operator=(const KisPreparedOverlayUpdate &) = delete;

        bool isValid() const;
        qsizetype metadataChangeCount() const;
        bool prepare(QString *error);
        // Caller keeps the owner gate from this last preparation through
        // tryInstallLocked, so sibling page/extent changes cannot be lost.
        bool prepareSurfaceLocked(QString *error);
        bool tryInstallLocked(
            KisPageMetadataCoordinator::DeferredPublicationCleanup *metadataCleanup,
            QString *error);
        void collectRetirementsLocked(QVector<KisPageTransitionEffect> *retirementEffects);

    private:
        class Data;
        std::unique_ptr<Data> data;
        void cancel() noexcept;
        friend class KisPagePublicationCoordinator;
    };

    using TransactionHasMutationActivity = bool (*)(void *context, KisPageTransactionId transaction);
    using PageWriteClaimed = bool (*)(void *context, const KisPageKey &key);
    using BeginMutationPreparation = bool (*)(void *context, KisPageTransactionId transaction,
                                              QMutexLocker<QMutex> &ownerLock, QString *error);
    using MutationPreparation = void (*)(void *context, KisPageTransactionId transaction);
    using DisposeMetadataCleanup = void (*)(void *context,
                                            KisPageMetadataCoordinator::DeferredPublicationCleanup cleanup);
    using RestoreIsIdle = bool (*)(void *context);
    using PrepareAbort = bool (*)(void *context,
                                  KisPageTransactionId transaction,
                                  QVector<KisPageTransitionEffect> *retirementEffects,
                                  KisPageReadCleanup &cleanup);

    KisPagePublicationCoordinator(KisImageEpochReferenceModel &epochs,
                                  KisPageMetadataCoordinator &metadata,
                                  KisPageOwnerLedger &owner,
                                  KisPageRetirementQueue &retirementQueue,
                                  KisPageHistoryCollector &history,
                                  KisCompletionTicket &readyHostCompletion,
                                  qsizetype &activeProviderCalls,
                                  bool &operational,
                                  bool &backgroundReclamation,
                                  void *ownerContext,
                                  TransactionHasMutationActivity transactionHasMutationActivity,
                                  PageWriteClaimed pageWriteClaimed,
                                  BeginMutationPreparation beginMutationPreparation,
                                  MutationPreparation endMutationPreparation,
                                  DisposeMetadataCleanup disposeMetadataCleanup,
                                  RestoreIsIdle restoreIsIdle,
                                  PrepareAbort prepareAbort);
    ~KisPagePublicationCoordinator() = default;

    KisPagePublicationCoordinator(const KisPagePublicationCoordinator &) = delete;
    KisPagePublicationCoordinator &operator=(const KisPagePublicationCoordinator &) = delete;
    KisPagePublicationCoordinator(KisPagePublicationCoordinator &&) = delete;
    KisPagePublicationCoordinator &operator=(KisPagePublicationCoordinator &&) = delete;

    bool resolveSurfaceLocked(KisSurfaceId surface, const KisPageReadView &view, KisSurfaceEpochState *state) const;
    bool resolveVersionLocked(const KisPageKey &key, const KisPageReadView &view, KisPageVersion *version) const;
    bool ensureVirtualDefaultLocked(const KisPageVersion &, const KisSurfaceEpochState &, QString *error);
    bool importDefaultRevisionLocked(KisSurfaceId surface, quint64 revision, QString *error);
    bool configureDerivedExtentLocked(KisSurfaceId surface);

    bool stageSurfaceMetadataLocked(const KisPageTransaction &transaction,
                                    const KisSurfaceEpochState &after,
                                    QString *error);
    bool stageSurfaceDefaultPixelLocked(const KisPageTransaction &transaction,
                                        KisSurfaceId surface,
                                        const QByteArray &pixel,
                                        QString *error);
    bool stagePageRemovalLocked(const KisPageTransaction &transaction,
                                const KisPageVersion &observed,
                                QString *error,
                                QMutexLocker<QMutex> &ownerLock);
    KisPreparedPageSet preparedPagesLocked(const KisPageTransaction &transaction) const;
    bool preparedPageExtentLocked(const KisPageTransaction &transaction,
                                  KisSurfaceId surface,
                                  QRect *extent,
                                  QString *error,
                                  QMutexLocker<QMutex> &ownerLock) const;
    KisImageEpochCommitTicket commitLocked(const KisPageTransaction &transaction,
                                           const KisPreparedPageSet &preparedPages,
                                           KisRetainedImageEpochSnapshot *retainedAfter,
                                           const KisPageStore *diagnosticOwner,
                                           QMutexLocker<QMutex> &ownerLock,
                                           QWriteLocker *publicationLock = nullptr);
    KisImageEpochCommitTicket restoreRetainedEpochLocked(const KisRetainedImageEpochSnapshot &retained,
                                                         const QVector<KisPageKey> *changedPages,
                                                         const KisPageStore *diagnosticOwner,
                                                         QMutexLocker<QMutex> &ownerLock);
    bool abortLocked(const KisPageTransaction &transaction, QMutexLocker<QMutex> &ownerLock,
                     KisPageReadCleanup &cleanup);

    bool isPreparingCommitLocked(KisPageTransactionId transaction) const;

    KisPreparedPageProof findPreparedProofLocked(KisPageTransactionId transaction, const KisPageKey &key) const;
    KisPreparedPageSet transactionDeltaLocked(KisPageTransactionId transaction) const;
    bool stagesRemovalLocked(KisPageTransactionId transaction, const KisPageKey &key) const;
    KisPreparedOverlayUpdate prepareOverlayUpdateLocked(
        const KisPageTransaction &transaction,
        QVector<OverlayChange> changes,
        QString *error);
    bool revokePreparedProofLocked(const KisPreparedPageProof &proof);

    void putDescriptorLocked(const KisPageVersion &version, const KisPageAllocationDescriptor &descriptor);
    void removeDescriptorLocked(const KisPageVersion &version);
    bool descriptorLocked(const KisPageVersion &version, KisPageAllocationDescriptor *descriptor = nullptr) const;
    DescriptorMap prepareDescriptorAdditionsLocked(const QVector<PreparedDescriptorChange> &changes);
    void installDescriptorAdditionsLocked(DescriptorMap *prepared);

    KisPageStorePublicationStatistics statisticsLocked() const;
    KisPagePublicationCoordinatorSnapshot snapshotLocked() const;

private:
    struct PreparedTransactionState {
        ProofMap proofs;
        QVector<KisSurfaceEpochChange> surfaceChanges;
        RemovalSet removals;
        // Storage only: overlapping candidates must reserve enough buckets
        // for every constructed node. Cancellation may run outside the owner
        // gate; it only gives capacity back, never edits the visible delta.
        std::atomic<size_t> proofInsertions{0};
        std::atomic<size_t> removalInsertions{0};

        bool isEmpty() const
        {
            return proofs.empty() && surfaceChanges.isEmpty() && removals.empty()
                && !proofInsertions.load() && !removalInsertions.load();
        }
    };
    std::shared_ptr<PreparedTransactionState> preparedStateLocked(KisPageTransactionId transaction);
    const ProofMap *findProofsLocked(KisPageTransactionId transaction) const;

    /**
     * One-shot publication aggregate. Metadata and epoch candidates keep
     * their own authoritative storage; this facade owns descriptor changes,
     * backing admission and completion needed during installation.
     */
    class KisPreparedMutationCommit final
    {
    public:
        KisPreparedMutationCommit();
        ~KisPreparedMutationCommit();
        KisPreparedMutationCommit(KisPreparedMutationCommit &&) noexcept;
        KisPreparedMutationCommit &operator=(KisPreparedMutationCommit &&) noexcept;
        KisPreparedMutationCommit(const KisPreparedMutationCommit &) = delete;
        KisPreparedMutationCommit &operator=(const KisPreparedMutationCommit &) = delete;
        bool isValid() const;
        bool tryInstall();

    private:
        class Data;
        std::unique_ptr<Data> data;
        void cancel() noexcept;
        friend class KisPagePublicationCoordinator;
    };

    KisImageEpochReferenceModel &m_epochs;
    KisPageMetadataCoordinator &m_metadata;
    KisPageOwnerLedger &m_owner;
    KisPageRetirementQueue &m_retirementQueue;
    KisPageHistoryCollector &m_history;
    KisCompletionTicket &m_readyHostCompletion;
    qsizetype &m_activeProviderCalls;
    bool &m_operational;
    bool &m_backgroundReclamation;
    void *m_ownerContext = nullptr;
    TransactionHasMutationActivity m_transactionHasMutationActivity = nullptr;
    PageWriteClaimed m_pageWriteClaimed = nullptr;
    BeginMutationPreparation m_beginMutationPreparation = nullptr;
    MutationPreparation m_endMutationPreparation = nullptr;
    DisposeMetadataCleanup m_disposeMetadataCleanup = nullptr;
    RestoreIsIdle m_restoreIsIdle = nullptr;
    PrepareAbort m_prepareAbort = nullptr;

    QSet<quint64> m_preparingCommits;
    QHash<quint64, std::shared_ptr<PreparedTransactionState>> m_preparedTransactions;
    // Immutable Tiles3 surface policy, not a second extent or page registry.
    KisSurfaceId m_derivedExtentSurface;
    QHash<quint64, quint64> m_defaultRevisionHighWater;
    DescriptorMap m_descriptors;
    quint64 m_descriptorRevision = 1;
    KisPageStorePublicationStatistics m_statistics;
    quint64 m_committedTransactions = 0;

    bool hasActiveTransactionLocked(const KisPageTransaction &transaction) const;
    quint64 currentDefaultRevisionLocked(KisSurfaceId surface) const;
    bool reserveDefaultRevisionLocked(KisSurfaceId surface, quint64 revision, QString *error);
    void retireEffectsUnlocked(QVector<KisPageTransitionEffect> effects,
                               QMutexLocker<QMutex> &ownerLock);
};

#endif // KIS_PAGE_PUBLICATION_COORDINATOR_P_H

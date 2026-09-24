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

struct KisPagePublicationCoordinatorSnapshot {
    qsizetype preparedProofs = 0;
    qsizetype preparedSurfaceChanges = 0;
    qsizetype stagedPageRemovals = 0;
    quint64 committedTransactions = 0;
};

/**
 * Owner-gated transaction publication state.
 *
 * R1 deliberately keeps the existing publication algorithm and lock order,
 * while moving its side maps behind one by-value service.  Later metadata
 * milestones replace these maps with KisPreparedMutationCommit; callers must
 * therefore reach them only through this coordinator.
 */
class KisPagePublicationCoordinator final
{
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
        bool tryInstallLocked(
            QVector<KisPageTransitionEffect> *retirementEffects,
            KisPageMetadataCoordinator::DeferredPublicationCleanup *metadataCleanup,
            QString *error);

    private:
        class Data;
        std::unique_ptr<Data> data;
        void cancel() noexcept;
        friend class KisPagePublicationCoordinator;
    };

    using TransactionHasMutationActivity = bool (*)(void *context, KisPageTransactionId transaction);
    using PageWriteClaimed = bool (*)(void *context, const KisPageKey &key);
    using MutationPreparation = void (*)(void *context, KisPageTransactionId transaction);
    using DisposeMetadataCleanup = void (*)(void *context,
                                            KisPageMetadataCoordinator::DeferredPublicationCleanup cleanup);
    using RestoreIsIdle = bool (*)(void *context);
    using PrepareAbort = bool (*)(void *context,
                                  KisPageTransactionId transaction,
                                  QVector<KisPageTransitionEffect> *retirementEffects);

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
                                  MutationPreparation beginMutationPreparation,
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
                                           QMutexLocker<QMutex> &ownerLock);
    KisImageEpochCommitTicket restoreRetainedEpochLocked(const KisRetainedImageEpochSnapshot &retained,
                                                         const QVector<KisPageKey> *changedPages,
                                                         const KisPageStore *diagnosticOwner,
                                                         QMutexLocker<QMutex> &ownerLock);
    bool abortLocked(const KisPageTransaction &transaction, QMutexLocker<QMutex> &ownerLock);

    bool isPreparingCommitLocked(KisPageTransactionId transaction) const;

    const QHash<KisPageKey, KisPreparedPageProof> *findProofsLocked(KisPageTransactionId transaction) const;
    KisPreparedPageSet transactionDeltaLocked(KisPageTransactionId transaction) const;
    bool stagesRemovalLocked(KisPageTransactionId transaction, const KisPageKey &key) const;
    KisPreparedOverlayUpdate prepareOverlayUpdateLocked(
        const KisPageTransaction &transaction,
        QVector<OverlayChange> changes,
        QString *error);
    bool revokePreparedProofLocked(const KisPreparedPageProof &proof);
    void installPreparedProofLocked(const KisPreparedPageProof &proof);


    void setRemovalLocked(KisPageTransactionId transaction, const KisPageKey &key, bool removed);

    quint64 &defaultRevisionHighWaterLocked(KisSurfaceId surface);

    void putDescriptorLocked(const KisPageVersion &version, const KisPageAllocationDescriptor &descriptor);
    void removeDescriptorLocked(const KisPageVersion &version);
    bool descriptorLocked(const KisPageVersion &version, KisPageAllocationDescriptor *descriptor = nullptr) const;
    void reserveDescriptorAdditionsLocked(qsizetype additions);

    KisPageStorePublicationStatistics statisticsLocked() const;
    KisPagePublicationCoordinatorSnapshot snapshotLocked() const;

private:
    struct PreparedTransactionState {
        QHash<KisPageKey, KisPreparedPageProof> proofs;
        QVector<KisSurfaceEpochChange> surfaceChanges;
        QSet<KisPageKey> removals;

        bool isEmpty() const
        {
            return proofs.isEmpty() && surfaceChanges.isEmpty() && removals.isEmpty();
        }
    };

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
    MutationPreparation m_beginMutationPreparation = nullptr;
    MutationPreparation m_endMutationPreparation = nullptr;
    DisposeMetadataCleanup m_disposeMetadataCleanup = nullptr;
    RestoreIsIdle m_restoreIsIdle = nullptr;
    PrepareAbort m_prepareAbort = nullptr;

    QSet<quint64> m_preparingCommits;
    QHash<quint64, PreparedTransactionState> m_preparedTransactions;
    QHash<quint64, quint64> m_defaultRevisionHighWater;
    QHash<KisPageVersion, KisPageAllocationDescriptor> m_descriptors;
    quint64 m_descriptorRevision = 1;
    KisPageStorePublicationStatistics m_statistics;
    quint64 m_committedTransactions = 0;

    bool hasActiveTransactionLocked(const KisPageTransaction &transaction) const;
    void retireEffectsUnlocked(QVector<KisPageTransitionEffect> effects,
                               QMutexLocker<QMutex> &ownerLock);
};

#endif // KIS_PAGE_PUBLICATION_COORDINATOR_P_H

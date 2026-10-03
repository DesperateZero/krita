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

#include <QMutex>
#include <QMutexLocker>
#include <QVector>
#include <boost/intrusive/list.hpp>

#include <memory>
#include <map>
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
struct KisPageCapturedRelease;

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
    friend class KisPageStoreCpuMutationTest;
    struct PageHash {
        size_t operator()(const KisPageKey &key) const noexcept { return qHash(key, seed); }
        size_t seed = QHashSeed::globalSeed();
    };
    using ProofMap = std::unordered_map<KisPageKey, KisPreparedPageProof, PageHash,
        std::equal_to<KisPageKey>, KisMutationStorageAllocator<std::pair<const KisPageKey, KisPreparedPageProof>>>;
    using RemovalSet = std::unordered_set<KisPageKey, PageHash,
        std::equal_to<KisPageKey>, KisMutationStorageAllocator<KisPageKey>>;
    struct VersionLess {
        bool operator()(const KisPageVersion &a, const KisPageVersion &b) const noexcept {
            if (a.key.surface.value != b.key.surface.value) return a.key.surface.value < b.key.surface.value;
            if (a.key.page.row != b.key.page.row) return a.key.page.row < b.key.page.row;
            if (a.key.page.column != b.key.page.column) return a.key.page.column < b.key.page.column;
            if (a.generation.value != b.generation.value) return a.generation.value < b.generation.value;
            return a.defaultPixelRevision < b.defaultPixelRevision;
        }
    };
    using DescriptorMap = std::map<KisPageVersion, std::shared_ptr<const KisPageAllocationDescriptor>, VersionLess,
        KisMutationStorageAllocator<std::pair<const KisPageVersion, std::shared_ptr<const KisPageAllocationDescriptor>>>>;
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

    class KRITAIMAGE_EXPORT KisPreparedOverlayUpdate final
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
        void collectRetirementsLocked();

    private:
        class Data;
        struct DataDeleter {
            KisMutationStorageAllocator<Data> storage;
            void operator()(Data *) const noexcept;
        };
        std::unique_ptr<Data, DataDeleter> data;
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
                                  KisPageReadCleanup &cleanup);

    KRITAIMAGE_EXPORT KisPagePublicationCoordinator(KisImageEpochReferenceModel &epochs,
                                  KisPageMetadataCoordinator &metadata,
                                  KisPageOwnerLedger &owner,
                                  KisBackingBudgetController &budget,
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
    KRITAIMAGE_EXPORT bool ensureVirtualDefaultLocked(const KisPageVersion &, const KisSurfaceEpochState &, QString *error);
    KRITAIMAGE_EXPORT bool prepareDefaultRevisionsLocked(const KisPageSnapshotArray<KisSurfaceEpochState> &surfaces, QString *error);
    KRITAIMAGE_EXPORT bool configureDerivedExtentLocked(KisSurfaceId surface);

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
    KisImageEpochCommitTicket commitLocked(const KisPageTransaction &transaction,
                                           const KisPreparedPageSet &preparedPages,
                                           KisRetainedImageEpochSnapshot *retainedAfter,
                                           const KisPageStore *diagnosticOwner,
                                           QMutexLocker<QMutex> &ownerLock,
                                           QWriteLocker *publicationLock = nullptr);
    KisImageEpochCommitTicket restoreRetainedEpochLocked(const KisRetainedImageEpochSnapshot &retained,
                                                         const KisPageKeyStorage *changedPages,
                                                         const KisPageStore *diagnosticOwner,
                                                         QMutexLocker<QMutex> &ownerLock);
    bool abortLocked(const KisPageTransaction &transaction, QMutexLocker<QMutex> &ownerLock,
                     KisPageReadCleanup &cleanup);

    bool isPreparingCommitLocked(KisPageTransactionId transaction) const;

    KisPreparedPageProof findPreparedProofLocked(KisPageTransactionId transaction, const KisPageKey &key) const;
    KisPreparedPageSet transactionDeltaLocked(KisPageTransactionId transaction) const;
    bool captureDeltaLocked(KisPageTransactionId transaction, KisPageCapturedRelease &capture,
                            size_t *versions, size_t *removals, size_t *surfaces) const;
    bool stagesRemovalLocked(KisPageTransactionId transaction, const KisPageKey &key) const;
    KRITAIMAGE_EXPORT KisPreparedOverlayUpdate prepareOverlayUpdateLocked(
        const KisPageTransaction &transaction,
        const OverlayChange *changes, size_t count,
        QString *error);

    KRITAIMAGE_EXPORT DescriptorMap prepareDescriptorLocked(const KisPageVersion &version, const KisPageAllocationDescriptor &descriptor);
    void removeDescriptorLocked(const KisPageVersion &version);
    // A borrow lasts until its version's descriptor is replaced or removed.
    // The native page's original claim/session excludes both during its use.
    const KisPageAllocationDescriptor *descriptorLocked(
        const KisPageVersion &version, KisPageAllocationDescriptor *descriptor = nullptr) const;
    DescriptorMap prepareDescriptorAdditionsLocked(const PreparedDescriptorChange *changes, size_t count);
    KRITAIMAGE_EXPORT void installDescriptorAdditionsLocked(DescriptorMap *prepared);

    KisPageStorePublicationStatistics statisticsLocked() const;
    KisPagePublicationCoordinatorSnapshot snapshotLocked() const;

private:
    void revokePreparedProofLocked(const KisPreparedPageProof &proof) noexcept;
    struct PreparedTransactionState {
        explicit PreparedTransactionState(const KisMutationStorageAllocator<char> &storage)
            : proofs(0, PageHash{}, std::equal_to<KisPageKey>{}, storage)
            , surfaceChanges(storage)
            , removals(0, PageHash{}, std::equal_to<KisPageKey>{}, storage) {}
        ProofMap proofs;
        std::vector<KisSurfaceEpochChange, KisMutationStorageAllocator<KisSurfaceEpochChange>> surfaceChanges;
        RemovalSet removals;
        // Storage only: overlapping candidates must reserve enough buckets
        // for every constructed node. Cancellation may run outside the owner
        // gate; it only gives capacity back, never edits the visible delta.
        std::atomic<size_t> proofInsertions{0};
        std::atomic<size_t> removalInsertions{0};

        bool isEmpty() const
        {
            return proofs.empty() && surfaceChanges.empty() && removals.empty()
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
        KRITAIMAGE_EXPORT explicit KisPreparedMutationCommit(KisBackingBudgetController &budget);
        KRITAIMAGE_EXPORT ~KisPreparedMutationCommit();
        KisPreparedMutationCommit(KisPreparedMutationCommit &&) noexcept;
        KisPreparedMutationCommit &operator=(KisPreparedMutationCommit &&) noexcept;
        KisPreparedMutationCommit(const KisPreparedMutationCommit &) = delete;
        KisPreparedMutationCommit &operator=(const KisPreparedMutationCommit &) = delete;
        bool isValid() const;
        bool tryInstall();

    private:
        class Data;
        struct DataDeleter {
            KisMutationStorageAllocator<Data> storage;
            void operator()(Data *) const noexcept;
        };
        std::unique_ptr<Data, DataDeleter> data;
        void cancel() noexcept;
        friend class KisPagePublicationCoordinator;
    };

    KisImageEpochReferenceModel &m_epochs;
    KisPageMetadataCoordinator &m_metadata;
    KisPageOwnerLedger &m_owner;
    KisBackingBudgetController &m_budget;
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

    struct CommitPreparation : boost::intrusive::list_base_hook<> {
        explicit CommitPreparation(KisPageTransactionId value) : transaction(value) {}
        KisPageTransactionId transaction;
    };
    // The synchronous call owns the claim record through every owner unlock.
    // Publication/abort admission never allocates a container node.
    boost::intrusive::list<CommitPreparation> m_preparingCommits;
    using TransactionMap = std::map<quint64, std::shared_ptr<PreparedTransactionState>,
        std::less<quint64>, KisMutationStorageAllocator<std::pair<const quint64, std::shared_ptr<PreparedTransactionState>>>>;
    TransactionMap m_preparedTransactions;
    // Immutable Tiles3 surface policy, not a second extent or page registry.
    KisSurfaceId m_derivedExtentSurface;
    using DefaultRevisionMap = std::map<quint64, quint64, std::less<quint64>,
        KisMutationStorageAllocator<std::pair<const quint64, quint64>>>;
    DefaultRevisionMap m_defaultRevisionHighWater;
    DescriptorMap m_descriptors;
    quint64 m_descriptorRevision = 1;
    KisPageStorePublicationStatistics m_statistics;
    quint64 m_committedTransactions = 0;

    bool hasActiveTransactionLocked(const KisPageTransaction &transaction) const;
    quint64 currentDefaultRevisionLocked(KisSurfaceId surface) const;
    bool reserveDefaultRevisionLocked(KisSurfaceId surface, quint64 revision, QString *error);
    void processRetirementsUnlocked(QMutexLocker<QMutex> &ownerLock);
};

#endif // KIS_PAGE_PUBLICATION_COORDINATOR_P_H

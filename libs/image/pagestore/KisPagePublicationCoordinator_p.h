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
#include "KisPagePublicationInput_p.h"

#include <QMutex>
#include <QMutexLocker>
#include <QVector>
#include <boost/intrusive/list.hpp>
#include <boost/container/small_vector.hpp>

#include <memory>
#include <optional>
#include <map>
#include <atomic>
#include <algorithm>
#include <tuple>
#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>

struct KisPagePublicationCoordinatorSnapshot {
    qsizetype preparedProofs = 0;
    qsizetype preparedSurfaceChanges = 0;
    qsizetype stagedPageRemovals = 0;
    quint64 committedTransactions = 0;
};

class KisPageReadCleanup;
struct KisPageCapturedRelease;

// The publication owner's original mutable delta. Epoch preparation borrows
// these containers only while the owner's CommitPreparation excludes edits;
// it still creates its own necessary immutable version projection.
struct KisPagePublicationDelta {
    struct PageLess {
        bool operator()(const KisPageKey &a, const KisPageKey &b) const noexcept
        {
            return std::tie(a.surface.value, a.page.row, a.page.column)
                < std::tie(b.surface.value, b.page.row, b.page.column);
        }
    };
    struct PageState {
        KisPagePublicationProof proof;
    };
    using ProofMap = boost::container::flat_map<KisPageKey, PageState, PageLess,
        KisMutationStorageAllocator<std::pair<KisPageKey, PageState>>>;
    using RemovalSet = boost::container::flat_set<KisPageKey, PageLess,
        KisMutationStorageAllocator<KisPageKey>>;
    explicit KisPagePublicationDelta(const KisMutationStorageAllocator<char> &storage)
        : proofs(PageLess{}, storage), surfaceChanges(storage), removals(PageLess{}, storage) {}
    ProofMap proofs;
    std::vector<KisSurfaceEpochChange, KisMutationStorageAllocator<KisSurfaceEpochChange>> surfaceChanges;
    RemovalSet removals;
    // Capacity only. Cancellation may release it without the owner gate.
    std::atomic<size_t> proofInsertions{0};
    std::atomic<size_t> removalInsertions{0};
    bool hasChanges() const
    {
        return !proofs.empty() || !surfaceChanges.empty() || !removals.empty();
    }
    bool isEmpty() const
    {
        return !hasChanges() && !proofInsertions.load() && !removalInsertions.load();
    }
};

// A borrowed projection of the original operation's prepared rows. It owns
// no proof nodes, transaction state or recovery responsibility. Both epoch
// and publication preparation consume it while the original owner excludes
// edits. Filtering visits each row once; no per-index map walk is hidden here.
struct KisPageOperationPublication {
    template<bool Proof> struct Pages {
        const KisPagePublicationChanges &rows;
        size_t length;
        struct Iterator {
            using iterator_category = std::forward_iterator_tag;
            using difference_type = std::ptrdiff_t;
            using value_type = std::conditional_t<Proof, KisPagePublicationChange::Replacement, KisPageKey>;
            using pointer = const value_type *;
            using reference = const value_type &;
            KisPagePublicationChanges::const_iterator current, last;
            bool selected() const {
                return Proof ? !current->replacement.removal
                    : current->replacement.removal && current->removalWasInBase;
            }
            void skip() { while (current != last && !selected()) ++current; }
            Iterator &operator++() { ++current; skip(); return *this; }
            bool operator!=(const Iterator &other) const { return current != other.current; }
            bool operator==(const Iterator &other) const { return current == other.current; }
            const auto &operator*() const {
                if constexpr (Proof) return current->replacement;
                else return current->replacement.key();
            }
        };
        Iterator begin() const { Iterator it{rows.begin(), rows.end()}; it.skip(); return it; }
        Iterator end() const { return {rows.end(), rows.end()}; }
        size_t size() const { return length; }
        bool count(const KisPageKey &key) const {
            const auto found = std::lower_bound(rows.begin(), rows.end(), key, [](const auto &a, const auto &b) {
                const auto &key = a.replacement.key();
                return std::tie(key.surface.value, key.page.row, key.page.column)
                    < std::tie(b.surface.value, b.page.row, b.page.column);
            });
            return found != rows.end() && found->replacement.key() == key && !found->replacement.removal;
        }
    };
    struct Surfaces {
        const KisSurfaceEpochChange *value;
        size_t length;
        const auto *begin() const { return value; }
        const auto *end() const { return value + length; }
        const auto *cbegin() const { return begin(); }
        const auto *cend() const { return end(); }
        size_t size() const { return length; }
    };
    Pages<true> proofs;
    Pages<false> removals;
    Surfaces surfaceChanges;
};

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
    using ProofMap = KisPagePublicationDelta::ProofMap;
    // Installation storage only; the original metadata version owns layout.
    // A single target stays inline instead of allocating a replacement node.
    using DescriptorEntry = std::pair<KisPageVersion, std::shared_ptr<const KisPageAllocationDescriptor>>;
    class PreparedDescriptors final
    {
        using Entries = boost::container::small_vector<DescriptorEntry, 1,
            KisMutationStorageAllocator<DescriptorEntry>>;
    public:
        using allocator_type = Entries::allocator_type;
        PreparedDescriptors() = default;
        explicit PreparedDescriptors(const allocator_type &allocator)
            : entries(std::in_place, allocator) {}
        PreparedDescriptors(PreparedDescriptors &&other)
        { *this = std::move(other); }
        PreparedDescriptors &operator=(PreparedDescriptors &&other)
        {
            if (this == &other) return *this;
            // Moving a preparation transfers the complete producer carrier.
            // Never assign its buffer through a different allocator domain.
            entries.reset();
            if (other.entries) entries.emplace(std::move(*other.entries));
            other.entries.reset();
            return *this;
        }
        bool hasStorage() const noexcept { return entries.has_value(); }
        bool empty() const noexcept { return !entries || entries->empty(); }
        size_t capacity() const noexcept { return entries ? entries->capacity() : 0; }
        static size_t internal_capacity() { return Entries::internal_capacity(); }
        void reserve(size_t count) { Q_ASSERT(entries); entries->reserve(count); }
        void emplace_back(KisPageVersion version, std::shared_ptr<const KisPageAllocationDescriptor> value)
        { Q_ASSERT(entries); entries->emplace_back(version, std::move(value)); }
        DescriptorEntry &front() { Q_ASSERT(entries && !entries->empty()); return entries->front(); }
        auto begin() { return entries ? entries->begin() : Entries::iterator{}; }
        auto end() { return entries ? entries->end() : Entries::iterator{}; }
        auto cbegin() const { return entries ? entries->cbegin() : Entries::const_iterator{}; }
        auto cend() const { return entries ? entries->cend() : Entries::const_iterator{}; }
        void clear() noexcept { if (entries) entries->clear(); }
        void reset() noexcept { entries.reset(); }
    private:
        std::optional<Entries> entries;
    };
    struct PreparedDescriptorChange {
        KisPageVersion version;
        KisPageAllocationDescriptor descriptor;
    };
public:
    using OverlayChange = KisPagePublicationChange::Input;

    // One synchronous selection window under the Store gate. The original
    // immutable root and transaction delta remain the authorities; this view
    // may not escape that gate. Independent reads retain metadata afterwards.
    struct NativeReadSelection {
        KisImageEpochRootSnapshot root;
        const KisPagePublicationDelta *delta = nullptr;
        KisSurfaceEpochState surface;
        bool resolve(const KisPageKey &, KisPageVersion *, KisVersionSlotId *) const;
    };
    bool prepareNativeReadSelectionLocked(KisSurfaceId, const KisPageReadView &,
                                         NativeReadSelection *) const;

    class KRITAIMAGE_EXPORT KisPreparedPagePublication final
    {
    public:
        KisPreparedPagePublication();
        ~KisPreparedPagePublication();
        KisPreparedPagePublication(KisPreparedPagePublication &&) noexcept;
        KisPreparedPagePublication &operator=(KisPreparedPagePublication &&) noexcept;
        KisPreparedPagePublication(const KisPreparedPagePublication &) = delete;
        KisPreparedPagePublication &operator=(const KisPreparedPagePublication &) = delete;

        bool isValid() const;
        qsizetype metadataChangeCount() const;
        // The candidate owns these facts through acceptance and delivery;
        // finishUnlocked is their last consumer. Order is surface/row/column;
        // keys are unique. This does not export storage.
        bool forEachAdapterChange(const void *, bool (*)(const void *, const KisPageKey &, bool)) const;
        bool prepare(QString *error, bool *storageRefused = nullptr);
        // Keep the owner gate from the latest surface preparation through
        // acceptance. A late capacity conflict drops it for one conservative
        // reprepare, then rechecks the original inputs and surface before install.
        bool prepareSurfaceLocked(QString *error, bool *storageRefused = nullptr);
        bool tryAcceptLocked(
            KisPageMetadataCoordinator::DeferredPublicationCleanup *metadataCleanup,
            QMutexLocker<QMutex> &ownerLock, QString *error, bool *storageRefused = nullptr);
        bool acceptsCanonical() const;
        bool tryAcceptCanonicalLocked(const KisPageStore *diagnosticOwner, QMutexLocker<QMutex> &ownerLock);
        // Caller first relinquishes any accepted producer proofs. Keep its
        // admission across cleanup. Enter/return unlocked; the gate is acquired
        // only for history collection. Consumes accepted or rejected preparation.
        void finishUnlocked(KisPageMetadataCoordinator::DeferredPublicationCleanup cleanup,
                            QMutexLocker<QMutex> &ownerLock);

    private:
        bool installOverlayLocked(KisPageMetadataCoordinator::DeferredPublicationCleanup *, QString *);
        // This preparation belongs to the original call/handle. Only actual
        // row and installation capacity allocates; the header has no separate
        // charged object or allocator-retention lifetime.
        struct Data {
            using Change = KisPagePublicationChange;
            explicit Data(const KisMutationStorageAllocator<char> &storage)
                : changes(storage), ownedAuthorities(storage) {}
            Data(Data &&) noexcept = default;
            Data &operator=(Data &&) noexcept = default;
            ~Data() { releaseCapacity(); }
            const Change *findChange(const KisPageKey &) const;
            void releaseCapacity() noexcept
            {
                // A moved-from header no longer owns the reservation.
                if (!state) return;
                if (proofInsertions) state->proofInsertions.fetch_sub(std::exchange(proofInsertions, 0));
                if (removalInsertions) state->removalInsertions.fetch_sub(std::exchange(removalInsertions, 0));
            }
            KisPagePublicationCoordinator *owner = nullptr;
            KisPageTransaction transaction;
            KisPagePublicationChanges changes;
            size_t mutationCount = 0;
            std::shared_ptr<KisPagePublicationDelta> state;
            std::vector<KisReplicaHandle, KisMutationStorageAllocator<KisReplicaHandle>> ownedAuthorities;
            size_t proofInsertions = 0;
            size_t removalInsertions = 0;
            KisPageMetadataCoordinator::PreparedPublication metadata;
            KisSurfaceEpochChange surfaceChange;
            qsizetype surfaceIndex = -1;
            bool surfacePrepared = false;
            bool prepared = false;
            bool installed = false;
            bool canonical = false;
            bool retryMutationCapacity = false;
        };
        std::optional<Data> data;
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
    bool transactionDirtyExtentLocked(const KisPageTransaction &, KisSurfaceId, QRect *extent) const;
    bool captureAllocatedPageInputsLocked(KisSurfaceId surface, const KisPageTransaction &transaction,
        KisImageEpochRootSnapshot *root, KisPageKeyStorage *additions, KisPageKeyStorage *removals) const;
    KisImageEpochCommitTicket commitLocked(const KisPageTransaction &transaction,
                                           const KisPreparedPageSet &preparedPages,
                                           KisRetainedImageEpochSnapshot *retainedAfter,
                                           const KisPageStore *diagnosticOwner,
                                           QMutexLocker<QMutex> &ownerLock);
    bool hasPreparedDeltaLocked(KisPageTransactionId transaction) const;
    bool hasActiveTransactionLocked(const KisPageTransaction &transaction) const;
    KisImageEpochCommitTicket commitOwnedLocked(const KisPageTransaction &transaction,
                                                KisRetainedImageEpochSnapshot *retainedAfter,
                                                const KisPageStore *diagnosticOwner,
                                                QMutexLocker<QMutex> &ownerLock);
    KisImageEpochCommitTicket restoreRetainedEpochLocked(const KisRetainedImageEpochSnapshot &retained,
                                                         const KisPageStore *diagnosticOwner,
                                                         QMutexLocker<QMutex> &ownerLock,
        const std::function<bool(const KisImageEpochRootSnapshot &)> &prepareAdapter,
        const std::function<void(const KisPageVersion *, qsizetype)> &completeAdapter);
    bool abortLocked(const KisPageTransaction &transaction, QMutexLocker<QMutex> &ownerLock,
                     KisPageReadCleanup &cleanup);

    bool isPreparingCommitLocked(KisPageTransactionId transaction) const;

    KisPreparedPageProof projectProof(const KisPagePublicationProof &) const;
    KisPageVersion findPreparedVersionLocked(KisPageTransactionId, const KisPageKey &,
                                            KisVersionSlotId *slot = nullptr) const;
    KisReplicaHandle takeIdleCapacityLocked(const KisPageKey &key);
    // Same original records and Store gate for H0, H1, expiry and close.
    qint64 expireIdleCapacityLocked(qint64 now, bool all = false);
    KisPreparedPageSet transactionDeltaLocked(KisPageTransactionId transaction) const;
    bool captureDeltaLocked(KisPageTransactionId transaction, KisPageCapturedRelease &capture,
                            size_t *versions, size_t *removals, size_t *surfaces) const;
    bool stagesRemovalLocked(KisPageTransactionId transaction, const KisPageKey &key) const;
    KRITAIMAGE_EXPORT KisPreparedPagePublication preparePagePublicationLocked(
        const KisPageTransaction &transaction,
        const OverlayChange *changes, size_t count,
        QString *error, bool *storageRefused = nullptr);
    KisPreparedPagePublication preparePagePublicationLocked(
        const KisPageTransaction &transaction, KisPagePublicationChanges changes,
        QString *error, bool *storageRefused, bool canonical);
    // Consume the caller's stable sequence directly into candidate storage.
    // The reader is borrowed synchronously and is never kept by the candidate.
    template<class ReadChange>
    KisPreparedPagePublication preparePagePublicationLocked(
        const KisPageTransaction &transaction, size_t count, const ReadChange &readChange,
        QString *error, bool *storageRefused = nullptr, bool canonical = false)
    {
        return preparePagePublicationInputLocked(transaction, &readChange, count,
            [](const void *input, size_t i) { return (*static_cast<const ReadChange *>(input))(i); },
            error, storageRefused, canonical);
    }

    void preparePrivateLayoutLocked(KisPagePrivateWrite &, const KisPageAllocationDescriptor &,
                                    std::shared_ptr<const KisPageAllocationDescriptor>);
    KRITAIMAGE_EXPORT PreparedDescriptors prepareDescriptorLocked(const KisPageVersion &version, const KisPageAllocationDescriptor &descriptor);
    KRITAIMAGE_EXPORT PreparedDescriptors prepareDescriptorLocked(const KisPageVersion &version,
        const KisPageAllocationDescriptor &descriptor, const KisPageVersion &base);
    void noteVersionExitLocked(const KisPageVersion &version);
    // A borrow lasts until its version's descriptor is replaced or removed.
    // The native page's original claim/session excludes both during its use.
    const KisPageAllocationDescriptor *descriptorLocked(
        const KisPageVersion &version, KisPageAllocationDescriptor *descriptor = nullptr) const;
    PreparedDescriptors prepareDescriptorAdditionsLocked(const PreparedDescriptorChange *changes, size_t count,
        const std::shared_ptr<const KisPageAllocationDescriptor> *sharedLayout = nullptr);
    KRITAIMAGE_EXPORT void installDescriptorAdditionsLocked(PreparedDescriptors *prepared);
    void discardDescriptorAdditionsLocked(PreparedDescriptors *, QMutexLocker<QMutex> &ownerLock);

    KisPageStorePublicationStatistics statisticsLocked() const;
    KisPagePublicationCoordinatorSnapshot snapshotLocked() const;

private:
    KRITAIMAGE_EXPORT KisPreparedPagePublication preparePagePublicationInputLocked(
        const KisPageTransaction &transaction, const void *input, size_t count,
        OverlayChange (*readChange)(const void *, size_t), QString *error, bool *storageRefused,
        bool canonical = false);
    template<class Input>
    KisImageEpochCommitTicket commitInputLocked(const KisPageTransaction &transaction, const Input &input,
        KisRetainedImageEpochSnapshot *retainedAfter, const KisPageStore *diagnosticOwner,
        QMutexLocker<QMutex> &ownerLock);
    void finishAbortedProofLocked(const KisPagePublicationProof &proof) noexcept;
    std::shared_ptr<KisPagePublicationDelta> preparedStateLocked(KisPageTransactionId transaction);
    const ProofMap *findProofsLocked(KisPageTransactionId transaction) const;

    /**
     * One-shot publication aggregate. Metadata and epoch candidates keep
     * their own authoritative storage; this facade owns descriptor changes,
     * backing admission and completion needed during installation.
     */
    class KisPreparedMutationCommit;

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
    using TransactionMap = std::map<quint64, std::shared_ptr<KisPagePublicationDelta>,
        std::less<quint64>, KisMutationStorageAllocator<std::pair<const quint64, std::shared_ptr<KisPagePublicationDelta>>>>;
    TransactionMap m_preparedTransactions;
    // Immutable Tiles3 surface policy, not a second extent or page registry.
    KisSurfaceId m_derivedExtentSurface;
    using DefaultRevisionMap = std::map<quint64, quint64, std::less<quint64>,
        KisMutationStorageAllocator<std::pair<const quint64, quint64>>>;
    DefaultRevisionMap m_defaultRevisionHighWater;
    quint64 m_descriptorRevision = 1;
    KisPageStorePublicationStatistics m_statistics;
    quint64 m_committedTransactions = 0;
    // Non-owning: the original ledger keeps each record. Its intrusive hook
    // is unlinked before the retirement queue obtains ownership.
    boost::intrusive::list<KisPageRetirementRecord> m_idleCapacity;
    static constexpr qint64 IdleCapacityAgeMs = 100;
    void acceptCapacityLocked(const KisReplicaHandle &replica, KisPageRetirementRecord *record);
    quint64 idleAllowanceLocked() const;

    quint64 currentDefaultRevisionLocked(KisSurfaceId surface) const;
    bool reserveDefaultRevisionLocked(KisSurfaceId surface, quint64 revision, QString *error);
    void processRetirementsUnlocked(QMutexLocker<QMutex> &ownerLock);
};

#endif // KIS_PAGE_PUBLICATION_COORDINATOR_P_H

/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_METADATA_COORDINATOR_H
#define KIS_PAGE_METADATA_COORDINATOR_H

#include <QMutex>
#include <QSharedPointer>
#include <QString>
#include <QVector>
#include <memory>
#include <array>
#include <atomic>

#include "KisMutationStorage_p.h"

#include "KisCompletionRegistry.h"
#include "KisPageStateMachine.h"

class KisCpuReadBindingLink;
struct DeferredMetadataCleanupStatistics;
class KisPageHistoryCollector;
class KisPageOwnerLedger;
class KisBackingBudgetController;
class KisPageReplicaProvider;
class KisPagePublicationCoordinator;
class KisPageReadCoordinator;
class KisPageMetadataReadCleanup;
class KisPageRetirementQueue;

struct KRITAIMAGE_EXPORT KisPageMetadataTransitionResult
{
    bool accepted = false;
    bool storageRefused = false;
    QString rejectionReason;
};
class KisPageStore;

struct KRITAIMAGE_EXPORT KisPageMetadataShardMetrics {
    // Owner-only local projection work, not timing or a unique page count.
    // Whole snapshot/commit/GC work is deliberately not hidden in these.
    quint64 localTransitionSequences = 0;
    quint64 localVersionInputs = 0;
    quint64 localVersionInstalls = 0;
    quint64 localVersionRemovals = 0;
    // Direct owner cleanup reuses/removes existing overflow slots. These
    // transitions count in localTransitionSequences, but project/install no
    // version snapshots. Visits include validation and unlink passes.
    quint64 readProtectionTransitions = 0;
    quint64 readProtectionNodeVisits = 0;
    quint64 readProtectionSlotReuses = 0;
    quint64 readProtectionSlotReleases = 0;
    quint64 mutationBaseVersionInputs = 0;
    // Reclamation-thread subsets of the local totals; never add them twice.
    quint64 backgroundLocalTransitionSequences = 0;
    quint64 backgroundLocalVersionInputs = 0;
    quint64 backgroundLocalVersionInstalls = 0;
    quint64 backgroundLocalVersionRemovals = 0;
    quint64 backgroundMutationBaseVersionInputs = 0;
    // Root candidate records and installed publication deltas (not time,
    // unique identities or total commit work). Failed/retried candidates count.
    quint64 publicationVersionInputs = 0;
    quint64 publicationVersionInstalls = 0;
    quint64 publicationLookupVersionInputs = 0;
    quint64 publicationDirectoryHeaders = 0;
    quint64 historySliceQueries = 0;
    quint64 historySliceVersionInputs = 0;
    quint64 maximumHistorySliceVersionInputs = 0;
    // Explicit whole-page snapshot work at the diagnostic/reference boundary.
    // Background is a subset, not additional work; local counters cannot
    // stand in for this debt.
    mutable quint64 fullSnapshotExports = 0;
    mutable quint64 fullSnapshotVersionInputs = 0;
    mutable quint64 backgroundSnapshotExports = 0;
    mutable quint64 backgroundSnapshotVersionInputs = 0;
    quint64 publicationHistoryNodesTransferred = 0;
    quint64 publicationAdditionRecordsTransferred = 0;
    // M2 block payload causality. A candidate is one successful lock-external
    // system allocation; only an attached candidate becomes shard capacity.
    // Conflicts are page-revision failures after relock, not semantic rejects.
    quint64 metadataArenaGrowthBatches = 0;
    quint64 metadataArenaBlockCandidatesPrepared = 0;
    quint64 metadataArenaBlocksAttached = 0;
    quint64 metadataArenaGrowthConflicts = 0;
    quint64 metadataArenaGrowthFailures = 0;
};

// Global preparation/decision counters and shard-folded installation/work
// counters have one declaration each; the public snapshot exposes both.
struct KRITAIMAGE_EXPORT KisPageMetadataMetrics : KisPageMetadataShardMetrics {
    quint64 acceptedTransitions = 0;
    quint64 rejectedTransitions = 0;
    // Prepared publications are measured separately from local owner
    // transitions.
    quint64 preparedPublicationPages = 0;
    quint64 installedPublicationPages = 0;
    quint64 rejectedPublicationInstalls = 0;
    quint64 preparedMutationPages = 0;
    quint64 installedMutationPages = 0;
    quint64 rejectedMutationInstalls = 0;
    // Candidate work includes failed/retried preparation; transferred work is shard-local.
    quint64 publicationHistoryNodesPrepared = 0;
    quint64 publicationAdditionRecordsPrepared = 0;
    quint64 cpuReadBindings = 0;
};

struct KRITAIMAGE_EXPORT KisPageMetadataArenaStatistics {
    quint64 directoryEntries = 0;
    quint64 activeBlocks = 0;
    quint64 attachedBlocks = 0;
    quint64 allocatedBytes = 0;
    quint64 usedSlots = 0;
    quint64 freeSlots = 0;
    quint64 highWaterSlots = 0;
    quint64 releasedBytes = 0;
    quint64 quarantinedSlots = 0;
    quint64 quarantinedBlocks = 0;
    quint64 rejectedBlockAttaches = 0;
    quint64 outstandingReservations = 0;
    quint64 reservationBatches = 0;
    quint64 rejectedReservations = 0;

    KisPageMetadataArenaStatistics &operator+=(const KisPageMetadataArenaStatistics &rhs)
    {
        directoryEntries += rhs.directoryEntries; activeBlocks += rhs.activeBlocks;
        attachedBlocks += rhs.attachedBlocks; allocatedBytes += rhs.allocatedBytes;
        usedSlots += rhs.usedSlots; freeSlots += rhs.freeSlots; highWaterSlots += rhs.highWaterSlots;
        releasedBytes += rhs.releasedBytes; quarantinedSlots += rhs.quarantinedSlots;
        quarantinedBlocks += rhs.quarantinedBlocks; rejectedBlockAttaches += rhs.rejectedBlockAttaches;
        outstandingReservations += rhs.outstandingReservations; reservationBatches += rhs.reservationBatches;
        rejectedReservations += rhs.rejectedReservations;
        return *this;
    }
};

struct KRITAIMAGE_EXPORT KisPageMetadataIndexStatistics {
    quint64 entries = 0;
    quint64 capacity = 0;
    quint64 outstandingReservations = 0;
    quint64 highWaterEntries = 0;
    quint64 reservationBatches = 0;
    quint64 rejectedReservations = 0;

    KisPageMetadataIndexStatistics &operator+=(const KisPageMetadataIndexStatistics &rhs)
    {
        entries += rhs.entries; capacity += rhs.capacity;
        outstandingReservations += rhs.outstandingReservations; highWaterEntries += rhs.highWaterEntries;
        reservationBatches += rhs.reservationBatches; rejectedReservations += rhs.rejectedReservations;
        return *this;
    }
};

/**
 * Deterministic payload accounting for the coordinator-owned logical state.
 * This intentionally excludes allocator/hash-table implementation overhead;
 * callers can compare the same ABI across builds without mistaking RSS noise
 * for per-page metadata growth.
 */
struct KRITAIMAGE_EXPORT KisPageMetadataFootprint {
    quint64 pages = 0;
    quint64 pageActivities = 0;
    quint64 metadataPageBytes = 0;
    quint64 metadataPageActivityBytes = 0;
    quint64 versions = 0;
    quint64 replicas = 0;
    quint64 readLeases = 0;
    quint64 pendingLastUses = 0;
    quint64 trackedPayloadBytes = 0;
    // Shard object payload plus retained hash/index capacity charge. Actual
    // directory/shared-control allocation requests appear in backing usage.
    quint64 owningCapacityBytes = 0;
    KisPageMetadataArenaStatistics versionArena;
    KisPageMetadataArenaStatistics replicaArena;
    KisPageMetadataArenaStatistics overflowArena;
    KisPageMetadataIndexStatistics exactVersionIndex;
    KisPageMetadataIndexStatistics physicalSlotIndex;

    double trackedBytesPerPage() const
    {
        return pages ? double(trackedPayloadBytes) / double(pages) : 0.0;
    }
};

/**
 * Sharded logical metadata owner for the BR1 reference/production boundary.
 * Provider work and request/completion routing belong to the store owners.
 * This coordinator owns metadata transitions and prepared atomic installation;
 * it never calls a provider or maintains a second request scheduler.
 */
class KRITAIMAGE_EXPORT KisPageMetadataCoordinator
{
public:
    KisPageMetadataCoordinator();
    ~KisPageMetadataCoordinator();

    bool configure(qsizetype shardCount, QString *error = nullptr);
    bool isOperational() const;
    qsizetype shardCount() const;
    qsizetype shardFor(const KisPageKey &key) const;

    bool registerPage(const KisPageStateSnapshot &initial, QString *error = nullptr);
    bool pageSnapshot(const KisPageKey &key, KisPageStateSnapshot *snapshot) const;
    KisPageSnapshotArray<KisPageKey> pageKeys() const;
    KisPageSnapshotArray<KisReplicaHandle> shutdownReplicaHandles() const;
    KisPageMetadataTransitionResult acknowledgeLastUse(const KisPageVersion &version,
                                               const KisReplicaHandle &replica,
                                               const KisVerifiedCompletion &completion);
    qsizetype pageCount() const;
    KisPageMetadataMetrics metrics() const;
    KisPageMetadataFootprint footprint() const;

private:
    using PrepareRetirementDebt = bool (*)(void *,
                                           const KisPageTransitionEffect *, qsizetype,
                                           quint64 *,
                                           QString *);
    using CommitRetirementEffects = void (*)(void *, quint64,
                                             const KisPageTransitionEffect *, qsizetype) noexcept;
    using FinalizeRetirementDebt = void (*)(void *, quint64) noexcept;
    void attachBackingBudget(KisBackingBudgetController &budget);
    void attachRetirementDebtOwner(void *context,
                                   PrepareRetirementDebt prepare,
                                   CommitRetirementEffects commit,
                                   FinalizeRetirementDebt cancel);
    // Production-only bounded input and transition result. Neither API exports
    // the complete history or transfer effects to callers. The same
    // state-machine guards run on touched records plus indexed global collision
    // witnesses. Unclassified transitions retain the full reference path.
    // Omitted history is NOT absence of logical/physical owners. These bounded
    // projections cannot establish an in-place permit or root reachability.
    // Original exact query: scalar facts only. Generic read selection supplies
    // its funded candidate array; leases/tokens are never exported here.
    struct VersionInfo {
        KisPageVersion version;
        KisPageGeneration publishedGeneration;
        KisPagePublicationState publication = KisPagePublicationState::Unpublished;
        KisPageTransactionId preparedBy;
        KisReplicaHandle authority;
        quint32 replicaCount = 0;
        bool captured = false;
        bool isVirtualDefault() const {
            return version.isDefaultPixel() && !replicaCount && !authority.isValid()
                && (publication == KisPagePublicationState::Published
                    || publication == KisPagePublicationState::Historical);
        }
        bool needsDefaultMaterialization(const KisPageVersion &identity) const {
            return identity.isDefaultPixel() && (!version.isValid() || isVirtualDefault());
        }
    };
    struct ReplicaCandidate {
        KisReplicaHandle replica;
        KisReplicaValidity validity;
    };
    using ReplicaCandidates = std::vector<ReplicaCandidate, KisMutationStorageAllocator<ReplicaCandidate>>;
    bool versionSnapshot(const KisPageVersion &version, VersionInfo *snapshot,
                         ReplicaCandidates *replicas = nullptr) const;
    struct MutationBaseInfo {
        bool baseExists = false;
        bool hasWriter = false;
        KisPageGeneration nextGeneration;
        VersionInfo selected;
        KisReplicaHandle recoverableBefore;
    };
    // One shard cut for the root base, sealed selection and Fresh candidate.
    // This is a scalar observation; joint write preparation still revalidates
    // eligibility and acquires the physical claim before touching any bytes.
    bool queryMutationBase(const KisPageVersion &base, const KisPageVersion &sealed,
                           bool discoverBefore, MutationBaseInfo *info) const;
    // Cold exact query for original read-release records. Completion may clear
    // multiple leases carrying the same ticket; this never grants write access.
    bool queryLastUsePending(const KisReplicaHandle &replica,
                             const KisCompletionTicket &completion, bool *pending) const;
    // Admission guard only: exact target lookup plus mutable count and
    // Retiring witnesses. No whole-page version projection escapes the shard.
    bool canAddTransientVersion(const KisPageVersion &target, quint32 limit) const;
    struct PublicationInfo {
        VersionInfo current;
        VersionInfo target;
    };
    // Current head and optional exact target share one shard cut. No replica
    // or protection arrays escape this observation; preparation revalidates.
    bool queryPublication(const KisPageKey &key, const KisPageVersion &target,
                          PublicationInfo *info) const;
    using PublicationHeads = std::vector<KisPageVersion, KisMutationStorageAllocator<KisPageVersion>>;
    // Paid directory identities, not a coherent epoch view. The owner's
    // registration revision and per-page candidate revisions still apply.
    PublicationHeads publicationHeads() const;
    struct HistorySlice {
        static constexpr qsizetype Limit = 32;
        std::array<KisPageVersion, Limit> versions{};
        qsizetype count = 0;
        KisPageVersion after;
        qsizetype total = 0;
        bool exists = false;
    };
    // Value cursor, not a borrowed iterator. Removal cannot dangle it. A
    // caller must rescan after semantic changes can insert before the cursor;
    // this is enumeration, NOT root reachability or retirement authorization.
    HistorySlice historySlice(const KisPageKey &key, const KisPageVersion &after, qsizetype budget) const;
    // Select eligible, unreachable records and prepare their exact effects and
    // Debt before removal. Replica effects require a configured Debt owner and
    // their original registered retirement records. Refusal changes no record;
    // success commits Debt and transfers effects to the configured receiver
    // infallibly under the caller's owner gate, without invoking a provider.
    bool discardHistory(const KisPageKey &key, const KisPageVersion *versions,
                        qsizetype count, quint32 reachableMask,
                        quint32 *removedMask);
    void visitPageKeys(void *context, void (*visit)(void *, const KisPageKey &)) const;
    KisPageMetadataTransitionResult applyOwner(const KisPageKey &key, const KisPageTransition &transition,
                                      KisPageMetadataReadCleanup *cleanup = nullptr);
    KisPageMetadataTransitionResult acknowledgeLastUse(const KisPageVersion &version,
                                               const KisReplicaHandle &replica,
                                               const KisVerifiedCompletion &completion,
                                               KisPageMetadataReadCleanup *cleanup);
    KisPageMetadataTransitionResult applyOwnerSequence(const KisPageKey &key, const KisPageSnapshotArray<KisPageTransition> &transitions);
    // Directory invalidation, not a read-side counter. PageStore samples it
    // under its registration gate around a whole-directory default/restore
    // plan; per-page revision claims alone cannot detect a newly added key.
    quint64 pageRegistrationCount() const;
    std::shared_ptr<KisCpuReadBindingLink> installCpuReadBinding(const KisReplicaHandle &replica,
                                                                const std::shared_ptr<KisPageReplicaProvider> &provider);
    // A stale reader may only evict its own candidate. Retirement omits
    // expected to invalidate every cached selection of this exact replica.
    void removeCpuReadBinding(const KisReplicaHandle &replica,
                              const KisCpuReadBindingLink *expected = nullptr);
    std::shared_ptr<KisCpuReadBindingLink> cpuReadBinding(const KisPageVersion &version) const;
    KisReplicaHandle cpuReadReplica(const KisPageVersion &version) const;
    friend class KisCapturedReadView;
    friend class KisPageHistoryCollector;
    friend class KisPagePublicationCoordinator;
    friend class KisPageReadCoordinator;
    friend class KisPageRetirementQueue;
    // PageStore-only, synchronous publication capability. It must never be
    // exposed as a caller-supplied "validated" snapshot. Preparation does not
    // lock out readers; installation claims only the changed pages, checks all
    // revisions (root publication) or exact detachment semantics (mutation)
    // before changing any state, and holds no two shard locks.
    // Ordinary commit/restore uses indexed records + publication-only deltas.
    // Candidates own the semantic delta plus arena-slot, exact-index,
    // physical-index and sparse-activity reservations needed by metadata
    // installation. Installation consumes those capabilities only after all
    // page claims succeed. Descriptor/proof/root preparation remains separate,
    // so this is not yet the whole-commit capability described by BR1 M4.
    class DeferredPublicationCleanup;
    class KRITAIMAGE_EXPORT PreparedPublication final
    {
    public:
        PreparedPublication();
        ~PreparedPublication();
        PreparedPublication(PreparedPublication &&) noexcept;
        PreparedPublication &operator=(PreparedPublication &&) noexcept;
        PreparedPublication(const PreparedPublication &) = delete;
        PreparedPublication &operator=(const PreparedPublication &) = delete;
        bool isValid() const;
        // Outcome only, never an install capability. The original owner can
        // retry a revision race or retain its input until capacity returns.
        bool needsReprepare() const { return m_failure == Failure::Conflict; }
        bool wasStorageRefused() const { return m_failure == Failure::Storage; }
        KisReplicaHandle backingAuthority(const KisPageVersion &version) const;

    private:
        class Data;
        struct DataDeleter {
            void operator()(Data *value) const noexcept;
        };
        using DataPointer = std::unique_ptr<Data, DataDeleter>;
        DataPointer data;
        enum class Failure : quint8 { None, Conflict, Storage };
        Failure m_failure = Failure::None;
        friend class KisPageMetadataCoordinator;
        friend class DeferredPublicationCleanup;
    };
    // Consumed candidate storage. PageStore moves it across the owner gate and calls
    // clearBatch() only after unlocking. It owns no live coordinator access.
    class KRITAIMAGE_EXPORT DeferredPublicationCleanup
    {
    public:
        DeferredPublicationCleanup();
        ~DeferredPublicationCleanup();
        DeferredPublicationCleanup(DeferredPublicationCleanup &&) noexcept;
        DeferredPublicationCleanup &operator=(DeferredPublicationCleanup &&) noexcept;
        DeferredPublicationCleanup(const DeferredPublicationCleanup &) = delete;
        DeferredPublicationCleanup &operator=(const DeferredPublicationCleanup &) = delete;
        bool isEmpty() const;
        // One unit is one candidate page (or one empty-candidate terminal
        // payload). Actual candidate capacity remains charged until freed.
        // This is a cooperative destruction budget,
        // not a byte or hard wall-time guarantee for one unit.
        qsizetype pendingWorkUnits() const;
        qsizetype clearBatch(qsizetype maximumWorkUnits);
        void dispose(const std::shared_ptr<DeferredMetadataCleanupStatistics> &statistics);

    private:
        PreparedPublication::DataPointer data;
        friend class KisPageMetadataCoordinator;
    };
    PreparedPublication preparePublication(const KisPageTransaction &transaction,
                                           KisImageEpochId minimumEpoch,
                                           const KisPageSnapshotArray<KisPageTransition> &transitions,
                                           QString *error = nullptr) const;
    PreparedPublication prepareRestoration(KisImageEpochId minimumEpoch,
                                           const KisPageSnapshotArray<KisPageTransition> &transitions,
                                           QString *error = nullptr) const;
    struct PublicationChange {
        KisPageTransitionKind kind;
        KisPageVersion version;
        KisReplicaHandle target;
    };
    PreparedPublication preparePublication(const KisPageTransaction &transaction,
                                           KisImageEpochId minimumEpoch,
                                           const PublicationChange *changes, qsizetype count,
                                           QString *error = nullptr) const;
    PreparedPublication prepareRestoration(KisImageEpochId minimumEpoch,
                                           const KisPageVersion *versions, qsizetype count,
                                           QString *error = nullptr) const;
    enum class PreparationKind : quint8 { Publication, Detachment, RecoverableWrite };
    PreparedPublication preparePublicationImpl(const KisPageTransaction &transaction,
                                               KisImageEpochId minimumEpoch,
                                               const KisPageTransition *transitions, qsizetype count,
                                               bool restoration,
                                               QString *error,
                                               PreparationKind kind = PreparationKind::Publication,
                                               const KisPageVersion *versions = nullptr,
                                               const PublicationChange *changes = nullptr) const;
    // Transaction-overlay detachment is a compact semantic delta. Install
    // claims all pages, revalidates exact Prepared identity/transaction and no
    // writer, then changes only publication/preparedBy in CURRENT metadata.
    // Reader/pin/last-use changes are preserved, not overwritten or retried.
    // The committed epoch tag is unchanged. This is not a stale root merge.
    // Versions are the complete private input; other transition kinds,
    // operation IDs and epoch tags cannot enter this capability.
    PreparedPublication prepareMutation(const KisPageTransaction &transaction,
                                        const KisPageVersion *versions, qsizetype count,
                                        QString *error = nullptr) const;
    bool installMutation(PreparedPublication &&prepared,
                         const KisPageTransaction &transaction,
                         QString *error = nullptr,
                         DeferredPublicationCleanup *deferredCleanup = nullptr);
    // Logical half only. The write coordinator must independently retain the
    // exact before, prepare provider retag/descriptor/budget/terminal storage,
    // and acquire physical claims before installation. Rejection consumes the
    // candidate without changing either version. Success installs the already
    // prepared writer, so no fallible PrepareWrite remains after retag;
    // no bytes may be exposed until provider retag has also been consumed.
    PreparedPublication prepareRecoverableWrite(const KisPageTransaction &transaction,
                                                const KisPageTransition &transition,
                                                QString *error = nullptr) const;
    bool installRecoverableWrite(PreparedPublication &&prepared,
                                 const KisPageTransaction &transaction,
                                 QString *error = nullptr,
                                 DeferredPublicationCleanup *deferredCleanup = nullptr);
    // Publication keeps superseded replicas in history; retirement effects
    // are produced later by history GC or an explicit abort/discard.
    bool installPublication(PreparedPublication &&prepared,
                            const KisPageTransaction &transaction,
                            KisImageEpochId epoch,
                            QString *error = nullptr,
                            DeferredPublicationCleanup *deferredCleanup = nullptr);
    bool installPublicationImpl(PreparedPublication &&prepared,
                                const KisPageTransaction &transaction,
                                KisImageEpochId epoch,
                                QString *error,
                                PreparationKind kind,
                                DeferredPublicationCleanup *deferredCleanup);

    friend class KisPageStore;
    friend class KisPageWriteCoordinator;
    friend class KisPageMutationSession;
    friend class KisPageOwnerLedger;
    friend class KisPagePublicationCoordinator;
    friend class KisPageStoreReferenceTest;
    friend class KisPageStoreCpuMutationTest;
    friend class KisPageStoreResidentReadTest;
    KisPageMetadataTransitionResult applyProjectedSequence(const KisPageKey &key,
                                                   const KisPageSnapshotArray<KisPageTransition> &transitions);
    KisPageMetadataTransitionResult applyReadProtection(const KisPageKey &key,
                                               const KisPageTransition &transition,
                                               KisPageMetadataReadCleanup *cleanup = nullptr);
    KisPageMetadataTransitionResult applyCapturedProtection(const KisPageKey &key,
                                                   const KisPageTransition &transition,
                                                   KisPageMetadataReadCleanup *cleanup = nullptr);
    class Private;
    // configure() publishes the complete immutable core once. Destruction
    // still requires owner quiescence; acquire does not extend its lifetime.
    std::atomic<Private *> m_core{nullptr};
    mutable QMutex m_configurationMutex;
    KisBackingBudgetController *m_budget = nullptr;
    void *m_retirementDebtContext = nullptr;
    PrepareRetirementDebt m_prepareRetirementDebt = nullptr;
    CommitRetirementEffects m_commitRetirementDebt = nullptr;
    FinalizeRetirementDebt m_cancelRetirementDebt = nullptr;
};

#endif // KIS_PAGE_METADATA_COORDINATOR_H

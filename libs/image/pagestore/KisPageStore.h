/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_STORE_H
#define KIS_PAGE_STORE_H

#include <QScopedPointer>
#include <QSet>
#include <QSharedPointer>

#include <limits>
#include <memory>

#include "KisCompletionRegistry.h"
#include "KisPageReplicaProvider.h"
#include "KisPageStoreTypes.h"
#include "KisMutationStorage_p.h"

class KisPageStoreMementoManager;
class KisBackingBudgetController;
class KisCpuReadGuard;
class KisCpuResidentBinding;
class KisCpuDefaultReadBuffer;
class KisCpuWriteGuard;
class KisCapturedReadView;
class KisTiles3PageReplicaProvider;
class KisTiledDataManagerPageStoreBackend;
class KisTiledDataManagerPageStoreWriteBatch;
class KisPageStoreWriteReservation;
class QWriteLocker;
struct KisPageMetadataMetrics;
struct KisPageBackingLimits;
struct KisPageBackingUsage;

struct KRITAIMAGE_EXPORT KisPageMutationStatistics {
    quint64 sessionsCreated = 0;
    quint64 generationsReserved = 0;
    quint64 guardsReleased = 0;
    quint64 pagesSealed = 0;
    quint64 pagesCancelled = 0;
    // Folded at segment seal/cancel, not atomics in the guard hot path.
    // Counts successful guard pins; provider preparation/failure is separate.
    quint64 writablePinsAcquired = 0;
    quint64 writablePinsReleased = 0;
    quint64 pendingWriteMaterializations = 0;
    // Exact high-water within one execution (or a direct serial session).
    // Concurrent executions are not summed into a purported segment peak.
    quint64 maximumPinnedPagesPerExecution = 0;
    // Canonical operation scopes may be semantic-only, without a CPU provider.
    // sessionsCreated above counts only scopes whose first pixel operation
    // selected a native CPU provider.
    quint64 operationSessionsCreated = 0;
    quint64 removalsSealed = 0;
    quint64 removalsCancelled = 0;
    quint64 pendingDefaultResetBytes = 0;
    // New generations prepared solely by final alias (no native first write).
    quint64 aliasGenerationsReserved = 0;
    // Seal is an overlay boundary, not a root commit. Compact detachment
    // candidates preserve current reader/pin/last-use facts without retries.
    quint64 sealMetadataPreparations = 0;
    quint64 sealMetadataRejections = 0;
    // Full payload copies into an existing pending target or a provider's
    // initialized fallback. Direct fresh initialization is provider work.
    quint64 pendingPayloadCopyBytes = 0;
};

struct KRITAIMAGE_EXPORT KisPageStoreRetirementProgress {
    qsizetype attempted = 0;
    qsizetype retired = 0;
    qsizetype pending = 0;
};

// Cold inspection, not per-read instrumentation. Retries are real repeated
// preparation work and must be included in operation/latency measurements.
struct KRITAIMAGE_EXPORT KisPageStorePublicationStatistics {
    quint64 preparedMutationCommits = 0;
    quint64 installedMutationCommits = 0;
    quint64 cancelledMutationCommits = 0;
    quint64 rootRebases = 0;
    quint64 metadataRepreparations = 0;
    quint64 directoryRepreparations = 0;
    quint64 descriptorRepreparations = 0;
    quint64 descriptorCapacityPreparations = 0;
    quint64 descriptorInstallations = 0;
    quint64 restoreMetadataRepreparations = 0;
    quint64 restoreDirectoryRepreparations = 0;
    // Deferred cleanup work is counted in candidate-page units, not records,
    // bytes or allocator calls. The worker applies a page/pass cap and checks
    // its time budget between units; one unit is not a hard real-time bound.
    quint64 metadataCleanupDeferredCandidates = 0;
    quint64 metadataCleanupCompletedCandidates = 0;
    quint64 metadataCleanupPasses = 0;
    quint64 metadataCleanupUnits = 0;
    quint64 metadataCleanupMaximumUnitsPerPass = 0;
    quint64 metadataCleanupNanoseconds = 0;
    quint64 metadataCleanupMaximumPassNanoseconds = 0;
    quint64 metadataCleanupQueueNanoseconds = 0;
    quint64 metadataCleanupMaximumQueueNanoseconds = 0;
    qsizetype metadataCleanupPendingCandidates = 0;
    qsizetype metadataCleanupPeakPendingCandidates = 0;
    qsizetype metadataCleanupPendingUnits = 0;
    qsizetype metadataCleanupPeakPendingUnits = 0;
    qsizetype activeCommitPreparations = 0;
};

/** One execution's declared write range in the original mutation session.
 * A borrow is confined to the worker that acquired it; another worker obtains
 * a new borrow from the same session. Disjoint ranges may execute together.
 * finish() returns execution only, never seals the segment. Abandonment poisons
 * the original segment; guards keep the range and cancellation owner alive.
 * This capability does not grant operation rollback, scheduler ordering, a
 * read footprint, or commit-only permission.
 */
class KRITAIMAGE_EXPORT KisPageMutationExecution
{
public:
    KisPageMutationExecution() = default;
    ~KisPageMutationExecution();
    KisPageMutationExecution(KisPageMutationExecution &&) noexcept = default;
    KisPageMutationExecution &operator=(KisPageMutationExecution &&) noexcept;
    KisPageMutationExecution(const KisPageMutationExecution &) = delete;
    KisPageMutationExecution &operator=(const KisPageMutationExecution &) = delete;
    bool isActive() const;
    bool finish(QString *error = nullptr);
    KisCpuWriteGuard beginWrite(const KisPageKey &, QString *error = nullptr);
    KisCpuWriteGuard beginWrite(const KisPageKey &, KisPageWriteMode, QString *error = nullptr);
    bool overwritePage(const KisPageKey &, const KisCpuPagePayload &, QString *error = nullptr);
private:
    enum class PreparationResult : quint8 { Unsupported, Failed, Ready };
    // Prepare ordinary mutable backing for the complete borrowed range before
    // a caller can expose pixels. Semantic removal/alias entries keep their
    // existing path because preparing them would itself change pending bytes.
    PreparationResult prepareWrites(QString *error);
    void abandon();
    class Private;
    std::shared_ptr<Private> d;
    friend class KisPageMutationSession;
    friend class KisTiledDataManagerPageStoreBackend;
    friend class KisPageStoreCpuMutationTest;
    // Adapter-only result export into unique storage prepared before pixels.
    // A null output queries the count; -1 rejects an unavailable execution or
    // insufficient/shared output. No new page keys or permissions are stored.
    qsizetype finishPreparedWrites(QVector<KisLogicalPageId> *changed,
                                   QString *error = nullptr);
    friend class KisCpuWriteGuard;
};

/** Owner-issued unpublished canonical mutation segment.
 * Joins an existing transaction; seal prepares its pages, not the epoch root.
 * The first write prepares independent backing (preserve COW or default/discard
 * initialization). A virtual default does not allocate a source. Repeated accesses reuse
 * that pending generation. No generic request/access-lease maps are used.
 * The segment retains a page-local writer reservation until seal/cancel;
 * physical RAM pins live only as long as writable guards. Parked pending
 * backing may swap; reaccess restores it only after a real NonResident result.
 * Private metadata/proof and detachment preparation run outside owner under
 * transaction/key claims. Overlay install claims all pages and revalidates
 * exact detachment semantics, preserving current read protections. Failure
 * cancels this segment, never earlier sealed work.
 * This is not an in-place permit or a completed short-publication API.
 */
class KRITAIMAGE_EXPORT KisPageMutationSession
{
public:
    KisPageMutationSession() = default;
    ~KisPageMutationSession();
    KisPageMutationSession(KisPageMutationSession &&) noexcept = default;
    KisPageMutationSession &operator=(KisPageMutationSession &&) noexcept = default;
    KisPageMutationSession(const KisPageMutationSession &) = delete;
    KisPageMutationSession &operator=(const KisPageMutationSession &) = delete;
    bool isActive() const;
    // Explicit write-only footprint. Shared input must already be immutable;
    // scheduler/read dependencies remain the caller's responsibility. Conflict
    // and storage rejection occur before execution and preserve older work.
    KisPageMutationExecution borrowExecution(KisSurfaceId,
        const QSet<KisLogicalPageId> &pages, QString *error = nullptr);
    // Private semantic absence. No pixel allocation, generation or overlay
    // publication occurs here. Repeated removal coalesces; a subsequent write
    // starts from the current surface default, reusing any pending backing.
    bool removePage(const KisPageKey &key, QString *error = nullptr);
    // Retain an exact immutable provider input privately. No target generation,
    // allocation or proof until seal/first pixel write; repeated aliases replace
    // the input. A following partial write initializes independent backing.
    bool aliasPage(const KisPageKey &key,
                   const QSharedPointer<const KisPageReplicaSource> &source,
                   QString *error = nullptr);
    KisCpuWriteGuard beginWrite(const KisPageKey &key, QString *error = nullptr);
    // Mode controls first preparation (including a staged alias's source
    // initialization). Otherwise repeated access reuses existing pending bytes,
    // even when the first access discarded.
    KisCpuWriteGuard beginWrite(const KisPageKey &key, KisPageWriteMode mode, QString *error = nullptr);
    // Complete page supplied now, not a promise to fill a raw pointer later.
    // Reuses same-segment pending backing; otherwise directly initializes an
    // independent target when supported. Input must remain immutable/readable
    // until return and must not alias the segment's pending writable target.
    // No implicit publication; ordinary segment cancel/abort remains valid.
    bool overwritePage(const KisPageKey &key, const KisCpuPagePayload &payload, QString *error = nullptr);
    // Outstanding guards reject seal/cancel; destroying the session with a
    // guard alive defers cancellation until that guard has relinquished data.
    bool seal(QString *error = nullptr);
    // Freeze every change in this session into the original transaction and
    // capture its complete sealed overlay. Outstanding executions/guards
    // reject without waiting or poisoning the session. On success the same
    // session remains open, with no page writer/pin carried across the cut.
    // The caller must first establish the scheduler's complete job cut; this
    // method cannot admit/drain queued jobs, coordinate other sessions/stores,
    // or provide operation rollback across a checkpoint.
    KisCapturedReadView checkpointForRead(QString *error = nullptr);
    bool cancel();

private:
    // A shared legacy tile may perform its final unlock on a reader thread.
    // Only the bridge can hand off terminal sealing; no active guard or new
    // write access is permitted at this boundary.
    bool sealForLegacyUnlock(QString *error = nullptr);
    // Seal the original segment but retain its original admission/write set
    // until this session is destroyed after adapter delivery. No writable
    // access survives seal, and the transaction activity still ends normally.
    bool sealForAdapterDelivery(QString *error = nullptr);
    bool sealImpl(QString *error, bool legacyFinalUnlock, KisCapturedReadView *checkpoint = nullptr,
                  bool retainAdmission = false);
    // Adapter reservations claim their targets before capture or pixel
    // exposure; adopting them does not create pending generations.
    bool adoptReservation(std::unique_ptr<KisPageStoreWriteReservation> range,
                           QString *error);
    bool reserveLegacyMutationPage(const KisPageKey &key, QString *error);
    KisCpuWriteGuard
    beginWriteImpl(const KisPageKey &key, KisPageWriteMode mode, const KisCpuPagePayload *payload, QString *error,
                   const std::shared_ptr<KisPageMutationExecution::Private> &execution = {},
                   bool preparationOnly = false);
    friend class KisTiledDataManagerPageStoreLease;
    friend class KisTiledDataManagerPageStoreBackend;
    friend class KisTiledDataManagerPageStoreWriteBatch;
    class Private;
    std::shared_ptr<Private> d;
    friend class KisPageStore;
    friend class KisCpuWriteGuard;
    friend class KisPageMutationExecution;
};

class KRITAIMAGE_EXPORT KisCpuWriteGuard
{
public:
    KisCpuWriteGuard() = default;
    ~KisCpuWriteGuard();
    KisCpuWriteGuard(KisCpuWriteGuard &&) noexcept = default;
    KisCpuWriteGuard &operator=(KisCpuWriteGuard &&) noexcept;
    KisCpuWriteGuard(const KisCpuWriteGuard &) = delete;
    KisCpuWriteGuard &operator=(const KisCpuWriteGuard &) = delete;
    bool isValid() const
    {
        return m_scope && m_data;
    }
    void *data() const
    {
        return isValid() ? m_data : nullptr;
    }
    KisPageVersion version() const;
    quint32 rowStride() const
    {
        return isValid() ? m_rowStride : 0;
    }
    quint64 byteSize() const
    {
        return isValid() ? m_byteSize : 0;
    }

private:
    void reset();
    bool providerBacking(KisReplicaHandle *) const;
    std::shared_ptr<KisPageMutationSession::Private> m_scope;
    KisPageMutationExecution::Private *m_execution = nullptr; // kept by aliasing m_scope
    // Borrow: immutable range position. Direct session: original slot handle.
    quint32 m_entry = std::numeric_limits<quint32>::max();
    quint64 m_entryIncarnation = 0;
    void *m_data = nullptr;
    quint32 m_rowStride = 0;
    quint64 m_byteSize = 0;
    friend class KisPageMutationSession;
    friend class KisTiles3PageReplicaProvider;
};

struct KRITAIMAGE_EXPORT KisPageStoreReadScopeStatistics {
    quint64 capturedReadViewsCreated = 0;
    quint64 capturedReadViewReleases = 0;
    quint64 historicalGcPagesVisited = 0;
    quint64 foregroundHistoricalGcPagesVisited = 0;
    quint64 maximumHistoryPagesPerPass = 0;
    // Worker enumeration, not total root-lookup/provider/GC time. One worker
    // pass selects <=16 keys and <=32 historical versions per key. Replica
    // lists and root-tree resolution have separate costs. Protected-root
    // enumeration across the entire pass is additionally capped at 32.
    quint64 maximumHistoryVersionsPerPass = 0;
    quint64 historyReachabilityRefreshes = 0;
    // Sum of protected-root visits across candidate-group refreshes; a root
    // may be visited again for another group or after invalidation.
    // Not retired registry size, tokens, tree-node visits, or time/total GC work.
    quint64 historyReachabilityRootsVisited = 0;
    quint64 maximumHistoryRootsPerPass = 0;
    quint64 historyReachabilityRestarts = 0;
};

/**
 * Move-only, owner-issued immutable read scope. Identity/surface lookup reads
 * the retained persistent root plus any frozen sealed delta directly, without
 * the PageStore mutex or an owning manifest export. This capability alone does
 * not grant pixel access. Overlay capture/release protects its sealed K pages;
 * lookup never follows the subsequently mutable transaction/head.
 * The retained owner/root lifetime survives destruction of the public facade.
 */
class KRITAIMAGE_EXPORT KisCapturedReadView
{
public:
    KisCapturedReadView() = default;
    ~KisCapturedReadView();
    KisCapturedReadView(KisCapturedReadView &&) noexcept = default;
    KisCapturedReadView &operator=(KisCapturedReadView &&) noexcept = default;
    KisCapturedReadView(const KisCapturedReadView &) = delete;
    KisCapturedReadView &operator=(const KisCapturedReadView &) = delete;

    bool isValid() const;
    // Overlay captures fix sealed private versions and staged metadata. epoch()
    // is their protected base epoch, not a newly published synthetic epoch.
    bool isTransactionOverlay() const;
    KisImageEpochId epoch() const;
    bool resolvePageVersion(const KisPageKey &key, KisPageVersion *version) const;
    bool resolveSurfaceState(KisSurfaceId surface, KisSurfaceEpochState *state) const;
    // A try only: local busy is distinct from nonresident/unsupported control.
    // Root retention and the returned guard jointly protect version + pixels.
    KisCpuReadGuard tryReadResidentPage(const KisPageKey &key, KisCpuResidentReadStatus *status = nullptr) const;
    // May wait for the page-local mutex/swap barrier, then rechecks residency.
    // Never initiates I/O or waits for a writer capability held by the caller.
    KisCpuReadGuard readResidentPage(const KisPageKey &key, KisCpuResidentReadStatus *status = nullptr) const;

private:
    KisCpuReadGuard
    readResidentPageImpl(const KisPageKey &key, KisCpuResidentReadStatus *status, bool waitForLocalGate) const;
    class Private;
    std::shared_ptr<Private> d;
    friend class KisPageStore;
    friend class KisCpuReadGuard;
};

class KRITAIMAGE_EXPORT KisCpuReadGuard
{
public:
    KisCpuReadGuard() = default;
    ~KisCpuReadGuard();
    KisCpuReadGuard(KisCpuReadGuard &&) noexcept = default;
    KisCpuReadGuard &operator=(KisCpuReadGuard &&) noexcept;
    KisCpuReadGuard(const KisCpuReadGuard &) = delete;
    KisCpuReadGuard &operator=(const KisCpuReadGuard &) = delete;
    bool isValid() const
    {
        return m_scope && m_data;
    }
    const void *data() const
    {
        return isValid() ? m_data : nullptr;
    }
    KisPageVersion version() const;
    quint32 rowStride() const;
    quint64 byteSize() const;

private:
    void reset();
    std::shared_ptr<KisCapturedReadView::Private> m_scope;
    std::shared_ptr<KisCpuResidentBinding> m_binding;
    const void *m_data = nullptr;
    QSharedPointer<const KisCpuDefaultReadBuffer> m_defaultBuffer;
    KisPageVersion m_version;
    QSize m_pageExtent;
    quint32 m_rowStride = 0;
    quint64 m_byteSize = 0;
    friend class KisCapturedReadView;
    friend class KisTiles3PageReplicaProvider;
};

struct KRITAIMAGE_EXPORT KisPageArchiveCompletionEvent {
    KisPageOperationId operation;
    KisPageVersion version;
    KisCompletionTicket completion;
    KisCompletionStatus status = KisCompletionStatus::Unknown;
    QString error;

    bool isValid() const
    {
        return operation.isValid() && version.isValid() && completion.isValid()
            && status != KisCompletionStatus::Unknown && status != KisCompletionStatus::Pending;
    }

    bool succeeded() const
    {
        return status == KisCompletionStatus::Succeeded;
    }
};

/**
 * Canonical owner of page generation, leases, publication and authority.
 *
 * BR1 starts from page-granular request/lease primitives. Multi-page atomicity
 * belongs to KisPageTransaction; a batch request is deliberately not allowed
 * to return one ambiguous lease spanning several independently versioned pages.
 *
 * The BR1 CPU facade becomes operational only after an exact initial epoch
 * manifest has been adopted and validated against registered providers.
 * Legacy tiles3 accessors may remain as ABI adapters, but their pixel access
 * must be backed by PageStore leases or owner-issued native guards once the
 * production bridge is enabled.
 */
class KRITAIMAGE_EXPORT KisPageStore
{
public:
    KisPageStore();
    ~KisPageStore();
    KisPageStore(const KisPageStore &) = delete;
    KisPageStore &operator=(const KisPageStore &) = delete;
    KisPageStore(KisPageStore &&) = delete;
    KisPageStore &operator=(KisPageStore &&) = delete;

    bool isOperational() const;
    KisPageStoreSessionStats sessionStats() const;
    KisPageStoreReadScopeStatistics readScopeStatistics() const;
    KisPageStorePublicationStatistics publicationStatistics() const;
    KisPageMutationStatistics mutationStatistics() const;
    // Pure snapshot of the last successfully admitted ledger state. Provider
    // domain changes are synchronized at fallible admission/publication gates,
    // never by this diagnostic observer.
    KisPageBackingUsage backingUsage() const;
    bool configureBackingLimits(const KisPageBackingLimits &limits,
                                QString *error = nullptr);
    bool closeSession(QString *error = nullptr);
    // Retry detached physical replicas, at most replicaBudget attempts. A
    // rejected retire keeps the exact identity/provider/last-use claim. This
    // does not scan roots/history or grant early reclamation permission.
    KisPageStoreRetirementProgress processRetirements(qsizetype replicaBudget);
    // Explicit non-interactive endpoint: wait for scheduled history and first-
    // retirement passes. Failed/pinned records remain debt; this is not a force-release or
    // an unbounded retry loop. Call with quiescent producers for a full drain.
    bool waitForRetirementIdle();

    bool configure(const KisImageEpochSnapshot &initialEpoch,
                   const QSharedPointer<KisCompletionRegistry> &completions,
                   qsizetype metadataShardCount,
                   QString *error = nullptr);

    bool registerReplicaProvider(const QSharedPointer<KisPageReplicaProvider> &provider);
    bool registerTransferBridge(const QSharedPointer<KisPageReplicaTransferBridge> &bridge);
    bool registerExactGenerationArchive(const QSharedPointer<KisExactGenerationArchive> &archive);
    bool adoptInitialPage(const KisPageVersion &version,
                          const KisPageAllocationDescriptor &descriptor,
                          const KisReplicaHandle &authority,
                          QString *error = nullptr);
    // Import already initialized exact replicas with the initial manifest.
    // The caller supplies the same canonical bytes for each handle, as for
    // authority adoption. This never creates/copies a recovery replica while
    // selecting a write. Validation and budget adoption are all-or-nothing.
    bool adoptInitialPage(const KisPageVersion &version,
                          const KisPageAllocationDescriptor &descriptor,
                          const KisReplicaHandle &authority,
                          const QVector<KisReplicaHandle> &readyAlternates,
                          QString *error = nullptr);
    bool adoptInitialPageBytes(const KisPageVersion &version,
                               const KisPageAllocationDescriptor &descriptor,
                               const QByteArray &canonicalBytes,
                               QString *error = nullptr);
    bool finalizeInitialization(QString *error = nullptr);
    bool resolveSurfaceState(KisSurfaceId surface, const KisPageReadView &view, KisSurfaceEpochState *state) const;
    /** Resolves identity only; it neither materializes bytes nor grants access. */
    bool resolvePageVersion(const KisPageKey &key, const KisPageReadView &view, KisPageVersion *version) const;
    // Ephemeral classification of a declared batch at one metadata boundary.
    // No retained view, pixels, read protection or access permission. Output is
    // replaced only on complete success; unsealed mutation bytes are excluded.
    bool resolvePagePresence(KisSurfaceId, const QVector<KisLogicalPageId> &,
                             const KisPageReadView &, QVector<quint8> *present) const;
    bool pageDescriptor(const KisPageVersion &version, KisPageAllocationDescriptor *descriptor) const;

    // Captures current/retained/transaction-base roots once. TransactionOverlay
    // additionally freezes the sealed delta and surface metadata at capture;
    // unsealed writable bytes are never part of a read view.
    KisCapturedReadView captureReadView(const KisPageReadView &selector = {}, QString *error = nullptr);
    KisReadRequest acquireReadInView(const KisPageKey &key,
                                     const KisCapturedReadView &view,
                                     KisPageAccessRequirement access,
                                     KisPagePriority priority);

    KisReadRequest acquireRead(const KisPageKey &key,
                               const KisPageReadView &view,
                               KisPageAccessRequirement access,
                               KisPagePriority priority);
    // Claims the PageKey before provider preparation, even before a request
    // exists. The reservation survives request -> lease -> publish/cancel.
    // A prior removal remains visible until successful prepared-proof seal;
    // allocation/resolve alone never revive the page in the overlay.
    KisWriteRequest acquireWrite(const KisPageTransaction &transaction,
                                 const KisPageKey &key,
                                 KisPageAccessRequirement access,
                                 KisPageWriteMode mode,
                                 KisPagePriority priority);

    // Provider-independent operation delta. Pure semantic work requires no
    // pixel capability. The first pixel operation selects the sole compatible
    // native CPU provider before claiming or preparing a page.
    KisPageMutationSession beginMutation(const KisPageTransaction &transaction, QString *error = nullptr);


    KisReadLease resolve(const KisReadRequest &request, const KisCompletionTicket &completion);
    KisWriteLease resolve(const KisWriteRequest &request, const KisCompletionTicket &completion);
    bool cancel(const KisReadRequest &request);
    bool cancel(const KisWriteRequest &request);

    void release(KisReadLease lease, const KisCompletionTicket &consumerLastUse = {});
    bool acknowledgeLastUse(const KisVerifiedCompletion &completion);
    KisCompletionTicket archiveRead(const KisPageKey &key,
                                    const KisPageReadView &view,
                                    KisPagePriority priority = KisPagePriority::Background,
                                    QString *error = nullptr);
    QVector<KisPageArchiveCompletionEvent> pollArchiveCompletions(qsizetype maximumCount = 64);
    bool cancelArchive(const KisCompletionTicket &completion);

    KisCompletionTicket publish(KisWriteLease lease, const KisCompletionTicket &producerCompletion);
    KisCompletionTicket publishHostWrite(KisWriteLease lease);
    void cancel(KisWriteLease lease);

    // Historical base must still have snapshot or active-transaction protection.
    // A bare old epoch ID / metadata snapshot cannot revive retired history.
    KisPageTransaction beginTransaction(KisImageEpochId baseEpoch);
    /** Captures and retains the current committed root as a transaction base
     * under one owner gate. Does not export/copy an owning page manifest.
     * Explicit retained/historical bases still use beginTransaction(epoch).
     */
    KisPageTransaction beginCurrentTransaction();
    // Finish this transaction's native segments, generic requests/leases and
    // provider/terminal preparation first. Surface/default changes must not
    // race allocation or reinterpret still-borrowed mutable bytes.
    bool stageSurfaceMetadata(const KisPageTransaction &transaction,
                              const KisSurfaceEpochState &after,
                              QString *error = nullptr);
    // Owner allocates a fresh default identity, including after abort/undo.
    // Explicit stageSurfaceMetadata revisions must exceed the surface's
    // lifetime high-water mark when changing default bytes; gaps are legal.
    // Uses the same mutation-boundary restriction as stageSurfaceMetadata.
    bool stageSurfaceDefaultPixel(const KisPageTransaction &transaction,
                                  KisSurfaceId surface,
                                  const QByteArray &pixel,
                                  QString *error = nullptr);
    // Semantic pruning based on previously inspected bytes must not erase a
    // newer overlay version. Compare exact identity under the owner gate.
    bool stagePageRemovalIfUnchanged(const KisPageTransaction &transaction,
                                     const KisPageVersion &observed,
                                     QString *error = nullptr);
    KisPreparedPageSet preparedPages(const KisPageTransaction &transaction) const;
    KisImageEpochCommitTicket commit(const KisPageTransaction &transaction,
                                     const KisPreparedPageSet &preparedPages,
                                     KisRetainedImageEpochSnapshot *retainedAfter = nullptr);
    KisImageEpochCommitTicket restoreRetainedEpoch(const KisRetainedImageEpochSnapshot &retained);
    // Rejects active writers and unretained Prepared read/last-use capabilities.
    // Captured sealed versions survive abort as independently retained history;
    // abort never revokes their live pointers or execution last-use protection.
    bool abort(const KisPageTransaction &transaction);
    // Metadata-only snapshot; historical access requires captureRetainedEpoch().
    KisImageEpochSnapshot captureCommittedEpoch() const;
    KisRetainedImageEpochSnapshot captureRetainedEpoch();
    bool validateRetainedEpoch(const KisRetainedImageEpochSnapshot &retained) const;
    bool releaseSnapshot(KisImageEpochSnapshotToken token);

private:
    // Resolve a raw-read selection without capturing/freezing the overlay.
    // Default bytes and their version are selected under the same owner gate.
    bool resolveTileReadIdentity(const KisPageKey &, const KisPageReadView &,
                                 KisPageVersion *, KisSurfaceEpochState *) const;
    // Tiles3 derives this surface's bounds from the same published page delta.
    // Generic stores keep their caller-supplied surface metadata contract.
    bool configureDerivedPageExtent(KisSurfaceId surface);
    bool configureSharedNonPayloadBudget(
        const QSharedPointer<KisBackingBudgetController> &budget,
        QString *error);
    // The backend has already hidden its anonymous selector under this lock.
    // Release it after successful root installation, before deferred cleanup.
    KisImageEpochCommitTicket commitAndReleasePublicationLock(
        const KisPageTransaction &transaction, const KisPreparedPageSet &preparedPages,
        QWriteLocker &publicationLock);
    KisCompletionTicket finishWrite(KisWriteLease lease, const KisCompletionTicket &completion);
    // Adapter-only scratch prepared before pixels. On failure scratch may
    // contain a partial classification and must not be consumed. This grants
    // no view/pin and never classifies unsealed pending bytes.
    bool resolvePagePresenceInto(KisSurfaceId, const QVector<KisLogicalPageId> &,
                                 const KisPageReadView &, quint8 *scratch, qsizetype capacity) const;
    // Adapter-only: prepare the complete range in the store's sole admission
    // before a transaction/session or captured input is exposed. The owned
    // write set and claims move intact into the resulting mutation session.
    std::unique_ptr<KisPageStoreWriteReservation> reserveManagedRange(
        KisSurfaceId surface, const QSet<KisLogicalPageId> &targets,
        bool legacyIntent, bool *borrowed, QString *error);
    friend class KisTiledDataManagerPageStoreBackend;
    friend class KisPageStoreCpuMutationTest; // Exercise the original private shared-budget boundary.
    // Product composition prepares the real facade/root storage before its
    // internal controller is configured. Reference construction stays valid.
    using StoragePointer = std::unique_ptr<KisPageStore, void (*)(KisPageStore *)>;
    static StoragePointer prepareStorage(
        const QSharedPointer<KisBackingBudgetController> &parent, QString *error);
    explicit KisPageStore(const KisMutationStorageAllocator<KisPageStore> &storage);
    static void destroyStorage(KisPageStore *) noexcept;
    KisReadRequest acquireReadImpl(const KisPageKey &key,
                                   const KisPageReadView &view,
                                   KisPageAccessRequirement access,
                                   KisPagePriority priority,
                                   const KisCapturedReadView *captured);
    friend class KisPageStoreMementoManager;
    friend class KisCapturedReadView;
    friend class KisPageMutationSession;
    friend KisPageMetadataMetrics kisPageStoreMetadataMetrics(const KisPageStore &store);

    /**
     * Memento-only optimized restore/release. The history owner derives the
     * complete delta from a sealed PreparedPageSet; arbitrary callers cannot
     * assert completeness. Surface-default changes use the public full-scan
     * operations instead.
     */
    KisImageEpochCommitTicket restoreRetainedEpochDelta(const KisRetainedImageEpochSnapshot &retained,
                                                        const QVector<KisPageKey> &changedPages);
    KisRetainedImageEpochSnapshot captureRetainedEpochRoot();
    bool releaseSnapshotDelta(KisImageEpochSnapshotToken token, const QVector<KisPageKey> &changedPages);
    class Private;
    static KisCapturedReadView captureReadViewImpl(Private *, const KisPageReadView &, QString *error);
    struct PrivateReleaser {
        static void cleanup(Private *);
    };
    // Preserve the one-pointer facade ABI. Captured scopes hold an intrusive
    // lifetime reference; that reference grants no writer or pixel access.
    QScopedPointer<Private, PrivateReleaser> d;
};

#endif // KIS_PAGE_STORE_H

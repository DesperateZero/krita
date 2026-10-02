/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_READ_COORDINATOR_P_H
#define KIS_PAGE_READ_COORDINATOR_P_H

#include "KisMutationStorage_p.h"

#include "KisPageHistoryCollector_p.h"
#include "KisPageMetadataCoordinator_p.h"
#include "KisPageOwnerLedger.h"
#include "KisPageRetirementQueue_p.h"

#include <QAtomicInt>
#include <QMutex>
#include <QMutexLocker>
#include <QSharedPointer>
#include <QVector>

#include <memory>
#include <map>
#include <optional>
#include <boost/intrusive/list.hpp>

class KisPageReadCleanup;

// Prepared before retaining the root or any sealed version. Capture resolution
// and release use this same immutable version storage; only the retained prefix
// and completed release facts change under the owner gate.
struct KisPageCapturedRelease;
struct KisPageCapturedReleaseDeleter
{
    KisBackingBudgetController *budget = nullptr;
    KRITAIMAGE_EXPORT void operator()(KisPageCapturedRelease *record) const noexcept;
};
using KisPageCapturedReleasePointer = std::unique_ptr<KisPageCapturedRelease, KisPageCapturedReleaseDeleter>;
struct KisPageCapturedRelease
{
    explicit KisPageCapturedRelease(KisBackingBudgetController &budget)
        : versions(KisMutationStorageAllocator<KisPageVersion>(&budget)) {}
    KRITAIMAGE_EXPORT static KisPageCapturedReleasePointer prepare(KisBackingBudgetController &budget);
    KisImageEpochSnapshotToken token;
    std::vector<KisPageVersion, KisMutationStorageAllocator<KisPageVersion>> versions;
    size_t retained = 0;
    size_t next = 0;
    bool versionReleased = false;
    bool snapshotReleased = false;
    bool historyPending = false;
    bool capturedScope = false;
    bool processing = false;
    boost::intrusive::list_member_hook<> releaseHook;
};

struct KisPagePendingReadRecord
{
    enum class State : quint8 { Pending, Resolving, Cancelling, Released, Collecting };
    State state = State::Pending;
    KisReplicaHandle replica;
    KisPageAccessRequirement access;
    KisPageOperationId accessOperation;
    KisPageLeaseId reservedLease;
    KisCompletionTicket readiness;
    quint64 requestId = 0;
    boost::intrusive::list_member_hook<> cancelHook;
};

struct KisPageLastUseAcknowledgeResult
{
    bool matched = false;
    bool accepted = true;
};

struct KisPageReadCoordinatorSnapshot
{
    qsizetype pendingRequests = 0;
    qsizetype activeLeases = 0;
    qsizetype pendingLastUses = 0;
    quint64 requestsCreated = 0;
    quint64 capturedViewsCreated = 0;
    quint64 capturedViewReleases = 0;
    bool lastUseJobScheduled = false;
    quint64 lastUsePasses = 0;
    quint64 maximumLastUsesPerPass = 0;
    quint64 lastUseDispatchFailures = 0;
    qsizetype pendingCapturedReleases = 0;
};

class KisPageReadCoordinator final
{
public:
    // Original private composition boundary, also exercised by budget tests.
    KRITAIMAGE_EXPORT KisPageReadCoordinator(KisPageMetadataCoordinator &metadata,
                           KisImageEpochReferenceModel &epochs,
                           KisPageOwnerLedger &owner,
                           KisBackingBudgetController &budget,
                           KisPageHistoryCollector &history,
                           KisPageRetirementQueue &retirementQueue,
                           QSharedPointer<KisCompletionRegistry> &completions,
                           QMutex &ownerMutex,
                           qsizetype &activeProviderCalls,
                           bool &operational,
                           bool &backgroundReclamation,
                           QAtomicInt &ownerLifetime,
                           void *lifetimeContext,
                           void (*releaseLifetime)(void *));
    KRITAIMAGE_EXPORT ~KisPageReadCoordinator();
    // Original configure cold boundary, before any read protection or pin.
    KRITAIMAGE_EXPORT void prepareTask();

    KisPageReadCoordinator(const KisPageReadCoordinator &) = delete;
    KisPageReadCoordinator &operator=(const KisPageReadCoordinator &) = delete;
    KisPageReadCoordinator(KisPageReadCoordinator &&) = delete;
    KisPageReadCoordinator &operator=(KisPageReadCoordinator &&) = delete;

    // Cold native selection only. The caller retains this exact version/root
    // across discovery and pinning; never substitute the current version.
    // A stale candidate is conditionally evicted, preserving a newer cache.
    static Q_NEVER_INLINE KRITAIMAGE_EXPORT QSharedPointer<KisCpuReadBindingLink> discoverCpuReadBinding(
        KisPageMetadataCoordinator &metadata, KisPageOwnerLedger &owner,
        const KisPageVersion &version,
        const QSharedPointer<KisCpuReadBindingLink> &stale = {});

    KRITAIMAGE_EXPORT KisReadRequest registerRequestLocked(
        const KisPageVersion &version,
        const KisReplicaHandle &replica,
        KisPageAccessRequirement access,
        const KisCompletionTicket &readiness,
        KisPageTransactionId transaction,
        KisPageReadCleanup &cleanup);
    KRITAIMAGE_EXPORT KisReadLease resolveLocked(const KisReadRequest &request,
                               const KisCompletionTicket &completion,
                               QMutexLocker<QMutex> &ownerLock, KisPageReadCleanup &cleanup);
    KRITAIMAGE_EXPORT bool cancelLocked(const KisReadRequest &request, QMutexLocker<QMutex> &ownerLock,
                      KisPageReadCleanup &cleanup);
    void retryCancelledRequestsLocked(QMutexLocker<QMutex> &ownerLock,
                                     KisPageReadCleanup &cleanup, bool drain = false);
    KRITAIMAGE_EXPORT void releaseLocked(
        KisReadLease lease,
        const KisCompletionTicket &consumerLastUse,
        QMutexLocker<QMutex> &ownerLock, KisPageReadCleanup &cleanup);
    void retryReleasedReadsLocked(
        QMutexLocker<QMutex> &ownerLock, KisPageReadCleanup &cleanup,
        const KisCompletionTicket &observed = {}, bool *acknowledged = nullptr,
        bool drain = false);
    KisPageLastUseAcknowledgeResult acknowledgeLastUseLocked(
        const KisVerifiedCompletion &completion,
        QMutexLocker<QMutex> &ownerLock, KisPageReadCleanup &cleanup);
    // The original read-release records own pending last uses. Notifications
    // only schedule exact records on the existing reclamation executor.
    KRITAIMAGE_EXPORT void beginCloseLocked();
    KRITAIMAGE_EXPORT void cancelCloseLocked();
    KRITAIMAGE_EXPORT void stopAutomaticWakeups();
    KRITAIMAGE_EXPORT void waitForIdle();
    KRITAIMAGE_EXPORT void acknowledgeCompletedLastUsesLocked(
        QMutexLocker<QMutex> &ownerLock, KisPageReadCleanup &cleanup);

    bool protectsPreparedTransactionLocked(
        KisPageTransactionId transaction) const;
    bool cancelPreparedRequestsLocked(KisPageTransactionId transaction,
                                      KisPageReadCleanup &cleanup);

    void noteCapturedViewCreatedLocked();
    KRITAIMAGE_EXPORT bool releaseCapturedView(
        KisPageCapturedReleasePointer pending,
        QMutexLocker<QMutex> *heldOwnerLock = nullptr);
    KRITAIMAGE_EXPORT void retryCapturedReleasesLocked(QMutexLocker<QMutex> &ownerLock, bool drain = false);
    bool releaseSnapshot(KisImageEpochSnapshotToken token,
                         const QVector<KisPageKey> *changedPages);

    KRITAIMAGE_EXPORT KisPageReadCoordinatorSnapshot snapshotLocked() const;

private:
    struct ActiveReadRecord
    {
        ActiveReadRecord(
            const QSharedPointer<KisPageReplicaProvider> &providerValue,
            const KisReplicaHandle &replicaValue);
        ~ActiveReadRecord();

        QSharedPointer<KisPageReplicaProvider> provider;
        KisReplicaHandle replica;
        // Prepared before resolveAccess; engage only when the provider returns.
        std::optional<KisReplicaAccess> access;
        KisCompletionTicket releaseLastUse;
        quint64 leaseId = 0;
        bool metadataReleased = false; // owner mutex; this record now owns last-use debt
        bool historyPending = false;
        bool historyProcessing = false;
        boost::intrusive::list_member_hook<> releaseHook;
        KisPageReadinessSubscription lastUseSubscription;
        // Intrusive ready queue under LastUseWakeContext::mutex. The original
        // lease map remains the owner; no second replica/completion ledger.
        std::shared_ptr<ActiveReadRecord> nextReady;
        bool readyQueued = false;
    };

    struct LastUseWakeContext;
    void armLastUseLocked(const std::shared_ptr<ActiveReadRecord> &record);
    static void notifyLastUse(const std::weak_ptr<LastUseWakeContext> &context,
                             const std::weak_ptr<ActiveReadRecord> &record);
    static void dispatchLastUses(const std::weak_ptr<LastUseWakeContext> &context);
    void processLastUses(const std::shared_ptr<LastUseWakeContext> &context);
    bool acknowledgeRecordLocked(const std::shared_ptr<ActiveReadRecord> &record,
                                 const KisVerifiedCompletion &completion,
                                 KisPageReadCleanup &cleanup);

    bool finishReleasedReadLocked(quint64 leaseId, KisPageReadCleanup &cleanup,
                                  bool *lastUseAcknowledged = nullptr);
    bool finishHistoryReadLocked(const std::shared_ptr<ActiveReadRecord> &record,
                                 QMutexLocker<QMutex> &ownerLock, KisPageReadCleanup &cleanup);
    bool releaseReadLocked(const KisReplicaHandle &replica,
                           KisPageLeaseId lease, KisPageReadCleanup &cleanup,
                           const KisCompletionTicket &completion = {});
    bool releaseSnapshotLocked(KisImageEpochSnapshotToken token,
                               const QVector<KisPageKey> *changedPages,
                               QMutexLocker<QMutex> &ownerLock);
    bool belongsToPreparedTransactionLocked(
        const KisPageVersion &version,
        KisPageTransactionId transaction) const;
    void processRetirementsUnlocked(QMutexLocker<QMutex> &ownerLock);

    friend class KisPageReadCleanup;
    KisPageMetadataCoordinator &m_metadata;
    KisImageEpochReferenceModel &m_epochs;
    KisPageOwnerLedger &m_owner;
    KisBackingBudgetController &m_budget;
    KisPageHistoryCollector &m_history;
    KisPageRetirementQueue &m_retirementQueue;
    QSharedPointer<KisCompletionRegistry> &m_completions;
    QMutex &m_ownerMutex;
    qsizetype &m_activeProviderCalls;
    bool &m_operational;
    bool &m_backgroundReclamation;
    QAtomicInt &m_ownerLifetime;
    void *m_lifetimeContext;
    void (*m_releaseLifetime)(void *);

    bool finishCapturedReleaseLocked(KisPageCapturedRelease &pending,
                                     QMutexLocker<QMutex> &ownerLock);

    // Node storage precedes protection/pinning. Active nodes can be installed
    // without allocation even when other requests grow the maps during resolve.
    using PendingReadAllocator = KisMutationStorageAllocator<
        std::pair<const quint64, KisPagePendingReadRecord>>;
    using PendingReadMap = std::map<quint64, KisPagePendingReadRecord,
                                   std::less<quint64>, PendingReadAllocator>;
    PendingReadMap m_requests;
    boost::intrusive::list<KisPagePendingReadRecord, boost::intrusive::member_hook<
        KisPagePendingReadRecord, boost::intrusive::list_member_hook<>,
        &KisPagePendingReadRecord::cancelHook>> m_cancelRetries;
    using ActiveReadAllocator = KisMutationStorageAllocator<
        std::pair<const quint64, std::shared_ptr<ActiveReadRecord>>>;
    using ActiveReadMap = std::map<quint64, std::shared_ptr<ActiveReadRecord>,
                                  std::less<quint64>, ActiveReadAllocator>;
    ActiveReadMap m_activeReads;
    boost::intrusive::list<ActiveReadRecord, boost::intrusive::member_hook<
        ActiveReadRecord, boost::intrusive::list_member_hook<>,
        &ActiveReadRecord::releaseHook>> m_releaseRetries;
    boost::intrusive::list<KisPageCapturedRelease, boost::intrusive::member_hook<
        KisPageCapturedRelease, boost::intrusive::list_member_hook<>,
        &KisPageCapturedRelease::releaseHook>> m_capturedReleaseRetries;
    qsizetype m_pendingLastUses = 0;
    qsizetype m_pendingHistoryReads = 0;
    std::shared_ptr<LastUseWakeContext> m_lastUseWakeContext;
    bool m_automaticWakeupsStopped = false;
    bool m_lastUseClosing = false;
    quint64 m_lastUsePasses = 0;
    quint64 m_maximumLastUsesPerPass = 0;
    quint64 m_requestsCreated = 0;
    quint64 m_capturedViewsCreated = 0;
    quint64 m_capturedViewReleases = 0;
};

// Construct before the outer store lock. Transfer the original map nodes and
// detached metadata blocks here without allocation, then free after that lock
// unwinds. The activity count covers actual cleanup through idle publication.
class KisPageReadCleanup
{
public:
    KRITAIMAGE_EXPORT explicit KisPageReadCleanup(KisPageReadCoordinator &owner);
    KRITAIMAGE_EXPORT ~KisPageReadCleanup();
    // Prepare the original request node before selecting a version. Selection
    // and AcquireRead must share one owner interval, including virtual defaults.
    KRITAIMAGE_EXPORT bool prepareRequestLocked(QMutexLocker<QMutex> &ownerLock, QString *error);
    KisPageReadCleanup(const KisPageReadCleanup &) = delete;
    KisPageReadCleanup &operator=(const KisPageReadCleanup &) = delete;
    void finishUnlocked(QMutexLocker<QMutex> &ownerLock);
private:
    void retainLocked();
    void finish();
    KisPageReadCoordinator &m_owner;
    KisPageMetadataReadCleanup m_storage;
    KisPageReadCoordinator::PendingReadMap m_requests;
    KisPageReadCoordinator::ActiveReadMap m_reads;
    quint64 m_preparedRequestId = 0;
    bool m_active = false;
    friend class KisPageReadCoordinator;
};

#endif // KIS_PAGE_READ_COORDINATOR_P_H

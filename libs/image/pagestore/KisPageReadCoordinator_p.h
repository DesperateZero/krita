/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_READ_COORDINATOR_P_H
#define KIS_PAGE_READ_COORDINATOR_P_H

#include "KisPageHistoryCollector_p.h"
#include "KisPageOwnerLedger.h"
#include "KisPageRetirementQueue_p.h"

#include <QAtomicInt>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSharedPointer>
#include <QVector>

#include <deque>
#include <memory>

struct KisPagePendingReadRecord
{
    enum class State : quint8 { Pending, Resolving, Cancelling };
    State state = State::Pending;
    KisReplicaHandle replica;
    KisPageAccessRequirement access;
    KisPageOperationId accessOperation;
    KisPageLeaseId reservedLease;
    KisCompletionTicket readiness;
};

struct KisPagePendingLastUseRecord
{
    KisReplicaHandle replica;
    KisCompletionTicket completion;
};

struct KisPageLastUseAcknowledgeResult
{
    bool matched = false;
    bool accepted = true;
    QVector<KisPageKey> releasedKeys;
};

struct KisPageReadCoordinatorSnapshot
{
    qsizetype pendingRequests = 0;
    qsizetype activeLeases = 0;
    qsizetype pendingLastUses = 0;
    quint64 requestsCreated = 0;
    quint64 capturedViewsCreated = 0;
    quint64 capturedViewReleases = 0;
};

class KisPageReadCoordinator final
{
public:
    KisPageReadCoordinator(KisPageMetadataCoordinator &metadata,
                           KisImageEpochReferenceModel &epochs,
                           KisPageOwnerLedger &owner,
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
    ~KisPageReadCoordinator() = default;

    KisPageReadCoordinator(const KisPageReadCoordinator &) = delete;
    KisPageReadCoordinator &operator=(const KisPageReadCoordinator &) = delete;
    KisPageReadCoordinator(KisPageReadCoordinator &&) = delete;
    KisPageReadCoordinator &operator=(KisPageReadCoordinator &&) = delete;

    KisReadRequest registerRequestLocked(
        const KisPageVersion &version,
        const KisReplicaHandle &replica,
        KisPageAccessRequirement access,
        const KisCompletionTicket &readiness,
        KisPageTransactionId transaction = {});
    KisReadLease resolveLocked(const KisReadRequest &request,
                               const KisCompletionTicket &completion,
                               QMutexLocker<QMutex> &ownerLock);
    bool cancelLocked(const KisReadRequest &request, QMutexLocker<QMutex> &ownerLock);
    void retryCancelledRequestsLocked(QMutexLocker<QMutex> &ownerLock, bool drain = false);
    KisPageKey releaseLocked(
        KisReadLease lease,
        const KisCompletionTicket &consumerLastUse,
        QMutexLocker<QMutex> &ownerLock);
    QVector<KisPageKey> retryReleasedReadsLocked(
        const KisCompletionTicket &observed = {}, bool *acknowledged = nullptr,
        bool drain = false);
    KisPageLastUseAcknowledgeResult acknowledgeLastUseLocked(
        const KisVerifiedCompletion &completion);

    bool protectsPreparedTransactionLocked(
        KisPageTransactionId transaction) const;
    bool cancelPreparedRequestsLocked(KisPageTransactionId transaction);

    void noteCapturedViewCreatedLocked();
    bool releaseCapturedView(
        KisImageEpochSnapshotToken token,
        const QHash<KisPageKey, KisPageVersion> &versions,
        bool capturedScope = true,
        QMutexLocker<QMutex> *heldOwnerLock = nullptr);
    void retryCapturedReleasesLocked(QMutexLocker<QMutex> &ownerLock, bool drain = false);
    bool releaseSnapshot(KisImageEpochSnapshotToken token,
                         const QVector<KisPageKey> *changedPages);

    KisPageReadCoordinatorSnapshot snapshotLocked() const;

private:
    struct ActiveReadRecord
    {
        ActiveReadRecord(
            const QSharedPointer<KisPageReplicaProvider> &providerValue,
            KisReplicaAccess &&accessValue);
        ~ActiveReadRecord();

        QSharedPointer<KisPageReplicaProvider> provider;
        KisReplicaHandle replica;
        KisReplicaAccess access;
        KisCompletionTicket releaseLastUse;
    };

    bool finishReleasedReadLocked(quint64 leaseId, KisPageKey *key,
                                  bool *lastUseAcknowledged = nullptr);
    bool releaseReadLocked(const KisReplicaHandle &replica,
                           KisPageLeaseId lease,
                           const KisCompletionTicket &completion = {});
    bool releaseSnapshotLocked(KisImageEpochSnapshotToken token,
                               const QVector<KisPageKey> *changedPages,
                               bool capturedScope,
                               QMutexLocker<QMutex> &ownerLock);
    bool belongsToPreparedTransactionLocked(
        const KisPageVersion &version,
        KisPageTransactionId transaction) const;
    void retireEffectsUnlocked(QVector<KisPageTransitionEffect> effects,
                               QMutexLocker<QMutex> &ownerLock);

    KisPageMetadataCoordinator &m_metadata;
    KisImageEpochReferenceModel &m_epochs;
    KisPageOwnerLedger &m_owner;
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

    struct PendingCapturedRelease {
        KisImageEpochSnapshotToken token;
        QVector<KisPageVersion> versions;
        bool capturedScope = true;
    };

    QVector<KisPageKey> releaseCapturedVersionsLocked(PendingCapturedRelease &pending);

    QHash<quint64, KisPagePendingReadRecord> m_requests;
    std::deque<quint64> m_cancelRetries;
    QHash<quint64, std::shared_ptr<ActiveReadRecord>> m_activeReads;
    std::deque<quint64> m_releaseRetries;
    std::deque<PendingCapturedRelease> m_capturedReleaseRetries;
    QVector<KisPagePendingLastUseRecord> m_pendingLastUses;
    quint64 m_requestsCreated = 0;
    quint64 m_capturedViewsCreated = 0;
    quint64 m_capturedViewReleases = 0;
};

#endif // KIS_PAGE_READ_COORDINATOR_P_H

/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageStore.h"
#include "KisCpuResidentBinding_p.h"

#include "KisImageEpochReferenceModel.h"
#include "KisPageDefaultStorage_p.h"
#include "KisPageHistoryCollector_p.h"
#include "KisPageMetadataCoordinator.h"
#include "KisPageOwnerLedger.h"
#include "KisPagePublicationCoordinator_p.h"
#include "KisPageReadCoordinator_p.h"
#include "KisPageRetirementQueue_p.h"
#include "KisPageStoreDiagnostics_p.h"
#include "KisPageStoreReclamation_p.h"
#include "KisPageWriteCoordinator_p.h"

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <QWaitCondition>

#include <algorithm>
#include <atomic>
#include <tuple>
#include <cstddef>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace
{

bool ownsPreparedReplica(const KisReplicaHandle &replica, const KisPageVersion &version,
                         const KisPageReplicaProvider &provider, const KisReplicaHandle &source = {})
{
    return replica.isValid() && replica.version == version
        && replica.provider == provider.providerId()
        && replica.providerEpoch == provider.providerEpoch()
        && !(replica.allocationIdentity() == source.allocationIdentity());
}

struct WriteClosureRecord {
    KisPageTransition transition(KisPageTransitionKind kind) const
    {
        KisPageTransition result;
        result.kind = kind;
        result.baseVersion = baseVersion;
        result.version = version;
        result.source = source;
        result.target = replica;
        result.operation = operation;
        result.writer = writer;
        result.transaction = transaction;
        result.writeMode = writeMode;
        return result;
    }
    KisPageVersion baseVersion;
    KisPageVersion version;
    KisReplicaHandle source;
    KisReplicaHandle replica;
    KisPageOperationId operation;
    KisPageTransactionId transaction;
    KisPageWriterToken writer;
    KisPageWriteMode writeMode = KisPageWriteMode::PreserveContents;
};

struct PendingWriteRequestRecord : WriteClosureRecord {
    enum class State : quint8 { Preparing, Pending, Resolving } state = State::Pending;
    KisPageAccessRequirement access;
    KisPageOperationId accessOperation;
    KisCompletionTicket readiness;
};

struct ActiveWriteRecord {
    ActiveWriteRecord(const QSharedPointer<KisPageReplicaProvider> &providerValue,
                      WriteClosureRecord requestValue)
        : provider(providerValue)
        , request(std::move(requestValue))
    {
    }

    ~ActiveWriteRecord()
    {
        if (provider && access && access->isValid()) {
            provider->releaseAccess(std::move(*access), {});
        }
    }

    QSharedPointer<KisPageReplicaProvider> provider;
    std::optional<KisReplicaAccess> access;
    WriteClosureRecord request;
};

using ActiveWriteMap = std::map<quint64, std::shared_ptr<ActiveWriteRecord>>;

struct PendingArchiveRecord {
    bool busy = false;
    KisPageVersion version;
    KisCompletionTicket completion;
    QSharedPointer<KisExactGenerationArchive> archive;
};

struct DeferredMetadataCleanupStatistics {
    QAtomicInteger<quint64> deferredCandidates{0};
    QAtomicInteger<quint64> completedCandidates{0};
    QAtomicInteger<quint64> passes{0};
    QAtomicInteger<quint64> units{0};
    QAtomicInteger<quint64> maximumUnitsPerPass{0};
    QAtomicInteger<quint64> nanoseconds{0};
    QAtomicInteger<quint64> maximumPassNanoseconds{0};
    QAtomicInteger<quint64> queueNanoseconds{0};
    QAtomicInteger<quint64> maximumQueueNanoseconds{0};
    QAtomicInteger<qsizetype> pendingCandidates{0};
    QAtomicInteger<qsizetype> peakPendingCandidates{0};
    QAtomicInteger<qsizetype> pendingUnits{0};
    QAtomicInteger<qsizetype> peakPendingUnits{0};
    QMutex idleMutex;
    QWaitCondition idle;
};

template<typename Cleanup>
struct DeferredMetadataCleanupJob {
    explicit DeferredMetadataCleanupJob(Cleanup &&value)
        : cleanup(std::move(value))
    {
        age.start();
    }
    Cleanup cleanup;
    QElapsedTimer age;
};

template<typename T>
void updateRelaxedMaximum(QAtomicInteger<T> &target, T value)
{
    T observed = target.loadRelaxed();
    while (observed < value && !target.testAndSetRelaxed(observed, value, observed)) { }
}

template<typename Cleanup>
void scheduleDeferredMetadataCleanupPass(const std::shared_ptr<DeferredMetadataCleanupJob<Cleanup>> &job,
                                         const QSharedPointer<DeferredMetadataCleanupStatistics> &statistics)
{
    kisSchedulePageStoreReclamation([job, statistics] {
        constexpr qsizetype maximumUnitsPerPass = 16;
        constexpr qint64 maximumNanosecondsPerPass = 1000000;
        QElapsedTimer timer;
        timer.start();
        qsizetype cleared = 0;
        while (!job->cleanup.isEmpty() && cleared < maximumUnitsPerPass) {
            cleared += job->cleanup.clearBatch(1);
            if (timer.nsecsElapsed() >= maximumNanosecondsPerPass)
                break;
        }
        const quint64 elapsed = quint64(timer.nsecsElapsed());
        statistics->passes.fetchAndAddRelaxed(1);
        statistics->units.fetchAndAddRelaxed(quint64(cleared));
        statistics->nanoseconds.fetchAndAddRelaxed(elapsed);
        {
            QMutexLocker lock(&statistics->idleMutex);
            const qsizetype before = statistics->pendingUnits.fetchAndSubRelaxed(cleared);
            Q_ASSERT(before >= cleared);
        }
        updateRelaxedMaximum(statistics->maximumUnitsPerPass, quint64(cleared));
        updateRelaxedMaximum(statistics->maximumPassNanoseconds, elapsed);
        if (!job->cleanup.isEmpty()) {
            scheduleDeferredMetadataCleanupPass(job, statistics);
        } else {
            const quint64 age = quint64(job->age.nsecsElapsed());
            statistics->completedCandidates.fetchAndAddRelaxed(1);
            statistics->queueNanoseconds.fetchAndAddRelaxed(age);
            updateRelaxedMaximum(statistics->maximumQueueNanoseconds, age);
            QMutexLocker lock(&statistics->idleMutex);
            const qsizetype before = statistics->pendingCandidates.fetchAndSubRelaxed(1);
            Q_ASSERT(before > 0);
            if (before == 1 && statistics->pendingUnits.loadRelaxed() == 0)
                statistics->idle.wakeAll();
        }
    });
}

template<typename Cleanup>
void disposeDeferredMetadataCleanup(Cleanup cleanup,
                                    const QSharedPointer<DeferredMetadataCleanupStatistics> &statistics)
{
    if (cleanup.isEmpty())
        return;
    constexpr qsizetype maximumInlineUnits = 8;
    const qsizetype units = cleanup.pendingWorkUnits();
    if (units <= maximumInlineUnits) {
        cleanup.clearBatch(units);
        return;
    }
    // Bound queued lifetime independently of commit rate. One work unit is a
    // publication page whose internal payload is already record-budgeted.
    // Overflow cleanup runs here, after the owner gate has been released.
    constexpr qsizetype maximumDeferredUnits = 1024;
    QMutexLocker idleLock(&statistics->idleMutex);
    const qsizetype currentPending = statistics->pendingUnits.loadRelaxed();
    if (units > maximumDeferredUnits
        || currentPending > maximumDeferredUnits - units) {
        idleLock.unlock();
        cleanup.clearBatch(units);
        return;
    }
    statistics->deferredCandidates.fetchAndAddRelaxed(1);
    const qsizetype pending = statistics->pendingUnits.fetchAndAddRelaxed(units) + units;
    const qsizetype pendingCandidates = statistics->pendingCandidates.fetchAndAddRelaxed(1) + 1;
    updateRelaxedMaximum(statistics->peakPendingUnits, pending);
    updateRelaxedMaximum(statistics->peakPendingCandidates, pendingCandidates);
    auto job = std::make_shared<DeferredMetadataCleanupJob<Cleanup>>(std::move(cleanup));
    scheduleDeferredMetadataCleanupPass(job, statistics);
}

} // namespace

class KisPageStore::Private
{
public:
    Private()
        : writeAdmission(mutex, writeAdmissionChanged, &backingBudget, &operational)
        , writeCoordinator(metadata, epochs, backingBudget, owner)
        , defaultStorage(backingBudget)
        , retirementQueue(owner, metadata, backingBudget, lifetimeReferences, this, &Private::releaseRetirementLifetime)
        , historyCollector(metadata,
                           epochs,
                           retirementQueue,
                           mutex,
                           lifetimeReferences,
                           operational,
                           closing,
                           backgroundReclamation,
                           this,
                           &Private::releaseRetirementLifetime,
                           &Private::removeHistoryDescriptor)
        , publicationCoordinator(epochs,
                                 metadata,
                                 owner,
                                 backingBudget,
                                 retirementQueue,
                                 historyCollector,
                                 readyHostCompletion,
                                 activeProviderCalls,
                                 operational,
                                 backgroundReclamation,
                                 this,
                                 &Private::publicationTransactionHasMutationActivity,
                                 &Private::publicationPageWriteClaimed,
                                 &Private::beginPublicationMutationPreparation,
                                 &Private::endPublicationMutationPreparation,
                                 &Private::disposePublicationMetadataCleanup,
                                 &Private::publicationRestoreIsIdle,
                                 &Private::preparePublicationAbort)
        , readCoordinator(metadata,
                          epochs,
                          owner,
                          backingBudget,
                          historyCollector,
                          retirementQueue,
                          completions,
                          mutex,
                          activeProviderCalls,
                          operational,
                          backgroundReclamation,
                          lifetimeReferences,
                          this,
                          &Private::releaseRetirementLifetime)
    {
        // Prepare the one process worker before this root can be released.
        // configure() admits its terminal/recurring nodes before any physical
        // state; the last reference never allocates a task or a thread.
        kisEnqueuePageStoreReclamation(nullptr);
        owner.attachBackingBudget(backingBudget);
        metadata.attachBackingBudget(backingBudget);
        epochs.attachBackingBudget(backingBudget);
        metadata.attachRetirementDebtOwner(this,
                                           &Private::prepareRetirementDebt,
                                           &Private::commitRetirementDebt,
                                           &Private::cancelRetirementDebt);
    }

    static bool prepareRetirementDebt(void *context,
                                      const KisPageTransitionEffect *effects, qsizetype count,
                                      quint64 *cookie,
                                      QString *error)
    {
        return static_cast<Private *>(context)->owner.prepareRetirementDebt(effects, count, cookie, error);
    }

    static void commitRetirementDebt(void *context, quint64 cookie,
                                      const KisPageTransitionEffect *effects, qsizetype count) noexcept
    {
        auto *self = static_cast<Private *>(context);
        self->owner.commitRetirementDebt(cookie);
        for (qsizetype i = 0; i < count; ++i) self->retirementQueue.acceptEffect(effects[i]);
    }

    static void cancelRetirementDebt(void *context, quint64 cookie) noexcept
    {
        static_cast<Private *>(context)->owner.cancelRetirementDebt(cookie);
    }

    static void releaseRetirementLifetime(void *context)
    {
        KisPageStore::PrivateReleaser::cleanup(static_cast<KisPageStore::Private *>(context));
    }

    static void removeHistoryDescriptor(void *context, const KisPageVersion &version)
    {
        static_cast<KisPageStore::Private *>(context)->publicationCoordinator.removeDescriptorLocked(version);
    }

    static bool publicationTransactionHasMutationActivity(void *context, KisPageTransactionId transaction)
    {
        const auto *owner = static_cast<KisPageStore::Private *>(context);
        return owner->writeCoordinator.transactionHasMutationActivity(transaction);
    }

    static bool publicationPageWriteClaimed(void *context, const KisPageKey &key)
    {
        return static_cast<KisPageStore::Private *>(context)->writeAdmission.pageClaimedLocked(key);
    }

    static bool beginPublicationMutationPreparation(void *context, KisPageTransactionId transaction,
                                                    QMutexLocker<QMutex> &lock, QString *error)
    {
        auto *owner = static_cast<KisPageStore::Private *>(context);
        ++owner->activeProviderCalls;
        const auto preparation = qScopeGuard([&] { --owner->activeProviderCalls; });
        return owner->writeCoordinator.beginPreparationActivity(transaction, lock, error);
    }

    static void endPublicationMutationPreparation(void *context, KisPageTransactionId transaction)
    {
        static_cast<KisPageStore::Private *>(context)->writeCoordinator.endPreparationActivity(transaction);
    }

    static void disposePublicationMetadataCleanup(void *context,
                                                  KisPageMetadataCoordinator::DeferredPublicationCleanup cleanup)
    {
        auto *owner = static_cast<KisPageStore::Private *>(context);
        disposeDeferredMetadataCleanup(std::move(cleanup), owner->metadataCleanupStatistics);
    }

    static bool publicationRestoreIsIdle(void *context)
    {
        const auto *owner = static_cast<KisPageStore::Private *>(context);
        const KisPageStoreSessionStats stats = owner->stats();
        return stats.activeTransactions == 0 && stats.pendingRequests == 0 && stats.activeWriteLeases == 0
            && stats.preparedPageProofs == 0 && stats.preparedSurfaceChanges == 0 && stats.stagedPageRemovals == 0
            && stats.activeProviderCalls == 0 && stats.activeCpuWritePages == 0
            && owner->owner.publicationBlockingOperationCount() == 0 && stats.sealedPreparedProofs == 0;
    }

    static bool preparePublicationAbort(void *context,
                                        KisPageTransactionId transaction,
                                        KisPageReadCleanup &cleanup)
    {
        auto *owner = static_cast<KisPageStore::Private *>(context);
        if (owner->writeCoordinator.transactionHasSessionOrPreparation(transaction)) {
            return false;
        }
        for (auto it = owner->activeWrites.begin(); it != owner->activeWrites.end();) {
            const auto active = it->second;
            if (!(active->request.transaction == transaction)) {
                ++it;
                continue;
            }
            // Consumed leases retain admission after a failed detach. Abort
            // retries them; an exposed or currently releasing lease blocks it.
            if (active->access && active->access->isValid()) return false;
            const auto result = owner->cancelWriteLocked(active->request);
            if (!result.accepted) return false;
            owner->releaseGenericWrite(transaction, active->request.version.key);
            it = owner->activeWrites.erase(it);
        }
        if (owner->readCoordinator.protectsPreparedTransactionLocked(transaction)
            || !owner->readCoordinator.cancelPreparedRequestsLocked(transaction, cleanup)) {
            return false;
        }
        for (auto it = owner->writeRequests.begin(); it != owner->writeRequests.end();) {
            if (!(it->second.transaction == transaction)) {
                ++it;
                continue;
            }
            if (it->second.state != PendingWriteRequestRecord::State::Pending)
                return false;
            const auto result = owner->cancelWriteLocked(it->second);
            if (!result.accepted)
                return false;
            owner->releaseGenericWrite(it->second.transaction, it->second.version.key);
            it = owner->writeRequests.erase(it);
        }
        return true;
    }

    KisPageStoreSessionStats stats(bool includeMetadataFootprint = false) const
    {
        KisPageStoreSessionStats result;
        result.configured = bool(completions);
        result.operational = operational;
        result.closed = closed;
        result.registeredPages = metadata.pageCount();
        if (includeMetadataFootprint) {
            const KisPageMetadataFootprint metadataFootprint = metadata.footprint();
            result.pageVersions = qsizetype(metadataFootprint.versions);
            result.replicas = qsizetype(metadataFootprint.replicas);
        }
        result.immutableRoots = epochs.rootCount();
        result.activeTransactions = epochs.activeTransactionCount();
        result.retainedSnapshots = epochs.retainedSnapshotCount();
        const KisPageReadCoordinatorSnapshot readSnapshot = readCoordinator.snapshotLocked();
        const KisPagePublicationCoordinatorSnapshot publicationSnapshot = publicationCoordinator.snapshotLocked();
        result.pendingRequests = writeRequests.size() + readSnapshot.pendingRequests;
        result.activeReadLeases = readSnapshot.activeLeases;
        result.activeWriteLeases = activeWrites.size();
        result.activeControlWritePages = writeAdmission.activeGenericClaimCountLocked();
        result.activeCpuWritePages = writeAdmission.activeNativeClaimCountLocked();
        result.pendingLastUses = readSnapshot.pendingLastUses;
        result.preparedPageProofs = publicationSnapshot.preparedProofs;
        result.preparedSurfaceChanges = publicationSnapshot.preparedSurfaceChanges;
        result.stagedPageRemovals = publicationSnapshot.stagedPageRemovals;
        result.activeProviderCalls = activeProviderCalls;
        result.providerOperations = owner.providerOperationCount();
        result.sealedPreparedProofs = owner.sealedProofCount();
        result.pendingArchiveOperations = pendingArchives.size();
        result.pendingShutdownReplicas = pendingShutdownReplicas.size();
        const KisPageRetirementQueueSnapshot retirementSnapshot = retirementQueue.snapshot();
        const KisPageHistoryCollectorSnapshot historySnapshot = historyCollector.snapshotLocked();
        result.pendingRetiredReplicas = retirementSnapshot.pendingReplicas;
        result.activeRetirementReplicas = retirementSnapshot.activeReplicas;
        result.pendingRetiredBytes = retirementSnapshot.pendingBytes;
        result.scheduledReclamationJobs =
            qsizetype(retirementSnapshot.jobScheduled) + qsizetype(historySnapshot.jobScheduled) +
            qsizetype(readSnapshot.lastUseJobScheduled);
        result.lastUseAcknowledgePasses = readSnapshot.lastUsePasses;
        result.maximumLastUsesPerPass = readSnapshot.maximumLastUsesPerPass;
        result.lastUseDispatchFailures = readSnapshot.lastUseDispatchFailures;
        result.backgroundRetirementPasses = retirementSnapshot.backgroundPasses;
        result.maximumReplicasPerRetirementPass = retirementSnapshot.maximumReplicasPerPass;
        result.peakRetiredReplicas = retirementSnapshot.peakPendingReplicas;
        result.peakRetiredBytes = retirementSnapshot.peakPendingBytes;
        result.retirementRetryRequeues = retirementSnapshot.retryRequeues;
        result.retirementCloseDrainedReplicas = retirementSnapshot.closeDrainedReplicas;
        result.delayedRetiredReplicas = retirementSnapshot.delayedReplicas;
        result.scheduledRetirementRetries = qsizetype(retirementSnapshot.retryScheduled);
        result.retirementRetryWakeups = retirementSnapshot.retryWakeups;
        result.maximumReplicasPerRetirementRetryWake = retirementSnapshot.maximumReplicasPerRetryWake;
        result.nextRetirementRetryDelayMs = retirementSnapshot.nextRetryDelayMs;
        result.pendingHistoricalPages = historySnapshot.pendingPages;
        result.deferredHistoricalPages = historySnapshot.deferredPages;
        result.activeHistoryScans = historySnapshot.activeScans;
        result.cachedHistoryReachableVersions = historySnapshot.cachedReachableVersions;
        result.readRequestsCreated = readSnapshot.requestsCreated;
        result.writeRequestsCreated = writeRequestsCreated;
        const KisPageDefaultStorageSnapshot defaultSnapshot = defaultStorage.snapshotLocked();
        result.defaultMaterializationRequests = defaultSnapshot.materializationRequests;
        result.synchronousHostWrites = synchronousHostWrites;
        result.committedTransactions = publicationSnapshot.committedTransactions;
        result.cachedDefaultReadBuffers = defaultSnapshot.cachedReadBuffers;
        result.cachedDefaultReadBytes = defaultSnapshot.cachedReadBytes;
        result.liveDefaultReadBytes = defaultSnapshot.liveReadBytes;
        result.defaultReadBuffersCreated = defaultSnapshot.readBuffersCreated;
        result.defaultReadInitializedBytes = defaultSnapshot.readInitializedBytes;
        result.defaultReadCacheEvictions = defaultSnapshot.cacheEvictions;
        result.defaultReadCacheOversizeBypasses = defaultSnapshot.cacheOversizeBypasses;
        result.activeDefaultPreparations = defaultSnapshot.activePreparations;
        result.peakDefaultPreparations = defaultSnapshot.peakPreparations;
        result.defaultPreparationWaits = defaultSnapshot.preparationWaits;
        return result;
    }

    KisPageMetadataTransitionResult cancelWriteLocked(const WriteClosureRecord &request)
    {
        auto result = writeCoordinator.cancelPrivateWrite(request.transition(KisPageTransitionKind::CancelWrite));
        if (result.accepted)
            publicationCoordinator.removeDescriptorLocked(request.version);
        return result;
    }

    void processRetirementsLocked(QMutexLocker<QMutex> &lock)
    {
        ++activeProviderCalls;
        lock.unlock();
        retirementQueue.processAcceptedEffects(backgroundReclamation);
        lock.relock();
        --activeProviderCalls;
    }

    void retireRejectedReplicaLocked(const KisReplicaHandle &replica,
                                     const QSharedPointer<KisPageReplicaProvider> &provider,
                                     KisPageBackingPreparation &&backing,
                                     QMutexLocker<QMutex> &lock)
    {
        ++activeProviderCalls;
        lock.unlock();
        retirementQueue.retireOrDefer(replica, provider, {}, std::move(backing));
        lock.relock();
        --activeProviderCalls;
    }


    bool ensureDefaultPageLocked(const KisPageVersion &version,
                                 KisImageEpochId publishedEpoch,
                                 const KisSurfaceEpochState &surface,
                                 KisPageAccessRequirement access,
                                 KisPagePriority priority,
                                 QMutexLocker<QMutex> *locker,
                                 QString *error)
    {
        KisPageMetadataCoordinator::VersionInfo existing;
        const auto isMaterialized = [&] {
            if (!metadata.versionSnapshot(version, &existing)) return false;
            return existing.version.isValid() && !existing.needsDefaultMaterialization(version);
        };
        if (isMaterialized()) {
            KisPageStoreDetail::setError(error, {});
            return true;
        }
        if (!version.isDefaultPixel() || !publishedEpoch.isValid() || !surface.isValid()
            || !(surface.surface == version.key.surface) || surface.defaultPixelRevision != version.defaultPixelRevision
            || !access.isValid() || !locker) {
            KisPageStoreDetail::setError(error, QStringLiteral("implicit default page request is invalid"));
            return false;
        }
        const auto awaitPreparation = [&] {
            while (operational && defaultStorage.preparationBlockedLocked(version.key))
                defaultStorage.waitForPreparationChangeLocked(&mutex);
            if (!operational || closed) {
                KisPageStoreDetail::setError(error, QStringLiteral("implicit default page preparation was interrupted"));
                return false;
            }
            return true;
        };
        if (!awaitPreparation()) return false;
        if (isMaterialized()) {
            KisPageStoreDetail::setError(error, {});
            return true;
        }
        const KisPageAllocationDescriptor descriptor = surface.allocationDescriptor();
        if (!descriptor.isValid()) {
            KisPageStoreDetail::setError(error, QStringLiteral("implicit default allocation descriptor is invalid"));
            return false;
        }
        const QSharedPointer<KisPageReplicaProvider> provider = owner.providerFor(access);
        const KisPageOperationId operation = owner.nextOperationId();
        if (!provider || !operation.isValid()) {
            KisPageStoreDetail::setError(error, QStringLiteral("implicit default provider is unavailable or ambiguous"));
            return false;
        }
        KisPageVersion currentVersion;
        if (!epochs.captureCommittedRoot().resolve(version.key, &currentVersion)) {
            KisPageStoreDetail::setError(error, QStringLiteral("implicit default has no committed root"));
            return false;
        }
        KisBackingBudgetClass backingClass = currentVersion == version
            ? KisBackingBudgetClass::Current : KisBackingBudgetClass::RetainedHistory;
        decltype(publicationCoordinator.prepareDescriptorLocked(version, descriptor)) descriptors;
        const auto discardDescriptors = qScopeGuard([&] {
            if (locker->isLocked()) locker->unlock();
            descriptors = {};
            locker->relock();
        });
        try { descriptors = publicationCoordinator.prepareDescriptorLocked(version, descriptor); }
        catch (const std::bad_alloc &) {
            KisPageStoreDetail::setError(error, QStringLiteral("implicit default descriptor storage budget was refused"));
            return false;
        }
        KisPageBackingPreparation backing;
        ++activeProviderCalls;
        {
            locker->unlock();
            const auto done = qScopeGuard([&] { locker->relock(); --activeProviderCalls; });
            backing = writeCoordinator.reserveBacking(descriptor, access.domain, backingClass, error);
        }
        const auto discardBacking = qScopeGuard([&] {
            if (!backing.retirement) return;
            locker->unlock(); backing.retirement.reset(); locker->relock();
        });
        if (!backing.reservation.isValid()) return false;
        // Cold record allocation releases the owner gate. Another caller may
        // now own this key or the last preparation slot, or have materialized
        // it already. Revalidate the same admission policy before provider I/O.
        if (!awaitPreparation()) return false;
        if (isMaterialized()) {
            KisPageStoreDetail::setError(error, {});
            return true;
        }
        defaultStorage.beginPreparationLocked(version.key);
        ++activeProviderCalls;
        locker->unlock();
        const KisReplicaOperation allocation =
            provider->requestReplica(operation, version, descriptor, access.domain, KisPageAccessMode::Read, priority);
        locker->relock();
        --activeProviderCalls;
        defaultStorage.finishPreparationLocked(version.key);
        const bool ours = ownsPreparedReplica(allocation.replica, version, *provider);
        const bool backingOwned = ours && owner.registerBacking(
            allocation.replica, backing.reservation, backingClass, error, &backing.retirement);

        auto retireRejected = [&]() {
            if (!ours)
                return;
            retireRejectedReplicaLocked(allocation.replica, provider, std::move(backing), *locker);
        };

        QString failure;
        if (!operational || closed || !backingOwned || !allocation.isValid()
            || allocation.replica.domain != access.domain
            || !allocation.replica.layout.matches(descriptor)
            || !owner.verifyTerminalProviderResult(operation, allocation, &failure).succeeded()) {
            retireRejected();
            KisPageStoreDetail::setError(error, failure.isEmpty() ? QStringLiteral("implicit default allocation failed") : failure);
            return false;
        }

        // The provider call ran without the owner gate; publication may have
        // changed which default version is current in the meantime.
        if (!epochs.captureCommittedRoot().resolve(version.key, &currentVersion)) {
            retireRejected();
            KisPageStoreDetail::setError(error, QStringLiteral("implicit default lost its committed root"));
            return false;
        }
        const KisBackingBudgetClass installedClass = currentVersion == version
            ? KisBackingBudgetClass::Current : KisBackingBudgetClass::RetainedHistory;
        if (installedClass != backingClass) {
            if (!owner.reclassifyBacking(allocation.replica, installedClass, error)) {
                retireRejected();
                return false;
            }
            backingClass = installedClass;
        }

        // A competing caller may have completed between provider allocation
        // and registration. Keep exactly one authority and retire the loser.
        if (metadata.versionSnapshot(version, &existing)) {
            if (existing.version.isValid() && !existing.needsDefaultMaterialization(version)) {
                retireRejected();
                KisPageStoreDetail::setError(error, {});
                return true;
            }
            KisPageTransition attach;
            attach.kind = existing.version.isValid() ? KisPageTransitionKind::MaterializeDefault
                                          : KisPageTransitionKind::AttachHistoricalDefault;
            attach.version = version;
            attach.target = allocation.replica;
            const KisPageMetadataTransitionResult attached = metadata.applyOwner(version.key, attach);
            if (!attached.accepted) {
                retireRejected();
                KisPageStoreDetail::setError(error, attached.rejectionReason);
                return false;
            }
        } else {
            KisPageVersionStateSnapshot versionState;
            versionState.version = version;
            versionState.publication = KisPagePublicationState::Published;
            versionState.replicas.append({allocation.replica, KisReplicaValidity::Valid, {}, {}, 0, {}});
            versionState.authority = allocation.replica;

            KisPageStateSnapshot page;
            page.key = version.key;
            page.publishedEpoch = publishedEpoch;
            page.publishedGeneration = version.generation;
            page.publishedDefaultPixelRevision = version.defaultPixelRevision;
            page.nextGeneration = KisPageGeneration{version.generation.value + 1};
            page.versions.append(versionState);
            if (!metadata.registerPage(page, &failure)) {
                retireRejected();
                KisPageStoreDetail::setError(error, failure);
                return false;
            }
        }
        publicationCoordinator.installDescriptorAdditionsLocked(&descriptors);
        KisPageStoreDetail::setError(error, {});
        return true;
    }

    // Allocator copies retain the original controller through the final
    // control-block deallocation, including capabilities outliving the facade.
    struct MutationLifetime {
        explicit MutationLifetime(Private *value) : owner(value) { owner->lifetimeReferences.ref(); }
        MutationLifetime(const MutationLifetime &other) : MutationLifetime(other.owner) {}
        MutationLifetime &operator=(const MutationLifetime &other)
        {
            if (owner != other.owner) {
                other.owner->lifetimeReferences.ref();
                auto *previous = std::exchange(owner, other.owner);
                KisPageStore::PrivateReleaser::cleanup(previous);
            }
            return *this;
        }
        ~MutationLifetime() { KisPageStore::PrivateReleaser::cleanup(owner); }
        Private *owner;
    };
    template<class T> struct MutationAllocator : KisMutationStorageAllocator<T> {
        explicit MutationAllocator(Private *owner)
            : KisMutationStorageAllocator<T>(&owner->backingBudget), lifetime(owner) {}
        template<class U> MutationAllocator(const MutationAllocator<U> &other)
            : KisMutationStorageAllocator<T>(other.budget), lifetime(other.lifetime) {}
        template<class U> bool operator==(const MutationAllocator<U> &other) const noexcept
        { return lifetime.owner == other.lifetime.owner; }
        template<class U> bool operator!=(const MutationAllocator<U> &other) const noexcept
        { return !(*this == other); }
        MutationLifetime lifetime;
    };

    mutable QMutex mutex;
    QWaitCondition writeAdmissionChanged;
    QSharedPointer<DeferredMetadataCleanupStatistics> metadataCleanupStatistics =
        QSharedPointer<DeferredMetadataCleanupStatistics>::create();
    void releaseGenericWrite(KisPageTransactionId id, const KisPageKey &key)
    {
        writeAdmission.releaseDirectLocked(key, id.value);
        writeCoordinator.endGenericActivity(id);
    }
    QAtomicInt lifetimeReferences{1};
    KisPageReclamationJobPointer terminalCleanup;
    bool backingLimitsConfigured = false;
    bool operational = false;
    bool closing = false;
    bool closed = false;
    bool backgroundReclamation = true;
    qsizetype activeProviderCalls = 0;
    QString closeFailure;
    QSharedPointer<KisCompletionRegistry> completions;
    // A source accepted during cold configuration cannot be registered again
    // when its first ticket refuses. Retain that original receipt until ready.
    QSharedPointer<KisCompletionRegistry> configurationSourceOwner;
    quint64 configurationSource = 0;
    // Reused terminal evidence for successful synchronous host work; this is
    // not an operation sequence number and never mutates the registry.
    KisCompletionTicket readyHostCompletion;
    // Outlives metadata/owner so their final arena/backing charges can settle.
    KisBackingBudgetController backingBudget;
    KisPageOwnerLedger owner;
    KisPageMetadataCoordinator metadata;
    KisImageEpochReferenceModel epochs;
    KisPageWriteAdmission writeAdmission;
    KisPageWriteCoordinator writeCoordinator;
    KisPageDefaultStorage defaultStorage;
    KisPageRetirementQueue retirementQueue;
    KisPageHistoryCollector historyCollector;
    KisPagePublicationCoordinator publicationCoordinator;
    KisPageReadCoordinator readCoordinator;
    QSharedPointer<KisExactGenerationArchive> exactArchive;
    QHash<quint64, PendingArchiveRecord> pendingArchives;
    KisPageRetirementRecords pendingShutdownReplicas;
    std::map<quint64, PendingWriteRequestRecord> writeRequests;
    ActiveWriteMap activeWrites;
    KisPageMutationSession::Private *orphanedMutations = nullptr;
    // Transaction lifetime claims and per-page write claims, not request or
    // access-lease maps. The issued scope owns unpublished native records.
    KisPageMutationStatistics mutationStats;
    quint64 writeRequestsCreated = 0;
    quint64 synchronousHostWrites = 0;
};

class KisPageMutationSession::Private
{
public:
    explicit Private(KisBackingBudgetController *budget) : writes(budget), coldPages(budget) {}
    struct Page {
        KisReplicaHandle source;
        KisReplicaHandle target;
        const KisPageAllocationDescriptor *descriptor = nullptr;
        KisCpuWriteBindingReservation writable;
        QByteArray resetPixel;
        KisPreparedPageProof proof;
    };
    static_assert(sizeof(Page) <= 512);
    struct ColdPageSet {
        std::optional<Page> inlinePage;
        KisMutationStorage<Page, 2> overflow;
        explicit ColdPageSet(KisBackingBudgetController *budget = nullptr) : overflow(budget) {}

        Page *at(KisMutationWriteSet::EntryIndex index)
        {
            if (index == KisMutationWriteSet::InvalidEntry)
                return nullptr;
            if (index == 0)
                return inlinePage ? &*inlinePage : nullptr;
            return overflow.at(size_t(index - 1));
        }
        const Page *at(KisMutationWriteSet::EntryIndex index) const
        {
            return const_cast<ColdPageSet *>(this)->at(index);
        }
        quint32 create()
        {
            if (!inlinePage) {
                inlinePage.emplace();
                return 0;
            }
            const auto slot = overflow.prepareSlot();
            auto *page = overflow.emplacePrepared(slot);
            Q_ASSERT(page);
            return slot.index + 1;
        }
        void erase(KisMutationWriteSet::EntryIndex index)
        {
            if (index == 0)
                inlinePage.reset();
            else if (index != KisMutationWriteSet::InvalidEntry)
                overflow.erase(size_t(index - 1));
        }
    };
    ~Private() { Q_ASSERT(state == State::Detached); }
    static void destroy(Private *scope) noexcept
    {
        auto *owner = scope->owner;
        KisMutationStorageAllocator<Private> allocator(&owner->backingBudget);
        scope->~Private();
        allocator.deallocate(scope, 1);
        KisPageStore::PrivateReleaser::cleanup(owner);
    }
    static void retainOrphanLocked(Private *scope) noexcept
    {
        Q_ASSERT(!scope->nextOrphan);
        scope->nextOrphan = scope->owner->orphanedMutations;
        scope->owner->orphanedMutations = scope;
    }
    static void releaseOrRetain(Private *scope)
    {
        if (!scope->cancelLocked()) {
            QMutexLocker lock(&scope->owner->mutex);
            retainOrphanLocked(scope);
            return;
        }
        destroy(scope);
    }
    static void retryOrphans(KisPageStore::Private *owner, KisPageTransactionId transaction = {})
    {
        // Reuse each original scope's terminal link. Neither detaching a pass
        // nor retaining rejected cancellation needs a new allocation.
        Private *pending = nullptr;
        Private **tail = &pending;
        {
            QMutexLocker lock(&owner->mutex);
            for (auto **link = &owner->orphanedMutations; *link;) {
                Private *scope = *link;
                if (transaction.isValid() && !(scope->transaction.id == transaction)) {
                    link = &scope->nextOrphan;
                    continue;
                }
                *link = scope->nextOrphan;
                scope->nextOrphan = nullptr;
                *tail = scope;
                tail = &scope->nextOrphan;
            }
        }
        while (pending) {
            Private *scope = pending;
            pending = std::exchange(scope->nextOrphan, nullptr);
            QMutexLocker scopeLock(&scope->mutex);
            const bool released = scope->cancelLocked();
            scopeLock.unlock();
            if (released) {
                destroy(scope);
            } else {
                QMutexLocker lock(&owner->mutex);
                retainOrphanLocked(scope);
            }
        }
    }
    void detachLocked(QMutexLocker<QMutex> &lock)
    {
        finishSegmentLocked(lock, true);
    }
    void finishSegmentLocked(QMutexLocker<QMutex> &lock, bool terminal, bool retainAdmission = false)
    {
        Q_ASSERT(!retainAdmission || terminal);
        KisMutationWriteSet abandonedWrites;
        if (!retainAdmission) {
            admission.releaseLocked();
            abandonedWrites = std::move(writes);
        }
        // Adapter delivery retains these same entries and their ClaimSet,
        // never a copied range or a second permission. The sealed scope is
        // inactive; its destructor releases admission before entry storage,
        // while the original scope allocation still keeps owner alive.
        // Storage carries the original controller. Keep the session activity
        // and owner alive until its actual free/charge return is complete.
        ++owner->activeProviderCalls;
        lock.unlock();
        abandonedWrites = KisMutationWriteSet{};
        if (!terminal) {
            writes = KisMutationWriteSet(&owner->backingBudget);
            coldPages = ColdPageSet(&owner->backingBudget);
        }
        lock.relock();
        --owner->activeProviderCalls;
        if (terminal) owner->writeCoordinator.endSessionActivity(transaction.id);
        owner->mutationStats.guardsReleased += counters.guardsReleased;
        owner->mutationStats.writablePinsAcquired += counters.pinsAcquired;
        owner->mutationStats.writablePinsReleased += counters.pinsReleased;
        owner->mutationStats.pendingWriteMaterializations += counters.materializations;
        owner->mutationStats.pendingDefaultResetBytes += counters.defaultResetBytes;
        owner->mutationStats.pendingPayloadCopyBytes += counters.payloadCopyBytes;
        owner->mutationStats.maximumPinnedPagesPerExecution =
            qMax(owner->mutationStats.maximumPinnedPagesPerExecution, counters.maximumPins);
        Q_ASSERT(counters.activePins == 0 && counters.pinsAcquired == counters.pinsReleased);
        counters = {};
        if (terminal) state = State::Detached;
    }
    KisPageTransition writeTransition(KisMutationWriteSet::EntryIndex index, const Page &page) const
    {
        auto *entry = writes.at(index);
        Q_ASSERT(entry && entry->key() == page.target.version.key);
        return owner->writeCoordinator.writeTransition(*entry, transaction.id, page.source, page.target);
    }
    bool cancelPageLocked(KisMutationWriteSet::EntryIndex index, Page &page)
    {
        auto *entry = writes.at(index);
        Q_ASSERT(entry && !entry->isExposed());
        page.writable.reset();
        const auto result = owner->writeCoordinator.cancelPrivateWrite(writeTransition(index, page));
        if (!result.accepted)
            return false;
        owner->writeCoordinator.recordCancelled(*entry);
        if (page.proof.isValid())
            owner->owner.revokePreparedPage(page.proof);
        owner->publicationCoordinator.removeDescriptorLocked(page.target.version);
        if (entry->isCpuWrite())
            ++owner->mutationStats.pagesCancelled;
        return true;
    }
    void releaseStorageLocked(QMutexLocker<QMutex> &lock,
                              QSharedPointer<const KisPageReplicaSource> source = {})
    {
        auto hot = writes.takeReleasedBlocks();
        auto cold = coldPages.overflow.takeReleasedBlocks();
        if (hot.isEmpty() && cold.isEmpty() && !source)
            return;
        ++owner->activeProviderCalls;
        lock.unlock();
        source.clear();
        cold.reset();
        hot.reset();
        lock.relock();
        --owner->activeProviderCalls;
    }
    bool cancelLocked(QMutexLocker<QMutex> *heldOwnerLock = nullptr)
    {
        if (state == State::Detached || !owner)
            return true;
        if (executions) return false;
        for (auto slot = writes.firstEntry(); slot.isValid(); slot = writes.nextEntry(slot))
            if (writes.at(slot)->isExposed())
                return false;
        std::optional<QMutexLocker<QMutex>> acquired;
        if (!heldOwnerLock) {
            acquired.emplace(&owner->mutex);
            heldOwnerLock = &*acquired;
        }
        auto &lock = *heldOwnerLock;
        const auto retire = qScopeGuard([&] {
            releaseStorageLocked(lock);
            owner->processRetirementsLocked(lock);
        });
        for (auto slot = writes.firstEntry(); slot.isValid();) {
            const auto next = writes.nextEntry(slot);
            auto *entry = writes.at(slot);
            if (Page *page = pageAtEntry(slot.index)) {
                if (!cancelPageLocked(slot.index, *page)) {
                    state = State::Failed;
                    return false;
                }
            }
            // Also discard an admitted cold slot whose target preparation
            // failed. Such a slot is not returned by pageAtEntry().
            erasePageAtEntry(slot.index);
            const bool removal = entry->isRemoval();
            auto source = entry->initializationSource();
            entry->setInitializationSource({});
            // A rejected initial claim can leave an IntentOnly entry. Never
            // remove the other writer's claim when clearing that intent.
            owner->writeAdmission.releaseOneLocked(admission, slot);
            const bool erased = writes.erase(slot);
            Q_ASSERT(erased);
            owner->mutationStats.removalsCancelled += removal;
            releaseStorageLocked(lock, std::move(source));
            slot = next;
        }
        ColdPageSet abandonedPages;
        std::swap(coldPages, abandonedPages);
        ++owner->activeProviderCalls;
        lock.unlock();
        abandonedPages = ColdPageSet{};
        owner->retirementQueue.processAcceptedEffects(owner->backgroundReclamation);
        lock.relock();
        --owner->activeProviderCalls;
        detachLocked(lock);
        return true;
    }
    QMutex mutex;
    KisPageStore::Private *owner = nullptr;
    const KisPageStore *diagnosticOwner = nullptr; // identity only, never dereferenced
    KisPageTransaction transaction;
    QSharedPointer<KisPageReplicaProvider> provider;
    KisMutationWriteSet writes;
    ColdPageSet coldPages;
    KisPageWriteAdmission::ClaimSet admission;
    Page *ensurePage(KisMutationPageEntry &entry) try
    {
        if (auto *page = coldPages.at(entry.coldPage()))
            return page;
        const auto slot = coldPages.create();
        entry.setColdPage(slot);
        return coldPages.at(slot);
    } catch (const std::bad_alloc &) {
        return nullptr;
    }
    Page *pageAtEntry(KisMutationWriteSet::EntryIndex index)
    {
        return const_cast<Page *>(std::as_const(*this).pageAtEntry(index));
    }
    const Page *pageAtEntry(KisMutationWriteSet::EntryIndex index) const
    {
        const auto *entry = writes.at(index);
        const auto *page = entry ? coldPages.at(entry->coldPage()) : nullptr;
        return page && page->target.isValid() ? page : nullptr;
    }
    void erasePageAtEntry(KisMutationWriteSet::EntryIndex index)
    {
        auto *entry = writes.at(index);
        Q_ASSERT(entry);
        coldPages.erase(entry->coldPage());
        entry->setColdPage(KisMutationWriteSet::InvalidEntry);
    }
    void clearSources()
    {
        for (auto slot = writes.firstEntry(); slot.isValid(); slot = writes.nextEntry(slot))
            writes.at(slot)->setInitializationSource({});
    }
    bool claimEntryLocked(const KisPageWriteIntent &intent, QMutexLocker<QMutex> &lock,
                          KisPageWriteAdmission::ClaimOrigin origin,
                          QString *error = nullptr) try
    {
        ++owner->activeProviderCalls;
        const auto preparation = qScopeGuard([&] {
            if (!lock.isLocked()) lock.relock();
            --owner->activeProviderCalls;
        });
        // The caller holds this session's mutex, so the sole write set cannot
        // change while its actual storage is prepared outside the store gate.
        if (!writes.find(intent.key)) {
            lock.unlock();
            writes.getOrCreate(intent);
            lock.relock();
        }
        if (!admission.isValid()) admission = owner->writeAdmission.beginClaimSet(writes);
        return owner->writeAdmission.claimOne(admission, writes.handleAt(writes.findIndex(intent.key)),
                                              lock, error, origin);
    } catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("mutation entry/admission storage is unavailable"));
        state = State::Failed;
        return false;
    }
    bool selectCpuProviderLocked(QString *error)
    {
        if (provider)
            return true;
        const KisPageAccessRequirement cpu{
            KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
        const auto selected = owner->owner.providerFor(cpu);
        if (!selected || !selected->capabilities().synchronousOperations
            || !selected->capabilities().nativeCpuMutation
            || !selected->capabilities().synchronousWriteCopy) {
            KisPageStoreDetail::setError(error, QStringLiteral(
                "native CPU mutation provider is unavailable or ambiguous"));
            return false;
        }
        provider = selected;
        ++owner->mutationStats.sessionsCreated;
        return true;
    }
    bool stageSemantic(const KisPageKey &key,
                       const QSharedPointer<const KisPageReplicaSource> &source,
                       bool removal, QString *error)
    {
        QMutexLocker scopeLock(&mutex);
        if (state != State::Active || executions || thread != QThread::currentThreadId() || !key.isValid()
            || (!removal && !source)) {
            KisPageStoreDetail::setError(error, removal ? QStringLiteral("mutation is unavailable or used from another thread")
                                    : QStringLiteral("alias mutation/input is unavailable"));
            return false;
        }
        auto entryIndex = writes.findIndex(key);
        auto *entry = writes.at(entryIndex);
        if (entry && entry->isExposed()) {
            KisPageStoreDetail::setError(error, removal ? QStringLiteral("removal still has a writable guard")
                                    : QStringLiteral("alias still has a writable guard"));
            return false;
        }
        QMutexLocker lock(&owner->mutex);
        const auto overlay = KisPageReadView::transactionOverlay(transaction.id);
        KisSurfaceEpochState surface;
        bool available = (entry || !owner->writeAdmission.pageClaimedLocked(key))
            && owner->publicationCoordinator.resolveSurfaceLocked(key.surface, overlay, &surface);
        if (!removal && available) {
            const auto producer = owner->owner.provider(source->provider(), source->providerEpoch());
            available = producer && producer->capabilities().synchronousSourceAdoption
                && surface.allocationDescriptor() == source->descriptor();
        }
        if (!available) {
            state = State::Failed;
            KisPageStoreDetail::setError(error, removal ? QStringLiteral("removal page is claimed or its surface is unavailable")
                                    : QStringLiteral("alias key, provider or source layout is unavailable"));
            return false;
        }
        KisPageWriteIntent intent;
        intent.key = key;
        intent.inputKind = KisPageWriteInputKind::Semantic;
        if (!claimEntryLocked(intent, lock, KisPageWriteAdmission::ClaimOrigin::NativeSession, error)) {
            state = State::Failed;
            return false;
        }
        entryIndex = writes.findIndex(key);
        entry = writes.at(entryIndex);
        Q_ASSERT(entry);
        const auto plan = owner->writeCoordinator.select(intent, entry);
        Page *pending = plan == KisPageWritePlanKind::ReusePending ? pageAtEntry(entryIndex) : nullptr;
        Q_ASSERT(plan == KisPageWritePlanKind::SemanticOnly || pending);
        if (removal) {
            entry->setRemoval(true);
            entry->setInitializationSource({});
            if (pending) pending->resetPixel = surface.format.defaultPixel;
            // A sealed proof remains visible until this segment seals.
        } else {
            entry->setInitializationSource(source);
            entry->setRemoval(false);
            if (pending) pending->resetPixel.clear();
        }
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    bool claimsHeldLocked() const
    {
        // The segment's lifetime reference and transaction claim protect the
        // provider registry, base and private Prepared versions across unlock.
        // Per-key claims exclude other writers/removals/adoptions, not readers
        // of earlier sealed versions. Detachment applies to current metadata;
        // reader protection must never be overwritten by a stale snapshot.
        if (!owner->operational || !owner->writeCoordinator.transactionHasSession(transaction.id))
            return false;
        return writes.size() == 0 || owner->writeAdmission.ownsClaimSetLocked(admission);
    }
    bool prepareAliasLocked(const KisPageKey &key,
                            const QSharedPointer<const KisPageReplicaSource> &source,
                            QMutexLocker<QMutex> &lock,
                            QString *error)
    {
        const auto producer = owner->owner.provider(source->provider(), source->providerEpoch());
        KisPageWriteIntent intent;
        intent.key = key;
        intent.mode = KisPageWriteMode::DiscardContents;
        intent.inputKind = KisPageWriteInputKind::Semantic;
        const auto entryIndex = writes.findIndex(key);
        auto *entry = writes.at(entryIndex);
        Q_ASSERT(entry);
        const auto plan = owner->writeCoordinator.select(intent, entry);
        Page *page = plan == KisPageWritePlanKind::ReusePending ? pageAtEntry(entryIndex) : nullptr;
        Q_ASSERT(plan == KisPageWritePlanKind::SemanticOnly || page);
        const auto pending = page ? writeTransition(entryIndex, *page) : KisPageTransition{};
        KisPageTransition adoption;
        KisPageAllocationDescriptor descriptor;
        if (!producer || !producer->capabilities().synchronousSourceAdoption
            || !owner->writeCoordinator.prepareWriteBaseLocked(transaction, intent, owner->publicationCoordinator,
                                                               &adoption, &descriptor, error, page ? &pending : nullptr)
            || !(descriptor == source->descriptor()))
            return false;
        adoption.kind = plan == KisPageWritePlanKind::ReusePending
            ? KisPageTransitionKind::ReplacePrivatePreparedBacking
            : KisPageTransitionKind::AdoptPreparedWrite;
        auto *resources = ensurePage(*entry);
        if (!resources) {
            KisPageStoreDetail::setError(error, QStringLiteral("mutation cold storage is unavailable"));
            return false;
        }
        decltype(owner->publicationCoordinator.prepareDescriptorLocked(adoption.version, descriptor)) descriptors;
        const auto discardDescriptors = qScopeGuard([&] {
            if (lock.isLocked()) lock.unlock();
            descriptors = {};
            lock.relock();
        });
        if (!page) {
            try { descriptors = owner->publicationCoordinator.prepareDescriptorLocked(adoption.version, descriptor); }
            catch (const std::bad_alloc &) {
                KisPageStoreDetail::setError(error, QStringLiteral("alias descriptor storage budget was refused"));
                return false;
            }
        }
        KisPageBackingPreparation backing;
        ++owner->activeProviderCalls;
        {
            lock.unlock();
            const auto done = qScopeGuard([&] { lock.relock(); --owner->activeProviderCalls; });
            backing = owner->writeCoordinator.reserveBacking(descriptor, KisPageAccessDomain::CpuRam,
                KisBackingBudgetClass::ActivePending, error, adoption.version);
        }
        const auto discardBacking = qScopeGuard([&] {
            if (!backing.retirement) return;
            lock.unlock(); backing.retirement.reset(); lock.relock();
        });
        if (!backing.reservation.isValid()) return false;
        ++owner->activeProviderCalls;
        lock.unlock();
        KisReplicaOperation allocation;
        {
            KisPageStoreDiagnosticTimer phase(diagnosticOwner, KisPageStoreDiagnosticPhase::MutationAliasPrepare, 1);
            allocation = producer->prepareSynchronousSource(adoption.operation,
                                                            source,
                                                            adoption.version,
                                                            descriptor,
                                                            KisReplicaSourceUse::ImmutableAlias,
                                                            KisPagePriority::Interactive);
        }
        lock.relock();
        --owner->activeProviderCalls;
        const bool reusesPendingSlot = page &&
            allocation.replica.physicalSlotIdentity() == page->target.physicalSlotIdentity();
        const bool ours = ownsPreparedReplica(allocation.replica, adoption.version, *producer)
            && !reusesPendingSlot; // a rejected reused slot is still owned by the old private target
        const bool backingOwned = ours && owner->owner.registerBacking(
            allocation.replica, backing.reservation, KisBackingBudgetClass::ActivePending, error, &backing.retirement);
        const auto reject = [&] {
            if (!ours)
                return;
            owner->retireRejectedReplicaLocked(allocation.replica, producer, std::move(backing), lock);
        };
        if (!backingOwned || allocation.status != KisPageRequestStatus::Ready
            || allocation.replica.domain != KisPageAccessDomain::CpuRam
            || !allocation.replica.layout.matches(descriptor)
            || !owner->owner.verifyTerminalProviderResult(adoption.operation, allocation, error).succeeded()) {
            reject();
            return false;
        }
        adoption.target = allocation.replica;
        const auto applied = owner->metadata.applyOwner(key, adoption);
        if (!applied.accepted) {
            reject();
            KisPageStoreDetail::setError(error, applied.rejectionReason);
            return false;
        }
        if (!page) {
            page = resources;
            page->source = adoption.source;
            page->target = adoption.target;
            owner->writeCoordinator.recordPrepared(*entry, intent, adoption);
            owner->publicationCoordinator.installDescriptorAdditionsLocked(&descriptors);
            page->descriptor = owner->publicationCoordinator.descriptorLocked(adoption.version);
            Q_ASSERT(page->descriptor);
            ++owner->mutationStats.aliasGenerationsReserved;
        } else {
            page->writable.reset();
            page->target = adoption.target;
        }
        return true;
    }
    KisPageMutationExecution::Private *executions = nullptr;
    Private *nextOrphan = nullptr;
    enum class State : quint8 { Active, Failed, Detached };
    Qt::HANDLE thread = QThread::currentThreadId();
    std::atomic<State> state{State::Detached};
    struct Counters {
        quint64 guardsReleased = 0;
        quint64 pinsAcquired = 0;
        quint64 pinsReleased = 0;
        quint64 activePins = 0;
        quint64 maximumPins = 0;
        quint64 materializations = 0;
        quint64 defaultResetBytes = 0;
        quint64 payloadCopyBytes = 0;
        void merge(const Counters &other)
        {
            Q_ASSERT(other.activePins == 0 && other.pinsAcquired == other.pinsReleased);
            guardsReleased += other.guardsReleased;
            pinsAcquired += other.pinsAcquired;
            pinsReleased += other.pinsReleased;
            maximumPins = qMax(maximumPins, other.maximumPins);
            materializations += other.materializations;
            defaultResetBytes += other.defaultResetBytes;
            payloadCopyBytes += other.payloadCopyBytes;
        }
    } counters;
    void *pinWritable(KisCpuWriteBindingReservation &writable, Counters &local)
    {
        KisPageStoreDiagnosticTimer phase(diagnosticOwner, KisPageStoreDiagnosticPhase::WriteWritablePin, 1);
        KisCpuResidentReadStatus status;
        void *data = writable.pinResident(&status);
        if (!data && status == KisCpuResidentReadStatus::NonResident) {
            // This is real pending backing restoration, not another first
            // write, COW or request. No owner gate is held. Current tiles3
            // storage uses its synchronous swap control path here.
            phase.next(KisPageStoreDiagnosticPhase::WritePendingMaterialize, 1);
            ++local.materializations;
            data = writable.materialize();
        }
        return data;
    }
};

class KisPageMutationExecution::Private
{
public:
    // Binding an original stable slot under the structure gate validates its
    // incarnation once. This linked borrow then excludes erase/reuse until
    // its last guard returns. No second key/pending authority or pixel pointer.
    struct BoundEntry {
        KisMutationPageEntry *entry = nullptr;
        KisPageMutationSession::Private::Page *page = nullptr;
    };
    static_assert(sizeof(BoundEntry) <= 16);
    Private(const std::shared_ptr<KisPageMutationSession::Private> &owner, size_t size)
        : scope(owner), overflow(KisMutationStorageAllocator<BoundEntry>(&owner->owner->backingBudget)), count(size),
          touchedOverflow(KisMutationStorageAllocator<quint64>(&owner->owner->backingBudget))
    {
        if (count > 1) overflow.resize(count);
        if (count > 64) touchedOverflow.resize(2 * ((count + 63) / 64));
    }
    ~Private()
    {
        if (!linked) return;
        QMutexLocker lock(&scope->mutex);
        Q_ASSERT(counters.activePins == 0);
        if (!finished) scope->state = KisPageMutationSession::Private::State::Failed;
        scope->counters.merge(counters);
        if (previous) previous->next = next;
        else scope->executions = next;
        if (next) next->previous = previous;
        // Storage and original control block free after leaving the gate.
        // scope is declared first and released last.
    }
    static auto order(const KisPageKey &key)
    { return std::make_tuple(key.surface.value, key.page.column, key.page.row); }
    static bool less(const BoundEntry &a, const BoundEntry &b)
    { return order(a.entry->key()) < order(b.entry->key()); }
    BoundEntry *entries() { return count == 1 ? &inlineEntry : overflow.data(); }
    quint64 *touched() { return count <= 64 ? &inlineTouched : touchedOverflow.data(); }
    quint64 *prepared()
    {
        return count <= 64 ? &inlinePrepared : touchedOverflow.data() + (count + 63) / 64;
    }
    void markPrepared(size_t index) noexcept
    { prepared()[index / 64] |= quint64(1) << (index % 64); }
    void markTouched(size_t index) noexcept
    {
        const quint64 mask = quint64(1) << (index % 64);
        prepared()[index / 64] &= ~mask;
        auto &word = touched()[index / 64];
        if (!(word & mask)) { word |= mask; ++touchedCount; }
    }
    BoundEntry *find(const KisPageKey &key)
    {
        auto *first = entries();
        auto *found = std::lower_bound(first, first + count, key,
            [](const BoundEntry &entry, const KisPageKey &key) { return order(entry.entry->key()) < order(key); });
        return found != first + count && found->entry->key() == key ? found : nullptr;
    }
    std::shared_ptr<KisPageMutationSession::Private> scope;
    QMutex mutex; // one execution, independent of structural growth/cold work
    BoundEntry inlineEntry;
    std::vector<BoundEntry, KisMutationStorageAllocator<BoundEntry>> overflow;
    size_t count = 0;
    // One bit per original BoundEntry, not a duplicate page-key index. The
    // execution's original control block/controller also own this storage.
    quint64 inlineTouched = 0;
    // Temporary ownership only: pages prepared for this execution but never
    // exposed by its callback are cancelled before result export.
    quint64 inlinePrepared = 0;
    std::vector<quint64, KisMutationStorageAllocator<quint64>> touchedOverflow;
    qsizetype touchedCount = 0;
    Qt::HANDLE thread = QThread::currentThreadId();
    Private *previous = nullptr;
    Private *next = nullptr;
    KisPageMutationSession::Private::Counters counters;
    bool linked = false;
    bool finished = false;
};

KisPageMutationExecution KisPageMutationSession::borrowExecution(
    KisSurfaceId surface, const QSet<KisLogicalPageId> &pages, QString *error) try
{
    KisPageMutationExecution result;
    if (!d || !surface.isValid() || pages.isEmpty()) {
        KisPageStoreDetail::setError(error, QStringLiteral("execution range is absent"));
        return result;
    }
    // Actual record, control block and range storage precede any authority.
    auto candidate = std::allocate_shared<KisPageMutationExecution::Private>(
        KisPageStore::Private::MutationAllocator<KisPageMutationExecution::Private>(d->owner), d, size_t(pages.size()));
    // Only original entry/cold-page bindings survive admission. Rollback uses
    // a transient charged bitmap, not another persistent PageKey/slot index.
    quint64 inlineCreated = 0;
    std::vector<quint64, KisMutationStorageAllocator<quint64>> createdOverflow{
        KisMutationStorageAllocator<quint64>(&d->owner->backingBudget)};
    if (pages.size() > 64) createdOverflow.resize((size_t(pages.size()) + 63) / 64);
    auto *created = pages.size() <= 64 ? &inlineCreated : createdOverflow.data();
    QMutexLocker scopeLock(&d->mutex);
    if (d->state != Private::State::Active || (!d->executions && d->counters.activePins)) {
        KisPageStoreDetail::setError(error, QStringLiteral("mutation cannot lend execution"));
        return result;
    }
    auto *range = candidate->entries();
    for (auto *active = d->executions; active; active = active->next)
        for (const auto &page : pages)
            if (active->find({surface, page})) {
                KisPageStoreDetail::setError(error, QStringLiteral("execution range overlaps an outstanding borrow"));
                return result;
            }
    bool accepted = false;
    const auto rollback = qScopeGuard([&] {
        if (accepted) return;
        for (size_t i = 0; i < candidate->count; ++i)
            if ((created[i / 64] & (quint64(1) << (i % 64))) && range[i].entry) {
                const bool erased = d->writes.erase(d->writes.handleAt(d->writes.findIndex(range[i].entry->key())));
                Q_ASSERT(erased);
            }
        d->writes.takeReleasedBlocks().reset();
    });
    // Only structure/cold preparation uses this gate. Bound entries stay at
    // their original addresses across unrelated directory/index growth.
    size_t i = 0;
    for (const auto &page : pages) {
        KisPageWriteIntent intent; intent.key = {surface, page};
        const bool newEntry = !d->writes.find(intent.key);
        auto &entry = d->writes.getOrCreate(intent);
        const auto slot = d->writes.handleAt(d->writes.findIndex(intent.key));
        Q_ASSERT(d->writes.at(slot) == &entry);
        range[i] = {&entry, d->pageAtEntry(slot.index)};
        if (newEntry) created[i / 64] |= quint64(1) << (i % 64);
        ++i;
    }
    QMutexLocker ownerLock(&d->owner->mutex);
    KisSurfaceEpochState surfaceState;
    if (!d->owner->operational
        || !d->owner->publicationCoordinator.resolveSurfaceLocked(surface,
            KisPageReadView::transactionOverlay(d->transaction.id), &surfaceState)) {
        KisPageStoreDetail::setError(error, QStringLiteral("execution surface is unavailable"));
        return result;
    }
    ++d->owner->activeProviderCalls;
    const auto preparation = qScopeGuard([&] { --d->owner->activeProviderCalls; });
    if (!d->admission.isValid()) d->admission = d->owner->writeAdmission.beginClaimSet(d->writes);
    if (!d->owner->writeAdmission.claimRange(d->admission, surface, pages, ownerLock, error))
        return result;
    std::sort(range, range + candidate->count, &KisPageMutationExecution::Private::less);
    candidate->next = d->executions;
    if (candidate->next) candidate->next->previous = candidate.get();
    d->executions = candidate.get();
    candidate->linked = true;
    accepted = true;
    result.d = std::move(candidate);
    KisPageStoreDetail::setError(error, {});
    return result;
} catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("execution range storage is unavailable"));
    return {};
}

KisPageMutationExecution::~KisPageMutationExecution() { abandon(); }
KisPageMutationExecution &KisPageMutationExecution::operator=(KisPageMutationExecution &&other) noexcept
{
    if (this != &other) { abandon(); d = std::move(other.d); }
    return *this;
}
void KisPageMutationExecution::abandon()
{
    if (d) {
        { QMutexLocker lock(&d->mutex);
          if (!d->finished) d->scope->state = KisPageMutationSession::Private::State::Failed; }
        d.reset();
    }
}
bool KisPageMutationExecution::isActive() const
{
    if (!d) return false;
    QMutexLocker lock(&d->mutex);
    return !d->finished && d->scope->state == KisPageMutationSession::Private::State::Active;
}
qsizetype KisPageMutationExecution::finishPreparedWrites(
    QVector<KisLogicalPageId> *changed, QString *error)
{
    if (!d) return -1;
    QMutexLocker executionLock(&d->mutex);
    if (d->finished || d->thread != QThread::currentThreadId() || d->counters.activePins ||
        d->scope->state != KisPageMutationSession::Private::State::Active) return -1;

    bool hasUntouchedPreparation = false;
    const size_t wordCount = (d->count + 63) / 64;
    for (size_t i = 0; i < wordCount; ++i)
        hasUntouchedPreparation |= d->prepared()[i] != 0;
    if (hasUntouchedPreparation) {
        QMutexLocker scopeLock(&d->scope->mutex);
        auto *scope = d->scope.get();
        auto *owner = scope->owner;
        QMutexLocker ownerLock(&owner->mutex);
        const auto retire = qScopeGuard([&] {
            scope->releaseStorageLocked(ownerLock);
            owner->processRetirementsLocked(ownerLock);
        });
        for (size_t i = 0; i < d->count; ++i) {
            const quint64 mask = quint64(1) << (i % 64);
            if (!(d->prepared()[i / 64] & mask))
                continue;
            d->prepared()[i / 64] &= ~mask;
            auto &bound = d->entries()[i];
            const auto index = scope->writes.findIndex(bound.entry->key());
            auto *page = scope->pageAtEntry(index);
            if (!page)
                continue;
            if (!scope->cancelPageLocked(index, *page)) {
                scope->state = KisPageMutationSession::Private::State::Failed;
                KisPageStoreDetail::setError(
                    error, QStringLiteral("operation write preparation could not be rolled back"));
                return -1;
            }
            scope->erasePageAtEntry(index);
            bound.page = nullptr;
        }
    }

    if (changed) {
        if (!changed->isEmpty() || !changed->isDetached() ||
            changed->capacity() < qsizetype(d->count)) return -1;
        for (size_t i = 0; i < d->count; ++i)
            if (d->touched()[i / 64] & (quint64(1) << (i % 64)))
                changed->append(d->entries()[i].entry->key().page);
    }
    KisPageStoreDetail::setError(error, {});
    return d->touchedCount;
}

KisPageMutationExecution::PreparationResult
KisPageMutationExecution::prepareWrites(QString *error)
{
    if (!d) {
        KisPageStoreDetail::setError(error, QStringLiteral("execution range is unavailable"));
        return PreparationResult::Failed;
    }

    {
        QMutexLocker executionLock(&d->mutex);
        QMutexLocker scopeLock(&d->scope->mutex);
        if (d->finished || d->thread != QThread::currentThreadId()
            || d->counters.activePins
            || d->scope->state != KisPageMutationSession::Private::State::Active) {
            KisPageStoreDetail::setError(error, QStringLiteral("execution range cannot be prepared"));
            return PreparationResult::Failed;
        }
        for (size_t i = 0; i < d->count; ++i) {
            const auto *entry = d->entries()[i].entry;
            if (!entry || entry->isRemoval() || entry->initializationSource()) {
                KisPageStoreDetail::setError(error, {});
                return PreparationResult::Unsupported;
            }
        }
    }

    KisPageMutationSession session;
    session.d = std::shared_ptr<KisPageMutationSession::Private>(d, d->scope.get());
    bool prepared = true;
    for (size_t i = 0; i < d->count; ++i) {
        KisPageKey key;
        {
            QMutexLocker lock(&d->mutex);
            if (d->finished || d->thread != QThread::currentThreadId()
                || d->scope->state != KisPageMutationSession::Private::State::Active) {
                KisPageStoreDetail::setError(error, QStringLiteral("execution range changed during preparation"));
                prepared = false;
                break;
            }
            const auto &bound = d->entries()[i];
            key = bound.entry->key();
            if (!bound.page)
                d->markPrepared(i);
        }
        auto guard = session.beginWriteImpl(key, KisPageWriteMode::PreserveContents,
                                            nullptr, error, d, true);
        if (!guard.isValid()) {
            prepared = false;
            break;
        }
        guard = {};
    }

    if (prepared) {
        KisPageStoreDetail::setError(error, {});
        return PreparationResult::Ready;
    }

    const QString failure = error ? *error : QString{};
    QString rollbackError;
    if (finishPreparedWrites(nullptr, &rollbackError) < 0) {
        KisPageStoreDetail::setError(error, rollbackError);
    } else {
        KisPageStoreDetail::setError(error, failure);
    }
    return PreparationResult::Failed;
}

bool KisPageMutationExecution::finish(QString *error)
{
    if (!d) return false;
    bool succeeded;
    {
        QMutexLocker lock(&d->mutex);
        if (d->thread != QThread::currentThreadId() || d->counters.activePins) {
            KisPageStoreDetail::setError(error, QStringLiteral("execution still has guards or belongs to another worker"));
            return false;
        }
        succeeded = d->scope->state == KisPageMutationSession::Private::State::Active;
        d->finished = true;
    }
    d.reset();
    KisPageStoreDetail::setError(error, succeeded ? QString{} : QStringLiteral("execution's mutation failed"));
    return succeeded;
}
KisCpuWriteGuard KisPageMutationExecution::beginWrite(const KisPageKey &key, QString *error)
{ return beginWrite(key, KisPageWriteMode::PreserveContents, error); }
KisCpuWriteGuard KisPageMutationExecution::beginWrite(const KisPageKey &key, KisPageWriteMode mode, QString *error)
{
    KisPageMutationSession session;
    if (d) session.d = std::shared_ptr<KisPageMutationSession::Private>(d, d->scope.get());
    return session.beginWriteImpl(key, mode, nullptr, error, d);
}
bool KisPageMutationExecution::overwritePage(const KisPageKey &key, const KisCpuPagePayload &payload, QString *error)
{
    KisPageMutationSession session;
    if (d) session.d = std::shared_ptr<KisPageMutationSession::Private>(d, d->scope.get());
    return session.beginWriteImpl(key, KisPageWriteMode::DiscardContents, &payload, error, d).isValid();
}

KisPageMutationSession::~KisPageMutationSession() = default;
bool KisPageMutationSession::isActive() const
{
    if (!d)
        return false;
    QMutexLocker lock(&d->mutex);
    return d->state == Private::State::Active;
}

static_assert(sizeof(KisCpuWriteGuard) <= 128);
KisCpuWriteGuard::~KisCpuWriteGuard()
{
    reset();
}
KisCpuWriteGuard &KisCpuWriteGuard::operator=(KisCpuWriteGuard &&other) noexcept
{
    if (this != &other) {
        reset();
        m_scope = std::move(other.m_scope);
        m_execution = std::exchange(other.m_execution, nullptr);
        m_entry = std::exchange(other.m_entry, KisMutationWriteSet::InvalidEntry);
        m_entryIncarnation = std::exchange(other.m_entryIncarnation, 0);
        m_data = std::exchange(other.m_data, nullptr);
        m_rowStride = std::exchange(other.m_rowStride, 0);
        m_byteSize = std::exchange(other.m_byteSize, 0);
    }
    return *this;
}
void KisCpuWriteGuard::reset()
{
    if (m_scope && m_data) {
        QMutexLocker lock(m_execution ? &m_execution->mutex : &m_scope->mutex);
        auto *bound = m_execution ? &m_execution->entries()[m_entry] : nullptr;
        auto *entry = bound ? bound->entry : m_scope->writes.at({m_entry, m_entryIncarnation});
        Q_ASSERT(entry && entry->isExposed());
        auto *page = bound ? bound->page : entry && entry->isExposed() ? m_scope->pageAtEntry(m_entry) : nullptr;
        Q_ASSERT(page);
        if (page) {
            // Drop only pixel residency, not the unpublished writer token.
            // The next guard must re-pin; no parked Page retains a pointer.
            page->writable.unpin();
            m_scope->owner->writeCoordinator.recordExposure(*entry, false);
            auto &counters = m_execution ? m_execution->counters : m_scope->counters;
            ++counters.guardsReleased;
            ++counters.pinsReleased;
            Q_ASSERT(counters.activePins);
            --counters.activePins;
        }
    }
    m_data = nullptr;
    m_entry = KisMutationWriteSet::InvalidEntry;
    m_entryIncarnation = 0;
    m_rowStride = 0;
    m_byteSize = 0;
    m_execution = nullptr;
    m_scope.reset();
}

KisPageVersion KisCpuWriteGuard::version() const
{
    if (!m_scope || !m_data)
        return {};
    QMutexLocker lock(m_execution ? &m_execution->mutex : &m_scope->mutex);
    const auto *bound = m_execution ? &m_execution->entries()[m_entry] : nullptr;
    const auto *entry = bound ? bound->entry : m_scope->writes.at({m_entry, m_entryIncarnation});
    return entry ? entry->preparedTargetVersion() : KisPageVersion{};
}

bool KisCpuWriteGuard::providerBacking(KisReplicaHandle *replica) const
{
    if (replica)
        *replica = {};
    if (!m_scope || !m_data || !replica)
        return false;
    QMutexLocker lock(m_execution ? &m_execution->mutex : &m_scope->mutex);
    const auto *bound = m_execution ? &m_execution->entries()[m_entry] : nullptr;
    const auto *entry = bound ? bound->entry : m_scope->writes.at({m_entry, m_entryIncarnation});
    const auto *page = bound ? bound->page : entry ? m_scope->pageAtEntry(m_entry) : nullptr;
    if (!page || !entry->isExposed() || !page->target.isValid()
        || !page->writable.isValid())
        return false;
    *replica = page->target;
    return true;
}

KisPageMutationSession KisPageStore::beginMutation(const KisPageTransaction &transaction, QString *error) try
{
    KisPageMutationSession result;
    std::shared_ptr<KisPageMutationSession::Private> candidate;
    QMutexLocker lock(&d->mutex);
    const auto available = [&] {
        const auto state = d->epochs.activeTransaction(transaction.id);
        return d->operational && transaction.isValid()
            && !d->publicationCoordinator.isPreparingCommitLocked(transaction.id)
            && state == transaction;
    };
    if (!available()) {
        KisPageStoreDetail::setError(error, QStringLiteral("mutation transaction is unavailable"));
        return result;
    }
    ++d->activeProviderCalls;
    const auto preparation = qScopeGuard([&] {
        if (lock.isLocked()) lock.unlock();
        candidate.reset(); // inert or rejected candidate, before active publication
        lock.relock();
        --d->activeProviderCalls;
    });
    lock.unlock();
    using Scope = KisPageMutationSession::Private;
    KisMutationStorageAllocator<Scope> allocator(&d->backingBudget);
    Scope *scope = allocator.allocate(1);
    try {
        new (scope) Scope(&d->backingBudget);
    } catch (...) {
        allocator.deallocate(scope, 1);
        throw;
    }
    scope->owner = d.data();
    d->lifetimeReferences.ref(); // the original scope also survives orphan retention
    candidate = std::shared_ptr<Scope>(scope, &Scope::releaseOrRetain, Private::MutationAllocator<Scope>(d.data()));
    lock.relock();
    if (!available()) {
        KisPageStoreDetail::setError(error, QStringLiteral("mutation transaction changed during preparation"));
        return result;
    }
    if (!d->writeCoordinator.beginSessionActivity(transaction.id, lock, error)) return result;
    if (!available()) {
        d->writeCoordinator.endSessionActivity(transaction.id);
        KisPageStoreDetail::setError(error, QStringLiteral("mutation transaction changed during admission"));
        return result;
    }
    candidate->diagnosticOwner = this;
    candidate->transaction = transaction;
    candidate->state = Scope::State::Active;
    result.d = std::move(candidate);
    ++d->mutationStats.operationSessionsCreated;
    KisPageStoreDetail::setError(error, {});
    return result;
} catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("mutation scope storage is unavailable"));
    return {};
}

std::unique_ptr<KisPageStoreWriteReservation> KisPageStore::reserveManagedRange(
    KisSurfaceId surface, const QSet<KisLogicalPageId> &targets,
    bool legacyIntent, bool *borrowed, QString *error) try
{
    if (borrowed)
        *borrowed = false;
    if (!surface.isValid() || targets.isEmpty()) {
        KisPageStoreDetail::setError(error, QStringLiteral("managed mutation range is invalid"));
        return {};
    }
    auto range = std::unique_ptr<KisPageStoreWriteReservation>(
        new (&d->backingBudget, d.data(),
             +[](void *owner) { static_cast<Private *>(owner)->lifetimeReferences.ref(); },
             &Private::releaseRetirementLifetime) KisPageStoreWriteReservation(&d->backingBudget));
    range->writes.reserveKnownTargetCount(targets.size());
    for (const KisLogicalPageId &page : targets) {
        const KisPageKey key{surface, page};
        if (!key.isValid()) {
            KisPageStoreDetail::setError(error, QStringLiteral("managed mutation has an invalid target"));
            return {};
        }
        KisPageWriteIntent intent;
        intent.key = key;
        intent.inputKind = KisPageWriteInputKind::Semantic;
        range->writes.getOrCreate(intent);
    }
    QMutexLocker lock(&d->mutex);
    ++d->activeProviderCalls;
    const auto preparation = qScopeGuard([&] { --d->activeProviderCalls; });
    while (d->operational) {
        bool mustWait = false;
        bool legacyBorrowed = legacyIntent;
        bool sameThreadReentry = false;
        for (const KisLogicalPageId &page : targets) {
            const auto conflict = d->writeAdmission.conflictLocked(
                {surface, page}, QThread::currentThreadId());
            sameThreadReentry |= conflict == KisPageWriteAdmission::Conflict::ManagedSameThread;
            legacyBorrowed |= conflict == KisPageWriteAdmission::Conflict::LegacyBorrower
                || conflict == KisPageWriteAdmission::Conflict::LegacySameThread;
            mustWait |= conflict == KisPageWriteAdmission::Conflict::ManagedOtherThread;
        }
        if (sameThreadReentry) {
            KisPageStoreDetail::setError(error, QStringLiteral("pixel operation cannot reenter its target range"));
            return {};
        }
        if (legacyBorrowed) {
            if (borrowed) *borrowed = true;
            KisPageStoreDetail::setError(error, QStringLiteral("pixel operation intersects a legacy writer"));
            return {};
        }
        if (!mustWait) break;
        KisPageStoreDiagnosticTimer waitPhase(
            this, KisPageStoreDiagnosticPhase::PixelOperationRangePrepare, 0);
        waitPhase.next(KisPageStoreDiagnosticPhase::PixelOperationRangeWait, 1);
        d->writeAdmissionChanged.wait(&d->mutex);
    }
    if (!d->operational) {
        KisPageStoreDetail::setError(error, QStringLiteral("managed mutation store is unavailable"));
        return {};
    }
    range->admission = d->writeAdmission.beginClaimSet(range->writes);
    if (!d->writeAdmission.claimAll(range->admission, lock, error,
                                    KisPageWriteAdmission::ClaimOrigin::ManagedRange))
        return {};
    return range;
} catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("managed mutation storage is unavailable"));
    return {};
}

bool KisPageMutationSession::adoptReservation(
    std::unique_ptr<KisPageStoreWriteReservation> range, QString *error)
{
    if (!d || !range) {
        KisPageStoreDetail::setError(error, QStringLiteral("write reservation is absent"));
        return false;
    }
    QMutexLocker scopeLock(&d->mutex);
    if (d->state != Private::State::Active || d->executions || d->thread != QThread::currentThreadId() || d->writes.size() != 0
        || d->admission.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("mutation is not a fresh operation scope"));
        return false;
    }
    QMutexLocker ownerLock(&d->owner->mutex);
    if (!d->owner->writeAdmission.ownsClaimSetLocked(range->admission)) {
        KisPageStoreDetail::setError(error, QStringLiteral("write reservation lost its admission claim"));
        return false;
    }
    d->writes = std::move(range->writes);
    d->admission = std::move(range->admission);
    d->owner->writeAdmission.rebindClaimSetLocked(d->admission, d->writes);
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPageMutationSession::reserveLegacyMutationPage(
    const KisPageKey &key, QString *error)
{
    if (!d || !key.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("legacy mutation page is invalid"));
        return false;
    }
    QMutexLocker scopeLock(&d->mutex);
    if (d->state != Private::State::Active || d->executions || d->thread != QThread::currentThreadId()) {
        KisPageStoreDetail::setError(error, QStringLiteral("legacy mutation is unavailable"));
        return false;
    }
    QMutexLocker ownerLock(&d->owner->mutex);
    if (d->writes.find(key)) {
        const bool held = d->owner->writeAdmission.ownsClaimSetLocked(d->admission);
        KisPageStoreDetail::setError(error, held ? QString{} : QStringLiteral("legacy mutation lost its page claim"));
        return held;
    }
    if (d->owner->operational) {
        const auto conflict = d->owner->writeAdmission.conflictLocked(
            key, QThread::currentThreadId());
        if (conflict != KisPageWriteAdmission::Conflict::None) {
            // Legacy acquisition can hold the tile swap barrier and the
            // manager gate. The current writer needs those gates to deliver
            // its index/cache changes before dropping this same claim. Never
            // wait here or expose cached bytes as writable on rejection.
            const bool sameThread = conflict == KisPageWriteAdmission::Conflict::ManagedSameThread
                || conflict == KisPageWriteAdmission::Conflict::LegacySameThread
                || conflict == KisPageWriteAdmission::Conflict::WriterSameThread;
            KisPageStoreDetail::setError(error, sameThread
                ? QStringLiteral("legacy mutation cannot reenter its page")
                : QStringLiteral("legacy mutation conflicts with an active page writer"));
            return false;
        }
    }
    if (!d->owner->operational) {
        KisPageStoreDetail::setError(error, QStringLiteral("legacy mutation store is unavailable"));
        return false;
    }
    KisPageWriteIntent intent;
    intent.key = key;
    intent.inputKind = KisPageWriteInputKind::Semantic;
    const bool claimed = d->claimEntryLocked(
        intent, ownerLock, KisPageWriteAdmission::ClaimOrigin::LegacyAdapter, error);
    return claimed;
}

bool KisPageMutationSession::removePage(const KisPageKey &key, QString *error)
{
    if (!d) {
        KisPageStoreDetail::setError(error, QStringLiteral("mutation is absent"));
        return false;
    }
    return d->stageSemantic(key, {}, true, error);
}

bool KisPageMutationSession::aliasPage(const KisPageKey &key,
                                       const QSharedPointer<const KisPageReplicaSource> &source,
                                       QString *error)
{
    if (!d) {
        KisPageStoreDetail::setError(error, QStringLiteral("mutation is absent"));
        return false;
    }
    return d->stageSemantic(key, source, false, error);
}

KisCpuWriteGuard KisPageMutationSession::beginWrite(const KisPageKey &key, QString *error)
{
    return beginWrite(key, KisPageWriteMode::PreserveContents, error);
}

KisCpuWriteGuard KisPageMutationSession::beginWrite(const KisPageKey &key, KisPageWriteMode mode, QString *error)
{
    return beginWriteImpl(key, mode, nullptr, error);
}

bool KisPageMutationSession::overwritePage(const KisPageKey &key, const KisCpuPagePayload &payload, QString *error)
{
    auto guard = beginWriteImpl(key, KisPageWriteMode::DiscardContents, &payload, error);
    return guard.isValid();
}

KisCpuWriteGuard KisPageMutationSession::beginWriteImpl(const KisPageKey &key,
                                                        KisPageWriteMode mode,
                                                        const KisCpuPagePayload *payload,
                                                        QString *error,
                                                        const std::shared_ptr<KisPageMutationExecution::Private> &execution,
                                                        bool preparationOnly)
{
    KisCpuWriteGuard result;
    if (!d) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation is absent"));
        return result;
    }
    KisPageStoreDiagnosticTimer diagnostic(d->diagnosticOwner, KisPageStoreDiagnosticPhase::WriteAcquire, 1);
    std::optional<QMutexLocker<QMutex>> executionLock;
    if (execution) executionLock.emplace(&execution->mutex);
    auto *bound = execution ? execution->find(key) : nullptr;
    const bool permitted = !execution || (execution->scope == d && execution->linked
        && !execution->finished && execution->thread == QThread::currentThreadId() && bound);
    if (!permitted || !key.isValid()
        || (mode != KisPageWriteMode::PreserveContents && mode != KisPageWriteMode::DiscardContents)) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation is unavailable or used from another thread"));
        return result;
    }
    std::optional<QMutexLocker<QMutex>> scopeLock;
    if (!bound || !bound->page) scopeLock.emplace(&d->mutex);
    if (d->state != Private::State::Active
        || (!execution && (d->executions || d->thread != QThread::currentThreadId()))) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation is unavailable or used from another thread"));
        return result;
    }
    auto entryIndex = scopeLock ? d->writes.findIndex(key) : KisMutationWriteSet::InvalidEntry;
    auto *entry = bound ? bound->entry : d->writes.at(entryIndex);
    auto *page = bound ? bound->page : d->pageAtEntry(entryIndex);
    auto &counters = execution ? execution->counters : d->counters;
    if (entry && entry->isExposed()) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation page already has a writable guard"));
        return result;
    }
    void *data = nullptr;
    bool payloadInitialized = false;
    auto initialization = entry
        ? entry->initializationSource()
        : QSharedPointer<const KisPageReplicaSource>{};
    if (initialization && mode == KisPageWriteMode::DiscardContents) {
        // A declared complete overwrite needs no pixels from a preceding
        // private alias. Keep its key claim, discard only the initialization.
        entry->setInitializationSource({});
        initialization.clear();
    }
    if (!d->provider) {
        QMutexLocker lock(&d->owner->mutex);
        if (!d->selectCpuProviderLocked(error))
            return result;
    }
    if (initialization
        && (!(initialization->provider() == d->provider->providerId())
            || !(initialization->providerEpoch() == d->provider->providerEpoch()))) {
        if (!preparationOnly)
            d->state = Private::State::Failed;
        KisPageStoreDetail::setError(error, QStringLiteral("alias writable provider does not match its input"));
        return result;
    }
    if (!page) {
        auto *owner = d->owner;
        QMutexLocker lock(&owner->mutex);
        auto fail = [&](const QString &message) {
            if (!preparationOnly)
                d->state = Private::State::Failed;
            KisPageStoreDetail::setError(error, message);
        };
        if (!entry && owner->writeAdmission.pageClaimedLocked(key)) {
            fail(QStringLiteral("page is claimed by another writer"));
            return result;
        }
        KisPageWriteIntent intent;
        intent.key = key;
        intent.mode = mode;
        if (payload)
            intent.flags = quint8(KisPageWriteIntentFlag::InputBytesReady);
        QString claimError;
        if (!d->claimEntryLocked(intent, lock, KisPageWriteAdmission::ClaimOrigin::NativeSession, &claimError)) {
            fail(claimError);
            return result;
        }
        entryIndex = d->writes.findIndex(key);
        entry = d->writes.at(entryIndex);
        Q_ASSERT(entry);
        if (entry->isRemoval())
            intent.flags |= quint8(KisPageWriteIntentFlag::SemanticRemoval);
        KisPageTransition acquire;
        KisReplicaHandle recoverableBefore;
        KisPageAllocationDescriptor descriptor;
        QString failure;
        if (!owner->writeCoordinator.prepareWriteBaseLocked(d->transaction, intent, owner->publicationCoordinator,
                                                            &acquire, &descriptor, &failure, nullptr, &recoverableBefore)) {
            fail(failure);
            return result;
        }
        const auto source = acquire.source;
        if (source.isValid() && (!(source.provider == d->provider->providerId())
                            || !(source.providerEpoch == d->provider->providerEpoch())
                            || source.domain != KisPageAccessDomain::CpuRam)) {
            fail(QStringLiteral("CPU mutation requires an exact native CPU authority"));
            return result;
        }
        const auto target = acquire.version;
        const auto operation = acquire.operation;
        if (payload && !payload->isValidFor(descriptor)) {
            fail(QStringLiteral("complete CPU payload layout is invalid"));
            return result;
        }
        const bool directPayload = payload && d->provider->capabilities().synchronousCpuPayload;
        intent.mode = acquire.writeMode;
        intent.flags = directPayload ? quint8(KisPageWriteIntentFlag::InputBytesReady) : 0;
        if (initialization) intent.flags |= quint8(KisPageWriteIntentFlag::SourceInitialization);
        auto *resources = d->ensurePage(*entry);
        if (!resources) {
            fail(QStringLiteral("mutation cold storage is unavailable"));
            return result;
        }
        // Storage must precede retag, but an uninstalled slot is not a page
        // requiring CancelWrite. Keep failed Fresh preparation cancellable.
        auto discardUnprepared = qScopeGuard([&] { d->erasePageAtEntry(entryIndex); });
        KisCpuWriteBindingReservation writable;
        const auto plan = owner->writeCoordinator.prepareWritePlanLocked(
            d->transaction, intent, d->provider,
            {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer}, descriptor,
            owner->publicationCoordinator, acquire, recoverableBefore, lock, writable, diagnostic);
        if (plan != KisPageWritePlanKind::RecoverableHandoff) {
            decltype(owner->publicationCoordinator.prepareDescriptorLocked(target, descriptor)) descriptors;
            const auto discardDescriptors = qScopeGuard([&] {
                if (lock.isLocked()) lock.unlock();
                descriptors = {};
                lock.relock();
            });
            try { descriptors = owner->publicationCoordinator.prepareDescriptorLocked(target, descriptor); }
            catch (const std::bad_alloc &) {
                fail(QStringLiteral("mutation descriptor storage budget was refused"));
                return result;
            }
            KisPageBackingPreparation backing;
            ++owner->activeProviderCalls;
            {
                lock.unlock();
                const auto done = qScopeGuard([&] { lock.relock(); --owner->activeProviderCalls; });
                backing = owner->writeCoordinator.reserveBacking(descriptor, KisPageAccessDomain::CpuRam,
                    KisBackingBudgetClass::ActivePending, &failure, target);
            }
            const auto discardBacking = qScopeGuard([&] {
                if (!backing.retirement) return;
                lock.unlock(); backing.retirement.reset(); lock.relock();
            });
            if (!backing.reservation.isValid()) {
                fail(failure);
                return result;
            }
            ++owner->activeProviderCalls;
            lock.unlock();
            KisReplicaOperation allocation;
            {
                KisPageStoreDiagnosticTimer prepare(d->diagnosticOwner,
                                                    KisPageStoreDiagnosticPhase::WriteProviderPrepare,
                                                    1);
                allocation = owner->writeCoordinator.prepareFreshReplica(intent, *d->provider,
                    acquire, descriptor,
                    {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                    KisPagePriority::Interactive, payload, initialization);
            }
            const bool ownsNewTarget = ownsPreparedReplica(allocation.replica, target, *d->provider, source);
            const bool backingOwned = ownsNewTarget && owner->owner.registerBacking(
                allocation.replica, backing.reservation, KisBackingBudgetClass::ActivePending, &failure, &backing.retirement);
            const auto binding = backingOwned ? d->provider->cpuResidentBinding(allocation.replica)
                                               : QSharedPointer<KisCpuResidentBinding>{};
            writable = KisCpuWriteBindingReservation::acquire(binding, allocation.replica.allocationIdentity());
            data = d->pinWritable(writable, counters);
            lock.relock();
            --owner->activeProviderCalls;
            auto rejectAllocation = [&] {
                ++owner->activeProviderCalls;
                lock.unlock();
                writable.reset();
                if (ownsNewTarget)
                    owner->retirementQueue.retireOrDefer(allocation.replica, d->provider, {}, std::move(backing));
                lock.relock();
                --owner->activeProviderCalls;
            };
            if (!backingOwned || !allocation.isValid() || allocation.status != KisPageRequestStatus::Ready
                || allocation.replica.domain != KisPageAccessDomain::CpuRam
                || !allocation.replica.layout.matches(descriptor) || !binding
                || !data || !owner->owner.verifyTerminalProviderResult(operation, allocation, &failure).succeeded()) {
                rejectAllocation();
                fail(QStringLiteral("CPU mutation native allocation/access failed: ") + failure);
                return result;
            }
            acquire.target = allocation.replica;
            const auto prepared = owner->writeCoordinator.preparePrivateWrite(acquire, true);
            if (!prepared.accepted) {
                rejectAllocation();
                fail(prepared.rejectionReason);
                return result;
            }
            owner->publicationCoordinator.installDescriptorAdditionsLocked(&descriptors);
            payloadInitialized = directPayload;
        }
        page = resources;
        page->source = acquire.source;
        page->target = acquire.target;
        page->descriptor = owner->publicationCoordinator.descriptorLocked(target);
        Q_ASSERT(page->descriptor);
        page->writable = std::move(writable);
        owner->writeCoordinator.recordPrepared(*entry, intent, acquire);
        discardUnprepared.dismiss();
        ++owner->mutationStats.generationsReserved;
        if (initialization) {
            entry->setInitializationSource({});
            initialization.clear();
        }
    }
    if (payload && !payload->isValidFor(*page->descriptor)) {
        if (!preparationOnly)
            d->state = Private::State::Failed;
        KisPageStoreDetail::setError(error, QStringLiteral("complete CPU payload layout is invalid"));
        return result;
    }
    if (!data)
        data = d->pinWritable(page->writable, counters);
    if (!data) {
        if (!preparationOnly)
            d->state = Private::State::Failed;
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation pending backing cannot be pinned"));
        return result;
    }
    if (initialization) {
        KisPageStoreDiagnosticTimer phase(d->diagnosticOwner, KisPageStoreDiagnosticPhase::MutationSourceInitialize, 1);
        if (!d->provider->copySynchronousSourceToCpu(initialization,
                                                     *page->descriptor,
                                                     data,
                                                     page->target.layout.rowStride,
                                                     page->target.layout.byteSize)) {
            page->writable.unpin();
            if (!preparationOnly)
                d->state = Private::State::Failed;
            KisPageStoreDetail::setError(error, QStringLiteral("alias pending initialization failed"));
            return result;
        }
        entry->setInitializationSource({});
    }
    if (payload) {
        if (!payloadInitialized) {
            KisPageStoreDiagnosticTimer phase(d->diagnosticOwner, KisPageStoreDiagnosticPhase::MutationPayloadCopy, 1);
            const auto rowBytes = page->descriptor->minimumRowBytes();
            const int rows = page->descriptor->pageExtent.height();
            for (int row = 0; row < rows; ++row)
                std::memcpy(static_cast<quint8 *>(data) + quint64(row) * page->target.layout.rowStride,
                            static_cast<const quint8 *>(payload->data) + row * payload->rowStride,
                            size_t(rowBytes));
            counters.payloadCopyBytes += rowBytes * rows;
        }
        // Full input supersedes a staged semantic default, without a redundant fill.
        page->resetPixel.clear();
    } else if (!page->resetPixel.isEmpty()) {
        const auto &rect = page->descriptor->validRect;
        const auto &pixel = page->resetPixel;
        auto *pixels = static_cast<quint8 *>(data);
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            quint8 *row = pixels + quint64(y) * page->target.layout.rowStride;
            for (int x = rect.left(); x <= rect.right(); ++x)
                std::memcpy(row + quint64(x) * pixel.size(), pixel.constData(), size_t(pixel.size()));
        }
        counters.defaultResetBytes += quint64(rect.width()) * rect.height() * pixel.size();
        page->resetPixel.clear();
    }
    Q_ASSERT(entry);
    entry->setRemoval(false);
    d->owner->writeCoordinator.recordExposure(*entry, true);
    ++counters.pinsAcquired;
    counters.maximumPins = qMax(counters.maximumPins, ++counters.activePins);
    if (bound) bound->page = page;
    result.m_scope = execution ? std::shared_ptr<Private>(execution, d.get()) : d;
    result.m_execution = execution.get();
    result.m_entry = bound ? quint32(bound - execution->entries()) : entryIndex;
    result.m_entryIncarnation = bound ? 0 : d->writes.handleAt(entryIndex).incarnation;
    result.m_data = data;
    result.m_rowStride = page->target.layout.rowStride;
    result.m_byteSize = page->target.layout.byteSize;
    if (execution && !preparationOnly)
        execution->markTouched(size_t(bound - execution->entries()));
    KisPageStoreDetail::setError(error, {});
    return result;
}

bool KisPageMutationSession::cancel()
{
    if (!d)
        return true;
    QMutexLocker lock(&d->mutex);
    if (d->executions) {
        d->state = Private::State::Failed; // irrevocable cancellation intent
        return false;
    }
    return d->cancelLocked();
}

bool KisPageMutationSession::seal(QString *error)
{
    return sealImpl(error, false);
}

bool KisPageMutationSession::sealForLegacyUnlock(QString *error)
{
    return sealImpl(error, true);
}

bool KisPageMutationSession::sealForAdapterDelivery(QString *error)
{
    return sealImpl(error, false, nullptr, true);
}

KisCapturedReadView KisPageMutationSession::checkpointForRead(QString *error)
{
    KisCapturedReadView result;
    // Like borrowExecution, a quiescent checkpoint is not tied to the worker
    // that originally created the business-owned session.
    sealImpl(error, true, &result);
    return result;
}

bool KisPageMutationSession::sealImpl(QString *error, bool legacyFinalUnlock, KisCapturedReadView *checkpoint,
                                    bool retainAdmission)
{
    if (!d) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation is absent"));
        return false;
    }
    std::unique_lock<QMutex> scopeLock(d->mutex, std::defer_lock);
    if (checkpoint) {
        if (!scopeLock.try_lock()) {
            KisPageStoreDetail::setError(error, QStringLiteral("checkpoint requires a quiescent mutation"));
            return false;
        }
    } else {
        scopeLock.lock();
    }
    // One canonical segment seal, not one generic lease publish. Work items
    // count distinct claimed semantic/pixel keys, including removal and alias.
    KisPageStoreDiagnosticTimer diagnostic(d->diagnosticOwner,
                                           KisPageStoreDiagnosticPhase::WritePublishHost,
                                           quint64(d->writes.size()));
    if (d->state == Private::State::Detached || d->executions ||
        (!legacyFinalUnlock && d->thread != QThread::currentThreadId())) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation cannot seal"));
        return false;
    }
    for (auto slot = d->writes.firstEntry(); slot.isValid(); slot = d->writes.nextEntry(slot)) {
        if (d->writes.at(slot.index)->isExposed()) {
            KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation still has a writable guard"));
            return false;
        }
    }
    if (d->state == Private::State::Failed) {
        d->cancelLocked();
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation was cancelled after a failed write"));
        return false;
    }
    auto *owner = d->owner;
    using Phase = KisPageStoreDiagnosticPhase;
    const quint64 pageWork = quint64(d->writes.size());
    KisPageStoreDiagnosticTimer phase(d->diagnosticOwner, Phase::MutationSealOwnerWait, pageWork);
    QMutexLocker lock(&owner->mutex);
    phase.next(Phase::MutationSealInputs, pageWork);
    const auto retire = [&] {
        d->releaseStorageLocked(lock);
        owner->processRetirementsLocked(lock);
    };
    // A final semantic removal discards only this segment's unsealed target.
    // Previously sealed history remains untouched until the batch install.
    qsizetype privatePageCount = 0;
    for (auto slot = d->writes.firstEntry(); slot.isValid(); slot = d->writes.nextEntry(slot)) {
        const auto index = slot.index;
        auto *page = d->pageAtEntry(index);
        if (!page)
            continue;
        if (!d->writes.at(index)->isRemoval()) {
            ++privatePageCount;
            continue;
        }
        if (!d->cancelPageLocked(index, *page)) {
            d->state = Private::State::Failed;
            retire();
            KisPageStoreDetail::setError(error, QStringLiteral("mutation could not discard its removed target"));
            return false;
        }
        d->erasePageAtEntry(index);
    }
    // Return detached cold capacity before admitting alias replacements.
    // Otherwise the next prepare would pay for both the dead blocks and their
    // replacements, despite these discarded targets no longer being usable.
    d->releaseStorageLocked(lock);
    ++owner->activeProviderCalls;
    lock.unlock();
    phase.next(Phase::MutationSealPrivatePublish, quint64(privatePageCount));
    // Configuration prepares this immutable terminal ticket before any
    // mutation can be admitted; seal never creates a completion source.
    const KisCompletionTicket completion = owner->readyHostCompletion;
    Q_ASSERT(completion.isValid());
    QString failure;
    bool success = true;
    const auto validateClaims = [&](const char *stage) {
        if (success && !d->claimsHeldLocked()) {
            success = false;
            failure = QStringLiteral("mutation claims were lost %1")
                          .arg(QString::fromLatin1(stage));
        }
    };
    for (auto slot = d->writes.firstEntry(); slot.isValid(); slot = d->writes.nextEntry(slot)) {
        const auto index = slot.index;
        auto *page = d->pageAtEntry(index);
        if (!page)
            continue;
        if (!success)
            break;
        const auto applied = owner->writeCoordinator.publishPrivateWrite(
            d->writeTransition(index, *page));
        success = applied.accepted;
        if (!success) {
            failure = applied.rejectionReason.isEmpty()
                ? QStringLiteral("private publication was rejected")
                : applied.rejectionReason;
        }
    }
    phase.next(Phase::MutationSealOwnerWait, pageWork);
    lock.relock();
    --owner->activeProviderCalls;
    phase.next(Phase::MutationSealInputs, pageWork);
    validateClaims("after private publication");
    qsizetype sealedPageCount = privatePageCount;
    if (success)
        for (auto slot = d->writes.firstEntry(); slot.isValid(); slot = d->writes.nextEntry(slot)) {
            const auto index = slot.index;
            const auto *entry = d->writes.at(index);
            const auto source = entry->initializationSource();
            const bool hadPage = d->pageAtEntry(index) != nullptr;
            if (source && !d->prepareAliasLocked(entry->key(), source, lock, &failure)) {
                success = false;
                break;
            }
            if (source && !hadPage)
                ++sealedPageCount;
        }
    ++owner->activeProviderCalls;
    lock.unlock();
    phase.next(Phase::MutationSealProofPrepare, quint64(sealedPageCount));
    if (success)
        for (auto slot = d->writes.firstEntry(); slot.isValid(); slot = d->writes.nextEntry(slot)) {
            auto *page = d->pageAtEntry(slot.index);
            if (!page)
                continue;
            if (!owner->owner.sealPreparedPage(owner->metadata,
                                               page->target.version,
                                               d->transaction.id,
                                               *page->descriptor,
                                               completion,
                                               &page->proof,
                                               &failure)) {
                success = false;
                break;
            }
        }
    // No guards remain and these targets are still private. Drop physical
    // writer reservations before making proofs visible to new readers.
    if (success)
        for (auto slot = d->writes.firstEntry(); slot.isValid(); slot = d->writes.nextEntry(slot))
            if (auto *page = d->pageAtEntry(slot.index))
                page->writable.reset();

    phase.next(Phase::MutationSealOwnerWait, pageWork);
    lock.relock();
    phase.next(Phase::MutationSealInputs, pageWork);
    validateClaims("after proof preparation");
    using OverlayChange = KisPagePublicationCoordinator::OverlayChange;
    std::vector<OverlayChange, KisMutationStorageAllocator<OverlayChange>> overlayChanges{
        KisMutationStorageAllocator<OverlayChange>(&owner->backingBudget)};
    quint64 sealedCpuWrites = 0;
    quint64 sealedRemovals = 0;
    quint64 sealedSources = 0;
    if (success) try {
        overlayChanges.reserve(d->writes.size());
        for (auto slot = d->writes.firstEntry(); slot.isValid(); slot = d->writes.nextEntry(slot)) {
            const auto index = slot.index;
            auto *entry = d->writes.at(index);
            auto *page = d->pageAtEntry(index);
            if (entry->isRemoval()) {
                overlayChanges.push_back(
                    {entry->key(), {}, true});
                ++sealedRemovals;
            } else if (page) {
                overlayChanges.push_back(
                    {entry->key(), page->proof, false});
                sealedCpuWrites += entry->isCpuWrite();
            }
            sealedSources += bool(entry->initializationSource());
        }
    } catch (const std::bad_alloc &) {
        success = false;
        failure = QStringLiteral("overlay input storage preparation was refused");
    }
    const bool hasOverlay = success && !overlayChanges.empty();
    phase.next(Phase::MutationSealStoragePrepare, quint64(overlayChanges.size()));
    auto overlay = hasOverlay
        ? owner->publicationCoordinator.prepareOverlayUpdateLocked(
              d->transaction, overlayChanges.data(), overlayChanges.size(), &failure)
        : KisPagePublicationCoordinator::KisPreparedOverlayUpdate{};
    if (success && hasOverlay && !overlay.isValid()) {
        success = false;
        if (failure.isEmpty())
            failure = QStringLiteral("overlay update preparation was rejected");
    }
    if (success && hasOverlay) {
        // The aggregate now owns every new sealed proof. A failed prepare or
        // install revokes them together while the former overlay stays live.
        for (auto slot = d->writes.firstEntry(); slot.isValid(); slot = d->writes.nextEntry(slot)) {
            if (auto *page = d->pageAtEntry(
                    slot.index)) {
                page->proof = {};
            }
        }
    }
    const qsizetype metadataChangeCount = overlay.metadataChangeCount();
    lock.unlock();
    // The candidate owns these inputs now; free the producer's paid copy
    // before admitting the metadata candidate in the same shared budget.
    overlayChanges = decltype(overlayChanges){overlayChanges.get_allocator()};
    phase.next(Phase::MutationSealMetadataPrepare,
               quint64(metadataChangeCount));
    if (success && hasOverlay && !overlay.prepare(&failure)) {
        success = false;
        if (failure.isEmpty())
            failure = QStringLiteral("overlay metadata preparation was rejected");
    }
    KisPageMetadataCoordinator::DeferredPublicationCleanup metadataCleanup;
    phase.next(Phase::MutationSealPublishOwnerWait, pageWork);
    lock.relock();
    phase.next(Phase::MutationSealSurfacePrepare, pageWork);
    if (success && hasOverlay && !overlay.prepareSurfaceLocked(&failure)) success = false;
    phase.next(Phase::MutationSealInstall, pageWork);
    validateClaims("before overlay installation");
    if (success && hasOverlay && !overlay.tryInstallLocked(
            &metadataCleanup, &failure)) {
        success = false;
        if (failure.isEmpty())
            failure = QStringLiteral("overlay installation was rejected");
    }
    --owner->activeProviderCalls;
    owner->mutationStats.sealMetadataPreparations +=
        quint64(metadataChangeCount != 0);
    owner->mutationStats.sealMetadataRejections +=
        quint64(metadataChangeCount != 0 && !success);
    if (!success) {
        // No proof/overlay has been exposed; rollback only this segment's
        // versions, preserving previously prepared history and its proofs.
        d->state = Private::State::Failed;
        lock.unlock();
        phase.next(Phase::MutationSealCleanup, pageWork);
        disposeDeferredMetadataCleanup(std::move(metadataCleanup), owner->metadataCleanupStatistics);
        overlay = {};
        lock.relock();
        retire();
        d->cancelLocked(&lock);
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation seal failed: ") + failure);
        return false;
    }
    owner->mutationStats.pagesSealed += sealedCpuWrites;
    owner->synchronousHostWrites += sealedSources;
    owner->mutationStats.removalsSealed += sealedRemovals;
    KisPageMutationSession::Private::ColdPageSet sealedPages;
    std::swap(d->coldPages, sealedPages);
    // The complete overlay is installed. Destruction and physical retirement
    // may call providers or queue large releases; keep the claims but not the
    // owner gate while doing that work.
    ++owner->activeProviderCalls;
    lock.unlock();
    phase.next(Phase::MutationSealCleanup, pageWork);
    disposeDeferredMetadataCleanup(std::move(metadataCleanup), owner->metadataCleanupStatistics);
    lock.relock();
    overlay.collectRetirementsLocked();
    lock.unlock();
    overlay = {};
    sealedPages = Private::ColdPageSet{};
    d->clearSources();
    owner->retirementQueue.processAcceptedEffects(owner->backgroundReclamation);
    phase.next(Phase::MutationSealOwnerWait, pageWork);
    lock.relock();
    --owner->activeProviderCalls;
    d->finishSegmentLocked(lock, !checkpoint, retainAdmission);
    if (checkpoint) {
        // scopeLock excludes the next borrow through capture. The original
        // activity still excludes transaction commit/abort during this gap;
        // capture itself fixes the whole sealed overlay under the owner gate.
        lock.unlock();
        *checkpoint = KisPageStore::captureReadViewImpl(owner,
            KisPageReadView::transactionOverlay(d->transaction.id), error);
        lock.relock();
        if (!checkpoint->isValid()) {
            d->state = Private::State::Failed;
            d->cancelLocked(&lock);
            return false;
        }
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPageMutationStatistics KisPageStore::mutationStatistics() const
{
    QMutexLocker lock(&d->mutex);
    return d->mutationStats;
}

KisPageBackingUsage KisPageStore::backingUsage() const
{
    QMutexLocker lock(&d->mutex);
    return d->backingBudget.usage();
}

bool KisPageStore::configureBackingLimits(const KisPageBackingLimits &limits,
                                           QString *error)
{
    QMutexLocker lock(&d->mutex);
    if (d->operational || d->closing || d->closed || d->backingLimitsConfigured) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing limits must be frozen before initialization"));
        return false;
    }
    if (!d->backingBudget.configureLimits(limits, error)) return false;
    d->backingLimitsConfigured = true;
    return true;
}

bool KisPageStore::configureSharedNonPayloadBudget(
    const QSharedPointer<KisBackingBudgetController> &budget,
    QString *error)
{
    QMutexLocker lock(&d->mutex);
    if (d->operational || d->closing || d->closed || d->backingLimitsConfigured) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("shared backing budget must be frozen before initialization"));
        return false;
    }
    return d->backingBudget.configureSharedNonPayloadBudget(budget, error);
}

KisPageStorePublicationStatistics KisPageStore::publicationStatistics() const
{
    QMutexLocker lock(&d->mutex);
    auto result = d->publicationCoordinator.statisticsLocked();
    const auto statistics = d->metadataCleanupStatistics;
    result.metadataCleanupDeferredCandidates = statistics->deferredCandidates.loadRelaxed();
    result.metadataCleanupCompletedCandidates = statistics->completedCandidates.loadRelaxed();
    result.metadataCleanupPasses = statistics->passes.loadRelaxed();
    result.metadataCleanupUnits = statistics->units.loadRelaxed();
    result.metadataCleanupMaximumUnitsPerPass = statistics->maximumUnitsPerPass.loadRelaxed();
    result.metadataCleanupNanoseconds = statistics->nanoseconds.loadRelaxed();
    result.metadataCleanupMaximumPassNanoseconds = statistics->maximumPassNanoseconds.loadRelaxed();
    result.metadataCleanupQueueNanoseconds = statistics->queueNanoseconds.loadRelaxed();
    result.metadataCleanupMaximumQueueNanoseconds = statistics->maximumQueueNanoseconds.loadRelaxed();
    result.metadataCleanupPendingCandidates = statistics->pendingCandidates.loadRelaxed();
    result.metadataCleanupPeakPendingCandidates = statistics->peakPendingCandidates.loadRelaxed();
    result.metadataCleanupPendingUnits = statistics->pendingUnits.loadRelaxed();
    result.metadataCleanupPeakPendingUnits = statistics->peakPendingUnits.loadRelaxed();
    return result;
}

class KisCapturedReadView::Private
{
public:
    using DefaultCache = std::map<quint64, QSharedPointer<const KisCpuDefaultReadBuffer>, std::less<quint64>,
        KisMutationStorageAllocator<std::pair<const quint64, QSharedPointer<const KisCpuDefaultReadBuffer>>>>;
    explicit Private(KisBackingBudgetController &budget)
        : defaults(std::less<quint64>{}, DefaultCache::allocator_type(&budget)) {}
    static bool lessKey(const KisPageKey &a, const KisPageKey &b)
    {
        return std::tie(a.surface.value, a.page.row, a.page.column) <
               std::tie(b.surface.value, b.page.row, b.page.column);
    }
    bool surfaceState(KisSurfaceId surface, KisSurfaceEpochState *state) const
    {
        const auto &surfaces = release->stagedSurfaces;
        const auto found = std::lower_bound(surfaces.begin(), surfaces.end(), surface.value,
            [](const auto &candidate, quint64 id) { return candidate.surface.value < id; });
        if (found == surfaces.end() || !(found->surface == surface))
            return root.surfaceState(surface, state);
        *state = *found;
        return true;
    }

    bool resolve(const KisPageKey &key, KisPageVersion *version) const
    {
        if (!key.isValid())
            return false;
        const auto &versions = release->versions;
        const auto found = std::lower_bound(versions.begin(), versions.end(), key,
            [&](const KisPageVersion &candidate, const KisPageKey &value) { return lessKey(candidate.key, value); });
        if (found != versions.end() && found->key == key) {
            *version = *found;
            return true;
        }
        if (!root.resolve(key, version))
            return false;
        if (overlayTransaction.isValid() && (version->isDefaultPixel()
            || std::binary_search(release->removedPages.begin(), release->removedPages.end(), key, lessKey))) {
            KisSurfaceEpochState surface;
            if (!surfaceState(key.surface, &surface))
                return false;
            *version = {key, KisPageGeneration{1}, surface.defaultPixelRevision};
        }
        return true;
    }
    QSharedPointer<const KisCpuDefaultReadBuffer> defaultReadBuffer(const KisPageVersion &version) const
    try
    {
        QMutexLocker lock(&defaultMutex);
        const auto found = defaults.find(version.key.surface.value);
        if (found != defaults.end()) return found->second;
        KisSurfaceEpochState surface;
        if (!surfaceState(version.key.surface, &surface)
            || surface.defaultPixelRevision != version.defaultPixelRevision)
            return {};
        const auto insertion = defaults.try_emplace(version.key.surface.value);
        const auto slot = insertion.first;
        const bool inserted = insertion.second;
        Q_ASSERT(inserted); Q_UNUSED(inserted);
        const auto rollback = qScopeGuard([&] { if (!slot->second) defaults.erase(slot); });
        slot->second = owner->defaultStorage.readBuffer(surface);
        return slot->second;
    }
    catch (const std::bad_alloc &) { return {}; }

    ~Private()
    {
        if (!owner)
            return;
        owner->readCoordinator.releaseCapturedView(std::move(release));
    }
    // Last member to release: all ordinary funded arrays/root/defaults must
    // die before the Store controller. The shared allocation itself uses the
    // original retained allocator, covering control-block disposal afterward.
    std::unique_ptr<KisPageStore::Private, void (*)(KisPageStore::Private *)>
        owner{nullptr, &KisPageStore::PrivateReleaser::cleanup};
    KisImageEpochRootSnapshot root;
    KisImageEpochSnapshotToken retention;
    KisPageVersion exactVersion;
    KisPageTransactionId overlayTransaction;
    KisPageCapturedReleasePointer release;
    mutable QMutex defaultMutex;
    mutable DefaultCache defaults;
};

KisCapturedReadView::~KisCapturedReadView() = default;
bool KisCapturedReadView::isValid() const
{
    return bool(d);
}
bool KisCapturedReadView::isTransactionOverlay() const
{
    return d && d->overlayTransaction.isValid();
}
KisImageEpochId KisCapturedReadView::epoch() const
{
    return d ? d->root.epoch() : KisImageEpochId{};
}

bool KisCapturedReadView::resolvePageVersion(const KisPageKey &key, KisPageVersion *version) const
{
    if (version)
        *version = {};
    if (!d || !version || (d->exactVersion.isValid() && !(key == d->exactVersion.key)))
        return false;
    return d->resolve(key, version);
}

bool KisCapturedReadView::resolveSurfaceState(KisSurfaceId surface, KisSurfaceEpochState *state) const
{
    if (state)
        *state = {};
    if (!d || !state || (d->exactVersion.isValid() && !(surface == d->exactVersion.key.surface)))
        return false;
    return d->surfaceState(surface, state);
}

KisCpuReadGuard KisCapturedReadView::tryReadResidentPage(const KisPageKey &key, KisCpuResidentReadStatus *status) const
{
    return readResidentPageImpl(key, status, false);
}

KisCpuReadGuard KisCapturedReadView::readResidentPage(const KisPageKey &key, KisCpuResidentReadStatus *status) const
{
    return readResidentPageImpl(key, status, true);
}

KisCpuReadGuard KisCapturedReadView::readResidentPageImpl(const KisPageKey &key,
                                                          KisCpuResidentReadStatus *status,
                                                          bool waitForLocalGate) const
{
    KisCpuReadGuard guard;
    if (status)
        *status = KisCpuResidentReadStatus::InvalidView;
    if (!d)
        return guard;
    if (status)
        *status = KisCpuResidentReadStatus::InvalidIdentity;
    KisPageVersion version;
    if (!resolvePageVersion(key, &version))
        return guard;
    if (version.isDefaultPixel()) {
        auto buffer = d->defaultReadBuffer(version);
        if (status)
            *status = buffer ? KisCpuResidentReadStatus::Ready : KisCpuResidentReadStatus::ResourceUnavailable;
        if (!buffer)
            return guard;
        guard.m_scope = d;
        guard.m_data = buffer->data;
        guard.m_defaultBuffer = std::move(buffer);
        guard.m_version = version;
        guard.m_rowStride = guard.m_defaultBuffer->rowStride;
        guard.m_byteSize = guard.m_defaultBuffer->byteSize;
        return guard;
    }
    auto observed = version.defaultPixelRevision ? KisCpuResidentReadStatus::VirtualDefault
                                                 : KisCpuResidentReadStatus::BindingUnavailable;
    auto link = d->owner->metadata.cpuReadBinding(version);
    // The captured root and its exact version remain protected across every
    // cold miss/recheck. No provider lookup or large handle on the warm path.
    if (!link)
        link = KisPageReadCoordinator::discoverCpuReadBinding(d->owner->metadata, d->owner->owner, version);
    QSharedPointer<KisCpuResidentBinding> binding;
    const void *data = nullptr;
    constexpr int maximumAttempts = 3; // initial pin plus two immediate rediscoveries
    for (int attempt = 0; link && attempt < maximumAttempts; ++attempt) {
        if (!(link->replica.version == version)) {
            observed = KisCpuResidentReadStatus::InvalidIdentity;
            break;
        }
        binding = link->resolve(&observed);
        if (binding)
            data = binding->acquireRead(link->replica.allocationIdentity(), true, &observed, waitForLocalGate);
        if (data || (observed != KisCpuResidentReadStatus::InvalidIdentity
                     && observed != KisCpuResidentReadStatus::Retired)
            || attempt + 1 == maximumAttempts)
            break;
        link = KisPageReadCoordinator::discoverCpuReadBinding(
            d->owner->metadata, d->owner->owner, version, link);
        if (!link) observed = KisCpuResidentReadStatus::BindingUnavailable;
    }
    if (status) *status = observed;
    if (!data)
        return guard;
    guard.m_scope = d;
    guard.m_binding = std::move(binding);
    guard.m_data = data;
    guard.m_version = link->replica.version;
    guard.m_pageExtent = link->replica.layout.pageExtent;
    guard.m_rowStride = link->replica.layout.rowStride;
    guard.m_byteSize = link->replica.layout.byteSize;
    return guard;
}

KisCpuReadGuard::~KisCpuReadGuard()
{
    reset();
}
KisCpuReadGuard &KisCpuReadGuard::operator=(KisCpuReadGuard &&other) noexcept
{
    if (this != &other) {
        reset();
        m_scope = std::move(other.m_scope);
        m_binding = std::move(other.m_binding);
        m_data = std::exchange(other.m_data, nullptr);
        m_defaultBuffer = std::move(other.m_defaultBuffer);
        m_version = std::exchange(other.m_version, {});
        m_pageExtent = std::exchange(other.m_pageExtent, {});
        m_rowStride = std::exchange(other.m_rowStride, 0);
        m_byteSize = std::exchange(other.m_byteSize, 0);
    }
    return *this;
}
void KisCpuReadGuard::reset()
{
    if (m_data && m_binding)
        m_binding->releaseRead();
    m_data = nullptr;
    m_binding.clear();
    m_defaultBuffer.clear();
    m_version = {};
    m_pageExtent = {};
    m_rowStride = 0;
    m_byteSize = 0;
    // Physical unpin precedes root release/GC. The retained root keeps the
    // logical version non-discardable for the entire native access lifetime.
    m_scope.reset();
}
KisPageVersion KisCpuReadGuard::version() const
{
    return isValid() ? m_version : KisPageVersion{};
}
quint32 KisCpuReadGuard::rowStride() const
{
    return isValid() ? m_rowStride : 0;
}
quint64 KisCpuReadGuard::byteSize() const
{
    return isValid() ? m_byteSize : 0;
}

KisPageMetadataMetrics kisPageStoreMetadataMetrics(const KisPageStore &store)
{
    return store.d->metadata.metrics();
}

KisPageStore::KisPageStore()
    : d(new Private)
{
}

KisPageStore::~KisPageStore()
{
    // Outstanding capabilities keep Private alive, but recurring maintenance
    // must not sustain it after the facade is gone. Stop admission without
    // waiting on providers or invalidating the capabilities' original bytes.
    d->readCoordinator.stopAutomaticWakeups();
    d->retirementQueue.stopAutomaticWakeups();
    d->historyCollector.stopAutomaticWakeups();
}

void KisPageStore::PrivateReleaser::cleanup(Private *owner)
{
    if (owner && !owner->lifetimeReferences.deref()) {
        if (!owner->terminalCleanup || !owner->backgroundReclamation || kisOnPageStoreReclamationThread())
            delete owner;
        else
            kisEnqueuePageStoreReclamation(owner->terminalCleanup.release());
    }
}

KisCapturedReadView KisPageStore::captureReadView(const KisPageReadView &selector, QString *error)
{
    return captureReadViewImpl(d.data(), selector, error);
}

KisCapturedReadView KisPageStore::captureReadViewImpl(Private *d, const KisPageReadView &selector, QString *error)
{
    KisCapturedReadView result;
    const KisPageKey probe = selector.kind == KisPageReadViewKind::ExactVersion ? selector.exactVersion.key
                                                                                : KisPageKey{KisSurfaceId{1}, {0, 0}};
    if (!selector.isValidFor(probe)) {
        KisPageStoreDetail::setError(error, QStringLiteral("read selector is invalid"));
        return result;
    }
    // Bodies and the original release node precede protection. Failure leaves
    // no root/token obligation. The version capacity is prepared similarly
    // below, then selection is repeated under the owner gate before claiming.
    try {
        result.d = std::allocate_shared<KisCapturedReadView::Private>(
            KisMutationStorageAllocator<KisCapturedReadView::Private>::retained(&d->backingBudget),
            d->backingBudget);
        result.d->release = KisPageCapturedRelease::prepare(d->backingBudget);
    } catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("read scope storage is unavailable"));
        return {};
    }
    QMutexLocker locker(&d->mutex);
    KisImageEpochRootSnapshot root;
    KisPageVersion exact;
    for (;;) {
        if (!d->operational || d->closing) {
            KisPageStoreDetail::setError(error, QStringLiteral("PageStore is not operational"));
            return {};
        }
        root = {};
        switch (selector.kind) {
        case KisPageReadViewKind::CurrentCommittedEpoch:
            root = d->epochs.captureCommittedRoot();
            break;
        case KisPageReadViewKind::CommittedEpoch:
        case KisPageReadViewKind::ExactVersion:
            root = d->epochs.retainedRoot(selector.retention, selector.epoch);
            break;
        case KisPageReadViewKind::TransactionOverlay:
        case KisPageReadViewKind::TransactionBaseEpoch: {
            const auto transaction = d->epochs.activeTransaction(selector.transaction);
            if (transaction.isValid())
                root = d->epochs.root(transaction.baseEpoch);
            break;
        }
        }
        exact = {};
        if (!root.isValid()
            || (selector.kind == KisPageReadViewKind::ExactVersion
                && (!root.resolve(selector.exactVersion.key, &exact) || !(exact == selector.exactVersion)))) {
            KisPageStoreDetail::setError(error, QStringLiteral("read selector is stale or not owned by this PageStore"));
            return {};
        }
        if (selector.kind != KisPageReadViewKind::TransactionOverlay) break;
        size_t versions = 0, removals = 0, surfaces = 0;
        if (d->publicationCoordinator.captureDeltaLocked(selector.transaction, *result.d->release,
                                                         &versions, &removals, &surfaces)) break;
        ++d->activeProviderCalls;
        locker.unlock();
        try {
            result.d->release->versions.reserve(versions);
            result.d->release->removedPages.reserve(removals);
            result.d->release->stagedSurfaces.reserve(surfaces);
        } catch (const std::bad_alloc &) {
            locker.relock(); --d->activeProviderCalls;
            KisPageStoreDetail::setError(error, QStringLiteral("read scope version storage is unavailable"));
            return {};
        }
        locker.relock(); --d->activeProviderCalls;
    }
    auto &release = *result.d->release;
    if (selector.kind == KisPageReadViewKind::TransactionOverlay) {
        std::sort(release.versions.begin(), release.versions.end(), [](const auto &a, const auto &b) {
            return KisCapturedReadView::Private::lessKey(a.key, b.key);
        });
        std::sort(release.removedPages.begin(), release.removedPages.end(), KisCapturedReadView::Private::lessKey);
        std::sort(release.stagedSurfaces.begin(), release.stagedSurfaces.end(),
            [](const auto &a, const auto &b) { return a.surface.value < b.surface.value; });
        result.d->overlayTransaction = selector.transaction;
    }
    const auto retained = d->epochs.retainSnapshot(root.epoch());
    if (!retained.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("read scope retention could not be allocated"));
        return {};
    }
    release.token = retained.token;
    QString failure;
    try {
        // The prepared immutable delta and all of its claims share one cut.
        for (const auto &version : release.versions) {
            KisPageTransition claim;
            claim.kind = KisPageTransitionKind::RetainCapturedVersion;
            claim.version = version;
            claim.transaction = selector.transaction;
            claim.readView = retained.token;
            const auto claimed = d->metadata.applyOwner(version.key, claim);
            if (!claimed.accepted) {
                failure = claimed.rejectionReason;
                break;
            }
            ++release.retained;
        }
    } catch (const std::bad_alloc &) {
        failure = QStringLiteral("read scope protection storage is unavailable");
    }
    if (!failure.isEmpty()) {
        d->readCoordinator.releaseCapturedView(std::move(result.d->release), &locker);
        KisPageStoreDetail::setError(error, failure);
        return {};
    }
    d->lifetimeReferences.ref();
    result.d->owner.reset(d);
    result.d->root = std::move(root);
    result.d->retention = retained.token;
    result.d->exactVersion = exact;
    release.capturedScope = true;
    d->readCoordinator.noteCapturedViewCreatedLocked();
    KisPageStoreDetail::setError(error, {});
    return result;
}

KisReadRequest KisPageStore::acquireReadInView(const KisPageKey &key,
                                               const KisCapturedReadView &view,
                                               KisPageAccessRequirement access,
                                               KisPagePriority priority)
{
    return acquireReadImpl(key, {}, access, priority, &view);
}

bool KisPageStore::isOperational() const
{
    QMutexLocker locker(&d->mutex);
    return d->operational;
}

KisPageStoreSessionStats KisPageStore::sessionStats() const
{
    QMutexLocker locker(&d->mutex);
    return d->stats(true);
}

KisPageStoreReadScopeStatistics KisPageStore::readScopeStatistics() const
{
    QMutexLocker locker(&d->mutex);
    const auto history = d->historyCollector.snapshotLocked();
    const auto read = d->readCoordinator.snapshotLocked();
    return {read.capturedViewsCreated,
            read.capturedViewReleases,
            history.pagesVisited,
            history.foregroundPagesVisited,
            history.maximumPagesPerPass,
            history.maximumVersionsPerPass,
            history.reachabilityRefreshes,
            history.reachabilityRootsVisited,
            history.maximumRootsPerPass,
            history.reachabilityRestarts};
}

KisPageStoreRetirementProgress KisPageStore::processRetirements(qsizetype replicaBudget)
{
    QMutexLocker locker(&d->mutex);
    if (!d->operational || d->closing)
        return {};
    ++d->activeProviderCalls;
    locker.unlock();
    const auto result = d->retirementQueue.process(replicaBudget);
    locker.relock();
    --d->activeProviderCalls;
    return result;
}

bool KisPageStore::waitForRetirementIdle()
{
    if (kisOnPageStoreReclamationThread())
        return false;
    d->readCoordinator.waitForIdle();
    {
        QMutexLocker lock(&d->mutex);
        d->historyCollector.waitForIdleLocked();
    }
    d->retirementQueue.waitForIdle();
    {
        const auto statistics = d->metadataCleanupStatistics;
        QMutexLocker lock(&statistics->idleMutex);
        while (statistics->pendingUnits.loadRelaxed() != 0 || statistics->pendingCandidates.loadRelaxed() != 0)
            statistics->idle.wait(&statistics->idleMutex);
    }
    return true;
}

bool KisPageStore::closeSession(QString *error)
{
    if (kisOnPageStoreReclamationThread()) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore close cannot wait on its own reclamation worker"));
        return false;
    }
    KisPageMutationSession::Private::retryOrphans(d.data());
    KisPageReadCleanup cleanup(d->readCoordinator);
    QMutexLocker locker(&d->mutex);
    if (!d->completions) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore session was not configured"));
        return false;
    }
    if (d->closed) {
        KisPageStoreDetail::setError(error, d->closeFailure);
        return d->closeFailure.isEmpty();
    }
    if (d->closing) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore session close is already in progress"));
        return false;
    }

    // Freeze scheduling before examining provider operation counts: a worker
    // owns real ledger operations while retiring. Wait outside the owner gate
    // so provider callbacks cannot deadlock trying to inspect the store.
    d->closing = true;
    d->readCoordinator.beginCloseLocked();
    d->retirementQueue.beginClose();
    d->historyCollector.scheduleLocked(); // Freeze semantic retry before idle observation.
    locker.unlock();
    d->readCoordinator.waitForIdle();
    d->retirementQueue.waitForIdle();
    {
        const auto statistics = d->metadataCleanupStatistics;
        QMutexLocker cleanupLock(&statistics->idleMutex);
        while (statistics->pendingUnits.loadRelaxed() != 0 || statistics->pendingCandidates.loadRelaxed() != 0)
            statistics->idle.wait(&statistics->idleMutex);
    }
    KisPageMutationSession::Private::retryOrphans(d.data());
    locker.relock();
    d->historyCollector.waitForIdleLocked();
    d->epochs.collectFinishedTransactions();
    d->historyCollector.collectUnreachableLocked(nullptr, 0);
    d->readCoordinator.retryCancelledRequestsLocked(locker, cleanup, true);
    d->readCoordinator.retryCapturedReleasesLocked(locker, true);
    d->readCoordinator.retryReleasedReadsLocked(locker, cleanup, {}, nullptr, true);
    d->readCoordinator.acknowledgeCompletedLastUsesLocked(locker, cleanup);
    cleanup.finishUnlocked(locker);
    KisPageStoreSessionStats stats = d->stats();
    const qsizetype pendingShutdownReplicas = stats.pendingShutdownReplicas;
    stats.pendingShutdownReplicas = 0;
    // Detached replicas are also drained by close, even though metadata no
    // longer enumerates them. Other live capabilities still reject shutdown.
    stats.pendingRetiredReplicas = 0;
    // Saved retirement operations belong to the queues drained below. They
    // must reach terminal success before close succeeds, but must not prevent
    // close from polling them (including a retry after operational=false).
    // The public total remains unchanged: this is only the pre-drain gate.
    stats.providerOperations = d->owner.publicationBlockingOperationCount();
    const auto capturedReleases = d->readCoordinator.snapshotLocked().pendingCapturedReleases;
    if (stats.hasOutstandingCapabilities() || capturedReleases) {
        d->closing = false;
        d->readCoordinator.cancelCloseLocked();
        d->retirementQueue.cancelCloseAndSchedule();
        d->historyCollector.scheduleLocked();
        KisPageStoreDetail::setError(error,
                 QStringLiteral("PageStore session still owns capabilities: "
                                "transactions=%1 snapshots=%2 requests=%3 "
                                "readLeases=%4 writeLeases=%5 lastUses=%6 "
                                "proofs=%7 surfaceChanges=%8 removals=%9 "
                                "providerCalls=%10 blockingOperations=%11 "
                                "seals=%12 archives=%13 shutdownReplicas=%14 "
                                "defaultPreparations=%15 capturedReleases=%16")
                     .arg(stats.activeTransactions)
                     .arg(stats.retainedSnapshots)
                     .arg(stats.pendingRequests)
                     .arg(stats.activeReadLeases)
                     .arg(stats.activeWriteLeases)
                     .arg(stats.pendingLastUses)
                     .arg(stats.preparedPageProofs)
                     .arg(stats.preparedSurfaceChanges)
                     .arg(stats.stagedPageRemovals)
                     .arg(stats.activeProviderCalls)
                     .arg(stats.providerOperations)
                     .arg(stats.sealedPreparedProofs)
                     .arg(stats.pendingArchiveOperations)
                     .arg(pendingShutdownReplicas)
                     .arg(stats.activeDefaultPreparations)
                     .arg(capturedReleases));
        return false;
    }

    KisPageRetirementRecords liveShutdownReplicas;
    if (d->pendingShutdownReplicas.empty() && d->operational) {
        d->epochs.collectUnretainedRoots();
        QSet<KisReplicaAllocationIdentity> seenReplicas;
        for (const KisReplicaHandle &replica : d->metadata.shutdownReplicaHandles()) {
            if (!replica.isValid()) continue;
            const auto identity = replica.allocationIdentity();
            if (seenReplicas.contains(identity)) continue;
            seenReplicas.insert(identity);
            const auto provider = d->owner.provider(replica.provider, replica.providerEpoch);
            auto record = d->owner.takeRetirementRecord(replica);
            Q_ASSERT(record);
            if (!record) continue;
            record->provider = provider;
            liveShutdownReplicas.push_back(*record.release());
        }
    }

    KisPageRetirementRecords shutdownReplicas =
        std::move(d->pendingShutdownReplicas);
    // Drain already Debt-admitted records before current live replicas. With
    // a finite one-page Debt budget, trying current replicas first can reject
    // all of them even though retiring the queued debt immediately frees the
    // capacity required for a sequential shutdown.
    auto queuedShutdown = d->retirementQueue.takeForClose();
    shutdownReplicas.splice(shutdownReplicas.end(), queuedShutdown);
    shutdownReplicas.splice(shutdownReplicas.end(), liveShutdownReplicas);
    d->closing = true;
    d->operational = false;
    d->writeAdmissionChanged.wakeAll();
    const qsizetype shutdownCount = qsizetype(shutdownReplicas.size());
    d->activeProviderCalls += shutdownCount;
    locker.unlock();
    QStringList retirementFailures;
    KisPageRetirementRecords failedReplicas;
    for (auto it = shutdownReplicas.begin(); it != shutdownReplicas.end();) {
        const auto entry = it++;
        auto &shutdown = *entry;
        if (!shutdown.provider) {
            retirementFailures.append(QStringLiteral("replica %1:%2 lost its provider during shutdown")
                                          .arg(shutdown.replica.provider.value)
                                          .arg(shutdown.replica.allocation.slot));
            failedReplicas.splice(failedReplicas.end(), shutdownReplicas, entry);
            continue;
        }
        if (!d->retirementQueue.retireRecord(shutdown)) {
            retirementFailures.append(QStringLiteral("replica %1:%2 retirement failed")
                                          .arg(shutdown.replica.provider.value)
                                          .arg(shutdown.replica.allocation.slot));
            failedReplicas.splice(failedReplicas.end(), shutdownReplicas, entry);
        } else {
            d->metadata.removeCpuReadBinding(shutdown.replica);
        }
    }
    locker.relock();
    d->activeProviderCalls -= shutdownCount;
    d->pendingShutdownReplicas = std::move(failedReplicas);
    d->closing = false;
    if (!retirementFailures.isEmpty()) {
        d->closed = false;
        d->closeFailure = retirementFailures.join(QStringLiteral("; "));
    } else {
        d->closed = true;
        d->closeFailure.clear();
        d->historyCollector.clearLocked();
        Q_ASSERT(d->retirementQueue.isDrained());
        Q_ASSERT(d->defaultStorage.isDrainedLocked());
        d->defaultStorage.clearReadCache();
    }
    KisPageStoreDetail::setError(error, d->closeFailure);
    return d->closeFailure.isEmpty();
}

bool KisPageStore::configure(const KisImageEpochSnapshot &initialEpoch,
                             const QSharedPointer<KisCompletionRegistry> &completions,
                             qsizetype metadataShardCount,
                             QString *error)
{
    if (!initialEpoch.isValid() || !completions || !completions->isOperational() || metadataShardCount <= 0) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore configuration is invalid"));
        return false;
    }
    for (const auto &version : initialEpoch.manifest) {
        if (version.isDefaultPixel()) {
            KisPageStoreDetail::setError(error,
                     QStringLiteral("initial default content belongs to surface metadata, not a payload manifest"));
            return false;
        }
    }
    QMutexLocker locker(&d->mutex);
    if (d->completions || d->activeProviderCalls) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore is already configured"));
        return false;
    }
    if ((d->configurationSourceOwner && d->configurationSourceOwner != completions)
        || (d->metadata.isOperational() && d->metadata.shardCount() != metadataShardCount)) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore configuration changed accepted preparation"));
        return false;
    }
    // A root with physical state must already own its terminal and recurring
    // task storage. Prepare outside the owner gate, before initialization can
    // adopt any backing; concurrent configuration cannot enter this interval.
    ++d->activeProviderCalls;
    locker.unlock();
    std::shared_ptr<KisImageEpochReferenceModel::Private> epoch;
    QString failure;
    try {
        if (!d->terminalCleanup) {
            d->terminalCleanup = kisPreparePageStoreReclamation(
                [owner = d.data()] { delete owner; }, &d->backingBudget);
            d->terminalCleanup->reusable = false;
        }
        d->retirementQueue.prepareTask();
        d->historyCollector.prepareTask(d->backingBudget);
        d->readCoordinator.prepareTask();
        epoch = d->epochs.prepareInitialization(initialEpoch, &failure);
    } catch (const std::bad_alloc &) {
        locker.relock();
        --d->activeProviderCalls;
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore reclamation budget storage was refused"));
        return false;
    }
    locker.relock();
    --d->activeProviderCalls;
    if (!epoch || !d->publicationCoordinator.prepareDefaultRevisionsLocked(initialEpoch.surfaces, &failure)
        || (d->metadata.isOperational() ? d->metadata.shardCount() != metadataShardCount
                                      : !d->metadata.configure(metadataShardCount, &failure))) {
        locker.unlock();
        KisPageStoreDetail::setError(error, failure);
        return false;
    }
    if (!d->configurationSource) {
        d->configurationSource = completions->registerSource(KisCompletionDomain::HostLogical);
        if (d->configurationSource) d->configurationSourceOwner = completions;
    }
    const KisCompletionTicket ready = d->configurationSource != 0
        ? completions->allocatePending(d->configurationSource) : KisCompletionTicket();
    if (!ready.isValid()) {
        locker.unlock();
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore host completion source registration failed"));
        return false;
    }
    completions->completePrepared(ready, KisCompletionStatus::Succeeded);
    const bool ownerConfigured = d->owner.configure(completions, &failure);
    Q_ASSERT(ownerConfigured);
    Q_UNUSED(ownerConfigured);
    const bool epochInstalled = d->epochs.installInitialization(std::move(epoch));
    Q_ASSERT(epochInstalled);
    Q_UNUSED(epochInstalled);
    d->completions = completions;
    d->readyHostCompletion = ready;
    d->configurationSourceOwner.clear();
    d->configurationSource = 0;
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPageStore::configureDerivedPageExtent(KisSurfaceId surface)
{
    QMutexLocker locker(&d->mutex);
    return d->completions && !d->operational && !d->closed && !d->closing
        && d->publicationCoordinator.configureDerivedExtentLocked(surface);
}

bool KisPageStore::registerReplicaProvider(const QSharedPointer<KisPageReplicaProvider> &provider)
{
    QMutexLocker locker(&d->mutex);
    const bool registered = d->completions && !d->operational && !d->closed && !d->closing && provider
        && provider->capabilities().synchronousOperations && d->owner.registerProvider(provider);
    if (registered)
        d->backgroundReclamation = d->backgroundReclamation && provider->capabilities().backgroundRetirement;
    return registered;
}

bool KisPageStore::registerTransferBridge(const QSharedPointer<KisPageReplicaTransferBridge> &bridge)
{
    QMutexLocker locker(&d->mutex);
    if (!d->completions || d->operational || d->closed
        || !d->writeCoordinator.registerTransferBridge(bridge)) {
        return false;
    }
    return true;
}

bool KisPageStore::registerExactGenerationArchive(const QSharedPointer<KisExactGenerationArchive> &archive)
{
    QMutexLocker locker(&d->mutex);
    if (!d->completions || d->operational || d->closed || d->exactArchive || !archive || !archive->isOperational()
        || !archive->archiveId().isValid() || !archive->archiveEpoch().isValid()) {
        return false;
    }
    d->exactArchive = archive;
    return true;
}

bool KisPageStore::adoptInitialPage(const KisPageVersion &version,
                                    const KisPageAllocationDescriptor &descriptor,
                                    const KisReplicaHandle &authority,
                                    QString *error)
{
    return adoptInitialPage(version, descriptor, authority, QVector<KisReplicaHandle>{}, error);
}

bool KisPageStore::adoptInitialPage(const KisPageVersion &version,
                                    const KisPageAllocationDescriptor &descriptor,
                                    const KisReplicaHandle &authority,
                                    const QVector<KisReplicaHandle> &readyAlternates,
                                    QString *error)
{
    QMutexLocker locker(&d->mutex);
    if (!d->completions || d->operational || d->closed || !version.isValid() || version.isDefaultPixel()
        || !descriptor.isValid() || !authority.isValid() || !(authority.version == version)
        || !authority.layout.matches(descriptor)) {
        KisPageStoreDetail::setError(error, QStringLiteral("initial PageStore page adoption is invalid"));
        return false;
    }
    const KisImageEpochRootSnapshot initialRoot = d->epochs.captureCommittedRoot();
    KisPageVersion initialVersion;
    if (!initialRoot.resolve(version.key, &initialVersion) || !(initialVersion == version)) {
        KisPageStoreDetail::setError(error, QStringLiteral("initial page is absent from the configured epoch manifest"));
        return false;
    }
    QVector<KisReplicaHandle> replicas{authority};
    QVector<QSharedPointer<KisPageReplicaProvider>> providers;
    for (const auto &replica : readyAlternates) {
        if (!replica.isValid() || !(replica.version == version) || !replica.layout.matches(descriptor)
            || std::any_of(replicas.cbegin(), replicas.cend(), [&](const auto &existing) {
                return existing.physicalSlotIdentity() == replica.physicalSlotIdentity();
            })) {
            KisPageStoreDetail::setError(error, QStringLiteral("initial exact replica is invalid or duplicated"));
            return false;
        }
        replicas.append(replica);
    }
    for (const auto &replica : std::as_const(replicas)) {
        auto provider = d->owner.provider(replica.provider, replica.providerEpoch);
        if (!provider) {
            KisPageStoreDetail::setError(error, QStringLiteral("initial replica provider is unavailable"));
            return false;
        }
        providers.append(std::move(provider));
    }
    ++d->activeProviderCalls;
    locker.unlock();
    bool valid = true;
    for (qsizetype i = 0; i < replicas.size(); ++i)
        valid = providers[i]->validate(replicas[i], descriptor) && valid;
    locker.relock();
    --d->activeProviderCalls;
    KisPageMetadataCoordinator::VersionInfo existingPage;
    if (!valid || d->operational || d->closed || d->metadata.versionSnapshot(version, &existingPage)) {
        KisPageStoreDetail::setError(error, QStringLiteral("initial page replicas failed provider validation"));
        return false;
    }
    decltype(d->publicationCoordinator.prepareDescriptorLocked(version, descriptor)) descriptors;
    const auto discardDescriptors = qScopeGuard([&] {
        if (locker.isLocked()) locker.unlock();
        descriptors = {};
        locker.relock();
    });
    try { descriptors = d->publicationCoordinator.prepareDescriptorLocked(version, descriptor); }
    catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("initial descriptor storage budget was refused"));
        return false;
    }
    using TerminalNodes = std::vector<KisPageRetirementRecordPointer,
        KisMutationStorageAllocator<KisPageRetirementRecordPointer>>;
    TerminalNodes terminals{KisMutationStorageAllocator<KisPageRetirementRecordPointer>(&d->backingBudget)};
    const auto discardTerminals = qScopeGuard([&] {
        locker.unlock();
        TerminalNodes{terminals.get_allocator()}.swap(terminals);
        locker.relock();
    });
    // Prepare the whole terminal-node footprint before the first registration.
    // Dropping the store gate between partial registrations would let another
    // initial adoption publish a page that rollback then incorrectly removed.
    ++d->activeProviderCalls;
    {
        locker.unlock();
        const auto done = qScopeGuard([&] { locker.relock(); --d->activeProviderCalls; });
        try {
            terminals.resize(size_t(replicas.size()));
            for (qsizetype i = 0; i < replicas.size(); ++i)
                if (d->owner.backingClass(replicas[i]) == KisBackingBudgetClass::Count)
                    terminals[size_t(i)] = kisPreparePageRetirementRecord(&d->backingBudget);
        } catch (const std::bad_alloc &) {
            KisPageStoreDetail::setError(error, QStringLiteral("initial backing retirement storage budget was refused"));
            return false;
        }
    }
    if (d->operational || d->closed || d->metadata.versionSnapshot(version, &existingPage)) {
        KisPageStoreDetail::setError(error, QStringLiteral("initial page changed during retirement storage preparation"));
        return false;
    }
    QVector<KisReplicaHandle> registered;
    auto undoRegistration = qScopeGuard([&] {
        // Keep partial registration invisible to another initial adoption.
        // Reuse the admitted array for node disposal after rollback completes.
        for (const auto &replica : registered) {
            const auto found = std::find(replicas.cbegin(), replicas.cend(), replica);
            Q_ASSERT(found != replicas.cend());
            auto &terminal = terminals[size_t(found - replicas.cbegin())];
            Q_ASSERT(!terminal);
            terminal = d->owner.takeRetirementRecord(replica);
            d->owner.releaseRetiredBacking(replica);
        }
    });
    for (qsizetype i = 0; i < replicas.size(); ++i) {
        const auto &replica = replicas[i];
        if (d->owner.backingClass(replica) != KisBackingBudgetClass::Count) continue;
        auto backing = d->writeCoordinator.reserveBacking(descriptor, replica.domain,
            KisBackingBudgetClass::Current, error, {}, &terminals[size_t(i)]);
        const auto discardBacking = qScopeGuard([&] {
            if (!backing.retirement) return;
            Q_ASSERT(!terminals[size_t(i)]);
            terminals[size_t(i)] = std::move(backing.retirement);
        });
        if (!backing.reservation.isValid()
            || !d->owner.registerBacking(replica, backing.reservation, KisBackingBudgetClass::Current, error, &backing.retirement))
            return false;
        registered.append(replica);
    }

    KisPageVersionStateSnapshot versionState;
    versionState.version = version;
    versionState.publication = KisPagePublicationState::Published;
    for (const auto &replica : std::as_const(replicas))
        versionState.replicas.append({replica, KisReplicaValidity::Valid, {}, {}, 0, {}});
    versionState.authority = authority;

    KisPageStateSnapshot page;
    page.key = version.key;
    page.publishedEpoch = initialRoot.epoch();
    page.publishedGeneration = version.generation;
    page.publishedDefaultPixelRevision = version.defaultPixelRevision;
    page.nextGeneration = KisPageGeneration{version.generation.value + 1};
    page.versions.append(versionState);
    QString failure;
    if (!d->metadata.registerPage(page, &failure)) {
        KisPageStoreDetail::setError(error, failure);
        return false;
    }
    d->publicationCoordinator.installDescriptorAdditionsLocked(&descriptors);
    undoRegistration.dismiss();
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPageStore::adoptInitialPageBytes(const KisPageVersion &version,
                                         const KisPageAllocationDescriptor &descriptor,
                                         const QByteArray &canonicalBytes,
                                         QString *error)
{
    const KisPageAccessRequirement cpuAccess{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
    QSharedPointer<KisPageReplicaProvider> provider;
    KisPageOperationId allocationOperation;
    KisPageOperationId accessOperation;
    KisPageLeaseId lease;
    KisPageBackingPreparation backing;
    {
        QMutexLocker locker(&d->mutex);
        if (!d->completions || d->operational || d->closed || !version.isValid() || version.isDefaultPixel()
            || !descriptor.isValid() || canonicalBytes.size() != qsizetype(descriptor.minimumByteSize())) {
            KisPageStoreDetail::setError(error, QStringLiteral("initial canonical page bytes are invalid"));
            return false;
        }
        provider = d->owner.providerFor(cpuAccess);
        allocationOperation = d->owner.nextOperationId();
        accessOperation = d->owner.nextOperationId();
        lease = d->owner.nextLeaseId();
        if (!provider || !allocationOperation.isValid() || !accessOperation.isValid() || !lease.isValid()) {
            KisPageStoreDetail::setError(error, QStringLiteral("initial CPU page provider or identity is unavailable"));
            return false;
        }
        ++d->activeProviderCalls;
        {
            locker.unlock();
            const auto done = qScopeGuard([&] { locker.relock(); --d->activeProviderCalls; });
            backing = d->writeCoordinator.reserveBacking(descriptor, KisPageAccessDomain::CpuRam,
                KisBackingBudgetClass::Current, error);
        }
        if (!backing.reservation.isValid()) return false;
        ++d->activeProviderCalls;
    }

    const KisReplicaOperation allocation = provider->requestReplica(allocationOperation,
                                                                    version,
                                                                    descriptor,
                                                                    KisPageAccessDomain::CpuRam,
                                                                    KisPageAccessMode::Write,
                                                                    KisPagePriority::Normal);
    QString failure;
    const bool ownsNewTarget = ownsPreparedReplica(allocation.replica, version, *provider);
    const bool backingOwned = ownsNewTarget && d->owner.registerBacking(
        allocation.replica, backing.reservation, KisBackingBudgetClass::Current, &failure, &backing.retirement);
    const KisVerifiedCompletion allocated = allocation.isValid()
        ? d->owner.verifyTerminalProviderResult(allocationOperation, allocation, &failure)
        : KisVerifiedCompletion();
    bool bytesCopied = false;
    if (backingOwned && allocated.isValid() && allocated.succeeded()
        && allocation.replica.layout.matches(descriptor)) {
        KisReplicaAccess access =
            provider->resolveAccess(lease, accessOperation, allocation.replica, cpuAccess, KisPageAccessMode::Write);
        if (access.isValid()) {
            const quint64 rowBytes = descriptor.minimumRowBytes();
            bytesCopied = access.replica.layout.rowStride >= rowBytes
                && access.replica.layout.byteSize >= quint64(access.replica.layout.rowStride)
                    * quint64(descriptor.pageExtent.height());
            if (bytesCopied) {
                for (int row = 0; row < descriptor.pageExtent.height(); ++row) {
                    std::memcpy(static_cast<quint8 *>(access.cpuWriteData)
                                    + quint64(row) * access.replica.layout.rowStride,
                                canonicalBytes.constData() + quint64(row) * rowBytes,
                                size_t(rowBytes));
                }
            }
            provider->releaseAccess(std::move(access), {});
        }
    }

    bool adopted = false;
    if (bytesCopied) {
        adopted = adoptInitialPage(version, descriptor, allocation.replica, &failure);
    }
    if (!adopted && ownsNewTarget) {
        d->retirementQueue.retireOrDefer(allocation.replica, provider, {}, std::move(backing));
    }
    {
        QMutexLocker locker(&d->mutex);
        --d->activeProviderCalls;
    }
    if (!adopted) {
        KisPageStoreDetail::setError(error, failure.isEmpty() ? QStringLiteral("initial canonical page import failed") : failure);
        return false;
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPageStore::finalizeInitialization(QString *error)
{
    QMutexLocker locker(&d->mutex);
    if (!d->completions || d->operational || d->closed || d->activeProviderCalls != 0) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore is not awaiting finalization"));
        return false;
    }
    for (const KisPageVersion &version : d->epochs.captureCommittedRoot().manifest()) {
        KisPageMetadataCoordinator::VersionInfo page;
        if (!d->publicationCoordinator.descriptorLocked(version) || !d->metadata.versionSnapshot(version, &page)
            || !(page.publishedGeneration == version.generation)) {
            KisPageStoreDetail::setError(error, QStringLiteral("PageStore initial manifest is not fully adopted"));
            return false;
        }
    }
    d->operational = true;
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPageStore::resolveSurfaceState(KisSurfaceId surface,
                                       const KisPageReadView &view,
                                       KisSurfaceEpochState *state) const
{
    QMutexLocker locker(&d->mutex);
    return d->publicationCoordinator.resolveSurfaceLocked(surface, view, state);
}

bool KisPageStore::resolvePageVersion(const KisPageKey &key, const KisPageReadView &view, KisPageVersion *version) const
{
    QMutexLocker locker(&d->mutex);
    return d->publicationCoordinator.resolveVersionLocked(key, view, version);
}

bool KisPageStore::resolveTileReadIdentity(const KisPageKey &key, const KisPageReadView &view,
                                         KisPageVersion *version, KisSurfaceEpochState *state) const
{
    QMutexLocker locker(&d->mutex);
    return d->operational && !d->closing && !d->closed &&
        d->publicationCoordinator.resolveVersionLocked(key, view, version) &&
        (!version->isDefaultPixel() || d->publicationCoordinator.resolveSurfaceLocked(key.surface, view, state));
}

bool KisPageStore::resolvePagePresence(KisSurfaceId surface,
                                       const QVector<KisLogicalPageId> &pages,
                                       const KisPageReadView &view,
                                       QVector<quint8> *present) const
{
    if (!present || !surface.isValid()) return false;
    // Actual output storage precedes the owner gate, including Qt detachment.
    // A rejected selector leaves the caller's previous classification intact.
    QVector<quint8> candidate;
    try {
        candidate.reserve(pages.size());
        if (candidate.capacity() < pages.size()) return false;
        candidate.resize(pages.size());
    } catch (const std::bad_alloc &) {
        return false;
    }
    if (!resolvePagePresenceInto(surface, pages, view, candidate.data(), candidate.size())) return false;
    // The output's previous storage also dies outside the owner gate.
    present->swap(candidate);
    return true;
}

bool KisPageStore::resolvePagePresenceInto(KisSurfaceId surface,
                                          const QVector<KisLogicalPageId> &pages,
                                          const KisPageReadView &view,
                                          quint8 *scratch, qsizetype capacity) const
{
    if (!surface.isValid() || capacity < pages.size() || (!scratch && !pages.isEmpty())) return false;
    QMutexLocker lock(&d->mutex);
    KisSurfaceEpochState state;
    if (!d->publicationCoordinator.resolveSurfaceLocked(surface, view, &state)) return false;
    for (qsizetype i = 0; i < pages.size(); ++i) {
        KisPageVersion version;
        if (!d->publicationCoordinator.resolveVersionLocked({surface, pages[i]}, view, &version)) return false;
        scratch[i] = !version.isDefaultPixel();
    }
    return true;
}

bool KisPageStore::pageDescriptor(const KisPageVersion &version, KisPageAllocationDescriptor *descriptor) const
{
    QMutexLocker locker(&d->mutex);
    if (!d->operational || !version.isValid() || !descriptor)
        return false;
    return d->publicationCoordinator.descriptorLocked(version, descriptor);
}

KisReadRequest KisPageStore::acquireRead(const KisPageKey &key,
                                         const KisPageReadView &view,
                                         KisPageAccessRequirement access,
                                         KisPagePriority priority)
{
    return acquireReadImpl(key, view, access, priority, nullptr);
}

KisReadRequest KisPageStore::acquireReadImpl(const KisPageKey &key,
                                             const KisPageReadView &view,
                                             KisPageAccessRequirement access,
                                             KisPagePriority priority,
                                             const KisCapturedReadView *captured)
{
    KisReadRequest request;
    request.access = access;
    KisPageReadCleanup cleanup(d->readCoordinator);
    QMutexLocker locker(&d->mutex);
    d->readCoordinator.retryCancelledRequestsLocked(locker, cleanup);
    if (!d->operational || !view.isValidFor(key) || !access.isValid()
        || (captured && (!captured->d || captured->d->owner.get() != d.data()))) {
        request.error = QStringLiteral("PageStore read request is invalid or unavailable");
        return request;
    }
    const auto fail = [&request](const QString &error) {
        request.status = KisPageRequestStatus::Failed;
        request.error = error;
        return request;
    };

    QString preparationError;
    if (!cleanup.prepareRequestLocked(locker, &preparationError)) return fail(preparationError);
    KisPageVersion version;
    KisPageMetadataCoordinator::VersionInfo page;
    KisPageMetadataCoordinator::ReplicaCandidates candidates{
        KisMutationStorageAllocator<KisPageMetadataCoordinator::ReplicaCandidate>(&d->backingBudget)};
    const auto snapshotVersion = [&] {
        for (;;) {
            const bool exists = d->metadata.versionSnapshot(version, &page);
            // Prepare the first physical candidate before materializing a
            // default. Once capacity is ready, selection and AcquireRead keep
            // the original owner gate, including after default installation.
            const size_t count = std::max<size_t>(1, exists ? page.replicaCount : 0);
            if (candidates.capacity() >= count)
                return exists && d->metadata.versionSnapshot(version, &page, &candidates);
            ++d->activeProviderCalls;
            locker.unlock();
            const auto relock = qScopeGuard([&] { locker.relock(); --d->activeProviderCalls; });
            candidates.reserve(count);
        }
    };
    if (!(captured ? captured->resolvePageVersion(key, &version)
                   : d->publicationCoordinator.resolveVersionLocked(key, view, &version))) {
        return fail(QStringLiteral("PageStore read view does not resolve a page"));
    }
    bool hasPage = false;
    try { hasPage = snapshotVersion(); }
    catch (const std::bad_alloc &) { return fail(QStringLiteral("exact replica candidate storage was refused")); }
    if (!hasPage || page.needsDefaultMaterialization(version)) {
        KisSurfaceEpochState surface;
        KisImageEpochId publishedEpoch;
        if (captured) {
            publishedEpoch = captured->epoch();
        } else if (view.kind == KisPageReadViewKind::CurrentCommittedEpoch) {
            publishedEpoch = d->epochs.captureCommittedRoot().epoch();
        } else if (view.kind == KisPageReadViewKind::CommittedEpoch || view.kind == KisPageReadViewKind::ExactVersion) {
            publishedEpoch = view.epoch;
        } else {
            publishedEpoch = d->epochs.activeTransaction(view.transaction).baseEpoch;
        }
        QString failure;
        if (!(captured ? captured->resolveSurfaceState(key.surface, &surface)
                       : d->publicationCoordinator.resolveSurfaceLocked(key.surface, view, &surface))) {
            return fail(QStringLiteral("PageStore default surface metadata is unavailable"));
        }
        if (!hasPage) {
            // A private/historical default read must not install its revision
            // as the current published metadata head of a virgin coordinate.
            KisPageVersion committedVersion;
            KisSurfaceEpochState committedSurface;
            const KisPageReadView current;
            if (!d->publicationCoordinator.resolveVersionLocked(key, current, &committedVersion)
                || !d->publicationCoordinator.resolveSurfaceLocked(key.surface, current, &committedSurface)
                || !d->publicationCoordinator.ensureVirtualDefaultLocked(committedVersion,
                                                  committedSurface,
                                                  &failure)) {
                return fail(failure.isEmpty()
                    ? QStringLiteral("committed default identity is unavailable") : failure);
            }
        }
        if (!d->ensureDefaultPageLocked(version, publishedEpoch, surface, access, priority, &locker, &failure)) {
            return fail(failure.isEmpty()
                ? QStringLiteral("PageStore default page materialization failed") : failure);
        }
        try {
            if (!snapshotVersion()) return fail(QStringLiteral("PageStore default metadata is unavailable"));
        } catch (const std::bad_alloc &) { return fail(QStringLiteral("exact replica candidate storage was refused")); }
    }
    // Keep the exact version captured above, including across provider work.
    const KisPageMetadataCoordinator::ReplicaCandidate *replica = nullptr;
    QSharedPointer<KisPageReplicaProvider> provider;
    if (page.version.isValid()) {
        const auto authority = std::find_if(candidates.begin(), candidates.end(),
            [&](const auto &candidate) { return candidate.replica == page.authority; });
        const auto *authorityReplica = authority == candidates.end() ? nullptr : &*authority;
        if (authorityReplica && authorityReplica->validity == KisReplicaValidity::Valid
            && authorityReplica->replica.domain == access.domain) {
            const QSharedPointer<KisPageReplicaProvider> authorityProvider =
                d->owner.provider(authorityReplica->replica.provider, authorityReplica->replica.providerEpoch);
            if (authorityProvider && authorityProvider->capabilities().supports(access)) {
                replica = authorityReplica;
                provider = authorityProvider;
            }
        }
    }
    if (page.version.isValid() && !replica) {
        for (const auto &candidate : candidates) {
            if (candidate.validity != KisReplicaValidity::Valid || candidate.replica.domain != access.domain) {
                continue;
            }
            const QSharedPointer<KisPageReplicaProvider> candidateProvider =
                d->owner.provider(candidate.replica.provider, candidate.replica.providerEpoch);
            if (!candidateProvider || !candidateProvider->capabilities().supports(access)) {
                continue;
            }
            if (replica) {
                replica = nullptr;
                provider.clear();
                break;
            }
            replica = &candidate;
            provider = candidateProvider;
        }
    }
    if (!replica || !provider) {
        return fail(QStringLiteral("requested exact generation has no compatible valid replica"));
    }

    KisPageTransactionId readTransaction;
    if (captured && captured->isTransactionOverlay()) {
        readTransaction = captured->d->overlayTransaction;
    } else if (view.kind == KisPageReadViewKind::TransactionOverlay) {
        readTransaction = view.transaction;
    }
    return d->readCoordinator.registerRequestLocked(version,
                                                    replica->replica,
                                                    access,
                                                    d->readyHostCompletion,
                                                    readTransaction, cleanup);
}

KisWriteRequest KisPageStore::acquireWrite(const KisPageTransaction &transaction,
                                           const KisPageKey &key,
                                           KisPageAccessRequirement access,
                                           KisPageWriteMode mode,
                                           KisPagePriority priority)
{
    KisPageStoreDiagnosticTimer diagnostic(this, KisPageStoreDiagnosticPhase::WriteAcquire, 1);
    KisWriteRequest request;
    request.access = access;
    request.mode = mode;
    QMutexLocker locker(&d->mutex);
    if (d->writeAdmission.pageClaimedLocked(key)
        || d->publicationCoordinator.isPreparingCommitLocked(transaction.id)) {
        request.error = QStringLiteral("page or transaction is already claimed for mutation");
        return request;
    }
    const auto activeTransaction = d->epochs.activeTransaction(transaction.id);
    if (!d->operational || !transaction.isValid() || !key.isValid() || !access.isValid()
        || (mode != KisPageWriteMode::PreserveContents && mode != KisPageWriteMode::DiscardContents)
        || !(activeTransaction == transaction)) {
        request.error = QStringLiteral("PageStore write transaction is invalid or unavailable");
        return request;
    }
    const auto fail = [&request](const QString &error = {}) {
        request.status = KisPageRequestStatus::Failed;
        if (!error.isNull()) request.error = error;
        return request;
    };
    {
        ++d->activeProviderCalls;
        const auto preparing = qScopeGuard([&] { --d->activeProviderCalls; });
        if (!d->writeCoordinator.beginPreparationActivity(transaction.id, locker, &request.error)) return fail();
    }
    const auto preparationClaim = qScopeGuard([&] {
        d->writeCoordinator.endPreparationActivity(transaction.id);
    });
    const auto currentTransaction = d->epochs.activeTransaction(transaction.id);
    if (!d->operational || !(currentTransaction == transaction)
        || d->publicationCoordinator.isPreparingCommitLocked(transaction.id))
        return fail(QStringLiteral("write transaction changed during admission"));
    const bool claimed = d->writeAdmission.claimDirectLocked(key, transaction.id.value, locker, &request.error);
    if (!claimed) {
        request.error = QStringLiteral("page is claimed by another writer");
        return request;
    }
    auto pageClaim = qScopeGuard([&] {
        d->writeAdmission.releaseDirectLocked(key, transaction.id.value);
    });
    KisPageWriteIntent writeIntent;
    writeIntent.key = key;
    writeIntent.mode = mode;
    writeIntent.flags |= quint8(KisPageWriteIntentFlag::AsyncLease);
    KisPageTransition transition;
    KisReplicaHandle recoverableBefore;
    KisPageAllocationDescriptor descriptor;
    if (!d->writeCoordinator.prepareWriteBaseLocked(transaction, writeIntent, d->publicationCoordinator,
                                                    &transition, &descriptor, &request.error, nullptr, &recoverableBefore)) {
        return fail();
    }
    const auto baseVersion = transition.baseVersion;
    const auto writeVersion = transition.version;
    const auto writeOperation = transition.operation;
    const auto writer = transition.writer;
    const auto requestId = d->owner.nextRequestId();
    const auto accessOperation = d->owner.nextOperationId();
    mode = transition.writeMode;
    request.mode = mode;
    auto provider = d->owner.provider(transition.source.provider, transition.source.providerEpoch);
    if (!provider || !provider->capabilities().supports(access))
        provider = d->owner.providerFor(access);
    if (!provider) {
        return fail(QStringLiteral("write target provider selection is unavailable or ambiguous"));
    }
    writeIntent.mode = mode;
    PendingWriteRequestRecord pending;
    pending.state = PendingWriteRequestRecord::State::Preparing;
    pending.baseVersion = baseVersion;
    pending.version = writeVersion;
    pending.source = transition.source;
    pending.access = access;
    pending.operation = writeOperation;
    pending.accessOperation = accessOperation;
    pending.transaction = transaction.id;
    pending.writer = writer;
    pending.writeMode = mode;
    if (!requestId.isValid() || !accessOperation.isValid()) {
        return fail(QStringLiteral("PageStore write identity allocation failed"));
    }
    try {
        // Prepare the actual terminal-owner node before either write plan can
        // acquire a private writer. Bucket capacity alone is not node storage.
        const auto inserted = d->writeRequests.emplace(requestId.value, pending);
        Q_ASSERT(inserted.second);
    } catch (const std::bad_alloc &) {
        return fail();
    }
    if (!d->writeCoordinator.beginGenericActivity(transaction.id)) {
        d->writeRequests.erase(requestId.value);
        return fail(QStringLiteral("generic write activity is unavailable or exhausted"));
    }
    pageClaim.dismiss(); // full request/lease lifetime now owns the page claim
    auto writeClaim = qScopeGuard([&] {
        d->releaseGenericWrite(transaction.id, key);
    });
    ++d->writeRequestsCreated;

    KisCpuWriteBindingReservation writable;
    KisPageWritePlanKind plan;
    try {
        plan = d->writeCoordinator.prepareWritePlanLocked(
            transaction, writeIntent, provider, access, descriptor,
            d->publicationCoordinator, transition, recoverableBefore, locker, writable, diagnostic);
    } catch (const std::bad_alloc &) {
        // The common plan restores this gate and destroys prepared candidates
        // outside it. All fallible preparation precedes physical handoff.
        d->writeRequests.erase(requestId.value);
        return fail();
    }
    if (plan == KisPageWritePlanKind::RecoverableHandoff) {
        // The preallocated request is the sole generic terminal owner. The
        // logical writer/admission survive releasing the short native pin;
        // resolve() acquires the ordinary generic provider access later.
        auto &prepared = d->writeRequests.find(requestId.value)->second;
        prepared.source = transition.source;
        prepared.replica = transition.target;
        prepared.readiness = d->readyHostCompletion;
        prepared.state = PendingWriteRequestRecord::State::Pending;
        request.status = KisPageRequestStatus::Ready;
        request.id = requestId;
        request.transaction = transaction.id;
        request.baseVersion = baseVersion;
        request.writeVersion = writeVersion;
        request.writer = writer;
        request.readiness = prepared.readiness;
        Q_ASSERT(request.isValid());
        writeClaim.dismiss();
        locker.unlock();
        writable.reset();
        locker.relock();
        return request;
    }
    decltype(d->publicationCoordinator.prepareDescriptorLocked(writeVersion, descriptor)) descriptors;
    const auto discardDescriptors = qScopeGuard([&] {
        if (locker.isLocked()) locker.unlock();
        descriptors = {};
        locker.relock();
    });
    try { descriptors = d->publicationCoordinator.prepareDescriptorLocked(writeVersion, descriptor); }
    catch (const std::bad_alloc &) {
        d->writeRequests.erase(requestId.value);
        return fail(QStringLiteral("write descriptor storage budget was refused"));
    }
    KisPageBackingPreparation backing;
    ++d->activeProviderCalls;
    {
        locker.unlock();
        const auto done = qScopeGuard([&] { locker.relock(); --d->activeProviderCalls; });
        backing = d->writeCoordinator.reserveBacking(descriptor, access.domain,
            KisBackingBudgetClass::ActivePending, &request.error, writeVersion);
    }
    const auto discardBacking = qScopeGuard([&] {
        if (!backing.retirement) return;
        locker.unlock(); backing.retirement.reset(); locker.relock();
    });
    if (!backing.reservation.isValid()) {
        d->writeRequests.erase(requestId.value);
        return fail();
    }

    bool nativeWriteCopy = false;

    // Provider allocation is deliberately outside the PageStore lock. The
    // provisional request keeps close/abort from crossing this operation; the
    // metadata transition below revalidates writer/generation ownership.
    locker.unlock();
    KisReplicaOperation allocation;
    try {
        KisPageStoreDiagnosticTimer phase(this, KisPageStoreDiagnosticPhase::WriteProviderPrepare, 1);
        allocation = d->writeCoordinator.prepareFreshReplica(writeIntent, *provider,
            transition, descriptor, access, priority, nullptr, {}, &nativeWriteCopy);
    } catch (const std::bad_alloc &) {
        locker.relock();
        d->writeRequests.erase(requestId.value);
        return fail();
    }
    locker.relock();

    QString failure;
    const bool ownedReplica = ownsPreparedReplica(allocation.replica, writeVersion, *provider, pending.source);
    const bool backingOwned = ownedReplica
        && d->owner.registerBacking(allocation.replica, backing.reservation,
                                    KisBackingBudgetClass::ActivePending, &failure, &backing.retirement);

    const auto rejectAllocation = [&](const QString &message, bool removeRequest = true) {
        if (ownedReplica)
            d->retireRejectedReplicaLocked(allocation.replica, provider, std::move(backing), locker);
        if (removeRequest) d->writeRequests.erase(requestId.value);
        request.status = KisPageRequestStatus::Failed;
        request.error = message;
    };
    auto pendingIt = d->writeRequests.find(requestId.value);
    if (pendingIt == d->writeRequests.end() || pendingIt->second.state != PendingWriteRequestRecord::State::Preparing) {
        rejectAllocation(QStringLiteral("write allocation reservation was lost"), false);
        return request;
    }

    if (!backingOwned || !allocation.isValid()
        || (nativeWriteCopy && allocation.status != KisPageRequestStatus::Ready)
        || allocation.replica.domain != access.domain
        || !allocation.replica.layout.matches(descriptor)) {
        rejectAllocation(QStringLiteral("write allocation failed"));
        return request;
    }
    if (!d->owner.verifyTerminalProviderResult(writeOperation, allocation, &failure).succeeded()) {
        rejectAllocation(failure.isEmpty() ? QStringLiteral("write allocation completion failed") : failure);
        return request;
    }
    pendingIt->second.replica = allocation.replica;
    transition.target = allocation.replica;
    KisPageMetadataTransitionResult stateResult = d->writeCoordinator.preparePrivateWrite(transition, false);
    if (!stateResult.accepted) {
        rejectAllocation(stateResult.rejectionReason);
        return request;
    }

    d->publicationCoordinator.installDescriptorAdditionsLocked(&descriptors);
    pendingIt->second.source = transition.source;
    pendingIt->second.readiness = allocation.completion;
    const auto rejectPrepared = [&](const QString &message) {
        const auto cancelled = d->cancelWriteLocked(d->writeRequests.find(requestId.value)->second);
        if (cancelled.accepted) {
            d->writeRequests.erase(requestId.value);
            d->processRetirementsLocked(locker);
        } else {
            // No request capability was returned, so transaction abort owns
            // the retry. Do not release its only page/admission record.
            d->writeRequests.find(requestId.value)->second.state = PendingWriteRequestRecord::State::Pending;
            writeClaim.dismiss();
        }
        request.status = KisPageRequestStatus::Failed;
        request.error = message;
    };
    locker.unlock();
    KisCompletionTicket initializationReadiness;
    try {
        KisPageStoreDiagnosticTimer phase(this, KisPageStoreDiagnosticPhase::WriteProviderTransfer, 1);
        initializationReadiness = d->writeCoordinator.initializeFreshReplica(
            writeIntent, nativeWriteCopy, pending.source, allocation.replica,
            descriptor, provider, priority, allocation.completion, &failure);
    } catch (const std::bad_alloc &) {
        // A private writer already exists. Use its original cancellation
        // record; a rejected detach must remain owned by transaction abort.
        failure = QStringLiteral("write initialization storage allocation failed");
    }
    locker.relock();
    pendingIt = d->writeRequests.find(requestId.value);
    if (pendingIt == d->writeRequests.end() || pendingIt->second.state != PendingWriteRequestRecord::State::Preparing) {
        return fail(QStringLiteral("write initialization reservation was lost"));
    }
    if (!initializationReadiness.isValid()) {
        rejectPrepared(failure.isEmpty() ? QStringLiteral("write generation initialization failed") : failure);
        return request;
    }
    pendingIt->second.readiness = initializationReadiness;

    transition.kind = KisPageTransitionKind::PrepareWrite;
    stateResult = d->metadata.applyOwner(key, transition);
    if (!stateResult.accepted) {
        rejectPrepared(stateResult.rejectionReason);
        return request;
    }

    request.status = KisPageRequestStatus::Ready;
    request.id = requestId;
    request.transaction = transaction.id;
    request.baseVersion = baseVersion;
    request.writeVersion = writeVersion;
    request.writer = writer;
    request.readiness = pendingIt->second.readiness;
    if (!request.isValid()) {
        rejectPrepared(QStringLiteral("PageStore write request construction failed"));
        return request;
    }
    pendingIt->second.state = PendingWriteRequestRecord::State::Pending;
    writeClaim.dismiss(); // request -> lease -> publish/cancel owns the count
    return request;
}


KisReadLease KisPageStore::resolve(const KisReadRequest &request, const KisCompletionTicket &completion)
{
    KisPageReadCleanup cleanup(d->readCoordinator);
    QMutexLocker locker(&d->mutex);
    return d->readCoordinator.resolveLocked(request, completion, locker, cleanup);
}

KisWriteLease KisPageStore::resolve(const KisWriteRequest &request, const KisCompletionTicket &completion)
{
    KisPageStoreDiagnosticTimer diagnostic(this, KisPageStoreDiagnosticPhase::WriteResolve, 1);
    QMutexLocker locker(&d->mutex);
    auto requestIt = d->writeRequests.find(request.id.value);
    if (!d->operational || !request.isValid() || requestIt == d->writeRequests.end()
        || requestIt->second.state != PendingWriteRequestRecord::State::Pending
        || !(requestIt->second.transaction == request.transaction)
        || !(requestIt->second.baseVersion == request.baseVersion)
        || !(requestIt->second.version == request.writeVersion)
        || !(requestIt->second.writer == request.writer)
        || !(requestIt->second.access == request.access) || requestIt->second.writeMode != request.mode
        || !(requestIt->second.readiness == request.readiness)
        || !(requestIt->second.readiness == completion) || !d->completions->verifyTerminal(completion).succeeded()) {
        return {};
    }
    const QSharedPointer<KisPageReplicaProvider> provider =
        d->owner.provider(requestIt->second.replica.provider, requestIt->second.replica.providerEpoch);
    if (!provider)
        return {};

    const KisPageLeaseId leaseId = d->owner.nextLeaseId();
    if (!leaseId.isValid())
        return {};
    const PendingWriteRequestRecord pending = requestIt->second;
    std::shared_ptr<ActiveWriteRecord> active;
    ActiveWriteMap::node_type preparedNode;
    try {
        active = std::make_shared<ActiveWriteRecord>(provider, pending);
        ActiveWriteMap prepared;
        prepared.emplace(leaseId.value, active);
        preparedNode = prepared.extract(prepared.begin());
    } catch (const std::bad_alloc &) {
        // The original Pending request still owns the writer and admission.
        return {};
    }
    requestIt->second.state = PendingWriteRequestRecord::State::Resolving;
    locker.unlock();
    try {
        active->access.emplace(provider->resolveAccess(leaseId,
            pending.accessOperation, pending.replica, pending.access, KisPageAccessMode::Write));
    } catch (const std::bad_alloc &) {
        locker.relock();
        requestIt = d->writeRequests.find(request.id.value);
        if (requestIt != d->writeRequests.end()
            && requestIt->second.state == PendingWriteRequestRecord::State::Resolving)
            requestIt->second.state = PendingWriteRequestRecord::State::Pending;
        return {};
    }
    locker.relock();
    auto &access = *active->access;
    requestIt = d->writeRequests.find(request.id.value);
    if (requestIt == d->writeRequests.end() || requestIt->second.state != PendingWriteRequestRecord::State::Resolving) {
        if (access.isValid()) {
            locker.unlock();
            provider->releaseAccess(std::move(access), {});
            locker.relock();
        }
        return {};
    }
    if (!access.isValid(pending.access) || access.mode != KisPageAccessMode::Write
        || !(access.lease == leaseId) || !(access.operation == pending.accessOperation)
        || !(access.replica == pending.replica)) {
        if (access.isValid()) {
            locker.unlock();
            provider->releaseAccess(std::move(access), {});
            locker.relock();
            requestIt = d->writeRequests.find(request.id.value);
            if (requestIt == d->writeRequests.end()) return {};
        }
        requestIt->second.state = PendingWriteRequestRecord::State::Pending;
        return {};
    }

    KisWriteLease lease;
    lease.m_leaseId = leaseId;
    lease.m_baseVersion = pending.baseVersion;
    lease.m_version = pending.version;
    lease.m_writer = pending.writer;
    lease.m_transaction = pending.transaction;
    lease.m_access = pending.access;
    lease.m_cpuData = access.cpuWriteData;
    lease.m_gpuAccess = access.gpuAccess;
    lease.m_layout = pending.replica.layout;
    lease.m_lifetime = active;
    // Resolving excludes another consumer; the unique ID and prepared node
    // make installation independent of index growth while the gate was open.
    const auto installed = d->activeWrites.insert(std::move(preparedNode));
    Q_ASSERT(installed.inserted);
    d->writeRequests.erase(requestIt);
    return lease;
}

bool KisPageStore::cancel(const KisReadRequest &request)
{
    KisPageReadCleanup cleanup(d->readCoordinator);
    QMutexLocker locker(&d->mutex);
    return d->readCoordinator.cancelLocked(request, locker, cleanup);
}

bool KisPageStore::cancel(const KisWriteRequest &request)
{
    QMutexLocker locker(&d->mutex);
    auto requestIt = d->writeRequests.find(request.id.value);
    if (!d->operational || !request.isValid() || requestIt == d->writeRequests.end()
        || requestIt->second.state != PendingWriteRequestRecord::State::Pending
        || !(requestIt->second.transaction == request.transaction)
        || !(requestIt->second.baseVersion == request.baseVersion) || !(requestIt->second.version == request.writeVersion)
        || !(requestIt->second.writer == request.writer) || !(requestIt->second.readiness == request.readiness)
        || !(requestIt->second.access == request.access) || requestIt->second.writeMode != request.mode) {
        return false;
    }
    const auto result = d->cancelWriteLocked(requestIt->second);
    if (!result.accepted)
        return false;
    d->releaseGenericWrite(requestIt->second.transaction, requestIt->second.version.key);
    d->writeRequests.erase(requestIt);
    d->processRetirementsLocked(locker);
    return true;
}

void KisPageStore::release(KisReadLease lease, const KisCompletionTicket &consumerLastUse)
{
    KisPageReadCleanup cleanup(d->readCoordinator);
    QMutexLocker locker(&d->mutex);
    d->readCoordinator.retryReleasedReadsLocked(locker, cleanup);
    d->readCoordinator.releaseLocked(std::move(lease), consumerLastUse, locker, cleanup);
}

bool KisPageStore::acknowledgeLastUse(const KisVerifiedCompletion &completion)
{
    if (!completion.isValid())
        return false;
    KisPageReadCleanup cleanup(d->readCoordinator);
    QMutexLocker locker(&d->mutex);
    bool retriedAcknowledge = false;
    d->readCoordinator.retryReleasedReadsLocked(locker, cleanup, completion.ticket(), &retriedAcknowledge);
    const auto acknowledged = d->readCoordinator.acknowledgeLastUseLocked(completion, locker, cleanup);
    return (retriedAcknowledge || acknowledged.matched) && acknowledged.accepted;
}

KisCompletionTicket
KisPageStore::archiveRead(const KisPageKey &key, const KisPageReadView &view, KisPagePriority priority, QString *error)
{
    const KisPageAccessRequirement cpuAccess{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
    const KisReadRequest request = acquireRead(key, view, cpuAccess, priority);
    if (!request.isValid()) {
        KisPageStoreDetail::setError(error, request.error.isEmpty() ? QStringLiteral("archive read acquire failed") : request.error);
        return {};
    }
    KisReadLease lease = resolve(request, request.readiness);
    if (!lease.isValid()) {
        cancel(request);
        KisPageStoreDetail::setError(error, QStringLiteral("archive read resolve failed"));
        return {};
    }

    QSharedPointer<KisExactGenerationArchive> archive;
    QSharedPointer<KisCompletionRegistry> completions;
    KisPageAllocationDescriptor descriptor;
    KisPageOperationId operation;
    {
        QMutexLocker locker(&d->mutex);
        archive = d->exactArchive;
        completions = d->completions;
        d->publicationCoordinator.descriptorLocked(lease.version(), &descriptor);
        operation = d->owner.nextOperationId();
        if (!d->operational || !archive || !archive->isOperational() || !descriptor.isValid() || !operation.isValid()) {
            locker.unlock();
            release(std::move(lease));
            KisPageStoreDetail::setError(error, QStringLiteral("exact-generation archive is unavailable"));
            return {};
        }
        ++d->activeProviderCalls;
    }

    KisExactPageArchiveWrite write;
    write.operation = operation;
    write.version = lease.version();
    write.descriptor = descriptor;
    write.sourceLayout = {descriptor.layoutRevision,
                          descriptor.format.formatId,
                          descriptor.pageExtent,
                          descriptor.validRect,
                          lease.rowStride(),
                          lease.byteSize()};
    write.sourceData = lease.cpuData();
    const KisPageArchiveOperation archived = archive->storeExact(write, priority);

    bool accepted = false;
    QString failure = archived.error;
    {
        QMutexLocker locker(&d->mutex);
        --d->activeProviderCalls;
        const bool identityMatches =
            archived.isValid() && archived.operation == operation && archived.version == request.version;
        if (!d->operational || d->closed || !identityMatches) {
            if (failure.isEmpty()) {
                failure = QStringLiteral("exact-generation archive returned a mismatched operation");
            }
        } else if (archived.status == KisPageRequestStatus::Ready) {
            const KisVerifiedCompletion terminal = completions->verifyTerminal(archived.completion);
            accepted = terminal.isValid() && terminal.succeeded();
            if (!accepted && failure.isEmpty()) {
                failure = QStringLiteral("exact-generation archive completed unsuccessfully");
            }
        } else {
            PendingArchiveRecord record;
            record.version = request.version;
            record.completion = archived.completion;
            record.archive = archive;
            const bool newPending = completions->status(archived.completion) != KisCompletionStatus::Unknown
                && !d->pendingArchives.contains(operation.value);
            if (newPending) {
                d->pendingArchives.insert(operation.value, record);
                accepted = true;
            } else if (failure.isEmpty()) {
                failure = QStringLiteral("exact-generation archive completion is foreign or duplicated");
            }
        }
    }
    release(std::move(lease));

    if (accepted && archived.status == KisPageRequestStatus::Ready && !archive->contains(request.version)) {
        accepted = false;
        failure = QStringLiteral("archive reported success without an exact-generation record");
    }
    if (!accepted) {
        KisPageStoreDetail::setError(error, failure.isEmpty() ? QStringLiteral("exact-generation archive write failed") : failure);
        return {};
    }
    KisPageStoreDetail::setError(error, {});
    return archived.completion;
}

QVector<KisPageArchiveCompletionEvent> KisPageStore::pollArchiveCompletions(qsizetype maximumCount)
{
    QVector<KisPageArchiveCompletionEvent> events;
    if (maximumCount <= 0)
        return events;

    struct Candidate {
        KisPageOperationId operation;
        PendingArchiveRecord record;
        KisVerifiedCompletion terminal;
    };
    QVector<Candidate> candidates;
    {
        QMutexLocker locker(&d->mutex);
        if (!d->completions)
            return events;
        for (auto it = d->pendingArchives.begin(); it != d->pendingArchives.end() && candidates.size() < maximumCount;
             ++it) {
            if (it->busy)
                continue;
            const KisVerifiedCompletion terminal = d->completions->verifyTerminal(it->completion);
            if (!terminal.isValid())
                continue;
            it->busy = true;
            candidates.append({KisPageOperationId{it.key()}, it.value(), terminal});
        }
    }

    for (const Candidate &candidate : std::as_const(candidates)) {
        KisPageArchiveCompletionEvent event;
        event.operation = candidate.operation;
        event.version = candidate.record.version;
        event.completion = candidate.record.completion;
        event.status = candidate.terminal.status();
        if (event.status == KisCompletionStatus::Succeeded
            && (!candidate.record.archive || !candidate.record.archive->contains(candidate.record.version))) {
            event.status = KisCompletionStatus::Failed;
            event.error = QStringLiteral("archive reported success without an exact-generation record");
        } else if (event.status == KisCompletionStatus::Failed) {
            event.error = QStringLiteral("exact-generation archive write failed");
        } else if (event.status == KisCompletionStatus::Cancelled) {
            event.error = QStringLiteral("exact-generation archive write was cancelled");
        }

        QMutexLocker locker(&d->mutex);
        auto it = d->pendingArchives.find(candidate.operation.value);
        if (it == d->pendingArchives.end() || !(it->completion == candidate.record.completion)
            || !(it->version == candidate.record.version)) {
            continue;
        }
        d->pendingArchives.erase(it);
        events.append(event);
    }
    return events;
}

bool KisPageStore::cancelArchive(const KisCompletionTicket &completion)
{
    PendingArchiveRecord record;
    KisPageOperationId operation;
    {
        QMutexLocker locker(&d->mutex);
        if (!d->operational || !completion.isValid())
            return false;
        auto selected = d->pendingArchives.end();
        for (auto it = d->pendingArchives.begin(); it != d->pendingArchives.end(); ++it) {
            if (it->completion == completion) {
                selected = it;
                break;
            }
        }
        if (selected == d->pendingArchives.end() || selected->busy
            || !selected->archive) {
            return false;
        }
        operation = KisPageOperationId{selected.key()};
        selected->busy = true;
        record = selected.value();
        ++d->activeProviderCalls;
    }

    const bool cancelled = record.archive->cancelStoreExact(operation);
    QMutexLocker locker(&d->mutex);
    --d->activeProviderCalls;
    auto it = d->pendingArchives.find(operation.value);
    if (it != d->pendingArchives.end() && it->completion == record.completion) {
        it->busy = false;
    }
    return cancelled;
}

KisCompletionTicket KisPageStore::publish(KisWriteLease lease, const KisCompletionTicket &producerCompletion)
{
    return finishWrite(std::move(lease), producerCompletion);
}

KisCompletionTicket KisPageStore::finishWrite(KisWriteLease lease, const KisCompletionTicket &completion)
{
    QMutexLocker lock(&d->mutex);
    const auto it = d->activeWrites.find(lease.m_leaseId.value);
    const auto active = it == d->activeWrites.end() ? nullptr : it->second;
    if (!lease.isValid() || !active || lease.m_lifetime.get() != active.get())
        return {};
    if (!d->writeCoordinator.beginPreparationActivity(active->request.transaction, lock))
        return {};
    const auto preparationClaim = qScopeGuard([&] {
        d->writeCoordinator.endPreparationActivity(active->request.transaction);
    });
    lock.unlock();
    active->provider->releaseAccess(std::move(*active->access), completion);
    lock.relock();

    const auto &request = active->request;
    bool success = d->completions->verifyTerminal(completion).succeeded();
    KisPreparedPageProof proof;
    if (success) {
        KisPageAllocationDescriptor descriptor;
        d->publicationCoordinator.descriptorLocked(request.version, &descriptor);
        // The lease's admission and preparation activity protect this private
        // generation. Provider validation must not run under the store gate.
        lock.unlock();
        success = d->writeCoordinator.publishPrivateWrite(
                request.transition(KisPageTransitionKind::BeginPublish)).accepted
            && d->owner.sealPreparedPage(d->metadata, request.version, request.transaction,
                                         descriptor, completion, &proof);
        lock.relock();
    }
    KisPagePublicationCoordinator::KisPreparedOverlayUpdate overlay;
    if (success) {
        const auto transaction = d->epochs.activeTransaction(request.transaction);
        if (transaction.isValid()) {
            const KisPagePublicationCoordinator::OverlayChange change{request.version.key, proof, false};
            overlay = d->publicationCoordinator.prepareOverlayUpdateLocked(
                transaction, &change, 1, nullptr);
        }
        success = overlay.isValid();
        if (success)
            proof = {};
    }
    KisPageMetadataCoordinator::DeferredPublicationCleanup metadataCleanup;
    if (success) {
        lock.unlock();
        success = overlay.prepare(nullptr);
        lock.relock();
    }
    if (success) {
        success = overlay.prepareSurfaceLocked(nullptr) && overlay.tryInstallLocked(
            &metadataCleanup, nullptr);
        if (success)
            overlay.collectRetirementsLocked();
    }
    const auto disposeCleanup = qScopeGuard([&] {
        lock.unlock();
        disposeDeferredMetadataCleanup(
            std::move(metadataCleanup), d->metadataCleanupStatistics);
        overlay = {};
        lock.relock();
    });
    if (!success) {
        if (proof.isValid()) d->owner.revokePreparedPage(proof);
        const auto cancelled = d->cancelWriteLocked(request);
        if (!cancelled.accepted)
            return {}; // consumed lease remains abort-retryable in activeWrites
    }
    d->activeWrites.erase(lease.m_leaseId.value);
    d->processRetirementsLocked(lock);
    d->releaseGenericWrite(request.transaction, request.version.key);
    return success ? completion : KisCompletionTicket{};
}

KisCompletionTicket KisPageStore::publishHostWrite(KisWriteLease lease)
{
    KisPageStoreDiagnosticTimer diagnostic(this, KisPageStoreDiagnosticPhase::WritePublishHost, 1);
    if (!lease.isValid() || lease.accessKind() != KisPageAccessKind::CpuPointer) {
        return {};
    }
    KisCompletionTicket completion;
    {
        QMutexLocker locker(&d->mutex);
        if (!d->operational)
            return {};
        completion = d->readyHostCompletion;
    }
    return completion.isValid() ? publish(std::move(lease), completion) : KisCompletionTicket();
}

void KisPageStore::cancel(KisWriteLease lease)
{
    finishWrite(std::move(lease), {});
}

KisPageTransaction KisPageStore::beginTransaction(KisImageEpochId baseEpoch)
{
    using Phase = KisPageStoreDiagnosticPhase;
    KisPageStoreDiagnosticTimer diagnostic(this, Phase::TransactionBeginOwnerWait, 1);
    QMutexLocker locker(&d->mutex);
    diagnostic.next(Phase::TransactionBegin, 1);
    return d->operational ? d->epochs.beginTransaction(baseEpoch) : KisPageTransaction();
}

KisPageTransaction KisPageStore::beginCurrentTransaction()
{
    using Phase = KisPageStoreDiagnosticPhase;
    KisPageStoreDiagnosticTimer diagnostic(this, Phase::TransactionBeginOwnerWait, 1);
    QMutexLocker locker(&d->mutex);
    diagnostic.next(Phase::TransactionBegin, 1);
    if (!d->operational)
        return {};
    // Epoch capture and transaction retention share the PageStore owner gate;
    // a concurrent commit/GC cannot discard this base between the two steps.
    return d->epochs.beginTransaction(d->epochs.captureCommittedRoot().epoch());
}

bool KisPageStore::stageSurfaceMetadata(const KisPageTransaction &transaction,
                                        const KisSurfaceEpochState &after,
                                        QString *error)
{
    QMutexLocker locker(&d->mutex);
    return d->publicationCoordinator.stageSurfaceMetadataLocked(transaction, after, error);
}

bool KisPageStore::stageSurfaceDefaultPixel(const KisPageTransaction &transaction,
                                            KisSurfaceId surface,
                                            const QByteArray &pixel,
                                            QString *error)
{
    QMutexLocker locker(&d->mutex);
    return d->publicationCoordinator.stageSurfaceDefaultPixelLocked(transaction, surface, pixel, error);
}

bool KisPageStore::stagePageRemovalIfUnchanged(const KisPageTransaction &transaction,
                                               const KisPageVersion &observed,
                                               QString *error)
{
    if (!observed.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("conditional page removal requires an exact version"));
        return false;
    }
    QMutexLocker locker(&d->mutex);
    return d->publicationCoordinator.stagePageRemovalLocked(transaction, observed, error, locker);
}

KisPreparedPageSet KisPageStore::preparedPages(const KisPageTransaction &transaction) const
{
    QMutexLocker locker(&d->mutex);
    return d->publicationCoordinator.preparedPagesLocked(transaction);
}

KisImageEpochCommitTicket KisPageStore::commit(const KisPageTransaction &transaction,
                                               const KisPreparedPageSet &preparedPages,
                                               KisRetainedImageEpochSnapshot *retainedAfter)
{
    if (transaction.id.isValid())
        KisPageMutationSession::Private::retryOrphans(d.data(), transaction.id);
    QMutexLocker locker(&d->mutex);
    return d->publicationCoordinator.commitLocked(transaction, preparedPages, retainedAfter, this, locker);
}
KisImageEpochCommitTicket KisPageStore::commitAndReleasePublicationLock(
    const KisPageTransaction &transaction, const KisPreparedPageSet &preparedPages,
    QWriteLocker &publicationLock)
{
    if (transaction.id.isValid())
        KisPageMutationSession::Private::retryOrphans(d.data(), transaction.id);
    QMutexLocker locker(&d->mutex);
    return d->publicationCoordinator.commitLocked(transaction, preparedPages, nullptr, this, locker,
                                                  &publicationLock);
}

KisImageEpochCommitTicket KisPageStore::restoreRetainedEpoch(const KisRetainedImageEpochSnapshot &retained)
{
    QMutexLocker locker(&d->mutex);
    return d->publicationCoordinator.restoreRetainedEpochLocked(retained, nullptr, this, locker);
}

KisImageEpochCommitTicket KisPageStore::restoreRetainedEpochDelta(const KisRetainedImageEpochSnapshot &retained,
                                                                  const QVector<KisPageKey> &changedPages)
{
    QMutexLocker locker(&d->mutex);
    return d->publicationCoordinator.restoreRetainedEpochLocked(retained, &changedPages, this, locker);
}
bool KisPageStore::abort(const KisPageTransaction &transaction)
{
    if (transaction.id.isValid())
        KisPageMutationSession::Private::retryOrphans(d.data(), transaction.id);
    KisPageReadCleanup cleanup(d->readCoordinator);
    QMutexLocker locker(&d->mutex);
    return d->publicationCoordinator.abortLocked(transaction, locker, cleanup);
}
KisImageEpochSnapshot KisPageStore::captureCommittedEpoch() const
{
    using Phase = KisPageStoreDiagnosticPhase;
    KisPageStoreDiagnosticTimer diagnostic(this, Phase::ManifestExportOwnerWait, 1);
    QMutexLocker locker(&d->mutex);
    if (!d->operational)
        return {};
    const auto root = d->epochs.captureCommittedRoot();
    diagnostic.next(Phase::ManifestExport, quint64(root.pageCount()));
    return root.snapshot();
}

KisRetainedImageEpochSnapshot KisPageStore::captureRetainedEpoch()
{
    QMutexLocker locker(&d->mutex);
    return d->operational ? d->epochs.captureRetainedSnapshot() : KisRetainedImageEpochSnapshot();
}

KisRetainedImageEpochSnapshot KisPageStore::captureRetainedEpochRoot()
{
    QMutexLocker locker(&d->mutex);
    return d->operational ? d->epochs.captureRetainedRoot() : KisRetainedImageEpochSnapshot();
}

bool KisPageStore::validateRetainedEpoch(const KisRetainedImageEpochSnapshot &retained) const
{
    QMutexLocker locker(&d->mutex);
    return d->operational && d->epochs.validateRetainedSnapshot(retained);
}

bool KisPageStore::releaseSnapshot(KisImageEpochSnapshotToken token)
{
    return d->readCoordinator.releaseSnapshot(token, nullptr);
}

bool KisPageStore::releaseSnapshotDelta(KisImageEpochSnapshotToken token, const QVector<KisPageKey> &changedPages)
{
    return d->readCoordinator.releaseSnapshot(token, &changedPages);
}

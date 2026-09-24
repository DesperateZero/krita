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
#include <cstddef>
#include <cstring>
#include <deque>
#include <limits>
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

bool needsDefaultMaterialization(const KisPageVersionStateSnapshot *state,
                                 const KisPageVersion &version)
{
    return version.isDefaultPixel() && (!state || state->isVirtualDefault());
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
                      KisReplicaAccess &&accessValue,
                      WriteClosureRecord &&requestValue)
        : provider(providerValue)
        , access(std::move(accessValue))
        , request(std::move(requestValue))
    {
    }

    ~ActiveWriteRecord()
    {
        if (provider && access.isValid()) {
            provider->releaseAccess(std::move(access), {});
        }
    }

    QSharedPointer<KisPageReplicaProvider> provider;
    KisReplicaAccess access;
    WriteClosureRecord request;
};

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
        : writeAdmission(mutex, writeAdmissionChanged)
        , writeCoordinator(metadata, epochs, backingBudget, owner)
        , defaultStorage(backingBudget)
        , retirementQueue(owner, metadata, lifetimeReferences, this, &Private::releaseRetirementLifetime)
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
        owner.attachBackingBudget(backingBudget);
        metadata.attachBackingBudget(backingBudget);
        metadata.attachRetirementDebtOwner(&owner,
                                           &Private::prepareRetirementDebt,
                                           &Private::commitRetirementDebt,
                                           &Private::cancelRetirementDebt);
    }

    static bool prepareRetirementDebt(void *context,
                                      const QVector<KisPageTransitionEffect> &effects,
                                      quint64 *cookie,
                                      QString *error)
    {
        auto *ledger = static_cast<KisPageOwnerLedger *>(context);
        return ledger->prepareRetirementDebt(effects, cookie, error);
    }

    static void commitRetirementDebt(void *context, quint64 cookie) noexcept
    {
        static_cast<KisPageOwnerLedger *>(context)->commitRetirementDebt(cookie);
    }

    static void cancelRetirementDebt(void *context, quint64 cookie) noexcept
    {
        static_cast<KisPageOwnerLedger *>(context)->cancelRetirementDebt(cookie);
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

    static void beginPublicationMutationPreparation(void *context, KisPageTransactionId transaction)
    {
        auto *owner = static_cast<KisPageStore::Private *>(context);
        owner->writeCoordinator.beginPreparationActivity(transaction);
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
            && stats.activeProviderCalls == 0
            && owner->owner.publicationBlockingOperationCount() == 0 && stats.sealedPreparedProofs == 0;
    }

    static bool preparePublicationAbort(void *context,
                                        KisPageTransactionId transaction,
                                        QVector<KisPageTransitionEffect> *retirementEffects)
    {
        auto *owner = static_cast<KisPageStore::Private *>(context);
        if (!retirementEffects || owner->writeCoordinator.transactionHasSessionOrPreparation(transaction)) {
            return false;
        }
        for (auto it = owner->activeWrites.begin(); it != owner->activeWrites.end();) {
            const auto active = it.value();
            if (!(active->request.transaction == transaction)) {
                ++it;
                continue;
            }
            // Consumed leases retain admission after a failed detach. Abort
            // retries them; an exposed or currently releasing lease blocks it.
            if (active->access.isValid()) return false;
            const auto result = owner->cancelWriteLocked(active->request);
            if (!result.accepted) return false;
            *retirementEffects += result.effects;
            owner->releaseGenericWrite(transaction, active->request.version.key);
            it = owner->activeWrites.erase(it);
        }
        if (owner->readCoordinator.protectsPreparedTransactionLocked(transaction)
            || !owner->readCoordinator.cancelPreparedRequestsLocked(transaction)) {
            return false;
        }
        for (auto it = owner->writeRequests.begin(); it != owner->writeRequests.end();) {
            if (!(it->transaction == transaction)) {
                ++it;
                continue;
            }
            if (it->state != PendingWriteRequestRecord::State::Pending)
                return false;
            const auto result = owner->cancelWriteLocked(it.value());
            if (!result.accepted)
                return false;
            *retirementEffects += result.effects;
            owner->releaseGenericWrite(it->transaction, it->version.key);
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
            qsizetype(retirementSnapshot.jobScheduled) + qsizetype(historySnapshot.jobScheduled);
        result.backgroundRetirementPasses = retirementSnapshot.backgroundPasses;
        result.maximumReplicasPerRetirementPass = retirementSnapshot.maximumReplicasPerPass;
        result.peakRetiredReplicas = retirementSnapshot.peakPendingReplicas;
        result.peakRetiredBytes = retirementSnapshot.peakPendingBytes;
        result.retirementRetryRequeues = retirementSnapshot.retryRequeues;
        result.retirementCloseDrainedReplicas = retirementSnapshot.closeDrainedReplicas;
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

    KisPageTransitionResult cancelWriteLocked(const WriteClosureRecord &request)
    {
        auto result = writeCoordinator.cancelPrivateWrite(request.transition(KisPageTransitionKind::CancelWrite));
        if (result.accepted)
            publicationCoordinator.removeDescriptorLocked(request.version);
        return result;
    }

    void retireEffectsLocked(const QVector<KisPageTransitionEffect> &effects, QMutexLocker<QMutex> &lock)
    {
        if (effects.isEmpty()) return;
        ++activeProviderCalls;
        lock.unlock();
        retirementQueue.retireEffects(effects, backgroundReclamation);
        lock.relock();
        --activeProviderCalls;
    }

    void retireRejectedReplicaLocked(const KisReplicaHandle &replica,
                                     const QSharedPointer<KisPageReplicaProvider> &provider,
                                     KisBackingBudgetReservation &&backing,
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
        KisPageStateSnapshot existing;
        const auto isMaterialized = [&] {
            if (!metadata.versionSnapshot(version, &existing)) return false;
            const auto *existingVersion = existing.findVersion(version);
            return existingVersion && !needsDefaultMaterialization(existingVersion, version);
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
        while (operational && defaultStorage.preparationBlockedLocked(version.key)) {
            defaultStorage.waitForPreparationChangeLocked(&mutex);
        }
        if (!operational || closed) {
            KisPageStoreDetail::setError(error, QStringLiteral("implicit default page preparation was interrupted"));
            return false;
        }
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
        auto backing = writeCoordinator.reserveBacking(descriptor, access.domain, backingClass, error);
        if (!backing.isValid()) return false;

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
            allocation.replica, backing, backingClass, error);

        auto retireRejected = [&]() {
            if (!ours)
                return;
            retireRejectedReplicaLocked(allocation.replica, provider, std::move(backing), *locker);
        };

        QString failure;
        if (!operational || closed || !backingOwned || !allocation.isValid()
            || allocation.replica.domain != access.domain
            || !allocation.replica.layout.matches(descriptor)
            || !owner.consumeTerminalProviderOperation(operation, allocation, &failure).succeeded()) {
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
            const auto *existingVersion = existing.findVersion(version);
            if (existingVersion && !needsDefaultMaterialization(existingVersion, version)) {
                retireRejected();
                KisPageStoreDetail::setError(error, {});
                return true;
            }
            KisPageTransition attach;
            attach.kind = existingVersion ? KisPageTransitionKind::MaterializeDefault
                                          : KisPageTransitionKind::AttachHistoricalDefault;
            attach.version = version;
            attach.target = allocation.replica;
            const KisPageTransitionResult attached = metadata.applyOwner(version.key, attach);
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
        publicationCoordinator.putDescriptorLocked(version, descriptor);
        KisPageStoreDetail::setError(error, {});
        return true;
    }

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
    bool backingLimitsConfigured = false;
    bool operational = false;
    bool closing = false;
    bool closed = false;
    bool backgroundReclamation = true;
    qsizetype activeProviderCalls = 0;
    QString closeFailure;
    QSharedPointer<KisCompletionRegistry> completions;
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
    QVector<KisPageRetirementRecord> pendingShutdownReplicas;
    QHash<quint64, PendingWriteRequestRecord> writeRequests;
    QHash<quint64, std::shared_ptr<ActiveWriteRecord>> activeWrites;
    std::vector<KisPageMutationSession::Private *> orphanedMutations;
    // Transaction lifetime claims and per-page write claims, not request or
    // access-lease maps. The issued scope owns unpublished native records.
    KisPageMutationStatistics mutationStats;
    quint64 writeRequestsCreated = 0;
    quint64 synchronousHostWrites = 0;
};

class KisPageMutationSession::Private
{
public:
    struct Page {
        KisReplicaHandle source;
        KisReplicaHandle target;
        KisPageAllocationDescriptor descriptor;
        KisCpuWriteBindingReservation writable;
        QByteArray resetPixel;
        KisPreparedPageProof proof;
    };
    static_assert(sizeof(Page) <= 688);
    struct ColdPageSet {
        std::optional<Page> inlinePage;
        std::vector<std::optional<Page>> overflow;

        Page *at(KisMutationWriteSet::EntryIndex index)
        {
            if (index == KisMutationWriteSet::InvalidEntry)
                return nullptr;
            if (index == 0)
                return inlinePage ? &*inlinePage : nullptr;
            return size_t(index - 1) < overflow.size() && overflow[size_t(index - 1)]
                ? &*overflow[size_t(index - 1)] : nullptr;
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
            overflow.emplace_back(std::in_place);
            return quint32(overflow.size());
        }
        void erase(KisMutationWriteSet::EntryIndex index)
        {
            if (index == 0)
                inlinePage.reset();
            else if (index != KisMutationWriteSet::InvalidEntry && size_t(index - 1) < overflow.size())
                overflow[size_t(index - 1)].reset();
        }
    };
    ~Private()
    {
        Q_ASSERT(state == State::Detached);
        KisPageStore::PrivateReleaser::cleanup(owner);
    }
    static void releaseOrRetain(Private *scope)
    {
        if (!scope->cancelLocked()) {
            QMutexLocker lock(&scope->owner->mutex);
            scope->owner->orphanedMutations.push_back(scope);
            return;
        }
        delete scope;
    }
    static void retryOrphans(KisPageStore::Private *owner, KisPageTransactionId transaction = {})
    {
        std::vector<Private *> pending;
        {
            QMutexLocker lock(&owner->mutex);
            auto &orphans = owner->orphanedMutations;
            for (auto it = orphans.begin(); it != orphans.end();) {
                if (transaction.isValid() && !((*it)->transaction.id == transaction)) {
                    ++it;
                    continue;
                }
                pending.push_back(*it);
                it = orphans.erase(it);
            }
        }
        for (Private *scope : pending) {
            QMutexLocker scopeLock(&scope->mutex);
            const bool released = scope->cancelLocked();
            scopeLock.unlock();
            if (released) {
                delete scope;
            } else {
                QMutexLocker lock(&owner->mutex);
                owner->orphanedMutations.push_back(scope);
            }
        }
    }
    void detachLocked()
    {
        admission.releaseLocked();
        owner->writeCoordinator.endSessionActivity(transaction.id);
        writes = KisMutationWriteSet{};
        owner->mutationStats.guardsReleased += guardsReleased;
        owner->mutationStats.writablePinsAcquired += pinsAcquired;
        owner->mutationStats.writablePinsReleased += pinsReleased;
        owner->mutationStats.pendingWriteMaterializations += materializations;
        owner->mutationStats.pendingDefaultResetBytes += defaultResetBytes;
        owner->mutationStats.pendingPayloadCopyBytes += payloadCopyBytes;
        owner->mutationStats.maximumPinnedPagesPerSegment =
            qMax(owner->mutationStats.maximumPinnedPagesPerSegment, maximumPins);
        Q_ASSERT(activePins == 0 && pinsAcquired == pinsReleased);
        state = State::Detached;
    }
    KisPageTransition writeTransition(KisMutationWriteSet::EntryIndex index, const Page &page) const
    {
        const auto *entry = writes.at(index);
        Q_ASSERT(entry && entry->key() == page.target.version.key);
        return owner->writeCoordinator.writeTransition(*entry, transaction.id, page.source, page.target);
    }
    bool cancelPageLocked(KisMutationWriteSet::EntryIndex index, Page &page,
                          QVector<KisPageTransitionEffect> &effects)
    {
        const auto *entry = writes.at(index);
        Q_ASSERT(entry && !entry->isExposed());
        page.writable.reset();
        const auto result = owner->writeCoordinator.cancelPrivateWrite(writeTransition(index, page));
        if (!result.accepted)
            return false;
        if (page.proof.isValid())
            owner->owner.revokePreparedPage(page.proof);
        effects += result.effects;
        owner->publicationCoordinator.removeDescriptorLocked(page.target.version);
        if (entry->isCpuWrite())
            ++owner->mutationStats.pagesCancelled;
        return true;
    }
    bool cancelLocked(QMutexLocker<QMutex> *heldOwnerLock = nullptr)
    {
        if (state == State::Detached || !owner)
            return true;
        for (qsizetype i = 0; i < writes.size(); ++i)
            if (writes.at(KisMutationWriteSet::EntryIndex(i))->isExposed())
                return false;
        std::optional<QMutexLocker<QMutex>> acquired;
        if (!heldOwnerLock) {
            acquired.emplace(&owner->mutex);
            heldOwnerLock = &*acquired;
        }
        auto &lock = *heldOwnerLock;
        QVector<KisPageTransitionEffect> effects;
        const auto retire = qScopeGuard([&] { owner->retireEffectsLocked(effects, lock); });
        quint64 cancelledRemovals = 0;
        for (qsizetype i = 0; i < writes.size(); ++i) {
            const auto index = KisMutationWriteSet::EntryIndex(i);
            cancelledRemovals += writes.at(index)->isRemoval();
            if (Page *page = pageAtEntry(index)) {
                if (!cancelPageLocked(index, *page, effects)) {
                    state = State::Failed;
                    return false;
                }
                erasePageAtEntry(index);
            }
        }
        owner->mutationStats.removalsCancelled += cancelledRemovals;
        ColdPageSet abandonedPages;
        std::swap(coldPages, abandonedPages);
        // Keep the transaction claim while physical cleanup is outside owner.
        ++owner->activeProviderCalls;
        lock.unlock();
        abandonedPages = {};
        clearSources();
        owner->retirementQueue.retireEffects(effects, owner->backgroundReclamation);
        effects.clear();
        lock.relock();
        --owner->activeProviderCalls;
        detachLocked();
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
    Page *ensurePage(KisMutationPageEntry &entry)
    {
        if (auto *page = coldPages.at(entry.coldPage()))
            return page;
        const auto slot = coldPages.create();
        entry.setColdPage(slot);
        return coldPages.at(slot);
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
        for (qsizetype i = 0; i < writes.size(); ++i)
            writes.at(KisMutationWriteSet::EntryIndex(i))->setInitializationSource({});
    }
    bool claimEntryLocked(
        const KisPageWriteIntent &intent,
        KisPageWriteAdmission::ClaimOrigin origin =
            KisPageWriteAdmission::ClaimOrigin::NativeSession)
    {
        KisMutationPageEntry &entry = writes.getOrCreate(intent);
        if (!admission.isValid())
            admission = owner->writeAdmission.beginClaimSet(writes);
        return owner->writeAdmission.claimOne(admission, writes.findIndex(entry.key()),
                                              nullptr, origin);
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
        if (state != State::Active || thread != QThread::currentThreadId() || !key.isValid()
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
        if (!claimEntryLocked(intent)) {
            state = State::Failed;
            KisPageStoreDetail::setError(error, removal ? QStringLiteral("removal page is claimed by another writer")
                                    : QStringLiteral("alias page is claimed by another writer"));
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
                            QVector<KisPageTransitionEffect> &effects,
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
        auto backing = owner->writeCoordinator.reserveBacking(descriptor,
                                             KisPageAccessDomain::CpuRam,
                                             KisBackingBudgetClass::ActivePending, error,
                                             adoption.version);
        if (!backing.isValid()) return false;
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
            allocation.replica, backing, KisBackingBudgetClass::ActivePending, error);
        const auto reject = [&] {
            if (!ours)
                return;
            owner->retireRejectedReplicaLocked(allocation.replica, producer, std::move(backing), lock);
        };
        if (!backingOwned || allocation.status != KisPageRequestStatus::Ready
            || allocation.replica.domain != KisPageAccessDomain::CpuRam
            || !allocation.replica.layout.matches(descriptor)
            || !owner->owner.consumeTerminalProviderOperation(adoption.operation, allocation, error).succeeded()) {
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
        effects += applied.effects;
        if (!page) {
            page = resources;
            page->source = adoption.source;
            page->target = adoption.target;
            owner->writeCoordinator.recordPrepared(*entry, intent, adoption);
            page->descriptor = descriptor;
            owner->publicationCoordinator.putDescriptorLocked(adoption.version, page->descriptor);
            ++owner->mutationStats.aliasGenerationsReserved;
        } else {
            page->writable.reset();
            page->target = adoption.target;
        }
        return true;
    }
    enum class State : quint8 { Active, Failed, Detached };
    Qt::HANDLE thread = QThread::currentThreadId();
    State state = State::Detached;
    quint64 guardsReleased = 0;
    quint64 pinsAcquired = 0;
    quint64 pinsReleased = 0;
    quint64 activePins = 0;
    quint64 maximumPins = 0;
    quint64 materializations = 0;
    quint64 defaultResetBytes = 0;
    quint64 payloadCopyBytes = 0;
    void *pinWritable(KisCpuWriteBindingReservation &writable)
    {
        KisPageStoreDiagnosticTimer phase(diagnosticOwner, KisPageStoreDiagnosticPhase::WriteWritablePin, 1);
        KisCpuResidentReadStatus status;
        void *data = writable.pinResident(&status);
        if (!data && status == KisCpuResidentReadStatus::NonResident) {
            // This is real pending backing restoration, not another first
            // write, COW or request. No owner gate is held. Current tiles3
            // storage uses its synchronous swap control path here.
            phase.next(KisPageStoreDiagnosticPhase::WritePendingMaterialize, 1);
            ++materializations;
            data = writable.materialize();
        }
        return data;
    }
};

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
        m_entry = std::exchange(other.m_entry, KisMutationWriteSet::InvalidEntry);
        m_data = std::exchange(other.m_data, nullptr);
        m_rowStride = std::exchange(other.m_rowStride, 0);
        m_byteSize = std::exchange(other.m_byteSize, 0);
    }
    return *this;
}
void KisCpuWriteGuard::reset()
{
    if (m_scope && m_data) {
        QMutexLocker lock(&m_scope->mutex);
        auto *entry = m_scope->writes.at(m_entry);
        Q_ASSERT(entry && entry->isExposed());
        auto *page = m_scope->pageAtEntry(m_entry);
        Q_ASSERT(page);
        if (page) {
            // Drop only pixel residency, not the unpublished writer token.
            // The next guard must re-pin; no parked Page retains a pointer.
            page->writable.unpin();
            m_scope->owner->writeCoordinator.recordExposure(*entry, false);
            ++m_scope->guardsReleased;
            ++m_scope->pinsReleased;
            Q_ASSERT(m_scope->activePins);
            --m_scope->activePins;
        }
    }
    m_data = nullptr;
    m_entry = KisMutationWriteSet::InvalidEntry;
    m_rowStride = 0;
    m_byteSize = 0;
    m_scope.clear();
}

KisPageVersion KisCpuWriteGuard::version() const
{
    if (!m_scope || !m_data)
        return {};
    QMutexLocker lock(&m_scope->mutex);
    const auto *entry = m_scope->writes.at(m_entry);
    return entry ? entry->preparedTargetVersion() : KisPageVersion{};
}

bool KisCpuWriteGuard::providerBacking(KisReplicaHandle *replica) const
{
    if (replica)
        *replica = {};
    if (!m_scope || !m_data || !replica)
        return false;
    QMutexLocker lock(&m_scope->mutex);
    const auto *entry = m_scope->writes.at(m_entry);
    const auto *page = entry ? m_scope->pageAtEntry(m_entry) : nullptr;
    if (!page || !entry->isExposed() || !page->target.isValid()
        || !page->writable.isValid())
        return false;
    *replica = page->target;
    return true;
}

KisPageMutationSession KisPageStore::beginMutation(const KisPageTransaction &transaction, QString *error)
{
    KisPageMutationSession result;
    QMutexLocker lock(&d->mutex);
    const auto state = d->epochs.transaction(transaction.id);
    if (!d->operational || !transaction.isValid() || d->publicationCoordinator.isPreparingCommitLocked(transaction.id)
        || !(state.transaction == transaction)
        || !state.isActive()) {
        KisPageStoreDetail::setError(error, QStringLiteral("mutation transaction is unavailable"));
        return result;
    }
    result.d = QSharedPointer<KisPageMutationSession::Private>(
        new KisPageMutationSession::Private,
        &KisPageMutationSession::Private::releaseOrRetain);
    result.d->owner = d.data();
    result.d->diagnosticOwner = this;
    result.d->transaction = transaction;
    result.d->state = KisPageMutationSession::Private::State::Active;
    d->lifetimeReferences.ref();
    d->writeCoordinator.beginSessionActivity(transaction.id);
    ++d->mutationStats.operationSessionsCreated;
    KisPageStoreDetail::setError(error, {});
    return result;
}

std::unique_ptr<KisPageStoreWriteReservation> KisPageStore::reserveManagedRange(
    KisSurfaceId surface, const QSet<KisLogicalPageId> &targets,
    bool legacyIntent, bool *borrowed, QString *error)
{
    if (borrowed)
        *borrowed = false;
    if (!surface.isValid() || targets.isEmpty()) {
        KisPageStoreDetail::setError(error, QStringLiteral("managed mutation range is invalid"));
        return {};
    }
    auto range = std::make_unique<KisPageStoreWriteReservation>();
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
    if (!d->writeAdmission.claimAll(range->admission, error,
                                    KisPageWriteAdmission::ClaimOrigin::ManagedRange))
        return {};
    return range;
}

bool KisPageMutationSession::adoptReservation(
    std::unique_ptr<KisPageStoreWriteReservation> range, QString *error)
{
    if (!d || !range) {
        KisPageStoreDetail::setError(error, QStringLiteral("write reservation is absent"));
        return false;
    }
    QMutexLocker scopeLock(&d->mutex);
    if (d->state != Private::State::Active || d->thread != QThread::currentThreadId() || d->writes.size() != 0
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
    if (d->state != Private::State::Active || d->thread != QThread::currentThreadId()) {
        KisPageStoreDetail::setError(error, QStringLiteral("legacy mutation is unavailable"));
        return false;
    }
    QMutexLocker ownerLock(&d->owner->mutex);
    if (d->writes.find(key)) {
        const bool held = d->owner->writeAdmission.ownsClaimSetLocked(d->admission);
        KisPageStoreDetail::setError(error, held ? QString{} : QStringLiteral("legacy mutation lost its page claim"));
        return held;
    }
    while (d->owner->operational) {
        const auto conflict = d->owner->writeAdmission.conflictLocked(
            key, QThread::currentThreadId());
        if (conflict == KisPageWriteAdmission::Conflict::None)
            break;
        if (conflict == KisPageWriteAdmission::Conflict::ManagedSameThread
            || conflict == KisPageWriteAdmission::Conflict::LegacySameThread
            || conflict == KisPageWriteAdmission::Conflict::WriterSameThread) {
            KisPageStoreDetail::setError(error, QStringLiteral(
                "legacy mutation cannot reenter its page"));
            return false;
        }
        d->owner->writeAdmissionChanged.wait(&d->owner->mutex);
    }
    if (!d->owner->operational) {
        KisPageStoreDetail::setError(error, QStringLiteral("legacy mutation store is unavailable"));
        return false;
    }
    KisPageWriteIntent intent;
    intent.key = key;
    intent.inputKind = KisPageWriteInputKind::Semantic;
    const bool claimed = d->claimEntryLocked(
        intent, KisPageWriteAdmission::ClaimOrigin::LegacyAdapter);
    KisPageStoreDetail::setError(error, claimed ? QString{} : QStringLiteral("legacy mutation page claim failed"));
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
                                                        QString *error)
{
    KisCpuWriteGuard result;
    if (!d) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation is absent"));
        return result;
    }
    KisPageStoreDiagnosticTimer diagnostic(d->diagnosticOwner, KisPageStoreDiagnosticPhase::WriteAcquire, 1);
    QMutexLocker scopeLock(&d->mutex);
    if (d->state != Private::State::Active || d->thread != QThread::currentThreadId() || !key.isValid()
        || (mode != KisPageWriteMode::PreserveContents && mode != KisPageWriteMode::DiscardContents)) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation is unavailable or used from another thread"));
        return result;
    }
    auto entryIndex = d->writes.findIndex(key);
    auto *entry = d->writes.at(entryIndex);
    auto *page = d->pageAtEntry(entryIndex);
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
        d->state = Private::State::Failed;
        KisPageStoreDetail::setError(error, QStringLiteral("alias writable provider does not match its input"));
        return result;
    }
    if (!page) {
        auto *owner = d->owner;
        QMutexLocker lock(&owner->mutex);
        auto fail = [&](const QString &message) {
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
        if (!d->claimEntryLocked(intent)) {
            fail(QStringLiteral("page is claimed by another writer"));
            return result;
        }
        entryIndex = d->writes.findIndex(key);
        entry = d->writes.at(entryIndex);
        Q_ASSERT(entry);
        if (entry->isRemoval())
            intent.flags |= quint8(KisPageWriteIntentFlag::SemanticRemoval);
        KisPageTransition acquire;
        KisPageAllocationDescriptor descriptor;
        QString failure;
        if (!owner->writeCoordinator.prepareWriteBaseLocked(d->transaction, intent, owner->publicationCoordinator,
                                                            &acquire, &descriptor, &failure)) {
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
        auto backing = owner->writeCoordinator.reserveBacking(descriptor, KisPageAccessDomain::CpuRam,
                                             KisBackingBudgetClass::ActivePending, &failure,
                                             target);
        if (!backing.isValid()) {
            fail(failure);
            return result;
        }
        auto *resources = d->ensurePage(*entry);
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
            allocation.replica, backing, KisBackingBudgetClass::ActivePending, &failure);
        const auto binding = backingOwned ? d->provider->cpuResidentBinding(allocation.replica)
                                           : QSharedPointer<KisCpuResidentBinding>{};
        auto writable = KisCpuWriteBindingReservation::acquire(binding);
        data = d->pinWritable(writable);
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
            || !data || !owner->owner.consumeTerminalProviderOperation(operation, allocation, &failure).succeeded()) {
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
        page = resources;
        page->source = acquire.source;
        page->target = acquire.target;
        page->descriptor = descriptor;
        page->writable = std::move(writable);
        owner->publicationCoordinator.putDescriptorLocked(target, descriptor);
        owner->writeCoordinator.recordPrepared(*entry, intent, acquire);
        ++owner->mutationStats.generationsReserved;
        payloadInitialized = directPayload;
        if (initialization) {
            entry->setInitializationSource({});
            initialization.clear();
        }
    }
    if (payload && !payload->isValidFor(page->descriptor)) {
        d->state = Private::State::Failed;
        KisPageStoreDetail::setError(error, QStringLiteral("complete CPU payload layout is invalid"));
        return result;
    }
    if (!data)
        data = d->pinWritable(page->writable);
    if (!data) {
        d->state = Private::State::Failed;
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation pending backing cannot be pinned"));
        return result;
    }
    if (initialization) {
        KisPageStoreDiagnosticTimer phase(d->diagnosticOwner, KisPageStoreDiagnosticPhase::MutationSourceInitialize, 1);
        if (!d->provider->copySynchronousSourceToCpu(initialization,
                                                     page->descriptor,
                                                     data,
                                                     page->target.layout.rowStride,
                                                     page->target.layout.byteSize)) {
            page->writable.unpin();
            d->state = Private::State::Failed;
            KisPageStoreDetail::setError(error, QStringLiteral("alias pending initialization failed"));
            return result;
        }
        entry->setInitializationSource({});
    }
    if (payload) {
        if (!payloadInitialized) {
            KisPageStoreDiagnosticTimer phase(d->diagnosticOwner, KisPageStoreDiagnosticPhase::MutationPayloadCopy, 1);
            const auto rowBytes = page->descriptor.minimumRowBytes();
            const int rows = page->descriptor.pageExtent.height();
            for (int row = 0; row < rows; ++row)
                std::memcpy(static_cast<quint8 *>(data) + quint64(row) * page->target.layout.rowStride,
                            static_cast<const quint8 *>(payload->data) + row * payload->rowStride,
                            size_t(rowBytes));
            d->payloadCopyBytes += rowBytes * rows;
        }
        // Full input supersedes a staged semantic default, without a redundant fill.
        page->resetPixel.clear();
    } else if (!page->resetPixel.isEmpty()) {
        const auto &rect = page->descriptor.validRect;
        const auto &pixel = page->resetPixel;
        auto *pixels = static_cast<quint8 *>(data);
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            quint8 *row = pixels + quint64(y) * page->target.layout.rowStride;
            for (int x = rect.left(); x <= rect.right(); ++x)
                std::memcpy(row + quint64(x) * pixel.size(), pixel.constData(), size_t(pixel.size()));
        }
        d->defaultResetBytes += quint64(rect.width()) * rect.height() * pixel.size();
        page->resetPixel.clear();
    }
    Q_ASSERT(entry);
    entry->setRemoval(false);
    d->owner->writeCoordinator.recordExposure(*entry, true);
    ++d->pinsAcquired;
    d->maximumPins = qMax(d->maximumPins, ++d->activePins);
    result.m_scope = d;
    result.m_entry = entryIndex;
    result.m_data = data;
    result.m_rowStride = page->target.layout.rowStride;
    result.m_byteSize = page->target.layout.byteSize;
    KisPageStoreDetail::setError(error, {});
    return result;
}

bool KisPageMutationSession::cancel()
{
    if (!d)
        return true;
    QMutexLocker lock(&d->mutex);
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

bool KisPageMutationSession::sealImpl(QString *error, bool legacyFinalUnlock)
{
    if (!d) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation is absent"));
        return false;
    }
    QMutexLocker scopeLock(&d->mutex);
    // One canonical segment seal, not one generic lease publish. Work items
    // count distinct claimed semantic/pixel keys, including removal and alias.
    KisPageStoreDiagnosticTimer diagnostic(d->diagnosticOwner,
                                           KisPageStoreDiagnosticPhase::WritePublishHost,
                                           quint64(d->writes.size()));
    if (d->state == Private::State::Detached ||
        (!legacyFinalUnlock && d->thread != QThread::currentThreadId())) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation cannot seal"));
        return false;
    }
    for (qsizetype i = 0; i < d->writes.size(); ++i) {
        if (d->writes.at(KisMutationWriteSet::EntryIndex(i))->isExposed()) {
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
    QVector<KisPageTransitionEffect> retirements;
    const auto retire = [&] {
        owner->retireEffectsLocked(retirements, lock);
        retirements.clear();
    };
    // A final semantic removal discards only this segment's unsealed target.
    // Previously sealed history remains untouched until the batch install.
    qsizetype privatePageCount = 0;
    for (qsizetype i = 0; i < d->writes.size(); ++i) {
        const auto index = KisMutationWriteSet::EntryIndex(i);
        auto *page = d->pageAtEntry(index);
        if (!page)
            continue;
        if (!d->writes.at(index)->isRemoval()) {
            ++privatePageCount;
            continue;
        }
        if (!d->cancelPageLocked(index, *page, retirements)) {
            d->state = Private::State::Failed;
            retire();
            KisPageStoreDetail::setError(error, QStringLiteral("mutation could not discard its removed target"));
            return false;
        }
        d->erasePageAtEntry(index);
    }
    ++owner->activeProviderCalls;
    lock.unlock();
    phase.next(Phase::MutationSealPrivatePublish, quint64(privatePageCount));
    KisCompletionTicket completion;
    if (privatePageCount)
        completion = owner->readyHostCompletion;
    QString failure;
    bool success = !privatePageCount || completion.isValid();
    for (qsizetype i = 0; i < d->writes.size(); ++i) {
        const auto index = KisMutationWriteSet::EntryIndex(i);
        auto *page = d->pageAtEntry(index);
        if (!page)
            continue;
        if (!success)
            break;
        const auto applied = owner->writeCoordinator.publishPrivateWrite(
            d->writeTransition(index, *page));
        success = applied.accepted;
        if (!success)
            failure = applied.rejectionReason;
    }
    phase.next(Phase::MutationSealOwnerWait, pageWork);
    lock.relock();
    --owner->activeProviderCalls;
    phase.next(Phase::MutationSealInputs, pageWork);
    success = success && d->claimsHeldLocked();
    qsizetype sealedPageCount = privatePageCount;
    if (success)
        for (qsizetype i = 0; i < d->writes.size(); ++i) {
            const auto index = KisMutationWriteSet::EntryIndex(i);
            const auto *entry = d->writes.at(index);
            const auto source = entry->initializationSource();
            const bool hadPage = d->pageAtEntry(index) != nullptr;
            if (source && !d->prepareAliasLocked(entry->key(), source, lock, retirements, &failure)) {
                success = false;
                break;
            }
            if (source && !hadPage)
                ++sealedPageCount;
        }
    ++owner->activeProviderCalls;
    lock.unlock();
    phase.next(Phase::MutationSealProofPrepare, quint64(sealedPageCount));
    if (success && sealedPageCount && !completion.isValid())
        completion = owner->readyHostCompletion;
    success = success && (!sealedPageCount || completion.isValid());
    if (success)
        for (qsizetype i = 0; i < d->writes.size(); ++i) {
            auto *page = d->pageAtEntry(KisMutationWriteSet::EntryIndex(i));
            if (!page)
                continue;
            if (!owner->owner.sealPreparedPage(owner->metadata,
                                               page->target.version,
                                               d->transaction.id,
                                               page->descriptor,
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
        for (qsizetype i = 0; i < d->writes.size(); ++i)
            if (auto *page = d->pageAtEntry(KisMutationWriteSet::EntryIndex(i)))
                page->writable.reset();

    phase.next(Phase::MutationSealOwnerWait, pageWork);
    lock.relock();
    phase.next(Phase::MutationSealInputs, pageWork);
    success = success && d->claimsHeldLocked();
    QVector<KisPagePublicationCoordinator::OverlayChange> overlayChanges;
    overlayChanges.reserve(d->writes.size());
    quint64 sealedCpuWrites = 0;
    quint64 sealedRemovals = 0;
    quint64 sealedSources = 0;
    if (success) {
        for (qsizetype i = 0; i < d->writes.size(); ++i) {
            const auto index = KisMutationWriteSet::EntryIndex(i);
            auto *entry = d->writes.at(index);
            auto *page = d->pageAtEntry(index);
            if (entry->isRemoval()) {
                overlayChanges.append(
                    {entry->key(), {}, true});
                ++sealedRemovals;
            } else if (page) {
                overlayChanges.append(
                    {entry->key(), page->proof, false});
                sealedCpuWrites += entry->isCpuWrite();
            }
            sealedSources += bool(entry->initializationSource());
        }
    }
    auto overlay = success && !overlayChanges.isEmpty()
        ? owner->publicationCoordinator.prepareOverlayUpdateLocked(
              d->transaction, std::move(overlayChanges), &failure)
        : KisPagePublicationCoordinator::KisPreparedOverlayUpdate{};
    const bool hasOverlay = success && d->writes.size() != 0;
    success = success && (!hasOverlay || overlay.isValid());
    if (success && hasOverlay) {
        // The aggregate now owns every new sealed proof. A failed prepare or
        // install revokes them together while the former overlay stays live.
        for (qsizetype i = 0; i < d->writes.size(); ++i) {
            if (auto *page = d->pageAtEntry(
                    KisMutationWriteSet::EntryIndex(i))) {
                page->proof = {};
            }
        }
    }
    const qsizetype metadataChangeCount = overlay.metadataChangeCount();
    lock.unlock();
    phase.next(Phase::MutationSealMetadataPrepare,
               quint64(metadataChangeCount));
    if (success && hasOverlay)
        success = overlay.prepare(&failure);
    KisPageMetadataCoordinator::DeferredPublicationCleanup metadataCleanup;
    phase.next(Phase::MutationSealPublishOwnerWait, pageWork);
    lock.relock();
    phase.next(Phase::MutationSealInstall, pageWork);
    success = success && d->claimsHeldLocked();
    if (success && hasOverlay) {
        success = overlay.tryInstallLocked(
            &retirements, &metadataCleanup, &failure);
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
    sealedPages = {};
    d->clearSources();
    owner->retirementQueue.retireEffects(retirements, owner->backgroundReclamation);
    phase.next(Phase::MutationSealOwnerWait, pageWork);
    lock.relock();
    --owner->activeProviderCalls;
    d->detachLocked();
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
    bool surfaceState(KisSurfaceId surface, KisSurfaceEpochState *state) const
    {
        const auto found = stagedSurfaces.constFind(surface.value);
        if (found == stagedSurfaces.constEnd())
            return root.surfaceState(surface, state);
        *state = found.value();
        return true;
    }

    bool resolve(const KisPageKey &key, KisPageVersion *version) const
    {
        if (!key.isValid())
            return false;
        const auto found = sealedPages.constFind(key);
        if (found != sealedPages.constEnd()) {
            *version = found.value();
            return true;
        }
        if (!root.resolve(key, version))
            return false;
        if (overlayTransaction.isValid() && (version->isDefaultPixel() || removedPages.contains(key))) {
            KisSurfaceEpochState surface;
            if (!surfaceState(key.surface, &surface))
                return false;
            *version = {key, KisPageGeneration{1}, surface.defaultPixelRevision};
        }
        return true;
    }
    // Keep the large cold handle and ledger/provider discovery out of the
    // warm guard's stack/initialization path. This is not a safety bypass:
    // installation still revalidates under the metadata shard gate.
    Q_NEVER_INLINE QSharedPointer<KisCpuReadBindingLink> discoverCpuReadBinding(const KisPageVersion &version) const
    {
        const auto replica = owner->metadata.cpuReadReplica(version);
        if (!replica.isValid())
            return {};
        const auto provider = owner->owner.provider(replica.provider, replica.providerEpoch);
        return owner->metadata.installCpuReadBinding(replica, provider);
    }

    QSharedPointer<const KisCpuDefaultReadBuffer> defaultReadBuffer(const KisPageVersion &version) const
    {
        QMutexLocker lock(&defaultMutex);
        const auto found = defaults.constFind(version.key.surface.value);
        if (found != defaults.constEnd())
            return found.value();
        KisSurfaceEpochState surface;
        if (!surfaceState(version.key.surface, &surface)
            || surface.defaultPixelRevision != version.defaultPixelRevision)
            return {};
        auto buffer = owner->defaultStorage.readBuffer(surface);
        if (buffer)
            defaults.insert(version.key.surface.value, buffer);
        return buffer;
    }

    ~Private()
    {
        if (!owner)
            return;
        owner->readCoordinator.releaseCapturedView(retention, sealedPages);
        KisPageStore::PrivateReleaser::cleanup(owner);
    }
    KisPageStore::Private *owner = nullptr;
    KisImageEpochRootSnapshot root;
    KisImageEpochSnapshotToken retention;
    KisPageVersion exactVersion;
    KisPageTransactionId overlayTransaction;
    QHash<KisPageKey, KisPageVersion> sealedPages;
    QSet<KisPageKey> removedPages;
    QHash<quint64, KisSurfaceEpochState> stagedSurfaces;
    mutable QMutex defaultMutex;
    mutable QHash<quint64, QSharedPointer<const KisCpuDefaultReadBuffer>> defaults;
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
    if (status)
        *status = version.defaultPixelRevision ? KisCpuResidentReadStatus::VirtualDefault
                                               : KisCpuResidentReadStatus::BindingUnavailable;
    auto link = d->owner->metadata.cpuReadBinding(version);
    // The captured root remains protected across this cold miss/recheck.
    if (!link)
        link = d->discoverCpuReadBinding(version);
    if (!link || !(link->replica.version == version))
        return guard;
    auto binding = link->resolve(status);
    if (!binding)
        return guard;
    const void *data = binding->acquireRead(true, status, waitForLocalGate);
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
    m_scope.clear();
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

KisPageStore::~KisPageStore() = default;

void KisPageStore::PrivateReleaser::cleanup(Private *owner)
{
    if (owner && !owner->lifetimeReferences.deref()) {
        if (!owner->backgroundReclamation || kisOnPageStoreReclamationThread())
            delete owner;
        else
            kisSchedulePageStoreReclamation([owner] {
                delete owner;
            });
    }
}

KisCapturedReadView KisPageStore::captureReadView(const KisPageReadView &selector, QString *error)
{
    KisCapturedReadView result;
    const KisPageKey probe = selector.kind == KisPageReadViewKind::ExactVersion ? selector.exactVersion.key
                                                                                : KisPageKey{KisSurfaceId{1}, {0, 0}};
    if (!selector.isValidFor(probe)) {
        KisPageStoreDetail::setError(error, QStringLiteral("read selector is invalid"));
        return result;
    }
    QMutexLocker locker(&d->mutex);
    if (!d->operational) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore is not operational"));
        return result;
    }
    KisImageEpochRootSnapshot root;
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
        const auto transaction = d->epochs.transaction(selector.transaction);
        if (transaction.isActive())
            root = d->epochs.root(transaction.transaction.baseEpoch);
        break;
    }
    }
    KisPageVersion exact;
    if (!root.isValid()
        || (selector.kind == KisPageReadViewKind::ExactVersion
            && (!root.resolve(selector.exactVersion.key, &exact) || !(exact == selector.exactVersion)))) {
        KisPageStoreDetail::setError(error, QStringLiteral("read selector is stale or not owned by this PageStore"));
        return result;
    }
    const auto retained = d->epochs.retainSnapshot(root.epoch());
    if (!retained.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("read scope retention could not be allocated"));
        return result;
    }
    result.d = QSharedPointer<KisCapturedReadView::Private>::create();
    if (selector.kind == KisPageReadViewKind::TransactionOverlay) {
        // Owner gate makes the whole sealed delta and its default/removal
        // metadata one visibility cut. Unsealed writer bytes are not exposed.
        const KisPreparedPageSet delta =
            d->publicationCoordinator.transactionDeltaLocked(selector.transaction);
        for (const auto &proof : delta.proofs) {
            KisPageTransition claim;
            claim.kind = KisPageTransitionKind::RetainCapturedVersion;
            claim.version = proof.authority.version;
            claim.transaction = selector.transaction;
            claim.readView = retained.token;
            const auto claimed = d->metadata.applyOwner(proof.authority.version.key, claim);
            if (!claimed.accepted) {
                d->readCoordinator.releaseCapturedView(
                    retained.token, result.d->sealedPages, false, &locker);
                result.d.clear(); // owner pointer has not been installed yet
                KisPageStoreDetail::setError(error, claimed.rejectionReason);
                return result;
            }
            result.d->sealedPages.insert(proof.authority.version.key,
                                         proof.authority.version);
        }
        result.d->overlayTransaction = selector.transaction;
        result.d->removedPages = QSet<KisPageKey>(delta.removedPages.cbegin(), delta.removedPages.cend());
        for (const auto &change : delta.surfaceChanges)
            result.d->stagedSurfaces.insert(change.after.surface.value, change.after);
    }
    d->lifetimeReferences.ref();
    result.d->owner = d.data();
    result.d->root = std::move(root);
    result.d->retention = retained.token;
    result.d->exactVersion = exact;
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
            history.reachabilityRootsVisited};
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
    d->retirementQueue.beginClose();
    locker.unlock();
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
    d->readCoordinator.retryCancelledRequestsLocked(locker, true);
    d->readCoordinator.retryCapturedReleasesLocked(locker, true);
    const auto releasedReadKeys = d->readCoordinator.retryReleasedReadsLocked({}, nullptr, true);
    d->retireEffectsLocked(d->historyCollector.collectUnreachableLocked(releasedReadKeys), locker);
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
    if (stats.hasOutstandingCapabilities()) {
        d->closing = false;
        d->retirementQueue.cancelCloseAndSchedule();
        d->historyCollector.scheduleLocked();
        KisPageStoreDetail::setError(error,
                 QStringLiteral("PageStore session still owns capabilities: "
                                "transactions=%1 snapshots=%2 requests=%3 "
                                "readLeases=%4 writeLeases=%5 lastUses=%6 "
                                "proofs=%7 surfaceChanges=%8 removals=%9 "
                                "providerCalls=%10 blockingOperations=%11 "
                                "seals=%12 archives=%13 shutdownReplicas=%14 "
                                "defaultPreparations=%15")
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
                     .arg(stats.activeDefaultPreparations));
        return false;
    }

    QVector<KisPageRetirementRecord> liveShutdownReplicas;
    if (d->pendingShutdownReplicas.isEmpty() && d->operational) {
        d->epochs.collectUnretainedRoots();
        QSet<KisReplicaAllocationIdentity> seenReplicas;
        for (const KisReplicaHandle &replica : d->metadata.shutdownReplicaHandles()) {
            if (!replica.isValid()) continue;
            const auto identity = replica.allocationIdentity();
            if (seenReplicas.contains(identity)) continue;
            seenReplicas.insert(identity);
            const auto provider = d->owner.provider(replica.provider, replica.providerEpoch);
            liveShutdownReplicas.append({replica, provider, {}, {}, {}});
        }
    }

    QVector<KisPageRetirementRecord> shutdownReplicas =
        std::move(d->pendingShutdownReplicas);
    // Drain already Debt-admitted records before current live replicas. With
    // a finite one-page Debt budget, trying current replicas first can reject
    // all of them even though retiring the queued debt immediately frees the
    // capacity required for a sequential shutdown.
    shutdownReplicas += d->retirementQueue.takeForClose();
    shutdownReplicas += std::move(liveShutdownReplicas);
    d->closing = true;
    d->operational = false;
    d->writeAdmissionChanged.wakeAll();
    d->activeProviderCalls += shutdownReplicas.size();
    locker.unlock();
    QStringList retirementFailures;
    QVector<KisPageRetirementRecord> failedReplicas;
    for (KisPageRetirementRecord &shutdown : shutdownReplicas) {
        if (!shutdown.provider) {
            retirementFailures.append(QStringLiteral("replica %1:%2 lost its provider during shutdown")
                                          .arg(shutdown.replica.provider.value)
                                          .arg(shutdown.replica.allocation.slot));
            failedReplicas.append(std::move(shutdown));
            continue;
        }
        if (!d->retirementQueue.retireRecord(shutdown)) {
            retirementFailures.append(QStringLiteral("replica %1:%2 retirement failed")
                                          .arg(shutdown.replica.provider.value)
                                          .arg(shutdown.replica.allocation.slot));
            failedReplicas.append(std::move(shutdown));
        } else {
            d->metadata.removeCpuReadBinding(shutdown.replica);
        }
    }
    locker.relock();
    d->activeProviderCalls -= shutdownReplicas.size();
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
    if (d->completions) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore is already configured"));
        return false;
    }
    QString failure;
    if (!d->owner.configure(completions, &failure) || !d->metadata.configure(metadataShardCount, &failure)
        || !d->epochs.initialize(initialEpoch, &failure)) {
        KisPageStoreDetail::setError(error, failure);
        return false;
    }
    const quint64 source = completions->registerSource(KisCompletionDomain::HostLogical);
    const KisCompletionTicket ready = source != 0 ? completions->allocatePending(source) : KisCompletionTicket();
    if (source == 0 || !ready.isValid() || !completions->complete(ready, KisCompletionStatus::Succeeded)) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore host completion source registration failed"));
        return false;
    }
    d->completions = completions;
    d->readyHostCompletion = ready;
    for (const auto &surface : initialEpoch.surfaces) {
        d->publicationCoordinator.defaultRevisionHighWaterLocked(surface.surface) = surface.defaultPixelRevision;
    }
    KisPageStoreDetail::setError(error, {});
    return true;
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
    const QSharedPointer<KisPageReplicaProvider> provider =
        d->owner.provider(authority.provider, authority.providerEpoch);
    if (!provider) {
        KisPageStoreDetail::setError(error, QStringLiteral("initial page authority failed provider validation"));
        return false;
    }
    ++d->activeProviderCalls;
    locker.unlock();
    const bool authorityValid = provider->validate(authority, descriptor);
    locker.relock();
    --d->activeProviderCalls;
    KisPageStateSnapshot existingPage;
    if (!authorityValid || d->operational || d->closed || d->metadata.versionSnapshot(version, &existingPage)) {
        KisPageStoreDetail::setError(error, QStringLiteral("initial page authority failed provider validation"));
        return false;
    }
    const bool registeredHere =
        d->owner.backingClass(authority) == KisBackingBudgetClass::Count;
    if (registeredHere) {
        auto backing = d->writeCoordinator.reserveBacking(descriptor, authority.domain,
                                         KisBackingBudgetClass::Current, error);
        if (!backing.isValid()
            || !d->owner.registerBacking(authority, backing,
                                          KisBackingBudgetClass::Current, error))
            return false;
    }

    KisPageVersionStateSnapshot versionState;
    versionState.version = version;
    versionState.publication = KisPagePublicationState::Published;
    versionState.replicas.append({authority, KisReplicaValidity::Valid, {}, {}, 0, {}});
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
        if (registeredHere) d->owner.releaseRetiredBacking(authority);
        KisPageStoreDetail::setError(error, failure);
        return false;
    }
    d->publicationCoordinator.putDescriptorLocked(version, descriptor);
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
    KisBackingBudgetReservation backing;
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
        backing = d->writeCoordinator.reserveBacking(descriptor, KisPageAccessDomain::CpuRam,
                                    KisBackingBudgetClass::Current, error);
        if (!backing.isValid()) return false;
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
        allocation.replica, backing, KisBackingBudgetClass::Current, &failure);
    const KisVerifiedCompletion allocated = allocation.isValid()
        ? d->owner.consumeTerminalProviderOperation(allocationOperation, allocation, &failure)
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
        KisPageStateSnapshot page;
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
    QMutexLocker locker(&d->mutex);
    d->readCoordinator.retryCancelledRequestsLocked(locker);
    if (!d->operational || !view.isValidFor(key) || !access.isValid()
        || (captured && (!captured->d || captured->d->owner != d.data()))) {
        request.error = QStringLiteral("PageStore read request is invalid or unavailable");
        return request;
    }
    const auto fail = [&request](const QString &error) {
        request.status = KisPageRequestStatus::Failed;
        request.error = error;
        return request;
    };

    KisPageVersion version;
    KisPageStateSnapshot page;
    if (!(captured ? captured->resolvePageVersion(key, &version)
                   : d->publicationCoordinator.resolveVersionLocked(key, view, &version))) {
        return fail(QStringLiteral("PageStore read view does not resolve a page"));
    }
    const bool hasPage = d->metadata.versionSnapshot(version, &page);
    if (!hasPage || needsDefaultMaterialization(page.findVersion(version), version)) {
        KisSurfaceEpochState surface;
        KisImageEpochId publishedEpoch;
        if (captured) {
            publishedEpoch = captured->epoch();
        } else if (view.kind == KisPageReadViewKind::CurrentCommittedEpoch) {
            publishedEpoch = d->epochs.captureCommittedRoot().epoch();
        } else if (view.kind == KisPageReadViewKind::CommittedEpoch || view.kind == KisPageReadViewKind::ExactVersion) {
            publishedEpoch = view.epoch;
        } else {
            publishedEpoch = d->epochs.transaction(view.transaction).transaction.baseEpoch;
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
        if (!d->ensureDefaultPageLocked(version, publishedEpoch, surface, access, priority, &locker, &failure)
            || !d->metadata.versionSnapshot(version, &page)) {
            return fail(failure.isEmpty()
                ? QStringLiteral("PageStore default page materialization failed") : failure);
        }
    }
    // Keep the exact version captured above, including across provider work.
    const KisPageVersionStateSnapshot *versionState = page.findVersion(version);
    const KisReplicaStateSnapshot *replica = nullptr;
    QSharedPointer<KisPageReplicaProvider> provider;
    if (versionState) {
        const KisReplicaStateSnapshot *authorityReplica =
            versionState->findReplica(versionState->authority);
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
    if (versionState && !replica) {
        for (const KisReplicaStateSnapshot &candidate : versionState->replicas) {
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
                                                    readTransaction);
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
    const KisPageTransactionSnapshot transactionSnapshot = d->epochs.transaction(transaction.id);
    if (!d->operational || !transaction.isValid() || !key.isValid() || !access.isValid()
        || (mode != KisPageWriteMode::PreserveContents && mode != KisPageWriteMode::DiscardContents)
        || !(transactionSnapshot.transaction == transaction)
        || !transactionSnapshot.isActive()) {
        request.error = QStringLiteral("PageStore write transaction is invalid or unavailable");
        return request;
    }
    const auto fail = [&request](const QString &error = {}) {
        request.status = KisPageRequestStatus::Failed;
        if (!error.isNull()) request.error = error;
        return request;
    };
    d->writeCoordinator.beginPreparationActivity(transaction.id);
    const auto preparationClaim = qScopeGuard([&] {
        d->writeCoordinator.endPreparationActivity(transaction.id);
    });
    const bool claimed = d->writeAdmission.claimDirectLocked(key, transaction.id.value);
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
    KisPageAllocationDescriptor descriptor;
    if (!d->writeCoordinator.prepareWriteBaseLocked(transaction, writeIntent, d->publicationCoordinator,
                                                    &transition, &descriptor, &request.error)) {
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
    auto backing = d->writeCoordinator.reserveBacking(descriptor, access.domain,
                                     KisBackingBudgetClass::ActivePending,
                                     &request.error, writeVersion);
    if (!backing.isValid()) {
        return fail();
    }
    d->writeRequests.insert(requestId.value, pending);
    d->writeCoordinator.beginGenericActivity(transaction.id);
    pageClaim.dismiss(); // full request/lease lifetime now owns the page claim
    auto writeClaim = qScopeGuard([&] {
        d->releaseGenericWrite(transaction.id, key);
    });
    ++d->writeRequestsCreated;

    bool nativeWriteCopy = false;

    // Provider allocation is deliberately outside the PageStore lock. The
    // provisional request keeps close/abort from crossing this operation; the
    // metadata transition below revalidates writer/generation ownership.
    locker.unlock();
    KisReplicaOperation allocation;
    {
        KisPageStoreDiagnosticTimer phase(this, KisPageStoreDiagnosticPhase::WriteProviderPrepare, 1);
        allocation = d->writeCoordinator.prepareFreshReplica(writeIntent, *provider,
            transition, descriptor, access, priority, nullptr, {}, &nativeWriteCopy);
    }
    locker.relock();

    QString failure;
    const bool ownedReplica = ownsPreparedReplica(allocation.replica, writeVersion, *provider, pending.source);
    const bool backingOwned = ownedReplica
        && d->owner.registerBacking(allocation.replica, backing,
                                    KisBackingBudgetClass::ActivePending, &failure);

    const auto rejectAllocation = [&](const QString &message, bool removeRequest = true) {
        if (ownedReplica)
            d->retireRejectedReplicaLocked(allocation.replica, provider, std::move(backing), locker);
        if (removeRequest) d->writeRequests.remove(requestId.value);
        request.status = KisPageRequestStatus::Failed;
        request.error = message;
    };
    auto pendingIt = d->writeRequests.find(requestId.value);
    if (pendingIt == d->writeRequests.end() || pendingIt->state != PendingWriteRequestRecord::State::Preparing) {
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
    if (!d->owner.consumeTerminalProviderOperation(writeOperation, allocation, &failure).succeeded()) {
        rejectAllocation(failure.isEmpty() ? QStringLiteral("write allocation completion failed") : failure);
        return request;
    }
    pendingIt->replica = allocation.replica;
    transition.target = allocation.replica;
    KisPageTransitionResult stateResult = d->writeCoordinator.preparePrivateWrite(transition, false);
    if (!stateResult.accepted) {
        rejectAllocation(stateResult.rejectionReason);
        return request;
    }

    d->publicationCoordinator.putDescriptorLocked(writeVersion, descriptor);
    pendingIt->source = transition.source;
    pendingIt->readiness = allocation.completion;
    const auto rejectPrepared = [&](const QString &message) {
        const auto cancelled = d->cancelWriteLocked(d->writeRequests.value(requestId.value));
        if (cancelled.accepted) {
            d->writeRequests.remove(requestId.value);
            d->retireEffectsLocked(cancelled.effects, locker);
        } else {
            // No request capability was returned, so transaction abort owns
            // the retry. Do not release its only page/admission record.
            d->writeRequests[requestId.value].state = PendingWriteRequestRecord::State::Pending;
            writeClaim.dismiss();
        }
        request.status = KisPageRequestStatus::Failed;
        request.error = message;
    };
    locker.unlock();
    KisCompletionTicket initializationReadiness;
    {
        KisPageStoreDiagnosticTimer phase(this, KisPageStoreDiagnosticPhase::WriteProviderTransfer, 1);
        initializationReadiness = d->writeCoordinator.initializeFreshReplica(
            writeIntent, nativeWriteCopy, pending.source, allocation.replica,
            descriptor, provider, priority, allocation.completion, &failure);
    }
    locker.relock();
    pendingIt = d->writeRequests.find(requestId.value);
    if (pendingIt == d->writeRequests.end() || pendingIt->state != PendingWriteRequestRecord::State::Preparing) {
        return fail(QStringLiteral("write initialization reservation was lost"));
    }
    if (!initializationReadiness.isValid()) {
        rejectPrepared(failure.isEmpty() ? QStringLiteral("write generation initialization failed") : failure);
        return request;
    }
    pendingIt->readiness = initializationReadiness;

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
    request.readiness = pendingIt->readiness;
    if (!request.isValid()) {
        rejectPrepared(QStringLiteral("PageStore write request construction failed"));
        return request;
    }
    pendingIt->state = PendingWriteRequestRecord::State::Pending;
    writeClaim.dismiss(); // request -> lease -> publish/cancel owns the count
    return request;
}


KisReadLease KisPageStore::resolve(const KisReadRequest &request, const KisCompletionTicket &completion)
{
    QMutexLocker locker(&d->mutex);
    return d->readCoordinator.resolveLocked(request, completion, locker);
}

KisWriteLease KisPageStore::resolve(const KisWriteRequest &request, const KisCompletionTicket &completion)
{
    KisPageStoreDiagnosticTimer diagnostic(this, KisPageStoreDiagnosticPhase::WriteResolve, 1);
    QMutexLocker locker(&d->mutex);
    auto requestIt = d->writeRequests.find(request.id.value);
    if (!d->operational || !request.isValid() || requestIt == d->writeRequests.end()
        || requestIt->state != PendingWriteRequestRecord::State::Pending
        || !(requestIt->version == request.writeVersion)
        || !(requestIt->readiness == completion) || !d->completions->verifyTerminal(completion).succeeded()) {
        return {};
    }
    const QSharedPointer<KisPageReplicaProvider> provider =
        d->owner.provider(requestIt->replica.provider, requestIt->replica.providerEpoch);
    if (!provider)
        return {};

    const KisPageLeaseId leaseId = d->owner.nextLeaseId();
    if (!leaseId.isValid())
        return {};
    requestIt->state = PendingWriteRequestRecord::State::Resolving;
    PendingWriteRequestRecord pending = requestIt.value();
    locker.unlock();
    KisReplicaAccess access = provider->resolveAccess(leaseId,
                                                      pending.accessOperation,
                                                      pending.replica,
                                                      pending.access,
                                                      KisPageAccessMode::Write);
    locker.relock();
    requestIt = d->writeRequests.find(request.id.value);
    if (requestIt == d->writeRequests.end() || requestIt->state != PendingWriteRequestRecord::State::Resolving) {
        if (access.isValid()) {
            locker.unlock();
            provider->releaseAccess(std::move(access), {});
        }
        return {};
    }
    if (!access.isValid(pending.access)) {
        requestIt->state = PendingWriteRequestRecord::State::Pending;
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
    const std::shared_ptr<ActiveWriteRecord> active =
        std::make_shared<ActiveWriteRecord>(provider, std::move(access), std::move(pending));
    lease.m_lifetime = active;
    d->activeWrites.insert(leaseId.value, active);
    d->writeRequests.erase(requestIt);
    return lease;
}

bool KisPageStore::cancel(const KisReadRequest &request)
{
    QMutexLocker locker(&d->mutex);
    return d->readCoordinator.cancelLocked(request, locker);
}

bool KisPageStore::cancel(const KisWriteRequest &request)
{
    QMutexLocker locker(&d->mutex);
    auto requestIt = d->writeRequests.find(request.id.value);
    if (!d->operational || !request.isValid() || requestIt == d->writeRequests.end()
        || requestIt->state != PendingWriteRequestRecord::State::Pending
        || !(requestIt->transaction == request.transaction)
        || !(requestIt->baseVersion == request.baseVersion) || !(requestIt->version == request.writeVersion)
        || !(requestIt->writer == request.writer) || !(requestIt->readiness == request.readiness)
        || !(requestIt->access == request.access) || requestIt->writeMode != request.mode) {
        return false;
    }
    const auto result = d->cancelWriteLocked(requestIt.value());
    if (!result.accepted)
        return false;
    d->releaseGenericWrite(requestIt->transaction, requestIt->version.key);
    d->writeRequests.erase(requestIt);
    d->retireEffectsLocked(result.effects, locker);
    return true;
}

void KisPageStore::release(KisReadLease lease, const KisCompletionTicket &consumerLastUse)
{
    QMutexLocker locker(&d->mutex);
    auto keys = d->readCoordinator.retryReleasedReadsLocked();
    const auto released = d->readCoordinator.releaseLocked(std::move(lease), consumerLastUse, locker);
    if (released.isValid()) keys.append(released);
    if (keys.isEmpty()) return;
    const auto retirements = d->historyCollector.collectUnreachableLocked(keys);
    d->retireEffectsLocked(retirements, locker);
}

bool KisPageStore::acknowledgeLastUse(const KisVerifiedCompletion &completion)
{
    if (!completion.isValid())
        return false;
    QMutexLocker locker(&d->mutex);
    bool retriedAcknowledge = false;
    auto releasedKeys = d->readCoordinator.retryReleasedReadsLocked(completion.ticket(),
                                                                    &retriedAcknowledge);
    const auto acknowledged = d->readCoordinator.acknowledgeLastUseLocked(completion);
    releasedKeys += acknowledged.releasedKeys;
    const auto retirements = d->historyCollector.collectUnreachableLocked(releasedKeys);
    d->retireEffectsLocked(retirements, locker);
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
    const auto active = d->activeWrites.value(lease.m_leaseId.value);
    if (!lease.isValid() || !active || lease.m_lifetime.get() != active.get())
        return {};
    d->writeCoordinator.beginPreparationActivity(active->request.transaction);
    const auto preparationClaim = qScopeGuard([&] {
        d->writeCoordinator.endPreparationActivity(active->request.transaction);
    });
    lock.unlock();
    active->provider->releaseAccess(std::move(active->access), completion);
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
        const auto transaction = d->epochs.transaction(request.transaction);
        if (transaction.isActive()) {
            QVector<KisPagePublicationCoordinator::OverlayChange> changes;
            changes.append(
                {request.version.key, proof, false});
            overlay = d->publicationCoordinator.prepareOverlayUpdateLocked(
                transaction.transaction, std::move(changes), nullptr);
        }
        success = overlay.isValid();
        if (success)
            proof = {};
    }
    QVector<KisPageTransitionEffect> retirements;
    KisPageMetadataCoordinator::DeferredPublicationCleanup metadataCleanup;
    if (success) {
        lock.unlock();
        success = overlay.prepare(nullptr);
        lock.relock();
    }
    if (success) {
        success = overlay.tryInstallLocked(
            &retirements, &metadataCleanup, nullptr);
    }
    const auto disposeCleanup = qScopeGuard([&] {
        lock.unlock();
        disposeDeferredMetadataCleanup(
            std::move(metadataCleanup), d->metadataCleanupStatistics);
        lock.relock();
    });
    if (!success) {
        if (proof.isValid()) d->owner.revokePreparedPage(proof);
        const auto cancelled = d->cancelWriteLocked(request);
        if (!cancelled.accepted)
            return {}; // consumed lease remains abort-retryable in activeWrites
        retirements = cancelled.effects;
    }
    d->activeWrites.remove(lease.m_leaseId.value);
    d->retireEffectsLocked(retirements, lock);
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

bool KisPageStore::preparedPageExtent(const KisPageTransaction &transaction,
                                      KisSurfaceId surface,
                                      QRect *extent,
                                      QString *error) const
{
    QMutexLocker locker(&d->mutex);
    return d->publicationCoordinator.preparedPageExtentLocked(transaction, surface, extent, error, locker);
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
    QMutexLocker locker(&d->mutex);
    return d->publicationCoordinator.abortLocked(transaction, locker);
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

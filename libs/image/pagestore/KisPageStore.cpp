/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageStore.h"
#include "KisCpuResidentBinding_p.h"

#include "KisImageEpochReferenceModel.h"
#include "KisPageDefaultStorage_p.h"
#include "KisDeferredMetadataCleanup_p.h"
#include "KisPageHistoryCollector_p.h"
#include "KisPageMetadataCoordinator.h"
#include "KisPageOwnerLedger.h"
#include "KisPagePublicationCoordinator_p.h"
#include "KisPageReadCoordinator_p.h"
#include "KisPageRetirementQueue_p.h"
#include "KisPageStoreDiagnostics_p.h"
#include "KisPageStoreReclamation_p.h"
#include "KisPageWriteCoordinator_p.h"
#include "KisPageWaitCondition_p.h"

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>
#include <QSet>
#include <QThread>

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
                         const KisPageReplicaProvider &provider, const KisReplicaHandle *source = nullptr)
{
    return replica.isValid() && replica.version == version
        && replica.provider == provider.providerId()
        && replica.providerEpoch == provider.providerEpoch()
        && (!source || !(replica.allocationIdentity() == source->allocationIdentity()));
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
    ActiveWriteRecord(const std::shared_ptr<KisPageReplicaProvider> &providerValue,
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

    std::shared_ptr<KisPageReplicaProvider> provider;
    std::optional<KisReplicaAccess> access;
    WriteClosureRecord request;
};

using ActiveWriteMap = std::map<quint64, std::shared_ptr<ActiveWriteRecord>, std::less<quint64>,
    KisMutationStorageAllocator<std::pair<const quint64, std::shared_ptr<ActiveWriteRecord>>>>;
using PendingWriteMap = std::map<quint64, PendingWriteRequestRecord, std::less<quint64>,
    KisMutationStorageAllocator<std::pair<const quint64, PendingWriteRequestRecord>>>;

struct PendingArchiveRecord {
    bool busy = false;
    KisPageVersion version;
    KisCompletionTicket completion;
    std::shared_ptr<KisExactGenerationArchive> archive;
};
using PendingArchiveMap = std::map<quint64, PendingArchiveRecord, std::less<quint64>,
    KisMutationStorageAllocator<std::pair<const quint64, PendingArchiveRecord>>>;

} // namespace

class KisPageStore::Private
{
public:
    using Storage = KisMutationStorageAllocator<Private>;
    explicit Private(const Storage &allocator)
        : storage(allocator)
        , metadataCleanupStatistics(std::allocate_shared<DeferredMetadataCleanupStatistics>(allocator))
        , owner(allocator)
        , writeAdmission(mutex, writeAdmissionChanged, &backingBudget, &operational)
        , writeCoordinator(metadata, epochs, backingBudget, owner)
        , defaultStorage(backingBudget, allocator)
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
                           &Private::hasAbandonedMutations,
                           &Private::collectAbandonedMutations,
                           +[](void *context) {
                               return static_cast<Private *>(context)->publicationCoordinator.nextIdleDeadlineLocked();
                           },
                           +[](void *context, qint64 now, qsizetype budget) {
                               static_cast<Private *>(context)->publicationCoordinator.collectExpiredIdleCapacityLocked(now, budget);
                           })
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
        if (!kisEnqueuePageStoreReclamation(nullptr)) throw std::bad_alloc();
        owner.attachBackingBudget(backingBudget);
        metadata.attachBackingBudget(backingBudget);
        epochs.attachBackingBudget(backingBudget);
        metadata.attachRetirementDebtOwner(this,
                                           &Private::prepareRetirementDebt,
                                           &Private::commitRetirementDebt,
                                           &Private::cancelRetirementDebt);
    }

    ~Private()
    {
        owner.detachRetirementIndexesForDestruction();
    }

    static void destroy(Private *owner) noexcept
    {
        auto allocator = owner->storage;
        std::destroy_at(owner);
        allocator.deallocate(owner, 1);
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


    static bool hasAbandonedMutations(void *context)
    {
        return static_cast<Private *>(context)->orphanedMutations != nullptr;
    }
    static void collectAbandonedMutations(void *context);

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
        cleanup.dispose(owner->metadataCleanupStatistics);
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
        result.activeReadRanges = readSnapshot.nativeRanges;
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
        return writeCoordinator.cancelLeaseWrite(request.transition(KisPageTransitionKind::CancelWrite));
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
                                     const std::shared_ptr<KisPageReplicaProvider> &provider,
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
                                 decltype(std::declval<KisPagePublicationCoordinator &>()
                                              .prepareDescriptorLocked({}, {})) &descriptors,
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
        const std::shared_ptr<KisPageReplicaProvider> provider = owner.providerFor(access);
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
        Q_ASSERT(descriptors.empty());
        // The acquiring read owns cleanup through registration of its pin.
        // Unlocking here, even to dispose an empty map, allows historical
        // default collection between materialization and AcquireRead.
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
        KisReplicaOperation allocation;
        try {
            locker->unlock();
            const auto done = qScopeGuard([&] { locker->relock(); --activeProviderCalls; });
            allocation = provider->requestReplica(
                operation, version, descriptor, access.domain, KisPageAccessMode::Read, priority);
        } catch (const std::bad_alloc &) {
            defaultStorage.finishPreparationLocked(version.key);
            KisPageStoreDetail::setError(error, QStringLiteral("implicit default provider storage was refused"));
            return false;
        }
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

    Storage storage;
    mutable QMutex mutex;
    KisPageWaitCondition writeAdmissionChanged;
    std::shared_ptr<DeferredMetadataCleanupStatistics> metadataCleanupStatistics;
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
    bool facadeGone = false;
    bool backgroundReclamation = true;
    qsizetype activeProviderCalls = 0;
    QString closeFailure;
    std::shared_ptr<KisCompletionRegistry> completions;
    // A source accepted during cold configuration cannot be registered again
    // when its first ticket refuses. Retain that original receipt until ready.
    std::shared_ptr<KisCompletionRegistry> configurationSourceOwner;
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
    std::shared_ptr<KisExactGenerationArchive> exactArchive;
    PendingArchiveMap pendingArchives{std::less<quint64>{}, PendingArchiveMap::allocator_type(&backingBudget)};
    KisPageRetirementRecords pendingShutdownReplicas;
    PendingWriteMap writeRequests{std::less<quint64>{}, PendingWriteMap::allocator_type(&backingBudget)};
    ActiveWriteMap activeWrites{std::less<quint64>{}, ActiveWriteMap::allocator_type(&backingBudget)};
    KisPageMutationSession::Private *orphanedMutations = nullptr;
    // Transaction lifetime claims and per-page write claims, not request or
    // access-lease maps. The issued scope owns unpublished native records.
    KisPageMutationStatistics mutationStats;
    quint64 writeRequestsCreated = 0;
    quint64 synchronousHostWrites = 0;
};

class KisPageMutationSession::Private : public KisReusableMutationStorage
{
public:
    explicit Private(KisPageStore::Private *owner)
        : writes(&owner->backingBudget), coldPages(owner) {}
    struct PageSlot;
    struct Page : KisPagePrivateWrite {
        Page() : publicationRow(KisPagePublicationChange::Replacement(this)) {}
        KisPagePublicationChange publicationRow;
        KisCpuWriteBindingReservation writable;
        KisPageByteArray resetPixel;
        PageSlot *storage = nullptr;
        ~Page() { Q_ASSERT(!reserved && !location.value.isValid()); }
    };
    static_assert(sizeof(Page) <= 832);
    struct PageSlot {
        PageSlot() noexcept : next(nullptr) {}
        std::optional<Page> page;
        PageSlot *next;
    };
    static_assert(std::is_nothrow_default_constructible_v<PageSlot>);
    struct ColdPageSet {
        // This is the original D's workspace, never a backing cache. Only
        // actual writes construct rows; untouched claims carry no Page.
        struct alignas(PageSlot) Block {
            Block *next = nullptr;
            size_t count = 0;
            PageSlot *slots() { return reinterpret_cast<PageSlot *>(this + 1); }
            size_t bytes() const { return sizeof(Block) + count * sizeof(PageSlot); }
        };
        KisBackingBudgetController *budget = nullptr;
        Block *blocks = nullptr;
        PageSlot *free = nullptr;
        PageSlot *released = nullptr;
        size_t live = 0;
        size_t capacityBytes = 0;
        explicit ColdPageSet(KisPageStore::Private *owner = nullptr)
            : budget(owner ? &owner->backingBudget : nullptr) {}
        ColdPageSet(ColdPageSet &&other) noexcept { *this = std::move(other); }
        ColdPageSet &operator=(ColdPageSet &&other) noexcept {
            if (this != &other) {
                clear(); budget = other.budget;
                blocks = std::exchange(other.blocks, nullptr);
                free = std::exchange(other.free, nullptr);
                released = std::exchange(other.released, nullptr);
                live = std::exchange(other.live, 0);
                capacityBytes = std::exchange(other.capacityBytes, 0);
            }
            return *this;
        }
        ColdPageSet(const ColdPageSet &) = delete;
        ColdPageSet &operator=(const ColdPageSet &) = delete;
        ~ColdPageSet() { clear(); }
        size_t storageBytes() const noexcept {
            return capacityBytes;
        }
        void clearValues() noexcept {
            if (!live && !released) return;
            free = released = nullptr;
            live = 0;
            for (auto *block = blocks; block; block = block->next)
                for (size_t i = block->count; i > 0; --i) {
                    auto *slot = block->slots() + i - 1;
                    slot->page.reset();
                    slot->next = free;
                    free = slot;
                }
        }
        void clear() noexcept {
            while (blocks) {
                auto *block = blocks;
                blocks = block->next;
                const size_t bytes = block->bytes();
                std::destroy_n(block->slots(), block->count);
                block->~Block();
                kisFreeMutationStorage(budget, block, bytes, alignof(Block));
            }
            free = released = nullptr;
            live = 0;
            capacityBytes = 0;
        }
        struct ReleasedBlocks {
            ColdPageSet *owner;
            PageSlot *head;
            ReleasedBlocks(ColdPageSet *owner, PageSlot *head) : owner(owner), head(head) {}
            ReleasedBlocks(const ReleasedBlocks &) = delete;
            ~ReleasedBlocks() { reset(); }
            bool isEmpty() const { return !head; }
            void reset() noexcept {
                // The scope gate excludes reuse until this unlocked cleanup
                // returns. No provider/binding destructor runs under its gate.
                while (head) {
                    auto *slot = head;
                    head = slot->next;
                    slot->page.reset();
                    slot->next = owner->free;
                    owner->free = slot;
                    --owner->live;
                }
            }
        };
        ReleasedBlocks takeReleasedBlocks() noexcept
        { return {this, std::exchange(released, nullptr)}; }
        Page *create(size_t admitted) {
            if (!free) {
                // Start small for sparse/no-op ranges. Once that actual work
                // fills, group the remaining admitted rows up to one bounded
                // block; their addresses never move during acceptance/retry.
                const size_t count = blocks ? std::min(size_t(16), std::max(size_t(2), admitted > live ? admitted - live : 0)) : 2;
                const size_t bytes = sizeof(Block) + count * sizeof(PageSlot);
                auto *block = new (kisAllocateMutationStorage(budget, bytes, alignof(Block))) Block;
                capacityBytes += kisPageStorageBytes(block, alignof(Block));
                block->count = count;
                // Empty optionals need no initialization of the absent Page.
                std::uninitialized_default_construct_n(block->slots(), count);
                block->next = blocks;
                blocks = block;
                for (size_t i = count; i > 0; --i) {
                    auto *slot = block->slots() + i - 1;
                    slot->next = free;
                    free = slot;
                }
            }
            auto *slot = free;
            auto *page = &slot->page.emplace();
            free = slot->next;
            slot->next = nullptr;
            page->storage = slot;
            ++live;
            return page;
        }
        void erase(Page *page) noexcept {
            if (!page) return;
            auto *slot = page->storage;
            Q_ASSERT(slot && slot->page && &*slot->page == page);
            slot->next = released;
            released = slot;
        }
    };
    ~Private() {
        Q_ASSERT(state == State::Detached);
        if (executionStorage)
            kisFreeMutationStorage(&owner->backingBudget, executionStorage, 0, alignof(std::max_align_t));
    }
    std::atomic<quint32> references{0};
    // A fixed one-entry operation can fund its initial borrow in this block.
    // Scalable ranges and extra borrows own transferable execution storage.
    void *initialExecutionStorage = nullptr;
    size_t initialExecutionCount = 0;
    size_t allocationBytes = 0;
    void *executionStorage = nullptr;
    size_t executionCapacity = 0;
    static boost::intrusive_ptr<Private> create(KisPageStore::Private *owner, size_t initialCount = 0);
    static void destroy(Private *scope, bool deferOwnerDestruction = false) noexcept
    {
        auto *owner = scope->owner;
        scope->~Private();
        kisFreeMutationStorage(&owner->backingBudget, scope, 0, alignof(Private));
        KisPageStore::PrivateReleaser::cleanup(owner, deferOwnerDestruction);
    }
    static void retainOrphanLocked(Private *scope) noexcept
    {
        Q_ASSERT(!scope->nextOrphan);
        scope->nextOrphan = scope->owner->orphanedMutations;
        scope->owner->orphanedMutations = scope;
        scope->owner->historyCollector.scheduleLocked();
    }
    static void releaseOrRetain(Private *scope)
    {
        if (!scope->cancelAndAbort()) {
            QMutexLocker lock(&scope->owner->mutex);
            if (!scope->owner->facadeGone) {
                retainOrphanLocked(scope);
                return;
            }
            const bool released = scope->cancelLocked(&lock, true);
            Q_ASSERT(released); Q_UNUSED(released);
        }
        recycleOrDestroy(scope);
    }
    static void recycleOrDestroy(Private *scope)
    {
        // A terminal capacity offer owns one original registry reference.
        // Once consumed/reclaimed, the remaining facade is only a tombstone;
        // its last reference must not offer that identity a second time.
        if (scope->dispose) {
            destroy(scope);
            return;
        }
        // At the actual last public reference there is no permission or
        // result left to preserve. Only the original charged capacity may
        // cross into another operation. An adapter may have retained claims
        // until this last delivery/destruction, so release those first.
        if (scope->admission.isValid()) {
            QMutexLocker lock(&scope->owner->mutex);
            scope->admission.releaseLocked();
        }
        scope->admission = {};
        scope->coldPages.clearValues();
        scope->writes.clearValues();
        scope->provider.reset();
        scope->transaction = {};
        scope->diagnosticOwner = nullptr;
        if (scope->ownsTransaction) destroy(scope);
        else scope->returnTerminalCapacity();
    }
    void returnTerminalCapacity()
    {
        {
            QMutexLocker lock(&mutex);
            if (state != State::Detached || admission.isValid() || dispose) return;
            if (ownsTransaction) {
                // Keep the original unaccepted commit/abort responsibility,
                // without retaining empty arrays that could relieve pressure.
                discardCapacityUnlocked();
                return;
            }
            bytes = allocationBytes + writes.storageBytes() + coldPages.storageBytes() + executionCapacity;
            dispose = [](KisReusableMutationStorage *value) noexcept {
                releaseRetainedCapacity(static_cast<Private *>(value), true);
            };
            intrusive_ptr_add_ref(this);
        }
        // Replacing another completed scope may take its scope gate. Never
        // hold this one's gate across the root's original retain/disposal.
        if (!owner->backingBudget.retainMutationStorage(this)) releaseRetainedCapacity(this, false);
    }
    static void releaseRetainedCapacity(Private *scope, bool deferOwnerDestruction) noexcept
    {
        {
            QMutexLocker lock(&scope->mutex);
            scope->discardCapacityUnlocked();
        }
        // The original registry pin is known terminal: no admission or owned
        // transaction can reappear. Its actual root callback must defer the
        // last Store destruction, just as the previous root disposer did.
        if (scope->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
            destroy(scope, deferOwnerDestruction);
    }
    static void retryOrphans(KisPageStore::Private *owner, KisPageTransactionId transaction = {},
                             bool terminalWithoutFacade = false)
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
            const bool released = scope->cancelAndAbort(terminalWithoutFacade);
            scopeLock.unlock();
            if (released) {
                recycleOrDestroy(scope);
            } else {
                QMutexLocker lock(&owner->mutex);
                if (!owner->facadeGone) {
                    retainOrphanLocked(scope);
                } else {
                    const bool terminal = scope->cancelLocked(&lock, true);
                    Q_ASSERT(terminal); Q_UNUSED(terminal);
                    lock.unlock();
                    destroy(scope);
                }
            }
        }
    }
    bool finishOwnedTransaction(bool publish)
    {
        if (!ownsTransaction) return true;
        if (state != State::Detached) return false;
        std::optional<KisPageReadCleanup> cleanup;
        QMutexLocker lock(&owner->mutex);
        if (owner->facadeGone) {
            ownsTransaction = false;
            return !publish;
        }
        const auto abort = [&] {
            cleanup.emplace(owner->readCoordinator);
            const bool accepted = owner->publicationCoordinator.abortLocked(transaction, lock, *cleanup)
                // An explicit Store abort may already have ended this record.
                // Recheck only a refusal; successful publication/cancellation
                // does not pay a second transaction identity lookup.
                || !owner->publicationCoordinator.hasActiveTransactionLocked(transaction);
            if (accepted) ownsTransaction = false;
            return accepted;
        };
        if (!publish || !owner->publicationCoordinator.hasPreparedDeltaLocked(transaction.id))
            return abort();
        const bool accepted = owner->publicationCoordinator.commitOwnedLocked(
            transaction, nullptr, diagnosticOwner, lock).isValid();
        if (accepted) ownsTransaction = false;
        else abort(); // Refusal retains responsibility on this original scope.
        return accepted;
    }
    bool cancelAndAbort(bool terminalWithoutFacade = false)
    {
        if (!cancelLocked(nullptr, terminalWithoutFacade)) return false;
        // Only the last public reference/orphan retry enters here. A sealed
        // adapter may still exclude writers until its owned abort is accepted,
        // but the admission table already holds those keys. Keep its original
        // token and return all duplicate rows before the potentially refused
        // abort; an orphan must not pin arrays that could relieve pressure.
        if (ownsTransaction && admission.isValid()) {
            admission.retainTokenForTerminal();
            discardCapacityUnlocked();
        }
        const bool finished = terminalWithoutFacade || finishOwnedTransaction(false);
        if (!finished) {
            discardCapacityUnlocked();
            provider.reset();
        }
        return finished;
    }
    void discardCapacityUnlocked()
    {
        Q_ASSERT(!admission.isValid() || admission.isTerminal());
        writes = KisMutationWriteSet(&owner->backingBudget);
        coldPages = ColdPageSet(owner);
        if (executionStorage) {
            kisFreeMutationStorage(&owner->backingBudget, executionStorage, 0, alignof(std::max_align_t));
            executionStorage = nullptr;
            executionCapacity = 0;
        }
    }
    void detachLocked(QMutexLocker<QMutex> &lock)
    {
        finishSegmentLocked(lock, true);
    }
    void finishSegmentLocked(QMutexLocker<QMutex> &lock, bool terminal, bool retainAdmission = false)
    {
        Q_ASSERT(!retainAdmission || terminal);
        if (!retainAdmission) {
            admission.releaseLocked();
            admission = {};
        }
        // Adapter delivery retains these same entries and their ClaimSet,
        // never a copied range or a second permission. The sealed scope is
        // inactive; its destructor releases admission before entry storage,
        // while the original scope allocation still keeps owner alive.
        // Storage carries the original controller. Keep the session activity
        // and owner alive until its actual free/charge return is complete.
        if (!retainAdmission) {
            ++owner->activeProviderCalls;
            lock.unlock();
            coldPages.clearValues();
            writes.clearValues();
            const size_t capacity = writes.storageBytes() + coldPages.storageBytes();
            if (capacity + allocationBytes + executionCapacity > KisBackingBudgetController::ReusableMutationBytes) {
                discardCapacityUnlocked();
            }
            lock.relock();
            --owner->activeProviderCalls;
        }
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
        state = terminal ? State::Detached : State::Active;
    }
    void cancelPageLocked(KisMutationPageEntry &value, Page &page)
    {
        auto *entry = &value;
        Q_ASSERT(entry && !entry->isExposed());
        page.writable.reset();
        // The scope claims exclude installation. Cancellation unlinks the
        // original result and transfers its already funded retirement record.
        if (!owner->metadata.cancelPrivateResult(&page))
            qFatal("CPU private result lost its cancellation reservation");
        if (!page.backing) qFatal("CPU private result lost its original backing");
        owner->retirementQueue.acceptDetachedRecord(*page.backing, page.target);
        page.backing = nullptr;
        owner->writeCoordinator.recordCancelled(*entry);
        if (entry->isCpuWrite())
            ++owner->mutationStats.pagesCancelled;
    }
    void releaseStorageLocked(QMutexLocker<QMutex> &lock,
                              std::shared_ptr<const KisPageReplicaSource> source = {})
    {
        auto hot = writes.takeReleasedBlocks();
        auto cold = coldPages.takeReleasedBlocks();
        if (hot.isEmpty() && cold.isEmpty() && !source)
            return;
        ++owner->activeProviderCalls;
        lock.unlock();
        source.reset();
        cold.reset();
        hot.reset();
        lock.relock();
        --owner->activeProviderCalls;
    }
    bool cancelLocked(QMutexLocker<QMutex> *heldOwnerLock = nullptr,
                      bool terminalWithoutFacade = false)
    {
        if (state == State::Detached || !owner)
            return true;
        if (executions || counters.activePins) return false;
        std::optional<QMutexLocker<QMutex>> acquired;
        if (!heldOwnerLock) {
            acquired.emplace(&owner->mutex);
            heldOwnerLock = &*acquired;
        }
        auto &lock = *heldOwnerLock;
        Q_ASSERT(!terminalWithoutFacade || owner->facadeGone);
        const auto retire = qScopeGuard([&] {
            releaseStorageLocked(lock);
            if (!terminalWithoutFacade) owner->processRetirementsLocked(lock);
        });
        // Without a facade no publication or explicit whole-owner drain can
        // consume these declarations. The provider remains the physical owner
        // until its last external capability/reference releases the bytes.
        // An abandoned scope must not keep that composition root alive merely
        // to retry an unaccepted logical cancellation indefinitely.
        for (auto slot = writes.firstEntry(); !terminalWithoutFacade && slot.isValid();) {
            const auto next = writes.nextEntry(slot);
            auto *entry = writes.at(slot);
            if (Page *page = pageFor(entry)) cancelPageLocked(*entry, *page);
            // Also discard an admitted cold slot whose target preparation
            // failed. Such a slot is not returned by pageFor().
            erasePage(*entry);
            const bool removal = entry->isRemoval();
            auto source = entry->initializationSource();
            entry->setInitializationSource({});
            // Keep this original ClaimSet until the entire cancellation is
            // accepted. finishSegment releases it once, including intents
            // whose initial claim was refused by a different writer.
            owner->mutationStats.removalsCancelled += removal;
            releaseStorageLocked(lock, std::move(source));
            slot = next;
        }
        if (terminalWithoutFacade)
            for (auto slot = writes.firstEntry(); slot.isValid(); slot = writes.nextEntry(slot))
                if (auto *page = pageFor(writes.at(slot)); page && page->reserved) {
                    page->writable.reset();
                    const bool cancelled = owner->metadata.cancelPrivateResult(page);
                    Q_ASSERT(cancelled); Q_UNUSED(cancelled);
                }
        ++owner->activeProviderCalls;
        lock.unlock();
        coldPages.clearValues();
        if (!terminalWithoutFacade)
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
    bool ownsTransaction = false;
    std::shared_ptr<KisPageReplicaProvider> provider;
    KisMutationWriteSet writes;
    ColdPageSet coldPages;
    KisPageWriteAdmission::ClaimSet admission;
    Page *ensurePage(KisMutationPageEntry &entry) try
    {
        if (auto *page = static_cast<Page *>(entry.coldPage()))
            return page;
        auto *page = coldPages.create(writes.size());
        entry.setColdPage(page);
        entry.publicationRow = &page->publicationRow;
        return page;
    } catch (const std::bad_alloc &) {
        return nullptr;
    }
    static Page *pageFor(const KisMutationPageEntry *entry)
    {
        auto *page = entry ? static_cast<Page *>(entry->coldPage()) : nullptr;
        // Acceptance alone installs this reservation; cancel/install revoke
        // it before the original row can disappear. Full provider identity
        // validation belongs to that boundary, not every internal visit.
        return page && page->reserved ? page : nullptr;
    }
    void erasePage(KisMutationPageEntry &value)
    {
        auto *entry = &value;
        auto *page = static_cast<Page *>(entry->coldPage());
        entry->setColdPage(nullptr);
        entry->publicationRow = nullptr;
        coldPages.erase(page);
    }
    void discardUnpreparedPageLocked(KisMutationPageEntry &entry, QMutexLocker<QMutex> &lock)
    {
        auto *page = static_cast<Page *>(entry.coldPage());
        Q_ASSERT(page && !page->reserved);
        owner->metadata.discardPrivateValue(page);
        auto layout = std::move(page->descriptor);
        erasePage(entry);
        if (layout) {
            if (lock.isLocked()) lock.unlock();
            layout.reset();
            lock.relock();
        }
    }
    void clearPublishedRows()
    {
        for (auto slot = writes.firstEntry(); slot.isValid(); slot = writes.nextEntry(slot)) {
            auto *entry = writes.at(slot);
            entry->setInitializationSource({});
            entry->setColdPage(nullptr);
            entry->publicationRow = nullptr;
        }
    }
    KisMutationPageEntry *claimEntryLocked(const KisPageWriteIntent &intent, QMutexLocker<QMutex> &lock,
                          KisPageWriteAdmission::ClaimOrigin origin,
                          QString *error = nullptr, KisMutationPageEntry *entry = nullptr) try
    {
        ++owner->activeProviderCalls;
        const auto preparation = qScopeGuard([&] {
            if (!lock.isLocked()) lock.relock();
            --owner->activeProviderCalls;
        });
        // The caller holds this session's mutex, so the sole write set cannot
        // change while its actual storage is prepared outside the store gate.
        if (!entry) {
            lock.unlock();
            entry = &writes.getOrCreate(intent);
            lock.relock();
        }
        if (!admission.isValid()) admission = owner->writeAdmission.beginClaimSet(writes);
        return owner->writeAdmission.claimOne(admission, writes.handleOf(*entry), lock, error, origin)
            ? entry : nullptr;
    } catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("mutation entry/admission storage is unavailable"));
        state = State::Failed;
        return nullptr;
    }
    bool selectCpuProviderLocked(QString *error)
    {
        if (provider)
            return true;
        const KisPageAccessRequirement cpu{
            KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
        const auto selected = owner->owner.providerFor(cpu);
        const auto capabilities = selected ? selected->capabilities() : KisReplicaCapabilities{};
        if (!selected || !capabilities.synchronousOperations
            || !capabilities.nativeCpuMutation || !capabilities.synchronousWriteCopy) {
            KisPageStoreDetail::setError(error, QStringLiteral(
                "native CPU mutation provider is unavailable or ambiguous"));
            return false;
        }
        provider = selected;
        ++owner->mutationStats.sessionsCreated;
        return true;
    }
    bool stageSemantic(const KisPageKey &key,
                       const std::shared_ptr<const KisPageReplicaSource> &source,
                       bool removal, QString *error)
    {
        QMutexLocker scopeLock(&mutex);
        if (state != State::Active || executions || thread != QThread::currentThreadId() || !key.isValid()
            || (!removal && !source)) {
            KisPageStoreDetail::setError(error, removal ? QStringLiteral("mutation is unavailable or used from another thread")
                                    : QStringLiteral("alias mutation/input is unavailable"));
            return false;
        }
        auto *entry = writes.find(key);
        if (entry && !entry->acceptsMutableInput()) {
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
        entry = claimEntryLocked(intent, lock, KisPageWriteAdmission::ClaimOrigin::NativeSession, error, entry);
        if (!entry) {
            state = State::Failed;
            return false;
        }
        const auto plan = owner->writeCoordinator.select(intent, entry);
        Page *pending = plan == KisPageWritePlanKind::ReusePending ? static_cast<Page *>(entry->coldPage()) : nullptr;
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
    KisPageMutationExecution::Private *executions = nullptr;
    Private *nextOrphan = nullptr;
    enum class State : quint8 { Active, Sealing, Failed, Detached };
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
    struct PreparedTarget {
        Page *page = nullptr;
        // A successful mutable preparation lends one local pin to its caller,
        // which either exposes it or releases it before returning from preflight.
        void *data = nullptr;
        bool payloadInitialized = false;
    };
    PreparedTarget prepareTarget(KisMutationPageEntry &entry,
                                 KisPageWriteIntent intent, const KisCpuPagePayload *payload,
                                 Counters &local, QMutexLocker<QMutex> &lock,
                                 KisPageStoreDiagnosticTimer &diagnostic, QString *error,
                                 KisPageWriteSelection *selected = nullptr, bool *storageRefused = nullptr);
};

KisPageMutationSession::Private::PreparedTarget
KisPageMutationSession::Private::prepareTarget(
    KisMutationPageEntry &value, KisPageWriteIntent intent,
    const KisCpuPagePayload *payload, Counters &local, QMutexLocker<QMutex> &lock,
    KisPageStoreDiagnosticTimer &diagnostic, QString *error,
    KisPageWriteSelection *selected, bool *storageRefused)
{
    if (storageRefused) *storageRefused = false;
    auto *entry = &value;
    const bool alias = intent.inputKind == KisPageWriteInputKind::Semantic;
    auto *previous = pageFor(entry);
    Q_ASSERT(alias || !static_cast<Page *>(entry->coldPage()));
    if (alias && entry->hasAcceptedAlias()) return {previous};

    auto initialization = entry->initializationSource();
    if (!alias && initialization && intent.mode == KisPageWriteMode::DiscardContents) {
        entry->setInitializationSource({});
        initialization.reset();
    }
    std::shared_ptr<KisPageReplicaProvider> aliasProvider;
    if (alias && initialization)
        aliasProvider = owner->owner.provider(initialization->provider(), initialization->providerEpoch());
    const auto &targetProvider = alias ? aliasProvider : provider;
    if (alias) {
        if (!initialization || !targetProvider
            || !targetProvider->capabilities().synchronousSourceAdoption) return {};
    } else if (initialization
        && (!(initialization->provider() == targetProvider->providerId())
            || !(initialization->providerEpoch() == targetProvider->providerEpoch()))) {
        KisPageStoreDetail::setError(error, QStringLiteral("alias writable provider does not match its input"));
        return {};
    }
    if (entry->isRemoval()) intent.flags |= quint8(KisPageWriteIntentFlag::SemanticRemoval);
    std::optional<KisPageWriteSelection> localSelection;
    if (!selected) {
        selected = &localSelection.emplace();
        if (previous) {
            selected->write = owner->writeCoordinator.writeInput(*entry, transaction.id);
            selected->layout = previous->descriptor;
        } else if (!owner->publicationCoordinator.selectWriteBaseLocked(transaction, intent, selected, error)) return {};
    }
    auto &acquire = selected->write;
    const auto &descriptor = selected->descriptor();
    if (alias && !(descriptor == initialization->descriptor())) return {};
    acquire.operation = owner->owner.nextOperationId();
    // The input retains the source through checkout. Recovery may replace it
    // only on success; Fresh and rejection consume this same original fact.
    const auto &source = acquire.source;
    if (!alias && source.isValid() && (!(source.provider == targetProvider->providerId())
                        || !(source.providerEpoch == targetProvider->providerEpoch())
                        || source.domain != KisPageAccessDomain::CpuRam)) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation requires an exact native CPU authority"));
        return {};
    }
    if (payload && !payload->isValidFor(descriptor)) {
        KisPageStoreDetail::setError(error, QStringLiteral("complete CPU payload layout is invalid"));
        return {};
    }
    const bool directPayload = payload && targetProvider->capabilities().synchronousCpuPayload;
    intent.mode = acquire.writeMode;
    intent.flags = directPayload ? quint8(KisPageWriteIntentFlag::InputBytesReady) : 0;
    if (initialization && !alias) intent.flags |= quint8(KisPageWriteIntentFlag::SourceInitialization);
    auto *page = previous ? previous : ensurePage(*entry);
    if (!page) {
        if (storageRefused) *storageRefused = true;
        KisPageStoreDetail::setError(error, QStringLiteral("mutation cold storage is unavailable"));
        return {};
    }
    auto discardUnprepared = qScopeGuard([&] {
        if (!page->reserved) discardUnpreparedPageLocked(*entry, lock);
    });
    if (!previous) {
        lock.unlock();
        const bool prepared = owner->metadata.preparePrivateValue(acquire, page, error);
        lock.relock();
        if (!prepared) {
            if (storageRefused) *storageRefused = true;
            return {};
        }
    }
    try {
        if (!previous)
            owner->publicationCoordinator.preparePrivateLayoutLocked(*page, descriptor, selected->layout);
    } catch (const std::bad_alloc &) {
        if (storageRefused) *storageRefused = true;
        KisPageStoreDetail::setError(error, QStringLiteral("private descriptor storage budget was refused"));
        return {};
    }
    auto idle = owner->publicationCoordinator.takeIdleCapacityLocked(intent.key);
    const auto retireIdle = [&] {
        if (!idle.replica.isValid()) return;
        if (lock.isLocked()) lock.unlock();
        const auto relock = qScopeGuard([&] { idle.binding.reset(); lock.relock(); });
        owner->retirementQueue.retireOrDefer(idle.replica, {}, {}, {}, idle.original);
        idle.replica = {};
    };
    const auto releaseUnusedIdle = qScopeGuard(retireIdle);
    KisCpuWriteBindingReservation writable;
    bool reused = false;
    if (alias) {
        // Optional capacity must leave before this same key requests Fresh
        // headroom. Alias neither takes its writer nor invents a generic plan.
        retireIdle();
    } else try {
        reused = owner->writeCoordinator.tryAcquireReusedTargetLocked(
            transaction, intent, targetProvider,
            {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer}, *selected,
            owner->publicationCoordinator, std::exchange(idle, {}), lock, page->target, writable, diagnostic, page);
    } catch (const std::bad_alloc &) {
        if (storageRefused) *storageRefused = true;
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation write plan storage is unavailable"));
        return {};
    }
    PreparedTarget result;
    bool finalAccepted = false;
    if (!reused) {
        // Fresh and alias share one provider activity through preparation,
        // acceptance and funded cleanup. Reuse retains its original scope
        // claims; it does not gain another global busy condition.
        ++owner->activeProviderCalls;
        const auto providerActivity = qScopeGuard([&] {
            if (!lock.isLocked()) lock.relock();
            --owner->activeProviderCalls;
        });
        KisPageBackingPreparation backing;
        KisReplicaOperation allocation;
        KisPageRetirementRecord *registeredBacking = nullptr;
        bool ownsNewTarget = false;
        bool accepted = false;
        // Fresh and alias retain the same funded rejection obligation. An
        // alias refusal leaves the previous private result and its writer
        // intact; no caller has another copy of this terminal policy.
        const auto finishPreparation = qScopeGuard([&] {
            if (lock.isLocked()) lock.unlock();
            if (!accepted && ownsNewTarget) {
                writable.reset();
                owner->retirementQueue.retireOrDefer(allocation.replica, targetProvider, {}, std::move(backing), registeredBacking);
            }
            backing = {};
            lock.relock();
        });
        lock.unlock();
        backing = owner->writeCoordinator.reserveBacking(descriptor, KisPageAccessDomain::CpuRam,
            KisBackingBudgetClass::ActivePending, error, acquire.version, nullptr,
            alias ? KisReplicaSourceUse::ImmutableAlias : KisReplicaSourceUse::WritableCopy);
        if (!backing.reservation.isValid()) {
            if (storageRefused) *storageRefused = true;
            return {};
        }
        std::shared_ptr<KisCpuResidentBinding> binding;
        try {
            KisPageStoreDiagnosticTimer prepare(diagnosticOwner, alias
                ? KisPageStoreDiagnosticPhase::MutationAliasPrepare : KisPageStoreDiagnosticPhase::WriteProviderPrepare, 1);
            allocation = owner->writeCoordinator.prepareFreshReplica(intent, *targetProvider,
                acquire, descriptor, {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                KisPagePriority::Interactive, payload, initialization);
            ownsNewTarget = ownsPreparedReplica(allocation.replica, acquire.version, *targetProvider,
                alias ? nullptr : &source)
                && (!previous || !(allocation.replica.physicalSlotIdentity() == previous->target.physicalSlotIdentity()));
            if (ownsNewTarget) binding = targetProvider->cpuResidentBinding(allocation.replica);
        } catch (const std::bad_alloc &) {
            if (storageRefused) *storageRefused = true;
            KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation provider preparation storage was refused"));
            return {};
        }
        backing.retirement->backingNode.second.nativeBinding = binding;
        auto *preparedBacking = backing.retirement.get();
        const bool backingOwned = ownsNewTarget && owner->owner.registerBacking(
            allocation.replica, backing.reservation, KisBackingBudgetClass::ActivePending, error, &backing.retirement);
        if (backingOwned) registeredBacking = preparedBacking;
        const bool finalPayload = !alias && intent.inputKind == KisPageWriteInputKind::FinalPayload && directPayload;
        if (!alias && !finalPayload && ownsNewTarget) {
            writable = KisCpuWriteBindingReservation::acquire(binding, allocation.replica.allocationIdentity());
            result.data = pinWritable(writable, local);
        }
        const bool ready = backingOwned && allocation.isValid() && allocation.status == KisPageRequestStatus::Ready
            && allocation.replica.domain == KisPageAccessDomain::CpuRam
            && allocation.replica.layout.matches(descriptor)
            && (alias || (binding && (finalPayload || result.data)))
            && owner->owner.verifyTerminalProviderResult(acquire.operation, allocation, error).succeeded();
        lock.relock();
        if (!ready) {
            if (error && error->isEmpty()) *error = QStringLiteral("CPU mutation native allocation/access failed");
            return {};
        }
        auto *oldBacking = previous ? previous->backing : nullptr;
        std::optional<KisReplicaHandle> oldTarget;
        if (previous) oldTarget.emplace(previous->target);
        accepted = previous ? owner->metadata.replacePrivateWrite(page, allocation.replica)
            : owner->metadata.acceptPrivateTarget(acquire, acquire.source, allocation.replica, page,
                owner->backingBudget.maxTransientVersionsPerPage(), error, {}, alias);
        if (!accepted) return {};
        page->binding = std::move(binding);
        page->backing = registeredBacking;
        if (previous) {
            page->writable.reset();
            if (!oldBacking) qFatal("CPU alias lost its original prior backing");
            owner->retirementQueue.acceptDetachedRecord(*oldBacking, *oldTarget);
        }
        finalAccepted = finalPayload;
        result.payloadInitialized = directPayload;
    }
    Q_ASSERT(page->descriptor);
    if (!previous) {
        page->writable = std::move(writable);
        if (!page->binding) page->binding = page->writable.binding();
        owner->writeCoordinator.recordTarget(*entry, intent, acquire, alias
            ? KisMutationPageEntryState::AliasAccepted
            : finalAccepted ? KisMutationPageEntryState::PrivatePrepared : KisMutationPageEntryState::MutableReady);
        if (alias) ++owner->mutationStats.aliasGenerationsReserved;
        else ++owner->mutationStats.generationsReserved;
    } else {
        owner->writeCoordinator.recordAliasAccepted(*entry);
    }
    discardUnprepared.dismiss();
    if (initialization && !alias) entry->setInitializationSource({});
    result.page = page;
    return result;
}


void KisPageStore::Private::collectAbandonedMutations(void *context)
{
    KisPageMutationSession::Private::retryOrphans(static_cast<Private *>(context));
}

void intrusive_ptr_add_ref(KisPageMutationSession::Private *scope) noexcept
{ scope->references.fetch_add(1, std::memory_order_relaxed); }
void intrusive_ptr_release(KisPageMutationSession::Private *scope) noexcept
{
    if (scope->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
        KisPageMutationSession::Private::releaseOrRetain(scope);
}

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
    Private(const boost::intrusive_ptr<KisPageMutationSession::Private> &owner, size_t size)
        : scope(owner), count(size) {}
    static size_t storageBytes(size_t count)
    {
        static_assert(alignof(Private) >= alignof(BoundEntry));
        static_assert(alignof(Private) >= alignof(quint64));
        const size_t maximum = size_t(std::numeric_limits<qint64>::max());
        const size_t words = count / 64 + bool(count % 64);
        if (!count || count > (maximum - sizeof(Private)) / (sizeof(BoundEntry) + sizeof(quint64)))
            throw std::bad_alloc();
        return sizeof(Private) + (count > 1 ? count * sizeof(BoundEntry) : 0)
            + (count > 64 ? 2 * words * sizeof(quint64) : 0);
    }
    static boost::intrusive_ptr<Private> create(
        const boost::intrusive_ptr<KisPageMutationSession::Private> &scope, size_t count)
    {
        const size_t bytes = storageBytes(count);
        bool embedded = false;
        void *raw = nullptr;
        size_t capacity = 0;
        {
            QMutexLocker lock(&scope->mutex);
            if (scope->initialExecutionStorage && scope->initialExecutionCount >= count) {
                raw = std::exchange(scope->initialExecutionStorage, nullptr);
                embedded = true;
            } else if (scope->executionStorage && scope->executionCapacity >= bytes) {
                raw = std::exchange(scope->executionStorage, nullptr);
                capacity = std::exchange(scope->executionCapacity, 0);
            }
        }
        if (!raw) {
            raw = kisAllocateMutationStorage(&scope->owner->backingBudget, bytes, alignof(std::max_align_t));
            capacity = kisPageStorageBytes(raw, alignof(std::max_align_t));
        }
        Private *value = new (raw) Private(scope, count);
        value->embedded = embedded;
        value->allocationBytes = capacity;
        if (count > 1) std::uninitialized_value_construct_n(value->entries(), count);
        if (count > 64) std::uninitialized_value_construct_n(value->touched(), 2 * ((count + 63) / 64));
        return boost::intrusive_ptr<Private>(value);
    }
    ~Private()
    {
        if (!linked) return;
        // The sole destructor caller holds the structure gate through both
        // unlink and workspace handback. Terminal offering cannot see half
        // of this lifetime, and returning a borrow needs no second gate.
        Q_ASSERT(counters.activePins == 0);
        if (!finished) scope->state = KisPageMutationSession::Private::State::Failed;
        scope->counters.merge(counters);
        if (previous) previous->next = next;
        else scope->executions = next;
        if (next) next->previous = previous;
        // The caller returns workspace under this same gate or frees it
        // outside, with scope still pinned through the actual refund.
    }
    static auto order(const KisPageKey &key)
    { return std::make_tuple(key.surface.value, key.page.row, key.page.column); }
    void link()
    {
        next = scope->executions;
        if (next) next->previous = this;
        scope->executions = this;
        linked = true;
    }
    BoundEntry *entries() { return count == 1 ? &inlineEntry : reinterpret_cast<BoundEntry *>(this + 1); }
    quint64 *touched()
    { return count <= 64 ? &inlineTouched : reinterpret_cast<quint64 *>(entries() + count); }
    quint64 *prepared()
    {
        return count <= 64 ? &inlinePrepared : touched() + (count + 63) / 64;
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
    boost::intrusive_ptr<KisPageMutationSession::Private> scope;
    std::atomic<quint32> references{0};
    QMutex mutex; // one execution, independent of structural growth/cold work
    BoundEntry inlineEntry;
    size_t count = 0;
    // One bit per original BoundEntry, not a duplicate page-key index. The
    // execution's original allocation also owns this storage.
    quint64 inlineTouched = 0;
    // Temporary ownership only: pages prepared for this execution but never
    // exposed by its callback are cancelled before result export.
    quint64 inlinePrepared = 0;
    qsizetype touchedCount = 0;
    Qt::HANDLE thread = QThread::currentThreadId();
    Private *previous = nullptr;
    Private *next = nullptr;
    KisPageMutationSession::Private::Counters counters;
    bool linked = false;
    bool finished = false;
    bool embedded = false;
    size_t allocationBytes = 0;
};

boost::intrusive_ptr<KisPageMutationSession::Private>
KisPageMutationSession::Private::create(KisPageStore::Private *owner, size_t initialCount)
{
    using Execution = KisPageMutationExecution::Private;
    static_assert(alignof(Private) >= alignof(Execution));
    boost::intrusive_ptr<Private> retained;
    if (auto *storage = owner->backingBudget.takeMutationStorage()) {
        retained = boost::intrusive_ptr<Private>(static_cast<Private *>(storage), false);
        Q_ASSERT(retained->owner == owner && retained->state == State::Detached
            && !retained->executions && !retained->ownsTransaction && !retained->admission.isValid());
        if (retained->references.load(std::memory_order_acquire) == 1) {
            // No facade/borrow still names this identity; consume the root's
            // reference and reuse the entire original allocation in place.
            retained->dispose = nullptr;
            retained->provider.reset();
            retained->transaction = {};
            retained->diagnosticOwner = nullptr;
            retained->thread = QThread::currentThreadId();
            return retained;
        }
    }
    // A live terminal facade keeps its stable control identity. A genuinely
    // new operation needs a different control, but can consume its empty
    // arrays and separately allocated execution without rebuilding them.
    auto restore = qScopeGuard([&] {
        if (retained) {
            intrusive_ptr_add_ref(retained.get());
            if (!owner->backingBudget.retainMutationStorage(retained.get()))
                releaseRetainedCapacity(retained.get(), false);
        }
    });
    // Only the fixed one-entry execution shares the control allocation.
    // A scalable range belongs to transferable workspace, never to a facade
    // tombstone that may survive its operation indefinitely.
    const size_t requested = initialCount == 1 ? Execution::storageBytes(1) : 0;
    const size_t extra = retained && retained->executionStorage && retained->executionCapacity >= requested
        ? 0 : requested;
    if (extra > size_t(std::numeric_limits<qint64>::max()) - sizeof(Private)) throw std::bad_alloc();
    void *raw = nullptr;
    Private *scope;
    try {
        raw = kisAllocateMutationStorage(&owner->backingBudget, sizeof(Private) + extra, alignof(Private));
        scope = new (raw) Private(owner);
    } catch (...) {
        if (raw) kisFreeMutationStorage(&owner->backingBudget, raw, 0, alignof(Private));
        throw;
    }
    scope->owner = owner;
    scope->allocationBytes = kisPageStorageBytes(scope, alignof(Private));
    scope->initialExecutionStorage = extra ? scope + 1 : nullptr;
    scope->initialExecutionCount = extra ? initialCount : 0;
    owner->lifetimeReferences.ref();
    if (retained) {
        QMutexLocker lock(&retained->mutex);
        scope->writes = std::move(retained->writes);
        scope->coldPages = std::move(retained->coldPages);
        scope->executionStorage = std::exchange(retained->executionStorage, nullptr);
        scope->executionCapacity = std::exchange(retained->executionCapacity, 0);
    }
    restore.dismiss();
    return boost::intrusive_ptr<Private>(scope);
}

void intrusive_ptr_add_ref(KisPageMutationExecution::Private *execution) noexcept
{ execution->references.fetch_add(1, std::memory_order_relaxed); }
void intrusive_ptr_release(KisPageMutationExecution::Private *execution) noexcept
{
    if (execution->references.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    // The original scope protects the controller through destruction AND free.
    auto scope = execution->scope;
    auto *budget = &scope->owner->backingBudget;
    const bool embedded = execution->embedded;
    const size_t capacity = execution->allocationBytes;
    QMutexLocker lock(&scope->mutex);
    execution->~Private();
    if (embedded) {
        Q_ASSERT(!scope->initialExecutionStorage);
        scope->initialExecutionStorage = execution;
        return;
    }
    if (scope->state != decltype(scope->state.load())::Detached
        && !scope->executionStorage
        && capacity <= KisBackingBudgetController::ReusableMutationBytes) {
        scope->executionStorage = execution;
        scope->executionCapacity = capacity;
        return;
    }
    lock.unlock();
    kisFreeMutationStorage(budget, execution, 0, alignof(std::max_align_t));
}

KisPageMutationExecution KisPageMutationSession::borrowExecution(
    KisSurfaceId surface, const KisPageSnapshotArray<KisLogicalPageId> &inputPages, QString *error) try
{
    KisPageMutationExecution result;
    if (!d || !surface.isValid() || inputPages.isEmpty()) {
        KisPageStoreDetail::setError(error, QStringLiteral("execution range is absent"));
        return result;
    }
    // This was a set-valued input. Preserve that contract at the boundary;
    // canonical adapter ranges share the array without another allocation.
    auto pages = inputPages;
    const auto less = [](const auto &a, const auto &b) {
        return std::make_tuple(a.row, a.column) < std::make_tuple(b.row, b.column);
    };
    if (!std::is_sorted(pages.cbegin(), pages.cend(), less)
        || std::adjacent_find(pages.cbegin(), pages.cend()) != pages.cend()) {
        std::sort(pages.begin(), pages.end(), less);
        pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    }
    // One paid workspace holds the original record, bindings and bitmaps.
    // Before admission, the prepared bitmap records newly created slots;
    // after acceptance it returns to its ordinary preparation purpose.
    auto candidate = KisPageMutationExecution::Private::create(d, size_t(pages.size()));
    auto *created = candidate->prepared();
    QMutexLocker scopeLock(&d->mutex);
    if (d->state != Private::State::Active || (!d->executions && d->counters.activePins)) {
        KisPageStoreDetail::setError(error, QStringLiteral("mutation cannot lend execution"));
        return result;
    }
    auto *range = candidate->entries();
    for (auto *active = d->executions; active; active = active->next)
        for (const auto &page : std::as_const(pages))
            if (active->find({surface, page})) {
                KisPageStoreDetail::setError(error, QStringLiteral("execution range overlaps an outstanding borrow"));
                return result;
            }
    bool accepted = false;
    const auto rollback = qScopeGuard([&] {
        if (accepted) return;
        for (size_t i = 0; i < candidate->count; ++i)
            if ((created[i / 64] & (quint64(1) << (i % 64))) && range[i].entry) {
                const bool erased = d->writes.erase(d->writes.handleOf(*range[i].entry));
                Q_ASSERT(erased);
            }
        d->writes.takeReleasedBlocks().reset();
    });
    // Only structure/cold preparation uses this gate. Bound entries stay at
    // their original addresses across unrelated directory/index growth.
    // The complete set is already known here. Prepare its directory and hot
    // index once; no private result or payload capacity is admitted early.
    if (!d->writes.size()) d->writes.reserveKnownTargetCount(pages.size());
    size_t i = 0;
    for (const auto &page : std::as_const(pages)) {
        KisPageWriteIntent intent; intent.key = {surface, page};
        bool newEntry = false;
        auto &entry = d->writes.getOrCreate(intent, &newEntry);
        range[i] = {&entry, Private::pageFor(&entry)};
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
    std::fill_n(created, (candidate->count + 63) / 64, quint64(0));
    candidate->link();
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
            auto *page = bound.page;
            if (!page)
                continue;
            scope->cancelPageLocked(*bound.entry, *page);
            bound.page = nullptr;
            scope->erasePage(*bound.entry);
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
            if (!entry || entry->isRemoval() || entry->initializationSource() || entry->hasPrivateResult()) {
                KisPageStoreDetail::setError(error, {});
                return PreparationResult::Unsupported;
            }
        }
    }

    bool prepared = true;
    try {
        for (size_t i = 0; i < d->count; ++i) {
            QMutexLocker executionLock(&d->mutex);
            auto *scope = d->scope.get();
            QMutexLocker scopeLock(&scope->mutex);
            if (d->finished || d->thread != QThread::currentThreadId()
                || scope->state != KisPageMutationSession::Private::State::Active) {
                KisPageStoreDetail::setError(error, QStringLiteral("execution range changed during preparation"));
                prepared = false;
                break;
            }
            auto &bound = d->entries()[i];
            // Parked mutable targets already own their capacity. The eventual
            // real borrower restores residency if needed, as before.
            if (bound.page) continue;
            d->markPrepared(i);
            KisPageStoreDiagnosticTimer diagnostic(scope->diagnosticOwner, KisPageStoreDiagnosticPhase::WriteAcquire, 1);
            QMutexLocker ownerLock(&scope->owner->mutex);
            if (!scope->selectCpuProviderLocked(error)) {
                prepared = false;
                break;
            }
            KisPageWriteIntent intent;
            intent.key = bound.entry->key();
            const auto target = scope->prepareTarget(*bound.entry, intent, nullptr,
                                                     d->counters, ownerLock, diagnostic, error);
            bound.page = target.page;
            if (!target.page) {
                prepared = false;
                break;
            }
            ownerLock.unlock();
            // Keep the existing preflight access/residency guarantee. This
            // local pin is not a public borrow, exposure, or touched page.
            void *data = target.data ? target.data : scope->pinWritable(target.page->writable, d->counters);
            if (!data) {
                KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation pending backing cannot be pinned"));
                prepared = false;
                break;
            }
            ++d->counters.pinsAcquired;
            d->counters.maximumPins = qMax(d->counters.maximumPins, quint64(1));
            target.page->writable.unpin();
            ++d->counters.pinsReleased;
        }
    } catch (const std::bad_alloc &) {
        // A cold preparation can throw while obtaining a target, including
        // the implicit default snapshot's COW storage. Keep
        // this refusal on the original preparation/rollback path: unwinding
        // the execution would otherwise cancel the whole history scope.
        KisPageStoreDetail::setError(error, QStringLiteral("execution backing storage preparation was refused"));
        prepared = false;
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
    if (d) session.d = d->scope;
    return session.beginWriteImpl(key, mode, nullptr, error, d);
}
bool KisPageMutationExecution::overwritePage(const KisPageKey &key, const KisCpuPagePayload &payload, QString *error)
{
    KisPageMutationSession session;
    if (d) session.d = d->scope;
    return session.beginWriteImpl(key, KisPageWriteMode::DiscardContents, &payload, error, d).isValid();
}

bool KisPageMutationExecution::writeFinalRect(const KisPageKey &key, const QRect &rect,
    const KisCpuPagePayload &payload, QString *error)
{
    if (!d) return false;
    QMutexLocker executionLock(&d->mutex);
    auto *scope = d->scope.get();
    QMutexLocker scopeLock(&scope->mutex);
    auto *bound = d->find(key);
    if (!bound || !d->linked || d->finished || d->thread != QThread::currentThreadId()
        || scope->state != KisPageMutationSession::Private::State::Active
        || !bound->entry->acceptsMutableInput()) {
        KisPageStoreDetail::setError(error, QStringLiteral("final CPU input requires an unexposed mutable target"));
        return false;
    }
    KisPageStoreDiagnosticTimer diagnostic(scope->diagnosticOwner, KisPageStoreDiagnosticPhase::WriteAcquire, 1);
    auto *entry = bound->entry;
    auto *page = bound->page;
    void *data = nullptr;
    bool initialized = false;
    QRect affected = rect;
    const auto fail = [&] {
        scope->state = KisPageMutationSession::Private::State::Failed;
        return false;
    };
    const auto validRect = [&](const KisPageAllocationDescriptor &descriptor) {
        if (affected.isEmpty()) affected = QRect(QPoint(), descriptor.pageExtent);
        const quint64 rowBytes = quint64(affected.width()) * descriptor.format.pixelStride;
        // Internal uniform fill borrows one immutable row (stride zero).
        // Public complete-payload admission keeps its ordinary layout rules.
        return QRect(QPoint(), descriptor.pageExtent).contains(affected) && payload.data
            && (payload.rowStride >= qint64(rowBytes) || payload.rowStride == 0)
            && quint64(payload.rowStride) <= std::numeric_limits<quint64>::max() / quint64(affected.height())
            && payload.byteSize >= 0 && quint64(payload.byteSize) >= quint64(affected.height()-1) * payload.rowStride + rowBytes;
    };
    if (!page) {
        QMutexLocker ownerLock(&scope->owner->mutex);
        if (!scope->selectCpuProviderLocked(error)) return fail();
        KisPageWriteIntent intent;
        intent.key = key;
        intent.mode = KisPageWriteMode::PreserveContents;
        intent.inputKind = KisPageWriteInputKind::FinalPayload;
        KisPageWriteSelection selection;
        if (!scope->owner->publicationCoordinator.selectWriteBaseLocked(scope->transaction, intent, &selection, error)) return fail();
        auto &selected = selection.write;
        auto &binding = selection.binding;
        const auto &descriptor = selection.descriptor();
        if (!validRect(descriptor)) {
            KisPageStoreDetail::setError(error, QStringLiteral("final CPU rectangle layout is invalid"));
            return fail();
        }
        const quint64 rowBytes = quint64(affected.width()) * descriptor.format.pixelStride;
        if (!selected.baseVersion.isDefaultPixel()) {
            // The original admission and transaction retain this selection.
            // A physical pin covers the comparison, then ends before any
            // writable claim. No captured overlay or second base lookup.
            ++scope->owner->activeProviderCalls;
            ownerLock.unlock();
            const auto restore = qScopeGuard([&] { ownerLock.relock(); --scope->owner->activeProviderCalls; });
            if (!binding) binding = scope->provider->cpuResidentBinding(selected.source);
            const auto *bytes = binding ? static_cast<const quint8 *>(binding->acquireRead(
                selected.source.allocationIdentity(), false, nullptr, true)) : nullptr;
            const auto matches = [&](const void *data) {
                for (int row = 0; row < affected.height(); ++row)
                    if (std::memcmp(static_cast<const quint8 *>(data)
                        + quint64(affected.y()+row) * selected.source.layout.rowStride
                        + quint64(affected.x()) * descriptor.format.pixelStride,
                        static_cast<const quint8 *>(payload.data) + row * payload.rowStride, size_t(rowBytes))) return false;
                return true;
            };
            bool same = false;
            if (bytes) {
                const auto release = qScopeGuard([&] { binding->releaseRead(); });
                same = matches(bytes);
            } else {
                // Generic access consumes the SAME selected replica and
                // transaction. It never resolves a later head after a miss.
                KisPageReadCleanup cleanup(scope->owner->readCoordinator);
                std::optional<KisReadLease> read;
                ownerLock.relock();
                const auto finish = qScopeGuard([&] {
                    if (!ownerLock.isLocked()) ownerLock.relock();
                    if (read && read->isValid()) scope->owner->readCoordinator.releaseLocked(std::move(*read), {}, ownerLock, cleanup);
                    cleanup.finishUnlocked(ownerLock);
                    ownerLock.unlock();
                });
                if (!cleanup.prepareRequestLocked(ownerLock, error)) return fail();
                const auto request = scope->owner->readCoordinator.registerRequestLocked(selected.baseVersion,
                    selected.source, {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                    scope->owner->readyHostCompletion, scope->transaction.id, cleanup);
                read.emplace(scope->owner->readCoordinator.resolveLocked(request, request.readiness, ownerLock, cleanup));
                if (!read->isValid() || !read->cpuData()) {
                    scope->owner->readCoordinator.cancelLocked(request, ownerLock, cleanup);
                    KisPageStoreDetail::setError(error, QStringLiteral("final CPU comparison could not read its exact base"));
                    return fail();
                }
                ownerLock.unlock();
                same = matches(read->cpuData());
            }
            if (same) { KisPageStoreDetail::setError(error, {}); return true; }
        }
        const bool full = affected == QRect(QPoint(), descriptor.pageExtent);
        if (full) { intent.mode = selected.writeMode = KisPageWriteMode::DiscardContents; }
        const auto target = scope->prepareTarget(*entry, intent, full && payload.rowStride ? &payload : nullptr,
            d->counters, ownerLock, diagnostic, error, &selection);
        if (!target.page) return fail();
        page = bound->page = target.page;
        data = target.data;
        initialized = target.payloadInitialized;
    }
    if (!entry->hasPrivateResult()) {
        if (!validRect(*page->descriptor)) {
            KisPageStoreDetail::setError(error, QStringLiteral("final CPU rectangle layout is invalid"));
            return fail();
        }
        if (!data) data = scope->pinWritable(page->writable, d->counters);
        if (!data) {
            KisPageStoreDetail::setError(error, QStringLiteral("final CPU input cannot pin its mutable target"));
            return fail();
        }
        // Fallback/handoff retains its real physical permission, but no
        // public guard or Exposed transition is needed for controlled input.
        ++d->counters.pinsAcquired;
        d->counters.maximumPins = qMax(d->counters.maximumPins, d->counters.activePins + 1);
        if (!initialized) {
            diagnostic.next(KisPageStoreDiagnosticPhase::MutationPayloadCopy, 1);
            const auto rowBytes = quint64(affected.width()) * page->descriptor->format.pixelStride;
            const int rows = affected.height();
            for (int row = 0; row < rows; ++row)
                std::memcpy(static_cast<quint8 *>(data) + quint64(row + affected.y()) * page->target.layout.rowStride
                                + quint64(affected.x()) * page->descriptor->format.pixelStride,
                            static_cast<const quint8 *>(payload.data) + row * payload.rowStride, size_t(rowBytes));
            d->counters.payloadCopyBytes += rowBytes * rows;
        }
        page->writable.unpin();
        ++d->counters.pinsReleased;
        // Controlled input is now immutable in the original entry. Its
        // unpublished result has no metadata consumer until alias preparation
        // or sealing; that consumer registers completion under its own gate.
        scope->owner->writeCoordinator.recordPrivateCompletion(*entry);
    }
    entry->setInitializationSource({});
    entry->setRemoval(false);
    page->resetPixel.clear();
    d->markTouched(size_t(bound - d->entries()));
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPageMutationSession::~KisPageMutationSession() = default;

void KisPageMutationSession::takeTransactionOwnership()
{
    Q_ASSERT(d && d->transaction.isValid() && !d->ownsTransaction);
    d->ownsTransaction = true;
}

bool KisPageMutationSession::ownsTransaction() const
{
    return d && d->ownsTransaction;
}

bool KisPageMutationSession::finishOwnedTransaction(bool publish)
{
    const bool accepted = d && d->finishOwnedTransaction(publish);
    returnTerminalCapacity();
    return accepted;
}
void KisPageMutationSession::returnTerminalCapacity()
{
    // The public facade remains a stable identity for concurrent borrow
    // preparation and terminal queries. Only the original capacity is lent.
    if (d) d->returnTerminalCapacity();
}
bool KisPageMutationSession::isActive() const
{
    if (!d)
        return false;
    QMutexLocker lock(&d->mutex);
    return d->state == Private::State::Active || d->state == Private::State::Sealing;
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
        m_execution = std::move(other.m_execution);
        m_entry = std::exchange(other.m_entry, KisMutationWriteSet::InvalidEntry);
        m_entryIncarnation = std::exchange(other.m_entryIncarnation, 0);
        m_data = std::exchange(other.m_data, nullptr);
        m_rowStride = std::exchange(other.m_rowStride, 0);
        m_byteSize = std::exchange(other.m_byteSize, 0);
    }
    return *this;
}
KisPageMutationSession::Private *KisCpuWriteGuard::scope() const noexcept
{ return m_execution ? m_execution->scope.get() : m_scope.get(); }
void KisCpuWriteGuard::reset()
{
    auto *owner = scope();
    if (owner && m_data) {
        QMutexLocker lock(m_execution ? &m_execution->mutex : &owner->mutex);
        auto *bound = m_execution ? &m_execution->entries()[m_entry] : nullptr;
        auto *entry = bound ? bound->entry : owner->writes.at({m_entry, m_entryIncarnation});
        Q_ASSERT(entry && entry->isExposed());
        auto *page = bound ? bound->page : entry && entry->isExposed() ? owner->pageFor(entry) : nullptr;
        Q_ASSERT(page);
        if (page) {
            // Drop only pixel residency, not the unpublished writer token.
            // The next guard must re-pin; no parked Page retains a pointer.
            page->writable.unpin();
            owner->owner->writeCoordinator.recordExposure(*entry, false);
            auto &counters = m_execution ? m_execution->counters : owner->counters;
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
    auto *owner = scope();
    if (!owner || !m_data)
        return {};
    QMutexLocker lock(m_execution ? &m_execution->mutex : &owner->mutex);
    const auto *bound = m_execution ? &m_execution->entries()[m_entry] : nullptr;
    const auto *entry = bound ? bound->entry : owner->writes.at({m_entry, m_entryIncarnation});
    return entry ? entry->preparedTargetVersion() : KisPageVersion{};
}

bool KisCpuWriteGuard::providerBacking(KisReplicaHandle *replica) const
{
    auto *owner = scope();
    if (replica)
        *replica = {};
    if (!owner || !m_data || !replica)
        return false;
    QMutexLocker lock(m_execution ? &m_execution->mutex : &owner->mutex);
    const auto *bound = m_execution ? &m_execution->entries()[m_entry] : nullptr;
    const auto *entry = bound ? bound->entry : owner->writes.at({m_entry, m_entryIncarnation});
    const auto *page = bound ? bound->page : entry ? owner->pageFor(entry) : nullptr;
    if (!page || !entry->isExposed() || !page->target.isValid()
        || !page->writable.isValid())
        return false;
    *replica = page->target;
    return true;
}

KisPageMutationSession KisPageStore::beginMutation(const KisPageTransaction &transaction, QString *error)
{
    return beginMutation(transaction, {}, nullptr, error);
}

KisPageMutationSession KisPageStore::beginMutation(const KisPageTransaction &transaction,
    KisPageMutationSession reservation, KisPageMutationExecution *execution, QString *error) try
{
    KisPageMutationSession result;
    auto candidate = std::move(reservation.d);
    if ((candidate && (candidate->owner != d.data()
            || candidate->state != KisPageMutationSession::Private::State::Detached
            || !candidate->admission.isValid() || !candidate->writes.size()))
        || (execution && (execution->d || !candidate))) {
        KisPageStoreDetail::setError(error, QStringLiteral("mutation scope cannot activate this range"));
        return result;
    }
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
        if (candidate) {
            if (lock.isLocked()) lock.unlock();
            candidate.reset(); // inert or rejected candidate, before active publication
        }
        if (!lock.isLocked()) lock.relock();
        --d->activeProviderCalls;
    });
    lock.unlock();
    using Scope = KisPageMutationSession::Private;
    if (!candidate) candidate = Scope::create(d.data());
    auto bound = execution ? KisPageMutationExecution::Private::create(candidate,
        size_t(candidate->writes.size())) : boost::intrusive_ptr<KisPageMutationExecution::Private>{};
    lock.relock();
    if (!available()) {
        KisPageStoreDetail::setError(error, QStringLiteral("mutation transaction changed during preparation"));
        return result;
    }
    if (execution) {
        KisSurfaceEpochState state;
        const auto surface = candidate->writes.at(candidate->writes.firstEntry())->key().surface;
        if (!d->publicationCoordinator.resolveSurfaceLocked(surface,
                KisPageReadView::transactionOverlay(transaction.id), &state)) {
            KisPageStoreDetail::setError(error, QStringLiteral("mutation range surface is unavailable"));
            return result;
        }
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
    if (bound) {
        size_t i = 0;
        for (auto slot = candidate->writes.firstEntry(); slot.isValid(); slot = candidate->writes.nextEntry(slot)) {
            auto *entry = candidate->writes.at(slot);
            bound->entries()[i++] = {entry, candidate->pageFor(entry)};
        }
        Q_ASSERT(i == bound->count);
        bound->link();
        execution->d = std::move(bound);
    }
    result.d = std::move(candidate);
    ++d->mutationStats.operationSessionsCreated;
    KisPageStoreDetail::setError(error, {});
    return result;
} catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("mutation scope storage is unavailable"));
    return {};
}

KisPageMutationSession KisPageStore::reserveManagedRange(
    KisSurfaceId surface, const KisPageSnapshotArray<KisLogicalPageId> &targets,
    bool legacyIntent, bool *borrowed, QString *error, bool initialExecution) try
{
    if (borrowed)
        *borrowed = false;
    if (!surface.isValid() || targets.isEmpty()) {
        KisPageStoreDetail::setError(error, QStringLiteral("managed mutation range is invalid"));
        return {};
    }
    // Both private producers supply unique row-major targets. The original
    // slot order is also the execution's search order after adoption.
    Q_ASSERT(std::is_sorted(targets.cbegin(), targets.cend(), [](const auto &a, const auto &b) {
        return std::make_tuple(a.row, a.column) < std::make_tuple(b.row, b.column);
    }));
    Q_ASSERT(std::adjacent_find(targets.cbegin(), targets.cend()) == targets.cend());
    boost::intrusive_ptr<KisPageMutationSession::Private> range;
    QMutexLocker lock(&d->mutex);
    ++d->activeProviderCalls;
    const auto preparation = qScopeGuard([&] {
        if (range) {
            if (lock.isLocked()) lock.unlock();
            range.reset();
        }
        if (!lock.isLocked()) lock.relock();
        --d->activeProviderCalls;
    });
    lock.unlock();
    range = KisPageMutationSession::Private::create(d.data(), initialExecution ? size_t(targets.size()) : 0);
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
    lock.relock();
    while (d->operational) {
        bool mustWait = false;
        bool legacyBorrowed = legacyIntent;
        bool sameThreadReentry = false;
        bool unmanagedConflict = false;
        for (const KisLogicalPageId &page : targets) {
            const auto conflict = d->writeAdmission.conflictLocked(
                {surface, page}, QThread::currentThreadId());
            sameThreadReentry |= conflict == KisPageWriteAdmission::Conflict::ManagedSameThread;
            legacyBorrowed |= conflict == KisPageWriteAdmission::Conflict::LegacyBorrower
                || conflict == KisPageWriteAdmission::Conflict::LegacySameThread;
            mustWait |= conflict == KisPageWriteAdmission::Conflict::ManagedOtherThread;
            unmanagedConflict |= conflict == KisPageWriteAdmission::Conflict::WriterOtherThread
                || conflict == KisPageWriteAdmission::Conflict::WriterSameThread;
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
        if (unmanagedConflict) {
            KisPageStoreDetail::setError(error, QStringLiteral("write set intersects another writer"));
            return {};
        }
        if (!mustWait) {
            if (!range->admission.isValid())
                range->admission = d->writeAdmission.beginClaimSet(range->writes);
            const auto result = d->writeAdmission.claimAll(range->admission, lock, error,
                KisPageWriteAdmission::ClaimOrigin::ManagedRange);
            if (result == KisPageWriteAdmission::Result::Acquired) {
                KisPageMutationSession session;
                session.d = std::move(range);
                return session;
            }
            if (result == KisPageWriteAdmission::Result::Failed) return {};
            // Growth dropped the gate and observed a new writer. Recheck the
            // original range; no pixels or partial claim were accepted.
            continue;
        }
        KisPageStoreDiagnosticTimer waitPhase(
            this, KisPageStoreDiagnosticPhase::PixelOperationRangePrepare, 0);
        waitPhase.next(KisPageStoreDiagnosticPhase::PixelOperationRangeWait, 1);
        d->writeAdmissionChanged.wait(&d->mutex);
    }
    KisPageStoreDetail::setError(error, QStringLiteral("managed mutation store is unavailable"));
    return {};
} catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("managed mutation storage is unavailable"));
    return {};
}

bool KisPageMutationSession::reserveLegacyMutationPage(
    const KisPageKey &key, QString *error, bool *present)
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
    auto *entry = d->writes.find(key);
    const auto resolvePresence = [&] {
        if (!present) return true;
        Q_ASSERT(entry);
        if (entry->isRemoval()) { *present = false; return true; }
        if (entry->initializationSource() || d->pageFor(entry)) { *present = true; return true; }
        KisPageVersion version;
        if (!d->owner->publicationCoordinator.resolveVersionLocked(
                key, KisPageReadView::transactionOverlay(d->transaction.id), &version)) {
            KisPageStoreDetail::setError(error, QStringLiteral("legacy page presence is unavailable"));
            return false;
        }
        *present = !version.isDefaultPixel();
        return true;
    };
    if (entry) {
        const bool held = d->owner->writeAdmission.ownsClaimLocked(d->admission, key);
        KisPageStoreDetail::setError(error, held ? QString{} : QStringLiteral("legacy mutation lost its page claim"));
        return held && resolvePresence();
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
    entry = d->claimEntryLocked(intent, ownerLock, KisPageWriteAdmission::ClaimOrigin::LegacyAdapter, error);
    return entry && resolvePresence();
}

KisPageVersion KisPageMutationSession::legacyMutationPageVersion(const KisPageKey &key) const
{
    if (!d) return {};
    QMutexLocker scopeLock(&d->mutex);
    if (d->state != Private::State::Active || d->executions || d->thread != QThread::currentThreadId()) return {};
    const auto *entry = d->writes.find(key);
    if (!entry || entry->isExposed() || !d->pageFor(entry)) return {};
    QMutexLocker ownerLock(&d->owner->mutex);
    return d->owner->writeAdmission.ownsClaimLocked(d->admission, key)
        ? entry->preparedTargetVersion() : KisPageVersion{};
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
                                       const std::shared_ptr<const KisPageReplicaSource> &source,
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
                                                        const boost::intrusive_ptr<KisPageMutationExecution::Private> &execution)
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
    auto *entry = bound ? bound->entry : d->writes.find(key);
    auto *page = bound ? bound->page : d->pageFor(entry);
    auto &counters = execution ? execution->counters : d->counters;
    if (entry && !entry->acceptsMutableInput()) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation page is exposed or privately completed"));
        return result;
    }
    void *data = nullptr;
    bool payloadInitialized = false;
    if (!d->provider) {
        QMutexLocker lock(&d->owner->mutex);
        if (!d->selectCpuProviderLocked(error))
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
        QString claimError;
        if (!bound) {
            entry = d->claimEntryLocked(intent, lock, KisPageWriteAdmission::ClaimOrigin::NativeSession, &claimError, entry);
            if (!entry) {
                fail(claimError);
                return result;
            }
        }
        Q_ASSERT(entry);
        const auto prepared = d->prepareTarget(*entry, intent, payload, counters, lock, diagnostic, error);
        if (!prepared.page) {
            d->state = Private::State::Failed;
            return result;
        }
        page = prepared.page;
        data = prepared.data;
        payloadInitialized = prepared.payloadInitialized;
    }
    auto initialization = entry->initializationSource();
    if (initialization && mode == KisPageWriteMode::DiscardContents) {
        // A complete overwrite supersedes this still-private semantic input.
        entry->setInitializationSource({});
        initialization.reset();
    }
    if (initialization
        && (!(initialization->provider() == d->provider->providerId())
            || !(initialization->providerEpoch() == d->provider->providerEpoch()))) {
        d->state = Private::State::Failed;
        KisPageStoreDetail::setError(error, QStringLiteral("alias writable provider does not match its input"));
        return result;
    }
    if (payload && !payload->isValidFor(*page->descriptor)) {
        d->state = Private::State::Failed;
        KisPageStoreDetail::setError(error, QStringLiteral("complete CPU payload layout is invalid"));
        return result;
    }
    if (!data)
        data = d->pinWritable(page->writable, counters);
    if (!data) {
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
    if (execution) result.m_execution = execution;
    else result.m_scope = d;
    const auto handle = bound ? KisMutationWriteSet::EntryHandle{} : d->writes.handleOf(*entry);
    result.m_entry = bound ? quint32(bound - execution->entries()) : handle.index;
    result.m_entryIncarnation = handle.incarnation;
    result.m_data = data;
    result.m_rowStride = page->target.layout.rowStride;
    result.m_byteSize = page->target.layout.byteSize;
    if (execution)
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
    const bool accepted = d->cancelLocked();
    lock.unlock();
    if (accepted) returnTerminalCapacity();
    return accepted;
}

bool KisPageMutationSession::seal(QString *error)
{
    const bool accepted = sealImpl(SealMode::Final, error);
    if (accepted) returnTerminalCapacity();
    return accepted;
}

bool KisPageMutationSession::AdapterChanges::forEachImpl(
    const void *visitor, bool (*visit)(const void *, const KisPageKey &, bool)) const
{
    return m_forEach(m_changes, visitor, visit);
}

bool KisPageMutationSession::sealForLegacyUnlock(QString *error,
    const AdapterPreparation &prepareAdapter, const AdapterInstallation &installAdapter)
{
    const bool accepted = sealImpl(SealMode::LegacyFinal, error, nullptr, prepareAdapter, installAdapter);
    if (accepted) returnTerminalCapacity();
    return accepted;
}

bool KisPageMutationSession::sealForLegacyAdapterDelivery(QString *error)
{
    return sealImpl(SealMode::LegacyAdapterDelivery, error);
}

bool KisPageMutationSession::visitAdapterPages(const std::function<void(const KisPageVersion &)> &visit)
{
    if (!d) return true;
    QMutexLocker lock(&d->mutex);
    if (d->executions || d->counters.activePins) return false;
    // A successful seal is already Detached. Otherwise this is the original
    // batch's irreversible cancellation, not a new mutable/readable phase.
    if (d->state != Private::State::Detached) d->state = Private::State::Failed;
    lock.unlock();
    for (auto slot = d->writes.firstEntry(); slot.isValid(); slot = d->writes.nextEntry(slot)) {
        const auto *entry = d->writes.at(slot);
        auto version = entry->preparedTargetVersion();
        version.key = entry->key(); // Intent-only entries still have a real key.
        visit(version);
    }
    return true;
}

KisPageMutationSession::AdapterChanges KisPageMutationSession::sealedAdapterChanges() const
{
    Q_ASSERT(d && d->state == Private::State::Detached);
    return AdapterChanges(d.get(), [](const void *changes, const void *visitor, AdapterChanges::Visitor visit) {
        const auto *scope = static_cast<const Private *>(changes);
        for (auto slot = scope->writes.firstEntry(); slot.isValid(); slot = scope->writes.nextEntry(slot)) {
            const auto *entry = scope->writes.at(slot);
            if ((entry->isRemoval() || entry->hasPrivateResult()) &&
                !visit(visitor, entry->key(), !entry->isRemoval())) return false;
        }
        return true;
    });
}

bool KisPageMutationSession::sealForAdapterDelivery(QString *error, const AdapterPreparation &prepareAdapter)
{
    return sealImpl(SealMode::AdapterDelivery, error, nullptr, prepareAdapter);
}

KisCapturedReadView KisPageMutationSession::checkpointForRead(QString *error)
{
    KisCapturedReadView result;
    sealImpl(SealMode::Checkpoint, error, &result);
    return result;
}

bool KisPageMutationSession::checkpointForAdapterDelivery(QString *error,
    const AdapterPreparation &prepareAdapter, const AdapterInstallation &installAdapter)
{
    // Like borrowExecution, a quiescent checkpoint is not tied to the worker
    // that originally created the business-owned session. This consumer needs
    // only the completed cut, so do not allocate an unused captured read.
    return sealImpl(SealMode::Checkpoint, error, nullptr, prepareAdapter, installAdapter);
}

bool KisPageMutationSession::sealImpl(SealMode mode, QString *error, KisCapturedReadView *checkpoint,
    const AdapterPreparation &prepareAdapter, const AdapterInstallation &installAdapter)
{
    Q_ASSERT(!checkpoint || mode == SealMode::Checkpoint);
    const bool retainAdmission = mode == SealMode::AdapterDelivery || mode == SealMode::LegacyAdapterDelivery;
    if (!d) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation is absent"));
        return false;
    }
    std::unique_lock<QMutex> scopeLock(d->mutex, std::defer_lock);
    if (mode == SealMode::Checkpoint) {
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
        ((mode == SealMode::Final || mode == SealMode::AdapterDelivery)
            && d->thread != QThread::currentThreadId())) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation cannot seal"));
        return false;
    }
    if (d->counters.activePins) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation still has a writable guard"));
        return false;
    }
    if (d->state == Private::State::Failed) {
        if (!retainAdmission) d->cancelLocked();
        KisPageStoreDetail::setError(error, QStringLiteral("CPU mutation was cancelled after a failed write"));
        return false;
    }
    // The same original input remains sealable after preparation refusal,
    // but cannot lend another mutable guard once publication has begun.
    d->state = Private::State::Sealing;
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
    size_t overlayChangeCount = 0;
    quint64 sealedCpuWrites = 0;
    quint64 sealedRemovals = 0;
    quint64 sealedSources = 0;
    for (auto slot = d->writes.firstEntry(); slot.isValid(); slot = d->writes.nextEntry(slot)) {
        const auto index = slot.index;
        auto *entry = d->writes.at(index);
        auto *page = d->pageFor(entry);
        const bool source = bool(entry->initializationSource());
        sealedSources += source;
        if (entry->isRemoval()) {
            ++overlayChangeCount;
            ++sealedRemovals;
            if (page) {
                d->cancelPageLocked(*entry, *page);
                d->erasePage(*entry);
            }
        } else if (page || source) {
            ++overlayChangeCount;
            privatePageCount += bool(page);
            sealedCpuWrites += entry->isCpuWrite();
        }
    }
    // Return detached cold capacity before admitting alias replacements.
    // Otherwise the next prepare would pay for both the dead blocks and their
    // replacements, despite these discarded targets no longer being usable.
    d->releaseStorageLocked(lock);
    const bool initialClaimsHeld = d->claimsHeldLocked();
    ++owner->activeProviderCalls;
    // One seal owns every unlocked preparation/cleanup window. Restore the
    // gate and its activity on exceptions as well as ordinary refusals.
    const auto providerActivity = qScopeGuard([&] {
        if (!lock.isLocked()) lock.relock();
        --owner->activeProviderCalls;
    });
    lock.unlock();
    phase.next(Phase::MutationSealPrivatePublish, quint64(privatePageCount));
    QString failure;
    bool success = true;
    bool storageRefused = false;
    const auto validateClaims = [&](bool held) {
        if (!held) {
            success = false;
            storageRefused = false;
            failure = QStringLiteral("mutation claims were lost");
        }
    };
    validateClaims(initialClaimsHeld);
    const auto completePrivate = [&](KisMutationPageEntry *entry, Private::Page *page) {
        if (page->completed) return;
        success = owner->metadata.completePrivateWrite(page);
        if (!success) failure = QStringLiteral("CPU private result lost its reservation");
        else if (!entry->hasPrivateResult()) owner->writeCoordinator.recordPrivateCompletion(*entry);
    };
    // Alias preparation consumes completed private sources. With no sources,
    // completion and proof production share the single output traversal.
    if (sealedSources) {
        for (auto slot = d->writes.firstEntry(); success && slot.isValid(); slot = d->writes.nextEntry(slot)) {
            auto *entry = d->writes.at(slot);
            if (auto *page = d->pageFor(entry)) completePrivate(entry, page);
        }
        phase.next(Phase::MutationSealOwnerWait, pageWork);
        lock.relock();
        phase.next(Phase::MutationSealInputs, pageWork);
        validateClaims(d->claimsHeldLocked());
        for (auto slot = d->writes.firstEntry(); success && slot.isValid(); slot = d->writes.nextEntry(slot)) {
            const auto index = slot.index;
            auto *entry = d->writes.at(index);
            const auto source = entry->initializationSource();
            if (source) {
                KisPageWriteIntent intent;
                intent.key = entry->key();
                intent.mode = KisPageWriteMode::DiscardContents;
                intent.inputKind = KisPageWriteInputKind::Semantic;
                if (!d->prepareTarget(*entry, intent, nullptr, d->counters, lock, phase,
                                      &failure, nullptr, &storageRefused).page) {
                    success = false;
                    break;
                }
            }
        }
        lock.unlock();
    }
    phase.next(Phase::MutationSealStoragePrepare, quint64(overlayChangeCount));
    KisPagePublicationRows changes;
    try {
        phase.next(Phase::MutationSealProofPrepare, quint64(overlayChangeCount - sealedRemovals));
        if (success && overlayChangeCount) {
            changes = d->writes.publicationRows();
            success = owner->owner.validatePrivateWrites(changes, d->transaction.id, &failure, !sealedSources);
        }
        for (auto slot = d->writes.firstEntry(); success && overlayChangeCount && slot.isValid(); slot = d->writes.nextEntry(slot)) {
            auto *entry = d->writes.at(slot.index);
            if (entry->isRemoval()) continue;
            auto *page = d->pageFor(entry);
            if (!page) continue;
            const bool completeAtSeal = !sealedSources;
            // The batch validator consumed these original results. Capacity
            // retry retains their proof; revocation clears it in place.
            if (completeAtSeal && !entry->hasPrivateResult())
                owner->writeCoordinator.recordPrivateCompletion(*entry);
            // Proof is still private; no writer may survive its publication.
            page->writable.reset();
        }
    } catch (const std::bad_alloc &) {
        success = false;
        storageRefused = true;
        failure = QStringLiteral("overlay storage preparation was refused");
    }

    phase.next(Phase::MutationSealOwnerWait, pageWork);
    lock.relock();
    phase.next(Phase::MutationSealInputs, pageWork);
    // The scope gate and Sealing keep the accepted claim set unchanged.
    // Readers and metadata remain dynamic and are validated by their owners.
    validateClaims(owner->operational && owner->writeCoordinator.transactionHasSession(d->transaction.id));
    const bool hasOverlay = success && overlayChangeCount != 0;
    // Only the bridge can transfer its private anonymous transaction to this
    // scope. Ordinary seal/history/checkpoint retain their overlay contract.
    // Revalidate the sole activity under this same gate at acceptance, where
    // the original commit claim then excludes all new transaction activity.
    const bool canonical = d->ownsTransaction
        && (mode == SealMode::Final || mode == SealMode::AdapterDelivery)
        && owner->writeCoordinator.transactionHasSoleSession(d->transaction.id)
        && !owner->publicationCoordinator.hasPreparedDeltaLocked(d->transaction.id);
    phase.next(Phase::MutationSealStoragePrepare, quint64(overlayChangeCount));
    auto overlay = hasOverlay
        ? owner->publicationCoordinator.preparePagePublicationLocked(
              d->transaction, changes, &failure, &storageRefused, canonical)
        : KisPagePublicationCoordinator::KisPreparedPagePublication{};
    if (success && hasOverlay && !overlay.isValid()) {
        success = false;
        if (failure.isEmpty())
            failure = QStringLiteral("overlay update preparation was rejected");
    }
    const qsizetype metadataChangeCount = overlay.metadataChangeCount();
    lock.unlock();
    // Sealing and the scope gate retain original rows through unlocked
    // preparation, acceptance, adapter installation and candidate cleanup.
    phase.next(Phase::MutationSealMetadataPrepare,
               quint64(metadataChangeCount));
    if (success && hasOverlay && !overlay.prepare(&failure, &storageRefused)) {
        success = false;
        if (failure.isEmpty())
            failure = QStringLiteral("overlay metadata preparation was rejected");
    }
    const AdapterChanges adapterChanges(&overlay,
        [](const void *changes, const void *visitor, AdapterChanges::Visitor visit) {
            return static_cast<const KisPagePublicationCoordinator::KisPreparedPagePublication *>(changes)
                ->forEachAdapterChange(visitor, visit);
        });
    if (success && hasOverlay && prepareAdapter) {
        phase.next(Phase::MutationAdapterPrepare, quint64(overlayChangeCount));
        try { success = prepareAdapter(adapterChanges, &failure); }
        catch (const std::bad_alloc &) { success = false; }
        if (!success) {
            storageRefused = true; // Keep the frozen original segment retryable.
            if (failure.isEmpty()) failure = QStringLiteral("segment adapter preparation was refused");
        }
        phase.next(Phase::MutationAdapterPrepared, quint64(overlayChangeCount));
    }
    phase.next(Phase::MutationSealPublishOwnerWait, pageWork);
    lock.relock();
    phase.next(Phase::MutationSealSurfacePrepare, pageWork);
    if (success && hasOverlay && !overlay.prepareSurfaceLocked(&failure, &storageRefused)) success = false;
    phase.next(Phase::MutationSealInstall, pageWork);
    validateClaims(owner->operational && owner->writeCoordinator.transactionHasSession(d->transaction.id));
    const bool acceptsCanonical = success && hasOverlay && overlay.acceptsCanonical();
    bool accepted = !hasOverlay;
    if (success && hasOverlay) accepted = acceptsCanonical
            ? owner->writeCoordinator.transactionHasSoleSession(d->transaction.id)
                && overlay.tryAcceptCanonicalLocked(d->diagnosticOwner, lock)
            : overlay.tryAcceptLocked(lock, &failure, &storageRefused);
    if (success && !accepted) {
        success = false;
        if (failure.isEmpty())
            failure = QStringLiteral("overlay installation was rejected");
    }
    owner->mutationStats.sealMetadataPreparations +=
        quint64(metadataChangeCount != 0);
    owner->mutationStats.sealMetadataRejections +=
        quint64(metadataChangeCount != 0 && !success);
    if (!success) {
        // Preparation borrows the original proofs until installation accepts.
        // Capacity refusal preserves this frozen segment and its claims;
        // semantic rejection still cancels only this segment.
        if (!storageRefused) d->state = Private::State::Failed;
        lock.unlock();
        phase.next(Phase::MutationSealCleanup, pageWork);
        overlay.finishUnlocked(lock);
        lock.relock();
        retire();
        if (!storageRefused && !retainAdmission) d->cancelLocked(&lock);
        KisPageStoreDetail::setError(error, failure.isEmpty() ? QStringLiteral("CPU mutation seal failed") : failure);
        return false;
    }
    // Acceptance moved authority in metadata. The proof values themselves
    // own no resources and disappear with the original cold pages below.
    if (acceptsCanonical) d->ownsTransaction = false;
    owner->mutationStats.pagesSealed += sealedCpuWrites;
    owner->synchronousHostWrites += sealedSources;
    owner->mutationStats.removalsSealed += sealedRemovals;
    // The chosen destination is installed. Its candidate still supplies the
    // exact accepted changes until finishUnlocked. Adapter callbacks and
    // retirement must not hold the owner gate.
    lock.unlock();
    if (hasOverlay && installAdapter) {
        // Publication is accepted and its candidate/claims are still live.
        // Install cannot refuse; all adapter storage was prepared above.
        phase.next(Phase::MutationAdapterInstall, quint64(overlayChangeCount));
        installAdapter(adapterChanges);
        phase.next(Phase::MutationAdapterInstalled, quint64(overlayChangeCount));
    }
    phase.next(Phase::MutationSealCleanup, pageWork);
    overlay.finishUnlocked(lock);
    d->clearPublishedRows();
    d->coldPages.clearValues();
    owner->retirementQueue.processAcceptedEffects(owner->backgroundReclamation);
    phase.next(Phase::MutationSealOwnerWait, pageWork);
    lock.relock();
    d->finishSegmentLocked(lock, mode != SealMode::Checkpoint, retainAdmission);
    if (checkpoint) {
        // scopeLock excludes the next borrow through capture. The original
        // activity still excludes transaction commit/abort during this gap;
        // capture itself fixes the whole sealed overlay under the owner gate.
        lock.unlock();
        *checkpoint = KisPageStore::captureReadViewImpl(owner,
            KisPageReadView::transactionOverlay(d->transaction.id), error);
        lock.relock();
        if (!checkpoint->isValid()) {
            // The overlay and segment completion were already accepted.
            // Only the read output was refused; keep this original session
            // so a quiescent retry captures that result without replaying it.
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
    const std::shared_ptr<KisBackingBudgetController> &budget,
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

class KisCapturedReadView::Private : public KisPageCapturedRelease
{
public:
    using DefaultCache = std::map<quint64, std::shared_ptr<const KisCpuDefaultReadBuffer>, std::less<quint64>,
        KisMutationStorageAllocator<std::pair<const quint64, std::shared_ptr<const KisCpuDefaultReadBuffer>>>>;
    explicit Private(KisPageStore::Private *store)
        : owner(store), defaults(std::less<quint64>{}, DefaultCache::allocator_type(&store->backingBudget)) {}
    static boost::intrusive_ptr<Private> create(KisPageStore::Private *owner,
        size_t versions = 0, size_t removals = 0, size_t surfaces = 0, size_t references = 0)
    {
        const Layout layout(sizeof(Private), versions, removals, surfaces, references);
        void *raw = kisAllocateMutationStorage(&owner->backingBudget, layout.bytes, alignof(Private));
        Private *view;
        try { view = new (raw) Private(owner); }
        catch (...) { kisFreeMutationStorage(&owner->backingBudget, raw, 0, alignof(Private)); throw; }
        layout.bind(*view, raw);
        view->dispose = &Private::destroy;
        owner->lifetimeReferences.ref();
        return boost::intrusive_ptr<Private>(view);
    }
    static void destroy(KisPageCapturedRelease *record, KisBackingBudgetController *) noexcept
    {
        auto *view = static_cast<Private *>(record);
        auto *owner = view->owner;
        view->~Private();
        kisFreeMutationStorage(&owner->backingBudget, view, 0, alignof(Private));
        KisPageStore::PrivateReleaser::cleanup(owner);
    }
    static KisPageCapturedReleasePointer takeRelease(boost::intrusive_ptr<Private> &view)
    {
        Q_ASSERT(view && view->references.load(std::memory_order_relaxed) == 1);
        auto *record = view.detach();
        record->references.store(0, std::memory_order_relaxed);
        return {record, KisPageCapturedReleaseDeleter{&record->owner->backingBudget}};
    }
    static bool lessKey(const KisPageKey &a, const KisPageKey &b)
    {
        return std::tie(a.surface.value, a.page.row, a.page.column) <
               std::tie(b.surface.value, b.page.row, b.page.column);
    }
    bool surfaceState(KisSurfaceId surface, KisSurfaceEpochState *state) const
    {
        const auto &surfaces = stagedSurfaces;
        const auto found = std::lower_bound(surfaces.begin(), surfaces.end(), surface.value,
            [](const auto &candidate, quint64 id) { return candidate.surface.value < id; });
        if (found == surfaces.end() || !(found->surface == surface))
            return root.surfaceState(surface, state);
        *state = *found;
        return true;
    }

    bool resolve(const KisPageKey &key, KisPageVersion *version, KisCpuReadBinding *binding = nullptr) const
    {
        if (!key.isValid() || (exactVersion.isValid() && !(key == exactVersion.key)))
            return false;
        if (nativeRange) {
            if (stagedSurfaces.empty() || !(key.surface == stagedSurfaces.front().surface)
                || !range.contains(key.page.column, key.page.row)) return false;
            const size_t index = size_t(qint64(key.page.row) - range.top()) * size_t(range.width())
                + size_t(qint64(key.page.column) - range.left());
            const auto &references = nativeRefs;
            const auto &reference = references[index];
            *version = reference.version.isValid() ? owner->metadata.nativeReadVersion(reference, binding)
                : KisPageVersion{key, KisPageGeneration{1}, stagedSurfaces.front().defaultPixelRevision};
            return version->isValid();
        }
        const auto &versions = this->versions;
        const auto found = std::lower_bound(versions.begin(), versions.end(), key,
            [&](const KisPageVersion &candidate, const KisPageKey &value) { return lessKey(candidate.key, value); });
        if (found != versions.end() && found->key == key) {
            *version = *found;
            if (binding && !version->isDefaultPixel()) {
                // The original capture retained this exact slot in the same
                // ordinal; radix grouping changes links, never array order.
                const auto selected = owner->metadata.nativeReadVersion(nativeRefs[size_t(found - versions.begin())], binding);
                if (!(selected == *version)) return false;
            }
            return true;
        }
        if (!root.resolve(key, version))
            return false;
        if (overlayTransaction.isValid() && (version->isDefaultPixel()
            || std::binary_search(removedPages.begin(), removedPages.end(), key, lessKey))) {
            KisSurfaceEpochState surface;
            if (!surfaceState(key.surface, &surface))
                return false;
            *version = {key, KisPageGeneration{1}, surface.defaultPixelRevision};
        }
        if (binding && !version->isDefaultPixel())
            *binding = owner->metadata.selectCpuReadBinding(*version);
        return true;
    }
    std::shared_ptr<const KisCpuDefaultReadBuffer> defaultReadBuffer(const KisPageVersion &version) const
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

    // This original record outlives all views/guards and, if needed, its
    // asynchronous release. No separate control block or release allocation.
    KisPageStore::Private *owner;
    std::atomic<quint32> references{0};
    KisImageEpochRootSnapshot root;
    KisImageEpochSnapshotToken retention;
    KisPageVersion exactVersion;
    KisPageTransactionId overlayTransaction;
    QRect range;
    KisImageEpochId rangeEpoch;
    mutable QMutex defaultMutex;
    mutable DefaultCache defaults;
};

void intrusive_ptr_add_ref(KisCapturedReadView::Private *view) noexcept
{ view->references.fetch_add(1, std::memory_order_relaxed); }
void intrusive_ptr_release(KisCapturedReadView::Private *view) noexcept
{
    if (view->references.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    view->defaults.clear();
    view->root = {};
    if (view->token.isValid() || view->nativeRange) {
        view->owner->readCoordinator.releaseCapturedView(
            {view, KisPageCapturedReleaseDeleter{&view->owner->backingBudget}});
    } else {
        KisCapturedReadView::Private::destroy(view, nullptr);
    }
}

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
    return d ? (d->nativeRange ? d->rangeEpoch : d->root.epoch()) : KisImageEpochId{};
}

KisImageEpochSnapshot KisPageStore::snapshotForClone(const KisCapturedReadView &view, QString *error) const
try
{
    KisPageStoreDiagnosticTimer phase(this, KisPageStoreDiagnosticPhase::CopyPrepare);
    if (!view.d || view.d->owner != d.data() || view.d->exactVersion.isValid() || !view.d->range.isEmpty()) {
        KisPageStoreDetail::setError(error, QStringLiteral("clone requires this store's complete captured view"));
        return {};
    }
    phase.next(KisPageStoreDiagnosticPhase::ManifestExport, quint64(view.d->root.pageCount()));
    const auto &delta = *view.d;
    auto result = view.d->root.snapshotWithChanges(delta.versions.data(), delta.versions.size(),
        delta.removedPages.data(), delta.removedPages.size(), delta.stagedSurfaces.data(), delta.stagedSurfaces.size());
    KisPageStoreDetail::setError(error, {});
    return result;
}
catch (const std::bad_alloc &)
{
    KisPageStoreDetail::setError(error, QStringLiteral("clone snapshot storage is unavailable"));
    return {};
}

bool KisCapturedReadView::resolvePageVersion(const KisPageKey &key, KisPageVersion *version) const
{
    if (version)
        *version = {};
    if (!d || !version)
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
                                                          bool waitForLocalGate, KisCpuReadBinding *selection) const
{
    KisCpuReadGuard guard;
    if (status)
        *status = KisCpuResidentReadStatus::InvalidView;
    if (!d)
        return guard;
    if (status)
        *status = KisCpuResidentReadStatus::InvalidIdentity;
    KisPageVersion version;
    KisCpuReadBinding link;
    if (selection && selection->replica.version.generation.isValid()) {
        // Internal windows only contain selections made from this exact cut.
        link = std::move(*selection);
        version = link.replica.version;
        if (!(version.key == key)) return guard;
    } else if (!d->resolve(key, &version, &link)) return guard;
    if (version.isDefaultPixel()) {
        auto buffer = d->defaultReadBuffer(version);
        if (status)
            *status = buffer ? KisCpuResidentReadStatus::Ready : KisCpuResidentReadStatus::ResourceUnavailable;
        if (!buffer)
            return guard;
        if (!selection) guard.m_scope = d;
        guard.m_data = buffer->data;
        guard.m_defaultBuffer = std::move(buffer);
        guard.m_version = version;
        guard.m_rowStride = guard.m_defaultBuffer->rowStride;
        guard.m_byteSize = guard.m_defaultBuffer->byteSize;
        return guard;
    }
    auto observed = version.defaultPixelRevision ? KisCpuResidentReadStatus::VirtualDefault
                                                 : KisCpuResidentReadStatus::BindingUnavailable;
    // Resolution already selected this exact source under its original shard
    // gate, including a cache miss. Continue only the unlocked provider stage.
    // The captured root/slot protection survives discovery and physical pin.
    if (!link)
        link = KisPageReadCoordinator::resolveSelectedCpuReadBinding(
            d->owner->metadata, d->owner->owner, link, &observed);
    const void *data = nullptr;
    constexpr int maximumAttempts = 3; // initial pin plus two immediate rediscoveries
    for (int attempt = 0; attempt < maximumAttempts; ++attempt) {
        if (link && !(link.replica.version == version)) {
            observed = KisCpuResidentReadStatus::InvalidIdentity;
            break;
        }
        if (link.binding)
            data = link.binding->acquireRead(link.replica.allocationIdentity(), true, &observed, waitForLocalGate);
        if (data || (observed != KisCpuResidentReadStatus::InvalidIdentity
                     && observed != KisCpuResidentReadStatus::Retired)
            || attempt + 1 == maximumAttempts)
            break;
        link = KisPageReadCoordinator::discoverCpuReadBinding(
            d->owner->metadata, d->owner->owner, version, link, &observed);
    }
    if (status) *status = observed;
    if (!data)
        return guard;
    if (!selection) guard.m_scope = d;
    guard.m_binding = std::move(link.binding);
    guard.m_data = data;
    guard.m_version = link.replica.version;
    guard.m_pageExtent = link.replica.layout.pageExtent;
    guard.m_rowStride = link.replica.layout.rowStride;
    guard.m_byteSize = link.replica.layout.byteSize;
    return guard;
}

bool KisCapturedReadView::visitResidentPage(const KisPageKey &key, KisCpuReadBinding &selection,
    const void *context, void (*visit)(const void *, const void *, quint32), KisCpuResidentReadStatus *status) const
{
    auto guard = readResidentPageImpl(key, status, true, &selection);
    if (!guard.m_data) return false;
    visit(context, guard.m_data, guard.m_rowStride);
    return true;
}

void KisCapturedReadView::selectReadWindow(size_t first, size_t count,
    quint32 *indices, KisCpuReadBinding *bindings) const
{
    Q_ASSERT(d && d->nativeRange && count <= 16
        && first + count <= d->nativeRefs.size());
    d->owner->metadata.nativeReadBindings(d->nativeRefs.data(), indices, first, count, bindings);
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
    m_binding.reset();
    m_defaultBuffer.reset();
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
    : KisPageStore(KisMutationStorageAllocator<KisPageStore>{})
{
}

KisPageStore::KisPageStore(const KisMutationStorageAllocator<KisPageStore> &storage)
{
    Private::Storage allocator(storage);
    auto *raw = allocator.allocate(1);
    try { std::allocator_traits<Private::Storage>::construct(allocator, raw, allocator); }
    catch (...) { allocator.deallocate(raw, 1); throw; }
    d.reset(raw);
}

KisPageStore::StoragePointer KisPageStore::prepareStorage(
    const std::shared_ptr<KisBackingBudgetController> &parent, QString *error)
{
    StoragePointer result(nullptr, &KisPageStore::destroyStorage);
    // The internal controller must still accept the original shared/limits
    // setup. This cold child only pays fixed root storage; its retained
    // accounting keeps the parent and registration until the actual last free.
    KisBackingBudgetController bootstrap;
    if (!bootstrap.configureSharedNonPayloadBudget(parent, error)) return result;
    try {
        auto allocator = KisMutationStorageAllocator<KisPageStore>::retained(&bootstrap);
        auto *raw = allocator.allocate(1);
        try { ::new (raw) KisPageStore(allocator); }
        catch (...) { allocator.deallocate(raw, 1); throw; }
        result.reset(raw);
    } catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore root storage budget was refused"));
        return result;
    }
    KisPageStoreDetail::setError(error, {});
    return result;
}

void KisPageStore::destroyStorage(KisPageStore *store) noexcept
{
    if (!store) return;
    KisMutationStorageAllocator<KisPageStore> allocator(store->d->storage);
    std::destroy_at(store);
    allocator.deallocate(store, 1);
}

KisPageStore::~KisPageStore()
{
    d->backingBudget.stopMutationStorageReuse();
    // Outstanding capabilities keep Private alive, but recurring maintenance
    // must not sustain it after the facade is gone. Stop admission without
    // waiting on providers or invalidating the capabilities' original bytes.
    d->readCoordinator.stopAutomaticWakeups();
    d->retirementQueue.stopAutomaticWakeups();
    d->historyCollector.stopAutomaticWakeups();
    {
        QMutexLocker lock(&d->mutex);
        d->facadeGone = true;
        d->publicationCoordinator.drainIdleCapacityLocked();
    }
    KisPageMutationSession::Private::retryOrphans(d.data(), {}, true);
}

void KisPageStore::PrivateReleaser::cleanup(Private *owner, bool deferDestruction)
{
    if (owner && !owner->lifetimeReferences.deref()) {
        if (!owner->terminalCleanup || (!deferDestruction
            && (!owner->backgroundReclamation || kisOnPageStoreReclamationThread())))
            Private::destroy(owner);
        else {
            auto cleanup = std::move(owner->terminalCleanup);
            if (kisEnqueuePageStoreReclamation(cleanup.get())) cleanup.release();
            else Private::destroy(owner);
        }
    }
}

KisCapturedReadView KisPageStore::captureReadRange(KisSurfaceId surface, const QRect &pages,
    const KisPageReadView &selector, QString *error, KisCpuReadBinding *firstBindings, size_t bindingCount)
try
{
    if (!surface.isValid() || pages.isEmpty() || !selector.isValidFor({surface, {0, 0}})
        || (selector.kind != KisPageReadViewKind::CurrentCommittedEpoch && selector.kind != KisPageReadViewKind::TransactionOverlay)
        || quint64(pages.width()) * quint64(pages.height()) > std::numeric_limits<quint32>::max()) {
        KisPageStoreDetail::setError(error, QStringLiteral("native read range is invalid"));
        return {};
    }
    KisCapturedReadView result;
    result.d = KisCapturedReadView::Private::create(d.data(), 0, 0, 1,
        size_t(pages.width()) * size_t(pages.height()));
    auto &release = *result.d;
    release.nativeRange = true;
    release.nativeRefs.resize(size_t(pages.width()) * size_t(pages.height()));
    release.stagedSurfaces.resize(1);
    const auto keyAt = [&](quint32 i) {
        return KisPageKey{surface, {qint32(qint64(pages.left()) + qint64(i % quint32(pages.width()))),
                                   qint32(qint64(pages.top()) + qint64(i / quint32(pages.width())))}};
    };
    for (size_t i = 0; i < release.nativeRefs.size(); ++i) {
        release.nativeRefs[i].shard = quint32(d->metadata.shardFor(keyAt(quint32(i))));
    }
    release.groupNativeRefs(d->metadata.shardCount());
    bindingCount = std::min({bindingCount, size_t(16), release.nativeRefs.size()});
    QMutexLocker lock(&d->mutex);
    KisPagePublicationCoordinator::NativeReadSelection selection;
    if (!d->operational || d->closing
        || !d->publicationCoordinator.prepareNativeReadSelectionLocked(surface, selector, &selection)) {
        KisPageStoreDetail::setError(error, QStringLiteral("native read surface is unavailable"));
        return {};
    }
    release.stagedSurfaces.front() = selection.surface;
    result.d->range = pages;
    result.d->rangeEpoch = selection.root.epoch();
    if (selector.kind == KisPageReadViewKind::TransactionOverlay) result.d->overlayTransaction = selector.transaction;
    auto select = [&](quint32 ordinal, KisPageVersion *version, KisVersionSlotId *slot) {
        return selection.resolve(keyAt(ordinal), version, slot);
    };
    size_t next = release.next;
    while (release.retained < release.nativeRefs.size()) {
        const auto retained = d->metadata.retainNativeReads(release.nativeRefs.data(), next,
            release.nativeRefs.size() - release.retained, &select,
            [](void *context, quint32 ordinal, KisPageVersion *version, KisVersionSlotId *slot) {
                return (*static_cast<decltype(select) *>(context))(ordinal, version, slot);
            }, firstBindings, bindingCount);
        release.retained += retained;
        if (!retained) {
            d->readCoordinator.releaseCapturedView(KisCapturedReadView::Private::takeRelease(result.d), &lock);
            KisPageStoreDetail::setError(error, QStringLiteral("native read range selection was refused"));
            return {};
        }
    }
    release.capturedScope = true;
    d->readCoordinator.noteCapturedViewCreatedLocked(true);
    KisPageStoreDetail::setError(error, {});
    return result;
}
catch (const std::bad_alloc &)
{
    KisPageStoreDetail::setError(error, QStringLiteral("native read range storage was refused"));
    return {};
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
        if (selector.kind != KisPageReadViewKind::TransactionOverlay)
            result.d = KisCapturedReadView::Private::create(d);
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
        KisPageCapturedRelease empty;
        auto &candidate = result.d ? static_cast<KisPageCapturedRelease &>(*result.d) : empty;
        const bool captured = d->publicationCoordinator.captureDeltaLocked(selector.transaction, candidate,
                                                                           &versions, &removals, &surfaces);
        if (result.d && captured && candidate.nativeRefs.capacity() >= candidate.versions.size()) break;
        versions = qMax(versions, candidate.versions.size());
        if (versions > KisPageNativeReadRef::End) {
            KisPageStoreDetail::setError(error, QStringLiteral("read scope has too many exact versions"));
            return {};
        }
        ++d->activeProviderCalls;
        locker.unlock();
        try {
            // Nothing is protected yet. Dispose the obsolete whole capacity
            // before preparing the exact next cut, then reselect under the
            // owner gate. This preserves the original late-growth retry.
            result.d.reset();
            result.d = KisCapturedReadView::Private::create(d, versions, removals, surfaces, versions);
        } catch (const std::bad_alloc &) {
            locker.relock(); --d->activeProviderCalls;
            KisPageStoreDetail::setError(error, QStringLiteral("read scope version storage is unavailable"));
            return {};
        }
        locker.relock(); --d->activeProviderCalls;
    }
    auto &release = *result.d;
    if (release.versions.size() > KisPageNativeReadRef::End) {
        KisPageStoreDetail::setError(error, QStringLiteral("read scope has too many exact versions"));
        return {};
    }
    if (selector.kind == KisPageReadViewKind::TransactionOverlay) {
        std::sort(release.versions.begin(), release.versions.end(), [](const auto &a, const auto &b) {
            return KisCapturedReadView::Private::lessKey(a.key, b.key);
        });
        std::sort(release.removedPages.begin(), release.removedPages.end(), KisCapturedReadView::Private::lessKey);
        std::sort(release.stagedSurfaces.begin(), release.stagedSurfaces.end(),
            [](const auto &a, const auto &b) { return a.surface.value < b.surface.value; });
        result.d->overlayTransaction = selector.transaction;
    }
    release.nativeRefs.resize(release.versions.size());
    for (size_t i = 0; i < release.versions.size(); ++i) {
        release.nativeRefs[i].shard = quint32(d->metadata.shardFor(release.versions[i].key));
    }
    release.groupNativeRefs(d->metadata.shardCount());
    const auto retained = d->epochs.retainSnapshot(root.epoch());
    if (!retained.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("read scope retention could not be allocated"));
        return {};
    }
    release.token = retained.token;
    QString failure;
    // Full and range captures retain the same exact slots under the same
    // publication cut. The full view additionally retains its immutable root
    // and sparse key map for clone/export; neither needs token-per-page state.
    auto select = [&](quint32 ordinal, KisPageVersion *version) {
        *version = release.versions[ordinal];
        return true;
    };
    size_t next = release.next;
    while (release.retained < release.nativeRefs.size()) {
        const auto count = d->metadata.retainNativeReads(release.nativeRefs.data(), next,
            release.nativeRefs.size() - release.retained, &select,
            [](void *context, quint32 ordinal, KisPageVersion *version, KisVersionSlotId *) {
                return (*static_cast<decltype(select) *>(context))(ordinal, version);
            });
        release.retained += count;
        if (!count) {
            failure = QStringLiteral("read scope exact protection was refused");
            break;
        }
    }
    if (!failure.isEmpty()) {
        d->readCoordinator.releaseCapturedView(KisCapturedReadView::Private::takeRelease(result.d), &locker);
        KisPageStoreDetail::setError(error, failure);
        return {};
    }
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
    if (auto *storage = d->backingBudget.takeMutationStorage()) storage->dispose(storage);
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
    d->publicationCoordinator.drainIdleCapacityLocked();
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
    d->historyCollector.collectUnreachableLocked();
    d->readCoordinator.retryCancelledRequestsLocked(locker, cleanup, true);
    d->readCoordinator.retryCapturedReleasesLocked(locker, true);
    d->readCoordinator.retryReleasedReadsLocked(locker, cleanup, {}, nullptr, true);
    d->readCoordinator.acknowledgeCompletedLastUsesLocked(locker, cleanup);
    cleanup.finishUnlocked(locker);
    KisPageStoreSessionStats stats = d->stats();
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
                 QStringLiteral("PageStore session still owns capabilities"));
        return false;
    }

    KisPageRetirementRecords liveShutdownReplicas;
    if (d->pendingShutdownReplicas.empty() && d->operational) {
        d->epochs.collectUnretainedRoots();
        liveShutdownReplicas = d->owner.takeShutdownRetirementRecords();
    }

    KisPageRetirementRecords shutdownReplicas =
        std::move(d->pendingShutdownReplicas);
    // Queued and live records can each retain their original class. The
    // retirement owner resumes admission after this close releases capacity.
    auto queuedShutdown = d->retirementQueue.takeForClose();
    shutdownReplicas.splice(shutdownReplicas.end(), queuedShutdown);
    shutdownReplicas.splice(shutdownReplicas.end(), liveShutdownReplicas);
    d->closing = true;
    d->operational = false;
    d->writeAdmissionChanged.wakeAll();
    const qsizetype shutdownCount = qsizetype(shutdownReplicas.size());
    d->activeProviderCalls += shutdownCount;
    locker.unlock();
    d->backingBudget.stopMutationStorageReuse();
    d->retirementQueue.retireForClose(shutdownReplicas);
    locker.relock();
    d->activeProviderCalls -= shutdownCount;
    d->pendingShutdownReplicas = std::move(shutdownReplicas);
    d->closing = false;
    if (!d->pendingShutdownReplicas.empty()) {
        d->closed = false;
        d->closeFailure = QStringLiteral("PageStore shutdown retirement is pending");
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
                             const std::shared_ptr<KisCompletionRegistry> &completions,
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
    if (!d->backingBudget.ensureProcessStorageBudget(error)) return false;
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
                [owner = d.data()] { Private::destroy(owner); }, &d->backingBudget);
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
    const bool cpuProofsBound = d->owner.bindCpuProofs(d->metadata, ready);
    Q_ASSERT(cpuProofsBound);
    if (!cpuProofsBound) qFatal("PageStore CPU proof metadata owner changed");
    const bool epochInstalled = d->epochs.installInitialization(std::move(epoch));
    Q_ASSERT(epochInstalled);
    Q_UNUSED(epochInstalled);
    d->completions = completions;
    d->readyHostCompletion = ready;
    d->configurationSourceOwner.reset();
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

bool KisPageStore::registerReplicaProvider(const std::shared_ptr<KisPageReplicaProvider> &provider)
{
    QMutexLocker locker(&d->mutex);
    const bool registered = d->completions && !d->operational && !d->closed && !d->closing && provider
        && provider->capabilities().synchronousOperations && d->owner.registerProvider(provider);
    if (registered)
        d->backgroundReclamation = d->backgroundReclamation && provider->capabilities().backgroundRetirement;
    return registered;
}

bool KisPageStore::registerTransferBridge(const std::shared_ptr<KisPageReplicaTransferBridge> &bridge)
{
    QMutexLocker locker(&d->mutex);
    if (!d->completions || d->operational || d->closed
        || !d->writeCoordinator.registerTransferBridge(bridge)) {
        return false;
    }
    return true;
}

bool KisPageStore::registerExactGenerationArchive(const std::shared_ptr<KisExactGenerationArchive> &archive)
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
                                    QString *error) try
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
    std::vector<KisReplicaHandle, KisMutationStorageAllocator<KisReplicaHandle>> replicas{
        KisMutationStorageAllocator<KisReplicaHandle>(&d->backingBudget)};
    std::vector<std::shared_ptr<KisPageReplicaProvider>, KisMutationStorageAllocator<std::shared_ptr<KisPageReplicaProvider>>> providers{
        KisMutationStorageAllocator<std::shared_ptr<KisPageReplicaProvider>>(&d->backingBudget)};
    replicas.reserve(size_t(readyAlternates.size()) + 1);
    providers.reserve(size_t(readyAlternates.size()) + 1);
    replicas.push_back(authority);
    for (const auto &replica : readyAlternates) {
        if (!replica.isValid() || !(replica.version == version) || !replica.layout.matches(descriptor)
            || std::any_of(replicas.cbegin(), replicas.cend(), [&](const auto &existing) {
                return existing.physicalSlotIdentity() == replica.physicalSlotIdentity();
            })) {
            KisPageStoreDetail::setError(error, QStringLiteral("initial exact replica is invalid or duplicated"));
            return false;
        }
        replicas.push_back(replica);
    }
    for (const auto &replica : std::as_const(replicas)) {
        auto provider = d->owner.provider(replica.provider, replica.providerEpoch);
        if (!provider) {
            KisPageStoreDetail::setError(error, QStringLiteral("initial replica provider is unavailable"));
            return false;
        }
        providers.push_back(std::move(provider));
    }
    ++d->activeProviderCalls;
    bool valid = true;
    {
        locker.unlock();
        const auto done = qScopeGuard([&] { locker.relock(); --d->activeProviderCalls; });
        for (qsizetype i = 0; i < replicas.size(); ++i)
            valid = providers[i]->validate(replicas[i], descriptor) && valid;
    }
    KisPageMetadataCoordinator::VersionInfo existingPage;
    if (!valid || d->operational || d->closed || d->metadata.versionSnapshot(version, &existingPage)) {
        KisPageStoreDetail::setError(error, QStringLiteral("initial page replicas failed provider validation"));
        return false;
    }
    decltype(d->publicationCoordinator.prepareDescriptorLocked(version, descriptor)) descriptors;
    const auto discardDescriptors = qScopeGuard([&] {
        d->publicationCoordinator.discardDescriptorAdditionsLocked(&descriptors, locker);
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
                if (d->owner.backingClass(replicas[i]) == KisBackingBudgetClass::Count) {
                    terminals[size_t(i)] = kisPreparePageRetirementRecord(&d->backingBudget);
                    terminals[size_t(i)]->preparePhysicalStorage();
                }
        } catch (const std::bad_alloc &) {
            KisPageStoreDetail::setError(error, QStringLiteral("initial backing retirement storage budget was refused"));
            return false;
        }
    }
    if (d->operational || d->closed || d->metadata.versionSnapshot(version, &existingPage)) {
        KisPageStoreDetail::setError(error, QStringLiteral("initial page changed during retirement storage preparation"));
        return false;
    }
    decltype(replicas) registered(replicas.get_allocator());
    registered.reserve(replicas.size());
    auto undoRegistration = qScopeGuard([&] {
        // Keep partial registration invisible to another initial adoption.
        // Reuse the admitted array for node disposal after rollback completes.
        for (const auto &replica : registered) {
            const auto found = std::find(replicas.cbegin(), replicas.cend(), replica);
            Q_ASSERT(found != replicas.cend());
            auto &terminal = terminals[size_t(found - replicas.cbegin())];
            Q_ASSERT(!terminal);
            terminal = d->owner.takeRetirementRecord(replica);
            const bool withdrawn = terminal && d->owner.rollbackBackingRegistration(*terminal);
            Q_ASSERT(withdrawn);
            Q_UNUSED(withdrawn);
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
        registered.push_back(replica);
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
catch (const std::bad_alloc &)
{
    KisPageStoreDetail::setError(error, QStringLiteral("initial page registration storage preparation was refused"));
    return false;
}

bool KisPageStore::adoptInitialPageBytes(const KisPageVersion &version,
                                         const KisPageAllocationDescriptor &descriptor,
                                         const QByteArray &canonicalBytes,
                                         QString *error)
{
    const KisPageAccessRequirement cpuAccess{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
    std::shared_ptr<KisPageReplicaProvider> provider;
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
    const auto finishProviderCall = qScopeGuard([&] {
        QMutexLocker locker(&d->mutex);
        --d->activeProviderCalls;
    });

    KisReplicaOperation allocation;
    try {
        allocation = provider->requestReplica(allocationOperation,
                                              version,
                                              descriptor,
                                              KisPageAccessDomain::CpuRam,
                                              KisPageAccessMode::Write,
                                              KisPagePriority::Normal);
    } catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("initial canonical page provider storage was refused"));
        return false;
    }
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
        KisReplicaAccess access = [&]() -> KisReplicaAccess {
            try {
                return provider->resolveAccess(
                    lease, accessOperation, allocation.replica, cpuAccess, KisPageAccessMode::Write);
            } catch (const std::bad_alloc &) {
                failure = QStringLiteral("initial canonical page access storage was refused");
                return {};
            }
        }();
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
    decltype(d->publicationCoordinator.prepareDescriptorLocked({}, {})) defaultDescriptors;
    QMutexLocker locker(&d->mutex);
    d->readCoordinator.retryCancelledRequestsLocked(locker, cleanup);
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
    while (!hasPage || page.needsDefaultMaterialization(version)) {
        if (!defaultDescriptors.empty()) {
            locker.unlock();
            defaultDescriptors = {};
            locker.relock();
        }
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
        if (!d->ensureDefaultPageLocked(version, publishedEpoch, surface, access, priority,
                                       &locker, defaultDescriptors, &failure)) {
            return fail(failure.isEmpty()
                ? QStringLiteral("PageStore default page materialization failed") : failure);
        }
        try {
            hasPage = snapshotVersion();
            if (!hasPage) return fail(QStringLiteral("PageStore default metadata is unavailable"));
        } catch (const std::bad_alloc &) { return fail(QStringLiteral("exact replica candidate storage was refused")); }
    }
    // Keep the exact version captured above, including across provider work.
    const KisPageMetadataCoordinator::ReplicaCandidate *replica = nullptr;
    std::shared_ptr<KisPageReplicaProvider> provider;
    if (page.version.isValid()) {
        const auto authority = std::find_if(candidates.begin(), candidates.end(),
            [&](const auto &candidate) { return candidate.replica == page.authority; });
        const auto *authorityReplica = authority == candidates.end() ? nullptr : &*authority;
        if (authorityReplica && authorityReplica->validity == KisReplicaValidity::Valid
            && authorityReplica->replica.domain == access.domain) {
            const std::shared_ptr<KisPageReplicaProvider> authorityProvider =
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
            const std::shared_ptr<KisPageReplicaProvider> candidateProvider =
                d->owner.provider(candidate.replica.provider, candidate.replica.providerEpoch);
            if (!candidateProvider || !candidateProvider->capabilities().supports(access)) {
                continue;
            }
            if (replica) {
                replica = nullptr;
                provider.reset();
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
    KisPageWriteSelection selection;
    auto &transition = selection.write;
    if (!d->publicationCoordinator.selectWriteBaseLocked(transaction, writeIntent,
                                                    &selection, &request.error)) {
        return fail();
    }
    const auto &descriptor = selection.descriptor();
    transition.operation = d->owner.nextOperationId();
    transition.writer = d->owner.nextWriterToken();
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
    bool reused;
    try {
        auto &target = d->writeRequests.find(requestId.value)->second.replica;
        reused = d->writeCoordinator.tryAcquireReusedTargetLocked(
            transaction, writeIntent, provider, access, selection,
            d->publicationCoordinator,
            d->publicationCoordinator.takeIdleCapacityLocked(transition.version.key), locker, target, writable, diagnostic);
    } catch (const std::bad_alloc &) {
        // The common plan restores this gate and destroys prepared candidates
        // outside it. All fallible preparation precedes physical handoff.
        d->writeRequests.erase(requestId.value);
        return fail();
    }
    if (reused) {
        // The preallocated request is the sole generic terminal owner. The
        // logical writer/admission survive releasing the short native pin;
        // resolve() acquires the ordinary generic provider access later.
        auto &prepared = d->writeRequests.find(requestId.value)->second;
        prepared.source = transition.source;
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
        d->publicationCoordinator.discardDescriptorAdditionsLocked(&descriptors, locker);
    });
    try { descriptors = d->publicationCoordinator.prepareDescriptorLocked(writeVersion, descriptor, baseVersion); }
    catch (const std::bad_alloc &) {
        d->writeRequests.erase(requestId.value);
        return fail(QStringLiteral("write descriptor storage budget was refused"));
    }
    KisPageBackingPreparation backing;
    const auto discardBacking = qScopeGuard([&] {
        if (!backing.retirement) return;
        locker.unlock(); backing.retirement.reset(); locker.relock();
    });
    ++d->activeProviderCalls;
    const auto providerPreparation = qScopeGuard([&] {
        if (!locker.isLocked()) locker.relock();
        --d->activeProviderCalls;
    });
    // Preparing requests are not cancellable; their original activity blocks
    // abort/close. Only this call consumes the local backing reservation.
    locker.unlock();
    backing = d->writeCoordinator.reserveBacking(descriptor, access.domain,
        KisBackingBudgetClass::ActivePending, &request.error, writeVersion);
    if (!backing.reservation.isValid()) {
        locker.relock();
        d->writeRequests.erase(requestId.value);
        return fail();
    }

    bool nativeWriteCopy = false;

    // Provider allocation is deliberately outside the PageStore lock. The
    // provisional request keeps close/abort from crossing this operation; the
    // metadata transition below revalidates writer/generation ownership.
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
    const bool ownedReplica = ownsPreparedReplica(allocation.replica, writeVersion, *provider, &pending.source);
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
    KisPageMetadataTransitionResult stateResult = d->writeCoordinator.acquireLeaseWrite(transition, allocation.replica);
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

    stateResult = d->metadata.applyOwner(key, transition.leaseTransition(allocation.replica, KisPageTransitionKind::PrepareWrite));
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
    const std::shared_ptr<KisPageReplicaProvider> provider =
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
        active = std::allocate_shared<ActiveWriteRecord>(
            KisMutationStorageAllocator<ActiveWriteRecord>::retained(&d->backingBudget), provider, pending);
        ActiveWriteMap prepared(d->activeWrites.get_allocator());
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

    std::shared_ptr<KisExactGenerationArchive> archive;
    std::shared_ptr<KisCompletionRegistry> completions;
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
    PendingArchiveMap prepared(d->pendingArchives.get_allocator());
    try {
        prepared.emplace(operation.value, PendingArchiveRecord{false, lease.version(), {}, archive});
    } catch (const std::bad_alloc &) {
        {
            QMutexLocker locker(&d->mutex);
            --d->activeProviderCalls;
        }
        release(std::move(lease));
        KisPageStoreDetail::setError(error, QStringLiteral("archive tracking storage preparation was refused"));
        return {};
    }
    KisPageArchiveOperation archived;
    try {
        archived = archive->storeExact(write, priority);
    } catch (const std::bad_alloc &) {
        {
            QMutexLocker locker(&d->mutex);
            --d->activeProviderCalls;
        }
        release(std::move(lease));
        KisPageStoreDetail::setError(error, QStringLiteral("exact-generation archive storage was refused"));
        return {};
    }

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
            auto &record = prepared.begin()->second;
            record.completion = archived.completion;
            const bool newPending = completions->status(archived.completion) != KisCompletionStatus::Unknown
                && d->pendingArchives.find(operation.value) == d->pendingArchives.end();
            if (newPending) {
                d->pendingArchives.insert(prepared.extract(prepared.begin()));
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

QVector<KisPageArchiveCompletionEvent> KisPageStore::pollArchiveCompletions(qsizetype maximumCount) try
{
    QVector<KisPageArchiveCompletionEvent> events;
    if (maximumCount <= 0)
        return events;

    struct Candidate {
        KisPageOperationId operation;
        PendingArchiveRecord record;
        KisVerifiedCompletion terminal;
    };
    std::vector<Candidate, KisMutationStorageAllocator<Candidate>> candidates{
        KisMutationStorageAllocator<Candidate>(&d->backingBudget)};
    {
        QMutexLocker locker(&d->mutex);
        if (!d->completions)
            return events;
        const auto capacity = std::min(size_t(maximumCount), d->pendingArchives.size());
        candidates.reserve(capacity);
        events.reserve(qsizetype(capacity));
        for (auto it = d->pendingArchives.begin(); it != d->pendingArchives.end() && candidates.size() < maximumCount;
             ++it) {
            if (it->second.busy)
                continue;
            const KisVerifiedCompletion terminal = d->completions->verifyTerminal(it->second.completion);
            if (!terminal.isValid())
                continue;
            it->second.busy = true;
            candidates.push_back({KisPageOperationId{it->first}, it->second, terminal});
        }
    }
    const auto releaseBusy = qScopeGuard([&] {
        QMutexLocker locker(&d->mutex);
        for (const Candidate &candidate : std::as_const(candidates)) {
            auto it = d->pendingArchives.find(candidate.operation.value);
            if (it != d->pendingArchives.end() && it->second.busy
                && it->second.completion == candidate.record.completion
                && it->second.version == candidate.record.version) {
                it->second.busy = false;
            }
        }
    });

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
        if (it == d->pendingArchives.end() || !(it->second.completion == candidate.record.completion)
            || !(it->second.version == candidate.record.version)) {
            continue;
        }
        d->pendingArchives.erase(it);
        events.append(event);
    }
    return events;
}
catch (const std::bad_alloc &)
{
    return {};
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
            if (it->second.completion == completion) {
                selected = it;
                break;
            }
        }
        if (selected == d->pendingArchives.end() || selected->second.busy
            || !selected->second.archive) {
            return false;
        }
        operation = KisPageOperationId{selected->first};
        selected->second.busy = true;
        record = selected->second;
        ++d->activeProviderCalls;
    }

    const auto releaseBusy = qScopeGuard([&] {
        QMutexLocker locker(&d->mutex);
        --d->activeProviderCalls;
        auto it = d->pendingArchives.find(operation.value);
        if (it != d->pendingArchives.end() && it->second.completion == record.completion) {
            it->second.busy = false;
        }
    });
    try {
        return record.archive->cancelStoreExact(operation);
    } catch (const std::bad_alloc &) {
        return false;
    }
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
        success = d->writeCoordinator.publishLeaseWrite(
                request.transition(KisPageTransitionKind::BeginPublish)).accepted
            && d->owner.sealPreparedPage(d->metadata, request.version, request.transaction,
                                         descriptor, completion, &proof);
        lock.relock();
    }
    KisPagePublicationCoordinator::KisPreparedPagePublication overlay;
    if (success) {
        const auto transaction = d->epochs.activeTransaction(request.transaction);
        if (transaction.isValid()) {
            const KisPagePublicationCoordinator::OverlayChange change{request.version.key, proof, false};
            overlay = d->publicationCoordinator.preparePagePublicationLocked(
                transaction, &change, 1, nullptr);
        }
        success = overlay.isValid();
    }
    if (success) {
        lock.unlock();
        success = overlay.prepare(nullptr);
        lock.relock();
    }
    if (success) {
        success = overlay.prepareSurfaceLocked(nullptr) && overlay.tryAcceptLocked(lock, nullptr);
        if (success) proof = {};
    }
    lock.unlock();
    overlay.finishUnlocked(lock);
    lock.relock();
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

QVector<KisLogicalPageId> KisPageStore::allocatedPages(
    KisSurfaceId surface, const KisPageTransaction &transaction) const
{
    // Structural metadata only: the immutable root keeps its nodes alive;
    // enumerating keys does not pin pixels or issue a retained epoch token.
    KisImageEpochRootSnapshot root;
    const auto storage = KisMutationStorageAllocator<KisPageKey>::retained(&d->backingBudget);
    KisPageKeyStorage additions(storage), removals(storage);
    QMutexLocker locker(&d->mutex);
    if (!d->publicationCoordinator.captureAllocatedPageInputsLocked(surface, transaction, &root, &additions, &removals))
        return {};
    locker.unlock();
    return root.allocatedPages(surface, additions, removals);
}

KisPreparedPageSet KisPageStore::preparedPages(const KisPageTransaction &transaction) const
{
    QMutexLocker locker(&d->mutex);
    return d->publicationCoordinator.preparedPagesLocked(transaction);
}

bool KisPageStore::transactionDirtyExtent(const KisPageTransaction &transaction, KisSurfaceId surface, QRect *extent) const
{
    QMutexLocker lock(&d->mutex);
    return d->publicationCoordinator.transactionDirtyExtentLocked(transaction, surface, extent);
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
bool KisPageStore::commitPreparedTransaction(
    const KisPageTransaction &transaction, KisRetainedImageEpochSnapshot *retainedAfter)
{
    if (retainedAfter) *retainedAfter = {};
    if (transaction.id.isValid())
        KisPageMutationSession::Private::retryOrphans(d.data(), transaction.id);
    // Only empty commits need abort cleanup; its storage still dies after
    // the owner locker on every exit, including refusal.
    std::optional<KisPageReadCleanup> cleanup;
    QMutexLocker locker(&d->mutex);
    if (!d->publicationCoordinator.hasPreparedDeltaLocked(transaction.id)) {
        cleanup.emplace(d->readCoordinator);
        return d->publicationCoordinator.abortLocked(transaction, locker, *cleanup);
    }
    return d->publicationCoordinator.commitOwnedLocked(transaction, retainedAfter, this, locker).isValid();
}

KisImageEpochCommitTicket KisPageStore::restoreRetainedEpoch(const KisRetainedImageEpochSnapshot &retained)
{
    return restoreRetainedEpoch(retained, {}, {});
}

KisImageEpochCommitTicket KisPageStore::restoreRetainedEpoch(const KisRetainedImageEpochSnapshot &retained,
    const std::function<bool(const KisImageEpochRootSnapshot &)> &prepareAdapter,
    const std::function<void(const KisPageVersion *, qsizetype)> &completeAdapter)
{
    QMutexLocker locker(&d->mutex);
    return d->publicationCoordinator.restoreRetainedEpochLocked(retained, this, locker, prepareAdapter, completeAdapter);
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
    return d->readCoordinator.releaseSnapshot(token);
}
KisMutationStorageAllocator<char> KisPageStore::storageAllocator() const
{
    return KisMutationStorageAllocator<char>::retained(&d->backingBudget);
}

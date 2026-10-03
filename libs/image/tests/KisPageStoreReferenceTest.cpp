/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryFile>
#include <QTest>

#include <atomic>
#include <ctime>
#include <functional>
#include <memory>
#include <random>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "KisCompletionRegistry.h"
#include "kis_image_config.h"
#include "KisCpuPageReplicaProvider.h"
#include "KisImageEpochReferenceModel.h"
#include "KisPageDefaultStorage_p.h"
#include "KisPageMetadataCoordinator.h"
#include "KisPageMetadataCoordinator_p.h"
#include "KisPageOwnerLedger.h"
#include "KisPageReplicaProvider.h"
#include "KisPageStore.h"
#include "KisPageStoreCheckpoint.h"
#include "KisPageStoreCpuAccessSession.h"
#include "KisPageStoreCpuSurfaceOps.h"
#include "KisPageStoreDiagnostics_p.h"
#include "KisPageStoreMementoManager.h"
#include "KisPageStoreRandomAccessor.h"
#include "KisPageStoreReclamation_p.h"
#include "KisPageRetirementRecord_p.h"
#include "KisPageRetirementQueue_p.h"
#include "KisPageWriteCoordinator_p.h"
#include "KisTiledDataManagerPageStoreBackend.h"
#include "KisTiles3PageReplicaProvider.h"
#include "KisUnifiedSsdPageStore.h"
#include "tiles3/kis_random_accessor.h"
#include "tiles3/kis_hline_iterator.h"
#include "tiles3/kis_vline_iterator.h"
#include "tiles3/kis_tile_data.h"
#include "tiles3/kis_tile_data_store.h"
#include "tiles3/tests/kis_tile_data_store_test_access.h"

namespace
{

// Synthetic oracle receiver. Its output storage is prepared before metadata
// changes; commit observes the delivered span without allocating after accept.
struct MetadataEffectReceiver {
    QVector<KisPageTransitionEffect> effects, candidate;
    quint64 prepared = 0, committed = 0, cancelled = 0;
    static bool prepare(void *p, const KisPageTransitionEffect *values, qsizetype count,
                        quint64 *cookie, QString *)
    {
        auto &self = *static_cast<MetadataEffectReceiver *>(p);
        self.candidate = QVector<KisPageTransitionEffect>(values, values + count);
        self.prepared += quint64(count); *cookie = quint64(count);
        return true;
    }
    static void commit(void *p, quint64 cookie, const KisPageTransitionEffect *values, qsizetype count) noexcept
    {
        auto &self = *static_cast<MetadataEffectReceiver *>(p);
        Q_ASSERT(cookie == quint64(count) && self.candidate.size() == count);
        for (qsizetype i = 0; i < count; ++i) self.candidate[i] = values[i];
        self.effects = std::move(self.candidate);
        self.committed += cookie;
    }
    static void cancel(void *p, quint64 cookie) noexcept
    {
        auto &self = *static_cast<MetadataEffectReceiver *>(p);
        self.candidate.clear(); self.cancelled += cookie;
    }
};

struct CollidingIndexKey {
    quint64 value = 0;
    friend bool operator==(CollidingIndexKey a, CollidingIndexKey b) { return a.value == b.value; }
    friend size_t qHash(CollidingIndexKey, size_t) { return 127; }
};

template<typename Callback>
bool invokeEpochCallback(void *context, KisImageEpochId epoch)
{
    return (*static_cast<Callback *>(context))(epoch);
}

KisPageKey pageKey(qint32 column, qint32 row = 0)
{
    return {KisSurfaceId{1}, KisLogicalPageId{column, row}};
}

KisPageVersion pageVersion(qint32 column, quint64 generation, qint32 row = 0)
{
    return {pageKey(column, row), KisPageGeneration{generation}};
}

KisPageAllocationDescriptor allocationDescriptor()
{
    KisPageAllocationDescriptor descriptor;
    descriptor.layoutRevision = 1;
    descriptor.format.formatId = 1;
    descriptor.format.colorModelId = "ALPHA";
    descriptor.format.colorDepthId = "U8";
    descriptor.format.profileFingerprint = "reference-test";
    descriptor.format.channelOrder = "A";
    descriptor.format.packing = "interleaved";
    descriptor.format.defaultPixel = QByteArray(1, 0);
    descriptor.format.channelCount = 1;
    descriptor.format.pixelStride = 1;
    descriptor.format.pixelAlignment = 1;
    descriptor.format.hasAlpha = true;
    descriptor.format.alphaSemantic = KisSurfaceAlphaSemantic::SelectionMask;
    descriptor.format.endianness = KisSurfaceEndianness::NativeEndian;
    descriptor.format.codecVersion = 1;
    descriptor.pageExtent = QSize(64, 64);
    descriptor.validRect = QRect(QPoint(0, 0), descriptor.pageExtent);
    descriptor.rowAlignment = 1;
    return descriptor;
}

KisSurfaceEpochState
surfaceEpochState(KisSurfaceId surface = KisSurfaceId{1}, quint64 defaultPixelRevision = 1, quint8 defaultPixel = 0)
{
    const KisPageAllocationDescriptor base = allocationDescriptor();
    KisSurfaceEpochState state;
    state.surface = surface;
    state.format = base.format;
    state.format.defaultPixel = QByteArray(1, char(defaultPixel));
    state.contentExtent = QRect(0, 0, 512, 512);
    state.logicalPageExtent = base.pageExtent;
    state.layoutRevision = base.layoutRevision;
    state.rowAlignment = base.rowAlignment;
    state.defaultPixelRevision = defaultPixelRevision;
    state.extentRevision = 1;
    return state;
}

bool configureCapturedViewStore(KisPageStore &store,
                                std::shared_ptr<KisCpuPageReplicaProvider> *provider,
                                QString *error)
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    *provider = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig config;
    config.provider = {170};
    config.providerEpoch = {1};
    config.budgetBytes = 64 * 1024 * 1024;
    if (!(*provider)->configure(config, completions, error))
        return false;
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.surfaces = {surfaceEpochState()};
    return store.configure(initial, completions, 4, error) && store.registerReplicaProvider(*provider)
        && store.finalizeInitialization(error);
}

bool commitCapturedViewFill(KisPageStore &store, int pages, quint8 value, QString *error)
{
    const auto transaction = store.beginCurrentTransaction();
    return transaction.isValid()
        && KisPageStoreCpuSurfaceOps::fillRect(&store,
                                               {1},
                                               transaction,
                                               QRect(0, 0, pages * 64, 64),
                                               QByteArray(1, char(value)),
                                               error)
        && store.commit(transaction, store.preparedPages(transaction)).isValid();
}

KisReplicaHandle replica(const KisPageVersion &version,
                         quint64 provider,
                         quint64 providerEpoch,
                         quint64 slot,
                         quint64 allocationGeneration = 1)
{
    KisReplicaHandle result;
    result.provider = KisReplicaProviderId{provider};
    result.providerEpoch = KisReplicaProviderEpoch{providerEpoch};
    result.allocation = KisReplicaAllocationToken{slot, allocationGeneration};
    result.version = version;
    result.domain = KisPageAccessDomain::CpuRam;
    result.layout = {1, 1, QSize(64, 64), QRect(0, 0, 64, 64), 64, 4096};
    return result;
}

void comparePageRecords(const KisPageStateSnapshot &a, const KisPageStateSnapshot &b)
{
    QCOMPARE(a.key, b.key);
    QCOMPARE(a.publishedEpoch, b.publishedEpoch);
    QCOMPARE(a.publishedGeneration, b.publishedGeneration);
    QCOMPARE(a.publishedDefaultPixelRevision, b.publishedDefaultPixelRevision);
    QCOMPARE(a.nextGeneration, b.nextGeneration);
    QCOMPARE(a.writer.token, b.writer.token);
    QCOMPARE(a.writer.operation, b.writer.operation);
    QCOMPARE(a.writer.transaction, b.writer.transaction);
    QCOMPARE(a.writer.baseVersion, b.writer.baseVersion);
    QCOMPARE(a.writer.baseAuthority, b.writer.baseAuthority);
    QCOMPARE(a.writer.target, b.writer.target);
    QCOMPARE(a.writer.mode, b.writer.mode);
    QCOMPARE(a.writer.phase, b.writer.phase);
    QCOMPARE(a.authorityHandoff.operation, b.authorityHandoff.operation);
    QCOMPARE(a.authorityHandoff.source, b.authorityHandoff.source);
    QCOMPARE(a.authorityHandoff.target, b.authorityHandoff.target);
    QCOMPARE(a.versions.size(), b.versions.size());
    for (qsizetype i = 0; i < a.versions.size(); ++i) {
        const auto &av = a.versions[i], &bv = b.versions[i];
        QCOMPARE(av.version, bv.version);
        QCOMPARE(av.publication, bv.publication);
        QCOMPARE(av.authority, bv.authority);
        QCOMPARE(av.preparedBy, bv.preparedBy);
        QCOMPARE(av.capturedReadViews, bv.capturedReadViews);
        QCOMPARE(av.replicas.size(), bv.replicas.size());
        for (qsizetype j = 0; j < av.replicas.size(); ++j) {
            const auto &ar = av.replicas[j], &br = bv.replicas[j];
            QCOMPARE(ar.replica, br.replica);
            QCOMPARE(ar.validity, br.validity);
            QCOMPARE(ar.activeOperation, br.activeOperation);
            QCOMPARE(ar.readLeases, br.readLeases);
            QCOMPARE(ar.pinCount, br.pinCount);
            QCOMPARE(ar.pendingLastUses, br.pendingLastUses);
        }
    }
}

KisPageStateSnapshot pageWithHistory(int history)
{
    const auto current = pageVersion(0, quint64(history + 1));
    KisPageStateSnapshot page;
    page.key = current.key;
    page.publishedEpoch = {1};
    page.publishedGeneration = current.generation;
    page.nextGeneration = {quint64(history + 2)};
    for (int i = 0; i <= history; ++i) {
        const auto version = pageVersion(0, quint64(history + 1 - i));
        const auto authority = replica(version, 1, 1, version.generation.value);
        page.versions.append({version,
                              i ? KisPagePublicationState::Historical : KisPagePublicationState::Published,
                              {{authority, KisReplicaValidity::Valid, {}, {}, 0, {}}},
                              authority,
                              {},
                              {}});
    }
    return page;
}

KisPreparedPageProof preparedProof(const KisPageVersion &version,
                                   KisPageTransactionId transaction,
                                   const KisCompletionTicket &completion,
                                   quint64 validationStamp)
{
    const KisReplicaHandle authority = replica(version, 17, 1, validationStamp);
    KisPreparedPageProof proof;
    proof.transaction = transaction;
    proof.authority = authority;
    proof.producerCompletion = completion;
    proof.providerValidationStamp = validationStamp;
    return proof;
}

KisPageStateSnapshot initialPageState(const KisPageVersion &version, const KisReplicaHandle &authority)
{
    KisPageVersionStateSnapshot versionState;
    versionState.version = version;
    versionState.publication = KisPagePublicationState::Published;
    versionState.replicas.append({authority, KisReplicaValidity::Valid, {}, {}, 0, {}});
    versionState.authority = authority;

    KisPageStateSnapshot state;
    state.key = version.key;
    state.publishedEpoch = KisImageEpochId{1};
    state.publishedGeneration = version.generation;
    state.nextGeneration = KisPageGeneration{version.generation.value + 1};
    state.versions.append(versionState);
    return state;
}

enum class FakeFailurePoint : quint8 {
    None,
    Allocate,
    Transfer,
    Validate,
    Retire
};

class FakeCpuReplicaProvider final : public KisPageReplicaProvider
{
public:
    explicit FakeCpuReplicaProvider(const std::shared_ptr<KisCompletionRegistry> &completions)
        : m_completions(completions)
    {
        const KisCompletionDomain source = KisCompletionDomain::CpuJob;
        m_completionSource = m_completions ? m_completions->registerSource(source) : 0;
    }

    QString name() const override
    {
        return QStringLiteral("BR1 fake CPU replica provider");
    }

    KisReplicaProviderId providerId() const override
    {
        return KisReplicaProviderId{17};
    }

    KisReplicaProviderEpoch providerEpoch() const override
    {
        QMutexLocker locker(&m_mutex);
        return KisReplicaProviderEpoch{m_epoch};
    }

    KisReplicaCapabilities capabilities() const override
    {
        KisReplicaCapabilities result;
        result.domains = {KisPageAccessDomain::CpuRam};
        result.consumerAccess = {{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer}};
        return result;
    }

    KisReplicaOperation requestReplica(KisPageOperationId operation,
                                       const KisPageVersion &version,
                                       const KisPageAllocationDescriptor &descriptor,
                                       KisPageAccessDomain domain,
                                       KisPageAccessMode mode,
                                       KisPagePriority priority) override
    {
        Q_UNUSED(mode);
        Q_UNUSED(priority);
        return allocate(operation, version, descriptor, domain);
    }

    KisReplicaOperation prepareWrite(KisPageOperationId operation,
                                     const KisPageVersion &version,
                                     const KisPageAllocationDescriptor &descriptor,
                                     KisPageAccessDomain domain,
                                     KisPageWriteMode mode,
                                     KisPagePriority priority) override
    {
        Q_UNUSED(mode);
        Q_UNUSED(priority);
        return allocate(operation, version, descriptor, domain);
    }

    KisReplicaOperation transfer(const KisReplicaTransferRequest &request, KisPagePriority priority) override
    {
        Q_UNUSED(priority);
        QMutexLocker locker(&m_mutex);
        const auto sourceIt = m_allocations.constFind(request.source.allocation.slot);
        const auto targetIt = m_allocations.constFind(request.target.allocation.slot);
        if (consumeFailure(FakeFailurePoint::Transfer)) {
            return failed(request.operation, QStringLiteral("injected transfer failure"));
        }
        if (!request.isSameProviderTransfer() || !owns(request.source) || !owns(request.target)
            || sourceIt == m_allocations.constEnd() || targetIt == m_allocations.constEnd()
            || !(sourceIt->handle == request.source) || !sourceIt->valid || !(targetIt->handle == request.target)
            || m_pending.contains(request.operation.value) || !m_completions || m_completionSource == 0) {
            return failed(request.operation, QStringLiteral("fake transfer request is invalid"));
        }
        const KisCompletionTicket ticket = m_completions->allocatePending(m_completionSource);
        if (!ticket.isValid()) {
            return failed(request.operation, QStringLiteral("completion allocation failed"));
        }
        Pending pending;
        pending.kind = PendingKind::Transfer;
        pending.ticket = ticket;
        pending.replica = request.target;
        pending.source = request.source;
        m_pending.insert(request.operation.value, pending);
        return {KisPageRequestStatus::Pending, request.operation, request.target, ticket, {}};
    }

    KisReplicaAccess resolveAccess(KisPageLeaseId lease,
                                   KisPageOperationId operation,
                                   const KisReplicaHandle &handle,
                                   KisPageAccessRequirement requirement,
                                   KisPageAccessMode mode) override
    {
        QMutexLocker locker(&m_mutex);
        auto allocationIt = m_allocations.find(handle.allocation.slot);
        if (!lease.isValid() || !operation.isValid() || !owns(handle) || !capabilities().supports(requirement)
            || requirement.domain != handle.domain || allocationIt == m_allocations.end()
            || !(allocationIt->handle == handle) || !allocationIt->valid || m_activeLeases.contains(lease.value)) {
            return {};
        }

        m_activeLeases.insert(lease.value, handle);
        KisReplicaAccess access;
        access.lease = lease;
        access.operation = operation;
        access.replica = handle;
        access.mode = mode;
        if (mode == KisPageAccessMode::Read) {
            access.cpuReadData = allocationIt->bytes.constData();
        } else {
            access.cpuWriteData = allocationIt->bytes.data();
        }
        return access;
    }

    void releaseAccess(KisReplicaAccess access, const KisCompletionTicket &lastUse) override
    {
        Q_UNUSED(lastUse);
        QMutexLocker locker(&m_mutex);
        auto leaseIt = m_activeLeases.find(access.lease.value);
        if (access.isValid() && leaseIt != m_activeLeases.end() && leaseIt.value() == access.replica) {
            m_activeLeases.erase(leaseIt);
            m_releaseCount++;
        }
    }

    bool validate(const KisReplicaHandle &handle, const KisPageAllocationDescriptor &descriptor) const override
    {
        QMutexLocker locker(&m_mutex);
        if (consumeFailure(FakeFailurePoint::Validate))
            return false;
        const auto allocationIt = m_allocations.constFind(handle.allocation.slot);
        return owns(handle) && descriptor.isValid() && handle.layout.matches(descriptor)
            && allocationIt != m_allocations.constEnd() && allocationIt->handle == handle && allocationIt->valid;
    }

    KisReplicaOperation
    retire(KisPageOperationId operation, const KisReplicaHandle &handle, const KisCompletionTicket &lastUse) override
    {
        Q_UNUSED(lastUse);
        QMutexLocker locker(&m_mutex);
        if (consumeFailure(FakeFailurePoint::Retire)) {
            return failed(operation, QStringLiteral("injected retirement failure"));
        }
        const auto allocationIt = m_allocations.constFind(handle.allocation.slot);
        if (!operation.isValid() || !owns(handle) || allocationIt == m_allocations.constEnd()
            || !(allocationIt->handle == handle) || !allocationIt->valid || hasActiveLease(handle)
            || m_pending.contains(operation.value) || !m_completions || m_completionSource == 0) {
            return failed(operation, QStringLiteral("fake retirement request is invalid"));
        }
        const KisCompletionTicket ticket = m_completions->allocatePending(m_completionSource);
        if (!ticket.isValid()) {
            return failed(operation, QStringLiteral("completion allocation failed"));
        }
        Pending pending;
        pending.kind = PendingKind::Retire;
        pending.ticket = ticket;
        pending.replica = handle;
        m_pending.insert(operation.value, pending);
        return {KisPageRequestStatus::Pending, operation, handle, ticket, {}};
    }

    KisReplicaMemoryUsage memoryUsage() const override
    {
        QMutexLocker locker(&m_mutex);
        KisReplicaMemoryUsage usage;
        usage.residentBytes = quint64(m_allocations.size()) * 4096;
        usage.committedBytes = usage.residentBytes;
        usage.budgetBytes = 4096 * 1024;
        return usage;
    }

    bool complete(KisPageOperationId operation, bool succeeded)
    {
        KisCompletionTicket ticket;
        {
            QMutexLocker locker(&m_mutex);
            auto pendingIt = m_pending.find(operation.value);
            if (pendingIt == m_pending.end())
                return false;
            ticket = pendingIt->ticket;

            auto targetIt = m_allocations.find(pendingIt->replica.allocation.slot);
            if (pendingIt->kind == PendingKind::Allocate) {
                if (succeeded && targetIt != m_allocations.end()) {
                    targetIt->valid = true;
                } else if (targetIt != m_allocations.end()) {
                    m_allocations.erase(targetIt);
                }
            } else if (pendingIt->kind == PendingKind::Transfer) {
                const auto sourceIt = m_allocations.constFind(pendingIt->source.allocation.slot);
                if (succeeded && sourceIt != m_allocations.constEnd() && sourceIt->handle == pendingIt->source
                    && sourceIt->valid && targetIt != m_allocations.end() && targetIt->handle == pendingIt->replica) {
                    targetIt->bytes = sourceIt->bytes;
                    targetIt->valid = true;
                } else if (targetIt != m_allocations.end()) {
                    targetIt->valid = false;
                }
            } else if (succeeded && targetIt != m_allocations.end()) {
                m_allocations.erase(targetIt);
            }
            m_pending.erase(pendingIt);
        }
        return m_completions->complete(ticket,
                                       succeeded ? KisCompletionStatus::Succeeded : KisCompletionStatus::Failed);
    }

    void injectNext(FakeFailurePoint point)
    {
        QMutexLocker locker(&m_mutex);
        m_failure = point;
    }

    void restart()
    {
        QVector<KisCompletionTicket> cancelledTickets;
        {
            QMutexLocker locker(&m_mutex);
            cancelledTickets.reserve(m_pending.size());
            for (const Pending &pending : std::as_const(m_pending)) {
                cancelledTickets.append(pending.ticket);
            }
            m_epoch++;
            m_nextSlot = 1;
            m_allocations.clear();
            m_pending.clear();
            m_activeLeases.clear();
        }
        for (const KisCompletionTicket &ticket : cancelledTickets) {
            m_completions->complete(ticket, KisCompletionStatus::Cancelled);
        }
    }

    quint64 releaseCount() const
    {
        QMutexLocker locker(&m_mutex);
        return m_releaseCount;
    }

private:
    enum class PendingKind : quint8 {
        Allocate,
        Transfer,
        Retire
    };

    struct Allocation {
        KisReplicaHandle handle;
        QByteArray bytes;
        bool valid = false;
    };

    struct Pending {
        PendingKind kind = PendingKind::Allocate;
        KisCompletionTicket ticket;
        KisReplicaHandle replica;
        KisReplicaHandle source;
    };

    bool owns(const KisReplicaHandle &handle) const
    {
        return handle.isValid() && handle.provider == providerId() && handle.providerEpoch.value == m_epoch
            && handle.domain == KisPageAccessDomain::CpuRam;
    }

    bool hasActiveLease(const KisReplicaHandle &handle) const
    {
        for (const KisReplicaHandle &leased : std::as_const(m_activeLeases)) {
            if (leased == handle)
                return true;
        }
        return false;
    }

    bool consumeFailure(FakeFailurePoint point) const
    {
        if (m_failure != point)
            return false;
        m_failure = FakeFailurePoint::None;
        return true;
    }

    KisReplicaOperation failed(KisPageOperationId operation, const QString &error) const
    {
        KisReplicaOperation result;
        result.status = KisPageRequestStatus::Failed;
        result.operation = operation;
        result.error = error;
        return result;
    }

    KisReplicaOperation allocate(KisPageOperationId operation,
                                 const KisPageVersion &version,
                                 const KisPageAllocationDescriptor &descriptor,
                                 KisPageAccessDomain domain)
    {
        QMutexLocker locker(&m_mutex);
        if (consumeFailure(FakeFailurePoint::Allocate)) {
            return failed(operation, QStringLiteral("injected allocation failure"));
        }
        if (!operation.isValid() || !version.isValid() || !descriptor.isValid() || domain != KisPageAccessDomain::CpuRam
            || m_pending.contains(operation.value) || !m_completions || m_completionSource == 0) {
            return failed(operation, QStringLiteral("fake allocation request is invalid"));
        }

        KisReplicaHandle handle;
        handle.provider = providerId();
        handle.providerEpoch = KisReplicaProviderEpoch{m_epoch};
        handle.allocation = KisReplicaAllocationToken{m_nextSlot++, 1};
        handle.version = version;
        handle.domain = domain;
        handle.layout = {descriptor.layoutRevision,
                         descriptor.format.formatId,
                         descriptor.pageExtent,
                         descriptor.validRect,
                         quint32(descriptor.minimumRowBytes()),
                         descriptor.minimumByteSize()};

        const KisCompletionTicket ticket = m_completions->allocatePending(m_completionSource);
        if (!ticket.isValid()) {
            return failed(operation, QStringLiteral("completion allocation failed"));
        }

        Allocation allocation;
        allocation.handle = handle;
        allocation.bytes = QByteArray(qsizetype(handle.layout.byteSize), 0);
        m_allocations.insert(handle.allocation.slot, allocation);

        Pending pending;
        pending.ticket = ticket;
        pending.replica = handle;
        m_pending.insert(operation.value, pending);
        return {KisPageRequestStatus::Pending, operation, handle, ticket, {}};
    }

    std::shared_ptr<KisCompletionRegistry> m_completions;
    quint64 m_completionSource = 0;
    mutable QMutex m_mutex;
    quint64 m_epoch = 1;
    quint64 m_nextSlot = 1;
    QHash<quint64, Allocation> m_allocations;
    QHash<quint64, Pending> m_pending;
    QHash<quint64, KisReplicaHandle> m_activeLeases;
    mutable FakeFailurePoint m_failure = FakeFailurePoint::None;
    quint64 m_releaseCount = 0;
};

class StubTransferBridge final : public KisPageReplicaTransferBridge
{
public:
    explicit StubTransferBridge(bool synchronous)
        : m_synchronous(synchronous)
    {
    }

    bool synchronousOperations() const override
    {
        return m_synchronous;
    }

    bool supports(const KisReplicaTransferRequest &request) const override
    {
        Q_UNUSED(request);
        return false;
    }

    KisReplicaOperation transfer(const KisReplicaTransferRequest &request,
                                 const std::shared_ptr<KisPageReplicaProvider> &sourceProvider,
                                 const std::shared_ptr<KisPageReplicaProvider> &targetProvider,
                                 KisPagePriority priority) override
    {
        Q_UNUSED(request);
        Q_UNUSED(sourceProvider);
        Q_UNUSED(targetProvider);
        Q_UNUSED(priority);
        return {};
    }

private:
    bool m_synchronous = false;
};

class FakeAsyncExactArchive final : public KisExactGenerationArchive
{
public:
    explicit FakeAsyncExactArchive(const std::shared_ptr<KisCompletionRegistry> &completions)
        : m_completions(completions)
    {
        const KisCompletionDomain source = KisCompletionDomain::IoOperation;
        m_completionSource = m_completions ? m_completions->registerSource(source) : 0;
    }

    QString name() const override
    {
        return QStringLiteral("fake async exact-generation archive");
    }
    KisReplicaProviderId archiveId() const override
    {
        return KisReplicaProviderId{72};
    }
    KisReplicaProviderEpoch archiveEpoch() const override
    {
        return KisReplicaProviderEpoch{1};
    }
    bool isOperational() const override
    {
        return m_completions && m_completionSource != 0;
    }
    bool synchronousOperations() const override
    {
        return false;
    }

    KisPageArchiveOperation storeExact(const KisExactPageArchiveWrite &write, KisPagePriority priority) override
    {
        Q_UNUSED(priority);
        KisPageArchiveOperation result;
        result.operation = write.operation;
        result.version = write.version;
        if (!write.isValid() || !isOperational()) {
            result.status = KisPageRequestStatus::Failed;
            result.error = QStringLiteral("fake async archive write is invalid");
            return result;
        }
        QMutexLocker locker(&m_mutex);
        if (m_pending.contains(write.operation.value) || m_records.contains(write.version)) {
            result.status = KisPageRequestStatus::Failed;
            result.error = QStringLiteral("fake async archive identity is duplicated");
            return result;
        }
        const KisCompletionTicket completion = m_completions->allocatePending(m_completionSource);
        if (!completion.isValid()) {
            result.status = KisPageRequestStatus::Failed;
            result.error = QStringLiteral("fake async completion allocation failed");
            return result;
        }
        Entry entry;
        entry.version = write.version;
        entry.descriptor = write.descriptor;
        entry.layout = write.sourceLayout;
        entry.bytes = QByteArray(static_cast<const char *>(write.sourceData), qsizetype(write.sourceLayout.byteSize));
        entry.completion = completion;
        m_pending.insert(write.operation.value, entry);
        result.status = KisPageRequestStatus::Pending;
        result.completion = completion;
        return result;
    }

    bool cancelStoreExact(KisPageOperationId operation) override
    {
        return finish(operation, KisCompletionStatus::Cancelled);
    }

    bool loadExact(const KisPageVersion &version,
                   const KisPageAllocationDescriptor &descriptor,
                   QByteArray *bytes,
                   KisReplicaLayout *layout) const override
    {
        if (!bytes)
            return false;
        QMutexLocker locker(&m_mutex);
        const auto it = m_records.constFind(version);
        if (it == m_records.constEnd() || !(it->descriptor == descriptor)) {
            return false;
        }
        *bytes = it->bytes;
        if (layout)
            *layout = it->layout;
        return true;
    }
    bool contains(const KisPageVersion &version) const override
    {
        QMutexLocker locker(&m_mutex);
        return m_records.contains(version);
    }
    bool forget(const KisPageVersion &version) override
    {
        QMutexLocker locker(&m_mutex);
        return m_records.remove(version) == 1;
    }

    bool finish(KisPageOperationId operation, KisCompletionStatus status)
    {
        if (status != KisCompletionStatus::Succeeded && status != KisCompletionStatus::Failed
            && status != KisCompletionStatus::Cancelled) {
            return false;
        }
        QMutexLocker locker(&m_mutex);
        auto it = m_pending.find(operation.value);
        if (it == m_pending.end())
            return false;
        const Entry entry = it.value();
        if (!m_completions->complete(entry.completion, status))
            return false;
        if (status == KisCompletionStatus::Succeeded) {
            m_records.insert(entry.version, entry);
        }
        m_pending.erase(it);
        return true;
    }

    KisPageOperationId pendingOperation(const KisCompletionTicket &completion) const
    {
        QMutexLocker locker(&m_mutex);
        for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it) {
            if (it->completion == completion) {
                return KisPageOperationId{it.key()};
            }
        }
        return {};
    }

private:
    struct Entry {
        KisPageVersion version;
        KisPageAllocationDescriptor descriptor;
        KisReplicaLayout layout;
        QByteArray bytes;
        KisCompletionTicket completion;
    };

    std::shared_ptr<KisCompletionRegistry> m_completions;
    quint64 m_completionSource = 0;
    mutable QMutex m_mutex;
    QHash<quint64, Entry> m_pending;
    QHash<KisPageVersion, Entry> m_records;
};

class ReentrantProbeCpuProvider final : public KisPageReplicaProvider
{
public:
    explicit ReentrantProbeCpuProvider(const std::shared_ptr<KisPageReplicaProvider> &delegate)
        : m_delegate(delegate)
    {
    }

    std::function<void()> prepareProbe;
    std::function<void()> transferProbe;
    std::function<void()> resolveProbe;
    std::function<void()> releaseProbe;
    std::function<void()> retireProbe;
    std::function<void()> nativeCopyProbe;
    bool failNextRetire = false;
    bool enableNativeCopy = false;
    bool corruptNextNativeOperation = false;
    bool pendNextNativeResult = false;

    QString name() const override
    {
        return m_delegate->name();
    }
    KisReplicaProviderId providerId() const override
    {
        return m_delegate->providerId();
    }
    KisReplicaProviderEpoch providerEpoch() const override
    {
        return m_delegate->providerEpoch();
    }
    KisReplicaCapabilities capabilities() const override
    {
        auto result = m_delegate->capabilities();
        if (enableNativeCopy)
            result.synchronousWriteCopy = true;
        return result;
    }
    KisReplicaOperation requestReplica(KisPageOperationId operation,
                                       const KisPageVersion &version,
                                       const KisPageAllocationDescriptor &descriptor,
                                       KisPageAccessDomain domain,
                                       KisPageAccessMode mode,
                                       KisPagePriority priority) override
    {
        return m_delegate->requestReplica(operation, version, descriptor, domain, mode, priority);
    }
    KisReplicaOperation prepareWrite(KisPageOperationId operation,
                                     const KisPageVersion &version,
                                     const KisPageAllocationDescriptor &descriptor,
                                     KisPageAccessDomain domain,
                                     KisPageWriteMode mode,
                                     KisPagePriority priority) override
    {
        if (prepareProbe)
            prepareProbe();
        return m_delegate->prepareWrite(operation, version, descriptor, domain, mode, priority);
    }
    KisReplicaOperation transfer(const KisReplicaTransferRequest &request, KisPagePriority priority) override
    {
        if (transferProbe)
            transferProbe();
        return m_delegate->transfer(request, priority);
    }
    KisReplicaAccess resolveAccess(KisPageLeaseId lease,
                                   KisPageOperationId operation,
                                   const KisReplicaHandle &replica,
                                   KisPageAccessRequirement requirement,
                                   KisPageAccessMode mode) override
    {
        if (resolveProbe)
            resolveProbe();
        return m_delegate->resolveAccess(lease, operation, replica, requirement, mode);
    }
    void releaseAccess(KisReplicaAccess access, const KisCompletionTicket &lastUse) override
    {
        if (releaseProbe)
            releaseProbe();
        m_delegate->releaseAccess(std::move(access), lastUse);
    }
    bool validate(const KisReplicaHandle &replica, const KisPageAllocationDescriptor &descriptor) const override
    {
        return m_delegate->validate(replica, descriptor);
    }
    KisReplicaOperation
    retire(KisPageOperationId operation, const KisReplicaHandle &replica, const KisCompletionTicket &lastUse) override
    {
        if (retireProbe)
            retireProbe();
        if (failNextRetire) {
            failNextRetire = false;
            return {};
        }
        return m_delegate->retire(operation, replica, lastUse);
    }
    KisReplicaMemoryUsage memoryUsage() const override
    {
        return m_delegate->memoryUsage();
    }
    KisReplicaOperation prepareSynchronousWriteCopy(KisPageOperationId operation,
                                                    const KisReplicaHandle &source,
                                                    const KisPageVersion &version,
                                                    const KisPageAllocationDescriptor &descriptor,
                                                    KisPagePriority priority) override
    {
        if (nativeCopyProbe)
            nativeCopyProbe();
        auto result = m_delegate->prepareSynchronousWriteCopy(operation, source, version, descriptor, priority);
        if (corruptNextNativeOperation) {
            corruptNextNativeOperation = false;
            ++result.operation.value;
        }
        if (pendNextNativeResult) {
            pendNextNativeResult = false;
            result.status = KisPageRequestStatus::Pending;
        }
        return result;
    }

private:
    std::shared_ptr<KisPageReplicaProvider> m_delegate;
};

} // namespace

class KisPageStoreReferenceTest : public QObject
{
    Q_OBJECT

    static void attachEffects(KisPageMetadataCoordinator &metadata, MetadataEffectReceiver &receiver)
    {
        metadata.attachRetirementDebtOwner(&receiver, &MetadataEffectReceiver::prepare,
                                           &MetadataEffectReceiver::commit, &MetadataEffectReceiver::cancel);
    }

private Q_SLOTS:
    void indexedHistorySlices_data()
    {
        QTest::addColumn<int>("history");
        for (int h : {0, 31, 32, 33, 511, 4095})
            QTest::newRow(qPrintable(QString::number(h))) << h;
    }
    void indexedHistorySlices();
    void metadataCompactRecordsRoundTrip();
    void metadataArenaGrowthCausalBaseline();
    void metadataSlotArenaGenerationAndRelease();
    void metadataArenaDirectoryGrowthAtCapacity();
    void metadataShardSlotIndexReservations();
    void metadataShardSlotIndexGrowthAndErasure();
    void metadataOwningCapacityIsBudgeted();
    void metadataConfigurationStorageRefusal_data()
    {
        QTest::addColumn<int>("shards");
        QTest::addColumn<bool>("firstAllocation");
        for (int count : {1, 4, 64})
            for (bool first : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("shards%1-first%2").arg(count).arg(first))) << count << first;
    }
    void metadataConfigurationStorageRefusal();
    void metadataConfigurationLateCandidate_data()
    {
        QTest::addColumn<int>("shards");
        QTest::addColumn<bool>("install");
        QTest::addColumn<bool>("mutation");
        for (int count : {1, 64})
            for (bool install : {false, true})
                for (bool mutation : {false, true})
                    QTest::newRow(qPrintable(QStringLiteral("shards%1-install%2-mutation%3")
                        .arg(count).arg(install).arg(mutation))) << count << install << mutation;
    }
    void metadataConfigurationLateCandidate();
    void metadataReadProtectionAtCapacity_data();
    void metadataReadProtectionAtCapacity();
    void metadataReadProtectionMatchesReference_data();
    void metadataReadProtectionMatchesReference();
    void metadataReadProtectionReclaimsEmptyBlocks();
    void metadataReadProtectionDeferredCleanup_data();
    void metadataReadProtectionDeferredCleanup();
    void metadataReadCleanupAfterController_data()
    {
        QTest::addColumn<bool>("acknowledge");
        QTest::addColumn<bool>("explicitClear");
        for (bool acknowledge : {false, true})
            for (bool clear : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("ack%1-clear%2").arg(acknowledge).arg(clear)))
                    << acknowledge << clear;
    }
    void metadataReadCleanupAfterController();
    void capturedProtectionAndExactQueryAtCapacity();
    void metadataReadProtectionReclaimerIdentity();
    void reclamationTaskStorageBeforeOwnerRelease();
    void reclamationTaskRejectedPreparation();
    void retainedStorageWeakTailAndCapacity();
    void retainedStorageConcurrentTeardown();
    void retainedWakeInFlightAfterController();
    void preparedReclamationRunsAtCapacity();
    void preparedReadinessAtCapacity();
    void readinessRetainsOriginalPayer();
    void retainedDeadlineInFlightAfterController();
    void preparedDeadlineRearmsAtCapacity();
    void preparedDeadlineRejectsCancelledGeneration();
    void metadataShardIndexesEnforcePhysicalOwnership();
    void indexedHistoricalDiscard_data()
    {
        QTest::addColumn<int>("variant");
        for (int v = 0; v < 20; ++v)
            QTest::newRow(qPrintable(QString::number(v))) << v;
    }
    void indexedHistoricalDiscard();
    void indexedMutationTransitions_data();
    void indexedMutationTransitions();
    void genericWorkingStorageRefusalIsAtomic_data()
    {
        QTest::addColumn<int>("variant");
        QTest::newRow("read-release") << 0;
        QTest::newRow("private-replacement") << 1;
        QTest::newRow("write-prepare-cancel") << 2;
    }
    void genericWorkingStorageRefusalIsAtomic();
    void concurrentRetirementDebtRefusalPreservesPage();
    void indexedMutationBaseLookup_data()
    {
        QTest::addColumn<int>("history");
        for (int h : {0, 255, 4095})
            QTest::newRow(qPrintable(QString::number(h))) << h;
    }
    void indexedMutationBaseLookup();
    void indexedMutationConcurrentProtection();
    void indexedRecordSlotReuse();
    void indexedCompetingPreparedTransactions();
    void indexedPublicationTransitions_data()
    {
        QTest::addColumn<int>("history");
        QTest::addColumn<int>("variant");
        QTest::addColumn<bool>("compact");
        for (int h : {0, 31, 511})
            for (int v = 0; v < 18; ++v)
                for (bool compact : {false, true})
                    QTest::newRow(qPrintable(QStringLiteral("history%1-case%2-compact%3").arg(h).arg(v).arg(compact)))
                        << h << v << compact;
    }
    void indexedPublicationTransitions();
    void publicationDirectoryStorageLifetime();
    void indexedPublicationIgnoresRetainedHistory_data()
    {
        QTest::addColumn<int>("history");
        for (int h : {0, 30, 31, 511})
            QTest::newRow(qPrintable(QString::number(h))) << h;
    }
    void indexedPublicationIgnoresRetainedHistory();
    void preparedPublicationStorageIsBound_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("outcome");
        for (int k : {0, 2, 3})
            for (int o = 0; o < 6; ++o)
                QTest::newRow(qPrintable(QStringLiteral("kind%1-outcome%2").arg(k).arg(o))) << k << o;
    }
    void preparedPublicationStorageIsBound();
    void deferredPublicationCleanupOwnsCandidate_data()
    {
        QTest::addColumn<int>("history");
        QTest::addColumn<bool>("reject");
        for (int h : {0, 30})
            for (bool r : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("history%1-reject%2").arg(h).arg(r))) << h << r;
    }
    void deferredPublicationCleanupOwnsCandidate();
    void publicationStorageRefusalAndLateCleanup_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("outcome");
        for (int kind = 0; kind < 4; ++kind)
            for (int outcome = 0; outcome < 4; ++outcome)
                QTest::newRow(qPrintable(QStringLiteral("kind%1-outcome%2").arg(kind).arg(outcome)))
                    << kind << outcome;
    }
    void publicationStorageRefusalAndLateCleanup();
    void mutationCandidateStorageAtSharedCapacity_data()
    {
        QTest::addColumn<bool>("install");
        QTest::newRow("cancel") << false;
        QTest::newRow("install") << true;
    }
    void mutationCandidateStorageAtSharedCapacity();
    void diagnosticRecorderIsOwnerAndThreadScoped();
    void currentTransactionPinsBaseWithoutManifestExport();
    void capturedReadViewFreezesPagesAndSurfaceDefault();
    void capturedReadViewValidatesSelectorsAndOutlivesFacade();
    void capturedReadViewReleasesChangedPagesOnly();
    void capturedReadViewReleaseSkipsReachableHistory();
    void capturedReadViewBenchmark_data();
    void capturedReadViewBenchmark();
    void capturedReadViewIsCoherentDuringConcurrentCommits();
    void preparedMetadataPublicationIsBoundAndOneShot();
    void preparedMetadataRejectsReaderLastUseAbortAndCompletionChanges();
    void metadataInstallationPrecedesRootAndRejectsAtomically();
    void preparedEpochCandidateIsInvisibleAndSingleUse();
    void preparedEpochCandidateRejectsChangedInputs_data();
    void preparedEpochCandidateRejectsChangedInputs();
    void preparedEpochCandidateOwnerAndLifetime();
    void epochTransactionStorageAtCapacity_data()
    {
        QTest::addColumn<bool>("complete");
        QTest::addColumn<int>("refusal");
        for (bool full : {false, true})
            for (int storage = 0; storage < 4; ++storage)
                QTest::newRow(qPrintable(QStringLiteral("complete%1-storage%2").arg(full).arg(storage))) << full << storage;
    }
    void epochTransactionStorageAtCapacity();
    void epochTransactionStorageRetainsOriginalOwner();
    void epochTransactionPreparationRevalidatesConcurrentRevision();
    void epochTransactionRejectsOpposingIncrementalChanges_data()
    {
        QTest::addColumn<bool>("firstRemoval");
        QTest::addColumn<bool>("complete");
        for (bool removed : {false, true})
            for (bool full : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("removed%1-complete%2").arg(removed).arg(full))) << removed << full;
    }
    void epochTransactionRejectsOpposingIncrementalChanges();
    void epochInitializationStorageRefusal_data()
    {
        QTest::addColumn<int>("kind");
        for (int i = 0; i < 4; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void epochInitializationStorageRefusal();
    void epochInitializationPublishesOnce();
    void epochRootStorageRefusal_data()
    {
        QTest::addColumn<int>("kind");
        for (int i = 0; i < 6; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void epochRootStorageRefusal();
    void epochTreeTailAtCapacity_data()
    {
        QTest::addColumn<bool>("controllerGone");
        QTest::newRow("live-controller") << false;
        QTest::newRow("detached-accounting") << true;
    }
    void epochTreeTailAtCapacity();
    void persistentExtentIndexMatchesManifestOracle();
    void rootCollectionTracksReferencesIncrementally();
    void retirementQueuesPreserveBudgetAndIdentity_data()
    {
        QTest::addColumn<int>("epochs");
        QTest::addColumn<bool>("transactionsFirst");
        for (int n : {1, 33, 513})
            for (bool first : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("epochs%1-transactionsFirst%2").arg(n).arg(first)))
                    << n << first;
    }
    void retirementQueuesPreserveBudgetAndIdentity();
    void rootAdmissionEndsBeforeCollection_data();
    void rootAdmissionEndsBeforeCollection();
    void reachabilitySkipsRetiredRoots_data();
    void reachabilitySkipsRetiredRoots();
    void boundedReachabilitySurvivesRootRemoval_data()
    {
        QTest::addColumn<int>("history"); QTest::addColumn<int>("removal");
        for (int history : {0, 1, 33, 97}) for (int removal : {0, 1, 2, 3})
            QTest::newRow(qPrintable(QStringLiteral("h%1-remove%2").arg(history).arg(removal))) << history << removal;
    }
    void boundedReachabilitySurvivesRootRemoval();
    void boundedReachabilityInvalidatesOnlyChangedKeys();
    void boundedReachabilitySlotsAndCancellation();
    void boundedReachabilityPreservesDefaultRevision();
    void rootAdmissionSerializesWithFinalRelease_data();
    void rootAdmissionSerializesWithFinalRelease();
    void lastRootDestructionIsIncrementalAndOffThread();
    void preparedMetadataClaimsDoNotLoseConcurrentReaders();
    void preparedPublicationMatchesFullReferenceTransitions();
    void privatePreparedBackingReplacement_data();
    void privatePreparedBackingReplacement();
    void preparedMutationIsAtomicAndBound();
    void recoverableWriteGuards_data();
    void recoverableWriteGuards();
    void preparedRecoverableWriteLifecycle_data();
    void preparedRecoverableWriteLifecycle();
    void recoverableWritePreparationHonorsBudget();
    void mutationWriteSetUsesInlineAndSparseIndexStorage();
    void mutationStorageGrowthIsStableAndCharged();
    void sharedNonPayloadBudgetBoundsStoresAndReleases();
    void mutationStorageReclaimsBlocksAndRejectsABA();
    void mutationStorageIncarnationExhaustion();
    void mutationWriteSetRecyclesHoles_data();
    void mutationWriteSetRecyclesHoles();
    void mutationWriteSetRejectsLiveResourceErasure();
    void writeAdmissionReleasesSubsetClaims();
    void mutationWriteSetBudgetFailureIsAtomic_data();
    void mutationWriteSetBudgetFailureIsAtomic();
    void writeAdmissionClaimsWholeSetsAtomically();
    void admissionStorageRevalidatesGrowth();
    void admissionStorageCaptureRebasesBeforeCopy();
    void writeAdmissionBudgetFailureIsAtomic_data();
    void writeAdmissionBudgetFailureIsAtomic();
    void writeAdmissionTokenExhaustion();
    void mutationActivityAdmissionPreservesExisting();
    void freshWriteSelectorAndBackingBudgetAreBounded();
    void preparedMutationPreservesLateProtection_data()
    {
        QTest::addColumn<int>("protection");
        for (int n = 0; n < 16; ++n)
            QTest::newRow(qPrintable(QString::number(n))) << n;
    }
    void preparedMutationPreservesLateProtection();
    void metadataDirectoryPublicationIsImmutableAndConcurrent();
    void imageEpochCommitIsAtomicAndDetectsConflicts();
    void imageEpochRootUsesBalancedPersistentPageDelta();
    void materializationOracleAndPublicationGuards();
    void fakeCpuProviderSupportsReorderingAndFailure();
    void productionCpuProviderPreservesFormatDefaultAndCowBytes();
    void tiles3ProviderUsesExistingTileStoreBehindLeases();
    void tiles3NativeCopyPreservesSourceAndRejectsInvalidWork();
    void tiles3SourceSurvivesFailedPreparation();
    void pageStoreNativeCopyIsLockExternalRetainedAndFailClosed();
    void cpuProviderPreserves4_8_16ByteFormats();
    void ownerLedgerSealsPreparedPageBeforeEpochCommit();
    void ownerLedgerSeparatesDetachedRetirement();
    void ownerLedgerVerifiesTerminalProviderIdentity();
    void pageStoreCpuFacadeCommitsCowWithoutMixedEpoch();
    void pageStoreChainsRepeatedWritesWithinTransaction();
    void pageStoreLeaseOutlivesOwnerSafely();
    void pageStoreCallsProviderOutsideGlobalLock();
    void pageStoreShutdownRetriesFailedReplicaRetirement();
    void cpuAccessSessionScopesOldAndWritablePageLeases();
    void pageStoreMementoOwnsBeforeAfterRetentionAndPurge();
    void cpuSurfaceOpsPreserveRectCopyAndSparseClear();
    void sparseDefaultPageFirstWriteAndUndoStayVersioned();
    void virtualDefaultRemovalPreservesReaderAndDefaultRevision();
    void surfaceDefaultAndExtentMutateAtomicallyWithEpoch();
    void exactGenerationArchivePreservesPublishedAuthorityOnFailure();
    void asyncArchiveCompletionCancellationAndShutdownAreFailClosed();
    void metadataShardsSurviveConcurrentLeaseStressWithinBudget();
    void checkpointRoundTripPreservesWideFormatAndSparseDefaults();
    void randomAccessorMatchesTiles3AcrossNegativePageBoundary();
    void tiles3BackendMapsIdentityDefaultExtentAndCow();
    void tiles3BackendPublishesDefaultExtentAndHistoryAtomically();
    void tiles3GenericPublicationDerivesOverlayExtent();
    void tiles3BackendCoalescesOverlappingAnonymousLeases();
    void tiles3BackendReclaimsPurgedHistoryVersions();
    void tiles3BackendCoalescesConcurrentProductWrites();
    void tiles3BackendsShareProcessResidentLimit();
};

void KisPageStoreReferenceTest::epochInitializationStorageRefusal()
{
    QFETCH(int, kind);
    kisDrainPageStoreReclamation();
    KisPageBackingLimits limits; limits.metadataArenaBytes = 2 * 1024 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    KisBackingBudgetController budget; QVERIFY(budget.configureSharedNonPayloadBudget(parent));
    const auto warm = KisMutationStorageAllocator<KisPageVersion>::retained(&budget);
    Q_UNUSED(warm);
    const auto live = [&] { return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    KisImageEpochReferenceModel model; model.attachBackingBudget(budget);
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    if (kind == 1 || kind == 3)
        for (int i = 0; i < 4096; ++i) initial.manifest.append(pageVersion(i, 1));
    if (kind == 2)
        for (int i = 1; i <= 4096; ++i) initial.surfaces.append(surfaceEpochState({quint64(i)}));
    const auto before = live();
    const quint64 headroom = kind == 0 ? 0 : kind == 3
        ? quint64(initial.manifest.size()) * sizeof(KisPageVersion) + 16384 : 8192;
    const size_t bytes = size_t(limits.metadataArenaBytes - live() - headroom);
    void *filler = kisAllocateMutationStorage(parent.get(), bytes, 1);
    const auto free = qScopeGuard([&] { if (filler) kisFreeMutationStorage(parent.get(), filler, bytes, 1); });
    QString error;
    QVERIFY(!model.initialize(initial, &error));
    QVERIFY(error.contains(QStringLiteral("storage admission")));
    QCOMPARE(model.rootCount(), qsizetype(0));
    QVERIFY(!model.captureCommittedRoot().isValid());
    QVERIFY(!model.beginTransaction({1}).isValid());
    kisDrainPageStoreReclamation(); // Partial paid tree drains even under the same pressure.
    QCOMPARE(live(), before + bytes);
    kisFreeMutationStorage(parent.get(), filler, bytes, 1); filler = nullptr;
    QVERIFY2(model.initialize(initial, &error), qPrintable(error));
    const auto root = model.captureCommittedRoot();
    QCOMPARE(root.pageCount(), initial.manifest.size());
    QCOMPARE(root.surfaces(), initial.surfaces);
}

void KisPageStoreReferenceTest::epochInitializationPublishesOnce()
{
    kisDrainPageStoreReclamation();
    KisPageBackingLimits limits; limits.metadataArenaBytes = 8 * 1024 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    std::array<KisBackingBudgetReservation, 8> slots;
    for (auto &slot : slots) { slot = parent->reserve({}, nullptr); QVERIFY(slot.isValid()); }
    for (auto &slot : slots) slot.release();
    KisBackingBudgetController budget; QVERIFY(budget.configureSharedNonPayloadBudget(parent));
    const auto warm = KisMutationStorageAllocator<KisPageVersion>::retained(&budget);
    Q_UNUSED(warm);
    const auto live = [&] { return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const auto before = live();
    auto model = std::make_unique<KisImageEpochReferenceModel>(); model->attachBackingBudget(budget);
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    for (int i = 0; i < 4096; ++i) initial.manifest.append(pageVersion(i, 1));
    std::atomic<bool> start{false};
    std::array<bool, 2> accepted{};
    std::array<QString, 2> errors;
    const auto initialize = [&](int i) {
        while (!start.load()) std::this_thread::yield();
        accepted[size_t(i)] = model->initialize(initial, &errors[size_t(i)]);
    };
    std::thread first(initialize, 0), second(initialize, 1);
    start = true; first.join(); second.join();
    QCOMPARE(int(accepted[0]) + int(accepted[1]), 1);
    QVERIFY(!errors[accepted[0] ? 1 : 0].isEmpty());
    QCOMPARE(model->rootCount(), qsizetype(1));
    QCOMPARE(model->captureCommittedRoot().pageCount(), qsizetype(4096));
    kisDrainPageStoreReclamation();
    model.reset(); kisDrainPageStoreReclamation();
    QCOMPARE(live(), before);
}

void KisPageStoreReferenceTest::epochRootStorageRefusal()
{
    QFETCH(int, kind);
    kisDrainPageStoreReclamation();
    KisPageBackingLimits limits; limits.metadataArenaBytes = 2 * 1024 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    KisBackingBudgetController budget; QVERIFY(budget.configureSharedNonPayloadBudget(parent));
    KisImageEpochReferenceModel model; model.attachBackingBudget(budget);
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.surfaces = {surfaceEpochState()};
    const int pages = kind == 3 ? 512 : kind == 4 ? 64 : 0;
    for (int i = 0; i < pages; ++i) initial.manifest.append(pageVersion(i, 1));
    if (kind == 5)
        for (int i = 2; i <= 4096; ++i) initial.surfaces.append(surfaceEpochState({quint64(i)}));
    QVERIFY(model.initialize(initial));
    KisRetainedImageEpochSnapshot retained;
    KisPageTransaction tx;
    if (kind == 1 || kind == 2) { retained = model.captureRetainedRoot(); QVERIFY(retained.isValid()); }
    if (kind == 0 || kind >= 3) { tx = model.beginTransaction({1}); QVERIFY(tx.isValid()); }
    KisCompletionRegistry completions;
    const auto ticket = completions.allocatePending(completions.registerSource(KisCompletionDomain::HostLogical));
    QVERIFY(completions.complete(ticket, KisCompletionStatus::Succeeded));
    if (kind >= 3) {
        KisPreparedPageSet delta; delta.transaction = tx.id;
        if (kind == 5) delta.surfaceChanges = {{initial.surfaces[0], surfaceEpochState({1}, 2, 0x17)}};
        else for (int i = 0; i < (kind == 4 ? pages : 1); ++i)
            delta.proofs.append(preparedProof(pageVersion(i, 2), tx.id, ticket, quint64(i + 1)));
        QVERIFY(model.prepare(delta));
    }
    const auto live = [&] { return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const auto before = live();
    const quint64 headroom = kind == 4 ? quint64(2 * pages) * sizeof(KisPageVersion) + 4096 : kind == 5 ? 2048 : 0;
    const size_t bytes = size_t(limits.metadataArenaBytes - live() - headroom);
    void *filler = kisAllocateMutationStorage(parent.get(), bytes, 1);
    const auto free = qScopeGuard([&] { if (filler) kisFreeMutationStorage(parent.get(), filler, bytes, 1); });
    QString error;
    if (kind == 0) QVERIFY(!model.beginTransaction({1}, &error).isValid());
    else if (kind == 1) { QVERIFY(!model.captureRetainedRoot().isValid()); QVERIFY(model.validateRetainedSnapshot(retained)); }
    else {
        KisImageEpochCommitResult failure;
        if (kind == 2) QVERIFY(!model.prepareRestore(retained, &failure).isValid());
        else QVERIFY(!model.prepareCommit(tx, &failure).isValid());
        error = failure.error;
    }
    if (kind != 1) QVERIFY(error.contains(QStringLiteral("storage admission")));
    QCOMPARE(model.rootCount(), qsizetype(1));
    QCOMPARE(model.captureCommittedRoot().epoch().value, quint64(1));
    if (tx.isValid()) QCOMPARE(model.activeTransaction(tx.id), tx);
    kisDrainPageStoreReclamation();
    QCOMPARE(live(), before + bytes);
    kisFreeMutationStorage(parent.get(), filler, bytes, 1); filler = nullptr;
    if (kind == 0) {
        const auto next = model.beginTransaction({1}); QVERIFY(next.isValid());
        QVERIFY(model.abort(tx)); QVERIFY(model.abort(next));
        QCOMPARE(model.collectFinishedTransactions(), qsizetype(2));
    } else if (kind == 1) {
        const auto next = model.captureRetainedRoot(); QVERIFY(next.isValid());
        QVERIFY(model.releaseSnapshot(retained.token)); QVERIFY(model.releaseSnapshot(next.token));
        QCOMPARE(model.retainedSnapshotCount(), qsizetype(0));
    } else {
        KisImageEpochReferenceModel::PreparedRootReservation restore;
        KisImageEpochReferenceModel::PreparedCommit commit;
        if (kind == 2) { restore = model.prepareRestore(retained, nullptr); QVERIFY(restore.isValid()); }
        else { commit = model.prepareCommit(tx, nullptr); QVERIFY(commit.isValid()); }
        const size_t installBytes = size_t(limits.metadataArenaBytes - live());
        void *installPressure = kisAllocateMutationStorage(parent.get(), installBytes, 1);
        const auto unfill = qScopeGuard([&] { kisFreeMutationStorage(parent.get(), installPressure, installBytes, 1); });
        const auto result = kind == 2 ? model.installRestore(std::move(restore), nullptr, nullptr)
                                     : model.installCommit(std::move(commit), nullptr, nullptr);
        QVERIFY(result.isCommitted());
        QCOMPARE(live(), limits.metadataArenaBytes);
        if (kind == 2) QVERIFY(model.releaseSnapshot(retained.token));
        model.collectFinishedTransactions(); model.collectUnretainedRoots();
    }
}

void KisPageStoreReferenceTest::epochTreeTailAtCapacity()
{
    QFETCH(bool, controllerGone);
    kisDrainPageStoreReclamation();
    KisPageBackingLimits limits; limits.metadataArenaBytes = 4 * 1024 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    std::array<KisBackingBudgetReservation, 8> slots;
    for (auto &slot : slots) { slot = parent->reserve({}, nullptr); QVERIFY(slot.isValid()); }
    for (auto &slot : slots) slot.release();
    const auto live = [&] { return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const auto parentBaseline = live();
    auto budget = std::make_unique<KisBackingBudgetController>();
    QVERIFY(budget->configureSharedNonPayloadBudget(parent));
    { const auto warm = KisMutationStorageAllocator<KisPageVersion>::retained(budget.get()); Q_UNUSED(warm); }
    const auto attachedBaseline = live();
    KisImageEpochRootSnapshot root;
    constexpr int count = 8192;
    {
        KisImageEpochReferenceModel model; model.attachBackingBudget(*budget);
        KisImageEpochSnapshot initial;
        initial.epoch = {1};
        initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
        for (int i = 0; i < count; ++i) initial.manifest.append(pageVersion(i, 1));
        QVERIFY(model.initialize(initial)); root = model.captureCommittedRoot();
    }
    if (controllerGone) budget.reset();
    KisPageVersion version;
    QVERIFY(root.resolve(pageKey(count - 1), &version)); QCOMPARE(version, pageVersion(count - 1, 1));
    QSemaphore entered, resume;
    quint64 observed = 0;
    const auto before = kisPageTreeReclamationStatistics();
    auto blocker = kisPreparePageStoreReclamation([&] { entered.release(); resume.acquire(); });
    auto probe = kisPreparePageStoreReclamation([&] {
        observed = kisPageTreeReclamationStatistics().backgroundNodeDestructions - before.backgroundNodeDestructions;
    });
    blocker->reusable = probe->reusable = false;
    kisEnqueuePageStoreReclamation(blocker.release());
    bool paused = true;
    const auto finish = qScopeGuard([&] { if (paused) resume.release(); kisDrainPageStoreReclamation(); });
    QVERIFY(entered.tryAcquire(1, 5000));
    const size_t bytes = size_t(limits.metadataArenaBytes - live());
    void *filler = kisAllocateMutationStorage(parent.get(), bytes, 1);
    const auto free = qScopeGuard([&] { kisFreeMutationStorage(parent.get(), filler, bytes, 1); });
    root = {};
    QCOMPARE(kisPageTreeReclamationStatistics().foregroundNodeDestructions, before.foregroundNodeDestructions);
    QVERIFY(live() - bytes >= quint64(count) * sizeof(KisPageVersion)); // Actual nodes remain charged while blocked.
    kisEnqueuePageStoreReclamation(probe.release());
    resume.release(); paused = false;
    kisDrainPageStoreReclamation();
    QCOMPARE(observed, quint64(128)); // The next pre-existing job runs between original tree passes.
    const auto after = kisPageTreeReclamationStatistics();
    QCOMPARE(after.backgroundNodeDestructions - before.backgroundNodeDestructions, quint64(count));
    QVERIFY(after.maximumReferenceDropsPerPass <= 128);
    QCOMPARE(live() - bytes, controllerGone ? parentBaseline : attachedBaseline);
    budget.reset();
    QCOMPARE(live() - bytes, parentBaseline);
}

void KisPageStoreReferenceTest::lastRootDestructionIsIncrementalAndOffThread()
{
    kisDrainPageStoreReclamation();
    KisImageEpochRootSnapshot root;
    constexpr int count = 8192;
    {
        KisImageEpochSnapshot initial;
        initial.epoch = {1};
        initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
        for (int i = 0; i < count; ++i)
            initial.manifest.append(pageVersion(i, 1));
        KisImageEpochReferenceModel model;
        QVERIFY(model.initialize(initial));
        root = model.captureCommittedRoot();
    }
    // Root value remains usable after the model dies; cleanup must respect
    // structural sharing, not traverse/delete live branches speculatively.
    KisPageVersion value;
    QVERIFY(root.resolve(pageKey(count - 1), &value));
    QCOMPARE(value, pageVersion(count - 1, 1));
    kisDrainPageStoreReclamation();
    const auto before = kisPageTreeReclamationStatistics();
    root = {};
    const auto foreground = kisPageTreeReclamationStatistics().foregroundNodeDestructions;
    QCOMPARE(foreground - before.foregroundNodeDestructions, quint64(0));
    kisDrainPageStoreReclamation();
    const auto after = kisPageTreeReclamationStatistics();
    QCOMPARE(after.backgroundNodeDestructions - before.backgroundNodeDestructions, quint64(count));
    QVERIFY(after.passes > before.passes);
    QVERIFY(after.maximumReferenceDropsPerPass <= 128);
}

void KisPageStoreReferenceTest::diagnosticRecorderIsOwnerAndThreadScoped()
{
    using Phase = KisPageStoreDiagnosticPhase;
    KisPageStore first, second;
    {
        KisPageStoreDiagnosticRecorder disabled(false, &first);
        QVERIFY(!disabled.setRecording(true));
        KisPageStoreDiagnosticTimer ignored(&first, Phase::CommitOwnerWait, 100);
        QCOMPARE(disabled.metrics()[size_t(Phase::CommitOwnerWait)].intervals, quint64(0));
    }
    KisPageStoreDiagnosticRecorder outer(true, &first);
    {
        KisPageStoreDiagnosticTimer timer(&first, Phase::CommitOwnerWait, 2);
        timer.next(Phase::CommitProofValidation, 2);
    }
    {
        KisPageStoreDiagnosticTimer ignored(&second, Phase::CommitOwnerWait, 10);
    }
    {
        KisPageStoreDiagnosticRecorder inner(true, &second);
        {
            KisPageStoreDiagnosticTimer timer(&second, Phase::CommitOwnerWait, 3);
        }
        QCOMPARE(inner.metrics()[size_t(Phase::CommitOwnerWait)].workItems, quint64(3));
    }
    quint64 workerIntervals = 0;
    std::thread worker([&] {
        // The main thread's recorder must not capture another thread's work.
        {
            KisPageStoreDiagnosticTimer ignored(&first, Phase::CommitOwnerWait, 100);
        }
        KisPageStoreDiagnosticRecorder own(true, &first);
        {
            KisPageStoreDiagnosticTimer timer(&first, Phase::CommitOwnerWait, 4);
        }
        workerIntervals = own.metrics()[size_t(Phase::CommitOwnerWait)].intervals;
    });
    worker.join();
    QCOMPARE(workerIntervals, quint64(1));
    {
        KisPageStoreDiagnosticTimer timer(&first, Phase::CommitOwnerWait, 5);
    }
    QCOMPARE(outer.metrics()[size_t(Phase::CommitOwnerWait)].intervals, quint64(2));
    QCOMPARE(outer.metrics()[size_t(Phase::CommitOwnerWait)].workItems, quint64(7));
    QCOMPARE(outer.metrics()[size_t(Phase::CommitProofValidation)].intervals, quint64(1));
    QVERIFY(outer.setRecording(false));
    {
        KisPageStoreDiagnosticTimer ignored(&first, Phase::CommitOwnerWait, 1000);
    }
    QCOMPARE(outer.metrics()[size_t(Phase::CommitOwnerWait)].workItems, quint64(7));
    QVERIFY(outer.setRecording(true));
    {
        KisPageStoreDiagnosticTimer active(&first, Phase::CommitOwnerWait, 1);
        QVERIFY(!outer.setRecording(false)); // cannot split a sampled operation
    }
    QCOMPARE(outer.metrics()[size_t(Phase::CommitOwnerWait)].intervals, quint64(3));
    QCOMPARE(outer.metrics()[size_t(Phase::CommitProofValidation)].workItems, quint64(2));
}

void KisPageStoreReferenceTest::capturedReadViewFreezesPagesAndSurfaceDefault()
{
    static_assert(!std::is_copy_constructible<KisCapturedReadView>::value, "retention is move-only");
    static_assert(std::is_nothrow_move_constructible<KisCapturedReadView>::value, "scope move is nonthrowing");
    static_assert(!std::is_copy_constructible<KisPageStore>::value, "canonical facade must not become copyable");
    static_assert(sizeof(KisPageStore) == sizeof(void *), "keep the original facade ABI");
    KisPageStore store;
    std::shared_ptr<KisCpuPageReplicaProvider> provider;
    QString error;
    QVERIFY2(configureCapturedViewStore(store, &provider, &error), qPrintable(error));
    QVERIFY(commitCapturedViewFill(store, 2, 0x21, &error));
    auto view = store.captureReadView({}, &error);
    QVERIFY(view.isValid());
    const auto epoch = view.epoch();
    KisPageVersion before;
    QVERIFY(view.resolvePageVersion(pageKey(1), &before));
    KisPageStoreCpuAccessSession reader;
    QVERIFY(reader.beginRead(&store, {1}, {}, KisPagePriority::Normal, &error));
    const auto first = reader.readPage(0, 0, &error);
    QVERIFY(first.isValid());
    QCOMPARE(first.data[0], quint8(0x21));
    KisPageStoreRandomAccessor accessor(&store, {1}, KisPageReadView{});
    QVERIFY(accessor.isValid());
    QVERIFY(!store.closeSession(&error));
    QVERIFY(commitCapturedViewFill(store, 2, 0x45, &error));
    const auto metadata = store.beginCurrentTransaction();
    KisPageReadView metadataBase;
    metadataBase.kind = KisPageReadViewKind::TransactionBaseEpoch;
    metadataBase.transaction = metadata.id;
    KisSurfaceEpochState surface;
    QVERIFY(store.resolveSurfaceState({1}, metadataBase, &surface));
    surface.format.defaultPixel = QByteArray(1, char(0x77));
    ++surface.defaultPixelRevision;
    QVERIFY2(store.stageSurfaceMetadata(metadata, surface, &error), qPrintable(error));
    QVERIFY(store.commit(metadata, store.preparedPages(metadata)).isValid());
    const auto second = reader.readPage(1, 0, &error);
    QVERIFY2(second.isValid(), qPrintable(error));
    QCOMPARE(second.version, before);
    QCOMPARE(second.data[0], quint8(0x21));
    accessor.moveTo(64, 0);
    QVERIFY(accessor.rawDataConst());
    QCOMPARE(accessor.rawDataConst()[0], quint8(0x21));
    QCOMPARE(accessor.surfaceState().defaultPixelRevision, quint64(1));
    const auto blank = reader.readPage(-8, 0, &error);
    QVERIFY2(blank.isValid(), qPrintable(error));
    QCOMPARE(blank.data[0], quint8(0));
    QCOMPARE(blank.version.defaultPixelRevision, quint64(1));
    KisSurfaceEpochState oldSurface;
    QVERIFY(view.resolveSurfaceState({1}, &oldSurface));
    QCOMPARE(oldSurface.defaultPixelRevision, quint64(1));
    QCOMPARE(view.epoch(), epoch);
    auto fresh = store.captureReadView();
    KisSurfaceEpochState newSurface;
    QVERIFY(fresh.resolveSurfaceState({1}, &newSurface));
    QCOMPARE(newSurface.defaultPixelRevision, quint64(2));
    const auto bytesWithHistory = provider->memoryUsage().committedBytes;
    QVERIFY(reader.finish(&error));
    QVERIFY(accessor.finish(&error));
    view = {};
    QVERIFY(provider->memoryUsage().committedBytes + 2 * 4096 <= bytesWithHistory);
    fresh = {};
    QCOMPARE(store.sessionStats().retainedSnapshots, qsizetype(0));
    QVERIFY2(store.closeSession(&error), qPrintable(error));
}

void KisPageStoreReferenceTest::capturedReadViewValidatesSelectorsAndOutlivesFacade()
{
    auto store = std::make_unique<KisPageStore>();
    std::shared_ptr<KisCpuPageReplicaProvider> provider;
    QString error;
    QVERIFY(configureCapturedViewStore(*store, &provider, &error));
    const auto retained = store->captureRetainedEpoch();
    KisPageReadView selected;
    selected.kind = KisPageReadViewKind::CommittedEpoch;
    selected.epoch = retained.snapshot.epoch;
    selected.retention = retained.token;
    auto captured = store->captureReadView(selected, &error);
    QVERIFY(captured.isValid());
    selected.kind = KisPageReadViewKind::ExactVersion;
    selected.exactVersion = {pageKey(0), {1}, 1};
    auto exact = store->captureReadView(selected, &error);
    QVERIFY(exact.isValid());
    KisPageVersion version;
    QVERIFY(!exact.resolvePageVersion(pageKey(1), &version));
    const KisPageAccessRequirement cpu{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
    QVERIFY(!store->acquireReadInView(pageKey(1), exact, cpu, KisPagePriority::Normal).isValid());
    QVERIFY(store->releaseSnapshot(retained.token));
    QVERIFY(!store->captureReadView(selected, &error).isValid());
    QVERIFY(exact.resolvePageVersion(pageKey(0), &version));
    KisPageStore foreign;
    std::shared_ptr<KisCpuPageReplicaProvider> foreignProvider;
    QVERIFY(configureCapturedViewStore(foreign, &foreignProvider, &error));
    QVERIFY(!foreign.acquireReadInView(pageKey(0), captured, cpu, KisPagePriority::Normal).isValid());
    const auto transaction = store->beginCurrentTransaction();
    KisPageReadView base;
    base.kind = KisPageReadViewKind::TransactionBaseEpoch;
    base.transaction = transaction.id;
    auto baseScope = store->captureReadView(base, &error);
    QVERIFY(baseScope.isValid());
    base.kind = KisPageReadViewKind::TransactionOverlay;
    auto overlayScope = store->captureReadView(base, &error);
    QVERIFY(overlayScope.isValid());
    QVERIFY(overlayScope.isTransactionOverlay());
    QVERIFY(store->abort(transaction));
    QVERIFY(overlayScope.resolvePageVersion(pageKey(0), &version));
    overlayScope = {};
    QVERIFY(baseScope.resolvePageVersion(pageKey(0), &version));
    baseScope = {};
    exact = {};
    auto moved = std::move(captured);
    QVERIFY(!captured.isValid());
    const std::weak_ptr<KisCpuPageReplicaProvider> weak(provider);
    provider.reset();
    store.reset();
    QVERIFY(!weak.expired());
    QVERIFY(moved.resolvePageVersion(pageKey(0), &version));
    moved = {};
    kisDrainPageStoreReclamation();
    QVERIFY(weak.expired());
}

void KisPageStoreReferenceTest::capturedReadViewReleasesChangedPagesOnly()
{
    for (int pageCount : {16, 256}) {
        KisPageStore store;
        std::shared_ptr<KisCpuPageReplicaProvider> provider;
        QString error;
        QVERIFY(configureCapturedViewStore(store, &provider, &error));
        QVERIFY(commitCapturedViewFill(store, pageCount, 0x31, &error));
        const auto before = store.readScopeStatistics();
        const auto beforeSession = store.sessionStats();
        KisPageStoreDiagnosticRecorder recorder(true, &store);
        for (int i = 0; i < 64; ++i) {
            auto view = store.captureReadView();
            QVERIFY(view.isValid());
            KisPageVersion version;
            QVERIFY(view.resolvePageVersion(pageKey(pageCount - 1), &version));
        }
        const auto idle = store.readScopeStatistics();
        QCOMPARE(idle.capturedReadViewsCreated - before.capturedReadViewsCreated, quint64(64));
        QCOMPARE(idle.capturedReadViewReleases - before.capturedReadViewReleases, quint64(64));
        QCOMPARE(idle.historicalGcPagesVisited, before.historicalGcPagesVisited);
        QCOMPARE(store.sessionStats().readRequestsCreated, beforeSession.readRequestsCreated);
        QCOMPARE(recorder.metrics()[size_t(KisPageStoreDiagnosticPhase::ManifestExport)].intervals, quint64(0));
        auto view = store.captureReadView();
        const auto oldBytes = provider->memoryUsage().committedBytes;
        QVERIFY(commitCapturedViewFill(store, 1, 0x52, &error));
        QCOMPARE(provider->memoryUsage().committedBytes, oldBytes + 4096);
        const auto beforeRelease = store.readScopeStatistics();
        view = {};
        const auto afterRelease = store.readScopeStatistics();
        QCOMPARE(afterRelease.historicalGcPagesVisited - beforeRelease.historicalGcPagesVisited, quint64(1));
        QCOMPARE(provider->memoryUsage().committedBytes, oldBytes);
    }
}

void KisPageStoreReferenceTest::capturedReadViewReleaseSkipsReachableHistory()
{
    for (int pageCount : {16, 256}) {
        KisPageStore store;
        std::shared_ptr<KisCpuPageReplicaProvider> provider;
        QString error;
        QVERIFY(configureCapturedViewStore(store, &provider, &error));
        QVERIFY(commitCapturedViewFill(store, pageCount, 0x31, &error));
        auto old = store.captureReadView();
        auto sameOld = store.captureReadView();
        const auto oldBytes = provider->memoryUsage().committedBytes;
        QVERIFY(commitCapturedViewFill(store, pageCount, 0x52, &error));
        const auto before = store.readScopeStatistics();
        for (int i = 0; i < 64; ++i) {
            auto current = store.captureReadView();
            KisPageVersion version;
            QVERIFY(current.resolvePageVersion(pageKey(i % pageCount), &version));
        }
        QCOMPARE(store.readScopeStatistics().historicalGcPagesVisited, before.historicalGcPagesVisited);
        old = {};
        QCOMPARE(store.readScopeStatistics().historicalGcPagesVisited, before.historicalGcPagesVisited);
        QCOMPARE(provider->memoryUsage().committedBytes, oldBytes + quint64(pageCount) * 4096);
        sameOld = {};
        QCOMPARE(store.readScopeStatistics().historicalGcPagesVisited - before.historicalGcPagesVisited,
                 quint64(pageCount));
        QCOMPARE(provider->memoryUsage().committedBytes, oldBytes);
    }
}

void KisPageStoreReferenceTest::capturedReadViewBenchmark_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<int>("pages");
    QTest::addColumn<int>("workPages");
    QTest::addColumn<bool>("history");
    QTest::addColumn<bool>("captured");
    const auto row = [](const char *mode, int n, int k, bool h) {
        for (bool captured : {false, true}) {
            const auto tag = QStringLiteral("%1-%2-n%3-k%4-h%5")
                                 .arg(QString::fromLatin1(mode))
                                 .arg(captured ? "captured" : "legacy")
                                 .arg(n)
                                 .arg(k)
                                 .arg(h ? 1 : 0)
                                 .toLatin1();
            QTest::newRow(tag.constData()) << QString::fromLatin1(mode) << n << k << h << captured;
        }
    };
    row("lookup", 16, 1, false);
    row("lookup", 256, 64, false);
    row("scope", 16, 1, false);
    row("scope", 256, 1, false);
    row("scope", 256, 64, false);
    row("scope", 256, 1, true);
}

void KisPageStoreReferenceTest::capturedReadViewBenchmark()
{
    QFETCH(QString, mode);
    QFETCH(int, pages);
    QFETCH(int, workPages);
    QFETCH(bool, history);
    QFETCH(bool, captured);
    bool validDuration = true;
    const auto durationText = qgetenv("KIS_READ_BASELINE_MS");
    const qint64 durationMs = durationText.isEmpty() ? 0 : durationText.toLongLong(&validDuration);
    QVERIFY(validDuration && durationMs >= 0 && durationMs <= 10000);
    KisPageStore store;
    std::shared_ptr<KisCpuPageReplicaProvider> provider;
    QString error;
    QVERIFY(configureCapturedViewStore(store, &provider, &error));
    QVERIFY(commitCapturedViewFill(store, pages, 0x31, &error));
    auto historyRoot = history ? store.captureReadView() : KisCapturedReadView{};
    if (history)
        QVERIFY(commitCapturedViewFill(store, pages, 0x52, &error));
    KisCapturedReadView held;
    KisRetainedImageEpochSnapshot retained;
    const auto selectorFor = [](const KisRetainedImageEpochSnapshot &snapshot) {
        KisPageReadView selector;
        selector.kind = KisPageReadViewKind::CommittedEpoch;
        selector.epoch = snapshot.snapshot.epoch;
        selector.retention = snapshot.token;
        return selector;
    };
    if (mode == "lookup") {
        if (captured)
            held = store.captureReadView();
        else
            retained = store.captureRetainedEpoch();
    }
    const auto selector = selectorFor(retained);
    quint64 checksum = 0;
    const auto read = [&](const KisCapturedReadView &view, const KisPageReadView &legacy) {
        for (int i = 0; i < workPages; ++i) {
            KisPageVersion version;
            const auto key = pageKey((i * 17) % pages);
            if (!(captured ? view.resolvePageVersion(key, &version) : store.resolvePageVersion(key, legacy, &version)))
                return false;
            checksum += version.generation.value;
        }
        return true;
    };
    const auto operation = [&]() {
        if (mode == "lookup")
            return read(held, selector);
        if (captured) {
            const auto view = store.captureReadView();
            return read(view, {});
        }
        const auto snapshot = store.captureRetainedEpoch();
        const bool valid = snapshot.isValid() && read({}, selectorFor(snapshot));
        return store.releaseSnapshot(snapshot.token) && valid;
    };
    for (int i = 0; i < 4; ++i)
        QVERIFY(operation());
    const auto before = store.readScopeStatistics();
    const auto requests = store.sessionStats().readRequestsCreated;
    qint64 completed = 0;
    bool correct = true;
    const auto cpuStart = std::clock();
    QElapsedTimer timer;
    timer.start();
    do {
        correct = operation() && correct;
        ++completed;
    } while (durationMs > 0 ? (completed % 64 != 0 || timer.nsecsElapsed() < durationMs * 1000000) : completed < 4);
    const auto wallNs = timer.nsecsElapsed();
    const auto cpuNs = qint64(double(std::clock() - cpuStart) * 1e9 / CLOCKS_PER_SEC);
    QVERIFY(correct && checksum > 0);
    const auto after = store.readScopeStatistics();
    QCOMPARE(store.sessionStats().readRequestsCreated, requests);
    if (captured)
        QCOMPARE(after.historicalGcPagesVisited, before.historicalGcPagesVisited);
    held = {};
    if (retained.isValid())
        QVERIFY(store.releaseSnapshot(retained.token));
    historyRoot = {};
    QCOMPARE(store.sessionStats().retainedSnapshots, qsizetype(0));
    QJsonObject result;
    result["fixture"] = QString::fromLatin1(QTest::currentDataTag());
    result["scope"] = mode == "lookup"
        ? "retained-identity-lookup; excludes-capture-release; no-pixels"
        : "identity-scope-lifecycle; capture-lookup-release; no-pixels; legacy-owning-manifest-included";
    result["measurement"] = durationMs ? "completed-throughput" : "correctness-short-run";
    result["completed_operations"] = completed;
    result["wall_ns"] = wallNs;
    result["process_cpu_ns"] = cpuNs;
    result["gc_pages_visited"] = qint64(after.historicalGcPagesVisited - before.historicalGcPagesVisited);
    result["captured_scopes"] = qint64(after.capturedReadViewsCreated - before.capturedReadViewsCreated);
    result["N"] = pages;
    result["K"] = workPages;
    result["H"] = history ? 1 : 0;
    result["correctness"] = "Pass";
    qInfo().noquote() << "READ_SCOPE" << QJsonDocument(result).toJson(QJsonDocument::Compact);
}

void KisPageStoreReferenceTest::capturedReadViewIsCoherentDuringConcurrentCommits()
{
    KisPageStore store;
    std::shared_ptr<KisCpuPageReplicaProvider> provider;
    QString error;
    QVERIFY(configureCapturedViewStore(store, &provider, &error));
    QVERIFY(commitCapturedViewFill(store, 2, 0x11, &error));
    std::atomic<bool> done{false}, valid{true};
    std::thread writer([&] {
        QString localError;
        for (int i = 0; i < 64; ++i)
            if (!commitCapturedViewFill(store, 2, quint8(0x40 + (i & 1)), &localError)) {
                valid.store(false);
                break;
            }
        done.store(true);
    });
    do {
        auto view = store.captureReadView();
        KisPageVersion first, second;
        if (!view.resolvePageVersion(pageKey(0), &first) || !view.resolvePageVersion(pageKey(1), &second)
            || first.generation.value != second.generation.value)
            valid.store(false);
    } while (!done.load());
    writer.join();
    QVERIFY(valid.load());
    QCOMPARE(store.sessionStats().retainedSnapshots, qsizetype(0));
}

void KisPageStoreReferenceTest::currentTransactionPinsBaseWithoutManifestExport()
{
    KisPageStore store;
    QVERIFY(!store.beginCurrentTransaction().isValid());
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig config;
    config.provider = KisReplicaProviderId{168};
    config.providerEpoch = KisReplicaProviderEpoch{1};
    config.budgetBytes = 1024 * 1024;
    QString error;
    QVERIFY2(provider->configure(config, completions, &error), qPrintable(error));
    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.surfaces = {surfaceEpochState()};
    QVERIFY(store.configure(initial, completions, 4, &error));
    QVERIFY(store.registerReplicaProvider(provider));
    QVERIFY(store.finalizeInitialization(&error));
    QVERIFY(!store.beginTransaction(KisImageEpochId{}).isValid());
    KisPageStoreDiagnosticRecorder recorder(true, &store);
    auto first = store.beginCurrentTransaction();
    QVERIFY(first.isValid());
    QCOMPARE(first.baseEpoch, initial.epoch);
    QVERIFY(KisPageStoreCpuSurfaceOps::fillRect(&store,
                                                KisSurfaceId{1},
                                                first,
                                                QRect(0, 0, 64, 64),
                                                QByteArray(1, char(0x31)),
                                                &error));
    const auto firstCommit = store.commit(first, store.preparedPages(first));
    QVERIFY(firstCommit.isValid());
    const auto oldBase = store.beginCurrentTransaction();
    QVERIFY(oldBase.isValid());
    QCOMPARE(oldBase.baseEpoch, firstCommit.epoch);
    const auto writer = store.beginCurrentTransaction();
    QVERIFY(KisPageStoreCpuSurfaceOps::fillRect(&store,
                                                KisSurfaceId{1},
                                                writer,
                                                QRect(0, 0, 64, 64),
                                                QByteArray(1, char(0x52)),
                                                &error));
    const auto secondCommit = store.commit(writer, store.preparedPages(writer));
    QVERIFY(secondCommit.isValid());
    const auto latest = store.beginCurrentTransaction();
    QVERIFY(latest.isValid());
    QCOMPARE(latest.baseEpoch, secondCommit.epoch);
    KisPageReadView baseView;
    baseView.kind = KisPageReadViewKind::TransactionBaseEpoch;
    baseView.transaction = oldBase.id;
    QByteArray oldBytes, newBytes;
    QVERIFY(KisPageStoreCpuSurfaceOps::readRect(&store,
                                                KisSurfaceId{1},
                                                baseView,
                                                QRect(0, 0, 1, 1),
                                                &oldBytes,
                                                0,
                                                &error));
    QVERIFY(KisPageStoreCpuSurfaceOps::readRect(&store, KisSurfaceId{1}, {}, QRect(0, 0, 1, 1), &newBytes, 0, &error));
    QCOMPARE(oldBytes, QByteArray(1, char(0x31)));
    QCOMPARE(newBytes, QByteArray(1, char(0x52)));
    QVERIFY(store.abort(oldBase));
    QVERIFY(store.abort(latest));
    QCOMPARE(recorder.metrics()[size_t(KisPageStoreDiagnosticPhase::ManifestExport)].intervals, quint64(0));
    QCOMPARE(recorder.metrics()[size_t(KisPageStoreDiagnosticPhase::TransactionBegin)].intervals, quint64(4));
}

void KisPageStoreReferenceTest::preparedMetadataPublicationIsBoundAndOneShot()
{
    using Batch = KisPageMetadataCoordinator::PreparedPublication;
    static_assert(!std::is_copy_constructible_v<Batch>);
    static_assert(std::is_nothrow_move_constructible_v<Batch>);
    const KisPageTransaction transaction{KisPageTransactionId{71}, KisImageEpochId{1}};
    KisPageMetadataCoordinator coordinator;
    KisPageMetadataCoordinator foreign;
    QVERIFY(coordinator.configure(8));
    QVERIFY(foreign.configure(8));
    QVector<KisPageTransition> transitions;
    for (int i = 0; i < 3; ++i) {
        auto page = initialPageState(pageVersion(i, 1), replica(pageVersion(i, 1), 1, 1, 1));
        const auto authority = replica(pageVersion(i, 2), 1, 1, 2);
        page.versions.append({pageVersion(i, 2),
                              KisPagePublicationState::Prepared,
                              {{authority, KisReplicaValidity::Valid, {}, {}, 0, {}}},
                              authority,
                              transaction.id});
        page.nextGeneration = KisPageGeneration{3};
        QVERIFY(coordinator.registerPage(page));
        QVERIFY(foreign.registerPage(page));
        if (i < 2) {
            KisPageTransition transition;
            transition.kind = KisPageTransitionKind::CommitTransaction;
            transition.transaction = transaction.id;
            transition.version = pageVersion(i, 2);
            transition.imageEpoch = KisImageEpochId{2};
            transitions.append(transition);
        }
    }
    auto make = [&]() {
        return coordinator.preparePublication(transaction, KisImageEpochId{2}, transitions);
    };
    KisPageMetadataCoordinator unconfigured;
    auto cold = make();
    QVERIFY(cold.isValid());
    QVERIFY(!unconfigured.installPublication(std::move(cold), transaction, KisImageEpochId{2}));
    QVERIFY(!cold.isValid());
    QVERIFY(unconfigured.configure(8));
    auto batch = make();
    QVERIFY(batch.isValid());
    Batch moved(std::move(batch));
    QVERIFY(!batch.isValid());
    QVERIFY(!foreign.installPublication(std::move(moved), transaction, KisImageEpochId{2}));
    QVERIFY(!moved.isValid());
    batch = make();
    auto wrong = transaction;
    wrong.id.value++;
    QVERIFY(!coordinator.installPublication(std::move(batch), wrong, KisImageEpochId{2}));
    batch = make();
    wrong = transaction;
    wrong.baseEpoch.value++;
    QVERIFY(!coordinator.installPublication(std::move(batch), wrong, KisImageEpochId{2}));
    batch = make();
    QVERIFY(!coordinator.installPublication(std::move(batch), transaction, KisImageEpochId{1}));
    auto duplicate = transitions;
    duplicate.append(transitions.first());
    QVERIFY(!coordinator.preparePublication(transaction, KisImageEpochId{2}, duplicate).isValid());
    auto invalid = transitions;
    invalid.last().version.generation.value = 100;
    QVERIFY(!coordinator.preparePublication(transaction, KisImageEpochId{2}, invalid).isValid());

    batch = make();
    QVERIFY(batch.isValid());
    QCOMPARE(coordinator.footprint().pageActivities, quint64(2));
    batch = {};
    QVERIFY(!batch.isValid());
    QCOMPARE(coordinator.footprint().pageActivities, quint64(0));

    // An unrelated page's read must not invalidate this K-page capability.
    batch = make();
    KisPageTransition read;
    read.kind = KisPageTransitionKind::AcquireRead;
    read.version = pageVersion(2, 1);
    read.target = replica(read.version, 1, 1, 1);
    read.lease = KisPageLeaseId{9};
    QVERIFY(coordinator.applyOwner(read.version.key, read).accepted);
    QVERIFY(coordinator.installPublication(std::move(batch), transaction, KisImageEpochId{7}));
    QVERIFY(!coordinator.installPublication(std::move(batch), transaction, KisImageEpochId{8}));
    for (int i = 0; i < 2; ++i) {
        KisPageStateSnapshot page;
        QVERIFY(coordinator.pageSnapshot(pageKey(i), &page));
        QCOMPARE(page.publishedEpoch.value, quint64(7));
        QCOMPARE(page.publishedGeneration.value, quint64(2));
        QVERIFY(KisPageStateMachine().validateInvariants(page));
    }
}

void KisPageStoreReferenceTest::preparedMetadataRejectsReaderLastUseAbortAndCompletionChanges()
{
    const KisPageTransaction transaction{KisPageTransactionId{72}, KisImageEpochId{1}};
    KisPageMetadataCoordinator coordinator;
    MetadataEffectReceiver receiver;
    attachEffects(coordinator, receiver);
    QVERIFY(coordinator.configure(8));
    QVector<KisPageTransition> transitions;
    for (int i = 0; i < 2; ++i) {
        auto page = initialPageState(pageVersion(i, 1), replica(pageVersion(i, 1), 1, 1, 1));
        const auto authority = replica(pageVersion(i, 2), 1, 1, 2);
        page.versions.append({pageVersion(i, 2),
                              KisPagePublicationState::Prepared,
                              {{authority, KisReplicaValidity::Valid, {}, {}, 0, {}}},
                              authority,
                              transaction.id});
        page.nextGeneration = KisPageGeneration{3};
        QVERIFY(coordinator.registerPage(page));
        KisPageTransition transition;
        transition.kind = KisPageTransitionKind::CommitTransaction;
        transition.transaction = transaction.id;
        transition.version = pageVersion(i, 2);
        transition.imageEpoch = KisImageEpochId{2};
        transitions.append(transition);
    }
    auto make = [&]() {
        return coordinator.preparePublication(transaction, KisImageEpochId{2}, transitions);
    };
    auto batch = make();
    auto reject = [&]() {
        const bool rejected =
            !coordinator.installPublication(std::move(batch), transaction, KisImageEpochId{2});
        KisPageStateSnapshot first, second;
        return rejected && !batch.isValid() && coordinator.pageSnapshot(pageKey(0), &first)
            && coordinator.pageSnapshot(pageKey(1), &second) && first.publishedGeneration.value == 1
            && second.publishedGeneration.value == 1;
    };
    KisPageTransition read;
    read.kind = KisPageTransitionKind::AcquireRead;
    read.version = pageVersion(1, 1);
    read.target = replica(read.version, 1, 1, 1);
    read.lease = KisPageLeaseId{10};
    bool acquired = false;
    std::thread reader([&]() {
        acquired = coordinator.applyOwner(read.version.key, read).accepted;
    });
    reader.join();
    QVERIFY(acquired);
    QVERIFY(reject()); // later page fails; the earlier claimed page is untouched
    batch = make();
    QVERIFY(batch.isValid());
    KisCompletionRegistry completions;
    const KisCompletionDomain source = KisCompletionDomain::HostLogical;
    const auto completionSource = completions.registerSource(source);
    const auto ticket = completions.allocatePending(completionSource);
    read.kind = KisPageTransitionKind::ReleaseRead;
    read.completion = ticket;
    const auto beforeRelease = coordinator.metrics();
    QVERIFY(coordinator.applyOwner(read.version.key, read).accepted);
    const auto afterRelease = coordinator.metrics();
    QCOMPARE(afterRelease.fullSnapshotExports, beforeRelease.fullSnapshotExports);
    QCOMPARE(afterRelease.localTransitionSequences, beforeRelease.localTransitionSequences + 1);
    QVERIFY(reject());
    batch = make();
    QVERIFY(completions.complete(ticket, KisCompletionStatus::Succeeded));
    KisPageTransition unverified;
    unverified.kind = KisPageTransitionKind::AcknowledgeLastUse;
    unverified.version = read.version;
    unverified.target = read.target;
    unverified.completion = ticket;
    QVERIFY(!coordinator.applyOwner(read.version.key, unverified).accepted);
    const auto beforeAcknowledge = coordinator.metrics();
    QVERIFY(coordinator.acknowledgeLastUse(read.version, read.target, completions.verifyTerminal(ticket)).accepted);
    const auto afterAcknowledge = coordinator.metrics();
    QCOMPARE(afterAcknowledge.fullSnapshotExports, beforeAcknowledge.fullSnapshotExports);
    QCOMPARE(afterAcknowledge.localTransitionSequences, beforeAcknowledge.localTransitionSequences + 1);
    QVERIFY(reject());

    batch = make();
    KisPageTransition abort;
    abort.kind = KisPageTransitionKind::AbortTransaction;
    abort.transaction = transaction.id;
    QVERIFY(coordinator.applyOwner(pageKey(1), abort).accepted);
    QVERIFY(reject());
}

void KisPageStoreReferenceTest::metadataInstallationPrecedesRootAndRejectsAtomically()
{
    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.manifest = {pageVersion(0, 1), pageVersion(1, 1)};
    KisImageEpochReferenceModel model;
    QVERIFY(model.initialize(initial));
    const auto original = model.captureCommittedRoot();
    const auto retainedOriginal = model.captureRetainedRoot();
    const auto transaction = model.beginTransaction(initial.epoch);
    KisCompletionRegistry completions;
    const KisCompletionDomain source = KisCompletionDomain::HostLogical;
    const auto ticket = completions.allocatePending(completions.registerSource(source));
    QVERIFY(completions.complete(ticket, KisCompletionStatus::Succeeded));
    KisPreparedPageSet proofs;
    proofs.transaction = transaction.id;
    KisPageMetadataCoordinator coordinator;
    QVERIFY(coordinator.configure(8));
    QVector<KisPageTransition> transitions;
    for (int i = 0; i < 2; ++i) {
        const auto version = pageVersion(i, 2);
        proofs.proofs.append(preparedProof(version, transaction.id, ticket, quint64(i + 1)));
        auto page = initialPageState(pageVersion(i, 1), replica(pageVersion(i, 1), 1, 1, 1));
        const auto authority = replica(version, 1, 1, 2);
        page.versions.append({version,
                              KisPagePublicationState::Prepared,
                              {{authority, KisReplicaValidity::Valid, {}, {}, 0, {}}},
                              authority,
                              transaction.id});
        page.nextGeneration = KisPageGeneration{3};
        QVERIFY(coordinator.registerPage(page));
        KisPageTransition transition;
        transition.kind = KisPageTransitionKind::CommitTransaction;
        transition.transaction = transaction.id;
        transition.version = version;
        transition.imageEpoch = KisImageEpochId{2};
        transitions.append(transition);
    }
    QVERIFY(model.prepare(proofs));
    auto batch = coordinator.preparePublication(transaction, KisImageEpochId{2}, transitions);
    KisPageTransition read;
    read.kind = KisPageTransitionKind::AcquireRead;
    read.version = pageVersion(1, 1);
    read.target = replica(read.version, 1, 1, 1);
    read.lease = KisPageLeaseId{20};
    QVERIFY(coordinator.applyOwner(read.version.key, read).accepted);
    auto install = [&](KisImageEpochId epoch) {
        return coordinator.installPublication(std::move(batch), transaction, epoch);
    };
    auto candidate = model.prepareCommit(transaction, nullptr);
    QVERIFY(candidate.isValid());
    QVERIFY(!model
                 .installCommit(std::move(candidate), &install, invokeEpochCallback<decltype(install)>)
                 .isCommitted());
    QCOMPARE(model.captureCommittedRoot().epoch().value, quint64(1));
    QCOMPARE(model.rootCount(), qsizetype(1));
    QCOMPARE(model.transaction(transaction.id).state, KisPageTransactionState::Prepared);
    KisPageStateSnapshot page;
    QVERIFY(coordinator.pageSnapshot(pageKey(0), &page));
    QCOMPARE(page.publishedGeneration.value, quint64(1));
    batch = coordinator.preparePublication(transaction, KisImageEpochId{2}, transitions);
    candidate = model.prepareCommit(transaction, nullptr);
    QVERIFY(candidate.isValid());
    const auto committed =
        model.installCommit(std::move(candidate), &install, invokeEpochCallback<decltype(install)>);
    QVERIFY(committed.isCommitted());
    QCOMPARE(committed.root.epoch().value, quint64(3)); // failed attempt burned 2
    for (int i = 0; i < 2; ++i) {
        QVERIFY(coordinator.pageSnapshot(pageKey(i), &page));
        QCOMPARE(page.publishedEpoch, committed.root.epoch());
        QCOMPARE(page.publishedGeneration.value, quint64(2));
        KisPageVersion old;
        QVERIFY(original.resolve(pageKey(i), &old));
        QCOMPARE(old.generation.value, quint64(1));
        QCOMPARE(page.versions.first().publication, KisPagePublicationState::Historical);
    }
    QCOMPARE(page.versions.first().replicas.first().readLeases, QVector<KisPageLeaseId>{read.lease});
    read.kind = KisPageTransitionKind::ReleaseRead;
    QVERIFY(coordinator.applyOwner(read.version.key, read).accepted);
    bool calledAfterCommit = false;
    candidate = model.prepareCommit(transaction, nullptr);
    QVERIFY(!model
                 .installCommit(std::move(candidate),
                                &calledAfterCommit,
                                [](void *context, KisImageEpochId) {
                                    *static_cast<bool *>(context) = true;
                                    return true;
                                })
                 .isCommitted());
    QVERIFY(!calledAfterCommit);

    // Undo/redo must obey the same metadata-before-root contract. A changed
    // second page rejects the complete restore, not just that page's update.
    QVector<KisPageTransition> restores;
    for (int i = 0; i < 2; ++i) {
        KisPageTransition restore;
        restore.kind = KisPageTransitionKind::RestoreCommittedVersion;
        restore.version = pageVersion(i, 1);
        restore.imageEpoch = {4};
        restores.append(restore);
    }
    QVERIFY(!coordinator.prepareRestoration({4}, transitions).isValid());
    QVERIFY(!coordinator.preparePublication({}, {4}, restores).isValid());
    auto restoration = coordinator.prepareRestoration({4}, restores);
    QVERIFY(restoration.isValid());
    read.kind = KisPageTransitionKind::AcquireRead;
    read.version = pageVersion(1, 2);
    read.target = replica(read.version, 1, 1, 2);
    read.lease = {21};
    QVERIFY(coordinator.applyOwner(read.version.key, read).accepted);
    bool completeMetadataInstalled = false;
    auto installRestore = [&](KisImageEpochId epoch) {
        if (!coordinator.installPublication(std::move(restoration), {}, epoch))
            return false;
        completeMetadataInstalled = true;
        for (int i = 0; i < 2; ++i) {
            KisPageStateSnapshot installed;
            completeMetadataInstalled = completeMetadataInstalled && coordinator.pageSnapshot(pageKey(i), &installed)
                && installed.publishedEpoch == epoch && installed.publishedGeneration.value == 1;
        }
        return true;
    };
    auto restoreCandidate = model.prepareRestore(retainedOriginal, nullptr);
    QVERIFY(restoreCandidate.isValid());
    QVERIFY(!model
                 .installRestore(std::move(restoreCandidate),
                                 &installRestore,
                                 invokeEpochCallback<decltype(installRestore)>)
                 .isCommitted());
    QVERIFY(!completeMetadataInstalled);
    QCOMPARE(model.captureCommittedRoot().epoch(), committed.root.epoch());
    QCOMPARE(model.rootCount(), qsizetype(2));
    QVERIFY(coordinator.pageSnapshot(pageKey(0), &page));
    QCOMPARE(page.publishedGeneration.value, quint64(2));
    restoration = coordinator.prepareRestoration({4}, restores);
    restoreCandidate = model.prepareRestore(retainedOriginal, nullptr);
    QVERIFY(restoreCandidate.isValid());
    const auto restored = model.installRestore(std::move(restoreCandidate),
                                               &installRestore,
                                               invokeEpochCallback<decltype(installRestore)>);
    QVERIFY(restored.isCommitted());
    QVERIFY(completeMetadataInstalled);
    QCOMPARE(restored.root.epoch().value, quint64(5));
    for (int i = 0; i < 2; ++i) {
        KisPageVersion version;
        QVERIFY(restored.root.resolve(pageKey(i), &version));
        QCOMPARE(version, pageVersion(i, 1));
        QVERIFY(coordinator.pageSnapshot(pageKey(i), &page));
        QCOMPARE(page.publishedGeneration.value, quint64(1));
        QVERIFY(KisPageStateMachine().validateInvariants(page));
    }
    read.kind = KisPageTransitionKind::ReleaseRead;
    QVERIFY(coordinator.applyOwner(read.version.key, read).accepted);
    QVERIFY(model.releaseSnapshot(retainedOriginal.token));
}

void KisPageStoreReferenceTest::epochTransactionStorageAtCapacity()
{
    QFETCH(bool, complete); QFETCH(int, refusal);
    KisPageBackingLimits limits; limits.metadataArenaBytes = 1024 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    KisBackingBudgetController budget;
    QVERIFY(budget.configureSharedNonPayloadBudget(parent));
    KisImageEpochReferenceModel model;
    model.attachBackingBudget(budget);
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.manifest = {pageVersion(0, 1), pageVersion(1, 1), pageVersion(2, 1)};
    initial.surfaces = {surfaceEpochState()};
    if (refusal == 2)
        for (int i = 2; i <= 512; ++i) initial.surfaces.append(surfaceEpochState({quint64(i)}));
    if (refusal == 3)
        for (int i = 3; i < 512; ++i) initial.manifest.append(pageVersion(i, 1));
    QVERIFY(model.initialize(initial));
    const auto tx = model.beginTransaction({1});
    KisCompletionRegistry completions;
    const auto ticket = completions.allocatePending(completions.registerSource(KisCompletionDomain::HostLogical));
    QVERIFY(completions.complete(ticket, KisCompletionStatus::Succeeded));
    KisPreparedPageSet original;
    original.transaction = tx.id;
    original.proofs = {preparedProof(pageVersion(0, 2), tx.id, ticket, 1)};
    original.surfaceChanges = {{initial.surfaces.first(), surfaceEpochState({1}, 2, 0x19)}};
    original.removedPages = {pageKey(1)};
    const auto live = [&] { return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const auto before = live();
    QVERIFY(model.prepare(original));
    QVERIFY(live() > before);
    const auto accepted = model.transaction(tx.id);
    KisPreparedPageSet changed = complete ? original : KisPreparedPageSet{};
    changed.transaction = tx.id;
    changed.proofs = {preparedProof(pageVersion(complete ? 0 : 2, complete ? 3 : 2), tx.id, ticket, 2)};
    if (refusal == 1)
        for (int i = 3; i < 512; ++i) changed.proofs.append(preparedProof(pageVersion(i, 2), tx.id, ticket, quint64(i)));
    if (refusal == 2)
        for (int i = 1; i < initial.surfaces.size(); ++i)
            changed.surfaceChanges.append({initial.surfaces[i], surfaceEpochState({quint64(i + 1)}, 2, 0x19)});
    if (refusal == 3)
        for (int i = 3; i < 512; ++i) changed.removedPages.append(pageKey(i));
    const auto acceptedBytes = live();
    // For array cases the original shared value is admitted first; only the
    // selected large real capacity refuses. All partial charges must unwind.
    const quint64 headroom = refusal ? 2048 : 0;
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - live() - headroom);
    void *filler = kisAllocateMutationStorage(parent.get(), fillerBytes, 1);
    const auto freeFiller = qScopeGuard([&] { if (filler) kisFreeMutationStorage(parent.get(), filler, fillerBytes, 1); });
    QCOMPARE(live(), limits.metadataArenaBytes - headroom);
    QCOMPARE(model.activeTransaction(tx.id), tx); // Native queries export no arrays.
    QVERIFY(model.preparePublication(original)); // Identical full facts reuse the original storage.
    QString error;
    QVERIFY(!(complete ? model.preparePublication(changed, &error) : model.prepare(changed, &error)));
    QVERIFY(error.contains(QStringLiteral("storage admission")));
    QCOMPARE(model.activeTransaction(tx.id), tx);
    QCOMPARE(live(), limits.metadataArenaBytes - headroom);
    kisFreeMutationStorage(parent.get(), filler, fillerBytes, 1); filler = nullptr;
    QCOMPARE(live(), acceptedBytes);
    const auto refused = model.transaction(tx.id);
    QCOMPARE(refused.changes, accepted.changes);
    QCOMPARE(refused.surfaceChanges, accepted.surfaceChanges);
    QCOMPARE(refused.removedPages, accepted.removedPages);
    QVERIFY(complete ? model.preparePublication(changed) : model.prepare(changed));
    auto candidate = model.prepareCommit(tx, nullptr);
    QVERIFY(candidate.isValid());
    const auto preparedBytes = live();
    const size_t installFillerBytes = size_t(limits.metadataArenaBytes - live());
    void *installFiller = kisAllocateMutationStorage(parent.get(), installFillerBytes, 1);
    const auto freeInstallFiller = qScopeGuard([&] { kisFreeMutationStorage(parent.get(), installFiller, installFillerBytes, 1); });
    if (complete) QVERIFY(model.preparePublication(changed)); // Preserve the prepared root's revision.
    QVERIFY(model.installCommit(std::move(candidate), nullptr, nullptr).isCommitted());
    QCOMPARE(live(), limits.metadataArenaBytes);
    // The original committed record remains charged until collection.
    QCOMPARE(model.collectFinishedTransactions(0), qsizetype(0));
    QCOMPARE(live(), limits.metadataArenaBytes);
    QCOMPARE(model.collectFinishedTransactions(1), qsizetype(1));
    QVERIFY(live() < limits.metadataArenaBytes);
    QVERIFY(live() - installFillerBytes < preparedBytes);
}

void KisPageStoreReferenceTest::epochTransactionStorageRetainsOriginalOwner()
{
    KisPageBackingLimits limits; limits.metadataArenaBytes = 1024 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    std::array<KisBackingBudgetReservation, 8> slots;
    for (auto &slot : slots) { slot = parent->reserve({}, nullptr); QVERIFY(slot.isValid()); }
    for (auto &slot : slots) slot.release();
    const auto live = [&] { return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const auto baseline = live();
    KisImageEpochReferenceModel::PreparedCommit orphan;
    auto budget = std::make_unique<KisBackingBudgetController>();
    QVERIFY(budget->configureSharedNonPayloadBudget(parent));
    auto model = std::make_unique<KisImageEpochReferenceModel>();
    model->attachBackingBudget(*budget);
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.manifest = {pageVersion(0, 1)};
    QVERIFY(model->initialize(initial));
    const auto tx = model->beginTransaction({1});
    KisPreparedPageSet delta; delta.transaction = tx.id; delta.removedPages = {pageKey(0)};
    QVERIFY(model->prepare(delta));
    orphan = model->prepareCommit(tx, nullptr);
    QVERIFY(orphan.isValid());
    model.reset(); budget.reset();
    QVERIFY(live() > baseline); // Original immutable value/control retains its accounting owner.
    orphan = {};
    kisDrainPageStoreReclamation();
    QCOMPARE(live(), baseline);
}

void KisPageStoreReferenceTest::epochTransactionPreparationRevalidatesConcurrentRevision()
{
    KisPageBackingLimits limits; limits.metadataArenaBytes = 8 * 1024 * 1024;
    KisBackingBudgetController budget(limits);
    const auto storage = KisMutationStorageAllocator<KisPageVersion>::retained(&budget);
    KisImageEpochReferenceModel model; model.attachBackingBudget(budget);
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.surfaces = {surfaceEpochState()};
    QVERIFY(model.initialize(initial));
    const auto baseline = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    const auto tx = model.beginTransaction({1});
    const auto live = [&] { return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const auto before = live();
    KisCompletionRegistry completions;
    const auto ticket = completions.allocatePending(completions.registerSource(KisCompletionDomain::HostLogical));
    QVERIFY(completions.complete(ticket, KisCompletionStatus::Succeeded));
    KisPreparedPageSet input; input.transaction = tx.id;
    for (int i = 0; i < 32768; ++i) input.proofs.append(preparedProof(pageVersion(i, 2), tx.id, ticket, quint64(i + 1)));
    std::atomic<bool> start{false};
    std::array<bool, 2> accepted{};
    std::array<QString, 2> errors;
    const auto prepare = [&](int index) {
        while (!start.load()) std::this_thread::yield();
        accepted[size_t(index)] = model.prepare(input, &errors[size_t(index)]);
    };
    std::thread first(prepare, 0), second(prepare, 1);
    start = true;
    first.join(); second.join();
    QCOMPARE(int(accepted[0]) + int(accepted[1]), 1);
    QVERIFY(!errors[accepted[0] ? 1 : 0].isEmpty());
    const auto bytes = quint64(input.proofs.size()) * sizeof(KisPageVersion);
    QVERIFY(live() - before >= bytes && live() - before < bytes + 512);
    QCOMPARE(model.transaction(tx.id).changes.size(), input.proofs.size());
    QVERIFY(model.abort(tx));
    QCOMPARE(model.collectFinishedTransactions(), qsizetype(1));
    QCOMPARE(live(), baseline); // Original record, losing work and accepted data physically freed.
}

void KisPageStoreReferenceTest::epochTransactionRejectsOpposingIncrementalChanges()
{
    QFETCH(bool, firstRemoval); QFETCH(bool, complete);
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.manifest = {pageVersion(0, 1)};
    KisImageEpochReferenceModel model; QVERIFY(model.initialize(initial));
    const auto tx = model.beginTransaction({1});
    KisCompletionRegistry completions;
    const auto ticket = completions.allocatePending(completions.registerSource(KisCompletionDomain::HostLogical));
    QVERIFY(completions.complete(ticket, KisCompletionStatus::Succeeded));
    KisPreparedPageSet written; written.transaction = tx.id;
    written.proofs = {preparedProof(pageVersion(0, 2), tx.id, ticket, 1)};
    KisPreparedPageSet removed; removed.transaction = tx.id; removed.removedPages = {pageKey(0)};
    QVERIFY(model.prepare(firstRemoval ? removed : written));
    const auto &next = firstRemoval ? written : removed;
    QCOMPARE(complete ? model.preparePublication(next) : model.prepare(next), complete);
    const auto saved = model.transaction(tx.id);
    const bool endsRemoved = complete ? !firstRemoval : firstRemoval;
    QCOMPARE(saved.changes.isEmpty(), endsRemoved);
    QCOMPARE(saved.removedPages.isEmpty(), !endsRemoved);
    QVERIFY(model.commit(tx).isCommitted());
    KisPageVersion version;
    QCOMPARE(model.captureCommittedRoot().containsPage(pageKey(0), &version), !endsRemoved);
    if (!endsRemoved) QCOMPARE(version, pageVersion(0, 2));
}

void KisPageStoreReferenceTest::preparedEpochCandidateIsInvisibleAndSingleUse()
{
    using Candidate = KisImageEpochReferenceModel::PreparedCommit;
    static_assert(!std::is_copy_constructible<Candidate>::value, "candidate is single-use");
    static_assert(!std::is_copy_constructible<KisImageEpochReferenceModel>::value, "owner cannot be copied");
    static_assert(std::is_nothrow_move_constructible<Candidate>::value, "move cannot fail");
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.manifest = {pageVersion(0, 1)};
    KisImageEpochReferenceModel model;
    QVERIFY(model.initialize(initial));
    const auto tx = model.beginTransaction({1});
    KisCompletionRegistry completions;
    const KisCompletionDomain source = KisCompletionDomain::HostLogical;
    const auto ticket = completions.allocatePending(completions.registerSource(source));
    QVERIFY(completions.complete(ticket, KisCompletionStatus::Succeeded));
    KisPreparedPageSet pages;
    pages.transaction = tx.id;
    pages.proofs = {preparedProof(pageVersion(0, 2), tx.id, ticket, 1)};
    QVERIFY(model.prepare(pages));
    KisImageEpochCommitResult failure;
    auto candidate = model.prepareCommit(tx, &failure);
    QVERIFY2(candidate.isValid(), qPrintable(failure.error));
    QCOMPARE(model.rootCount(), qsizetype(1));
    QCOMPARE(model.captureCommittedRoot().epoch(), initial.epoch);
    QVERIFY(!model.root({2}).isValid());
    QVERIFY(!model.retainSnapshot({2}).isValid());
    QVERIFY(!model.beginTransaction({2}).isValid());
    QCOMPARE(model.collectUnretainedRoots(), qsizetype(0));
    QCOMPARE(model.reachablePageVersions({pageKey(0)}), QSet<KisPageVersion>{pageVersion(0, 1)});
    auto moved = std::move(candidate);
    QVERIFY(!candidate.isValid());
    int calls = 0;
    auto install = [&](KisImageEpochId epoch) {
        ++calls;
        return epoch == KisImageEpochId{2};
    };
    const auto installed =
        model.installCommit(std::move(moved), &install, invokeEpochCallback<decltype(install)>);
    QVERIFY(installed.isCommitted());
    QVERIFY(!moved.isValid());
    QCOMPARE(calls, 1);
    QCOMPARE(installed.root.epoch(), KisImageEpochId{2});
    QCOMPARE(model.rootCount(), qsizetype(2));
    auto reinstall = [&](KisImageEpochId) {
        ++calls;
        return true;
    };
    QVERIFY(!model
                 .installCommit(std::move(moved),
                                &reinstall,
                                invokeEpochCallback<decltype(reinstall)>)
                 .isCommitted());
    QCOMPARE(calls, 1);
    KisPageVersion actual;
    QVERIFY(model.captureCommittedRoot().resolve(pageKey(0), &actual));
    QCOMPARE(actual, pageVersion(0, 2));
}

void KisPageStoreReferenceTest::preparedEpochCandidateRejectsChangedInputs_data()
{
    QTest::addColumn<int>("change");
    QTest::newRow("reprepare") << 0;
    QTest::newRow("abort-and-collect") << 1;
    QTest::newRow("intervening-disjoint-commit") << 2;
    QTest::newRow("metadata-reject") << 3;
}

void KisPageStoreReferenceTest::preparedEpochCandidateRejectsChangedInputs()
{
    QFETCH(int, change);
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.manifest = {pageVersion(0, 1), pageVersion(1, 1)};
    KisImageEpochReferenceModel model;
    QVERIFY(model.initialize(initial));
    KisCompletionRegistry completions;
    const KisCompletionDomain source = KisCompletionDomain::HostLogical;
    const auto ticket = completions.allocatePending(completions.registerSource(source));
    QVERIFY(completions.complete(ticket, KisCompletionStatus::Succeeded));
    const auto tx = model.beginTransaction({1});
    KisPreparedPageSet pages;
    pages.transaction = tx.id;
    pages.proofs = {preparedProof(pageVersion(0, 2), tx.id, ticket, 1)};
    QVERIFY(model.prepare(pages));
    auto candidate = model.prepareCommit(tx, nullptr);
    QVERIFY(candidate.isValid());
    if (change == 0) {
        pages.proofs = {preparedProof(pageVersion(0, 3), tx.id, ticket, 2)};
        QVERIFY(model.prepare(pages));
    } else if (change == 1) {
        QVERIFY(model.abort(tx));
        QCOMPARE(model.collectFinishedTransactions(), qsizetype(1));
    } else if (change == 2) {
        const auto other = model.beginTransaction({1});
        KisPreparedPageSet otherPages;
        otherPages.transaction = other.id;
        otherPages.proofs = {preparedProof(pageVersion(1, 2), other.id, ticket, 3)};
        QVERIFY(model.prepare(otherPages));
        QVERIFY(model.commit(other).isCommitted());
    }
    const auto before = model.captureCommittedRoot();
    const auto roots = model.rootCount();
    int calls = 0;
    auto reject = [&](KisImageEpochId) {
        ++calls;
        return false;
    };
    const auto rejected =
        model.installCommit(std::move(candidate), &reject, invokeEpochCallback<decltype(reject)>);
    QVERIFY(!rejected.isCommitted());
    QVERIFY(!candidate.isValid());
    QCOMPARE(calls, change == 3 ? 1 : 0);
    QCOMPARE(rejected.status, change == 3 ? KisImageEpochCommitStatus::Rejected : KisImageEpochCommitStatus::Conflict);
    QCOMPARE(model.rootCount(), roots);
    QCOMPARE(model.captureCommittedRoot().epoch(), before.epoch());
    QVERIFY(!model.root({2}).isValid());
    KisPageVersion actual;
    QVERIFY(model.captureCommittedRoot().resolve(pageKey(0), &actual));
    QCOMPARE(actual, pageVersion(0, 1));
    if (change != 1) {
        const auto retry = model.commit(tx);
        QVERIFY2(retry.isCommitted(), qPrintable(retry.error));
        QVERIFY(retry.root.resolve(pageKey(0), &actual));
        QCOMPARE(actual, pageVersion(0, change == 0 ? 3 : 2));
    }
}

void KisPageStoreReferenceTest::preparedEpochCandidateOwnerAndLifetime()
{
    using Candidate = KisImageEpochReferenceModel::PreparedCommit;
    Candidate orphan;
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.manifest = {pageVersion(0, 1)};
    KisCompletionRegistry completions;
    const KisCompletionDomain source = KisCompletionDomain::HostLogical;
    const auto ticket = completions.allocatePending(completions.registerSource(source));
    QVERIFY(completions.complete(ticket, KisCompletionStatus::Succeeded));
    {
        KisImageEpochReferenceModel model;
        QVERIFY(model.initialize(initial));
        const auto tx = model.beginTransaction({1});
        KisPreparedPageSet pages;
        pages.transaction = tx.id;
        pages.proofs = {preparedProof(pageVersion(0, 2), tx.id, ticket, 1)};
        QVERIFY(model.prepare(pages));
        orphan = model.prepareCommit(tx, nullptr);
        QVERIFY(orphan.isValid());
        // Move assignment must cancel its previously reserved slot.
        orphan = model.prepareCommit(tx, nullptr);
        QVERIFY(orphan.isValid());
        QCOMPARE(model.rootCount(), qsizetype(1));
        QCOMPARE(model.collectUnretainedRoots(), qsizetype(0));
    }
    QVERIFY(orphan.isValid());
    KisImageEpochReferenceModel foreign;
    QVERIFY(foreign.initialize(initial));
    int calls = 0;
    auto install = [&](KisImageEpochId) {
        ++calls;
        return true;
    };
    QVERIFY(!foreign
                 .installCommit(std::move(orphan),
                                &install,
                                invokeEpochCallback<decltype(install)>)
                 .isCommitted());
    QCOMPARE(calls, 0);
    QVERIFY(!orphan.isValid());
    QCOMPARE(foreign.rootCount(), qsizetype(1));
}

void KisPageStoreReferenceTest::rootCollectionTracksReferencesIncrementally()
{
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.manifest = {pageVersion(0, 1)};
    initial.surfaces = {surfaceEpochState(), surfaceEpochState({2}, 3, 0x19), surfaceEpochState({3}, 5, 0x28)};
    KisImageEpochReferenceModel model;
    QVERIFY(model.initialize(initial));
    const auto retained = model.captureRetainedRoot();
    QVERIFY(retained.isValid());
    const auto first = model.beginTransaction({1});
    const auto second = model.beginTransaction({1});
    const auto writer = model.beginTransaction({1});
    QCOMPARE(model.activeTransactionCount(), qsizetype(3));
    KisCompletionRegistry completions;
    const KisCompletionDomain source = KisCompletionDomain::HostLogical;
    const auto ticket = completions.allocatePending(completions.registerSource(source));
    QVERIFY(completions.complete(ticket, KisCompletionStatus::Succeeded));
    KisPreparedPageSet delta;
    delta.transaction = writer.id;
    delta.proofs = {preparedProof(pageVersion(0, 2), writer.id, ticket, 1)};
    QVERIFY(model.prepare(delta));
    QVERIFY(model.commit(writer).isCommitted());
    QCOMPARE(model.activeTransactionCount(), qsizetype(2));
    QCOMPARE(model.collectFinishedTransactions(), qsizetype(1));
    QCOMPARE(model.collectUnretainedRoots(), qsizetype(0));
    QVERIFY(model.releaseSnapshot(retained.token));
    QCOMPARE(model.collectUnretainedRoots(), qsizetype(0));
    // The last token is gone, but active transactions still own their base.
    KisPageVersion baseVersion;
    QVERIFY(model.resolve(pageKey(0), KisPageReadView::transactionBase(first.id), &baseVersion));
    QCOMPARE(baseVersion, pageVersion(0, 1));
    delta.transaction = first.id;
    delta.proofs.clear();
    delta.removedPages = {pageKey(0)};
    QVERIFY(model.prepare(delta));
    QCOMPARE(model.commit(first).status, KisImageEpochCommitStatus::Conflict);
    QVERIFY(model.abort(first));
    QCOMPARE(model.activeTransactionCount(), qsizetype(1));
    QCOMPARE(model.collectUnretainedRoots(), qsizetype(0));
    QVERIFY(model.abort(second));
    QCOMPARE(model.activeTransactionCount(), qsizetype(0));
    QCOMPARE(model.collectUnretainedRoots(), qsizetype(1));
    QVERIFY(!model.root({1}).isValid());
    QCOMPARE(model.rootCount(), qsizetype(1));
    QCOMPARE(model.collectFinishedTransactions(), qsizetype(2));
    QCOMPARE(model.collectFinishedTransactions(), qsizetype(0));
    const auto current = model.captureRetainedRoot();
    const auto beforeRestore = model.captureCommittedRoot();
    auto restore = model.prepareRestore(current, nullptr);
    const auto restored = model.installRestore(std::move(restore), nullptr, nullptr);
    QVERIFY(restored.isCommitted());
    QVERIFY(restored.root.isValid());
    QCOMPARE(restored.root.previousEpoch(), beforeRestore.epoch());
    QCOMPARE(restored.root.commitSequence(), beforeRestore.commitSequence() + 1);
    QCOMPARE(restored.root.surfaces(), beforeRestore.surfaces());
    QCOMPARE(restored.root.manifest(), beforeRestore.manifest());
    QCOMPARE(restored.root.graphRevision(), beforeRestore.graphRevision());
    QCOMPARE(restored.root.defaultPixelRevision(), beforeRestore.defaultPixelRevision());
    QCOMPARE(restored.root.extentRevision(), beforeRestore.extentRevision());
    QCOMPARE(restored.root.propertyRevision(), beforeRestore.propertyRevision());
    QCOMPARE(model.collectUnretainedRoots(), qsizetype(0));
    QVERIFY(model.releaseSnapshot(current.token));
    QCOMPARE(model.collectUnretainedRoots(), qsizetype(1));
    const auto live = model.captureRetainedRoot();
    KisSurfaceEpochState state;
    KisPageReadView invalidView;
    invalidView.kind = KisPageReadViewKind::CommittedEpoch;
    QVERIFY(!model.surfaceState({1}, invalidView, &state));
    invalidView.kind = KisPageReadViewKind::ExactVersion;
    QVERIFY(!model.surfaceState({1}, invalidView, &state));
    QVERIFY(model.releaseSnapshot(live.token));
    QCOMPARE(model.collectUnretainedRoots(), qsizetype(0));
    QCOMPARE(model.rootCount(), qsizetype(1));
}

void KisPageStoreReferenceTest::retirementQueuesPreserveBudgetAndIdentity()
{
    QFETCH(int, epochs);
    QFETCH(bool, transactionsFirst);
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.surfaces = {surfaceEpochState()};
    KisImageEpochReferenceModel model;
    QVERIFY(model.initialize(initial));
    QVector<KisPageTransaction> finished;
    QVector<KisRetainedImageEpochSnapshot> retained;
    for (int i = 0; i < epochs; ++i) {
        const auto base = model.captureCommittedRoot();
        if (i % 7 == 0) {
            retained.append(model.captureRetainedRoot());
            QVERIFY(retained.back().isValid());
        }
        // Repeatedly queue the same current root before publishing it. Root
        // links must deduplicate; each independent terminal transaction must
        // remain in FIFO order. Registry growth relocates QHash values.
        for (int n = 0; n < 3; ++n) {
            const auto tx = model.beginTransaction(base.epoch());
            QVERIFY(tx.isValid());
            QVERIFY(model.abort(tx));
            finished.append(tx);
        }
        const auto tx = model.beginTransaction(base.epoch());
        QVERIFY(tx.isValid());
        KisSurfaceEpochState before;
        QVERIFY(base.surfaceState({1}, &before));
        auto after = before;
        after.contentExtent.translate(1, 0);
        ++after.extentRevision;
        KisPreparedPageSet delta;
        delta.transaction = tx.id;
        delta.surfaceChanges = {{before, after}};
        QVERIFY(delta.surfaceChanges.first().isValid());
        QVERIFY(model.prepare(delta));
        KisImageEpochCommitResult failure;
        // Erasing a reserved hash slot may relocate queued records too. A
        // rejected metadata callback must neither enqueue the candidate nor
        // finish the still-live transaction.
        auto rejected = model.prepareCommit(tx, &failure);
        QVERIFY(rejected.isValid());
        QVERIFY(!model
                     .installCommit(std::move(rejected),
                                    nullptr,
                                    [](void *, KisImageEpochId) {
                                        return false;
                                    })
                     .isCommitted());
        QCOMPARE(model.transaction(tx.id).state, KisPageTransactionState::Prepared);
        QCOMPARE(model.captureCommittedRoot().epoch(), base.epoch());
        auto cancelled = model.prepareCommit(tx, &failure);
        QVERIFY(cancelled.isValid());
        auto candidate = model.prepareCommit(tx, &failure);
        QVERIFY(candidate.isValid());
        QVERIFY(model.installCommit(std::move(candidate), nullptr, nullptr).isCommitted());
        cancelled = {}; // Unpublished reservation can outlive another install.
        finished.append(tx);
        QCOMPARE(model.rootCount(), qsizetype(i + 2));
    }
    QCOMPARE(model.activeTransactionCount(), qsizetype(0));
    QCOMPARE(model.collectFinishedTransactions(0), qsizetype(0));
    QCOMPARE(model.collectUnretainedRoots(0), qsizetype(0));
    auto collectTransactions = [&] {
        int consumed = 0;
        while (consumed < finished.size()) {
            const int budget = consumed % 3 == 0 ? 1 : 32;
            const int count = qMin(budget, int(finished.size()) - consumed);
            QCOMPARE(model.collectFinishedTransactions(budget), qsizetype(count));
            for (int i = consumed; i < consumed + count; ++i)
                QVERIFY(!model.transaction(finished.at(i).id).isValid());
            consumed += count;
            if (consumed < finished.size())
                QVERIFY(model.transaction(finished.at(consumed).id).isValid());
        }
        QCOMPARE(model.collectFinishedTransactions(), qsizetype(0));
    };
    if (transactionsFirst)
        collectTransactions();
    int cursor = 0;
    while (cursor < epochs) {
        const int budget = cursor % 3 == 0 ? 1 : 32;
        const int end = qMin(cursor + budget, epochs);
        int eligible = 0;
        for (int i = cursor; i < end; ++i)
            if (i % 7)
                ++eligible;
        // The budget counts examined roots, including still-protected roots.
        QCOMPARE(model.collectUnretainedRoots(budget), qsizetype(eligible));
        cursor = end;
    }
    QCOMPARE(model.collectUnretainedRoots(), qsizetype(0));
    QCOMPARE(model.rootCount(), qsizetype(retained.size() + 1));
    if (!transactionsFirst)
        collectTransactions();
    QVERIFY(!model.hasCollectionWork());
    // Previously examined protected records must be able to re-enter a now
    // empty queue; release order intentionally differs from publication order.
    for (int i = retained.size() - 1; i >= 0; --i) {
        QVERIFY(model.releaseSnapshot(retained.at(i).token));
        QVERIFY(!model.root(retained.at(i).snapshot.epoch).isValid());
    }
    for (int i = 0; i < retained.size(); ++i)
        QCOMPARE(model.collectUnretainedRoots(1), qsizetype(1));
    QCOMPARE(model.rootCount(), qsizetype(1));
    QVERIFY(!model.hasCollectionWork());
    // A queued current root is examined but never removed. Both empty tails
    // must reset correctly before another independent transaction is queued.
    for (int i = 0; i < 3; ++i) {
        const auto tx = model.beginTransaction(model.captureCommittedRoot().epoch());
        QVERIFY(tx.isValid());
        QVERIFY(model.abort(tx));
        QCOMPARE(model.collectUnretainedRoots(1), qsizetype(0));
        QCOMPARE(model.collectFinishedTransactions(1), qsizetype(1));
        QVERIFY(!model.hasCollectionWork());
        QCOMPARE(model.rootCount(), qsizetype(1));
    }
}

void KisPageStoreReferenceTest::rootAdmissionEndsBeforeCollection_data()
{
    QTest::addColumn<int>("protection");
    QTest::addColumn<bool>("restore");
    for (int p = 0; p < 4; ++p)
        for (bool r : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("protection%1-restore%2").arg(p).arg(r))) << p << r;
}

void KisPageStoreReferenceTest::rootAdmissionEndsBeforeCollection()
{
    QFETCH(int, protection);
    QFETCH(bool, restore);
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.manifest = {pageVersion(0, 1), pageVersion(1, 1)};
    KisImageEpochReferenceModel model;
    QVERIFY(model.initialize(initial));
    const auto raw = model.captureCommittedRoot(); // Metadata copy is not retention.
    auto retained = (protection & 1) ? model.captureRetainedRoot() : KisRetainedImageEpochSnapshot{};
    KisPageTransaction active;
    if (!restore && (protection & 2))
        active = model.beginTransaction({1});
    if (restore) {
        // Restore rejects active transactions. Acquire the protected historical
        // base after restoration while its explicit token still admits it.
        const auto source = model.captureRetainedRoot();
        auto candidate = model.prepareRestore(source, nullptr);
        QVERIFY(model.installRestore(std::move(candidate), nullptr, nullptr).isCommitted());
        if (protection & 2)
            active = model.beginTransaction({1});
        QVERIFY(model.releaseSnapshot(source.token));
    } else {
        const auto writer = model.beginTransaction({1});
        KisPreparedPageSet delta;
        delta.transaction = writer.id;
        delta.removedPages = {pageKey(1)};
        QVERIFY(model.prepare(delta));
        QVERIFY(model.commit(writer).isCommitted());
    }
    QCOMPARE(model.rootCount(), qsizetype(2)); // No collector has run.
    QCOMPARE(model.root({1}).isValid(), protection != 0);
    if (protection) {
        const auto clone = model.retainSnapshot({1});
        QVERIFY(clone.isValid());
        const auto adopted = model.beginTransaction({1});
        QVERIFY(adopted.isValid());
        if (retained.isValid())
            QVERIFY(model.releaseSnapshot(retained.token));
        if (active.isValid())
            QVERIFY(model.abort(active));
        QVERIFY(model.root({1}).isValid());
        QVERIFY(model.releaseSnapshot(clone.token));
        QVERIFY(model.root({1}).isValid()); // Transaction still protects base.
        QVERIFY(model.abort(adopted));
    }
    QVERIFY(!model.root({1}).isValid());
    QVERIFY(!model.retainSnapshot({1}).isValid());
    QString error;
    QVERIFY(!model.beginTransaction({1}, &error).isValid());
    QVERIFY(!error.isEmpty());
    QCOMPARE(model.rootCount(), qsizetype(2));
    KisPageVersion version;
    QVERIFY(raw.resolve(pageKey(1), &version));
    QCOMPARE(version, pageVersion(1, 1));
    // A saved structural copy never resurrects pixel history, even after a
    // rejected metadata install on a candidate for the current root.
    const auto current = model.captureCommittedRoot();
    const auto tx = model.beginTransaction(current.epoch());
    QVERIFY(tx.isValid());
    KisPreparedPageSet delta;
    delta.transaction = tx.id;
    delta.removedPages = {pageKey(0)};
    QVERIFY(model.prepare(delta));
    auto rejected = model.prepareCommit(tx, nullptr);
    QVERIFY(rejected.isValid());
    QVERIFY(!model
                 .installCommit(std::move(rejected),
                                nullptr,
                                [](void *, KisImageEpochId) {
                                    return false;
                                })
                 .isCommitted());
    QVERIFY(model.root(current.epoch()).isValid());
    QVERIFY(!model.retainSnapshot({1}).isValid());
    QVERIFY(model.abort(tx));
    QCOMPARE(model.collectUnretainedRoots(0), qsizetype(0));
    QCOMPARE(model.rootCount(), qsizetype(2));
    QCOMPARE(model.collectUnretainedRoots(), qsizetype(1));
    QCOMPARE(model.rootCount(), qsizetype(1));
}

void KisPageStoreReferenceTest::reachabilitySkipsRetiredRoots_data()
{
    QTest::addColumn<int>("history");
    for (int n : {0, 7, 127, 1023})
        QTest::newRow(qPrintable(QString::number(n))) << n;
}

void KisPageStoreReferenceTest::reachabilitySkipsRetiredRoots()
{
    QFETCH(int, history);
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.manifest = {pageVersion(0, 1), pageVersion(1, 1)};
    KisImageEpochReferenceModel model;
    QVERIFY(model.initialize(initial));
    QVector<KisRetainedImageEpochSnapshot> snapshots;
    QVector<KisPageTransaction> transactions;
    for (int i = 0; i < 64; ++i) {
        snapshots.append(model.captureRetainedRoot());
        QVERIFY(snapshots.back().isValid());
        transactions.append(model.beginTransaction({1}));
        QVERIFY(transactions.back().isValid());
    }
    KisCompletionRegistry completions;
    const KisCompletionDomain source = KisCompletionDomain::HostLogical;
    const auto ticket = completions.allocatePending(completions.registerSource(source));
    QVERIFY(completions.complete(ticket, KisCompletionStatus::Succeeded));
    auto publish = [&](quint64 generation) {
        const auto tx = model.beginTransaction(model.captureCommittedRoot().epoch());
        KisPreparedPageSet delta;
        delta.transaction = tx.id;
        delta.proofs = {preparedProof(pageVersion(0, generation), tx.id, ticket, generation)};
        return model.prepare(delta) && model.commit(tx).isCommitted();
    };
    QVERIFY(publish(2));
    const auto second = model.captureRetainedRoot();
    QVERIFY(second.isValid());
    for (int n = 0; n <= history; ++n)
        QVERIFY(publish(quint64(n + 3)));
    QCOMPARE(model.rootCount(), qsizetype(history + 3));
    const auto current = model.captureRetainedRoot();
    const auto currentTx = model.beginTransaction(current.snapshot.epoch);
    quint64 visited = 99;
    QSet<KisPageVersion> expected{pageVersion(0, 1),
                                  pageVersion(0, 2),
                                  pageVersion(0, quint64(history + 3)),
                                  pageVersion(1, 1)};
    QCOMPARE(model.reachablePageVersions({pageKey(0), pageKey(1)}, &visited), expected);
    QCOMPARE(visited, quint64(3)); // Not H, 128 tokens/bases, or a duplicated current.
    QVERIFY(model.reachablePageVersions({}, &visited).isEmpty());
    QCOMPARE(visited, quint64(0));
    for (const auto &snapshot : snapshots)
        QVERIFY(model.releaseSnapshot(snapshot.token));
    QCOMPARE(model.reachablePageVersions({pageKey(0), pageKey(1)}, &visited), expected);
    QCOMPARE(visited, quint64(3)); // Active transaction bases remain reachable.
    for (const auto &tx : transactions)
        QVERIFY(model.abort(tx));
    expected.remove(pageVersion(0, 1));
    QCOMPARE(model.reachablePageVersions({pageKey(0), pageKey(1)}, &visited), expected);
    QCOMPARE(visited, quint64(2));
    QVERIFY(model.releaseSnapshot(second.token));
    expected.remove(pageVersion(0, 2));
    QCOMPARE(model.reachablePageVersions({pageKey(0), pageKey(1)}, &visited), expected);
    QCOMPARE(visited, quint64(1));
    QCOMPARE(model.rootCount(), qsizetype(history + 3)); // Destruction still deferred.
    QVERIFY(model.releaseSnapshot(current.token));
    QVERIFY(model.abort(currentTx));
    QVERIFY(!model.beginTransaction({1}).isValid());
    QVERIFY(!model.retainSnapshot({2}).isValid());
    QCOMPARE(model.reachablePageVersions({pageKey(0), pageKey(1)}, &visited), expected);
    QCOMPARE(visited, quint64(1));
    QCOMPARE(model.collectUnretainedRoots(), qsizetype(history + 2));
    QCOMPARE(model.rootCount(), qsizetype(1));
}

void KisPageStoreReferenceTest::boundedReachabilitySurvivesRootRemoval()
{
    QFETCH(int, history); QFETCH(int, removal);
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.manifest = {pageVersion(0, 1), pageVersion(1, 1)};
    KisImageEpochReferenceModel model; QVERIFY(model.initialize(initial));
    KisCompletionRegistry completions;
    const auto ticket = completions.allocatePending(completions.registerSource(KisCompletionDomain::HostLogical));
    QVERIFY(completions.complete(ticket, KisCompletionStatus::Succeeded));
    auto publish = [&](int column, quint64 generation) {
        const auto tx = model.beginTransaction(model.captureCommittedRoot().epoch());
        KisPreparedPageSet delta; delta.transaction = tx.id;
        delta.proofs = {preparedProof(pageVersion(column, generation), tx.id, ticket, generation)};
        return model.prepare(delta) && model.commit(tx).isCommitted();
    };
    QVector<KisRetainedImageEpochSnapshot> held;
    for (int i = 0; i < history; ++i) {
        held.append(model.captureRetainedRoot()); QVERIFY(held.back().isValid());
        QVERIFY(publish(0, quint64(i + 2)));
    }
    const auto start = model.beginReachabilityScan(pageKey(0)); QVERIFY(start.cookie);
    QSet<KisPageVersion> seen{start.current};
    auto remove = [&](int i) {
        if (held[i].token.isValid()) {
            const bool ok = model.releaseSnapshot(held[i].token);
            held[i] = {}; return ok;
        }
        return true;
    };
    if (history && removal == 1) QVERIFY(remove(0)); // cursor's next record
    if (history && removal == 2) QVERIFY(remove(history - 1)); // original last record
    if (removal == 3) for (int i = 0; i < history; ++i) QVERIFY(remove(i));
    model.collectUnretainedRoots(); // Actual erasure cannot dangle a value cursor.
    // Rehash/new roots/current-root removal for another key must not invalidate
    // this scan or extend its original finite frontier.
    QVERIFY(publish(1, 2));
    quint64 visited = 1;
    for (;;) {
        const auto slice = model.advanceReachabilityScan(start.cookie, 3);
        QVERIFY(slice.valid); QVERIFY(slice.rootsVisited <= 3);
        visited += quint64(slice.rootsVisited);
        for (qsizetype i = 0; i < slice.rootsVisited; ++i)
            seen.insert(slice.versions[size_t(i)]);
        if (slice.complete) break;
    }
    quint64 oracleVisits = 0;
    QCOMPARE(seen, model.reachablePageVersions({pageKey(0)}, &oracleVisits));
    QCOMPARE(visited, oracleVisits);
    model.endReachabilityScan(start.cookie);
    QVERIFY(!model.advanceReachabilityScan(start.cookie, 32).valid);
    for (int i = 0; i < history; ++i) QVERIFY(remove(i));
    model.collectUnretainedRoots(); QCOMPARE(model.rootCount(), qsizetype(1));
}

void KisPageStoreReferenceTest::boundedReachabilityInvalidatesOnlyChangedKeys()
{
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.manifest = {pageVersion(0, 1), pageVersion(1, 1)};
    KisImageEpochReferenceModel model; QVERIFY(model.initialize(initial));
    const auto retained = model.captureRetainedRoot(); QVERIFY(retained.isValid());
    const auto changed = model.beginReachabilityScan(pageKey(0));
    const auto unchanged = model.beginReachabilityScan(pageKey(1));
    QVERIFY(changed.cookie && unchanged.cookie);
    KisCompletionRegistry completions;
    const auto ticket = completions.allocatePending(completions.registerSource(KisCompletionDomain::HostLogical));
    QVERIFY(completions.complete(ticket, KisCompletionStatus::Succeeded));
    const auto tx = model.beginTransaction({1});
    KisPreparedPageSet delta; delta.transaction = tx.id;
    delta.proofs = {preparedProof(pageVersion(0, 2), tx.id, ticket, 2)};
    QVERIFY(model.prepare(delta)); QVERIFY(model.commit(tx).isCommitted());
    QVERIFY(!model.advanceReachabilityScan(changed.cookie, 32).valid);
    const auto unaffected = model.advanceReachabilityScan(unchanged.cookie, 32);
    QVERIFY(unaffected.valid && unaffected.complete);
    model.endReachabilityScan(changed.cookie);
    const auto restoredKey = model.beginReachabilityScan(pageKey(0));
    auto restore = model.prepareRestore(retained, nullptr); QVERIFY(restore.isValid());
    QVERIFY(model.installRestore(std::move(restore), nullptr, nullptr).isCommitted());
    QVERIFY(!model.advanceReachabilityScan(restoredKey.cookie, 32).valid);
    QVERIFY(model.advanceReachabilityScan(unchanged.cookie, 32).valid);
    model.endReachabilityScan(restoredKey.cookie); model.endReachabilityScan(unchanged.cookie);
    QVERIFY(model.releaseSnapshot(retained.token));
}

void KisPageStoreReferenceTest::boundedReachabilitySlotsAndCancellation()
{
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.manifest = {pageVersion(0, 1)};
    KisImageEpochReferenceModel model; QVERIFY(model.initialize(initial));
    QVector<quint64> cookies;
    for (int i = 0; i < 16; ++i) {
        auto start = model.beginReachabilityScan(pageKey(0)); QVERIFY(start.cookie);
        cookies.append(start.cookie);
    }
    QVERIFY(!model.beginReachabilityScan(pageKey(0)).cookie);
    model.endReachabilityScan(cookies.front());
    const auto replacement = model.beginReachabilityScan(pageKey(0));
    QVERIFY(replacement.cookie && replacement.cookie != cookies.front());
    QVERIFY(!model.advanceReachabilityScan(cookies.front(), 32).valid);
    model.endReachabilityScan(cookies.front()); // A late cancel cannot cancel the reused slot.
    const auto complete = model.advanceReachabilityScan(replacement.cookie, 32);
    QVERIFY(complete.valid && complete.complete); QCOMPARE(complete.rootsVisited, qsizetype(0));
    for (quint64 cookie : cookies) model.endReachabilityScan(cookie);
    model.endReachabilityScan(replacement.cookie);
    QVERIFY(!model.advanceReachabilityScan(0, 32).valid);
}

void KisPageStoreReferenceTest::boundedReachabilityPreservesDefaultRevision()
{
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    const KisPageVersion first{pageKey(0), {1}, 1};
    const KisPageVersion second{pageKey(0), {1}, 2};
    initial.surfaces = {surfaceEpochState()};
    KisImageEpochReferenceModel model; QVERIFY(model.initialize(initial));
    const auto retained = model.captureRetainedRoot(); QVERIFY(retained.isValid());
    const auto beforeScan = model.beginReachabilityScan(pageKey(0));
    QVERIFY(beforeScan.cookie);
    const auto tx = model.beginTransaction({1});
    KisPreparedPageSet delta; delta.transaction = tx.id;
    const auto before = initial.surfaces.first();
    const auto after = surfaceEpochState({1}, 2, 0x19);
    delta.surfaceChanges = {{before, after}};
    QVERIFY(model.prepare(delta)); QVERIFY(model.commit(tx).isCommitted());
    QVERIFY(!model.advanceReachabilityScan(beforeScan.cookie, 32).valid);
    model.endReachabilityScan(beforeScan.cookie);
    const auto scan = model.beginReachabilityScan(pageKey(0)); QVERIFY(scan.cookie);
    QCOMPARE(scan.current, second);
    const auto slice = model.advanceReachabilityScan(scan.cookie, 32);
    QVERIFY(slice.valid && slice.complete); QCOMPARE(slice.rootsVisited, qsizetype(1));
    QCOMPARE(slice.versions[0], first); // Equal generation never aliases default revisions.
    model.endReachabilityScan(scan.cookie);
    QVERIFY(model.releaseSnapshot(retained.token));
}

void KisPageStoreReferenceTest::rootAdmissionSerializesWithFinalRelease_data()
{
    QTest::addColumn<bool>("releaseTransaction");
    QTest::addColumn<bool>("acquireTransaction");
    QTest::addColumn<int>("order");
    for (bool r : {false, true})
        for (bool a : {false, true})
            for (int o = 0; o < 3; ++o)
                QTest::newRow(qPrintable(QStringLiteral("releaseTx%1-acquireTx%2-order%3").arg(r).arg(a).arg(o)))
                    << r << a << o;
}

void KisPageStoreReferenceTest::rootAdmissionSerializesWithFinalRelease()
{
    QFETCH(bool, releaseTransaction);
    QFETCH(bool, acquireTransaction);
    QFETCH(int, order);
    for (int repeat = 0; repeat < 32; ++repeat) {
        KisImageEpochSnapshot initial;
        initial.epoch = {1};
        initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
        initial.manifest = {pageVersion(0, 1), pageVersion(1, 1)};
        KisImageEpochReferenceModel model;
        QVERIFY(model.initialize(initial));
        const auto retained = releaseTransaction ? KisRetainedImageEpochSnapshot{} : model.captureRetainedRoot();
        const auto active = releaseTransaction ? model.beginTransaction({1}) : KisPageTransaction{};
        const auto writer = model.beginTransaction({1});
        KisPreparedPageSet delta;
        delta.transaction = writer.id;
        delta.removedPages = {pageKey(1)};
        QVERIFY(model.prepare(delta));
        QVERIFY(model.commit(writer).isCommitted());
        std::atomic<bool> start{false}, acquired{false}, released{false};
        KisRetainedImageEpochSnapshot nextSnapshot;
        KisPageTransaction nextTransaction;
        bool releaseOk = false;
        std::thread acquirer([&] {
            while (!start.load() || (order == 1 && !released.load()))
                std::this_thread::yield();
            if (acquireTransaction)
                nextTransaction = model.beginTransaction({1});
            else
                nextSnapshot = model.retainSnapshot({1});
            acquired = true;
        });
        std::thread releaser([&] {
            while (!start.load() || (order == 0 && !acquired.load()))
                std::this_thread::yield();
            releaseOk = releaseTransaction ? model.abort(active) : model.releaseSnapshot(retained.token);
            released = true;
        });
        start = true;
        acquirer.join();
        releaser.join(); // Join before every assertion.
        QVERIFY(releaseOk);
        const bool admitted = acquireTransaction ? nextTransaction.isValid() : nextSnapshot.isValid();
        if (order != 2)
            QCOMPARE(admitted, order == 0);
        QCOMPARE(model.root({1}).isValid(), admitted);
        QCOMPARE(model.rootCount(), qsizetype(2));
        if (admitted) {
            if (acquireTransaction)
                QVERIFY(model.abort(nextTransaction));
            else
                QVERIFY(model.releaseSnapshot(nextSnapshot.token));
        }
        QVERIFY(!model.retainSnapshot({1}).isValid());
        QVERIFY(!model.beginTransaction({1}).isValid());
        QCOMPARE(model.collectUnretainedRoots(), qsizetype(1));
    }
}

void KisPageStoreReferenceTest::persistentExtentIndexMatchesManifestOracle()
{
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    for (quint64 surface = 1; surface <= 3; ++surface) {
        for (int y = -8; y < 8; ++y) {
            for (int x = -64; x < 64; ++x)
                initial.manifest.append({{{surface}, {x, y}}, {1}});
        }
    }
    KisImageEpochReferenceModel model;
    QVERIFY(model.initialize(initial));
    const auto original = model.captureCommittedRoot();
    const QSize tileSize(64, 64);
    const QRect originalExtent(-4096, -512, 8192, 1024);
    for (int column = -64; column < 0; ++column) {
        const auto base = model.captureCommittedRoot();
        const auto tx = model.beginTransaction(base.epoch());
        KisPreparedPageSet delta;
        delta.transaction = tx.id;
        for (int row = -8; row < 8; ++row)
            delta.removedPages.append(pageKey(column, row));
        QRect expected;
        // The full manifest is an independent test oracle, not the production
        // extent algorithm. Exercise boundary contraction and surface pruning.
        for (const auto &version : base.manifest()) {
            if (!(version.key.surface == KisSurfaceId{1}) || delta.removedPages.contains(version.key))
                continue;
            const QRect tile(version.key.page.column * 64, version.key.page.row * 64, 64, 64);
            expected = expected.isNull() ? tile : expected.united(tile);
        }
        QRect actual;
        QVERIFY(base.contentExtentAfterDelta({1}, tileSize, delta, &actual));
        QCOMPARE(actual, expected);
        QVERIFY(model.prepare(delta));
        const auto committed = model.commit(tx);
        QVERIFY(committed.isCommitted());
        QVERIFY(committed.root.contentExtentAfterDelta({1}, tileSize, {}, &actual));
        QCOMPARE(actual, expected);
        for (quint64 surface : {quint64(2), quint64(3)}) {
            QVERIFY(committed.root.contentExtentAfterDelta({surface}, tileSize, {}, &actual));
            QCOMPARE(actual, originalExtent);
        }
        model.collectFinishedTransactions();
        model.collectUnretainedRoots();
    }
    QRect actual;
    QVERIFY(original.contentExtentAfterDelta({1}, tileSize, {}, &actual));
    QCOMPARE(actual, originalExtent);
    QVERIFY(original.contentExtentAfterDelta({4}, tileSize, {}, &actual));
    QVERIFY(actual.isNull());
    KisPreparedPageSet addition;
    KisPreparedPageProof proof;
    proof.authority.version = {{{4}, {-5, -9}}, {2}};
    addition.proofs.append(proof);
    QVERIFY(original.contentExtentAfterDelta({4}, tileSize, addition, &actual));
    QCOMPARE(actual, QRect(-320, -576, 64, 64));
    addition.removedPages = {proof.authority.version.key}; // replacement order matches commit
    QVERIFY(original.contentExtentAfterDelta({4}, tileSize, addition, &actual));
    QCOMPARE(actual, QRect(-320, -576, 64, 64));
    proof.authority.version.key.page.column = std::numeric_limits<qint32>::max();
    addition.proofs = {proof};
    QVERIFY(!original.contentExtentAfterDelta({4}, tileSize, addition, &actual));
    proof.authority.version.key.page.column = std::numeric_limits<qint32>::max() / 64;
    addition.proofs = {proof};
    proof.authority.version.key.page.column = std::numeric_limits<qint32>::min() / 64;
    addition.proofs.append(proof);
    QVERIFY(!original.contentExtentAfterDelta({4}, tileSize, addition, &actual));
    QVERIFY(!original.contentExtentAfterDelta({1}, QSize(), {}, &actual));
    QVERIFY(!original.contentExtentAfterDelta({}, tileSize, {}, &actual));
}

void KisPageStoreReferenceTest::preparedMetadataClaimsDoNotLoseConcurrentReaders()
{
    const KisPageTransaction transaction{KisPageTransactionId{73}, KisImageEpochId{1}};
    KisPageMetadataCoordinator coordinator;
    QVERIFY(coordinator.configure(8));
    QVector<KisPageTransition> transitions;
    constexpr int pages = 64;
    for (int i = 0; i < pages; ++i) {
        auto page = initialPageState(pageVersion(i, 1), replica(pageVersion(i, 1), 1, 1, quint64(i * 2 + 1)));
        const auto authority = replica(pageVersion(i, 2), 1, 1, quint64(i * 2 + 2));
        page.versions.append({pageVersion(i, 2),
                              KisPagePublicationState::Prepared,
                              {{authority, KisReplicaValidity::Valid, {}, {}, 0, {}}},
                              authority,
                              transaction.id});
        page.nextGeneration = KisPageGeneration{3};
        QVERIFY(coordinator.registerPage(page));
        KisPageTransition transition;
        transition.kind = KisPageTransitionKind::CommitTransaction;
        transition.transaction = transaction.id;
        transition.version = pageVersion(i, 2);
        transition.imageEpoch = KisImageEpochId{2};
        transitions.append(transition);
    }
    std::atomic<bool> start{false};
    std::atomic<int> failures{0};
    std::vector<std::thread> readers;
    for (int worker = 0; worker < 4; ++worker) {
        readers.emplace_back([&, worker]() {
            while (!start.load())
                std::this_thread::yield();
            for (int visit = 0; visit < 1000; ++visit) {
                KisPageTransition read;
                read.kind = KisPageTransitionKind::AcquireRead;
                const int pageIndex = (visit * 7 + worker) % pages;
                read.version = pageVersion(pageIndex, 1);
                read.target = replica(read.version, 1, 1, quint64(pageIndex * 2 + 1));
                read.lease = KisPageLeaseId{quint64(worker * 1000 + visit + 1)};
                auto apply = [&]() {
                    for (int retry = 0; retry < 100000; ++retry) {
                        if (coordinator.applyOwner(read.version.key, read).accepted)
                            return true;
                        std::this_thread::yield();
                    }
                    ++failures;
                    return false;
                };
                if (!apply())
                    return;
                read.kind = KisPageTransitionKind::ReleaseRead;
                if (!apply())
                    return;
            }
        });
    }
    auto batch = coordinator.preparePublication(transaction, KisImageEpochId{2}, transitions);
    start.store(true);
    bool installed = false;
    for (int attempt = 0; attempt < 32 && !installed; ++attempt) {
        installed = coordinator.installPublication(std::move(batch), transaction, KisImageEpochId{2});
        if (!installed)
            batch = coordinator.preparePublication(transaction, KisImageEpochId{2}, transitions);
    }
    for (auto &reader : readers)
        reader.join();
    QCOMPARE(failures.load(), 0);
    if (!installed) {
        batch = coordinator.preparePublication(transaction, KisImageEpochId{2}, transitions);
        QVERIFY(coordinator.installPublication(std::move(batch), transaction, KisImageEpochId{2}));
    }
    for (int i = 0; i < pages; ++i) {
        KisPageStateSnapshot page;
        QVERIFY(coordinator.pageSnapshot(pageKey(i), &page));
        QCOMPARE(page.publishedGeneration.value, quint64(2));
        QVERIFY(KisPageStateMachine().validateInvariants(page));
        QVERIFY(page.versions.first().replicas.first().readLeases.isEmpty());
    }
    QCOMPARE(coordinator.metrics().installedPublicationPages, quint64(pages));
}

void KisPageStoreReferenceTest::indexedHistorySlices()
{
    QFETCH(int, history);
    auto expected = pageWithHistory(history);
    // This metadata oracle has synthetic replicas. Its model retirement owner
    // accepts each emitted responsibility; physical handoff is tested by the
    // original ledger/queue fixtures.
    MetadataEffectReceiver debt;
    KisPageMetadataCoordinator coordinator;
    attachEffects(coordinator, debt);
    QVERIFY(coordinator.configure(4));
    QVERIFY(coordinator.registerPage(expected));
    KisPageVersion cursor;
    int visited = 0;
    do {
        const auto slice = coordinator.historySlice(expected.key, cursor, 32);
        QVERIFY(slice.count <= 32);
        QCOMPARE(slice.total, qsizetype(history - visited));
        KisPageTransition discard;
        discard.kind = KisPageTransitionKind::DiscardHistoricalVersions;
        for (qsizetype i = 0; i < slice.count; ++i) {
            const auto &v = slice.versions[size_t(i)];
            QVERIFY(v.generation.value > cursor.generation.value);
            discard.versions.append(v);
            cursor = v;
            ++visited;
        }
        if (!discard.versions.isEmpty()) {
            // Remove even the cursor's own record before continuation. Numeric
            // identity cursors must not dangle or restart from the list head.
            const auto oracle = KisPageStateMachine().apply(expected, discard);
            QVERIFY(oracle.accepted);
            expected = oracle.next;
            debt.effects.clear();
            quint32 removed = 0;
            QVERIFY(coordinator.discardHistory(expected.key, slice.versions.data(), slice.count,
                                               0, &removed));
            QCOMPARE(removed, slice.count == 32 ? ~quint32(0) : (quint32(1) << slice.count) - 1);
            QCOMPARE(debt.effects.size(), oracle.effects.size());
            for (const auto &effect : oracle.effects) {
                QVERIFY(std::any_of(debt.effects.cbegin(), debt.effects.cend(), [&](const auto &a) {
                    return a.replica == effect.replica && a.lastUse == effect.lastUse;
                }));
            }
        }
        if (!slice.after.isValid())
            break;
    } while (visited <= history);
    QCOMPARE(visited, history);
    QCOMPARE(debt.prepared, quint64(history));
    QCOMPARE(debt.committed, quint64(history));
    QCOMPARE(debt.cancelled, quint64(0));
    const auto metrics = coordinator.metrics();
    QCOMPARE(metrics.historySliceVersionInputs, quint64(history));
    QVERIFY(metrics.maximumHistorySliceVersionInputs <= 32);
    QCOMPARE(metrics.localVersionRemovals, quint64(history));
    QCOMPARE(metrics.fullSnapshotExports, quint64(0));
    KisPageStateSnapshot actual;
    QVERIFY(coordinator.pageSnapshot(expected.key, &actual));
    comparePageRecords(actual, expected);
    if (history >= 4095) {
        const auto footprint = coordinator.footprint();
        QVERIFY(footprint.versionArena.releasedBytes > 0);
        QVERIFY(footprint.replicaArena.releasedBytes > 0);
        QCOMPARE(footprint.versionArena.activeBlocks, quint64(1));
        QCOMPARE(footprint.replicaArena.activeBlocks, quint64(1));
    }

    // Published -> Historical can insert below an old cursor. Explicit restart
    // must see it, including a second default revision at the same generation.
    KisPageTransition restore;
    restore.kind = KisPageTransitionKind::RestoreCommittedVersion;
    restore.version = {expected.key, {1}, 11};
    restore.imageEpoch = {2};
    auto batch = coordinator.prepareRestoration({2}, {restore});
    QVERIFY(batch.isValid());
    QVERIFY(coordinator.installPublication(std::move(batch), {}, {2}));
    restore.version.defaultPixelRevision = 12;
    restore.imageEpoch = {3};
    batch = coordinator.prepareRestoration({3}, {restore});
    QVERIFY(batch.isValid());
    QVERIFY(coordinator.installPublication(std::move(batch), {}, {3}));
    const auto restarted = coordinator.historySlice(expected.key, {}, 32);
    QCOMPARE(restarted.count, qsizetype(2));
    QCOMPARE(restarted.total, qsizetype(2));
    QVERIFY(std::any_of(restarted.versions.cbegin(), restarted.versions.cbegin() + restarted.count, [](const auto &v) {
        return v.defaultPixelRevision == 11;
    }));
}

void KisPageStoreReferenceTest::metadataCompactRecordsRoundTrip()
{
    QCOMPARE(sizeof(KisVersionSlotId), size_t(8));
    QVERIFY(sizeof(KisVersionRecord) <= size_t(128));
    QVERIFY(sizeof(KisReplicaRecord) <= size_t(128));
    QVERIFY(sizeof(KisMetadataOverflowNode) <= size_t(64));

    KisCompletionRegistry completions;
    const KisCompletionDomain source = KisCompletionDomain::HostLogical;
    const KisCompletionTicket lastUse = completions.allocatePending(completions.registerSource(source));
    QVERIFY(lastUse.isValid());

    const KisPageVersion version = pageVersion(41, 7);
    const KisReplicaHandle authority = replica(version, 17, 3, 91, 5);
    KisPageStateSnapshot expected = initialPageState(version, authority);
    auto &versionState = expected.versions.first();
    versionState.capturedReadViews = {{301}, {302}};
    versionState.replicas.first().readLeases = {{401}, {402}};
    versionState.replicas.first().pinCount = 2;
    versionState.replicas.first().pendingLastUses = {lastUse};
    const KisReplicaHandle secondary = replica(version, 18, 4, 92, 6);
    versionState.replicas.append({secondary, KisReplicaValidity::Valid, {}, {{403}}, 1, {lastUse}});
    const KisPageVersion historicalVersion = pageVersion(41, 6);
    const KisReplicaHandle historical = replica(historicalVersion, 17, 3, 90, 5);
    expected.versions.append({historicalVersion, KisPagePublicationState::Historical,
                              {{historical, KisReplicaValidity::Valid, {}, {{405}}, 0, {}}},
                              historical, {}, {}});

    KisPageStateMachine machine;
    QString error;
    QVERIFY2(machine.validateInvariants(expected, &error), qPrintable(error));
    KisPageMetadataCoordinator coordinator;
    MetadataEffectReceiver receiver;
    attachEffects(coordinator, receiver);
    QVERIFY(coordinator.configure(1));
    QVERIFY2(coordinator.registerPage(expected, &error), qPrintable(error));

    KisPageStateSnapshot actual;
    QVERIFY(coordinator.pageSnapshot(expected.key, &actual));
    comparePageRecords(actual, expected);
    auto footprint = coordinator.footprint();
    QCOMPARE(footprint.pages, quint64(1));
    QCOMPARE(footprint.pageActivities, quint64(0));
    QCOMPARE(footprint.metadataPageBytes, quint64(96));
    QCOMPARE(footprint.metadataPageActivityBytes, quint64(0));

    KisPageTransition acquire;
    acquire.kind = KisPageTransitionKind::AcquireRead;
    acquire.version = version;
    acquire.target = secondary;
    acquire.lease = {405}; // occupied only by the historical version
    QVERIFY(!machine.apply(expected, acquire).accepted);
    QVERIFY(!coordinator.applyOwner(expected.key, acquire).accepted);
    acquire.lease = {404};
    const auto oracle = machine.apply(expected, acquire);
    QVERIFY2(oracle.accepted, qPrintable(oracle.rejectionReason));
    const auto beforeAcquire = coordinator.metrics();
    const auto applied = coordinator.applyOwner(expected.key, acquire);
    QVERIFY2(applied.accepted, qPrintable(applied.rejectionReason));
    const auto afterAcquire = coordinator.metrics();
    QCOMPARE(afterAcquire.fullSnapshotExports, beforeAcquire.fullSnapshotExports);
    QCOMPARE(afterAcquire.localTransitionSequences, beforeAcquire.localTransitionSequences + 1);
    expected = oracle.next;
    QVERIFY(coordinator.pageSnapshot(expected.key, &actual));
    comparePageRecords(actual, expected);

    KisPageTransition duplicateCapture;
    duplicateCapture.kind = KisPageTransitionKind::RetainCapturedVersion;
    duplicateCapture.version = version;
    duplicateCapture.readView = {301};
    QVERIFY(!coordinator.applyOwner(expected.key, duplicateCapture).accepted);
    QVERIFY(coordinator.pageSnapshot(expected.key, &actual));
    comparePageRecords(actual, expected);

    KisPageTransition write;
    write.kind = KisPageTransitionKind::AcquireWrite;
    write.baseVersion = version;
    write.source = authority;
    write.version = {expected.key, expected.nextGeneration, 0};
    write.target = replica(write.version, 17, 3, 93, 1);
    write.transaction = {501};
    write.operation = {502};
    write.writer = {503};
    auto writerOracle = machine.apply(expected, write);
    QVERIFY2(writerOracle.accepted, qPrintable(writerOracle.rejectionReason));
    QVERIFY(coordinator.applyOwner(expected.key, write).accepted);
    expected = writerOracle.next;
    QVERIFY(coordinator.pageSnapshot(expected.key, &actual));
    comparePageRecords(actual, expected);
    footprint = coordinator.footprint();
    QCOMPARE(footprint.pageActivities, quint64(1));
    QVERIFY(footprint.metadataPageActivityBytes <= quint64(96));

    write.kind = KisPageTransitionKind::CancelWrite;
    writerOracle = machine.apply(expected, write);
    QVERIFY2(writerOracle.accepted, qPrintable(writerOracle.rejectionReason));
    QVERIFY(coordinator.applyOwner(expected.key, write).accepted);
    expected = writerOracle.next;
    QVERIFY(coordinator.pageSnapshot(expected.key, &actual));
    comparePageRecords(actual, expected);
    footprint = coordinator.footprint();
    QCOMPARE(footprint.pageActivities, quint64(0));
    QCOMPARE(footprint.metadataPageActivityBytes, quint64(0));
}

void KisPageStoreReferenceTest::metadataArenaGrowthCausalBaseline()
{
    struct Sample {
        int pages = 0;
        int history = 0;
        KisPageMetadataMetrics metrics;
        KisPageMetadataFootprint footprint;
    };
    QString error;
    const auto run = [&](int pageCount, int history, Sample *sample) {
        KisPageMetadataCoordinator coordinator;
        if (!coordinator.configure(1, &error))
            return false;
        for (int pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
            const KisPageVersion current = pageVersion(pageIndex, quint64(history + 1));
            KisPageStateSnapshot page;
            page.key = current.key;
            page.publishedEpoch = {1};
            page.publishedGeneration = current.generation;
            page.nextGeneration = {quint64(history + 2)};
            for (int offset = 0; offset <= history; ++offset) {
                const KisPageVersion version = pageVersion(pageIndex, quint64(history + 1 - offset));
                const KisReplicaHandle authority =
                    replica(version, 81, 1, quint64(pageIndex + 1) * 10000 + version.generation.value);
                page.versions.append({version,
                                      offset ? KisPagePublicationState::Historical : KisPagePublicationState::Published,
                                      {{authority, KisReplicaValidity::Valid, {}, {}, 0, {}}},
                                      authority,
                                      {},
                                      {}});
            }
            if (!coordinator.registerPage(page, &error))
                return false;
        }
        sample->pages = pageCount;
        sample->history = history;
        sample->metrics = coordinator.metrics();
        sample->footprint = coordinator.footprint();
        return true;
    };

    Sample k1h0;
    Sample k16h0;
    Sample k16h4;
    QVERIFY2(run(1, 0, &k1h0), qPrintable(error));
    QVERIFY2(run(16, 0, &k16h0), qPrintable(error));
    QVERIFY2(run(16, 4, &k16h4), qPrintable(error));

    const auto verify = [](const Sample &sample) {
        const quint64 records = quint64(sample.pages) * quint64(sample.history + 1);
        QCOMPARE(sample.footprint.pages, quint64(sample.pages));
        QCOMPARE(sample.footprint.pageActivities, quint64(0));
        QCOMPARE(sample.footprint.metadataPageBytes, quint64(sample.pages) * quint64(96));
        QCOMPARE(sample.footprint.metadataPageActivityBytes, quint64(0));
        QCOMPARE(sample.footprint.versions, records);
        QCOMPARE(sample.footprint.replicas, records);
        QCOMPARE(sample.footprint.overflowArena.usedSlots, quint64(0));
        QCOMPARE(sample.footprint.versionArena.usedSlots, records);
        QCOMPARE(sample.footprint.replicaArena.usedSlots, records);
        QCOMPARE(sample.footprint.exactVersionIndex.entries, records);
        QCOMPARE(sample.footprint.physicalSlotIndex.entries, records);
        QCOMPARE(sample.footprint.exactVersionIndex.outstandingReservations, quint64(0));
        QCOMPARE(sample.footprint.physicalSlotIndex.outstandingReservations, quint64(0));
        QCOMPARE(sample.metrics.metadataArenaGrowthConflicts, quint64(0));
        QCOMPARE(sample.metrics.metadataArenaGrowthFailures, quint64(0));
        QCOMPARE(sample.metrics.metadataArenaBlockCandidatesPrepared, sample.metrics.metadataArenaBlocksAttached);
        QCOMPARE(sample.metrics.metadataArenaBlocksAttached,
                 sample.footprint.versionArena.attachedBlocks + sample.footprint.replicaArena.attachedBlocks
                     + sample.footprint.overflowArena.attachedBlocks);
        QCOMPARE(sample.footprint.versionArena.allocatedBytes, sample.footprint.versionArena.activeBlocks * 16 * 1024);
        QCOMPARE(sample.footprint.replicaArena.allocatedBytes, sample.footprint.replicaArena.activeBlocks * 32 * 1024);
    };
    verify(k1h0);
    verify(k16h0);
    verify(k16h4);

    QVERIFY(k16h0.footprint.trackedPayloadBytes > k1h0.footprint.trackedPayloadBytes);
    QVERIFY(k16h4.footprint.trackedPayloadBytes > k16h0.footprint.trackedPayloadBytes);
    QVERIFY(k16h0.metrics.metadataArenaBlockCandidatesPrepared < quint64(k16h0.pages * 2));
    QVERIFY(k16h4.metrics.metadataArenaBlockCandidatesPrepared < quint64(k16h4.pages * (k16h4.history + 1) * 2));

    if (qEnvironmentVariableIntValue("KIS_BR1_METADATA_BASELINE") != 0) {
        QJsonArray rows;
        for (const Sample *sample : {&k1h0, &k16h0, &k16h4}) {
            rows.append(QJsonObject{
                {"K", sample->pages},
                {"H", sample->history},
                {"logical_bytes", double(sample->footprint.trackedPayloadBytes)},
                {"arena_bytes",
                 double(sample->footprint.versionArena.allocatedBytes + sample->footprint.replicaArena.allocatedBytes
                        + sample->footprint.overflowArena.allocatedBytes)},
                {"block_candidates", double(sample->metrics.metadataArenaBlockCandidatesPrepared)},
                {"blocks_attached", double(sample->metrics.metadataArenaBlocksAttached)},
                {"version_slack", double(sample->footprint.versionArena.freeSlots)},
                {"replica_slack", double(sample->footprint.replicaArena.freeSlots)},
                {"overflow_slack", double(sample->footprint.overflowArena.freeSlots)}});
        }
        qInfo().noquote() << "BR1_METADATA_ARENA" << QJsonDocument(rows).toJson(QJsonDocument::Compact);
    }
}

void KisPageStoreReferenceTest::metadataShardSlotIndexReservations()
{
    using Index = KisShardSlotIndex<quint64, KisVersionSlotId>;
    Index index;

    QVERIFY(index.prepareCapacity(index.requiredCapacity(2)));
    auto first = index.reserveInsertions(2);
    QVERIFY(first.isValid());
    QCOMPARE(first.remaining(), qsizetype(2));
    QCOMPARE(index.statistics().outstandingReservations, quint64(2));
    QVERIFY(index.insertReserved(&first, 11, {1, 1}));
    QCOMPARE(first.remaining(), qsizetype(1));

    auto moved = std::move(first);
    QVERIFY(!first.isValid());
    QVERIFY(moved.isValid());
    QVERIFY(index.insertReserved(&moved, 12, {2, 1}));
    QVERIFY(!moved.isValid());
    QCOMPARE(index.statistics().outstandingReservations, quint64(0));

    KisVersionSlotId slot;
    QVERIFY(index.findExact(11, &slot));
    QCOMPARE(slot, (KisVersionSlotId{1, 1}));
    QVERIFY(index.findExact(12, &slot));
    QCOMPARE(slot, (KisVersionSlotId{2, 1}));

    QVERIFY(index.prepareCapacity(index.requiredCapacity(1)));
    auto duplicate = index.reserveInsertions(1);
    QVERIFY(duplicate.isValid());
    QVERIFY(!index.insertReserved(&duplicate, 11, {3, 1}));
    QCOMPARE(duplicate.remaining(), qsizetype(1));
    index.cancelReservation(&duplicate);
    QVERIFY(!duplicate.isValid());

    QVERIFY(!index.eraseExact(11, {99, 1}));
    QVERIFY(index.eraseExact(11, {1, 1}));
    QVERIFY(!index.findExact(11));
    QCOMPARE(index.size(), qsizetype(1));
    const auto stats = index.statistics();
    QCOMPARE(stats.entries, quint64(1));
    QCOMPARE(stats.outstandingReservations, quint64(0));
    QCOMPARE(stats.highWaterEntries, quint64(2));
    QCOMPARE(stats.reservationBatches, quint64(2));
}

void KisPageStoreReferenceTest::metadataShardSlotIndexGrowthAndErasure()
{
    // Identical hashes force a cluster across the end of the initial array.
    // Compare every operation with an independent reference, including erasure
    // from the start/middle and growth while another token is outstanding.
    const auto exercise = [](auto sampleKey) {
        using Key = decltype(sampleKey);
        KisShardSlotIndex<Key, KisVersionSlotId> index;
        QHash<quint64, KisVersionSlotId> expected;
        std::mt19937 random(12345);
        QVERIFY(!index.prepareCapacity(0));
        QVERIFY(!index.prepareCapacity(std::numeric_limits<qsizetype>::max()));
        QVERIFY(!index.reserveInsertions(1).isValid());
        for (int round = 0; round < 160; ++round) {
            QVERIFY(index.prepareCapacity(index.requiredCapacity(32)));
            auto held = index.reserveInsertions(16);
            auto active = index.reserveInsertions(16);
            QVERIFY(held.isValid() && active.isValid());
            // Growing for another candidate must preserve both tokens.
            QVERIFY(index.prepareCapacity(index.requiredCapacity(32)));
            for (int operation = 0; operation < 16; ++operation) {
                const quint64 key = random() % 1024;
                const Key typedKey{key};
                const KisVersionSlotId slot{quint32(key + 1), quint32(round + 1)};
                if (random() % 3 == 0) {
                    QCOMPARE(index.eraseExact(typedKey), bool(expected.remove(key)));
                } else {
                    const bool absent = !expected.contains(key);
                    QCOMPARE(index.insertReserved(&active, typedKey, slot), absent);
                    if (absent) expected.insert(key, slot);
                }
            }
            index.cancelReservation(&held);
            index.cancelReservation(&active);
            QCOMPARE(index.statistics().outstandingReservations, quint64(0));
            QCOMPARE(index.size(), expected.size());
            for (quint64 key = 0; key < 1024; ++key) {
                KisVersionSlotId actual;
                QCOMPARE(index.findExact(Key{key}, &actual), expected.contains(key));
                if (expected.contains(key)) QCOMPARE(actual, expected.value(key));
            }
        }
        const auto capacity = index.statistics().capacity;
        for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
            QVERIFY(!index.eraseExact(Key{it.key()}, {0xffffffff, 1}));
            QVERIFY(index.eraseExact(Key{it.key()}, it.value()));
        }
        QCOMPARE(index.size(), qsizetype(0));
        QCOMPARE(index.statistics().capacity, capacity);
        auto final = index.reserveInsertions(qsizetype(capacity));
        QVERIFY(final.isValid());
        index.cancelReservation(&final);
    };
    exercise(quint64{});
    exercise(CollidingIndexKey{});
}

void KisPageStoreReferenceTest::metadataReadProtectionAtCapacity_data()
{
    QTest::addColumn<int>("kind");
    QTest::addColumn<int>("position");
    QTest::addColumn<int>("history");
    for (int kind = 0; kind < 3; ++kind)
        for (int position = 0; position < 3; ++position)
            for (int history : {0, 257})
                QTest::newRow(qPrintable(QStringLiteral("kind%1-position%2-history%3")
                    .arg(kind).arg(position).arg(history))) << kind << position << history;
}

void KisPageStoreReferenceTest::metadataReadProtectionAtCapacity()
{
    QFETCH(int, kind);
    QFETCH(int, position);
    QFETCH(int, history);
    using OverflowArena = KisShardSlotArena<KisMetadataOverflowNode, 16 * 1024>;
    const int count = int(OverflowArena::slotsPerBlock());
    const int selected = position == 0 ? 0 : position == 1 ? count / 2 : count - 1;
    KisCompletionRegistry completions;
    const auto source = completions.registerSource(KisCompletionDomain::HostLogical);
    const auto ticket = completions.allocatePending(source);
    const auto otherTicket = completions.allocatePending(source);
    QVERIFY(completions.complete(ticket, KisCompletionStatus::Succeeded));
    auto initial = pageWithHistory(history);
    auto &target = initial.versions.first().replicas.first();
    for (int i = 0; i < count; ++i) {
        if (kind == 2)
            target.pendingLastUses.append(i == selected ? ticket : otherTicket);
        else
            target.readLeases.append(KisPageLeaseId{quint64(i + 1)});
    }
    KisPageTransition transition;
    transition.kind = kind == 2 ? KisPageTransitionKind::AcknowledgeLastUse
                               : KisPageTransitionKind::ReleaseRead;
    transition.version = target.replica.version;
    transition.target = target.replica;
    transition.lease = {quint64(selected + 1)};
    if (kind != 0) transition.completion = ticket;
    const auto expected = KisPageStateMachine{}.apply(initial, transition);
    QVERIFY(expected.accepted);

    KisPageBackingLimits limits;
    limits.metadataArenaBytes = 4 * 1024 * 1024;
    KisBackingBudgetController budget(limits);
    KisPageMetadataCoordinator coordinator;
    coordinator.attachBackingBudget(budget);
    QVERIFY(coordinator.configure(1));
    QVERIFY(coordinator.registerPage(initial));
    // Fill after preparation: paid candidate arrays need transient headroom.
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - budget.usage()
        .buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam);
    void *filler = kisAllocateMutationStorage(&budget, fillerBytes, 1);
    const auto freeFiller = qScopeGuard([&] { kisFreeMutationStorage(&budget, filler, fillerBytes, 1); });
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
             limits.metadataArenaBytes);
    QCOMPARE(coordinator.footprint().overflowArena.freeSlots, quint64(0));
    const auto before = coordinator.metrics();
    const auto result = kind == 2
        ? coordinator.acknowledgeLastUse(transition.version, transition.target, completions.verifyTerminal(ticket))
        : coordinator.applyOwner(initial.key, transition);
    QVERIFY2(result.accepted, qPrintable(result.rejectionReason));
    const auto after = coordinator.metrics();
    QCOMPARE(after.localTransitionSequences, before.localTransitionSequences + 1);
    QCOMPARE(after.localVersionInputs, before.localVersionInputs);
    QCOMPARE(after.localVersionInstalls, before.localVersionInstalls);
    QCOMPARE(after.metadataArenaGrowthBatches, before.metadataArenaGrowthBatches);
    QCOMPARE(after.readProtectionTransitions, before.readProtectionTransitions + 1);
    QCOMPARE(after.readProtectionSlotReuses, before.readProtectionSlotReuses + (kind == 1 ? 1 : 0));
    QCOMPARE(after.readProtectionSlotReleases, before.readProtectionSlotReleases + (kind == 1 ? 0 : 1));
    const auto usage = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
    QCOMPARE(usage.live.cpuRam, limits.metadataArenaBytes);
    QCOMPARE(usage.reserved.cpuRam, quint64(0));
    QCOMPARE(coordinator.footprint().overflowArena.usedSlots, quint64(count - (kind == 1 ? 0 : 1)));
    KisPageStateSnapshot actual;
    QVERIFY(coordinator.pageSnapshot(initial.key, &actual));
    comparePageRecords(actual, expected.next);
}

void KisPageStoreReferenceTest::metadataReadProtectionMatchesReference_data()
{
    QTest::addColumn<int>("variant");
    QTest::addColumn<int>("history");
    for (int v = 0; v < 24; ++v)
        for (int h : {0, 257})
            QTest::newRow(qPrintable(QStringLiteral("variant%1-history%2").arg(v).arg(h))) << v << h;
}

void KisPageStoreReferenceTest::metadataReadProtectionMatchesReference()
{
    QFETCH(int, variant);
    QFETCH(int, history);
    KisCompletionRegistry completions;
    const auto source = completions.registerSource(KisCompletionDomain::HostLogical);
    const auto first = completions.allocatePending(source);
    const auto second = completions.allocatePending(source);
    const auto unknown = completions.allocatePending(source);
    const auto pending = completions.allocatePending(source);
    QVERIFY(completions.complete(first, KisCompletionStatus::Succeeded));
    QVERIFY(completions.complete(second, KisCompletionStatus::Failed));
    QVERIFY(completions.complete(unknown, KisCompletionStatus::Cancelled));
    auto initial = pageWithHistory(history);
    auto &replicaState = initial.versions.first().replicas.first();
    replicaState.readLeases = {{11}, {12}, {13}};
    replicaState.pendingLastUses = {first, second, first, pending};
    KisPageMetadataCoordinator coordinator;
    QVERIFY(coordinator.configure(1));
    QVERIFY(coordinator.registerPage(initial));
    KisPageTransition transition;
    transition.kind = KisPageTransitionKind::ReleaseRead;
    transition.version = replicaState.replica.version;
    transition.target = replicaState.replica;
    transition.lease = {quint64(11 + variant % 3)};
    bool acknowledge = false;
    switch (variant) {
    case 3: case 4: case 5:
        transition.completion = variant == 3 ? first : variant == 4 ? second : unknown;
        break;
    case 6: case 7: case 8: case 16: case 22: case 23:
        transition.kind = KisPageTransitionKind::AcknowledgeLastUse;
        transition.completion = variant == 7 ? second : variant == 8 ? pending
            : variant == 16 ? unknown : variant == 23 ? KisCompletionTicket{} : first;
        acknowledge = variant != 22;
        break;
    case 9: transition.lease = {}; break;
    case 10: transition.lease = {99}; break;
    case 11: ++transition.target.allocation.generation; break;
    case 12: ++transition.target.providerEpoch.value; break;
    case 13: ++transition.version.defaultPixelRevision; break;
    case 14: ++transition.target.layout.formatId; break;
    case 15: ++transition.target.version.generation.value; break;
    default: break;
    }
    QVector<KisPageTransition> sequence{transition};
    if (variant == 19 || variant == 20 || variant == 21) {
        auto next = transition;
        next.lease = variant == 19 ? transition.lease : KisPageLeaseId{13};
        if (variant == 20) {
            next.kind = KisPageTransitionKind::ReleaseCapturedVersion;
            next.readView = {999};
        }
        sequence.append(next);
    }
    KisPageStateSnapshot expected = initial;
    bool accepted = true;
    for (const auto &step : sequence) {
        const auto result = KisPageStateMachine{}.apply(expected, step);
        if (!result.accepted) { accepted = false; break; }
        expected = result.next;
    }
    // The reference transition receives a ticket; production must first
    // validate terminal provenance and rejects the ordinary-owner entry.
    if (variant == 8 || variant == 22) accepted = false;
    if (!accepted) expected = initial;
    const auto result = acknowledge
        ? coordinator.acknowledgeLastUse(transition.version, transition.target,
                                         completions.verifyTerminal(transition.completion))
        : variant >= 18 && variant <= 21
            ? coordinator.applyOwnerSequence(initial.key, sequence)
            : coordinator.applyOwner(initial.key, transition);
    QCOMPARE(result.accepted, accepted);
    KisPageStateSnapshot actual;
    QVERIFY(coordinator.pageSnapshot(initial.key, &actual));
    comparePageRecords(actual, expected);
    if (variant == 17) {
        QVERIFY(!coordinator.applyOwner(initial.key, transition).accepted);
        QVERIFY(coordinator.pageSnapshot(initial.key, &actual));
        comparePageRecords(actual, expected);
    }
    if (variant == 6 || variant == 7) {
        QVERIFY(!coordinator.acknowledgeLastUse(transition.version, transition.target,
                                               completions.verifyTerminal(transition.completion)).accepted);
        QVERIFY(coordinator.pageSnapshot(initial.key, &actual));
        comparePageRecords(actual, expected);
    }
}

void KisPageStoreReferenceTest::metadataReadProtectionReclaimsEmptyBlocks()
{
    using Arena = KisShardSlotArena<KisMetadataOverflowNode, 16 * 1024>;
    KisCompletionRegistry completions;
    const auto ticket = completions.allocatePending(completions.registerSource(KisCompletionDomain::HostLogical));
    QVERIFY(completions.complete(ticket, KisCompletionStatus::Succeeded));
    auto initial = pageWithHistory(257);
    const auto handle = initial.versions.first().replicas.first().replica;
    const int count = int(3 * Arena::slotsPerBlock() + 7);
    initial.versions.first().replicas.first().pendingLastUses = QVector<KisCompletionTicket>(count, ticket);
    KisBackingBudgetController budget;
    KisPageMetadataCoordinator coordinator;
    coordinator.attachBackingBudget(budget);
    QVERIFY(coordinator.configure(1));
    QVERIFY(coordinator.registerPage(initial));
    const auto before = coordinator.footprint();
    QCOMPARE(before.overflowArena.activeBlocks, quint64(4));
    const auto beforeBytes = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    QVERIFY(coordinator.acknowledgeLastUse(handle.version, handle, completions.verifyTerminal(ticket)).accepted);
    const auto after = coordinator.footprint();
    QCOMPARE(after.overflowArena.usedSlots, quint64(0));
    QCOMPARE(after.overflowArena.activeBlocks, quint64(1));
    QCOMPARE(after.versionArena.attachedBlocks, before.versionArena.attachedBlocks);
    QCOMPARE(after.replicaArena.attachedBlocks, before.replicaArena.attachedBlocks);
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
             beforeBytes - 3 * Arena::blockByteSize());
    QCOMPARE(coordinator.metrics().readProtectionSlotReleases, quint64(count));
    QCOMPARE(coordinator.metrics().readProtectionNodeVisits, quint64(2 * count));

    // A live reservation forbids detaching its admitted empty payload. On
    // cancellation the existing arena cleanup can reclaim it; old slot IDs
    // stay invalid when the directory is later reused.
    Arena arena(4 * Arena::blockByteSize());
    Arena::PreparedDirectory directory;
    directory.prepare(arena.directoryCapacityForBlocks(4));
    QVERIFY(arena.installPreparedDirectory(&directory, 4));
    for (int i = 0; i < 4; ++i) {
        auto prepared = Arena::prepareBlock();
        QVERIFY(arena.attachPreparedBlock(&prepared));
    }
    const auto slot = arena.emplace(KisMetadataOverflowNode{});
    QVERIFY(slot.isValid());
    auto reservation = arena.reserveSlots(1);
    QVERIFY(reservation.isValid());
    Arena::ReleasedBlocks released;
    QVERIFY(arena.erase(slot, &released, 1));
    QVERIFY(released.isEmpty());
    QCOMPARE(arena.statistics().activeBlocks, quint64(4));
    arena.cancelReservation(&reservation);
    released = arena.takeEmptyBlocks(1);
    QCOMPARE(released.blockCount(), qsizetype(3));
    QVERIFY(!arena.get(slot));
    released = {};
    const auto next = arena.emplace(KisMetadataOverflowNode{});
    QVERIFY(next.isValid());
    QVERIFY(!arena.get(slot));
}

void KisPageStoreReferenceTest::metadataReadProtectionDeferredCleanup_data()
{
    QTest::addColumn<bool>("acknowledge");
    QTest::addColumn<int>("shards");
    QTest::addColumn<bool>("explicitClear");
    for (bool acknowledge : {false, true})
        for (int shards : {1, 8})
            for (bool clear : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("ack%1-shards%2-clear%3")
                    .arg(acknowledge).arg(shards).arg(clear))) << acknowledge << shards << clear;
}

void KisPageStoreReferenceTest::metadataReadProtectionDeferredCleanup()
{
    QFETCH(bool, acknowledge);
    QFETCH(int, shards);
    QFETCH(bool, explicitClear);
    using Arena = KisShardSlotArena<KisMetadataOverflowNode, 16 * 1024>;
    const int count = int(3 * Arena::slotsPerBlock() + 7);
    KisCompletionRegistry completions;
    const auto ticket = completions.allocatePending(completions.registerSource(KisCompletionDomain::HostLogical));
    QVERIFY(completions.complete(ticket, KisCompletionStatus::Succeeded));
    const auto terminal = completions.verifyTerminal(ticket);
    KisBackingBudgetController budget, foreignBudget;
    KisPageMetadataCoordinator coordinator, foreign;
    coordinator.attachBackingBudget(budget);
    foreign.attachBackingBudget(foreignBudget);
    QVERIFY(coordinator.configure(shards));
    QVERIFY(foreign.configure(shards));
    int secondKey = 1;
    while (shards > 1 && coordinator.shardFor(pageKey(secondKey)) == coordinator.shardFor(pageKey(0)))
        ++secondKey;
    QVector<KisPageStateSnapshot> pages;
    for (int key : {0, secondKey}) {
        const auto version = pageVersion(key, 1);
        auto page = initialPageState(version, replica(version, 1, 1, quint64(key + 1)));
        auto &record = page.versions.first().replicas.first();
        for (int i = 0; i < count; ++i) {
            if (acknowledge) record.pendingLastUses.append(ticket);
            else record.readLeases.append({quint64(i + 1)});
        }
        QVERIFY(coordinator.registerPage(page));
        pages.append(page);
    }
    QVERIFY(foreign.registerPage(pages.first()));
    const auto live = [](const KisBackingBudgetController &b) {
        return b.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const quint64 before = live(budget), foreignBefore = live(foreignBudget);
    const auto blocksBefore = coordinator.footprint().overflowArena.activeBlocks;
    const auto finish = [&](KisPageMetadataCoordinator &owner, const KisPageStateSnapshot &page,
                            KisPageMetadataReadCleanup &cleanup) {
        const auto &handle = page.versions.first().authority;
        if (acknowledge)
            return owner.acknowledgeLastUse(handle.version, handle, terminal, &cleanup).accepted;
        KisPageTransition release;
        release.kind = KisPageTransitionKind::ReleaseRead;
        release.version = handle.version;
        release.target = handle;
        for (int i = 0; i < count; ++i) {
            release.lease = {quint64(i + 1)};
            if (!owner.applyOwner(page.key, release, &cleanup).accepted) return false;
        }
        return true;
    };
    quint64 bytes = 0;
    {
        KisPageMetadataReadCleanup cleanup;
        QVERIFY(finish(coordinator, pages.first(), cleanup));
        QVERIFY(!cleanup.isEmpty());
        QCOMPARE(live(budget), before); // detached payload is still allocated
        const quint64 firstBytes = cleanup.byteSize();
        // A carrier from another coordinator cannot absorb this owner's debt,
        // even if the replica's complete identity happens to match.
        QVERIFY(!finish(foreign, pages.first(), cleanup));
        QCOMPARE(cleanup.byteSize(), firstBytes);
        KisPageStateSnapshot unchanged;
        QVERIFY(foreign.pageSnapshot(pages.first().key, &unchanged));
        comparePageRecords(unchanged, pages.first());
        QCOMPARE(live(foreignBudget), foreignBefore);
        QVERIFY(finish(coordinator, pages.last(), cleanup));
        QVERIFY(cleanup.byteSize() > firstBytes);
        bytes = (blocksBefore - coordinator.footprint().overflowArena.activeBlocks) * Arena::blockByteSize();
        QCOMPARE(cleanup.byteSize(), bytes);
        QCOMPARE(coordinator.footprint().overflowArena.usedSlots, quint64(0));
        QCOMPARE(live(budget), before);

        KisPageMetadataReadCleanup moved(std::move(cleanup));
        QVERIFY(cleanup.isEmpty());
        cleanup.clear();
        QCOMPARE(live(budget), before);
        KisPageMetadataReadCleanup destination;
        QVERIFY(finish(foreign, pages.first(), destination));
        const quint64 foreignBytes = destination.byteSize();
        QVERIFY(foreignBytes > 0);
        QCOMPARE(live(foreignBudget), foreignBefore);
        destination = std::move(moved); // old payload/charge released exactly once
        QVERIFY(moved.isEmpty());
        moved.clear();
        QCOMPARE(live(foreignBudget), foreignBefore - foreignBytes);
        QCOMPARE(destination.byteSize(), bytes);
        QCOMPARE(live(budget), before);
        if (explicitClear) {
            destination.clear();
            QVERIFY(destination.isEmpty());
            QCOMPARE(live(budget), before - bytes);
            destination.clear();
        }
    }
    QCOMPARE(live(budget), before - bytes);
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].reserved.cpuRam, quint64(0));
}

void KisPageStoreReferenceTest::metadataReadCleanupAfterController()
{
    QFETCH(bool, acknowledge);
    QFETCH(bool, explicitClear);
    using Arena = KisShardSlotArena<KisMetadataOverflowNode, 16 * 1024>;
    const int count = int(Arena::slotsPerBlock()) + 7;
    KisPageBackingLimits limits;
    limits.metadataArenaBytes = 1024 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    std::array<KisBackingBudgetReservation, 8> warm;
    for (auto &slot : warm) { slot = parent->reserve({}, nullptr); QVERIFY(slot.isValid()); }
    for (auto &slot : warm) slot.release();
    const auto live = [&] {
        return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const auto baseline = live();
    auto budget = std::make_unique<KisBackingBudgetController>(limits);
    QVERIFY(budget->configureSharedNonPayloadBudget(parent));
    auto metadata = std::make_unique<KisPageMetadataCoordinator>();
    metadata->attachBackingBudget(*budget);
    QVERIFY(metadata->configure(1));
    KisCompletionRegistry completions;
    const auto ticket = completions.allocatePending(completions.registerSource(KisCompletionDomain::HostLogical));
    QVERIFY(completions.complete(ticket, KisCompletionStatus::Succeeded));
    auto page = initialPageState(pageVersion(0, 1), replica(pageVersion(0, 1), 1, 1, 1));
    auto &record = page.versions.first().replicas.first();
    for (int i = 0; i < count; ++i) {
        if (acknowledge) record.pendingLastUses.append(ticket);
        else record.readLeases.append({quint64(i + 1)});
    }
    QVERIFY(metadata->registerPage(page));
    const auto before = live();
    auto cleanup = std::make_unique<KisPageMetadataReadCleanup>();
    if (acknowledge) {
        QVERIFY(metadata->acknowledgeLastUse(record.replica.version, record.replica,
            completions.verifyTerminal(ticket), cleanup.get()).accepted);
    } else {
        KisPageTransition release;
        release.kind = KisPageTransitionKind::ReleaseRead;
        release.version = record.replica.version;
        release.target = record.replica;
        for (int i = 0; i < count; ++i) {
            release.lease = {quint64(i + 1)};
            QVERIFY(metadata->applyOwner(page.key, release, cleanup.get()).accepted);
        }
    }
    QVERIFY(!cleanup->isEmpty());
    QCOMPARE(live(), before);
    metadata.reset();
    budget.reset();
    // Only the detached read value retains the authority and child here;
    // no shard or publication candidate supplies the accounting lifetime.
    QVERIFY(live() > baseline + cleanup->byteSize());
    const std::weak_ptr<KisBackingBudgetController> lifetime(parent);
    parent.reset();
    QVERIFY(!lifetime.expired());
    parent = lifetime.lock();
    QVERIFY(parent);
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - live());
    void *filler = kisAllocateMutationStorage(parent.get(), fillerBytes, 1);
    auto freeFiller = qScopeGuard([&] { kisFreeMutationStorage(parent.get(), filler, fillerBytes, 1); });
    QCOMPARE(live(), limits.metadataArenaBytes);
    if (explicitClear) { cleanup->clear(); QVERIFY(cleanup->isEmpty()); cleanup->clear(); }
    else cleanup.reset();
    QCOMPARE(live(), baseline + fillerBytes);
    QCOMPARE(parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].reserved.cpuRam, quint64(0));
    freeFiller.dismiss();
    kisFreeMutationStorage(parent.get(), filler, fillerBytes, 1);
    QCOMPARE(live(), baseline);
    parent.reset();
    QVERIFY(lifetime.expired());
}

void KisPageStoreReferenceTest::metadataReadProtectionReclaimerIdentity()
{
    auto initial = pageWithHistory(0);
    KisPageMetadataCoordinator coordinator;
    QVERIFY(coordinator.configure(1));
    QVERIFY(coordinator.registerPage(initial));
    for (int round = 0; round < 2; ++round) {
        KisPageTransition read;
        read.kind = KisPageTransitionKind::AcquireRead;
        read.version = initial.versions.first().version;
        read.target = initial.versions.first().authority;
        read.lease = {quint64(round + 1)};
        QVERIFY(coordinator.applyOwner(initial.key, read).accepted);
        read.kind = KisPageTransitionKind::ReleaseRead;
        QSemaphore started, finish, destroyed;
        std::atomic<bool> body{false}, capture{false}, outsider{true}, released{false};
        struct Capture {
            Capture(std::atomic<bool> *marked, QSemaphore *done) : marked(marked), done(done) {}
            ~Capture() { marked->store(kisOnPageStoreReclamationThread()); done->release(); }
            std::atomic<bool> *marked;
            QSemaphore *done;
        };
        kisSchedulePageStoreReclamation([&, witness = std::make_shared<Capture>(&capture, &destroyed)] {
            body.store(kisOnPageStoreReclamationThread());
            released.store(coordinator.applyOwner(initial.key, read).accepted);
            started.release();
            finish.acquire();
        });
        started.acquire();
        const bool mainMarked = kisOnPageStoreReclamationThread();
        std::thread fresh([&] { outsider.store(kisOnPageStoreReclamationThread()); });
        fresh.join();
        finish.release();
        destroyed.acquire();
        kisDrainPageStoreReclamation();
        QVERIFY(body.load() && capture.load() && released.load());
        QVERIFY(!mainMarked && !outsider.load() && !kisOnPageStoreReclamationThread());
    }
    QCOMPARE(coordinator.metrics().backgroundLocalTransitionSequences, quint64(2));
    QCOMPARE(coordinator.metrics().readProtectionTransitions, quint64(2));
}

void KisPageStoreReferenceTest::reclamationTaskStorageBeforeOwnerRelease()
{
    struct Result {
        std::unique_ptr<KisBackingBudgetController> budget =
            std::make_unique<KisBackingBudgetController>();
        quint64 baseline = 0;
        bool body = false;
        bool destroyed = false;
        bool finished = false;
        bool settledBeforeRelease = false;
    } result;
    struct alignas(128) Payload {
        std::array<quint64, 512> bytes{};
        Result *result;
        explicit Payload(Result *value) : result(value) { bytes.back() = 42; }
        Payload(Payload &&other) noexcept
            : bytes(other.bytes), result(std::exchange(other.result, nullptr)) {}
        ~Payload() { if (result) result->destroyed = kisOnPageStoreReclamationThread(); }
        void operator()()
        {
            result->body = kisOnPageStoreReclamationThread() && bytes.back() == 42 &&
                reinterpret_cast<quintptr>(this) % alignof(Payload) == 0;
        }
    };
    KisPageBackingLimits limits;
    limits.metadataArenaBytes = 16 * 1024;
    QVERIFY(result.budget->configureLimits(limits, nullptr));
    kisSchedulePageStoreReclamation([] {}, result.budget.get());
    kisDrainPageStoreReclamation();
    result.baseline = result.budget->usage().buckets[
        size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;

    QSemaphore entered, resume;
    kisSchedulePageStoreReclamation([&] { entered.release(); resume.acquire(); });
    entered.acquire();
    const auto unblock = qScopeGuard([&] { resume.release(); kisDrainPageStoreReclamation(); });
    kisSchedulePageStoreReclamation(Payload(&result), result.budget.get(),
        +[](void *context) {
            auto *result = static_cast<Result *>(context);
            result->settledBeforeRelease = result->destroyed && kisOnPageStoreReclamationThread() &&
                result->budget->usage().buckets[
                    size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam == result->baseline;
            result->budget.reset();
            result->finished = true;
        }, &result);
    const auto queued = result.budget->usage();
    QVERIFY(queued.buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam >=
            result.baseline + sizeof(Payload));
    QCOMPARE(queued.buckets[size_t(KisBackingBudgetClass::MetadataArena)].reserved.cpuRam, quint64(0));
    resume.release();
    kisDrainPageStoreReclamation();
    QVERIFY(result.body && result.destroyed && result.finished && result.settledBeforeRelease);
    QVERIFY(!result.budget);
}

void KisPageStoreReferenceTest::reclamationTaskRejectedPreparation()
{
    KisBackingBudgetController budget;
    KisPageBackingLimits limits;
    limits.metadataArenaBytes = 16 * 1024;
    QVERIFY(budget.configureLimits(limits, nullptr));
    kisSchedulePageStoreReclamation([] {}, &budget);
    kisDrainPageStoreReclamation();
    const auto live = [&] {
        return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const quint64 baseline = live();
    const size_t remaining = size_t(limits.metadataArenaBytes - baseline);
    void *filler = kisAllocateMutationStorage(&budget, remaining, 1);
    const auto releaseFiller = qScopeGuard([&] {
        if (filler) kisFreeMutationStorage(&budget, filler, remaining, 1);
    });
    bool invoked = false, finished = false;
    QVERIFY_EXCEPTION_THROWN(kisSchedulePageStoreReclamation([&] { invoked = true; }, &budget,
        +[](void *context) { *static_cast<bool *>(context) = true; }, &finished), std::bad_alloc);
    QCOMPARE(live(), limits.metadataArenaBytes);
    QVERIFY(!invoked && !finished);
    kisFreeMutationStorage(&budget, std::exchange(filler, nullptr), remaining, 1);
    struct RejectMove {
        RejectMove() = default;
        RejectMove(RejectMove &&) { throw std::bad_alloc(); }
        void operator()() {}
    };
    QVERIFY_EXCEPTION_THROWN(kisSchedulePageStoreReclamation(RejectMove{}, &budget,
        +[](void *context) { *static_cast<bool *>(context) = true; }, &finished), std::bad_alloc);
    QCOMPARE(live(), baseline);
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].reserved.cpuRam, quint64(0));
    QVERIFY(!finished);
    kisSchedulePageStoreReclamation([&] { invoked = kisOnPageStoreReclamationThread(); }, &budget,
        +[](void *context) { *static_cast<bool *>(context) = true; }, &finished);
    kisDrainPageStoreReclamation();
    QVERIFY(invoked && finished);
    QCOMPARE(live(), baseline);
}

void KisPageStoreReferenceTest::preparedReclamationRunsAtCapacity()
{
    KisPageBackingLimits limits;
    limits.metadataArenaBytes = 64 * 1024;
    KisBackingBudgetController budget(limits);
    auto owner = KisMutationStorageAllocator<char>::retained(&budget);
    const auto live = [&] {
        return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const quint64 baseline = live();
    std::atomic<int> calls{0}, destroyed{0};
    struct alignas(128) Function {
        std::array<char, 4096> bytes{};
        std::atomic<int> *calls, *destroyed;
        bool active = true;
        Function(std::atomic<int> *a, std::atomic<int> *b) : calls(a), destroyed(b) {}
        Function(Function &&other) : bytes(other.bytes), calls(other.calls), destroyed(other.destroyed),
            active(std::exchange(other.active, false)) {}
        ~Function() { if (active) ++*destroyed; }
        void operator()() { ++*calls; }
    };
    struct Repeat { KisPageReclamationJob *task = nullptr; int remaining = 64; } repeat;
    auto task = kisPreparePageStoreReclamation(Function(&calls, &destroyed), &budget,
        +[](void *p) {
            auto *repeat = static_cast<Repeat *>(p);
            if (--repeat->remaining) kisEnqueuePageStoreReclamation(repeat->task);
        }, &repeat);
    repeat.task = task.get();
    const quint64 taskBytes = live() - baseline;
    QVERIFY(taskBytes >= sizeof(Function));
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - live());
    void *filler = kisAllocateMutationStorage(&budget, fillerBytes, 1);
    const auto release = qScopeGuard([&] {
        kisDrainPageStoreReclamation();
        kisFreeMutationStorage(&budget, filler, fillerBytes, 1);
    });
    QCOMPARE(live(), limits.metadataArenaBytes);
    QVERIFY_THROWS_EXCEPTION(std::bad_alloc,
        kisPreparePageStoreReclamation([] {}, &budget));
    kisEnqueuePageStoreReclamation(task.get());
    kisDrainPageStoreReclamation();
    QCOMPARE(calls.load(), 64);
    QCOMPARE(destroyed.load(), 0);
    QCOMPARE(live(), limits.metadataArenaBytes);
    task.reset();
    QCOMPARE(destroyed.load(), 1);
    QCOMPARE(live(), limits.metadataArenaBytes - taskBytes);
}

void KisPageStoreReferenceTest::preparedReadinessAtCapacity()
{
    KisPageBackingLimits limits;
    limits.metadataArenaBytes = 64 * 1024;
    KisBackingBudgetController budget(limits);
    auto accounting = KisMutationStorageAllocator<char>::retained(&budget);
    const auto live = [&] {
        return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    int calls = 0, copies = 0;
    struct alignas(128) Function {
        std::array<char, 4096> payload{};
        int *calls, *copies;
        Function(int *a, int *b) : calls(a), copies(b) {}
        Function(const Function &other) : payload(other.payload), calls(other.calls), copies(other.copies) { ++*copies; }
        Function(Function &&) = default;
        void operator()() { Q_ASSERT(quintptr(this) % alignof(Function) == 0); ++*calls; }
    };
    KisPageReadinessCallback callback(Function(&calls, &copies), &budget);
    QVERIFY(callback.storageBytes() >= sizeof(Function));
    KisPageReadinessSignal signal(callback.storageAllocator<std::byte>());
    auto subscription = signal.subscribe(callback);
    QVERIFY(subscription.isValid());
    const quint64 preparedBytes = live();
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - live());
    void *filler = kisAllocateMutationStorage(&budget, fillerBytes, 1);
    const auto release = qScopeGuard([&] { kisFreeMutationStorage(&budget, filler, fillerBytes, 1); });
    QCOMPARE(live(), limits.metadataArenaBytes);
    for (int i = 0; i < 64; ++i) { auto retained = callback; retained(); }
    QCOMPARE(copies, 0);
    QCOMPARE(live(), limits.metadataArenaBytes);
    QVERIFY_THROWS_EXCEPTION(std::bad_alloc, KisPageReadinessSubscription{callback});
    QVERIFY_THROWS_EXCEPTION(std::bad_alloc, KisPageReclamationDelay(callback, &budget));
    KisPageReadinessCallback external([&] { ++calls; });
    QVERIFY_THROWS_EXCEPTION(std::bad_alloc, external.fund(&budget));
    external(); // Rejected sponsorship did not lose the original capture.
    signal.notify();
    QCOMPARE(calls, 66);
    QCOMPARE(copies, 0);
    QVERIFY(live() < limits.metadataArenaBytes);
    QCOMPARE(signal.subscriberCount(), qsizetype(0));
    subscription.reset();
    QVERIFY(live() >= fillerBytes + callback.storageBytes());
    QVERIFY(preparedBytes > callback.storageBytes());
}

void KisPageStoreReferenceTest::readinessRetainsOriginalPayer()
{
    KisPageBackingLimits limits;
    limits.metadataArenaBytes = 64 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    auto warm1 = parent->reserve({}, nullptr), warm2 = parent->reserve({}, nullptr);
    QVERIFY(warm1.isValid() && warm2.isValid()); warm1.release(); warm2.release();
    const auto live = [&] { return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const quint64 baseline = live();
    KisCompletionRegistry registry;
    const auto source = registry.registerSource(KisCompletionDomain::HostLogical);
    const auto ticket = registry.allocatePending(source);
    KisPageReadinessSubscription first, second;
    int calls = 0;
    auto child = std::make_unique<KisBackingBudgetController>();
    QVERIFY(child->configureSharedNonPayloadBudget(parent));
    QCOMPARE(registry.watchTerminal(ticket, KisPageReadinessCallback([&] { ++calls; }, child.get()), &first),
             KisPageReadinessStatus::Waiting);
    child.reset();
    QVERIFY(live() > baseline);
    {
        KisBackingBudgetController sibling;
        QVERIFY(sibling.configureSharedNonPayloadBudget(parent));
        // New subscriber uses its live payer, even though the signal's first
        // accounting controller is gone. No allocation from that dead owner.
        QCOMPARE(registry.watchTerminal(ticket, KisPageReadinessCallback([&] { ++calls; }, &sibling), &second),
                 KisPageReadinessStatus::Waiting);
        const size_t fillerBytes = size_t(limits.metadataArenaBytes - live());
        void *filler = kisAllocateMutationStorage(parent.get(), fillerBytes, 1);
        const auto release = qScopeGuard([&] { kisFreeMutationStorage(parent.get(), filler, fillerBytes, 1); });
        QCOMPARE(live(), limits.metadataArenaBytes);
        QVERIFY(registry.complete(ticket, KisCompletionStatus::Succeeded));
        QCOMPARE(calls, 2);
        QCOMPARE(registry.sourceStatistics(source).readinessCapacity, quint64(0));
        // The weak subscription still owns real state/control-block storage.
        const quint64 afterComplete = live();
        first.reset();
        QCOMPARE(live(), afterComplete);
        second.reset();
        QVERIFY(live() < afterComplete);
    }
    QCOMPARE(live(), baseline);
}

void KisPageStoreReferenceTest::retainedDeadlineInFlightAfterController()
{
    auto parent = std::make_shared<KisBackingBudgetController>();
    auto child = std::make_unique<KisBackingBudgetController>();
    QVERIFY(child->configureSharedNonPayloadBudget(parent));
    QSemaphore entered, resume, finished;
    KisPageReclamationDelay delay(
        KisPageReadinessCallback([&] { entered.release(); resume.acquire(); finished.release(); }, child.get()), child.get());
    const auto unblock = qScopeGuard([&] { resume.release(); delay.reset(); });
    QVERIFY(delay.arm(1));
    QVERIFY(entered.tryAcquire(1, 5000));
    delay.reset();
    child.reset();
    const std::weak_ptr<KisBackingBudgetController> lifetime(parent);
    parent.reset();
    QVERIFY(!lifetime.expired());
    resume.release();
    QVERIFY(finished.tryAcquire(1, 5000));
    QTRY_VERIFY(lifetime.expired());
}

void KisPageStoreReferenceTest::preparedDeadlineRearmsAtCapacity()
{
    KisPageBackingLimits limits; limits.metadataArenaBytes = 64 * 1024;
    KisBackingBudgetController budget(limits);
    auto warm = KisMutationStorageAllocator<char>::retained(&budget);
    std::atomic<int> calls{0};
    KisPageReclamationDelay delay(KisPageReadinessCallback([&] { ++calls; }, &budget), &budget);
    QVERIFY(delay.isValid());
    const auto live = [&] {
        return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - live());
    void *filler = kisAllocateMutationStorage(&budget, fillerBytes, 1);
    const auto release = qScopeGuard([&] { kisFreeMutationStorage(&budget, filler, fillerBytes, 1); });
    QCOMPARE(live(), limits.metadataArenaBytes);
    for (int i = 1; i <= 32; ++i) {
        QVERIFY(delay.arm(1));
        QTRY_COMPARE_WITH_TIMEOUT(calls.load(), i, 5000);
        QVERIFY(delay.takeReady());
        QVERIFY(!delay.takeReady());
        QCOMPARE(live(), limits.metadataArenaBytes);
        QVERIFY(delay.arm(60000));
        delay.cancel();
        QVERIFY(!delay.takeReady());
        QCOMPARE(live(), limits.metadataArenaBytes);
    }
}

void KisPageStoreReferenceTest::preparedDeadlineRejectsCancelledGeneration()
{
    KisBackingBudgetController budget;
    QSemaphore entered, resume;
    std::atomic<int> finished{0};
    KisPageReclamationDelay delay(KisPageReadinessCallback([&] {
        entered.release(); resume.acquire(); ++finished;
    }, &budget), &budget);
    const auto unblock = qScopeGuard([&] { resume.release(2); delay.reset(); });
    QVERIFY(delay.arm(1));
    QVERIFY(entered.tryAcquire(1, 5000)); // First generation already dispatched.
    delay.cancel();
    QVERIFY(delay.arm(1));
    QVERIFY(!delay.takeReady()); // An old callback cannot grant the new generation.
    resume.release();
    QVERIFY(entered.tryAcquire(1, 5000)); // The monitor now publishes the new generation.
    QVERIFY(delay.takeReady());
    QVERIFY(!delay.takeReady());
    delay.cancel();
    resume.release();
    QTRY_COMPARE_WITH_TIMEOUT(finished.load(), 2, 5000);
}

void KisPageStoreReferenceTest::retainedStorageWeakTailAndCapacity()
{
    KisPageBackingLimits limits;
    limits.metadataArenaBytes = 32 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    auto warm1 = parent->reserve({}, nullptr);
    auto warm2 = parent->reserve({}, nullptr);
    QVERIFY(warm1.isValid() && warm2.isValid());
    warm1.release(); warm2.release();
    const auto live = [&] {
        return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const quint64 baseline = live();
    bool destroyed = false;
    struct Value {
        explicit Value(bool *value) : destroyed(value) {}
        ~Value() { *destroyed = true; }
        bool *destroyed;
    };
    std::weak_ptr<Value> weak;
    {
        KisBackingBudgetController child;
        QVERIFY(child.configureSharedNonPayloadBudget(parent));
        auto strong = std::allocate_shared<Value>(
            KisMutationStorageAllocator<Value>::retained(&child), &destroyed);
        weak = strong;
        const quint64 before = live();
        strong.reset();
        QVERIFY(destroyed && weak.expired());
        QCOMPARE(live(), before); // The weak control allocation is still real.
    }
    QVERIFY(live() > baseline);
    {
        KisBackingBudgetController sibling;
        QVERIFY(sibling.configureSharedNonPayloadBudget(parent));
        auto storage = KisMutationStorageAllocator<int>::retained(&sibling);
        const size_t remaining = size_t(limits.metadataArenaBytes - live());
        void *filler = kisAllocateMutationStorage(parent.get(), remaining, 1);
        const auto release = qScopeGuard([&] {
            kisFreeMutationStorage(parent.get(), filler, remaining, 1);
        });
        QCOMPARE(live(), limits.metadataArenaBytes);
        QVERIFY_EXCEPTION_THROWN(storage.allocate(1), std::bad_alloc);
        weak.reset();
        QVERIFY(live() < limits.metadataArenaBytes);
        int *data = storage.allocate(1);
        storage.deallocate(data, 1);
    }
    QCOMPARE(live(), baseline);

    {
        KisBackingBudgetController child;
        QVERIFY(child.configureSharedNonPayloadBudget(parent));
        auto strong = std::allocate_shared<Value>(
            KisMutationStorageAllocator<Value>::retained(&child), &destroyed);
        weak = strong;
    }
    const std::weak_ptr<KisBackingBudgetController> lifetime(parent);
    parent.reset();
    QVERIFY(!lifetime.expired()); // Original parent survives; no replacement pool.
    weak.reset();
    QVERIFY(lifetime.expired());
}

void KisPageStoreReferenceTest::retainedStorageConcurrentTeardown()
{
    auto parent = std::make_shared<KisBackingBudgetController>();
    auto warm = parent->reserve({}, nullptr);
    auto warmSibling = parent->reserve({}, nullptr);
    QVERIFY(warm.isValid() && warmSibling.isValid());
    warm.release(); warmSibling.release();
    const auto live = [&] {
        return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const quint64 baseline = live();
    struct alignas(128) Block { char bytes[4096]; };
    for (int round = 0; round < 32; ++round) {
        {
            auto child = std::make_unique<KisBackingBudgetController>();
            QVERIFY(child->configureSharedNonPayloadBudget(parent));
            auto storage = KisMutationStorageAllocator<Block>::retained(child.get());
            Block *data = storage.allocate(4);
            QVERIFY(reinterpret_cast<quintptr>(data) % alignof(Block) == 0);
            QSemaphore start;
            std::thread release([&] { start.acquire(); storage.deallocate(data, 4); });
            start.release();
            child.reset();
            release.join();
            QVERIFY_EXCEPTION_THROWN(storage.allocate(1), std::bad_alloc);
        }
        QCOMPARE(live(), baseline);
    }
}

void KisPageStoreReferenceTest::retainedWakeInFlightAfterController()
{
    auto parent = std::make_shared<KisBackingBudgetController>();
    auto child = std::make_unique<KisBackingBudgetController>();
    QVERIFY(child->configureSharedNonPayloadBudget(parent));
    QSemaphore entered, resume, finished;
    auto wake = kisPreparePageStoreReclamationWake([&] {
        entered.release(); resume.acquire(); finished.release();
    }, child.get());
    const auto unblock = qScopeGuard([&] { resume.release(); wake.reset(); });
    wake.notify();
    QVERIFY(entered.tryAcquire(1, 5000));
    wake.reset();
    child.reset();
    const std::weak_ptr<KisBackingBudgetController> lifetime(parent);
    parent.reset();
    QVERIFY(!lifetime.expired());
    resume.release();
    QVERIFY(finished.tryAcquire(1, 5000));
    QTRY_VERIFY(lifetime.expired());
}

void KisPageStoreReferenceTest::metadataOwningCapacityIsBudgeted()
{
    QString error;
    quint64 oneCapacityBatch = 0;
    quint64 pageNodeBytes = 0;
    {
        KisBackingBudgetController budget;
        std::array<KisBackingBudgetReservation, 8> slots;
        for (auto &slot : slots) { slot = budget.reserve({}, nullptr); QVERIFY(slot.isValid()); }
        for (auto &slot : slots) slot.release();
        KisPageMetadataCoordinator metadata;
        metadata.attachBackingBudget(budget);
        QVERIFY2(metadata.configure(1, &error), qPrintable(error));
        const auto first = pageVersion(0, 1);
        QVERIFY2(metadata.registerPage(
                     initialPageState(first, replica(first, 1, 1, 1)), &error),
                 qPrintable(error));
        oneCapacityBatch = budget.usage()
            .buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
        const auto footprint = metadata.footprint();
        QVERIFY(oneCapacityBatch > 0);
        QVERIFY(footprint.owningCapacityBytes > 0);
        QCOMPARE(footprint.exactVersionIndex.capacity, quint64(64));
        QCOMPARE(footprint.physicalSlotIndex.capacity, quint64(64));
        const auto second = pageVersion(1, 1);
        QVERIFY(metadata.registerPage(initialPageState(second, replica(second, 1, 1, 2))));
        pageNodeBytes = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam - oneCapacityBatch;
        QVERIFY(pageNodeBytes > 0); // Actual page map node, without another arena/index batch.
    }

    KisPageBackingLimits limits;
    limits.metadataArenaBytes = 2 * oneCapacityBatch;
    KisBackingBudgetController budget(limits);
    auto storageOwner = KisMutationStorageAllocator<char>::retained(&budget);
    std::array<KisBackingBudgetReservation, 8> slots;
    for (auto &slot : slots) { slot = budget.reserve({}, nullptr); QVERIFY(slot.isValid()); }
    for (auto &slot : slots) slot.release();
    const quint64 controllerStorage = budget.usage().buckets[
        size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    QVERIFY(controllerStorage > 0);
    {
        KisPageMetadataCoordinator metadata;
        metadata.attachBackingBudget(budget);
        QVERIFY2(metadata.configure(1, &error), qPrintable(error));
        for (int i = 0; i < 64; ++i) {
            const auto version = pageVersion(i, 1);
            QVERIFY2(metadata.registerPage(
                         initialPageState(version, replica(version, 1, 1, quint64(i + 1))), &error),
                     qPrintable(error));
        }
        const auto before = budget.usage()
            .buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        QCOMPARE(before.live.cpuRam, oneCapacityBatch + 63 * pageNodeBytes);
        const size_t fillerBytes = size_t(limits.metadataArenaBytes - before.live.cpuRam);
        void *filler = kisAllocateMutationStorage(&budget, fillerBytes, 1);
        const auto freeFiller = qScopeGuard([&] { kisFreeMutationStorage(&budget, filler, fillerBytes, 1); });
        QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
                 limits.metadataArenaBytes);
        const auto rejected = pageVersion(64, 1);
        QVERIFY(!metadata.registerPage(
            initialPageState(rejected, replica(rejected, 1, 1, 65)), &error));
        QVERIFY(error.contains(QStringLiteral("storage")) || error.contains(QStringLiteral("budget")));
        QCOMPARE(metadata.pageCount(), qsizetype(64));
        const auto after = budget.usage()
            .buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        QCOMPARE(after.live.cpuRam, limits.metadataArenaBytes);
        QCOMPARE(after.reserved.cpuRam, quint64(0));
    }
    QCOMPARE(budget.usage()
                 .buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
             controllerStorage);
}

void KisPageStoreReferenceTest::metadataConfigurationStorageRefusal()
{
    QFETCH(int, shards);
    QFETCH(bool, firstAllocation);
    KisPageBackingLimits limits;
    limits.metadataArenaBytes = 4 * 1024 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    {
        // A cold facade must allow the original shared-budget/limit setup
        // before admitting its runtime core through configure().
        KisBackingBudgetController coldBudget;
        KisPageMetadataCoordinator cold;
        cold.attachBackingBudget(coldBudget);
        QVERIFY(coldBudget.configureSharedNonPayloadBudget(parent));
        QVERIFY(coldBudget.configureLimits(limits, nullptr));
        QVERIFY(cold.configure(shards));
    }
    KisBackingBudgetController budget(limits);
    QVERIFY(budget.configureSharedNonPayloadBudget(parent));
    auto storage = KisMutationStorageAllocator<char>::retained(&budget);
    std::array<KisBackingBudgetReservation, 8> warm;
    for (auto &slot : warm) { slot = budget.reserve({}, nullptr); QVERIFY(slot.isValid()); }
    for (auto &slot : warm) slot.release();
    const auto live = [&] {
        return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const auto baseline = live();
    quint64 requiredBytes = 0;
    {
        KisPageMetadataCoordinator measured;
        measured.attachBackingBudget(budget);
        QVERIFY(measured.configure(shards));
        requiredBytes = live() - baseline;
        QCOMPARE(measured.shardCount(), qsizetype(shards));
        QCOMPARE(measured.pageCount(), qsizetype(0));
    }
    QCOMPARE(live(), baseline);
    QVERIFY(requiredBytes > 1 && requiredBytes < limits.metadataArenaBytes - baseline);
    qInfo() << "BR1_METADATA_CONFIGURATION_BYTES" << shards << requiredBytes;
    const quint64 headroom = firstAllocation ? 1 : requiredBytes - 1;
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - baseline - headroom);
    char *filler = storage.allocate(fillerBytes);
    auto releaseFiller = qScopeGuard([&] { storage.deallocate(filler, fillerBytes); });
    const auto filled = live();
    KisPageMetadataCoordinator metadata;
    metadata.attachBackingBudget(budget);
    // Budget selection remains allocation-free; with one byte available the
    // first actual Private allocation is refused before authority or shards.
    QCOMPARE(live(), filled);
    QVERIFY(metadata.publicationHeads().empty());
    const auto version = pageVersion(0, 1);
    KisPageStateSnapshot snapshot;
    for (int attempt = 0; attempt < 3; ++attempt) {
        QString error;
        QVERIFY(!metadata.configure(shards, &error));
        QVERIFY(error.contains(QStringLiteral("budget")));
        QVERIFY(!metadata.isOperational());
        QCOMPARE(metadata.shardCount(), qsizetype(0));
        QCOMPARE(metadata.pageCount(), qsizetype(0));
        QCOMPARE(metadata.pageRegistrationCount(), quint64(0));
        QCOMPARE(metadata.shardFor(version.key), qsizetype(-1));
        QVERIFY(metadata.pageKeys().isEmpty());
        QVERIFY(metadata.shutdownReplicaHandles().isEmpty());
        QVERIFY(!metadata.pageSnapshot(version.key, &snapshot));
        QVERIFY(!metadata.cpuReadBinding(version));
        QVERIFY(!metadata.cpuReadReplica(version).isValid());
        QCOMPARE(metadata.metrics().acceptedTransitions, quint64(0));
        QCOMPARE(metadata.footprint().owningCapacityBytes, quint64(0));
        QCOMPARE(live(), filled);
        for (const auto &bucket : parent->usage().buckets)
            QCOMPARE(bucket.reserved.cpuRam, quint64(0));
    }
    releaseFiller.dismiss();
    storage.deallocate(filler, fillerBytes);
    QCOMPARE(live(), baseline);
    QVERIFY(metadata.configure(shards));
    QCOMPARE(live(), baseline + requiredBytes);
    QCOMPARE(metadata.shardCount(), qsizetype(shards));
    QVERIFY(!metadata.configure(shards));
    QVERIFY(metadata.registerPage(initialPageState(version, replica(version, 1, 1, 1))));
}

void KisPageStoreReferenceTest::metadataConfigurationLateCandidate()
{
    QFETCH(int, shards);
    QFETCH(bool, install);
    QFETCH(bool, mutation);
    KisPageBackingLimits limits;
    limits.metadataArenaBytes = 4 * 1024 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    std::array<KisBackingBudgetReservation, 8> warm;
    for (auto &slot : warm) { slot = parent->reserve({}, nullptr); QVERIFY(slot.isValid()); }
    for (auto &slot : warm) slot.release();
    const auto live = [&] {
        return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const auto baseline = live();
    auto budget = std::make_unique<KisBackingBudgetController>(limits);
    QVERIFY(budget->configureSharedNonPayloadBudget(parent));
    auto metadata = std::make_unique<KisPageMetadataCoordinator>();
    metadata->attachBackingBudget(*budget);
    QVERIFY(metadata->configure(shards));
    const KisPageTransaction transaction{{74}, {1}};
    using Versions = KisShardSlotArena<KisVersionRecord, 16 * 1024>;
    auto page = mutation ? initialPageState(pageVersion(0, 1), replica(pageVersion(0, 1), 1, 1, 1))
                         : pageWithHistory(int(4 * Versions::slotsPerBlock()) - 1);
    const auto target = replica(mutation ? pageVersion(0, 2) : KisPageVersion{page.key, {1}, 2}, 1, 1, 2);
    if (mutation) {
        page.versions.append({target.version, KisPagePublicationState::Prepared,
            {{target, KisReplicaValidity::Valid, {}, {}, 0, {}}}, target, transaction.id, {}});
        ++page.nextGeneration.value;
    } else {
        page.publishedDefaultPixelRevision = 1;
        for (auto &version : page.versions) {
            version.version.defaultPixelRevision = 1;
            version.authority.version = version.version;
            for (auto &copy : version.replicas) copy.replica.version = version.version;
        }
    }
    QVERIFY(metadata->registerPage(page));
    const auto directoryEntries = metadata->footprint().versionArena.directoryEntries;
    if (!mutation) QCOMPARE(metadata->footprint().versionArena.freeSlots, quint64(0));
    KisPageTransition transition;
    transition.kind = mutation ? KisPageTransitionKind::CommitTransaction : KisPageTransitionKind::ReplaceDefaultPixel;
    transition.version = target.version;
    if (mutation) transition.transaction = transaction.id;
    transition.imageEpoch = {2};
    auto candidate = mutation ? metadata->prepareMutation(transaction, &target.version, 1)
                             : metadata->preparePublication(transaction, {2}, {transition});
    QVERIFY(candidate.isValid());
    if (!mutation) QVERIFY(metadata->footprint().versionArena.directoryEntries > directoryEntries);
    KisPageMetadataCoordinator::DeferredPublicationCleanup cleanup;
    if (install) QVERIFY(mutation ? metadata->installMutation(std::move(candidate), transaction, nullptr, &cleanup)
                                 : metadata->installPublication(std::move(candidate), transaction, {2}, nullptr, &cleanup));
    metadata.reset();
    budget.reset();
    QVERIFY(live() > baseline);
    const auto retained = live();
    const std::weak_ptr<KisBackingBudgetController> lifetime(parent);
    parent.reset();
    QVERIFY(!lifetime.expired());
    parent = lifetime.lock();
    QVERIFY(parent);
    if (install) QCOMPARE(cleanup.clearBatch(1), qsizetype(1));
    else candidate = {};
    QVERIFY(retained > baseline);
    QCOMPARE(live(), baseline);
    QVERIFY(cleanup.isEmpty());
    parent.reset();
    QVERIFY(lifetime.expired());
}

void KisPageStoreReferenceTest::metadataArenaDirectoryGrowthAtCapacity()
{
    using Arena = KisShardSlotArena<int, 128>;
    KisPageBackingLimits limits;
    limits.metadataArenaBytes = 64 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    KisBackingBudgetController budget(limits);
    QVERIFY(budget.configureSharedNonPayloadBudget(parent));
    auto storage = KisMutationStorageAllocator<char>::retained(&budget);
    std::array<KisBackingBudgetReservation, 8> warm;
    for (auto &slot : warm) { slot = budget.reserve({}, nullptr); QVERIFY(slot.isValid()); }
    for (auto &slot : warm) slot.release();
    const auto live = [&] {
        return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const auto baseline = live();
    {
        Arena arena(8 * Arena::blockByteSize(), storage);
        QCOMPARE(live(), baseline); // Empty arenas have no directory allocation.
        Arena::PreparedDirectory first(storage);
        first.prepare(arena.directoryCapacityForBlocks(1));
        QVERIFY(arena.installPreparedDirectory(&first, 1));
        auto block = Arena::prepareBlock();
        QVERIFY(arena.attachPreparedBlock(&block));
        const auto id = arena.emplace(19);
        QVERIFY(id.isValid());
        int *const address = arena.get(id);
        QVERIFY(address);
        for (quint32 i = 1; i < Arena::slotsPerBlock(); ++i)
            QVERIFY(arena.emplace(int(i)).isValid());
        const auto original = live();
        quint64 bytes = 0;
        {
            Arena::PreparedDirectory measured(storage);
            measured.prepare(arena.directoryCapacityForBlocks(1));
            bytes = live() - original;
        }
        QCOMPARE(live(), original);
        QVERIFY(bytes > 1);
        const size_t shortFill = size_t(limits.metadataArenaBytes - original - bytes + 1);
        char *filler = storage.allocate(shortFill);
        auto release = qScopeGuard([&] { storage.deallocate(filler, shortFill); });
        const auto filled = live();
        Arena::PreparedDirectory next(storage);
        QVERIFY_EXCEPTION_THROWN(next.prepare(arena.directoryCapacityForBlocks(1)), std::bad_alloc);
        QCOMPARE(live(), filled);
        QCOMPARE(arena.get(id), address);
        QCOMPARE(*address, 19);
        QCOMPARE(arena.statistics().directoryEntries, quint64(1));
        release.dismiss();
        storage.deallocate(filler, shortFill);
        next.prepare(arena.directoryCapacityForBlocks(1));
        auto second = Arena::prepareBlock();
        const auto prepared = live();
        const size_t fullFill = size_t(limits.metadataArenaBytes - prepared);
        filler = storage.allocate(fullFill);
        auto releaseFull = qScopeGuard([&] { storage.deallocate(filler, fullFill); });
        QCOMPARE(live(), limits.metadataArenaBytes);
        QVERIFY(arena.installPreparedDirectory(&next, 1));
        QVERIFY(arena.attachPreparedBlock(&second));
        QVERIFY(arena.emplace(20).isValid());
        QCOMPARE(live(), limits.metadataArenaBytes); // Install allocates/frees no directory.
        QCOMPARE(arena.get(id), address);
        QCOMPARE(*address, 19);
        releaseFull.dismiss();
        storage.deallocate(filler, fullFill);
        next = Arena::PreparedDirectory(storage); // Retired array is freed outside the gate.

        Arena::PreparedDirectory stale(storage), competitor(storage);
        stale.prepare(arena.directoryCapacityForBlocks(1));
        competitor.prepare(arena.directoryCapacityForBlocks(3));
        QVERIFY(arena.installPreparedDirectory(&competitor, 3));
        for (int i = 0; i < 3; ++i) {
            auto other = Arena::prepareBlock();
            QVERIFY(arena.attachPreparedBlock(&other));
        }
        const auto competing = live();
        QVERIFY(!arena.installPreparedDirectory(&stale, 1));
        QCOMPARE(live(), competing);
        QCOMPARE(arena.statistics().directoryEntries, quint64(5));
        QCOMPARE(arena.get(id), address);
        stale.prepare(arena.directoryCapacityForBlocks(1));
        QVERIFY(arena.installPreparedDirectory(&stale, 1));
        auto last = Arena::prepareBlock();
        QVERIFY(arena.attachPreparedBlock(&last));
        QCOMPARE(arena.get(id), address);
        QCOMPARE(*address, 19);
    }
    QCOMPARE(live(), baseline);
    for (const auto &bucket : parent->usage().buckets)
        QCOMPARE(bucket.reserved.cpuRam, quint64(0));
}

void KisPageStoreReferenceTest::metadataShardIndexesEnforcePhysicalOwnership()
{
    KisPageMetadataCoordinator coordinator;
    QVERIFY(coordinator.configure(1));

    const auto firstVersion = pageVersion(0, 1);
    const auto firstReplica = replica(firstVersion, 91, 1, 7001);
    QVERIFY(coordinator.registerPage(initialPageState(firstVersion, firstReplica)));

    const auto aliasVersion = pageVersion(1, 1);
    const auto aliasReplica = replica(aliasVersion, 91, 1, 7001, 2);
    QVERIFY(!coordinator.registerPage(initialPageState(aliasVersion, aliasReplica)));

    const auto secondVersion = pageVersion(2, 1);
    const auto secondReplica = replica(secondVersion, 91, 1, 7002);
    QVERIFY(coordinator.registerPage(initialPageState(secondVersion, secondReplica)));

    const auto footprint = coordinator.footprint();
    QCOMPARE(footprint.pages, quint64(2));
    QCOMPARE(footprint.exactVersionIndex.entries, quint64(2));
    QCOMPARE(footprint.physicalSlotIndex.entries, quint64(2));
    QCOMPARE(footprint.exactVersionIndex.outstandingReservations, quint64(0));
    QCOMPARE(footprint.physicalSlotIndex.outstandingReservations, quint64(0));
}

void KisPageStoreReferenceTest::metadataSlotArenaGenerationAndRelease()
{
    struct TrackedRecord {
        explicit TrackedRecord(int value, std::atomic<int> *live)
            : value(value)
            , live(live)
        {
            live->fetch_add(1, std::memory_order_relaxed);
        }
        ~TrackedRecord()
        {
            live->fetch_sub(1, std::memory_order_relaxed);
        }
        int value = 0;
        std::atomic<int> *live = nullptr;
    };
    using Arena = KisShardSlotArena<TrackedRecord, 128>;
    static_assert(Arena::slotsPerBlock() >= 2);

    std::atomic<int> live{0};
    const quint64 oneBlockBudget = Arena::blockByteSize();
    typename Arena::SlotId releasedId;
    quint32 releasedDirectoryIndex = 0;
    {
        Arena arena(oneBlockBudget);
        Arena::PreparedDirectory directory;
        directory.prepare(arena.directoryCapacityForBlocks(1));
        QVERIFY(arena.installPreparedDirectory(&directory, 1));
        QVERIFY(arena.canAttachBlocks(1));
        auto candidate = Arena::prepareBlock();
        QVERIFY(candidate.isValid());
        QVERIFY(arena.attachPreparedBlock(&candidate));
        QVERIFY(!candidate.isValid());
        QVERIFY(!arena.canAttachBlocks(1));
        QCOMPARE(arena.statistics().attachedBlocks, quint64(1));

        auto reservation = arena.reserveSlots(1);
        QVERIFY(reservation.isValid());
        QCOMPARE(arena.statistics().outstandingReservations, quint64(1));
        const auto reserved = arena.emplaceReserved(&reservation, 10, &live);
        QVERIFY(reserved.isValid());
        QVERIFY(!reservation.isValid());
        QCOMPARE(arena.statistics().outstandingReservations, quint64(0));
        QVERIFY(arena.erase(reserved));

        const auto first = arena.emplace(11, &live);
        const auto second = arena.emplace(12, &live);
        QVERIFY(first.isValid());
        QVERIFY(second.isValid());
        QCOMPARE(live.load(std::memory_order_relaxed), 2);
        TrackedRecord *const stable = arena.get(first);
        QVERIFY(stable);
        QCOMPARE(stable->value, 11);

        QVERIFY(arena.erase(first));
        QVERIFY(!arena.get(first));
        const auto reused = arena.emplace(13, &live);
        QCOMPARE(reused.index, first.index);
        QVERIFY(reused.generation != first.generation);
        QVERIFY(!arena.get(first));
        QCOMPARE(arena.get(reused), stable);
        QCOMPARE(arena.get(reused)->value, 13);

        QVERIFY(arena.erase(reused));
        QVERIFY(arena.erase(second));
        QCOMPARE(live.load(std::memory_order_relaxed), 0);
        releasedId = reused;
        releasedDirectoryIndex = (releasedId.index - 1) / Arena::slotsPerBlock();
        auto detachGuard = arena.reserveSlots(1);
        QVERIFY(detachGuard.isValid());
        QVERIFY(arena.takeEmptyBlocks().isEmpty());
        arena.cancelReservation(&detachGuard);
        auto released = arena.takeEmptyBlocks();
        QCOMPARE(released.blockCount(), qsizetype(1));
        QCOMPARE(released.byteSize(), oneBlockBudget);
        auto stats = arena.statistics();
        QCOMPARE(stats.activeBlocks, quint64(0));
        QCOMPARE(stats.allocatedBytes, quint64(0));
        QCOMPARE(stats.releasedBytes, oneBlockBudget);

        auto replacement = Arena::prepareBlock();
        QVERIFY(arena.attachPreparedBlock(&replacement));
        QCOMPARE(arena.statistics().attachedBlocks, quint64(2));
        const auto afterBlockReuse = arena.emplace(14, &live);
        QCOMPARE((afterBlockReuse.index - 1) / Arena::slotsPerBlock(), releasedDirectoryIndex);
        QVERIFY(afterBlockReuse.generation != releasedId.generation);
        QVERIFY(!arena.get(releasedId));

        // A prepared extra block remains caller-owned after a hard-budget
        // rejection and is consequently freed after the shard lock is left.
        auto excess = Arena::prepareBlock();
        QVERIFY(!arena.attachPreparedBlock(&excess));
        QVERIFY(excess.isValid());
        QCOMPARE(arena.statistics().rejectedBlockAttaches, quint64(1));

        arena.close();
        QVERIFY(arena.isClosed());
        QVERIFY(!arena.emplace(15, &live).isValid());
        auto drained = arena.drain();
        QCOMPARE(drained.blockCount(), qsizetype(1));
        QCOMPARE(live.load(std::memory_order_relaxed), 0);
        QCOMPARE(arena.statistics().usedSlots, quint64(0));
        QCOMPARE(arena.statistics().allocatedBytes, quint64(0));
    }

    // Owner quiescence is sufficient for destruction; a separate close call
    // is not required to run the remaining value destructors.
    {
        Arena arena(oneBlockBudget);
        Arena::PreparedDirectory directory;
        directory.prepare(arena.directoryCapacityForBlocks(1));
        QVERIFY(arena.installPreparedDirectory(&directory, 1));
        auto candidate = Arena::prepareBlock();
        QVERIFY(arena.attachPreparedBlock(&candidate));
        QVERIFY(arena.emplace(21, &live).isValid());
        QCOMPARE(live.load(std::memory_order_relaxed), 1);
    }
    QCOMPARE(live.load(std::memory_order_relaxed), 0);

    // The arena intentionally delegates synchronization to the shard. This
    // stresses concurrent prepare outside the lock and attach/use under it.
    Arena concurrentArena(oneBlockBudget * 8);
    Arena::PreparedDirectory directory;
    directory.prepare(concurrentArena.directoryCapacityForBlocks(8));
    QVERIFY(concurrentArena.installPreparedDirectory(&directory, 8));
    QMutex shardMutex;
    std::atomic<int> completed{0};
    std::vector<std::thread> workers;
    for (int thread = 0; thread < 8; ++thread) {
        workers.emplace_back([&, thread] {
            for (int iteration = 0; iteration < 128; ++iteration) {
                auto candidate = Arena::prepareBlock();
                QMutexLocker locker(&shardMutex);
                auto id = concurrentArena.emplace(thread * 1000 + iteration, &live);
                if (!id.isValid() && candidate.isValid()) {
                    concurrentArena.attachPreparedBlock(&candidate);
                    id = concurrentArena.emplace(thread * 1000 + iteration, &live);
                }
                if (id.isValid() && concurrentArena.get(id) && concurrentArena.erase(id)) {
                    completed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto &worker : workers)
        worker.join();
    QCOMPARE(completed.load(std::memory_order_relaxed), 8 * 128);
    QCOMPARE(live.load(std::memory_order_relaxed), 0);
    QCOMPARE(concurrentArena.statistics().usedSlots, quint64(0));
    auto released = concurrentArena.takeEmptyBlocks();
    QVERIFY(!released.isEmpty());
    QCOMPARE(concurrentArena.statistics().allocatedBytes, quint64(0));
}

void KisPageStoreReferenceTest::indexedHistoricalDiscard()
{
    QFETCH(int, variant);
    auto page = pageWithHistory(257);
    auto &target = page.versions.last();
    const auto version = target.version;
    KisPageTransition discard;
    discard.kind = KisPageTransitionKind::DiscardHistoricalVersions;
    discard.versions = {page.versions.at(2).version, version};
    KisCompletionRegistry completions;
    const KisCompletionDomain source = KisCompletionDomain::HostLogical;
    const auto ticket = completions.allocatePending(completions.registerSource(source));
    switch (variant) {
    case 1:
        target.capturedReadViews.append({19});
        break;
    case 2:
        target.replicas.first().readLeases.append({11});
        break;
    case 3:
        target.replicas.first().pinCount = 1;
        break;
    case 4:
        target.replicas.first().pendingLastUses.append(ticket);
        break;
    case 5:
        discard.versions.append(version);
        break;
    case 6:
        discard.versions.append(page.versions.first().version);
        break;
    case 7:
        discard.versions.append(pageVersion(99, 1));
        break;
    case 8:
        discard.transaction = {17};
        break;
    case 9:
        discard.operation = {17};
        break;
    case 10:
        discard.versions = {pageVersion(0, 5000)};
        break;
    case 11:
        discard.versions.clear();
        break;
    }
    KisPageTransition detach;
    if (variant >= 12) {
        target.publication = KisPagePublicationState::Prepared;
        target.preparedBy = {74};
        detach.kind = KisPageTransitionKind::DetachPreparedVersion;
        detach.transaction = {quint64(variant == 13 ? 75 : 74)};
        detach.version = version;
        discard.versions = {version};
        if (variant == 14)
            detach.version = page.versions.first().version; // cannot detach current root
        if (variant == 15)
            target.capturedReadViews.append({19});
        if (variant == 16)
            target.replicas.first().readLeases.append({11});
        if (variant == 17)
            discard.versions.append(version);
        if (variant == 18)
            detach.operation = {75};
        if (variant == 19) {
            KisPageTransition write;
            write.kind = KisPageTransitionKind::AcquireWrite;
            write.baseVersion = page.versions.first().version;
            write.source = page.versions.first().authority;
            write.version = {page.key, page.nextGeneration, 0};
            write.target = replica(write.version, 1, 1, 20000);
            write.transaction = {76};
            write.operation = {19};
            write.writer = {19};
            const auto result = KisPageStateMachine().apply(page, write);
            QVERIFY(result.accepted);
            page = result.next;
        }
    }
    const KisPageStateMachine machine;
    QString error;
    QVERIFY2(machine.validateInvariants(page, &error), qPrintable(error));
    KisPageMetadataCoordinator coordinator;
    MetadataEffectReceiver receiver;
    attachEffects(coordinator, receiver);
    QVERIFY(coordinator.configure(4));
    QVERIFY(coordinator.registerPage(page));
    if (variant >= 12) {
        const auto expectedDetach = machine.apply(page, detach);
        const auto detached = coordinator.applyOwner(page.key, detach);
        QCOMPARE(detached.accepted, expectedDetach.accepted);
        QCOMPARE(detached.rejectionReason, expectedDetach.rejectionReason);
        QVERIFY(receiver.effects.isEmpty());
        if (detached.accepted) page = expectedDetach.next;
    }
    const auto expected = machine.apply(page, discard);
    const auto result = coordinator.applyOwner(page.key, discard);
    QCOMPARE(result.accepted, expected.accepted);
    QCOMPARE(result.rejectionReason, expected.rejectionReason);
    QCOMPARE(receiver.effects.size(), expected.effects.size());
    QCOMPARE(coordinator.metrics().fullSnapshotExports, quint64(0));
    QVERIFY(coordinator.metrics().localVersionInputs <= (variant >= 12 ? 6 : 5));
    QCOMPARE(coordinator.metrics().localVersionRemovals, quint64(expected.accepted ? discard.versions.size() : 0));
    KisPageStateSnapshot actual;
    QVERIFY(coordinator.pageSnapshot(page.key, &actual));
    comparePageRecords(actual, expected.next);
}

void KisPageStoreReferenceTest::indexedMutationTransitions_data()
{
    QTest::addColumn<int>("history");
    QTest::addColumn<int>("variant");
    for (int h : {0, 17, 257})
        for (int v = 0; v < 16; ++v)
            QTest::newRow(qPrintable(QStringLiteral("history%1-case%2").arg(h).arg(v))) << h << v;
}

void KisPageStoreReferenceTest::indexedMutationTransitions()
{
    QFETCH(int, history);
    QFETCH(int, variant);
    auto expected = pageWithHistory(history);
    const auto initial = expected; // immutable external diagnostic copy
    KisPageMetadataCoordinator coordinator;
    MetadataEffectReceiver receiver;
    attachEffects(coordinator, receiver);
    QVERIFY(coordinator.configure(4));
    QVERIFY(coordinator.registerPage(initial));
    const KisPageStateMachine machine;
    auto run = [&](const QVector<KisPageTransition> &transitions) {
        auto next = expected;
        QVector<KisPageTransitionEffect> effects;
        bool accepted = true;
        QString rejection;
        for (const auto &transition : transitions) {
            const auto step = machine.apply(next, transition);
            if (!step.accepted) {
                accepted = false;
                rejection = step.rejectionReason;
                effects.clear();
                break;
            }
            next = step.next;
            effects += step.effects;
        }
        const auto before = coordinator.metrics();
        receiver.effects.clear();
        KisPageMetadataTransitionResult result;
        if (transitions.size() == 1 && transitions.first().kind == KisPageTransitionKind::CommitTransaction) {
            const KisPageTransaction tx{transitions.first().transaction, {1}};
            auto publication = coordinator.preparePublication(tx, transitions.first().imageEpoch, transitions);
            result.accepted = publication.isValid()
                && coordinator.installPublication(std::move(publication), tx, transitions.first().imageEpoch);
        } else {
            result = coordinator.applyOwnerSequence(expected.key, transitions);
        }
        QCOMPARE(result.accepted, accepted);
        QCOMPARE(result.rejectionReason, rejection);
        QCOMPARE(receiver.effects.size(), effects.size());
        for (qsizetype i = 0; i < effects.size(); ++i) {
            QCOMPARE(receiver.effects[i].replica, effects[i].replica);
            QCOMPARE(receiver.effects[i].lastUse, effects[i].lastUse);
        }
        if (accepted)
            expected = next;
        KisPageStateSnapshot actual;
        QVERIFY(coordinator.pageSnapshot(expected.key, &actual));
        QVERIFY(machine.validateInvariants(actual));
        comparePageRecords(actual, expected);
        const auto after = coordinator.metrics();
        QVERIFY(after.localVersionInputs - before.localVersionInputs <= 4);
        if (!accepted)
            QCOMPARE(after.localVersionInstalls, before.localVersionInstalls);
        comparePageRecords(initial, pageWithHistory(history));
    };
    KisPageTransition write;
    write.kind = KisPageTransitionKind::AcquireWrite;
    write.baseVersion = initial.versions.first().version;
    write.source = initial.versions.first().authority;
    write.version = pageVersion(0, initial.nextGeneration.value);
    write.target = replica(write.version, 1, 1, write.version.generation.value);
    write.transaction = {71};
    write.writer = {72};
    write.operation = {73};
    auto prepare = write;
    prepare.kind = KisPageTransitionKind::PrepareWrite;
    auto begin = write;
    begin.kind = KisPageTransitionKind::BeginPublish;
    auto publish = write;
    publish.kind = KisPageTransitionKind::PublishWrite;
    auto cancel = write;
    cancel.kind = KisPageTransitionKind::CancelWrite;
    if (variant == 8) {
        auto collision = write;
        collision.target.allocation = initial.versions.last().authority.allocation;
        ++collision.target.allocation.generation;
        collision.target.domain = KisPageAccessDomain::UmaShared;
        run({collision});
        run({write, prepare});
        run({cancel});
        return;
    }
    if (variant == 9) {
        auto wrong = prepare;
        ++wrong.writer.value;
        run({write, wrong});
        run({write, prepare});
        run({cancel});
        return;
    }
    if (variant == 15) {
        auto foreign = write;
        foreign.baseVersion.key.page.column = 99;
        run({foreign});
        run({write, prepare});
        run({cancel});
        return;
    }
    if (variant == 10 || variant == 13) {
        auto alias = write;
        alias.kind = KisPageTransitionKind::AdoptPreparedWrite;
        alias.writeMode = KisPageWriteMode::DiscardContents;
        run({alias});
        KisPageTransition replace;
        replace.kind = KisPageTransitionKind::ReplacePrivatePreparedBacking;
        replace.version = write.version;
        replace.source = write.target;
        replace.transaction = write.transaction;
        replace.target = replica(write.version, 1, 1, write.target.allocation.slot + 1);
        if (variant == 13)
            replace.target.allocation = initial.versions.last().authority.allocation;
        run({replace});
    } else {
        if (variant == 1) {
            run({write});
            run({cancel});
            return;
        }
        run({write, prepare});
        if (variant == 2) {
            run({cancel});
            return;
        }
        if (variant == 14) {
            auto wrong = begin;
            ++wrong.writer.value;
            run({wrong});
        }
        run({begin});
        if (variant == 3) {
            run({cancel});
            run({publish});
            return;
        }
        if (variant == 4) {
            auto fail = write;
            fail.kind = KisPageTransitionKind::FailWrite;
            run({fail});
            return;
        }
        run({publish});
    }
    KisPageTransition capture;
    capture.kind = KisPageTransitionKind::RetainCapturedVersion;
    capture.version = write.version;
    capture.transaction = write.transaction;
    capture.readView = {81};
    if (variant == 6 || variant == 7 || variant == 11 || variant == 12)
        run({capture});
    if (variant == 11)
        run({capture}); // duplicate token must still reject
    if (variant == 12) {
        auto wrong = capture;
        wrong.kind = KisPageTransitionKind::ReleaseCapturedVersion;
        ++wrong.readView.value;
        run({wrong});
    }
    if (variant == 7) {
        auto read = capture;
        read.kind = KisPageTransitionKind::AcquireRead;
        read.target = write.target;
        read.lease = {91};
        run({read}); // owner read projects the exact version and lease-conflict witness
    }
    auto finish = capture;
    finish.kind = variant == 0 ? KisPageTransitionKind::CommitTransaction
        : variant == 7         ? KisPageTransitionKind::DetachPreparedVersion
                               : KisPageTransitionKind::AbortPreparedVersion;
    if (variant == 0)
        finish.imageEpoch = {2};
    run({finish});
    if (variant == 6 || variant == 7 || variant == 11 || variant == 12) {
        capture.kind = KisPageTransitionKind::ReleaseCapturedVersion;
        run({capture});
    }
}

void KisPageStoreReferenceTest::concurrentRetirementDebtRefusalPreservesPage()
{
    struct Debt {
        std::atomic<int> entered{0};
        std::atomic<int> committed{0}, cancelled{0};
    } debt;
    KisBackingBudgetController budget;
    KisPageMetadataCoordinator coordinator;
    coordinator.attachBackingBudget(budget);
    coordinator.attachRetirementDebtOwner(&debt,
        +[](void *p, const KisPageTransitionEffect *, qsizetype, quint64 *, QString *error) {
            auto &d = *static_cast<Debt *>(p);
            d.entered.fetch_add(1, std::memory_order_acq_rel);
            while (d.entered.load(std::memory_order_acquire) != 2) std::this_thread::yield();
            *error = QStringLiteral("retirement receiver refuses capacity");
            return false;
        }, +[](void *p, quint64, const KisPageTransitionEffect *, qsizetype) noexcept {
            static_cast<Debt *>(p)->committed.fetch_add(1);
        }, +[](void *p, quint64) noexcept {
            static_cast<Debt *>(p)->cancelled.fetch_add(1);
        });
    QVERIFY(coordinator.configure(1));
    const auto initial = pageWithHistory(1);
    QVERIFY(coordinator.registerPage(initial));
    KisPageTransition discard;
    discard.kind = KisPageTransitionKind::DiscardHistoricalVersions;
    discard.versions = {initial.versions.last().version};
    const QVector<KisPageTransition> transitions{discard};
    const auto before = coordinator.metrics();
    KisPageMetadataTransitionResult results[2];
    std::thread first([&] { results[0] = coordinator.applyOwnerSequence(initial.key, transitions); });
    std::thread second([&] { results[1] = coordinator.applyOwnerSequence(initial.key, transitions); });
    first.join();
    second.join();
    QCOMPARE(debt.entered.load(), 2);
    for (const auto &result : results) {
        QVERIFY(!result.accepted);
        QCOMPARE(result.rejectionReason, QStringLiteral("retirement receiver refuses capacity"));
    }
    QCOMPARE(debt.committed.load(), 0);
    QCOMPARE(debt.cancelled.load(), 0);
    QCOMPARE(coordinator.metrics().rejectedTransitions, before.rejectedTransitions + 2);
    KisPageStateSnapshot observed;
    QVERIFY(coordinator.pageSnapshot(initial.key, &observed));
    comparePageRecords(observed, initial);
}

void KisPageStoreReferenceTest::genericWorkingStorageRefusalIsAtomic()
{
    QFETCH(int, variant);
    KisPageBackingLimits limits; limits.metadataArenaBytes = 2 * 1024 * 1024;
    KisBackingBudgetController budget(limits);
    struct Debt {
        KisBackingBudgetController *budget;
        quint64 limit;
        bool fill = true;
        void *filler = nullptr;
        size_t bytes = 0;
        int prepared = 0, committed = 0, cancelled = 0;
        MetadataEffectReceiver receiver;
    } debt{&budget, limits.metadataArenaBytes, true, nullptr, 0, 0, 0, 0, {}};
    KisPageMetadataCoordinator coordinator;
    coordinator.attachBackingBudget(budget);
    // Synthetic metadata oracle: this receiver owns its effect contract.
    // Actual provider/backing transfer remains in the ledger/queue tests.
    coordinator.attachRetirementDebtOwner(&debt,
        +[](void *p, const KisPageTransitionEffect *effects, qsizetype count, quint64 *cookie, QString *error) {
            auto &d = *static_cast<Debt *>(p);
            MetadataEffectReceiver::prepare(&d.receiver, effects, count, cookie, error);
            ++d.prepared;
            if (d.fill) {
                const auto live = d.budget->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
                d.bytes = size_t(d.limit - live);
                d.filler = kisAllocateMutationStorage(d.budget, d.bytes, 1);
            }
            return true;
        }, +[](void *p, quint64 cookie, const KisPageTransitionEffect *effects, qsizetype count) noexcept {
            auto &d = *static_cast<Debt *>(p); ++d.committed;
            MetadataEffectReceiver::commit(&d.receiver, cookie, effects, count);
        }, +[](void *p, quint64 cookie) noexcept {
            auto &d = *static_cast<Debt *>(p); ++d.cancelled;
            MetadataEffectReceiver::cancel(&d.receiver, cookie);
        });
    QVERIFY(coordinator.configure(1));
    auto initial = pageWithHistory(17);
    auto &current = initial.versions.first();
    KisCompletionRegistry completions;
    const auto lastUse = completions.allocatePending(completions.registerSource(KisCompletionDomain::CpuJob));
    for (quint64 i = 1; i <= 64; ++i) current.replicas.first().readLeases.append({i});
    current.replicas.first().pendingLastUses.append(lastUse);
    current.capturedReadViews.append({200});
    QVector<KisPageTransition> transitions;
    if (variant == 0) {
        KisPageTransition read;
        read.kind = KisPageTransitionKind::AcquireRead;
        read.version = current.version; read.target = current.authority; read.lease = {1000};
        auto release = read; release.kind = KisPageTransitionKind::ReleaseRead;
        transitions = {read, release};
    } else {
        KisPageTransition write;
        write.kind = KisPageTransitionKind::AcquireWrite;
        write.baseVersion = current.version; write.source = current.authority;
        write.version = pageVersion(0, initial.nextGeneration.value);
        write.target = replica(write.version, 1, 1, 100000);
        write.transaction = {71}; write.writer = {72}; write.operation = {73};
        if (variant == 1) {
            ++initial.nextGeneration.value;
            auto prepared = initialPageState(write.version, write.target).versions.first();
            prepared.publication = KisPagePublicationState::Prepared;
            prepared.preparedBy = write.transaction;
            initial.versions.append(prepared);
            auto replacement = write;
            replacement.kind = KisPageTransitionKind::ReplacePrivatePreparedBacking;
            replacement.source = write.target;
            replacement.target.allocation.slot++;
            transitions = {replacement};
        } else {
            auto prepare = write; prepare.kind = KisPageTransitionKind::PrepareWrite;
            auto cancel = write; cancel.kind = KisPageTransitionKind::CancelWrite;
            transitions = {write, prepare, cancel};
        }
    }
    QVERIFY(KisPageStateMachine().validateInvariants(initial));
    QVERIFY(coordinator.registerPage(initial));
    const auto live = [&] { return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const quint64 baseline = live();
    bool accepted = false;
    for (size_t headroom : {size_t(0), size_t(512), size_t(2048), size_t(8192), size_t(65536)}) {
        const size_t bytes = size_t(limits.metadataArenaBytes - live()) - headroom;
        void *filler = kisAllocateMutationStorage(&budget, bytes, 1);
        const auto free = qScopeGuard([&] {
            kisFreeMutationStorage(&budget, filler, bytes, 1);
            kisFreeMutationStorage(&budget, std::exchange(debt.filler, nullptr), debt.bytes, 1);
        });
        const auto before = coordinator.metrics();
        const auto result = coordinator.applyOwnerSequence(initial.key, transitions);
        accepted = result.accepted;
        if (headroom == 0 || variant != 0) {
            QVERIFY(!result.accepted);
            QVERIFY(!result.rejectionReason.isEmpty());
            QVERIFY(debt.receiver.effects.isEmpty());
            QCOMPARE(coordinator.metrics().localVersionInstalls, before.localVersionInstalls);
            KisPageStateSnapshot actual; QVERIFY(coordinator.pageSnapshot(initial.key, &actual));
            comparePageRecords(actual, initial);
        }
        if (accepted) break;
    }
    if (variant == 0) QVERIFY(accepted);
    else {
        QVERIFY(debt.prepared > 0);
        QCOMPARE(debt.cancelled, debt.prepared); QCOMPARE(debt.committed, 0);
    }
    QCOMPARE(live(), baseline); // Every failed working value returned its actual fee.
    debt.fill = false;
    auto expected = initial;
    QVector<KisPageTransitionEffect> effects;
    for (const auto &transition : transitions) {
        const auto step = KisPageStateMachine().apply(expected, transition);
        QVERIFY(step.accepted); expected = step.next; effects += step.effects;
    }
    const auto result = coordinator.applyOwnerSequence(initial.key, transitions);
    QVERIFY2(result.accepted, qPrintable(result.rejectionReason));
    QCOMPARE(debt.receiver.effects.size(), effects.size());
    for (qsizetype i = 0; i < effects.size(); ++i) QCOMPARE(debt.receiver.effects[i].replica, effects[i].replica);
    KisPageStateSnapshot actual; QVERIFY(coordinator.pageSnapshot(initial.key, &actual));
    comparePageRecords(actual, expected);
    if (variant != 0) QCOMPARE(debt.committed, 1);
}

void KisPageStoreReferenceTest::capturedProtectionAndExactQueryAtCapacity()
{
    using Arena = KisShardSlotArena<KisMetadataOverflowNode, 16 * 1024>;
    KisPageBackingLimits limits; limits.metadataArenaBytes = 1024 * 1024;
    KisBackingBudgetController budget(limits);
    KisPageMetadataCoordinator coordinator;
    coordinator.attachBackingBudget(budget);
    QVERIFY(coordinator.configure(1));
    auto page = initialPageState(pageVersion(0, 1), replica(pageVersion(0, 1), 1, 1, 1));
    auto prepared = initialPageState(pageVersion(0, 2), replica(pageVersion(0, 2), 1, 1, 2)).versions.first();
    prepared.publication = KisPagePublicationState::Prepared;
    prepared.preparedBy = {71};
    const int count = int(2 * Arena::slotsPerBlock());
    for (int i = 0; i < count; ++i) prepared.capturedReadViews.append({quint64(i + 1)});
    page.versions.append(prepared); page.nextGeneration = {3};
    QVERIFY(coordinator.registerPage(page));
    QCOMPARE(coordinator.footprint().overflowArena.freeSlots, quint64(0));
    const auto live = [&] {
        return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const quint64 before = live();
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - before);
    void *filler = kisAllocateMutationStorage(&budget, fillerBytes, 1);
    const auto freeFiller = qScopeGuard([&] { kisFreeMutationStorage(&budget, filler, fillerBytes, 1); });
    QCOMPARE(live(), limits.metadataArenaBytes);
    KisPageTransition claim;
    claim.kind = KisPageTransitionKind::RetainCapturedVersion;
    claim.version = prepared.version; claim.transaction = prepared.preparedBy;
    claim.readView = {quint64(count + 1)};
    const auto metrics = coordinator.metrics();
    QVERIFY(!coordinator.applyOwner(page.key, claim).accepted);
    QCOMPARE(coordinator.metrics().rejectedTransitions, metrics.rejectedTransitions + 1);
    QCOMPARE(coordinator.footprint().overflowArena.usedSlots, quint64(count));
    KisPageMetadataCoordinator::VersionInfo info;
    QVERIFY(coordinator.versionSnapshot(prepared.version, &info));
    QVERIFY(info.captured); QCOMPARE(info.authority, prepared.authority);
    QCOMPARE(coordinator.cpuReadReplica(prepared.version), prepared.authority);
    KisPageMetadataCoordinator::ReplicaCandidates candidates{
        KisMutationStorageAllocator<KisPageMetadataCoordinator::ReplicaCandidate>(&budget)};
    info.preparedBy = {99};
    QVERIFY_EXCEPTION_THROWN(coordinator.versionSnapshot(prepared.version, &info, &candidates), std::bad_alloc);
    QCOMPARE(info.preparedBy, KisPageTransactionId{99}); QVERIFY(candidates.empty());
    KisPageStateSnapshot actual;
    QVERIFY(coordinator.pageSnapshot(page.key, &actual)); comparePageRecords(actual, page);

    KisPageMetadataReadCleanup cleanup;
    claim.kind = KisPageTransitionKind::ReleaseCapturedVersion;
    for (int i = 0; i < count; ++i) {
        claim.readView = {quint64(i + 1)};
        QVERIFY(coordinator.applyOwner(page.key, claim, &cleanup).accepted);
    }
    QVERIFY(!coordinator.applyOwner(page.key, claim, &cleanup).accepted);
    QVERIFY(!cleanup.isEmpty()); QCOMPARE(cleanup.byteSize(), quint64(Arena::blockByteSize()));
    QCOMPARE(live(), limits.metadataArenaBytes); // Detached allocation is still charged.
    QVERIFY(coordinator.versionSnapshot(prepared.version, &info)); QVERIFY(!info.captured);
    QVERIFY(!coordinator.cpuReadReplica(prepared.version).isValid());
    const auto after = coordinator.metrics();
    QCOMPARE(after.localVersionInputs, metrics.localVersionInputs);
    QCOMPARE(after.localVersionInstalls, metrics.localVersionInstalls);
    cleanup.clear(); QCOMPARE(live(), limits.metadataArenaBytes - Arena::blockByteSize());
    QVERIFY(coordinator.versionSnapshot(prepared.version, &info, &candidates));
    QCOMPARE(candidates.size(), size_t(1)); QCOMPARE(candidates.front().replica, prepared.authority);
    auto missing = prepared.version; ++missing.generation.value;
    QVERIFY(coordinator.versionSnapshot(missing, &info, &candidates));
    QVERIFY(!info.version.isValid() && candidates.empty());
}

void KisPageStoreReferenceTest::indexedMutationBaseLookup()
{
    QFETCH(int, history);
    auto page = pageWithHistory(history);
    const auto base = page.versions.first().version;
    KisCompletionRegistry completions;
    const auto lastUse = completions.allocatePending(completions.registerSource(KisCompletionDomain::CpuJob));
    auto beforeReplica = replica(base, 1, 1, 10000);
    auto wrongDomain = beforeReplica; wrongDomain.allocation.slot = 10001;
    wrongDomain.domain = KisPageAccessDomain::DiscreteVram;
    auto wrongLayout = beforeReplica; wrongLayout.allocation.slot = 10002;
    ++wrongLayout.layout.layoutRevision;
    auto notReady = beforeReplica; notReady.allocation.slot = 10003;
    auto &baseState = page.versions.first();
    baseState.replicas.append({wrongDomain, KisReplicaValidity::Valid, {}, {}, 0, {}});
    baseState.replicas.append({wrongLayout, KisReplicaValidity::Valid, {}, {}, 0, {}});
    baseState.replicas.append({notReady, KisReplicaValidity::Allocated, {}, {}, 0, {}});
    baseState.replicas.append({beforeReplica, KisReplicaValidity::Valid, {}, {{29}}, 2, {lastUse}});
    baseState.capturedReadViews.append({20});
    const auto sealed = pageVersion(0, page.nextGeneration.value++);
    const auto authority = replica(sealed, 1, 1, sealed.generation.value);
    page.versions.append({sealed,
                          KisPagePublicationState::Prepared,
                          {{authority, KisReplicaValidity::Valid, {}, {}, 0, {}}},
                          authority,
                          {71},
                          {}});
    KisPageBackingLimits limits; limits.metadataArenaBytes = 16 * 1024 * 1024;
    KisBackingBudgetController budget(limits);
    KisPageMetadataCoordinator coordinator;
    coordinator.attachBackingBudget(budget);
    QVERIFY(coordinator.configure(1));
    QVERIFY(coordinator.registerPage(page));
    const auto before = coordinator.metrics();
    KisPageMetadataCoordinator::MutationBaseInfo info;
    QVERIFY(coordinator.queryMutationBase(base, sealed, true, &info));
    QVERIFY(info.baseExists && !info.hasWriter);
    QCOMPARE(info.selected.version, sealed);
    QCOMPARE(info.selected.authority, authority);
    QVERIFY(!info.recoverableBefore.isValid());
    QCOMPARE(info.nextGeneration, page.nextGeneration);
    KisPageMetadataCoordinator::VersionInfo exact;
    QVERIFY(coordinator.versionSnapshot(page.versions[history].version, &exact));
    QCOMPARE(exact.version, page.versions[history].version);
    QVERIFY(coordinator.queryMutationBase(base, base, true, &info));
    QCOMPARE(info.selected.version, base);
    QCOMPARE(info.selected.authority, page.versions.first().authority);
    QVERIFY(info.selected.captured);
    QCOMPARE(info.recoverableBefore, beforeReplica);
    QCOMPARE(coordinator.metrics().mutationBaseVersionInputs - before.mutationBaseVersionInputs, quint64(3));
    auto foreign = sealed;
    ++foreign.key.surface.value;
    QVERIFY(!coordinator.queryMutationBase(base, foreign, true, &info));
    QCOMPARE(info.selected.version, base); // Refusal leaves caller output intact.

    KisPageTransition write;
    write.kind = KisPageTransitionKind::AcquireWrite;
    write.baseVersion = base; write.version = {base.key, page.nextGeneration};
    write.source = page.versions.first().authority; write.target = replica(write.version, 1, 1, 30000);
    write.transaction = {73}; write.operation = {74}; write.writer = {75};
    const auto expected = KisPageStateMachine().apply(page, write); QVERIFY(expected.accepted);
    QVERIFY(coordinator.applyOwner(page.key, write).accepted);
    const KisPageVersion defaultVersion{pageKey(1), {1}, 7};
    KisPageStateSnapshot defaultPage;
    defaultPage.key = defaultVersion.key; defaultPage.publishedEpoch = {1};
    defaultPage.publishedGeneration = {1}; defaultPage.publishedDefaultPixelRevision = 7;
    defaultPage.nextGeneration = {2};
    defaultPage.versions.append({defaultVersion, KisPagePublicationState::Published, {}, {}, {}, {}});
    QVERIFY(coordinator.registerPage(defaultPage));
    const auto live = [&] {
        return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - live());
    void *filler = kisAllocateMutationStorage(&budget, fillerBytes, 1);
    const auto freeFiller = qScopeGuard([&] { kisFreeMutationStorage(&budget, filler, fillerBytes, 1); });
    QCOMPARE(live(), limits.metadataArenaBytes);
    const auto pressure = budget.usage().backpressureCount;
    for (int attempt = 0; attempt < 2; ++attempt) {
        QVERIFY(coordinator.queryMutationBase(base, {}, true, &info));
        QVERIFY(info.baseExists && info.hasWriter);
        QCOMPARE(info.nextGeneration, expected.next.nextGeneration);
        QCOMPARE(info.selected.version, base);
        QCOMPARE(info.recoverableBefore, beforeReplica);
        QVERIFY(coordinator.queryMutationBase(base, {}, false, &info));
        QVERIFY(!info.recoverableBefore.isValid());
        auto missing = base; missing.generation = {50000};
        QVERIFY(coordinator.queryMutationBase(missing, sealed, true, &info));
        QVERIFY(!info.baseExists); QCOMPARE(info.selected.version, sealed);
        QVERIFY(coordinator.queryMutationBase(base, missing, true, &info));
        QVERIFY(info.baseExists && !info.selected.version.isValid());
        QVERIFY(coordinator.queryMutationBase(defaultVersion, {}, true, &info));
        QVERIFY(info.baseExists && info.selected.isVirtualDefault());
        QVERIFY(!info.hasWriter && !info.recoverableBefore.isValid());
    }
    QCOMPARE(live(), limits.metadataArenaBytes);
    QCOMPARE(budget.usage().backpressureCount, pressure);
    KisPageStateSnapshot actual;
    QVERIFY(coordinator.pageSnapshot(page.key, &actual)); comparePageRecords(actual, expected.next);
}

void KisPageStoreReferenceTest::indexedMutationConcurrentProtection()
{
    auto page = pageWithHistory(257);
    const auto version = pageVersion(0, page.nextGeneration.value++);
    const auto authority = replica(version, 1, 1, version.generation.value);
    page.versions.append({version,
                          KisPagePublicationState::Prepared,
                          {{authority, KisReplicaValidity::Valid, {}, {}, 0, {}}},
                          authority,
                          {71},
                          {}});
    KisPageMetadataCoordinator coordinator;
    QVERIFY(coordinator.configure(4));
    QVERIFY(coordinator.registerPage(page));
    const auto retainedDiagnostic = page;
    std::atomic<int> failures{0};
    std::vector<std::thread> readers;
    for (int worker = 0; worker < 4; ++worker)
        readers.emplace_back([&, worker] {
            for (int i = 0; i < 200; ++i) {
                KisPageTransition retain;
                retain.kind = KisPageTransitionKind::RetainCapturedVersion;
                retain.version = version;
                retain.transaction = {71};
                retain.readView = {quint64(1 + worker * 200 + i)};
                if (!coordinator.applyOwner(version.key, retain).accepted)
                    ++failures;
                retain.kind = KisPageTransitionKind::ReleaseCapturedVersion;
                if (!coordinator.applyOwner(version.key, retain).accepted)
                    ++failures;
            }
        });
    for (auto &reader : readers)
        reader.join();
    QCOMPARE(failures.load(), 0);
    KisPageStateSnapshot actual;
    QVERIFY(coordinator.pageSnapshot(page.key, &actual));
    comparePageRecords(actual, retainedDiagnostic);
    QCOMPARE(coordinator.metrics().localTransitionSequences, quint64(1600));
    QCOMPARE(coordinator.metrics().localVersionInputs, quint64(0));
    QCOMPARE(coordinator.metrics().localVersionInstalls, quint64(0));
}

void KisPageStoreReferenceTest::indexedRecordSlotReuse()
{
    auto expected = pageWithHistory(127);
    KisPageMetadataCoordinator coordinator;
    MetadataEffectReceiver receiver;
    attachEffects(coordinator, receiver);
    QVERIFY(coordinator.configure(1));
    QVERIFY(coordinator.registerPage(expected));
    auto run = [&](const KisPageTransition &transition) {
        receiver.effects.clear();
        const auto oracle = KisPageStateMachine().apply(expected, transition);
        QVERIFY2(oracle.accepted, qPrintable(oracle.rejectionReason));
        const auto actual = coordinator.applyOwner(expected.key, transition);
        QVERIFY2(actual.accepted, qPrintable(actual.rejectionReason));
        QCOMPARE(receiver.effects.size(), oracle.effects.size());
        for (qsizetype i = 0; i < oracle.effects.size(); ++i)
            QCOMPARE(receiver.effects[i].replica, oracle.effects[i].replica);
        expected = oracle.next;
        KisPageStateSnapshot snapshot;
        QVERIFY(coordinator.pageSnapshot(expected.key, &snapshot));
        comparePageRecords(snapshot, expected);
    };
    KisPageTransition write;
    write.kind = KisPageTransitionKind::AcquireWrite;
    write.baseVersion = expected.versions.first().version;
    write.source = expected.versions.first().authority;
    write.transaction = {71};
    write.writer = {72};
    write.operation = {73};
    for (int i = 0; i < 4; ++i) {
        write.version = pageVersion(0, expected.nextGeneration.value);
        // The same physical slot may be reused ONLY after its old exact record
        // has gone. A stale slot witness must not reject a new allocation token.
        write.target = replica(write.version, 1, 1, 1000, quint64(i + 1));
        write.kind = KisPageTransitionKind::AcquireWrite;
        run(write);
        if (i & 1) {
            for (auto kind : {KisPageTransitionKind::PrepareWrite,
                              KisPageTransitionKind::BeginPublish,
                              KisPageTransitionKind::PublishWrite}) {
                write.kind = kind;
                run(write);
            }
            write.kind = KisPageTransitionKind::AbortPreparedVersion;
            run(write);
        } else {
            write.kind = KisPageTransitionKind::CancelWrite;
            run(write);
        }
        KisPageMetadataCoordinator::VersionInfo missing;
        QVERIFY(coordinator.versionSnapshot(write.version, &missing));
        QVERIFY(!missing.version.isValid());
    }
}

void KisPageStoreReferenceTest::indexedCompetingPreparedTransactions()
{
    auto expected = pageWithHistory(17);
    KisPageMetadataCoordinator coordinator;
    QVERIFY(coordinator.configure(1));
    QVERIFY(coordinator.registerPage(expected));
    const auto base = expected.versions.first();
    QVector<KisPageTransition> writes;
    // Existing production optimistic-commit tests allow independently sealed
    // versions of one page. Only the active writer is unique; preparedBy is
    // per VERSION, and the image owner resolves the eventual root conflict.
    for (quint64 tx : {71u, 81u}) {
        KisPageTransition write;
        write.baseVersion = base.version;
        write.source = base.authority;
        write.version = pageVersion(0, expected.nextGeneration.value);
        write.target = replica(write.version, 1, 1, write.version.generation.value);
        write.transaction = {tx};
        write.writer = {tx + 1};
        write.operation = {tx + 2};
        for (auto kind : {KisPageTransitionKind::AcquireWrite,
                          KisPageTransitionKind::PrepareWrite,
                          KisPageTransitionKind::BeginPublish,
                          KisPageTransitionKind::PublishWrite}) {
            write.kind = kind;
            const auto oracle = KisPageStateMachine().apply(expected, write);
            QVERIFY2(oracle.accepted, qPrintable(oracle.rejectionReason));
            QVERIFY(coordinator.applyOwner(expected.key, write).accepted);
            expected = oracle.next;
            KisPageStateSnapshot actual;
            QVERIFY(coordinator.pageSnapshot(expected.key, &actual));
            comparePageRecords(actual, expected);
        }
        writes.append(write);
    }
    auto read = writes.first();
    read.kind = KisPageTransitionKind::AcquireRead;
    read.lease = {101};
    read.transaction = writes.last().transaction;
    QVERIFY(!KisPageStateMachine().apply(expected, read).accepted);
    QVERIFY(!coordinator.applyOwner(expected.key, read).accepted);
    auto retain = writes.first();
    retain.kind = KisPageTransitionKind::RetainCapturedVersion;
    retain.readView = {111};
    auto step = KisPageStateMachine().apply(expected, retain);
    QVERIFY(step.accepted);
    QVERIFY(coordinator.applyOwner(expected.key, retain).accepted);
    expected = step.next;
    auto commit = writes.last();
    commit.kind = KisPageTransitionKind::CommitTransaction;
    commit.imageEpoch = {2};
    step = KisPageStateMachine().apply(expected, commit);
    QVERIFY(step.accepted);
    const KisPageTransaction committedTx{commit.transaction, {1}};
    auto publication = coordinator.preparePublication(committedTx, commit.imageEpoch, {commit});
    QVERIFY(publication.isValid());
    QVERIFY(coordinator.installPublication(std::move(publication), committedTx, commit.imageEpoch));
    expected = step.next;
    commit = writes.first();
    commit.kind = KisPageTransitionKind::CommitTransaction;
    commit.imageEpoch = {3};
    QVERIFY(!KisPageStateMachine().apply(expected, commit).accepted);
    const KisPageTransaction staleTx{commit.transaction, {1}};
    QVERIFY(!coordinator.preparePublication(staleTx, commit.imageEpoch, {commit}).isValid());
    auto abort = writes.first();
    abort.kind = KisPageTransitionKind::AbortPreparedVersion;
    step = KisPageStateMachine().apply(expected, abort);
    QVERIFY(step.accepted);
    QVERIFY(coordinator.applyOwner(expected.key, abort).accepted);
    expected = step.next;
    retain.kind = KisPageTransitionKind::ReleaseCapturedVersion;
    step = KisPageStateMachine().apply(expected, retain);
    QVERIFY(step.accepted);
    QVERIFY(coordinator.applyOwner(expected.key, retain).accepted);
    expected = step.next;
    KisPageStateSnapshot actual;
    QVERIFY(coordinator.pageSnapshot(expected.key, &actual));
    comparePageRecords(actual, expected);
}

void KisPageStoreReferenceTest::privatePreparedBackingReplacement_data()
{
    QTest::addColumn<int>("variant");
    const char *names[] = {"idle",
                           "foreign-tx",
                           "published",
                           "wrong-source",
                           "wrong-version",
                           "same-allocation",
                           "older-slot",
                           "captured-view",
                           "read-lease",
                           "pin",
                           "last-use",
                           "replica",
                           "writer"};
    for (int i = 0; i < 13; ++i)
        QTest::newRow(names[i]) << i;
}

void KisPageStoreReferenceTest::privatePreparedBackingReplacement()
{
    QFETCH(int, variant);
    KisPageStateMachine machine;
    auto page = initialPageState(pageVersion(0, 1), replica(pageVersion(0, 1), 1, 1, 1));
    const auto version = pageVersion(0, 2);
    const auto authority = replica(version, 1, 1, 2);
    page.versions.append({version,
                          KisPagePublicationState::Prepared,
                          {{authority, KisReplicaValidity::Valid, {}, {}, 0, {}}},
                          authority,
                          {74},
                          {}});
    page.nextGeneration = {3};
    KisPageTransition change;
    change.kind = KisPageTransitionKind::ReplacePrivatePreparedBacking;
    change.transaction = {74};
    change.version = version;
    change.source = authority;
    change.target = replica(version, 1, 1, 3);
    KisCompletionRegistry completions;
    const KisCompletionDomain completionSource = KisCompletionDomain::HostLogical;
    const auto ticket = completions.allocatePending(completions.registerSource(completionSource));
    switch (variant) {
    case 1:
        change.transaction = {75};
        break;
    case 2:
        change.version = pageVersion(0, 1);
        change.source = page.versions.first().authority;
        change.target.version = change.version;
        break;
    case 3:
        change.source.allocation.slot = 19;
        break;
    case 4:
        change.target.version = pageVersion(0, 3);
        break;
    case 5:
        change.target = authority;
        break;
    case 6:
        change.target.allocation = page.versions.first().authority.allocation;
        break;
    case 7:
        page.versions.last().capturedReadViews = {{19}};
        break;
    case 8:
        page.versions.last().replicas.first().readLeases = {{19}};
        break;
    case 9:
        page.versions.last().replicas.first().pinCount = 1;
        break;
    case 10:
        page.versions.last().replicas.first().pendingLastUses = {ticket};
        break;
    case 11:
        page.versions.last().replicas.append({replica(version, 2, 1, 9), KisReplicaValidity::Valid, {}, {}, 0, {}});
        break;
    case 12: {
        KisPageTransition write;
        write.kind = KisPageTransitionKind::AcquireWrite;
        write.baseVersion = version;
        write.version = pageVersion(0, 3);
        write.source = authority;
        write.target = replica(write.version, 1, 1, 4);
        write.transaction = {74};
        write.operation = {19};
        write.writer = {19};
        const auto result = machine.apply(page, write);
        QVERIFY2(result.accepted, qPrintable(result.rejectionReason));
        page = result.next;
        break;
    }
    }
    QString error;
    QVERIFY2(machine.validateInvariants(page, &error), qPrintable(error));
    const auto result = machine.apply(page, change);
    QCOMPARE(result.accepted, variant == 0);
    QVERIFY2(machine.validateInvariants(result.next, &error), qPrintable(error));
    QCOMPARE(result.next.publishedGeneration, page.publishedGeneration);
    QCOMPARE(result.next.nextGeneration, page.nextGeneration);
    QCOMPARE(result.next.versions.size(), page.versions.size());
    if (variant == 0) {
        QCOMPARE(result.effects.size(), 1);
        QCOMPARE(result.effects.first().replica, authority);
        QCOMPARE(result.next.versions.last().authority, change.target);
        QCOMPARE(result.next.versions.last().preparedBy, change.transaction);
    } else {
        QVERIFY(result.effects.isEmpty());
        QCOMPARE(result.next.versions.at(1).authority, authority);
        QCOMPARE(result.next.versions.at(1).capturedReadViews, page.versions.at(1).capturedReadViews);
        QCOMPARE(result.next.versions.at(1).replicas.first().readLeases,
                 page.versions.at(1).replicas.first().readLeases);
        QCOMPARE(result.next.writer.token, page.writer.token);
    }
}

void KisPageStoreReferenceTest::indexedPublicationTransitions()
{
    QFETCH(int, history);
    QFETCH(int, variant);
    QFETCH(bool, compact);
    const KisPageTransaction tx{{74}, {1}};
    KisPageStateMachine machine;
    auto page = pageWithHistory(history);
    const auto appendPrepared = [&](KisPageTransactionId owner) {
        const KisPageVersion identity{page.key, page.nextGeneration, 0};
        ++page.nextGeneration.value;
        const auto authority = replica(identity, 1, 1, identity.generation.value);
        page.versions.append({identity,
                              KisPagePublicationState::Prepared,
                              {{authority, KisReplicaValidity::Valid, {}, {}, 0, {}}},
                              authority,
                              owner,
                              {}});
        return identity;
    };
    KisPageTransition transition;
    transition.imageEpoch = {2};
    if (variant < 5 || variant == 16 || variant == 17) {
        transition.kind = KisPageTransitionKind::CommitTransaction;
        transition.transaction = tx.id;
        transition.version = appendPrepared(tx.id);
        if (variant == 1)
            transition.version = appendPrepared(tx.id);
        if (variant == 2)
            appendPrepared(tx.id); // cannot select an older own Prepared
        if (variant == 3)
            appendPrepared({75}); // independent Prepared must survive
        if (variant == 4) {
            page.versions.last().capturedReadViews.append({11});
            page.versions.last().replicas.first().readLeases.append({12});
            page.versions.last().replicas.first().pinCount = 2;
        }
        if (variant == 16)
            transition.version.generation = page.nextGeneration;
    } else if (variant < 10) {
        transition.kind = KisPageTransitionKind::RestoreCommittedVersion;
        transition.version = variant == 6 ? KisPageVersion{page.key, {1}, 1} : page.versions.last().version;
        if (variant == 7 || variant == 8)
            appendPrepared(variant == 7 ? tx.id : KisPageTransactionId{75});
    } else {
        transition.kind = KisPageTransitionKind::ReplaceDefaultPixel;
        auto &head = page.versions.first();
        head.version.defaultPixelRevision = 1;
        head.authority.version = head.version;
        head.replicas.first().replica.version = head.version;
        page.publishedDefaultPixelRevision = 1;
        transition.version = {page.key, head.version.generation, 2};
        if (variant == 11) {
            const auto authority = replica(transition.version, 1, 1, 10000);
            page.versions.append({transition.version,
                                  KisPagePublicationState::Historical,
                                  {{authority, KisReplicaValidity::Valid, {}, {}, 0, {}}},
                                  authority,
                                  {},
                                  {}});
            transition.target = authority;
        }
        if (variant == 12 || variant == 15) {
            transition.target = replica(transition.version,
                                        1,
                                        1,
                                        variant == 12 ? page.versions.last().authority.allocation.slot : 10000);
            // A different allocation generation does not make a live physical
            // slot free. The collision may be outside current/target records.
            transition.target.allocation.generation = 9;
        }
        if (variant == 13)
            appendPrepared({75});
    }
    if (variant == 9 || variant == 14 || variant == 17) {
        KisPageTransition write;
        write.kind = KisPageTransitionKind::AcquireWrite;
        write.baseVersion = page.versions.first().version;
        write.source = page.versions.first().authority;
        write.version = {page.key, page.nextGeneration, 0};
        write.target = replica(write.version, 1, 1, 20000);
        write.transaction = {76};
        write.operation = {19};
        write.writer = {19};
        const auto result = machine.apply(page, write);
        QVERIFY2(result.accepted, qPrintable(result.rejectionReason));
        page = result.next;
    }
    QString error;
    QVERIFY2(machine.validateInvariants(page, &error), qPrintable(error));
    const auto expected = machine.apply(page, transition);
    quint64 historyNodes = 0;
    quint64 additionRecords = 0;
    if (expected.accepted) {
        QHash<KisPageVersion, KisPagePublicationState> previous;
        for (const auto &version : std::as_const(page.versions))
            previous.insert(version.version, version.publication);
        for (const auto &version : std::as_const(expected.next.versions)) {
            const auto old = previous.constFind(version.version);
            if (old == previous.cend())
                ++additionRecords;
            else if (old.value() != KisPagePublicationState::Historical
                     && version.publication == KisPagePublicationState::Historical)
                ++historyNodes;
        }
    }
    KisPageMetadataCoordinator coordinator;
    QVERIFY(coordinator.configure(4));
    QVERIFY(coordinator.registerPage(page));
    const bool restore = compact && transition.kind == KisPageTransitionKind::RestoreCommittedVersion;
    const KisPageMetadataCoordinator::PublicationChange change{transition.kind, transition.version, transition.target};
    auto candidate = restore ? coordinator.prepareRestoration({2}, &transition.version, 1, &error)
        : compact ? coordinator.preparePublication(tx, {2}, &change, 1, &error)
                  : coordinator.preparePublication(tx, {2}, {transition}, &error);
    QCOMPARE(candidate.isValid(), expected.accepted);
    const auto metrics = coordinator.metrics();
    QVERIFY(metrics.publicationVersionInputs <= 4);
    QCOMPARE(metrics.fullSnapshotExports, quint64(0));
    QCOMPARE(metrics.publicationHistoryNodesPrepared, historyNodes);
    QCOMPARE(metrics.publicationHistoryNodesTransferred, quint64(0));
    QCOMPARE(metrics.publicationAdditionRecordsPrepared, additionRecords);
    QCOMPARE(metrics.publicationAdditionRecordsTransferred, quint64(0));
    if (expected.accepted) {
        QVERIFY(expected.effects.isEmpty()); // Publication leaves replicas for history GC.
        QVERIFY2(coordinator.installPublication(std::move(candidate), restore ? KisPageTransaction{} : tx, {2}, &error),
                 qPrintable(error));
        QVERIFY(coordinator.metrics().publicationVersionInstalls <= 3);
    }
    KisPageStateSnapshot actual;
    QVERIFY(coordinator.pageSnapshot(page.key, &actual));
    QCOMPARE(coordinator.metrics().publicationHistoryNodesTransferred, historyNodes);
    QCOMPARE(coordinator.metrics().publicationAdditionRecordsTransferred, additionRecords);
    QVERIFY2(machine.validateInvariants(actual, &error), qPrintable(error));
    comparePageRecords(actual, expected.accepted ? expected.next : page);
    // Directory and target lookup must not export the retained version list.
    const auto beforeLookup = coordinator.metrics();
    const auto heads = coordinator.publicationHeads();
    QCOMPARE(heads.size(), size_t(1));
    const KisPageVersion expectedHead{actual.key, actual.publishedGeneration, actual.publishedDefaultPixelRevision};
    QVERIFY(heads.front() == expectedHead);
    KisPageMetadataCoordinator::PublicationInfo lookup;
    QVERIFY(coordinator.queryPublication(page.key, transition.version, &lookup));
    QVERIFY(lookup.current.version == heads.front());
    const auto target = actual.findVersion(transition.version);
    QCOMPARE(lookup.target.version.isValid(), target != nullptr);
    if (target) {
        QVERIFY(lookup.target.version == target->version);
        QCOMPARE(lookup.target.publication, target->publication);
        QVERIFY(lookup.target.authority == target->authority);
    }
    QCOMPARE(coordinator.metrics().fullSnapshotExports, beforeLookup.fullSnapshotExports);
    QCOMPARE(coordinator.metrics().publicationDirectoryHeaders, quint64(1));
}

void KisPageStoreReferenceTest::publicationDirectoryStorageLifetime()
{
    KisPageBackingLimits limits; limits.metadataArenaBytes = 256 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    std::array<KisBackingBudgetReservation, 8> warm;
    for (auto &slot : warm) { slot = parent->reserve({}, nullptr); QVERIFY(slot.isValid()); }
    for (auto &slot : warm) slot.release();
    const auto live = [&] { return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const auto baseline = live();
    auto budget = std::make_unique<KisBackingBudgetController>(limits);
    QVERIFY(budget->configureSharedNonPayloadBudget(parent));
    auto metadata = std::make_unique<KisPageMetadataCoordinator>();
    metadata->attachBackingBudget(*budget); QVERIFY(metadata->configure(1));
    const auto page = pageWithHistory(31); QVERIFY(metadata->registerPage(page));
    const auto before = live();
    auto heads = metadata->publicationHeads();
    QCOMPARE(heads.size(), size_t(1));
    const auto storage = live() - before; QVERIFY(storage > 0);
    const auto fillerBytes = size_t(limits.metadataArenaBytes - live());
    void *filler = kisAllocateMutationStorage(parent.get(), fillerBytes, 1);
    {
        const auto freeFiller = qScopeGuard([&] { kisFreeMutationStorage(parent.get(), filler, fillerBytes, 1); });
        QCOMPARE(live(), limits.metadataArenaBytes);
        KisPageMetadataCoordinator::PublicationInfo info;
        QVERIFY(metadata->queryPublication(page.key, page.versions.last().version, &info));
        QVERIFY(info.current.version == heads.front());
        QVERIFY(info.target.version == page.versions.last().version);
        bool refused = false;
        try { auto other = metadata->publicationHeads(); }
        catch (const std::bad_alloc &) { refused = true; }
        QVERIFY(refused); // Real paid directory allocation; scalar observation remains available.
        QCOMPARE(metadata->pageRegistrationCount(), quint64(1));
        QCOMPARE(live(), limits.metadataArenaBytes);
    }
    metadata.reset(); budget.reset();
    QVERIFY(live() >= baseline + storage); // Accounting owner and parent-child registration also remain paid.
    heads = decltype(heads){}; // Last array frees its physical capacity and retained accounting owner.
    QCOMPARE(live(), baseline);
}

void KisPageStoreReferenceTest::indexedPublicationIgnoresRetainedHistory()
{
    QFETCH(int, history);
    const KisPageTransaction tx{{74}, {1}};
    auto page = pageWithHistory(history);
    const KisPageVersion target{page.key, page.nextGeneration, 0};
    ++page.nextGeneration.value;
    const auto authority = replica(target, 1, 1, target.generation.value);
    page.versions.append({target,
                          KisPagePublicationState::Prepared,
                          {{authority, KisReplicaValidity::Valid, {}, {}, 0, {}}},
                          authority,
                          tx.id,
                          {}});
    KisPageMetadataCoordinator coordinator;
    QVERIFY(coordinator.configure(4));
    QVERIFY(coordinator.registerPage(page));
    KisPageTransition transition;
    transition.kind = KisPageTransitionKind::CommitTransaction;
    transition.version = target;
    transition.transaction = tx.id;
    transition.imageEpoch = {2};
    auto candidate = coordinator.preparePublication(tx, {2}, {transition});
    QVERIFY(candidate.isValid());
    QCOMPARE(coordinator.metrics().publicationVersionInputs, quint64(2));
    QCOMPARE(coordinator.metrics().fullSnapshotExports, quint64(0));
    const auto expected = KisPageStateMachine().apply(page, transition);
    QVERIFY(expected.accepted);
    const auto ready = coordinator.metrics();
    QCOMPARE(ready.publicationHistoryNodesPrepared, quint64(1));
    QCOMPARE(ready.publicationHistoryNodesTransferred, quint64(0));
    QCOMPARE(ready.publicationAdditionRecordsPrepared, quint64(0));
    QCOMPARE(ready.publicationAdditionRecordsTransferred, quint64(0));
    QVERIFY(expected.effects.isEmpty());
    QVERIFY(coordinator.installPublication(std::move(candidate), tx, {2}));
    QCOMPARE(coordinator.metrics().publicationHistoryNodesTransferred, ready.publicationHistoryNodesPrepared);
    QCOMPARE(coordinator.metrics().fullSnapshotExports, quint64(0));
    KisPageStateSnapshot actual;
    QVERIFY(coordinator.pageSnapshot(page.key, &actual));
    comparePageRecords(actual, expected.next);
}

void KisPageStoreReferenceTest::preparedPublicationStorageIsBound()
{
    QFETCH(int, kind);
    QFETCH(int, outcome);
    const bool mutation = kind == 2, synthesizedDefault = kind == 3;
    const KisPageTransaction tx{{74}, {1}};
    auto coordinator = std::make_unique<KisPageMetadataCoordinator>();
    QVERIFY(coordinator->configure(4));
    KisPageMetadataCoordinator foreign;
    QVERIFY(foreign.configure(4));
    QVector<KisPageStateSnapshot> expected;
    QVector<KisPageTransition> transitions;
    for (int x = 0; x < 2; ++x) {
        const KisPageVersion initial{pageKey(x), {1}, synthesizedDefault ? 1u : 0u};
        auto state = initialPageState(initial, replica(initial, 1, 1, 1));
        if (synthesizedDefault)
            state.publishedDefaultPixelRevision = 1;
        if (!synthesizedDefault) {
            const auto authority = replica(pageVersion(x, 2), 1, 1, 2);
            state.versions.append({pageVersion(x, 2),
                                   KisPagePublicationState::Prepared,
                                   {{authority, KisReplicaValidity::Valid, {}, {}, 0, {}}},
                                   authority,
                                   tx.id,
                                   {}});
            state.nextGeneration = {3};
        }
        QVERIFY(coordinator->registerPage(state));
        expected.append(state);
        KisPageTransition transition;
        transition.kind = synthesizedDefault ? KisPageTransitionKind::ReplaceDefaultPixel
            : mutation                       ? KisPageTransitionKind::DetachPreparedVersion
                                             : KisPageTransitionKind::CommitTransaction;
        transition.version = synthesizedDefault ? KisPageVersion{pageKey(x), {1}, 2} : pageVersion(x, 2);
        if (!synthesizedDefault)
            transition.transaction = tx.id;
        if (!mutation)
            transition.imageEpoch = {2};
        transitions.append(transition);
    }
    QVector<KisPageVersion> detachments;
    for (const auto &transition : transitions) detachments.append(transition.version);
    auto candidate = mutation ? coordinator->prepareMutation(tx, detachments.constData(), detachments.size())
                              : coordinator->preparePublication(tx, {2}, transitions);
    QVERIFY(candidate.isValid());
    const auto reservedFootprint = coordinator->footprint();
    QCOMPARE(reservedFootprint.pageActivities, quint64(2));
    if (!mutation && synthesizedDefault) {
        QVERIFY(reservedFootprint.versionArena.outstandingReservations > 0);
        QVERIFY(reservedFootprint.exactVersionIndex.outstandingReservations > 0);
    }
    const auto prepared = coordinator->metrics();
    QCOMPARE(prepared.publicationHistoryNodesPrepared, quint64(2));
    QCOMPARE(prepared.publicationHistoryNodesTransferred, quint64(0));
    QCOMPARE(prepared.publicationAdditionRecordsPrepared, quint64(synthesizedDefault ? 2 : 0));
    QCOMPARE(prepared.publicationAdditionRecordsTransferred, quint64(0));
    for (int x = 0; x < 2; ++x) {
        KisPageStateSnapshot actual;
        QVERIFY(coordinator->pageSnapshot(pageKey(x), &actual));
        comparePageRecords(actual, expected.at(x));
    }
    if (outcome == 1) {
        KisPageTransition change;
        if (mutation) {
            change.kind = KisPageTransitionKind::AcquireWrite;
            change.baseVersion = pageVersion(1, 2);
            change.source = replica(change.baseVersion, 1, 1, 2);
            change.version = pageVersion(1, 3);
            change.target = replica(change.version, 1, 1, 3);
            change.transaction = tx.id;
            change.writer = {51};
            change.operation = {52};
        } else {
            change.kind = KisPageTransitionKind::AcquireRead;
            change.version = expected.at(1).versions.first().version;
            change.target = expected.at(1).versions.first().authority;
            change.lease = {19};
        }
        const auto changed = KisPageStateMachine().apply(expected.at(1), change);
        QVERIFY(changed.accepted);
        QVERIFY(coordinator->applyOwner(pageKey(1), change).accepted);
        expected[1] = changed.next;
    }
    if (outcome == 4) {
        auto moved = std::move(candidate);
        QVERIFY(!candidate.isValid());
        candidate = std::move(moved);
        QVERIFY(candidate.isValid());
        QVERIFY(!moved.isValid());
    }
    const auto install = [&](KisPageMetadataCoordinator &owner) {
        return mutation ? owner.installMutation(std::move(candidate), tx)
                        : owner.installPublication(std::move(candidate), tx, {2});
    };
    if (outcome == 5) {
        coordinator.reset(); // Candidate storage must not borrow the dead shards.
        QVERIFY(!install(foreign));
        QVERIFY(!candidate.isValid());
        QCOMPARE(foreign.metrics().publicationHistoryNodesTransferred, quint64(0));
        QCOMPARE(foreign.metrics().publicationAdditionRecordsTransferred, quint64(0));
        return;
    }
    const bool success = outcome == 0 || outcome == 4;
    if (outcome == 2)
        candidate = {};
    else
        QCOMPARE(install(outcome == 3 ? foreign : *coordinator), success);
    QVERIFY(!candidate.isValid());
    QVERIFY(!install(*coordinator)); // All outcomes are one-shot, including cancellation.
    const auto releasedFootprint = coordinator->footprint();
    QCOMPARE(releasedFootprint.versionArena.outstandingReservations, quint64(0));
    QCOMPARE(releasedFootprint.replicaArena.outstandingReservations, quint64(0));
    QCOMPARE(releasedFootprint.overflowArena.outstandingReservations, quint64(0));
    QCOMPARE(releasedFootprint.exactVersionIndex.outstandingReservations, quint64(0));
    QCOMPARE(releasedFootprint.physicalSlotIndex.outstandingReservations, quint64(0));
    const auto installed = coordinator->metrics();
    QCOMPARE(installed.publicationHistoryNodesPrepared, prepared.publicationHistoryNodesPrepared);
    QCOMPARE(installed.publicationHistoryNodesTransferred,
             success ? prepared.publicationHistoryNodesPrepared : quint64(0));
    QCOMPARE(installed.publicationAdditionRecordsPrepared, prepared.publicationAdditionRecordsPrepared);
    QCOMPARE(installed.publicationAdditionRecordsTransferred,
             success ? prepared.publicationAdditionRecordsPrepared : quint64(0));
    for (int x = 0; x < 2; ++x) {
        if (success) {
            const auto next = KisPageStateMachine().apply(expected.at(x), transitions.at(x));
            QVERIFY(next.accepted);
            expected[x] = next.next;
        }
        KisPageStateSnapshot actual;
        QVERIFY(coordinator->pageSnapshot(pageKey(x), &actual));
        comparePageRecords(actual, expected.at(x));
        const auto history = coordinator->historySlice(pageKey(x), {}, 32);
        const qsizetype expectedHistory = std::count_if(
            expected.at(x).versions.cbegin(), expected.at(x).versions.cend(),
            [](const KisPageVersionStateSnapshot &version) {
                return version.publication == KisPagePublicationState::Historical;
            });
        QCOMPARE(history.count, expectedHistory);
    }
}

void KisPageStoreReferenceTest::deferredPublicationCleanupOwnsCandidate()
{
    QFETCH(int, history);
    QFETCH(bool, reject);
    const KisPageTransaction tx{{74}, {1}};
    auto page = pageWithHistory(history);
    const KisPageVersion target{page.key, page.nextGeneration, 0};
    const auto authority = replica(target, 1, 1, 20000);
    page.versions.append({target,
                          KisPagePublicationState::Prepared,
                          {{authority, KisReplicaValidity::Valid, {}, {}, 0, {}}},
                          authority,
                          tx.id,
                          {}});
    ++page.nextGeneration.value;
    KisPageTransition transition;
    transition.kind = KisPageTransitionKind::CommitTransaction;
    transition.version = target;
    transition.transaction = tx.id;
    transition.imageEpoch = {2};
    const auto expected = KisPageStateMachine().apply(page, transition);
    QVERIFY(expected.accepted);
    auto coordinator = std::make_unique<KisPageMetadataCoordinator>();
    QVERIFY(coordinator->configure(4));
    QVERIFY(coordinator->registerPage(page));
    auto candidate = coordinator->preparePublication(tx, {2}, {transition});
    QVERIFY(candidate.isValid());
    if (reject) {
        const KisPageVersion current{page.key, page.publishedGeneration, page.publishedDefaultPixelRevision};
        const KisPageVersionStateSnapshot *published = nullptr;
        for (const auto &version : std::as_const(page.versions)) {
            if (version.version == current) {
                published = &version;
                break;
            }
        }
        QVERIFY(published);
        KisPageTransition read;
        read.kind = KisPageTransitionKind::AcquireRead;
        read.version = current;
        read.target = published->authority;
        read.lease = {19};
        const auto expectedChange = KisPageStateMachine().apply(page, read);
        QVERIFY(expectedChange.accepted);
        QVERIFY(coordinator->applyOwner(page.key, read).accepted);
        page = expectedChange.next;
    }
    KisPageMetadataCoordinator::DeferredPublicationCleanup cleanup;
    const bool installed =
        coordinator->installPublication(std::move(candidate), tx, {2}, nullptr, &cleanup);
    QCOMPARE(installed, !reject);
    QVERIFY(!cleanup.isEmpty());
    QCOMPARE(cleanup.pendingWorkUnits(), qsizetype(1));
    QCOMPARE(cleanup.clearBatch(0), qsizetype(0));
    KisPageStateSnapshot actual;
    QVERIFY(coordinator->pageSnapshot(page.key, &actual));
    comparePageRecords(actual, reject ? page : expected.next);
    QVERIFY(expected.effects.isEmpty());
    coordinator.reset(); // Cleanup owns no dereferenced shard/coordinator state.
    QCOMPARE(cleanup.clearBatch(1), qsizetype(1));
    QVERIFY(cleanup.isEmpty());
}

void KisPageStoreReferenceTest::mutationCandidateStorageAtSharedCapacity()
{
    QFETCH(bool, install);
    KisPageBackingLimits limits; limits.metadataArenaBytes = 4 * 1024 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    KisBackingBudgetController budget(limits);
    QVERIFY(budget.configureSharedNonPayloadBudget(parent));
    KisPageMetadataCoordinator metadata;
    metadata.attachBackingBudget(budget); QVERIFY(metadata.configure(1));
    const KisPageTransaction tx{{74}, {1}};
    QVector<KisPageVersion> versions;
    for (int x = 0; x < 324; ++x) {
        auto page = initialPageState(pageVersion(x, 1), replica(pageVersion(x, 1), 1, 1, quint64(2 * x + 1)));
        const auto target = replica(pageVersion(x, 2), 1, 1, quint64(2 * x + 2));
        page.versions.append({target.version, KisPagePublicationState::Prepared,
            {{target, KisReplicaValidity::Valid, {}, {}, 0, {}}}, target, tx.id, {}});
        page.nextGeneration = {3};
        QVERIFY(metadata.registerPage(page)); versions.append(target.version);
    }
    const auto prepare = [&] { return metadata.prepareMutation(tx, versions.constData(), versions.size()); };
    auto warm = prepare(); QVERIFY(warm.isValid()); warm = {};
    const auto live = [](const KisBackingBudgetController &owner) {
        return owner.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const quint64 before = live(budget);
    constexpr quint64 candidateRoom = 64 * 1024;
    const size_t fillerBytes = size_t(limits.metadataArenaBytes - live(*parent) - candidateRoom);
    void *filler = kisAllocateMutationStorage(parent.get(), fillerBytes, 1);
    const auto release = qScopeGuard([&] { kisFreeMutationStorage(parent.get(), filler, fillerBytes, 1); });
    auto candidate = prepare(); QVERIFY(candidate.isValid());
    QVERIFY(live(budget) > before && live(budget) - before < candidateRoom);
    qInfo() << "BR1_DETACHMENT_CANDIDATE_BYTES" << live(budget) - before;
    QCOMPARE(metadata.footprint().pageActivities, quint64(324));
    const size_t remainingBytes = size_t(limits.metadataArenaBytes - live(*parent));
    void *remaining = kisAllocateMutationStorage(parent.get(), remainingBytes, 1);
    const auto releaseRemaining = qScopeGuard([&] { kisFreeMutationStorage(parent.get(), remaining, remainingBytes, 1); });
    QCOMPARE(live(*parent), limits.metadataArenaBytes);
    KisPageMetadataCoordinator::DeferredPublicationCleanup cleanup;
    if (install) QVERIFY(metadata.installMutation(std::move(candidate), tx, nullptr, &cleanup));
    else candidate = {};
    for (const auto &version : versions) {
        KisPageStateSnapshot page; QVERIFY(metadata.pageSnapshot(version.key, &page));
        const auto *record = page.findVersion(version); QVERIFY(record);
        QCOMPARE(record->publication, install ? KisPagePublicationState::Historical : KisPagePublicationState::Prepared);
        QCOMPARE(record->preparedBy, install ? KisPageTransactionId{} : tx.id);
        QCOMPARE(page.publishedGeneration, KisPageGeneration{1});
        QCOMPARE(page.nextGeneration, KisPageGeneration{3});
    }
    qsizetype cleared = 0;
    while (!cleanup.isEmpty()) cleared += cleanup.clearBatch(17);
    QCOMPARE(cleared, install ? qsizetype(324) : qsizetype(0));
    QCOMPARE(metadata.footprint().pageActivities, quint64(0));
    QCOMPARE(live(budget), before);
    for (const auto &bucket : parent->usage().buckets) QCOMPARE(bucket.reserved.cpuRam, quint64(0));
}

void KisPageStoreReferenceTest::publicationStorageRefusalAndLateCleanup()
{
    QFETCH(int, kind);
    QFETCH(int, outcome);
    KisPageBackingLimits limits; limits.metadataArenaBytes = 1024 * 1024;
    auto parent = std::make_shared<KisBackingBudgetController>(limits);
    std::array<KisBackingBudgetReservation, 8> parentSlots;
    for (auto &slot : parentSlots) { slot = parent->reserve({}, nullptr); QVERIFY(slot.isValid()); }
    for (auto &slot : parentSlots) slot.release();
    const auto parentLive = [&] {
        return parent->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const quint64 parentBaseline = parentLive();
    auto budget = std::make_unique<KisBackingBudgetController>(limits);
    QVERIFY(budget->configureSharedNonPayloadBudget(parent));
    auto metadata = std::make_unique<KisPageMetadataCoordinator>();
    metadata->attachBackingBudget(*budget);
    QVERIFY(metadata->configure(1));
    const KisPageTransaction tx{{74}, {1}};
    auto page = initialPageState(pageVersion(0, 1), replica(pageVersion(0, 1), 1, 1, 1));
    KisPageTransition transition;
    transition.version = pageVersion(0, 2);
    transition.transaction = tx.id;
    transition.imageEpoch = {2};
    KisCompletionRegistry registry;
    const auto lastUse = registry.allocatePending(registry.registerSource(KisCompletionDomain::CpuJob));
    if (kind == 1) {
        auto &base = page.versions.first();
        auto before = base.authority; before.allocation.slot = 10000;
        base.replicas.append({before, KisReplicaValidity::Valid, {}, {{4}}, 2, {lastUse}});
        base.capturedReadViews.append({19});
        transition.kind = KisPageTransitionKind::AcquireRecoverableWrite;
        transition.baseVersion = base.version;
        transition.source = before; transition.target = base.authority;
        transition.target.version = transition.version; ++transition.target.allocation.generation;
        transition.writer = {51}; transition.operation = {52}; transition.imageEpoch = {};
    } else if (kind == 2) {
        transition.kind = KisPageTransitionKind::RestoreCommittedVersion;
        transition.version = {page.key, {1}, 2}; transition.transaction = {};
    } else {
        const auto target = replica(transition.version, 1, 1, 2);
        page.versions.append({transition.version, KisPagePublicationState::Prepared,
            {{target, KisReplicaValidity::Valid, {}, {{5}}, 1, {lastUse}}}, target, tx.id, {{20}}});
        page.nextGeneration = {3};
        transition.kind = kind == 3 ? KisPageTransitionKind::DetachPreparedVersion
                                    : KisPageTransitionKind::CommitTransaction;
        if (kind == 3) transition.imageEpoch = {};
    }
    QVERIFY(metadata->registerPage(page));
    const auto prepare = [&] {
        return kind == 1 ? metadata->prepareRecoverableWrite(tx, transition)
             : kind == 3 ? metadata->prepareMutation(tx, &transition.version, 1)
                         : metadata->preparePublication(tx, {2}, {transition});
    };
    // Warm only the original reusable activity/index capacity, then discard
    // the complete candidate. No candidate payload is exempted from pressure.
    auto warm = prepare(); QVERIFY(warm.isValid()); warm = {};
    const auto childLive = [&] {
        return budget->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    const quint64 before = childLive();
    const auto beforeIndexes = metadata->footprint().owningCapacityBytes;
    size_t fillerBytes = size_t(limits.metadataArenaBytes - parentLive());
    void *filler = kisAllocateMutationStorage(parent.get(), fillerBytes, 1);
    const auto freeFiller = qScopeGuard([&] {
        if (filler) kisFreeMutationStorage(parent.get(), filler, fillerBytes, 1);
    });
    QCOMPARE(parentLive(), limits.metadataArenaBytes);
    auto refused = prepare(); QVERIFY(!refused.isValid()); QVERIFY(!refused.needsReprepare());
    QCOMPARE(childLive(), before);
    QCOMPARE(metadata->footprint().pageActivities, quint64(0));
    KisPageStateSnapshot actual;
    QVERIFY(metadata->pageSnapshot(page.key, &actual)); comparePageRecords(actual, page);
    kisFreeMutationStorage(parent.get(), filler, fillerBytes, 1); filler = nullptr;
    auto candidate = prepare(); QVERIFY(candidate.isValid());
    QVERIFY(childLive() > before);
    QCOMPARE(metadata->footprint().pageActivities, quint64(1));
    auto expected = KisPageStateMachine().apply(page, transition);
    QVERIFY(expected.accepted);
    if (kind == 1) {
        auto ready = transition; ready.kind = KisPageTransitionKind::PrepareWrite;
        expected = KisPageStateMachine().apply(expected.next, ready); QVERIFY(expected.accepted);
    }
    quint64 readerStorage = 0;
    if (outcome == 2) {
        KisPageTransition read;
        read.kind = KisPageTransitionKind::AcquireRead; read.version = page.versions.first().version;
        read.target = page.versions.first().authority; read.lease = {33};
        const quint64 beforeRead = childLive();
        QVERIFY(metadata->applyOwner(page.key, read).accepted);
        QVERIFY(childLive() >= beforeRead);
        // A new reader can retain an overflow arena beyond candidate cleanup.
        readerStorage = childLive() - beforeRead;
        const auto changed = KisPageStateMachine().apply(page, read); QVERIFY(changed.accepted);
        page = changed.next;
        // Detachment accepts reader churn and preserves the newest protection.
        if (kind == 3) expected = KisPageStateMachine().apply(page, transition);
    }
    fillerBytes = size_t(limits.metadataArenaBytes - parentLive());
    filler = kisAllocateMutationStorage(parent.get(), fillerBytes, 1);
    QCOMPARE(parentLive(), limits.metadataArenaBytes);
    KisPageMetadataCoordinator::DeferredPublicationCleanup cleanup;
    const bool success = outcome != 1 && (outcome != 2 || kind == 3);
    if (outcome == 1) candidate = {};
    else {
        const bool installed = kind == 1
            ? metadata->installRecoverableWrite(std::move(candidate), tx, nullptr, &cleanup)
            : kind == 3 ? metadata->installMutation(std::move(candidate), tx, nullptr, &cleanup)
                        : metadata->installPublication(std::move(candidate), tx, {2}, nullptr, &cleanup);
        QCOMPARE(installed, success);
        QVERIFY(!cleanup.isEmpty());
        QVERIFY(childLive() > before); // Transfer has not freed candidate storage.
    }
    QVERIFY(!candidate.isValid());
    QVERIFY(metadata->pageSnapshot(page.key, &actual));
    comparePageRecords(actual, success ? expected.next : page);
    if (success || outcome == 1) {
        QCOMPARE(metadata->footprint().versionArena.outstandingReservations, quint64(0));
        QCOMPARE(metadata->footprint().replicaArena.outstandingReservations, quint64(0));
        QCOMPARE(metadata->footprint().overflowArena.outstandingReservations, quint64(0));
    } // A rejected candidate retains its reservations until deferred cleanup.
    kisFreeMutationStorage(parent.get(), filler, fillerBytes, 1); filler = nullptr;
    if (outcome == 3) {
        metadata.reset(); budget.reset();
        QVERIFY(parentLive() > parentBaseline); // Original arenas and control storage remain physical.
        QCOMPARE(cleanup.clearBatch(1), qsizetype(1));
        QCOMPARE(parentLive(), parentBaseline);
    } else {
        if (!cleanup.isEmpty()) QCOMPARE(cleanup.clearBatch(1), qsizetype(1));
        QCOMPARE(metadata->footprint().versionArena.outstandingReservations, quint64(0));
        QCOMPARE(metadata->footprint().replicaArena.outstandingReservations, quint64(0));
        QCOMPARE(metadata->footprint().overflowArena.outstandingReservations, quint64(0));
        // A successful recoverable write retains its actual activity node.
        // Refused/cancelled candidates must release that same sparse node.
        const auto retainedIndexBytes = metadata->footprint().owningCapacityBytes - beforeIndexes;
        if (!success || kind != 1) QCOMPARE(retainedIndexBytes, quint64(0));
        QCOMPARE(childLive(), before + readerStorage + retainedIndexBytes);
        metadata.reset(); budget.reset();
        QCOMPARE(parentLive(), parentBaseline);
    }
}

void KisPageStoreReferenceTest::preparedPublicationMatchesFullReferenceTransitions()
{
    const KisPageTransaction transaction{KisPageTransactionId{74}, KisImageEpochId{1}};
    const QVector<KisPageTransitionKind> kinds{KisPageTransitionKind::CommitTransaction,
                                               KisPageTransitionKind::ReplaceDefaultPixel,
                                               KisPageTransitionKind::RestoreCommittedVersion,
                                               KisPageTransitionKind::DetachPreparedVersion};
    // Empty/reader/pin/last-use/prepared-source combinations: the optimized
    // owner path must accept/reject exactly like full reference apply().
    for (auto kind : kinds) {
        for (int variant = 0; variant < (kind == KisPageTransitionKind::DetachPreparedVersion ? 16 : 8); ++variant) {
            auto page = initialPageState(pageVersion(0, 1), replica(pageVersion(0, 1), 1, 1, 1));
            if (variant & 1)
                page.versions.first().replicas.first().readLeases.append(KisPageLeaseId{1});
            if (variant & 2)
                page.versions.first().replicas.first().pinCount = 1;
            KisCompletionRegistry completions;
            const KisCompletionDomain source = KisCompletionDomain::HostLogical;
            const auto ticket = completions.allocatePending(completions.registerSource(source));
            if (variant & 4)
                page.versions.first().replicas.first().pendingLastUses.append(ticket);
            KisPageTransition transition;
            transition.kind = kind;
            transition.imageEpoch = KisImageEpochId{2};
            if (kind == KisPageTransitionKind::CommitTransaction
                || kind == KisPageTransitionKind::DetachPreparedVersion) {
                const auto version = pageVersion(0, 2);
                const auto authority = replica(version, 1, 1, 2);
                page.versions.append({version,
                                      KisPagePublicationState::Prepared,
                                      {{authority, KisReplicaValidity::Valid, {}, {}, 0, {}}},
                                      authority,
                                      transaction.id});
                page.nextGeneration = KisPageGeneration{3};
                transition.transaction = transaction.id;
                transition.version = version;
                if (kind == KisPageTransitionKind::DetachPreparedVersion) {
                    transition.imageEpoch = {};
                    auto &target = page.versions.last();
                    if (variant & 1)
                        target.replicas.first().readLeases.append(KisPageLeaseId{2});
                    if (variant & 2)
                        target.replicas.first().pinCount = 1;
                    if (variant & 4)
                        target.replicas.first().pendingLastUses.append(ticket);
                    if (variant & 8)
                        target.capturedReadViews.append(KisImageEpochSnapshotToken{1});
                }
            } else if (kind == KisPageTransitionKind::ReplaceDefaultPixel) {
                page.publishedDefaultPixelRevision = 1;
                page.versions.first().version.defaultPixelRevision = 1;
                auto &version = page.versions.first();
                version.authority.version = version.version;
                version.replicas.first().replica.version = version.version;
                transition.version = {pageKey(0), KisPageGeneration{1}, 2};
            } else {
                transition.version = {pageKey(0), KisPageGeneration{1}, 1};
            }
            const auto expected = KisPageStateMachine().apply(page, transition);
            KisPageMetadataCoordinator coordinator;
            QVERIFY(coordinator.configure(8));
            QVERIFY(KisPageStateMachine().validateInvariants(page));
            QVERIFY(coordinator.registerPage(page));
            const bool mutation = kind == KisPageTransitionKind::DetachPreparedVersion;
            auto batch = mutation ? coordinator.prepareMutation(transaction, &transition.version, 1)
                                  : coordinator.preparePublication(transaction, KisImageEpochId{2}, {transition});
            QCOMPARE(batch.isValid(), expected.accepted);
            if (!expected.accepted)
                continue;
            QVERIFY(expected.effects.isEmpty());
            QVERIFY(mutation ? coordinator.installMutation(std::move(batch), transaction)
                             : coordinator.installPublication(std::move(batch),
                                                              transaction,
                                                              KisImageEpochId{2}));
            KisPageStateSnapshot actual;
            QVERIFY(coordinator.pageSnapshot(pageKey(0), &actual));
            QVERIFY(KisPageStateMachine().validateInvariants(actual));
            const auto &next = expected.next;
            QCOMPARE(actual.key, next.key);
            QCOMPARE(actual.publishedEpoch, next.publishedEpoch);
            QCOMPARE(actual.publishedGeneration, next.publishedGeneration);
            QCOMPARE(actual.publishedDefaultPixelRevision, next.publishedDefaultPixelRevision);
            QCOMPARE(actual.nextGeneration, next.nextGeneration);
            QCOMPARE(actual.writer.isValid(), next.writer.isValid());
            QCOMPARE(actual.authorityHandoff.isValid(), next.authorityHandoff.isValid());
            QCOMPARE(actual.versions.size(), next.versions.size());
            for (qsizetype v = 0; v < actual.versions.size(); ++v) {
                const auto &a = actual.versions.at(v);
                const auto &b = next.versions.at(v);
                QCOMPARE(a.version, b.version);
                QCOMPARE(a.publication, b.publication);
                QCOMPARE(a.authority, b.authority);
                QCOMPARE(a.preparedBy, b.preparedBy);
                QCOMPARE(a.capturedReadViews, b.capturedReadViews);
                QCOMPARE(a.replicas.size(), b.replicas.size());
                for (qsizetype r = 0; r < a.replicas.size(); ++r) {
                    const auto &ar = a.replicas.at(r);
                    const auto &br = b.replicas.at(r);
                    QCOMPARE(ar.replica, br.replica);
                    QCOMPARE(ar.validity, br.validity);
                    QCOMPARE(ar.activeOperation, br.activeOperation);
                    QCOMPARE(ar.readLeases, br.readLeases);
                    QCOMPARE(ar.pinCount, br.pinCount);
                    QCOMPARE(ar.pendingLastUses, br.pendingLastUses);
                }
            }
        }
    }
}

void KisPageStoreReferenceTest::recoverableWriteGuards_data()
{
    QTest::addColumn<int>("bpp");
    QTest::addColumn<int>("variant");
    const char *names[] = {"ready", "old-read", "old-pin", "old-last-use", "missing-before",
        "same-before", "before-not-ready", "before-layout", "target-layout", "provider",
        "provider-epoch", "slot", "old-allocation-generation", "skipped-allocation-generation",
        "allocation-generation-exhausted", "logical-generation", "before-pin-exhausted",
        "foreign-before-version", "missing-writer", "missing-operation", "foreign-transaction", "historical-base",
        "invalid-old-operation", "before-materializing", "authority-handoff", "target-domain"};
    for (int bpp : {1, 4, 8, 16})
        for (int i = 0; i < int(std::size(names)); ++i)
            QTest::newRow(qPrintable(QStringLiteral("bpp%1-%2").arg(bpp).arg(names[i]))) << bpp << i;
}

void KisPageStoreReferenceTest::recoverableWriteGuards()
{
    QFETCH(int, bpp);
    QFETCH(int, variant);
    const KisPageTransaction tx{{74}, {1}};
    auto page = pageWithHistory(variant == 21 ? 1 : 0);
    auto &base = variant == 21 ? page.versions.last() : page.versions.first();
    base.authority.layout.rowStride *= bpp;
    base.authority.layout.byteSize *= bpp;
    if (variant == 14)
        base.authority.allocation.generation = std::numeric_limits<quint64>::max();
    base.replicas.first().replica = base.authority;
    auto before = base.authority;
    before.allocation.slot = 10000;
    base.replicas.append({before, KisReplicaValidity::Valid, {}, {}, 0, {}});
    KisCompletionRegistry registry;
    const auto completion = registry.allocatePending(registry.registerSource(KisCompletionDomain::CpuJob));
    KisPageTransition write;
    write.kind = KisPageTransitionKind::AcquireRecoverableWrite;
    write.baseVersion = base.version;
    write.version = {page.key, page.nextGeneration};
    write.source = before;
    write.target = base.authority;
    write.target.version = write.version;
    write.target.allocation.generation = 2;
    write.operation = {50}; write.writer = {51}; write.transaction = tx.id;
    switch (variant) {
    case 1: base.replicas.first().readLeases.append(KisPageLeaseId{1}); break;
    case 2: base.replicas.first().pinCount = 1; break;
    case 3: base.replicas.first().pendingLastUses.append(completion); break;
    case 4: base.replicas.removeLast(); break;
    case 5: write.source = base.authority; break;
    case 6: base.replicas.last().validity = KisReplicaValidity::Allocated; break;
    case 7:
        base.replicas.last().replica.layout.formatId++;
        write.source = base.replicas.last().replica;
        break;
    case 8: write.target.layout.formatId++; break;
    case 9: write.target.provider.value++; break;
    case 10: write.target.providerEpoch.value++; break;
    case 11: write.target.allocation.slot += 2; break;
    case 12: write.target.allocation.generation = 1; break;
    case 13: write.target.allocation.generation = 3; break;
    case 15: write.version.generation.value++; write.target.version = write.version; break;
    case 16: base.replicas.last().pinCount = std::numeric_limits<quint32>::max(); break;
    case 17: write.source.version.generation.value++; break;
    case 18: write.writer = {}; break;
    case 19: write.operation = {}; break;
    case 20: write.transaction.value++; break;
    case 22: base.replicas.first().activeOperation = {60}; break;
    case 23:
        base.replicas.last().validity = KisReplicaValidity::Materializing;
        base.replicas.last().activeOperation = {60};
        break;
    case 24:
        page.authorityHandoff = {{70}, base.authority, before};
        ++base.replicas.first().pinCount;
        ++base.replicas.last().pinCount;
        break;
    case 25: write.target.domain = KisPageAccessDomain::Ssd; break;
    default: break;
    }
    const KisPageStateMachine machine;
    QString error;
    if (variant == 22) {
        // An authority must be Valid, which excludes an active operation.
        // Invalid input is rejected before any prepared ownership is created.
        QVERIFY(!machine.validateInvariants(page, &error));
        const auto rejected = machine.apply(page, write);
        QVERIFY(!rejected.accepted);
        comparePageRecords(rejected.next, page);
        KisPageMetadataCoordinator metadata;
        QVERIFY(metadata.configure(4));
        QVERIFY(!metadata.registerPage(page, &error));
        return;
    }
    QVERIFY2(machine.validateInvariants(page, &error), qPrintable(error));
    const auto expected = machine.apply(page, write);
    // The reference transition has no outer transaction object; production
    // preparation additionally binds the supplied transaction incarnation.
    QCOMPARE(expected.accepted, variant == 0 || variant == 20);
    if (!expected.accepted)
        comparePageRecords(expected.next, page);
    KisPageMetadataCoordinator metadata;
    QVERIFY(metadata.configure(4));
    QVERIFY2(metadata.registerPage(page, &error), qPrintable(error));
    QVERIFY(!metadata.applyOwner(page.key, write).accepted); // Prepared-only.
    auto prepared = metadata.prepareRecoverableWrite(tx, write, &error);
    QCOMPARE(prepared.isValid(), variant == 0);
    if (prepared.isValid()) {
        QVERIFY(expected.effects.isEmpty()); // The old backing is transferred.
        QVERIFY(metadata.installRecoverableWrite(std::move(prepared), tx));
        auto ready = write; ready.kind = KisPageTransitionKind::PrepareWrite;
        const auto writable = machine.apply(expected.next, ready);
        QVERIFY(writable.accepted);
        QVERIFY(writable.effects.isEmpty());
        KisPageStateSnapshot actual;
        QVERIFY(metadata.pageSnapshot(page.key, &actual));
        comparePageRecords(actual, writable.next);
    } else {
        KisPageStateSnapshot actual;
        QVERIFY(metadata.pageSnapshot(page.key, &actual));
        comparePageRecords(actual, page);
    }
    // The ordinary path never inherits recoverable slot reuse permission.
    auto fresh = write;
    fresh.kind = KisPageTransitionKind::AcquireWrite;
    fresh.source = base.authority;
    if (variant == 0) {
        QVERIFY(!machine.apply(page, fresh).accepted);
        QVERIFY(!metadata.prepareRecoverableWrite(tx, fresh).isValid());
    }
}

void KisPageStoreReferenceTest::preparedRecoverableWriteLifecycle_data()
{
    QTest::addColumn<int>("bpp");
    QTest::addColumn<int>("history");
    QTest::addColumn<bool>("preparedBase");
    QTest::addColumn<int>("outcome");
    for (int bpp : {1, 4, 8, 16})
        for (int history : {0, 128})
            for (bool prepared : {false, true}) {
                if (!history && prepared) continue;
                for (int outcome = 0; outcome < 11; ++outcome) {
                    if (outcome == 10 && !prepared) continue;
                    QTest::newRow(qPrintable(QStringLiteral("bpp%1-H%2-prepared%3-outcome%4")
                        .arg(bpp).arg(history).arg(prepared).arg(outcome)))
                        << bpp << history << prepared << outcome;
                }
            }
}

void KisPageStoreReferenceTest::preparedRecoverableWriteLifecycle()
{
    QFETCH(int, bpp);
    QFETCH(int, history);
    QFETCH(bool, preparedBase);
    QFETCH(int, outcome);
    const KisPageTransaction tx{{74}, {1}};
    auto page = pageWithHistory(history);
    auto &base = preparedBase ? page.versions.last() : page.versions.first();
    if (preparedBase) {
        base.publication = KisPagePublicationState::Prepared;
        base.preparedBy = tx.id;
    }
    base.authority.layout.rowStride *= bpp;
    base.authority.layout.byteSize *= bpp;
    base.replicas.first().replica = base.authority;
    const auto old = base.authority;
    auto before = old; before.allocation.slot = 10000;
    KisCompletionRegistry registry;
    const auto lastUse = registry.allocatePending(registry.registerSource(KisCompletionDomain::CpuJob));
    base.replicas.append({before, KisReplicaValidity::Valid, {}, {{4}}, 2, {lastUse}});
    base.capturedReadViews.append(KisImageEpochSnapshotToken{19});
    KisPageTransition write;
    write.kind = KisPageTransitionKind::AcquireRecoverableWrite;
    write.baseVersion = base.version;
    write.version = {page.key, page.nextGeneration};
    write.source = before; write.target = old;
    write.target.version = write.version; ++write.target.allocation.generation;
    write.writer = {51}; write.operation = {52}; write.transaction = tx.id;
    const auto baseVersion = base.version;
    const KisPageStateMachine machine;
    auto expected = page;
    auto metadata = std::make_unique<KisPageMetadataCoordinator>();
    MetadataEffectReceiver receiver;
    attachEffects(*metadata, receiver);
    QVERIFY(metadata->configure(4));
    QVERIFY(metadata->registerPage(page));
    QString error;
    auto prepared = metadata->prepareRecoverableWrite(tx, write, &error);
    QVERIFY2(prepared.isValid(), qPrintable(error));
    QCOMPARE(metadata->metrics().publicationVersionInputs, quint64(preparedBase ? 2 : 1));
    QCOMPARE(metadata->metrics().fullSnapshotExports, quint64(0));
    const auto reserved = metadata->footprint();
    QVERIFY(reserved.versionArena.outstandingReservations > 0);
    QVERIFY(reserved.replicaArena.outstandingReservations > 0);
    QVERIFY(reserved.overflowArena.outstandingReservations > 0);
    QCOMPARE(reserved.exactVersionIndex.outstandingReservations, quint64(1));
    QVERIFY(reserved.physicalSlotIndex.outstandingReservations > 0);
    QCOMPARE(prepared.backingAuthority(baseVersion), before);
    KisPageStateSnapshot actual;
    QVERIFY(metadata->pageSnapshot(page.key, &actual));
    comparePageRecords(actual, page); // Prepare never detaches or grants a writer.
    KisPageMetadataCoordinator foreign;
    QVERIFY(foreign.configure(4));
    if ((outcome >= 5 && outcome <= 7) || outcome == 10) {
        KisPageTransition change;
        change.version = baseVersion;
        change.transaction = tx.id;
        if (outcome == 10) {
            change.kind = KisPageTransitionKind::RetainCapturedVersion;
            change.readView = {20};
        } else if (outcome == 7) {
            change.kind = KisPageTransitionKind::ReleaseCapturedVersion;
            change.readView = {19};
        } else {
            change.kind = KisPageTransitionKind::AcquireRead;
            change.target = outcome == 5 ? old : before;
            change.lease = {21};
        }
        const auto changed = machine.apply(expected, change);
        QVERIFY2(changed.accepted, qPrintable(changed.rejectionReason));
        QVERIFY(metadata->applyOwner(page.key, change).accepted);
        expected = changed.next;
    }
    if (outcome == 8) {
        auto spare = metadata->prepareRecoverableWrite(tx, write);
        QVERIFY(spare.isValid());
        // Move assignment must release its old reservations exactly once.
        spare = std::move(prepared);
        QVERIFY(!prepared.isValid());
        prepared = std::move(spare);
        QVERIFY(!spare.isValid());
    }
    if (outcome == 9) {
        metadata.reset();
        QVERIFY(!foreign.installRecoverableWrite(std::move(prepared), tx));
        QVERIFY(!prepared.isValid());
        return;
    }
    const bool success = outcome == 0 || outcome == 1 || outcome == 8;
    KisPageMetadataCoordinator::DeferredPublicationCleanup cleanup;
    if (outcome == 2) prepared = {};
    else if (outcome == 3) QVERIFY(!foreign.installRecoverableWrite(std::move(prepared), tx));
    else if (outcome == 4) QVERIFY(!metadata->installMutation(std::move(prepared), tx));
    else QCOMPARE(metadata->installRecoverableWrite(std::move(prepared), tx, &error, &cleanup), success);
    QVERIFY(!prepared.isValid());
    QVERIFY(!metadata->installRecoverableWrite(std::move(prepared), tx));
    while (!cleanup.isEmpty()) QVERIFY(cleanup.clearBatch(1) > 0);
    const auto released = metadata->footprint();
    QCOMPARE(released.versionArena.outstandingReservations, quint64(0));
    QCOMPARE(released.replicaArena.outstandingReservations, quint64(0));
    QCOMPARE(released.overflowArena.outstandingReservations, quint64(0));
    QCOMPARE(released.exactVersionIndex.outstandingReservations, quint64(0));
    QCOMPARE(released.physicalSlotIndex.outstandingReservations, quint64(0));
    if (success) {
        const auto transferred = machine.apply(expected, write);
        QVERIFY2(transferred.accepted, qPrintable(transferred.rejectionReason));
        auto ready = write; ready.kind = KisPageTransitionKind::PrepareWrite;
        const auto writable = machine.apply(transferred.next, ready);
        QVERIFY(writable.accepted);
        expected = writable.next;
    }
    QVERIFY(metadata->pageSnapshot(page.key, &actual));
    comparePageRecords(actual, expected);
    if (!success) return;
    QVERIFY(!actual.findVersion(baseVersion)->findReplica(old));
    QCOMPARE(actual.findVersion(baseVersion)->authority, before);
    QCOMPARE(actual.findVersion(baseVersion)->findReplica(before)->pinCount, quint32(3));
    QCOMPARE(actual.publishedGeneration, page.publishedGeneration);
    // The ordinary writer lifecycle owns both successful publication and
    // cancellation after transfer. It never reattaches or rewrites old A.
    QVector<KisPageTransitionKind> finish;
    if (outcome == 1)
        finish += {KisPageTransitionKind::BeginPublish, KisPageTransitionKind::PublishWrite};
    else
        finish.append(KisPageTransitionKind::CancelWrite);
    for (const auto kind : finish) {
        auto next = write; next.kind = kind;
        const auto oracle = machine.apply(expected, next);
        QVERIFY2(oracle.accepted, qPrintable(oracle.rejectionReason));
        receiver.effects.clear();
        const auto applied = metadata->applyOwner(page.key, next);
        QVERIFY2(applied.accepted, qPrintable(applied.rejectionReason));
        QCOMPARE(receiver.effects.size(), oracle.effects.size());
        for (qsizetype i = 0; i < receiver.effects.size(); ++i)
            QCOMPARE(receiver.effects.at(i).replica, write.target);
        expected = oracle.next;
    }
    if (outcome == 1) {
        KisPageTransition commit;
        commit.kind = KisPageTransitionKind::CommitTransaction;
        commit.version = write.version; commit.transaction = tx.id; commit.imageEpoch = {2};
        const auto oracle = machine.apply(expected, commit);
        QVERIFY(oracle.accepted);
        auto publication = metadata->preparePublication(tx, {2}, {commit});
        QVERIFY(publication.isValid());
        QVERIFY(metadata->installPublication(std::move(publication), tx, {2}));
        expected = oracle.next;
    }
    QVERIFY(metadata->pageSnapshot(page.key, &actual));
    comparePageRecords(actual, expected);
    QCOMPARE(actual.findVersion(baseVersion)->authority, before);
    QCOMPARE(actual.findVersion(baseVersion)->findReplica(before)->pinCount, quint32(2));
    QCOMPARE(actual.findVersion(baseVersion)->capturedReadViews, QVector<KisImageEpochSnapshotToken>{{19}});
    QCOMPARE(actual.findVersion(baseVersion)->findReplica(before)->readLeases, QVector<KisPageLeaseId>{{4}});
    QCOMPARE(actual.findVersion(baseVersion)->findReplica(before)->pendingLastUses, QVector<KisCompletionTicket>{lastUse});
}

void KisPageStoreReferenceTest::recoverableWritePreparationHonorsBudget()
{
    const KisPageTransaction tx{{74}, {1}};
    auto page = pageWithHistory(0);
    auto &base = page.versions.first();
    auto before = base.authority; before.allocation.slot = 2;
    base.replicas.append({before, KisReplicaValidity::Valid, {}, {}, 0, {}});
    KisPageTransition write;
    write.kind = KisPageTransitionKind::AcquireRecoverableWrite;
    write.baseVersion = base.version; write.version = {page.key, page.nextGeneration};
    write.source = before; write.target = base.authority;
    write.target.version = write.version; ++write.target.allocation.generation;
    write.writer = {51}; write.operation = {52}; write.transaction = tx.id;
    quint64 initialBytes = 0;
    {
        KisBackingBudgetController budget;
        KisPageMetadataCoordinator metadata;
        metadata.attachBackingBudget(budget);
        QVERIFY(metadata.configure(1));
        QVERIFY(metadata.registerPage(page));
        initialBytes = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
        auto prepared = metadata.prepareRecoverableWrite(tx, write);
        QVERIFY(prepared.isValid()); // Same input has a positive control.
    }
    KisPageBackingLimits limits; limits.metadataArenaBytes = 2 * initialBytes;
    KisBackingBudgetController budget(limits);
    auto storageOwner = KisMutationStorageAllocator<char>::retained(&budget);
    std::array<KisBackingBudgetReservation, 8> slots;
    for (auto &slot : slots) { slot = budget.reserve({}, nullptr); QVERIFY(slot.isValid()); }
    for (auto &slot : slots) slot.release();
    const quint64 controllerStorage = budget.usage().buckets[
        size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    QVERIFY(controllerStorage > 0);
    {
        KisPageMetadataCoordinator metadata;
        metadata.attachBackingBudget(budget);
        QVERIFY(metadata.configure(1));
        QVERIFY(metadata.registerPage(page));
        const size_t fillerBytes = size_t(limits.metadataArenaBytes - budget.usage()
            .buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam);
        void *filler = kisAllocateMutationStorage(&budget, fillerBytes, 1);
        const auto freeFiller = qScopeGuard([&] { kisFreeMutationStorage(&budget, filler, fillerBytes, 1); });
        QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
                 limits.metadataArenaBytes);
        for (int attempt = 0; attempt < 2; ++attempt) {
            QString error;
            const auto previousPressure = budget.usage().backpressureCount;
            auto prepared = metadata.prepareRecoverableWrite(tx, write, &error);
            QVERIFY(!prepared.isValid());
            QVERIFY(!prepared.needsReprepare()); // Real pressure, not a revision race.
            QCOMPARE(error, QStringLiteral("metadata publication preparation storage was refused"));
            QCOMPARE(budget.usage().backpressureCount, previousPressure + 1);
            KisPageStateSnapshot actual;
            QVERIFY(metadata.pageSnapshot(page.key, &actual));
            comparePageRecords(actual, page);
            const auto usage = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
            QCOMPARE(usage.live.cpuRam, limits.metadataArenaBytes);
            QCOMPARE(usage.reserved.cpuRam, quint64(0));
            QCOMPARE(metadata.footprint().pageActivities, quint64(0));
        }
    }
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam, controllerStorage);
}

void KisPageStoreReferenceTest::mutationWriteSetUsesInlineAndSparseIndexStorage()
{
    KisMutationWriteSet writes;
    writes.reserveKnownTargetCount(12);
    const auto intentFor = [](int x) {
        KisPageWriteIntent intent;
        intent.key = pageKey(x);
        return intent;
    };
    for (int x = 0; x < 12; ++x) {
        KisPageWriteIntent intent = intentFor(x);
        QCOMPARE(writes.getOrCreate(intent).key(), intent.key);
    }
    QCOMPARE(writes.size(), 12);
    QCOMPARE(writes.findIndex(pageKey(0)), 0u);
    QCOMPARE(writes.findIndex(pageKey(8)), 8u);
    QCOMPARE(writes.findIndex(pageKey(11)), 11u);
    QCOMPARE(writes.findIndex(pageKey(99)), KisMutationWriteSet::InvalidEntry);
    QCOMPARE(writes.getOrCreate(intentFor(8)).key(), pageKey(8));
    QCOMPARE(writes.size(), 12);
}

void KisPageStoreReferenceTest::sharedNonPayloadBudgetBoundsStoresAndReleases()
{
    {
        KisPageBackingLimits tinyLimits;
        tinyLimits.metadataArenaBytes = 1;
        auto tiny = std::make_shared<KisBackingBudgetController>(tinyLimits);
        KisBackingBudgetController rejected;
        QString tinyError;
        QVERIFY(!rejected.configureSharedNonPayloadBudget(tiny, &tinyError));
        QVERIFY(!tinyError.isEmpty());
        QCOMPARE(tiny->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
                 quint64(0));
        QCOMPARE(tiny->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].reserved.cpuRam,
                 quint64(0));
    }

    KisPageBackingLimits sharedLimits;
    sharedLimits.metadataArenaBytes = 8192;
    auto shared = std::make_shared<KisBackingBudgetController>(sharedLimits);
    auto firstWarm = shared->reserve({}, nullptr);
    auto secondWarm = shared->reserve({}, nullptr);
    QVERIFY(firstWarm.isValid() && secondWarm.isValid());
    firstWarm.release(); secondWarm.release();
    const auto rootControlBytes = shared->usage()
        .buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    auto first = std::make_unique<KisBackingBudgetController>();
    QString error;
    QVERIFY2(first->configureSharedNonPayloadBudget(shared, &error), qPrintable(error));

    KisBackingBudgetDelta firstMetadata;
    firstMetadata.buckets[size_t(KisBackingBudgetClass::MetadataArena)].cpuRam = 600;
    auto firstReservation = first->reserve(firstMetadata, &error);
    QVERIFY2(firstReservation.isValid(), qPrintable(error));
    first->commitReservation(std::move(firstReservation), firstMetadata);

    // Registration storage is charged to the same parent, but an idle store
    // must not divide the usable pool or throttle the active store.
    auto second = std::make_unique<KisBackingBudgetController>();
    QVERIFY2(second->configureSharedNonPayloadBudget(shared, &error), qPrintable(error));
    // Admit the child's accounting slot before measuring spendable capacity.
    auto secondWarmSlot = second->reserve({}, &error);
    QVERIFY(secondWarmSlot.isValid());
    secondWarmSlot.release();
    const quint64 firstControlBytes = first->usage()
        .buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam - 600;
    const quint64 sharedAfterRegistration = shared->usage()
        .buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    QVERIFY(sharedAfterRegistration > quint64(600));
    const quint64 processFree = sharedLimits.metadataArenaBytes - sharedAfterRegistration;
    QVERIFY(processFree > 0);

    KisBackingBudgetDelta secondCache;
    secondCache.buckets[size_t(KisBackingBudgetClass::OptionalCache)].cpuRam =
        qint64(processFree + 1);
    QVERIFY(!second->reserve(secondCache, &error).isValid());
    QVERIFY(!error.isEmpty());
    secondCache.buckets[size_t(KisBackingBudgetClass::OptionalCache)].cpuRam =
        qint64(processFree);
    auto secondReservation = second->reserve(secondCache, &error);
    QVERIFY2(secondReservation.isValid(), qPrintable(error));
    second->commitReservation(std::move(secondReservation), secondCache);

    QCOMPARE(first->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
             quint64(600) + firstControlBytes);
    QCOMPARE(second->usage().buckets[size_t(KisBackingBudgetClass::OptionalCache)].live.cpuRam,
             processFree);
    QCOMPARE(shared->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
             sharedLimits.metadataArenaBytes);

    first->releaseLive(KisBackingBudgetClass::MetadataArena,
                       KisPageAccessDomain::CpuRam, 600);
    // The remaining active store borrows all released capacity. Fairness
    // between failed operations requires an owned continuation; registration
    // alone is not treated as demand.
    KisBackingBudgetDelta secondMetadata;
    const quint64 secondRemainder = 600;
    secondMetadata.buckets[size_t(KisBackingBudgetClass::MetadataArena)].cpuRam =
        qint64(secondRemainder + 1);
    QVERIFY(!second->reserve(secondMetadata, &error).isValid());
    QVERIFY(error.contains(QStringLiteral("process budget")));
    secondMetadata.buckets[size_t(KisBackingBudgetClass::MetadataArena)].cpuRam =
        qint64(secondRemainder);
    auto releasedCapacity = second->reserve(secondMetadata, &error);
    QVERIFY2(releasedCapacity.isValid(), qPrintable(error));
    second->commitReservation(std::move(releasedCapacity), secondMetadata);

    QCOMPARE(shared->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
             sharedLimits.metadataArenaBytes);

    second->releaseLive(KisBackingBudgetClass::OptionalCache,
                        KisPageAccessDomain::CpuRam, processFree);
    second->releaseLive(KisBackingBudgetClass::MetadataArena,
                        KisPageAccessDomain::CpuRam, secondRemainder);

    second.reset();
    const quint64 soleControlBytes = shared->usage()
        .buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    QVERIFY(soleControlBytes > 0 && soleControlBytes < sharedLimits.metadataArenaBytes);
    KisBackingBudgetDelta soleStoreCapacity;
    const quint64 soleCapacity = sharedLimits.metadataArenaBytes - soleControlBytes;
    soleStoreCapacity.buckets[size_t(KisBackingBudgetClass::MetadataArena)].cpuRam =
        qint64(soleCapacity);
    auto soleStoreReservation = first->reserve(soleStoreCapacity, &error);
    QVERIFY2(soleStoreReservation.isValid(), qPrintable(error));
    first->commitReservation(std::move(soleStoreReservation), soleStoreCapacity);
    first->releaseLive(KisBackingBudgetClass::MetadataArena,
                       KisPageAccessDomain::CpuRam, soleCapacity);
    first.reset();
    QCOMPARE(shared->usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam,
             rootControlBytes);
}

void KisPageStoreReferenceTest::mutationStorageGrowthIsStableAndCharged()
{
    KisBackingBudgetController budget;
    const auto live = [&] {
        return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    auto slot = budget.reserve({}, nullptr);
    QVERIFY(slot.isValid()); slot.release();
    const quint64 controlBytes = live();
    QVERIFY(controlBytes > 0);
    {
        KisMutationWriteSet writes(&budget);
        KisPageWriteIntent intent;
        intent.key = pageKey(0);
        writes.getOrCreate(intent);
        QCOMPARE(live(), controlBytes); // The inline entry needs no dynamic storage.
        intent.key = pageKey(1);
        auto *const stable = &writes.getOrCreate(intent);
        QVERIFY(live() > 0);
        for (int i = 2; i < 1000; ++i) {
            intent.key = pageKey(i);
            writes.getOrCreate(intent);
            QCOMPARE(writes.at(1), stable);
            QCOMPARE(stable->key(), pageKey(1));
        }
        const auto charged = live();
        KisMutationWriteSet transferred(std::move(writes));
        QCOMPARE(writes.size(), 0);
        QCOMPARE(transferred.at(1), stable);
        QCOMPARE(live(), charged);
        for (int i = 0; i < 1000; ++i)
            QCOMPARE(transferred.findIndex(pageKey(i)), quint32(i));
        // Reaccess needs neither a new slot nor another index node/reservation.
        const auto pressure = budget.usage().backpressureCount;
        for (int i = 0; i < 1000; ++i) {
            intent.key = pageKey(i);
            QCOMPARE(&transferred.getOrCreate(intent), transferred.at(quint32(i)));
        }
        QCOMPARE(live(), charged);
        QCOMPARE(budget.usage().backpressureCount, pressure);
    }
    QCOMPARE(live(), controlBytes);
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].reserved.cpuRam, quint64(0));
    // Exercise the cold-storage primitive with nontrivial owning values too.
    {
        KisMutationStorage<std::unique_ptr<int>, 2> cold(&budget);
        cold.emplace_back(std::make_unique<int>(17));
        auto *const slot = cold.at(0);
        auto *const value = slot->get();
        for (int i = 0; i < 1000; ++i)
            cold.emplace_back(std::make_unique<int>(i));
        QCOMPARE(cold.at(0), slot);
        QCOMPARE(cold.at(0)->get(), value);
        QCOMPARE(**slot, 17);
        cold.erase(0);
        QVERIFY(!cold.at(0));
        QVERIFY(live() > 0); // Retained capacity remains charged after erasure.
    }
    QCOMPARE(live(), controlBytes);
}

void KisPageStoreReferenceTest::mutationStorageReclaimsBlocksAndRejectsABA()
{
    KisBackingBudgetController budget;
    auto accountingSlot = budget.reserve({}, nullptr);
    QVERIFY(accountingSlot.isValid()); accountingSlot.release();
    const quint64 controllerStorage = budget.usage().buckets[
        size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    QVERIFY(controllerStorage > 0);
    const auto live = [&] {
        return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    using Storage = KisMutationStorage<int, 2>;
    Storage::ReleasedBlocks released;
    {
        Storage storage(&budget);
        std::vector<Storage::Handle> slots;
        for (int i = 0; i < 6; ++i) {
            const auto slot = storage.prepareSlot();
            QVERIFY(storage.emplacePrepared(slot, i));
            slots.push_back(slot);
        }
        auto *const last = storage.at(slots.back());
        const auto charged = live();
        QVERIFY(storage.erase(slots[0]));
        QVERIFY(storage.erase(slots[1]));
        QCOMPARE(storage.size(), size_t(4));
        released = storage.takeReleasedBlocks();
        QVERIFY(!released.isEmpty());
        QCOMPARE(live(), charged); // Detach has not freed actual memory yet.
        released.reset();
        QVERIFY(live() < charged);
        const auto reused = storage.prepareSlot();
        QCOMPARE(reused.index, slots[0].index);
        QVERIFY(reused.incarnation != slots[0].incarnation);
        QVERIFY(storage.emplacePrepared(reused, 99));
        QVERIFY(!storage.at(slots[0]));
        QVERIFY(!storage.erase(slots[0]));
        QVERIFY(!storage.emplacePrepared(reused, 100));
        QCOMPARE(*storage.at(reused), 99);
        QCOMPARE(storage.at(slots.back()), last);
        QCOMPARE(*last, 5);
        int count = 0;
        for (auto slot = storage.first(); slot.isValid(); slot = storage.next(slot)) ++count;
        QCOMPARE(count, 5);
        Storage moved(std::move(storage));
        QCOMPARE(storage.size(), size_t(0));
        QVERIFY(!storage.first().isValid());
        QCOMPARE(moved.at(slots.back()), last);
        for (auto slot = moved.first(); slot.isValid();) {
            const auto next = moved.next(slot);
            QVERIFY(moved.erase(slot));
            slot = next;
        }
        released = moved.takeReleasedBlocks();
        QVERIFY(!released.isEmpty());
        QVERIFY(live() > 0);
    }
    QVERIFY(live() > 0); // The carrier still owns detached blocks and their charge.
    released.reset();
    QCOMPARE(live(), controllerStorage);
}

void KisPageStoreReferenceTest::mutationStorageIncarnationExhaustion()
{
    KisMutationStorage<int, 2, quint8> storage;
    KisMutationSlotHandle first;
    for (int i = 1; i <= 255; ++i) {
        const auto slot = storage.prepareSlot();
        if (i == 1) first = slot;
        QCOMPARE(slot.index, first.index);
        QCOMPARE(slot.incarnation, quint64(i));
        QVERIFY(storage.emplacePrepared(slot, i));
        if (i != 1) QVERIFY(!storage.at(first));
        QVERIFY(storage.erase(slot));
        QVERIFY(!storage.erase(slot));
    }
    QVERIFY_THROWS_EXCEPTION(std::bad_alloc, storage.prepareSlot());
    QVERIFY(!storage.first().isValid());
    QCOMPARE(storage.size(), size_t(0));
}

void KisPageStoreReferenceTest::mutationWriteSetRecyclesHoles_data()
{
    QTest::addColumn<int>("count");
    for (int count : {2, 8, 9, 65, 1000})
        QTest::newRow(qPrintable(QString::number(count))) << count;
}

void KisPageStoreReferenceTest::mutationWriteSetRecyclesHoles()
{
    QFETCH(int, count);
    KisBackingBudgetController budget;
    KisMutationWriteSet writes(&budget);
    std::vector<KisMutationSlotHandle> original;
    KisPageWriteIntent intent;
    for (int i = 0; i < count; ++i) {
        intent.key = pageKey(i);
        writes.getOrCreate(intent);
        original.push_back(writes.handleAt(writes.findIndex(intent.key)));
    }
    auto *const stable = writes.at(original.back());
    for (int i = 0; i < count - 1; i += 2) QVERIFY(writes.erase(original[i]));
    QVERIFY(!writes.at(original.front()));
    QCOMPARE(writes.find(pageKey(count - 1)), stable); // Inline hole cannot hide overflow.
    int alive = 0;
    for (auto slot = writes.firstEntry(); slot.isValid(); slot = writes.nextEntry(slot)) {
        const auto *entry = writes.at(slot);
        QVERIFY(entry);
        QCOMPARE(writes.find(entry->key()), entry);
        ++alive;
    }
    QCOMPARE(writes.size(), qsizetype(alive));
    writes.takeReleasedBlocks().reset();
    for (int i = 0; i < count - 1; i += 2) {
        intent.key = pageKey(count + i);
        writes.getOrCreate(intent);
        QVERIFY(!writes.at(original[i]));
        QVERIFY(!writes.erase(original[i]));
        QVERIFY(!writes.find(pageKey(i)));
    }
    QCOMPARE(writes.size(), qsizetype(count));
    QCOMPARE(writes.find(pageKey(count - 1)), stable);
    // Repeated inline reuse keeps its own incarnation, even without an index.
    const auto charged = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    for (int i = 0; i < 1000; ++i) {
        const auto old = writes.handleAt(0);
        QVERIFY(writes.erase(old));
        intent.key = pageKey(100000 + i);
        writes.getOrCreate(intent);
        QVERIFY(!writes.at(old));
        QCOMPARE(writes.findIndex(intent.key), quint32(0));
    }
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam, charged);
}

void KisPageStoreReferenceTest::mutationWriteSetRejectsLiveResourceErasure()
{
    KisPageMetadataCoordinator metadata;
    KisImageEpochReferenceModel epochs;
    KisPageOwnerLedger owner;
    KisBackingBudgetController budget;
    KisPageWriteCoordinator coordinator(metadata, epochs, budget, owner);
    KisMutationWriteSet writes;
    KisPageWriteIntent intent;
    intent.key = pageKey(0);
    auto &entry = writes.getOrCreate(intent);
    const auto slot = writes.handleAt(0);
    KisPageTransition prepared;
    prepared.version = pageVersion(0, 2);
    coordinator.recordPrepared(entry, intent, prepared);
    coordinator.recordExposure(entry, true);
    QVERIFY(!writes.erase(slot));
    coordinator.recordExposure(entry, false);
    entry.setColdPage(7);
    QVERIFY(!writes.erase(slot));
    entry.setColdPage(KisMutationWriteSet::InvalidEntry);
    struct Source final : KisPageReplicaSource {
        Source() : KisPageReplicaSource({1}, {1}, allocationDescriptor()) {}
    };
    entry.setInitializationSource(std::make_shared<Source>());
    QVERIFY(!writes.erase(slot));
    entry.setInitializationSource({});
    QVERIFY(writes.erase(slot));
    QVERIFY(!writes.at(slot));
}

void KisPageStoreReferenceTest::writeAdmissionReleasesSubsetClaims()
{
    QMutex mutex;
    QWaitCondition condition;
    KisPageWriteAdmission admission(mutex, condition);
    KisMutationWriteSet writes;
    KisPageWriteIntent intent;
    for (int i = 0; i < 17; ++i) {
        intent.key = pageKey(i);
        writes.getOrCreate(intent);
    }
    KisMutationWriteSet moved;
    KisMutationWriteSet competing;
    intent.key = pageKey(100);
    competing.getOrCreate(intent);
    auto claims = admission.beginClaimSet(writes);
    auto foreign = admission.beginClaimSet(competing);
    QString error;
    QMutexLocker lock(&mutex);
    QVERIFY(admission.claimAll(claims, lock, &error));
    const auto oldInline = writes.handleAt(0);
    for (int i : {0, 8, 16}) {
        const auto slot = writes.handleAt(quint32(i));
        QVERIFY(admission.releaseOneLocked(claims, slot));
        QVERIFY(writes.erase(slot));
        QVERIFY(!admission.releaseOneLocked(claims, slot));
    }
    QCOMPARE(admission.activeNativeClaimCountLocked(), qsizetype(14));
    writes.getOrCreate(intent);
    const auto current = writes.handleAt(0);
    QVERIFY(admission.claimOne(claims, current, lock, &error));
    QVERIFY(!admission.claimOne(claims, oldInline, lock, &error));
    QVERIFY(!admission.releaseOneLocked(claims, oldInline));
    QVERIFY(!admission.releaseOneLocked(foreign, competing.handleAt(0)));
    QVERIFY(admission.pageClaimedLocked(pageKey(100)));
    QVERIFY(admission.claimDirectLocked(pageKey(8), 99, lock));
    moved = std::move(writes);
    admission.rebindClaimSetLocked(claims, moved);
    QVERIFY(admission.ownsClaimSetLocked(claims));
    claims.releaseLocked();
    QCOMPARE(admission.activeNativeClaimCountLocked(), qsizetype(0));
    QVERIFY(admission.pageClaimedLocked(pageKey(8)));
    admission.releaseDirectLocked(pageKey(8), 99);
    foreign.releaseLocked();
}

void KisPageStoreReferenceTest::mutationWriteSetBudgetFailureIsAtomic_data()
{
    QTest::addColumn<int>("count");
    for (int count : {2, 9, 10, 65, 1000})
        QTest::newRow(qPrintable(QString::number(count))) << count;
}

void KisPageStoreReferenceTest::mutationWriteSetBudgetFailureIsAtomic()
{
    QFETCH(int, count);
    const auto add = [](KisMutationWriteSet &writes, int i) {
        KisPageWriteIntent intent;
        intent.key = pageKey(i);
        return &writes.getOrCreate(intent);
    };
    quint64 admittedBytes = 0;
    {
        KisBackingBudgetController probe;
        KisMutationWriteSet writes(&probe);
        for (int i = 0; i < count - 1; ++i) add(writes, i);
        admittedBytes = probe.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    }
    KisPageBackingLimits limits;
    limits.metadataArenaBytes = admittedBytes;
    KisBackingBudgetController budget(limits);
    {
        KisMutationWriteSet writes(&budget);
        for (int i = 0; i < count - 1; ++i) add(writes, i);
        const auto *const stable = writes.at(0);
        for (int attempt = 0; attempt < 2; ++attempt) {
            QVERIFY_THROWS_EXCEPTION(std::bad_alloc, add(writes, count - 1));
            QCOMPARE(writes.size(), count - 1);
            QCOMPARE(writes.at(0), stable);
            QCOMPARE(writes.findIndex(pageKey(count - 1)), KisMutationWriteSet::InvalidEntry);
            for (int i = 0; i < count - 1; ++i)
                QCOMPARE(writes.findIndex(pageKey(i)), quint32(i));
            const auto usage = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
            QCOMPARE(usage.live.cpuRam, admittedBytes);
            QCOMPARE(usage.reserved.cpuRam, quint64(0));
        }
        QVERIFY_THROWS_EXCEPTION(std::bad_alloc, writes.reserveKnownTargetCount(count + 1000));
        QCOMPARE(writes.size(), count - 1);
        for (int i = 0; i < count - 1; ++i)
            QCOMPARE(writes.findIndex(pageKey(i)), quint32(i));
    }
    // Inline-only refusal needs no accounting storage. Once dynamic storage
    // has been admitted its controller cache remains charged after writes die.
    KisBackingBudgetController probe;
    auto accountingSlot = probe.reserve({}, nullptr);
    QVERIFY(accountingSlot.isValid()); accountingSlot.release();
    const quint64 retainedControlBytes = admittedBytes ? probe.usage().buckets[
        size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam : 0;
    const auto usage = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
    QCOMPARE(usage.live.cpuRam, retainedControlBytes);
    QCOMPARE(usage.reserved.cpuRam, quint64(0));
}

void KisPageStoreReferenceTest::writeAdmissionClaimsWholeSetsAtomically()
{
    QMutex mutex;
    QWaitCondition condition;
    KisPageMetadataCoordinator metadata;
    KisImageEpochReferenceModel epochs;
    KisPageOwnerLedger owner;
    KisPageWriteAdmission admission(mutex, condition);
    KisBackingBudgetController budget;
    KisPageWriteCoordinator coordinator(metadata, epochs, budget, owner);

    KisMutationWriteSet firstWrites;
    KisPageWriteIntent intent;
    intent.key = pageKey(0);
    firstWrites.getOrCreate(intent);
    intent.key = pageKey(1);
    firstWrites.getOrCreate(intent);
    KisMutationWriteSet secondWrites;
    secondWrites.getOrCreate(intent);
    intent.key = pageKey(2);
    secondWrites.getOrCreate(intent);

    KisPageWriteAdmission::ClaimSet firstClaims;
    KisPageWriteAdmission::ClaimSet secondClaims;
    QString error;
    {
        QMutexLocker lock(&mutex);
        firstClaims = admission.beginClaimSet(firstWrites);
        secondClaims = admission.beginClaimSet(secondWrites);
        QVERIFY(admission.claimAll(firstClaims, lock, &error));
        QVERIFY(!admission.claimAll(secondClaims, lock, &error));
        QCOMPARE(admission.activeNativeClaimCountLocked()
                     + admission.activeGenericClaimCountLocked(), 2);
    }
    firstClaims.release();
    {
        QMutexLocker lock(&mutex);
        QVERIFY(admission.claimAll(secondClaims, lock, &error));
        QCOMPARE(admission.activeNativeClaimCountLocked()
                     + admission.activeGenericClaimCountLocked(), 2);
    }
    secondClaims.release();
    {
        QMutexLocker lock(&mutex);
        QCOMPARE(admission.activeNativeClaimCountLocked()
                     + admission.activeGenericClaimCountLocked(), 0);
        const auto genericTransaction = KisPageTransactionId{13};
        firstClaims = admission.beginClaimSet(firstWrites);
        QVERIFY(admission.claimOne(firstClaims, firstWrites.handleAt(0), lock, &error));
        QVERIFY(admission.pageClaimedLocked(pageKey(0)));
        QVERIFY(!admission.pageClaimedLocked(pageKey(1)));
        QCOMPARE(admission.activeNativeClaimCountLocked(), 1);
        QVERIFY(!admission.claimDirectLocked(pageKey(0), genericTransaction.value, lock));
        secondClaims = admission.beginClaimSet(firstWrites);
        QVERIFY(!admission.claimAll(secondClaims, lock, &error));
        firstClaims.releaseLocked();
        QVERIFY(admission.claimDirectLocked(pageKey(0), genericTransaction.value, lock));
        QCOMPARE(admission.activeGenericClaimCountLocked(), 1);
        QVERIFY(admission.pageClaimedLocked(pageKey(0)));
        QVERIFY(!admission.claimOne(secondClaims, firstWrites.handleAt(0), lock, &error));
        admission.releaseDirectLocked(pageKey(0), genericTransaction.value);
        QCOMPARE(admission.activeNativeClaimCountLocked()
                     + admission.activeGenericClaimCountLocked(), 0);
        QCOMPARE(admission.activeNativeClaimCountLocked(), 0);
        QCOMPARE(admission.activeGenericClaimCountLocked(), 0);
    }
    secondClaims.release();

    // A managed adapter moves the exact write set into its session. Rebind
    // the move-only claim without releasing/reacquiring any PageKey between
    // the two owners; the previous storage may then be destroyed.
    KisMutationWriteSet adapterWrites;
    intent.key = pageKey(3);
    adapterWrites.getOrCreate(intent);
    intent.key = pageKey(4);
    adapterWrites.getOrCreate(intent);
    KisPageWriteAdmission::ClaimSet adapterClaims;
    {
        QMutexLocker lock(&mutex);
        adapterClaims = admission.beginClaimSet(adapterWrites);
        QVERIFY(admission.claimAll(adapterClaims, lock, &error));
    }
    KisMutationWriteSet sessionWrites = std::move(adapterWrites);
    {
        QMutexLocker lock(&mutex);
        admission.rebindClaimSetLocked(adapterClaims, sessionWrites);
        QVERIFY(admission.ownsClaimSetLocked(adapterClaims));
        QCOMPARE(admission.activeNativeClaimCountLocked()
                     + admission.activeGenericClaimCountLocked(), 2);
    }
    adapterClaims.release();
    {
        QMutexLocker lock(&mutex);
        QCOMPARE(admission.activeNativeClaimCountLocked()
                     + admission.activeGenericClaimCountLocked(), 0);
    }
}

void KisPageStoreReferenceTest::admissionStorageRevalidatesGrowth()
{
    KisPageBackingLimits limits; limits.metadataArenaBytes = 256 * 1024;
    KisBackingBudgetController budget(limits);
    auto accountingSlot = budget.reserve({}, nullptr);
    QVERIFY(accountingSlot.isValid()); accountingSlot.release();
    const quint64 controllerStorage = budget.usage().buckets[
        size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    QVERIFY(controllerStorage > 0);
    const auto bytes = [&] { return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    {
        KisMutationAdmissionTable<quint64, quint64> table(&budget);
        for (quint64 i = 1; i <= 4; ++i) QVERIFY(table.insertPrepared(i, i * 7));
        QCOMPARE(bytes(), controllerStorage);
        auto stale = table.planGrowth(1);
        stale.allocate();
        QVERIFY(table.capture(stale));
        *table.find(2) = 1234;
        table.changed(); // same keys/size, but a different activity counter
        stale.build();
        QVERIFY(!table.install(stale));
        QCOMPARE(*table.find(2), quint64(1234));
        const auto preparedBytes = bytes();
        const size_t fillerBytes = size_t(limits.metadataArenaBytes - preparedBytes);
        void *filler = kisAllocateMutationStorage(&budget, fillerBytes, 1);
        const auto releaseFiller = qScopeGuard([&] { kisFreeMutationStorage(&budget, filler, fillerBytes, 1); });
        for (int churn = 0; churn < 6; ++churn) {
            // More than the former three attempts, at actual hard capacity.
            // Rebuild retains one candidate and captures current values.
            QVERIFY(table.capture(stale, 1));
            stale.build();
            *table.find(2) += 1; table.changed();
            QVERIFY(!table.install(stale, 1));
            QCOMPARE(bytes(), limits.metadataArenaBytes);
        }
        *table.find(2) = 1234; table.changed();
        QVERIFY(table.capture(stale, 1)); stale.build();
        QVERIFY(table.install(stale, 1));
        QCOMPARE(*table.find(2), quint64(1234));
        kisFreeMutationStorage(&budget, std::exchange(filler, nullptr), fillerBytes, 1);
        stale = {};
        QVERIFY(bytes() > controllerStorage && bytes() < preparedBytes);
        for (quint64 i = 5; i <= 1000; ++i) {
            if (!table.canInsert(1)) {
                auto growth = table.planGrowth(1);
                growth.allocate();
                QVERIFY(table.capture(growth));
                growth.build();
                QVERIFY(table.install(growth));
            }
            QVERIFY(table.insertPrepared(i, i * 7));
        }
        QCOMPARE(table.size(), size_t(1000));
        const auto charged = bytes();
        QVERIFY(charged > 0);
        // Deletion repairs displaced clusters; repeated reuse consumes the
        // actual prepared buckets without nodes or retained tombstones.
        for (quint64 i = 1; i <= 1000; i += 2) QVERIFY(table.erase(i));
        for (quint64 i = 2; i <= 1000; i += 2)
            QCOMPARE(*table.find(i), i == 2 ? quint64(1234) : i * 7);
        for (quint64 i = 1; i <= 1000; i += 2) QVERIFY(table.insertPrepared(i, i * 11));
        QCOMPARE(bytes(), charged);
        for (quint64 i = 1; i <= 1000; ++i) QVERIFY(table.erase(i));
        QCOMPARE(table.size(), size_t(0));
        QCOMPARE(bytes(), charged); // retained prepared capacity is still billed
    }
    QCOMPARE(bytes(), controllerStorage);
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].reserved.cpuRam, quint64(0));
}

void KisPageStoreReferenceTest::admissionStorageCaptureRebasesBeforeCopy()
{
    KisBackingBudgetController budget;
    auto accountingSlot = budget.reserve({}, nullptr);
    QVERIFY(accountingSlot.isValid()); accountingSlot.release();
    const quint64 controllerStorage = budget.usage().buckets[
        size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    QVERIFY(controllerStorage > 0);
    for (int change = 0; change < 3; ++change) {
        KisMutationAdmissionTable<quint64, quint64> table(&budget);
        QVERIFY(table.insertPrepared(1, 7)); QVERIFY(table.insertPrepared(2, 14));
        auto growth = table.planGrowth(7); growth.allocate();
        if (change == 0) { *table.find(2) = 1234; table.changed(); }
        if (change == 1) { QVERIFY(table.insertPrepared(3, 21)); QVERIFY(table.insertPrepared(4, 28)); }
        if (change == 2) { QVERIFY(table.erase(2)); QVERIFY(table.insertPrepared(9, 63)); }
        // No snapshot existed during allocation. The actual copy must include
        // these current records, not consume a retry or resurrect old values.
        QVERIFY(table.capture(growth, 7)); growth.build(); QVERIFY(table.install(growth, 7));
        QVERIFY(table.canInsert(7)); QCOMPARE(*table.find(1), quint64(7));
        if (change == 0) QCOMPARE(*table.find(2), quint64(1234));
        if (change == 1) { QCOMPARE(*table.find(3), quint64(21)); QCOMPARE(*table.find(4), quint64(28)); }
        if (change == 2) { QVERIFY(!table.contains(2)); QCOMPARE(*table.find(9), quint64(63)); }
    }
    {
        KisMutationAdmissionTable<quint64, quint64> table(&budget), foreign(&budget);
        QVERIFY(table.insertPrepared(1, 7));
        auto small = table.planGrowth(7); small.allocate();
        QVERIFY(!foreign.capture(small, 7)); // matching shape is not ownership
        for (quint64 i = 2; i <= 4; ++i) QVERIFY(table.insertPrepared(i, i * 7));
        QVERIFY(!table.capture(small, 7)); // new records no longer fit the target
        auto enough = table.planGrowth(7); enough.allocate();
        QVERIFY(table.capture(enough, 7)); enough.build(); QVERIFY(table.install(enough, 7));
        QVERIFY(!table.capture(small, 0)); // its snapshot buffer has the old shape
        for (quint64 i = 1; i <= 4; ++i) QCOMPARE(*table.find(i), i * 7);
    }
    const auto usage = budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)];
    QCOMPARE(usage.live.cpuRam, controllerStorage); QCOMPARE(usage.reserved.cpuRam, quint64(0));
    // admissionStorageRevalidatesGrowth separately keeps the strict rejection
    // of value/revision changes after capture and before installation.
}

void KisPageStoreReferenceTest::writeAdmissionBudgetFailureIsAtomic_data()
{
    QTest::addColumn<int>("count");
    for (int count : {5, 9, 65}) QTest::newRow(qPrintable(QString::number(count))) << count;
}

void KisPageStoreReferenceTest::writeAdmissionBudgetFailureIsAtomic()
{
    QFETCH(int, count);
    KisPageBackingLimits limits; limits.metadataArenaBytes = 1;
    KisBackingBudgetController budget(limits);
    QMutex mutex; QWaitCondition condition;
    KisPageWriteAdmission admission(mutex, condition, &budget);
    KisMutationWriteSet writes;
    for (int i = 0; i < count; ++i) {
        KisPageWriteIntent intent; intent.key = pageKey(i); writes.getOrCreate(intent);
    }
    KisPageWriteAdmission::ClaimSet claims;
    QMutexLocker lock(&mutex);
    QString error;
    claims = admission.beginClaimSet(writes);
    QVERIFY(admission.claimOne(claims, writes.handleAt(0), lock, &error));
    QVERIFY(admission.claimOne(claims, writes.handleAt(1), lock, &error));
    QVERIFY(!admission.claimAll(claims, lock, &error));
    QVERIFY(error.contains(QStringLiteral("storage")));
    QCOMPARE(admission.activeNativeClaimCountLocked(), qsizetype(2));
    for (int i = 0; i < count; ++i) QCOMPARE(admission.pageClaimedLocked(pageKey(i)), i < 2);
    QVERIFY(admission.claimDirectLocked(pageKey(count), 17, lock));
    QVERIFY(admission.claimDirectLocked(pageKey(count + 1), 17, lock));
    QVERIFY(!admission.claimDirectLocked(pageKey(count + 2), 17, lock, &error));
    QCOMPARE(admission.activeGenericClaimCountLocked(), qsizetype(2));
    claims.releaseLocked();
    QVERIFY(admission.pageClaimedLocked(pageKey(count)));
    admission.releaseDirectLocked(pageKey(count), 17);
    admission.releaseDirectLocked(pageKey(count + 1), 17);
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam, quint64(0));
}

void KisPageStoreReferenceTest::writeAdmissionTokenExhaustion()
{
    QMutex mutex; QWaitCondition condition;
    KisPageWriteAdmission admission(mutex, condition);
    KisMutationWriteSet writes;
    KisPageWriteIntent intent; intent.key = pageKey(0); writes.getOrCreate(intent);
    KisPageWriteAdmission::ClaimSet older, last, rejected;
    QMutexLocker lock(&mutex);
    admission.m_nextClaimToken = std::numeric_limits<quint64>::max() - 1;
    older = admission.beginClaimSet(writes);
    last = admission.beginClaimSet(writes);
    rejected = admission.beginClaimSet(writes);
    QVERIFY(older.isValid()); QVERIFY(last.isValid()); QVERIFY(!rejected.isValid());
    QString error;
    QVERIFY(admission.claimAll(older, lock, &error));
    QVERIFY(!admission.claimAll(last, lock, &error));
    older.releaseLocked();
    QVERIFY(admission.claimAll(last, lock, &error));
    QVERIFY(!admission.beginClaimSet(writes).isValid());
    last.releaseLocked();
}

void KisPageStoreReferenceTest::mutationActivityAdmissionPreservesExisting()
{
    KisPageBackingLimits limits; limits.metadataArenaBytes = 1;
    KisBackingBudgetController budget(limits);
    KisPageMetadataCoordinator metadata;
    KisImageEpochReferenceModel epochs;
    KisPageOwnerLedger owner;
    KisPageWriteCoordinator coordinator(metadata, epochs, budget, owner);
    QMutex mutex; QMutexLocker lock(&mutex);
    QString error;
    for (quint64 i = 1; i <= 4; ++i) QVERIFY(coordinator.beginSessionActivity({i}, lock, &error));
    QVERIFY(!coordinator.beginPreparationActivity({5}, lock, &error));
    QVERIFY(!coordinator.transactionHasMutationActivity({5}));
    QVERIFY(!coordinator.beginGenericActivity({5}));
    for (quint64 i = 1; i <= 4; ++i) {
        QVERIFY(coordinator.beginPreparationActivity({i}, lock, &error));
        QVERIFY(coordinator.beginGenericActivity({i}));
        coordinator.endSessionActivity({i});
        coordinator.endPreparationActivity({i});
        QVERIFY(coordinator.transactionHasMutationActivity({i}));
        QVERIFY(!coordinator.transactionHasSessionOrPreparation({i}));
        coordinator.endGenericActivity({i});
        QVERIFY(!coordinator.transactionHasMutationActivity({i}));
    }
    QVERIFY(coordinator.beginSessionActivity({5}, lock, &error));
    coordinator.endSessionActivity({5});
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam, quint64(0));
}

void KisPageStoreReferenceTest::freshWriteSelectorAndBackingBudgetAreBounded()
{
    QMutex mutex;
    QWaitCondition condition;
    KisPageMetadataCoordinator metadata;
    KisImageEpochReferenceModel epochs;
    KisPageOwnerLedger owner;
    KisPageWriteAdmission admission(mutex, condition);
    KisPageBackingLimits limits;
    limits.activePendingBytes = 4096;
    limits.logicalCurrentBytes = 4096;
    limits.retainedHistoryBytes = 4096;
    limits.optionalCacheBytes = 4096;
    limits.maxTransientVersionsPerPage = 0;
    limits.metadataArenaBytes = 4 * 1024 * 1024;
    limits.residentCurrentBytes.cpuRam = 4096;
    limits.residentHistoryBytes.cpuRam = 4096;
    KisBackingBudgetController budget(limits);
    owner.attachBackingBudget(budget);
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig providerConfig;
    providerConfig.provider = {1};
    providerConfig.providerEpoch = {1};
    providerConfig.budgetBytes = 64 * 1024 * 1024;
    QString error;
    QVERIFY(owner.configure(completions, &error));
    QVERIFY(provider->configure(providerConfig, completions, &error));
    QVERIFY(owner.registerProvider(provider, &error));
    KisPageWriteCoordinator coordinator(metadata, epochs, budget, owner);

    KisPageWriteIntent intent;
    intent.key = pageKey(0);
    intent.flags = quint8(KisPageWriteIntentFlag::InputBytesReady);
    QCOMPARE(coordinator.select(intent), KisPageWritePlanKind::FreshPayload);
    intent.flags = 0;
    intent.mode = KisPageWriteMode::DiscardContents;
    QCOMPARE(coordinator.select(intent), KisPageWritePlanKind::FreshDiscard);
    intent.mode = KisPageWriteMode::PreserveContents;
    QCOMPARE(coordinator.select(intent), KisPageWritePlanKind::FreshCow);
    KisMutationPageEntry pendingEntry(intent);
    KisPageTransition prepared;
    prepared.baseVersion = pageVersion(0, 1);
    prepared.version = pageVersion(0, 2);
    prepared.source = replica(prepared.baseVersion, 1, 1, 1);
    prepared.target = replica(prepared.version, 1, 2, 1);
    prepared.transaction = {91};
    prepared.operation = {71};
    prepared.writer = {81};
    prepared.writeMode = intent.mode;
    coordinator.recordPrepared(pendingEntry, intent, prepared);
    intent.inputKind = KisPageWriteInputKind::Semantic;
    QCOMPARE(coordinator.select(intent), KisPageWritePlanKind::SemanticOnly);
    QCOMPARE(coordinator.select(intent, &pendingEntry), KisPageWritePlanKind::ReusePending);
    QVERIFY(pendingEntry.isCpuWrite());
    QVERIFY(!pendingEntry.isExposed());
    for (int i = 0; i < 2; ++i) {
        coordinator.recordExposure(pendingEntry, true);
        QVERIFY(pendingEntry.isExposed());
        coordinator.recordExposure(pendingEntry, false);
        QVERIFY(!pendingEntry.isExposed());
    }
    const auto reconstructed = coordinator.writeTransition(pendingEntry, prepared.transaction,
                                                            prepared.source, prepared.target);
    QCOMPARE(reconstructed.baseVersion, prepared.baseVersion);
    QCOMPARE(reconstructed.version, prepared.version);
    QCOMPARE(reconstructed.operation, prepared.operation);
    QCOMPARE(reconstructed.writer, prepared.writer);
    QCOMPARE(reconstructed.transaction, prepared.transaction);
    QCOMPARE(reconstructed.writeMode, prepared.writeMode);
    QCOMPARE(reconstructed.source, prepared.source);
    QCOMPARE(reconstructed.target, prepared.target);
    const KisPageTransactionId transaction{91};
    QMutexLocker activityLock(&mutex);
    QVERIFY(coordinator.beginSessionActivity(transaction, activityLock));
    QVERIFY(coordinator.beginPreparationActivity(transaction, activityLock));
    QVERIFY(coordinator.beginGenericActivity(transaction));
    QVERIFY(coordinator.transactionHasMutationActivity(transaction));
    QVERIFY(coordinator.transactionHasSessionOrPreparation(transaction));
    QVERIFY(coordinator.transactionHasSession(transaction));
    coordinator.endGenericActivity(transaction);
    coordinator.endPreparationActivity(transaction);
    QVERIFY(coordinator.transactionHasSession(transaction));
    coordinator.endSessionActivity(transaction);
    QVERIFY(!coordinator.transactionHasMutationActivity(transaction));

    KisBackingBudgetDelta firstDelta;
    firstDelta.buckets[size_t(KisBackingBudgetClass::ActivePending)].cpuRam = 3072;
    auto first = budget.reserve(firstDelta, &error);
    QVERIFY2(first.isValid(), qPrintable(error));
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::ActivePending)].reserved.cpuRam, 3072u);

    KisBackingBudgetDelta secondDelta;
    secondDelta.buckets[size_t(KisBackingBudgetClass::ActivePending)].cpuRam = 2048;
    auto rejected = budget.reserve(secondDelta, &error);
    QVERIFY(!rejected.isValid());
    QCOMPARE(budget.usage().backpressureCount, 1u);
    first.release();
    auto second = budget.reserve(secondDelta, &error);
    QVERIFY2(second.isValid(), qPrintable(error));
    second.release();
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::ActivePending)].reserved.cpuRam, 0u);

    const auto current = replica(pageVersion(0, 1), 1, 1, 1);
    const auto pending = replica(pageVersion(0, 2), 1, 1, 2);
    KisBackingBudgetDelta currentBytes;
    currentBytes.buckets[size_t(KisBackingBudgetClass::Current)].cpuRam = 4096;
    auto currentReservation = budget.reserve(currentBytes, &error);
    QVERIFY2(currentReservation.isValid(), qPrintable(error));
    QVERIFY(owner.registerBacking(current, currentReservation, KisBackingBudgetClass::Current, &error));
    KisBackingBudgetDelta pendingBytes;
    pendingBytes.buckets[size_t(KisBackingBudgetClass::ActivePending)].cpuRam = 4096;
    auto pendingReservation = budget.reserve(pendingBytes, &error);
    QVERIFY2(pendingReservation.isValid(), qPrintable(error));
    QVERIFY(owner.registerBacking(pending, pendingReservation, KisBackingBudgetClass::ActivePending, &error));

    QVector<KisBackingClassChange> publication{
        {current, KisBackingBudgetClass::Current, KisBackingBudgetClass::RetainedHistory},
        {pending, KisBackingBudgetClass::ActivePending, KisBackingBudgetClass::Current}};
    QVector<KisBackingClassChange> duplicate{publication[0], publication[1], publication[0]};
    QVERIFY(!owner.prepareBackingChanges(duplicate, {}, &error).isValid());
    QVERIFY(error.contains(QStringLiteral("more than once")));
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::Current)].live.cpuRam, 4096u);
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::RetainedHistory)].reserved.cpuRam, 0u);
    auto publicationReservation = owner.prepareBackingChanges(
        std::move(publication), {}, &error);
    QVERIFY2(publicationReservation.isValid(), qPrintable(error));
    owner.commitBackingChanges(std::move(publicationReservation));
    const auto installed = budget.usage();
    QCOMPARE(installed.buckets[size_t(KisBackingBudgetClass::Current)].live.cpuRam, 4096u);
    QCOMPARE(installed.buckets[size_t(KisBackingBudgetClass::RetainedHistory)].live.cpuRam, 4096u);
    QCOMPARE(installed.buckets[size_t(KisBackingBudgetClass::ActivePending)].live.cpuRam, 0u);

    const auto metadataLive = [&] {
        return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam;
    };
    KisPageOwnerLedger::BackingChanges originalChanges{KisMutationStorageAllocator<KisBackingClassChange>(&budget)};
    originalChanges.reserve(4096);
    const auto inputBytes = originalChanges.capacity() * sizeof(KisBackingClassChange);
    originalChanges.push_back({pending, KisBackingBudgetClass::Current, KisBackingBudgetClass::OptionalCache});
    auto nativeReservation = owner.prepareBackingChanges(std::move(originalChanges), nullptr, 0, &error);
    QVERIFY2(nativeReservation.isValid(), qPrintable(error));
    QVERIFY(metadataLive() >= inputBytes); // A compact copy would lose this large original paid capacity.
    const auto fillerBytes = size_t(limits.metadataArenaBytes - metadataLive());
    void *filler = kisAllocateMutationStorage(&budget, fillerBytes, 1);
    {
        const auto freeFiller = qScopeGuard([&] { kisFreeMutationStorage(&budget, filler, fillerBytes, 1); });
        QCOMPARE(metadataLive(), limits.metadataArenaBytes);
        owner.commitBackingChanges(std::move(nativeReservation));
        QCOMPARE(owner.backingClass(current), KisBackingBudgetClass::RetainedHistory);
        QCOMPARE(owner.backingClass(pending), KisBackingBudgetClass::OptionalCache);
        QVERIFY(metadataLive() >= fillerBytes + inputBytes); // Headroom may free; the original array remains paid.
    }
    const auto withOriginalSlot = metadataLive();
    auto restoreClass = owner.prepareBackingChanges(
        {{pending, KisBackingBudgetClass::OptionalCache, KisBackingBudgetClass::Current}}, {}, &error);
    QVERIFY2(restoreClass.isValid(), qPrintable(error));
    owner.commitBackingChanges(std::move(restoreClass));
    QVERIFY(metadataLive() + inputBytes / 2 < withOriginalSlot); // Replacing the original slot frees its array.

    const QVector<KisPageTransitionEffect> retireCurrent{{current, {}}};
    const QVector<KisPageTransitionEffect> duplicatedRetirement{
        retireCurrent[0], retireCurrent[0]};
    QVector<KisBackingClassChange> mergedRetirement;
    auto mergedReservation = owner.prepareBackingChanges(
        std::move(mergedRetirement), duplicatedRetirement, &error);
    QVERIFY2(mergedReservation.isValid(), qPrintable(error));
    QCOMPARE(budget.usage()
                 .buckets[size_t(KisBackingBudgetClass::RetirementDebt)]
                 .reserved.cpuRam,
             4096u);
    mergedReservation.release();
    quint64 debtCookie = 0;
    QVERIFY2(owner.prepareRetirementDebt(duplicatedRetirement.constData(), duplicatedRetirement.size(), &debtCookie, &error),
             qPrintable(error));
    auto preparedDebt = budget.usage();
    QCOMPARE(preparedDebt.buckets[size_t(KisBackingBudgetClass::RetainedHistory)].live.cpuRam,
             4096u);
    QCOMPARE(preparedDebt.buckets[size_t(KisBackingBudgetClass::RetirementDebt)].reserved.cpuRam,
             4096u);
    owner.cancelRetirementDebt(debtCookie);
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::RetirementDebt)].reserved.cpuRam,
             0u);
    QVERIFY2(owner.prepareRetirementDebt(retireCurrent.constData(), retireCurrent.size(), &debtCookie, &error),
             qPrintable(error));
    owner.commitRetirementDebt(debtCookie);
    const auto committedDebt = budget.usage();
    QCOMPARE(committedDebt.buckets[size_t(KisBackingBudgetClass::RetainedHistory)].live.cpuRam,
             0u);
    QCOMPARE(committedDebt.buckets[size_t(KisBackingBudgetClass::RetirementDebt)].live.cpuRam,
             4096u);
    owner.releaseRetiredBacking(current);
    owner.releaseRetiredBacking(pending);
    QVERIFY(metadata.configure(4));
    QVERIFY(metadata.registerPage(initialPageState(pageVersion(3, 1),
                                                   replica(pageVersion(3, 1), 1, 1, 3))));
    const auto snapshotExportsBeforeAdmission = metadata.metrics().fullSnapshotExports;
    QVERIFY(!coordinator.reserveBacking(allocationDescriptor(), KisPageAccessDomain::CpuRam,
                                        KisBackingBudgetClass::ActivePending, &error,
                                        pageVersion(3, 2)).reservation.isValid());
    QVERIFY(error.contains(QStringLiteral("transient-version")));
    QCOMPARE(metadata.metrics().fullSnapshotExports, snapshotExportsBeforeAdmission);

    KisPageMetadataCoordinator boundedMetadata;
    QVERIFY(boundedMetadata.configure(4));
    auto boundedPage = initialPageState(pageVersion(4, 1), replica(pageVersion(4, 1), 1, 1, 4));
    const auto preparedVersion = pageVersion(4, 2);
    const auto preparedReplica = replica(preparedVersion, 1, 1, 5);
    boundedPage.versions.append({preparedVersion, KisPagePublicationState::Prepared,
                                 {{preparedReplica, KisReplicaValidity::Valid, {}, {}, 0, {}}},
                                 preparedReplica, {91}, {}});
    boundedPage.nextGeneration = {3};
    QVERIFY(boundedMetadata.registerPage(boundedPage));
    auto boundedLimits = limits;
    boundedLimits.maxTransientVersionsPerPage = 1;
    KisBackingBudgetController boundedBudget(boundedLimits);
    KisPageOwnerLedger boundedOwner;
    boundedOwner.attachBackingBudget(boundedBudget);
    KisPageWriteCoordinator boundedCoordinator(boundedMetadata, epochs, boundedBudget, boundedOwner);
    const auto boundedExports = boundedMetadata.metrics().fullSnapshotExports;
    QVERIFY(!boundedCoordinator.reserveBacking(allocationDescriptor(), KisPageAccessDomain::CpuRam,
                                               KisBackingBudgetClass::ActivePending, &error,
                                               pageVersion(4, 3)).reservation.isValid());
    QVERIFY(error.contains(QStringLiteral("transient-version")));
    QCOMPARE(boundedMetadata.metrics().fullSnapshotExports, boundedExports);
    auto existingTarget = boundedCoordinator.reserveBacking(allocationDescriptor(), KisPageAccessDomain::CpuRam,
                                                            KisBackingBudgetClass::ActivePending, &error,
                                                            preparedVersion);
    QVERIFY2(existingTarget.reservation.isValid(), qPrintable(error));
    existingTarget = {};
    QCOMPARE(boundedMetadata.metrics().fullSnapshotExports, boundedExports);

    KisPageDefaultStorage defaults(budget);
    auto firstDefault = defaults.readBuffer(surfaceEpochState());
    QVERIFY(firstDefault);
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::OptionalCache)].live.cpuRam, 4096u);
    QVERIFY(!defaults.readBuffer(surfaceEpochState({1}, 2)));
    defaults.clearReadCache();
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::OptionalCache)].live.cpuRam, 4096u);
    firstDefault.reset();
    QCOMPARE(budget.usage().buckets[size_t(KisBackingBudgetClass::OptionalCache)].live.cpuRam, 0u);
    QVERIFY(defaults.readBuffer(surfaceEpochState({1}, 2)));
}

void KisPageStoreReferenceTest::preparedMutationIsAtomicAndBound()
{
    const KisPageTransaction tx{KisPageTransactionId{89}, KisImageEpochId{1}};
    KisPageMetadataCoordinator coordinator, foreign;
    MetadataEffectReceiver receiver;
    attachEffects(coordinator, receiver);
    QVERIFY(coordinator.configure(4));
    QVERIFY(foreign.configure(4));
    QVector<KisPageVersion> changes;
    for (int x = 0; x < 2; ++x) {
        auto state = initialPageState(pageVersion(x, 1), replica(pageVersion(x, 1), 1, 1, 1));
        const auto authority = replica(pageVersion(x, 2), 1, 1, 2);
        state.versions.append({pageVersion(x, 2),
                               KisPagePublicationState::Prepared,
                               {{authority, KisReplicaValidity::Valid, {}, {}, 0, {}}},
                               authority,
                               tx.id});
        state.nextGeneration = {3};
        QVERIFY(coordinator.registerPage(state));
        QVERIFY(foreign.registerPage(state));
        changes.append(pageVersion(x, 2));
    }
    const auto make = [&] {
        return coordinator.prepareMutation(tx, changes.constData(), changes.size());
    };
    auto batch = make();
    QVERIFY(batch.isValid());
    QVERIFY(!foreign.installMutation(std::move(batch), tx));
    QVERIFY(!batch.isValid());
    batch = make();
    auto wrong = tx;
    ++wrong.id.value;
    QVERIFY(!coordinator.installMutation(std::move(batch), wrong));
    batch = make();
    wrong = tx;
    ++wrong.baseEpoch.value;
    QVERIFY(!coordinator.installMutation(std::move(batch), wrong));
    batch = make();
    QVERIFY(!coordinator.installPublication(std::move(batch), tx, {2}));
    auto invalid = changes;
    invalid.append(changes.first());
    QVERIFY(!coordinator.prepareMutation(tx, invalid.constData(), invalid.size()).isValid());
    // This private entry accepts only versions, so foreign transition kinds,
    // operation IDs and epoch tags are no longer representable inputs.
    QVERIFY(!coordinator.prepareMutation(tx, nullptr, 1).isValid());
    QVERIFY(!coordinator.prepareMutation(tx, changes.constData(), -1).isValid());
    wrong = tx;
    ++wrong.id.value;
    QVERIFY(!coordinator.prepareMutation(wrong, changes.constData(), changes.size()).isValid());
    invalid = changes;
    invalid.last().generation = {};
    QVERIFY(!coordinator.prepareMutation(tx, invalid.constData(), invalid.size()).isValid());
    QVector<KisPageTransition> publication;
    for (const auto &version : changes) {
        KisPageTransition t;
        t.kind = KisPageTransitionKind::CommitTransaction;
        t.version = version;
        t.transaction = tx.id;
        t.imageEpoch = {2};
        publication.append(t);
    }
    batch = coordinator.preparePublication(tx, {2}, publication);
    QVERIFY(batch.isValid());
    QVERIFY(!coordinator.installMutation(std::move(batch), tx));
    batch = make();
    KisPageTransition read;
    read.kind = KisPageTransitionKind::AcquireRead;
    read.version = pageVersion(1, 2);
    read.target = replica(read.version, 1, 1, 2);
    read.lease = {19};
    read.transaction = tx.id;
    QVERIFY(coordinator.applyOwner(read.version.key, read).accepted);
    KisPageTransition write;
    write.kind = KisPageTransitionKind::AcquireWrite;
    write.version = pageVersion(1, 3);
    write.baseVersion = read.version;
    write.source = read.target;
    write.target = replica(write.version, 1, 1, 3);
    write.transaction = tx.id;
    write.writer = {51};
    write.operation = {52};
    QVERIFY(coordinator.applyOwner(write.version.key, write).accepted);
    // A real semantic conflict on the second page rejects ALL edits. A mere
    // reader revision change is no longer a reason to discard the candidate.
    QVERIFY(!coordinator.installMutation(std::move(batch), tx));
    for (int x = 0; x < 2; ++x) {
        KisPageStateSnapshot state;
        QVERIFY(coordinator.pageSnapshot(pageKey(x), &state));
        QCOMPARE(state.versions.at(1).publication, KisPagePublicationState::Prepared);
    }
    write.kind = KisPageTransitionKind::CancelWrite;
    QVERIFY(coordinator.applyOwner(write.version.key, write).accepted);
    batch = make();
    QVERIFY(batch.isValid());
    QVERIFY(coordinator.installMutation(std::move(batch), tx));
    QVERIFY(!coordinator.installMutation(std::move(batch), tx));
    for (int x = 0; x < 2; ++x) {
        KisPageStateSnapshot state;
        QVERIFY(coordinator.pageSnapshot(pageKey(x), &state));
        QCOMPARE(state.publishedEpoch, KisImageEpochId{1});
        QCOMPARE(state.publishedGeneration, KisPageGeneration{1});
        QCOMPARE(state.versions.last().publication, KisPagePublicationState::Historical);
        QVERIFY(!state.versions.last().preparedBy.isValid());
        QVERIFY(KisPageStateMachine().validateInvariants(state));
    }
    read.kind = KisPageTransitionKind::ReleaseRead;
    QVERIFY(coordinator.applyOwner(read.version.key, read).accepted); // capability survived detachment
}

void KisPageStoreReferenceTest::preparedMutationPreservesLateProtection()
{
    QFETCH(int, protection);
    const KisPageTransaction tx{KisPageTransactionId{91}, KisImageEpochId{1}};
    const auto version = pageVersion(0, 2);
    const auto authority = replica(version, 1, 1, 2), other = replica(version, 2, 1, 3);
    auto initial = initialPageState(pageVersion(0, 1), replica(pageVersion(0, 1), 1, 1, 1));
    initial.versions.append(
        {version,
         KisPagePublicationState::Prepared,
         {{authority, KisReplicaValidity::Valid, {}, {}, 0, {}}, {other, KisReplicaValidity::Valid, {}, {}, 0, {}}},
         authority,
         tx.id,
         {}});
    initial.nextGeneration = {3};
    KisPageMetadataCoordinator coordinator;
    QVERIFY(coordinator.configure(4));
    QVERIFY(coordinator.registerPage(initial));
    KisPageTransition detach;
    detach.kind = KisPageTransitionKind::DetachPreparedVersion;
    detach.version = version;
    detach.transaction = tx.id;
    auto candidate = coordinator.prepareMutation(tx, &detach.version, 1);
    QVERIFY(candidate.isValid());
    KisCompletionRegistry completions;
    const KisCompletionDomain source = KisCompletionDomain::HostLogical;
    const auto completion = completions.allocatePending(completions.registerSource(source));
    QVERIFY(completion.isValid());
    KisPageTransition read;
    read.kind = KisPageTransitionKind::AcquireRead;
    read.version = version;
    read.target = authority;
    read.transaction = tx.id;
    read.lease = {19};
    KisPageTransition capture;
    capture.kind = KisPageTransitionKind::RetainCapturedVersion;
    capture.version = version;
    capture.transaction = tx.id;
    capture.readView = {21};
    KisPageTransition handoff;
    handoff.kind = KisPageTransitionKind::BeginAuthorityHandoff;
    handoff.version = version;
    handoff.source = authority;
    handoff.target = other;
    handoff.operation = {37};
    if (protection & 1)
        QVERIFY(coordinator.applyOwner(version.key, capture).accepted);
    if (protection & 2)
        QVERIFY(coordinator.applyOwner(version.key, read).accepted);
    if (protection & 4) {
        auto pending = read;
        pending.lease = {20};
        QVERIFY(coordinator.applyOwner(version.key, pending).accepted);
        pending.kind = KisPageTransitionKind::ReleaseRead;
        pending.completion = completion;
        QVERIFY(coordinator.applyOwner(version.key, pending).accepted);
    }
    if (protection & 8)
        QVERIFY(coordinator.applyOwner(version.key, handoff).accepted);
    KisPageStateSnapshot current;
    QVERIFY(coordinator.pageSnapshot(version.key, &current));
    const auto reference = KisPageStateMachine().apply(current, detach);
    QVERIFY(reference.accepted);
    QVERIFY(coordinator.installMutation(std::move(candidate), tx));
    KisPageStateSnapshot actual;
    QVERIFY(coordinator.pageSnapshot(version.key, &actual));
    QVERIFY(KisPageStateMachine().validateInvariants(actual));
    const auto &expected = reference.next;
    QCOMPARE(actual.key, expected.key);
    QCOMPARE(actual.publishedEpoch, expected.publishedEpoch);
    QCOMPARE(actual.publishedGeneration, expected.publishedGeneration);
    QCOMPARE(actual.publishedDefaultPixelRevision, expected.publishedDefaultPixelRevision);
    QCOMPARE(actual.nextGeneration, expected.nextGeneration);
    QCOMPARE(actual.versions.size(), expected.versions.size());
    QCOMPARE(actual.writer.isValid(), expected.writer.isValid());
    QCOMPARE(actual.authorityHandoff.operation, expected.authorityHandoff.operation);
    QCOMPARE(actual.authorityHandoff.source, expected.authorityHandoff.source);
    QCOMPARE(actual.authorityHandoff.target, expected.authorityHandoff.target);
    for (qsizetype i = 0; i < actual.versions.size(); ++i) {
        const auto &a = actual.versions.at(i), &b = expected.versions.at(i);
        QCOMPARE(a.version, b.version);
        QCOMPARE(a.publication, b.publication);
        QCOMPARE(a.preparedBy, b.preparedBy);
        QCOMPARE(a.authority, b.authority);
        QCOMPARE(a.capturedReadViews, b.capturedReadViews);
        QCOMPARE(a.replicas.size(), b.replicas.size());
        for (qsizetype r = 0; r < a.replicas.size(); ++r) {
            const auto &ar = a.replicas.at(r), &br = b.replicas.at(r);
            QCOMPARE(ar.replica, br.replica);
            QCOMPARE(ar.validity, br.validity);
            QCOMPARE(ar.activeOperation, br.activeOperation);
            QCOMPARE(ar.readLeases, br.readLeases);
            QCOMPARE(ar.pendingLastUses, br.pendingLastUses);
            QCOMPARE(ar.pinCount, br.pinCount);
        }
    }
    if (protection & 1) {
        capture.kind = KisPageTransitionKind::ReleaseCapturedVersion;
        QVERIFY(coordinator.applyOwner(version.key, capture).accepted);
    }
    if (protection & 2) {
        read.kind = KisPageTransitionKind::ReleaseRead;
        QVERIFY(coordinator.applyOwner(version.key, read).accepted);
    }
    if (protection & 4) {
        // Completion acknowledgement requires the registry-issued proof, not
        // a raw transition that could discard an in-flight last-use claim.
        QVERIFY(completions.complete(completion, KisCompletionStatus::Succeeded));
        QVERIFY(coordinator.acknowledgeLastUse(version, authority, completions.verifyTerminal(completion)).accepted);
    }
    if (protection & 8) {
        handoff.kind = KisPageTransitionKind::CommitAuthorityHandoff;
        QVERIFY(coordinator.applyOwner(version.key, handoff).accepted);
    }
}

void KisPageStoreReferenceTest::metadataDirectoryPublicationIsImmutableAndConcurrent()
{
    KisPageMetadataCoordinator coordinator;
    QCOMPARE(coordinator.shardCount(), qsizetype(0));
    QCOMPARE(coordinator.shardFor(pageKey(0)), qsizetype(-1));
    QVERIFY(coordinator.publicationHeads().empty());
    QVERIFY(!coordinator.configure(0));
    std::atomic<bool> start{false};
    std::atomic<int> failures{0};
    std::atomic<int> successfulConfigurations{0};
    std::vector<std::thread> threads;
    for (int w = 0; w < 4; ++w) {
        threads.emplace_back([&, w]() {
            while (!start.load())
                std::this_thread::yield();
            bool inspectedAuthority = false;
            for (int i = 0; i < 10000; ++i) {
                const bool ready = coordinator.isOperational();
                const auto count = coordinator.shardCount();
                if (count != 0 && count != 16)
                    ++failures;
                const auto shard = coordinator.shardFor(pageKey(i + w));
                if (shard < -1 || shard >= 16 || (ready && (count != 16 || shard < 0)))
                    ++failures;
                if (ready && !inspectedAuthority) {
                    if (!coordinator.publicationHeads().empty()) ++failures;
                    inspectedAuthority = true;
                }
                if (ready && coordinator.configure(8))
                    ++failures;
            }
        });
    }
    for (int w = 0; w < 2; ++w) {
        threads.emplace_back([&]() {
            while (!start.load())
                std::this_thread::yield();
            if (coordinator.configure(16))
                ++successfulConfigurations;
        });
    }
    start.store(true);
    for (auto &thread : threads)
        thread.join();
    QCOMPARE(successfulConfigurations.load(), 1);
    QCOMPARE(failures.load(), 0);
    QCOMPARE(coordinator.shardCount(), qsizetype(16));
    QVERIFY(coordinator.registerPage(initialPageState(pageVersion(0, 1), replica(pageVersion(0, 1), 1, 1, 1))));
    KisPageStateSnapshot state;
    QVERIFY(coordinator.pageSnapshot(pageKey(0), &state));
    QVERIFY(KisPageStateMachine().validateInvariants(state));
}

void KisPageStoreReferenceTest::imageEpochCommitIsAtomicAndDetectsConflicts()
{
    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = 1;
    initial.defaultPixelRevision = 1;
    initial.extentRevision = 1;
    initial.propertyRevision = 1;
    initial.manifest = {pageVersion(0, 1), pageVersion(1, 1)};

    KisImageEpochReferenceModel model;
    QString error;
    QVERIFY2(model.initialize(initial, &error), qPrintable(error));
    const KisImageEpochRootSnapshot original = model.captureCommittedRoot();
    QVERIFY(original.isValid());
    const KisRetainedImageEpochSnapshot retained = model.captureRetainedSnapshot();
    QVERIFY(retained.isValid());
    QVERIFY(retained.hasCompleteManifest());
    QCOMPARE(retained.retainedPageCount(), retained.snapshot.manifest.size());
    QVERIFY(model.validateRetainedSnapshot(retained));

    const KisRetainedImageEpochSnapshot retainedRoot = model.captureRetainedRoot();
    QVERIFY(retainedRoot.isValid());
    QVERIFY(!retainedRoot.hasCompleteManifest());
    QVERIFY(retainedRoot.snapshot.manifest.isEmpty());
    QCOMPARE(retainedRoot.retainedPageCount(), qsizetype(2));
    QVERIFY(model.validateRetainedSnapshot(retainedRoot));
    QVERIFY(model.releaseSnapshot(retainedRoot.token));

    KisImageEpochReferenceModel secondModel;
    QVERIFY2(secondModel.initialize(initial, &error), qPrintable(error));
    const KisRetainedImageEpochSnapshot secondRetained = secondModel.captureRetainedSnapshot();
    QVERIFY(secondRetained.isValid());
    QVERIFY(!(retained.token == secondRetained.token));
    QVERIFY(!model.validateRetainedSnapshot(secondRetained));
    const KisPageTransaction secondModelTransaction = secondModel.beginTransaction(initial.epoch, &error);
    QVERIFY(secondModelTransaction.isValid());
    QVERIFY(!model.transaction(secondModelTransaction.id).isValid());

    KisRetainedImageEpochSnapshot forgedRetained = retained;
    forgedRetained.snapshot.propertyRevision++;
    QVERIFY(!model.validateRetainedSnapshot(forgedRetained));
    QCOMPARE(model.retainedSnapshotCount(), qsizetype(1));

    KisCompletionRegistry proofCompletions;
    const KisCompletionDomain proofSourceDescriptor = KisCompletionDomain::HostLogical;
    const quint64 proofSource = proofCompletions.registerSource(proofSourceDescriptor);
    auto nextProofTicket = [&proofCompletions, proofSource]() {
        const KisCompletionTicket ticket = proofCompletions.allocatePending(proofSource);
        proofCompletions.complete(ticket, KisCompletionStatus::Succeeded);
        return ticket;
    };

    const KisPageTransaction primary = model.beginTransaction(KisImageEpochId{1}, &error);
    const KisPageTransaction overlap = model.beginTransaction(KisImageEpochId{1}, &error);
    const KisPageTransaction disjoint = model.beginTransaction(KisImageEpochId{1}, &error);
    QVERIFY(primary.isValid());
    QVERIFY(overlap.isValid());
    QVERIFY(disjoint.isValid());

    KisPreparedPageSet primaryPages;
    primaryPages.transaction = primary.id;
    primaryPages.proofs = {preparedProof(pageVersion(0, 2), primary.id, nextProofTicket(), 1),
                           preparedProof(pageVersion(1, 2), primary.id, nextProofTicket(), 2)};
    QVERIFY2(model.prepare(primaryPages, &error), qPrintable(error));
    QVERIFY(!model.prepare(primaryPages, &error)); // Public preparation still means an advancing incremental delta.

    KisPageReadView overlayView;
    overlayView.kind = KisPageReadViewKind::TransactionOverlay;
    overlayView.transaction = primary.id;
    KisPageVersion resolved;
    QVERIFY(model.resolve(pageKey(0), overlayView, &resolved));
    QCOMPARE(resolved.generation.value, quint64(2));

    KisPageReadView currentView;
    QVERIFY(model.resolve(pageKey(0), currentView, &resolved));
    QCOMPARE(resolved.generation.value, quint64(1));

    KisPreparedPageSet overlapPages;
    overlapPages.transaction = overlap.id;
    overlapPages.proofs = {preparedProof(pageVersion(0, 3), overlap.id, nextProofTicket(), 3)};
    QVERIFY2(model.prepare(overlapPages, &error), qPrintable(error));

    KisPreparedPageSet disjointPages;
    disjointPages.transaction = disjoint.id;
    disjointPages.proofs = {preparedProof(pageVersion(2, 1), disjoint.id, nextProofTicket(), 4)};
    QVERIFY2(model.prepare(disjointPages, &error), qPrintable(error));

    const KisImageEpochCommitResult primaryCommit = model.commit(primary);
    QVERIFY2(primaryCommit.isCommitted(), qPrintable(primaryCommit.error));
    QCOMPARE(primaryCommit.root.previousEpoch().value, quint64(1));
    QCOMPARE(primaryCommit.root.commitSequence(), quint64(2));
    QVERIFY(primaryCommit.root.resolve(pageKey(0), &resolved));
    QCOMPARE(resolved.generation.value, quint64(2));
    QVERIFY(primaryCommit.root.resolve(pageKey(1), &resolved));
    QCOMPARE(resolved.generation.value, quint64(2));

    QVERIFY(original.resolve(pageKey(0), &resolved));
    QCOMPARE(resolved.generation.value, quint64(1));
    QVERIFY(original.resolve(pageKey(1), &resolved));
    QCOMPARE(resolved.generation.value, quint64(1));

    const KisImageEpochCommitResult conflict = model.commit(overlap);
    QCOMPARE(conflict.status, KisImageEpochCommitStatus::Conflict);
    QVERIFY(!conflict.error.isEmpty());
    QVERIFY(model.abort(overlap, &error));

    const KisImageEpochCommitResult rebased = model.commit(disjoint);
    QVERIFY2(rebased.isCommitted(), qPrintable(rebased.error));
    QCOMPARE(rebased.root.previousEpoch().value, primaryCommit.root.epoch().value);
    QCOMPARE(rebased.root.commitSequence(), quint64(3));
    QVERIFY(rebased.root.resolve(pageKey(0), &resolved));
    QCOMPARE(resolved.generation.value, quint64(2));
    QVERIFY(rebased.root.resolve(pageKey(2), &resolved));
    QCOMPARE(resolved.generation.value, quint64(1));

    KisPageReadView historicalView;
    historicalView.kind = KisPageReadViewKind::CommittedEpoch;
    historicalView.epoch = KisImageEpochId{1};
    historicalView.retention = retained.token;
    QVERIFY(model.resolve(pageKey(0), historicalView, &resolved));
    QCOMPARE(resolved.generation.value, quint64(1));
    KisPageReadView exactHistorical;
    exactHistorical.kind = KisPageReadViewKind::ExactVersion;
    exactHistorical.epoch = KisImageEpochId{1};
    exactHistorical.retention = retained.token;
    exactHistorical.exactVersion = pageVersion(0, 1);
    QVERIFY(model.resolve(pageKey(0), exactHistorical, &resolved));
    exactHistorical.exactVersion = pageVersion(0, 2);
    QVERIFY(!model.resolve(pageKey(0), exactHistorical, &resolved));

    QCOMPARE(model.collectUnretainedRoots(), qsizetype(1));
    QVERIFY(model.root(KisImageEpochId{1}).isValid());
    QVERIFY(model.releaseSnapshot(retained.token, &error));
    QVERIFY(!model.validateRetainedSnapshot(retained));
    QVERIFY(!model.releaseSnapshot(retained.token, &error));
    QCOMPARE(model.retainedSnapshotCount(), qsizetype(0));
    QCOMPARE(model.collectUnretainedRoots(), qsizetype(1));
    QVERIFY(!model.root(KisImageEpochId{1}).isValid());
}

void KisPageStoreReferenceTest::imageEpochRootUsesBalancedPersistentPageDelta()
{
    constexpr qint32 pageCount = 32768;
    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = 1;
    initial.defaultPixelRevision = 1;
    initial.extentRevision = 1;
    initial.propertyRevision = 1;
    initial.surfaces = {surfaceEpochState()};
    initial.manifest.reserve(pageCount);
    for (qint32 column = 0; column < pageCount; ++column) {
        initial.manifest.append(pageVersion(column, 1));
    }

    KisImageEpochReferenceModel model;
    QString error;
    QVERIFY2(model.initialize(initial, &error), qPrintable(error));
    const KisImageEpochRootSnapshot before = model.captureCommittedRoot();
    QCOMPARE(before.pageCount(), qsizetype(pageCount));
    QVERIFY(before.pageTreeHeight() <= 16);

    const KisPageTransaction transaction = model.beginTransaction(before.epoch(), &error);
    QVERIFY2(transaction.isValid(), qPrintable(error));

    KisCompletionRegistry completions;
    const KisCompletionDomain sourceDescriptor = KisCompletionDomain::HostLogical;
    const quint64 source = completions.registerSource(sourceDescriptor);
    const KisCompletionTicket completion = completions.allocatePending(source);
    QVERIFY(completions.complete(completion, KisCompletionStatus::Succeeded));

    KisPreparedPageSet prepared;
    prepared.transaction = transaction.id;
    prepared.proofs = {preparedProof(pageVersion(pageCount / 2, 2), transaction.id, completion, 1)};
    QVERIFY2(model.prepare(prepared, &error), qPrintable(error));
    const KisImageEpochCommitResult committed = model.commit(transaction);
    QVERIFY2(committed.isCommitted(), qPrintable(committed.error));
    QCOMPARE(committed.root.pageCount(), qsizetype(pageCount));
    QVERIFY(committed.root.pageTreeHeight() <= 17);

    KisPageVersion version;
    QVERIFY(before.containsPage(pageKey(pageCount / 2), &version));
    QCOMPARE(version.generation.value, quint64(1));
    QVERIFY(committed.root.containsPage(pageKey(pageCount / 2), &version));
    QCOMPARE(version.generation.value, quint64(2));

    const QVector<KisPageVersion> exported = committed.root.manifest();
    QCOMPARE(exported.size(), qsizetype(pageCount));
    QCOMPARE(exported.first().key.page.column, qint32(0));
    QCOMPARE(exported.last().key.page.column, pageCount - 1);
}

void KisPageStoreReferenceTest::materializationOracleAndPublicationGuards()
{
    const auto version = pageVersion(0, 1);
    const auto source = replica(version, 1, 1, 1);
    auto page = initialPageState(version, source);
    const KisPageStateMachine machine;
    auto apply = [&](const KisPageTransition &transition) {
        auto result = machine.apply(page, transition);
        if (result.accepted) page = std::move(result.next);
        return result.accepted;
    };
    KisPageTransition materialize;
    materialize.kind = KisPageTransitionKind::BeginMaterialize;
    materialize.version = version;
    materialize.source = source;
    materialize.target = replica(version, 2, 3, 2);
    materialize.operation = KisPageOperationId{10};
    QVERIFY(apply(materialize));
    QVERIFY(!apply(materialize));
    QCOMPARE(page.versions.first().replicas.first().pinCount, quint32(1));
    QCOMPARE(page.versions.first().replicas.last().validity, KisReplicaValidity::Materializing);

    // The shared state machine still defines async materialization semantics.
    // Production metadata must not bypass verified completion ownership, nor
    // retire a source protected by an actual in-flight replica/pin.
    KisPageMetadataCoordinator coordinator;
    QVERIFY(coordinator.configure(8));
    const KisPageTransaction transaction{KisPageTransactionId{72}, KisImageEpochId{1}};
    auto pending = page;
    const auto target = replica(pageVersion(0, 2), 1, 1, 9);
    pending.versions.append({target.version, KisPagePublicationState::Prepared,
                            {{target, KisReplicaValidity::Valid, {}, {}, 0, {}}}, target, transaction.id, {}});
    pending.nextGeneration = KisPageGeneration{3};
    QVERIFY(coordinator.registerPage(pending));
    KisPageTransition publish;
    publish.kind = KisPageTransitionKind::CommitTransaction;
    publish.transaction = transaction.id;
    publish.version = target.version;
    publish.imageEpoch = KisImageEpochId{2};
    auto publication = coordinator.preparePublication(transaction, publish.imageEpoch, {publish});
    QVERIFY(publication.isValid());
    QVERIFY(coordinator.installPublication(std::move(publication), transaction, publish.imageEpoch));
    KisPageStateSnapshot published;
    QVERIFY(coordinator.pageSnapshot(version.key, &published));
    QCOMPARE(published.versions.first().replicas.first().pinCount, quint32(1));
    QCOMPARE(published.versions.first().replicas.last().validity, KisReplicaValidity::Materializing);
    materialize.kind = KisPageTransitionKind::CompleteMaterialize;
    QVERIFY(!coordinator.applyOwner(version.key, materialize).accepted);
    auto stale = materialize;
    stale.operation = KisPageOperationId{99};
    QVERIFY(!apply(stale));
    stale = materialize;
    ++stale.target.providerEpoch.value;
    QVERIFY(!apply(stale));
    QVERIFY(apply(materialize));
    QVERIFY(!apply(materialize));
    QCOMPARE(page.versions.first().replicas.first().pinCount, quint32(0));
    QCOMPARE(page.versions.first().replicas.last().validity, KisReplicaValidity::Valid);

    materialize.kind = KisPageTransitionKind::BeginMaterialize;
    materialize.operation = KisPageOperationId{20};
    materialize.target = replica(version, 2, 3, 3);
    QVERIFY(apply(materialize));
    materialize.kind = KisPageTransitionKind::FailMaterialize;
    QVERIFY(apply(materialize));
    QVERIFY(!apply(materialize));
    QCOMPARE(page.versions.first().replicas.size(), 3);
    QCOMPARE(page.versions.first().replicas.first().pinCount, quint32(0));
    QCOMPARE(page.versions.first().replicas.last().validity, KisReplicaValidity::Failed);
    QVERIFY(KisPageStateMachine().validateInvariants(page));
}

void KisPageStoreReferenceTest::fakeCpuProviderSupportsReorderingAndFailure()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    FakeCpuReplicaProvider provider(completions);
    QVERIFY(provider.capabilities().isValid());

    const KisReplicaOperation first = provider.requestReplica(KisPageOperationId{1},
                                                              pageVersion(0, 1),
                                                              allocationDescriptor(),
                                                              KisPageAccessDomain::CpuRam,
                                                              KisPageAccessMode::Read,
                                                              KisPagePriority::Normal);
    const KisReplicaOperation second = provider.requestReplica(KisPageOperationId{2},
                                                               pageVersion(1, 1),
                                                               allocationDescriptor(),
                                                               KisPageAccessDomain::CpuRam,
                                                               KisPageAccessMode::Read,
                                                               KisPagePriority::Normal);
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());

    QVERIFY(provider.complete(KisPageOperationId{2}, true));
    QVERIFY(provider.complete(KisPageOperationId{1}, false));
    QCOMPARE(completions->status(second.completion), KisCompletionStatus::Succeeded);
    QCOMPARE(completions->status(first.completion), KisCompletionStatus::Failed);
    QVERIFY(provider.validate(second.replica, allocationDescriptor()));
    QVERIFY(!provider.validate(first.replica, allocationDescriptor()));

    KisReplicaAccess access = provider.resolveAccess(KisPageLeaseId{9},
                                                     KisPageOperationId{90},
                                                     second.replica,
                                                     {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                                                     KisPageAccessMode::Read);
    QVERIFY(access.isValid());
    provider.releaseAccess(std::move(access), {});
    QVERIFY(!access.isValid());
    QCOMPARE(provider.releaseCount(), quint64(1));

    KisReplicaAccess writableSource =
        provider.resolveAccess(KisPageLeaseId{10},
                               KisPageOperationId{91},
                               second.replica,
                               {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                               KisPageAccessMode::Write);
    QVERIFY(writableSource.isValid());
    static_cast<quint8 *>(writableSource.cpuWriteData)[0] = 42;
    provider.releaseAccess(std::move(writableSource), {});
    QVERIFY(!writableSource.isValid());

    const KisReplicaOperation writeTarget = provider.prepareWrite(KisPageOperationId{30},
                                                                  {second.replica.version.key, KisPageGeneration{2}},
                                                                  allocationDescriptor(),
                                                                  KisPageAccessDomain::CpuRam,
                                                                  KisPageWriteMode::PreserveContents,
                                                                  KisPagePriority::Normal);
    QVERIFY(writeTarget.isValid());
    QVERIFY(provider.complete(KisPageOperationId{30}, true));

    KisReplicaTransferRequest transferRequest;
    transferRequest.operation = KisPageOperationId{31};
    transferRequest.source = second.replica;
    transferRequest.target = writeTarget.replica;
    transferRequest.descriptor = allocationDescriptor();
    transferRequest.kind = KisReplicaTransferKind::WriteGenerationInitialization;
    const KisReplicaOperation transfer = provider.transfer(transferRequest, KisPagePriority::Normal);
    QVERIFY(transfer.isValid());
    QVERIFY(provider.complete(KisPageOperationId{31}, true));

    KisReplicaAccess copied = provider.resolveAccess(KisPageLeaseId{11},
                                                     KisPageOperationId{92},
                                                     writeTarget.replica,
                                                     {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                                                     KisPageAccessMode::Read);
    QVERIFY(copied.isValid());
    QCOMPARE(static_cast<const quint8 *>(copied.cpuReadData)[0], quint8(42));
    provider.releaseAccess(std::move(copied), {});
    QVERIFY(!copied.isValid());

    KisReplicaAccess retirementBlocker =
        provider.resolveAccess(KisPageLeaseId{12},
                               KisPageOperationId{93},
                               writeTarget.replica,
                               {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                               KisPageAccessMode::Read);
    QVERIFY(retirementBlocker.isValid());
    const KisReplicaOperation blockedRetirement = provider.retire(KisPageOperationId{39}, writeTarget.replica, {});
    QCOMPARE(blockedRetirement.status, KisPageRequestStatus::Failed);
    provider.releaseAccess(std::move(retirementBlocker), {});
    QVERIFY(!retirementBlocker.isValid());

    provider.injectNext(FakeFailurePoint::Transfer);
    transferRequest.operation = KisPageOperationId{32};
    const KisReplicaOperation injectedTransfer = provider.transfer(transferRequest, KisPagePriority::Normal);
    QCOMPARE(injectedTransfer.status, KisPageRequestStatus::Failed);
    QCOMPARE(injectedTransfer.operation.value, quint64(32));

    provider.injectNext(FakeFailurePoint::Validate);
    QVERIFY(!provider.validate(second.replica, allocationDescriptor()));
    QVERIFY(provider.validate(second.replica, allocationDescriptor()));

    const KisReplicaOperation failedRetirement = provider.retire(KisPageOperationId{3}, second.replica, {});
    QVERIFY(failedRetirement.isValid());
    QVERIFY(provider.complete(KisPageOperationId{3}, false));
    QVERIFY(provider.validate(second.replica, allocationDescriptor()));

    const KisReplicaOperation retirement = provider.retire(KisPageOperationId{4}, second.replica, {});
    QVERIFY(retirement.isValid());
    QVERIFY(provider.complete(KisPageOperationId{4}, true));
    QVERIFY(!provider.validate(second.replica, allocationDescriptor()));

    provider.injectNext(FakeFailurePoint::Retire);
    const KisReplicaOperation injectedRetirement = provider.retire(KisPageOperationId{40}, writeTarget.replica, {});
    QCOMPARE(injectedRetirement.status, KisPageRequestStatus::Failed);
    QVERIFY(provider.validate(writeTarget.replica, allocationDescriptor()));

    provider.injectNext(FakeFailurePoint::Allocate);
    const KisReplicaOperation injected = provider.requestReplica(KisPageOperationId{5},
                                                                 pageVersion(2, 1),
                                                                 allocationDescriptor(),
                                                                 KisPageAccessDomain::CpuRam,
                                                                 KisPageAccessMode::Read,
                                                                 KisPagePriority::Normal);
    QCOMPARE(injected.status, KisPageRequestStatus::Failed);
    QCOMPARE(injected.operation.value, quint64(5));

    const KisReplicaOperation stale = provider.requestReplica(KisPageOperationId{6},
                                                              pageVersion(3, 1),
                                                              allocationDescriptor(),
                                                              KisPageAccessDomain::CpuRam,
                                                              KisPageAccessMode::Read,
                                                              KisPagePriority::Normal);
    QVERIFY(stale.isValid());
    provider.restart();
    QVERIFY(!provider.complete(KisPageOperationId{6}, true));
    QCOMPARE(completions->status(stale.completion), KisCompletionStatus::Cancelled);
    QVERIFY(!provider.validate(stale.replica, allocationDescriptor()));
    QCOMPARE(provider.providerEpoch().value, quint64(2));

    QVector<KisReplicaOperation> stress;
    stress.reserve(128);
    for (quint64 i = 0; i < 128; ++i) {
        const KisReplicaOperation operation = provider.requestReplica(KisPageOperationId{100 + i},
                                                                      pageVersion(qint32(100 + i), 1),
                                                                      allocationDescriptor(),
                                                                      KisPageAccessDomain::CpuRam,
                                                                      KisPageAccessMode::Read,
                                                                      KisPagePriority::Background);
        QVERIFY(operation.isValid());
        stress.append(operation);
    }
    for (qsizetype i = stress.size(); i > 0; --i) {
        const bool succeed = ((i - 1) % 5) != 0;
        const KisReplicaOperation &operation = stress.at(i - 1);
        QVERIFY(provider.complete(operation.operation, succeed));
        QCOMPARE(completions->status(operation.completion),
                 succeed ? KisCompletionStatus::Succeeded : KisCompletionStatus::Failed);
        QCOMPARE(provider.validate(operation.replica, allocationDescriptor()), succeed);
    }
}

void KisPageStoreReferenceTest::productionCpuProviderPreservesFormatDefaultAndCowBytes()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    KisCpuPageReplicaProvider provider;
    KisCpuResidentReplicaProviderConfig config;
    config.provider = KisReplicaProviderId{41};
    config.providerEpoch = KisReplicaProviderEpoch{3};
    config.budgetBytes = 1024 * 1024;
    QString error;
    QVERIFY2(provider.configure(config, completions, &error), qPrintable(error));
    QVERIFY(provider.capabilities().synchronousOperations);
    QVERIFY(provider.capabilities().supports({KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer}));

    KisPageAllocationDescriptor descriptor = allocationDescriptor();
    descriptor.layoutRevision = 7;
    descriptor.format.formatId = 77;
    descriptor.format.colorModelId = "RGBA";
    descriptor.format.channelOrder = "RGBA";
    descriptor.format.defaultPixel = QByteArray::fromHex("11223344");
    descriptor.format.channelCount = 4;
    descriptor.format.pixelStride = 4;
    descriptor.format.pixelAlignment = 4;
    descriptor.format.alphaSemantic = KisSurfaceAlphaSemantic::Premultiplied;
    descriptor.pageExtent = QSize(3, 2);
    descriptor.validRect = QRect(0, 0, 3, 2);
    descriptor.rowAlignment = 64;
    descriptor.initialization = KisPageInitialization::DefaultPixel;
    QVERIFY(descriptor.isValid());

    const KisReplicaOperation source = provider.requestReplica(KisPageOperationId{1000},
                                                               pageVersion(20, 1),
                                                               descriptor,
                                                               KisPageAccessDomain::CpuRam,
                                                               KisPageAccessMode::Read,
                                                               KisPagePriority::Normal);
    QVERIFY(source.isValid());
    QCOMPARE(source.status, KisPageRequestStatus::Ready);
    QCOMPARE(source.replica.layout.rowStride, quint32(64));
    QCOMPARE(source.replica.layout.byteSize, quint64(128));
    QCOMPARE(completions->status(source.completion), KisCompletionStatus::Succeeded);
    QVERIFY(provider.validate(source.replica, descriptor));

    KisReplicaAccess sourceAccess = provider.resolveAccess(KisPageLeaseId{1000},
                                                           KisPageOperationId{1001},
                                                           source.replica,
                                                           {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                                                           KisPageAccessMode::Write);
    QVERIFY(sourceAccess.isValid());
    const QByteArray expectedDefault = QByteArray::fromHex("11223344");
    QCOMPARE(QByteArray(static_cast<const char *>(sourceAccess.cpuWriteData), 4), expectedDefault);
    QCOMPARE(QByteArray(static_cast<const char *>(sourceAccess.cpuWriteData) + 64, 4), expectedDefault);
    static_cast<quint8 *>(sourceAccess.cpuWriteData)[0] = 0x9a;
    provider.releaseAccess(std::move(sourceAccess), {});

    const KisPageVersion targetVersion{source.replica.version.key, KisPageGeneration{2}};
    const KisReplicaOperation target = provider.prepareWrite(KisPageOperationId{1002},
                                                             targetVersion,
                                                             descriptor,
                                                             KisPageAccessDomain::CpuRam,
                                                             KisPageWriteMode::PreserveContents,
                                                             KisPagePriority::Normal);
    QVERIFY(target.isValid());

    KisReplicaTransferRequest transferRequest;
    transferRequest.operation = KisPageOperationId{1003};
    transferRequest.source = source.replica;
    transferRequest.target = target.replica;
    transferRequest.descriptor = descriptor;
    transferRequest.kind = KisReplicaTransferKind::WriteGenerationInitialization;
    const KisReplicaOperation transfer = provider.transfer(transferRequest, KisPagePriority::Normal);
    QVERIFY(transfer.isValid());
    QCOMPARE(completions->status(transfer.completion), KisCompletionStatus::Succeeded);

    KisReplicaAccess copied = provider.resolveAccess(KisPageLeaseId{1001},
                                                     KisPageOperationId{1004},
                                                     target.replica,
                                                     {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                                                     KisPageAccessMode::Read);
    QVERIFY(copied.isValid());
    QCOMPARE(static_cast<const quint8 *>(copied.cpuReadData)[0], quint8(0x9a));
    provider.releaseAccess(std::move(copied), {});

    const KisReplicaMemoryUsage beforeRetire = provider.memoryUsage();
    QCOMPARE(beforeRetire.committedBytes, quint64(256));
    const KisReplicaOperation retired = provider.retire(KisPageOperationId{1005}, source.replica, {});
    QVERIFY(retired.isValid());
    QVERIFY(!provider.validate(source.replica, descriptor));
    QCOMPARE(provider.memoryUsage().committedBytes, quint64(128));

    KisPageAllocationDescriptor tooLarge = descriptor;
    tooLarge.pageExtent = QSize(4096, 4096);
    tooLarge.validRect = QRect(QPoint(0, 0), tooLarge.pageExtent);
    const KisReplicaOperation exhausted = provider.requestReplica(KisPageOperationId{1006},
                                                                  pageVersion(21, 1),
                                                                  tooLarge,
                                                                  KisPageAccessDomain::CpuRam,
                                                                  KisPageAccessMode::Read,
                                                                  KisPagePriority::Background);
    QCOMPARE(exhausted.status, KisPageRequestStatus::Failed);
}

void KisPageStoreReferenceTest::tiles3ProviderUsesExistingTileStoreBehindLeases()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<KisTiles3PageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig providerConfig;
    providerConfig.provider = KisReplicaProviderId{81};
    providerConfig.providerEpoch = KisReplicaProviderEpoch{1};
    providerConfig.budgetBytes = 4 * 1024 * 1024;
    QString error;
    QVERIFY2(provider->configure(providerConfig, completions, &error), qPrintable(error));

    KisSurfaceEpochState surface = surfaceEpochState();
    surface.format.formatId = 81;
    surface.format.colorModelId = QByteArrayLiteral("RGBA");
    surface.format.colorDepthId = QByteArrayLiteral("U8");
    surface.format.profileFingerprint = QByteArrayLiteral("tiles3-provider");
    surface.format.channelOrder = QByteArrayLiteral("BGRA");
    surface.format.defaultPixel = QByteArray::fromHex("11223344");
    surface.format.channelCount = 4;
    surface.format.pixelStride = 4;
    surface.format.pixelAlignment = 4;
    surface.format.alphaSemantic = KisSurfaceAlphaSemantic::Premultiplied;
    surface.rowAlignment = 1;
    QVERIFY(surface.isValid());

    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = 1;
    initial.defaultPixelRevision = 1;
    initial.extentRevision = 1;
    initial.propertyRevision = 1;
    initial.surfaces = {surface};

    KisPageStore store;
    QVERIFY2(store.configure(initial, completions, 8, &error), qPrintable(error));
    QVERIFY(store.registerReplicaProvider(provider));
    QVERIFY2(store.finalizeInitialization(&error), qPrintable(error));

    const KisPageAccessRequirement cpuAccess{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
    const KisPageKey key = pageKey(-2, 3);
    KisReadRequest readRequest = store.acquireRead(key, {}, cpuAccess, KisPagePriority::Normal);
    QVERIFY2(readRequest.isValid(), qPrintable(readRequest.error));
    KisReadLease read = store.resolve(readRequest, readRequest.readiness);
    QVERIFY(read.isValid());
    KisTileData *readTile = provider->tileDataForLease(read.leaseId());
    QVERIFY(readTile);
    QCOMPARE(readTile->pixelSize(), quint32(4));
    QCOMPARE(QByteArray(static_cast<const char *>(read.cpuData()), 4), surface.format.defaultPixel);
    store.release(std::move(read));

    const KisPageTransaction transaction = store.beginTransaction(initial.epoch);
    QVERIFY(transaction.isValid());
    const KisWriteRequest writeRequest = store.acquireWrite(transaction,
                                                            key,
                                                            cpuAccess,
                                                            KisPageWriteMode::PreserveContents,
                                                            KisPagePriority::Interactive);
    QVERIFY2(writeRequest.isValid(), qPrintable(writeRequest.error));
    KisWriteLease write = store.resolve(writeRequest, writeRequest.readiness);
    QVERIFY(write.isValid());
    KisTileData *writeTile = provider->tileDataForLease(write.leaseId());
    QVERIFY(writeTile);
    QVERIFY(writeTile != readTile);
    QCOMPARE(static_cast<quint8 *>(write.cpuData())[0], quint8(0x11));
    static_cast<quint8 *>(write.cpuData())[0] = 0x9a;
    QVERIFY(store.publishHostWrite(std::move(write)).isValid());
    QVERIFY(store.commit(transaction, store.preparedPages(transaction)).isValid());

    readRequest = store.acquireRead(key, {}, cpuAccess, KisPagePriority::Normal);
    KisReadLease readAfter = store.resolve(readRequest, readRequest.readiness);
    QVERIFY(readAfter.isValid());
    QCOMPARE(static_cast<const quint8 *>(readAfter.cpuData())[0], quint8(0x9a));
    QCOMPARE(provider->tileDataForLease(readAfter.leaseId()), writeTile);
    store.release(std::move(readAfter));

    QVERIFY2(store.closeSession(&error), qPrintable(error));
    QCOMPARE(provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreReferenceTest::tiles3NativeCopyPreservesSourceAndRejectsInvalidWork()
{
    const KisPageAccessRequirement cpu{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
    for (int bpp : {4, 8, 16}) {
        auto completions = std::make_shared<KisCompletionRegistry>();
        KisTiles3PageReplicaProvider provider;
        auto descriptor = allocationDescriptor();
        descriptor.format.pixelStride = bpp;
        descriptor.format.pixelAlignment = bpp;
        descriptor.format.defaultPixel = QByteArray(bpp, char(0x31));
        QVERIFY(descriptor.isValid());
        const quint64 bytes = descriptor.minimumByteSize();
        QVERIFY(provider.configure({{81}, {1}, 2 * bytes}, completions));
        quint64 id = 1000;
        const auto source = provider.requestReplica({id++},
                                                    pageVersion(1, 1),
                                                    descriptor,
                                                    cpu.domain,
                                                    KisPageAccessMode::Read,
                                                    KisPagePriority::Normal);
        QVERIFY(source.isValid());
        const KisPageLeaseId writeId{id++};
        auto write = provider.resolveAccess(writeId, {id++}, source.replica, cpu, KisPageAccessMode::Write);
        QVERIFY(write.isValid());
        static_cast<char *>(write.cpuWriteData)[0] = char(0x64);
        QVERIFY(!provider
                     .prepareSynchronousWriteCopy({id++},
                                                  source.replica,
                                                  pageVersion(1, 2),
                                                  descriptor,
                                                  KisPagePriority::Normal)
                     .isValid());
        provider.releaseAccess(std::move(write), {});
        const KisPageLeaseId readId{id++};
        auto read = provider.resolveAccess(readId, {id++}, source.replica, cpu, KisPageAccessMode::Read);
        QVERIFY(read.isValid());
        const KisPageOperationId copyId{id++};
        const auto copy = provider.prepareSynchronousWriteCopy(copyId,
                                                               source.replica,
                                                               pageVersion(1, 2),
                                                               descriptor,
                                                               KisPagePriority::Normal);
        QVERIFY2(copy.isValid(), qPrintable(copy.error));
        QCOMPARE(copy.status, KisPageRequestStatus::Ready);
        QCOMPARE(completions->status(copy.completion), KisCompletionStatus::Succeeded);
        QVERIFY(!(copy.replica.allocation == source.replica.allocation));
        const KisPageLeaseId targetId{id++};
        auto target = provider.resolveAccess(targetId, {id++}, copy.replica, cpu, KisPageAccessMode::Write);
        QVERIFY(target.isValid());
        QVERIFY(target.cpuWriteData != read.cpuReadData);
        QCOMPARE(QByteArray(static_cast<const char *>(target.cpuWriteData), qsizetype(bytes)),
                 QByteArray(static_cast<const char *>(read.cpuReadData), qsizetype(bytes)));
        static_cast<char *>(target.cpuWriteData)[0] = char(0x72);
        QCOMPARE(static_cast<const char *>(read.cpuReadData)[0], char(0x64));
        QVERIFY(!provider.retire({id++}, copy.replica, {}).isValid());
        provider.releaseAccess(std::move(target), {});
        QVERIFY(!provider
                     .prepareSynchronousWriteCopy(copyId,
                                                  source.replica,
                                                  pageVersion(1, 2),
                                                  descriptor,
                                                  KisPagePriority::Normal)
                     .isValid());
        // A full budget must reject before creating or initializing a payload.
        QVERIFY(!provider
                     .prepareSynchronousWriteCopy({id++},
                                                  source.replica,
                                                  pageVersion(1, 3),
                                                  descriptor,
                                                  KisPagePriority::Normal)
                     .isValid());
        auto stale = source.replica;
        ++stale.providerEpoch.value;
        QVERIFY(
            !provider.prepareSynchronousWriteCopy({id++}, stale, pageVersion(1, 3), descriptor, KisPagePriority::Normal)
                 .isValid());
        QVERIFY(!provider
                     .prepareSynchronousWriteCopy({id++},
                                                  source.replica,
                                                  pageVersion(2, 3),
                                                  descriptor,
                                                  KisPagePriority::Normal)
                     .isValid());
        QVERIFY(!provider
                     .prepareSynchronousWriteCopy({id++},
                                                  source.replica,
                                                  pageVersion(1, 1),
                                                  descriptor,
                                                  KisPagePriority::Normal)
                     .isValid());
        auto mismatch = descriptor;
        ++mismatch.layoutRevision;
        QVERIFY(!provider
                     .prepareSynchronousWriteCopy({id++},
                                                  source.replica,
                                                  pageVersion(1, 3),
                                                  mismatch,
                                                  KisPagePriority::Normal)
                     .isValid());
        const auto work = provider.payloadWork();
        QCOMPARE(work.defaultInitializedPages, quint64(1));
        QCOMPARE(work.defaultInitializedBytes, bytes);
        QCOMPARE(work.explicitCopyPages, quint64(0));
        QCOMPARE(work.nativeDuplicatePages, quint64(1));
        QCOMPARE(work.nativeDuplicateBytes, bytes);
        QCOMPARE(work.nativeForegroundCopyBytes, bytes * (1 - work.nativePrecloneHits));
        provider.releaseAccess(std::move(read), {});
        QVERIFY(provider.retire({id++}, copy.replica, {}).isValid());
        QVERIFY(provider.retire({id++}, source.replica, {}).isValid());
        QCOMPARE(provider.memoryUsage().committedBytes, quint64(0));
    }
}

void KisPageStoreReferenceTest::tiles3SourceSurvivesFailedPreparation()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    KisTiles3PageReplicaProvider provider;
    const auto descriptor = allocationDescriptor();
    QVERIFY(provider.configure({{82}, {1}, descriptor.minimumByteSize()}, completions));
    const auto full = provider.requestReplica({1000},
                                              pageVersion(1, 1),
                                              descriptor,
                                              KisPageAccessDomain::CpuRam,
                                              KisPageAccessMode::Read,
                                              KisPagePriority::Normal);
    QVERIFY(full.isValid());
    const quint8 pixel = 0x37;
    KisTileData *tile = KisTileDataStore::instance()->createDefaultTileData(1, &pixel);
    QVERIFY(tile->ref());
    auto input = provider.captureCompletedTileSource(descriptor, tile);
    QVERIFY(input);
    QVERIFY(!provider
                 .prepareWrite({1001},
                               pageVersion(2, 1),
                               descriptor,
                               KisPageAccessDomain::CpuRam,
                               KisPageWriteMode::DiscardContents,
                               KisPagePriority::Normal)
                 .isValid());
    // Both failed preparations leave the explicit source available for retry.
    QVERIFY(!provider.prepareSynchronousSource({1002}, input, pageVersion(2, 1), descriptor,
        KisReplicaSourceUse::ImmutableAlias, KisPagePriority::Normal).isValid());
    QCOMPARE(provider.payloadWork().adoptedPages, quint64(0));
    QVERIFY(provider.retire({1003}, full.replica, {}).isValid());
    const auto adopted = provider.prepareSynchronousSource({1004}, input, pageVersion(2, 1), descriptor,
        KisReplicaSourceUse::ImmutableAlias, KisPagePriority::Normal);
    QVERIFY(adopted.isValid());
    const KisPageLeaseId lease{1005};
    auto access = provider.resolveAccess(lease, {1005}, adopted.replica,
        {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
        KisPageAccessMode::Read);
    QVERIFY(access.isValid());
    QCOMPARE(provider.tileDataForLease(lease), tile);
    provider.releaseAccess(std::move(access), {});
    QVERIFY(provider.retire({1006}, adopted.replica, {}).isValid());
    input.reset();
    // Source and allocation ownership were both released exactly once.
    QVERIFY(!tile->deref());
}

void KisPageStoreReferenceTest::pageStoreNativeCopyIsLockExternalRetainedAndFailClosed()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto native = std::make_shared<KisTiles3PageReplicaProvider>();
    QVERIFY(native->configure({{83}, {1}, 1024 * 1024}, completions));
    auto provider = std::make_shared<ReentrantProbeCpuProvider>(native);
    provider->enableNativeCopy = true;
    const auto descriptor = allocationDescriptor();
    const auto initialVersion = pageVersion(1, 1);
    const auto allocation = provider->requestReplica({1000},
                                                     initialVersion,
                                                     descriptor,
                                                     KisPageAccessDomain::CpuRam,
                                                     KisPageAccessMode::Read,
                                                     KisPagePriority::Normal);
    QVERIFY(allocation.isValid());
    KisImageEpochSnapshot initial;
    initial.epoch = {1};
    initial.graphRevision = initial.defaultPixelRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.manifest = {initialVersion};
    KisPageStore store;
    QString error;
    QVERIFY(store.configure(initial, completions, 4, &error));
    QVERIFY(store.registerReplicaProvider(provider));
    QVERIFY(store.adoptInitialPage(initialVersion, descriptor, allocation.replica, &error));
    QVERIFY(store.finalizeInitialization(&error));
    const auto retained = store.captureRetainedEpoch();
    QVERIFY(retained.isValid());
    const KisPageAccessRequirement cpu{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
    const auto readRequest = store.acquireRead(initialVersion.key, {}, cpu, KisPagePriority::Normal);
    auto reader = store.resolve(readRequest, readRequest.readiness);
    QVERIFY(reader.isValid());
    int nativeCalls = 0, transferCalls = 0, prepareCalls = 0;
    provider->nativeCopyProbe = [&]() {
        QCOMPARE(store.sessionStats().pendingRequests, qsizetype(1));
        ++nativeCalls;
    };
    provider->transferProbe = [&]() {
        ++transferCalls;
    };
    provider->prepareProbe = [&]() {
        ++prepareCalls;
    };
    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto transaction = store.beginCurrentTransaction();
        QVERIFY(transaction.isValid());
        provider->corruptNextNativeOperation = attempt == 0;
        provider->pendNextNativeResult = attempt == 1;
        auto request = store.acquireWrite(transaction,
                                          initialVersion.key,
                                          cpu,
                                          KisPageWriteMode::PreserveContents,
                                          KisPagePriority::Interactive);
        if (attempt < 2) {
            QVERIFY(!request.isValid());
            QVERIFY(store.waitForRetirementIdle());
            QCOMPARE(native->memoryUsage().committedBytes, descriptor.minimumByteSize());
            QCOMPARE(store.sessionStats().pendingRequests, qsizetype(0));
            QVERIFY(store.abort(transaction));
            continue;
        }
        QVERIFY2(request.isValid(), qPrintable(request.error));
        auto writer = store.resolve(request, request.readiness);
        QVERIFY(writer.isValid());
        QCOMPARE(static_cast<const quint8 *>(writer.cpuData())[0], quint8(0));
        static_cast<quint8 *>(writer.cpuData())[0] = 0x75;
        QVERIFY(store.publishHostWrite(std::move(writer)).isValid());
        if (attempt == 2) {
            QVERIFY(store.abort(transaction));
        } else {
            QVERIFY(store.commit(transaction, store.preparedPages(transaction)).isValid());
        }
        QCOMPARE(static_cast<const quint8 *>(reader.cpuData())[0], quint8(0));
    }
    QCOMPARE(nativeCalls, 4);
    QCOMPARE(prepareCalls, 0);
    QCOMPARE(transferCalls, 0);
    QCOMPARE(native->payloadWork().nativeDuplicatePages, quint64(4));
    QCOMPARE(native->payloadWork().defaultInitializedPages, quint64(1));
    store.release(std::move(reader));
    KisPageReadView old;
    old.kind = KisPageReadViewKind::CommittedEpoch;
    old.epoch = retained.snapshot.epoch;
    old.retention = retained.token;
    const auto oldRequest = store.acquireRead(initialVersion.key, old, cpu, KisPagePriority::Normal);
    auto oldRead = store.resolve(oldRequest, oldRequest.readiness);
    QVERIFY(oldRead.isValid());
    QCOMPARE(static_cast<const quint8 *>(oldRead.cpuData())[0], quint8(0));
    store.release(std::move(oldRead));
    const auto current = store.acquireRead(initialVersion.key, {}, cpu, KisPagePriority::Normal);
    auto currentRead = store.resolve(current, current.readiness);
    QVERIFY(currentRead.isValid());
    QCOMPARE(static_cast<const quint8 *>(currentRead.cpuData())[0], quint8(0x75));
    store.release(std::move(currentRead));
    QVERIFY(store.releaseSnapshot(retained.token));
    QVERIFY2(store.closeSession(&error), qPrintable(error));
    QCOMPARE(native->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreReferenceTest::cpuProviderPreserves4_8_16ByteFormats()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    KisCpuPageReplicaProvider provider;
    KisCpuResidentReplicaProviderConfig config;
    config.provider = KisReplicaProviderId{76};
    config.providerEpoch = KisReplicaProviderEpoch{1};
    config.budgetBytes = 1024 * 1024;
    QString error;
    QVERIFY2(provider.configure(config, completions, &error), qPrintable(error));

    quint64 identity = 2000;
    for (const quint32 pixelStride : {quint32(4), quint32(8), quint32(16)}) {
        KisPageAllocationDescriptor descriptor = allocationDescriptor();
        descriptor.layoutRevision = pixelStride;
        descriptor.format.formatId = pixelStride;
        descriptor.format.colorModelId = QByteArrayLiteral("RGBA");
        descriptor.format.colorDepthId = QByteArray::number(pixelStride * 2);
        descriptor.format.profileFingerprint = QByteArrayLiteral("format-width-") + QByteArray::number(pixelStride);
        descriptor.format.channelOrder = QByteArrayLiteral("RGBA");
        descriptor.format.defaultPixel = QByteArray(qsizetype(pixelStride), 0);
        for (quint32 i = 0; i < pixelStride; ++i) {
            descriptor.format.defaultPixel[qsizetype(i)] = char(0x20 + i);
        }
        descriptor.format.channelCount = 4;
        descriptor.format.pixelStride = pixelStride;
        descriptor.format.pixelAlignment = pixelStride >= 8 ? 8 : 4;
        descriptor.format.hasAlpha = true;
        descriptor.format.alphaSemantic = KisSurfaceAlphaSemantic::Premultiplied;
        descriptor.pageExtent = QSize(3, 2);
        descriptor.validRect = QRect(QPoint(0, 0), descriptor.pageExtent);
        descriptor.rowAlignment = 64;
        descriptor.initialization = KisPageInitialization::DefaultPixel;
        QVERIFY(descriptor.isValid());

        const KisPageVersion version = pageVersion(qint32(pixelStride), 1);
        const KisReplicaOperation allocation = provider.requestReplica(KisPageOperationId{identity++},
                                                                       version,
                                                                       descriptor,
                                                                       KisPageAccessDomain::CpuRam,
                                                                       KisPageAccessMode::Read,
                                                                       KisPagePriority::Normal);
        QVERIFY(allocation.isValid());
        QCOMPARE(allocation.replica.layout.rowStride, quint32(64));
        const KisPageLeaseId lease{identity++};
        const KisPageOperationId accessOperation{identity++};
        KisReplicaAccess access = provider.resolveAccess(lease,
                                                         accessOperation,
                                                         allocation.replica,
                                                         {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                                                         KisPageAccessMode::Read);
        QVERIFY(access.isValid());
        QCOMPARE(QByteArray(static_cast<const char *>(access.cpuReadData), qsizetype(pixelStride)),
                 descriptor.format.defaultPixel);
        QCOMPARE(QByteArray(static_cast<const char *>(access.cpuReadData) + access.replica.layout.rowStride, qsizetype(pixelStride)),
                 descriptor.format.defaultPixel);
        provider.releaseAccess(std::move(access), {});
        const KisReplicaOperation retired = provider.retire(KisPageOperationId{identity++}, allocation.replica, {});
        QVERIFY(retired.isValid());
    }
    QCOMPARE(provider.memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreReferenceTest::ownerLedgerVerifiesTerminalProviderIdentity()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<KisCpuPageReplicaProvider>();
    QVERIFY(provider->configure({{51}, {1}, 1024 * 1024}, completions));
    KisPageOwnerLedger owner; QVERIFY(owner.configure(completions));
    QVERIFY(owner.registerProvider(provider));
    auto descriptor = allocationDescriptor();
    descriptor.initialization = KisPageInitialization::DefaultPixel;
    const auto operation = owner.nextOperationId();
    const auto result = provider->requestReplica(operation, pageVersion(30, 1), descriptor,
        KisPageAccessDomain::CpuRam, KisPageAccessMode::Read, KisPagePriority::Normal);
    QVERIFY(result.isValid());
    QVERIFY(owner.verifyTerminalProviderResult(operation, result).succeeded());
    QVERIFY(!owner.verifyTerminalProviderResult(owner.nextOperationId(), result).isValid());
    auto foreign = result; foreign.replica.providerEpoch.value++;
    QVERIFY(!owner.verifyTerminalProviderResult(operation, foreign).isValid());
    auto unknown = result;
    auto other = std::make_shared<KisCompletionRegistry>();
    unknown.completion = other->allocatePending(other->registerSource(KisCompletionDomain::CpuJob));
    QVERIFY(other->complete(unknown.completion, KisCompletionStatus::Succeeded));
    QVERIFY(!owner.verifyTerminalProviderResult(operation, unknown).isValid());

    const auto source = completions->registerSource(KisCompletionDomain::CpuJob);
    for (const auto status : {KisCompletionStatus::Succeeded, KisCompletionStatus::Failed, KisCompletionStatus::Cancelled}) {
        auto pending = result;
        pending.status = KisPageRequestStatus::Pending;
        pending.completion = completions->allocatePending(source);
        QVERIFY(!owner.verifyTerminalProviderResult(operation, pending).isValid());
        QCOMPARE(owner.providerOperationCount(), qsizetype(0));
        QVERIFY(completions->complete(pending.completion, status));
        const auto verified = owner.verifyTerminalProviderResult(operation, pending);
        QVERIFY(verified.isValid());
        QCOMPARE(verified.status(), status);
        QCOMPARE(verified.succeeded(), status == KisCompletionStatus::Succeeded);
    }
    // Direct verification cannot consume a result already owned by binding.
    QVERIFY(owner.bindProviderOperation(operation, result));
    QVERIFY(!owner.verifyTerminalProviderResult(operation, result).isValid());
    QCOMPARE(owner.providerOperationCount(), qsizetype(1));
    QVERIFY(owner.releaseTerminalProviderOperation(operation));
    QCOMPARE(owner.providerOperationCount(), qsizetype(0));
    QVERIFY(provider->retire(owner.nextOperationId(), result.replica, {}).isValid());
    QCOMPARE(provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreReferenceTest::ownerLedgerSeparatesDetachedRetirement()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<FakeCpuReplicaProvider>(completions);
    KisBackingBudgetController budget;
    KisPageOwnerLedger owner;
    QVERIFY(owner.configure(completions));
    owner.attachBackingBudget(budget);
    QVERIFY(owner.registerProvider(provider));
    auto descriptor = allocationDescriptor();
    descriptor.initialization = KisPageInitialization::DefaultPixel;
    const auto operation = owner.nextOperationId();
    auto result = provider->requestReplica(operation,
                                           pageVersion(30, 1),
                                           descriptor,
                                           KisPageAccessDomain::CpuRam,
                                           KisPageAccessMode::Read,
                                           KisPagePriority::Normal);
    QVERIFY(result.isValid());
    QVERIFY(owner.bindProviderOperation(operation, result));
    QCOMPARE(owner.providerOperationCount(), qsizetype(1));
    QCOMPARE(owner.publicationBlockingOperationCount(), qsizetype(1));
    // A duplicate bind cannot reclassify an existing payload operation.
    QVERIFY(!owner.bindRetirementOperation(operation, result));
    QCOMPARE(owner.publicationBlockingOperationCount(), qsizetype(1));
    QVERIFY(provider->complete(operation, true));
    QVERIFY(owner.releaseTerminalProviderOperation(operation));
    // An unprepared result cannot recreate the old post-provider allocation.
    auto unprepared = result; unprepared.operation = owner.nextOperationId();
    QVERIFY(!owner.bindRetirementOperation(unprepared.operation, unprepared));
    QCOMPARE(owner.providerOperationCount(), qsizetype(0));
    KisPageMetadataCoordinator metadata; QVERIFY(metadata.configure(1));
    QAtomicInt references{1};
    KisPageRetirementQueue queue(owner, metadata, budget, references, &references,
        [](void *p) { static_cast<QAtomicInt *>(p)->deref(); });
    auto record = kisPreparePageRetirementRecord(&budget);
    record->replica = result.replica; record->provider = provider;
    QVERIFY(!queue.retireRecord(*record)); // Actual provider returns Pending.
    const auto retirement = record->retirementOperation;
    QVERIFY(retirement.isValid());
    auto duplicate = result; duplicate.operation = retirement;
    QVERIFY(!owner.bindProviderOperation(retirement, duplicate));
    QVERIFY(!owner.releaseTerminalProviderOperation(retirement));
    QCOMPARE(owner.providerOperationCount(), qsizetype(1));
    QCOMPARE(owner.publicationBlockingOperationCount(), qsizetype(0));
    // Payload and retirement coexist without two-snapshot subtraction races.
    const auto second = owner.nextOperationId();
    auto payload = result;
    payload.operation = second;
    const auto ticket = completions->allocatePending(completions->registerSource(KisCompletionDomain::CpuJob));
    payload.completion = ticket;
    QVERIFY(owner.bindProviderOperation(second, payload));
    QCOMPARE(owner.providerOperationCount(), qsizetype(2));
    QCOMPARE(owner.publicationBlockingOperationCount(), qsizetype(1));
    QVERIFY(provider->complete(retirement, true));
    QVERIFY(queue.retireRecord(*record));
    QVERIFY(!record->operationStorage.empty());
    QVERIFY(!owner.releaseTerminalProviderOperation(retirement));
    QCOMPARE(owner.providerOperationCount(), qsizetype(1));
    QCOMPARE(owner.publicationBlockingOperationCount(), qsizetype(1));
    QVERIFY(completions->complete(ticket, KisCompletionStatus::Succeeded));
    QVERIFY(owner.releaseTerminalProviderOperation(second));
    QCOMPARE(owner.providerOperationCount(), qsizetype(0));
    QCOMPARE(owner.publicationBlockingOperationCount(), qsizetype(0));
    QCOMPARE(provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreReferenceTest::ownerLedgerSealsPreparedPageBeforeEpochCommit()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig providerConfig;
    providerConfig.provider = KisReplicaProviderId{51};
    providerConfig.providerEpoch = KisReplicaProviderEpoch{1};
    providerConfig.budgetBytes = 1024 * 1024;
    QString error;
    QVERIFY2(provider->configure(providerConfig, completions, &error), qPrintable(error));

    KisPageBackingLimits limits;
    limits.metadataArenaBytes = 64 * 1024;
    KisBackingBudgetController budget(limits);
    KisPageOwnerLedger owner;
    QVERIFY2(owner.configure(completions, &error), qPrintable(error));
    owner.attachBackingBudget(budget);
    KisPageOwnerLedger secondOwner;
    QVERIFY2(secondOwner.configure(completions, &error), qPrintable(error));
    QVERIFY(!(owner.nextLeaseId() == secondOwner.nextLeaseId()));
    QVERIFY(!(owner.nextOperationId() == secondOwner.nextOperationId()));
    QVERIFY2(owner.registerProvider(provider, &error), qPrintable(error));
    QVERIFY(owner.providerFor({KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer}) == provider);

    KisPageAllocationDescriptor descriptor = allocationDescriptor();
    descriptor.initialization = KisPageInitialization::DefaultPixel;
    const KisPageVersion base = pageVersion(30, 1);
    const KisPageOperationId baseOperation = owner.nextOperationId();
    const KisReplicaOperation baseAllocation = provider->requestReplica(baseOperation,
                                                                        base,
                                                                        descriptor,
                                                                        KisPageAccessDomain::CpuRam,
                                                                        KisPageAccessMode::Read,
                                                                        KisPagePriority::Normal);
    QVERIFY(baseAllocation.isValid());
    QVERIFY2(owner.bindProviderOperation(baseOperation, baseAllocation, &error), qPrintable(error));
    QVERIFY(owner.verifyProviderOperation(baseOperation, &error).succeeded());

    KisPageMetadataCoordinator metadata;
    QVERIFY2(metadata.configure(4, &error), qPrintable(error));
    QVERIFY2(metadata.registerPage(initialPageState(base, baseAllocation.replica), &error), qPrintable(error));

    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = 1;
    initial.defaultPixelRevision = 1;
    initial.extentRevision = 1;
    initial.propertyRevision = 1;
    initial.manifest = {base};
    KisImageEpochReferenceModel epochs;
    QVERIFY2(epochs.initialize(initial, &error), qPrintable(error));
    const KisPageTransaction transaction = epochs.beginTransaction(initial.epoch, &error);
    QVERIFY(transaction.isValid());

    const KisPageVersion writeVersion{base.key, KisPageGeneration{2}};
    const KisPageOperationId writeOperation = owner.nextOperationId();
    const KisReplicaOperation writeAllocation = provider->prepareWrite(writeOperation,
                                                                       writeVersion,
                                                                       descriptor,
                                                                       KisPageAccessDomain::CpuRam,
                                                                       KisPageWriteMode::PreserveContents,
                                                                       KisPagePriority::Normal);
    QVERIFY(writeAllocation.isValid());
    QVERIFY2(owner.bindProviderOperation(writeOperation, writeAllocation, &error), qPrintable(error));

    KisPageTransition write;
    write.kind = KisPageTransitionKind::AcquireWrite;
    write.baseVersion = base;
    write.version = writeVersion;
    write.source = baseAllocation.replica;
    write.target = writeAllocation.replica;
    write.operation = writeOperation;
    write.writer = owner.nextWriterToken();
    write.transaction = transaction.id;
    write.writeMode = KisPageWriteMode::PreserveContents;
    KisPageMetadataTransitionResult transition = metadata.applyOwner(base.key, write);
    QVERIFY2(transition.accepted, qPrintable(transition.rejectionReason));

    write.kind = KisPageTransitionKind::PrepareWrite;
    transition = metadata.applyOwner(base.key, write);
    QVERIFY2(transition.accepted, qPrintable(transition.rejectionReason));

    const KisPageLeaseId writeLease = owner.nextLeaseId();
    KisReplicaAccess access = provider->resolveAccess(writeLease,
                                                      owner.nextOperationId(),
                                                      writeAllocation.replica,
                                                      {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                                                      KisPageAccessMode::Write);
    QVERIFY(access.isValid());
    static_cast<quint8 *>(access.cpuWriteData)[0] = 0x5a;
    provider->releaseAccess(std::move(access), {});

    const KisCompletionDomain producerDescriptor = KisCompletionDomain::HostLogical;
    const quint64 producerSource = completions->registerSource(producerDescriptor);
    const KisCompletionTicket producerCompletion = completions->allocatePending(producerSource);
    QVERIFY(completions->complete(producerCompletion, KisCompletionStatus::Succeeded));

    write.kind = KisPageTransitionKind::BeginPublish;
    transition = metadata.applyOwner(base.key, write);
    QVERIFY2(transition.accepted, qPrintable(transition.rejectionReason));
    write.kind = KisPageTransitionKind::PublishWrite;
    transition = metadata.applyOwner(base.key, write);
    QVERIFY2(transition.accepted, qPrintable(transition.rejectionReason));

    auto warmFirst = budget.reserve({}, nullptr), warmSecond = budget.reserve({}, nullptr);
    QVERIFY(warmFirst.isValid() && warmSecond.isValid()); warmFirst.release(); warmSecond.release();
    const auto live = [&] { return budget.usage().buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam; };
    const auto baseline = live();
    KisPreparedPageProof proof;
    proof.providerValidationStamp = 77; // Refusal must not change the caller's output.
    {
        const size_t bytes = size_t(limits.metadataArenaBytes - live());
        void *filler = kisAllocateMutationStorage(&budget, bytes, 1);
        const auto release = qScopeGuard([&] { kisFreeMutationStorage(&budget, filler, bytes, 1); });
        QVERIFY(!owner.sealPreparedPage(metadata, writeVersion, transaction.id, descriptor,
                                       producerCompletion, &proof, &error));
        QVERIFY(error.contains(QStringLiteral("proof storage")));
        QCOMPARE(proof.providerValidationStamp, quint64(77));
        QCOMPARE(owner.sealedProofCount(), qsizetype(0));
        QCOMPARE(live(), limits.metadataArenaBytes);
        KisPageMetadataCoordinator::VersionInfo unchanged;
        QVERIFY(metadata.versionSnapshot(writeVersion, &unchanged));
        QCOMPARE(unchanged.version, writeVersion);
        QCOMPARE(unchanged.publication, KisPagePublicationState::Prepared);
        QVERIFY(unchanged.authority == writeAllocation.replica && unchanged.preparedBy == transaction.id);
    }
    QCOMPARE(live(), baseline);
    size_t sealFillerBytes = 0;
    void *sealFiller = nullptr;
    quint64 proofBytes = 0;
    const auto clearValidationPressure = qScopeGuard([&] {
        provider->beforeValidate = {};
        kisFreeMutationStorage(&budget, sealFiller, sealFillerBytes, 1);
    });
    provider->beforeValidate = [&] {
        proofBytes = live() - baseline;
        sealFillerBytes = size_t(limits.metadataArenaBytes - live());
        sealFiller = kisAllocateMutationStorage(&budget, sealFillerBytes, 1);
    };
    QVERIFY2(
        owner.sealPreparedPage(metadata, writeVersion, transaction.id, descriptor, producerCompletion, &proof, &error),
        qPrintable(error));
    QCOMPARE(proof.providerValidationStamp, quint64(1)); // A refused node consumed no stamp.
    QVERIFY(proofBytes > 0);
    QCOMPARE(live(), limits.metadataArenaBytes); // Prepared node installs after provider validation at capacity.
    provider->beforeValidate = {};
    kisFreeMutationStorage(&budget, std::exchange(sealFiller, nullptr), sealFillerBytes, 1);
    QCOMPARE(live(), baseline + proofBytes);
    KisPreparedPageProof extra;
    QVERIFY(owner.sealPreparedPage(metadata, writeVersion, transaction.id, descriptor,
                                  producerCompletion, &extra, &error));
    QCOMPARE(extra.providerValidationStamp, quint64(2));
    QCOMPARE(live(), baseline + 2 * proofBytes);
    {
        const size_t bytes = size_t(limits.metadataArenaBytes - live());
        void *filler = kisAllocateMutationStorage(&budget, bytes, 1);
        const auto release = qScopeGuard([&] { kisFreeMutationStorage(&budget, filler, bytes, 1); });
        QVERIFY(owner.ownsPreparedPageProof(extra));
        QVERIFY(owner.validatePreparedPage(metadata, extra, descriptor, &error));
        QVERIFY(owner.revokePreparedPage(extra));
        QCOMPARE(live(), limits.metadataArenaBytes - proofBytes);
        QVERIFY(!owner.revokePreparedPage(extra));
    }
    QCOMPARE(live(), baseline + proofBytes);
    QVERIFY2(owner.validatePreparedPage(metadata, proof, descriptor, &error), qPrintable(error));
    KisPreparedPageProof forgedProof = proof;
    forgedProof.providerValidationStamp++;
    QVERIFY(!owner.validatePreparedPage(metadata, forgedProof, descriptor, &error));

    KisPreparedPageSet prepared;
    prepared.transaction = transaction.id;
    prepared.proofs = {proof};
    QVERIFY2(epochs.prepare(prepared, &error), qPrintable(error));
    const KisImageEpochCommitResult committed = epochs.commit(transaction);
    QVERIFY2(committed.isCommitted(), qPrintable(committed.error));

    KisPageTransition commit;
    commit.kind = KisPageTransitionKind::CommitTransaction;
    commit.version = writeVersion;
    commit.transaction = transaction.id;
    commit.imageEpoch = committed.root.epoch();
    auto metadataCommit = metadata.preparePublication(
        transaction, commit.imageEpoch, {commit}, &error);
    QVERIFY2(metadataCommit.isValid(), qPrintable(error));
    QVERIFY2(metadata.installPublication(std::move(metadataCommit), transaction,
                                         commit.imageEpoch, &error),
             qPrintable(error));

    KisPageStateSnapshot finalState;
    QVERIFY(metadata.pageSnapshot(base.key, &finalState));
    QCOMPARE(finalState.publishedEpoch.value, committed.root.epoch().value);
    QCOMPARE(finalState.publishedGeneration.value, quint64(2));
    QVERIFY(!owner.validatePreparedPage(metadata, proof, descriptor, &error));
    QVERIFY(owner.revokePreparedPage(proof));
    QCOMPARE(live(), baseline);
    QVERIFY(!owner.revokePreparedPage(proof));
    QCOMPARE(committed.root.snapshot().manifest.first().generation.value, quint64(2));

    auto ambiguousProvider = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig ambiguousConfig = providerConfig;
    ambiguousConfig.provider = KisReplicaProviderId{52};
    QVERIFY2(ambiguousProvider->configure(ambiguousConfig, completions, &error), qPrintable(error));
    QVERIFY2(owner.registerProvider(ambiguousProvider, &error), qPrintable(error));
    QVERIFY(!owner.providerFor({KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer}));
}

void KisPageStoreReferenceTest::pageStoreCpuFacadeCommitsCowWithoutMixedEpoch()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig providerConfig;
    providerConfig.provider = KisReplicaProviderId{61};
    providerConfig.providerEpoch = KisReplicaProviderEpoch{1};
    providerConfig.budgetBytes = 1024 * 1024;
    QString error;
    QVERIFY2(provider->configure(providerConfig, completions, &error), qPrintable(error));

    KisPageAllocationDescriptor descriptor = allocationDescriptor();
    descriptor.initialization = KisPageInitialization::DefaultPixel;
    const KisPageVersion initialVersion = pageVersion(40, 1);
    const KisReplicaOperation initialAllocation = provider->requestReplica(KisPageOperationId{500},
                                                                           initialVersion,
                                                                           descriptor,
                                                                           KisPageAccessDomain::CpuRam,
                                                                           KisPageAccessMode::Read,
                                                                           KisPagePriority::Normal);
    QVERIFY(initialAllocation.isValid());

    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = 4;
    initial.defaultPixelRevision = 2;
    initial.extentRevision = 3;
    initial.propertyRevision = 5;
    initial.manifest = {initialVersion};

    KisPageStore store;
    QVERIFY2(store.configure(initial, completions, 4, &error), qPrintable(error));
    QVERIFY(!store.isOperational());
    auto asyncBridge = std::make_shared<StubTransferBridge>(false);
    auto syncBridge = std::make_shared<StubTransferBridge>(true);
    QVERIFY(!store.registerTransferBridge(asyncBridge));
    QVERIFY(store.registerTransferBridge(syncBridge));
    QVERIFY(!store.registerTransferBridge(syncBridge));
    QVERIFY(store.registerReplicaProvider(provider));
    QVERIFY2(store.adoptInitialPage(initialVersion, descriptor, initialAllocation.replica, &error), qPrintable(error));
    QCOMPARE(store.backingUsage().buckets[size_t(KisBackingBudgetClass::Current)].live.cpuRam, 4096u);
    QVERIFY2(store.finalizeInitialization(&error), qPrintable(error));
    QVERIFY(store.isOperational());
    const KisRetainedImageEpochSnapshot retainedInitial = store.captureRetainedEpoch();
    QVERIFY(retainedInitial.isValid());
    QVERIFY(store.validateRetainedEpoch(retainedInitial));

    KisPageReadView current;
    KisReadRequest beforeRequest = store.acquireRead(initialVersion.key,
                                                     current,
                                                     {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                                                     KisPagePriority::Normal);
    QVERIFY(beforeRequest.isValid());
    KisReadLease before = store.resolve(beforeRequest, beforeRequest.readiness);
    QVERIFY(before.isValid());
    QCOMPARE(static_cast<const quint8 *>(before.cpuData())[0], quint8(0));
    store.release(std::move(before));

    const KisPageTransaction transaction = store.beginTransaction(KisImageEpochId{1});
    QVERIFY(transaction.isValid());
    KisPageTransaction forgedTransaction = transaction;
    forgedTransaction.baseEpoch = KisImageEpochId{999};
    QVERIFY(!store
                 .acquireWrite(forgedTransaction,
                               initialVersion.key,
                               {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                               KisPageWriteMode::PreserveContents,
                               KisPagePriority::Normal)
                 .isValid());
    QVERIFY(!store.abort(forgedTransaction));
    KisWriteRequest writeRequest = store.acquireWrite(transaction,
                                                      initialVersion.key,
                                                      {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                                                      KisPageWriteMode::PreserveContents,
                                                      KisPagePriority::Interactive);
    QVERIFY2(writeRequest.isValid(), qPrintable(writeRequest.error));
    QCOMPARE(store.backingUsage().buckets[size_t(KisBackingBudgetClass::ActivePending)].live.cpuRam, 4096u);
    KisWriteLease write = store.resolve(writeRequest, writeRequest.readiness);
    QVERIFY(write.isValid());
    static_cast<quint8 *>(write.cpuData())[0] = 0xa5;

    const KisCompletionDomain producerDescriptor = KisCompletionDomain::HostLogical;
    const quint64 producerSource = completions->registerSource(producerDescriptor);
    const KisCompletionTicket producerCompletion = completions->allocatePending(producerSource);
    QVERIFY(completions->complete(producerCompletion, KisCompletionStatus::Succeeded));
    QCOMPARE(store.publish(std::move(write), producerCompletion), producerCompletion);

    const KisPreparedPageSet prepared = store.preparedPages(transaction);
    QVERIFY(prepared.isValid());
    const KisImageEpochCommitTicket committed = store.commit(transaction, prepared);
    QVERIFY(committed.isValid());
    QCOMPARE(committed.epoch.value, quint64(2));
    const auto committedBacking = store.backingUsage();
    QCOMPARE(committedBacking.buckets[size_t(KisBackingBudgetClass::Current)].live.cpuRam, 4096u);
    QCOMPARE(committedBacking.buckets[size_t(KisBackingBudgetClass::RetainedHistory)].live.cpuRam, 4096u);
    QCOMPARE(committedBacking.buckets[size_t(KisBackingBudgetClass::ActivePending)].live.cpuRam, 0u);
    const auto publicationStatistics = store.publicationStatistics();
    QCOMPARE(publicationStatistics.preparedMutationCommits, quint64(1));
    QCOMPARE(publicationStatistics.installedMutationCommits, quint64(1));
    QCOMPARE(publicationStatistics.cancelledMutationCommits, quint64(0));

    const KisImageEpochSnapshot committedSnapshot = store.captureCommittedEpoch();
    QVERIFY(committedSnapshot.isValid());
    QCOMPARE(committedSnapshot.graphRevision, quint64(4));
    QCOMPARE(committedSnapshot.manifest.first().generation.value, quint64(2));

    KisReadRequest afterRequest = store.acquireRead(initialVersion.key,
                                                    current,
                                                    {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                                                    KisPagePriority::Normal);
    QVERIFY(afterRequest.isValid());
    KisReadLease after = store.resolve(afterRequest, afterRequest.readiness);
    QVERIFY(after.isValid());
    QCOMPARE(static_cast<const quint8 *>(after.cpuData())[0], quint8(0xa5));
    store.release(std::move(after));
    KisReadRequest cancelledRead = store.acquireRead(initialVersion.key,
                                                     current,
                                                     {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                                                     KisPagePriority::Normal);
    QVERIFY(cancelledRead.isValid());
    QVERIFY(store.cancel(cancelledRead));
    QVERIFY(!store.cancel(cancelledRead));
    QVERIFY(!store.resolve(cancelledRead, cancelledRead.readiness).isValid());

    KisPageReadView historical;
    historical.kind = KisPageReadViewKind::CommittedEpoch;
    historical.epoch = KisImageEpochId{1};
    historical.retention = retainedInitial.token;
    KisReadRequest oldRequest = store.acquireRead(initialVersion.key,
                                                  historical,
                                                  {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                                                  KisPagePriority::Background);
    QVERIFY(oldRequest.isValid());
    KisReadLease old = store.resolve(oldRequest, oldRequest.readiness);
    QVERIFY(old.isValid());
    QCOMPARE(static_cast<const quint8 *>(old.cpuData())[0], quint8(0));
    store.release(std::move(old));

    const KisReplicaMemoryUsage committedUsage = provider->memoryUsage();
    const KisPageTransaction pendingAbort = store.beginTransaction(committed.epoch);
    QVERIFY(pendingAbort.isValid());
    const KisWriteRequest pendingWrite =
        store.acquireWrite(pendingAbort,
                           initialVersion.key,
                           {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                           KisPageWriteMode::PreserveContents,
                           KisPagePriority::Normal);
    QVERIFY(pendingWrite.isValid());
    QVERIFY(provider->memoryUsage().committedBytes > committedUsage.committedBytes);
    QVERIFY(store.abort(pendingAbort));
    QVERIFY(!store.resolve(pendingWrite, pendingWrite.readiness).isValid());
    QCOMPARE(provider->memoryUsage().committedBytes, committedUsage.committedBytes);

    const KisPageTransaction requestCancel = store.beginTransaction(committed.epoch);
    QVERIFY(requestCancel.isValid());
    const KisWriteRequest cancelledWriteRequest =
        store.acquireWrite(requestCancel,
                           initialVersion.key,
                           {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                           KisPageWriteMode::DiscardContents,
                           KisPagePriority::Normal);
    QVERIFY(cancelledWriteRequest.isValid());
    QVERIFY(store.cancel(cancelledWriteRequest));
    QVERIFY(!store.cancel(cancelledWriteRequest));
    QVERIFY(store.abort(requestCancel));
    QCOMPARE(provider->memoryUsage().committedBytes, committedUsage.committedBytes);

    const KisPageTransaction activeAbort = store.beginTransaction(committed.epoch);
    QVERIFY(activeAbort.isValid());
    const KisWriteRequest activeWriteRequest =
        store.acquireWrite(activeAbort,
                           initialVersion.key,
                           {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                           KisPageWriteMode::PreserveContents,
                           KisPagePriority::Normal);
    QVERIFY(activeWriteRequest.isValid());
    KisWriteLease activeWrite = store.resolve(activeWriteRequest, activeWriteRequest.readiness);
    QVERIFY(activeWrite.isValid());
    QVERIFY(!store.abort(activeAbort));
    QVERIFY(provider->memoryUsage().committedBytes > committedUsage.committedBytes);
    store.cancel(std::move(activeWrite));
    QVERIFY(store.abort(activeAbort));
    QCOMPARE(provider->memoryUsage().committedBytes, committedUsage.committedBytes);

    const KisPageTransaction retry = store.beginTransaction(committed.epoch);
    QVERIFY(retry.isValid());
    const KisWriteRequest retryRequest =
        store.acquireWrite(retry,
                           initialVersion.key,
                           {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                           KisPageWriteMode::DiscardContents,
                           KisPagePriority::Normal);
    QVERIFY(retryRequest.isValid());
    KisWriteLease retryWrite = store.resolve(retryRequest, retryRequest.readiness);
    QVERIFY(retryWrite.isValid());
    store.cancel(std::move(retryWrite));
    QVERIFY(store.abort(retry));
    QCOMPARE(provider->memoryUsage().committedBytes, committedUsage.committedBytes);

    QVERIFY(store.releaseSnapshot(retainedInitial.token));
    QVERIFY(!store.validateRetainedEpoch(retainedInitial));
    QVERIFY(!store.releaseSnapshot(retainedInitial.token));
}

void KisPageStoreReferenceTest::pageStoreChainsRepeatedWritesWithinTransaction()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig providerConfig;
    providerConfig.provider = KisReplicaProviderId{62};
    providerConfig.providerEpoch = KisReplicaProviderEpoch{1};
    providerConfig.budgetBytes = 1024 * 1024;
    QString error;
    QVERIFY2(provider->configure(providerConfig, completions, &error), qPrintable(error));

    KisPageAllocationDescriptor descriptor = allocationDescriptor();
    descriptor.initialization = KisPageInitialization::DefaultPixel;
    const KisPageVersion initialVersion = pageVersion(41, 1);
    const KisReplicaOperation initialAllocation = provider->requestReplica(KisPageOperationId{600},
                                                                           initialVersion,
                                                                           descriptor,
                                                                           KisPageAccessDomain::CpuRam,
                                                                           KisPageAccessMode::Read,
                                                                           KisPagePriority::Normal);
    QVERIFY(initialAllocation.isValid());

    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = 1;
    initial.defaultPixelRevision = 1;
    initial.extentRevision = 1;
    initial.propertyRevision = 1;
    initial.manifest = {initialVersion};

    KisPageStore store;
    QVERIFY2(store.configure(initial, completions, 4, &error), qPrintable(error));
    QVERIFY(store.registerReplicaProvider(provider));
    QVERIFY2(store.adoptInitialPage(initialVersion, descriptor, initialAllocation.replica, &error), qPrintable(error));
    QVERIFY2(store.finalizeInitialization(&error), qPrintable(error));

    const KisCompletionDomain producerDescriptor = KisCompletionDomain::HostLogical;
    const quint64 producerSource = completions->registerSource(producerDescriptor);
    QVERIFY(producerSource != 0);

    const KisPageTransaction transaction = store.beginTransaction(initial.epoch);
    QVERIFY(transaction.isValid());
    const KisPageAccessRequirement cpuAccess{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};

    KisWriteRequest firstRequest = store.acquireWrite(transaction,
                                                      initialVersion.key,
                                                      cpuAccess,
                                                      KisPageWriteMode::PreserveContents,
                                                      KisPagePriority::Interactive);
    QVERIFY2(firstRequest.isValid(), qPrintable(firstRequest.error));
    QCOMPARE(firstRequest.baseVersion.generation.value, quint64(1));
    QCOMPARE(firstRequest.writeVersion.generation.value, quint64(2));
    KisWriteLease firstWrite = store.resolve(firstRequest, firstRequest.readiness);
    QVERIFY(firstWrite.isValid());
    static_cast<quint8 *>(firstWrite.cpuData())[0] = 0xa5;
    const KisCompletionTicket firstCompletion = completions->allocatePending(producerSource);
    QVERIFY(completions->complete(firstCompletion, KisCompletionStatus::Succeeded));
    QCOMPARE(store.publish(std::move(firstWrite), firstCompletion), firstCompletion);

    KisPageReadView overlay;
    overlay.kind = KisPageReadViewKind::TransactionOverlay;
    overlay.transaction = transaction.id;
    KisReadRequest overlayRequest = store.acquireRead(initialVersion.key, overlay, cpuAccess, KisPagePriority::Normal);
    QVERIFY2(overlayRequest.isValid(), qPrintable(overlayRequest.error));
    QCOMPARE(overlayRequest.version.generation.value, quint64(2));
    KisReadLease overlayRead = store.resolve(overlayRequest, overlayRequest.readiness);
    QVERIFY(overlayRead.isValid());
    QCOMPARE(static_cast<const quint8 *>(overlayRead.cpuData())[0], quint8(0xa5));

    KisWriteRequest secondRequest = store.acquireWrite(transaction,
                                                       initialVersion.key,
                                                       cpuAccess,
                                                       KisPageWriteMode::PreserveContents,
                                                       KisPagePriority::Interactive);
    QVERIFY2(secondRequest.isValid(), qPrintable(secondRequest.error));
    QCOMPARE(secondRequest.baseVersion.generation.value, quint64(2));
    QCOMPARE(secondRequest.writeVersion.generation.value, quint64(3));
    KisWriteLease secondWrite = store.resolve(secondRequest, secondRequest.readiness);
    QVERIFY(secondWrite.isValid());
    QCOMPARE(static_cast<const quint8 *>(secondWrite.cpuData())[0], quint8(0xa5));
    static_cast<quint8 *>(secondWrite.cpuData())[1] = 0x5a;
    const KisCompletionTicket secondCompletion = completions->allocatePending(producerSource);
    QVERIFY(completions->complete(secondCompletion, KisCompletionStatus::Succeeded));
    QCOMPARE(store.publish(std::move(secondWrite), secondCompletion), secondCompletion);

    QCOMPARE(store.sessionStats().pageVersions, qsizetype(3));
    const KisCompletionTicket readerLastUse = completions->allocatePending(producerSource);
    QVERIFY(readerLastUse.isValid());
    store.release(std::move(overlayRead), readerLastUse);
    QCOMPARE(store.sessionStats().pendingLastUses, qsizetype(1));
    QCOMPARE(store.sessionStats().pageVersions, qsizetype(3));
    QVERIFY(completions->complete(readerLastUse, KisCompletionStatus::Succeeded));
    const KisVerifiedCompletion verifiedLastUse = completions->verifyTerminal(readerLastUse);
    QVERIFY(verifiedLastUse.isValid());
    QVERIFY(store.acknowledgeLastUse(verifiedLastUse));

    KisPageVersion resolvedVersion;
    KisPageReadView committedView;
    QVERIFY(store.resolvePageVersion(initialVersion.key, committedView, &resolvedVersion));
    QCOMPARE(resolvedVersion, initialVersion);
    QVERIFY(store.resolvePageVersion(initialVersion.key, overlay, &resolvedVersion));
    QCOMPARE(resolvedVersion.generation.value, quint64(3));

    const KisPageStoreSessionStats preparedStats = store.sessionStats();
    QCOMPARE(preparedStats.preparedPageProofs, qsizetype(1));
    QCOMPARE(preparedStats.sealedPreparedProofs, qsizetype(1));
    QCOMPARE(preparedStats.pageVersions, qsizetype(2));

    const KisPreparedPageSet prepared = store.preparedPages(transaction);
    QVERIFY(prepared.isValid());
    QCOMPARE(prepared.proofs.size(), qsizetype(1));
    QCOMPARE(prepared.proofs.first().authority.version.generation.value, quint64(3));
    const KisImageEpochCommitTicket committed = store.commit(transaction, prepared);
    QVERIFY(committed.isValid());

    KisPageReadView current;
    KisReadRequest committedRequest =
        store.acquireRead(initialVersion.key, current, cpuAccess, KisPagePriority::Normal);
    QVERIFY(committedRequest.isValid());
    QCOMPARE(committedRequest.version.generation.value, quint64(3));
    KisReadLease committedRead = store.resolve(committedRequest, committedRequest.readiness);
    QVERIFY(committedRead.isValid());
    QCOMPARE(static_cast<const quint8 *>(committedRead.cpuData())[0], quint8(0xa5));
    QCOMPARE(static_cast<const quint8 *>(committedRead.cpuData())[1], quint8(0x5a));
    store.release(std::move(committedRead));

    KisPageStoreSessionStats stats = store.sessionStats();
    QVERIFY(stats.configured);
    QVERIFY(stats.operational);
    QVERIFY(!stats.closed);
    QCOMPARE(stats.activeTransactions, qsizetype(0));
    QCOMPARE(stats.providerOperations, qsizetype(0));
    QCOMPARE(stats.sealedPreparedProofs, qsizetype(0));
    QCOMPARE(stats.immutableRoots, qsizetype(1));

    const KisRetainedImageEpochSnapshot retained = store.captureRetainedEpoch();
    QVERIFY(retained.isValid());
    QVERIFY(!store.closeSession(&error));
    QCOMPARE(error, QStringLiteral("PageStore session still owns capabilities"));
    QVERIFY(store.sessionStats().hasOutstandingCapabilities());
    QVERIFY(store.releaseSnapshot(retained.token));

    KisReadRequest closeBlockedRequest =
        store.acquireRead(initialVersion.key, current, cpuAccess, KisPagePriority::Normal);
    QVERIFY(closeBlockedRequest.isValid());
    KisReadLease closeBlockedRead = store.resolve(closeBlockedRequest, closeBlockedRequest.readiness);
    QVERIFY(closeBlockedRead.isValid());
    QVERIFY(!store.closeSession(&error));
    QCOMPARE(error, QStringLiteral("PageStore session still owns capabilities"));
    QVERIFY(store.sessionStats().hasOutstandingCapabilities());
    store.release(std::move(closeBlockedRead));

    QVERIFY2(store.closeSession(&error), qPrintable(error));
    QVERIFY(store.closeSession(&error));
    QVERIFY(!store.isOperational());
    QCOMPARE(provider->memoryUsage().committedBytes, quint64(0));
    stats = store.sessionStats();
    QVERIFY(stats.closed);
    QVERIFY(!stats.operational);
    QVERIFY(!stats.hasOutstandingCapabilities());
    QVERIFY(!store.acquireRead(initialVersion.key, current, cpuAccess, KisPagePriority::Normal).isValid());
}

void KisPageStoreReferenceTest::pageStoreLeaseOutlivesOwnerSafely()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig providerConfig;
    providerConfig.provider = KisReplicaProviderId{63};
    providerConfig.providerEpoch = KisReplicaProviderEpoch{1};
    providerConfig.budgetBytes = 1024 * 1024;
    QString error;
    QVERIFY2(provider->configure(providerConfig, completions, &error), qPrintable(error));

    KisPageAllocationDescriptor descriptor = allocationDescriptor();
    descriptor.initialization = KisPageInitialization::DefaultPixel;
    const KisPageVersion initialVersion = pageVersion(42, 1);
    const KisReplicaOperation initialAllocation = provider->requestReplica(KisPageOperationId{700},
                                                                           initialVersion,
                                                                           descriptor,
                                                                           KisPageAccessDomain::CpuRam,
                                                                           KisPageAccessMode::Read,
                                                                           KisPagePriority::Normal);
    QVERIFY(initialAllocation.isValid());

    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = 1;
    initial.defaultPixelRevision = 1;
    initial.extentRevision = 1;
    initial.propertyRevision = 1;
    initial.manifest = {initialVersion};

    std::unique_ptr<KisReadLease> heldLease;
    {
        KisPageStore store;
        QVERIFY2(store.configure(initial, completions, 4, &error), qPrintable(error));
        QVERIFY(store.registerReplicaProvider(provider));
        QVERIFY2(store.adoptInitialPage(initialVersion, descriptor, initialAllocation.replica, &error),
                 qPrintable(error));
        QVERIFY2(store.finalizeInitialization(&error), qPrintable(error));

        KisPageReadView current;
        const KisReadRequest request = store.acquireRead(initialVersion.key,
                                                         current,
                                                         {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                                                         KisPagePriority::Normal);
        QVERIFY(request.isValid());
        KisReadLease lease = store.resolve(request, request.readiness);
        QVERIFY(lease.isValid());
        heldLease = std::make_unique<KisReadLease>(std::move(lease));
    }

    const KisReplicaOperation blocked = provider->retire(KisPageOperationId{701}, initialAllocation.replica, {});
    QVERIFY(!blocked.isValid());
    QVERIFY(provider->memoryUsage().committedBytes != 0);

    heldLease.reset();
    const KisReplicaOperation retired = provider->retire(KisPageOperationId{702}, initialAllocation.replica, {});
    QVERIFY(retired.isValid());
    QCOMPARE(provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreReferenceTest::pageStoreCallsProviderOutsideGlobalLock()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto delegate = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig providerConfig;
    providerConfig.provider = KisReplicaProviderId{64};
    providerConfig.providerEpoch = KisReplicaProviderEpoch{1};
    providerConfig.budgetBytes = 1024 * 1024;
    QString error;
    QVERIFY2(delegate->configure(providerConfig, completions, &error), qPrintable(error));
    auto provider = std::make_shared<ReentrantProbeCpuProvider>(delegate);

    KisPageAllocationDescriptor descriptor = allocationDescriptor();
    descriptor.initialization = KisPageInitialization::DefaultPixel;
    const KisPageVersion initialVersion = pageVersion(43, 1);
    const KisReplicaOperation initialAllocation = provider->requestReplica(KisPageOperationId{800},
                                                                           initialVersion,
                                                                           descriptor,
                                                                           KisPageAccessDomain::CpuRam,
                                                                           KisPageAccessMode::Read,
                                                                           KisPagePriority::Normal);
    QVERIFY(initialAllocation.isValid());

    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = 1;
    initial.defaultPixelRevision = 1;
    initial.extentRevision = 1;
    initial.propertyRevision = 1;
    initial.manifest = {initialVersion};

    KisPageStore store;
    QVERIFY2(store.configure(initial, completions, 4, &error), qPrintable(error));
    QVERIFY(store.registerReplicaProvider(provider));
    QVERIFY2(store.adoptInitialPage(initialVersion, descriptor, initialAllocation.replica, &error), qPrintable(error));
    QVERIFY2(store.finalizeInitialization(&error), qPrintable(error));

    int prepareCalls = 0;
    int transferCalls = 0;
    int resolveCalls = 0;
    int releaseCalls = 0;
    int retireCalls = 0;
    bool releasingRead = false;
    provider->prepareProbe = [&]() {
        QCOMPARE(store.sessionStats().pendingRequests, qsizetype(1));
        ++prepareCalls;
    };
    provider->transferProbe = [&]() {
        QCOMPARE(store.sessionStats().pendingRequests, qsizetype(1));
        ++transferCalls;
    };
    provider->resolveProbe = [&]() {
        QCOMPARE(store.sessionStats().pendingRequests, qsizetype(1));
        ++resolveCalls;
    };
    provider->releaseProbe = [&]() {
        if (releasingRead) {
            QCOMPARE(store.sessionStats().activeReadLeases, qsizetype(1));
        } else {
            QCOMPARE(store.sessionStats().activeWriteLeases, qsizetype(1));
        }
        ++releaseCalls;
    };
    provider->retireProbe = [&]() {
        QCOMPARE(store.sessionStats().activeProviderCalls, qsizetype(1));
        ++retireCalls;
    };

    const KisPageTransaction transaction = store.beginTransaction(initial.epoch);
    QVERIFY(transaction.isValid());
    const KisWriteRequest request = store.acquireWrite(transaction,
                                                       initialVersion.key,
                                                       {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                                                       KisPageWriteMode::PreserveContents,
                                                       KisPagePriority::Interactive);
    QVERIFY2(request.isValid(), qPrintable(request.error));
    KisWriteLease lease = store.resolve(request, request.readiness);
    QVERIFY(lease.isValid());
    static_cast<quint8 *>(lease.cpuData())[0] = 0x5a;
    store.cancel(std::move(lease));
    QVERIFY(store.abort(transaction));

    KisPageReadView current;
    const KisReadRequest readRequest = store.acquireRead(initialVersion.key,
                                                         current,
                                                         {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                                                         KisPagePriority::Normal);
    QVERIFY2(readRequest.isValid(), qPrintable(readRequest.error));
    KisReadLease readLease = store.resolve(readRequest, readRequest.readiness);
    QVERIFY(readLease.isValid());
    releasingRead = true;
    store.release(std::move(readLease));
    releasingRead = false;

    QCOMPARE(prepareCalls, 1);
    QCOMPARE(transferCalls, 1);
    QCOMPARE(resolveCalls, 2);
    QCOMPARE(releaseCalls, 2);
    QCOMPARE(retireCalls, 1);
    QVERIFY(!store.sessionStats().hasOutstandingCapabilities());
}

void KisPageStoreReferenceTest::pageStoreShutdownRetriesFailedReplicaRetirement()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto delegate = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig providerConfig;
    providerConfig.provider = KisReplicaProviderId{66};
    providerConfig.providerEpoch = KisReplicaProviderEpoch{1};
    providerConfig.budgetBytes = 1024 * 1024;
    QString error;
    QVERIFY2(delegate->configure(providerConfig, completions, &error), qPrintable(error));
    auto provider = std::make_shared<ReentrantProbeCpuProvider>(delegate);

    KisPageAllocationDescriptor descriptor = allocationDescriptor();
    descriptor.initialization = KisPageInitialization::DefaultPixel;
    const KisPageVersion initialVersion = pageVersion(45, 1);
    const KisReplicaOperation initialAllocation = provider->requestReplica(KisPageOperationId{1000},
                                                                           initialVersion,
                                                                           descriptor,
                                                                           KisPageAccessDomain::CpuRam,
                                                                           KisPageAccessMode::Read,
                                                                           KisPagePriority::Normal);
    QVERIFY(initialAllocation.isValid());

    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = 1;
    initial.defaultPixelRevision = 1;
    initial.extentRevision = 1;
    initial.propertyRevision = 1;
    initial.manifest = {initialVersion};

    KisPageStore store;
    QVERIFY2(store.configure(initial, completions, 4, &error), qPrintable(error));
    QVERIFY(store.registerReplicaProvider(provider));
    QVERIFY2(store.adoptInitialPage(initialVersion, descriptor, initialAllocation.replica, &error), qPrintable(error));
    QVERIFY2(store.finalizeInitialization(&error), qPrintable(error));

    provider->failNextRetire = true;
    QVERIFY(!store.closeSession(&error));
    QCOMPARE(error, QStringLiteral("PageStore shutdown retirement is pending"));
    QVERIFY(!store.isOperational());
    KisPageStoreSessionStats stats = store.sessionStats();
    QVERIFY(!stats.closed);
    QCOMPARE(stats.pendingShutdownReplicas, qsizetype(1));
    QVERIFY(delegate->memoryUsage().committedBytes != 0);

    QVERIFY2(store.closeSession(&error), qPrintable(error));
    stats = store.sessionStats();
    QVERIFY(stats.closed);
    QCOMPARE(stats.pendingShutdownReplicas, qsizetype(0));
    QVERIFY(!stats.hasOutstandingCapabilities());
    QCOMPARE(delegate->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreReferenceTest::cpuAccessSessionScopesOldAndWritablePageLeases()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig providerConfig;
    providerConfig.provider = KisReplicaProviderId{65};
    providerConfig.providerEpoch = KisReplicaProviderEpoch{1};
    providerConfig.budgetBytes = 1024 * 1024;
    QString error;
    QVERIFY2(provider->configure(providerConfig, completions, &error), qPrintable(error));

    KisPageAllocationDescriptor descriptor = allocationDescriptor();
    descriptor.initialization = KisPageInitialization::DefaultPixel;
    const KisPageVersion initialVersion = pageVersion(44, 1);
    const KisReplicaOperation initialAllocation = provider->requestReplica(KisPageOperationId{900},
                                                                           initialVersion,
                                                                           descriptor,
                                                                           KisPageAccessDomain::CpuRam,
                                                                           KisPageAccessMode::Read,
                                                                           KisPagePriority::Normal);
    QVERIFY(initialAllocation.isValid());

    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = 1;
    initial.defaultPixelRevision = 1;
    initial.extentRevision = 1;
    initial.propertyRevision = 1;
    initial.manifest = {initialVersion};

    KisPageStore store;
    QVERIFY2(store.configure(initial, completions, 4, &error), qPrintable(error));
    QVERIFY(store.registerReplicaProvider(provider));
    QVERIFY2(store.adoptInitialPage(initialVersion, descriptor, initialAllocation.replica, &error), qPrintable(error));
    QVERIFY2(store.finalizeInitialization(&error), qPrintable(error));

    const KisRetainedImageEpochSnapshot retainedInitial = store.captureRetainedEpoch();
    QVERIFY(retainedInitial.isValid());

    const KisPageTransaction transaction = store.beginTransaction(initial.epoch);
    QVERIFY(transaction.isValid());
    KisPageStoreCpuAccessSession writeSession;
    QVERIFY2(writeSession.beginWrite(&store,
                                     initialVersion.key.surface,
                                     transaction,
                                     KisPageWriteMode::PreserveContents,
                                     KisPagePriority::Interactive,
                                     &error),
             qPrintable(error));
    KisCpuPageWriteSpan write =
        writeSession.writePage(initialVersion.key.page.column, initialVersion.key.page.row, &error);
    QVERIFY2(write.isValid(), qPrintable(error));
    QCOMPARE(write.baseVersion.generation.value, quint64(1));
    QCOMPARE(write.version.generation.value, quint64(2));
    QCOMPARE(write.oldData[0], quint8(0));
    write.data[0] = 0xa5;
    write.data[1] = 0x5a;
    const KisCpuPageWriteSpan cached =
        writeSession.writePage(initialVersion.key.page.column, initialVersion.key.page.row, &error);
    QCOMPARE(cached.data, write.data);
    QCOMPARE(cached.version, write.version);
    QVERIFY2(writeSession.finish(&error), qPrintable(error));

    const KisPreparedPageSet prepared = store.preparedPages(transaction);
    QVERIFY(prepared.isValid());
    const KisImageEpochCommitTicket committed = store.commit(transaction, prepared);
    QVERIFY(committed.isValid());
    const KisRetainedImageEpochSnapshot retainedCommitted = store.captureRetainedEpoch();
    QVERIFY(retainedCommitted.isValid());

    KisPageStoreCpuAccessSession readSession;
    KisPageReadView current;
    QVERIFY2(readSession.beginRead(&store, initialVersion.key.surface, current, KisPagePriority::Normal, &error),
             qPrintable(error));
    const KisCpuPageReadSpan read =
        readSession.readPage(initialVersion.key.page.column, initialVersion.key.page.row, &error);
    QVERIFY2(read.isValid(), qPrintable(error));
    QCOMPARE(read.version.generation.value, quint64(2));
    QCOMPARE(read.data[0], quint8(0xa5));
    QCOMPARE(read.data[1], quint8(0x5a));
    QVERIFY2(readSession.finish(&error), qPrintable(error));

    const KisImageEpochCommitTicket undone = store.restoreRetainedEpoch(retainedInitial);
    QVERIFY(undone.isValid());
    QCOMPARE(undone.epoch.value, quint64(3));
    KisPageStoreCpuAccessSession undoReadSession;
    QVERIFY(undoReadSession.beginRead(&store, initialVersion.key.surface, current, KisPagePriority::Normal, &error));
    const KisCpuPageReadSpan undoneRead =
        undoReadSession.readPage(initialVersion.key.page.column, initialVersion.key.page.row, &error);
    QVERIFY(undoneRead.isValid());
    QCOMPARE(undoneRead.version.generation.value, quint64(1));
    QCOMPARE(undoneRead.data[0], quint8(0));
    QVERIFY(undoReadSession.finish(&error));

    const KisImageEpochCommitTicket redone = store.restoreRetainedEpoch(retainedCommitted);
    QVERIFY(redone.isValid());
    QCOMPARE(redone.epoch.value, quint64(4));

    const KisPageTransaction cancelled = store.beginTransaction(redone.epoch);
    QVERIFY(cancelled.isValid());
    {
        KisPageStoreCpuAccessSession abandoned;
        QVERIFY(abandoned.beginWrite(&store,
                                     initialVersion.key.surface,
                                     cancelled,
                                     KisPageWriteMode::DiscardContents,
                                     KisPagePriority::Normal,
                                     &error));
        QVERIFY(abandoned.writePage(initialVersion.key.page.column, initialVersion.key.page.row, &error).isValid());
    }
    QVERIFY(store.abort(cancelled));
    QVERIFY(store.releaseSnapshot(retainedInitial.token));
    QVERIFY(store.releaseSnapshot(retainedCommitted.token));
    QVERIFY(!store.sessionStats().hasOutstandingCapabilities());
}

void KisPageStoreReferenceTest::pageStoreMementoOwnsBeforeAfterRetentionAndPurge()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig providerConfig;
    providerConfig.provider = KisReplicaProviderId{76};
    providerConfig.providerEpoch = KisReplicaProviderEpoch{1};
    providerConfig.budgetBytes = 1024 * 1024;
    QString error;
    QVERIFY2(provider->configure(providerConfig, completions, &error), qPrintable(error));

    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = 1;
    initial.defaultPixelRevision = 1;
    initial.extentRevision = 1;
    initial.propertyRevision = 1;
    initial.surfaces = {surfaceEpochState(KisSurfaceId{1}, 1, 0x11)};

    KisPageStore store;
    QVERIFY2(store.configure(initial, completions, 8, &error), qPrintable(error));
    QVERIFY(store.registerReplicaProvider(provider));
    QVERIFY2(store.finalizeInitialization(&error), qPrintable(error));

    KisPageStoreMementoManager history;
    QVERIFY2(history.configure(&store, &error), qPrintable(error));
    const KisPageStoreHistoryTransaction edit = history.begin(&error);
    QVERIFY2(edit.isValid(), qPrintable(error));

    KisPageStoreRandomAccessor writer(&store, KisSurfaceId{1}, edit.transaction, KisPageWriteMode::PreserveContents);
    QVERIFY2(writer.isValid(), qPrintable(writer.error()));
    writer.moveTo(70, -1);
    QVERIFY(writer.rawData());
    QCOMPARE(writer.oldRawData()[0], quint8(0x11));
    writer.rawData()[0] = 0x77;
    QVERIFY2(writer.finish(&error), qPrintable(error));

    KisPageStoreRandomAccessor secondWriter(&store,
                                            KisSurfaceId{1},
                                            edit.transaction,
                                            KisPageWriteMode::PreserveContents);
    QVERIFY2(secondWriter.isValid(), qPrintable(secondWriter.error()));
    secondWriter.moveTo(70, -1);
    QVERIFY(secondWriter.rawData());
    QCOMPARE(secondWriter.rawData()[0], quint8(0x77));
    QCOMPARE(secondWriter.oldRawData()[0], quint8(0x11));
    secondWriter.rawData()[0] = 0x78;
    QVERIFY2(secondWriter.finish(&error), qPrintable(error));

    KisSurfaceEpochState changed = initial.surfaces.first();
    changed.format.defaultPixel = QByteArray(1, char(0x22));
    changed.contentExtent = QRect(-96, -64, 768, 640);
    changed.defaultPixelRevision = 2;
    changed.extentRevision = 2;
    QVERIFY2(store.stageSurfaceMetadata(edit.transaction, changed, &error), qPrintable(error));

    const KisPageStoreMemento memento = history.commit(edit, &error);
    QVERIFY2(memento.isValid(), qPrintable(error));
    QCOMPARE(store.sessionStats().retainedSnapshots, qsizetype(2));

    auto currentPixel = [&]() -> int {
        KisPageStoreRandomAccessor reader(&store, KisSurfaceId{1}, {});
        if (!reader.isValid())
            return -1;
        reader.moveTo(70, -1);
        const quint8 *data = reader.rawDataConst();
        const int value = data ? data[0] : -1;
        reader.finish();
        return value;
    };
    QCOMPARE(currentPixel(), 0x78);

    const KisImageEpochCommitTicket undone = history.rollback(memento, &error);
    QVERIFY2(undone.isValid(), qPrintable(error));
    QCOMPARE(currentPixel(), 0x11);
    KisSurfaceEpochState currentSurface;
    QVERIFY(store.resolveSurfaceState(KisSurfaceId{1}, {}, &currentSurface));
    QCOMPARE(currentSurface.defaultPixelRevision, quint64(1));
    QCOMPARE(currentSurface.extentRevision, quint64(1));

    const KisImageEpochCommitTicket redone = history.rollforward(memento, &error);
    QVERIFY2(redone.isValid(), qPrintable(error));
    QCOMPARE(currentPixel(), 0x78);
    QVERIFY(store.resolveSurfaceState(KisSurfaceId{1}, {}, &currentSurface));
    QCOMPARE(currentSurface.format.defaultPixel, QByteArray(1, char(0x22)));
    QCOMPARE(currentSurface.contentExtent, QRect(-96, -64, 768, 640));

    QVERIFY(!history.rollback(KisPageStoreMemento{memento.capability + 1}, &error).isValid());
    QVERIFY2(history.purge(memento, &error), qPrintable(error));
    QCOMPARE(store.sessionStats().retainedSnapshots, qsizetype(0));

    const KisPageStoreHistoryTransaction noOp = history.begin(&error);
    QVERIFY(noOp.isValid());
    const KisPageStoreMemento noOpMemento = history.commit(noOp, &error);
    QVERIFY2(noOpMemento.isValid(), qPrintable(error));
    QCOMPARE(store.sessionStats().retainedSnapshots, qsizetype(1));
    QVERIFY(history.rollback(noOpMemento, &error).isValid());
    QVERIFY(history.rollforward(noOpMemento, &error).isValid());
    QVERIFY2(history.purge(noOpMemento, &error), qPrintable(error));

    const KisPageStoreHistoryTransaction abandoned = history.begin(&error);
    QVERIFY(abandoned.isValid());
    QVERIFY2(history.abort(abandoned, &error), qPrintable(error));
    QVERIFY2(history.close(&error), qPrintable(error));
    QVERIFY2(store.closeSession(&error), qPrintable(error));
    QCOMPARE(provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreReferenceTest::cpuSurfaceOpsPreserveRectCopyAndSparseClear()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig providerConfig;
    providerConfig.provider = KisReplicaProviderId{77};
    providerConfig.providerEpoch = KisReplicaProviderEpoch{1};
    providerConfig.budgetBytes = 8 * 1024 * 1024;
    QString error;
    QVERIFY2(provider->configure(providerConfig, completions, &error), qPrintable(error));

    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = 1;
    initial.defaultPixelRevision = 1;
    initial.extentRevision = 1;
    initial.propertyRevision = 1;
    initial.surfaces = {surfaceEpochState(KisSurfaceId{1}, 1, 0x19), surfaceEpochState(KisSurfaceId{2}, 1, 0x19)};

    KisPageStore store;
    QVERIFY2(store.configure(initial, completions, 8, &error), qPrintable(error));
    QVERIFY(store.registerReplicaProvider(provider));
    QVERIFY2(store.finalizeInitialization(&error), qPrintable(error));

    const QRect sourceRect(-2, -1, 68, 3);
    QByteArray pattern(sourceRect.width() * sourceRect.height(), char(0));
    for (qsizetype i = 0; i < pattern.size(); ++i) {
        pattern[i] = char((i * 17 + 3) & 0xff);
    }
    KisPageTransaction transaction = store.beginTransaction(initial.epoch);
    QVERIFY(transaction.isValid());
    QVERIFY2(KisPageStoreCpuSurfaceOps::writeRect(&store, KisSurfaceId{1}, transaction, sourceRect, pattern, 0, &error),
             qPrintable(error));
    KisImageEpochCommitTicket committed = store.commit(transaction, store.preparedPages(transaction));
    QVERIFY(committed.isValid());

    QByteArray readBack;
    QVERIFY2(KisPageStoreCpuSurfaceOps::readRect(&store, KisSurfaceId{1}, {}, sourceRect, &readBack, 0, &error),
             qPrintable(error));
    QCOMPARE(readBack, pattern);

    transaction = store.beginTransaction(committed.epoch);
    QVERIFY(transaction.isValid());
    const QPoint destinationTopLeft(-70, 60);
    QVERIFY2(KisPageStoreCpuSurfaceOps::copyRect(&store,
                                                 KisSurfaceId{1},
                                                 {},
                                                 sourceRect,
                                                 KisSurfaceId{2},
                                                 transaction,
                                                 destinationTopLeft,
                                                 &error),
             qPrintable(error));
    committed = store.commit(transaction, store.preparedPages(transaction));
    QVERIFY(committed.isValid());
    QByteArray copied;
    QVERIFY2(KisPageStoreCpuSurfaceOps::readRect(&store,
                                                 KisSurfaceId{2},
                                                 {},
                                                 QRect(destinationTopLeft, sourceRect.size()),
                                                 &copied,
                                                 0,
                                                 &error),
             qPrintable(error));
    QCOMPARE(copied, pattern);

    transaction = store.beginTransaction(committed.epoch);
    QVERIFY(transaction.isValid());
    QVERIFY2(KisPageStoreCpuSurfaceOps::fillRect(&store,
                                                 KisSurfaceId{1},
                                                 transaction,
                                                 QRect(0, 0, 64, 64),
                                                 QByteArray(1, char(0x19)),
                                                 &error),
             qPrintable(error));
    committed = store.commit(transaction, store.preparedPages(transaction));
    QVERIFY(committed.isValid());
    bool pageWasPurged = true;
    for (const KisPageVersion &version : store.captureCommittedEpoch().manifest) {
        if (version.key == pageKey(0, 0))
            pageWasPurged = false;
    }
    QVERIFY(pageWasPurged);
    QByteArray cleared;
    QVERIFY2(KisPageStoreCpuSurfaceOps::readRect(&store, KisSurfaceId{1}, {}, QRect(0, 0, 64, 64), &cleared, 0, &error),
             qPrintable(error));
    QCOMPARE(cleared, QByteArray(64 * 64, char(0x19)));

    QVERIFY2(store.closeSession(&error), qPrintable(error));
    QCOMPARE(provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreReferenceTest::sparseDefaultPageFirstWriteAndUndoStayVersioned()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig providerConfig;
    providerConfig.provider = KisReplicaProviderId{66};
    providerConfig.providerEpoch = KisReplicaProviderEpoch{1};
    providerConfig.budgetBytes = 1024 * 1024;
    QString error;
    QVERIFY2(provider->configure(providerConfig, completions, &error), qPrintable(error));

    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = 1;
    initial.defaultPixelRevision = 7;
    initial.extentRevision = 1;
    initial.propertyRevision = 1;
    initial.surfaces = {surfaceEpochState(KisSurfaceId{1}, 7, 0x2a)};
    QVERIFY(initial.isValid());
    QVERIFY(initial.manifest.isEmpty());

    KisPageStore store;
    QVERIFY2(store.configure(initial, completions, 4, &error), qPrintable(error));
    QVERIFY(store.registerReplicaProvider(provider));
    QVERIFY2(store.finalizeInitialization(&error), qPrintable(error));

    const KisPageKey sparseKey = pageKey(17, -3);
    KisPageReadView current;
    KisPageVersion resolvedVersion;
    QVERIFY(store.resolvePageVersion(sparseKey, current, &resolvedVersion));
    QVERIFY(resolvedVersion.isDefaultPixel());
    QCOMPARE(resolvedVersion.defaultPixelRevision, quint64(7));
    const KisPageAccessRequirement cpuAccess{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
    KisReadRequest defaultRequest = store.acquireRead(sparseKey, current, cpuAccess, KisPagePriority::Normal);
    QVERIFY2(defaultRequest.isValid(), qPrintable(defaultRequest.error));
    QVERIFY(defaultRequest.version.isDefaultPixel());
    QCOMPARE(defaultRequest.version.generation.value, quint64(1));
    QCOMPARE(defaultRequest.version.defaultPixelRevision, quint64(7));
    KisReadLease defaultRead = store.resolve(defaultRequest, defaultRequest.readiness);
    QVERIFY(defaultRead.isValid());
    QCOMPARE(static_cast<const quint8 *>(defaultRead.cpuData())[0], quint8(0x2a));
    store.release(std::move(defaultRead));

    const KisRetainedImageEpochSnapshot retainedInitial = store.captureRetainedEpoch();
    QVERIFY(retainedInitial.isValid());
    const KisPageTransaction transaction = store.beginTransaction(initial.epoch);
    QVERIFY(transaction.isValid());
    KisPageStoreCpuAccessSession writeSession;
    QVERIFY2(writeSession.beginWrite(&store,
                                     sparseKey.surface,
                                     transaction,
                                     KisPageWriteMode::PreserveContents,
                                     KisPagePriority::Interactive,
                                     &error),
             qPrintable(error));
    KisCpuPageWriteSpan write = writeSession.writePage(sparseKey.page.column, sparseKey.page.row, &error);
    QVERIFY2(write.isValid(), qPrintable(error));
    QVERIFY(write.baseVersion.isDefaultPixel());
    QCOMPARE(write.oldData[0], quint8(0x2a));
    QCOMPARE(write.version.generation.value, quint64(2));
    QVERIFY(!write.version.isDefaultPixel());
    write.data[0] = 0x9c;
    QVERIFY2(writeSession.finish(&error), qPrintable(error));

    const KisPreparedPageSet prepared = store.preparedPages(transaction);
    QVERIFY(prepared.isValid());
    const KisImageEpochCommitTicket committed = store.commit(transaction, prepared);
    QVERIFY(committed.isValid());
    QCOMPARE(store.captureCommittedEpoch().manifest.size(), qsizetype(1));
    const KisRetainedImageEpochSnapshot retainedWritten = store.captureRetainedEpoch();
    QVERIFY(retainedWritten.isValid());

    const KisPageTransaction revived = store.beginTransaction(committed.epoch);
    QVERIFY(revived.isValid());
    QVERIFY2(KisPageStoreCpuSurfaceOps::removePage(&store, revived, sparseKey, &error), qPrintable(error));
    KisSurfaceEpochState overlaySurface = initial.surfaces.first();
    ++overlaySurface.defaultPixelRevision;
    overlaySurface.format.defaultPixel = QByteArray(1, char(0x55));
    QVERIFY2(store.stageSurfaceMetadata(revived, overlaySurface, &error), qPrintable(error));
    KisPageReadView revivedOverlay;
    revivedOverlay.kind = KisPageReadViewKind::TransactionOverlay;
    revivedOverlay.transaction = revived.id;
    QVERIFY(store.resolvePageVersion(sparseKey, revivedOverlay, &resolvedVersion));
    QVERIFY(resolvedVersion.isDefaultPixel());
    QCOMPARE(resolvedVersion.defaultPixelRevision, quint64(8));
    const KisReadRequest removedOverlayRequest =
        store.acquireRead(sparseKey, revivedOverlay, cpuAccess, KisPagePriority::Normal);
    QVERIFY2(removedOverlayRequest.isValid(), qPrintable(removedOverlayRequest.error));
    QCOMPARE(removedOverlayRequest.version, resolvedVersion);
    KisReadLease removedOverlayRead = store.resolve(removedOverlayRequest, removedOverlayRequest.readiness);
    QVERIFY(removedOverlayRead.isValid());
    QCOMPARE(static_cast<const quint8 *>(removedOverlayRead.cpuData())[0], quint8(0x55));
    store.release(std::move(removedOverlayRead));
    const KisPageKey virginKey = pageKey(18, -3);
    QVERIFY(store.resolvePageVersion(virginKey, revivedOverlay, &resolvedVersion));
    QCOMPARE(resolvedVersion.defaultPixelRevision, quint64(8));
    const KisReadRequest virginOverlayRequest =
        store.acquireRead(virginKey, revivedOverlay, cpuAccess, KisPagePriority::Normal);
    QVERIFY2(virginOverlayRequest.isValid(), qPrintable(virginOverlayRequest.error));
    QCOMPARE(virginOverlayRequest.version, resolvedVersion);
    KisReadLease virginOverlayRead = store.resolve(virginOverlayRequest, virginOverlayRequest.readiness);
    QVERIFY(virginOverlayRead.isValid());
    QCOMPARE(static_cast<const quint8 *>(virginOverlayRead.cpuData())[0], quint8(0x55));
    store.release(std::move(virginOverlayRead));
    const KisWriteRequest revivedRequest =
        store.acquireWrite(revived, sparseKey, cpuAccess, KisPageWriteMode::PreserveContents, KisPagePriority::Normal);
    QVERIFY2(revivedRequest.isValid(), qPrintable(revivedRequest.error));
    QCOMPARE(revivedRequest.mode, KisPageWriteMode::DiscardContents);
    KisWriteLease revivedWrite = store.resolve(revivedRequest, revivedRequest.readiness);
    QVERIFY(revivedWrite.isValid());
    QCOMPARE(static_cast<const quint8 *>(revivedWrite.cpuData())[0], quint8(0x55));
    static_cast<quint8 *>(revivedWrite.cpuData())[0] = 0x7d;
    QVERIFY(store.publishHostWrite(std::move(revivedWrite)).isValid());
    const KisPreparedPageSet revivedPrepared = store.preparedPages(revived);
    QVERIFY(revivedPrepared.isValid());
    QCOMPARE(revivedPrepared.proofs.size(), qsizetype(1));
    QVERIFY(revivedPrepared.removedPages.isEmpty());
    QVERIFY(store.abort(revived));

    const KisPageTransaction removal = store.beginTransaction(committed.epoch);
    QVERIFY(removal.isValid());
    QVERIFY2(KisPageStoreCpuSurfaceOps::removePage(&store, removal, sparseKey, &error), qPrintable(error));
    const KisPreparedPageSet removalPrepared = store.preparedPages(removal);
    QVERIFY(removalPrepared.isValid());
    QVERIFY(removalPrepared.proofs.isEmpty());
    QCOMPARE(removalPrepared.removedPages, QVector<KisPageKey>{sparseKey});
    const KisImageEpochCommitTicket removed = store.commit(removal, removalPrepared);
    QVERIFY(removed.isValid());
    QCOMPARE(removed.epoch.value, quint64(3));
    QVERIFY(store.captureCommittedEpoch().manifest.isEmpty());
    const KisRetainedImageEpochSnapshot retainedRemoved = store.captureRetainedEpoch();
    QVERIFY(retainedRemoved.isValid());

    KisReadRequest removedRequest = store.acquireRead(sparseKey, current, cpuAccess, KisPagePriority::Normal);
    QVERIFY(removedRequest.isValid());
    QVERIFY(removedRequest.version.isDefaultPixel());
    KisReadLease removedRead = store.resolve(removedRequest, removedRequest.readiness);
    QVERIFY(removedRead.isValid());
    QCOMPARE(static_cast<const quint8 *>(removedRead.cpuData())[0], quint8(0x2a));
    store.release(std::move(removedRead));

    const KisImageEpochCommitTicket restoredWritten = store.restoreRetainedEpoch(retainedWritten);
    QVERIFY(restoredWritten.isValid());
    KisReadRequest writtenRequest = store.acquireRead(sparseKey, current, cpuAccess, KisPagePriority::Normal);
    QVERIFY(writtenRequest.isValid());
    KisReadLease writtenRead = store.resolve(writtenRequest, writtenRequest.readiness);
    QVERIFY(writtenRead.isValid());
    QCOMPARE(static_cast<const quint8 *>(writtenRead.cpuData())[0], quint8(0x9c));
    store.release(std::move(writtenRead));

    const KisImageEpochCommitTicket restoredRemoved = store.restoreRetainedEpoch(retainedRemoved);
    QVERIFY(restoredRemoved.isValid());
    QVERIFY(store.captureCommittedEpoch().manifest.isEmpty());

    const KisImageEpochCommitTicket restoredInitial = store.restoreRetainedEpoch(retainedInitial);
    QVERIFY(restoredInitial.isValid());
    QVERIFY(store.captureCommittedEpoch().manifest.isEmpty());

    KisReadRequest restoredRequest = store.acquireRead(sparseKey, current, cpuAccess, KisPagePriority::Normal);
    QVERIFY(restoredRequest.isValid());
    QVERIFY(restoredRequest.version.isDefaultPixel());
    QCOMPARE(restoredRequest.version.defaultPixelRevision, quint64(7));
    KisReadLease restoredRead = store.resolve(restoredRequest, restoredRequest.readiness);
    QVERIFY(restoredRead.isValid());
    QCOMPARE(static_cast<const quint8 *>(restoredRead.cpuData())[0], quint8(0x2a));
    store.release(std::move(restoredRead));

    QVERIFY(store.releaseSnapshot(retainedInitial.token));
    QVERIFY(store.releaseSnapshot(retainedWritten.token));
    QVERIFY(store.releaseSnapshot(retainedRemoved.token));
    QVERIFY(!store.sessionStats().hasOutstandingCapabilities());
}

void KisPageStoreReferenceTest::virtualDefaultRemovalPreservesReaderAndDefaultRevision()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig config;
    config.provider = KisReplicaProviderId{166};
    config.providerEpoch = KisReplicaProviderEpoch{1};
    config.budgetBytes = 1024 * 1024;
    QString error;
    QVERIFY2(provider->configure(config, completions, &error), qPrintable(error));
    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = initial.extentRevision = initial.propertyRevision = 1;
    initial.defaultPixelRevision = 7;
    initial.surfaces = {surfaceEpochState(KisSurfaceId{1}, 7, 0x2a)};
    KisPageStore store;
    QVERIFY(store.configure(initial, completions, 4, &error));
    QVERIFY(store.registerReplicaProvider(provider));
    QVERIFY(store.finalizeInitialization(&error));
    const auto key = pageKey(0);
    const KisPageAccessRequirement access{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
    auto transaction = store.beginTransaction(initial.epoch);
    QVERIFY2(KisPageStoreCpuSurfaceOps::fillRect(&store,
                                                 key.surface,
                                                 transaction,
                                                 QRect(0, 0, 64, 64),
                                                 QByteArray(1, char(0x9c)),
                                                 &error),
             qPrintable(error));
    auto committed = store.commit(transaction, store.preparedPages(transaction));
    QVERIFY(committed.isValid());
    const auto written = store.captureRetainedEpoch();
    const auto request = store.acquireRead(key, {}, access, KisPagePriority::Normal);
    QVERIFY(request.isValid());
    auto reader = store.resolve(request, request.readiness);
    QVERIFY(reader.isValid());
    const auto before = store.sessionStats();

    transaction = store.beginTransaction(committed.epoch);
    QVERIFY(KisPageStoreCpuSurfaceOps::removePage(&store, transaction, key, &error));
    committed = store.commit(transaction, store.preparedPages(transaction));
    QVERIFY(committed.isValid());
    QCOMPARE(store.sessionStats().defaultMaterializationRequests, before.defaultMaterializationRequests);
    QCOMPARE(static_cast<const quint8 *>(reader.cpuData())[0], quint8(0x9c));
    const auto removed = store.captureRetainedEpoch();

    KisSurfaceEpochState changed;
    QVERIFY(store.resolveSurfaceState(key.surface, {}, &changed));
    changed.format.defaultPixel = QByteArray(1, char(0x55));
    changed.defaultPixelRevision++;
    transaction = store.beginTransaction(committed.epoch);
    QVERIFY(store.stageSurfaceMetadata(transaction, changed, &error));
    committed = store.commit(transaction, store.preparedPages(transaction));
    QVERIFY(committed.isValid());
    QCOMPARE(store.sessionStats().defaultMaterializationRequests, before.defaultMaterializationRequests);

    std::vector<KisReadLease> pendingReads;
    auto checkRead = [&](const KisPageReadView &view, quint8 expected) {
        const auto readRequest = store.acquireRead(key, view, access, KisPagePriority::Normal);
        QVERIFY2(readRequest.isValid(), qPrintable(readRequest.error));
        auto lease = store.resolve(readRequest, readRequest.readiness);
        QVERIFY(lease.isValid());
        QCOMPARE(static_cast<const quint8 *>(lease.cpuData())[0], expected);
        pendingReads.emplace_back(std::move(lease));
    };
    const auto beforeDefaultReads = kisPageStoreMetadataMetrics(store);
    checkRead({}, 0x55);
    KisPageReadView historical;
    historical.kind = KisPageReadViewKind::CommittedEpoch;
    historical.epoch = removed.snapshot.epoch;
    historical.retention = removed.token;
    checkRead(historical, 0x2a);
    const auto afterDefaultReads = kisPageStoreMetadataMetrics(store);
    QCOMPARE(afterDefaultReads.fullSnapshotExports - afterDefaultReads.backgroundSnapshotExports,
             beforeDefaultReads.fullSnapshotExports - beforeDefaultReads.backgroundSnapshotExports);
    QCOMPARE(afterDefaultReads.mutationBaseVersionInputs - afterDefaultReads.backgroundMutationBaseVersionInputs,
             beforeDefaultReads.mutationBaseVersionInputs - beforeDefaultReads.backgroundMutationBaseVersionInputs);
    for (auto &lease : pendingReads) store.release(std::move(lease));
    pendingReads.clear();
    const auto afterReadRelease = kisPageStoreMetadataMetrics(store);
    QCOMPARE(afterReadRelease.fullSnapshotExports - afterReadRelease.backgroundSnapshotExports,
             beforeDefaultReads.fullSnapshotExports - beforeDefaultReads.backgroundSnapshotExports);
    QVERIFY(afterReadRelease.maximumHistorySliceVersionInputs <= 32);
    QCOMPARE(store.sessionStats().defaultMaterializationRequests, before.defaultMaterializationRequests + 2);
    QCOMPARE(static_cast<const quint8 *>(reader.cpuData())[0], quint8(0x9c));
    store.release(std::move(reader));
    QVERIFY(store.restoreRetainedEpoch(written).isValid());
    checkRead({}, 0x9c);
    for (auto &lease : pendingReads) store.release(std::move(lease));
    pendingReads.clear();
    QVERIFY(store.restoreRetainedEpoch(removed).isValid());
    checkRead({}, 0x2a);
    for (auto &lease : pendingReads) store.release(std::move(lease));
    QVERIFY(store.releaseSnapshot(written.token));
    QVERIFY(store.releaseSnapshot(removed.token));
    QVERIFY2(store.closeSession(&error), qPrintable(error));
    QCOMPARE(provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreReferenceTest::surfaceDefaultAndExtentMutateAtomicallyWithEpoch()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig providerConfig;
    providerConfig.provider = KisReplicaProviderId{67};
    providerConfig.providerEpoch = KisReplicaProviderEpoch{1};
    providerConfig.budgetBytes = 1024 * 1024;
    QString error;
    QVERIFY2(provider->configure(providerConfig, completions, &error), qPrintable(error));

    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = 1;
    initial.defaultPixelRevision = 7;
    initial.extentRevision = 1;
    initial.propertyRevision = 1;
    initial.surfaces = {surfaceEpochState(KisSurfaceId{1}, 7, 0x2a)};

    KisPageStore store;
    QVERIFY2(store.configure(initial, completions, 4, &error), qPrintable(error));
    QVERIFY(store.registerReplicaProvider(provider));
    QVERIFY2(store.finalizeInitialization(&error), qPrintable(error));
    const KisPageAccessRequirement cpuAccess{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
    KisPageReadView current;

    const KisPageKey firstKey = pageKey(2, 3);
    KisReadRequest initialReadRequest = store.acquireRead(firstKey, current, cpuAccess, KisPagePriority::Normal);
    QVERIFY(initialReadRequest.isValid());
    KisReadLease initialRead = store.resolve(initialReadRequest, initialReadRequest.readiness);
    QVERIFY(initialRead.isValid());
    QCOMPARE(static_cast<const quint8 *>(initialRead.cpuData())[0], quint8(0x2a));
    store.release(std::move(initialRead));
    const KisRetainedImageEpochSnapshot retainedInitial = store.captureRetainedEpoch();
    QVERIFY(retainedInitial.isValid());

    const KisPageTransaction metadataTransaction = store.beginTransaction(initial.epoch);
    QVERIFY(metadataTransaction.isValid());
    KisSurfaceEpochState changed = initial.surfaces.first();
    changed.format.defaultPixel = QByteArray(1, char(0x55));
    changed.defaultPixelRevision = 8;
    changed.contentExtent = QRect(-128, -32, 640, 480);
    changed.extentRevision = 2;
    QVERIFY2(store.stageSurfaceMetadata(metadataTransaction, changed, &error), qPrintable(error));
    const KisPreparedPageSet prepared = store.preparedPages(metadataTransaction);
    QVERIFY(prepared.isValid());
    QVERIFY(prepared.proofs.isEmpty());
    QCOMPARE(prepared.surfaceChanges.size(), qsizetype(1));
    const KisImageEpochCommitTicket committed = store.commit(metadataTransaction, prepared);
    QVERIFY(committed.isValid());
    QCOMPARE(committed.epoch.value, quint64(2));
    const KisImageEpochSnapshot committedSnapshot = store.captureCommittedEpoch();
    QCOMPARE(committedSnapshot.surfaces.first(), changed);
    QCOMPARE(committedSnapshot.defaultPixelRevision, quint64(8));
    QCOMPARE(committedSnapshot.extentRevision, quint64(2));
    const KisRetainedImageEpochSnapshot retainedChanged = store.captureRetainedEpoch();
    QVERIFY(retainedChanged.isValid());

    auto readFirstByte = [&](const KisPageKey &key, const KisPageReadView &view) -> int {
        KisReadRequest request = store.acquireRead(key, view, cpuAccess, KisPagePriority::Normal);
        if (!request.isValid())
            return -1;
        KisReadLease lease = store.resolve(request, request.readiness);
        if (!lease.isValid())
            return -1;
        const int value = static_cast<const quint8 *>(lease.cpuData())[0];
        store.release(std::move(lease));
        return value;
    };

    QCOMPARE(readFirstByte(firstKey, current), 0x55);
    const KisPageKey secondKey = pageKey(-8, 9);
    QCOMPARE(readFirstByte(secondKey, current), 0x55);

    KisPageReadView historicalInitial;
    historicalInitial.kind = KisPageReadViewKind::CommittedEpoch;
    historicalInitial.epoch = retainedInitial.snapshot.epoch;
    historicalInitial.retention = retainedInitial.token;
    QCOMPARE(readFirstByte(firstKey, historicalInitial), 0x2a);
    QCOMPARE(readFirstByte(secondKey, historicalInitial), 0x2a);
    KisPageStoreRandomAccessor historicalAccessor(&store, firstKey.surface, historicalInitial);
    QVERIFY2(historicalAccessor.isValid(), qPrintable(historicalAccessor.error()));
    historicalAccessor.moveTo(firstKey.page.column * 64, firstKey.page.row * 64);
    QVERIFY(historicalAccessor.rawDataConst());
    QCOMPARE(historicalAccessor.rawDataConst()[0], quint8(0x2a));
    QVERIFY(historicalAccessor.finish(&error));

    const KisImageEpochCommitTicket undone = store.restoreRetainedEpoch(retainedInitial);
    QVERIFY(undone.isValid());
    QCOMPARE(readFirstByte(firstKey, current), 0x2a);
    QCOMPARE(readFirstByte(secondKey, current), 0x2a);
    const KisImageEpochCommitTicket redone = store.restoreRetainedEpoch(retainedChanged);
    QVERIFY(redone.isValid());
    QCOMPARE(readFirstByte(firstKey, current), 0x55);
    QCOMPARE(readFirstByte(secondKey, current), 0x55);

    QVERIFY(store.releaseSnapshot(retainedInitial.token));
    QVERIFY(store.releaseSnapshot(retainedChanged.token));
    QVERIFY(!store.sessionStats().hasOutstandingCapabilities());
}

void KisPageStoreReferenceTest::exactGenerationArchivePreservesPublishedAuthorityOnFailure()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig providerConfig;
    providerConfig.provider = KisReplicaProviderId{69};
    providerConfig.providerEpoch = KisReplicaProviderEpoch{1};
    providerConfig.budgetBytes = 1024 * 1024;
    QString error;
    QVERIFY2(provider->configure(providerConfig, completions, &error), qPrintable(error));

    auto archive = std::make_shared<KisUnifiedSsdPageStore>();
    KisLegacySwapArchiveConfig archiveConfig;
    archiveConfig.archive = KisReplicaProviderId{70};
    archiveConfig.epoch = KisReplicaProviderEpoch{1};
    archiveConfig.byteBudget = 1024 * 1024;
    QVERIFY2(archive->configureLegacySwapArchive(archiveConfig, completions, &error), qPrintable(error));

    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = 1;
    initial.defaultPixelRevision = 1;
    initial.extentRevision = 1;
    initial.propertyRevision = 1;
    initial.surfaces = {surfaceEpochState(KisSurfaceId{1}, 1, 0x2a)};

    KisPageStore store;
    QVERIFY2(store.configure(initial, completions, 4, &error), qPrintable(error));
    QVERIFY(store.registerReplicaProvider(provider));
    QVERIFY(store.registerExactGenerationArchive(archive));
    QVERIFY2(store.finalizeInitialization(&error), qPrintable(error));

    const KisRetainedImageEpochSnapshot retainedInitial = store.captureRetainedEpoch();
    QVERIFY(retainedInitial.isValid());
    const KisPageKey key = pageKey(4, -2);
    auto writeAndCommit = [&](quint8 value) -> KisImageEpochCommitTicket {
        const KisImageEpochId base = store.captureCommittedEpoch().epoch;
        const KisPageTransaction transaction = store.beginTransaction(base);
        if (!transaction.isValid())
            return {};
        KisPageStoreRandomAccessor accessor(&store, key.surface, transaction, KisPageWriteMode::PreserveContents);
        if (!accessor.isValid())
            return {};
        accessor.moveTo(key.page.column * 64, key.page.row * 64);
        if (!accessor.rawData())
            return {};
        accessor.rawData()[0] = value;
        if (!accessor.finish(&error))
            return {};
        return store.commit(transaction, store.preparedPages(transaction));
    };
    auto currentVersionAndByte = [&]() -> QPair<KisPageVersion, int> {
        KisPageReadView current;
        const KisPageAccessRequirement cpuAccess{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
        const KisReadRequest request = store.acquireRead(key, current, cpuAccess, KisPagePriority::Normal);
        if (!request.isValid())
            return {};
        KisReadLease lease = store.resolve(request, request.readiness);
        if (!lease.isValid())
            return {};
        const int value = static_cast<const quint8 *>(lease.cpuData())[0];
        store.release(std::move(lease));
        return {request.version, value};
    };

    QVERIFY(writeAndCommit(0x8d).isValid());
    const QPair<KisPageVersion, int> generation2 = currentVersionAndByte();
    QVERIFY(generation2.first.isValid());
    QCOMPARE(generation2.first.generation.value, quint64(2));
    QCOMPARE(generation2.second, 0x8d);
    const KisCompletionTicket archived2 = store.archiveRead(key, {}, KisPagePriority::Background, &error);
    QVERIFY2(archived2.isValid(), qPrintable(error));
    QVERIFY(archive->contains(generation2.first));
    QByteArray archivedBytes;
    KisReplicaLayout archivedLayout;
    QVERIFY(archive->loadExact(generation2.first,
                               initial.surfaces.first().allocationDescriptor(),
                               &archivedBytes,
                               &archivedLayout));
    QCOMPARE(quint8(archivedBytes.at(0)), quint8(0x8d));
    QVERIFY(archivedLayout.matches(initial.surfaces.first().allocationDescriptor()));

    QVERIFY(writeAndCommit(0x44).isValid());
    const QPair<KisPageVersion, int> generation3 = currentVersionAndByte();
    QCOMPARE(generation3.first.generation.value, quint64(3));
    QCOMPARE(generation3.second, 0x44);
    archive->injectNextArchiveWriteFailure();
    QVERIFY(!store.archiveRead(key, {}, KisPagePriority::Background, &error).isValid());
    QVERIFY(!error.isEmpty());
    QVERIFY(!archive->contains(generation3.first));
    QCOMPARE(currentVersionAndByte(), generation3);

    QVERIFY(store.archiveRead(key, {}, KisPagePriority::Background, &error).isValid());
    QVERIFY(archive->contains(generation3.first));
    QVERIFY(!store.archiveRead(key, {}, KisPagePriority::Background, &error).isValid());
    QCOMPARE(currentVersionAndByte(), generation3);

    KisPageReadView initialView;
    initialView.kind = KisPageReadViewKind::CommittedEpoch;
    initialView.epoch = retainedInitial.snapshot.epoch;
    initialView.retention = retainedInitial.token;
    QVERIFY(store.archiveRead(key, initialView, KisPagePriority::Background, &error).isValid());
    const KisPageVersion initialDefault{key, KisPageGeneration{1}, initial.surfaces.first().defaultPixelRevision};
    QVERIFY(archive->contains(initialDefault));
    QVERIFY(archive->loadExact(initialDefault, initial.surfaces.first().allocationDescriptor(), &archivedBytes));
    QCOMPARE(quint8(archivedBytes.at(0)), quint8(0x2a));
    QVERIFY(archive->forget(generation2.first));
    QVERIFY(archive->forget(generation3.first));
    QVERIFY(archive->forget(initialDefault));
    QVERIFY(!archive->contains(generation2.first));
    QVERIFY(!archive->contains(generation3.first));
    QVERIFY(!archive->contains(initialDefault));
    QVERIFY(store.releaseSnapshot(retainedInitial.token));
    QVERIFY(!store.sessionStats().hasOutstandingCapabilities());
}

void KisPageStoreReferenceTest::asyncArchiveCompletionCancellationAndShutdownAreFailClosed()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig providerConfig;
    providerConfig.provider = KisReplicaProviderId{73};
    providerConfig.providerEpoch = KisReplicaProviderEpoch{1};
    providerConfig.budgetBytes = 1024 * 1024;
    QString error;
    QVERIFY2(provider->configure(providerConfig, completions, &error), qPrintable(error));
    auto archive = std::make_shared<FakeAsyncExactArchive>(completions);
    QVERIFY(archive->isOperational());

    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = 1;
    initial.defaultPixelRevision = 1;
    initial.extentRevision = 1;
    initial.propertyRevision = 1;
    initial.surfaces = {surfaceEpochState(KisSurfaceId{1}, 1, 0x2a)};
    KisPageStore store;
    QVERIFY2(store.configure(initial, completions, 8, &error), qPrintable(error));
    QVERIFY(store.registerReplicaProvider(provider));
    QVERIFY(store.registerExactGenerationArchive(archive));
    QVERIFY2(store.finalizeInitialization(&error), qPrintable(error));

    const KisPageKey key = pageKey(-3, 5);
    auto writeAndCommit = [&](quint8 value) -> KisPageVersion {
        const KisPageTransaction transaction = store.beginTransaction(store.captureCommittedEpoch().epoch);
        if (!transaction.isValid())
            return {};
        KisPageStoreRandomAccessor accessor(&store, key.surface, transaction, KisPageWriteMode::PreserveContents);
        if (!accessor.isValid())
            return {};
        accessor.moveTo(key.page.column * 64, key.page.row * 64);
        if (!accessor.rawData())
            return {};
        accessor.rawData()[0] = value;
        if (!accessor.finish(&error) || !store.commit(transaction, store.preparedPages(transaction)).isValid()) {
            return {};
        }
        KisPageReadView current;
        const KisReadRequest request = store.acquireRead(key,
                                                         current,
                                                         {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                                                         KisPagePriority::Normal);
        if (!request.isValid() || !store.cancel(request))
            return {};
        return request.version;
    };
    auto currentByte = [&]() -> int {
        const KisReadRequest request = store.acquireRead(key,
                                                         {},
                                                         {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
                                                         KisPagePriority::Normal);
        if (!request.isValid())
            return -1;
        KisReadLease lease = store.resolve(request, request.readiness);
        if (!lease.isValid())
            return -1;
        const int value = static_cast<const quint8 *>(lease.cpuData())[0];
        store.release(std::move(lease));
        return value;
    };

    const KisPageVersion generation2 = writeAndCommit(0x61);
    QVERIFY(generation2.isValid());
    const KisCompletionTicket first = store.archiveRead(key, {}, KisPagePriority::Background, &error);
    QVERIFY2(first.isValid(), qPrintable(error));
    QCOMPARE(store.sessionStats().pendingArchiveOperations, qsizetype(1));
    QVERIFY(!store.closeSession(&error));
    const KisPageOperationId firstOperation = archive->pendingOperation(first);
    QVERIFY(firstOperation.isValid());
    QVERIFY(archive->finish(firstOperation, KisCompletionStatus::Succeeded));
    const QVector<KisPageArchiveCompletionEvent> firstEvents = store.pollArchiveCompletions();
    QCOMPARE(firstEvents.size(), 1);
    QVERIFY(firstEvents.first().isValid());
    QVERIFY(firstEvents.first().succeeded());
    QCOMPARE(firstEvents.first().version, generation2);
    QVERIFY(archive->contains(generation2));

    const KisPageVersion generation3 = writeAndCommit(0x72);
    QVERIFY(generation3.isValid());
    const KisCompletionTicket second = store.archiveRead(key, {}, KisPagePriority::Background, &error);
    QVERIFY(second.isValid());
    QVERIFY(store.cancelArchive(second));
    const QVector<KisPageArchiveCompletionEvent> secondEvents = store.pollArchiveCompletions();
    QCOMPARE(secondEvents.size(), 1);
    QCOMPARE(secondEvents.first().status, KisCompletionStatus::Cancelled);
    QVERIFY(!archive->contains(generation3));
    QCOMPARE(currentByte(), 0x72);

    const KisPageVersion generation4 = writeAndCommit(0x83);
    QVERIFY(generation4.isValid());
    const KisCompletionTicket third = store.archiveRead(key, {}, KisPagePriority::Background, &error);
    QVERIFY(third.isValid());
    const KisPageOperationId thirdOperation = archive->pendingOperation(third);
    QVERIFY(thirdOperation.isValid());
    QVERIFY(archive->finish(thirdOperation, KisCompletionStatus::Failed));
    const QVector<KisPageArchiveCompletionEvent> thirdEvents = store.pollArchiveCompletions();
    QCOMPARE(thirdEvents.size(), 1);
    QCOMPARE(thirdEvents.first().status, KisCompletionStatus::Failed);
    QVERIFY(!archive->contains(generation4));
    QCOMPARE(currentByte(), 0x83);
    QVERIFY(!store.sessionStats().hasOutstandingCapabilities());
    QVERIFY2(store.closeSession(&error), qPrintable(error));
    QVERIFY(!store.isOperational());
    QCOMPARE(provider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreReferenceTest::metadataShardsSurviveConcurrentLeaseStressWithinBudget()
{
    constexpr int pageCount = 256;
    constexpr int threadCount = 8;
    constexpr int iterationsPerPage = 100;
    KisPageMetadataCoordinator coordinator;
    QString error;
    QVERIFY2(coordinator.configure(32, &error), qPrintable(error));
    for (int i = 0; i < pageCount; ++i) {
        const KisPageVersion version = pageVersion(i, 1);
        QVERIFY2(coordinator.registerPage(initialPageState(version, replica(version, 71, 1, quint64(i + 1))), &error),
                 qPrintable(error));
    }

    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    workers.reserve(threadCount);
    for (int worker = 0; worker < threadCount; ++worker) {
        workers.emplace_back([&, worker]() {
            for (int pageIndex = worker; pageIndex < pageCount; pageIndex += threadCount) {
                const KisPageVersion version = pageVersion(pageIndex, 1);
                const KisReplicaHandle authority = replica(version, 71, 1, quint64(pageIndex + 1));
                for (int iteration = 0; iteration < iterationsPerPage; ++iteration) {
                    const quint64 leaseValue = 1 + quint64(pageIndex) * iterationsPerPage + quint64(iteration);
                    KisPageTransition acquire;
                    acquire.kind = KisPageTransitionKind::AcquireRead;
                    acquire.version = version;
                    acquire.target = authority;
                    acquire.lease = KisPageLeaseId{leaseValue};
                    if (!coordinator.applyOwner(version.key, acquire).accepted) {
                        failed.store(true, std::memory_order_relaxed);
                        return;
                    }
                    KisPageTransition release = acquire;
                    release.kind = KisPageTransitionKind::ReleaseRead;
                    if (!coordinator.applyOwner(version.key, release).accepted) {
                        failed.store(true, std::memory_order_relaxed);
                        return;
                    }
                }
            }
        });
    }
    for (std::thread &worker : workers)
        worker.join();
    QVERIFY(!failed.load(std::memory_order_relaxed));

    const KisPageMetadataMetrics metrics = coordinator.metrics();
    const quint64 expectedTransitions = quint64(pageCount) * iterationsPerPage * 2;
    QCOMPARE(metrics.acceptedTransitions, expectedTransitions);
    QCOMPARE(metrics.rejectedTransitions, quint64(0));

    const KisPageMetadataFootprint footprint = coordinator.footprint();
    QCOMPARE(footprint.pages, quint64(pageCount));
    QCOMPARE(footprint.versions, quint64(pageCount));
    QCOMPARE(footprint.replicas, quint64(pageCount));
    QCOMPARE(footprint.readLeases, quint64(0));
    QCOMPARE(footprint.pendingLastUses, quint64(0));
    QVERIFY(footprint.trackedBytesPerPage() > 0.0);
    QVERIFY2(footprint.trackedBytesPerPage() <= 1536.0,
             qPrintable(QStringLiteral("tracked bytes/page: %1").arg(footprint.trackedBytesPerPage())));
    QCOMPARE(footprint.versionArena.usedSlots, quint64(pageCount));
    QCOMPARE(footprint.replicaArena.usedSlots, quint64(pageCount));
    QCOMPARE(footprint.overflowArena.usedSlots, quint64(0));
    QVERIFY(footprint.versionArena.allocatedBytes > 0);
    QVERIFY(footprint.replicaArena.allocatedBytes > 0);
    QVERIFY(footprint.versionArena.freeSlots > 0);
    QVERIFY(footprint.replicaArena.freeSlots > 0);
    QVERIFY(footprint.versionArena.highWaterSlots >= footprint.versionArena.usedSlots);
    QVERIFY(footprint.replicaArena.highWaterSlots >= footprint.replicaArena.usedSlots);

    const QVector<KisPageKey> pages = coordinator.pageKeys();
    QCOMPARE(pages.size(), pageCount);
    const KisPageStateMachine stateMachine;
    for (const KisPageKey &key : pages) {
        KisPageStateSnapshot page;
        QVERIFY(coordinator.pageSnapshot(key, &page));
        QVERIFY2(stateMachine.validateInvariants(page, &error), qPrintable(error));
    }
}

void KisPageStoreReferenceTest::checkpointRoundTripPreservesWideFormatAndSparseDefaults()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig providerConfig;
    providerConfig.provider = KisReplicaProviderId{74};
    providerConfig.providerEpoch = KisReplicaProviderEpoch{1};
    providerConfig.budgetBytes = 8 * 1024 * 1024;
    QString error;
    QVERIFY2(provider->configure(providerConfig, completions, &error), qPrintable(error));

    KisSurfaceEpochState surface = surfaceEpochState();
    surface.format.formatId = 16;
    surface.format.colorModelId = QByteArrayLiteral("RGBA");
    surface.format.colorDepthId = QByteArrayLiteral("F32");
    surface.format.profileFingerprint = QByteArrayLiteral("wide-checkpoint");
    surface.format.channelOrder = QByteArrayLiteral("RGBA");
    surface.format.defaultPixel = QByteArray(16, char(0));
    for (int i = 0; i < surface.format.defaultPixel.size(); ++i) {
        surface.format.defaultPixel[i] = char(0x10 + i);
    }
    surface.format.channelCount = 4;
    surface.format.pixelStride = 16;
    surface.format.pixelAlignment = 4;
    surface.format.hasAlpha = true;
    surface.format.alphaSemantic = KisSurfaceAlphaSemantic::Straight;
    surface.rowAlignment = 16;
    QVERIFY(surface.isValid());

    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = 1;
    initial.defaultPixelRevision = 1;
    initial.extentRevision = 1;
    initial.propertyRevision = 1;
    initial.surfaces = {surface};
    KisPageStore source;
    QVERIFY2(source.configure(initial, completions, 16, &error), qPrintable(error));
    QVERIFY(source.registerReplicaProvider(provider));
    QVERIFY2(source.finalizeInitialization(&error), qPrintable(error));

    const KisPageTransaction transaction = source.beginTransaction(initial.epoch);
    QVERIFY(transaction.isValid());
    KisPageStoreRandomAccessor writer(&source, surface.surface, transaction, KisPageWriteMode::PreserveContents);
    QVERIFY(writer.isValid());
    QByteArray firstPixel(16, char(0xa1));
    QByteArray secondPixel(16, char(0xb2));
    writer.moveTo(-1, -1);
    QVERIFY(writer.rawData());
    std::memcpy(writer.rawData(), firstPixel.constData(), 16);
    writer.moveTo(64, 0);
    QVERIFY(writer.rawData());
    std::memcpy(writer.rawData(), secondPixel.constData(), 16);
    QVERIFY2(writer.finish(&error), qPrintable(error));
    QVERIFY(source.commit(transaction, source.preparedPages(transaction)).isValid());

    KisPageStoreCheckpoint checkpoint;
    QVERIFY2(KisPageStoreCheckpointCodec::capture(&source, &checkpoint, &error), qPrintable(error));
    QCOMPARE(checkpoint.pages.size(), qsizetype(2));
    const QByteArray encoded = KisPageStoreCheckpointCodec::encode(checkpoint, &error);
    QVERIFY2(!encoded.isEmpty(), qPrintable(error));

    QTemporaryFile file;
    QVERIFY(file.open());
    QCOMPARE(file.write(encoded), qint64(encoded.size()));
    QVERIFY(file.flush());
    QVERIFY(file.seek(0));
    const QByteArray reopenedBytes = file.readAll();
    QCOMPARE(reopenedBytes, encoded);
    KisPageStoreCheckpoint decoded;
    QVERIFY2(KisPageStoreCheckpointCodec::decode(reopenedBytes, &decoded, &error), qPrintable(error));
    QVERIFY(decoded.isValid());

    QByteArray corrupted = reopenedBytes;
    corrupted[corrupted.size() / 2] ^= char(0x5a);
    KisPageStoreCheckpoint rejected;
    QVERIFY(!KisPageStoreCheckpointCodec::decode(corrupted, &rejected, &error));

    auto reopenedCompletions = std::make_shared<KisCompletionRegistry>();
    auto reopenedProvider = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig reopenedProviderConfig;
    reopenedProviderConfig.provider = KisReplicaProviderId{75};
    reopenedProviderConfig.providerEpoch = KisReplicaProviderEpoch{1};
    reopenedProviderConfig.budgetBytes = 8 * 1024 * 1024;
    QVERIFY2(reopenedProvider->configure(reopenedProviderConfig, reopenedCompletions, &error), qPrintable(error));
    KisPageStore reopened;
    QVERIFY2(
        KisPageStoreCheckpointCodec::restore(decoded, &reopened, reopenedProvider, reopenedCompletions, 16, &error),
        qPrintable(error));

    KisPageStoreRandomAccessor reader(&reopened, surface.surface, {});
    QVERIFY(reader.isValid());
    reader.moveTo(-1, -1);
    QVERIFY(!reader.rawData());
    QVERIFY(reader.rawDataConst());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(reader.rawDataConst()), 16), firstPixel);
    reader.moveTo(64, 0);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(reader.rawDataConst()), 16), secondPixel);
    reader.moveTo(130, 130);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(reader.rawDataConst()), 16), surface.format.defaultPixel);
    QVERIFY(reader.finish(&error));
    QCOMPARE(reopened.captureCommittedEpoch().epoch, checkpoint.snapshot.epoch);
    QCOMPARE(reopened.captureCommittedEpoch().manifest, checkpoint.snapshot.manifest);

    QVERIFY2(source.closeSession(&error), qPrintable(error));
    QVERIFY2(reopened.closeSession(&error), qPrintable(error));
    QCOMPARE(provider->memoryUsage().committedBytes, quint64(0));
    QCOMPARE(reopenedProvider->memoryUsage().committedBytes, quint64(0));
}

void KisPageStoreReferenceTest::randomAccessorMatchesTiles3AcrossNegativePageBoundary()
{
    auto completions = std::make_shared<KisCompletionRegistry>();
    auto provider = std::make_shared<KisCpuPageReplicaProvider>();
    KisCpuResidentReplicaProviderConfig providerConfig;
    providerConfig.provider = KisReplicaProviderId{68};
    providerConfig.providerEpoch = KisReplicaProviderEpoch{1};
    providerConfig.budgetBytes = 1024 * 1024;
    QString error;
    QVERIFY2(provider->configure(providerConfig, completions, &error), qPrintable(error));

    KisImageEpochSnapshot initial;
    initial.epoch = KisImageEpochId{1};
    initial.graphRevision = 1;
    initial.defaultPixelRevision = 1;
    initial.extentRevision = 1;
    initial.propertyRevision = 1;
    initial.surfaces = {surfaceEpochState(KisSurfaceId{1}, 1, 0x2a)};
    KisPageStore store;
    QVERIFY2(store.configure(initial, completions, 4, &error), qPrintable(error));
    QVERIFY(store.registerReplicaProvider(provider));
    QVERIFY2(store.finalizeInitialization(&error), qPrintable(error));

    const KisPageTransaction transaction = store.beginTransaction(initial.epoch);
    QVERIFY(transaction.isValid());
    KisPageStoreRandomAccessor writer(&store, KisSurfaceId{1}, transaction, KisPageWriteMode::PreserveContents, 5, -7);
    QVERIFY2(writer.isValid(), qPrintable(writer.error()));
    writer.moveTo(4, -8);
    QVERIFY2(writer.rawData(), qPrintable(writer.error()));
    QCOMPARE(writer.oldRawData()[0], quint8(0x2a));
    QCOMPARE(writer.numContiguousColumns(4), qint32(1));
    QCOMPARE(writer.numContiguousRows(-8), qint32(1));
    writer.rawData()[0] = 0x11;
    writer.moveTo(5, -7);
    QVERIFY2(writer.rawData(), qPrintable(writer.error()));
    QCOMPARE(writer.oldRawData()[0], quint8(0x2a));
    QCOMPARE(writer.numContiguousColumns(5), qint32(64));
    QCOMPARE(writer.numContiguousRows(-7), qint32(64));
    QCOMPARE(writer.rowStride(5, -7), qint32(64));
    writer.rawData()[0] = 0x22;
    QVERIFY2(writer.finish(&error), qPrintable(error));

    const KisPreparedPageSet prepared = store.preparedPages(transaction);
    QVERIFY(prepared.isValid());
    QCOMPARE(prepared.proofs.size(), qsizetype(2));
    const KisImageEpochCommitTicket committed = store.commit(transaction, prepared);
    QVERIFY(committed.isValid());

    KisPageReadView current;
    KisPageStoreRandomAccessor reader(&store, KisSurfaceId{1}, current, 5, -7);
    QVERIFY2(reader.isValid(), qPrintable(reader.error()));
    reader.moveTo(4, -8);
    QVERIFY2(reader.rawDataConst(), qPrintable(reader.error()));
    QCOMPARE(reader.rawDataConst()[0], quint8(0x11));
    QCOMPARE(reader.oldRawData()[0], quint8(0x11));
    reader.moveTo(5, -7);
    QCOMPARE(reader.rawDataConst()[0], quint8(0x22));
    QCOMPARE(reader.x(), qint32(5));
    QCOMPARE(reader.y(), qint32(-7));

    quint8 defaultPixel = 0x2a;
    KisSharedPtr<KisDataManager> manager = new KisDataManager(1, &defaultPixel);
    quint8 first = 0x11;
    quint8 second = 0x22;
    manager->setPixel(-1, -1, &first);
    manager->setPixel(0, 0, &second);
    KisRandomAccessor2 legacy(manager.data(), 5, -7, false, nullptr);
    legacy.moveTo(4, -8);
    QCOMPARE(reader.numContiguousColumns(4), legacy.numContiguousColumns(4));
    QCOMPARE(reader.numContiguousRows(-8), legacy.numContiguousRows(-8));
    QCOMPARE(reader.rowStride(4, -8), legacy.rowStride(4, -8));
    QCOMPARE(legacy.rawDataConst()[0], quint8(0x11));
    legacy.moveTo(5, -7);
    QCOMPARE(legacy.rawDataConst()[0], quint8(0x22));
    QVERIFY2(reader.finish(&error), qPrintable(error));

    // Exercise the actual business iterators and their captured PageStore
    // scope, not the retired standalone accessor-based prototypes.
    auto scope = manager->capturePageStoreReadScope(false);
    QVERIFY(scope && scope->isValid());
    {
        KisPageStoreReadCursor cursor(scope);
        const auto pair = cursor.read(-1, -1, &error);
        QVERIFY2(pair.isValid(), qPrintable(error));
        const auto defaultPair = cursor.read(0, -1, &error);
        QVERIFY2(defaultPair.isValid(), qPrintable(error));
    }
    KisHLineIterator2 hline(manager.data(), 4, -8, 2, 5, -7, false, nullptr, scope);
    QCOMPARE(hline.nConseqPixels(), qint32(1));
    QCOMPARE(hline.rawDataConst()[0], quint8(0x11));
    QVERIFY(hline.nextPixel());
    QCOMPARE(hline.rawDataConst()[0], quint8(0x2a));
    QVERIFY(!hline.nextPixel());
    hline.resetPixelPos();
    hline.nextRow();
    QCOMPARE(hline.x(), qint32(4));
    QCOMPARE(hline.y(), qint32(-7));
    QCOMPARE(hline.rawDataConst()[0], quint8(0x2a));
    hline.resetRowPos();
    QCOMPARE(hline.rawDataConst()[0], quint8(0x11));

    KisVLineIterator2 vline(manager.data(), 5, -8, 2, 5, -7, false, nullptr, scope);
    QCOMPARE(vline.nConseqPixels(), qint32(1));
    QCOMPARE(vline.rawDataConst()[0], quint8(0x2a));
    QVERIFY(vline.nextPixel());
    QCOMPARE(vline.rawDataConst()[0], quint8(0x22));
    QVERIFY(!vline.nextPixel());
    vline.resetPixelPos();
    vline.nextColumn();
    QCOMPARE(vline.x(), qint32(6));
    QCOMPARE(vline.y(), qint32(-8));
    QCOMPARE(vline.rawDataConst()[0], quint8(0x2a));
    vline.resetColumnPos();
    QCOMPARE(vline.rawDataConst()[0], quint8(0x2a));
    QVERIFY(!store.sessionStats().hasOutstandingCapabilities());
}

void KisPageStoreReferenceTest::tiles3BackendMapsIdentityDefaultExtentAndCow()
{
    const quint8 defaultPixel = 0;
    KisTiledDataManagerPageStoreBackend backend;
    QString error;
    QVERIFY(!backend.configure(0, &defaultPixel, &error));
    QVERIFY2(backend.configure(1, &defaultPixel, &error), qPrintable(error));
    QVERIFY(!backend.configure(1, &defaultPixel, &error));
    auto read = [&](KisTiledDataManagerPageStoreBackend &owner, qint32 column, qint32 row) {
        return KisPageStoreReadPage(owner.store(), owner.captureReadView(),
                                   {owner.surface(), {column, row}}, &error);
    };
    const auto initialView = backend.captureReadView();
    KisSurfaceEpochState initialSurface;
    QVERIFY(initialView.resolveSurfaceState(backend.surface(), &initialSurface));
    QCOMPARE(initialSurface.defaultPixelRevision, quint64(1));
    QCOMPARE(initialSurface.extentRevision, quint64(1));
    const auto initialPage = read(backend, 0, 0);
    QVERIFY(initialPage.data());
    QCOMPARE(initialPage.version().key, (KisPageKey{backend.surface(), {0, 0}}));
    QVERIFY(initialPage.version().isDefaultPixel());
    QVERIFY(!backend.pageAllocated(0, 0, false));
    const auto emptyBefore = read(backend, 10, -2);
    QVERIFY(emptyBefore.data());
    QCOMPARE(emptyBefore.version().key.page, (KisLogicalPageId{10, -2}));

    QVERIFY2(backend.fillRect(QRect(1, 1, 1, 1), QByteArray(1, char(0xa5)), &error), qPrintable(error));
    const auto written = read(backend, 0, 0);
    QVERIFY(written.data());
    QVERIFY(backend.pageAllocated(0, 0, false));
    QVERIFY(!written.version().isDefaultPixel());
    QCOMPARE(written.data()[written.rowStride() + 1], quint8(0xa5));
    QCOMPARE(initialPage.data()[initialPage.rowStride() + 1], quint8(0));
    KisPageAllocationDescriptor allocation;
    QVERIFY(backend.store()->pageDescriptor(written.version(), &allocation));
    QCOMPARE(allocation.layoutRevision, initialSurface.layoutRevision);
    QCOMPARE(allocation.format.defaultPixel, QByteArray(1, 0));
    KisSurfaceEpochState writtenSurface;
    QVERIFY(backend.store()->resolveSurfaceState(backend.surface(), {}, &writtenSurface));
    QVERIFY(writtenSurface.extentRevision > initialSurface.extentRevision);
    const auto stable = read(backend, 0, 0);
    QCOMPARE(stable.version(), written.version());
    QCOMPARE(stable.data(), written.data());

    KisTiledDataManagerPageStoreBackend clone;
    QVERIFY2(clone.configureClone(backend, &error), qPrintable(error));
    const auto sharedClone = read(clone, 0, 0);
    QVERIFY(sharedClone.data());
    QCOMPARE(sharedClone.data(), written.data()); // actual shared provider backing
    QVERIFY2(clone.fillRect(QRect(2, 2, 1, 1), QByteArray(1, char(0x5a)), &error), qPrintable(error));
    const auto detached = read(clone, 0, 0);
    QVERIFY(detached.data());
    QVERIFY(detached.version().generation.value > sharedClone.version().generation.value);
    QVERIFY(detached.data() != sharedClone.data());
    QCOMPARE(detached.data()[detached.rowStride() + 1], quint8(0xa5));
    QCOMPARE(detached.data()[2 * detached.rowStride() + 2], quint8(0x5a));
    const auto unchangedSource = read(backend, 0, 0);
    QCOMPARE(unchangedSource.version(), written.version());
    QCOMPARE(unchangedSource.data()[2 * unchangedSource.rowStride() + 2], quint8(0));

    QVERIFY2(backend.setDefaultPixel(QByteArray(1, char(0x11)), &error), qPrintable(error));
    const auto emptyAfter = read(backend, 10, -2);
    QVERIFY(emptyAfter.data());
    QCOMPARE(emptyAfter.version().defaultPixelRevision, quint64(2));
    QCOMPARE(emptyAfter.data()[0], quint8(0x11));
    QCOMPARE(emptyBefore.data()[0], quint8(0));
    QCOMPARE(emptyBefore.version().defaultPixelRevision, quint64(1));
    QVERIFY(!backend.pageAllocated(10, -2, false));
}

void KisPageStoreReferenceTest::tiles3GenericPublicationDerivesOverlayExtent()
{
    const quint8 blank = 0;
    KisTiledDataManagerPageStoreBackend backend;
    QString error;
    QVERIFY(backend.configure(1, &blank, &error));
    auto *store = backend.store();
    auto tx = store->beginCurrentTransaction();
    const auto selection = KisPageReadView::transactionOverlay(tx.id);
    auto before = store->captureReadView(selection);
    const KisPageKey key{backend.surface(), {-3, 2}};
    const KisPageAccessRequirement cpuAccess{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
    const auto request = store->acquireWrite(tx, key, cpuAccess, KisPageWriteMode::PreserveContents,
                                             KisPagePriority::Interactive);
    QVERIFY(request.isValid());
    auto write = store->resolve(request, request.readiness);
    QVERIFY(write.isValid());
    static_cast<quint8 *>(write.cpuData())[0] = 0x61;
    QVERIFY(store->publishHostWrite(std::move(write)).isValid());
    auto written = store->captureReadView(selection);
    KisSurfaceEpochState state;
    QVERIFY(written.resolveSurfaceState(backend.surface(), &state));
    QCOMPARE(state.contentExtent, QRect(-192, 128, 64, 64));
    QCOMPARE(state.extentRevision, quint64(2));
    QVERIFY(before.resolveSurfaceState(backend.surface(), &state));
    QCOMPARE(state.contentExtent, QRect());
    KisPageVersion observed;
    QVERIFY(written.resolvePageVersion(key, &observed));
    QVERIFY2(store->stagePageRemovalIfUnchanged(tx, observed, &error), qPrintable(error));
    auto removed = store->captureReadView(selection);
    QVERIFY(removed.resolveSurfaceState(backend.surface(), &state));
    QCOMPARE(state.contentExtent, QRect());
    QCOMPARE(state.extentRevision, quint64(3));
    KisSurfaceEpochChange roundTrip;
    QVERIFY(before.resolveSurfaceState(backend.surface(), &roundTrip.before));
    roundTrip.after = state;
    QVERIFY(roundTrip.isValid());
    roundTrip.after.extentRevision = roundTrip.before.extentRevision;
    QVERIFY(!roundTrip.isValid());
    roundTrip.after.contentExtent = QRect(0, 0, 64, 64);
    QVERIFY(!roundTrip.isValid());
    roundTrip.after.extentRevision = 0;
    QVERIFY(!roundTrip.isValid());
    auto oldPage = written.readResidentPage({backend.surface(), {-3, 2}});
    auto newPage = removed.readResidentPage({backend.surface(), {-3, 2}});
    QVERIFY(oldPage.isValid() && newPage.isValid());
    QCOMPARE(static_cast<const quint8 *>(oldPage.data())[0], quint8(0x61));
    QCOMPARE(static_cast<const quint8 *>(newPage.data())[0], quint8(0));
    QVERIFY(store->commit(tx, store->preparedPages(tx)).isValid());
}

void KisPageStoreReferenceTest::tiles3BackendPublishesDefaultExtentAndHistoryAtomically()
{
    quint8 initialDefault = 0;
    KisTiledDataManagerPageStoreBackend backend;
    QString error;
    QVERIFY2(backend.configure(1, &initialDefault, &error), qPrintable(error));

    const QByteArray paint(1, char(0x71));
    QVERIFY2(backend.fillRect(QRect(-10, -2, 80, 70), paint, &error), qPrintable(error));
    KisSurfaceEpochState state;
    QVERIFY(backend.store()->resolveSurfaceState(backend.surface(), {}, &state));
    QCOMPARE(state.contentExtent, QRect(-64, -64, 192, 192));
    QCOMPARE(state.extentRevision, quint64(2));

    const KisMementoSP memento = backend.beginHistory(&initialDefault, 1, &error);
    QVERIFY2(memento, qPrintable(error));
    QVERIFY2(backend.fillRect(QRect(256, 128, 1, 1), paint, &error), qPrintable(error));

    KisTiledDataManagerPageStoreBackend clone;
    QVERIFY2(clone.configureClone(backend, &error), qPrintable(error));
    KisSurfaceEpochState cloneState;
    QVERIFY(clone.store()->resolveSurfaceState(clone.surface(), {}, &cloneState));
    QCOMPARE(cloneState.contentExtent, QRect(-64, -64, 384, 256));
    auto extended = backend.captureReadView();
    QVERIFY(extended.resolveSurfaceState(backend.surface(), &state));
    QCOMPARE(state.contentExtent, cloneState.contentExtent);
    QCOMPARE(state.extentRevision, quint64(3));
    const QByteArray changedDefault(1, char(0x22));
    QVERIFY2(backend.setDefaultPixel(changedDefault, &error), qPrintable(error));
    QVERIFY2(backend.clearAll(&error), qPrintable(error));
    const quint8 newDefault = 0x22;
    QVERIFY2(backend.commitHistory(&newDefault, 1, &error), qPrintable(error));

    QVERIFY(backend.store()->resolveSurfaceState(backend.surface(), {}, &state));
    QCOMPARE(state.format.defaultPixel, changedDefault);
    QCOMPARE(state.contentExtent, QRect());
    QCOMPARE(state.defaultPixelRevision, quint64(2));
    QCOMPARE(state.extentRevision, quint64(4));

    QVERIFY2(backend.rollback(memento, &error), qPrintable(error));
    QVERIFY(backend.store()->resolveSurfaceState(backend.surface(), {}, &state));
    QCOMPARE(state.format.defaultPixel, QByteArray(1, char(0)));
    QCOMPARE(state.contentExtent, QRect(-64, -64, 192, 192));

    QVERIFY2(backend.rollforward(memento, &error), qPrintable(error));
    QVERIFY(backend.store()->resolveSurfaceState(backend.surface(), {}, &state));
    QCOMPARE(state.format.defaultPixel, changedDefault);
    QCOMPARE(state.contentExtent, QRect());
    QVERIFY2(backend.purgeHistory(memento, &newDefault, 1, &error), qPrintable(error));
}

void KisPageStoreReferenceTest::tiles3BackendCoalescesOverlappingAnonymousLeases()
{
    quint8 initialDefault = 0;
    KisTiledDataManagerPageStoreBackend backend;
    QString error;
    QVERIFY2(backend.configure(1, &initialDefault, &error), qPrintable(error));

    std::unique_ptr<KisTilePageStoreLease> first = backend.acquireTile(0, 0, true, false);
    std::unique_ptr<KisTilePageStoreLease> second = backend.acquireTile(1, 0, true, false);
    QVERIFY(first && first->tileData() && first->writable());
    QVERIFY(second && second->tileData() && second->writable());
    first->markDirty();
    second->markDirty();
    first->tileData()->data()[0] = 0x31;
    second->tileData()->data()[0] = 0x42;

    QVERIFY(first->finish());
    QCOMPARE(backend.store()->captureCommittedEpoch().manifest.size(), 0);
    QVERIFY(second->finish());
    QCOMPARE(backend.store()->captureCommittedEpoch().manifest.size(), 2);

    std::unique_ptr<KisTilePageStoreLease> firstRead = backend.acquireTile(0, 0, false, false);
    std::unique_ptr<KisTilePageStoreLease> secondRead = backend.acquireTile(1, 0, false, false);
    QVERIFY(firstRead && secondRead);
    QCOMPARE(firstRead->tileData()->data()[0], quint8(0x31));
    QCOMPARE(secondRead->tileData()->data()[0], quint8(0x42));
    QVERIFY(firstRead->finish());
    QVERIFY(secondRead->finish());
    QVERIFY(backend.store()->waitForRetirementIdle());
    QVERIFY(!backend.store()->sessionStats().hasOutstandingCapabilities());

    std::unique_ptr<KisTilePageStoreLease> dirty = backend.acquireTile(2, 0, true, false);
    std::unique_ptr<KisTilePageStoreLease> clean = backend.acquireTile(3, 0, true, false);
    QVERIFY(dirty && clean);
    dirty->markDirty();
    dirty->tileData()->data()[0] = 0x53;
    QVERIFY(dirty->finish());
    QVERIFY(clean->finish());
    QCOMPARE(backend.store()->captureCommittedEpoch().manifest.size(), 3);
    QVERIFY(backend.store()->waitForRetirementIdle());
    QVERIFY(!backend.store()->sessionStats().hasOutstandingCapabilities());
}

void KisPageStoreReferenceTest::tiles3BackendReclaimsPurgedHistoryVersions()
{
    quint8 initialDefault = 0;
    KisTiledDataManagerPageStoreBackend backend;
    QString error;
    QVERIFY2(backend.configure(1, &initialDefault, &error), qPrintable(error));
    QVERIFY2(backend.fillRect(QRect(0, 0, 1, 1), QByteArray(1, char(1)), &error), qPrintable(error));

    QElapsedTimer timer;
    timer.start();
    for (int revision = 2; revision <= 65; ++revision) {
        const KisMementoSP memento = backend.beginHistory(&initialDefault, 1, &error);
        QVERIFY2(memento, qPrintable(error));
        QVERIFY2(backend.fillRect(QRect(0, 0, 1, 1), QByteArray(1, char(revision)), &error), qPrintable(error));
        QVERIFY2(backend.commitHistory(&initialDefault, 1, &error), qPrintable(error));
        QVERIFY2(backend.rollback(memento, &error), qPrintable(error));
        QVERIFY2(backend.rollforward(memento, &error), qPrintable(error));
        QVERIFY2(backend.purgeHistory(memento, &initialDefault, 1, &error), qPrintable(error));
        QVERIFY(backend.store()->waitForRetirementIdle());
        const KisPageStoreSessionStats stats = backend.store()->sessionStats();
        QCOMPARE(stats.registeredPages, qsizetype(1));
        QCOMPARE(stats.pageVersions, qsizetype(1));
        QCOMPARE(stats.replicas, qsizetype(1));
        QVERIFY(!stats.hasOutstandingCapabilities());
    }
    QVERIFY(timer.elapsed() < 5000);
}

void KisPageStoreReferenceTest::tiles3BackendCoalescesConcurrentProductWrites()
{
    constexpr int workerCount = 8;
    constexpr int writesPerWorker = 16;
    quint8 initialDefault = 0;
    KisTiledDataManagerPageStoreBackend backend;
    QString error;
    QVERIFY2(backend.configure(1, &initialDefault, &error), qPrintable(error));

    std::atomic<bool> failed{false};
    std::atomic<bool> stopReaders{false};
    std::atomic<int> readyReaders{0};
    QMutex failuresMutex;
    QStringList failures;
    const auto recordFailure = [&](const QString &message) {
        QMutexLocker lock(&failuresMutex);
        failures.append(message);
        failed.store(true, std::memory_order_relaxed);
    };
    std::vector<std::thread> readers;
    readers.reserve(2);
    for (int reader = 0; reader < 2; ++reader) {
        readers.emplace_back([&, reader]() {
            int iteration = reader;
            readyReaders.fetch_add(1, std::memory_order_release);
            while (!stopReaders.load(std::memory_order_acquire)) {
                std::unique_ptr<KisTilePageStoreLease> lease =
                    backend.acquireTile(iteration % writesPerWorker,
                                        (iteration / writesPerWorker) % workerCount,
                                        false,
                                        false);
                if (!lease || !lease->tileData() || !lease->finish()) {
                    recordFailure(QStringLiteral("reader %1 iteration %2: %3")
                                      .arg(reader)
                                      .arg(iteration)
                                      .arg(lease ? QStringLiteral("tile/finish") : QStringLiteral("acquire")));
                    return;
                }
                ++iteration;
                std::this_thread::yield();
            }
        });
    }
    while (readyReaders.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    QElapsedTimer timer;
    timer.start();
    for (int worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&, worker]() {
            const QByteArray pixel(1, char(worker + 1));
            for (int column = 0; column < writesPerWorker; ++column) {
                QString workerError;
                if (!backend.fillRect(QRect(column * KisTileData::WIDTH, worker * KisTileData::HEIGHT, 1, 1),
                                      pixel,
                                      &workerError)) {
                    recordFailure(QStringLiteral("writer %1 column %2: %3").arg(worker).arg(column).arg(workerError));
                    return;
                }
            }
        });
    }
    for (std::thread &worker : workers)
        worker.join();
    stopReaders.store(true, std::memory_order_release);
    for (std::thread &reader : readers)
        reader.join();

    QVERIFY2(!failed.load(std::memory_order_relaxed), qPrintable(failures.join(QStringLiteral("; "))));
    QCOMPARE(backend.allocatedPages().size(), workerCount * writesPerWorker);
    KisSurfaceEpochState state;
    QVERIFY(backend.store()->resolveSurfaceState(backend.surface(), {}, &state));
    QCOMPARE(state.contentExtent, QRect(0, 0, writesPerWorker * KisTileData::WIDTH, workerCount * KisTileData::HEIGHT));
    QVERIFY(timer.elapsed() < 10000);
    QVERIFY(backend.store()->waitForRetirementIdle());
    QVERIFY(!backend.store()->sessionStats().hasOutstandingCapabilities());

    const KisMementoSP memento = backend.beginHistory(&initialDefault, 1, &error);
    QVERIFY2(memento, qPrintable(error));
    workers.clear();
    failed.store(false, std::memory_order_relaxed);
    for (int worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&, worker]() {
            const QByteArray pixel(1, char(worker + 17));
            for (int column = 0; column < writesPerWorker; ++column) {
                QString workerError;
                if (!backend.fillRect(
                        QRect(column * KisTileData::WIDTH, (workerCount + worker) * KisTileData::HEIGHT, 1, 1),
                        pixel,
                        &workerError)) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    for (std::thread &worker : workers)
        worker.join();
    QVERIFY(!failed.load(std::memory_order_relaxed));
    QVERIFY2(backend.commitHistory(&initialDefault, 1, &error), qPrintable(error));
    QCOMPARE(backend.allocatedPages().size(), 2 * workerCount * writesPerWorker);
    QVERIFY(backend.store()->resolveSurfaceState(backend.surface(), {}, &state));
    QCOMPARE(state.contentExtent,
             QRect(0, 0, writesPerWorker * KisTileData::WIDTH, 2 * workerCount * KisTileData::HEIGHT));
    QVERIFY2(backend.rollback(memento, &error), qPrintable(error));
    QCOMPARE(backend.allocatedPages().size(), workerCount * writesPerWorker);
    QVERIFY2(backend.rollforward(memento, &error), qPrintable(error));
    QCOMPARE(backend.allocatedPages().size(), 2 * workerCount * writesPerWorker);
    QVERIFY2(backend.purgeHistory(memento, &initialDefault, 1, &error), qPrintable(error));
    QVERIFY(backend.store()->waitForRetirementIdle());
    QVERIFY(!backend.store()->sessionStats().hasOutstandingCapabilities());
}

void KisPageStoreReferenceTest::tiles3BackendsShareProcessResidentLimit()
{
    KisImageConfig config(false);
    const qreal oldHard = config.memoryHardLimitPercent();
    const qreal oldSoft = config.memorySoftLimitPercent();
    const qreal oldPool = config.memoryPoolLimitPercent();
    const int oldSwap = config.maxSwapSize();
    const auto restore = qScopeGuard([&] {
        config.setMemoryHardLimitPercent(oldHard);
        config.setMemorySoftLimitPercent(oldSoft);
        config.setMemoryPoolLimitPercent(oldPool);
        config.setMaxSwapSize(oldSwap);
        KisTileDataStoreTestAccess::rereadConfig();
    });
    // Keep the physical tile ceiling at 8 MiB while giving the process-wide
    // PageStore control/metadata pool its own 16 MiB share.
    config.setMemoryHardLimitPercent(100.0 * 24.5 / KisImageConfig::totalRAM());
    config.setMemorySoftLimitPercent(0);
    config.setMemoryPoolLimitPercent(100.0 * 16.0 / 24.5);
    config.setMaxSwapSize(0);
    KisTileDataStoreTestAccess::rereadConfig();
    KisTileDataStoreTestAccess::clear();
    const quint64 hardBytes = quint64(KisImageConfig(true).tilesHardLimit()) << 20;
    QCOMPARE(hardBytes, quint64(8 * 1024 * 1024));
    const quint64 nonPayloadBytes =
        quint64(KisImageConfig(true).tilesHardLimit() + KisImageConfig(true).poolLimit()) << 20;
    QCOMPARE(KisTileDataStore::instance()->memoryMetric(), qint64(0));

    const int width = 384 * KisTileData::WIDTH;
    QByteArray pixels(width * KisTileData::HEIGHT * 4, Qt::Uninitialized);
    for (int y = 0; y < KisTileData::HEIGHT; ++y) {
        for (int x = 0; x < width; ++x) {
            const quint32 value = quint32(1 + x / KisTileData::WIDTH)
                | (quint32(y) << 16);
            memcpy(pixels.data() + (y * width + x) * 4, &value, 4);
        }
    }

    quint8 initialDefault[4]{};
    QString error;
    KisTiledDataManagerPageStoreBackend first;
    KisTiledDataManagerPageStoreBackend second;
    QVERIFY2(first.configure(4, initialDefault, &error), qPrintable(error));
    QVERIFY2(second.configure(4, initialDefault, &error), qPrintable(error));
    QVector<KisLogicalPageId> changed;
    const auto firstResult = first.writeBytes(
        reinterpret_cast<const quint8 *>(pixels.constData()),
        0, 0, width, KisTileData::HEIGHT, width * 4, false,
        &changed, &error);
    if (firstResult != KisPageStoreWriteOperationResult::Succeeded) {
        const auto usage = first.store()->backingUsage();
        const auto metadata = usage.buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        qWarning() << "First backend metadata live/peak/reserved:" << metadata.live.cpuRam
                   << metadata.peak.cpuRam << metadata.reserved.cpuRam;
    }
    QVERIFY2(firstResult == KisPageStoreWriteOperationResult::Succeeded,
             qPrintable(error));
    const auto productNonPayloadUsage = [](const KisPageBackingUsage &usage) {
        return usage.buckets[size_t(KisBackingBudgetClass::MetadataArena)].live.cpuRam
            + usage.buckets[size_t(KisBackingBudgetClass::OptionalCache)].live.cpuRam;
    };
    QVERIFY(productNonPayloadUsage(first.store()->backingUsage())
                + productNonPayloadUsage(second.store()->backingUsage())
            <= nonPayloadBytes);
    QCOMPARE(first.store()->backingUsage()
                 .buckets[size_t(KisBackingBudgetClass::Current)].live.cpuRam,
             quint64(6 * 1024 * 1024));
    const auto totalDomainBytes = [](const KisPageDomainBytes &bytes) {
        return bytes.cpuRam + bytes.umaShared + bytes.discreteVram + bytes.ssd;
    };
    const auto totalReservedBytes = [&](const KisPageBackingUsage &usage) {
        quint64 result = 0;
        for (const auto &bucket : usage.buckets)
            result += totalDomainBytes(bucket.reserved);
        return result;
    };
    QCOMPARE(totalReservedBytes(first.store()->backingUsage()), quint64(0));

    auto retained = first.captureReadView();
    QVERIFY(retained.isValid());
    auto slowRead = retained.readResidentPage({first.surface(), {0, 0}});
    QVERIFY(slowRead.isValid());
    quint32 retainedPixel = 0;
    memcpy(&retainedPixel, slowRead.data(), sizeof(retainedPixel));
    QCOMPARE(retainedPixel, quint32(1));

    const quint32 replacement = 0xff345678;
    changed.clear();
    QCOMPARE(first.writeBytes(reinterpret_cast<const quint8 *>(&replacement),
                              0, 0, 1, 1, sizeof(replacement), false,
                              &changed, &error),
             KisPageStoreWriteOperationResult::Succeeded);
    QCOMPARE(changed.size(), 1);
    memcpy(&retainedPixel, slowRead.data(), sizeof(retainedPixel));
    QCOMPARE(retainedPixel, quint32(1));
    const auto heldUsage = first.store()->backingUsage();
    QVERIFY(heldUsage.buckets[size_t(KisBackingBudgetClass::RetainedHistory)]
                .live.cpuRam >= quint64(KisTileData::WIDTH * KisTileData::HEIGHT * 4));
    QCOMPARE(totalReservedBytes(heldUsage), quint64(0));

    slowRead = {};
    retained = {};
    QVERIFY(first.store()->waitForRetirementIdle());
    const auto drainedUsage = first.store()->backingUsage();
    QCOMPARE(totalDomainBytes(drainedUsage.buckets[
                 size_t(KisBackingBudgetClass::RetainedHistory)].live), quint64(0));
    QCOMPARE(totalDomainBytes(drainedUsage.buckets[
                 size_t(KisBackingBudgetClass::RetirementDebt)].live), quint64(0));
    QCOMPARE(totalReservedBytes(drainedUsage), quint64(0));

    changed.clear();
    const auto secondResult = second.writeBytes(
                reinterpret_cast<const quint8 *>(pixels.constData()),
                0, 0, width, KisTileData::HEIGHT, width * 4, false,
                &changed, &error);
    QVERIFY2(secondResult != KisPageStoreWriteOperationResult::Succeeded,
             qPrintable(error));
    QVERIFY(changed.isEmpty());
    QVERIFY2(second.store()->waitForRetirementIdle(), "failed batch did not drain");
    QCOMPARE(totalReservedBytes(first.store()->backingUsage()), quint64(0));
    QCOMPARE(totalReservedBytes(second.store()->backingUsage()), quint64(0));
    const quint64 residentBytes = quint64(KisTileDataStore::instance()->memoryMetric())
        * KisTileData::WIDTH * KisTileData::HEIGHT;
    QVERIFY(residentBytes <= hardBytes);
    QVERIFY2(first.store()->closeSession(&error), qPrintable(error));
    QVERIFY2(second.store()->closeSession(&error), qPrintable(error));
    QCOMPARE(totalReservedBytes(first.store()->backingUsage()), quint64(0));
    QCOMPARE(totalReservedBytes(second.store()->backingUsage()), quint64(0));
    QCOMPARE(KisTileDataStore::instance()->memoryMetric(), qint64(0));
}

QTEST_GUILESS_MAIN(KisPageStoreReferenceTest)

#include "KisPageStoreReferenceTest.moc"

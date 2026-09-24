/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisTiles3PageReplicaProvider.h"
#include "KisCpuResidentBinding_p.h"
#include "KisPageStore.h"

#include "tiles3/kis_tile_data.h"
#include "tiles3/kis_tile_data_store.h"

#include <QHash>
#include <QMutexLocker>
#include <QScopeGuard>

#include <cstring>
#include <limits>
#include <utility>

namespace {

bool supportsNativeTileLayout(const KisPageAllocationDescriptor &descriptor)
{
    if (!descriptor.isValid() ||
        descriptor.pageExtent != QSize(KisTileData::WIDTH,
                                       KisTileData::HEIGHT) ||
        descriptor.format.pixelStride >
            quint32(std::numeric_limits<qint32>::max())) {
        return false;
    }
    const quint64 rowStride = descriptor.minimumRowBytes();
    return rowStride <= quint64(std::numeric_limits<quint32>::max()) &&
           rowStride % descriptor.rowAlignment == 0 &&
           rowStride % descriptor.format.pixelAlignment == 0;
}

}

class Tiles3ResidentBinding final : public KisCpuResidentBinding
{
public:
    explicit Tiles3ResidentBinding(KisTileData *tile)
        : m_tile(tile) { m_tile->ref(); }
    ~Tiles3ResidentBinding() override { if (m_tile) m_tile->deref(); }
    // Only used while an owner-issued immutable read guard holds this binding.
    KisTileData *guardedTile() const { return m_tile; }
private:
    void *pinStorage(bool residentOnly, KisCpuResidentReadStatus *status) override
    {
        if (!m_tile) {
            if (status) *status = KisCpuResidentReadStatus::Retired;
            return nullptr;
        }
        if (residentOnly) {
            bool busy = false;
            if (!m_tile->tryBlockSwapping(&busy)) {
                if (status) *status = busy ? KisCpuResidentReadStatus::Busy : KisCpuResidentReadStatus::NonResident;
                return nullptr;
            }
        } else {
            m_tile->blockSwapping();
        }
        if (status) *status = KisCpuResidentReadStatus::Ready;
        return m_tile->data();
    }
    void unpinStorage() override { m_tile->unblockSwapping(); }
    void *pinResidentStorageAfterGateWait(KisCpuResidentReadStatus *status) override
    {
        if (!m_tile) {
            if (status) *status = KisCpuResidentReadStatus::Retired;
            return nullptr;
        }
        const bool ready = m_tile->blockSwappingIfResident();
        if (status) *status = ready ? KisCpuResidentReadStatus::Ready : KisCpuResidentReadStatus::NonResident;
        return ready ? m_tile->data() : nullptr;
    }
    void releaseStorage() override
    {
        if (m_tile) { m_tile->deref(); m_tile = nullptr; }
    }
    KisTileData *m_tile;
};

struct Tiles3Allocation
{
    KisReplicaHandle handle;
    QSharedPointer<Tiles3ResidentBinding> binding;
    quint64 physicalBacking = 0;

    KisTileData *tileData() const
    {
        return binding ? binding->guardedTile() : nullptr;
    }
};

struct Tiles3PhysicalPayload
{
    quint64 identity = 0;
    quint64 logicalReferences = 0;
};

class Tiles3ReplicaSource final : public KisPageReplicaSource
{
public:
    Tiles3ReplicaSource(KisReplicaProviderId provider, KisReplicaProviderEpoch epoch,
                       const KisPageAllocationDescriptor &descriptor,
                       QSharedPointer<const quint8> owner, KisTileData *tile)
        : KisPageReplicaSource(provider, epoch, descriptor), owner(std::move(owner)), tile(tile)
    { tile->acquire(); }
    ~Tiles3ReplicaSource() override { tile->release(); }
    const QSharedPointer<const quint8> owner;
    KisTileData *const tile;
};

class KisTiles3PageReplicaProvider::Private : public KisCpuResidentAllocationIndex<Tiles3Allocation>
{
public:
    class ResidencyObserver final : public KisTileDataResidencyObserver
    {
    public:
        void residencyChanged(KisTileData *tileData, bool resident,
                              quint64 revision) override
        {
            QMutexLocker locker(&m_mutex);
            auto tracked = m_payloads.find(tileData);
            if (tracked == m_payloads.end() || !revision)
                return;
            const KisPageAccessDomain domain = resident
                ? KisPageAccessDomain::CpuRam : KisPageAccessDomain::Ssd;
            const auto order = tracked->revision
                ? kisCompareBackingRevision(revision, tracked->revision)
                : KisBackingRevisionOrder::Newer;
            if (order == KisBackingRevisionOrder::Older)
                return;
            if (order == KisBackingRevisionOrder::Same) {
                Q_ASSERT(tracked->domain == domain);
                return;
            }
            if (order == KisBackingRevisionOrder::Ambiguous)
                return;
            tracked->revision = revision;
            tracked->domain = domain;

            const KisReplicaBackingDomainChange change{
                revision, tracked->slot, domain, tracked->bytes};
            auto pending = m_changes.find(tracked->slot);
            if (pending == m_changes.end()) {
                m_changes.insert(tracked->slot, change);
                return;
            }
            const auto pendingOrder = kisCompareBackingRevision(
                revision, pending->revision);
            if (pendingOrder == KisBackingRevisionOrder::Newer)
                *pending = change;
            else if (pendingOrder == KisBackingRevisionOrder::Same)
                Q_ASSERT(pending->domain == domain && pending->bytes == tracked->bytes);
        }

        bool track(KisTileData *tileData, quint64 slot, quint64 bytes)
        {
            QMutexLocker locker(&m_mutex);
            if (!tileData || !slot || !bytes || m_payloads.contains(tileData))
                return false;
            m_payloads.insert(tileData, {slot, bytes});
            return true;
        }

        bool initialize(KisTileData *tileData,
                        const KisTileDataResidencyState &state)
        {
            if (!state.isValid())
                return false;
            QMutexLocker locker(&m_mutex);
            auto tracked = m_payloads.find(tileData);
            if (tracked == m_payloads.end())
                return false;
            const KisPageAccessDomain domain = state.resident
                ? KisPageAccessDomain::CpuRam : KisPageAccessDomain::Ssd;
            if (!tracked->revision) {
                tracked->revision = state.revision;
                tracked->domain = domain;
                return true;
            }
            const auto order = kisCompareBackingRevision(state.revision,
                                                         tracked->revision);
            if (order == KisBackingRevisionOrder::Newer) {
                tracked->revision = state.revision;
                tracked->domain = domain;
                return true;
            }
            if (order == KisBackingRevisionOrder::Same)
                return tracked->domain == domain;
            // A callback may have delivered a newer state between observer
            // registration and this initial snapshot installation.
            return order == KisBackingRevisionOrder::Older;
        }

        bool observation(KisTileData *tileData,
                         KisPageAccessDomain *domain,
                         quint64 *revision) const
        {
            QMutexLocker locker(&m_mutex);
            const auto tracked = m_payloads.constFind(tileData);
            if (tracked == m_payloads.cend() || !tracked->revision
                || tracked->domain == KisPageAccessDomain::Unknown) {
                return false;
            }
            if (domain) *domain = tracked->domain;
            if (revision) *revision = tracked->revision;
            return true;
        }

        void untrack(KisTileData *tileData, quint64 slot)
        {
            QMutexLocker locker(&m_mutex);
            m_payloads.remove(tileData);
            m_changes.remove(slot);
        }

        QVector<KisReplicaBackingDomainChange> changes() const
        {
            QMutexLocker locker(&m_mutex);
            QVector<KisReplicaBackingDomainChange> result;
            result.reserve(m_changes.size());
            for (const auto &change : std::as_const(m_changes))
                result.append(change);
            return result;
        }

        void acknowledge(quint64 slot, quint64 revision)
        {
            QMutexLocker locker(&m_mutex);
            const auto change = m_changes.find(slot);
            if (change != m_changes.end() && change->revision == revision)
                m_changes.erase(change);
        }

    private:
        struct Payload {
            quint64 slot = 0;
            quint64 bytes = 0;
            quint64 revision = 0;
            KisPageAccessDomain domain = KisPageAccessDomain::Unknown;
        };
        mutable QMutex m_mutex;
        QHash<KisTileData *, Payload> m_payloads;
        QHash<quint64, KisReplicaBackingDomainChange> m_changes;
    };

    Private()
        : residencyObserver(QSharedPointer<ResidencyObserver>::create()) {}

    ~Private()
    {
        for (auto payload = physicalPayloads.cbegin(); payload != physicalPayloads.cend(); ++payload) {
            KisTileDataStore::instance()->unregisterResidencyObserver(payload.key(), residencyObserver);
            residencyObserver->untrack(payload.key(), payload->identity);
        }
    }

    const QSharedPointer<const quint8> sourceOwner = QSharedPointer<const quint8>::create(0);
    bool canAllocateHandle() const
    {
        return config.isValid() && nextSlot != 0 &&
               nextSlot != std::numeric_limits<quint64>::max();
    }

    KisReplicaHandle allocateHandle(const KisPageVersion &version,
                                    const KisPageAllocationDescriptor &descriptor)
    {
        Q_ASSERT(canAllocateHandle());
        KisReplicaHandle handle;
        handle.provider = config.provider;
        handle.providerEpoch = config.providerEpoch;
        handle.allocation = {nextSlot++, 1};
        handle.version = version;
        handle.domain = KisPageAccessDomain::CpuRam;
        handle.layout = {descriptor.layoutRevision, descriptor.format.formatId,
                         descriptor.pageExtent, descriptor.validRect,
                         quint32(descriptor.minimumRowBytes()), descriptor.minimumByteSize()};
        return handle;
    }

    quint64 retainPhysical(KisTileData *tileData, quint64 bytes)
    {
        auto existing = physicalPayloads.find(tileData);
        if (existing != physicalPayloads.end()) {
            if (quint64(tileData->pixelSize()) * KisTileData::WIDTH * KisTileData::HEIGHT != bytes)
                return {};
            ++existing->logicalReferences;
            return existing->identity;
        }
        if (!tileData || bytes > config.budgetBytes - qMin(config.budgetBytes, committedBytes)
            || nextPhysicalSlot == 0
            || nextPhysicalSlot == std::numeric_limits<quint64>::max()) {
            return {};
        }
        const quint64 identity = nextPhysicalSlot++;
        physicalPayloads.insert(tileData, {identity, 1});
        KisTileDataResidencyState initialState;
        if (!residencyObserver->track(tileData, identity, bytes)
            || !KisTileDataStore::instance()->registerResidencyObserver(
                tileData, residencyObserver, &initialState)
            || !residencyObserver->initialize(tileData, initialState)) {
            KisTileDataStore::instance()->unregisterResidencyObserver(
                tileData, residencyObserver);
            residencyObserver->untrack(tileData, identity);
            physicalPayloads.remove(tileData);
            return {};
        }
        committedBytes += bytes;
        return identity;
    }

    void releasePhysical(KisTileData *tileData,
                         quint64 identity)
    {
        auto existing = physicalPayloads.find(tileData);
        Q_ASSERT(existing != physicalPayloads.end());
        Q_ASSERT(existing->identity == identity);
        Q_ASSERT(existing->logicalReferences != 0);
        if (--existing->logicalReferences == 0) {
            const quint64 bytes = quint64(tileData->pixelSize()) *
                KisTileData::WIDTH * KisTileData::HEIGHT;
            Q_ASSERT(committedBytes >= bytes);
            committedBytes -= bytes;
            KisTileDataStore::instance()->unregisterResidencyObserver(tileData, residencyObserver);
            residencyObserver->untrack(tileData, identity);
            physicalPayloads.erase(existing);
        }
    }

    KisReplicaOperation allocate(
        KisPageOperationId operation,
        const KisPageVersion &version,
        const KisPageAllocationDescriptor &descriptor,
        KisPageAccessDomain domain,
        KisTileData *copySource = nullptr, const KisCpuPagePayload *payload = nullptr)
    {
        // This allocator returns independent mutable storage only; complete
        // payload aliases use an explicit immutable source instead.
        KisTileData *tileData = nullptr;
        auto releaseTile = qScopeGuard([&]() {
            if (tileData) tileData->deref();
        });
        if (!config.isValid()) {
            return KisReplicaOperation::failed(
                operation, QStringLiteral("tiles3 provider is not configured"));
        }
        if (!version.isValid() ||
            domain != KisPageAccessDomain::CpuRam ||
            !supportsNativeTileLayout(descriptor) ||
            !operationAvailable(operation) ||
            (payload && (copySource || !payload->isValidFor(descriptor)))) {
            return KisReplicaOperation::failed(
                operation, QStringLiteral("tiles3 allocation request is invalid"));
        }
        consumeOperation(operation);

        const quint64 byteSize = descriptor.minimumByteSize();
        if (byteSize > config.budgetBytes - qMin(config.budgetBytes, committedBytes)
            || !canAllocateHandle()) {
            return KisReplicaOperation::failed(
                operation,
                QStringLiteral("tiles3 page budget or identity is exhausted"));
        }
        const KisCompletionTicket completion =
            completions->allocatePending(completionSource);
        if (!completion.isValid()) {
            return KisReplicaOperation::failed(
                operation,
                QStringLiteral("tiles3 completion allocation failed"));
        }
        auto failCompletion = qScopeGuard([&]() {
            completions->complete(completion, KisCompletionStatus::Failed);
        });
        if (payload) {
            tileData = KisTileDataStore::instance()->createTileDataFromRows(
                qint32(descriptor.format.pixelStride), static_cast<const quint8 *>(payload->data),
                payload->rowStride, payload->byteSize);
            if (tileData) {
                ++work.payloadInitializedPages;
                work.payloadInitializedBytes += byteSize;
            }
        } else if (copySource) {
            bool precloneHit = false;
            tileData = KisTileDataStore::instance()->duplicatePinnedTileData(
                copySource, &precloneHit);
            ++work.nativeDuplicatePages;
            work.nativeDuplicateBytes += byteSize;
            work.nativePrecloneHits += precloneHit;
            if (!precloneHit) work.nativeForegroundCopyBytes += byteSize;
        } else {
            tileData = KisTileDataStore::instance()->createDefaultTileData(
                  qint32(descriptor.format.pixelStride),
                  reinterpret_cast<const quint8 *>(
                      descriptor.format.defaultPixel.constData()));
            ++work.defaultInitializedPages;
            work.defaultInitializedBytes += byteSize;
        }
        if (!tileData || !tileData->ref()) {
            return KisReplicaOperation::failed(
                operation, QStringLiteral("tiles3 tile allocation failed"));
        }

        tileData->blockSwapping();
        const quintptr address = reinterpret_cast<quintptr>(tileData->data());
        const bool aligned = address != 0 &&
            address % descriptor.format.pixelAlignment == 0;
        tileData->unblockSwapping();
        if (!aligned) {
            return KisReplicaOperation::failed(
                operation,
                QStringLiteral("tiles3 tile allocation alignment is insufficient"));
        }

        const KisReplicaHandle handle = allocateHandle(version, descriptor);

        const quint64 physical = retainPhysical(tileData, byteSize);
        if (!physical) {
            return KisReplicaOperation::failed(operation,
                                   QStringLiteral("tiles3 physical payload budget is exhausted"));
        }
        allocations.insert(handle.allocation.slot,
                           {handle, QSharedPointer<Tiles3ResidentBinding>::create(tileData), physical});
        if (!completions->complete(completion,
                                   KisCompletionStatus::Succeeded)) {
            allocations.remove(handle.allocation.slot);
            releasePhysical(tileData, physical);
            return KisReplicaOperation::failed(
                operation,
                QStringLiteral("tiles3 completion publication failed"));
        }
        failCompletion.dismiss();
        return {KisPageRequestStatus::Ready, operation, handle, completion, {}};
    }

    quint64 nextPhysicalSlot = 1;
    QHash<KisTileData *, Tiles3PhysicalPayload> physicalPayloads;
    QSharedPointer<ResidencyObserver> residencyObserver;
    KisTiles3PayloadWork work;
};

KisTiles3PageReplicaProvider::KisTiles3PageReplicaProvider()
    : d(new Private)
{
}

KisTiles3PageReplicaProvider::~KisTiles3PageReplicaProvider()
{
    d->revokeBindings();
}

bool KisTiles3PageReplicaProvider::configure(
    const KisCpuResidentReplicaProviderConfig &config,
    const QSharedPointer<KisCompletionRegistry> &completions,
    QString *error)
{
    return d->configure(config, completions, QStringLiteral("tiles3"), error);
}

QString KisTiles3PageReplicaProvider::name() const
{
    return QStringLiteral("Krita tiles3 RAM/swap page replica provider");
}

KisReplicaProviderId KisTiles3PageReplicaProvider::providerId() const
{
    return d->providerId();
}

KisReplicaProviderEpoch KisTiles3PageReplicaProvider::providerEpoch() const
{
    return d->providerEpoch();
}

KisReplicaCapabilities KisTiles3PageReplicaProvider::capabilities() const
{
    static const KisReplicaCapabilities result{{KisPageAccessDomain::CpuRam},
        {{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer}},
        true, true, true, true, true, true};
    return result;
}

KisReplicaOperation KisTiles3PageReplicaProvider::requestReplica(
    KisPageOperationId operation,
    const KisPageVersion &version,
    const KisPageAllocationDescriptor &descriptor,
    KisPageAccessDomain domain,
    KisPageAccessMode mode,
    KisPagePriority priority)
{
    Q_UNUSED(mode);
    Q_UNUSED(priority);
    QMutexLocker locker(&d->mutex);
    return d->allocate(operation, version, descriptor, domain);
}

KisReplicaOperation KisTiles3PageReplicaProvider::prepareWrite(
    KisPageOperationId operation,
    const KisPageVersion &version,
    const KisPageAllocationDescriptor &descriptor,
    KisPageAccessDomain domain,
    KisPageWriteMode mode,
    KisPagePriority priority)
{
    Q_UNUSED(mode);
    return requestReplica(operation, version, descriptor, domain,
                          KisPageAccessMode::Write, priority);
}

KisReplicaOperation KisTiles3PageReplicaProvider::prepareSynchronousCpuPayload(
    KisPageOperationId operation, const KisPageVersion &version,
    const KisPageAllocationDescriptor &descriptor, const KisCpuPagePayload &payload,
    KisPagePriority priority)
{
    Q_UNUSED(priority);
    QMutexLocker locker(&d->mutex);
    return d->allocate(operation, version, descriptor, KisPageAccessDomain::CpuRam, nullptr, &payload);
}

KisReplicaOperation KisTiles3PageReplicaProvider::prepareSynchronousWriteCopy(
    KisPageOperationId operation,
    const KisReplicaHandle &source,
    const KisPageVersion &targetVersion,
    const KisPageAllocationDescriptor &descriptor,
    KisPagePriority priority)
{
    Q_UNUSED(priority);
    QMutexLocker locker(&d->mutex);
    const auto it = d->findExactAllocation(source);
    KisTileData *sourceTile = it != d->allocations.end() ? it->tileData() : nullptr;
    if (!targetVersion.isValid() ||
        !(source.version.key == targetVersion.key) ||
        targetVersion.generation.value <= source.version.generation.value ||
        !source.layout.matches(descriptor) ||
        !sourceTile) {
        return KisReplicaOperation::failed(operation,
            QStringLiteral("tiles3 native copy source is stale or mutable"));
    }
    // Join the same binding gate as generic reads, transfer and retire BEFORE consuming a
    // preclone or looking at source bytes. A physical/native writer rejects
    // here instead of waiting on its swap pin while holding the provider lock.
    // Keep the binding itself, not an allocations iterator: allocate() may
    // rehash that table. The guarded source is pinned once for both cache-hit
    // and copy paths; duplicatePinnedTileData must not take a nested read lock.
    const auto sourceBinding = it->binding;
    if (!sourceBinding || !sourceBinding->acquireRead(false)) {
        return KisReplicaOperation::failed(operation,
            QStringLiteral("tiles3 native copy source binding is busy or retired"));
    }
    const auto releaseSource = qScopeGuard([&] { sourceBinding->releaseRead(); });
    return d->allocate(operation, targetVersion, descriptor, source.domain,
                       sourceTile);
}


QSharedPointer<const KisPageReplicaSource> KisTiles3PageReplicaProvider::captureCompletedTileSource(
    const KisPageAllocationDescriptor &descriptor, KisTileData *tileData, QString *error)
{
    if (!supportsNativeTileLayout(descriptor) ||
        descriptor.format.pixelAlignment > alignof(void *) || !tileData ||
        tileData->pixelSize() != descriptor.format.pixelStride) {
        KisPageStoreDetail::setError(error, QStringLiteral("completed tile source has an invalid layout")); return {};
    }
    QMutexLocker locker(&d->mutex);
    if (!d->config.isValid()) { KisPageStoreDetail::setError(error, QStringLiteral("source provider is unavailable")); return {}; }
    auto source = QSharedPointer<Tiles3ReplicaSource>::create(
        d->config.provider, d->config.providerEpoch, descriptor, d->sourceOwner, tileData);
    KisPageStoreDetail::setError(error, {});
    return source;
}

bool KisTiles3PageReplicaProvider::sourceMatchesReadGuard(
    const QSharedPointer<const KisPageReplicaSource> &source, const KisCpuReadGuard &guard) const
{
    const auto input = qSharedPointerDynamicCast<const Tiles3ReplicaSource>(source);
    return input && input->owner == d->sourceOwner &&
        input->tile == tileDataForCpuReadGuard(guard);
}

KisTileData *KisTiles3PageReplicaProvider::tileDataForCpuReadGuard(const KisCpuReadGuard &guard) const
{
    const auto binding = qSharedPointerDynamicCast<Tiles3ResidentBinding>(guard.m_binding);
    return guard.isValid() && binding ? binding->guardedTile() : nullptr;
}

QSharedPointer<const KisPageReplicaSource> KisTiles3PageReplicaProvider::captureCpuReadSource(
    const KisCpuReadGuard &guard, const KisPageAllocationDescriptor &descriptor, QString *error)
{
    KisTileData *tileData = tileDataForCpuReadGuard(guard);
    if (!guard.m_scope || !tileData ||
        guard.m_pageExtent != descriptor.pageExtent ||
        guard.m_rowStride != descriptor.minimumRowBytes()) {
        KisPageStoreDetail::setError(error, QStringLiteral("alias source needs an exact tiles3 read guard")); return {};
    }
    return captureCompletedTileSource(descriptor, tileData, error);
}

bool KisTiles3PageReplicaProvider::copySynchronousSourceToCpu(
    const QSharedPointer<const KisPageReplicaSource> &source,
    const KisPageAllocationDescriptor &descriptor, void *destination,
    quint32 rowStride, quint64 byteSize)
{
    const auto input = qSharedPointerDynamicCast<const Tiles3ReplicaSource>(source);
    QMutexLocker locker(&d->mutex);
    if (!d->config.isValid() || !input || input->owner != d->sourceOwner ||
        !(input->descriptor() == descriptor) || !destination ||
        rowStride < descriptor.minimumRowBytes() ||
        byteSize < quint64(rowStride) * descriptor.pageExtent.height()) return false;
    input->tile->blockSwapping();
    const auto unpin = qScopeGuard([&] { input->tile->unblockSwapping(); });
    for (int y = 0; y < descriptor.pageExtent.height(); ++y)
        std::memcpy(static_cast<quint8 *>(destination) + quint64(y) * rowStride,
            input->tile->data() + quint64(y) * descriptor.minimumRowBytes(), size_t(descriptor.minimumRowBytes()));
    ++d->work.explicitCopyPages;
    d->work.explicitCopyBytes += descriptor.minimumByteSize();
    return true;
}

KisReplicaOperation KisTiles3PageReplicaProvider::prepareSynchronousSource(
    KisPageOperationId operation, const QSharedPointer<const KisPageReplicaSource> &source,
    const KisPageVersion &targetVersion, const KisPageAllocationDescriptor &descriptor,
    KisReplicaSourceUse use, KisPagePriority priority)
{
    Q_UNUSED(priority);
    const auto input = qSharedPointerDynamicCast<const Tiles3ReplicaSource>(source);
    QMutexLocker locker(&d->mutex);
    if (!d->config.isValid() || !input || input->owner != d->sourceOwner ||
        !(input->descriptor() == descriptor) || !targetVersion.isValid() ||
        !supportsNativeTileLayout(descriptor) || !d->operationAvailable(operation) ||
        (use != KisReplicaSourceUse::ImmutableAlias && use != KisReplicaSourceUse::WritableCopy)) {
        return KisReplicaOperation::failed(operation, QStringLiteral("immutable source is foreign, stale or incompatible"));
    }
    // Residency is needed only for independent pixel initialization. Holding
    // an alias source never forces swap-in merely to validate an address.
    if (use == KisReplicaSourceUse::WritableCopy) {
        input->tile->blockSwapping();
        const auto unpin = qScopeGuard([&] { input->tile->unblockSwapping(); });
        return d->allocate(operation, targetVersion, descriptor, KisPageAccessDomain::CpuRam, input->tile);
    }
    const quint64 bytes = descriptor.minimumByteSize();
    if (!d->canAllocateHandle())
        return KisReplicaOperation::failed(operation, QStringLiteral("immutable alias budget or identity is exhausted"));
    d->consumeOperation(operation);
    const auto completion = d->completions->allocatePending(d->completionSource);
    if (!completion.isValid()) return KisReplicaOperation::failed(operation, QStringLiteral("alias completion is unavailable"));
    const KisReplicaHandle handle = d->allocateHandle(targetVersion, descriptor);
    const quint64 physical = d->retainPhysical(input->tile, bytes);
    if (!physical) {
        d->completions->complete(completion, KisCompletionStatus::Failed);
        return KisReplicaOperation::failed(operation, QStringLiteral("immutable alias physical budget is exhausted"));
    }
    d->allocations.insert(handle.allocation.slot,
         {handle, QSharedPointer<Tiles3ResidentBinding>::create(input->tile), physical});
    ++d->work.adoptedPages; d->work.adoptedBytes += bytes;
    d->completions->complete(completion, KisCompletionStatus::Succeeded);
    return {KisPageRequestStatus::Ready, operation, handle, completion, {}};
}


KisReplicaHandle KisTiles3PageReplicaProvider::adoptInitialTile(
    const KisPageVersion &version,
    const KisPageAllocationDescriptor &descriptor,
    KisTileData *tileData,
    QString *error)
{
    if (!version.isValid() || !supportsNativeTileLayout(descriptor) ||
        !tileData || tileData->pixelSize() != descriptor.format.pixelStride ||
        !tileData->ref()) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 initial tile is invalid"));
        return {};
    }
    const auto releaseTile = qScopeGuard([&] { tileData->deref(); });
    QMutexLocker locker(&d->mutex);
    const quint64 byteSize = descriptor.minimumByteSize();
    if (!d->canAllocateHandle()) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 initial tile is unavailable"));
        return {};
    }
    const KisReplicaHandle handle = d->allocateHandle(version, descriptor);
    const quint64 physical = d->retainPhysical(tileData, byteSize);
    if (!physical) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 initial physical payload budget is exhausted"));
        return {};
    }
    d->allocations.insert(handle.allocation.slot,
                          {handle, QSharedPointer<Tiles3ResidentBinding>::create(tileData), physical});
    KisPageStoreDetail::setError(error, {});
    ++d->work.adoptedPages;
    d->work.adoptedBytes += byteSize;
    return handle;
}

KisReplicaOperation KisTiles3PageReplicaProvider::transfer(
    const KisReplicaTransferRequest &request,
    KisPagePriority priority)
{
    Q_UNUSED(priority);
    QMutexLocker locker(&d->mutex);
    auto result = d->transfer(request, QStringLiteral("tiles3"));
    if (result.status == KisPageRequestStatus::Ready) {
        ++d->work.explicitCopyPages;
        d->work.explicitCopyBytes += request.target.layout.byteSize;
    }
    return result;
}

KisReplicaAccess KisTiles3PageReplicaProvider::resolveAccess(
    KisPageLeaseId lease,
    KisPageOperationId operation,
    const KisReplicaHandle &replica,
    KisPageAccessRequirement requirement,
    KisPageAccessMode mode)
{
    return d->resolveAccess(lease, operation, replica, requirement, mode);
}

void KisTiles3PageReplicaProvider::releaseAccess(
    KisReplicaAccess access,
    const KisCompletionTicket &lastUse)
{
    Q_UNUSED(lastUse);
    d->releaseAccess(access);
}

bool KisTiles3PageReplicaProvider::validate(
    const KisReplicaHandle &replica,
    const KisPageAllocationDescriptor &descriptor) const
{
    QMutexLocker locker(&d->mutex);
    const auto allocationIt = d->findExactAllocation(replica);
    return supportsNativeTileLayout(descriptor) &&
           replica.layout.matches(descriptor) &&
           allocationIt != d->allocations.end() &&
           allocationIt->tileData();
}

KisReplicaOperation KisTiles3PageReplicaProvider::retire(
    KisPageOperationId operation,
    const KisReplicaHandle &replica,
    const KisCompletionTicket &lastUse)
{
    KisTileData *retired = nullptr;
    KisCompletionTicket completion;
    {
        QMutexLocker locker(&d->mutex);
        auto retirement = d->beginRetirement(operation, replica, lastUse);
        if (retirement.failure)
            return KisReplicaOperation::failed(operation,
                QStringLiteral("tiles3 retirement %1").arg(QString::fromLatin1(retirement.failure)));
        completion = retirement.completion;
        retired = retirement.allocation->tileData();
        if (!retired) {
            d->completions->complete(completion, KisCompletionStatus::Failed);
            return KisReplicaOperation::failed(
                operation, QStringLiteral("tiles3 retirement allocation is stale"));
        }
        retired->ref();
        if (!retirement.allocation->binding->retire()) {
            retired->deref();
            d->completions->complete(completion, KisCompletionStatus::Failed);
            return KisReplicaOperation::failed(operation, QStringLiteral("tiles3 native reader still pins allocation"));
        }
        d->consumeOperation(operation);
        d->releasePhysical(retired, retirement.allocation->physicalBacking);
        d->allocations.erase(retirement.allocation);
    }
    retired->deref();
    if (!d->completions->complete(completion,
                                  KisCompletionStatus::Succeeded)) {
        return KisReplicaOperation::failed(
            operation,
            QStringLiteral("tiles3 retirement completion publication failed"));
    }
    return {KisPageRequestStatus::Ready, operation, replica, completion, {}};
}

KisReplicaMemoryUsage KisTiles3PageReplicaProvider::memoryUsage() const
{
    QMutexLocker locker(&d->mutex);
    quint64 residentBytes = 0;
    for (auto payload = d->physicalPayloads.cbegin();
         payload != d->physicalPayloads.cend(); ++payload) {
        if (payload.key()->isResident())
            residentBytes += quint64(payload.key()->pixelSize()) *
                KisTileData::WIDTH * KisTileData::HEIGHT;
    }
    return {residentBytes, d->committedBytes, d->config.budgetBytes};
}

KisReplicaBackingFootprint KisTiles3PageReplicaProvider::backingFootprint(
    const KisReplicaHandle &replica) const
{
    QMutexLocker locker(&d->mutex);
    const auto it = d->findExactAllocation(replica);
    if (it == d->allocations.end() || !it->physicalBacking) {
        return {};
    }
    KisTileData *tileData = it->tileData();
    KisPageAccessDomain domain = KisPageAccessDomain::Unknown;
    quint64 revision = 0;
    if (!tileData || !d->residencyObserver->observation(
            tileData, &domain, &revision)) {
        return {};
    }
    return {it->physicalBacking, domain, it->handle.layout.byteSize, revision};
}

QVector<KisReplicaBackingDomainChange>
KisTiles3PageReplicaProvider::backingDomainChanges() const
{
    return d->residencyObserver->changes();
}

void KisTiles3PageReplicaProvider::acknowledgeBackingDomainChange(
    quint64 physicalSlot, quint64 revision)
{
    d->residencyObserver->acknowledge(physicalSlot, revision);
}

QSharedPointer<KisCpuResidentBinding> KisTiles3PageReplicaProvider::cpuResidentBinding(
    const KisReplicaHandle &replica, KisCpuResidentReadStatus *status) const
{
    return d->binding(replica, status);
}

KisTiles3PayloadWork KisTiles3PageReplicaProvider::payloadWork() const
{
    QMutexLocker locker(&d->mutex);
    return d->work;
}


KisTileData *KisTiles3PageReplicaProvider::tileDataForLease(
    KisPageLeaseId lease) const
{
    if (!lease.isValid()) return nullptr;
    QMutexLocker locker(&d->mutex);
    const auto leaseIt = d->activeLeases.constFind(lease.value);
    if (leaseIt == d->activeLeases.constEnd()) return nullptr;
    const auto allocationIt = d->allocations.constFind(leaseIt->allocation.slot);
    if (allocationIt == d->allocations.constEnd() ||
        !(allocationIt->handle.allocation == leaseIt->allocation)) {
        return nullptr;
    }
    return allocationIt->tileData();
}

KisTileData *KisTiles3PageReplicaProvider::tileDataForCpuWriteGuard(const KisCpuWriteGuard &guard) const
{
    if (!guard.isValid()) return nullptr;
    KisReplicaHandle replica;
    if (!guard.providerBacking(&replica)) return nullptr;
    QMutexLocker lock(&d->mutex);
    const auto it = d->findExactAllocation(replica);
    return it != d->allocations.end() ? it->tileData() : nullptr;
}

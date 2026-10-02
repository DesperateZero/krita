/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisTiles3PageReplicaProvider.h"
#include "KisCpuResidentBinding_p.h"
#include "KisPageStore.h"
#include "KisPageWriteCoordinator_p.h"

#include "tiles3/kis_tile_data.h"
#include "tiles3/kis_tile_data_store.h"

#include <QHash>
#include <QMutexLocker>
#include <QScopeGuard>
#include <QWaitCondition>

#include <atomic>
#include <boost/intrusive/list.hpp>
#include <cstring>
#include <limits>
#include <map>
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

// Derived from provider admission registrations, not a second page owner.
// The count keeps registration out of a short physical claim without holding
// the provider mutex or serializing claims of different allocations.
struct Tiles3HandoffAdmission
{
    QMutex mutex;
    quint64 claims = 0;
    bool singleOwner = true;
    bool tryClaim()
    {
        if (!mutex.tryLock()) return false;
        const auto unlock = qScopeGuard([&] { mutex.unlock(); });
        if (!singleOwner || claims == std::numeric_limits<quint64>::max()) return false;
        ++claims;
        return true;
    }
    void release() noexcept
    {
        QMutexLocker locker(&mutex);
        Q_ASSERT(claims);
        --claims;
    }
};

class Tiles3ResidentBinding final : public KisCpuResidentBinding
{
public:
    Tiles3ResidentBinding(KisTileData *tile, const KisReplicaHandle &handle,
                          Tiles3HandoffAdmission *admission)
        : KisCpuResidentBinding(handle), m_tile(tile), m_admission(admission)
    { m_tile->ref(); }
    ~Tiles3ResidentBinding() override { if (m_tile) m_tile->deref(); }
    // Only used while an owner-issued immutable read guard holds this binding.
    KisTileData *guardedTile() const { return m_tile; }
private:
    bool supportsBackingHandoff() const override { return true; }
    void *tryClaimStorageForHandoff() override
    {
        // Provider registration and retag must linearize. The same handle
        // may otherwise have been imported by another canonical Store without
        // adding a TileData reference. Never infer its absence from refs()==1.
        // This gate covers only owner registration, never ordinary provider
        // access/retirement. A claimed backing still rejects those operations
        // at its local binding instead of blocking the entire provider.
        if (!m_admission->tryClaim()) return nullptr;
        if (!m_tile || !KisTileDataStore::instance()->tryClaimBackingHandoff(m_tile)) {
            m_admission->release();
            return nullptr;
        }
        return m_tile->data();
    }
    void finishStorageHandoff(bool writable) noexcept override
    {
        KisTileDataStore::instance()->finishBackingHandoff(m_tile, writable);
        m_admission->release();
    }
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
            if (!m_tile->blockSwapping()) {
                if (status) *status = KisCpuResidentReadStatus::ResourceUnavailable;
                return nullptr;
            }
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
    // Used only by a short claim whose KisCpuBackingHandoff retains provider.
    Tiles3HandoffAdmission *m_admission;
};

// A derived identity hint, not a root/transaction or a storage owner. KisTile
// retains its existing COW reference while caching this object. Only a fresh
// binding pin permits tileData(); finish returns that pin without discarding
// the cache storage. There is no provider map lookup on a warm read.
class Tiles3TileReadCache final : public KisTilePageStoreLease
{
public:
    ~Tiles3TileReadCache() override { finish(); }
    KisTileData *tileData() const override { return m_pinned ? m_tile : nullptr; }
    bool writable() const override { return false; }
    void markDirty() override {}
    bool reusableReadCache() const override { return true; }
    bool readPinned() const override { return m_pinned; }
    bool matchesVersion(const KisPageVersion &version) const
    { return m_binding && m_binding->matchesReadIdentity(m_expected, version); }
    bool finish() override
    {
        if (std::exchange(m_pinned, false)) m_binding->releaseRead();
        return true;
    }
    ReadPinResult repinRead() override
    {
        if (m_pinned) return ReadPinResult::Ready;
        KisCpuResidentReadStatus status = KisCpuResidentReadStatus::InvalidIdentity;
        if (m_binding && m_binding->acquireRead(m_expected, false, &status)) {
            m_pinned = true;
            return ReadPinResult::Ready;
        }
        if (status == KisCpuResidentReadStatus::InvalidIdentity) return ReadPinResult::Stale;
        if (status == KisCpuResidentReadStatus::Retired) return ReadPinResult::Retired;
        return ReadPinResult::Unavailable;
    }
    static TileLease prepare(const QSharedPointer<KisCpuResidentBinding> &binding,
                             const KisReplicaAllocationIdentity &expected,
                             KisTileData *tile, TileLease reuse, bool pin)
    {
        if (!binding || !expected.isValid() || !tile) return {};
        auto *cache = dynamic_cast<Tiles3TileReadCache *>(reuse.get());
        // A nested reader's capability must never be stolen for a writer.
        if (cache && cache->readPinned()) return {};
        if (!cache) {
            reuse = std::make_unique<Tiles3TileReadCache>();
            cache = static_cast<Tiles3TileReadCache *>(reuse.get());
        }
        cache->m_binding = binding;
        cache->m_expected = expected;
        cache->m_tile = tile;
        if (pin && cache->repinRead() != ReadPinResult::Ready) return {};
        return reuse;
    }
private:
    QSharedPointer<KisCpuResidentBinding> m_binding;
    KisReplicaAllocationIdentity m_expected;
    KisTileData *m_tile = nullptr;
    bool m_pinned = false;
};

struct Tiles3Allocation
{
    QSharedPointer<Tiles3ResidentBinding> binding;
    quint64 physicalBacking = 0;
    KisCompletionTicket retirementCompletion;

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
        struct Payload : boost::intrusive::list_base_hook<>
        {
            Payload(quint64 identity, quint64 size) noexcept : slot(identity), bytes(size) {}
            quint64 slot;
            quint64 bytes;
            quint64 revision = 0;
            KisPageAccessDomain domain = KisPageAccessDomain::Unknown;
            quint64 transition = 0;
        };
        using PayloadMap = std::map<KisTileData *, Payload, std::less<KisTileData *>,
            KisMutationStorageAllocator<std::pair<KisTileData *const, Payload>>>;
        using SlotMap = std::map<quint64, Payload *, std::less<quint64>,
            KisMutationStorageAllocator<std::pair<const quint64, Payload *>>>;
        using Admissions = std::vector<QSharedPointer<KisReplicaBackingDomainAdmission>,
            KisMutationStorageAllocator<QSharedPointer<KisReplicaBackingDomainAdmission>>>;
        using WeakAdmissions = std::vector<QWeakPointer<KisReplicaBackingDomainAdmission>,
            KisMutationStorageAllocator<QWeakPointer<KisReplicaBackingDomainAdmission>>>;
    public:
        explicit ResidencyObserver(const QSharedPointer<KisBackingBudgetController> &process)
            : m_budget(QSharedPointer<KisBackingBudgetController>::create())
        {
            if (process && !m_budget->configureSharedNonPayloadBudget(process)) throw std::bad_alloc();
            const auto storage = KisMutationStorageAllocator<Payload>::retained(m_budget.data());
            m_payloads = PayloadMap(storage);
            m_slots = SlotMap(storage);
            m_admissions = WeakAdmissions(storage);
        }
        class Transition final : public KisTileDataResidencyTransition
        {
        public:
            Transition(ResidencyObserver *owner, KisTileData *tileData,
                       quint64 serial, KisPageAccessDomain targetDomain)
                : m_owner(owner)
                , m_tileData(tileData)
                , m_serial(serial)
                , m_targetDomain(targetDomain)
                , m_reservations(owner->m_admissions.get_allocator()) {}
            ~Transition() override
            {
                m_reservations.clear();
                if (m_owner)
                    m_owner->cancelTransition(m_tileData, m_serial);
            }

            void commit(quint64 revision) noexcept override
            {
                if (!m_owner)
                    return;
                for (const auto &reservation : std::as_const(m_reservations))
                    reservation->commit(revision);
                m_reservations.clear();
                m_owner->commitTransition(
                    m_tileData, m_serial, m_targetDomain, revision);
                m_owner = nullptr;
            }

        private:
            ResidencyObserver *m_owner = nullptr;
            KisTileData *m_tileData = nullptr;
            quint64 m_serial = 0;
            KisPageAccessDomain m_targetDomain = KisPageAccessDomain::Unknown;
        public:
            std::vector<QSharedPointer<KisReplicaBackingDomainReservation>,
                KisMutationStorageAllocator<QSharedPointer<KisReplicaBackingDomainReservation>>> m_reservations;
        };

        void setProviderIdentity(KisReplicaProviderId provider,
                                 KisReplicaProviderEpoch epoch)
        {
            QMutexLocker locker(&m_mutex);
            m_provider = provider;
            m_providerEpoch = epoch;
        }

        bool registerAdmission(
            const QSharedPointer<KisReplicaBackingDomainAdmission> &admission, bool *singleOwner)
        {
            if (!admission)
                return false;
            QMutexLocker locker(&m_mutex);
            for (const auto &existing : std::as_const(m_admissions)) {
                if (existing.toStrongRef() == admission)
                    return true;
            }
            // Conservative provider-wide exclusion: expiry does not prove
            // that all replicas imported by that owner have been released.
            // Do not re-enable handoff merely because an observer disappeared.
            m_admissions.push_back(admission.toWeakRef());
            if (m_admissions.size() > 1) *singleOwner = false;
            return true;
        }

        QSharedPointer<KisTileDataResidencyTransition> prepareResidencyChange(
            KisTileData *tileData,
            const KisTileDataResidencyState &source,
            bool targetResident,
            QString *error) override
        try
        {
            Admissions admissions(m_admissions.get_allocator());
            quint64 serial = 0;
            auto cancel = qScopeGuard([&] { if (serial) cancelTransition(tileData, serial); });
            quint64 slot = 0;
            quint64 bytes = 0;
            const KisPageAccessDomain targetDomain = targetResident
                ? KisPageAccessDomain::CpuRam : KisPageAccessDomain::Ssd;
            {
                QMutexLocker locker(&m_mutex);
                auto tracked = m_payloads.find(tileData);
                const KisPageAccessDomain sourceDomain = source.resident
                    ? KisPageAccessDomain::CpuRam : KisPageAccessDomain::Ssd;
                if (tracked == m_payloads.end() || !source.isValid()
                    || !m_provider.isValid() || !m_providerEpoch.isValid()
                    || tracked->second.domain != sourceDomain
                    || tracked->second.revision != source.revision
                    || tracked->second.transition) {
                    KisPageStoreDetail::setError(
                        error, QStringLiteral("tiles3 residency source changed"));
                    return {};
                }
                admissions.reserve(m_admissions.size());
                for (const auto &weak : std::as_const(m_admissions)) {
                    auto admission = weak.toStrongRef();
                    if (!admission)
                        continue;
                    admissions.push_back(std::move(admission));
                }
                m_admissions.erase(std::remove_if(m_admissions.begin(), m_admissions.end(),
                    [](const auto &weak) { return weak.isNull(); }), m_admissions.end());
                ++m_nextTransition;
                if (!m_nextTransition) ++m_nextTransition;
                serial = m_nextTransition;
                tracked->second.transition = serial;
                slot = tracked->second.slot;
                bytes = tracked->second.bytes;
            }

            auto result = QSharedPointer<Transition>::create(
                this, tileData, serial, targetDomain);
            result->m_reservations.reserve(admissions.size());
            const KisReplicaPhysicalSlotIdentity physical{
                m_provider, m_providerEpoch, slot};
            const KisPageAccessDomain sourceDomain = source.resident
                ? KisPageAccessDomain::CpuRam : KisPageAccessDomain::Ssd;
            for (const auto &admission : std::as_const(admissions)) {
                auto reservation = admission->prepare(
                    physical, bytes, sourceDomain, source.revision,
                    targetDomain, error);
                if (!reservation)
                    return {};
                result->m_reservations.push_back(std::move(reservation));
            }
            cancel.dismiss();
            KisPageStoreDetail::setError(error, {});
            return result;
        }
        catch (const std::bad_alloc &)
        {
            KisPageStoreDetail::setError(error, QStringLiteral("tiles3 residency preparation storage is unavailable"));
            return {};
        }

        bool track(KisTileData *tileData, quint64 slot, quint64 bytes)
        try
        {
            PayloadMap::node_type rejected;
            QMutexLocker locker(&m_mutex);
            if (!tileData || !slot || !bytes || m_payloads.find(tileData) != m_payloads.end())
                return false;
            const auto payload = m_payloads.try_emplace(tileData, slot, bytes).first;
            auto rollback = qScopeGuard([&] { rejected = m_payloads.extract(payload); });
            if (!m_slots.emplace(slot, &payload->second).second) return false;
            rollback.dismiss();
            return true;
        }
        catch (const std::bad_alloc &) { return false; }

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
            if (!tracked->second.revision) {
                tracked->second.revision = state.revision;
                tracked->second.domain = domain;
                return true;
            }
            const auto order = kisCompareBackingRevision(state.revision,
                                                         tracked->second.revision);
            if (order == KisBackingRevisionOrder::Newer) {
                tracked->second.revision = state.revision;
                tracked->second.domain = domain;
                return true;
            }
            if (order == KisBackingRevisionOrder::Same)
                return tracked->second.domain == domain;
            // A callback may have delivered a newer state between observer
            // registration and this initial snapshot installation.
            return order == KisBackingRevisionOrder::Older;
        }

        bool observation(KisTileData *tileData,
                         KisPageAccessDomain *domain,
                         quint64 *revision) const
        {
            QMutexLocker locker(&m_mutex);
            auto tracked = m_payloads.find(tileData);
            while (tracked != m_payloads.cend() && tracked->second.transition) {
                m_transitionChanged.wait(&m_mutex);
                tracked = m_payloads.find(tileData);
            }
            if (tracked == m_payloads.cend() || !tracked->second.revision
                || tracked->second.domain == KisPageAccessDomain::Unknown) {
                return false;
            }
            if (domain) *domain = tracked->second.domain;
            if (revision) *revision = tracked->second.revision;
            return true;
        }

        bool transitionActive(KisTileData *tileData) const
        {
            QMutexLocker locker(&m_mutex);
            const auto tracked = m_payloads.find(tileData);
            return tracked != m_payloads.cend() && tracked->second.transition;
        }

        void untrack(KisTileData *tileData, quint64 slot)
        {
            PayloadMap::node_type released;
            SlotMap::node_type releasedSlot;
            QMutexLocker locker(&m_mutex);
            auto tracked = m_payloads.find(tileData);
            while (tracked != m_payloads.cend() && tracked->second.transition) {
                m_transitionChanged.wait(&m_mutex);
                tracked = m_payloads.find(tileData);
            }
            if (tracked == m_payloads.end() || tracked->second.slot != slot) return;
            if (tracked->second.is_linked()) m_dirty.erase(m_dirty.iterator_to(tracked->second));
            releasedSlot = m_slots.extract(slot);
            released = m_payloads.extract(tracked);
            m_hasChanges.store(!m_dirty.empty(), std::memory_order_release);
        }

        bool mayHaveChanges() const noexcept
        {
            return m_hasChanges.load(std::memory_order_acquire);
        }

        KisReplicaBackingDomainChanges changes(KisBackingBudgetController *budget) const
        {
            KisReplicaBackingDomainChanges result{budget
                ? KisMutationStorageAllocator<KisReplicaBackingDomainChange>(budget)
                : KisMutationStorageAllocator<KisReplicaBackingDomainChange>(m_admissions.get_allocator())};
            QMutexLocker locker(&m_mutex);
            result.reserve(m_dirty.size());
            for (const auto &payload : m_dirty)
                result.push_back({payload.revision, payload.slot, payload.domain, payload.bytes});
            return result;
        }

        void acknowledge(quint64 slot, quint64 revision)
        {
            QMutexLocker locker(&m_mutex);
            const auto entry = m_slots.find(slot);
            if (entry != m_slots.end()) {
                auto &payload = *entry->second;
                if (payload.is_linked() && payload.revision == revision)
                    m_dirty.erase(m_dirty.iterator_to(payload));
            }
            m_hasChanges.store(!m_dirty.empty(), std::memory_order_release);
        }

    private:
        void cancelTransition(KisTileData *tileData, quint64 serial) noexcept
        {
            QMutexLocker locker(&m_mutex);
            auto tracked = m_payloads.find(tileData);
            if (tracked != m_payloads.end() && tracked->second.transition == serial) {
                tracked->second.transition = 0;
                m_transitionChanged.wakeAll();
            }
        }

        void commitTransition(KisTileData *tileData, quint64 serial,
                              KisPageAccessDomain domain,
                              quint64 revision) noexcept
        {
            QMutexLocker locker(&m_mutex);
            auto tracked = m_payloads.find(tileData);
            Q_ASSERT(tracked != m_payloads.end());
            Q_ASSERT(tracked == m_payloads.end()
                     || tracked->second.transition == serial);
            if (tracked == m_payloads.end() || tracked->second.transition != serial)
                return;
            const auto order = kisCompareBackingRevision(
                revision, tracked->second.revision);
            Q_ASSERT(order == KisBackingRevisionOrder::Newer);
            if (order != KisBackingRevisionOrder::Newer)
                return;
            auto &payload = tracked->second;
            payload.revision = revision;
            payload.domain = domain;
            payload.transition = 0;
            if (!payload.is_linked()) m_dirty.push_back(payload);
            m_hasChanges.store(true, std::memory_order_release);
            m_transitionChanged.wakeAll();
        }

        QSharedPointer<KisBackingBudgetController> m_budget;
        mutable QMutex m_mutex;
        mutable QWaitCondition m_transitionChanged;
        KisReplicaProviderId m_provider;
        KisReplicaProviderEpoch m_providerEpoch;
        quint64 m_nextTransition = 0;
        PayloadMap m_payloads;
        SlotMap m_slots;
        boost::intrusive::list<Payload> m_dirty;
        std::atomic<bool> m_hasChanges{false};
        WeakAdmissions m_admissions;
    };

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
        const auto retirementCompletion = completions->allocatePending(completionSource);
        auto failCompletion = qScopeGuard([&]() {
            completions->complete(completion, KisCompletionStatus::Failed);
            if (retirementCompletion.isValid()) completions->complete(retirementCompletion, KisCompletionStatus::Failed);
        });
        if (!retirementCompletion.isValid())
            return KisReplicaOperation::failed(operation, QStringLiteral("tiles3 retirement completion preparation failed"));
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

        if (!tileData->blockSwapping()) {
            return KisReplicaOperation::failed(
                operation, QStringLiteral("tiles3 tile residency admission failed"));
        }
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
                           {QSharedPointer<Tiles3ResidentBinding>::create(tileData, handle, &handoffAdmission), physical, retirementCompletion});
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
    Tiles3HandoffAdmission handoffAdmission;
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
    QString *error,
    const QSharedPointer<KisBackingBudgetController> &processBudget)
try
{
    auto observer = QSharedPointer<Private::ResidencyObserver>::create(processBudget);
    const bool configured = d->configure(
        config, completions, QStringLiteral("tiles3"), error);
    if (configured) {
        observer->setProviderIdentity(config.provider, config.providerEpoch);
        d->residencyObserver = std::move(observer);
    }
    return configured;
}
catch (const std::bad_alloc &)
{
    KisPageStoreDetail::setError(error, QStringLiteral("tiles3 residency owner preparation failed"));
    return false;
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
    if (!sourceBinding || !sourceBinding->acquireRead(source.allocationIdentity(), false)) {
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

TileLease KisTiles3PageReplicaProvider::acquireTileReadCache(
    const KisCpuReadGuard &guard, TileLease reuse) const
{
    if (!guard.isValid()) return {};
    const auto binding = qSharedPointerDynamicCast<Tiles3ResidentBinding>(guard.m_binding);
    if (!binding) return {};
    const auto expected = binding->readIdentity(guard.version());
    {
        QMutexLocker lock(&d->mutex);
        const auto allocation = d->allocations.constFind(expected.allocation.slot);
        if (!(expected.provider == d->config.provider) ||
            !(expected.providerEpoch == d->config.providerEpoch) ||
            allocation == d->allocations.constEnd() || allocation->binding != binding) return {};
    }
    return Tiles3TileReadCache::prepare(binding, expected, binding->guardedTile(), std::move(reuse), true);
}

bool KisTiles3PageReplicaProvider::readCacheMatchesVersion(
    const KisTilePageStoreLease *cache, const KisPageVersion &version) const
{
    const auto *read = dynamic_cast<const Tiles3TileReadCache *>(cache);
    return read && read->matchesVersion(version);
}

TileLease KisTiles3PageReplicaProvider::acquireTileReadCache(
    KisPageLeaseId lease, TileLease reuse) const
{
    QMutexLocker lock(&d->mutex);
    const auto found = d->activeLeases.find(lease.value);
    if (!lease.isValid() || found == d->activeLeases.end() ||
        found->second.mode != KisPageAccessMode::Read) return {};
    const auto allocation = d->allocations.constFind(found->second.allocation.slot);
    if (allocation == d->allocations.constEnd() ||
        !allocation->binding->matchesAllocation(found->second.allocation)) return {};
    const KisReplicaAllocationIdentity expected{d->config.provider, d->config.providerEpoch, found->second.allocation};
    return Tiles3TileReadCache::prepare(allocation->binding, expected, allocation->tileData(), std::move(reuse), true);
}

TileLease KisTiles3PageReplicaProvider::prepareTileReadCache(
    const KisCpuWriteGuard &guard, TileLease reuse) const
{
    KisReplicaHandle replica;
    if (!guard.isValid() || !guard.providerBacking(&replica)) return {};
    QMutexLocker lock(&d->mutex);
    const auto allocation = d->findExactAllocation(replica);
    if (allocation == d->allocations.end()) return {};
    return Tiles3TileReadCache::prepare(allocation->binding, replica.allocationIdentity(),
                                        allocation->tileData(), std::move(reuse), false);
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
    if (!input->tile->blockSwapping())
        return false;
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
        if (!input->tile->blockSwapping()) {
            return KisReplicaOperation::failed(
                operation, QStringLiteral("immutable source residency admission failed"));
        }
        const auto unpin = qScopeGuard([&] { input->tile->unblockSwapping(); });
        return d->allocate(operation, targetVersion, descriptor, KisPageAccessDomain::CpuRam, input->tile);
    }
    const quint64 bytes = descriptor.minimumByteSize();
    if (!d->canAllocateHandle())
        return KisReplicaOperation::failed(operation, QStringLiteral("immutable alias budget or identity is exhausted"));
    d->consumeOperation(operation);
    const auto completion = d->completions->allocatePending(d->completionSource);
    if (!completion.isValid()) return KisReplicaOperation::failed(operation, QStringLiteral("alias completion is unavailable"));
    const auto retirementCompletion = d->completions->allocatePending(d->completionSource);
    auto failCompletion = qScopeGuard([&] {
        d->completions->complete(completion, KisCompletionStatus::Failed);
        if (retirementCompletion.isValid()) d->completions->complete(retirementCompletion, KisCompletionStatus::Failed);
    });
    if (!retirementCompletion.isValid())
        return KisReplicaOperation::failed(operation, QStringLiteral("alias retirement completion preparation failed"));
    const KisReplicaHandle handle = d->allocateHandle(targetVersion, descriptor);
    const quint64 physical = d->retainPhysical(input->tile, bytes);
    if (!physical) {
        return KisReplicaOperation::failed(operation, QStringLiteral("immutable alias physical budget is exhausted"));
    }
    d->allocations.insert(handle.allocation.slot,
         {QSharedPointer<Tiles3ResidentBinding>::create(input->tile, handle, &d->handoffAdmission), physical, retirementCompletion});
    ++d->work.adoptedPages; d->work.adoptedBytes += bytes;
    d->completions->complete(completion, KisCompletionStatus::Succeeded);
    failCompletion.dismiss();
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
    const auto retirementCompletion = d->completions->allocatePending(d->completionSource);
    if (!retirementCompletion.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("initial retirement completion preparation failed"));
        return {};
    }
    auto failCompletion = qScopeGuard([&] { d->completions->complete(retirementCompletion, KisCompletionStatus::Failed); });
    const KisReplicaHandle handle = d->allocateHandle(version, descriptor);
    const quint64 physical = d->retainPhysical(tileData, byteSize);
    if (!physical) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 initial physical payload budget is exhausted"));
        return {};
    }
    d->allocations.insert(handle.allocation.slot,
                          {QSharedPointer<Tiles3ResidentBinding>::create(tileData, handle, &d->handoffAdmission), physical, retirementCompletion});
    failCompletion.dismiss();
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
            return KisReplicaOperation::failed(
                operation, QStringLiteral("tiles3 retirement allocation is stale"));
        }
        if (d->residencyObserver->transitionActive(retired)) {
            return KisReplicaOperation::failed(
                operation, QStringLiteral("tiles3 residency transition is active"));
        }
        retired->ref();
        if (!retirement.allocation->binding->retire(replica.allocationIdentity())) {
            retired->deref();
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
    return {it->physicalBacking, domain, replica.layout.byteSize, revision};
}

KisReplicaBackingDomainChanges
KisTiles3PageReplicaProvider::backingDomainChanges(KisBackingBudgetController *budget) const
{
    return d->residencyObserver ? d->residencyObserver->changes(budget)
        : KisReplicaBackingDomainChanges(KisMutationStorageAllocator<KisReplicaBackingDomainChange>(budget));
}

bool KisTiles3PageReplicaProvider::mayHaveBackingDomainChanges() const noexcept
{
    return d->residencyObserver && d->residencyObserver->mayHaveChanges();
}

void KisTiles3PageReplicaProvider::acknowledgeBackingDomainChange(
    quint64 physicalSlot, quint64 revision)
{
    if (d->residencyObserver) d->residencyObserver->acknowledge(physicalSlot, revision);
}

bool KisTiles3PageReplicaProvider::registerBackingDomainAdmission(
    const QSharedPointer<KisReplicaBackingDomainAdmission> &admission,
    QString *error)
{
    QMutexLocker locker(&d->handoffAdmission.mutex);
    if (d->handoffAdmission.claims) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 physical handoff blocks owner registration"));
        return false;
    }
    bool registered = false;
    try {
        registered = d->residencyObserver && d->residencyObserver->registerAdmission(admission, &d->handoffAdmission.singleOwner);
    } catch (const std::bad_alloc &) {} // Original registration remains unaccepted.
    KisPageStoreDetail::setError(
        error, registered ? QString{}
                          : QStringLiteral("tiles3 backing-domain admission is invalid"));
    return registered;
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
    const auto leaseIt = d->activeLeases.find(lease.value);
    if (leaseIt == d->activeLeases.end()) return nullptr;
    const auto allocationIt = d->allocations.constFind(leaseIt->second.allocation.slot);
    if (allocationIt == d->allocations.constEnd() ||
        !allocationIt->binding->matchesAllocation(leaseIt->second.allocation)) {
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

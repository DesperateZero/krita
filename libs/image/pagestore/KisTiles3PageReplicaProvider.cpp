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

#include <QMutexLocker>
#include <QScopeGuard>
#include "KisPageWaitCondition_p.h"

#include <atomic>
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/set.hpp>
#include <cstring>
#include <exception>
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

struct Tiles3PhysicalBacking final : KisTileDataStore::ResidencyObservers, boost::intrusive::list_base_hook<>
{
    using DirtyHook = boost::intrusive::list_base_hook<>;
    using DirtyHook::is_linked;
    Tiles3PhysicalBacking(std::shared_ptr<KisTileDataResidencyObserver> owner, quint64 identity, quint64 size) noexcept
        : ResidencyObservers(std::move(owner)), slot(identity), bytes(size)
    { prepareMember(registration); }
    KisTileData *tileData = nullptr;
    boost::intrusive::set_member_hook<> tileIndex;
    quint64 slot;
    quint64 bytes;
    quint64 revision = 0;
    KisPageAccessDomain domain = KisPageAccessDomain::Unknown;
    quint64 transition = 0;
    quint64 logicalReferences = 1;
    boost::intrusive::set_member_hook<> slotIndex;
    KisTileDataStore::ResidencyObserverRegistration registration;
private:
    void destroy() noexcept override {
        auto storage = allocationOwner();
        this->~Tiles3PhysicalBacking();
        storage->deallocate(this, sizeof(*this), alignof(Tiles3PhysicalBacking));
    }
};

class Tiles3ResidentBinding final : public KisCpuResidentBinding, public boost::intrusive::set_base_hook<>
{
public:
    Tiles3ResidentBinding(const KisReplicaHandle &handle, Tiles3HandoffAdmission *admission)
        : KisCpuResidentBinding(handle), m_admission(admission) {}
    void adoptTile(KisTileData *tile) noexcept
    {
        Q_ASSERT(!m_tile && tile);
        m_tile = tile;
        m_tile->ref();
    }
    ~Tiles3ResidentBinding() override { if (m_tile) m_tile->deref(); }
    Tiles3PhysicalBacking *physicalBacking = nullptr; // Borrowed only by the live provider index.
private:
    friend struct KisCpuResidentAllocationIndex<Tiles3ResidentBinding>;
    std::shared_ptr<Tiles3ResidentBinding> providerOwnership;
    KisCompletionTicket retirementCompletion;
public:
    // Only used while an owner-issued immutable read guard holds this binding.
    KisTileData *guardedTile() const { return m_tile; }
    bool belongsTo(const Tiles3HandoffAdmission *admission) const { return m_admission == admission; }
private:
    bool supportsBackingHandoff() const override { return true; }
    void *tryClaimStorageForHandoff() noexcept override
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
    void unpinStorage() noexcept override { m_tile->unblockSwapping(); }
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
    void releaseStorage() noexcept override
    {
        if (m_tile) { m_tile->deref(); m_tile = nullptr; }
    }
    KisTileData *m_tile = nullptr;
    // Used only by a short claim whose KisCpuBackingHandoff retains provider.
    Tiles3HandoffAdmission *m_admission;
};

// A derived identity hint, not a root/transaction or a storage owner. KisTile
// retains its existing COW reference while caching this object. Only a fresh
// binding pin permits tileData(); finish returns that pin without discarding
// the cache storage. There is no provider map lookup on a warm read.
class Tiles3TileReadCache final : public KisTilePageStoreLease, public KisPageProcessStorageObject
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
    bool finish(TileLeaseRelease * = nullptr) override
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
    static TileLease prepare(const std::shared_ptr<KisCpuResidentBinding> &binding,
                             const KisReplicaAllocationIdentity &expected,
                             KisTileData *tile, TileLease reuse, bool pin)
    {
        if (!binding || !expected.isValid() || !tile) return {};
        auto *cache = dynamic_cast<Tiles3TileReadCache *>(reuse.get());
        // A nested reader's capability must never be stolen for a writer.
        if (cache && cache->readPinned()) return {};
        if (!cache) {
            // This is only a derived read hint. Capacity refusal is the same
            // recoverable cache miss that the public bridge already returns.
            try {
                reuse = std::make_unique<Tiles3TileReadCache>();
            } catch (const std::bad_alloc &) {
                return {};
            }
            cache = static_cast<Tiles3TileReadCache *>(reuse.get());
        }
        cache->m_binding = binding;
        cache->m_expected = expected;
        cache->m_tile = tile;
        if (pin && cache->repinRead() != ReadPinResult::Ready) return {};
        return reuse;
    }
private:
    std::shared_ptr<KisCpuResidentBinding> m_binding;
    KisReplicaAllocationIdentity m_expected;
    KisTileData *m_tile = nullptr;
    bool m_pinned = false;
};

class Tiles3ReplicaSource final : public KisPageReplicaSource
{
public:
    Tiles3ReplicaSource(KisReplicaProviderId provider, KisReplicaProviderEpoch epoch,
                       const KisPageAllocationDescriptor &descriptor,
                       std::shared_ptr<const quint8> owner, KisTileData *tile)
        : KisPageReplicaSource(provider, epoch, descriptor), owner(std::move(owner)), tile(tile)
    { tile->acquire(); }
    ~Tiles3ReplicaSource() override { tile->release(); }
    const std::shared_ptr<const quint8> owner;
    KisTileData *const tile;
};

class KisTiles3PageReplicaProvider::Private : public KisCpuResidentAllocationIndex<Tiles3ResidentBinding>
{
public:
    using Storage = KisMutationStorageAllocator<Private>;
    explicit Private(const Storage &allocator)
        : storage(allocator), sourceOwner(std::allocate_shared<const quint8>(allocator, 0)) {}
    class ResidencyObserver final : public KisTileDataResidencyObserver
    {
        using Payload = Tiles3PhysicalBacking;
        struct TileKey {
            using type = KisTileData *;
            KisTileData *operator()(const Payload &payload) const { return payload.tileData; }
        };
        using PayloadMap = boost::intrusive::set<Payload,
            boost::intrusive::member_hook<Payload, boost::intrusive::set_member_hook<>, &Payload::tileIndex>,
            boost::intrusive::key_of_value<TileKey>, boost::intrusive::compare<std::less<KisTileData *>>>;
        struct SlotKey {
            using type = quint64;
            const quint64 &operator()(const Payload &payload) const { return payload.slot; }
        };
        // Provider indices borrow one original physical record. The tile index
        // owns one of its existing registration pins; swap snapshots may keep
        // the same record alive after all provider indices have been removed.
        using SlotMap = boost::intrusive::set<Payload,
            boost::intrusive::member_hook<Payload, boost::intrusive::set_member_hook<>, &Payload::slotIndex>,
            boost::intrusive::key_of_value<SlotKey>>;
        using Admissions = std::vector<std::shared_ptr<KisReplicaBackingDomainAdmission>,
            KisMutationStorageAllocator<std::shared_ptr<KisReplicaBackingDomainAdmission>>>;
        using WeakAdmissions = std::vector<std::weak_ptr<KisReplicaBackingDomainAdmission>,
            KisMutationStorageAllocator<std::weak_ptr<KisReplicaBackingDomainAdmission>>>;
    public:
        using PreparedPayload = boost::intrusive_ptr<Payload>;
        auto storageAllocator() const { return m_admissions.get_allocator(); }
        explicit ResidencyObserver(const std::shared_ptr<KisBackingBudgetController> &process)
        {
            if (process && !m_budget.configureSharedNonPayloadBudget(process)) throw std::bad_alloc();
            m_admissions = WeakAdmissions(KisMutationStorageAllocator<Payload>::retained(&m_budget));
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
            std::vector<std::shared_ptr<KisReplicaBackingDomainReservation>,
                KisMutationStorageAllocator<std::shared_ptr<KisReplicaBackingDomainReservation>>> m_reservations;
        };

        void setProviderIdentity(KisReplicaProviderId provider,
                                 KisReplicaProviderEpoch epoch)
        {
            QMutexLocker locker(&m_mutex);
            m_provider = provider;
            m_providerEpoch = epoch;
        }

        bool registerAdmission(
            const std::shared_ptr<KisReplicaBackingDomainAdmission> &admission, bool *singleOwner)
        {
            if (!admission)
                return false;
            QMutexLocker locker(&m_mutex);
            for (const auto &existing : std::as_const(m_admissions)) {
                if (existing.lock() == admission)
                    return true;
            }
            // Conservative provider-wide exclusion: expiry does not prove
            // that all replicas imported by that owner have been released.
            // Do not re-enable handoff merely because an observer disappeared.
            m_admissions.push_back(admission);
            if (m_admissions.size() > 1) *singleOwner = false;
            return true;
        }

        std::shared_ptr<KisTileDataResidencyTransition> prepareResidencyChange(
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
                    || tracked->domain != sourceDomain
                    || tracked->revision != source.revision
                    || tracked->transition) {
                    KisPageStoreDetail::setError(
                        error, QStringLiteral("tiles3 residency source changed"));
                    return {};
                }
                admissions.reserve(m_admissions.size());
                for (const auto &weak : std::as_const(m_admissions)) {
                    auto admission = weak.lock();
                    if (!admission)
                        continue;
                    admissions.push_back(std::move(admission));
                }
                m_admissions.erase(std::remove_if(m_admissions.begin(), m_admissions.end(),
                    [](const auto &weak) { return weak.expired(); }), m_admissions.end());
                ++m_nextTransition;
                if (!m_nextTransition) ++m_nextTransition;
                serial = m_nextTransition;
                tracked->transition = serial;
                slot = tracked->slot;
                bytes = tracked->bytes;
            }

            auto result = std::allocate_shared<Transition>(m_admissions.get_allocator(),
                this, tileData, serial, targetDomain);
            // This original Transition now owns cancellation of its serial.
            // The local guard covers only failure before that acceptance.
            cancel.dismiss();
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
            KisPageStoreDetail::setError(error, {});
            return result;
        }
        catch (const std::bad_alloc &)
        {
            KisPageStoreDetail::setError(error, QStringLiteral("tiles3 residency preparation storage is unavailable"));
            return {};
        }

        PreparedPayload preparePayload(const std::shared_ptr<KisTileDataResidencyObserver> &owner,
                                       quint64 identity, quint64 bytes)
        {
            Q_ASSERT(owner.get() == this);
            void *raw = allocate(sizeof(Payload), alignof(Payload));
            return PreparedPayload(::new (raw) Payload(owner, identity, bytes), false);
        }

        quint64 physicalIdentity(KisTileData *tile) const
        {
            QMutexLocker locker(&m_mutex);
            const auto found = m_payloads.find(tile);
            return found == m_payloads.end() ? 0 : found->slot;
        }

        Payload *retainExisting(KisTileData *tile, quint64 bytes)
        {
            QMutexLocker locker(&m_mutex);
            const auto found = m_payloads.find(tile);
            if (found == m_payloads.end() || found->bytes != bytes
                || found->logicalReferences == std::numeric_limits<quint64>::max()) return nullptr;
            ++found->logicalReferences;
            return &*found;
        }

        bool track(KisTileData *tile, PreparedPayload &prepared)
        {
            QMutexLocker locker(&m_mutex);
            if (!tile || !prepared || prepared->tileIndex.is_linked()
                || m_payloads.count(tile) || m_slots.count(prepared->slot)) return false;
            prepared->tileData = tile;
            m_payloads.insert(*prepared);
            m_slots.insert(*prepared);
            intrusive_ptr_add_ref(prepared.get()); // The original tile index owns this pin.
            return true;
        }

        bool releaseReference(Payload *payload)
        {
            QMutexLocker locker(&m_mutex);
            Q_ASSERT(payload && payload->tileIndex.is_linked() && payload->logicalReferences);
            return !--payload->logicalReferences;
        }

        Payload *firstPayload()
        {
            QMutexLocker locker(&m_mutex);
            return m_payloads.empty() ? nullptr : &*m_payloads.begin();
        }

        quint64 residentBytes() const
        {
            QMutexLocker locker(&m_mutex);
            // Swap holds the physical gate before entering this observer.
            // Wait for its original serial, never take a tile gate here.
            for (;;) {
                const bool active = std::any_of(m_payloads.begin(), m_payloads.end(),
                    [](const auto &entry) { return entry.transition != 0; });
                if (!active) break;
                m_transitionChanged.wait(&m_mutex);
            }
            quint64 bytes = 0;
            for (const auto &payload : m_payloads)
                if (payload.domain == KisPageAccessDomain::CpuRam) bytes += payload.bytes;
            return bytes;
        }

        bool initialize(Payload *tracked,
                        const KisTileDataResidencyState &state)
        {
            if (!state.isValid())
                return false;
            QMutexLocker locker(&m_mutex);
            Q_ASSERT(tracked && tracked->tileIndex.is_linked());
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

        bool observation(Payload *tracked,
                         KisPageAccessDomain *domain,
                         quint64 *revision) const
        {
            QMutexLocker locker(&m_mutex);
            Q_ASSERT(tracked && tracked->tileIndex.is_linked());
            while (tracked->transition) m_transitionChanged.wait(&m_mutex);
            if (!tracked->revision
                || tracked->domain == KisPageAccessDomain::Unknown) {
                return false;
            }
            if (domain) *domain = tracked->domain;
            if (revision) *revision = tracked->revision;
            return true;
        }

        bool transitionActive(const Payload *tracked) const
        {
            QMutexLocker locker(&m_mutex);
            Q_ASSERT(tracked && tracked->tileIndex.is_linked());
            return tracked->transition != 0;
        }

        void untrack(Payload *tracked)
        {
            PreparedPayload released;
            QMutexLocker locker(&m_mutex);
            Q_ASSERT(tracked && tracked->tileIndex.is_linked());
            while (tracked->transition) m_transitionChanged.wait(&m_mutex);
            if (tracked->is_linked()) m_dirty.erase(m_dirty.iterator_to(*tracked));
            m_slots.erase(m_slots.iterator_to(*tracked));
            released.reset(&*tracked, false); // Take the original index pin; release outside its gate.
            m_payloads.erase(m_payloads.iterator_to(*tracked));
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
                auto &payload = *entry;
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
            if (tracked != m_payloads.end() && tracked->transition == serial) {
                tracked->transition = 0;
                m_transitionChanged.wakeAll();
            }
        }

        void commitTransition(KisTileData *tileData, quint64 serial,
                              KisPageAccessDomain domain,
                              quint64 revision) noexcept
        {
            QMutexLocker locker(&m_mutex);
            auto tracked = m_payloads.find(tileData);
            // Physical movement has committed; untrack waits for this serial.
            if (tracked == m_payloads.end() || tracked->transition != serial)
                std::terminate();
            const auto order = kisCompareBackingRevision(
                revision, tracked->revision);
            if (order != KisBackingRevisionOrder::Newer)
                std::terminate();
            auto &payload = *tracked;
            payload.revision = revision;
            payload.domain = domain;
            payload.transition = 0;
            if (!payload.is_linked()) m_dirty.push_back(payload);
            m_hasChanges.store(true, std::memory_order_release);
            m_transitionChanged.wakeAll();
        }

        void *do_allocate(size_t bytes, size_t alignment) override {
            return kisAllocateMutationStorage(&m_budget, bytes, alignment);
        }
        void do_deallocate(void *data, size_t bytes, size_t alignment) override {
            kisFreeMutationStorage(&m_budget, data, bytes, alignment);
        }

        KisBackingBudgetController m_budget;
        mutable QMutex m_mutex;
        mutable KisPageWaitCondition m_transitionChanged;
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
        while (residencyObserver) {
            auto *physical = residencyObserver->firstPayload();
            if (!physical) break;
            KisTileDataStore::instance()->unregisterResidencyObserver(physical->tileData, residencyObserver);
            residencyObserver->untrack(physical);
        }
    }

    Storage storage;
    const std::shared_ptr<const quint8> sourceOwner;
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

    struct PreparedAdoption {
        KisReplicaHandle handle;
        std::shared_ptr<Tiles3ResidentBinding> allocation;
        ResidencyObserver::PreparedPayload physical;
    };

    PreparedAdoption prepareAdoption(const KisPageVersion &version,
        const KisPageAllocationDescriptor &descriptor, KisTileData *tile = nullptr)
    {
        const bool existing = tile && residencyObserver->physicalIdentity(tile);
        const quint64 bytes = descriptor.minimumByteSize();
        if (!canAllocateHandle() || (!existing &&
            (bytes > config.budgetBytes - qMin(config.budgetBytes, committedBytes)
             || !nextPhysicalSlot || nextPhysicalSlot == std::numeric_limits<quint64>::max()))) return {};
        PreparedAdoption prepared;
        prepared.handle = allocateHandle(version, descriptor);
        // The one original binding/control block now includes the complete
        // logical record. Admit it before any physical result or adoption.
        prepared.allocation = std::allocate_shared<Tiles3ResidentBinding>(
            residencyObserver->storageAllocator(), prepared.handle, &handoffAdmission);
        if (!existing) {
            prepared.physical = residencyObserver->preparePayload(residencyObserver, nextPhysicalSlot, bytes);
        }
        return prepared;
    }

    Tiles3PhysicalBacking *retainPhysical(KisTileData *tileData, quint64 bytes,
                           ResidencyObserver::PreparedPayload &prepared)
    {
        if (auto *existing = residencyObserver->retainExisting(tileData, bytes)) {
            return existing;
        }
        if (!tileData || !prepared
            || !residencyObserver->track(tileData, prepared)) {
            return {};
        }
        auto rollback = qScopeGuard([&] {
            KisTileDataStore::instance()->unregisterResidencyObserver(tileData, residencyObserver);
            residencyObserver->untrack(prepared.get());
        });
        KisTileDataResidencyState initialState;
        if (!KisTileDataStore::instance()->registerResidencyObserver(
                tileData, residencyObserver, &initialState, prepared)
            || !residencyObserver->initialize(prepared.get(), initialState)) {
            return {};
        }
        ++nextPhysicalSlot;
        committedBytes += bytes;
        rollback.dismiss();
        return prepared.get();
    }

    void releasePhysical(Tiles3PhysicalBacking *physical)
    {
        if (residencyObserver->releaseReference(physical)) {
            const quint64 bytes = physical->bytes;
            Q_ASSERT(committedBytes >= bytes);
            committedBytes -= bytes;
            KisTileDataStore::instance()->unregisterResidencyObserver(physical->tileData, residencyObserver);
            residencyObserver->untrack(physical);
        }
    }

    bool adopt(PreparedAdoption &prepared, KisTileData *tile,
               const KisCompletionTicket &retirementCompletion)
    {
        if (!prepared.handle.isValid() || !prepared.allocation || !tile) return false;
        const auto &binding = prepared.allocation;
        Q_ASSERT(binding);
        binding->adoptTile(tile);
        auto *physical = retainPhysical(tile, prepared.handle.layout.byteSize, prepared.physical);
        if (!physical) return false;
        auto rollback = qScopeGuard([&] { releasePhysical(physical); });
        binding->physicalBacking = physical;
        if (!installAllocation(prepared.allocation, retirementCompletion)) return false;
        rollback.dismiss();
        return true;
    }

    KisCpuReplicaPreparation allocate(
        const KisPageVersion &version,
        const KisPageAllocationDescriptor &descriptor,
        KisPageAccessDomain domain,
        KisTileData *copySource = nullptr, const KisCpuPagePayload *payload = nullptr,
        const KisPageOperationId *operation = nullptr)
    try
    {
        // This allocator returns independent mutable storage only; complete
        // payload aliases use an explicit immutable source instead.
        KisTileData *tileData = nullptr;
        auto releaseTile = qScopeGuard([&]() {
            if (tileData) tileData->deref();
        });
        if (!config.isValid()) {
            return KisCpuReplicaPreparation::failed(QStringLiteral("tiles3 provider is not configured"));
        }
        if (!version.isValid() ||
            domain != KisPageAccessDomain::CpuRam ||
            !supportsNativeTileLayout(descriptor) ||
            (operation && !operationAvailable(*operation)) ||
            (payload && (copySource || !payload->isValidFor(descriptor)))) {
            return KisCpuReplicaPreparation::failed(QStringLiteral("tiles3 allocation request is invalid"));
        }
        if (operation) consumeOperation(*operation);

        const quint64 byteSize = descriptor.minimumByteSize();
        if (byteSize > config.budgetBytes - qMin(config.budgetBytes, committedBytes)
            || !canAllocateHandle()) {
            return KisCpuReplicaPreparation::failed(QStringLiteral("tiles3 page budget or identity is exhausted"));
        }
        const auto retirementCompletion = completions->allocatePending(completionSource);
        auto failCompletion = qScopeGuard([&]() {
            if (retirementCompletion.isValid()) completions->completePrepared(retirementCompletion, KisCompletionStatus::Failed);
        });
        if (!retirementCompletion.isValid())
            return KisCpuReplicaPreparation::failed(QStringLiteral("tiles3 retirement completion preparation failed"));
        auto prepared = prepareAdoption(version, descriptor);
        if (!prepared.handle.isValid())
            return KisCpuReplicaPreparation::failed(QStringLiteral("tiles3 physical identity or budget is exhausted"));
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
            return KisCpuReplicaPreparation::failed(QStringLiteral("tiles3 tile allocation failed"));
        }

        if (!tileData->blockSwapping()) {
            return KisCpuReplicaPreparation::failed(QStringLiteral("tiles3 tile residency admission failed"));
        }
        const quintptr address = reinterpret_cast<quintptr>(tileData->data());
        const bool aligned = address != 0 &&
            address % descriptor.format.pixelAlignment == 0;
        tileData->unblockSwapping();
        if (!aligned) {
            return KisCpuReplicaPreparation::failed(QStringLiteral("tiles3 tile allocation alignment is insufficient"));
        }

        // The original index consumes prepared.allocation. Only a direct
        // receiver retains this same binding; public callers receive a handle.
        std::shared_ptr<KisCpuResidentBinding> native = operation ? nullptr : prepared.allocation;
        if (!adopt(prepared, tileData, retirementCompletion)) {
            return KisCpuReplicaPreparation::failed(QStringLiteral("tiles3 physical payload budget is exhausted"));
        }
        failCompletion.dismiss();
        return operation ? KisCpuReplicaPreparation::external(
            {KisPageRequestStatus::Ready, *operation, prepared.handle, readyCompletion, {}})
            : KisCpuReplicaPreparation(std::move(native));
    }
    catch (const std::bad_alloc &)
    {
        return KisCpuReplicaPreparation::failed(QStringLiteral("tiles3 adoption storage preparation failed"));
    }

    KisCpuReplicaPreparation prepareCopy(const KisPageWriteSource &source,
        const KisPageVersion &targetVersion, const KisPageAllocationDescriptor &descriptor,
        const std::shared_ptr<KisCpuResidentBinding> &knownBinding,
        const KisPageOperationId *operation = nullptr)
    {
        std::shared_ptr<Tiles3ResidentBinding> located;
        auto *binding = dynamic_cast<Tiles3ResidentBinding *>(knownBinding.get());
        if (knownBinding) {
            if (!binding || !binding->belongsTo(&handoffAdmission) || !binding->matchesReadIdentity(source.allocationIdentity(), source.version))
                return KisCpuReplicaPreparation::failed(QStringLiteral("tiles3 native copy source binding is foreign or stale"));
        } else {
            const auto it = findExactAllocation(source.handle());
            if (it != allocations.end()) located = providerBinding(it);
            binding = located.get();
        }
        KisTileData *sourceTile = binding ? binding->guardedTile() : nullptr;
        if (!targetVersion.isValid() || !(source.version.key == targetVersion.key) ||
            targetVersion.generation.value <= source.version.generation.value ||
            !source.layout().matches(descriptor) || !sourceTile) {
            return KisCpuReplicaPreparation::failed(QStringLiteral("tiles3 native copy source is stale or mutable"));
        }
        // Same exact source and physical pin for native and public callers.
        // A supplied stable binding removes location, never read exclusion.
        if (!binding->acquireRead(source.allocationIdentity(), false))
            return KisCpuReplicaPreparation::failed(QStringLiteral("tiles3 native copy source binding is busy or retired"));
        const auto releaseSource = qScopeGuard([&] { binding->releaseRead(); });
        return allocate(targetVersion, descriptor, source.domain(), sourceTile, nullptr, operation);
    }

    KisCpuReplicaPreparation prepareSource(const std::shared_ptr<const KisPageReplicaSource> &source,
        const KisPageVersion &targetVersion, const KisPageAllocationDescriptor &descriptor,
        KisReplicaSourceUse use, const KisPageOperationId *operation = nullptr)
    try
    {
        const auto input = std::dynamic_pointer_cast<const Tiles3ReplicaSource>(source);
        if (!config.isValid() || !input || input->owner != sourceOwner ||
            !(input->descriptor() == descriptor) || !targetVersion.isValid() ||
            !supportsNativeTileLayout(descriptor) || (operation && !operationAvailable(*operation)) ||
            (use != KisReplicaSourceUse::ImmutableAlias && use != KisReplicaSourceUse::WritableCopy)) {
            return KisCpuReplicaPreparation::failed(QStringLiteral("immutable source is foreign, stale or incompatible"));
        }
        // Aliasing never forces swap-in just to validate an address.
        if (use == KisReplicaSourceUse::WritableCopy) {
            if (!input->tile->blockSwapping())
                return KisCpuReplicaPreparation::failed(QStringLiteral("immutable source residency admission failed"));
            const auto unpin = qScopeGuard([&] { input->tile->unblockSwapping(); });
            return allocate(targetVersion, descriptor, KisPageAccessDomain::CpuRam, input->tile, nullptr, operation);
        }
        if (!canAllocateHandle())
            return KisCpuReplicaPreparation::failed(QStringLiteral("immutable alias budget or identity is exhausted"));
        if (operation) consumeOperation(*operation);
        const auto retirementCompletion = completions->allocatePending(completionSource);
        auto failCompletion = qScopeGuard([&] {
            if (retirementCompletion.isValid()) completions->completePrepared(retirementCompletion, KisCompletionStatus::Failed);
        });
        if (!retirementCompletion.isValid())
            return KisCpuReplicaPreparation::failed(QStringLiteral("alias retirement completion preparation failed"));
        auto prepared = prepareAdoption(targetVersion, descriptor, input->tile);
        std::shared_ptr<KisCpuResidentBinding> native = operation ? nullptr : prepared.allocation;
        if (!adopt(prepared, input->tile, retirementCompletion))
            return KisCpuReplicaPreparation::failed(QStringLiteral("immutable alias physical budget is exhausted"));
        ++work.adoptedPages; work.adoptedBytes += descriptor.minimumByteSize();
        failCompletion.dismiss();
        return operation ? KisCpuReplicaPreparation::external(
            {KisPageRequestStatus::Ready, *operation, prepared.handle, readyCompletion, {}})
            : KisCpuReplicaPreparation(std::move(native));
    }
    catch (const std::bad_alloc &)
    {
        return KisCpuReplicaPreparation::failed(QStringLiteral("immutable alias storage preparation failed"));
    }

    quint64 nextPhysicalSlot = 1;
    std::shared_ptr<ResidencyObserver> residencyObserver;
    Tiles3HandoffAdmission handoffAdmission;
    KisTiles3PayloadWork work;
};

KisTiles3PageReplicaProvider::KisTiles3PageReplicaProvider(
    const KisMutationStorageAllocator<KisTiles3PageReplicaProvider> &storage)
{
    Private::Storage allocator(storage);
    auto *raw = allocator.allocate(1);
    try { std::allocator_traits<Private::Storage>::construct(allocator, raw, allocator); }
    catch (...) { allocator.deallocate(raw, 1); throw; }
    d.reset(raw);
}

void KisTiles3PageReplicaProvider::PrivateReleaser::cleanup(Private *owner)
{
    if (!owner) return;
    auto storage = owner->storage;
    std::destroy_at(owner);
    storage.deallocate(owner, 1);
}

KisTiles3PageReplicaProvider::~KisTiles3PageReplicaProvider()
{
    d->revokeBindings();
}

bool KisTiles3PageReplicaProvider::configure(
    const KisCpuResidentReplicaProviderConfig &config,
    const std::shared_ptr<KisCompletionRegistry> &completions,
    QString *error,
    const std::shared_ptr<KisBackingBudgetController> &processBudget)
try
{
    const auto parent = processBudget ? processBudget : kisAcquirePageStoreBootstrapBudget(error);
    if (!parent) return false;
    KisBackingBudgetController coldStorage;
    if (!coldStorage.configureSharedNonPayloadBudget(parent, error)) return false;
    auto observer = std::allocate_shared<Private::ResidencyObserver>(
        KisMutationStorageAllocator<Private::ResidencyObserver>::retained(&coldStorage), parent);
    const bool configured = d->configure(
        config, completions, error);
    if (configured) {
        observer->setProviderIdentity(config.provider, config.providerEpoch);
        d->activeLeases = KisCpuBindingLeaseMap(observer->storageAllocator());
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
    return {{KisPageAccessDomain::CpuRam},
        {{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer}},
        true, true, true, true, true, true};
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
    return d->allocate(version, descriptor, domain, nullptr, nullptr, &operation).takeExternalOperation(operation);
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
    return d->allocate(version, descriptor, KisPageAccessDomain::CpuRam, nullptr, &payload, &operation)
        .takeExternalOperation(operation);
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
    return d->prepareCopy(source, targetVersion, descriptor, {}, &operation).takeExternalOperation(operation);
}

KisCpuReplicaPreparation KisTiles3PageReplicaProvider::prepareCpuReplica(const KisCpuReplicaInput &input)
{
    QMutexLocker locker(&d->mutex);
    auto result = [&]() -> KisCpuReplicaPreparation {
        using Kind = KisCpuReplicaInput::Kind;
        switch (input.kind) {
        case Kind::Default:
            return d->allocate(input.version, input.descriptor, KisPageAccessDomain::CpuRam);
        case Kind::Copy:
            return d->prepareCopy(input.source, input.version, input.descriptor, input.source.binding());
        case Kind::Payload:
            return input.payload ? d->allocate(input.version, input.descriptor,
                KisPageAccessDomain::CpuRam, nullptr, input.payload)
                : KisCpuReplicaPreparation::failed(QStringLiteral("CPU payload is missing"));
        case Kind::SourceCopy:
        case Kind::Alias:
            return d->prepareSource(input.initialization, input.version, input.descriptor,
                input.kind == Kind::Alias ? KisReplicaSourceUse::ImmutableAlias : KisReplicaSourceUse::WritableCopy);
        }
        return KisCpuReplicaPreparation::failed(QStringLiteral("CPU preparation kind is invalid"));
    }();
    // Only this actual producer may sign a native result. A custom provider
    // forwarding another producer's result cannot assume its direct authority.
    if (auto *native = std::get_if<KisCpuReplicaPreparation::Native>(&result.m_result)) {
        Q_ASSERT(!native->issuer);
        native->issuer = this;
    }
    return result;
}


std::shared_ptr<const KisPageReplicaSource> KisTiles3PageReplicaProvider::captureCompletedTileSource(
    const KisPageAllocationDescriptor &descriptor, KisTileData *tileData, QString *error) try
{
    if (!supportsNativeTileLayout(descriptor) ||
        descriptor.format.pixelAlignment > alignof(void *) || !tileData ||
        tileData->pixelSize() != descriptor.format.pixelStride) {
        KisPageStoreDetail::setError(error, QStringLiteral("completed tile source has an invalid layout")); return {};
    }
    QMutexLocker locker(&d->mutex);
    if (!d->config.isValid()) { KisPageStoreDetail::setError(error, QStringLiteral("source provider is unavailable")); return {}; }
    auto source = std::allocate_shared<Tiles3ReplicaSource>(d->residencyObserver->storageAllocator(),
        d->config.provider, d->config.providerEpoch, descriptor, d->sourceOwner, tileData);
    KisPageStoreDetail::setError(error, {});
    return source;
}
catch (const std::bad_alloc &)
{
    KisPageStoreDetail::setError(error, QStringLiteral("completed tile source storage was refused"));
    return {};
}

bool KisTiles3PageReplicaProvider::sourceMatchesReadGuard(
    const std::shared_ptr<const KisPageReplicaSource> &source, const KisCpuReadGuard &guard) const
{
    const auto input = std::dynamic_pointer_cast<const Tiles3ReplicaSource>(source);
    return input && input->owner == d->sourceOwner &&
        input->tile == tileDataForCpuReadGuard(guard);
}

KisTileData *KisTiles3PageReplicaProvider::tileDataForCpuReadGuard(const KisCpuReadGuard &guard) const
{
    const auto binding = std::dynamic_pointer_cast<Tiles3ResidentBinding>(guard.m_binding);
    return guard.isValid() && binding ? binding->guardedTile() : nullptr;
}

TileLease KisTiles3PageReplicaProvider::acquireTileReadCache(
    const KisCpuReadGuard &guard, TileLease reuse) const
{
    if (!guard.isValid()) return {};
    const auto binding = std::dynamic_pointer_cast<Tiles3ResidentBinding>(guard.m_binding);
    if (!binding) return {};
    const auto expected = binding->readIdentity(guard.version());
    {
        QMutexLocker lock(&d->mutex);
        const auto allocation = d->allocations.find(expected.allocation.slot);
        if (!(expected.provider == d->config.provider) ||
            !(expected.providerEpoch == d->config.providerEpoch) ||
            allocation == d->allocations.end() || d->providerBinding(allocation) != binding) return {};
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
    const auto allocation = d->allocations.find(found->second.allocation.slot);
    if (allocation == d->allocations.end() ||
        !allocation->matchesAllocation(found->second.allocation)) return {};
    const KisReplicaAllocationIdentity expected{d->config.provider, d->config.providerEpoch, found->second.allocation};
    return Tiles3TileReadCache::prepare(d->providerBinding(allocation), expected, allocation->guardedTile(), std::move(reuse), true);
}

TileLease KisTiles3PageReplicaProvider::prepareTileReadCache(
    const KisCpuWriteGuard &guard, TileLease reuse) const
{
    KisReplicaHandle replica;
    if (!guard.isValid() || !guard.providerBacking(&replica)) return {};
    QMutexLocker lock(&d->mutex);
    const auto allocation = d->findExactAllocation(replica);
    if (allocation == d->allocations.end()) return {};
    return Tiles3TileReadCache::prepare(d->providerBinding(allocation), replica.allocationIdentity(),
                                        allocation->guardedTile(), std::move(reuse), false);
}

std::shared_ptr<const KisPageReplicaSource> KisTiles3PageReplicaProvider::captureCpuReadSource(
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
    const std::shared_ptr<const KisPageReplicaSource> &source,
    const KisPageAllocationDescriptor &descriptor, void *destination,
    quint32 rowStride, quint64 byteSize)
{
    const auto input = std::dynamic_pointer_cast<const Tiles3ReplicaSource>(source);
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
    KisPageOperationId operation, const std::shared_ptr<const KisPageReplicaSource> &source,
    const KisPageVersion &targetVersion, const KisPageAllocationDescriptor &descriptor,
    KisReplicaSourceUse use, KisPagePriority priority)
{
    Q_UNUSED(priority);
    QMutexLocker locker(&d->mutex);
    return d->prepareSource(source, targetVersion, descriptor, use, &operation).takeExternalOperation(operation);
}


KisReplicaHandle KisTiles3PageReplicaProvider::adoptInitialTile(
    const KisPageVersion &version,
    const KisPageAllocationDescriptor &descriptor,
    KisTileData *tileData,
    QString *error)
try
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
    auto failCompletion = qScopeGuard([&] { d->completions->completePrepared(retirementCompletion, KisCompletionStatus::Failed); });
    auto prepared = d->prepareAdoption(version, descriptor, tileData);
    const KisReplicaHandle handle = prepared.handle;
    if (!d->adopt(prepared, tileData, retirementCompletion)) {
        KisPageStoreDetail::setError(error, QStringLiteral("tiles3 initial physical payload budget is exhausted"));
        return {};
    }
    failCompletion.dismiss();
    KisPageStoreDetail::setError(error, {});
    ++d->work.adoptedPages;
    d->work.adoptedBytes += byteSize;
    return handle;
}
catch (const std::bad_alloc &)
{
    KisPageStoreDetail::setError(error, QStringLiteral("tiles3 initial adoption storage preparation failed"));
    return {};
}

KisReplicaOperation KisTiles3PageReplicaProvider::transfer(
    const KisReplicaTransferRequest &request,
    KisPagePriority priority)
{
    Q_UNUSED(priority);
    QMutexLocker locker(&d->mutex);
    auto result = d->transfer(request);
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
           allocationIt->guardedTile();
}

bool KisTiles3PageReplicaProvider::validateCpuBinding(const KisReplicaHandle &replica,
    const KisPageAllocationDescriptor &descriptor,
    const std::shared_ptr<KisCpuResidentBinding> &binding) const
{
    return prepareCpuPublication(replica, descriptor, binding)
        && (!binding || binding->matchesHandle(replica));
}

bool KisTiles3PageReplicaProvider::prepareCpuPublication(const KisReplicaHandle &replica,
    const KisPageAllocationDescriptor &descriptor,
    const std::shared_ptr<KisCpuResidentBinding> &binding) const
{
    if (!binding) return validate(replica, descriptor); // A real unbound/generic input.
    // The original provider-created binding owns the allocation. Retirement
    // revokes it before unlinking the provider index; handoff changes its
    // exact generation under the same local gate. No provider-wide lookup or
    // gate adds a fact here. The admission address establishes provenance,
    // including two providers configured with identical numeric identities.
    const auto *original = dynamic_cast<const Tiles3ResidentBinding *>(binding.get());
    return original && original->belongsTo(&d->handoffAdmission)
        && supportsNativeTileLayout(descriptor) && replica.layout.matches(descriptor);
}

KisReplicaOperation KisTiles3PageReplicaProvider::retire(
    KisPageOperationId operation,
    const KisReplicaHandle &replica,
    const KisCompletionTicket &lastUse)
{
    KisTileData *retired = nullptr;
    KisCompletionTicket completion;
    std::shared_ptr<Tiles3ResidentBinding> released;
    {
        QMutexLocker locker(&d->mutex);
        auto retirement = d->beginRetirement(operation, replica, lastUse);
        if (!retirement.failure.isEmpty())
            return KisReplicaOperation::failed(operation, retirement.failure);
        completion = retirement.completion;
        retired = retirement.allocation->guardedTile();
        if (!retired) {
            return KisReplicaOperation::failed(
                operation, QStringLiteral("tiles3 retirement allocation is stale"));
        }
        if (d->residencyObserver->transitionActive(retirement.allocation->physicalBacking)) {
            return KisReplicaOperation::failed(
                operation, QStringLiteral("tiles3 residency transition is active"));
        }
        retired->ref();
        if (!retirement.allocation->retire(replica.allocationIdentity())) {
            retired->deref();
            return KisReplicaOperation::failed(operation, QStringLiteral("tiles3 native reader still pins allocation"));
        }
        d->consumeOperation(operation);
        d->releasePhysical(std::exchange(retirement.allocation->physicalBacking, nullptr));
        released = d->extractAllocation(retirement.allocation);
    }
    retired->deref();
    d->completions->completePrepared(completion, KisCompletionStatus::Succeeded);
    return {KisPageRequestStatus::Ready, operation, replica, completion, {}};
}

KisReplicaMemoryUsage KisTiles3PageReplicaProvider::memoryUsage() const
{
    QMutexLocker locker(&d->mutex);
    return {d->residencyObserver ? d->residencyObserver->residentBytes() : 0,
            d->committedBytes, d->config.budgetBytes};
}

KisReplicaBackingFootprint KisTiles3PageReplicaProvider::backingFootprint(
    const KisReplicaHandle &replica) const
{
    QMutexLocker locker(&d->mutex);
    const auto it = d->findExactAllocation(replica);
    if (it == d->allocations.end() || !it->physicalBacking) {
        return {};
    }
    KisTileData *tileData = it->guardedTile();
    KisPageAccessDomain domain = KisPageAccessDomain::Unknown;
    quint64 revision = 0;
    if (!tileData || !d->residencyObserver->observation(
            it->physicalBacking, &domain, &revision)) {
        return {};
    }
    return {it->physicalBacking->slot, domain, replica.layout.byteSize, revision};
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
    const std::shared_ptr<KisReplicaBackingDomainAdmission> &admission,
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

std::shared_ptr<KisCpuResidentBinding> KisTiles3PageReplicaProvider::cpuResidentBinding(
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
    const auto allocationIt = d->allocations.find(leaseIt->second.allocation.slot);
    if (allocationIt == d->allocations.end() ||
        !allocationIt->matchesAllocation(leaseIt->second.allocation)) {
        return nullptr;
    }
    return allocationIt->guardedTile();
}

KisTileData *KisTiles3PageReplicaProvider::tileDataForCpuWriteGuard(const KisCpuWriteGuard &guard) const
{
    if (!guard.isValid()) return nullptr;
    KisReplicaHandle replica;
    if (!guard.providerBacking(&replica)) return nullptr;
    QMutexLocker lock(&d->mutex);
    const auto it = d->findExactAllocation(replica);
    return it != d->allocations.end() ? it->guardedTile() : nullptr;
}

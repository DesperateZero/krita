/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_PAGE_BACKING_BUDGET_P_H
#define KIS_PAGE_BACKING_BUDGET_P_H
#include "KisPageStoreTypes.h"
#include "KisMutationStorage_p.h"
#include "KisPageReadiness_p.h"
#include "KisPageWaitCondition_p.h"
#include <map>
#include <array>
#include <unordered_map>
#include <new>
#include <type_traits>
#include <QtCore/qalgorithms.h>

enum class KisBackingBudgetClass : quint8 {
    Current,
    RetainedHistory,
    ActivePending,
    InFlight,
    RetirementDebt,
    OptionalCache,
    IdleCapacity,
    MetadataArena,
    Count
};

struct KisPageDomainBytes {
    quint64 cpuRam = 0;
    quint64 umaShared = 0;
    quint64 discreteVram = 0;
    quint64 ssd = 0;
};

struct KisPageBackingLimits {
    quint64 logicalCurrentBytes = std::numeric_limits<quint64>::max();
    quint64 retainedHistoryBytes = std::numeric_limits<quint64>::max();
    KisPageDomainBytes residentCurrentBytes{std::numeric_limits<quint64>::max(),
                                            std::numeric_limits<quint64>::max(),
                                            std::numeric_limits<quint64>::max(),
                                            std::numeric_limits<quint64>::max()};
    KisPageDomainBytes residentHistoryBytes = residentCurrentBytes;
    quint64 activePendingBytes = std::numeric_limits<quint64>::max();
    quint64 inFlightReplicaBytes = std::numeric_limits<quint64>::max();
    quint64 retirementDebtBytes = std::numeric_limits<quint64>::max();
    quint64 optionalCacheBytes = std::numeric_limits<quint64>::max();
    // Sub-limit of optionalCacheBytes; includes queued capacity until its
    // original charge actually changes or is released.
    quint64 idleCapacityBytes = 256 * 1024;
    quint64 metadataArenaBytes = std::numeric_limits<quint64>::max();
    quint64 durableStoreCapacity = std::numeric_limits<quint64>::max();
    quint32 maxTransientVersionsPerPage = std::numeric_limits<quint32>::max();
};

struct KisPageBackingUsageBucket {
    KisPageDomainBytes live;
    KisPageDomainBytes peak;
    KisPageDomainBytes reserved;
};

struct KisPageBackingUsage {
    std::array<KisPageBackingUsageBucket, static_cast<size_t>(KisBackingBudgetClass::Count)> buckets{};
    quint64 backpressureCount = 0;
    quint32 waitingRequests = 0;
    quint32 grantedRequests = 0;
    quint64 waiterGrants = 0;
    quint64 waiterCancellations = 0;
};

struct KisPageDomainByteDelta {
    qint64 cpuRam = 0;
    qint64 umaShared = 0;
    qint64 discreteVram = 0;
    qint64 ssd = 0;
};

template<typename DomainBytes>
auto kisPageDomainComponent(DomainBytes &bytes, KisPageAccessDomain domain)
    -> decltype(&bytes.cpuRam)
{
    switch (domain) {
    case KisPageAccessDomain::CpuRam: return &bytes.cpuRam;
    case KisPageAccessDomain::UmaShared: return &bytes.umaShared;
    case KisPageAccessDomain::DiscreteVram: return &bytes.discreteVram;
    case KisPageAccessDomain::Ssd: return &bytes.ssd;
    case KisPageAccessDomain::Unknown: return nullptr;
    }
    return nullptr;
}

class KisBackingBudgetDelta
{
    static constexpr size_t Count = static_cast<size_t>(KisBackingBudgetClass::Count);
    static_assert(Count <= 8);
    static_assert(std::is_trivially_copyable_v<KisPageDomainByteDelta>
                  && std::is_trivially_destructible_v<KisPageDomainByteDelta>);
public:
    KisBackingBudgetDelta() noexcept {} // Value-initialization must not zero unused capacity.
    KisBackingBudgetDelta(const KisBackingBudgetDelta &other) noexcept
        : m_constructed(other.m_constructed) { copyValues(other); }
    KisBackingBudgetDelta &operator=(const KisBackingBudgetDelta &other) noexcept
    {
        if (this == &other) return *this;
        copyValues(other);
        m_constructed = other.m_constructed;
        return *this;
    }
    // Untouched classes are semantic zero, not constructed scratch values.
    // Mutation constructs one actual class; full-value producers avoid even
    // that zero initialization through setBucket. No heap/index owns a delta.
    KisPageDomainByteDelta &bucket(size_t i) noexcept
    {
        Q_ASSERT(i < Count);
        if (!(m_constructed & (1u << i))) setBucket(i, {});
        return *value(i);
    }
    const KisPageDomainByteDelta &bucket(size_t i) const noexcept
    {
        Q_ASSERT(i < Count);
        return m_constructed & (1u << i) ? *value(i) : Empty;
    }
    KisPageDomainByteDelta &bucket(KisBackingBudgetClass c) noexcept { return bucket(size_t(c)); }
    const KisPageDomainByteDelta &bucket(KisBackingBudgetClass c) const noexcept { return bucket(size_t(c)); }
    void setBucket(size_t i, KisPageDomainByteDelta value) noexcept
    {
        Q_ASSERT(i < Count);
        ::new (static_cast<void *>(&m_values[i].value)) KisPageDomainByteDelta(value);
        m_constructed |= quint8(1u << i);
    }
    quint32 changedClasses() const noexcept
    {
        quint32 changed = 0;
        for (quint32 mask = m_constructed; mask; mask &= mask - 1) {
            const size_t i = size_t(qCountTrailingZeroBits(mask));
            const auto &v = *value(i);
            if (v.cpuRam || v.umaShared || v.discreteVram || v.ssd) changed |= quint32(1) << i;
        }
        return changed;
    }
private:
    union Cell {
        KisPageDomainByteDelta value;
        Cell() noexcept {} // No active value until this class is touched.
    };
    static_assert(std::is_trivially_copyable_v<Cell>);
    void copyValues(const KisBackingBudgetDelta &other) noexcept
    {
        const quint32 mask = other.m_constructed;
        if (!mask) return;
        if (!(mask & (mask - 1))) {
            const size_t i = size_t(qCountTrailingZeroBits(mask));
            m_values[i] = other.m_values[i];
        } else {
            // Keep multiple-class copying at a fixed vector-copy cost instead
            // of a per-class branch/address/mask loop. Trivial union copy
            // starts corresponding active member lifetimes; unselected cells
            // are never interpreted as domain values, including under C++17.
            m_values = other.m_values;
        }
    }
    KisPageDomainByteDelta *value(size_t i) noexcept
    { return std::launder(&m_values[i].value); }
    const KisPageDomainByteDelta *value(size_t i) const noexcept
    { return std::launder(&m_values[i].value); }
    inline static constexpr KisPageDomainByteDelta Empty{};
    std::array<Cell, Count> m_values;
    quint8 m_constructed = 0;
};

class KisBackingBudgetController;
struct KisBackingBudgetWaitContext;
// Private lifetime boundary for the existing waiter/controller. References
// retain only its inert context after controller teardown, never the owner.
KRITAIMAGE_EXPORT void intrusive_ptr_add_ref(KisBackingBudgetWaitContext *) noexcept;
KRITAIMAGE_EXPORT void intrusive_ptr_release(KisBackingBudgetWaitContext *) noexcept;

class KRITAIMAGE_EXPORT KisBackingBudgetReservation final
{
public:
    KisBackingBudgetReservation() = default;
    ~KisBackingBudgetReservation();
    KisBackingBudgetReservation(KisBackingBudgetReservation &&) noexcept;
    KisBackingBudgetReservation &operator=(KisBackingBudgetReservation &&) noexcept;
    KisBackingBudgetReservation(const KisBackingBudgetReservation &) = delete;
    KisBackingBudgetReservation &operator=(const KisBackingBudgetReservation &) = delete;

    bool isValid() const;
    void release() noexcept;

private:
    KisBackingBudgetReservation(KisBackingBudgetController *, quint64);
    void clear() noexcept;
    void commit(const KisBackingBudgetDelta &) noexcept;
    void commitRetaining(const KisBackingBudgetDelta &installed,
                         const KisBackingBudgetDelta &retained) noexcept;
    // OwnerLedger holds the original physical claim. Consume the admitted
    // signed change, optionally leaving its already funded retirement debt.
    void commitPrepared(const KisBackingBudgetDelta *retained = nullptr) noexcept;
    void settle(const KisBackingBudgetDelta *installed,
                const KisBackingBudgetDelta *retained, bool prepared = false) noexcept;
    bool retainOnly(const KisBackingBudgetDelta &retained) noexcept;

    KisBackingBudgetController *owner = nullptr;
    quint64 cookie = 0;
    KisBackingBudgetController *sharedOwner = nullptr;
    quint64 sharedCookie = 0;

    friend class KisBackingBudgetController;
    friend class KisPageWriteCoordinator;
    friend class KisPageOwnerLedger;
    friend class KisPageDefaultStorage;
};

// Cold, cancelable admission request. A ready waiter owns a real reservation,
// not permission to retry against unreserved headroom. Neither waiting nor a
// callback keeps its controller/store alive. The handle retains only the
// context and sequence; the context uniquely owns charged request storage.
// Teardown invalidates the owner and empties requests before detaching charge.
// take() is one-shot and the usual reservation owner-lifetime rule applies
// after take().
class KRITAIMAGE_EXPORT KisBackingBudgetWaiter final
{
public:
    KisBackingBudgetWaiter() = default;
    ~KisBackingBudgetWaiter();
    KisBackingBudgetWaiter(KisBackingBudgetWaiter &&) noexcept;
    KisBackingBudgetWaiter &operator=(KisBackingBudgetWaiter &&) noexcept;
    KisBackingBudgetWaiter(const KisBackingBudgetWaiter &) = delete;
    KisBackingBudgetWaiter &operator=(const KisBackingBudgetWaiter &) = delete;
    void reset();
    bool isValid() const;
    KisBackingBudgetReservation take();
private:
    boost::intrusive_ptr<KisBackingBudgetWaitContext> context;
    quint64 sequence = 0;
    friend class KisBackingBudgetController;
};

// The parent's stable registration is retained by the child, then by its
// original inert storage owner until the last actual free. It is never a
// second account; every access still holds the parent's accounting gate.
struct KisSharedNonPayloadUsage {
    // Ordinary accepted fees plus the actual storage capacity charge. This
    // child record retains ownership, not the parent's live/reserved split.
    quint64 chargedBytes = 0;
    quint64 reservedBytes = 0;
};

// An empty original operation, not a second account or permission record.
// Its allocation remains live/charged. Only the accounting root links it;
// disposal runs after dropping that gate and must acquire no Store gate.
struct KisReusableMutationStorage {
    KisReusableMutationStorage *previous = nullptr;
    KisReusableMutationStorage *next = nullptr;
    KisBackingBudgetController *origin = nullptr;
    size_t bytes = 0;
    void (*dispose)(KisReusableMutationStorage *) noexcept = nullptr;
};

class KRITAIMAGE_EXPORT KisBackingBudgetController final
{
public:
    explicit KisBackingBudgetController(const KisPageBackingLimits &limits = {}, bool processStorage = false);
    ~KisBackingBudgetController();
    KisPageReadinessStatus waitForChange(const KisBackingBudgetDelta &change,
        KisPageReadinessCallback notify, KisBackingBudgetWaiter *waiter, QString *error = nullptr);

    KisBackingBudgetReservation reserve(const KisBackingBudgetDelta &, QString *error);
    // Reserves positive per-bucket destinations while durable capacity uses
    // only the net SSD growth of the signed physical move/reclassification.
    // Shared non-payload classes are mapped before taking positive demand;
    // the caller must hold the original class/physical claim through commit.
    KisBackingBudgetReservation reserveChange(const KisBackingBudgetDelta &,
                                               QString *error);
    quint64 idleCapacityAllowance() const;
    // Identity-only handoff still observes hard limits/process peaks, but
    // has no positive or retained fee and needs no reservation lifetime.
    bool admitUnchangedBacking(QString *error);
    void commitReservation(KisBackingBudgetReservation &&reservation,
                           const KisBackingBudgetDelta &installed) noexcept;
    KisPageBackingUsage usage() const;
    quint32 maxTransientVersionsPerPage() const;
    bool configureLimits(const KisPageBackingLimits &, QString *error);
    // Product stores retain their own diagnostics and class limits while this
    // parent bounds their combined metadata/default-cache CPU allocation.
    // Configure before the controller is used. The parent must be root-level.
    bool configureSharedNonPayloadBudget(
        const std::shared_ptr<KisBackingBudgetController> &parent,
        QString *error = nullptr);
    bool ensureProcessStorageBudget(QString *error = nullptr);
    void releaseLive(KisBackingBudgetClass, KisPageAccessDomain, quint64 bytes) noexcept;
    static constexpr size_t ReusableMutationBytes = 64 * 1024;
    bool retainMutationStorage(KisReusableMutationStorage *) noexcept;
    KisReusableMutationStorage *takeMutationStorage() noexcept;
    void stopMutationStorageReuse() noexcept;

private:
    // The ledger's original physical handoff claim owns this exact one-shot
    // CPU transfer. No independently addressable budget slot is needed. The
    // caller must settle once, with its still-claimed source class and bytes.
    bool reserveCpuOptionalHandoff(quint64 bytes, QString *error);
    void settleCpuOptionalHandoff(KisBackingBudgetClass source, quint64 bytes, bool commit) noexcept;
    struct ReservationSlot {
        static_assert(static_cast<size_t>(KisBackingBudgetClass::Count) == 8);
        // Accepted signed change; only its positive components are reserved.
        // Partial consumers still supply installed/retained facts to settle.
        // Fixed capacity supports the complete public delta without another
        // allocation. Only mask-selected components are initialized/consumed;
        // recycling never clears or copies an unrelated class/domain.
        std::array<qint64, 32> values;
        KisPageDomainByteDelta sharedChange;
        std::array<quint64, static_cast<size_t>(KisBackingBudgetClass::Count)>
            aggregateBytes;
        quint64 durableBytes = 0;
        KisSharedNonPayloadUsage *sharedChild = nullptr;
        quint32 generation = 0;
        quint32 nextFree = std::numeric_limits<quint32>::max();
        quint32 changedComponents = 0;
        bool active = false;
        quint8 changedClasses = 0;
        ReservationSlot() noexcept {} // Unselected capacity is not initialized.
        KisPageDomainByteDelta bucket(size_t i) const noexcept
        {
            const auto first = i * 4;
            const auto value = [&](size_t n) {
                return changedComponents & (quint32(1) << (first + n)) ? values[first + n] : qint64(0);
            };
            return {value(0), value(1), value(2), value(3)};
        }
        void setBucket(size_t i, const KisPageDomainByteDelta &delta) noexcept
        {
            const qint64 input[]{delta.cpuRam, delta.umaShared, delta.discreteVram, delta.ssd};
            const auto first = i * 4;
            changedComponents &= ~(quint32(15) << first);
            for (size_t n = 0; n < 4; ++n) if (input[n]) {
                values[first + n] = input[n];
                changedComponents |= quint32(1) << (first + n);
            }
        }
        quint64 aggregate(size_t i) const noexcept
        { return changedClasses & (quint32(1) << i) ? aggregateBytes[i] : 0; }
    };
    static_assert(sizeof(ReservationSlot) <= 384);
    using SharedChildUsage = KisSharedNonPayloadUsage;
    using SharedChildAllocator = KisMutationStorageAllocator<
        std::pair<const quint64, SharedChildUsage>>;
    using SharedChildMap = std::map<quint64, SharedChildUsage,
                                    std::less<quint64>, SharedChildAllocator>;

    ReservationSlot *activeReservation(quint64 cookie)
    {
        const quint32 index = quint32(cookie);
        auto *slot = index < m_slotCount ? &m_slots[index] : nullptr;
        if (!slot || !slot->active || slot->generation != quint32(cookie >> 32))
            return nullptr;
        return slot;
    }
    const ReservationSlot *activeReservation(quint64 cookie) const
    {
        return const_cast<KisBackingBudgetController *>(this)->activeReservation(cookie);
    }

    // The parent consumes one shared domain bucket; the local controller
    // consumes full class deltas. Both use the same settlement policy.
    template<class Delta>
    bool settle(quint64 cookie, const Delta *installed,
                const Delta *retained, bool prepared = false,
                KisPageDomainByteDelta *sharedInstalled = nullptr) noexcept;
    // OwnerLedger-only: headroom for reclassifying an already charged physical
    // backing. Moving it creates no payload or additional durable allocation.
    void moveRetirementHeadroom(quint64 cookie, KisPageAccessDomain source,
                                KisPageAccessDomain target, quint64 bytes) noexcept;
    void release(quint64 cookie) noexcept;
    bool reservationCovers(quint64 cookie, KisBackingBudgetClass,
                           KisPageAccessDomain, quint64 bytes) const;
    bool reservationCovers(quint64 cookie, const KisBackingBudgetDelta &) const;
    KisBackingBudgetReservation reserveImpl(const KisBackingBudgetDelta &,
                                             const std::array<quint64,
                                                 static_cast<size_t>(KisBackingBudgetClass::Count)> &aggregateBytes,
                                             quint64 durableBytes,
                                             QString *error,
                                             const KisBackingBudgetDelta *signedChange = nullptr);
    KisBackingBudgetReservation reserveSharedNonPayload(
        SharedChildUsage *child, const KisPageDomainByteDelta &, QString *error);
    quint64 registerSharedNonPayloadChild(SharedChildUsage **usage, QString *error);
    void unregisterSharedNonPayloadChild(quint64 child) noexcept;
    void releaseSharedNonPayloadLive(SharedChildUsage *child,
                                     KisPageAccessDomain domain,
                                     quint64 bytes) noexcept;
    void releaseSharedNonPayloadLive(SharedChildUsage *child,
                                     const KisPageDomainByteDelta &change) noexcept;
    void releaseSharedNonPayloadLiveLocked(SharedChildUsage *child,
                                           KisPageAccessDomain domain,
                                           quint64 bytes) noexcept;
    void releaseStorage(quint64 bytes, bool installed = true) noexcept;
    void releaseSharedStorage(SharedChildUsage *child, quint64 bytes, bool installed) noexcept;
    void releaseStorageLocked(quint64 bytes, bool installed) noexcept;
    quint64 storageBytesLocked() const noexcept;

    bool fitsLocked(const KisBackingBudgetDelta &,
        const std::array<quint64, static_cast<size_t>(KisBackingBudgetClass::Count)> &,
        quint64 durableBytes, bool emptyUsage = false,
        quint64 irreducibleMetadataBytes = 0) const;
    bool fitsMetadataStorageLocked(quint64 bytes, quint64 *processBytes = nullptr) const;
    bool fitsCpuOptionalHandoffLocked(quint64 bytes) const;
    bool reclaimMutationStorage(bool ownOnly, bool waitingOnly = false) noexcept;
    void unlinkMutationStorageLocked(KisReusableMutationStorage *) noexcept;
    void *allocateStorage(const KisPageStorageLayout &layout);
    quint64 nonMetadataLiveSsdLocked() const;
    bool priorWaiterConflictsLocked(const KisBackingBudgetDelta &,
        const std::array<quint64, static_cast<size_t>(KisBackingBudgetClass::Count)> &,
        quint64 durableBytes, quint64 beforeSequence = std::numeric_limits<quint64>::max()) const;
    KisBackingBudgetReservation reserveLocked(const KisBackingBudgetDelta &,
        const std::array<quint64, static_cast<size_t>(KisBackingBudgetClass::Count)> &,
        quint64, SharedChildUsage *sharedChild = nullptr, const KisBackingBudgetDelta *signedChange = nullptr,
        const KisPageDomainByteDelta *sharedChange = nullptr);
    bool prepareReservationSlot(QString *error);
    void activateReservationLocked(quint64 cookie, const KisBackingBudgetDelta &,
        const std::array<quint64, static_cast<size_t>(KisBackingBudgetClass::Count)> &,
        quint64, SharedChildUsage *sharedChild = nullptr, const KisBackingBudgetDelta *signedChange = nullptr,
        const KisPageDomainByteDelta *sharedChange = nullptr);
    using WaitCallbacks = std::array<KisPageReadinessCallback, 32>;
    WaitCallbacks dispatchWaiters();
    void notifyWaitersLocked() noexcept;
    KisBackingBudgetReservation finishWaiter(quint64 sequence, bool take);
    static KisPageDomainByteDelta sharedNonPayloadDelta(const KisBackingBudgetDelta &,
                                                       bool *valid = nullptr) noexcept;
    boost::intrusive_ptr<KisBackingBudgetWaitContext> m_waitContext;
    boost::intrusive_ptr<KisMutationStorageOwner> m_storageOwner;
    QMutex m_storageOwnerMutex;
    std::shared_ptr<KisBackingBudgetController> m_sharedNonPayloadBudget;
    quint64 m_sharedNonPayloadChild = 0;
    SharedChildUsage *m_sharedNonPayloadUsage = nullptr;
    // Guarded by the shared root's m_mutex (this->m_mutex without a parent).
    KisReusableMutationStorage *m_reusableMutation = nullptr;
    KisReusableMutationStorage *m_firstReusableMutation = nullptr;
    KisReusableMutationStorage *m_lastReusableMutation = nullptr;
    bool m_acceptReusableMutation = true;

    mutable QMutex m_mutex;
    KisPageBackingLimits m_limits;
    KisPageBackingUsage m_usage;
    // The ordinary slot array is actual storage too. Its original allocator
    // needs no reservation slot to fund itself, including on first growth.
    struct SlotStorageDeleter {
        KisBackingBudgetController *budget = nullptr;
        void operator()(ReservationSlot *storage) const noexcept
        { kisFreeMutationStorage(budget, storage, 0, alignof(ReservationSlot)); }
    };
    std::unique_ptr<ReservationSlot[], SlotStorageDeleter> m_slots{nullptr, SlotStorageDeleter{this}};
    quint32 m_slotCount = 0;
    quint32 m_slotCapacity = 0;
    QMutex m_slotGrowthMutex;
    quint32 m_firstFreeSlot = std::numeric_limits<quint32>::max();
    // Ordinary transactions reserve their net aggregate change; that can be
    // less than their positive domain reservations during a cross-domain move.
    std::array<quint64, static_cast<size_t>(KisBackingBudgetClass::Count)>
        m_reservedAggregateBytes{};
    // Actual storage occupies its whole admitted capacity until physical free.
    // Success publishes only the live split, without re-entering either gate.
    quint64 m_storageBytes = 0;
    std::atomic<quint64> m_storageLiveBytes{0};
    quint64 m_reservedDurableBytes = 0;
    SharedChildMap m_sharedChildren;
    quint64 m_nextSharedChild = 1;
    const bool m_processStorage;
    // Derived under m_mutex, never a second balance. A bit proves the
    // unchanged non-metadata class fits its current limits. Changes that can
    // increase its load invalidate it; metadata/process and cross-class constraints
    // are checked from live state on every admission.
    mutable quint64 m_nonMetadataLiveSsd = 0;
    mutable quint32 m_fittingBudgetClasses = 0;
    mutable bool m_nonMetadataLiveSsdValid = false;

    friend class KisBackingBudgetReservation;
    friend class KisBackingHandoffReservation;
    friend class KisBackingBudgetWaiter;
    friend class KisPageOwnerLedger;
    friend class KisMutationStorageOwner;
    friend KisMutationStorageOwner *kisMutationStorageOwner(KisBackingBudgetController *);
    friend void *kisAllocateMutationStorage(KisBackingBudgetController *, size_t, size_t);
    friend void kisFreeMutationStorage(KisBackingBudgetController *, void *, size_t, size_t) noexcept;
    friend void *kisAllocatePageProcessStorage(size_t, size_t);
    friend void kisFreePageProcessStorage(void *, size_t, size_t) noexcept;
    friend void kisReservePageProcessStorage(size_t);
    friend void kisCommitPageProcessStorage(size_t, size_t) noexcept;
    friend void kisReleasePageProcessStorage(size_t) noexcept;
    friend void kisReleaseLivePageProcessStorage(size_t) noexcept;
    friend std::shared_ptr<KisBackingBudgetController> kisAcquirePageStoreProcessBudget(quint64, QString *, bool);
};

// Same product parent, including storage prepared before attachment and inert
// weak tails. Fixed runtime allocations do not retain a document controller.
KRITAIMAGE_EXPORT std::shared_ptr<KisBackingBudgetController>
kisAcquirePageStoreProcessBudget(quint64 metadataBytes, QString *error = nullptr, bool publishPolicy = true);
KRITAIMAGE_EXPORT std::shared_ptr<KisBackingBudgetController>
kisAcquirePageStoreBootstrapBudget(QString *error = nullptr);
KRITAIMAGE_EXPORT quint64 kisPageProcessStorageBytes() noexcept;
KRITAIMAGE_EXPORT quint64 kisPageProcessStorageLimit() noexcept;


#endif

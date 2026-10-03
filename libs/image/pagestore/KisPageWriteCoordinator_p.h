/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_WRITE_COORDINATOR_P_H
#define KIS_PAGE_WRITE_COORDINATOR_P_H

#include "KisPageReadiness_p.h"
#include "KisPageRetirementStorage_p.h"

#include "KisPageStoreTypes.h"
#include "KisMutationStorage_p.h"

#include <QHash>
#include <QMutex>
#include <QSharedPointer>
#include <QThread>
#include <QVector>
#include "KisPageWaitCondition_p.h"

#include <boost/intrusive_ptr.hpp>

#include <array>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <unordered_map>
#include <vector>

struct KisPageBackingPreparation;
class KisImageEpochReferenceModel;
class KisPageMetadataCoordinator;
class KisPageOwnerLedger;
class KisPageReplicaProvider;
class KisPageReplicaSource;
class KisPageReplicaTransferBridge;
struct KisCpuPagePayload;
struct KisReplicaOperation;
class KisPagePublicationCoordinator;
class KisPageWriteCoordinator;
class KisPageStoreDiagnosticTimer;
class KisPageStore;
class KisCpuWriteBindingReservation;
class KisPageStoreWriteReservation;
struct KisPageTransition;
struct KisPageMetadataTransitionResult;

enum class KisPageWriteInputKind : quint8 {
    MutableGuard,
    Semantic
};

enum class KisPageWritePlanKind : quint8 {
    SemanticOnly,
    ReusePending,
    FreshPayload,
    FreshDiscard,
    FreshCow,
    RecoverableHandoff
};

enum class KisPageWriteIntentFlag : quint8 {
    InputBytesReady = 1u << 0,
    SemanticRemoval = 1u << 1,
    AsyncLease = 1u << 2,
    SourceInitialization = 1u << 3
};

enum class KisMutationPageEntryState : quint8 {
    IntentOnly,
    Prepared,
    Exposed
};

struct KisPageWriteIntent {
    KisPageKey key;
    KisPageWriteMode mode = KisPageWriteMode::PreserveContents;
    KisPageWriteInputKind inputKind = KisPageWriteInputKind::MutableGuard;
    quint8 flags = 0;
};

enum class KisBackingBudgetClass : quint8 {
    Current,
    RetainedHistory,
    ActivePending,
    InFlight,
    RetirementDebt,
    OptionalCache,
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

struct KisBackingBudgetDelta {
    std::array<KisPageDomainByteDelta, static_cast<size_t>(KisBackingBudgetClass::Count)> buckets{};
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
    KisBackingBudgetReservation reserveChange(const KisBackingBudgetDelta &,
                                               QString *error);
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

private:
    struct ReservationSlot {
        KisBackingBudgetDelta delta;
        std::array<quint64, static_cast<size_t>(KisBackingBudgetClass::Count)>
            aggregateBytes{};
        quint64 durableBytes = 0;
        quint64 sharedChild = 0;
        quint32 generation = 0;
        quint32 nextFree = std::numeric_limits<quint32>::max();
        bool active = false;
    };

    struct SharedChildUsage {
        quint64 liveBytes = 0;
        quint64 reservedBytes = 0;
    };
    using SharedChildAllocator = KisMutationStorageAllocator<
        std::pair<const quint64, SharedChildUsage>>;
    using SharedChildMap = std::map<quint64, SharedChildUsage,
                                    std::less<quint64>, SharedChildAllocator>;

    ReservationSlot *activeReservation(quint64 cookie)
    {
        const quint32 index = quint32(cookie);
        auto *slot = index == std::numeric_limits<quint32>::max()
            ? &m_storageReservation : index < m_slotCount ? &m_slots[index] : nullptr;
        if (!slot || !slot->active || slot->generation != quint32(cookie >> 32))
            return nullptr;
        return slot;
    }
    const ReservationSlot *activeReservation(quint64 cookie) const
    {
        return const_cast<KisBackingBudgetController *>(this)->activeReservation(cookie);
    }

    void commit(quint64 cookie, const KisBackingBudgetDelta &) noexcept;
    bool commitRetaining(quint64 cookie,
                         const KisBackingBudgetDelta &installed,
                         const KisBackingBudgetDelta &retained) noexcept;
    bool retainOnly(quint64 cookie,
                    const KisBackingBudgetDelta &retained) noexcept;
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
                                             QString *error);
    KisBackingBudgetReservation reserveSharedNonPayload(
        quint64 child, const KisBackingBudgetDelta &, QString *error);
    quint64 registerSharedNonPayloadChild(QString *error);
    void unregisterSharedNonPayloadChild(quint64 child) noexcept;
    void releaseSharedNonPayloadLive(quint64 child,
                                     KisPageAccessDomain domain,
                                     quint64 bytes) noexcept;

    bool fitsLocked(const KisBackingBudgetDelta &,
        const std::array<quint64, static_cast<size_t>(KisBackingBudgetClass::Count)> &,
        quint64 durableBytes, bool emptyUsage = false,
        quint64 irreducibleMetadataBytes = 0) const;
    bool priorWaiterConflictsLocked(const KisBackingBudgetDelta &,
        const std::array<quint64, static_cast<size_t>(KisBackingBudgetClass::Count)> &,
        quint64 durableBytes, quint64 beforeSequence = std::numeric_limits<quint64>::max()) const;
    KisBackingBudgetReservation reserveLocked(const KisBackingBudgetDelta &,
        const std::array<quint64, static_cast<size_t>(KisBackingBudgetClass::Count)> &,
        quint64, quint64 sharedChild = 0);
    bool prepareReservationSlot(QString *error);
    void activateReservationLocked(quint64 cookie, const KisBackingBudgetDelta &,
        const std::array<quint64, static_cast<size_t>(KisBackingBudgetClass::Count)> &,
        quint64, quint64 sharedChild = 0);
    using WaitCallbacks = std::array<KisPageReadinessCallback, 32>;
    WaitCallbacks dispatchWaiters();
    void notifyWaitersLocked() noexcept;
    KisBackingBudgetReservation finishWaiter(quint64 sequence, bool take);
    static KisBackingBudgetDelta sharedNonPayloadDelta(const KisBackingBudgetDelta &) noexcept;
    boost::intrusive_ptr<KisBackingBudgetWaitContext> m_waitContext;
    boost::intrusive_ptr<KisMutationStorageOwner> m_storageOwner;
    QMutex m_storageOwnerMutex;
    std::shared_ptr<KisBackingBudgetController> m_sharedNonPayloadBudget;
    quint64 m_sharedNonPayloadChild = 0;

    mutable QMutex m_mutex;
    KisPageBackingLimits m_limits;
    KisPageBackingUsage m_usage;
    // A single embedded cold reservation bootstraps charging the slot array
    // through the same accounting transitions, without reserving another slot.
    // It never escapes into a caller or waiter and growth is serialized.
    ReservationSlot m_storageReservation;
    struct SlotStorageDeleter {
        void operator()(ReservationSlot *storage) const noexcept
        { kisFreePageStorage(storage, alignof(ReservationSlot)); }
    };
    std::unique_ptr<ReservationSlot[], SlotStorageDeleter> m_slots;
    quint32 m_slotCount = 0;
    quint32 m_slotCapacity = 0;
    QMutex m_slotGrowthMutex;
    quint32 m_firstFreeSlot = std::numeric_limits<quint32>::max();
    // Aggregate/durable reservations are authoritative derivatives of the
    // active slots. Keep them current at every slot transition so admission
    // cost depends on this request, not on a historical concurrency peak.
    std::array<quint64, static_cast<size_t>(KisBackingBudgetClass::Count)>
        m_reservedAggregateBytes{};
    quint64 m_reservedDurableBytes = 0;
    SharedChildMap m_sharedChildren;
    quint64 m_nextSharedChild = 1;
    const bool m_processStorage;

    friend class KisBackingBudgetReservation;
    friend class KisBackingBudgetWaiter;
    friend class KisPageOwnerLedger;
    friend class KisMutationStorageOwner;
    friend KisMutationStorageOwner *kisMutationStorageOwner(KisBackingBudgetController *);
    friend void *kisAllocatePageProcessStorage(size_t, size_t);
    friend void kisFreePageProcessStorage(void *, size_t, size_t) noexcept;
    friend void kisReservePageProcessStorage(size_t);
    friend void kisReleasePageProcessStorage(size_t) noexcept;
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

class KRITAIMAGE_EXPORT KisMutationPageEntry final
{
public:
    explicit KisMutationPageEntry(const KisPageWriteIntent &value) noexcept : intent(value) {}
    KisMutationPageEntry(KisMutationPageEntry &&) noexcept = default;
    KisMutationPageEntry &operator=(KisMutationPageEntry &&) noexcept = default;
    KisMutationPageEntry(const KisMutationPageEntry &) = delete;
    KisMutationPageEntry &operator=(const KisMutationPageEntry &) = delete;

    const KisPageKey &key() const { return intent.key; }
    KisPageVersion preparedTargetVersion() const
    {
        return targetGeneration.isValid() ? KisPageVersion{key(), targetGeneration} : KisPageVersion{};
    }
    bool isExposed() const { return state == KisMutationPageEntryState::Exposed; }
    bool isCpuWrite() const { return intent.inputKind != KisPageWriteInputKind::Semantic; }
    const std::shared_ptr<const KisPageReplicaSource> &initializationSource() const { return initialization; }
    void setInitializationSource(std::shared_ptr<const KisPageReplicaSource> source)
    { initialization = std::move(source); }
    bool isRemoval() const
    { return intent.flags & quint8(KisPageWriteIntentFlag::SemanticRemoval); }
    void setRemoval(bool removed)
    {
        const quint8 flag = quint8(KisPageWriteIntentFlag::SemanticRemoval);
        removed ? intent.flags |= flag : intent.flags &= ~flag;
    }
    quint32 coldPage() const { return coldPageIndex; }
    void setColdPage(quint32 index) { coldPageIndex = index; }

private:
    KisPageWriteIntent intent;
    KisPageVersion baseVersion;
    KisPageGeneration targetGeneration;
    KisPageOperationId operation;
    KisPageWriterToken writer;
    std::shared_ptr<const KisPageReplicaSource> initialization;
    quint32 coldPageIndex = std::numeric_limits<quint32>::max();
    KisMutationPageEntryState state = KisMutationPageEntryState::IntentOnly;

    friend class KisPageWriteCoordinator;
};

class KRITAIMAGE_EXPORT KisMutationWriteSet final
{
public:
    using EntryIndex = quint32;
    using EntryHandle = KisMutationSlotHandle;
    using ReleasedBlocks = KisMutationStorage<KisMutationPageEntry, 8>::ReleasedBlocks;
    static constexpr EntryIndex InvalidEntry = std::numeric_limits<EntryIndex>::max();

    explicit KisMutationWriteSet(KisBackingBudgetController *budget = nullptr);
    KisMutationWriteSet(KisMutationWriteSet &&) noexcept;
    KisMutationWriteSet &operator=(KisMutationWriteSet &&) noexcept;
    KisMutationWriteSet(const KisMutationWriteSet &) = delete;
    KisMutationWriteSet &operator=(const KisMutationWriteSet &) = delete;

    KisMutationPageEntry *find(const KisPageKey &);
    const KisMutationPageEntry *find(const KisPageKey &) const;
    EntryIndex findIndex(const KisPageKey &) const;
    KisMutationPageEntry &getOrCreate(const KisPageWriteIntent &);
    KisMutationPageEntry *at(EntryIndex);
    const KisMutationPageEntry *at(EntryIndex) const;
    KisMutationPageEntry *at(EntryHandle);
    const KisMutationPageEntry *at(EntryHandle) const;
    EntryHandle handleAt(EntryIndex) const;
    EntryHandle firstEntry() const;
    EntryHandle nextEntry(EntryHandle) const;
    // Caller first terminates cold resources and releases this entry's claim.
    bool erase(EntryHandle);
    ReleasedBlocks takeReleasedBlocks() noexcept { return overflow.takeReleasedBlocks(); }
    void reserveKnownTargetCount(qsizetype);
    qsizetype size() const;

private:
    struct KeyHash {
        size_t seed = QHashSeed::globalSeed();
        size_t operator()(const KisPageKey &key) const noexcept { return qHash(key, seed); }
    };
    using Index = std::unordered_map<KisPageKey, EntryHandle, KeyHash, std::equal_to<KisPageKey>,
        KisMutationStorageAllocator<std::pair<const KisPageKey, EntryHandle>>>;
    Index prepareIndex(qsizetype count) const;

    std::optional<KisMutationPageEntry> inlineEntry;
    quint64 inlineIncarnation = 0;
    quint64 nextInlineIncarnation = 1;
    KisMutationStorage<KisMutationPageEntry, 8> overflow;
    std::optional<Index> index;
    KisBackingBudgetController *budget = nullptr;
};

class KRITAIMAGE_EXPORT KisPageWriteAdmission final
{
public:
    enum class ClaimOrigin : quint8 {
        NativeSession,
        ManagedRange,
        LegacyAdapter,
        GenericWrite
    };
    enum class Conflict : quint8 {
        None,
        ManagedOtherThread,
        ManagedSameThread,
        LegacyBorrower,
        LegacySameThread,
        WriterOtherThread,
        WriterSameThread
    };
    enum class Result : quint8 { Acquired, Contended, Failed };

    class ClaimSet final
    {
    public:
        ClaimSet() = default;
        ~ClaimSet();
        ClaimSet(ClaimSet &&) noexcept;
        ClaimSet &operator=(ClaimSet &&) noexcept;
        ClaimSet(const ClaimSet &) = delete;
        ClaimSet &operator=(const ClaimSet &) = delete;

        bool isValid() const;
        void release() noexcept;
        void releaseLocked() noexcept;

    private:
        void releaseImpl(bool lockOwner) noexcept;
        KisPageWriteAdmission *owner = nullptr;
        const KisMutationWriteSet *writeSet = nullptr;
        quint64 token = 0;

        friend class KisPageWriteAdmission;
    };

    KisPageWriteAdmission(QMutex &ownerMutex, KisPageWaitCondition &ownerCondition,
                          KisBackingBudgetController *budget = nullptr,
                          const bool *operational = nullptr);
    qsizetype activeNativeClaimCountLocked() const;
    qsizetype activeGenericClaimCountLocked() const;
    Conflict conflictLocked(const KisPageKey &, Qt::HANDLE requester) const;
    bool pageClaimedLocked(const KisPageKey &key) const { return m_claims.contains(key); }
    // Growth temporarily releases ownerLock. The caller retains the original
    // owner/transaction and serializes its write set independently (session
    // mutex or a private range). Never pass a mutable unprotected write set.
    bool claimDirectLocked(const KisPageKey &, quint64 ownerToken, QMutexLocker<QMutex> &, QString *error = nullptr);
    void releaseDirectLocked(const KisPageKey &, quint64 ownerToken) noexcept;
    ClaimSet beginClaimSet(const KisMutationWriteSet &);
    bool claimOne(ClaimSet &, KisMutationWriteSet::EntryHandle, QMutexLocker<QMutex> &, QString *error,
                  ClaimOrigin origin = ClaimOrigin::NativeSession);
    bool releaseOneLocked(ClaimSet &, KisMutationWriteSet::EntryHandle) noexcept;
    // Contention observed across growth remains distinct from real storage
    // refusal, even if that writer releases while unused growth is freed.
    Result claimAll(ClaimSet &, QMutexLocker<QMutex> &, QString *error,
                  ClaimOrigin origin = ClaimOrigin::NativeSession);
    // Unique keys, stable under the caller's session gate. Only this range is
    // visited, not every page previously touched by a long-lived session.
    bool claimRange(ClaimSet &, KisSurfaceId, const KisPageSnapshotArray<KisLogicalPageId> &,
                    QMutexLocker<QMutex> &, QString *error);
    bool ownsClaimSetLocked(const ClaimSet &) const;
    void rebindClaimSetLocked(ClaimSet &, const KisMutationWriteSet &) noexcept;

private:
    struct ActiveClaim {
        quint64 token = 0;
        ClaimOrigin origin = ClaimOrigin::NativeSession;
        Qt::HANDLE thread = nullptr;
    };

    void release(ClaimSet &, bool lockOwner) noexcept;

    QMutex *m_ownerMutex = nullptr;
    KisPageWaitCondition *m_ownerCondition = nullptr;
    KisMutationAdmissionTable<KisPageKey, ActiveClaim> m_claims;
    const bool *m_operational = nullptr;
    quint64 m_nextClaimToken = 1;
    friend class KisPageStoreReferenceTest;
};

// An adapter owns its claimed write set until it transfers to a mutation
// session or a semantic operation terminates. ClaimSet
// references this stable heap object or the session set, never a copied key array.
class KisPageStoreWriteReservation final
{
public:
    explicit KisPageStoreWriteReservation(KisBackingBudgetController *budget) : writes(budget) {}
    static void *operator new(size_t, KisBackingBudgetController *, void *owner,
                              void (*retain)(void *), void (*release)(void *));
    static void operator delete(void *) noexcept;
    static void operator delete(void *data, KisBackingBudgetController *, void *,
                                void (*)(void *), void (*)(void *)) noexcept { operator delete(data); }
private:
    struct alignas(std::max_align_t) Allocation {
        KisBackingBudgetController *budget;
        void *owner;
        void (*release)(void *);
    };
    KisMutationWriteSet writes;
    KisPageWriteAdmission::ClaimSet admission;

    friend class KisPageStore;
    friend class KisPageMutationSession;
};

class KRITAIMAGE_EXPORT KisPageWriteCoordinator final
{
public:
    KisPageWriteCoordinator(KisPageMetadataCoordinator &,
                            KisImageEpochReferenceModel &,
                            KisBackingBudgetController &,
                            KisPageOwnerLedger &);

    KisPageBackingPreparation reserveBacking(const KisPageAllocationDescriptor &descriptor,
                                                KisPageAccessDomain domain,
                                                KisBackingBudgetClass budgetClass,
                                                QString *error,
                                                const KisPageVersion &target = {},
                                                KisPageRetirementRecordPointer *preparedRetirement = nullptr);

    // Caller holds owner admission. The output is transient transition input,
    // not a second page/overlay authority; no provider allocation occurs here.
    bool prepareWriteBaseLocked(const KisPageTransaction &, const KisPageWriteIntent &,
                                KisPagePublicationCoordinator &, KisPageTransition *,
                                KisPageAllocationDescriptor *, QString *error,
                                const KisPageTransition *pending = nullptr,
                                KisReplicaHandle *recoverableBefore = nullptr);
    KisPageMetadataTransitionResult preparePrivateWrite(KisPageTransition &, bool initialized);
    // Both access adapters use this cold first-write decision. On handoff it
    // installs the logical writer, descriptor and accounting before returning
    // the retagged physical reservation. Otherwise write is unchanged and the
    // caller continues the selected Fresh plan. Admission spans unlocked work.
    KisPageWritePlanKind prepareWritePlanLocked(
        const KisPageTransaction &, const KisPageWriteIntent &,
        const std::shared_ptr<KisPageReplicaProvider> &, KisPageAccessRequirement,
        const KisPageAllocationDescriptor &,
        KisPagePublicationCoordinator &, KisPageTransition &, const KisReplicaHandle &recoverableBefore,
        QMutexLocker<QMutex> &,
        KisCpuWriteBindingReservation &, KisPageStoreDiagnosticTimer &);
    KisPageMetadataTransitionResult publishPrivateWrite(KisPageTransition);
    KisReplicaOperation prepareFreshReplica(const KisPageWriteIntent &,
        KisPageReplicaProvider &, const KisPageTransition &,
        const KisPageAllocationDescriptor &, KisPageAccessRequirement,
        KisPagePriority, const KisCpuPagePayload *,
        const std::shared_ptr<const KisPageReplicaSource> &initialization,
        bool *synchronousCopy = nullptr) const;
    bool registerTransferBridge(const std::shared_ptr<KisPageReplicaTransferBridge> &bridge);
    KisCompletionTicket initializeFreshReplica(
        const KisPageWriteIntent &intent, bool initializedDuringAllocation,
        const KisReplicaHandle &source, const KisReplicaHandle &target,
        const KisPageAllocationDescriptor &descriptor,
        const std::shared_ptr<KisPageReplicaProvider> &targetProvider,
        KisPagePriority priority, const KisCompletionTicket &allocationReadiness,
        QString *error) const;
    // The caller retains admission until detach succeeds. Metadata, not an
    // adapter-local flag, determines which private state must be cancelled.
    KisPageMetadataTransitionResult cancelPrivateWrite(KisPageTransition write);

    KisPageWritePlanKind select(const KisPageWriteIntent &,
                                const KisMutationPageEntry *pending = nullptr,
                                bool recoverablePrepared = false) const;
    void recordPrepared(KisMutationPageEntry &, const KisPageWriteIntent &, const KisPageTransition &);
    void recordCancelled(KisMutationPageEntry &);
    void recordExposure(KisMutationPageEntry &, bool exposed);
    KisPageTransition writeTransition(const KisMutationPageEntry &, KisPageTransactionId,
                                     const KisReplicaHandle &source, const KisReplicaHandle &target) const;

    [[nodiscard]] bool beginSessionActivity(KisPageTransactionId, QMutexLocker<QMutex> &, QString *error = nullptr);
    void endSessionActivity(KisPageTransactionId);
    [[nodiscard]] bool beginPreparationActivity(KisPageTransactionId, QMutexLocker<QMutex> &, QString *error = nullptr);
    void endPreparationActivity(KisPageTransactionId);
    [[nodiscard]] bool beginGenericActivity(KisPageTransactionId);
    void endGenericActivity(KisPageTransactionId);
    bool transactionHasMutationActivity(KisPageTransactionId) const;
    bool transactionHasSessionOrPreparation(KisPageTransactionId) const;
    bool transactionHasSession(KisPageTransactionId) const;

private:
    struct TransactionActivity {
        qsizetype sessions = 0;
        qsizetype preparations = 0;
        qsizetype genericWrites = 0;

        bool isEmpty() const
        {
            return sessions == 0 && preparations == 0 && genericWrites == 0;
        }
    };

    void endActivity(KisPageTransactionId, qsizetype TransactionActivity::*member);
    bool beginActivity(KisPageTransactionId, qsizetype TransactionActivity::*member,
                       QMutexLocker<QMutex> &, QString *error);

    KisPageMetadataCoordinator *metadata = nullptr;
    KisImageEpochReferenceModel *epoch = nullptr;
    KisBackingBudgetController *budget = nullptr;
    KisPageOwnerLedger *ownerLedger = nullptr;
    std::vector<std::shared_ptr<KisPageReplicaTransferBridge>,
        KisMutationStorageAllocator<std::shared_ptr<KisPageReplicaTransferBridge>>> transferBridges;
    KisMutationAdmissionTable<quint64, TransactionActivity> transactionActivities;
};

static_assert(sizeof(KisPageWriteIntent) <= 32);
static_assert(sizeof(KisPageWriteAdmission::ClaimSet) <= 40);
static_assert(sizeof(KisBackingBudgetReservation) <= 40);
static_assert(sizeof(KisMutationPageEntry) <= 104);

#endif // KIS_PAGE_WRITE_COORDINATOR_P_H

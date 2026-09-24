/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_WRITE_COORDINATOR_P_H
#define KIS_PAGE_WRITE_COORDINATOR_P_H

#include "KisPageStoreTypes.h"

#include <QHash>
#include <QMutex>
#include <QSharedPointer>
#include <QThread>
#include <QVector>
#include <QWaitCondition>

#include <array>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

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
class KisPageStore;
class KisPageStoreWriteReservation;
struct KisPageTransition;
struct KisPageTransitionResult;

enum class KisPageWriteInputKind : quint8 {
    MutableGuard,
    Semantic
};

enum class KisPageWritePlanKind : quint8 {
    SemanticOnly,
    ReusePending,
    FreshPayload,
    FreshDiscard,
    FreshCow
};

enum class KisPageWriteIntentFlag : quint8 {
    InputBytesReady = 1u << 0,
    SemanticRemoval = 1u << 1,
    AsyncLease = 1u << 2
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

    friend class KisBackingBudgetController;
    friend class KisPageWriteCoordinator;
    friend class KisPageOwnerLedger;
    friend class KisPageDefaultStorage;
};

class KRITAIMAGE_EXPORT KisBackingBudgetController final
{
public:
    explicit KisBackingBudgetController(const KisPageBackingLimits &limits = {});

    KisBackingBudgetReservation reserve(const KisBackingBudgetDelta &, QString *error);
    void commitReservation(KisBackingBudgetReservation &&reservation,
                           const KisBackingBudgetDelta &installed) noexcept;
    KisPageBackingUsage usage() const;
    quint32 maxTransientVersionsPerPage() const;
    bool configureLimits(const KisPageBackingLimits &, QString *error);
    void releaseLive(KisBackingBudgetClass, KisPageAccessDomain, quint64 bytes) noexcept;

private:
    struct ReservationSlot {
        KisBackingBudgetDelta delta;
        quint32 generation = 0;
        bool active = false;
    };

    ReservationSlot *activeReservation(quint64 cookie)
    {
        const quint32 index = quint32(cookie);
        if (index >= m_slots.size() || !m_slots[index].active
            || m_slots[index].generation != quint32(cookie >> 32))
            return nullptr;
        return &m_slots[index];
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
    void release(quint64 cookie) noexcept;
    bool reservationCovers(quint64 cookie, KisBackingBudgetClass,
                           KisPageAccessDomain, quint64 bytes) const;
    bool reservationCovers(quint64 cookie, const KisBackingBudgetDelta &) const;

    mutable QMutex m_mutex;
    KisPageBackingLimits m_limits;
    KisPageBackingUsage m_usage;
    std::vector<ReservationSlot> m_slots;
    std::vector<quint32> m_freeSlots;

    friend class KisBackingBudgetReservation;
    friend class KisPageOwnerLedger;
};

class KRITAIMAGE_EXPORT KisMutationPageEntry final
{
public:
    explicit KisMutationPageEntry(const KisPageWriteIntent &value) : intent(value) {}
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
    const QSharedPointer<const KisPageReplicaSource> &initializationSource() const { return initialization; }
    void setInitializationSource(QSharedPointer<const KisPageReplicaSource> source)
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
    QSharedPointer<const KisPageReplicaSource> initialization;
    quint32 coldPageIndex = std::numeric_limits<quint32>::max();
    KisMutationPageEntryState state = KisMutationPageEntryState::IntentOnly;

    friend class KisPageWriteCoordinator;
};

class KRITAIMAGE_EXPORT KisMutationWriteSet final
{
public:
    using EntryIndex = quint32;
    static constexpr EntryIndex InvalidEntry = std::numeric_limits<EntryIndex>::max();

    KisMutationWriteSet() = default;
    KisMutationWriteSet(KisMutationWriteSet &&) noexcept = default;
    KisMutationWriteSet &operator=(KisMutationWriteSet &&) noexcept = default;
    KisMutationWriteSet(const KisMutationWriteSet &) = delete;
    KisMutationWriteSet &operator=(const KisMutationWriteSet &) = delete;

    KisMutationPageEntry *find(const KisPageKey &);
    const KisMutationPageEntry *find(const KisPageKey &) const;
    EntryIndex findIndex(const KisPageKey &) const;
    KisMutationPageEntry &getOrCreate(const KisPageWriteIntent &);
    KisMutationPageEntry *at(EntryIndex);
    const KisMutationPageEntry *at(EntryIndex) const;
    void reserveKnownTargetCount(qsizetype);
    qsizetype size() const;

private:
    void ensureIndex();

    std::optional<KisMutationPageEntry> inlineEntry;
    std::vector<KisMutationPageEntry> overflow;
    std::optional<QHash<KisPageKey, EntryIndex>> index;
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

    KisPageWriteAdmission(QMutex &ownerMutex, QWaitCondition &ownerCondition);
    qsizetype activeNativeClaimCountLocked() const;
    qsizetype activeGenericClaimCountLocked() const;
    Conflict conflictLocked(const KisPageKey &, Qt::HANDLE requester) const;
    bool pageClaimedLocked(const KisPageKey &key) const { return m_claims.contains(key); }
    bool claimDirectLocked(const KisPageKey &, quint64 ownerToken);
    void releaseDirectLocked(const KisPageKey &, quint64 ownerToken) noexcept;
    ClaimSet beginClaimSet(const KisMutationWriteSet &);
    bool claimOne(ClaimSet &, KisMutationWriteSet::EntryIndex, QString *error,
                  ClaimOrigin origin = ClaimOrigin::NativeSession);
    bool claimAll(ClaimSet &, QString *error,
                  ClaimOrigin origin = ClaimOrigin::NativeSession);
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
    QWaitCondition *m_ownerCondition = nullptr;
    QHash<KisPageKey, ActiveClaim> m_claims;
    quint64 m_nextClaimToken = 1;
};

// An adapter owns its claimed write set until it transfers to a mutation
// session or a semantic operation terminates. ClaimSet
// references this stable heap object or the session set, never a copied key array.
class KisPageStoreWriteReservation final
{
private:
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

    KisBackingBudgetReservation reserveBacking(const KisPageAllocationDescriptor &descriptor,
                                                KisPageAccessDomain domain,
                                                KisBackingBudgetClass budgetClass,
                                                QString *error,
                                                const KisPageVersion &target = {});

    // Caller holds owner admission. The output is transient transition input,
    // not a second page/overlay authority; no provider allocation occurs here.
    bool prepareWriteBaseLocked(const KisPageTransaction &, const KisPageWriteIntent &,
                                KisPagePublicationCoordinator &, KisPageTransition *,
                                KisPageAllocationDescriptor *, QString *error,
                                const KisPageTransition *pending = nullptr);
    KisPageTransitionResult preparePrivateWrite(KisPageTransition &, bool initialized);
    KisPageTransitionResult publishPrivateWrite(KisPageTransition);
    KisReplicaOperation prepareFreshReplica(const KisPageWriteIntent &,
        KisPageReplicaProvider &, const KisPageTransition &,
        const KisPageAllocationDescriptor &, KisPageAccessRequirement,
        KisPagePriority, const KisCpuPagePayload *,
        const QSharedPointer<const KisPageReplicaSource> &initialization,
        bool *synchronousCopy = nullptr) const;
    bool registerTransferBridge(const QSharedPointer<KisPageReplicaTransferBridge> &bridge);
    KisCompletionTicket initializeFreshReplica(
        const KisPageWriteIntent &intent, bool initializedDuringAllocation,
        const KisReplicaHandle &source, const KisReplicaHandle &target,
        const KisPageAllocationDescriptor &descriptor,
        const QSharedPointer<KisPageReplicaProvider> &targetProvider,
        KisPagePriority priority, const KisCompletionTicket &allocationReadiness,
        QString *error) const;
    // The caller retains admission until detach succeeds. Metadata, not an
    // adapter-local flag, determines which private state must be cancelled.
    KisPageTransitionResult cancelPrivateWrite(KisPageTransition write);

    KisPageWritePlanKind select(const KisPageWriteIntent &,
                                const KisMutationPageEntry *pending = nullptr) const;
    void recordPrepared(KisMutationPageEntry &, const KisPageWriteIntent &, const KisPageTransition &);
    void recordExposure(KisMutationPageEntry &, bool exposed);
    KisPageTransition writeTransition(const KisMutationPageEntry &, KisPageTransactionId,
                                     const KisReplicaHandle &source, const KisReplicaHandle &target) const;

    void beginSessionActivity(KisPageTransactionId);
    void endSessionActivity(KisPageTransactionId);
    void beginPreparationActivity(KisPageTransactionId);
    void endPreparationActivity(KisPageTransactionId);
    void beginGenericActivity(KisPageTransactionId);
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

    KisPageMetadataCoordinator *metadata = nullptr;
    KisImageEpochReferenceModel *epoch = nullptr;
    KisBackingBudgetController *budget = nullptr;
    KisPageOwnerLedger *ownerLedger = nullptr;
    QVector<QSharedPointer<KisPageReplicaTransferBridge>> transferBridges;
    QHash<quint64, TransactionActivity> transactionActivities;
};

static_assert(sizeof(KisPageWriteIntent) <= 32);
static_assert(sizeof(KisPageWriteAdmission::ClaimSet) <= 40);
static_assert(sizeof(KisBackingBudgetReservation) <= 40);
static_assert(sizeof(KisMutationPageEntry) <= 104);

#endif // KIS_PAGE_WRITE_COORDINATOR_P_H

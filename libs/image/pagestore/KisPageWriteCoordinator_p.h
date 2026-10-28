/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_WRITE_COORDINATOR_P_H
#define KIS_PAGE_WRITE_COORDINATOR_P_H

#include "KisPageReadiness_p.h"
#include "KisPageBackingBudget_p.h"
#include "KisPagePublicationInput_p.h"
#include <boost/container/small_vector.hpp>
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
struct KisPageIdleCapacity;
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
class KisCpuResidentBinding;
struct KisPageTransition;
struct KisPagePrivateWrite;
struct KisPageWriteSelection;
enum class KisPageTransitionKind : quint8;
struct KisPageMetadataTransitionResult;

enum class KisPageWriteInputKind : quint8 {
    MutableGuard,
    FinalPayload,
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
    AsyncLease = 1u << 2,
    SourceInitialization = 1u << 3
};

enum class KisMutationPageEntryState : quint8 {
    IntentOnly,
    MutableReady,
    Exposed,
    PrivatePrepared,
    AliasAccepted
};

struct KisPageWriteIntent {
    KisPageKey key;
    KisPageWriteMode mode = KisPageWriteMode::PreserveContents;
    KisPageWriteInputKind inputKind = KisPageWriteInputKind::MutableGuard;
    quint8 flags = 0;
};

class KRITAIMAGE_EXPORT KisMutationPageEntry final : public KisPagePublicationSource
{
public:
    explicit KisMutationPageEntry(const KisPageWriteIntent &value) noexcept : intent(value) {}
    KisMutationPageEntry(KisMutationPageEntry &&) noexcept = default;
    KisMutationPageEntry &operator=(KisMutationPageEntry &&) noexcept = default;
    KisMutationPageEntry(const KisMutationPageEntry &) = delete;
    KisMutationPageEntry &operator=(const KisMutationPageEntry &) = delete;

    const KisPageKey &key() const { return intent.key; }
    KisPageVersion preparedTargetVersion() const;
    bool isExposed() const { return state == KisMutationPageEntryState::Exposed; }
    bool acceptsMutableInput() const { return state < KisMutationPageEntryState::Exposed; }
    bool hasPrivateResult() const
    { return state == KisMutationPageEntryState::PrivatePrepared || state == KisMutationPageEntryState::AliasAccepted; }
    bool hasAcceptedAlias() const { return state == KisMutationPageEntryState::AliasAccepted; }
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
    KisPagePrivateWrite *coldPage() const { return coldPageStorage; }
    void setColdPage(KisPagePrivateWrite *page) { coldPageStorage = page; }

private:
    KisPageWriteIntent intent;
    // The key already belongs to intent; the target/source belong to the
    // original cold result. Retain only the independent base identity suffix.
    KisPageGeneration baseGeneration;
    quint64 baseDefaultPixelRevision = 0;
    std::shared_ptr<const KisPageReplicaSource> initialization;
    KisPagePrivateWrite *coldPageStorage = nullptr;
    KisMutationPageEntryState state = KisMutationPageEntryState::IntentOnly;

    quint32 slotIndex = 0;
    friend class KisMutationWriteSet;
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
    KisMutationPageEntry &getOrCreate(const KisPageWriteIntent &, bool *inserted = nullptr);
    KisMutationPageEntry *at(EntryIndex);
    const KisMutationPageEntry *at(EntryIndex) const;
    KisMutationPageEntry *at(EntryHandle);
    const KisMutationPageEntry *at(EntryHandle) const;
    EntryHandle handleAt(EntryIndex) const;
    // The caller still owns this original entry; do not rediscover it by key.
    EntryHandle handleOf(const KisMutationPageEntry &) const;
    EntryHandle firstEntry() const;
    EntryHandle nextEntry(EntryHandle) const;
    // Caller first terminates cold resources and releases this entry's claim.
    bool erase(EntryHandle);
    ReleasedBlocks takeReleasedBlocks() noexcept { return overflow.takeReleasedBlocks(); }
    void reserveKnownTargetCount(qsizetype);
    void clearValues() noexcept;
    size_t storageBytes() const noexcept;
    qsizetype size() const;
    // Freeze the one original directory: actual changes first, claims-only
    // rows last. No entry moves, and retries reuse this same frozen D.
    KisPagePublicationRows publicationRows();

private:
    // Private locator only. Keys and incarnations stay on the original
    // stable rows; no copied key or public capability lives in this index.
    using Index = boost::container::small_vector<KisPagePublicationSource *, 1,
        KisMutationStorageAllocator<KisPagePublicationSource *>>;
    void rebindInline(const KisMutationPageEntry *old) noexcept;
    size_t directoryPosition(const KisPageKey &) const;
    size_t directoryBucket(const Index &, const KisPageKey &) const noexcept;
    Index prepareDirectory(qsizetype count) const;
    bool directoryHashed() const { return index && !publishedCount && index->size() > size_t(size()); }

    std::optional<KisMutationPageEntry> inlineEntry;
    quint64 inlineIncarnation = 0;
    quint64 nextInlineIncarnation = 1;
    KisMutationStorage<KisMutationPageEntry, 8> overflow;
    std::optional<Index> index;
    // Only semantic deletions lack an actual native page record.
    std::optional<KisPagePublicationChanges> semanticRows;
    KisBackingBudgetController *budget = nullptr;
    std::optional<size_t> publishedCount;
    size_t indexSeed = QHashSeed::globalSeed();
    mutable size_t indexStorageCapacity = 0;
    mutable size_t indexStorageBytes = 0;
    mutable size_t semanticStorageCapacity = 0;
    mutable size_t semanticStorageBytes = 0;
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
        // No further execution/delivery may consume the original write rows.
        // Admission retains the keys; this terminal owner retains only token.
        void retainTokenForTerminal() noexcept;
        bool isTerminal() const { return isValid() && !writeSet; }
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
    // The caller has bound these unique keys to its original write-set rows
    // under the session gate, and retains that gate across admission growth.
    // Only dynamic competing claims need resolution here, not the write set.
    bool claimRange(ClaimSet &, KisSurfaceId, const KisPageSnapshotArray<KisLogicalPageId> &,
                    QMutexLocker<QMutex> &, QString *error);
    bool ownsClaimSetLocked(const ClaimSet &) const;
    bool ownsClaimLocked(const ClaimSet &, const KisPageKey &) const;

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

    KisPageMetadataTransitionResult acquireLeaseWrite(KisPageWriteInput &, const KisReplicaHandle &target);
    // Both adapters try the same physical reuse/metadata/accounting protocol.
    // A success writes the original private or request target, not selection.
    // A refusal leaves selection/result unchanged for Fresh. Admission spans
    // unlocked work; native accepts directly, only a lease projects a transition.
    bool tryAcquireReusedTargetLocked(
        const KisPageTransaction &, const KisPageWriteIntent &,
        const std::shared_ptr<KisPageReplicaProvider> &, KisPageAccessRequirement,
        KisPageWriteSelection &, KisPagePublicationCoordinator &,
        KisPageIdleCapacity idleCapacity,
        QMutexLocker<QMutex> &, KisReplicaHandle *target,
        KisCpuWriteBindingReservation &, KisPageStoreDiagnosticTimer &,
        KisPagePrivateWrite *native = nullptr);
    KisPageMetadataTransitionResult publishLeaseWrite(KisPageTransition);
    KisReplicaOperation prepareFreshReplica(const KisPageWriteIntent &,
        KisPageReplicaProvider &, const KisPageWriteInput &,
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
    KisPageMetadataTransitionResult cancelLeaseWrite(KisPageTransition write);

    KisPageWritePlanKind select(const KisPageWriteIntent &,
                                const KisMutationPageEntry *pending = nullptr) const;
    void recordTarget(KisMutationPageEntry &, const KisPageWriteIntent &, const KisPageWriteInput &,
                      KisMutationPageEntryState);
    void recordPrivateCompletion(KisMutationPageEntry &);
    void recordAliasAccepted(KisMutationPageEntry &);
    void recordCancelled(KisMutationPageEntry &);
    void recordExposure(KisMutationPageEntry &, bool exposed);
    KisPageWriteInput writeInput(const KisMutationPageEntry &, KisPageTransactionId) const;

    [[nodiscard]] bool beginSessionActivity(KisPageTransactionId, QMutexLocker<QMutex> &, QString *error = nullptr);
    void endSessionActivity(KisPageTransactionId);
    [[nodiscard]] bool beginPreparationActivity(KisPageTransactionId, QMutexLocker<QMutex> &, QString *error = nullptr);
    void endPreparationActivity(KisPageTransactionId);
    [[nodiscard]] bool beginGenericActivity(KisPageTransactionId);
    void endGenericActivity(KisPageTransactionId);
    bool transactionHasMutationActivity(KisPageTransactionId) const;
    bool transactionHasSessionOrPreparation(KisPageTransactionId) const;
    bool transactionHasSession(KisPageTransactionId) const;
    bool transactionHasSoleSession(KisPageTransactionId) const;

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
// Direct cold borrow replaces the separately allocated slot directory.
// Only an original producer link is added to the compact permission entry.
static_assert(sizeof(KisMutationPageEntry) <= 120);

#endif // KIS_PAGE_WRITE_COORDINATOR_P_H

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
class KisCpuReplicaPreparation;
struct KisPageTransition;
struct KisPagePrivateWrite;
class KisPreparedPrivateTarget;
struct KisPageWriteSelection;
enum class KisPageTransitionKind : quint8;
struct KisPageMetadataTransitionResult;

// Stored once in the stable PageControl. Only the containing Store gate
// protects these fields; metadata's shard gate protects the surrounding page.
struct KisPageWriteClaim {
    enum class Origin : quint8 { NativeSession, ManagedRange, LegacyAdapter, GenericWrite };
    quint64 token = 0;
    Qt::HANDLE thread = nullptr;
    KisPageWriteClaim *previous = nullptr;
    KisPageWriteClaim *next = nullptr;
    Origin origin = Origin::NativeSession;
    // Non-owning adapter fact under this original page claim. Unlink clears
    // it before another Frame can acquire this PageControl. No second owner.
    void *adapterLocation = nullptr;
};

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
    // Structural ownership, protected by the containing Frame gate. A live
    // execution owns this row even when it currently has no pixel pin.
    bool isExecutionBorrowed() const { return executionBorrowed; }
    void setExecutionBorrowed(bool borrowed) { executionBorrowed = borrowed; }
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
    KisPageWriteClaim *writeClaim = nullptr; // Stable original PageControl location.
    KisMutationPageEntryState state = KisMutationPageEntryState::IntentOnly;
    bool executionBorrowed = false;

public:
    // Reuse the original PageControl claim, avoiding growth of every Frame
    // row and of the inline single-page Frame allocation.
    void *&adapterLocation() const { Q_ASSERT(writeClaim && writeClaim->token); return writeClaim->adapterLocation; }
private:
    quint32 slotIndex = 0;
    friend class KisPageWriteAdmission;
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
    // The scope/range owner keeps membership stable throughout this visit.
    // Public handles still validate their issued incarnation separately.
    template<class Visit> bool visitEntries(const Visit &visit) const
    {
        return (!inlineEntry || visit(*inlineEntry)) && overflow.visitLive(visit);
    }
    template<class Visit> bool visitEntries(const Visit &visit)
    {
        return (!inlineEntry || visit(*inlineEntry)) && overflow.visitLive(visit);
    }
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
    // The frozen directory already separates actual D from claims-only rows.
    // Its original entries survive result cleanup through adapter delivery;
    // never dereference their released publication workspace at this phase.
    template<class Visit> bool visitPublishedEntries(const Visit &visit) const
    {
        if (!publishedCount) return true; // A successful no-op never froze D.
        Q_ASSERT(index);
        for (size_t i = 0; i < *publishedCount; ++i)
            if (!visit(*static_cast<const KisMutationPageEntry *>((*index)[i]))) return false;
        return true;
    }

private:
    // Private locator only. Keys and incarnations stay on the original
    // stable rows; no copied key or public capability lives in this index.
    using Index = boost::container::small_vector<KisPagePublicationSource *, 1,
        KisMutationStorageAllocator<KisPagePublicationSource *>>;
    void rebindInline(const KisMutationPageEntry *old) noexcept;
    size_t directoryPosition(const KisPageKey &) const;
    size_t directoryBucket(const Index &, const KisPageKey &) const noexcept;
    void growDirectory(qsizetype count);
    bool directoryHashed() const { return index && !publishedCount && index->size() > size_t(size()); }

    KisPageMetadataCoordinator *claimMetadata = nullptr;
    friend class KisPageWriteAdmission;
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
    using ClaimOrigin = KisPageWriteClaim::Origin;
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

    class LegacyInspection final
    {
    public:
        explicit operator bool() const { return owner; }
    private:
        const KisPageWriteAdmission *owner = nullptr;
        const KisPageKey *key = nullptr; // Original caller input stays alive until claimOne.
        KisPageWriteClaim *location = nullptr;
        friend class KisPageWriteAdmission;
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
        // No further execution/delivery may consume the original write rows.
        // Stable PageControls retain the claims; this owner retains their chain.
        void retainClaimsForTerminal() noexcept;
        bool isTerminal() const { return isValid() && !writeSet; }
        void release() noexcept;
        void releaseLocked() noexcept;

    private:
        void releaseImpl(bool lockOwner) noexcept;
        KisPageWriteAdmission *owner = nullptr;
        KisMutationWriteSet *writeSet = nullptr;
        KisPageWriteClaim *head = nullptr;
        quint64 token = 0;

        friend class KisPageWriteAdmission;
    };

    KisPageWriteAdmission(KisPageMetadataCoordinator &, QMutex &ownerMutex, KisPageWaitCondition &ownerCondition,
                          const bool *operational = nullptr);
    qsizetype activeNativeClaimCountLocked() const;
    qsizetype activeGenericClaimCountLocked() const;
    Conflict conflictLocked(const KisPageKey &, Qt::HANDLE requester) const;
    // Store-gated original row keeps the location for the subsequent claim.
    Conflict conflictLocked(KisMutationPageEntry &, Qt::HANDLE requester) const;
    bool pageClaimedLocked(const KisPageKey &) const;
    // One legacy entry inspection, before fallible operation preparation.
    // A null location means a real first key, not authority to skip the
    // final claim. Store admission and original metadata lifetime are held.
    LegacyInspection inspectLegacyClaimLocked(const KisPageKey &, QString *error) const;
    // Growth temporarily releases ownerLock. The caller retains the original
    // owner/transaction and serializes its write set independently (session
    // mutex or a private range). Never pass a mutable unprotected write set.
    bool claimDirectLocked(const KisPageKey &, quint64 ownerToken, QMutexLocker<QMutex> &, QString *error = nullptr);
    void releaseDirectLocked(const KisPageKey &, quint64 ownerToken) noexcept;
    ClaimSet beginClaimSet(KisMutationWriteSet &);
    bool claimOne(ClaimSet &, KisMutationWriteSet::EntryHandle, QMutexLocker<QMutex> &, QString *error,
                  ClaimOrigin origin = ClaimOrigin::NativeSession,
                  const LegacyInspection *inspection = nullptr);
    bool releaseOneLocked(ClaimSet &, KisMutationWriteSet::EntryHandle) noexcept;
    // Contention observed across growth remains distinct from real storage
    // refusal, even if that writer releases while unused growth is freed.
    Result claimAll(ClaimSet &, QMutexLocker<QMutex> &, QString *error,
                  ClaimOrigin origin = ClaimOrigin::NativeSession);
    // Borrow the existing execution bindings; no copied keys or second row directory.
    struct Rows {
        const void *data;
        size_t count;
        KisMutationPageEntry *(*at)(const void *, size_t);
    };
    bool claimRange(ClaimSet &, Rows, QMutexLocker<QMutex> &, QString *error);
    bool ownsClaimSetLocked(const ClaimSet &) const;
    bool ownsClaimLocked(const ClaimSet &, const KisPageKey &) const;
    bool ownsEntryLocked(const ClaimSet &, const KisMutationPageEntry &) const;

private:
    template<class Visit> Result claimEntries(ClaimSet &, Visit &&, QMutexLocker<QMutex> &,
                                             QString *, ClaimOrigin);
    static Conflict classify(const KisPageWriteClaim *, Qt::HANDLE requester);
    bool valid(const ClaimSet &) const;
    bool owned(const KisPageWriteClaim *, quint64 token, bool generic = false) const;
    void attach(ClaimSet &, KisPageWriteClaim &, ClaimOrigin);
    void unlink(ClaimSet &, KisPageWriteClaim &) noexcept;
    KisPageWriteClaim *locate(const KisPageKey &, QMutexLocker<QMutex> &, QString *, bool *unlocked = nullptr);
    void release(ClaimSet &, bool lockOwner) noexcept;

    KisPageMetadataCoordinator *m_metadata;
    QMutex *m_ownerMutex = nullptr;
    KisPageWaitCondition *m_ownerCondition = nullptr;
    qsizetype m_nativeCount = 0;
    qsizetype m_genericCount = 0;
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

    // Store admission gate must be held through metadata acceptance. Native
    // acceptance consumes the source phase in its existing shard window.
    bool validateWriteBase(const KisPageWriteInput &, bool native, QString *error = nullptr) const;
    KisPageMetadataTransitionResult acquireLeaseWrite(KisPageWriteSelection &, const KisReplicaHandle &target);
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
        KisPreparedPrivateTarget *preparedNative = nullptr);
    KisPageMetadataTransitionResult publishLeaseWrite(KisPageTransition);
    KisReplicaOperation prepareFreshReplica(const KisPageWriteIntent &,
        KisPageReplicaProvider &, const KisPageWriteSelection &,
        KisPageAccessRequirement,
        KisPagePriority, const KisCpuPagePayload *,
        const std::shared_ptr<const KisPageReplicaSource> &initialization,
        bool *synchronousCopy = nullptr) const;
    KisCpuReplicaPreparation prepareFreshCpuReplica(const KisPageWriteIntent &,
        KisPageReplicaProvider &, KisPageWriteSelection &, KisPagePriority,
        const KisCpuPagePayload *, const std::shared_ptr<const KisPageReplicaSource> &) const;
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

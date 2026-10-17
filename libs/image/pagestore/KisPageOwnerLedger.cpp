/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageOwnerLedger.h"
#include "KisPageRetirementRecord_p.h"
#include "KisPageWriteCoordinator_p.h"
#include "KisPageMetadataCoordinator_p.h"
#include "KisPagePublicationInput_p.h"

#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <QScopeGuard>
#include "KisPageWaitCondition_p.h"

#include <boost/intrusive/set.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <map>

namespace {

std::atomic<quint64> s_nextRequestId{1};
std::atomic<quint64> s_nextOperationId{1};
std::atomic<quint64> s_nextLeaseId{1};
std::atomic<quint64> s_nextWriterToken{1};
std::atomic<quint64> s_nextProofIssuer{1};
struct ProofIssuerId { quint64 value = 0; };

using ProviderKey = QPair<quint64, quint64>;

ProviderKey providerKey(KisReplicaProviderId provider,
                        KisReplicaProviderEpoch epoch)
{
    return {provider.value, epoch.value};
}

using ProviderOperationRecord = KisPageProviderOperationRecord;
using OperationIndex = KisPageProviderOperationIndex;

template<class T> struct DeleteLedgerNode
{
    void operator()(T *node) const noexcept
    {
        if (!node) return;
        KisMutationStorageAllocator<T> allocator(node->budget);
        std::destroy_at(node);
        allocator.deallocate(node, 1);
    }
};
template<class T> using LedgerNode = std::unique_ptr<T, DeleteLedgerNode<T>>;
template<class T> LedgerNode<T> prepareLedgerNode(KisBackingBudgetController *budget)
{
    KisMutationStorageAllocator<T> allocator(budget);
    T *node = allocator.allocate(1);
    try {
        std::allocator_traits<KisMutationStorageAllocator<T>>::construct(allocator, node);
    } catch (...) {
        allocator.deallocate(node, 1);
        throw;
    }
    node->budget = budget;
    return LedgerNode<T>(node);
}
template<class T, class Compare>
class LedgerIndex : public boost::intrusive::set<T, boost::intrusive::compare<Compare>>
{
public:
    ~LedgerIndex() { this->clear_and_dispose(DeleteLedgerNode<T>{}); }
};

struct ProviderRegistration : boost::intrusive::set_base_hook<>
{
    KisBackingBudgetController *budget = nullptr;
    ProviderKey key;
    std::shared_ptr<KisPageReplicaProvider> provider;
    bool backgroundRetirement = false;
    quint64 acceptedRevision = 0; // zero: original registration is still pending
};
struct ProviderLess
{
    bool operator()(const ProviderRegistration &a, const ProviderRegistration &b) const { return a.key < b.key; }
    bool operator()(const ProviderKey &a, const ProviderRegistration &b) const { return a < b.key; }
    bool operator()(const ProviderRegistration &a, const ProviderKey &b) const { return a.key < b; }
};
using ProviderIndex = LedgerIndex<ProviderRegistration, ProviderLess>;

struct SealedProofRecord : boost::intrusive::set_base_hook<>
{
    KisBackingBudgetController *budget = nullptr;
    KisPreparedPageProof proof;
};
struct ProofLess
{
    bool operator()(const SealedProofRecord &a, const SealedProofRecord &b) const
    { return a.proof.providerValidationStamp < b.proof.providerValidationStamp; }
    bool operator()(quint64 a, const SealedProofRecord &b) const { return a < b.proof.providerValidationStamp; }
    bool operator()(const SealedProofRecord &a, quint64 b) const { return a.proof.providerValidationStamp < b; }
};
using ProofIndex = LedgerIndex<SealedProofRecord, ProofLess>;

using BackingKey = KisReplicaAllocationIdentity;
using PhysicalBackingKey = KisReplicaPhysicalSlotIdentity;

template<class T> using ChargedVector = std::vector<T, KisMutationStorageAllocator<T>>;
// Generic class-change cookies always have a nonzero upper generation word.
// A handoff instead lives in its original move-only caller reservation.
constexpr quint64 HandoffClaim = 1;

PhysicalBackingKey physicalBackingKey(
    const KisReplicaHandle &replica,
    const KisReplicaBackingFootprint &footprint)
{
    return {replica.provider, replica.providerEpoch, footprint.physicalSlot};
}

void addBackingDelta(KisBackingBudgetDelta *delta, KisBackingBudgetClass budgetClass,
                     KisPageAccessDomain domain, qint64 bytes)
{
    auto &bucket = delta->buckets[static_cast<size_t>(budgetClass)];
    if (qint64 *component = kisPageDomainComponent(bucket, domain))
        *component += bytes;
}

bool addBackingDeltaChecked(KisBackingBudgetDelta *delta,
                            KisBackingBudgetClass budgetClass,
                            KisPageAccessDomain domain, qint64 bytes)
{
    auto &bucket = delta->buckets[static_cast<size_t>(budgetClass)];
    qint64 *component = kisPageDomainComponent(bucket, domain);
    if (!component) return false;
    qint64 result = 0;
    if (__builtin_add_overflow(*component, bytes, &result)) return false;
    *component = result;
    return true;
}

KisBackingBudgetDelta positiveBackingDelta(const KisBackingBudgetDelta &signedDelta)
{
    KisBackingBudgetDelta result;
    for (size_t i = 0; i < signedDelta.buckets.size(); ++i) {
        const auto &source = signedDelta.buckets[i];
        auto &target = result.buckets[i];
        target.cpuRam = std::max(qint64(0), source.cpuRam);
        target.umaShared = std::max(qint64(0), source.umaShared);
        target.discreteVram = std::max(qint64(0), source.discreteVram);
        target.ssd = std::max(qint64(0), source.ssd);
    }
    return result;
}

bool hasBackingDelta(const KisBackingBudgetDelta &delta)
{
    return std::any_of(delta.buckets.begin(), delta.buckets.end(), [](const auto &bucket) {
        return bucket.cpuRam || bucket.umaShared || bucket.discreteVram || bucket.ssd;
    });
}

using BackingRecord = KisPageBackingRecord;
using BackingIndex = KisPageBackingIndex;
using PhysicalBackingRecord = KisPagePhysicalBackingRecord;
using PhysicalBackingIndex = KisPagePhysicalBackingIndex;

KisBackingBudgetClass chargedBackingClass(const PhysicalBackingRecord &record)
{
    // Physical bytes are charged once. Prefer the class that must remain live
    // for foreground correctness over rebuildable/history/debt ownership.
    constexpr std::array<KisBackingBudgetClass, 8> priority{
        KisBackingBudgetClass::Current,
        KisBackingBudgetClass::ActivePending,
        KisBackingBudgetClass::InFlight,
        KisBackingBudgetClass::RetainedHistory,
        KisBackingBudgetClass::RetirementDebt,
        KisBackingBudgetClass::OptionalCache,
        KisBackingBudgetClass::IdleCapacity,
        KisBackingBudgetClass::MetadataArena};
    for (const KisBackingBudgetClass budgetClass : priority) {
        if (record.references[static_cast<size_t>(budgetClass)] != 0)
            return budgetClass;
    }
    return KisBackingBudgetClass::Count;
}

bool changePhysicalReferenceClass(PhysicalBackingRecord *record,
                                  KisBackingBudgetClass before,
                                  KisBackingBudgetClass after)
{
    if (!record || before == after)
        return false;
    auto &beforeCount = record->references[static_cast<size_t>(before)];
    auto &afterCount = record->references[static_cast<size_t>(after)];
    if (!beforeCount || afterCount == std::numeric_limits<quint32>::max())
        return false;
    --beforeCount;
    ++afterCount;
    return true;
}

bool addPhysicalChargeChange(KisBackingBudgetDelta *delta,
                             const PhysicalBackingRecord &record,
                             KisBackingBudgetClass before,
                             KisBackingBudgetClass after)
{
    if (before == after)
        return true;
    if (before == KisBackingBudgetClass::Count
        || after == KisBackingBudgetClass::Count
        || record.byteSize() > quint64(std::numeric_limits<qint64>::max())) {
        return false;
    }
    const qint64 bytes = qint64(record.byteSize());
    return addBackingDeltaChecked(delta, before, record.domain, -bytes)
        && addBackingDeltaChecked(delta, after, record.domain, bytes);
}

struct PreparedRetirementCredit
{
    BackingRecord *backing;
    KisBackingBudgetClass before;
    KisPageAccessDomain domain;
    quint64 bytes;

    KisBackingBudgetDelta delta() const
    {
        KisBackingBudgetDelta value;
        addBackingDelta(&value, before, domain, -qint64(bytes));
        addBackingDelta(&value, KisBackingBudgetClass::RetirementDebt, domain, qint64(bytes));
        return value;
    }
};

struct PreparedBackingChange
{
    // The original physical claim protects both locations until completion.
    // A handoff computes its neighbour afresh, then updates this iterator.
    BackingIndex::iterator backing;
    PhysicalBackingRecord *physical;
    size_t order;
    KisBackingBudgetClass after;
};

void subtractBackingDelta(KisBackingBudgetDelta *total, const KisBackingBudgetDelta &part)
{
    for (size_t i = 0; i < total->buckets.size(); ++i) {
        total->buckets[i].cpuRam -= part.buckets[i].cpuRam;
        total->buckets[i].umaShared -= part.buckets[i].umaShared;
        total->buckets[i].discreteVram -= part.buckets[i].discreteVram;
        total->buckets[i].ssd -= part.buckets[i].ssd;
    }
}

struct PreparedBackingChangeSlot
{
    explicit PreparedBackingChangeSlot(KisBackingBudgetController *budget)
        : changes(KisMutationStorageAllocator<PreparedBackingChange>(budget))
        , retirementCredits(KisMutationStorageAllocator<PreparedRetirementCredit>(budget)) {}
    ChargedVector<PreparedBackingChange> changes;
    // map nodes stay stable under the original physical/target claims.
    // Never retain a neighbouring iterator across the unlocked provider call.
    KisBackingBudgetReservation reservation;
    ChargedVector<PreparedRetirementCredit> retirementCredits;
    quint32 generation = 0;
    bool occupied = false;
};

}

class KisPageOwnerLedger::Private
{
public:
    using Storage = KisMutationStorageAllocator<Private>;
    explicit Private(const Storage &allocator) : storage(allocator) {}
    const ProviderRegistration *registeredProvider(const ProviderKey &key) const
    {
        const auto found = providers.find(key, ProviderLess{});
        return found != providers.end() && found->acceptedRevision ? &*found : nullptr;
    }

    quint64 providerCut() const
    {
        QMutexLocker lock(&mutex);
        return providerRevision;
    }

    std::shared_ptr<KisPageReplicaProvider> nextProviderInCut(quint64 cut, ProviderKey *cursor) const
    {
        QMutexLocker lock(&mutex);
        for (auto next = providers.upper_bound(*cursor, ProviderLess{}); next != providers.end(); ++next) {
            *cursor = next->key;
            if (next->acceptedRevision && next->acceptedRevision <= cut) return next->provider;
        }
        return {};
    }

    bool acceptsProviderResultLocked(KisPageOperationId operation,
                                     const KisReplicaOperation &result, QString *error) const
    {
        if (!operation.isValid() || !result.isValid() || !(result.operation == operation)) {
            KisPageStoreDetail::setError(error, QStringLiteral("provider operation result is invalid or mismatched"));
            return false;
        }
        if (!completions || operations.find(operation.value) != operations.end() ||
            !registeredProvider(providerKey(result.replica.provider, result.replica.providerEpoch))) {
            KisPageStoreDetail::setError(error, QStringLiteral("provider operation is duplicate or foreign"));
            return false;
        }
        return true;
    }

    KisCompletionTicket operationCompletion(
        KisPageOperationId operation,
        std::shared_ptr<KisCompletionRegistry> *registry) const
    {
        QMutexLocker locker(&mutex);
        const auto found = operations.find(operation.value);
        if (!completions || !operation.isValid() || found == operations.end())
            return {};
        *registry = completions;
        return found->second.completion;
    }

    PreparedBackingChangeSlot *preparedChange(quint64 cookie)
    {
        const quint32 slotIndex = quint32(cookie);
        if (slotIndex >= preparedChanges.size())
            return nullptr;
        auto &slot = preparedChanges.at(slotIndex);
        return slot.occupied && slot.generation == quint32(cookie >> 32)
            ? &slot : nullptr;
    }

    bool isPreparedBacking(const BackingRecord &record) const noexcept
    {
        // Free-slot accounting is already authoritative under the ledger gate.
        // The common idle query does not visit either physical or claim indexes.
        if (!activeHandoffs && freePreparedChanges.size() == preparedChanges.size()) return false;
        const auto key = record.replica.allocationIdentity();
        const auto physical = physicalBackings.find(record.physical);
        if (physical == physicalBackings.cend() || !physical->second.classClaim) return false;
        const quint64 cookie = physical->second.classClaim;
        if (cookie == HandoffClaim) return true;
        const quint32 index = quint32(cookie);
        Q_ASSERT(index < preparedChanges.size());
        if (index >= preparedChanges.size()) return true;
        const auto &slot = preparedChanges[index];
        Q_ASSERT(slot.generation == quint32(cookie >> 32));
        if (slot.generation != quint32(cookie >> 32)) return true;
        // A physical claim also excludes unselected aliases from real class
        // changes. Only the selected keys reject a same-class no-op, exactly as
        // the former source-key index did. The original slot owns that set.
        return std::any_of(slot.changes.cbegin(), slot.changes.cend(), [&](const auto &change) {
            return change.backing->first == key;
        });
    }

    bool isHandoffTarget(BackingKey key) const
    {
        if (!activeHandoffs || !key.allocation.generation) return false;
        // A target is exactly its original source allocation's next generation.
        // Registration/release are cold consumers; derive it from the claimed
        // record instead of maintaining another hash index on every handoff.
        --key.allocation.generation;
        const auto source = backings.find(key);
        if (source == backings.end()) return false;
        const auto physical = physicalBackings.find(source->second.physical);
        return physical != physicalBackings.end() && physical->second.classClaim == HandoffClaim;
    }

    template<class NextChange>
    bool buildPhysicalBackingDelta(
        KisBackingClassChange change, NextChange &nextChange, size_t maximum,
        KisBackingBudgetDelta *signedDelta,
        PreparedBackingChangeSlot &slot, quint64 cookie,
        QString *error)
    {
        // The original slot carries accepted locations through commit/cancel.
        // Physical claims keep these map nodes stable outside the ledger gate.
        auto &projected = slot.changes;
        projected.reserve(maximum);
        bool shared = false;
        do {
            const BackingKey key = change.replica.allocationIdentity();
            const auto found = backings.find(key);
            if (!change.replica.isValid() || found == backings.end()
                || found->second.budgetClass != change.before
                || !(found->second.replica == change.replica)
                || change.before == change.after
                || static_cast<size_t>(change.after) >=
                       static_cast<size_t>(KisBackingBudgetClass::Count)) {
                KisPageStoreDetail::setError(error, QStringLiteral("backing class change is invalid"));
                return false;
            }
            // Membership is already authoritative under this gate. Shared
            // rows defer physical lookup to their one grouped visit, rather
            // than paying N lookups just to discover G shared resources.
            const bool alias = found->second.previous || found->second.next;
            PhysicalBackingRecord *physical = nullptr;
            if (!alias) {
                const auto source = physicalBackings.find(found->second.physical);
                if (source == physicalBackings.end()) {
                    KisPageStoreDetail::setError(error, QStringLiteral("physical backing owner is missing"));
                    return false;
                }
                physical = &source->second;
                if (physical->domainClaim || (physical->classClaim && physical->classClaim != cookie)) {
                    KisPageStoreDetail::setError(error, QStringLiteral("physical backing transition is active"));
                    return false;
                }
                if (physical->classClaim == cookie) {
                    KisPageStoreDetail::setError(error, QStringLiteral("backing is classified more than once"));
                    return false;
                }
            }
            // Record before claiming: allocation failure must not strand an
            // untracked claim. All partial claims use the original rollback.
            projected.push_back({found, physical, projected.size(), change.after});
            if (physical) physical->classClaim = cookie;
            shared |= alias;
        } while (nextChange(&change));
        if (shared) {
            const KisPageBackingIdentityLess less;
            std::sort(projected.begin(), projected.end(), [&](const auto &a, const auto &b) {
                return less(a.backing->first, b.backing->first);
            });
            for (size_t i = 1; i < projected.size(); ++i) {
                if (projected[i - 1].backing == projected[i].backing) {
                    KisPageStoreDetail::setError(error, QStringLiteral("backing is classified more than once"));
                    return false;
                }
            }
            std::sort(projected.begin(), projected.end(), [&](const auto &a, const auto &b) {
                if (less(a.backing->second.physical, b.backing->second.physical)) return true;
                if (less(b.backing->second.physical, a.backing->second.physical)) return false;
                // Preserve the caller's order and its original first credit owner.
                return a.order < b.order;
            });
        }
        for (size_t first = 0; first < projected.size();) {
            auto *source = projected[first].physical;
            const auto key = projected[first].backing->second.physical;
            if (!source) {
                const auto found = physicalBackings.find(key);
                if (found == physicalBackings.end()) {
                    KisPageStoreDetail::setError(error, QStringLiteral("physical backing owner is missing"));
                    return false;
                }
                source = &found->second;
                if (source->domainClaim || source->classClaim) {
                    KisPageStoreDetail::setError(error, QStringLiteral("physical backing transition is active"));
                    return false;
                }
            }
            auto next = *source;
            BackingRecord *credit = nullptr;
            do {
                auto &entry = projected[first];
                if (!entry.physical) {
                    entry.physical = source;
                    source->classClaim = cookie;
                }
                if (!credit && entry.backing->second.retirementHeadroom.isValid())
                    credit = &entry.backing->second;
                if (!changePhysicalReferenceClass(&next, entry.backing->second.budgetClass, entry.after)) {
                    KisPageStoreDetail::setError(error, QStringLiteral("physical backing class references are invalid"));
                    return false;
                }
                ++first;
            } while (first < projected.size() && projected[first].backing->second.physical == key);
            if (!addPhysicalChargeChange(signedDelta, next,
                                         source->chargedClass,
                                         chargedBackingClass(next))) {
                KisPageStoreDetail::setError(error, QStringLiteral("physical backing delta overflows"));
                return false;
            }
            if (source->chargedClass != KisBackingBudgetClass::RetirementDebt
                && chargedBackingClass(next) == KisBackingBudgetClass::RetirementDebt) {
                // A physical charge needs one credit even when all its aliases
                // retire together. Only touched records are inspected.
                if (credit)
                    slot.retirementCredits.push_back({credit, source->chargedClass,
                                        source->domain, source->byteSize()});
            }
        }
        return true;
    }

    template<class NextChange>
    bool prepareClassChanges(size_t maximum, NextChange nextChange, quint64 *cookie, QString *error)
    {
        *cookie = 0;
        KisBackingClassChange first;
        if (!nextChange(&first)) return backingBudget->admitUnchangedBacking(error);
        const quint64 prepared = beginPreparedChange();
        auto &slot = *preparedChange(prepared);
        auto rollback = qScopeGuard([&] { finishPreparedChange(slot, prepared); });
        KisBackingBudgetDelta delta;
        if (!buildPhysicalBackingDelta(first, nextChange, maximum, &delta, slot, prepared, error)) return false;
        for (const auto &credit : slot.retirementCredits) subtractBackingDelta(&delta, credit.delta());
        if (hasBackingDelta(delta)) {
            slot.reservation = backingBudget->reserveChange(delta, error);
            if (!slot.reservation.isValid()) return false;
        } else if (!backingBudget->admitUnchangedBacking(error)) {
            return false;
        }
        *cookie = prepared;
        rollback.dismiss();
        KisPageStoreDetail::setError(error, {});
        return true;
    }

    quint64 beginPreparedChange()
    {
        quint32 slotIndex = 0;
        if (freePreparedChanges.empty()) {
            slotIndex = quint32(preparedChanges.size());
            if (freePreparedChanges.capacity() < preparedChanges.size() + 1) {
                freePreparedChanges.reserve(
                    std::max(preparedChanges.size() + 1,
                             freePreparedChanges.capacity()
                                 ? freePreparedChanges.capacity() * 2 : size_t(1)));
            }
            preparedChanges.emplace_back(backingBudget);
        } else {
            slotIndex = freePreparedChanges.back();
            freePreparedChanges.pop_back();
        }
        PreparedBackingChangeSlot &slot = preparedChanges.at(slotIndex);
        ++slot.generation;
        if (!slot.generation)
            ++slot.generation;
        const quint64 cookie = (quint64(slot.generation) << 32) | slotIndex;
        Q_ASSERT(slot.changes.empty() && slot.retirementCredits.empty());
        slot.occupied = true;
        return cookie;
    }


    void finishPreparedChange(PreparedBackingChangeSlot &slot, quint64 cookie) noexcept
    {
        for (const auto &change : std::as_const(slot.changes)) {
            // A refused projection can contain rows not yet physically located.
            if (change.physical && change.physical->classClaim == cookie)
                change.physical->classClaim = 0;
        }
        slot.reservation.release();
        slot.changes.clear();
        slot.retirementCredits.clear();
        slot.occupied = false;
        freePreparedChanges.push_back(quint32(cookie));
        physicalClaimsChanged.wakeAll();
    }

    void applyBackingChange(const PreparedBackingChange &change,
                            KisBackingBudgetDelta *signedDelta) noexcept
    {
        const auto found = change.backing;
        auto *physical = change.physical;
        Q_ASSERT(found != backings.end());
        const auto before = found->second.budgetClass;
        if (before == change.after) return;
        Q_ASSERT(physical);
        const KisBackingBudgetClass beforeCharge = physical->chargedClass;
        const bool changed = changePhysicalReferenceClass(
            physical, before, change.after);
        Q_ASSERT(changed);
        Q_UNUSED(changed);
        const KisBackingBudgetClass afterCharge = chargedBackingClass(*physical);
        const bool deltaAdded = addPhysicalChargeChange(
            signedDelta, *physical, beforeCharge, afterCharge);
        Q_ASSERT(deltaAdded);
        Q_UNUSED(deltaAdded);
        physical->chargedClass = afterCharge;
        found->second.budgetClass = change.after;
    }

    void moveRetirementHeadroom(const PhysicalBackingRecord &physical,
                                KisPageAccessDomain target) noexcept
    {
        // Handoff requires a sole physical owner and makes that owner the
        // representative. Aliases cannot create another headroom. Retirement
        // consumes/releases it before replacing the representative, so domain
        // migration needs one lookup, not an alias-membership scan.
        auto found = backings.find(physical.representative.allocationIdentity());
        Q_ASSERT(found != backings.end());
        auto &headroom = found->second.retirementHeadroom;
        if (headroom.isValid())
            backingBudget->moveRetirementHeadroom(headroom.cookie, physical.domain,
                                                   target, physical.byteSize());
    }

    Storage storage;
    mutable QMutex mutex;
    std::shared_ptr<KisCompletionRegistry> completions;
    quint64 nextValidationStamp = 1;
    KisPageMetadataCoordinator *cpuMetadata = nullptr;
    quint64 cpuProofIssuer = 0;
    ProviderIndex providers;
    quint64 providerRevision = 0;
    OperationIndex operations;
    ProofIndex sealedProofs;
    KisBackingBudgetController *backingBudget = nullptr;
    BackingIndex backings;
    PhysicalBackingIndex physicalBackings;
    KisPageWaitCondition physicalClaimsChanged;
    size_t activeHandoffs = 0;
    ChargedVector<PreparedBackingChangeSlot> preparedChanges;
    ChargedVector<quint32> freePreparedChanges;
    std::shared_ptr<KisPageOwnerDomainAdmission> domainAdmission;
};

class KisPageOwnerDomainAdmission final
    : public KisReplicaBackingDomainAdmission
    , public KisReplicaBackingDomainReservation
    , public std::enable_shared_from_this<KisPageOwnerDomainAdmission>
{
public:
    struct Registration {
        PhysicalBackingKey physical;
        Registration *next = nullptr;
    };
    explicit KisPageOwnerDomainAdmission(KisPageOwnerLedger *owner)
        : m_owner(owner) {}
    // A transition outside this ledger needs only the original authority
    // lifetime. Alias this paid root instead of allocating an empty claim.
    void commit(quint64) noexcept override {}

    class Reservation final : public KisReplicaBackingDomainReservation
    {
    public:
        Reservation(std::shared_ptr<KisPageOwnerDomainAdmission> owner,
                    quint64 cookie)
            : m_owner(std::move(owner)), m_cookie(cookie) {}
        ~Reservation() override
        {
            if (m_owner)
                m_owner->cancel(m_cookie);
        }
        void commit(quint64 targetRevision) noexcept override
        {
            if (!m_owner)
                return;
            m_owner->commit(m_cookie, targetRevision);
            m_owner.reset();
            m_cookie = 0;
        }

    private:
        std::shared_ptr<KisPageOwnerDomainAdmission> m_owner;
        quint64 m_cookie = 0;
        friend class KisPageOwnerDomainAdmission;
    };

    std::shared_ptr<KisReplicaBackingDomainReservation> prepare(
        const KisReplicaPhysicalSlotIdentity &physical,
        quint64 bytes,
        KisPageAccessDomain sourceDomain,
        quint64 sourceRevision,
        KisPageAccessDomain targetDomain,
        QString *error) override
    try
    {
        QMutexLocker authorityLocker(&m_mutex);
        if (!m_owner || !physical.provider.isValid()
            || !physical.providerEpoch.isValid() || !physical.slot
            || !bytes || !sourceRevision
            || sourceDomain == KisPageAccessDomain::Unknown
            || targetDomain == KisPageAccessDomain::Unknown
            || sourceDomain == targetDomain
            || bytes > quint64(std::numeric_limits<qint64>::max())) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("backing-domain transition request is invalid"));
            return {};
        }
        for (auto *registration = m_pendingRegistrations; registration; registration = registration->next) {
            if (registration->physical == physical) {
                KisPageStoreDetail::setError(
                    error, QStringLiteral("backing-domain registration is active"));
                return {};
            }
        }
        auto *owner = m_owner->d.data();
        QMutexLocker ownerLocker(&owner->mutex);
        auto record = owner->physicalBackings.find(physical);
        if (record == owner->physicalBackings.end()) {
            KisPageStoreDetail::setError(error, {});
            return shared_from_this();
        }
        if (!owner->backingBudget || record->second.byteSize() != bytes
            || record->second.domain != sourceDomain
            || record->second.domainRevision != sourceRevision
            || record->second.domainClaim || record->second.classClaim) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("backing-domain transition source is stale or busy"));
            return {};
        }

        KisBackingBudgetDelta signedDelta;
        if (!addBackingDeltaChecked(&signedDelta, record->second.chargedClass,
                                    sourceDomain, -qint64(bytes))
            || !addBackingDeltaChecked(&signedDelta, record->second.chargedClass,
                                       targetDomain, qint64(bytes))) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("backing-domain transition delta overflows"));
            return {};
        }
        auto reservation = owner->backingBudget->reserveChange(
            signedDelta, error);
        if (!reservation.isValid())
            return {};
        ++m_nextCookie;
        if (!m_nextCookie)
            ++m_nextCookie;
        const quint64 cookie = m_nextCookie;
        // Prepare both actual records before publishing the original claim.
        // An inactive Reservation cannot reenter this gate during rollback.
        auto terminal = std::allocate_shared<Reservation>(
            KisMutationStorageAllocator<Reservation>::retained(owner->backingBudget),
            std::shared_ptr<KisPageOwnerDomainAdmission>{}, 0);
        Claim claim;
        claim.physical = physical;
        claim.sourceDomain = sourceDomain;
        claim.sourceRevision = sourceRevision;
        claim.targetDomain = targetDomain;
        claim.bytes = bytes;
        claim.signedDelta = signedDelta;
        claim.reservation = std::move(reservation);
        m_claims.emplace(cookie, std::move(claim));
        record->second.domainClaim = cookie;
        terminal->m_owner = shared_from_this();
        terminal->m_cookie = cookie;
        KisPageStoreDetail::setError(error, {});
        return terminal;
    }
    catch (const std::bad_alloc &)
    {
        KisPageStoreDetail::setError(error, QStringLiteral("backing-domain claim preparation storage is unavailable"));
        return {};
    }

    void attachBackingBudget(KisBackingBudgetController &budget)
    {
        QMutexLocker locker(&m_mutex);
        Q_ASSERT(m_claims.empty() && !m_pendingRegistrations);
        m_claims = ClaimMap(KisMutationStorageAllocator<std::pair<const quint64, Claim>>(&budget));
    }

    void detach() noexcept
    {
        ClaimMap released(m_claims.get_allocator());
        QMutexLocker authorityLocker(&m_mutex);
        if (!m_owner)
            return;
        auto *owner = m_owner->d.data();
        QMutexLocker ownerLocker(&owner->mutex);
        for (const auto &[cookie, claim] : m_claims) {
            auto record = owner->physicalBackings.find(claim.physical);
            if (record != owner->physicalBackings.end()
                && record->second.domainClaim == cookie) {
                record->second.domainClaim = 0;
            }
        }
        released.swap(m_claims);
        m_pendingRegistrations = nullptr;
        owner->physicalClaimsChanged.wakeAll();
        m_owner = nullptr;
    }

    bool beginRegistration(Registration &registration)
    {
        QMutexLocker locker(&m_mutex);
        if (!m_owner) return false;
        for (auto *pending = m_pendingRegistrations; pending; pending = pending->next)
            if (pending->physical == registration.physical) return false;
        // The synchronous registering call owns this scoped marker until its
        // scope exits. No result or physical responsibility is copied here.
        registration.next = m_pendingRegistrations;
        m_pendingRegistrations = &registration;
        return true;
    }

    void endRegistration(Registration &registration) noexcept
    {
        QMutexLocker locker(&m_mutex);
        for (auto **pending = &m_pendingRegistrations; *pending; pending = &(*pending)->next) {
            if (*pending == &registration) {
                *pending = registration.next;
                break;
            }
        }
        registration.next = nullptr;
    }

private:
    struct Claim {
        PhysicalBackingKey physical;
        KisPageAccessDomain sourceDomain = KisPageAccessDomain::Unknown;
        quint64 sourceRevision = 0;
        KisPageAccessDomain targetDomain = KisPageAccessDomain::Unknown;
        quint64 bytes = 0;
        KisBackingBudgetDelta signedDelta;
        KisBackingBudgetReservation reservation;
    };
    using ClaimMap = std::map<quint64, Claim, std::less<quint64>,
        KisMutationStorageAllocator<std::pair<const quint64, Claim>>>;

    void cancel(quint64 cookie) noexcept
    {
        if (!cookie)
            return;
        ClaimMap::node_type released;
        QMutexLocker authorityLocker(&m_mutex);
        auto claim = m_claims.find(cookie);
        if (claim == m_claims.end())
            return;
        if (m_owner) {
            auto *owner = m_owner->d.data();
            QMutexLocker ownerLocker(&owner->mutex);
            auto record = owner->physicalBackings.find(claim->second.physical);
            if (record != owner->physicalBackings.end()
                && record->second.domainClaim == cookie) {
                record->second.domainClaim = 0;
            }
            released = m_claims.extract(claim);
            owner->physicalClaimsChanged.wakeAll();
        } else {
            released = m_claims.extract(claim);
        }
    }

    void commit(quint64 cookie, quint64 targetRevision) noexcept
    {
        if (!cookie)
            return;
        ClaimMap::node_type released;
        QMutexLocker authorityLocker(&m_mutex);
        auto claim = m_claims.find(cookie);
        if (claim == m_claims.end() || !m_owner)
            return;
        auto *owner = m_owner->d.data();
        QMutexLocker ownerLocker(&owner->mutex);
        auto record = owner->physicalBackings.find(claim->second.physical);
        const auto revisionOrder = record != owner->physicalBackings.end()
            ? kisCompareBackingRevision(targetRevision, record->second.domainRevision)
            : KisBackingRevisionOrder::Ambiguous;
        Q_ASSERT(record != owner->physicalBackings.end()
                 && record->second.domainClaim == cookie
                 && record->second.domain == claim->second.sourceDomain
                 && record->second.domainRevision == claim->second.sourceRevision
                 && revisionOrder == KisBackingRevisionOrder::Newer);
        if (record != owner->physicalBackings.end()
            && record->second.domainClaim == cookie
            && record->second.domain == claim->second.sourceDomain
            && record->second.domainRevision == claim->second.sourceRevision
            && revisionOrder == KisBackingRevisionOrder::Newer) {
            owner->backingBudget->commitReservation(
                std::move(claim->second.reservation), claim->second.signedDelta);
            owner->moveRetirementHeadroom(record->second, claim->second.targetDomain);
            record->second.domain = claim->second.targetDomain;
            record->second.domainRevision = targetRevision;
            record->second.domainClaim = 0;
        }
        released = m_claims.extract(claim);
        owner->physicalClaimsChanged.wakeAll();
    }

    QMutex m_mutex;
    KisPageOwnerLedger *m_owner = nullptr;
    quint64 m_nextCookie = 0;
    ClaimMap m_claims;
    Registration *m_pendingRegistrations = nullptr;
};

KisBackingHandoffReservation::~KisBackingHandoffReservation()
{
    release();
}

KisBackingHandoffReservation::KisBackingHandoffReservation(
    KisBackingHandoffReservation &&other) noexcept
    : m_owner(std::exchange(other.m_owner, nullptr))
    , m_source(other.m_source), m_physical(other.m_physical)
    , m_target(other.m_target), m_budget(std::move(other.m_budget))
{}

KisBackingHandoffReservation &KisBackingHandoffReservation::operator=(
    KisBackingHandoffReservation &&other) noexcept
{
    if (this == &other) return *this;
    release();
    m_owner = std::exchange(other.m_owner, nullptr);
    m_source = other.m_source;
    m_physical = other.m_physical;
    m_target = other.m_target;
    m_budget = std::move(other.m_budget);
    return *this;
}

void KisBackingHandoffReservation::release() noexcept
{
    if (!m_owner) return;
    auto *owner = std::exchange(m_owner, nullptr);
    QMutexLocker lock(&owner->d->mutex);
    Q_ASSERT(m_physical->classClaim == HandoffClaim && owner->d->activeHandoffs);
    m_physical->classClaim = 0;
    --owner->d->activeHandoffs;
    m_budget.release();
    owner->d->physicalClaimsChanged.wakeAll();
}

KisBackingClassChangeReservation::KisBackingClassChangeReservation(
    KisPageOwnerLedger *owner, quint64 cookie)
    : m_owner(owner), m_cookie(cookie)
{
}

KisBackingClassChangeReservation::~KisBackingClassChangeReservation()
{
    release();
}

KisBackingClassChangeReservation::KisBackingClassChangeReservation(
    KisBackingClassChangeReservation &&other) noexcept
    : m_owner(std::exchange(other.m_owner, nullptr))
    , m_cookie(std::exchange(other.m_cookie, 0))
{
}

KisBackingClassChangeReservation &
KisBackingClassChangeReservation::operator=(
    KisBackingClassChangeReservation &&other) noexcept
{
    if (this != &other) {
        release();
        m_owner = std::exchange(other.m_owner, nullptr);
        m_cookie = std::exchange(other.m_cookie, 0);
    }
    return *this;
}

bool KisBackingClassChangeReservation::isValid() const
{
    return m_owner;
}

void KisBackingClassChangeReservation::release() noexcept
{
    if (m_owner && m_cookie)
        m_owner->cancelPreparedBackingChanges(m_cookie);
    m_owner = nullptr;
    m_cookie = 0;
}

KisPageOwnerLedger::KisPageOwnerLedger()
    : KisPageOwnerLedger(KisMutationStorageAllocator<KisPageOwnerLedger>{})
{
}

KisPageOwnerLedger::KisPageOwnerLedger(const KisMutationStorageAllocator<KisPageOwnerLedger> &storage)
{
    Private::Storage allocator(storage);
    auto *raw = allocator.allocate(1);
    try { std::allocator_traits<Private::Storage>::construct(allocator, raw, allocator); }
    catch (...) { allocator.deallocate(raw, 1); throw; }
    d.reset(raw);
    d->domainAdmission = std::allocate_shared<KisPageOwnerDomainAdmission>(storage, this);
}

void KisPageOwnerLedger::PrivateReleaser::cleanup(Private *owner)
{
    if (!owner) return;
    auto storage = owner->storage;
    std::destroy_at(owner);
    storage.deallocate(owner, 1);
}

KisPageOwnerLedger::~KisPageOwnerLedger()
{
    d->domainAdmission->detach();
}

bool KisPageOwnerLedger::configure(
    const std::shared_ptr<KisCompletionRegistry> &completions,
    QString *error)
{
    if (!completions || !completions->isOperational()) {
        KisPageStoreDetail::setError(error, QStringLiteral("owner ledger completion registry is invalid"));
        return false;
    }
    QMutexLocker locker(&d->mutex);
    if (d->completions) {
        KisPageStoreDetail::setError(error, QStringLiteral("owner ledger is already configured"));
        return false;
    }
    d->completions = completions;
    KisPageStoreDetail::setError(error, {});
    return true;
}

void KisPageOwnerLedger::attachBackingBudget(KisBackingBudgetController &budget)
{
    d->domainAdmission->attachBackingBudget(budget); // Admission gate precedes the ledger gate.
    QMutexLocker locker(&d->mutex);
    Q_ASSERT(!d->backingBudget);
    Q_ASSERT(d->operations.empty());
    Q_ASSERT(d->backings.empty() && d->physicalBackings.empty());
    d->backings = BackingIndex(KisPageBackingIdentityLess{},
        KisMutationStorageAllocator<std::pair<const BackingKey, BackingRecord>>(&budget));
    d->physicalBackings = PhysicalBackingIndex(KisPageBackingIdentityLess{},
        KisMutationStorageAllocator<std::pair<const PhysicalBackingKey, PhysicalBackingRecord>>(&budget));
    d->operations = OperationIndex(std::less<quint64>{},
        KisMutationStorageAllocator<std::pair<const quint64, ProviderOperationRecord>>(&budget));
    d->preparedChanges = ChargedVector<PreparedBackingChangeSlot>(
        KisMutationStorageAllocator<PreparedBackingChangeSlot>(&budget));
    d->freePreparedChanges = ChargedVector<quint32>(KisMutationStorageAllocator<quint32>(&budget));
    d->backingBudget = &budget;
}

KisReplicaBackingFootprint KisPageOwnerLedger::observeBackingFootprint(
    const KisReplicaHandle &replica) const
{
    std::shared_ptr<KisPageReplicaProvider> replicaProvider;
    {
        QMutexLocker locker(&d->mutex);
        const auto registered = d->registeredProvider(providerKey(replica.provider, replica.providerEpoch));
        if (registered) replicaProvider = registered->provider;
    }
    return replicaProvider ? replicaProvider->backingFootprint(replica)
                           : KisReplicaBackingFootprint{};
}

bool KisPageOwnerLedger::registerBacking(const KisReplicaHandle &replica,
                                         KisBackingBudgetReservation &reservation,
                                         KisBackingBudgetClass budgetClass,
                                         QString *error,
                                         KisPageRetirementRecordPointer *preparedRetirement)
{
    if (!replica.isValid() || !reservation.isValid()
        || static_cast<size_t>(budgetClass) >= static_cast<size_t>(KisBackingBudgetClass::Count)
        || replica.layout.byteSize > quint64(std::numeric_limits<qint64>::max())) {
        KisPageStoreDetail::setError(error, QStringLiteral("replica exceeds its backing reservation"));
        return false;
    }
    KisPageRetirementRecordPointer localRetirement;
    if (!preparedRetirement) {
        // Direct registration/adoption leaves physical ownership with its
        // caller on refusal. Fresh production already prepared this before
        // entering the provider, and transfers that exact node below.
        try { localRetirement = kisPreparePageRetirementRecord(d->backingBudget); }
        catch (const std::bad_alloc &) {
            KisPageStoreDetail::setError(error, QStringLiteral("backing retirement record budget storage was refused"));
            return false;
        }
        preparedRetirement = &localRetirement;
    }
    try {
        if (*preparedRetirement && (*preparedRetirement)->storage.budget == d->backingBudget)
            (*preparedRetirement)->prepareBackingStorage();
    } catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing installation storage was refused"));
        return false;
    }
    if (!*preparedRetirement || (*preparedRetirement)->backingStorage.empty()
        || (*preparedRetirement)->physicalStorage.empty()
        || (*preparedRetirement)->storage.budget != d->backingBudget) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing installation storage is absent or foreign"));
        return false;
    }
    KisReplicaBackingFootprint footprint = observeBackingFootprint(replica);
    if (!footprint.isValid() || footprint.bytes != replica.layout.byteSize) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider physical backing footprint is invalid"));
        return false;
    }
    const PhysicalBackingKey registrationPhysical =
        physicalBackingKey(replica, footprint);
    KisPageOwnerDomainAdmission::Registration registration{registrationPhysical};
    if (!d->domainAdmission->beginRegistration(registration)) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("physical backing registration is busy"));
        return false;
    }
    const auto finishRegistration = qScopeGuard([&] {
        d->domainAdmission->endRegistration(registration);
    });
    footprint = observeBackingFootprint(replica);
    if (!footprint.isValid() || footprint.bytes != replica.layout.byteSize
        || !(physicalBackingKey(replica, footprint) == registrationPhysical)) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("provider physical backing changed during registration"));
        return false;
    }

    PhysicalBackingIndex::node_type unusedPhysical;
    QMutexLocker locker(&d->mutex);
    const BackingKey key = replica.allocationIdentity();
    const PhysicalBackingKey physicalKey = physicalBackingKey(replica, footprint);
    if (!d->backingBudget || reservation.owner != d->backingBudget
        || d->backings.find(key) != d->backings.end()
        || d->isHandoffTarget(key)) {
        KisPageStoreDetail::setError(error, QStringLiteral("replica backing identity is already owned"));
        return false;
    }
    // Attaching an empty ledger must preserve cold budget reconfiguration.
    // Bind its empty indexes to the already prepared node allocator here;
    // constructing empty maps admits no storage and publishes no identity.
    if (d->backings.empty()) {
        Q_ASSERT(d->physicalBackings.empty());
        d->backings = BackingIndex(KisPageBackingIdentityLess{},
            (*preparedRetirement)->backingStorage.get_allocator());
        d->physicalBackings = PhysicalBackingIndex(KisPageBackingIdentityLess{},
            (*preparedRetirement)->physicalStorage.get_allocator());
    }
    if ((*preparedRetirement)->backingStorage.get_allocator() != d->backings.get_allocator()
        || (*preparedRetirement)->physicalStorage.get_allocator() != d->physicalBackings.get_allocator()) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing installation storage is foreign"));
        return false;
    }
    auto physical = d->physicalBackings.find(physicalKey);
    const bool newPhysicalBacking = physical == d->physicalBackings.end();
    KisBackingBudgetDelta sharedChargeChange;
    KisBackingBudgetClass sharedAfterCharge = KisBackingBudgetClass::Count;
    if (newPhysicalBacking) {
        if (!d->backingBudget->reservationCovers(reservation.cookie,
                                                 budgetClass, footprint.domain,
                                                 footprint.bytes)) {
            KisPageStoreDetail::setError(error, QStringLiteral("physical backing domain is not reserved"));
            return false;
        }
        PhysicalBackingRecord record;
        record.representative = replica;
        record.domain = footprint.domain;
        record.domainRevision = footprint.revision;
        record.references[static_cast<size_t>(budgetClass)] = 1;
        record.chargedClass = budgetClass;
        auto &node = (*preparedRetirement)->physicalStorage;
        node.key() = physicalKey;
        node.mapped() = record;
        auto installed = d->physicalBackings.insert(std::move(node));
        Q_ASSERT(installed.inserted);
        physical = installed.position;
    } else {
        if (physical->second.domainClaim || physical->second.classClaim) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("shared physical backing transition is active"));
            return false;
        }
        if (physical->second.domain != footprint.domain
            || physical->second.domainRevision != footprint.revision
            || physical->second.byteSize() != footprint.bytes) {
            KisPageStoreDetail::setError(error, QStringLiteral("shared physical backing footprint changed"));
            return false;
        }
        PhysicalBackingRecord next = physical->second;
        auto &references = next.references[static_cast<size_t>(budgetClass)];
        if (references == std::numeric_limits<quint32>::max()) {
            KisPageStoreDetail::setError(error, QStringLiteral("shared physical backing reference count overflow"));
            return false;
        }
        ++references;
        sharedAfterCharge = chargedBackingClass(next);
        if (!addPhysicalChargeChange(&sharedChargeChange, next,
                                     physical->second.chargedClass, sharedAfterCharge)
            || (sharedAfterCharge != physical->second.chargedClass
                && !d->backingBudget->reservationCovers(
                    reservation.cookie, sharedAfterCharge, footprint.domain,
                    footprint.bytes))) {
            KisPageStoreDetail::setError(error, QStringLiteral("shared physical backing class change is not reserved"));
            return false;
        }
        physical->second = next;
        unusedPhysical = std::move((*preparedRetirement)->physicalStorage);
    }
    auto &node = (*preparedRetirement)->backingStorage;
    node.key() = key;
    node.mapped().replica = replica;
    node.mapped().budgetClass = budgetClass;
    node.mapped().physical = physicalKey;
    const auto installedBacking = d->backings.insert(std::move(node));
    Q_ASSERT(installedBacking.inserted);
    auto &backing = installedBacking.position->second;
    physical->second.link(backing);
    backing.retirement = std::move(*preparedRetirement);

    if (!newPhysicalBacking) {
        // The same bytes have one charge, but a new foreground alias can
        // outrank the former owner class and must move that charge atomically.
        if (sharedAfterCharge != physical->second.chargedClass) {
            reservation.commit(sharedChargeChange);
            physical->second.chargedClass = sharedAfterCharge;
        } else {
            reservation.release();
        }
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    KisBackingBudgetDelta installed;
    addBackingDelta(&installed, budgetClass, footprint.domain, qint64(footprint.bytes));
    if (budgetClass != KisBackingBudgetClass::RetirementDebt
        && d->backingBudget->reservationCovers(
            reservation.cookie, KisBackingBudgetClass::RetirementDebt,
            footprint.domain, footprint.bytes)) {
        KisBackingBudgetDelta retirementFallback;
        addBackingDelta(&retirementFallback, KisBackingBudgetClass::RetirementDebt,
                        footprint.domain, qint64(footprint.bytes));
        reservation.commitRetaining(installed, retirementFallback);
    } else {
        reservation.commit(installed);
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisBackingBudgetClass KisPageOwnerLedger::backingClass(
    const KisReplicaHandle &replica) const
{
    QMutexLocker locker(&d->mutex);
    if (!replica.isValid()) return KisBackingBudgetClass::Count;
    const auto found = d->backings.find(replica.allocationIdentity());
    return found == d->backings.end() || !(found->second.replica == replica)
        ? KisBackingBudgetClass::Count : found->second.budgetClass;
}

KisPageRetirementRecordPointer KisPageOwnerLedger::takeRetirementRecord(const KisReplicaHandle &replica)
{
    QMutexLocker lock(&d->mutex);
    const auto found = d->backings.find(replica.allocationIdentity());
    if (found == d->backings.end() || !(found->second.replica == replica)) return {};
    auto record = std::move(found->second.retirement);
    if (record) {
        const auto registered = d->registeredProvider(providerKey(replica.provider, replica.providerEpoch));
        Q_ASSERT(registered);
        record->replica = replica;
        record->provider = registered ? registered->provider : std::shared_ptr<KisPageReplicaProvider>{};
        record->backgroundRetirement = registered && registered->backgroundRetirement;
    }
    return record;
}

KisPageRetirementRecords KisPageOwnerLedger::takeShutdownRetirementRecords()
{
    QMutexLocker lock(&d->mutex);
    KisPageRetirementRecords records;
    for (auto &entry : d->backings) {
        auto &backing = entry.second;
        auto record = std::move(backing.retirement);
        if (!record) continue; // The original queue already owns this record.
        const auto registered = d->registeredProvider(
            providerKey(backing.replica.provider, backing.replica.providerEpoch));
        Q_ASSERT(registered);
        record->replica = backing.replica;
        record->provider = registered ? registered->provider : std::shared_ptr<KisPageReplicaProvider>{};
        record->backgroundRetirement = registered && registered->backgroundRetirement;
        records.push_back(*record.release());
    }
    return records;
}

KisPageRetirementRecord *KisPageOwnerLedger::idleBackingRecord(const KisReplicaHandle &replica)
{
    QMutexLocker lock(&d->mutex);
    const auto found = d->backings.find(replica.allocationIdentity());
    if (found == d->backings.end() || !(found->second.replica == replica)
        || found->second.budgetClass != KisBackingBudgetClass::IdleCapacity
        || !found->second.retirement) return nullptr;
    auto *record = found->second.retirement.get();
    Q_ASSERT(!record->is_linked());
    record->replica = replica;
    return record;
}

bool KisPageOwnerLedger::reclassifyBacking(const KisReplicaHandle &replica,
                                           KisBackingBudgetClass budgetClass,
                                           QString *error)
{
    return reclassifyBackingImpl(replica, budgetClass, nullptr, error);
}

bool KisPageOwnerLedger::reclassifyBacking(
    const KisReplicaHandle &replica,
    KisBackingBudgetClass budgetClass,
    KisBackingBudgetReservation &reservation,
    QString *error)
{
    return reclassifyBackingImpl(replica, budgetClass, &reservation, error);
}

bool KisPageOwnerLedger::reclassifyBackingImpl(
    const KisReplicaHandle &replica,
    KisBackingBudgetClass budgetClass,
    KisBackingBudgetReservation *reservation,
    QString *error)
{
    QMutexLocker locker(&d->mutex);
    const BackingKey key = replica.allocationIdentity();
    auto found = d->backings.find(key);
    if (found == d->backings.end() || !(found->second.replica == replica) || !d->backingBudget
        || (reservation && reservation->owner != d->backingBudget)
        || d->isPreparedBacking(found->second)) {
        KisPageStoreDetail::setError(error, reservation
            ? QStringLiteral("replica backing is not available for reserved reclassification")
            : QStringLiteral("replica backing is not owned by this store"));
        return false;
    }
    if (found->second.budgetClass == budgetClass) {
        if (reservation) reservation->release();
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    auto physical = d->physicalBackings.find(found->second.physical);
    if (physical == d->physicalBackings.end()) {
        KisPageStoreDetail::setError(error, QStringLiteral("replica physical backing is missing"));
        return false;
    }
    if (physical->second.domainClaim || physical->second.classClaim) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("replica physical backing transition is active"));
        return false;
    }
    PhysicalBackingRecord next = physical->second;
    const KisBackingBudgetClass beforeCharge = physical->second.chargedClass;
    if (!changePhysicalReferenceClass(&next, found->second.budgetClass, budgetClass)) {
        KisPageStoreDetail::setError(error, QStringLiteral("replica physical class references are invalid"));
        return false;
    }
    const KisBackingBudgetClass afterCharge = chargedBackingClass(next);
    KisBackingBudgetDelta installed;
    if (!addPhysicalChargeChange(&installed, next, beforeCharge, afterCharge)) {
        KisPageStoreDetail::setError(error, QStringLiteral("replica physical class delta is invalid"));
        return false;
    }
    const KisBackingBudgetDelta needed = positiveBackingDelta(installed);
    KisBackingBudgetReservation automaticReservation;
    if (!reservation && afterCharge == KisBackingBudgetClass::RetirementDebt
        && found->second.retirementHeadroom.isValid())
        reservation = &found->second.retirementHeadroom;
    if (!reservation) {
        if (beforeCharge == afterCharge) {
            if (!d->backingBudget->admitUnchangedBacking(error)) return false;
        } else {
            automaticReservation = d->backingBudget->reserveChange(installed, error);
            if (!automaticReservation.isValid()) return false;
            reservation = &automaticReservation;
        }
    } else if (!d->backingBudget->reservationCovers(reservation->cookie, needed)) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing reservation does not cover the physical class change"));
        return false;
    }
    if (reservation) reservation->commit(installed);
    physical->second = next;
    physical->second.chargedClass = afterCharge;
    found->second.budgetClass = budgetClass;
    if (budgetClass != KisBackingBudgetClass::ActivePending)
        found->second.retirementHeadroom.release();
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPageReadinessStatus KisPageOwnerLedger::watchRetirementBudget(
    const KisReplicaHandle &replica, KisPageReadinessCallback notify,
    KisBackingBudgetWaiter *waiter)
{
    QMutexLocker lock(&d->mutex);
    const auto key = replica.allocationIdentity();
    const auto found = d->backings.find(key);
    if (!d->backingBudget || found == d->backings.end() ||
        !(found->second.replica == replica) || d->isPreparedBacking(found->second) ||
        found->second.retirementHeadroom.isValid()) return KisPageReadinessStatus::Unavailable;
    const auto physical = d->physicalBackings.find(found->second.physical);
    if (physical == d->physicalBackings.end() || physical->second.domainClaim || physical->second.classClaim)
        return KisPageReadinessStatus::Unavailable;
    auto next = physical->second;
    if (!changePhysicalReferenceClass(&next, found->second.budgetClass,
                                       KisBackingBudgetClass::RetirementDebt))
        return KisPageReadinessStatus::Unavailable;
    KisBackingBudgetDelta change;
    if (!addPhysicalChargeChange(&change, next, physical->second.chargedClass, chargedBackingClass(next)))
        return KisPageReadinessStatus::Unavailable;
    auto *budget = d->backingBudget;
    lock.unlock(); // Cold waiter/capture admission never holds the backing gate.
    return budget->waitForChange(change, std::move(notify), waiter);
}

bool KisPageOwnerLedger::retainRetirementDebtReservation(
    const KisReplicaHandle &replica,
    KisBackingBudgetReservation &reservation,
    QString *error)
try
{
    const KisReplicaBackingFootprint footprint = observeBackingFootprint(replica);
    QMutexLocker locker(&d->mutex);
    if (!replica.isValid() || !footprint.isValid() || !d->backingBudget
        || reservation.owner != d->backingBudget
        || !d->backingBudget->reservationCovers(
            reservation.cookie, KisBackingBudgetClass::RetirementDebt,
            footprint.domain, footprint.bytes)) {
        KisPageStoreDetail::setError(error, QStringLiteral("orphan reservation does not cover retirement debt"));
        return false;
    }
    if (d->physicalBackings.count(physicalBackingKey(replica, footprint))) {
        reservation.release();
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    KisBackingBudgetDelta retained;
    addBackingDelta(&retained, KisBackingBudgetClass::RetirementDebt,
                    footprint.domain, qint64(footprint.bytes));
    if (!reservation.retainOnly(retained)) {
        KisPageStoreDetail::setError(error, QStringLiteral("orphan retirement reservation is stale"));
        return false;
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}
catch (const std::bad_alloc &) {
    // Observation precedes any reservation change. Both first handoff and
    // queued retries keep the original paid record/debt on refusal.
    KisPageStoreDetail::setError(error, QStringLiteral("orphan backing observation was refused"));
    return false;
}

bool KisPageOwnerLedger::synchronizeBackingDomains(QString *error)
try
{
    struct Probe {
        std::shared_ptr<KisPageReplicaProvider> provider;
        KisReplicaBackingDomainChange change;
        bool acknowledged = false;

        PhysicalBackingKey key() const
        {
            return {provider->providerId(), provider->providerEpoch(),
                    change.physicalSlot};
        }
    };
    // Accepted records are never removed while this ledger lives. A scalar
    // cut excludes pending/later registrations; each iterator exists only
    // under the gate and provider calls retain their own pointer outside it.
    quint64 cut = 0;
    KisBackingBudgetController *budget = nullptr;
    {
        QMutexLocker locker(&d->mutex);
        budget = d->backingBudget;
        if (!budget) {
            KisPageStoreDetail::setError(error, QStringLiteral("backing budget is not attached"));
            return false;
        }
        cut = d->providerRevision;
    }
    ChargedVector<Probe> probes{KisMutationStorageAllocator<Probe>(budget)};

    ProviderKey cursor{};
    while (const auto provider = d->nextProviderInCut(cut, &cursor)) {
        if (!provider->mayHaveBackingDomainChanges()) continue;
        const auto changes = provider->backingDomainChanges(budget);
        probes.reserve(probes.size() + changes.size());
        for (const auto &change : changes)
            probes.push_back({provider, change});
    }

    QMutexLocker locker(&d->mutex);
    if (!d->backingBudget) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing budget is not attached"));
        return false;
    }
    for (Probe &probe : probes) {
        if (!probe.change.isValid()) {
            KisPageStoreDetail::setError(error, QStringLiteral("provider backing-domain change is invalid"));
            return false;
        }
        const PhysicalBackingKey key = probe.key();
        const auto physical = d->physicalBackings.find(key);
        if (physical == d->physicalBackings.cend()) {
            // Allocation and ledger registration are separate operations. Do
            // not consume an observation before this owner has the backing.
            continue;
        }
        if (probe.change.bytes != physical->second.byteSize()) {
            KisPageStoreDetail::setError(error, QStringLiteral("provider physical backing observation is invalid"));
            return false;
        }
        const auto order = kisCompareBackingRevision(
            probe.change.revision, physical->second.domainRevision);
        if (order == KisBackingRevisionOrder::Ambiguous) {
            KisPageStoreDetail::setError(error, QStringLiteral("provider backing-domain revision is ambiguous"));
            return false;
        }
        if (order == KisBackingRevisionOrder::Older) {
            probe.acknowledged = true;
            continue;
        }
        if (order == KisBackingRevisionOrder::Same) {
            if (probe.change.domain != physical->second.domain) {
                KisPageStoreDetail::setError(error, QStringLiteral("provider reused a backing-domain revision"));
                return false;
            }
            probe.acknowledged = true;
            continue;
        }
        // Mutable providers must install domain/revision through the
        // pre-admission token before publishing this journal observation.
        // A newer observation here proves that an owner was skipped.
        KisPageStoreDetail::setError(
            error, QStringLiteral("provider published an unadmitted backing-domain change"));
        return false;
    }
    locker.unlock();
    for (const Probe &probe : probes) {
        if (!probe.acknowledged) continue;
        probe.provider->acknowledgeBackingDomainChange(
            probe.change.physicalSlot, probe.change.revision);
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}
catch (const std::bad_alloc &)
{
    KisPageStoreDetail::setError(error, QStringLiteral("backing-domain synchronization storage is unavailable"));
    return false; // No acknowledgement before the whole prepared cut validates.
}

KisBackingClassChangeReservation KisPageOwnerLedger::prepareBackingChanges(
    KisPageSnapshotArray<KisBackingClassChange> changes,
    const KisPageSnapshotArray<KisPageTransitionEffect> &retirementEffects,
    QString *error)
try {
    KisBackingBudgetController *budget;
    {
        QMutexLocker lock(&d->mutex);
        budget = d->backingBudget;
    }
    if (!budget) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing budget is not attached"));
        return {};
    }
    return prepareBackingChangesImpl(BackingChanges(changes.begin(), changes.end(),
        KisMutationStorageAllocator<KisBackingClassChange>(budget)),
        retirementEffects.constData(), retirementEffects.size(), error);
} catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("backing change storage budget is exhausted"));
    return {};
}

KisBackingClassChangeReservation KisPageOwnerLedger::prepareBackingChanges(
    BackingChanges changes, const KisPageTransitionEffect *effects, qsizetype count, QString *error)
{
    return prepareBackingChangesImpl(std::move(changes), effects, count, error);
}

KisBackingClassChangeReservation KisPageOwnerLedger::prepareIdleCapacity(
    const KisPagePublicationChange *rows, size_t count, quint64 allowance, QString *error)
try {
    if (count && !rows) {
        KisPageStoreDetail::setError(error, QStringLiteral("capacity release range is invalid"));
        return {};
    }
    if (!synchronizeBackingDomains(error)) return {};
    QMutexLocker locker(&d->mutex);
    if (!d->backingBudget) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing budget is not attached"));
        return {};
    }
    auto select = [rows, count, index = size_t(0), allowance]() mutable -> const KisReplicaHandle * {
        while (index < count) {
            const auto &row = rows[index++];
            if (!row.releasesCapacity || row.supersededAuthority.layout.byteSize > allowance) continue;
            allowance -= row.supersededAuthority.layout.byteSize;
            return &row.supersededAuthority;
        }
        return nullptr;
    };
    // Size only the selected original rows, including partial allowances.
    // The second walk replaces the old carrier-to-ledger projection walk.
    auto counter = select;
    size_t selected = 0;
    while (counter()) ++selected;
    auto next = [select](KisBackingClassChange *change) mutable {
        const auto *replica = select();
        if (!replica) return false;
        *change = {*replica, KisBackingBudgetClass::ActivePending, KisBackingBudgetClass::IdleCapacity};
        return true;
    };
    quint64 cookie = 0;
    if (!d->prepareClassChanges(selected, next, &cookie, error)) return {};
    return {this, cookie};
} catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("backing change storage budget is exhausted"));
    return {};
}

KisBackingClassChangeReservation KisPageOwnerLedger::prepareBackingChangesImpl(
    BackingChanges changes,
    const KisPageTransitionEffect *retirementEffects, qsizetype count, QString *error)
try {
    if (count < 0 || (count && !retirementEffects)) {
        KisPageStoreDetail::setError(error, QStringLiteral("retirement effect range is invalid"));
        return {};
    }
    if (!synchronizeBackingDomains(error)) return {};
    QMutexLocker locker(&d->mutex);
    if (!d->backingBudget) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing budget is not attached"));
        return {};
    }

    if (!changes.get_allocator().budget && changes.empty())
        changes = BackingChanges(KisMutationStorageAllocator<KisBackingClassChange>(d->backingBudget));
    if (changes.get_allocator().budget != d->backingBudget) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing change storage belongs to another budget"));
        return {};
    }
    bool needsRetirementChange = false;
    for (qsizetype i = 0; i < count; ++i) {
        const auto &effect = retirementEffects[i];
        if (!effect.replica.isValid()) {
            KisPageStoreDetail::setError(error, QStringLiteral("publication retirement effect is invalid"));
            return {};
        }
        const BackingKey key = effect.replica.allocationIdentity();
        const auto found = d->backings.find(key);
        if (found == d->backings.end() || !(found->second.replica == effect.replica)
            || !found->second.retirement) {
            KisPageStoreDetail::setError(error, QStringLiteral("retirement effect has no registered backing and original record"));
            return {};
        }
        needsRetirementChange |= found->second.budgetClass != KisBackingBudgetClass::RetirementDebt;
    }
    if (count && (!changes.empty() || needsRetirementChange)) {
        // Merge initial targets and repeated retirement effects in one paid
        // index array. Numeric indices survive growth of the original changes
        // carrier, and final append order stays that of the caller's effects.
        const size_t initial = changes.size();
        ChargedVector<size_t> targets{KisMutationStorageAllocator<size_t>(d->backingBudget)};
        if (size_t(count) > targets.max_size() - initial) throw std::bad_alloc();
        targets.reserve(initial + size_t(count));
        for (size_t i = 0; i < initial + size_t(count); ++i) targets.push_back(i);
        const auto keyAt = [&](size_t i) {
            return i < initial ? changes[i].replica.allocationIdentity()
                               : retirementEffects[i - initial].replica.allocationIdentity();
        };
        const KisPageBackingIdentityLess less;
        std::sort(targets.begin(), targets.end(), [&](size_t a, size_t b) {
            const auto left = keyAt(a), right = keyAt(b);
            if (less(left, right)) return true;
            if (less(right, left)) return false;
            return a < b;
        });
        size_t selected = 0;
        for (size_t first = 0; first < targets.size();) {
            const auto key = keyAt(targets[first]);
            size_t last = first + 1;
            while (last < targets.size() && keyAt(targets[last]) == key) ++last;
            size_t effect = first;
            while (effect < last && targets[effect] < initial) ++effect;
            if (effect < last) {
                if (targets[first] < initial) {
                    if (changes[targets[first]].after != KisBackingBudgetClass::RetirementDebt) {
                        KisPageStoreDetail::setError(error, QStringLiteral("retired publication backing has a competing target class"));
                        return {};
                    }
                } else if (d->backings.find(key)->second.budgetClass != KisBackingBudgetClass::RetirementDebt) {
                    targets[selected++] = targets[effect];
                }
            }
            first = last;
        }
        targets.resize(selected);
        std::sort(targets.begin(), targets.end());
        for (const size_t index : targets) {
            const auto &replica = retirementEffects[index - initial].replica;
            changes.push_back({replica, d->backings.find(replica.allocationIdentity())->second.budgetClass,
                              KisBackingBudgetClass::RetirementDebt});
        }
    }

    auto next = [position = changes.cbegin(), end = changes.cend()](KisBackingClassChange *change) mutable {
        if (position == end) return false;
        *change = *position++;
        return true;
    };
    quint64 cookie = 0;
    if (!d->prepareClassChanges(changes.size(), next, &cookie, error)) return {};
    return {this, cookie};
} catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("backing change storage budget is exhausted"));
    return {};
}

KisBackingHandoffReservation KisPageOwnerLedger::prepareBackingHandoff(
    const KisReplicaHandle &source, const KisReplicaHandle &target, QString *error)
{
    if (!source.isValid() || !target.isValid()
        || source.domain != KisPageAccessDomain::CpuRam || target.domain != source.domain
        || !(source.provider == target.provider) || !(source.providerEpoch == target.providerEpoch)
        || source.allocation.slot != target.allocation.slot
        || source.allocation.generation == std::numeric_limits<quint64>::max()
        || target.allocation.generation != source.allocation.generation + 1
        || !(source.version.key == target.version.key)
        || target.version.generation.value <= source.version.generation.value
        || !(source.layout == target.layout)) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing handoff identity is invalid"));
        return {};
    }
    if (!synchronizeBackingDomains(error)) return {};
    QMutexLocker locker(&d->mutex);
    const auto oldKey = source.allocationIdentity();
    const auto newKey = target.allocationIdentity();
    const auto backing = d->backings.find(oldKey);
    if (!d->backingBudget || backing == d->backings.end()
        || !(backing->second.replica == source)
        || (backing->second.budgetClass != KisBackingBudgetClass::Current
            && backing->second.budgetClass != KisBackingBudgetClass::ActivePending
            && backing->second.budgetClass != KisBackingBudgetClass::OptionalCache
            && backing->second.budgetClass != KisBackingBudgetClass::IdleCapacity)
        || d->backings.find(newKey) != d->backings.end()) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing handoff source is not available"));
        return {};
    }
    const auto physical = d->physicalBackings.find(backing->second.physical);
    if (physical == d->physicalBackings.cend() || physical->second.domainClaim || physical->second.classClaim
        || physical->second.domain != source.domain || !(physical->second.representative == source)
        || physical->second.shared()) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing handoff physical owner is shared or busy"));
        return {};
    }
    for (size_t i = 0; i < physical->second.references.size(); ++i) {
        if (physical->second.references[i] != (i == size_t(backing->second.budgetClass) ? 1u : 0u)) {
            KisPageStoreDetail::setError(error, QStringLiteral("backing handoff requires one physical owner"));
            return {};
        }
    }
    const bool noFee = backing->second.budgetClass == KisBackingBudgetClass::ActivePending
        && backing->second.retirementHeadroom.isValid();
    KisBackingBudgetReservation reservation;
    if (noFee) {
        if (!d->backingBudget->admitUnchangedBacking(error)) return {};
    } else {
        KisBackingBudgetDelta change;
        if (!addPhysicalChargeChange(&change, physical->second, physical->second.chargedClass,
                                     KisBackingBudgetClass::ActivePending)
            || (!backing->second.retirementHeadroom.isValid()
                && backing->second.budgetClass != KisBackingBudgetClass::OptionalCache
                && backing->second.budgetClass != KisBackingBudgetClass::IdleCapacity
                && !addBackingDeltaChecked(&change, KisBackingBudgetClass::RetirementDebt,
                                           physical->second.domain, qint64(physical->second.byteSize())))) {
            KisPageStoreDetail::setError(error, QStringLiteral("backing handoff delta overflows"));
            return {};
        }
        reservation = d->backingBudget->reserveChange(change, error);
        if (!reservation.isValid()) return {};
    }
    KisBackingHandoffReservation result;
    result.m_source = &backing->second;
    result.m_physical = &physical->second;
    result.m_target = target;
    result.m_budget = std::move(reservation);
    physical->second.classClaim = HandoffClaim;
    ++d->activeHandoffs;
    result.m_owner = this;
    KisPageStoreDetail::setError(error, {});
    return result;
}

bool KisPageOwnerLedger::commitBackingHandoff(
    KisBackingHandoffReservation &&reservation) noexcept
{
    QMutexLocker locker(&d->mutex);
    if (reservation.m_owner != this) return false;
    auto *source = reservation.m_source;
    auto *physical = reservation.m_physical;
    const auto found = d->backings.find(source->replica.allocationIdentity());
    Q_ASSERT(found != d->backings.end() && &found->second == source
             && physical->classClaim == HandoffClaim);
    const auto before = source->budgetClass;
    const PreparedBackingChange change{found, physical, 0, KisBackingBudgetClass::ActivePending};
    const auto &target = reservation.m_target;
    KisBackingBudgetDelta installed;
    d->applyBackingChange(change, &installed);

    // Rekey the same stable node: its physical membership and retirement
    // storage stay attached, with no allocation or new identity placeholder.
    const auto successor = std::next(found);
    auto node = d->backings.extract(found);
    Q_ASSERT(!node.empty());
    node.key() = target.allocationIdentity();
    node.mapped().replica = target;
    const auto inserted = d->backings.insert(successor, std::move(node));
    Q_ASSERT(node.empty() && physical->classClaim == HandoffClaim);
    physical->representative = target;

    // Detached optional capacity has no visible value to recover. A failed
    // private write returns to the original funded retirement endpoint.
    if (inserted->second.retirementHeadroom.isValid()
        || before == KisBackingBudgetClass::OptionalCache
        || before == KisBackingBudgetClass::IdleCapacity) {
        if (reservation.m_budget.isValid()) reservation.m_budget.commit(installed);
    } else {
        KisBackingBudgetDelta retained;
        addBackingDelta(&retained, KisBackingBudgetClass::RetirementDebt,
                         physical->domain, qint64(physical->byteSize()));
        reservation.m_budget.commitRetaining(installed, retained);
        inserted->second.retirementHeadroom = std::move(reservation.m_budget);
    }
    physical->classClaim = 0;
    --d->activeHandoffs;
    reservation.m_owner = nullptr;
    d->physicalClaimsChanged.wakeAll();
    return true;
}

bool KisPageOwnerLedger::prepareRetirementDebt(
    const KisPageTransitionEffect *effects, qsizetype count,
    quint64 *cookie,
    QString *error)
{
    if (!cookie) {
        KisPageStoreDetail::setError(error, QStringLiteral("retirement debt cookie output is missing"));
        return false;
    }
    *cookie = 0;
    auto reservation = prepareBackingChangesImpl({}, effects, count, error);
    if (!reservation.isValid())
        return false;
    *cookie = std::exchange(reservation.m_cookie, 0);
    reservation.m_owner = nullptr;
    KisPageStoreDetail::setError(error, {});
    return true;
}

void KisPageOwnerLedger::commitRetirementDebt(quint64 cookie) noexcept
{
    commitPreparedBackingChanges(cookie);
}

void KisPageOwnerLedger::cancelRetirementDebt(quint64 cookie) noexcept
{
    cancelPreparedBackingChanges(cookie);
}

void KisPageOwnerLedger::commitPreparedBackingChanges(quint64 cookie) noexcept
{
    if (!cookie) return; // Admitted empty set has no ledger/budget entity.
    QMutexLocker locker(&d->mutex);
    PreparedBackingChangeSlot *slot = d->preparedChange(cookie);
    if (!slot) return;

    KisBackingBudgetDelta signedDelta;
    for (const auto &change : std::as_const(slot->changes)) {
        Q_ASSERT(change.physical->classClaim == cookie);
        d->applyBackingChange(change, &signedDelta);
    }
    for (const auto &credit : slot->retirementCredits) {
        Q_ASSERT(credit.backing->retirementHeadroom.isValid());
        const auto funded = credit.delta();
        credit.backing->retirementHeadroom.commit(funded);
        subtractBackingDelta(&signedDelta, funded);
    }
    if (slot->reservation.isValid()) slot->reservation.commit(signedDelta);
    else Q_ASSERT(!hasBackingDelta(signedDelta));
    for (const auto &change : std::as_const(slot->changes)) {
        if (change.after != KisBackingBudgetClass::ActivePending)
            change.backing->second.retirementHeadroom.release();
    }
    d->finishPreparedChange(*slot, cookie);
}

void KisPageOwnerLedger::cancelPreparedBackingChanges(quint64 cookie) noexcept
{
    if (!cookie) return;
    QMutexLocker locker(&d->mutex);
    PreparedBackingChangeSlot *slot = d->preparedChange(cookie);
    if (!slot) return;
    d->finishPreparedChange(*slot, cookie);
}

void KisPageOwnerLedger::commitBackingChanges(
    KisBackingClassChangeReservation &&reservation) noexcept
{
    Q_ASSERT(reservation.m_owner == this);
    if (reservation.m_owner != this)
        return;
    const quint64 cookie = std::exchange(reservation.m_cookie, 0);
    reservation.m_owner = nullptr;
    commitPreparedBackingChanges(cookie);
}

void KisPageOwnerLedger::releaseRetiredBacking(const KisReplicaHandle &replica) noexcept
{
    BackingIndex::node_type released;
    PhysicalBackingIndex::node_type releasedPhysical;
    QMutexLocker locker(&d->mutex);
    const BackingKey key = replica.allocationIdentity();
    auto found = d->backings.find(key);
    Q_ASSERT(!d->isHandoffTarget(key)
             && (found == d->backings.end() || !d->isPreparedBacking(found->second)));
    if (found == d->backings.end() || !(found->second.replica == replica)) return;
    while (true) {
        const auto physical = d->physicalBackings.find(found->second.physical);
        if (physical == d->physicalBackings.cend()
            || (!physical->second.domainClaim && !physical->second.classClaim))
            break;
        d->physicalClaimsChanged.wait(&d->mutex);
        found = d->backings.find(key);
        if (found == d->backings.end() || !(found->second.replica == replica))
            return;
    }
    released = d->backings.extract(found);
    auto &record = released.mapped();
    auto physical = d->physicalBackings.find(record.physical);
    Q_ASSERT(physical != d->physicalBackings.end());
    if (physical == d->physicalBackings.end())
        return;
    physical->second.unlink(record);
    auto &references = physical->second.references[static_cast<size_t>(record.budgetClass)];
    Q_ASSERT(references != 0);
    if (references)
        --references;
    const KisBackingBudgetClass afterCharge = chargedBackingClass(physical->second);
    if (afterCharge == KisBackingBudgetClass::Count) {
        Q_ASSERT(!physical->second.members);
        if (d->backingBudget) {
            d->backingBudget->releaseLive(physical->second.chargedClass,
                                          physical->second.domain,
                                          physical->second.byteSize());
        }
        releasedPhysical = d->physicalBackings.extract(physical);
        return;
    }
    Q_ASSERT(physical->second.members);
    if (physical->second.representative == record.replica)
        physical->second.representative = physical->second.members->replica;
    // Retirement admission already exposed and budgeted every surviving
    // owner class before provider destruction. Removing a terminal Debt alias
    // therefore cannot reveal an unprepared class here.
    Q_ASSERT(afterCharge == physical->second.chargedClass);
}

bool KisPageOwnerLedger::registerProvider(
    const std::shared_ptr<KisPageReplicaProvider> &provider,
    QString *error)
try
{
    const auto capabilities = provider ? provider->capabilities() : KisReplicaCapabilities{};
    if (!provider || !provider->providerId().isValid() ||
        !provider->providerEpoch().isValid() || !capabilities.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("replica provider identity or capabilities are invalid"));
        return false;
    }
    const KisReplicaProviderId id = provider->providerId();
    const KisReplicaProviderEpoch epoch = provider->providerEpoch();
    const ProviderKey key = providerKey(id, epoch);
    LedgerNode<ProviderRegistration> candidate;
    {
        QMutexLocker locker(&d->mutex);
        if (!d->completions) {
            KisPageStoreDetail::setError(error, QStringLiteral("owner ledger is not configured"));
            return false;
        }
        if (d->providers.find(key, ProviderLess{}) != d->providers.end()) {
            KisPageStoreDetail::setError(error, QStringLiteral("replica provider identity is already registered"));
            return false;
        }
        // One active epoch per stable provider ID. Re-registration after
        // restart requires a new ledger so stale handles fail closed.
        for (const auto &registered : std::as_const(d->providers)) {
            if (registered.key.first == id.value) {
                KisPageStoreDetail::setError(
                    error, registered.acceptedRevision
                        ? QStringLiteral("replica provider ID already has an active epoch")
                        : QStringLiteral("replica provider ID registration is already active"));
                return false;
            }
        }
        // Size includes pending records, reserving acceptance revisions for
        // every in-flight call before any external admission can succeed.
        if (d->providers.size() >= std::numeric_limits<quint64>::max() - 1) {
            KisPageStoreDetail::setError(error, QStringLiteral("provider registration identity is exhausted"));
            return false;
        }
        candidate = prepareLedgerNode<ProviderRegistration>(d->backingBudget);
        candidate->key = key;
        candidate->provider = provider;
        candidate->backgroundRetirement = capabilities.backgroundRetirement;
        d->providers.insert(*candidate);
    }
    auto rollback = qScopeGuard([&] {
        QMutexLocker lock(&d->mutex);
        d->providers.erase(d->providers.iterator_to(*candidate));
    });
    // Provider code remains outside the owner gate. Mutable-domain providers
    // retain only a weak reference to this ledger admission authority.
    if (!provider->registerBackingDomainAdmission(d->domainAdmission, error)) return false;
    QMutexLocker locker(&d->mutex);
    candidate->acceptedRevision = ++d->providerRevision;
    rollback.dismiss();
    candidate.release(); // The original index owns the already admitted node.
    KisPageStoreDetail::setError(error, {});
    return true;
}
catch (const std::bad_alloc &)
{
    KisPageStoreDetail::setError(error, QStringLiteral("provider registration storage was refused"));
    return false;
}

std::shared_ptr<KisPageReplicaProvider> KisPageOwnerLedger::provider(
    KisReplicaProviderId providerId,
    KisReplicaProviderEpoch epoch) const
{
    QMutexLocker locker(&d->mutex);
    const auto registered = d->registeredProvider(providerKey(providerId, epoch));
    return registered ? registered->provider : std::shared_ptr<KisPageReplicaProvider>{};
}

std::shared_ptr<KisPageReplicaProvider> KisPageOwnerLedger::providerFor(
    KisPageAccessRequirement access) const
{
    if (!access.isValid()) return {};
    const auto cut = d->providerCut();
    ProviderKey cursor{};
    std::shared_ptr<KisPageReplicaProvider> selected;
    while (const auto candidate = d->nextProviderInCut(cut, &cursor)) {
        if (!candidate->capabilities().supports(access)) continue;
        if (selected) {
            // Route selection must be explicit when more than one provider
            // advertises the same consumer access requirement.
            return {};
        }
        selected = candidate;
    }
    return selected;
}

KisPageRequestId KisPageOwnerLedger::nextRequestId()
{
    QMutexLocker locker(&d->mutex);
    return d->completions ? KisPageStoreDetail::allocateMonotonicId<KisPageRequestId>(&s_nextRequestId)
                          : KisPageRequestId();
}

KisPageOperationId KisPageOwnerLedger::nextOperationId()
{
    QMutexLocker locker(&d->mutex);
    return d->completions ? KisPageStoreDetail::allocateMonotonicId<KisPageOperationId>(&s_nextOperationId)
                          : KisPageOperationId();
}

KisPageLeaseId KisPageOwnerLedger::nextLeaseId()
{
    QMutexLocker locker(&d->mutex);
    return d->completions ? KisPageStoreDetail::allocateMonotonicId<KisPageLeaseId>(&s_nextLeaseId)
                          : KisPageLeaseId();
}

KisPageWriterToken KisPageOwnerLedger::nextWriterToken()
{
    QMutexLocker locker(&d->mutex);
    return d->completions ? KisPageStoreDetail::allocateMonotonicId<KisPageWriterToken>(&s_nextWriterToken)
                          : KisPageWriterToken();
}

bool KisPageOwnerLedger::bindProviderOperation(
    KisPageOperationId operation,
    const KisReplicaOperation &result,
    QString *error)
{
    QMutexLocker locker(&d->mutex);
    if (!d->acceptsProviderResultLocked(operation, result, error)) return false;
    try {
        if (d->operations.empty())
            d->operations = OperationIndex(std::less<quint64>{},
                KisMutationStorageAllocator<std::pair<const quint64, ProviderOperationRecord>>::retained(d->backingBudget));
        d->operations.emplace(operation.value, ProviderOperationRecord{result.completion});
    }
    catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider operation storage was refused"));
        return false;
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisVerifiedCompletion KisPageOwnerLedger::verifyTerminalProviderResult(
    KisPageOperationId operation,
    const KisReplicaOperation &result,
    QString *error) const
{
    std::shared_ptr<KisCompletionRegistry> completions;
    {
        QMutexLocker lock(&d->mutex);
        if (!d->acceptsProviderResultLocked(operation, result, error)) return {};
        completions = d->completions;
    }
    const auto verified = completions->verifyTerminal(result.completion);
    if (!verified.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider operation is not terminal"));
        return {};
    }
    KisPageStoreDetail::setError(error, {});
    return verified;
}

bool KisPageOwnerLedger::bindRetirementOperation(
    KisPageOperationId operation,
    const KisReplicaOperation &result,
    QString *error)
{
    QMutexLocker lock(&d->mutex);
    auto found = d->operations.find(operation.value);
    if (!d->completions || found == d->operations.end() ||
        !found->second.retirement || found->second.completion.isValid() ||
        !result.isValid() || !(result.operation == operation) ||
        !d->registeredProvider(providerKey(result.replica.provider, result.replica.providerEpoch))) {
        KisPageStoreDetail::setError(error, QStringLiteral("retirement operation was not prepared or result mismatched"));
        return false;
    }
    found->second.completion = result.completion;
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPageOperationId KisPageOwnerLedger::prepareRetirementOperation(KisPageRetirementRecord &record)
{
    QMutexLocker lock(&d->mutex);
    if (!d->completions || record.operationStorage.empty()) return {};
    if (!d->operations.empty() && d->operations.get_allocator() != record.operationStorage.get_allocator())
        return {}; // A node handle must retain the same accounting allocator.
    const auto operation = KisPageStoreDetail::allocateMonotonicId<KisPageOperationId>(&s_nextOperationId);
    if (!operation.isValid()) return {};
    record.operationStorage.key() = operation.value;
    record.operationStorage.mapped() = {{}, &record};
    if (d->operations.empty() && d->operations.get_allocator() != record.operationStorage.get_allocator())
        d->operations = OperationIndex(std::less<quint64>{}, record.operationStorage.get_allocator());
    const auto inserted = d->operations.insert(std::move(record.operationStorage));
    Q_ASSERT(inserted.inserted);
    return operation;
}

void KisPageOwnerLedger::cancelRetirementOperation(KisPageOperationId operation)
{
    QMutexLocker lock(&d->mutex);
    auto found = d->operations.find(operation.value);
    Q_ASSERT(found != d->operations.end() && !found->second.completion.isValid());
    if (found != d->operations.end() && !found->second.completion.isValid()) {
        auto *record = found->second.retirement;
        Q_ASSERT(record);
        record->operationStorage = d->operations.extract(found);
    }
}

KisVerifiedCompletion KisPageOwnerLedger::verifyProviderOperation(
    KisPageOperationId operation,
    QString *error) const
{
    std::shared_ptr<KisCompletionRegistry> completions;
    const KisCompletionTicket completion = d->operationCompletion(operation, &completions);
    if (!completion.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider operation is unknown"));
        return {};
    }
    const KisVerifiedCompletion verified = completions->verifyTerminal(completion);
    if (!verified.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider operation is not terminal"));
        return {};
    }
    KisPageStoreDetail::setError(error, {});
    return verified;
}

bool KisPageOwnerLedger::releaseTerminalProviderOperation(
    KisPageOperationId operation,
    QString *error)
{
    std::shared_ptr<KisCompletionRegistry> completions;
    const KisCompletionTicket completion = d->operationCompletion(operation, &completions);
    if (!completion.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider operation is unknown"));
        return false;
    }
    if (!completions || !completions->verifyTerminal(completion).isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider operation is not terminal"));
        return false;
    }
    QMutexLocker locker(&d->mutex);
    auto operationIt = d->operations.find(operation.value);
    if (!d->completions || operationIt == d->operations.end() ||
        !(operationIt->second.completion == completion)) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider operation changed before release"));
        return false;
    }
    if (auto *record = operationIt->second.retirement)
        record->operationStorage = d->operations.extract(operationIt);
    else
        d->operations.erase(operationIt);
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPageReadinessStatus KisPageOwnerLedger::watchProviderOperation(
    KisPageOperationId operation, KisPageReadinessCallback scheduleReady,
    KisPageReadinessSubscription *subscription) const
{
    std::shared_ptr<KisCompletionRegistry> registry;
    const auto ticket = d->operationCompletion(operation, &registry);
    return registry && ticket.isValid()
        ? registry->watchTerminal(ticket, std::move(scheduleReady), subscription)
        : KisPageReadinessStatus::Unavailable;
}

KisPageReadinessStatus KisPageOwnerLedger::watchCompletion(
    const KisCompletionTicket &ticket, KisPageReadinessCallback scheduleReady,
    KisPageReadinessSubscription *subscription) const
{
    std::shared_ptr<KisCompletionRegistry> registry;
    {
        QMutexLocker lock(&d->mutex);
        registry = d->completions;
    }
    return registry ? registry->watchTerminal(ticket, std::move(scheduleReady), subscription)
                    : KisPageReadinessStatus::Unavailable;
}

qsizetype KisPageOwnerLedger::providerOperationCount() const
{
    QMutexLocker locker(&d->mutex);
    return d->operations.size();
}

qsizetype KisPageOwnerLedger::publicationBlockingOperationCount() const
{
    QMutexLocker locker(&d->mutex);
    return std::count_if(d->operations.cbegin(), d->operations.cend(),
                         [](const auto &entry) {
                             return !entry.second.retirement;
                         });
}

qsizetype KisPageOwnerLedger::sealedProofCount() const
{
    QMutexLocker locker(&d->mutex);
    const qsizetype exported = d->sealedProofs.size();
    auto *metadata = d->cpuMetadata;
    const auto issuer = d->cpuProofIssuer;
    locker.unlock();
    return exported + (metadata ? metadata->cpuProofCount(issuer) : 0);
}

bool KisPageOwnerLedger::bindCpuProofs(KisPageMetadataCoordinator &metadata, const KisCompletionTicket &completion)
{
    QMutexLocker locker(&d->mutex);
    if (d->cpuMetadata) return d->cpuMetadata == &metadata;
    if (!d->completions || !d->completions->verifyTerminal(completion).succeeded()) return false;
    const auto issuer = KisPageStoreDetail::allocateMonotonicId<ProofIssuerId>(&s_nextProofIssuer).value;
    if (!issuer || !metadata.bindCpuProofIssuer(issuer, completion)) return false;
    d->cpuMetadata = &metadata;
    d->cpuProofIssuer = issuer;
    return true;
}

KisPreparedPageProof KisPageOwnerLedger::projectPreparedPage(const KisPageVersion &version,
    KisPageTransactionId transaction, quint64 stamp, const KisReplicaAllocationIdentity &allocation) const
{
    QMutexLocker lock(&d->mutex);
    const auto found = d->sealedProofs.find(stamp, ProofLess{});
    if (found != d->sealedProofs.end()) {
        const auto &proof = found->proof;
        return proof.authority.version == version && proof.transaction == transaction
            && proof.authority.allocationIdentity() == allocation ? proof : KisPreparedPageProof{};
    }
    const auto backing = d->backings.find(allocation);
    if (backing == d->backings.end() || !(backing->second.replica.version == version)) return {};
    // This registration's handle is immutable until its exact allocation key
    // is removed/rekeyed. Budget/domain facts have separate mutable owners.
    const auto authority = backing->second.replica;
    auto *metadata = d->cpuMetadata;
    const auto issuer = d->cpuProofIssuer;
    lock.unlock();
    return metadata ? metadata->cpuProof(version, transaction, stamp, issuer, authority) : KisPreparedPageProof{};
}

bool KisPageOwnerLedger::ownsPreparedPageProof(
    const KisPreparedPageProof &proof) const
{
    if (!proof.isValid())
        return false;
    QMutexLocker locker(&d->mutex);
    const auto found = d->sealedProofs.find(proof.providerValidationStamp, ProofLess{});
    if (found != d->sealedProofs.end() && found->proof == proof) return true;
    auto *metadata = d->cpuMetadata;
    const auto issuer = d->cpuProofIssuer;
    locker.unlock();
    return metadata && metadata->ownsCpuProof(proof, issuer);
}

bool KisPageOwnerLedger::sealCpuPreparedPage(KisPagePrivateWrite &result,
    KisPageTransactionId transaction, const KisCompletionTicket &producerCompletion, QString *error)
{
    // The caller retains its scope/claims and has joined every mutable guard.
    // The fixed successful CPU ticket was verified when binding this issuer;
    // sealCpuProof checks that exact ticket and the original reservation.
    const auto authorityProvider = provider(result.target.provider, result.target.providerEpoch);
    if (!result.reserved || !result.completed || !result.descriptor || !authorityProvider
        || !authorityProvider->validate(result.target, *result.descriptor)) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU private authority failed provider validation"));
        return false;
    }
    KisPageMetadataCoordinator *metadata;
    KisPreparedPageProof sealed;
    quint64 issuer;
    {
        QMutexLocker locker(&d->mutex);
        metadata = d->cpuMetadata;
        issuer = d->cpuProofIssuer;
        if (!metadata || !d->nextValidationStamp
            || d->nextValidationStamp == std::numeric_limits<quint64>::max()) return false;
        sealed.transaction = transaction;
        sealed.authority = result.target;
        sealed.producerCompletion = producerCompletion;
        sealed.providerValidationStamp = d->nextValidationStamp++;
    }
    if (!metadata->sealCpuProof(sealed, issuer)) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU result changed or already has a capability"));
        return false;
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPageOwnerLedger::sealPreparedPage(const KisPageMetadataCoordinator &metadata,
    const KisPageVersion &version, KisPageTransactionId transaction,
    const KisPageAllocationDescriptor &descriptor, const KisCompletionTicket &producerCompletion,
    KisPreparedPageProof *proof, QString *error, bool *storageRefused)
try
{
    if (storageRefused) *storageRefused = false;
    if (!proof || !version.isValid() || !transaction.isValid() ||
        !descriptor.isValid() || !producerCompletion.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page proof request is invalid"));
        return false;
    }

    std::shared_ptr<KisCompletionRegistry> completions;
    KisBackingBudgetController *budget = nullptr;
    {
        QMutexLocker locker(&d->mutex);
        if (!d->completions) {
            KisPageStoreDetail::setError(error, QStringLiteral("owner ledger is not configured"));
            return false;
        }
        completions = d->completions;
        budget = d->backingBudget;
    }
    const KisVerifiedCompletion verified =
        completions->verifyTerminal(producerCompletion);
    if (!verified.isValid() || !verified.succeeded()) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page producer did not complete successfully"));
        return false;
    }

    KisPageMetadataCoordinator::VersionInfo snapshot;
    if (!metadata.versionSnapshot(version, &snapshot)) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page metadata is unavailable"));
        return false;
    }
    if (!snapshot.version.isValid() || snapshot.publication != KisPagePublicationState::Prepared ||
        !(snapshot.preparedBy == transaction) || !snapshot.authority.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("page is not prepared by the requested transaction"));
        return false;
    }

    auto record = prepareLedgerNode<SealedProofRecord>(budget);
    const std::shared_ptr<KisPageReplicaProvider> authorityProvider =
        provider(snapshot.authority.provider, snapshot.authority.providerEpoch);
    if (!authorityProvider ||
        !authorityProvider->validate(snapshot.authority, descriptor)) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page authority failed provider validation"));
        return false;
    }

    KisPreparedPageProof sealed;
    sealed.transaction = transaction;
    sealed.authority = snapshot.authority;
    sealed.producerCompletion = producerCompletion;
    QMutexLocker locker(&d->mutex);
    if (!d->completions || d->nextValidationStamp == 0 ||
        d->nextValidationStamp == std::numeric_limits<quint64>::max()) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page proof could not be sealed"));
        return false;
    }
    sealed.providerValidationStamp = d->nextValidationStamp;
    if (!sealed.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page proof could not be sealed"));
        return false;
    }
    record->proof = sealed;
    d->sealedProofs.insert(*record);
    record.release();
    ++d->nextValidationStamp;
    locker.unlock();
    *proof = sealed;
    KisPageStoreDetail::setError(error, {});
    return true;
}
catch (const std::bad_alloc &)
{
    if (storageRefused) *storageRefused = true;
    KisPageStoreDetail::setError(error, QStringLiteral("prepared page proof storage was refused"));
    return false;
}

bool KisPageOwnerLedger::validatePreparedPage(
    const KisPageMetadataCoordinator &metadata,
    const KisPreparedPageProof &proof,
    const KisPageAllocationDescriptor &descriptor,
    QString *error) const
try
{
    if (!proof.isValid() || !descriptor.isValid() ||
        !proof.authority.layout.matches(descriptor)) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page proof or layout is invalid"));
        return false;
    }
    std::shared_ptr<KisCompletionRegistry> completions;
    if (!ownsPreparedPageProof(proof)) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page proof was not sealed by this owner"));
        return false;
    }
    {
        QMutexLocker locker(&d->mutex);
        if (!d->completions) {
            KisPageStoreDetail::setError(error, QStringLiteral("prepared page proof was not sealed by this owner"));
            return false;
        }
        completions = d->completions;
    }
    const KisVerifiedCompletion verified =
        completions ? completions->verifyTerminal(proof.producerCompletion)
                    : KisVerifiedCompletion();
    if (!verified.isValid() || !verified.succeeded()) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page proof completion is not successful"));
        return false;
    }

    KisPageMetadataCoordinator::VersionInfo snapshot;
    if (!metadata.versionSnapshot(proof.authority.version, &snapshot) || !snapshot.version.isValid()
        || snapshot.publication != KisPagePublicationState::Prepared
        || !(snapshot.preparedBy == proof.transaction) || !(snapshot.authority == proof.authority)) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page proof no longer matches metadata"));
        return false;
    }
    const std::shared_ptr<KisPageReplicaProvider> authorityProvider =
        provider(proof.authority.provider, proof.authority.providerEpoch);
    if (!authorityProvider ||
        !authorityProvider->validate(snapshot.authority, descriptor)) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page proof no longer matches provider state"));
        return false;
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}
catch (const std::bad_alloc &)
{
    KisPageStoreDetail::setError(error, QStringLiteral("prepared page proof validation storage was refused"));
    return false;
}

bool KisPageOwnerLedger::revokePreparedPage(const KisPreparedPageProof &proof)
{
    if (!proof.isValid()) return false;
    LedgerNode<SealedProofRecord> released;
    QMutexLocker locker(&d->mutex);
    auto proofIt = d->sealedProofs.find(proof.providerValidationStamp, ProofLess{});
    if (!d->completions || proofIt == d->sealedProofs.end() ||
        !(proofIt->proof == proof)) {
        auto *metadata = d->cpuMetadata;
        const auto issuer = d->cpuProofIssuer;
        locker.unlock();
        return metadata && metadata->revokeCpuProof(proof, issuer);
    }
    released.reset(&*proofIt);
    d->sealedProofs.erase(proofIt);
    return true;
}

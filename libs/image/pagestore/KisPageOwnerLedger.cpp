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
#include <boost/container/static_vector.hpp>

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
const KisPagePreparedBackingChange s_handoffClaim;
const auto *const HandoffClaim = &s_handoffClaim;

PhysicalBackingKey physicalBackingKey(
    const KisReplicaHandle &replica,
    const KisReplicaBackingFootprint &footprint)
{
    return {replica.provider, replica.providerEpoch, footprint.physicalSlot};
}

void addBackingDelta(KisBackingBudgetDelta *delta, KisBackingBudgetClass budgetClass,
                     KisPageAccessDomain domain, qint64 bytes)
{
    auto &bucket = delta->bucket(static_cast<size_t>(budgetClass));
    if (qint64 *component = kisPageDomainComponent(bucket, domain))
        *component += bytes;
}

bool addBackingDeltaChecked(KisBackingBudgetDelta *delta,
                            KisBackingBudgetClass budgetClass,
                            KisPageAccessDomain domain, qint64 bytes)
{
    auto &bucket = delta->bucket(static_cast<size_t>(budgetClass));
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
    for (quint32 classes = signedDelta.changedClasses(); classes; classes &= classes - 1) {
        const size_t i = size_t(qCountTrailingZeroBits(classes));
        const auto &source = signedDelta.bucket(i);
        result.setBucket(i, {std::max(qint64(0), source.cpuRam),
            std::max(qint64(0), source.umaShared), std::max(qint64(0), source.discreteVram),
            std::max(qint64(0), source.ssd)});
    }
    return result;
}

bool hasBackingDelta(const KisBackingBudgetDelta &delta)
{
    return delta.changedClasses() != 0;
}

using BackingRecord = KisPageBackingRecord;
using BackingIndex = KisPageBackingIndex;
using PhysicalBackingRecord = KisPagePhysicalBackingRecord;
using PhysicalBackingIndex = KisPagePhysicalBackingIndex;

using ClassReferences = decltype(PhysicalBackingRecord::references);

KisBackingBudgetClass chargedBackingClass(const ClassReferences &references)
{
    // Physical bytes are charged once, to a surviving use before terminal
    // aliases. Expose that use at fallible admission, not after the provider
    // has destroyed the last Debt alias and can no longer reject a new fee.
    constexpr std::array<KisBackingBudgetClass, 8> priority{
        KisBackingBudgetClass::Current,
        KisBackingBudgetClass::ActivePending,
        KisBackingBudgetClass::InFlight,
        KisBackingBudgetClass::RetainedHistory,
        KisBackingBudgetClass::OptionalCache,
        KisBackingBudgetClass::IdleCapacity,
        KisBackingBudgetClass::MetadataArena,
        KisBackingBudgetClass::RetirementDebt};
    for (const KisBackingBudgetClass budgetClass : priority) {
        if (references[static_cast<size_t>(budgetClass)] != 0)
            return budgetClass;
    }
    return KisBackingBudgetClass::Count;
}

bool changePhysicalReferenceClass(ClassReferences *references,
                                  KisBackingBudgetClass before,
                                  KisBackingBudgetClass after)
{
    if (!references || before == after)
        return false;
    auto &beforeCount = (*references)[static_cast<size_t>(before)];
    auto &afterCount = (*references)[static_cast<size_t>(after)];
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

using PreparedBackingChange = KisPagePreparedBackingChange;

// One row view serves contiguous commit scratch and the prepared member in D.
// Only prepared fields move during sorting; publication inputs retain order.
class BorrowedBackingChanges
{
    class Iterator : public boost::iterator_facade<Iterator, PreparedBackingChange,
                                                   boost::random_access_traversal_tag>
    {
    public:
        Iterator() = default;
        Iterator(PreparedBackingChange *rows, KisPagePublicationRows publicationRows, ptrdiff_t index)
            : rows(rows), publicationRows(publicationRows), index(index) {}
    private:
        PreparedBackingChange &dereference() const
        { return publicationRows ? publicationRows[size_t(index)].backingPreparation : rows[index]; }
        bool equal(const Iterator &other) const { return rows == other.rows && index == other.index; }
        void increment() { ++index; }
        void decrement() { --index; }
        void advance(ptrdiff_t n) { index += n; }
        ptrdiff_t distance_to(const Iterator &other) const { return other.index - index; }
        PreparedBackingChange *rows = nullptr;
        KisPagePublicationRows publicationRows;
        ptrdiff_t index = 0;
        friend class boost::iterator_core_access;
    };
public:
    BorrowedBackingChanges(PreparedBackingChange *rows, KisPagePublicationRows publicationRows, size_t capacity, size_t size = 0)
        : rows(rows), publicationRows(publicationRows), capacity(capacity), used(size) {}
    void reserve(size_t n) { Q_ASSERT(n <= capacity); Q_UNUSED(n); }
    void push_back(PreparedBackingChange value)
    {
        static_assert(std::is_trivially_destructible_v<PreparedBackingChange>);
        Q_ASSERT(used < capacity);
        auto *slot = &*(begin() + ptrdiff_t(used++));
        ::new (static_cast<void *>(slot)) PreparedBackingChange(value);
    }
    size_t size() const { return used; }
    Iterator begin() const { return Iterator(rows, publicationRows, 0); }
    Iterator end() const { return Iterator(rows, publicationRows, ptrdiff_t(used)); }
    PreparedBackingChange &operator[](size_t i) const { Q_ASSERT(i < used); return *(begin() + ptrdiff_t(i)); }
private:
    PreparedBackingChange *rows;
    KisPagePublicationRows publicationRows;
    size_t capacity;
    size_t used;
};

KisBackingBudgetDelta retirementCredit(const PreparedBackingChange &change)
{
    KisBackingBudgetDelta value;
    Q_ASSERT(change.credit && change.physical);
    addBackingDelta(&value, change.creditBefore, change.physical->domain, -qint64(change.physical->byteSize()));
    addBackingDelta(&value, KisBackingBudgetClass::RetirementDebt, change.physical->domain, qint64(change.physical->byteSize()));
    return value;
}

struct PreparedBackingChangeSlot
{
    explicit PreparedBackingChangeSlot(KisBackingBudgetController *budget)
        : changes(KisMutationStorageAllocator<PreparedBackingChange>(budget))
        {}
    ChargedVector<PreparedBackingChange> changes;
    // map nodes stay stable under the original physical/target claims.
    // Never retain a neighbouring iterator across the unlocked provider call.
    KisBackingBudgetReservation reservation;
    quint32 generation = 0;
    bool occupied = false;
};

}

class KisPageOwnerLedger::Private
{
public:
    using Storage = KisMutationStorageAllocator<Private>;
    explicit Private(const Storage &allocator) : storage(allocator) {}
    void detachRetirementIndexes() noexcept {
        operations.clear_and_dispose([&](KisPageProviderOperationNode *node) {
            if (!node->second.retirement) {
                std::destroy_at(node); operationStorage.deallocate(node, 1);
            }
        });
        backings.clear_and_dispose([](KisPageBackingNode *node) {
            auto &record = node->second;
            if (record.physicalRecord) record.physicalRecord->unlink(record);
            record.retirementHeadroom.release();
            KisPageRetirementRecordDeleter{}(std::exchange(record.retirement, nullptr));
        });
        physicalBackings.clear_and_dispose(KisPagePhysicalBackingRecordDeleter{});
    }
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

    struct ProviderBatch {
        boost::container::static_vector<std::shared_ptr<KisPageReplicaProvider>, 16> providers;
        bool more = false;
    };
    ProviderBatch providersInCut(quint64 cut, ProviderKey *cursor) const
    {
        ProviderBatch result;
        QMutexLocker lock(&mutex);
        auto next = *cursor == ProviderKey{} ? providers.begin() : providers.upper_bound(*cursor, ProviderLess{});
        while (next != providers.end() && result.providers.size() < result.providers.capacity()) {
            *cursor = next->key;
            if (next->acceptedRevision && next->acceptedRevision <= cut)
                result.providers.push_back(next->provider);
            ++next;
        }
        result.more = next != providers.end();
        return result;
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
        std::shared_ptr<KisCompletionRegistry> *registry, bool allowRetirement = true) const
    {
        QMutexLocker locker(&mutex);
        const auto found = operations.find(operation.value);
        if (!completions || !operation.isValid() || found == operations.end()
            || (!allowRetirement && found->second.retirement))
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
        if (!activeHandoffs && !activeBorrowedChanges && freePreparedChanges.size() == preparedChanges.size()) return false;
        const auto *physical = record.physicalRecord;
        Q_ASSERT(physical);
        const auto *claim = physical->classClaim;
        if (claim == HandoffClaim) return true;
        // Follow only this physical group's selected logical rows. Unselected
        // aliases retain their existing same-class no-op semantics.
        for (; claim; claim = claim->next)
            if (&claim->backing->second == &record) return true;
        return false;
    }

    // Caller owns the original record and holds mutex. Deferred storage and
    // headroom destruction belong to its lock-free exit, including claim waits.
    bool settleBacking(KisPageRetirementRecord &original,
                       KisBackingBudgetReservation &releasedHeadroom,
                       KisPagePhysicalBackingPointer &releasedPhysical) noexcept
    {
        auto &node = original.backingNode;
        if (!node.is_linked() || original.storage.budget != backingBudget
            || node.second.retirement || !(node.second.replica == original.replica)) return false;
        Q_ASSERT(!isPreparedBacking(node.second));
        auto *claimedPhysical = node.second.physicalRecord;
        Q_ASSERT(claimedPhysical);
        while (claimedPhysical->domainClaim || claimedPhysical->classClaim) {
            ++physicalWaiters;
            physicalClaimsChanged.wait(&mutex);
            --physicalWaiters;
            // Other physical members may change; this transferred member is
            // pinned by the sole caller and cannot be reclassified or retagged.
            Q_ASSERT(node.is_linked() && node.second.physicalRecord == claimedPhysical);
        }
        const auto found = backings.iterator_to(node);
        auto &record = found->second;
        backings.erase(found);
        releasedHeadroom = std::move(record.retirementHeadroom);
        auto *physical = record.physicalRecord;
        Q_ASSERT(physical);
        physical->unlink(record);
        auto &references = physical->references[static_cast<size_t>(record.budgetClass)];
        Q_ASSERT(references != 0);
        if (references)
            --references;
        const KisBackingBudgetClass afterCharge = chargedBackingClass(physical->references);
        if (afterCharge == KisBackingBudgetClass::Count) {
            Q_ASSERT(!physical->members);
            if (backingBudget) {
                backingBudget->releaseLive(physical->chargedClass,
                                              physical->domain,
                                              physical->byteSize());
            }
            releasedPhysical.reset(physical);
            physicalBackings.erase(physicalBackings.iterator_to(*physical));
            return true;
        }
        Q_ASSERT(physical->members);
        if (physical->representative == record.replica)
            physical->representative = physical->members->replica;
        // Retirement admission already exposed and budgeted every surviving
        // owner class before provider destruction. Removing a terminal Debt alias
        // therefore cannot reveal an unprepared class here.
        Q_ASSERT(afterCharge == physical->chargedClass);
        return true;
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
        return source->second.physicalRecord->classClaim == HandoffClaim;
    }

    bool projectPhysicalCharge(KisBackingBudgetDelta *delta, PreparedBackingChange &last,
                               KisBackingBudgetClass after, BackingRecord *credit, QString *error)
    {
        auto &source = *last.physical;
        last.chargedAfter = after;
        if (source.chargedClass != KisBackingBudgetClass::RetirementDebt
            && after == KisBackingBudgetClass::RetirementDebt && credit) {
            // The existing headroom pays this exact physical transition. Do
            // not build a wide delta only to subtract the same two components.
            last.credit = credit;
            last.creditBefore = source.chargedClass;
            return true;
        }
        if (addPhysicalChargeChange(delta, source, source.chargedClass, after)) return true;
        KisPageStoreDetail::setError(error, QStringLiteral("physical backing delta overflows"));
        return false;
    }

    // A location resolved under this ledger gate, not an external authority or
    // a second live record. All projection/claim work consumes it in that same
    // window; the original prepared rows retain locations after the gate opens.
    struct ResolvedBackingChange {
        BackingIndex::iterator backing;
        KisBackingBudgetClass after;
        KisPageRetirementRecord **idleResult;
    };

    bool resolveBackingChange(const KisBackingClassChange &change,
                              ResolvedBackingChange *resolved, QString *error)
    {
        auto *original = change.original;
        if (original && (!original->backingNode.is_linked() || original->storage.budget != backingBudget
            || original->backingNode.second.retirement != original)) {
            KisPageStoreDetail::setError(error, QStringLiteral("original backing class input lost ownership"));
            return false;
        }
        const auto found = original ? backings.iterator_to(original->backingNode)
                                    : backings.find(change.replica.allocationIdentity());
        if (!change.replica.isValid() || found == backings.end()
            || found->second.budgetClass != change.before
            || !(found->second.replica == change.replica)
            || change.before == change.after
            || static_cast<size_t>(change.after) >= static_cast<size_t>(KisBackingBudgetClass::Count)) {
            KisPageStoreDetail::setError(error, QStringLiteral("backing class change is invalid"));
            return false;
        }
        *resolved = {found, change.after, change.idleResult};
        return true;
    }

    template<class NextChange, class Rows>
    bool buildPhysicalBackingDelta(
        ResolvedBackingChange change, NextChange &nextChange, size_t maximum,
        KisBackingBudgetDelta *signedDelta,
        Rows &projected, const void *claim,
        QString *error)
    {
        // Original caller storage carries accepted locations through commit/cancel.
        // Physical claims keep these map nodes stable outside the ledger gate.
        projected.reserve(maximum);
        bool shared = false;
        int readResult = 0;
        do {
            if (projected.size() == maximum) {
                KisPageStoreDetail::setError(error, QStringLiteral("backing change workspace is too small"));
                return false;
            }
            const auto found = change.backing;
            if (!found->second.retirement) {
                KisPageStoreDetail::setError(error, QStringLiteral("backing responsibility was transferred"));
                return false;
            }
            // Membership retains a stable physical record until unlink. The
            // sole case has exactly one class before and after: no copied
            // reference array, priority scan or second grouping pass.
            const bool alias = found->second.previous || found->second.next;
            auto *physical = found->second.physicalRecord;
            Q_ASSERT(physical);
            if (physical->domainClaim || (physical->classClaim && physical->classClaim->claim != claim)) {
                KisPageStoreDetail::setError(error, QStringLiteral("physical backing transition is active"));
                return false;
            }
            if (!alias && physical->classClaim) {
                KisPageStoreDetail::setError(error, QStringLiteral("backing is classified more than once"));
                return false;
            }
            // Record before claiming so allocation failure uses the same rollback.
            projected.push_back({found, physical, projected.size(), change.after,
                                 KisBackingBudgetClass::Count, change.idleResult, claim});
            auto &row = projected[projected.size() - 1];
            if (!alias) {
                physical->classClaim = &row;
                Q_ASSERT(physical->chargedClass == found->second.budgetClass);
                Q_ASSERT(physical->references[static_cast<size_t>(found->second.budgetClass)] == 1);
                if (!projectPhysicalCharge(signedDelta, row, change.after,
                        found->second.retirementHeadroom.isValid() ? &found->second : nullptr, error)) return false;
            }
            shared |= alias;
        } while ((readResult = nextChange(&change)) > 0);
        if (readResult < 0) {
            KisPageStoreDetail::setError(error, QStringLiteral("backing change source is invalid"));
            return false;
        }
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
                if (less(a.physical->identity, b.physical->identity)) return true;
                if (less(b.physical->identity, a.physical->identity)) return false;
                // Preserve the caller's order and its original first credit owner.
                return a.order < b.order;
            });
        }
        if (shared) for (size_t first = 0; first < projected.size();) {
            auto *source = projected[first].physical;
            // Sorting moved row addresses, including already projected sole rows.
            if (projected[first].chargedAfter != KisBackingBudgetClass::Count) {
                source->classClaim = &projected[first++];
                continue;
            }
            Q_ASSERT(!source->classClaim && !source->domainClaim);
            source->classClaim = &projected[first];
            auto next = source->references;
            BackingRecord *credit = nullptr;
            do {
                auto &entry = projected[first];
                if (!credit && entry.backing->second.retirementHeadroom.isValid())
                    credit = &entry.backing->second;
                if (!changePhysicalReferenceClass(&next, entry.backing->second.budgetClass, entry.after)) {
                    KisPageStoreDetail::setError(error, QStringLiteral("physical backing class references are invalid"));
                    return false;
                }
                ++first;
                if (first < projected.size() && projected[first].physical == source)
                    entry.next = &projected[first];
            } while (first < projected.size() && projected[first].physical == source);
            if (!projectPhysicalCharge(signedDelta, projected[first - 1], chargedBackingClass(next), credit, error)) return false;
        }
        return true;
    }

    template<class NextChange>
    bool prepareClassChanges(size_t maximum, NextChange nextChange, quint64 *cookie, QString *error)
    {
        *cookie = 0;
        KisBackingClassChange change; // Reuse the one external input carrier for this whole stream.
        auto next = [&](ResolvedBackingChange *resolved) -> int {
            if (!nextChange(&change)) return 0;
            return resolveBackingChange(change, resolved, error) ? 1 : -1;
        };
        ResolvedBackingChange first;
        const int status = next(&first);
        if (status < 0) return false;
        if (!status) return backingBudget->admitUnchangedBacking(error);
        const quint64 prepared = beginPreparedChange();
        auto &slot = *preparedChange(prepared);
        auto rollback = qScopeGuard([&] { finishPreparedChange(slot, prepared); });
        slot.changes.reserve(maximum);
        if (!prepareProjectedChanges(first, next, maximum, slot.changes,
                                     slot.changes.data(), slot.reservation, error)) return false;
        *cookie = prepared;
        rollback.dismiss();
        KisPageStoreDetail::setError(error, {});
        return true;
    }

    template<class NextChange, class Rows>
    bool prepareProjectedChanges(ResolvedBackingChange first, NextChange &next, size_t maximum,
        Rows &rows, const void *claim, KisBackingBudgetReservation &reservation, QString *error)
    {
        KisBackingBudgetDelta delta;
        if (!buildPhysicalBackingDelta(first, next, maximum, &delta, rows, claim, error)) return false;
        if (hasBackingDelta(delta)) {
            reservation = backingBudget->reserveChange(delta, error);
            if (!reservation.isValid()) return false;
        } else if (!backingBudget->admitUnchangedBacking(error)) return false;
        KisPageStoreDetail::setError(error, {});
        return true;
    }

    template<class NextChange>
    KisBackingClassChangeReservation prepareBorrowedChangesLocked(
        KisPageOwnerLedger *owner, KisPagePreparedBackingChange *rows, size_t capacity,
        NextChange &next, QString *error, KisPagePublicationRows publicationRows = {})
    {
        ResolvedBackingChange first;
        const int status = next(&first);
        if (status < 0) {
            KisPageStoreDetail::setError(error, QStringLiteral("backing change source is invalid"));
            return {};
        }
        if (!status)
            return backingBudget->admitUnchangedBacking(error) ? KisBackingClassChangeReservation(owner, 0)
                                                               : KisBackingClassChangeReservation{};
        KisBackingClassChangeReservation result;
        BorrowedBackingChanges projected(rows, publicationRows, capacity);
        auto rollback = qScopeGuard([&] { finishProjectedChanges(projected, rows, result.m_budget); });
        if (!prepareProjectedChanges(first, next, capacity, projected, rows, result.m_budget, error)) return {};
        result.m_owner = owner;
        result.m_rows = rows;
        result.m_publicationRows = publicationRows;
        result.m_count = projected.size();
        ++activeBorrowedChanges;
        rollback.dismiss();
        return result;
    }

    template<class Rows>
    void finishProjectedChanges(Rows &rows, const void *claim, KisBackingBudgetReservation &reservation,
                                bool committed = false) noexcept
    {
        for (const auto &row : rows) {
            // Commit the complete fee before releasing headroom or the claim.
            if (committed && row.after != KisBackingBudgetClass::ActivePending)
                row.backing->second.retirementHeadroom.release();
            if (row.physical && row.physical->classClaim && row.physical->classClaim->claim == claim)
                row.physical->classClaim = nullptr;
        }
        reservation.release();
        notifyPhysicalClaimsChanged();
    }

    template<class Rows>
    void commitProjectedChanges(Rows &rows, const void *claim, KisBackingBudgetReservation &reservation) noexcept
    {
        for (const auto &row : rows) {
            Q_ASSERT(row.physical->classClaim && row.physical->classClaim->claim == claim);
            installBackingChange(row);
            if (row.credit) row.credit->retirementHeadroom.commit(retirementCredit(row));
        }
        if (reservation.isValid()) reservation.commitPrepared();
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
        Q_ASSERT(slot.changes.empty());
        slot.occupied = true;
        return cookie;
    }


    void finishPreparedChange(PreparedBackingChangeSlot &slot, quint64 cookie, bool committed = false) noexcept
    {
        finishProjectedChanges(slot.changes, slot.changes.data(), slot.reservation, committed);
        slot.changes.clear();
        slot.occupied = false;
        freePreparedChanges.push_back(quint32(cookie));
    }

    void installBackingChange(const PreparedBackingChange &change) noexcept
    {
        const auto found = change.backing;
        auto *physical = change.physical;
        Q_ASSERT(found != backings.end());
        const auto before = found->second.budgetClass;
        if (before == change.after) return;
        Q_ASSERT(physical);
        const bool changed = changePhysicalReferenceClass(
            &physical->references, before, change.after);
        Q_ASSERT(changed);
        Q_UNUSED(changed);
        // The claim froze the complete physical group at preparation. Only
        // its final row installs the already admitted charge; intermediate
        // alias classes are not another budget transaction.
        if (change.chargedAfter != KisBackingBudgetClass::Count)
            physical->chargedClass = change.chargedAfter;
        found->second.budgetClass = change.after;
        if (change.idleResult) {
            Q_ASSERT(change.after == KisBackingBudgetClass::IdleCapacity);
            auto *record = found->second.retirement;
            Q_ASSERT(!record || !record->is_linked());
            if (record) record->replica = found->second.replica;
            *change.idleResult = record;
        }
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
    KisMutationStorageAllocator<KisPageProviderOperationNode> operationStorage;
    ProofIndex sealedProofs;
    KisBackingBudgetController *backingBudget = nullptr;
    BackingIndex backings;
    PhysicalBackingIndex physicalBackings;
    // Both waiter registration and all claim transitions use mutex. The wait
    // releases it only after registration, so a zero-waiter skip cannot lose a wake.
    void notifyPhysicalClaimsChanged() noexcept
    { if (physicalWaiters) physicalClaimsChanged.wakeAll(); }
    KisPageWaitCondition physicalClaimsChanged;
    size_t physicalWaiters = 0;
    size_t activeHandoffs = 0;
    size_t activeBorrowedChanges = 0;
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
        if (!owner->backingBudget || record->byteSize() != bytes
            || record->domain != sourceDomain
            || record->domainRevision != sourceRevision
            || record->domainClaim || record->classClaim) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("backing-domain transition source is stale or busy"));
            return {};
        }

        KisBackingBudgetDelta signedDelta;
        if (!addBackingDeltaChecked(&signedDelta, record->chargedClass,
                                    sourceDomain, -qint64(bytes))
            || !addBackingDeltaChecked(&signedDelta, record->chargedClass,
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
        record->domainClaim = cookie;
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
                && record->domainClaim == cookie) {
                record->domainClaim = 0;
            }
        }
        released.swap(m_claims);
        m_pendingRegistrations = nullptr;
        owner->notifyPhysicalClaimsChanged();
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
                && record->domainClaim == cookie) {
                record->domainClaim = 0;
            }
            released = m_claims.extract(claim);
            owner->notifyPhysicalClaimsChanged();
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
            ? kisCompareBackingRevision(targetRevision, record->domainRevision)
            : KisBackingRevisionOrder::Ambiguous;
        Q_ASSERT(record != owner->physicalBackings.end()
                 && record->domainClaim == cookie
                 && record->domain == claim->second.sourceDomain
                 && record->domainRevision == claim->second.sourceRevision
                 && revisionOrder == KisBackingRevisionOrder::Newer);
        if (record != owner->physicalBackings.end()
            && record->domainClaim == cookie
            && record->domain == claim->second.sourceDomain
            && record->domainRevision == claim->second.sourceRevision
            && revisionOrder == KisBackingRevisionOrder::Newer) {
            owner->backingBudget->commitReservation(
                std::move(claim->second.reservation), claim->second.signedDelta);
            owner->moveRetirementHeadroom((*record), claim->second.targetDomain);
            record->domain = claim->second.targetDomain;
            record->domainRevision = targetRevision;
            record->domainClaim = 0;
        }
        released = m_claims.extract(claim);
        owner->notifyPhysicalClaimsChanged();
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
    , m_cpuOptionalFee(std::exchange(other.m_cpuOptionalFee, false))
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
    m_cpuOptionalFee = std::exchange(other.m_cpuOptionalFee, false);
    return *this;
}

void KisBackingHandoffReservation::release() noexcept
{
    if (!m_owner) return;
    auto *owner = std::exchange(m_owner, nullptr);
    QMutexLocker lock(&owner->d->mutex);
    Q_ASSERT(m_physical->classClaim == HandoffClaim && owner->d->activeHandoffs);
    if (std::exchange(m_cpuOptionalFee, false))
        owner->d->backingBudget->settleCpuOptionalHandoff(
            m_source->second.budgetClass, m_physical->byteSize(), false);
    m_physical->classClaim = 0;
    --owner->d->activeHandoffs;
    m_budget.release();
    owner->d->notifyPhysicalClaimsChanged();
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
    , m_rows(std::exchange(other.m_rows, nullptr))
    , m_publicationRows(std::exchange(other.m_publicationRows, {}))
    , m_count(std::exchange(other.m_count, 0))
    , m_budget(std::move(other.m_budget))
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
        m_rows = std::exchange(other.m_rows, nullptr);
        m_publicationRows = std::exchange(other.m_publicationRows, {});
        m_count = std::exchange(other.m_count, 0);
        m_budget = std::move(other.m_budget);
    }
    return *this;
}

bool KisBackingClassChangeReservation::isValid() const
{
    return m_owner;
}

void KisBackingClassChangeReservation::release() noexcept
{
    if (m_owner && m_rows) m_owner->finishBorrowedBackingChanges(*this, false);
    else if (m_owner && m_cookie) m_owner->cancelPreparedBackingChanges(m_cookie);
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
    d->detachRetirementIndexes();
}

void KisPageOwnerLedger::detachRetirementIndexesForDestruction() noexcept
{
    // Root teardown has excluded callbacks and external capabilities. End
    // ledger borrows before pending/abandoned queue records are destroyed.
    if (d->backings.empty() && d->operations.empty()) return;
    d->domainAdmission->detach();
    QMutexLocker lock(&d->mutex);
    d->detachRetirementIndexes();
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
    d->operationStorage = KisMutationStorageAllocator<KisPageProviderOperationNode>(&budget);
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
try {
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
    if (!*preparedRetirement || (*preparedRetirement)->backingNode.is_linked()
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

    KisPagePhysicalBackingPointer unusedPhysical;
    QMutexLocker locker(&d->mutex);
    const BackingKey key = replica.allocationIdentity();
    const PhysicalBackingKey physicalKey = physicalBackingKey(replica, footprint);
    const auto validRegistration = [&] {
        if (!d->backingBudget || reservation.owner != d->backingBudget
            || d->backings.find(key) != d->backings.end() || d->isHandoffTarget(key)) {
            KisPageStoreDetail::setError(error, QStringLiteral("replica backing identity is already owned"));
            return false;
        }
        const auto &physical = (*preparedRetirement)->physicalStorage;
        if (physical && physical->storage != (*preparedRetirement)->storage) {
            KisPageStoreDetail::setError(error, QStringLiteral("backing installation storage is foreign"));
            return false;
        }
        return true;
    };
    if (!validRegistration()) return false;
    auto physical = d->physicalBackings.find(physicalKey);
    if (physical == d->physicalBackings.end() && !(*preparedRetirement)->physicalStorage) {
        // Fresh arrived with this capacity before its provider call. Only an
        // actual first alias/direct adoption needs late installation storage;
        // its already funded terminal keeps refusal recoverable. Never do the
        // allocation or provider observation under the ledger gate.
        locker.unlock();
        try { (*preparedRetirement)->preparePhysicalStorage(); }
        catch (const std::bad_alloc &) {
            KisPageStoreDetail::setError(error, QStringLiteral("backing installation storage was refused"));
            return false;
        }
        footprint = observeBackingFootprint(replica);
        if (!footprint.isValid() || footprint.bytes != replica.layout.byteSize
            || !(physicalBackingKey(replica, footprint) == registrationPhysical)) {
            KisPageStoreDetail::setError(error, QStringLiteral("provider physical backing changed during storage preparation"));
            return false;
        }
        locker.relock();
        if (!validRegistration()) return false;
        physical = d->physicalBackings.find(physicalKey);
    }
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
        auto node = std::move((*preparedRetirement)->physicalStorage);
        node->identity = physicalKey;
        node->representative = replica;
        node->domain = footprint.domain;
        node->domainRevision = footprint.revision;
        node->references[static_cast<size_t>(budgetClass)] = 1;
        node->chargedClass = budgetClass;
        auto installed = d->physicalBackings.insert(*node);
        Q_ASSERT(installed.second);
        physical = installed.first;
        node.release(); // The original physical index owns this endpoint.
    } else {
        if (physical->domainClaim || physical->classClaim) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("shared physical backing transition is active"));
            return false;
        }
        if (physical->domain != footprint.domain
            || physical->domainRevision != footprint.revision
            || physical->byteSize() != footprint.bytes) {
            KisPageStoreDetail::setError(error, QStringLiteral("shared physical backing footprint changed"));
            return false;
        }
        auto next = physical->references;
        auto &references = next[static_cast<size_t>(budgetClass)];
        if (references == std::numeric_limits<quint32>::max()) {
            KisPageStoreDetail::setError(error, QStringLiteral("shared physical backing reference count overflow"));
            return false;
        }
        ++references;
        sharedAfterCharge = chargedBackingClass(next);
        if (!addPhysicalChargeChange(&sharedChargeChange, *physical,
                                     physical->chargedClass, sharedAfterCharge)
            || (sharedAfterCharge != physical->chargedClass
                && !d->backingBudget->reservationCovers(
                    reservation.cookie, sharedAfterCharge, footprint.domain,
                    footprint.bytes))) {
            KisPageStoreDetail::setError(error, QStringLiteral("shared physical backing class change is not reserved"));
            return false;
        }
        physical->references = next;
        unusedPhysical = std::move((*preparedRetirement)->physicalStorage);
    }
    auto &node = (*preparedRetirement)->backingNode;
    node.first = key;
    node.second.replica = replica;
    node.second.budgetClass = budgetClass;
    const auto installedBacking = d->backings.insert(node);
    Q_ASSERT(installedBacking.second);
    auto &backing = installedBacking.first->second;
    physical->link(backing);
    backing.retirement = preparedRetirement->release();

    if (!newPhysicalBacking) {
        // The same bytes have one charge, but a new foreground alias can
        // outrank the former owner class and must move that charge atomically.
        if (sharedAfterCharge != physical->chargedClass) {
            reservation.commit(sharedChargeChange);
            physical->chargedClass = sharedAfterCharge;
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
} catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("backing observation or installation storage was refused"));
    return false;
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

KisPageRetirementRecordPointer KisPageOwnerLedger::takeRetirementRecord(
    const KisReplicaHandle &replica, KisBackingBudgetClass *originalClass)
{
    return takeRetirementRecordImpl(replica, originalClass, nullptr);
}

KisPageRetirementRecordPointer KisPageOwnerLedger::takeRetirementRecordImpl(
    const KisReplicaHandle &replica, KisBackingBudgetClass *originalClass, bool *registered)
{
    QMutexLocker lock(&d->mutex);
    if (originalClass) *originalClass = KisBackingBudgetClass::Count;
    if (registered) *registered = false;
    const auto found = d->backings.find(replica.allocationIdentity());
    if (found == d->backings.end() || !(found->second.replica == replica)) return {};
    if (registered) *registered = true;
    return takeRetirementRecordLocked(found->second, replica, originalClass);
}

KisPageRetirementRecordPointer KisPageOwnerLedger::takeRetirementRecordLocked(
    KisPageBackingRecord &backing, const KisReplicaHandle &replica,
    KisBackingBudgetClass *originalClass)
{
    // A prepared writer must commit/cancel before transferring this member.
    // Claims on other aliases do not transfer this logical responsibility.
    if (d->isPreparedBacking(backing)) return {};
    KisPageRetirementRecordPointer record(std::exchange(backing.retirement, nullptr));
    if (record) {
        if (originalClass) *originalClass = backing.budgetClass;
        const auto registered = d->registeredProvider(providerKey(replica.provider, replica.providerEpoch));
        Q_ASSERT(registered);
        record->replica = replica;
        record->provider = registered ? registered->provider : std::shared_ptr<KisPageReplicaProvider>{};
        record->backgroundRetirement = registered && registered->backgroundRetirement;
    }
    return record;
}

KisPageRetirementRecordPointer KisPageOwnerLedger::takeRetirementRecord(
    KisPageRetirementRecord &original, const KisReplicaHandle &expected)
{
    // The publication/queue owner already keeps this exact record alive.
    // Its mutable identity and ownership are still consumed under our gate.
    QMutexLocker lock(&d->mutex);
    auto &node = original.backingNode;
    if (!node.is_linked() || original.storage.budget != d->backingBudget
        || node.second.retirement != &original || !(node.second.replica == expected)) return {};
    return takeRetirementRecordLocked(node.second, expected, nullptr);
}

KisPageRetirementRecords KisPageOwnerLedger::takeShutdownRetirementRecords()
{
    QMutexLocker lock(&d->mutex);
    KisPageRetirementRecords records;
    for (auto &entry : d->backings) {
        auto &backing = entry.second;
        auto record = takeRetirementRecordLocked(backing, backing.replica, nullptr);
        if (!record) continue; // The original queue already owns this record.
        records.push_back(*record.release());
    }
    return records;
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
    if (found == d->backings.end() || !found->second.retirement
        || !(found->second.replica == replica) || !d->backingBudget
        || (reservation && reservation->owner != d->backingBudget)
        || d->isPreparedBacking(found->second)) {
        KisPageStoreDetail::setError(error, reservation
            ? QStringLiteral("replica backing is not available for reserved reclassification")
            : QStringLiteral("replica backing is not owned by this store"));
        return false;
    }
    return reclassifyBackingLocked(found, replica, budgetClass, reservation, error);
}

bool KisPageOwnerLedger::reclassifyBackingLocked(KisPageBackingIndex::iterator found,
    const KisReplicaHandle &replica, KisBackingBudgetClass budgetClass,
    KisBackingBudgetReservation *reservation, QString *error)
{
    if (reservation && reservation->owner != d->backingBudget) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing reservation belongs to another budget"));
        return false;
    }
    if (found->second.budgetClass == budgetClass) {
        if (reservation) reservation->release();
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    auto *physical = found->second.physicalRecord;
    Q_ASSERT(physical);
    if (physical->domainClaim || physical->classClaim) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("replica physical backing transition is active"));
        return false;
    }
    auto next = physical->references;
    const KisBackingBudgetClass beforeCharge = physical->chargedClass;
    if (!changePhysicalReferenceClass(&next, found->second.budgetClass, budgetClass)) {
        KisPageStoreDetail::setError(error, QStringLiteral("replica physical class references are invalid"));
        return false;
    }
    const KisBackingBudgetClass afterCharge = chargedBackingClass(next);
    KisBackingBudgetDelta installed;
    if (!addPhysicalChargeChange(&installed, *physical, beforeCharge, afterCharge)) {
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
    physical->references = next;
    physical->chargedClass = afterCharge;
    found->second.budgetClass = budgetClass;
    if (budgetClass != KisBackingBudgetClass::ActivePending)
        found->second.retirementHeadroom.release();
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPageOwnerLedger::RecordAdmission
KisPageOwnerLedger::admitRetirementRecord(KisPageRetirementRecord &record)
{
    QMutexLocker lock(&d->mutex);
    return admitRetirementRecordLocked(record);
}

KisPageOwnerLedger::RecordAdmission
KisPageOwnerLedger::admitRetirementRecordLocked(KisPageRetirementRecord &record)
{
    auto &node = record.backingNode;
    if (!node.is_linked()) return RecordAdmission::Unregistered;
    if (!d->backingBudget || record.storage.budget != d->backingBudget
        || !(node.second.replica == record.replica) || node.second.retirement
        || !node.second.physicalRecord)
        return RecordAdmission::Refused;
    const auto found = d->backings.iterator_to(node);
    if (node.second.budgetClass == KisBackingBudgetClass::RetirementDebt) {
        record.admissionReservation.release();
        record.orphanReservation.release();
        return RecordAdmission::Accepted;
    }
    // Already-Debt retries keep the original no-reclassification path. Only
    // an actual class transition needs to consult outstanding class claims.
    if (d->isPreparedBacking(node.second)) return RecordAdmission::Refused;
    if (record.admissionReservation.isValid()) {
        const bool accepted = reclassifyBackingLocked(found, record.replica,
            KisBackingBudgetClass::RetirementDebt, &record.admissionReservation, nullptr);
        record.admissionReservation.release();
        if (accepted) {
            record.orphanReservation.release();
            return RecordAdmission::Accepted;
        }
    }
    if (record.orphanReservation.isValid() && reclassifyBackingLocked(found, record.replica,
            KisBackingBudgetClass::RetirementDebt, &record.orphanReservation, nullptr))
        return RecordAdmission::Accepted;
    if (!reclassifyBackingLocked(found, record.replica, KisBackingBudgetClass::RetirementDebt,
                                nullptr, nullptr)) return RecordAdmission::Refused;
    record.orphanReservation.release();
    return RecordAdmission::Accepted;
}

KisPageReadinessStatus KisPageOwnerLedger::watchRetirementBudget(
    KisPageRetirementRecord &record, KisPageReadinessCallback notify,
    KisBackingBudgetWaiter *waiter)
{
    QMutexLocker lock(&d->mutex);
    const auto &node = record.backingNode;
    if (!node.is_linked() || !d->backingBudget || record.storage.budget != d->backingBudget
        || !(node.second.replica == record.replica) || node.second.retirement
        || d->isPreparedBacking(node.second) || node.second.retirementHeadroom.isValid())
        return KisPageReadinessStatus::Unavailable;
    const auto *physical = node.second.physicalRecord;
    Q_ASSERT(physical);
    if (physical->domainClaim || physical->classClaim)
        return KisPageReadinessStatus::Unavailable;
    auto next = physical->references;
    if (!changePhysicalReferenceClass(&next, node.second.budgetClass,
                                       KisBackingBudgetClass::RetirementDebt))
        return KisPageReadinessStatus::Unavailable;
    KisBackingBudgetDelta change;
    if (!addPhysicalChargeChange(&change, *physical, physical->chargedClass, chargedBackingClass(next)))
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
    bool more = true;
    while (more) {
        auto batch = d->providersInCut(cut, &cursor);
        more = batch.more;
        for (size_t i = 0; i < batch.providers.size(); ++i) {
            const auto &provider = batch.providers[i];
            if (!provider->mayHaveBackingDomainChanges()) continue;
            const auto changes = provider->backingDomainChanges(budget);
            probes.reserve(probes.size() + changes.size());
            for (const auto &change : changes) probes.push_back({provider, change});
        }
    }
    // No journal work was observed in this cut. There is no state to apply or
    // acknowledge; the ledger's attached budget is immutable for its lifetime.
    if (probes.empty()) {
        KisPageStoreDetail::setError(error, {});
        return true;
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
        if (probe.change.bytes != physical->byteSize()) {
            KisPageStoreDetail::setError(error, QStringLiteral("provider physical backing observation is invalid"));
            return false;
        }
        const auto order = kisCompareBackingRevision(
            probe.change.revision, physical->domainRevision);
        if (order == KisBackingRevisionOrder::Ambiguous) {
            KisPageStoreDetail::setError(error, QStringLiteral("provider backing-domain revision is ambiguous"));
            return false;
        }
        if (order == KisBackingRevisionOrder::Older) {
            probe.acknowledged = true;
            continue;
        }
        if (order == KisBackingRevisionOrder::Same) {
            if (probe.change.domain != physical->domain) {
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
    KisPagePreparedBackingChange *rows, size_t capacity, void *context,
    int (*read)(void *, KisBackingClassChange *), QString *error, KisPagePublicationRows publicationRows)
try {
    if ((capacity && !rows) || !read || (publicationRows && capacity > publicationRows.size())) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing change workspace is invalid"));
        return {};
    }
    if (!synchronizeBackingDomains(error)) return {};
    QMutexLocker locker(&d->mutex);
    if (!d->backingBudget) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing budget is not attached"));
        return {};
    }
    KisBackingClassChange change; // The callback boundary initializes once, never once per row.
    auto next = [&](Private::ResolvedBackingChange *resolved) -> int {
        const int status = read(context, &change);
        return status > 0 ? (d->resolveBackingChange(change, resolved, error) ? 1 : -1) : status;
    };
    return d->prepareBorrowedChangesLocked(this, rows, capacity, next, error, publicationRows);
} catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("backing change storage budget is exhausted"));
    return {};
}

KisBackingClassChangeReservation KisPageOwnerLedger::prepareIdleCapacity(
    KisPagePublicationRows rows, quint64 allowance, QString *error)
try
{
    const size_t count = rows.size();
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
    auto next = [this, rows, count, index = size_t(0), allowance]
                (Private::ResolvedBackingChange *change) mutable -> int {
        while (index < count) {
            auto &row = rows[index++];
            if (!row.detachesSuperseded) continue;
            if (!row.supersededAuthority) return -1;
            const auto &authority = *row.supersededAuthority;
            if (authority.layout.byteSize > allowance) continue;
            const auto found = d->backings.find(authority.allocationIdentity());
            if (!authority.isValid() || found == d->backings.end()
                || !(found->second.replica == authority)) return -1;
            // Metadata detachment is independent of this optional fee choice.
            // Only actual pending backings consume the idle allowance; another
            // legal class neither poisons the whole D nor authorizes reuse.
            if (found->second.budgetClass != KisBackingBudgetClass::ActivePending) continue;
            allowance -= authority.layout.byteSize;
            *change = {found, KisBackingBudgetClass::IdleCapacity, &row.acceptedIdle};
            return 1;
        }
        return 0;
    };
    return d->prepareBorrowedChangesLocked(this, count ? &rows[0].backingPreparation : nullptr,
                                          count, next, error, rows);
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
    const KisReplicaHandle &source, const KisReplicaHandle &target, QString *error,
    KisPageRetirementRecord *originalIdle)
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
    // The original checkout retains this record across the unlocked physical
    // observation. Consume its location under our gate; the same physical
    // class claim below then protects preparation through commit/cancel.
    if (originalIdle && (!originalIdle->backingNode.is_linked()
        || originalIdle->storage.budget != d->backingBudget
        || originalIdle->backingNode.second.retirement != originalIdle
        || originalIdle->backingNode.second.budgetClass != KisBackingBudgetClass::IdleCapacity
        || !(originalIdle->backingNode.first == oldKey))) return {};
    const auto backing = originalIdle ? d->backings.iterator_to(originalIdle->backingNode)
                                     : d->backings.find(oldKey);
    if (!d->backingBudget || backing == d->backings.end()
        || !backing->second.retirement
        || !(backing->second.replica == source)
        || (backing->second.budgetClass != KisBackingBudgetClass::Current
            && backing->second.budgetClass != KisBackingBudgetClass::ActivePending
            && backing->second.budgetClass != KisBackingBudgetClass::OptionalCache
            && backing->second.budgetClass != KisBackingBudgetClass::IdleCapacity)) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing handoff source is not available"));
        return {};
    }
    // Target differs only by +1 in the last ordered component. If registered,
    // it is this node's immediate successor; a second root-to-key search adds
    // no fact. Registration still excludes this target while the claim lives.
    const auto successor = std::next(backing);
    if (successor != d->backings.end() && successor->first == newKey) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing handoff target is already registered"));
        return {};
    }
    auto *physical = backing->second.physicalRecord;
    Q_ASSERT(physical);
    if (physical->domainClaim || physical->classClaim
        || physical->domain != source.domain || !(physical->representative == source)
        || physical->shared()) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing handoff physical owner is shared or busy"));
        return {};
    }
    for (size_t i = 0; i < physical->references.size(); ++i) {
        if (physical->references[i] != (i == size_t(backing->second.budgetClass) ? 1u : 0u)) {
            KisPageStoreDetail::setError(error, QStringLiteral("backing handoff requires one physical owner"));
            return {};
        }
    }
    const bool noFee = backing->second.budgetClass == KisBackingBudgetClass::ActivePending
        && backing->second.retirementHeadroom.isValid();
    const bool cpuOptionalFee = backing->second.budgetClass == KisBackingBudgetClass::OptionalCache
        || backing->second.budgetClass == KisBackingBudgetClass::IdleCapacity;
    KisBackingBudgetReservation reservation;
    if (noFee) {
        if (!d->backingBudget->admitUnchangedBacking(error)) return {};
    } else if (cpuOptionalFee) {
        // Physical/domain/sole-owner checks above establish the entire signed
        // change. Keep its lifetime on this original claim, not another slot.
        if (!d->backingBudget->reserveCpuOptionalHandoff(physical->byteSize(), error)) return {};
    } else {
        KisBackingBudgetDelta change;
        if (!addPhysicalChargeChange(&change, *physical, physical->chargedClass,
                                     KisBackingBudgetClass::ActivePending)
            || (!backing->second.retirementHeadroom.isValid()
                && backing->second.budgetClass != KisBackingBudgetClass::OptionalCache
                && backing->second.budgetClass != KisBackingBudgetClass::IdleCapacity
                && !addBackingDeltaChecked(&change, KisBackingBudgetClass::RetirementDebt,
                                           physical->domain, qint64(physical->byteSize())))) {
            KisPageStoreDetail::setError(error, QStringLiteral("backing handoff delta overflows"));
            return {};
        }
        reservation = d->backingBudget->reserveChange(change, error);
        if (!reservation.isValid()) return {};
    }
    KisBackingHandoffReservation result;
    result.m_source = backing;
    result.m_physical = physical;
    result.m_target = target;
    result.m_budget = std::move(reservation);
    result.m_cpuOptionalFee = cpuOptionalFee;
    physical->classClaim = HandoffClaim;
    ++d->activeHandoffs;
    result.m_owner = this;
    KisPageStoreDetail::setError(error, {});
    return result;
}

bool KisPageOwnerLedger::commitBackingHandoff(
    KisBackingHandoffReservation &&reservation,
    const std::shared_ptr<KisCpuResidentBinding> &binding) noexcept
{
    QMutexLocker locker(&d->mutex);
    if (reservation.m_owner != this) return false;
    const auto found = reservation.m_source;
    auto *source = &found->second;
    auto *physical = reservation.m_physical;
    Q_ASSERT(found != d->backings.end() && &found->second == source
             && physical->classClaim == HandoffClaim);
    const auto before = source->budgetClass;
    const auto &target = reservation.m_target;
    if (before != KisBackingBudgetClass::ActivePending) {
        const bool changed = changePhysicalReferenceClass(
            &physical->references, before, KisBackingBudgetClass::ActivePending);
        Q_ASSERT(changed); Q_UNUSED(changed);
        source->budgetClass = KisBackingBudgetClass::ActivePending;
        physical->chargedClass = KisBackingBudgetClass::ActivePending;
    }

    // Only the final lexicographic component changes, by exactly one. The
    // target was absent at preparation and isHandoffTarget() excludes its
    // registration while this claim is live. No key can lie between these
    // two integers, so every predecessor/successor relation stays intact.
    // Intrusive storage permits this order-preserving key update: keep the
    // original node, iterator and physical membership instead of erasing and
    // reinserting a resource merely because its permission generation changed.
    Q_ASSERT(target.allocation.generation == found->first.allocation.generation + 1);
    Q_ASSERT(std::next(found) == d->backings.end()
        || KisPageBackingIdentityLess{}(target.allocationIdentity(), std::next(found)->first));
    found->first = target.allocationIdentity();
    source->replica = target;
    // Generic/imported capacity may acquire its first native capability here.
    // Keep that original binding through later idle cycles as on native Fresh.
    if (!source->nativeBinding) source->nativeBinding = binding;
    Q_ASSERT(physical->classClaim == HandoffClaim);
    physical->representative = target;

    // Detached optional capacity has no visible value to recover. A failed
    // private write returns to the original funded retirement endpoint.
    if (std::exchange(reservation.m_cpuOptionalFee, false)) {
        d->backingBudget->settleCpuOptionalHandoff(before, physical->byteSize(), true);
    } else if (source->retirementHeadroom.isValid()
        || before == KisBackingBudgetClass::OptionalCache
        || before == KisBackingBudgetClass::IdleCapacity) {
        if (reservation.m_budget.isValid()) reservation.m_budget.commitPrepared();
    } else {
        KisBackingBudgetDelta retained;
        addBackingDelta(&retained, KisBackingBudgetClass::RetirementDebt,
                         physical->domain, qint64(physical->byteSize()));
        reservation.m_budget.commitPrepared(&retained);
        source->retirementHeadroom = std::move(reservation.m_budget);
    }
    physical->classClaim = 0;
    --d->activeHandoffs;
    reservation.m_owner = nullptr;
    d->notifyPhysicalClaimsChanged();
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

    d->commitProjectedChanges(slot->changes, slot->changes.data(), slot->reservation);
    d->finishPreparedChange(*slot, cookie, true);
}

void KisPageOwnerLedger::cancelPreparedBackingChanges(quint64 cookie) noexcept
{
    if (!cookie) return;
    QMutexLocker locker(&d->mutex);
    PreparedBackingChangeSlot *slot = d->preparedChange(cookie);
    if (!slot) return;
    d->finishPreparedChange(*slot, cookie);
}

void KisPageOwnerLedger::finishBorrowedBackingChanges(KisBackingClassChangeReservation &reservation, bool commit) noexcept
{
    QMutexLocker locker(&d->mutex);
    BorrowedBackingChanges rows(reservation.m_rows, reservation.m_publicationRows, reservation.m_count, reservation.m_count);
    if (commit) d->commitProjectedChanges(rows, reservation.m_rows, reservation.m_budget);
    d->finishProjectedChanges(rows, reservation.m_rows, reservation.m_budget, commit);
    Q_ASSERT(d->activeBorrowedChanges);
    --d->activeBorrowedChanges;
    reservation.m_rows = nullptr;
    reservation.m_publicationRows = {};
    reservation.m_count = 0;
    reservation.m_owner = nullptr;
}

void KisPageOwnerLedger::commitBackingChanges(
    KisBackingClassChangeReservation &&reservation) noexcept
{
    Q_ASSERT(reservation.m_owner == this);
    if (reservation.m_owner != this)
        return;
    if (reservation.m_rows) {
        finishBorrowedBackingChanges(reservation, true);
    } else {
        const quint64 cookie = std::exchange(reservation.m_cookie, 0);
        reservation.m_owner = nullptr;
        commitPreparedBackingChanges(cookie);
    }
}

bool KisPageOwnerLedger::rollbackBackingRegistration(KisPageRetirementRecord &record) noexcept
{
    KisBackingBudgetReservation releasedHeadroom;
    KisPagePhysicalBackingPointer releasedPhysical;
    QMutexLocker lock(&d->mutex);
    if (record.operationNode.is_linked() || record.retirementOperation.isValid()
        || record.retirementResult.isValid() || !record.backingNode.is_linked()
        || d->isPreparedBacking(record.backingNode.second)) return false;
    return d->settleBacking(record, releasedHeadroom, releasedPhysical);
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
    bool more = true;
    while (more) {
        auto batch = d->providersInCut(cut, &cursor);
        more = batch.more;
        for (size_t i = 0; i < batch.providers.size(); ++i) {
            const auto &candidate = batch.providers[i];
            if (!candidate->capabilities().supports(access)) continue;
            // Route selection remains ambiguous with two suitable providers.
            if (selected) return {};
            selected = candidate;
        }
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
            d->operationStorage = KisMutationStorageAllocator<KisPageProviderOperationNode>::retained(d->backingBudget);
        auto *node = d->operationStorage.allocate(1);
        new (node) KisPageProviderOperationNode();
        node->first = operation.value;
        node->second.completion = result.completion;
        const auto inserted = d->operations.insert(*node);
        Q_ASSERT(inserted.second);

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

KisPageOwnerLedger::RecordAdmission
KisPageOwnerLedger::prepareRetirementOperation(KisPageRetirementRecord &record,
                                               KisPageOperationId *operation)
{
    *operation = {};
    QMutexLocker lock(&d->mutex);
    const auto admission = admitRetirementRecordLocked(record);
    if (admission == RecordAdmission::Refused
        || (admission == RecordAdmission::Unregistered
            && record.orphanReservation.isValid() && !record.orphanDebtAdmitted)
        || !d->completions || record.operationNode.is_linked()) return admission;
    *operation = KisPageStoreDetail::allocateMonotonicId<KisPageOperationId>(&s_nextOperationId);
    if (!operation->isValid()) return admission;
    record.operationNode.first = operation->value;
    record.operationNode.second = {{}, &record};
    const auto inserted = d->operations.insert(record.operationNode);
    Q_ASSERT(inserted.second);
    return admission;
}

void KisPageOwnerLedger::cancelRetirementOperation(KisPageRetirementRecord &record)
{
    QMutexLocker lock(&d->mutex);
    auto &node = record.operationNode;
    Q_ASSERT(node.is_linked() && node.second.retirement == &record && !node.second.completion.isValid());
    if (node.is_linked() && node.second.retirement == &record && !node.second.completion.isValid())
        d->operations.erase(d->operations.iterator_to(node));
}

KisVerifiedCompletion KisPageOwnerLedger::observeRetirementOperation(KisPageRetirementRecord &record)
{
    std::shared_ptr<KisCompletionRegistry> registry;
    KisCompletionTicket ticket;
    {
        QMutexLocker lock(&d->mutex);
        auto &node = record.operationNode;
        if (!d->completions || !node.is_linked() || node.second.retirement != &record
            || node.first != record.retirementOperation.value) return {};
        if (record.retirementResult.isValid()) {
            const auto &result = record.retirementResult;
            if (node.second.completion.isValid() || !(result.replica == record.replica)
                || !(result.operation == record.retirementOperation)
                || !d->registeredProvider(providerKey(result.replica.provider, result.replica.providerEpoch)))
                return {}; // Accepted but mismatched: never reissue or refund.
            node.second.completion = result.completion;
            record.retirementResult = {};
        }
        ticket = node.second.completion;
        registry = d->completions;
    }
    return ticket.isValid() ? registry->verifyTerminal(ticket) : KisVerifiedCompletion{};
}

bool KisPageOwnerLedger::consumeRetirementOperation(KisPageRetirementRecord &record,
                                                   const KisVerifiedCompletion &terminal)
{
    KisBackingBudgetReservation releasedHeadroom;
    KisPagePhysicalBackingPointer releasedPhysical;
    QMutexLocker lock(&d->mutex);
    auto &node = record.operationNode;
    if (!terminal.isValid() || !node.is_linked() || node.second.retirement != &record
        || node.first != record.retirementOperation.value
        || !(node.second.completion == terminal.ticket())) return false;
    if (terminal.succeeded() && record.backingNode.is_linked()
        && !d->settleBacking(record, releasedHeadroom, releasedPhysical)) return false;
    d->operations.erase(d->operations.iterator_to(node));
    record.retirementOperation = {};
    return true; // Failed/cancelled consumes only this attempt, retaining backing.
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
    const KisCompletionTicket completion = d->operationCompletion(operation, &completions, false);
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
    if (!d->completions || operationIt == d->operations.end() || operationIt->second.retirement ||
        !(operationIt->second.completion == completion)) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider operation changed before release"));
        return false;
    }
    auto *node = &*operationIt;
    d->operations.erase(operationIt);
    std::destroy_at(node);
    d->operationStorage.deallocate(node, 1);
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPageReadinessStatus KisPageOwnerLedger::watchRetirementOperation(
    const KisPageRetirementRecord &record, KisPageReadinessCallback notify,
    KisPageReadinessSubscription *subscription) const
{
    std::shared_ptr<KisCompletionRegistry> registry;
    KisCompletionTicket ticket;
    {
        QMutexLocker lock(&d->mutex);
        const auto &node = record.operationNode;
        if (!node.is_linked() || node.second.retirement != &record
            || node.first != record.retirementOperation.value) return KisPageReadinessStatus::Unavailable;
        ticket = node.second.completion;
        registry = d->completions;
    }
    return registry && ticket.isValid()
        ? registry->watchTerminal(ticket, std::move(notify), subscription)
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

KisPreparedPageProof KisPageOwnerLedger::exportPublication(const KisPagePublicationIdentity &identity) const
{
    if (!identity.isValid()) return {};
    QMutexLocker lock(&d->mutex);
    if (!identity.native) {
        const auto found = d->sealedProofs.find(identity.stamp, ProofLess{});
        if (found != d->sealedProofs.end()) {
            const auto &proof = found->proof;
            return proof.authority.version == identity.version && proof.transaction == identity.transaction
                ? proof : KisPreparedPageProof{};
        }
    }
    auto *metadata = d->cpuMetadata;
    const auto issuer = d->cpuProofIssuer;
    lock.unlock();
    return metadata ? metadata->cpuProof(identity, issuer) : KisPreparedPageProof{};
}

bool KisPageOwnerLedger::validatePublicationIdentity(const KisPageMetadataCoordinator &metadata,
    const KisPagePublicationIdentity &identity) const
try
{
    if (!identity.native) {
        const auto proof = exportPublication(identity);
        auto descriptor = metadata.versionDescriptor(identity.version);
        return descriptor && validatePreparedPage(metadata, proof, *descriptor);
    }
    quint64 issuer;
    {
        QMutexLocker lock(&d->mutex);
        if (d->cpuMetadata != &metadata) return false;
        issuer = d->cpuProofIssuer;
    }
    std::shared_ptr<const KisPageAllocationDescriptor> descriptor;
    std::shared_ptr<KisCpuResidentBinding> binding;
    KisReplicaHandle authority;
    if (!metadata.cpuPreparedAuthority(identity, issuer, &authority, &descriptor, &binding)
        || !descriptor || !descriptor->isValid() || !authority.layout.matches(*descriptor)) return false;
    // This is the real dynamic boundary. Internal validation consumes the
    // original authority; the immutable completion belongs to public export.
    const auto authorityProvider = provider(authority.provider, authority.providerEpoch);
    return authorityProvider && authorityProvider->validateCpuBinding(authority, *descriptor, binding);
}
catch (const std::bad_alloc &) { return false; }

bool KisPageOwnerLedger::releasePublicationIdentity(const KisPagePublicationIdentity &identity)
{
    if (!identity.native)
        return revokePreparedPage(exportPublication(identity));
    QMutexLocker lock(&d->mutex);
    auto *metadata = d->cpuMetadata;
    const auto issuer = d->cpuProofIssuer;
    lock.unlock();
    return metadata && metadata->revokeCpuProof(identity, issuer);
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

bool KisPageOwnerLedger::validatePrivateWrites(KisPagePublicationRows rows,
    KisPageTransactionId transaction, QString *error, bool complete)
{
    if (!transaction.isValid() || (rows.size() && !rows)) return false;
    size_t pending = 0;
    const KisPagePrivateWrite *firstPending = nullptr;
    for (const auto &row : rows) {
        if (row.replacement.removal) continue;
        const auto *result = row.replacement.native;
        if (!result || !result->reserved || (!result->completed && !complete)
            || !result->location.page || !result->location.shard) return false;
        if (result->proof().isValid()) {
            if (!(result->proof().transaction == transaction)) return false;
        } else { ++pending; if (!firstPending) firstPending = result; }
    }
    if (!pending) { KisPageStoreDetail::setError(error, {}); return true; }

    KisPageMetadataCoordinator *metadata;
    quint64 issuer, stamp;
    std::shared_ptr<KisPageReplicaProvider> authorityProvider;
    ProviderKey selected = providerKey(firstPending->target().provider, firstPending->target().providerEpoch);
    {
        QMutexLocker locker(&d->mutex);
        metadata = d->cpuMetadata;
        issuer = d->cpuProofIssuer;
        const auto registered = d->registeredProvider(selected);
        if (registered) authorityProvider = registered->provider;
        if (!metadata || !d->nextValidationStamp
            || pending > std::numeric_limits<quint64>::max() - d->nextValidationStamp) return false;
        stamp = d->nextValidationStamp;
        d->nextValidationStamp += pending;
    }
    // This is an original frozen input, not an allocated proof/authority
    // projection. Issuance is batch-local; unused stamps on refusal remain
    // consumed, just as they did for a refused individual validation.
    for (auto &row : rows) {
        auto *result = row.replacement.native;
        if (row.replacement.removal || result->proof().isValid()) continue;
        const auto key = providerKey(result->target().provider, result->target().providerEpoch);
        if (!authorityProvider || key != selected) {
            authorityProvider = provider(result->target().provider, result->target().providerEpoch);
            selected = key;
        }
        if (!result->descriptor() || !authorityProvider
            || !authorityProvider->validateCpuBinding(result->target(), *result->descriptor(), result->binding())) {
            KisPageStoreDetail::setError(error, QStringLiteral("CPU private authority failed provider validation"));
            return false;
        }
        if (!metadata->acceptPrivateValidation(*result, transaction, stamp++, issuer, complete)) {
            KisPageStoreDetail::setError(error, QStringLiteral("CPU original result changed during validation"));
            return false;
        }
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

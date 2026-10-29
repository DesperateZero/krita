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
// Only physical handoff escapes the ledger gate; class rows are local scratch.
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

    bool isHandoffBacking(const BackingRecord &record) const noexcept
    {
        Q_ASSERT(record.physicalRecord);
        // Synchronous class acceptance never exposes its temporary rows to
        // another ledger operation. Only physical handoff survives this gate.
        return record.physicalRecord->classClaim == HandoffClaim;
    }

    // Caller owns the original record and holds mutex. Deferred storage and
    // headroom destruction belong to its lock-free exit, including claim waits.
    bool settleBacking(KisPageRetirementRecord &original,
                       KisBackingBudgetReservation &releasedHeadroom,
                       KisPagePhysicalBackingPointer &releasedPhysical) noexcept
    {
        auto &node = original.backingNode;
        if (!node.is_linked() || original.storage.budget != backingBudget
            || node.second.retirement) return false;
        Q_ASSERT(!isHandoffBacking(node.second));
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
        physical->removeClass(record.budgetClass);
        const KisBackingBudgetClass afterCharge = physical->currentClass();
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
        if (source.chargedClass == after) return true;
        last.changedCharge = after;
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
    // window; no accepted location is retained as a later fee capability.
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
        Rows &projected, QString *error)
    {
        // Original caller storage carries accepted locations through commit/cancel.
        // The original ledger gate keeps these map nodes stable through install.
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
            if (physical->domainClaim || physical->classClaim == HandoffClaim) {
                KisPageStoreDetail::setError(error, QStringLiteral("physical backing transition is active"));
                return false;
            }
            if (!alias && physical->classClaim) {
                KisPageStoreDetail::setError(error, QStringLiteral("backing is classified more than once"));
                return false;
            }
            // Record before claiming so allocation failure uses the same rollback.
            projected.push_back({found, physical, projected.size(), change.after,
                                 KisBackingBudgetClass::Count, change.idleResult});
            auto &row = projected[projected.size() - 1];
            if (!alias) {
                physical->classClaim = &row;
                Q_ASSERT(physical->chargedClass == found->second.budgetClass);
                Q_ASSERT(physical->classes().count(found->second.budgetClass) == 1);
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
            if (projected[first].changedCharge != KisBackingBudgetClass::Count) {
                source->classClaim = &projected[first++];
                continue;
            }
            Q_ASSERT(!source->classClaim && !source->domainClaim);
            source->classClaim = &projected[first];
            auto next = source->classes();
            BackingRecord *credit = nullptr;
            do {
                auto &entry = projected[first];
                if (!credit && entry.backing->second.retirementHeadroom.isValid())
                    credit = &entry.backing->second;
                if (!next.change(entry.backing->second.budgetClass, entry.after)) {
                    KisPageStoreDetail::setError(error, QStringLiteral("physical backing class references are invalid"));
                    return false;
                }
                ++first;
            } while (first < projected.size() && projected[first].physical == source);
            if (!projectPhysicalCharge(signedDelta, projected[first - 1], next.chargedClass(), credit, error)) return false;
        }
        return true;
    }

    BackingAcceptance acceptRetirementDebtLocked(
        const KisPageTransitionEffect *effects, qsizetype count,
        void *context, bool (*install)(void *), QString *error)
    {
        // The actual producer supplies retirement effects only. Resolve each
        // exact identity once and retain its original location in this window;
        // duplicate effects are one role change, not a second transaction.
        ChargedVector<ResolvedBackingChange> selected{KisMutationStorageAllocator<ResolvedBackingChange>(backingBudget)};
        for (qsizetype i = 0; i < count; ++i) {
            const auto &replica = effects[i].replica;
            const auto found = backings.find(replica.allocationIdentity());
            if (!replica.isValid() || found == backings.end()
                || !(found->second.replica == replica) || !found->second.retirement) {
                KisPageStoreDetail::setError(error, QStringLiteral("retirement effect has no registered backing and original record"));
                return BackingAcceptance::FeesRefused;
            }
            if (found->second.budgetClass == KisBackingBudgetClass::RetirementDebt) continue;
            if (selected.empty()) selected.reserve(size_t(count));
            selected.push_back({found, KisBackingBudgetClass::RetirementDebt, nullptr});
        }
        const KisPageBackingIdentityLess less;
        std::sort(selected.begin(), selected.end(), [&](const auto &a, const auto &b) {
            return less(a.backing->first, b.backing->first);
        });
        selected.erase(std::unique(selected.begin(), selected.end(), [](const auto &a, const auto &b) {
            return a.backing == b.backing;
        }), selected.end());
        auto next = [position = selected.cbegin(), end = selected.cend()](ResolvedBackingChange *change) mutable -> int {
            if (position == end) return 0;
            *change = *position++;
            return 1;
        };
        Q_ASSERT(classChanges.empty());
        const auto clear = qScopeGuard([&] { classChanges.clear(); });
        return acceptProjectedChanges(next, selected.size(), classChanges, context, install, error);
    }

    template<class NextChange, class Rows>
    bool prepareProjectedChanges(ResolvedBackingChange first, NextChange &next, size_t maximum,
        Rows &rows, KisBackingBudgetReservation &reservation, QString *error)
    {
        KisBackingBudgetDelta delta;
        if (!buildPhysicalBackingDelta(first, next, maximum, &delta, rows, error)) return false;
        if (hasBackingDelta(delta)) {
            reservation = backingBudget->reserveChange(delta, error);
            if (!reservation.isValid()) return false;
        } else if (!backingBudget->admitUnchangedBacking(error)) return false;
        KisPageStoreDetail::setError(error, {});
        return true;
    }

    template<class NextChange, class Rows>
    BackingAcceptance acceptProjectedChanges(NextChange &next, size_t capacity,
        Rows &rows, void *context, bool (*install)(void *), QString *error)
    {
        ResolvedBackingChange first;
        const int status = next(&first);
        if (status < 0) {
            KisPageStoreDetail::setError(error, QStringLiteral("backing change source is invalid"));
            return BackingAcceptance::FeesRefused;
        }
        KisBackingBudgetReservation reservation;
        bool committed = false;
        const auto finish = qScopeGuard([&] { finishProjectedChanges(rows, reservation, committed); });
        if (status ? !prepareProjectedChanges(first, next, capacity, rows, reservation, error)
                   : !backingBudget->admitUnchangedBacking(error)) return BackingAcceptance::FeesRefused;
        if (!install(context)) return BackingAcceptance::InstallRejected;
        commitProjectedChanges(rows, reservation);
        committed = true;
        KisPageStoreDetail::setError(error, {});
        return BackingAcceptance::Accepted;
    }

    template<class Rows>
    void finishProjectedChanges(Rows &rows, KisBackingBudgetReservation &reservation,
                                bool committed = false) noexcept
    {
        for (const auto &row : rows) {
            // Commit the complete fee before releasing headroom or the claim.
            if (committed && row.after != KisBackingBudgetClass::ActivePending)
                row.backing->second.retirementHeadroom.release();
            Q_ASSERT(row.physical && row.physical->classClaim != HandoffClaim);
            row.physical->classClaim = nullptr;
        }
        reservation.release();
        // No thread can observe these synchronous claims, so they cannot
        // acquire waiters. Handoff/domain exits notify their real waiters.
    }

    template<class Rows>
    void commitProjectedChanges(Rows &rows, KisBackingBudgetReservation &reservation) noexcept
    {
        for (const auto &row : rows) {
            Q_ASSERT(row.physical->classClaim && row.physical->classClaim != HandoffClaim);
            installBackingChange(row);
            if (row.credit) row.credit->retirementHeadroom.commit(retirementCredit(row));
        }
        if (reservation.isValid()) reservation.commitPrepared();
    }

    void installBackingChange(const PreparedBackingChange &change) noexcept
    {
        const auto found = change.backing;
        auto *physical = change.physical;
        Q_ASSERT(found != backings.end());
        const auto before = found->second.budgetClass;
        if (before == change.after) return;
        Q_ASSERT(physical);
        const bool changed = physical->changeClass(before, change.after);
        Q_ASSERT(changed);
        Q_UNUSED(changed);
        // Reference-only changes leave the physical fee untouched.
        if (change.changedCharge != KisBackingBudgetClass::Count)
            physical->chargedClass = change.changedCharge;
        found->second.budgetClass = change.after;
        if (change.idleResult) {
            Q_ASSERT(change.after == KisBackingBudgetClass::IdleCapacity);
            auto *record = found->second.retirement;
            Q_ASSERT(!record || !record->is_linked());
            *change.idleResult = record;
        }
    }

    void moveRetirementHeadroom(const PhysicalBackingRecord &physical,
                                KisPageAccessDomain target) noexcept
    {
        // The original member carrying headroom is already known. A logical
        // generation change never rebuilds a representative or looks it up.
        Q_ASSERT(physical.accountMember);
        auto &headroom = physical.accountMember->retirementHeadroom;
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
    ChargedVector<PreparedBackingChange> classChanges;
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
        // A marker excludes a residency change, not another logical alias.
        // The ledger gate serializes membership and the one physical charge.
        // Keep every registering call's own marker until its scope exits so
        // returning one cannot expose a still active registration to a move.
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
    d->classChanges = ChargedVector<PreparedBackingChange>(
        KisMutationStorageAllocator<PreparedBackingChange>(&budget));
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
            error, QStringLiteral("physical backing registration authority is detached"));
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

    KisPageWideClassCountsPointer preparedClasses; // Destroy outside the ledger gate.
    QMutexLocker locker(&d->mutex);
    const BackingKey key = replica.allocationIdentity();
    const PhysicalBackingKey physicalKey = physicalBackingKey(replica, footprint);
    for (;;) {
        if (!d->backingBudget || reservation.owner != d->backingBudget
            || d->backings.find(key) != d->backings.end() || d->isHandoffTarget(key)) {
            KisPageStoreDetail::setError(error, QStringLiteral("replica backing identity is already owned"));
            return false;
        }
        auto physical = d->physicalBackings.find(physicalKey);
        const bool newPhysicalBacking = physical == d->physicalBackings.end();
        KisBackingBudgetDelta sharedChargeChange;
        KisBackingBudgetClass sharedAfterCharge = KisBackingBudgetClass::Count;
        if (newPhysicalBacking) {
            if ((*preparedRetirement)->physical.is_linked()) {
                KisPageStoreDetail::setError(error, QStringLiteral("original resource account still belongs to another physical backing"));
                return false;
            }
            if (!d->backingBudget->reservationCovers(reservation.cookie,
                                                     budgetClass, footprint.domain,
                                                     footprint.bytes)) {
                KisPageStoreDetail::setError(error, QStringLiteral("physical backing domain is not reserved"));
                return false;
            }
            auto node = (*preparedRetirement)->retainPhysicalEndpoint();
            node->identity = physicalKey;
            node->bytes = footprint.bytes;
            node->domain = footprint.domain;
            node->domainRevision = footprint.revision;
            node->installClasses(KisPageClassCounts::single(budgetClass));
            node->chargedClass = budgetClass;
            auto installed = d->physicalBackings.insert(*node);
            Q_ASSERT(installed.second);
            physical = installed.first;
            node.release(); // The original physical index owns this endpoint.
        } else {
            if (physical->domainClaim || physical->classClaim) {
                if (error) *error = QStringLiteral("shared physical backing transition is active: slot=%1 domain=%2 handoff=%3 charge=%4 incoming=%5")
                    .arg(physicalKey.slot).arg(physical->domainClaim)
                    .arg(physical->classClaim == HandoffClaim)
                    .arg(int(physical->chargedClass)).arg(int(budgetClass));
                return false;
            }
            if (physical->domain != footprint.domain
                || physical->domainRevision != footprint.revision
                || physical->byteSize() != footprint.bytes) {
                KisPageStoreDetail::setError(error, QStringLiteral("shared physical backing footprint changed"));
                return false;
            }
            auto next = physical->classes();
            if (!next.add(budgetClass)) {
                KisPageStoreDetail::setError(error, QStringLiteral("shared physical backing reference count overflow"));
                return false;
            }
            if (next.isWide() && !physical->classStorage && !preparedClasses) {
                // This real alias group outgrew the compact distribution. Prepare
                // its one retained extension before accepting membership. No
                // physical location survives the unlocked allocation window.
                locker.unlock();
                preparedClasses = kisPreparePageWideClassCounts(d->backingBudget);
                footprint = observeBackingFootprint(replica);
                if (!footprint.isValid() || footprint.bytes != replica.layout.byteSize
                    || !(physicalBackingKey(replica, footprint) == registrationPhysical)) {
                    KisPageStoreDetail::setError(error, QStringLiteral("physical backing changed during class capacity preparation"));
                    return false;
                }
                locker.relock();
                continue;
            }
            sharedAfterCharge = next.chargedClass();
            if (!addPhysicalChargeChange(&sharedChargeChange, *physical,
                                         physical->chargedClass, sharedAfterCharge)
                || (sharedAfterCharge != physical->chargedClass
                    && !d->backingBudget->reservationCovers(
                        reservation.cookie, sharedAfterCharge, footprint.domain,
                        footprint.bytes))) {
                KisPageStoreDetail::setError(error, QStringLiteral("shared physical backing class change is not reserved"));
                return false;
            }
            if (next.isWide() && !physical->classStorage) physical->classStorage = std::move(preparedClasses);
            physical->installClasses(next);
        }
        auto &node = (*preparedRetirement)->backingNode;
        node.first = key;
        node.second.replica = replica;
        node.second.budgetClass = budgetClass;
        const auto installedBacking = d->backings.insert(node);
        Q_ASSERT(installedBacking.second);
        auto &backing = installedBacking.first->second;
        physical->link(backing);
        const auto registered = d->registeredProvider(providerKey(replica.provider, replica.providerEpoch));
        Q_ASSERT(registered);
        (*preparedRetirement)->provider = registered->provider;
        (*preparedRetirement)->backgroundRetirement = registered->backgroundRetirement;
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
    }
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
    if (d->isHandoffBacking(backing)) return {};
    KisPageRetirementRecordPointer record(std::exchange(backing.retirement, nullptr));
    if (record) {
        if (originalClass) *originalClass = backing.budgetClass;
        Q_ASSERT(record->replica() == replica && record->provider);
        Q_UNUSED(replica);
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
    if (size_t(budgetClass) >= size_t(KisBackingBudgetClass::Count)) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing target class is invalid"));
        return false;
    }
    QMutexLocker locker(&d->mutex);
    const BackingKey key = replica.allocationIdentity();
    auto found = d->backings.find(key);
    if (found == d->backings.end() || !found->second.retirement
        || !(found->second.replica == replica) || !d->backingBudget
        || (reservation && reservation->owner != d->backingBudget)
        || d->isHandoffBacking(found->second)) {
        KisPageStoreDetail::setError(error, reservation
            ? QStringLiteral("replica backing is not available for reserved reclassification")
            : QStringLiteral("replica backing is not owned by this store"));
        return false;
    }
    return reclassifyBackingLocked(found, budgetClass, reservation, error);
}

bool KisPageOwnerLedger::reclassifyBackingLocked(KisPageBackingIndex::iterator found,
    KisBackingBudgetClass budgetClass,
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
    KisPageClassCounts next;
    const KisBackingBudgetClass beforeCharge = physical->chargedClass;
    if (!physical->projectClassChange(found->second.budgetClass, budgetClass, next)) {
        KisPageStoreDetail::setError(error, QStringLiteral("replica physical class references are invalid"));
        return false;
    }
    const KisBackingBudgetClass afterCharge = next.chargedClass();
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
    physical->installClasses(next);
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
        || node.second.retirement
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
    if (d->isHandoffBacking(node.second)) return RecordAdmission::Refused;
    if (record.admissionReservation.isValid()) {
        const bool accepted = reclassifyBackingLocked(found,
            KisBackingBudgetClass::RetirementDebt, &record.admissionReservation, nullptr);
        record.admissionReservation.release();
        if (accepted) {
            record.orphanReservation.release();
            return RecordAdmission::Accepted;
        }
    }
    if (record.orphanReservation.isValid() && reclassifyBackingLocked(found,
            KisBackingBudgetClass::RetirementDebt, &record.orphanReservation, nullptr))
        return RecordAdmission::Accepted;
    if (!reclassifyBackingLocked(found, KisBackingBudgetClass::RetirementDebt,
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
        || node.second.retirement
        || d->isHandoffBacking(node.second) || node.second.retirementHeadroom.isValid())
        return KisPageReadinessStatus::Unavailable;
    const auto *physical = node.second.physicalRecord;
    Q_ASSERT(physical);
    if (physical->domainClaim || physical->classClaim)
        return KisPageReadinessStatus::Unavailable;
    KisPageClassCounts next;
    if (!physical->projectClassChange(node.second.budgetClass, KisBackingBudgetClass::RetirementDebt, next))
        return KisPageReadinessStatus::Unavailable;
    KisBackingBudgetDelta change;
    if (!addPhysicalChargeChange(&change, *physical, physical->chargedClass, next.chargedClass()))
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

KisPageOwnerLedger::BackingAcceptance KisPageOwnerLedger::acceptPreparedBackingChanges(
    KisPagePreparedBackingChange *rows, size_t capacity, void *reader,
    int (*read)(void *, KisBackingClassChange *),
    void *context, bool (*install)(void *), QString *error)
try {
    if ((capacity && !rows) || !read || !install) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing change workspace is invalid"));
        return BackingAcceptance::FeesRefused;
    }
    QMutexLocker locker(&d->mutex);
    if (!d->backingBudget) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing budget is not attached"));
        return BackingAcceptance::FeesRefused;
    }
    KisBackingClassChange change; // The callback boundary initializes once, never once per row.
    auto next = [&](Private::ResolvedBackingChange *resolved) -> int {
        const int status = read(reader, &change);
        return status > 0 ? (d->resolveBackingChange(change, resolved, error) ? 1 : -1) : status;
    };
    BorrowedBackingChanges projected(rows, {}, capacity);
    return d->acceptProjectedChanges(next, capacity, projected, context, install, error);
} catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("backing change storage budget is exhausted"));
    return BackingAcceptance::FeesRefused;
}

KisPageOwnerLedger::BackingAcceptance KisPageOwnerLedger::acceptPreparedIdleCapacity(
    KisPagePublicationRows rows, quint64 allowance,
    void *context, bool (*install)(void *), QString *error)
try
{
    const size_t count = rows.size();
    if ((count && !rows) || !install) {
        KisPageStoreDetail::setError(error, QStringLiteral("capacity release range is invalid"));
        return BackingAcceptance::FeesRefused;
    }
    QMutexLocker locker(&d->mutex);
    if (!d->backingBudget) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing budget is not attached"));
        return BackingAcceptance::FeesRefused;
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
    auto *scratch = count ? &rows[0].backingPreparation : nullptr;
    BorrowedBackingChanges projected(scratch, rows, count);
    return d->acceptProjectedChanges(next, count, projected, context, install, error);
} catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("backing change storage budget is exhausted"));
    return BackingAcceptance::FeesRefused;
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
        || physical->domain != source.domain || physical->accountMember != &backing->second
        || physical->shared()) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing handoff physical owner is shared or busy"));
        return {};
    }
    // Membership already proved a sole owner. Its compact class is the
    // complete distribution, rather than eight independently checked buckets.
    Q_ASSERT(physical->classCounts == KisPageClassCounts::single(backing->second.budgetClass).packed());
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
        // The original handoff claim already froze sole ownership. Consume
        // that fact directly; no count search or fallible class operation.
        physical->installClasses(KisPageClassCounts::single(KisBackingBudgetClass::ActivePending));
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

bool KisPageOwnerLedger::acceptRetirementDebt(
    const KisPageTransitionEffect *effects, qsizetype count,
    void *context, bool (*install)(void *), QString *error)
try {
    if (count < 0 || (count && !effects) || !install) {
        KisPageStoreDetail::setError(error, QStringLiteral("retirement acceptance input is invalid"));
        return false;
    }
    if (!synchronizeBackingDomains(error)) return false;
    QMutexLocker lock(&d->mutex);
    if (!d->backingBudget) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing budget is not attached"));
        return false;
    }
    return d->acceptRetirementDebtLocked(effects, count, context, install, error) == BackingAcceptance::Accepted;
} catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("retirement acceptance storage budget is exhausted"));
    return false;
}

bool KisPageOwnerLedger::rollbackBackingRegistration(KisPageRetirementRecord &record) noexcept
{
    KisBackingBudgetReservation releasedHeadroom;
    KisPagePhysicalBackingPointer releasedPhysical;
    QMutexLocker lock(&d->mutex);
    if (record.operationNode.is_linked() || record.retirementOperation.isValid()
        || record.retirementResult.isValid() || !record.backingNode.is_linked()
        || d->isHandoffBacking(record.backingNode.second)) return false;
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
            if (node.second.completion.isValid() || !(result.replica == record.replica())
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
    return exported + (metadata ? metadata->sealedCpuValueCount(issuer) : 0);
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
    const quint64 exportStamp = identity.native && d->nextValidationStamp
        && d->nextValidationStamp < std::numeric_limits<quint64>::max() ? d->nextValidationStamp++ : 0;
    lock.unlock();
    return metadata ? metadata->cpuProof(identity, issuer, exportStamp) : KisPreparedPageProof{};
}

bool KisPageOwnerLedger::ownsPublicationIdentity(const KisPagePublicationIdentity &identity) const
{
    if (!identity.native) return exportPublication(identity).isValid();
    QMutexLocker lock(&d->mutex);
    auto *metadata = d->cpuMetadata;
    const auto issuer = d->cpuProofIssuer;
    lock.unlock();
    return metadata && metadata->ownsCpuValue(identity, issuer);
}

bool KisPageOwnerLedger::matchesPublicationIdentity(const KisPagePublicationIdentity &identity,
    const KisPreparedPageProof &proof) const
{
    if (!identity.isValid() || !proof.isValid() || !(identity.version == proof.authority.version)
        || !(identity.transaction == proof.transaction)
        || (!identity.native && identity.stamp != proof.providerValidationStamp)) return false;
    QMutexLocker lock(&d->mutex);
    if (!identity.native) {
        const auto found = d->sealedProofs.find(identity.stamp, ProofLess{});
        if (found != d->sealedProofs.end()) return found->proof == proof;
    }
    auto *metadata = d->cpuMetadata;
    const auto issuer = d->cpuProofIssuer;
    lock.unlock();
    // Matching an external snapshot must not mint a capability. A different
    // valid proof for the same version/transaction is not this selection's
    // original export, and an unexported Value has nothing public to match.
    return metadata && metadata->cpuProof(identity, issuer) == proof;
}

KisReplicaHandle KisPageOwnerLedger::publicationAuthority(const KisPagePublicationIdentity &identity) const
{
    if (!identity.native) return exportPublication(identity).authority;
    QMutexLocker lock(&d->mutex);
    auto *metadata = d->cpuMetadata;
    const auto issuer = d->cpuProofIssuer;
    lock.unlock();
    return metadata ? metadata->cpuValueAuthority(identity, issuer) : KisReplicaHandle{};
}

bool KisPageOwnerLedger::validatePublicationIdentity(const KisPageMetadataCoordinator &metadata,
    const KisPagePublicationIdentity &identity) const
{ return inspectPublicationIdentity(metadata, identity, false); }

bool KisPageOwnerLedger::preparePublicationIdentity(const KisPageMetadataCoordinator &metadata,
    const KisPagePublicationIdentity &identity) const
{ return inspectPublicationIdentity(metadata, identity, true); }

bool KisPageOwnerLedger::inspectPublicationIdentity(const KisPageMetadataCoordinator &metadata,
    const KisPagePublicationIdentity &identity, bool preparation) const
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
    // Public observation checks current binding authority. Internal preparing
    // establishes provider policy; the final Resource window checks revocation.
    const auto authorityProvider = provider(authority.provider, authority.providerEpoch);
    return authorityProvider && (preparation
        ? authorityProvider->prepareCpuPublication(authority, *descriptor, binding)
        : authorityProvider->validateCpuBinding(authority, *descriptor, binding));
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

bool KisPageOwnerLedger::preparePrivatePublication(KisPagePublicationRows rows,
    KisPageTransactionId transaction, QString *error, bool complete)
{
    if (!transaction.isValid() || (rows.size() && !rows)) return false;
    const KisPagePrivateWrite *firstPending = nullptr;
    for (const auto &row : rows) {
        if (row.replacement.removal) continue;
        const auto *result = row.replacement.native;
        if (!result || !result->reserved || (!result->completed && !complete)
            || !result->location.page || !result->location.shard) return false;
        if (result->isSealed()) {
            if (!(result->sealedBy() == transaction)) return false;
        } else if (!firstPending) firstPending = result;
    }
    if (!firstPending) { KisPageStoreDetail::setError(error, {}); return true; }

    KisPageMetadataCoordinator *metadata;
    quint64 issuer;
    std::shared_ptr<KisPageReplicaProvider> authorityProvider;
    ProviderKey selected = providerKey(firstPending->target().provider, firstPending->target().providerEpoch);
    {
        QMutexLocker locker(&d->mutex);
        metadata = d->cpuMetadata;
        issuer = d->cpuProofIssuer;
        const auto registered = d->registeredProvider(selected);
        if (registered) authorityProvider = registered->provider;
        if (!metadata) return false;
    }
    // Provider preparation establishes provenance/layout/custom policy.
    // Final binding identity/revocation is consumed at the actual cut.
    for (auto &row : rows) {
        auto *result = row.replacement.native;
        if (row.replacement.removal || result->isSealed()) continue;
        const auto key = providerKey(result->target().provider, result->target().providerEpoch);
        if (!authorityProvider || key != selected) {
            authorityProvider = provider(result->target().provider, result->target().providerEpoch);
            selected = key;
        }
        if (!result->descriptor() || !authorityProvider
            || !authorityProvider->prepareCpuPublication(result->target(), *result->descriptor(), result->binding())) {
            KisPageStoreDetail::setError(error, QStringLiteral("CPU private authority failed provider validation"));
            return false;
        }
    }
    if (!metadata->freezePrivateValues(rows, transaction, issuer, complete)) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU original results changed during freezing"));
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

/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageWriteCoordinator_p.h"

#include "KisPageMetadataCoordinator.h"
#include "KisPageOwnerLedger.h"
#include "KisPageReplicaProvider.h"
#include "KisPagePublicationCoordinator_p.h"
#include "KisCpuResidentBinding_p.h"
#include "KisPageStoreDiagnostics_p.h"
#include "KisPageStoreReclamation_p.h"
#include "KisPageRetirementRecord_p.h"
#if defined(Q_OS_WIN) || (defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN))
#include "KisPagePlatformStorage_p.h"
#endif

#include <QMutexLocker>
#include <QScopeGuard>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>

#ifdef Q_OS_DARWIN
#include <malloc/malloc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifndef _MSC_VER
#include <cxxabi.h>
#endif

namespace
{

#ifdef _MSC_VER
constexpr size_t initializationGuardStorageBytes = sizeof(std::int32_t);
#else
template<class Result, class Guard>
constexpr size_t initializationGuardBytes(Result (*)(Guard *))
{
    return sizeof(Guard);
}
constexpr size_t initializationGuardStorageBytes =
    initializationGuardBytes(&__cxxabiv1::__cxa_guard_acquire);
#endif

struct ProcessStorageState
{
    static constexpr size_t fixedStorageBytes() noexcept
    {
        // Registry/history/epoch/ledger/provider use nine fixed identities.
        // Original Resource/executor handles and the three local-static
        // initialization guards occupy image storage after their heap dies.
        return sizeof(ProcessStorageState) + KisPageDiagnosticFixedStorageBytes
            + KisPageReclamationFixedStorageBytes
            + 9 * sizeof(std::atomic<quint64>)
            + sizeof(std::shared_ptr<std::pmr::memory_resource>)
            + 3 * initializationGuardStorageBytes;
    }
    std::mutex gate;
    std::mutex creation;
    std::weak_ptr<KisBackingBudgetController> parent;
    quint64 limit = 64 * 1024 * 1024;
    bool productPolicy = false;
    std::atomic<quint64> snapshotSequence{0};
    std::atomic<quint64> liveBytes{fixedStorageBytes()};
    std::atomic<quint64> reservedBytes{0};
    std::atomic<quint64> bytes{fixedStorageBytes()};
    ~ProcessStorageState()
    {
        // Free a remaining expired weak control while the accounting gate is
        // still alive, and while the published weak identity is empty.
        std::weak_ptr<KisBackingBudgetController> released;
        released.swap(parent);
    }
};
ProcessStorageState &processStorage()
{
    static ProcessStorageState state;
    return state;
}
struct ProcessStorageSnapshot
{
    quint64 live = 0;
    quint64 reserved = 0;
    quint64 total = 0;
};
ProcessStorageSnapshot processStorageSnapshot() noexcept
{
    auto &state = processStorage();
    for (;;) {
        const auto before = state.snapshotSequence.load(std::memory_order_acquire);
        if (before & 1) continue;
        const ProcessStorageSnapshot result{
            state.liveBytes.load(std::memory_order_relaxed),
            state.reservedBytes.load(std::memory_order_relaxed),
            state.bytes.load(std::memory_order_relaxed)};
        const auto after = state.snapshotSequence.load(std::memory_order_acquire);
        if (before == after && !(after & 1)
            && result.live <= std::numeric_limits<quint64>::max() - result.reserved
            && result.live + result.reserved == result.total)
            return result;
    }
}
void publishProcessStorageLocked(ProcessStorageState &state,
                                 quint64 live, quint64 reserved, quint64 total) noexcept
{
    Q_ASSERT(live <= std::numeric_limits<quint64>::max() - reserved);
    Q_ASSERT(live + reserved == total);
    state.snapshotSequence.fetch_add(1, std::memory_order_acq_rel);
    state.liveBytes.store(live, std::memory_order_relaxed);
    state.reservedBytes.store(reserved, std::memory_order_relaxed);
    state.bytes.store(total, std::memory_order_relaxed);
    state.snapshotSequence.fetch_add(1, std::memory_order_release);
}
#if !defined(Q_OS_DARWIN) && !defined(Q_OS_WIN) && !defined(Q_OS_UNIX)
void freePhysicalStorage(void *data, size_t alignment) noexcept
{
    if (alignment > alignof(std::max_align_t)) ::operator delete(data, std::align_val_t(alignment));
    else ::operator delete(data);
}
#endif

#if defined(Q_OS_DARWIN)
// Keep fixed native size classes on the common path. Large-cache reuse and
// memalign can return more than malloc_good_size(). Align small payloads inside
// an ordinary prepaid size class; use an exact mapping above those classes.
// Both carry their release facts in the original allocation.
struct PageStorageMapping { void *base; size_t bytes; };
bool pageStorageHasPrefix(size_t bytes, size_t alignment) noexcept
{
    return bytes > 64 * 1024 || alignment > alignof(std::max_align_t);
}
#elif !defined(Q_OS_WIN) && !defined(Q_OS_UNIX)
size_t pageStoragePrefix(size_t alignment) noexcept
{
    return std::max(sizeof(size_t), alignment);
}
#endif

// Caller keeps the original owner and its operation alive across unlocks.
// Both callbacks run under the owner gate. required() revalidates the complete
// request; install(missing) consumes that same-gate fact and constructed
// buckets. It cannot fail or allocate; unrelated unlocks always revalidate.
template<class Table, class Required, class Install>
bool admitMutationRecords(Table &table, QMutexLocker<QMutex> &lock,
                          Required required, Install install, QString *error) try
{
    typename Table::Growth growth;
    const auto dispose = [&] {
        if (!growth.hasStorage()) return;
        if (lock.isLocked()) lock.unlock();
        growth = {};
        lock.relock();
    };
    const auto cleanup = qScopeGuard([&] {
        dispose();
        if (!lock.isLocked()) lock.relock();
    });
    // Unrelated edits invalidate only the snapshot, not admission. Reuse its
    // prepared storage until the original request rejects or can be installed;
    // allocation and rebuild never run while holding the owner gate.
    for (;;) {
        const auto additional = required();
        if (!additional) return false;
        if (table.canInsert(*additional)) {
            install(*additional);
            KisPageStoreDetail::setError(error, {});
            return true;
        }
        if (!growth.hasStorage()) {
            growth = table.planGrowth(*additional);
            lock.unlock();
            growth.allocate();
            lock.relock();
        }
        const auto afterAllocation = required();
        if (!afterAllocation) return false;
        // Another preparer may already have installed sufficient capacity.
        // Reuse it before requiring this candidate's old geometry to match.
        if (table.canInsert(*afterAllocation)) {
            install(*afterAllocation);
            KisPageStoreDetail::setError(error, {});
            return true;
        }
        if (!table.capture(growth, *afterAllocation)) {
            dispose();
            continue;
        }
        lock.unlock();
        growth.build();
        lock.relock();
        const auto current = required();
        if (!current) return false;
        if (table.canInsert(*current) || table.install(growth, *current)) {
            Q_ASSERT(table.canInsert(*current));
            install(*current);
            KisPageStoreDetail::setError(error, {});
            return true;
        }
        // A changed revision can recapture into these same actual arrays.
        // Only changed geometry/insufficient capacity discards them above.
    }
} catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("mutation admission storage is unavailable"));
    return false;
}

constexpr size_t budgetClassCount = static_cast<size_t>(KisBackingBudgetClass::Count);

template<typename DomainBytes>
auto components(const DomainBytes &bytes)
{
    return std::array{bytes.cpuRam, bytes.umaShared, bytes.discreteVram, bytes.ssd};
}

KisPageDomainBytes fromComponents(const std::array<quint64, 4> &values)
{
    return {values[0], values[1], values[2], values[3]};
}

quint64 saturatedAdd(quint64 left, quint64 right)
{
    const quint64 maximum = std::numeric_limits<quint64>::max();
    return right > maximum - left ? maximum : left + right;
}

bool hasDomainDelta(const KisPageDomainByteDelta &delta)
{
    return delta.cpuRam || delta.umaShared || delta.discreteVram || delta.ssd;
}

bool hasPositiveBytes(const KisPageDomainByteDelta &delta)
{
    return delta.cpuRam > 0 || delta.umaShared > 0 || delta.discreteVram > 0 || delta.ssd > 0;
}

quint32 changedBudgetClasses(const KisBackingBudgetDelta &delta) { return delta.changedClasses(); }
quint32 changedBudgetClasses(const KisPageDomainByteDelta &delta)
{
    return hasDomainDelta(delta) ? quint32(1) << size_t(KisBackingBudgetClass::MetadataArena) : 0;
}
const KisPageDomainByteDelta &budgetBucket(const KisBackingBudgetDelta &delta, size_t i)
{
    return delta.bucket(i);
}
const KisPageDomainByteDelta &budgetBucket(const KisPageDomainByteDelta &delta, size_t i)
{
    // Only the original shared-child slot consumes a single bucket.
    Q_ASSERT(i == size_t(KisBackingBudgetClass::MetadataArena));
    Q_UNUSED(i);
    return delta;
}

std::array<quint64, 4> domainLimits(
    const KisPageBackingLimits &limits, KisBackingBudgetClass budgetClass)
{
    switch (budgetClass) {
    case KisBackingBudgetClass::Current:
        return components(limits.residentCurrentBytes);
    case KisBackingBudgetClass::RetainedHistory:
        return components(limits.residentHistoryBytes);
    case KisBackingBudgetClass::ActivePending:
    case KisBackingBudgetClass::InFlight:
    case KisBackingBudgetClass::RetirementDebt:
    case KisBackingBudgetClass::OptionalCache:
    case KisBackingBudgetClass::IdleCapacity:
    case KisBackingBudgetClass::MetadataArena: {
        constexpr auto maximum = std::numeric_limits<quint64>::max();
        return {maximum, maximum, maximum, maximum};
    }
    case KisBackingBudgetClass::Count:
        break;
    }
    return {};
}

std::array<quint64, budgetClassCount> aggregateLimits(const KisPageBackingLimits &limits)
{
    return {limits.logicalCurrentBytes, limits.retainedHistoryBytes,
            limits.activePendingBytes, limits.inFlightReplicaBytes,
            limits.retirementDebtBytes, limits.optionalCacheBytes,
            limits.idleCapacityBytes, limits.metadataArenaBytes};
}

quint64 sumComponents(const std::array<quint64, 4> &values)
{
    quint64 result = 0;
    for (quint64 value : values)
        result = saturatedAdd(result, value);
    return result;
}

quint64 optionalBytes(const KisPageBackingUsage &usage,
                      const std::array<quint64, budgetClassCount> &reserved)
{
    quint64 total = 0;
    for (auto purpose : {KisBackingBudgetClass::OptionalCache, KisBackingBudgetClass::IdleCapacity}) {
        const auto &bucket = usage.buckets[size_t(purpose)];
        total = saturatedAdd(total, sumComponents(components(bucket.live)));
        total = saturatedAdd(total, reserved[size_t(purpose)]);
    }
    return total;
}

quint64 optionalDemand(const std::array<quint64, budgetClassCount> &aggregate)
{
    return saturatedAdd(aggregate[size_t(KisBackingBudgetClass::OptionalCache)],
                        aggregate[size_t(KisBackingBudgetClass::IdleCapacity)]);
}

bool fitsBudgetBucket(const KisPageDomainBytes &liveBytes,
    const KisPageDomainBytes &reservedBytes, const KisPageDomainByteDelta &delta,
    const std::array<quint64, 4> &limits, quint64 aggregateBytes,
    quint64 pending, quint64 classLimit)
{
    const auto live = components(liveBytes);
    const auto reserved = components(reservedBytes);
    const auto requested = components(delta);
    for (size_t domain = 0; domain < requested.size(); ++domain) {
        if (requested[domain] < 0) return false;
        const quint64 addition = quint64(requested[domain]);
        const quint64 limit = limits[domain];
        if (addition > limit || live[domain] > limit - addition ||
            reserved[domain] > limit - addition - live[domain]) return false;
    }
    const auto current = sumComponents(live);
    return aggregateBytes <= classLimit && current <= classLimit - aggregateBytes &&
        pending <= classLimit - aggregateBytes - current;
}

bool prepareBudgetChange(const KisBackingBudgetDelta &change, KisBackingBudgetDelta *prepared,
    std::array<quint64, budgetClassCount> *aggregateBytes, quint64 *durableBytes, QString *error)
{
    qint64 durableChange = 0;
    // All callers supply fresh empty output and zero aggregates. Populate
    // only actual input classes; absent classes remain semantic zero.
    Q_ASSERT(!prepared->changedClasses());
    for (quint32 classes = change.changedClasses(); classes; classes &= classes - 1) {
        const size_t i = size_t(qCountTrailingZeroBits(classes));
        const auto &input = change.bucket(i);
        const auto values = components(input);
        std::array<qint64, 4> target;
        qint64 aggregateChange = 0;
        for (size_t domain = 0; domain < values.size(); ++domain)
            target[domain] = std::max(qint64(0), values[domain]);
        for (qint64 value : values) {
            if (__builtin_add_overflow(aggregateChange, value,
                                       &aggregateChange)) {
                KisPageStoreDetail::setError(
                    error, QStringLiteral("backing transition aggregate delta overflows"));
                return false;
            }
        }
        prepared->setBucket(i, {target[0], target[1], target[2], target[3]});
        (*aggregateBytes)[i] = aggregateChange > 0
            ? quint64(aggregateChange) : 0;
        if (i == size_t(KisBackingBudgetClass::IdleCapacity)) {
            // Q covers all reserved destination bytes, including a move
            // between domains, until the original source charge is released.
            quint64 positive = 0;
            for (qint64 value : target) {
                if (__builtin_add_overflow(positive, quint64(value), &positive)) {
                    KisPageStoreDetail::setError(error, QStringLiteral("idle preparation delta overflows"));
                    return false;
                }
            }
            (*aggregateBytes)[i] = positive;
        }
        if (__builtin_add_overflow(durableChange, input.ssd,
                                   &durableChange)) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("backing transition SSD delta overflows"));
            return false;
        }
    }
    *durableBytes = durableChange > 0 ? quint64(durableChange) : 0;
    return true;
}

} // namespace

struct KisBackingBudgetWaitState
{
    KisBackingBudgetDelta requested;
    std::array<quint64, budgetClassCount> aggregateBytes{};
    quint64 durableBytes = 0;
    quint64 cookie = 0;
    quint64 sequence = 0;
    bool granted = false;
    KisPageReadinessCallback notify;
};

struct KisBackingBudgetWaitStateDeleter
{
    KisBackingBudgetController *budget = nullptr;
    void operator()(KisBackingBudgetWaitState *state) const noexcept
    {
        if (!state) return;
        std::destroy_at(state);
        KisMutationStorageAllocator<KisBackingBudgetWaitState>(budget).deallocate(state, 1);
    }
};

using KisBackingBudgetWaitStatePointer =
    std::unique_ptr<KisBackingBudgetWaitState, KisBackingBudgetWaitStateDeleter>;

struct KisBackingBudgetWaitContext
{
    static constexpr size_t Limit = 32;
    using RequestAllocator = KisMutationStorageAllocator<KisBackingBudgetWaitStatePointer>;
    explicit KisBackingBudgetWaitContext(KisBackingBudgetController *budget,
        KisMutationStorageAllocator<KisBackingBudgetWaitContext> storage)
        : requests(RequestAllocator(budget))
        , storage(std::move(storage))
    {
        requests.resize(Limit);
    }
    // Consumer cancellation/worker/destruction take gate before controller's
    // mutex. Producers only take the controller gate and signal the monitor.
    QMutex gate;
    KisBackingBudgetController *owner = nullptr;
    // Separate charged storage can be freed at teardown even when dormant
    // handles/callbacks retain this small inert lifetime anchor.
    std::vector<KisBackingBudgetWaitStatePointer, RequestAllocator> requests;
    quint64 nextSequence = 1;
    bool scheduled = false;
    bool dispatchAgain = false;
    KisPageReclamationJobPointer task;
    KisPageReclamationWake wake;
    KisMutationStorageAllocator<KisBackingBudgetWaitContext> storage;
    std::atomic<quint32> references{0};
};

void intrusive_ptr_add_ref(KisBackingBudgetWaitContext *context) noexcept
{
    context->references.fetch_add(1, std::memory_order_relaxed);
}

void intrusive_ptr_release(KisBackingBudgetWaitContext *context) noexcept
{
    if (context->references.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    auto storage = std::move(context->storage);
    std::destroy_at(context);
    storage.deallocate(context, 1);
}

KisBackingBudgetWaiter::~KisBackingBudgetWaiter() { reset(); }
KisBackingBudgetWaiter::KisBackingBudgetWaiter(KisBackingBudgetWaiter &&other) noexcept
    : context(std::move(other.context))
    , sequence(std::exchange(other.sequence, 0)) {}
KisBackingBudgetWaiter &KisBackingBudgetWaiter::operator=(KisBackingBudgetWaiter &&other) noexcept
{
    if (this != &other) {
        reset();
        context = std::move(other.context);
        sequence = std::exchange(other.sequence, 0);
    }
    return *this;
}
bool KisBackingBudgetWaiter::isValid() const
{
    if (!sequence) return false;
    const auto strongContext = context;
    if (!strongContext) return false;
    QMutexLocker lifetime(&strongContext->gate);
    if (!strongContext->owner) return false;
    QMutexLocker lock(&strongContext->owner->m_mutex);
    for (const auto &state : strongContext->requests)
        if (state && state->sequence == sequence && state->cookie) return true;
    return false;
}
void KisBackingBudgetWaiter::reset()
{
    const quint64 currentSequence = std::exchange(sequence, 0);
    const auto strongContext = std::move(context);
    if (!strongContext || !currentSequence) return;
    QMutexLocker lock(&strongContext->gate);
    if (strongContext->owner) {
        // Cancellation releases the prepared/granted slot while the context
        // gate still excludes destruction of the reservation owner.
        auto reservation = strongContext->owner->finishWaiter(currentSequence, false);
        reservation.release();
    }
}
KisBackingBudgetReservation KisBackingBudgetWaiter::take()
{
    if (!sequence) return {};
    const auto strongContext = context;
    if (!strongContext) return {};
    KisBackingBudgetReservation reservation;
    {
        QMutexLocker lock(&strongContext->gate);
        if (strongContext->owner)
            reservation = strongContext->owner->finishWaiter(sequence, true);
    }
    if (reservation.isValid()) {
        sequence = 0;
        context.reset();
    }
    return reservation;
}

KisBackingBudgetController::~KisBackingBudgetController()
{
    stopMutationStorageReuse();
    if (m_storageOwner) m_storageOwner->stopAllocations();
    if (m_waitContext) {
        {
            decltype(m_waitContext->requests) released;
            {
                QMutexLocker lifetime(&m_waitContext->gate);
                m_waitContext->owner = nullptr;
                m_waitContext->wake.reset();
                QMutexLocker lock(&m_mutex);
                for (auto &state : m_waitContext->requests) if (state) state->cookie = 0;
                released.swap(m_waitContext->requests);
                m_usage.waitingRequests = 0;
                m_usage.grantedRequests = 0;
            }
            // Charged states and the request array are freed without either
            // controller gate, before the lifetime anchor becomes inert.
            for (auto &state : released) state.reset();
        }
        m_waitContext.reset();
    }
    // State destruction above mirrors its metadata release to the parent.
    // Only then may the child identity satisfy the zero-live unregister rule.
    if (m_slotCapacity) {
        m_slots.reset();
    }
    if (m_storageOwner) {
        m_storageOwner->detach(m_sharedNonPayloadBudget,
                              m_sharedNonPayloadChild, m_sharedNonPayloadUsage);
        m_sharedNonPayloadChild = 0;
        m_storageOwner.reset();
    } else if (m_sharedNonPayloadBudget && m_sharedNonPayloadChild) {
        m_sharedNonPayloadBudget->unregisterSharedNonPayloadChild(
            std::exchange(m_sharedNonPayloadChild, 0));
    }
    if (m_processStorage) {
        std::weak_ptr<KisBackingBudgetController> released;
        {
            auto &state = processStorage();
            QMutexLocker gate(&state.gate);
            // A successor may have been published after this root's last
            // strong reference. Never withdraw the successor's identity.
            if (state.parent.expired()) released.swap(state.parent);
        }
    }
}

quint64 KisBackingBudgetController::nonMetadataLiveSsdLocked() const
{
    if (!m_nonMetadataLiveSsdValid) {
        m_nonMetadataLiveSsd = 0;
        for (size_t i = 0; i < size_t(KisBackingBudgetClass::MetadataArena); ++i)
            m_nonMetadataLiveSsd = saturatedAdd(m_nonMetadataLiveSsd, m_usage.buckets[i].live.ssd);
        m_nonMetadataLiveSsdValid = true;
    }
    return m_nonMetadataLiveSsd;
}

quint64 KisBackingBudgetController::storageBytesLocked() const noexcept
{
    return m_storageBytes;
}

bool KisBackingBudgetController::fitsLocked(
    const KisBackingBudgetDelta &delta,
    const std::array<quint64, budgetClassCount> &aggregateBytes,
    quint64 durableBytes, bool emptyUsage,
    quint64 irreducibleMetadataBytes) const
{
    if (m_limits.optionalCacheBytes != std::numeric_limits<quint64>::max()) {
        const quint64 optional = emptyUsage ? 0 : optionalBytes(m_usage, m_reservedAggregateBytes);
        const quint64 demand = optionalDemand(aggregateBytes);
        if (demand > m_limits.optionalCacheBytes || optional > m_limits.optionalCacheBytes - demand)
            return false;
    }
    quint64 liveSsd = emptyUsage ? 0 : nonMetadataLiveSsdLocked();
    const auto process = m_processStorage ? processStorageSnapshot() : ProcessStorageSnapshot{};
    const auto classLimits = aggregateLimits(m_limits);
    for (size_t i = 0; i < budgetClassCount; ++i) {
        const auto budgetClass = static_cast<KisBackingBudgetClass>(i);
        auto live = emptyUsage ? KisPageDomainBytes{} : m_usage.buckets[i].live;
        if (budgetClass == KisBackingBudgetClass::MetadataArena)
            liveSsd = saturatedAdd(liveSsd, live.ssd);
        const quint32 bit = quint32(1) << i;
        const bool metadata = budgetClass == KisBackingBudgetClass::MetadataArena;
        if (!emptyUsage && !metadata && (m_fittingBudgetClasses & bit)
            && !aggregateBytes[i] && !hasDomainDelta(delta.bucket(i))) continue;
        auto reserved = emptyUsage ? KisPageDomainBytes{} : m_usage.buckets[i].reserved;
        // Storage's complete capacity remains occupied through physical free.
        // Admission needs only this total, not the concurrent live split.
        if (!emptyUsage && metadata)
            reserved.cpuRam = saturatedAdd(reserved.cpuRam, storageBytesLocked());
        if (emptyUsage && metadata) live.cpuRam = irreducibleMetadataBytes;
        if (m_processStorage && metadata) {
            live.cpuRam = saturatedAdd(live.cpuRam, process.live);
            reserved.cpuRam = saturatedAdd(reserved.cpuRam, process.reserved);
        }
        auto pending = emptyUsage ? 0 : m_reservedAggregateBytes[i];
        if (!emptyUsage && metadata) pending = saturatedAdd(pending, storageBytesLocked());
        if (m_processStorage && metadata) pending = saturatedAdd(pending, process.reserved);
        if (!fitsBudgetBucket(live, reserved, delta.bucket(i),
                domainLimits(m_limits, budgetClass), aggregateBytes[i], pending, classLimits[i]))
            return false;
        // A fitting nonnegative proposal also proves its present base fits.
        // Hypothetical empty-usage admission cannot certify real usage.
        if (!emptyUsage && !metadata) m_fittingBudgetClasses |= bit;
    }
    const auto limit = m_limits.durableStoreCapacity;
    return durableBytes <= limit && liveSsd <= limit - durableBytes &&
        (emptyUsage ? 0 : m_reservedDurableBytes) <= limit - durableBytes - liveSsd;
}

bool KisBackingBudgetController::fitsMetadataStorageLocked(quint64 bytes, quint64 *processBytes) const
{
    if (bytes > quint64(std::numeric_limits<qint64>::max())) return false;
    constexpr auto metadata = size_t(KisBackingBudgetClass::MetadataArena);
    static_assert(metadata + 1 == budgetClassCount);
    if (m_limits.optionalCacheBytes != std::numeric_limits<quint64>::max()
        && optionalBytes(m_usage, m_reservedAggregateBytes) > m_limits.optionalCacheBytes) return false;
    constexpr quint32 nonMetadataMask = (quint32(1) << metadata) - 1;
    if ((m_fittingBudgetClasses & nonMetadataMask) != nonMetadataMask) {
        const auto classLimits = aggregateLimits(m_limits);
        for (size_t i = 0; i < metadata; ++i) {
            const quint32 bit = quint32(1) << i;
            if (m_fittingBudgetClasses & bit) continue;
            const auto &usage = m_usage.buckets[i];
            if (!fitsBudgetBucket(usage.live, usage.reserved, {},
                    domainLimits(m_limits, static_cast<KisBackingBudgetClass>(i)),
                    0, m_reservedAggregateBytes[i], classLimits[i])) return false;
            m_fittingBudgetClasses |= bit;
        }
    }
    const auto process = m_processStorage ? processStorageSnapshot() : ProcessStorageSnapshot{};
    if (processBytes) *processBytes = process.total;
    const auto &usage = m_usage.buckets[metadata];
    const auto cpuLive = saturatedAdd(usage.live.cpuRam, process.live);
    const auto cpuReserved = saturatedAdd(saturatedAdd(usage.reserved.cpuRam,
        storageBytesLocked()), process.reserved);
    constexpr auto maximum = std::numeric_limits<quint64>::max();
    // Metadata has no per-domain cap below maximum. Check the real CPU
    // increase and existing cross-domain overflow without constructing a
    // four-domain proposal, limits array and generic bucket projection.
    if (cpuLive > maximum - bytes || cpuReserved > maximum - bytes - cpuLive
        || usage.reserved.umaShared > maximum - usage.live.umaShared
        || usage.reserved.discreteVram > maximum - usage.live.discreteVram
        || usage.reserved.ssd > maximum - usage.live.ssd) return false;
    const auto current = saturatedAdd(saturatedAdd(saturatedAdd(cpuLive,
        usage.live.umaShared), usage.live.discreteVram), usage.live.ssd);
    const auto pending = saturatedAdd(saturatedAdd(m_reservedAggregateBytes[metadata],
        storageBytesLocked()), process.reserved);
    const auto classLimit = m_limits.metadataArenaBytes;
    if (bytes > classLimit || current > classLimit - bytes
        || pending > classLimit - bytes - current) return false;
    const auto liveSsd = saturatedAdd(nonMetadataLiveSsdLocked(), usage.live.ssd);
    const auto limit = m_limits.durableStoreCapacity;
    return liveSsd <= limit && m_reservedDurableBytes <= limit - liveSsd;
}

bool KisBackingBudgetController::priorWaiterConflictsLocked(
    const KisBackingBudgetDelta &delta,
    const std::array<quint64, budgetClassCount> &aggregateBytes,
    quint64 durableBytes, quint64 beforeSequence) const
{
    if (!m_usage.waitingRequests) return false;
    const auto classLimits = aggregateLimits(m_limits);
    for (const auto &prior : m_waitContext->requests) {
        if (!prior || prior->granted || prior->sequence >= beforeSequence) continue;
        if (m_limits.optionalCacheBytes != std::numeric_limits<quint64>::max()
            && optionalDemand(aggregateBytes) && optionalDemand(prior->aggregateBytes)) return true;
        if (durableBytes && prior->durableBytes &&
            m_limits.durableStoreCapacity != std::numeric_limits<quint64>::max()) return true;
        for (size_t i = 0; i < budgetClassCount; ++i) {
            const auto budgetClass = static_cast<KisBackingBudgetClass>(i);
            if (aggregateBytes[i] && prior->aggregateBytes[i] &&
                classLimits[i] != std::numeric_limits<quint64>::max()) return true;
            const auto limits = domainLimits(m_limits, budgetClass);
            const auto requested = components(delta.bucket(i));
            const auto waiting = components(std::as_const(prior->requested).bucket(i));
            for (size_t domain = 0; domain < requested.size(); ++domain)
                if (requested[domain] && waiting[domain] &&
                    limits[domain] != std::numeric_limits<quint64>::max()) return true;
        }
    }
    return false;
}

void KisBackingBudgetController::notifyWaitersLocked() noexcept
{
    if (m_usage.waitingRequests) m_waitContext->wake.notify();
}

KisBackingBudgetReservation KisBackingBudgetController::finishWaiter(
    quint64 sequence, bool take)
{
    KisBackingBudgetWaitStatePointer released;
    KisBackingBudgetReservation reservation;
    {
        QMutexLocker lock(&m_mutex);
        for (auto &entry : m_waitContext->requests) {
            if (!entry || entry->sequence != sequence || !entry->cookie
                || (take && !entry->granted)) continue;
            reservation = {
                this, std::exchange(entry->cookie, 0)};
            if (entry->granted) --m_usage.grantedRequests;
            else --m_usage.waitingRequests;
            if (!take) ++m_usage.waiterCancellations;
            released = std::move(entry);
            notifyWaitersLocked();
            break;
        }
    }
    // The budget-backed state is destroyed only after releasing m_mutex.
    return reservation;
}

KisPageReadinessStatus KisBackingBudgetController::waitForChange(
    const KisBackingBudgetDelta &change, KisPageReadinessCallback notify,
    KisBackingBudgetWaiter *waiter, QString *error)
{
    if (!waiter || !notify) return KisPageReadinessStatus::Unavailable;
    waiter->reset();
    KisBackingBudgetDelta delta;
    std::array<quint64, budgetClassCount> aggregate{};
    quint64 durable = 0;
    if (!prepareBudgetChange(change, &delta, &aggregate, &durable, error))
        return KisPageReadinessStatus::Unavailable;
    if (m_sharedNonPayloadBudget
        && hasPositiveBytes(sharedNonPayloadDelta(delta))) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("shared non-payload budget does not support asynchronous waits"));
        return KisPageReadinessStatus::Unavailable;
    }
    const auto dispatch = [](KisBackingBudgetWaitContext *context) {
        return [context] {
            WaitCallbacks callbacks;
            {
                QMutexLocker lifetime(&context->gate);
                if (context->owner) callbacks = context->owner->dispatchWaiters();
            }
            for (auto &callback : callbacks) if (callback) callback();
        };
    };
    const auto wakeDispatch = [](boost::intrusive_ptr<KisBackingBudgetWaitContext> context) {
        return [context = std::move(context)] {
            QMutexLocker lifetime(&context->gate);
            if (!context->owner) return;
            if (context->scheduled) {
                context->dispatchAgain = true;
                return;
            }
            context->scheduled = true;
            intrusive_ptr_add_ref(context.get());
            if (!kisEnqueuePageStoreReclamation(context->task.get())) {
                context->scheduled = false;
                intrusive_ptr_release(context.get());
            }
        };
    };
    bool needsContext = false;
    using DispatchTask = KisPageReclamationTask<decltype(dispatch(nullptr))>;
    const quint64 waiterFixedBytes =
        kisPageStorageAllocationBytes(sizeof(KisBackingBudgetWaitContext), alignof(KisBackingBudgetWaitContext))
        + kisPageStorageAllocationBytes(sizeof(KisBackingBudgetWaitState), alignof(KisBackingBudgetWaitState))
        + kisPageStorageAllocationBytes(KisBackingBudgetWaitContext::Limit * sizeof(KisBackingBudgetWaitStatePointer),
                                        alignof(KisBackingBudgetWaitStatePointer))
        + kisPageStorageAllocationBytes(sizeof(KisPageReclamationWakeState), alignof(KisPageReclamationWakeState))
        + kisPageStorageAllocationBytes(sizeof(KisMutationStorageOwner), alignof(KisMutationStorageOwner))
        + kisPageStorageAllocationBytes(sizeof(DispatchTask), alignof(DispatchTask))
        + KisPageReadinessCallback::storageBytesFor<decltype(wakeDispatch({}))>()
        + notify.storageBytes();
    {
        QMutexLocker lock(&m_mutex);
        const auto controlBytes = waiterFixedBytes + (m_slots
            ? kisPageStorageBytes(m_slots.get(), alignof(ReservationSlot))
            : kisPageStorageAllocationBytes(sizeof(ReservationSlot), alignof(ReservationSlot)));
        if (!fitsLocked(delta, aggregate, durable, true, controlBytes)) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("budget request exceeds capacity even with no other users"));
            return KisPageReadinessStatus::Unavailable;
        }
        if (m_usage.waitingRequests + m_usage.grantedRequests
            >= KisBackingBudgetWaitContext::Limit) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("budget waiter storage is full"));
            return KisPageReadinessStatus::Unavailable;
        }
        needsContext = !m_waitContext;
    }
    try { notify.fund(this); }
    catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("budget waiter allocation failed"));
        return KisPageReadinessStatus::Unavailable;
    }
    boost::intrusive_ptr<KisBackingBudgetWaitContext> preparedContext;
    // A prepared wake retains this context for an in-flight monitor callback.
    // Only the installed controller may own that cycle. Failed/losing
    // candidates cancel the wake before their final context reference drops.
    const auto cancelCandidate = qScopeGuard([&] {
        if (preparedContext) preparedContext->wake.reset();
    });
    if (needsContext) {
        try {
            auto storage = KisMutationStorageAllocator<KisBackingBudgetWaitContext>::retained(this);
            auto *raw = storage.allocate(1);
            try {
                ::new (static_cast<void *>(raw)) KisBackingBudgetWaitContext(this, storage);
            } catch (...) {
                storage.deallocate(raw, 1);
                throw;
            }
            preparedContext.reset(raw);
            preparedContext->task = kisPreparePageStoreReclamation(dispatch(raw), this,
                +[](void *value) {
                    auto *context = static_cast<KisBackingBudgetWaitContext *>(value);
                    {
                        QMutexLocker lifetime(&context->gate);
                        context->scheduled = false;
                        if (context->owner && std::exchange(context->dispatchAgain, false))
                            context->wake.notify();
                    }
                    intrusive_ptr_release(context);
                }, raw);
            preparedContext->wake = kisPreparePageStoreReclamationWake(
                KisPageReadinessCallback(wakeDispatch(preparedContext), this), this);
            if (!preparedContext->wake.isValid()) {
                KisPageStoreDetail::setError(
                    error, QStringLiteral("budget waiter notification preparation failed"));
                return KisPageReadinessStatus::Unavailable;
            }
        } catch (const std::bad_alloc &) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("budget waiter allocation failed"));
            return KisPageReadinessStatus::Unavailable;
        }
    }
    KisBackingBudgetWaitStatePointer state(
        nullptr, KisBackingBudgetWaitStateDeleter{this});
    try {
        KisMutationStorageAllocator<KisBackingBudgetWaitState> allocator(this);
        KisBackingBudgetWaitState *raw = allocator.allocate(1);
        try {
            ::new (static_cast<void *>(raw)) KisBackingBudgetWaitState();
        } catch (...) {
            allocator.deallocate(raw, 1);
            throw;
        }
        state.reset(raw);
        state->requested = delta;
        state->aggregateBytes = aggregate;
        state->durableBytes = durable;
        state->notify = std::move(notify);
    } catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("budget waiter allocation failed"));
        return KisPageReadinessStatus::Unavailable;
    }
    QMutexLocker lock(&m_mutex);
    while (m_firstFreeSlot == std::numeric_limits<quint32>::max()
           && m_slotCount == m_slotCapacity) {
        lock.unlock();
        const bool prepared = prepareReservationSlot(error);
        lock.relock();
        if (!prepared) return KisPageReadinessStatus::Unavailable;
    }
    const auto controlBytes = waiterFixedBytes + kisPageStorageBytes(m_slots.get(), alignof(ReservationSlot));
    if (!fitsLocked(delta, aggregate, durable, true, controlBytes)) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("budget request exceeds capacity even with no other users"));
        return KisPageReadinessStatus::Unavailable;
    }
    if (m_usage.waitingRequests + m_usage.grantedRequests >= KisBackingBudgetWaitContext::Limit) {
        KisPageStoreDetail::setError(error, QStringLiteral("budget waiter storage is full"));
        return KisPageReadinessStatus::Unavailable;
    }
    try {
        if (!m_waitContext) {
            Q_ASSERT(preparedContext && preparedContext->wake.isValid());
            if (!preparedContext || !preparedContext->wake.isValid()) {
                KisPageStoreDetail::setError(
                    error, QStringLiteral("budget waiter context preparation was lost"));
                return KisPageReadinessStatus::Unavailable;
            }
            m_waitContext = std::move(preparedContext);
            m_waitContext->owner = this;
        }
        if (m_waitContext->nextSequence == std::numeric_limits<quint64>::max())
            return KisPageReadinessStatus::Unavailable;
        state->sequence = m_waitContext->nextSequence++;
        // Prepare the real slot capacity before registering. It is
        // empty until a grant is made; granting never allocates another slot.
        auto slot = reserveLocked({}, {}, 0);
        state->cookie = slot.cookie;
        slot.clear();
        const bool ready = !priorWaiterConflictsLocked(delta, aggregate, durable) &&
                            fitsLocked(delta, aggregate, durable);
        if (ready) {
            activateReservationLocked(state->cookie, delta, aggregate, durable);
            state->granted = true;
            ++m_usage.grantedRequests;
            ++m_usage.waiterGrants;
        } else {
            ++m_usage.waitingRequests;
        }
        const quint64 waiterSequence = state->sequence;
        bool installed = false;
        for (auto &entry : m_waitContext->requests) {
            if (!entry) {
                entry = std::move(state);
                installed = true;
                break;
            }
        }
        Q_ASSERT(installed);
        waiter->context = m_waitContext;
        waiter->sequence = waiterSequence;
        KisPageStoreDetail::setError(error, {});
        if (!ready) {
            // Publishing the request excludes new empty-capacity retention.
            // Drain only while a waiter still needs progress; this also
            // covers a scope parked during unlocked request preparation.
            lock.unlock();
            while (reclaimMutationStorage(bool(m_sharedNonPayloadBudget), true)) {}
        }
        return ready ? KisPageReadinessStatus::Ready : KisPageReadinessStatus::Waiting;
    } catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("budget waiter allocation failed"));
        return KisPageReadinessStatus::Unavailable;
    }
}

KisBackingBudgetController::WaitCallbacks KisBackingBudgetController::dispatchWaiters()
{
    // Caller holds the lifetime context gate. No grant touches physical state.
    std::array<KisPageReadinessCallback, KisBackingBudgetWaitContext::Limit> callbacks;
    size_t count = 0;
    {
        QMutexLocker lock(&m_mutex);
        // Bounded FIFO selection; unrelated resource classes can pass a blocked
        // head, but overlapping new/smaller requests cannot starve that head.
        quint64 after = 0;
        for (size_t n = 0; n < KisBackingBudgetWaitContext::Limit; ++n) {
            KisBackingBudgetWaitState *next = nullptr;
            for (const auto &state : m_waitContext->requests)
                if (state && !state->granted && state->sequence > after &&
                    (!next || state->sequence < next->sequence)) next = state.get();
            if (!next) break;
            after = next->sequence;
            if (priorWaiterConflictsLocked(next->requested, next->aggregateBytes,
                                          next->durableBytes, next->sequence) ||
                !fitsLocked(next->requested, next->aggregateBytes, next->durableBytes)) continue;
            activateReservationLocked(next->cookie, next->requested, next->aggregateBytes, next->durableBytes);
            next->granted = true;
            --m_usage.waitingRequests;
            ++m_usage.grantedRequests;
            ++m_usage.waiterGrants;
            callbacks[count++] = std::move(next->notify);
        }
    }
    return callbacks; // Caller unlocks lifetime gate before invoking callbacks.
}

KisBackingBudgetReservation::KisBackingBudgetReservation(KisBackingBudgetController *ownerValue,
                                                         quint64 cookieValue)
    : owner(ownerValue)
    , cookie(cookieValue)
{
}

KisBackingBudgetReservation::~KisBackingBudgetReservation()
{
    release();
}

KisBackingBudgetReservation::KisBackingBudgetReservation(KisBackingBudgetReservation &&other) noexcept
{
    *this = std::move(other);
}

KisBackingBudgetReservation &KisBackingBudgetReservation::operator=(KisBackingBudgetReservation &&other) noexcept
{
    if (this != &other) {
        release();
        owner = std::exchange(other.owner, nullptr);
        cookie = std::exchange(other.cookie, 0);
        sharedOwner = std::exchange(other.sharedOwner, nullptr);
        sharedCookie = std::exchange(other.sharedCookie, 0);
    }
    return *this;
}

bool KisBackingBudgetReservation::isValid() const
{
    return owner != nullptr;
}

void KisBackingBudgetReservation::release() noexcept
{
    if (owner)
        owner->release(cookie);
    if (sharedOwner)
        sharedOwner->release(sharedCookie);
    clear();
}

void KisBackingBudgetReservation::clear() noexcept
{
    owner = nullptr;
    cookie = 0;
    sharedOwner = nullptr;
    sharedCookie = 0;
}

void KisBackingBudgetReservation::commit(const KisBackingBudgetDelta &delta) noexcept
{
    settle(&delta, nullptr);
}

void KisBackingBudgetReservation::commitRetaining(
    const KisBackingBudgetDelta &installed,
    const KisBackingBudgetDelta &retained) noexcept
{
    settle(&installed, &retained);
}

void KisBackingBudgetReservation::commitPrepared(const KisBackingBudgetDelta *retained) noexcept
{
    settle(nullptr, retained, true);
}

void KisBackingBudgetReservation::settle(const KisBackingBudgetDelta *installed,
                                       const KisBackingBudgetDelta *retained,
                                       bool prepared) noexcept
{
    KisBackingBudgetController *local = owner;
    if (!local || !local->m_sharedNonPayloadBudget) {
        Q_ASSERT(!sharedOwner);
        if (!local || !local->settle(cookie, installed, retained, prepared)) {
            owner = nullptr;
            cookie = 0;
        }
        return;
    }
    KisPageDomainByteDelta sharedInstalled, sharedRetained;
    if (!prepared && installed) sharedInstalled = local->sharedNonPayloadDelta(*installed);
    if (retained) sharedRetained = local->sharedNonPayloadDelta(*retained);
    // Exact ledger completion retains only debt, outside the shared classes.
    // Its parent change was accepted at preparation, including negative-only
    // refunds that deliberately did not allocate a parent reservation.
    Q_ASSERT(!prepared || !hasPositiveBytes(sharedRetained));
    if (!local->settle(cookie, installed, retained, prepared,
                       prepared ? &sharedInstalled : nullptr)) {
        owner = nullptr;
        cookie = 0;
    }
    if (sharedOwner) {
        // The original parent slot retains its accepted signed change too.
        // Exact acceptance consumes that fact; partial consumers provide only
        // the actual shared bucket, through the same settlement implementation.
        if (!sharedOwner->settle(sharedCookie, &sharedInstalled,
                                retained ? &sharedRetained : nullptr, prepared)) {
            sharedOwner = nullptr;
            sharedCookie = 0;
        }
    } else if (hasDomainDelta(sharedInstalled)) {
        // No positive parent demand means no parent ticket. Releasing its
        // existing child charge still has to happen, without another admission.
        // Existing retained consumers keep only RetirementDebt; a non-payload
        // retained amount would require its own already prepared parent ticket.
        Q_ASSERT(!hasPositiveBytes(sharedRetained));
        local->m_sharedNonPayloadBudget->releaseSharedNonPayloadLive(
            local->m_sharedNonPayloadUsage, sharedInstalled);
    }
}

bool KisBackingBudgetReservation::retainOnly(
    const KisBackingBudgetDelta &retained) noexcept
{
    settle(nullptr, &retained);
    return owner != nullptr;
}

KisBackingBudgetController::KisBackingBudgetController(const KisPageBackingLimits &limits, bool processStorage)
    : m_limits(limits)
    , m_sharedChildren(std::less<quint64>{}, SharedChildAllocator(this))
    , m_processStorage(processStorage)
{
}

void KisBackingBudgetController::unlinkMutationStorageLocked(KisReusableMutationStorage *value) noexcept
{
    Q_ASSERT(value && value->origin && value->origin->m_reusableMutation == value);
    if (value->previous) value->previous->next = value->next;
    else m_firstReusableMutation = value->next;
    if (value->next) value->next->previous = value->previous;
    else m_lastReusableMutation = value->previous;
    value->origin->m_reusableMutation = nullptr;
    value->previous = value->next = nullptr;
    value->origin = nullptr;
}

bool KisBackingBudgetController::retainMutationStorage(KisReusableMutationStorage *value) noexcept
{
    if (!value || !value->dispose || value->bytes > ReusableMutationBytes) return false;
    auto *root = m_sharedNonPayloadBudget ? m_sharedNonPayloadBudget.get() : this;
    KisReusableMutationStorage *replaced;
    {
        QMutexLocker lock(&root->m_mutex);
        if (!m_acceptReusableMutation || root->m_usage.waitingRequests) return false;
        Q_ASSERT(!value->origin && !value->previous && !value->next);
        replaced = m_reusableMutation;
        if (replaced) root->unlinkMutationStorageLocked(replaced);
        value->origin = this;
        value->previous = root->m_lastReusableMutation;
        if (value->previous) value->previous->next = value;
        else root->m_firstReusableMutation = value;
        root->m_lastReusableMutation = value;
        m_reusableMutation = value;
    }
    // Keep the most recently completed operation, without retaining a second
    // empty scope after concurrent operations finish in a different order.
    if (replaced) replaced->dispose(replaced);
    return true;
}

KisReusableMutationStorage *KisBackingBudgetController::takeMutationStorage() noexcept
{
    auto *root = m_sharedNonPayloadBudget ? m_sharedNonPayloadBudget.get() : this;
    QMutexLocker lock(&root->m_mutex);
    auto *value = m_reusableMutation;
    if (value) root->unlinkMutationStorageLocked(value);
    return value;
}

bool KisBackingBudgetController::reclaimMutationStorage(bool ownOnly, bool waitingOnly) noexcept
{
    auto *root = m_sharedNonPayloadBudget ? m_sharedNonPayloadBudget.get() : this;
    KisReusableMutationStorage *value;
    {
        QMutexLocker lock(&root->m_mutex);
        if (waitingOnly && !root->m_usage.waitingRequests) return false;
        value = ownOnly ? m_reusableMutation : root->m_firstReusableMutation;
        if (!value) return false;
        root->unlinkMutationStorageLocked(value);
    }
    value->dispose(value);
    return true;
}

void KisBackingBudgetController::stopMutationStorageReuse() noexcept
{
    auto *root = m_sharedNonPayloadBudget ? m_sharedNonPayloadBudget.get() : this;
    KisReusableMutationStorage *value;
    {
        QMutexLocker lock(&root->m_mutex);
        m_acceptReusableMutation = false;
        value = m_reusableMutation;
        if (value) root->unlinkMutationStorageLocked(value);
    }
    if (value) value->dispose(value);
}

KisBackingBudgetReservation KisBackingBudgetController::reserve(
    const KisBackingBudgetDelta &delta, QString *error)
{
    std::array<quint64, budgetClassCount> aggregateBytes{};
    quint64 durableBytes = 0;
    for (quint32 classes = delta.changedClasses(); classes; classes &= classes - 1) {
        const size_t i = size_t(qCountTrailingZeroBits(classes));
        const auto &bucket = delta.bucket(i);
        for (qint64 value : components(bucket)) {
            if (value < 0) {
                KisPageStoreDetail::setError(
                    error, QStringLiteral("backing reservation contains a negative prepare delta"));
                return {};
            }
            aggregateBytes[i] = saturatedAdd(
                aggregateBytes[i], quint64(value));
        }
        if (bucket.ssd > 0)
            durableBytes = saturatedAdd(durableBytes, quint64(bucket.ssd));
    }
    return reserveImpl(delta, aggregateBytes, durableBytes, error);
}

KisBackingBudgetReservation KisBackingBudgetController::reserveChange(
    const KisBackingBudgetDelta &change, QString *error)
{
    KisBackingBudgetDelta prepared;
    std::array<quint64, budgetClassCount> aggregateBytes{};
    quint64 durableBytes = 0;
    if (!prepareBudgetChange(change, &prepared, &aggregateBytes, &durableBytes, error)) return {};
    return reserveImpl(prepared, aggregateBytes, durableBytes, error, &change);
}

bool KisBackingBudgetController::reserveCpuOptionalHandoff(quint64 bytes, QString *error)
{
    if (!bytes || bytes > quint64(std::numeric_limits<qint64>::max())) return false;
    constexpr auto active = size_t(KisBackingBudgetClass::ActivePending);
    KisBackingBudgetDelta demand;
    demand.setBucket(active, {qint64(bytes), 0, 0, 0});
    std::array<quint64, budgetClassCount> aggregate{};
    aggregate[active] = bytes;
    QMutexLocker lock(&m_mutex);
    for (;;) {
        if (priorWaiterConflictsLocked(demand, aggregate, 0)) {
            ++m_usage.backpressureCount;
            KisPageStoreDetail::setError(error, QStringLiteral("backing reservation waits for an earlier admission request"));
            return false;
        }
        if (fitsLocked(demand, aggregate, 0)) break;
        lock.unlock();
        const bool reclaimed = reclaimMutationStorage(bool(m_sharedNonPayloadBudget));
        lock.relock();
        if (reclaimed) continue;
        ++m_usage.backpressureCount;
        KisPageStoreDetail::setError(error, QStringLiteral("backing reservation exceeds its hard budget"));
        return false;
    }
    // The source stays live until commit. Reserve the full positive
    // destination, including its peak, exactly as the generic signed change.
    auto &pending = m_usage.buckets[active];
    pending.reserved.cpuRam += bytes;
    m_reservedAggregateBytes[active] += bytes;
    pending.peak.cpuRam = std::max(pending.peak.cpuRam,
        saturatedAdd(pending.live.cpuRam, pending.reserved.cpuRam));
    if (m_processStorage) {
        auto &metadata = m_usage.buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        metadata.peak.cpuRam = std::max(metadata.peak.cpuRam,
            saturatedAdd(saturatedAdd(saturatedAdd(metadata.live.cpuRam, metadata.reserved.cpuRam),
                                      storageBytesLocked()), processStorageSnapshot().total));
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

void KisBackingBudgetController::settleCpuOptionalHandoff(
    KisBackingBudgetClass source, quint64 bytes, bool commit) noexcept
{
    Q_ASSERT(source == KisBackingBudgetClass::OptionalCache || source == KisBackingBudgetClass::IdleCapacity);
    constexpr auto active = size_t(KisBackingBudgetClass::ActivePending);
    {
        QMutexLocker lock(&m_mutex);
        auto &pending = m_usage.buckets[active];
        Q_ASSERT(pending.reserved.cpuRam >= bytes && m_reservedAggregateBytes[active] >= bytes);
        pending.reserved.cpuRam -= bytes;
        m_reservedAggregateBytes[active] -= bytes;
        if (commit) {
            auto &original = m_usage.buckets[size_t(source)].live.cpuRam;
            Q_ASSERT(original >= bytes);
            original -= bytes;
            pending.live.cpuRam += bytes;
        }
        // Reserved -> live does not increase the admitted peak; releasing the
        // source preserves cached fit facts. Cancellation changes no live fee.
        notifyWaitersLocked();
    }
    // Optional CPU capacity was already charged to the parent's one child.
    // Its destination is payload: only successful acceptance refunds it.
    if (commit && m_sharedNonPayloadBudget)
        m_sharedNonPayloadBudget->releaseSharedNonPayloadLive(
            m_sharedNonPayloadUsage, KisPageAccessDomain::CpuRam, bytes);
}

quint64 KisBackingBudgetController::idleCapacityAllowance() const
{
    QMutexLocker lock(&m_mutex);
    const auto &idle = m_usage.buckets[size_t(KisBackingBudgetClass::IdleCapacity)];
    const quint64 used = saturatedAdd(sumComponents(components(idle.live)),
                                     sumComponents(components(idle.reserved)));
    const quint64 total = optionalBytes(m_usage, m_reservedAggregateBytes);
    if (used >= m_limits.idleCapacityBytes || total >= m_limits.optionalCacheBytes) return 0;
    return std::min(m_limits.idleCapacityBytes - used, m_limits.optionalCacheBytes - total);
}

bool KisBackingBudgetController::admitUnchangedBacking(QString *error)
{
    QMutexLocker lock(&m_mutex);
    if (!fitsLocked({}, {}, 0)) {
        ++m_usage.backpressureCount;
        KisPageStoreDetail::setError(error, QStringLiteral("backing reservation exceeds its hard budget"));
        return false;
    }
    if (m_processStorage) {
        auto &metadata = m_usage.buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        metadata.peak.cpuRam = std::max(metadata.peak.cpuRam,
            saturatedAdd(saturatedAdd(saturatedAdd(metadata.live.cpuRam, metadata.reserved.cpuRam),
                                      storageBytesLocked()), processStorageSnapshot().total));
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisBackingBudgetReservation KisBackingBudgetController::reserveImpl(
    const KisBackingBudgetDelta &delta,
    const std::array<quint64, budgetClassCount> &aggregateBytes,
    quint64 durableBytes, QString *error, const KisBackingBudgetDelta *signedChange)
{
    KisBackingBudgetReservation sharedReservation;
    KisPageDomainByteDelta sharedChange;
    if (m_sharedNonPayloadBudget) {
        bool valid = false;
        sharedChange = sharedNonPayloadDelta(
            signedChange ? *signedChange : delta, &valid);
        if (!valid) {
            KisPageStoreDetail::setError(error, QStringLiteral("shared non-payload transition delta overflows"));
            return {};
        }
        if (hasPositiveBytes(sharedChange)) {
            sharedReservation = m_sharedNonPayloadBudget->reserveSharedNonPayload(
                m_sharedNonPayloadUsage, sharedChange, error);
            if (!sharedReservation.isValid()) {
                QMutexLocker lock(&m_mutex);
                ++m_usage.backpressureCount;
                return {};
            }
        }
    }
    QMutexLocker lock(&m_mutex);
    const auto reject = [&](const QString &message) {
        ++m_usage.backpressureCount;
        KisPageStoreDetail::setError(error, message);
        return KisBackingBudgetReservation{};
    };

    for (;;) {
        if (priorWaiterConflictsLocked(delta, aggregateBytes, durableBytes))
            return reject(QStringLiteral("backing reservation waits for an earlier admission request"));
        if (!fitsLocked(delta, aggregateBytes, durableBytes)) {
            lock.unlock();
            const bool reclaimed = reclaimMutationStorage(bool(m_sharedNonPayloadBudget));
            lock.relock();
            if (reclaimed) continue;
            return reject(QStringLiteral("backing reservation exceeds its hard budget"));
        }
        if (m_firstFreeSlot != std::numeric_limits<quint32>::max()
            || m_slotCount < m_slotCapacity) break;
        lock.unlock();
        const bool prepared = prepareReservationSlot(error);
        lock.relock();
        if (!prepared) {
            ++m_usage.backpressureCount;
            return {};
        }
    }
    KisPageStoreDetail::setError(error, {});
    auto reservation = reserveLocked(delta, aggregateBytes, durableBytes, 0,
                                     signedChange, &sharedChange);
    if (sharedReservation.isValid()) {
        reservation.sharedOwner = sharedReservation.owner;
        reservation.sharedCookie = sharedReservation.cookie;
        sharedReservation.clear();
    }
    return reservation;
}

bool KisBackingBudgetController::prepareReservationSlot(QString *error)
{
    // Serialise array growth, never physical allocation under accounting gates.
    // The same actual-storage protocol bootstraps the first slot and owns every
    // candidate/refund; no synthetic cookie or second reservation is needed.
    QMutexLocker growth(&m_slotGrowthMutex);
    QMutexLocker lock(&m_mutex);
    if (m_firstFreeSlot != std::numeric_limits<quint32>::max()
        || m_slotCount < m_slotCapacity) return true;
    constexpr quint32 maximum = std::numeric_limits<quint32>::max();
    if (m_slotCapacity == maximum) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing reservation slot capacity is exhausted"));
        return false;
    }
    const quint32 capacity = m_slotCapacity
        ? quint32(std::min<quint64>(quint64(m_slotCapacity) * 2, maximum)) : 1;
    const quint64 payloadBytes = quint64(capacity) * sizeof(ReservationSlot);
    if (payloadBytes > std::numeric_limits<size_t>::max()) return false;
    const KisPageStorageLayout layout(size_t(payloadBytes), alignof(ReservationSlot));
    if (!layout.isValid()) return false;
    lock.unlock();
    decltype(m_slots) candidate(nullptr, SlotStorageDeleter{this});
    try {
        candidate.reset(static_cast<ReservationSlot *>(allocateStorage(layout)));
        std::uninitialized_value_construct_n(candidate.get(), capacity);
    } catch (const std::bad_alloc &) {
        lock.relock();
        if (m_firstFreeSlot != maximum || m_slotCount < m_slotCapacity) {
            lock.unlock();
            return true;
        }
        lock.unlock();
        KisPageStoreDetail::setError(error, QStringLiteral("backing reservation storage allocation failed"));
        return false;
    }
    lock.relock();
    if (m_firstFreeSlot != maximum || m_slotCount < m_slotCapacity) {
        lock.unlock();
        return true;
    }
    // Cookies retain their indices. Unselected scalar cells are deliberately
    // indeterminate; copy object representations only under their original gate.
    static_assert(std::is_trivially_copyable_v<ReservationSlot>);
    if (m_slotCount) std::memcpy(candidate.get(), m_slots.get(), m_slotCount * sizeof(ReservationSlot));
    m_slots.swap(candidate);
    m_slotCapacity = capacity;
    lock.unlock();
    candidate.reset();
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisBackingBudgetReservation KisBackingBudgetController::reserveLocked(
    const KisBackingBudgetDelta &delta,
    const std::array<quint64, budgetClassCount> &aggregateBytes,
    quint64 durableBytes,
    SharedChildUsage *sharedChild, const KisBackingBudgetDelta *signedChange,
    const KisPageDomainByteDelta *sharedChange)
{
    quint32 slotIndex = 0;
    if (m_firstFreeSlot == std::numeric_limits<quint32>::max()) {
        // Cookie indices cannot wrap into an older slot. Recycling is linked
        // inside inactive slots, so release/commit need no second allocation.
        if (m_slotCount == m_slotCapacity)
            throw std::bad_alloc();
        slotIndex = m_slotCount++;
    } else {
        slotIndex = m_firstFreeSlot;
        m_firstFreeSlot = m_slots[slotIndex].nextFree;
    }
    ReservationSlot &slot = m_slots[slotIndex];
    slot.nextFree = std::numeric_limits<quint32>::max();
    ++slot.generation;
    if (!slot.generation)
        ++slot.generation;
    slot.active = true;
    const quint64 cookie = (quint64(slot.generation) << 32) | slotIndex;
    activateReservationLocked(cookie, delta, aggregateBytes, durableBytes, sharedChild,
                              signedChange, sharedChange);
    return {this, cookie};
}

void KisBackingBudgetController::activateReservationLocked(
    quint64 cookie, const KisBackingBudgetDelta &delta,
    const std::array<quint64, budgetClassCount> &aggregateBytes,
    quint64 durableBytes,
    SharedChildUsage *sharedChild, const KisBackingBudgetDelta *signedChange,
    const KisPageDomainByteDelta *sharedChange)
{
    auto *slot = activeReservation(cookie);
    Q_ASSERT(slot);
    const auto storageBytes = storageBytesLocked();
    const auto processBytes = m_processStorage ? processStorageSnapshot().total : 0;
    const auto &accepted = signedChange ? *signedChange : delta;
    slot->sharedChange = sharedChange ? *sharedChange : KisPageDomainByteDelta{};
    slot->durableBytes = durableBytes;
    slot->sharedChild = sharedChild;
    slot->changedComponents = 0;
    slot->changedClasses = 0;
    for (quint32 classes = accepted.changedClasses(); classes; classes &= classes - 1) {
        const size_t i = size_t(qCountTrailingZeroBits(classes));
        slot->setBucket(i, accepted.bucket(i));
        slot->aggregateBytes[i] = aggregateBytes[i];
        slot->changedClasses |= quint8(quint32(1) << i);
        m_reservedAggregateBytes[i] = saturatedAdd(m_reservedAggregateBytes[i], aggregateBytes[i]);
        auto &usage = m_usage.buckets[i];
        auto reserved = components(usage.reserved);
        auto peak = components(usage.peak);
        const auto live = components(usage.live);
        const auto requested = components(delta.bucket(i));
        for (size_t domain = 0; domain < requested.size(); ++domain) {
            if (!requested[domain]) continue;
            reserved[domain] += quint64(requested[domain]);
            auto total = saturatedAdd(live[domain], reserved[domain]);
            if (i == size_t(KisBackingBudgetClass::MetadataArena) && domain == 0)
                total = saturatedAdd(total, storageBytes);
            if (m_processStorage && i == size_t(KisBackingBudgetClass::MetadataArena) && domain == 0)
                total = saturatedAdd(total, processBytes);
            peak[domain] = std::max(peak[domain], total);
        }
        usage.reserved = fromComponents(reserved);
        usage.peak = fromComponents(peak);
    }
    // fitsLocked proved the proposed domain and aggregate totals under this
    // same gate. Installing that exact reservation preserves the fit facts.
    m_reservedDurableBytes = saturatedAdd(m_reservedDurableBytes, durableBytes);
    if (sharedChild) sharedChild->reservedBytes = saturatedAdd(sharedChild->reservedBytes,
        aggregateBytes[size_t(KisBackingBudgetClass::MetadataArena)]);
    if (m_processStorage) {
        auto &metadata = m_usage.buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        metadata.peak.cpuRam = std::max(metadata.peak.cpuRam,
            saturatedAdd(saturatedAdd(saturatedAdd(metadata.live.cpuRam, metadata.reserved.cpuRam),
                                      storageBytes), processBytes));
    }
}

void KisBackingBudgetController::commitReservation(
    KisBackingBudgetReservation &&reservation,
    const KisBackingBudgetDelta &installed) noexcept
{
    if (reservation.owner == this)
        reservation.commit(installed);
}

KisPageBackingUsage KisBackingBudgetController::usage() const
{
    QMutexLocker lock(&m_mutex);
    auto result = m_usage;
    const auto storage = storageBytesLocked();
    const auto live = m_storageLiveBytes.load(std::memory_order_relaxed);
    Q_ASSERT(live <= storage);
    auto &metadata = result.buckets[size_t(KisBackingBudgetClass::MetadataArena)];
    metadata.live.cpuRam = saturatedAdd(metadata.live.cpuRam, live);
    metadata.reserved.cpuRam = saturatedAdd(metadata.reserved.cpuRam, storage - live);
    if (m_processStorage) {
        const auto process = processStorageSnapshot();
        metadata.live.cpuRam = saturatedAdd(metadata.live.cpuRam, process.live);
        metadata.reserved.cpuRam = saturatedAdd(metadata.reserved.cpuRam, process.reserved);
        metadata.peak.cpuRam = std::max(metadata.peak.cpuRam,
            saturatedAdd(metadata.live.cpuRam, metadata.reserved.cpuRam));
    }
    return result;
}

quint32 KisBackingBudgetController::maxTransientVersionsPerPage() const
{
    QMutexLocker lock(&m_mutex);
    return m_limits.maxTransientVersionsPerPage;
}

bool KisBackingBudgetController::reservationCovers(quint64 cookie,
                                                    KisBackingBudgetClass budgetClass,
                                                    KisPageAccessDomain domain,
                                                    quint64 bytes) const
{
    const size_t classIndex = static_cast<size_t>(budgetClass);
    QMutexLocker lock(&m_mutex);
    const ReservationSlot *slot = activeReservation(cookie);
    if (classIndex >= budgetClassCount || !slot)
        return false;
    const auto bucket = slot->bucket(classIndex);
    const qint64 *reserved = kisPageDomainComponent(bucket, domain);
    return reserved && std::max(qint64(0), *reserved) >= qint64(bytes);
}

bool KisBackingBudgetController::reservationCovers(
    quint64 cookie, const KisBackingBudgetDelta &needed) const
{
    QMutexLocker lock(&m_mutex);
    const ReservationSlot *slot = activeReservation(cookie);
    if (!slot) return false;
    for (quint32 classes = needed.changedClasses(); classes; classes &= classes - 1) {
        const size_t i = size_t(qCountTrailingZeroBits(classes));
        const auto reserved = components(slot->bucket(i));
        const auto requested = components(needed.bucket(i));
        for (size_t domain = 0; domain < requested.size(); ++domain)
            if (requested[domain] < 0 || requested[domain] > std::max(qint64(0), reserved[domain])) return false;
    }
    return true;
}

bool KisBackingBudgetController::configureLimits(const KisPageBackingLimits &limits,
                                                  QString *error)
{
    QMutexLocker lock(&m_mutex);
    if (m_usage.waitingRequests || m_usage.grantedRequests
        || !m_sharedChildren.empty() || storageBytesLocked()) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("backing limits have active users or waiters"));
        return false;
    }
    for (const auto &bucket : m_usage.buckets) {
        if (sumComponents(components(bucket.live))
            || sumComponents(components(bucket.reserved))) {
            KisPageStoreDetail::setError(error, QStringLiteral("backing limits are already in use"));
            return false;
        }
    }
    m_limits = limits;
    m_fittingBudgetClasses = 0;
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisBackingBudgetController::configureSharedNonPayloadBudget(
    const std::shared_ptr<KisBackingBudgetController> &parent,
    QString *error)
{
    QMutexLocker lock(&m_mutex);
    if (!parent || parent.get() == this || parent->m_sharedNonPayloadBudget) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("shared non-payload budget parent is invalid"));
        return false;
    }
    if (m_sharedNonPayloadBudget || !m_sharedChildren.empty() || storageBytesLocked()
        || m_usage.waitingRequests || m_usage.grantedRequests) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("shared non-payload budget is already configured or in use"));
        return false;
    }
    for (const auto &bucket : m_usage.buckets) {
        if (sumComponents(components(bucket.live))
            || sumComponents(components(bucket.reserved))) {
            KisPageStoreDetail::setError(error, QStringLiteral("shared non-payload budget is already in use"));
            return false;
        }
    }
    SharedChildUsage *usage = nullptr;
    const quint64 child = parent->registerSharedNonPayloadChild(&usage, error);
    if (!child) return false;
    m_sharedNonPayloadBudget = parent;
    m_sharedNonPayloadChild = child;
    m_sharedNonPayloadUsage = usage;
    KisPageStoreDetail::setError(error, {});
    return true;
}

quint64 KisBackingBudgetController::registerSharedNonPayloadChild(SharedChildUsage **usage, QString *error)
{
    SharedChildMap prepared(std::less<quint64>{}, SharedChildAllocator(this));
    try {
        prepared.emplace(0, SharedChildUsage{});
    } catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("shared non-payload store registration allocation failed"));
        return 0;
    }
    auto node = prepared.extract(prepared.begin());

    QMutexLocker lock(&m_mutex);
    if (m_sharedNonPayloadBudget || !m_nextSharedChild) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("shared non-payload parent cannot register another store"));
        return 0;
    }
    const quint64 child = m_nextSharedChild++;
    node.key() = child;
    auto installed = m_sharedChildren.insert(std::move(node));
    Q_ASSERT(installed.inserted);
    if (!installed.inserted) {
        [[maybe_unused]] auto rejected = std::move(installed.node);
        lock.unlock();
        KisPageStoreDetail::setError(
            error, QStringLiteral("shared non-payload store identity is exhausted"));
        return 0;
    }
    *usage = &installed.position->second;
    KisPageStoreDetail::setError(error, {});
    return child;
}

void KisBackingBudgetController::unregisterSharedNonPayloadChild(quint64 child) noexcept
{
    SharedChildMap::node_type released;
    {
        QMutexLocker lock(&m_mutex);
        const auto found = m_sharedChildren.find(child);
        Q_ASSERT(found != m_sharedChildren.end());
        if (found == m_sharedChildren.end()) return;
        Q_ASSERT(found->second.chargedBytes == 0 && found->second.reservedBytes == 0);
        if (found->second.chargedBytes || found->second.reservedBytes) return;
        released = m_sharedChildren.extract(found);
    }
}

KisBackingBudgetReservation KisBackingBudgetController::reserveSharedNonPayload(
    SharedChildUsage *child,
    const KisPageDomainByteDelta &change,
    QString *error)
{
    KisBackingBudgetDelta delta;
    delta.setBucket(size_t(KisBackingBudgetClass::MetadataArena), change);
    std::array<quint64, budgetClassCount> aggregateBytes{};
    quint64 durableBytes = 0;
    KisBackingBudgetDelta prepared;
    if (!prepareBudgetChange(delta, &prepared, &aggregateBytes, &durableBytes, error)) return {};

    QMutexLocker lock(&m_mutex);
    const auto reject = [&](const QString &message) {
        ++m_usage.backpressureCount;
        KisPageStoreDetail::setError(error, message);
        return KisBackingBudgetReservation{};
    };
    if (!child)
        return reject(QStringLiteral("shared non-payload store registration is absent"));
    for (;;) {
        if (priorWaiterConflictsLocked(prepared, aggregateBytes, durableBytes))
            return reject(QStringLiteral("shared non-payload reservation waits for an earlier request"));
        if (!fitsLocked(prepared, aggregateBytes, durableBytes)) {
            lock.unlock();
            const bool reclaimed = reclaimMutationStorage(false);
            lock.relock();
            if (reclaimed) continue;
            return reject(QStringLiteral("shared non-payload process budget is exhausted"));
        }
        if (m_firstFreeSlot != std::numeric_limits<quint32>::max()
            || m_slotCount < m_slotCapacity) break;
        lock.unlock();
        const bool prepared = prepareReservationSlot(error);
        lock.relock();
        if (!prepared) {
            ++m_usage.backpressureCount;
            return {};
        }
    }
    KisPageStoreDetail::setError(error, {});
    return reserveLocked(prepared, aggregateBytes, durableBytes, child, &delta);
}

KisPageDomainByteDelta KisBackingBudgetController::sharedNonPayloadDelta(
    const KisBackingBudgetDelta &delta, bool *valid) noexcept
{
    const auto metadata = components(delta.bucket(size_t(KisBackingBudgetClass::MetadataArena)));
    const auto cache = components(delta.bucket(size_t(KisBackingBudgetClass::OptionalCache)));
    const auto idle = components(delta.bucket(size_t(KisBackingBudgetClass::IdleCapacity)));
    std::array<qint64, 4> combined;
    bool overflow = false;
    for (size_t domain = 0; domain < combined.size(); ++domain) {
        if (cache[domain] > 0 && metadata[domain] > std::numeric_limits<qint64>::max() - cache[domain]) {
            combined[domain] = std::numeric_limits<qint64>::max();
            overflow = true;
        } else if (cache[domain] < 0 && metadata[domain] < std::numeric_limits<qint64>::min() - cache[domain]) {
            combined[domain] = std::numeric_limits<qint64>::min();
            overflow = true;
        } else
            combined[domain] = metadata[domain] + cache[domain];
        qint64 sum;
        if (__builtin_add_overflow(combined[domain], idle[domain], &sum)) {
            combined[domain] = idle[domain] > 0 ? std::numeric_limits<qint64>::max()
                                              : std::numeric_limits<qint64>::min();
            overflow = true;
        } else {
            combined[domain] = sum;
        }
    }
    if (valid) *valid = !overflow;
    Q_ASSERT(valid || !overflow); // Already admitted changes cannot overflow at commit.
    return {combined[0], combined[1], combined[2], combined[3]};
}

void KisBackingBudgetController::releaseStorageLocked(quint64 bytes, bool installed) noexcept
{
    Q_ASSERT(storageBytesLocked() >= bytes);
    m_storageBytes -= bytes;
    if (installed) {
        const auto live = m_storageLiveBytes.fetch_sub(bytes, std::memory_order_relaxed);
        Q_ASSERT(live >= bytes); Q_UNUSED(live);
    }
    notifyWaitersLocked();
}

void KisBackingBudgetController::releaseSharedStorage(
    SharedChildUsage *child, quint64 bytes, bool installed) noexcept
{
    QMutexLocker lock(&m_mutex);
    Q_ASSERT(child && child->chargedBytes >= bytes);
    child->chargedBytes -= bytes;
    releaseStorageLocked(bytes, installed);
}

void KisBackingBudgetController::releaseStorage(quint64 bytes, bool installed) noexcept
{
    {
        QMutexLocker lock(&m_mutex);
        releaseStorageLocked(bytes, installed);
    }
    if (m_sharedNonPayloadBudget)
        m_sharedNonPayloadBudget->releaseSharedStorage(m_sharedNonPayloadUsage, bytes, installed);
}

void KisBackingBudgetController::releaseLive(KisBackingBudgetClass budgetClass,
                                             KisPageAccessDomain domain,
                                             quint64 bytes) noexcept
{
    const size_t bucketIndex = static_cast<size_t>(budgetClass);
    if (bucketIndex >= budgetClassCount) return;
    {
        QMutexLocker lock(&m_mutex);
        quint64 *live = kisPageDomainComponent(m_usage.buckets[bucketIndex].live, domain);
        if (!live) return;
        Q_ASSERT(*live >= bytes);
        *live = bytes > *live ? 0 : *live - bytes;
        if (budgetClass != KisBackingBudgetClass::MetadataArena && domain == KisPageAccessDomain::Ssd && bytes)
            m_nonMetadataLiveSsdValid = false;
        // A decrease cannot invalidate a previously fitting class. Metadata,
        // process storage and cross-class constraints are checked separately.
        notifyWaitersLocked();
    }
    if (m_sharedNonPayloadBudget
        && (budgetClass == KisBackingBudgetClass::MetadataArena
            || budgetClass == KisBackingBudgetClass::OptionalCache
            || budgetClass == KisBackingBudgetClass::IdleCapacity)) {
        m_sharedNonPayloadBudget->releaseSharedNonPayloadLive(
            m_sharedNonPayloadUsage, domain, bytes);
    }
}

void KisBackingBudgetController::releaseSharedNonPayloadLive(
    SharedChildUsage *child,
    KisPageAccessDomain domain,
    quint64 bytes) noexcept
{
    QMutexLocker lock(&m_mutex);
    releaseSharedNonPayloadLiveLocked(child, domain, bytes);
    notifyWaitersLocked();
}

void KisBackingBudgetController::releaseSharedNonPayloadLive(
    SharedChildUsage *child, const KisPageDomainByteDelta &change) noexcept
{
    Q_ASSERT(hasDomainDelta(change) && !hasPositiveBytes(change));
    const auto amounts = components(change);
    const KisPageAccessDomain domains[] = {KisPageAccessDomain::CpuRam,
        KisPageAccessDomain::UmaShared, KisPageAccessDomain::DiscreteVram, KisPageAccessDomain::Ssd};
    QMutexLocker lock(&m_mutex);
    for (size_t i = 0; i < amounts.size(); ++i)
        if (amounts[i] < 0)
            releaseSharedNonPayloadLiveLocked(child, domains[i], quint64(-(amounts[i] + 1)) + 1);
    notifyWaitersLocked();
}

void KisBackingBudgetController::releaseSharedNonPayloadLiveLocked(
    SharedChildUsage *child, KisPageAccessDomain domain, quint64 bytes) noexcept
{
    Q_ASSERT(child && child->chargedBytes >= bytes);
    if (!child) return;
    child->chargedBytes = bytes > child->chargedBytes ? 0 : child->chargedBytes - bytes;
    quint64 *live = kisPageDomainComponent(
        m_usage.buckets[size_t(KisBackingBudgetClass::MetadataArena)].live, domain);
    if (!live) return;
    Q_ASSERT(*live >= bytes);
    *live = bytes > *live ? 0 : *live - bytes;
}

template<class Delta>
bool KisBackingBudgetController::settle(quint64 cookie,
    const Delta *installed,
    const Delta *retained, bool acceptPrepared,
    KisPageDomainByteDelta *sharedInstalled) noexcept
{
    QMutexLocker lock(&m_mutex);
    const quint32 slotIndex = quint32(cookie);
    ReservationSlot *slot = activeReservation(cookie);
    if (!slot) return false;
    constexpr auto metadata = size_t(KisBackingBudgetClass::MetadataArena);
    if constexpr (std::is_same_v<Delta, KisPageDomainByteDelta>) {
        Q_ASSERT(slot->sharedChild && !(slot->changedClasses & ~(quint32(1) << metadata)));
    }
    if (acceptPrepared && sharedInstalled) *sharedInstalled = slot->sharedChange;
    auto *sharedUsage = slot->sharedChild;
    const quint64 sharedPreparedBytes = slot->aggregate(metadata);
    Q_ASSERT(m_reservedDurableBytes >= slot->durableBytes);
    m_reservedDurableBytes -= slot->durableBytes;
    quint64 retainedDurable = 0;
    quint64 sharedRetainedBytes = 0;
    quint8 retainedClasses = 0;
    KisPageDomainByteDelta sharedApplied;
    const KisPageDomainByteDelta empty;
    quint32 affected = slot->changedClasses;
    if (installed && !acceptPrepared) affected |= changedBudgetClasses(*installed);
    if (retained) affected |= changedBudgetClasses(*retained);
    for (; affected; affected &= affected - 1) {
        const auto i = size_t(qCountTrailingZeroBits(affected));
        const auto original = slot->bucket(i);
        const auto &applied = acceptPrepared ? original : installed ? budgetBucket(*installed, i) : empty;
        const auto &kept = retained ? budgetBucket(*retained, i) : empty;
        const auto aggregate = slot->aggregate(i);
        if (i != metadata && applied.ssd) m_nonMetadataLiveSsdValid = false;
        auto &usage = m_usage.buckets[i];
        auto live = components(usage.live);
        auto peak = components(usage.peak);
        auto reserved = components(usage.reserved);
        const auto originalAggregate = saturatedAdd(sumComponents(live), m_reservedAggregateBytes[i]);
        Q_ASSERT(m_reservedAggregateBytes[i] >= aggregate);
        m_reservedAggregateBytes[i] -= aggregate;
        quint64 retainedAggregate = 0;
        const auto prepared = components(original);
        const auto keep = components(kept);
        auto commit = components(applied);
        bool increased = false;
        for (size_t domain = 0; domain < prepared.size(); ++domain) {
            if (!prepared[domain] && !keep[domain] && !commit[domain]) continue;
            const auto held = std::max(qint64(0), prepared[domain]);
            if (acceptPrepared) commit[domain] -= keep[domain];
            Q_ASSERT(keep[domain] >= 0 && keep[domain] <= held);
            const auto returned = quint64(held - keep[domain]);
            Q_ASSERT(reserved[domain] >= returned);
            reserved[domain] -= returned;
            if (commit[domain] < 0) {
                const quint64 removal = quint64(-(commit[domain] + 1)) + 1;
                Q_ASSERT(live[domain] >= removal);
                live[domain] = removal > live[domain] ? 0 : live[domain] - removal;
            } else {
                increased |= quint64(commit[domain]) > returned;
                live[domain] = saturatedAdd(live[domain], quint64(commit[domain]));
                peak[domain] = std::max(peak[domain], live[domain]);
            }
            retainedAggregate = saturatedAdd(retainedAggregate, quint64(keep[domain]));
        }
        usage.live = fromComponents(live);
        usage.peak = fromComponents(peak);
        usage.reserved = fromComponents(reserved);
        if (retainedAggregate) {
            retainedClasses |= quint8(quint32(1) << i);
            slot->aggregateBytes[i] = retainedAggregate;
            m_reservedAggregateBytes[i] = saturatedAdd(m_reservedAggregateBytes[i], retainedAggregate);
            if (kept.ssd > 0) retainedDurable = saturatedAdd(retainedDurable, quint64(kept.ssd));
        }
        if (retained) slot->setBucket(i, kept);
        // Replacing already reserved load by no greater live/retained load
        // preserves every class limit. General partial installation can still
        // increase a domain or the aggregate, so evaluate those real changes.
        if (increased || saturatedAdd(sumComponents(live), m_reservedAggregateBytes[i]) > originalAggregate)
            m_fittingBudgetClasses &= ~(quint32(1) << i);
        if (i == metadata) {
            usage.peak.cpuRam = std::max(usage.peak.cpuRam,
                saturatedAdd(saturatedAdd(usage.live.cpuRam, usage.reserved.cpuRam), storageBytesLocked()));
            sharedApplied = {commit[0], commit[1], commit[2], commit[3]};
            sharedRetainedBytes = retainedAggregate;
        }
    }
    if (sharedUsage) {
        Q_ASSERT(sharedPreparedBytes >= sharedRetainedBytes
                 && sharedUsage->reservedBytes >= sharedPreparedBytes - sharedRetainedBytes);
        sharedUsage->reservedBytes -= std::min(sharedUsage->reservedBytes,
            sharedPreparedBytes - sharedRetainedBytes);
        for (qint64 value : components(sharedApplied)) {
            if (value < 0) {
                const quint64 removal = quint64(-(value + 1)) + 1;
                Q_ASSERT(sharedUsage->chargedBytes >= removal);
                sharedUsage->chargedBytes = removal > sharedUsage->chargedBytes ? 0 : sharedUsage->chargedBytes - removal;
            } else sharedUsage->chargedBytes = saturatedAdd(sharedUsage->chargedBytes, quint64(value));
        }
    }
    if (retainedClasses) {
        slot->changedClasses = retainedClasses;
        slot->durableBytes = retainedDurable;
        m_reservedDurableBytes = saturatedAdd(m_reservedDurableBytes, retainedDurable);
    } else {
        slot->active = false;
        slot->sharedChild = nullptr;
        slot->nextFree = m_firstFreeSlot;
        m_firstFreeSlot = slotIndex;
    }
    notifyWaitersLocked();
    return retainedClasses != 0;
}

void KisBackingBudgetController::moveRetirementHeadroom(
    quint64 cookie, KisPageAccessDomain source, KisPageAccessDomain target,
    quint64 bytes) noexcept
{
    QMutexLocker lock(&m_mutex);
    auto *slot = activeReservation(cookie);
    const size_t debt = size_t(KisBackingBudgetClass::RetirementDebt);
    Q_ASSERT(slot && slot->durableBytes == 0 && slot->aggregate(debt) == bytes);
    if (!slot) return;
    m_fittingBudgetClasses &= ~(quint32(1) << debt);
    auto delta = slot->bucket(debt);
    auto *before = kisPageDomainComponent(delta, source);
    auto *after = kisPageDomainComponent(delta, target);
    Q_ASSERT(before && after && before != after && *before == qint64(bytes) && *after == 0);
    *before = 0;
    *after = qint64(bytes);
    slot->setBucket(debt, delta);
    auto &usage = m_usage.buckets[debt];
    *kisPageDomainComponent(usage.reserved, source) -= bytes;
    auto *reserved = kisPageDomainComponent(usage.reserved, target);
    *reserved += bytes;
    auto *peak = kisPageDomainComponent(usage.peak, target);
    *peak = std::max(*peak, saturatedAdd(*reserved, *kisPageDomainComponent(usage.live, target)));
    // Aggregate headroom is unchanged. Its SSD component reserves a future
    // class change of the same physical bytes, not another durable payload.
}

void KisBackingBudgetController::release(quint64 cookie) noexcept
{
    settle<KisBackingBudgetDelta>(cookie, nullptr, nullptr);
}

size_t kisPageStorageAllocationBytes(size_t bytes, size_t alignment) noexcept
{
    constexpr size_t maximum = size_t(std::numeric_limits<qint64>::max());
    if (!alignment || (alignment & (alignment - 1))) return size_t(-1);
    if (bytes > maximum) return size_t(-1);
    size_t capacity = std::max(bytes, size_t(1));
#ifdef Q_OS_DARWIN
    if (pageStorageHasPrefix(bytes, alignment)) {
        const size_t mappingAlignment = std::max(alignment, alignof(PageStorageMapping));
        if (mappingAlignment > maximum - sizeof(PageStorageMapping)) return size_t(-1);
        const size_t prefix = sizeof(PageStorageMapping) + mappingAlignment - 1;
        if (bytes > maximum - prefix) return size_t(-1);
        capacity = bytes + prefix;
    }
    if (capacity > 64 * 1024) {
        const long pageSize = sysconf(_SC_PAGESIZE);
        if (pageSize <= 0 || capacity > maximum - size_t(pageSize - 1)) return size_t(-1);
        return (capacity + size_t(pageSize - 1)) / size_t(pageSize) * size_t(pageSize);
    }
    if (capacity > maximum - (alignment - 1)) return size_t(-1);
    capacity = (capacity + alignment - 1) & ~(alignment - 1);
    const size_t rounded = malloc_good_size(capacity);
    if (rounded < capacity || rounded > maximum) return size_t(-1);
    capacity = rounded;
#elif defined(Q_OS_WIN) || defined(Q_OS_UNIX)
    capacity = KisPagePlatformStorage::allocationBytes(bytes, alignment);
#else
    const size_t prefix = pageStoragePrefix(alignment);
    if (prefix > maximum || bytes > maximum - prefix) return size_t(-1);
    capacity = bytes + prefix;
#endif
    return capacity;
}

void *kisAllocatePageStorage(const KisPageStorageLayout &layout)
{
    if (!layout.isValid()) throw std::bad_alloc();
    const size_t bytes = layout.bytes();
    const size_t alignment = layout.alignment();
    const size_t capacity = layout.capacity();
#ifdef Q_OS_DARWIN
    if (pageStorageHasPrefix(bytes, alignment)) {
        void *raw = capacity > 64 * 1024
            ? mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0)
            : ::operator new(capacity);
        if (raw == MAP_FAILED) throw std::bad_alloc();
        const size_t mappingAlignment = std::max(alignment, alignof(PageStorageMapping));
        const uintptr_t start = reinterpret_cast<uintptr_t>(raw) + sizeof(PageStorageMapping);
        const uintptr_t aligned = (start + mappingAlignment - 1) & ~(mappingAlignment - 1);
        auto *data = reinterpret_cast<std::byte *>(aligned);
        ::new (data - sizeof(PageStorageMapping)) PageStorageMapping{raw, capacity};
        return data;
    }
    return ::operator new(capacity);
#elif defined(Q_OS_WIN) || defined(Q_OS_UNIX)
    return KisPagePlatformStorage::allocate(bytes, alignment, capacity);
#else
    const size_t prefix = pageStoragePrefix(alignment);
    auto *raw = static_cast<std::byte *>(alignment > alignof(std::max_align_t)
        ? ::operator new(capacity, std::align_val_t(alignment)) : ::operator new(capacity));
    ::new (raw) size_t(capacity);
    return raw + prefix;
#endif
}

size_t kisPageStorageBytes(const void *data, size_t alignment) noexcept
{
    if (!data) return 0;
#ifdef Q_OS_DARWIN
    if (alignment <= alignof(std::max_align_t)) {
        const size_t capacity = malloc_size(data);
        if (capacity) return capacity;
    }
    return reinterpret_cast<const PageStorageMapping *>(
        static_cast<const std::byte *>(data) - sizeof(PageStorageMapping))->bytes;
#elif defined(Q_OS_WIN) || defined(Q_OS_UNIX)
    return KisPagePlatformStorage::bytes(data);
#else
    const auto *raw = static_cast<const std::byte *>(data) - pageStoragePrefix(alignment);
    return *reinterpret_cast<const size_t *>(raw);
#endif
}

size_t kisFreePageStorage(void *data, size_t alignment) noexcept
{
    if (!data) return 0;
#ifdef Q_OS_DARWIN
    // Decode each allocation once. Ordinary native blocks need one size
    // lookup, while over-aligned/mapped blocks carry their original layout.
    if (alignment <= alignof(std::max_align_t)) {
        if (const size_t capacity = malloc_size(data)) {
            ::operator delete(data);
            return capacity;
        }
    }
    const auto mapping = *reinterpret_cast<const PageStorageMapping *>(
        static_cast<const std::byte *>(data) - sizeof(PageStorageMapping));
    if (malloc_size(mapping.base)) {
        ::operator delete(mapping.base);
    } else if (munmap(mapping.base, mapping.bytes) != 0) {
        // Do not drop the fee before actual physical release, even when the
        // ordinary OS release fails. The original native fallback is retained.
        const auto result = mach_vm_deallocate(mach_task_self(),
            reinterpret_cast<mach_vm_address_t>(mapping.base), mapping.bytes);
        if (result != KERN_SUCCESS) std::terminate();
    }
    return mapping.bytes;
#else
    const size_t capacity = kisPageStorageBytes(data, alignment);
#if defined(Q_OS_WIN) || defined(Q_OS_UNIX)
    if (!KisPagePlatformStorage::release(data)) std::terminate();
#else
    auto *raw = static_cast<std::byte *>(data) - pageStoragePrefix(alignment);
    freePhysicalStorage(raw, alignment);
#endif
    return capacity;
#endif
}

quint64 kisPageProcessStorageBytes() noexcept
{
    return processStorage().bytes.load(std::memory_order_acquire);
}

quint64 kisPageProcessStorageLimit() noexcept
{
    auto &state = processStorage();
    QMutexLocker gate(&state.gate);
    return state.limit;
}

void kisReservePageProcessStorage(size_t bytes)
{
    if (bytes > size_t(std::numeric_limits<qint64>::max())) throw std::bad_alloc();
    auto &state = processStorage();
    std::shared_ptr<KisBackingBudgetController> parent;
    for (;;) {
        QMutexLocker gate(&state.gate);
        parent = state.parent.lock();
        const auto current = state.bytes.load(std::memory_order_relaxed);
        if (bytes > state.limit || current > state.limit - bytes) throw std::bad_alloc();
        if (parent) {
            QMutexLocker budget(&parent->m_mutex);
            if (!parent->fitsMetadataStorageLocked(bytes)) {
                budget.unlock();
                gate.unlock();
                if (parent->reclaimMutationStorage(false)) continue;
                budget.relock();
                ++parent->m_usage.backpressureCount;
                throw std::bad_alloc();
            }
            const auto live = state.liveBytes.load(std::memory_order_relaxed);
            const auto reserved = state.reservedBytes.load(std::memory_order_relaxed);
            publishProcessStorageLocked(state, live, reserved + bytes, current + bytes);
            auto &metadata = parent->m_usage.buckets[size_t(KisBackingBudgetClass::MetadataArena)];
            metadata.peak.cpuRam = std::max(metadata.peak.cpuRam,
                saturatedAdd(saturatedAdd(saturatedAdd(metadata.live.cpuRam, metadata.reserved.cpuRam),
                                          parent->storageBytesLocked()), current + bytes));
        } else {
            const auto live = state.liveBytes.load(std::memory_order_relaxed);
            const auto reserved = state.reservedBytes.load(std::memory_order_relaxed);
            publishProcessStorageLocked(state, live, reserved + bytes, current + bytes);
        }
        return;
    }
}

void *kisAllocatePageProcessStorage(size_t bytes, size_t alignment)
{
    const KisPageStorageLayout layout(bytes, alignment);
    const size_t capacity = layout.capacity();
    kisReservePageProcessStorage(capacity);
    try {
        void *result = kisAllocatePageStorage(layout);
        kisCommitPageProcessStorage(capacity, capacity);
        return result;
    } catch (...) {
        kisReleasePageProcessStorage(capacity);
        throw;
    }
}

void kisCommitPageProcessStorage(size_t preparedBytes, size_t liveBytes) noexcept
{
    auto &state = processStorage();
    std::shared_ptr<KisBackingBudgetController> parent;
    {
        QMutexLocker gate(&state.gate);
        parent = state.parent.lock();
        const auto live = state.liveBytes.load(std::memory_order_relaxed);
        const auto reserved = state.reservedBytes.load(std::memory_order_relaxed);
        const auto total = state.bytes.load(std::memory_order_relaxed);
        Q_ASSERT(liveBytes <= preparedBytes && reserved >= preparedBytes && total >= preparedBytes);
        publishProcessStorageLocked(state, live + liveBytes, reserved - preparedBytes,
                                    total - preparedBytes + liveBytes);
    }
    if (parent && liveBytes < preparedBytes) {
        QMutexLocker budget(&parent->m_mutex);
        parent->notifyWaitersLocked();
    }
}

void kisReleasePageProcessStorage(size_t bytes) noexcept
{
    auto &state = processStorage();
    std::shared_ptr<KisBackingBudgetController> parent;
    {
        QMutexLocker gate(&state.gate);
        parent = state.parent.lock();
        const auto live = state.liveBytes.load(std::memory_order_relaxed);
        const auto reserved = state.reservedBytes.load(std::memory_order_relaxed);
        const auto total = state.bytes.load(std::memory_order_relaxed);
        Q_ASSERT(reserved >= bytes && total >= bytes);
        publishProcessStorageLocked(state, live, reserved - bytes, total - bytes);
    }
    if (parent) {
        QMutexLocker budget(&parent->m_mutex);
        parent->notifyWaitersLocked();
    }
}

void kisReleaseLivePageProcessStorage(size_t bytes) noexcept
{
    auto &state = processStorage();
    std::shared_ptr<KisBackingBudgetController> parent;
    {
        QMutexLocker gate(&state.gate);
        parent = state.parent.lock();
        const auto live = state.liveBytes.load(std::memory_order_relaxed);
        const auto reserved = state.reservedBytes.load(std::memory_order_relaxed);
        const auto total = state.bytes.load(std::memory_order_relaxed);
        Q_ASSERT(live >= ProcessStorageState::fixedStorageBytes() + bytes && total >= bytes);
        publishProcessStorageLocked(state, live - bytes, reserved, total - bytes);
    }
    if (parent) {
        QMutexLocker budget(&parent->m_mutex);
        parent->notifyWaitersLocked();
    }
}

void kisFreePageProcessStorage(void *data, size_t, size_t alignment) noexcept
{
    if (!data) return;
    const size_t capacity = kisFreePageStorage(data, alignment);
    kisReleaseLivePageProcessStorage(capacity);
}

std::shared_ptr<KisBackingBudgetController> kisAcquirePageStoreProcessBudget(
    quint64 metadataBytes, QString *error, bool publishPolicy) try
{
    auto &state = processStorage();
    QMutexLocker creation(&state.creation);
    std::shared_ptr<KisBackingBudgetController> parent;
    {
        QMutexLocker gate(&state.gate);
        parent = state.parent.lock();
        if (parent) {
            if (state.productPolicy && state.limit != metadataBytes) {
                KisPageStoreDetail::setError(error, QStringLiteral("PageStore memory configuration changed while product stores are active"));
                return {};
            }
            if (state.limit != metadataBytes) {
                // Bootstrap has a finite ceiling before product configuration.
                // Adopt the real policy once, only if all original live and
                // reserved storage fits. No allocation or charge is moved.
                QMutexLocker budget(&parent->m_mutex);
                const auto previous = parent->m_limits.metadataArenaBytes;
                parent->m_limits.metadataArenaBytes = metadataBytes;
                if (!parent->fitsLocked({}, {}, 0)) {
                    parent->m_limits.metadataArenaBytes = previous;
                    KisPageStoreDetail::setError(error, QStringLiteral("PageStore bootstrap storage exceeds the configured process budget"));
                    return {};
                }
                state.limit = metadataBytes;
            }
            state.productPolicy |= publishPolicy;
            KisPageStoreDetail::setError(error, {});
            return parent;
        }
        if (metadataBytes < state.bytes.load(std::memory_order_acquire)) {
            KisPageStoreDetail::setError(error, QStringLiteral("PageStore fixed storage exceeds the configured process budget"));
            return {};
        }
        state.limit = metadataBytes;
        state.productPolicy = publishPolicy;
    }
    KisPageBackingLimits limits;
    limits.metadataArenaBytes = metadataBytes;
    parent = std::allocate_shared<KisBackingBudgetController>(
        KisMutationStorageAllocator<KisBackingBudgetController>{}, limits, true);
    std::weak_ptr<KisBackingBudgetController> released;
    {
        QMutexLocker gate(&state.gate);
        released.swap(state.parent);
        state.parent = parent;
    }
    KisPageStoreDetail::setError(error, {});
    return parent;
} catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("PageStore process storage preparation was refused"));
    return {};
}

bool KisBackingBudgetController::ensureProcessStorageBudget(QString *error)
{
    {
        QMutexLocker lock(&m_mutex);
        if (m_sharedNonPayloadBudget) return true;
    }
    const auto parent = kisAcquirePageStoreBootstrapBudget(error);
    return parent && configureSharedNonPayloadBudget(parent, error);
}

std::shared_ptr<KisBackingBudgetController> kisAcquirePageStoreBootstrapBudget(QString *error)
{
    return kisAcquirePageStoreProcessBudget(kisPageProcessStorageLimit(), error, false);
}

void *KisPageProcessStorageObject::operator new(size_t bytes)
{
    return kisAllocatePageProcessStorage(bytes, alignof(std::max_align_t));
}

void KisPageProcessStorageObject::operator delete(void *data) noexcept
{
    kisFreePageProcessStorage(data, 0, alignof(std::max_align_t));
}

std::shared_ptr<std::pmr::memory_resource> kisPageProcessMemoryResource()
{
    class Resource final : public std::pmr::memory_resource {
        void *do_allocate(size_t bytes, size_t alignment) override { return kisAllocatePageProcessStorage(bytes, alignment); }
        void do_deallocate(void *data, size_t bytes, size_t alignment) override { kisFreePageProcessStorage(data, bytes, alignment); }
        bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
    };
    static const auto resource = std::allocate_shared<Resource>(KisMutationStorageAllocator<Resource>{});
    static_assert(sizeof(resource) == sizeof(std::shared_ptr<std::pmr::memory_resource>));
    return resource;
}

KisMutationStorageOwner *kisMutationStorageOwner(KisBackingBudgetController *budget)
{
    if (!budget) return nullptr;
    QMutexLocker preparation(&budget->m_storageOwnerMutex);
    if (!budget->m_storageOwner) {
        void *storage = kisAllocateMutationStorage(
            budget, sizeof(KisMutationStorageOwner), alignof(KisMutationStorageOwner));
        try {
            budget->m_storageOwner.reset(::new (storage) KisMutationStorageOwner(budget));
        } catch (...) {
            kisFreeMutationStorage(budget, storage,
                sizeof(KisMutationStorageOwner), alignof(KisMutationStorageOwner));
            throw;
        }
    }
    return budget->m_storageOwner.get();
}

void *KisMutationStorageOwner::allocate(size_t bytes, size_t alignment)
{
    const KisPageStorageLayout layout(bytes, alignment);
    const size_t capacity = layout.capacity();
    QMutexLocker lock(&m_gate);
    if (!m_controller || !m_accepting || capacity > std::numeric_limits<quint64>::max() - m_bytes)
        throw std::bad_alloc();
    void *data = m_controller->allocateStorage(layout);
    m_bytes += capacity; // Same exact physical capacity already admitted above.
    ref(); // One accounting-lifetime reference per real allocation.
    return data;
}

void KisMutationStorageOwner::deallocate(void *data, size_t, size_t alignment) noexcept
{
    if (!data) return;
    const size_t capacity = kisFreePageStorage(data, alignment);
    refundCharge(capacity, KisBackingBudgetClass::MetadataArena, true);
    deref();
}

void KisMutationStorageOwner::retainLiveCharge(quint64 bytes) noexcept
{
    QMutexLocker lock(&m_gate);
    Q_ASSERT(m_controller && bytes <= std::numeric_limits<quint64>::max() - m_bytes);
    m_bytes += bytes;
}

void KisMutationStorageOwner::releaseLiveCharge(quint64 bytes) noexcept
{
    releaseLiveCharge(bytes, KisBackingBudgetClass::MetadataArena);
}

void KisMutationStorageOwner::releaseLiveCharge(quint64 bytes, KisBackingBudgetClass budgetClass) noexcept
{
    refundCharge(bytes, budgetClass, false);
}

void KisMutationStorageOwner::refundCharge(
    quint64 bytes, KisBackingBudgetClass budgetClass, bool storage) noexcept
{
    {
        QMutexLocker lock(&m_gate);
        Q_ASSERT(m_bytes >= kisPageStorageBytes(this, alignof(KisMutationStorageOwner)) + bytes);
        m_bytes -= bytes;
        if (m_controller) {
            if (storage) m_controller->releaseStorage(bytes);
            else m_controller->releaseLive(budgetClass, KisPageAccessDomain::CpuRam, bytes);
        } else if (m_parent) {
            if (storage) m_parent->releaseSharedStorage(m_childUsage, bytes, true);
            else m_parent->releaseSharedNonPayloadLive(m_childUsage, KisPageAccessDomain::CpuRam, bytes);
        }
    }
}

void KisMutationStorageOwner::detach(
    const std::shared_ptr<KisBackingBudgetController> &parent, quint64 child,
    KisSharedNonPayloadUsage *usage) noexcept
{
    QMutexLocker lock(&m_gate);
    m_controller = nullptr;
    m_parent = parent;
    m_child = child;
    m_childUsage = usage;
}

void KisMutationStorageOwner::stopAllocations() noexcept
{
    QMutexLocker lock(&m_gate);
    m_accepting = false;
}

void KisMutationStorageOwner::deref() noexcept
{
    if (m_references.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    Q_ASSERT(!m_controller);
    const auto expectedCapacity = m_bytes;
    auto parent = std::move(m_parent);
    const quint64 child = m_child;
    auto *usage = m_childUsage;
    this->~KisMutationStorageOwner();
    const size_t capacity = kisFreePageStorage(this, alignof(KisMutationStorageOwner));
    Q_ASSERT(capacity == expectedCapacity);
    Q_UNUSED(expectedCapacity);
    if (parent) {
        parent->releaseSharedStorage(usage, capacity, true);
        parent->unregisterSharedNonPayloadChild(child);
    }
}

void *kisAllocateMutationStorage(KisBackingBudgetController *budget, size_t bytes, size_t alignment)
{
    if (!budget) return kisAllocatePageProcessStorage(bytes, alignment);
    return budget->allocateStorage(KisPageStorageLayout(bytes, alignment));
}

void *KisBackingBudgetController::allocateStorage(const KisPageStorageLayout &layout)
{
    if (!layout.isValid()) throw std::bad_alloc();
    const size_t capacity = layout.capacity();
    auto *budget = this;
    auto *parent = budget->m_sharedNonPayloadBudget.get();
    auto *shared = budget->m_sharedNonPayloadUsage;
    // The original child registration outlives this stack preparation and all
    // returned storage. Consume it directly; do not rediscover it at settle.
    // A normal allocation has one CPU metadata amount, not a general delta.
    const auto prepare = [&](KisBackingBudgetController *owner, KisSharedNonPayloadUsage *child) {
      for (;;) {
        QMutexLocker lock(&owner->m_mutex);
        if (owner->m_usage.waitingRequests) {
            KisBackingBudgetDelta delta;
            delta.bucket(size_t(KisBackingBudgetClass::MetadataArena)).cpuRam = qint64(capacity);
            std::array<quint64, budgetClassCount> aggregate{};
            aggregate[size_t(KisBackingBudgetClass::MetadataArena)] = capacity;
            if (owner->priorWaiterConflictsLocked(delta, aggregate, 0)) {
                ++owner->m_usage.backpressureCount;
                return false;
            }
        }
        quint64 processBytes = 0;
        if (!owner->fitsMetadataStorageLocked(capacity, &processBytes)) {
            lock.unlock();
            if (owner->reclaimMutationStorage(bool(owner->m_sharedNonPayloadBudget))) continue;
            lock.relock();
            ++owner->m_usage.backpressureCount;
            return false;
        }
        auto &metadata = owner->m_usage.buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        owner->m_storageBytes += capacity;
        if (child) child->chargedBytes += capacity;
        auto total = saturatedAdd(saturatedAdd(metadata.live.cpuRam, metadata.reserved.cpuRam),
                                  owner->storageBytesLocked());
        // Growth of process storage also takes this budget gate. Concurrent
        // process releases may only reduce this admitted snapshot's total.
        total = saturatedAdd(total, processBytes);
        metadata.peak.cpuRam = std::max(metadata.peak.cpuRam, total);
        return true;
      }
    };
    bool localPrepared = false;
    bool parentPrepared = false;
    const auto cleanup = qScopeGuard([&] {
        if (localPrepared) budget->releaseStorage(capacity, false);
        else if (parentPrepared) parent->releaseSharedStorage(shared, capacity, false);
    });
    if (parent) {
        Q_ASSERT(shared);
        if (!prepare(parent, shared)) {
            QMutexLocker lock(&budget->m_mutex);
            ++budget->m_usage.backpressureCount;
            throw std::bad_alloc();
        }
        parentPrepared = true;
    }
    if (!prepare(budget, nullptr)) throw std::bad_alloc();
    localPrepared = true;
    void *result = kisAllocatePageStorage(layout);
    // The admitted total does not change on success. Publishing actual live
    // storage needs neither another admission nor a waiter wake; only failure
    // or physical free returns quota. Never hold budget gates across new/mmap.
    budget->m_storageLiveBytes.fetch_add(capacity, std::memory_order_relaxed);
    if (parent) parent->m_storageLiveBytes.fetch_add(capacity, std::memory_order_relaxed);
    localPrepared = false;
    parentPrepared = false;
    return result;
}

void kisFreeMutationStorage(KisBackingBudgetController *budget, void *data, size_t bytes, size_t alignment) noexcept
{
    if (!data) return;
    if (!budget) {
        kisFreePageProcessStorage(data, bytes, alignment);
        return;
    }
    const size_t capacity = kisFreePageStorage(data, alignment);
    budget->releaseStorage(capacity);
}

KisMutationWriteSet::KisMutationWriteSet(KisBackingBudgetController *value)
    : overflow(value), index(std::in_place, Index::allocator_type(KisMutationStorageAllocator<KisPagePublicationSource *>(value))), budget(value) {}

KisMutationWriteSet::KisMutationWriteSet(KisMutationWriteSet &&other) noexcept
    : claimMetadata(std::exchange(other.claimMetadata, nullptr))
    , inlineEntry(std::move(other.inlineEntry))
    , inlineIncarnation(std::exchange(other.inlineIncarnation, 0))
    , nextInlineIncarnation(std::exchange(other.nextInlineIncarnation, 0))
    , overflow(std::move(other.overflow))
    , index(std::move(other.index)), semanticRows(std::move(other.semanticRows)), budget(std::exchange(other.budget, nullptr)), publishedCount(other.publishedCount), indexSeed(other.indexSeed)
{
    rebindInline(other.inlineEntry ? &*other.inlineEntry : nullptr);
    other.inlineEntry.reset();
    other.index.reset();
}

KisMutationWriteSet &KisMutationWriteSet::operator=(KisMutationWriteSet &&other) noexcept
{
    if (this != &other) {
        claimMetadata = std::exchange(other.claimMetadata, nullptr);
        inlineEntry = std::move(other.inlineEntry);
        inlineIncarnation = std::exchange(other.inlineIncarnation, 0);
        nextInlineIncarnation = std::exchange(other.nextInlineIncarnation, 0);
        overflow = std::move(other.overflow);
        index.reset();
        if (other.index) index.emplace(std::move(*other.index));
        semanticRows.reset();
        if (other.semanticRows) semanticRows.emplace(std::move(*other.semanticRows));
        budget = std::exchange(other.budget, nullptr);
        publishedCount = other.publishedCount;
        indexSeed = other.indexSeed;
        indexStorageCapacity = indexStorageBytes = 0;
        semanticStorageCapacity = semanticStorageBytes = 0;
        rebindInline(other.inlineEntry ? &*other.inlineEntry : nullptr);
        other.inlineEntry.reset();
        other.index.reset();
    }
    return *this;
}

KisMutationPageEntry *KisMutationWriteSet::find(const KisPageKey &key)
{
    return const_cast<KisMutationPageEntry *>(std::as_const(*this).find(key));
}

const KisMutationPageEntry *KisMutationWriteSet::find(const KisPageKey &key) const
{
    const size_t position = directoryPosition(key);
    return index && position < index->size()
        ? static_cast<const KisMutationPageEntry *>((*index)[position]) : nullptr;
}

void KisMutationWriteSet::rebindInline(const KisMutationPageEntry *old) noexcept
{
    if (!old || !index) return;
    for (auto &row : *index) if (row == old) row = &*inlineEntry;
}

size_t KisMutationWriteSet::directoryPosition(const KisPageKey &key) const
{
    if (!index) return 0;
    if (directoryHashed()) return directoryBucket(*index, key);
    if (!publishedCount) {
        const auto found = std::find_if(index->begin(), index->end(), [&](const auto *source) {
            return static_cast<const KisMutationPageEntry *>(source)->key() == key;
        });
        return size_t(found - index->begin());
    }
    const auto less = [](const KisPageKey &a, const KisPageKey &b) {
        return std::tie(a.surface.value, a.page.row, a.page.column)
            < std::tie(b.surface.value, b.page.row, b.page.column);
    };
    const size_t split = publishedCount.value_or(index->size());
    for (const auto range : {std::pair<size_t, size_t>{0, split}, {split, index->size()}}) {
        const auto end = index->begin() + range.second;
        const auto found = std::lower_bound(index->begin() + range.first, end, key,
            [&](const auto *row, const auto &key) { return less(static_cast<const KisMutationPageEntry *>(row)->key(), key); });
        if (found != end && static_cast<const KisMutationPageEntry *>(*found)->key() == key)
            return size_t(found - index->begin());
    }
    return index->size();
}

KisMutationWriteSet::EntryIndex KisMutationWriteSet::findIndex(const KisPageKey &key) const
{
    const auto *entry = find(key);
    return entry ? entry->slotIndex : InvalidEntry;
}

KisMutationPageEntry &KisMutationWriteSet::getOrCreate(const KisPageWriteIntent &intent, bool *inserted)
{
    if (inserted) *inserted = false;
    Q_ASSERT(index);
    size_t position = directoryPosition(intent.key);
    if (position < index->size() && (*index)[position])
        return *static_cast<KisMutationPageEntry *>((*index)[position]);
    Q_ASSERT(!publishedCount);
    const bool useInline = !inlineEntry && nextInlineIncarnation;
    const auto raw = useInline ? EntryHandle{} : overflow.prepareSlot();
    const EntryHandle slot = useInline ? EntryHandle{0, nextInlineIncarnation}
                                      : EntryHandle{raw.index + 1, raw.incarnation};
    bool hashed = directoryHashed();
    // One source directory serves admission and the sealed D. Keep append/
    // expected-constant lookup during mutable input; never shift an unbounded
    // sorted suffix for each arriving key. Freeze later sorts pointers once.
    if ((!hashed && size() >= 8) || (hashed && size_t(size()) + 1 > index->size() / 2)) {
        growDirectory(size() + 1);
        position = directoryBucket(*index, intent.key);
        hashed = true;
    } else if (!hashed && index->size() == index->capacity()) {
        index->reserve(index->size() + std::max(size_t(1), size_t(index->size())));
    }
    KisMutationPageEntry *entry;
    if (useInline) {
        inlineEntry.emplace(intent);
        inlineIncarnation = nextInlineIncarnation;
        nextInlineIncarnation = nextInlineIncarnation == std::numeric_limits<quint64>::max() ? 0 : nextInlineIncarnation + 1;
        entry = &*inlineEntry;
    } else entry = overflow.emplacePrepared(raw, intent);
    Q_ASSERT(entry);
    entry->slotIndex = slot.index;
    if (hashed) (*index)[position] = entry;
    else index->push_back(entry);
    if (inserted) *inserted = true;
    return *entry;
}

KisPagePublicationRows KisMutationWriteSet::publicationRows()
{
    Q_ASSERT(index);
    if (!publishedCount) {
        // Reuse the same directory allocation. No page/permission entry moves
        // and no second D index or key/proof snapshot is built.
        size_t used = 0;
        for (auto slot = firstEntry(); slot.isValid(); slot = nextEntry(slot))
            (*index)[used++] = at(slot);
        index->resize(used);
        const auto active = [](const auto *row) {
            const auto *entry = static_cast<const KisMutationPageEntry *>(row);
            return entry->isRemoval() || (entry->coldPage() && entry->coldPage()->reserved);
        };
        const size_t removals = size_t(std::count_if(index->begin(), index->end(), [](const auto *source) {
            return static_cast<const KisMutationPageEntry *>(source)->isRemoval();
        }));
        if (removals) {
            if (!semanticRows)
                semanticRows.emplace(KisMutationStorageAllocator<KisPagePublicationChange>(budget));
            semanticRows->clear();
            semanticRows->reserve(removals);
            for (auto *source : *index) {
                const auto *entry = static_cast<const KisMutationPageEntry *>(source);
                if (!entry->isRemoval()) continue;
                semanticRows->emplace_back(KisPagePublicationChange::Replacement{entry->key(), {}, true});
                source->publicationRow = &semanticRows->back();
            }
        }
        // Classify each source once. Activity checks include the full native
        // handle/layout validation; repeating them inside a sorting comparator
        // turns one eligibility decision into O(log N) validation per row.
        const auto split = std::partition(index->begin(), index->end(), active);
        publishedCount = size_t(split - index->begin());
        const auto less = [](const auto *a, const auto *b) {
            const auto &ka = static_cast<const KisMutationPageEntry *>(a)->key();
            const auto &kb = static_cast<const KisMutationPageEntry *>(b)->key();
            return std::tie(ka.surface.value, ka.page.row, ka.page.column) < std::tie(kb.surface.value, kb.page.row, kb.page.column);
        };
        // Only source pointers move; row facts and acceptance scratch stay put.
        std::sort(index->begin(), split, less);
        std::sort(split, index->end(), less);
    }
    return {index->data(), *publishedCount};
}

KisMutationWriteSet::EntryHandle KisMutationWriteSet::handleAt(EntryIndex entry) const
{
    if (entry == InvalidEntry)
        return {};
    if (entry == 0)
        return inlineEntry ? EntryHandle{0, inlineIncarnation} : EntryHandle{};
    const auto raw = overflow.handleAt(entry - 1);
    return raw.isValid() ? EntryHandle{raw.index + 1, raw.incarnation} : EntryHandle{};
}

KisMutationWriteSet::EntryHandle KisMutationWriteSet::firstEntry() const
{
    if (inlineEntry)
        return {0, inlineIncarnation};
    const auto raw = overflow.first();
    return raw.isValid() ? EntryHandle{raw.index + 1, raw.incarnation} : EntryHandle{};
}

KisMutationWriteSet::EntryHandle KisMutationWriteSet::handleOf(const KisMutationPageEntry &entry) const
{
    Q_ASSERT(at(entry.slotIndex) == &entry);
    return handleAt(entry.slotIndex);
}

KisMutationWriteSet::EntryHandle KisMutationWriteSet::nextEntry(EntryHandle entry) const
{
    if (!entry.isValid() || (entry.index == 0 && (!inlineEntry || inlineIncarnation != entry.incarnation)))
        return {};
    const auto raw = entry.index == 0 ? overflow.first()
        : overflow.next({entry.index - 1, entry.incarnation});
    return raw.isValid() ? EntryHandle{raw.index + 1, raw.incarnation} : EntryHandle{};
}

KisMutationPageEntry *KisMutationWriteSet::at(EntryHandle entry)
{
    return const_cast<KisMutationPageEntry *>(std::as_const(*this).at(entry));
}

const KisMutationPageEntry *KisMutationWriteSet::at(EntryHandle entry) const
{
    if (!entry.isValid())
        return nullptr;
    if (entry.index == 0)
        return inlineEntry && inlineIncarnation == entry.incarnation ? &*inlineEntry : nullptr;
    return overflow.at({entry.index - 1, entry.incarnation});
}

KisMutationPageEntry *KisMutationWriteSet::at(EntryIndex entry)
{
    return const_cast<KisMutationPageEntry *>(std::as_const(*this).at(entry));
}

const KisMutationPageEntry *KisMutationWriteSet::at(EntryIndex entry) const
{
    if (entry == InvalidEntry) return nullptr;
    return entry == 0 ? (inlineEntry ? &*inlineEntry : nullptr) : overflow.at(size_t(entry - 1));
}

bool KisMutationWriteSet::erase(EntryHandle slot)
{
    auto *entry = at(slot);
    if (!entry || entry->isExposed() || entry->coldPage() != nullptr
        || entry->initializationSource())
        return false;
    const auto position = directoryPosition(entry->key());
    Q_ASSERT(index && position < index->size());
    if (directoryHashed()) {
        const size_t mask = index->size() - 1;
        size_t hole = position;
        for (size_t next = (hole + 1) & mask; (*index)[next]; next = (next + 1) & mask) {
            const auto *source = static_cast<const KisMutationPageEntry *>((*index)[next]);
            const size_t home = qHash(source->key(), indexSeed) & mask;
            if (((next - home) & mask) >= ((hole - home) & mask)) {
                (*index)[hole] = (*index)[next]; hole = next;
            }
        }
        (*index)[hole] = nullptr;
    } else index->erase(index->begin() + position);
    if (publishedCount && position < *publishedCount) --*publishedCount;
    if (!slot.index) {
        inlineEntry.reset();
        inlineIncarnation = 0;
        return true;
    }
    return overflow.erase({slot.index - 1, slot.incarnation});
}

void KisMutationWriteSet::reserveKnownTargetCount(qsizetype count)
{
    if (count > qsizetype(InvalidEntry) || count < 0)
        throw std::bad_alloc();
    const qsizetype inlineCapacity = inlineEntry || nextInlineIncarnation ? 1 : 0;
    if (count > inlineCapacity)
        overflow.reserveTogether(size_t(count - inlineCapacity));
    Q_ASSERT(index && !publishedCount);
    if (count > 8 && (!directoryHashed() || size_t(count) > index->size() / 2))
        growDirectory(count);
    else if (!directoryHashed()) index->reserve(size_t(count));
}

void KisMutationWriteSet::clearValues() noexcept
{
    claimMetadata = nullptr;
    if (!size() && !publishedCount) {
        overflow.takeReleasedBlocks().reset();
        return;
    }
    inlineEntry.reset();
    inlineIncarnation = 0;
    overflow.clearValues();
    if (semanticRows) semanticRows->clear();
    publishedCount.reset();
    Q_ASSERT(index);
    // Retain physical capacity, not the preceding phase's logical bucket
    // population. A new small operation starts small in this same allocation.
    index->clear();
}

size_t KisMutationWriteSet::storageBytes() const noexcept
{
    const size_t capacity = index ? index->capacity() : 0;
    if (capacity != indexStorageCapacity) {
        indexStorageCapacity = capacity;
        indexStorageBytes = index && capacity > Index::internal_capacity()
            ? kisPageStorageBytes(index->data(), alignof(KisPagePublicationSource *)) : 0;
    }
    const size_t semanticCapacity = semanticRows ? semanticRows->capacity() : 0;
    if (semanticCapacity != semanticStorageCapacity) {
        semanticStorageCapacity = semanticCapacity;
        semanticStorageBytes = semanticCapacity
            ? kisPageStorageBytes(semanticRows->data(), alignof(KisPagePublicationChange)) : 0;
    }
    return overflow.storageBytes() + indexStorageBytes + semanticStorageBytes;
}

size_t KisMutationWriteSet::directoryBucket(const Index &table, const KisPageKey &key) const noexcept
{
    const size_t mask = table.size() - 1;
    size_t position = qHash(key, indexSeed) & mask;
    while (table[position] && !(static_cast<const KisMutationPageEntry *>(table[position])->key() == key))
        position = (position + 1) & mask;
    return position;
}

void KisMutationWriteSet::growDirectory(qsizetype count)
{
    size_t buckets = 32;
    while (buckets / 2 < size_t(count)) {
        if (buckets > size_t(std::numeric_limits<qint64>::max()) / sizeof(KisPagePublicationSource *) / 2)
            throw std::bad_alloc();
        buckets *= 2;
    }
    const size_t previous = index->size();
    Q_ASSERT(previous <= buckets);
    // Pointer growth has the strong failure guarantee. Only after it succeeds
    // rebuild from the original live rows, independent of directory contents.
    // Reuse already funded capacity and initialize only today's logical range.
    index->resize(buckets, nullptr);
    std::fill_n(index->begin(), previous, nullptr);
    for (auto slot = firstEntry(); slot.isValid(); slot = nextEntry(slot)) {
        auto *entry = at(slot);
        (*index)[directoryBucket(*index, entry->key())] = entry;
    }
}

qsizetype KisMutationWriteSet::size() const
{
    return qsizetype(overflow.size()) + (inlineEntry ? 1 : 0);
}

KisPageWriteAdmission::ClaimSet::~ClaimSet()
{
    release();
}

KisPageWriteAdmission::ClaimSet::ClaimSet(ClaimSet &&other) noexcept
{
    *this = std::move(other);
}

KisPageWriteAdmission::ClaimSet &KisPageWriteAdmission::ClaimSet::operator=(ClaimSet &&other) noexcept
{
    if (this != &other) {
        release();
        owner = std::exchange(other.owner, nullptr);
        writeSet = std::exchange(other.writeSet, nullptr);
        head = std::exchange(other.head, nullptr);
        token = std::exchange(other.token, 0);
    }
    return *this;
}

bool KisPageWriteAdmission::ClaimSet::isValid() const
{
    return owner && token;
}

void KisPageWriteAdmission::ClaimSet::retainClaimsForTerminal() noexcept
{
    Q_ASSERT(isValid());
    writeSet = nullptr;
}

void KisPageWriteAdmission::ClaimSet::release() noexcept
{
    releaseImpl(true);
}

void KisPageWriteAdmission::ClaimSet::releaseLocked() noexcept
{
    releaseImpl(false);
}

void KisPageWriteAdmission::ClaimSet::releaseImpl(bool lockOwner) noexcept
{
    if (owner)
        owner->release(*this, lockOwner);
    owner = nullptr;
    writeSet = nullptr;
    token = 0;
}

KisPageWriteAdmission::KisPageWriteAdmission(KisPageMetadataCoordinator &metadata,
    QMutex &ownerMutex, KisPageWaitCondition &ownerCondition, const bool *operational)
    : m_metadata(&metadata), m_ownerMutex(&ownerMutex), m_ownerCondition(&ownerCondition),
      m_operational(operational)
{
}

qsizetype KisPageWriteAdmission::activeNativeClaimCountLocked() const { return m_nativeCount; }
qsizetype KisPageWriteAdmission::activeGenericClaimCountLocked() const { return m_genericCount; }

bool KisPageWriteAdmission::pageClaimedLocked(const KisPageKey &key) const
{
    const auto *claim = m_metadata->findWriteClaim(key);
    return claim && claim->token;
}

KisPageWriteAdmission::Conflict KisPageWriteAdmission::conflictLocked(
    const KisPageKey &key, Qt::HANDLE requester) const
{
    const auto *found = m_metadata->findWriteClaim(key);
    if (!found || !found->token) return Conflict::None;
    if (found->origin == ClaimOrigin::LegacyAdapter)
        return found->thread == requester ? Conflict::LegacySameThread : Conflict::LegacyBorrower;
    if (found->origin == ClaimOrigin::ManagedRange)
        return found->thread == requester ? Conflict::ManagedSameThread : Conflict::ManagedOtherThread;
    return found->thread == requester ? Conflict::WriterSameThread : Conflict::WriterOtherThread;
}

KisPageWriteAdmission::LegacyInspection KisPageWriteAdmission::inspectLegacyClaimLocked(
    const KisPageKey &key, QString *error) const
{
    if (!key.isValid() || (m_operational && !*m_operational)) {
        KisPageStoreDetail::setError(error, QStringLiteral("legacy mutation store is unavailable"));
        return {};
    }
    LegacyInspection result;
    result.location = m_metadata->findWriteClaim(key);
    if (result.location && result.location->token) {
        KisPageStoreDetail::setError(error, result.location->thread == QThread::currentThreadId()
            ? QStringLiteral("legacy mutation cannot reenter its page")
            : QStringLiteral("legacy mutation conflicts with an active page writer"));
        return {};
    }
    result.owner = this;
    result.key = &key;
    KisPageStoreDetail::setError(error, {});
    return result;
}

KisPageWriteClaim *KisPageWriteAdmission::locate(const KisPageKey &key,
    QMutexLocker<QMutex> &lock, QString *error, bool *unlocked)
{
    if (auto *claim = m_metadata->findWriteClaim(key)) return claim;
    if (unlocked) *unlocked = true;
    lock.unlock();
    auto *claim = m_metadata->prepareWriteClaim(key, error);
    lock.relock();
    // A competing creator may have installed the control during a refused
    // allocation. Its actual claim still decides contention versus failure.
    return claim ? claim : m_metadata->findWriteClaim(key);
}

bool KisPageWriteAdmission::owned(const KisPageWriteClaim *claim, quint64 token, bool generic) const
{
    return claim && claim->token == token && token
        && (claim->origin == ClaimOrigin::GenericWrite) == generic;
}

bool KisPageWriteAdmission::claimDirectLocked(const KisPageKey &key, quint64 ownerToken,
    QMutexLocker<QMutex> &lock, QString *error)
{
    if (!ownerToken || (m_operational && !*m_operational)) {
        KisPageStoreDetail::setError(error, QStringLiteral("write admission is unavailable"));
        return false;
    }
    auto *claim = locate(key, lock, error);
    if (!claim) return false;
    if (m_operational && !*m_operational) {
        KisPageStoreDetail::setError(error, QStringLiteral("write admission is unavailable"));
        return false;
    }
    if (owned(claim, ownerToken, true)) return true;
    if (claim->token) {
        KisPageStoreDetail::setError(error, QStringLiteral("page is claimed by another writer"));
        return false;
    }
    claim->token = ownerToken;
    claim->origin = ClaimOrigin::GenericWrite;
    claim->thread = QThread::currentThreadId();
    ++m_genericCount;
    return true;
}

void KisPageWriteAdmission::releaseDirectLocked(const KisPageKey &key, quint64 ownerToken) noexcept
{
    auto *claim = m_metadata->findWriteClaim(key);
    Q_ASSERT(owned(claim, ownerToken, true));
    if (!owned(claim, ownerToken, true)) return;
    *claim = {};
    --m_genericCount;
    m_ownerCondition->wakeAll();
}

KisPageWriteAdmission::ClaimSet KisPageWriteAdmission::beginClaimSet(KisMutationWriteSet &writeSet)
{
    ClaimSet result;
    if (!m_nextClaimToken || (writeSet.claimMetadata && writeSet.claimMetadata != m_metadata)) return result;
    writeSet.claimMetadata = m_metadata;
    result.owner = this;
    result.writeSet = &writeSet;
    result.token = m_nextClaimToken;
    m_nextClaimToken = m_nextClaimToken == std::numeric_limits<quint64>::max() ? 0 : m_nextClaimToken + 1;
    return result;
}

bool KisPageWriteAdmission::valid(const ClaimSet &claims) const
{
    return claims.isValid() && claims.owner == this && claims.writeSet
        && claims.writeSet->claimMetadata == m_metadata && (!m_operational || *m_operational);
}

void KisPageWriteAdmission::attach(ClaimSet &claims, KisPageWriteClaim &claim, ClaimOrigin origin)
{
    Q_ASSERT(!claim.token && origin != ClaimOrigin::GenericWrite);
    claim.token = claims.token;
    claim.thread = QThread::currentThreadId();
    claim.origin = origin;
    claim.next = claims.head;
    if (claim.next) claim.next->previous = &claim;
    claims.head = &claim;
    ++m_nativeCount;
}

void KisPageWriteAdmission::unlink(ClaimSet &claims, KisPageWriteClaim &claim) noexcept
{
    Q_ASSERT(owned(&claim, claims.token));
    if (claim.previous) claim.previous->next = claim.next;
    else { Q_ASSERT(claims.head == &claim); claims.head = claim.next; }
    if (claim.next) claim.next->previous = claim.previous;
    claim = {};
    --m_nativeCount;
}

template<class Visit>
KisPageWriteAdmission::Result KisPageWriteAdmission::claimEntries(
    ClaimSet &claims, Visit &&visit, QMutexLocker<QMutex> &lock, QString *error, ClaimOrigin origin)
{
    if (!valid(claims) || origin == ClaimOrigin::GenericWrite) {
        KisPageStoreDetail::setError(error, QStringLiteral("write admission claim set is invalid"));
        return Result::Failed;
    }
    bool prepared = true;
    bool contended = false;
    bool unlocked = false;
    size_t missing = 0;
    // Resolve only original rows without a PageControl location. Real cold
    // allocation can release Store; no claim is installed until ALL selected
    // rows have been checked together again under the original Store gate.
    visit([&](KisMutationPageEntry &entry) {
        if (!entry.writeClaim) entry.writeClaim = locate(entry.key(), lock, error, &unlocked);
        if (!entry.writeClaim) { prepared = false; return false; }
        missing += !entry.writeClaim->token;
        if (entry.writeClaim->token && !owned(entry.writeClaim, claims.token)) {
            contended = true;
            return false;
        }
        return true;
    });
    if (!valid(claims)) {
        KisPageStoreDetail::setError(error, QStringLiteral("write admission is unavailable"));
        return Result::Failed;
    }
    // There is no invalidation boundary on the ordinary cached path. Only
    // a real unlocked creation needs the additional full-range revalidation.
    if (unlocked) {
        missing = 0;
        visit([&](KisMutationPageEntry &entry) {
            missing += entry.writeClaim && !entry.writeClaim->token;
            if (entry.writeClaim && entry.writeClaim->token && !owned(entry.writeClaim, claims.token)) {
                contended = true;
                return false;
            }
            return true;
        });
    }
    if (contended) {
        KisPageStoreDetail::setError(error, QStringLiteral("write set intersects another writer"));
        return Result::Contended;
    }
    if (!prepared) return Result::Failed;
    if (!missing) return Result::Acquired;
    visit([&](KisMutationPageEntry &entry) {
        if (!entry.writeClaim->token) attach(claims, *entry.writeClaim, origin);
        return true;
    });
    return Result::Acquired;
}

bool KisPageWriteAdmission::claimOne(ClaimSet &claims, KisMutationWriteSet::EntryHandle slot,
    QMutexLocker<QMutex> &lock, QString *error, ClaimOrigin origin, const LegacyInspection *inspection)
{
    auto *entry = valid(claims) ? claims.writeSet->at(slot) : nullptr;
    if (!entry) {
        KisPageStoreDetail::setError(error, QStringLiteral("write admission entry is invalid"));
        return false;
    }
    if (inspection) {
        if (inspection->owner != this || !inspection->key || !(*inspection->key == entry->key())
            || (entry->writeClaim && entry->writeClaim != inspection->location)) {
            KisPageStoreDetail::setError(error, QStringLiteral("legacy inspection is foreign or names another entry"));
            return false;
        }
        entry->writeClaim = inspection->location;
    }
    return claimEntries(claims, [&](auto &&consume) { consume(*entry); }, lock, error, origin) == Result::Acquired;
}

bool KisPageWriteAdmission::releaseOneLocked(ClaimSet &claims, KisMutationWriteSet::EntryHandle slot) noexcept
{
    if (!claims.isValid() || !claims.writeSet || claims.owner != this) return false;
    const auto *entry = claims.writeSet->at(slot);
    if (!entry || !owned(entry->writeClaim, claims.token)) return false;
    unlink(claims, *entry->writeClaim);
    m_ownerCondition->wakeAll();
    return true;
}

KisPageWriteAdmission::Result KisPageWriteAdmission::claimAll(ClaimSet &claims, QMutexLocker<QMutex> &lock,
    QString *error, ClaimOrigin origin)
{
    return claimEntries(claims, [&](auto &&consume) {
        for (auto slot = claims.writeSet->firstEntry(); slot.isValid(); slot = claims.writeSet->nextEntry(slot))
            if (!consume(*claims.writeSet->at(slot))) break;
    }, lock, error, origin);
}

bool KisPageWriteAdmission::claimRange(ClaimSet &claims, Rows rows,
    QMutexLocker<QMutex> &lock, QString *error)
{
    if (!rows.data || !rows.count || !rows.at) {
        KisPageStoreDetail::setError(error, QStringLiteral("execution claim range is invalid"));
        return false;
    }
    return claimEntries(claims, [&](auto &&consume) {
        for (size_t i = 0; i < rows.count; ++i)
            if (!consume(*rows.at(rows.data, i))) break;
    }, lock, error, ClaimOrigin::NativeSession) == Result::Acquired;
}

bool KisPageWriteAdmission::ownsClaimSetLocked(const ClaimSet &claims) const
{
    if (!claims.isValid() || !claims.writeSet || claims.owner != this) return false;
    for (auto slot = claims.writeSet->firstEntry(); slot.isValid(); slot = claims.writeSet->nextEntry(slot))
        if (!owned(claims.writeSet->at(slot)->writeClaim, claims.token)) return false;
    return true;
}

bool KisPageWriteAdmission::ownsClaimLocked(const ClaimSet &claims, const KisPageKey &key) const
{
    if (!claims.isValid() || claims.owner != this) return false;
    return owned(m_metadata->findWriteClaim(key), claims.token);
}

bool KisPageWriteAdmission::ownsEntryLocked(const ClaimSet &claims, const KisMutationPageEntry &entry) const
{
    // The caller obtained this original entry from claims.writeSet while
    // holding its scope gate; its stable location is not an exported token.
    return claims.isValid() && claims.writeSet && claims.owner == this
        && claims.writeSet->claimMetadata == m_metadata && owned(entry.writeClaim, claims.token);
}

void KisPageWriteAdmission::release(ClaimSet &claims, bool lockOwner) noexcept
{
    std::optional<QMutexLocker<QMutex>> lock;
    if (lockOwner) lock.emplace(m_ownerMutex);
    if (claims.owner != this || !claims.token) return;
    const bool changed = claims.head;
    // Original PageControls form the owner's chain, including abandoned
    // terminal scopes. No row lookup, key rehash or global-table scan remains.
    while (claims.head) unlink(claims, *claims.head);
    if (changed) m_ownerCondition->wakeAll();
}

KisPageWriteCoordinator::KisPageWriteCoordinator(KisPageMetadataCoordinator &metadataValue,
                                                 KisImageEpochReferenceModel &epochValue,
                                                 KisBackingBudgetController &budgetValue,
                                                 KisPageOwnerLedger &ownerLedgerValue)
    : metadata(&metadataValue)
    , epoch(&epochValue)
    , budget(&budgetValue)
    , ownerLedger(&ownerLedgerValue)
    , transferBridges(decltype(transferBridges)::allocator_type(&budgetValue))
    , transactionActivities(&budgetValue)
{
}

bool KisPageWriteCoordinator::validateWriteBase(const KisPageWriteInput &write, bool native, QString *error) const
{
    if (write.baseWasPrepared) {
        if (native) return true; // Checked with the actual target acceptance.
        KisPageMetadataCoordinator::VersionInfo base;
        if (metadata->versionSnapshot(write.baseVersion, &base)
            && base.publication == KisPagePublicationState::Prepared
            && base.preparedBy == write.transaction) return true;
    } else {
        KisPageVersion current;
        if (epoch->resolve(write.baseVersion.key, {}, &current)
            && current == write.baseVersion) return true;
    }
    KisPageStoreDetail::setError(error, QStringLiteral("write base is no longer selected"));
    return false;
}

KisPageMetadataTransitionResult KisPageWriteCoordinator::acquireLeaseWrite(
    KisPageWriteInput &write, const KisReplicaHandle &target)
{
    KisPageMetadataTransitionResult rejected;
    if (!validateWriteBase(write, false, &rejected.rejectionReason)) return rejected;
    // A reader can materialize a virtual before-image during the unlocked
    // provider allocation. Acquire its actual authority pin, not a stale null
    // source; the target still initializes directly from the exact default.
    if (write.baseVersion.isDefaultPixel() && !write.source.isValid()) {
        KisPageMetadataCoordinator::VersionInfo current;
        if (!metadata->versionSnapshot(write.baseVersion, &current)) {
            rejected.rejectionReason = QStringLiteral("write default base disappeared");
            return rejected;
        }
        write.source = current.authority;
    }
    return metadata->applyOwner(write.version.key, write.leaseTransition(target));
}

KisPageMetadataTransitionResult KisPageWriteCoordinator::publishLeaseWrite(KisPageTransition write)
{
    // The original private generation is the acceptance record. A later
    // preparation refusal must not replay its already consumed writer.
    return metadata->publishLeaseWrite(write);
}

bool KisPageWriteCoordinator::tryAcquireReusedTargetLocked(
    const KisPageTransaction &transaction, const KisPageWriteIntent &intent,
    const std::shared_ptr<KisPageReplicaProvider> &provider, KisPageAccessRequirement access,
    KisPageWriteSelection &selection, KisPagePublicationCoordinator &publication,
    KisPageIdleCapacity capacity,
    QMutexLocker<QMutex> &locker, KisReplicaHandle *target, KisCpuWriteBindingReservation &writable,
    KisPageStoreDiagnosticTimer &diagnostic, KisPagePrivateWrite *native)
{
    auto &write = selection.write;
    const auto &descriptor = selection.descriptor();
    const auto &before = selection.recoverableBefore;
    auto &spare = capacity.replica;
    const auto retireUnused = qScopeGuard([&] {
        if (!spare.isValid()) return;
        // Give abandoned optional capacity its first retirement attempt
        // before Fresh admission. Otherwise this operation could be refused
        // by its own idle capacity's charge. Physical refusal still
        // transfers the original funded record to the existing retry queue.
        if (locker.isLocked()) locker.unlock();
        const auto restoreLock = qScopeGuard([&] {
            capacity.binding.reset(); // The last capability must leave before relocking.
            locker.relock();
        });
        publication.m_retirementQueue.retireOrDefer(spare, {}, {}, {}, capacity.original);
    });
    if (intent.inputKind == KisPageWriteInputKind::Semantic || !provider
        || (intent.flags & quint8(KisPageWriteIntentFlag::SourceInitialization))
        || access.domain != KisPageAccessDomain::CpuRam || access.kind != KisPageAccessKind::CpuPointer
        || write.source.domain != KisPageAccessDomain::CpuRam
        || write.baseVersion.isDefaultPixel() || !write.source.isValid()
        || !(provider->providerId() == write.source.provider)
        || !(provider->providerEpoch() == write.source.providerEpoch)
        || writable.isValid()) return false;

    const auto transientLimit = budget->maxTransientVersionsPerPage();
    if (transientLimit != std::numeric_limits<quint32>::max()
        && !metadata->canAddTransientVersion(write.version, transientLimit)) return false;

    const bool reuseCapacity = spare.isValid();
    if (!reuseCapacity && !before.isValid()) return false;

    // Cold short-lived storage only. The existing owners retain every live
    // fact after install; neither adapter receives a second terminal ledger.
    struct Prepared {
        std::shared_ptr<KisPageReplicaProvider> beforeProvider;
        std::shared_ptr<KisCpuResidentBinding> beforeBinding;
        bool beforePinned = false;
        const void *sourceBytes = nullptr;
        KisBackingHandoffReservation accounting;
        struct Publication {
            KisPageMetadataCoordinator::PreparedPublication metadata;
            KisPageMetadataCoordinator::DeferredPublicationCleanup cleanup;
            KisPagePublicationCoordinator::PreparedDescriptors descriptors;
        };
        // Native idle capacity goes straight into the original private result;
        // it has no generic publication preparation or descriptor installation.
        std::optional<Publication> publication;
        ~Prepared()
        {
            if (beforePinned) beforeBinding->releaseRead();
        }
    };
    // A rejected physical observation needs none of the remaining cold state.
    KisCpuBackingHandoff physical;
    std::optional<Prepared> prepared;
    // Even rejected metadata candidates can own real arena/index storage.
    // Destroy them, the before pin and provider references outside owner gates.
    const auto cleanup = qScopeGuard([&] {
        // Preparation may throw while the gate is already open. Qt's locker
        // requires a held lock for unlock(); always restore it for the caller.
        if (locker.isLocked()) locker.unlock();
        diagnostic.next(KisPageStoreDiagnosticPhase::RecoverableCleanup, 1);
        physical.reset();
        prepared.reset();
        locker.relock();
    });

    locker.unlock();
    diagnostic.next(KisPageStoreDiagnosticPhase::RecoverablePrepare, 1);
    // Check the source before pinning the independent before or preparing
    // fallible storage. This observes eligibility but acquires no write claim.
    physical = KisCpuBackingHandoff::prepare(provider, reuseCapacity ? spare : write.source, write.version, descriptor, reuseCapacity ? std::move(capacity.binding) : selection.binding);
    if (!physical.isValid()) return false;
    const auto targetReplica = physical.target();
    prepared.emplace();
    // The caller already retains this exact source provider across the plan
    // and its cleanup. Only an independent provider needs another ledger owner.
    const bool needsSource = !reuseCapacity || (write.writeMode == KisPageWriteMode::PreserveContents
        && !(intent.flags & quint8(KisPageWriteIntentFlag::InputBytesReady)));
    if (needsSource) {
        const auto &source = reuseCapacity ? write.source : before;
        auto *beforeProvider = provider.get();
        if (!(source.provider == write.source.provider) || !(source.providerEpoch == write.source.providerEpoch)) {
            prepared->beforeProvider = ownerLedger->provider(source.provider, source.providerEpoch);
            beforeProvider = prepared->beforeProvider.get();
        }
        if (!beforeProvider) return false;
        prepared->beforeBinding = reuseCapacity && selection.binding
            ? selection.binding : beforeProvider->cpuResidentBinding(source);
        prepared->sourceBytes = prepared->beforeBinding
            ? prepared->beforeBinding->acquireRead(source.allocationIdentity(), true) : nullptr;
        prepared->beforePinned = bool(prepared->sourceBytes);
        if (!prepared->beforePinned) return false;
    }
    // All stable selection facts stay in the original input. Only the source
    // may differ for recovery; the target is owned by physical until accepted.
    const auto &acceptedSource = reuseCapacity ? write.source : before;
    prepared->accounting = ownerLedger->prepareBackingHandoff(reuseCapacity ? spare : write.source,
        targetReplica, nullptr, reuseCapacity ? capacity.original : nullptr);
    if (prepared->accounting.isValid() && !native) {
        auto &publicationPreparation = prepared->publication.emplace();
        auto lease = write.leaseTransition(targetReplica, reuseCapacity
            ? KisPageTransitionKind::AcquireWrite : KisPageTransitionKind::AcquireRecoverableWrite);
        lease.source = acceptedSource;
        publicationPreparation.metadata = reuseCapacity ? metadata->prepareLeaseWrite(transaction, lease)
            : metadata->prepareRecoverableWrite(transaction, lease);
    }
    diagnostic.next(KisPageStoreDiagnosticPhase::RecoverablePrepared, 1);
    locker.relock();
    if (!prepared->accounting.isValid()
        || (!native && !prepared->publication->metadata.isValid())) return false;

    // All fallible storage exists before taking the physical claim. Metadata
    // install rechecks actual blockers and the exact recovery identity.
    if (!native) prepared->publication->descriptors = publication.prepareDescriptorLocked(write.version, descriptor, write.baseVersion);
    diagnostic.next(KisPageStoreDiagnosticPhase::RecoverableInstall, 1);
    if (!validateWriteBase(write, bool(native))) return false;
    if (!physical.tryClaim()) return false;
    const bool installed = native
        ? metadata->acceptPrivateTarget(write, acceptedSource, targetReplica, native, transientLimit, nullptr,
              reuseCapacity ? std::shared_ptr<KisCpuResidentBinding>{} : prepared->beforeBinding)
        : reuseCapacity
        ? metadata->installLeaseWrite(std::move(prepared->publication->metadata), transaction, nullptr, &prepared->publication->cleanup)
        : metadata->installRecoverableWrite(std::move(prepared->publication->metadata), transaction, nullptr, &prepared->publication->cleanup);
    if (!installed) return false;
    writable = physical.commit();
    auto *originalBacking = prepared->accounting.originalRecord();
    const bool accounted = ownerLedger->commitBackingHandoff(std::move(prepared->accounting), writable.binding());
    if (native) native->backing = originalBacking;
    Q_ASSERT(writable.isValid() && accounted);
    Q_UNUSED(accounted);
    spare = {}; // The original ledger now owns the target identity.
    if (!native) publication.installDescriptorAdditionsLocked(&prepared->publication->descriptors);
    diagnostic.next(KisPageStoreDiagnosticPhase::RecoverableInstalled, 1);
    // A discard guard without final input preserves the existing adapter
    // contract: untouched pixels start at the surface default. Complete input
    // bypasses this fill and is written directly by the native payload caller.
    // Retag already owns a pinned writer, so this synchronous kernel cannot
    // require storage, I/O, callbacks or another permission check.
    if (reuseCapacity && needsSource) {
        locker.unlock();
        auto *bytes = static_cast<quint8 *>(writable.pinResident());
        Q_ASSERT(bytes);
        const auto rowBytes = descriptor.minimumRowBytes();
        for (int y = 0; y < descriptor.pageExtent.height(); ++y)
            std::memcpy(bytes + quint64(y) * targetReplica.layout.rowStride,
                static_cast<const quint8 *>(prepared->sourceBytes) + quint64(y) * write.source.layout.rowStride,
                size_t(rowBytes));
        locker.relock();
    } else if (write.writeMode == KisPageWriteMode::DiscardContents
        && !(intent.flags & quint8(KisPageWriteIntentFlag::InputBytesReady))) {
        locker.unlock();
        auto *bytes = static_cast<quint8 *>(writable.pinResident());
        Q_ASSERT(bytes);
        const auto &layout = targetReplica.layout;
        const auto &pixel = descriptor.format.defaultPixel;
        for (int y = 0; y < layout.pageExtent.height(); ++y)
            for (int x = 0; x < layout.pageExtent.width(); ++x)
                std::memcpy(bytes + quint64(y) * layout.rowStride + quint64(x) * pixel.size(),
                            pixel.constData(), size_t(pixel.size()));
        locker.relock();
    }
    if (native) {
        Q_ASSERT(!target && native->target() == targetReplica);
    } else {
        Q_ASSERT(target);
        *target = targetReplica; // Original preallocated request is the owner.
    }
    if (!reuseCapacity) write.source = before;
    return true;
}

KisPageBackingPreparation KisPageWriteCoordinator::reserveBacking(
    const KisPageAllocationDescriptor &descriptor,
    KisPageAccessDomain domain,
    KisBackingBudgetClass budgetClass,
    QString *error,
    const KisPageVersion &target,
    KisPageRetirementRecordPointer *preparedRetirement)
{
    if (!descriptor.isValid() || domain == KisPageAccessDomain::Unknown
        || static_cast<size_t>(budgetClass) >= budgetClassCount
        || (budgetClass == KisBackingBudgetClass::ActivePending && !target.isValid())) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing reservation layout is invalid"));
        return {};
    }
    if (!ownerLedger->synchronizeBackingDomains(error))
        return {};
    const quint64 bytes = descriptor.minimumByteSize();
    if (bytes > quint64(std::numeric_limits<qint64>::max())) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing reservation exceeds signed delta range"));
        return {};
    }
    const quint32 transientLimit = budget->maxTransientVersionsPerPage();
    if (budgetClass == KisBackingBudgetClass::ActivePending
        && transientLimit != std::numeric_limits<quint32>::max()
        && !metadata->canAddTransientVersion(target, transientLimit)) {
        KisPageStoreDetail::setError(error, QStringLiteral("page transient-version budget is exhausted"));
        return {};
    }
    KisBackingBudgetDelta delta;
    auto &bucket = delta.bucket(static_cast<size_t>(budgetClass));
    qint64 *reserved = kisPageDomainComponent(bucket, domain);
    if (!reserved) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing reservation domain is invalid"));
        return {};
    }
    *reserved = qint64(bytes);
    // A fresh physical allocation is not allowed to become unaccounted debt
    // if provider acceptance or metadata adoption fails. Keep one composite
    // reservation through that interval; registerBacking() commits the target
    // class while retaining only this Debt fallback.
    if (budgetClass != KisBackingBudgetClass::RetirementDebt
        && budgetClass != KisBackingBudgetClass::OptionalCache
        && budgetClass != KisBackingBudgetClass::IdleCapacity
        && budgetClass != KisBackingBudgetClass::MetadataArena) {
        auto &debt = delta.bucket(static_cast<size_t>(KisBackingBudgetClass::RetirementDebt));
        *kisPageDomainComponent(debt, domain) = qint64(bytes);
    }
    KisPageBackingPreparation prepared;
    prepared.reservation = budget->reserve(delta, error);
    if (!prepared.reservation.isValid()) return {};
    try {
        KisPageRetirementRecordPointer local;
        if (!preparedRetirement) {
            local = kisPreparePageRetirementRecord(budget);
            preparedRetirement = &local;
        }
        if (!*preparedRetirement || (*preparedRetirement)->storage.budget != budget) {
            KisPageStoreDetail::setError(error, QStringLiteral("prepared backing retirement storage has changed"));
            return {};
        }
        // Physical account and terminal capacity share this original admitted
        // resource allocation, for Fresh and real aliases alike.
        prepared.retirement = std::move(*preparedRetirement);
    } catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing retirement record budget storage was refused"));
        return {};
    }
    return prepared;
}

KisPageMetadataTransitionResult KisPageWriteCoordinator::cancelLeaseWrite(KisPageTransition write)
{
    KisPageMetadataCoordinator::VersionInfo snapshot;
    if (metadata->versionSnapshot(write.version, &snapshot) && snapshot.version.isValid()) {
        write.kind = snapshot.publication == KisPagePublicationState::Prepared
            ? KisPageTransitionKind::AbortPreparedVersion : KisPageTransitionKind::CancelWrite;
        return metadata->applyOwner(write.version.key, write);
    }
    KisPageMetadataTransitionResult rejected;
    rejected.rejectionReason = QStringLiteral("private write version is absent");
    return rejected;
}

KisPageWritePlanKind KisPageWriteCoordinator::select(
    const KisPageWriteIntent &intent,
    const KisMutationPageEntry *pending) const
{
    if (intent.inputKind != KisPageWriteInputKind::Semantic
        && !(intent.flags & quint8(KisPageWriteIntentFlag::InputBytesReady))
        && intent.mode == KisPageWriteMode::PreserveContents) {
        return KisPageWritePlanKind::FreshCow;
    }
    if (intent.inputKind != KisPageWriteInputKind::Semantic) {
        if (intent.flags & quint8(KisPageWriteIntentFlag::InputBytesReady))
            return KisPageWritePlanKind::FreshPayload;
        return intent.mode == KisPageWriteMode::DiscardContents
            ? KisPageWritePlanKind::FreshDiscard
            : KisPageWritePlanKind::FreshCow;
    }
    return pending && pending->preparedTargetVersion().isValid()
        ? KisPageWritePlanKind::ReusePending
        : KisPageWritePlanKind::SemanticOnly;
}

KisCpuReplicaPreparation KisPageWriteCoordinator::prepareFreshCpuReplica(
    const KisPageWriteIntent &intent, KisPageReplicaProvider &provider,
    KisPageWriteSelection &selection, KisPagePriority priority, const KisCpuPagePayload *payload,
    const std::shared_ptr<const KisPageReplicaSource> &initialization) const
{
    using Kind = KisCpuReplicaInput::Kind;
    const auto plan = select(intent);
    const auto kind = intent.inputKind == KisPageWriteInputKind::Semantic ? Kind::Alias
        : plan == KisPageWritePlanKind::FreshPayload ? Kind::Payload
        : initialization ? Kind::SourceCopy
        : plan == KisPageWritePlanKind::FreshCow ? Kind::Copy : Kind::Default;
    auto &write = selection.write;
    return provider.prepareCpuReplica({kind, *ownerLedger, write.operation, write.version, selection.descriptor(),
        write.writeMode, priority, write.source, selection.binding, payload, initialization});
}

KisReplicaOperation KisPageWriteCoordinator::prepareFreshReplica(
    const KisPageWriteIntent &intent, KisPageReplicaProvider &provider,
    const KisPageWriteInput &write, const KisPageAllocationDescriptor &descriptor,
    KisPageAccessRequirement access, KisPagePriority priority,
    const KisCpuPagePayload *payload,
    const std::shared_ptr<const KisPageReplicaSource> &initialization,
    bool *synchronousCopy) const
{
    if (synchronousCopy) *synchronousCopy = false;
    if (intent.inputKind == KisPageWriteInputKind::Semantic)
        return initialization ? provider.prepareSynchronousSource(write.operation, initialization,
            write.version, descriptor, KisReplicaSourceUse::ImmutableAlias, priority) : KisReplicaOperation{};
    const auto plan = select(intent);
    if (plan == KisPageWritePlanKind::FreshPayload)
        return payload ? provider.prepareSynchronousCpuPayload(
            write.operation, write.version, descriptor, *payload, priority) : KisReplicaOperation{};
    if (initialization)
        return provider.prepareSynchronousSource(write.operation, initialization,
            write.version, descriptor, KisReplicaSourceUse::WritableCopy, priority);
    if (plan == KisPageWritePlanKind::FreshCow) {
        const bool copy = !(intent.flags & quint8(KisPageWriteIntentFlag::AsyncLease))
            || (access.kind == KisPageAccessKind::CpuPointer
                && write.source.domain == access.domain
                && write.source.provider == provider.providerId()
                && write.source.providerEpoch == provider.providerEpoch()
                && provider.capabilities().synchronousWriteCopy);
        if (copy) {
            if (synchronousCopy) *synchronousCopy = true;
            return provider.prepareSynchronousWriteCopy(write.operation, write.source,
                write.version, descriptor, priority);
        }
    }
    return provider.prepareWrite(write.operation, write.version, descriptor,
        access.domain, write.writeMode, priority);
}

bool KisPageWriteCoordinator::registerTransferBridge(
    const std::shared_ptr<KisPageReplicaTransferBridge> &bridge) try
{
    if (!bridge || !bridge->synchronousOperations()
        || std::find(transferBridges.cbegin(), transferBridges.cend(), bridge) != transferBridges.cend())
        return false;
    transferBridges.push_back(bridge);
    return true;
}
catch (const std::bad_alloc &) { return false; }

KisCompletionTicket KisPageWriteCoordinator::initializeFreshReplica(
    const KisPageWriteIntent &intent, bool initializedDuringAllocation,
    const KisReplicaHandle &source, const KisReplicaHandle &target,
    const KisPageAllocationDescriptor &descriptor,
    const std::shared_ptr<KisPageReplicaProvider> &targetProvider,
    KisPagePriority priority, const KisCompletionTicket &allocationReadiness,
    QString *error) const
{
    if (select(intent) != KisPageWritePlanKind::FreshCow || initializedDuringAllocation) {
        KisPageStoreDetail::setError(error, {});
        return allocationReadiness;
    }
    const auto sourceProvider = ownerLedger->provider(source.provider, source.providerEpoch);
    if (!sourceProvider || !targetProvider) {
        KisPageStoreDetail::setError(error, QStringLiteral("write transfer provider is unavailable"));
        return {};
    }
    KisReplicaTransferRequest request;
    request.operation = ownerLedger->nextOperationId();
    request.source = source;
    request.target = target;
    request.descriptor = descriptor;
    request.kind = KisReplicaTransferKind::WriteGenerationInitialization;
    if (!request.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("write transfer request is invalid"));
        return {};
    }

    KisReplicaOperation transfer;
    if (request.isSameProviderTransfer()) {
        transfer = targetProvider->transfer(request, priority);
    } else {
        std::shared_ptr<KisPageReplicaTransferBridge> selected;
        for (const auto &bridge : std::as_const(transferBridges)) {
            if (!bridge->supports(request))
                continue;
            if (selected) {
                KisPageStoreDetail::setError(error, QStringLiteral("write transfer bridge is ambiguous"));
                return {};
            }
            selected = bridge;
        }
        if (!selected) {
            KisPageStoreDetail::setError(error, QStringLiteral("write transfer bridge is unavailable"));
            return {};
        }
        transfer = selected->transfer(request, sourceProvider, targetProvider, priority);
    }

    if (!transfer.isValid() || !(transfer.replica == target)) {
        KisPageStoreDetail::setError(error, transfer.error.isEmpty()
            ? QStringLiteral("write generation initialization failed") : transfer.error);
        return {};
    }
    const auto completion = ownerLedger->verifyTerminalProviderResult(
        request.operation, transfer, error);
    return completion.succeeded() ? transfer.completion : KisCompletionTicket{};
}

KisPageVersion KisMutationPageEntry::preparedTargetVersion() const
{
    return state != KisMutationPageEntryState::IntentOnly && coldPageStorage
        ? coldPageStorage->version() : KisPageVersion{};
}

void KisPageWriteCoordinator::recordTarget(KisMutationPageEntry &entry,
                                           const KisPageWriteIntent &intent,
                                           const KisPageWriteInput &write,
                                           KisMutationPageEntryState state)
{
    Q_ASSERT(entry.key() == intent.key && entry.state == KisMutationPageEntryState::IntentOnly);
    Q_ASSERT(write.version.key == entry.key() && !write.version.isDefaultPixel());
    Q_ASSERT(entry.coldPageStorage && entry.coldPageStorage->version() == write.version);
    Q_ASSERT(state == KisMutationPageEntryState::MutableReady
             || state == KisMutationPageEntryState::PrivatePrepared
             || state == KisMutationPageEntryState::AliasAccepted);
    const quint8 semanticRemoval = entry.intent.flags & quint8(KisPageWriteIntentFlag::SemanticRemoval);
    entry.intent = intent;
    entry.intent.flags |= semanticRemoval;
    entry.baseGeneration = write.baseVersion.generation;
    entry.baseDefaultPixelRevision = write.baseVersion.defaultPixelRevision;
    entry.state = state;
}

void KisPageWriteCoordinator::recordPrivateCompletion(KisMutationPageEntry &entry)
{
    Q_ASSERT(entry.state == KisMutationPageEntryState::MutableReady);
    entry.state = KisMutationPageEntryState::PrivatePrepared;
}

void KisPageWriteCoordinator::recordAliasAccepted(KisMutationPageEntry &entry)
{
    Q_ASSERT(entry.state == KisMutationPageEntryState::PrivatePrepared);
    entry.state = KisMutationPageEntryState::AliasAccepted;
}

void KisPageWriteCoordinator::recordCancelled(KisMutationPageEntry &entry)
{
    Q_ASSERT(entry.state == KisMutationPageEntryState::MutableReady || entry.hasPrivateResult());
    entry.baseGeneration = {};
    entry.baseDefaultPixelRevision = 0;
    entry.state = KisMutationPageEntryState::IntentOnly;
}

void KisPageWriteCoordinator::recordExposure(KisMutationPageEntry &entry, bool exposed)
{
    Q_ASSERT(entry.state == (exposed ? KisMutationPageEntryState::MutableReady : KisMutationPageEntryState::Exposed));
    entry.state = exposed ? KisMutationPageEntryState::Exposed : KisMutationPageEntryState::MutableReady;
}

KisPageWriteInput KisPageWriteCoordinator::writeInput(const KisMutationPageEntry &entry,
                                                    KisPageTransactionId transaction) const
{
    const auto *result = entry.coldPageStorage;
    Q_ASSERT(result && entry.state != KisMutationPageEntryState::IntentOnly);
    KisPageWriteInput write;
    write.baseVersion = {entry.key(), entry.baseGeneration, entry.baseDefaultPixelRevision};
    write.version = result->version();
    write.writeMode = entry.intent.mode;
    write.transaction = transaction;
    write.source = result->source;
    return write;
}

bool KisPageWriteCoordinator::beginActivity(KisPageTransactionId transaction,
                                             qsizetype TransactionActivity::*member,
                                             QMutexLocker<QMutex> &lock, QString *error)
{
    TransactionActivity *found = nullptr;
    const auto required = [&]() -> std::optional<size_t> {
        found = transactionActivities.find(transaction.value);
        if (!transaction.isValid() || (found && found->*member == std::numeric_limits<qsizetype>::max())) {
            KisPageStoreDetail::setError(error, QStringLiteral("mutation activity is unavailable or exhausted"));
            return {};
        }
        return found ? 0 : 1;
    };
    return admitMutationRecords(transactionActivities, lock, required, [&](size_t missing) {
        if (missing) {
            TransactionActivity value;
            value.*member = 1;
            const bool inserted = transactionActivities.insertPrepared(transaction.value, value);
            Q_ASSERT(inserted);
        } else {
            ++(found->*member);
            transactionActivities.changed();
        }
    }, error);
}

bool KisPageWriteCoordinator::beginSessionActivity(KisPageTransactionId transaction,
                                                    QMutexLocker<QMutex> &lock, QString *error)
{
    return beginActivity(transaction, &TransactionActivity::sessions, lock, error);
}

void KisPageWriteCoordinator::endSessionActivity(KisPageTransactionId transaction)
{
    endActivity(transaction, &TransactionActivity::sessions);
}

bool KisPageWriteCoordinator::beginPreparationActivity(KisPageTransactionId transaction,
                                                        QMutexLocker<QMutex> &lock, QString *error)
{
    return beginActivity(transaction, &TransactionActivity::preparations, lock, error);
}

void KisPageWriteCoordinator::endPreparationActivity(KisPageTransactionId transaction)
{
    endActivity(transaction, &TransactionActivity::preparations);
}

bool KisPageWriteCoordinator::beginGenericActivity(KisPageTransactionId transaction)
{
    // Generic ownership is handed over from an admitted preparation. This
    // transition must not manufacture a missing activity record after a plan.
    auto *found = transactionActivities.find(transaction.value);
    if (!found || !found->preparations || found->genericWrites == std::numeric_limits<qsizetype>::max())
        return false;
    ++found->genericWrites;
    transactionActivities.changed();
    return true;
}

void KisPageWriteCoordinator::endGenericActivity(KisPageTransactionId transaction)
{
    endActivity(transaction, &TransactionActivity::genericWrites);
}

bool KisPageWriteCoordinator::transactionHasMutationActivity(KisPageTransactionId transaction) const
{
    return transactionActivities.contains(transaction.value);
}

bool KisPageWriteCoordinator::transactionHasSessionOrPreparation(KisPageTransactionId transaction) const
{
    const auto *found = transactionActivities.find(transaction.value);
    return found && (found->sessions > 0 || found->preparations > 0);
}

bool KisPageWriteCoordinator::transactionHasSession(KisPageTransactionId transaction) const
{
    const auto *found = transactionActivities.find(transaction.value);
    return found && found->sessions > 0;
}

bool KisPageWriteCoordinator::transactionHasSoleSession(KisPageTransactionId transaction) const
{
    const auto *found = transactionActivities.find(transaction.value);
    return found && found->sessions == 1 && !found->preparations && !found->genericWrites;
}

void KisPageWriteCoordinator::endActivity(KisPageTransactionId transaction, qsizetype TransactionActivity::*member)
{
    auto *found = transactionActivities.find(transaction.value);
    Q_ASSERT(found && found->*member > 0);
    if (!found || found->*member <= 0) return;
    --(found->*member);
    if (found->isEmpty()) transactionActivities.erase(transaction.value);
    else transactionActivities.changed();
}

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
#ifndef Q_OS_DARWIN
void freePhysicalStorage(void *data, size_t alignment) noexcept
{
    if (alignment > alignof(std::max_align_t)) ::operator delete(data, std::align_val_t(alignment));
    else ::operator delete(data);
}
#endif

#ifdef Q_OS_DARWIN
// Keep fixed native size classes on the common path. Large-cache reuse and
// memalign can return more than malloc_good_size(). Align small payloads inside
// an ordinary prepaid size class; use an exact mapping above those classes.
// Both carry their release facts in the original allocation.
struct PageStorageMapping { void *base; size_t bytes; };
bool pageStorageHasPrefix(size_t bytes, size_t alignment) noexcept
{
    return bytes > 64 * 1024 || alignment > alignof(std::max_align_t);
}
#else
size_t pageStoragePrefix(size_t alignment) noexcept
{
    return std::max(sizeof(size_t), alignment);
}
#endif

// Caller keeps the original owner and its operation alive across unlocks.
// Both callbacks run under the owner gate. required() revalidates the complete
// request; install() consumes constructed buckets and cannot fail or allocate.
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
            install();
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
            install();
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
            install();
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

bool hasPositiveBytes(const KisBackingBudgetDelta &delta)
{
    for (const auto &bucket : delta.buckets)
        for (qint64 value : components(bucket))
            if (value > 0) return true;
    return false;
}

quint64 domainLimit(const KisPageBackingLimits &limits, KisBackingBudgetClass budgetClass, size_t domain)
{
    switch (budgetClass) {
    case KisBackingBudgetClass::Current:
        return components(limits.residentCurrentBytes)[domain];
    case KisBackingBudgetClass::RetainedHistory:
        return components(limits.residentHistoryBytes)[domain];
    case KisBackingBudgetClass::ActivePending:
    case KisBackingBudgetClass::InFlight:
    case KisBackingBudgetClass::RetirementDebt:
    case KisBackingBudgetClass::OptionalCache:
    case KisBackingBudgetClass::MetadataArena:
        return std::numeric_limits<quint64>::max();
    case KisBackingBudgetClass::Count:
        break;
    }
    return 0;
}

quint64 aggregateLimit(const KisPageBackingLimits &limits, KisBackingBudgetClass budgetClass)
{
    switch (budgetClass) {
    case KisBackingBudgetClass::Current:
        return limits.logicalCurrentBytes;
    case KisBackingBudgetClass::RetainedHistory:
        return limits.retainedHistoryBytes;
    case KisBackingBudgetClass::ActivePending:
        return limits.activePendingBytes;
    case KisBackingBudgetClass::InFlight:
        return limits.inFlightReplicaBytes;
    case KisBackingBudgetClass::RetirementDebt:
        return limits.retirementDebtBytes;
    case KisBackingBudgetClass::OptionalCache:
        return limits.optionalCacheBytes;
    case KisBackingBudgetClass::MetadataArena:
        return limits.metadataArenaBytes;
    case KisBackingBudgetClass::Count:
        break;
    }
    return 0;
}

quint64 sumComponents(const std::array<quint64, 4> &values)
{
    quint64 result = 0;
    for (quint64 value : values)
        result = saturatedAdd(result, value);
    return result;
}

bool prepareBudgetChange(const KisBackingBudgetDelta &change, KisBackingBudgetDelta *prepared,
    std::array<quint64, budgetClassCount> *aggregateBytes, quint64 *durableBytes, QString *error)
{
    qint64 durableChange = 0;
    for (size_t i = 0; i < change.buckets.size(); ++i) {
        const auto values = components(change.buckets[i]);
        auto target = components(prepared->buckets[i]);
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
        prepared->buckets[i] = {
            target[0], target[1], target[2], target[3]};
        (*aggregateBytes)[i] = aggregateChange > 0
            ? quint64(aggregateChange) : 0;
        if (__builtin_add_overflow(durableChange, change.buckets[i].ssd,
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
        const size_t bytes = kisPageStorageBytes(m_slots.get(), alignof(ReservationSlot));
        m_slots.reset();
        releaseLive(KisBackingBudgetClass::MetadataArena,
                    KisPageAccessDomain::CpuRam,
                    bytes);
    }
    if (m_storageOwner) {
        m_storageOwner->detach(m_sharedNonPayloadBudget,
                              m_sharedNonPayloadChild);
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

bool KisBackingBudgetController::fitsLocked(
    const KisBackingBudgetDelta &delta,
    const std::array<quint64, budgetClassCount> &aggregateBytes,
    quint64 durableBytes, bool emptyUsage,
    quint64 irreducibleMetadataBytes) const
{
    quint64 liveSsd = 0;
    for (size_t i = 0; i < budgetClassCount; ++i) {
        const auto budgetClass = static_cast<KisBackingBudgetClass>(i);
        auto live = components(emptyUsage ? KisPageDomainBytes{} : m_usage.buckets[i].live);
        const auto reserved = components(emptyUsage ? KisPageDomainBytes{} : m_usage.buckets[i].reserved);
        const auto requested = components(delta.buckets[i]);
        if (emptyUsage && budgetClass == KisBackingBudgetClass::MetadataArena)
            live[0] = irreducibleMetadataBytes;
        if (m_processStorage && budgetClass == KisBackingBudgetClass::MetadataArena)
            live[0] = saturatedAdd(live[0], kisPageProcessStorageBytes());
        for (size_t domain = 0; domain < requested.size(); ++domain) {
            if (requested[domain] < 0) return false;
            const quint64 addition = quint64(requested[domain]);
            const quint64 limit = domainLimit(m_limits, budgetClass, domain);
            if (addition > limit || live[domain] > limit - addition ||
                reserved[domain] > limit - addition - live[domain]) return false;
        }
        const auto limit = aggregateLimit(m_limits, budgetClass);
        const auto current = sumComponents(live);
        const auto pending = emptyUsage ? 0 : m_reservedAggregateBytes[i];
        if (aggregateBytes[i] > limit || current > limit - aggregateBytes[i] ||
            pending > limit - aggregateBytes[i] - current) return false;
        liveSsd = saturatedAdd(liveSsd, live[3]);
    }
    const auto limit = m_limits.durableStoreCapacity;
    return durableBytes <= limit && liveSsd <= limit - durableBytes &&
        (emptyUsage ? 0 : m_reservedDurableBytes) <= limit - durableBytes - liveSsd;
}

bool KisBackingBudgetController::priorWaiterConflictsLocked(
    const KisBackingBudgetDelta &delta,
    const std::array<quint64, budgetClassCount> &aggregateBytes,
    quint64 durableBytes, quint64 beforeSequence) const
{
    if (!m_usage.waitingRequests) return false;
    for (const auto &prior : m_waitContext->requests) {
        if (!prior || prior->granted || prior->sequence >= beforeSequence) continue;
        if (durableBytes && prior->durableBytes &&
            m_limits.durableStoreCapacity != std::numeric_limits<quint64>::max()) return true;
        for (size_t i = 0; i < budgetClassCount; ++i) {
            const auto budgetClass = static_cast<KisBackingBudgetClass>(i);
            if (aggregateBytes[i] && prior->aggregateBytes[i] &&
                aggregateLimit(m_limits, budgetClass) != std::numeric_limits<quint64>::max()) return true;
            const auto requested = components(delta.buckets[i]);
            const auto waiting = components(prior->requested.buckets[i]);
            for (size_t domain = 0; domain < requested.size(); ++domain)
                if (requested[domain] && waiting[domain] &&
                    domainLimit(m_limits, budgetClass, domain) != std::numeric_limits<quint64>::max()) return true;
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
    KisBackingBudgetController *local = owner;
    const KisBackingBudgetDelta shared = local
        ? local->sharedNonPayloadDelta(delta) : KisBackingBudgetDelta{};
    if (local)
        local->commit(cookie, delta);
    if (sharedOwner)
        sharedOwner->commit(sharedCookie, shared);
    clear();
}

void KisBackingBudgetReservation::commitRetaining(
    const KisBackingBudgetDelta &installed,
    const KisBackingBudgetDelta &retained) noexcept
{
    KisBackingBudgetController *local = owner;
    const KisBackingBudgetDelta sharedInstalled = local
        ? local->sharedNonPayloadDelta(installed) : KisBackingBudgetDelta{};
    const KisBackingBudgetDelta sharedRetained = local
        ? local->sharedNonPayloadDelta(retained) : KisBackingBudgetDelta{};
    if (!local || !local->commitRetaining(cookie, installed, retained)) {
        owner = nullptr;
        cookie = 0;
    }
    if (sharedOwner
        && !sharedOwner->commitRetaining(sharedCookie, sharedInstalled, sharedRetained)) {
        sharedOwner = nullptr;
        sharedCookie = 0;
    }
}

bool KisBackingBudgetReservation::retainOnly(
    const KisBackingBudgetDelta &retained) noexcept
{
    KisBackingBudgetController *local = owner;
    const KisBackingBudgetDelta sharedRetained = local
        ? local->sharedNonPayloadDelta(retained) : KisBackingBudgetDelta{};
    if (!local || !local->retainOnly(cookie, retained)) {
        owner = nullptr;
        cookie = 0;
    }
    if (sharedOwner
        && !sharedOwner->retainOnly(sharedCookie, sharedRetained)) {
        sharedOwner = nullptr;
        sharedCookie = 0;
    }
    return owner != nullptr;
}

KisBackingBudgetController::KisBackingBudgetController(const KisPageBackingLimits &limits, bool processStorage)
    : m_limits(limits)
    , m_sharedChildren(std::less<quint64>{}, SharedChildAllocator(this))
    , m_processStorage(processStorage)
{
}

KisBackingBudgetReservation KisBackingBudgetController::reserve(
    const KisBackingBudgetDelta &delta, QString *error)
{
    std::array<quint64, budgetClassCount> aggregateBytes{};
    quint64 durableBytes = 0;
    for (size_t i = 0; i < delta.buckets.size(); ++i) {
        const auto &bucket = delta.buckets[i];
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
    return reserveImpl(prepared, aggregateBytes, durableBytes, error);
}

KisBackingBudgetReservation KisBackingBudgetController::reserveImpl(
    const KisBackingBudgetDelta &delta,
    const std::array<quint64, budgetClassCount> &aggregateBytes,
    quint64 durableBytes, QString *error)
{
    KisBackingBudgetReservation sharedReservation;
    if (m_sharedNonPayloadBudget) {
        const KisBackingBudgetDelta sharedDelta = sharedNonPayloadDelta(delta);
        if (hasPositiveBytes(sharedDelta)) {
            sharedReservation = m_sharedNonPayloadBudget->reserveSharedNonPayload(
                m_sharedNonPayloadChild, sharedDelta, error);
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
        if (!fitsLocked(delta, aggregateBytes, durableBytes))
            return reject(QStringLiteral("backing reservation exceeds its hard budget"));
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
    auto reservation = reserveLocked(delta, aggregateBytes, durableBytes);
    if (sharedReservation.isValid()) {
        reservation.sharedOwner = sharedReservation.owner;
        reservation.sharedCookie = sharedReservation.cookie;
        sharedReservation.clear();
    }
    return reservation;
}

bool KisBackingBudgetController::prepareReservationSlot(QString *error)
{
    // Never hold the accounting gate while waiting for another growth or
    // allocating. The embedded slot funds its own array and the existing
    // parent reservation funds the same actual bytes in the process budget.
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
    const size_t bytes = kisPageStorageAllocationBytes(size_t(payloadBytes), alignof(ReservationSlot));
    if (bytes > size_t(std::numeric_limits<qint64>::max())) return false;
    const size_t oldBytes = kisPageStorageBytes(m_slots.get(), alignof(ReservationSlot));
    KisBackingBudgetDelta storageDelta;
    storageDelta.buckets[size_t(KisBackingBudgetClass::MetadataArena)].cpuRam = qint64(bytes);
    std::array<quint64, budgetClassCount> aggregate{};
    aggregate[size_t(KisBackingBudgetClass::MetadataArena)] = bytes;
    lock.unlock();
    // Any rejected allocation releases its parent/local reservation only
    // after the accounting gate is dropped.
    KisBackingBudgetReservation storage;
    KisBackingBudgetReservation shared;
    if (m_sharedNonPayloadBudget) {
        shared = m_sharedNonPayloadBudget->reserveSharedNonPayload(
            m_sharedNonPayloadChild, storageDelta, error);
        if (!shared.isValid()) {
            lock.relock();
            return m_firstFreeSlot != std::numeric_limits<quint32>::max()
                || m_slotCount < m_slotCapacity;
        }
    }
    lock.relock();
    if (m_firstFreeSlot != std::numeric_limits<quint32>::max()
        || m_slotCount < m_slotCapacity) {
        lock.unlock();
        return true;
    }
    if (priorWaiterConflictsLocked(storageDelta, aggregate, 0)) {
        lock.unlock();
        KisPageStoreDetail::setError(error, QStringLiteral("backing reservation storage waits for an earlier admission request"));
        return false;
    }
    if (!fitsLocked(storageDelta, aggregate, 0)) {
        lock.unlock();
        KisPageStoreDetail::setError(error, QStringLiteral("backing reservation storage exceeds its hard budget"));
        return false;
    }
    Q_ASSERT(!m_storageReservation.active);
    if (!++m_storageReservation.generation) ++m_storageReservation.generation;
    m_storageReservation.active = true;
    const quint64 cookie = (quint64(m_storageReservation.generation) << 32) | maximum;
    activateReservationLocked(cookie, storageDelta, aggregate, 0);
    storage = KisBackingBudgetReservation(this, cookie);
    if (shared.isValid()) {
        storage.sharedOwner = shared.owner;
        storage.sharedCookie = shared.cookie;
        shared.clear();
    }
    lock.unlock();
    decltype(m_slots) candidate;
    try {
        candidate.reset(static_cast<ReservationSlot *>(kisAllocatePageStorage(
            size_t(payloadBytes), alignof(ReservationSlot), bytes)));
        std::uninitialized_value_construct_n(candidate.get(), capacity);
    } catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing reservation storage allocation failed"));
        return false;
    }
    lock.relock();
    // Growth owns no payload or callbacks; these trivial records may be moved
    // under the gate without allocation or destruction. Cookies stay stable.
    static_assert(std::is_trivially_destructible_v<ReservationSlot>);
    if (m_slotCount) std::copy_n(m_slots.get(), m_slotCount, candidate.get());
    m_slots.swap(candidate);
    m_slotCapacity = capacity;
    lock.unlock();
    storage.commit(storageDelta);
    candidate.reset();
    if (oldBytes) releaseLive(KisBackingBudgetClass::MetadataArena,
                              KisPageAccessDomain::CpuRam, oldBytes);
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisBackingBudgetReservation KisBackingBudgetController::reserveLocked(
    const KisBackingBudgetDelta &delta,
    const std::array<quint64, budgetClassCount> &aggregateBytes,
    quint64 durableBytes,
    quint64 sharedChild)
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
    activateReservationLocked(cookie, delta, aggregateBytes, durableBytes, sharedChild);
    return {this, cookie};
}

void KisBackingBudgetController::activateReservationLocked(
    quint64 cookie, const KisBackingBudgetDelta &delta,
    const std::array<quint64, budgetClassCount> &aggregateBytes,
    quint64 durableBytes,
    quint64 sharedChild)
{
    auto *slot = activeReservation(cookie);
    Q_ASSERT(slot && !hasPositiveBytes(slot->delta));
    slot->delta = delta;
    slot->aggregateBytes = aggregateBytes;
    slot->durableBytes = durableBytes;
    slot->sharedChild = sharedChild;
    slot->active = true;
    for (size_t bucketIndex = 0; bucketIndex < budgetClassCount; ++bucketIndex) {
        m_reservedAggregateBytes[bucketIndex] = saturatedAdd(
            m_reservedAggregateBytes[bucketIndex], aggregateBytes[bucketIndex]);
    }
    m_reservedDurableBytes = saturatedAdd(m_reservedDurableBytes, durableBytes);
    if (sharedChild) {
        auto child = m_sharedChildren.find(sharedChild);
        Q_ASSERT(child != m_sharedChildren.end());
        if (child != m_sharedChildren.end()) {
            child->second.reservedBytes = saturatedAdd(
                child->second.reservedBytes,
                aggregateBytes[size_t(KisBackingBudgetClass::MetadataArena)]);
        }
    }

    for (size_t bucketIndex = 0; bucketIndex < budgetClassCount; ++bucketIndex) {
        auto reserved = components(m_usage.buckets[bucketIndex].reserved);
        auto peak = components(m_usage.buckets[bucketIndex].peak);
        const auto live = components(m_usage.buckets[bucketIndex].live);
        const auto requested = components(delta.buckets[bucketIndex]);
        for (size_t domain = 0; domain < requested.size(); ++domain) {
            reserved[domain] += quint64(requested[domain]);
            auto total = saturatedAdd(live[domain], reserved[domain]);
            if (m_processStorage && bucketIndex == size_t(KisBackingBudgetClass::MetadataArena) && domain == 0)
                total = saturatedAdd(total, kisPageProcessStorageBytes());
            peak[domain] = std::max(peak[domain], total);
        }
        m_usage.buckets[bucketIndex].reserved = fromComponents(reserved);
        m_usage.buckets[bucketIndex].peak = fromComponents(peak);
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
    if (m_processStorage) {
        auto &metadata = result.buckets[size_t(KisBackingBudgetClass::MetadataArena)];
        metadata.live.cpuRam = saturatedAdd(metadata.live.cpuRam, kisPageProcessStorageBytes());
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
    const qint64 *reserved = kisPageDomainComponent(
        slot->delta.buckets[classIndex], domain);
    return reserved && *reserved >= qint64(bytes);
}

bool KisBackingBudgetController::reservationCovers(
    quint64 cookie, const KisBackingBudgetDelta &needed) const
{
    QMutexLocker lock(&m_mutex);
    const ReservationSlot *slot = activeReservation(cookie);
    if (!slot) return false;
    for (size_t i = 0; i < budgetClassCount; ++i) {
        const auto reserved = components(slot->delta.buckets[i]);
        const auto requested = components(needed.buckets[i]);
        for (size_t domain = 0; domain < requested.size(); ++domain)
            if (requested[domain] < 0 || requested[domain] > reserved[domain]) return false;
    }
    return true;
}

bool KisBackingBudgetController::configureLimits(const KisPageBackingLimits &limits,
                                                  QString *error)
{
    QMutexLocker lock(&m_mutex);
    if (m_usage.waitingRequests || m_usage.grantedRequests
        || !m_sharedChildren.empty()) {
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
    if (m_sharedNonPayloadBudget || !m_sharedChildren.empty()
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
    const quint64 child = parent->registerSharedNonPayloadChild(error);
    if (!child) return false;
    m_sharedNonPayloadBudget = parent;
    m_sharedNonPayloadChild = child;
    KisPageStoreDetail::setError(error, {});
    return true;
}

quint64 KisBackingBudgetController::registerSharedNonPayloadChild(QString *error)
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
        Q_ASSERT(found->second.liveBytes == 0 && found->second.reservedBytes == 0);
        if (found->second.liveBytes || found->second.reservedBytes) return;
        released = m_sharedChildren.extract(found);
    }
}

KisBackingBudgetReservation KisBackingBudgetController::reserveSharedNonPayload(
    quint64 child,
    const KisBackingBudgetDelta &delta,
    QString *error)
{
    std::array<quint64, budgetClassCount> aggregateBytes{};
    quint64 durableBytes = 0;
    for (size_t i = 0; i < delta.buckets.size(); ++i) {
        const auto &bucket = delta.buckets[i];
        for (qint64 value : components(bucket)) {
            if (value < 0) {
                KisPageStoreDetail::setError(
                    error, QStringLiteral("shared non-payload reservation contains a negative delta"));
                return {};
            }
            aggregateBytes[i] = saturatedAdd(aggregateBytes[i], quint64(value));
        }
        if (bucket.ssd > 0)
            durableBytes = saturatedAdd(durableBytes, quint64(bucket.ssd));
    }

    QMutexLocker lock(&m_mutex);
    const auto reject = [&](const QString &message) {
        ++m_usage.backpressureCount;
        KisPageStoreDetail::setError(error, message);
        return KisBackingBudgetReservation{};
    };
    const auto found = m_sharedChildren.find(child);
    if (!child || found == m_sharedChildren.end())
        return reject(QStringLiteral("shared non-payload store registration is absent"));
    for (;;) {
        if (priorWaiterConflictsLocked(delta, aggregateBytes, durableBytes))
            return reject(QStringLiteral("shared non-payload reservation waits for an earlier request"));
        if (!fitsLocked(delta, aggregateBytes, durableBytes))
            return reject(QStringLiteral("shared non-payload process budget is exhausted"));
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
    return reserveLocked(delta, aggregateBytes, durableBytes, child);
}

KisBackingBudgetDelta KisBackingBudgetController::sharedNonPayloadDelta(
    const KisBackingBudgetDelta &delta) noexcept
{
    KisBackingBudgetDelta result;
    const auto metadata = components(delta.buckets[size_t(KisBackingBudgetClass::MetadataArena)]);
    const auto cache = components(delta.buckets[size_t(KisBackingBudgetClass::OptionalCache)]);
    auto combined = components(result.buckets[size_t(KisBackingBudgetClass::MetadataArena)]);
    for (size_t domain = 0; domain < combined.size(); ++domain) {
        if (cache[domain] > 0 && metadata[domain] > std::numeric_limits<qint64>::max() - cache[domain])
            combined[domain] = std::numeric_limits<qint64>::max();
        else if (cache[domain] < 0 && metadata[domain] < std::numeric_limits<qint64>::min() - cache[domain])
            combined[domain] = std::numeric_limits<qint64>::min();
        else
            combined[domain] = metadata[domain] + cache[domain];
    }
    auto &output = result.buckets[size_t(KisBackingBudgetClass::MetadataArena)];
    output.cpuRam = combined[0];
    output.umaShared = combined[1];
    output.discreteVram = combined[2];
    output.ssd = combined[3];
    return result;
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
        notifyWaitersLocked();
    }
    if (m_sharedNonPayloadBudget
        && (budgetClass == KisBackingBudgetClass::MetadataArena
            || budgetClass == KisBackingBudgetClass::OptionalCache)) {
        m_sharedNonPayloadBudget->releaseSharedNonPayloadLive(
            m_sharedNonPayloadChild, domain, bytes);
    }
}

void KisBackingBudgetController::releaseSharedNonPayloadLive(
    quint64 child,
    KisPageAccessDomain domain,
    quint64 bytes) noexcept
{
    QMutexLocker lock(&m_mutex);
    auto found = m_sharedChildren.find(child);
    Q_ASSERT(found != m_sharedChildren.end() && found->second.liveBytes >= bytes);
    if (found == m_sharedChildren.end()) return;
    found->second.liveBytes = bytes > found->second.liveBytes
        ? 0 : found->second.liveBytes - bytes;
    quint64 *live = kisPageDomainComponent(
        m_usage.buckets[size_t(KisBackingBudgetClass::MetadataArena)].live, domain);
    if (!live) return;
    Q_ASSERT(*live >= bytes);
    *live = bytes > *live ? 0 : *live - bytes;
    notifyWaitersLocked();
}

void KisBackingBudgetController::commit(quint64 cookie, const KisBackingBudgetDelta &delta) noexcept
{
    commitRetaining(cookie, delta, {});
}

bool KisBackingBudgetController::commitRetaining(
    quint64 cookie,
    const KisBackingBudgetDelta &installed,
    const KisBackingBudgetDelta &retained) noexcept
{
    QMutexLocker lock(&m_mutex);
    const quint32 slotIndex = quint32(cookie);
    ReservationSlot *slot = activeReservation(cookie);
    if (!slot) return 0;

    SharedChildUsage *sharedUsage = nullptr;
    if (slot->sharedChild) {
        auto child = m_sharedChildren.find(slot->sharedChild);
        Q_ASSERT(child != m_sharedChildren.end());
        if (child != m_sharedChildren.end()) sharedUsage = &child->second;
    }

    for (size_t bucketIndex = 0; bucketIndex < budgetClassCount; ++bucketIndex) {
        Q_ASSERT(m_reservedAggregateBytes[bucketIndex]
                 >= slot->aggregateBytes[bucketIndex]);
        m_reservedAggregateBytes[bucketIndex] -= slot->aggregateBytes[bucketIndex];
    }
    Q_ASSERT(m_reservedDurableBytes >= slot->durableBytes);
    m_reservedDurableBytes -= slot->durableBytes;

    for (size_t bucketIndex = 0; bucketIndex < budgetClassCount; ++bucketIndex) {
        auto live = components(m_usage.buckets[bucketIndex].live);
        auto peak = components(m_usage.buckets[bucketIndex].peak);
        auto reserved = components(m_usage.buckets[bucketIndex].reserved);
        const auto prepared = components(slot->delta.buckets[bucketIndex]);
        const auto keep = components(retained.buckets[bucketIndex]);
        const auto commit = components(installed.buckets[bucketIndex]);
        for (size_t domain = 0; domain < prepared.size(); ++domain) {
            Q_ASSERT(prepared[domain] >= 0 && keep[domain] >= 0
                     && keep[domain] <= prepared[domain]);
            reserved[domain] -= quint64(prepared[domain] - keep[domain]);
            if (commit[domain] < 0) {
                const quint64 removal = quint64(-commit[domain]);
                Q_ASSERT(live[domain] >= removal);
                live[domain] = removal > live[domain] ? 0 : live[domain] - removal;
            } else {
                live[domain] = saturatedAdd(live[domain], quint64(commit[domain]));
                peak[domain] = std::max(peak[domain], live[domain]);
            }
        }
        m_usage.buckets[bucketIndex].live = fromComponents(live);
        m_usage.buckets[bucketIndex].peak = fromComponents(peak);
        m_usage.buckets[bucketIndex].reserved = fromComponents(reserved);
    }
    if (sharedUsage) {
        const size_t metadata = size_t(KisBackingBudgetClass::MetadataArena);
        quint64 retainedBytes = 0;
        for (qint64 value : components(retained.buckets[metadata])) {
            if (value > 0)
                retainedBytes = saturatedAdd(retainedBytes, quint64(value));
        }
        const quint64 preparedBytes = slot->aggregateBytes[metadata];
        Q_ASSERT(preparedBytes >= retainedBytes
                 && sharedUsage->reservedBytes >= preparedBytes - retainedBytes);
        sharedUsage->reservedBytes -= std::min(
            sharedUsage->reservedBytes, preparedBytes - retainedBytes);
        for (qint64 value : components(installed.buckets[metadata])) {
            if (value < 0) {
                const quint64 removal = quint64(-value);
                Q_ASSERT(sharedUsage->liveBytes >= removal);
                sharedUsage->liveBytes = removal > sharedUsage->liveBytes
                    ? 0 : sharedUsage->liveBytes - removal;
            } else {
                sharedUsage->liveBytes = saturatedAdd(
                    sharedUsage->liveBytes, quint64(value));
            }
        }
    }
    slot->delta = retained;
    slot->aggregateBytes = {};
    slot->durableBytes = 0;
    for (size_t i = 0; i < retained.buckets.size(); ++i) {
        const auto &bucket = retained.buckets[i];
        for (qint64 value : components(bucket)) {
            if (value > 0) {
                slot->aggregateBytes[i] = saturatedAdd(
                    slot->aggregateBytes[i], quint64(value));
            }
        }
        if (bucket.ssd > 0)
            slot->durableBytes = saturatedAdd(
                slot->durableBytes, quint64(bucket.ssd));
        m_reservedAggregateBytes[i] = saturatedAdd(
            m_reservedAggregateBytes[i], slot->aggregateBytes[i]);
    }
    m_reservedDurableBytes = saturatedAdd(
        m_reservedDurableBytes, slot->durableBytes);
    const bool retainedReservation = hasPositiveBytes(retained);
    if (!retainedReservation) {
        slot->active = false;
        slot->aggregateBytes = {};
        slot->durableBytes = 0;
        slot->sharedChild = 0;
        if (slotIndex != std::numeric_limits<quint32>::max()) {
            slot->nextFree = m_firstFreeSlot;
            m_firstFreeSlot = slotIndex;
        }
    }
    notifyWaitersLocked();
    return retainedReservation;
}

void KisBackingBudgetController::moveRetirementHeadroom(
    quint64 cookie, KisPageAccessDomain source, KisPageAccessDomain target,
    quint64 bytes) noexcept
{
    QMutexLocker lock(&m_mutex);
    auto *slot = activeReservation(cookie);
    const size_t debt = size_t(KisBackingBudgetClass::RetirementDebt);
    Q_ASSERT(slot && slot->durableBytes == 0 && slot->aggregateBytes[debt] == bytes);
    if (!slot) return;
    auto *before = kisPageDomainComponent(slot->delta.buckets[debt], source);
    auto *after = kisPageDomainComponent(slot->delta.buckets[debt], target);
    Q_ASSERT(before && after && before != after && *before == qint64(bytes) && *after == 0);
    *before = 0;
    *after = qint64(bytes);
    auto &usage = m_usage.buckets[debt];
    *kisPageDomainComponent(usage.reserved, source) -= bytes;
    auto *reserved = kisPageDomainComponent(usage.reserved, target);
    *reserved += bytes;
    auto *peak = kisPageDomainComponent(usage.peak, target);
    *peak = std::max(*peak, saturatedAdd(*reserved, *kisPageDomainComponent(usage.live, target)));
    // Aggregate headroom is unchanged. Its SSD component reserves a future
    // class change of the same physical bytes, not another durable payload.
}

bool KisBackingBudgetController::retainOnly(
    quint64 cookie,
    const KisBackingBudgetDelta &retained) noexcept
{
    return commitRetaining(cookie, {}, retained);
}

void KisBackingBudgetController::release(quint64 cookie) noexcept
{
    commitRetaining(cookie, {}, {});
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
#else
    const size_t prefix = pageStoragePrefix(alignment);
    if (prefix > maximum || bytes > maximum - prefix) return size_t(-1);
    capacity = bytes + prefix;
#endif
    return capacity;
}

void *kisAllocatePageStorage(size_t bytes, size_t alignment, size_t capacity)
{
    if (!alignment || (alignment & (alignment - 1))
        || capacity > size_t(std::numeric_limits<qint64>::max())
        || capacity != kisPageStorageAllocationBytes(bytes, alignment)) throw std::bad_alloc();
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
#else
    const auto *raw = static_cast<const std::byte *>(data) - pageStoragePrefix(alignment);
    return *reinterpret_cast<const size_t *>(raw);
#endif
}

size_t kisFreePageStorage(void *data, size_t alignment) noexcept
{
    if (!data) return 0;
    const size_t capacity = kisPageStorageBytes(data, alignment);
#ifdef Q_OS_DARWIN
    if (alignment > alignof(std::max_align_t) || !malloc_size(data)) {
        const auto mapping = *reinterpret_cast<const PageStorageMapping *>(
            static_cast<const std::byte *>(data) - sizeof(PageStorageMapping));
        if (malloc_size(mapping.base)) {
            ::operator delete(mapping.base);
            return capacity;
        }
        const int result = munmap(mapping.base, mapping.bytes);
        Q_ASSERT(result == 0);
        return result == 0 ? capacity : 0;
    }
    ::operator delete(data);
#else
    auto *raw = static_cast<std::byte *>(data) - pageStoragePrefix(alignment);
    freePhysicalStorage(raw, alignment);
#endif
    return capacity;
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
    {
        QMutexLocker gate(&state.gate);
        parent = state.parent.lock();
        const auto current = state.bytes.load(std::memory_order_relaxed);
        if (bytes > state.limit || current > state.limit - bytes) throw std::bad_alloc();
        if (parent) {
            QMutexLocker budget(&parent->m_mutex);
            KisBackingBudgetDelta delta;
            delta.buckets[size_t(KisBackingBudgetClass::MetadataArena)].cpuRam = qint64(bytes);
            std::array<quint64, budgetClassCount> aggregate{};
            aggregate[size_t(KisBackingBudgetClass::MetadataArena)] = bytes;
            if (!parent->fitsLocked(delta, aggregate, 0)) {
                ++parent->m_usage.backpressureCount;
                throw std::bad_alloc();
            }
            state.bytes.store(current + bytes, std::memory_order_release);
            auto &metadata = parent->m_usage.buckets[size_t(KisBackingBudgetClass::MetadataArena)];
            metadata.peak.cpuRam = std::max(metadata.peak.cpuRam,
                saturatedAdd(saturatedAdd(metadata.live.cpuRam, metadata.reserved.cpuRam), current + bytes));
        } else {
            state.bytes.store(current + bytes, std::memory_order_release);
        }
    }
}

void *kisAllocatePageProcessStorage(size_t bytes, size_t alignment)
{
    const size_t capacity = kisPageStorageAllocationBytes(bytes, alignment);
    kisReservePageProcessStorage(capacity);
    try {
        return kisAllocatePageStorage(bytes, alignment, capacity);
    } catch (...) {
        kisReleasePageProcessStorage(capacity);
        throw;
    }
}

void kisReleasePageProcessStorage(size_t bytes) noexcept
{
    auto &state = processStorage();
    std::shared_ptr<KisBackingBudgetController> parent;
    {
        QMutexLocker gate(&state.gate);
        parent = state.parent.lock();
        const auto current = state.bytes.load(std::memory_order_relaxed);
        Q_ASSERT(current >= ProcessStorageState::fixedStorageBytes() + bytes);
        state.bytes.store(current - bytes, std::memory_order_release);
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
    kisReleasePageProcessStorage(capacity);
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
    const size_t capacity = kisPageStorageAllocationBytes(bytes, alignment);
    QMutexLocker lock(&m_gate);
    if (!m_controller || !m_accepting || capacity > std::numeric_limits<quint64>::max() - m_bytes)
        throw std::bad_alloc();
    void *data = kisAllocateMutationStorage(m_controller, bytes, alignment);
    m_bytes += kisPageStorageBytes(data, alignment);
    ref(); // One accounting-lifetime reference per real allocation.
    return data;
}

void KisMutationStorageOwner::deallocate(void *data, size_t, size_t alignment) noexcept
{
    if (!data) return;
    const size_t capacity = kisFreePageStorage(data, alignment);
    releaseLiveCharge(capacity);
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
    {
        QMutexLocker lock(&m_gate);
        Q_ASSERT(m_bytes >= kisPageStorageBytes(this, alignof(KisMutationStorageOwner)) + bytes);
        m_bytes -= bytes;
        if (m_controller)
            m_controller->releaseLive(budgetClass,
                                      KisPageAccessDomain::CpuRam, bytes);
        else if (m_parent)
            m_parent->releaseSharedNonPayloadLive(m_child, KisPageAccessDomain::CpuRam, bytes);
    }
}

void KisMutationStorageOwner::detach(
    const std::shared_ptr<KisBackingBudgetController> &parent, quint64 child) noexcept
{
    QMutexLocker lock(&m_gate);
    m_controller = nullptr;
    m_parent = parent;
    m_child = child;
}

void KisMutationStorageOwner::stopAllocations() noexcept
{
    QMutexLocker lock(&m_gate);
    m_accepting = false;
}

void KisMutationStorageOwner::deref() noexcept
{
    if (m_references.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    const size_t capacity = kisPageStorageBytes(this, alignof(KisMutationStorageOwner));
    Q_ASSERT(!m_controller && m_bytes == capacity);
    auto parent = std::move(m_parent);
    const quint64 child = m_child;
    this->~KisMutationStorageOwner();
    kisFreePageStorage(this, alignof(KisMutationStorageOwner));
    if (parent) {
        parent->releaseSharedNonPayloadLive(child, KisPageAccessDomain::CpuRam,
                                          capacity);
        parent->unregisterSharedNonPayloadChild(child);
    }
}

void *kisAllocateMutationStorage(KisBackingBudgetController *budget, size_t bytes, size_t alignment)
{
    if (!budget) return kisAllocatePageProcessStorage(bytes, alignment);
    const size_t capacity = kisPageStorageAllocationBytes(bytes, alignment);
    if (capacity > size_t(std::numeric_limits<qint64>::max()))
        throw std::bad_alloc();
    KisBackingBudgetDelta delta;
    delta.buckets[size_t(KisBackingBudgetClass::MetadataArena)].cpuRam = qint64(capacity);
    auto reservation = budget->reserve(delta, nullptr);
    if (!reservation.isValid())
        throw std::bad_alloc();
    void *result = kisAllocatePageStorage(bytes, alignment, capacity);
    if (reservation.isValid())
        budget->commitReservation(std::move(reservation), delta);
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
    budget->releaseLive(KisBackingBudgetClass::MetadataArena, KisPageAccessDomain::CpuRam, capacity);
}

KisMutationWriteSet::KisMutationWriteSet(KisBackingBudgetController *value)
    : overflow(value), budget(value) {}

KisMutationWriteSet::KisMutationWriteSet(KisMutationWriteSet &&other) noexcept
    : inlineEntry(std::move(other.inlineEntry))
    , inlineIncarnation(std::exchange(other.inlineIncarnation, 0))
    , nextInlineIncarnation(std::exchange(other.nextInlineIncarnation, 0))
    , overflow(std::move(other.overflow))
    , index(std::move(other.index)), budget(std::exchange(other.budget, nullptr))
{
    other.inlineEntry.reset();
    other.index.reset();
}

KisMutationWriteSet &KisMutationWriteSet::operator=(KisMutationWriteSet &&other) noexcept
{
    if (this != &other) {
        inlineEntry = std::move(other.inlineEntry);
        inlineIncarnation = std::exchange(other.inlineIncarnation, 0);
        nextInlineIncarnation = std::exchange(other.nextInlineIncarnation, 0);
        overflow = std::move(other.overflow);
        index = std::move(other.index);
        budget = std::exchange(other.budget, nullptr);
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
    const EntryIndex found = findIndex(key);
    return found == InvalidEntry ? nullptr : at(found);
}

KisMutationWriteSet::EntryIndex KisMutationWriteSet::findIndex(const KisPageKey &key) const
{
    if (index) {
        const auto found = index->find(key);
        return found != index->end() && at(found->second) ? found->second.index : InvalidEntry;
    }
    for (auto slot = firstEntry(); slot.isValid(); slot = nextEntry(slot)) {
        if (at(slot)->key() == key)
            return slot.index;
    }
    return InvalidEntry;
}

KisMutationPageEntry &KisMutationWriteSet::getOrCreate(const KisPageWriteIntent &intent)
{
    if (KisMutationPageEntry *found = find(intent.key))
        return *found;
    const bool useInline = !inlineEntry && nextInlineIncarnation;
    const auto raw = useInline ? EntryHandle{} : overflow.prepareSlot();
    const EntryHandle slot = useInline ? EntryHandle{0, nextInlineIncarnation}
                                      : EntryHandle{raw.index + 1, raw.incarnation};
    // All fallible index work precedes the noexcept entry construction. The
    // owner serializes preparation and installation; no candidate survives an
    // unlock or a second mutation of this set.
    if (!index && size() >= 8) {
        auto prepared = prepareIndex(size() + 1);
        prepared.emplace(intent.key, slot);
        index.emplace(std::move(prepared));
    } else if (index) {
        index->emplace(intent.key, slot);
    }
    if (useInline) {
        inlineEntry.emplace(intent);
        inlineIncarnation = nextInlineIncarnation;
        nextInlineIncarnation = nextInlineIncarnation == std::numeric_limits<quint64>::max()
            ? 0 : nextInlineIncarnation + 1;
        return *inlineEntry;
    }
    auto *entry = overflow.emplacePrepared(raw, intent);
    Q_ASSERT(entry);
    return *entry;
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

KisMutationWriteSet::EntryHandle KisMutationWriteSet::nextEntry(EntryHandle entry) const
{
    if (!at(entry))
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
    return at(handleAt(entry));
}

const KisMutationPageEntry *KisMutationWriteSet::at(EntryIndex entry) const
{
    return at(handleAt(entry));
}

bool KisMutationWriteSet::erase(EntryHandle slot)
{
    auto *entry = at(slot);
    if (!entry || entry->isExposed() || entry->coldPage() != InvalidEntry
        || entry->initializationSource())
        return false;
    if (index)
        index->erase(entry->key());
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
        overflow.reserve(size_t(count - inlineCapacity));
    if (count > 8) {
        if (index)
            index->reserve(size_t(count));
        else
            index.emplace(prepareIndex(count));
    }
}

qsizetype KisMutationWriteSet::size() const
{
    return qsizetype(overflow.size()) + (inlineEntry ? 1 : 0);
}

KisMutationWriteSet::Index KisMutationWriteSet::prepareIndex(qsizetype count) const
{
    Index result(0, KeyHash{}, std::equal_to<KisPageKey>{},
                 KisMutationStorageAllocator<std::pair<const KisPageKey, EntryHandle>>(budget));
    result.reserve(size_t(count));
    for (auto slot = firstEntry(); slot.isValid(); slot = nextEntry(slot))
        result.emplace(at(slot)->key(), slot);
    return result;
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
        token = std::exchange(other.token, 0);
    }
    return *this;
}

bool KisPageWriteAdmission::ClaimSet::isValid() const
{
    return owner && writeSet && token;
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

void *KisPageStoreWriteReservation::operator new(size_t bytes, KisBackingBudgetController *budget,
                                                void *owner, void (*retain)(void *), void (*release)(void *))
{
    if (bytes > size_t(std::numeric_limits<qint64>::max()) - sizeof(Allocation)) throw std::bad_alloc();
    const size_t total = bytes + sizeof(Allocation);
    auto *allocation = static_cast<Allocation *>(kisAllocateMutationStorage(budget, total, alignof(Allocation)));
    new (allocation) Allocation{budget, owner, release};
    retain(owner);
    return allocation + 1;
}

void KisPageStoreWriteReservation::operator delete(void *data) noexcept
{
    if (!data) return;
    auto *allocation = static_cast<Allocation *>(data) - 1;
    const auto value = *allocation;
    allocation->~Allocation();
    kisFreeMutationStorage(value.budget, allocation, 0, alignof(Allocation));
    value.release(value.owner);
}

KisPageWriteAdmission::KisPageWriteAdmission(QMutex &ownerMutex, KisPageWaitCondition &ownerCondition,
                                               KisBackingBudgetController *budget, const bool *operational)
    : m_ownerMutex(&ownerMutex)
    , m_ownerCondition(&ownerCondition)
    , m_claims(budget)
    , m_operational(operational)
{
}

qsizetype KisPageWriteAdmission::activeNativeClaimCountLocked() const
{
    return qsizetype(m_claims.size()) - activeGenericClaimCountLocked();
}

qsizetype KisPageWriteAdmission::activeGenericClaimCountLocked() const
{
    qsizetype count = 0;
    m_claims.forEach([&](const KisPageKey &, const ActiveClaim &claim) {
        count += claim.origin == ClaimOrigin::GenericWrite;
    });
    return count;
}

KisPageWriteAdmission::Conflict KisPageWriteAdmission::conflictLocked(
    const KisPageKey &key, Qt::HANDLE requester) const
{
    const auto *found = m_claims.find(key);
    if (!found) return Conflict::None;
    if (found->origin == ClaimOrigin::LegacyAdapter)
        return found->thread == requester ? Conflict::LegacySameThread : Conflict::LegacyBorrower;
    if (found->origin == ClaimOrigin::ManagedRange)
        return found->thread == requester ? Conflict::ManagedSameThread : Conflict::ManagedOtherThread;
    return found->thread == requester ? Conflict::WriterSameThread : Conflict::WriterOtherThread;
}

bool KisPageWriteAdmission::claimDirectLocked(const KisPageKey &key, quint64 ownerToken,
                                              QMutexLocker<QMutex> &lock, QString *error)
{
    const auto required = [&]() -> std::optional<size_t> {
        if (!ownerToken || (m_operational && !*m_operational)) {
            KisPageStoreDetail::setError(error, QStringLiteral("write admission is unavailable"));
            return {};
        }
        const auto *found = m_claims.find(key);
        if (!found) return 1;
        if (found->origin == ClaimOrigin::GenericWrite && found->token == ownerToken) return 0;
        KisPageStoreDetail::setError(error, QStringLiteral("page is claimed by another writer"));
        return {};
    };
    return admitMutationRecords(m_claims, lock, required, [&] {
        if (!m_claims.contains(key)) {
            const bool inserted = m_claims.insertPrepared(key, {ownerToken, ClaimOrigin::GenericWrite,
                                                                QThread::currentThreadId()});
            Q_ASSERT(inserted);
        }
    }, error);
}

void KisPageWriteAdmission::releaseDirectLocked(const KisPageKey &key, quint64 ownerToken) noexcept
{
    const auto *found = m_claims.find(key);
    Q_ASSERT(found && found->origin == ClaimOrigin::GenericWrite && found->token == ownerToken);
    if (!found || found->origin != ClaimOrigin::GenericWrite || found->token != ownerToken) return;
    m_claims.erase(key);
    m_ownerCondition->wakeAll();
}

KisPageWriteAdmission::ClaimSet KisPageWriteAdmission::beginClaimSet(const KisMutationWriteSet &writeSet)
{
    ClaimSet result;
    if (!m_nextClaimToken) return result;
    result.owner = this;
    result.writeSet = &writeSet;
    result.token = m_nextClaimToken;
    m_nextClaimToken = m_nextClaimToken == std::numeric_limits<quint64>::max() ? 0 : m_nextClaimToken + 1;
    return result;
}

bool KisPageWriteAdmission::claimOne(ClaimSet &claims, KisMutationWriteSet::EntryHandle entry,
                                     QMutexLocker<QMutex> &lock, QString *error, ClaimOrigin origin)
{
    const auto required = [&]() -> std::optional<size_t> {
        if (!claims.isValid() || claims.owner != this || (m_operational && !*m_operational)) {
            KisPageStoreDetail::setError(error, QStringLiteral("write admission claim set is invalid"));
            return {};
        }
        const auto *page = claims.writeSet->at(entry);
        if (!page) {
            KisPageStoreDetail::setError(error, QStringLiteral("write admission entry is invalid"));
            return {};
        }
        const auto *found = m_claims.find(page->key());
        if (!found) return 1;
        if (found->origin != ClaimOrigin::GenericWrite && found->token == claims.token) return 0;
        KisPageStoreDetail::setError(error, QStringLiteral("page is claimed by another writer"));
        return {};
    };
    return admitMutationRecords(m_claims, lock, required, [&] {
        const auto &key = claims.writeSet->at(entry)->key();
        if (!m_claims.contains(key)) {
            const bool inserted = m_claims.insertPrepared(key, {claims.token, origin, QThread::currentThreadId()});
            Q_ASSERT(inserted);
        }
    }, error);
}

bool KisPageWriteAdmission::releaseOneLocked(ClaimSet &claims, KisMutationWriteSet::EntryHandle slot) noexcept
{
    if (!claims.isValid() || claims.owner != this) return false;
    const auto *entry = claims.writeSet->at(slot);
    if (!entry) return false;
    const auto *found = m_claims.find(entry->key());
    if (!found || found->origin == ClaimOrigin::GenericWrite || found->token != claims.token) return false;
    m_claims.erase(entry->key());
    m_ownerCondition->wakeAll();
    return true;
}

KisPageWriteAdmission::Result KisPageWriteAdmission::claimAll(ClaimSet &claims, QMutexLocker<QMutex> &lock,
                                     QString *error, ClaimOrigin origin)
{
    bool contended = false;
    const auto required = [&]() -> std::optional<size_t> {
        if (!claims.isValid() || claims.owner != this || (m_operational && !*m_operational)) {
            KisPageStoreDetail::setError(error, QStringLiteral("write admission claim set is invalid"));
            return {};
        }
        size_t missing = 0;
        for (auto slot = claims.writeSet->firstEntry(); slot.isValid(); slot = claims.writeSet->nextEntry(slot)) {
            const auto *entry = claims.writeSet->at(slot);
            const auto *found = m_claims.find(entry->key());
            if (!found) ++missing;
            else if (found->origin == ClaimOrigin::GenericWrite || found->token != claims.token) {
                contended = true;
                KisPageStoreDetail::setError(error, QStringLiteral("write set intersects another writer"));
                return {};
            }
        }
        return missing;
    };
    const bool acquired = admitMutationRecords(m_claims, lock, required, [&] {
        for (auto slot = claims.writeSet->firstEntry(); slot.isValid(); slot = claims.writeSet->nextEntry(slot)) {
            const auto &key = claims.writeSet->at(slot)->key();
            if (!m_claims.contains(key)) {
                const bool inserted = m_claims.insertPrepared(key, {claims.token, origin, QThread::currentThreadId()});
                Q_ASSERT(inserted);
            }
        }
    }, error);
    return acquired ? Result::Acquired : contended ? Result::Contended : Result::Failed;
}

bool KisPageWriteAdmission::claimRange(ClaimSet &claims, KisSurfaceId surface,
                                      const KisPageSnapshotArray<KisLogicalPageId> &pages, QMutexLocker<QMutex> &lock, QString *error)
{
    const auto required = [&]() -> std::optional<size_t> {
        if (!claims.isValid() || claims.owner != this || !surface.isValid() || pages.isEmpty()
            || (m_operational && !*m_operational)) {
            KisPageStoreDetail::setError(error, QStringLiteral("execution claim range is invalid"));
            return {};
        }
        size_t missing = 0;
        for (const auto &page : pages) {
            const auto *entry = claims.writeSet->find({surface, page});
            if (!entry) {
                KisPageStoreDetail::setError(error, QStringLiteral("execution range lost its entry"));
                return {};
            }
            const auto *found = m_claims.find(entry->key());
            if (found && (found->origin == ClaimOrigin::GenericWrite || found->token != claims.token)) {
                KisPageStoreDetail::setError(error, QStringLiteral("execution range lost its entry or intersects another writer"));
                return {};
            }
            missing += !found;
        }
        return missing;
    };
    return admitMutationRecords(m_claims, lock, required, [&] {
        for (const auto &page : pages) {
            const auto &key = claims.writeSet->find({surface, page})->key();
            if (!m_claims.contains(key)) {
                const bool inserted = m_claims.insertPrepared(key,
                    {claims.token, ClaimOrigin::NativeSession, QThread::currentThreadId()});
                Q_ASSERT(inserted);
            }
        }
    }, error);
}

bool KisPageWriteAdmission::ownsClaimSetLocked(const ClaimSet &claims) const
{
    if (!claims.isValid() || claims.owner != this) return false;
    for (auto slot = claims.writeSet->firstEntry(); slot.isValid(); slot = claims.writeSet->nextEntry(slot)) {
        const auto *found = m_claims.find(claims.writeSet->at(slot)->key());
        if (!found || found->origin == ClaimOrigin::GenericWrite || found->token != claims.token) return false;
    }
    return true;
}

void KisPageWriteAdmission::rebindClaimSetLocked(ClaimSet &claims, const KisMutationWriteSet &writeSet) noexcept
{
    Q_ASSERT(claims.isValid() && claims.owner == this);
    Q_ASSERT(writeSet.size() > 0);
    for (auto slot = writeSet.firstEntry(); slot.isValid(); slot = writeSet.nextEntry(slot)) {
        const auto *found = m_claims.find(writeSet.at(slot)->key());
        Q_ASSERT(found && found->origin != ClaimOrigin::GenericWrite && found->token == claims.token);
    }
    claims.writeSet = &writeSet;
}

void KisPageWriteAdmission::release(ClaimSet &claims, bool lockOwner) noexcept
{
    std::optional<QMutexLocker<QMutex>> lock;
    if (lockOwner) lock.emplace(m_ownerMutex);
    if (claims.owner != this || !claims.writeSet) return;
    bool changed = false;
    for (auto slot = claims.writeSet->firstEntry(); slot.isValid(); slot = claims.writeSet->nextEntry(slot)) {
        const auto &key = claims.writeSet->at(slot)->key();
        const auto *found = m_claims.find(key);
        if (found && found->origin != ClaimOrigin::GenericWrite && found->token == claims.token) {
            m_claims.erase(key);
            changed = true;
        }
    }
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

bool KisPageWriteCoordinator::prepareWriteBaseLocked(
    const KisPageTransaction &transaction, const KisPageWriteIntent &intent,
    KisPagePublicationCoordinator &publication, KisPageTransition *write,
    KisPageAllocationDescriptor *descriptor, QString *error, const KisPageTransition *pending,
    KisReplicaHandle *recoverableBefore)
{
    const auto fail = [&](const QString &reason) { KisPageStoreDetail::setError(error, reason); return false; };
    const auto overlay = KisPageReadView::transactionOverlay(transaction.id);
    KisPageVersion baseVersion;
    if (!epoch->resolve(intent.key, overlay, &baseVersion))
        return fail(QStringLiteral("write base does not resolve"));
    const auto sealed = pending ? pending->baseVersion
        : publication.findPreparedProofLocked(transaction.id, intent.key).authority.version;
    const bool removed = (intent.flags & quint8(KisPageWriteIntentFlag::SemanticRemoval))
        || publication.stagesRemovalLocked(transaction.id, intent.key);
    const bool discoverBefore = recoverableBefore && !pending && !removed;
    KisPageMetadataCoordinator::MutationBaseInfo baseInfo;
    if (!metadata->queryMutationBase(baseVersion, sealed, discoverBefore, &baseInfo)
        || !baseInfo.baseExists) {
        KisSurfaceEpochState before;
        if (!epoch->surfaceState(intent.key.surface, overlay, &before)
            || !publication.ensureVirtualDefaultLocked(baseVersion, before, error)
            || !metadata->queryMutationBase(baseVersion, sealed, discoverBefore, &baseInfo))
            return false;
    }
    if (baseInfo.hasWriter)
        return fail(QStringLiteral("write page already has a writer"));
    // Only a previously sealed overlay (or the exact base of a private target
    // being replaced) supersedes the transaction root. Never scan all history
    // for another Prepared version and accidentally expose unsealed work.
    if (sealed.isValid()) baseVersion = sealed;
    const auto &base = baseInfo.selected;
    if (!base.version.isValid() || (!base.isVirtualDefault() && !base.authority.isValid()))
        return fail(QStringLiteral("write base has no exact authority"));
    if (removed || baseVersion.isDefaultPixel() || intent.inputKind == KisPageWriteInputKind::Semantic) {
        KisSurfaceEpochState surface;
        if (!publication.resolveSurfaceLocked(intent.key.surface, overlay, &surface))
            return fail(QStringLiteral("write surface does not resolve"));
        *descriptor = surface.allocationDescriptor();
    } else if (!publication.descriptorLocked(baseVersion, descriptor))
        return fail(QStringLiteral("write base descriptor is unavailable"));
    if (recoverableBefore) *recoverableBefore = baseInfo.recoverableBefore;
    *write = {};
    write->kind = KisPageTransitionKind::AcquireWrite;
    write->baseVersion = baseVersion;
    write->version = pending ? pending->version : KisPageVersion{intent.key, baseInfo.nextGeneration};
    write->source = pending ? pending->target : base.authority;
    write->transaction = transaction.id;
    write->operation = ownerLedger->nextOperationId();
    write->writer = ownerLedger->nextWriterToken();
    write->writeMode = removed || baseVersion.isDefaultPixel() ? KisPageWriteMode::DiscardContents : intent.mode;
    if (!descriptor->isValid() || !write->version.isValid() || !write->operation.isValid() || !write->writer.isValid())
        return fail(QStringLiteral("write identity or layout is unavailable"));
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPageMetadataTransitionResult KisPageWriteCoordinator::preparePrivateWrite(KisPageTransition &write, bool initialized)
{
    // A reader can materialize a virtual before-image during the unlocked
    // provider allocation. Acquire its actual authority pin, not a stale null
    // source; the target still initializes directly from the exact default.
    if (write.baseVersion.isDefaultPixel() && !write.source.isValid()) {
        KisPageMetadataCoordinator::VersionInfo current;
        if (!metadata->versionSnapshot(write.baseVersion, &current) || !current.version.isValid()) {
            KisPageMetadataTransitionResult rejected;
            rejected.rejectionReason = QStringLiteral("write default base disappeared");
            return rejected;
        }
        write.source = current.authority;
    }
    if (!initialized)
        return metadata->applyOwner(write.version.key, write);
    auto prepare = write;
    prepare.kind = KisPageTransitionKind::PrepareWrite;
    return metadata->applyOwnerSequence(write.version.key, {write, prepare});
}

KisPageMetadataTransitionResult KisPageWriteCoordinator::publishPrivateWrite(KisPageTransition write)
{
    // The original private generation is the acceptance record. A later
    // preparation refusal must not replay its already consumed writer.
    KisPageMetadataCoordinator::VersionInfo accepted;
    if (metadata->versionSnapshot(write.version, &accepted)
        && accepted.version == write.version
        && accepted.publication == KisPagePublicationState::Prepared
        && accepted.preparedBy == write.transaction && accepted.authority == write.target) {
        KisPageMetadataTransitionResult result;
        result.accepted = true;
        return result;
    }
    write.kind = KisPageTransitionKind::BeginPublish;
    auto publish = write;
    publish.kind = KisPageTransitionKind::PublishWrite;
    return metadata->applyOwnerSequence(write.version.key, {write, publish});
}

KisPageWritePlanKind KisPageWriteCoordinator::prepareWritePlanLocked(
    const KisPageTransaction &transaction, const KisPageWriteIntent &intent,
    const std::shared_ptr<KisPageReplicaProvider> &provider, KisPageAccessRequirement access,
    const KisPageAllocationDescriptor &descriptor, KisPagePublicationCoordinator &publication,
    KisPageTransition &write, const KisReplicaHandle &before,
    QMutexLocker<QMutex> &locker, KisCpuWriteBindingReservation &writable,
    KisPageStoreDiagnosticTimer &diagnostic)
{
    const auto fallback = select(intent);
    if (!before.isValid() || intent.inputKind != KisPageWriteInputKind::MutableGuard || !provider
        || (intent.flags & quint8(KisPageWriteIntentFlag::SourceInitialization))
        || access.domain != KisPageAccessDomain::CpuRam || access.kind != KisPageAccessKind::CpuPointer
        || write.source.domain != KisPageAccessDomain::CpuRam
        || write.baseVersion.isDefaultPixel() || !write.source.isValid()
        || !(provider->providerId() == write.source.provider)
        || !(provider->providerEpoch() == write.source.providerEpoch)
        || writable.isValid()) return fallback;

    const auto transientLimit = budget->maxTransientVersionsPerPage();
    if (transientLimit != std::numeric_limits<quint32>::max()
        && !metadata->canAddTransientVersion(write.version, transientLimit)) return fallback;

    // Cold short-lived storage only. The existing owners retain every live
    // fact after install; neither adapter receives a second terminal ledger.
    struct Prepared {
        std::shared_ptr<KisPageReplicaProvider> beforeProvider;
        std::shared_ptr<KisCpuResidentBinding> beforeBinding;
        bool beforePinned = false;
        KisCpuBackingHandoff physical;
        KisBackingHandoffReservation accounting;
        KisPageMetadataCoordinator::PreparedPublication metadata;
        KisPageMetadataCoordinator::DeferredPublicationCleanup cleanup;
        KisPagePublicationCoordinator::DescriptorMap descriptors;
        ~Prepared()
        {
            physical.reset();
            if (beforePinned) beforeBinding->releaseRead();
        }
    };
    // Invocation-local storage needs no separate allocation. Reset below
    // still destroys all owning members while the owner gate is open.
    std::optional<Prepared> prepared(std::in_place);
    prepared->beforeProvider = ownerLedger->provider(before.provider, before.providerEpoch);
    // Even rejected metadata candidates can own real arena/index storage.
    // Destroy them, the before pin and provider references outside owner gates.
    const auto cleanup = qScopeGuard([&] {
        // Preparation may throw while the gate is already open. Qt's locker
        // requires a held lock for unlock(); always restore it for the caller.
        if (locker.isLocked()) locker.unlock();
        diagnostic.next(KisPageStoreDiagnosticPhase::RecoverableCleanup, 1);
        prepared.reset();
        locker.relock();
    });
    if (!prepared->beforeProvider) return fallback;

    locker.unlock();
    diagnostic.next(KisPageStoreDiagnosticPhase::RecoverablePrepare, 1);
    prepared->beforeBinding = prepared->beforeProvider->cpuResidentBinding(before);
    prepared->beforePinned = prepared->beforeBinding
        && prepared->beforeBinding->acquireRead(before.allocationIdentity(), true);
    if (prepared->beforePinned)
        prepared->physical = KisCpuBackingHandoff::prepare(provider, write.source, write.version, descriptor);
    auto recoverable = write;
    recoverable.kind = KisPageTransitionKind::AcquireRecoverableWrite;
    recoverable.source = before;
    recoverable.target = prepared->physical.target();
    if (prepared->physical.isValid()) {
        prepared->accounting = ownerLedger->prepareBackingHandoff(write.source, recoverable.target);
        if (prepared->accounting.isValid())
            prepared->metadata = metadata->prepareRecoverableWrite(transaction, recoverable);
    }
    diagnostic.next(KisPageStoreDiagnosticPhase::RecoverablePrepared, 1);
    locker.relock();
    if (!prepared->metadata.isValid()) return fallback;

    // All fallible storage exists before taking the physical claim. Metadata
    // install strictly rechecks the page revision and exact recovery identity.
    prepared->descriptors = publication.prepareDescriptorLocked(write.version, descriptor);
    const auto plan = select(intent, nullptr, true);
    diagnostic.next(KisPageStoreDiagnosticPhase::RecoverableInstall, 1);
    if (plan != KisPageWritePlanKind::RecoverableHandoff || !prepared->physical.tryClaim()) return fallback;
    if (!metadata->installRecoverableWrite(std::move(prepared->metadata), transaction,
                                           nullptr, &prepared->cleanup)) return fallback;
    writable = prepared->physical.commit();
    const bool accounted = ownerLedger->commitBackingHandoff(std::move(prepared->accounting));
    Q_ASSERT(writable.isValid() && accounted);
    Q_UNUSED(accounted);
    publication.installDescriptorAdditionsLocked(&prepared->descriptors);
    diagnostic.next(KisPageStoreDiagnosticPhase::RecoverableInstalled, 1);
    // A discard guard without final input preserves the existing adapter
    // contract: untouched pixels start at the surface default. Complete input
    // bypasses this fill and is written directly by the native payload caller.
    // Retag already owns a pinned writer, so this synchronous kernel cannot
    // require storage, I/O, callbacks or another permission check.
    if (write.writeMode == KisPageWriteMode::DiscardContents
        && !(intent.flags & quint8(KisPageWriteIntentFlag::InputBytesReady))) {
        locker.unlock();
        auto *bytes = static_cast<quint8 *>(writable.pinResident());
        Q_ASSERT(bytes);
        const auto &layout = recoverable.target.layout;
        const auto &pixel = descriptor.format.defaultPixel;
        for (int y = 0; y < layout.pageExtent.height(); ++y)
            for (int x = 0; x < layout.pageExtent.width(); ++x)
                std::memcpy(bytes + quint64(y) * layout.rowStride + quint64(x) * pixel.size(),
                            pixel.constData(), size_t(pixel.size()));
        locker.relock();
    }
    write = recoverable;
    return plan;
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
    auto &bucket = delta.buckets[static_cast<size_t>(budgetClass)];
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
        && budgetClass != KisBackingBudgetClass::MetadataArena) {
        auto &debt = delta.buckets[static_cast<size_t>(KisBackingBudgetClass::RetirementDebt)];
        *kisPageDomainComponent(debt, domain) = qint64(bytes);
    }
    KisPageBackingPreparation prepared;
    prepared.reservation = budget->reserve(delta, error);
    if (!prepared.reservation.isValid()) return {};
    try {
        if (preparedRetirement) {
            if (!*preparedRetirement || (*preparedRetirement)->storage.budget != budget) {
                KisPageStoreDetail::setError(error, QStringLiteral("prepared backing retirement storage has changed"));
                return {};
            }
            (*preparedRetirement)->prepareBackingStorage();
            prepared.retirement = std::move(*preparedRetirement);
        } else prepared.retirement = kisPreparePageRetirementRecord(budget);
    } catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing retirement record budget storage was refused"));
        return {};
    }
    return prepared;
}

KisPageMetadataTransitionResult KisPageWriteCoordinator::cancelPrivateWrite(KisPageTransition write)
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
    const KisMutationPageEntry *pending, bool recoverablePrepared) const
{
    if (intent.inputKind == KisPageWriteInputKind::MutableGuard && recoverablePrepared)
        return KisPageWritePlanKind::RecoverableHandoff;
    if (intent.inputKind == KisPageWriteInputKind::MutableGuard
        && !(intent.flags & quint8(KisPageWriteIntentFlag::InputBytesReady))
        && intent.mode == KisPageWriteMode::PreserveContents) {
        return KisPageWritePlanKind::FreshCow;
    }
    if (intent.inputKind == KisPageWriteInputKind::MutableGuard) {
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

KisReplicaOperation KisPageWriteCoordinator::prepareFreshReplica(
    const KisPageWriteIntent &intent, KisPageReplicaProvider &provider,
    const KisPageTransition &write, const KisPageAllocationDescriptor &descriptor,
    KisPageAccessRequirement access, KisPagePriority priority,
    const KisCpuPagePayload *payload,
    const std::shared_ptr<const KisPageReplicaSource> &initialization,
    bool *synchronousCopy) const
{
    if (synchronousCopy) *synchronousCopy = false;
    if (intent.inputKind != KisPageWriteInputKind::MutableGuard)
        return {};
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

void KisPageWriteCoordinator::recordPrepared(KisMutationPageEntry &entry,
                                             const KisPageWriteIntent &intent,
                                             const KisPageTransition &write)
{
    Q_ASSERT(entry.key() == intent.key && entry.state == KisMutationPageEntryState::IntentOnly);
    Q_ASSERT(write.version.key == entry.key() && !write.version.isDefaultPixel());
    const quint8 semanticRemoval = entry.intent.flags & quint8(KisPageWriteIntentFlag::SemanticRemoval);
    entry.intent = intent;
    entry.intent.flags |= semanticRemoval;
    entry.baseVersion = write.baseVersion;
    entry.targetGeneration = write.version.generation;
    entry.operation = write.operation;
    entry.writer = write.writer;
    entry.state = KisMutationPageEntryState::Prepared;
}

void KisPageWriteCoordinator::recordCancelled(KisMutationPageEntry &entry)
{
    Q_ASSERT(entry.state == KisMutationPageEntryState::Prepared);
    entry.baseVersion = {};
    entry.targetGeneration = {};
    entry.operation = {};
    entry.writer = {};
    entry.state = KisMutationPageEntryState::IntentOnly;
}

void KisPageWriteCoordinator::recordExposure(KisMutationPageEntry &entry, bool exposed)
{
    Q_ASSERT(entry.state == (exposed ? KisMutationPageEntryState::Prepared : KisMutationPageEntryState::Exposed));
    entry.state = exposed ? KisMutationPageEntryState::Exposed : KisMutationPageEntryState::Prepared;
}

KisPageTransition KisPageWriteCoordinator::writeTransition(const KisMutationPageEntry &entry,
                                                           KisPageTransactionId transaction,
                                                           const KisReplicaHandle &source,
                                                           const KisReplicaHandle &target) const
{
    KisPageTransition write;
    write.baseVersion = entry.baseVersion;
    write.version = entry.preparedTargetVersion();
    write.operation = entry.operation;
    write.writer = entry.writer;
    write.writeMode = entry.intent.mode;
    write.transaction = transaction;
    write.source = source;
    write.target = target;
    return write;
}

bool KisPageWriteCoordinator::beginActivity(KisPageTransactionId transaction,
                                             qsizetype TransactionActivity::*member,
                                             QMutexLocker<QMutex> &lock, QString *error)
{
    const auto required = [&]() -> std::optional<size_t> {
        const auto *found = transactionActivities.find(transaction.value);
        if (!transaction.isValid() || (found && found->*member == std::numeric_limits<qsizetype>::max())) {
            KisPageStoreDetail::setError(error, QStringLiteral("mutation activity is unavailable or exhausted"));
            return {};
        }
        return found ? 0 : 1;
    };
    return admitMutationRecords(transactionActivities, lock, required, [&] {
        auto *found = transactionActivities.find(transaction.value);
        if (!found) {
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

void KisPageWriteCoordinator::endActivity(KisPageTransactionId transaction, qsizetype TransactionActivity::*member)
{
    auto *found = transactionActivities.find(transaction.value);
    Q_ASSERT(found && found->*member > 0);
    if (!found || found->*member <= 0) return;
    --(found->*member);
    if (found->isEmpty()) transactionActivities.erase(transaction.value);
    else transactionActivities.changed();
}

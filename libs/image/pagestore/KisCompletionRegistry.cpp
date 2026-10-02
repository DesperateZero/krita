/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisCompletionRegistry.h"
#include "KisPageWriteCoordinator_p.h"

#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>

#include <boost/intrusive/set.hpp>
#include <atomic>
#include <limits>
#include <new>
#include <map>
#include <utility>

namespace {

std::atomic<quint64> s_nextRegistryId{1};

}

struct CompletionReadinessRecord : boost::intrusive::set_base_hook<> {
    quint64 value;
    std::shared_ptr<KisPageReadinessSignal> signal;
    KisMutationStorageAllocator<CompletionReadinessRecord> storage;
    CompletionReadinessRecord(quint64 id, std::shared_ptr<KisPageReadinessSignal> state,
        KisMutationStorageAllocator<CompletionReadinessRecord> allocator)
        : value(id), signal(std::move(state)), storage(std::move(allocator)) {}
    static void dispose(CompletionReadinessRecord *record) noexcept
    {
        auto allocator = std::move(record->storage);
        std::destroy_at(record);
        allocator.deallocate(record, 1);
    }
};
struct ReadinessLess {
    bool operator()(const CompletionReadinessRecord &a, const CompletionReadinessRecord &b) const { return a.value < b.value; }
    bool operator()(quint64 id, const CompletionReadinessRecord &b) const { return id < b.value; }
    bool operator()(const CompletionReadinessRecord &a, quint64 id) const { return a.value < id; }
};
struct CompletionReadinessMap : boost::intrusive::set<CompletionReadinessRecord, boost::intrusive::compare<ReadinessLess>> {
    ~CompletionReadinessMap() { clear_and_dispose([](CompletionReadinessRecord *record) { CompletionReadinessRecord::dispose(record); }); }
    auto find(quint64 value) { return boost::intrusive::set<CompletionReadinessRecord, boost::intrusive::compare<ReadinessLess>>::find(value, ReadinessLess{}); }
};
using ReadinessRecordPointer = std::unique_ptr<CompletionReadinessRecord, decltype(&CompletionReadinessRecord::dispose)>;

struct CompletionInterval : boost::intrusive::set_base_hook<>
{
    quint64 first;
    quint64 last;
    KisCompletionStatus status = KisCompletionStatus::Pending;
    CompletionInterval *releasedNext = nullptr;
    KisMutationStorageAllocator<CompletionInterval> storage;
    CompletionInterval(quint64 value, KisMutationStorageAllocator<CompletionInterval> allocator) noexcept
        : first(value), last(value), storage(std::move(allocator)) {}
    static void dispose(CompletionInterval *record) noexcept
    {
        auto allocator = std::move(record->storage);
        std::destroy_at(record);
        allocator.deallocate(record, 1);
    }
};
struct IntervalLess {
    bool operator()(const CompletionInterval &a, const CompletionInterval &b) const { return a.first < b.first; }
    bool operator()(quint64 value, const CompletionInterval &b) const { return value < b.first; }
    bool operator()(const CompletionInterval &a, quint64 value) const { return a.first < value; }
};
struct CompletionIntervals : boost::intrusive::set<CompletionInterval, boost::intrusive::compare<IntervalLess>> {
    ~CompletionIntervals() { clear_and_dispose([](CompletionInterval *record) { CompletionInterval::dispose(record); }); }
    void release(iterator entry, CompletionInterval *&released) noexcept
    {
        auto *record = &*entry;
        erase(entry);
        record->releasedNext = std::exchange(released, record);
    }
};
void disposeIntervals(CompletionInterval *&released) noexcept
{
    while (released) {
        auto *record = released;
        released = record->releasedNext;
        CompletionInterval::dispose(record);
    }
}

struct CompletionSourceState
{
    using ReadinessMap = CompletionReadinessMap;
    explicit CompletionSourceState(KisMutationStorageAllocator<CompletionInterval> allocator)
        : storage(std::move(allocator)) {}
    KisMutationStorageAllocator<CompletionInterval> storage;
    KisCompletionDomain domain = KisCompletionDomain::Unknown;
    quint64 nextValue = 1;
    // Successful prefix values need no record. Other equal-status terminal
    // neighbours coalesce; every Pending value owns its prepared terminal node.
    quint64 terminalPrefix = 0;
    CompletionIntervals intervals;
    ReadinessMap readiness;

    KisCompletionStatus status(quint64 value) const
    {
        if (value == 0 || value >= nextValue) return KisCompletionStatus::Unknown;
        auto next = intervals.upper_bound(value, IntervalLess{});
        if (next != intervals.cbegin()) {
            --next;
            if (value <= next->last) return next->status;
        }
        return value <= terminalPrefix ? KisCompletionStatus::Succeeded : KisCompletionStatus::Pending;
    }

    bool complete(quint64 value, KisCompletionStatus terminal, CompletionInterval *&released) noexcept
    {
        auto entry = intervals.find(value, IntervalLess{});
        if (entry == intervals.end() || entry->status != KisCompletionStatus::Pending) return false;
        entry->status = terminal;
        if (entry != intervals.begin()) {
            auto previous = entry;
            --previous;
            if (previous->status == terminal && previous->last + 1 == entry->first) {
                previous->last = entry->last;
                intervals.release(entry, released);
                entry = previous;
            }
        }
        auto next = entry;
        ++next;
        if (next != intervals.end() && next->status == terminal && entry->last + 1 == next->first) {
            entry->last = next->last;
            intervals.release(next, released);
        }
        if (entry->first <= terminalPrefix + 1 && entry->last >= terminalPrefix + 1) {
            terminalPrefix = entry->last;
            if (entry->status == KisCompletionStatus::Succeeded) intervals.release(entry, released);
        }
        next = intervals.lower_bound(terminalPrefix + 1, IntervalLess{});
        while (next != intervals.end() && next->first == terminalPrefix + 1 &&
               next->status != KisCompletionStatus::Pending) {
            terminalPrefix = next->last;
            auto completed = next++;
            if (completed->status == KisCompletionStatus::Succeeded) intervals.release(completed, released);
        }
        return true;
    }
};

class KisCompletionRegistry::Private
{
public:
    explicit Private(QSharedPointer<KisBackingBudgetController> owner)
        : budget(std::move(owner)), storage(KisMutationStorageAllocator<CompletionInterval>::retained(budget.data()))
        , sources(std::less<quint64>{}, KisMutationStorageAllocator<std::pair<const quint64, CompletionSourceState>>(storage)) {}
    void releaseEmptyReadiness(quint64 source, quint64 value, const KisPageReadinessState *expected)
    {
        ReadinessRecordPointer released(nullptr, CompletionReadinessRecord::dispose);
        {
            QMutexLocker lock(&mutex);
            const auto found = sources.find(source);
            if (found == sources.end()) return;
            const auto entry = found->second.readiness.find(value);
            // Recheck both the original signal identity and emptiness: a new
            // subscriber may have won the owner gate after cancellation.
            if (entry == found->second.readiness.end() || !entry->signal->emptyState(expected)) return;
            released.reset(&*entry);
            found->second.readiness.erase(entry);
        } // Actual record/state/capture frees remain outside the registry gate.
    }
    QSharedPointer<KisBackingBudgetController> budget;
    KisMutationStorageAllocator<CompletionInterval> storage;
    mutable QMutex mutex;
    quint64 registryId = KisPageStoreDetail::allocateMonotonicId<quint64>(
        &s_nextRegistryId);
    quint64 nextSource = 1;
    std::map<quint64, CompletionSourceState, std::less<quint64>,
        KisMutationStorageAllocator<std::pair<const quint64, CompletionSourceState>>> sources;
};

KisCompletionRegistry::KisCompletionRegistry(const QSharedPointer<KisBackingBudgetController> &processBudget)
{
    auto budget = QSharedPointer<KisBackingBudgetController>::create();
    if (processBudget && !budget->configureSharedNonPayloadBudget(processBudget)) throw std::bad_alloc();
    // The source owner is independent of any one Store. Its actual control
    // block remains charged through late weak callbacks after body teardown.
    d = std::allocate_shared<Private>(KisMutationStorageAllocator<Private>::retained(budget.data()), budget);
}

KisCompletionRegistry::~KisCompletionRegistry() = default;

bool KisCompletionRegistry::isOperational() const
{
    return d->registryId != 0;
}

quint64 KisCompletionRegistry::registerSource(KisCompletionDomain domain)
{
    if (domain == KisCompletionDomain::Unknown) {
        return 0;
    }

    QMutexLocker locker(&d->mutex);
    if (d->registryId == 0 || d->nextSource == 0 ||
        d->nextSource == std::numeric_limits<quint64>::max()) {
        return 0;
    }
    const quint64 source = d->nextSource;
    try {
        const auto entry = d->sources.try_emplace(source, d->storage);
        entry.first->second.domain = domain;
    } catch (const std::bad_alloc &) {
        return 0;
    }
    ++d->nextSource;
    return source;
}

KisCompletionTicket KisCompletionRegistry::allocatePending(quint64 source)
{
    QMutexLocker locker(&d->mutex);
    auto sourceIt = d->sources.find(source);
    if (sourceIt == d->sources.end()) {
        return {};
    }

    if (sourceIt->second.nextValue == 0 ||
        sourceIt->second.nextValue == std::numeric_limits<quint64>::max()) {
        return {};
    }
    const quint64 value = sourceIt->second.nextValue;
    auto allocator = sourceIt->second.storage;
    try {
        auto *node = allocator.allocate(1);
        auto *record = ::new (node) CompletionInterval(value, allocator);
        sourceIt->second.intervals.insert(*record);
    } catch (const std::bad_alloc &) {
        return {};
    }
    ++sourceIt->second.nextValue;
    return KisCompletionTicket(sourceIt->second.domain, d->registryId, source, value);
}

bool KisCompletionRegistry::complete(const KisCompletionTicket &ticket,
                                     KisCompletionStatus status)
{
    if (!ticket.isValid() || ticket.registry() != d->registryId ||
        (status != KisCompletionStatus::Succeeded &&
         status != KisCompletionStatus::Failed &&
         status != KisCompletionStatus::Cancelled)) {
        return false;
    }

    CompletionInterval *releasedIntervals = nullptr;
    const auto freeIntervals = qScopeGuard([&] { disposeIntervals(releasedIntervals); });
    ReadinessRecordPointer released(nullptr, CompletionReadinessRecord::dispose);
    QMutexLocker locker(&d->mutex);
    auto sourceIt = d->sources.find(ticket.source());
    if (sourceIt == d->sources.end() ||
        ticket.domain() != sourceIt->second.domain) {
        return false;
    }

    if (!sourceIt->second.complete(ticket.value(), status, releasedIntervals)) return false;
    std::shared_ptr<KisPageReadinessSignal> readiness;
    const auto waiting = sourceIt->second.readiness.find(ticket.value());
    if (waiting != sourceIt->second.readiness.end()) {
        readiness = waiting->signal;
        released.reset(&*waiting);
        sourceIt->second.readiness.erase(waiting);
    }
    locker.unlock();
    disposeIntervals(releasedIntervals);
    released.reset();
    if (readiness) readiness->notify();
    return true;
}

void KisCompletionRegistry::completePrepared(const KisCompletionTicket &ticket,
                                           KisCompletionStatus status) noexcept
{
    if (!complete(ticket, status))
        qFatal("Prepared completion ticket is invalid or already terminal");
}

KisPageReadinessStatus KisCompletionRegistry::watchTerminal(const KisCompletionTicket &ticket,
    KisPageReadinessCallback ready, KisPageReadinessSubscription *subscription)
{
    if (!subscription || !ready || !ticket.isValid() || ticket.registry() != d->registryId)
        return KisPageReadinessStatus::Unavailable;
    subscription->reset();
    std::shared_ptr<KisPageReadinessSignal> signal;
    KisPageReadinessSubscription prepared;
    ReadinessRecordPointer record(nullptr, CompletionReadinessRecord::dispose);
    QMutexLocker lock(&d->mutex);
    auto source = d->sources.find(ticket.source());
    if (source == d->sources.end() || ticket.domain() != source->second.domain)
        return KisPageReadinessStatus::Unavailable;
    auto status = source->second.status(ticket.value());
    if (status == KisCompletionStatus::Unknown) return KisPageReadinessStatus::Unavailable;
    if (status != KisCompletionStatus::Pending) return KisPageReadinessStatus::Ready;
    const auto existing = source->second.readiness.find(ticket.value());
    if (existing != source->second.readiness.end()) signal = existing->signal;
    lock.unlock();
    try {
        const auto storage = ready.storageAllocator<std::byte>();
        prepared = KisPageReadinessSubscription(std::move(ready));
        if (!signal) {
            auto emptied = KisPageReadinessCallback::prepare(
                [owner = std::weak_ptr<Private>(d), sourceId = ticket.source(), value = ticket.value()]
                (const KisPageReadinessState *expected) {
                    if (const auto state = owner.lock())
                        state->releaseEmptyReadiness(sourceId, value, expected);
                }, storage);
            signal = std::allocate_shared<KisPageReadinessSignal>(
                KisMutationStorageAllocator<KisPageReadinessSignal>(storage), std::move(emptied), storage);
        }
        // The observed record can be removed by a last cancellation while the
        // owner gate is dropped. Prepare a replacement before revalidation.
        auto allocator = KisMutationStorageAllocator<CompletionReadinessRecord>(storage);
        auto *node = allocator.allocate(1);
        try { record.reset(::new (node) CompletionReadinessRecord(ticket.value(), signal, allocator)); }
        catch (...) { allocator.deallocate(node, 1); throw; }
    } catch (const std::bad_alloc &) {
        return KisPageReadinessStatus::Unavailable;
    }
    lock.relock();
    // Cold preparation cannot publish a waiter for a ticket that completed
    // in the meantime; attach only to the winning original registry record.
    source = d->sources.find(ticket.source());
    if (source == d->sources.end() || ticket.domain() != source->second.domain)
        return KisPageReadinessStatus::Unavailable;
    status = source->second.status(ticket.value());
    if (status == KisCompletionStatus::Unknown) return KisPageReadinessStatus::Unavailable;
    if (status != KisCompletionStatus::Pending) return KisPageReadinessStatus::Ready;
    const auto winner = source->second.readiness.find(ticket.value());
    if (winner != source->second.readiness.end()) {
        // Use a separate local so replacement destruction is outside the gate.
        if (!winner->signal->subscribeRetained(prepared)) return KisPageReadinessStatus::Unavailable;
    } else {
        if (!signal->subscribeRetained(prepared)) return KisPageReadinessStatus::Unavailable;
        source->second.readiness.insert(*record);
        record.release();
    }
    *subscription = std::move(prepared);
    return KisPageReadinessStatus::Waiting;
}

KisCompletionStatus KisCompletionRegistry::status(const KisCompletionTicket &ticket) const
{
    if (!ticket.isValid() || ticket.registry() != d->registryId) {
        return KisCompletionStatus::Unknown;
    }

    QMutexLocker locker(&d->mutex);
    const auto sourceIt = d->sources.find(ticket.source());
    if (sourceIt == d->sources.end() ||
        ticket.domain() != sourceIt->second.domain) {
        return KisCompletionStatus::Unknown;
    }
    return sourceIt->second.status(ticket.value());
}

KisVerifiedCompletion KisCompletionRegistry::verifyTerminal(
    const KisCompletionTicket &ticket) const
{
    const KisCompletionStatus terminalStatus = status(ticket);
    if (terminalStatus == KisCompletionStatus::Unknown ||
        terminalStatus == KisCompletionStatus::Pending) {
        return {};
    }
    return KisVerifiedCompletion(ticket, terminalStatus);
}

KisCompletionSourceStatistics KisCompletionRegistry::sourceStatistics(quint64 source) const
{
    QMutexLocker locker(&d->mutex);
    const auto it = d->sources.find(source);
    if (it == d->sources.end()) return {};
    KisCompletionSourceStatistics result;
    result.knownSource = true;
    result.allocatedTickets = it->second.nextValue - 1;
    result.storageRecords = quint64(it->second.intervals.size()) +
                            quint64(it->second.terminalPrefix != 0) + quint64(it->second.readiness.size());
    result.readinessSignals = quint64(it->second.readiness.size());
    result.readinessCapacity = quint64(it->second.readiness.size());
    for (const auto &signal : it->second.readiness) result.readinessWaiters += signal.signal->subscriberCount();
    result.terminalTickets = it->second.terminalPrefix;
    for (const auto &range : it->second.intervals) {
        if (range.status != KisCompletionStatus::Pending && range.last > it->second.terminalPrefix)
            result.terminalTickets += range.last - qMax(range.first, it->second.terminalPrefix + 1) + 1;
    }
    result.pendingTickets = result.allocatedTickets - result.terminalTickets;
    return result;
}

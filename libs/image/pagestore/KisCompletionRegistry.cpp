/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisCompletionRegistry.h"

#include <QMutex>
#include <QMutexLocker>

#include <boost/intrusive/set.hpp>
#include <atomic>
#include <limits>
#include <new>
#include <map>
#include <unordered_map>
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

struct CompletionSourceState
{
    using ReadinessMap = CompletionReadinessMap;
    KisCompletionDomain domain = KisCompletionDomain::Unknown;
    quint64 nextValue = 1;
    // Coverage is independent of terminal status, so alternating results do
    // not fragment the completion index. Every covered value was explicitly
    // completed; a larger completed host ticket never closes a pending gap.
    quint64 terminalPrefix = 0;
    std::map<quint64, quint64> terminalRanges;
    std::unordered_map<quint64, KisCompletionStatus> nonSuccess;
    ReadinessMap readiness;

    KisCompletionStatus terminalStatus(quint64 value) const
    {
        const auto found = nonSuccess.find(value);
        return found == nonSuccess.end() ? KisCompletionStatus::Succeeded : found->second;
    }

    KisCompletionStatus status(quint64 value) const
    {
        if (value == 0 || value >= nextValue) return KisCompletionStatus::Unknown;
        if (value <= terminalPrefix) return terminalStatus(value);
        if (!terminalRanges.empty()) {
            auto last = terminalRanges.cend();
            --last;
            if (value >= last->first) {
                return value <= last->second ? terminalStatus(value)
                                             : KisCompletionStatus::Pending;
            }
        }
        auto next = terminalRanges.upper_bound(value);
        if (next != terminalRanges.cbegin()) {
            --next;
            if (value <= next->second) return terminalStatus(value);
        }
        return KisCompletionStatus::Pending;
    }

    bool markTerminal(quint64 value)
    {
        if (value == 0 || value >= nextValue || value <= terminalPrefix) return false;
        if (value == terminalPrefix + 1) {
            terminalPrefix = value;
            const auto next = terminalRanges.begin();
            if (next != terminalRanges.end() && next->first == value + 1) {
                terminalPrefix = next->second;
                terminalRanges.erase(next);
            }
            return true;
        }
        if (!terminalRanges.empty()) {
            auto last = terminalRanges.end();
            --last;
            if (value > last->second) {
                if (value == last->second + 1) {
                    last->second = value;
                } else {
                    terminalRanges.emplace_hint(terminalRanges.cend(), value, value);
                }
                return true;
            }
            if (value >= last->first) return false;
        }
        auto next = terminalRanges.upper_bound(value);
        if (next != terminalRanges.begin()) {
            auto previous = next;
            --previous;
            if (value <= previous->second) return false;
            if (previous->second + 1 == value) {
                previous->second = value;
                if (next != terminalRanges.end() && next->first == value + 1) {
                    previous->second = next->second;
                    terminalRanges.erase(next);
                }
                return true;
            }
        }
        if (next != terminalRanges.end() && next->first == value + 1) {
            terminalRanges.emplace(value, next->second);
            terminalRanges.erase(next);
        } else {
            terminalRanges.emplace(value, value);
        }
        return true;
    }

    bool complete(quint64 value, KisCompletionStatus terminal)
    {
        if (status(value) != KisCompletionStatus::Pending) return false;
        bool preparedStatus = false;
        try {
            // Stage the exceptional status before publishing terminal coverage.
            // Both container insertions have the strong exception guarantee;
            // erase/merge/prefix advancement then require no storage. These
            // source-owned maps are never implicitly shared with a snapshot.
            if (terminal != KisCompletionStatus::Succeeded) {
                preparedStatus = nonSuccess.emplace(value, terminal).second;
                Q_ASSERT(preparedStatus);
            }
            if (markTerminal(value)) return true;
        } catch (const std::bad_alloc &) {
            // Keep Pending and its original waiter so the producer can retry.
        }
        if (preparedStatus) nonSuccess.erase(value);
        if (nonSuccess.empty()) decltype(nonSuccess){}.swap(nonSuccess);
        return false;
    }
};

class KisCompletionRegistry::Private
{
public:
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
    mutable QMutex mutex;
    quint64 registryId = KisPageStoreDetail::allocateMonotonicId<quint64>(
        &s_nextRegistryId);
    quint64 nextSource = 1;
    std::unordered_map<quint64, CompletionSourceState> sources;
};

KisCompletionRegistry::KisCompletionRegistry()
    : d(QSharedPointer<Private>::create())
{
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
        const auto entry = d->sources.try_emplace(source);
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
    const quint64 value = sourceIt->second.nextValue++;
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

    ReadinessRecordPointer released(nullptr, CompletionReadinessRecord::dispose);
    QMutexLocker locker(&d->mutex);
    auto sourceIt = d->sources.find(ticket.source());
    if (sourceIt == d->sources.end() ||
        ticket.domain() != sourceIt->second.domain) {
        return false;
    }

    if (!sourceIt->second.complete(ticket.value(), status)) return false;
    std::shared_ptr<KisPageReadinessSignal> readiness;
    const auto waiting = sourceIt->second.readiness.find(ticket.value());
    if (waiting != sourceIt->second.readiness.end()) {
        readiness = waiting->signal;
        released.reset(&*waiting);
        sourceIt->second.readiness.erase(waiting);
    }
    locker.unlock();
    released.reset();
    if (readiness) readiness->notify();
    return true;
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
                [owner = QWeakPointer<Private>(d), sourceId = ticket.source(), value = ticket.value()]
                (const KisPageReadinessState *expected) {
                    if (const auto state = owner.toStrongRef())
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
    result.storageRecords = quint64(it->second.terminalRanges.size()) + quint64(it->second.nonSuccess.size()) +
                            quint64(it->second.terminalPrefix != 0) + quint64(it->second.readiness.size());
    result.readinessSignals = quint64(it->second.readiness.size());
    result.readinessCapacity = quint64(it->second.readiness.size());
    for (const auto &signal : it->second.readiness) result.readinessWaiters += signal.signal->subscriberCount();
    result.terminalTickets = it->second.terminalPrefix;
    for (auto range = it->second.terminalRanges.cbegin(); range != it->second.terminalRanges.cend(); ++range) {
        result.terminalTickets += range->second - range->first + 1;
    }
    result.pendingTickets = result.allocatedTickets - result.terminalTickets;
    return result;
}

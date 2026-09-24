/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisCompletionRegistry.h"

#include <QHash>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>

#include <atomic>
#include <limits>

namespace {

std::atomic<quint64> s_nextRegistryId{1};

}

struct CompletionSourceState
{
    KisCompletionDomain domain = KisCompletionDomain::Unknown;
    quint64 nextValue = 1;
    // Coverage is independent of terminal status, so alternating results do
    // not fragment the completion index. Every covered value was explicitly
    // completed; a larger completed host ticket never closes a pending gap.
    quint64 terminalPrefix = 0;
    QMap<quint64, quint64> terminalRanges;
    QHash<quint64, KisCompletionStatus> nonSuccess;

    KisCompletionStatus status(quint64 value) const
    {
        if (value == 0 || value >= nextValue) return KisCompletionStatus::Unknown;
        if (value <= terminalPrefix) return nonSuccess.value(value, KisCompletionStatus::Succeeded);
        if (!terminalRanges.isEmpty()) {
            auto last = terminalRanges.cend();
            --last;
            if (value >= last.key()) {
                return value <= last.value() ? nonSuccess.value(value, KisCompletionStatus::Succeeded)
                                             : KisCompletionStatus::Pending;
            }
        }
        auto next = terminalRanges.upperBound(value);
        if (next != terminalRanges.cbegin()) {
            --next;
            if (value <= next.value()) return nonSuccess.value(value, KisCompletionStatus::Succeeded);
        }
        return KisCompletionStatus::Pending;
    }

    bool markTerminal(quint64 value)
    {
        if (value == 0 || value >= nextValue || value <= terminalPrefix) return false;
        if (value == terminalPrefix + 1) {
            terminalPrefix = value;
            const auto next = terminalRanges.begin();
            if (next != terminalRanges.end() && next.key() == value + 1) {
                terminalPrefix = next.value();
                terminalRanges.erase(next);
            }
            return true;
        }
        if (!terminalRanges.isEmpty()) {
            auto last = terminalRanges.end();
            --last;
            if (value > last.value()) {
                if (value == last.value() + 1) {
                    last.value() = value;
                } else {
                    terminalRanges.insert(terminalRanges.cend(), value, value);
                }
                return true;
            }
            if (value >= last.key()) return false;
        }
        auto next = terminalRanges.upperBound(value);
        if (next != terminalRanges.begin()) {
            auto previous = next;
            --previous;
            if (value <= previous.value()) return false;
            if (previous.value() + 1 == value) {
                previous.value() = value;
                if (next != terminalRanges.end() && next.key() == value + 1) {
                    previous.value() = next.value();
                    terminalRanges.erase(next);
                }
                return true;
            }
        }
        if (next != terminalRanges.end() && next.key() == value + 1) {
            terminalRanges.insert(value, next.value());
            terminalRanges.erase(next);
        } else {
            terminalRanges.insert(value, value);
        }
        return true;
    }

    bool complete(quint64 value, KisCompletionStatus status)
    {
        if (!markTerminal(value)) return false;
        if (status != KisCompletionStatus::Succeeded) nonSuccess.insert(value, status);
        return true;
    }
};

class KisCompletionRegistry::Private
{
public:
    mutable QMutex mutex;
    quint64 registryId = KisPageStoreDetail::allocateMonotonicId<quint64>(
        &s_nextRegistryId);
    quint64 nextSource = 1;
    QHash<quint64, CompletionSourceState> sources;
};

KisCompletionRegistry::KisCompletionRegistry()
    : d(new Private)
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
    const quint64 source = d->nextSource++;
    CompletionSourceState state;
    state.domain = domain;
    d->sources.insert(source, state);
    return source;
}

KisCompletionTicket KisCompletionRegistry::allocatePending(quint64 source)
{
    QMutexLocker locker(&d->mutex);
    auto sourceIt = d->sources.find(source);
    if (sourceIt == d->sources.end()) {
        return {};
    }

    if (sourceIt->nextValue == 0 ||
        sourceIt->nextValue == std::numeric_limits<quint64>::max()) {
        return {};
    }
    const quint64 value = sourceIt->nextValue++;
    return KisCompletionTicket(sourceIt->domain, d->registryId, source, value);
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

    QMutexLocker locker(&d->mutex);
    auto sourceIt = d->sources.find(ticket.source());
    if (sourceIt == d->sources.end() ||
        ticket.domain() != sourceIt->domain) {
        return false;
    }

    return sourceIt->complete(ticket.value(), status);
}

KisCompletionStatus KisCompletionRegistry::status(const KisCompletionTicket &ticket) const
{
    if (!ticket.isValid() || ticket.registry() != d->registryId) {
        return KisCompletionStatus::Unknown;
    }

    QMutexLocker locker(&d->mutex);
    const auto sourceIt = d->sources.constFind(ticket.source());
    if (sourceIt == d->sources.constEnd() ||
        ticket.domain() != sourceIt->domain) {
        return KisCompletionStatus::Unknown;
    }
    return sourceIt->status(ticket.value());
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
    const auto it = d->sources.constFind(source);
    if (it == d->sources.constEnd()) return {};
    KisCompletionSourceStatistics result;
    result.knownSource = true;
    result.allocatedTickets = it->nextValue - 1;
    result.storageRecords = quint64(it->terminalRanges.size()) + quint64(it->nonSuccess.size()) +
                            quint64(it->terminalPrefix != 0);
    result.terminalTickets = it->terminalPrefix;
    for (auto range = it->terminalRanges.cbegin(); range != it->terminalRanges.cend(); ++range) {
        result.terminalTickets += range.value() - range.key() + 1;
    }
    result.pendingTickets = result.allocatedTickets - result.terminalTickets;
    return result;
}

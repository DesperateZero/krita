/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisCompletionRegistry.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>

struct CompletionSourceState
{
    KisCompletionSourceDescriptor descriptor;
    quint64 nextValue = 1;
    QHash<quint64, KisCompletionStatus> tickets;
};

class KisCompletionRegistry::Private
{
public:
    mutable QMutex mutex;
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
    return true;
}

quint64 KisCompletionRegistry::registerSource(const KisCompletionSourceDescriptor &descriptor)
{
    if (!descriptor.isValid()) {
        return 0;
    }

    QMutexLocker locker(&d->mutex);
    const quint64 source = d->nextSource++;
    CompletionSourceState state;
    state.descriptor = descriptor;
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

    const quint64 value = sourceIt->nextValue++;
    sourceIt->tickets.insert(value, KisCompletionStatus::Pending);
    return KisCompletionTicket(source, value);
}

bool KisCompletionRegistry::complete(const KisCompletionTicket &ticket,
                                     KisCompletionStatus status)
{
    if (!ticket.isValid() ||
        status == KisCompletionStatus::Unknown ||
        status == KisCompletionStatus::Pending) {
        return false;
    }

    QMutexLocker locker(&d->mutex);
    auto sourceIt = d->sources.find(ticket.source());
    if (sourceIt == d->sources.end()) {
        return false;
    }

    auto ticketIt = sourceIt->tickets.find(ticket.value());
    if (ticketIt == sourceIt->tickets.end() ||
        ticketIt.value() != KisCompletionStatus::Pending) {
        return false;
    }

    ticketIt.value() = status;
    return true;
}

KisCompletionStatus KisCompletionRegistry::status(const KisCompletionTicket &ticket) const
{
    if (!ticket.isValid()) {
        return KisCompletionStatus::Unknown;
    }

    QMutexLocker locker(&d->mutex);
    const auto sourceIt = d->sources.constFind(ticket.source());
    if (sourceIt == d->sources.constEnd()) {
        return KisCompletionStatus::Unknown;
    }
    return sourceIt->tickets.value(ticket.value(), KisCompletionStatus::Unknown);
}

bool KisCompletionRegistry::isKnownSource(quint64 source) const
{
    QMutexLocker locker(&d->mutex);
    return source != 0 && d->sources.contains(source);
}

quint64 KisCompletionRegistry::latestAllocatedValue(quint64 source) const
{
    QMutexLocker locker(&d->mutex);
    const auto sourceIt = d->sources.constFind(source);
    if (sourceIt == d->sources.constEnd() || sourceIt->nextValue == 1) {
        return 0;
    }
    return sourceIt->nextValue - 1;
}

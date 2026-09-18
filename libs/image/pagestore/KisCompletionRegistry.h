/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_COMPLETION_REGISTRY_H
#define KIS_COMPLETION_REGISTRY_H

#include <QScopedPointer>
#include <QString>

#include "KisPageStoreTypes.h"

enum class KisCompletionStatus : quint8 {
    Unknown,
    Pending,
    Succeeded,
    Failed,
    Cancelled
};

struct KRITAIMAGE_EXPORT KisCompletionSourceDescriptor
{
    QString name;
    bool gpuTimeline = false;
    bool cpuJob = false;
    bool ioOperation = false;

    bool isValid() const
    {
        const int kindCount = int(gpuTimeline) + int(cpuJob) + int(ioOperation);
        return !name.isEmpty() && kindCount == 1;
    }
};

/**
 * Bridge for GPU timeline, CPU job and I/O completions. Ticket values are
 * meaningful only when registered here and can never expose native fences.
 */
class KRITAIMAGE_EXPORT KisCompletionRegistry
{
public:
    KisCompletionRegistry();
    ~KisCompletionRegistry();

    bool isOperational() const;
    quint64 registerSource(const KisCompletionSourceDescriptor &descriptor);
    KisCompletionTicket allocatePending(quint64 source);
    bool complete(const KisCompletionTicket &ticket, KisCompletionStatus status);
    KisCompletionStatus status(const KisCompletionTicket &ticket) const;

    bool isKnownSource(quint64 source) const;
    quint64 latestAllocatedValue(quint64 source) const;

private:
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_COMPLETION_REGISTRY_H

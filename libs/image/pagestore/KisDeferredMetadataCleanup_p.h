/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_DEFERRED_METADATA_CLEANUP_P_H
#define KIS_DEFERRED_METADATA_CLEANUP_P_H
#include <QAtomicInteger>
#include <QMutex>
#include "KisPageWaitCondition_p.h"

struct DeferredMetadataCleanupStatistics {
    QAtomicInteger<quint64> deferredCandidates{0};
    QAtomicInteger<quint64> completedCandidates{0};
    QAtomicInteger<quint64> passes{0};
    QAtomicInteger<quint64> units{0};
    QAtomicInteger<quint64> maximumUnitsPerPass{0};
    QAtomicInteger<quint64> nanoseconds{0};
    QAtomicInteger<quint64> maximumPassNanoseconds{0};
    QAtomicInteger<quint64> queueNanoseconds{0};
    QAtomicInteger<quint64> maximumQueueNanoseconds{0};
    QAtomicInteger<qsizetype> pendingCandidates{0};
    QAtomicInteger<qsizetype> peakPendingCandidates{0};
    QAtomicInteger<qsizetype> pendingUnits{0};
    QAtomicInteger<qsizetype> peakPendingUnits{0};
    QMutex idleMutex;
    KisPageWaitCondition idle;
};

template<typename T>
void updateMetadataCleanupMaximum(QAtomicInteger<T> &target, T value)
{
    T observed = target.loadRelaxed();
    while (observed < value && !target.testAndSetRelaxed(observed, value, observed)) {}
}
#endif

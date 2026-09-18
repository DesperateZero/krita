/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_STROKE_COORDINATOR_H
#define KIS_STROKE_COORDINATOR_H

#include "KisGpuStrokeTypes.h"

struct KRITAIMAGE_EXPORT KisStrokeHandle
{
    KisGpuStrokeId strokeId;
    quint64 journalGeneration = 0;

    bool isValid() const { return strokeId.isValid() && journalGeneration != 0; }
};

struct KRITAIMAGE_EXPORT KisStrokeAppendResult
{
    KisPageRequestStatus status = KisPageRequestStatus::Unsupported;
    quint64 acceptedThroughSequence = 0;
    QString error;
};

struct KRITAIMAGE_EXPORT KisStrokeCommitHandle
{
    KisGpuStrokeId strokeId;
    quint64 acceptedThroughSequence = 0;
    KisCompletionTicket completion;

    bool isValid() const
    {
        return strokeId.isValid() &&
               acceptedThroughSequence != 0 &&
               completion.isValid();
    }
};

/**
 * Document-scoped journal/batch/end/cancel coordinator. Admission never drops
 * an accepted dab and never discovers a target through UI global state.
 */
class KRITAIMAGE_EXPORT KisStrokeCoordinator
{
public:
    KisStrokeCoordinator();
    ~KisStrokeCoordinator();

    bool isOperational() const;
    KisStrokeHandle beginStroke(const KisStrokeEpochDesc &epoch);
    KisStrokeAppendResult appendDabs(const KisStrokeHandle &stroke,
                                     const QVector<KisResolvedDab> &dabs);
    KisStrokeCommitHandle endStroke(const KisStrokeHandle &stroke);
    KisCompletionTicket cancelStroke(const KisStrokeHandle &stroke,
                                     const QString &reason);
};

#endif // KIS_STROKE_COORDINATOR_H

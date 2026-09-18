/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_GPU_STROKE_TYPES_H
#define KIS_GPU_STROKE_TYPES_H

#include <QByteArray>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QVector>

#include "KisEvaluationGraphTypes.h"
#include "KisPageStoreTypes.h"

struct KRITAIMAGE_EXPORT KisGpuStrokeId
{
    quint64 value = 0;

    bool isValid() const { return value != 0; }
};

enum class KisStrokeExecutionBackend : quint8 {
    Cpu,
    Gpu
};

struct KRITAIMAGE_EXPORT KisStrokeEpochDesc
{
    KisGpuStrokeId strokeId;
    KisEvaluationEpochId evaluationEpoch;
    QVector<KisSurfaceId> writableTargets;
    quint64 brushProgramGeneration = 0;
    KisStrokeExecutionBackend backend = KisStrokeExecutionBackend::Cpu;
    KisPageTransactionId transaction;

    bool isValid() const
    {
        if (!strokeId.isValid() || !evaluationEpoch.isValid() ||
            writableTargets.isEmpty() || brushProgramGeneration == 0 ||
            !transaction.isValid()) {
            return false;
        }
        QSet<quint64> targets;
        for (KisSurfaceId target : writableTargets) {
            if (!target.isValid() || targets.contains(target.value)) return false;
            targets.insert(target.value);
        }
        return true;
    }
};

/** Immutable output of CPU input/dynamics evaluation. */
struct KRITAIMAGE_EXPORT KisResolvedDab
{
    KisGpuStrokeId strokeId;
    quint64 sequence = 0;
    quint32 subSequence = 0;
    QPointF center;
    qreal radiusX = 0.0;
    qreal radiusY = 0.0;
    qreal rotationRadians = 0.0;
    qreal opacity = 0.0;
    qreal flow = 0.0;
    QByteArray canonicalColor;
    QByteArray blendModeId;
    quint64 brushResourceGeneration = 0;
    QRectF conservativeBounds;

    bool isValid() const
    {
        return strokeId.isValid() &&
               sequence != 0 &&
               radiusX > 0.0 &&
               radiusY > 0.0 &&
               !canonicalColor.isEmpty() &&
               !blendModeId.isEmpty() &&
               brushResourceGeneration != 0 &&
               conservativeBounds.isValid();
    }
};

struct KRITAIMAGE_EXPORT KisStrokeBatch
{
    KisGpuStrokeId strokeId;
    quint64 batchId = 0;
    quint64 firstSequence = 0;
    quint64 lastSequence = 0;
    bool sealed = false;

    bool isValid() const
    {
        return strokeId.isValid() &&
               batchId != 0 &&
               firstSequence != 0 &&
               firstSequence <= lastSequence &&
               sealed;
    }
};

struct KRITAIMAGE_EXPORT KisPageDabRef
{
    quint32 dabIndex = 0;
    quint64 sequence = 0;
    quint32 subSequence = 0;
};

struct KRITAIMAGE_EXPORT KisMicrotileRange
{
    quint32 microtileIndex = 0;
    quint32 firstDabRef = 0;
    quint32 dabRefCount = 0;
};

struct KRITAIMAGE_EXPORT KisPageJob
{
    KisPageKey target;
    KisPageGeneration baseGeneration;
    KisPageGeneration pendingGeneration;
    QVector<KisPageDabRef> orderedDabs;
    QVector<KisMicrotileRange> microtiles;
    KisWriteRequest writeRequest;
    QVector<KisCompletionTicket> dependencies;
};

enum class KisPageWorkCompileStatus : quint8 {
    Unsupported,
    Pending,
    Ready,
    Failed
};

struct KRITAIMAGE_EXPORT KisPageWorkPlan
{
    KisPageWorkCompileStatus status = KisPageWorkCompileStatus::Unsupported;
    KisStrokeBatch batch;
    QVector<KisPageJob> pageJobs;
    QVector<KisPageVersion> resourceReads;
    QString error;

    bool isReady() const
    {
        return status == KisPageWorkCompileStatus::Ready &&
               batch.isValid() &&
               !pageJobs.isEmpty();
    }
};

#endif // KIS_GPU_STROKE_TYPES_H

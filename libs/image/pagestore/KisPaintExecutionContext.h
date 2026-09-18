/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAINT_EXECUTION_CONTEXT_H
#define KIS_PAINT_EXECUTION_CONTEXT_H

#include <QSet>
#include <QSharedPointer>
#include <QVector>

#include "KisDocumentGpuSession.h"
#include "KisEvaluationGraphTypes.h"

/**
 * Immutable-by-convention context captured when a PaintOp/StrokeStrategy is
 * created. It replaces discovery through an active canvas or global router.
 */
struct KRITAIMAGE_EXPORT KisPaintExecutionContext
{
    quint64 documentId = 0;
    quint64 imageGeneration = 0;
    KisEvaluationEpochId evaluationEpoch;
    QVector<KisSurfaceId> writableTargets;
    QSharedPointer<KisDocumentGpuSession> accelerationSession;
    KisAccelerationCapabilities capabilitySnapshot;

    bool hasReadyAccelerationSession() const
    {
        if (!accelerationSession || !evaluationEpoch.isValid() || writableTargets.isEmpty()) {
            return false;
        }
        QSet<quint64> targets;
        for (KisSurfaceId target : writableTargets) {
            if (!target.isValid() || targets.contains(target.value)) return false;
            targets.insert(target.value);
        }
        return accelerationSession->documentId() == documentId &&
               accelerationSession->state() == KisAccelerationSessionState::Ready &&
               accelerationSession->acceptsNewWork() &&
               capabilitySnapshot.isValid() &&
               capabilitySnapshot.serviceId == accelerationSession->backendServiceId() &&
               capabilitySnapshot.deviceGeneration == accelerationSession->deviceGeneration() &&
               capabilitySnapshot.sessionGeneration == accelerationSession->sessionGeneration();
    }
};

#endif // KIS_PAINT_EXECUTION_CONTEXT_H

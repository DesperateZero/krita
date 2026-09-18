/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_EVALUATION_PLANNER_H
#define KIS_EVALUATION_PLANNER_H

#include <QString>
#include <QVector>

#include "KisEvaluationGraphTypes.h"

struct KRITAIMAGE_EXPORT KisEvaluationTaskId
{
    quint64 value = 0;

    bool isValid() const { return value != 0; }
};

enum class KisEvaluationBackendHint : quint8 {
    Any,
    CpuReference,
    GpuCompute,
    ExternalAsync
};

struct KRITAIMAGE_EXPORT KisEvaluationValueReservation
{
    KisEvaluationValueId value;
    quint64 pendingGeneration = 0;
    KisEvaluationValueType type;
    KisSurfaceVersion surface;

    bool isValid() const
    {
        if (!value.isValid() || pendingGeneration == 0 || !type.isValid()) {
            return false;
        }
        return !type.isSurfaceBacked() ||
               (surface.isValid() && surface.generation.value == pendingGeneration);
    }
};

/**
 * Backend-neutral execution task produced from EvalGraphIR. PageStore access
 * plans and native command packets are later stages and are not embedded here.
 */
struct KRITAIMAGE_EXPORT KisEvaluationTaskDesc
{
    KisEvaluationTaskId id;
    KisEvaluationNodeId node;
    QByteArray operationId;
    quint64 semanticVersion = 0;
    QVector<KisEvaluationValueVersion> inputs;
    QVector<KisEvaluationValueReservation> outputs;
    QVector<KisLogicalPageId> dirtyPages;
    QVector<KisEvaluationTaskId> dependencies;
    KisEvaluationBackendHint backendHint = KisEvaluationBackendHint::Any;
    bool exact = false;

    bool isValid() const
    {
        if (!id.isValid() || !node.isValid() || operationId.isEmpty() ||
            semanticVersion == 0 || outputs.isEmpty() || !exact) {
            return false;
        }
        for (const KisEvaluationValueVersion &input : inputs) {
            if (!input.isValid()) return false;
        }
        for (const KisEvaluationValueReservation &output : outputs) {
            if (!output.isValid()) return false;
        }
        for (KisEvaluationTaskId dependency : dependencies) {
            if (!dependency.isValid()) return false;
        }
        return true;
    }
};

struct KRITAIMAGE_EXPORT KisEvaluationPlan
{
    KisEvaluationEpochId epoch;
    QVector<KisEvaluationPortRef> requestedOutputs;
    QVector<KisEvaluationTaskDesc> tasks;
    bool exact = false;

    bool isValid() const
    {
        if (!epoch.isValid() || requestedOutputs.isEmpty() || tasks.isEmpty() || !exact) {
            return false;
        }
        for (const KisEvaluationTaskDesc &task : tasks) {
            if (!task.isValid()) return false;
        }
        return true;
    }
};

class KRITAIMAGE_EXPORT KisEvaluationPlanner
{
public:
    KisEvaluationPlanner();
    ~KisEvaluationPlanner();

    KisEvaluationPlan plan(const KisEvaluationGraphSnapshot &graph) const;
};

#endif // KIS_EVALUATION_PLANNER_H

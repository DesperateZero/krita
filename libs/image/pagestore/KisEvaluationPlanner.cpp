/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisEvaluationPlanner.h"

KisEvaluationPlanner::KisEvaluationPlanner() = default;
KisEvaluationPlanner::~KisEvaluationPlanner() = default;

KisEvaluationPlan KisEvaluationPlanner::plan(const KisEvaluationGraphSnapshot &graph) const
{
    Q_UNUSED(graph);
    // TODO(BR4): validate the DAG, propagate ROI/halo, select exact cache hits,
    // partition CPU/GPU/external work, and emit an immutable task DAG.
    return {};
}

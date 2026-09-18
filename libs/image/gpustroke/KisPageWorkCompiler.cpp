/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageWorkCompiler.h"

KisPageWorkCompiler::KisPageWorkCompiler() = default;
KisPageWorkCompiler::~KisPageWorkCompiler() = default;

KisPageWorkPlan KisPageWorkCompiler::compile(
    const KisStrokeEpochDesc &epoch,
    const KisStrokeBatch &batch,
    const QVector<KisResolvedDab> &resolvedDabs) const
{
    Q_UNUSED(epoch);
    Q_UNUSED(resolvedDabs);

    KisPageWorkPlan plan;
    plan.batch = batch;
    plan.error = QStringLiteral("BR5 PageWorkCompiler is disabled");
    // TODO(BR5): stable-bin by logical page while preserving sequence/subSequence order.
    return plan;
}

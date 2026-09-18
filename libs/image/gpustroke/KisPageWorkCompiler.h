/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_WORK_COMPILER_H
#define KIS_PAGE_WORK_COMPILER_H

#include "KisGpuStrokeTypes.h"

/**
 * Backend-neutral stable page binning contract. Its first implementation is a
 * CPU reference compiler; changing execution backend may not change ordering.
 */
class KRITAIMAGE_EXPORT KisPageWorkCompiler
{
public:
    KisPageWorkCompiler();
    ~KisPageWorkCompiler();

    KisPageWorkPlan compile(const KisStrokeEpochDesc &epoch,
                            const KisStrokeBatch &batch,
                            const QVector<KisResolvedDab> &resolvedDabs) const;
};

#endif // KIS_PAGE_WORK_COMPILER_H

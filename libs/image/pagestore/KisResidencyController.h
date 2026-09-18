/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_RESIDENCY_CONTROLLER_H
#define KIS_RESIDENCY_CONTROLLER_H

#include <QVector>

#include "KisEvaluationGraphTypes.h"
#include "KisPageStoreTypes.h"

enum class KisPageSemanticClass : quint8 {
    Unknown,
    WritableTarget,
    PredictedStroke,
    VisibleDependency,
    DerivedReusable,
    CoveredCold,
    UndoRetained,
    SaveSnapshot
};

struct KRITAIMAGE_EXPORT KisPageSemanticEvent
{
    KisPageKey page;
    KisPageSemanticClass semanticClass = KisPageSemanticClass::Unknown;
    KisEvaluationEpochId evaluationEpoch;
    quint64 sequence = 0;
};

struct KRITAIMAGE_EXPORT KisResidencyBudgetSnapshot
{
    quint64 cpuRamBytes = 0;
    quint64 umaBytes = 0;
    quint64 vramBytes = 0;
    quint64 ssdBytes = 0;
};

struct KRITAIMAGE_EXPORT KisResidencyOperation
{
    KisPageVersion version;
    KisPageAccessDomain source = KisPageAccessDomain::Unknown;
    KisPageAccessDomain target = KisPageAccessDomain::Unknown;
    bool authorityHandoff = false;
};

/**
 * Policy-only planner. It never changes PageStore state or retires a replica.
 */
class KRITAIMAGE_EXPORT KisResidencyController
{
public:
    KisResidencyController();
    ~KisResidencyController();

    void consumeSemanticEvents(const QVector<KisPageSemanticEvent> &events);
    QVector<KisResidencyOperation> plan(const KisResidencyBudgetSnapshot &budget) const;
};

#endif // KIS_RESIDENCY_CONTROLLER_H

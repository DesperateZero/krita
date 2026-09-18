/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisResidencyController.h"

KisResidencyController::KisResidencyController() = default;
KisResidencyController::~KisResidencyController() = default;

void KisResidencyController::consumeSemanticEvents(const QVector<KisPageSemanticEvent> &events)
{
    Q_UNUSED(events);
    // TODO(BR4): retain document-local semantic evidence, pins, and hysteresis inputs.
}

QVector<KisResidencyOperation> KisResidencyController::plan(const KisResidencyBudgetSnapshot &budget) const
{
    Q_UNUSED(budget);
    // TODO(BR4): rank legal replica migrations; PageStore remains the executor.
    return {};
}

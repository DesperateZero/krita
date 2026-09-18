/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisStrokeCoordinator.h"

KisStrokeCoordinator::KisStrokeCoordinator() = default;
KisStrokeCoordinator::~KisStrokeCoordinator() = default;

bool KisStrokeCoordinator::isOperational() const
{
    // TODO(BR5): enable after lossless journal and backpressure tests pass.
    return false;
}

KisStrokeHandle KisStrokeCoordinator::beginStroke(const KisStrokeEpochDesc &epoch)
{
    Q_UNUSED(epoch);
    // TODO(BR5): freeze graph, targets, resources and backend for the full epoch.
    return {};
}

KisStrokeAppendResult KisStrokeCoordinator::appendDabs(
    const KisStrokeHandle &stroke,
    const QVector<KisResolvedDab> &dabs)
{
    Q_UNUSED(stroke);
    Q_UNUSED(dabs);

    KisStrokeAppendResult result;
    result.error = QStringLiteral("BR5 stroke journal is disabled");
    // TODO(BR5): append all-or-explicit-backpressure; never silently drop accepted dabs.
    return result;
}

KisStrokeCommitHandle KisStrokeCoordinator::endStroke(const KisStrokeHandle &stroke)
{
    Q_UNUSED(stroke);
    // TODO(BR5): seal the final contiguous range and return its commit ticket.
    return {};
}

KisCompletionTicket KisStrokeCoordinator::cancelStroke(const KisStrokeHandle &stroke,
                                                        const QString &reason)
{
    Q_UNUSED(stroke);
    Q_UNUSED(reason);
    // TODO(BR5): discard write generations while retaining recoverable before-images.
    return {};
}

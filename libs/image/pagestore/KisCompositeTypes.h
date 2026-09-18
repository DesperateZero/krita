/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_COMPOSITE_TYPES_H
#define KIS_COMPOSITE_TYPES_H

#include <QByteArray>
#include <QSize>
#include <QString>
#include <QVector>

#include "KisEvaluationGraphTypes.h"
#include "KisPageStoreTypes.h"

struct KRITAIMAGE_EXPORT KisCompositeDependency
{
    KisSurfaceVersion surface;
    QVector<KisPageVersion> exactPages;
    QByteArray dependencyDigest;

    bool isValid() const
    {
        return surface.isValid() &&
               !exactPages.isEmpty() &&
               !dependencyDigest.isEmpty();
    }
};

struct KRITAIMAGE_EXPORT KisCompositePageAccess
{
    KisPageKey output;
    KisPageGeneration pendingGeneration;
    QVector<KisPageVersion> reads;
    QVector<KisCompletionTicket> readiness;

    bool isValid() const
    {
        return output.isValid() &&
               pendingGeneration.isValid() &&
               !reads.isEmpty();
    }
};

/**
 * Exact, surface-specific work description produced after EvalGraphIR
 * semantics and PageStore access have both been resolved. It is one operation
 * plan consumed by a specialized executor, not the global evaluation DAG.
 */
struct KRITAIMAGE_EXPORT KisCompositeAccessPlan
{
    KisEvaluationEpochId evaluationEpoch;
    KisSurfaceVersion output;
    KisSurfaceFormat workingFormat;
    QByteArray operationId;
    QVector<KisCompositeDependency> dependencies;
    QVector<KisCompositePageAccess> pages;
    quint64 semanticVersion = 0;
    bool exact = false;

    QString validationError() const
    {
        if (!evaluationEpoch.isValid()) {
            return QStringLiteral("Composite plan has no evaluation epoch");
        }
        if (!output.isValid()) {
            return QStringLiteral("Composite plan has no output surface generation");
        }
        if (!workingFormat.isValid()) {
            return QStringLiteral("Composite plan has no valid working format");
        }
        if (operationId.isEmpty() || semanticVersion == 0) {
            return QStringLiteral("Composite plan has no operation identity or semantic version");
        }
        if (!exact || dependencies.isEmpty() || pages.isEmpty()) {
            return QStringLiteral("Composite plan is incomplete or approximate");
        }
        for (const KisCompositeDependency &dependency : dependencies) {
            if (!dependency.isValid()) {
                return QStringLiteral("Composite plan contains an invalid dependency");
            }
        }
        for (const KisCompositePageAccess &page : pages) {
            if (!page.isValid()) {
                return QStringLiteral("Composite plan contains an invalid page access");
            }
        }
        return {};
    }

    bool isValid() const { return validationError().isEmpty(); }
};

struct KRITAIMAGE_EXPORT KisDisplaySurfaceSnapshot
{
    KisEvaluationEpochId evaluationEpoch;
    KisSurfaceVersion surface;
    KisSurfaceFormat format;
    QSize pixelExtent;
    QVector<KisLogicalPageId> damagedPages;
    QByteArray dependencyDigest;
    QByteArray displayTransformId;
    KisCompletionTicket ready;

    QString validationError() const
    {
        if (!evaluationEpoch.isValid() || !surface.isValid()) {
            return QStringLiteral("Display snapshot has no evaluation epoch or surface generation");
        }
        if (!format.isValid() || !pixelExtent.isValid()) {
            return QStringLiteral("Display snapshot format or extent is invalid");
        }
        if (dependencyDigest.isEmpty() || displayTransformId.isEmpty()) {
            return QStringLiteral("Display snapshot lacks dependency or transform identity");
        }
        if (!ready.isValid()) {
            return QStringLiteral("Display snapshot has no readiness ticket");
        }
        return {};
    }

    bool isValid() const { return validationError().isEmpty(); }
};

#endif // KIS_COMPOSITE_TYPES_H

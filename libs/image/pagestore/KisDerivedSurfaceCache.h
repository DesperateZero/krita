/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_DERIVED_SURFACE_CACHE_H
#define KIS_DERIVED_SURFACE_CACHE_H

#include <QByteArray>
#include <QVector>

#include <optional>

#include "KisEvaluationGraphTypes.h"

/** Exact semantic and dependency identity of one derived output page. */
struct KRITAIMAGE_EXPORT KisDerivedSurfaceCacheKey
{
    KisEvaluationEpochId evaluationEpoch;
    KisEvaluationNodeId producerNode;
    quint32 outputPortId = 0;
    quint64 semanticVersion = 0;
    KisLogicalPageId outputPage;
    QVector<KisEvaluationValueVersion> inputValues;
    QVector<KisPageVersion> sourcePages;
    QVector<quint64> propertyGenerations;
    QByteArray evaluationContextDigest;

    bool isValid() const
    {
        if (!evaluationEpoch.isValid() || !producerNode.isValid() ||
            outputPortId == 0 || semanticVersion == 0 ||
            evaluationContextDigest.isEmpty()) {
            return false;
        }
        for (const KisEvaluationValueVersion &input : inputValues) {
            if (!input.isValid()) return false;
        }
        for (const KisPageVersion &page : sourcePages) {
            if (!page.isValid()) return false;
        }
        for (quint64 generation : propertyGenerations) {
            if (generation == 0) return false;
        }
        return true;
    }
};

struct KRITAIMAGE_EXPORT KisCoverageCertificate
{
    KisEvaluationEpochId evaluationEpoch;
    QVector<KisEvaluationValueId> mutationRoots;
    QVector<KisPageVersion> sourceDependencies;
    KisPageVersion derivedCoverage;
    quint64 producerRouteId = 0;
    QByteArray evaluationContextDigest;

    bool isValid() const
    {
        if (!evaluationEpoch.isValid() || !derivedCoverage.isValid() ||
            producerRouteId == 0 ||
            evaluationContextDigest.isEmpty()) {
            return false;
        }
        for (KisEvaluationValueId root : mutationRoots) {
            if (!root.isValid()) return false;
        }
        for (const KisPageVersion &page : sourceDependencies) {
            if (!page.isValid()) return false;
        }
        return true;
    }
};

/**
 * Dependency-qualified cache for surface-backed EvalGraphIR outputs. Entries
 * are never canonical authority. Non-surface values belong to a separate
 * value/resource store rather than being forced into PageStore.
 */
class KRITAIMAGE_EXPORT KisDerivedSurfaceCache
{
public:
    KisDerivedSurfaceCache();
    ~KisDerivedSurfaceCache();

    bool isOperational() const;
    std::optional<KisPageVersion> lookup(const KisDerivedSurfaceCacheKey &key) const;
    bool publish(const KisDerivedSurfaceCacheKey &key,
                 const KisPageVersion &derivedPage);
    std::optional<KisCoverageCertificate> coverageFor(
        const KisPageVersion &canonicalPage) const;
    void invalidateEvaluationEpoch(KisEvaluationEpochId evaluationEpoch);
};

#endif // KIS_DERIVED_SURFACE_CACHE_H

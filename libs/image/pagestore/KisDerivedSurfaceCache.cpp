/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisDerivedSurfaceCache.h"

KisDerivedSurfaceCache::KisDerivedSurfaceCache() = default;
KisDerivedSurfaceCache::~KisDerivedSurfaceCache() = default;

bool KisDerivedSurfaceCache::isOperational() const
{
    // TODO(BR4): enable after exact dependency and randomized DAG invalidation tests.
    return false;
}

std::optional<KisPageVersion> KisDerivedSurfaceCache::lookup(
    const KisDerivedSurfaceCacheKey &key) const
{
    Q_UNUSED(key);
    // TODO(BR4): return only an exact semantic/dependency/context match.
    return std::nullopt;
}

bool KisDerivedSurfaceCache::publish(
    const KisDerivedSurfaceCacheKey &key,
    const KisPageVersion &derivedPage)
{
    Q_UNUSED(key);
    Q_UNUSED(derivedPage);
    // TODO(BR4): reject canonical surfaces and stale evaluation epochs.
    return false;
}

std::optional<KisCoverageCertificate> KisDerivedSurfaceCache::coverageFor(
    const KisPageVersion &canonicalPage) const
{
    Q_UNUSED(canonicalPage);
    // TODO(BR4): prove evaluation context, dependency generations and producer route.
    return std::nullopt;
}

void KisDerivedSurfaceCache::invalidateEvaluationEpoch(
    KisEvaluationEpochId evaluationEpoch)
{
    Q_UNUSED(evaluationEpoch);
    // TODO(BR4): retire matching derived entries after their last-use tickets.
}

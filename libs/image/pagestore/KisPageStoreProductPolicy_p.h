/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KIS_PAGE_STORE_PRODUCT_POLICY_P_H
#define KIS_PAGE_STORE_PRODUCT_POLICY_P_H
#include "KisPageBackingBudget_p.h"
#include "KisPageStoreTypes.h"
#include "kis_image_config.h"
#include <limits>
inline void kisPagePolicyError(QString *out, const QString &message) { if (out) *out = message; }
// Shared product configuration only. This is not a mutation/acceptance service.
struct KisPageStoreProductPolicy
{
    quint64 providerBytes = 0;
    KisPageBackingLimits limits;
};

inline bool kisPageConfigMiBToBytes(int value, quint64 *bytes,
                      QString *error)
{
    constexpr quint64 bytesPerMiB = quint64(1) << 20;
    if (!bytes || value < 0
        || quint64(value) > std::numeric_limits<quint64>::max() / bytesPerMiB) {
        kisPagePolicyError(
            error, QStringLiteral("PageStore configured memory limit is invalid"));
        return false;
    }
    *bytes = quint64(value) * bytesPerMiB;
    return true;
}

inline bool kisDerivePageStoreProductPolicy(KisPageStoreProductPolicy *policy, QString *error)
{
    if (!policy) {
        kisPagePolicyError(
            error, QStringLiteral("PageStore backing policy output is missing"));
        return false;
    }
    const KisImageConfig config(true);
    quint64 ramBytes = 0;
    quint64 poolBytes = 0;
    quint64 swapBytes = 0;
    if (!kisPageConfigMiBToBytes(config.tilesHardLimit(), &ramBytes, error)
        || !kisPageConfigMiBToBytes(config.poolLimit(), &poolBytes, error)
        || !kisPageConfigMiBToBytes(config.maxSwapSize(), &swapBytes, error)) {
        return false;
    }
    if (swapBytes > std::numeric_limits<quint64>::max() - ramBytes
        || poolBytes > std::numeric_limits<quint64>::max() - ramBytes) {
        kisPagePolicyError(
            error, QStringLiteral("PageStore configured memory limits overflow"));
        return false;
    }
    const quint64 logicalBytes = ramBytes + swapBytes;
    const quint64 internalCpuBytes = ramBytes + poolBytes;
    KisPageStoreProductPolicy result;
    result.providerBytes = logicalBytes;
    result.limits.logicalCurrentBytes = logicalBytes;
    result.limits.retainedHistoryBytes = logicalBytes;
    result.limits.residentCurrentBytes = {ramBytes, 0, 0, swapBytes};
    result.limits.residentHistoryBytes = result.limits.residentCurrentBytes;
    result.limits.activePendingBytes = ramBytes;
    // BR1 has no GPU transfer provider. Rejecting this bucket prevents a
    // finite product configuration from advertising unimplemented capacity.
    result.limits.inFlightReplicaBytes = 0;
    result.limits.retirementDebtBytes = logicalBytes;
    // These allocations are outside the tiles payload allocator. Their class
    // ceilings therefore use the configured RAM total (tiles plus pool). The
    // default zero-sized pool must not disable required default-page reads.
    result.limits.optionalCacheBytes = internalCpuBytes;
    result.limits.metadataArenaBytes = internalCpuBytes;
    result.limits.durableStoreCapacity = swapBytes;
    *policy = result;
    kisPagePolicyError(error, {});
    return true;
}

inline std::shared_ptr<KisBackingBudgetController> kisAcquireProductNonPayloadBudget(
    const KisPageStoreProductPolicy &policy,
    QString *error)
{
    return kisAcquirePageStoreProcessBudget(policy.limits.metadataArenaBytes, error);
}

#endif

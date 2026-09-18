/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisUnifiedSsdPageStore.h"

namespace {

KisReplicaOperation unsupportedSsdOperation()
{
    KisReplicaOperation operation;
    operation.error = QStringLiteral("BR0 SSD replica provider is disabled");
    return operation;
}

}

KisUnifiedSsdPageStore::KisUnifiedSsdPageStore() = default;
KisUnifiedSsdPageStore::~KisUnifiedSsdPageStore() = default;

QString KisUnifiedSsdPageStore::name() const
{
    return QStringLiteral("Krita unified SSD page store (disabled)");
}

KisReplicaCapabilities KisUnifiedSsdPageStore::capabilities() const
{
    // TODO(BR3): advertise Ssd/durable only after atomic index and recovery tests pass.
    return {};
}

KisReplicaOperation KisUnifiedSsdPageStore::requestReplica(const KisPageVersion &version,
                                                            KisPageAccessDomain domain,
                                                            KisPageAccessMode mode,
                                                            KisPagePriority priority)
{
    Q_UNUSED(version);
    Q_UNUSED(domain);
    Q_UNUSED(mode);
    Q_UNUSED(priority);
    // TODO(BR3): schedule checked materialization from the committed segment index.
    return unsupportedSsdOperation();
}

KisReplicaOperation KisUnifiedSsdPageStore::prepareWrite(const KisPageKey &key,
                                                          KisPageGeneration generation,
                                                          KisPageAccessDomain domain,
                                                          KisPagePriority priority)
{
    Q_UNUSED(key);
    Q_UNUSED(generation);
    Q_UNUSED(domain);
    Q_UNUSED(priority);
    // TODO(BR3): reserve an append record without changing page authority.
    return unsupportedSsdOperation();
}

KisReplicaOperation KisUnifiedSsdPageStore::transfer(const KisReplicaHandle &source,
                                                      const KisReplicaHandle &target,
                                                      KisPagePriority priority)
{
    Q_UNUSED(source);
    Q_UNUSED(target);
    Q_UNUSED(priority);
    // TODO(BR3): compress, checksum, append, fsync policy, then atomically commit index.
    return unsupportedSsdOperation();
}

bool KisUnifiedSsdPageStore::validate(const KisReplicaHandle &replica,
                                      KisPageGeneration expectedGeneration) const
{
    Q_UNUSED(replica);
    Q_UNUSED(expectedGeneration);
    // TODO(BR3): verify record identity, generation, size, codec, and checksum.
    return false;
}

void KisUnifiedSsdPageStore::retire(const KisReplicaHandle &replica,
                                    const KisCompletionTicket &lastUse)
{
    Q_UNUSED(replica);
    Q_UNUSED(lastUse);
    // TODO(BR3): mark unreachable in the index; reclaim only through background GC.
}

KisReplicaMemoryUsage KisUnifiedSsdPageStore::memoryUsage() const
{
    // TODO(BR3): expose committed/live/garbage bytes and configured budget.
    return {};
}

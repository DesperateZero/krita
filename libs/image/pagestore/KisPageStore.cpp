/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageStore.h"

KisPageStore::KisPageStore() = default;
KisPageStore::~KisPageStore() = default;

bool KisPageStore::isOperational() const
{
    // TODO(BR1): enable only after the CPU generation/lease reference path passes.
    return false;
}

bool KisPageStore::registerReplicaProvider(const QSharedPointer<KisPageReplicaProvider> &provider)
{
    Q_UNUSED(provider);
    // TODO(BR1): register providers by stable identity and reject domain conflicts.
    return false;
}

KisReadRequest KisPageStore::acquireRead(const KisPageRange &range,
                                         KisPageAccessDomain domain,
                                         KisPagePriority priority)
{
    Q_UNUSED(priority);

    KisReadRequest request;
    request.range = range;
    request.requestedDomain = domain;
    request.error = QStringLiteral("BR0 PageStore is disabled");
    // TODO(BR1): reserve an exact generation and schedule materialization.
    return request;
}

KisWriteRequest KisPageStore::acquireWrite(const KisPageRange &range,
                                           KisPageAccessDomain domain,
                                           KisPageWriteMode mode,
                                           KisPagePriority priority)
{
    Q_UNUSED(priority);

    KisWriteRequest request;
    request.range = range;
    request.requestedDomain = domain;
    request.mode = mode;
    request.error = QStringLiteral("BR0 PageStore is disabled");
    // TODO(BR1): reserve one writer and a non-published generation per page.
    return request;
}

KisReadLease KisPageStore::resolve(const KisReadRequest &request,
                                   const KisCompletionTicket &completion)
{
    Q_UNUSED(request);
    Q_UNUSED(completion);
    // TODO(BR1): validate readiness and generation before constructing a lease.
    return {};
}

KisWriteLease KisPageStore::resolve(const KisWriteRequest &request,
                                    const KisCompletionTicket &completion)
{
    Q_UNUSED(request);
    Q_UNUSED(completion);
    // TODO(BR1): expose the reserved writable replica only after COW is retained.
    return {};
}

KisCompletionTicket KisPageStore::publish(KisWriteLease &&lease,
                                          const KisCompletionTicket &producerCompletion)
{
    Q_UNUSED(lease);
    Q_UNUSED(producerCompletion);
    // TODO(BR1): publish only from the completion callback after validation.
    return {};
}

void KisPageStore::cancel(KisWriteLease &&lease)
{
    Q_UNUSED(lease);
    // TODO(BR1): discard the reserved generation while preserving old authority.
}

KisPageTransaction KisPageStore::beginTransaction(KisImageEpochId baseEpoch)
{
    Q_UNUSED(baseEpoch);
    // TODO(BR1): allocate a document-local transaction ledger entry.
    return {};
}

KisImageEpochCommitTicket KisPageStore::commit(const KisPageTransaction &transaction,
                                               const KisPreparedPageSet &preparedPages)
{
    Q_UNUSED(transaction);
    Q_UNUSED(preparedPages);
    // TODO(BR1): atomically install an immutable epoch manifest.
    return {};
}

void KisPageStore::abort(const KisPageTransaction &transaction)
{
    Q_UNUSED(transaction);
    // TODO(BR1): release only transaction-owned prepared generations.
}

KisImageEpochSnapshot KisPageStore::captureCommittedEpoch() const
{
    // TODO(BR1): retain and return the current immutable image epoch.
    return {};
}

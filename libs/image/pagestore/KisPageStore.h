/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_STORE_H
#define KIS_PAGE_STORE_H

#include <QSharedPointer>

#include "KisPageReplicaProvider.h"
#include "KisPageStoreTypes.h"

/**
 * Canonical owner of page generation, leases, publication and authority.
 *
 * BR0 is a deliberately disabled contract skeleton. No method may report
 * success until the CPU-only BR1 state machine and invariant tests land.
 */
class KRITAIMAGE_EXPORT KisPageStore
{
public:
    KisPageStore();
    ~KisPageStore();

    bool isOperational() const;

    bool registerReplicaProvider(const QSharedPointer<KisPageReplicaProvider> &provider);

    KisReadRequest acquireRead(const KisPageRange &range,
                               KisPageAccessDomain domain,
                               KisPagePriority priority);
    KisWriteRequest acquireWrite(const KisPageRange &range,
                                 KisPageAccessDomain domain,
                                 KisPageWriteMode mode,
                                 KisPagePriority priority);

    KisReadLease resolve(const KisReadRequest &request,
                         const KisCompletionTicket &completion);
    KisWriteLease resolve(const KisWriteRequest &request,
                          const KisCompletionTicket &completion);

    KisCompletionTicket publish(KisWriteLease &&lease,
                                const KisCompletionTicket &producerCompletion);
    void cancel(KisWriteLease &&lease);

    KisPageTransaction beginTransaction(KisImageEpochId baseEpoch);
    KisImageEpochCommitTicket commit(const KisPageTransaction &transaction,
                                     const KisPreparedPageSet &preparedPages);
    void abort(const KisPageTransaction &transaction);
    KisImageEpochSnapshot captureCommittedEpoch() const;
};

#endif // KIS_PAGE_STORE_H

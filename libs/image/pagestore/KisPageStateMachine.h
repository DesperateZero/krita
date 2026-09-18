/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_STATE_MACHINE_H
#define KIS_PAGE_STATE_MACHINE_H

#include <QString>
#include <QVector>

#include "KisPageReplicaProvider.h"

enum class KisPagePublicationState : quint8 {
    Unpublished,
    Prepared,
    Published,
    Historical,
    Retiring
};

enum class KisReplicaValidity : quint8 {
    Allocated,
    Materializing,
    Valid,
    Failed,
    Retiring
};

struct KRITAIMAGE_EXPORT KisReplicaStateSnapshot
{
    KisReplicaHandle replica;
    KisReplicaValidity validity = KisReplicaValidity::Allocated;
    quint64 providerEpoch = 0;
    quint32 readLeaseCount = 0;
    quint32 pinCount = 0;
};

struct KRITAIMAGE_EXPORT KisPageStateSnapshot
{
    KisPageVersion version;
    KisPagePublicationState publication = KisPagePublicationState::Unpublished;
    QVector<KisReplicaStateSnapshot> replicas;
    KisReplicaHandle authority;
    quint64 writerToken = 0;
};

enum class KisPageTransitionKind : quint8 {
    AcquireRead,
    ReleaseRead,
    AcquireWrite,
    PrepareWrite,
    PublishWrite,
    CancelWrite,
    BeginMaterialize,
    CompleteMaterialize,
    FailMaterialize,
    BeginAuthorityHandoff,
    CommitAuthorityHandoff,
    BeginRetire,
    CompleteRetire,
    CommitTransaction,
    AbortTransaction
};

struct KRITAIMAGE_EXPORT KisPageTransition
{
    KisPageTransitionKind kind = KisPageTransitionKind::AcquireRead;
    KisPageVersion version;
    KisReplicaHandle source;
    KisReplicaHandle target;
    quint64 operationId = 0;
    quint64 providerEpoch = 0;
};

struct KRITAIMAGE_EXPORT KisPageTransitionResult
{
    bool accepted = false;
    KisPageStateSnapshot next;
    QString rejectionReason;
};

/**
 * Shared deterministic transition function for reference and production
 * providers. BR1 must implement it before PageStore can become operational.
 */
class KRITAIMAGE_EXPORT KisPageStateMachine
{
public:
    KisPageStateMachine();
    ~KisPageStateMachine();

    KisPageTransitionResult apply(const KisPageStateSnapshot &current,
                                  const KisPageTransition &transition) const;
    bool validateInvariants(const KisPageStateSnapshot &state,
                            QString *failureReason = nullptr) const;
};

#endif // KIS_PAGE_STATE_MACHINE_H

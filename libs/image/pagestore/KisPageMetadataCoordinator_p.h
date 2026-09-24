/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_METADATA_COORDINATOR_P_H
#define KIS_PAGE_METADATA_COORDINATOR_P_H

#include "KisPageMetadataArena_p.h"
#include "KisPageMetadataIndex_p.h"
#include "KisPageStateMachine.h"

/**
 * Compact authoritative records. Public/reference snapshots are projections
 * of these records and never own production metadata. M2 stores them in the
 * shard-owned generational arenas; M3 links and indexes their typed slots
 * while the compact page/activity directory projects public values on demand.
 */
struct KisVersionRecord {
    KisPageVersion version;
    KisVersionSlotId slot;
    KisVersionSlotId previousVersion;
    KisVersionSlotId nextVersion;
    KisVersionSlotId previousHistory;
    KisVersionSlotId nextHistory;
    KisVersionSlotId nextMutable;
    KisReplicaSlotId firstReplica;
    KisReplicaSlotId authorityReplica;
    KisMetadataOverflowSlotId capturedReadViews;
    KisPageTransactionId preparedBy;
    quint32 replicaCount = 0;
    KisPagePublicationState publication = KisPagePublicationState::Unpublished;
};

struct KisReplicaRecord {
    KisReplicaAllocationIdentity identity;
    quint64 layoutRevision = 0;
    quint64 formatId = 0;
    quint64 byteSize = 0;
    KisVersionSlotId ownerVersion;
    KisReplicaSlotId nextReplica;
    KisMetadataOverflowSlotId overflow;
    KisPageOperationId activeOperation;
    QRect validRect;
    QSize pageExtent;
    quint32 rowStride = 0;
    quint32 pinCount = 0;
    KisPageAccessDomain domain = KisPageAccessDomain::Unknown;
    KisReplicaValidity validity = KisReplicaValidity::Allocated;
};

enum class KisMetadataOverflowKind : quint8 {
    CapturedReadView,
    ReadLease,
    PendingLastUse
};

struct KisMetadataOverflowNode {
    KisMetadataOverflowSlotId next;
    KisCompletionTicket completion;
    quint64 value = 0;
    KisMetadataOverflowKind kind = KisMetadataOverflowKind::CapturedReadView;
};

static_assert(sizeof(KisVersionRecord) <= 128);
static_assert(sizeof(KisReplicaRecord) <= 128);
static_assert(sizeof(KisMetadataOverflowNode) <= 64);

template<>
struct KisSlotIdFor<KisVersionRecord> {
    using Type = KisVersionSlotId;
};

template<>
struct KisSlotIdFor<KisReplicaRecord> {
    using Type = KisReplicaSlotId;
};

template<>
struct KisSlotIdFor<KisMetadataOverflowNode> {
    using Type = KisMetadataOverflowSlotId;
};

#endif // KIS_PAGE_METADATA_COORDINATOR_P_H

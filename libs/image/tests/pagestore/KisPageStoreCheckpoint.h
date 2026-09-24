/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_STORE_CHECKPOINT_H
#define KIS_PAGE_STORE_CHECKPOINT_H

#include <QByteArray>
#include <QSharedPointer>
#include <QVector>

#include "KisPageStore.h"

struct KisPageStoreCheckpointPage
{
    KisPageVersion version;
    KisPageAllocationDescriptor descriptor;
    QByteArray canonicalBytes;
    QByteArray checksum;

    bool isValid() const;
};

/**
 * CPU-portable save image of one committed ImageEpoch. Rows are tightly
 * packed canonical bytes, never provider padding or allocation identities.
 */
struct KisPageStoreCheckpoint
{
    quint32 schemaVersion = 2;
    KisImageEpochSnapshot snapshot;
    QVector<KisPageStoreCheckpointPage> pages;

    bool isValid() const;
};

class KisPageStoreCheckpointCodec
{
public:
    static bool capture(KisPageStore *store,
                        KisPageStoreCheckpoint *checkpoint,
                        QString *error = nullptr);
    static QByteArray encode(const KisPageStoreCheckpoint &checkpoint,
                             QString *error = nullptr);
    static bool decode(const QByteArray &encoded,
                       KisPageStoreCheckpoint *checkpoint,
                       QString *error = nullptr);
    static bool restore(
        const KisPageStoreCheckpoint &checkpoint,
        KisPageStore *store,
        const QSharedPointer<KisPageReplicaProvider> &cpuProvider,
        const QSharedPointer<KisCompletionRegistry> &completions,
        qsizetype metadataShardCount,
        QString *error = nullptr);
};

#endif // KIS_PAGE_STORE_CHECKPOINT_H

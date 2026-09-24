/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_STORE_RANDOM_ACCESSOR_H
#define KIS_PAGE_STORE_RANDOM_ACCESSOR_H

#include <QScopedPointer>

#include "kis_random_accessor_ng.h"
#include "KisPageStoreCpuAccessSession.h"

/**
 * Test-only KisRandomAccessorNG-compatible view backed by PageStore access.
 *
 * The accessor publishes touched write generations on finish/destruction but
 * never commits its surrounding transaction. This matches the existing
 * iterator/memento split: the operation owner decides the multi-page commit.
 * Raw pointers are borrowed for the current position and must not be retained
 * across moveTo/finish/cancel/destruction. Same-page moves reuse the pinned
 * span; cross-page moves release native target and before-image pins without
 * publishing. Control write leases remain until finish when no native parking
 * contract exists. Direct OperationSpans sessions keep their longer lifetime.
 */
class KisPageStoreRandomAccessor final
    : public KisRandomAccessorNG
{
public:
    KisPageStoreRandomAccessor(KisPageStore *store,
                               KisSurfaceId surface,
                               const KisPageReadView &view,
                               qint32 offsetX = 0,
                               qint32 offsetY = 0,
                               KisPagePriority priority =
                                   KisPagePriority::Normal);
    KisPageStoreRandomAccessor(KisPageStore *store,
                               KisSurfaceId surface,
                               const KisPageTransaction &transaction,
                               KisPageWriteMode mode,
                               qint32 offsetX = 0,
                               qint32 offsetY = 0,
                               KisPagePriority priority =
                                   KisPagePriority::Interactive);
    ~KisPageStoreRandomAccessor() override;

    bool isValid() const;
    KisSurfaceEpochState surfaceState() const;
    QString error() const;
    bool finish(QString *error = nullptr);
    void cancel();

    void moveTo(qint32 x, qint32 y) override;
    quint8 *rawData() override;
    const quint8 *oldRawData() const override;
    const quint8 *rawDataConst() const override;
    qint32 numContiguousColumns(qint32 x) const override;
    qint32 numContiguousRows(qint32 y) const override;
    qint32 rowStride(qint32 x, qint32 y) const override;
    qint32 x() const override;
    qint32 y() const override;

private:
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_PAGE_STORE_RANDOM_ACCESSOR_H

/*
 *  SPDX-FileCopyrightText: 2017 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_painter.h"
#include "kis_painter_p.h"

#include "kis_paint_device.h"
#include "kis_fixed_paint_device.h"
#include "kis_random_accessor_ng.h"
#include "KisRenderedDab.h"
#include "KisPixelWriteCursor.h"
#include "KisStrokeJobFailureContext.h"
#include <utility>

bool KisPainter::Private::applyDevice(const QRect &applyRect,
                                      const KisRenderedDab &dab,
                                      KisPixelWriteCursor *dstIt,
                                      const KoColorSpace *srcColorSpace,
                                      const KoCompositeOp *operation,
                                      KoCompositeOp::ParameterInfo &localParamInfo,
                                      bool useDabParameters,
                                      const QRect &sourceRect)
{
    const QRect sourceBounds = dab.device->bounds();
    const QRect sourceView = sourceRect.isNull() ? sourceBounds : sourceRect;
    const QRect dabRect(dab.offset, sourceView.size());
    const QRect rc = applyRect & dabRect;
    if (rc.isEmpty()) return true;

    const int srcPixelSize = srcColorSpace->pixelSize();
    const int dabRowStride = srcPixelSize * sourceBounds.width();
    if (useDabParameters) {
        localParamInfo.setOpacityAndAverage(dab.opacity, dab.averageOpacity);
        localParamInfo.flow = dab.flow;
    }


    qint32 dstY = rc.y();
    qint32 rowsRemaining = rc.height();

    while (rowsRemaining > 0) {
        qint32 dstX = rc.x();

        qint32 numContiguousDstRows = dstIt->numContiguousRows(dstY);
        qint32 rows = qMin(rowsRemaining, numContiguousDstRows);

        if (rows <= 0) return false;
        qint32 columnsRemaining = rc.width();

        while (columnsRemaining > 0) {

            qint32 numContiguousDstColumns = dstIt->numContiguousColumns(dstX);
            qint32 columns = qMin(numContiguousDstColumns, columnsRemaining);

            if (columns <= 0) return false;
            qint32 dstRowStride = dstIt->rowStride(dstX, dstY);
            dstIt->moveTo(dstX, dstY);

            localParamInfo.dstRowStart   = dstIt->rawData();
            localParamInfo.dstRowStride  = dstRowStride;
            localParamInfo.maskRowStart  = 0;
            localParamInfo.maskRowStride = 0;
            localParamInfo.rows          = rows;
            localParamInfo.cols          = columns;


            const int dabX = dstX - dabRect.x() + sourceView.x() - sourceBounds.x();
            const int dabY = dstY - dabRect.y() + sourceView.y() - sourceBounds.y();

            localParamInfo.srcRowStart   = dab.device->constData() + dabX * srcPixelSize + dabY * dabRowStride;
            localParamInfo.srcRowStride  = dabRowStride;
            if (!localParamInfo.dstRowStart || !localParamInfo.srcRowStart) return false;
            colorSpace->bitBlt(srcColorSpace, localParamInfo, operation, renderingIntent, conversionFlags);

            dstX += columns;
            columnsRemaining -= columns;
        }

        dstY += rows;
        rowsRemaining -= rows;
    }
    return true;
}

bool KisPainter::Private::applyDeviceWithSelection(const QRect &applyRect,
                                                   const KisRenderedDab &dab,
                                                   KisPixelWriteCursor *dstIt,
                                                   KisRandomConstAccessorSP maskIt,
                                                   const KoColorSpace *srcColorSpace,
                                                   const KoCompositeOp *operation,
                                                   KoCompositeOp::ParameterInfo &localParamInfo,
                                                   bool useDabParameters,
                                                   const QRect &sourceRect)
{
    const QRect sourceBounds = dab.device->bounds();
    const QRect sourceView = sourceRect.isNull() ? sourceBounds : sourceRect;
    const QRect dabRect(dab.offset, sourceView.size());
    const QRect rc = applyRect & dabRect;
    if (rc.isEmpty()) return true;

    const int srcPixelSize = srcColorSpace->pixelSize();
    const int dabRowStride = srcPixelSize * sourceBounds.width();
    if (useDabParameters) {
        localParamInfo.setOpacityAndAverage(dab.opacity, dab.averageOpacity);
        localParamInfo.flow = dab.flow;
    }


    qint32 dstY = rc.y();
    qint32 rowsRemaining = rc.height();

    while (rowsRemaining > 0) {
        qint32 dstX = rc.x();

        qint32 numContiguousDstRows = dstIt->numContiguousRows(dstY);
        qint32 numContiguousMaskRows = maskIt->numContiguousRows(dstY);
        qint32 rows = qMin(rowsRemaining, qMin(numContiguousDstRows, numContiguousMaskRows));

        if (rows <= 0) return false;
        qint32 columnsRemaining = rc.width();

        while (columnsRemaining > 0) {

            qint32 numContiguousDstColumns = dstIt->numContiguousColumns(dstX);
            qint32 numContiguousMaskColumns = maskIt->numContiguousColumns(dstX);
            qint32 columns = qMin(columnsRemaining, qMin(numContiguousDstColumns, numContiguousMaskColumns));

            if (columns <= 0) return false;
            qint32 dstRowStride = dstIt->rowStride(dstX, dstY);
            qint32 maskRowStride = maskIt->rowStride(dstX, dstY);
            dstIt->moveTo(dstX, dstY);
            maskIt->moveTo(dstX, dstY);

            localParamInfo.dstRowStart   = dstIt->rawData();
            localParamInfo.dstRowStride  = dstRowStride;
            localParamInfo.maskRowStart  = maskIt->rawDataConst();
            if (!localParamInfo.maskRowStart) return false;
            localParamInfo.maskRowStride = maskRowStride;
            localParamInfo.rows          = rows;
            localParamInfo.cols          = columns;


            const int dabX = dstX - dabRect.x() + sourceView.x() - sourceBounds.x();
            const int dabY = dstY - dabRect.y() + sourceView.y() - sourceBounds.y();

            localParamInfo.srcRowStart   = dab.device->constData() + dabX * srcPixelSize + dabY * dabRowStride;
            localParamInfo.srcRowStride  = dabRowStride;
            if (!localParamInfo.dstRowStart || !localParamInfo.srcRowStart) return false;
            colorSpace->bitBlt(srcColorSpace, localParamInfo, operation, renderingIntent, conversionFlags);

            dstX += columns;
            columnsRemaining -= columns;
        }

        dstY += rows;
        rowsRemaining -= rows;
    }
    return true;
}

void KisPainter::bltFixed(const QRect &applyRect, const QList<KisRenderedDab> allSrcDevices)
{
    bltFixedImpl(QVector<QRect>{applyRect}, allSrcDevices, d->strokeMutationOwner);
}

void KisPainter::bltFixed(const QVector<QRect> &applyRects,
                          const QList<KisRenderedDab> &allSrcDevices)
{
    bltFixedImpl(applyRects, allSrcDevices, d->strokeMutationOwner);
}

void KisPainter::bltFixedImpl(const QVector<QRect> &applyRects,
                              const QList<KisRenderedDab> &allSrcDevices,
                              KisTransaction *strokeOwner)
{
    if (KisStrokeJobFailureContext::currentJobHasFailed()) return;
    const KoColorSpace *srcColorSpace = 0;
    QList<KisRenderedDab> devices;
    QVector<QRect> clips;
    const QRect selectionBounds = d->selection ? d->selection->selectedRect() : QRect();
    for (QRect rc : applyRects) {
        if (d->selection) rc &= selectionBounds;
        if (!rc.isEmpty()) clips.append(rc);
    }

    QVector<QRect> targetRects;

    Q_FOREACH (const KisRenderedDab &dab, allSrcDevices) {
        bool intersects = false;
        for (const QRect &rc : std::as_const(clips)) {
            const QRect target = rc & dab.realBounds();
            if (!target.isEmpty()) {
                intersects = true;
                targetRects.append(target);
            }
        }
        if (intersects) devices.append(dab);

        if (!srcColorSpace) {
            srcColorSpace = dab.device->colorSpace();
        } else {
            KIS_SAFE_ASSERT_RECOVER_RETURN(*srcColorSpace == *dab.device->colorSpace());
        }
    }

    if (devices.isEmpty()) return;

    KoCompositeOp::ParameterInfo localParamInfo = d->paramInfo;
    // This painter is shared by non-overlapping brush jobs. Resolve once into
    // this operation's local state, without writing the painter-wide cache.
    const KoCompositeOp *operation = d->colorSpace->compositeOp(d->compositeOpId, srcColorSpace);
    KisRandomConstAccessorSP maskIt = d->selection ? d->selection->projection()->createRandomConstAccessorNG() : nullptr;
    const bool painted = d->device->applyPixelOperation(targetRects, [&](KisPixelWriteCursor *dstIt) {
        for (const KisRenderedDab &dab : std::as_const(devices)) {
            for (const QRect &rc : std::as_const(clips)) {
                const bool accepted = maskIt
                    ? d->applyDeviceWithSelection(rc, dab, dstIt, maskIt, srcColorSpace, operation, localParamInfo)
                    : d->applyDevice(rc, dab, dstIt, srcColorSpace, operation, localParamInfo);
                if (!accepted) return false;
            }
        }
        return true;
    }, strokeOwner);
    if (!painted) {
        KisStrokeJobFailureContext::reportFailure(QStringLiteral("Painter multi-dab pixel operation failed"));
    }
}

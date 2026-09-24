/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageStoreRandomAccessor.h"

#include <limits>

namespace {

qint64 floorDiv(qint64 value, qint32 divisor)
{
    qint64 quotient = value / divisor;
    if (value % divisor < 0) --quotient;
    return quotient;
}

qint32 positiveRemainder(qint64 value, qint32 divisor)
{
    const qint64 remainder = value % divisor;
    return qint32(remainder < 0 ? remainder + divisor : remainder);
}

quint32 alignedRowStride(const KisSurfaceEpochState &surface)
{
    const quint64 bytes = quint64(surface.logicalPageExtent.width()) *
                          surface.format.pixelStride;
    const quint64 alignment = qMax<quint64>(
        surface.rowAlignment, surface.format.pixelAlignment);
    const quint64 aligned = (bytes + alignment - 1) & ~(alignment - 1);
    return aligned <= std::numeric_limits<quint32>::max()
        ? quint32(aligned) : 0;
}

}

class KisPageStoreRandomAccessor::Private
{
public:
    bool initializeSurface(KisPageStore *pageStore,
                           KisSurfaceId requestedSurface,
                           const KisPageReadView &view,
                           bool capturedSession = false)
    {
        store = pageStore;
        surface = requestedSurface;
        if (!store || !surface.isValid()) {
            failure = QStringLiteral("PageStore random accessor identity is invalid");
            return false;
        }
        if (capturedSession) session.resolveReadSurfaceState(&surfaceState);
        else store->resolveSurfaceState(surface, view, &surfaceState);
        if (!surfaceState.isValid()) {
            failure = QStringLiteral("PageStore surface metadata is unavailable");
            return false;
        }
        pageWidth = surfaceState.logicalPageExtent.width();
        pageHeight = surfaceState.logicalPageExtent.height();
        pixelStride = qint32(surfaceState.format.pixelStride);
        fallbackRowStride = qint32(alignedRowStride(surfaceState));
        if (pageWidth <= 0 || pageHeight <= 0 || pixelStride <= 0 ||
            fallbackRowStride <= 0) {
            failure = QStringLiteral("PageStore surface layout is invalid");
            return false;
        }
        return true;
    }

    void clearPointers()
    {
        data = nullptr;
        oldData = nullptr;
        currentRowStride = 0;
        currentColumn = 0;
        currentRow = 0;
        hasPosition = false;
    }

    KisPageStore *store = nullptr;
    KisSurfaceId surface;
    KisSurfaceEpochState surfaceState;
    KisPageStoreCpuAccessSession session{KisCpuAccessLifetime::CursorPage};
    KisCpuPageReadSpan readSpan;
    KisCpuPageWriteSpan writeSpan;
    bool hasCachedPage = false;
    qint32 cachedColumn = 0, cachedRow = 0;
    bool writable = false;
    bool active = false;
    qint32 pageWidth = 0;
    qint32 pageHeight = 0;
    qint32 pixelStride = 0;
    qint32 fallbackRowStride = 0;
    qint32 offsetX = 0;
    qint32 offsetY = 0;
    qint32 positionX = 0;
    qint32 positionY = 0;
    qint32 currentColumn = 0;
    qint32 currentRow = 0;
    qint32 currentRowStride = 0;
    quint8 *data = nullptr;
    const quint8 *oldData = nullptr;
    bool hasPosition = false;
    QString failure;
};

KisPageStoreRandomAccessor::KisPageStoreRandomAccessor(
    KisPageStore *store,
    KisSurfaceId surface,
    const KisPageReadView &view,
    qint32 offsetX,
    qint32 offsetY,
    KisPagePriority priority)
    : d(new Private)
{
    d->offsetX = offsetX;
    d->offsetY = offsetY;
    d->active = d->session.beginRead(store, surface, view, priority, &d->failure) &&
                d->initializeSurface(store, surface, view, true);
    if (!d->active) d->session.cancel();
}

KisPageStoreRandomAccessor::KisPageStoreRandomAccessor(
    KisPageStore *store,
    KisSurfaceId surface,
    const KisPageTransaction &transaction,
    KisPageWriteMode mode,
    qint32 offsetX,
    qint32 offsetY,
    KisPagePriority priority)
    : d(new Private)
{
    d->writable = true;
    d->offsetX = offsetX;
    d->offsetY = offsetY;
    KisPageReadView overlay;
    overlay.kind = KisPageReadViewKind::TransactionOverlay;
    overlay.transaction = transaction.id;
    d->active = d->session.beginWrite(store, surface, transaction, mode, priority, &d->failure) &&
                d->initializeSurface(store, surface, overlay);
    if (!d->active) d->session.cancel();
}

KisPageStoreRandomAccessor::~KisPageStoreRandomAccessor()
{
    if (d->active) {
        if (d->writable) {
            d->session.finish(&d->failure);
        } else {
            d->session.finish();
        }
    }
}

bool KisPageStoreRandomAccessor::isValid() const
{
    return d->active && d->session.isActive();
}

QString KisPageStoreRandomAccessor::error() const
{
    return d->failure;
}

KisSurfaceEpochState KisPageStoreRandomAccessor::surfaceState() const
{
    return isValid() ? d->surfaceState : KisSurfaceEpochState{};
}

bool KisPageStoreRandomAccessor::finish(QString *error)
{
    if (!d->active) {
        if (error) *error = d->failure;
        return false;
    }
    const bool finished = d->session.finish(&d->failure);
    d->active = false;
    d->clearPointers();
    if (error) *error = d->failure;
    return finished;
}

void KisPageStoreRandomAccessor::cancel()
{
    if (d->active) d->session.cancel();
    d->active = false;
    d->clearPointers();
}

void KisPageStoreRandomAccessor::moveTo(qint32 x, qint32 y)
{
    d->positionX = x;
    d->positionY = y;
    d->clearPointers();
    if (!d->active) return;

    const qint64 localX = qint64(x) - d->offsetX;
    const qint64 localY = qint64(y) - d->offsetY;
    const qint64 column64 = floorDiv(localX, d->pageWidth);
    const qint64 row64 = floorDiv(localY, d->pageHeight);
    if (column64 < std::numeric_limits<qint32>::min() ||
        column64 > std::numeric_limits<qint32>::max() ||
        row64 < std::numeric_limits<qint32>::min() ||
        row64 > std::numeric_limits<qint32>::max()) {
        d->failure = QStringLiteral(
            "PageStore accessor coordinate exceeds logical page identity");
        return;
    }
    const qint32 column = qint32(column64);
    const qint32 row = qint32(row64);
    const qint32 xInPage = positiveRemainder(localX, d->pageWidth);
    const qint32 yInPage = positiveRemainder(localY, d->pageHeight);

    if (!d->hasCachedPage || d->cachedColumn != column || d->cachedRow != row) {
        // Calling the cursor on a new page invalidates the previous spans.
        // Never retain a stale cached pointer after an acquisition failure.
        d->hasCachedPage = false; d->readSpan = {}; d->writeSpan = {};
        if (d->writable) d->writeSpan = d->session.writePage(column, row, &d->failure);
        else d->readSpan = d->session.readPage(column, row, &d->failure);
        if (d->writable ? !d->writeSpan.isValid() : !d->readSpan.isValid()) return;
        d->cachedColumn = column; d->cachedRow = row; d->hasCachedPage = true;
    }
    if (d->writable) {
        const auto &span = d->writeSpan;
        if (!span.isValid()) return;
        const quint64 offset = quint64(yInPage) * span.rowStride +
                               quint64(xInPage) * quint64(d->pixelStride);
        if (offset + quint64(d->pixelStride) > span.byteSize) {
            d->failure = QStringLiteral("PageStore writable pixel offset is outside the lease");
            return;
        }
        d->data = span.data + offset;
        d->oldData = span.oldData + offset;
        d->currentRowStride = qint32(span.rowStride);
    } else {
        const auto &span = d->readSpan;
        if (!span.isValid()) return;
        const quint64 offset = quint64(yInPage) * span.rowStride +
                               quint64(xInPage) * quint64(d->pixelStride);
        if (offset + quint64(d->pixelStride) > span.byteSize) {
            d->failure = QStringLiteral("PageStore readable pixel offset is outside the lease");
            return;
        }
        d->data = nullptr;
        d->oldData = span.data + offset;
        d->currentRowStride = qint32(span.rowStride);
    }
    d->currentColumn = column;
    d->currentRow = row;
    d->hasPosition = true;
    d->failure.clear();
}

quint8 *KisPageStoreRandomAccessor::rawData()
{
    return d->writable ? d->data : nullptr;
}

const quint8 *KisPageStoreRandomAccessor::oldRawData() const
{
    return d->oldData;
}

const quint8 *KisPageStoreRandomAccessor::rawDataConst() const
{
    return d->writable ? d->data : d->oldData;
}

qint32 KisPageStoreRandomAccessor::numContiguousColumns(qint32 x) const
{
    return d->pageWidth > 0
        ? d->pageWidth - positiveRemainder(qint64(x) - d->offsetX,
                                           d->pageWidth)
        : 0;
}

qint32 KisPageStoreRandomAccessor::numContiguousRows(qint32 y) const
{
    return d->pageHeight > 0
        ? d->pageHeight - positiveRemainder(qint64(y) - d->offsetY,
                                            d->pageHeight)
        : 0;
}

qint32 KisPageStoreRandomAccessor::rowStride(qint32 x, qint32 y) const
{
    const qint64 localX = qint64(x) - d->offsetX;
    const qint64 localY = qint64(y) - d->offsetY;
    if (d->hasPosition && floorDiv(localX, d->pageWidth) == d->currentColumn &&
        floorDiv(localY, d->pageHeight) == d->currentRow) {
        return d->currentRowStride;
    }
    return d->fallbackRowStride;
}

qint32 KisPageStoreRandomAccessor::x() const
{
    return d->positionX;
}

qint32 KisPageStoreRandomAccessor::y() const
{
    return d->positionY;
}

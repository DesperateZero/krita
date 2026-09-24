/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageStoreCpuSurfaceOps.h"

#include <cstring>
#include <limits>

namespace {

qint64 floorDiv(qint64 value, qint32 divisor)
{
    qint64 quotient = value / divisor;
    if (value % divisor < 0) --quotient;
    return quotient;
}

bool checkedBufferLayout(const QRect &rect,
                         quint32 pixelStride,
                         qsizetype requestedRowStride,
                         qsizetype availableBytes,
                         qsizetype *rowStride,
                         qsizetype *requiredBytes)
{
    if (rect.isEmpty() || pixelStride == 0) return false;
    const quint64 tightRow = quint64(rect.width()) * pixelStride;
    const quint64 stride = requestedRowStride > 0
        ? quint64(requestedRowStride) : tightRow;
    if (stride < tightRow ||
        quint64(rect.height()) >
            quint64(std::numeric_limits<qsizetype>::max()) / stride) {
        return false;
    }
    const quint64 required = stride * quint64(rect.height());
    if (required > quint64(std::numeric_limits<qsizetype>::max()) ||
        (availableBytes >= 0 && required > quint64(availableBytes))) {
        return false;
    }
    *rowStride = qsizetype(stride);
    *requiredBytes = qsizetype(required);
    return true;
}

bool sameStorageFormat(KisSurfaceFormat lhs, KisSurfaceFormat rhs)
{
    lhs.defaultPixel.clear();
    rhs.defaultPixel.clear();
    return lhs == rhs;
}

// A cursor owns one page's pointers. Visit all rows of that page before
// advancing, otherwise a row-major rectangle would re-pin every page once
// per scanline. Wide counters also avoid overflow at the coordinate limits.
template<typename CopyBlock>
bool visitPageBlocks(KisPageStoreRandomAccessor &accessor, const QRect &rect,
                     CopyBlock copyBlock, QString *error)
{
    for (qint64 y = rect.top(); y <= rect.bottom();) {
        const qint32 rows = qint32(qMin<qint64>(
            accessor.numContiguousRows(qint32(y)), qint64(rect.bottom()) - y + 1));
        for (qint64 x = rect.left(); x <= rect.right();) {
            const qint32 columns = qint32(qMin<qint64>(
                accessor.numContiguousColumns(qint32(x)), qint64(rect.right()) - x + 1));
            accessor.moveTo(qint32(x), qint32(y));
            if (rows <= 0 || columns <= 0 || !accessor.rawDataConst()) {
                KisPageStoreDetail::setError(error, accessor.error());
                accessor.cancel();
                return false;
            }
            copyBlock(qsizetype(x - rect.left()), qsizetype(y - rect.top()),
                      columns, rows, accessor.rowStride(qint32(x), qint32(y)));
            x += columns;
        }
        y += rows;
    }
    return true;
}

bool readCapturedRect(
    KisPageStoreRandomAccessor &accessor,
    const QRect &rect,
    QByteArray *bytes,
    qsizetype destinationRowStride,
    QString *error)
{
    const KisSurfaceEpochState state = accessor.surfaceState();
    qsizetype rowStride = 0;
    qsizetype requiredBytes = 0;
    if (!bytes || !accessor.isValid() || !state.isValid() ||
        !checkedBufferLayout(rect, state.format.pixelStride,
                             destinationRowStride, -1,
                             &rowStride, &requiredBytes)) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore read rectangle is invalid"));
        return false;
    }
    QByteArray result(requiredBytes, char(0));
    const qsizetype pixelStride = qsizetype(state.format.pixelStride);
    if (!visitPageBlocks(accessor, rect,
        [&](qsizetype x, qsizetype y, qint32 columns, qint32 rows, qsizetype pageStride) {
            for (qint32 row = 0; row < rows; ++row)
                std::memcpy(result.data() + (y + row) * rowStride + x * pixelStride,
                            accessor.rawDataConst() + row * pageStride,
                            size_t(qsizetype(columns) * pixelStride));
        }, error)) return false;
    if (!accessor.finish(error)) return false;
    *bytes = result;
    KisPageStoreDetail::setError(error, {});
    return true;
}

}

bool KisPageStoreCpuSurfaceOps::removePage(KisPageStore *store,
                                           const KisPageTransaction &transaction,
                                           const KisPageKey &key,
                                           QString *error)
{
    auto mutation = store ? store->beginMutation(transaction, error) : KisPageMutationSession{};
    if (mutation.isActive() && mutation.removePage(key, error) && mutation.seal(error))
        return true;
    mutation.cancel();
    return false;
}

bool KisPageStoreCpuSurfaceOps::readRect(
    KisPageStore *store, KisSurfaceId surface, const KisPageReadView &view,
    const QRect &rect, QByteArray *bytes, qsizetype destinationRowStride, QString *error)
{
    KisPageStoreRandomAccessor accessor(store, surface, view);
    return readCapturedRect(accessor, rect, bytes, destinationRowStride, error);
}

bool KisPageStoreCpuSurfaceOps::writeRect(
    KisPageStore *store,
    KisSurfaceId surface,
    const KisPageTransaction &transaction,
    const QRect &rect,
    const QByteArray &bytes,
    qsizetype sourceRowStride,
    QString *error)
{
    KisPageStoreRandomAccessor accessor(
        store, surface, transaction, KisPageWriteMode::PreserveContents);
    const auto state = accessor.surfaceState();
    qsizetype rowStride = 0;
    qsizetype requiredBytes = 0;
    if (!accessor.isValid() || !state.isValid() ||
        !checkedBufferLayout(rect, state.format.pixelStride, sourceRowStride,
                             bytes.size(), &rowStride, &requiredBytes)) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore write rectangle is invalid"));
        accessor.cancel();
        return false;
    }
    Q_UNUSED(requiredBytes);
    const qsizetype pixelStride = qsizetype(state.format.pixelStride);
    if (!visitPageBlocks(accessor, rect,
        [&](qsizetype x, qsizetype y, qint32 columns, qint32 rows, qsizetype pageStride) {
            for (qint32 row = 0; row < rows; ++row)
                std::memcpy(accessor.rawData() + row * pageStride,
                            bytes.constData() + (y + row) * rowStride + x * pixelStride,
                            size_t(qsizetype(columns) * pixelStride));
        }, error)) return false;
    return accessor.finish(error);
}

bool KisPageStoreCpuSurfaceOps::fillRect(
    KisPageStore *store,
    KisSurfaceId surface,
    const KisPageTransaction &transaction,
    const QRect &rect,
    const QByteArray &pixel,
    QString *error)
{
    KisPageReadView overlay;
    overlay.kind = KisPageReadViewKind::TransactionOverlay;
    overlay.transaction = transaction.id;
    KisSurfaceEpochState state;
    if (!store || !store->resolveSurfaceState(surface, overlay, &state) ||
        rect.isEmpty() || pixel.size() != qsizetype(state.format.pixelStride)) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore fill rectangle is invalid"));
        return false;
    }

    KisPageStoreRandomAccessor accessor(
        store, surface, transaction, KisPageWriteMode::PreserveContents);
    if (!accessor.isValid()) {
        KisPageStoreDetail::setError(error, accessor.error());
        return false;
    }
    const qint32 pageWidth = state.logicalPageExtent.width();
    const qint32 pageHeight = state.logicalPageExtent.height();
    const qint64 firstColumn = floorDiv(rect.left(), pageWidth);
    const qint64 lastColumn = floorDiv(rect.right(), pageWidth);
    const qint64 firstRow = floorDiv(rect.top(), pageHeight);
    const qint64 lastRow = floorDiv(rect.bottom(), pageHeight);
    if (firstColumn < std::numeric_limits<qint32>::min() ||
        lastColumn > std::numeric_limits<qint32>::max() ||
        firstRow < std::numeric_limits<qint32>::min() ||
        lastRow > std::numeric_limits<qint32>::max()) {
        accessor.cancel();
        KisPageStoreDetail::setError(error, QStringLiteral(
            "PageStore fill rectangle exceeds logical page identity"));
        return false;
    }

    for (qint64 row = firstRow; row <= lastRow; ++row) {
        for (qint64 column = firstColumn; column <= lastColumn; ++column) {
            const QRect pageRect(
                qint32(column * pageWidth), qint32(row * pageHeight),
                pageWidth, pageHeight);
            const QRect affected = pageRect.intersected(rect);
            if (affected == pageRect &&
                pixel == state.format.defaultPixel) {
                if (!removePage(store, transaction,
                                {surface, {qint32(column), qint32(row)}}, error)) {
                    accessor.cancel();
                    return false;
                }
                continue;
            }
            for (qint32 y = affected.top();; ++y) {
                accessor.moveTo(affected.left(), y);
                quint8 *destination = accessor.rawData();
                if (!destination) {
                    accessor.cancel();
                    KisPageStoreDetail::setError(error, accessor.error());
                    return false;
                }
                for (qint32 x = 0; x < affected.width(); ++x) {
                    std::memcpy(destination +
                                    qsizetype(x) * state.format.pixelStride,
                                pixel.constData(), size_t(pixel.size()));
                }
                if (y == affected.bottom()) break;
            }
        }
    }
    return accessor.finish(error);
}

bool KisPageStoreCpuSurfaceOps::copyRect(
    KisPageStore *store,
    KisSurfaceId sourceSurface,
    const KisPageReadView &sourceView,
    const QRect &sourceRect,
    KisSurfaceId destinationSurface,
    const KisPageTransaction &destinationTransaction,
    const QPoint &destinationTopLeft,
    QString *error)
{
    KisPageReadView destinationView;
    destinationView.kind = KisPageReadViewKind::TransactionOverlay;
    destinationView.transaction = destinationTransaction.id;
    KisPageStoreRandomAccessor sourceAccessor(store, sourceSurface, sourceView);
    const KisSurfaceEpochState sourceState = sourceAccessor.surfaceState();
    KisSurfaceEpochState destinationState;
    const QRect destinationRect(destinationTopLeft, sourceRect.size());
    if (!store || sourceRect.isEmpty() || destinationRect.isEmpty() ||
        !sourceAccessor.isValid() || !sourceState.isValid() ||
        !store->resolveSurfaceState(destinationSurface, destinationView,
                                    &destinationState) ||
        !sameStorageFormat(sourceState.format, destinationState.format)) {
        KisPageStoreDetail::setError(error, QStringLiteral(
            "PageStore copy surfaces are invalid or incompatible"));
        return false;
    }
    QByteArray bytes;
    if (!readCapturedRect(sourceAccessor, sourceRect, &bytes, 0, error)) {
        return false;
    }
    return writeRect(store, destinationSurface, destinationTransaction,
                     destinationRect, bytes, 0, error);
}

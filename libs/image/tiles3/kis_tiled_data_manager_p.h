/*
 *  SPDX-FileCopyrightText: 2004 C. Boemann <cbo@boemann.dk>
 *  SPDX-FileCopyrightText: 2009 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */



#include "pagestore/KisTiledDataManagerPageStoreBackend.h"
#include "pagestore/KisPageStoreIteratorReadScope_p.h"
#include "pagestore/KisPageStoreDiagnostics_p.h"
#include <QScopeGuard>
#include <limits>
#include <optional>

/* FIXME: Think over SSE here */
void KisTiledDataManager::writeBytesBody(const quint8 *data,
                                         qint32 x, qint32 y,
                                         qint32 width, qint32 height,
                                         qint32 dataRowStride)
{
    if (!data) return;

    width  = width < 0  ? 0 : width;
    height = height < 0 ? 0 : height;

    qint32 dataY = 0;
    qint32 imageY = y;
    qint32 rowsRemaining = height;
    const qint32 pixelSize = this->pixelSize();

    if (dataRowStride <= 0) {
        dataRowStride = pixelSize * width;
    }

    while (rowsRemaining > 0) {

        qint32 dataX = 0;
        qint32 imageX = x;
        qint32 columnsRemaining = width;
        qint32 numContiguousImageRows = numContiguousRows(imageY, imageX,
                                                          imageX + width - 1);

        qint32 rowsToWork = qMin(numContiguousImageRows, rowsRemaining);

        while (columnsRemaining > 0) {

            qint32 numContiguousImageColumns =
                    numContiguousColumns(imageX, imageY,
                                         imageY + rowsToWork - 1);

            qint32 columnsToWork = qMin(numContiguousImageColumns,
                                        columnsRemaining);
            const qint32 tileRowStride = rowStride(imageX, imageY);
            const quint8 *dataIt = data +
                    dataX * pixelSize + dataY * dataRowStride;
            const qint32 lineSize = columnsToWork * pixelSize;

            // PageStore generations are semantic changes, not write-call
            // counters.  Avoid allocating/publishing a new generation when
            // the caller writes bytes already present in the tile.  The data
            // manager write lock protects this compare-and-write decision;
            // the tile read lock also preserves the normal swap lifetime.
            bool unchanged = true;
            {
                KisTileDataWrapper readWrapper(
                    this, imageX, imageY, KisTileDataWrapper::READ);
                const quint8 *tileIt = readWrapper.data();
                const quint8 *compareIt = dataIt;
                for (qint32 row = 0; row < rowsToWork; ++row) {
                    if (memcmp(tileIt, compareIt, lineSize) != 0) {
                        unchanged = false;
                        break;
                    }
                    tileIt += tileRowStride;
                    compareIt += dataRowStride;
                }
            }

            if (unchanged) {
                imageX += columnsToWork;
                dataX += columnsToWork;
                columnsRemaining -= columnsToWork;
                continue;
            }

            KisTileDataWrapper tw(this, imageX, imageY,
                                  KisTileDataWrapper::WRITE);
            quint8 *tileIt = tw.tile()->tryWriteData();
            if (!tileIt) return; // batch cancellation restores all pending pages
            tileIt += tw.offset();

            for (qint32 row = 0; row < rowsToWork; row++) {
                memcpy(tileIt, dataIt, lineSize);
                tileIt += tileRowStride;
                dataIt += dataRowStride;
            }

            imageX += columnsToWork;
            dataX += columnsToWork;
            columnsRemaining -= columnsToWork;
        }

        imageY += rowsToWork;
        dataY += rowsToWork;
        rowsRemaining -= rowsToWork;
    }
}


void KisTiledDataManager::readBytesBody(quint8 *data,
                                        qint32 x, qint32 y,
                                        qint32 width, qint32 height,
                                        qint32 dataRowStride) const
{
    if (!data) return;

    width  = width < 0  ? 0 : width;
    height = height < 0 ? 0 : height;

    qint32 dataY = 0;
    qint32 imageY = y;
    qint32 rowsRemaining = height;
    const qint32 pixelSize = this->pixelSize();

    if (dataRowStride <= 0) {
        dataRowStride = pixelSize * width;
    }

    while (rowsRemaining > 0) {

        qint32 dataX = 0;
        qint32 imageX = x;
        qint32 columnsRemaining = width;
        qint32 numContiguousImageRows = numContiguousRows(imageY, imageX,
                                                          imageX + width - 1);

        qint32 rowsToWork = qMin(numContiguousImageRows, rowsRemaining);

        while (columnsRemaining > 0) {

            qint32 numContiguousImageColumns = numContiguousColumns(imageX, imageY,
                                                                    imageY + rowsToWork - 1);

            qint32 columnsToWork = qMin(numContiguousImageColumns,
                                        columnsRemaining);

            // XXX: Ugly const cast because of the old pixelPtr design copied from tiles1.
            KisTileDataWrapper tw(const_cast<KisTiledDataManager*>(this), imageX, imageY, KisTileDataWrapper::READ);
            quint8 *tileIt = tw.data();


            const qint32 tileRowStride = rowStride(imageX, imageY);

            quint8 *dataIt = data +
                    dataX * pixelSize + dataY * dataRowStride;

            const qint32 lineSize = columnsToWork * pixelSize;

            for (qint32 row = 0; row < rowsToWork; row++) {
                memcpy(dataIt, tileIt, lineSize);
                tileIt += tileRowStride;
                dataIt += dataRowStride;
            }

            imageX += columnsToWork;
            dataX += columnsToWork;
            columnsRemaining -= columnsToWork;
        }

        imageY += rowsToWork;
        dataY += rowsToWork;
        rowsRemaining -= rowsToWork;
    }
}


#define forEachChannel(_idx, _channelSize)                              \
    for(qint32 _idx=0, _channelSize=channelSizes[_idx];         \
    _idx<numChannels && (_channelSize=channelSizes[_idx], 1);   \
    _idx++)

template <bool allChannelsPresent>
bool KisTiledDataManager::writePlanarBytesBody(QVector </*const*/ quint8* > planes,
                                               QVector<qint32> channelSizes,
                                               qint32 x, qint32 y,
                                               qint32 width, qint32 height)
{
    Q_ASSERT(planes.size() == channelSizes.size());
    Q_ASSERT(planes.size() > 0);

    width  = width < 0  ? 0 : width;
    height = height < 0 ? 0 : height;

    const qint32 numChannels = planes.size();
    const qint32 pixelSize = this->pixelSize();

    qint32 dataY = 0;
    qint32 imageY = y;
    qint32 rowsRemaining = height;

    while (rowsRemaining > 0) {

        qint32 dataX = 0;
        qint32 imageX = x;
        qint32 columnsRemaining = width;
        qint32 numContiguousImageRows = numContiguousRows(imageY, imageX,
                                                          qint32(qint64(imageX) + width - 1));

        qint32 rowsToWork = qMin(numContiguousImageRows, rowsRemaining);

        while (columnsRemaining > 0) {

            qint32 numContiguousImageColumns =
                    numContiguousColumns(imageX, imageY,
                                         qint32(qint64(imageY) + rowsToWork - 1));
            qint32 columnsToWork = qMin(numContiguousImageColumns,
                                        columnsRemaining);

            const qsizetype dataIdx = dataX + qsizetype(dataY) * width;
            const qsizetype tileRowStride = rowStride(imageX, imageY);

            KisTileDataWrapper tw(this, imageX, imageY,
                                  KisTileDataWrapper::WRITE);
            quint8 *tileItStart = tw.tile()->tryWriteData();
            if (!tileItStart) return false;
            tileItStart += tw.offset();


            forEachChannel(i, channelSize) {
                if (channelSize > 0 && (allChannelsPresent || planes[i])) {
                    const quint8* planeStart = planes[i] + dataIdx * channelSize;
                    const qsizetype dataStride = qsizetype(width) * channelSize;
                    for (qint32 row = 0; row < rowsToWork; row++) {
                        const quint8 *planeRow = planeStart + qsizetype(row) * dataStride;
                        quint8 *tileRow = tileItStart + qsizetype(row) * tileRowStride;
                        for (int col = 0; col < columnsToWork; col++) {
                            memcpy(tileRow + qsizetype(col) * pixelSize,
                                   planeRow + qsizetype(col) * channelSize, channelSize);
                        }
                    }
                    // Do not advance either cursor after the final row/pixel:
                    // an offset channel or partial tile can be beyond one-past.
                }

                tileItStart += channelSize;
            }

            dataX += columnsToWork;
            columnsRemaining -= columnsToWork;
            if (columnsRemaining) imageX += columnsToWork;
        }

        dataY += rowsToWork;
        rowsRemaining -= rowsToWork;
        if (rowsRemaining) imageY += rowsToWork;
    }
    return true;
}

QVector<quint8*> KisTiledDataManager::readPlanarBytesBody(QVector<qint32> channelSizes,
                                                          qint32 x, qint32 y,
                                                          qint32 width, qint32 height) const
{
    Q_ASSERT(channelSizes.size() > 0);

    width  = width < 0  ? 0 : width;
    height = height < 0 ? 0 : height;

    const qint32 numChannels = channelSizes.size();
    const qint32 pixelSize = this->pixelSize();

    QVector<quint8*> planes;
    qint64 channelBytes = 0;
    for (auto size : channelSizes) {
        KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(size >= 0, QVector<quint8 *>());
        channelBytes += size;
        KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(
            quint64(width) * quint64(height) <= quint64(std::numeric_limits<qsizetype>::max()) / qMax(1, size), QVector<quint8 *>());
    }
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(!channelSizes.isEmpty() && channelBytes <= pixelSize, QVector<quint8 *>());
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(qint64(x) + width - 1 <= std::numeric_limits<qint32>::max() &&
                                       qint64(y) + height - 1 <= std::numeric_limits<qint32>::max(), QVector<quint8 *>());
    auto cleanup = qScopeGuard([&] { for (auto plane : planes) delete[] plane; });
    forEachChannel(i, channelSize) {
        planes.append(new quint8[qsizetype(width) * height * channelSize]);
    }
    if (!width || !height) { cleanup.dismiss(); return planes; }

    const bool nativeRead = m_pageStoreBackend && !m_pageStoreBackend->hasCurrentThreadIteratorWrites();
    auto *store = nativeRead ? m_pageStoreBackend->store() : nullptr;
    KisPageStoreDiagnosticTimer diagnostic(store, KisPageStoreDiagnosticPhase::ReadPlanarCapture, 1);
    KisCapturedReadView view;
    if (nativeRead) {
        view = m_pageStoreBackend->captureReadView();
        KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(view.isValid(), QVector<quint8 *>());
    }

    qint32 dataY = 0;
    qint32 imageY = y;
    qint32 rowsRemaining = height;

    while (rowsRemaining > 0) {

        qint32 dataX = 0;
        qint32 imageX = x;
        qint32 columnsRemaining = width;
        qint32 numContiguousImageRows = numContiguousRows(imageY, imageX,
                                                          imageX + width - 1);

        qint32 rowsToWork = qMin(numContiguousImageRows, rowsRemaining);

        while (columnsRemaining > 0) {

            qint32 numContiguousImageColumns =
                    numContiguousColumns(imageX, imageY,
                                         imageY + rowsToWork - 1);
            qint32 columnsToWork = qMin(numContiguousImageColumns,
                                        columnsRemaining);

            const qsizetype dataIdx = dataX + qsizetype(dataY) * width;
            diagnostic.next(KisPageStoreDiagnosticPhase::ReadPlanarPage, 1);
            KisPageStoreReadPage page;
            std::optional<KisTileDataWrapper> legacy;
            const quint8 *tileItStart = nullptr;
            qint32 tileRowStride = 0;
            if (nativeRead) {
                const qint32 column = xToCol(imageX), row = yToRow(imageY);
                page = KisPageStoreReadPage(store, view, {m_pageStoreBackend->surface(), {column, row}});
                KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(page.data(), QVector<quint8 *>());
                tileRowStride = page.rowStride();
                tileItStart = page.data() + (qint64(imageY) - qint64(row) * KisTileData::HEIGHT) * tileRowStride +
                    (qint64(imageX) - qint64(column) * KisTileData::WIDTH) * pixelSize;
            } else {
                legacy.emplace(const_cast<KisTiledDataManager *>(this), imageX, imageY, KisTileDataWrapper::READ);
                tileItStart = legacy->data();
                tileRowStride = rowStride(imageX, imageY);
            }
            forEachChannel(i, channelSize) {
                for (qint32 row = 0; row < rowsToWork; row++) {
                    const quint8 *tileIt = tileItStart + qsizetype(row) * tileRowStride;
                    quint8 *planeIt = planes[i] + (dataIdx + qsizetype(row) * width) * channelSize;
                    for (int col = 0; col < columnsToWork; col++) {
                        memcpy(planeIt + qsizetype(col) * channelSize,
                               tileIt + qsizetype(col) * pixelSize, channelSize);
                    }
                }
                tileItStart += channelSize;
            }

            dataX += columnsToWork;
            columnsRemaining -= columnsToWork;
            if (columnsRemaining) imageX += columnsToWork;
        }


        dataY += rowsToWork;
        rowsRemaining -= rowsToWork;
        if (rowsRemaining) imageY += rowsToWork;
    }
    diagnostic.next(KisPageStoreDiagnosticPhase::ReadPlanarRelease, 1);
    cleanup.dismiss();
    return planes;
}

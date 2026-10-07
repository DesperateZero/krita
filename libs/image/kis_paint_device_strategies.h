/*
 *  SPDX-FileCopyrightText: 2013 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __KIS_PAINT_DEVICE_STRATEGIES_H
#define __KIS_PAINT_DEVICE_STRATEGIES_H

#include "kis_wrapped_rect.h"
#include "kis_wrapped_hline_iterator.h"
#include "kis_wrapped_vline_iterator.h"
#include "kis_wrapped_random_accessor.h"
#include <limits>

// Coordinate decoration only. Native pixel ownership stays in the operation
// cursor; this adapter never acquires a compatibility tile or retains a pointer.
class KisOperationMappedAccessor final : public KisPixelWriteCursor
{
public:
    KisOperationMappedAccessor(KisPixelWriteCursor *cursor, QPoint offset,
                               QRect wrap, WrapAroundAxis axis)
        : m_cursor(cursor), m_offset(offset), m_wrap(wrap), m_axis(axis) {}
    qint64 mapX(qint32 x) const {
        qint64 value = x;
        if (!m_wrap.isEmpty() && m_axis != WRAPAROUND_VERTICAL) {
            value = (value - m_wrap.x()) % m_wrap.width(); if (value < 0) value += m_wrap.width();
        }
        return value - m_offset.x();
    }
    qint64 mapY(qint32 y) const {
        qint64 value = y;
        if (!m_wrap.isEmpty() && m_axis != WRAPAROUND_HORIZONTAL) {
            value = (value - m_wrap.y()) % m_wrap.height(); if (value < 0) value += m_wrap.height();
        }
        return value - m_offset.y();
    }
    static bool fits(qint64 value) {
        return value >= std::numeric_limits<qint32>::min() && value <= std::numeric_limits<qint32>::max();
    }
    qint64 columnsUntilWrap(qint32 x) const {
        return m_wrap.isEmpty() || m_axis == WRAPAROUND_VERTICAL ? std::numeric_limits<qint32>::max() :
            qint64(m_wrap.right()) + 1 - (mapX(x) + m_offset.x());
    }
    qint64 rowsUntilWrap(qint32 y) const {
        return m_wrap.isEmpty() || m_axis == WRAPAROUND_HORIZONTAL ? std::numeric_limits<qint32>::max() :
            qint64(m_wrap.bottom()) + 1 - (mapY(y) + m_offset.y());
    }
    bool isValid() const { return !m_failed; }
    void moveTo(qint32 x, qint32 y) override {
        m_position = {x,y};
        const auto localX = mapX(x), localY = mapY(y);
        if (m_failed || !fits(localX) || !fits(localY)) { m_failed = true; return; }
        m_cursor->moveTo(qint32(localX), qint32(localY));
    }
    quint8 *rawData() override { return m_failed ? nullptr : m_cursor->rawData(); }
    qint32 numContiguousColumns(qint32 x) const override {
        if (!fits(mapX(x))) return 0;
        const auto n = m_cursor->numContiguousColumns(qint32(mapX(x)));
        return qint32(qMin<qint64>(n, columnsUntilWrap(x)));
    }
    qint32 numContiguousRows(qint32 y) const override {
        if (!fits(mapY(y))) return 0;
        const auto n = m_cursor->numContiguousRows(qint32(mapY(y)));
        return qint32(qMin<qint64>(n, rowsUntilWrap(y)));
    }
    qint32 rowStride(qint32 x, qint32 y) const override {
        return fits(mapX(x)) && fits(mapY(y)) ? m_cursor->rowStride(qint32(mapX(x)), qint32(mapY(y))) : 0;
    }
    qint32 x() const override { return m_position.x(); }
    qint32 y() const override { return m_position.y(); }
private:
    KisPixelWriteCursor *m_cursor;
    QPoint m_offset, m_position;
    QRect m_wrap;
    WrapAroundAxis m_axis;
    bool m_failed = false;
};


class KisPaintDevice::Private::KisPaintDeviceStrategy
{
public:
    KisPaintDeviceStrategy(KisPaintDevice *device,
                           KisPaintDevice::Private *d)
        : m_device(device), m_d(d)
    {
    }

    virtual ~KisPaintDeviceStrategy() {
    }

    virtual void move(const QPoint& pt) {
        m_d->setX(pt.x());
        m_d->setY(pt.y());
        m_d->cache()->invalidate();
    }

    virtual QRect extent() const {
        QRect extent;

        qint32 x, y, w, h;
        m_d->dataManager()->extent(x, y, w, h);
        x += m_d->x();
        y += m_d->y();
        extent = QRect(x, y, w, h);

        quint8 defaultOpacity = m_device->defaultPixel().opacityU8();

        if (defaultOpacity != OPACITY_TRANSPARENT_U8)
            extent |= m_d->defaultBounds->bounds();

        return extent;
    }

    virtual KisRegion region() const {
        return m_d->cache()->region().translated(m_d->x(), m_d->y());
    }

    virtual void crop(const QRect &rect) {
        m_d->dataManager()->setExtent(rect.translated(-m_d->x(), -m_d->y()));
        m_d->cache()->invalidate();
    }

    virtual void clear(const QRect & rc) {
        KisDataManagerSP dm = m_d->dataManager();

        dm->clear(rc.x() - m_d->x(), rc.y() - m_d->y(),
                  rc.width(), rc.height(),
                  dm->defaultPixel());
        m_d->cache()->invalidate();
    }

    virtual void fill(const QRect &rc, const quint8 *fillPixel) {
        m_d->dataManager()->clear(rc.x() - m_d->x(),
                                  rc.y() - m_d->y(),
                                  rc.width(),
                                  rc.height(),
                                  fillPixel);
        m_d->cache()->invalidate();
    }


    virtual KisHLineIteratorSP createHLineIteratorNG(KisDataManager *dataManager, qint32 x, qint32 y, qint32 w, qint32 offsetX, qint32 offsetY) {
        return new KisHLineIterator2(dataManager, x, y, w, offsetX, offsetY, true, m_d->cacheInvalidator());
    }

    virtual KisHLineConstIteratorSP createHLineConstIteratorNG(KisDataManager *dataManager, qint32 x, qint32 y, qint32 w, qint32 offsetX, qint32 offsetY) const {
        return new KisHLineIterator2(dataManager, x, y, w, offsetX, offsetY, false, m_d->cacheInvalidator());
    }


    virtual KisVLineIteratorSP createVLineIteratorNG(qint32 x, qint32 y, qint32 w) {
        m_d->cache()->invalidate();
        return new KisVLineIterator2(m_d->dataManager().data(), x, y, w, m_d->x(), m_d->y(), true, m_d->cacheInvalidator());
    }

    virtual KisVLineConstIteratorSP createVLineConstIteratorNG(qint32 x, qint32 y, qint32 w) const {
        return new KisVLineIterator2(m_d->dataManager().data(), x, y, w, m_d->x(), m_d->y(), false, m_d->cacheInvalidator());
    }

    virtual KisRandomAccessorSP createRandomAccessorNG() {
        m_d->cache()->invalidate();
        return new KisRandomAccessor2(m_d->dataManager().data(), m_d->x(), m_d->y(), true, m_d->cacheInvalidator());
    }

    virtual KisRandomConstAccessorSP createRandomConstAccessorNG() const {
        return new KisRandomAccessor2(m_d->dataManager().data(), m_d->x(), m_d->y(), false, m_d->cacheInvalidator());
    }

    virtual bool applyPixelOperation(const QVector<QRect> &rects, const KisPageStorePixelOperation &operation,
                                     KisTransaction *strokeOwner) {
        return applyPixelOperationImpl(rects, operation, {}, strokeOwner);
    }

    virtual bool partitionWriteRects(const QVector<QRect> &rects, int patchSize,
                                     QVector<QVector<QRect>> *jobs) {
        return partitionWriteRectsImpl(rects, patchSize, {}, jobs);
    }

    virtual void fastBitBlt(KisPaintDeviceSP src, const QRect &rect) {
        Q_ASSERT(m_device->fastBitBltPossible(src));
        fastBitBltImpl(src->dataManager(), rect);
    }

    virtual void fastBitBltOldData(KisPaintDeviceSP src, const QRect &rect) {
        Q_ASSERT(m_device->fastBitBltPossible(src));

        m_d->dataManager()->bitBltOldData(src->dataManager(), rect.translated(-m_d->x(), -m_d->y()));
        m_d->cache()->invalidate();
    }

    virtual void fastBitBltRough(KisPaintDeviceSP src, const QRect &rect) {
        Q_ASSERT(m_device->fastBitBltPossible(src));
        fastBitBltRoughImpl(src->dataManager(), rect);
    }

    virtual void fastBitBltRough(KisDataManagerSP srcDataManager, const QRect &rect) {
        fastBitBltRoughImpl(srcDataManager, rect);
    }

    virtual void fastBitBltRoughOldData(KisPaintDeviceSP src, const QRect &rect) {
        Q_ASSERT(m_device->fastBitBltPossible(src));

        m_d->dataManager()->bitBltRoughOldData(src->dataManager(), rect.translated(-m_d->x(), -m_d->y()));
        m_d->cache()->invalidate();
    }

    virtual void readBytes(quint8 *data, const QRect &rect) const {
        readBytesImpl(data, rect, -1);
    }

    virtual void writeBytes(const quint8 * data, const QRect &rect) {
        writeBytesImpl(data, rect, -1);
    }

    virtual QVector<quint8*> readPlanarBytes(qint32 x, qint32 y, qint32 w, qint32 h) const {
        return m_d->dataManager()->readPlanarBytes(m_device->channelSizes(), x, y, w, h);
    }

    virtual void writePlanarBytes(QVector<quint8*> planes, qint32 x, qint32 y, qint32 w, qint32 h) {
        m_d->dataManager()->writePlanarBytes(planes, m_device->channelSizes(), x, y, w, h);
        m_d->cache()->invalidate();
    }
protected:
    bool partitionWriteRectsImpl(const QVector<QRect> &rects, int patchSize, const QRect &wrap,
                                 QVector<QVector<QRect>> *jobs) {
        // Use the cursor's mapping and seam boundaries. A mapped patch owns all
        // its source-coordinate clips, including both sides of a wrap seam.
        // Neither this derived geometry nor its temporary index grants access.
        KisOperationMappedAccessor mapped(nullptr, QPoint(m_d->x(), m_d->y()), wrap,
                                          m_device->defaultBounds()->wrapAroundModeAxis());
        QRegion coverage;
        for (const QRect &rect : rects) if (!rect.isEmpty()) coverage += rect;
        QHash<QPair<int, int>, int> patchIndices;
        QVector<QRegion> patches;
        const auto remainder = [patchSize](qint64 value) {
            const qint64 r = value % patchSize;
            return r < 0 ? r + patchSize : r;
        };
        for (const QRect &rect : coverage) {
            for (qint64 y = rect.top(); y <= rect.bottom();) {
                const qint64 localY = mapped.mapY(qint32(y));
                if (!mapped.fits(localY)) return false;
                const qint64 ry = remainder(localY);
                const qint64 height = std::min({qint64(rect.bottom()) + 1 - y,
                                               patchSize - ry, mapped.rowsUntilWrap(qint32(y))});
                if (height <= 0 || !mapped.fits(localY + height - 1)) return false;
                for (qint64 x = rect.left(); x <= rect.right();) {
                    const qint64 localX = mapped.mapX(qint32(x));
                    if (!mapped.fits(localX)) return false;
                    const qint64 rx = remainder(localX);
                    const qint64 width = std::min({qint64(rect.right()) + 1 - x,
                                                  patchSize - rx, mapped.columnsUntilWrap(qint32(x))});
                    if (width <= 0 || !mapped.fits(localX + width - 1)) return false;
                    const QPair<int, int> key(int((localX - rx) / patchSize), int((localY - ry) / patchSize));
                    auto it = patchIndices.find(key);
                    if (it == patchIndices.end()) {
                        it = patchIndices.insert(key, patches.size());
                        patches.append(QRegion());
                    }
                    patches[*it] += QRect(qint32(x), qint32(y), qint32(width), qint32(height));
                    x += width;
                }
                y += height;
            }
        }
        QVector<QVector<QRect>> result;
        result.reserve(patches.size());
        for (const QRegion &patch : patches) {
            QVector<QRect> clips;
            clips.reserve(patch.rectCount());
            for (const QRect &rect : patch) clips.append(rect);
            result.append(clips);
        }
        jobs->swap(result);
        return true;
    }

    bool applyPixelOperationImpl(const QVector<QRect> &rects, const KisPageStorePixelOperation &operation,
                                 const QRect &wrap, KisTransaction *strokeOwner) {
        const QPoint offset(m_d->x(), m_d->y());
        QVector<QRect> localRects;
        for (const auto &rect : rects) if (!rect.isEmpty()) {
            const qint64 x = qint64(rect.x()) - offset.x(), y = qint64(rect.y()) - offset.y();
            if (!KisOperationMappedAccessor::fits(x) || !KisOperationMappedAccessor::fits(y) ||
                !KisOperationMappedAccessor::fits(x + rect.width() - 1) ||
                !KisOperationMappedAccessor::fits(y + rect.height() - 1)) return false;
            localRects.append(QRect(qint32(x), qint32(y), rect.width(), rect.height()));
        }
        // Retain the selected animation/LOD manager for the synchronous
        // callback and cache update. Device metadata must not be mutated here.
        auto manager = m_d->dataManager();
        QString error;
        const auto mappedOperation = [&](KisPixelWriteCursor *cursor) {
            KisOperationMappedAccessor mapped(cursor, offset, wrap, m_device->defaultBounds()->wrapAroundModeAxis());
            const bool accepted = operation(&mapped);
            return accepted && mapped.isValid();
        };
        if (strokeOwner) {
            return strokeOwner->applyStrokePixelOperation(KisPaintDeviceSP(m_device), localRects,
                                                         mappedOperation, &error);
        }
        const auto result = manager->writePageStoreOperation(localRects, mappedOperation, &error);
        using Result = KisPageStoreWriteOperationResult;
        if (result == Result::Succeeded) return true;
        if (result != Result::Unavailable && result != Result::Borrowed) {
            qWarning() << "PageStore pixel operation failed:" << error;
            return false;
        }
        // Borrowed/unsupported is decided before callback execution. Keep the
        // legacy visibility contract; never execute this branch after failure.
        auto cursor = createRandomAccessorNG();
        class LegacyDestination final : public KisPixelWriteCursor {
        public:
            explicit LegacyDestination(KisRandomAccessorNG *cursor) : cursor(cursor) {}
            void moveTo(qint32 x, qint32 y) override { cursor->moveTo(x,y); }
            quint8 *rawData() override { return cursor->rawData(); }
            qint32 numContiguousColumns(qint32 x) const override { return cursor->numContiguousColumns(x); }
            qint32 numContiguousRows(qint32 y) const override { return cursor->numContiguousRows(y); }
            qint32 rowStride(qint32 x, qint32 y) const override { return cursor->rowStride(x,y); }
            qint32 x() const override { return cursor->x(); }
            qint32 y() const override { return cursor->y(); }
            KisRandomAccessorNG *cursor;
        } destination(cursor.data());
        return operation(&destination);
    }
    virtual void readBytesImpl(quint8 *data, const QRect &rect, int dataRowStride) const {
        m_d->dataManager()->readBytes(data,
                                      rect.x() - m_d->x(),
                                      rect.y() - m_d->y(),
                                      rect.width(),
                                      rect.height(),
                                      dataRowStride);
    }

    virtual void writeBytesImpl(const quint8 * data, const QRect &rect, int dataRowStride) {
        m_d->dataManager()->writeBytes(data,
                                       rect.x() - m_d->x(),
                                       rect.y() - m_d->y(),
                                       rect.width(),
                                       rect.height(),
                                       dataRowStride);
        m_d->cache()->invalidate();
    }

    virtual void fastBitBltImpl(KisDataManagerSP srcDataManager, const QRect &rect)
    {
        m_d->dataManager()->bitBlt(srcDataManager, rect.translated(-m_d->x(), -m_d->y()));
        m_d->cache()->invalidate();
    }

    virtual void fastBitBltRoughImpl(KisDataManagerSP srcDataManager, const QRect &rect)
    {
        m_d->dataManager()->bitBltRough(srcDataManager, rect.translated(-m_d->x(), -m_d->y()));
        m_d->cache()->invalidate();
    }

protected:
    KisPaintDevice *m_device;
    KisPaintDevice::Private * const m_d;
};


class KisPaintDevice::Private::KisPaintDeviceWrappedStrategy : public KisPaintDeviceStrategy
{
public:
    KisPaintDeviceWrappedStrategy(const QRect &wrapRect,
                                  KisPaintDevice *device,
                                  KisPaintDevice::Private *d)
        : KisPaintDeviceStrategy(device, d),
          m_wrapRect(wrapRect)
    {
    }

    const QRect wrapRect() const {
        return m_wrapRect;
    }

    void setWrapRect(const QRect &rc) {
        m_wrapRect = rc;
    }

    void move(const QPoint& pt) override {
        QPoint offset (pt.x() - m_device->x(), pt.y() - m_device->y());

        QRect exactBoundsBeforeMove = m_device->exactBounds();
        KisPaintDeviceStrategy::move(pt);

        QRegion borderRegion(exactBoundsBeforeMove.translated(offset.x(), offset.y()));
        borderRegion -= m_wrapRect;

        const int pixelSize = m_device->pixelSize();

        auto rectIter = borderRegion.begin();
        while (rectIter != borderRegion.end()) {
            QRect rc = *rectIter;
            KisRandomConstAccessorSP srcIt = KisPaintDeviceStrategy::createRandomConstAccessorNG();
            KisRandomAccessorSP dstIt = createRandomAccessorNG();

            int rows = 1;
            int columns = 1;

            for (int y = rc.y(); y <= rc.bottom(); y += rows) {
                int rows = qMin(srcIt->numContiguousRows(y), dstIt->numContiguousRows(y));
                rows = qMin(rows, rc.bottom() - y + 1);

                for (int x = rc.x(); x <= rc.right(); x += columns) {
                    int columns = qMin(srcIt->numContiguousColumns(x), dstIt->numContiguousColumns(x));
                    columns = qMin(columns, rc.right() - x + 1);

                    srcIt->moveTo(x, y);
                    dstIt->moveTo(x, y);

                    int srcRowStride = srcIt->rowStride(x, y);
                    int dstRowStride = dstIt->rowStride(x, y);
                    const quint8 *srcPtr = srcIt->rawDataConst();
                    quint8 *dstPtr = dstIt->rawData();

                    for (int i = 0; i < rows; i++) {
                        memcpy(dstPtr, srcPtr, pixelSize * columns);
                        srcPtr += srcRowStride;
                        dstPtr += dstRowStride;
                    }
                }
            }
            rectIter++;
        }
    }

    QRect extent() const override {
        return KisWrappedRect::clipToWrapRect(KisPaintDeviceStrategy::extent(),
            m_wrapRect, m_device->defaultBounds()->wrapAroundModeAxis());
    }

    KisRegion region() const override {
        const WrapAroundAxis wrapAxis = m_device->defaultBounds()->wrapAroundModeAxis();
        if (wrapAxis != WRAPAROUND_BOTH) {
            KisRegion region = KisPaintDeviceStrategy::region();
            QVector<QRect> rects;
            Q_FOREACH (const QRect &rc, region.rects()) {
                const QRect clippedRect = KisWrappedRect::clipToWrapRect(rc, m_wrapRect, wrapAxis);
                if (!clippedRect.isEmpty()) {
                    rects.append(clippedRect);
                }
            }
            region = KisRegion(rects);
            return region;
        }
        else {
            return KisPaintDeviceStrategy::region() & m_wrapRect;
        }
    }

    void crop(const QRect &rect) override {
        KisPaintDeviceStrategy::crop(rect & m_wrapRect);
    }

    void clear(const QRect &rect) override {
        KisWrappedRect splitRect(rect, m_wrapRect, m_device->defaultBounds()->wrapAroundModeAxis());
        Q_FOREACH (const QRect &rc, splitRect) {
            KisPaintDeviceStrategy::clear(rc);
        }
    }

    void fill(const QRect &rect, const quint8 *fillPixel) override {
        KisWrappedRect splitRect(rect, m_wrapRect, m_device->defaultBounds()->wrapAroundModeAxis());
        Q_FOREACH (const QRect &rc, splitRect) {
            KisPaintDeviceStrategy::fill(rc, fillPixel);
        }
    }

    KisHLineIteratorSP createHLineIteratorNG(KisDataManager *dataManager, qint32 x, qint32 y, qint32 w, qint32 offsetX, qint32 offsetY) override {
        KisWrappedRect splitRect(QRect(x, y, w, m_wrapRect.height()), m_wrapRect, m_device->defaultBounds()->wrapAroundModeAxis());
        if (!splitRect.isSplit()) {
            return KisPaintDeviceStrategy::createHLineIteratorNG(dataManager, x, y, w, offsetX, offsetY);
        }
        return new KisWrappedHLineIterator(dataManager, splitRect, offsetX, offsetY, true, m_d->cacheInvalidator());
    }

    KisHLineConstIteratorSP createHLineConstIteratorNG(KisDataManager *dataManager, qint32 x, qint32 y, qint32 w, qint32 offsetX, qint32 offsetY) const override {
        KisWrappedRect splitRect(QRect(x, y, w, m_wrapRect.height()), m_wrapRect, m_device->defaultBounds()->wrapAroundModeAxis());
        if (!splitRect.isSplit()) {
            return KisPaintDeviceStrategy::createHLineConstIteratorNG(dataManager, x, y, w, offsetX, offsetY);
        }
        return new KisWrappedHLineIterator(dataManager, splitRect, offsetX, offsetY, false, m_d->cacheInvalidator());
    }

    KisVLineIteratorSP createVLineIteratorNG(qint32 x, qint32 y, qint32 h) override {
        m_d->cache()->invalidate();

        KisWrappedRect splitRect(QRect(x, y, m_wrapRect.width(), h), m_wrapRect, m_device->defaultBounds()->wrapAroundModeAxis());
        if (!splitRect.isSplit()) {
            return KisPaintDeviceStrategy::createVLineIteratorNG(x, y, h);
        }
        return new KisWrappedVLineIterator(m_d->dataManager().data(), splitRect, m_d->x(), m_d->y(), true, m_d->cacheInvalidator());
    }

    KisVLineConstIteratorSP createVLineConstIteratorNG(qint32 x, qint32 y, qint32 h) const override {
        KisWrappedRect splitRect(QRect(x, y, m_wrapRect.width(), h), m_wrapRect, m_device->defaultBounds()->wrapAroundModeAxis());
        if (!splitRect.isSplit()) {
            return KisPaintDeviceStrategy::createVLineConstIteratorNG(x, y, h);
        }
        return new KisWrappedVLineIterator(m_d->dataManager().data(), splitRect, m_d->x(), m_d->y(), false, m_d->cacheInvalidator());
    }

    KisRandomAccessorSP createRandomAccessorNG() override {
        m_d->cache()->invalidate();
        return new KisWrappedRandomAccessor(
            m_d->dataManager().data(), m_d->x(), m_d->y(), true, m_d->cacheInvalidator(), m_wrapRect,
            m_device->defaultBounds()->wrapAroundModeAxis());
    }

    KisRandomConstAccessorSP createRandomConstAccessorNG() const override {
        return new KisWrappedRandomAccessor(
            m_d->dataManager().data(), m_d->x(), m_d->y(), false, m_d->cacheInvalidator(), m_wrapRect,
            m_device->defaultBounds()->wrapAroundModeAxis());
    }

    bool applyPixelOperation(const QVector<QRect> &rects, const KisPageStorePixelOperation &operation,
                             KisTransaction *strokeOwner) override {
        QVector<QRect> parts;
        for (const QRect &rect : rects) {
            if (!rect.isEmpty()) parts += KisWrappedRect(rect, m_wrapRect, m_device->defaultBounds()->wrapAroundModeAxis());
        }
        return applyPixelOperationImpl(parts, operation, m_wrapRect, strokeOwner);
    }

    bool partitionWriteRects(const QVector<QRect> &rects, int patchSize,
                             QVector<QVector<QRect>> *jobs) override {
        return partitionWriteRectsImpl(rects, patchSize, m_wrapRect, jobs);
    }

    void fastBitBltImpl(KisDataManagerSP srcDataManager, const QRect &rect) override {
        KisWrappedRect splitRect(rect, m_wrapRect, m_device->defaultBounds()->wrapAroundModeAxis());
        Q_FOREACH (const QRect &rc, splitRect) {
            KisPaintDeviceStrategy::fastBitBltImpl(srcDataManager, rc);
        }
    }

    void fastBitBltOldData(KisPaintDeviceSP src, const QRect &rect) override {
        KisWrappedRect splitRect(rect, m_wrapRect, m_device->defaultBounds()->wrapAroundModeAxis());
        Q_FOREACH (const QRect &rc, splitRect) {
            KisPaintDeviceStrategy::fastBitBltOldData(src, rc);
        }
    }

    void fastBitBltRoughImpl(KisDataManagerSP srcDataManager, const QRect &rect) override
    {
        // no rough version in wrapped mode
        fastBitBltImpl(srcDataManager, rect);
    }

    void fastBitBltRoughOldData(KisPaintDeviceSP src, const QRect &rect) override {
        // no rough version in wrapped mode
        fastBitBltOldData(src, rect);
    }

    void readBytes(quint8 *data, const QRect &rect) const override {
        KisWrappedRect splitRect(rect, m_wrapRect, m_device->defaultBounds()->wrapAroundModeAxis());

        if (!splitRect.isSplit()) {
            KisPaintDeviceStrategy::readBytes(data, rect);
        } else {
            const int pixelSize = m_device->pixelSize();

            int leftWidth = splitRect[KisWrappedRect::TOPLEFT].width();
            int rightWidth = splitRect[KisWrappedRect::TOPRIGHT].width();

            int totalHeight = rect.height();
            int totalWidth = rect.width();
            int dataRowStride = totalWidth * pixelSize;

            int bufOffset = 0;
            int row = 0;
            while (row < totalHeight) {
                int leftIndex = KisWrappedRect::TOPLEFT + bufOffset;
                int rightIndex = KisWrappedRect::TOPRIGHT + bufOffset;

                QPoint leftRectOrigin = splitRect[leftIndex].topLeft();
                QPoint rightRectOrigin = splitRect[rightIndex].topLeft();

                int height = qMin(splitRect[leftIndex].height(), totalHeight - row);

                int col = 0;
                while (col < totalWidth) {
                    int width;
                    quint8 *dataPtr;

                    width = qMin(leftWidth, totalWidth - col);
                    dataPtr = data + pixelSize * (col + row * totalWidth);
                    readBytesImpl(dataPtr, QRect(leftRectOrigin, QSize(width, height)), dataRowStride);
                    col += width;

                    if (col >= totalWidth) break;

                    width = qMin(rightWidth, totalWidth - col);
                    dataPtr = data + pixelSize * (col + row * totalWidth);
                    readBytesImpl(dataPtr, QRect(rightRectOrigin, QSize(width, height)), dataRowStride);
                    col += width;
                }

                row += height;
                bufOffset = (bufOffset + 2) % 4;
            }
        }
    }

    void writeBytes(const quint8 *data, const QRect &rect) override {
        KisWrappedRect splitRect(rect, m_wrapRect, m_device->defaultBounds()->wrapAroundModeAxis());

        if (!splitRect.isSplit()) {
            KisPaintDeviceStrategy::writeBytes(data, rect);
        } else {
            const int pixelSize = m_device->pixelSize();

            int totalWidth = rect.width();
            int dataRowStride = totalWidth * pixelSize;

            QRect rc;
            QPoint origin;
            const quint8 *dataPtr;

            origin.rx() = 0;
            origin.ry() = 0;
            rc = splitRect.topLeft();
            dataPtr = data + pixelSize * (origin.x() + totalWidth * origin.y());
            writeBytesImpl(dataPtr, rc, dataRowStride);

            origin.rx() = splitRect.topLeft().width();
            origin.ry() = 0;
            rc = splitRect.topRight();
            dataPtr = data + pixelSize * (origin.x() + totalWidth * origin.y());
            writeBytesImpl(dataPtr, rc, dataRowStride);

            origin.rx() = 0;
            origin.ry() = splitRect.topLeft().height();
            rc = splitRect.bottomLeft();
            dataPtr = data + pixelSize * (origin.x() + totalWidth * origin.y());
            writeBytesImpl(dataPtr, rc, dataRowStride);

            origin.rx() = splitRect.topLeft().width();
            origin.ry() = splitRect.topLeft().height();
            rc = splitRect.bottomRight();
            dataPtr = data + pixelSize * (origin.x() + totalWidth * origin.y());
            writeBytesImpl(dataPtr, rc, dataRowStride);
        }
    }

private:
    QRect m_wrapRect;
};



#endif /* __KIS_PAINT_DEVICE_STRATEGIES_H */

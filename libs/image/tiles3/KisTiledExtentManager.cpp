/*
 *  SPDX-FileCopyrightText: 2017 Dmitry Kazakov <dimula73@gmail.com>
 *  SPDX-FileCopyrightText: 2018 Andrey Kamakin <a.kamakin@icloud.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisTiledExtentManager.h"

#include <QVector>
#include <utility>
#include <limits>
#include <new>
#include <algorithm>
#include "kis_tile_data_interface.h"
#include "kis_assert.h"
#include "kis_global.h"
#include "kis_debug.h"
#include "pagestore/KisMutationStorage_p.h"

KisTiledExtentManager::Data::Data()
    : m_min(qint32_MAX), m_max(qint32_MIN), m_count(0)
{
    QWriteLocker lock(&m_migrationLock);
    m_capacity = 0;
    m_offset = 1;
}

KisTiledExtentManager::Data::~Data()
{
    QWriteLocker lock(&m_migrationLock);
    m_buffer.reset();
}

bool KisTiledExtentManager::Data::add(qint32 index)
{
    QReadLocker lock(&m_migrationLock);
    qint64 currentIndex = qint64(m_offset) + index;

    if (currentIndex < 0 || currentIndex >= m_capacity) {
        lock.unlock();
        prepare(index, index);
        lock.relock();
        currentIndex = qint64(m_offset) + index;
    }

    KIS_ASSERT_RECOVER_NOOP(m_buffer[currentIndex].loadAcquire() >= 0);
    bool needsUpdateExtent = false;

    while (true) {
        QReadLocker rl(&m_extentLock);

        int oldValue = m_buffer[currentIndex].loadAcquire();
        if (oldValue == 0) {
            rl.unlock();
            QWriteLocker wl(&m_extentLock);

            if ((oldValue = m_buffer[currentIndex].loadAcquire()) == 0) {

                if (m_min > index) m_min = index;
                if (m_max < index) m_max = index;

                ++m_count;
                needsUpdateExtent = true;

                m_buffer[currentIndex].storeRelease(1);
            } else {
                m_buffer[currentIndex].storeRelease(oldValue + 1);
            }

            break;
        } else if (m_buffer[currentIndex].testAndSetOrdered(oldValue, oldValue + 1)) {
            break;
        }
    }

    return needsUpdateExtent;
}

bool KisTiledExtentManager::Data::remove(qint32 index)
{
    QReadLocker lock(&m_migrationLock);
    qint64 currentIndex = qint64(m_offset) + index;

    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(currentIndex >= 0 && currentIndex < m_capacity, false);
    bool needsUpdateExtent = false;
    QReadLocker rl(&m_extentLock);

    const int oldValue = m_buffer[currentIndex].fetchAndAddAcquire(-1);

    /**
     * That is not the droid you're looking for. If you see this assert
     * in the backtrace, most probably, the bug is not here. The crash
     * happens because two threads are trying to do device->clear(rc)
     * concurrently for the overlapping rects. That is, they are trying
     * to remove the same tile. Look higher!
     */
    KIS_SAFE_ASSERT_RECOVER(oldValue > 0) {
        m_buffer[currentIndex].storeRelaxed(0);
        return false;
    }

    if (oldValue == 1) {
        rl.unlock();
        QWriteLocker wl(&m_extentLock);

        if (m_min == index) updateMin();
        if (m_max == index) updateMax();

        --m_count;
        needsUpdateExtent = true;
    }

    return needsUpdateExtent;
}

void KisTiledExtentManager::Data::replace(const QVector<QPoint> &indexes, bool columns)
{
    QWriteLocker lock(&m_migrationLock);
    for (const auto &point : indexes) {
        const auto index = columns ? point.x() : point.y();
        if (!covers(index, index)) throw std::bad_alloc();
    }
    QWriteLocker l(&m_extentLock);

    for (qint32 i = 0; i < m_capacity; ++i) {
        m_buffer[i].storeRelaxed(0);
    }

    m_min = qint32_MAX;
    m_max = qint32_MIN;
    m_count = 0;

    for (const auto &point : indexes) unsafeAdd(columns ? point.x() : point.y());
}

void KisTiledExtentManager::Data::takePrepared(Data &prepared) noexcept
{
    QWriteLocker migration(&m_migrationLock);
    QWriteLocker extent(&m_extentLock);
    using std::swap;
    swap(m_min, prepared.m_min);
    swap(m_max, prepared.m_max);
    swap(m_offset, prepared.m_offset);
    swap(m_capacity, prepared.m_capacity);
    swap(m_count, prepared.m_count);
    swap(m_buffer, prepared.m_buffer);
}

void KisTiledExtentManager::Data::clear()
{
    QWriteLocker lock(&m_migrationLock);
    QWriteLocker l(&m_extentLock);

    for (qint32 i = 0; i < m_capacity; ++i) {
        m_buffer[i].storeRelaxed(0);
    }

    m_min = qint32_MAX;
    m_max = qint32_MIN;
    m_count = 0;
}

bool KisTiledExtentManager::Data::isEmpty()
{
    return m_count == 0;
}

qint32 KisTiledExtentManager::Data::min()
{
    return m_min;
}

qint32 KisTiledExtentManager::Data::max()
{
    return m_max;
}

void KisTiledExtentManager::Data::unsafeAdd(qint32 index)
{
    qint64 currentIndex = qint64(m_offset) + index;

    // replace() prepares and revalidates the complete input before clearing.
    Q_ASSERT(currentIndex >= 0 && currentIndex < m_capacity);

    if (!m_buffer[currentIndex].fetchAndAddRelaxed(1)) {
        if (m_min > index) m_min = index;
        if (m_max < index) m_max = index;
        ++m_count;
    }
}

void KisTiledExtentManager::Data::Growth::allocate()
{
    if (!capacity) return;
    const size_t bytes = size_t(capacity) * sizeof(QAtomicInt);
    const auto &resource = buffer.get_deleter().resource;
    auto *data = static_cast<QAtomicInt *>(resource
        ? resource->allocate(bytes, alignof(QAtomicInt)) : ::operator new(bytes));
    std::uninitialized_value_construct_n(data, size_t(capacity));
    buffer.get_deleter().count = size_t(capacity);
    buffer.reset(data);
}

void KisTiledExtentManager::Data::Growth::DeleteBuffer::operator()(QAtomicInt *data) const noexcept
{
    if (!data) return;
    std::destroy_n(data, count);
    if (resource) resource->deallocate(data, count * sizeof(QAtomicInt), alignof(QAtomicInt));
    else ::operator delete(data);
}

bool KisTiledExtentManager::Data::covers(qint32 first, qint32 last) const
{
    return qint64(m_offset) + first >= 0 && qint64(m_offset) + last < m_capacity;
}

KisTiledExtentManager::Data::Growth KisTiledExtentManager::Data::planGrowth(
    qint32 first, qint32 last) const
{
    Growth growth;
    growth.buffer.get_deleter().resource = m_buffer.get_deleter().resource;
    if (covers(first, last)) return growth;
    qint64 low = std::min(-qint64(m_offset), qint64(first));
    const qint64 high = std::max(qint64(m_capacity) - m_offset - 1, qint64(last));
    const qint64 required = high - low + 1;
    qint64 capacity = std::max(InitialBufferSize, m_capacity);
    while (capacity < required) capacity *= 2;
    // Leave the new slack on the side that grew. Otherwise every small
    // negative-coordinate insertion shifts the full old buffer and doubles
    // capacity again, even when the declared range remains small.
    if (first < -qint64(m_offset))
        low = std::max(-qint64(std::numeric_limits<qint32>::max()), low - (capacity - required));
    if (capacity > std::numeric_limits<qint32>::max() ||
        -low > std::numeric_limits<qint32>::max() ||
        quint64(capacity) > std::numeric_limits<size_t>::max() / sizeof(QAtomicInt))
        throw std::bad_alloc();
    growth.capacity = qint32(capacity);
    growth.offset = qint32(-low);
    return growth;
}

bool KisTiledExtentManager::Data::canInstall(
    const Growth &growth, qint32 first, qint32 last) const
{
    if (covers(first, last)) return true;
    const qint64 start = qint64(growth.offset) - m_offset;
    return growth.buffer && start >= 0 && start + m_capacity <= growth.capacity &&
        qint64(growth.offset) + first >= 0 && qint64(growth.offset) + last < growth.capacity;
}

void KisTiledExtentManager::Data::install(
    Growth &growth, qint32 first, qint32 last) noexcept
{
    if (covers(first, last)) return;
    Q_ASSERT(canInstall(growth, first, last));
    const qint32 start = growth.offset - m_offset;
    for (qint32 i = 0; i < m_capacity; ++i)
        growth.buffer[start + i].storeRelaxed(m_buffer[i].loadRelaxed());
    // Move the old allocation into the caller's private candidate. Its owner
    // destroys it after releasing every migration/extent lock.
    m_buffer.swap(growth.buffer);
    std::swap(m_capacity, growth.capacity);
    std::swap(m_offset, growth.offset);
}

void KisTiledExtentManager::Data::prepare(qint32 first, qint32 last)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        Growth growth;
        {
            QReadLocker lock(&m_migrationLock);
            if (covers(first, last)) return;
            growth = planGrowth(first, last);
        }
        growth.allocate();
        QWriteLocker lock(&m_migrationLock);
        if (canInstall(growth, first, last)) {
            install(growth, first, last);
            return;
        }
    }
    // A concurrent preparer may have installed enough capacity during the
    // final candidate's destruction. This check creates no further storage.
    QReadLocker lock(&m_migrationLock);
    if (!covers(first, last)) throw std::bad_alloc();
}

void KisTiledExtentManager::Data::updateMin()
{
    KIS_SAFE_ASSERT_RECOVER_NOOP(m_min != qint32_MAX);

    qint32 start = m_min + m_offset;

    for (qint32 i = start; i < m_capacity; ++i) {
        qint32 current = m_buffer[i].loadRelaxed();

        if (current > 0) {
            m_min = i - m_offset;
            return;
        }
    }

    m_min = qint32_MAX;
}

void KisTiledExtentManager::Data::updateMax()
{
    KIS_SAFE_ASSERT_RECOVER_NOOP(m_min != qint32_MIN);

    qint32 start = m_max + m_offset;

    for (qint32 i = start; i >= 0; --i) {
        qint32 current = m_buffer[i].loadRelaxed();

        if (current > 0) {
            m_max = i - m_offset;
            return;
        }
    }

    m_max = qint32_MIN;
}

KisTiledExtentManager::KisTiledExtentManager()
{
    QWriteLocker l(&m_extentLock);
    m_currentExtent = QRect();
}

void *KisTiledExtentManager::operator new(size_t bytes)
{ return KisPageProcessStorageObject::operator new(bytes); }
void KisTiledExtentManager::operator delete(void *data) noexcept
{ KisPageProcessStorageObject::operator delete(data); }

bool KisTiledExtentManager::configureStorage(std::shared_ptr<std::pmr::memory_resource> resource)
{
    QWriteLocker cols(&m_colsData.m_migrationLock);
    QWriteLocker rows(&m_rowsData.m_migrationLock);
    if (!resource || m_colsData.m_buffer || m_rowsData.m_buffer || m_colsData.m_count || m_rowsData.m_count) return false;
    m_colsData.m_buffer.get_deleter().resource = resource;
    m_rowsData.m_buffer.get_deleter().resource = std::move(resource);
    return true;
}

bool KisTiledExtentManager::prepareTileRange(const QRect &tileRange) try
{
    if (tileRange.isEmpty()) return true;
    const qint32 left = tileRange.left(), right = tileRange.right();
    const qint32 top = tileRange.top(), bottom = tileRange.bottom();
    for (int attempt = 0; attempt < 3; ++attempt) {
        Data::Growth cols, rows;
        {
            QReadLocker colLock(&m_colsData.m_migrationLock);
            QReadLocker rowLock(&m_rowsData.m_migrationLock);
            if (m_colsData.covers(left, right) && m_rowsData.covers(top, bottom)) return true;
            cols = m_colsData.planGrowth(left, right);
            rows = m_rowsData.planGrowth(top, bottom);
        }
        // Both real allocations must succeed before either active axis changes.
        cols.allocate();
        rows.allocate();
        QWriteLocker colLock(&m_colsData.m_migrationLock);
        QWriteLocker rowLock(&m_rowsData.m_migrationLock);
        if (m_colsData.canInstall(cols, left, right) && m_rowsData.canInstall(rows, top, bottom)) {
            m_colsData.install(cols, left, right);
            m_rowsData.install(rows, top, bottom);
            return true;
        }
    }
    QReadLocker colLock(&m_colsData.m_migrationLock);
    QReadLocker rowLock(&m_rowsData.m_migrationLock);
    return m_colsData.covers(left, right) && m_rowsData.covers(top, bottom);
} catch (const std::bad_alloc &) {
    return false;
}

void KisTiledExtentManager::notifyTileAdded(qint32 col, qint32 row)
{
    // Native operations already prepared their complete range before pixels.
    // Other callers must also prepare both axes before changing either count.
    if (!prepareTileRange(QRect(col, row, 1, 1))) throw std::bad_alloc();
    bool needsUpdateExtent = false;

    needsUpdateExtent |= m_colsData.add(col);
    needsUpdateExtent |= m_rowsData.add(row);

    if (needsUpdateExtent) {
        updateExtent();
    }
}

void KisTiledExtentManager::notifyTileRemoved(qint32 col, qint32 row)
{
    bool needsUpdateExtent = false;

    needsUpdateExtent |= m_colsData.remove(col);
    needsUpdateExtent |= m_rowsData.remove(row);

    if (needsUpdateExtent) {
        updateExtent();
    }
}

void KisTiledExtentManager::replaceTileStats(const QVector<QPoint> &indexes)
{
    if (!indexes.isEmpty()) {
        const auto cols = std::minmax_element(indexes.cbegin(), indexes.cend(),
            [](const QPoint &a, const QPoint &b) { return a.x() < b.x(); });
        const auto rows = std::minmax_element(indexes.cbegin(), indexes.cend(),
            [](const QPoint &a, const QPoint &b) { return a.y() < b.y(); });
        const qint64 width = qint64(cols.second->x()) - cols.first->x() + 1;
        const qint64 height = qint64(rows.second->y()) - rows.first->y() + 1;
        if (width > std::numeric_limits<qint32>::max() || height > std::numeric_limits<qint32>::max() ||
            !prepareTileRange(QRect(cols.first->x(), rows.first->y(), qint32(width), qint32(height))))
            throw std::bad_alloc();
    }
    m_colsData.replace(indexes, true);
    m_rowsData.replace(indexes, false);
    updateExtent();
}

void KisTiledExtentManager::takePrepared(KisTiledExtentManager &prepared) noexcept
{
    Q_ASSERT(this != &prepared);
    m_colsData.takePrepared(prepared.m_colsData);
    m_rowsData.takePrepared(prepared.m_rowsData);
    QWriteLocker lock(&m_extentLock);
    std::swap(m_currentExtent, prepared.m_currentExtent);
}

void KisTiledExtentManager::clear()
{
    m_colsData.clear();
    m_rowsData.clear();

    QWriteLocker lock(&m_extentLock);
    m_currentExtent = QRect();
}

QRect KisTiledExtentManager::extent() const
{
    QReadLocker lock(&m_extentLock);
    return m_currentExtent;
}

void KisTiledExtentManager::updateExtent()
{
    qint32 minX, width, minY, height;

    {
        QReadLocker cl(&m_colsData.m_extentLock);

        if (m_colsData.isEmpty()) {
            minX = 0;
            width = 0;
        } else {
            minX = m_colsData.min() * KisTileData::WIDTH;
            width = (m_colsData.max() + 1) * KisTileData::WIDTH - minX;
        }
    }

    {
        QReadLocker rl(&m_rowsData.m_extentLock);

        if (m_rowsData.isEmpty()) {
            minY = 0;
            height = 0;
        } else {
            minY = m_rowsData.min() * KisTileData::HEIGHT;
            height = (m_rowsData.max() + 1) * KisTileData::HEIGHT - minY;
        }
    }

    QWriteLocker lock(&m_extentLock);
    m_currentExtent = QRect(minX, minY, width, height);
}

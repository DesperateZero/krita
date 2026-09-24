/*
 *  copyright (c) 2006,2010 Cyrille Berger <cberger@cberger.net>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_random_accessor.h"


#include <kis_debug.h>


const quint32 KisRandomAccessor2::CACHESIZE = 4; // Define the number of tiles we keep in cache

KisRandomAccessor2::KisRandomAccessor2(KisTiledDataManager *ktm, qint32 offsetX, qint32 offsetY, bool writable, KisIteratorCompleteListener *completeListener) :
        KisBaseIterator(ktm, writable, completeListener),
        m_tilesCache(nullptr),
        m_tilesCacheSize(0),
        m_data(0),
        m_oldData(0),
        m_lastX(0),
        m_lastY(0),
        m_offsetX(offsetX),
        m_offsetY(offsetY)
{
    Q_ASSERT(ktm != 0);
    if (!m_readCursor) m_tilesCache = new KisTileInfo*[CACHESIZE];
    registerWriteBoundary(this, [](const void *context) {
        return static_cast<const KisRandomAccessor2 *>(context)->m_tilesCacheSize != 0;
    });
}

KisRandomAccessor2::~KisRandomAccessor2()
{
    unregisterWriteBoundary();
    releaseWriteCache();
    delete [] m_tilesCache;
}

void KisRandomAccessor2::releaseWriteCache()
{
    for (uint i = 0; i < m_tilesCacheSize; i++) {
        unlockTile(m_tilesCache[i]->tile);
        unlockOldTile(m_tilesCache[i]->oldtile);
        delete m_tilesCache[i];
    }
    m_tilesCacheSize = 0;
    m_data = nullptr; m_oldData = nullptr;
    m_readScope.clear();
}

void KisRandomAccessor2::moveTo(qint32 x, qint32 y)
{
    m_lastX = x;
    m_lastY = y;

    x -= m_offsetX;
    y -= m_offsetY;

    if (m_readCursor) {
        const qint32 col = xToCol(x), row = yToRow(y);
        const auto pair = m_readCursor->read(col, row);
        m_data = nullptr; m_oldData = nullptr;
        KIS_SAFE_ASSERT_RECOVER_RETURN(pair.isValid());
        const qsizetype localX = qint64(x) - qint64(col) * KisTileData::WIDTH;
        const qsizetype localY = qint64(y) - qint64(row) * KisTileData::HEIGHT;
        m_data = const_cast<quint8 *>(pair.current + localY * pair.currentStride + localX * m_pixelSize);
        m_oldData = pair.before + localY * pair.beforeStride + localX * m_pixelSize;
        return;
    }

    // Look in the cache if the tile if the data is available
    for (uint i = 0; i < m_tilesCacheSize; i++) {
        if (x >= m_tilesCache[i]->area_x1 && x <= m_tilesCache[i]->area_x2 &&
                y >= m_tilesCache[i]->area_y1 && y <= m_tilesCache[i]->area_y2) {
            KisTileInfo* kti = m_tilesCache[i];
            quint32 offset = x - kti->area_x1 + (y - kti->area_y1) * KisTileData::WIDTH;
            offset *= m_pixelSize;
            m_data = kti->data + offset;
            m_oldData = kti->oldData + offset;
            if (i > 0) {
                memmove(m_tilesCache + 1, m_tilesCache, i * sizeof(KisTileInfo*));
                m_tilesCache[0] = kti;
            }
            return;
        }
    }
    // The tile wasn't in cache
    if (m_tilesCacheSize == KisRandomAccessor2::CACHESIZE) { // Remove last element of cache
        unlockTile(m_tilesCache[CACHESIZE-1]->tile);
        unlockOldTile(m_tilesCache[CACHESIZE-1]->oldtile);
        delete m_tilesCache[CACHESIZE-1];
    } else {
        m_tilesCacheSize++;
    }
    quint32 col = xToCol(x);
    quint32 row = yToRow(y);
    KisTileInfo* kti = fetchTileData(col, row);
    quint32 offset = x - kti->area_x1 + (y - kti->area_y1) * KisTileData::WIDTH;
    offset *= m_pixelSize;
    m_data = kti->data + offset;
    m_oldData = kti->oldData + offset;
    memmove(m_tilesCache + 1, m_tilesCache, (KisRandomAccessor2::CACHESIZE - 1) * sizeof(KisTileInfo*));
    m_tilesCache[0] = kti;
}


quint8* KisRandomAccessor2::rawData()
{
    return m_writable ? m_data : nullptr;
}


const quint8* KisRandomAccessor2::oldRawData() const
{
#ifdef DEBUG
    if (!m_dataManager->hasCurrentMemento()) warnTiles << "Accessing oldRawData() when no transaction is in progress.";
#endif
    return m_oldData;
}

const quint8* KisRandomAccessor2::rawDataConst() const
{
    return m_data;
}

KisRandomAccessor2::KisTileInfo* KisRandomAccessor2::fetchTileData(qint32 col, qint32 row)
{
    KisTileInfo* kti = new KisTileInfo;
    if (m_writable && !m_readScope) m_readScope = m_dataManager->capturePageStoreReadScope(true);
    fetchTileDataForCache(*kti, col, row);

    kti->area_x1 = col * KisTileData::HEIGHT;
    kti->area_y1 = row * KisTileData::WIDTH;
    kti->area_x2 = kti->area_x1 + KisTileData::HEIGHT - 1;
    kti->area_y2 = kti->area_y1 + KisTileData::WIDTH - 1;

    return kti;
}

qint32 KisRandomAccessor2::numContiguousColumns(qint32 x) const
{
    return m_dataManager->numContiguousColumns(x - m_offsetX, 0, 0);
}

qint32 KisRandomAccessor2::numContiguousRows(qint32 y) const
{
    return m_dataManager->numContiguousRows(y - m_offsetY, 0, 0);
}

qint32 KisRandomAccessor2::rowStride(qint32 x, qint32 y) const
{
    return m_dataManager->rowStride(x - m_offsetX, y - m_offsetY);
}

qint32 KisRandomAccessor2::x() const
{
    return m_lastX;
}

qint32 KisRandomAccessor2::y() const
{
    return m_lastY;
}

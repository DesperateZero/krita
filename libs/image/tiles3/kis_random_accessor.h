/*
 *  SPDX-FileCopyrightText: 2006,2010 Cyrille Berger <cberger@cberger.net>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_TILED_RANDOM_ACCESSOR_H
#define KIS_TILED_RANDOM_ACCESSOR_H


#include <kis_shared.h>

#include "kis_base_iterator.h"
#include "kis_random_accessor_ng.h"


class KRITAIMAGE_EXPORT KisRandomAccessor2 : public KisRandomAccessorNG, private KisBaseIterator
{

    struct KisTileInfo {
        KisTileSP tile;
        KisTileSP oldtile;
        KisPageStoreReadPage before;
        quint8* data;
        const quint8* oldData;
        qint32 area_x1, area_y1, area_x2, area_y2;
    };

public:

    KisRandomAccessor2(KisTiledDataManager *ktm, qint32 offsetX, qint32 offsetY, bool writable, KisIteratorCompleteListener *completeListener);
    ~KisRandomAccessor2() override;


private:
    KisTileInfo* fetchTileData(qint32 col, qint32 row);
    void releaseWriteCache();

public:
    /// Move to a given x,y position, fetch tiles and data
    void moveTo(qint32 x, qint32 y) override;
    quint8* rawData() override;
    const quint8* oldRawData() const override;
    const quint8* rawDataConst() const override;
    qint32 numContiguousColumns(qint32 x) const override;
    qint32 numContiguousRows(qint32 y) const override;
    qint32 rowStride(qint32 x, qint32 y) const override;
    qint32 x() const override;
    qint32 y() const override;

private:
    KisTileInfo** m_tilesCache;
    quint32 m_tilesCacheSize;
    quint8* m_data;
    const quint8* m_oldData;
    int m_lastX, m_lastY;
    qint32 m_offsetX, m_offsetY;
    static const quint32 CACHESIZE; // Define the number of tiles we keep in cache

};

#endif

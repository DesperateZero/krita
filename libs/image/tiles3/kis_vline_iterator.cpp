/*
 *  SPDX-FileCopyrightText: 2010 Lukáš Tvrdý <lukast.dev@gmail.com>
 *  SPDX-FileCopyrightText: 2010 Cyrille Berger <cberger@cberger.net>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <memory>
#include "kis_vline_iterator.h"

#include <iostream>

KisVLineIterator2::KisVLineIterator2(KisDataManager *dataManager, qint32 x, qint32 y, qint32 h, qint32 offsetX, qint32 offsetY, bool writable, KisIteratorCompleteListener *completeListener,
                                  std::shared_ptr<const KisPageStoreIteratorReadScope> scope)
    : KisBaseIterator(dataManager, writable, completeListener, std::move(scope)),
      m_offsetX(offsetX),
      m_offsetY(offsetY)
{
    if (m_readScope && !m_readScope->isValid()) return;
    x -= m_offsetX;
    y -= m_offsetY;
    Q_ASSERT(dataManager != 0);

    Q_ASSERT(h > 0); // for us, to warn us when abusing the iterators
    if (h < 1) h = 1;  // for release mode, to make sure there's always at least one pixel read.

    m_lineStride = m_pixelSize * KisTileData::WIDTH;

    m_x = x;
    m_y = y;

    m_top = y;
    m_bottom = y + h - 1;

    m_left = m_x;

    m_havePixels = (h == 0) ? false : true;
    if (m_top > m_bottom) {
        m_havePixels = false;
        return;
    }

    m_topRow = yToRow(m_top);
    m_bottomRow = yToRow(m_bottom);

    m_column = xToCol(m_x);
    m_xInTile = calcXInTile(m_x, m_column);

    m_topInTopmostTile = m_top - m_topRow * KisTileData::WIDTH;

    // let's preallocate first row
    if (!populateTileCache(m_tilesCache, m_bottomRow - m_topRow + 1,
                           m_column, m_topRow, 0, 1)) {
        m_havePixels = false;
        return;
    }
    m_index = 0;
    if (!switchToTile(m_topInTopmostTile)) {
        m_havePixels = false;
        return;
    }
}

void KisVLineIterator2::resetPixelPos()
{
    m_y = m_top;

    m_index = 0;
    m_havePixels = switchToTile(m_topInTopmostTile);
}

void KisVLineIterator2::resetColumnPos()
{
    m_x = m_left;

    m_column = xToCol(m_x);
    m_xInTile = calcXInTile(m_x, m_column);
    if (!preallocateTiles()) {
        m_havePixels = false;
        return;
    }

    resetPixelPos();
}

bool KisVLineIterator2::nextPixel()
{
    // We won't increment m_y here as integer can overflow here
    if (m_y >= m_bottom) {
        return m_havePixels = false;
    } else {
        ++m_y;
        if (m_y % KisTileData::HEIGHT != 0) {
            if (m_data) { m_data += m_lineStride; m_oldData += m_lineStride; }
        }
        else {
            // Switching to the beginning of the next tile
            ++m_index;
            if (!switchToTile(0))
                return m_havePixels = false;
        }
    }

    return m_havePixels;
}


void KisVLineIterator2::nextColumn()
{
    m_y = m_top;
    ++m_x;

    if (++m_xInTile < KisTileData::HEIGHT) {
        /* do nothing, usual case */
    } else {
        ++m_column;
        m_xInTile = 0;
        if (!preallocateTiles()) {
            m_havePixels = false;
            return;
        }
    }
    m_index = 0;
    m_havePixels = switchToTile(m_topInTopmostTile);
}


qint32 KisVLineIterator2::nConseqPixels() const
{
    return 1;
}

bool KisVLineIterator2::nextPixels(qint32 n)
{
    Q_ASSERT_X(!(m_y > 0 && (m_y + n) < 0), "vlineIt+=", "Integer overflow");

    qint32 previousRow = yToRow(m_y);
    // We won't increment m_y here first as integer can overflow
    if (m_y >= m_bottom || (m_y += n) > m_bottom) {
        m_havePixels = false;
    } else {
        qint32 row = yToRow(m_y);
        // if we are in the same column in tiles
        if (row == previousRow) {
            if (m_data) { m_data += n * m_lineStride; m_oldData += n * m_lineStride; }
        } else {
            qint32 yInTile = calcYInTile(m_y, row);
            m_index += row - previousRow;
            if (!switchToTile(yInTile))
                m_havePixels = false;
        }
    }
    return m_havePixels;
}



KisVLineIterator2::~KisVLineIterator2()
{
    releaseTileCache(m_tilesCache, m_data, m_oldData);
}


quint8* KisVLineIterator2::rawData()
{
    return m_writable ? m_data : nullptr;
}


const quint8* KisVLineIterator2::oldRawData() const
{
    return m_oldData;
}

const quint8* KisVLineIterator2::rawDataConst() const
{
    return m_data;
}

bool KisVLineIterator2::switchToTile(qint32 yInTile)
{
    // The caller must ensure that we are not out of bounds
    Q_ASSERT(m_index < m_bottomRow - m_topRow + 1);
    Q_ASSERT(m_index >= 0);
    if (!m_readCursor && m_tilesCache.empty() && !preallocateTiles())
        return false;

    int offset_row = m_pixelSize * m_xInTile;
    if (m_readCursor) {
        const auto pair = m_readCursor->read(m_column, m_topRow + m_index);
        m_data = const_cast<quint8 *>(pair.current);
        m_oldData = const_cast<quint8 *>(pair.before);
        if (!pair.isValid() || pair.currentStride != quint32(m_lineStride)
            || pair.beforeStride != pair.currentStride) {
            m_data = nullptr;
            m_oldData = nullptr;
            return false;
        }
    } else {
        m_data = m_tilesCache[m_index].data;
        m_oldData = m_tilesCache[m_index].oldData;
        if (!m_data || !m_oldData)
            return false;
    }
    m_data += offset_row;
    int offset_col = m_pixelSize * yInTile * KisTileData::WIDTH;
    m_data  += offset_col;
    m_oldData += offset_row + offset_col;
    return true;
}


bool KisVLineIterator2::preallocateTiles()
{
    if (m_writable && !m_readScope) m_readScope = m_dataManager->capturePageStoreReadScope(true);
    const bool populated = populateTileCache(
        m_tilesCache, m_bottomRow - m_topRow + 1,
        m_column, m_topRow, 0, 1);
    if (!populated)
        m_data = m_oldData = nullptr;
    return populated;
}

qint32 KisVLineIterator2::x() const
{
    return m_x + m_offsetX;
}

qint32 KisVLineIterator2::y() const
{
    return m_y + m_offsetY;
}

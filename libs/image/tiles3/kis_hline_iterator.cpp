/*
 *  SPDX-FileCopyrightText: 2010 Lukáš Tvrdý <lukast.dev@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <memory>
#include "kis_hline_iterator.h"


KisHLineIterator2::KisHLineIterator2(KisDataManager *dataManager, qint32 x, qint32 y, qint32 w, qint32 offsetX, qint32 offsetY, bool writable, KisIteratorCompleteListener *completionListener,
                                  std::shared_ptr<const KisPageStoreIteratorReadScope> scope)
    : KisBaseIterator(dataManager, writable, completionListener, std::move(scope)),
      m_offsetX(offsetX),
      m_offsetY(offsetY)
{
    if (m_readScope && !m_readScope->isValid()) return;
    x -= m_offsetX;
    y -= m_offsetY;
    Q_ASSERT(dataManager);

    if (w < 1) w = 1;  // To make sure there's always at least one pixel read.

    m_x = x;
    m_y = y;

    m_left = x;
    m_right = x + w - 1;

    m_top = y;

    m_havePixels = (w == 0) ? false : true;
    if (m_left > m_right) {
        m_havePixels = false;
        return;
    }

    m_leftCol = xToCol(m_left);
    m_rightCol = xToCol(m_right);

    m_row = yToRow(m_y);
    m_yInTile = calcYInTile(m_y, m_row);

    m_leftInLeftmostTile = m_left - m_leftCol * KisTileData::WIDTH;

    // let's preallocate first row
    if (!populateTileCache(m_tilesCache, m_rightCol - m_leftCol + 1,
                           m_leftCol, m_row, 1, 0)) {
        m_havePixels = false;
        return;
    }
    m_index = 0;
    if (!switchToTile(m_leftInLeftmostTile)) {
        m_havePixels = false;
        return;
    }
}

void KisHLineIterator2::resetPixelPos()
{
    m_x = m_left;

    m_index = 0;
    m_havePixels = switchToTile(m_leftInLeftmostTile);
}

void KisHLineIterator2::resetRowPos()
{
    m_y = m_top;

    m_row = yToRow(m_y);
    m_yInTile = calcYInTile(m_y, m_row);
    if (!preallocateTiles()) {
        m_havePixels = false;
        return;
    }

    resetPixelPos();
}

bool KisHLineIterator2::nextPixel()
{
    // We won't increment m_x here as integer can overflow here
    if (m_x >= m_right) {
        return m_havePixels = false;
    } else {
        ++m_x;
        if (m_x <= m_rightmostInTile) {
            if (m_data) { m_data += m_pixelSize; m_oldData += m_pixelSize; }
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


void KisHLineIterator2::nextRow()
{
    m_x = m_left;
    ++m_y;

    if (++m_yInTile < KisTileData::HEIGHT) {
        /* do nothing, usual case */
    } else {
        ++m_row;
        m_yInTile = 0;
        if (!preallocateTiles()) {
            m_havePixels = false;
            return;
        }
    }
    m_index = 0;
    m_havePixels = switchToTile(m_leftInLeftmostTile);
}


qint32 KisHLineIterator2::nConseqPixels() const
{
    return qMin(m_rightmostInTile, m_right) - m_x + 1;
}



bool KisHLineIterator2::nextPixels(qint32 n)
{
    Q_ASSERT_X(!(m_x > 0 && (m_x + n) < 0), "hlineIt+=", "Integer overflow");

    qint32 previousCol = xToCol(m_x);
    // We won't increment m_x here first as integer can overflow
    if (m_x >= m_right || (m_x += n) > m_right) {
        m_havePixels = false;
    } else {
        qint32 col = xToCol(m_x);
        // if we are in the same column in tiles
        if (col == previousCol) {
            if (m_data) { m_data += n * m_pixelSize; m_oldData += n * m_pixelSize; }
        } else {
            qint32 xInTile = calcXInTile(m_x, col);
            m_index += col - previousCol;
            if (!switchToTile(xInTile))
                m_havePixels = false;
        }
    }
    return m_havePixels;
}



KisHLineIterator2::~KisHLineIterator2()
{
    releaseTileCache(m_tilesCache, m_data, m_oldData);
}


quint8* KisHLineIterator2::rawData()
{
    return m_writable ? m_data : nullptr;
}


const quint8* KisHLineIterator2::oldRawData() const
{
    return m_oldData;
}

const quint8* KisHLineIterator2::rawDataConst() const
{
    return m_data;
}

bool KisHLineIterator2::switchToTile(qint32 xInTile)
{
    // The caller must ensure that we are not out of bounds
    Q_ASSERT(m_index < quint32(m_rightCol - m_leftCol + 1));
    if (!m_readCursor && m_tilesCache.empty() && !preallocateTiles())
        return false;

    if (m_readCursor) {
        const auto pair = m_readCursor->read(m_leftCol + qint32(m_index), m_row);
        m_data = const_cast<quint8 *>(pair.current);
        m_oldData = const_cast<quint8 *>(pair.before);
        if (!pair.isValid()
            || pair.currentStride != quint32(m_pixelSize * KisTileData::WIDTH)
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

    int offset_row = m_pixelSize * (m_yInTile * KisTileData::WIDTH);
    m_data += offset_row;
    m_rightmostInTile = (m_leftCol + m_index + 1) * KisTileData::WIDTH - 1;
    int offset_col = m_pixelSize * xInTile;
    m_data  += offset_col;
    m_oldData += offset_row + offset_col;
    return true;
}


bool KisHLineIterator2::preallocateTiles()
{
    if (m_writable && !m_readScope) m_readScope = m_dataManager->capturePageStoreReadScope(true);
    const bool populated = populateTileCache(
        m_tilesCache, m_rightCol - m_leftCol + 1,
        m_leftCol, m_row, 1, 0);
    if (!populated)
        m_data = m_oldData = nullptr;
    return populated;
}

qint32 KisHLineIterator2::x() const
{
    return m_x + m_offsetX;
}

qint32 KisHLineIterator2::y() const
{
    return m_y + m_offsetY;
}

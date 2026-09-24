/*
 *  SPDX-FileCopyrightText: 2010 Lukáš Tvrdý <lukast.dev@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_hline_iterator.h"


KisHLineIterator2::KisHLineIterator2(KisDataManager *dataManager, qint32 x, qint32 y, qint32 w, qint32 offsetX, qint32 offsetY, bool writable, KisIteratorCompleteListener *completionListener,
                                  QSharedPointer<const KisPageStoreIteratorReadScope> scope)
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
    populateTileCache(m_tilesCache, m_rightCol - m_leftCol + 1, m_leftCol, m_row, 1, 0);
    m_index = 0;
    switchToTile(m_leftInLeftmostTile);
    registerWriteBoundary(this, [](const void *context) {
        return !static_cast<const KisHLineIterator2 *>(context)->m_tilesCache.empty();
    });
}

void KisHLineIterator2::resetPixelPos()
{
    m_x = m_left;

    m_index = 0;
    switchToTile(m_leftInLeftmostTile);

    m_havePixels = true;
}

void KisHLineIterator2::resetRowPos()
{
    m_y = m_top;

    m_row = yToRow(m_y);
    m_yInTile = calcYInTile(m_y, m_row);
    preallocateTiles();

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
            switchToTile(0);
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
        preallocateTiles();
    }
    m_index = 0;
    switchToTile(m_leftInLeftmostTile);

    m_havePixels = true;
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
            switchToTile(xInTile);
        }
    }
    return m_havePixels;
}



KisHLineIterator2::~KisHLineIterator2()
{
    unregisterWriteBoundary();
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

void KisHLineIterator2::switchToTile(qint32 xInTile)
{
    // The caller must ensure that we are not out of bounds
    Q_ASSERT(m_index < quint32(m_rightCol - m_leftCol + 1));
    if (!m_readCursor && m_tilesCache.empty()) preallocateTiles();

    if (m_readCursor) {
        const auto pair = m_readCursor->read(m_leftCol + qint32(m_index), m_row);
        m_data = const_cast<quint8 *>(pair.current);
        m_oldData = const_cast<quint8 *>(pair.before);
        KIS_SAFE_ASSERT_RECOVER_RETURN(pair.isValid() && pair.currentStride == quint32(m_pixelSize * KisTileData::WIDTH) &&
                                       pair.beforeStride == pair.currentStride);
    } else {
        m_data = m_tilesCache[m_index].data;
        m_oldData = m_tilesCache[m_index].oldData;
    }

    int offset_row = m_pixelSize * (m_yInTile * KisTileData::WIDTH);
    m_data += offset_row;
    m_rightmostInTile = (m_leftCol + m_index + 1) * KisTileData::WIDTH - 1;
    int offset_col = m_pixelSize * xInTile;
    m_data  += offset_col;
    m_oldData += offset_row + offset_col;
}


void KisHLineIterator2::preallocateTiles()
{
    if (m_writable && !m_readScope) m_readScope = m_dataManager->capturePageStoreReadScope(true);
    populateTileCache(m_tilesCache, m_rightCol - m_leftCol + 1, m_leftCol, m_row, 1, 0);
}

qint32 KisHLineIterator2::x() const
{
    return m_x + m_offsetX;
}

qint32 KisHLineIterator2::y() const
{
    return m_y + m_offsetY;
}

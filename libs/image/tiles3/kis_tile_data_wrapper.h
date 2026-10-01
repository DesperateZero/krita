/*
 *  SPDX-FileCopyrightText: 2009 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __KIS_TILE_DATA_WRAPPER_H
#define __KIS_TILE_DATA_WRAPPER_H


/**
 * KisTileDataWrapper is a special object, that fetches the tile from
 * the data manager according to the position, locks it and returns
 * a pointer to the needed piece of data
 */
class KisTileDataWrapper
{
public:
    enum accessType {
        READ,
        WRITE
    };

    /**
     * Fetches the tile which contains point (\p x, \p y) from
     * the data manager \p dm with access \p type
     */
    inline KisTileDataWrapper(KisTiledDataManager *dm,
                              qint32 x, qint32 y,
                              enum KisTileDataWrapper::accessType type)
    {
        const qint32 col = dm->xToCol(x);
        const qint32 row = dm->yToRow(y);

        /* FIXME: Always positive? */
        const qint32 xInTile = x - col * KisTileData::WIDTH;
        const qint32 yInTile = y - row * KisTileData::HEIGHT;

        const qint32 pixelIndex = xInTile + yInTile * KisTileData::WIDTH;

        KisTileSP tile = dm->getTile(col, row, type == WRITE);

        m_tile = tile;
        m_offset = pixelIndex * dm->pixelSize();

        if (!m_tile) {
            m_locked = false;
        } else if (type == READ) {
            m_locked = m_tile->lockForRead();
        }
        else {
            m_locked = m_tile->lockForWrite();
        }

        m_type = type;
    }

    virtual ~KisTileDataWrapper()
    {
        if (!m_locked) {
            return;
        } else if (m_type == READ) {
            m_tile->unlockForRead();
        } else {
            m_tile->unlockForWrite();
        }
    }

    /**
     * Returns the offset of the data in the tile's chunk of memory
     *
     * \see data()
     */
    inline qint32 offset() const
    {
        return m_offset;
    }

    /**
     * Returns the fetched tile
     */
    inline KisTileSP& tile()
    {
        return m_tile;
    }

    /**
     * Returns the pointer to the pixel, that was passed to
     * the constructor. This points to the raw data of the tile,
     * so you should think about the borders of the tile yourself.
     * When (x,y) is the top-left corner of the tile, the pointer
     * will lead to the beginning of the tile's chunk of memory.
     */
    inline quint8* data() const
    {
        if (!m_locked) return nullptr;
        // A write lock can be only an intent. Failed canonical admission must
        // not expose the cached before-image, even when the tile is resident.
        quint8 *bytes = m_type == WRITE ? m_tile->tryWriteData() : m_tile->data();
        return bytes ? bytes + m_offset : nullptr;
    }

    inline bool isValid() const { return m_locked; }

private:
    Q_DISABLE_COPY(KisTileDataWrapper)

    KisTileSP m_tile;
    qint32 m_offset;
    KisTileDataWrapper::accessType m_type;
    bool m_locked = false;
};
#endif /* __KIS_TILE_DATA_WRAPPER_H */

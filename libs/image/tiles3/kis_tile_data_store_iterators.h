/*
 *  SPDX-FileCopyrightText: 2010 Dmitry Kazakov <dimula73@gmail.com>
 *  SPDX-FileCopyrightText: 2018 Andrey Kamakin <a.kamakin@icloud.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_TILE_DATA_STORE_ITERATORS_H_
#define KIS_TILE_DATA_STORE_ITERATORS_H_

#include "kis_tile_data.h"
#include "kis_debug.h"

/**
 * KisTileDataStoreIterator,
 * KisTileDataStoreReverseIterator,
 * KisTileDataStoreClockIterator
 * - are general iterators for the contents of KisTileDataStore.
 * The store starts holding a lock when returns one of such
 * iterators, so no one will be able to change the list while
 * you are iterating.
 *
 * But be careful! You can't change the list while iterating either,
 * because it can invalidate the iterator. This is a general rule.
 */


class KisTileDataStoreIterator
{
public:
    explicit KisTileDataStoreIterator(ConcurrentMap<int, KisTileData*> &map)
        : m_map(map)
    {
        m_iterator.setMap(m_map);
    }

    inline KisTileData* peekNext()
    {
        return m_iterator.getValue();
    }

    inline KisTileData* next()
    {
        KisTileData *current = m_iterator.getValue();
        m_iterator.next();
        return current;
    }

    inline bool hasNext() const
    {
        return m_iterator.isValid();
    }

private:
    ConcurrentMap<int, KisTileData*> &m_map;
    ConcurrentMap<int, KisTileData*>::Iterator m_iterator;
};

class KisTileDataStoreReverseIterator : public KisTileDataStoreIterator
{
public:
    explicit KisTileDataStoreReverseIterator(ConcurrentMap<int, KisTileData*> &map)
        : KisTileDataStoreIterator(map)
    {
    }
};

class KisTileDataStoreClockIterator
{
public:
    KisTileDataStoreClockIterator(ConcurrentMap<int, KisTileData*> &map,
                                  int startIndex)
        : m_map(map)
    {
        m_iterator.setMap(m_map);
        if (!m_iterator.isValid()) {
            m_finalPosition = startIndex;
            m_startItem = nullptr;
            m_endReached = true;
            return;
        }
        m_finalPosition = m_iterator.getValue()->m_tileNumber;
        m_startItem = m_map.get(startIndex);

        if (m_iterator.getValue() == m_startItem || !m_startItem) {
            m_startItem = 0;
            m_endReached = true;
        } else {
            while (m_iterator.getValue() != m_startItem) {
                m_iterator.next();
            }
            m_endReached = false;
        }
    }

    inline KisTileData* peekNext()
    {
        if (!m_iterator.isValid()) {
            m_iterator.setMap(m_map);
            m_endReached = true;
        }

        return m_iterator.getValue();
    }

    inline KisTileData* next()
    {
        if (!m_iterator.isValid()) {
            m_iterator.setMap(m_map);
            m_endReached = true;
        }

        KisTileData *current = m_iterator.getValue();
        m_iterator.next();
        return current;
    }

    inline bool hasNext() const
    {
        if (!m_iterator.isValid() && !m_startItem)
            return false;
        return !(m_endReached && m_iterator.getValue() == m_startItem);
    }

private:
    friend class KisTileDataStore;
    inline int getFinalPosition()
    {
        if (!m_iterator.isValid()) {
            return m_finalPosition;
        }

        return m_iterator.getValue()->m_tileNumber;
    }

private:
    ConcurrentMap<int, KisTileData*> &m_map;
    ConcurrentMap<int, KisTileData*>::Iterator m_iterator;
    KisTileData *m_startItem;
    bool m_endReached;
    int m_finalPosition;
};

#endif /* KIS_TILE_DATA_STORE_ITERATORS_H_ */

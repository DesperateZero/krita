/* 
 *  SPDX-FileCopyrightText: 2010 Cyrille Berger <cberger@cberger.net>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef _KIS_BASE_ITERATOR_H_
#define _KIS_BASE_ITERATOR_H_

#include "kis_datamanager.h"
#include "kis_tiled_data_manager.h"
#include "kis_tile.h"
#include "kis_types.h"
#include "kis_shared.h"
#include "kis_iterator_complete_listener.h"
#include "pagestore/KisPageStoreIteratorReadScope_p.h"
#include <optional>
#include <utility>
#include <vector>

class KisBaseIterator {
protected:
    KisBaseIterator(KisTiledDataManager * _dataManager, bool _writable, KisIteratorCompleteListener *listener,
                    QSharedPointer<const KisPageStoreIteratorReadScope> scope = {}) {
        m_dataManager = _dataManager;
        m_pixelSize = m_dataManager->pixelSize();
        m_writable = _writable;
        m_completeListener = listener;
        m_readScope = m_dataManager->capturePageStoreReadScope(_writable, std::move(scope));
        if (m_readScope && m_readScope->observesLiveLegacyWriter()) m_readScope.clear();
        if (m_readScope && !_writable) m_readCursor.emplace(m_readScope);
    }
    ~KisBaseIterator() {
        m_readCursor.reset();
        m_readScope.clear();
        if (m_writable && m_completeListener) {
            m_completeListener->notifyWritableIteratorCompleted();
        }
    }

    KisTiledDataManager *m_dataManager;
    qint32 m_pixelSize;        // bytes per pixel
    bool m_writable;
    QSharedPointer<const KisPageStoreIteratorReadScope> m_readScope;
    std::optional<KisPageStoreReadCursor> m_readCursor;
    void registerWriteBoundary(const void *key, KisPageStoreWriteBoundary boundary) {
        if (!m_writable || !m_readScope) return;
        m_boundaryKey = key;
        m_boundaryThread = m_dataManager->registerPageStoreWriteBoundary(key, boundary);
    }
    void unregisterWriteBoundary() {
        if (m_boundaryThread) m_dataManager->unregisterPageStoreWriteBoundary(m_boundaryThread, m_boundaryKey);
        m_boundaryThread = nullptr;
    }
    inline bool lockTile(KisTileSP &tile) {
        return m_writable ? tile->lockForWrite() : tile->lockForRead();
    }
    inline bool lockOldTile(KisTileSP &tile) {
        // Doesn't depend on current access type
        return tile->lockForRead();
    }
    inline void unlockTile(KisTileSP &tile) {
        if (!tile) return;
        if (m_writable) {
            tile->unlockForWrite();
        } else {
            tile->unlockForRead();
        }
    }

    inline void unlockOldTile(KisTileSP &tile) {
        if (tile) tile->unlockForRead();
    }

    template<typename TileInfo>
    void fetchTileDataForCache(TileInfo &info, qint32 column, qint32 row) {
        if (m_readScope) info.tile = m_dataManager->getTile(column, row, m_writable);
        else m_dataManager->getTilesPair(column, row, m_writable, &info.tile, &info.oldtile);
        if (!lockTile(info.tile)) {
            info.tile.clear();
            info.oldtile.clear();
            info.data = nullptr;
            info.oldData = nullptr;
            return;
        }
        info.data = info.tile->data();
        if (m_readScope) {
            if (m_readScope->beforeAliasesWrite()) info.oldData = info.data;
            else {
                info.before = m_readScope->readPage(column, row, true);
                info.oldData = const_cast<quint8 *>(info.before.data());
            }
            if (!info.oldData) {
                unlockTile(info.tile);
                info.tile.clear();
                info.data = nullptr;
                return;
            }
        } else {
            if (!lockOldTile(info.oldtile)) {
                unlockTile(info.tile);
                info.tile.clear();
                info.oldtile.clear();
                info.data = nullptr;
                info.oldData = nullptr;
                return;
            }
            info.oldData = info.oldtile->data();
        }
    }

    template<typename TileInfo>
    bool populateTileCache(std::vector<TileInfo> &cache, qsizetype count,
                           qint32 column, qint32 row, qint32 columnStep, qint32 rowStep) {
        if (m_readCursor) return true;
        if (cache.empty()) cache.resize(size_t(count));
        for (size_t i = 0; i < cache.size(); ++i) {
            unlockTile(cache[i].tile);
            unlockOldTile(cache[i].oldtile);
            cache[i].tile.clear();
            cache[i].oldtile.clear();
            cache[i].before = {};
            cache[i].data = nullptr;
            cache[i].oldData = nullptr;
            fetchTileDataForCache(cache[i], column + qint32(i) * columnStep,
                                  row + qint32(i) * rowStep);
            if (!cache[i].data || !cache[i].oldData) {
                for (TileInfo &entry : cache) {
                    unlockTile(entry.tile);
                    unlockOldTile(entry.oldtile);
                }
                cache.clear();
                return false;
            }
        }
        return true;
    }

    template<typename TileInfo>
    void releaseTileCache(std::vector<TileInfo> &cache, quint8 *&data, quint8 *&oldData) {
        for (TileInfo &info : cache) {
            unlockTile(info.tile);
            unlockOldTile(info.oldtile);
        }
        cache.clear();
        m_readScope.clear();
        data = oldData = nullptr;
    }

    inline quint32 xToCol(quint32 x) const {
        return m_dataManager ? m_dataManager->xToCol(x) : 0;
    }
    inline quint32 yToRow(quint32 y) const {
        return m_dataManager ? m_dataManager->yToRow(y) : 0;
    }

    inline qint32 calcXInTile(qint32 x, qint32 col) const {
        return x - col * KisTileData::WIDTH;
    }

    inline qint32 calcYInTile(qint32 y, qint32 row) const {
        return y - row * KisTileData::HEIGHT;
    }
    
private:
    Qt::HANDLE m_boundaryThread = nullptr;
    const void *m_boundaryKey = nullptr;
    KisIteratorCompleteListener *m_completeListener;
};

#endif

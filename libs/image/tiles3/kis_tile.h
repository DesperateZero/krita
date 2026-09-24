/*
 *  SPDX-FileCopyrightText: 2004 C. Boemann <cbo@boemann.dk>
 *            (c) 2009 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_TILE_H_
#define KIS_TILE_H_

#include <QReadWriteLock>

#include <QMutex>
#include <QAtomicPointer>

#include <QRect>
#include <QStack>

#include <memory>
#include <vector>

#include <kis_shared.h>
#include <kis_shared_ptr.h>

#include "kis_tile_data.h"
#include "kis_tile_data_store.h"
#include "KisTilePageStoreBridge.h"

//#define DEAD_TILES_SANITY_CHECK

class KisTile;
typedef KisSharedPtr<KisTile> KisTileSP;

class KisMementoManager;
/**
 * Provides abstraction to a tile.
 * + A tile contains a part of a PaintDevice,
 *   but only the individual pixels are accessible
 *   and that only via iterators.
 * + Actual tile data is stored in KisTileData that can be
 *   shared between many tiles
 */
class KRITAIMAGE_EXPORT KisTile : public KisShared
{
public:
    KisTile(qint32 col, qint32 row,
            KisTileData *defaultTileData, KisMementoManager* mm);
    KisTile(const KisTile& rhs, qint32 col, qint32 row, KisMementoManager* mm);
    KisTile(const KisTile& rhs, KisMementoManager* mm);
    KisTile(const KisTile& rhs);
    ~KisTile();

    /**
     * This method is called by the hash table when the tile is
     * disconnected from it. It means that from now on the tile is not
     * associated with any particular datamanager. All the users of
     * the tile (via shared pointers) may silently finish they work on
     * this tile and leave it. No result will be saved. Used for
     * threading purposes
     */
    void notifyDetachedFromDataManager();

    /**
     * Sometimes the tile gets replaced with another tile. In this case
     * we shouldn't notify memento manager that the tile has died. Just
     * forget the link to the manager and bury it in peace.
     */
    void notifyDeadWithoutDetaching();

    /**
     * Called by the hash table to notify that the tile has been attached
     * to the data manager.
     */
    void notifyAttachedToDataManager(KisMementoManager *mm);

    void setPageStoreBridge(KisTilePageStoreBridge *bridge, bool oldData);
    /**
     * Marks the currently cached TileData as the exact native CPU view for
     * this wrapper.  Used for virtual/default reads and after a successful
     * PageStore refresh so lockForRead() can use the normal tile-local swap
     * pin without issuing a second generic PageStore request.
    */
    void setPageStoreNativeReadReady();
    bool refreshPageStoreData();
    // The caller holds an exact canonical read capability over replacement.
    // Existing borrowed pointers are not revoked; refresh waits for unlock.
    void installPageStoreReadCache(KisTileData *replacement);

public:
    void debugPrintInfo();
    void debugDumpTile();

    // False means no tile lock or backing pin was acquired; callers must not
    // access data() or call the matching unlock method in that case.
    bool lockForRead() const;
    bool lockForWrite();
    void unlockForWrite();
    void unlockForRead() const;

    // Fallible mutable-pointer entry for bulk operations. Do not return the
    // cached before-image when PageStore denies write authorization.
    quint8 *tryWriteData() const;
    // Compatibility classification only, not a pixel/access capability.
    // Includes a borrowed write intent before its first mutable data exposure.
    bool hasPageStoreWriteIntent() const;


    /* this allows us work directly on tile's data */
    inline quint8 *data() const {
        markPageStoreWriteDirty();
        return m_tileData->data();
    }
    inline void setData(const quint8 *data) {
        markPageStoreWriteDirty();
        m_tileData->setData(data);
    }

    inline qint32 row() const {
        return m_row;
    }
    inline qint32 col() const {
        return m_col;
    }

    inline QRect extent() const {
        return m_extent;
    }

    inline KisTileSP next() const {
        return m_nextTile;
    }

    void setNext(KisTileSP next) {
        m_nextTile = next;
    }

    inline qint32 pixelSize() const {
        /* don't lock here as pixelSize is constant */
        return m_tileData->pixelSize();
    }

    inline KisTileData*  tileData() const {
        markPageStoreWriteDirty();
        return m_tileData;
    }

private:
    inline KisTilePageStoreLease *pageStoreWriteLease() const {
        for (auto it = m_pageStoreLeases.rbegin(); it != m_pageStoreLeases.rend(); ++it)
            if ((*it)->writable()) return it->get();
        return nullptr;
    }

    inline void markPageStoreWriteDirty() const {
        KisTilePageStoreLease *lease = pageStoreWriteLease();
        if (!lease && Q_UNLIKELY(m_pageStoreWriteLockCount > 0)) {
            KIS_SAFE_ASSERT_RECOVER_NOOP(ensurePageStoreWriteAccess());
            lease = pageStoreWriteLease();
        }
        if (lease) lease->markDirty();
    }

    void init(qint32 col, qint32 row,
              KisTileData *defaultTileData, KisMementoManager* mm);

    inline bool blockSwapping() const;
    inline void unblockSwapping() const;

    inline void safeReleaseOldTileData(KisTileData *td);
    bool ensurePageStoreWriteAccess() const;

private:
    KisTilePageStoreBridge *resolvePageStoreBridge(KisMementoManager *manager) const;
    void replacePageStoreReadCacheLocked(KisTileData *replacement) const;
    bool releasePageStoreLeasesLocked() const;

    KisTileData *m_tileData;
    mutable QStack<KisTileData*> m_oldTileData;
    mutable volatile int m_lockCounter;

    qint32 m_col;
    qint32 m_row;

    /**
     * Added for faster retrieving by processors
     */
    QRect m_extent;

    /**
     * For KisTiledDataManager's hash table
     */
    KisTileSP m_nextTile;

    QAtomicPointer<KisMementoManager> m_mementoManager;
    QAtomicInteger<int> m_pageStoreOldDataView{0};
    mutable QAtomicInteger<int> m_pageStoreNativeReadReady{0};
    mutable qint32 m_pageStoreWriteLockCount = 0;
    mutable bool m_pageStoreWriteIntentOwnsNativePin = false;
    QAtomicPointer<KisTilePageStoreBridge> m_pageStoreBridge;
    mutable std::vector<std::unique_ptr<KisTilePageStoreLease>> m_pageStoreLeases;

    /**
     * This is a special mutex for guarding copy-on-write
     * operations. We do not use lockless way here as it'll
     * create too much overhead for the most common operations
     * like "read the pointer of m_tileData".
     */
    QMutex m_COWMutex;

    /**
     * This lock is used to ensure no one will read the tile data
     * before it has been loaded from to the memory.
     */
    mutable QMutex m_swapBarrierLock;


#ifdef DEAD_TILES_SANITY_CHECK
    QAtomicInt m_sanityHasBeenDetached;
    QAtomicInt m_sanityIsDead;
    QAtomicInt m_sanityMMHasBeenInitializedManually;
    QAtomicInt m_sanityNumCOWHappened;
    QAtomicInt m_sanityLockedForWrite;
    mutable QAtomicInt m_sanityLockedForRead;

    void sanityCheckIsNotDestroyedYet();
    void sanityCheckIsNotLockedForWrite();
#endif

};

#endif // KIS_TILE_H_

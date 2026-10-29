/*
 *  SPDX-FileCopyrightText: 2002 Patrick Julien <freak@codepimps.org>
 *            (c) 2009 Dmitry  Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */


#include <QMutexLocker>
#include "kis_tile_data.h"
#include "kis_tile_data_store.h"
#include "kis_tile.h"
#include "kis_memento_manager.h"
#include "KisTilePageStoreBridge.h"
#include "kis_debug.h"

#include <algorithm>
#include <new>


void KisTile::init(qint32 col, qint32 row,
                   KisTileData *defaultTileData, KisMementoManager* mm)
{
    if (!defaultTileData) throw std::bad_alloc();
    m_col = col;
    m_row = row;
    m_lockCounter = 0;
    m_pageStoreOldDataView.storeRelaxed(0);
    m_pageStoreNativeReadReady.storeRelaxed(0);
    m_pageStoreWriteLockCount = 0;
    m_pageStoreWriteIntentOwnsNativePin = false;
    m_pageStoreBridge.storeRelaxed(nullptr);

    m_extent = QRect(m_col * KisTileData::WIDTH, m_row * KisTileData::HEIGHT,
                     KisTileData::WIDTH, KisTileData::HEIGHT);

    m_tileData = defaultTileData;
    m_tileData->acquire();

    if (mm) {
        mm->registerTileChange(this);
    }
    m_mementoManager.storeRelease(mm);
}

void KisTile::setPageStoreBridge(KisTilePageStoreBridge *bridge, bool oldData)
{
    if (m_pageStoreBridge.loadAcquire() == bridge &&
        bool(m_pageStoreOldDataView.loadAcquire()) == oldData) {
        return;
    }
    QMutexLocker locker(&m_swapBarrierLock);
    if (m_lockCounter != 0) {
        Q_ASSERT(m_pageStoreBridge.loadRelaxed() == bridge &&
                 bool(m_pageStoreOldDataView.loadRelaxed()) == oldData);
        return;
    }
    if (m_pageStoreBridge.loadRelaxed() != bridge ||
        bool(m_pageStoreOldDataView.loadRelaxed()) != oldData) {
        m_pageStoreLeases[ReadLease].reset();
        m_pageStoreNativeReadReady.storeRelaxed(0);
        m_pageStoreOldDataView.storeRelaxed(oldData ? 1 : 0);
        m_pageStoreBridge.storeRelease(bridge);
    }
}

void KisTile::setPageStoreNativeReadReady()
{
    if (m_pageStoreNativeReadReady.loadAcquire()) return;
    QMutexLocker locker(&m_swapBarrierLock);
    Q_ASSERT(m_lockCounter == 0);
    m_pageStoreNativeReadReady.storeRelease(1);
}

void KisTile::invalidatePageStoreReadCache()
{
    QMutexLocker lock(&m_swapBarrierLock);
    // Existing nested reads keep their exact bytes until the last unlock.
    // No backing resolve, pin, swap-in or replacement belongs to invalidation.
    m_pageStoreNativeReadReady.storeRelease(0);
}

KisTilePageStoreBridge *KisTile::resolvePageStoreBridge(KisMementoManager *manager) const
{
    KisTilePageStoreBridge *bridge = m_pageStoreBridge.loadAcquire();
    return bridge ? bridge : (manager ? manager->pageStoreBridge() : nullptr);
}

TileLease KisTile::acquirePageStoreLease(bool writable, bool resolveSelection) const
{
    // All callers hold the tile barrier. The bridge owns selection and batch
    // authorization; this wrapper only supplies intent and dormant cache.
    KisTilePageStoreBridge *bridge = resolvePageStoreBridge(m_mementoManager.loadAcquire());
    if (!bridge) return {};
    const auto access = writable ? KisTilePageStoreBridge::Access::Write
        : resolveSelection ? KisTilePageStoreBridge::Access::ReadSelection
                           : KisTilePageStoreBridge::Access::ReadCache;
    return bridge->acquireTile(m_col, m_row, access, &m_pageStoreLeases[ReadLease]);
}

void KisTile::replacePageStoreReadCacheLocked(KisTileData *replacement) const
{
    if (replacement == m_tileData) return;
    replacement->acquire();
    KisTileData *previous = m_tileData;
    const_cast<KisTile *>(this)->m_tileData = replacement;
    previous->release();
}

bool KisTile::releasePageStoreLeasesLocked(TileLeaseRelease &afterUnlock) const
{
    if (!hasPageStoreLeases()) return false;
    Q_ASSERT(m_lockCounter > 0);
    --m_lockCounter;
    if (m_lockCounter != 0) return true;
    auto &read = m_pageStoreLeases[ReadLease];
    if (read) {
        KIS_SAFE_ASSERT_RECOVER_NOOP(read->finish(&afterUnlock));
        if (!read->reusableReadCache()) read = read->takeReadCache();
    }
    auto &write = m_pageStoreLeases[WriteLease];
    if (write) {
        KIS_SAFE_ASSERT_RECOVER_NOOP(write->finish(&afterUnlock));
        read = write->takeReadCache();
        write.reset();
    }
    for (KisTileData *tileData : m_oldTileData) {
        tileData->unblockSwapping();
        tileData->release();
    }
    m_oldTileData.clear();
    return true;
}

bool KisTile::refreshPageStoreData()
{
    if (m_pageStoreNativeReadReady.loadAcquire()) return true;
    KisMementoManager *manager = m_mementoManager.loadAcquire();
    QMutexLocker locker(&m_swapBarrierLock);
    if (!resolvePageStoreBridge(manager) || m_lockCounter != 0) return true;
    if (m_pageStoreNativeReadReady.loadRelaxed()) return true;
    TileLease lease = acquirePageStoreLease(false);
    if (!lease || !lease->tileData()) return false;
    KisTileData *replacement = lease->tileData();
    replacePageStoreReadCacheLocked(replacement);
    const bool finished = lease->finish();
    if (finished) {
        m_pageStoreLeases[ReadLease] = std::move(lease);
        m_pageStoreNativeReadReady.storeRelease(1);
    }
    return finished;
}

KisTile::KisTile(qint32 col, qint32 row,
                 KisTileData *defaultTileData, KisMementoManager* mm)
{
    init(col, row, defaultTileData, mm);
}

KisTile::KisTile(const KisTile& rhs, qint32 col, qint32 row, KisMementoManager* mm)
        : KisShared()
{
    init(col, row, rhs.tileData(), mm);
}

KisTile::KisTile(const KisTile& rhs, KisMementoManager* mm)
        : KisShared()
{
    init(rhs.col(), rhs.row(), rhs.tileData(), mm);
}

KisTile::KisTile(const KisTile& rhs)
        : KisShared()
{
    init(rhs.col(), rhs.row(), rhs.tileData(), rhs.m_mementoManager);
}

KisTile::~KisTile()
{
#ifdef DEAD_TILES_SANITY_CHECK
    KIS_ASSERT(!m_lockCounter);

    /**
     * We should have been disconnected from the memento manager in
     * notifyDetachedFromDataManager() or notifyDeadWithoutDetaching(),
     * otherwise there is a bug
     */

    if (m_mementoManager) {
        qDebug() << this << ppVar(m_sanityNumCOWHappened);
        qDebug() << this << ppVar(m_sanityHasBeenDetached);
        qDebug() << this << ppVar(m_sanityMMHasBeenInitializedManually);
        qDebug() << this << ppVar(m_sanityIsDead);
        KIS_ASSERT(0 && "m_mementoManager is still initialized during destruction");
    }
#endif

    m_tileData->release();
}

void KisTile::notifyDetachedFromDataManager()
{
#ifdef DEAD_TILES_SANITY_CHECK
    sanityCheckIsNotLockedForWrite();
#endif

    KisMementoManager *manager = nullptr;
    {
        QMutexLocker lock(&m_swapBarrierLock);
        manager = m_mementoManager.loadAcquire();
        m_mementoManager.storeRelease(0);
        m_pageStoreBridge.storeRelease(nullptr);
        if (!m_lockCounter) m_pageStoreLeases[ReadLease].reset();
    }
    if (manager) manager->registerTileDeleted(this);

#ifdef DEAD_TILES_SANITY_CHECK
    m_sanityHasBeenDetached.ref();
#endif
}

void KisTile::notifyDeadWithoutDetaching()
{
#ifdef DEAD_TILES_SANITY_CHECK
    sanityCheckIsNotLockedForWrite();
#endif

    {
        QMutexLocker lock(&m_swapBarrierLock);
        m_mementoManager.storeRelease(0);
        m_pageStoreBridge.storeRelease(nullptr);
        if (!m_lockCounter) m_pageStoreLeases[ReadLease].reset();
    }

#ifdef DEAD_TILES_SANITY_CHECK
    m_sanityIsDead.ref();
#endif
}

void KisTile::notifyAttachedToDataManager(KisMementoManager *mm)
{
#ifdef DEAD_TILES_SANITY_CHECK
    sanityCheckIsNotDestroyedYet();
#endif

    // TODO: check if we really need locking here
    if (!m_mementoManager.loadAcquire()) {
        QMutexLocker locker(&m_COWMutex);

        if (!m_mementoManager.loadAcquire()) {

            if (mm) {
                mm->registerTileChange(this);
            }
            m_mementoManager.storeRelease(mm);

#ifdef DEAD_TILES_SANITY_CHECK
            m_sanityMMHasBeenInitializedManually.ref();
#endif
        }
    }

#ifdef DEAD_TILES_SANITY_CHECK
    sanityCheckIsNotDestroyedYet();
#endif
}

//#define DEBUG_TILE_LOCKING
//#define DEBUG_TILE_COWING

#ifdef DEBUG_TILE_LOCKING
#define DEBUG_LOG_ACTION(action)                                        \
    printf("### %s \ttile:\t0x%llX (%d, %d) (0x%llX) ###\n", action, (quintptr)this, m_col, m_row, (quintptr)m_tileData)
#else
#define DEBUG_LOG_ACTION(action)
#endif

#ifdef DEBUG_TILE_COWING
#define DEBUG_COWING(newTD)                                             \
    printf("### COW done \ttile:\t0x%X (%d, %d) (0x%X -> 0x%X) [mm: 0x%X] ###\n", (quintptr)this, m_col, m_row, (quintptr)m_tileData, (quintptr)newTD, m_mementoManager);
#else
#define DEBUG_COWING(newTD)
#endif

inline bool KisTile::blockSwapping() const
{
    /**
     * We need to hold a special barrier lock here to ensure
     * m_tileData->blockSwapping() has finished executing
     * before anyone started reading the tile data. That is
     * why we can not use atomic operations here.
     */

    QMutexLocker locker(&m_swapBarrierLock);
    Q_ASSERT(m_lockCounter >= 0);

    if (m_lockCounter == 0 && !m_tileData->blockSwapping())
        return false;
    ++m_lockCounter;

    Q_ASSERT(m_tileData->data());
    return true;
}

inline void KisTile::unblockSwappingLocked() const
{
    Q_ASSERT(m_lockCounter > 0);

    if(--m_lockCounter == 0) {
        m_tileData->unblockSwapping();
        // The final native unlock may belong to a reader that outlived an
        // unexposed writer. Its pin and the write-intent ownership end together.
        m_pageStoreWriteIntentOwnsNativePin = false;

        if(!m_oldTileData.isEmpty()) {
            Q_FOREACH (KisTileData *td, m_oldTileData) {
                td->unblockSwapping();
                td->release();
            }
            m_oldTileData.clear();
        }
    }
}

inline void KisTile::safeReleaseOldTileData(KisTileData *td)
{
    QMutexLocker locker(&m_swapBarrierLock);
    Q_ASSERT(m_lockCounter >= 0);

    if(m_lockCounter > 0) {
        m_oldTileData.push(td);
    }
    else {
        td->unblockSwapping();
        td->release();
    }
}

bool KisTile::lockForRead() const
{
    const auto succeeded = [&] {
#ifdef DEAD_TILES_SANITY_CHECK
        m_sanityLockedForRead.ref();
#endif
        return true;
    };
    KisMementoManager *manager = m_mementoManager.loadAcquire();
    {
        QMutexLocker locker(&m_swapBarrierLock);
        if (resolvePageStoreBridge(manager)) {
            if (m_lockCounter != 0) {
                ++m_lockCounter;
                return succeeded();
            }
            if (m_pageStoreNativeReadReady.loadRelaxed()) {
                auto &cache = m_pageStoreLeases[ReadLease];
                if (cache) {
                    const auto result = cache->repinRead();
                    if (result == KisTilePageStoreLease::ReadPinResult::Ready) {
                        Q_ASSERT(cache->tileData() == m_tileData);
                        ++m_lockCounter;
                        return succeeded();
                    }
                    // Retirement permanently closes this binding. This
                    // wrapper still owns the original immutable/COW storage,
                    // so retain its old pixels without reconnecting current.
                    // An identity mismatch is different: never read a retagged
                    // backing through this cache. Idle-reference migration
                    // must replace this ownership before removing that ref.
                    if (result != KisTilePageStoreLease::ReadPinResult::Retired) {
                        auto borrow = acquirePageStoreLease(false, false);
                        if (!borrow || borrow->tileData() != m_tileData) return false;
                        cache = std::move(borrow);
                        ++m_lockCounter;
                        return succeeded();
                    }
                    cache.reset();
                }
                if (!m_tileData->blockSwapping())
                    return false;
                ++m_lockCounter;
                Q_ASSERT(m_tileData->data());
                return succeeded();
            }
            TileLease lease = acquirePageStoreLease(false);
            if (!lease || !lease->tileData())
                return false;
            replacePageStoreReadCacheLocked(lease->tileData());
            m_pageStoreNativeReadReady.storeRelease(1);
            ++m_lockCounter;
            Q_ASSERT(!hasPageStoreLeases());
            m_pageStoreLeases[ReadLease] = std::move(lease);
            return succeeded();
        }
    }

    DEBUG_LOG_ACTION("lock [R]");
    const bool locked = blockSwapping();
    return locked && succeeded();
}


#define lazyCopying() (m_tileData->m_usersCount>1)

bool KisTile::ensurePageStoreWriteAccess() const
{
    QMutexLocker locker(&m_swapBarrierLock);
    if (m_pageStoreWriteLockCount <= 0 || pageStoreWriteLease()) {
        return true;
    }

    auto &read = m_pageStoreLeases[ReadLease];
    if (read && read->writable()) {
        // A same-owner batch read already borrowed this pending guard. Nested
        // write uses that capability; a second beginWrite would reject it.
        m_pageStoreLeases[WriteLease] = std::move(read);
        return true;
    }

    const bool hadPageStoreLease = hasPageStoreLeases();
    TileLease lease = acquirePageStoreLease(true);
    if (!lease || !lease->tileData() || !lease->writable()) return false;

    KisTileData *replacement = lease->tileData();
    KisTileData *previous = m_tileData;
    if (replacement != previous) {
        replacement->acquire();
        const_cast<KisTile *>(this)->m_tileData = replacement;
    }

    // A write intent entered through the native resident path owns the one
    // tile-local swap pin.  The resolved provider lease now owns the pin for
    // the writable generation, so hand over without keeping both backings
    // pinned for the remainder of the mutation.
    if (m_pageStoreWriteIntentOwnsNativePin) {
        previous->unblockSwapping();
        if (replacement != previous) previous->release();
        m_pageStoreWriteIntentOwnsNativePin = false;
    } else if (replacement != previous) {
        // This is the uncommon read-then-write nesting case.  A generic read
        // lease already owns its pin.  A native outer read, however, needs us
        // to retain and later release the previous backing explicitly.
        if (hadPageStoreLease) {
            previous->release();
        } else {
            m_oldTileData.push(previous);
        }
    }

    m_pageStoreNativeReadReady.storeRelease(1);
    Q_ASSERT(!m_pageStoreLeases[WriteLease]);
    m_pageStoreLeases[WriteLease] = std::move(lease);
    return true;
}

quint8 *KisTile::tryWriteData() const
{
    return data();
}

bool KisTile::hasPageStoreWriteIntent() const
{
    QMutexLocker locker(&m_swapBarrierLock);
    return m_pageStoreWriteLockCount > 0;
}

bool KisTile::lockForWrite()
{
    const auto succeeded = [&] {
#ifdef DEAD_TILES_SANITY_CHECK
        m_sanityLockedForWrite.ref();
#endif
        return true;
    };
    KisMementoManager *manager = m_mementoManager.loadAcquire();
    {
        QMutexLocker locker(&m_swapBarrierLock);
        // A standalone G0 wrapper has storage lifetime, no writer authority.
        if (m_pageStoreOldDataView.loadRelaxed()) return false;
        if (resolvePageStoreBridge(manager)) {
            if (m_lockCounter == 0) {
                if (!m_tileData->blockSwapping())
                    return false;
                m_pageStoreWriteIntentOwnsNativePin = true;
            }
            ++m_lockCounter;
            ++m_pageStoreWriteLockCount;
            DEBUG_LOG_ACTION("lock [W/PageStore intent]");
            return succeeded();
        }
    }

    if (!blockSwapping())
        return false;

    /* We are doing COW here */
    if (lazyCopying()) {
        m_COWMutex.lock();

        /**
         * Everything could have happened before we took
         * the mutex, so let's check again...
         */

        if (lazyCopying()) {

            KisTileData *tileData = m_tileData->clone();
            if (!tileData) {
                m_COWMutex.unlock();
                QMutexLocker locker(&m_swapBarrierLock);
                unblockSwappingLocked();
                return false;
            }
            tileData->acquire();
            if (!tileData->blockSwapping()) {
                tileData->release();
                m_COWMutex.unlock();
                QMutexLocker locker(&m_swapBarrierLock);
                unblockSwappingLocked();
                return false;
            }
            KisTileData *oldTileData = m_tileData;
            m_tileData = tileData;
            safeReleaseOldTileData(oldTileData);

            DEBUG_COWING(tileData);

            KisMementoManager *mm = m_mementoManager.loadRelaxed();
            if (mm) {
                mm->registerTileChange(this);
            }
        }
        m_COWMutex.unlock();

#ifdef DEAD_TILES_SANITY_CHECK
        m_sanityNumCOWHappened.ref();
#endif
    }

    DEBUG_LOG_ACTION("lock [W]");
    return succeeded();
}

void KisTile::unlockForWrite()
{
    // Only one borrowed mutable guard can exist for this Tile. A batch read
    // is moved into WriteLease before writable exposure, never duplicated.
    TileLeaseRelease afterUnlock{nullptr, nullptr};
#ifdef DEAD_TILES_SANITY_CHECK
    m_sanityLockedForWrite.deref();
    KIS_ASSERT(m_sanityLockedForWrite.loadAcquire() >= 0);
#endif
    {
        QMutexLocker locker(&m_swapBarrierLock);
        if (m_pageStoreWriteLockCount > 0) {
            --m_pageStoreWriteLockCount;
        }
        if (releasePageStoreLeasesLocked(afterUnlock)) {
            DEBUG_LOG_ACTION("unlock [W/PageStore]");
            return;
        }
        unblockSwappingLocked();
    }
    DEBUG_LOG_ACTION("unlock [W]");

}

void KisTile::unlockForRead() const
{
    TileLeaseRelease afterUnlock{nullptr, nullptr};
#ifdef DEAD_TILES_SANITY_CHECK
    m_sanityLockedForRead.deref();
    KIS_ASSERT(m_sanityLockedForRead.loadAcquire() >= 0);
#endif
    {
        QMutexLocker locker(&m_swapBarrierLock);
        if (releasePageStoreLeasesLocked(afterUnlock)) {
            DEBUG_LOG_ACTION("unlock [R/PageStore]");
            return;
        }
        unblockSwappingLocked();
    }
    DEBUG_LOG_ACTION("unlock [R]");

}

#include <stdio.h>
void KisTile::debugPrintInfo()
{
    dbgTiles << "------\n"
                "Tile:\t\t\t" << this
                << "\n   data:\t" << m_tileData
                << "\n   next:\t" <<  m_nextTile.data();

}

void KisTile::debugDumpTile()
{
    if (!lockForRead())
        return;
    quint8 *data = this->data();
    if (!data) {
        unlockForRead();
        return;
    }

    for (int i = 0; i < KisTileData::HEIGHT; i++) {
        for (int j = 0; j < KisTileData::WIDTH; j++) {
            dbgTiles << data[(i*KisTileData::WIDTH+j)*pixelSize()];
        }
    }
    unlockForRead();
}

#ifdef DEAD_TILES_SANITY_CHECK

void KisTile::sanityCheckIsNotDestroyedYet()
{
    if (m_lockCounter) {
        qDebug() << this << ppVar(m_sanityLockedForRead);
        qDebug() << this << ppVar(m_sanityLockedForWrite);
        qDebug() << this << ppVar(m_lockCounter);

        KIS_ASSERT(!m_lockCounter || !m_sanityLockedForWrite && "sanityCheckIsNotDestroyedYet() failed");
    }
}

void KisTile::sanityCheckIsNotLockedForWrite()
{
    if (m_sanityHasBeenDetached.loadAcquire()) {
        qDebug() << this << ppVar(m_sanityNumCOWHappened);
        qDebug() << this << ppVar(m_sanityHasBeenDetached);
        qDebug() << this << ppVar(m_sanityMMHasBeenInitializedManually);
        qDebug() << this << ppVar(m_sanityIsDead);
        KIS_ASSERT(0 && "sanityCheckIsNotLockedForWrite() failed");
    }
}

#endif

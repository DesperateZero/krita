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


void KisTile::init(qint32 col, qint32 row,
                   KisTileData *defaultTileData, KisMementoManager* mm)
{
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

void KisTile::installPageStoreReadCache(KisTileData *replacement)
{
    Q_ASSERT(replacement);
    QMutexLocker locker(&m_swapBarrierLock);
    if (m_lockCounter != 0) {
        m_pageStoreNativeReadReady.storeRelease(0);
        return;
    }
    replacePageStoreReadCacheLocked(replacement);
    m_pageStoreNativeReadReady.storeRelease(1);
}

KisTilePageStoreBridge *KisTile::resolvePageStoreBridge(KisMementoManager *manager) const
{
    KisTilePageStoreBridge *bridge = m_pageStoreBridge.loadAcquire();
    return bridge ? bridge : (manager ? manager->pageStoreBridge() : nullptr);
}

void KisTile::replacePageStoreReadCacheLocked(KisTileData *replacement) const
{
    if (replacement == m_tileData) return;
    replacement->acquire();
    KisTileData *previous = m_tileData;
    const_cast<KisTile *>(this)->m_tileData = replacement;
    previous->release();
}

bool KisTile::releasePageStoreLeasesLocked() const
{
    if (m_pageStoreLeases.empty()) return false;
    Q_ASSERT(m_lockCounter > 0);
    --m_lockCounter;
    if (m_lockCounter != 0) return true;
    for (const auto &lease : m_pageStoreLeases)
        KIS_SAFE_ASSERT_RECOVER_NOOP(lease->finish());
    m_pageStoreLeases.clear();
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
    KisTilePageStoreBridge *bridge = resolvePageStoreBridge(manager);
    if (!bridge || m_lockCounter != 0) return true;
    if (m_pageStoreNativeReadReady.loadRelaxed()) return true;
    std::unique_ptr<KisTilePageStoreLease> lease =
        bridge->acquireTile(
            m_col, m_row, false,
            bool(m_pageStoreOldDataView.loadRelaxed()));
    if (!lease || !lease->tileData()) return false;
    KisTileData *replacement = lease->tileData();
    replacePageStoreReadCacheLocked(replacement);
    const bool finished = lease->finish();
    if (finished) m_pageStoreNativeReadReady.storeRelease(1);
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

    if (m_mementoManager.loadAcquire()) {
        KisMementoManager *manager = m_mementoManager;
        m_mementoManager.storeRelease(0);
        manager->registerTileDeleted(this);
    }

#ifdef DEAD_TILES_SANITY_CHECK
    m_sanityHasBeenDetached.ref();
#endif
}

void KisTile::notifyDeadWithoutDetaching()
{
#ifdef DEAD_TILES_SANITY_CHECK
    sanityCheckIsNotLockedForWrite();
#endif

    m_mementoManager.storeRelease(0);

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

inline void KisTile::blockSwapping() const
{
    /**
     * We need to hold a special barrier lock here to ensure
     * m_tileData->blockSwapping() has finished executing
     * before anyone started reading the tile data. That is
     * why we can not use atomic operations here.
     */

    QMutexLocker locker(&m_swapBarrierLock);
    Q_ASSERT(m_lockCounter >= 0);

    if(!m_lockCounter++)
        m_tileData->blockSwapping();

    Q_ASSERT(data());
}

inline void KisTile::unblockSwapping() const
{
    QMutexLocker locker(&m_swapBarrierLock);
    Q_ASSERT(m_lockCounter > 0);

    if(--m_lockCounter == 0) {
        m_tileData->unblockSwapping();

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

void KisTile::lockForRead() const
{
#ifdef DEAD_TILES_SANITY_CHECK
    m_sanityLockedForRead.ref();
#endif

    KisMementoManager *manager = m_mementoManager.loadAcquire();
    {
        QMutexLocker locker(&m_swapBarrierLock);
        KisTilePageStoreBridge *bridge = resolvePageStoreBridge(manager);
        if (bridge) {
            if (m_lockCounter != 0) {
                ++m_lockCounter;
                return;
            }
            if (m_pageStoreNativeReadReady.loadRelaxed()) {
                ++m_lockCounter;
                m_tileData->blockSwapping();
                Q_ASSERT(m_tileData->data());
                return;
            }
            std::unique_ptr<KisTilePageStoreLease> lease =
                bridge->acquireTile(m_col, m_row, false,
                                    bool(m_pageStoreOldDataView.loadRelaxed()));
            KIS_SAFE_ASSERT_RECOVER_RETURN(lease && lease->tileData());
            replacePageStoreReadCacheLocked(lease->tileData());
            m_pageStoreNativeReadReady.storeRelease(1);
            ++m_lockCounter;
            m_pageStoreLeases.push_back(std::move(lease));
            return;
        }
    }

    DEBUG_LOG_ACTION("lock [R]");
    blockSwapping();
}


#define lazyCopying() (m_tileData->m_usersCount>1)

bool KisTile::ensurePageStoreWriteAccess() const
{
    QMutexLocker locker(&m_swapBarrierLock);
    if (m_pageStoreWriteLockCount <= 0 || pageStoreWriteLease()) {
        return true;
    }

    KisMementoManager *manager = m_mementoManager.loadAcquire();
    KisTilePageStoreBridge *bridge = resolvePageStoreBridge(manager);
    if (!bridge || m_pageStoreOldDataView.loadRelaxed()) return false;

    std::unique_ptr<KisTilePageStoreLease> lease =
        bridge->acquireTile(m_col, m_row, true, false);
    if (!lease || !lease->tileData() || !lease->writable()) return false;

    const bool hadPageStoreLease = !m_pageStoreLeases.empty();
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
    m_pageStoreLeases.push_back(std::move(lease));
    return true;
}

quint8 *KisTile::tryWriteData() const
{
    if (!ensurePageStoreWriteAccess()) return nullptr;
    return data();
}

bool KisTile::hasPageStoreWriteIntent() const
{
    QMutexLocker locker(&m_swapBarrierLock);
    return m_pageStoreWriteLockCount > 0;
}

void KisTile::lockForWrite()
{
#ifdef DEAD_TILES_SANITY_CHECK
    m_sanityLockedForWrite.ref();
#endif

    KisMementoManager *manager = m_mementoManager.loadAcquire();
    {
        QMutexLocker locker(&m_swapBarrierLock);
        KisTilePageStoreBridge *bridge = resolvePageStoreBridge(manager);
        if (bridge) {
            if (m_lockCounter == 0) {
                m_tileData->blockSwapping();
                m_pageStoreWriteIntentOwnsNativePin = true;
            }
            ++m_lockCounter;
            ++m_pageStoreWriteLockCount;
            DEBUG_LOG_ACTION("lock [W/PageStore intent]");
            return;
        }
    }

    blockSwapping();

    /* We are doing COW here */
    if (lazyCopying()) {
        m_COWMutex.lock();

        /**
         * Everything could have happened before we took
         * the mutex, so let's check again...
         */

        if (lazyCopying()) {

            KisTileData *tileData = m_tileData->clone();
            tileData->acquire();
            tileData->blockSwapping();
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
}

void KisTile::unlockForWrite()
{
    {
        QMutexLocker locker(&m_swapBarrierLock);
        if (m_pageStoreWriteLockCount > 0) {
            --m_pageStoreWriteLockCount;
        }
        if (releasePageStoreLeasesLocked()) {
            DEBUG_LOG_ACTION("unlock [W/PageStore]");
            return;
        }
        if (m_pageStoreWriteIntentOwnsNativePin) {
            Q_ASSERT(m_lockCounter > 0);
            --m_lockCounter;
            if (m_lockCounter == 0) {
                m_tileData->unblockSwapping();
                m_pageStoreWriteIntentOwnsNativePin = false;
            }
            DEBUG_LOG_ACTION("unlock [W/PageStore intent]");
            return;
        }
    }
    unblockSwapping();
    DEBUG_LOG_ACTION("unlock [W]");

#ifdef DEAD_TILES_SANITY_CHECK
    m_sanityLockedForWrite.deref();
    KIS_ASSERT(m_sanityLockedForWrite.loadAcquire() >= 0);
#endif
}

void KisTile::unlockForRead() const
{
    {
        QMutexLocker locker(&m_swapBarrierLock);
        if (releasePageStoreLeasesLocked()) {
            DEBUG_LOG_ACTION("unlock [R/PageStore]");
            return;
        }
    }
    unblockSwapping();
    DEBUG_LOG_ACTION("unlock [R]");

#ifdef DEAD_TILES_SANITY_CHECK
    m_sanityLockedForRead.deref();
    KIS_ASSERT(m_sanityLockedForRead.loadAcquire() >= 0);
#endif
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
    lockForRead();
    quint8 *data = this->data();

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

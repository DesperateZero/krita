/*
 *  SPDX-FileCopyrightText: 2009 Dmitry Kazakov <dimula73@gmail.com>
 *  SPDX-FileCopyrightText: 2018 Andrey Kamakin <a.kamakin@icloud.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */


#include "kis_tile_data.h"
#include "kis_tile_data_store.h"

#include <kis_debug.h>
#include <QScopeGuard>
#include <new>

#include <boost/pool/singleton_pool.hpp>
#include "kis_tile_data_store_iterators.h"

// BPP == bytes per pixel
#define TILE_SIZE_4BPP (4 * __TILE_DATA_WIDTH * __TILE_DATA_HEIGHT)
#define TILE_SIZE_8BPP (8 * __TILE_DATA_WIDTH * __TILE_DATA_HEIGHT)

typedef boost::singleton_pool<KisTileData, TILE_SIZE_4BPP, boost::default_user_allocator_new_delete, boost::details::pool::default_mutex, 256, 4096> BoostPool4BPP;
typedef boost::singleton_pool<KisTileData, TILE_SIZE_8BPP, boost::default_user_allocator_new_delete, boost::details::pool::default_mutex, 128, 2048> BoostPool8BPP;

const qint32 KisTileData::WIDTH = __TILE_DATA_WIDTH;
const qint32 KisTileData::HEIGHT = __TILE_DATA_HEIGHT;

SimpleCache KisTileData::m_cache;

SimpleCache::~SimpleCache()
{
    clear();
}

void SimpleCache::clear()
{
    QWriteLocker l(&m_cacheLock);
    quint8 *ptr = 0;

    while (m_4Pool.pop(ptr)) {
        BoostPool4BPP::ordered_free(ptr);
    }

    while (m_8Pool.pop(ptr)) {
        BoostPool8BPP::ordered_free(ptr);
    }

    while (m_16Pool.pop(ptr)) {
        free(ptr);
    }
}


KisTileData::KisTileData(qint32 pixelSize, const quint8 *defPixel, KisTileDataStore *store, bool checkFreeMemory)
    : m_state(NORMAL),
      m_mementoFlag(0),
      m_age(0),
      m_usersCount(0),
      m_refCount(0),
      m_pixelSize(pixelSize),
      m_store(store)
{
    if (checkFreeMemory) {
        m_store->checkFreeMemory();
    }
    m_data = allocateData(m_pixelSize);

    fillWithPixel(defPixel);
}


KisTileData::KisTileData(qint32 pixelSize, const quint8 *source, qsizetype sourceStride, KisTileDataStore *store)
    : m_state(NORMAL), m_mementoFlag(0), m_age(0), m_usersCount(0),
      m_refCount(0), m_pixelSize(pixelSize), m_store(store)
{
    m_store->checkFreeMemory();
    m_data = allocateData(m_pixelSize);
    const qsizetype rowBytes = qsizetype(WIDTH) * m_pixelSize;
    for (int row = 0; row < HEIGHT; ++row)
        memcpy(m_data + row * rowBytes, source + row * sourceStride, size_t(rowBytes));
}

/**
 * Duplicating tiledata
 * + new object loaded in memory
 * + it's unlocked and has refCount==0
 *
 * NOTE: the memory allocated by the pooler for clones is not counted
 * by the store in memoryHardLimit. The pooler has it's own slice of
 * memory and keeps track of the its size itself. So we should be able
 * to disable the memory check with checkFreeMemory, otherwise, there
 * is a deadlock.
 */
KisTileData::KisTileData(const KisTileData& rhs, bool checkFreeMemory)
    : m_state(NORMAL),
      m_mementoFlag(0),
      m_age(0),
      m_usersCount(0),
      m_refCount(0),
      m_pixelSize(rhs.m_pixelSize),
      m_store(rhs.m_store)
{
    if (checkFreeMemory) {
        m_store->checkFreeMemory();
    }
    m_data = allocateData(m_pixelSize);

    memcpy(m_data, rhs.data(), size_t(m_pixelSize) * WIDTH * HEIGHT);
}


KisTileData::~KisTileData()
{
    releaseMemory();
}

void KisTileData::fillWithPixel(const quint8 *defPixel)
{
    quint8 *it = m_data;

    for (int i = 0; i < WIDTH * HEIGHT; i++, it += m_pixelSize) {
        memcpy(it, defPixel, m_pixelSize);
    }
}

void KisTileData::releaseMemory()
{
    if (m_data) {
        freeData(m_data, m_pixelSize);
        m_data = 0;
    }

    KisTileData *clone = 0;
    while (m_clonesStack.pop(clone)) {
        delete clone;
    }

    Q_ASSERT(m_clonesStack.isEmpty());
}

void KisTileData::allocateMemory()
{
    Q_ASSERT(!m_data);
    m_data = allocateData(m_pixelSize);
}

quint8* KisTileData::allocateData(const qint32 pixelSize)
{
    quint8 *ptr = 0;

    if (!m_cache.pop(pixelSize, ptr)) {
        switch (pixelSize) {
        case 4:
            ptr = (quint8*)BoostPool4BPP::ordered_malloc();
            break;
        case 8:
            ptr = (quint8*)BoostPool8BPP::ordered_malloc();
            break;
        default:
            ptr = (quint8*) malloc(size_t(pixelSize) * WIDTH * HEIGHT);
            break;
        }
    }

    if (!ptr) throw std::bad_alloc();
    return ptr;
}

void KisTileData::freeData(quint8* ptr, const qint32 pixelSize)
{
    try {
        if (m_cache.push(pixelSize, ptr)) return;
    } catch (const std::bad_alloc &) {
        // Cache admission is optional, including during destruction.
    }
    switch (pixelSize) {
    case 4:
        BoostPool4BPP::ordered_free(ptr);
        break;
    case 8:
        BoostPool8BPP::ordered_free(ptr);
        break;
    default:
        free(ptr);
        break;
    }
}

void KisTileData::releaseInternalPools()
{
    const int maxCloneCleanupTiles = 100;
    if (KisTileDataStore::instance()->numTilesInMemory() < maxCloneCleanupTiles) {
        try {
            auto *store = KisTileDataStore::instance();
            auto *iter = store->beginIteration();
            const auto finish = qScopeGuard([&] { store->endIteration(iter); });
            while (iter->hasNext()) {
                KisTileData *item = iter->next();
                KisTileData *clone = nullptr;
                while (item->m_clonesStack.pop(clone)) delete clone;
            }
        } catch (const std::bad_alloc &) {
            // Failure to inspect optional clones does not prevent reclaiming
            // cache entries and wholly unused pool blocks below.
        }
    }
    // Native construction can hold an allocated tile before registration.
    // Reclaim only unused blocks; never invalidate such a tile or replace a
    // live backing with a post-purge allocation that can itself be refused.
    try { m_cache.clear(); }
    catch (const std::bad_alloc &) { return; }
    BoostPool4BPP::release_memory();
    BoostPool8BPP::release_memory();
}

/*
 *  SPDX-FileCopyrightText: 2009 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_TILE_DATA_H_
#define KIS_TILE_DATA_H_

/**
 * Some methods of KisTileData have a cyclic dependency
 * to the KisTileDataStore, so we've moved the class
 * declaration to a separate file, that will be included
 * by the store.
 */
#include "kis_tile_data_interface.h"


#include "kis_tile_data_store.h"
#include <limits>


inline quint8* KisTileData::data() const {
        // WARN: be careful - it can be null when swapped out!
        return m_data;
    }

void KisTileData::setData(const quint8 *data) {
    Q_ASSERT(m_data);
    memcpy(m_data, data, m_pixelSize*WIDTH*HEIGHT);
}

inline quint32 KisTileData::pixelSize() const {
    return m_pixelSize;
}

inline bool KisTileData::acquire() {
    /**
     * We need to ensure the clones in the stack are
     * consistent with the data. When we have a single
     * user, the most probably, the clone has already
     * started stinking.
     * So just clean it up.
     */
    if(m_usersCount == 1) {
        KisTileData *clone = 0;
        while(m_clonesStack.pop(clone)) {
            delete clone;
        }
    }

    bool _ref = ref();
    m_usersCount.ref();
    return _ref;
}

inline bool KisTileData::release() {
    m_usersCount.deref();
    bool _ref = deref();
    return _ref;
}

inline bool KisTileData::ref() const {
    return m_refCount.ref();
}

inline bool KisTileData::tryRef() const {
    int count = m_refCount.loadAcquire();
    while (count > 0 && count < std::numeric_limits<int>::max()) {
        if (m_refCount.testAndSetOrdered(count, count + 1)) return true;
        count = m_refCount.loadAcquire();
    }
    return false;
}

inline bool KisTileData::deref() {
    bool _ref;

    if (!(_ref = m_refCount.deref())) {
        m_store->freeTileData(this);
        return 0;
    }
    return _ref;
}

inline KisTileData* KisTileData::clone() {
    return m_store->duplicateTileData(this);
}

inline bool KisTileData::blockSwapping() {
    m_swapLock.lockForRead();
    if(!m_data) {
        m_swapLock.unlock();
        if (!m_store->ensureTileDataLoaded(this)) {
            return false;
        }
    }
    resetAge();
    return true;
}

inline void KisTileData::unblockSwapping() {
    m_swapLock.unlock();
}

inline bool KisTileData::tryBlockSwapping(bool *busy) {
    if (busy) *busy = false;
    if (!m_swapLock.tryLockForRead()) {
        if (busy) *busy = true;
        return false;
    }
    if (!m_data) {
        m_swapLock.unlock();
        return false;
    }
    resetAge();
    return true;
}

inline bool KisTileData::blockSwappingIfResident() {
    m_swapLock.lockForRead();
    if (!m_data) {
        m_swapLock.unlock();
        return false;
    }
    resetAge();
    return true;
}

inline bool KisTileData::isResident() const {
    m_swapLock.lockForRead();
    const bool result = m_data != nullptr;
    m_swapLock.unlock();
    return result;
}

inline KisChunk KisTileData::swapChunk() const {
    return m_swapChunk;
}
inline void KisTileData::setSwapChunk(KisChunk chunk) {
    m_swapChunk = chunk;
}

inline bool KisTileData::mementoed() const {
    return m_mementoFlag;
}
inline void KisTileData::setMementoed(bool value) {
    m_mementoFlag += value ? 1 : -1;
}

inline bool KisTileData::historical() const {
    return mementoed() && numUsers() <= 1;
}

inline int KisTileData::age() const {
    return m_age.loadRelaxed();
}
inline void KisTileData::resetAge() {
    m_age.storeRelaxed(0);
}
inline void KisTileData::markOld() {
    m_age.fetchAndAddRelaxed(1);
}

inline qint32 KisTileData::numUsers() const {
    return m_usersCount;
}

#endif /* KIS_TILE_DATA_H_ */

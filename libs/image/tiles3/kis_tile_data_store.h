/*
 *  SPDX-FileCopyrightText: 2009 Dmitry Kazakov <dimula73@gmail.com>
 *  SPDX-FileCopyrightText: 2018 Andrey Kamakin <a.kamakin@icloud.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_TILE_DATA_STORE_H_
#define KIS_TILE_DATA_STORE_H_

#include "kritaimage_export.h"

#include <QHash>
#include <QMutex>
#include <QReadWriteLock>
#include <QSharedPointer>
#include <QVector>
#include "kis_tile_data_interface.h"

#include "kis_tile_data_pooler.h"
#include "swap/kis_tile_data_swapper.h"
#include "swap/kis_swapped_data_store.h"
#include "3rdparty/lock_free_map/concurrent_map.h"

class KisTileDataStoreIterator;
class KisTileDataStoreReverseIterator;
class KisTileDataStoreClockIterator;
class KisTileDataStoreTestAccess;
struct KisTileDataResidencyState;

class KRITAIMAGE_EXPORT KisTileDataResidencyTransition
{
public:
    virtual ~KisTileDataResidencyTransition() = default;
    virtual void commit(quint64 revision) noexcept = 0;
};

class KRITAIMAGE_EXPORT KisTileDataResidencyObserver
{
public:
    virtual ~KisTileDataResidencyObserver() = default;
    virtual QSharedPointer<KisTileDataResidencyTransition>
        prepareResidencyChange(KisTileData *tileData,
                               const KisTileDataResidencyState &source,
                               bool targetResident,
                               QString *error) = 0;
};

struct KRITAIMAGE_EXPORT KisTileDataResidencyState
{
    bool resident = false;
    quint64 revision = 0;

    bool isValid() const { return revision != 0; }
};

/**
 * Stores tileData objects. When needed compresses them and swaps.
 */
class KRITAIMAGE_EXPORT KisTileDataStore
{
public:
    KisTileDataStore();
    ~KisTileDataStore();
    static KisTileDataStore* instance();

    void debugPrintList();

    struct MemoryStatistics {
        qint64 totalMemorySize;
        qint64 realMemorySize;
        qint64 historicalMemorySize;

        qint64 poolSize;

        qint64 swapSize;
    };

    MemoryStatistics memoryStatistics();
    void tryForceUpdateMemoryStatisticsWhileIdle();

    /**
     * Returns total number of tiles present: in memory
     * or in a swap file
     */
    inline qint32 numTiles() const
    {
        return m_numTiles.loadAcquire() + m_swappedStore.numTiles();
    }

    /**
     * Returns the number of tiles present in memory only
     */
    inline qint32 numTilesInMemory() const
    {
        return m_numTiles.loadAcquire();
    }

    inline void checkFreeMemory()
    {
        m_swapper.checkFreeMemory();
    }

    /**
     * \see m_memoryMetric
     */
    inline qint64 memoryMetric() const
    {
        return m_memoryMetric.loadAcquire();
    }

    KisTileDataStoreIterator* beginIteration();
    void endIteration(KisTileDataStoreIterator* iterator);

    KisTileDataStoreReverseIterator* beginReverseIteration();
    void endIteration(KisTileDataStoreReverseIterator* iterator);

    KisTileDataStoreClockIterator* beginClockIteration();
    void endIteration(KisTileDataStoreClockIterator* iterator);

    inline KisTileData* createDefaultTileData(qint32 pixelSize, const quint8 *defPixel)
    {
        return allocTileData(pixelSize, defPixel);
    }
    // Synchronous borrowed complete rows. Never registers uninitialized pixels;
    // returns independent storage, with the normal allocator/swap lifecycle.
    KisTileData *createTileDataFromRows(qint32 pixelSize, const quint8 *source,
                                      qsizetype sourceStride, qsizetype sourceBytes);

    // Called by The Memento Manager after every commit
    inline void kickPooler()
    {
        m_pooler.kick();

        //FIXME: maybe, rename a function?
        m_swapper.kick();
    }

    /**
     * Try swap out the tile data.
     * It may fail in case the tile is being accessed
     * at the same moment of time.
     */
    // The caller retains td for this call. Observer admission is prepared
    // before the iterator/swap locks and committed only after both are free.
    bool trySwapTileData(KisTileData *td);


    /**
     * WARN: The following three method are only for usage
     * in KisTileData. Do not call them directly!
     */

    KisTileData *duplicateTileData(KisTileData *rhs);
    // Reports whether this call consumed a background clone; logical payload
    // bytes must not be mistaken for a foreground memcpy or hardware traffic.
    KisTileData *duplicateTileData(KisTileData *rhs, bool *precloneHit);
    // Internal native-provider path. The caller already holds an exact
    // source pixel pin for this entire call; do not acquire a nested pin.
    KisTileData *duplicatePinnedTileData(KisTileData *rhs, bool *precloneHit);

    void freeTileData(KisTileData *td);

    /**
     * Ensures that the tile data is totally present in memory
     * and it's swapping is blocked by holding td->m_swapLock
     * in a read mode.
     * PRECONDITIONS: td->m_swapLock is *unlocked*
     *                m_listRWLock is *unlocked*
     * POSTCONDITIONS: td->m_data is in memory and
     *                 td->m_swapLock is locked
     *                 m_listRWLock is unlocked
     */
    // On success returns with td->m_swapLock held for read. On admission
    // failure returns false with no storage lock held and leaves SSD valid.
    bool ensureTileDataLoaded(KisTileData *td);

    void registerTileData(KisTileData *td);
    void unregisterTileData(KisTileData *td);

    // Sparse observers are installed only for PageStore-owned physical
    // payloads. Swap work reports the exact changed tile after releasing its
    // storage lock; ordinary tiles pay no per-object state cost.
    bool registerResidencyObserver(
        KisTileData *td, const QSharedPointer<KisTileDataResidencyObserver> &observer,
        KisTileDataResidencyState *initialState = nullptr);
    void unregisterResidencyObserver(
        KisTileData *td, const QSharedPointer<KisTileDataResidencyObserver> &observer);

private:
    struct ResidencyObservers {
        bool resident = false;
        quint64 revision = 0;
        quint64 observersRevision = 0;
        QVector<QSharedPointer<KisTileDataResidencyObserver>> observers;
    };
    struct PreparedResidencyChange {
        bool valid = false;
        bool sourceResident = false;
        quint64 sourceRevision = 0;
        quint64 observersRevision = 0;
        QVector<QSharedPointer<KisTileDataResidencyObserver>> observers;
        QVector<QSharedPointer<KisTileDataResidencyTransition>> transitions;
    };

    PreparedResidencyChange prepareResidencyChange(KisTileData *td,
                                                   bool targetResident);
    // Called with the tile swap lock held for write.
    bool validateResidencyChangeLocked(
        KisTileData *td, const PreparedResidencyChange &prepared);
    quint64 recordResidencyChangeLocked(KisTileData *td, bool resident);
    static void commitResidencyChange(
        PreparedResidencyChange &prepared, quint64 revision) noexcept;

    KisTileData *allocTileData(qint32 pixelSize, const quint8 *defPixel);
    bool reserveResidentMemory(qint32 pixelSize);
    void releaseResidentMemoryReservation(qint32 pixelSize) noexcept;

    inline void registerTileDataImp(KisTileData *td);
    inline void unregisterTileDataImp(KisTileData *td);
    void freeRegisteredTiles();

    friend class DeadlockyThread;
    friend class KisLowMemoryTests;
    void debugSwapAll();
    void debugClear();

    friend class KisTiledDataManagerTest;
    void testingSuspendPooler();
    void testingResumePooler();
    void testingFailNextSwapIn(KisSwapInFailurePoint point);

    friend class KisTileDataStoreTestAccess;

    friend class KisLowMemoryBenchmark;
    void testingRereadConfig();
private:
    KisTileDataPooler m_pooler;
    KisTileDataSwapper m_swapper;

    friend class KisTileDataStoreTest;
    friend class KisTileDataPoolerTest;
    KisSwappedDataStore m_swappedStore;

    /**
     * This metric is used for computing the volume
     * of memory occupied by tile data objects.
     * metric = num_bytes / (KisTileData::WIDTH * KisTileData::HEIGHT)
     */
    QAtomicInt m_numTiles;
    QAtomicInt m_memoryMetric;
    QAtomicInt m_counter;
    QAtomicInt m_clockIndex;
    ConcurrentMap<int, KisTileData*> m_tileDataMap;
    QReadWriteLock m_iteratorLock;
    QMutex m_residencyObserverLock;
    QHash<KisTileData *, ResidencyObservers> m_residencyObservers;
    QMutex m_residentMemoryAdmissionLock;
    quint64 m_reservedResidentBytes = 0;
    quint64 m_residentHardLimitBytes = 0;
};

template<typename T>
inline T MiB_TO_METRIC(T value)
{
    unsigned long long __MiB = 1ULL << 20;
    return value * (__MiB / (KisTileData::WIDTH * KisTileData::HEIGHT));
}

#endif /* KIS_TILE_DATA_STORE_H_ */

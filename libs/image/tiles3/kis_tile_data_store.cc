/*
 *  SPDX-FileCopyrightText: 2009 Dmitry Kazakov <dimula73@gmail.com>
 *  SPDX-FileCopyrightText: 2018 Andrey Kamakin <a.kamakin@icloud.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// to disable assert when the leak tracker is active
#include "config-memory-leak-tracker.h"

#include <QGlobalStatic>
#include <limits>
#include <utility>
#include <memory>

#include <QScopeGuard>

#include "kis_tile_data_store.h"
#include "kis_tile_data.h"
#include "kis_debug.h"
#include "kis_image_config.h"

#include "kis_tile_data_store_iterators.h"

Q_GLOBAL_STATIC(KisTileDataStore, s_instance)

namespace {
quint64 configuredResidentHardLimitBytes()
{
    const int limitMiB = KisImageConfig(true).tilesHardLimit();
    return limitMiB > 0 ? quint64(limitMiB) << 20 : 0;
}
}

//#define DEBUG_PRECLONE

#ifdef DEBUG_PRECLONE
#include <stdio.h>
#define DEBUG_PRECLONE_ACTION(action, oldTD, newTD) \
    printf("!!! %s:\t\t\t  0x%X -> 0x%X    \t\t!!!\n",  \
           action, (quintptr)oldTD, (quintptr) newTD)
#define DEBUG_FREE_ACTION(td)                   \
    printf("Tile data free'd \t(0x%X)\n", td)
#else
#define DEBUG_PRECLONE_ACTION(action, oldTD, newTD)
#define DEBUG_FREE_ACTION(td)
#endif

#ifdef DEBUG_HIT_MISS
qint64 __preclone_miss = 0;
qint64 __preclone_hit = 0;

qint64 __preclone_miss_user_count = 0;
qint64 __preclone_miss_age = 0;

#define DEBUG_COUNT_PRECLONE_HIT(td) __preclone_hit++
#define DEBUG_COUNT_PRECLONE_MISS(td) __preclone_miss++; __preclone_miss_user_count+=td->numUsers(); __preclone_miss_age+=td->age()
#define DEBUG_REPORT_PRECLONE_EFFICIENCY()                      \
    dbgKrita << "Hits:" << __preclone_hit                       \
             << "of" << __preclone_hit + __preclone_miss        \
             << "("                                             \
             << qreal(__preclone_hit) / (__preclone_hit + __preclone_miss)       \
             << ")"                                             \
             << "miss users" << qreal(__preclone_miss_user_count) / __preclone_miss \
             << "miss age" << qreal(__preclone_miss_age) / __preclone_miss
#else
#define DEBUG_COUNT_PRECLONE_HIT(td)
#define DEBUG_COUNT_PRECLONE_MISS(td)
#define DEBUG_REPORT_PRECLONE_EFFICIENCY()
#endif

KisTileDataStore::KisTileDataStore()
    : m_pooler(this),
      m_swapper(this),
      m_numTiles(0),
      m_memoryMetric(0),
      m_counter(1),
      m_clockIndex(1),
      m_residentHardLimitBytes(configuredResidentHardLimitBytes())
{
    m_pooler.start();
    m_swapper.start();
}

KisTileDataStore::~KisTileDataStore()
{
    m_pooler.terminatePooler();
    m_swapper.terminateSwapper();

    if (numTiles() > 0) {
        errKrita << "Warning: some tiles have leaked:";
        errKrita << "\tTiles in memory:" << numTilesInMemory() << "\n"
                 << "\tTotal tiles:" << numTiles();
    }
}

KisTileDataStore* KisTileDataStore::instance()
{
    return s_instance;
}

KisTileDataStore::MemoryStatistics KisTileDataStore::memoryStatistics()
{
    QReadLocker lock(&m_iteratorLock);

    MemoryStatistics stats;

    const qint64 metricCoeff = qint64(KisTileData::WIDTH) * KisTileData::HEIGHT;

    stats.realMemorySize = m_pooler.lastRealMemoryMetric() * metricCoeff;
    stats.historicalMemorySize = m_pooler.lastHistoricalMemoryMetric() * metricCoeff;
    stats.poolSize = m_pooler.lastPoolMemoryMetric() * metricCoeff;

    stats.totalMemorySize = memoryMetric() * metricCoeff + stats.poolSize;

    stats.swapSize = m_swappedStore.totalSwapMemoryUsed();

    return stats;
}

void KisTileDataStore::tryForceUpdateMemoryStatisticsWhileIdle()
{
    // in case the pooler is disabled, we should force it
    // to update the stats
    if (!m_pooler.isRunning()) {
        m_pooler.forceUpdateMemoryStats();
    }
}

inline void KisTileDataStore::registerTileDataImp(KisTileData *td)
{
    const int index = m_counter.fetchAndAddOrdered(1);
    QSBR::RawPointerAccess access(m_tileDataMap.getGC());
    auto cell = m_tileDataMap.insertOrFind(index);
    registerTileDataInCell(td, index, cell);
}

void KisTileDataStore::registerTileDataInCell(
    KisTileData *td, int index, ConcurrentMap<int, KisTileData*>::Mutator &cell)
{
    td->m_tileNumber = index;
    bool installed = false;
    const auto restoreNumber = qScopeGuard([&] { if (!installed) td->m_tileNumber = -1; });
    // A concurrent migration can still require preparation for ordinary
    // registration. A rejection occurs before the new value is published.
    KisTileData *old = cell.exchangeValue(td);
    KIS_ASSERT(!old); // Registration indices are unique.
    installed = true;
    m_numTiles.ref();
    m_memoryMetric += td->pixelSize();
}

void KisTileDataStore::registerTileData(KisTileData *td)
{
    {
        QReadLocker lock(&m_iteratorLock);
        registerTileDataImp(td);
    }
    m_tileDataMap.getGC().update();
}

inline void KisTileDataStore::unregisterTileDataImp(KisTileData *td)
{
    // make sure that access to the hash table is guarded by GC block
    // (it avoids removal of the referenced cells caused by concurrent
    // migrations)
    QSBR::RawPointerAccess access(m_tileDataMap.getGC());

    if (m_clockIndex == td->m_tileNumber) {
        do {
            m_clockIndex.ref();
        } while (!m_tileDataMap.get(m_clockIndex.loadAcquire()) && m_clockIndex < m_counter);
    }

    int index = td->m_tileNumber;
    m_tileDataMap.erase(index);
    td->m_tileNumber = -1;
    m_numTiles.deref();
    m_memoryMetric -= td->pixelSize();

}

void KisTileDataStore::unregisterTileData(KisTileData *td)
{
    {
        QReadLocker lock(&m_iteratorLock);
        unregisterTileDataImp(td);
    }
    m_tileDataMap.getGC().update();
}

bool KisTileDataStore::reserveResidentMemory(qint32 pixelSize)
{
    if (pixelSize <= 0)
        return false;
    checkFreeMemory();
    const quint64 pagePixels = quint64(KisTileData::WIDTH) * KisTileData::HEIGHT;
    if (quint64(pixelSize) > std::numeric_limits<quint64>::max() / pagePixels)
        return false;
    const quint64 requestedBytes = quint64(pixelSize) * pagePixels;
    for (;;) {
        quint64 excessBytes = 0;
        {
            QMutexLocker locker(&m_residentMemoryAdmissionLock);
            const qint64 metric = memoryMetric();
            if (metric < 0 || quint64(metric) > std::numeric_limits<quint64>::max() / pagePixels)
                return false;
            const quint64 residentBytes = quint64(metric) * pagePixels;
            if (requestedBytes <= m_residentHardLimitBytes
                && residentBytes <= m_residentHardLimitBytes - requestedBytes
                && m_reservedResidentBytes <= m_residentHardLimitBytes
                    - requestedBytes - residentBytes) {
                m_reservedResidentBytes += requestedBytes;
                return true;
            }
            if (requestedBytes > m_residentHardLimitBytes
                || m_reservedResidentBytes > m_residentHardLimitBytes - requestedBytes) {
                return false;
            }
            excessBytes = residentBytes + m_reservedResidentBytes
                + requestedBytes - m_residentHardLimitBytes;
        }
        const qint64 needMetric = qint64((excessBytes + pagePixels - 1) / pagePixels);
        if (m_swapper.tryFreeMemory(needMetric) <= 0)
            return false;
    }
}

void KisTileDataStore::releaseResidentMemoryReservation(qint32 pixelSize) noexcept
{
    if (pixelSize <= 0)
        return;
    const quint64 bytes = quint64(pixelSize) * KisTileData::WIDTH * KisTileData::HEIGHT;
    QMutexLocker locker(&m_residentMemoryAdmissionLock);
    Q_ASSERT(m_reservedResidentBytes >= bytes);
    m_reservedResidentBytes = bytes > m_reservedResidentBytes
        ? 0 : m_reservedResidentBytes - bytes;
}

KisTileData *KisTileDataStore::allocTileData(qint32 pixelSize, const quint8 *defPixel)
{
    if (!reserveResidentMemory(pixelSize))
        return nullptr;
    const auto releaseReservation = qScopeGuard([&] {
        releaseResidentMemoryReservation(pixelSize);
    });
    auto td = std::make_unique<KisTileData>(pixelSize, defPixel, this);
    registerTileData(td.get());
    return td.release();
}

KisTileData *KisTileDataStore::createTileDataFromRows(
    qint32 pixelSize, const quint8 *source, qsizetype sourceStride, qsizetype sourceBytes)
{
    if (!source || pixelSize <= 0 ||
        pixelSize > std::numeric_limits<qint32>::max() / (KisTileData::WIDTH * KisTileData::HEIGHT)) return nullptr;
    const qsizetype rowBytes = qsizetype(KisTileData::WIDTH) * pixelSize;
    if (sourceStride < rowBytes || sourceStride >
        (std::numeric_limits<qsizetype>::max() - rowBytes) / (KisTileData::HEIGHT - 1) ||
        sourceBytes < (KisTileData::HEIGHT - 1) * sourceStride + rowBytes) return nullptr;
    const qsizetype required = (KisTileData::HEIGHT - 1) * sourceStride + rowBytes;
    if (quintptr(source) > std::numeric_limits<quintptr>::max() - quintptr(required - 1)) return nullptr;
    if (!reserveResidentMemory(pixelSize))
        return nullptr;
    const auto releaseReservation = qScopeGuard([&] {
        releaseResidentMemoryReservation(pixelSize);
    });
    auto td = std::unique_ptr<KisTileData>(new KisTileData(pixelSize, source, sourceStride, this));
    registerTileData(td.get());
    return td.release();
}

KisTileData *KisTileDataStore::duplicateTileData(KisTileData *rhs)
{
    return duplicateTileData(rhs, nullptr);
}

KisTileData *KisTileDataStore::duplicateTileData(KisTileData *rhs, bool *precloneHit)
{
    KisTileData *td = 0;

    if (!rhs || !reserveResidentMemory(qint32(rhs->pixelSize())))
        return nullptr;
    const auto releaseReservation = qScopeGuard([&] {
        releaseResidentMemoryReservation(qint32(rhs->pixelSize()));
    });

    if (rhs->m_clonesStack.pop(td)) {
        if (precloneHit) *precloneHit = true;
        DEBUG_PRECLONE_ACTION("+ Pre-clone HIT", rhs, td);
        DEBUG_COUNT_PRECLONE_HIT(rhs);
    } else {
        if (precloneHit) *precloneHit = false;
        if (!rhs->blockSwapping())
            return nullptr;
        const auto releaseSwap = qScopeGuard([&] { rhs->unblockSwapping(); });
        td = new KisTileData(*rhs);
        DEBUG_PRECLONE_ACTION("- Pre-clone #MISS#", rhs, td);
        DEBUG_COUNT_PRECLONE_MISS(rhs);
    }

    std::unique_ptr<KisTileData> candidate(td);
    registerTileData(candidate.get());
    return candidate.release();
}

KisTileData *KisTileDataStore::duplicatePinnedTileData(KisTileData *rhs, bool *precloneHit)
{
    Q_ASSERT(rhs && rhs->data());
    if (!rhs || !reserveResidentMemory(qint32(rhs->pixelSize())))
        return nullptr;
    const auto releaseReservation = qScopeGuard([&] {
        releaseResidentMemoryReservation(qint32(rhs->pixelSize()));
    });
    KisTileData *td = nullptr;
    if (rhs->m_clonesStack.pop(td)) {
        if (precloneHit) *precloneHit = true;
        DEBUG_PRECLONE_ACTION("+ Pre-clone HIT", rhs, td);
        DEBUG_COUNT_PRECLONE_HIT(rhs);
    } else {
        if (precloneHit) *precloneHit = false;
        td = new KisTileData(*rhs);
        DEBUG_PRECLONE_ACTION("- Pre-clone #MISS#", rhs, td);
        DEBUG_COUNT_PRECLONE_MISS(rhs);
    }
    std::unique_ptr<KisTileData> candidate(td);
    registerTileData(candidate.get());
    return candidate.release();
}

bool KisTileDataStore::tryClaimBackingHandoff(KisTileData *td)
{
    if (!td || td->m_store != this || !m_iteratorLock.tryLockForRead()) return false;
    if (!td->m_swapLock.tryLockForWrite()) {
        m_iteratorLock.unlock();
        return false;
    }
    // The binding owns one ref. Extra lifetime holders (including aliases,
    // legacy caches and immutable sources) conservatively exclude transfer.
    // Ref count is not permission: both physical barriers are already held.
    if (!td->m_data || td->m_refCount.loadAcquire() != 1 || td->m_usersCount.loadAcquire() != 0 ||
        td->m_mementoFlag || td->m_clonesStack.size()) {
        td->m_swapLock.unlock();
        m_iteratorLock.unlock();
        return false;
    }
    return true;
}

void KisTileDataStore::finishBackingHandoff(KisTileData *td, bool writable) noexcept
{
    Q_ASSERT(td && td->m_store == this && td->m_data);
    td->m_swapLock.unlock();
    // Qt has no atomic write-to-read downgrade. The iterator barrier still
    // excludes the swapper/pooler here; no external lifetime holder existed
    // at claim. Binding consumers remain excluded until this pin is ready.
    if (writable) td->m_swapLock.lockForRead();
    m_iteratorLock.unlock();
}

void KisTileDataStore::freeTileData(KisTileData *td)
{
    Q_ASSERT(td->m_store == this);

    DEBUG_FREE_ACTION(td);

    m_iteratorLock.lockForRead();
    td->m_swapLock.lockForWrite();

    if (!td->data()) {
        m_swappedStore.forgetTileData(td);
    } else {
        unregisterTileDataImp(td);
    }

    td->m_swapLock.unlock();
    m_iteratorLock.unlock();
    m_tileDataMap.getGC().update();

    {
        QMutexLocker locker(&m_residencyObserverLock);
        m_residencyObservers.remove(td);
    }

    delete td;
}

bool KisTileDataStore::registerResidencyObserver(
    KisTileData *td, const QSharedPointer<KisTileDataResidencyObserver> &observer,
    KisTileDataResidencyState *initialState)
{
    if (!td || !observer)
        return false;
    // Match the transition lock order: swap state first, sparse observer
    // record second. This makes the initial domain/revision one observation.
    QReadLocker swapLocker(&td->m_swapLock);
    QMutexLocker observerLocker(&m_residencyObserverLock);
    const bool resident = td->data() != nullptr;
    auto found = m_residencyObservers.find(td);
    if (found == m_residencyObservers.end()) {
        ResidencyObservers record;
        record.resident = resident;
        record.revision = 1;
        record.observersRevision = 1;
        found = m_residencyObservers.insert(td, std::move(record));
    } else {
        Q_ASSERT(found->revision != 0);
        Q_ASSERT(found->resident == resident);
    }
    if (!found->observers.contains(observer)) {
        found->observers.append(observer);
        ++found->observersRevision;
        if (!found->observersRevision)
            ++found->observersRevision;
    }
    if (initialState)
        *initialState = {found->resident, found->revision};
    return true;
}

void KisTileDataStore::unregisterResidencyObserver(
    KisTileData *td, const QSharedPointer<KisTileDataResidencyObserver> &observer)
{
    QMutexLocker locker(&m_residencyObserverLock);
    auto existing = m_residencyObservers.find(td);
    if (existing == m_residencyObservers.end())
        return;
    if (existing->observers.removeAll(observer)) {
        ++existing->observersRevision;
        if (!existing->observersRevision)
            ++existing->observersRevision;
    }
    if (existing->observers.isEmpty())
        m_residencyObservers.erase(existing);
}

KisTileDataStore::PreparedResidencyChange
KisTileDataStore::prepareResidencyChange(KisTileData *td, bool targetResident)
{
    PreparedResidencyChange result;
    if (!td)
        return result;
    {
        QReadLocker swapLocker(&td->m_swapLock);
        result.sourceResident = td->data() != nullptr;
        if (result.sourceResident == targetResident)
            return result;
        QMutexLocker observerLocker(&m_residencyObserverLock);
        const auto found = m_residencyObservers.constFind(td);
        if (found == m_residencyObservers.cend()) {
            result.valid = true;
            return result;
        }
        if (found->resident != result.sourceResident || !found->revision
            || !found->observersRevision) {
            return {};
        }
        result.sourceRevision = found->revision;
        result.observersRevision = found->observersRevision;
        result.observers = found->observers;
    }

    const KisTileDataResidencyState source{
        result.sourceResident, result.sourceRevision};
    result.transitions.reserve(result.observers.size());
    for (const auto &observer : std::as_const(result.observers)) {
        QString error;
        auto transition = observer->prepareResidencyChange(
            td, source, targetResident, &error);
        if (!transition) {
            result.transitions.clear();
            result.valid = false;
            return result;
        }
        result.transitions.append(std::move(transition));
    }
    result.valid = true;
    return result;
}

bool KisTileDataStore::validateResidencyChangeLocked(
    KisTileData *td, const PreparedResidencyChange &prepared)
{
    if (!prepared.valid || (td->data() != nullptr) != prepared.sourceResident)
        return false;
    QMutexLocker locker(&m_residencyObserverLock);
    const auto found = m_residencyObservers.constFind(td);
    if (!prepared.observersRevision)
        return found == m_residencyObservers.cend();
    return found != m_residencyObservers.cend()
        && found->resident == prepared.sourceResident
        && found->revision == prepared.sourceRevision
        && found->observersRevision == prepared.observersRevision;
}

quint64 KisTileDataStore::recordResidencyChangeLocked(KisTileData *td,
                                                      bool resident)
{
    QMutexLocker locker(&m_residencyObserverLock);
    const auto found = m_residencyObservers.find(td);
    if (found == m_residencyObservers.end())
        return 0;
    Q_ASSERT(found->revision != 0);
    Q_ASSERT(found->resident != resident);
    ++found->revision;
    if (!found->revision)
        ++found->revision;
    found->resident = resident;
    return found->revision;
}

void KisTileDataStore::commitResidencyChange(
    PreparedResidencyChange &prepared, quint64 revision) noexcept
{
    for (const auto &transition : std::as_const(prepared.transitions))
        transition->commit(revision);
    prepared.transitions.clear();
}

bool KisTileDataStore::ensureTileDataLoaded(KisTileData *td)
{
//    dbgKrita << "#### SWAP MISS! ####" << td << ppVar(td->mementoed()) << ppVar(td->age()) << ppVar(td->numUsers());
    checkFreeMemory();

    constexpr int maximumAttempts = 4;
    for (int attempt = 0; attempt < maximumAttempts; ++attempt) {
        td->m_swapLock.lockForRead();
        if (td->data())
            return true;
        td->m_swapLock.unlock();

        PreparedResidencyChange prepared = prepareResidencyChange(td, true);
        if (!prepared.valid)
            return false;
        if (!reserveResidentMemory(qint32(td->pixelSize()))) {
            prepared.transitions.clear();
            return false;
        }
        const auto releaseReservation = qScopeGuard([&] {
            releaseResidentMemoryReservation(qint32(td->pixelSize()));
        });

        /**
         * The order of this heavy locking is very important.
         * Change it only in case, you really know what you are doing.
         */
        QWriteLocker iteratorLocker(&m_iteratorLock);

        /**
         * If someone has managed to load the td from swap, then, most
         * probably, they have already taken the swap lock. This may
         * lead to a deadlock, because COW mechanism breaks lock
         * ordering rules in duplicateTileData() (it takes m_listLock
         * while the swap lock is held). In our case it is enough just
         * to check whether the other thread has already fetched the
         * data. Please notice that we do not take both of the locks
         * while checking this, because holding m_listLock is
         * enough. Nothing can happen to the tile while we hold
         * m_listLock.
         */

        bool loaded = false;
        bool stale = false;
        bool attemptedLoad = false;
        quint64 revision = 0;
        if (!td->data()) {
            QWriteLocker swapLocker(&td->m_swapLock);
            if (!validateResidencyChangeLocked(td, prepared)) {
                stale = true;
            } else try {
                // Prepare the real map cell while both original gates exclude
                // registration/migration. Loading may consume the swap record;
                // no map allocation may follow that physical transition.
                const int index = m_counter.fetchAndAddOrdered(1);
                QSBR::RawPointerAccess access(m_tileDataMap.getGC());
                auto cell = m_tileDataMap.insertOrFind(index);
                attemptedLoad = true;
                loaded = m_swappedStore.swapInTileData(td);
                if (loaded) {
                    registerTileDataInCell(td, index, cell);
                    revision = recordResidencyChangeLocked(td, true);
                }
            } catch (const std::bad_alloc &) {
                // Preserve the bool admission contract. Preparation precedes
                // swap-in; both locks and the reservation unwind on rejection.
                return false;
            }
        }

        iteratorLocker.unlock();
        m_tileDataMap.getGC().update();

        if (loaded)
            commitResidencyChange(prepared, revision);
        else
            prepared.transitions.clear();

        if (stale)
            continue;
        if (attemptedLoad && !loaded)
            return false;

        /**
         * <-- In theory, livelock is possible here...
         */

        td->m_swapLock.lockForRead();
        if (td->data())
            return true;
        td->m_swapLock.unlock();
    }
    return false;
}

bool KisTileDataStore::trySwapTileData(KisTileData *td)
{
    constexpr int maximumAttempts = 4;
    for (int attempt = 0; attempt < maximumAttempts; ++attempt) {
        PreparedResidencyChange prepared = prepareResidencyChange(td, false);
        if (!prepared.valid)
            return false;

        bool result = false;
        bool stale = false;
        quint64 revision = 0;
        m_iteratorLock.lockForWrite();
        if (!td->m_swapLock.tryLockForWrite()) {
            m_iteratorLock.unlock();
            return false;
        }
        if (!validateResidencyChangeLocked(td, prepared)) {
            stale = true;
        } else if (td->data() && m_swappedStore.trySwapOutTileData(td)) {
            unregisterTileDataImp(td);
            result = true;
            revision = recordResidencyChangeLocked(td, false);
        }
        td->m_swapLock.unlock();
        m_iteratorLock.unlock();
        m_tileDataMap.getGC().update();

        if (result) {
            commitResidencyChange(prepared, revision);
            return true;
        }
        prepared.transitions.clear();
        if (!stale)
            return false;
    }
    return false;
}

KisTileDataStoreIterator* KisTileDataStore::beginIteration()
{
    m_iteratorLock.lockForWrite();
    return new KisTileDataStoreIterator(m_tileDataMap);
}
void KisTileDataStore::endIteration(KisTileDataStoreIterator* iterator)
{
    delete iterator;
    m_iteratorLock.unlock();
}

KisTileDataStoreReverseIterator* KisTileDataStore::beginReverseIteration()
{
    m_iteratorLock.lockForWrite();
    return new KisTileDataStoreReverseIterator(m_tileDataMap);
}
void KisTileDataStore::endIteration(KisTileDataStoreReverseIterator* iterator)
{
    delete iterator;
    m_iteratorLock.unlock();
    DEBUG_REPORT_PRECLONE_EFFICIENCY();
}

KisTileDataStoreClockIterator* KisTileDataStore::beginClockIteration()
{
    m_iteratorLock.lockForWrite();
    return new KisTileDataStoreClockIterator(m_tileDataMap, m_clockIndex.loadAcquire());
}

void KisTileDataStore::endIteration(KisTileDataStoreClockIterator* iterator)
{
    m_clockIndex = iterator->getFinalPosition();
    delete iterator;
    m_iteratorLock.unlock();
}

void KisTileDataStore::debugPrintList()
{
    KisTileDataStoreIterator* iter = beginIteration();
    KisTileData *item = 0;

    while (iter->hasNext()) {
        item = iter->next();
        dbgTiles << "-------------------------\n"
                 << "TileData:\t\t\t" << item
                 << "\n  refCount:\t" << item->m_refCount;
    }

    endIteration(iter);
}

void KisTileDataStore::debugSwapAll()
{
    KisTileDataStoreIterator* iter = beginIteration();
    QVector<KisTileData *> items;

    while (iter->hasNext()) {
        KisTileData *item = iter->next();
        if (item->ref())
            items.append(item);
    }
    endIteration(iter);
    for (KisTileData *item : std::as_const(items)) {
        trySwapTileData(item);
        item->deref();
    }
}

void KisTileDataStore::debugClear()
{
    QWriteLocker l(&m_iteratorLock);
    ConcurrentMap<int, KisTileData*>::Iterator iter(m_tileDataMap);

    while (iter.isValid()) {
        delete iter.getValue();
        iter.next();
    }

    m_counter = 1;
    m_clockIndex = 1;
    m_numTiles = 0;
    m_memoryMetric = 0;
}

void KisTileDataStore::testingRereadConfig()
{
    {
        QMutexLocker locker(&m_residentMemoryAdmissionLock);
        m_residentHardLimitBytes = configuredResidentHardLimitBytes();
    }
    m_pooler.testingRereadConfig();
    m_swapper.testingRereadConfig();
    kickPooler();
}

void KisTileDataStore::testingSuspendPooler()
{
    m_pooler.terminatePooler();
}

void KisTileDataStore::testingResumePooler()
{
    m_pooler.start();
}

void KisTileDataStore::testingFailNextSwapIn(
    KisSwapInFailurePoint point)
{
    m_swappedStore.testingFailNextSwapIn(point);
}

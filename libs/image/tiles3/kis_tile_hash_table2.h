/*
 *  SPDX-FileCopyrightText: 2018 Andrey Kamakin <a.kamakin@icloud.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_TILEHASHTABLE_2_H
#define KIS_TILEHASHTABLE_2_H

#include "kis_shared.h"
#include "kis_shared_ptr.h"
#include "3rdparty/lock_free_map/concurrent_map.h"
#include "kis_tile.h"
#include "kis_debug.h"
#include <vector>
#include <memory>
#include <new>

#define SANITY_CHECK

/**
 * This is a  template for a hash table that stores  tiles (or some other
 * objects  resembling tiles).   Actually, this  object should  only have
 * col()/row() methods and be able to answer notifyDetachedFromDataManager() requests to
 * be   stored   here.    It   is   used   in   KisTiledDataManager   and
 * KisMementoManager.
 *
 * How to use:
 *   1) each hash must be unique, otherwise tiles would rewrite each-other
 *   2) 0 key is reserved, so can't be used
 *   3) col and row must be less than 0x7FFF to guarantee uniqueness of hash for each pair
 */

template <class T>
class KisTileHashTableIteratorTraits2;

template <class T>
class KisTileHashTableTraits2
{
    static constexpr bool isInherited = std::is_convertible<T*, KisShared*>::value;
    Q_STATIC_ASSERT_X(isInherited, "Template must inherit KisShared");

public:
    typedef T TileType;
    typedef KisSharedPtr<T> TileTypeSP;
    typedef KisWeakSharedPtr<T> TileTypeWSP;

    KisTileHashTableTraits2(KisMementoManager *mm);
    KisTileHashTableTraits2(const KisTileHashTableTraits2<T> &ht, KisMementoManager *mm);
    ~KisTileHashTableTraits2();

    bool isEmpty()
    {
        return !m_numTiles.loadRelaxed();
    }

    bool tileExists(qint32 col, qint32 row);
    // The packed key reserves zero and the (0x7fff, 0x7fff) origin escape.
    // Let bulk entry points reject unsupported ranges before opening a write
    // transaction instead of silently using detached, unindexed tiles.
    static bool supportsCoordinates(qint32 col, qint32 row)
    {
        return col > -0x7fff && col < 0x7fff && row > -0x7fff && row < 0x7fff;
    }

    /**
     * Returns a tile in position (col,row). If no tile exists,
     * returns null.
     * \param col column of the tile
     * \param row row of the tile
     */
    TileTypeSP getExistingTile(qint32 col, qint32 row);

    /**
     * Returns a tile in position (col,row). If no tile exists,
     * creates a new one, attaches it to the list and returns.
     * \param col column of the tile
     * \param row row of the tile
     * \param newTile out-parameter, returns true if a new tile
     *                was created
     */
    TileTypeSP getTileLazy(qint32 col, qint32 row, bool& newTile);

    // Storage for a missing wrapper only. An existing tile needs no key
    // record. Caller must own the original page operation through delivery.
    class PreparedTile;
    PreparedTile prepareMissingTile(qint32 col, qint32 row);
    TileTypeSP installPreparedTile(PreparedTile &prepared, bool &newTile);
    // Delivery must not run the collector or participate in map growth.
    TileTypeSP getExistingTileForPreparedUpdate(qint32 col, qint32 row);


    /**
     * Returns a tile in position (col,row). If no tile exists,
     * creates nothing, but returns shared default tile object
     * of the table. Be careful, this object has column and row
     * parameters set to (qint32_MIN, qint32_MIN).
     * \param col column of the tile
     * \param row row of the tile
     * \param existingTile returns true if the tile actually exists in the table
     *                     and it is not a lazily created default wrapper tile
     */
    TileTypeSP getReadOnlyTileLazy(qint32 col, qint32 row, bool &existingTile);
    void addTile(TileTypeSP tile);
    bool deleteTile(TileTypeSP tile);
    bool deleteTile(qint32 col, qint32 row);

    void clear();

    void setDefaultTileData(KisTileData *defaultTileData);
    KisTileData* defaultTileData();

    /**
     * Returns a pointer to the default tile data object with ref counter
     * increased by one. Make sure you call deref() after you finished using
     * this object.
     */
    KisTileData* refAndFetchDefaultTileData();


    qint32 numTiles()
    {
        return m_numTiles.loadRelaxed();
    }

    void debugPrintInfo();
    void debugMaxListLength(qint32 &min, qint32 &max);

    friend class KisTileHashTableIteratorTraits2<T>;
    friend class KisLocklessStackTest;

private:
    // A stable identity for the one reference held by the map/retirement
    // record. The leak tracker sees the same owner when this reference moves.
    static const TileTypeSP *mapReferenceOwner(TileType *tile)
    {
        return reinterpret_cast<const TileTypeSP *>(tile);
    }

    static void releaseMapReference(TileType *tile)
    {
        TileTypeSP::deref(mapReferenceOwner(tile), tile);
    }

    using MapReference = std::unique_ptr<TileType, decltype(&releaseMapReference)>;
    typedef ConcurrentMap<quint32, TileType*, DefaultKeyTraits<quint32>, DefaultValueTraits<TileType*>, true> LockFreeTileMap;
    typedef typename LockFreeTileMap::Mutator LockFreeTileMapMutator;

public:
    class PreparedTile {
    public:
        PreparedTile() = default;
        PreparedTile(PreparedTile &&) = default;
        PreparedTile &operator=(PreparedTile &&) = default;
        PreparedTile(const PreparedTile &) = delete;
        PreparedTile &operator=(const PreparedTile &) = delete;
        explicit operator bool() const { return m_tile && bool(m_key) && !m_consumed; }
        qint32 col() const { return m_tile->col(); }
        qint32 row() const { return m_tile->row(); }
    private:
        friend class KisTileHashTableTraits2<T>;
        bool m_consumed = false;
        TileTypeSP m_tile;
        MapReference m_reference{nullptr, &releaseMapReference};
        typename LockFreeTileMap::PreparedKey m_key;
    };

private:


    static MapReference prepareMapReference(TileType *tile)
    {
        TileTypeSP::ref(mapReferenceOwner(tile), tile);
        return MapReference(tile, &releaseMapReference);
    }

    void retire(TileType *tile, QSBR::PreparedAction action) noexcept
    {
        action->bind(&releaseMapReference, tile);
        m_map.getGC().enqueuePrepared(std::move(action));
    }

    inline quint32 calculateHashImpl(qint32 col, qint32 row)
    {
        if (col == 0 && row == 0) {
            col = 0x7FFF;
            row = 0x7FFF;
        }

        return ((static_cast<quint32>(row) << 16) | (static_cast<quint32>(col) & 0xFFFF));
    }

    inline quint32 calculateHash(qint32 col, qint32 row)
    {
#ifdef SANITY_CHECK
        KIS_ASSERT_RECOVER_NOOP(supportsCoordinates(col, row));
#endif // SANITY_CHECK

        return calculateHashImpl(col, row);
    }

    /**
     * A version of the hash function that returns an invalid hash in
     * case the requested tile is out of range
     */
    inline quint32 calculateHashSafe(qint32 col, qint32 row)
    {
        KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(supportsCoordinates(col, row), 0);
        return calculateHashImpl(col, row);
    }

    inline void insert(quint32 idx, TileTypeSP item)
    {
        // Both the actual retirement node and the candidate's map reference
        // exist before assign can remove the previous map reference.
        auto retirement = QSBR::prepare();
        auto candidate = prepareMapReference(item.data());
        {
            QReadLocker locker(&m_iteratorLock);
            QSBR::RawPointerAccess access(m_map.getGC());
            auto mutator = m_map.insertOrFind(idx);
            TileType *old = mutator.exchangeValue(item.data());
            if (old == item.data()) {
                // A losing private candidate needs no grace period. A true
                // self-assignment must not detach the value still in the map.
                if (mutator.getValue() != item.data()) item->notifyDeadWithoutDetaching();
            } else {
                candidate.release();
                if (old) {
                    retire(old, std::move(retirement));
                    old->notifyDeadWithoutDetaching();
                } else {
                    m_numTiles.fetchAndAddRelaxed(1);
                }
            }
        }
        m_map.getGC().update();
    }

    // Caller owns iterator read access, or an iterator's exclusive access.
    // No update here: a collector must not wait for readers under that gate.
    inline bool erasePrepared(quint32 idx, QSBR::PreparedAction retirement)
    {
        QSBR::RawPointerAccess access(m_map.getGC());
        TileType *tile = m_map.erase(idx);
        if (!tile) return false;
        m_numTiles.fetchAndSubRelaxed(1);
        retire(tile, std::move(retirement));
        // The queued reference is protected by access even if notification
        // throws. No removed map reference can be lost on that path.
        tile->notifyDetachedFromDataManager();
        return true;
    }

    inline bool erase(quint32 idx)
    {
        return erasePrepared(idx, QSBR::prepare());
    }

private:
    mutable LockFreeTileMap m_map;

    /**
     * We still need something to guard changes in m_defaultTileData,
     * otherwise there will be concurrent read/writes, resulting in broken memory.
     */
    QReadWriteLock m_defaultPixelDataLock;
    mutable QReadWriteLock m_iteratorLock;

    QAtomicInt m_numTiles;
    KisTileData *m_defaultTileData;
    KisMementoManager *m_mementoManager;
};

template <class T>
class KisTileHashTableIteratorTraits2
{
public:
    typedef T TileType;
    typedef KisSharedPtr<T> TileTypeSP;
    typedef typename KisTileHashTableTraits2<T>::LockFreeTileMap::Iterator Iterator;

    KisTileHashTableIteratorTraits2(KisTileHashTableTraits2<T> *ht) : m_ht(ht)
    {
        m_ht->m_iteratorLock.lockForWrite();
        m_iter.setMap(m_ht->m_map);
    }

    ~KisTileHashTableIteratorTraits2()
    {
        m_ht->m_iteratorLock.unlock();
        m_ht->m_map.getGC().update();
    }

    void next()
    {
        m_iter.next();
    }

    TileTypeSP tile() const
    {
        return TileTypeSP(m_iter.getValue());
    }

    bool isDone() const
    {
        return !m_iter.isValid();
    }

    void deleteCurrent()
    {
        m_ht->erase(m_iter.getKey());
        next();
    }

    void moveCurrentToHashTable(KisTileHashTableTraits2<T> *newHashTable)
    {
        TileTypeSP tile = m_iter.getValue();
        next();

        quint32 idx = m_ht->calculateHash(tile->col(), tile->row());
        m_ht->erase(idx);
        newHashTable->insert(idx, tile);
    }

private:
    KisTileHashTableTraits2<T> *m_ht;
    Iterator m_iter;
};

template <class T>
KisTileHashTableTraits2<T>::KisTileHashTableTraits2(KisMementoManager *mm)
    : m_numTiles(0), m_defaultTileData(0), m_mementoManager(mm)
{
}

template <class T>
KisTileHashTableTraits2<T>::KisTileHashTableTraits2(const KisTileHashTableTraits2<T> &ht, KisMementoManager *mm)
    : KisTileHashTableTraits2(mm)
{
    setDefaultTileData(ht.m_defaultTileData);

    QWriteLocker locker(&ht.m_iteratorLock);
    typename LockFreeTileMap::Iterator iter(ht.m_map);

    while (iter.isValid()) {
        TileTypeSP tile = new TileType(*iter.getValue(), m_mementoManager);
        insert(iter.getKey(), tile);
        iter.next();
    }
}

template <class T>
KisTileHashTableTraits2<T>::~KisTileHashTableTraits2()
{
    // Storage tokens may outlive this table. Close them before draining tile
    // references, so a late install cannot repopulate the dying adapter.
    m_map.closePreparedKeys();
    // Owner destruction requires all map operations/iterators to have left.
    // External TileSP values may survive, but raw map access may not.
    Q_ASSERT(!m_map.getGC().sanityRawPointerAccessLocked());
    m_map.getGC().flush();
    typename LockFreeTileMap::Iterator iter(m_map);
    while (iter.isValid()) {
        MapReference tile(m_map.erase(iter.getKey()), &releaseMapReference);
        if (tile) {
            m_numTiles.fetchAndSubRelaxed(1);
            tile->notifyDetachedFromDataManager();
        }
        iter.next();
    }
    setDefaultTileData(0);
}

template<class T>
bool KisTileHashTableTraits2<T>::tileExists(qint32 col, qint32 row)
{
    return getExistingTile(col, row);
}

template <class T>
typename KisTileHashTableTraits2<T>::TileTypeSP KisTileHashTableTraits2<T>::getExistingTile(qint32 col, qint32 row)
{
    const quint32 idx = calculateHashSafe(col, row);
    if (!idx) {
        /// a tile with invalid index obviously doesn't exist
        return TileTypeSP();
    }

    TileTypeSP tile;
    {
        QSBR::RawPointerAccess access(m_map.getGC());
        tile = m_map.get(idx);
    }

    m_map.getGC().update();
    return tile;
}

template <class T>
typename KisTileHashTableTraits2<T>::TileTypeSP KisTileHashTableTraits2<T>::getTileLazy(qint32 col, qint32 row, bool &newTile)
{
    newTile = false;
    const quint32 idx = calculateHashSafe(col, row);
    if (!idx) {
        /// when invalid tile index is requested, just return a
        /// detached tile with the default data

        /// we pretend as if this tile has already existed, it will
        /// allow the calling code to avoid modifying the extent
        /// manager
        newTile = false;

        QReadLocker locker(&m_defaultPixelDataLock);
        return new TileType(col, row, m_defaultTileData, 0);
    }

    TileTypeSP tile;
    {
        QSBR::RawPointerAccess access(m_map.getGC());
        tile = m_map.get(idx);
    }
    if (!tile) {
        TileTypeSP candidate;
        {
            QReadLocker locker(&m_defaultPixelDataLock);
            candidate = new TileType(col, row, m_defaultTileData, 0);
        }
        auto reference = prepareMapReference(candidate.data());
        {
            QReadLocker locker(&m_iteratorLock);
            QSBR::RawPointerAccess access(m_map.getGC());
            auto mutator = m_map.insertOrFind(idx);
            TileType *winner = mutator.insertIfAbsentValue(candidate.data());
            if (winner) {
                tile = winner;
                candidate->notifyDeadWithoutDetaching();
            } else {
                reference.release();
                tile = candidate;
                newTile = true;
                m_numTiles.fetchAndAddRelaxed(1);
                tile->notifyAttachedToDataManager(m_mementoManager);
            }
        }
        // An uninstalled candidate and its extra reference die outside gates;
        // insertIfAbsentValue proves that no raw map reader ever saw it.
    }
    m_map.getGC().update();
    return tile;
}

template <class T>
typename KisTileHashTableTraits2<T>::TileTypeSP
KisTileHashTableTraits2<T>::getExistingTileForPreparedUpdate(qint32 col, qint32 row)
{
    const quint32 idx = calculateHashSafe(col, row);
    if (!idx) return {};
    QSBR::RawPointerAccess access(m_map.getGC());
    return TileTypeSP(m_map.get(idx));
}

template <class T>
typename KisTileHashTableTraits2<T>::PreparedTile
KisTileHashTableTraits2<T>::prepareMissingTile(qint32 col, qint32 row)
{
    PreparedTile prepared;
    if (getExistingTileForPreparedUpdate(col, row)) return prepared;
    const quint32 idx = calculateHashSafe(col, row);
    if (!idx) throw std::bad_alloc();
    {
        QReadLocker locker(&m_defaultPixelDataLock);
        // No attachment or memento registration until actual installation.
        prepared.m_tile = new TileType(col, row, m_defaultTileData, nullptr);
    }
    prepared.m_reference = prepareMapReference(prepared.m_tile.data());
    prepared.m_key = m_map.prepareKey(idx);
    if (!prepared.m_key) throw std::bad_alloc();
    return prepared;
}

template <class T>
typename KisTileHashTableTraits2<T>::TileTypeSP
KisTileHashTableTraits2<T>::installPreparedTile(PreparedTile &prepared, bool &newTile)
{
    newTile = false;
    if (!prepared || !prepared.m_reference || !prepared.m_key.belongsTo(m_map)) return {};
    QReadLocker locker(&m_iteratorLock);
    QSBR::RawPointerAccess access(m_map.getGC());
    const auto result = prepared.m_key.insertIfAbsentValue(prepared.m_tile.data());
    if (!result.accepted) return {};
    prepared.m_consumed = true;
    if (result.previous) return TileTypeSP(result.previous);
    prepared.m_reference.release();
    m_numTiles.fetchAndAddRelaxed(1);
    prepared.m_tile->notifyAttachedToDataManager(m_mementoManager);
    newTile = true;
    // The caller retains the candidate and key until outside its install
    // gates. No collector update or candidate destruction occurs here.
    return prepared.m_tile;
}

template <class T>
typename KisTileHashTableTraits2<T>::TileTypeSP KisTileHashTableTraits2<T>::getReadOnlyTileLazy(qint32 col, qint32 row, bool &existingTile)
{
    const quint32 idx = calculateHashSafe(col, row);
    if (!idx) {
        /// when invalid tile index is requested, just return a
        /// detached tile with the default data

        /// we pretend as if this tile hasn't existed, it will
        /// allow the calling code to avoid modifying the extent
        /// manager (note, that is opposite to what happens in
        /// getTileLazy())
        existingTile = false;

        QReadLocker locker(&m_defaultPixelDataLock);
        return new TileType(col, row, m_defaultTileData, 0);
    }

    TileTypeSP tile;
    {
        QSBR::RawPointerAccess access(m_map.getGC());
        tile = m_map.get(idx);
    }

    existingTile = tile;

    if (!existingTile) {
        QReadLocker locker(&m_defaultPixelDataLock);
        tile = new TileType(col, row, m_defaultTileData, 0);
    }

    m_map.getGC().update();
    return tile;
}

template <class T>
void KisTileHashTableTraits2<T>::addTile(TileTypeSP tile)
{
    quint32 idx = calculateHash(tile->col(), tile->row());
    insert(idx, tile);
}

template <class T>
bool KisTileHashTableTraits2<T>::deleteTile(TileTypeSP tile)
{
    return deleteTile(tile->col(), tile->row());
}

template <class T>
bool KisTileHashTableTraits2<T>::deleteTile(qint32 col, qint32 row)
{
    const quint32 idx = calculateHashSafe(col, row);
    if (!idx) {
        /// when invalid tile index is requested, just do nothing
        return false;
    }

    auto retirement = QSBR::prepare();
    bool result;
    {
        QReadLocker locker(&m_iteratorLock);
        result = erasePrepared(idx, std::move(retirement));
    }
    m_map.getGC().update();
    return result;
}

template<class T>
void KisTileHashTableTraits2<T>::clear()
{
    // Prepare all actual queue nodes before erasing anything. Mutations and
    // their counts share iterator exclusion, so the locked recheck is exact.
    for (int attempt = 0; attempt < 3; ++attempt) {
        // Read-lock holders may publish/erase before updating their count.
        // The estimate can transiently be negative; only the write-locked
        // count below is a complete snapshot of all those mutations.
        const int count = qMax(0, m_numTiles.loadAcquire());
        std::vector<QSBR::PreparedAction> retirement(count);
        for (auto &action : retirement) action = QSBR::prepare();
        {
            QWriteLocker locker(&m_iteratorLock);
            if (m_numTiles.loadRelaxed() > count) continue;
            typename LockFreeTileMap::Iterator iter(m_map);
            size_t next = 0;
            while (iter.isValid()) {
                erasePrepared(iter.getKey(), std::move(retirement[next++]));
                iter.next();
            }
            Q_ASSERT(!m_numTiles.loadRelaxed());
        }
        m_map.getGC().update();
        return;
    }
    // No cell has been changed by this call. Existing allocation-failure
    // semantics apply; do not spin indefinitely against concurrent producers.
    throw std::bad_alloc();
}

template <class T>
inline void KisTileHashTableTraits2<T>::setDefaultTileData(KisTileData *defaultTileData)
{
    QWriteLocker locker(&m_defaultPixelDataLock);

    if (m_defaultTileData) {
        m_defaultTileData->release();
        m_defaultTileData = 0;
    }

    if (defaultTileData) {
        defaultTileData->acquire();
        m_defaultTileData = defaultTileData;
    }
}

template <class T>
inline KisTileData* KisTileHashTableTraits2<T>::defaultTileData()
{
    QReadLocker locker(&m_defaultPixelDataLock);
    return m_defaultTileData;
}

template <class T>
inline KisTileData* KisTileHashTableTraits2<T>::refAndFetchDefaultTileData()
{
    QReadLocker locker(&m_defaultPixelDataLock);
    m_defaultTileData->ref();
    return m_defaultTileData;
}

template <class T>
void KisTileHashTableTraits2<T>::debugPrintInfo()
{
}

template <class T>
void KisTileHashTableTraits2<T>::debugMaxListLength(qint32 &/*min*/, qint32 &/*max*/)
{
}

typedef KisTileHashTableTraits2<KisTile> KisTileHashTable;
typedef KisTileHashTableIteratorTraits2<KisTile> KisTileHashTableIterator;
typedef KisTileHashTableIteratorTraits2<KisTile> KisTileHashTableConstIterator;

#endif // KIS_TILEHASHTABLE_2_H

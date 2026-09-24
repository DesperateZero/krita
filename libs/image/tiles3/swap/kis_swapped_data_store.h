/*
 *  SPDX-FileCopyrightText: 2010 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __KIS_SWAPPED_DATA_STORE_H
#define __KIS_SWAPPED_DATA_STORE_H

#include "kritaimage_export.h"

#include <QMutex>
#include <QByteArray>


class QMutex;
class KisTileData;
class KisAbstractTileCompressor;
class KisChunkAllocator;
class KisMemoryWindow;
class KisSwappedDataStoreTest;

enum class KisSwapInFailurePoint : quint8 {
    None,
    Mapping,
    Allocation,
    Decompression
};

class KRITAIMAGE_EXPORT KisSwappedDataStore
{
public:
    KisSwappedDataStore();
    ~KisSwappedDataStore();

    /**
     * Returns number of swapped out tile data objects
     */
    quint64 numTiles() const;

    /**
     * Swap out the data stored in the \a td to the swap file
     * and free memory occupied by td->data().
     * LOCKING: the lock on the tile data should be taken
     *          by the caller before making a call.
     */
    bool trySwapOutTileData(KisTileData *td);

    /**
     * Restore the data of a \a td basing on information
     * stored in the swap file.
     * LOCKING: the lock on the tile data should be taken
     *          by the caller before making a call.
     */
    // Returns false without consuming the swap chunk when mapping,
    // allocation, or decompression fails.
    bool swapInTileData(KisTileData *td);

    /**
     * Forget all the information linked with the tile data.
     * This should be done before deleting of the tile data,
     * whose actual data is swapped-out
     */
    void forgetTileData(KisTileData *td);

    /**
     * Returns the metric of the total memory stored in the swap
     * in *uncompressed* form!
     */
    qint64 totalSwapMemoryUsed() const;

    /**
     * Raw record bridge used by the canonical PageStore migration. The legacy
     * tile API continues to own KisTileData identity; these methods only put
     * opaque bytes into the same chunk allocator and swap file so the BR1
     * exact-generation index does not create a parallel SSD store.
     */
    quint64 storeRawRecord(const QByteArray &bytes);
    bool loadRawRecord(quint64 record, QByteArray *bytes);
    bool forgetRawRecord(quint64 record);

    /**
     * Some debugging output
     */
    void debugStatistics();

private:
    void testingFailNextSwapIn(KisSwapInFailurePoint point);

    QByteArray m_buffer;
    KisAbstractTileCompressor *m_compressor;

    KisChunkAllocator *m_allocator;
    KisMemoryWindow *m_swapSpace;

    mutable QMutex m_lock;

    qint64 m_totalSwapMemoryUsed;
    class RawPrivate;
    RawPrivate *m_raw;
    KisSwapInFailurePoint m_nextSwapInFailure =
        KisSwapInFailurePoint::None;

    friend class KisTileDataStore;
    friend class KisSwappedDataStoreTest;
};

#endif /* __KIS_SWAPPED_DATA_STORE_H */

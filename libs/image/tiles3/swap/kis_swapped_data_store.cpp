/*
 *  SPDX-FileCopyrightText: 2010 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QMutexLocker>
#include <QHash>
#include <cstring>
#include <limits>
//#include "kis_debug.h"
#include "kis_swapped_data_store.h"
#include "kis_memory_window.h"
#include "kis_image_config.h"

#include "kis_tile_compressor_2.h"

class KisSwappedDataStore::RawPrivate
{
public:
    quint64 nextRecord = 1;
    QHash<quint64, KisChunk> chunks;
};

//#define COMPRESSOR_VERSION 2

KisSwappedDataStore::KisSwappedDataStore()
    : m_totalSwapMemoryUsed(0)
    , m_raw(new RawPrivate)
{
    KisImageConfig config(true);
    const quint64 maxSwapSize = config.maxSwapSize() * MiB;
    const quint64 swapSlabSize = config.swapSlabSize() * MiB;
    const quint64 swapWindowSize = config.swapWindowSize() * MiB;

    m_allocator = new KisChunkAllocator(swapSlabSize, maxSwapSize);
    m_swapSpace = new KisMemoryWindow(config.swapDir(), swapWindowSize);

    // FIXME: use a factory after the patch is committed
    m_compressor = new KisTileCompressor2();
}

KisSwappedDataStore::~KisSwappedDataStore()
{
    delete m_raw;
    delete m_compressor;
    delete m_swapSpace;
    delete m_allocator;
}

quint64 KisSwappedDataStore::numTiles() const
{
    QMutexLocker locker(&m_lock);
    return m_allocator->numChunks();
}

bool KisSwappedDataStore::trySwapOutTileData(KisTileData *td)
{
    Q_ASSERT(td->data());
    QMutexLocker locker(&m_lock);

    /**
     * We are expecting that the lock of KisTileData
     * has already been taken by the caller for us.
     * So we can modify the tile data freely.
     */

    const qint32 expectedBufferSize = m_compressor->tileDataBufferSize(td);
    if(m_buffer.size() < expectedBufferSize)
        m_buffer.resize(expectedBufferSize);

    qint32 bytesWritten;
    m_compressor->compressTileData(td, (quint8*) m_buffer.data(), m_buffer.size(), bytesWritten);

    KisChunk chunk;
    if (!m_allocator->tryGetChunk(quint64(bytesWritten), &chunk))
        return false;
    quint8 *ptr = m_swapSpace->getWriteChunkPtr(chunk);
    if (!ptr) {
        m_allocator->freeChunk(chunk);
        qWarning() << "swap out of tile failed";
        return false;
    }
    memcpy(ptr, m_buffer.data(), bytesWritten);

    td->releaseMemory();
    td->setSwapChunk(chunk);

    m_totalSwapMemoryUsed += chunk.size();

    return true;
}

bool KisSwappedDataStore::swapInTileData(KisTileData *td)
{
    Q_ASSERT(!td->data());
    QMutexLocker locker(&m_lock);

    // see comment in swapOutTileData()

    KisChunk chunk = td->swapChunk();
    quint8 *ptr = m_swapSpace->getReadChunkPtr(chunk);
    if (!ptr)
        return false;

    td->allocateMemory();
    if (!td->data())
        return false;
    if (!m_compressor->decompressTileData(ptr, chunk.size(), td)) {
        td->releaseMemory();
        return false;
    }

    m_totalSwapMemoryUsed -= chunk.size();
    td->setSwapChunk(KisChunk());
    m_allocator->freeChunk(chunk);
    return true;
}

void KisSwappedDataStore::forgetTileData(KisTileData *td)
{
    QMutexLocker locker(&m_lock);

    m_totalSwapMemoryUsed -= td->swapChunk().size();

    m_allocator->freeChunk(td->swapChunk());
    td->setSwapChunk(KisChunk());
}

qint64 KisSwappedDataStore::totalSwapMemoryUsed() const
{
    QMutexLocker locker(&m_lock);
    return m_totalSwapMemoryUsed;
}

quint64 KisSwappedDataStore::storeRawRecord(const QByteArray &bytes)
{
    if (bytes.isEmpty() || m_raw->nextRecord == 0) return 0;
    QMutexLocker locker(&m_lock);
    KisChunk chunk;
    if (!m_allocator->tryGetChunk(quint64(bytes.size()), &chunk)) return 0;
    quint8 *destination = m_swapSpace->getWriteChunkPtr(chunk);
    if (!destination) {
        m_allocator->freeChunk(chunk);
        return 0;
    }
    std::memcpy(destination, bytes.constData(), size_t(bytes.size()));
    const quint64 record = m_raw->nextRecord++;
    if (record == 0 || m_raw->nextRecord == 0) {
        m_allocator->freeChunk(chunk);
        return 0;
    }
    m_raw->chunks.insert(record, chunk);
    m_totalSwapMemoryUsed += bytes.size();
    return record;
}

bool KisSwappedDataStore::loadRawRecord(quint64 record, QByteArray *bytes)
{
    if (record == 0 || !bytes) return false;
    QMutexLocker locker(&m_lock);
    const auto it = m_raw->chunks.constFind(record);
    if (it == m_raw->chunks.constEnd() ||
        it->size() > quint64(std::numeric_limits<qsizetype>::max())) {
        return false;
    }
    quint8 *source = m_swapSpace->getReadChunkPtr(it.value());
    if (!source) return false;
    *bytes = QByteArray(reinterpret_cast<const char *>(source),
                        qsizetype(it->size()));
    return true;
}

bool KisSwappedDataStore::forgetRawRecord(quint64 record)
{
    QMutexLocker locker(&m_lock);
    auto it = m_raw->chunks.find(record);
    if (record == 0 || it == m_raw->chunks.end()) return false;
    const quint64 size = it->size();
    m_allocator->freeChunk(it.value());
    m_raw->chunks.erase(it);
    m_totalSwapMemoryUsed -= qint64(size);
    return true;
}

void KisSwappedDataStore::debugStatistics()
{
    m_allocator->sanityCheck();
    m_allocator->debugFragmentation();
}

/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageDefaultStorage_p.h"
#include "KisPageWriteCoordinator_p.h"

#include <QMutexLocker>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

struct KisCpuDefaultReadBuffer::Statistics
{
    QAtomicInteger<quint64> liveBytes{0};
    QAtomicInteger<quint64> buffersCreated{0};
    QAtomicInteger<quint64> initializedBytes{0};
    QAtomicInteger<quint64> cacheEvictions{0};
    QAtomicInteger<quint64> cacheOversizeBypasses{0};
};

KisCpuDefaultReadBuffer::~KisCpuDefaultReadBuffer()
{
    if (data) {
        ::operator delete(data, std::align_val_t(alignment));
        statistics->liveBytes.fetchAndSubRelaxed(byteSize);
        storageOwner->releaseLiveCharge(byteSize, KisBackingBudgetClass::OptionalCache);
    }
}

KisPageDefaultStorage::KisPageDefaultStorage(KisBackingBudgetController &budget,
    const KisMutationStorageAllocator<KisPageDefaultStorage> &storage)
    : m_readBuffers(KisMutationStorageAllocator<ReadCacheEntry>(&budget))
    , m_statistics(std::allocate_shared<KisCpuDefaultReadBuffer::Statistics>(storage))
    , m_budget(budget)
    , m_preparations(PreparationLess{}, KisMutationStorageAllocator<KisPageKey>(&budget))
{
}

std::shared_ptr<const KisCpuDefaultReadBuffer> KisPageDefaultStorage::createReadBuffer(
    const KisSurfaceEpochState &surface,
    const std::shared_ptr<KisCpuDefaultReadBuffer::Statistics> &statistics,
    KisBackingBudgetController &budget) try
{
    const KisPageAllocationDescriptor descriptor = surface.allocationDescriptor();
    if (!descriptor.isValid()) return {};

    const quint64 rowAlignment =
        std::max(descriptor.rowAlignment, descriptor.format.pixelAlignment);
    const quint64 stride =
        (descriptor.minimumRowBytes() + rowAlignment - 1) & ~(rowAlignment - 1);
    const quint64 height = quint64(descriptor.pageExtent.height());
    if (stride > std::numeric_limits<quint32>::max() ||
        height > std::numeric_limits<size_t>::max() / stride ||
        height > quint64(std::numeric_limits<qint64>::max()) / stride) {
        return {};
    }

    KisBackingBudgetDelta requested;
    requested.buckets[size_t(KisBackingBudgetClass::OptionalCache)].cpuRam = qint64(stride * height);
    auto reservation = budget.reserve(requested, nullptr);
    if (!reservation.isValid()) return {};

    auto result = std::allocate_shared<KisCpuDefaultReadBuffer>(
        KisMutationStorageAllocator<KisCpuDefaultReadBuffer>::retained(&budget));
    result->rowStride = quint32(stride);
    result->byteSize = stride * height;
    result->alignment = std::max(
        size_t(descriptor.format.pixelAlignment), alignof(std::max_align_t));
    result->statistics = statistics;
    result->storageOwner.reset(kisMutationStorageOwner(&budget));
    result->data = ::operator new(
        size_t(result->byteSize), std::align_val_t(result->alignment), std::nothrow);
    if (!result->data) return {};

    reservation.commit(requested);
    result->storageOwner->retainLiveCharge(result->byteSize);
    statistics->liveBytes.fetchAndAddRelaxed(result->byteSize);
    statistics->buffersCreated.fetchAndAddRelaxed(1);
    statistics->initializedBytes.fetchAndAddRelaxed(result->byteSize);
    auto *bytes = static_cast<char *>(result->data);
    for (int y = 0; y < descriptor.pageExtent.height(); ++y) {
        auto *row = bytes + size_t(y) * stride;
        for (int x = 0; x < descriptor.pageExtent.width(); ++x) {
            std::memcpy(row + size_t(x) * descriptor.format.pixelStride,
                        descriptor.format.defaultPixel.constData(),
                        descriptor.format.pixelStride);
        }
        std::memset(row + descriptor.minimumRowBytes(), 0,
                    size_t(stride - descriptor.minimumRowBytes()));
    }
    return result;
}
catch (const std::bad_alloc &)
{
    return {};
}

std::shared_ptr<const KisCpuDefaultReadBuffer> KisPageDefaultStorage::readBuffer(
    const KisSurfaceEpochState &surface)
{
    const ReadCacheKey key{surface.surface.value, surface.defaultPixelRevision};
    {
        QMutexLocker lock(&m_cacheMutex);
        const auto found = std::find_if(m_readBuffers.begin(), m_readBuffers.end(),
            [&](const auto &entry) { return entry.key == key; });
        if (found != m_readBuffers.end()) return found->buffer;
    }

    auto buffer = createReadBuffer(surface, m_statistics, m_budget);
    if (!buffer) {
        clearReadCache();
        buffer = createReadBuffer(surface, m_statistics, m_budget);
        if (!buffer) return {};
    }

    // Each captured view keeps its own strong reference; eviction cannot
    // invalidate old defaults/guards. Oversize buffers remain scope-local.
    if (buffer->byteSize > ReadCacheByteBudget) {
        m_statistics->cacheOversizeBypasses.fetchAndAddRelaxed(1);
        return buffer;
    }

    QMutexLocker lock(&m_cacheMutex);
    const auto found = std::find_if(m_readBuffers.begin(), m_readBuffers.end(),
        [&](const auto &entry) { return entry.key == key; });
    if (found != m_readBuffers.end()) return found->buffer;
    while (!m_readBuffers.empty() &&
           (m_readBuffers.size() >= size_t(ReadCacheEntryBudget) ||
            m_readBufferBytes + buffer->byteSize > ReadCacheByteBudget)) {
        m_readBufferBytes -= m_readBuffers.front().buffer->byteSize;
        m_readBuffers.pop_front();
        m_statistics->cacheEvictions.fetchAndAddRelaxed(1);
    }
    try {
        m_readBuffers.push_back({key, buffer});
    } catch (const std::bad_alloc &) {
        // A valid scope-local buffer needs no optional directory entry.
        m_statistics->cacheOversizeBypasses.fetchAndAddRelaxed(1);
        return buffer;
    }
    m_readBufferBytes += buffer->byteSize;
    return buffer;
}

void KisPageDefaultStorage::clearReadCache()
{
    QMutexLocker lock(&m_cacheMutex);
    m_readBuffers.clear();
    m_readBufferBytes = 0;
}

bool KisPageDefaultStorage::preparationBlockedLocked(const KisPageKey &key) const
{
    return m_preparations.find(key) != m_preparations.end() ||
           m_preparations.size() >= size_t(PreparationEntryBudget);
}

void KisPageDefaultStorage::waitForPreparationChangeLocked(QMutex *ownerMutex)
{
    Q_ASSERT(ownerMutex);
    ++m_preparationWaits;
    m_preparationChanged.wait(ownerMutex);
}

void KisPageDefaultStorage::beginPreparationLocked(const KisPageKey &key)
{
    Q_ASSERT(key.isValid());
    Q_ASSERT(!preparationBlockedLocked(key));
    m_preparations.insert(key);
    ++m_materializationRequests;
    m_peakPreparations = std::max(m_peakPreparations, qsizetype(m_preparations.size()));
}

void KisPageDefaultStorage::finishPreparationLocked(const KisPageKey &key)
{
    const qsizetype removed = qsizetype(m_preparations.erase(key));
    Q_ASSERT(removed == 1);
    if (removed == 1) m_preparationChanged.wakeAll();
}

bool KisPageDefaultStorage::isDrainedLocked() const
{
    return m_preparations.empty();
}

KisPageDefaultStorageSnapshot KisPageDefaultStorage::snapshotLocked() const
{
    KisPageDefaultStorageSnapshot result;
    {
        QMutexLocker lock(&m_cacheMutex);
        result.cachedReadBuffers = m_readBuffers.size();
        result.cachedReadBytes = m_readBufferBytes;
    }
    result.liveReadBytes = m_statistics->liveBytes.loadRelaxed();
    result.readBuffersCreated = m_statistics->buffersCreated.loadRelaxed();
    result.readInitializedBytes = m_statistics->initializedBytes.loadRelaxed();
    result.cacheEvictions = m_statistics->cacheEvictions.loadRelaxed();
    result.cacheOversizeBypasses =
        m_statistics->cacheOversizeBypasses.loadRelaxed();
    result.activePreparations = m_preparations.size();
    result.peakPreparations = m_peakPreparations;
    result.preparationWaits = m_preparationWaits;
    result.materializationRequests = m_materializationRequests;
    return result;
}

/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_DEFAULT_STORAGE_P_H
#define KIS_PAGE_DEFAULT_STORAGE_P_H

#include "KisPageStoreTypes.h"
#include "KisMutationStorage_p.h"
#include "KisPageWaitCondition_p.h"

#include <QAtomicInteger>
#include <QMutex>

#include <cstddef>
#include <list>
#include <set>
#include <tuple>

class KisBackingBudgetController;

/**
 * Immutable bytes for one exact surface-default revision.
 *
 * This is semantic default content, not a provider allocation or a fabricated
 * replica identity. A read guard supplies the logical page key/revision from
 * its protected epoch root, so absent pages may safely share these bytes.
 */
class KRITAIMAGE_EXPORT KisCpuDefaultReadBuffer final
{
public:
    ~KisCpuDefaultReadBuffer();

    void *data = nullptr;
    quint32 rowStride = 0;
    quint64 byteSize = 0;

private:
    friend class KisPageDefaultStorage;

    struct Statistics;
    size_t alignment = alignof(std::max_align_t);
    // byteSize is the readable page span; allocator padding is charged until
    // free but must never become readable bytes in a captured guard.
    size_t allocationBytes = 0;
    std::shared_ptr<Statistics> statistics;
    boost::intrusive_ptr<KisMutationStorageOwner> storageOwner;
};

struct KisPageDefaultStorageSnapshot
{
    qsizetype cachedReadBuffers = 0;
    quint64 cachedReadBytes = 0;
    quint64 liveReadBytes = 0;
    quint64 readBuffersCreated = 0;
    quint64 readInitializedBytes = 0;
    quint64 cacheEvictions = 0;
    quint64 cacheOversizeBypasses = 0;

    qsizetype activePreparations = 0;
    qsizetype peakPreparations = 0;
    quint64 preparationWaits = 0;
    quint64 materializationRequests = 0;
};

/**
 * Store-private owner of virtual-default bytes and materialization admission.
 *
 * Cache methods are internally synchronized. Preparation methods ending in
 * Locked must be called while KisPageStore's owner mutex is held; wait releases
 * and reacquires that same mutex. This preserves the existing lock order and
 * keeps epoch/provider/publication policy outside this leaf service.
 */
class KRITAIMAGE_EXPORT KisPageDefaultStorage final
{
public:
    static constexpr qsizetype ReadCacheEntryBudget = 64;
    static constexpr quint64 ReadCacheByteBudget = 8 * 1024 * 1024;
    static constexpr qsizetype PreparationEntryBudget = 64;

    explicit KisPageDefaultStorage(KisBackingBudgetController &budget,
        const KisMutationStorageAllocator<KisPageDefaultStorage> &storage = KisMutationStorageAllocator<KisPageDefaultStorage>{});
    ~KisPageDefaultStorage() = default;

    KisPageDefaultStorage(const KisPageDefaultStorage &) = delete;
    KisPageDefaultStorage &operator=(const KisPageDefaultStorage &) = delete;
    KisPageDefaultStorage(KisPageDefaultStorage &&) = delete;
    KisPageDefaultStorage &operator=(KisPageDefaultStorage &&) = delete;

    std::shared_ptr<const KisCpuDefaultReadBuffer> readBuffer(
        const KisSurfaceEpochState &surface);
    void clearReadCache();

    bool preparationBlockedLocked(const KisPageKey &key) const;
    void waitForPreparationChangeLocked(QMutex *ownerMutex);
    void beginPreparationLocked(const KisPageKey &key);
    void finishPreparationLocked(const KisPageKey &key);
    bool isDrainedLocked() const;

    KisPageDefaultStorageSnapshot snapshotLocked() const;

private:
    using ReadCacheKey = QPair<quint64, quint64>;
    struct ReadCacheEntry {
        ReadCacheKey key;
        std::shared_ptr<const KisCpuDefaultReadBuffer> buffer;
    };
    struct PreparationLess {
        bool operator()(const KisPageKey &a, const KisPageKey &b) const {
            return std::tie(a.surface.value, a.page.row, a.page.column)
                 < std::tie(b.surface.value, b.page.row, b.page.column);
        }
    };

    static std::shared_ptr<const KisCpuDefaultReadBuffer> createReadBuffer(
        const KisSurfaceEpochState &surface,
        const std::shared_ptr<KisCpuDefaultReadBuffer::Statistics> &statistics,
        KisBackingBudgetController &budget);

    mutable QMutex m_cacheMutex;
    std::list<ReadCacheEntry, KisMutationStorageAllocator<ReadCacheEntry>> m_readBuffers;
    quint64 m_readBufferBytes = 0;
    std::shared_ptr<KisCpuDefaultReadBuffer::Statistics> m_statistics;
    KisBackingBudgetController &m_budget;

    // Guarded by the PageStore owner mutex, not m_cacheMutex.
    std::set<KisPageKey, PreparationLess, KisMutationStorageAllocator<KisPageKey>> m_preparations;
    KisPageWaitCondition m_preparationChanged;
    qsizetype m_peakPreparations = 0;
    quint64 m_preparationWaits = 0;
    quint64 m_materializationRequests = 0;
};

#endif // KIS_PAGE_DEFAULT_STORAGE_P_H

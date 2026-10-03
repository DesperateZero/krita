/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_PAGE_STORE_STORAGE_PRESSURE_H
#define KIS_PAGE_STORE_STORAGE_PRESSURE_H

#include "KisPageWriteCoordinator_p.h"
#include <vector>

// Real storage pressure measured in charged capacity, not requested payload.
// Keep the original hard limits and one-byte refusal boundaries. Most capacity
// is occupied by physical allocations. A residue smaller than the minimum
// allocation is an explicit budget demand, released with this fixture.
class KisPageStoreStoragePressure
{
public:
    KisPageStoreStoragePressure(KisMutationStorageAllocator<char> storage, size_t capacity)
        : m_storage(std::move(storage))
    {
        try {
            while (capacity >= kisPageStorageAllocationBytes(0, alignof(char))) {
                size_t first = 0, last = capacity;
                while (first < last) {
                    const size_t middle = first + (last - first + 1) / 2;
                    if (kisPageStorageAllocationBytes(middle, alignof(char)) <= capacity)
                        first = middle;
                    else
                        last = middle - 1;
                }
                char *data = m_storage.allocate(first);
                try { m_allocations.push_back({data, first}); }
                catch (...) { m_storage.deallocate(data, first); throw; }
                capacity -= kisPageStorageBytes(data, alignof(char));
            }
            if (!capacity) return;
            if (m_storage.budget) {
                KisBackingBudgetDelta delta;
                delta.buckets[size_t(KisBackingBudgetClass::MetadataArena)].cpuRam = qint64(capacity);
                auto reservation = m_storage.budget->reserve(delta, nullptr);
                if (!reservation.isValid()) throw std::bad_alloc();
                m_storage.budget->commitReservation(std::move(reservation), delta);
            } else {
                kisReservePageProcessStorage(capacity);
            }
            m_residue = capacity;
        } catch (...) {
            clear();
            throw;
        }
    }
    ~KisPageStoreStoragePressure() { clear(); }
    KisPageStoreStoragePressure(const KisPageStoreStoragePressure &) = delete;
    KisPageStoreStoragePressure &operator=(const KisPageStoreStoragePressure &) = delete;
private:
    void clear() noexcept
    {
        for (const auto &allocation : m_allocations)
            m_storage.deallocate(allocation.data, allocation.bytes);
        m_allocations.clear();
        if (m_residue) {
            if (m_storage.budget)
                m_storage.budget->releaseLive(KisBackingBudgetClass::MetadataArena,
                                             KisPageAccessDomain::CpuRam, m_residue);
            else
                kisReleasePageProcessStorage(m_residue);
            m_residue = 0;
        }
    }
    struct Allocation { char *data; size_t bytes; };
    KisMutationStorageAllocator<char> m_storage;
    std::vector<Allocation> m_allocations;
    size_t m_residue = 0;
};

inline void *allocateTestStoragePressure(KisBackingBudgetController *budget, size_t capacity, size_t alignment)
{
    Q_ASSERT(alignment == alignof(char));
    Q_UNUSED(alignment);
    return new KisPageStoreStoragePressure(KisMutationStorageAllocator<char>(budget), capacity);
}

inline KisPageStoreStoragePressure *allocateTestStoragePressure(KisMutationStorageAllocator<char> storage, size_t capacity)
{
    return new KisPageStoreStoragePressure(std::move(storage), capacity);
}

inline void freeTestStoragePressure(void *data) noexcept
{
    delete static_cast<KisPageStoreStoragePressure *>(data);
}

inline void freeTestStoragePressure(KisBackingBudgetController *, void *data, size_t, size_t) noexcept
{
    freeTestStoragePressure(data);
}

inline void *allocateTestProcessStoragePressure(size_t capacity, size_t alignment)
{
    return allocateTestStoragePressure(nullptr, capacity, alignment);
}

inline void freeTestProcessStoragePressure(void *data, size_t, size_t) noexcept
{
    freeTestStoragePressure(data);
}

#endif

/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#else
#error "KisPagePlatformStorage requires Windows or a Unix mmap implementation"
#endif

namespace KisPagePlatformStorage
{

struct Mapping
{
    void *base;
    size_t bytes;
};

inline size_t pageSize() noexcept
{
#ifdef _WIN32
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return size_t(info.dwPageSize);
#else
    const long result = sysconf(_SC_PAGESIZE);
    return result > 0 ? size_t(result) : 0;
#endif
}

inline size_t allocationBytes(size_t bytes, size_t alignment) noexcept
{
    constexpr size_t maximum = size_t(std::numeric_limits<std::int64_t>::max());
    if (!alignment || (alignment & (alignment - 1))) return size_t(-1);
    const size_t page = pageSize();
    const size_t mappingAlignment = std::max(alignment, alignof(Mapping));
    if (!page || mappingAlignment > maximum - sizeof(Mapping)) return size_t(-1);
    const size_t prefix = sizeof(Mapping) + mappingAlignment - 1;
    if (bytes > maximum - prefix) return size_t(-1);
    const size_t required = bytes + prefix;
    if (required > maximum - (page - 1)) return size_t(-1);
    return (required + page - 1) / page * page;
}

inline void *allocate(size_t bytes, size_t alignment, size_t capacity)
{
    if (capacity != allocationBytes(bytes, alignment)) throw std::bad_alloc();
#ifdef _WIN32
    void *raw = VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!raw) throw std::bad_alloc();
#else
#ifdef MAP_ANONYMOUS
    constexpr int anonymous = MAP_ANONYMOUS;
#else
    constexpr int anonymous = MAP_ANON;
#endif
    void *raw = mmap(nullptr, capacity, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | anonymous, -1, 0);
    if (raw == MAP_FAILED) throw std::bad_alloc();
#endif
    const size_t mappingAlignment = std::max(alignment, alignof(Mapping));
    const uintptr_t start = reinterpret_cast<uintptr_t>(raw) + sizeof(Mapping);
    const uintptr_t aligned = (start + mappingAlignment - 1) & ~(mappingAlignment - 1);
    auto *data = reinterpret_cast<std::byte *>(aligned);
    ::new (data - sizeof(Mapping)) Mapping{raw, capacity};
    return data;
}

inline size_t bytes(const void *data) noexcept
{
    if (!data) return 0;
    return reinterpret_cast<const Mapping *>(
        static_cast<const std::byte *>(data) - sizeof(Mapping))->bytes;
}

inline bool release(void *data) noexcept
{
    if (!data) return true;
    const auto mapping = *reinterpret_cast<const Mapping *>(
        static_cast<const std::byte *>(data) - sizeof(Mapping));
#ifdef _WIN32
    return VirtualFree(mapping.base, 0, MEM_RELEASE) != 0;
#else
    return munmap(mapping.base, mapping.bytes) == 0;
#endif
}

} // namespace KisPagePlatformStorage

/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPagePlatformStorage_p.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

namespace
{

bool exercise(size_t bytes, size_t alignment)
{
    using namespace KisPagePlatformStorage;
    const size_t capacity = allocationBytes(bytes, alignment);
    if (capacity == size_t(-1) || capacity < bytes || capacity % pageSize()) return false;
    void *data = allocate(bytes, alignment, capacity);
    if (!data || reinterpret_cast<uintptr_t>(data) % alignment || KisPagePlatformStorage::bytes(data) != capacity)
        return false;
#ifdef _WIN32
    const auto mapping = *reinterpret_cast<const Mapping *>(
        static_cast<const std::byte *>(data) - sizeof(Mapping));
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(mapping.base, &info, sizeof(info)) != sizeof(info)
        || info.AllocationBase != mapping.base || info.RegionSize != capacity) {
        release(data);
        return false;
    }
#endif
    if (bytes) {
        auto *payload = static_cast<unsigned char *>(data);
        payload[0] = 0x31;
        payload[bytes - 1] = 0x72;
    }
    return release(data);
}

} // namespace

int main()
{
    using namespace KisPagePlatformStorage;
    if (!pageSize() || pageSize() & (pageSize() - 1)) return 1;
    if (allocationBytes(1, 0) != size_t(-1) || allocationBytes(1, 3) != size_t(-1)) return 2;
    if (allocationBytes(size_t(std::numeric_limits<std::int64_t>::max()), 1) != size_t(-1)) return 3;

    constexpr std::array<size_t, 8> sizes{
        0, 1, 15, 961, 65536, 65537, 1024 * 1024 - 32768, 1024 * 1024};
    constexpr std::array<size_t, 6> alignments{1, 16, 32, 4096, 32768, 1024 * 1024};
    for (size_t alignment : alignments) {
        for (size_t bytes : sizes) {
            if (!exercise(bytes, alignment)) return 4;
        }
    }

    std::atomic<bool> ok{true};
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&, worker] {
            for (size_t iteration = 0; iteration < 128; ++iteration) {
                const size_t bytes = 1 + ((worker + 1) * (iteration + 31) * 7919) % (2 * 1024 * 1024);
                const size_t alignment = alignments[(worker + iteration) % alignments.size()];
                try {
                    if (!exercise(bytes, alignment)) ok.store(false, std::memory_order_relaxed);
                } catch (...) {
                    ok.store(false, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto &worker : workers) worker.join();
    if (!ok.load(std::memory_order_relaxed)) return 5;

    std::cout << "platform="
#ifdef _WIN32
              << "windows"
#elif defined(__linux__)
              << "linux"
#else
              << "unix"
#endif
              << " page_size=" << pageSize()
              << " cases=" << sizes.size() * alignments.size()
              << " concurrent=1024 result=pass\n";
    return 0;
}

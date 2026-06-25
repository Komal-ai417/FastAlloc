#pragma once
#include "slab.h"
#include "fast_alloc_config.h"
#include "global_heap.h"
#include <array>
#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#else
#include <pthread.h>
#endif

#if defined(_MSC_VER)
#define FAST_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define FAST_THREAD_LOCAL __thread
#else
#define FAST_THREAD_LOCAL thread_local
#endif

namespace FastAlloc {

class TLSCache;

class TLSCache {
public:
    static TLSCache& GetSlow();

    void* AllocateBlockSlow(std::size_t class_index);
    void DeallocateBlockSlow(std::size_t class_index);

    // Large Allocation Cache
    void* AllocateLargeCached(std::size_t size);
    void  DeallocateLargeCached(void* ptr, std::size_t alloc_size);

    ~TLSCache();

private:
    TLSCache();

    std::array<CacheBin, NUM_SIZE_CLASSES> bins_{};
    uint32_t arena_index_{0};

    struct LargeFreeEntry {
        LargeFreeEntry* next;
        std::size_t alloc_size;
    };

    static inline std::size_t GetMaxLargeCacheSize(std::size_t size) {
        std::size_t count = (16 * 1024 * 1024) / size;
        if (count < 8) return 8;
        if (count > 2048) return 2048;
        return count;
    }

    std::array<LargeFreeEntry*, NUM_LARGE_CLASSES> large_free_bins_{};
    std::array<std::size_t, NUM_LARGE_CLASSES> large_counts_{};

#ifdef _WIN32
    static DWORD tls_key_;
#else
    static pthread_key_t tls_key_;
    static void TlsDestructor(void* ptr);
#endif
    static void InitTlsKey();
};

} // namespace FastAlloc
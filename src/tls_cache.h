#pragma once
#include "slab.h"
#include "fast_alloc_config.h"
#include <array>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#else
#include <pthread.h>
#endif

namespace FastAlloc {

class TLSCache {
public:
    static TLSCache& GetFast();
    static TLSCache& GetSlow();

    void* AllocateBlock(std::size_t class_index);
    void DeallocateBlock(std::size_t class_index, FreeBlock* block);

    // Large Allocation Cache
    void* AllocateLargeCached(std::size_t size);
    void  DeallocateLargeCached(void* ptr, std::size_t alloc_size, std::size_t user_size);

    ~TLSCache();

private:
    TLSCache() = default;

    std::array<FreeBlock*, NUM_SIZE_CLASSES> fast_bins_{};
    std::array<std::size_t, NUM_SIZE_CLASSES> counts_{};

    // MEMORY OPTIMIZATION: Prevent memory hoarding.
    static std::size_t GetMaxCacheSize(std::size_t class_index) {
        std::size_t size = ClassIndexToSize(class_index);
        if (size <= 128) return 1024;
        if (size <= 512) return 512;
        if (size <= 1024) return 256;
        if (size <= 4096) return 64;
        return 16;
    }

    // OPT-5: Large Allocation Reuse Pool
    struct LargeFreeEntry {
        LargeFreeEntry* next;
        std::size_t alloc_size;
        std::size_t user_size;
    };

    static constexpr std::size_t NUM_LARGE_CLASSES = 16;
    static constexpr std::size_t LARGE_CLASS_BASE = 16; // 2^16 = 64KB
    static constexpr std::size_t MAX_LARGE_CACHE = 8;

    std::array<LargeFreeEntry*, NUM_LARGE_CLASSES> large_free_bins_{};
    std::array<std::size_t, NUM_LARGE_CLASSES> large_counts_{};

    std::size_t LargeSizeToClass(std::size_t size) {
        if (size <= (1ULL << LARGE_CLASS_BASE)) return 0;
        unsigned long clz = 0;
#if defined(_MSC_VER)
        _BitScanReverse(&clz, (unsigned long)(size - 1));
#else
        clz = 31 - __builtin_clz((unsigned int)(size - 1));
#endif
        std::size_t cls = clz - LARGE_CLASS_BASE + 1;
        return cls < NUM_LARGE_CLASSES ? cls : NUM_LARGE_CLASSES - 1;
    }

#ifdef _WIN32
    static DWORD tls_key_;
#else
    static pthread_key_t tls_key_;
    static void TlsDestructor(void* ptr);
#endif
    static void InitTlsKey();
};

} // namespace FastAlloc

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

namespace FastAlloc {

class TLSCache;
extern thread_local TLSCache* fast_path_cache;

class TLSCache {
public:
    static inline TLSCache& GetFast() {
        if (fast_path_cache) return *fast_path_cache;
        return GetSlow();
    }
    static TLSCache& GetSlow();

    inline void* AllocateBlock(std::size_t class_index) {
        FreeBlock* block = fast_bins_[class_index];
        if (block) {
            fast_bins_[class_index] = block->next;
            counts_[class_index]--;
            return block;
        }
        return AllocateBlockSlow(class_index);
    }
    
    void* AllocateBlockSlow(std::size_t class_index);

    inline void DeallocateBlock(std::size_t class_index, FreeBlock* block) {
        block->next = fast_bins_[class_index];
        fast_bins_[class_index] = block;
        counts_[class_index]++;

        std::size_t max_cache = GetMaxCacheSize(class_index);
        if (counts_[class_index] >= max_cache) {
            DeallocateBlockSlow(class_index);
        }
    }
    
    void DeallocateBlockSlow(std::size_t class_index);

    // Large Allocation Cache
    void* AllocateLargeCached(std::size_t size);
    void  DeallocateLargeCached(void* ptr, std::size_t alloc_size);

    ~TLSCache();

private:
    TLSCache();

    std::array<FreeBlock*, NUM_SIZE_CLASSES> fast_bins_{};
    std::array<std::size_t, NUM_SIZE_CLASSES> counts_{};
    uint32_t arena_index_{0};

    static inline std::size_t GetMaxCacheSize(std::size_t class_index) {
        std::size_t size = ClassIndexToSize(class_index);
        if (size <= 64) return 16384;
        if (size <= 256) return 8192;
        if (size <= 1024) return 4096;
        if (size <= 4096) return 1024;
        if (size <= 8192) return 512;
        return 128;
    }

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
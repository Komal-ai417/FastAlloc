#pragma once
#include "slab.h"
#include "fast_alloc_config.h"
#include "global_heap.h"
#include "debug_aid.h"
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

// fast_alloc_config.h pins _WIN32_WINNT >= 0x0600 before any windows.h
// include chain reaches this point (audit H5 fix: Fls* APIs require Vista+).

#if defined(_MSC_VER)
#define FAST_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define FAST_THREAD_LOCAL __thread
#else
#define FAST_THREAD_LOCAL thread_local
#endif

namespace FastAlloc {

class TLSCache;
extern FAST_THREAD_LOCAL TLSCache* fast_path_cache;

class TLSCache {
public:
    static inline TLSCache& GetFast() {
        if (fast_path_cache) return *fast_path_cache;
        return GetSlow();
    }
    static TLSCache& GetSlow();

    inline void* AllocateBlock(std::size_t class_index) {
        CacheBin& bin = bins_[class_index];
        FreeBlock* block = bin.head;
        if (FAST_LIKELY(block != nullptr)) {
            bin.head = block->next;
            bin.count--;
            return block;
        }
        return AllocateBlockSlow(class_index);
    }

    void* AllocateBlockSlow(std::size_t class_index);

    inline void DeallocateBlock(std::size_t class_index, FreeBlock* block) {
        CacheBin& bin = bins_[class_index];
        block->next = bin.head;
        bin.head = block;
        bin.count++;

        if (FAST_UNLIKELY(bin.count >= CACHE_LIMITS[class_index])) {
            DeallocateBlockSlow(class_index);
        }
    }

    void DeallocateBlockSlow(std::size_t class_index);

    // Large Allocation Cache
    void* AllocateLargeCached(std::size_t size);
    void  DeallocateLargeCached(void* ptr, std::size_t alloc_size);

    // Flush every cached block and large entry back to the global heap.
    // Used by fast_alloc_purge_thread_cache() and by the destructor.
    void FlushToGlobalHeap();

    // ------------------------------------------------------------------
    // Hot-path statistics batching.
    // Plain member increments (no atomics!) accumulated per thread and
    // flushed to the global counters every kStatsFlushOps operations, at
    // stats() calls and at thread exit. Keeps the alloc/free fast path free
    // of shared cache-line traffic.
    // ------------------------------------------------------------------
    inline void CountSmallAlloc(std::size_t bytes) {
        ++stats_.small_allocs;
        stats_.small_alloc_bytes += bytes;
        if (++stats_.ops >= kStatsFlushOps) FlushStats();
    }
    inline void CountSmallFree(std::size_t bytes) {
        ++stats_.small_frees;
        stats_.small_free_bytes += bytes;
        if (++stats_.ops >= kStatsFlushOps) FlushStats();
    }
    inline void CountLargeAlloc(std::size_t bytes) {
        ++stats_.large_allocs;
        stats_.large_alloc_bytes += bytes;
        if (++stats_.ops >= kStatsFlushOps) FlushStats();
    }
    inline void CountLargeFree(std::size_t bytes) {
        ++stats_.large_frees;
        stats_.large_free_bytes += bytes;
        if (++stats_.ops >= kStatsFlushOps) FlushStats();
    }
    inline void CountRealloc() {
        ++stats_.reallocs;
        if (++stats_.ops >= kStatsFlushOps) FlushStats();
    }
    void FlushStats();

    ~TLSCache();

private:
    TLSCache();

    // ------------------------------------------------------------------
    // CacheBin: head + count + adaptive refill sizing.
    //
    // 'next_refill' is the batch size the NEXT refill of this bin will
    // request from the global heap. It starts small (kInitialRefill) and
    // doubles on every consecutive miss, capped at CACHE_LIMITS/2. This
    // keeps short-lived threads (a handful of allocations) from pulling
    // thousands of blocks they will only have to flush back at exit,
    // while steady-state threads ramp up to full batches within a few
    // misses and behave exactly like the old fixed-size policy.
    // The field occupies the old padding slot: CacheBin stays 16 bytes.
    // ------------------------------------------------------------------
    struct CacheBin {
        FreeBlock* head{nullptr};
        uint32_t count{0};
        uint32_t next_refill{0};
    };
    std::array<CacheBin, NUM_SIZE_CLASSES> bins_{};
    uint32_t arena_index_{0};

    struct LargeFreeEntry {
        LargeFreeEntry* next;
        std::size_t alloc_size;
    };

    static inline std::size_t GetMaxLargeCacheSize(std::size_t size) {
        // Per-class retention cap: ~2 MB of spans per large class per thread
        // (was 16 MB, which hoarded up to ~50 MB/thread and pushed the
        // shared 64 MB page cache over its cap -> constant mmap/munmap
        // cycling under mixed large sizes). Entry count stays within
        // [4, 64]; LIFO cycling workloads only need depth 1-4, and misses
        // are served by the (now best-fit) global page cache.
        std::size_t count = (2 * 1024 * 1024) / (size ? size : 1);
        if (count < 4) return 4;
        if (count > 64) return 64;
        return count;
    }

    std::array<LargeFreeEntry*, NUM_LARGE_CLASSES> large_free_bins_{};
    std::array<std::size_t, NUM_LARGE_CLASSES> large_counts_{};

    // ------------------------------------------------------------------
    // Per-thread stats accumulation (see Count* methods above).
    // ------------------------------------------------------------------
    static constexpr uint64_t kStatsFlushOps = 256;
    struct LocalStats {
        uint64_t small_allocs = 0, small_alloc_bytes = 0;
        uint64_t small_frees = 0, small_free_bytes = 0;
        uint64_t large_allocs = 0, large_alloc_bytes = 0;
        uint64_t large_frees = 0, large_free_bytes = 0;
        uint64_t reallocs = 0;
        uint64_t ops = 0;
    };
    LocalStats stats_{};

#ifdef _WIN32
    static DWORD tls_key_;
#else
    static pthread_key_t tls_key_;
    static void TlsDestructor(void* ptr);
#endif
    static void InitTlsKey();

#if FASTALLOC_DEBUG_ENABLED
    // Debug invariants: bin count vs actual list length (audit M6).
    void AuditBin(std::size_t class_index) const;
#endif
};

} // namespace FastAlloc

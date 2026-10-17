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

// Requires _WIN32_WINNT >= 0x0600 for FLS APIs on Windows.

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

// Logs diagnostic information once if a double push of the bin head is detected.
void ReportDoublePushGuard();

// Logs diagnostic information once if a bin head contains an invalid pointer.
void ReportBinHeadCorruption(std::size_t class_index, std::uintptr_t bad_head);

// Logs diagnostic information once if a freelist link fails pointer plausibility.
void ReportWildFreelistLink(const char* where, std::size_t class_index,
                             std::uintptr_t bad_link);

// Diagnostic scan: inspect calling thread's TLSCache and pool for pointers in [lo, hi).
std::size_t AuditCurrentThreadCache(const void* lo, const void* hi);

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
        // Verify head pointer plausibility before dereferencing next pointer.
        if (FAST_LIKELY(IsPlausibleHeapPtr(block))) {
            bin.head = block->next;
            bin.count--;
            return block;
        }
        if (FAST_UNLIKELY(block != nullptr)) {
            // Reset bin and route to slow path if bin head holds an invalid pointer.
            ReportBinHeadCorruption(class_index,
                                    reinterpret_cast<std::uintptr_t>(block));
            bin.head = nullptr;
            bin.count = 0;
            bin.next_refill = 0;
        }
        return AllocateBlockSlow(class_index);
    }

    void* AllocateBlockSlow(std::size_t class_index);

    inline void DeallocateBlock(std::size_t class_index, FreeBlock* block) {
        // Validate block pointer plausibility before linking into bin.
        if (FAST_UNLIKELY(!IsPlausibleHeapPtr(block))) {
            ReportWildFreelistLink("DeallocateBlock(push)", class_index,
                                   reinterpret_cast<std::uintptr_t>(block));
            return;
        }
        CacheBin& bin = bins_[class_index];
        if (FAST_UNLIKELY(bin.head == block)) {
            // Guard against double-pushing the current head block.
            ReportDoublePushGuard();
            return;
        }
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

    // Diagnostic pointer scan: inspect bin and large-cache heads for pointers in [lo, hi).
    std::size_t AuditPointers(const void* lo, const void* hi) const;

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
    // 'next_refill' is the batch size for the next refill of this bin.
    // Starts small and ramps up on consecutive misses to optimize for both short-
    // and long-lived thread allocations.
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
        // Per-class retention cap: ~2 MB of spans per large class per thread,
        // bounded between 4 and 64 entries.
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
    // Debug invariants: verifies bin count matches actual list length.
    void AuditBin(std::size_t class_index) const;
#endif
};

} // namespace FastAlloc

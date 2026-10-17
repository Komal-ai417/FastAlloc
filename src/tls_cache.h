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

// ---------------------------------------------------------------------------
// Alloc-site hint filter (v12.2, packed v13.2): a 128-entry direct-mapped
// per-thread table recording (user pointer -> class|arena) at HANDOUT time.
// A free whose pointer hits the filter is provably a valid block start of
// that class (the allocator itself handed it out moments ago), so it skips
// both the span map and the slab-header line entirely - the freed block's
// cacheline is touched only by the freelist-link store, exactly like
// jemalloc. The table + accessors live in tls_cache (TLSCache::AllocateBlock
// records on the pop path); fast_alloc's free path consumes them.
//
// v13.2 packing: ONE 8-byte word per entry - 1 KB table, L1-resident.
//   word = ((ptr >> 4) << 13) | (arena << 9) | class      (small)
//        | ((ptr >> 4) << 13) | kHintLargeFlag            (large span)
//
// v13.7: entries ARE consumed on hit (the v13.4 no-consume experiment was
// reverted - it opened a double-free hole: a stale entry could route a block
// of a parked-EMPTY slab into a thread cache, and the span's later reuse
// would hand the same block out twice). The consume sends the second free
// down the map path, where kKindSlabParked markers and the poisoned slab
// header reject it; the bin-head guards only cover head-position duplicates.
// ---------------------------------------------------------------------------
constexpr std::size_t kAllocHintSlots = 128;
constexpr std::uint64_t kHintLargeFlag = 1ull << 56; // large-span handout

// Definition lives in tls_cache.cpp (single instance shared across TUs).
extern FAST_THREAD_LOCAL std::uint64_t g_alloc_hints[kAllocHintSlots];

inline void RecordAllocHint(const void* user, uint32_t cls, uint32_t arena) {
    const std::size_t idx =
        (reinterpret_cast<std::uintptr_t>(user) >> 4) & (kAllocHintSlots - 1);
    const std::uint64_t w =
        ((reinterpret_cast<std::uintptr_t>(user) >> 4) << 13) |
        (static_cast<std::uint64_t>(arena) << 9) | cls;
    g_alloc_hints[idx] = w;
}

inline void RecordAllocHintLarge(const void* user) {
    const std::size_t idx =
        (reinterpret_cast<std::uintptr_t>(user) >> 4) & (kAllocHintSlots - 1);
    g_alloc_hints[idx] =
        (((reinterpret_cast<std::uintptr_t>(user) >> 4) << 13) | kHintLargeFlag);
}

// Returns true when the pointer was recorded at handout. 'large' receives
// the kHintLargeFlag bit; cls/arena are only valid for small handouts.
inline bool LookupAllocHint(const void* ptr, uint32_t& cls, uint32_t& arena,
                            bool& large) {
    const std::size_t idx =
        (reinterpret_cast<std::uintptr_t>(ptr) >> 4) & (kAllocHintSlots - 1);
    const std::uint64_t w = g_alloc_hints[idx];
    if (FAST_UNLIKELY(w == 0)) return false;
    if (FAST_UNLIKELY((w >> 13) != (reinterpret_cast<std::uintptr_t>(ptr) >> 4)))
        return false; // different pointer evicted into this slot
    large = (w & kHintLargeFlag) != 0;
    const std::uint64_t meta = w & 0x1FFF;
    cls = static_cast<uint32_t>(meta & 0x1FF);
    arena = static_cast<uint32_t>(meta >> 9);
    if (FAST_UNLIKELY(cls >= NUM_SIZE_CLASSES)) return false; // corrupt meta
    // Consume (v13.7 RESTORED): the entry must not survive the first free.
    // Without the consume, a double free re-hits the stale entry and routes
    // the block into a thread cache even after its slab parked EMPTY - the
    // parked span's later reuse hands the same block out twice (double
    // presence). With the consume, the second free takes the map path,
    // where the kKindSlabParked marker + poisoned slab header reject it.
    g_alloc_hints[idx] = 0;
    return true;
}

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
        // Full plausibility predicate (>=64KB, <2^47, 16-aligned), inlined.
        // The upper bound is tightened by USER_OFFSET so the returned user
        // pointer (block + 16) satisfies the predicate as well - the caller
        // does not re-check it (one guard, not two).
        if (FAST_LIKELY(reinterpret_cast<std::uintptr_t>(block) >=
                            kMinCanonicalUserPtr &&
                        reinterpret_cast<std::uintptr_t>(block) <
                            kMaxCanonicalUserPtr - USER_OFFSET &&
                        (reinterpret_cast<std::uintptr_t>(block) & 0xF) == 0)) {
            bin.head = block->next;
            bin.count--;
            if (FAST_UNLIKELY(bin.head == nullptr)) {
                // Bin drained: reset the tail + arena hint so the next batch
                // intake / push re-derives them exactly.
                bin.tail = nullptr;
                bin.arena_hint = kArenaHintUnset;
            } else {
                // v13.4: record the handout in the alloc-site hint filter
                // right on the pop path (bin.arena_hint is already loaded;
                // single-arena bins only - a MIXED bin cannot name the
                // owning arena cheaply, and those frees take the span map).
                const uint32_t hint = bin.arena_hint;
                if (FAST_LIKELY(hint >= 1 && hint <= GlobalHeap::kNumArenas)) {
                    RecordAllocHint(reinterpret_cast<char*>(block) + USER_OFFSET,
                                    static_cast<uint32_t>(class_index),
                                    HintToArena(hint));
                }
            }
            return block;
        }
        if (FAST_UNLIKELY(block != nullptr)) {
            // Reset bin and route to slow path if bin head holds an invalid pointer.
            ReportBinHeadCorruption(class_index,
                                    reinterpret_cast<std::uintptr_t>(block));
            bin.head = nullptr;
            bin.tail = nullptr;
            bin.count = 0;
            bin.next_refill = 0;
            bin.arena_hint = kArenaHintUnset;
        }
        return AllocateBlockSlow(class_index);
    }

    void* AllocateBlockSlow(std::size_t class_index);

    inline void DeallocateBlock(std::size_t class_index, FreeBlock* block, uint32_t arena) {
        // Callers have already validated the block before reaching here:
        // the hint path by construction (the exact pointer was handed out
        // by this allocator), the span-cache / span-map paths via
        // IsBlockStart, the debug caller via the registry + canaries.
        CacheBin& bin = bins_[class_index];
        if (FAST_UNLIKELY(bin.head == block)) {
            // Guard against double-pushing the current head block.
            ReportDoublePushGuard();
            return;
        }
        // Arena-hint maintenance (v12): the owning arena comes from the
        // slab header (a hot line shared by every block of the slab) - the
        // free path never touches the freed block's own cacheline for
        // routing. A bin whose blocks all belong to one arena can later be
        // flushed to that arena's pending queue with a single O(1) splice.
        // v13.3: branch-free update (two cmovs) - this runs on EVERY free.
        {
            uint32_t hint = ArenaToHint(arena);
            if (FAST_UNLIKELY(hint > GlobalHeap::kNumArenas)) {
                hint = kArenaHintMixed; // corrupt header: force the guarded split
            }
            const uint32_t prev = bin.arena_hint;
            bin.arena_hint = (prev == kArenaHintUnset)
                                 ? hint
                                 : ((prev == hint) ? prev : kArenaHintMixed);
        }
        if (FAST_UNLIKELY(bin.head == nullptr)) {
            bin.tail = block; // empty -> non-empty: this is the oldest node
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

public:
    // (v13.4: handout_arena_hint() removed - the hint record moved onto the
    // AllocateBlock pop path itself.)

private:
    // Fresh construction: value-initialize everything.
    TLSCache();
    // Recycled construction (from the pool or a fresh zero-filled mmap):
    // the previous owner's destructor flushed and cleared every bin/large
    // bin/stats field, and mmap pages are zero - both guarantee the exact
    // state a fresh cache needs, so the ~17 KB of zeroing writes are
    // skipped. Thread spawn/exit cycles (thread-churn workload) pay only
    // the arena assignment.
    struct RecycledTag {};
    explicit TLSCache(RecycledTag);

    // ------------------------------------------------------------------
    // CacheBin: head + tail + count + adaptive refill sizing + arena hint.
    //
    // 'next_refill' is the batch size the NEXT refill of this bin will
    // request from the global heap. It starts small (kInitialRefill) and
    // doubles on every consecutive miss, capped at CACHE_LIMITS/2.
    // 'tail' is the oldest node (next == nullptr), maintained in O(1) by
    // push/pop so an overflow flush can splice the whole bin onto a pending
    // queue without walking it. 'arena_hint' tracks whether every block in
    // the bin belongs to one arena (see global_heap.h encoding: 0 = unset,
    // 1..NUM_ARENAS = arena+1, 0xFF = mixed) - the enabler for the O(1)
    // splice flush.
    // NO in-class initializers: a recycled thread cache is placement-new'd
    // over memory the previous owner's destructor already cleared bin-by-bin
    // (FlushToGlobalHeap resets every field unconditionally), and a fresh
    // cache lands on zero-filled mmap pages; skipping the 17 KB of zeroing
    // writes measurably shortens thread spawn/exit cycles. The default
    // constructor value-initializes everything for direct (non-pool)
    // construction.
    // ------------------------------------------------------------------
    struct CacheBin {
        FreeBlock* head;
        FreeBlock* tail;
        uint32_t count;
        uint32_t next_refill;
        uint32_t arena_hint;
    };
    static_assert(sizeof(CacheBin) == 32, "CacheBin layout contract");
    // NOTE: no NSDMI here on purpose - see TLSCache(RecycledTag). The default
    // ctor value-initializes bins_ explicitly; the recycled ctor must NOT
    // re-zero 16 KB the previous owner's destructor already cleared.
    std::array<CacheBin, NUM_SIZE_CLASSES> bins_;
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
    // 4096 ops (was 256): every flush does ~10 relaxed RMWs on the shared
    // global counter lines; at 256 ops the flush rate made those lines
    // ping-pong between cores and cost measurable T>=2 throughput (the
    // counters are diagnostics - flush-granularity accuracy is documented
    // as approximate in release builds).
    // ------------------------------------------------------------------
    static constexpr uint64_t kStatsFlushOps = 4096;
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

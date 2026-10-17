#pragma once
#include "slab.h"
#include "fast_alloc_config.h"
#include <atomic>
#include <array>
#include <thread>
#include <cstdint>

#include "debug_aid.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace FastAlloc {

// Portable pause/yield intrinsic for spin loops.
#if defined(_MSC_VER)
#define FAST_CPU_PAUSE() _mm_pause()
#elif defined(__i386__) || defined(__x86_64__)
#define FAST_CPU_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__) || defined(__arm__)
#define FAST_CPU_PAUSE() asm volatile("yield" ::: "memory")
#else
#define FAST_CPU_PAUSE() std::this_thread::yield()
#endif

class ScopedSpinLock {
    std::atomic_flag& flag_;
public:
    explicit ScopedSpinLock(std::atomic_flag& f) : flag_(f) {
        int spins = 0;
        while (flag_.test_and_set(std::memory_order_acquire)) {
            if (++spins > 64) {
                std::this_thread::yield();
                spins = 0;
            } else {
                FAST_CPU_PAUSE();
            }
        }
    }
    ~ScopedSpinLock() { flag_.clear(std::memory_order_release); }
    ScopedSpinLock(const ScopedSpinLock&) = delete;
    ScopedSpinLock& operator=(const ScopedSpinLock&) = delete;
};

// TLS bin arena-hint encoding (TLSCache::CacheBin). A bin that only ever
// saw blocks of one arena can be flushed to that arena's pending queue with
// a single O(1) Treiber splice; unset/mixed forces the guarded arena-split
// walk. 0 encodes "unset" so a freshly mmap'd (zero-filled) or recycled
// (flush-cleared) thread cache starts in the correct state with no
// initialization writes.
constexpr uint32_t kArenaHintUnset = 0;
constexpr uint32_t kArenaHintMixed = 0xFFu;
inline constexpr uint32_t ArenaToHint(uint32_t arena) { return arena + 1; }
inline constexpr uint32_t HintToArena(uint32_t hint) { return hint - 1; } // valid for 1..NUM_ARENAS

class GlobalHeap {
public:
    static GlobalHeap& GetInstance();

    static constexpr std::size_t NUM_ARENAS = 16;
    static constexpr std::size_t kNumArenas = NUM_ARENAS; // public: TLS hint checks

    uint32_t GetNextArena() {
        return next_arena_.fetch_add(1, std::memory_order_relaxed) % NUM_ARENAS;
    }

    // Fetch a block from a specific size class
    void* AllocateBlock(std::size_t class_index, uint32_t arena_index);

    // Fetch a batch of blocks to re-fill thread cache. Returns the batch
    // head; *out_tail receives the batch's last node (next == nullptr) so
    // the caller can park the rest in its TLS bin while keeping an exact
    // tail pointer for the next O(1) flush splice. *out_foreign is set when
    // the batch contains blocks from a foreign arena (cross-arena steal).
    FreeBlock* AllocateBatch(std::size_t class_index, std::size_t target_count, std::size_t& actual_count, uint32_t arena_index,
                             FreeBlock** out_tail = nullptr, bool* out_foreign = nullptr);

    // Return a block directly to its slab (slow, kept for compatibility)
    void DeallocateBlock(Slab* slab, void* ptr);

    // Return a batched linked list of blocks back to their respective slabs.
    // Safe for mixed size classes: blocks are routed by their own slab's
    // class index.
    void DeallocateBatch(FreeBlock* head);

    // Single-class variant used by the TLS overflow flush: every block in
    // 'head' belongs to 'class_index'. Skips the 512-entry per-class split
    // (two 4 KB stack tables) of the general path; only the 16-entry arena
    // split remains because a TLS bin may hold blocks foreign to this
    // thread's arena (cross-thread frees).
    void DeallocateBatchClass(std::size_t class_index, FreeBlock* head);

    // O(1) overflow flush: splice a whole TLS bin (head..tail, 'count'
    // nodes, 'hint' = the bin's arena hint) to the per-arena pending
    // queues. A valid single-arena hint avoids touching the blocks at all;
    // a mixed hint takes the guarded arena-split walk. No per-block slab
    // work happens here - the owning arena's allocations drain the queue
    // via AllocateBatch's direct feed, and the global pending-bytes cap
    // makes the pusher drain its own push when nobody allocates (pure
    // free-storm workloads), so retention stays bounded.
    void FlushBinToPending(std::size_t class_index, FreeBlock* head, FreeBlock* tail,
                           std::size_t count, uint32_t hint, uint32_t my_arena);

    // Lock-free deferred hand-off of a single-class bin list to the
    // per-arena pending queues (thread-exit path).
    void DeferBinToPending(std::size_t class_index, FreeBlock* head);

    // Bypasses slabs for allocations > MAX_SLAB_SIZE, uses global large block cache
    void* AllocateLarge(std::size_t size);
    void  DeallocateLarge(void* ptr, std::size_t size);

    // True once process shutdown has begun. Thread-cache destructors check
    // this and skip reflushing memory back into the global heap, avoiding
    // destruction-order conflicts with late TLS callbacks and lock-holding exits.
    bool IsShuttingDown() const { return shutting_down_.load(std::memory_order_acquire); }

    // Drop every cached page span back to the OS. Thread-safe; acquires each
    // bin lock in turn. Returns the number of bytes released.
    std::size_t PurgePageCache();

    // Reports (stderr) every arena slab-list head, pending queue head, and
    // page-bin head pointing into [lo, hi); returns the hit count.
    // Read-only inspection of internal pointer arrays.
    std::size_t AuditPointers(const void* lo, const void* hi);

    // OOM-injection seam for tests: when set, the next N page allocations
    // fail. Default null. Only consulted on the slow path.
    static bool ShouldFailAllocationForTest();

private:
    GlobalHeap();
    ~GlobalHeap() = default;
    GlobalHeap(const GlobalHeap&) = delete;
    GlobalHeap& operator=(const GlobalHeap&) = delete;

    std::atomic<uint32_t> next_arena_;
    std::atomic<bool> shutting_down_{false};

    // Lock-free pending return queue per size class. 'len' is an advisory
    // relaxed block count: it lets allocators skip empty queues without
    // taking the bin lock, bounds the feed's remainder walk, and drives the
    // global pending-bytes cap. It may transiently under/over-count across
    // concurrent push/drain pairs - every consumer treats it as a hint.
    struct alignas(64) PendingList {
        std::atomic<FreeBlock*> head{nullptr};
        std::atomic<uint32_t> len{0};
        char _padding[64 - sizeof(std::atomic<FreeBlock*>) - sizeof(std::atomic<uint32_t>)];
    };
    static_assert(sizeof(PendingList) == 64, "PendingList must fill one cache line");

    struct alignas(64) Arena {
        std::array<std::atomic_flag, NUM_SIZE_CLASSES> class_locks_{};
        std::array<Slab*, NUM_SIZE_CLASSES> partial_slabs_{};
        std::array<Slab*, NUM_SIZE_CLASSES> full_slabs_{};
        // Retained empty slabs per class (bounded: kEmptySlabKeepPerClass
        // per class + the global empty-slab byte cap). An emptied slab is
        // parked here instead of being released to the OS, so the next slab
        // for the same class reuses a resident span: no madvise, no mmap,
        // no first-touch fault cycle. fast_alloc_purge() releases them all.
        std::array<Slab*, NUM_SIZE_CLASSES> empty_slab_cache_{};
        std::array<uint32_t, NUM_SIZE_CLASSES> empty_slab_counts_{};
        std::array<PendingList, NUM_SIZE_CLASSES> pending_returns_{};

        Arena() {
            for (auto& lock : class_locks_) lock.clear();
        }
    };

    std::array<Arena, NUM_ARENAS> arenas_;

    // Drain pending returns into slabs
    void DrainPendingReturns(uint32_t arena_index, std::size_t class_index);

    // Process one popped single-arena single-class pending list into its
    // slabs (the loop body of DrainPendingReturns). Also used to fold
    // over-long pending lists back into warm slab free lists.
    void ProcessPendingListLocked(uint32_t arena_index, std::size_t class_index, FreeBlock* pending);

    // Split a single-class free list by each block's own slab arena.
    // heads/tails/counts are caller-provided arrays of NUM_ARENAS entries.
    void SplitListByArena(FreeBlock* head, FreeBlock** heads, FreeBlock** tails,
                          std::size_t* counts = nullptr);

    // Treiber splice of [head..tail] onto the (arena, class) pending queue.
    void SpliceToPending(uint32_t arena_index, std::size_t class_index,
                         FreeBlock* head, FreeBlock* tail, std::size_t count);

    // Global pending-bytes cap enforcement: when the cap is exceeded the
    // pusher drains the (arena, class) queue it just fed (bounded eager
    // work) so pure free-storm workloads cannot park unbounded memory.
    void EnforcePendingCap(uint32_t arena_index, std::size_t class_index);

    Slab* AllocateNewSlab(std::size_t class_index, uint32_t arena_index);

    // Empty-slab cache helpers (class lock of 'class_index' must be held).
    void ParkEmptySlabLocked(uint32_t arena_index, std::size_t class_index, Slab* slab);
    Slab* TakeEmptySlabLocked(uint32_t arena_index, std::size_t class_index);
    void ReleaseEmptySlabsOfClassLocked(uint32_t arena_index, std::size_t class_index); // purge path

    // Pop a pending queue, cut up to 'want' nodes for the caller and re-park
    // the remainder with one CAS. Source (arena,class) lock must be held.
    FreeBlock* FeedFromPendingLocked(PendingList& pl, std::size_t class_index,
                                     std::size_t want, std::size_t& taken_out,
                                     FreeBlock** taken_tail_out);

    // Helper to extract blocks and advance slab pointers
    FreeBlock* ExtractBlocksFromSlab(Slab* slab, std::size_t class_index, std::size_t target_count, std::size_t& actual_count, uint32_t arena_index,
                                     FreeBlock** out_tail = nullptr);

    // Shared logic of the two deallocation paths (locked processing of a
    // single-class, single-arena list).
    void ProcessBatchLocked(uint32_t arena_index, std::size_t class_index, FreeBlock* list);
};

} // namespace FastAlloc

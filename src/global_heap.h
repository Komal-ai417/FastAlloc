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

// Portable pause/yield intrinsic for spin loops (audit O3: ARM emitted no
// pause instruction before, degrading contended performance).
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

class GlobalHeap {
public:
    static GlobalHeap& GetInstance();

    uint32_t GetNextArena() {
        return next_arena_.fetch_add(1, std::memory_order_relaxed) % NUM_ARENAS;
    }

    // Fetch a block from a specific size class
    void* AllocateBlock(std::size_t class_index, uint32_t arena_index);

    // Fetch a batch of blocks to re-fill thread cache
    FreeBlock* AllocateBatch(std::size_t class_index, std::size_t target_count, std::size_t& actual_count, uint32_t arena_index);

    // Return a block directly to its slab
    void DeallocateBlock(Slab* slab, void* ptr);

    // Return a batched linked list of blocks back to their respective slabs.
    // Safe for mixed size classes: blocks are routed by their OWN slab's
    // class index (audit N2 fix - previously derived from the first block only).
    void DeallocateBatch(FreeBlock* head);

    // Single-class variant used by the TLS overflow flush: every block in
    // 'head' belongs to 'class_index'. Skips the 512-entry per-class split
    // (two 4 KB stack tables) of the general path; only the 16-entry arena
    // split remains because a TLS bin may hold blocks foreign to this
    // thread's arena (cross-thread frees).
    void DeallocateBatchClass(std::size_t class_index, FreeBlock* head);

    // Lock-free deferred hand-off of a single-class bin list to the
    // per-arena pending queues (thread-exit path). No class lock is taken
    // and no per-block slab work happens here; the next thread that
    // refills one of these (arena, class) pairs pays that cost via
    // DrainPendingReturns. Blocks are routed by their own slab's arena.
    void DeferBinToPending(std::size_t class_index, FreeBlock* head);

    // Bypasses slabs for allocations > MAX_SLAB_SIZE, uses global large block cache
    void* AllocateLarge(std::size_t size);
    void  DeallocateLarge(void* ptr, std::size_t size);

    // True once process shutdown has begun. Thread-cache destructors check
    // this and skip reflushing memory back into the global heap, which
    // avoids two hazards:
    //   (a) destruction-order UB between the GlobalHeap singleton and late
    //       TLS callbacks (audit C7), and
    //   (b) Windows process-exit deadlock when a thread was terminated while
    //       holding a page-bin lock (audit N3).
    bool IsShuttingDown() const { return shutting_down_.load(std::memory_order_acquire); }

    // Drop every cached page span back to the OS. Thread-safe; acquires each
    // bin lock in turn. Returns the number of bytes released.
    std::size_t PurgePageCache();

    // OOM-injection seam for tests: when set, the next N page allocations
    // fail. Default null. Only consulted on the slow path.
    static bool ShouldFailAllocationForTest();

private:
    GlobalHeap();
    ~GlobalHeap() = default;
    GlobalHeap(const GlobalHeap&) = delete;
    GlobalHeap& operator=(const GlobalHeap&) = delete;

    static constexpr std::size_t NUM_ARENAS = 16;
    std::atomic<uint32_t> next_arena_;
    std::atomic<bool> shutting_down_{false};

    // Lock-free pending return queue per size class
    struct alignas(64) PendingList {
        std::atomic<FreeBlock*> head{nullptr};
        char _padding[64 - sizeof(std::atomic<FreeBlock*>)];
    };
    static_assert(sizeof(PendingList) == 64, "PendingList must fill one cache line");

    struct alignas(64) Arena {
        std::array<std::atomic_flag, NUM_SIZE_CLASSES> class_locks_{};
        std::array<Slab*, NUM_SIZE_CLASSES> partial_slabs_{};
        std::array<Slab*, NUM_SIZE_CLASSES> full_slabs_{};
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
    // heads/tails are caller-provided arrays of NUM_ARENAS entries.
    void SplitListByArena(FreeBlock* head, FreeBlock** heads, FreeBlock** tails);

    Slab* AllocateNewSlab(std::size_t class_index, uint32_t arena_index);

    // Helper to extract blocks and advance slab pointers
    FreeBlock* ExtractBlocksFromSlab(Slab* slab, std::size_t class_index, std::size_t target_count, std::size_t& actual_count, uint32_t arena_index);

    // Shared logic of the two deallocation paths (locked processing of a
    // single-class, single-arena list).
    void ProcessBatchLocked(uint32_t arena_index, std::size_t class_index, FreeBlock* list);
};

} // namespace FastAlloc

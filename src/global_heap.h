#pragma once
#include "slab.h"
#include "fast_alloc_config.h"
#include <atomic>
#include <array>
#include <thread>
#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace FastAlloc {

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
#if defined(_MSC_VER)
                _mm_pause();
#elif defined(__i386__) || defined(__x86_64__)
                __builtin_ia32_pause();
#endif
            }
        }
    }
    ~ScopedSpinLock() { flag_.clear(std::memory_order_release); }
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

    // Return a batched linked list of blocks back to their respective slabs natively
    void DeallocateBatch(FreeBlock* head);

    // Bypasses slabs for allocations > MAX_SLAB_SIZE, uses global large block cache
    void* AllocateLarge(std::size_t size);
    void  DeallocateLarge(void* ptr, std::size_t size);

private:
    GlobalHeap() : next_arena_(0) {}
    ~GlobalHeap() = default;

    static constexpr std::size_t NUM_ARENAS = 16;
    std::atomic<uint32_t> next_arena_;

    // Lock-free pending return queue per size class
    struct alignas(64) PendingList {
        std::atomic<FreeBlock*> head{nullptr};
        char _padding[64 - sizeof(std::atomic<FreeBlock*>)];
    };

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

    Slab* AllocateNewSlab(std::size_t class_index, uint32_t arena_index);

    // Helper to extract blocks and advance slab pointers
    FreeBlock* ExtractBlocksFromSlab(Slab* slab, std::size_t class_index, std::size_t target_count, std::size_t& actual_count, uint32_t arena_index);
};

} // namespace FastAlloc
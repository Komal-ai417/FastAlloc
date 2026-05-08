#pragma once
#include "slab.h"
#include "fast_alloc_config.h"
#include <atomic>
#include <array>
#include <thread>

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
            if (++spins > 32) {
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

    // Fetch a block from a specific size class
    void* AllocateBlock(std::size_t class_index);

    // Fetch a batch of blocks to re-fill thread cache
    FreeBlock* AllocateBatch(std::size_t class_index, std::size_t target_count, std::size_t& actual_count);

    // Return a block directly to its slab
    void DeallocateBlock(Slab* slab, void* ptr);

    // Return a batched linked list of blocks back to their respective slabs natively
    void DeallocateBatch(FreeBlock* head);

    // Lock-free deferred return for use during thread teardown (avoids loader lock deadlock)
    void DeferredDeallocateBatch(FreeBlock* head);

    // Bypasses slabs for allocations > MAX_SLAB_SIZE
    void* AllocateLarge(std::size_t size);
    void  DeallocateLarge(void* ptr, std::size_t size);

private:
    GlobalHeap() : class_locks_() {
        for (auto& lock : class_locks_) lock.clear();
    }
    ~GlobalHeap() = default;

    static constexpr std::size_t NUM_STRIPES = 16;
    std::array<std::atomic_flag, NUM_STRIPES> class_locks_;

    std::size_t ClassToStripe(std::size_t class_index) const {
        return (class_index * 7) & (NUM_STRIPES - 1);
    }
    
    // Arrays of intrusive linked lists for slabs
    std::array<Slab*, NUM_SIZE_CLASSES> partial_slabs_{};
    std::array<Slab*, NUM_SIZE_CLASSES> full_slabs_{};

    // Lock-free pending return queue per stripe
    struct alignas(64) PendingList {
        std::atomic<FreeBlock*> head{nullptr};
        char _padding[64 - sizeof(std::atomic<FreeBlock*>)];
    };
    std::array<PendingList, NUM_STRIPES> pending_returns_{};

    struct SlabToFree {
        void* ptr;
        std::size_t size;
    };
    static constexpr std::size_t MAX_DEFERRED_FREE = 128;

    // Drain pending returns into slabs (called under appropriate stripe lock)
    void DrainPendingReturns(std::size_t stripe_index, SlabToFree* deferred_slabs, std::size_t& deferred_count);

    Slab* AllocateNewSlab(std::size_t class_index);

    // Helper to extract blocks and advance slab pointers
    FreeBlock* ExtractBlocksFromSlab(Slab* slab, std::size_t class_index, std::size_t target_count, std::size_t& actual_count);
};

} // namespace FastAlloc

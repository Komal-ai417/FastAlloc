#pragma once
#include "slab.h"
#include "fast_alloc_config.h"
#include <mutex>
#include <array>

namespace FastAlloc {

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

    // Bypasses slabs for allocations > MAX_SLAB_SIZE
    void* AllocateLarge(std::size_t size);
    void  DeallocateLarge(void* ptr, std::size_t size);

private:
    GlobalHeap() = default;
    ~GlobalHeap() = default;

    std::mutex mutex_;
    
    // Arrays of intrusive linked lists for slabs
    std::array<Slab*, NUM_SIZE_CLASSES> partial_slabs_{};
    std::array<Slab*, NUM_SIZE_CLASSES> full_slabs_{};

    Slab* AllocateNewSlab(std::size_t class_index);
};

} // namespace FastAlloc

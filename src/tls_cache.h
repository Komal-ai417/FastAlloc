#pragma once
#include "slab.h"
#include "fast_alloc_config.h"
#include <array>

namespace FastAlloc {

class TLSCache {
public:
    static TLSCache& Get();

    // Allocate a block for the given size class.
    // Returns the raw FreeBlock* (including the slab header inside).
    void* AllocateBlock(std::size_t class_index);

    // Deallocate a raw FreeBlock* back to the cache (or global heap if cache is full)
    void DeallocateBlock(std::size_t class_index, FreeBlock* block);

private:
    TLSCache() = default;
    ~TLSCache();

    std::array<FreeBlock*, NUM_SIZE_CLASSES> fast_bins_{};
    std::array<std::size_t, NUM_SIZE_CLASSES> counts_{};

    // To prevent hoarding and unbounded memory growth
    static constexpr std::size_t MAX_CACHE_SIZE = 128; 
};

} // namespace FastAlloc

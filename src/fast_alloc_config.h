#pragma once
#include <cstdint>
#include <cstddef>

namespace FastAlloc {

// Configuration constants for the allocator
constexpr std::size_t PAGE_SIZE = 4096; // Typical OS page size
constexpr std::size_t MAX_SLAB_SIZE = 8192; // Allocations above this size bypass TLS/Slabs
constexpr std::size_t ALIGNMENT = 16;   // 16-byte alignment

// Number of size classes: 16, 32, ..., 8192 = 512 classes
constexpr std::size_t NUM_SIZE_CLASSES = MAX_SLAB_SIZE / ALIGNMENT;

// Map requested size to size class index (0-based)
inline std::size_t SizeToClassIndex(std::size_t size) {
    if (size == 0) return 0;
    return ((size + ALIGNMENT - 1) / ALIGNMENT) - 1;
}

// Map class index back to actual block size
inline std::size_t ClassIndexToSize(std::size_t index) {
    return (index + 1) * ALIGNMENT;
}

struct alignas(16) LargeAllocHeader {
    void* slab; // Should be Slab*, but void* avoids circular dependency
    std::size_t alloc_size;
};

} // namespace FastAlloc

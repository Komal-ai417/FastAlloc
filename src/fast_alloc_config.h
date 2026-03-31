#pragma once
#include <cstdint>
#include <cstddef>

namespace FastAlloc {

// Configuration constants for the allocator
constexpr std::size_t PAGE_SIZE = 4096; // Typical OS page size
constexpr std::size_t MAX_SLAB_SIZE = 1024; // Allocations above this size bypass TLS/Slabs
constexpr std::size_t ALIGNMENT = 16;   // 16-byte alignment

// Number of size classes: 16, 32, 48, ..., 1024 = 64 classes
constexpr std::size_t NUM_SIZE_CLASSES = MAX_SLAB_SIZE / ALIGNMENT;

// Map requested size to size class index (0-based)
inline std::size_t SizeToClassIndex(std::size_t size) {
    if (size == 0) return 0;
    // e.g. size=15 -> (15+15)/16 - 1 = 0 (size 16)
    // size=16 -> (16+15)/16 - 1 = 0
    // size=17 -> (17+15)/16 - 1 = 1 (size 32)
    return ((size + ALIGNMENT - 1) / ALIGNMENT) - 1;
}

// Map class index back to actual block size
inline std::size_t ClassIndexToSize(std::size_t index) {
    return (index + 1) * ALIGNMENT;
}

} // namespace FastAlloc

#pragma once
#include <cstdint>
#include <cstddef>
#include <array>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define FAST_LIKELY(x) __builtin_expect(!!(x), 1)
#define FAST_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define FAST_LIKELY(x) (x)
#define FAST_UNLIKELY(x) (x)
#endif

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
    return (size - 1) >> 4;
}

// Map class index back to actual block size
inline std::size_t ClassIndexToSize(std::size_t index) {
    return (index + 1) * ALIGNMENT;
}

constexpr std::array<uint32_t, NUM_SIZE_CLASSES> ComputeCacheLimits() {
    std::array<uint32_t, NUM_SIZE_CLASSES> limits{};
    for (std::size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
        std::size_t size = (i + 1) * ALIGNMENT;
        if (size <= 64) limits[i] = 16384;
        else if (size <= 256) limits[i] = 8192;
        else if (size <= 1024) limits[i] = 4096;
        else if (size <= 4096) limits[i] = 1024;
        else if (size <= 8192) limits[i] = 512;
        else limits[i] = 128;
    }
    return limits;
}
constexpr std::array<uint32_t, NUM_SIZE_CLASSES> CACHE_LIMITS = ComputeCacheLimits();

struct alignas(16) LargeAllocHeader {
    void* slab; // Should be Slab*, but void* avoids circular dependency
    std::size_t alloc_size;
};

static constexpr std::size_t NUM_LARGE_CLASSES = 64;
static constexpr std::size_t LARGE_CLASS_BASE = 12; // 4096 bytes (page size)

inline std::size_t LargeSizeToClass(std::size_t size) {
    if (size <= (1ULL << LARGE_CLASS_BASE)) return 0;
    unsigned long clz = 0;
#if defined(_MSC_VER)
#if defined(_WIN64)
    _BitScanReverse64(&clz, size - 1);
#else
    _BitScanReverse(&clz, (unsigned long)(size - 1));
#endif
#else
    clz = 63 - __builtin_clzll((unsigned long long)(size - 1));
#endif
    std::size_t cls = clz - LARGE_CLASS_BASE + 1;
    return cls < NUM_LARGE_CLASSES ? cls : NUM_LARGE_CLASSES - 1;
}

} // namespace FastAlloc
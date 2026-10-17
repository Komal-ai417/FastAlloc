#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <array>

// ============================================================================
// Platform detection & Windows version lockdown
// ============================================================================
// FlsAlloc/FlsGetValue/FlsSetValue/FlsFree require Windows Vista (0x0600)+.
// Ensures _WIN32_WINNT is set before windows.h is included.
#if defined(_WIN32)
#if !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0600
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#endif

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

// ---------------------------------------------------------------------------
// Cross-compiler builtins (MSVC has no __builtin_*; these shims keep every
// translation unit portable across GCC/Clang/MSVC. All are release-path.)
// ---------------------------------------------------------------------------
#if defined(_MSC_VER)
#define FAST_RETURN_ADDRESS() _ReturnAddress()
#else
#define FAST_RETURN_ADDRESS() __builtin_return_address(0)
#endif

// Count trailing zeros of a NONZERO 64-bit word.
inline unsigned fast_ctzll(unsigned long long v) {
#if defined(_MSC_VER) && defined(_WIN64)
    unsigned long i = 0;
    _BitScanForward64(&i, v);
    return static_cast<unsigned>(i);
#elif defined(_MSC_VER)
    unsigned long i = 0;
    unsigned long lo = static_cast<unsigned long>(v);
    if (lo) { _BitScanForward(&i, lo); return static_cast<unsigned>(i); }
    _BitScanForward(&i, static_cast<unsigned long>(v >> 32));
    return static_cast<unsigned>(i) + 32u;
#else
    return static_cast<unsigned>(__builtin_ctzll(v));
#endif
}

// Read-only getenv without MSVC C4996 deprecation noise (C4996 is a hard
// error under /W4 /WX). MSVC's _dupenv_s alternative hands back a malloc'd
// copy the caller must free - pointless for a read of a tunable.
inline const char* fast_getenv(const char* name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    return std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

// ============================================================================
// Debug instrumentation framework
// ============================================================================
// FASTALLOC_DEBUG enables (all zero-cost when not defined):
//   - Front canary (4 bytes inside FreeBlock padding) -> detects underflow
//   - Rear red-zone (rest of block after user size)    -> detects overflow
//   - Free poisoning (0xDD) + fresh pattern (0xCD)     -> detects UAF writes
//   - Magic cookies on Slab & large headers            -> detects invalid free
//   - Pointer registry (exact double-free / invalid-free / leak detection)
//   - Runtime asserts on invariants
// FASTALLOC_LOGGING enables the event logging subsystem (independent of
// FASTALLOC_DEBUG so release builds can log if desired).
#if defined(FASTALLOC_DEBUG)
#define FASTALLOC_DEBUG_ENABLED 1
#else
#define FASTALLOC_DEBUG_ENABLED 0
#endif

#if defined(FASTALLOC_LOGGING) || FASTALLOC_DEBUG_ENABLED
#define FASTALLOC_LOGGING_ENABLED 1
#else
#define FASTALLOC_LOGGING_ENABLED 0
#endif

namespace FastAlloc {

// Configuration constants for the allocator
constexpr std::size_t PAGE_SIZE = 4096;   // Minimum/typical OS page size (fallback).
                                          // Runtime page size is queried from OSMemory::GetPageSize().
constexpr std::size_t MAX_SLAB_SIZE = 8192;   // Allocations above this size bypass TLS/Slabs
constexpr std::size_t ALIGNMENT = 16;         // 16-byte alignment

// Lowest address any valid user pointer or freelist node can hold.
// Memory below 64 KB is reserved or unmapped across supported operating systems.
constexpr std::uintptr_t kMinCanonicalUserPtr = 0x10000;

// Upper canonical address bound for 48-bit user virtual address space.
constexpr std::uintptr_t kMaxCanonicalUserPtr = 0x0000800000000000ull;

// Validates whether a pointer is plausible for a FastAlloc allocation:
//   - >= 64 KB (avoids null page and small-integer values)
//   - < 2^47 (canonical user-space address)
//   - 16-byte aligned (all FastAlloc blocks maintain 16-byte alignment)
inline bool IsPlausibleHeapPtr(const void* p) {
    const std::uintptr_t v = reinterpret_cast<std::uintptr_t>(p);
    return v >= kMinCanonicalUserPtr && v < kMaxCanonicalUserPtr &&
           (v & (std::uintptr_t)0xF) == 0;
}

// Effective maximum request size served by the slab path
constexpr std::size_t USER_OFFSET = 16;
constexpr std::size_t MAX_SMALL_REQUEST = MAX_SLAB_SIZE - USER_OFFSET; // 8176

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

// ---------------------------------------------------------------------------
// Debug sizing helpers.
//
// In DEBUG builds every block reserves RED_ZONE_RESERVE tail bytes so the
// rear red zone is NEVER empty - a request that exactly fills its class
// would otherwise have zero overflow-detection slack (a request of 16 in a
// 32-byte block leaves usable == requested).
// ---------------------------------------------------------------------------
#if FASTALLOC_DEBUG_ENABLED
constexpr std::size_t RED_ZONE_RESERVE = 8;
inline std::size_t RequestToClass(std::size_t size) {
    return SizeToClassIndex(size + USER_OFFSET + RED_ZONE_RESERVE);
}
inline std::size_t UsableSize(std::size_t class_index) {
    return ClassIndexToSize(class_index) - USER_OFFSET - RED_ZONE_RESERVE;
}
inline std::size_t MaxSmallRequest() { return MAX_SLAB_SIZE - USER_OFFSET - RED_ZONE_RESERVE; }
#else
inline std::size_t RequestToClass(std::size_t size) {
    return SizeToClassIndex(size + USER_OFFSET);
}
inline std::size_t UsableSize(std::size_t class_index) {
    return ClassIndexToSize(class_index) - USER_OFFSET;
}
inline std::size_t MaxSmallRequest() { return MAX_SLAB_SIZE - USER_OFFSET; }
#endif

// Per-class TLS bin limits (original count-based policy; measured best on
// the alloc side: refill batches stay large, and the pending-queue
// inventory is bounded by the global pending-bytes cap instead).
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

// ---------------------------------------------------------------------------
// Division-free block-start validation (span-map free path).
//
// fast_free must verify that a freed pointer is exactly a BLOCK USER START
// of the slab the span map returned: off = ptr - first_user must be an exact
// multiple of block_size and below total_blocks * block_size. A hardware
// div/mod costs ~20-40 cycles with a runtime divisor, so each class
// precomputes the classic round-up magic: m = ceil(2^34 / d). For every
// off < 2^21 (spans are <= 2 MB) and d <= 8192, q = (off * m) >> 34 equals
// floor(off / d) EXACTLY (error term x*e/(d*2^34) < 1/d because
// x*e < 2^21 * 2^13 = 2^34), so the check is q < total_blocks &&
// q * block_size == off - six-ish instructions, no division.
// ---------------------------------------------------------------------------
constexpr std::array<uint64_t, NUM_SIZE_CLASSES> ComputeClassMagics() {
    std::array<uint64_t, NUM_SIZE_CLASSES> magics{};
    for (std::size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
        std::size_t d = (i + 1) * ALIGNMENT; // block_size of class i
        magics[i] = ((1ull << 34) + d - 1) / d; // ceil(2^34 / d)
    }
    return magics;
}
constexpr std::array<uint64_t, NUM_SIZE_CLASSES> CLASS_MAGICS = ComputeClassMagics();

struct alignas(16) LargeAllocHeader {
    // 'slab' does double duty:
    //   release builds : always nullptr for large blocks
    //   debug builds   : LARGE_MAGIC while the block is live, so that
    //                    fast_free can validate provenance.
    // When parked in the large free-cache the field aliases LargeFreeEntry::next.
    void* slab;
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

// ============================================================================
// Debug patterns & magic values
// ============================================================================
namespace debug {
    constexpr uint32_t FRONT_CANARY     = 0xC8C8C8C8u; // written at alloc, checked at free (underflow)
    constexpr uint64_t SLAB_MAGIC       = 0x5A8B0D5A8B0D0001ull; // validates slab provenance
    constexpr uint64_t LARGE_MAGIC      = 0xFA578AFA578A0002ull; // validates large header
    constexpr unsigned char RED_ZONE    = 0xBB; // rear red-zone fill (overflow)
    constexpr unsigned char POISON_FREE = 0xDD; // freed-block fill (UAF write detection)
    constexpr unsigned char FRESH       = 0xCD; // never-touched block fill
} // namespace debug

} // namespace FastAlloc

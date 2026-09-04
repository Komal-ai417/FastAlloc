#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <array>

// ============================================================================
// Platform detection & Windows version lockdown
// ============================================================================
// FlsAlloc/FlsGetValue/FlsSetValue/FlsFree require Windows Vista (0x0600)+.
// If the consuming project (or an old SDK) has not pinned _WIN32_WINNT, pin
// it here BEFORE any windows.h include so the build cannot silently rot.
// (Audit H5 fix)
#if defined(_WIN32)
#if !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0600
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
constexpr std::size_t PAGE_SIZE = 4096;   // Minimum/typical OS page size (fallback only).
                                          // The RUNTIME page size must be taken from
                                          // OSMemory::GetPageSize() for all rounding
                                          // arithmetic (audit N1 fix).
constexpr std::size_t MAX_SLAB_SIZE = 8192;   // Allocations above this size bypass TLS/Slabs
constexpr std::size_t ALIGNMENT = 16;         // 16-byte alignment

// Effective maximum request size served by the slab path (audit M4 fix:
// this named constant now documents what the code actually does).
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
    // 'slab' does double duty:
    //   release builds : always nullptr for large blocks
    //   debug builds   : LARGE_HEADER_MAGIC while the block is live, so that
    //                    fast_free can validate provenance (audit C3 fix).
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
// Debug patterns & magic values (audit C1/C2/C3 + Final Report section 4)
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

// ============================================================================
// Public API behaviour tests: malloc / free / calloc / realloc, zero-size
// semantics, null handling, alignment guarantees, calloc overflow guard.
// (Audit gaps: C6 zero-size policy, alignment coverage, null handling.)
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"
#include "fast_alloc_config.h"
#include "test_hooks.h"

#include <cstring>
#include <cstdint>
#include <cerrno>
#include <vector>
#include <set>

using namespace FastAlloc;

// ---------------------------------------------------------------------------
TEST(ApiTest, MallocFreeBasic) {
    void* p = fast_malloc(64);
    ASSERT_NE(p, nullptr);
    std::memset(p, 0xAB, 64);
    fast_free(p);
}

TEST(ApiTest, FreeNullIsNoop) {
    fast_free(nullptr); // must not crash
    SUCCEED();
}

// v9 guard: a freed pointer below 64 KB can never be a FastAlloc block
// (Linux keeps the first 64 KB unmapped, Windows reserves it). The old code
// dereferenced ptr-16 immediately and faulted at the NULL page - the exact
// signature of the Sep-2026 CI runner crash (ptr = 0x10, si_addr = 0x0).
// The guard must drop the pointer and keep serving instead of crashing,
// in release AND debug builds.
TEST(ApiTest, FreeNonCanonicalPointerIsDroppedNotFatal) {
    fast_free(reinterpret_cast<void*>(0x10));           // the runner-crash value
    fast_free(reinterpret_cast<void*>(0x1000));        // anywhere in the null page
    fast_free_sized(reinterpret_cast<void*>(0x20), 8); // sized path, same guard
    void* live = fast_malloc(32);                       // allocator still serviceable
    ASSERT_NE(live, nullptr);
    std::memset(live, 0x5A, 32);
    fast_free(live);
    SUCCEED();
}

// v10: fast_free_sized with a WRONG size hint must route the block through
// its TRUE class (from the block's own header), never into the hinted
// class's TLS bin. Misrouting handed the next allocation from that bin an
// undersized block - the seed of every downstream freelist corruption.
TEST(ApiTest, FreeSizedWrongHintRoutesToTrueClass) {
    void* p = fast_malloc(8); // true class: RequestToClass(8 + 16) -> 1
    ASSERT_NE(p, nullptr);
    // Free with a hint that maps to a DIFFERENT class (64 -> class 4).
    fast_free_sized(p, 64);
    // The block must come back from a class-1 allocation (its true bin),
    // not be lost in the class-4 bin.
    void* again = fast_malloc(8);
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again, p); // recycled through the correct bin
    fast_free(again);

    // Allocator stays fully serviceable afterwards.
    for (int i = 0; i < 256; ++i) {
        void* q = fast_malloc(48);
        ASSERT_NE(q, nullptr);
        fast_free_sized(q, 48); // matching hint: fast path
    }
    SUCCEED();
}

// v10: a block whose header was zeroed/scribbled (slab field 0, garbage
// alloc_size) must be DROPPED on the large-discrimination path, never fed
// into the large cache with its bogus size - a poisoned entry there can be
// handed out later as fresh memory.
TEST(ApiTest, FreeWithCorruptLargeHeaderIsDroppedNotCached) {
#if FASTALLOC_DEBUG_ENABLED
    GTEST_SKIP() << "debug build: the registry fatals on forged pointers by design";
#else
    // Forge a 16-byte header in writable memory: slab=0 (reads "large" on
    // the release discrimination path), alloc_size=0xDD (not a page-rounded
    // span). The user pointer follows the header.
    alignas(16) unsigned char forged[64] = {};
    forged[0] = forged[1] = forged[2] = forged[3] = 0;          // slab == nullptr
    std::memset(forged + 8, 0xDD, 8);                          // alloc_size = garbage
    void* user = forged + 16;
    fast_free(user); // must be dropped, not cached, not crashed

    // Same for fast_realloc: a bogus large header yields EINVAL/nullptr,
    // never a copy through a garbage size.
    errno = 0;
    void* r = fast_realloc(user, 128);
    EXPECT_EQ(r, nullptr);

    // The allocator stays serviceable; the large path still works.
    void* live = fast_malloc(64 * 1024);
    ASSERT_NE(live, nullptr);
    std::memset(live, 1, 64 * 1024);
    fast_free(live);
    void* small = fast_malloc(32);
    ASSERT_NE(small, nullptr);
    fast_free(small);
    SUCCEED();
#endif
}

// ---------------------------------------------------------------------------
// v11 guards: the closing datapoint of the Sep-2026 runner crash was a slot
// holding 0xd90b6085fe368400 - 16-byte aligned, >= 64 KB, NON-CANONICAL. It
// passed every v10 check (>= 64 KB only) and the inlined fast_free read its
// header at [ptr-16] in the non-canonical half of the address space: the CPU
// raises #GP and the kernel reports si_code=SI_KERNEL(128) with si_addr=0
// ("faulting address (nil)"). The v11 predicate completes the checks with
// the upper bound (< 2^47) and 16-alignment; all three free/resize entry
// points must drop such values and keep serving, in release AND debug.
// ---------------------------------------------------------------------------
TEST(ApiTest, FreeNonCanonicalHighPointerIsDroppedNotFatal) {
    // The exact runner-crash value class (16-aligned, >= 64 KB, >= 2^47).
    fast_free(reinterpret_cast<void*>(0xd90b6085fe368400ull));
    // The canonical boundary itself and beyond.
    fast_free(reinterpret_cast<void*>(0x0000800000000000ull));
    fast_free(reinterpret_cast<void*>(0xffffffffff600000ull));
    // Same class through the sized and realloc paths.
    fast_free_sized(reinterpret_cast<void*>(0xd90b6085fe368400ull), 8);
    errno = 0;
    void* r = fast_realloc(reinterpret_cast<void*>(0xd90b6085fe368400ull), 128);
    EXPECT_EQ(r, nullptr);
    EXPECT_EQ(errno, EINVAL);

    // Allocator stays fully serviceable afterwards.
    void* live = fast_malloc(64);
    ASSERT_NE(live, nullptr);
    std::memset(live, 0x5A, 64);
    fast_free(live);
    SUCCEED();
}

TEST(ApiTest, FreeMisalignedPointerIsDroppedNotFatal) {
    // Canonical and above 64 KB but NOT 16-byte aligned: every real FastAlloc
    // user pointer is 16-aligned by construction (fast_aligned_alloc clamps
    // alignment to >= 16), so a misaligned value is forged or shifted.
    fast_free(reinterpret_cast<void*>(0x00007f0000001008ull));
    fast_free(reinterpret_cast<void*>(0x0000555555555555ull + 8));
    fast_free_sized(reinterpret_cast<void*>(0x00007f0000001008ull), 16);
    errno = 0;
    void* r = fast_realloc(reinterpret_cast<void*>(0x00007f0000001008ull), 64);
    EXPECT_EQ(r, nullptr);
    EXPECT_EQ(errno, EINVAL);

    void* live = fast_malloc(48);
    ASSERT_NE(live, nullptr);
    fast_free_sized(live, 48); // matching-hint fast path still works
    SUCCEED();
}

// v11: every pointer the allocator hands out must satisfy the full shared
// plausibility predicate (>= 64 KB, < 2^47, 16-aligned) - small, large and
// aligned paths alike. This is the forward contract the free-side guards
// rely on: a legitimate return is never rejected later.
TEST(ApiTest, AllReturnsPassFullPlausibility) {
    const std::size_t sizes[] = {1, 8, 16, 24, 64, 100, 256, 1024, 4096,
                                 8192, 8193, 16 * 1024, 256 * 1024,
                                 1024 * 1024};
    for (std::size_t size : sizes) {
        void* p = fast_malloc(size);
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(IsPlausibleHeapPtr(p)) << "size=" << size;
        fast_free_sized(p, size);
    }
    for (std::size_t align : {16u, 32u, 64u, 4096u}) {
        void* p = fast_aligned_alloc(align, 128);
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(IsPlausibleHeapPtr(p)) << "align=" << align;
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % align, 0u);
        fast_free(p);
    }
    SUCCEED();
}

TEST(ApiTest, MallocZeroReturnsUniqueNonNull) {
    // Documented policy (glibc-compatible): unique non-null pointer.
    void* a = fast_malloc(0);
    void* b = fast_malloc(0);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b); // unique per allocation
    // The pointer must be usable as a 1-byte allocation.
    *static_cast<char*>(a) = 7;
    EXPECT_EQ(*static_cast<char*>(a), 7);
    fast_free(a);
    fast_free(b);
}

TEST(ApiTest, CallocZeroSizeReturnsNonNull) {
    void* a = fast_calloc(0, 16);
    void* b = fast_calloc(16, 0);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    fast_free(a);
    fast_free(b);
}

TEST(ApiTest, CallocZeroesMemory) {
    unsigned char* p = static_cast<unsigned char*>(fast_calloc(1024, 1));
    ASSERT_NE(p, nullptr);
    for (std::size_t i = 0; i < 1024; ++i) {
        EXPECT_EQ(p[i], 0u) << "byte " << i << " not zeroed";
        if (p[i] != 0) break;
    }
    fast_free(p);
}

TEST(ApiTest, CallocOverflowGuard) {
    // num * size would overflow SIZE_MAX -> must return nullptr.
    const std::size_t max = static_cast<std::size_t>(-1);
    EXPECT_EQ(fast_calloc(max / 2 + 1, 2), nullptr);
    EXPECT_EQ(fast_calloc(2, max / 2 + 1), nullptr);
    EXPECT_EQ(fast_calloc(max, max), nullptr);
}

TEST(ApiTest, ReallocNullActsAsMalloc) {
    void* p = fast_realloc(nullptr, 128);
    ASSERT_NE(p, nullptr);
    std::memset(p, 1, 128);
    fast_free(p);
}

TEST(ApiTest, ReallocZeroActsAsFree) {
    void* p = fast_malloc(128);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(fast_realloc(p, 0), nullptr); // C11 semantics: frees, returns null
    // p was freed: allocating the same class again must work.
    void* q = fast_malloc(128);
    ASSERT_NE(q, nullptr);
    fast_free(q);
}

TEST(ApiTest, Alignment16ForAllSizes) {
    const std::size_t sizes[] = {
        1, 2, 3, 5, 7, 8, 15, 16, 17, 24, 31, 32, 33, 48, 63, 64, 65,
        100, 127, 128, 129, 255, 256, 257, 511, 512, 1000, 1024, 1025,
        4095, 4096, 4097, 8175, 8176, 8177, 8192, 8193, 16384, 65536,
        1024 * 1024, 4 * 1024 * 1024 + 137
    };
    for (std::size_t s : sizes) {
        void* p = fast_malloc(s);
        ASSERT_NE(p, nullptr) << "size " << s;
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 16u, 0u)
            << "size " << s << " not 16-byte aligned";
        fast_free(p);
    }
}

TEST(ApiTest, EveryByteOfRequestedImageIsWritable) {
    // Touch every byte of the requested size for boundary sizes: off-by-one
    // in size-class math would corrupt the next block's metadata.
    for (std::size_t s = 1; s <= 512; ++s) {
        unsigned char* p = static_cast<unsigned char*>(fast_malloc(s));
        ASSERT_NE(p, nullptr) << "size " << s;
        for (std::size_t i = 0; i < s; ++i) p[i] = static_cast<unsigned char>(i & 0xFF);
        for (std::size_t i = 0; i < s; ++i) {
            ASSERT_EQ(p[i], static_cast<unsigned char>(i & 0xFF))
                << "size " << s << " byte " << i << " corrupted";
        }
        fast_free(p);
    }
}

TEST(ApiTest, UniquePointers) {
    std::vector<void*> ptrs;
    std::set<void*> unique;
    for (int i = 0; i < 1000; ++i) {
        void* p = fast_malloc(64);
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
        unique.insert(p);
    }
    EXPECT_EQ(ptrs.size(), unique.size()); // no duplicates handed out
    for (void* p : ptrs) fast_free(p);
}

TEST(ApiTest, FreeSizedMatchesFree) {
    // fast_free_sized must produce the same state as fast_free.
    void* a = fast_malloc(100);
    ASSERT_NE(a, nullptr);
    fast_free_sized(a, 100);

    void* b = fast_malloc(300);
    ASSERT_NE(b, nullptr);
    fast_free_sized(b, 300);

    void* c = fast_malloc(5000);
    ASSERT_NE(c, nullptr);
    fast_free_sized(c, 5000);

    // Allocator still healthy afterwards:
    void* d = fast_malloc(100);
    ASSERT_NE(d, nullptr);
    fast_free(d);
}

TEST(ApiTest, FreeSizedNullIsNoop) {
    fast_free_sized(nullptr, 64);
    SUCCEED();
}

TEST(ApiTest, FreeSizedWrongHintFallsBackSafely) {
    // A size hint that routes to "large" while the block is small must not
    // corrupt anything: the implementation falls back to the general path.
    void* p = fast_malloc(64);
    ASSERT_NE(p, nullptr);
    fast_free_sized(p, 64 * 1024); // deliberately wrong hint
    // Allocator healthy?
    void* q = fast_malloc(64);
    ASSERT_NE(q, nullptr);
    fast_free(q);
}

TEST(ApiTest, ReallocGrowsAndShrinksData) {
    unsigned char* p = static_cast<unsigned char*>(fast_malloc(64));
    ASSERT_NE(p, nullptr);
    for (int i = 0; i < 64; ++i) p[i] = static_cast<unsigned char>(i);

    p = static_cast<unsigned char*>(fast_realloc(p, 256));
    ASSERT_NE(p, nullptr);
    for (int i = 0; i < 64; ++i) EXPECT_EQ(p[i], static_cast<unsigned char>(i));
    for (int i = 64; i < 256; ++i) p[i] = 0xCD;

    p = static_cast<unsigned char*>(fast_realloc(p, 32)); // shrink
    ASSERT_NE(p, nullptr);
    for (int i = 0; i < 32; ++i) EXPECT_EQ(p[i], static_cast<unsigned char>(i));

    fast_free(p);
}

TEST(ApiTest, ReallocFailurePreservesOriginal) {
    // OOM on realloc: original block must stay valid and unfreed.
    void* p = fast_malloc(64);
    ASSERT_NE(p, nullptr);
    std::memset(p, 0x5A, 64);

    FastAllocTestSetOOMCountdown(1);
    void* q = fast_realloc(p, 64 * 1024 * 1024); // large growth needs a new span
    FastAllocTestSetOOMCountdown(0);
    EXPECT_EQ(q, nullptr);

    // Original intact?
    EXPECT_EQ(static_cast<unsigned char*>(p)[0], 0x5A);
    EXPECT_EQ(static_cast<unsigned char*>(p)[63], 0x5A);
    fast_free(p); // and still freeable
}

TEST(ApiTest, StatsBasics) {
    FastAllocStats before = fast_alloc_stats();

    void* p = fast_malloc(64);
    ASSERT_NE(p, nullptr);
    FastAllocStats mid = fast_alloc_stats();
    EXPECT_EQ(mid.small_allocs, before.small_allocs + 1);

    fast_free(p);
    FastAllocStats after = fast_alloc_stats();
    EXPECT_EQ(after.small_frees, before.small_frees + 1);
#if FASTALLOC_DEBUG_ENABLED
    EXPECT_EQ(after.current_live_blocks, before.current_live_blocks); // exact via registry
#endif
}

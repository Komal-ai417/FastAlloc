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
    // Drop thread and page caches so the large allocation must hit the OS.
    fast_alloc_purge_thread_cache();
    FastAllocTestPurgePageCache();

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

// ============================================================================
// Large-allocation path: cache reuse, in-place realloc growth/shrink,
// page-rounding semantics.
// (Audit H2: the large cache had ZERO tests; M2/O5: realloc always remapped.)
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"
#include "fast_alloc_config.h"
#include "test_hooks.h"

#include <cstring>
#include <vector>

using namespace FastAlloc;

TEST(LargeAllocTest, BasicLargeRoundTrip) {
    void* p = fast_malloc(1024 * 1024);
    ASSERT_NE(p, nullptr);
    char* c = static_cast<char*>(p);
    c[0] = 'X';
    c[1024 * 1024 - 1] = 'Y';
    EXPECT_EQ(c[0], 'X');
    EXPECT_EQ(c[1024 * 1024 - 1], 'Y');
    fast_free(p);
}

TEST(LargeAllocTest, CacheReusesExactSamePointer) {
    // Two identical >8KB alloc->free->alloc cycles must reuse the cached
    // page span: the second allocation returns the SAME pointer (LIFO cache).
    void* a = fast_malloc(64 * 1024 + 123);
    ASSERT_NE(a, nullptr);
    std::memset(a, 0x11, 64 * 1024 + 123);
    fast_free(a);

    void* b = fast_malloc(64 * 1024 + 123);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a, b) << "large allocation cache failed to reuse the span";
    fast_free(b);
}

TEST(LargeAllocTest, CacheServesMultipleSizes) {
    // Allocate several distinct large sizes, free all, allocate again:
    // every second pointer should be one of the first pointers (reuse).
    const std::size_t sizes[] = {16 * 1024, 32 * 1024, 128 * 1024, 1024 * 1024};
    const std::size_t n = sizeof(sizes) / sizeof(sizes[0]);
    std::vector<void*> first;
    for (std::size_t i = 0; i < n; ++i) {
        void* p = fast_malloc(sizes[i]);
        ASSERT_NE(p, nullptr);
        first.push_back(p);
    }
    for (void* p : first) fast_free(p);

    std::size_t reused = 0;
    for (std::size_t i = 0; i < n; ++i) {
        void* p = fast_malloc(sizes[i]);
        ASSERT_NE(p, nullptr);
        for (void* q : first) {
            if (q == p) { ++reused; break; }
        }
        fast_free(p);
    }
    // At least the majority should be direct cache hits.
    EXPECT_GE(reused, n);
}

TEST(LargeAllocTest, ReallocGrowWithinPageSlackKeepsPointer) {
    // 1MB usable + header rounds to whole pages; growing within the same
    // page count must NOT move the block.
    const std::size_t base = 256 * 1024; // multiple of 4096
    unsigned char* p = static_cast<unsigned char*>(fast_malloc(base));
    ASSERT_NE(p, nullptr);
    std::memset(p, 0x66, base);

    // +16 bytes still fits in the same span (page slack + header room).
    unsigned char* q = static_cast<unsigned char*>(fast_realloc(p, base + 16));
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q, p) << "realloc moved the block despite page slack";
    for (std::size_t i = 0; i < base; i += 64) EXPECT_EQ(q[i], 0x66);
    fast_free(q);
}

TEST(LargeAllocTest, ReallocGrowAcrossPagesPreservesData) {
    // Growth beyond the current span may move (copy path) or grow in place
    // via mremap; either way the data must survive.
    const std::size_t old_size = 128 * 1024;
    const std::size_t new_size = 512 * 1024;

    unsigned char* p = static_cast<unsigned char*>(fast_malloc(old_size));
    ASSERT_NE(p, nullptr);
    for (std::size_t i = 0; i < old_size; ++i) {
        p[i] = static_cast<unsigned char>((i * 7 + 3) & 0xFF);
    }

    unsigned char* q = static_cast<unsigned char*>(fast_realloc(p, new_size));
    ASSERT_NE(q, nullptr);
    for (std::size_t i = 0; i < old_size; ++i) {
        ASSERT_EQ(q[i], static_cast<unsigned char>((i * 7 + 3) & 0xFF))
            << "data lost at " << i;
    }
    // New region writable.
    std::memset(q + old_size, 0x99, new_size - old_size);
    fast_free(q);
}

TEST(LargeAllocTest, ReallocShrinkReleasesPagesInPlace) {
    // Shrinking by >= 1 page now happens IN PLACE: same pointer, tail pages
    // returned to the OS (audit M2/O5: previously always copy+remap).
    const std::size_t big = 512 * 1024;
    unsigned char* p = static_cast<unsigned char*>(fast_malloc(big));
    ASSERT_NE(p, nullptr);
    for (std::size_t i = 0; i < big; i += 3) p[i] = 0x42;

    unsigned char* q = static_cast<unsigned char*>(fast_realloc(p, 64 * 1024));
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q, p) << "large shrink should keep the front pages in place";
    for (std::size_t i = 0; i < 64 * 1024; i += 3) {
        ASSERT_EQ(q[i], 0x42);
    }
    fast_free(q);
}

TEST(LargeAllocTest, PageCachePurgeReleasesMemory) {
    // Purge must empty the global page-span cache.
    void* p = fast_malloc(64 * 1024);
    ASSERT_NE(p, nullptr);
    fast_free(p);
    // p's span is now parked in the TLS large cache; flush it to global and
    // into the page cache.
    fast_alloc_purge_thread_cache();

    std::size_t released = FastAllocTestPurgePageCache();
    EXPECT_GT(released, 0u); // at least our 64KB span went back to the OS
    EXPECT_EQ(FastAllocTestPageCacheBytes(), 0u);

    // Allocator still fully functional after purge.
    void* q = fast_malloc(64 * 1024);
    ASSERT_NE(q, nullptr);
    std::memset(q, 1, 64 * 1024);
    fast_free(q);
}

TEST(LargeAllocTest, HugeAllocation) {
    // 64MB allocation: touch first and last byte to prove full mapping.
    const std::size_t sz = 64ull * 1024 * 1024;
    unsigned char* p = static_cast<unsigned char*>(fast_malloc(sz));
    if (!p) GTEST_SKIP() << "64MB allocation unavailable in this environment";
    p[0] = 1;
    p[sz / 2] = 2;
    p[sz - 1] = 3;
    EXPECT_EQ(p[0], 1);
    EXPECT_EQ(p[sz / 2], 2);
    EXPECT_EQ(p[sz - 1], 3);
    fast_free(p);
}

TEST(LargeAllocTest, OverflowGuardLarge) {
    // size + header would wrap around: must return nullptr, not crash.
    const std::size_t max = static_cast<std::size_t>(-1);
    EXPECT_EQ(fast_malloc(max), nullptr);
    EXPECT_EQ(fast_malloc(max - 8), nullptr);
}

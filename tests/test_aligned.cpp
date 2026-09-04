// ============================================================================
// Over-aligned allocation tests (audit M7: no aligned API existed).
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"
#include "fast_alloc_config.h"

#include <cstring>
#include <cstdint>

using namespace FastAlloc;

TEST(AlignedTest, NaturalAlignmentDelegates) {
    void* p = fast_aligned_alloc(16, 100);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 16u, 0u);
    std::memset(p, 1, 100);
    fast_free(p);
}

TEST(AlignedTest, CommonAlignments) {
    const std::size_t alignments[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
    for (std::size_t a : alignments) {
        SCOPED_TRACE(testing::Message() << "alignment=" << a);
        void* p = fast_aligned_alloc(a, 1000);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % a, 0u);
        // Full requested image writable.
        std::memset(p, 0x42, 1000);
        fast_free(p);
    }
}

TEST(AlignedTest, AlignmentWithOddSizes) {
    for (std::size_t a : {32u, 64u, 256u}) {
        for (std::size_t s : {1u, 2u, 17u, 63u, 255u, 4095u, 8191u}) {
            SCOPED_TRACE(testing::Message() << "align=" << a << " size=" << s);
            void* p = fast_aligned_alloc(a, s);
            ASSERT_NE(p, nullptr);
            EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % a, 0u);
            std::memset(p, 0x77, s);
            fast_free(p);
        }
    }
}

TEST(AlignedTest, LargeAlignedAllocation) {
    void* p = fast_aligned_alloc(4096, 256 * 1024);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 4096u, 0u);
    std::memset(p, 0x99, 256 * 1024);
    fast_free(p);
}

TEST(AlignedTest, ReallocPreservesAlignment) {
    void* p = fast_aligned_alloc(256, 500);
    ASSERT_NE(p, nullptr);
    std::memset(p, 0x33, 500);

    p = fast_realloc(p, 2000);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 256u, 0u)
        << "realloc lost the over-alignment";
    for (int i = 0; i < 500; ++i) {
        ASSERT_EQ(static_cast<unsigned char*>(p)[i], 0x33);
    }
    fast_free(p);
}

TEST(AlignedTest, ReallocShrinkKeepsPointer) {
    void* p = fast_aligned_alloc(128, 4096);
    ASSERT_NE(p, nullptr);
    std::memset(p, 0x44, 4096);

    p = fast_realloc(p, 512);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 128u, 0u);
    fast_free(p);
}

TEST(AlignedTest, InvalidAlignmentsRejected) {
    EXPECT_EQ(fast_aligned_alloc(0, 100), nullptr);    // zero
    EXPECT_EQ(fast_aligned_alloc(3, 100), nullptr);    // not power of two
    EXPECT_EQ(fast_aligned_alloc(48, 100), nullptr);   // not power of two
    EXPECT_EQ(fast_aligned_alloc(8192, 100), nullptr); // beyond supported max
    EXPECT_EQ(fast_aligned_alloc(1 << 20, 100), nullptr);
}

TEST(AlignedTest, ZeroSizeAligned) {
    void* p = fast_aligned_alloc(64, 0);
    ASSERT_NE(p, nullptr); // unique pointer policy
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 64u, 0u);
    fast_free(p);
}

TEST(AlignedTest, ManyAlignedBlocks) {
    std::vector<void*> ptrs;
    for (int i = 0; i < 500; ++i) {
        std::size_t a = 32u << (i % 6); // 32..1024
        void* p = fast_aligned_alloc(a, 128);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % a, 0u);
        std::memset(p, 0x66, 128);
        ptrs.push_back(p);
    }
    for (void* p : ptrs) fast_free(p);
}

TEST(AlignedTest, MixedAlignedAndRegular) {
    void* a = fast_aligned_alloc(256, 100);
    void* b = fast_malloc(100);
    void* c = fast_aligned_alloc(64, 100);
    void* d = fast_malloc(100);
    ASSERT_NE(a, nullptr); ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr); ASSERT_NE(d, nullptr);

    std::memset(a, 1, 100); std::memset(b, 2, 100);
    std::memset(c, 3, 100); std::memset(d, 4, 100);

    fast_free(a); fast_free(b); fast_free(c); fast_free(d);

    // Allocator healthy.
    void* e = fast_malloc(100);
    ASSERT_NE(e, nullptr);
    fast_free(e);
}

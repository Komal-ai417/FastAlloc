// ============================================================================
// Size-class boundary tests and slab-path data integrity.
// (Audit gap: class-boundary behaviour was never verified numerically.)
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"
#include "fast_alloc_config.h"

#include <cstring>
#include <cstdint>
#include <vector>

using namespace FastAlloc;

namespace {
// Exercise a request size: allocate, fill with a rolling pattern across the
// FULL requested image, verify, free, re-allocate, verify the block comes
// back reusable.
void ExerciseSize(std::size_t s) {
    SCOPED_TRACE(testing::Message() << "size=" << s);
    unsigned char* p = static_cast<unsigned char*>(fast_malloc(s));
    ASSERT_NE(p, nullptr);
    for (std::size_t i = 0; i < s; ++i) {
        p[i] = static_cast<unsigned char>((i * 31 + s) & 0xFF);
    }
    for (std::size_t i = 0; i < s; ++i) {
        ASSERT_EQ(p[i], static_cast<unsigned char>((i * 31 + s) & 0xFF));
    }
    fast_free(p);

    // Same size again: the block should be immediately reusable.
    unsigned char* q = static_cast<unsigned char*>(fast_malloc(s));
    ASSERT_NE(q, nullptr);
    std::memset(q, 0xEE, s);
    fast_free(q);
}
} // namespace

TEST(SizeClassTest, AllClassBoundaries) {
    // Every 16-byte boundary from 1..512 plus the high end.
    for (std::size_t s = 1; s <= 512; ++s) ExerciseSize(s);
}

TEST(SizeClassTest, HighClassBoundaries) {
    const std::size_t sizes[] = {
        511, 512, 513, 1023, 1024, 1025, 2047, 2048, 2049,
        4095, 4096, 4097, 8160, 8175, 8176, 8177, 8192
    };
    for (std::size_t s : sizes) ExerciseSize(s);
}

TEST(SizeClassTest, ThresholdIsExactly8176) {
    // MAX_SMALL_REQUEST = 8176 goes through the slab path; 8177 takes the
    // large path. Both must work and both must be 16-byte aligned.
    void* small = fast_malloc(8176);
    ASSERT_NE(small, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(small) % 16u, 0u);
    std::memset(small, 1, 8176);

    void* large = fast_malloc(8177);
    ASSERT_NE(large, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(large) % 16u, 0u);
    std::memset(large, 2, 8177);

    fast_free(small);
    fast_free(large);
}

TEST(SizeClassTest, ManyBlocksSameClass) {
    // Force multiple slab creations in one class: 5000 blocks of 64B.
    std::vector<void*> ptrs;
    ptrs.reserve(5000);
    for (int i = 0; i < 5000; ++i) {
        void* p = fast_malloc(64);
        ASSERT_NE(p, nullptr);
        *static_cast<uint64_t*>(p) = i;
        ptrs.push_back(p);
    }
    for (int i = 0; i < 5000; ++i) {
        EXPECT_EQ(*static_cast<uint64_t*>(ptrs[i]), static_cast<uint64_t>(i));
    }
    for (void* p : ptrs) fast_free(p);
}

TEST(SizeClassTest, MixedSizesInterleaved) {
    // Interleave sizes across classes to catch cross-class routing bugs.
    std::vector<void*> ptrs;
    std::vector<std::size_t> sizes;
    for (int i = 0; i < 2000; ++i) {
        std::size_t s = 1 + (static_cast<std::size_t>(i * 37) % 8176);
        void* p = fast_malloc(s);
        ASSERT_NE(p, nullptr);
        std::memset(p, 0x77, s);
        ptrs.push_back(p);
        sizes.push_back(s);
    }
    for (std::size_t i = 0; i < ptrs.size(); ++i) {
        const unsigned char* p = static_cast<const unsigned char*>(ptrs[i]);
        for (std::size_t j = 0; j < sizes[i]; j += 97) {
            ASSERT_EQ(p[j], 0x77);
        }
        fast_free(ptrs[i]);
    }
}

TEST(SizeClassTest, ReallocAcrossClasses) {
    // Grow and shrink across multiple class boundaries; data must survive.
    unsigned char* p = static_cast<unsigned char*>(fast_malloc(16));
    ASSERT_NE(p, nullptr);
    for (int i = 0; i < 16; ++i) p[i] = static_cast<unsigned char>(i);

    std::size_t cur = 16;
    for (std::size_t next : {17u, 100u, 600u, 4096u, 8176u, 300u, 40u, 17u}) {
        p = static_cast<unsigned char*>(fast_realloc(p, next));
        ASSERT_NE(p, nullptr) << "realloc to " << next;
        // Original 16 bytes must survive every step.
        for (int i = 0; i < 16; ++i) {
            ASSERT_EQ(p[i], static_cast<unsigned char>(i)) << "after realloc to " << next;
        }
        for (std::size_t i = cur; i < next && i < 8176u; ++i) {
            p[i] = 0x33; // newly exposed region writable
        }
        cur = next;
    }
    fast_free(p);
}

TEST(SizeClassTest, ReallocSmallToLargeAndBack) {
    unsigned char* p = static_cast<unsigned char*>(fast_malloc(100));
    ASSERT_NE(p, nullptr);
    for (int i = 0; i < 100; ++i) p[i] = static_cast<unsigned char>(0x40 + i);

    p = static_cast<unsigned char*>(fast_realloc(p, 100 * 1024)); // small -> large
    ASSERT_NE(p, nullptr);
    for (int i = 0; i < 100; ++i) EXPECT_EQ(p[i], static_cast<unsigned char>(0x40 + i));
    p[100 * 1024 - 1] = 0x55;

    p = static_cast<unsigned char*>(fast_realloc(p, 200)); // large -> small
    ASSERT_NE(p, nullptr);
    for (int i = 0; i < 100; ++i) EXPECT_EQ(p[i], static_cast<unsigned char>(0x40 + i));

    fast_free(p);
}

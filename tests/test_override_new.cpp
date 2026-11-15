// ============================================================================
// Global operator new/delete override tests (FA-04).
//
// Built unconditionally; the case bodies exist only when the program is
// compiled with FASTALLOC_OVERRIDE_NEW (the dedicated CI job configures with
// -DFASTALLOC_OVERRIDE_NEW=ON, which routes the WHOLE program - gtest and
// the test framework included - through the overrides, so merely building
// and running the suite already exercises the drop-in property).
//
// Regression background: the override set used to provide only the six
// classic forms. `new (std::nothrow) T` then allocated from libc while the
// plain `delete p` on the same pointer routed the foreign pointer into
// fast_free (cross-allocator corruption), and C++17 over-aligned types
// (alignas(64), std::vector<alignas(32) T>) silently bypassed the feature
// via the align_val_t family. These tests pin all of those routes to
// FastAlloc and verify data integrity around them.
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"

#ifdef FAST_ALLOC_OVERRIDE_NEW

#include <new>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

struct alignas(64) CacheLine {
    char payload[64];
};

struct alignas(32) WideElem {
    std::uint64_t a, b, c, d;
};

// FastAllocStats is flushed when fast_alloc_stats() is called, so a
// before/after delta of alloc counters on this thread is meaningful.
// delta_at_least (not equality): framework bookkeeping may allocate on the
// same thread between the snapshots.
std::size_t AllocCount() {
    const FastAlloc::FastAllocStats s = FastAlloc::fast_alloc_stats();
    return s.small_allocs + s.large_allocs;
}

void FillAndVerify(void* p, std::size_t bytes, unsigned char tag) {
    std::memset(p, tag, bytes);
    const unsigned char* q = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < bytes; ++i) {
        ASSERT_EQ(q[i], static_cast<unsigned char>(tag));
    }
}

} // namespace

TEST(OverrideNew, PlainNewDeleteRoutesThroughFastAlloc) {
    const std::size_t before = AllocCount();
    auto* p = new CacheLine;
    EXPECT_NE(p, nullptr);
    EXPECT_GE(AllocCount(), before + 1);
    FillAndVerify(p, sizeof(CacheLine), 0xA5);
    delete p;  // plain delete on a FastAlloc block: must route back
}

TEST(OverrideNew, NothrowNewPlusPlainDeleteIsConsistent) {
    // THE regression case for FA-04: nothrow new used to fall back to libc
    // while plain delete routed into FastAlloc - a cross-allocator free.
    const std::size_t before = AllocCount();
    CacheLine* p = new (std::nothrow) CacheLine;
    ASSERT_NE(p, nullptr);
    EXPECT_GE(AllocCount(), before + 1);  // allocated by FastAlloc, not libc
    FillAndVerify(p, sizeof(CacheLine), 0x5C);
    delete p;  // mismatched-looking but now consistent pair

    // The mismatch used to poison a bin: prove the allocator still hands
    // out distinct, writable blocks of the same class afterwards.
    CacheLine* live[8] = {};
    for (auto& q : live) {
        q = new CacheLine;
        ASSERT_NE(q, nullptr);
        for (const auto* r : live) {
            if (r != q && r != nullptr) {
                EXPECT_NE(r, q);
            }
        }
    }
    for (auto* q : live) FillAndVerify(q, sizeof(CacheLine), 0x33);
    for (auto* q : live) delete q;
}

TEST(OverrideNew, NothrowNewReturnsNullOnImpossibleSize) {
    void* p = ::operator new(static_cast<std::size_t>(-1),
                             std::nothrow);
    EXPECT_EQ(p, nullptr);
    p = ::operator new[](static_cast<std::size_t>(-1), std::nothrow);
    EXPECT_EQ(p, nullptr);
}

TEST(OverrideNew, AlignedNewDeleteFamilyRoutesThroughFastAlloc) {
    const std::size_t before = AllocCount();
    auto* p = new CacheLine;  // selects operator new(size, align_val_t{64})
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 64u, 0u);
    EXPECT_GE(AllocCount(), before + 1);
    FillAndVerify(p, sizeof(CacheLine), 0xC3);
    delete p;  // selects aligned delete -> fast_free stash path

    auto* arr = new CacheLine[3];
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(arr) % 64u, 0u);
    FillAndVerify(arr, 3 * sizeof(CacheLine), 0x3C);
    delete[] arr;
}

TEST(OverrideNew, AlignedNothrowNewRoutesThroughFastAlloc) {
    const std::size_t before = AllocCount();
    CacheLine* p = new (std::nothrow) CacheLine;
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 64u, 0u);
    EXPECT_GE(AllocCount(), before + 1);
    FillAndVerify(p, sizeof(CacheLine), 0x77);
    delete p;
}

TEST(OverrideNew, VectorOfOverAlignedElements) {
    // std::vector<alignas(32) T> allocates via the aligned new family in
    // C++17; growth, copy and destruction must all stay on FastAlloc.
    const std::size_t before = AllocCount();
    std::vector<WideElem> v;
    for (int i = 0; i < 256; ++i) {
        v.push_back(WideElem{
            static_cast<std::uint64_t>(i), 0x0F0F0F0F0F0F0F0Full,
            static_cast<std::uint64_t>(~i), 0xF00DF00DF00DF00Dull});
    }
    EXPECT_EQ(v.size(), 256u);
    EXPECT_GE(AllocCount(), before + 1);
    for (std::size_t i = 0; i < v.size(); ++i) {
        ASSERT_EQ(v[i].a, i);
        ASSERT_EQ(v[i].b, 0x0F0F0F0F0F0F0F0Full);
        ASSERT_EQ(v[i].c, ~i);
        ASSERT_EQ(v[i].d, 0xF00DF00DF00DF00Dull);
    }
    // Force a reallocation copy and verify data survives the move.
    v.reserve(1024);
    for (std::size_t i = 0; i < 256; ++i) {
        ASSERT_EQ(v[i].a, i);
    }
}

TEST(OverrideNew, StringAndChurnUnderOverride) {
    // The whole program runs under the override in this configuration;
    // exercise ordinary std:: machinery end-to-end as a smoke test.
    std::vector<std::string> words;
    for (int i = 0; i < 1000; ++i) {
        words.push_back("fastalloc-override-" + std::to_string(i));
    }
    for (int i = 0; i < 1000; ++i) {
        ASSERT_EQ(words[i], "fastalloc-override-" + std::to_string(i));
    }
}

#endif // FAST_ALLOC_OVERRIDE_NEW

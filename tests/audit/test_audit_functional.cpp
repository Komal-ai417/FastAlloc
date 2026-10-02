// ============================================================================
// FastAlloc audit — functional correctness matrix (audit item 1).
// Every size-class boundary N-1/N/N+1, powers of 2 +/-1, tiny..huge,
// alignment matrix, realloc semantics, zero-size, usable-image writes.
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"
#include "fast_alloc_config.h"
#include "../test_hooks.h"

#include <cstring>
#include <cstdint>
#include <vector>
#include <set>
#include <cmath>

using namespace FastAlloc;

namespace {
unsigned char PB(std::size_t seed, std::size_t i) {
    return static_cast<unsigned char>((seed * 2654435761u + i * 40503u) >> 8);
}
void Fill(void* p, std::size_t n, std::size_t seed) {
    auto* c = static_cast<unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i) c[i] = PB(seed, i);
}
bool Verify(const void* p, std::size_t n, std::size_t seed) {
    auto* c = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i) if (c[i] != PB(seed, i)) return false;
    return true;
}
} // namespace

// ---- 1.1 every size-class boundary: N-1, N, N+1 for all classes -------------
TEST(AuditFunctional, AllClassBoundariesNMinus1NPlus1) {
    // Class c serves block (c+1)*16; request r maps to class (r+16-1)>>4.
    // Boundaries that matter: r where the class CHANGES: r = 16k+1 (new class
    // for release sizing: RequestToClass(r) = (r+16-1)>>4).
    for (std::size_t c = 1; c < NUM_SIZE_CLASSES; ++c) {
        std::size_t block = ClassIndexToSize(c);          // (c+1)*16
        for (std::size_t r : {block - 17, block - 16, block - 15}) { // N-1,N,N+1 in request terms
            if (r == 0 || r > 8176) continue;
            void* p = fast_malloc(r);
            ASSERT_NE(p, nullptr) << "r=" << r;
            EXPECT_EQ(0u, reinterpret_cast<std::uintptr_t>(p) & 15u) << "16-alignment r=" << r;
            Fill(p, r, r);
            fast_free(p);
        }
    }
}

// ---- 1.2 exhaustive every request size 1..8176 (the full small domain) ------
TEST(AuditFunctional, EverySmallSizeExhaustive) {
    std::size_t seed = 7;
    for (std::size_t r = 1; r <= 8176; ++r) {
        void* p = fast_malloc(r);
        ASSERT_NE(p, nullptr) << "r=" << r;
        Fill(p, r, seed + r);
        EXPECT_TRUE(Verify(p, r, seed + r));
        fast_free(p);
    }
}

// ---- 1.3 powers of two +/- 1, small and large -------------------------------
TEST(AuditFunctional, PowersOfTwoAndNeighbours) {
    for (std::size_t e = 0; e <= 21; ++e) {
        std::size_t base = 1ull << e;
        for (std::size_t r : {base - 1, base, base + 1}) {
            if (r == 0) continue;
            void* p = fast_malloc(r);
            ASSERT_NE(p, nullptr) << "r=" << r;
            Fill(p, r, 0xE0 + e);
            fast_free(p);
        }
    }
}

// ---- 1.4 tiny/medium/large/huge ---------------------------------------------
TEST(AuditFunctional, SizeRegimes) {
    const std::size_t sizes[] = {1, 2, 3, 15, 16, 17, 31, 32, 100, 1000, 4095, 4096, 8176,
                                 8177, 16384, 65536, 1 << 20, (1 << 20) + 1, 4 << 20, 16 << 20};
    for (std::size_t r : sizes) {
        void* p = fast_malloc(r);
        ASSERT_NE(p, nullptr) << "r=" << r;
        // Touch first and last byte (mapping + bounds). Sequential: for r==1
        // the two stores hit the same byte, so verify after each store.
        auto* c = static_cast<unsigned char*>(p);
        c[0] = 0x11; EXPECT_EQ(c[0], 0x11);
        c[r - 1] = 0x22; EXPECT_EQ(c[r - 1], 0x22);
        fast_free(p);
    }
}

// ---- 1.5 repeated allocate/free (same pointer reuse) ------------------------
TEST(AuditFunctional, RepeatedAllocFreeSamePointer) {
    void* first = nullptr;
    for (int i = 0; i < 10000; ++i) {
        void* p = fast_malloc(128);
        ASSERT_NE(p, nullptr);
        if (i == 0) { first = p; }
        else if (i < 64) { EXPECT_EQ(p, first) << "LIFO recycling expected, i=" << i; }
        Fill(p, 128, i);
        fast_free(p);
    }
}

// ---- 1.6 allocate without free (leak-only, no crash; stats live grows) -------
TEST(AuditFunctional, AllocateWithoutFree) {
    FastAllocStats before = fast_alloc_stats();
    std::vector<void*> keep;
    for (int i = 0; i < 5000; ++i) keep.push_back(fast_malloc(64 + (i % 512)));
    FastAllocStats after = fast_alloc_stats();
    EXPECT_GT(after.current_live_blocks, before.current_live_blocks);
    for (void* p : keep) fast_free(p);
}

// ---- 1.7 free in arbitrary orders (fixed-seed permutations) -----------------
TEST(AuditFunctional, FreeArbitraryOrders) {
    for (unsigned seed : {1u, 2u, 3u}) {
        std::vector<void*> ptrs;
        for (int i = 0; i < 2048; ++i) ptrs.push_back(fast_malloc(96));
        // Fisher-Yates with seed
        std::size_t s = seed;
        for (std::size_t i = ptrs.size(); i-- > 1;) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            std::size_t j = (s >> 33) % (i + 1);
            std::swap(ptrs[i], ptrs[j]);
        }
        for (void* p : ptrs) fast_free(p);
    }
}

// ---- 1.8 alignment matrix: default + every supported power of two -----------
TEST(AuditFunctional, AlignmentMatrixAllPowers) {
    for (std::size_t a = 2; a <= 4096; a *= 2) {
        for (std::size_t r : {1, 17, 100, 8176, 8192, 1 << 20}) {
            void* p = fast_aligned_alloc(a, r);
            ASSERT_NE(p, nullptr) << "a=" << a << " r=" << r;
            EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) & (a - 1), 0u) << "a=" << a;
            Fill(p, r, a + r);
            fast_free(p);
        }
    }
    // default alignment: every returned pointer 16-aligned
    for (std::size_t r = 1; r <= 2048; r = r * 3 + 7) {
        void* p = fast_malloc(r);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(0u, reinterpret_cast<std::uintptr_t>(p) & 15u);
        fast_free(p);
        void* q = fast_malloc(r + 8176);
        ASSERT_NE(q, nullptr);
        EXPECT_EQ(0u, reinterpret_cast<std::uintptr_t>(q) & 15u);
        fast_free(q);
    }
}

// ---- 1.9 invalid alignments rejected ----------------------------------------
TEST(AuditFunctional, InvalidAlignmentRejected) {
    for (std::size_t a : {0u, 3u, 5u, 12u, 24u, 100u, 4095u, 8192u, 1u << 20}) {
        EXPECT_EQ(fast_aligned_alloc(a, 64), nullptr) << "a=" << a;
    }
}

// ---- 1.10 alignment x size-class boundary combinations ----------------------
TEST(AuditFunctional, AlignmentTimesClassBoundaries) {
    for (std::size_t a : {16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u}) {
        for (std::size_t c : {1u, 2u, 15u, 16u, 31u, 32u, 255u, 256u, 511u}) {
            std::size_t block = ClassIndexToSize(c);
            for (std::size_t r : {block - 15, block}) {  // near boundary
                void* p = fast_aligned_alloc(a, r);
                ASSERT_NE(p, nullptr);
                EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) & (a - 1), 0u);
                Fill(p, r, a * 100 + c);
                fast_free(p);
            }
        }
    }
}

// ---- 1.11 realloc full semantics --------------------------------------------
TEST(AuditFunctional, ReallocSemanticsMatrix) {
    // realloc(NULL, n) == malloc(n)
    void* p = fast_realloc(nullptr, 100);
    ASSERT_NE(p, nullptr); Fill(p, 100, 1); 
    // realloc(p, 0) == free(p), returns null
    EXPECT_EQ(fast_realloc(p, 0), nullptr);
    // grow in place / grow with move / shrink / repeated
    p = fast_malloc(64); Fill(p, 64, 2);
    p = fast_realloc(p, 128); ASSERT_NE(p, nullptr);
    EXPECT_TRUE(Verify(p, 64, 2)) << "grow must preserve prefix";
    p = fast_realloc(p, 32); ASSERT_NE(p, nullptr);
    EXPECT_TRUE(Verify(p, 32, 2)) << "shrink must preserve prefix";
    for (int i = 0; i < 100; ++i) {  // repeated grow/shrink
        std::size_t ns = 16 + (i * 37 % 8000);
        p = fast_realloc(p, ns);
        ASSERT_NE(p, nullptr);
        p = fast_realloc(p, ns / 2 + 1);
        ASSERT_NE(p, nullptr);
    }
    fast_free(p);
    // realloc across size classes: small->large->small
    p = fast_malloc(100); Fill(p, 100, 3);
    p = fast_realloc(p, 1 << 20); ASSERT_NE(p, nullptr);
    EXPECT_TRUE(Verify(p, 100, 3)) << "small->large must preserve";
    p = fast_realloc(p, 50); ASSERT_NE(p, nullptr);
    EXPECT_TRUE(Verify(p, 50, 3)) << "large->small must preserve";
    fast_free(p);
    // realloc preserving old contents to the min(old,new) on EVERY path
    for (std::size_t old : {std::size_t{64}, std::size_t{1024}, std::size_t{8176}, std::size_t{1} << 20}) {
        for (std::size_t ns : {std::size_t{32}, old / 2, old, old * 2, old + 4096}) {
            if (ns == 0) continue;
            void* b = fast_malloc(old); ASSERT_NE(b, nullptr);
            Fill(b, old, 0x5EED);
            void* np = fast_realloc(b, ns);
            ASSERT_NE(np, nullptr);
            std::size_t chk = old < ns ? old : ns;
            EXPECT_TRUE(Verify(np, chk, 0x5EED)) << "old=" << old << " new=" << ns;
            fast_free(np);
        }
    }
}

// ---- 1.12 zero-size semantics ------------------------------------------------
TEST(AuditFunctional, ZeroSizeUniquePointers) {
    // Distinctness is guaranteed among LIVE allocations; a freed pointer may
    // legitimately be recycled (glibc does the same). Hold 1000 live blocks.
    std::set<void*> uniq;
    std::vector<void*> hold;
    hold.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        void* p = fast_malloc(0);
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(uniq.insert(p).second) << "live malloc(0) must be unique";
        hold.push_back(p);
    }
    for (void* p : hold) fast_free(p);
    // calloc zero forms also give distinct live pointers
    void* q = fast_calloc(0, 8); ASSERT_NE(q, nullptr);
    void* r = fast_calloc(8, 0); ASSERT_NE(r, nullptr);
    EXPECT_NE(q, r);
    fast_free(q); fast_free(r);
}

// ---- 1.13 calloc zeroing + overflow ------------------------------------------
TEST(AuditFunctional, CallocZeroAndOverflow) {
    void* p = fast_calloc(1000, 8);
    ASSERT_NE(p, nullptr);
    auto* c = static_cast<unsigned char*>(p);
    for (std::size_t i = 0; i < 8000; ++i) EXPECT_EQ(c[i], 0);
    fast_free(p);
    EXPECT_EQ(fast_calloc(SIZE_MAX, 2), nullptr);
    EXPECT_EQ(fast_calloc(2, SIZE_MAX), nullptr);
    EXPECT_EQ(fast_calloc(SIZE_MAX / 2, 3), nullptr);
}

// ---- 1.14 huge sizes / SIZE_MAX neighbourhood ---------------------------------
// Two distinct regimes, asserted differently ON PURPOSE:
//
//  (a) Arithmetic-overflow neighbourhood: the header+size computation wraps
//      and the overflow guards reject the request BEFORE any OS call, on
//      every platform, regardless of kernel overcommit policy.
//
//  (b) Huge-but-mappable sizes: whether the OS accepts a multi-TiB
//      *reservation* is kernel policy (vm.overcommit_memory, VA width,
//      RAM+swap) -- not an allocator property. glibc malloc(1<<40) also
//      succeeds on overcommit_memory=1 systems, so demanding nullptr here
//      would be asserting a kernel policy, not an allocator invariant.
//      Instead assert the environment-independent invariants: every return
//      is either nullptr or a plausible 16-aligned block; realloc ownership
//      is correct EITHER way (a successful move consumes the original --
//      freeing both would be a double free); and the allocator stays fully
//      serviceable afterwards.
//      (Found in the field 2026-10-01: on an overcommit=1 host this test
//      demanded nullptr for a 1 TiB reservation that the kernel happily
//      granted, then double-freed the realloc'd block when the assumption
//      broke -- the TLS double-push guard absorbed it, which is FastAlloc
//      working as designed; the test's assumption was the defect.)
TEST(AuditFunctional, HugeAndOverflowSizes) {
    // (a) overflow guards: must be rejected unconditionally
    for (std::size_t s : {SIZE_MAX, SIZE_MAX - 1, SIZE_MAX - 16}) {
        EXPECT_EQ(fast_malloc(s), nullptr) << "s=" << s;
        EXPECT_EQ(fast_realloc(nullptr, s), nullptr);
        void* p = fast_malloc(64);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(fast_realloc(p, s), nullptr);  // original preserved
        fast_free(p);
    }
    // (b) huge-but-mappable: outcome is OS policy; assert the invariants
    for (std::size_t s : {std::size_t{1} << 40, (std::size_t{1} << 47) + 4096,
                          std::size_t{1} << 48, SIZE_MAX / 2}) {
        SCOPED_TRACE(testing::Message() << "s=" << s);
        void* p = fast_malloc(s);
        if (p != nullptr) {
            EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 16, 0);
            fast_free(p);
        }
        void* r = fast_realloc(nullptr, s);  // malloc-equivalent form
        if (r != nullptr) {
            EXPECT_EQ(reinterpret_cast<std::uintptr_t>(r) % 16, 0);
            fast_free(r);
        }
        void* q = fast_malloc(64);
        ASSERT_NE(q, nullptr);
        void* moved = fast_realloc(q, s);
        if (moved != nullptr) {
            // Successful move: q was already consumed by realloc. Free the
            // NEW block -- freeing q here would be a double free.
            EXPECT_EQ(reinterpret_cast<std::uintptr_t>(moved) % 16, 0);
            fast_free(moved);
        } else {
            fast_free(q);  // failed realloc preserved the original
        }
        // Allocator fully serviceable afterwards
        void* v = fast_malloc(256);
        ASSERT_NE(v, nullptr);
        Fill(v, 256, 77);
        EXPECT_TRUE(Verify(v, 256, 77));
        fast_free(v);
    }
}

// ---- 1.15 full usable image writable for every class -------------------------
TEST(AuditFunctional, FullUsableImageWritable) {
    for (std::size_t c = 1; c < NUM_SIZE_CLASSES; ++c) {
        std::size_t block = ClassIndexToSize(c);
        std::size_t req_hi = block - 16;             // max release-servable request
        for (std::size_t r : {req_hi, req_hi - 1}) {
            if (r == 0) continue;
            void* p = fast_malloc(r);
            ASSERT_NE(p, nullptr);
            Fill(p, r, c);
            EXPECT_TRUE(Verify(p, r, c));
            fast_free(p);
        }
    }
}

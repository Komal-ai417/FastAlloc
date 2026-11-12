// ============================================================================
// Statistics balance across realloc grow/shrink (current_live_bytes /
// current_live_blocks must net to zero over a balanced alloc/free battery).
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"
#include "test_hooks.h"

#include <cstring>
#include <vector>

using namespace FastAlloc;

// Ensure the in-place large-realloc paths (mremap grow / mremap or decommit shrink)
// adjust large alloc/free byte counters so current_live_bytes remains balanced.
// A grow-then-shrink-then-free cycle must net live bytes and blocks back to zero.
TEST(StatsBalanceTest, ReallocGrowShrinkFreeNetsToZero) {
    // Warm the thread cache first so refill batches don't add noise.
    {
        std::vector<void*> warm;
        warm.reserve(64);
        for (int i = 0; i < 64; ++i) warm.push_back(fast_malloc(1000));
        for (void* p : warm) fast_free(p);
    }

    FastAllocStats before = fast_alloc_stats();

    // Battery 1: large grow + shrink + free.
    // Sizes chosen so both transitions change the page count: 3,000,000 ->
    // grow 6,000,005 -> shrink 1,500,001 -> free, verifying live-bytes balance.
    {
        void* p = fast_malloc(3000000);
        ASSERT_NE(p, nullptr);
        p = fast_realloc(p, 6000005);   // grow: page count increases
        ASSERT_NE(p, nullptr);
        p = fast_realloc(p, 1500001);   // shrink: >= 1 page released
        ASSERT_NE(p, nullptr);
        fast_free(p);
    }
    // Battery 2: small realloc churn (class transitions).
    for (std::size_t sz : {100u, 1000u, 8000u}) {
        void* p = fast_malloc(sz);
        ASSERT_NE(p, nullptr);
        p = fast_realloc(p, sz * 2 + 5);
        ASSERT_NE(p, nullptr);
        p = fast_realloc(p, sz / 2 + 1);
        ASSERT_NE(p, nullptr);
        fast_free(p);
    }
    // Battery 3: plain large/small pairs (control: known-balanced).
    for (std::size_t sz : {100u, 8176u, 8177u, 100000u, 3000000u}) {
        void* p = fast_malloc(sz);
        ASSERT_NE(p, nullptr);
        fast_free(p);
    }
    // Battery 4: aligned alloc/free pairs (control).
    for (std::size_t align = 32; align <= 4096; align <<= 1) {
        void* p = fast_aligned_alloc(align, 1000);
        ASSERT_NE(p, nullptr);
        fast_free(p);
    }

    FastAllocStats after = fast_alloc_stats();
    EXPECT_EQ(after.current_live_blocks, before.current_live_blocks)
        << "live blocks drifted over a balanced battery";
    EXPECT_EQ(after.current_live_bytes, before.current_live_bytes)
        << "live bytes drifted over a balanced battery: "
        << "current_live_bytes before=" << before.current_live_bytes
        << " after=" << after.current_live_bytes;
}

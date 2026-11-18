// ============================================================================
// Statistics balance across realloc grow/shrink (current_live_bytes /
// current_live_blocks must net to zero over a balanced alloc/free battery).
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"
#include "test_hooks.h"

#include <cstring>
#include <vector>
#include <thread>

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

// Regression: cross-thread batched stats must never wrap the live counters
// or poison the peaks. Threads allocate and free in overlapping waves so
// free batches can land before the matching alloc batches; a transient
// wrap of g_live_bytes near SIZE_MAX must never ratchet peak_live_bytes
// to a garbage value. Post-condition: peaks are sane (< 2^63),
// current_live returns exactly to baseline.
TEST(StatsBalanceTest, CrossThreadPeakSanity) {
    FastAllocStats before = fast_alloc_stats();
    constexpr int kThreads = 8;
    constexpr int kRounds = 40;
    {
        // Scoped so the vector's own buffer (routed through FastAlloc when
        // new/delete are overridden) is freed before the after-snapshot.
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([t] {
                for (int r = 0; r < kRounds; ++r) {
                    std::vector<void*> live;
                    live.reserve(64);
                    std::size_t sz = 32 + (t * 7 + r * 13) % 4000;
                    for (int i = 0; i < 64; ++i) live.push_back(fast_malloc(sz));
                    if ((t + r) % 2 == 0) {
                        for (void* p : live) fast_free(p);
                    } else {
                        // Interleaved pattern: free two thirds, allocate a
                        // fresh wave while the rest are still live, then free
                        // the remainder. Every block is freed EXACTLY once
                        // (double frees are dropped by the release stamp guard
                        // but still count free bytes, breaking balance).
                        for (std::size_t i = 0; i < live.size(); ++i)
                            if (i % 3 != 0) fast_free(live[i]);
                        std::vector<void*> second;
                        second.reserve(16);
                        for (int i = 0; i < 16; ++i) second.push_back(fast_malloc(sz));
                        for (std::size_t i = 0; i < live.size(); ++i)
                            if (i % 3 == 0) fast_free(live[i]);
                        for (void* p : second) fast_free(p);
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
    }
    FastAllocStats after = fast_alloc_stats();
    // Peaks must stay under the sanity ceiling (a wrapped snapshot would
    // have ratcheted a peak to ~SIZE_MAX).
    EXPECT_LT(after.peak_live_blocks, (std::size_t)1 << 63);
    EXPECT_LT(after.peak_live_bytes, (std::size_t)1 << 63);
    // Live counters return exactly to baseline after a balanced battery.
    EXPECT_EQ(after.current_live_blocks, before.current_live_blocks);
    EXPECT_EQ(after.current_live_bytes, before.current_live_bytes);
}

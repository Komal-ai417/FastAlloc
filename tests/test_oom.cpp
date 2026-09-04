// ============================================================================
// Out-of-memory handling (audit gap: "how does the allocator behave under
// memory exhaustion?" was never defined or tested).
//
// The OOM seam fails the next N OS page allocations. fast_malloc must return
// nullptr, previously allocated memory must stay valid, and the allocator
// must return to full service afterwards.
//
// NOTE ON DETERMINISM: the allocator legitimately serves requests from the
// TLS cache, partial slabs and the page-span cache WITHOUT touching the OS.
// The OOM seam only fires when a fresh page allocation is genuinely needed,
// so each test first drains those caches (PurgeAll) and/or drains partial
// slabs by repeated allocation until a fresh slab is required.
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"
#include "fast_alloc_config.h"
#include "test_hooks.h"

#include <cstring>
#include <vector>

using namespace FastAlloc;

namespace {
// Drop the thread large cache and the global page-span cache so the next
// large allocation is guaranteed to require a fresh OS mapping.
void PurgeAll() {
    fast_alloc_purge_thread_cache();
    FastAllocTestPurgePageCache();
}
} // namespace

TEST(OomTest, SmallAllocationFailsCleanly) {
    // Drain the partial slab for this class by allocating repeatedly: once
    // the arena needs a FRESH slab, the injected OOM must fire.
    FastAllocTestSetOOMCountdown(1);
    std::vector<void*> held;
    void* p;
    while ((p = fast_malloc(1237)) != nullptr) {
        held.push_back(p);
        ASSERT_LT(held.size(), 100000u) << "OOM never fired";
    }
    EXPECT_FALSE(held.empty());   // the class worked before the failure
    FastAllocTestSetOOMCountdown(0);

    // Allocator fully recovered:
    void* q = fast_malloc(1237);
    ASSERT_NE(q, nullptr);
    std::memset(q, 0, 1237);
    fast_free(q);

    for (void* h : held) fast_free(h);
}

TEST(OomTest, LargeAllocationFailsCleanly) {
    PurgeAll();
    FastAllocTestSetOOMCountdown(1);
    void* p = fast_malloc(1024 * 1024);
    EXPECT_EQ(p, nullptr);
    FastAllocTestSetOOMCountdown(0);

    void* q = fast_malloc(1024 * 1024);
    ASSERT_NE(q, nullptr);
    std::memset(q, 0, 1024);
    fast_free(q);
}

TEST(OomTest, ExistingMemoryStaysValidDuringOom) {
    unsigned char* a = static_cast<unsigned char*>(fast_malloc(256));
    ASSERT_NE(a, nullptr);
    std::memset(a, 0x77, 256);

    PurgeAll();
    FastAllocTestSetOOMCountdown(100);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(fast_malloc(9999 + i * 4096 + 1), nullptr); // all fail
    }
    FastAllocTestSetOOMCountdown(0);

    // 'a' untouched through the whole OOM episode.
    for (int i = 0; i < 256; ++i) {
        ASSERT_EQ(a[i], 0x77);
    }
    fast_free(a);
}

TEST(OomTest, OomCountdownActuallyCounts) {
    PurgeAll();
    FastAllocTestSetOOMCountdown(2);
    EXPECT_EQ(FastAllocTestGetOOMCountdown(), 2);
    EXPECT_EQ(fast_malloc(55555), nullptr); // consumes one
    EXPECT_EQ(FastAllocTestGetOOMCountdown(), 1);
    EXPECT_EQ(fast_malloc(55555), nullptr); // consumes the other
    EXPECT_EQ(FastAllocTestGetOOMCountdown(), 0);
    // Countdown exhausted: allocation succeeds again.
    void* p = fast_malloc(55555);
    ASSERT_NE(p, nullptr);
    fast_free(p);
}

TEST(OomTest, AllocatorUsableAfterRepeatedOom) {
    // Hammer the failure path, then verify recovery each time. Unique large
    // sizes + purge guarantee a fresh page span is required every round.
    for (int round = 0; round < 8; ++round) {
        const std::size_t sz = 64u * 1024 + round * 8192 + 1;
        PurgeAll();
        FastAllocTestSetOOMCountdown(1);
        EXPECT_EQ(fast_malloc(sz), nullptr) << "round " << round;
        FastAllocTestSetOOMCountdown(0);
        void* p = fast_malloc(sz);
        ASSERT_NE(p, nullptr) << "round " << round;
        std::memset(p, 0x21, 256);
        fast_free(p);
    }
    PurgeAll();
}

TEST(OomTest, ThreadCacheStillServesFastPathDuringOom) {
    // Populate the thread-local cache first.
    void* warm[64];
    for (int i = 0; i < 64; ++i) {
        warm[i] = fast_malloc(48);
        ASSERT_NE(warm[i], nullptr);
    }
    for (int i = 0; i < 64; ++i) fast_free(warm[i]);

    // OOM only affects the SLOW path (OS allocation): the TLS cache must
    // still hand out cached blocks.
    FastAllocTestSetOOMCountdown(50);
    void* p = fast_malloc(48);
    EXPECT_NE(p, nullptr); // served from TLS cache, no OS call
    fast_free(p);
    FastAllocTestSetOOMCountdown(0);
}

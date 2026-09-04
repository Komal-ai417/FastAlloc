// ============================================================================
// Debug-instrumentation tests: canaries, red zones, double-free detection,
// invalid-free detection, UAF poison, leak reporting, statistics.
//
// The detection tests are meaningful ONLY in FASTALLOC_DEBUG builds; in
// release builds they are skipped (detection is intentionally absent there).
// Violations abort the process: we run them in forked children via Google
// Test death tests and match the diagnostic text on stderr.
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"
#include "fast_alloc_config.h"
#include "debug_aid.h"

#include <cstring>
#include <cstdlib>

using namespace FastAlloc;

#if FASTALLOC_DEBUG_ENABLED && !defined(_WIN32)

// ---------------------------------------------------------------------------
// Detection tests (death tests - each runs in a forked child)
// ---------------------------------------------------------------------------
TEST(DebugAidsDeathTest, DoubleFreeIsDetected) {
    void* p = fast_malloc(64);
    ASSERT_NE(p, nullptr);
    fast_free(p);
    EXPECT_DEATH(fast_free(p), "DOUBLE FREE");
}

TEST(DebugAidsDeathTest, DoubleFreeLargeIsDetected) {
    void* p = fast_malloc(64 * 1024);
    ASSERT_NE(p, nullptr);
    fast_free(p);
    EXPECT_DEATH(fast_free(p), "DOUBLE FREE");
}

TEST(DebugAidsDeathTest, InvalidFreeStackPointerIsDetected) {
    // A pointer FastAlloc never allocated must be rejected loudly instead of
    // corrupting the arena (audit C3). Under ASan, the sanitizer itself may
    // catch the wild access first - both diagnostics are acceptable proof.
    EXPECT_DEATH({
        int stack_var = 0;
        fast_free(&stack_var);
    }, "not live|stack-buffer|FATAL");
}

TEST(DebugAidsDeathTest, BufferOverflowRearCanaryIsDetected) {
    EXPECT_DEATH({
        unsigned char* p = static_cast<unsigned char*>(fast_malloc(16));
        p[16] = 0x41; // one byte past the requested image -> red zone
        fast_free(p); // the red-zone check fires HERE
    }, "OVERFLOW|not live|FATAL");
}

TEST(DebugAidsDeathTest, BufferUnderflowFrontCanaryIsDetected) {
    EXPECT_DEATH({
        unsigned char* p = static_cast<unsigned char*>(fast_malloc(16));
        p[-1] = 0x41; // one byte before the user image -> front canary
        fast_free(p);
    }, "UNDERFLOW|not live|FATAL");
}

TEST(DebugAidsDeathTest, LargeBlockUnderflowDestroysHeader) {
    EXPECT_DEATH({
        unsigned char* p = static_cast<unsigned char*>(fast_malloc(64 * 1024));
        p[-1] = 0x41; // overwrites the large header's alloc_size field
        fast_free(p);
    }, "UNDERFLOW|bad magic|corrupted|not live|FATAL");
}

TEST(DebugAidsDeathTest, UseAfterFreeWriteIsDetectedOnReuse) {
    // Write AFTER free, then allocate the same class again: the poison
    // check on allocation must fire.
    EXPECT_DEATH({
        unsigned char* p = static_cast<unsigned char*>(fast_malloc(32));
        fast_free(p);
        p[8] = 0x42; // UAF write past the free-list link (offset 8..16 is poison)
        void* q = fast_malloc(32);
        (void)q;
    }, "use-after-free|FATAL");
}

// ---------------------------------------------------------------------------
// Leak reporting
// ---------------------------------------------------------------------------
TEST(DebugAidsTest, LeakRegistryTracksLiveBlocks) {
    FastAllocStats before = fast_alloc_stats();

    void* a = fast_malloc(100);
    ASSERT_NE(a, nullptr);
    void* b = fast_malloc(200);
    ASSERT_NE(b, nullptr);
    fast_free(a);

    FastAllocStats mid = fast_alloc_stats();
    EXPECT_EQ(mid.current_live_blocks, before.current_live_blocks + 1);
    EXPECT_EQ(mid.current_live_bytes, before.current_live_bytes + 200);

    fast_free(b);
    FastAllocStats after = fast_alloc_stats();
    EXPECT_EQ(after.current_live_blocks, before.current_live_blocks);
    EXPECT_EQ(after.current_live_bytes, before.current_live_bytes);
}

TEST(DebugAidsTest, StatsReportPeak) {
    void* a = fast_malloc(1024);
    ASSERT_NE(a, nullptr);
    void* b = fast_malloc(4096);
    ASSERT_NE(b, nullptr);
    fast_free(a);
    FastAllocStats s = fast_alloc_stats();
    EXPECT_GE(s.peak_live_blocks, 2u);
    fast_free(b);
}

// ---------------------------------------------------------------------------
// Canary positive behaviour: well-behaved code is NOT interrupted
// ---------------------------------------------------------------------------
TEST(DebugAidsTest, ExactSizeWritesNeverTrip) {
    // Writing every byte of the requested image across many classes must
    // never trigger any check.
    for (std::size_t s = 1; s <= 4096; s = s * 2 + 3) {
        unsigned char* p = static_cast<unsigned char*>(fast_malloc(s));
        ASSERT_NE(p, nullptr);
        std::memset(p, 0xAA, s);
        fast_free(p);
    }
    SUCCEED();
}

TEST(DebugAidsTest, ViolationHandlerHook) {
    // A custom handler can observe violations before termination. Run in a
    // child process so the abort does not kill the test binary.
    EXPECT_EXIT({
        static ViolationInfo captured{};
        SetViolationHandler([](const ViolationInfo& info) {
            captured = info;
            std::fprintf(stderr, "HOOK-FIRED kind=%d ptr=%p\n",
                         static_cast<int>(info.kind), info.ptr);
            std::abort();
        });
        void* p = fast_malloc(64);
        fast_free(p);
        fast_free(p); // double free -> handler must run
    }, ::testing::KilledBySignal(SIGABRT), "HOOK-FIRED kind=1");
}

#endif // FASTALLOC_DEBUG_ENABLED && !_WIN32

// ---------------------------------------------------------------------------
// Logging API (available whenever logging is compiled in)
// ---------------------------------------------------------------------------
#if FASTALLOC_LOGGING_ENABLED
TEST(DebugAidsTest, LogLevelSetGet) {
    fast_alloc_log_set_level(0);
    EXPECT_EQ(static_cast<int>(LogGetLevel()), 0);
    fast_alloc_log_set_level(4);
    EXPECT_EQ(static_cast<int>(LogGetLevel()), 4);
    fast_alloc_log_set_level(0);
}
#else
TEST(DebugAidsTest, LogLevelNoopInRelease) {
    fast_alloc_log_set_level(4); // must be a silent no-op
    SUCCEED();
}
#endif

// ---------------------------------------------------------------------------
// Leak dump entry point exists and does not crash in either mode
// ---------------------------------------------------------------------------
TEST(DebugAidsTest, DumpLeaksDoesNotCrash) {
    void* p = fast_malloc(64);
    ASSERT_NE(p, nullptr);
    fast_alloc_dump_leaks(); // live allocation present
    fast_free(p);
    fast_alloc_dump_leaks(); // clean
    SUCCEED();
}

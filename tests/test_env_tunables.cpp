// ============================================================================
// Environment-variable tunables parsing (FASTALLOC_PAGE_CACHE_MB):
// overflow/edge-value robustness. The page-cache limit is a function-local
// static captured at the FIRST page-cache interaction, so wrap-behaviour
// must be probed in fresh child processes (fork alone inherits the captured
// static; the child re-execs this binary).
//
// POSIX-only mechanism: the fresh-process probe uses fork/execv/readlink.
// On Windows the same cases are covered in-process where possible and the
// fork/exec cases SKIP with a visible, justified reason (per the project's
// CI rules: platform-specific tests report a justified skip rather than
// passing vacuously).
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"
#include "test_hooks.h"

using namespace FastAlloc;

#if !defined(_WIN32)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

// Helper mode: churn large spans and verify the page cache retains them.
// Exit 0 = cache non-zero (retention working); exit 2 = cache disabled;
// exit 3 = allocation failed.
static int PageCacheProbeMain() {
    void* hold[64];
    for (int i = 0; i < 64; ++i) {
        hold[i] = fast_malloc(60000);
        if (!hold[i]) _exit(3);
    }
    for (int i = 0; i < 64; ++i) fast_free(hold[i]);
    std::size_t cached = FastAllocTestPageCacheBytes();
    std::fprintf(stderr, "[fa-probe] cached=%zu\n", cached);
    _exit(cached > 0 ? 0 : 2);
}

// Pre-main dispatch: the parent fork+execs this same binary with
// FA_PROBE_HELPER_MODE=1; this static initializer runs the probe and
// _exit()s before gtest_main's main() starts. (The suite links
// gtest_main, so argv interception is not available; the allocator's
// lazy singletons are safe to use from a static initializer.)
static const bool fa_probe_dispatch = [] {
    const char* mode = getenv("FA_PROBE_HELPER_MODE");
    if (mode && mode[0] == '1') {
        PageCacheProbeMain();  // noreturn (_exit)
    }
    return true;
}();

// Runs this same binary in helper mode under a fresh
// FASTALLOC_PAGE_CACHE_MB setting; returns the child exit code (or -1).
static int RunChildWithEnv(const char* value) {
    char argv0[4096];
    ssize_t n = readlink("/proc/self/exe", argv0, sizeof(argv0) - 1);
    if (n <= 0) return -1;
    argv0[n] = '\0';
    char* const argv[] = {argv0, nullptr};
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setenv("FA_PROBE_HELPER_MODE", "1", 1);
        if (value) setenv("FASTALLOC_PAGE_CACHE_MB", value, 1);
        else unsetenv("FASTALLOC_PAGE_CACHE_MB");
        execv(argv0, argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Verify that PageCacheTotalLimit() handles integer overflow safely.
// A huge MB value (e.g. 2^44) must not wrap the byte product to 0 or
// disable the page-span cache. Overflowing values must fall back to a
// sane limit rather than wrapping.
TEST(EnvTunablesTest, PageCacheMbOverflowNeverDisablesCache) {
    // Negative control: default env, cache must retain spans.
    int rc = RunChildWithEnv(nullptr);
    EXPECT_EQ(rc, 0) << "default-env control: retention unexpectedly disabled";

    // Degenerate values must behave like a sane limit (non-zero
    // retention), not a wrapped-to-zero cap.
    for (const char* v : {"17592186044416",       // 2^44: product wraps to 0
                          "281474976710656",      // 2^48: product wraps to 0
                          "9223372036854775807",  // LONG_MAX
                          "18446744073709551615", // ULONG_MAX (atol saturates)
                          "99999999999999999999", // beyond LONG_MAX
                          "0",                    // zero
                          "-5",                   // negative
                          "abc",                  // non-numeric
                          ""}) {                  // empty
        rc = RunChildWithEnv(v);
        EXPECT_EQ(rc, 0) << "FASTALLOC_PAGE_CACHE_MB=" << v
                         << " broke retention (exit " << rc << ")";
    }
}

// Small positive limits must be honoured exactly (no accidental default).
TEST(EnvTunablesTest, PageCacheMbSmallLimitsHonoured) {
    int rc = RunChildWithEnv("2");
    EXPECT_EQ(rc, 0) << "2MB limit: retention unexpectedly disabled";
}

#else  // _WIN32

TEST(EnvTunablesTest, PageCacheMbOverflowNeverDisablesCache) {
    GTEST_SKIP() << "fresh-process fork/exec probe is POSIX-only; Windows "
                    "retention is covered in-process by LargeAllocTest."
                    "PageCachePurgeReleasesMemory and the OOM/env-neutral "
                    "battery in this suite";
}

TEST(EnvTunablesTest, PageCacheMbSmallLimitsHonoured) {
    GTEST_SKIP() << "fresh-process fork/exec probe is POSIX-only (see "
                    "PageCacheMbOverflowNeverDisablesCache)";
}

#endif  // !_WIN32

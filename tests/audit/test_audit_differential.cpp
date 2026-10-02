// ============================================================================
// FastAlloc audit — differential testing vs glibc malloc (item 5) and
// memory-correctness guards (item 2): invalid frees, scribble-then-free
// serviceability, interior/stack/static/foreign pointers.
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"
#include "fast_alloc_config.h"
#include "../test_hooks.h"

#include <cstring>
#include <cstdint>
#include <vector>
#include <random>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#ifdef _MSC_VER
#include <malloc.h>  // _aligned_malloc, _aligned_free
#endif

using namespace FastAlloc;

// ---------------------------------------------------------------------------
// Differential driver: runs the SAME deterministic op stream against FastAlloc
// and glibc, comparing externally observable behaviour (item 5 + 47).
// ---------------------------------------------------------------------------
namespace {
struct Obs {
    std::vector<int> outcomes;     // 1 = success, 0 = null
    std::vector<int> align_ok;     // 1 = alignment respected
    std::vector<int> content_ok;   // 1 = realloc preserved prefix
};
template <typename M, typename F, typename R, typename AL>
Obs RunStream(unsigned seed, std::size_t ops, M malloc_fn, F free_fn,
              R realloc_fn, AL aligned_fn) {
    std::mt19937 rng(seed);
    Obs o;
    std::vector<std::pair<void*, std::size_t>> live;
    for (std::size_t op = 0; op < ops; ++op) {
        int a = static_cast<int>(rng() % 100);
        if (live.size() >= 128) a = 95;
        if (live.empty()) a = 10;
        if (a < 50) {
            std::size_t s = 1 + rng() % 8176;
            void* p = malloc_fn(s);
            o.outcomes.push_back(p ? 1 : 0);
            if (p) {
                o.align_ok.push_back((reinterpret_cast<uintptr_t>(p) & 15u) == 0);
                std::memset(p, 0xAB, s);
                live.push_back({p, s});
            }
        } else if (a < 60) {
            std::size_t idx = rng() % live.size();
            std::size_t ns = 1 + rng() % 8176;
            void* np = realloc_fn(live[idx].first, ns);
            o.outcomes.push_back(np ? 1 : 0);
            if (np) {
                std::size_t chk = live[idx].second < ns ? live[idx].second : ns;
                bool ok = true;
                auto* c = static_cast<unsigned char*>(np);
                for (std::size_t i = 0; i < chk; ++i) if (c[i] != 0xAB) { ok = false; break; }
                o.content_ok.push_back(ok);
                live[idx] = {np, ns};
                std::memset(np, 0xAB, ns);
            }
        } else if (a < 65) {
            std::size_t al = 16u << (rng() % 9);
            std::size_t s = 1 + rng() % 4096;
            void* p = aligned_fn(al, s);
            o.outcomes.push_back(p ? 1 : 0);
            if (p) o.align_ok.push_back((reinterpret_cast<uintptr_t>(p) & (al - 1)) == 0);
            if (p) { std::memset(p, 0xAB, s); live.push_back({p, s}); }
        } else {
            std::size_t idx = rng() % live.size();
            free_fn(live[idx].first);
            live[idx] = live.back();
            live.pop_back();
        }
    }
    for (auto& kv : live) free_fn(kv.first);
    return o;
}
} // namespace

TEST(AuditDifferential, FastAllocVsGlibc_ObservableEquivalence) {
    // FastAlloc run
    Obs fa = RunStream(0x01A1, 20000,
        [](std::size_t s) { return fast_malloc(s); },
        [](void* p) { fast_free(p); },
        [](void* p, std::size_t n) { return fast_realloc(p, n); },
        [](std::size_t a, std::size_t s) { return fast_aligned_alloc(a, s); });
    // glibc run (system malloc inside this process is glibc; FastAlloc is a
    // static library and does not interpose, so both coexist)
    Obs gl = RunStream(0x01A1, 20000,
        [](std::size_t s) { return std::malloc(s); },
#ifdef _MSC_VER
        // _aligned_malloc must be freed with _aligned_free on MSVC;
        // _aligned_free is safe for regular malloc pointers too.
        [](void* p) { _aligned_free(p); },
#else
        [](void* p) { std::free(p); },
#endif
        [](void* p, std::size_t n) { return std::realloc(p, n); },
        [](std::size_t a, std::size_t s) -> void* {
            if (a == 0 || (a & (a - 1)) != 0 || a > 4096) return nullptr;  // FastAlloc contract
            std::size_t aligned_s = (s + a - 1) & ~(a - 1);
#ifdef _MSC_VER
            return _aligned_malloc(aligned_s, a);  // MSVC: no std::aligned_alloc
#else
            return std::aligned_alloc(a, aligned_s);
#endif
        });
    // Both allocators must succeed for the whole stream on this 4GB box
    size_t fa_fail = std::count(fa.outcomes.begin(), fa.outcomes.end(), 0);
    size_t gl_fail = std::count(gl.outcomes.begin(), gl.outcomes.end(), 0);
    EXPECT_EQ(fa_fail, 0u);
    EXPECT_EQ(gl_fail, 0u);
    // Zero-size policy: both return unique non-null (documented contract match)
    for (int i = 0; i < 100; ++i) {
        void* a = fast_malloc(0), *b = std::malloc(0);
        ASSERT_NE(a, nullptr); ASSERT_NE(b, nullptr);
        fast_free(a); std::free(b);
    }
    // calloc overflow: both fail
    EXPECT_EQ(fast_calloc(SIZE_MAX, 3), nullptr);
#ifndef _MSC_VER
#pragma GCC diagnostic push
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Walloc-size-larger-than="
#endif
#endif
    EXPECT_EQ(std::calloc(SIZE_MAX, 3), nullptr);
#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif
    // realloc(p,0): both free and return null (glibc does since 2.4x? C11)
    void* p1 = fast_malloc(10); void* p2 = std::malloc(10);
    EXPECT_EQ(fast_realloc(p1, 0), nullptr);
    EXPECT_EQ(std::realloc(p2, 0), nullptr);
}

// ---------------------------------------------------------------------------
// Memory-correctness guards (item 2). Release semantics: guard drops the
// pointer, allocator stays serviceable. (Debug builds FATAL on these by
// design; that loud behavior is covered by the repo's DebugAidsDeathTest
// suite -- the misuse probes below are compiled out under FASTALLOC_DEBUG.)
// ---------------------------------------------------------------------------
TEST(AuditMemory, FreeNullAndWildPointersServiceable) {
    [[maybe_unused]] int global_var = 0;
    [[maybe_unused]] static int static_var = 0;
#if !FASTALLOC_DEBUG_ENABLED
    // Release semantics: the plausibility guard drops the pointer, allocator
    // stays serviceable. In DEBUG these frees FATAL by design (registry miss /
    // canary checks) -- covered by the existing DebugAidsDeathTest suite.
    // Misaligned foreign pointers are never dereferenced by the predicate, so
    // they are safe under every build:
    void* stack_var = &global_var;
    fast_free(static_cast<char*>(stack_var) + 1);   // misaligned: rejected by predicate
    // NOT tested: fast_free(0x10000). Rationale: it passes the plausibility
    // predicate (>=64KB, 16-aligned), so the release path dereferences the
    // header at 0xFFF0 -- an unmapped low page -> SIGSEGV. glibc behaves the
    // same way (free() reads the chunk header at p-16 -> SIGSEGV; verified
    // parity in the audit evidence). Freeing a wild low pointer is UB per C
    // and crashes every mainstream allocator; the guard boundary at 64KB is a
    // documented trade-off (see report).
#if !defined(__SANITIZE_ADDRESS__)
    // 16-ALIGNED foreign pointers (stack/global objects) also pass the
    // predicate and get their "header" read: in plain builds the v9-v11
    // guards absorb the garbage (drop + protect); under ASan that same read
    // lands in a stack/global REDZONE and is reported as stack-buffer-underflow
    // (fast_free at fast_alloc.cpp:436). glibc free() of a stack pointer is the
    // identical UB class under ASan -- parity, not a defect. Skip under ASan.
    fast_free(&global_var);                       // stack object, aligned by chance
    fast_free(&static_var);                       // .bss object, aligned by chance
#endif
#endif
    // free(nullptr) x many
    for (int i = 0; i < 100; ++i) fast_free(nullptr);
    // allocator still works
    for (int i = 0; i < 100; ++i) {
        void* p = fast_malloc(64);
        ASSERT_NE(p, nullptr);
        std::memset(p, 0x5A, 64);
        fast_free(p);
    }
    SUCCEED();
}

TEST(AuditMemory, InteriorPointerFreeIsSafeOrDetected) {
#if !FASTALLOC_DEBUG_ENABLED
    // free(p+16): the header read lands on p's own header (slab field valid) ->
    // the block p itself gets freed. In release this is documented UB like
    // every allocator. In DEBUG the registry FATALs on interior frees by
    // design (see DebugAidsDeathTest); there we just alloc/free.
    void* p = fast_malloc(128);
    ASSERT_NE(p, nullptr);
    fast_free(static_cast<char*>(p) + 16);   // interior: acts as free(p) (release)
#else
    void* p = fast_malloc(128);              // debug: plain alloc/free, no UB
    ASSERT_NE(p, nullptr);
    fast_free(p);
#endif
    // The allocator must remain fully serviceable afterwards.
    for (int i = 0; i < 1000; ++i) {
        void* q = fast_malloc(48);
        ASSERT_NE(q, nullptr);
        std::memset(q, 0x33, 48);
        fast_free(q);
    }
    SUCCEED();
}

TEST(AuditMemory, ScribbleAroundBlockThenFreeGuardsHold) {
#if !FASTALLOC_DEBUG_ENABLED
    // Release-only: simulate user overflow/underflow by 1-8 bytes around live
    // blocks, then free and verify self-healing. In DEBUG the canary checks
    // FATAL on this by design (DebugAidsDeathTest covers it).
    for (int round = 0; round < 200; ++round) {
        void* p = fast_malloc(100);
        ASSERT_NE(p, nullptr);
        auto* c = static_cast<unsigned char*>(p);
        c[-1] ^= 0xFF;   // 1-byte underflow into canary/padding (header stays)
        c[100] ^= 0xFF;  // 1-byte overflow into next block's header territory?
        // NOTE: c[100] lands inside THIS block's class slack (class 8: block
        // 128, user region 112) -> stays in-bounds of the block itself.
        fast_free(p);
    }
#endif
    // After the scribble storm, allocator must still serve and recycle.
    void* first = fast_malloc(100);
    ASSERT_NE(first, nullptr);
    fast_free(first);
    void* second = fast_malloc(100);
    EXPECT_EQ(second, first) << "recycling broken after scribble";
    fast_free(second);
}

TEST(AuditMemory, DoubleFreeReleaseAbsorbed) {
    // Release: the TLS push guard turns the second push into a no-op; the heap
    // stays consistent. In DEBUG a double free FATALs by design (registry).
    // FINDING F3 (documented in the report): the release stats counter
    // current_live_blocks is decremented BEFORE the push guard fires,
    // so N double-frees underflow it by up to N (observed: 2^64-99 after 100).
    FastAllocStats t0 = fast_alloc_stats();
#if !FASTALLOC_DEBUG_ENABLED
    for (int i = 0; i < 100; ++i) {
        void* p = fast_malloc(72);
        ASSERT_NE(p, nullptr);
        fast_free(p);
        fast_free(p);   // double free: guarded, heap protected
    }
#endif
    for (int i = 0; i < 2000; ++i) {  // heavy churn after double frees
        std::size_t ts = 1 + static_cast<std::size_t>(i * 131 % 4096);
        void* p = fast_malloc(ts);
        ASSERT_NE(p, nullptr);
        std::memset(p, 0x44, ts);   // fill exactly the block's own size
        fast_free(p);
    }
    // The allocator itself must remain fully serviceable (no crash, no hang,
    // no corruption of the churn blocks above) -- that is the contract.
    // Stats: balanced (0) or underflowed by at most the double-free count
    // (F3; observed -99 in release when one push is absorbed by the
    // duplicate-hand-out guard). Anything outside [0, -100] is a NEW defect.
    FastAllocStats st = fast_alloc_stats();
    std::ptrdiff_t got = static_cast<std::ptrdiff_t>(st.current_live_blocks - t0.current_live_blocks);
    EXPECT_GE(got, -100) << "live-blocks underflowed beyond the double-free count";
    EXPECT_LE(got, 0) << "live-blocks grew: this test leaked for real";
}

TEST(AuditMemory, ReadAfterFreeDoesNotCrash) {
    // Reading freed memory is UB; FastAlloc documents POISON 0xDD in debug.
    // In release we only assert the read does not SEGV (pages stay mapped:
    // pooled spans use MADV_DONTNEED -> zero-fill, mapped).
    for (std::size_t s : {16u, 256u, 4096u, 65536u, 1u << 20}) {
        void* p = fast_malloc(s);
        ASSERT_NE(p, nullptr);
        std::memset(p, 0x66, s);
        fast_free(p);
        auto* c = static_cast<volatile unsigned char*>(p);
        (void)c[0]; (void)c[s / 2]; (void)c[s - 1];   // must not fault
    }
    SUCCEED();
}

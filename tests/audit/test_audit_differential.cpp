// ============================================================================
// Differential testing vs system allocator and memory-correctness guards:
// invalid frees, scribble-then-free serviceability, interior/stack/static/foreign pointers.
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
    // Reference run: the system allocator inside this process (glibc on
    // Linux; FastAlloc is a static library and does not interpose, so both
    // coexist).
#ifdef _MSC_VER
    // MSVC has no std::aligned_alloc, and _aligned_free is ONLY defined for
    // blocks from _aligned_malloc/_aligned_realloc (Microsoft docs: freeing
    // any other pointer is undefined behaviour -- the CRT reads an internal
    // offset header that plain malloc/realloc blocks do not have). RunStream
    // frees malloc-, realloc- AND aligned-blocks through ONE free lambda, so
    // the reference run must come from a single allocation family: route
    // everything through the aligned heap. 16-byte alignment matches what
    // glibc malloc returns, keeping the observable contract equivalent.
    Obs gl = RunStream(0x01A1, 20000,
        [](std::size_t s) { return _aligned_malloc(s, 16); },
        [](void* p) { _aligned_free(p); },
        [](void* p, std::size_t n) { return _aligned_realloc(p, n, 16); },
        [](std::size_t a, std::size_t s) -> void* {
            if (a == 0 || (a & (a - 1)) != 0 || a > 4096) return nullptr;
            std::size_t aligned_s = (s + a - 1) & ~(a - 1);
            return _aligned_malloc(aligned_s, a);
        });
#elif defined(_WIN32)
    // MinGW: _WIN32 defined but not _MSC_VER. std::aligned_alloc is absent
    // in MinGW's <cstdlib>. Mirror the MSVC strategy: route everything
    // through the _aligned_* family so the single free lambda is always
    // correct (_aligned_free is required for _aligned_malloc blocks).
    Obs gl = RunStream(0x01A1, 20000,
        [](std::size_t s) { return _aligned_malloc(s, 16); },
        [](void* p) { _aligned_free(p); },
        [](void* p, std::size_t n) { return _aligned_realloc(p, n, 16); },
        [](std::size_t a, std::size_t s) -> void* {
            if (a == 0 || (a & (a - 1)) != 0 || a > 4096) return nullptr;
            std::size_t aligned_s = (s + a - 1) & ~(a - 1);
            return _aligned_malloc(aligned_s, a);
        });
#else
    Obs gl = RunStream(0x01A1, 20000,
        [](std::size_t s) { return std::malloc(s); },
        [](void* p) { std::free(p); },
        [](void* p, std::size_t n) { return std::realloc(p, n); },
        [](std::size_t a, std::size_t s) -> void* {
            if (a == 0 || (a & (a - 1)) != 0 || a > 4096) return nullptr;
            std::size_t aligned_s = (s + a - 1) & ~(a - 1);
            return std::aligned_alloc(a, aligned_s);
        });
#endif
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
    // 16-aligned foreign pointers (stack/global objects) pass the plausibility
    // predicate and have their header read. In standard builds, the entry guards
    // safely drop the pointer. Under ASan, the read lands in an ASan redzone
    // and is reported as a stack-buffer-underflow, so skip under ASan.
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
    // c[-1] lands on the canary dword's high byte, which carries the
    // release live/free state stamp. A scribbled block is
    // therefore DROPPED (leaked) at free instead of recycled - strictly
    // safer than the old padding behaviour, and symmetric with DEBUG's
    // fatal canary. The post-storm recycling assertion below only uses
    // unscribbled blocks, so it is unaffected.
    for (int round = 0; round < 200; ++round) {
        void* p = fast_malloc(100);
        ASSERT_NE(p, nullptr);
        auto* c = static_cast<unsigned char*>(p);
        c[-1] ^= 0xFF;   // 1-byte underflow into the canary/stamp dword
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
    // The release stats counter
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
    // (a release run observed -99 when one push is absorbed by the
    // duplicate-hand-out guard). Anything outside [0, -100] is a NEW defect.
    FastAllocStats st = fast_alloc_stats();
    std::ptrdiff_t got = static_cast<std::ptrdiff_t>(st.current_live_blocks - t0.current_live_blocks);
    EXPECT_GE(got, -100) << "live-blocks underflowed beyond the double-free count";
    EXPECT_LE(got, 0) << "live-blocks grew: this test leaked for real";
}

TEST(AuditMemory, NonHeadDoubleFreeGuardedByStateStamp) {
#if !FASTALLOC_DEBUG_ENABLED
    // Regression: double-free detection must not depend on the victim being
    // the CURRENT freelist head. Freeing A, then B, then A again leaves A
    // non-head - linking A twice builds a cycle (A->B->A) and later hands
    // one block to two owners. The canary dword carries a
    // live/free state stamp, so the second free of A is dropped and
    // reported, and the bin holds exactly {B, A}.
    void* a = fast_malloc(72);
    void* b = fast_malloc(72);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    fast_free(a);   // bin: A (head)
    fast_free(b);   // bin: B -> A (B is head; A is NOT head)
    fast_free(a);   // non-head double free: must be dropped, not linked
    // The next two same-class allocations must return exactly the two
    // freed blocks (either LIFO order), never a duplicate.
    void* x = fast_malloc(72);
    void* y = fast_malloc(72);
    void* z = fast_malloc(72);
    ASSERT_NE(x, nullptr);
    ASSERT_NE(y, nullptr);
    ASSERT_NE(z, nullptr);
    EXPECT_TRUE((x == a && y == b) || (x == b && y == a))
        << "bin did not contain exactly {A, B} after the guarded double free";
    EXPECT_NE(z, a);
    EXPECT_NE(z, b);
    EXPECT_NE(x, y) << "one block handed to two owners";
    // Both owners' blocks stay writable and independent.
    std::memset(x, 0x11, 72);
    std::memset(y, 0x22, 72);
    std::memset(z, 0x33, 72);
    ASSERT_EQ(static_cast<unsigned char*>(x)[71], 0x11);
    ASSERT_EQ(static_cast<unsigned char*>(y)[71], 0x22);
    fast_free(x);
    fast_free(y);
    fast_free(z);
#endif
    // Heavy same-class churn after the guarded event.
    for (int i = 0; i < 5000; ++i) {
        void* p = fast_malloc(72);
        ASSERT_NE(p, nullptr);
        std::memset(p, 0x44, 72);
        fast_free(p);
    }
}

TEST(AuditMemory, CorruptClassIndexReallocRejectedNotRead) {
#if !FASTALLOC_DEBUG_ENABLED
    // Regression: the realloc copy path must bounds-check class_index the
    // way fast_free does. Computing old_size =
    // UsableSize(block->class_index) unchecked lets a corrupted class_index
    // (0xFFFF) read past the size table and use out-of-bounds garbage as the
    // shrink/copy bound - it must be rejected with EINVAL instead.
    void* p = fast_malloc(64);
    ASSERT_NE(p, nullptr);
    // class_index lives at header offset 8 = user pointer - 8.
    *reinterpret_cast<std::uint32_t*>(static_cast<char*>(p) - 8) = 0xFFFFu;
    errno = 0;
    void* q = fast_realloc(p, 128);
    EXPECT_EQ(q, nullptr) << "corrupt class_index accepted by realloc";
    EXPECT_EQ(errno, EINVAL);
    // The corrupted block is dropped (leak-by-contract); the allocator must
    // remain fully serviceable.
    for (int i = 0; i < 200; ++i) {
        void* w = fast_malloc(64);
        ASSERT_NE(w, nullptr);
        std::memset(w, 0x55, 64);
        fast_free(w);
    }
#endif
    SUCCEED();
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

// ---- aligned stash validation + >1 GiB aligned blocks ------------------------

TEST(AuditMemory, AlignedReallocAboveOneGiBIsLegitimate) {
    // Regression: fast_realloc's aligned branch must not cap the stashed
    // old_size at 1 GiB - a perfectly valid fast_aligned_alloc(64,
    // 1.5 GiB) would fail ANY subsequent realloc with EINVAL and a bogus
    // "corrupt large header" report. The span is lazily faulted, so this
    // costs ~no RSS: only the shrink-in-place path is exercised (stash
    // update, no copy).
    const std::size_t big = (1ull << 30) + (512ull << 20);  // 1.5 GiB
    void* p = fast_aligned_alloc(64, big);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 64u, 0u);
    void* q = fast_realloc(p, (1ull << 30) + (256ull << 20));  // 1.25 GiB
    EXPECT_EQ(q, p) << "in-place shrink of a >1 GiB aligned block rejected";
    // And a size update must round-trip through the stash.
    EXPECT_EQ(fast_realloc(q, (1ull << 30)), q);
    fast_free(q);
}

TEST(AuditMemory, ForgedAlignedStashRoundTripGuarded) {
#if !FASTALLOC_DEBUG_ENABLED
    // Release-only misuse simulation (debug registry FATALs on corrupted
    // provenance by design). The aligned stash ([alignment][raw][MAGIC]
    // [size] at ptr-32..-8) is user-writable: a forged raw must NOT be
    // recursively freed (round-trip validation), and a forged
    // size must NOT become a memcpy bound (capacity cross-check).
    void* victim = fast_malloc(64);
    ASSERT_NE(victim, nullptr);
    std::memset(victim, 0x5A, 64);

    // (a) Forged raw pointing at an unrelated LIVE block: fast_free must
    // drop the aligned pointer instead of freeing the victim.
    void* p = fast_aligned_alloc(64, 128);
    ASSERT_NE(p, nullptr);
    *reinterpret_cast<void**>(static_cast<char*>(p) - 24) = victim;
    // Precondition: the forged raw must not accidentally round-trip to p
    // (victim would then look like a legitimate raw and the free would be
    // correct behaviour, making this sub-case vacuous). Victim must sit
    // outside the 64-byte window below p.
    ASSERT_NE((reinterpret_cast<std::uintptr_t>(victim) + 32 + 63) &
                  ~std::uintptr_t{63},
              reinterpret_cast<std::uintptr_t>(p))
        << "test setup: victim accidentally round-trips to p";
    fast_free(p);  // must be rejected and dropped (leaked by contract)
    for (int i = 0; i < 64; ++i) {
        ASSERT_EQ(static_cast<unsigned char*>(victim)[i], 0x5A)
            << "forged raw freed a live block";
    }

    // (b) Forged size far beyond the raw block's capacity: realloc must
    // fail cleanly instead of using it as a copy bound (OOB read under
    // the old code, caught by the ASan jobs).
    void* r = fast_aligned_alloc(64, 64);
    ASSERT_NE(r, nullptr);
    *reinterpret_cast<std::size_t*>(static_cast<char*>(r) - 8) = 100 * 1024;
    errno = 0;
    void* s = fast_realloc(r, 100 * 1024);
    EXPECT_EQ(s, nullptr) << "forged stash size accepted as a copy bound";
    EXPECT_EQ(errno, EINVAL);
    // r was rejected, not freed or moved: the allocator must stay healthy.
    void* u = fast_malloc(64);
    ASSERT_NE(u, nullptr);
    EXPECT_NE(u, r);
    fast_free(u);
    fast_free(r);      // still the owner; normal free of the (restored-size) block
    fast_free(victim); // never freed by the forged path
#endif
    // Serviceability churn after the misuse pair.
    for (int i = 0; i < 500; ++i) {
        void* w = fast_malloc(97);
        ASSERT_NE(w, nullptr);
        std::memset(w, 0x33, 97);
        fast_free(w);
    }
}

// ---- page-cache parked-bytes accounting + per-bin budget ---------------------

TEST(AuditMemory, PageCacheBytesStatTracksParking) {
    // Regression: page_cache_bytes must track every state change, not just
    // stores and evicts. The POP paths (exact-bin hit and best-fit split)
    // also remove parked bytes - missing that strands bytes counted as
    // parked forever on every store->hit cycle. The cap-bypass path must not
    // subtract never-stored bytes (that drives the stat negative). And the
    // per-bin retention floor must be 1, not 2: 2 entries park up to 4 MB
    // of 2 MB spans per bin vs the documented 2 MB budget.
    constexpr std::size_t kSpan = 2u * 1024 * 1024 - 4096;  // -> 512-page span
    // Order matters: flush the thread caches FIRST, then empty the global
    // page cache - flushing afterwards would park spans after the purge and
    // pollute the parked-bytes baseline with residue from earlier tests.
    fast_alloc_purge_thread_cache();
    FastAllocTestPurgePageCache();

    // 1) store -> hit cycle: parked bytes must return to zero on the pop.
    {
        void* a = fast_malloc(kSpan);
        ASSERT_NE(a, nullptr);
        fast_free(a);
        fast_alloc_purge_thread_cache();  // span leaves TLS large cache
        EXPECT_GT(fast_alloc_stats().page_cache_bytes, 0u)
            << "freed 2 MB span never parked (setup failed)";
        void* b = fast_malloc(kSpan);      // exact-bin page-cache hit
        ASSERT_NE(b, nullptr);
        EXPECT_EQ(fast_alloc_stats().page_cache_bytes, 0u)
            << "hit path never subtracted parked bytes";
        fast_free(b);
        fast_alloc_purge_thread_cache();
        FastAllocTestPurgePageCache();
        EXPECT_EQ(fast_alloc_stats().page_cache_bytes, 0u);
    }

    // 2) cap-bypass: two same-size spans freed together - the bin holds one
    //    (2 MB budget), the second bypasses; the stat must neither underflow
    //    (never-stored bytes subtracted) nor exceed the per-bin budget.
    {
        void* x = fast_malloc(kSpan);
        void* y = fast_malloc(kSpan);
        ASSERT_NE(x, nullptr);
        ASSERT_NE(y, nullptr);
        fast_free(x);
        fast_free(y);
        fast_alloc_purge_thread_cache();
        EXPECT_LE(fast_alloc_stats().page_cache_bytes, 2u * 1024 * 1024)
            << "per-bin retention exceeded the documented 2 MB budget";
        FastAllocTestPurgePageCache();
        EXPECT_EQ(fast_alloc_stats().page_cache_bytes, 0u)
            << "parked-bytes stat drifted (bypass subtracted never-stored bytes)";
    }
}

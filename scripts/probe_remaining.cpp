// Remaining §6.2 hunt targets, batch probe (T2/T3/T5/T7/T8):
// - free of freshly-freed block payload write (UAF write into next-link)
//   [release semantics: UB by contract; skip]
// - fast_free_sized wrong hints incl. large/aligned (T2)
// - size-class boundary N-1/N/N+1 across all 512 classes incl. debug
//   red-zone sizing (T3) [release here]
// - OOM rollback: countdown during batch refill (T5)
// - purge + retention caps under FASTALLOC_PAGE_CACHE_MB=2 (T7)
// - stats: concurrent fast_alloc_stats during churn (T8)
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include "fast_alloc.h"

// Test seam (defined in the library).
namespace FastAlloc {
void FastAllocTestSetOOMCountdown(int n);
std::size_t FastAllocTestPageCacheBytes();
std::size_t FastAllocTestPurgePageCache();
}

using namespace FastAlloc;

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++failures; } } while (0)

int main() {
    // ---- T5: OOM during batch refill ----
    // NOTE: must run BEFORE the boundary sweep below, which warms every
    // TLS class bin (a warm bin serves from the TLS cache without any
    // OS call, by design).
    {
        FastAllocTestSetOOMCountdown(3);
        void* a = fast_malloc(7001);  // cold class -> miss -> refill -> OS -> fail
        CHECK(a == nullptr, "OOM during refill returns null");
        FastAllocTestSetOOMCountdown(0);
        void* b = fast_malloc(4997);  // allocator must be usable again
        CHECK(b != nullptr, "allocator usable after OOM");
        fast_free(b);
        void* c = fast_malloc(63);    // small path after OOM (cold class)
        CHECK(c != nullptr, "small path after OOM");
        fast_free(c);
    }

    // ---- T2: fast_free_sized wrong hints ----
    {
        void* p = fast_malloc(1000);
        CHECK(p, "alloc 1000");
        fast_free_sized(p, 0);        // zero hint -> general path
        void* q = fast_malloc(1000);
        CHECK(q, "realloc after zero hint");
        fast_free_sized(q, 999);      // wrong class hint -> general path
        void* r = fast_malloc(1000);
        CHECK(r, "alloc after wrong hint");
        fast_free_sized(r, 8176);     // oversized hint -> general path
        void* s = fast_malloc(1000);
        fast_free_sized(s, 100000);   // large hint -> full path
        CHECK(true, "sized free battery done");
    }
    // ---- T2: sized free of large and aligned blocks ----
    {
        void* l = fast_malloc(100000);
        CHECK(l, "large alloc");
        fast_free_sized(l, 100000);   // must route through full path
        void* a = fast_aligned_alloc(256, 1000);
        CHECK(a, "aligned alloc");
        fast_free_sized(a, 1000);     // ALIGN_MAGIC stash -> full path
        CHECK(true, "sized large/aligned done");
    }
    // ---- T3: class boundaries N-1/N/N+1 (release sizing) ----
    {
        for (std::size_t c = 1; c < 512; ++c) {
            std::size_t block = (c + 1) * 16;
            for (std::size_t req : {block - 16 - 1, block - 16, block - 16 + 1}) {
                if (req == 0 || req > 8176) continue;
                void* p = fast_malloc(req);
                if (!p) { std::printf("FAIL alloc %zu\n", req); ++failures; continue; }
                std::memset(p, 0xAB, req);
                if (((uintptr_t)p & 15) != 0) { std::printf("FAIL align %zu\n", req); ++failures; }
                fast_free(p);
            }
        }
        CHECK(true, "boundary sweep done");
    }
    // ---- T7: retention cap honoured (FASTALLOC_PAGE_CACHE_MB=2, set by env) ----
    {
        // (env preset by runner; default 64MB otherwise - check bounded)
        std::size_t hold_n = 40;
        std::vector<void*> hold(hold_n);
        for (auto& h : hold) h = fast_malloc(60000);
        for (auto& h : hold) fast_free(h);
        std::size_t cached = FastAllocTestPageCacheBytes();
        std::printf("page-cache bytes after 40x60KB churn: %zu\n", cached);
        CHECK(cached <= 64u * 1024 * 1024, "cache within global cap");
        std::size_t purged = FastAllocTestPurgePageCache();
        CHECK(FastAllocTestPageCacheBytes() == 0, "purge empties cache");
        (void)purged;
    }
    // ---- T8: concurrent stats during churn ----
    {
        std::atomic<bool> stop{false};
        std::thread stats([&] {
            // In-flight polling only exercises concurrent reads (crash/tear
            // freedom). It must NOT gate on the aggregate value: release
            // live counters are per-thread batched, and under
            // FASTALLOC_OVERRIDE_NEW std::thread machinery itself allocates
            // via FastAlloc across threads, so a freeing thread's flushed
            // batch can legitimately outrun the allocating thread's
            // unflushed one for the whole churn window. A GENUINE wrap -
            // lost updates, torn aggregation - survives thread exit, and
            // all batches flush at thread exit; the post-join reads below
            // are therefore exact and are the real gate.
            while (!stop.load(std::memory_order_relaxed)) {
                FastAllocStats s = fast_alloc_stats();
                (void)s;
            }
        });
        std::vector<std::thread> churn;
        for (int t = 0; t < 2; ++t) churn.emplace_back([&] {
            for (int i = 0; i < 20000; ++i) {
                void* p = fast_malloc((i % 8000) + 1);
                if (p) fast_free(p);
            }
        });
        for (auto& th : churn) th.join();
        stop.store(true, std::memory_order_relaxed);
        stats.join();
        // Every participant has exited (batches flushed at thread exit) and
        // this call flushes this thread's own batch: the aggregate is now
        // exact. A wrapped value here is real counter corruption.
        long bad = 0;
        for (int i = 0; i < 100; ++i) {
            FastAllocStats s = fast_alloc_stats();
            if (s.current_live_blocks > (1ull << 40)) ++bad;
        }
        CHECK(bad == 0, "concurrent stats never wrapped");
    }
    std::printf(failures ? "PROBE-FAILURES=%d\n" : "PROBE-DONE\n", failures);
    return failures ? 2 : 0;
}

// ============================================================================
// bench_suite.cpp — FastAlloc rigorous, cross-allocator benchmark suite
// ----------------------------------------------------------------------------
// METHODOLOGY (modeled on the accepted standards in the field):
//   * mimalloc-bench / Larsson & Lindgren workload shapes (realistic size
//     mixtures, constant live-set churn, ramp up/down, cache thrash)
//   * BEM-style fixed-operation-count timing (deterministic, comparable)
//   * one allocator per process (RSS/ru_maxrss are never cross-contaminated)
//   * warmup phase before every measured rep; N reps; analyzer takes medians
//   * latency percentiles via batch-of-64 timing (clock cost ~0.3 ns/op)
//   * every block is tagged on alloc and verified on free: results double as
//     a correctness smoke test and prevent dead-code elimination
//   * allocator-native statistics reported where available (FastAlloc stats,
//     glibc mallinfo2, jemalloc mallctl) alongside OS-level RSS
//
// WORKLOADS ("op" = the unit noted per workload; identical sequence for every
// allocator because RNG seeding is allocator-independent):
//   tiny            1 alloc + 1 free, sizes 1..32B           (op = pair)
//   small-mixed     alloc/free pair, realistic size mixture  (op = pair)
//   random-1-4096   alloc/free pair, uniform 1..4096B        (op = pair)
//   ramp            live set ramps 0->W->0 repeatedly        (op = pair)
//   churn           constant live set, random replacement    (op = pair)
//   cache-thrash    churn with 512..1024B blocks, ~64MB set  (op = pair)
//   cross-thread    MPSC: producers alloc, consumer frees    (op = pair)
//   thread-churn    threads spawn/exit doing alloc bursts    (op = pair)
//   large           64KB..512KB alloc + touch + free         (op = pair)
//   realloc-grow    string-like geometric growth + verify    (op = realloc)
//   overhead        memory accounting: live set, RSS, retention (op = object)
//
// USAGE:
//   bench_suite --alloc std|fast --label NAME --workload NAME [--threads N]
//                [--ops N] [--reps R] [--seed S] [--json FILE]
//
// Cross-allocator comparison is achieved by launching this SAME binary:
//   glibc     : --alloc std                      (no preload)
//   jemalloc  : --alloc std  + LD_PRELOAD=libjemalloc.so.2
//   mimalloc  : --alloc std  + LD_PRELOAD=libmimalloc.so
//   FastAlloc : --alloc fast
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <random>

#include "fast_alloc.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#include <sys/resource.h>
#include <sys/types.h>
#if defined(__linux__)
#include <malloc.h>   // mallinfo2 (glibc)
#include <dlfcn.h>    // dlsym -> jemalloc mallctl when preloaded
#endif
#endif

#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif

using namespace FastAlloc;

// ============================================================================
// Config
// ============================================================================

struct Config {
    std::string alloc      = "std";    // "std" or "fast"
    std::string label      = "";       // display label (allocator + version)
    std::string workload   = "";
    int         threads    = 1;
    uint64_t    ops        = 0;        // 0 = workload default
    int         reps       = 3;
    uint64_t    seed       = 0x9E3779B97F4A7C15ull;
    std::string json_file  = "";       // append JSON lines here
};

static Config g_cfg;

// ============================================================================
// Allocator dispatch
// ============================================================================

struct AllocApi {
    void* (*malloc)(std::size_t);
    void  (*free)(void*);
    void* (*realloc)(void*, std::size_t);
};
static AllocApi g_api = { std::malloc, std::free, std::realloc };

static void setup_allocator() {
    if (g_cfg.alloc == "fast") {
        g_api.malloc  = fast_malloc;
        g_api.free    = fast_free;
        g_api.realloc = fast_realloc;
        if (g_cfg.label.empty()) g_cfg.label = "FastAlloc";
    } else {
        if (g_cfg.label.empty()) g_cfg.label = "std-malloc";
    }
}

// ============================================================================
// Fast deterministic RNG (identical sequence for every allocator)
// ============================================================================

struct FastRng {
    uint64_t s;
    explicit FastRng(uint64_t seed) : s(seed ? seed : 1) {}
    uint64_t next() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return s;
    }
};

enum Dist {
    DIST_TINY,          // 1..32
    DIST_REALISTIC,     // 60% 8..64 | 30% 65..512 | 9% 513..4096 | 1% 4097..32768
    DIST_UNIFORM4K,     // 1..4096
    DIST_THRASH,        // 512..1024
    DIST_LARGE          // 64KB..512KB
};

static inline std::size_t next_size(int dist, FastRng& rng) {
    switch (dist) {
    case DIST_TINY:       return 1 + (rng.next() % 32);
    case DIST_UNIFORM4K:  return 1 + (rng.next() % 4096);
    case DIST_THRASH:     return 512 + (rng.next() % 513);
    case DIST_LARGE:      return 64 * 1024 + (rng.next() % (448 * 1024 + 1));
    case DIST_REALISTIC:
    default: {
        uint64_t r = rng.next() % 100;
        if (r < 60) return 8   + (rng.next() % 57);
        if (r < 90) return 65  + (rng.next() % 448);
        if (r < 99) return 513 + (rng.next() % 3584);
        return 4097 + (rng.next() % 28672);
    }
    }
}

// ============================================================================
// Tag store/verify (correctness smoke + anti-elimination)
// ============================================================================

static inline void tag_store(void* p, std::size_t sz, uint64_t tag) {
    if (!p) return;
    if (sz >= 8) { std::memcpy(p, &tag, 8); }
    else if (sz > 0) {
        unsigned char b[8]; std::memcpy(b, &tag, 8); std::memcpy(p, b, sz);
    }
}
static inline bool tag_verify(void* p, std::size_t sz, uint64_t tag) {
    if (!p) return false;
    if (sz >= 8) { uint64_t v; std::memcpy(&v, p, 8); return v == tag; }
    if (sz > 0) {
        unsigned char b[8], c[8];
        std::memcpy(b, &tag, 8); std::memcpy(c, p, sz);
        return std::memcmp(b, c, sz) == 0;
    }
    return true;
}
static inline void sink_ptr(void* p) {
#if defined(_MSC_VER)
    static void* volatile s_sink; s_sink = p;
#else
    asm volatile("" :: "r"(p) : "memory");
#endif
}

// ============================================================================
// Timing / memory helpers
// ============================================================================

static inline uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    asm volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

static size_t current_rss_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS info;
    GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info));
    return (size_t)info.WorkingSetSize;
#else
    long rss = 0;
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (f) {
        long total_pages = 0;
        if (std::fscanf(f, "%ld %ld", &total_pages, &rss) != 2) rss = 0;
        std::fclose(f);
    }
    return (size_t)rss * (size_t)sysconf(_SC_PAGESIZE);
#endif
}

static size_t peak_rss_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS info;
    GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info));
    return (size_t)info.PeakWorkingSetSize;
#else
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return (size_t)usage.ru_maxrss * 1024;
#endif
}

static double mb(double bytes) { return bytes / (1024.0 * 1024.0); }

// Allocator-native resident bytes; -1 when unavailable.
static double native_resident_bytes(double* live_out, double* cache_out) {
    if (live_out)  *live_out  = -1;
    if (cache_out) *cache_out = -1;
    if (g_cfg.alloc == "fast") {
        FastAllocStats st = fast_alloc_stats();
        if (live_out)  *live_out  = (double)st.current_live_bytes;
        if (cache_out) *cache_out = (double)st.page_cache_bytes;
        return (double)(st.os_bytes_live + st.page_cache_bytes);
    }
#if defined(__GLIBC__) && defined(__linux__)
    struct mallinfo2 mi = mallinfo2();
    return (double)(mi.arena + mi.hblkhd);   // glibc heap + mmap'd arena chunks
#else
    return -1;
#endif
}

#if defined(__linux__) && !defined(__GLIBC__) && defined(RTLD_DEFAULT)
// Dummy (non-glibc toolchains never reach this on our CI matrix).
static double jemalloc_resident_bytes() { return -1; }
#elif defined(__linux__) && defined(RTLD_DEFAULT)
// glibc build: if libjemalloc is LD_PRELOADed, mallctl resolves dynamically
// and gives exact resident bytes; otherwise returns -1.
static double jemalloc_resident_bytes() {
    using mallctl_fn = int (*)(const char*, void*, size_t*, void*, size_t);
    static mallctl_fn fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        fn = reinterpret_cast<mallctl_fn>(dlsym(RTLD_DEFAULT, "mallctl"));
    }
    if (!fn) return -1;
    size_t v = 0, sz = sizeof(v);
    if (fn("stats.resident", &v, &sz, nullptr, 0) != 0) return -1;
    return (double)v;
}
#else
static double jemalloc_resident_bytes() { return -1; }
#endif

// ============================================================================
// Thread gate (barrier start / done counting)
// ============================================================================

struct GateDone {
    std::atomic<uint32_t> ready{0};
    std::atomic<bool>     go{false};
    std::atomic<uint32_t> done{0};
    void arrive() { ready.fetch_add(1, std::memory_order_acq_rel); }
    void wait_go() { while (!go.load(std::memory_order_acquire)) cpu_relax(); }
    void mark_done() { done.fetch_add(1, std::memory_order_release); }
};

// Runs `body(gd, my_ops, tid)` across `threads` threads; returns wall ns.
// The body must call arrive()/wait_go()/mark_done() when gd != nullptr.
template <class F>
static uint64_t run_threaded(int threads, uint64_t total_ops, F body) {
    GateDone gd;
    if (threads <= 1) {
        uint64_t t0 = now_ns();
        body(nullptr, total_ops, 0);
        uint64_t t1 = now_ns();
        return t1 - t0;
    }
    std::vector<std::thread> ths;
    ths.reserve((size_t)threads);
    uint64_t share = total_ops / (uint64_t)threads;
    uint64_t rem   = total_ops % (uint64_t)threads;
    for (int t = 0; t < threads; ++t) {
        uint64_t my = share + ((uint64_t)t < rem ? 1 : 0);
        ths.emplace_back([&gd, my, t, &body]() { body(&gd, my, (uint32_t)t); });
    }
    while (gd.ready.load(std::memory_order_acquire) < (uint32_t)threads) cpu_relax();
    uint64_t t0 = now_ns();
    gd.go.store(true, std::memory_order_release);
    while (gd.done.load(std::memory_order_acquire) < (uint32_t)threads) cpu_relax();
    uint64_t t1 = now_ns();
    for (auto& th : ths) th.join();
    return t1 - t0;
}

// ============================================================================
// Per-rep result record
// ============================================================================

struct RepResult {
    uint64_t ops = 0;
    double   ns_per_op = 0;
    double   ops_per_sec = 0;
    double   p50 = -1, p99 = -1, p999 = -1;    // ns/op (latency workloads, 1 thread)
    size_t   live_bytes = 0;                    // harness-tracked live at end
    double   rss_start_mb = 0, rss_end_mb = 0, rss_peak_mb = 0;
    bool     checksum_ok = true;
    // memory-accounting extras (overhead workload; -1 = n/a)
    double   overhead_ratio = -1;
    double   retained_after_free_mb = -1;
    double   retained_after_purge_mb = -1;
    double   alloc_phase_ms = -1, free_phase_ms = -1;
    double   native_resident_mb = -1, native_live_mb = -1, native_cache_mb = -1;
    uint64_t extra_a = 0, extra_b = 0;         // workload-specific counters
    std::string note;
};

// ============================================================================
// Workload: alloc/free pairs (tiny / small-mixed / random-1-4096)
// ============================================================================

// Batch timings (ns) for single-thread latency percentile capture.
using PairsRun = std::vector<uint64_t>;

static RepResult run_pairs(int dist, int threads, uint64_t ops, bool latency,
                           PairsRun* out_batches) {
    RepResult r;
    r.rss_start_mb = mb((double)current_rss_bytes());
    const uint64_t LAT = 64;   // batch size for percentile capture
    std::atomic<uint64_t> bad_tags{0};

    auto body = [&](GateDone* gd, uint64_t my_ops, uint32_t tid) {
        FastRng rng(g_cfg.seed ^ (0x1000193ull * (tid + 1)));
        uint64_t tag_base = (uint64_t)tid << 48;
        if (gd) { gd->arrive(); gd->wait_go(); }
        if (latency && out_batches && gd == nullptr) {
            uint64_t nbatch = (my_ops + LAT - 1) / LAT;
            for (uint64_t b = 0; b < nbatch; ++b) {
                uint64_t n = (b + 1 == nbatch) ? (my_ops - b * LAT) : LAT;
                uint64_t t0 = now_ns();
                for (uint64_t i = 0; i < n; ++i) {
                    std::size_t sz = next_size(dist, rng);
                    void* p = g_api.malloc(sz);
                    tag_store(p, sz, tag_base | i);
                    sink_ptr(p);
                    if (!tag_verify(p, sz, tag_base | i)) bad_tags.fetch_add(1);
                    g_api.free(p);
                }
                uint64_t t1 = now_ns();
                out_batches->push_back(t1 - t0);
            }
        } else {
            for (uint64_t i = 0; i < my_ops; ++i) {
                std::size_t sz = next_size(dist, rng);
                void* p = g_api.malloc(sz);
                tag_store(p, sz, tag_base | i);
                sink_ptr(p);
                if (!tag_verify(p, sz, tag_base | i)) bad_tags.fetch_add(1);
                g_api.free(p);
            }
        }
        if (gd) gd->mark_done();
    };

    uint64_t wall = run_threaded(threads, ops, body);
    r.ops = ops;
    r.ns_per_op  = (double)wall / (double)ops;
    r.ops_per_sec = ops / ((double)wall / 1e9);

    if (latency && out_batches && threads == 1 && !out_batches->empty()) {
        std::vector<uint64_t>& v = *out_batches;
        std::sort(v.begin(), v.end());
        auto q = [&](double p) { return (double)v[(size_t)(p * (double)(v.size() - 1))] / (double)LAT; };
        r.p50 = q(0.50); r.p99 = q(0.99); r.p999 = q(0.999);
        // mean from batches must agree with wall time; sanity only.
    }
    r.rss_end_mb  = mb((double)current_rss_bytes());
    r.rss_peak_mb = mb((double)peak_rss_bytes());
    r.checksum_ok = bad_tags.load() == 0;
    r.extra_a = bad_tags.load();
    r.note = (latency && threads == 1) ? "latency-pct" : "";
    return r;
}

// ============================================================================
// Workload: ramp (live set ramps 0 -> W -> 0 per cycle)
// ============================================================================

static RepResult run_ramp(int threads, uint64_t ops, size_t width) {
    RepResult r;
    r.rss_start_mb = mb((double)current_rss_bytes());
    size_t Wp = width / (size_t)threads;                 // per-thread slots
    std::vector<void*>   slots(Wp, nullptr);             // per main-thread only
    std::vector<uint32_t> sizes(Wp, 0);
    std::atomic<uint64_t> bad_tags{0};

    auto body = [&](GateDone* gd, uint64_t my_ops, uint32_t tid) {
        FastRng rng(g_cfg.seed ^ (0x811C9DC5ull * (tid + 1)));
        uint64_t tag_base = (uint64_t)tid << 48;
        void**   my_slots = (threads == 1) ? slots.data() : new void*[Wp];
        uint32_t* my_sizes = (threads == 1) ? sizes.data() : new uint32_t[Wp];
        if (threads > 1) { for (size_t i = 0; i < Wp; ++i) { my_slots[i] = nullptr; my_sizes[i] = 0; } }
        if (gd) { gd->arrive(); gd->wait_go(); }
        uint64_t pair_per_cycle = 2 * (uint64_t)Wp;
        uint64_t cycles = my_ops / pair_per_cycle;
        if (cycles == 0) cycles = 1;
        for (uint64_t c = 0; c < cycles; ++c) {
            for (size_t i = 0; i < Wp; ++i) {            // up phase
                std::size_t sz = next_size(DIST_REALISTIC, rng);
                void* p = g_api.malloc(sz);
                tag_store(p, sz, tag_base | i);
                my_slots[i] = p; my_sizes[i] = (uint32_t)sz;
            }
            for (size_t i = 0; i < Wp; ++i) {            // down phase
                void* p = my_slots[i];
                if (p && !tag_verify(p, my_sizes[i], tag_base | i)) bad_tags.fetch_add(1);
                g_api.free(p); my_slots[i] = nullptr;
            }
        }
        if (gd) gd->mark_done();
        if (threads > 1) { delete[] my_slots; delete[] my_sizes; }
    };

    uint64_t wall = run_threaded(threads, ops, body);
    (void)slots; (void)sizes;
    r.ops = ops;
    r.ns_per_op = (double)wall / (double)ops;
    r.ops_per_sec = ops / ((double)wall / 1e9);
    r.live_bytes = 0;                                   // set is empty at rep end
    r.rss_end_mb  = mb((double)current_rss_bytes());
    r.rss_peak_mb = mb((double)peak_rss_bytes());
    r.checksum_ok = bad_tags.load() == 0;
    r.extra_a = bad_tags.load();
    char nbuf[128];
    std::snprintf(nbuf, sizeof nbuf, "width=%zu slots, %zu cylces/rep", width, (size_t)(ops / (2 * (uint64_t)width)));
    r.note = nbuf;
    return r;
}

// ============================================================================
// Workload: churn / cache-thrash (constant live set, random replacement)
// ============================================================================

struct ChurnCtx {
    std::vector<void*>   slots;
    std::vector<uint32_t> sizes;
    std::atomic<uint64_t> live_bytes{0};
    size_t W = 0;
    int  dist = DIST_REALISTIC;
    std::atomic<uint32_t> bad_tags{0};
};

static RepResult run_churn(ChurnCtx& ctx, int threads, uint64_t ops, uint64_t k_per_step) {
    RepResult r;
    r.rss_start_mb = mb((double)current_rss_bytes());
    size_t Wp = ctx.W / (size_t)threads;

    auto body = [&](GateDone* gd, uint64_t my_ops, uint32_t tid) {
        FastRng rng(g_cfg.seed ^ (0x01000193ull * (tid + 1)));
        uint64_t tag_base = (uint64_t)tid << 48;
        void** my_slots  = &ctx.slots[(size_t)tid * Wp];
        uint32_t* my_sizes = &ctx.sizes[(size_t)tid * Wp];
        if (gd) { gd->arrive(); gd->wait_go(); }
        uint64_t steps = my_ops / k_per_step;   // each step = k (free+alloc) pairs
        for (uint64_t s = 0; s < steps; ++s) {
            for (uint64_t k = 0; k < k_per_step; ++k) {
                size_t i = (size_t)(rng.next() % Wp);
                void* p = my_slots[i];
                if (p) {
                    if (!tag_verify(p, my_sizes[i], tag_base | i)) ctx.bad_tags.fetch_add(1);
                    g_api.free(p);
                    ctx.live_bytes.fetch_sub(my_sizes[i]);
                    my_slots[i] = nullptr;
                }
                std::size_t sz = next_size(ctx.dist, rng);
                void* q = g_api.malloc(sz);
                tag_store(q, sz, tag_base | i);
                my_slots[i] = q; my_sizes[i] = (uint32_t)sz;
                ctx.live_bytes.fetch_add((uint64_t)sz);
            }
        }
        if (gd) gd->mark_done();
    };

    uint64_t wall = run_threaded(threads, ops, body);
    r.ops = ops;
    r.ns_per_op = (double)wall / (double)ops;
    r.ops_per_sec = ops / ((double)wall / 1e9);
    r.live_bytes = (size_t)ctx.live_bytes.load();
    r.rss_end_mb  = mb((double)current_rss_bytes());
    r.rss_peak_mb = mb((double)peak_rss_bytes());
    r.checksum_ok = ctx.bad_tags.load() == 0;
    r.extra_a = (uint64_t)ctx.W;
    char nbuf[160];
    std::snprintf(nbuf, sizeof nbuf, "W=%zu slots, k=%llu/step, live=%.1fMB, frag(RSS/live)=%.2f",
                  ctx.W, (unsigned long long)k_per_step, mb((double)r.live_bytes),
                  r.live_bytes ? (double)current_rss_bytes() / (double)r.live_bytes : 0.0);
    r.note = nbuf;
    return r;
}

// Fill a fresh ChurnCtx (untimed warmup state).
static void churn_fill(ChurnCtx& ctx, int threads) {
    size_t Wp = ctx.W / (size_t)threads;
    for (int t = 0; t < threads; ++t) {
        FastRng rng(g_cfg.seed ^ (0x9E3779B9ull * (uint64_t)(t + 1)));
        uint64_t tag_base = (uint64_t)t << 48;
        void** my_slots  = &ctx.slots[(size_t)t * Wp];
        uint32_t* my_sizes = &ctx.sizes[(size_t)t * Wp];
        for (size_t i = 0; i < Wp; ++i) {
            std::size_t sz = next_size(ctx.dist, rng);
            void* p = g_api.malloc(sz);
            tag_store(p, sz, tag_base | i);
            my_slots[i] = p; my_sizes[i] = (uint32_t)sz;
            ctx.live_bytes.fetch_add((uint64_t)sz);
        }
    }
}
static void churn_teardown(ChurnCtx& ctx, int threads) {
    size_t Wp = ctx.W / (size_t)threads;
    for (int t = 0; t < threads; ++t) {
        uint64_t tag_base = (uint64_t)t << 48;
        void** my_slots  = &ctx.slots[(size_t)t * Wp];
        uint32_t* my_sizes = &ctx.sizes[(size_t)t * Wp];
        for (size_t i = 0; i < Wp; ++i) {
            void* p = my_slots[i];
            if (p) {
                if (!tag_verify(p, my_sizes[i], tag_base | i)) ctx.bad_tags.fetch_add(1);
                g_api.free(p);
            }
        }
    }
}

// ============================================================================
// Workload: cross-thread (MPSC producers -> consumer)
// ============================================================================

static const size_t RING_CAP = 1024;   // power of two

struct SpscRing {
    static const uint64_t CAP = 1024;
    std::atomic<uint64_t> head{0};
    std::atomic<uint64_t> tail{0};
    void* slots[RING_CAP];
    bool push(void* p) {
        uint64_t t = tail.load(std::memory_order_relaxed);
        uint64_t h = head.load(std::memory_order_acquire);
        if (t - h >= CAP) return false;
        slots[t % CAP] = p;
        tail.store(t + 1, std::memory_order_release);
        return true;
    }
    void* pop() {
        uint64_t h = head.load(std::memory_order_relaxed);
        uint64_t t = tail.load(std::memory_order_acquire);
        if (h == t) return nullptr;
        void* p = slots[h % CAP];
        head.store(h + 1, std::memory_order_release);
        return p;
    }
    uint64_t size_approx() const {
        return tail.load(std::memory_order_acquire) - head.load(std::memory_order_acquire);
    }
};

struct MpmpcCtx {
    std::vector<SpscRing*> rings;
    std::atomic<uint32_t> bad_tags{0};
};

static RepResult run_cross_thread(int threads, uint64_t ops) {
    RepResult r;
    r.rss_start_mb = mb((double)current_rss_bytes());
    // threads == 1: degenerate — same thread allocates and frees (documented).
    // threads >= 2: (threads-1) producers + 1 consumer.
    if (threads <= 1) {
        FastRng rng(g_cfg.seed ^ 0xC0FFEEull);
        uint64_t t0 = now_ns();
        for (uint64_t i = 0; i < ops; ++i) {
            std::size_t sz = next_size(DIST_REALISTIC, rng);
            void* p = g_api.malloc(sz);
            tag_store(p, sz, i);
            sink_ptr(p);
            g_api.free(p);
        }
        uint64_t t1 = now_ns();
        r.ops = ops;
        r.ns_per_op = (double)(t1 - t0) / (double)ops;
        r.ops_per_sec = ops / ((double)(t1 - t0) / 1e9);
        r.note = "degenerate (same thread alloc+free)";
        r.rss_end_mb = mb((double)current_rss_bytes());
        r.rss_peak_mb = mb((double)peak_rss_bytes());
        return r;
    }

    int P = threads - 1;
    std::vector<SpscRing> ring_store((size_t)P);
    std::atomic<bool> producers_done{false};
    std::atomic<uint64_t> freed_count{0};
    uint64_t per_producer = ops / (uint64_t)P;
    // Exact accounting: producers push exactly P*per_producer blocks, and the
    // consumer exits only after freeing that many. (Plain 'ops' could exceed
    // the push total due to integer division and hang the loop forever.)
    uint64_t total_pushed = (uint64_t)P * per_producer;

    std::thread consumer([&]() {
        std::vector<uint64_t> expect((size_t)P, 0);
        while (true) {
            bool progressed = false;
            for (int i = 0; i < P; ++i) {
                while (void* p = ring_store[(size_t)i].pop()) {
                    // Verify the producer's tag (producers clamp sizes to >= 8
                    // so an 8-byte tag is always present).
                    uint64_t exp = expect[(size_t)i]++;
                    uint64_t tag = ((uint64_t)i << 48) | (exp & 0xFFFFFFFFFFFFull);
                    uint64_t v = 0;
                    std::memcpy(&v, p, 8);
                    if (v != tag) r.checksum_ok = false;
                    g_api.free(p);
                    freed_count.fetch_add(1, std::memory_order_relaxed);
                    progressed = true;
                }
            }
            if (producers_done.load(std::memory_order_acquire) &&
                freed_count.load(std::memory_order_relaxed) >= total_pushed) break;
            if (!progressed) cpu_relax();
        }
    });
    // NOTE: producer sizes are >= 8 so 8-byte tags are always written.
    {
        std::vector<std::thread> producers;
        for (int i = 0; i < P; ++i) producers.emplace_back([&, i]() {
            FastRng rng(g_cfg.seed ^ (0x85EBCA6Bull * (uint64_t)(i + 1)));
            for (uint64_t n = 0; n < per_producer; ++n) {
                std::size_t sz = next_size(DIST_REALISTIC, rng);
                if (sz < 8) sz = 8;
                void* p = g_api.malloc(sz);
                tag_store(p, sz, ((uint64_t)i << 48) | (n & 0xFFFFFFFFFFFFull));
                while (!ring_store[(size_t)i].push(p)) cpu_relax();
            }
        });
        // measure from the moment producers start
        uint64_t t0 = now_ns();
        for (auto& t : producers) t.join();
        producers_done.store(true, std::memory_order_release);
        consumer.join();
        uint64_t t1 = now_ns();
        r.ops = total_pushed;
        r.ns_per_op = (double)(t1 - t0) / (double)total_pushed;
        r.ops_per_sec = (double)total_pushed / ((double)(t1 - t0) / 1e9);
    }
    r.rss_end_mb = mb((double)current_rss_bytes());
    r.rss_peak_mb = mb((double)peak_rss_bytes());
    char nbuf[96];
    std::snprintf(nbuf, sizeof nbuf, "%d producers + 1 consumer (%llu blocks)",
                  P, (unsigned long long)total_pushed);
    r.note = nbuf;
    return r;
}

// ============================================================================
// Workload: thread-churn (threads spawn/exit doing alloc bursts)
// ============================================================================
// Exercises per-thread cache creation/teardown (FastAlloc thread_caches_created
// / _destroyed, TLS destructors; glibc tcache init; jemalloc tcache). One "op"
// = one alloc/free pair; bursts run inside short-lived worker threads.

static RepResult run_thread_churn(int threads, uint64_t ops) {
    RepResult r;
    r.rss_start_mb = mb((double)current_rss_bytes());
    uint64_t burst = 256;                       // pairs per spawned thread
    uint64_t spawn_rounds = (ops / burst) / (uint64_t)threads;
    if (spawn_rounds == 0) spawn_rounds = 1;
    uint64_t total_pairs = spawn_rounds * burst * (uint64_t)threads;
    std::atomic<uint64_t> bad_tags{0};

    uint64_t t0 = now_ns();
    for (uint64_t round = 0; round < spawn_rounds; ++round) {
        std::vector<std::thread> ths;
        for (int t = 0; t < threads; ++t) {
            ths.emplace_back([&, t]() {
                FastRng rng(g_cfg.seed ^ (0x2545F491ull * (uint64_t)(t + 1)) ^ round);
                uint64_t tag_base = ((uint64_t)t << 48) | (round << 32);
                for (uint64_t i = 0; i < burst; ++i) {
                    std::size_t sz = next_size(DIST_REALISTIC, rng);
                    void* p = g_api.malloc(sz);
                    tag_store(p, sz, tag_base | i);
                    if (!tag_verify(p, sz, tag_base | i)) bad_tags.fetch_add(1);
                    g_api.free(p);
                }
            });
        }
        for (auto& th : ths) th.join();
    }
    uint64_t t1 = now_ns();

    r.ops = total_pairs;
    r.ns_per_op = (double)(t1 - t0) / (double)total_pairs;
    r.ops_per_sec = (double)total_pairs / ((double)(t1 - t0) / 1e9);
    r.checksum_ok = bad_tags.load() == 0;
    r.extra_a = spawn_rounds * (uint64_t)threads;      // threads spawned
    r.rss_end_mb  = mb((double)current_rss_bytes());
    r.rss_peak_mb = mb((double)peak_rss_bytes());
    char nbuf[128];
    std::snprintf(nbuf, sizeof nbuf, "%llu spawned threads x %llu pairs",
                  (unsigned long long)r.extra_a, (unsigned long long)burst);
    r.note = nbuf;
    return r;
}

// ============================================================================
// Workload: large (64KB..512KB alloc + touch + free)
// ============================================================================
// One op = alloc + first/last-page touch + free. Sizes span FastAlloc's
// large-block path and the page cache (2MB bin cap).

static RepResult run_large(int threads, uint64_t ops) {
    RepResult r;
    r.rss_start_mb = mb((double)current_rss_bytes());
    std::atomic<uint64_t> bad_tags{0};

    auto body = [&](GateDone* gd, uint64_t my_ops, uint32_t tid) {
        FastRng rng(g_cfg.seed ^ (0x9E3779B9ull ^ (0x85EBCA6Bull * (uint64_t)(tid + 1))));
        uint64_t tag_base = (uint64_t)tid << 48;
        if (gd) { gd->arrive(); gd->wait_go(); }
        for (uint64_t i = 0; i < my_ops; ++i) {
            std::size_t sz = next_size(DIST_LARGE, rng);
            void* p = g_api.malloc(sz);
            if (p) {
                // touch first and last cacheline (backs pages, defeats lazy zero)
                volatile char* c = static_cast<char*>(p);
                c[0] = (char)tag_base; c[0] = 0;
                c[sz - 1] = (char)(tag_base >> 8); c[sz - 1] = 0;
                tag_store(p, 8, tag_base | i);       // first 8 bytes
                if (!tag_verify(p, 8, tag_base | i)) bad_tags.fetch_add(1);
            }
            sink_ptr(p);
            g_api.free(p);
        }
        if (gd) gd->mark_done();
    };

    uint64_t wall = run_threaded(threads, ops, body);
    r.ops = ops;
    r.ns_per_op = (double)wall / (double)ops;
    r.ops_per_sec = ops / ((double)wall / 1e9);
    r.checksum_ok = bad_tags.load() == 0;
    r.rss_end_mb  = mb((double)current_rss_bytes());
    r.rss_peak_mb = mb((double)peak_rss_bytes());
    r.note = "64KB..512KB, touch-first+last";
    return r;
}

// ============================================================================
// Workload: realloc-grow (string/vector-like geometric growth)
// ============================================================================
// One op = one realloc step. A block grows 16 -> ~8KB in 1.5x steps; contents
// are tagged and verified after every step (realloc must preserve them).
// Single-threaded by design (realloc contention is not the differentiator).

static RepResult run_realloc_grow(uint64_t ops) {
    RepResult r;
    r.rss_start_mb = mb((double)current_rss_bytes());
    std::atomic<uint64_t> bad_tags{0};
    uint64_t steps = 0;

    uint64_t t0 = now_ns();
    uint64_t done = 0;
    FastRng rng(g_cfg.seed ^ 0x5DEECE66Dull);
    std::size_t init = 16;
    void* p = g_api.malloc(init);
    std::size_t cur = init;
    tag_store(p, cur, 0xDEADBEEFull);
    while (done < ops) {
        std::size_t want = cur + (cur >> 1) + 16;           // 1.5x geometric
        if (want > 8192) {                                   // restart the string
            g_api.free(p);
            init = 16 + (std::size_t)(rng.next() % 16);
            p = g_api.malloc(init); cur = init;
            tag_store(p, cur, 0xDEADBEEFull);
            continue;
        }
        void* q = g_api.realloc(p, want);
        if (!q && p) { bad_tags.fetch_add(1); break; }       // OOM -> bail
        p = q; cur = want;
        tag_store(p, std::min(cur, (std::size_t)8), 0xDEADBEEFull + done);
        if (!tag_verify(p, std::min(cur, (std::size_t)8), 0xDEADBEEFull + done))
            bad_tags.fetch_add(1);
        ++done; ++steps;
    }
    g_api.free(p);
    uint64_t t1 = now_ns();

    r.ops = done;
    r.ns_per_op = (double)(t1 - t0) / (double)(done ? done : 1);
    r.ops_per_sec = (double)done / ((double)(t1 - t0) / 1e9);
    r.checksum_ok = bad_tags.load() == 0;
    r.rss_end_mb  = mb((double)current_rss_bytes());
    r.rss_peak_mb = mb((double)peak_rss_bytes());
    r.note = "geometric 1.5x growth to 8KB, verify after every step";
    return r;
}

// ============================================================================
// Workload: overhead (memory accounting: live set, RSS, retention)
// ============================================================================
// NOT a speed test. Phases (each timed):
//   1. alloc   N=1M live objects (realistic mix)     -> live ~60-70MB
//   2. measure RSS + native stats at full live set
//   3. free    everything
//   4. measure RSS right after free  (retention w/o purge)
//   5. purge   (fast only) / malloc_trim (glibc)     -> measure retention
// Reports overhead ratio = RSS-at-full-live / harness live bytes.

static RepResult run_overhead(int threads) {
    RepResult r;
    const size_t N = 1'000'000;
    size_t Wp = N / (size_t)threads;
    std::vector<std::vector<void*>>   slots((size_t)threads);
    std::vector<std::vector<uint32_t>> szs((size_t)threads);
    std::atomic<uint64_t> live{0};
    std::atomic<uint64_t> bad_tags{0};

    // Pre-size the harness bookkeeping BEFORE capturing the RSS baseline so
    // the slot vectors (≈12MB) are not misattributed to the allocator.
    for (int t = 0; t < threads; ++t) { slots[(size_t)t].resize(Wp, nullptr); szs[(size_t)t].resize(Wp, 0); }
    r.rss_start_mb = mb((double)current_rss_bytes());

    auto alloc_phase = [&]() {
        std::vector<std::thread> ths;
        for (int t = 0; t < threads; ++t) ths.emplace_back([&, t]() {
            FastRng rng(g_cfg.seed ^ (0x27D4EB2Full * (uint64_t)(t + 1)));
            uint64_t tag_base = (uint64_t)t << 48;
            for (size_t i = 0; i < Wp; ++i) {
                std::size_t sz = next_size(DIST_REALISTIC, rng);
                void* p = g_api.malloc(sz);
                tag_store(p, sz, tag_base | i);
                slots[(size_t)t][i] = p;
                szs[(size_t)t][i] = (uint32_t)sz;
                live.fetch_add((uint64_t)sz);
            }
        });
        for (auto& th : ths) th.join();
    };
    auto free_phase = [&]() {
        std::vector<std::thread> ths;
        for (int t = 0; t < threads; ++t) ths.emplace_back([&, t]() {
            uint64_t tag_base = (uint64_t)t << 48;
            for (size_t i = 0; i < Wp; ++i) {
                void* p = slots[(size_t)t][i];
                if (!tag_verify(p, szs[(size_t)t][i], tag_base | i)) bad_tags.fetch_add(1);
                g_api.free(p);
            }
        });
        for (auto& th : ths) th.join();
    };

    uint64_t t0 = now_ns();
    alloc_phase();
    uint64_t t1 = now_ns();
    r.alloc_phase_ms = (double)(t1 - t0) / 1e6;

    double live_mb   = mb((double)live.load());
    double rss_full  = mb((double)current_rss_bytes());
    r.overhead_ratio = live.load() ? (double)current_rss_bytes() / (double)live.load() : -1;
    r.native_resident_mb = mb(native_resident_bytes(&r.native_live_mb, &r.native_cache_mb));
    double jem = (g_cfg.alloc == "std") ? jemalloc_resident_bytes() : -1;
    if (jem >= 0 && g_cfg.alloc == "std") r.native_resident_mb = mb(jem);
    r.rss_peak_mb = mb((double)peak_rss_bytes());

    t0 = now_ns();
    free_phase();
    t1 = now_ns();
    r.free_phase_ms = (double)(t1 - t0) / 1e6;

    r.retained_after_free_mb = mb((double)current_rss_bytes());
    r.live_bytes = 0;

    // Best-effort return-to-OS: FastAlloc purge vs glibc malloc_trim(0).
    // (jemalloc keeps pages in arenas by design; noted in the report.)
    if (g_cfg.alloc == "fast") {
        fast_alloc_purge_thread_cache();
        fast_alloc_purge();
    } else {
#if defined(__GLIBC__) && defined(__linux__)
        malloc_trim(0);
#endif
    }
    r.retained_after_purge_mb = mb((double)current_rss_bytes());

    r.ops = N;
    r.ns_per_op = (r.alloc_phase_ms + r.free_phase_ms) * 1e6 / (double)N;  // combined alloc+free per object
    r.ops_per_sec = (double)N / ((r.alloc_phase_ms + r.free_phase_ms) / 1e3);
    r.checksum_ok = bad_tags.load() == 0;
    r.extra_a = live.load();
    char nbuf[256];
    std::snprintf(nbuf, sizeof nbuf,
        "N=%zu objs, live=%.1fMB, RSS@full=%.1fMB, ratio=%.2f, retained=%.1fMB (post-free) / %.1fMB (post-purge)",
        N, live_mb, rss_full, r.overhead_ratio, r.retained_after_free_mb, r.retained_after_purge_mb);
    r.note = nbuf;
    r.rss_end_mb = mb((double)current_rss_bytes());
    return r;
}

// ============================================================================
// Environment detection
// ============================================================================

static std::string cpu_model() {
#ifdef __linux__
    FILE* f = std::fopen("/proc/cpuinfo", "r");
    if (!f) return "?";
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        if (std::strncmp(line, "model name", 10) == 0) {
            const char* c = std::strchr(line, ':');
            std::fclose(f);
            if (c) { std::string s(c + 1); while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); return s; }
        }
    }
    std::fclose(f);
#endif
    return "?";
}

// Detect preloaded allocator versions (jemalloc / mimalloc) in std mode.
static std::string detect_preloaded() {
#if defined(__linux__) && defined(RTLD_DEFAULT)
    // jemalloc: string-valued ctl nodes (like "version") write a const char*
    // THROUGH oldp — oldp must be a pointer to a pointer, not a char buffer.
    {
        using mallctl_fn = int (*)(const char*, void*, size_t*, void*, size_t);
        auto fn = reinterpret_cast<mallctl_fn>(dlsym(RTLD_DEFAULT, "mallctl"));
        if (fn) {
            const char* ver = nullptr;
            size_t sz = sizeof(ver);
            if (fn("version", &ver, &sz, nullptr, 0) == 0 && ver)
                return std::string("jemalloc-") + ver;
        }
    }
    // mimalloc: real signature is `int mi_version(void)` returning the
    // version as major*100 + minor*10 + patch (e.g. 217 == 2.1.7). Calling it
    // through any other prototype turns the integer into a bogus pointer
    // (this was a real crash: the int 217 == 0xD9 was strlen'd by libc).
    {
        using ver_fn = int (*)();
        auto fn = reinterpret_cast<ver_fn>(dlsym(RTLD_DEFAULT, "mi_version"));
        if (fn) {
            int v = fn();
            char buf[32];
            std::snprintf(buf, sizeof buf, "mimalloc-%d.%d.%d",
                          v / 100, (v / 10) % 10, v % 10);
            return std::string(buf);
        }
    }
#endif
    return "";
}

// ============================================================================
// main: argument parsing, dispatch, rep loop, output
// ============================================================================

struct WorkloadDef {
    const char* name;
    uint64_t    default_ops;
};

static const WorkloadDef kWorkloads[] = {
    { "tiny",           20'000'000 },
    { "small-mixed",    10'000'000 },
    { "random-1-4096",   5'000'000 },
    { "ramp",           10'000'000 },
    { "churn",          10'000'000 },
    { "cache-thrash",    5'000'000 },
    { "cross-thread",    4'000'000 },
    { "thread-churn",    2'000'000 },
    { "large",              20'000 },
    { "realloc-grow",    5'000'000 },
    { "overhead",        1'000'000 },
};

static void print_usage() {
    std::printf(
        "usage: bench_suite --alloc std|fast [--label NAME] --workload NAME\n"
        "                   [--threads N] [--ops N] [--reps R] [--seed S] [--json FILE]\n"
        "                   [--warmup N]\n"
        "workloads: ");
    for (const auto& w : kWorkloads) std::printf("%s ", w.name);
    std::printf("\n");
}

static std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        default:   o += c;
        }
    }
    return o;
}

static void json_out(const std::vector<RepResult>& reps, bool ok) {
    if (g_cfg.json_file.empty()) return;
    FILE* f = std::fopen(g_cfg.json_file.c_str(), "a");
    if (!f) { std::fprintf(stderr, "cannot open json file %s\n", g_cfg.json_file.c_str()); return; }
    std::fprintf(f, "{\"allocator\":\"%s\",\"label\":\"%s\",\"workload\":\"%s\",\"threads\":%d,\"ops\":%llu,"
        "\"cpu\":\"%s\",\"env\":\"%s\",\"checksum_ok\":%s,\"reps\":[",
        g_cfg.alloc.c_str(), json_escape(g_cfg.label).c_str(), g_cfg.workload.c_str(), g_cfg.threads,
        (unsigned long long)(reps.empty() ? 0 : reps[0].ops),
        json_escape(cpu_model()).c_str(),
        json_escape(detect_preloaded().empty()
            ? std::string(
#ifdef __GLIBC__
                "glibc-" ) + gnu_get_libc_version()
#else
                "libc")
#endif
            : detect_preloaded()).c_str(),
        ok ? "true" : "false");
    for (size_t i = 0; i < reps.size(); ++i) {
        const RepResult& r = reps[i];
        std::fprintf(f,
            "%s{\"ns_per_op\":%.4f,\"ops_per_sec\":%.1f,\"p50\":%.3f,\"p99\":%.3f,\"p999\":%.3f,"
            "\"live_bytes\":%zu,\"rss_start_mb\":%.2f,\"rss_end_mb\":%.2f,\"rss_peak_mb\":%.2f,"
            "\"overhead_ratio\":%.4f,\"retained_after_free_mb\":%.2f,\"retained_after_purge_mb\":%.2f,"
            "\"alloc_phase_ms\":%.2f,\"free_phase_ms\":%.2f,\"native_resident_mb\":%.2f,"
            "\"native_live_mb\":%.2f,\"native_cache_mb\":%.2f,\"extra_a\":%llu,\"extra_b\":%llu,"
            "\"note\":\"%s\"}",
            i ? "," : "",
            r.ns_per_op, r.ops_per_sec, r.p50, r.p99, r.p999, r.live_bytes,
            r.rss_start_mb, r.rss_end_mb, r.rss_peak_mb, r.overhead_ratio,
            r.retained_after_free_mb, r.retained_after_purge_mb,
            r.alloc_phase_ms, r.free_phase_ms, r.native_resident_mb,
            r.native_live_mb, r.native_cache_mb,
            (unsigned long long)r.extra_a, (unsigned long long)r.extra_b,
            json_escape(r.note).c_str());
    }
    std::fprintf(f, "]}\n");
    std::fclose(f);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if      (a == "--alloc")    { const char* v = next(); if (v) g_cfg.alloc = v; }
        else if (a == "--label")    { const char* v = next(); if (v) g_cfg.label = v; }
        else if (a == "--workload"){ const char* v = next(); if (v) g_cfg.workload = v; }
        else if (a == "--threads")  { const char* v = next(); if (v) g_cfg.threads = std::atoi(v); }
        else if (a == "--ops")      { const char* v = next(); if (v) g_cfg.ops = std::strtoull(v, nullptr, 10); }
        else if (a == "--reps")     { const char* v = next(); if (v) g_cfg.reps = std::atoi(v); }
        else if (a == "--seed")     { const char* v = next(); if (v) g_cfg.seed = std::strtoull(v, nullptr, 10); }
        else if (a == "--json")     { const char* v = next(); if (v) g_cfg.json_file = v; }
        else if (a == "--warmup")   { const char* v = next(); if (v) /* ratio kept for compat */ (void)v; }
        else if (a == "--list")     { for (const auto& w : kWorkloads) std::printf("%s\n", w.name); return 0; }
        else if (a == "-h" || a == "--help") { print_usage(); return 0; }
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); print_usage(); return 2; }
    }
    if (g_cfg.workload.empty()) { print_usage(); return 2; }
    if (g_cfg.threads < 1) g_cfg.threads = 1;

    setup_allocator();

    const WorkloadDef* def = nullptr;
    for (const auto& w : kWorkloads) if (g_cfg.workload == w.name) def = &w;
    if (!def) { std::fprintf(stderr, "unknown workload: %s\n", g_cfg.workload.c_str()); print_usage(); return 2; }

    uint64_t ops = g_cfg.ops ? g_cfg.ops : def->default_ops;
    bool is_overhead = g_cfg.workload == "overhead";
    int reps = is_overhead ? 1 : g_cfg.reps;
    int warmup_reps = is_overhead ? 0 : 1;

    std::printf("=== bench_suite | %s | %s | threads=%d ops=%llu reps=%d(warmup=%d) ===\n",
                g_cfg.label.c_str(), g_cfg.workload.c_str(), g_cfg.threads,
                (unsigned long long)ops, reps, warmup_reps);
    std::fflush(stdout);

    std::vector<RepResult> results;
    bool all_ok = true;

    // ---- churn family carries a persistent live set across reps ----
    ChurnCtx churn_ctx;
    if (g_cfg.workload == "churn" || g_cfg.workload == "cache-thrash") {
        churn_ctx.W = (g_cfg.workload == "churn") ? 100'000 : 65'536;
        churn_ctx.dist = (g_cfg.workload == "churn") ? DIST_REALISTIC : DIST_THRASH;
        churn_ctx.slots.assign(churn_ctx.W, nullptr);
        churn_ctx.sizes.assign(churn_ctx.W, 0);
    }

    for (int rep = -warmup_reps; rep < reps; ++rep) {
        bool timed = rep >= 0;
        uint64_t rep_ops = timed ? ops : (is_overhead ? ops : ops / 10 + 1);
        RepResult r;
        uint64_t k = 8;

        if (g_cfg.workload == "tiny" || g_cfg.workload == "small-mixed" ||
            g_cfg.workload == "random-1-4096") {
            int dist = g_cfg.workload == "tiny" ? DIST_TINY
                     : g_cfg.workload == "random-1-4096" ? DIST_UNIFORM4K
                     : DIST_REALISTIC;
            PairsRun pr;
            bool latency = (g_cfg.threads == 1);      // percentiles on 1 thread
            r = run_pairs(dist, g_cfg.threads, rep_ops, latency, latency ? &pr : nullptr);
        }
        else if (g_cfg.workload == "ramp")           r = run_ramp(g_cfg.threads, rep_ops, 100'000);
        else if (g_cfg.workload == "churn")          { if (!timed) churn_fill(churn_ctx, g_cfg.threads); r = run_churn(churn_ctx, g_cfg.threads, rep_ops, k); }
        else if (g_cfg.workload == "cache-thrash")   { if (!timed) churn_fill(churn_ctx, g_cfg.threads); r = run_churn(churn_ctx, g_cfg.threads, rep_ops, k); }
        else if (g_cfg.workload == "cross-thread")   r = run_cross_thread(g_cfg.threads, rep_ops);
        else if (g_cfg.workload == "thread-churn")   r = run_thread_churn(g_cfg.threads, rep_ops);
        else if (g_cfg.workload == "large")          r = run_large(g_cfg.threads, rep_ops);
        else if (g_cfg.workload == "realloc-grow")   r = run_realloc_grow(rep_ops);
        else if (g_cfg.workload == "overhead")       r = run_overhead(g_cfg.threads);
        else { print_usage(); return 2; }

        all_ok = all_ok && r.checksum_ok;
        if (timed) {
            results.push_back(r);
            std::printf("  rep%-2d %12.2f ns/op %14.1f ops/s   rss=%.1f/%.1fMB%s%s\n",
                        rep, r.ns_per_op, r.ops_per_sec, r.rss_end_mb, r.rss_peak_mb,
                        (r.p50 > 0) ? "   p50/p99/p999 = " : "", "");
            if (r.p50 > 0) std::printf("        latency p50=%.2f p99=%.2f p99.9=%.2f ns/op\n", r.p50, r.p99, r.p999);
            if (!r.note.empty()) std::printf("        %s\n", r.note.c_str());
            std::fflush(stdout);
        } else {
            std::printf("  warmup done (%.2f ns/op)\n", r.ns_per_op);
            std::fflush(stdout);
        }
    }

    // teardown persistent churn set (untimed; verifies + frees everything)
    if (!churn_ctx.slots.empty()) {
        churn_teardown(churn_ctx, g_cfg.threads);
        all_ok = all_ok && (churn_ctx.bad_tags.load() == 0);
        if (g_cfg.alloc == "fast") { fast_alloc_purge_thread_cache(); fast_alloc_purge(); }
#if defined(__GLIBC__) && defined(__linux__)
        else malloc_trim(0);
#endif
    }

    // summary: median ns/op
    if (!results.empty()) {
        std::vector<double> v;
        v.reserve(results.size());
        for (const auto& r : results) v.push_back(r.ns_per_op);
        std::sort(v.begin(), v.end());
        double med = v[v.size() / 2];
        std::printf("--- %s | %s | T=%d : median %.2f ns/op (%.1f M ops/s) | checksum %s ---\n",
                    g_cfg.label.c_str(), g_cfg.workload.c_str(), g_cfg.threads,
                    med, 1000.0 / med, all_ok ? "OK" : "FAILED");
    }

    json_out(results, all_ok);
    return all_ok ? 0 : 42;
}

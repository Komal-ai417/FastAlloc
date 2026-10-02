// ============================================================================
// FastAlloc audit benchmark (item 17/18/20/23/25): latency percentiles,
// throughput scaling, realistic workloads, warm/cold, 20 reps + CoV + 95% CI.
// Two build variants share this source:
//   default:        FastAlloc explicit API
//   -DUSE_GLIBC:    maps to glibc malloc/free/realloc/calloc/aligned_alloc
// ============================================================================
#include "fast_alloc.h"
#include "fast_alloc_config.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#ifdef USE_GLIBC
namespace fa {
inline void* malloc(std::size_t n) { return std::malloc(n); }
inline void free(void* p) { std::free(p); }
inline void* realloc(void* p, std::size_t n) { return std::realloc(p, n); }
inline void* calloc(std::size_t a, std::size_t b) { return std::calloc(a, b); }
inline void* aligned_alloc(std::size_t a, std::size_t n) {
    if (n == 0) n = 1;
    return std::aligned_alloc(a, n);
}
inline void purge() {}
const char* name() { return "glibc"; }
}  // namespace fa
#else
namespace fa {
inline void* malloc(std::size_t n) { return FastAlloc::fast_malloc(n); }
inline void free(void* p) { return FastAlloc::fast_free(p); }
inline void* realloc(void* p, std::size_t n) { return FastAlloc::fast_realloc(p, n); }
inline void* calloc(std::size_t a, std::size_t b) { return FastAlloc::fast_calloc(a, b); }
inline void* aligned_alloc(std::size_t a, std::size_t n) {
    return FastAlloc::fast_aligned_alloc(a, n);
}
inline void purge() { FastAlloc::fast_alloc_purge(); }
const char* name() { return "FastAlloc"; }
}  // namespace fa
#endif

using clk = std::chrono::steady_clock;
static double NowNs() {
    return std::chrono::duration<double, std::nano>(clk::now().time_since_epoch()).count();
}

// ----------------------------------------------------------------------------
struct RepStats { double mean; double cov; double ci95; double min; double max; };

static RepStats Summarize(std::vector<double>& xs) {
    std::sort(xs.begin(), xs.end());
    RepStats r{};
    double s = 0, s2 = 0;
    for (double x : xs) { s += x; s2 += x * x; }
    double n = static_cast<double>(xs.size());
    r.mean = s / n;
    double var = n > 1 ? (s2 - n * r.mean * r.mean) / (n - 1) : 0;
    double sd = var > 0 ? std::sqrt(var) : 0;
    r.cov = r.mean > 0 ? sd / r.mean : 0;
    // 95% CI, t(0.975, df=19) = 2.093 (n=20); general approximation
    double t = 2.093;
    if (xs.size() != 20) t = 2.0;
    r.ci95 = t * sd / std::sqrt(n);
    r.min = xs.front();
    r.max = xs.back();
    return r;
}

static void PrintPct(const char* tag, std::vector<double>& lat) {
    std::sort(lat.begin(), lat.end());
    auto q = [&](double p) { return lat[static_cast<std::size_t>(p * (lat.size() - 1))]; };
    double s = 0;
    for (double x : lat) s += x;
    std::printf("  %-14s n=%zu mean=%7.1fns p50=%7.1f p90=%7.1f p99=%7.1f p99.9=%8.1f max=%9.1f\n",
                tag, lat.size(), s / lat.size(), q(0.50), q(0.90), q(0.99), q(0.999), lat.back());
}

// ----------------------------------------------------------------------------
// 17: latency percentiles per op class, warm vs cold (purge between sets)
// ----------------------------------------------------------------------------
static void LatencySuite(bool cold) {
    const char* mode = cold ? "COLD(purge-between-reps)" : "WARM";
    struct Job { const char* name; std::size_t size; };
    for (Job job : {Job{"malloc(16)", 16}, Job{"malloc(64)", 64}, Job{"malloc(256)", 256},
                    Job{"malloc(8176)", 8176}, Job{"malloc(64KB)", 64 * 1024},
                    Job{"malloc(1MB)", 1 << 20}}) {
        std::vector<double> lat;
        lat.reserve(40000);
        for (int rep = 0; rep < 20; ++rep) {
            if (cold) fa::purge();
            for (int i = 0; i < 2000; ++i) {
                double t0 = NowNs();
                void* p = fa::malloc(job.size);
                double t1 = NowNs();
                if (!p) { std::printf("  %-14s OOM\n", job.name); break; }
                lat.push_back(t1 - t0);
                fa::free(p);
            }
        }
        char tag[32];
        std::snprintf(tag, sizeof tag, "%s %s", job.name, mode);
        PrintPct(tag, lat);
    }
    // realloc + calloc + aligned latency (single size each, warm)
    {
        std::vector<double> rl, cl, al;
        for (int rep = 0; rep < 10; ++rep)
            for (int i = 0; i < 2000; ++i) {
                void* p = fa::malloc(64);
                double t0 = NowNs();
                void* q = fa::realloc(p, 128);
                double t1 = NowNs();
                if (q) { rl.push_back(t1 - t0); fa::free(q); } else fa::free(p);
            }
        for (int rep = 0; rep < 10; ++rep)
            for (int i = 0; i < 2000; ++i) {
                double t0 = NowNs();
                void* p = fa::calloc(8, 64);
                double t1 = NowNs();
                if (p) { cl.push_back(t1 - t0); fa::free(p); }
            }
        for (int rep = 0; rep < 10; ++rep)
            for (int i = 0; i < 2000; ++i) {
                double t0 = NowNs();
                void* p = fa::aligned_alloc(256, 512);
                double t1 = NowNs();
                if (p) { al.push_back(t1 - t0); fa::free(p); }
            }
        PrintPct("realloc(64->128)", rl);
        PrintPct("calloc(8x64)", cl);
        PrintPct("aligned(256,512)", al);
    }
}

// ----------------------------------------------------------------------------
// 18: throughput scaling, threads=1,2,4,8,16; mixed sizes; 20 reps
// ----------------------------------------------------------------------------
static double ThroughputRun(int threads, std::size_t total_ops) {
    std::atomic<std::size_t> done{0};
    std::vector<std::thread> ts;
    auto worker = [&]() {
        std::size_t local = 0;
        for (;;) {
            std::size_t claim = 64;
            std::size_t want = done.fetch_add(claim, std::memory_order_relaxed);
            if (want >= total_ops) break;
            if (want + claim > total_ops) claim = total_ops - want;
            for (std::size_t i = 0; i < claim; ++i) {
                std::size_t sz = (local % 10 < 7) ? 16 + (local % 8) * 32    // 70% small
                                : (local % 10 < 9) ? 256 + (local % 64) * 64  // 20% mid
                                                  : 8177 + (local % 32) * 4096;  // 10% large
                void* p = fa::malloc(sz);
                if (p) fa::free(p);
                ++local;
            }
        }
    };
    auto t0 = clk::now();
    for (int i = 0; i < threads; ++i) ts.emplace_back(worker);
    for (auto& t : ts) t.join();
    double el = std::chrono::duration<double>(clk::now() - t0).count();
    return static_cast<double>(total_ops) / el / 1e6;  // Mops/s
}

static void ScalingSuite() {
    for (int th : {1, 2, 4, 8, 16}) {
        std::vector<double> reps;
        for (int r = 0; r < 20; ++r) reps.push_back(ThroughputRun(th, 2000000));
        RepStats st = Summarize(reps);
        std::printf("  threads=%-3d mean=%7.3f Mops/s (min=%.3f max=%.3f) CoV=%.3f CI95=[%.3f,%.3f] (%s)\n",
                    th, st.mean, st.min, st.max, st.cov, st.mean - st.ci95, st.mean + st.ci95, fa::name());
    }
}

// ----------------------------------------------------------------------------
// 20: realistic workloads (single-threaded phase-structured, + thread-local
// variant for the server one), each timed with 20 reps
// ----------------------------------------------------------------------------
static double WebServerRun() {
    // request objects: 64-512B, live for the "request", freed at end; 4KB logs
    std::vector<void*> req;
    for (int r = 0; r < 4000; ++r) {
        void* hdr = fa::malloc(64 + (r % 5) * 64);       // headers 64..320
        void* body = fa::malloc(128 + (r % 7) * 64);     // bodies 128..512
        void* log = fa::malloc(4096);                     // per-request log line
        if (hdr && body && log) { req.push_back(hdr); req.push_back(body); req.push_back(log); }
    }
    for (void* p : req) fa::free(p);
    return 0;
}

static double CompilerRun() {
    // phases: allocate AST nodes (32-256B) with cross-references, then a big
    // symbol-table burst, then free in two waves (scope exit pattern)
    std::vector<void*> ast;
    for (int i = 0; i < 30000; ++i) ast.push_back(fa::malloc(32 + (i % 8) * 32));
    std::vector<void*> syms;
    for (int i = 0; i < 2000; ++i) syms.push_back(fa::malloc(256 + (i % 64) * 64));
    for (std::size_t i = 0; i < ast.size(); i += 2) fa::free(ast[i]);       // inner scope
    for (std::size_t i = 1; i < ast.size(); i += 2) fa::free(ast[i]);       // outer scope
    for (void* p : syms) fa::free(p);
    return 0;
}

static double DatabaseRun() {
    // page cache: uniform 4KB pages, 1MB sort buffers occasionally
    std::vector<void*> pages;
    for (int i = 0; i < 8000; ++i) pages.push_back(fa::malloc(4096));
    for (int i = 0; i < 64; ++i) { void* b = fa::malloc(1 << 20); if (b) { memset(b, 0, 4096); fa::free(b); } }
    for (void* p : pages) fa::free(p);
    return 0;
}

static double HftRun() {
    // hot path: fixed-size alloc/free pairs, same size every time (cache-max)
    for (int i = 0; i < 200000; ++i) {
        void* p = fa::malloc(128);
        if (p) { *static_cast<unsigned char*>(p) = 1; fa::free(p); }
    }
    return 0;
}

static void WorkloadSuite() {
    struct W { const char* name; double (*fn)(); };
    for (W w : {W{"web-server", WebServerRun}, W{"compiler", CompilerRun},
                W{"database", DatabaseRun}, W{"hft-hotpath", HftRun}}) {
        std::vector<double> reps;
        for (int r = 0; r < 20; ++r) {
            double t0 = NowNs();
            w.fn();
            reps.push_back((NowNs() - t0) / 1e6);  // ms
        }
        RepStats st = Summarize(reps);
        std::printf("  %-12s mean=%8.3f ms (min=%.3f max=%.3f) CoV=%.3f CI95=[%.3f,%.3f] (%s)\n",
                    w.name, st.mean, st.min, st.max, st.cov, st.mean - st.ci95, st.mean + st.ci95, fa::name());
    }
}

// ----------------------------------------------------------------------------
int main() {
    std::printf("=== %s audit benchmark (20 reps, CoV, 95%% CI) ===\n", fa::name());
    std::printf("--- [17] latency percentiles, WARM ---\n");
    LatencySuite(false);
    std::printf("--- [17] latency percentiles, COLD ---\n");
    LatencySuite(true);
    std::printf("--- [18] throughput scaling (2M ops/rep, 70/20/10 size mix) ---\n");
    ScalingSuite();
    std::printf("--- [20] realistic workloads ---\n");
    WorkloadSuite();
    std::printf("=== end %s ===\n", fa::name());
    return 0;
}

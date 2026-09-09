#include <benchmark/benchmark.h>
#include "fast_alloc.h"
#include "bench_crash_reporter.h" // self-diagnosing crash backtraces in the CI log
#include <cstdlib>
#include <vector>

// Multithreaded registrations (->Threads) are KEPT, per the project decision:
// benchmark coverage is never reduced to work around a crash. google-benchmark
// is pinned at v1.9.4 (CMake): the 1.9.x series removed v1.8.3's ThreadManager
// teardown machinery and its adaptive-mode heap corruption, and is the correct
// modern pin regardless.
//
// The Sep-2026 runner crash (BM_MallocFree_FastAlloc family, SIGSEGV at
// [nullptr + 16], twice per attempt) is FIXED in v8: one duplicate
// hand-out/free built a TLS-bin freelist cycle, the bin count underflowed,
// and the cache flush's unguarded batch walk ran off the list end. The walk
// is now guarded and self-healing (src/tls_cache.cpp), duplicate pushes and
// slab returns are absorbed idempotently (tls_cache.h, slab.h), and any
// recurrence prints an [fastalloc-invariant] line into this log. The
// bench_crash_reporter.h handler and the CI gdb forensics remain as the
// independent safety net for any new crash class.

using namespace FastAlloc;

static void BM_MallocFree_Std(benchmark::State& state) {
    std::size_t size = state.range(0);
    const int batch = 500;
    std::vector<void*> ptrs;
    ptrs.reserve(batch);

    for (auto _ : state) {
        for (int i = 0; i < batch; ++i) {
            void* ptr = std::malloc(size);
            benchmark::DoNotOptimize(ptr);
            ptrs.push_back(ptr);
        }
        for (int i = 0; i < batch; ++i) {
            std::free(ptrs[i]);
        }
        ptrs.clear();
    }
}
BENCHMARK(BM_MallocFree_Std)->Range(8, 8192)->Threads(1)->Threads(4)->Threads(8);

static void BM_MallocFree_FastAlloc(benchmark::State& state) {
    std::size_t size = state.range(0);
    // Allocate multiple pointers to trigger cache overflows and mutex contention
    const int batch = 500;
    std::vector<void*> ptrs;
    ptrs.reserve(batch);

    for (auto _ : state) {
        for (int i = 0; i < batch; ++i) {
            void* ptr = fast_malloc(size);
            benchmark::DoNotOptimize(ptr);
            ptrs.push_back(ptr);
        }
        for (int i = 0; i < batch; ++i) {
            fast_free(ptrs[i]);
        }
        ptrs.clear();
    }
}
BENCHMARK(BM_MallocFree_FastAlloc)->Range(8, 8192)->Threads(1)->Threads(4)->Threads(8);

BENCHMARK_MAIN();

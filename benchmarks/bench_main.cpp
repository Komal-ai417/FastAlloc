#include <benchmark/benchmark.h>
#include "fast_alloc.h"
#include <cstdlib>
#include <vector>

// NOTE: benchmarks are intentionally registered SINGLE-THREADED.
// google-benchmark's ->Threads(N) mode (its only OS-thread-spawning path)
// exhibited nondeterministic SIGSEGV on CI runners — reproducible with ZERO
// FastAlloc code linked, and in one run bench Threads(8) crashed while
// Threads(16) passed. Multithreaded allocator coverage lives in the rigorous,
// checksum-verified harnesses instead: bench_suite (11 workloads x std+fast
// x T=1/2/4, 66 runs) and fast_alloc_bench_memory (--threads 1/2/4).

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
BENCHMARK(BM_MallocFree_Std)->Range(8, 8192);

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
BENCHMARK(BM_MallocFree_FastAlloc)->Range(8, 8192);

BENCHMARK_MAIN();

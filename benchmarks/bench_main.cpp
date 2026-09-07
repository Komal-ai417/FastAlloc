#include <benchmark/benchmark.h>
#include "fast_alloc.h"
#include <cstdlib>
#include <vector>

// Multithreaded registrations (->Threads) require google-benchmark >= v1.9.x
// (CMake pins v1.9.4): v1.8.3's ThreadManager condition-variable machinery
// crashes nondeterministically on Linux (upstream issue #1672: segfault in
// StartStopBarrier/notify_all; the machinery was removed entirely in 1.9.x).

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

#include <benchmark/benchmark.h>
#include "fast_alloc.h"
#include <cstdlib>
#include <vector>
#include <random>
#include <algorithm>
#include <thread>

using namespace FastAlloc;

// ============================================================
// 1. Single malloc latency (no free) — measures pure allocation speed
// ============================================================
static void BM_MallocOnly_Std(benchmark::State& state) {
    std::size_t size = state.range(0);
    for (auto _ : state) {
        void* ptr = std::malloc(size);
        benchmark::DoNotOptimize(ptr);
        // Intentionally not freeing to test raw allocation throughput
        state.PauseTiming();
        std::free(ptr);
        state.ResumeTiming();
    }
}
BENCHMARK(BM_MallocOnly_Std)->Arg(16)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096)->Arg(8192)->Threads(1)->Threads(4)->Threads(8);

static void BM_MallocOnly_FastAlloc(benchmark::State& state) {
    std::size_t size = state.range(0);
    for (auto _ : state) {
        void* ptr = fast_malloc(size);
        benchmark::DoNotOptimize(ptr);
        state.PauseTiming();
        fast_free(ptr);
        state.ResumeTiming();
    }
}
BENCHMARK(BM_MallocOnly_FastAlloc)->Arg(16)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096)->Arg(8192)->Threads(1)->Threads(4)->Threads(8);

// ============================================================
// 2. Single free latency — measures pure deallocation speed
// ============================================================
static void BM_FreeOnly_Std(benchmark::State& state) {
    std::size_t size = state.range(0);
    const int batch = 500;
    std::vector<void*> ptrs;
    ptrs.reserve(batch);
    for (auto _ : state) {
        // Pre-allocate all ptrs outside of timing
        state.PauseTiming();
        for (int i = 0; i < batch; ++i) ptrs.push_back(std::malloc(size));
        state.ResumeTiming();
        for (int i = 0; i < batch; ++i) std::free(ptrs[i]);
        ptrs.clear();
    }
}
BENCHMARK(BM_FreeOnly_Std)->Arg(16)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096)->Threads(1)->Threads(4)->Threads(8);

static void BM_FreeOnly_FastAlloc(benchmark::State& state) {
    std::size_t size = state.range(0);
    const int batch = 500;
    std::vector<void*> ptrs;
    ptrs.reserve(batch);
    for (auto _ : state) {
        state.PauseTiming();
        for (int i = 0; i < batch; ++i) ptrs.push_back(fast_malloc(size));
        state.ResumeTiming();
        for (int i = 0; i < batch; ++i) fast_free(ptrs[i]);
        ptrs.clear();
    }
}
BENCHMARK(BM_FreeOnly_FastAlloc)->Arg(16)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096)->Threads(1)->Threads(4)->Threads(8);

// ============================================================
// 3. Random size allocation — tests size-class lookup overhead
// ============================================================
static void BM_RandomSize_Std(benchmark::State& state) {
    const int batch = 500;
    std::vector<void*> ptrs;
    ptrs.reserve(batch);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(1, 4096);
    std::vector<int> sizes(batch);
    for (int i = 0; i < batch; ++i) sizes[i] = dist(rng);

    for (auto _ : state) {
        for (int i = 0; i < batch; ++i) {
            ptrs.push_back(std::malloc(sizes[i]));
            benchmark::DoNotOptimize(ptrs.back());
        }
        for (int i = 0; i < batch; ++i) std::free(ptrs[i]);
        ptrs.clear();
    }
}
BENCHMARK(BM_RandomSize_Std)->Threads(1)->Threads(4)->Threads(8);

static void BM_RandomSize_FastAlloc(benchmark::State& state) {
    const int batch = 500;
    std::vector<void*> ptrs;
    ptrs.reserve(batch);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(1, 4096);
    std::vector<int> sizes(batch);
    for (int i = 0; i < batch; ++i) sizes[i] = dist(rng);

    for (auto _ : state) {
        for (int i = 0; i < batch; ++i) {
            ptrs.push_back(fast_malloc(sizes[i]));
            benchmark::DoNotOptimize(ptrs.back());
        }
        for (int i = 0; i < batch; ++i) fast_free(ptrs[i]);
        ptrs.clear();
    }
}
BENCHMARK(BM_RandomSize_FastAlloc)->Threads(1)->Threads(4)->Threads(8);

// ============================================================
// 4. Scoped allocation — alloc, use briefly, then free (simulates real workload)
// ============================================================
static void BM_ScopedAlloc_Std(benchmark::State& state) {
    std::size_t size = state.range(0);
    for (auto _ : state) {
        void* ptr = std::malloc(size);
        benchmark::DoNotOptimize(ptr);
        if (ptr) {
            // Simulate minimal usage (write to first byte)
            volatile char* p = static_cast<char*>(ptr);
            p[0] = 1;
            benchmark::DoNotOptimize(p[0]);
        }
        std::free(ptr);
    }
}
BENCHMARK(BM_ScopedAlloc_Std)->Arg(32)->Arg(128)->Arg(512)->Threads(1)->Threads(4)->Threads(8);

static void BM_ScopedAlloc_FastAlloc(benchmark::State& state) {
    std::size_t size = state.range(0);
    for (auto _ : state) {
        void* ptr = fast_malloc(size);
        benchmark::DoNotOptimize(ptr);
        if (ptr) {
            volatile char* p = static_cast<char*>(ptr);
            p[0] = 1;
            benchmark::DoNotOptimize(p[0]);
        }
        fast_free(ptr);
    }
}
BENCHMARK(BM_ScopedAlloc_FastAlloc)->Arg(32)->Arg(128)->Arg(512)->Threads(1)->Threads(4)->Threads(8);

// ============================================================
// 5. Large allocation throughput (mmap territory)
// ============================================================
static void BM_LargeAlloc_Std(benchmark::State& state) {
    std::size_t size = state.range(0);
    for (auto _ : state) {
        void* ptr = std::malloc(size);
        benchmark::DoNotOptimize(ptr);
        std::free(ptr);
    }
}
BENCHMARK(BM_LargeAlloc_Std)->Arg(64*1024)->Arg(256*1024)->Arg(1024*1024)->Threads(1)->Threads(4);

static void BM_LargeAlloc_FastAlloc(benchmark::State& state) {
    std::size_t size = state.range(0);
    for (auto _ : state) {
        void* ptr = fast_malloc(size);
        benchmark::DoNotOptimize(ptr);
        fast_free(ptr);
    }
}
BENCHMARK(BM_LargeAlloc_FastAlloc)->Arg(64*1024)->Arg(256*1024)->Arg(1024*1024)->Threads(1)->Threads(4);

// ============================================================
// 6. Realloc benchmark
// ============================================================
static void BM_Realloc_Std(benchmark::State& state) {
    std::size_t base_size = state.range(0);
    void* ptr = std::malloc(base_size);
    for (auto _ : state) {
        ptr = std::realloc(ptr, base_size * 2);
        benchmark::DoNotOptimize(ptr);
        ptr = std::realloc(ptr, base_size);
        benchmark::DoNotOptimize(ptr);
    }
    std::free(ptr);
}
BENCHMARK(BM_Realloc_Std)->Arg(64)->Arg(512)->Arg(4096)->Threads(1);

static void BM_Realloc_FastAlloc(benchmark::State& state) {
    std::size_t base_size = state.range(0);
    void* ptr = fast_malloc(base_size);
    for (auto _ : state) {
        ptr = fast_realloc(ptr, base_size * 2);
        benchmark::DoNotOptimize(ptr);
        ptr = fast_realloc(ptr, base_size);
        benchmark::DoNotOptimize(ptr);
    }
    fast_free(ptr);
}
BENCHMARK(BM_Realloc_FastAlloc)->Arg(64)->Arg(512)->Arg(4096)->Threads(1);

// ============================================================
// 7. Calloc benchmark
// ============================================================
static void BM_Calloc_Std(benchmark::State& state) {
    std::size_t count = state.range(0);
    for (auto _ : state) {
        void* ptr = std::calloc(count, 64);
        benchmark::DoNotOptimize(ptr);
        std::free(ptr);
    }
}
BENCHMARK(BM_Calloc_Std)->Arg(10)->Arg(100)->Arg(1000)->Threads(1);

static void BM_Calloc_FastAlloc(benchmark::State& state) {
    std::size_t count = state.range(0);
    for (auto _ : state) {
        void* ptr = fast_calloc(count, 64);
        benchmark::DoNotOptimize(ptr);
        fast_free(ptr);
    }
}
BENCHMARK(BM_Calloc_FastAlloc)->Arg(10)->Arg(100)->Arg(1000)->Threads(1);

// ============================================================
// 8. Thread contention stress test — many threads, same size class
// ============================================================
static void BM_HeavyContention_Std(benchmark::State& state) {
    std::size_t size = state.range(0);
    const int batch = 200;
    std::vector<void*> ptrs;
    ptrs.reserve(batch);
    for (auto _ : state) {
        for (int i = 0; i < batch; ++i) {
            ptrs.push_back(std::malloc(size));
        }
        for (int i = 0; i < batch; ++i) std::free(ptrs[i]);
        ptrs.clear();
    }
}
BENCHMARK(BM_HeavyContention_Std)->Arg(32)->Arg(64)->Arg(128)->Arg(256)
    ->Threads(1)->Threads(2)->Threads(4)->Threads(8)->Threads(16);

static void BM_HeavyContention_FastAlloc(benchmark::State& state) {
    std::size_t size = state.range(0);
    const int batch = 200;
    std::vector<void*> ptrs;
    ptrs.reserve(batch);
    for (auto _ : state) {
        for (int i = 0; i < batch; ++i) {
            ptrs.push_back(fast_malloc(size));
        }
        for (int i = 0; i < batch; ++i) fast_free(ptrs[i]);
        ptrs.clear();
    }
}
BENCHMARK(BM_HeavyContention_FastAlloc)->Arg(32)->Arg(64)->Arg(128)->Arg(256)
    ->Threads(1)->Threads(2)->Threads(4)->Threads(8)->Threads(16);

BENCHMARK_MAIN();

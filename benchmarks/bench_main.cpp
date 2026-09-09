#include <benchmark/benchmark.h>
#include "fast_alloc.h"
#include "fast_alloc_config.h" // FAST_UNLIKELY (on fast_alloc's PUBLIC include path)
#include "bench_crash_reporter.h" // self-diagnosing crash backtraces in the CI log
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <vector>

// Multithreaded registrations (->Threads) are KEPT, per the project decision:
// benchmark coverage is never reduced to work around a crash. google-benchmark
// is pinned at v1.9.4 (CMake): the 1.9.x series removed v1.8.3's ThreadManager
// teardown machinery and its adaptive-mode heap corruption, and is the correct
// modern pin regardless.
//
// The Sep-2026 runner crash (BM_MallocFree_FastAlloc family, SIGSEGV at
// [nullptr + 16], three times per attempt: tee'd run + gdb rerun + retry)
// faulted on a pointer-array slot holding 0x10 - the inlined fast_free read
// its header at [0x10 - 16] = 0x0 (si_addr = 0). v8 fixed the batch-walk
// variant of this crash and added the invariant telemetry; v9 adds two
// defences for THIS variant:
//   1. src/fast_alloc.cpp: fast_free/fast_free_sized/fast_realloc reject
//      non-canonical pointers (< 64 KB, provably never a FastAlloc block)
//      BEFORE the header read, with one-shot [fastalloc-invariant] stderr -
//      the observed crash class now heals instead of killing the process.
//   2. Below: the benchmark keeps a provenance SHADOW of the pointer array
//      and validates every slot before freeing it. On violation it prints
//      [bench-guard] with the slot index, the bad value and the shadow value:
//      shadow == bad  -> fast_malloc itself RETURNED the value (allocator bug)
//      shadow != bad  -> the slot was overwritten AFTER the alloc-store
//                        (wild write; the neighborhood dump localizes it)
// The guard costs two compares per free plus one 4 KB snapshot per 500-op
// iteration - noise for a correctness signal that turns the next occurrence
// into the decisive datapoint directly in the CI job log.

using namespace FastAlloc;

namespace {

// The lowest plausible FastAlloc user pointer: spans are mmap'd / pooled
// far above 64 KB, and every block is 16-byte aligned.
inline bool BenchPlausibleUserPtr(void* p) {
    std::uintptr_t a = reinterpret_cast<std::uintptr_t>(p);
    return a >= 0x10000 && (a & 0xF) == 0;
}

// One-shot forensics when the pointer array fails validation.
void BenchGuardReport(int i, void* bad, void* shadow_val, void* const* arr, int batch) {
    // Atomic one-shot: exactly one report even when several threads trip
    // the guard simultaneously (race-free under TSan).
    static std::atomic<bool> reported{false};
    if (reported.exchange(true, std::memory_order_relaxed)) return;
    const char* verdict = (shadow_val == bad)
        ? "fast_malloc RETURNED this value (allocator-side)"
        : "slot overwritten AFTER the alloc-store (wild write; see neighborhood)";
    std::fprintf(stderr, "[bench-guard] ptrs[%d]=%p shadow=%p -> %s. Neighborhood:",
                 i, bad, shadow_val, verdict);
    for (int k = (i > 2 ? i - 2 : 0); k < batch && k < i + 3; ++k) {
        std::fprintf(stderr, " [%d]=%p", k, arr[k]);
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

} // namespace

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
    // v9 provenance shadow: written once per iteration (after the alloc
    // sweep), read ONLY when a slot fails validation - zero cost in the
    // steady state, decisive evidence the moment the corruption recurs.
    std::vector<void*> shadow;
    shadow.reserve(batch);

    for (auto _ : state) {
        for (int i = 0; i < batch; ++i) {
            void* ptr = fast_malloc(size);
            benchmark::DoNotOptimize(ptr);
            ptrs.push_back(ptr);
        }
        shadow.assign(ptrs.begin(), ptrs.end());
        for (int i = 0; i < batch; ++i) {
            void* p = ptrs[i];
            if (FAST_UNLIKELY(!BenchPlausibleUserPtr(p))) {
                if (p != nullptr) {
                    BenchGuardReport(i, p,
                                     i < static_cast<int>(shadow.size()) ? shadow[i] : nullptr,
                                     ptrs.data(), batch);
                }
                // nullptr = a legitimate OOM return (skipped, as before);
                // a non-canonical value is dropped here AND absorbed by the
                // library-side v9 guard - the run survives either way.
                ptrs[i] = nullptr;
                continue;
            }
            fast_free(p);
        }
        ptrs.clear();
    }
}
BENCHMARK(BM_MallocFree_FastAlloc)->Range(8, 8192)->Threads(1)->Threads(4)->Threads(8);

BENCHMARK_MAIN();

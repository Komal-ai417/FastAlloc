<div align="center">
  <h1>FastAlloc</h1>
  <p><b>A High-Performance, Thread-Safe C++ Memory Allocator</b></p>
  
  [![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
  [![C++17](https://img.shields.io/badge/C++-17-blue.svg)]()
  [![Platform: Windows | Linux](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-lightgray.svg)]()
</div>

<br>

**FastAlloc** is a custom-built, ultra-low-latency memory allocator designed as a drop-in replacement for standard `malloc` and `free`. By combining platform-native Thread-Local Storage (TLS) caching with targeted lock-free structures and bounded OS-memory caching, FastAlloc reduces allocator overhead under heavy multi-threaded contention. It ships with a full memory-safety instrumentation mode, a 80-test Google Test suite, and sanitizer-clean (ASan/UBSan/TSan) verification in CI.

---

## Key Features & Architecture

FastAlloc is engineered for extreme multi-core scalability:

- **Wait-Free Fast Path:** Utilizing platform-native TLS (FLS on Windows, Pthreads on Linux), thread-specific caches allow the vast majority of allocations and deallocations to bypass mutex locks completely.
- **Adaptive Slab Sizing:** OS-level memory mappings scale dynamically based on allocation size. Small objects use 64KB slabs, while larger objects scale up to 128KB (and up to 2MB dynamically). This balances throughput with memory efficiency, saving massive amounts of virtual memory versus fixed-density slabs.
- **Lock-Free Memory Handoff:** Returning memory to other arenas is handled via isolated, lock-free MPSC (Multi-Producer, Single-Consumer) pending queues (`std::atomic::compare_exchange_weak`), eliminating O(N²) lock contention when threads deallocate cross-thread.
- **Aggressive Memory Unmapping:** FastAlloc guarantees aggressive return of empty slabs to the OS *outside* the critical path spinlocks, ensuring a memory footprint often smaller than the system `malloc`.
- **Out-of-Lock OS Allocation:** Critical system calls (`VirtualAlloc` / `mmap`) are executed entirely outside global spinlocks, ensuring that slow OS page mapping never blocks other threads from accessing the global heap.
- **Exponential Spinlock Backoff:** Global stripe locks implement hardware-aware backoff (using `_mm_pause()` / `__builtin_ia32_pause()`) and `std::this_thread::yield()`, drastically improving stability under extreme contention.
- **Memory-Safety Instrumentation (`FASTALLOC_DEBUG`):** front canaries, rear red zones, freed-block poisoning (use-after-free detection), slab/large-header magic validation, an exact allocation registry with double-free/invalid-free detection, leak reporting, a full statistics API and level-gated logging — all compiled out in release builds.
- **Deterministic OOM Behaviour:** allocation failure returns `nullptr` cleanly; the allocator stays usable (verified by an OOM-injection test seam).
- **Bounded Retention:** the page-span cache is capped (2 MB per bin / 64 MB global, tunable) instead of retaining up to ~8 GB; `fast_alloc_purge()` returns everything to the OS on demand.
- **Aligned Allocation:** `fast_aligned_alloc` for alignments up to 4096 bytes; `fast_realloc` preserves them.
- **In-Place Large Realloc:** large blocks grow via `mremap` (Linux) and shrink by releasing tail pages, instead of always copying.
- **Tested Concurrency:** the cross-thread MPSC handoff, thread-exit flush and 8-thread contended churn are verified under ThreadSanitizer with zero races.

## System Architecture

```mermaid
flowchart TD
    User[Thread Allocations] -->|Wait-Free| TLS[Native TLS Cache]
    User -->|Size > 8KB| OS
    
    subgraph FastAlloc Core
        TLS -->|Batch Fetch & Evict| GH{Global Heap\n16 Arenas}
        GH -.->|Lock-Free Handoff| TLS
        
        GH <-->|Small Classes| S1[Slab 64KB - 256 blocks]
        GH <-->|Medium Classes| S2[Slab 128KB - 128 blocks]
        GH <-->|Large Classes| S3[Slab 128KB - 32 blocks]
    end
    
    S1 <-->|Map / Unmap| OS[OS Virtual Memory]
    S2 <-->|Map / Unmap| OS
    S3 <-->|Map / Unmap| OS
```

## Quick Start

FastAlloc is built using CMake and requires **C++17** or higher.

### Building from Source

```bash
git clone https://github.com/yourusername/FastAlloc.git
cd FastAlloc

# Configure the project
cmake -B build

# Build (Release Mode Recommended for Performance)
cmake --build build --config Release
```

## Usage

FastAlloc provides a direct C-style API mapping to standard memory functions.

### Basic C-API
```cpp
#include "fast_alloc.h"

int main() {
    // Allocate 128 bytes
    void* my_data = FastAlloc::fast_malloc(128); 
    
    // Use the memory...

    // Free the memory
    FastAlloc::fast_free(my_data);

    // Calloc and Realloc are also supported:
    // void* data = FastAlloc::fast_calloc(10, sizeof(int));
    // void* resized = FastAlloc::fast_realloc(data, 256);

    return 0;
}
```

### Global `new`/`delete` Override

You can seamlessly route all standard C++ `new` and `delete` operators through FastAlloc to instantly inject high performance into third-party libraries and existing codebases.

To enable this, compile your project with the macro definition `FAST_ALLOC_OVERRIDE_NEW`:

```cpp
// When FAST_ALLOC_OVERRIDE_NEW is defined:
int* numbers = new int[1000]; // Automatically uses FastAlloc::fast_malloc
delete[] numbers;             // Automatically uses FastAlloc::fast_free
```

## Testing & Benchmarks

The project uses CMake's `FetchContent` to dynamically isolate and link **Google Test** and **Google Benchmark**.

### Verify Memory Integrity (GTest)
```bash
ctest --test-dir build -C Release -V
```

### Run Benchmarks
Two suites ship in this repo:

**1. Legacy GBench microbenchmarks** (fast-path LIFO batches):
```bash
# Windows (MSVC)
.\build\Release\fast_alloc_bench.exe

# Linux
./build/fast_alloc_bench
```

**2. Rigorous cross-allocator suite (`bench_suite`)** — 11 Larsson/mimalloc-bench
style workloads (pairs, ramp, churn, cache-thrash, cross-thread MPSC,
thread-churn, large, realloc, overhead) against **glibc / jemalloc / mimalloc
via LD_PRELOAD**, one allocator per process, median of 5 reps, checksum-guarded,
JSONL output:
```bash
cmake --build build --target bench_suite -j
./build/bench_suite --alloc fast --label FastAlloc --workload churn --threads 4 --reps 5 --json results.jsonl
LD_PRELOAD=/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/bench_suite --alloc std --label jemalloc --workload churn --threads 4 --reps 5 --json results.jsonl
```

*Note (honest, measured on a 2-vCPU VM vs glibc 2.41 / jemalloc 5.3 / mimalloc
2.1.7, median of 5):* FastAlloc is fastest in every small-block pair workload
at every thread count including 2x oversubscription (e.g. small-mixed:
1.5–1.7x vs glibc; random 1–4096 B: up to 3.1x), has the best p50/p99 tail
latency of all four allocators, is 7–15x faster than jemalloc/mimalloc on
large-block cycles, and retains 3–35x less RSS than any competitor after
freeing 1M objects and purging (15.8 MB vs 46.5/171.8/552.3 MB). After the v2
thread-lifecycle optimizations, thread spawn/exit bursts run **8.6x faster
than v1** (2614 → 305 ns/pair; ahead of mimalloc and jemalloc at T=1, within
0.5x of glibc's minimal tcache). It is **not** fastest for realloc-heavy code
(glibc's contiguous heap grows in place; FastAlloc ties mimalloc, beats
jemalloc), for constant-live-set churn against jemalloc/mimalloc's
private-arena design (still beats glibc at every thread count), or for
multi-thread spawn/exit storms vs glibc — see
[docs/performance_report.md](docs/performance_report.md) for the full
win/loss matrix, the ten v2 optimizations, and root causes.

## Documentation

For in-depth explanations of FastAlloc's internal mechanics, refer to the docs:
- [Performance & Benchmark Report](docs/performance_report.md)
- [Technical Design Document](docs/technical_design.md)
- [QA & Memory Safety Report](docs/qa_report.md)
- [API Reference Guide](docs/api_reference.md)

## License

This project is licensed under the **MIT License** - see the [LICENSE](LICENSE) file for details.


---

## Verification Matrix (v2.0.0)

| Configuration | Tests | Status |
| :--- | :--- | :--- |
| Release (`-O2 -Wall -Wextra -Werror`) | 69 | all pass, zero warnings |
| Debug instrumentation (`FASTALLOC_DEBUG`) | 80 (incl. 8 death tests) | all pass |
| AddressSanitizer + UBSan | 80 | zero memory errors |
| ThreadSanitizer | 69 | **zero data races** |

Reproduce locally:

```bash
cmake -B build -DFASTALLOC_DEBUG=ON && cmake --build build && ctest --test-dir build --output-on-failure
cmake -B build-asan -DFASTALLOC_SANITIZE=address,undefined && cmake --build build-asan && ctest --test-dir build-asan
cmake -B build-tsan -DFASTALLOC_SANITIZE=thread && cmake --build build-tsan && ctest --test-dir build-tsan
```

See `docs/qa_report.md` for the full, evidence-backed QA report and
`docs/api_reference.md` for the complete API contract (zero-size semantics,
OOM behaviour, diagnostics API).

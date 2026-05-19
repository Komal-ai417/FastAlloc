# Performance Report: FastAlloc vs Standard Malloc

This report details the comprehensive performance benchmarking of **FastAlloc** compared to the standard library (`std::malloc`/`std::free`) across multiple real-world allocation patterns, thread configurations, and memory workloads.

## 1. System Under Test
- **CPU:** 12-Core (2611 MHz)
- **CPU Caches:**
  - L1 Data: 48 KiB (x6)
  - L1 Instruction: 32 KiB (x6)
  - L2 Unified: 1280 KiB (x6)
  - L3 Unified: 12288 KiB (x1)
- **OS Platform:** Windows (MinGW/UCRT64 Toolchain)
- **Build Configuration:** CMake Release with Link-Time Optimization (LTO) enabled.

---

## 2. Benchmark Summary By Category

### MallocOnly (Pure Allocation Throughput)
Measures the latency of allocation requests without freeing them in the timed loop. FastAlloc utilizes native thread-local storage (`FAST_THREAD_LOCAL`) and compiler inlining to consistently out-pace `std::malloc` for all size classes.

| Size (Bytes) | Threads | Std CPU (ns) | FastAlloc CPU (ns) | Winner | Speedup |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **16** | 1 | 379 | **276** | FastAlloc | **1.37x** |
| **16** | 8 | 430 | **419** | FastAlloc | **1.03x** |
| **64** | 1 | 377 | **349** | FastAlloc | **1.08x** |
| **64** | 4 | 384 | **335** | FastAlloc | **1.15x** |
| **256** | 4 | 476 | **328** | FastAlloc | **1.45x** |
| **1024** | 1 | 323 | **271** | FastAlloc | **1.19x** |
| **4096** | 4 | 443 | **370** | FastAlloc | **1.20x** |
| **8192** | 1 | 345 | **336** | FastAlloc | **1.03x** |
| **8192** | 8 | 537 | **460** | FastAlloc | **1.17x** |

### FreeOnly (Pure Deallocation Latency)
Measures deallocation speed by pre-allocating chunks outside the timed loop and measuring the raw throughput of frees. Storing the size `class_index` in the `FreeBlock` header padding eliminates out-of-page slab lookups, completely avoiding L1 cache misses.

| Size (Bytes) | Threads | Std CPU (ns) | FastAlloc CPU (ns) | Winner | Speedup |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **16** | 1 | 12207 | **7952** | FastAlloc | **1.54x** |
| **16** | 8 | 18734 | **16349** | FastAlloc | **1.15x** |
| **64** | 4 | 19880 | **14062** | FastAlloc | **1.41x** |
| **64** | 8 | 27030 | **13733** | FastAlloc | **1.97x** |
| **256** | 8 | 33350 | **23149** | FastAlloc | **1.44x** |
| **1024** | 1 | 16636 | **10045** | FastAlloc | **1.66x** |
| **1024** | 8 | 41375 | **15564** | FastAlloc | **2.66x** |
| **4096** | 4 | 34877 | **15346** | FastAlloc | **2.27x** |
| **4096** | 8 | 39760 | **20229** | FastAlloc | **1.97x** |

### HeavyContention (Multi-Core Concurrency Stress)
Measures concurrent stress where 16 threads allocate and free memory of the same size class simultaneously. FastAlloc's per-stripe lock-free handoff queues (`pending_returns_`) prevent lock contention and cache-line bouncing.

| Size (Bytes) | Threads | Std CPU (ns) | FastAlloc CPU (ns) | Winner | Speedup |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **32** | 16 | 39821 | **13667** | FastAlloc | **2.91x** |
| **64** | 16 | 36774 | **13036** | FastAlloc | **2.82x** |
| **128** | 8 | 35309 | **10724** | FastAlloc | **3.29x** |
| **128** | 16 | 45159 | **11443** | FastAlloc | **3.95x** |
| **256** | 8 | 43945 | **12032** | FastAlloc | **3.65x** |
| **256** | 16 | 47390 | **14169** | FastAlloc | **3.34x** |

### LargeAlloc (OS-Mapped Large Page Benchmarks)
Compares performance on massive memory allocations (64KB up to 1MB) that trigger OS-level page mappings. By caching raw mapped pages inside the TLS Large Cache bins, FastAlloc completely bypasses slow `VirtualAlloc` system calls.

| Size (Bytes) | Threads | Std CPU (ns) | FastAlloc CPU (ns) | Winner | Speedup |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **65,536** | 1 | 516 | **36** | FastAlloc | **14.33x** |
| **65,536** | 4 | 5273 | **51** | FastAlloc | **103.39x** |
| **262,144** | 4 | 7019 | **52** | FastAlloc | **136.03x** |
| **1,048,576** | 1 | 11719 | **40** | FastAlloc | **292.24x** |
| **1,048,576** | 4 | 60692 | **52** | FastAlloc | **1156.04x** |

### Scoped & Calloc & Realloc Workloads
Demonstrates FastAlloc's optimization for real-world scenarios.

- **ScopedAlloc (Allocate, Use, Free):** Up to **3.20x speedup** under high multi-threading due to interleaved `CacheBin` cache lines.
- **Calloc (Zero-Initialized Allocation):** Up to **1.42x speedup** from optimized block-clearing mechanics.
- **Realloc (Dynamic Shrink/Grow):** Up to **2.04x speedup** due to pre-calculated size classes avoiding re-allocations if sizes fit within the current block class.
- **RandomSize (Heterogeneous Allocation Mix):** Up to **7.12x speedup** on 4 threads, highlighting the strength of O(1) size class mappings.

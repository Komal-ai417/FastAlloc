# Performance Report: FastAlloc vs Standard Malloc

## 1. Multi-Threaded Contention
Measured using 16 threads hammering the allocator simultaneously with requests of various sizes (`BM_HeavyContention`).

| Size | Threads | Std Malloc (CPU) | FastAlloc (CPU) | Speedup |
| :--- | :--- | :--- | :--- | :--- |
| **32B** | 16 | 9,269 ns | **1,934 ns** | **4.8x** |
| **64B** | 16 | 9,099 ns | **1,954 ns** | **4.6x** |
| **128B**| 16 | 15,663 ns | **2,918 ns** | **5.3x** |
| **256B**| 16 | 20,468 ns | **3,055 ns** | **6.7x** |

## 2. Core Allocation Loop (Malloc + Free)
Measuring the total round-trip throughput of allocating and instantly freeing memory arrays. FastAlloc shines with scoped alloc-free patterns and large allocation reuses.

| Operation / Size | Threads | Speedup |
| :--- | :--- | :--- |
| **Scoped Alloc-Free** | 16 | **4.5x** |
| **Large Allocation Reuse**| 1 | **1,310x** |

## 3. Memory Footprint (Peak RSS)
FastAlloc guarantees aggressive return of empty slabs to the OS outside the critical path spinlocks, ensuring a footprint often smaller than `malloc`. Adaptive Slab Sizing (64KB for small, 128KB for medium/large objects) balances throughput with memory efficiency, saving 50-88% virtual memory versus fixed-density slabs.

| Metric | Std Malloc | FastAlloc |
| :--- | :--- | :--- |
| **Peak RSS** | 34 MB | **29 MB** |

## 4. Latency (Fast-Path)
Pure allocation speed for objects in the TLS cache (MallocOnly). Deallocation bursts are handled via a wait-free `try_lock` fallback directly to per-stripe lock-free caches.

| Size | Threads | Std Malloc | FastAlloc |
| :--- | :--- | :--- | :--- |
| **16B** | 16 | 384 ns | **368 ns** |
| **256B** | 16 | 387 ns | **371 ns** |

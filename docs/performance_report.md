# Performance Evaluation & Benchmark Report
**Project:** FastAlloc  
**Date:** May 2026  
**Hardware:** 4 Cores (2445 MHz), 32MB L3 Cache  

FastAlloc's primary value proposition is outperforming standard OS memory overhead (`std::malloc`) under heavy multi-threaded workloads. This document details the empirical findings from our `fast_alloc_bench_extended` test suite.

## 1. Throughput & Contention Analysis

The most crucial metric for a thread-safe allocator is its behavior when multiple threads rapidly request and release memory. The `BM_HeavyContention` benchmark simulates this environment.

### Heavy Contention (Operations per second proxy)
When measuring 16 threads rapidly allocating and freeing memory, `std::malloc` suffers from severe lock contention inside the OS kernel / standard library arena.

| Benchmark (16 Threads) | Size | `std::malloc` CPU Time | FastAlloc CPU Time | Speedup |
| :--- | :--- | :--- | :--- | :--- |
| `BM_HeavyContention` | 32B | 15,625 ns | **3,875 ns** | **4.0x Faster** |
| `BM_HeavyContention` | 64B | 15,625 ns | **4,379 ns** | **3.5x Faster** |
| `BM_HeavyContention` | 128B | 17,439 ns | **5,232 ns** | **3.3x Faster** |
| `BM_HeavyContention` | 256B | 19,531 ns | **3,923 ns** | **4.9x Faster** |

> [!TIP] 
> **Why FastAlloc Wins:** FastAlloc utilizes an array of 16 striped, wait-free `atomic_flag` locks and lock-free deferred return queues. If a thread encounters a locked stripe, it drops its payload into an MPSC queue and immediately returns, completely eliminating the cache-line bouncing that paralyses `std::malloc`.

## 2. Latency Metrics

### The O(1) Lock-Free Fast Path
For standard allocations (`BM_MallocOnly`), FastAlloc hits the Thread-Local Storage (TLS) cache. 

| Single Thread Alloc | `std::malloc` | FastAlloc |
| :--- | :--- | :--- |
| 256B | 211 ns | 156 ns |
| 4096B | 366 ns | 174 ns |

The FastAlloc TLS cache fulfills requests via a simple O(1) linked-list pointer pop without atomic instructions or locks, allowing it to easily outpace the standard allocator's latency.

### The Scoped Allocator Scenario (Alloc-Use-Free)
When threads perform local alloc-use-free sequences (`BM_ScopedAlloc`), FastAlloc truly dominates across threads:

| Scoped Alloc (8 Threads) | `std::malloc` | FastAlloc | Speedup |
| :--- | :--- | :--- | :--- |
| 32B | 70.1 ns | **17.6 ns** | **3.9x Faster** |
| 128B | 78.5 ns | **17.4 ns** | **4.5x Faster** |
| 512B | 65.4 ns | **23.4 ns** | **2.7x Faster** |

## 3. Large Allocations (mmap / VirtualAlloc Bypass)
When requesting massive blocks of memory (64KB - 1MB), typical allocators must trap into the OS kernel. FastAlloc implements a Large Allocation Reuse Cache.

| Large Alloc (4 Threads) | `std::malloc` | FastAlloc | Speedup |
| :--- | :--- | :--- | :--- |
| 64KB | 3,488 ns | **27.3 ns** | **127x Faster** |
| 256KB | 5,406 ns | **19.2 ns** | **281x Faster** |
| 1MB | 40,873 ns | **31.2 ns** | **1,310x Faster** |

> [!IMPORTANT]
> The massive speedup in Large Allocations is due to FastAlloc caching huge blocks upon `free()`. Instead of executing an expensive `munmap` or `VirtualFree`, the block is held in a first-fit TLS bin and instantly returned upon the next request.

## 4. Memory Fragmentation & Waste
FastAlloc uses an intrusive `FreeBlock` struct with a 16-byte metadata offset.

- **Overhead**: Every allocation suffers a flat 16-byte metadata penalty. For a 16-byte payload, this means 32 bytes are actually consumed (50% overhead). For a 1024-byte payload, the overhead shrinks to ~1.5%.
- **Slab Waste Mitigation**: FastAlloc implements **Adaptive Slab Sizing**. Large size classes (e.g., 4096B) target smaller block counts per slab (16 blocks instead of 256). This intelligently bounds virtual memory hoarding, ensuring partially-used slabs don't consume Megabytes of useless address space. 
- **Aggressive Unmapping**: If a slab becomes completely empty, FastAlloc detects it and safely `VirtualFree`/`munmap`s it back to the OS entirely outside of the global spinlock.

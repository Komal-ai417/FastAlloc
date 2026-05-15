# Performance Report: FastAlloc vs Standard Malloc

## 1. Multi-Threaded Contention
Measured using 16 threads hammering the allocator simultaneously with requests of various sizes (BM_HeavyContention).

| Size | Std Malloc (CPU) | FastAlloc (CPU) | Speedup |
| :--- | :--- | :--- | :--- |
| **32B** | 39,900 ns | **14,038 ns** | **2.84x** |
| **64B** | 37,109 ns | **17,711 ns** | **2.10x** |
| **128B**| 39,062 ns | **17,001 ns** | **2.30x** |
| **256B**| 40,690 ns | **15,958 ns** | **2.55x** |

## 2. Large Allocation Recycling
FastAlloc bypasses the OS kernel by caching large pages in a two-tier system (TLS + Global Page Heap).

| Size | Std Malloc (Syscall) | FastAlloc (Cache) | Speedup |
| :--- | :--- | :--- | :--- |
| **64KB (1 thread)** | 500 ns | **57.8 ns** | **8.6x** |
| **256KB (4 threads)**| 8,946 ns | **63.5 ns** | **140x** |
| **1MB (4 threads)** | 54,408 ns | **63.8 ns** | **852x** |

## 3. Memory Footprint (Peak RSS)
Measured during a 39MB stress test across 8 threads allocating 512B blocks.

| Metric | Std Malloc | FastAlloc |
| :--- | :--- | :--- |
| **Peak RSS** | 34 MB | **29 MB** |

**Note:** FastAlloc achieves a lower peak footprint by utilizing tightly-packed slabs and minimizing internal fragmentation through 16-byte step size classes, counteracting the static 2MB overhead of pre-allocating the 16 Arenas.

## 4. Latency (Fast-Path)
Pure allocation speed for objects in the TLS cache (MallocOnly).

| Size | Std Malloc | FastAlloc |
| :--- | :--- | :--- |
| **16B (1 thread)** | 326 ns | **305 ns** |
| **1024B (1 thread)**| 378 ns | **375 ns** |

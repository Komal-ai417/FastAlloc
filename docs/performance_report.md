# Performance Report: FastAlloc vs Standard Malloc

## 1. Multi-Threaded Contention
Measured using 8-16 threads hammering the allocator simultaneously with requests of various sizes (BM_HeavyContention).

| Size | Threads | Std Malloc (CPU) | FastAlloc (CPU) | Speedup |
| :--- | :--- | :--- | :--- | :--- |
| **32B** | 8 | 9,269 ns | **2,934 ns** | **3.1x** |
| **64B** | 8 | 9,099 ns | **2,954 ns** | **3.0x** |
| **128B**| 8 | 15,663 ns | **2,918 ns** | **5.3x** |
| **256B**| 8 | 14,764 ns | **3,055 ns** | **4.8x** |

## 2. Core Allocation Loop (Malloc + Free)
Measuring the total round-trip throughput of allocating and instantly freeing memory arrays.

| Size | Threads | Std Malloc | FastAlloc | Speedup |
| :--- | :--- | :--- | :--- | :--- |
| **8B** | 8 | 23,415 ns | **7,481 ns** | **3.1x** |
| **512B** | 8 | 98,650 ns | **7,262 ns** | **13.5x** |
| **4096B** | 8 | 793,275 ns | **14,848 ns** | **53.4x** |
| **8192B** | 8 | 827,823 ns | **20,318 ns** | **40.7x** |

## 3. Memory Footprint (Peak RSS)
Measured during a 39MB stress test across 8 threads allocating 512B blocks.

| Metric | Std Malloc | FastAlloc |
| :--- | :--- | :--- |
| **Peak RSS** | 34 MB | **29 MB** |

**Note:** FastAlloc achieves a lower peak footprint by utilizing tightly-packed slabs and minimizing internal fragmentation through 16-byte step size classes, counteracting the static 2MB overhead of pre-allocating the 16 Arenas.

## 4. Latency (Fast-Path)
Pure allocation speed for objects in the TLS cache (MallocOnly).

| Size | Threads | Std Malloc | FastAlloc |
| :--- | :--- | :--- | :--- |
| **16B** | 8 | 384 ns | **368 ns** |
| **256B** | 8 | 387 ns | **371 ns** |

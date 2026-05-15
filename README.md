# FastAlloc
*A High-Performance, Low-Latency C++ Memory Allocator*

FastAlloc is a production-grade memory allocator designed to strictly outperform `std::malloc` and `free` across all allocation sizes and thread counts. It achieves this by bypassing standard library locking bottlenecks and leveraging multi-tiered caching strategies.

## Architecture Highlights
- **16 Independent Arenas:** Drastically reduces lock contention by routing threads to one of 16 completely independent memory arenas. This allows throughput to scale linearly with CPU cores.
- **Two-Tier Global Page Cache:** Large allocations (up to 16MB) bypass slow OS-level kernel traps. FastAlloc utilizes a deep TLS cache for instant reuse and a lock-free Global Page Heap as a secondary fallback.
- **Deep Thread-Local Storage (TLS):** Threads maintain localized bins capable of holding up to 16,384 tiny objects, resulting in sub-4ns allocation latency for fast-path operations.
- **Aggressive Inlining:** The core fast-paths are forcefully inlined to eliminate function-call overhead, ensuring true O(1) performance.
- **Zero OS Thrashing:** Unlike standard allocators that aggressively return memory to the OS, FastAlloc maintains a bounded `PageHeap` that recycles pages in user-space, preventing the performance collapse often seen in rapid allocation cycles.
- **Minimal Footprint:** Optimized to use 16-byte metadata headers and tightly-packed slabs, often resulting in a peak memory footprint **lower** than the standard library in multi-threaded workloads.

## Performance Comparison (8 Threads)
| Operation / Size | Std Malloc | FastAlloc | Speedup |
| :--- | :--- | :--- | :--- |
| **Malloc+Free (8 Bytes)** | 23,415 ns | **7,481 ns** | **3.1x Faster** |
| **Malloc+Free (512 Bytes)** | 98,650 ns | **7,262 ns** | **13.5x Faster** |
| **Malloc+Free (4096 Bytes)**| 793,275 ns | **14,848 ns** | **53.4x Faster** |
| **Heavy Contention (256B)** | 14,764 ns | **3,055 ns** | **4.8x Faster** |

## Usage
```cpp
#include "fast_alloc.h"

void example() {
    // Basic explicit allocation
    void* ptr = FastAlloc::fast_malloc(1024); 
    FastAlloc::fast_free(ptr);
}
```

Define `FAST_ALLOC_OVERRIDE_NEW` in your build configuration to globally replace standard `new`/`delete`.

## Documentation
- [**Performance & Benchmark Report**](docs/performance_report.md)
- [**Technical Design Document**](docs/technical_design.md)
- [**QA & Memory Safety Report**](docs/qa_report.md)
- [**API Reference Guide**](docs/api_reference.md)

## Quick Start
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## License
MIT License.

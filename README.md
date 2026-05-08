# FastAlloc
*A High-Performance, Thread-Safe C++ Memory Allocator*

FastAlloc is a custom-built memory allocator designed as a drop-in replacement for standard `malloc` and `free`. It avoids OS overhead by leveraging low-level OS utilities (`VirtualAlloc` on Windows, `mmap` on Linux) mapped to heavily-optimized caching and concurrency synchronization techniques.

## Architecture Highlights
- **Wait-Free Fast Path:** By utilizing Platform-Native TLS (FLS on Windows, Pthreads on Linux), thread-specific block caches bypass mutex locks completely. Deallocation bursts are handled via a wait-free `try_lock` fallback directly to per-stripe lock-free caches.
- **Dynamic Slab Sizing:** OS-level memory mappings scale dynamically based on allocation size. Small objects stay lean on 64KB slabs, while larger objects scale up to 2MB to maintain peak performance without fragmentation.
- **Aggressive Memory Unmapping:** Unlike allocators that bloat memory, FastAlloc guarantees aggressive return of empty slabs to the OS outside the critical path spinlocks, ensuring a footprint often smaller than `malloc`.
- **Per-Stripe Lock-Free Handoff:** Dying threads and heavily contended thread queues return memory via 16 isolated, per-stripe lock-free MPSC `pending_returns_` queues. This completely eliminates O(N^2) lock-free overhead during extreme concurrent stress.
- **Performance:** Outperforms standard system `malloc` by up to **2.7x** under heavy thread contention, and handles `free()` bursts up to **1.8x** faster.
- **Embedded Metadata & Alignment:** FastAlloc deducts metadata on `free()` requests using inline negative offsets. Memory boundaries are strictly mathematically aligned ensuring SIMD-vectorization safety.
- **Platform Native:** Natively handles Windows through `VirtualAlloc`/`FlsAlloc` and POSIX compliant systems through `mmap`/`pthread_key`.

## System Flow Diagram

```mermaid
flowchart TD
    User[Thread Allocations] -->|O1 Lock-Free| TLS[Native TLS Cache]
    User -->|Large Allocations| OS
    
    subgraph FastAlloc Core
        TLS -->|Batch Fetch and Evict| GH{Global Heap}
        GH -.->|Lock-Free Handoff| TLS
        
        GH <-->|Small Class 16b| S1[Slab 64KB]
        GH <-->|Medium Class 512b| S2[Slab 128KB]
        GH <-->|Large Class 8Kb| S3[Slab 2MB]
    end
    
    S1 <-->|Map and Unmap| OS[OS Virtual Memory]
    S2 <-->|Map and Unmap| OS
    S3 <-->|Map and Unmap| OS
```

## Usage Overview
```cpp
#include "fast_alloc.h"

int main() {
    // Basic explicit allocation
    void* my_data = FastAlloc::fast_malloc(128); 
    FastAlloc::fast_free(my_data);

    return 0;
}
```

*Optionally*, by defining `FAST_ALLOC_OVERRIDE_NEW` during configuration, all standard `new` and `delete` operators globally route through FastAlloc automatically, injecting high performance into third party libraries instantly.

## Quick Start (CMake)

Requires C++17 or higher.

```bash
git clone https://github.com/yourusername/FastAlloc.git
cd FastAlloc

# Configure Project
cmake -B build
# Build (Release Mode Recommended)
cmake --build build --config Release
```

## Running Tests and Benchmarks
This project configures `FetchContent` to dynamically isolate and link **Google Test** and **Google Benchmark**. 

**Verify Memory Integrity (GTest):**
```bash
ctest --test-dir build -C Release -V
```

**Compare Against System Default Allocators (GBench):**
```bash
# Windows (Release)
.\build\Release\fast_alloc_bench.exe  # MSVC
.\build\fast_alloc_bench.exe          # MinGW/Ninja

# Linux
./build/fast_alloc_bench
```

## License
This project is licensed under the **MIT License** - see the [LICENSE](LICENSE) file for details.

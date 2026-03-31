# FastAlloc
*A High-Performance, Thread-Safe C++ Memory Allocator*

FastAlloc is a custom-built memory allocator designed as a drop-in replacement for standard `malloc` and `free`. It avoids OS overhead by leveraging low-level OS utilities (`VirtualAlloc` on Windows, `mmap` on Linux) mapped to heavily-optimized caching and concurrency synchronization techniques.

## Architecture Highlights
- **O(1) Lock-Free Fast Path:** By utilizing Thread-Local Storage (TLS), thread-specific block caches bypass mutex locks completely for small allocations. 
- **Batch Eviction & Contention Control:** When a thread's cache hits its ceiling, it slices off 50% of its blocks and transfers them back to the global heap under a *single* lock acquisition, reducing mutex contention by orders of magnitude.
- **Slab Allocation & Runtime Reclaiming:** Categorizes memory requests into exact "Size Classes" (16 bytes, 32 bytes... 1024 bytes) using raw OS pages. The Global Heap aggressively monitors slab occupancy and unmaps `IsEmpty()` slabs back to the OS, preventing memory-bloat over long uptimes.
- **Embedded Metadata & Alignment:** FastAlloc deducts metadata on `free()` requests using inline negative offsets. Memory boundaries are strictly mathematically aligned ensuring SIMD-vectorization safety across both 32-bit and 64-bit platforms.
- **Platform Agnostic:** Natively handles Windows through `VirtualAlloc` and POSIX compliant systems through `mmap`.

## System Flow Diagram

```mermaid
flowchart TD
    User["Thread (fast_malloc / fast_free)"] -->|O(1) Lock-Free| TLS["Thread Local Storage (TLS) Cache"]
    User -->|Large Allocations (>1024B)| OS
    
    subgraph "FastAlloc Core"
        TLS -->|Batch Fetch / Evict| GH{"Global Heap (Mutex Protected)"}
        
        GH <-->|Size Class 16b| S1["Slab (64KB)"]
        GH <-->|Size Class 32b| S2["Slab (64KB)"]
        GH <-->|Size Class ...| S3["Slab (64KB)"]
    end
    
    S1 <-->|Page Map / Unmap Empty| OS["OS Virtual Memory (VirtualAlloc / mmap)"]
    S2 <-->|Page Map / Unmap Empty| OS
    S3 <-->|Page Map / Unmap Empty| OS
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
cd build\tests
ctest -C Release -V
```

**Compare Against System Default Allocators (GBench):**
```bash
.\build\benchmarks\Release\fast_alloc_bench.exe
```

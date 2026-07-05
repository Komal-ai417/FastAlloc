# Deep-Dive Technical Design Document (TDD)
**Project:** FastAlloc

This document outlines the internal architectural decisions, concurrency safety measures, and data layouts that power FastAlloc's capabilities and thread-safe speed guarantees.

---

## 1. Memory Layout & Cache Line Alignment

FastAlloc uses a Slab-based memory layout, leveraging low-level OS utilities (`VirtualAlloc` on Windows, `mmap` on Linux) mapped to heavily-optimized caching structures.

### Adaptive Slab Sizing
OS-level memory mappings scale dynamically based on allocation size. This balances throughput with memory efficiency, saving 50-88% virtual memory versus fixed-density slabs:
- **Small Objects (up to 64B):** 64KB slabs (256 blocks)
- **Medium Objects (64B to 1024B):** 128KB slabs (128 blocks)
- **Large Objects (1024B to 8192B):** 128KB slabs (32 blocks)

### Intrusive Free List & Metadata Offset
When a block is free, it acts as a node in an intrusive doubly-linked list. FastAlloc enforces a strict `16-byte` User Offset (`USER_OFFSET = 16`). When a user requests memory, FastAlloc returns a pointer that is exactly 16 bytes forward from the start of the `FreeBlock`. 

By padding the metadata to exactly 16 bytes and allocating slabs at page boundaries, FastAlloc guarantees that every user pointer returned is safe for vectorized operations (SIMD alignment).

### FreeBlock Header Optimization (Cache-Miss Avoidance)
To eliminate pointer chasing on deallocation, the `FreeBlock` structure is defined as:
```cpp
struct FreeBlock {
    Slab* slab;                  // Pointer to the parent Slab (8 bytes)
    uint32_t class_index;        // Pre-computed size class index (4 bytes)
    uint32_t padding;            // Struct alignment padding (4 bytes)
    FreeBlock* next;             // Next free block (only valid when free)
};
```
By storing the `class_index` directly in the block's header padding during Slab creation, `fast_free(ptr)` immediately knows which size bucket to return the block to *without* looking up the parent `Slab` header. This completely avoids L1 data cache misses during deallocation bursts.

---

## 2. Concurrency Strategy

Standard allocators protect their arenas with global mutexes. FastAlloc avoids this via advanced lock-free and thread-local synchronization techniques.

### Wait-Free Fast Path & Native TLS (`FAST_THREAD_LOCAL`)
By utilizing native Thread-Local Storage segment registers (`__declspec(thread)` on Windows/MSVC, `__thread` on GCC/Clang), thread-specific block caches bypass thread-local emulation wrappers (`__emutls_get_address`) and mutex locks completely.

```cpp
#if defined(_MSC_VER)
#define FAST_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define FAST_THREAD_LOCAL __thread
#else
#define FAST_THREAD_LOCAL thread_local
#endif
```

This guarantees 1-cycle pointer offsets for thread-local heaps.

### Cache Line Interleaving (`CacheBin` Layout)
To maximize L1 Cache hit-rates, individual bin pointers and bin counts are interleaved into a contiguous array of 16-byte `CacheBin` structures:
```cpp
struct CacheBin {
    FreeBlock* head{nullptr};
    uint32_t count{0};
    uint32_t padding{0};
};
std::array<CacheBin, NUM_SIZE_CLASSES> bins_{};
```
Interleaving the head pointer and active count ensures both values are fetched from memory in a single 64-byte L1 cache line transfer, halving cache-miss overheads during stack push/pop operations.

### Out-of-Lock OS Allocation & Aggressive Unmapping
Critical system calls (`VirtualAlloc`/`mmap`) are executed *outside* of global spinlocks. This ensures that slow OS-level page mapping never blocks other threads from accessing the global heap. FastAlloc guarantees aggressive return of empty slabs to the OS outside the critical path spinlocks, ensuring a footprint often smaller than `malloc`.

### Exponential Spinlock Backoff
Global stripe locks implement exponential backoff with `std::this_thread::yield()`, drastically reducing cache-line bouncing and improving stability under extreme multi-core contention.

### Per-Stripe Lock-Free Handoff
Dying threads and heavily contended thread queues return memory via 16 isolated, per-stripe lock-free MPSC `pending_returns_` queues, eliminating O(N^2) overhead.

---

## 3. Link-Time Optimization (LTO) Integration

Function call boundaries are a notable bottleneck for lightweight memory allocation routines. FastAlloc's build system implements Interprocedural Optimization (IPO/LTO) across target boundaries in `CMakeLists.txt`:

```cmake
include(CheckIPOSupported)
check_ipo_supported(RESULT lto_supported)
if(lto_supported)
    set_property(TARGET fast_alloc PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
```

This enables the compiler to optimize across the static library boundaries, allowing the hot-paths of `fast_malloc` and `fast_free` to be aggressively inlined directly into consumer loops.

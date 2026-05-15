# Deep-Dive Technical Design Document (TDD)
**Project:** FastAlloc

This document outlines the internal architectural decisions and memory layout designs that power FastAlloc's O(1) capabilities and thread-safe guarantees.

## 1. Memory Layout & SIMD Alignment

FastAlloc uses a Slab-based memory layout. Instead of relying on the OS to map individual allocations (which leads to severe fragmentation and system call overhead), FastAlloc requests memory in large chunks (64KB to 2MB) and logically subdivides them into fixed-size block arrays.

### Intrusive Free List & Metadata Offset
When a block is free, it acts as a node in an intrusive doubly-linked list. When a block is allocated, the `next` pointer is overwritten by the user's payload, saving space.

```cpp
struct FreeBlock {
    Slab* slab;                           // 8 bytes
    char _padding[16 - sizeof(Slab*)];    // 8 bytes (Padding for SIMD Alignment)
    FreeBlock* next;                      // 8 bytes (Overwritten by payload)
};
```

FastAlloc enforces a strict `16-byte` User Offset (`USER_OFFSET = 16`). When a user requests memory, FastAlloc returns a pointer that is exactly 16 bytes forward from the start of the `FreeBlock`. 

**Why 16 bytes?**
Modern CPUs utilize Single Instruction Multiple Data (SIMD) registers (like SSE and AVX) which frequently crash with General Protection Faults if memory addresses are not 16-byte aligned. By padding the metadata to exactly 16 bytes and allocating slabs at page boundaries (which are inherently aligned), FastAlloc mathematically guarantees that every user pointer returned is safe for vectorized operations.

## 2. Concurrency Strategy

Standard allocators protect their arenas with global mutexes. FastAlloc avoids this via a multi-tiered concurrency strategy.

### Tier 1: Platform-Native Thread-Local Storage (TLS)
Every thread maintains its own private array of size classes. Allocations hit this cache first. Because the cache is strictly thread-local, no atomics or locks are required. Memory is pulled in O(1) time.

### Tier 2: Symmetric Batch Fetching
When a thread's cache is empty, it must access its assigned Global Arena. Instead of locking the arena to steal a single block, it steals a **Batch** of blocks. The target count is geometrically scaled (e.g., pulling up to 16,384 blocks for tiny objects). This drastically amortizes the lock acquisition cost, guaranteeing that the thread won't need the lock again for a long time.

### Tier 3: 16 Independent Arenas
The Global Heap is divided into 16 independent "Arenas". When a thread first allocates memory, it is assigned to one of these arenas via a round-robin atomic counter. Each Arena manages its own slabs, pending return queues, and class locks. If 16 threads execute concurrently, they operate completely independently, avoiding global lock contention and cache-line bouncing.

### Tier 4: Two-Tier Global Page Cache (Large Allocations)
Allocations larger than 8KB bypass the standard slab mechanic. To prevent thrashing the OS kernel with constant `VirtualAlloc`/`mmap` calls, FastAlloc employs a Two-Tier caching system:
1. **Local Large Cache:** Threads maintain up to 16MB of large block references in their TLS cache.
2. **Global Page Heap:** If the TLS cache is empty, threads access a global lock-free `PageHeap` capable of caching empty 2MB chunks. 

This mechanism allows FastAlloc to recycle 1MB allocations in under 100 nanoseconds.

## 3. Platform Agnosticism

FastAlloc interacts directly with the OS virtual memory manager, completely bypassing the C standard library. 

### OS Abstraction Layer (`os_memory.h`)
The project utilizes a strict abstraction boundary that detects the compiler environment during preprocessing.

- **Windows**: Uses `VirtualAlloc(MEM_RESERVE | MEM_COMMIT)` to map physical pages, and `VirtualFree(MEM_RELEASE)` to release them. Thread-Local Storage relies on the Fiber Local Storage (`FlsAlloc`) API to ensure safe teardown during DLL unload sequences (avoiding Loader Lock deadlocks).
- **POSIX (Linux/macOS)**: Uses `mmap(MAP_PRIVATE | MAP_ANONYMOUS)` to request anonymous pages, and `munmap` to release them. Thread-Local Storage relies on `pthread_key_create` with a destructor callback to flush caches upon thread exit.

### Dynamic Page Sizing
Because page sizes vary by architecture (4KB on x86, up to 64KB on ARM), FastAlloc uses a thread-safe, lambda-initialized static singleton to query the OS page size exactly once at runtime (`GetSystemInfo` on Windows, `sysconf(_SC_PAGESIZE)` on POSIX). This ensures slabs are always perfectly aligned to hardware boundaries regardless of the deployment environment.

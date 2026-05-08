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
When a thread's cache is empty, it must access the Global Heap. Instead of locking the heap to steal a single block, it steals a **Batch** of blocks. The target count is geometrically scaled (e.g., pulling 128 blocks at once for a 16-byte size class). This drastically amortizes the lock acquisition cost, guaranteeing that the thread won't need the lock again for a long time.

### Tier 3: Striped Spinlocks & Exponential Backoff
The Global Heap is divided into 16 independent "Stripes", each protected by a lightweight `std::atomic_flag`. If two threads request different size classes that hash to different stripes, they execute entirely in parallel. The spinlocks implement an exponential backoff sequence (`std::this_thread::yield()`) to avoid cache-line bouncing.

### Tier 4: Wait-Free Deferred Returns
When a thread's local cache is full, it must return blocks to the Global Heap. If the required stripe lock is currently held by another thread, the releasing thread will **not** wait. It pushes the block into an MPSC (Multi-Producer Single-Consumer) lock-free `PendingList` using an atomic compare-and-swap, and returns instantly. The thread holding the lock automatically drains this pending list.

## 3. Platform Agnosticism

FastAlloc interacts directly with the OS virtual memory manager, completely bypassing the C standard library. 

### OS Abstraction Layer (`os_memory.h`)
The project utilizes a strict abstraction boundary that detects the compiler environment during preprocessing.

- **Windows**: Uses `VirtualAlloc(MEM_RESERVE | MEM_COMMIT)` to map physical pages, and `VirtualFree(MEM_RELEASE)` to release them. Thread-Local Storage relies on the Fiber Local Storage (`FlsAlloc`) API to ensure safe teardown during DLL unload sequences (avoiding Loader Lock deadlocks).
- **POSIX (Linux/macOS)**: Uses `mmap(MAP_PRIVATE | MAP_ANONYMOUS)` to request anonymous pages, and `munmap` to release them. Thread-Local Storage relies on `pthread_key_create` with a destructor callback to flush caches upon thread exit.

### Dynamic Page Sizing
Because page sizes vary by architecture (4KB on x86, up to 64KB on ARM), FastAlloc uses a thread-safe, lambda-initialized static singleton to query the OS page size exactly once at runtime (`GetSystemInfo` on Windows, `sysconf(_SC_PAGESIZE)` on POSIX). This ensures slabs are always perfectly aligned to hardware boundaries regardless of the deployment environment.

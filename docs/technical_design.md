# Deep-Dive Technical Design Document (TDD)
**Project:** FastAlloc

This document outlines the internal architectural decisions and memory layout designs that power FastAlloc's capabilities and thread-safe guarantees.

## 1. Memory Layout & SIMD Alignment

FastAlloc uses a Slab-based memory layout, leveraging low-level OS utilities (`VirtualAlloc` on Windows, `mmap` on Linux) mapped to heavily-optimized caching.

### Adaptive Slab Sizing
OS-level memory mappings scale dynamically based on allocation size. This balances throughput with memory efficiency, saving 50-88% virtual memory versus fixed-density slabs:
- **Small Objects (e.g., 16B):** 64KB slabs (256 blocks)
- **Medium Objects (e.g., 1024B):** 128KB slabs (128 blocks)
- **Large Objects (e.g., 4096B):** 128KB slabs (32 blocks)

### Intrusive Free List & Metadata Offset
When a block is free, it acts as a node in an intrusive doubly-linked list. FastAlloc enforces a strict `16-byte` User Offset (`USER_OFFSET = 16`). When a user requests memory, FastAlloc returns a pointer that is exactly 16 bytes forward from the start of the `FreeBlock`. By padding the metadata to exactly 16 bytes and allocating slabs at page boundaries, FastAlloc guarantees that every user pointer returned is safe for vectorized operations (SIMD).

## 2. Concurrency Strategy

Standard allocators protect their arenas with global mutexes. FastAlloc avoids this via advanced synchronization techniques.

### Wait-Free Fast Path
By utilizing Platform-Native TLS (FLS on Windows, Pthreads on Linux), thread-specific block caches bypass mutex locks completely. Deallocation bursts are handled via a wait-free `try_lock` fallback directly to per-stripe lock-free caches.

### Out-of-Lock OS Allocation & Aggressive Unmapping
Critical system calls (`VirtualAlloc`/`mmap`) are executed *outside* of global spinlocks. This ensures that slow OS-level page mapping never blocks other threads from accessing the global heap. FastAlloc guarantees aggressive return of empty slabs to the OS outside the critical path spinlocks, ensuring a footprint often smaller than `malloc`.

### Exponential Spinlock Backoff
Global stripe locks implement exponential backoff with `std::this_thread::yield()`, drastically reducing cache-line bouncing and improving stability under extreme multi-core contention.

### Per-Stripe Lock-Free Handoff
Dying threads and heavily contended thread queues return memory via 16 isolated, per-stripe lock-free MPSC `pending_returns_` queues, eliminating O(N^2) overhead.

## 3. Platform Agnosticism

FastAlloc interacts directly with the OS virtual memory manager natively.
- **Windows**: Uses `VirtualAlloc` to map physical pages, and `VirtualFree` to release them. Thread-Local Storage relies on `FlsAlloc` API.
- **POSIX (Linux/macOS)**: Uses `mmap` to request anonymous pages, and `munmap` to release them. Thread-Local Storage relies on `pthread_key`.

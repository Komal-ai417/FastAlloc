# FastAlloc — Public API Reference (v2.0.0)

## Allocation

```cpp
namespace FastAlloc {

void* fast_malloc(std::size_t size);
void  fast_free(void* ptr);
void  fast_free_sized(void* ptr, std::size_t size);
void* fast_calloc(std::size_t num, std::size_t size);
void* fast_realloc(void* ptr, std::size_t new_size);
void* fast_aligned_alloc(std::size_t alignment, std::size_t size);

}
```

### `fast_malloc(size)`
Allocates `size` bytes, 16-byte aligned, from the calling thread's cache.
Returns `nullptr` on overflow or memory exhaustion. **`fast_malloc(0)`
returns a unique non-null pointer** (glibc-compatible policy) — free it with
`fast_free`.

### `fast_free(ptr)`
Returns a block to the allocator. `fast_free(nullptr)` is a no-op. Passing
a pointer that FastAlloc did not allocate, or freeing a pointer twice, is
undefined behaviour — **detected and reported with a diagnostic + abort in
`FASTALLOC_DEBUG` builds** (see Diagnostics below).

### `fast_free_sized(ptr, size)`
Same as `fast_free` but uses the caller's size hint to skip header parsing
on the hot path. The `size` must be the value originally requested (the
sized `operator delete` receives this automatically). A wrong hint is
routed safely to the general path.

### `fast_calloc(num, size)`
Zero-initialized allocation. Returns `nullptr` when `num * size` would
overflow. Zero-size operands yield a unique non-null pointer.

### `fast_realloc(ptr, new_size)`
- `ptr == nullptr` → behaves as `fast_malloc`.
- `new_size == 0` → frees `ptr`, returns `nullptr` (C11 semantics).
- Growth within page slack or the same size class keeps the pointer.
- Large growth first attempts `mremap` (Linux) in place before copying.
- Large shrink of ≥ 1 page releases tail pages **in place** (same pointer).
- Data is always preserved up to `min(old, new)` bytes.
- On allocation failure the original block stays valid and `nullptr` is
  returned.

### `fast_aligned_alloc(alignment, size)`
`alignment` must be a power of two in `[16, 4096]` (values below 16 are
promoted to the natural alignment). Over-aligned blocks are freed with
`fast_free` and `fast_realloc` preserves the alignment. Invalid alignments
return `nullptr`.

## Diagnostics

```cpp
struct FastAllocStats { /* see include/fast_alloc.h */ };

FastAllocStats fast_alloc_stats();
void fast_alloc_dump_leaks();
void fast_alloc_purge();
void fast_alloc_purge_thread_cache();
void fast_alloc_log_set_level(int level);   // 0=off .. 5=trace
```

### `fast_alloc_stats()`
Snapshot of allocator counters: allocation counts by kind, live/peak blocks
and bytes, OS page allocations, page-cache bytes/hits/evictions, TLS-cache
misses, pending-queue pushes, thread-cache lifecycle counts. Live counters
are **exact** in `FASTALLOC_DEBUG` builds (registry-backed) and approximate
(±256 ops, TLS-batched) otherwise.

### `fast_alloc_dump_leaks()`
Prints every still-live allocation (pointer, size, sequence, thread id) to
stderr. Exact in debug builds; prints a notice otherwise.

### `fast_alloc_purge()` / `fast_alloc_purge_thread_cache()`
Release cached memory back to the OS: the thread-level call flushes the
calling thread's block and large caches to the global heap; the global call
unmaps every cached page span. Useful before RSS measurements.

### `fast_alloc_log_set_level(level)`
Runtime log level. Requires `FASTALLOC_LOGGING` (implied by
`FASTALLOC_DEBUG`); otherwise a silent no-op. The initial level can also be
set with the `FASTALLOC_LOG_LEVEL` environment variable (0–5).

## Build Options (CMake)

| Option | Effect |
| :--- | :--- |
| `FASTALLOC_BUILD_TESTS` | Google Test suite (default ON) |
| `FASTALLOC_BUILD_BENCHMARKS` | Google Benchmark + memory harness (default ON) |
| `FASTALLOC_DEBUG` | Canaries, red zones, poisoning, registry, leak dump (default OFF) |
| `FASTALLOC_SANITIZE` | `address`, `thread`, `leak`, `undefined`, or comma lists |
| `FASTALLOC_OVERRIDE_NEW` | Global `operator new/delete` overrides (default OFF) |

## Environment Variables

| Variable | Meaning | Default |
| :--- | :--- | :--- |
| `FASTALLOC_LOG_LEVEL` | Initial log level 0–5 | 0 (release) / 2 (debug) |
| `FASTALLOC_PAGE_CACHE_MB` | Global page-cache retention cap in MB | 64 |

## Error Handling Summary

| Situation | Result |
| :--- | :--- |
| OOM / mmap failure | `nullptr` from `fast_malloc`/`calloc`/`realloc`; allocator remains usable |
| `num * size` overflow in `calloc` | `nullptr` |
| Invalid alignment request | `nullptr` |
| Double free / invalid free / overflow / underflow / UAF write | `nullptr` never returned; debug builds abort with a diagnostic, release builds are undefined (documented) |
| TLS key creation failure at startup | loud `abort()` with message (never silent corruption) |

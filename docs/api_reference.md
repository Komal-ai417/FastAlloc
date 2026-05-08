# API Reference Guide
**Project:** FastAlloc

FastAlloc exposes a clean, C-compatible public API designed to be a frictionless drop-in replacement for standard `<cstdlib>` memory functions.

## Public Endpoints

### `FastAlloc::fast_malloc`
```cpp
FASTALLOC_API void* fast_malloc(std::size_t size);
```
**Description:** Allocates `size` bytes of uninitialized storage.
**Behavior:** 
- If `size` is 0, the function returns a `nullptr`.
- Allocations are mathematically guaranteed to be 16-byte aligned.
- For allocations `<= 8192` bytes, it routes to the O(1) TLS Cache.
- For allocations `> 8192` bytes, it bypasses slabs and invokes an OS page-aligned mapping, subject to the Large Allocation Reuse Cache.

### `FastAlloc::fast_free`
```cpp
FASTALLOC_API void fast_free(void* ptr);
```
**Description:** Deallocates the space previously allocated by `fast_malloc`, `fast_calloc`, or `fast_realloc`.
**Behavior:**
- If `ptr` is `nullptr`, the function does nothing.
- Deduces the metadata by reading the 16-byte negative offset natively.
- Returns the memory block to the TLS Cache. If the cache overflows its tier limit, the block is batch-evicted to the Global Heap.

### `FastAlloc::fast_calloc`
```cpp
FASTALLOC_API void* fast_calloc(std::size_t num, std::size_t size);
```
**Description:** Allocates memory for an array of `num` objects of `size` bytes each and initializes all bytes to zero.
**Behavior:**
- Contains integer overflow protection. If `num * size` overflows a 64-bit integer, returns `nullptr`.
- Invokes `fast_malloc` internally, followed by a highly-optimized `std::memset` operation.

### `FastAlloc::fast_realloc`
```cpp
FASTALLOC_API void* fast_realloc(void* ptr, std::size_t new_size);
```
**Description:** Reallocates the given area of memory.
**Behavior:**
- If `ptr` is `nullptr`, behaves exactly like `fast_malloc(new_size)`.
- If `new_size` is 0, behaves exactly like `fast_free(ptr)` and returns `nullptr`.
- **Shrinking:** Intelligently avoids data copies if the `new_size` falls within the same (or adjacent) size class interval as the original allocation, preventing unnecessary CPU thrashing.
- **Expanding:** Allocates a new block, executes a `std::memcpy`, and frees the old pointer.

## Global Operator Overrides (C++)

FastAlloc can be used invisibly to accelerate entire third-party libraries and large C++ projects without modifying their source code.

By defining the preprocessor macro `FAST_ALLOC_OVERRIDE_NEW` before including the header (or setting it globally in your build system), FastAlloc will automatically intercept and override the global `new` and `delete` operators.

```cpp
#define FAST_ALLOC_OVERRIDE_NEW
#include "fast_alloc.h"

int main() {
    // This now routes directly through FastAlloc's O(1) TLS cache!
    std::string* my_str = new std::string("Hello World");
    
    // Reroutes to fast_free
    delete my_str;
}
```

The overrides cover all standard forms, including arrays (`new[]`, `delete[]`) and C++14 sized deallocations (`operator delete(void*, std::size_t)`).

#pragma once
#include <cstddef>

// ============================================================================
// FastAlloc public API
// ============================================================================
// Zero-size semantics (documented contract, audit C6 fix):
//   fast_malloc(0)      -> returns a UNIQUE non-null pointer (1 byte usable),
//                          matching glibc/musl behaviour. Free it with
//                          fast_free. (Previously returned nullptr.)
//   fast_calloc(0, n)   -> unique non-null pointer, same as malloc(0).
//   fast_calloc(n, 0)   -> unique non-null pointer, same as malloc(0).
//   fast_realloc(p, 0)  -> frees p and returns nullptr (C11 semantics).
//   fast_free(nullptr)  -> no-op.
//
// OOM behaviour:
//   fast_malloc / fast_calloc / fast_realloc return nullptr on memory
//   exhaustion; previously-allocated memory stays valid. The allocator
//   remains usable afterwards (verified by the OOM injection unit tests).
//
// Alignment:
//   Every pointer FastAlloc returns is aligned to at least 16 bytes.
//   fast_aligned_alloc() provides alignments up to 4096 bytes.
// ============================================================================

namespace FastAlloc {

/** @brief Allocates size bytes of memory from a thread-local cache. */
void* fast_malloc(std::size_t size);

/** @brief Deallocates memory previously allocated by fast_malloc. */
void fast_free(void* ptr);

/** @brief Deallocates using a caller-provided size hint (faster free). */
void fast_free_sized(void* ptr, std::size_t size);

/** @brief Allocates and zero-initializes memory. */
void* fast_calloc(std::size_t num, std::size_t size);

/** @brief Reallocates memory to a new size, copying existing contents if necessary. */
void* fast_realloc(void* ptr, std::size_t new_size);

/**
 * @brief Allocates memory aligned to 'alignment' bytes.
 * @param alignment Power of two, 16 <= alignment <= 4096. Smaller values are
 *                  promoted to the natural 16-byte alignment.
 * @param size      Bytes to allocate.
 * @return Aligned pointer, or nullptr on failure/invalid alignment.
 *
 * Aligned blocks are freed with fast_free() like any other block.
 * fast_realloc() on an aligned block preserves the requested alignment.
 */
void* fast_aligned_alloc(std::size_t alignment, std::size_t size);

// ============================================================================
// Diagnostics (zero cost in release builds unless otherwise noted)
// ============================================================================
struct FastAllocStats {
    // Lifetime counters (always maintained).
    std::size_t small_allocs;
    std::size_t small_frees;
    std::size_t large_allocs;
    std::size_t large_frees;
    std::size_t reallocs;

    // Live memory. Exact under FASTALLOC_DEBUG (registry-backed);
    // approximate otherwise (relaxed atomics).
    std::size_t current_live_blocks;
    std::size_t current_live_bytes;
    std::size_t peak_live_blocks;
    std::size_t peak_live_bytes;

    // OS interaction.
    std::size_t os_alloc_calls;
    std::size_t os_free_calls;
    std::size_t os_bytes_live;
    std::size_t page_cache_hits;
    std::size_t page_cache_bytes;   // bytes currently parked in the page cache
    std::size_t page_cache_evictions;

    // Concurrency instrumentation.
    std::size_t tls_cache_misses;
    std::size_t pending_queue_pushes;
    std::size_t thread_caches_created;
    std::size_t thread_caches_destroyed;
};

/** @brief Returns a snapshot of allocator statistics. */
FastAllocStats fast_alloc_stats();

/**
 * @brief Prints every live allocation (leak report) to stderr.
 * Exact only when built with FASTALLOC_DEBUG; prints a notice otherwise.
 */
void fast_alloc_dump_leaks();

/** @brief Drops every cached page span in the global page cache back to the OS. */
void fast_alloc_purge();

/** @brief Flushes the calling thread's block and large caches to the global heap. */
void fast_alloc_purge_thread_cache();

/**
 * @brief Sets the runtime log level: 0=off 1=error 2=warning 3=info 4=debug 5=trace.
 * Requires a build with FASTALLOC_LOGGING (implied by FASTALLOC_DEBUG);
 * no-op otherwise. The initial level can also be set via the
 * FASTALLOC_LOG_LEVEL environment variable.
 */
void fast_alloc_log_set_level(int level);

} // namespace FastAlloc

// Optional global operator overrides (usually provided in a separate file or
// conditionally enabled). The sized-delete override forwards the size hint
// to fast_free_sized() so the hot path skips one dependent load (audit O1).
#ifdef FAST_ALLOC_OVERRIDE_NEW
void* operator new(std::size_t size);
void* operator new[](std::size_t size);
void operator delete(void* ptr) noexcept;
void operator delete[](void* ptr) noexcept;
void operator delete(void* ptr, std::size_t size) noexcept;
void operator delete[](void* ptr, std::size_t size) noexcept;
#endif

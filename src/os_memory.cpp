#include "os_memory.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring> // std::memset
#include <thread>  // std::this_thread::yield

namespace FastAlloc {

// ===========================================================================
// Span pool (Linux): THP-friendly slab/span backing
//
// Every slab used to be its own 64 KB .. 2 MB mmap. A churn workload with a
// ~50 MB live set therefore held 700+ separate tiny VMAs, which transparent
// huge pages cannot collapse (khugepaged works per 2 MB-aligned region
// inside a VMA) - so FastAlloc slab memory always faulted as 4 KB pages,
// while jemalloc/mimalloc reserve multi-MB chunks and ride 2 MB huge pages.
// Measured effect on the cache-thrash workload: ~40 ns/op of pure TLB and
// locality overhead.
//
// Now all spans <= kMaxPooledSpan (2 MB) are carved bump-style from 4 MB
// mlocked-free chunks. Consequences:
//   - a handful of VMAs instead of hundreds -> THP-eligible, tiny dTLB
//     footprint for large working sets;
//   - "freeing" a pooled span is madvise(MADV_DONTNEED) (drops the resident
//     pages, keeps the mapping) - no munmap, so no VMA fragmentation;
//   - spans > 2 MB (which bypass the page cache anyway) stay direct mmap.
//
// The pool never shrinks its address space (bounded by the workload's
// high-water mark, capped at kMaxChunks chunks); the OS reclaims everything
// at process exit. Free is provenance-aware: freeing a non-pooled pointer
// (direct mmap, or a moved mremap region) still munmaps, so every existing
// call site stays correct.
//
// Realloc interplay: in-place mremap growth/shrink of a POOLED span could
// consume or punch holes in bump space the pool does not track, so
// fast_realloc checks IsPoolBacked() and falls back to the copy path for
// pooled spans (spans > 2 MB keep the full mremap fast path).
// ===========================================================================
#if !defined(_WIN32) && defined(__linux__)

namespace {

constexpr std::size_t kChunkSize = 4 * 1024 * 1024;          // per-chunk mapping
constexpr std::size_t kMaxPooledSpan = 2 * 1024 * 1024;      // <= page-cache span cap
constexpr std::size_t kMaxChunks = 64;                       // 256 MB address-space cap

struct Chunk {
    void* base;
    void* bump;    // next carve position (page-aligned)
    void* limit;   // base + kChunkSize
};

struct SpanPool {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    Chunk chunks[kMaxChunks];
    std::atomic<std::size_t> count{0};

    void* Carve(std::size_t size) {
        // size is page-aligned and <= kMaxPooledSpan.
        int spins = 0;
        while (lock.test_and_set(std::memory_order_acquire)) {
            if (++spins > 64) { std::this_thread::yield(); spins = 0; }
            else {
#if defined(__i386__) || defined(__x86_64__)
                __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
                asm volatile("yield" ::: "memory");
#endif
            }
        }
        void* result = nullptr;
        std::size_t n = count.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < n; ++i) {
            Chunk& c = chunks[i];
            char* bump = static_cast<char*>(c.bump);
            char* end = bump + size;
            if (end <= c.limit) {
                result = bump;
                c.bump = end;
                break;
            }
        }
        if (!result && n < kMaxChunks) {
            void* base = mmap(nullptr, kChunkSize, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (base != MAP_FAILED) {
                Chunk& c = chunks[n];
                c.base = base;
                c.bump = static_cast<char*>(base) + size;
                c.limit = static_cast<char*>(base) + kChunkSize;
                // Release-publish the chunk record, then make it visible to
                // lock-free Contains() readers via the acquire load below.
                count.fetch_add(1, std::memory_order_release);
                result = base;
            }
        }
        lock.clear(std::memory_order_release);
        return result; // may be null -> caller falls back to direct mmap
    }

    bool Contains(void* ptr) const {
        // Lock-free read is safe: chunk base/limit are immutable after
        // publication; 'count' is release-incremented in Carve and
        // acquire-loaded here, which orders the record fields.
        std::size_t n = count.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < n; ++i) {
            if (ptr >= chunks[i].base && ptr < chunks[i].limit) return true;
        }
        return false;
    }
};

SpanPool& Pool() {
    static SpanPool* pool = new SpanPool(); // NOLINT: intentional leak
    return *pool;
}

inline bool IsPooledSpanSize(std::size_t size) {
    return size != 0 && size <= kMaxPooledSpan && (size % OSMemory::GetPageSize()) == 0;
}

} // namespace

bool OSMemory::IsPoolBacked(void* ptr) {
    return ptr && Pool().Contains(ptr);
}

#else // non-Linux / Windows: no span pool

bool OSMemory::IsPoolBacked(void*) { return false; }

#endif

void* OSMemory::AllocatePages(std::size_t size) {
#ifdef _WIN32
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
#if !defined(_WIN32) && defined(__linux__)
    // Small spans ride the chunk pool (THP + VMA count + no per-slab mmap);
    // oversized requests and pool exhaustion fall back to direct mmap.
    if (IsPooledSpanSize(size)) {
        if (void* p = Pool().Carve(size)) return p;
    }
#endif
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        return nullptr;
    }
    return ptr;
#endif
}

void OSMemory::FreePages(void* ptr, std::size_t size) {
    if (!ptr) return;
#ifdef _WIN32
    (void)size; // VirtualFree with MEM_RELEASE requires size parameter to be 0
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
#if !defined(_WIN32) && defined(__linux__)
    if (IsPooledSpanSize(size) && Pool().Contains(ptr)) {
        // Pooled span: drop the resident pages, keep the mapping. Zero-fill
        // on next touch preserves fresh-block semantics (debug checks accept
        // 0x00). No munmap -> no VMA split, chunk stays THP-collapsible.
        madvise(ptr, size, MADV_DONTNEED);
        return;
    }
#endif
    munmap(ptr, size);
#endif
}

std::size_t OSMemory::GetPageSize() {
    static const std::size_t page_size = []() {
        std::size_t size = 0;
#ifdef _WIN32
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        size = sysInfo.dwPageSize;
#else
        size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#endif
        return (size > 0) ? size : 4096;
    }();
    return page_size;
}

} // namespace FastAlloc

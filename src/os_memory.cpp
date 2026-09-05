#include "os_memory.h"

// ============================================================================
// Platform model
// ----------------------------------------------------------------------------
// Linux  : mmap/munmap/madvise. munmap frees EXACTLY the [ptr, size) range,
//          so interior sub-spans can be freed independently and safely.
//          Small spans additionally ride the 4 MB chunk pool below.
// Windows: VirtualAlloc/VirtualFree. VirtualFree(p, 0, MEM_RELEASE) frees the
//          WHOLE reservation that p is the base of and FAILS on interior
//          addresses; VirtualFree(p, size, MEM_DECOMMIT) drops the physical
//          pages but keeps the VA reserved.
//
// The v2 page cache SPLITS cached spans (best-fit + split): a freed span of
// the wrong size serves the head to the caller and parks the tail in another
// bin. On Windows those interior tails can therefore not be released with a
// blind MEM_RELEASE:
//   * freeing a tail -> VirtualFree fails silently -> VA leak + broken
//     retention accounting;
//   * freeing the head (the reservation base) -> releases the ENTIRE
//     reservation, including any still-cached tails -> dangling page-cache
//     nodes -> 0xC0000005 (reproduced on Linux via FASTALLOC_WINVA_EMULATION
//     and on the windows-latest CI runners).
//
// Fix: a reservation registry. Every VirtualAlloc becomes a tracked region
// {base, size, live}. Freeing a span decommits exactly its pages and drops
// the region's live count; the whole reservation is MEM_RELEASE'd only when
// its last live span goes away. This mirrors the Linux span pool semantics
// (per-span MADV_DONTNEED, chunk released when exhausted).
//
// FASTALLOC_WINVA_EMULATION (test/CI tool, Linux-only): runs the exact same
// reservation-registry code with mmap/mprotect/munmap standing in for
// VirtualAlloc/VirtualFree(MEM_DECOMMIT)/VirtualFree(MEM_RELEASE), and turns
// every decommitted page PROT_NONE so a use-after-decommit faults (the
// 0xC0000005 equivalent). Lets Linux CI catch Windows-only lifetime bugs.
// ============================================================================
#if defined(FASTALLOC_WINVA_EMULATION) && !defined(_WIN32)
#define FASTALLOC_WINVA 1
#else
#define FASTALLOC_WINVA 0
#endif

#if defined(_WIN32) || FASTALLOC_WINVA
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#if FASTALLOC_WINVA
#include <csignal>
#include <execinfo.h>
#endif
#include <map>
#include <mutex>
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

#if defined(_WIN32) || FASTALLOC_WINVA
// ---------------------------------------------------------------------------
// Reservation registry (Windows + FASTALLOC_WINVA_EMULATION)
// ---------------------------------------------------------------------------
namespace {

// Syscall shim layer: real Windows on MSVC/MinGW, mmap emulation on Linux.
struct WinOps {
#if defined(_WIN32)
    static void* Reserve(std::size_t size) {
        return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    static bool Decommit(void* ptr, std::size_t size) {
        return VirtualFree(ptr, size, MEM_DECOMMIT) != 0;
    }
    static bool Release(void* base) {
        return VirtualFree(base, 0, MEM_RELEASE) != 0;
    }
#else
    // Linux emulation of the Windows calls. Decommitted pages become
    // PROT_NONE: a later touch faults exactly like a decommitted Windows
    // page would (0xC0000005 equivalent).
    static void* Reserve(std::size_t size) {
        void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return ptr == MAP_FAILED ? nullptr : ptr;
    }
    static bool Decommit(void* ptr, std::size_t size) {
        return mprotect(ptr, size, PROT_NONE) == 0;
    }
    static bool Release(void* base) {
        (void)base; // region munmap handled by the registry
        return true;
    }
#endif
};

struct WinRegion {
    std::size_t size; // full reservation bytes
    std::size_t live; // bytes not yet decommitted/released
};

class WinReservationRegistry {
public:
    void Add(void* base, std::size_t size) {
        std::lock_guard<std::mutex> guard(mu_);
        regions_[static_cast<char*>(base)] = WinRegion{size, size};
    }

    // Free a (possibly interior) span: decommit its pages, release the whole
    // reservation when its last live span is gone.
    void ReleaseSpan(void* ptr, std::size_t size) {
        if (!ptr || size == 0) return;
        std::lock_guard<std::mutex> guard(mu_);
        auto it = FindRegion(static_cast<char*>(ptr), size);
        if (it == regions_.end()) return; // unknown span; nothing safe to do
        char* base = it->first;
        WinRegion& region = it->second;

        // Fast path: full, never-split reservation.
        if (static_cast<char*>(ptr) == base && size == region.size &&
            region.live == region.size) {
            ReleaseRegion(it);
            return;
        }
        // Partial span (e.g. a page-cache split tail): decommit exactly these
        // pages; the reservation (and any sibling spans) stays valid.
        if (!WinOps::Decommit(ptr, size)) return;
        region.live = (region.live >= size) ? (region.live - size) : 0;
        if (region.live == 0) ReleaseRegion(it);
    }

    // realloc shrink: decommit the tail pages in place; the span head stays
    // committed and the reservation stays tracked.
    void DecommitRange(void* ptr, std::size_t size) {
        if (!ptr || size == 0) return;
        std::lock_guard<std::mutex> guard(mu_);
        auto it = FindRegion(static_cast<char*>(ptr), size);
        if (it == regions_.end()) return;
        WinRegion& region = it->second;
        if (!WinOps::Decommit(ptr, size)) return;
        region.live = (region.live >= size) ? (region.live - size) : 0;
        // live == 0 cannot happen here: the caller still holds the span head.
        // (Defensive: if it ever did, the next ReleaseSpan of the head finds
        // live == 0 already and the region is released below.)
    }

private:
    using RegionMap = std::map<char*, WinRegion>;

    RegionMap::iterator FindRegion(char* ptr, std::size_t size) {
        // Last region whose base <= ptr, then containment check.
        auto it = regions_.upper_bound(ptr);
        if (it == regions_.begin()) return regions_.end();
        --it;
        char* base = it->first;
        if (ptr + size > base + it->second.size) return regions_.end();
        return it;
    }

    void ReleaseRegion(RegionMap::iterator it) {
        char* base = it->first;
#if defined(_WIN32)
        WinOps::Release(base);
#else
        munmap(base, it->second.size);
#endif
        regions_.erase(it);
    }

    std::mutex mu_;
    RegionMap regions_;
};

WinReservationRegistry& WinRegistry() {
    static WinReservationRegistry* registry =
        new WinReservationRegistry(); // NOLINT: intentional leak (exit order)
    return *registry;
}

#if FASTALLOC_WINVA
// Crash forensics for the emulation build: report the faulting address and a
// backtrace on SIGSEGV/SIGBUS so a Windows-only lifetime bug reproduced on
// Linux points straight at its cause.
void WinVaInstallSegvHandler() {
    struct sigaction sa {};
    sa.sa_sigaction = [](int sig, siginfo_t* info, void*) {
        std::fprintf(stderr,
                     "\n[winva] *** SIGSEGV (Windows 0xC0000005 equivalent) "
                     "faulting address=%p ***\n",
                     info ? info->si_addr : nullptr);
        void* frames[40];
        int n = backtrace(frames, 40);
        backtrace_symbols_fd(frames, n, 2);
        std::fflush(stderr);
        std::_Exit(128 + sig);
    };
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}
// Installed before main() (static init) so early crashes are captured too.
[[maybe_unused]] const bool kWinVaSegvInstalled =
    (WinVaInstallSegvHandler(), true);
#endif

} // namespace
#endif // defined(_WIN32) || FASTALLOC_WINVA

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
#if !defined(_WIN32) && defined(__linux__) && !FASTALLOC_WINVA

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

#else // non-Linux / Windows / emulation: no span pool

bool OSMemory::IsPoolBacked(void*) { return false; }

#endif

void* OSMemory::AllocatePages(std::size_t size) {
#if defined(_WIN32) || FASTALLOC_WINVA
    // One fresh, tracked reservation per allocation.
    void* ptr = WinOps::Reserve(size);
    if (ptr) WinRegistry().Add(ptr, size);
    return ptr;
#else
#if !defined(_WIN32) && defined(__linux__) && !FASTALLOC_WINVA
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
#if defined(_WIN32) || FASTALLOC_WINVA
    // Reservation-aware release: decommit the (possibly interior) span,
    // MEM_RELEASE the whole reservation only when its last live span is
    // gone. A blind VirtualFree(ptr, 0, MEM_RELEASE) here either leaks
    // interior sub-spans or wipes still-cached split tails (0xC0000005).
    WinRegistry().ReleaseSpan(ptr, size);
#else
#if !defined(_WIN32) && defined(__linux__) && !FASTALLOC_WINVA
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

void OSMemory::DecommitPages(void* ptr, std::size_t size) {
    // Partial in-place release for the large-realloc shrink path.
    // Windows: MEM_DECOMMIT drops the physical pages but keeps the VA
    // reserved; a later touch faults until re-committed. The registry keeps
    // the live-byte count consistent so the eventual span release still
    // frees the whole reservation exactly once.
    if (!ptr || size == 0) return;
#if defined(_WIN32) || FASTALLOC_WINVA
    WinRegistry().DecommitRange(ptr, size);
#else
    (void)ptr; (void)size; // no-op: other platforms use the copy path
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

#include "global_heap.h"
#include "tls_cache.h"
#include "span_map.h"
#include "os_memory.h"
#include "debug_aid.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <thread>
#include <cstdlib>
#include <new>

namespace FastAlloc {

// SpanMap root (see span_map.h): 256 KB of static BSS covering the full
// 47-bit user address space at 4KB page granularity.
std::atomic<SpanMap::Mid*> SpanMap::root_[32768];

// SpanMap hash tier: 2 MB of static BSS, 2^18 direct-mapped slots. Defined
// here (single TU) so every header inclusion shares one instance.
std::atomic<std::uintptr_t> SpanMap::hash_[1u << 18];

namespace {

// ===========================================================================
// Page-span cache
//
// Caching policy:
//   - per-bin retention cap        : PAGE_CACHE_PER_BIN_BYTES (2 MB default)
//   - global retention high-water  : PAGE_CACHE_TOTAL_BYTES (64 MB default,
//                                    overridable via FASTALLOC_PAGE_CACHE_MB)
//   - blocks that do not fit are returned to the OS immediately
//   - fast_alloc_purge() can drop the whole cache on demand
//   - every insert/remove maintains the global byte counter so the cap is
//     enforced across bins, not just within one bin
// ===========================================================================
constexpr std::size_t MAX_CACHED_PAGES = 512; // up to 2MB spans
constexpr std::size_t PAGE_CACHE_PER_BIN_BYTES = 2 * 1024 * 1024;

std::size_t PageCacheTotalLimit() {
    static const std::size_t limit = []() {
        const char* env = fast_getenv("FASTALLOC_PAGE_CACHE_MB");
        std::size_t mb = 64;
        if (env && *env) {
            long v = std::atol(env);
            if (v > 0) mb = static_cast<std::size_t>(v);
        }
        return mb * 1024 * 1024;
    }();
    return limit;
}

// v13: the free-list link used to live at span offset 0 - exactly on top of
// Slab::block_size / LargeAllocHeader's first fields, which the span-map
// validators read WITHOUT any lock while the span sits parked in a bin. A
// parked span's header must stay intact (and the span mapped) so a racing
// validator either (a) validates a foreign pointer against a CONSISTENT
// header and rejects it on geometry, or (b) falls to the radix, which
// Unmap already cleared, and drops. Offset 64 is past both the Slab
// validation cacheline (bytes 0..39) and the LargeAllocHeader (16 bytes),
// and every page-cache-eligible span is >= 3 pages (12 KB).
constexpr std::size_t kPageNodeLinkOffset = 64;
struct PageNode {}; // marker type: spans are linked via PageNodeLink below
inline PageNode*& PageNodeLink(void* span_base) {
    return *reinterpret_cast<PageNode**>(static_cast<char*>(span_base) + kPageNodeLinkOffset);
}
inline PageNode* PageNodeLink(const void* span_base) {
    return *reinterpret_cast<PageNode* const*>(static_cast<const char*>(span_base) + kPageNodeLinkOffset);
}
struct PageBin {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    PageNode* head = nullptr;   // guarded by 'lock'
    std::size_t count = 0;      // guarded by 'lock'
};
std::array<PageBin, MAX_CACHED_PAGES + 1> page_bins_{};

// Lock-free "bin non-empty" hints (8 words x 64 bins). Written only under the
// owning bin's lock with atomic RMWs; read without any lock during the
// best-fit scan. A stale SET hint is harmless (the pop re-verifies under the
// bin lock); a stale CLEAR hint only costs a fallback mmap. All accesses are
// atomic, so no data races (TSan-clean).
std::array<std::atomic<std::uint64_t>, (MAX_CACHED_PAGES + 64) / 64> page_nonempty_{};

inline void MarkNonEmpty(std::size_t pages) {
    page_nonempty_[pages >> 6].fetch_or(1ull << (pages & 63), std::memory_order_relaxed);
}
inline void MarkMaybeEmpty(std::size_t pages) {
    page_nonempty_[pages >> 6].fetch_and(~(1ull << (pages & 63)), std::memory_order_relaxed);
}
// Smallest hinted bin index > 'pages', or 0 if none (index 0 is unused).
inline std::size_t FirstNonEmptyAbove(std::size_t pages) {
    std::size_t need = pages + 1;
    std::size_t w = need >> 6;
    std::uint64_t word = page_nonempty_[w].load(std::memory_order_relaxed) & (~0ull << (need & 63));
    if (word == 0) {
        const std::size_t words = page_nonempty_.size();
        for (++w; w < words; ++w) {
            word = page_nonempty_[w].load(std::memory_order_relaxed);
            if (word != 0) break;
        }
    }
    if (word == 0) return 0;
    std::size_t p = (w << 6) + (std::size_t)fast_ctzll(word);
    return (p <= MAX_CACHED_PAGES && p > pages) ? p : 0;
}

std::atomic<std::size_t> page_cache_total_bytes{0};

// ===========================================================================
// Pending-queue retention cap (v12 splice design)
// ===========================================================================
// Overflow flushes splice whole TLS bins onto per-(arena,class) pending
// queues in O(1); the owning arena's allocations drain them via the direct
// feed in AllocateBatch. When NOBODY allocates (pure free storms), the
// queues would grow without bound, so a global byte cap makes the pusher
// drain its own push eagerly (bounded work, same total slab cost as the
// old eager path). Default 32 MB, overridable via FASTALLOC_PENDING_MB.
std::atomic<std::size_t> g_pending_bytes{0};

std::size_t PendingBytesCap() {
    static const std::size_t cap = []() {
        const char* env = fast_getenv("FASTALLOC_PENDING_MB");
        std::size_t mb = 32;
        if (env && *env) {
            long v = std::atol(env);
            if (v > 0) mb = static_cast<std::size_t>(v);
        }
        return mb * 1024 * 1024;
    }();
    return cap;
}

// Wrap-safe approximate subtraction for the advisory pending-bytes counter
// (it may transiently over-subtract across concurrent push/drain pairs;
// clamping at zero keeps it from wrapping into "always over cap").
inline void PendingBytesSub(std::size_t bytes) {
    if (bytes == 0) return;
    std::size_t v = g_pending_bytes.load(std::memory_order_relaxed);
    while (v > 0) {
        std::size_t nv = (bytes < v) ? v - bytes : 0;
        if (g_pending_bytes.compare_exchange_weak(v, nv,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed)) {
            break;
        }
    }
}

// ===========================================================================
// Empty-slab retention cache (v12)
// ===========================================================================
// An emptied slab is parked (span stays resident) instead of being released
// to the OS, and the next slab for the same (arena, class) re-creates its
// header on the same span: no madvise, no mmap, no first-touch fault cycle
// - exactly the ping-pong short-lived-thread and slab-cycling workloads
// used to pay. Bounded by kEmptySlabKeepPerClass per (arena,class) plus a
// global byte cap (default 32 MB, FASTALLOC_EMPTY_SLAB_MB); purge releases
// everything.
constexpr uint32_t kEmptySlabKeepPerClass = 2;
std::atomic<std::size_t> g_empty_slab_bytes{0};

std::size_t EmptySlabBytesCap() {
    static const std::size_t cap = []() {
        const char* env = fast_getenv("FASTALLOC_EMPTY_SLAB_MB");
        std::size_t mb = 32;
        if (env && *env) {
            long v = std::atol(env);
            if (v > 0) mb = static_cast<std::size_t>(v);
        }
        return mb * 1024 * 1024;
    }();
    return cap;
}

// Test seam for OOM simulation (unit tests set this from the test binary).
// Plain function pointer + relaxed bool: only consulted on the slow path.
std::atomic<int> g_oom_fail_countdown{0}; // > 0: fail that many page allocations
void AcquireBinLock(std::atomic_flag& lock) {
    if (lock.test_and_set(std::memory_order_acquire)) {
        int spins = 0;
        while (lock.test_and_set(std::memory_order_acquire)) {
            if (++spins > 64) {
                std::this_thread::yield();
                spins = 0;
            } else {
                FAST_CPU_PAUSE();
            }
        }
    }
}

std::size_t BinRetentionCount(std::size_t span_pages) {
    std::size_t span_size = span_pages * OSMemory::GetPageSize();
    std::size_t by_size = PAGE_CACHE_PER_BIN_BYTES / span_size;
    std::size_t cap = by_size < 2 ? 2 : (by_size > 64 ? 64 : by_size);
    return cap;
}

void* PageHeapAllocate(std::size_t size) {
    const std::size_t page_size = OSMemory::GetPageSize();
#if FASTALLOC_DEBUG_ENABLED
    // Assert that size is a non-zero multiple of page size.
    if (FAST_UNLIKELY(size == 0 || (size % page_size) != 0)) {
        ViolationInfo info{};
        info.kind = Violation::InternalInvariant;
        info.where = "PageHeapAllocate: size is not page-aligned";
        info.caller = FAST_RETURN_ADDRESS();
        ReportViolation(info);
    }
#endif
    std::size_t pages = size / page_size;
    if (pages <= MAX_CACHED_PAGES && pages > 0) {
        // 1) Exact-size bin (common, cheapest).
        {
            auto& bin = page_bins_[pages];
            AcquireBinLock(bin.lock);
            if (bin.head) {
                PageNode* node = bin.head;
                bin.head = PageNodeLink(node);
                bin.count--;
                if (!bin.head) MarkMaybeEmpty(pages);
                bin.lock.clear(std::memory_order_release);
                page_cache_total_bytes.fetch_sub(size, std::memory_order_relaxed);
                stats::CountPageCacheHit();
                return node;
            }
            bin.lock.clear(std::memory_order_release);
        }

        // 2) Best-fit split: a freed span of the WRONG size used to be
        //    unusable (page_cache_hits == 0 under mixed-size workloads,
        //    constant mmap/munmap cycling). Now take the smallest cached
        //    span >= the request, use its head, and park the tail pages in
        //    the exact-size bin for the next caller. Lock order is always
        //    high bin -> low bin, so concurrent splits cannot deadlock.
        std::size_t p = FirstNonEmptyAbove(pages);
        while (p != 0) {
            auto& bin = page_bins_[p];
            AcquireBinLock(bin.lock);
            if (bin.head) {
                PageNode* node = bin.head;
                bin.head = PageNodeLink(node);
                bin.count--;
                if (!bin.head) MarkMaybeEmpty(p);
                std::size_t span_bytes = p * page_size;
                page_cache_total_bytes.fetch_sub(span_bytes, std::memory_order_relaxed);

                std::size_t rem_pages = p - pages;
                if (rem_pages > 0) {
                    // Internal move: no retention increase (net bytes
                    // already subtracted above; the tail re-enters below).
                    // Per-bin retention caps are skipped for splits - the
                    // global high-water cap still bounds total retention.
                    // Lock order is strictly high bin -> low bin
                    // (rem_pages < p), so concurrent splits cannot deadlock.
                    auto& rbin = page_bins_[rem_pages];
                    PageNode* rem = reinterpret_cast<PageNode*>(
                        reinterpret_cast<char*>(node) + size);
                    // v13.3: re-publish the tail under its OWN identity.
                    // The popped span's kept map entries cover the WHOLE
                    // p-page range with desc=node; leaving them in place
                    // would orphan the tail's slots at eviction time (the
                    // head's Unmap never walks the tail's pages, and the
                    // tail's own Unmap only clears slots whose desc == the
                    // tail base) -> stale (node|kind) entries pointing at
                    // unmapped memory -> validator SEGV. Reproduced
                    // deterministically by a 2M-block live-set probe once
                    // the span pool (1 GB) exhausts and slab spans go
                    // direct-mmap. Own-identity + zero poison also keeps a
                    // wild free of the parked tail from routing anywhere.
                    {
                        const std::size_t rem_bytes = rem_pages * page_size;
                        SpanMap::Map(rem, rem_bytes, rem, SpanMap::kKindSlab,
                                     OSMemory::IsPoolBacked(rem));
                        *reinterpret_cast<std::uint64_t*>(rem) = 0; // poisoned
                    }
                    AcquireBinLock(rbin.lock);
                    PageNodeLink(rem) = rbin.head;
                    rbin.head = rem;
                    rbin.count++;
                    MarkNonEmpty(rem_pages);
                    rbin.lock.clear(std::memory_order_release);
                    page_cache_total_bytes.fetch_add(rem_pages * page_size,
                                                     std::memory_order_relaxed);
                    stats::CountPageCacheStore(rem_pages * page_size);
                }
                bin.lock.clear(std::memory_order_release);
                stats::CountPageCacheHit();
                return node;
            }
            MarkMaybeEmpty(p); // stale hint
            bin.lock.clear(std::memory_order_release);
            p = FirstNonEmptyAbove(p);
        }
    }
    return OSMemory::AllocatePages(size);
}

// ---------------------------------------------------------------------------
// v13 retire ring: deferred munmap for DIRECT-mmap spans.
//
// A lock-free span-map validator may have loaded a hash/radix entry for a
// span in the nanoseconds before PageHeapFree's SpanMap::Unmap cleared it.
// If that span is then munmap'd, the validator's header read faults on
// unmapped VA (the runner's intermittent SI_KERNEL crashes). Pooled spans
// are safe (FreePages keeps the mapping, madvise only - a stale read sees
// zero pages, fails the block_size sanity check, falls to the cleared
// radix and drops). DIRECT-mmap spans therefore defer their munmap by a
// grace window: the reader between 'entry load' and 'header read' (a few
// instructions) is given milliseconds to finish. Drain happens lazily on
// the next retire, and forcibly on purge. This matches production practice
// (jemalloc's retained extents, mimalloc's delayed segment free).
// ---------------------------------------------------------------------------
struct RetiredSpan {
    void* ptr;
    std::size_t bytes;
    std::uint64_t due_ns;
};
constexpr std::size_t kRetireCap = 128;
constexpr std::uint64_t kRetireGraceNs = 2'000'000; // 2 ms
RetiredSpan g_retired[kRetireCap];
std::size_t g_retired_n = 0;
std::atomic_flag g_retire_lock = ATOMIC_FLAG_INIT;

std::uint64_t RetireNowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

void DrainRetiredLocked(bool force) {
    const std::uint64_t now = force ? ~0ull : RetireNowNs();
    std::size_t w = 0;
    for (std::size_t i = 0; i < g_retired_n; ++i) {
        if (now < g_retired[i].due_ns) {
            g_retired[w++] = g_retired[i];
        } else {
            OSMemory::FreePages(g_retired[i].ptr, g_retired[i].bytes);
        }
    }
    g_retired_n = w;
}

// ---------------------------------------------------------------------------
// v13.6 dirty-span list: deferred, coalesced madvise for pooled spans.
//
// The overhead workload exposed the free-phase cost: its 1% large tail is
// ~10K individually-evicted pooled spans, and evict-madvise per span costs
// a syscall + TLB shootdown each (~12 us) - 137 ms of pure syscall time in
// one free phase, vs glibc's 53 ms (its 8..36KB objects live inside the
// brk heap: free touches metadata only, nothing returns to the OS).
//
// Deferred batching: evicted pooled spans go on a dirty list instead of
// being madvise'd on the spot. The list flushes (a) when it exceeds
// kDirtyThreshold bytes, (b) at purge, (c) when the entry array fills.
// A flush SORTS the entries and coalesces EXACTLY-ADJACENT ranges - spans
// carved consecutively from one 4 MB pool chunk are adjacent, so a flush
// of 64 MB typically issues a few dozen madvise calls of 1-4 MB instead
// of a thousand calls of ~56 KB. Coalescing never crosses a gap: a gap
// between two dirty spans is a span that is still live.
//
// Safety: a dirty span has left every page bin (evicted), the pool never
// re-uses freed span VA (bump-only carving), and its map entries were
// retracted by Unmap before eviction - nothing touches the memory between
// the free and the delayed madvise, and nothing can hand it out again.
// Retained-dirty is bounded by kDirtyThreshold (64 MB).
// ---------------------------------------------------------------------------
struct DirtySpan {
    char* base;
    std::size_t bytes;
};
constexpr std::size_t kDirtyCap = 16384;
constexpr std::size_t kDirtyThreshold = 64ull << 20;
DirtySpan g_dirty[kDirtyCap];
std::size_t g_dirty_n = 0;
std::size_t g_dirty_bytes = 0;
std::atomic_flag g_dirty_lock = ATOMIC_FLAG_INIT;

int DirtySpanCmp(const void* a, const void* b) {
    const DirtySpan* x = static_cast<const DirtySpan*>(a);
    const DirtySpan* y = static_cast<const DirtySpan*>(b);
    if (x->base < y->base) return -1;
    if (x->base > y->base) return 1;
    return 0;
}

void FlushDirtyLocked() {
    if (g_dirty_n == 0) return;
    std::qsort(g_dirty, g_dirty_n, sizeof(DirtySpan), DirtySpanCmp);
    std::size_t i = 0;
    while (i < g_dirty_n) {
        char* run_base = g_dirty[i].base;
        std::size_t run_bytes = g_dirty[i].bytes;
        std::size_t j = i + 1;
        // Coalesce exactly-adjacent ranges only; a gap is live memory.
        while (j < g_dirty_n &&
               run_base + run_bytes == g_dirty[j].base) {
            run_bytes += g_dirty[j].bytes;
            ++j;
        }
        OSMemory::FreePages(run_base, run_bytes); // madvise(DONTNEED)
        i = j;
    }
    g_dirty_n = 0;
    g_dirty_bytes = 0;
}

// Pooled spans: defer to the dirty list (bulk coalesced madvise).
// Direct-mmap spans: retire with a grace window (see the ring below).
void ReleaseSpanMemory(void* ptr, std::size_t bytes) {
    if (OSMemory::IsPoolBacked(ptr)) {
        AcquireBinLock(g_dirty_lock);
        if (g_dirty_n == kDirtyCap) FlushDirtyLocked(); // array full: flush now
        g_dirty[g_dirty_n].base = static_cast<char*>(ptr);
        g_dirty[g_dirty_n].bytes = bytes;
        ++g_dirty_n;
        g_dirty_bytes += bytes;
        if (g_dirty_bytes >= kDirtyThreshold) FlushDirtyLocked();
        g_dirty_lock.clear(std::memory_order_release);
        return;
    }
    AcquireBinLock(g_retire_lock);
    DrainRetiredLocked(false); // opportunistic: expired entries leave now
    if (g_retired_n == kRetireCap) {
        // Ring full and nothing expired: the oldest entry's grace is by far
        // the largest elapsed - release it to make room.
        OSMemory::FreePages(g_retired[0].ptr, g_retired[0].bytes);
        std::memmove(&g_retired[0], &g_retired[1],
                     (kRetireCap - 1) * sizeof(RetiredSpan));
        g_retired_n = kRetireCap - 1;
    }
    g_retired[g_retired_n].ptr = ptr;
    g_retired[g_retired_n].bytes = bytes;
    g_retired[g_retired_n].due_ns = RetireNowNs() + kRetireGraceNs;
    ++g_retired_n;
    g_retire_lock.clear(std::memory_order_release);
}

void PageHeapFree(void* ptr, std::size_t size) {
    const std::size_t page_size = OSMemory::GetPageSize();
#if FASTALLOC_DEBUG_ENABLED
    if (FAST_UNLIKELY(size == 0 || (size % page_size) != 0)) {
        ViolationInfo info{};
        info.kind = Violation::InternalInvariant;
        info.where = "PageHeapFree: size is not page-aligned";
        info.caller = FAST_RETURN_ADDRESS();
        ReportViolation(info);
    }
#endif
    std::size_t pages = size / page_size;
    if (pages <= MAX_CACHED_PAGES && pages > 0) {
        auto& bin = page_bins_[pages];
        // Global high-water mark: never let the whole cache exceed the cap
        // because of this insertion.
        std::size_t total = page_cache_total_bytes.fetch_add(size, std::memory_order_relaxed) + size;
        if (total <= PageCacheTotalLimit()) {
            AcquireBinLock(bin.lock);
            if (bin.count < BinRetentionCount(pages)) {
                // v13.1: the span KEEPS its map entries while parked in the
                // bin (mapped, header poisoned) - a cache round-trip then
                // costs O(1) instead of an O(pages) Unmap+Map pair (the
                // v13 'large' 4x regression: ~2 us per 512 KB round-trip).
                // Entries are retracted only when the memory truly leaves
                // (eviction / purge / release), and reuse re-publishes via
                // the cheap EntryMatches skip in AllocateLarge/AllocateNewSlab.
                SpanMap::PoisonParkedSpan(ptr);
                PageNode* node = static_cast<PageNode*>(ptr);
                PageNodeLink(node) = bin.head;
                bin.head = node;
                bin.count++;
                MarkNonEmpty(pages);
                bin.lock.clear(std::memory_order_release);
                stats::CountPageCacheStore(size);
                return;
            }
            bin.lock.clear(std::memory_order_release);
        }
        page_cache_total_bytes.fetch_sub(size, std::memory_order_relaxed);
        stats::CountPageCacheEvict(size);
    }
    // Real departure: retract the entries, then release the memory
    // (page-bin eviction above, oversized span, or cache over budget).
    SpanMap::Unmap(ptr, size);
    ReleaseSpanMemory(ptr, size);
}

std::size_t PurgePageCacheLocked() {
    std::size_t released = 0;
    const std::size_t page_size = OSMemory::GetPageSize();
    for (std::size_t p = 1; p <= MAX_CACHED_PAGES; ++p) {
        auto& bin = page_bins_[p];
        AcquireBinLock(bin.lock);
        PageNode* node = bin.head;
        bin.head = nullptr;
        bin.count = 0;
        MarkMaybeEmpty(p);
        bin.lock.clear(std::memory_order_release);
        while (node) {
            PageNode* next = PageNodeLink(node);
            std::size_t span_size = p * page_size;
            // Real departure: retract entries before the memory leaves.
            SpanMap::Unmap(node, span_size);
            ReleaseSpanMemory(node, span_size);
            page_cache_total_bytes.fetch_sub(span_size, std::memory_order_relaxed);
            stats::CountPageCacheEvict(span_size);
            released += span_size;
            node = next;
        }
    }
    // Purge semantics: give the memory back NOW. The grace window only ever
    // covered entries retired within the last 2 ms; a validator that loaded
    // an entry before its Unmap has long since finished its few-instruction
    // read (and the user explicitly asked for the memory back).
    AcquireBinLock(g_retire_lock);
    DrainRetiredLocked(true);
    g_retire_lock.clear(std::memory_order_release);
    // Flush the deferred dirty list as well: purge promises the OS got the
    // pages back (this is also where a workload's entire deferred tail is
    // released in a few coalesced calls).
    AcquireBinLock(g_dirty_lock);
    FlushDirtyLocked();
    g_dirty_lock.clear(std::memory_order_release);
    return released;
}

} // namespace

// ---------------------------------------------------------------------------
// Singleton
//
// The instance uses placement new into static storage and is intentionally
// not destroyed at exit. This avoids destruction-order issues with late
// thread-local destructors and removes heap allocations during bootstrap.
// ---------------------------------------------------------------------------
GlobalHeap& GlobalHeap::GetInstance() {
    alignas(GlobalHeap) static unsigned char storage[sizeof(GlobalHeap)];
    static GlobalHeap* instance = ::new (static_cast<void*>(storage)) GlobalHeap();
    return *instance;
}

GlobalHeap::GlobalHeap() : next_arena_(0) {
    // Mark shutdown at process exit so TLSCache destructors skip
    // reflushing memory into the global heap.
    std::atexit([]() {
        GlobalHeap::GetInstance().shutting_down_.store(true, std::memory_order_release);
    });
}

bool GlobalHeap::ShouldFailAllocationForTest() {
    int left = g_oom_fail_countdown.load(std::memory_order_relaxed);
    while (left > 0) {
        if (g_oom_fail_countdown.compare_exchange_weak(left, left - 1,
                                                       std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Test hooks (declared in tests/fast_alloc_test_hooks.h). External linkage,
// defined here next to the state they touch. Zero cost when unused.
// ---------------------------------------------------------------------------
void FastAllocTestSetOOMCountdown(int n) {
    g_oom_fail_countdown.store(n, std::memory_order_relaxed);
}
int FastAllocTestGetOOMCountdown() {
    return g_oom_fail_countdown.load(std::memory_order_relaxed);
}
std::size_t FastAllocTestPageCacheBytes() {
    return page_cache_total_bytes.load(std::memory_order_relaxed);
}
std::size_t FastAllocTestPurgePageCache() {
    return GlobalHeap::GetInstance().PurgePageCache();
}

static void PushSlab(Slab*& head, Slab* slab) {
    slab->next = head;
    slab->prev = nullptr;
    if (head) head->prev = slab;
    head = slab;
}

static void RemoveSlab(Slab*& head, Slab* slab) {
    if (slab->prev) slab->prev->next = slab->next;
    else head = slab->next;
    if (slab->next) slab->next->prev = slab->prev;
    slab->next = nullptr;
    slab->prev = nullptr;
}

// Empty-slab cache helpers (all require the (arena,class) lock held).

void GlobalHeap::ParkEmptySlabLocked(uint32_t arena_index, std::size_t class_index, Slab* slab) {
    Arena& arena = arenas_[arena_index];
    if (arena.empty_slab_counts_[class_index] >= kEmptySlabKeepPerClass ||
        g_empty_slab_bytes.load(std::memory_order_relaxed) + slab->memory_size >
            EmptySlabBytesCap()) {
        // Cache full / over budget: release the span as before.
        std::size_t freed_size = slab->memory_size; // capture BEFORE unmap
        PageHeapFree(slab, freed_size);
        stats::CountOsFree(freed_size);
        return;
    }
    // v13.1: parked-in-cache spans keep their map entries (empty-slab reuse
    // re-Creates the same Slab* on the same span, so the entry never goes
    // stale) - poison the validation field so a wild/double free that
    // routes here is rejected instead of pushing a block of an EMPTY slab
    // into a thread cache (double presence). Slab::Create rewrites the
    // header (and the memory_size field below stays intact for reuse).
    // v13.7: also re-mark the entries kKindSlabParked (see PageHeapFree).
    slab->block_size = 0; // poisoned: IsBlockStart rejects (d < ALIGNMENT)
    slab->next = arena.empty_slab_cache_[class_index];
    arena.empty_slab_cache_[class_index] = slab;
    arena.empty_slab_counts_[class_index]++;
    g_empty_slab_bytes.fetch_add(slab->memory_size, std::memory_order_relaxed);
}

Slab* GlobalHeap::TakeEmptySlabLocked(uint32_t arena_index, std::size_t class_index) {
    Arena& arena = arenas_[arena_index];
    Slab* slab = arena.empty_slab_cache_[class_index];
    if (!slab) return nullptr;
    arena.empty_slab_cache_[class_index] = slab->next;
    arena.empty_slab_counts_[class_index]--;
    g_empty_slab_bytes.fetch_sub(slab->memory_size, std::memory_order_relaxed);
    slab->next = nullptr;
    slab->prev = nullptr;
    return slab; // span still resident; caller re-creates the header on it
}

void GlobalHeap::ReleaseEmptySlabsOfClassLocked(uint32_t arena_index, std::size_t class_index) {
    Arena& arena = arenas_[arena_index];
    Slab* slab = arena.empty_slab_cache_[class_index];
    while (slab) {
        Slab* next = slab->next;
        std::size_t freed_size = slab->memory_size;
        PageHeapFree(slab, freed_size);
        stats::CountOsFree(freed_size);
        g_empty_slab_bytes.fetch_sub(freed_size, std::memory_order_relaxed);
        slab = next;
    }
    arena.empty_slab_cache_[class_index] = nullptr;
    arena.empty_slab_counts_[class_index] = 0;
}

Slab* GlobalHeap::AllocateNewSlab(std::size_t class_index, uint32_t arena_index) {
    std::size_t block_size = ClassIndexToSize(class_index);

    // 1) Reuse a resident empty slab: no madvise/mmap and no first-touch
    //    faults (the span never left the process). Slab::Create only
    //    rewrites the header page (release: lazy block wiring), so this is
    //    O(1) and touches one page.
    if (Slab* span = TakeEmptySlabLocked(arena_index, class_index)) {
        Slab* slab = Slab::Create(span, span->memory_size, block_size, arena_index);
        if (FAST_LIKELY(slab)) {
            stats::CountOsAlloc(slab->memory_size);
            // Map entries kept through the park (same span, same class -
            // the per-(arena,class) cache guarantees it, so the entry's
            // class bits stay correct); Slab::Create rewrote the header.
            return slab;
        }
        // Cannot happen (the same span served this exact class before), but
        // be robust: hand the span back to the page heap and continue.
        PageHeapFree(span, span->memory_size);
        stats::CountOsFree(span->memory_size);
    }

    std::size_t target_blocks = 256;
    if (block_size >= 1024) target_blocks = 128;
    if (block_size >= 4096) target_blocks = 32;

    std::size_t target_size = block_size * target_blocks;
    if (target_size < 65536) target_size = 65536;
    if (target_size > 2 * 1024 * 1024) target_size = 2 * 1024 * 1024;

    std::size_t page_size = OSMemory::GetPageSize();
    std::size_t memory_size = (target_size + page_size - 1) & ~(page_size - 1);

    // OOM injection seam (tests only; zero cost otherwise: one relaxed load).
    if (FAST_UNLIKELY(ShouldFailAllocationForTest())) {
        return nullptr;
    }

    void* memory = PageHeapAllocate(memory_size);
    if (!memory) return nullptr;

    stats::CountOsAlloc(memory_size);

    Slab* slab = Slab::Create(memory, memory_size, block_size, arena_index);
    if (FAST_UNLIKELY(!slab)) {
        // Slab::Create rejected the geometry (should not happen with our
        // call sites, but be robust): give the span back.
        PageHeapFree(memory, memory_size);
        stats::CountOsFree(memory_size);
        return nullptr;
    }
    // Publish the span in the span map so fast_free can route blocks of
    // this slab by ADDRESS (no block-header read). The release-store in
    // Map() publishes the freshly written slab header fields as well.
    // (Empty-slab reuse keeps the same Slab* on the same span - the entry
    // stays valid across re-Creates.) The pool flag is computed ONCE per
    // span (vs. per free in the old design) and gates per-thread caching.
    // v13.1: a span recycled from the page cache as the SAME role (slab of
    // the same base) usually kept a matching entry - skip the O(pages) Map.
    {
        const bool pooled = OSMemory::IsPoolBacked(memory);
        if (FAST_UNLIKELY(!SpanMap::EntryMatches(memory, memory, SpanMap::kKindSlab,
                                                 pooled,
                                                 static_cast<uint32_t>(class_index)))) {
            SpanMap::Map(memory, memory_size, slab, SpanMap::kKindSlab, pooled,
                         static_cast<uint32_t>(class_index));
        }
    }
#if FASTALLOC_LOGGING_ENABLED
    LogEvent(LogLevel::Debug, "slab-create", "slab=%p size=%zu class=%zu arena=%u",
             (void*)slab, memory_size, class_index, arena_index);
#endif
    return slab;
}

// Process a popped single-arena single-class pending list into its slabs.
// Returns memory, moves full->partial, frees empty slabs. Also the fallback
// for over-long pending lists (which keeps the queues short and the slab
// free lists warm with recently-faulted blocks).
namespace {

// Freelist link validation: validates a link before dereferencing.
// Freelist links reside in the user area of free blocks (FreeBlock::next).
// A link that fails the plausibility check (<64KB / <2^47 / 16-aligned)
// is treated as the end of the list and dropped to prevent corrupt traversals.
inline bool WalkLinkPlausible(FreeBlock* node, const char* where,
                               std::size_t class_index) {
    if (node == nullptr) return true; // list end: fine
    if (IsPlausibleHeapPtr(node)) return true;
    ReportWildFreelistLink(where, class_index,
                           reinterpret_cast<std::uintptr_t>(node));
    return false;
}

// Slab pointer validation: validates that the block's slab pointer is a
// plausible heap address and references a valid arena index within bounds.
inline bool BlockSlabUsable(FreeBlock* node, const char* where,
                            std::size_t class_index) {
    constexpr uint32_t kArenaCount = 16; // == GlobalHeap::NUM_ARENAS
    Slab* slab = node->slab;
    if (FAST_UNLIKELY(!IsPlausibleHeapPtr(slab) ||
                      slab->arena_index >= kArenaCount)) {
        ReportWildFreelistLink(where, class_index,
                               reinterpret_cast<std::uintptr_t>(slab));
        return false;
    }
    return true;
}

// Walk bound for every guarded freelist traversal: a legitimate queue can
// never exceed the pending-bytes cap in blocks (cap / min block size), so
// this bound only ever fires on cyclic/corrupt lists - the remainder is
// dropped and reported (leak-once discipline) instead of looping forever.
constexpr std::size_t kListWalkBound = 8u << 20;

// Splice-batch arena extraction: one guarded walk that routes each node to
// its own slab's arena. 'counts' may be null (callers that don't need the
// per-arena node counts).

} // namespace

void GlobalHeap::ProcessPendingListLocked(uint32_t arena_index, std::size_t class_index, FreeBlock* pending) {
    if (!pending) return;
    Arena& arena = arenas_[arena_index];
    std::size_t walked = 0;
    while (pending && walked < kListWalkBound) {
        // Validate node + slab metadata before the dereferences below (the
        // pending queue is fed by lock-free MPSC pushes; a corrupted link
        // must not reach slab->IsFull / Deallocate). The successor link is
        // validated as the next iteration's node.
        if (FAST_UNLIKELY(!WalkLinkPlausible(pending, "ProcessPendingList",
                                             class_index) ||
                          !BlockSlabUsable(pending, "ProcessPendingList(slab)",
                                           class_index))) {
            return;
        }
        FreeBlock* next = pending->next;
        Slab* slab = pending->slab;

#if FASTALLOC_DEBUG_ENABLED
        if (FAST_UNLIKELY(slab == nullptr ||
                          slab->magic != debug::SLAB_MAGIC)) {
            ViolationInfo info{};
            info.kind = Violation::BadSlabMagic;
            info.ptr = pending;
            info.where = "DrainPendingReturns: pending block has corrupt slab";
            info.caller = FAST_RETURN_ADDRESS();
            ReportViolation(info);
        }
#endif

        bool was_full = slab->IsFull();
        slab->Deallocate(pending);

        if (was_full) {
            RemoveSlab(arena.full_slabs_[class_index], slab);
            PushSlab(arena.partial_slabs_[class_index], slab);
        }

        if (slab->IsEmpty()) {
            RemoveSlab(arena.partial_slabs_[class_index], slab);
            ParkEmptySlabLocked(arena_index, class_index, slab);
        }
        pending = next;
        ++walked;
    }
    if (FAST_UNLIKELY(pending != nullptr)) {
        ReportWildFreelistLink("ProcessPendingList(walk bound)", class_index,
                               reinterpret_cast<std::uintptr_t>(pending));
    }
}

void GlobalHeap::DrainPendingReturns(uint32_t arena_index, std::size_t class_index) {
    Arena& arena = arenas_[arena_index];
    PendingList& pl = arena.pending_returns_[class_index];
    if (pl.len.load(std::memory_order_relaxed) == 0 &&
        pl.head.load(std::memory_order_relaxed) == nullptr) {
        return;
    }
    FreeBlock* pending = pl.head.exchange(nullptr, std::memory_order_acquire);
    if (!pending) {
        pl.len.store(0, std::memory_order_relaxed);
        return;
    }
    uint32_t n = pl.len.exchange(0, std::memory_order_relaxed);
    ProcessPendingListLocked(arena_index, class_index, pending);
    PendingBytesSub(static_cast<std::size_t>(n) * ClassIndexToSize(class_index));
}

void* GlobalHeap::AllocateBlock(std::size_t class_index, uint32_t arena_index) {
    Arena& arena = arenas_[arena_index];
    void* ptr = nullptr;

    {
        ScopedSpinLock lock(arena.class_locks_[class_index]);
        DrainPendingReturns(arena_index, class_index);

        Slab* slab = arena.partial_slabs_[class_index];
        if (slab) {
            ptr = slab->Allocate();
            if (slab->IsFull()) {
                RemoveSlab(arena.partial_slabs_[class_index], slab);
                PushSlab(arena.full_slabs_[class_index], slab);
            }
        } else {
            Slab* new_slab = AllocateNewSlab(class_index, arena_index);
            if (new_slab) {
                PushSlab(arena.partial_slabs_[class_index], new_slab);
                ptr = new_slab->Allocate();
                if (new_slab->IsFull()) {
                    RemoveSlab(arena.partial_slabs_[class_index], new_slab);
                    PushSlab(arena.full_slabs_[class_index], new_slab);
                }
            }
        }
    }

    return ptr;
}

FreeBlock* GlobalHeap::ExtractBlocksFromSlab(Slab* slab, std::size_t class_index, std::size_t target_count, std::size_t& actual_count, uint32_t arena_index,
                                             FreeBlock** out_tail) {
    FreeBlock* head = nullptr;
    FreeBlock* tail = nullptr;

    while (actual_count < target_count && !slab->IsFull()) {
        FreeBlock* block = static_cast<FreeBlock*>(slab->Allocate());
        if (FAST_UNLIKELY(!block || !IsPlausibleHeapPtr(block))) {
            // Stop extraction if slab returns null or an implausible address,
            // returning only valid extracted blocks to the caller.
            if (block) {
                ReportWildFreelistLink("ExtractBlocksFromSlab", class_index,
                                       reinterpret_cast<std::uintptr_t>(block));
            }
            break;
        }
        if (!head) {
            head = block;
            tail = block;
        } else {
            tail->next = block;
            tail = block;
        }
        actual_count++;
    }

    if (slab->IsFull()) {
        RemoveSlab(arenas_[arena_index].partial_slabs_[class_index], slab);
        PushSlab(arenas_[arena_index].full_slabs_[class_index], slab);
    }

    if (tail) tail->next = nullptr;
    if (out_tail) *out_tail = tail;
    return head;
}

// ----------------------------------------------------------------------------
// Feed from a pending queue: pop the whole queue, cut up to 'want' nodes off
// the head for the caller, and re-park the remainder with one CAS. Returns
// the taken segment [head..*taken_tail] (null-terminated). The remainder
// walk is bounded by the advisory queue length plus a margin so a corrupt
// cyclic list terminates (remainder dropped + reported, leak-once).
// The (arena,class) lock of the SOURCE queue must be held by the caller.
// ----------------------------------------------------------------------------
FreeBlock* GlobalHeap::FeedFromPendingLocked(PendingList& pl, std::size_t class_index,
                                             std::size_t want, std::size_t& taken_out,
                                             FreeBlock** taken_tail_out) {
    taken_out = 0;
    *taken_tail_out = nullptr;
    if (pl.len.load(std::memory_order_relaxed) == 0 &&
        pl.head.load(std::memory_order_relaxed) == nullptr) {
        return nullptr;
    }
    FreeBlock* pending = pl.head.exchange(nullptr, std::memory_order_acquire);
    if (!pending) {
        pl.len.store(0, std::memory_order_relaxed);
        return nullptr;
    }
    uint32_t qlen = pl.len.exchange(0, std::memory_order_relaxed);
    if (FAST_UNLIKELY(!IsPlausibleHeapPtr(pending))) {
        ReportWildFreelistLink("AllocateBatch(pending head)", class_index,
                               reinterpret_cast<std::uintptr_t>(pending));
        return nullptr;
    }

    // Cut walk: take up to 'want' nodes from the head.
    FreeBlock* split = pending;
    std::size_t taken = 1;
    bool rest_wild = false;
    while (taken < want) {
        FreeBlock* nxt = split->next;
        if (!nxt) break;
        if (FAST_UNLIKELY(!IsPlausibleHeapPtr(nxt))) {
            ReportWildFreelistLink("AllocateBatch(feed walk)", class_index,
                                   reinterpret_cast<std::uintptr_t>(nxt));
            rest_wild = true;
            break;
        }
        split = nxt;
        ++taken;
    }
    FreeBlock* rest = rest_wild ? nullptr : split->next;
    split->next = nullptr;
    taken_out = taken;
    *taken_tail_out = split;

    if (rest) {
        // Remainder walk: find its true tail for the re-park splice.
        std::size_t bound = (static_cast<std::size_t>(qlen) > taken
                                 ? static_cast<std::size_t>(qlen) - taken
                                 : 0) + (64u << 10);
        FreeBlock* rt = rest;
        std::size_t rn = 1;
        bool dropped = false;
        while (rt->next) {
            if (rn >= bound) { dropped = true; break; }
            FreeBlock* nxt = rt->next;
            if (FAST_UNLIKELY(!IsPlausibleHeapPtr(nxt))) { dropped = true; break; }
            rt = nxt;
            ++rn;
        }
        if (FAST_UNLIKELY(dropped)) {
            ReportWildFreelistLink("AllocateBatch(remainder)", class_index,
                                   reinterpret_cast<std::uintptr_t>(rt->next));
            rt->next = nullptr; // over-long or wild tail: drop it (leak, once)
        }
        // Re-park [rest..rt] with the lock-free CAS protocol.
        FreeBlock* old_head = pl.head.load(std::memory_order_relaxed);
        do {
            rt->next = old_head;
        } while (!pl.head.compare_exchange_weak(old_head, rest,
                                                std::memory_order_release,
                                                std::memory_order_relaxed));
        pl.len.fetch_add(static_cast<uint32_t>(rn), std::memory_order_relaxed);
    }
    return pending;
}

FreeBlock* GlobalHeap::AllocateBatch(std::size_t class_index, std::size_t target_count, std::size_t& actual_count, uint32_t arena_index,
                                     FreeBlock** out_tail, bool* out_foreign) {
    actual_count = 0;
    Arena& arena = arenas_[arena_index];
    FreeBlock* result = nullptr;
    FreeBlock* result_tail = nullptr;
    bool foreign = false;

    {
        ScopedSpinLock lock(arena.class_locks_[class_index]);

        // ------------------------------------------------------------------
        // 1) Direct-feed from this arena's pending queue.
        //
        // Blocks spliced here by overflow flushes / thread exits are
        // already free, single-class, single-arena: feed them straight to
        // the caller with zero slab work. This is what makes the O(1)
        // splice flush safe for mixed alloc/free workloads - the blocks
        // simply cycle TLS bin -> pending queue -> TLS bin.
        // ------------------------------------------------------------------
        result = FeedFromPendingLocked(arena.pending_returns_[class_index],
                                       class_index, target_count, actual_count,
                                       &result_tail);
        if (result) {
            PendingBytesSub(actual_count * ClassIndexToSize(class_index));
        }

        // ------------------------------------------------------------------
        // 2) Cross-arena steal. Arena assignment rotates round-robin, so the
        // next owner of arena A appears 16 spawns later; with random class
        // sets the LOCAL pending queue for a class is often empty while
        // another arena holds a stock of freed blocks for it. A single
        // try-lock per foreign arena cannot deadlock (never retried while
        // holding A). The advisory len counter skips empty queues without
        // taking the lock at all.
        // ------------------------------------------------------------------
        if (actual_count < target_count) {
            for (uint32_t off = 1; off < NUM_ARENAS && actual_count < target_count; ++off) {
                uint32_t a2 = (arena_index + off) % NUM_ARENAS;
                PendingList& pl2 = arenas_[a2].pending_returns_[class_index];
                if (pl2.len.load(std::memory_order_relaxed) == 0) continue; // cheap skip
                auto& lk = arenas_[a2].class_locks_[class_index];
                if (lk.test_and_set(std::memory_order_acquire)) continue; // busy: skip
                std::size_t taken2 = 0;
                FreeBlock* tail2 = nullptr;
                FreeBlock* p2 = FeedFromPendingLocked(pl2, class_index,
                                                      target_count - actual_count,
                                                      taken2, &tail2);
                if (p2) {
                    PendingBytesSub(taken2 * ClassIndexToSize(class_index));
                    if (!result) {
                        result = p2;
                        result_tail = tail2;
                        actual_count = taken2;
                    } else {
                        // O(1) append (both segments' tails are exact).
                        result_tail->next = p2;
                        result_tail = tail2;
                        actual_count += taken2;
                    }
                    foreign = true; // foreign-arena blocks entered the batch
                }
                lk.clear(std::memory_order_release);
            }
        }

        // ------------------------------------------------------------------
        // 3) Slab extraction: warm freelist blocks first (returned by the
        // drain paths), virgin carving only as the last resort.
        // ------------------------------------------------------------------
        if (!result) {
            Slab* slab = arena.partial_slabs_[class_index];
            if (slab) {
                result = ExtractBlocksFromSlab(slab, class_index, target_count, actual_count, arena_index, &result_tail);
            } else {
                Slab* new_slab = AllocateNewSlab(class_index, arena_index);
                if (new_slab) {
                    PushSlab(arena.partial_slabs_[class_index], new_slab);
                    result = ExtractBlocksFromSlab(new_slab, class_index, target_count, actual_count, arena_index, &result_tail);
                }
            }
        }
    }

    if (out_tail) *out_tail = result_tail;
    if (out_foreign) *out_foreign = foreign;
    return result;
}

void GlobalHeap::DeallocateBlock(Slab* slab, void* ptr) {
    // Compatibility path (not used by the TLS fast paths): route the block
    // to its owning arena's pending queue with the O(1) splice so any
    // caller benefits from the deferred drain as well.
    (void)slab;
    FreeBlock* block = static_cast<FreeBlock*>(ptr);
    // Validate block address and slab metadata before modifying block fields.
    if (FAST_UNLIKELY(!IsPlausibleHeapPtr(block) || !BlockSlabUsable(
            block, "GlobalHeap::DeallocateBlock(slab)", block->class_index))) {
        return;
    }
    block->next = nullptr;
    // The arena index rides the slab header (hot line shared by the slab's
    // blocks); the block header itself is no longer consulted for routing.
    uint32_t hint = ArenaToHint(block->slab->arena_index);
    if (hint >= 1 && hint <= NUM_ARENAS) {
        SpliceToPending(HintToArena(hint), block->class_index, block, block, 1);
        EnforcePendingCap(HintToArena(hint), block->class_index);
    } else {
        DeallocateBatchClass(block->class_index, block);
    }
}

// ----------------------------------------------------------------------------
// O(1) splice plumbing (v12)
// ----------------------------------------------------------------------------

void GlobalHeap::SpliceToPending(uint32_t arena_index, std::size_t class_index,
                                 FreeBlock* head, FreeBlock* tail, std::size_t count) {
    PendingList& pl = arenas_[arena_index].pending_returns_[class_index];
    FreeBlock* old_head = pl.head.load(std::memory_order_relaxed);
    do {
        tail->next = old_head;
    } while (!pl.head.compare_exchange_weak(old_head, head,
                                            std::memory_order_release,
                                            std::memory_order_relaxed));
    pl.len.fetch_add(static_cast<uint32_t>(count), std::memory_order_relaxed);
    g_pending_bytes.fetch_add(count * ClassIndexToSize(class_index),
                              std::memory_order_relaxed);
    stats::CountPendingPush();
}

void GlobalHeap::EnforcePendingCap(uint32_t arena_index, std::size_t class_index) {
    if (g_pending_bytes.load(std::memory_order_relaxed) <= PendingBytesCap()) {
        return;
    }
    auto& lk = arenas_[arena_index].class_locks_[class_index];
    if (lk.test_and_set(std::memory_order_acquire)) return; // someone else drains
    PendingList& pl = arenas_[arena_index].pending_returns_[class_index];
    FreeBlock* p = pl.head.exchange(nullptr, std::memory_order_acquire);
    if (p) {
        uint32_t n = pl.len.exchange(0, std::memory_order_relaxed);
        ProcessPendingListLocked(arena_index, class_index, p);
        PendingBytesSub(static_cast<std::size_t>(n) * ClassIndexToSize(class_index));
    } else {
        pl.len.store(0, std::memory_order_relaxed);
    }
    lk.clear(std::memory_order_release);
}

void GlobalHeap::FlushBinToPending(std::size_t class_index, FreeBlock* head, FreeBlock* tail,
                                   std::size_t count, uint32_t hint, uint32_t my_arena) {
    (void)my_arena;
    if (!head) return;
    if (FAST_UNLIKELY(!tail || tail->next != nullptr)) {
        // Tail invariant broken (corruption upstream): fall back to the
        // guarded, self-healing arena-split path.
        DeallocateBatchClass(class_index, head);
        return;
    }
    if (hint >= 1 && hint <= NUM_ARENAS) {
        // Single-arena bin: zero-touch Treiber splice.
        uint32_t arena = HintToArena(hint);
        SpliceToPending(arena, class_index, head, tail, count);
        EnforcePendingCap(arena, class_index);
        return;
    }
    // Mixed / unknown: guarded split by each block's own slab arena.
    FreeBlock* heads[NUM_ARENAS];
    FreeBlock* tails_[NUM_ARENAS];
    std::size_t counts[NUM_ARENAS];
    SplitListByArena(head, heads, tails_, counts);
    for (uint32_t i = 0; i < NUM_ARENAS; ++i) {
        if (!heads[i]) continue;
        SpliceToPending(i, class_index, heads[i], tails_[i], counts[i]);
        EnforcePendingCap(i, class_index);
    }
}
// ---------------------------------------------------------------------------
// Single-class batch return (TLS overflow flush) and lock-free deferred
// return (thread exit).
//
// Both share the arena split: blocks in one TLS bin usually belong to the
// owning thread's arena, but cross-thread frees can mix arenas in, so the
// split is per-block via each block's own slab. Only a 16-entry stack table
// is touched - the 512-entry per-class tables of the general path are
// unnecessary because the class is known.
// ---------------------------------------------------------------------------
void GlobalHeap::SplitListByArena(FreeBlock* head, FreeBlock** heads, FreeBlock** tails,
                                  std::size_t* counts) {
    for (uint32_t i = 0; i < NUM_ARENAS; ++i) {
        heads[i] = nullptr;
        tails[i] = nullptr;
        if (counts) counts[i] = 0;
    }
    std::size_t walked = 0;
    while (head && walked < kListWalkBound) {
        // Stop at the first invalid link and drop blocks with invalid slab metadata.
        if (FAST_UNLIKELY(!WalkLinkPlausible(head, "SplitListByArena", 0) ||
                          !BlockSlabUsable(head, "SplitListByArena(slab)", 0))) {
            return;
        }
        FreeBlock* next = head->next;
        uint32_t arena_index = head->slab->arena_index;
        if (!heads[arena_index]) {
            heads[arena_index] = head;
            tails[arena_index] = head;
        } else {
            tails[arena_index]->next = head;
            tails[arena_index] = head;
        }
        head->next = nullptr;
        if (counts) counts[arena_index]++;
        head = next;
        ++walked;
    }
    if (FAST_UNLIKELY(head != nullptr)) {
        ReportWildFreelistLink("SplitListByArena(walk bound)", 0,
                               reinterpret_cast<std::uintptr_t>(head));
    }
}

void GlobalHeap::DeferBinToPending(std::size_t class_index, FreeBlock* head) {
    if (!head) return;
    FreeBlock* heads[NUM_ARENAS];
    FreeBlock* tails[NUM_ARENAS];
    std::size_t counts[NUM_ARENAS];
    SplitListByArena(head, heads, tails, counts);
    for (uint32_t i = 0; i < NUM_ARENAS; ++i) {
        if (!heads[i]) continue;
        SpliceToPending(i, class_index, heads[i], tails[i], counts[i]);
    }
}

void GlobalHeap::DeallocateBatchClass(std::size_t class_index, FreeBlock* head) {
    if (!head) return;
    FreeBlock* heads[NUM_ARENAS];
    FreeBlock* tails[NUM_ARENAS];
    SplitListByArena(head, heads, tails);

    for (uint32_t i = 0; i < NUM_ARENAS; ++i) {
        FreeBlock* list = heads[i];
        if (!list) continue;
        Arena& arena = arenas_[i];

        if (arena.class_locks_[class_index].test_and_set(std::memory_order_acquire)) {
            // Contended: lock-free MPSC hand-off.
            FreeBlock* tail = tails[i];
            PendingList& pl = arena.pending_returns_[class_index];
            FreeBlock* old_head = pl.head.load(std::memory_order_relaxed);
            do {
                tail->next = old_head;
            } while (!pl.head.compare_exchange_weak(old_head, list,
                                                    std::memory_order_release,
                                                    std::memory_order_relaxed));
            stats::CountPendingPush();
        } else {
            ProcessBatchLocked(i, class_index, list);
            arena.class_locks_[class_index].clear(std::memory_order_release);
        }
    }
}

// ---------------------------------------------------------------------------
// DeallocateBatch
//
// The incoming list is split by (arena, class) using each block's own slab
// metadata. Routing tables are stack-resident (NUM_ARENAS * 2 pointers)
// plus a per-arena 512-entry bucket pass without heap allocation.
// ---------------------------------------------------------------------------
void GlobalHeap::ProcessBatchLocked(uint32_t arena_index, std::size_t class_index, FreeBlock* list) {
    Arena& arena = arenas_[arena_index];
    FreeBlock* curr = list;
    std::size_t walked = 0;
    while (curr && walked < kListWalkBound) {
        // Validate the node and its slab metadata before dereferencing;
        // a wild node or unusable slab ends the batch here.
        if (FAST_UNLIKELY(!WalkLinkPlausible(curr, "ProcessBatchLocked",
                                             class_index) ||
                          !BlockSlabUsable(curr, "ProcessBatchLocked(slab)",
                                           class_index))) {
            return;
        }
        FreeBlock* next = curr->next;
        Slab* slab = curr->slab;
        bool was_full = slab->IsFull();
        slab->Deallocate(curr);

        if (was_full) {
            RemoveSlab(arena.full_slabs_[class_index], slab);
            PushSlab(arena.partial_slabs_[class_index], slab);
        }

        if (slab->IsEmpty()) {
            RemoveSlab(arena.partial_slabs_[class_index], slab);
            ParkEmptySlabLocked(arena_index, class_index, slab);
        }
        curr = next;
        ++walked;
    }
    if (FAST_UNLIKELY(curr != nullptr)) {
        ReportWildFreelistLink("ProcessBatchLocked(walk bound)", class_index,
                               reinterpret_cast<std::uintptr_t>(curr));
        return;
    }
    DrainPendingReturns(arena_index, class_index);
}

void GlobalHeap::DeallocateBatch(FreeBlock* head) {
    if (!head) return;

    // Phase 1: split into per-arena lists (stack tables, no allocation).
    std::array<FreeBlock*, NUM_ARENAS> arena_heads{};
    std::array<FreeBlock*, NUM_ARENAS> arena_tails{};

    while (head) {
        // Validate node and slab metadata before dereferencing head->slab->arena_index.
        if (FAST_UNLIKELY(!WalkLinkPlausible(head, "DeallocateBatch", 0) ||
                          !BlockSlabUsable(head, "DeallocateBatch(slab)", 0))) {
            return;
        }
        FreeBlock* next = head->next;
        if (FAST_UNLIKELY(!WalkLinkPlausible(next, "DeallocateBatch(next)", 0))) {
            next = nullptr;
        }
        uint32_t arena_index = head->slab->arena_index;

        if (!arena_heads[arena_index]) {
            arena_heads[arena_index] = head;
            arena_tails[arena_index] = head;
        } else {
            arena_tails[arena_index]->next = head;
            arena_tails[arena_index] = head;
        }
        head->next = nullptr;
        head = next;
    }

    // Phase 2: within each arena, split by size class using per-block metadata,
    // then push each class run to the pending queue or process directly
    // if the class lock is free.
    for (uint32_t i = 0; i < NUM_ARENAS; ++i) {
        if (!arena_heads[i]) continue;

        std::array<FreeBlock*, NUM_SIZE_CLASSES> cls_heads{};
        std::array<FreeBlock*, NUM_SIZE_CLASSES> cls_tails{};
        std::array<uint16_t, NUM_SIZE_CLASSES> present_idx{}; // classes seen (pack class list)
        std::size_t present_count = 0;

        FreeBlock* curr = arena_heads[i];
        while (curr) {
            // Validate link and slab metadata before indexing class tables.
            if (FAST_UNLIKELY(!WalkLinkPlausible(curr, "DeallocateBatch(class)", 0) ||
                              !BlockSlabUsable(curr, "DeallocateBatch(class slab)", 0))) {
                curr = nullptr;
                break;
            }
            FreeBlock* next = curr->next; // save: we rewire curr below
            if (FAST_UNLIKELY(!WalkLinkPlausible(next, "DeallocateBatch(class next)", 0))) {
                next = nullptr;
            }
            std::size_t cls = SizeToClassIndex(curr->slab->block_size);
            if (!cls_heads[cls]) {
                cls_heads[cls] = curr;
                cls_tails[cls] = curr;
                if (present_count < NUM_SIZE_CLASSES) {
                    present_idx[present_count] = static_cast<uint16_t>(cls);
                    ++present_count;
                }
            } else {
                cls_tails[cls]->next = curr;
                cls_tails[cls] = curr;
            }
            curr->next = nullptr;
            curr = next;
        }

        for (std::size_t pi = 0; pi < present_count; ++pi) {
            std::size_t class_index = present_idx[pi];
            FreeBlock* list = cls_heads[class_index];
            Arena& arena = arenas_[i];

            if (arena.class_locks_[class_index].test_and_set(std::memory_order_acquire)) {
                // Contended: lock-free MPSC handoff (unchanged protocol).
                FreeBlock* tail = cls_tails[class_index];
                FreeBlock* old_head = arena.pending_returns_[class_index].head.load(std::memory_order_relaxed);
                do {
                    tail->next = old_head;
                } while (!arena.pending_returns_[class_index].head.compare_exchange_weak(
                             old_head, list, std::memory_order_release, std::memory_order_relaxed));
                stats::CountPendingPush();
            } else {
                ProcessBatchLocked(i, class_index, list);
                arena.class_locks_[class_index].clear(std::memory_order_release);
            }
        }
    }
}

void* GlobalHeap::AllocateLarge(std::size_t size) {
    std::size_t page_size = OSMemory::GetPageSize();
    std::size_t alloc_size = (size + page_size - 1) & ~(page_size - 1);
    if (FAST_UNLIKELY(ShouldFailAllocationForTest())) return nullptr;
    void* p = PageHeapAllocate(alloc_size);
    if (p) {
        stats::CountOsAlloc(alloc_size);
        // Publish as a large span: the descriptor is the span base, where
        // AllocateLargeCached writes the LargeAllocHeader immediately after.
        // v13.1: a span recycled from the page cache usually kept its large
        // identity (same base, same kind, same pool flag) - the entry check
        // skips the O(pages) Map walk. Mismatch (new role, or a colliding
        // entry) takes the full Map.
        const bool pooled = OSMemory::IsPoolBacked(p);
        if (FAST_UNLIKELY(!SpanMap::EntryMatches(p, p, SpanMap::kKindLarge, pooled))) {
            SpanMap::Map(p, alloc_size, p, SpanMap::kKindLarge, pooled);
        }
#if FASTALLOC_DEBUG_ENABLED
        // A page-cache span may have previously served as a SLAB: its first
        // and last pages then carry slab/block metadata (pointers, counts,
        // free-list links), which the large-allocation reuse check would
        // misread as a use-after-free write. Pre-fill the sampled windows
        // with the FRESH pattern so the check stays exact for user data.
        // (Spans served from the thread-local large cache were poisoned at
        // free time and do not take this path.)
        {
            unsigned char* base = static_cast<unsigned char*>(p);
            std::size_t window = alloc_size < 1024 ? alloc_size : 1024;
            std::memset(base, debug::FRESH, window);
            std::memset(base + alloc_size - window, debug::FRESH, window);
        }
#endif
    }
    return p;
}

void GlobalHeap::DeallocateLarge(void* ptr, std::size_t size) {
    std::size_t page_size = OSMemory::GetPageSize();
    std::size_t alloc_size = (size + page_size - 1) & ~(page_size - 1);
    PageHeapFree(ptr, alloc_size);
    stats::CountOsFree(alloc_size);
}

std::size_t GlobalHeap::PurgePageCache() {
    // Step 1: drain every pending-return queue first. Deferred (lock-free)
    // hand-offs park freed blocks without doing slab work; if nobody ever
    // refills those classes again the blocks - and the slabs they came
    // from, which cannot empty while the blocks are "out" - stay resident
    // forever. Draining here returns them to the slabs so step 2 can free
    // the emptied spans. This is what fast_alloc_purge() promises: "drop
    // every cached span back to the OS".
    // Step 1b: release every retained empty slab the drains produced (and
    // every one parked earlier) - purge returns EVERYTHING.
    for (uint32_t a = 0; a < NUM_ARENAS; ++a) {
        Arena& arena = arenas_[a];
        for (std::size_t c = 0; c < NUM_SIZE_CLASSES; ++c) {
            ScopedSpinLock lock(arena.class_locks_[c]);
            DrainPendingReturns(a, c);
            ReleaseEmptySlabsOfClassLocked(a, c);
        }
    }
    // Step 2: drop every cached span back to the OS.
    return PurgePageCacheLocked();
}

// ===========================================================================
// Pointer-array diagnostic scan (see fast_alloc.h).
// Reads head arrays across arena slab lists, pending queues, and page bins.
// Performs read-only inspections without foreign dereferences.
// ===========================================================================
std::size_t GlobalHeap::AuditPointers(const void* lo, const void* hi) {
    const std::uintptr_t low = reinterpret_cast<std::uintptr_t>(lo);
    const std::uintptr_t high = reinterpret_cast<std::uintptr_t>(hi);
    std::size_t hits = 0;
    for (uint32_t a = 0; a < NUM_ARENAS; ++a) {
        Arena& arena = arenas_[a];
        for (std::size_t c = 0; c < NUM_SIZE_CLASSES; ++c) {
            const Slab* partial = arena.partial_slabs_[c];
            if (reinterpret_cast<std::uintptr_t>(partial) >= low &&
                reinterpret_cast<std::uintptr_t>(partial) < high) {
                std::fprintf(stderr,
                             "[fastalloc-audit] arena %u class %zu partial_slabs head=%p -> IN RANGE\n",
                             a, c, const_cast<Slab*>(partial));
                ++hits;
            }
            const Slab* full = arena.full_slabs_[c];
            if (reinterpret_cast<std::uintptr_t>(full) >= low &&
                reinterpret_cast<std::uintptr_t>(full) < high) {
                std::fprintf(stderr,
                             "[fastalloc-audit] arena %u class %zu full_slabs head=%p -> IN RANGE\n",
                             a, c, const_cast<Slab*>(full));
                ++hits;
            }
            const FreeBlock* pending =
                arena.pending_returns_[c].head.load(std::memory_order_relaxed);
            if (reinterpret_cast<std::uintptr_t>(pending) >= low &&
                reinterpret_cast<std::uintptr_t>(pending) < high) {
                std::fprintf(stderr,
                             "[fastalloc-audit] arena %u class %zu pending head=%p -> IN RANGE\n",
                             a, c, static_cast<void*>(const_cast<FreeBlock*>(pending)));
                ++hits;
            }
        }
    }
    for (std::size_t p = 1; p <= MAX_CACHED_PAGES; ++p) {
        const PageNode* node = page_bins_[p].head;
        if (reinterpret_cast<std::uintptr_t>(node) >= low &&
            reinterpret_cast<std::uintptr_t>(node) < high) {
            std::fprintf(stderr,
                         "[fastalloc-audit] page bin %zu head=%p -> IN RANGE\n",
                         p, static_cast<void*>(const_cast<PageNode*>(node)));
            ++hits;
        }
    }
    return hits;
}

} // namespace FastAlloc

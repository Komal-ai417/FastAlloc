#include "global_heap.h"
#include "os_memory.h"
#include "debug_aid.h"
#include <algorithm>
#include <cstring>
#include <thread>
#include <cstdlib>

namespace FastAlloc {

namespace {

// ===========================================================================
// Page-span cache (C5 fix)
//
// OLD policy: per-bin cap of (16 MB / span_size) with floor 8, no global
// limit -> worst-case retention of 513 x 16 MB ~= 8.2 GB of anonymous memory
// that was never returned to the OS.
//
// NEW policy:
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

struct PageNode {
    PageNode* next;
};
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
    // Audit M1: the page-multiple contract is now actually asserted.
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
                bin.head = node->next;
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
                bin.head = node->next;
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
                    AcquireBinLock(rbin.lock);
                    rem->next = rbin.head;
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
                PageNode* node = static_cast<PageNode*>(ptr);
                node->next = bin.head;
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
    OSMemory::FreePages(ptr, size);
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
            PageNode* next = node->next;
            std::size_t span_size = p * page_size;
            OSMemory::FreePages(node, span_size);
            page_cache_total_bytes.fetch_sub(span_size, std::memory_order_relaxed);
            stats::CountPageCacheEvict(span_size);
            released += span_size;
            node = next;
        }
    }
    return released;
}

} // namespace

// ---------------------------------------------------------------------------
// Singleton (audit C7 fix)
//
// The instance is intentionally LEAKED: it is constructed on first use and
// never destroyed. A destroyed function-local static could be accessed by a
// late TLS destructor at process shutdown (unspecified destruction order
// across translation units) -> use-after-destroy. Leaking is the standard
// allocator practice: the OS reclaims everything at exit anyway.
// ---------------------------------------------------------------------------
GlobalHeap& GlobalHeap::GetInstance() {
    static GlobalHeap* instance = new GlobalHeap(); // NOLINT: intentional leak
    return *instance;
}

GlobalHeap::GlobalHeap() : next_arena_(0) {
    // N3/C7: mark shutdown as soon as exit begins. TLSCache destructors then
    // skip reflushing into the global heap, which removes both the
    // destruction-order hazard and the Windows killed-thread-holding-a-lock
    // deadlock at process exit.
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

Slab* GlobalHeap::AllocateNewSlab(std::size_t class_index, uint32_t arena_index) {
    std::size_t block_size = ClassIndexToSize(class_index);
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
void GlobalHeap::ProcessPendingListLocked(uint32_t arena_index, std::size_t class_index, FreeBlock* pending) {
    if (!pending) return;
    Arena& arena = arenas_[arena_index];
    while (pending) {
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
            std::size_t freed_size = slab->memory_size; // capture BEFORE unmap
            PageHeapFree(slab, freed_size);
            stats::CountOsFree(freed_size);
        }
        pending = next;
    }
}

void GlobalHeap::DrainPendingReturns(uint32_t arena_index, std::size_t class_index) {
    Arena& arena = arenas_[arena_index];
    if (arena.pending_returns_[class_index].head.load(std::memory_order_relaxed) == nullptr) return;
    FreeBlock* pending = arena.pending_returns_[class_index].head.exchange(nullptr, std::memory_order_acquire);
    if (!pending) return;
    ProcessPendingListLocked(arena_index, class_index, pending);
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

FreeBlock* GlobalHeap::ExtractBlocksFromSlab(Slab* slab, std::size_t class_index, std::size_t target_count, std::size_t& actual_count, uint32_t arena_index) {
    FreeBlock* head = nullptr;
    FreeBlock* tail = nullptr;

    while (actual_count < target_count && !slab->IsFull()) {
        FreeBlock* block = static_cast<FreeBlock*>(slab->Allocate());
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
    return head;
}

FreeBlock* GlobalHeap::AllocateBatch(std::size_t class_index, std::size_t target_count, std::size_t& actual_count, uint32_t arena_index) {
    actual_count = 0;
    Arena& arena = arenas_[arena_index];
    FreeBlock* result = nullptr;

    constexpr std::size_t kFeedMaxList = 64; // bounded pending-walk threshold

    {
        ScopedSpinLock lock(arena.class_locks_[class_index]);

        // ------------------------------------------------------------------
        // 1) Direct-feed from this arena's pending queue.
        //
        // Blocks parked in pending (thread-exit defers / contended flushes)
        // are already free, single-class, single-arena: feed them straight
        // to the caller with zero slab work. Short remainders are re-parked
        // (bounded walk); OVER-LONG remainders are drained into the slabs
        // instead, which (a) keeps every pending queue O(64) and (b) puts
        // recently-faulted WARM blocks back on the slab free list, so the
        // extraction path below stops carving virgin (page-faulting) space.
        // ------------------------------------------------------------------
        PendingList& pl = arena.pending_returns_[class_index];
        FreeBlock* pending = pl.head.exchange(nullptr, std::memory_order_acquire);
        if (pending) {
            FreeBlock* split = pending;
            std::size_t taken = 1;
            while (taken < target_count && split->next) {
                split = split->next;
                ++taken;
            }
            FreeBlock* rest_head = split->next;
            split->next = nullptr;
            result = pending;
            actual_count = taken;
            if (rest_head) {
                // Measure the remainder with a hard cap.
                FreeBlock* tail = rest_head;
                std::size_t len = 1;
                while (tail->next && len < kFeedMaxList) {
                    tail = tail->next;
                    ++len;
                }
                if (tail->next == nullptr) {
                    // Short: re-park via the lock-free CAS protocol.
                    FreeBlock* old_head = pl.head.load(std::memory_order_relaxed);
                    do {
                        tail->next = old_head;
                    } while (!pl.head.compare_exchange_weak(old_head, rest_head,
                                                             std::memory_order_release,
                                                             std::memory_order_relaxed));
                } else {
                    // Over-long: drain into the slabs (warm free lists).
                    ProcessPendingListLocked(arena_index, class_index, rest_head);
                }
            }
        }

        // ------------------------------------------------------------------
        // 2) Cross-arena steal. Arena assignment rotates round-robin, so the
        // next owner of arena A appears 16 spawns later; with random class
        // sets the LOCAL pending queue for a class is often empty while
        // another arena holds a stock of freed blocks for it. Without the
        // steal, ~half of all first-misses carve virgin slab pages (first-
        // touch faults ~1.5-3 us each on this VM). A single try-lock per
        // foreign arena cannot deadlock (never retried while holding A).
        // ------------------------------------------------------------------
        if (actual_count < target_count) {
            for (uint32_t off = 1; off < NUM_ARENAS && actual_count < target_count; ++off) {
                uint32_t a2 = (arena_index + off) % NUM_ARENAS;
                auto& lk = arenas_[a2].class_locks_[class_index];
                if (lk.test_and_set(std::memory_order_acquire)) continue; // busy: skip
                PendingList& pl2 = arenas_[a2].pending_returns_[class_index];
                FreeBlock* p2 = pl2.head.exchange(nullptr, std::memory_order_acquire);
                if (p2) {
                    FreeBlock* split2 = p2;
                    std::size_t taken2 = 1;
                    while (actual_count + taken2 < target_count && split2->next) {
                        split2 = split2->next;
                        ++taken2;
                    }
                    FreeBlock* rest2 = split2->next;
                    split2->next = nullptr;
                    // Append the stolen batch to the result (result is short).
                    if (!result) {
                        result = p2;
                        actual_count = taken2;
                    } else {
                        FreeBlock* tailr = result;
                        while (tailr->next) tailr = tailr->next;
                        tailr->next = p2;
                        actual_count += taken2;
                    }
                    if (rest2) {
                        ProcessPendingListLocked(a2, class_index, rest2);
                    }
                }
                lk.clear(std::memory_order_release);
            }
        }

        // ------------------------------------------------------------------
        // 3) Slab extraction: warm freelist blocks first (returned by the
        // drain above), virgin carving only as the last resort.
        // ------------------------------------------------------------------
        if (!result) {
            Slab* slab = arena.partial_slabs_[class_index];
            if (slab) {
                result = ExtractBlocksFromSlab(slab, class_index, target_count, actual_count, arena_index);
            } else {
                Slab* new_slab = AllocateNewSlab(class_index, arena_index);
                if (new_slab) {
                    PushSlab(arena.partial_slabs_[class_index], new_slab);
                    result = ExtractBlocksFromSlab(new_slab, class_index, target_count, actual_count, arena_index);
                }
            }
        }
    }

    return result;
}

void GlobalHeap::DeallocateBlock(Slab* slab, void* ptr) {
    (void)slab;
    FreeBlock* block = static_cast<FreeBlock*>(ptr);
    block->next = nullptr;
    DeallocateBatchClass(block->class_index, block);
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
void GlobalHeap::SplitListByArena(FreeBlock* head, FreeBlock** heads, FreeBlock** tails) {
    for (uint32_t i = 0; i < NUM_ARENAS; ++i) { heads[i] = nullptr; tails[i] = nullptr; }
    while (head) {
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
        head = next;
    }
}

void GlobalHeap::DeferBinToPending(std::size_t class_index, FreeBlock* head) {
    if (!head) return;
    FreeBlock* heads[NUM_ARENAS];
    FreeBlock* tails[NUM_ARENAS];
    SplitListByArena(head, heads, tails);

    for (uint32_t i = 0; i < NUM_ARENAS; ++i) {
        FreeBlock* list = heads[i];
        if (!list) continue;
        FreeBlock* tail = tails[i];
        // Lock-free MPSC hand-off (same protocol as the contended path of
        // DeallocateBatch; established by the TSan soak evidence).
        PendingList& pl = arenas_[i].pending_returns_[class_index];
        FreeBlock* old_head = pl.head.load(std::memory_order_relaxed);
        do {
            tail->next = old_head;
        } while (!pl.head.compare_exchange_weak(old_head, list,
                                                std::memory_order_release,
                                                std::memory_order_relaxed));
        stats::CountPendingPush();
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
// DeallocateBatch (audit N2 fix)
//
// The incoming list is split by (arena, class) using EACH BLOCK'S OWN slab
// metadata. Previously the class was derived from the first block only,
// which silently relied on every caller passing single-class lists. The
// routing tables are stack-resident (NUM_ARENAS * 2 pointers) plus a
// per-arena 512-entry bucket pass, so no heap allocation occurs inside the
// allocator.
// ---------------------------------------------------------------------------
void GlobalHeap::ProcessBatchLocked(uint32_t arena_index, std::size_t class_index, FreeBlock* list) {
    Arena& arena = arenas_[arena_index];
    FreeBlock* curr = list;
    while (curr) {
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
            std::size_t freed_size = slab->memory_size; // capture BEFORE unmap
            PageHeapFree(slab, freed_size);
            stats::CountOsFree(freed_size);
        }
        curr = next;
    }
    DrainPendingReturns(arena_index, class_index);
}

void GlobalHeap::DeallocateBatch(FreeBlock* head) {
    if (!head) return;

    // Phase 1: split into per-arena lists (stack tables, no allocation).
    std::array<FreeBlock*, NUM_ARENAS> arena_heads{};
    std::array<FreeBlock*, NUM_ARENAS> arena_tails{};

    while (head) {
        FreeBlock* next = head->next;
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

    // Phase 2: within each arena, split by size class (per-block metadata,
    // audit N2), then push each class run to the pending queue or process
    // directly if the class lock is free.
    for (uint32_t i = 0; i < NUM_ARENAS; ++i) {
        if (!arena_heads[i]) continue;

        std::array<FreeBlock*, NUM_SIZE_CLASSES> cls_heads{};
        std::array<FreeBlock*, NUM_SIZE_CLASSES> cls_tails{};
        std::array<uint16_t, NUM_SIZE_CLASSES> present_idx{}; // classes seen (pack class list)
        std::size_t present_count = 0;

        FreeBlock* curr = arena_heads[i];
        while (curr) {
            FreeBlock* next = curr->next; // save: we rewire curr below
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
    for (uint32_t a = 0; a < NUM_ARENAS; ++a) {
        Arena& arena = arenas_[a];
        for (std::size_t c = 0; c < NUM_SIZE_CLASSES; ++c) {
            ScopedSpinLock lock(arena.class_locks_[c]);
            DrainPendingReturns(a, c);
        }
    }
    // Step 2: drop every cached span back to the OS.
    return PurgePageCacheLocked();
}

} // namespace FastAlloc

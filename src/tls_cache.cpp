#include "tls_cache.h"
#include "global_heap.h"
#include "os_memory.h"
#include "debug_aid.h"
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <new>

namespace FastAlloc {

FAST_THREAD_LOCAL TLSCache* fast_path_cache = nullptr;

// Alloc-site hint filter table (see tls_cache.h): 1 KB per thread.
FAST_THREAD_LOCAL std::uint64_t g_alloc_hints[kAllocHintSlots];

// ===========================================================================
// Invariant checking and telemetry for TLS bins
// ===========================================================================
namespace {

std::atomic<std::uint64_t> g_double_push_guard_hits{0};
std::atomic<std::uint64_t> g_bin_heal_events{0};

void PrintInvariantEvent(const char* kind, std::size_t class_index,
                          std::uint32_t claimed, std::size_t actual) {
    std::fprintf(stderr,
                 "[fastalloc-invariant] %s: class=%zu count=%u list=%zu "
                 "(duplicate hand-out/free upstream); guarded, self-healed. "
                 "Total guard=%llu heal=%llu\n",
                 kind, class_index, claimed, actual,
                 (unsigned long long)g_double_push_guard_hits.load(
                     std::memory_order_relaxed),
                 (unsigned long long)g_bin_heal_events.load(
                     std::memory_order_relaxed));
    std::fflush(stderr);
}

} // namespace

void ReportDoublePushGuard() {
    std::uint64_t n =
        g_double_push_guard_hits.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) {
        PrintInvariantEvent("double-push guard", 0, 0, 0);
    }
}

// Logs diagnostic information once when an invalid bin head pointer is encountered.
void ReportBinHeadCorruption(std::size_t class_index, std::uintptr_t bad_head) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[fastalloc-invariant] corrupt TLS bin head: class=%zu "
                     "head=%#zx (< 64 KB; never a FastAlloc block). Bin "
                     "re-derived from a fresh batch, allocator state "
                     "protected.\n",
                     class_index, bad_head);
        std::fflush(stderr);
    }
}

// Logs diagnostic information once when an invalid freelist pointer is encountered.
void ReportWildFreelistLink(const char* where, std::size_t class_index,
                             std::uintptr_t bad_link) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[fastalloc-invariant] wild freelist value at %s: "
                     "class=%zu value=%#zx (fails <64KB / <2^47 / 16-aligned "
                     "plausibility). Dropped, allocator state protected.\n",
                     where, class_index, bad_link);
        std::fflush(stderr);
    }
}

// ===========================================================================
// TLSCache recycling pool (thread-lifecycle optimization)
//
// Every thread used to pay an mmap + 3 first-touch page faults + a munmap
// just to exist (the cache object is a page-rounded 12 KB allocation), which
// made short-lived-thread workloads ~15x slower than glibc. Retired caches
// are now parked in a small lock-guarded pool and re-used by new threads:
// page-cache-warm memory, zero syscalls per lifecycle. The pool is capped
// (kCachePoolCap x 12 KB = 384 KB worst case) and anything beyond the cap
// (or arriving during process shutdown) is returned to the OS as before.
// ===========================================================================
namespace {

constexpr std::size_t kCachePoolCap = 32;

struct CachePool {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    TLSCache* slots[kCachePoolCap];
    std::atomic<std::size_t> count{0}; // relaxed: fast-path emptiness hint + locked truth

    void Push(TLSCache* c) {
        // Spin briefly; thread exit is rare enough that contention here is
        // negligible and a plain lock avoids the ABA hazards of a lock-free
        // Treiber stack with recycled nodes.
        int spins = 0;
        while (lock.test_and_set(std::memory_order_acquire)) {
            if (++spins > 64) { std::this_thread::yield(); spins = 0; }
            else FAST_CPU_PAUSE();
        }
        if (count.load(std::memory_order_relaxed) < kCachePoolCap) {
            slots[count.load(std::memory_order_relaxed)] = c;
            count.fetch_add(1, std::memory_order_relaxed);
            lock.clear(std::memory_order_release);
        } else {
            lock.clear(std::memory_order_release);
            std::size_t page_size = OSMemory::GetPageSize();
            std::size_t alloc_size = (sizeof(TLSCache) + page_size - 1) & ~(page_size - 1);
            OSMemory::FreePages(c, alloc_size);
        }
    }

    TLSCache* Pop() {
        if (count.load(std::memory_order_relaxed) == 0) return nullptr; // hint, racy-safe
        TLSCache* c = nullptr;
        int spins = 0;
        while (lock.test_and_set(std::memory_order_acquire)) {
            if (++spins > 64) { std::this_thread::yield(); spins = 0; }
            else FAST_CPU_PAUSE();
        }
        std::size_t n = count.load(std::memory_order_relaxed);
        if (n > 0) {
            c = slots[n - 1];
            count.store(n - 1, std::memory_order_relaxed);
        }
        lock.clear(std::memory_order_release);
        return c;
    }
};

CachePool& CachePoolInstance() {
    // Static storage placement new removes heap allocations during bootstrap.
    alignas(CachePool) static unsigned char storage[sizeof(CachePool)];
    static CachePool* pool = ::new (static_cast<void*>(storage)) CachePool();
    return *pool;
}

// Park a retired cache in the pool or return to the OS.
void RecycleOrFreeCache(TLSCache* cache, bool flushed_ok) {
    if (flushed_ok) {
        CachePoolInstance().Push(cache);
        return;
    }
    std::size_t page_size = OSMemory::GetPageSize();
    std::size_t alloc_size = (sizeof(TLSCache) + page_size - 1) & ~(page_size - 1);
    OSMemory::FreePages(cache, alloc_size);
}

} // namespace

// ===========================================================================
// TLS key management
// ===========================================================================
#ifdef _WIN32
DWORD TLSCache::tls_key_ = FLS_OUT_OF_INDEXES;

static void WINAPI FlsCleanupCallback(PVOID ptr) {
    if (ptr) {
        TLSCache* cache = static_cast<TLSCache*>(ptr);
        bool process_exiting = GlobalHeap::GetInstance().IsShuttingDown();
        bool flushed_ok = true;
        if (!process_exiting) {
            cache->~TLSCache(); // flush cached blocks back to the global heap
        } else {
            flushed_ok = false;  // shutting down: leak the raw memory, never syscall
        }
        fast_path_cache = nullptr;
        stats::CountTlsDestroy();
        RecycleOrFreeCache(cache, flushed_ok);
    }
}

void TLSCache::InitTlsKey() {
    tls_key_ = FlsAlloc(FlsCleanupCallback);
    if (tls_key_ == FLS_OUT_OF_INDEXES) {
        std::fprintf(stderr,
            "FastAlloc FATAL: FlsAlloc failed (no FLS slots left). "
            "Refusing to continue with broken TLS - aborting.\n");
        std::abort();
    }
}
#else
pthread_key_t TLSCache::tls_key_;

void TLSCache::TlsDestructor(void* ptr) {
    if (ptr) {
        TLSCache* cache = static_cast<TLSCache*>(ptr);
        bool process_exiting = GlobalHeap::GetInstance().IsShuttingDown();
        bool flushed_ok = true;
        if (!process_exiting) {
            cache->~TLSCache(); // flush cached blocks back to the global heap
        } else {
            flushed_ok = false; // shutting down: park/leak, never munmap under a reaper
        }
        fast_path_cache = nullptr;
        stats::CountTlsDestroy();
        RecycleOrFreeCache(cache, flushed_ok);
    }
}

void TLSCache::InitTlsKey() {
    int rc = pthread_key_create(&tls_key_, TlsDestructor);
    if (rc != 0) {
        // tls_key_ may hold a stale/foreign value here; a zero-initialized
        // key is a valid key number that can belong to another library, and
        // pthread_getspecific on it can return foreign data we would
        // reinterpret as TLSCache*. Fail loudly instead of corrupting.
        std::fprintf(stderr,
            "FastAlloc FATAL: pthread_key_create failed (rc=%d). "
            "Refusing to continue with broken TLS - aborting.\n", rc);
        std::abort();
    }
}
#endif

TLSCache::TLSCache() : bins_() {
    arena_index_ = GlobalHeap::GetInstance().GetNextArena();
    stats::CountTlsCreate();
#if FASTALLOC_LOGGING_ENABLED
    LogEvent(LogLevel::Debug, "tls-create", "cache=%p arena=%u", (void*)this, arena_index_);
#endif
}

TLSCache::TLSCache(RecycledTag) {
    // Pre-cleared by construction: pool entries only come from fully-flushed
    // owners (RecycleOrFreeCache), and a fresh mmap is zero-filled. Every
    // bin field (head/tail/count/next_refill/arena_hint), large bin and
    // stats field is therefore already in the fresh-cache state - skip the
    // 16 KB re-zero. arena_hint 0 == kArenaHintUnset matches zeroed memory.
    //
    // v13.6: a RECYCLED cache keeps its previous owner's arena. The previous
    // owner's exit flush pushed its blocks to THAT arena's pending queues;
    // the next short-lived owner's refills then hit its own arena's queue
    // directly (no cross-arena steal scan, warmer lines). Long-lived threads
    // still spread across arenas via the fresh-construction path below;
    // recycled caches are the churn path, and consecutive churn threads
    // sharing one arena exit quickly enough that contention stays bounded.
    if (FAST_UNLIKELY(arena_index_ >= GlobalHeap::kNumArenas)) {
        arena_index_ = GlobalHeap::GetInstance().GetNextArena(); // zeroed page
    }
    stats::CountTlsCreate();
}

TLSCache& TLSCache::GetSlow() {
    static bool key_init = [] { InitTlsKey(); return true; }();
    (void)key_init;

#ifdef _WIN32
    void* val = FlsGetValue(tls_key_);
    if (val) {
        fast_path_cache = static_cast<TLSCache*>(val);
        return *fast_path_cache;
    }
    std::size_t page_size = OSMemory::GetPageSize();
    std::size_t alloc_size = (sizeof(TLSCache) + page_size - 1) & ~(page_size - 1);
    // Recycle a retired cache when possible: no mmap, no first-touch faults.
    void* mem = CachePoolInstance().Pop();
    if (!mem) mem = OSMemory::AllocatePages(alloc_size);
    if (FAST_UNLIKELY(!mem)) {
        std::fprintf(stderr, "FastAlloc FATAL: cannot map memory for thread cache - aborting.\n");
        std::abort();
    }
    TLSCache* cache = new (mem) TLSCache(RecycledTag{});
    if (FAST_UNLIKELY(!FlsSetValue(tls_key_, cache))) {
        // Degrade gracefully: this thread keeps a working cache through
        // fast_path_cache, but the destructor will never run - log it.
        std::fprintf(stderr,
            "FastAlloc ERROR: FlsSetValue failed; thread cache will not be "
            "reclaimed at thread exit (memory retention).\n");
    }
    fast_path_cache = cache;
    return *cache;
#else
    void* val = pthread_getspecific(tls_key_);
    if (val) {
        fast_path_cache = static_cast<TLSCache*>(val);
        return *fast_path_cache;
    }
    std::size_t page_size = OSMemory::GetPageSize();
    std::size_t alloc_size = (sizeof(TLSCache) + page_size - 1) & ~(page_size - 1);
    // Recycle a retired cache when possible: no mmap, no first-touch faults.
    void* mem = CachePoolInstance().Pop();
    if (!mem) mem = OSMemory::AllocatePages(alloc_size);
    if (FAST_UNLIKELY(!mem)) {
        std::fprintf(stderr, "FastAlloc FATAL: cannot mmap memory for thread cache - aborting.\n");
        std::abort();
    }
    TLSCache* cache = new (mem) TLSCache(RecycledTag{});
    int rc = pthread_setspecific(tls_key_, cache);
    if (FAST_UNLIKELY(rc != 0)) {
        // Degrade gracefully: this thread keeps a working cache through
        // fast_path_cache, but the destructor will never run - log it.
        std::fprintf(stderr,
            "FastAlloc ERROR: pthread_setspecific failed (rc=%d); thread cache "
            "will not be reclaimed at thread exit (memory retention).\n", rc);
    }
    fast_path_cache = cache;
    return *cache;
#endif
}

TLSCache::~TLSCache() {
    FlushStats();
    FlushToGlobalHeap();
}

void TLSCache::FlushStats() {
    if (stats_.ops == 0) return;
    stats::CountSmallAlloc(stats_.small_allocs, stats_.small_alloc_bytes);
    stats::CountSmallFree(stats_.small_frees, stats_.small_free_bytes);
    stats::CountLargeAlloc(stats_.large_allocs, stats_.large_alloc_bytes);
    stats::CountLargeFree(stats_.large_frees, stats_.large_free_bytes);
    stats::CountRealloc(stats_.reallocs);
    stats_ = LocalStats{};
}

void TLSCache::FlushToGlobalHeap() {
    // v12: every bin takes the O(1) splice to the per-arena pending queues
    // (single-arena bins: zero block touches; mixed bins: the guarded arena
    // split inside FlushBinToPending). No class lock is taken and no
    // per-block slab work happens here; the next thread that allocates one
    // of these (arena, class) pairs drains the queue via the direct feed,
    // and the global pending-bytes cap bounds retention when nobody does
    // (the pusher then drains its own push - the same slab work the old
    // locked path did, just deferred to the rare overflow case).
    GlobalHeap& gh = GlobalHeap::GetInstance();
    for (std::size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
        if (bins_[i].head) {
#if FASTALLOC_DEBUG_ENABLED
            AuditBin(i);
#endif
            gh.FlushBinToPending(i, bins_[i].head, bins_[i].tail,
                                 bins_[i].count, bins_[i].arena_hint,
                                 arena_index_);
        }
        // Reset all bin fields (head, count, and refill ramp) unconditionally
        // so that any recycled cache instance starts in a clean state.
        bins_[i].head = nullptr;
        bins_[i].tail = nullptr;
        bins_[i].count = 0;
        bins_[i].arena_hint = kArenaHintUnset;
        // Reset the adaptive refill ramp: the next owner of this (recycled
        // or fresh) cache starts from the initial small batch. Without
        // this, next_refill doubles on every thread's miss and each
        // short-lived thread carves hundreds of virgin slab blocks
        // (first-touch page faults) it will only hand back at exit.
        // Long-lived threads ramp back up within a few misses, so steady
        // throughput is unaffected.
        bins_[i].next_refill = 0;
    }
    for (std::size_t i = 0; i < NUM_LARGE_CLASSES; ++i) {
        LargeFreeEntry* entry = large_free_bins_[i];
        while (entry) {
            LargeFreeEntry* next = entry->next;
            GlobalHeap::GetInstance().DeallocateLarge(entry, entry->alloc_size);
            entry = next;
        }
        large_free_bins_[i] = nullptr;
        large_counts_[i] = 0;
    }
}

#if FASTALLOC_DEBUG_ENABLED
void TLSCache::AuditBin(std::size_t class_index) const {
    // Debug check: verify bin count matches actual linked list length before flushing.
    const CacheBin& bin = bins_[class_index];
    std::size_t n = 0;
    for (FreeBlock* p = bin.head; p; p = p->next) ++n;
    if (n != bin.count) {
        ViolationInfo info{};
        info.kind = Violation::RegistryInconsistency;
        info.where = "TLSCache bin count does not match list length";
        info.size = class_index;
        info.caller = FAST_RETURN_ADDRESS();
        ReportViolation(info);
    }
}
#endif

void* TLSCache::AllocateBlockSlow(std::size_t class_index) {
    stats::CountTlsMisses();
    std::size_t max_cache = CACHE_LIMITS[class_index];

    // Adaptive refill: start at initial refill target and double on consecutive misses, capped at max_cache/2.
    CacheBin& bin = bins_[class_index];
    std::size_t target_count = bin.next_refill;
    if (target_count == 0) target_count = 4; // kInitialRefill (v13.6: 4 - short-lived threads touch ~8 blocks/class; a 16-batch drags 16 cold lines and flushes the surplus back at exit)
    std::size_t hard_cap = max_cache / 2;
    if (hard_cap == 0) hard_cap = 1;
    if (target_count > hard_cap) target_count = hard_cap;
    bin.next_refill = static_cast<uint32_t>(
            (target_count * 2 <= hard_cap) ? target_count * 2 : hard_cap);

    std::size_t actual_count = 0;
    FreeBlock* batch_tail = nullptr;
    bool foreign = false;
    FreeBlock* batch_head = GlobalHeap::GetInstance().AllocateBatch(
            class_index, target_count, actual_count, arena_index_,
            &batch_tail, &foreign);

    if (!batch_head) return nullptr;

    // Verify batch head plausibility before dereferencing.
    if (FAST_UNLIKELY(!IsPlausibleHeapPtr(batch_head))) {
        ReportWildFreelistLink("AllocateBlockSlow(batch head)", class_index,
                               reinterpret_cast<std::uintptr_t>(batch_head));
        return nullptr;
    }

#if defined(__GNUC__) || defined(__clang__)
    FreeBlock* curr = batch_head;
    for (int i = 0; i < 4 && curr; ++i) {
        __builtin_prefetch(curr, 0, 1);
        // Stop prefetch walk at the first non-plausible pointer.
        FreeBlock* nxt = curr->next;
        curr = IsPlausibleHeapPtr(nxt) ? nxt : nullptr;
    }
#endif

    FreeBlock* block = batch_head;
    // Verify next pointer plausibility before updating bin head.
    FreeBlock* rest = block->next;
    if (FAST_UNLIKELY(rest != nullptr && !IsPlausibleHeapPtr(rest))) {
        ReportWildFreelistLink("AllocateBlockSlow(pop rest)", class_index,
                               reinterpret_cast<std::uintptr_t>(rest));
        rest = nullptr;
        if (actual_count > 1) actual_count = 1;
        batch_tail = block;
    }
    bins_[class_index].head = rest;
    // Exact tail bookkeeping (v12): the batch arrived pre-cut with a known
    // tail, so the bin's tail stays O(1)-maintained for the next splice
    // flush. The wild-tail cut above re-derives it locally.
    bins_[class_index].tail = rest ? batch_tail : nullptr;
    bins_[class_index].count = static_cast<uint32_t>(actual_count - 1);
    // Arena hint: the batch is single-arena (own pending queue / own
    // slabs) unless the cross-arena steal contributed foreign blocks.
    bins_[class_index].arena_hint =
        foreign ? kArenaHintMixed : ArenaToHint(arena_index_);
    // v13.4: the slow path hands out the batch head too - record it in the
    // alloc-site hint filter (mirrors the fast pop path in tls_cache.h).
    {
        const uint32_t hint = bins_[class_index].arena_hint;
        if (FAST_LIKELY(hint >= 1 && hint <= GlobalHeap::kNumArenas)) {
            RecordAllocHint(reinterpret_cast<char*>(block) + USER_OFFSET,
                            static_cast<uint32_t>(class_index),
                            HintToArena(hint));
        }
    }

    return block;
}

void TLSCache::DeallocateBlockSlow(std::size_t class_index) {
    CacheBin& bin = bins_[class_index];
    FreeBlock* head = bin.head;

    if (FAST_UNLIKELY(head == nullptr)) {
        // Empty list with a diverged count: nothing to splice - re-derive
        // the counters and keep serving.
        if (g_bin_heal_events.fetch_add(1, std::memory_order_relaxed) == 0) {
            PrintInvariantEvent("bin count/list divergence (empty)", class_index,
                                bin.count, 0);
        }
        bin.count = 0;
        bin.tail = nullptr;
        bin.arena_hint = kArenaHintUnset;
        return;
    }

    // v12 FLUSH-ALL SPLICE: the whole bin is handed to the global heap in
    // O(1). Single-arena bins (the common case - the arena index rides the
    // block headers) splice onto the owning arena's lock-free pending queue
    // without touching a single block; mixed bins take one guarded
    // arena-split walk inside FlushBinToPending. Either way, no per-block
    // slab work happens on the free path - the owning arena's allocations
    // drain the queue through the direct feed, and the pending-bytes cap
    // makes the pusher drain when nobody allocates (free storms).
    //
    // Self-healing (v8/v11 discipline): a broken tail invariant (tail null
    // with a non-empty list, or tail->next != nullptr - the fingerprint of
    // a duplicated node / corrupted list) falls back to the guarded,
    // self-healing split path instead of publishing a broken list.
    if (FAST_UNLIKELY(bin.tail == nullptr || bin.tail->next != nullptr)) {
        if (g_bin_heal_events.fetch_add(1, std::memory_order_relaxed) == 0) {
            PrintInvariantEvent("bin tail invariant broken", class_index,
                                bin.count, 0);
        }
        GlobalHeap::GetInstance().DeallocateBatchClass(class_index, head);
        bin.head = nullptr;
        bin.tail = nullptr;
        bin.count = 0;
        bin.next_refill = 0;
        bin.arena_hint = kArenaHintUnset;
        return;
    }

    GlobalHeap::GetInstance().FlushBinToPending(
            class_index, head, bin.tail, bin.count, bin.arena_hint, arena_index_);

    bin.head = nullptr;
    bin.tail = nullptr;
    bin.count = 0;
    bin.arena_hint = kArenaHintUnset;
    // The adaptive refill ramp is intentionally kept: the refill target
    // describes how much to PULL next time, independent of how much was
    // just pushed out.
}

void* TLSCache::AllocateLargeCached(std::size_t size) {
    std::size_t cls = LargeSizeToClass(size);
    LargeFreeEntry** pp = &large_free_bins_[cls];
    while (*pp) {
        // Validate the entry with the plausibility predicate before dereferencing.
        if (FAST_UNLIKELY(!IsPlausibleHeapPtr(*pp))) {
            ReportWildFreelistLink("AllocateLargeCached(walk)", cls,
                                   reinterpret_cast<std::uintptr_t>(*pp));
            *pp = nullptr; // cut off wild tail
            continue;
        }
        if ((*pp)->alloc_size >= size) {
            LargeFreeEntry* entry = *pp;
            *pp = entry->next;
            large_counts_[cls]--;

            LargeAllocHeader* header = reinterpret_cast<LargeAllocHeader*>(entry);
#if FASTALLOC_DEBUG_ENABLED
            header->slab = reinterpret_cast<void*>(debug::LARGE_MAGIC);
#else
            header->slab = nullptr;
#endif
            header->alloc_size = entry->alloc_size;
            return reinterpret_cast<char*>(header) + sizeof(LargeAllocHeader);
        }
        pp = &(*pp)->next;
    }

    void* mem = GlobalHeap::GetInstance().AllocateLarge(size);
    if (mem) {
        LargeAllocHeader* header = static_cast<LargeAllocHeader*>(mem);
        header->alloc_size = size;
#if FASTALLOC_DEBUG_ENABLED
        header->slab = reinterpret_cast<void*>(debug::LARGE_MAGIC);
#else
        header->slab = nullptr;
#endif
        return reinterpret_cast<char*>(mem) + sizeof(LargeAllocHeader);
    }
    return nullptr;
}

void TLSCache::DeallocateLargeCached(void* ptr, std::size_t alloc_size) {
    std::size_t cls = LargeSizeToClass(alloc_size);
    if (large_counts_[cls] < GetMaxLargeCacheSize(alloc_size)) {
        LargeFreeEntry* entry = reinterpret_cast<LargeFreeEntry*>(ptr);
        // v13.3 double-free guard (mirrors the small-bin head guard): a
        // second free of the span currently at this bin's head would store
        // entry->next = entry (self-cycle) and hand the span out twice on
        // the next two allocations. One compare on an already-loaded word.
        if (FAST_UNLIKELY(large_free_bins_[cls] == entry)) {
            ReportDoublePushGuard();
            return;
        }
        entry->alloc_size = alloc_size;
        entry->next = large_free_bins_[cls];
        large_free_bins_[cls] = entry;
        large_counts_[cls]++;
    } else {
        GlobalHeap::GetInstance().DeallocateLarge(ptr, alloc_size);
    }
}

// ===========================================================================
// Diagnostic pointer scan
//
// Scans the fixed head arrays of this thread's cache without foreign dereferences.
// ===========================================================================
std::size_t TLSCache::AuditPointers(const void* lo, const void* hi) const {
    std::size_t hits = 0;
    for (std::size_t c = 0; c < NUM_SIZE_CLASSES; ++c) {
        const FreeBlock* head = bins_[c].head;
        if (reinterpret_cast<std::uintptr_t>(head) >=
                reinterpret_cast<std::uintptr_t>(lo) &&
            reinterpret_cast<std::uintptr_t>(head) <
                reinterpret_cast<std::uintptr_t>(hi)) {
            std::fprintf(stderr,
                         "[fastalloc-audit] tls bin class=%zu head=%p -> IN RANGE\n",
                         c, static_cast<void*>(const_cast<FreeBlock*>(head)));
            ++hits;
        }
    }
    for (std::size_t i = 0; i < NUM_LARGE_CLASSES; ++i) {
        const LargeFreeEntry* entry = large_free_bins_[i];
        if (reinterpret_cast<std::uintptr_t>(entry) >=
                reinterpret_cast<std::uintptr_t>(lo) &&
            reinterpret_cast<std::uintptr_t>(entry) <
                reinterpret_cast<std::uintptr_t>(hi)) {
            std::fprintf(stderr,
                         "[fastalloc-audit] tls large bin %zu head=%p -> IN RANGE\n",
                         i, static_cast<void*>(const_cast<LargeFreeEntry*>(entry)));
            ++hits;
        }
    }
    return hits;
}

std::size_t AuditCurrentThreadCache(const void* lo, const void* hi) {
    std::size_t hits = TLSCache::GetFast().AuditPointers(lo, hi);
    // The cross-thread recycling pool: retired TLSCache objects. A pool slot
    // pointing into the audited range means a recycled cache was corrupted.
    CachePool& pool = CachePoolInstance();
    std::size_t n = pool.count.load(std::memory_order_relaxed);
    if (n > kCachePoolCap) n = kCachePoolCap;
    for (std::size_t i = 0; i < n; ++i) {
        TLSCache* c = pool.slots[i];
        if (reinterpret_cast<std::uintptr_t>(c) >=
                reinterpret_cast<std::uintptr_t>(lo) &&
            reinterpret_cast<std::uintptr_t>(c) <
                reinterpret_cast<std::uintptr_t>(hi)) {
            std::fprintf(stderr,
                         "[fastalloc-audit] cache pool slot %zu = %p -> IN RANGE\n",
                         i, static_cast<void*>(c));
            ++hits;
        }
    }
    return hits;
}

} // namespace FastAlloc

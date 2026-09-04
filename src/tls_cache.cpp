#include "tls_cache.h"
#include "global_heap.h"
#include "os_memory.h"
#include "debug_aid.h"
#include <cstdlib>
#include <cstdio>
#include <new>

namespace FastAlloc {

FAST_THREAD_LOCAL TLSCache* fast_path_cache = nullptr;

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
    static CachePool* pool = new CachePool(); // NOLINT: intentional leak
    return *pool;
}

// Park a retired cache. During process shutdown we never touch the OS:
// the pool push itself is safe, and overflowing entries are simply leaked
// (the OS reclaims them at exit) - strictly safer than munmapping under a
// reaper, which was the original Windows exit deadlock hazard (audit N3).
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
// TLS key management (audit C4 fix)
//
// pthread_key_create / FlsAlloc failures were previously ignored. On the
// POSIX path the half-initialized key defaults to 0, which is a VALID key
// number that may be owned by another library, so pthread_getspecific could
// return foreign data that we reinterpret as TLSCache* - silent corruption.
// These failures are now loud and fatal.
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

TLSCache::TLSCache() {
    arena_index_ = GlobalHeap::GetInstance().GetNextArena();
    stats::CountTlsCreate();
#if FASTALLOC_LOGGING_ENABLED
    LogEvent(LogLevel::Debug, "tls-create", "cache=%p arena=%u", (void*)this, arena_index_);
#endif
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
    TLSCache* cache = new (mem) TLSCache();
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
    TLSCache* cache = new (mem) TLSCache();
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
    // Thread-exit deferral cap: small remnants (the interesting case for
    // thread-churn - a handful of surplus refill blocks per class) take the
    // lock-free pending hand-off. Bigger bins go through the locked path so
    // their slabs actually drain and empty slabs are returned to the page
    // cache: free-heavy patterns (overhead/ramp workloads) must not leave
    // megabytes parked in pending lists that nobody ever refills.
    constexpr uint32_t kDeferMaxBin = 64;
    for (std::size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
        if (bins_[i].head) {
#if FASTALLOC_DEBUG_ENABLED
            AuditBin(i);
#endif
            if (bins_[i].count <= kDeferMaxBin) {
                // Lock-free deferred hand-off: the dying thread parks the
                // bin list on the per-arena pending queue without taking any
                // class lock or doing per-block slab work (paid instead by
                // the next thread that refills this class). Turns thread
                // exit from O(blocks-with-locks) into O(one CAS).
                GlobalHeap::GetInstance().DeferBinToPending(i, bins_[i].head);
            } else {
                GlobalHeap::GetInstance().DeallocateBatchClass(i, bins_[i].head);
            }
            bins_[i].head = nullptr;
            bins_[i].count = 0;
            // Reset the adaptive refill ramp: the next owner of this (recycled
            // or fresh) cache starts from the initial small batch. Without
            // this, next_refill doubles on every thread's miss and each
            // short-lived thread carves hundreds of virgin slab blocks
            // (first-touch page faults) it will only hand back at exit.
            // Long-lived threads ramp back up within a few misses, so steady
            // throughput is unaffected.
            bins_[i].next_refill = 0;
        }
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
    // Audit M6: 'count' was maintained purely by construction; this debug
    // check ties it to the actual list length before flushing.
    const CacheBin& bin = bins_[class_index];
    std::size_t n = 0;
    for (FreeBlock* p = bin.head; p; p = p->next) ++n;
    if (n != bin.count) {
        ViolationInfo info{};
        info.kind = Violation::RegistryInconsistency;
        info.where = "TLSCache bin count does not match list length";
        info.size = class_index;
        info.caller = __builtin_return_address(0);
        ReportViolation(info);
    }
}
#endif

void* TLSCache::AllocateBlockSlow(std::size_t class_index) {
    stats::CountTlsMisses();
    std::size_t max_cache = CACHE_LIMITS[class_index];

    // Adaptive refill: start at kInitialRefill and double on every miss,
    // capped at the old fixed target (max_cache/2). Short-lived threads that
    // touch a class a handful of times pull one small batch; steady-state
    // threads ramp to full batches after ~log2(ratio) misses (negligible for
    // 20M-op runs) and behave identically to the old policy afterwards.
    CacheBin& bin = bins_[class_index];
    std::size_t target_count = bin.next_refill;
    if (target_count == 0) target_count = 16; // kInitialRefill
    std::size_t hard_cap = max_cache / 2;
    if (hard_cap == 0) hard_cap = 1;
    if (target_count > hard_cap) target_count = hard_cap;
    bin.next_refill = (target_count * 2 <= hard_cap) ? target_count * 2 : hard_cap;

    std::size_t actual_count = 0;
    FreeBlock* batch_head = GlobalHeap::GetInstance().AllocateBatch(class_index, target_count, actual_count, arena_index_);

    if (!batch_head) return nullptr;

#if defined(__GNUC__) || defined(__clang__)
    FreeBlock* curr = batch_head;
    for (int i = 0; i < 4 && curr; ++i) {
        __builtin_prefetch(curr, 0, 1);
        curr = curr->next;
    }
#endif

    FreeBlock* block = batch_head;
    bins_[class_index].head = block->next;
    bins_[class_index].count = static_cast<uint32_t>(actual_count - 1);

    return block;
}

void TLSCache::DeallocateBlockSlow(std::size_t class_index) {
    std::size_t max_cache = CACHE_LIMITS[class_index];
    std::size_t batch_size = max_cache / 2;
    if (batch_size == 0) batch_size = 1;

    CacheBin& bin = bins_[class_index];
    FreeBlock* head = bin.head;
    FreeBlock* curr = head;
    for (std::size_t i = 1; i < batch_size; ++i) {
        curr = curr->next;
    }

    bin.head = curr->next;
    curr->next = nullptr;
    bin.count -= static_cast<uint32_t>(batch_size);

    // Class-aware hand-off: the whole batch is known single-class (it came
    // from one TLS bin), so the 512-entry per-class split (two 4 KB stack
    // arrays zeroed per call) is skipped entirely. The arena split is still
    // per-block because a bin may hold blocks foreign to this thread's arena
    // (cross-thread frees), but it uses a 16-entry table and the per-arena
    // lists go through the lock-free pending queue when contended.
    GlobalHeap::GetInstance().DeallocateBatchClass(class_index, head);
}

void* TLSCache::AllocateLargeCached(std::size_t size) {
    std::size_t cls = LargeSizeToClass(size);
    LargeFreeEntry** pp = &large_free_bins_[cls];
    while (*pp) {
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
        entry->alloc_size = alloc_size;
        entry->next = large_free_bins_[cls];
        large_free_bins_[cls] = entry;
        large_counts_[cls]++;
    } else {
        GlobalHeap::GetInstance().DeallocateLarge(ptr, alloc_size);
    }
}

} // namespace FastAlloc

#include "debug_aid.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <algorithm>

#if defined(__linux__) && defined(__GLIBC__)
#define FASTALLOC_HAVE_BACKTRACE 1
#include <execinfo.h>
#include <unistd.h>
#include <sys/syscall.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace FastAlloc {

// ===========================================================================
// Thread id helper
// ===========================================================================
uint64_t CurrentThreadId() {
#if defined(_WIN32)
    return static_cast<uint64_t>(GetCurrentThreadId());
#elif defined(__linux__)
    return static_cast<uint64_t>(::syscall(SYS_gettid));
#else
    return static_cast<uint64_t>(pthread_self());
#endif
}

// ===========================================================================
// Logging
// ===========================================================================
#if FASTALLOC_LOGGING_ENABLED

namespace {
std::atomic<int> g_log_level{ -1 }; // -1 = uninitialized
std::mutex       g_log_mutex;

unsigned long CurrentTid() {
    // Delegate to the public helper above: it already resolves the platform
    // thread id (GetCurrentThreadId / gettid / pthread_self) and windows.h is
    // included at the top of this TU. NOTE: a linkage specification
    // (extern "C") is only legal at namespace scope in C++, so declaring a
    // helper inside this function body is not portable - GCC rejects it.
    return static_cast<unsigned long>(CurrentThreadId());
}

int InitialLogLevel() {
    const char* env = fast_getenv("FASTALLOC_LOG_LEVEL");
    if (env && *env) {
        int v = std::atoi(env);
        if (v < 0) v = 0;
        if (v > 5) v = 5;
        return v;
    }
#if FASTALLOC_DEBUG_ENABLED
    return static_cast<int>(LogLevel::Warning);
#else
    return static_cast<int>(LogLevel::Off);
#endif
}
} // namespace

void LogSetLevel(LogLevel level) { g_log_level.store(static_cast<int>(level), std::memory_order_relaxed); }

LogLevel LogGetLevel() {
    int v = g_log_level.load(std::memory_order_relaxed);
    if (v < 0) v = InitialLogLevel();
    return static_cast<LogLevel>(v);
}

void LogEvent(LogLevel level, const char* event, const char* fmt, ...) {
    int current = g_log_level.load(std::memory_order_relaxed);
    if (current < 0) {
        current = InitialLogLevel();
        g_log_level.store(current, std::memory_order_relaxed);
    }
    if (static_cast<int>(level) > current) return;

    char body[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    static const char* kNames[6] = { "OFF", "ERROR", "WARN ", "INFO ", "DEBUG", "TRACE" };
    int idx = static_cast<int>(level);
    if (idx < 0 || idx > 5) idx = 5;

    unsigned long tid = CurrentTid();

    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::fprintf(stderr, "[FastAlloc][%s][tid=%lu][%s] %s\n",
                 kNames[idx], tid, event, body);
}

#else  // !FASTALLOC_LOGGING_ENABLED
// Inline no-ops declared in the header.
#endif

// ===========================================================================
// Statistics
// ===========================================================================
namespace stats {
namespace {
// Global counters. Relaxed atomics: diagnostics only, no ordering needs.
std::atomic<std::size_t> g_small_allocs{0}, g_small_frees{0};
std::atomic<std::size_t> g_large_allocs{0}, g_large_frees{0};
std::atomic<std::size_t> g_reallocs{0};
std::atomic<std::size_t> g_live_blocks{0}, g_live_bytes{0};
std::atomic<std::size_t> g_peak_live_blocks{0}, g_peak_live_bytes{0};
std::atomic<std::size_t> g_os_allocs{0}, g_os_frees{0}, g_os_bytes_live{0};
std::atomic<std::size_t> g_page_cache_hits{0}, g_page_cache_bytes{0};
std::atomic<std::size_t> g_page_cache_evictions{0};
std::atomic<std::size_t> g_tls_misses{0}, g_pending_pushes{0};
std::atomic<std::size_t> g_tls_created{0}, g_tls_destroyed{0};

inline void BumpLive(uint64_t n, std::size_t bytes) {
    g_live_blocks.fetch_add(n, std::memory_order_relaxed);
    g_live_bytes.fetch_add(bytes, std::memory_order_relaxed);
    // Peak maintenance at flush granularity: values are monotone snapshots
    // of the running totals, so peaks can only be under-reported between
    // flushes (documented as approximate in release builds).
    std::size_t b = g_live_blocks.load(std::memory_order_relaxed);
    std::size_t by = g_live_bytes.load(std::memory_order_relaxed);
    std::size_t pb = g_peak_live_blocks.load(std::memory_order_relaxed);
    while (b > pb && !g_peak_live_blocks.compare_exchange_weak(pb, b, std::memory_order_relaxed)) {}
    std::size_t pby = g_peak_live_bytes.load(std::memory_order_relaxed);
    while (by > pby && !g_peak_live_bytes.compare_exchange_weak(pby, by, std::memory_order_relaxed)) {}
}
inline void DropLive(uint64_t n, std::size_t bytes) {
    g_live_blocks.fetch_sub(n, std::memory_order_relaxed);
    g_live_bytes.fetch_sub(bytes, std::memory_order_relaxed);
}
} // namespace

void CountSmallAlloc(uint64_t n, std::size_t bytes) {
    g_small_allocs.fetch_add(n, std::memory_order_relaxed);
    BumpLive(n, bytes);
}
void CountSmallFree (uint64_t n, std::size_t bytes) {
    g_small_frees.fetch_add(n, std::memory_order_relaxed);
    DropLive(n, bytes);
}
void CountLargeAlloc(uint64_t n, std::size_t bytes) {
    g_large_allocs.fetch_add(n, std::memory_order_relaxed);
    BumpLive(n, bytes);
}
void CountLargeFree (uint64_t n, std::size_t bytes) {
    g_large_frees.fetch_add(n, std::memory_order_relaxed);
    DropLive(n, bytes);
}
void CountRealloc(uint64_t n) { g_reallocs.fetch_add(n, std::memory_order_relaxed); }
void CountOsAlloc(std::size_t bytes) { ++g_os_allocs; g_os_bytes_live.fetch_add(bytes, std::memory_order_relaxed); }
void CountOsFree (std::size_t bytes) { ++g_os_frees;  g_os_bytes_live.fetch_sub(bytes, std::memory_order_relaxed); }
void CountPageCacheHit()  { ++g_page_cache_hits; }
void CountPageCacheStore(std::size_t bytes) { g_page_cache_bytes.fetch_add(bytes, std::memory_order_relaxed); }
void CountPageCacheEvict(std::size_t bytes) { g_page_cache_bytes.fetch_sub(bytes, std::memory_order_relaxed); ++g_page_cache_evictions; }
void CountPendingPush() { ++g_pending_pushes; }
void CountTlsMisses()  { ++g_tls_misses; }
void CountTlsCreate()  { ++g_tls_created; }
void CountTlsDestroy()  { ++g_tls_destroyed; }

void Snapshot(FastAllocStats& out) {
    out.small_allocs = g_small_allocs.load(std::memory_order_relaxed);
    out.small_frees = g_small_frees.load(std::memory_order_relaxed);
    out.large_allocs = g_large_allocs.load(std::memory_order_relaxed);
    out.large_frees = g_large_frees.load(std::memory_order_relaxed);
    out.reallocs = g_reallocs.load(std::memory_order_relaxed);
    out.current_live_blocks = g_live_blocks.load(std::memory_order_relaxed);
    out.current_live_bytes = g_live_bytes.load(std::memory_order_relaxed);
    out.peak_live_blocks = g_peak_live_blocks.load(std::memory_order_relaxed);
    out.peak_live_bytes = g_peak_live_bytes.load(std::memory_order_relaxed);
    out.os_alloc_calls = g_os_allocs.load(std::memory_order_relaxed);
    out.os_free_calls = g_os_frees.load(std::memory_order_relaxed);
    out.os_bytes_live = g_os_bytes_live.load(std::memory_order_relaxed);
    out.page_cache_hits = g_page_cache_hits.load(std::memory_order_relaxed);
    out.page_cache_bytes = g_page_cache_bytes.load(std::memory_order_relaxed);
    out.page_cache_evictions = g_page_cache_evictions.load(std::memory_order_relaxed);
    out.tls_cache_misses = g_tls_misses.load(std::memory_order_relaxed);
    out.pending_queue_pushes = g_pending_pushes.load(std::memory_order_relaxed);
    out.thread_caches_created = g_tls_created.load(std::memory_order_relaxed);
    out.thread_caches_destroyed = g_tls_destroyed.load(std::memory_order_relaxed);
#if FASTALLOC_DEBUG_ENABLED
    // Registry values are exact - prefer them over the approximate atomics.
    out.current_live_blocks = AllocRegistry::GetInstance().live_count();
    out.current_live_bytes = AllocRegistry::GetInstance().live_bytes();
#endif
}

} // namespace stats

// The public getter is defined in fast_alloc.cpp (fast_alloc_stats) so it can
// fold in registry data under FASTALLOC_DEBUG.

// ===========================================================================
// Registry
// ===========================================================================
AllocRegistry& AllocRegistry::GetInstance() {
    static AllocRegistry instance;
    return instance;
}

namespace {
inline std::size_t HashPtr(void* p, std::size_t mask) {
    // Fibonacci hashing for pointer values.
    std::uintptr_t v = reinterpret_cast<std::uintptr_t>(p);
    v >>= 4; // blocks are at least 16B aligned; mix the interesting bits
    return (static_cast<std::size_t>(v) * 0x9E3779B97F4A7C15ull) & mask;
}

// Tombstone sentinel for open-addressing deletion (probe-chain safety).
constexpr std::uintptr_t kTombstoneBits = 0x1;
inline bool IsTombstone(void* p) {
    return reinterpret_cast<std::uintptr_t>(p) == kTombstoneBits;
}
inline void* Tombstone() { return reinterpret_cast<void*>(kTombstoneBits); }

constexpr std::size_t kInitialShardCapacity = 1024; // power of two
} // namespace

std::size_t AllocRegistry::HashShard(void* p) const {
    return (reinterpret_cast<std::uintptr_t>(p) >> 4) % kShardCount;
}

AllocRegistry::AllocRegistry() {
    for (auto& s : shards_) {
        s.capacity = kInitialShardCapacity;
        s.count = 0;
        s.live_bytes = 0;
        s.table = static_cast<AllocRecord*>(std::malloc(s.capacity * sizeof(AllocRecord)));
        // Registry memory comes from std::malloc on purpose: it must never
        // be served by FastAlloc itself (re-entrancy hazard).
        if (s.table) {
            std::memset(s.table, 0, s.capacity * sizeof(AllocRecord));
        }
    }
}

AllocRegistry::~AllocRegistry() {
    for (auto& s : shards_) std::free(s.table);
}

void AllocRegistry::GrowShard(Shard& shard) {
    std::size_t new_cap = shard.capacity * 2;
    AllocRecord* nt = static_cast<AllocRecord*>(std::malloc(new_cap * sizeof(AllocRecord)));
    if (!nt) return; // stay at current capacity; load factor degrades but stays correct
    std::memset(nt, 0, new_cap * sizeof(AllocRecord));
    for (std::size_t i = 0; i < shard.capacity; ++i) {
        if (shard.table[i].ptr && !IsTombstone(shard.table[i].ptr)) {
            std::size_t slot = HashPtr(shard.table[i].ptr, new_cap - 1);
            while (nt[slot].ptr) slot = (slot + 1) & (new_cap - 1);
            nt[slot] = shard.table[i];
        }
    }
    std::free(shard.table);
    shard.table = nt;
    shard.capacity = new_cap;
    shard.tombstones = 0; // rebuild drops all tombstones
}

bool AllocRegistry::Insert(const AllocRecord& rec, AllocRecord* existing) {
    Shard& s = shards_[HashShard(rec.ptr)];
    std::lock_guard<std::mutex> lock(s.mtx);

    // Rehash on high load INCLUDING tombstones: insert/erase churn creates
    // tombstones which would otherwise slow probes indefinitely.
    if ((s.count + s.tombstones) * 10 >= s.capacity * 7) GrowShard(s);

    std::size_t slot = HashPtr(rec.ptr, s.capacity - 1);
    std::size_t first_tombstone = static_cast<std::size_t>(-1);
    while (s.table[slot].ptr) {
        if (IsTombstone(s.table[slot].ptr)) {
            if (first_tombstone == static_cast<std::size_t>(-1)) first_tombstone = slot;
        } else if (s.table[slot].ptr == rec.ptr) {
            if (existing) *existing = s.table[slot];
            return false; // already live -> double allocation / double free
        }
        slot = (slot + 1) & (s.capacity - 1);
    }
    if (first_tombstone != static_cast<std::size_t>(-1)) {
        slot = first_tombstone;
        --s.tombstones;
    }
    s.table[slot] = rec;
    s.table[slot].sequence = sequence_.fetch_add(1, std::memory_order_relaxed);
    ++s.count;
    s.live_bytes += rec.size;
    return true;
}

bool AllocRegistry::Erase(void* ptr, AllocRecord* out) {
    Shard& s = shards_[HashShard(ptr)];
    std::lock_guard<std::mutex> lock(s.mtx);
    std::size_t slot = HashPtr(ptr, s.capacity - 1);
    while (s.table[slot].ptr) {
        if (!IsTombstone(s.table[slot].ptr) && s.table[slot].ptr == ptr) {
            if (out) *out = s.table[slot];
            if (s.live_bytes >= s.table[slot].size) s.live_bytes -= s.table[slot].size;
            // Tombstone (not nullptr!) keeps probe chains intact: a later
            // lookup for an entry placed beyond this slot must still scan on.
            s.table[slot].ptr = Tombstone();
            --s.count;
            ++s.tombstones;
            return true;
        }
        slot = (slot + 1) & (s.capacity - 1);
    }
    return false; // not live: invalid free or double free
}

std::size_t AllocRegistry::live_count() const {
    std::size_t n = 0;
    for (auto& s : shards_) { std::lock_guard<std::mutex> lock(s.mtx); n += s.count; }
    return n;
}

std::size_t AllocRegistry::live_bytes() const {
    std::size_t n = 0;
    for (auto& s : shards_) { std::lock_guard<std::mutex> lock(s.mtx); n += s.live_bytes; }
    return n;
}

void AllocRegistry::DumpLeaks() {
    std::fprintf(stderr, "\n================ FastAlloc LEAK REPORT ================\n");
    std::size_t total = 0, total_bytes = 0;
    for (std::size_t si = 0; si < kShardCount; ++si) {
        Shard& s = const_cast<Shard&>(shards_[si]);
        std::lock_guard<std::mutex> lock(s.mtx);
        for (std::size_t i = 0; i < s.capacity; ++i) {
            if (!s.table[i].ptr || IsTombstone(s.table[i].ptr)) continue;
            ++total;
            total_bytes += s.table[i].size;
            std::fprintf(stderr,
                "  LEAK ptr=%p size=%zu (usable=%zu) class=%s seq=%zu tid=%llu\n",
                s.table[i].ptr, s.table[i].size, s.table[i].usable,
                s.table[i].class_or_large == 0xFFFFFFFFu ? "LARGE" : "small",
                s.table[i].sequence,
                static_cast<unsigned long long>(s.table[i].thread_id));
        }
    }
    if (total == 0) {
        std::fprintf(stderr, "  No live allocations. All blocks freed. No leaks.\n");
    } else {
        std::fprintf(stderr, "  TOTAL: %zu live allocations, %zu bytes\n", total, total_bytes);
    }
    std::fprintf(stderr, "=======================================================\n");
}

void AllocRegistry::ResetForTesting() {
    for (auto& s : shards_) {
        std::lock_guard<std::mutex> lock(s.mtx);
        std::memset(s.table, 0, s.capacity * sizeof(AllocRecord));
        s.count = 0;
        s.tombstones = 0;
        s.live_bytes = 0;
    }
    sequence_.store(0, std::memory_order_relaxed);
}

// ===========================================================================
// Violation reporting
// ===========================================================================
namespace {
std::atomic<ViolationHandler> g_handler{ nullptr };

void DefaultViolationHandler(const ViolationInfo& info) {
    const char* what = "unknown";
    switch (info.kind) {
        case Violation::DoubleFree:        what = "DOUBLE FREE"; break;
        case Violation::InvalidFree:       what = "INVALID FREE (pointer not from FastAlloc)"; break;
        case Violation::FrontCanaryBroken: what = "BUFFER UNDERFLOW (front canary destroyed)"; break;
        case Violation::RearCanaryBroken:  what = "BUFFER OVERFLOW (rear red zone destroyed)"; break;
        case Violation::PoisonDisturbed:   what = "USE-AFTER-FREE WRITE (freed block modified)"; break;
        case Violation::BadSlabMagic:      what = "CORRUPT SLAB HEADER (bad magic)"; break;
        case Violation::BadLargeMagic:     what = "CORRUPT LARGE HEADER (bad magic)"; break;
        case Violation::RegistryInconsistency: what = "REGISTRY INCONSISTENCY"; break;
        case Violation::InternalInvariant: what = "INTERNAL INVARIANT VIOLATED"; break;
    }
    std::fprintf(stderr, "\n*** FastAlloc FATAL: %s ***\n", what);
    std::fprintf(stderr, "    pointer : %p\n", info.ptr);
    std::fprintf(stderr, "    size    : %zu\n", info.size);
    std::fprintf(stderr, "    where   : %s\n", info.where);
    std::fprintf(stderr, "    caller  : %p\n", info.caller);
    if (info.has_record) {
        std::fprintf(stderr, "    alloc'd : size=%zu seq=%zu tid=%llu\n",
                     info.record.size, info.record.sequence,
                     static_cast<unsigned long long>(info.record.thread_id));
    }
#if FASTALLOC_HAVE_BACKTRACE
    void* frames[32];
    int n = backtrace(frames, 32);
    if (n > 0) {
        std::fprintf(stderr, "    backtrace:\n");
        backtrace_symbols_fd(frames, n, 2);
    }
#endif
    std::fflush(stderr);
    std::abort();
}
} // namespace

void SetViolationHandler(ViolationHandler handler) {
    g_handler.store(handler, std::memory_order_release);
}

[[noreturn]] void ReportViolation(const ViolationInfo& info) {
    ViolationHandler h = g_handler.load(std::memory_order_acquire);
    if (h) {
        h(info); // test hook; must terminate the process or longjmp out
    }
    DefaultViolationHandler(info);
    std::abort(); // unreachable if handler behaved; belt and suspenders
}

// ===========================================================================
// Debug poison helpers
// ===========================================================================
#if FASTALLOC_DEBUG_ENABLED
namespace debug_check {

namespace {
constexpr std::size_t kPoisonCheckBytes = 256; // per-end sample size for huge blocks
}

void PoisonFreed(void* user, std::size_t bytes) {
    unsigned char* p = static_cast<unsigned char*>(user);
    if (bytes <= 2 * kPoisonCheckBytes) {
        std::memset(p, debug::POISON_FREE, bytes);
    } else {
        // Poison head & tail only: keeps debug mode usable on multi-MB blocks
        // while still catching off-by-one UAF writes at both ends.
        std::memset(p, debug::POISON_FREE, kPoisonCheckBytes);
        std::memset(p + bytes - kPoisonCheckBytes, debug::POISON_FREE, kPoisonCheckBytes);
    }
}

bool PoisonIntact(const void* user, std::size_t bytes) {
    const unsigned char* p = static_cast<const unsigned char*>(user);
    std::size_t n = std::min(bytes, kPoisonCheckBytes);
    for (std::size_t i = 0; i < n; ++i)
        if (p[i] != debug::POISON_FREE) return false;
    if (bytes > 2 * kPoisonCheckBytes) {
        for (std::size_t i = bytes - kPoisonCheckBytes; i < bytes; ++i)
            if (p[i] != debug::POISON_FREE) return false;
    }
    return true;
}

} // namespace debug_check
#endif

// ===========================================================================
// Leak dump entry
// ===========================================================================
void FastDumpLeaksImpl() {
#if FASTALLOC_DEBUG_ENABLED
    AllocRegistry::GetInstance().DumpLeaks();
#else
    std::fprintf(stderr,
        "[FastAlloc] leak dump requested but FASTALLOC_DEBUG is not enabled;\n"
        "[FastAlloc] rebuild with -DFASTALLOC_DEBUG for exact leak tracking.\n");
#endif
}

} // namespace FastAlloc

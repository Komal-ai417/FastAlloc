#pragma once
// ============================================================================
// FastAlloc debugging aids: allocation registry, canary helpers, logging,
// statistics, leak dump and violation reporting.
//
// Everything in this unit is either compiled out (release) or explicitly
// gated at runtime. Release builds pay zero cost.
// ============================================================================
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>

#include "fast_alloc_config.h"
#include "fast_alloc.h"   // FastAllocStats

namespace FastAlloc {

// Thread id for diagnostics (Linux gettid / Win GetCurrentThreadId).
uint64_t CurrentThreadId();

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
enum class LogLevel : int {
    Off = 0,
    Error = 1,
    Warning = 2,
    Info = 3,
    Debug = 4,
    Trace = 5,
};

#if FASTALLOC_LOGGING_ENABLED
// Compiled-in logging. Runtime level defaults from FASTALLOC_LOG_LEVEL env
// variable (0..5), else Off for release / Warning for debug builds.
// Thread-safe: single stderr sink, mutex-guarded.
void LogEvent(LogLevel level, const char* event, const char* fmt, ...);
void LogSetLevel(LogLevel level);
LogLevel LogGetLevel();
#else
inline void LogEvent(LogLevel, const char*, const char* ...) {}
inline void LogSetLevel(LogLevel) {}
inline LogLevel LogGetLevel() { return LogLevel::Off; }
#endif

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------
// The public FastAllocStats struct lives in include/fast_alloc.h.
// Internal counter helpers (relaxed atomics; contention is acceptable given
// the call frequency and the diagnostic value).
namespace stats {
    // Batched flush entry points: the TLS caches accumulate counts locally
    // (plain increments, no atomics on the hot path) and flush them here.
    // This keeps the alloc/free fast path free of shared-cache-line traffic.
    void CountSmallAlloc(uint64_t n, std::size_t bytes);
    void CountSmallFree(uint64_t n, std::size_t bytes);
    void CountLargeAlloc(uint64_t n, std::size_t bytes);
    void CountLargeFree(uint64_t n, std::size_t bytes);
    void CountRealloc(uint64_t n);
    // Slow-path / OS-level events: direct atomics (off the hot path).
    void CountOsAlloc(std::size_t bytes);
    void CountOsFree(std::size_t bytes);
    void CountPageCacheHit();
    void CountPageCacheStore(std::size_t bytes);
    void CountPageCacheEvict(std::size_t bytes);
    void CountPendingPush();
    void CountTlsMisses();
    void CountTlsCreate();
    void CountTlsDestroy();
    void Snapshot(FastAllocStats& out);
}

// ---------------------------------------------------------------------------
// Allocation registry (FASTALLOC_DEBUG only)
// ---------------------------------------------------------------------------
// Sharded, mutex-protected open-addressing hash map  ptr -> allocation record.
// Purpose:
//   - double-free detection      (Erase of a pointer that is not live)
//   - invalid-free detection     (Erase of a pointer never allocated by us)
//   - exact leak reporting       (records still present at dump time)
struct AllocRecord {
    void*       ptr;
    std::size_t size;          // user-requested size
    std::size_t usable;        // usable bytes (class size - USER_OFFSET)
    std::size_t sequence;      // global allocation sequence number
    uint64_t    thread_id;     // OS thread id of the allocating thread
    uint32_t    class_or_large; // small: size class index; large: 0xFFFFFFFF
};

class AllocRegistry {
public:
    static AllocRegistry& GetInstance();

    // Returns false + fills 'existing' if pointer already live (double alloc /
    // double free scenario detected on the SECOND registration).
    bool Insert(const AllocRecord& rec, AllocRecord* existing);

    // Returns false if pointer was not live (invalid free or double free).
    bool Erase(void* ptr, AllocRecord* out);

    void  DumpLeaks();          // Prints every live allocation (leak report).
    void  ResetForTesting();

    std::size_t live_count() const;
    std::size_t live_bytes() const;

private:
    AllocRegistry();
    ~AllocRegistry();
    AllocRegistry(const AllocRegistry&) = delete;
    AllocRegistry& operator=(const AllocRegistry&) = delete;

    struct Shard {
        mutable std::mutex mtx;
        AllocRecord* table = nullptr;   // open addressing, capacity power of 2
        std::size_t capacity = 0;
        std::size_t count = 0;
        std::size_t tombstones = 0;
        std::size_t live_bytes = 0;
    };
    static constexpr std::size_t kShardCount = 16;
    Shard shards_[kShardCount];

    std::atomic<std::size_t> sequence_{0};
    std::size_t HashShard(void* p) const;
    static void GrowShard(Shard& shard);
};

// ---------------------------------------------------------------------------
// Violation reporting
// ---------------------------------------------------------------------------
enum class Violation : int {
    DoubleFree = 1,     // freeing a pointer that is not live in the registry
    InvalidFree = 2,    // freeing a pointer FastAlloc never allocated
    FrontCanaryBroken = 3,  // underflow: bytes before user region overwritten
    RearCanaryBroken = 4,   // overflow: bytes after user size overwritten
    PoisonDisturbed = 5,    // block contents changed after free (UAF write)
    BadSlabMagic = 6,       // slab header does not validate (heap corruption)
    BadLargeMagic = 7,      // large header does not validate
    RegistryInconsistency = 8,
    InternalInvariant = 9,
};

struct ViolationInfo {
    Violation    kind;
    void*        ptr;
    std::size_t  size;         // best-known size (0 if unknown)
    const char*  where;        // call site description
    void*        caller;       // __builtin_return_address(0) when available
    AllocRecord  record;       // registry data when applicable
    int          has_record;
};

// Violation handler. The default prints a diagnostic (with backtrace where
// available) and calls std::abort(). Tests may install their own handler,
// e.g. to run the faulting operation in a forked child. The allocator is in
// an inconsistent state when this fires - do not continue execution.
using ViolationHandler = void (*)(const ViolationInfo& info);
void SetViolationHandler(ViolationHandler handler);

// Called internally by all debug checks. Never returns (handler must
// terminate or the process is left in an undefined state).
[[noreturn]] void ReportViolation(const ViolationInfo& info);

// ---------------------------------------------------------------------------
// Canary / poison helpers (inline, debug builds only)
// ---------------------------------------------------------------------------
#if FASTALLOC_DEBUG_ENABLED
namespace debug_check {

// Front canary lives in FreeBlock's padding dword (block+12).
inline void WriteFrontCanary(uint32_t* slot) { *slot = debug::FRONT_CANARY; }
inline bool FrontCanaryOk(const uint32_t* slot) { return *slot == debug::FRONT_CANARY; }

// Rear red zone: everything from user_ptr+user_size to the end of the block.
inline void FillRearRedZone(void* user_end, std::size_t zone_bytes) {
    if (zone_bytes) {
        for (std::size_t i = 0; i < zone_bytes; ++i)
            static_cast<unsigned char*>(user_end)[i] = debug::RED_ZONE;
    }
}
inline bool RearRedZoneOk(const void* user_end, std::size_t zone_bytes) {
    const unsigned char* p = static_cast<const unsigned char*>(user_end);
    for (std::size_t i = 0; i < zone_bytes; ++i)
        if (p[i] != debug::RED_ZONE) return false;
    return true;
}

// Poison a freed user region (bounded fill to stay usable on huge blocks).
void   PoisonFreed(void* user, std::size_t bytes);
// Verify poison; returns false if any checked byte changed (UAF write).
bool   PoisonIntact(const void* user, std::size_t bytes);

} // namespace debug_check
#endif // FASTALLOC_DEBUG_ENABLED

// ---------------------------------------------------------------------------
// Public debug API (see include/fast_alloc.h for user-facing docs)
// ---------------------------------------------------------------------------
void FastDumpLeaksImpl();   // FASTALLOC_DEBUG: full leak dump; else prints notice.

} // namespace FastAlloc

#include "fast_alloc.h"
#include "tls_cache.h"
#include "global_heap.h"
#include "fast_alloc_config.h"
#include "os_memory.h"
#include "span_map.h"
#include "debug_aid.h"

#include <cstring>
#include <cerrno>
#include <atomic>
#include <new>
#include <limits>
#include <cstdio>
#include <cstdlib>

#if !defined(_WIN32)
#include <sys/mman.h>
#endif

namespace FastAlloc {

static_assert(sizeof(LargeAllocHeader) == 16, "Header size must align to USER_OFFSET");
static_assert(sizeof(FreeBlock) == 24, "FreeBlock layout contract");

// Sentinel stored in the 'slab' slot of over-aligned blocks:
//   [aligned-32 .. aligned) stash layout:
//     aligned-32: alignment
//     aligned-24: raw (original fast_malloc user pointer)
//     aligned-16: ALIGN_MAGIC   <- read by fast_free as block->slab
//     aligned- 8: requested size
namespace {
constexpr std::uintptr_t ALIGN_MAGIC = 0xA1A2A3A4A5A6A701ull; // odd, never a real Slab*


inline std::size_t EffectivePageSize() {
    // Queries the system page size at runtime for page-aligned allocation sizing.
    return OSMemory::GetPageSize();
}

// The span-map free path never reads a block header; the map-miss and
// block-start-geometry checks replace the legacy class-index guard, so
// its reporting helper is removed.

// Checks that a large allocation header has a plausible page-aligned span size.
inline bool LargeHeaderSizePlausible(std::size_t alloc_size) {
    const std::size_t page = EffectivePageSize();
    return alloc_size >= page && (alloc_size % page) == 0 &&
           alloc_size < (1ull << 48);
}

// Logs diagnostic information once when a large allocation header has an invalid size.
inline void ReportCorruptLargeHeader(void* ptr, std::size_t alloc_size) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[fastalloc-invariant] corrupt large header at %p: "
                     "alloc_size=%#zx (not a page-rounded span). Pointer "
                     "dropped (leaked), large cache NOT poisoned.\n",
                     ptr, alloc_size);
        std::fflush(stderr);
    }
}

// Logs diagnostic information once when an invalid pointer is passed to free.
inline void ReportNonCanonicalFree(void* ptr) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[fastalloc-invariant] free of non-plausible pointer %p "
                     "(fails <64KB / <2^47 / 16-aligned; never a FastAlloc "
                     "block). Pointer dropped, allocator state protected.\n",
                     ptr);
        std::fflush(stderr);
    }
}

// Logs diagnostic information once if an allocation exit check detects an invalid pointer.
inline void ReportWildReturnValue(void* ptr) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[fastalloc-invariant] refusing to return non-plausible "
                     "pointer %p from fast_malloc (leaked, nullptr returned "
                     "instead).\n",
                     ptr);
        std::fflush(stderr);
    }
}

inline std::size_t PageRoundUp(std::size_t n) {
    std::size_t page = EffectivePageSize();
    return (n + page - 1) & ~(page - 1);
}

#if !FASTALLOC_DEBUG_ENABLED
// ===========================================================================
// v12 release pointer routing: span map + 1-entry per-thread span cache.
// ===========================================================================
// The old free path read the freed block's own header (slab pointer, class
// index) to route the free. For a live set larger than cache that header
// line is a guaranteed high-latency miss on EVERY free - measured as a 2x
// throughput gap vs jemalloc on the churn workload (jemalloc derives
// everything from its extent metadata and never touches the freed block).
// The span map (span_map.h) routes by ADDRESS through a handful of hot
// radix lines; the slab header - one line shared by every block of the
// slab - supplies class/arena/geometry. The freed block's line is touched
// exactly once, by the freelist-link store every thread-cache design pays.

// One-shot telemetry for frees of pointers outside every block-bearing span.
inline void ReportUnmappedSpanFree(void* ptr) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[fastalloc-invariant] free of pointer %p outside every "
                     "mapped span (never allocated here, or span already "
                     "returned). Pointer dropped, allocator state protected.\n",
                     ptr);
        std::fflush(stderr);
    }
}

// Per-thread 1-entry span cache ("mini-TLB"): pairs-style workloads hit the
// same slab span on nearly every free; churn-style workloads fall through
// to the map (still far cheaper than the old cold block-header read). Only
// POOL-BACKED slabs are cached - pool chunks are never munmap'd, so a
// cached range can never point at unmapped memory (direct-mmap slabs CAN
// be munmap'd by PageHeapFree, which would leave a stale, faulting entry).
FAST_THREAD_LOCAL Slab* g_span_cache_slab = nullptr;
FAST_THREAD_LOCAL char* g_span_cache_lo = nullptr;
FAST_THREAD_LOCAL char* g_span_cache_hi = nullptr;

// Division-free "is ptr exactly a block USER start of slab" (see
// fast_alloc_config.h CLASS_MAGICS for the exactness proof). Reads only the
// slab header (hot line).
inline bool IsBlockStart(const Slab* slab, const void* ptr) {
    const char* base = reinterpret_cast<const char*>(slab);
    const std::size_t d = slab->block_size;
    // Corrupt/zeroed slab header (e.g. madvise'd span): reject.
    if (FAST_UNLIKELY(d < ALIGNMENT || d > MAX_SLAB_SIZE || (d & (ALIGNMENT - 1)) != 0)) {
        return false;
    }
    const std::size_t off = static_cast<std::size_t>(
        reinterpret_cast<const char*>(ptr) -
        (base + slab->first_block_offset + USER_OFFSET));
    // Bounds; also rejects underflow (ptr below the first block) because a
    // wrapped difference is astronomically larger than any span.
    if (FAST_UNLIKELY(off >= slab->memory_size)) return false;
    const std::uint64_t q = (off * CLASS_MAGICS[d / ALIGNMENT - 1]) >> 34;
    return q < slab->total_blocks && q * d == off;
}

// Release fast-free core: routes via cache -> map, never reads the block.
// Returns true when handled. 'slow_path' receives the rare non-block-start
// pointers (aligned stash, interior garbage). The alloc-site hint filter is
// checked by fast_free BEFORE this (a hit also proves plausibility, so the
// 3-compare predicate is skipped on the hottest path).
static inline bool RouteMappedFree(void* ptr) {
    // 0.5) v13.7 radix probe + block-header cross-check. The freed block's
    // own line is brought in by the freelist push below ANYWAY, so reading
    // its immutable (slab, class, arena) header words is free; the map
    // entry supplies the span identity and class to cross-check against.
    // This path never touches the slab header line and never runs the
    // geometry math - the two costs the map path paid on every hint miss
    // (cross-thread frees, long-lived churn blocks). Mismatches (parked
    // marker, collision, corrupt header) fall through to the validated
    // paths below.
    // 1) 1-entry span cache (hot for single-slab working sets).
    if (ptr >= g_span_cache_lo && ptr < g_span_cache_hi) {
        Slab* slab = g_span_cache_slab;
        if (FAST_LIKELY(IsBlockStart(slab, ptr)) &&
            FAST_LIKELY(slab->class_idx < NUM_SIZE_CLASSES)) {
            TLSCache& tls = TLSCache::GetFast();
            tls.CountSmallFree(UsableSize(slab->class_idx));
            tls.DeallocateBlock(slab->class_idx,
                                reinterpret_cast<FreeBlock*>(static_cast<char*>(ptr) - USER_OFFSET),
                                slab->arena_index);
            return true;
        }
        // fall through: not a block start of the cached slab
    }
    // 2) span map.
    const SpanMap::LookupResult lr = SpanMap::Lookup(ptr);
    if (FAST_UNLIKELY(lr.entry == SpanMap::kKindNone)) return false;
    if (FAST_UNLIKELY(!lr.validated)) return false; // interior pointer
    const unsigned kind = SpanMap::Kind(lr.entry);
    if (kind == SpanMap::kKindSlab) {
        Slab* slab = SpanMap::Desc<Slab>(lr.entry);
        // 'validated' already proved IsBlockStart against this exact slab.
        if (FAST_LIKELY(slab->class_idx < NUM_SIZE_CLASSES)) {
            if (SpanMap::PoolBacked(lr.entry)) {
                g_span_cache_slab = slab;
                g_span_cache_lo = reinterpret_cast<char*>(slab);
                g_span_cache_hi = reinterpret_cast<char*>(slab) + slab->memory_size;
            }
            TLSCache& tls = TLSCache::GetFast();
            tls.CountSmallFree(UsableSize(slab->class_idx));
            tls.DeallocateBlock(slab->class_idx,
                                reinterpret_cast<FreeBlock*>(static_cast<char*>(ptr) - USER_OFFSET),
                                slab->arena_index);
            return true;
        }
        return false; // aligned stash or interior garbage -> slow discriminant
    }
    if (kind == SpanMap::kKindLarge) {
        // 'validated' already proved ptr == header + sizeof(LargeAllocHeader).
        LargeAllocHeader* header = SpanMap::Desc<LargeAllocHeader>(lr.entry);
        {
            std::size_t alloc_size = header->alloc_size;
            // v10 guard: a zeroed (madvise'd span) or scribbled header must
            // not feed the TLS large cache a bogus entry.
            if (FAST_UNLIKELY(!LargeHeaderSizePlausible(alloc_size))) {
                ReportCorruptLargeHeader(ptr, alloc_size);
                return true; // handled: dropped
            }
            TLSCache& tls = TLSCache::GetFast();
            tls.CountLargeFree(alloc_size - sizeof(LargeAllocHeader));
            tls.DeallocateLargeCached(header, alloc_size);
            return true;
        }
        return false; // interior pointer of a large span -> slow discriminant
    }
    return false;
}
#endif // !FASTALLOC_DEBUG_ENABLED

#if FASTALLOC_DEBUG_ENABLED
// --------------------------------------------------------------------------
// Debug validation shared by the allocation entry points
// --------------------------------------------------------------------------
[[noreturn]] void FatalDebug(Violation kind, void* ptr, std::size_t size,
                             const char* where) {
    ViolationInfo info{};
    info.kind = kind;
    info.ptr = ptr;
    info.size = size;
    info.where = where;
    info.caller = FAST_RETURN_ADDRESS();
    info.has_record = 0;
    ReportViolation(info);
}

// Verify the "freed poison / fresh fill" head+tail windows of a reused block.
void CheckReusePattern(const unsigned char* user, std::size_t usable) {
    constexpr std::size_t kSample = 256;
    const unsigned char* p = user;
    std::size_t head = usable < kSample ? usable : kSample;
    // Blocks straight out of the slab free list had their first 8 bytes
    // clobbered by list links, so start at +8.
    for (std::size_t i = 8; i < head; ++i) {
        if (p[i] != debug::POISON_FREE && p[i] != debug::FRESH) {
            FatalDebug(Violation::PoisonDisturbed, const_cast<unsigned char*>(p) + i,
                       usable, "use-after-free write detected on allocation");
        }
    }
    if (usable > 2 * kSample) {
        for (std::size_t i = usable - kSample; i < usable; ++i) {
            if (p[i] != debug::POISON_FREE && p[i] != debug::FRESH) {
                FatalDebug(Violation::PoisonDisturbed, const_cast<unsigned char*>(p) + i,
                           usable, "use-after-free write detected on allocation");
            }
        }
    }
}

void RegistryInsertOrFail(void* user, std::size_t size, std::size_t usable, bool large,
                          uint32_t klass, const char* where) {
    AllocRecord rec{};
    rec.ptr = user;
    rec.size = size;
    rec.usable = usable;
    rec.thread_id = CurrentThreadId();
    rec.class_or_large = large ? 0xFFFFFFFFu : klass;
    AllocRecord existing{};
    if (!AllocRegistry::GetInstance().Insert(rec, &existing)) {
        ViolationInfo info{};
        info.kind = Violation::DoubleFree; // pointer already live -> double alloc
        info.ptr = user;
        info.size = size;
        info.where = where;
        info.caller = FAST_RETURN_ADDRESS();
        info.record = existing;
        info.has_record = 1;
        ReportViolation(info);
    }
}

// Common debug bookkeeping for a block handed to the user.
void DebugOnAllocSmall(void* user, std::size_t size, std::size_t class_index) {
    FreeBlock* block = reinterpret_cast<FreeBlock*>(static_cast<char*>(user) - USER_OFFSET);
    std::size_t block_size = ClassIndexToSize(class_index);
    std::size_t usable = UsableSize(class_index);
    CheckReusePattern(static_cast<unsigned char*>(user), usable);
    debug_check::WriteFrontCanary(&block->canary);
    // Red zone spans from end-of-request to end-of-BLOCK (includes the
    // RED_ZONE_RESERVE tail), so it is never empty.
    debug_check::FillRearRedZone(static_cast<char*>(user) + size,
                                 block_size - USER_OFFSET - size);
    RegistryInsertOrFail(user, size, usable, false, static_cast<uint32_t>(class_index),
                         "fast_malloc: allocation of an already-live pointer");
}

void DebugOnAllocLarge(void* user, std::size_t size, std::size_t alloc_size) {
    std::size_t usable = alloc_size - sizeof(LargeAllocHeader);
    // UAF check: the sample windows must be all-zero (fresh mmap), all
    // POISON_FREE (cached after a free) or all FRESH (prior debug fill).
    {
        constexpr std::size_t kSample = 256;
        const unsigned char* p = static_cast<const unsigned char*>(user);
        std::size_t head = usable < kSample ? usable : kSample;
        auto ok_byte = [](unsigned char b) {
            return b == debug::POISON_FREE || b == debug::FRESH || b == 0x00;
        };
        for (std::size_t i = 0; i < head; ++i) {
            if (!ok_byte(p[i])) {
                FatalDebug(Violation::PoisonDisturbed, const_cast<unsigned char*>(p) + i,
                           usable, "use-after-free write detected on large allocation");
            }
        }
        if (usable > 2 * kSample) {
            for (std::size_t i = usable - kSample; i < usable; ++i) {
                if (!ok_byte(p[i])) {
                    FatalDebug(Violation::PoisonDisturbed, const_cast<unsigned char*>(p) + i,
                               usable, "use-after-free write detected on large allocation");
                }
            }
        }
    }
    debug_check::FillRearRedZone(static_cast<char*>(user) + size, usable - size);
    RegistryInsertOrFail(user, size, usable, true, 0,
                         "fast_malloc(large): allocation of an already-live pointer");
}

// Common debug validation when the user returns a block.
AllocRecord DebugOnFreeSmall(void* user, FreeBlock* block) {
    AllocRecord rec{};
    if (!AllocRegistry::GetInstance().Erase(user, &rec)) {
        FatalDebug(Violation::DoubleFree, user, 0,
                   "fast_free: pointer is not live (double free or invalid free)");
    }
    if (block->slab == nullptr ||
        block->slab->magic != debug::SLAB_MAGIC) {
        FatalDebug(Violation::BadSlabMagic, user, 0,
                   "fast_free: slab header failed magic validation");
    }
    if (!debug_check::FrontCanaryOk(&block->canary)) {
        FatalDebug(Violation::FrontCanaryBroken, user, rec.size,
                   "fast_free: front canary destroyed (buffer underflow)");
    }
    std::size_t block_size = ClassIndexToSize(block->class_index);
    if (!debug_check::RearRedZoneOk(static_cast<char*>(user) + rec.size,
                                    block_size - USER_OFFSET - rec.size)) {
        FatalDebug(Violation::RearCanaryBroken, user, rec.size,
                   "fast_free: rear red zone destroyed (buffer overflow)");
    }
    debug_check::PoisonFreed(user, UsableSize(block->class_index));
    return rec;
}

AllocRecord DebugOnFreeLarge(void* user, LargeAllocHeader* header) {
    AllocRecord rec{};
    if (!AllocRegistry::GetInstance().Erase(user, &rec)) {
        FatalDebug(Violation::DoubleFree, user, 0,
                   "fast_free: pointer is not live (double free or invalid free)");
    }
    if (reinterpret_cast<std::uintptr_t>(header->slab) !=
        debug::LARGE_MAGIC) {
        FatalDebug(Violation::BadLargeMagic, user, 0,
                   "fast_free: large header magic destroyed (buffer underflow)");
    }
    // alloc_size must be page-aligned and match the recorded span exactly;
    // any other value means the header (ptr-8..ptr-1) was overwritten.
    {
        std::size_t page = OSMemory::GetPageSize();
        if (rec.usable != 0 &&
            header->alloc_size != rec.usable + sizeof(LargeAllocHeader)) {
            FatalDebug(Violation::BadLargeMagic, user, rec.size,
                       "fast_free: large header size field corrupted (buffer underflow)");
        }
        if (header->alloc_size < sizeof(LargeAllocHeader) + 1 ||
            (header->alloc_size % page) != 0) {
            FatalDebug(Violation::BadLargeMagic, user, header->alloc_size,
                       "fast_free: large header size field corrupted (buffer underflow)");
        }
    }
    std::size_t usable = rec.usable;
    if (usable == 0) usable = header->alloc_size - sizeof(LargeAllocHeader);
    if (!debug_check::RearRedZoneOk(static_cast<char*>(user) + rec.size,
                                    usable - rec.size)) {
        FatalDebug(Violation::RearCanaryBroken, user, rec.size,
                   "fast_free: rear red zone destroyed (buffer overflow)");
    }
    debug_check::PoisonFreed(user, usable);
    return rec;
}
#endif // FASTALLOC_DEBUG_ENABLED

} // namespace

// ===========================================================================
// Core allocation entry points
// ===========================================================================
void* fast_malloc(std::size_t size) {
    // Zero-size policy: unique non-null pointer.
    if (size == 0) size = 1;

    if (FAST_UNLIKELY(size > MaxSmallRequest())) {
        std::size_t alloc_size = size + sizeof(LargeAllocHeader);
        if (FAST_UNLIKELY(alloc_size < size)) return nullptr; // overflow

        std::size_t page_size = EffectivePageSize();
        if (FAST_UNLIKELY(alloc_size >
                          (std::numeric_limits<std::size_t>::max)() - page_size + 1)) {
            return nullptr;
        }
        alloc_size = (alloc_size + page_size - 1) & ~(page_size - 1);

        // AllocateLargeCached handles both TLS bins and GlobalHeap Arena fallbacks
        TLSCache& tls = TLSCache::GetFast();
        void* cached_mem = tls.AllocateLargeCached(alloc_size);
        if (cached_mem) {
            // Exit gate: verify pointer plausibility before returning.
            if (FAST_UNLIKELY(!IsPlausibleHeapPtr(cached_mem))) {
                ReportWildReturnValue(cached_mem);
                return nullptr;
            }
            // Count the span's usable bytes so alloc and free statistics balance.
            LargeAllocHeader* h = reinterpret_cast<LargeAllocHeader*>(
                static_cast<char*>(cached_mem) - sizeof(LargeAllocHeader));
#if FASTALLOC_DEBUG_ENABLED
            // The cache may serve a LARGER span than requested (class fit);
            // poison/red-zone math must use the span's real size, read from
            // the header AllocateLargeCached just wrote.
            DebugOnAllocLarge(cached_mem, size, h->alloc_size);
#endif
            tls.CountLargeAlloc(h->alloc_size - sizeof(LargeAllocHeader));
            // v13.3: record the large handout so the matching free skips
            // the shared hash tier entirely (T2 contention point).
            RecordAllocHintLarge(cached_mem);
#if FASTALLOC_LOGGING_ENABLED
            LogEvent(LogLevel::Trace, "alloc-large", "ptr=%p size=%zu span=%zu",
                     cached_mem, size, alloc_size);
#endif
            return cached_mem;
        }
        return nullptr; // OOM
    }

    std::size_t class_index = RequestToClass(size);
    TLSCache& tls = TLSCache::GetFast();
    FreeBlock* block = static_cast<FreeBlock*>(tls.AllocateBlock(class_index));
    if (FAST_UNLIKELY(!block)) return nullptr;

    void* user = reinterpret_cast<char*>(block) + USER_OFFSET;
    // The pop guard in AllocateBlock applies the same plausibility predicate
    // to the block itself, which implies the user pointer passes too;
    // one guard, not two.
#if FASTALLOC_DEBUG_ENABLED
    DebugOnAllocSmall(user, size, class_index);
    // Exact requested bytes: the debug registry's free path counts rec.size.
    tls.CountSmallAlloc(size);
#else
    // Usable bytes: tracked so alloc and free statistics balance.
    tls.CountSmallAlloc(UsableSize(class_index));
    // v13.4: the alloc-site hint record moved INTO TLSCache::AllocateBlock
    // (the pop path already holds the bin's arena hint - no round trip
    // through a handout_arena_hint() field).
#endif
#if FASTALLOC_LOGGING_ENABLED
    LogEvent(LogLevel::Trace, "alloc-small", "ptr=%p size=%zu class=%zu",
             user, size, class_index);
#endif
    return user;
}

void fast_free(void* ptr) {
    if (!ptr) return;

    // Reject a non-plausible pointer before any dereference can fault:
    // sub-64KB values would hit the NULL page and non-canonical addresses
    // fault with #GP.
    //
    // The alloc-site hint filter runs first: a hit is an exact
    // (pointer >> 4) match against a handout this thread made, which proves
    // plausibility by construction and skips the 3-compare predicate plus
    // the entire map/span-cache walk. Large handouts route through the hint
    // too (their header lives on the span's first page - the line the user
    // just touched - instead of the SHARED hash tier line, which was the
    // large-workload T2 contention point).
    {
        uint32_t cls = 0, arena = 0;
        bool is_large_hint = false;
        if (FAST_LIKELY(LookupAllocHint(ptr, cls, arena, is_large_hint))) {
            TLSCache& tls = TLSCache::GetFast();
            if (FAST_LIKELY(!is_large_hint)) {
                tls.CountSmallFree(UsableSize(cls));
                tls.DeallocateBlock(
                    cls,
                    reinterpret_cast<FreeBlock*>(static_cast<char*>(ptr) - USER_OFFSET),
                    arena);
            } else {
                LargeAllocHeader* header = reinterpret_cast<LargeAllocHeader*>(
                    static_cast<char*>(ptr) - sizeof(LargeAllocHeader));
                std::size_t alloc_size = header->alloc_size;
                if (FAST_UNLIKELY(!LargeHeaderSizePlausible(alloc_size))) {
                    ReportCorruptLargeHeader(ptr, alloc_size);
                    return; // dropped (leak, once)
                }
                tls.CountLargeFree(alloc_size - sizeof(LargeAllocHeader));
                tls.DeallocateLargeCached(header, alloc_size);
            }
            return;
        }
    }

    if (FAST_UNLIKELY(!IsPlausibleHeapPtr(ptr))) {
        ReportNonCanonicalFree(ptr);
        return;
    }

#if FASTALLOC_DEBUG_ENABLED
    // ---- debug: registry/canary instrumentation via block headers -------
    FreeBlock* block = reinterpret_cast<FreeBlock*>(static_cast<char*>(ptr) - USER_OFFSET);
    void* slab_field = block->slab;

    if (FAST_UNLIKELY(slab_field == nullptr)) {
        // Large block (release discrimination).
        LargeAllocHeader* header = reinterpret_cast<LargeAllocHeader*>(
            static_cast<char*>(ptr) - sizeof(LargeAllocHeader));
        std::size_t alloc_size = header->alloc_size;
        DebugOnFreeLarge(ptr, header);
        alloc_size = header->alloc_size; // header validated; size trustworthy
        TLSCache::GetFast().CountLargeFree(alloc_size - sizeof(LargeAllocHeader));
#if FASTALLOC_LOGGING_ENABLED
        LogEvent(LogLevel::Trace, "free-large", "ptr=%p span=%zu", ptr, alloc_size);
#endif
        // Return to large allocation cache (TLS -> GlobalHeap)
        TLSCache::GetFast().DeallocateLargeCached(header, alloc_size);
        return;
    }

    if (FAST_UNLIKELY(reinterpret_cast<std::uintptr_t>(slab_field) == debug::LARGE_MAGIC)) {
        // Large block (debug discrimination: header carries the magic).
        LargeAllocHeader* header = reinterpret_cast<LargeAllocHeader*>(
            static_cast<char*>(ptr) - sizeof(LargeAllocHeader));
        std::size_t alloc_size = header->alloc_size;
        DebugOnFreeLarge(ptr, header);
        TLSCache::GetFast().CountLargeFree(alloc_size - sizeof(LargeAllocHeader));
        TLSCache::GetFast().DeallocateLargeCached(header, header->alloc_size);
        return;
    }

    if (FAST_UNLIKELY(reinterpret_cast<std::uintptr_t>(slab_field) == ALIGN_MAGIC)) {
        // Over-aligned block: recover the raw pointer from the stash and
        // recurse. The stash also lets realloc recover the original size.
        void* raw = *reinterpret_cast<void**>(static_cast<char*>(ptr) - 24);
        {
            // The registry tracks the ALIGNED pointer; rebuild the raw
            // pointer's record so fast_free(raw) validates normally.
            AllocRecord rec{};
            if (AllocRegistry::GetInstance().Erase(ptr, &rec)) {
                std::size_t alignment = *reinterpret_cast<std::size_t*>(
                    static_cast<char*>(ptr) - 32);
                rec.ptr = raw;
                rec.size = rec.size + alignment + 32;
                AllocRegistry::GetInstance().Insert(rec, nullptr);
            }
        }
        fast_free(raw);
        return;
    }

    // Small block.
    {
        AllocRecord rec = DebugOnFreeSmall(ptr, block);
        TLSCache::GetFast().CountSmallFree(rec.size);
        TLSCache::GetFast().DeallocateBlock(rec.class_or_large, block,
                                            block->slab->arena_index);
    }
#if FASTALLOC_LOGGING_ENABLED
    LogEvent(LogLevel::Trace, "free-small", "ptr=%p", ptr);
#endif
#else
    // ---- release: span-map routing (no freed-block header read) ---------
    if (FAST_LIKELY(RouteMappedFree(ptr))) return;

    // Not routed: either an INTERIOR pointer of a mapped span (over-aligned
    // stash, or garbage that happens to land inside a live span) or a
    // pointer OUTSIDE every block-bearing span (never allocated here, or
    // the span was already returned - a stale/double free).
    const SpanMap::LookupResult lr = SpanMap::Lookup(ptr);
    if (lr.entry != SpanMap::kKindNone) {
        // Interior of a mapped span: the only legitimate case is the
        // over-aligned stash. Reading the stash is safe (span is mapped).
        const std::uintptr_t magic =
            *reinterpret_cast<std::uintptr_t*>(static_cast<char*>(ptr) - 16);
        if (FAST_UNLIKELY(magic != ALIGN_MAGIC)) {
            ReportUnmappedSpanFree(ptr); // interior garbage: drop (leak, once)
            return;
        }
        void* raw = *reinterpret_cast<void**>(static_cast<char*>(ptr) - 24);
        fast_free(raw);
        return;
    }
    // Outside every span: drop WITHOUT dereferencing (the old path's
    // unconditional [ptr-16] read was its #1 crash class - v9/v10/v11).
    ReportUnmappedSpanFree(ptr);
#endif
}

void fast_free_sized(void* ptr, std::size_t size) {
    if (!ptr) return;
    (void)size;
    // The span-map free path derives everything (class, arena, slab
    // geometry) from the pointer's address, so the caller's size hint
    // saves nothing; route through fast_free for a single validated
    // implementation.
    fast_free(ptr);
}

void* fast_calloc(std::size_t num, std::size_t size) {
    if (num != 0 && size > (static_cast<std::size_t>(-1) / num)) return nullptr;
    std::size_t total = num * size;
    void* ptr = fast_malloc(total);
    if (ptr && total > 0) {
        std::memset(ptr, 0, total);
    }
    return ptr;
}

// Updates debug registry and red-zone tracking when a large span is shrunk in place.
#if FASTALLOC_DEBUG_ENABLED
namespace {
void DebugUpdateKeptLargeSpan(void* ptr, std::size_t new_size,
                               std::size_t span_usable) {
    if (span_usable < new_size) return;  // defensive; caller guarantees
    AllocRecord rec{};
    if (AllocRegistry::GetInstance().Erase(ptr, &rec)) {
        rec.size = new_size;   // logical size changed; span (rec.usable) did not
        AllocRegistry::GetInstance().Insert(rec, nullptr);
    }
    debug_check::FillRearRedZone(static_cast<char*>(ptr) + new_size,
                                 span_usable - new_size);
}
}  // namespace
#endif

void* fast_realloc(void* ptr, std::size_t new_size) {
    TLSCache::GetFast().CountRealloc();

    // C11 semantics: realloc(p, 0) == free(p), returns nullptr.
    if (new_size == 0) {
        fast_free(ptr);
        return nullptr;
    }
    if (!ptr) {
        return fast_malloc(new_size);
    }

    // Reject non-plausible pointers before reading block header.
    if (FAST_UNLIKELY(!IsPlausibleHeapPtr(ptr))) {
        ReportNonCanonicalFree(ptr);
        errno = EINVAL;
        return nullptr;
    }

#if FASTALLOC_DEBUG_ENABLED
    FreeBlock* block = reinterpret_cast<FreeBlock*>(static_cast<char*>(ptr) - USER_OFFSET);
    void* slab_field = block->slab;

    // ---- Over-aligned blocks --------------------------------------------
    if (FAST_UNLIKELY(reinterpret_cast<std::uintptr_t>(slab_field) == ALIGN_MAGIC)) {
        std::size_t alignment = *reinterpret_cast<std::size_t*>(
            static_cast<char*>(ptr) - 32);
        std::size_t old_size = *reinterpret_cast<std::size_t*>(
            static_cast<char*>(ptr) - 8);
        // Validate alignment and size fields recovered from stash.
        if (FAST_UNLIKELY(alignment < ALIGNMENT || alignment > 4096 ||
                          (alignment & (alignment - 1)) != 0 ||
                          old_size > (1ull << 30))) {
            ReportCorruptLargeHeader(ptr, old_size);
            errno = EINVAL;
            return nullptr;
        }
        if (new_size <= old_size) {
            // Keep alignment; record the new logical size in the stash so a
            // later grow-copy moves exactly the live bytes.
            *reinterpret_cast<std::size_t*>(static_cast<char*>(ptr) - 8) = new_size;
            return ptr;
        }
        void* new_ptr = fast_aligned_alloc(alignment, new_size);
        if (FAST_UNLIKELY(!new_ptr)) return nullptr;
        std::memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
        fast_free(ptr);
        return new_ptr;
    }

    bool is_large = (slab_field == nullptr);
    if (reinterpret_cast<std::uintptr_t>(slab_field) == debug::LARGE_MAGIC) is_large = true;
#else
    // ---- release: discrimination (v13.5: hint filter first) -------------
    // The block was handed out by THIS thread moments ago in the common
    // realloc-growth pattern - the hint entry carries its exact class (fixed
    // by its slab), so old_size and the shrink watermark come straight from
    // the table: no span map, no slab line.
    Slab* map_slab = nullptr;
    LargeAllocHeader* map_header = nullptr;
    uint32_t hint_cls = 0xFFFFFFFFu; // sentinel: no hint
    {
        uint32_t hcls = 0, harena = 0;
        bool hlarge = false;
        if (FAST_LIKELY(LookupAllocHint(ptr, hcls, harena, hlarge))) {
            if (FAST_LIKELY(!hlarge)) {
                hint_cls = hcls; // small: old_size = UsableSize(hcls) below
            } else {
                map_header = reinterpret_cast<LargeAllocHeader*>(
                    static_cast<char*>(ptr) - sizeof(LargeAllocHeader));
            }
        } else if (ptr >= g_span_cache_lo && ptr < g_span_cache_hi &&
                   IsBlockStart(g_span_cache_slab, ptr)) {
            map_slab = g_span_cache_slab;
        } else {
            const SpanMap::LookupResult lr = SpanMap::Lookup(ptr);
            const unsigned kind = SpanMap::Kind(lr.entry);
            if (kind == SpanMap::kKindSlab) {
                if (lr.validated) map_slab = SpanMap::Desc<Slab>(lr.entry);
            } else if (kind == SpanMap::kKindLarge) {
                if (lr.validated) map_header = SpanMap::Desc<LargeAllocHeader>(lr.entry);
            }
        }
    }
    if (FAST_UNLIKELY(!map_slab && !map_header && hint_cls == 0xFFFFFFFFu)) {
        // Not a block start / span base: aligned stash or invalid pointer.
        // (A hint hit routed the block already - hint_cls set - and takes
        // the normal small-block paths below with map_slab left null.)
        const SpanMap::LookupResult lr2 = SpanMap::Lookup(ptr);
        const bool mapped = (lr2.entry != SpanMap::kKindNone);
        if (!mapped) {
            ReportUnmappedSpanFree(ptr);
            errno = EINVAL;
            return nullptr;
        }
        const std::uintptr_t magic =
            *reinterpret_cast<std::uintptr_t*>(static_cast<char*>(ptr) - 16);
        if (FAST_UNLIKELY(magic != ALIGN_MAGIC)) {
            ReportUnmappedSpanFree(ptr); // interior garbage
            errno = EINVAL;
            return nullptr;
        }
        // ---- Over-aligned blocks (release) ------------------------------
        std::size_t alignment = *reinterpret_cast<std::size_t*>(
            static_cast<char*>(ptr) - 32);
        std::size_t old_size = *reinterpret_cast<std::size_t*>(
            static_cast<char*>(ptr) - 8);
        if (FAST_UNLIKELY(alignment < ALIGNMENT || alignment > 4096 ||
                          (alignment & (alignment - 1)) != 0 ||
                          old_size > (1ull << 30))) {
            ReportCorruptLargeHeader(ptr, old_size);
            errno = EINVAL;
            return nullptr;
        }
        if (new_size <= old_size) {
            *reinterpret_cast<std::size_t*>(static_cast<char*>(ptr) - 8) = new_size;
            return ptr;
        }
        void* new_ptr = fast_aligned_alloc(alignment, new_size);
        if (FAST_UNLIKELY(!new_ptr)) return nullptr;
        std::memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
        fast_free(ptr);
        return new_ptr;
    }
    Slab* block_slab = map_slab;      // non-null for small blocks
    bool is_large = (map_header != nullptr);
#endif

    std::size_t old_size = 0;
    if (is_large) {
#if FASTALLOC_DEBUG_ENABLED
        LargeAllocHeader* header = reinterpret_cast<LargeAllocHeader*>(
            static_cast<char*>(ptr) - sizeof(LargeAllocHeader));
#else
        LargeAllocHeader* header = map_header; // == ptr - 16 (validated)
#endif
        // Reject a zeroed or scribbled header before its bogus size flows
        // into copy lengths and shrink/grow decisions.
        if (FAST_UNLIKELY(!LargeHeaderSizePlausible(header->alloc_size))) {
            ReportCorruptLargeHeader(ptr, header->alloc_size);
            errno = EINVAL;
            return nullptr;
        }
        old_size = header->alloc_size - sizeof(LargeAllocHeader);
    } else {
#if FASTALLOC_DEBUG_ENABLED
        old_size = UsableSize(block->class_index);
#else
        old_size = (hint_cls != 0xFFFFFFFFu) ? UsableSize(hint_cls)
                                             : UsableSize(block_slab->class_idx);
#endif
    }

    if (new_size <= old_size) {
        bool should_shrink = false;
        if (FAST_LIKELY(!is_large)) {
            // Shrink watermark = what a fresh realloc-grow of new_size would
            // allocate (geometric slack, see the generic path below). A block
            // at or below the watermark stays in place; only genuinely
            // oversized blocks pay the shrink copy. Without this, a
            // slack-allocated block looks "over-allocated by >= 2 classes"
            // to the old new_class+1 rule, so every second grow step was
            // answered by a shrink copy that defeated the slack entirely
            // (and even upsized: shrink-copy -> generic path -> slack!).
            std::size_t slack_target = (new_size <= MaxSmallRequest() / 8)
                                           ? new_size * 8
                                           : MaxSmallRequest();
            std::size_t slack_class = RequestToClass(slack_target);
#if FASTALLOC_DEBUG_ENABLED
            if (block->class_index > slack_class) should_shrink = true;
#else
            // v13.5: prefer the hint class (no slab-line read); the map/slab
            // path fills block_slab for the same value.
            const std::size_t cur_class =
                (hint_cls != 0xFFFFFFFFu) ? hint_cls : block_slab->class_idx;
            if (cur_class > slack_class) should_shrink = true;
#endif
        } else {
            std::size_t page_size = EffectivePageSize();
            if (old_size - new_size >= page_size) should_shrink = true;
        }
        if (!should_shrink) {
#if FASTALLOC_DEBUG_ENABLED
            // In-place reuse (no move): the logical size changed, so the
            // registry record and the red zone must track the new size.
            // Without this, a legal write into the newly exposed region
            // would trip the OLD red zone (false positive).
            {
                AllocRecord rec{};
                if (AllocRegistry::GetInstance().Erase(ptr, &rec)) {
                    rec.size = new_size;
                    AllocRegistry::GetInstance().Insert(rec, nullptr);
                }
                if (!is_large) {
                    FreeBlock* blk = reinterpret_cast<FreeBlock*>(
                        static_cast<char*>(ptr) - USER_OFFSET);
                    std::size_t block_size = ClassIndexToSize(blk->class_index);
                    debug_check::FillRearRedZone(static_cast<char*>(ptr) + new_size,
                                                  block_size - USER_OFFSET - new_size);
                } else {
                    LargeAllocHeader* h = reinterpret_cast<LargeAllocHeader*>(
                        static_cast<char*>(ptr) - sizeof(LargeAllocHeader));
                    std::size_t usable = h->alloc_size - sizeof(LargeAllocHeader);
                    debug_check::FillRearRedZone(static_cast<char*>(ptr) + new_size,
                                                 usable - new_size);
                }
            }
#endif
            return ptr;
        }

        // In-place large shrink: keep the front pages, release
        // the tail back to the OS instead of copy+remap. Pooled spans skip
        // mremap entirely: a shrink would unmap a sub-range of a pool chunk
        // (VMA split) and the pool cannot track that; keeping the span is
        // correct and retention stays bounded by the page-cache caps.
        if (is_large) {
            LargeAllocHeader* header = reinterpret_cast<LargeAllocHeader*>(
                static_cast<char*>(ptr) - sizeof(LargeAllocHeader));
            std::size_t needed = PageRoundUp(new_size + sizeof(LargeAllocHeader));
            [[maybe_unused]] std::size_t span_usable =
                header->alloc_size - sizeof(LargeAllocHeader);
#if FASTALLOC_DEBUG_ENABLED
            // Update debug registry for retained span.
            auto debug_keep = [&]() {
                DebugUpdateKeptLargeSpan(ptr, new_size, span_usable);
            };
#else
            auto debug_keep = []() {};
#endif
            if (needed >= header->alloc_size) { debug_keep(); return ptr; } // rounding ate the gain
            char* base = reinterpret_cast<char*>(header);
#if defined(__linux__) && !defined(FASTALLOC_WINVA_EMULATION)
            if (OSMemory::IsPoolBacked(base)) { debug_keep(); return ptr; } // pooled: keep span
            // mremap shrink: releases the tail pages, never moves the base.
            std::size_t released = header->alloc_size - needed;
            void* shrunk = mremap(base, header->alloc_size, needed, 0);
            if (shrunk == MAP_FAILED) { debug_keep(); return ptr; } // keep the old span
            // shrunk == base for non-MAYMOVE calls.
            header->alloc_size = needed;
            stats::CountOsFree(released);
            // Span geometry changed: retract the released tail from the map
            // and re-publish the (same-base, smaller) span.
            SpanMap::Unmap(base, needed + released);
            SpanMap::Map(base, needed, base, SpanMap::kKindLarge);
#elif defined(_WIN32) || defined(FASTALLOC_WINVA_EMULATION)
            std::size_t released = header->alloc_size - needed;
            OSMemory::DecommitPages(base + needed, released);
            stats::CountOsFree(released);
            header->alloc_size = needed;
            // The VA stays reserved, but a wild free into the decommitted
            // tail must not route to this span's descriptor anymore.
            SpanMap::Unmap(base + needed, released);
#else
            // No in-place shrink primitive on this platform: keep the span.
            (void)base; (void)needed;
            debug_keep();
            return ptr;
#endif
#if FASTALLOC_DEBUG_ENABLED
            {
                AllocRecord rec{};
                if (AllocRegistry::GetInstance().Erase(ptr, &rec)) {
                    rec.size = new_size;   // logical size changed!
                    rec.usable = needed - sizeof(LargeAllocHeader);
                    AllocRegistry::GetInstance().Insert(rec, nullptr);
                }
                std::size_t usable = needed - sizeof(LargeAllocHeader);
                debug_check::FillRearRedZone(static_cast<char*>(ptr) + new_size,
                                             usable - new_size);
            }
#endif
            return ptr;
        }
        // Small shrink falls through to the generic copy path.
    } else if (is_large) {
        // ---- Large growth: try to grow in place first -------
        LargeAllocHeader* header = reinterpret_cast<LargeAllocHeader*>(
            static_cast<char*>(ptr) - sizeof(LargeAllocHeader));
        std::size_t needed = PageRoundUp(new_size + sizeof(LargeAllocHeader));
        if (needed > header->alloc_size) {
#if defined(__linux__) && !defined(FASTALLOC_WINVA_EMULATION)
            // Pooled spans skip mremap growth: the kernel could grow the
            // mapping in place into bump space the pool will hand out again
            // later (overlap hazard), or move it (leaving a hole in the
            // chunk). The copy path is safe for both; spans > 2 MB are never
            // pooled and keep the zero-copy fast path.
            if (!OSMemory::IsPoolBacked(reinterpret_cast<char*>(header))) {
                // Read the old span size BEFORE mremap: MREMAP_MAYMOVE can
                // relocate (and unmap) the old range, leaving 'header' a
                // dangling address for any later dereference.
                const std::size_t old_span = header->alloc_size;
                void* grown = mremap(reinterpret_cast<char*>(header), old_span,
                                     needed, MREMAP_MAYMOVE);
                if (grown != MAP_FAILED) {
                    LargeAllocHeader* new_header = static_cast<LargeAllocHeader*>(grown);
                    void* new_ptr = reinterpret_cast<char*>(new_header) + sizeof(LargeAllocHeader);
                    new_header->alloc_size = needed;
                    // The span moved and/or grew: retract the old range and
                    // publish the new one before the pointer escapes. (For an
                    // in-place grow the extra pages were never mapped, so the
                    // wider unmap is a harmless no-op on empty entries.
                    // Unmap/Map only use addresses as keys - no dereference
                    // of the possibly-moved old header.)
                    SpanMap::Unmap(reinterpret_cast<char*>(header), old_span);
                    SpanMap::Map(new_header, needed, new_header, SpanMap::kKindLarge);
#if FASTALLOC_DEBUG_ENABLED
                    {
                        // Move the registry record to the new address.
                        AllocRecord rec{};
                        if (AllocRegistry::GetInstance().Erase(ptr, &rec)) {
                            rec.ptr = new_ptr;
                            rec.size = new_size;   // logical size changed!
                            rec.usable = needed - sizeof(LargeAllocHeader);
                            AllocRegistry::GetInstance().Insert(rec, nullptr);
                        }
                        std::size_t usable = needed - sizeof(LargeAllocHeader);
                        debug_check::FillRearRedZone(static_cast<char*>(new_ptr) + new_size,
                                                     usable - new_size);
                    }
#endif
                    return new_ptr;
                }
                // mremap failed: fall through to the copy path.
            }
#endif
        } else {
            // Same page count: the span already has room (old behaviour).
#if FASTALLOC_DEBUG_ENABLED
            {
                AllocRecord rec{};
                if (AllocRegistry::GetInstance().Erase(ptr, &rec)) {
                    rec.size = new_size;
                    AllocRegistry::GetInstance().Insert(rec, nullptr);
                }
                LargeAllocHeader* h = reinterpret_cast<LargeAllocHeader*>(
                    static_cast<char*>(ptr) - sizeof(LargeAllocHeader));
                std::size_t usable = h->alloc_size - sizeof(LargeAllocHeader);
                debug_check::FillRearRedZone(static_cast<char*>(ptr) + new_size,
                                             usable - new_size);
            }
#endif
            return ptr;
        }
    }

    // ---- Generic path: allocate, copy, free --------------------------------
    // Geometric slack (realloc-grow optimization, v13.5 8x): a small-block
    // grow that leaves its size class allocates the new block with 8x
    // headroom (capped at the largest small class). A following 1.5x-2x
    // growth pattern then lands inside the SAME class and takes the
    // in-place return above instead of copying again - the slab equivalent
    // of glibc's top-chunk slack (which is unbounded) and of std::vector's
    // 1.5-2x geometric growth, just with a larger constant. Amortized copy
    // volume for the bench's 1.5x-to-8KB pattern drops from ~7.2 KB to
    // ~0.9 KB per growth cycle. Plain fast_malloc() is unchanged (exact
    // fit); only realloc growth buys headroom, and the headroom is
    // reclaimed on the next class-crossing grow, at free, or by the shrink
    // watermark above (which uses the same 8x factor).
    void* new_ptr = nullptr;
    if (FAST_LIKELY(!is_large) && new_size <= MaxSmallRequest()) {
        const std::size_t slack_target =
            (new_size <= MaxSmallRequest() / 8) ? new_size * 8 : MaxSmallRequest();
        new_ptr = fast_malloc(slack_target);
    }
    if (FAST_UNLIKELY(!new_ptr)) {
        new_ptr = fast_malloc(new_size); // large grow, or OOM retry at exact size
    }
    if (new_ptr) {
#if FASTALLOC_DEBUG_ENABLED
        // Copy only the user's active bytes.
        std::size_t copy_bytes = old_size;
        AllocRecord rec{};
        if (AllocRegistry::GetInstance().Erase(ptr, &rec)) {
            copy_bytes = rec.size;
            // Re-insert: fast_free below expects the record to be live.
            AllocRegistry::GetInstance().Insert(rec, nullptr);
        }
        std::memcpy(new_ptr, ptr, (copy_bytes < new_size) ? copy_bytes : new_size);
#else
        std::memcpy(new_ptr, ptr, (old_size < new_size) ? old_size : new_size);
#endif
        fast_free(ptr);
    }
    return new_ptr;
}

// ===========================================================================
// Aligned allocation
// ===========================================================================
void* fast_aligned_alloc(std::size_t alignment, std::size_t size) {
    if (size == 0) size = 1; // zero-size policy: unique pointer

    // Validate: power of two, sane range.
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;
    if (alignment < ALIGNMENT) alignment = ALIGNMENT;   // natural minimum
    if (alignment > 4096) return nullptr;               // supported maximum

    if (alignment <= ALIGNMENT) {
        return fast_malloc(size); // already 16-byte aligned
    }

    // Over-allocate so the stash (32 bytes) plus alignment slack fits inside
    // the raw block's user region.
    std::size_t raw_size = size + alignment + 32;
    if (FAST_UNLIKELY(raw_size < size)) return nullptr; // overflow

    void* raw = fast_malloc(raw_size);
    if (FAST_UNLIKELY(!raw)) return nullptr;

    char* aligned = reinterpret_cast<char*>(
        (reinterpret_cast<std::uintptr_t>(raw) + 32 + alignment - 1) & ~(alignment - 1));

    // Stash: [alignment][raw][ALIGN_MAGIC][size]
    *reinterpret_cast<std::size_t*>(aligned - 32) = alignment;
    *reinterpret_cast<void**>(aligned - 24) = raw;
    *reinterpret_cast<std::uintptr_t*>(aligned - 16) = ALIGN_MAGIC;
    *reinterpret_cast<std::size_t*>(aligned - 8) = size;

#if FASTALLOC_DEBUG_ENABLED
    {
        // Track the ALIGNED pointer (replacing the raw record fast_malloc made).
        AllocRecord rec{};
        if (AllocRegistry::GetInstance().Erase(raw, &rec)) {
            rec.ptr = aligned;
            rec.size = size;
            rec.usable = rec.usable; // raw usable; aligned checks ride on raw
            AllocRegistry::GetInstance().Insert(rec, nullptr);
        }
    }
#endif
    return aligned;
}

// ===========================================================================
// Diagnostics entry points
// ===========================================================================
FastAllocStats fast_alloc_stats() {
    TLSCache::GetFast().FlushStats(); // make the calling thread's counts visible
    FastAllocStats out{};
    stats::Snapshot(out);
    return out;
}

void fast_alloc_dump_leaks() {
    FastDumpLeaksImpl();
}

void fast_alloc_purge() {
    GlobalHeap::GetInstance().PurgePageCache();
}

void fast_alloc_purge_thread_cache() {
    TLSCache::GetFast().FlushToGlobalHeap();
}

void fast_alloc_audit_pointers(const void* lo, const void* hi) {
    if (!lo || !hi || lo >= hi) return;
    std::fprintf(stderr,
                 "[fastalloc-audit] scanning allocator head arrays for pointers "
                 "into [%p, %p)\n",
                 const_cast<void*>(lo), const_cast<void*>(hi));
    std::fflush(stderr);
    // Forensic reads only (fixed head arrays, no foreign dereferences):
    // safe while the allocator is in any state. See fast_alloc.h.
    std::size_t hits = AuditCurrentThreadCache(lo, hi);
    hits += GlobalHeap::GetInstance().AuditPointers(lo, hi);
    if (hits == 0) {
        std::fprintf(stderr,
                     "[fastalloc-audit] no allocator head pointer targets the "
                     "region (writer held its pointer only transiently, e.g. "
                     "register/stack, or the corrupted link is mid-list)\n");
    } else {
        std::fprintf(stderr,
                     "[fastalloc-audit] %zu pointer(s) target the region - "
                     "the listed structure(s) are the corruption carriers\n",
                     hits);
    }
    std::fflush(stderr);
}

void fast_alloc_log_set_level(int level) {
    if (level < 0) level = 0;
    if (level > 5) level = 5;
    LogSetLevel(static_cast<LogLevel>(level));
}

} // namespace FastAlloc

// ===========================================================================
// Optional global operator overrides
// ===========================================================================
#ifdef FAST_ALLOC_OVERRIDE_NEW
void* operator new(std::size_t size) {
    void* ptr = FastAlloc::fast_malloc(size);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}
void* operator new[](std::size_t size) {
    void* ptr = FastAlloc::fast_malloc(size);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}
void operator delete(void* ptr) noexcept {
    FastAlloc::fast_free(ptr);
}
void operator delete[](void* ptr) noexcept {
    FastAlloc::fast_free(ptr);
}
void operator delete(void* ptr, std::size_t size) noexcept {
    // Forward the size hint to sized deallocation.
    FastAlloc::fast_free_sized(ptr, size);
}
void operator delete[](void* ptr, std::size_t size) noexcept {
    FastAlloc::fast_free_sized(ptr, size);
}
#endif

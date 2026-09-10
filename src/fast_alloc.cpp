#include "fast_alloc.h"
#include "tls_cache.h"
#include "global_heap.h"
#include "fast_alloc_config.h"
#include "os_memory.h"
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
    // Audit N1 fix: the large-allocation rounding must use the RUNTIME page
    // size, not the compile-time constant. On 64KB-page systems the old code
    // recorded 4KB-rounded sizes in headers while the actual mappings were
    // 64KB-rounded -> munmap with wrong sizes and corrupted page-bin entries.
    return OSMemory::GetPageSize();
}

// v8 hardening telemetry (see fast_free): one-shot stderr on the first
// out-of-range class index observed per process, silent afterwards.
//
// v9: compiled ONLY when its (release-path) call site exists. FASTALLOC_DEBUG
// builds validate provenance through the registry instead and never call
// this function, which left it unreferenced - Clang treats an unused static
// inline function as -Wunused-function (a hard error under our -Werror) and
// failed the ubuntu-latest-clang-debug CI job (Sep 2026). GCC does not warn
// for unused static inline functions, which is why only the Clang job saw
// it. Guarding the definition with the same condition as the call site is
// the structural fix; no [[maybe_unused]] band-aid that would outlive the
// call site it protects.
#if !FASTALLOC_DEBUG_ENABLED
inline void ReportCorruptClassIndex(void* ptr, std::uint32_t class_index) {
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_expect(class_index < NUM_SIZE_CLASSES, 1)) return;
#endif
    // Atomic one-shot: two threads can observe the same corruption; the
    // exchange guarantees exactly one full report line, race-free (TSan).
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[fastalloc-invariant] corrupt block header at %p: "
                     "class_index=%u >= %zu; block dropped (leaked), allocator "
                     "state protected\n",
                     ptr, class_index, NUM_SIZE_CLASSES);
        std::fflush(stderr);
    }
}
#endif // !FASTALLOC_DEBUG_ENABLED

// v9 crash-survival guard: the lowest address any FastAlloc user pointer can
// ever have. Blocks live inside mmap'd / pooled spans (multi-MB regions far
// above this line); Linux keeps [0, 64KB) unmapped and Windows reserves the
// first 64 KB as the null-pointer zone, so nothing below 64 KB can be a
// valid block in either world. (v10: moved to fast_alloc_config.h as
// kMinCanonicalUserPtr; v11: the local alias was superseded entirely by
// IsPlausibleHeapPtr and removed - one predicate, one home.)

// v10 hardening: is 'alloc_size' a value the large path can trust? A live
// large header always carries the page-rounded SPAN size (>= one page, a
// page multiple, sane magnitude). A block whose header was zeroed
// (madvise'd span) or scribbled reads 0 / garbage here; the OLD code
// trusted it unconditionally, then pushed the bogus entry into the TLS
// large cache, from which AllocateLargeCached could later hand it out as
// fresh memory - a silent cross-arena aliasing machine. The check is one
// compare on an already-loaded word.
inline bool LargeHeaderSizePlausible(std::size_t alloc_size) {
    const std::size_t page = EffectivePageSize();
    return alloc_size >= page && (alloc_size % page) == 0 &&
           alloc_size < (1ull << 48);
}

// v10: one-shot telemetry when a forged/zeroed header is rejected on the
// large path (fast_free / fast_realloc). Referenced from BOTH the debug and
// release discrimination paths, so it is compiled unconditionally - no
// -Wunused-function exposure under FASTALLOC_DEBUG (the v9 lesson).
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

// One-shot telemetry for the v9/v11 guard: the Sep-2026 runner crashes
// faulted exactly on this class of value. v9: the benchmark's pointer array
// held 0x10 and the inlined fast_free read its header at [0x10 - 16] = 0x0,
// a SIGSEGV at the NULL page. v10 (closing datapoint): the array held
// 0xd90b6085fe368400 - 16-aligned, >= 64 KB, NON-CANONICAL - which passed
// every v10 check, and [ptr-16] landed in the non-canonical half: the CPU
// raises #GP and the kernel reports si_code=SI_KERNEL(128) with
// si_addr=0 ("faulting address (nil)", reproduced 3/3 on the runner,
// 0/650+ locally). The guard now applies the FULL plausibility predicate
// (>= 64 KB, < 2^47, 16-aligned) and drops the poisoned pointer (a
// one-block leak, once) so the run completes and the event lands in the
// CI job log; the benchmark-side store-time witness (bench_main.cpp)
// distinguishes "allocator returned it" from "slot overwritten after the
// store" on the same occurrence.
inline void ReportNonCanonicalFree(void* ptr) {
    // Atomic one-shot: exactly one report line even when several threads
    // trip the guard simultaneously (race-free under TSan).
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

// v11: one-shot telemetry when fast_malloc/fast_aligned_alloc is about to
// RETURN a value that fails the plausibility predicate. Defense-in-depth:
// the pop/batch guards upstream should make this unreachable, but the
// runner's codegen surprises (the v10 binary even dropped the bench's
// alignment check from the hot path) justify a final gate on every exit.
// The block is leaked (once) and nullptr is returned instead of handing a
// wild pointer to the caller.
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
    // Zero-size policy (audit C6): unique non-null pointer, like glibc.
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
            // v11 final gate (same rationale as the small path): never hand
            // the caller a non-plausible pointer.
            if (FAST_UNLIKELY(!IsPlausibleHeapPtr(cached_mem))) {
                ReportWildReturnValue(cached_mem);
                return nullptr;
            }
            // Count the span's usable bytes (header->alloc_size is the exact
            // page-rounded span): fast_free counts the same quantity, so
            // alloc/free stay balanced and current_live_bytes never
            // underflows (it used to: alloc counted the REQUESTED size while
            // free counted the SPAN size).
            LargeAllocHeader* h = reinterpret_cast<LargeAllocHeader*>(
                static_cast<char*>(cached_mem) - sizeof(LargeAllocHeader));
#if FASTALLOC_DEBUG_ENABLED
            // The cache may serve a LARGER span than requested (class fit);
            // poison/red-zone math must use the span's real size, read from
            // the header AllocateLargeCached just wrote.
            DebugOnAllocLarge(cached_mem, size, h->alloc_size);
#endif
            tls.CountLargeAlloc(h->alloc_size - sizeof(LargeAllocHeader));
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
    // v11 final gate: never hand the caller a pointer that fails the full
    // plausibility predicate. Upstream pop/batch guards make this belt-and-
    // braces, but the v10 runner run proved codegen can defeat expectations;
    // this is the last exit before the value escapes into user code (and
    // into the benchmark's slot array, where the v10 crash was born).
    if (FAST_UNLIKELY(!IsPlausibleHeapPtr(user))) {
        ReportWildReturnValue(user);
        return nullptr;
    }
#if FASTALLOC_DEBUG_ENABLED
    DebugOnAllocSmall(user, size, class_index);
    // Exact requested bytes: the debug registry's free path counts rec.size.
    tls.CountSmallAlloc(size);
#else
    // Usable bytes: the release free path counts UsableSize(class) too, so
    // per-block alloc/free counts match and current_live_bytes cannot
    // underflow (it previously wrapped to ~16 EB after enough churn).
    tls.CountSmallAlloc(UsableSize(class_index));
#endif
#if FASTALLOC_LOGGING_ENABLED
    LogEvent(LogLevel::Trace, "alloc-small", "ptr=%p size=%zu class=%zu",
             user, size, class_index);
#endif
    return user;
}

void fast_free(void* ptr) {
    if (!ptr) return;

    // v9/v11 guard (see ReportNonCanonicalFree): reject a non-plausible
    // pointer BEFORE the header read can fault. v9 caught sub-64KB values
    // (0x10 -> [ptr-16] = the NULL page); v11 completes the predicate with
    // the upper canonicality bound and alignment - the v10 runner crash
    // dereferenced 0xd90b6085fe368400-16 in the non-canonical half of the
    // address space, which faults with #GP (si_code=SI_KERNEL=128,
    // si_addr=0 - the "faulting address (nil)" signature).
    if (FAST_UNLIKELY(!IsPlausibleHeapPtr(ptr))) {
        ReportNonCanonicalFree(ptr);
        return;
    }

    FreeBlock* block = reinterpret_cast<FreeBlock*>(static_cast<char*>(ptr) - USER_OFFSET);
    void* slab_field = block->slab;

    if (FAST_UNLIKELY(slab_field == nullptr)) {
        // Large block (release discrimination).
        LargeAllocHeader* header = reinterpret_cast<LargeAllocHeader*>(
            static_cast<char*>(ptr) - sizeof(LargeAllocHeader));
        std::size_t alloc_size = header->alloc_size;
#if FASTALLOC_DEBUG_ENABLED
        DebugOnFreeLarge(ptr, header);
        alloc_size = header->alloc_size; // header validated; size trustworthy
        TLSCache::GetFast().CountLargeFree(alloc_size - sizeof(LargeAllocHeader));
#else
        // v10 guard: a zeroed (madvise'd span) or scribbled header reads a
        // non-span alloc_size here. Trusting it fed the TLS large cache a
        // bogus entry (size 0 / garbage), which AllocateLargeCached could
        // later serve - handing out a pointer into foreign memory. Dropping
        // the pointer leaks one block once; the cache stays clean.
        if (FAST_UNLIKELY(!LargeHeaderSizePlausible(alloc_size))) {
            ReportCorruptLargeHeader(ptr, alloc_size);
            return;
        }
        TLSCache::GetFast().CountLargeFree(alloc_size - sizeof(LargeAllocHeader));
#endif
#if FASTALLOC_LOGGING_ENABLED
        LogEvent(LogLevel::Trace, "free-large", "ptr=%p span=%zu", ptr, alloc_size);
#endif
        // Return to large allocation cache (TLS -> GlobalHeap)
        TLSCache::GetFast().DeallocateLargeCached(header, alloc_size);
        return;
    }

#if FASTALLOC_DEBUG_ENABLED
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
#endif

    if (FAST_UNLIKELY(reinterpret_cast<std::uintptr_t>(slab_field) == ALIGN_MAGIC)) {
        // Over-aligned block: recover the raw pointer from the stash and
        // recurse. The stash also lets realloc recover the original size.
        void* raw = *reinterpret_cast<void**>(static_cast<char*>(ptr) - 24);
#if FASTALLOC_DEBUG_ENABLED
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
#endif
        fast_free(raw);
        return;
    }

    // Small block.
#if FASTALLOC_DEBUG_ENABLED
    AllocRecord rec = DebugOnFreeSmall(ptr, block);
    TLSCache::GetFast().CountSmallFree(rec.size);
    TLSCache::GetFast().DeallocateBlock(rec.class_or_large, block);
#else
    // v8 hardening: the class index comes from the block header; a corrupted
    // value >= NUM_SIZE_CLASSES would index past the 512-entry TLS bins_
    // array (and CACHE_LIMITS) and silently corrupt the thread cache object.
    // The debug registry catches provenance; this is the release-side bound.
    // A corrupted header cannot be routed safely, so the block is dropped
    // (a leak, once) instead of corrupting the allocator.
    if (FAST_UNLIKELY(block->class_index >= NUM_SIZE_CLASSES)) {
        ReportCorruptClassIndex(ptr, block->class_index);
        return;
    }
    TLSCache& tls = TLSCache::GetFast();
    tls.CountSmallFree(UsableSize(block->class_index));
    tls.DeallocateBlock(block->class_index, block);
#endif
#if FASTALLOC_LOGGING_ENABLED
    LogEvent(LogLevel::Trace, "free-small", "ptr=%p", ptr);
#endif
}

void fast_free_sized(void* ptr, std::size_t size) {
    if (!ptr) return;

    // v9/v11 guard: same full-plausibility rejection as fast_free before
    // the header read (this path also loads block->slab first).
    if (FAST_UNLIKELY(!IsPlausibleHeapPtr(ptr))) {
        ReportNonCanonicalFree(ptr);
        return;
    }

    if (FAST_UNLIKELY(size > MaxSmallRequest())) {
        fast_free(ptr); // large or aligned: full path
        return;
    }

    FreeBlock* block = reinterpret_cast<FreeBlock*>(static_cast<char*>(ptr) - USER_OFFSET);

    // If the header suggests this is not actually a plain small block
    // (mismatched size hint, aligned block or large span), fall back to the
    // fully general path instead of corrupting the cache.
    void* slab_field = block->slab;
    if (FAST_UNLIKELY(slab_field == nullptr ||
                      reinterpret_cast<std::uintptr_t>(slab_field) == ALIGN_MAGIC)) {
        fast_free(ptr);
        return;
    }
#if FASTALLOC_DEBUG_ENABLED
    if (reinterpret_cast<std::uintptr_t>(slab_field) == debug::LARGE_MAGIC) {
        fast_free(ptr);
        return;
    }
    // Registry is the source of truth for the class; also cross-checks the hint.
    fast_free(ptr);
    return;
#endif

    // Release fast path: the size hint saves the header->class_index load
    // and its dependent-use latency (audit O1 / M3).
    std::size_t class_index = RequestToClass(size);
    // v10: cross-check the hint against the block's OWN header (same
    // already-touched 16 bytes, one 4-byte load + compare). A wrong hint
    // used to push the block into a foreign TLS bin: the next allocation
    // from that bin returned an undersized block, and the user's first
    // write overflowed into the NEXT block's header - the canonical seed
    // of freelist corruption (0x10-class garbage in pointers). A mismatch
    // now routes through fast_free, which trusts only the header.
    if (FAST_UNLIKELY(static_cast<std::size_t>(block->class_index) != class_index)) {
        fast_free(ptr);
        return;
    }
    TLSCache::GetFast().DeallocateBlock(class_index, block);
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

    // v9/v11 guard: fast_realloc reads block->slab immediately below; reject
    // a non-plausible pointer (full predicate: <64KB / >=2^47 / misaligned)
    // instead of faulting on its "header".
    if (FAST_UNLIKELY(!IsPlausibleHeapPtr(ptr))) {
        ReportNonCanonicalFree(ptr);
        errno = EINVAL;
        return nullptr;
    }

    FreeBlock* block = reinterpret_cast<FreeBlock*>(static_cast<char*>(ptr) - USER_OFFSET);
    void* slab_field = block->slab;

    // ---- Over-aligned blocks ------------------------------------------------
    if (FAST_UNLIKELY(reinterpret_cast<std::uintptr_t>(slab_field) == ALIGN_MAGIC)) {
        std::size_t alignment = *reinterpret_cast<std::size_t*>(
            static_cast<char*>(ptr) - 32);
        std::size_t old_size = *reinterpret_cast<std::size_t*>(
            static_cast<char*>(ptr) - 8);
        // v11: the stash lives in the raw block's user area - user data can
        // corrupt it (that is its threat model). A forged alignment would
        // make fast_aligned_alloc return nullptr (it validates power-of-two
        // range), but a forged old_size would drive the memcpy length below
        // straight into unmapped memory. Reject absurd values outright.
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
#if FASTALLOC_DEBUG_ENABLED
    if (reinterpret_cast<std::uintptr_t>(slab_field) == debug::LARGE_MAGIC) is_large = true;
#endif

    std::size_t old_size = 0;
    if (is_large) {
        LargeAllocHeader* header = reinterpret_cast<LargeAllocHeader*>(
            static_cast<char*>(ptr) - sizeof(LargeAllocHeader));
        // v10: reject a zeroed/scribbled header BEFORE its bogus size flows
        // into copy lengths and shrink/grow decisions. (Common path, both
        // configs: in DEBUG the slab==nullptr discrimination is normally
        // only reached with LARGE_MAGIC headers, so an implausible size
        // here already means the header is corrupt.)
        if (FAST_UNLIKELY(!LargeHeaderSizePlausible(header->alloc_size))) {
            ReportCorruptLargeHeader(ptr, header->alloc_size);
            errno = EINVAL;
            return nullptr;
        }
        old_size = header->alloc_size - sizeof(LargeAllocHeader);
    } else {
        old_size = UsableSize(block->class_index);
    }

    if (new_size <= old_size) {
        bool should_shrink = false;
        if (FAST_LIKELY(!is_large)) {
            std::size_t new_class_index = RequestToClass(new_size);
            if (block->class_index > new_class_index + 1) should_shrink = true;
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

        // In-place large shrink (audit M2/O5): keep the front pages, release
        // the tail back to the OS instead of copy+remap. Pooled spans skip
        // mremap entirely: a shrink would unmap a sub-range of a pool chunk
        // (VMA split) and the pool cannot track that; keeping the span is
        // correct and retention stays bounded by the page-cache caps.
        if (is_large) {
            LargeAllocHeader* header = reinterpret_cast<LargeAllocHeader*>(
                static_cast<char*>(ptr) - sizeof(LargeAllocHeader));
            std::size_t needed = PageRoundUp(new_size + sizeof(LargeAllocHeader));
            if (needed >= header->alloc_size) return ptr; // rounding ate the gain
            char* base = reinterpret_cast<char*>(header);
#if defined(__linux__) && !defined(FASTALLOC_WINVA_EMULATION)
            if (OSMemory::IsPoolBacked(base)) return ptr; // pooled: keep span
            // mremap shrink: releases the tail pages, never moves the base.
            std::size_t released = header->alloc_size - needed;
            void* shrunk = mremap(base, header->alloc_size, needed, 0);
            if (shrunk == MAP_FAILED) return ptr; // keep the old span
            // shrunk == base for non-MAYMOVE calls.
            header->alloc_size = needed;
            stats::CountOsFree(released);
#elif defined(_WIN32) || defined(FASTALLOC_WINVA_EMULATION)
            std::size_t released = header->alloc_size - needed;
            OSMemory::DecommitPages(base + needed, released);
            stats::CountOsFree(released);
            header->alloc_size = needed;
#else
            // No in-place shrink primitive on this platform: keep the span.
            (void)base; (void)needed;
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
        // ---- Large growth: try to grow in place first (audit M2/O5) -------
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
                void* grown = mremap(reinterpret_cast<char*>(header), header->alloc_size,
                                     needed, MREMAP_MAYMOVE);
                if (grown != MAP_FAILED) {
                    LargeAllocHeader* new_header = static_cast<LargeAllocHeader*>(grown);
                    void* new_ptr = reinterpret_cast<char*>(new_header) + sizeof(LargeAllocHeader);
                    new_header->alloc_size = needed;
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
    void* new_ptr = fast_malloc(new_size);
    if (new_ptr) {
#if FASTALLOC_DEBUG_ENABLED
        // Copy only the user's real bytes (audit N5: previously up to the
        // full class-usable size, dragging red-zone garbage into the copy).
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
// Aligned allocation (audit M7)
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
    // Audit O1/M3: honour the sized-deallocation hint.
    FastAlloc::fast_free_sized(ptr, size);
}
void operator delete[](void* ptr, std::size_t size) noexcept {
    FastAlloc::fast_free_sized(ptr, size);
}
#endif

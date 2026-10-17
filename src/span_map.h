#pragma once
// ============================================================================
// SpanMap: address -> span-descriptor routing for the release free path.
// ----------------------------------------------------------------------------
// PURPOSE: fast_free must route a freed pointer to its size class WITHOUT
// reading the freed block's header. That header lives in the freed block's
// own cacheline, which for a live set larger than cache is a guaranteed
// high-latency BLOCKING load on EVERY free (measured: 2x throughput gap vs
// jemalloc on large-live-set churn). Routing by address touches only hot,
// shared metadata lines instead.
//
// TWO TIERS:
//  1. A direct-mapped HASH table (2^18 x 8B slots, static). One multiply,
//     one load. Collisions (two spans hashing to one slot) simply evict the
//     older entry; lookups then fall through to the radix. Because spans
//     are DISJOINT address ranges, the block-start geometry check below is
//     a complete validation: a colliding/stale slab can never contain the
//     queried pointer, so it fails the range check and the radix answers.
//  2. A 3-level RADIX (root 2^15 -> mid 2^10 -> leaf 2^10, 4KB pages,
//     covers the full 47-bit user VA) - always correct, never loses
//     entries, used as the authoritative fallback.
//
// ENTRY ENCODING (uintptr_t):
//   0                      page not part of any block-bearing span
//   (Slab*)span       | 1  small-block slab (span base IS the Slab header)
//   (LargeAllocHeader*)| 2  large span (header at span base, user = base+16)
//   bit 2                    pool-backed flag (span caches may hold it)
// Descriptors are page aligned, so the low three bits are free.
//
// CONCURRENCY: entries are release-published atomics; hash slots are
// single-word (torn-read free); radix node creation is allocate-then-CAS
// (losers leak one 8 KB node - rare and bounded). Unmapping races only
// with lookups of pointers that cannot be legitimately freed anymore.
// ============================================================================
#include <cstdint>
#include <cstddef>
#include <atomic>

#include "fast_alloc_config.h"
#include "slab.h"
#include "os_memory.h"

namespace FastAlloc {

class SpanMap {
public:
    static constexpr std::uintptr_t kKindNone  = 0;
    static constexpr std::uintptr_t kKindSlab  = 1;
    static constexpr std::uintptr_t kKindLarge = 2;
    static constexpr std::uintptr_t kFlagPoolBacked = 4;

    // v13.7: slab entries carry the span's size class in bits 3..11
    // (page-aligned descriptors leave the low 12 bits free; 9 bits = 512
    // classes). A free can then obtain (span identity + class) from ONE map
    // line - the slab header line is not touched at all.
    static constexpr std::uintptr_t kEntryDescMask = ~static_cast<std::uintptr_t>(0xFFF);
    static inline unsigned Kind(std::uintptr_t entry) { return (unsigned)(entry & 3); }
    static inline bool PoolBacked(std::uintptr_t entry) { return (entry & kFlagPoolBacked) != 0; }
    static inline uint32_t EntryClass(std::uintptr_t entry) {
        return static_cast<uint32_t>((entry >> 3) & 0x1FF);
    }
    template <class T>
    static inline T* Desc(std::uintptr_t entry) {
        return reinterpret_cast<T*>(entry & kEntryDescMask);
    }

    // ------------------------------------------------------------------------
    // Division-free "is ptr exactly a block USER start of slab" (see
    // fast_alloc_config.h CLASS_MAGICS for the exactness proof). Reads only
    // the slab header's first cacheline. Because spans are disjoint, a pass
    // here also proves 'slab' is the RIGHT slab (no other span contains ptr)
    // - which is what makes the direct-mapped hash tier sound.
    // ------------------------------------------------------------------------
    static inline bool IsBlockStart(const Slab* slab, const void* ptr) {
        const char* base = reinterpret_cast<const char*>(slab);
        const std::size_t d = slab->block_size;
        // Corrupt/zeroed slab header (e.g. madvise'd span): reject.
        if (FAST_UNLIKELY(d < ALIGNMENT || d > MAX_SLAB_SIZE ||
                          (d & (ALIGNMENT - 1)) != 0)) {
            return false;
        }
        const std::size_t off = static_cast<std::size_t>(
            reinterpret_cast<const char*>(ptr) -
            (base + slab->first_block_offset + USER_OFFSET));
        // Bounds; also rejects underflow (ptr below the first block) because
        // a wrapped difference is astronomically larger than any span.
        if (FAST_UNLIKELY(off >= slab->memory_size)) return false;
        const std::uint64_t q = (off * CLASS_MAGICS[d / ALIGNMENT - 1]) >> 34;
        return q < slab->total_blocks && q * d == off;
    }

    // Validate an entry against the pointer (kind-aware).
    static inline bool Validates(std::uintptr_t entry, const void* ptr) {
        const unsigned kind = Kind(entry);
        if (kind == kKindSlab) {
            return IsBlockStart(Desc<Slab>(entry), ptr);
        }
        if (kind == kKindLarge) {
            const LargeAllocHeader* h = Desc<LargeAllocHeader>(entry);
            return ptr == reinterpret_cast<const char*>(h) + sizeof(LargeAllocHeader);
        }
        return false;
    }

    struct LookupResult {
        std::uintptr_t entry;   // 0 = unmapped
        bool validated;         // entry is DEFINITELY this pointer's span
                                // (block-start / large-base geometry check
                                // passed against it)
    };

    // ------------------------------------------------------------------------
    // Lookup: hash tier first (one load), radix fallback. The result's
    // 'validated' is true when the entry passed the geometry check; an
    // unvalidated non-zero entry means "inside a mapped span but not a
    // block start / span base" (over-aligned stash or interior garbage).
    // ------------------------------------------------------------------------
    static inline LookupResult Lookup(const void* ptr) {
        const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(ptr);
        const std::uint32_t pn = static_cast<std::uint32_t>(a >> 12);
        // 1) hash tier
        {
            const std::size_t h = HashPage(pn);
            const std::uintptr_t e = hash_[h].load(std::memory_order_acquire);
            if (e != kKindNone) {
                if (Validates(e, ptr)) return LookupResult{e, true};
                // collision or non-block-start: fall through to the radix,
                // which either validates the true span or proves the entry.
            }
        }
        // 2) radix tier
        {
            const std::uintptr_t e = RadixLookup(pn);
            if (e == kKindNone) return LookupResult{0, false};
            return LookupResult{e, Validates(e, ptr)};
        }
    }

    // ------------------------------------------------------------------------
    // v13.1: fast identity check for span reuse. A span parked in the page
    // cache keeps its map entries (see PageHeapFree); if the first page's
    // hash entry still equals the exact (desc|kind|pool) the new role is
    // about to publish, the full per-page Map walk can be skipped entirely -
    // this is what makes page-cache round-trips O(1) instead of O(pages)
    // (measured ~2 us per 512 KB span round-trip, the v13 'large' 4x
    // regression). Soundness: a mismatching/colliding entry simply forces the
    // full Map; the radix tier is never stale for a still-mapped span, so
    // pages whose hash slots were lost to collisions route via the radix.
    // ------------------------------------------------------------------------
    static inline bool EntryMatches(void* base, void* desc, unsigned kind,
                                    bool pool_backed, uint32_t cls = 0) {
        std::uintptr_t entry = reinterpret_cast<std::uintptr_t>(desc) | kind;
        if (pool_backed) entry |= kFlagPoolBacked;
        entry |= static_cast<std::uintptr_t>(cls) << 3;
        const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(base);
        const std::uint32_t pn = static_cast<std::uint32_t>(a >> 12);
        return hash_[HashPage(pn)].load(std::memory_order_acquire) == entry;
    }

    // v13.1: poison a parked span's validation fields so a wild/double free
    // that still routes to the span's (kept) map entry is REJECTED by the
    // plausibility checks instead of pushing the parked span into a cache a
    // second time (double presence -> corruption). Zero is the sentinel:
    // block_size 0 fails IsBlockStart's d < ALIGNMENT check; alloc_size 0
    // fails LargeHeaderSizePlausible's alloc_size >= page check. The header
    // is rewritten by Slab::Create / AllocateLargeCached on reuse.
    // The RADIX entry (authoritative, never a collision victim) identifies
    // the span's true descriptor; the hash slot may hold a colliding span.
    static inline void PoisonParkedSpan(void* base) {
        const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(base);
        const std::uint32_t pn = static_cast<std::uint32_t>(a >> 12);
        const std::uintptr_t e = RadixLookup(pn);
        if (e == kKindNone) return;
        if (Desc<void>(e) != base) return; // stale/foreign: not ours to poison
        const unsigned kind = Kind(e);
        if (kind == kKindSlab) {
            Desc<Slab>(e)->block_size = 0;
        } else if (kind == kKindLarge) {
            Desc<LargeAllocHeader>(e)->alloc_size = 0;
        }
    }

    // Mark every page of [base, base+bytes) with (desc|kind[|pool]). Bytes
    // must be page multiples and base page aligned.
    //
    // v13: the hash tier is populated for EVERY page of the span, not just
    // the first. The old single-representative design meant a pointer on
    // page 2..N of a multi-page span probed an unrelated slot and fell to
    // the radix ~94% of the time for 64 KB slabs (~100% for large spans) -
    // measured as the v12 'large' regression. Colliding pages simply evict
    // the older entry; the loser falls to the radix (correct, slower).
    //
    // v13.7: slab entries carry the size class (bits 3..11) so a free can
    // route without touching the slab header line.
    static void Map(void* base, std::size_t bytes, void* desc, unsigned kind,
                    bool pool_backed = false, uint32_t cls = 0) {
        std::uintptr_t entry = reinterpret_cast<std::uintptr_t>(desc) | kind;
        if (pool_backed) entry |= kFlagPoolBacked;
        entry |= static_cast<std::uintptr_t>(cls) << 3;
        // Hash tier: one slot per page (Fibonacci hashing spreads consecutive
        // page numbers; a span's own pages rarely self-collide, and if they
        // do the store is idempotent - same entry value).
        {
            const std::size_t page = OSMemory::GetPageSize();
            const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(base);
            const std::uintptr_t end = a + bytes;
            for (std::uintptr_t p = a; p < end; p += page) {
                const std::uint32_t pn = static_cast<std::uint32_t>(p >> 12);
                hash_[HashPage(pn)].store(entry, std::memory_order_release);
            }
        }
        // Radix tier: authoritative, never loses entries.
        ForEachPage(base, bytes, [&](std::atomic<std::uintptr_t>& slot) {
            slot.store(entry, std::memory_order_release);
        });
    }

    // Clear every page of [base, base+bytes).
    static void Unmap(void* base, std::size_t bytes) {
        const std::size_t page = OSMemory::GetPageSize();
        const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(base);
        const std::uintptr_t end = a + bytes;
        // v13: clear EVERY page's hash slot (mirrors Map), but only when the
        // slot still holds an entry of THIS span - a newer span's entry must
        // survive our departure.
        for (std::uintptr_t p = a; p < end; p += page) {
            const std::uint32_t pn = static_cast<std::uint32_t>(p >> 12);
            const std::size_t h = HashPage(pn);
            std::uintptr_t expect = hash_[h].load(std::memory_order_relaxed);
            if (expect != kKindNone && Desc<void>(expect) == base) {
                hash_[h].compare_exchange_strong(expect, kKindNone,
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed);
            }
        }
        ForEachPage(base, bytes, [](std::atomic<std::uintptr_t>& slot) {
            slot.store(kKindNone, std::memory_order_relaxed);
        });
    }

private:
    static constexpr std::size_t kHashSlots = 1u << 18; // 2 MB static, dense

    static inline std::size_t HashPage(std::uint32_t pn) {
        // Fibonacci hashing: uniform for page numbers of mapped regions.
        return static_cast<std::size_t>(
                   (static_cast<std::uint64_t>(pn) * 0x9E3779B97F4A7C15ull) >> 46) &
               (kHashSlots - 1);
    }

    static inline std::uintptr_t RadixLookup(std::uint32_t pn) {
        Mid* mid = root_[pn >> 20].load(std::memory_order_acquire);
        if (FAST_UNLIKELY(mid == nullptr)) return kKindNone;
        Leaf* leaf = mid->children[(pn >> 10) & 0x3FF].load(std::memory_order_acquire);
        if (FAST_UNLIKELY(leaf == nullptr)) return kKindNone;
        return leaf->pages[pn & 0x3FF].load(std::memory_order_acquire);
    }

    struct Leaf {
        std::atomic<std::uintptr_t> pages[1024];
    };
    struct Mid {
        std::atomic<Leaf*> children[1024];
    };

    template <class F>
    static void ForEachPage(void* base, std::size_t bytes, F fn) {
        const std::size_t page = OSMemory::GetPageSize();
        const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(base);
        const std::uintptr_t end = a + bytes;
        for (std::uintptr_t p = a; p < end; p += page) {
            const std::uint32_t pn = static_cast<std::uint32_t>(p >> 12);
            Mid* mid = EnsureMid(pn >> 20);
            if (FAST_UNLIKELY(!mid)) continue; // node OOM: lookups miss (safe drop)
            Leaf* leaf = EnsureLeaf(*mid, (pn >> 10) & 0x3FF);
            if (FAST_UNLIKELY(!leaf)) continue;
            fn(leaf->pages[pn & 0x3FF]);
        }
    }

    static Mid* EnsureMid(std::uint32_t idx) {
        Mid* mid = root_[idx].load(std::memory_order_acquire);
        if (FAST_LIKELY(mid != nullptr)) return mid;
        Mid* fresh = static_cast<Mid*>(OSMemory::AllocatePages(sizeof(Mid)));
        if (!fresh) return nullptr; // lookup will miss -> caller drops; safe
        for (auto& c : fresh->children) c.store(nullptr, std::memory_order_relaxed);
        Mid* expected = nullptr;
        if (root_[idx].compare_exchange_strong(expected, fresh,
                                               std::memory_order_release,
                                               std::memory_order_acquire)) {
            return fresh;
        }
        OSMemory::FreePages(fresh, sizeof(Mid)); // lost the race
        return expected;
    }

    static Leaf* EnsureLeaf(Mid& mid, std::uint32_t idx) {
        Leaf* leaf = mid.children[idx].load(std::memory_order_acquire);
        if (FAST_LIKELY(leaf != nullptr)) return leaf;
        Leaf* fresh = static_cast<Leaf*>(OSMemory::AllocatePages(sizeof(Leaf)));
        if (!fresh) return nullptr;
        for (auto& p : fresh->pages) p.store(kKindNone, std::memory_order_relaxed);
        Leaf* expected = nullptr;
        if (mid.children[idx].compare_exchange_strong(expected, fresh,
                                                      std::memory_order_release,
                                                      std::memory_order_acquire)) {
            return fresh;
        }
        OSMemory::FreePages(fresh, sizeof(Leaf)); // lost the race
        return expected;
    }

    static std::atomic<std::uintptr_t> hash_[kHashSlots]; // primary tier
    static std::atomic<Mid*> root_[32768];                // radix root (256 KB)
};

} // namespace FastAlloc

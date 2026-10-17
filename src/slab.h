#pragma once
#include <cstddef>
#include <cstdint>

#include "fast_alloc_config.h"

namespace FastAlloc {

struct Slab;

/**
 * @brief Node for intrusive singly-linked free list
 *
 * Layout (LP64):  slab @0, class_index @8, canary @12, next @16 -> 24 bytes.
 * User pointer is block + USER_OFFSET (16); user bytes overlap 'next' only.
 * 'slab' / 'class_index' are immutable after Slab::Create and stay valid in
 * every list the block visits (slab list, TLS cache, pending MPSC queue),
 * which is what makes the O(1) free path possible.
 *
 * RELEASE free path: the block header is NOT read on free anymore (the span
 * map routes by address - see span_map.h); reading it was a guaranteed cache
 * miss per free for live sets larger than cache. The header remains for the
 * debug instrumentation (canary / registry) and internal list walking.
 *
 * DEBUG: 'canary' (formerly padding) holds a front canary written at
 * allocation time and validated at free time to catch buffer underflows.
 */
struct FreeBlock {
    Slab* slab;           // Always valid (never overwritten by user data)
    uint32_t class_index; // Store class index
    // RELEASE (v13.7): owning arena index, written once at lazy wiring -
    // the free path reads it from this line (which the freelist push
    // brings into cache anyway) instead of the slab header line.
    // DEBUG: front canary, written at allocation, validated at free.
    uint32_t canary;
    FreeBlock* next;      // Overwritten by user data when allocated!
};

/**
 * @brief A Slab manages a large chunk of contiguous memory (e.g. OS Page),
 * dividing it into fixed-size blocks.
 *
 * WIRING (release): blocks are wired lazily. Slab::Create only initializes
 * the header (O(1), touches one page); Allocate() pops the free list or
 * carves the next virgin block, writing its 12-byte header on first use.
 * This avoids eagerly writing total_blocks headers across every page of the
 * span (a 64 KB / 32 B slab = 2048 headers on 16 pages) - which dominated
 * thread-lifecycle and slab-churn workloads. DEBUG builds keep the eager
 * full wiring (plus FRESH pattern) so poisoning checks see a fully
 * initialized slab exactly as before.
 */
struct Slab {
    // ---- cacheline 0 (bytes 0..39): span-map free-path validation ----
    // The v12 release free path validates a freed pointer against these six
    // fields and nothing else, so they are packed into the first cacheline:
    // one line - shared by every block of the slab - serves every free.
    std::size_t block_size;
    std::size_t total_blocks;
    std::size_t memory_size;
    std::size_t first_block_offset; // offset of block 0 from 'this'
    uint32_t arena_index;
    uint32_t class_idx;             // cached class index for lazy carving
    // ---- cacheline 1: allocator slow-path fields ----
    Slab* next; // Intrusive list pointer to link slabs of the same size class
    Slab* prev; // Added for O(1) removal
    FreeBlock* free_list;
    std::size_t free_blocks;
    std::size_t next_new;           // bump cursor: virgin blocks wired so far
#if FASTALLOC_DEBUG_ENABLED
    uint64_t magic;     // Validated on every free for pointer provenance
#endif

    /**
     * @brief Formats a raw memory buffer into a Slab.
     * The Slab header is placed at the beginning of the memory.
     * @param memory Pointer to raw aligned memory mapping.
     * @param memory_size The total size of the memory mapping.
     * @param block_size The size of each element in the slab.
     * @param arena_index The arena that owns this slab.
     * @return Pointer to the initialized Slab.
     */
    static Slab* Create(void* memory, std::size_t memory_size, std::size_t block_size, uint32_t arena_index);

    /**
     * @brief Allocates an object from this slab.
     * @return Pointer to object, or nullptr if slab is full.
     */
    inline void* Allocate() {
#if FASTALLOC_DEBUG_ENABLED
        // Invariant check: verify free blocks available.
        assert_fast(free_blocks > 0 && (free_list != nullptr || next_new < total_blocks),
                    "Allocate from exhausted slab");
#endif
        if (FAST_LIKELY(free_list != nullptr)) {
            FreeBlock* block = free_list;
            free_list = block->next;
            free_blocks--;
            return block;
        }
        if (FAST_LIKELY(next_new < total_blocks)) {
            // Carve the next virgin block (release lazy wiring).
            FreeBlock* block = reinterpret_cast<FreeBlock*>(
                reinterpret_cast<char*>(this) + first_block_offset + next_new * block_size);
            block->slab = this;
            block->class_index = class_idx;
#if FASTALLOC_DEBUG_ENABLED
            block->canary = 0; // debug: front canary written at allocation
#else
            // v13.7: release routing stores the owning arena here - the
            // block's own header line is brought in by every free's
            // freelist push anyway, so (slab, class, arena) can be read for
            // free and a free never touches the slab header line.
            block->canary = arena_index;
#endif
            next_new++;
            free_blocks--;
            return block;
        }
        return nullptr;
    }

    /**
     * @brief Returns an object to this slab's free list.
     * @param ptr The pointer to return.
     */
    inline void Deallocate(void* ptr) {
        FreeBlock* block = static_cast<FreeBlock*>(ptr);
#if FASTALLOC_DEBUG_ENABLED
        assert_fast(block->slab == this, "Block returned to wrong slab");
        assert_fast(free_blocks < total_blocks, "Double free / free_blocks overflow");
        assert_fast(free_list != block, "Double return of the free-list head");
#endif
        if (FAST_UNLIKELY(free_list == block)) {
            // Guard against double return of current free list head.
            return;
        }
        block->next = free_list;
        free_list = block;
        free_blocks++;
    }

    bool IsFull() const { return free_blocks == 0; }
    bool IsEmpty() const { return free_blocks == total_blocks; }

private:
#if FASTALLOC_DEBUG_ENABLED
    static void assert_fast(bool cond, const char* msg); // -> ReportViolation
#endif
};

} // namespace FastAlloc

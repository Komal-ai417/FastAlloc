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
 * DEBUG: 'canary' (formerly padding) holds a front canary written at
 * allocation time and validated at free time to catch buffer underflows.
 */
struct FreeBlock {
    Slab* slab;           // Always valid (never overwritten by user data)
    uint32_t class_index; // Store class index
    uint32_t canary;      // DEBUG front canary; padding in release builds
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
    Slab* next; // Intrusive list pointer to link slabs of the same size class
    Slab* prev; // Added for O(1) removal
    FreeBlock* free_list;
    std::size_t block_size;
    std::size_t total_blocks;
    std::size_t free_blocks;
    std::size_t memory_size;
    std::size_t first_block_offset; // offset of block 0 from 'this'
    std::size_t next_new;           // bump cursor: virgin blocks wired so far
    uint32_t arena_index;
    uint32_t class_idx;             // cached class index for lazy carving
#if FASTALLOC_DEBUG_ENABLED
    uint64_t magic;     // Validated on every free (audit C3: pointer provenance)
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
        // Audit H1/QA-report: these invariants are now actually implemented.
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
            block->canary = 0;
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
        // Audit H1/QA-report: runtime asserts that were claimed to exist.
        assert_fast(block->slab == this, "Block returned to wrong slab");
        assert_fast(free_blocks < total_blocks, "Double free / free_blocks overflow");
#endif
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

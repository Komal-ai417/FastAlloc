#include "slab.h"
#include "fast_alloc_config.h"
#include "debug_aid.h"

#include <cassert>
#include <cstring>

namespace FastAlloc {

#if FASTALLOC_DEBUG_ENABLED
void Slab::assert_fast(bool cond, const char* msg) {
    if (FAST_UNLIKELY(!cond)) {
        ViolationInfo info{};
        info.kind = Violation::InternalInvariant;
        info.ptr = nullptr;
        info.size = 0;
        info.where = msg;
        info.caller = FAST_RETURN_ADDRESS();
        info.has_record = 0;
        ReportViolation(info);
    }
}
#endif

Slab* Slab::Create(void* memory, std::size_t memory_size, std::size_t block_size, uint32_t arena_index) {
    // Need space for header + at least one block
    if (!memory || memory_size <= sizeof(Slab) + block_size) {
        return nullptr;
    }

    // Place the Slab header at the very beginning of the raw memory
    Slab* slab = static_cast<Slab*>(memory);
    slab->next = nullptr;
    slab->prev = nullptr;
    slab->block_size = block_size;
    slab->memory_size = memory_size;
    slab->arena_index = arena_index;
    slab->class_idx = static_cast<uint32_t>(SizeToClassIndex(block_size));
#if FASTALLOC_DEBUG_ENABLED
    slab->magic = debug::SLAB_MAGIC;
#endif

    // Determine the offset for user blocks perfectly aligned
    std::size_t offset = (sizeof(Slab) + ALIGNMENT - 1) & ~(ALIGNMENT - 1);

    if (memory_size < offset + block_size) {
        return nullptr;
    }

    std::size_t available_memory = memory_size - offset;

    slab->total_blocks = available_memory / block_size;
    slab->free_blocks = slab->total_blocks;
    slab->free_list = nullptr;
    slab->first_block_offset = offset;

    char* block_start = static_cast<char*>(memory) + offset;

#if FASTALLOC_DEBUG_ENABLED
    // DEBUG: eager full wiring (unchanged behaviour - the poison/fresh
    // pattern machinery expects every block initialized at creation).
    // Iterate backwards so the free list doles out lower addresses first
    // (good for cache locality)
    slab->next_new = slab->total_blocks; // all blocks pre-wired
    for (std::size_t i = slab->total_blocks; i > 0; --i) {
        FreeBlock* block = reinterpret_cast<FreeBlock*>(block_start + (i - 1) * block_size);
        block->slab = slab;
        block->class_index = slab->class_idx;
        block->next = slab->free_list;
        slab->free_list = block;
        // Fresh-block pattern for everything the free-list link does not use:
        // the link occupies block+16..24, canary lives at block+12.
        block->canary = 0;
        if (block_size > sizeof(FreeBlock)) {
            std::memset(reinterpret_cast<char*>(block) + sizeof(FreeBlock),
                        debug::FRESH, block_size - sizeof(FreeBlock));
        }
    }
#else
    // RELEASE: lazy wiring - O(1) creation, blocks are carved (and their
    // 12-byte headers written) on first allocation. Only the slab header
    // page is touched at creation, so short-lived threads that use a
    // handful of blocks out of a 2048-block slab fault one page instead
    // of all sixteen.
    slab->next_new = 0;
    (void)block_start;
#endif

    return slab;
}

} // namespace FastAlloc

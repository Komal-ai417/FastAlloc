#include "slab.h"
#include "fast_alloc_config.h"
#include <cassert>

namespace FastAlloc {

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

    // Determine the offset for user blocks perfectly aligned
    std::size_t offset = (sizeof(Slab) + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    
    if (memory_size < offset + block_size) {
        return nullptr;
    }
    
    std::size_t available_memory = memory_size - offset;
    
    slab->total_blocks = available_memory / block_size;
    slab->free_blocks = slab->total_blocks;
    slab->free_list = nullptr;

    char* block_start = static_cast<char*>(memory) + offset;

    std::size_t class_index = SizeToClassIndex(block_size);

    // Wire up the intrusive free list
    // Iterate backwards so the free list doles out lower addresses first (good for cache locality)
    for (std::size_t i = slab->total_blocks; i > 0; --i) {
        FreeBlock* block = reinterpret_cast<FreeBlock*>(block_start + (i - 1) * block_size);
        block->slab = slab;
        block->class_index = static_cast<uint32_t>(class_index);
        block->next = slab->free_list;
        slab->free_list = block;
    }

    return slab;
}



} // namespace FastAlloc
#include "slab.h"
#include "fast_alloc_config.h"
#include <cassert>

namespace FastAlloc {

Slab* Slab::Create(void* memory, std::size_t memory_size, std::size_t block_size) {
    // Need space for header + at least one block
    if (!memory || memory_size <= sizeof(Slab) + block_size) {
        return nullptr;
    }

    // Place the Slab header at the very beginning of the raw memory
    Slab* slab = static_cast<Slab*>(memory);
    slab->next = nullptr;
    slab->block_size = block_size;
    slab->memory_size = memory_size;

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

    // Wire up the intrusive free list
    // Iterate backwards so the free list doles out lower addresses first (good for cache locality)
    for (std::size_t i = slab->total_blocks; i > 0; --i) {
        FreeBlock* block = reinterpret_cast<FreeBlock*>(block_start + (i - 1) * block_size);
        block->slab = slab;
        block->next = slab->free_list;
        slab->free_list = block;
    }

    return slab;
}

void* Slab::Allocate() {
    if (free_blocks == 0 || free_list == nullptr) {
        return nullptr;
    }
    
    // Pop from head of the free list
    FreeBlock* block = free_list;
    free_list = block->next;
    free_blocks--;
    
    return block;
}

void Slab::Deallocate(void* ptr) {
    assert(ptr != nullptr);
    
    // Push onto head of free list
    FreeBlock* block = static_cast<FreeBlock*>(ptr);
    block->next = free_list;
    free_list = block;
    free_blocks++;
}

} // namespace FastAlloc

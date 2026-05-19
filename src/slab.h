#pragma once
#include <cstddef>
#include <cstdint>

namespace FastAlloc {

struct Slab;

/**
 * @brief Node for intrusive singly-linked free list
 */
struct FreeBlock {
    Slab* slab;       // Always valid (never overwritten by user data)
    uint32_t class_index; // Store class index
    uint32_t _padding; // Explicit padding
    FreeBlock* next;  // Overwritten by user data when allocated!
};

/**
 * @brief A Slab manages a large chunk of contiguous memory (e.g. OS Page),
 * dividing it into fixed-size blocks.
 */
struct Slab {
    Slab* next; // Intrusive list pointer to link slabs of the same size class
    Slab* prev; // Added for O(1) removal
    FreeBlock* free_list;
    std::size_t block_size;
    std::size_t total_blocks;
    std::size_t free_blocks;
    std::size_t memory_size;
    uint32_t arena_index;

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
        if (free_blocks == 0) return nullptr;
        FreeBlock* block = free_list;
        free_list = block->next;
        free_blocks--;
        return block;
    }

    /**
     * @brief Returns an object to this slab's free list.
     * @param ptr The pointer to return.
     */
    inline void Deallocate(void* ptr) {
        FreeBlock* block = static_cast<FreeBlock*>(ptr);
        block->next = free_list;
        free_list = block;
        free_blocks++;
    }

    bool IsFull() const { return free_blocks == 0; }
    bool IsEmpty() const { return free_blocks == total_blocks; }
};

} // namespace FastAlloc
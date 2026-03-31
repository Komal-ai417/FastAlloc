#pragma once
#include <cstddef>

namespace FastAlloc {

struct Slab;

/**
 * @brief Node for intrusive singly-linked free list
 */
struct FreeBlock {
    Slab* slab;       // Always valid (never overwritten by user data)
    char _padding[16 - sizeof(Slab*)]; // Explicit 16-byte offset padding
    FreeBlock* next;  // Overwritten by user data when allocated!
};

/**
 * @brief A Slab manages a large chunk of contiguous memory (e.g. OS Page),
 * dividing it into fixed-size blocks.
 */
struct Slab {
    Slab* next; // Intrusive list pointer to link slabs of the same size class
    FreeBlock* free_list;
    std::size_t block_size;
    std::size_t total_blocks;
    std::size_t free_blocks;
    std::size_t memory_size;

    /**
     * @brief Formats a raw memory buffer into a Slab.
     * The Slab header is placed at the beginning of the memory.
     * @param memory Pointer to raw aligned memory mapping.
     * @param memory_size The total size of the memory mapping.
     * @param block_size The size of each element in the slab.
     * @return Pointer to the initialized Slab.
     */
    static Slab* Create(void* memory, std::size_t memory_size, std::size_t block_size);

    /**
     * @brief Allocates an object from this slab.
     * @return Pointer to object, or nullptr if slab is full.
     */
    void* Allocate();

    /**
     * @brief Returns an object to this slab's free list.
     * @param ptr The pointer to return.
     */
    void Deallocate(void* ptr);

    bool IsFull() const { return free_blocks == 0; }
    bool IsEmpty() const { return free_blocks == total_blocks; }
};

} // namespace FastAlloc

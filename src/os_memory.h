#pragma once
#include <cstddef>

namespace FastAlloc {

class OSMemory {
public:
    /**
     * @brief Allocates raw memory pages from the OS.
     * Guaranteed to be page-aligned.
     * @param size Size in bytes. Must be a multiple of page size.
     * @return Pointer to allocated memory, or nullptr on failure.
     */
    static void* AllocatePages(std::size_t size);

    /**
     * @brief Frees memory pages back to the OS.
     * @param ptr Pointer previously returned by AllocatePages.
     * @param size Size in bytes allocated.
     */
    static void FreePages(void* ptr, std::size_t size);

    /**
     * @brief Retrieves the OS page size (typically 4096 bytes).
     * @return The system page size.
     */
    static std::size_t GetPageSize();
};

} // namespace FastAlloc

#pragma once
#include <cstddef>

#if defined(_WIN32)
#if defined(FASTALLOC_EXPORTS)
#define FASTALLOC_API __declspec(dllexport)
#else
#define FASTALLOC_API __declspec(dllimport)
#endif
#else
#define FASTALLOC_API __attribute__((visibility("default")))
#endif

// Usually we compile as static lib, so we can just use empty macro
#undef FASTALLOC_API
#define FASTALLOC_API

namespace FastAlloc {

/**
 * @brief Allocates size bytes of memory from a thread-local cache.
 */
FASTALLOC_API void* fast_malloc(std::size_t size);

/**
 * @brief Deallocates memory previously allocated by fast_malloc.
 */
FASTALLOC_API void fast_free(void* ptr);

/**
 * @brief Allocates and zero-initializes memory.
 */
FASTALLOC_API void* fast_calloc(std::size_t num, std::size_t size);

/**
 * @brief Reallocates memory to a new size, copying existing contents if necessary.
 */
FASTALLOC_API void* fast_realloc(void* ptr, std::size_t new_size);

} // namespace FastAlloc

// Optional global operator overrides (usually provided in a separate file or conditionally enabled)
#ifdef FAST_ALLOC_OVERRIDE_NEW
void* operator new(std::size_t size);
void* operator new[](std::size_t size);
void operator delete(void* ptr) noexcept;
void operator delete[](void* ptr) noexcept;
#endif

#pragma once
#include <cstddef>
#include <cstdint>
#include <new>
#include <cstdlib>

#if defined(_WIN32)
#if defined(FASTALLOC_EXPORTS)
#define FASTALLOC_API __declspec(dllexport)
#else
#define FASTALLOC_API __declspec(dllimport)
#endif
#else
#define FASTALLOC_API __attribute__((visibility("default")))
#endif

#undef FASTALLOC_API
#define FASTALLOC_API

#if defined(_MSC_VER)
#define FAST_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define FAST_THREAD_LOCAL __thread
#else
#define FAST_THREAD_LOCAL thread_local
#endif

#if defined(__GNUC__) || defined(__clang__)
#define FAST_LIKELY(x) __builtin_expect(!!(x), 1)
#define FAST_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define FAST_LIKELY(x) (x)
#define FAST_UNLIKELY(x) (x)
#endif

namespace FastAlloc {

constexpr std::size_t USER_OFFSET = 16;
constexpr std::size_t MAX_SLAB_SIZE = 8192;
constexpr uint32_t FAST_ALLOC_DEBUG_CANARY = 0xFA57A110;

struct Slab;

struct FreeBlock {
    Slab* slab;
    uint32_t class_index;
#ifdef FAST_ALLOC_DEBUG
    uint32_t canary;
#else
    uint32_t _padding;
#endif
    FreeBlock* next;
};

struct alignas(64) CacheBin {
    FreeBlock* head;
    uint32_t count;
    uint32_t limit;
    uint8_t _pad[48];
};

extern FAST_THREAD_LOCAL CacheBin* fast_bins;

FASTALLOC_API void* fast_malloc_slow(std::size_t size);
FASTALLOC_API void  fast_free_slow(void* ptr);
FASTALLOC_API void* fast_calloc(std::size_t num, std::size_t size);
FASTALLOC_API void* fast_realloc(void* ptr, std::size_t new_size);

inline void* fast_malloc(std::size_t size) {
    std::size_t total_size = size + USER_OFFSET;
    if (FAST_LIKELY(total_size <= MAX_SLAB_SIZE)) {
        std::size_t class_index = (total_size - 1) >> 4;
        CacheBin* bins = fast_bins;
        if (FAST_LIKELY(bins != nullptr)) {
            CacheBin& bin = bins[class_index];
            FreeBlock* block = bin.head;
            if (FAST_LIKELY(block != nullptr)) {
                bin.head = block->next;
                bin.count--;
#ifdef FAST_ALLOC_DEBUG
                block->canary = FAST_ALLOC_DEBUG_CANARY;
#endif
                return reinterpret_cast<char*>(block) + USER_OFFSET;
            }
        }
    }
    return fast_malloc_slow(size);
}

inline void fast_free(void* ptr) {
    if (FAST_UNLIKELY(!ptr)) return;
    FreeBlock* block = reinterpret_cast<FreeBlock*>(static_cast<char*>(ptr) - USER_OFFSET);
    
#ifdef FAST_ALLOC_DEBUG
    if (FAST_UNLIKELY(block->canary != FAST_ALLOC_DEBUG_CANARY)) {
        std::abort();
    }
    block->canary = 0;
#endif

    if (FAST_LIKELY(block->slab != nullptr)) {
        CacheBin* bins = fast_bins;
        if (FAST_LIKELY(bins != nullptr)) {
            std::size_t class_index = block->class_index;
            CacheBin& bin = bins[class_index];
            block->next = bin.head;
            bin.head = block;
            bin.count++;
            if (FAST_LIKELY(bin.count < bin.limit)) {
                return;
            }
        }
    }
    fast_free_slow(ptr);
}

} // namespace FastAlloc

// Optional global operator overrides (usually provided in a separate file or conditionally enabled)
#ifdef FAST_ALLOC_OVERRIDE_NEW
#pragma message("FastAlloc: FAST_ALLOC_OVERRIDE_NEW replaces the global allocator — verify no other allocator overrides are linked")
inline void* operator new(std::size_t size) {
    void* ptr = FastAlloc::fast_malloc(size);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}
inline void* operator new[](std::size_t size) {
    void* ptr = FastAlloc::fast_malloc(size);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}
inline void operator delete(void* ptr) noexcept {
    FastAlloc::fast_free(ptr);
}
inline void operator delete[](void* ptr) noexcept {
    FastAlloc::fast_free(ptr);
}
inline void operator delete(void* ptr, std::size_t) noexcept {
    FastAlloc::fast_free(ptr);
}
inline void operator delete[](void* ptr, std::size_t) noexcept {
    FastAlloc::fast_free(ptr);
}
#endif

#include "fast_alloc.h"
#include "tls_cache.h"
#include "global_heap.h"
#include "fast_alloc_config.h"
#include "os_memory.h"

#include <cstring>
#include <new>
#include <limits>
namespace FastAlloc {

constexpr std::size_t USER_OFFSET = 16;
static_assert(sizeof(LargeAllocHeader) == 16, "Header size must align to USER_OFFSET");

void* fast_malloc(std::size_t size) {
    if (size == 0) return nullptr;

    if (FAST_UNLIKELY(size > MAX_SLAB_SIZE - USER_OFFSET)) {
        std::size_t alloc_size = size + sizeof(LargeAllocHeader);
        if (FAST_UNLIKELY(alloc_size < size)) return nullptr; // overflow
        
        std::size_t page_size = PAGE_SIZE;
        if (FAST_UNLIKELY(alloc_size > std::numeric_limits<std::size_t>::max() - page_size + 1)) return nullptr;
        alloc_size = (alloc_size + page_size - 1) & ~(page_size - 1);
        
        // AllocateLargeCached handles both TLS bins and GlobalHeap Arena fallbacks
        void* cached_mem = TLSCache::GetFast().AllocateLargeCached(alloc_size);
        if (cached_mem) return cached_mem;
        
        return nullptr;
    }

    std::size_t class_index = SizeToClassIndex(size + USER_OFFSET);
    FreeBlock* block = static_cast<FreeBlock*>(TLSCache::GetFast().AllocateBlock(class_index));
    if (FAST_UNLIKELY(!block)) return nullptr;

    return reinterpret_cast<char*>(block) + USER_OFFSET;
}

void fast_free(void* ptr) {
    if (!ptr) return;

    FreeBlock* block = reinterpret_cast<FreeBlock*>(static_cast<char*>(ptr) - USER_OFFSET);

    if (FAST_UNLIKELY(block->slab == nullptr)) {
        LargeAllocHeader* header = reinterpret_cast<LargeAllocHeader*>(
            static_cast<char*>(ptr) - sizeof(LargeAllocHeader));
        std::size_t alloc_size = header->alloc_size;
        
        // Return to large allocation cache (TLS -> GlobalHeap)
        TLSCache::GetFast().DeallocateLargeCached(header, alloc_size);
        return;
    }

    TLSCache::GetFast().DeallocateBlock(block->class_index, block);
}

void* fast_calloc(std::size_t num, std::size_t size) {
    if (num != 0 && size > (static_cast<std::size_t>(-1) / num)) return nullptr;
    std::size_t total = num * size;
    void* ptr = fast_malloc(total);
    if (ptr) {
        std::memset(ptr, 0, total);
    }
    return ptr;
}

void* fast_realloc(void* ptr, std::size_t new_size) {
    if (new_size == 0) {
        fast_free(ptr);
        return nullptr;
    }
    if (!ptr) {
        return fast_malloc(new_size);
    }

    std::size_t old_size = 0;
    FreeBlock* block = reinterpret_cast<FreeBlock*>(static_cast<char*>(ptr) - USER_OFFSET);
    Slab* slab = block->slab;

    if (FAST_UNLIKELY(slab == nullptr)) {
        LargeAllocHeader* header = reinterpret_cast<LargeAllocHeader*>(
            static_cast<char*>(ptr) - sizeof(LargeAllocHeader));
        old_size = header->alloc_size - sizeof(LargeAllocHeader);
    } else {
        old_size = ClassIndexToSize(block->class_index) - USER_OFFSET;
    }

    if (new_size <= old_size) {
        bool should_shrink = false;
        if (FAST_LIKELY(slab != nullptr)) {
            std::size_t new_class_index = SizeToClassIndex(new_size + USER_OFFSET);
            if (block->class_index > new_class_index + 1) should_shrink = true;
        } else {
            std::size_t page_size = PAGE_SIZE;
            if (old_size - new_size >= page_size) should_shrink = true;
        }
        if (!should_shrink) return ptr;
    }

    void* new_ptr = fast_malloc(new_size);
    if (new_ptr) {
        std::memcpy(new_ptr, ptr, (old_size < new_size) ? old_size : new_size);
        fast_free(ptr);
    }
    return new_ptr;
}

} // namespace FastAlloc 

#ifdef FAST_ALLOC_OVERRIDE_NEW
void* operator new(std::size_t size) {
    void* ptr = FastAlloc::fast_malloc(size);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}
void* operator new[](std::size_t size) {
    void* ptr = FastAlloc::fast_malloc(size);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}
void operator delete(void* ptr) noexcept {
    FastAlloc::fast_free(ptr);
}
void operator delete[](void* ptr) noexcept {
    FastAlloc::fast_free(ptr);
}
void operator delete(void* ptr, std::size_t) noexcept {
    FastAlloc::fast_free(ptr);
}
void operator delete[](void* ptr, std::size_t) noexcept {
    FastAlloc::fast_free(ptr);
}
#endif
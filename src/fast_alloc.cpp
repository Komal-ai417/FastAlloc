#include "fast_alloc.h"
#include "tls_cache.h"
#include "global_heap.h"
#include "fast_alloc_config.h"
#include "os_memory.h"

#include <cstring>
#include <new>

namespace FastAlloc {

constexpr std::size_t USER_OFFSET = 16;
static_assert(sizeof(LargeAllocHeader) == 16, "Header size must align to USER_OFFSET");

void* fast_malloc(std::size_t size) {
    if (size == 0) return nullptr;

    std::size_t total_size = size + USER_OFFSET;
    if (total_size < size) return nullptr; // Integer overflow check
    
    if (total_size > MAX_SLAB_SIZE) {
        std::size_t alloc_size = size + sizeof(LargeAllocHeader);
        if (alloc_size < size) return nullptr;
        
        // OPT-5: Try large allocation cache
        void* cached_mem = TLSCache::GetFast().AllocateLargeCached(alloc_size);
        if (cached_mem) return cached_mem;
        
        void* mem = GlobalHeap::GetInstance().AllocateLarge(alloc_size);
        if (!mem) return nullptr;
        
        LargeAllocHeader* header = static_cast<LargeAllocHeader*>(mem);
        header->alloc_size = alloc_size;
        header->slab = nullptr; 
        
        return reinterpret_cast<char*>(mem) + sizeof(LargeAllocHeader); 
    }

    std::size_t class_index = SizeToClassIndex(total_size);
    FreeBlock* block = static_cast<FreeBlock*>(TLSCache::GetFast().AllocateBlock(class_index));
    if (!block) return nullptr;

    return reinterpret_cast<char*>(block) + USER_OFFSET;
}

void fast_free(void* ptr) {
    if (!ptr) return;

    Slab** slab_ptr = reinterpret_cast<Slab**>(static_cast<char*>(ptr) - USER_OFFSET);
    Slab* slab = *slab_ptr;

    if (slab == nullptr) {
        LargeAllocHeader* header = reinterpret_cast<LargeAllocHeader*>(
            static_cast<char*>(ptr) - sizeof(LargeAllocHeader));
        std::size_t alloc_size = header->alloc_size;
        
        // OPT-5: Return to large allocation cache
        TLSCache::GetFast().DeallocateLargeCached(header, alloc_size);
        return;
    }

    std::size_t class_index = SizeToClassIndex(slab->block_size);
    FreeBlock* block = reinterpret_cast<FreeBlock*>(static_cast<char*>(ptr) - USER_OFFSET);
    TLSCache::GetFast().DeallocateBlock(class_index, block);
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
    Slab** slab_ptr = reinterpret_cast<Slab**>(static_cast<char*>(ptr) - USER_OFFSET);
    Slab* slab = *slab_ptr;

    if (slab == nullptr) {
        LargeAllocHeader* header = reinterpret_cast<LargeAllocHeader*>(
            static_cast<char*>(ptr) - sizeof(LargeAllocHeader));
        old_size = header->alloc_size - sizeof(LargeAllocHeader);
    } else {
        old_size = slab->block_size - USER_OFFSET;
    }

    if (new_size <= old_size) {
        bool should_shrink = false;
        if (slab != nullptr) {
            std::size_t new_class_index = SizeToClassIndex(new_size + USER_OFFSET);
            std::size_t old_class_index = SizeToClassIndex(slab->block_size);
            if (old_class_index > new_class_index + 1) should_shrink = true;
        } else {
            std::size_t page_size = OSMemory::GetPageSize();
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

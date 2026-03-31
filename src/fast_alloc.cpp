#include "fast_alloc.h"
#include "tls_cache.h"
#include "global_heap.h"
#include "fast_alloc_config.h"

#include <cstring>
#include <new>

namespace FastAlloc {

void* fast_malloc(std::size_t size) {
    if (size == 0) return nullptr;

    // We need space for at least FreeBlock (which contains Slab* and next pointer)
    // The user sees memory starting at `next`.
    // offsetof(FreeBlock, next) is sizeof(Slab*).
    std::size_t total_size = size + offsetof(FreeBlock, next);
    
    if (total_size > MAX_SLAB_SIZE) {
        // Large allocation (bypassing slabs)
        // Memory layout: [ alloc_size (8 bytes) ] [ Slab* (8 bytes, nullptr) ] [ User Data ... ]
        std::size_t metadata_size = sizeof(std::size_t) + offsetof(FreeBlock, next);
        std::size_t alloc_size = size + metadata_size;
        
        void* mem = GlobalHeap::GetInstance().AllocateLarge(alloc_size);
        if (!mem) return nullptr;
        
        std::size_t* size_header = static_cast<std::size_t*>(mem);
        *size_header = alloc_size;
        
        // The slab pointer must be directly before the user data
        Slab** slab_header = reinterpret_cast<Slab**>(size_header + 1);
        *slab_header = nullptr; 
        
        return slab_header + 1; // User data starts here
    }

    std::size_t class_index = SizeToClassIndex(total_size);
    FreeBlock* block = static_cast<FreeBlock*>(TLSCache::Get().AllocateBlock(class_index));
    
    if (!block) return nullptr;

    // block->slab is already correctly set when the Slab was created!
    // We just return the pointer to the user area (which overlaps with `next` when free)
    return &block->next;
}

void fast_free(void* ptr) {
    if (!ptr) return;

    // Step back to find the FreeBlock start (the Slab pointer)
    FreeBlock* block = reinterpret_cast<FreeBlock*>(
        static_cast<char*>(ptr) - offsetof(FreeBlock, next));
    
    Slab* slab = block->slab;
    if (slab == nullptr) {
        // It's a large allocation!
        // Step back one more to find the size location
        std::size_t* size_header = reinterpret_cast<std::size_t*>(
            reinterpret_cast<char*>(block) - sizeof(std::size_t));
        
        std::size_t alloc_size = *size_header;
        GlobalHeap::GetInstance().DeallocateLarge(size_header, alloc_size);
        return;
    }

    std::size_t class_index = SizeToClassIndex(slab->block_size);
    TLSCache::Get().DeallocateBlock(class_index, block);
}

void* fast_calloc(std::size_t num, std::size_t size) {
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

    // Must find out the old size to copy data
    std::size_t old_size = 0;
    
    FreeBlock* block = reinterpret_cast<FreeBlock*>(
        static_cast<char*>(ptr) - offsetof(FreeBlock, next));
        
    Slab* slab = block->slab;
    if (slab == nullptr) {
        std::size_t* size_header = reinterpret_cast<std::size_t*>(
            reinterpret_cast<char*>(block) - sizeof(std::size_t));
        // Real user capacity is total allocated size minus metadata
        old_size = *size_header - sizeof(std::size_t) - offsetof(FreeBlock, next);
    } else {
        // Slab capacity minus the header
        old_size = slab->block_size - offsetof(FreeBlock, next);
    }

    if (new_size <= old_size) {
        bool should_shrink = false;
        
        if (slab != nullptr) {
            std::size_t new_class_index = SizeToClassIndex(new_size + offsetof(FreeBlock, next));
            std::size_t old_class_index = SizeToClassIndex(slab->block_size);
            // If dropping more than 1 size class, reclaim
            if (old_class_index > new_class_index + 1) should_shrink = true;
        } else {
            // Large to small dropping by more than 1 page
            std::size_t page_size = 4096; // typical
            if (old_size - new_size >= page_size) should_shrink = true;
        }

        if (!should_shrink) {
            return ptr; // Simple reuse buffer
        }
    }

    void* new_ptr = fast_malloc(new_size);
    if (new_ptr) {
        std::memcpy(new_ptr, ptr, old_size);
        fast_free(ptr);
    }
    return new_ptr;
}

} // namespace FastAlloc

// Global overrides (optional/conditionally compiled).
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
#endif

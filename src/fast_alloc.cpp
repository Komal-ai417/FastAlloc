#include "fast_alloc.h"
#include "tls_cache.h"
#include "global_heap.h"
#include "fast_alloc_config.h"
#include "os_memory.h"

#include <cstring>
#include <new>
#include <limits>
#include <cstdlib>
namespace FastAlloc {

static_assert(sizeof(LargeAllocHeader) == 16, "Header size must align to USER_OFFSET");

void* fast_malloc_slow(std::size_t size) {
    if (size == 0) return nullptr;

    std::size_t total_size = size + USER_OFFSET;

    if (FAST_UNLIKELY(total_size > MAX_SLAB_SIZE)) {
        std::size_t alloc_size = size + sizeof(LargeAllocHeader);
        if (FAST_UNLIKELY(alloc_size < size)) return nullptr; // overflow
        
        std::size_t page_size = PAGE_SIZE;
        if (FAST_UNLIKELY(alloc_size > (std::numeric_limits<std::size_t>::max)() - page_size + 1)) return nullptr;
        alloc_size = (alloc_size + page_size - 1) & ~(page_size - 1);
        
        // AllocateLargeCached handles both TLS bins and GlobalHeap Arena fallbacks
        void* cached_mem = TLSCache::GetSlow().AllocateLargeCached(alloc_size);
        if (cached_mem) return cached_mem;
        
        return nullptr;
    }

    std::size_t class_index = (total_size - 1) >> 4;
    TLSCache& cache = TLSCache::GetSlow();
    // Cache is initialized and fast_bins is set now. Try fast path one more time to avoid duplicating block extraction logic
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

    FreeBlock* block = static_cast<FreeBlock*>(cache.AllocateBlockSlow(class_index));
    if (FAST_UNLIKELY(!block)) return nullptr;

#ifdef FAST_ALLOC_DEBUG
    block->canary = FAST_ALLOC_DEBUG_CANARY;
#endif

    return reinterpret_cast<char*>(block) + USER_OFFSET;
}

void fast_free_slow(void* ptr) {
    if (!ptr) return;

    FreeBlock* block = reinterpret_cast<FreeBlock*>(static_cast<char*>(ptr) - USER_OFFSET);

    if (FAST_UNLIKELY(block->slab == nullptr)) {
        LargeAllocHeader* header = reinterpret_cast<LargeAllocHeader*>(
            static_cast<char*>(ptr) - sizeof(LargeAllocHeader));
        std::size_t alloc_size = header->alloc_size;
        
        // Return to large allocation cache
        TLSCache::GetSlow().DeallocateLargeCached(header, alloc_size);
        return;
    }

    bool was_uninitialized = (fast_bins == nullptr);
    TLSCache& cache = TLSCache::GetSlow();
    CacheBin* bins = fast_bins;
    std::size_t class_index = block->class_index;

    // The fast path in fast_alloc.h pushes the block if `fast_bins` was already initialized and count < limit.
    // If it reached fast_free_slow, it's either because fast_bins was null (uninitialized) or count == limit.
    
    if (FAST_UNLIKELY(was_uninitialized)) {
        // fast_bins is now initialized by GetSlow() above
        bins = fast_bins;
        if (FAST_LIKELY(bins != nullptr)) {
            CacheBin& bin = bins[class_index];
            block->next = bin.head;
            bin.head = block;
            bin.count++;
            if (bin.count < bin.limit) return;
        }
    }

    // It's definitely in the bin now (or already was), and count is >= limit.
    cache.DeallocateBlockSlow(class_index);
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
        old_size = ((block->class_index + 1) * 16) - USER_OFFSET;
    }

    if (new_size <= old_size) {
        bool should_shrink = false;
        if (FAST_LIKELY(slab != nullptr)) {
            std::size_t new_class_index = (new_size + USER_OFFSET - 1) >> 4;
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
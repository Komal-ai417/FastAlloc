#include "tls_cache.h"
#include "global_heap.h"
#include "os_memory.h"
#include <cstdlib>
#include <new>

namespace FastAlloc {

FAST_THREAD_LOCAL TLSCache* fast_path_cache = nullptr;

#ifdef _WIN32
DWORD TLSCache::tls_key_ = FLS_OUT_OF_INDEXES;

static void WINAPI FlsCleanupCallback(PVOID ptr) {
    if (ptr) {
        TLSCache* cache = static_cast<TLSCache*>(ptr);
        cache->~TLSCache();
        std::size_t page_size = OSMemory::GetPageSize();
        std::size_t alloc_size = (sizeof(TLSCache) + page_size - 1) & ~(page_size - 1);
        OSMemory::FreePages(cache, alloc_size);
        fast_path_cache = nullptr;
    }
}

void TLSCache::InitTlsKey() {
    tls_key_ = FlsAlloc(FlsCleanupCallback);
}
#else
pthread_key_t TLSCache::tls_key_;

void TLSCache::TlsDestructor(void* ptr) {
    if (ptr) {
        TLSCache* cache = static_cast<TLSCache*>(ptr);
        cache->~TLSCache();
        std::size_t page_size = OSMemory::GetPageSize();
        std::size_t alloc_size = (sizeof(TLSCache) + page_size - 1) & ~(page_size - 1);
        OSMemory::FreePages(cache, alloc_size);
        fast_path_cache = nullptr;
    }
}

void TLSCache::InitTlsKey() {
    pthread_key_create(&tls_key_, TlsDestructor);
}
#endif

TLSCache::TLSCache() {
    arena_index_ = GlobalHeap::GetInstance().GetNextArena();
}

TLSCache& TLSCache::GetSlow() {
    static bool key_init = [] { InitTlsKey(); return true; }();
    (void)key_init;

#ifdef _WIN32
    void* val = FlsGetValue(tls_key_);
    if (val) {
        fast_path_cache = static_cast<TLSCache*>(val);
        return *fast_path_cache;
    }
    std::size_t page_size = OSMemory::GetPageSize();
    std::size_t alloc_size = (sizeof(TLSCache) + page_size - 1) & ~(page_size - 1);
    void* mem = OSMemory::AllocatePages(alloc_size);
    TLSCache* cache = new (mem) TLSCache();
    FlsSetValue(tls_key_, cache);
    fast_path_cache = cache;
    return *cache;
#else
    void* val = pthread_getspecific(tls_key_);
    if (val) {
        fast_path_cache = static_cast<TLSCache*>(val);
        return *fast_path_cache;
    }
    std::size_t page_size = OSMemory::GetPageSize();
    std::size_t alloc_size = (sizeof(TLSCache) + page_size - 1) & ~(page_size - 1);
    void* mem = OSMemory::AllocatePages(alloc_size);
    TLSCache* cache = new (mem) TLSCache();
    pthread_setspecific(tls_key_, cache);
    fast_path_cache = cache;
    return *cache;
#endif
}

TLSCache::~TLSCache() {
    for (std::size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
        if (bins_[i].head) {
            GlobalHeap::GetInstance().DeallocateBatch(bins_[i].head);
            bins_[i].head = nullptr;
            bins_[i].count = 0;
        }
    }
    for (std::size_t i = 0; i < NUM_LARGE_CLASSES; ++i) {
        LargeFreeEntry* entry = large_free_bins_[i];
        while (entry) {
            LargeFreeEntry* next = entry->next;
            GlobalHeap::GetInstance().DeallocateLarge(entry, entry->alloc_size);
            entry = next;
        }
        large_free_bins_[i] = nullptr;
    }
}

void* TLSCache::AllocateBlockSlow(std::size_t class_index) {
    std::size_t max_cache = CACHE_LIMITS[class_index];
    std::size_t target_count = max_cache / 2;
    if (target_count == 0) target_count = 1; 
    
    std::size_t actual_count = 0;
    FreeBlock* batch_head = GlobalHeap::GetInstance().AllocateBatch(class_index, target_count, actual_count, arena_index_);
    
    if (!batch_head) return nullptr;

#if defined(__GNUC__) || defined(__clang__)
    FreeBlock* curr = batch_head;
    for (int i = 0; i < 4 && curr; ++i) {
        __builtin_prefetch(curr, 0, 1);
        curr = curr->next;
    }
#endif

    FreeBlock* block = batch_head;
    bins_[class_index].head = block->next;
    bins_[class_index].count = static_cast<uint32_t>(actual_count - 1);

    return block;
}

void TLSCache::DeallocateBlockSlow(std::size_t class_index) {
    std::size_t max_cache = CACHE_LIMITS[class_index];
    std::size_t batch_size = max_cache / 2;
    if (batch_size == 0) batch_size = 1;

    CacheBin& bin = bins_[class_index];
    FreeBlock* head = bin.head;
    FreeBlock* curr = head;
    for (std::size_t i = 1; i < batch_size; ++i) {
        curr = curr->next;
    }
    
    bin.head = curr->next;
    curr->next = nullptr; 
    bin.count -= static_cast<uint32_t>(batch_size);

    GlobalHeap::GetInstance().DeallocateBatch(head);
}

void* TLSCache::AllocateLargeCached(std::size_t size) {
    std::size_t cls = LargeSizeToClass(size);
    LargeFreeEntry** pp = &large_free_bins_[cls];
    while (*pp) {
        if ((*pp)->alloc_size >= size) {
            LargeFreeEntry* entry = *pp;
            *pp = entry->next;
            large_counts_[cls]--;
            
            LargeAllocHeader* header = reinterpret_cast<LargeAllocHeader*>(entry);
            header->slab = nullptr;
            header->alloc_size = entry->alloc_size;
            return reinterpret_cast<char*>(header) + sizeof(LargeAllocHeader);
        }
        pp = &(*pp)->next;
    }

    void* mem = GlobalHeap::GetInstance().AllocateLarge(size);
    if (mem) {
        LargeAllocHeader* header = static_cast<LargeAllocHeader*>(mem);
        header->alloc_size = size;
        header->slab = nullptr; 
        return reinterpret_cast<char*>(mem) + sizeof(LargeAllocHeader); 
    }
    return nullptr;
}

void TLSCache::DeallocateLargeCached(void* ptr, std::size_t alloc_size) {
    std::size_t cls = LargeSizeToClass(alloc_size);
    if (large_counts_[cls] < GetMaxLargeCacheSize(alloc_size)) {
        LargeFreeEntry* entry = reinterpret_cast<LargeFreeEntry*>(ptr);
        entry->alloc_size = alloc_size;
        entry->next = large_free_bins_[cls];
        large_free_bins_[cls] = entry;
        large_counts_[cls]++;
    } else {
        GlobalHeap::GetInstance().DeallocateLarge(ptr, alloc_size);
    }
}

} // namespace FastAlloc
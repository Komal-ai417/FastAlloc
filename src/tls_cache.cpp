#include "tls_cache.h"
#include "global_heap.h"
#include "os_memory.h"
#include <cstdlib>
#include <new>

namespace FastAlloc {

thread_local TLSCache* fast_path_cache = nullptr;

#ifdef _WIN32
DWORD TLSCache::tls_key_ = FLS_OUT_OF_INDEXES;

static void WINAPI FlsCleanupCallback(PVOID ptr) {
    if (ptr) {
        TLSCache* cache = static_cast<TLSCache*>(ptr);
        cache->~TLSCache();
        std::free(cache);
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
        std::free(cache);
    }
}

void TLSCache::InitTlsKey() {
    pthread_key_create(&tls_key_, TlsDestructor);
}
#endif

TLSCache& TLSCache::GetFast() {
    if (fast_path_cache) return *fast_path_cache;
    return GetSlow();
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
    void* mem = std::malloc(sizeof(TLSCache));
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
    void* mem = std::malloc(sizeof(TLSCache));
    TLSCache* cache = new (mem) TLSCache();
    pthread_setspecific(tls_key_, cache);
    fast_path_cache = cache;
    return *cache;
#endif
}

TLSCache::~TLSCache() {
    for (std::size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
        if (fast_bins_[i]) {
            GlobalHeap::GetInstance().DeferredDeallocateBatch(fast_bins_[i]);
            fast_bins_[i] = nullptr;
        }
    }
    // OPT-5: Free large cache
    for (std::size_t i = 0; i < NUM_LARGE_CLASSES; ++i) {
        LargeFreeEntry* entry = large_free_bins_[i];
        while (entry) {
            LargeFreeEntry* next = entry->next;
            OSMemory::FreePages(entry, entry->alloc_size);
            entry = next;
        }
        large_free_bins_[i] = nullptr;
    }
}

void* TLSCache::AllocateBlock(std::size_t class_index) {
    FreeBlock* block = fast_bins_[class_index];
    if (block) {
        fast_bins_[class_index] = block->next;
        counts_[class_index]--;
        return block;
    }

    std::size_t max_cache = GetMaxCacheSize(class_index);
    std::size_t target_count = max_cache / 2;
    if (target_count == 0) target_count = 1; 
    
    std::size_t actual_count = 0;
    FreeBlock* batch_head = GlobalHeap::GetInstance().AllocateBatch(class_index, target_count, actual_count);
    
    if (!batch_head) return nullptr;

    // OPT-8: Prefetch
    if (batch_head->next) {
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(batch_head->next, 0, 1);
#endif
    }

    block = batch_head;
    fast_bins_[class_index] = block->next;
    counts_[class_index] = actual_count - 1;

    return block;
}

void TLSCache::DeallocateBlock(std::size_t class_index, FreeBlock* block) {
    block->next = fast_bins_[class_index];
    fast_bins_[class_index] = block;
    counts_[class_index]++;

    std::size_t max_cache = GetMaxCacheSize(class_index);
    if (counts_[class_index] >= max_cache) {
        std::size_t batch_size = max_cache / 2;
        if (batch_size == 0) batch_size = 1;

        FreeBlock* head = fast_bins_[class_index];
        FreeBlock* curr = head;
        for (std::size_t i = 1; i < batch_size; ++i) {
            curr = curr->next;
        }
        
        fast_bins_[class_index] = curr->next;
        curr->next = nullptr; 
        counts_[class_index] -= batch_size;

        GlobalHeap::GetInstance().DeallocateBatch(head);
    }
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
    return nullptr;
}

void TLSCache::DeallocateLargeCached(void* ptr, std::size_t alloc_size, std::size_t user_size) {
    (void)user_size;
    std::size_t cls = LargeSizeToClass(alloc_size);
    if (large_counts_[cls] < MAX_LARGE_CACHE) {
        LargeFreeEntry* entry = reinterpret_cast<LargeFreeEntry*>(ptr);
        entry->alloc_size = alloc_size;
        entry->next = large_free_bins_[cls];
        large_free_bins_[cls] = entry;
        large_counts_[cls]++;
    } else {
        OSMemory::FreePages(ptr, alloc_size);
    }
}

} // namespace FastAlloc

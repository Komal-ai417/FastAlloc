#include "tls_cache.h"
#include "global_heap.h"
#include <cstdlib>

namespace FastAlloc {

// ---- Platform-specific TLS implementation ----
// We use raw OS TLS instead of C++ thread_local to avoid the Windows/MinGW
// loader-lock deadlock. C++ thread_local on MinGW runs destructors under
// LdrShutdownThread (holding the loader lock), which prevents other threads
// from initializing their own thread_local variables — causing deadlock
// when combined with our GlobalHeap mutex_.

#ifdef _WIN32

// Use FlsAlloc (Fiber Local Storage) on Windows — it supports a cleanup
// callback that runs on thread exit WITHOUT the loader lock issues.
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

TLSCache& TLSCache::Get() {
    // One-time key initialization (thread-safe via static local)
    static bool key_init = [] { InitTlsKey(); return true; }();
    (void)key_init;

#ifdef _WIN32
    void* val = FlsGetValue(tls_key_);
    if (val) {
        return *static_cast<TLSCache*>(val);
    }
    // First access on this thread — allocate via system malloc (not our allocator!)
    void* mem = std::malloc(sizeof(TLSCache));
    TLSCache* cache = new (mem) TLSCache();
    FlsSetValue(tls_key_, cache);
    return *cache;
#else
    void* val = pthread_getspecific(tls_key_);
    if (val) {
        return *static_cast<TLSCache*>(val);
    }
    void* mem = std::malloc(sizeof(TLSCache));
    TLSCache* cache = new (mem) TLSCache();
    pthread_setspecific(tls_key_, cache);
    return *cache;
#endif
}

TLSCache::~TLSCache() {
    // Return all cached blocks to the global heap via the lock-free deferred queue.
    // This runs from the FLS/pthread destructor callback, which on Windows does NOT
    // hold the loader lock (unlike C++ thread_local destructors on MinGW).
    for (std::size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
        if (fast_bins_[i]) {
            GlobalHeap::GetInstance().DeferredDeallocateBatch(fast_bins_[i]);
            fast_bins_[i] = nullptr;
        }
    }
}

void* TLSCache::AllocateBlock(std::size_t class_index) {
    FreeBlock* block = fast_bins_[class_index];
    if (block) {
        fast_bins_[class_index] = block->next;
        counts_[class_index]--;
        return block;
    }

    // Cache miss: dynamically calculate batch fetch size
    std::size_t max_cache = GetMaxCacheSize(class_index);
    std::size_t target_count = max_cache / 2;
    if (target_count == 0) target_count = 1; // Failsafe
    
    std::size_t actual_count = 0;
    FreeBlock* batch_head = GlobalHeap::GetInstance().AllocateBatch(class_index, target_count, actual_count);
    
    if (!batch_head) return nullptr;

    block = batch_head;
    fast_bins_[class_index] = block->next;
    counts_[class_index] = actual_count - 1;

    return block;
}

void TLSCache::DeallocateBlock(std::size_t class_index, FreeBlock* block) {
    block->next = fast_bins_[class_index];
    fast_bins_[class_index] = block;
    counts_[class_index]++;

    // Dynamic cache overflow check
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

} // namespace FastAlloc

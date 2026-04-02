#include "tls_cache.h"
#include "global_heap.h"

namespace FastAlloc {

TLSCache& TLSCache::Get() {
    thread_local TLSCache instance;
    return instance;
}

TLSCache::~TLSCache() {
    // Return all cached blocks to global heap on thread exit
    // Note: GlobalHeap outlives thread_local instances.
    for (std::size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
        if (fast_bins_[i]) {
            GlobalHeap::GetInstance().DeallocateBatch(fast_bins_[i]);
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

    // Cache miss, fetch a batch from global heap
    std::size_t target_count = MAX_CACHE_SIZE / 2;
    std::size_t actual_count = 0;
    
    FreeBlock* batch_head = GlobalHeap::GetInstance().AllocateBatch(class_index, target_count, actual_count);
    
    if (!batch_head) return nullptr;

    // Pop the first block to return to the user
    block = batch_head;
    
    // The rest goes into the thread-local cache
    fast_bins_[class_index] = block->next;
    counts_[class_index] = actual_count - 1;

    return block;
}

void TLSCache::DeallocateBlock(std::size_t class_index, FreeBlock* block) {
    // Push to thread-local free list
    block->next = fast_bins_[class_index];
    fast_bins_[class_index] = block;
    counts_[class_index]++;

    if (counts_[class_index] >= MAX_CACHE_SIZE) {
        // Cache full, batch evict half to global heap
        std::size_t batch_size = MAX_CACHE_SIZE / 2;
        FreeBlock* head = fast_bins_[class_index];
        FreeBlock* curr = head;
        
        for (std::size_t i = 1; i < batch_size; ++i) {
            curr = curr->next;
        }
        
        fast_bins_[class_index] = curr->next;
        curr->next = nullptr; // Terminate early
        counts_[class_index] -= batch_size;

        GlobalHeap::GetInstance().DeallocateBatch(head);
    }
}

} // namespace FastAlloc

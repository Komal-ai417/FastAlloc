#include "global_heap.h"
#include "os_memory.h"
#include <algorithm>
#include <cstring>

namespace FastAlloc {

GlobalHeap& GlobalHeap::GetInstance() {
    static GlobalHeap* instance = new GlobalHeap();
    return *instance;
}

Slab* GlobalHeap::AllocateNewSlab(std::size_t class_index) {
    std::size_t block_size = ClassIndexToSize(class_index);
    
    // MEMORY OPTIMIZATION: Dynamic OS Chunk Sizing
    // We want to fetch ~256 blocks at a time to keep it fast, 
    // but strictly clamp the memory between 64KB (min) and 2MB (max)
    std::size_t target_size = block_size * 256;
    if (target_size < 65536) target_size = 65536; // Tiny blocks stay lean (64KB)
    if (target_size > 2 * 1024 * 1024) target_size = 2 * 1024 * 1024; // Cap at 2MB
    
    std::size_t page_size = OSMemory::GetPageSize();
    std::size_t memory_size = (target_size + page_size - 1) & ~(page_size - 1);
    
    void* memory = OSMemory::AllocatePages(memory_size);
    if (!memory) return nullptr;

    return Slab::Create(memory, memory_size, block_size);
}

// ----- Lock-free deferred return queue -----
// TLS destructors on Windows/MinGW run under the loader lock.
// Calling std::mutex::lock() from there risks deadlock with threads that
// hold mutex_ and make OS calls that touch the loader lock.
// Solution: TLS destructor pushes blocks into a lock-free MPSC queue via
// DeferredDeallocateBatch(). Normal allocation paths drain it under mutex_.

void GlobalHeap::DeferredDeallocateBatch(FreeBlock* head) {
    if (!head) return;
    
    // Find the tail of the incoming list
    FreeBlock* tail = head;
    while (tail->next) {
        tail = tail->next;
    }
    
    // Atomic push onto the MPSC pending_returns_ stack
    FreeBlock* old_head = pending_returns_.load(std::memory_order_relaxed);
    do {
        tail->next = old_head;
    } while (!pending_returns_.compare_exchange_weak(old_head, head,
                std::memory_order_release, std::memory_order_relaxed));
}

void GlobalHeap::DrainPendingReturns() {
    // Must be called under mutex_!
    // Atomically steal the entire pending list
    FreeBlock* pending = pending_returns_.exchange(nullptr, std::memory_order_acquire);
    
    while (pending) {
        FreeBlock* next = pending->next;
        Slab* slab = pending->slab;
        
        bool was_full = slab->IsFull();
        slab->Deallocate(pending);
        
        if (was_full) {
            std::size_t class_index = SizeToClassIndex(slab->block_size);
            Slab** p = &full_slabs_[class_index];
            while (*p != nullptr && *p != slab) {
                p = &(*p)->next;
            }
            if (*p == slab) *p = slab->next;
            
            slab->next = partial_slabs_[class_index];
            partial_slabs_[class_index] = slab;
        }
        
        // Note: We intentionally do NOT free empty slabs here.
        // DrainPendingReturns runs under mutex_, and freeing pages under
        // the mutex could cause the same contention. Empty slabs are
        // reclaimed lazily the next time DeallocateBatch runs.
        
        pending = next;
    }
}

void* GlobalHeap::AllocateBlock(std::size_t class_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Drain any blocks deferred by dying threads
    DrainPendingReturns();

    Slab* slab = partial_slabs_[class_index];
    if (!slab) {
        slab = AllocateNewSlab(class_index);
        if (!slab) return nullptr;
        
        slab->next = partial_slabs_[class_index];
        partial_slabs_[class_index] = slab;
    }

    void* ptr = slab->Allocate();

    if (slab->IsFull()) {
        partial_slabs_[class_index] = slab->next;
        slab->next = full_slabs_[class_index];
        full_slabs_[class_index] = slab;
    }

    return ptr;
}

FreeBlock* GlobalHeap::AllocateBatch(std::size_t class_index, std::size_t target_count, std::size_t& actual_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Drain any blocks deferred by dying threads
    DrainPendingReturns();
    
    FreeBlock* head = nullptr;
    FreeBlock* tail = nullptr;
    actual_count = 0;

    while (actual_count < target_count) {
        Slab* slab = partial_slabs_[class_index];
        if (!slab) {
            slab = AllocateNewSlab(class_index);
            if (!slab) break; 
            slab->next = partial_slabs_[class_index];
            partial_slabs_[class_index] = slab;
        }

        while (actual_count < target_count && !slab->IsFull()) {
            FreeBlock* block = static_cast<FreeBlock*>(slab->Allocate());
            if (!head) {
                head = block;
                tail = block;
            } else {
                tail->next = block;
                tail = block;
            }
            actual_count++;
        }

        if (slab->IsFull()) {
            partial_slabs_[class_index] = slab->next;
            slab->next = full_slabs_[class_index];
            full_slabs_[class_index] = slab;
        }
    }

    if (tail) tail->next = nullptr;
    return head;
}

void GlobalHeap::DeallocateBlock(Slab* slab, void* ptr) {
    (void)slab; 
    FreeBlock* block = static_cast<FreeBlock*>(ptr);
    block->next = nullptr;
    DeallocateBatch(block);
}

void GlobalHeap::DeallocateBatch(FreeBlock* head) {
    if (!head) return;

    // Deferred-free list: collect empty slabs here, release pages AFTER dropping mutex.
    // Stack buffer avoids heap allocation in the common case (<=16 slabs freed per batch).
    struct SlabToFree { void* ptr; std::size_t size; };
    constexpr std::size_t kStackBuf = 16;
    SlabToFree stack_buf[kStackBuf];
    std::size_t deferred_count = 0;
    SlabToFree* deferred = stack_buf;
    std::size_t deferred_cap = kStackBuf;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Also drain pending returns from dead threads while we hold the lock
        DrainPendingReturns();

        FreeBlock* curr = head;
        while (curr) {
            FreeBlock* next = curr->next;
            Slab* slab = curr->slab;

            bool was_full = slab->IsFull();
            slab->Deallocate(curr);

            if (was_full) {
                std::size_t class_index = SizeToClassIndex(slab->block_size);
                Slab** p = &full_slabs_[class_index];
                while (*p != nullptr && *p != slab) {
                    p = &(*p)->next;
                }
                if (*p == slab) *p = slab->next;

                slab->next = partial_slabs_[class_index];
                partial_slabs_[class_index] = slab;
            }

            if (slab->IsEmpty()) {
                std::size_t class_index = SizeToClassIndex(slab->block_size);
                Slab** p = &partial_slabs_[class_index];
                while (*p != nullptr && *p != slab) {
                    p = &(*p)->next;
                }
                if (*p == slab) *p = slab->next;

                // Defer the OS free — do NOT call VirtualFree while holding mutex_
                if (deferred_count == deferred_cap) {
                    std::size_t new_cap = deferred_cap * 2;
                    SlabToFree* heap_buf = new SlabToFree[new_cap];
                    std::memcpy(heap_buf, deferred, deferred_count * sizeof(SlabToFree));
                    if (deferred != stack_buf) delete[] deferred;
                    deferred = heap_buf;
                    deferred_cap = new_cap;
                }
                deferred[deferred_count++] = {slab, slab->memory_size};
            }

            curr = next;
        }
    } // mutex_ released here

    // Now safe to call into the OS kernel without holding any locks
    for (std::size_t i = 0; i < deferred_count; ++i) {
        OSMemory::FreePages(deferred[i].ptr, deferred[i].size);
    }
    if (deferred != stack_buf) delete[] deferred;
}

void* GlobalHeap::AllocateLarge(std::size_t size) {
    std::size_t page_size = OSMemory::GetPageSize();
    std::size_t alloc_size = (size + page_size - 1) & ~(page_size - 1);
    return OSMemory::AllocatePages(alloc_size);
}

void GlobalHeap::DeallocateLarge(void* ptr, std::size_t size) {
    std::size_t page_size = OSMemory::GetPageSize();
    std::size_t alloc_size = (size + page_size - 1) & ~(page_size - 1);
    OSMemory::FreePages(ptr, alloc_size);
}

} // namespace FastAlloc

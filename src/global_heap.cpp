#include "global_heap.h"
#include "os_memory.h"
#include <algorithm>
#include <cstring>

namespace FastAlloc {

GlobalHeap& GlobalHeap::GetInstance() {
    static GlobalHeap instance;
    return instance;
}

static void PushSlab(Slab*& head, Slab* slab) {
    slab->next = head;
    slab->prev = nullptr;
    if (head) head->prev = slab;
    head = slab;
}

static void RemoveSlab(Slab*& head, Slab* slab) {
    if (slab->prev) slab->prev->next = slab->next;
    else head = slab->next;
    if (slab->next) slab->next->prev = slab->prev;
    slab->next = nullptr;
    slab->prev = nullptr;
}

Slab* GlobalHeap::AllocateNewSlab(std::size_t class_index) {
    std::size_t block_size = ClassIndexToSize(class_index);
    std::size_t target_size = block_size * 256;
    if (target_size < 65536) target_size = 65536; 
    if (target_size > 2 * 1024 * 1024) target_size = 2 * 1024 * 1024; 
    
    std::size_t page_size = OSMemory::GetPageSize();
    std::size_t memory_size = (target_size + page_size - 1) & ~(page_size - 1);
    
    void* memory = OSMemory::AllocatePages(memory_size);
    if (!memory) return nullptr;

    return Slab::Create(memory, memory_size, block_size);
}

void GlobalHeap::DeferredDeallocateBatch(FreeBlock* head) {
    if (!head) return;
    std::size_t class_index = SizeToClassIndex(head->slab->block_size);
    std::size_t stripe = ClassToStripe(class_index);

    FreeBlock* tail = head;
    while (tail->next) tail = tail->next;
    
    FreeBlock* old_head = pending_returns_[stripe].head.load(std::memory_order_relaxed);
    do {
        tail->next = old_head;
    } while (!pending_returns_[stripe].head.compare_exchange_weak(old_head, head,
                std::memory_order_release, std::memory_order_relaxed));
}

void GlobalHeap::DrainPendingReturns(std::size_t stripe_index, SlabToFree* deferred_slabs, std::size_t& deferred_count) {
    FreeBlock* pending = pending_returns_[stripe_index].head.exchange(nullptr, std::memory_order_acquire);
    if (!pending) return;

    while (pending) {
        FreeBlock* next = pending->next;
        Slab* slab = pending->slab;
        std::size_t class_index = SizeToClassIndex(slab->block_size);
        
        bool was_full = slab->IsFull();
        slab->Deallocate(pending);
        
        if (was_full) {
            RemoveSlab(full_slabs_[class_index], slab);
            PushSlab(partial_slabs_[class_index], slab);
        }

        if (slab->IsEmpty() && deferred_count < MAX_DEFERRED_FREE) {
            RemoveSlab(partial_slabs_[class_index], slab);
            deferred_slabs[deferred_count++] = {slab, slab->memory_size};
        }
        pending = next;
    }
}

void* GlobalHeap::AllocateBlock(std::size_t class_index) {
    std::size_t stripe = ClassToStripe(class_index);
    SlabToFree stack_buf[MAX_DEFERRED_FREE];
    std::size_t deferred_count = 0;
    void* ptr = nullptr;

    {
        ScopedSpinLock lock(class_locks_[stripe]);
        DrainPendingReturns(stripe, stack_buf, deferred_count);

        Slab* slab = partial_slabs_[class_index];
        if (!slab) {
            slab = AllocateNewSlab(class_index);
            if (slab) {
                PushSlab(partial_slabs_[class_index], slab);
            }
        }

        if (slab) {
            ptr = slab->Allocate();
            if (slab->IsFull()) {
                RemoveSlab(partial_slabs_[class_index], slab);
                PushSlab(full_slabs_[class_index], slab);
            }
        }
    }

    for (std::size_t i = 0; i < deferred_count; ++i) {
        OSMemory::FreePages(stack_buf[i].ptr, stack_buf[i].size);
    }

    return ptr;
}

FreeBlock* GlobalHeap::ExtractBlocksFromSlab(Slab* slab, std::size_t class_index, std::size_t target_count, std::size_t& actual_count) {
    FreeBlock* head = nullptr;
    FreeBlock* tail = nullptr;
    
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
        RemoveSlab(partial_slabs_[class_index], slab);
        PushSlab(full_slabs_[class_index], slab);
    }

    if (tail) tail->next = nullptr;
    return head;
}

FreeBlock* GlobalHeap::AllocateBatch(std::size_t class_index, std::size_t target_count, std::size_t& actual_count) {
    actual_count = 0;
    std::size_t stripe = ClassToStripe(class_index);
    SlabToFree stack_buf[MAX_DEFERRED_FREE];
    std::size_t deferred_count = 0;
    FreeBlock* result = nullptr;

    {
        ScopedSpinLock lock(class_locks_[stripe]);
        DrainPendingReturns(stripe, stack_buf, deferred_count);
        
        Slab* slab = partial_slabs_[class_index];
        if (slab) {
            result = ExtractBlocksFromSlab(slab, class_index, target_count, actual_count);
        }
    } 
    
    if (!result) {
        Slab* new_slab = AllocateNewSlab(class_index);
        if (new_slab) {
            ScopedSpinLock lock(class_locks_[stripe]);
            PushSlab(partial_slabs_[class_index], new_slab);
            result = ExtractBlocksFromSlab(new_slab, class_index, target_count, actual_count);
        }
    }

    for (std::size_t i = 0; i < deferred_count; ++i) {
        OSMemory::FreePages(stack_buf[i].ptr, stack_buf[i].size);
    }

    return result;
}

void GlobalHeap::DeallocateBlock(Slab* slab, void* ptr) {
    (void)slab;
    FreeBlock* block = static_cast<FreeBlock*>(ptr);
    block->next = nullptr;
    DeallocateBatch(block);
}

void GlobalHeap::DeallocateBatch(FreeBlock* head) {
    if (!head) return;
    std::size_t class_index = SizeToClassIndex(head->slab->block_size);
    std::size_t stripe = ClassToStripe(class_index);

    if (class_locks_[stripe].test_and_set(std::memory_order_acquire)) {
        // Lock is held by someone else, avoid spinning and defer!
        DeferredDeallocateBatch(head);
        return;
    }

    SlabToFree stack_buf[MAX_DEFERRED_FREE];
    std::size_t deferred_count = 0;

    auto process_list = [&](FreeBlock* list) {
        while (list) {
            FreeBlock* next = list->next;
            Slab* slab = list->slab;
            std::size_t c_idx = SizeToClassIndex(slab->block_size);
            bool was_full = slab->IsFull();
            slab->Deallocate(list);

            if (was_full) {
                RemoveSlab(full_slabs_[c_idx], slab);
                PushSlab(partial_slabs_[c_idx], slab);
            }

            if (slab->IsEmpty() && deferred_count < MAX_DEFERRED_FREE) {
                RemoveSlab(partial_slabs_[c_idx], slab);
                stack_buf[deferred_count++] = {slab, slab->memory_size};
            }
            list = next;
        }
    };

    process_list(head);

    // Drain pending for this stripe
    FreeBlock* pending = pending_returns_[stripe].head.exchange(nullptr, std::memory_order_acquire);
    process_list(pending);

    class_locks_[stripe].clear(std::memory_order_release);

    for (std::size_t i = 0; i < deferred_count; ++i) {
        OSMemory::FreePages(stack_buf[i].ptr, stack_buf[i].size);
    }
}

void* GlobalHeap::AllocateLarge(std::size_t size) {
    std::size_t page_size = OSMemory::GetPageSize();
    std::size_t alloc_size = (size + page_size - 1) & ~(page_size - 1);
    return OSMemory::AllocatePages(alloc_size);
}

void GlobalHeap::DeallocateLarge(void* ptr, std::size_t size) {
    OSMemory::FreePages(ptr, size);
}

} // namespace FastAlloc

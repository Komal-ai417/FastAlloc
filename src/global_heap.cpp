#include "global_heap.h"
#include "os_memory.h"
#include <algorithm>

namespace FastAlloc {

GlobalHeap& GlobalHeap::GetInstance() {
    static GlobalHeap* instance = new GlobalHeap();
    return *instance;
}

Slab* GlobalHeap::AllocateNewSlab(std::size_t class_index) {
    std::size_t block_size = ClassIndexToSize(class_index);
    // Allocate a chunk for the slab. 64KB is a good balance to minimize OS syscalls.
    std::size_t num_pages = 65536 / OSMemory::GetPageSize();
    if (num_pages == 0) num_pages = 1;

    std::size_t memory_size = num_pages * OSMemory::GetPageSize();
    void* memory = OSMemory::AllocatePages(memory_size);
    if (!memory) return nullptr;

    return Slab::Create(memory, memory_size, block_size);
}

void* GlobalHeap::AllocateBlock(std::size_t class_index) {
    std::lock_guard<std::mutex> lock(mutex_);

    Slab* slab = partial_slabs_[class_index];
    if (!slab) {
        // No partially filled slabs, map a new one from the OS
        slab = AllocateNewSlab(class_index);
        if (!slab) return nullptr;
        
        slab->next = partial_slabs_[class_index];
        partial_slabs_[class_index] = slab;
    }

    void* ptr = slab->Allocate();

    if (slab->IsFull()) {
        // Remove from partial_slabs_ and move into full_slabs_
        partial_slabs_[class_index] = slab->next;
        slab->next = full_slabs_[class_index];
        full_slabs_[class_index] = slab;
    }

    return ptr;
}

FreeBlock* GlobalHeap::AllocateBatch(std::size_t class_index, std::size_t target_count, std::size_t& actual_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    FreeBlock* head = nullptr;
    FreeBlock* tail = nullptr;
    actual_count = 0;

    while (actual_count < target_count) {
        Slab* slab = partial_slabs_[class_index];
        if (!slab) {
            slab = AllocateNewSlab(class_index);
            if (!slab) break; // Out of memory
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

    if (tail) {
        tail->next = nullptr;
    }
    return head;
}

void GlobalHeap::DeallocateBlock(Slab* slab, void* ptr) {
    (void)slab; // Unused since batch eviction natively retrieves it from the block layout
    FreeBlock* block = static_cast<FreeBlock*>(ptr);
    block->next = nullptr;
    DeallocateBatch(block);
}

void GlobalHeap::DeallocateBatch(FreeBlock* head) {
    if (!head) return;
    std::lock_guard<std::mutex> lock(mutex_);
    
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
            if (*p == slab) {
                *p = slab->next;
            }

            slab->next = partial_slabs_[class_index];
            partial_slabs_[class_index] = slab;
        }

        // Avoid memory bloat by returning empty slabs to the OS
        if (slab->IsEmpty()) {
            std::size_t class_index = SizeToClassIndex(slab->block_size);
            Slab** p = &partial_slabs_[class_index];
            while (*p != nullptr && *p != slab) {
                p = &(*p)->next;
            }
            if (*p == slab) {
                *p = slab->next;
            }
            OSMemory::FreePages(slab, slab->memory_size);
        }

        curr = next;
    }
}

void* GlobalHeap::AllocateLarge(std::size_t size) {
    // Round size up to page size
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

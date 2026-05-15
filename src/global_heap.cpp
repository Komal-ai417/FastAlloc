#include "global_heap.h"
#include "os_memory.h"
#include <algorithm>
#include <cstring>
#include <thread>

namespace FastAlloc {

namespace {

constexpr std::size_t MAX_CACHED_PAGES = 512; // up to 2MB spans
struct PageNode {
    PageNode* next;
};
struct PageBin {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    PageNode* head = nullptr;
    std::size_t count = 0;
};
std::array<PageBin, MAX_CACHED_PAGES + 1> page_bins_{};

void* PageHeapAllocate(std::size_t size) {
    std::size_t pages = size / OSMemory::GetPageSize();
    if (pages <= MAX_CACHED_PAGES && pages > 0) {
        auto& bin = page_bins_[pages];
        if (bin.lock.test_and_set(std::memory_order_acquire)) {
            int spins = 0;
            while (bin.lock.test_and_set(std::memory_order_acquire)) {
                if (++spins > 64) {
                    std::this_thread::yield();
                    spins = 0;
                } else {
#if defined(_MSC_VER)
                    _mm_pause();
#elif defined(__i386__) || defined(__x86_64__)
                    __builtin_ia32_pause();
#endif
                }
            }
        }
        if (bin.head) {
            PageNode* node = bin.head;
            bin.head = node->next;
            bin.count--;
            bin.lock.clear(std::memory_order_release);
            return node;
        }
        bin.lock.clear(std::memory_order_release);
    }
    return OSMemory::AllocatePages(size);
}

void PageHeapFree(void* ptr, std::size_t size) {
    std::size_t pages = size / OSMemory::GetPageSize();
    if (pages <= MAX_CACHED_PAGES && pages > 0) {
        auto& bin = page_bins_[pages];
        // Cache up to 16MB per bin size, minimum 8 blocks
        std::size_t max_count = (16 * 1024 * 1024) / size;
        if (max_count < 8) max_count = 8;
        
        if (bin.lock.test_and_set(std::memory_order_acquire)) {
            int spins = 0;
            while (bin.lock.test_and_set(std::memory_order_acquire)) {
                if (++spins > 64) {
                    std::this_thread::yield();
                    spins = 0;
                } else {
#if defined(_MSC_VER)
                    _mm_pause();
#elif defined(__i386__) || defined(__x86_64__)
                    __builtin_ia32_pause();
#endif
                }
            }
        }
        if (bin.count < max_count) {
            PageNode* node = static_cast<PageNode*>(ptr);
            node->next = bin.head;
            bin.head = node;
            bin.count++;
            bin.lock.clear(std::memory_order_release);
            return;
        }
        bin.lock.clear(std::memory_order_release);
    }
    OSMemory::FreePages(ptr, size);
}

} // namespace

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

Slab* GlobalHeap::AllocateNewSlab(std::size_t class_index, uint32_t arena_index) {
    std::size_t block_size = ClassIndexToSize(class_index);
    std::size_t target_blocks = 256;
    if (block_size >= 1024) target_blocks = 128;
    if (block_size >= 4096) target_blocks = 32;
    
    std::size_t target_size = block_size * target_blocks;
    if (target_size < 65536) target_size = 65536; 
    if (target_size > 2 * 1024 * 1024) target_size = 2 * 1024 * 1024; 
    
    std::size_t page_size = OSMemory::GetPageSize();
    std::size_t memory_size = (target_size + page_size - 1) & ~(page_size - 1);
    
    void* memory = PageHeapAllocate(memory_size);
    if (!memory) return nullptr;

    return Slab::Create(memory, memory_size, block_size, arena_index);
}

void GlobalHeap::DeferredDeallocateBatch(FreeBlock* head) {
    if (!head) return;
    std::size_t class_index = SizeToClassIndex(head->slab->block_size);
    uint32_t arena_index = head->slab->arena_index;

    FreeBlock* tail = head;
    while (tail->next) tail = tail->next;
    
    FreeBlock* old_head = arenas_[arena_index].pending_returns_[class_index].head.load(std::memory_order_relaxed);
    do {
        tail->next = old_head;
    } while (!arenas_[arena_index].pending_returns_[class_index].head.compare_exchange_weak(old_head, head,
                std::memory_order_release, std::memory_order_relaxed));
}

void GlobalHeap::DrainPendingReturns(uint32_t arena_index, std::size_t class_index) {
    Arena& arena = arenas_[arena_index];
    if (arena.pending_returns_[class_index].head.load(std::memory_order_relaxed) == nullptr) return;
    FreeBlock* pending = arena.pending_returns_[class_index].head.exchange(nullptr, std::memory_order_acquire);
    if (!pending) return;

    while (pending) {
        FreeBlock* next = pending->next;
        Slab* slab = pending->slab;
        
        bool was_full = slab->IsFull();
        slab->Deallocate(pending);
        
        if (was_full) {
            RemoveSlab(arena.full_slabs_[class_index], slab);
            PushSlab(arena.partial_slabs_[class_index], slab);
        }

        if (slab->IsEmpty()) {
            RemoveSlab(arena.partial_slabs_[class_index], slab);
            PageHeapFree(slab, slab->memory_size);
        }
        pending = next;
    }
}

void* GlobalHeap::AllocateBlock(std::size_t class_index, uint32_t arena_index) {
    Arena& arena = arenas_[arena_index];
    void* ptr = nullptr;

    {
        ScopedSpinLock lock(arena.class_locks_[class_index]);
        DrainPendingReturns(arena_index, class_index);

        Slab* slab = arena.partial_slabs_[class_index];
        if (slab) {
            ptr = slab->Allocate();
            if (slab->IsFull()) {
                RemoveSlab(arena.partial_slabs_[class_index], slab);
                PushSlab(arena.full_slabs_[class_index], slab);
            }
        } else {
            Slab* new_slab = AllocateNewSlab(class_index, arena_index);
            if (new_slab) {
                PushSlab(arena.partial_slabs_[class_index], new_slab);
                ptr = new_slab->Allocate();
                if (new_slab->IsFull()) {
                    RemoveSlab(arena.partial_slabs_[class_index], new_slab);
                    PushSlab(arena.full_slabs_[class_index], new_slab);
                }
            }
        }
    }

    return ptr;
}

FreeBlock* GlobalHeap::ExtractBlocksFromSlab(Slab* slab, std::size_t class_index, std::size_t target_count, std::size_t& actual_count, uint32_t arena_index) {
    Arena& arena = arenas_[arena_index];
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
        RemoveSlab(arena.partial_slabs_[class_index], slab);
        PushSlab(arena.full_slabs_[class_index], slab);
    }

    if (tail) tail->next = nullptr;
    return head;
}

FreeBlock* GlobalHeap::AllocateBatch(std::size_t class_index, std::size_t target_count, std::size_t& actual_count, uint32_t arena_index) {
    actual_count = 0;
    Arena& arena = arenas_[arena_index];
    FreeBlock* result = nullptr;

    {
        ScopedSpinLock lock(arena.class_locks_[class_index]);
        DrainPendingReturns(arena_index, class_index);
        
        Slab* slab = arena.partial_slabs_[class_index];
        if (slab) {
            result = ExtractBlocksFromSlab(slab, class_index, target_count, actual_count, arena_index);
        } else {
            Slab* new_slab = AllocateNewSlab(class_index, arena_index);
            if (new_slab) {
                PushSlab(arena.partial_slabs_[class_index], new_slab);
                result = ExtractBlocksFromSlab(new_slab, class_index, target_count, actual_count, arena_index);
            }
        }
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
    uint32_t arena_index = head->slab->arena_index;
    Arena& arena = arenas_[arena_index];

    if (arena.class_locks_[class_index].test_and_set(std::memory_order_acquire)) {
        DeferredDeallocateBatch(head);
        return;
    }

    auto process_list = [&](FreeBlock* list) {
        while (list) {
            FreeBlock* next = list->next;
            Slab* slab = list->slab;
            bool was_full = slab->IsFull();
            slab->Deallocate(list);

            if (was_full) {
                RemoveSlab(arena.full_slabs_[class_index], slab);
                PushSlab(arena.partial_slabs_[class_index], slab);
            }

            if (slab->IsEmpty()) {
                RemoveSlab(arena.partial_slabs_[class_index], slab);
                PageHeapFree(slab, slab->memory_size);
            }
            list = next;
        }
    };

    process_list(head);

    FreeBlock* pending = arena.pending_returns_[class_index].head.exchange(nullptr, std::memory_order_acquire);
    process_list(pending);

    arena.class_locks_[class_index].clear(std::memory_order_release);
}

void* GlobalHeap::AllocateLarge(std::size_t size) {
    std::size_t page_size = OSMemory::GetPageSize();
    std::size_t alloc_size = (size + page_size - 1) & ~(page_size - 1);
    return PageHeapAllocate(alloc_size);
}

void GlobalHeap::DeallocateLarge(void* ptr, std::size_t size) {
    std::size_t page_size = OSMemory::GetPageSize();
    std::size_t alloc_size = (size + page_size - 1) & ~(page_size - 1);
    PageHeapFree(ptr, alloc_size);
}

} // namespace FastAlloc
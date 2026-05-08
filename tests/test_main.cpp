#include <gtest/gtest.h>
#include "fast_alloc.h"
#include <vector>
#include <thread>
#include <mutex>

using namespace FastAlloc;

TEST(FastAllocTest, BasicAllocation) {
    void* ptr = fast_malloc(64);
    EXPECT_NE(ptr, nullptr);
    fast_free(ptr);
}

TEST(FastAllocTest, LargeAllocation) {
    void* ptr = fast_malloc(1024 * 1024); // 1MB
    EXPECT_NE(ptr, nullptr);
    
    // Write some data to ensure it's mapped correctly
    char* c_ptr = static_cast<char*>(ptr);
    c_ptr[0] = 'X';
    c_ptr[1024 * 1024 - 1] = 'Y';
    EXPECT_EQ(c_ptr[0], 'X');
    EXPECT_EQ(c_ptr[1024 * 1024 - 1], 'Y');

    fast_free(ptr);
}

TEST(FastAllocTest, MultipleAllocations) {
    std::vector<void*> ptrs;
    for (int i = 0; i < 1000; ++i) {
        void* ptr = fast_malloc(i % 512 + 1);
        EXPECT_NE(ptr, nullptr);
        ptrs.push_back(ptr);
    }
    for (void* ptr : ptrs) {
        fast_free(ptr);
    }
}

TEST(FastAllocTest, ReallocAndCalloc) {
    int* ptr = static_cast<int*>(fast_calloc(10, sizeof(int)));
    EXPECT_NE(ptr, nullptr);
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(ptr[i], 0);
        ptr[i] = i;
    }
    
    ptr = static_cast<int*>(fast_realloc(ptr, 20 * sizeof(int)));
    EXPECT_NE(ptr, nullptr);
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(ptr[i], i);
    }
    
    fast_free(ptr);
}

TEST(FastAllocTest, MultiThreading) {
    // Tests thread-local caches and global heap sync
    auto thread_func = []() {
        std::vector<void*> ptrs;
        for (int i = 0; i < 10000; ++i) {
            void* ptr = fast_malloc(32); // Trigger slab allocations rapidly
            EXPECT_NE(ptr, nullptr);
            ptrs.push_back(ptr);
        }
        for (void* ptr : ptrs) {
            fast_free(ptr);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back(thread_func);
    }
    for (auto& t : threads) {
        t.join();
    }
}

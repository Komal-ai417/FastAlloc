#include <gtest/gtest.h>
#include "fast_alloc.h"
#include <thread>
#include <vector>

using namespace FastAlloc;

TEST(FastAllocTest, FreeWithoutMalloc) {
    void* ptr = fast_malloc(64);
    EXPECT_NE(ptr, nullptr);

    auto thread_func = [ptr]() {
        // This thread never calls fast_malloc, so its fast_bins is nullptr
        // It frees the pointer, triggering the bug.
        fast_free(ptr);
    };
    std::thread t(thread_func);
    t.join();
}

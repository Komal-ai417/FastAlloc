#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <numeric>
#include <iomanip>
#include "fast_alloc.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

using namespace FastAlloc;

size_t GetPeakRSS() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS info;
    GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info));
    return (size_t)info.PeakWorkingSetSize;
#else
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return (size_t)(usage.ru_maxrss * 1024); // Linux returns in KB
#endif
}

void MemoryStress(bool use_fast, size_t num_allocs, size_t block_size) {
    std::vector<void*> ptrs;
    ptrs.reserve(num_allocs);
    for (size_t i = 0; i < num_allocs; ++i) {
        void* ptr = use_fast ? fast_malloc(block_size) : std::malloc(block_size);
        if (ptr) {
            // Touch memory to ensure it's backed by physical pages
            volatile char* p = static_cast<char*>(ptr);
            p[0] = 1;
            p[block_size - 1] = 1;
            ptrs.push_back(ptr);
        }
    }
    for (void* ptr : ptrs) {
        if (use_fast) fast_free(ptr);
        else std::free(ptr);
    }
}

int main(int argc, char** argv) {
    bool use_fast = false;
    if (argc > 1 && std::string(argv[1]) == "--fast") {
        use_fast = true;
    }

    size_t num_threads = 8;
    size_t allocs_per_thread = 10000;
    size_t block_size = 512;

    std::cout << "Testing " << (use_fast ? "FastAlloc" : "StdMalloc") << "..." << std::endl;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::vector<std::thread> threads;
    for (size_t i = 0; i < num_threads; ++i) {
        threads.emplace_back(MemoryStress, use_fast, allocs_per_thread, block_size);
    }
    for (auto& t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    size_t peak_rss = GetPeakRSS();
    
    std::cout << "-------------------------------------------" << std::endl;
    std::cout << "Threads:         " << num_threads << std::endl;
    std::cout << "Allocs/Thread:   " << allocs_per_thread << std::endl;
    std::cout << "Block Size:      " << block_size << " bytes" << std::endl;
    std::cout << "Total Allocated: " << (num_threads * allocs_per_thread * block_size) / (1024 * 1024) << " MB" << std::endl;
    std::cout << "Peak RSS:        " << peak_rss / (1024 * 1024) << " MB" << std::endl;
    std::cout << "Time:            " << std::fixed << std::setprecision(4) << diff.count() << " s" << std::endl;
    std::cout << "-------------------------------------------" << std::endl;

    return 0;
}

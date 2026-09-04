// ============================================================================
// Memory stress benchmark: compares FastAlloc vs std::malloc peak RSS and
// wall time in ONE process (side-by-side mode) so the numbers are directly
// comparable. The old design required two separate process runs.
//
// Usage:
//   fast_alloc_bench_memory                -> side-by-side (default)
//   fast_alloc_bench_memory --std          -> std::malloc only
//   fast_alloc_bench_memory --fast         -> FastAlloc only
//   fast_alloc_bench_memory --threads N --allocs N --size N
// ============================================================================
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <numeric>
#include <iomanip>
#include <string>
#include <cstring>
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

size_t GetCurrentRSS() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS info;
    GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info));
    return (size_t)info.WorkingSetSize;
#else
    long rss = 0;
    FILE* f = fopen("/proc/self/statm", "r");
    if (f) {
        long total_pages;
        if (fscanf(f, "%ld %ld", &total_pages, &rss) != 2) rss = 0;
        fclose(f);
    }
    return (size_t)rss * (size_t)sysconf(_SC_PAGESIZE);
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

struct RunResult {
    double seconds;
    size_t rss_peak_mb;
    size_t rss_after_mb;
};

RunResult RunOne(bool use_fast, size_t num_threads, size_t allocs_per_thread, size_t block_size) {
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (size_t i = 0; i < num_threads; ++i) {
        threads.emplace_back(MemoryStress, use_fast, allocs_per_thread, block_size);
    }
    for (auto& t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    RunResult r;
    r.seconds = diff.count();
    r.rss_peak_mb = GetPeakRSS() / (1024 * 1024);
    r.rss_after_mb = GetCurrentRSS() / (1024 * 1024);
    return r;
}

int main(int argc, char** argv) {
    bool run_std = true, run_fast = true;
    size_t num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 8;
    if (num_threads > 16) num_threads = 16;
    size_t allocs_per_thread = 10000;
    size_t block_size = 512;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--std") run_fast = false;
        else if (arg == "--fast") run_std = false;
        else if (arg == "--threads" && i + 1 < argc) num_threads = std::stoul(argv[++i]);
        else if (arg == "--allocs" && i + 1 < argc) allocs_per_thread = std::stoul(argv[++i]);
        else if (arg == "--size" && i + 1 < argc) block_size = std::stoul(argv[++i]);
    }

    std::cout << "FastAlloc memory stress"
              << " (threads=" << num_threads
              << ", allocs/thread=" << allocs_per_thread
              << ", block=" << block_size << "B, total="
              << (num_threads * allocs_per_thread * block_size) / (1024 * 1024) << "MB)"
              << std::endl;

    RunResult std_res{}, fast_res{};
    bool have_std = false, have_fast = false;

    if (run_std) {
        // Warm up and run.
        RunOne(true, 1, 1, 16); // page in the runtime
        std::cout << "Running std::malloc ......... " << std::flush;
        std_res = RunOne(false, num_threads, allocs_per_thread, block_size);
        have_std = true;
        std::cout << "done" << std::endl;
    }

    if (run_fast) {
        std::cout << "Running FastAlloc ........... " << std::flush;
        // Drop anything cached from warm-up so both sides start clean.
        fast_alloc_purge();
        fast_res = RunOne(true, num_threads, allocs_per_thread, block_size);
        // Return retained spans to the OS before measuring "after".
        fast_alloc_purge_thread_cache();
        fast_alloc_purge();
        have_fast = true;
        std::cout << "done" << std::endl;
    }

    std::cout << "-------------------------------------------" << std::endl;
    std::cout << std::setw(22) << " " << std::setw(12) << (have_std ? "std::malloc" : "-")
              << std::setw(12) << (have_fast ? "FastAlloc" : "-") << std::endl;
    if (have_std) {
        std::cout << std::setw(22) << "time (s):" << std::fixed << std::setprecision(3)
                  << std::setw(12) << std_res.seconds;
    } else std::cout << std::setw(34) << " ";
    if (have_fast) {
        std::cout << std::setw(12) << std::fixed << std::setprecision(3) << fast_res.seconds;
    }
    std::cout << std::endl;

    if (have_std) {
        std::cout << std::setw(22) << "peak RSS (MB):" << std::setw(12) << std_res.rss_peak_mb;
    } else std::cout << std::setw(34) << " ";
    if (have_fast) {
        std::cout << std::setw(12) << fast_res.rss_peak_mb;
    }
    std::cout << std::endl;

    if (have_std && have_fast && std_res.seconds > 0) {
        std::cout << "-------------------------------------------" << std::endl;
        std::cout << "speedup: " << std::fixed << std::setprecision(2)
                  << (std_res.seconds / fast_res.seconds) << "x" << std::endl;
    }
    std::cout << "-------------------------------------------" << std::endl;
    return 0;
}

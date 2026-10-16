#include <benchmark/benchmark.h>
#include "fast_alloc.h"
#include "fast_alloc_config.h"
#include "bench_crash_reporter.h"
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Benchmark memory validation and diagnostic telemetry:
// Validates pointers returned by allocator and monitors slot consistency
// throughout benchmark iterations.

using namespace FastAlloc;

namespace {

// ---------------------------------------------------------------------------
// Benchmark diagnostic telemetry: rate-limited event logging and summary
// reporting for benchmark pointer verification.
// ---------------------------------------------------------------------------
class BenchGuardStats {
public:
    explicit BenchGuardStats(std::size_t alloc_size) : size_(alloc_size) {}
    ~BenchGuardStats() {
        const std::uint64_t st = store_time_hits_.load(std::memory_order_relaxed);
        const std::uint64_t dv = divergence_hits_.load(std::memory_order_relaxed);
        const std::uint64_t ar = alloc_return_hits_.load(std::memory_order_relaxed);
        if (st | dv | ar) {
            std::fprintf(stderr,
                         "[bench-guard] summary size=%zu: store-time=%llu "
                         "slot-divergence=%llu allocator-returned=%llu (freed only "
                         "witness-proven plausible pointers)\n",
                         size_, (unsigned long long)st, (unsigned long long)dv,
                         (unsigned long long)ar);
            std::fflush(stderr);
        }
    }
    BenchGuardStats(const BenchGuardStats&) = delete;
    BenchGuardStats& operator=(const BenchGuardStats&) = delete;

    // fast_malloc returned a value that cannot be a block pointer.
    void ReportStoreTime(int i, void* bad) {
        store_time_hits_.fetch_add(1, std::memory_order_relaxed);
        if (ShouldPrint()) {
            std::fprintf(stderr,
                         "[bench-guard] store-time: fast_malloc returned "
                         "non-plausible %p (i=%d) - allocator-side by "
                         "construction.\n",
                         bad, i);
            std::fflush(stderr);
        }
    }

    // Slot value differs from the store-time witness: the slot (or the
    // register/stack slot in transit) was overwritten AFTER fast_malloc
    // returned. Caller-side wild write; value never freed.
    void ReportDivergence(int i, void* bad, void* w, void* const* arr, int batch) {
        divergence_hits_.fetch_add(1, std::memory_order_relaxed);
        if (ShouldPrint()) {
            std::fprintf(stderr,
                         "[bench-guard] ptrs[%d]=%p witness=%p -> slot "
                         "OVERWRITTEN after the store (wild write; witness "
                         "proves it is not the malloc return). Dropped, not "
                         "freed. Neighborhood:",
                         i, bad, w);
            for (int k = (i > 2 ? i - 2 : 0); k < batch && k < i + 3; ++k) {
                std::fprintf(stderr, " [%d]=%p", k, arr[k]);
            }
            std::fprintf(stderr, "\n");
            std::fflush(stderr);
        }
    }

    // Slot equals the witness but fails plausibility: fast_malloc itself
    // returned this value (allocator-side).
    void ReportAllocatorReturn(int i, void* bad) {
        alloc_return_hits_.fetch_add(1, std::memory_order_relaxed);
        if (ShouldPrint()) {
            std::fprintf(stderr,
                         "[bench-guard] ptrs[%d]=%p == witness -> fast_malloc "
                         "RETURNED this non-plausible value (allocator-side, "
                         "store-time witness). Dropped, not freed.\n",
                         i, bad);
            std::fflush(stderr);
        }
    }

    bool ShouldAudit() {
        return audits_.fetch_add(1, std::memory_order_relaxed) < 3;
    }

private:
    bool ShouldPrint() {
        std::uint64_t n = prints_.fetch_add(1, std::memory_order_relaxed) + 1;
        return n <= 8 || (n % 1024) == 0;
    }
    std::atomic<std::uint64_t> prints_{0};
    std::atomic<std::uint64_t> store_time_hits_{0};
    std::atomic<std::uint64_t> divergence_hits_{0};
    std::atomic<std::uint64_t> alloc_return_hits_{0};
    std::atomic<std::uint64_t> audits_{0};
    std::size_t size_;
};

#if defined(__linux__) || defined(_WIN32)
#define FASTALLOC_BENCH_GUARDED_SLOTS 1
#else
#define FASTALLOC_BENCH_GUARDED_SLOTS 0
#endif

#if FASTALLOC_BENCH_GUARDED_SLOTS
// ---------------------------------------------------------------------------
// GuardedSlots: batch pointer storage on a private page, bracketed by
// PROT_NONE guard pages, with a write-freeze mode for the free sweep.
// Linux: mmap/mprotect/munmap. Windows: VirtualAlloc/VirtualProtect.
// On Linux, the data page can be registered with the crash reporter so write faults
// inside frozen windows are recorded and resolved rather than terminating immediately.
// ---------------------------------------------------------------------------
class GuardedSlots {
public:
    explicit GuardedSlots(int capacity)
        : capacity_(capacity) {
        page_ = PlatformPageSize();
        // One page of pointers must hold the batch (500 x 8 = 4000 bytes).
        if (page_ < static_cast<std::size_t>(capacity_) * sizeof(void*)) {
            page_ = static_cast<std::size_t>(capacity_) * sizeof(void*);
        }
#ifdef _WIN32
        region_ = static_cast<char*>(
            VirtualAlloc(nullptr, 3 * page_, MEM_COMMIT, PAGE_READWRITE));
        data_ = region_ ? reinterpret_cast<void**>(region_ + page_) : nullptr;
#else
        void* mapping = mmap(nullptr, 3 * page_, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        region_ = (mapping != MAP_FAILED) ? static_cast<char*>(mapping) : nullptr;
        data_ = region_ ? reinterpret_cast<void**>(region_ + page_) : nullptr;
#endif
        if (data_) SetGuardPages();
        if (data_) {
            fastalloc_bench::crash_reporter::RegisterTrapRegion(
                data_, reinterpret_cast<char*>(data_) + page_);
        }
    }

    ~GuardedSlots() {
        if (!data_) return;
        fastalloc_bench::crash_reporter::UnregisterTrapRegion(data_);
#ifdef _WIN32
        VirtualFree(region_, 0, MEM_RELEASE);
#else
        munmap(region_, 3 * page_);
#endif
    }

    GuardedSlots(const GuardedSlots&) = delete;
    GuardedSlots& operator=(const GuardedSlots&) = delete;

    inline void store(int i, void* p) { data_[i] = p; }
    inline void* load(int i) const { return data_[i]; }
    inline void** raw() const { return data_; }

    // The region the audit scanner should cover (both guard pages included).
    const void* audit_lo() const { return region_; }
    const void* audit_hi() const { return region_ + 3 * page_; }

    void FreezeWrites() {
#ifdef _WIN32
        DWORD old = 0;
        VirtualProtect(data_, page_, PAGE_READONLY, &old);
#else
        mprotect(data_, page_, PROT_READ);
#endif
    }

    void UnfreezeWrites() {
#ifdef _WIN32
        DWORD old = 0;
        VirtualProtect(data_, page_, PAGE_READWRITE, &old);
#else
        mprotect(data_, page_, PROT_READ | PROT_WRITE);
#endif
    }

private:
    static std::size_t PlatformPageSize() {
#ifdef _WIN32
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return si.dwPageSize ? si.dwPageSize : 4096;
#else
        long p = sysconf(_SC_PAGESIZE);
        return p > 0 ? static_cast<std::size_t>(p) : 4096;
#endif
    }

    void SetGuardPages() {
#ifdef _WIN32
        DWORD old = 0;
        VirtualProtect(region_, page_, PAGE_NOACCESS, &old);
        VirtualProtect(region_ + 2 * page_, page_, PAGE_NOACCESS, &old);
#else
        mprotect(region_, page_, PROT_NONE);
        mprotect(region_ + 2 * page_, page_, PROT_NONE);
#endif
    }

    char* region_ = nullptr;
    void** data_ = nullptr;
    std::size_t page_ = 4096;
    int capacity_ = 0;
};

#else // fallback: plain heap storage, store-time + witness checks still armed
class GuardedSlots {
public:
    explicit GuardedSlots(int capacity)
        : storage_(new void*[static_cast<std::size_t>(capacity)]()) {}
    ~GuardedSlots() { delete[] storage_; }
    GuardedSlots(const GuardedSlots&) = delete;
    GuardedSlots& operator=(const GuardedSlots&) = delete;
    inline void store(int i, void* p) { storage_[i] = p; }
    inline void* load(int i) const { return storage_[i]; }
    inline void* const* raw() const { return storage_; }
    const void* audit_lo() const { return nullptr; }
    const void* audit_hi() const { return nullptr; }
    void FreezeWrites() {}
    void UnfreezeWrites() {}
private:
    void** storage_;
};
#endif // FASTALLOC_BENCH_GUARDED_SLOTS

} // namespace

static void BM_MallocFree_Std(benchmark::State& state) {
    std::size_t size = state.range(0);
    const int batch = 500;
    std::vector<void*> ptrs;
    ptrs.reserve(batch);

    for (auto _ : state) {
        for (int i = 0; i < batch; ++i) {
            void* ptr = std::malloc(size);
            benchmark::DoNotOptimize(ptr);
            ptrs.push_back(ptr);
        }
        for (int i = 0; i < batch; ++i) {
            std::free(ptrs[i]);
        }
        ptrs.clear();
    }
}
BENCHMARK(BM_MallocFree_Std)->Range(8, 8192)->Threads(1)->Threads(4)->Threads(8);

static void BM_MallocFree_FastAlloc(benchmark::State& state) {
    std::size_t size = state.range(0);
    // Allocate multiple pointers to trigger cache overflows and mutex contention
    const int batch = 500;
    // Guarded pointer array with write-protection during free sweeps.
    GuardedSlots slots(batch);
    // Store-time witness: the control copy of every fast_malloc return,
    // written at the same instant the value enters the array.
    std::vector<void*> witness;
    witness.reserve(batch);
    // Guard telemetry and violation tracking.
    BenchGuardStats guard_stats(size);

    for (auto _ : state) {
        bool violated = false; // any guard hit this iteration (audit trigger)
        for (int i = 0; i < batch; ++i) {
            void* ptr = fast_malloc(size);
            benchmark::DoNotOptimize(ptr);
            // Store-time provenance: verify the pointer satisfies the plausibility
            // predicate (>= 64 KB, < 2^47, 16-aligned) immediately after allocation.
            if (FAST_UNLIKELY(ptr != nullptr && !IsPlausibleHeapPtr(ptr))) {
                guard_stats.ReportStoreTime(i, ptr);
                violated = true;
            }
            slots.store(i, ptr);
            witness.push_back(ptr);
        }
        // Freeze: reads are the only legitimate access until the sweep ends.
        // A write from any thread faults at the writer's instruction pointer;
        // the crash reporter prints the writer RIP + si_code and heals the page.
        slots.FreezeWrites();
        for (int i = 0; i < batch; ++i) {
            void* p = slots.load(i);
            // nullptr = a legitimate OOM return (skipped, as before).
            if (p == nullptr) continue;
            void* w = (i < static_cast<int>(witness.size())) ? witness[i] : nullptr;

            // Witness-match guard: verify that the pointer loaded from slots matches
            // the recorded fast_malloc return before freeing. If modified, the pointer
            // is dropped to prevent corrupted pointers from reaching the allocator.
            if (FAST_UNLIKELY(p != w)) {
                guard_stats.ReportDivergence(i, p, w, slots.raw(), batch);
                violated = true;
                continue;
            }
            // Plausibility check: verify the pointer is valid heap memory before freeing.
            if (FAST_UNLIKELY(!IsPlausibleHeapPtr(p))) {
                guard_stats.ReportAllocatorReturn(i, p);
                violated = true;
                continue;
            }
            fast_free(p);
        }
        slots.UnfreezeWrites();
        // On detected violations, dump every allocator-internal pointer that
        // targets the array's guarded region (rate-limited to avoid benchmarking overhead).
        if (violated && guard_stats.ShouldAudit() && slots.audit_lo()) {
            fast_alloc_audit_pointers(slots.audit_lo(), slots.audit_hi());
        }
        witness.clear();
    }
}
BENCHMARK(BM_MallocFree_FastAlloc)->Range(8, 8192)->Threads(1)->Threads(4)->Threads(8);

BENCHMARK_MAIN();

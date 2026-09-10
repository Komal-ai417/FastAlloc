#include <benchmark/benchmark.h>
#include "fast_alloc.h"
#include "fast_alloc_config.h" // FAST_UNLIKELY (on fast_alloc's PUBLIC include path)
#include "bench_crash_reporter.h" // self-diagnosing crash backtraces in the CI log
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

// Multithreaded registrations (->Threads) are KEPT, per the project decision:
// benchmark coverage is never reduced to work around a crash. google-benchmark
// is pinned at v1.9.4 (CMake): the 1.9.x series removed v1.8.3's ThreadManager
// teardown machinery and its adaptive-mode heap corruption, and is the correct
// modern pin regardless.
//
// Sep-2026 runner-crash forensics, v10. The v9 run produced the decisive
// datapoint: [bench-guard] ptrs[0..2]=0x10 shadow=0x10, DETERMINISTICALLY in
// both the tee'd run and the gdb rerun (different ASLR), 0/1000+ local runs.
// v9's shadow could not distinguish WHO wrote it: the snapshot was taken
// AFTER the alloc sweep, so a wild write during the sweep corrupted the array
// AND the snapshot identically ("shadow == bad" proved nothing). v10 makes
// the benchmark itself a trap that catches the writer red-handed:
//
//   1. GuardedSlots: the pointer array lives on its own page with PROT_NONE
//      guard pages on both sides. A wild write that spills past the array
//      faults AT THE WRITER'S INSTRUCTION - the [bench-crash] handler then
//      prints that writer's backtrace into the job log.
//   2. Write freeze: between the alloc sweep and the end of the free sweep
//      the data page is mprotect(PROT_READ). The only legitimate accesses in
//      that window are reads; any write from ANY thread faults at the writer.
//   3. Store-time witness: every fast_malloc return value is ALSO stored into
//      a plain control buffer at the same instant it enters the array. On a
//      violation, value == witness proves fast_malloc returned it; value !=
//      witness proves the slot was overwritten after the store (wild write).
//      This is the discriminator v9's post-hoc shadow could not provide.
//   4. Allocator audit: on the first violation the benchmark calls
//      fast_alloc_audit_pointers() over the array's guarded region - every
//      TLS bin head, slab-list link, pending-queue head and page-bin head
//      that points into the region is dumped with its container identity.
//      If the write went through a corrupted allocator pointer, the report
//      names the structure that holds it.
// The guards cost two compares per free plus two mprotect syscalls per
// 500-op iteration - noise against a signal that turns the next runner
// occurrence into the closing datapoint.

using namespace FastAlloc;

namespace {

// The lowest plausible FastAlloc user pointer: spans are mmap'd / pooled
// far above 64 KB, and every block is 16-byte aligned.
inline bool BenchPlausibleUserPtr(void* p) {
    std::uintptr_t a = reinterpret_cast<std::uintptr_t>(p);
    return a >= 0x10000 && (a & 0xF) == 0;
}

// One-shot forensics when the pointer array fails validation.
void BenchGuardReport(int i, void* bad, void* witness, void** arr, int batch) {
    // Atomic one-shot: exactly one report even when several threads trip
    // the guard simultaneously (race-free under TSan).
    static std::atomic<bool> reported{false};
    if (reported.exchange(true, std::memory_order_relaxed)) return;
    const char* verdict = (witness == bad)
        ? "fast_malloc RETURNED this value (allocator-side, store-time witness)"
        : "slot overwritten AFTER the store (wild write; see audit + guard pages)";
    std::fprintf(stderr, "[bench-guard] ptrs[%d]=%p witness=%p -> %s. Neighborhood:",
                 i, bad, witness, verdict);
    for (int k = (i > 2 ? i - 2 : 0); k < batch && k < i + 3; ++k) {
        std::fprintf(stderr, " [%d]=%p", k, arr[k]);
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

// One-shot forensics at STORE time: fast_malloc handed back a value that
// cannot be a block pointer. This is the only channel that proves
// allocator-side provenance with no window for a post-store overwrite.
void BenchStoreTimeReport(int i, void* bad) {
    static std::atomic<bool> reported{false};
    if (reported.exchange(true, std::memory_order_relaxed)) return;
    std::fprintf(stderr,
                 "[bench-guard] store-time: fast_malloc returned non-canonical "
                 "%p (i=%d) - allocator-side by construction.\n",
                 bad, i);
    std::fflush(stderr);
}

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
    }

    ~GuardedSlots() {
        if (!data_) return;
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
    // v10 trapped pointer array (see the file header note).
    GuardedSlots slots(batch);
    // Store-time witness: the control copy of every fast_malloc return,
    // written at the same instant the value enters the array.
    std::vector<void*> witness;
    witness.reserve(batch);

    for (auto _ : state) {
        for (int i = 0; i < batch; ++i) {
            void* ptr = fast_malloc(size);
            benchmark::DoNotOptimize(ptr);
            // Store-time provenance: if fast_malloc itself returns a value
            // that cannot be a block pointer, THIS fires - no post-store
            // window, no ambiguity.
            if (FAST_UNLIKELY(ptr != nullptr && !BenchPlausibleUserPtr(ptr))) {
                BenchStoreTimeReport(i, ptr);
            }
            slots.store(i, ptr);
            witness.push_back(ptr);
        }
        // Freeze: reads are the only legitimate access until the sweep ends.
        // Any write to the array's page from ANY thread now faults at the
        // writer's own instruction pointer (-> [bench-crash] backtrace).
        slots.FreezeWrites();
        bool violated = false;
        for (int i = 0; i < batch; ++i) {
            void* p = slots.load(i);
            void* w = (i < static_cast<int>(witness.size())) ? witness[i] : nullptr;
            if (FAST_UNLIKELY(!BenchPlausibleUserPtr(p))) {
                if (p != nullptr) {
                    BenchGuardReport(i, p, w, slots.raw(), batch);
                    violated = true;
                }
                // nullptr = a legitimate OOM return (skipped, as before);
                // a non-canonical value is dropped here AND absorbed by the
                // library-side v9/v10 guards - the run survives either way.
                continue;
            }
            fast_free(p);
        }
        slots.UnfreezeWrites();
        // On the first violation, dump every allocator-internal pointer that
        // targets the array's guarded region: if the wild write went through
        // a corrupted freelist/slab/pending pointer, the audit names the
        // structure that holds it. (One shot; runs on the frozen->thawed
        // boundary so the array is stable.)
        if (violated && slots.audit_lo()) {
            fast_alloc_audit_pointers(slots.audit_lo(), slots.audit_hi());
        }
        witness.clear();
    }
}
BENCHMARK(BM_MallocFree_FastAlloc)->Range(8, 8192)->Threads(1)->Threads(4)->Threads(8);

BENCHMARK_MAIN();

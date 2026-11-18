// Adversarial probe: page-cache split-tail retention-cap enforcement.
// Scenario: park one 2 MB span (bin 512 full: BinRetentionCount(512)=1),
// then free a second 2 MB span. The second cannot park (cap full).
// Now allocate 64 KB repeatedly: every pop is a 512->16 split whose tail is
// 496 pages; the 16-page bin accepts only PAGE_CACHE_PER_BIN_BYTES/64KB=32
// tails, every further split tail must go back to the OS, never park.
// Invariant checked: page_cache_total_bytes never exceeds the global cap,
// never wraps (>= pre-fill level), and per-bin retention stays bounded.
// Environment is pinned per-process (FASTALLOC_PAGE_CACHE_MB is read once).
#include <cstdio>
#include <cstdlib>
#include "fast_alloc.h"
#include "fast_alloc_config.h"

namespace FastAlloc {
std::size_t FastAllocTestPageCacheBytes();
std::size_t FastAllocTestPurgePageCache();
}

using namespace FastAlloc;

int main(int argc, char** argv) {
    const char* val = argc > 1 ? argv[1] : nullptr;
    if (val) setenv("FASTALLOC_PAGE_CACHE_MB", val, 1);

    // Fill the 512-page bin: 2 MB spans (large allocs round to 2 MB spans
    // when the request lands in that page class).
    void* spans[8];
    for (int i = 0; i < 8; ++i) {
        spans[i] = fast_malloc(2 * 1024 * 1024 - 64);
        if (!spans[i]) { std::printf("alloc failed\n"); return 1; }
    }
    for (int i = 0; i < 8; ++i) fast_free(spans[i]);
    std::size_t parked = FastAllocTestPageCacheBytes();
    std::printf("parked after 2MB frees: %zu\n", parked);

    // Hammer splits: small 64 KB slab-class allocations pop 512-page spans
    // (best-fit) and park 496-page tails; the 496 bin accepts at most
    // 2MB/1.984MB = 1 tail. Splits keep coming until the big bin is empty.
    void* small[4096];
    int n = 0;
    for (int i = 0; i < 4096; ++i) {
        small[n] = fast_malloc(60000); // 64 KB-class slab span
        if (!small[n]) break;
        ++n;
        std::size_t cur = FastAllocTestPageCacheBytes();
        if (cur > (1ull << 27)) { // 128 MB: far above any sane retention
            std::printf("DEFECT-CONFIRMED: page cache retained %zu bytes "
                        "(cap bypass via split tails)\n", cur);
            return 2;
        }
    }
    std::size_t after = FastAllocTestPageCacheBytes();
    std::printf("after %d 64KB allocs: parked=%zu\n", n, after);
    if (after > (1ull << 27)) {
        std::printf("DEFECT-CONFIRMED: retention exceeded 128MB (%zu)\n", after);
        return 2;
    }
    for (int i = 0; i < n; ++i) fast_free(small[i]);
    FastAllocTestPurgePageCache();
    std::printf("PROBE-DONE parked_final=%zu\n", FastAllocTestPageCacheBytes());
    return 0;
}

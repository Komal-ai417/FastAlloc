// Adversarial probe: stats balance across aligned free path (T8).
// fast_aligned_alloc stores raw in stash; fast_free recurses to
// fast_free(raw) which counts UsableSize(class) free bytes. The ALLOC
// was counted by fast_malloc(raw_size) as UsableSize(class of raw_size).
// Same class -> balanced. Probe: alloc/free stats equality across many
// aligned allocations + verify current_live_bytes returns to baseline.
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include "fast_alloc.h"

using namespace FastAlloc;

int main() {
    // Warm the thread cache first so refill batches don't skew counts.
    void* warm[64];
    for (int i = 0; i < 64; ++i) warm[i] = fast_malloc(1000);
    for (int i = 0; i < 64; ++i) fast_free(warm[i]);

    FastAllocStats a = fast_alloc_stats();

    // Aligned allocations across all alignments and sizes.
    for (size_t align = 32; align <= 4096; align <<= 1) {
        for (size_t sz : { 16u, 100u, 1000u, 8100u, 20000u }) {
            void* p = fast_aligned_alloc(align, sz);
            if (!p) { std::printf("FAIL aligned(%zu,%zu)\n", align, sz); return 1; }
            fast_free(p);
        }
    }
    // Plain large/small alloc/free pairs.
    for (size_t sz : { 100u, 8176u, 8177u, 100000u, 3000000u }) {
        void* p = fast_malloc(sz); if (!p) return 1;
        fast_free(p);
    }
    // realloc churn: grow/shrink.
    for (size_t sz : { 100u, 9000u, 3000000u }) {
        void* p = fast_malloc(sz); if (!p) return 1;
        p = fast_realloc(p, sz * 2 + 5); if (!p) return 1;
        p = fast_realloc(p, sz / 2 + 1); if (!p) return 1;
        fast_free(p);
    }

    FastAllocStats b = fast_alloc_stats();
    std::printf("small_allocs delta=%zu small_frees delta=%zu\n",
                b.small_allocs - a.small_allocs, b.small_frees - a.small_frees);
    std::printf("large_allocs delta=%zu large_frees delta=%zu\n",
                b.large_allocs - a.large_allocs, b.large_frees - a.large_frees);
    std::printf("live_blocks before=%zu after=%zu\n",
                a.current_live_blocks, b.current_live_blocks);
    if (b.current_live_blocks != a.current_live_blocks) {
        std::printf("IMBALANCE: live blocks leaked %zu\n",
                    b.current_live_blocks - a.current_live_blocks);
    }
    if (b.current_live_blocks != a.current_live_blocks ||
        b.current_live_bytes != a.current_live_bytes) {
        std::printf("IMBALANCE: live bytes delta=%lld blocks delta=%lld\n",
                static_cast<long long>(b.current_live_bytes) -
                static_cast<long long>(a.current_live_bytes),
                static_cast<long long>(b.current_live_blocks) -
                static_cast<long long>(a.current_live_blocks));
        std::printf("PROBE-FAILED\n");
        return 2;  // Non-zero return indicates test failure
    }
    std::printf("PROBE-DONE\n");
    return 0;
}

// Adversarial probe: aligned-realloc overflow + shrink-wrapping edge (T1/T2).
// H1: fast_aligned_alloc(alignment, size) with size near SIZE_MAX:
//     raw_size = size + alignment + 32 wraps -> small raw block returned,
//     stash written OUT OF BOUNDS of the raw block.
// H2: fast_realloc aligned path: old_size stash > 2^30 rejected.
//     Also probe shrink PageRoundUp wrap on aligned blocks.
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include "fast_alloc.h"

using namespace FastAlloc;

int main() {
    // H1: aligned alloc with wrapping raw_size
    const size_t max = static_cast<size_t>(-1);
    for (size_t sz : { max, max - 1, max - 32, max - 33, max - 4096, max - 4128 }) {
        void* p = fast_aligned_alloc(4096, sz);
        std::printf("aligned_alloc(4096, SIZE_MAX-%zu) -> %p\n", max - sz, p);
        if (p) { std::printf("  NON-NULL for wrapping size!\n"); return 2; }
    }
    // control: sane aligned alloc works
    void* ok = fast_aligned_alloc(4096, 1000);
    std::printf("control aligned_alloc(4096, 1000) -> %p %%4096=%zu\n",
                ok, ok ? (uintptr_t)ok % 4096 : 0u);
    fast_free(ok);

    // H2: grow a large aligned block past 2^30 via realloc: functional limit
    void* big = fast_aligned_alloc(256, 1 << 20); // 1 MiB aligned
    std::printf("aligned 1MiB -> %p\n", big);
    // Grow to 2 GiB via realloc on the ALIGNED path (stash-limited?)
    // NOTE: 4 GB machine: this would really allocate 2 GiB - skip the real
    // grow; instead probe the >2^30 stash validation with a fake grow is
    // not possible without real memory. Skip; documented functional limit.
    fast_free(big);

    // H3: realloc(p, huge) on the ALIGNED path: alignment stash recovered,
    // new_size <= old_size path? new_size huge > old_size -> fast_aligned_
    // alloc(alignment, huge) which must fail cleanly (checked above), so
    // aligned path returns nullptr. Confirmed by H1 returning nullptr.
    void* a = fast_aligned_alloc(128, 4096);
    void* r = fast_realloc(a, max - 100);
    std::printf("realloc(aligned, SIZE_MAX-100) -> %p (expect nullptr)\n", r);
    if (r) { std::printf("  DEFECT: non-null for aligned grow wrap\n"); return 3; }
    fast_free(a);

    std::printf("PROBE-DONE\n");
    return 0;
}

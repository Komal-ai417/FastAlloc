// Adversarial probe: large shrink PageRoundUp wrap (T1 sibling path).
// fast_realloc(p, new_size) where new_size > old (grow) is confirmed broken.
// Sibling: the SHRINK branch also computes PageRoundUp(new_size + 16).
// For new_size in [SIZE_MAX-15, SIZE_MAX] the shrink branch is only taken
// when new_size <= old_size -> impossible. BUT: the mid branch
// "new_size <= old_size && !should_shrink" returns ptr directly - fine.
// The real sibling danger: new_size slightly less than a wrap such that
// needed wraps BELOW header->alloc_size -> "shrink" releases wrong size.
// new_size in [SIZE_MAX-15, SIZE_MAX] can't be <= old_size. So shrink
// wrap needs new_size < old_size AND new_size+16 wraps -> impossible
// since old_size < SIZE_MAX-16 realistically. Shrink path is SAFE.
//
// Instead probe: mremap-shrink accounting + PageRoundUp(new_size+16) when
// new_size + 16 wraps ONLY in the GROW path (already confirmed).
// Additional probe here: fast_realloc small->huge (small block path).
#include <cstdio>
#include <cstdint>
#include "fast_alloc.h"

using namespace FastAlloc;

int main() {
    const size_t max = static_cast<size_t>(-1);

    // Small block grown to impossible size: takes generic path
    // (is_large=false, new_size > old_size -> falls to generic malloc).
    // fast_malloc(SIZE_MAX) has the overflow guard -> nullptr. Verify.
    unsigned char* p = static_cast<unsigned char*>(fast_malloc(100));
    void* r = fast_realloc(p, max);
    std::printf("realloc(small 100, SIZE_MAX) -> %p (expect nullptr)\n", r);
    if (r) { std::printf("DEFECT: small->huge returned non-null\n"); return 2; }
    fast_free(p);

    // Large block with new_size just BELOW the wrap: e.g. SIZE_MAX - 4096 -
    // no wrap (sum < SIZE_MAX), needed = huge > alloc -> mremap fails ->
    // falls to copy path -> fast_malloc(SIZE_MAX-4096) -> guard -> nullptr.
    void* big = fast_malloc(1 << 20);
    void* r2 = fast_realloc(big, max - 4096);
    std::printf("realloc(large 1MiB, SIZE_MAX-4096) -> %p (expect nullptr)\n", r2);
    if (r2) { std::printf("DEFECT: large->huge(-4096) non-null\n"); return 3; }
    fast_free(big);

    // calloc overflow already guarded (test suite). Double-check extremes:
    if (fast_calloc(max, max)) { std::printf("DEFECT calloc\n"); return 4; }
    if (fast_malloc(max)) { std::printf("DEFECT malloc\n"); return 5; }

    // realloc(nullptr, huge) -> malloc path guarded
    if (fast_realloc(nullptr, max)) { std::printf("DEFECT realloc(null)\n"); return 6; }

    std::printf("PROBE-DONE\n");
    return 0;
}

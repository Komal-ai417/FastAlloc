// Adversarial probe: calloc overflow division guard with num=0 edge (T1).
// fast_calloc: `if (num != 0 && size > SIZE_MAX/num) return nullptr;`
// num=0: total=0 -> fast_malloc(0)=1byte; fine. But what about
// num=1, size=SIZE_MAX -> SIZE_MAX/1 = SIZE_MAX; size > SIZE_MAX false;
// total = SIZE_MAX -> fast_malloc(SIZE_MAX): large path, alloc_size =
// SIZE_MAX+16 wraps -> caught by `alloc_size < size` guard. OK.
// Sibling check: LARGE path in fast_malloc: alloc_size > max-page+1 guard:
// for size = SIZE_MAX - 4095: alloc_size = SIZE_MAX-4079; page=4096;
// max-page+1 = SIZE_MAX-4095; SIZE_MAX-4079 > SIZE_MAX-4095 -> guard fires.
// BUT size = SIZE_MAX-4096: alloc_size = SIZE_MAX-4080 > SIZE_MAX-4095 ->
// guard fires. size = SIZE_MAX-4111 (boundary from probe): alloc_size =
// SIZE_MAX-4095 = max-page+1 -> NOT > -> passes! then (alloc_size+4095)
// = SIZE_MAX -> &~4095 = SIZE_MAX-4095 (no wrap: SIZE_MAX+0... wait
// SIZE_MAX-4095+4095 = SIZE_MAX exactly; no wrap). alloc_size stays
// SIZE_MAX-4095 -> AllocateLargeCached(SIZE_MAX-4095) -> PageHeapAllocate
// -> pages = huge > MAX_CACHED_PAGES -> mmap(SIZE_MAX-4095) fails ->
// nullptr. Safe.
// What about size in [SIZE_MAX-4110, SIZE_MAX-4096]? alloc_size in
// [SIZE_MAX-4094, SIZE_MAX-4080] -> all > SIZE_MAX-4095 -> guard fires.
// So only size = SIZE_MAX-4111 exactly reaches mmap with a giant size ->
// mmap fails -> nullptr. CONFIRMED SAFE by analysis; verify at runtime.
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include "fast_alloc.h"

using namespace FastAlloc;

int main() {
    const size_t max = static_cast<size_t>(-1);
    // sweep the near-miss region around the malloc guard boundary
    for (size_t d : { 4095u, 4094u, 4096u, 4110u, 4111u, 4112u, 4127u, 5000u }) {
        void* p = fast_malloc(max - d);
        std::printf("fast_malloc(SIZE_MAX-%zu) -> %p\n", d, p);
        if (p) { std::printf("DEFECT: non-null\n"); return 2; }
    }
    // calloc boundary: num*size exactly SIZE_MAX
    if (fast_calloc(3, (max/3)+1)) { std::printf("DEFECT calloc exact wrap\n"); return 3; }
    // calloc num=0,size=0 -> unique pointer
    void* z1 = fast_calloc(0, 512);
    void* z2 = fast_calloc(512, 0);
    std::printf("calloc(0,512)=%p calloc(512,0)=%p distinct=%d\n", z1, z2, z1 != z2);
    fast_free(z1); fast_free(z2);
    std::printf("PROBE-DONE\n");
    return 0;
}

// Adversarial probe: how large can a "successful" realloc lie?
// Confirmed: realloc(large, SIZE_MAX-4096) returns the old pointer.
// Map the boundary: which new_size values get the false success?
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include "fast_alloc.h"

using namespace FastAlloc;

int main() {
    const size_t max = static_cast<size_t>(-1);
    // probe: does the false success only happen when PageRoundUp wraps
    // (new_size + 16 <= SIZE_MAX but +page rounds... no: PageRoundUp(n)
    // = (n + page-1) & ~(page-1): for n > SIZE_MAX - 4095, n + 4095 wraps.
    // Two wrap points: n+16 (LargeAllocHeader) and n+16+4095 (PageRoundUp).
    // Case A: n in [SIZE_MAX-15, SIZE_MAX]: needed wraps to <= 4096.
    // Case B: n in [SIZE_MAX-4095, SIZE_MAX-17]: n+16 ok, but (n+16)+4095
    //         wraps -> needed = tiny -> needed <= alloc -> "same page
    //         count" branch -> false success.
    for (size_t n : { max - 16, max - 100, max - 4095, max - 4096, max - 4111, max - 4112 }) {
        void* big = fast_malloc(1 << 20);
        void* r = fast_realloc(big, n);
        std::printf("realloc(1MiB, SIZE_MAX-%zu) -> %s\n", max - n, r ? "NON-NULL (LIE)" : "nullptr (correct)");
        if (r) fast_free(r); else fast_free(big);
    }
    // find exact boundary between lie and correct: binary search style probes
    // wrap point B: (n + 16 + 4095) mod 2^64 <= alloc (1MiB+16 rounded)
    // n + 4111 wraps iff n > SIZE_MAX - 4111, i.e. n >= SIZE_MAX - 4110.
    // needed after wrap = n + 4111 - 2^64 which for n = SIZE_MAX-4110 is 1
    // ... 1MiB-ish for n = SIZE_MAX - 4110 + k. All < 1MiB+4096 -> lie.
    // For n <= SIZE_MAX - 4111: no wrap; needed > 1MiB -> grow path ->
    // mremap fails -> copy path -> fast_malloc(n): n + 16 no wrap ->
    // PageRoundUp second wrap? n+16+4095 for n = SIZE_MAX-4111 = SIZE_MAX+80
    // -> WRAPS TOO. So fast_malloc itself returns... has guard:
    // alloc_size > max() - page + 1 check: alloc_size = n+16 = SIZE_MAX-4095;
    // page = 4096; max-page+1 = SIZE_MAX-4095; NOT >, so passes guard;
    // then (alloc_size + 4095) & ~4095 wraps to 0 -> alloc_size = 0 ->
    // AllocateLargeCached(0)?? LargeSizeToClass(0)... size<=4096 -> cls 0.
        // -> PageHeapAllocate(0) -> mmap(0)! Probe it:
    void* big2 = fast_malloc(1 << 20);
    void* r2 = fast_realloc(big2, max - 4111);
    std::printf("realloc(1MiB, SIZE_MAX-4111) -> %p\n", r2);
    if (r2) fast_free(r2); else fast_free(big2);
    void* m = fast_malloc(max - 4095);
    std::printf("fast_malloc(SIZE_MAX-4095) -> %p (guard check)\n", m);
    if (m) fast_free(m);
    return 0;
}

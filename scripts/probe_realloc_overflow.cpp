// Adversarial probe: fast_realloc large-growth integer overflow (T1).
// Hypothesis: PageRoundUp(new_size + 16) wraps for new_size in
// [SIZE_MAX-15, SIZE_MAX]; "needed" becomes a tiny page count <= old span,
// so realloc returns the OLD pointer for a huge request -> caller overflow.
#include <cstdio>
#include <cstdint>
#include <climits>
#include <cerrno>
#include <initializer_list>
#include "fast_alloc.h"

using namespace FastAlloc;

int main() {
    // Step 1: obtain a genuine large allocation (bypasses slab path).
    size_t old_n = 1 << 20; // 1 MiB
    unsigned char* p = static_cast<unsigned char*>(fast_malloc(old_n));
    if (!p) { std::printf("STEP1-FAIL: cannot allocate 1MiB\n"); return 1; }
    for (size_t i = 0; i < old_n; ++i) p[i] = static_cast<unsigned char>(i * 31);

    // Step 2: request a growth whose (new_size + 16) overflows size_t.
    const size_t max = static_cast<size_t>(-1);
    for (size_t over : { max, max - 1, max - 8, max - 15, max - 16, max - 17, max - 4096 }) {
        errno = 0;
        void* r = fast_realloc(p, over);
        std::printf("realloc(p, SIZE_MAX-%zu): -> %p errno=%d\n",
                    max - over, r, errno);
        if (r != nullptr) {
            // Success returned for an impossible request.
            // If r == p, the caller "owns" a 1MiB buffer believed to be
            // SIZE_MAX bytes -> catastrophic heap overflow on write.
            std::printf("  RESULT: NON-NULL RETURN for impossible size (r %s p)\n",
                        r == static_cast<void*>(p) ? "==" : "!=");
            if (r == static_cast<void*>(p)) {
                std::printf("  DEFECT-CONFIRMED: old pointer returned as success\n");
                return 2;
            }
        }
        // p must still be valid after a (correct) failure
        if (r == nullptr && p[0] != 0x00) { std::printf("  old data corrupted?\n"); }
    }
    // control: normal grow still works
    void* ok = fast_realloc(p, old_n * 2);
    std::printf("control realloc grow 2MiB: %p (errno=%d)\n", ok, errno);
    fast_free(ok ? ok : p);
    std::printf("PROBE-DONE\n");
    return 0;
}

// Adversarial probe: FASTALLOC_PAGE_CACHE_MB overflow wrap (T6 env parsing).
// Hypothesis: PageCacheTotalLimit computes mb * 1024 * 1024 in size_t;
// a huge mb value wraps. 2^48 MB -> limit wraps to 0 (cache silently
// disabled); 2^63-1 MB -> limit wraps to ~2^64 (unlimited retention).
// Env must be set before the first FastAlloc call (fresh process).
#include <cstdio>
#include <cstdlib>
#include "fast_alloc.h"
#include "fast_alloc_config.h"

// Test seam (defined in the library).
namespace FastAlloc {
std::size_t FastAllocTestPageCacheBytes();
}

using namespace FastAlloc;

int main(int argc, char** argv) {
    const char* val = argc > 1 ? argv[1] : nullptr;
    if (val) setenv("FASTALLOC_PAGE_CACHE_MB", val, 1);

    // Churn enough 64KB-class spans to populate the page cache.
    void* hold[64];
    for (int i = 0; i < 64; ++i) hold[i] = fast_malloc(60000);
    for (int i = 0; i < 64; ++i) fast_free(hold[i]);

    std::size_t cached = FastAllocTestPageCacheBytes();
    std::printf("PAGE_CACHE_MB=%s -> cached_bytes=%zu\n",
                val ? val : "(default 64)", cached);
    if (val) {
        // Any positive limit should cache these spans; a limit that
        // wrapped to 0 caches nothing.
        if (cached == 0) {
            std::printf("DEFECT-CONFIRMED: retention cap wrapped to 0 (cache silently disabled)\n");
            return 2;
        }
    }
    std::printf("PROBE-DONE\n");
    return 0;
}

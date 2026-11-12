// Adversarial probe: pin down the realloc-churn live-bytes imbalance.
// D1 showed +3002352 on grow (a net LIVE +1 block - expected: between
// snapshots p is live at grown size, that is CORRECT accounting since we
// report DURING the churn). D3 -3002352 frees it back. So D-series was a
// measurement artifact (snapshot while live).
// C: realloc churn (100, 9000, 3000000) - +1499136 net across all three
// despite ending with everything freed. 1499136 = 1454.25 KiB...
// = 3002352 (grow to 3100005) ... hmm not equal. Isolate per-size:
#include <cstdio>
#include <initializer_list>
#include "fast_alloc.h"

using namespace FastAlloc;

static void Warm() {
    void* warm[64];
    for (int i = 0; i < 64; ++i) warm[i] = fast_malloc(1000);
    for (int i = 0; i < 64; ++i) fast_free(warm[i]);
}

int main() {
    int failures = 0;
    Warm();
    FastAllocStats a = fast_alloc_stats();
    for (size_t sz : { 100u, 9000u, 3000000u }) {
        void* p = fast_malloc(sz);
        p = fast_realloc(p, sz * 2 + 5);
        p = fast_realloc(p, sz / 2 + 1);
        fast_free(p);
        FastAllocStats b = fast_alloc_stats();
        long long d_bytes = (long long)(b.current_live_bytes - a.current_live_bytes);
        long long d_blocks = (long long)(b.current_live_blocks - a.current_live_blocks);
        std::printf("sz=%zu: d_live_bytes=%lld d_blocks=%lld\n", sz, d_bytes, d_blocks);
        if (d_bytes != 0 || d_blocks != 0) ++failures;  // Live byte or block drift
        a = b;
    }
    std::printf(failures ? "PROBE-FAILED\n" : "PROBE-DONE\n");
    return failures ? 2 : 0;
}

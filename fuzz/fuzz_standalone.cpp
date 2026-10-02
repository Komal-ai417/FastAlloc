// Standalone entropy-driven fuzzer (no libFuzzer dependency; g++-buildable):
// runs random byte-programs against a persistent allocator state, checks
// invariants after every op, and prints progress. Exit code 0 = clean run.
// Build: g++ -O2 -std=gnu++17 -I../include -I../src fuzz_standalone.cpp
//        [../src/*.cpp] -pthread
// Usage: ./fuzz_standalone <seconds> [seed]
#include "fuzz_core.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>

int main(int argc, char** argv) {
    double budget = argc > 1 ? std::atof(argv[1]) : 60.0;
    unsigned seed = 0;
    if (argc > 2) {
        seed = static_cast<unsigned>(std::atoi(argv[2]));
    } else {
        seed = static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count());
    }
    std::mt19937 rng(seed);
    fuzzcore::Shadow shadow;
    fuzzcore::Stats st;
    auto t0 = std::chrono::steady_clock::now();
    std::size_t inputs = 0;
    for (;;) {
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (el >= budget) break;
        std::size_t len = 1 + rng() % 96;          // 1..96 byte-ops per input
        std::vector<uint8_t> prog(len);
        for (auto& b : prog) b = static_cast<uint8_t>(rng() & 0xFF);
        ++inputs;
        if (!fuzzcore::RunProgram(prog.data(), prog.size(), shadow, st)) {
            std::fprintf(stderr, "STANDALONE FUZZ FAILURE (seed=%u input=%zu)\nbytes:", seed, inputs);
            for (auto b : prog) std::fprintf(stderr, "%02x", b);
            std::fprintf(stderr, "\n");
            return 1;
        }
        // periodically drain: force cross-class reuse pressure
        if (inputs % 512 == 0) {
            for (auto& kv : shadow.live) FastAlloc::fast_free(kv.first);
            shadow.live.clear();
        }
    }
    std::fprintf(stderr,
                 "[fuzz-standalone] inputs=%zu ops=%zu live=%zu bytes=%zu "
                 "(a=%zu L=%zu al=%zu c=%zu r=%zu f=%zu x=%zu ch=%zu) seed=%u budget=%.0fs\n",
                 inputs, shadow.total_ops, shadow.live.size(), shadow.bytes_live(),
                 st.allocs, st.allocs_large, st.aligned, st.callocs, st.reallocs,
                 st.frees, st.cross_frees, st.churn, seed, budget);
    std::printf("FUZZ-CLEAN inputs=%zu ops=%zu\n", inputs, shadow.total_ops);
    return 0;
}

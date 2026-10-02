// libFuzzer target: persistent allocator state across inputs (the shadow
// model accumulates; the heap is NOT reset), ASan+UBSan instrumented.
// Build: clang++ -fsanitize=fuzzer,address,undefined -g -O1 -std=gnu++17
//        -I ../include -I ../src fuzz_ops_libfuzzer.cpp [../src/*.cpp]
#include "fuzz_core.h"
#include <fast_alloc.h>
#include <cstdio>

static fuzzcore::Shadow g_shadow;
static fuzzcore::Stats g_stats;
static bool g_reported = false;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    if (!fuzzcore::RunProgram(data, size, g_shadow, g_stats)) {
        if (!g_reported) {
            g_reported = true;
            std::fprintf(stderr, "input: ");
            for (size_t i = 0; i < size && i < 64; ++i)
                std::fprintf(stderr, "%02x", data[i]);
            std::fprintf(stderr, "\n");
        }
        abort();  // let the sanitizer(s) print the triage stack
    }
    return 0;
}

cmake_minimum_required(VERSION 3.15)
project(FastAlloc VERSION 2.0.0 LANGUAGES CXX)

# Enforce C++17 or higher
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Project options
option(FASTALLOC_BUILD_TESTS "Build unit tests" ON)
option(FASTALLOC_BUILD_BENCHMARKS "Build performance benchmarks" ON)
option(FASTALLOC_OVERRIDE_NEW "Override standard new/delete operators" OFF)
option(FASTALLOC_DEBUG "Enable debug instrumentation (canaries, red zones, poison, registry, leak dump)" OFF)
option(FASTALLOC_SANITIZE "Sanitizer preset: address|thread|leak|undefined|address,undefined (empty = off)" "")

# ---------------------------------------------------------------------------
# Threads (the library itself uses pthreads on POSIX: pthread_key_create etc.)
# ---------------------------------------------------------------------------
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Debug instrumentation
# ---------------------------------------------------------------------------
if(FASTALLOC_DEBUG)
    add_definitions(-DFASTALLOC_DEBUG)
endif()

# ---------------------------------------------------------------------------
# Sanitizers
# ---------------------------------------------------------------------------
if(FASTALLOC_SANITIZE AND NOT MSVC)
    # LTO is incompatible with sanitizers in many toolchains; disable it.
    set(FASTALLOC_SANITIZE_LTO OFF)
    add_compile_options(-fsanitize=${FASTALLOC_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${FASTALLOC_SANITIZE})
elseif(FASTALLOC_SANITIZE AND MSVC)
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/INCREMENTAL:NO)
endif()

# Compiler-specific optimizations
if(MSVC)
    # fopen/fscanf/getenv trigger C4996 deprecation on MSVC; under /W4 /WX
    # they become hard errors, so silence the CRT deprecation noise globally.
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    set(WARNING_FLAGS /W4 /WX)
    if(NOT FASTALLOC_SANITIZE)
        add_compile_options("$<$<CONFIG:Release>:/Ot>" "$<$<CONFIG:Release>:/Ox>" "$<$<CONFIG:Release>:/GL>")
        add_link_options("$<$<CONFIG:Release>:/LTCG>")
    endif()
else()
    # Aggressive optimizations for GCC/Clang
    set(WARNING_FLAGS -Wall -Wextra -Werror)
    if(NOT FASTALLOC_SANITIZE)
        add_compile_options("$<$<CONFIG:Release>:-O3>" "$<$<CONFIG:Release>:-march=native>")
        # -fno-plt is an ELF-target optimization; PE/COFF (MinGW) targets
        # should not use it.
        if(NOT WIN32)
            add_compile_options("$<$<CONFIG:Release>:-fno-plt>")
        endif()
    endif()
endif()

if(FASTALLOC_OVERRIDE_NEW)
    add_definitions(-DFAST_ALLOC_OVERRIDE_NEW)
endif()

# ---------------------------------------------------------------------------
# Core Library
# ---------------------------------------------------------------------------
add_library(fast_alloc STATIC
    src/fast_alloc.cpp
    src/slab.cpp
    src/tls_cache.cpp
    src/global_heap.cpp
    src/os_memory.cpp
    src/debug_aid.cpp
)
target_compile_options(fast_alloc PRIVATE ${WARNING_FLAGS})
target_link_libraries(fast_alloc PUBLIC Threads::Threads)

target_include_directories(fast_alloc PUBLIC include src)

# ---------------------------------------------------------------------------
# Dependencies (GTest and GBenchmark)
# ---------------------------------------------------------------------------
include(FetchContent)

if(FASTALLOC_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(fast_alloc_tests
        tests/test_main.cpp
        tests/test_api.cpp
        tests/test_size_classes.cpp
        tests/test_large.cpp
        tests/test_debug_aids.cpp
        tests/test_concurrency.cpp
        tests/test_oom.cpp
        tests/test_aligned.cpp
        tests/test_stress.cpp
    )
    target_compile_options(fast_alloc_tests PRIVATE ${WARNING_FLAGS})
    target_link_libraries(fast_alloc_tests PRIVATE fast_alloc GTest::gtest_main)

    include(CTest)
    add_test(NAME fast_alloc_validate_tests COMMAND fast_alloc_tests)

    # Convenience targets for the common verification matrix
    add_custom_target(check
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        DEPENDS fast_alloc_tests
        COMMENT "Running FastAlloc test suite"
    )
endif()

if(FASTALLOC_BUILD_BENCHMARKS)
    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.8.3
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googlebenchmark)

    if(TARGET benchmark AND NOT MSVC)
        target_compile_options(benchmark PRIVATE -Wno-switch)
    endif()

    add_executable(fast_alloc_bench benchmarks/bench_main.cpp)
    target_compile_options(fast_alloc_bench PRIVATE ${WARNING_FLAGS})
    target_link_libraries(fast_alloc_bench PRIVATE fast_alloc benchmark::benchmark)

    add_executable(fast_alloc_bench_extended benchmarks/bench_extended.cpp)
    target_compile_options(fast_alloc_bench_extended PRIVATE ${WARNING_FLAGS})
    target_link_libraries(fast_alloc_bench_extended PRIVATE fast_alloc benchmark::benchmark)

    add_executable(fast_alloc_bench_memory benchmarks/bench_memory.cpp)
    target_compile_options(fast_alloc_bench_memory PRIVATE ${WARNING_FLAGS})
    target_link_libraries(fast_alloc_bench_memory PRIVATE fast_alloc)
    if(WIN32)
        # GetProcessMemoryInfo (psapi.h) resolves to Psapi.lib on some
        # MinGW/MSVC SDK configurations; harmless when already in kernel32.
        target_link_libraries(fast_alloc_bench_memory PRIVATE psapi)
    endif()

    # Rigorous cross-allocator benchmark suite (Larsson/mimalloc-bench style
    # workloads, per-allocator process isolation, JSONL output). No google
    # benchmark dependency on this one: deterministic op counts + explicit
    # rep/warmup control are the methodology here.
    add_executable(bench_suite benchmarks/benchsuite/bench_suite.cpp)
    target_compile_options(bench_suite PRIVATE ${WARNING_FLAGS})
    if(NOT WIN32)
        target_link_libraries(bench_suite PRIVATE fast_alloc ${CMAKE_DL_LIBS})
    else()
        # GetProcessMemoryInfo (psapi.h) resolves to Psapi.lib on some
        # MinGW/MSVC SDK configurations; harmless when already in kernel32.
        target_link_libraries(bench_suite PRIVATE fast_alloc psapi)
    endif()
    target_link_libraries(bench_suite PRIVATE Threads::Threads)

    target_link_libraries(fast_alloc_bench PRIVATE Threads::Threads)
    target_link_libraries(fast_alloc_bench_extended PRIVATE Threads::Threads)
    target_link_libraries(fast_alloc_bench_memory PRIVATE Threads::Threads)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT lto_supported)
if(lto_supported AND NOT FASTALLOC_SANITIZE)
    set_property(TARGET fast_alloc PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    if(FASTALLOC_BUILD_TESTS)
        # Death tests (fork) + LTO is fine, but keep tests non-LTO for
        # maximum debuggability of the instrumented paths.
        # set_property(TARGET fast_alloc_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
    if(FASTALLOC_BUILD_BENCHMARKS)
        set_property(TARGET fast_alloc_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET fast_alloc_bench_extended PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET fast_alloc_bench_memory PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET bench_suite PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

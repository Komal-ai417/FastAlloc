cmake_minimum_required(VERSION 3.15)
project(FastAlloc VERSION 1.1.0 LANGUAGES CXX)

# Enforce C++17 or higher
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Project options
option(FASTALLOC_BUILD_TESTS "Build unit tests" ON)
option(FASTALLOC_BUILD_BENCHMARKS "Build performance benchmarks" ON)
option(FASTALLOC_OVERRIDE_NEW "Override standard new/delete operators" OFF)

# Compiler-specific optimizations
if(MSVC)
    add_compile_options(/W4 /WX /Ot /Ox /GL)
    add_link_options(/LTCG)
else()
    # Aggressive optimizations for GCC/Clang
    add_compile_options(-Wall -Wextra -Werror -O3 -march=native -fno-plt)
endif()

if(FASTALLOC_OVERRIDE_NEW)
    add_definitions(-DFAST_ALLOC_OVERRIDE_NEW)
endif()

# Core Library
add_library(fast_alloc STATIC
    src/fast_alloc.cpp
    src/slab.cpp
    src/tls_cache.cpp
    src/global_heap.cpp
    src/os_memory.cpp
)

target_include_directories(fast_alloc PUBLIC include src)

# Dependencies (GTest and GBenchmark)
include(FetchContent)

if(FASTALLOC_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(fast_alloc_tests tests/test_main.cpp)
    target_link_libraries(fast_alloc_tests PRIVATE fast_alloc GTest::gtest_main)
    
    include(CTest)
    add_test(NAME fast_alloc_validate_tests COMMAND fast_alloc_tests)
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

    add_executable(fast_alloc_bench benchmarks/bench_main.cpp)
    target_link_libraries(fast_alloc_bench PRIVATE fast_alloc benchmark::benchmark)
    
    add_executable(fast_alloc_bench_extended benchmarks/bench_extended.cpp)
    target_link_libraries(fast_alloc_bench_extended PRIVATE fast_alloc benchmark::benchmark)

    add_executable(fast_alloc_bench_memory benchmarks/bench_memory.cpp)
    target_link_libraries(fast_alloc_bench_memory PRIVATE fast_alloc)
    
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(fast_alloc_bench PRIVATE Threads::Threads)
    target_link_libraries(fast_alloc_bench_extended PRIVATE Threads::Threads)
    target_link_libraries(fast_alloc_bench_memory PRIVATE Threads::Threads)
endif()

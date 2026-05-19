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
    set(WARNING_FLAGS /W4 /WX)
    add_compile_options("$<$<CONFIG:Release>:/Ot>" "$<$<CONFIG:Release>:/Ox>" "$<$<CONFIG:Release>:/GL>")
    add_link_options("$<$<CONFIG:Release>:/LTCG>")
else()
    # Aggressive optimizations for GCC/Clang
    set(WARNING_FLAGS -Wall -Wextra -Werror)
    add_compile_options("$<$<CONFIG:Release>:-O3>" "$<$<CONFIG:Release>:-march=native>" "$<$<CONFIG:Release>:-fno-plt>")
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
target_compile_options(fast_alloc PRIVATE ${WARNING_FLAGS})

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
    target_compile_options(fast_alloc_tests PRIVATE ${WARNING_FLAGS})
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
    
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(fast_alloc_bench PRIVATE Threads::Threads)
    target_link_libraries(fast_alloc_bench_extended PRIVATE Threads::Threads)
    target_link_libraries(fast_alloc_bench_memory PRIVATE Threads::Threads)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT lto_supported)
if(lto_supported)
    set_property(TARGET fast_alloc PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    if(FASTALLOC_BUILD_TESTS)
        set_property(TARGET fast_alloc_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
    if(FASTALLOC_BUILD_BENCHMARKS)
        set_property(TARGET fast_alloc_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET fast_alloc_bench_extended PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET fast_alloc_bench_memory PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

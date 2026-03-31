cmake_minimum_required(VERSION 3.15)
project(FastAlloc VERSION 1.0.0 LANGUAGES CXX)

# Enforce C++17 or higher
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Project options
option(FASTALLOC_BUILD_TESTS "Build tests" ON)
option(FASTALLOC_BUILD_BENCHMARKS "Build benchmarks" ON)

# Includes and target definitions
add_library(fast_alloc STATIC
    src/fast_alloc.cpp
    src/os_memory.cpp
    src/slab.cpp
    src/tls_cache.cpp
    src/global_heap.cpp
)

target_include_directories(fast_alloc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    PRIVATE src
)

# Compiler warnings
if(MSVC)
    target_compile_options(fast_alloc PRIVATE /W4 /WX)
else()
    target_compile_options(fast_alloc PRIVATE -Wall -Wextra -Werror)
endif()

# Fetch dependencies (GTest and GBench)
include(FetchContent)

if(FASTALLOC_BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG release-1.12.1
    )
    # For Windows: Prevent overriding our compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(fast_alloc_tests tests/test_main.cpp)
    target_link_libraries(fast_alloc_tests PRIVATE fast_alloc gtest_main)
    # Windows threading is automatic, Linux needs pthread
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(fast_alloc_tests PRIVATE Threads::Threads)
    
    add_test(NAME fast_alloc_validate_tests COMMAND fast_alloc_tests)
endif()

if(FASTALLOC_BUILD_BENCHMARKS)
    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.8.3
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googlebenchmark)

    add_executable(fast_alloc_bench benchmarks/bench_main.cpp)
    target_link_libraries(fast_alloc_bench PRIVATE fast_alloc benchmark::benchmark)
    
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(fast_alloc_bench PRIVATE Threads::Threads)
endif()

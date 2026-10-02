#pragma once
// ============================================================================
// Test-only hooks into FastAlloc internals.
// Definitions live in the library (global_heap.cpp); they are compiled into
// every build and cost nothing unless a test calls them.
// ============================================================================
#include <cstddef>

// ----------------------------------------------------------------------------
// Compile-time ThreadSanitizer detection (no-op = 0 everywhere else).
// GCC defines __SANITIZE_THREAD__ when -fsanitize=thread is active; clang
// needs __has_feature(thread_sanitizer). MSVC has no TSan (its ASan job
// covers the Windows side), so the macro is 0 there.
//
// The audit suite uses this to keep its synchronization scaffolding friendly
// to TSan's 10-40x instrumentation overhead — NOT to shrink the tests: the
// race-detection value of a 128-thread sweep is highest exactly when TSan
// is watching, so the workload itself stays full-size.
// ----------------------------------------------------------------------------
#if defined(__SANITIZE_THREAD__)
#define FASTALLOC_TEST_TSAN 1
#elif defined(__clang__) && defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define FASTALLOC_TEST_TSAN 1
#endif
#endif
#if !defined(FASTALLOC_TEST_TSAN)
#define FASTALLOC_TEST_TSAN 0
#endif

namespace FastAlloc {
// OOM injection: the next 'n' OS page allocations fail (simulates mmap /
// VirtualAlloc exhaustion on the slow path only - the fast path is unaffected).
void FastAllocTestSetOOMCountdown(int n);
int  FastAllocTestGetOOMCountdown();

// Global page-span cache introspection & control.
std::size_t FastAllocTestPageCacheBytes();      // bytes currently cached
std::size_t FastAllocTestPurgePageCache();      // drop everything, return bytes freed
} // namespace FastAlloc

using namespace FastAlloc;

#pragma once
// ============================================================================
// Test-only hooks into FastAlloc internals.
// Definitions live in the library (global_heap.cpp); they are compiled into
// every build and cost nothing unless a test calls them.
// ============================================================================
#include <cstddef>

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

// ===========================================================================
// TSan bridge for kernel-mediated virtual-address handoffs
// ===========================================================================
// FastAlloc hands address ranges back to the kernel (munmap, mremap move,
// VirtualFree) and receives fresh ones (mmap, mremap grow, VirtualAlloc).
// When a range freed by thread A is re-issued by the kernel to thread B,
// the only ordering between A's last accesses and B's first accesses is
// inside the kernel - invisible to ThreadSanitizer, which then reports
// false data races of the shape "A read a block header at VA X" vs
// "B wrote a block header at VA X" (e.g. fast_realloc's plausibility read
// vs the post-mremap alloc_size store).
//
// The bridge publishes a release edge on every VA return and an acquire
// edge on every VA arrival. Chained through the kernel handoff this
// reconstructs exactly the ordering the kernel guarantees:
//
//   A: read header ... free ... [Release] munmap(X)
//                                       kernel re-issues X
//   B: mmap/mremap(X) [Acquire] ... write header
//
// and nothing more: a block a thread still OWNS never passes through
// Release, so genuine same-block races keep firing. All edges compile to
// nothing outside instrumented builds.
#pragma once

#if defined(__SANITIZE_THREAD__) || \
    (defined(__has_feature) && __has_feature(thread_sanitizer))
extern "C" {
void __tsan_acquire(void volatile* addr);
void __tsan_release(void volatile* addr);
}
namespace FastAlloc::tsan_bridge {
// One token for the whole process: function-local static in an inline
// function is a single entity across TUs (ODR), unlike a const namespace-
// scope variable, which would give every TU a private token and silently
// break the release/acquire pairing.
inline char* VaHandoffToken() {
    static char token = 0;
    return &token;
}
inline void ReleaseOnVaReturn() { __tsan_release(VaHandoffToken()); }
inline void AcquireOnVaArrival() { __tsan_acquire(VaHandoffToken()); }
}
#else
namespace FastAlloc::tsan_bridge {
inline void ReleaseOnVaReturn() {}
inline void AcquireOnVaArrival() {}
}
#endif

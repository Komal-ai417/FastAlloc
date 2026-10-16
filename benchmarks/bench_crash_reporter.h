#pragma once
// ============================================================================
// bench_crash_reporter.h — crash forensics for benchmark executables.
//
// Installs signal handlers (Linux) or unhandled exception filters (Windows)
// to output diagnostic reports on unexpected faults.
//
// Features:
//   * Prints faulting address, signal code, and stack frames to stderr.
//   * Trap-region registry: supports registering memory regions to catch and
//     diagnose unauthorized writes.
//
// Design notes:
//   * Header-only; initializes before main().
//   * Disabled when building with sanitizers.
//   * Linux path uses async-signal-safe system calls where possible.
//   * Windows path uses SetUnhandledExceptionFilter.
// ============================================================================
#if defined(_WIN32) && !defined(FASTALLOC_BENCH_NO_CRASH_REPORTER)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#elif defined(__linux__) && !defined(FASTALLOC_BENCH_NO_CRASH_REPORTER)
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <unistd.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <execinfo.h>
#include <cstdio>
#endif

// Sanitizer builds: their handlers are installed first and are strictly more
// informative; do not race them for SIGSEGV.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    defined(__SANITIZE_UNDEFINED__) || defined(FASTALLOC_BENCH_NO_CRASH_REPORTER)
#define FASTALLOC_BENCH_CRASH_REPORTER_OFF 1
#endif

namespace fastalloc_bench {
namespace crash_reporter {

#if defined(__linux__) && !defined(FASTALLOC_BENCH_CRASH_REPORTER_OFF)

// ---------------------------------------------------------------------------
// Trap-region registry (write-trap healing). Each entry is a frozen
// GuardedSlots data page [lo, hi). Registration order (lo then hi) and
// deregistration order (hi then lo) ensure thread-safe lock-free reads.
// ---------------------------------------------------------------------------
inline void* volatile g_trap_lo[8] = {};
inline void* volatile g_trap_hi[8] = {};
inline std::atomic<std::uint64_t> g_trap_heals{0};

inline void RegisterTrapRegion(const void* lo, const void* hi) {
    for (int i = 0; i < 8; ++i) {
        if (g_trap_lo[i] == nullptr && g_trap_hi[i] == nullptr) {
            g_trap_lo[i] = const_cast<void*>(lo);   // publish lo first
            __atomic_thread_fence(__ATOMIC_RELEASE);
            g_trap_hi[i] = const_cast<void*>(hi);   // entry live
            return;
        }
    }
}

inline void UnregisterTrapRegion(const void* lo) {
    for (int i = 0; i < 8; ++i) {
        if (g_trap_lo[i] == lo) {
            g_trap_hi[i] = nullptr;                 // deaden first
            __atomic_thread_fence(__ATOMIC_RELEASE);
            g_trap_lo[i] = nullptr;
            return;
        }
    }
}

// Fault classification: is this fault the write-trap firing?
inline int TrapRegionIndex(const void* addr) {
    for (int i = 0; i < 8; ++i) {
        const void* lo = g_trap_lo[i];
        const void* hi = g_trap_hi[i];
        if (lo && hi && addr >= lo && addr < hi) return i;
    }
    return -1;
}

inline const char* SegvCodeName(int code) {
    switch (code) {
    case 1:  return "SEGV_MAPERR (unmapped)";
    case 2:  return "SEGV_ACCERR (permission: write to frozen page)";
    case 128:return "SI_KERNEL (#GP: NON-CANONICAL access, addr masked to 0)";
    default: return "other";
    }
}

inline void ReportAndDie(int sig, siginfo_t* info, void* uc) {
    // Include si_code in diagnostic output to distinguish between invalid
    // mappings, access permission faults, and general protection violations.
    void* frames[48];
    int n = 0;
    // If the ucontext is available, seed the backtrace with the faulting
    // instruction so the printed frames start at the crashing RIP.
    if (uc) {
        ucontext_t* ctx = static_cast<ucontext_t*>(uc);
#ifdef REG_RIP
        frames[n++] = reinterpret_cast<void*>(ctx->uc_mcontext.gregs[REG_RIP]);
#else
        (void)ctx;
#endif
    }
    n += backtrace(frames + n, 48 - n);
    char msg[224];
    int len = std::snprintf(msg, sizeof(msg),
                            "\n[bench-crash] caught signal %d, faulting address %p, "
                            "si_code=%d (%s). Raw backtrace:\n",
                            sig, info ? info->si_addr : static_cast<void*>(nullptr),
                            info ? info->si_code : -1,
                            info ? SegvCodeName(info->si_code) : "n/a");
    if (len > 0) {
        ssize_t ignored = write(STDERR_FILENO, msg, static_cast<size_t>(len));
        (void)ignored;
    }
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    static const char kTail[] =
        "[bench-crash] symbolize offline with: addr2line -e <binary> -f -C <frame>\n";
    ssize_t ignored2 = write(STDERR_FILENO, kTail, sizeof(kTail) - 1);
    (void)ignored2;
    std::_Exit(128 + sig);
}

// When a write fault occurs inside a registered trap region, log diagnostic
// information (faulting address, si_code, writer RIP) and reprotect the page
// with write permissions so execution can resume.
inline void TrapFaultOrDie(int sig, siginfo_t* info, void* uc) {
    const void* addr = info ? info->si_addr : nullptr;
    int region = TrapRegionIndex(addr);
    if (region >= 0) {
        // 4 KiB alignment: assumes standard 4 KiB pages. On systems with
        // larger page sizes, mprotect fails safely and falls through to the fatal report.
        const std::uintptr_t page =
            reinterpret_cast<std::uintptr_t>(addr) & ~static_cast<std::uintptr_t>(0xFFFull);
        if (mprotect(reinterpret_cast<void*>(page), 0x1000, PROT_READ | PROT_WRITE) == 0) {
            std::uint64_t n = g_trap_heals.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 16 || (n % 256) == 0) {
                void* rip = nullptr;
                if (uc) {
#ifdef REG_RIP
                    ucontext_t* ctx = static_cast<ucontext_t*>(uc);
                    rip = reinterpret_cast<void*>(ctx->uc_mcontext.gregs[REG_RIP]);
#else
                    (void)uc;
#endif
                }
                void* frames[8];
                int nf = 0;
                if (rip) frames[nf++] = rip;
                nf += backtrace(frames + nf, 8 - nf);
                char msg[192];
                int len = std::snprintf(msg, sizeof(msg),
                                        "[bench-trap] write-trap fault #%llu at %p "
                                        "si_code=%d; writer RIP=%p (see backtrace; "
                                        "page healed, run continues)\n",
                                        (unsigned long long)n, addr,
                                        info ? info->si_code : -1, rip);
                if (len > 0) {
                    ssize_t ignored = write(STDERR_FILENO, msg, static_cast<size_t>(len));
                    (void)ignored;
                }
                backtrace_symbols_fd(frames, nf, STDERR_FILENO);
            }
            return; // retry the faulting store — it now completes
        }
    }
    ReportAndDie(sig, info, uc);
}

inline bool Install() {
    struct sigaction sa;
    sa.sa_sigaction = &TrapFaultOrDie;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    bool ok = sigaction(SIGSEGV, &sa, nullptr) == 0;
    ok = (sigaction(SIGBUS, &sa, nullptr) == 0) && ok;
    return ok;
}

#elif defined(_WIN32) && !defined(FASTALLOC_BENCH_CRASH_REPORTER_OFF)

inline LONG WINAPI ReportAndDie(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord) {
        std::fprintf(stderr,
                     "\n[bench-crash] unhandled exception 0x%08lX at address %p\n",
                     ep->ExceptionRecord->ExceptionCode,
                     ep->ExceptionRecord->ExceptionAddress);
    } else {
        std::fprintf(stderr, "\n[bench-crash] unhandled exception (no record)\n");
    }
    std::fflush(stderr);
    std::exit(3);
}

// Trap-region registry stubs: the write-trap healing is Linux-only; on
// Windows the guard pages still fault fatally with a full report.
inline void RegisterTrapRegion(const void*, const void*) {}
inline void UnregisterTrapRegion(const void*) {}

inline bool Install() {
    return SetUnhandledExceptionFilter(&ReportAndDie) != nullptr;
}

#else

inline void RegisterTrapRegion(const void*, const void*) {}
inline void UnregisterTrapRegion(const void*) {}
inline bool Install() { return false; } // sanitized or opted out: nothing to do

#endif

} // namespace crash_reporter

// Installed before main(); the value is never read (side-effect-only init).
[[maybe_unused]] const bool kCrashReporterInstalled = crash_reporter::Install();

} // namespace fastalloc_bench

#pragma once
// ============================================================================
// bench_crash_reporter.h — in-binary crash forensics for the google-benchmark
// executables (fast_alloc_bench, fast_alloc_bench_extended).
//
// WHY: the Sep-2026 benchmarks-ubuntu-latest failure (SIGSEGV rc=139 in the
// BM_MallocFree_FastAlloc family, twice per attempt) reproduces only on the
// CI runner: the identical binary/flags pass 25+ local runs (release, TSan,
// ASan, CPU-hog preemption pressure, per-size isolated reruns) and a full
// static audit of the cross-thread return path found no defect. The runner's
// gdb forensics step already captures a backtrace into an artifact, but the
// raw frames were not visible in the failure summary the maintainers see.
// This header installs a signal handler at process start so the FIRST crash
// — before any gdb rerun — prints the faulting address and a raw backtrace
// straight into the job log. It changes no allocator or benchmark logic.
//
// v11 additions (Sep-2026 runner crash, closing round):
//   * si_code is now printed with the faulting address. The v10 crash's
//     signature was si_code=SI_KERNEL(128) + si_addr=0 ("faulting address
//     (nil)") = a NON-CANONICAL access: the CPU raised #GP, not a page
//     fault. Printing si_code distinguishes that class from ordinary
//     SEGV_MAPERR/SEGV_ACCERR page faults at a glance.
//   * Trap-region registry: GuardedSlots (bench_main.cpp) registers its
//     frozen data page here. A write fault INSIDE a registered region is
//     the write-trap catching a wild writer red-handed: the handler prints
//     the faulting address, si_code AND the writer's own instruction
//     pointer (RIP from ucontext, symbolized via the backtrace machinery),
//     then HEALS the fault (mprotect RW + retry) so the run survives and
//     the benchmark completes. Faults outside registered regions keep the
//     fatal report-and-exit behavior.
//
// Design notes:
//   * Header-only; the installer runs as an inline-variable initializer
//     before main(), so BENCHMARK_MAIN() needs no changes.
//   * Not installed under sanitizers (they own the signal handlers and give
//     better reports).
//   * Linux path uses only write(2)/mprotect(2)/backtrace(3)/
//     backtrace_symbols_fd(3) (glibc documents these as async-signal-safe
//     enough for crash reporting); output is intentionally raw frames —
//     symbolize offline with: addr2line -e <binary> -f -C <frame>.
//   * Windows path is a SetUnhandledExceptionFilter that reports the
//     exception code and faulting address (no dbghelp dependency).
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
// v11 trap-region registry (write-trap healing). Each entry is a frozen
// GuardedSlots data page [lo, hi). Registration order (lo then hi) and
// deregistration order (hi then lo) make a torn concurrent read
// non-member-safe: the handler treats a half-published or half-removed
// region as absent. mprotect failure (page already unmapped) falls back to
// the fatal path, so a race at teardown can never spin.
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
    // v11: include si_code — the non-canonical/#GP class (si_code=128,
    // si_addr=0, "faulting address (nil)") is distinguishable at a glance
    // from ordinary page faults.
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

// v11: a fault inside a registered trap region = the write-trap caught a
// writer. Print (rate-limited) the faulting address, si_code and the
// WRITER'S OWN RIP, then heal the page so the write completes and the run
// continues. If healing fails (page gone), fall through to the fatal path.
inline void TrapFaultOrDie(int sig, siginfo_t* info, void* uc) {
    const void* addr = info ? info->si_addr : nullptr;
    int region = TrapRegionIndex(addr);
    if (region >= 0) {
        // 4 KiB alignment: the CI runners are x86-64 (4 KiB pages). On a
        // 64 KiB-page host the mprotect simply fails and we fall through
        // to the fatal report — never an unbounded fault loop.
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

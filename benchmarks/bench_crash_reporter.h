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
// Design notes:
//   * Header-only; the installer runs as an inline-variable initializer
//     before main(), so BENCHMARK_MAIN() needs no changes.
//   * Not installed under sanitizers (they own the signal handlers and give
//     better reports).
//   * Linux path uses only write(2)/backtrace(3)/backtrace_symbols_fd(3)
//     (glibc documents these as async-signal-safe enough for crash
//     reporting); output is intentionally raw frames — symbolize offline
//     with: addr2line -e <binary> -f -C <frame>.
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
#include <cstdlib>
#include <unistd.h>
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

inline void ReportAndDie(int sig, siginfo_t* info, void*) {
    void* frames[48];
    int n = backtrace(frames, 48);
    char msg[160];
    int len = std::snprintf(msg, sizeof(msg),
                            "\n[bench-crash] caught signal %d, faulting address %p. Raw backtrace:\n",
                            sig, info ? info->si_addr : static_cast<void*>(nullptr));
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

inline bool Install() {
    struct sigaction sa;
    sa.sa_sigaction = &ReportAndDie;
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

inline bool Install() {
    return SetUnhandledExceptionFilter(&ReportAndDie) != nullptr;
}

#else

inline bool Install() { return false; } // sanitized or opted out: nothing to do

#endif

} // namespace crash_reporter

// Installed before main(); the value is never read (side-effect-only init).
[[maybe_unused]] const bool kCrashReporterInstalled = crash_reporter::Install();

} // namespace fastalloc_bench

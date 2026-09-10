# Quality Assurance & Memory Safety Report
**Project:** FastAlloc — v2.0.0

> This report describes the test infrastructure that is **actually in this
> repository** and the results **actually reproduced with it** (including a
> full re-verification session on the reference machine, see §3). The previous
> version of this document claimed runtime assertions, double-free tracking,
> sanitizer runs and coverage numbers that did not exist in the code; that
> content has been replaced by evidence-backed reporting (see "History").

## 1. Test Plan Coverage

The Google Test suite (`fast_alloc_tests`) consists of **10 files, 86 test
cases** (release: 75 run; debug: 86 run, of which 85 pass and 1 —
`ApiTest.FreeWithCorruptLargeHeaderIsDroppedNotCached` — intentionally
skips in debug builds because the debug registry fatals on forged pointers
by design; it runs in release builds where the drop-not-cache path is
exercised).

| File | Focus | Key tests |
| :--- | :--- | :--- |
| `test_main.cpp` | Legacy smoke suite | BasicAllocation, LargeAllocation, MultipleAllocations, ReallocAndCalloc, MultiThreading (assertion-safe version) |
| `test_api.cpp` | Public API contract | zero-size semantics, null handling, alignment for 40+ sizes, every-byte-writable, unique pointers, `fast_free_sized`, realloc failure keeps original, calloc overflow guard, stats, corrupt-header drop-not-cache (release-only) |
| `test_size_classes.cpp` | Class boundaries | **every size 1..512** plus high boundaries to 8192; threshold exactly 8176; multi-slab classes; mixed interleaving; realloc across classes and small↔large |
| `test_large.cpp` | Large path & cache | cache reuse (exact pointer identity), in-place realloc growth within page slack, in-place shrink via `mremap`, page-cache purge, 64 MB mapping, overflow guards |
| `test_debug_aids.cpp` | Detection features | double-free (small+large), invalid free of a stack pointer, 1-byte buffer **overflow**, 1-byte buffer **underflow**, large-header underflow, use-after-free write on reuse, leak registry accuracy, custom violation-handler hook — as death tests |
| `test_concurrency.cpp` | Cross-thread behaviour | **producer/consumer cross-thread free** (exercises the lock-free MPSC `pending_returns_` handoff), thread-exit flush without user frees, 8-thread contended churn, cross-thread realloc, pending-push instrumentation |
| `test_oom.cpp` | Memory exhaustion | deterministic OOM injection: clean `nullptr` returns, existing memory stays valid, allocator fully recovers, countdown semantics, TLS fast path unaffected |
| `test_aligned.cpp` | Over-aligned allocations | alignments 32..4096, odd sizes, large aligned, realloc preserves alignment, invalid alignments rejected |
| `test_stress.cpp` | Randomized soak | 5 pattern-verified churns (every byte of every block verified before free), multi-threaded soak, reverse and round-robin free orders |

Assertions are never executed from worker threads: failures are recorded
per-thread and verified on the main thread (Google Test assertions are not
thread-safe — the original suite violated this).

## 2. Instrumentation (FASTALLOC_DEBUG builds)

| Feature | Status | Verified by |
| :--- | :--- | :--- |
| Front canary (4 B in `FreeBlock` padding) | implemented | `BufferUnderflowFrontCanaryIsDetected` |
| Rear red zone (request-end → block-end, never empty: +8 B debug reserve) | implemented | `BufferOverflowRearCanaryIsDetected` |
| Large-header magic + size-field validation | implemented | `LargeBlockUnderflowDestroysHeader` |
| Slab magic validation | implemented | every free of every small block |
| Allocation registry (ptr → size/class/thread/seq) | implemented | `LeakRegistryTracksLiveBlocks` |
| Double-free / invalid-free detection | implemented | 3 death tests |
| Free poisoning (0xDD) + fresh fill (0xCD) → UAF-write detection | implemented | `UseAfterFreeWriteIsDetectedOnReuse` |
| Leak report (`fast_alloc_dump_leaks()`) | implemented | `DumpLeaksDoesNotCrash`, registry tests |
| `Slab::Allocate/Deallocate` runtime invariants (the previously *claimed* asserts) | implemented | compile-time presence + all suites |
| Statistics (`fast_alloc_stats()`, TLS-batched, no hot-path atomics) | implemented | `StatsBasics`, `StatsReportPeak` |
| Level-gated logging (`FASTALLOC_LOG_LEVEL` env / `fast_alloc_log_set_level`) | implemented | `LogLevelSetGet` |
| Custom violation-handler hook for embedders | implemented | `ViolationHandlerHook` |

Release builds compile all of the above out; the only release-mode additions
to the hot path are one predictable branch (`ALIGN_MAGIC` discrimination) and
thread-local counter increments flushed every 256 ops.

## 3. Sanitizer & Tool Results (re-verified 2026-09-10)

All rows below were re-run in one session on the reference machine
(2 vCPU Intel Xeon VM, Debian 13, GCC 14.2, CMake 4.4 + Ninja) and are
reproducible via the commands in §5. CI runs the same battery on every push
across Linux and Windows (see §4).

| Configuration | Suite | Result | Wall time |
| :--- | :--- | :--- | ---: |
| Release (`-O3 -march=native` + LTO, `-Wall -Wextra -Werror`) | 75 tests | **PASS** — zero warnings | 5.5 s |
| Debug (`-g -DFASTALLOC_DEBUG`) | 86 tests | **PASS** (85 run + 1 by-design skip; all detections verified) | 114 s |
| AddressSanitizer + UBSan (release) | 75 tests | **PASS** — zero memory errors | 45 s |
| ThreadSanitizer | 75 tests | **PASS — zero data races** across the cross-thread MPSC handoff, thread-exit flush, and 8-thread contended churn tests | 90 s |
| LeakSanitizer (release) | 75 tests | **PASS** — zero leaks | 4.7 s |
| LeakSanitizer-equivalent (registry) | via debug suite | leak counts exact in debug mode | — |

Benchmark correctness was additionally re-verified in the same session by
`bench_compare.py`: all 132 cross-allocator cells and 92 in-process gbench
configs completed with **zero checksum failures** (every block is
tagged on alloc and verified on free).

## 4. Continuous Integration (all green)

`.github/workflows/ci.yml` runs the following matrix on every push (the
full suite also runs under the `windows-msvc-asan` job with the ASan runtime
DLL staged next to each executable — the v12 fix for the 0xC0000135
launch failure — repeated up to 3x with `-V`):

| Job | What it runs |
| :--- | :--- |
| build-test (7 jobs) | ubuntu/windows × release/debug + ubuntu-gcc-release, ubuntu-clang-release, ubuntu-clang-debug — full ctest suite |
| windows-msvc-asan | full suite under MSVC ASan, `--repeat until-fail:3`, cdb post-mortem on failure |
| sanitizer-{address-undefined, thread} | full suite under ASan+UBSan / TSan |
| leak-sanitizer | full suite under LSan |
| windows-va-emulation-{release,debug} | Linux emulation of the Windows virtual-address accounting paths |
| benchmarks-{ubuntu, windows} (2 jobs) | every benchmark binary executed family-isolated: fast_alloc_bench (all), fast_alloc_bench_extended (all), fast_alloc_bench_memory (T×size sweep), bench_suite (11 workloads × std+fast × T{1,2,4}), checksum-gated |
| benchmark-matrix (pushes to main, manual) | cross-allocator glibc/jemalloc/FastAlloc matrix with `run_matrix.sh` + `analyze.py` |

## 5. How to Reproduce

```bash
# Release + tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFASTALLOC_BUILD_TESTS=ON
cmake --build build -j && ctest --test-dir build --output-on-failure

# Debug-instrumented (canaries, registry, death tests)
cmake -S . -B build-dbg -DCMAKE_BUILD_TYPE=Debug -DFASTALLOC_DEBUG=ON
cmake --build build-dbg -j && ctest --test-dir build-dbg --output-on-failure

# Sanitizers
cmake -S . -B build-asan -DFASTALLOC_SANITIZE=address,undefined
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure
cmake -S . -B build-tsan -DFASTALLOC_SANITIZE=thread
cmake --build build-tsan -j && ctest --test-dir build-tsan --output-on-failure
cmake -S . -B build-lsan -DFASTALLOC_SANITIZE=leak
cmake --build build-lsan -j && ctest --test-dir build-lsan --output-on-failure
```

Tip for memory-constrained machines (< 8 GB RAM): TSan maps ~8x shadow
memory. Bound the allocator's page-span retention while testing:

```bash
FASTALLOC_PAGE_CACHE_MB=64 ctest --test-dir build-tsan --output-on-failure
```

## 6. Performance Snapshot (reference machine, single thread, 500 alloc/free per iteration)

| Size | std::malloc | FastAlloc v2 | Ratio |
| :--- | :--- | :--- | :--- |
| 8 B | 9.60 µs | 4.76 µs | **2.0× faster** |
| 64 B | 13.96 µs | 5.34 µs | **2.6× faster** |
| 512 B | 53.15 µs | 5.04 µs | **10.5× faster** |

Reproduce with `fast_alloc_bench --benchmark_min_time=0.5s` (Google
Benchmark), the side-by-side `fast_alloc_bench_memory` (this session: 9/9
configs 1.33x–2.22x faster), or the full cross-allocator verdict tool:

```bash
python3 benchmarks/benchsuite/bench_compare.py --reps 5
```

Numbers vary by hardware, core count and page-fault behaviour — measure on
your target before quoting figures (see
[performance_report.md](performance_report.md) §"Session-to-session
variance").

## 7. History

The original `qa_report.md` asserted asserts-in-slab, double-free tracking,
ASan/MSan/LSan/Valgrind cleanliness and 92–100 % coverage while the code
contained none of it. v2.0.0 implements the features, verifies them with
death tests and sanitizers, and limits every claim in this document to what
the repository can reproduce.

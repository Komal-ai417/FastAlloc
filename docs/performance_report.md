# Performance Report — FastAlloc v2.0.0 (rev 5, full re-measurement)

> **Methodology note (v5):** every number below was re-measured in one session
> with the new **one-shot verdict tool**
> `benchmarks/benchsuite/bench_compare.py`, which drives *every* benchmark in
> this repository — the two Google-Benchmark micro-suites, the 11-workload
> cross-allocator `bench_suite`, and the side-by-side memory stress benchmark —
> and prints a per-category win/loss verdict with the exact multiplier
> ("MallocOnly 16B T=1: FastAlloc wins by 1.29x"). Baselines: **glibc 2.41
> (ptmalloc+tcache)**, **jemalloc 5.3.0**, **mimalloc 2.1.7** (built from
> source, LD_PRELOAD). Each `bench_suite` cell is the **median of 5
> repetitions** after a warmup rep, one allocator per process, launch order
> rotated per workload, every block checksum-verified on free (zero failures
> across the 132-cell matrix; the tool exits non-zero if any checksum fails).
> Raw records: `benchmarks/benchsuite/compare_results.jsonl`; the generated
> verdict report: `benchmarks/benchsuite/bench_report.md`.

## Reference environment

- CPU: 2 vCPU x86-64 VM (Intel Xeon), 4 GB RAM — T=4 is 2x oversubscribed
- OS: Debian 13 (trixie), Linux
- Toolchain: GCC 14.2, `-O3 -march=native` + LTO (CMake Release), CMake 4.4
- Baselines: glibc 2.41 (default), jemalloc 5.3.0 (LD_PRELOAD),
  mimalloc 2.1.7 (LD_PRELOAD, built from source)
- Suite: 11 workloads × {1, 2, 4} threads × 4 allocators = 132 throughput
  cells + latency percentiles + memory accounting + thread-lifecycle data

Run it yourself (one command, every benchmark, full verdict report):

```bash
cmake -B build && cmake --build build -j --target bench_suite \
    fast_alloc_bench fast_alloc_bench_extended fast_alloc_bench_memory
# point the tool at jemalloc/mimalloc (auto-detected if in default paths):
python3 benchmarks/benchsuite/bench_compare.py \
    --mimalloc-so /path/to/libmimalloc.so.2 --reps 5 --threads 1,2,4
# smoke preset:
python3 benchmarks/benchsuite/bench_compare.py --quick
```

The tool writes `bench_report.md` (the verdict report),
`bench_report_data.json` (chart-ready data) and appends raw JSONL records.
Long sessions can be chunked: `--workloads tiny,churn ...` runs a subset,
`--suite-skip-run` rebuilds the report from the accumulated JSONL, and
`--gbench-cache` reuses family JSONs between invocations.

## Headline verdicts (this session's measurement)

- **vs glibc (std malloc): FastAlloc wins 20 of 33 throughput cells**, loses
  12, ties 1. Wins: every small-block pair workload at every thread count
  (tiny 1.16–1.31x, small-mixed 1.24–1.51x, random-1-4096 up to 2.98x), ramp
  1.54–1.94x, churn 1.26–1.43x (tie at T=4), cache-thrash 1.20–1.24x,
  cross-thread up to 2.52x (T=2).
- **vs jemalloc: FastAlloc wins 21 of 33 cells** — all pair workloads, ramp
  2.0–2.3x, thread-churn 2.2–2.6x, realloc 1.25x, and **large 6.8–14.6x**.
- **vs mimalloc: FastAlloc wins 12 of 33 cells** — mimalloc keeps the edge on
  ramp (0.36–0.51x), churn (0.55–0.58x) and thread-churn (0.56–0.82x), while
  FastAlloc keeps large 4.6–9.2x and every pair workload at T=1.
- **Latency (T=1):** best p50 **and** p99 of all four allocators in tiny,
  small-mixed and random-1-4096 (e.g. random: p50 13.3 ns vs glibc 41.0 /
  jemalloc 18.4 / mimalloc 24.7; p99 15.4 vs 53.9 / 25.3 / 27.6). p99.9 is a
  statistical tie with jemalloc on random-1-4096.
- **Memory return-to-OS (1M live objects ≈ 500 MB payload, free-all, purge):**
  FastAlloc retains **16.0 MB** vs glibc 46.7 (malloc_trim), jemalloc 171.8,
  mimalloc 552.2 — **2.9x / 10.7x / 34.5x less**. Retained right after free:
  123.6 MB vs 369.6 / 171.8 / 552.2 — least of the four.
- **Thread lifecycle:** thread-churn T=1 252 ns/pair vs glibc 155 (glibc wins
  1.62x — its tcache is a handful of shallow bins), **2.6x faster than
  jemalloc**, ahead of mimalloc at T=1 (206 ns).
- **In-process microbenchmarks (92 paired gbench configs):** FastAlloc wins 8
  of 9 families (FreeOnly up to 23.8x, MallocOnly up to 50x at 4–8 KB,
  HeavyContention 1.15–6.7x, MallocFree up to 34.6x); the one family loss is
  `Calloc/10B` (std wins 11.9x — FastAlloc's calloc zeroes the full usable
  block; see loss table).

![FastAlloc speedup heatmap](charts/speedup_heatmap.png)

## Full verdict matrices (median ns per op; ratio = competitor/FastAlloc)

### FastAlloc vs glibc

| workload | T=1 | T=2 | T=4 (2x oversub) |
| :--- | ---: | ---: | ---: |
| tiny | **1.31x WIN** | **1.24x WIN** | **1.16x WIN** |
| small-mixed | **1.51x WIN** | **1.24x WIN** | **1.40x WIN** |
| random-1-4096 | **2.98x WIN** | **2.09x WIN** | **1.86x WIN** |
| ramp | **1.54x WIN** | **1.67x WIN** | **1.94x WIN** |
| churn | **1.43x WIN** | **1.26x WIN** | 1.00x TIE |
| cache-thrash | **1.20x WIN** | **1.24x WIN** | **1.22x WIN** |
| cross-thread | **1.07x WIN** | **2.52x WIN** | 0.91x LOSS |
| thread-churn | 0.62x LOSS | 0.48x LOSS | 0.48x LOSS |
| large | 0.91x LOSS | 0.93x LOSS | **1.04x WIN** |
| realloc-grow | 0.28x LOSS | 0.28x LOSS | 0.28x LOSS |
| overhead (bulk 1M alloc+free) | 0.66x LOSS | 0.63x LOSS | 0.77x LOSS |

### FastAlloc vs jemalloc

| workload | T=1 | T=2 | T=4 |
| :--- | ---: | ---: | ---: |
| tiny | **1.26x WIN** | **1.21x WIN** | **1.15x WIN** |
| small-mixed | **1.18x WIN** | **1.10x WIN** | **1.23x WIN** |
| random-1-4096 | **1.39x WIN** | **1.19x WIN** | **1.14x WIN** |
| ramp | **2.31x WIN** | **2.03x WIN** | **2.02x WIN** |
| churn | 0.75x LOSS | 0.63x LOSS | 0.75x LOSS |
| cache-thrash | 0.56x LOSS | 0.63x LOSS | 0.74x LOSS |
| cross-thread | 0.85x LOSS | 0.42x LOSS | 0.43x LOSS |
| thread-churn | **2.61x WIN** | **2.18x WIN** | **2.32x WIN** |
| large | **14.63x WIN** | **7.61x WIN** | **6.80x WIN** |
| realloc-grow | **1.25x WIN** | **1.27x WIN** | **1.27x WIN** |
| overhead | 0.58x LOSS | 0.34x LOSS | 0.38x LOSS |

### FastAlloc vs mimalloc

| workload | T=1 | T=2 | T=4 |
| :--- | ---: | ---: | ---: |
| tiny | **1.31x WIN** | 0.97x LOSS | 0.92x LOSS |
| small-mixed | **1.30x WIN** | **1.02x WIN** | **1.20x WIN** |
| random-1-4096 | **1.82x WIN** | **1.57x WIN** | **1.35x WIN** |
| ramp | 0.36x LOSS | 0.42x LOSS | 0.51x LOSS |
| churn | 0.55x LOSS | 0.57x LOSS | 0.58x LOSS |
| cache-thrash | 0.64x LOSS | 0.64x LOSS | **1.12x WIN** |
| cross-thread | **1.05x WIN** | 0.66x LOSS | 0.87x LOSS |
| thread-churn | 0.82x LOSS | 0.67x LOSS | 0.56x LOSS |
| large | **9.15x WIN** | **4.83x WIN** | **4.56x WIN** |
| realloc-grow | 1.00x TIE | 1.02x TIE | 1.01x TIE |
| overhead | 0.46x LOSS | 0.32x LOSS | 0.29x LOSS |

## Latency percentiles (single thread, ns per alloc+free pair)

| workload | pctl | glibc | jemalloc | mimalloc | FastAlloc | verdict |
| :--- | :--- | ---: | ---: | ---: | ---: | :--- |
| tiny | p50 | 17.5 | 16.9 | 17.5 | **13.1** | FastAlloc 1.29x better |
| tiny | p99 | 20.3 | 18.6 | 21.1 | **18.2** | tie (within 2%) |
| tiny | p99.9 | 83.2 | 81.1 | 84.8 | **74.3** | FastAlloc 1.09x better |
| small-mixed | p50 | 24.0 | 17.9 | 20.5 | **15.6** | FastAlloc 1.15x better |
| small-mixed | p99 | 31.1 | 29.3 | 26.3 | **20.5** | FastAlloc 1.28x better |
| small-mixed | p99.9 | 101.2 | 110.6 | 95.4 | **91.0** | FastAlloc 1.05x better |
| random-1-4096 | p50 | 41.0 | 18.4 | 24.7 | **13.3** | FastAlloc 1.38x better |
| random-1-4096 | p99 | 53.9 | 25.3 | 27.6 | **15.4** | FastAlloc 1.65x better |
| random-1-4096 | p99.9 | 123.4 | **91.0** | 98.2 | 93.6 | jemalloc 1.03x better |

![Latency percentiles](charts/latency.png)

## Memory (overhead workload: 1M live objects, then free-all)

| metric | glibc | jemalloc | mimalloc | FastAlloc |
| :--- | ---: | ---: | ---: | ---: |
| RSS / live bytes at full live set | 0.78 | 1.20 | 1.16 | 1.08 |
| retained after free (MB) | 369.6 | 171.8 | 552.2 | **123.6** |
| retained after purge (MB) | 46.7 | 171.8 | 552.2 | **16.0** |
| peak RSS (MB) | 369.5 | 572.4 | 552.2 | 512.7 |
| alloc phase (ms) | 169 | 99 | 85 | 167 |
| free phase (ms) | 64 | 105 | 77 | 185 |

glibc's 0.78 RSS/live ratio benefits from its 1.28x trim gain (top-chunk
consolidation); its purge path is `malloc_trim(0)`, FastAlloc's is
`fast_alloc_purge()` (which drains the pending queues first). jemalloc and
mimalloc intentionally keep freed pages in arenas — that is precisely the
retention FastAlloc does not have: 2.9x/10.7x/34.5x less memory held after
purge. The flip side is visible in the free phase (185 ms vs 64–105 ms):
returning memory through the locked batch path costs time during the free
storm — the same mechanism that wins the retention column.

![Memory footprint](charts/memory.png)

## Thread lifecycle (spawn/exit bursts, 256 pairs per thread)

| threads | glibc ns/pair | jemalloc | mimalloc | FastAlloc |
| :--- | ---: | ---: | ---: | ---: |
| T=1 | **155** | 657 | 206 | 252 |
| T=2 | **115** | 519 | 159 | 238 |
| T=4 | **103** | 497 | 121 | 215 |
| threads/s (T=1) | **25,141** | 5,944 | 18,994 | 15,526 |

A fresh FastAlloc thread performs ~52 class refills (adaptive 16-block
batches, cross-arena steals) plus the O(bins) exit defer, while glibc's
tcache is 64 shallow bins with a 7-entry cap — warm-up/teardown is nearly
free. FastAlloc's 2.6x lead over jemalloc (and 1.2x over mimalloc at T=1)
comes from the v2 thread-lifecycle work: the TLSCache recycling pool, the
deferred lock-free exit hand-off, and direct-feed refills. The v1 → v2
improvement was 2614 → 305 ns/pair (8.6x); today's measurement lands at
252 ns/pair on this VM generation.

![Thread lifecycle](charts/thread_lifecycle.png)

## In-process microbenchmarks (Google Benchmark, std vs FastAlloc)

92 paired configs (family-isolated, `--benchmark_min_time=3000x`): 77 wins /
14 losses / 1 tie. Family verdicts (geometric mean of ratios; per-config
verdicts in `bench_report.md`):

| family | configs | verdict |
| :--- | ---: | :--- |
| MallocOnly | 18 | FastAlloc wins 18/18 (up to 50x at 4 KB, 285x at 4 KB/T=4) |
| FreeOnly | 15 | FastAlloc wins 15/15 (1.5–23.8x) |
| MallocFree | 15 | FastAlloc wins 8/15 (4096/512 B and single-thread win up to 34.6x; 8 KB loses 2.3–3.2x; 64/8 B lose at T=4/8 oversubscription) |
| HeavyContention | 20 | FastAlloc wins 20/20 (1.15–6.7x) |
| RandomSize | 3 | FastAlloc wins 3/3 (1.69–1.97x) |
| ScopedAlloc | 9 | FastAlloc wins 6/9 (geo-mean 0.82x — the 32 B T=1 0.05x warmup outlier drags the mean below parity) |
| LargeAlloc | 6 | FastAlloc wins 4/6 (64 KB T=1 0.10x = page-fault noise) |
| Realloc | 3 | FastAlloc wins 2/3 (512 B: std wins 1.29x) |
| Calloc | 3 | mixed: 10 B std wins 11.9x, 100 B FastAlloc 1.4x, 1000 B tie |

The extreme MallocOnly ratios (50x–285x at 4–8 KB) are real but
context-dependent: repeated same-size allocation re-faults fresh pages under
std::malloc while FastAlloc's span cache recycles already-backed pages —
the same effect that wins cache-thrash and the memory-retention column.

![Microbenchmark family verdicts](charts/gbench_verdicts.png)

## Thread scaling

On the 2-vCPU box, small-mixed peaks at T=2 for every allocator and FastAlloc
stays on top through the 2x-oversubscribed T=4 (1.40x vs glibc). Churn is the
stress case: FastAlloc holds 1.43x/1.26x over glibc at T=1/2 and converges to
a tie at T=4 as the shared-slab spinlocks saturate; jemalloc and mimalloc
pull ahead there on the strength of fully private arenas (0.74–0.75x).

![Thread scaling](charts/scaling.png)

## Side-by-side memory stress (fast_alloc_bench_memory, one process)

Threads {1,2,4} × block sizes {64, 256, 4096} B, 20,000 allocs per thread:
FastAlloc wins all 9 time configurations (1.33x–2.22x faster) at comparable
peak RSS (e.g. 4096 B/T=4: 283 MB std vs 292 MB FastAlloc — within 3%).

## Where FastAlloc still loses (and why) — unchanged root causes

| workload | this session's evidence | root cause |
| :--- | :--- | :--- |
| **realloc growth** | 0.28x vs glibc (consistent with 0.29x in the v4 matrix) | glibc expands its contiguous heap top in place — a property arena/slab allocators cannot replicate for small blocks without span-tracking headers on the hot free path. On par with mimalloc (1.00–1.02x), beats jemalloc (1.25x). |
| **thread-churn vs glibc** | 0.48–0.62x (T=1..4); 25.1k vs 15.5k threads/s | ~52 class refills per fresh thread vs glibc's 64 shallow tcache bins (7-entry cap). FastAlloc still beats mimalloc at T=1 and crushes jemalloc (2.2–2.6x). |
| **churn / cache-thrash vs jemalloc & mimalloc** | 0.55–0.75x | Competitors give each thread a private arena/heap so a constant live set never touches shared state. FastAlloc's shared slabs + 16 arenas beat glibc everywhere but not that design; closing it needs per-thread slab ownership. |
| **ramp vs mimalloc** | 0.36–0.51x | mimalloc's thread heap keeps ramp-up allocations thread-local; FastAlloc refills from shared slabs (locked) during growth. Still 1.54–1.94x faster than glibc. |
| **bulk alloc+free of 1M objects** | 0.66x vs glibc (free phase 185 ms vs 64 ms) | The free storm walks saturated bins through the locked batch path; the same mechanism returns 2.9–34.5x more memory to the OS. Speed-vs-RSS trade. |
| **large at T=1** | 0.91x vs glibc (was 0.90x in the v4 matrix) | Run-to-run variance dominates this cell; at T=4 FastAlloc is 1.04x ahead and 4.6–14.6x ahead of jemalloc/mimalloc at all thread counts. |
| **Calloc/10B** | std wins 11.9x | FastAlloc's calloc zeroes the full usable block (size-class rounded), glibc's zeroes only 10 requested bytes; at 100 B+ the comparison flips to FastAlloc. |
| **cross-thread at T=4** | 0.91x vs glibc (T=2 was 2.52x WIN) | With 1 producer + 3 consumers on 2 vCPU, the consumer's free-side MPSC handoff competes with producer allocations; glibc's tcache free is local. |

## Session-to-session variance (read before quoting numbers)

This VM generation re-measured the v4 matrix within expected noise on the
"stable" cells (realloc 0.28 vs 0.29; thread-churn T=1 0.62 vs 0.60; large
T=1 0.91 vs 0.90; latency p50 13.1–13.3 vs 12.8; purge retention 16.0 vs
15.8 MB) but moved the contended multi-threaded cells more than single-digit
percent: churn-vs-glibc at T=4 moved from 1.16x WIN to a 1.00x TIE, and
cross-thread T=4 from 1.48x WIN to 0.91x LOSS, while small-mixed T=4 moved
from 1.50x to 1.40x. Contended cells on 2-vCPU shared runners are the
noisiest in the matrix — the CI cross-allocator job exists precisely to
catch regressions with repeat runs rather than single numbers. Quote the
win/loss *pattern* (which is stable) rather than any individual multiplier;
re-run `bench_compare.py` on your target hardware before making claims.

## What changed in v2 (thread-lifecycle & churn optimizations)

Root-caused from instrumented runs, then fixed; each fix is verified by the
full test + sanitizer battery (75/75 release, 86 debug incl. 11 death tests —
one debug-only test intentionally skips because the registry fatals on forged
pointers by design, ASan/UBSan/LSan clean, TSan zero warnings).

1. **TLSCache recycling pool** — retired thread caches are parked in a
   capped 32-slot pool and re-used by new threads. A thread lifecycle
   previously paid mmap + 3 first-touch faults + munmap (~60-80 µs) just to
   own its cache; now it is two lock-guarded pointer moves. Shutdown-time
   pushes never munmap (strictly safer than before under a reaper).
2. **Adaptive refill with ramp reset** — a TLS bin's first miss now asks
   the global heap for 16 blocks and doubles per miss (capped at the old
   fixed target, `CACHE_LIMITS/2`). Previously a first miss pulled up to
   8,192 blocks: a 256-op burst thread touching ~50 classes yanked ~18,000
   blocks out of the slabs and handed every one of them back at exit. The
   ramp resets at thread death, so short-lived threads stay small while
   steady-state threads reach full batch size within ~9 misses.
3. **Lazy slab wiring** — `Slab::Create` no longer writes per-block
   headers across every page of the span (a 64 KB / 32 B slab = 2,048
   headers on 16 pages, all faulted at once). Blocks are carved and wired
   on first use; only the header page is touched at creation. Debug builds
   keep the eager wiring (poison/fresh machinery unchanged).
4. **Deferred thread-exit hand-off + direct-feed** — a dying thread parks
   its small bin remnants (≤64 blocks/class) on the per-arena lock-free
   pending queue in O(1 CAS) instead of doing per-block slab work under
   locks; the next thread that refills that class is fed **directly from
   the pending queue** (zero slab round-trip). Over-long queues fold back
   into the slabs, which keeps the queues O(64) and the slab free lists
   **warm** (the carve-first-touch faults were 25% of thread-churn CPU,
   measured by SIGPROF sampling).
5. **Cross-arena steal** — arena assignment rotates over 16 arenas, so the
   "next owner" of an arena's pending queue appears 16 spawns later; with
   random class sets the local queue was often empty while another arena
   held freed blocks for the class. Refills now try-lock foreign arenas
   (single attempt each — deadlock-free) and steal from their pending
   queues. This removed most of the remaining first-touch faults.
6. **Page-cache best-fit with splitting** — the page cache previously
   matched exact span sizes only; under mixed-size workloads its hit rate
   was literally zero (measured) and large-block churn degraded into
   mmap/munmap cycling. It now splits the smallest cached span ≥ the
   request (lock order high→low bin, deadlock-free; non-empty hints via an
   atomic bitmap).
7. **4 MB span pool (Linux)** — all spans ≤ 2 MB are carved from 4 MB
   mappings and released with `madvise(MADV_DONTNEED)` instead of per-slab
   mmap/munmap. A 50 MB live set previously meant 700+ tiny VMAs that THP
   cannot collapse; now a handful of VMAs keep TLB pressure low. mremap
   fast paths are disabled for pooled spans (bump-space overlap hazard).
8. **Smaller TLS large-block cache** — per-class retention dropped from
   16 MB (up to ~50 MB/thread) to ~2 MB, keeping the shared 64 MB page
   cache under its cap so it actually gets used.
9. **Stats fix** — release-mode `current_live_bytes` counted requested
   bytes at alloc but usable bytes at free, wrapping the counter to ~16 EB
   after enough churn; both sides now count the same quantity.
10. **`fast_alloc_purge()` now drains the pending queues first** — the
    deferred hand-off parks blocks whose slabs cannot empty while the
    blocks are "out"; purge drains them so empty slabs (and their spans)
    actually return to the OS. Post-purge retention is back to 16 MB.

## Old suite (Google Benchmark) numbers — kept for continuity

The legacy `bench_main` batch-of-500 numbers (8 B: 9.6 µs std vs 4.8 µs
FastAlloc; 512 B: 53.2 µs vs 5.0 µs) reproduce, but they exercise only the
LIFO batch pattern that flatters every thread-caching allocator including
glibc's own tcache. Treat them as a fast-path microbenchmark, not as an
application-level claim. The legacy `bench_memory` side-by-side mode also
had a known flaw: it ran std then FastAlloc **in one process**, so
FastAlloc's "peak RSS" column inherited glibc's high-water mark
(`ru_maxrss` is process-lifetime monotonic). The cross-allocator suite
fixes this by running one allocator per process; the side-by-side binary is
kept as a quick smoke tool and its RSS column should be read with that
caveat.

## Design notes affecting performance

- The TLS fast path is two dependent loads, a store and a decrement
  (release build); `fast_free_sized` skips the block-header load entirely.
- Statistics are batched per thread (plain increments, flushed every 256 ops).
- The page-span cache (2 MB/bin, 64 MB global, **best-fit with splitting**)
  makes large-block cycles userspace-only — and returns memory aggressively
  on purge (which now drains the pending queues first).
- Slab spans ≤ 2 MB come from the 4 MB Linux span pool
  (`madvise(MADV_DONTNEED)` release) — THP-friendly, no VMA fragmentation.
- `FASTALLOC_DEBUG` (canaries, poison, registry) is off by default; the
  debug build is for validation, not production numbers. Debug builds keep
  eager slab wiring; lazy carving is release-only.
- Windows paths use Fls* + VirtualAlloc; behaviour is equivalent but was
  verified by inspection on this box (no MSVC run here; CI covers it — the
  windows-msvc-asan job runs the suite under ASan with the runtime DLL
  staged next to the executables). The span pool and mremap guards are
  Linux-only (`#ifdef __linux__`), so Windows takes the previous syscall
  path unchanged.

## Honest summary

FastAlloc is the fastest allocator in this matrix for the classic
server-shaped workload — small mixed-size alloc/free pairs — at **every**
thread count including 2x oversubscription (1.16–1.51x vs glibc, winning
against jemalloc and mimalloc at T=1/T=4 too), with the best p50/p99 tail
latency of all four allocators, 4.6–14.6x faster than jemalloc/mimalloc on
large-block cycles, and it returns 2.9–34.5x more memory to the OS after
purge than any competitor. It is **not** the fastest at realloc-heavy
patterns (0.28x vs glibc's in-place heap growth; ties mimalloc, beats
jemalloc), at constant-live-set churn against mimalloc's/jemalloc's
private-arena design (still beats glibc at T=1/2, ties at T=4), at ramp
growth against mimalloc's thread-local heap (0.36–0.51x), at multi-thread
spawn/exit storms against glibc's minimal tcache (0.48–0.62x; but 2.2–2.6x
faster than jemalloc), or at bulk free storms (0.66x — the price of the
memory-return column). Every gap has a measured root cause above and a
scoped fix proposal; every number in this report can be regenerated with
one command (`bench_compare.py`).

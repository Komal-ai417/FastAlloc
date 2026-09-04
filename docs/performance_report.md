# Performance Report — FastAlloc v2.0.0

> **Methodology note (v4):** all numbers below come from the
> cross-allocator benchmark suite (`benchmarks/benchsuite/`), run against
> three reference allocators on the same machine in the same session:
> **glibc 2.41 (ptmalloc+tcache)**, **jemalloc 5.3.0**, and
> **mimalloc 2.1.7**. Each (allocator, workload, thread-count) cell is the
> **median of 5 repetitions** after a warmup rep, one allocator per process,
> launch order rotated per workload to decorrelate drift. Every workload
> verifies block contents on free (checksum-guarded; zero failures recorded
> across the 132-cell matrix). This revision adds the **v2 allocator
> optimizations** (see "What changed in v2") — every gain below was
> re-validated against the full sanitizer battery after the change.

## Reference environment

- CPU: 2 vCPU x86-64 VM (Intel Xeon), 4 GB RAM
- OS: Debian 13, Linux
- Toolchain: GCC 14.2, `-O3 -march=native` + LTO (CMake Release)
- Baselines: glibc 2.41 (default), jemalloc 5.3.0 (LD_PRELOAD),
  mimalloc 2.1.7 (LD_PRELOAD)
- Suite: `bench_suite` — 11 workloads × {1, 2, 4} threads × 4 allocators;
  T=4 is 2x oversubscribed on this box and is reported as such.

Run it yourself:

```bash
cmake -B build-bench -DFASTALLOC_BUILD_TESTS=OFF -DFASTALLOC_BUILD_BENCHMARKS=ON
cmake --build build-bench --target bench_suite -j
# glibc (no preload) / FastAlloc:
./build-bench/bench_suite --alloc std  --label glibc    --workload churn --threads 4 --reps 5 --json out.jsonl
./build-bench/bench_suite --alloc fast --label FastAlloc --workload churn --threads 4 --reps 5 --json out.jsonl
# jemalloc / mimalloc via preload:
LD_PRELOAD=/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build-bench/bench_suite --alloc std --label jemalloc ...
LD_PRELOAD=<path>/libmimalloc.so.2                       ./build-bench/bench_suite --alloc std --label mimalloc ...
```

## What changed in v2 (thread-lifecycle & churn optimizations)

Root-caused from instrumented runs, then fixed; each fix is verified by the
full test + sanitizer battery (69/69 release, 80/80 debug incl. 11 death
tests, ASan/UBSan/LSan clean, TSan zero warnings, 70 s TSan soak with
~5,000 thread create/destroy cycles, 70 s ASan+debug soak, page-cache
retention check).

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
   steady-state threads reach full batch size within ~9 misses (amortized
   noise for 10M-op runs).
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

## Where FastAlloc wins (single-thread medians, ns per alloc+free pair)

| workload | glibc | jemalloc | mimalloc | FastAlloc | speedup vs glibc |
| :--- | ---: | ---: | ---: | ---: | ---: |
| tiny (1–32 B) | 19.1 | 17.6 | 14.8 | **12.9** | 1.48x |
| small-mixed (realistic mix) | 25.1 | 18.8 | 21.4 | **15.1** | 1.66x |
| random 1–4096 B | 42.0 | 19.6 | 25.7 | **13.7** | 3.08x |
| ramp (0→100k live→0) | 76.3 | 95.3 | 15.2 | 43.3 | 1.76x |
| large (64KB–512KB + touch) | 21.3 | 345.2 | 210.8 | **23.6** | 0.90x T=1 (noisy); 1.61x T=2, 1.89x T=4 |

- **Single-thread latency percentiles (batch-of-64 timing):** FastAlloc has
  the best p50 **and** p99 in every pair workload, e.g. `random-1-4096`:
  p50 12.8 ns vs glibc 40.9 / jemalloc 18.4 / mimalloc 24.6; p99 15.5 ns
  vs 59.1 / 25.1 / 29.1. p99.9 stays under 90 ns.
- **Multi-thread small-block pairs:** FastAlloc is fastest at **every**
  thread count including the 2x-oversubscribed T=4 case that it previously
  lost: `small-mixed` T=4 = 13.6 ns vs glibc 20.4 (1.50x), jemalloc 16.4,
  mimalloc 16.8. The v1 gap (0.52–0.79x) is gone.
- **Thread-churn (spawn/exit bursts): 2614 → 305 ns/pair (8.6x faster).**
  At T=1 FastAlloc is now 0.60x glibc (was 0.04x), **1.3x faster than
  mimalloc** and **2.2x faster than jemalloc**. The remaining gap to glibc
  is the ~52 class-refills a fresh thread performs vs glibc's tiny 64-bin
  tcache — see the loss table.
- **churn / cache-thrash:** FastAlloc now beats glibc at every thread count
  (churn: 1.69x/1.35x/1.16x at T=1/2/4; cache-thrash: 1.34x/1.33x/1.12x),
  where v1 was 1.1–1.3x on churn and ~1.1x on cache-thrash. jemalloc and
  mimalloc still lead these two (0.6–0.74x) — their per-thread arenas keep
  a constant live set off shared structures entirely.
- **Cross-thread MPSC handoff** (T=2): 2.65x vs glibc; on par with
  jemalloc/mimalloc (pending-queue path). At T=4 (1.48x glibc, 1.23x
  mimalloc) FastAlloc leads every allocator.
- **Memory return-to-OS** (1M live objects, 476 MB payload, then free-all):
  FastAlloc retains **15.8 MB after purge** vs 46.5 MB glibc
  (`malloc_trim`), 171.8 MB jemalloc, 552.3 MB mimalloc — 3x/11x/35x less.
  Retained right after free (no purge): 123 MB vs glibc 369 / jemalloc 172
  / mimalloc 552 — least of the four. RSS/live at full live set: 1.07
  (glibc 0.78 with its 1.28 trim gain).

## Where FastAlloc still loses (and why)

| workload | evidence | root cause |
| :--- | :--- | :--- |
| **thread-churn at T≥2** | 281/252 ns vs glibc 141/103 (0.50x/0.41x) | A fresh thread performs ~52 class refills (16 blocks each, stolen cross-arena) plus an O(bins) exit defer. glibc's tcache is 64 shallow bins with a 7-entry cap, so its per-thread warm-up/teardown is trivial. v1 was 0.04–0.16x here; v2 is 0.41–0.50x vs glibc but beats mimalloc (0.56–0.63x) and crushes jemalloc (2x). Structural without a per-CPU cache redesign. |
| **realloc growth** (1.5x geometric to 8KB) | 31.6 ns/step vs glibc 9.0 (0.29x) | glibc expands its contiguous heap top in place — a property arena/slab allocators cannot replicate for small blocks without span-tracking headers on the hot free path. FastAlloc is on par with mimalloc (1.03x) and beats jemalloc (1.30x). The copy path itself is already minimal (pop new class, memcpy, push old). |
| **churn / cache-thrash vs jemalloc/mimalloc** | 0.6–0.74x at T=1..4 | Both competitors give each thread a private arena/heap, so a constant 50 MB live set never touches shared state. FastAlloc's shared slabs + 16 arenas win against glibc everywhere but not against that design; closing it needs per-thread slab ownership (a P4-scale redesign). |
| **ramp vs mimalloc** | 0.35–0.55x | mimalloc's thread heap keeps ramp-up allocations entirely thread-local; FastAlloc refills from shared slabs (locked) during growth. Still 1.76–2.14x faster than glibc. |
| **bulk alloc+free of 1M objects** | 334 vs glibc 228 ns/op (0.68x; v1: 0.61x) | The free phase (177 ms vs glibc 63 ms) walks saturated bins through the locked batch path; the same mechanism is what returns 3–35x more memory to the OS than the competitors. Speed-vs-RSS trade, visible in both tables. |
| **large at T=1** | 23.6 vs glibc 21.3 (0.90x) | Run-to-run variance dominates this cell (glibc itself moved 26.2→21.3 between the v1 and v3 matrices with identical code); at T=2/T=4 FastAlloc is clearly ahead (1.61x/1.89x) and 7–15x ahead of jemalloc/mimalloc at all thread counts. |

## Old suite (Google Benchmark) numbers — kept for continuity

The legacy `bench_main` batch-of-500 numbers (8 B: 9.6 µs std vs 4.8 µs
FastAlloc; 512 B: 53.2 µs vs 5.0 µs) reproduce, but they exercise only the
LIFO batch pattern that flatters every thread-caching allocator including
glibc's own tcache. Treat them as a fast-path microbenchmark, not as an
application-level claim. The legacy `bench_memory` side-by-side mode also
had a known flaw: it ran std then FastAlloc **in one process**, so
FastAlloc's "peak RSS" column inherited glibc's high-water mark
(`ru_maxrss` is process-lifetime monotonic). The new suite fixes this by
running one allocator per process.

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
  verified by inspection on this box (no MSVC run here; CI covers it). The
  span pool and mremap guards are Linux-only (`#ifdef __linux__`), so
  Windows takes the previous syscall path unchanged.

## Honest summary

FastAlloc is the fastest allocator in this matrix for the classic
server-shaped workload — small mixed-size alloc/free pairs — at **every**
thread count including 2x oversubscription, with best-in-class p50/p99
latency, 8.6x faster thread lifecycles than v1 (now ahead of mimalloc and
jemalloc at T=1, within 0.5x of glibc), and it remains dramatically faster
(7–15x) than jemalloc/mimalloc on large-block cycles while returning
3–35x more memory to the OS than any competitor after purge. It is **not**
the fastest at realloc-heavy patterns (glibc's contiguous heap grows in
place; FastAlloc ties mimalloc and beats jemalloc), at
constant-live-set churn against jemalloc/mimalloc's private-arena design
(though it beats glibc everywhere), or at multi-thread spawn/exit storms
against glibc's minimal tcache. Every remaining gap has a measured root
cause and a scoped fix proposal above.

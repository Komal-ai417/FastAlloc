#!/usr/bin/env python3
"""
analyze.py — analyzer for the FastAlloc cross-allocator benchmark matrix.

Usage: python3 analyze.py [bench_results.jsonl]

Reads the JSONL emitted by bench_suite (via run_matrix.sh), aggregates each
(allocator, workload, thread-count) cell as the median of its reps, and prints
markdown tables: throughput per thread count, FastAlloc speedups, latency
percentiles, memory overhead/retention, and thread scaling.
"""
import json
import statistics as st
import sys
from collections import defaultdict
from pathlib import Path

JSONL = Path(sys.argv[1] if len(sys.argv) > 1 else "bench_results.jsonl")
ALLOC_ORDER = ["glibc", "jemalloc", "mimalloc", "FastAlloc"]
FAST = "FastAlloc"
WLS = ["tiny", "small-mixed", "random-1-4096", "ramp", "churn", "cache-thrash",
       "cross-thread", "thread-churn", "large", "realloc-grow", "overhead"]


def load():
    recs = []
    for line in JSONL.read_text().splitlines():
        line = line.strip()
        if line:
            recs.append(json.loads(line))
    cells = {}
    for d in recs:
        key = (d["workload"], d["threads"], d["label"])
        cells.setdefault(key, []).extend(d["reps"])
    return cells


def med(cells, wl, t, a):
    v = cells.get((wl, t, a))
    return st.median([r["ns_per_op"] for r in v]) if v else None


def speed_table(cells, t):
    rows = [f"| workload (ns/op, T={t}) | " + " | ".join(f"{a:>10}" for a in ALLOC_ORDER) + " |",
            "|" + "---|" * (len(ALLOC_ORDER) + 1)]
    for wl in WLS:
        vals = [med(cells, wl, t, a) for a in ALLOC_ORDER]
        cells_ = [f"{v:>10.2f}" if v else f"{'-':>10}" for v in vals]
        rows.append(f"| {wl:<26} | " + " | ".join(cells_) + " |")
    return "\n".join(rows)


def speedup_table(cells, t):
    rows = [f"| workload (T={t}) | vs glibc | vs jemalloc | vs mimalloc | verdict |",
            "|---|---|---|---|---|"]
    for wl in WLS:
        f = med(cells, wl, t, FAST)
        if not f:
            continue
        su = {}
        for a in ("glibc", "jemalloc", "mimalloc"):
            b = med(cells, wl, t, a)
            su[a] = b / f if b else None
        vals = [v for v in su.values() if v]
        verdict = ("WIN" if all(v > 1 for v in vals)
                   else "PARTIAL" if any(v > 1 for v in vals) else "LOSS") if vals else "?"
        def cell(a):
            return f"{su[a]:>6.2f}x" if su[a] else "   -  "
        rows.append(f"| {wl:<26} | {cell('glibc')} | {cell('jemalloc')} | {cell('mimalloc')} | {verdict} |")
    return "\n".join(rows)


def main():
    cells = load()
    print(f"# FastAlloc benchmark matrix ({JSONL.name})\n")
    for t in (1, 2, 4):
        print(f"\n## Throughput (T={t})\n")
        print(speed_table(cells, t))
        print(f"\n### FastAlloc speedup (T={t}) — >1.00x = FastAlloc faster\n")
        print(speedup_table(cells, t))
    print("\n## Latency percentiles (1 thread)\n")
    print("| workload | allocator | p50 | p99 | p99.9 |")
    print("|---|---|---|---|---|")
    for wl in ("tiny", "small-mixed", "random-1-4096"):
        for a in ALLOC_ORDER:
            v = cells.get((wl, 1, a))
            if v and v[0].get("p50", -1) > 0:
                p50 = st.median([r["p50"] for r in v])
                p99 = st.median([r["p99"] for r in v])
                p999 = st.median([r["p999"] for r in v])
                print(f"| {wl} | {a} | {p50:.1f} | {p99:.1f} | {p999:.1f} |")
    print("\n## Memory (overhead workload, 1M objects)\n")
    print("| metric | " + " | ".join(ALLOC_ORDER) + " |")
    print("|" + "---|" * (len(ALLOC_ORDER) + 1))
    for field, name in [("overhead_ratio", "RSS/live ratio"),
                        ("retained_after_free_mb", "retained after free (MB)"),
                        ("retained_after_purge_mb", "retained after purge (MB)"),
                        ("alloc_phase_ms", "alloc phase (ms)"),
                        ("free_phase_ms", "free phase (ms)")]:
        vals = []
        for a in ALLOC_ORDER:
            v = cells.get(("overhead", 1, a))
            x = v[0].get(field) if v else None
            vals.append(f"{x:.2f}" if isinstance(x, (int, float)) and x >= 0 else "-")
        print(f"| {name} | " + " | ".join(vals) + " |")
    print("\n## Thread scaling (churn)\n")
    print("| threads | " + " | ".join(f"{a} M ops/s" for a in ALLOC_ORDER) + " |")
    print("|" + "---|" * (len(ALLOC_ORDER) + 1))
    for t in (1, 2, 4):
        row = []
        for a in ALLOC_ORDER:
            v = med(cells, "churn", t, a)
            row.append(f"{1e9 / v / 1e6:>10.2f}" if v else f"{'-':>10}")
        print(f"| T={t} | " + " | ".join(row) + " |")


if __name__ == "__main__":
    main()

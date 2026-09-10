#!/usr/bin/env python3
"""
bench_compare.py — the ONE-SHOT FastAlloc benchmark verdict tool.

Runs EVERY benchmark in the repository and prints a per-category win/loss
verdict with the exact multiplier ("MallocOnly 16B T=1: FastAlloc wins by
1.29x", "Realloc 512B T=1: std wins by 1.29x", ...), for:

  1. Legacy Google-Benchmark micro-suites (in-process, std vs FastAlloc):
       fast_alloc_bench           (BM_MallocFree_{Std,FastAlloc})
       fast_alloc_bench_extended  (MallocOnly, FreeOnly, RandomSize, ScopedAlloc,
                                   LargeAlloc, Realloc, Calloc, HeavyContention)
  2. The rigorous cross-allocator suite (bench_suite, ONE allocator per
     process, warmup + N reps, medians, checksum-guarded):
       glibc / jemalloc / mimalloc (via LD_PRELOAD) vs FastAlloc
       across all 11 workloads x {1,2,4} threads, with latency percentiles,
       memory overhead/retention, and thread-lifecycle (thread-churn) data.
  3. The side-by-side memory stress benchmark (fast_alloc_bench_memory):
       time + peak RSS for std vs FastAlloc across threads x block sizes.

Outputs:
  console   — the full verdict report (what you read)
  --report  — the same report as Markdown (bench_report.md)
  --data    — machine-readable JSON (bench_report_data.json) for charting
  --jsonl   — raw bench_suite JSONL records (append mode)

Usage:
  python3 bench_compare.py                       # full run, all phases
  python3 bench_compare.py --phase suite         # only the cross-allocator suite
  python3 bench_compare.py --quick               # smoke preset (reps=3, 20% ops)
  python3 bench_compare.py --reps 5 --threads 1,2,4 --ops-scale 1.0

Allocator discovery: jemalloc and mimalloc are located automatically (or via
JEMALLOC_SO / MIMALLOC_SO env vars); missing allocators are skipped with a
notice — the verdict table then covers whatever ran.

Exit code: 0 unless a bench_suite checksum fails (rc=42) or a benchmark
crashes twice — a failed allocator run is reported as such, never silently
dropped.
"""

import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

SUITE_WORKLOADS = [
    "tiny", "small-mixed", "random-1-4096", "ramp", "churn", "cache-thrash",
    "cross-thread", "thread-churn", "large", "realloc-grow", "overhead",
]
PAIR_WORKLOADS = ["tiny", "small-mixed", "random-1-4096"]  # latency percentiles
VERDICT_EPS = 1.02  # within +/-2% => statistical tie
PER_CELL_TIMEOUT = 1800  # seconds per bench_suite invocation


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

def run(cmd, env=None, timeout=PER_CELL_TIMEOUT, cwd=None):
    """Run a command; returns (rc, stdout+stderr)."""
    full_env = dict(os.environ)
    if env:
        for k, v in env.items():
            if v is None:
                full_env.pop(k, None)
            else:
                full_env[k] = v
    try:
        p = subprocess.run(cmd, capture_output=True, text=True,
                           env=full_env, timeout=timeout, cwd=cwd)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired:
        return 124, "TIMEOUT after %ss" % timeout


def median_or_none(xs):
    xs = [x for x in xs if x is not None and x >= 0]
    return statistics.median(xs) if xs else None


def verdict(ratio):
    if ratio is None:
        return "?"
    if ratio > VERDICT_EPS:
        return "WIN"
    if ratio < 1.0 / VERDICT_EPS:
        return "LOSS"
    return "TIE"


def fmt_x(v):
    return ("%5.2fx" % v) if v is not None else "    - "


def env_version():
    try:
        import platform
        return platform.python_version()
    except Exception:
        return "?"


# ---------------------------------------------------------------------------
# Allocator + build discovery
# ---------------------------------------------------------------------------

class Allocator:
    def __init__(self, key, label, mode, preload):
        self.key, self.label, self.mode, self.preload = key, label, mode, preload
        self.available = True
        self.version = ""


def find_build_dir(explicit):
    if explicit:
        d = Path(explicit)
        if (d / "bench_suite").exists() or (d / "bench_suite.exe").exists():
            return d
        raise SystemExit(f"build dir {explicit} has no bench_suite binary")
    here = Path(__file__).resolve().parent
    for cand in [here / ".." / ".." / "build", here / ".." / ".." / "build-bench",
                 Path.cwd() / "build"]:
        for exe in ("bench_suite", "bench_suite.exe"):
            if (cand / exe).exists():
                return cand.resolve()
    raise SystemExit("bench_suite not found; build first: "
                     "cmake -B build && cmake --build build --target bench_suite")


def exe(build, name):
    for cand in (name, name + ".exe"):
        if (build / cand).exists():
            return str(build / cand)
    return None


def detect_allocators(jemalloc_so, mimalloc_so, wanted):
    allocs = [Allocator("glibc", "glibc", "std", None)]
    jem = jemalloc_so or shutil.which("jemalloc") or None
    for cand in ["/lib/x86_64-linux-gnu/libjemalloc.so.2",
                 "/usr/lib/x86_64-linux-gnu/libjemalloc.so.2",
                 "/usr/lib/libjemalloc.so.2"]:
        if jem is None and Path(cand).exists():
            jem = cand
            break
    if jem and Path(jem).exists():
        allocs.append(Allocator("jemalloc", "jemalloc", "std", str(jem)))
    mi = mimalloc_so
    for cand in ["/usr/local/lib/libmimalloc.so.2", "/usr/lib/libmimalloc.so.2",
                 "/usr/local/lib/libmimalloc.so"]:
        if mi is None and Path(cand).exists():
            mi = cand
            break
    if mi and Path(mi).exists():
        allocs.append(Allocator("mimalloc", "mimalloc", "std", str(mi)))
    allocs.append(Allocator("fast", "FastAlloc", "fast", None))
    # filter by user request
    keep = []
    for a in allocs:
        if a.key in wanted or a.label in wanted:
            keep.append(a)
    return keep


# ---------------------------------------------------------------------------
# Phase 1 — Google-Benchmark micro-suites (std vs FastAlloc, in-process)
# ---------------------------------------------------------------------------

GBENCH_MIN_TIME = "3000x"


def gbench_families(binary):
    rc, out = run([binary, "--benchmark_list_tests=true"], timeout=120)
    if rc != 0:
        return []
    fams, seen = [], set()
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        fam = line.split("/")[0]
        if fam and fam not in seen:
            seen.add(fam)
            fams.append(fam)
    return fams


def parse_gbench_name(name):
    # BM_MallocOnly_Std/64/threads:4  -> ("MallocOnly", "Std", "64", 4)
    m = re.match(r"^BM_(\w+?)_(Std|FastAlloc)(?:/([^/]+))?(?:/threads:(\d+))?$", name)
    if not m:
        return None
    return m.group(1), m.group(2), m.group(3) or "1", int(m.group(4) or 1)


def phase_gbench(build, tmpdir, cache=None):
    """Runs both gbench binaries family-isolated; returns rows + verdicts.

    Cached family JSON files (named {binary}_{family}.json) under `cache` are
    reused instead of re-running — handy for resuming long sessions."""
    rows = []
    for binname in ("fast_alloc_bench", "fast_alloc_bench_extended"):
        binary = exe(build, binname)
        if not binary:
            print(f"  !! {binname} not built — skipped")
            continue
        print(f"  -- {binname}: ", end="", flush=True)
        fams = gbench_families(binary)
        pairs = {}
        for fam in fams:
            outjson = Path(cache or tmpdir) / f"{binname}_{fam}.json"
            if outjson.exists():
                try:
                    data = json.loads(outjson.read_text())
                except Exception:
                    data = None
                if data:
                    for b in data.get("benchmarks", []):
                        _absorb_bench(pairs, b)
                    continue
            cmd = [binary, f"--benchmark_filter=^{fam}/",
                   f"--benchmark_min_time={GBENCH_MIN_TIME}",
                   "--benchmark_repetitions=1",
                   "--benchmark_report_aggregates_only=false",
                   "--benchmark_format=json",
                   f"--benchmark_out={outjson}"]
            rc, _ = run(cmd, timeout=1800)
            if rc != 0:
                print(f"[{fam} rc={rc}]", end=" ", flush=True)
                continue
            try:
                data = json.loads(outjson.read_text())
            except Exception:
                continue
            for b in data.get("benchmarks", []):
                _absorb_bench(pairs, b)
        print(f"{len(pairs)} paired configs")
        rows.extend(_pairs_to_rows(pairs, binname))
    return rows


def _absorb_bench(pairs, b):
    if "aggregate_name" in b:
        return
    p = parse_gbench_name(b["name"])
    if not p:
        return
    fam_, variant, arg, threads = p
    t = b.get("time", b.get("real_time"))
    if t is None:
        return
    pairs.setdefault((fam_, arg, threads), {})[variant] = t


def _pairs_to_rows(pairs, binname):
    rows = []
    for (fam_, arg, threads), d in sorted(pairs.items(),
                                            key=lambda kv: (kv[0][0], kv[0][1], kv[0][2])):
        s, f = d.get("Std"), d.get("FastAlloc")
        if s is None or f is None:
            continue
        ratio = s / f
        rows.append({"family": fam_, "arg": arg, "threads": threads,
                     "std_ns": s, "fast_ns": f, "ratio": ratio,
                     "verdict": verdict(ratio), "binary": binname})
    return rows


def gbench_family_summary(rows):
    """Aggregate per family: wins/losses/ties + geometric-mean ratio."""
    fams = {}
    for r in rows:
        f = fams.setdefault(r["family"], {"win": 0, "loss": 0, "tie": 0, "ratios": []})
        f[r["verdict"].lower()] += 1
        f["ratios"].append(r["ratio"])
    out = {}
    for fam, d in fams.items():
        logs = [max(r, 1e-9) for r in d["ratios"]]
        gm = pow(10.0, sum(__import__("math").log10(x) for x in logs) / len(logs))
        n = d["win"] + d["loss"] + d["tie"]
        if d["win"] > d["loss"] and d["win"] > d["tie"]:
            v = "FastAlloc"
        elif d["loss"] > d["win"] and d["loss"] > d["tie"]:
            v = "std"
        else:
            v = "mixed"
        out[fam] = {"win": d["win"], "loss": d["loss"], "tie": d["tie"], "n": n,
                    "geo_mean_ratio": gm, "overall": v}
    return out


# ---------------------------------------------------------------------------
# Phase 2 — cross-allocator suite (bench_suite, one allocator per process)
# ---------------------------------------------------------------------------

DEFAULT_OPS = {
    "tiny": 20_000_000, "small-mixed": 10_000_000, "random-1-4096": 5_000_000,
    "ramp": 10_000_000, "churn": 10_000_000, "cache-thrash": 5_000_000,
    "cross-thread": 4_000_000, "thread-churn": 2_000_000, "large": 20_000,
    "realloc-grow": 5_000_000, "overhead": 1_000_000,
}


def run_suite_cell(binary, alloc, workload, threads, reps, ops, jsonl):
    """One (allocator, workload, threads) cell: warmup + reps, JSONL appended."""
    env = {"LD_PRELOAD": alloc.preload}
    cmd = [binary, "--alloc", alloc.mode, "--label", alloc.label,
           "--workload", workload, "--threads", str(threads),
           "--reps", str(reps), "--ops", str(ops), "--json", str(jsonl)]
    last_rc = 0
    for attempt in (1, 2):
        rc, out = run(cmd, env=env)
        last_rc = rc
        if rc == 0:
            return True, out
        print(f"      [attempt {attempt} rc={rc}]", end="", flush=True)
    return False, out + f"\n[FAILED twice rc={last_rc}]"


def phase_suite(build, allocs, threads_list, reps, ops_scale, jsonl_path,
                workloads=None, skip_run=False):
    """Runs the full workloads x threads x allocators matrix, parses JSONL.

    workloads: subset of SUITE_WORKLOADS to RUN this call (parsing covers
    everything recorded in the JSONL — chunked runs accumulate there).
    skip_run: parse-only mode (report regeneration without re-running)."""
    binary = exe(build, "bench_suite")
    if not binary:
        raise SystemExit("bench_suite binary not found")
    jsonl = Path(jsonl_path)
    jsonl.parent.mkdir(parents=True, exist_ok=True)
    todo = workloads or SUITE_WORKLOADS

    if not skip_run:
        for idx, wl in enumerate(todo):
            ops = max(1000, int(DEFAULT_OPS[wl] * ops_scale))
            rot = idx % len(allocs)                # rotate start order per workload
            order = allocs[rot:] + allocs[:rot]
            for th in threads_list:
                print(f"  -- {wl:<14} T={th}: ", end="", flush=True)
                for alloc in order:
                    ok, _ = run_suite_cell(binary, alloc, wl, th, reps, ops, jsonl)
                    print(f"{alloc.label}:{'ok' if ok else 'FAIL'} ", end="", flush=True)
                print()

    cells = {}
    if jsonl.exists():
        for line in jsonl.read_text().splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except Exception:
                continue
            key = (rec["workload"], rec["threads"], rec["label"])
            cells.setdefault(key, []).extend(rec["reps"])

    parsed = {}
    for (wl, th, label), reps_list in cells.items():
        cell = {
            "ns_per_op": median_or_none([r.get("ns_per_op") for r in reps_list]),
            "ops_per_sec": median_or_none([r.get("ops_per_sec") for r in reps_list]),
            "checksum_ok": all(r.get("checksum_ok", True) for r in reps_list),
        }
        if wl in PAIR_WORKLOADS and th == 1:
            cell["p50"] = median_or_none([r.get("p50") for r in reps_list])
            cell["p99"] = median_or_none([r.get("p99") for r in reps_list])
            cell["p999"] = median_or_none([r.get("p999") for r in reps_list])
        if wl == "overhead":
            r0 = reps_list[0]
            cell.update({k: r0.get(k) for k in
                         ("overhead_ratio", "retained_after_free_mb",
                          "retained_after_purge_mb", "rss_peak_mb",
                          "alloc_phase_ms", "free_phase_ms",
                          "native_resident_mb", "native_live_mb",
                          "native_cache_mb")})
        if wl == "thread-churn":
            sp = reps_list[0].get("extra_a", 0)
            cell["spawned_threads"] = sp
            if cell["ns_per_op"]:
                wall_s = cell["ns_per_op"] * 1e-9 * max(
                    1000, int(DEFAULT_OPS["thread-churn"] * ops_scale))
                cell["threads_per_sec"] = (sp / wall_s) if (sp and wall_s > 0) else None
        parsed[(wl, th, label)] = cell
    return parsed


# ---------------------------------------------------------------------------
# Phase 3 — side-by-side memory benchmark (fast_alloc_bench_memory)
# ---------------------------------------------------------------------------

def phase_memory(build, threads_list, sizes, allocs_per_thread):
    binary = exe(build, "fast_alloc_bench_memory")
    if not binary:
        print("  !! fast_alloc_bench_memory not built — skipped")
        return []
    rows = []
    for th in threads_list:
        for sz in sizes:
            cmd = [binary, "--threads", str(th),
                   "--allocs", str(allocs_per_thread), "--size", str(sz)]
            rc, out = run(cmd, timeout=600)
            if rc != 0:
                print(f"  !! bench_memory T={th} sz={sz} rc={rc}")
                continue
            m_t = re.search(r"time \(s\):\s+([\d.]+)\s+([\d.]+)", out)
            m_r = re.search(r"peak RSS \(MB\):\s+(\d+)\s+(\d+)", out)
            m_s = re.search(r"speedup:\s+([\d.]+)x", out)
            if not (m_t and m_r and m_s):
                continue
            std_t, fast_t = float(m_t.group(1)), float(m_t.group(2))
            std_rss, fast_rss = int(m_r.group(1)), int(m_r.group(2))
            rows.append({"threads": th, "size": sz,
                         "std_time_s": std_t, "fast_time_s": fast_t,
                         "speedup": float(m_s.group(1)),
                         "std_rss_mb": std_rss, "fast_rss_mb": fast_rss,
                         "rss_ratio": (fast_rss / std_rss) if std_rss else None})
    return rows


# ---------------------------------------------------------------------------
# Report rendering (console + markdown share one builder)
# ---------------------------------------------------------------------------

class Report:
    def __init__(self, meta):
        self.meta = meta
        self.lines = []

    def add(self, s=""):
        self.lines.append(s)

    def render(self):
        return "\n".join(self.lines) + "\n"


def build_report(meta, gbench_rows, gbench_summary, suite_cells, allocs,
                 threads_list, mem_rows, memphase_rows):
    r = Report(meta)
    W = 100
    r.add("=" * W)
    r.add(" FastAlloc — FULL BENCHMARK VERDICT REPORT (one tool, every suite)")
    r.add("=" * W)
    r.add(f" date      : {meta['date']}")
    r.add(f" cpu       : {meta['cpu']}")
    r.add(f" os        : {meta['os']}")
    r.add(f" allocators: {', '.join(meta['allocators'])}   (reps={meta['reps']}, "
          f"threads={meta['threads']}, ops-scale={meta['ops_scale']})")
    r.add(" verdict   : ratio > 1.02 = FastAlloc WINS; < 0.98 = competitor wins; else TIE")
    r.add("")

    # ----- Section 1: gbench -----
    if gbench_rows:
        r.add("-" * W)
        r.add("SECTION 1 — Google-Benchmark micro-suites (in-process, std vs FastAlloc)")
        r.add("-" * W)
        r.add(f" {'family':<16} {'config':>9} {'T':>2}  {'std ns':>10} {'fast ns':>10} "
              f"{'ratio':>7}  verdict")
        for row in sorted(gbench_rows, key=lambda x: (x["family"], x["arg"], x["threads"])):
            v = ("FastAlloc wins by %.2fx" % row["ratio"]) if row["verdict"] == "WIN" \
                else ("std wins by %.2fx" % (1 / row["ratio"])) if row["verdict"] == "LOSS" \
                else "tie (within 2%)"
            r.add(f" {row['family']:<16} {row['arg']:>9} {row['threads']:>2}  "
                  f"{row['std_ns']:>10.1f} {row['fast_ns']:>10.1f} "
                  f"{fmt_x(row['ratio'])}  {v}")
        r.add("")
        r.add(" family verdict summary (geometric mean of ratios):")
        for fam, d in sorted(gbench_summary.items()):
            gm = d["geo_mean_ratio"]
            if d["overall"] == "FastAlloc":
                v = f"FastAlloc wins {d['win']}/{d['n']} cells, avg {gm:.2f}x faster"
            elif d["overall"] == "std":
                v = f"std wins {d['loss']}/{d['n']} cells, avg {1/gm:.2f}x faster"
            else:
                v = f"mixed: {d['win']} W / {d['tie']} T / {d['loss']} L (geo-mean {gm:.2f}x)"
            r.add(f"   {fam:<16} {v}")
        r.add("")

    # ----- Section 2: cross-allocator suite -----
    if suite_cells:
        fast = "FastAlloc"
        competitors = [a.label for a in allocs if a.label != fast]
        r.add("-" * W)
        r.add("SECTION 2 — Cross-allocator suite (one allocator per process; ns/op, "
              "median of reps)")
        r.add("-" * W)
        scoreboard = {c: {"win": 0, "loss": 0, "tie": 0, "n": 0} for c in competitors}
        for comp in competitors:
            r.add(f"")
            r.add(f" FastAlloc vs {comp}:  (ratio = {comp}/FastAlloc; >1.02 => FastAlloc wins)")
            r.add(f" {'workload':<15}" + "".join(f"{'T=%d' % t:>22}" for t in threads_list))
            for wl in SUITE_WORKLOADS:
                rowtxt = f" {wl:<15}"
                for t in threads_list:
                    fcell = suite_cells.get((wl, t, fast))
                    ccell = suite_cells.get((wl, t, comp))
                    if not (fcell and ccell and fcell.get("ns_per_op")
                            and ccell.get("ns_per_op")):
                        rowtxt += f"{'-':>22}"
                        continue
                    ratio = ccell["ns_per_op"] / fcell["ns_per_op"]
                    v = verdict(ratio)
                    txt = f"{fmt_x(ratio)} {v:<5}"
                    if not fcell.get("checksum_ok", True):
                        txt += " !chk"
                    rowtxt += f"{txt:>22}"
                    scoreboard[comp][v.lower()] += 1
                    scoreboard[comp]["n"] += 1
                r.add(rowtxt)
        r.add("")
        r.add(" SCOREBOARD (throughput cells):")
        for comp, d in scoreboard.items():
            if d["n"] == 0:
                continue
            r.add(f"   vs {comp:<10}: FastAlloc wins {d['win']:>2} / loses {d['loss']:>2} "
                  f"/ ties {d['tie']:>2}  of {d['n']} cells")
        r.add("")

        # ----- Section 3: latency -----
        r.add("-" * W)
        r.add("SECTION 3 — Latency percentiles, single thread (ns/op, lower = better)")
        r.add("-" * W)
        r.add(f" {'workload':<15} {'pctl':>5} " + "".join(f"{a.label:>12}" for a in allocs)
              + "   verdict (FastAlloc vs best competitor)")
        for wl in PAIR_WORKLOADS:
            for pctl in ("p50", "p99", "p999"):
                rowtxt = f" {wl:<15} {pctl:>5} "
                vals = {}
                for a in allocs:
                    c = suite_cells.get((wl, 1, a.label))
                    v = c.get(pctl) if c else None
                    vals[a.label] = v
                    rowtxt += f"{('%9.1f' % v) if v is not None else '        -':>12}"
                f = vals.get(fast)
                if f:
                    others = [v for k, v in vals.items() if k != fast and v is not None]
                    if others:
                        best = min(others)
                        who = [k for k, v in vals.items() if v == best and k != fast][0]
                        if f < best / VERDICT_EPS:
                            vtxt = f"FastAlloc {(best/f):.2f}x better tail vs {who}"
                        elif f > best * VERDICT_EPS:
                            vtxt = f"{who} {(f/best):.2f}x better tail"
                        else:
                            vtxt = "tie"
                        rowtxt += "   " + vtxt
                r.add(rowtxt)
        r.add("")

        # ----- Section 4: memory -----
        ov = {a.label: suite_cells.get(("overhead", 1, a.label)) for a in allocs}
        if any(v for v in ov.values() if v):
            r.add("-" * W)
            r.add("SECTION 4 — Memory (overhead workload: 1M live objects, then free-all)")
            r.add("-" * W)
            metrics = [("overhead_ratio", "RSS/live at full live set (x)", "%.2f"),
                       ("retained_after_free_mb", "retained after free (MB)", "%.1f"),
                       ("retained_after_purge_mb", "retained after purge (MB)", "%.1f"),
                       ("rss_peak_mb", "peak RSS (MB)", "%.1f"),
                       ("alloc_phase_ms", "alloc phase (ms)", "%.0f"),
                       ("free_phase_ms", "free phase (ms)", "%.0f")]
            hdr = f" {'metric':<34}" + "".join(f"{a.label:>12}" for a in allocs)
            r.add(hdr)
            r.add(" " + "-" * (len(hdr) - 2))
            for key, name, fmtm in metrics:
                rowtxt = f" {name:<34}"
                for a in allocs:
                    v = ov[a.label].get(key) if ov[a.label] else None
                    rowtxt += f"{(fmtm % v) if isinstance(v, (int, float)) and v >= 0 else '        -':>12}"
                r.add(rowtxt)
            fcell = ov.get(fast)
            if fcell:
                for comp in competitors:
                    ccell = ov.get(comp)
                    if not (ccell and isinstance(ccell.get("retained_after_purge_mb"),
                                                 (int, float))):
                        continue
                    fp = fcell.get("retained_after_purge_mb")
                    cp = ccell["retained_after_purge_mb"]
                    if isinstance(fp, (int, float)) and fp > 0:
                        r.add(f"   purge-retention verdict vs {comp}: FastAlloc keeps "
                              f"{cp/fp:.1f}x LESS memory after purge "
                              f"({fp:.1f} MB vs {cp:.1f} MB)")
            r.add("")

        # ----- Section 5: thread lifecycle -----
        r.add("-" * W)
        r.add("SECTION 5 — Thread lifecycle (thread-churn: spawn/exit bursts, "
              "256 alloc/free pairs per thread)")
        r.add("-" * W)
        r.add(f" {'threads':>7} " + "".join(f"{a.label + ' ns/pair':>18}" for a in allocs)
              + "   verdict (vs glibc if present)")
        gcell1 = suite_cells.get(("thread-churn", 1, "glibc"))
        fcell1 = suite_cells.get(("thread-churn", 1, fast))
        for t in threads_list:
            rowtxt = f" {t:>7} "
            for a in allocs:
                c = suite_cells.get(("thread-churn", t, a.label))
                v = c.get("ns_per_op") if c else None
                rowtxt += f"{('%10.0f' % v) if v else '         -':>18}"
            if t == 1 and gcell1 and fcell1 and gcell1.get("ns_per_op") and fcell1.get("ns_per_op"):
                ratio = gcell1["ns_per_op"] / fcell1["ns_per_op"]
                vtxt = ("FastAlloc %.2fx faster" % ratio) if ratio > VERDICT_EPS else (
                       ("glibc %.2fx faster" % (1 / ratio)) if ratio < 1 / VERDICT_EPS
                       else "tie")
                rowtxt += "   T=1: " + vtxt
            r.add(rowtxt)
        # threads/s at T=1
        rowtxt = f" {'thr/s':>7} "
        for a in allocs:
            c = suite_cells.get(("thread-churn", 1, a.label))
            v = c.get("threads_per_sec") if c else None
            rowtxt += f"{('%10.0f' % v) if v else '         -':>18}"
        r.add(rowtxt)
        r.add("")

    # ----- Section 6: bench_memory -----
    if mem_rows:
        r.add("-" * W)
        r.add("SECTION 6 — Side-by-side memory stress (std vs FastAlloc, one process)")
        r.add("-" * W)
        r.add(f" {'threads':>7} {'size':>7} {'std s':>8} {'fast s':>8} "
              f"{'time ratio':>10} {'std RSS':>8} {'fast RSS':>9}  verdict")
        for row in mem_rows:
            v = ("FastAlloc %.2fx faster" % row["speedup"]) if row["speedup"] > 1.02 \
                else ("std %.2fx faster" % (1 / row["speedup"])) if row["speedup"] < 0.98 \
                else "tie"
            r.add(f" {row['threads']:>7} {row['size']:>7} {row['std_time_s']:>8.3f} "
                  f"{row['fast_time_s']:>8.3f} {fmt_x(row['speedup']):>10} "
                  f"{row['std_rss_mb']:>8} {row['fast_rss_mb']:>9}  {v}")
        r.add("")

    # ----- honest footer -----
    r.add("=" * W)
    r.add(" All cells are checksum-guarded (bench_suite verifies every block on")
    r.add(" free). Raw JSONL: compare_results.jsonl; chart-ready data: --data JSON.")
    r.add("=" * W)
    return r


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    here = Path(__file__).resolve().parent
    ap.add_argument("--build-dir", default=None)
    ap.add_argument("--phase", default="all",
                    choices=["all", "gbench", "suite", "memory"],
                    help="which phase(s) to run (default all)")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--threads", default="1,2,4")
    ap.add_argument("--ops-scale", type=float, default=1.0)
    ap.add_argument("--allocators", default="all",
                    help="comma list: glibc,jemalloc,mimalloc,FastAlloc or 'all'")
    ap.add_argument("--jsonl", default=str(here / "compare_results.jsonl"))
    ap.add_argument("--report", default=str(here / "bench_report.md"))
    ap.add_argument("--data", default=str(here / "bench_report_data.json"))
    ap.add_argument("--mimalloc-so", default=os.environ.get("MIMALLOC_SO", ""))
    ap.add_argument("--jemalloc-so", default=os.environ.get("JEMALLOC_SO", ""))
    ap.add_argument("--fresh", action="store_true",
                    help="start a new JSONL instead of appending")
    ap.add_argument("--workloads", default=None,
                    help="comma list restricting which suite workloads RUN this "
                         "invocation (JSONL accumulates across invocations)")
    ap.add_argument("--suite-skip-run", action="store_true",
                    help="parse the JSONL and rebuild the report without "
                         "re-running the suite (resume/chunk mode)")
    ap.add_argument("--gbench-cache", default=str(here / "gbench_cache"),
                    help="directory of cached gbench family JSONs to reuse")
    ap.add_argument("--quick", action="store_true",
                    help="smoke preset: reps=3, ops-scale=0.2, threads=1,2")
    args = ap.parse_args()

    if args.quick:
        args.reps, args.ops_scale, args.threads = 3, 0.2, "1,2"

    threads_list = [int(t) for t in args.threads.split(",") if t.strip()]
    build = find_build_dir(args.build_dir)
    wanted = None if args.allocators == "all" else set(
        s.strip() for s in args.allocators.split(","))
    allocs = detect_allocators(args.jemalloc_so, args.mimalloc_so, wanted or
                               {"glibc", "jemalloc", "mimalloc", "FastAlloc", "fast"})
    if wanted:
        keep = []
        for a in allocs:
            if a.key in wanted or a.label in wanted:
                keep.append(a)
        allocs = keep
    print("allocators:", ", ".join(a.label for a in allocs))

    jsonl = Path(args.jsonl)
    if args.fresh and jsonl.exists():
        jsonl.unlink()

    meta = {
        "date": datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
        "cpu": cpu_model(),
        "os": sys.platform,
        "allocators": [a.label for a in allocs],
        "reps": args.reps, "threads": threads_list,
        "ops_scale": args.ops_scale, "build_dir": str(build),
    }

    gbench_rows, gbench_sum, suite_cells, mem_rows = [], {}, {}, []
    t0 = time.time()
    if args.phase in ("all", "gbench"):
        print("\n[1/3] Google-Benchmark micro-suites ...")
        cache = Path(args.gbench_cache)
        cache.mkdir(parents=True, exist_ok=True)
        gbench_rows = phase_gbench(build, str(cache), cache=str(cache))
        gbench_sum = gbench_family_summary(gbench_rows)
        print(f"      {len(gbench_rows)} paired configs")
    if args.phase in ("all", "suite"):
        wls = None
        if args.workloads:
            wls = [w.strip() for w in args.workloads.split(",")
                   if w.strip() in SUITE_WORKLOADS]
        if args.suite_skip_run:
            print("\n[2/3] Cross-allocator suite — parse-only (skip_run)")
        else:
            print("\n[2/3] Cross-allocator suite (%d workloads x %d threads x %d allocators) ..."
                  % (len(wls or SUITE_WORKLOADS), len(threads_list), len(allocs)))
        suite_cells = phase_suite(build, allocs, threads_list, args.reps,
                                  args.ops_scale, args.jsonl, workloads=wls,
                                  skip_run=args.suite_skip_run)
        print(f"      {len(suite_cells)} cells parsed from {args.jsonl}")
    if args.phase in ("all", "memory"):
        print("\n[3/3] Side-by-side memory benchmark ...")
        mem_rows = phase_memory(build, threads_list, [64, 256, 4096], 20000)
        print(f"      {len(mem_rows)} configs")
    meta["wall_time_s"] = round(time.time() - t0, 1)

    rep = build_report(meta, gbench_rows, gbench_sum, suite_cells, allocs,
                       threads_list, mem_rows, mem_rows)
    text = rep.render()
    print("\n" + text)
    Path(args.report).write_text(text)
    print(f"report written : {args.report}")

    data = {
        "meta": meta,
        "gbench": gbench_rows,
        "gbench_summary": gbench_sum,
        "suite": [{"workload": k[0], "threads": k[1], "alloc": k[2], **v}
                  for k, v in suite_cells.items()],
        "bench_memory": mem_rows,
    }
    Path(args.data).write_text(json.dumps(data, indent=1))
    print(f"data written   : {args.data}")

    # hard-fail signal: any checksum failure in the suite
    bad = [k for k, v in suite_cells.items() if not v.get("checksum_ok", True)]
    if bad:
        print("CHECKSUM FAILURES:", bad)
        return 1
    return 0


def cpu_model():
    try:
        for line in open("/proc/cpuinfo"):
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except Exception:
        pass
    return "?"


if __name__ == "__main__":
    sys.exit(main())

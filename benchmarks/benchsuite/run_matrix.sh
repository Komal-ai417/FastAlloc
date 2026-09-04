#!/usr/bin/env bash
# ============================================================================
# run_matrix.sh — FastAlloc cross-allocator benchmark matrix driver
# ----------------------------------------------------------------------------
# Usage:  ./run_matrix.sh [jsonl_out] [reps]
# Env:    BENCH_SUITE  (path to the bench_suite binary; default: ../../build*/bench_suite)
#         JEMALLOC_SO  (path to libjemalloc; default: system libjemalloc.so.2)
#         MIMALLOC_SO  (path to libmimalloc;  default: skip mimalloc if absent)
#
# Methodology safeguards:
#   * one allocator per process (RSS / ru_maxrss isolation)
#   * allocator launch order ROTATES per workload (drift decorrelation)
#   * every (allocator, workload, threads) cell: warmup + N timed reps,
#     analyzer takes the median; every block checksum-verified on free
#   * crashed configs retried once; persistent failures recorded
#
# Suggested analyzer: python3 analyze.py results.jsonl
# ============================================================================
set -u
BENCH="${BENCH_SUITE:-$(ls -1 ../*/bench_suite ../build*/bench_suite ./bench_suite 2>/dev/null | head -1)}"
OUT="${1:-bench_results.jsonl}"
REPS="${2:-5}"
JEM="${JEMALLOC_SO:-/lib/x86_64-linux-gnu/libjemalloc.so.2}"
MI="${MIMALLOC_SO:-}"

if [ -z "$BENCH" ] || [ ! -x "$BENCH" ]; then
    echo "bench_suite binary not found; build it first:" >&2
    echo "  cmake -B build -DFASTALLOC_BUILD_BENCHMARKS=ON && cmake --build build --target bench_suite" >&2
    exit 1
fi
[ -n "$MI" ] && [ ! -f "$MI" ] && { echo "MIMALLOC_SO=$MI not found; unset to skip" >&2; exit 1; }

touch "$OUT"
have() { grep -q "\"label\":\"$1\",\"workload\":\"$2\",\"threads\":$3," "$OUT"; }

run_one() {  # alloc label preload workload threads
    local alloc="$1" label="$2" preload="$3" wl="$4" th="$5"
    have "$label" "$wl" "$th" && return 0
    echo ">>> $label $wl T=$th"
    local rc=0 attempt
    for attempt in 1 2; do
        if [ -n "$preload" ]; then
            LD_PRELOAD="$preload" "$BENCH" --alloc "$alloc" --label "$label" \
                --workload "$wl" --threads "$th" --reps "$REPS" --json "$OUT"
        else
            "$BENCH" --alloc "$alloc" --label "$label" \
                --workload "$wl" --threads "$th" --reps "$REPS" --json "$OUT"
        fi
        rc=$?
        [ $rc -eq 0 ] && break
        echo "  !! attempt $attempt failed rc=$rc (42=checksum fail)" >&2
    done
    [ $rc -ne 0 ] && echo "$label $wl T=$th rc=$rc" >> matrix_failures.txt
    return 0
}

WORKLOADS=(tiny small-mixed random-1-4096 ramp churn cache-thrash cross-thread thread-churn large realloc-grow overhead)
ALLOCS=(glibc jemalloc mimalloc fast)   # mimalloc auto-skipped if MIMALLOC_SO empty
IDX=0
for wl in "${WORKLOADS[@]}"; do
    for th in 1 2 4; do
        case $(( IDX % 4 )) in   # rotate start order per workload
            0) ORDER=(glibc jemalloc mimalloc fast) ;;
            1) ORDER=(jemalloc mimalloc fast glibc) ;;
            2) ORDER=(mimalloc fast glibc jemalloc) ;;
            3) ORDER=(fast glibc jemalloc mimalloc) ;;
        esac
        for a in "${ORDER[@]}"; do
            case $a in
                glibc)    run_one std glibc    ""      "$wl" "$th" ;;
                jemalloc) run_one std jemalloc "$JEM"  "$wl" "$th" ;;
                mimalloc) [ -n "$MI" ] && run_one std mimalloc "$MI" "$wl" "$th" ;;
                fast)     run_one fast FastAlloc ""     "$wl" "$th" ;;
            esac
        done
    done
    IDX=$((IDX + 1))
done
echo "done: $(wc -l < "$OUT") records in $OUT"
[ -s matrix_failures.txt ] && { echo "FAILURES:"; cat matrix_failures.txt; }
exit 0

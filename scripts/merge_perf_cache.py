#!/usr/bin/env python3
# Turn perf_cache.sh's raw counter CSVs into a derived-metrics CSV.
#
# ============================================================================
# HOW TO RUN
# ============================================================================
# Run from inside scripts/ (paths are relative to it).
#
#   ./merge_perf_cache.py <log_name> [<log_name> ...]
#
# Each <log_name> is a LOG_NAME you passed to perf_cache.sh, i.e. there must be
# a data/<log_name>/perf_cache.csv. Output:
#
#   data/<log_name>/perf_cache_metrics.csv     one row per (region, tree)
#   data/perf_cache_metrics_all.csv            every log_name concatenated,
#                                              with a source column
#
# --- recipes -----------------------------------------------------------------
#   # single sweep
#   cd scripts && ./merge_perf_cache.py perf_cache_1e9
#
#   # combine the 1e8 and 1e9 sweeps into one comparable table
#   cd scripts && ./merge_perf_cache.py perf_cache_1e8_all perf_cache_1e9
#
#   # print to the terminal as an aligned table as well as writing the csv
#   cd scripts && ./merge_perf_cache.py -v perf_cache_1e9
#
# --- row order ---------------------------------------------------------------
# Rows are grouped by distribution first, then operation (build, insert,
# delete), then sweep, then tree -- so every tree for a given
# (distribution, operation) sits together and can be read off against the
# others. perf_cache.sh writes its raw csv in run order instead (tree
# outermost), which interleaves the distributions.
#
# --- what it computes --------------------------------------------------------
# region_n       elements the counted region actually processed. This is the
#                important one, and it differs per region: `build`,
#                `incre_insert` and `incre_delete` touch all n, `insert`/
#                `delete` only the batch (n*batch_ratio), and `membership`
#                probes n*0.01 keys (kMemberQueryRatio).
#                `membership` is the half-half probe -- half the keys are in the
#                tree, half are not.
#                For `incre_insert`/`incre_delete` the counted region is the
#                whole chain of 1/batch_ratio batches, so batch_ratio is the
#                step size and the divisor stays n. Do NOT read those rows as
#                comparable to `insert`/`delete`: one builds (or empties) the
#                entire tree, the other adds one batch to a finished tree.
# <ev>_per_elem  each event divided by region_n.
# miss_rate_pct  100 * cache-misses / cache-references. NOTE this is an *LLC*
#                miss rate, not "fraction of memory accesses that miss":
#                perf's `cache-references`/`cache-misses` are the generic
#                PERF_TYPE_HARDWARE aliases (config 0x2/0x3), which Intel maps
#                to LONGEST_LAT_CACHE.REFERENCE/.MISS. The denominator is only
#                the accesses that already missed L1+L2 and reached the LLC, so
#                it excludes everything L2 absorbed. Prefer cache-misses_per_elem
#                for ranking -- the rate has a tree-dependent denominator and can
#                invert the ranking (at 1e9/b=1%, PMA insert shows a higher rate
#                than CPAM while having fewer absolute misses).
# ipc            instructions / cycles. Worth reading next to the miss columns:
#                misses only explain the cycle cost when the operation is
#                latency-bound. Point queries run at IPC 0.15-0.39 (dependent
#                load chains, nothing to overlap) so misses do track cycles
#                there, while batch updates run at 0.8-1.1 and can be dominated
#                by instruction count instead -- CPAM's exp delete has 6.9%
#                FEWER misses than BTree yet is 10% slower, because it executes
#                46.7% more instructions.
# vs_btree       cache-misses relative to BTree in the same
#                (source, distribution, region). 1.00 for BTree itself; blank if
#                no BTree row exists for that distribution+region.
#
# Counts are per single execution -- perf_cache.sh pins -r 1 for that reason.
# Treat cache-miss differences under ~20% as unresolved from one run: measured
# spread at fixed config is ~14% on miss columns vs ~4% on instructions.
# ============================================================================

import csv
import sys
from pathlib import Path

EVENTS = [
    "cycles",
    "instructions",
    "cache-references",
    "cache-misses",
    "LLC-load-misses",
    "L1-dcache-load-misses",
]

# Per-element denominators differ by region:
#   build, incre_insert, incre_delete   all n
#   insert, delete                      n * batch_ratio  (just the batch)
#   membership                          n * kMemberQueryRatio
#
# incre_insert / incre_delete are the ones to watch. The counted region is the
# WHOLE chain of 1/batch_ratio BatchInsert (or BatchDelete) calls, which between
# them walk every one of the n entries -- so batch_ratio there is the step size,
# not the amount of work, and the divisor is n no matter what it is set to.
# Keep MEMBER_QUERY_RATIO in sync with kMemberQueryRatio in
# tests/test_framework.h.
WHOLE_INPUT_REGIONS = {"build", "incre_insert", "incre_delete"}
MEMBER_QUERY_REGIONS = {"membership"}
MEMBER_QUERY_RATIO = 0.01

# Row order: distribution first, then operation, then sweep, then tree -- so all
# rows for one (distribution, operation) sit together and the trees can be read
# off against each other. perf_cache.sh emits rows in run order instead (tree
# outermost), which interleaves the distributions.
DIS_ORDER = ["uni", "exp", "norm", "dual_norm"]
# NOTE: build -> membership -> incre_insert -> incre_delete is the profiling
# order the sweep is run in; the legacy batch regions trail it.
REGION_ORDER = [
    "build",
    "membership",
    "incre_insert",
    "incre_delete",
    "insert",
    "delete",
]
TREE_ORDER = ["BTree", "CPAM", "PAM", "PMA"]


def _rank(value, order):
    """Position in `order`; anything unlisted sorts after, alphabetically."""
    try:
        return (0, order.index(value))
    except ValueError:
        return (1, value)


def sort_rows(rows):
    return sorted(
        rows,
        key=lambda r: (
            _rank(r["dis"], DIS_ORDER),
            _rank(r["region"], REGION_ORDER),
            r["source"],
            _rank(r["tree"], TREE_ORDER),
        ),
    )


def region_elements(region, n, batch_ratio):
    if region in WHOLE_INPUT_REGIONS:
        return n
    if region in MEMBER_QUERY_REGIONS:
        return int(round(n * MEMBER_QUERY_RATIO))
    return int(round(n * batch_ratio))


def derive(rows, source):
    out = []
    # BTree's cache-misses for the vs_btree column. MUST be keyed by
    # (dis, region), not region alone -- with more than one distribution in a
    # sweep, a region-only key lets the last distribution's BTree row overwrite
    # the others, and every row then gets compared against the wrong baseline
    # (BTree vs itself came out as 0.988 and 3.221 instead of 1.000).
    baseline = {
        (r["dis"], r["region"]): int(r["cache-misses"])
        for r in rows
        if r["tree"] == "BTree"
    }
    for r in rows:
        n = int(r["n"])
        ratio = float(r["batch_ratio"])
        region_n = region_elements(r["region"], n, ratio)
        cm = int(r["cache-misses"])
        cr = int(r["cache-references"])
        rec = {
            "source": source,
            "region": r["region"],
            "tree": r["tree"],
            "dis": r["dis"],
            "n": n,
            "batch_ratio": ratio,
            "threads": int(r["threads"]),
            # NOTE: .get -- csvs written before the numa column existed lack it.
            "numa": r.get("numa", ""),
            "region_n": region_n,
        }
        for ev in EVENTS:
            rec[ev] = int(r[ev])
        for ev in EVENTS:
            rec[f"{ev}_per_elem"] = round(int(r[ev]) / region_n, 4)
        cy = int(r["cycles"])
        rec["ipc"] = round(int(r["instructions"]) / cy, 3) if cy else ""
        rec["miss_rate_pct"] = round(100.0 * cm / cr, 2) if cr else ""
        base = baseline.get((r["dis"], r["region"]))
        rec["vs_btree"] = round(cm / base, 3) if base else ""
        out.append(rec)
    return out


def main(argv):
    verbose = "-v" in argv
    names = [a for a in argv if not a.startswith("-")]
    if not names:
        print(__doc__ or "usage: ./merge_perf_cache.py <log_name> ...")
        print("usage: ./merge_perf_cache.py [-v] <log_name> [<log_name> ...]")
        return 1

    fields = (
        ["source", "region", "tree", "dis", "n", "batch_ratio", "threads", "numa", "region_n"]
        + EVENTS
        + [f"{e}_per_elem" for e in EVENTS]
        + ["ipc", "miss_rate_pct", "vs_btree"]
    )

    combined = []
    for name in names:
        src = Path("data") / name / "perf_cache.csv"
        if not src.is_file():
            print(f"!!! missing {src} -- skipping {name}", file=sys.stderr)
            continue
        rows = list(csv.DictReader(open(src)))
        if not rows:
            print(f"!!! {src} has no data rows -- skipping", file=sys.stderr)
            continue
        derived = sort_rows(derive(rows, name))
        combined += derived

        dest = src.parent / "perf_cache_metrics.csv"
        with open(dest, "w", newline="") as fp:
            # NOTE: lineterminator -- csv defaults to CRLF, which leaves a \r on
            # the last field for awk/cut and breaks comparisons against it.
            w = csv.DictWriter(fp, fieldnames=fields, lineterminator="\n")
            w.writeheader()
            w.writerows(derived)
        print(f"wrote {dest}  ({len(derived)} rows)")

    if not combined:
        return 1

    combined = sort_rows(combined)

    if len(names) > 1:
        alldest = Path("data") / "perf_cache_metrics_all.csv"
        with open(alldest, "w", newline="") as fp:
            # NOTE: lineterminator -- csv defaults to CRLF, which leaves a \r on
            # the last field for awk/cut and breaks comparisons against it.
            w = csv.DictWriter(fp, fieldnames=fields, lineterminator="\n")
            w.writeheader()
            w.writerows(combined)
        print(f"wrote {alldest}  ({len(combined)} rows)")

    if verbose:
        hdr = (
            f"{'dis':10} {'region':13} {'tree':6} {'region_n':>13} "
            f"{'cache-misses':>15} {'/elem':>8} {'miss%':>7} {'IPC':>6} "
            f"{'vs BTree':>9}"
        )
        print("\n" + hdr)
        print("-" * len(hdr))
        prev = None
        for r in combined:
            # blank line between (dis, region) groups so the grouping is visible
            key = (r["dis"], r["region"])
            if prev is not None and key != prev:
                print()
            prev = key
            print(
                f"{r['dis']:10} {r['region']:13} {r['tree']:6} {r['region_n']:13,} "
                f"{r['cache-misses']:15,} {r['cache-misses_per_elem']:8.2f} "
                f"{r['miss_rate_pct']:6}% {r['ipc']:>6} {r['vs_btree']:>9}"
            )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

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
# --- what it computes --------------------------------------------------------
# region_n       elements the counted region actually processed. This is the
#                important one: `build` touches all n, but `insert`/`delete`
#                only touch the batch, so their denominator is n*batch_ratio.
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
# vs_btree       cache-misses relative to BTree in the same (source, region).
#                1.00 for BTree itself; blank if no BTree row exists.
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

# The counted region for insert/delete only processes the batch, not the tree.
WHOLE_INPUT_REGIONS = {"build"}


def region_elements(region, n, batch_ratio):
    if region in WHOLE_INPUT_REGIONS:
        return n
    return int(round(n * batch_ratio))


def derive(rows, source):
    out = []
    # cache-misses of BTree, keyed by region, for the vs_btree column
    baseline = {
        r["region"]: int(r["cache-misses"]) for r in rows if r["tree"] == "BTree"
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
            "region_n": region_n,
        }
        for ev in EVENTS:
            rec[ev] = int(r[ev])
        for ev in EVENTS:
            rec[f"{ev}_per_elem"] = round(int(r[ev]) / region_n, 4)
        rec["miss_rate_pct"] = round(100.0 * cm / cr, 2) if cr else ""
        base = baseline.get(r["region"])
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
        ["source", "region", "tree", "dis", "n", "batch_ratio", "threads", "region_n"]
        + EVENTS
        + [f"{e}_per_elem" for e in EVENTS]
        + ["miss_rate_pct", "vs_btree"]
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
        derived = derive(rows, name)
        combined += derived

        dest = src.parent / "perf_cache_metrics.csv"
        with open(dest, "w", newline="") as fp:
            w = csv.DictWriter(fp, fieldnames=fields)
            w.writeheader()
            w.writerows(derived)
        print(f"wrote {dest}  ({len(derived)} rows)")

    if not combined:
        return 1

    if len(names) > 1:
        alldest = Path("data") / "perf_cache_metrics_all.csv"
        with open(alldest, "w", newline="") as fp:
            w = csv.DictWriter(fp, fieldnames=fields)
            w.writeheader()
            w.writerows(combined)
        print(f"wrote {alldest}  ({len(combined)} rows)")

    if verbose:
        hdr = f"{'source':22} {'region':7} {'tree':6} {'region_n':>13} {'cache-misses':>15} {'/elem':>8} {'miss%':>7} {'vs BTree':>9}"
        print("\n" + hdr)
        print("-" * len(hdr))
        for r in combined:
            print(
                f"{r['source']:22} {r['region']:7} {r['tree']:6} {r['region_n']:13,} "
                f"{r['cache-misses']:15,} {r['cache-misses_per_elem']:8.2f} "
                f"{r['miss_rate_pct']:6}% {r['vs_btree']:>9}"
            )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

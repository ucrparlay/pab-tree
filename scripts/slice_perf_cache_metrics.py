#!/usr/bin/env python3
from __future__ import annotations

import csv
import sys
from pathlib import Path


# Usage:
#   python3 scripts/slice_perf_cache_metrics.py
#   python3 scripts/slice_perf_cache_metrics.py <input_csv> [output_csv]
#
# Defaults:
#   input  = scripts/data/perf_1e9_b001_uni_exp/perf_cache_metrics.csv
#   output = scripts/data/perf_1e9_b001_uni_exp/perf_cache_metrics_sliced.csv
#
# Pivots merge_perf_cache.py's long-format CSV into one row per
# (dis, tree), with a column per (region, metric). Counters are scaled to
# millions.
#
# Both region namings are handled: build/insert/delete/membership (perf_cache
# sweeps) and build/incre_insert/incre_delete/membership (perf_incre sweeps).
# Only the regions actually present in the input become columns, so neither
# sweep produces empty ones.

DEFAULT_INPUT = Path("scripts/data/perf_1e9_b001_uni_exp/perf_cache_metrics.csv")
DEFAULT_OUTPUT = Path("scripts/data/perf_1e9_b001_uni_exp/perf_cache_metrics_sliced.csv")
UNIT = 1_000_000.0

INPUT_COLUMNS = [
    "dis",
    "region",
    "tree",
    "cycles",
    "instructions",
    "cache-references",
    "cache-misses",
]
METRICS = [
    ("cycles", "cycles"),
    ("instructions", "ins"),
    ("cache-references", "cache_ref"),
    ("cache-misses", "cache"),
]

REGION_ORDER = [
    "build",
    "insert",
    "delete",
    "incre_insert",
    "incre_delete",
    "membership",
]
DIS_ORDER = ["uni", "exp", "norm", "seg"]
TREE_ORDER = ["btree", "pam", "cpam", "pma"]


def to_scaled_unit(value: str) -> str:
    if value is None or value == "":
        return ""
    scaled = float(value) / UNIT
    # Keep compact numeric text while preserving useful precision.
    return f"{scaled:.6f}".rstrip("0").rstrip(".")


def ordered_rank(value: str, order: list[str]) -> tuple[int, int | str]:
    lower = value.lower()
    if lower in order:
        return (0, order.index(lower))
    return (1, lower)


def main() -> int:
    input_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_INPUT
    output_path = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_OUTPUT

    with input_path.open(newline="") as fin:
        reader = csv.DictReader(fin)
        missing = [c for c in INPUT_COLUMNS if c not in (reader.fieldnames or [])]
        if missing:
            raise ValueError(f"Missing columns in source CSV: {missing}")

        rows = {}
        seen_regions = set()
        for row in reader:
            region = row["region"]
            seen_regions.add(region)
            key = (row["dis"], row["tree"])
            if key not in rows:
                rows[key] = {"dis": row["dis"], "tree": row["tree"]}
            for metric_in, metric_out in METRICS:
                rows[key][f"{region}_{metric_out}"] = to_scaled_unit(row[metric_in])

        regions = sorted(seen_regions, key=lambda r: ordered_rank(r, REGION_ORDER))
        output_columns = ["dis", "tree"] + [
            f"{region}_{metric_out}" for region in regions for _, metric_out in METRICS
        ]

        output_path.parent.mkdir(parents=True, exist_ok=True)
        with output_path.open("w", newline="") as fout:
            writer = csv.DictWriter(fout, fieldnames=output_columns, lineterminator="\n")
            writer.writeheader()
            for key in sorted(
                rows.keys(),
                key=lambda k: (
                    ordered_rank(k[0], DIS_ORDER),
                    ordered_rank(k[1], TREE_ORDER),
                ),
            ):
                rec = rows[key]
                writer.writerow({col: rec.get(col, "") for col in output_columns})

    print(f"Written: {output_path}  ({len(rows)} rows, regions: {', '.join(regions)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

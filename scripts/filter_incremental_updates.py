#!/usr/bin/env python3
import csv
import sys
from pathlib import Path


# Usage:
#   python3 scripts/filter_incremental_updates.py
#   python3 scripts/filter_incremental_updates.py <input_csv> [output_csv]
#
# Defaults:
#   input  = scripts/data/incremental_updates_new/incremental_updates_new.csv
#   output = scripts/data/incremental_updates_new/incremental_updates_new_filtered.csv
#
# The default input is merge_incre_updates_new.py's 36-value CSV (uni, norm and
# seg; one BTree row per leaf size run).  The columns below are a subset of both
# that layout and the older 90-value one that merge_incre_updates.py writes, so
# either can be passed explicitly -- pass the old CSV's path to filter it.
#
# build keeps only its timing; the build block's MIX/RRM/RCM are dropped, so the
# only query columns are the 1% blocks'.

DEFAULT_INPUT = Path("scripts/data/incremental_updates_new/incremental_updates_new.csv")
DEFAULT_OUTPUT = Path(
    "scripts/data/incremental_updates_new/incremental_updates_new_filtered.csv"
)

KEEP_COLUMNS = [
    "benchmark",
    "tree",
    "leaf_wrap",
    "build",
    "ins_0.01%",
    "ins_0.1%",
    "ins_1%",
    "ins_10%",
    "ins_1%_MIX",
    "ins_1%_RRM",
    "ins_1%_RCM",
    "del_0.01%",
    "del_0.1%",
    "del_1%",
    "del_10%",
    "del_1%_MIX",
    "del_1%_RRM",
    "del_1%_RCM",
]


def main() -> int:
    input_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_INPUT
    output_path = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_OUTPUT

    with input_path.open(newline="") as fin:
        reader = csv.DictReader(fin)
        missing = [c for c in KEEP_COLUMNS if c not in (reader.fieldnames or [])]
        if missing:
            raise ValueError(f"Missing columns in source CSV: {missing}")

        output_path.parent.mkdir(parents=True, exist_ok=True)
        with output_path.open("w", newline="") as fout:
            writer = csv.DictWriter(fout, fieldnames=KEEP_COLUMNS)
            writer.writeheader()
            for row in reader:
                writer.writerow({k: row[k] for k in KEEP_COLUMNS})

    print(f"Written: {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

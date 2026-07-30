#!/usr/bin/env python3
import csv
import sys
from pathlib import Path


# Usage:
#   python3 scripts/filter_incremental_updates.py
#   python3 scripts/filter_incremental_updates.py <input_csv> [output_csv]
#
# Defaults:
#   input  = scripts/data/incremental_updates/incremental_updates.csv
#   output = scripts/data/incremental_updates/incremental_updates_filtered.csv

DEFAULT_INPUT = Path("scripts/data/incremental_updates/incremental_updates.csv")
DEFAULT_OUTPUT = Path("scripts/data/incremental_updates/incremental_updates_filtered.csv")

KEEP_COLUMNS = [
    "benchmark",
    "tree",
    "build",
    "build_MIX",
    "build_RRM",
    "build_RCM",
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

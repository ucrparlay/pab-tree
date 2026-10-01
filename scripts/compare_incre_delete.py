#!/usr/bin/env python3
"""Graft delete vs repair delete vs CPAM on the incremental-update sweep.

Usage:  ./compare_incre_delete.py [<graft_rev> [<repair_rev>]]

Reads   logs/incremental_updates_new/{0,1}/<dis>/64.log at two git revisions
        (0 is BTree, 1 is CPAM)
Writes  data/incremental_updates_new/incre_updates_new_graft_vs_repair.csv

The repair run exists only in history, so both sides are read with `git show`.

Rows per distribution: BTree-graft and CPAM from <graft_rev>, BTree-repair from
<repair_rev>, then the time ratios a/b (above 1: b is faster).  Columns are
filter_incremental_updates.py's.

Build, insert and query code is the same at both revisions, so those columns
of repair/graft are a control: they show how far the two sessions drifted.
"""
import csv
import subprocess
import sys
from pathlib import Path
from typing import Dict, List

from filter_incremental_updates import KEEP_COLUMNS
from merge_incre_updates_new import (
    display_bench,
    header_36,
    kIdCols,
    kWantBench,
    log_folder_name,
)

kGraftRev = "92886ba"
kRepairRev = "1bf5576"

kLeafWrap = 64

kScripts = Path(__file__).resolve().parent
kRepo = kScripts.parent
kStorePath = (
    kScripts / "data" / log_folder_name / "incre_updates_new_graft_vs_repair.csv"
)

# what a/b rows to add under the runs
kRatios = [
    ("BTree-repair", "BTree-graft"),
    ("CPAM", "BTree-graft"),
    ("CPAM", "BTree-repair"),
]

kValueCols = [c for c in KEEP_COLUMNS if c not in kIdCols]


def read_run(rev: str, tree_dir: int, tree: str, dis: str) -> Dict[str, str]:
    """One run's 36 values by column name, from the log as of rev."""
    path = f"scripts/logs/{log_folder_name}/{tree_dir}/{dis}/{kLeafWrap}.log"
    # spelled for python 3.6, which is what `env python3` finds under bash here
    text = subprocess.run(
        ["git", "-C", str(kRepo), "show", f"{rev}:{path}"],
        check=True,
        stdout=subprocess.PIPE,
        universal_newlines=True,
    ).stdout
    head, _, body = text.partition("\n")
    # the merger trusts the header, not the path; make sure they agree
    want = f"Tree: {tree}; Buf: {kLeafWrap}; InputFile: 1.batch.bin; Dis: {dis};"
    if head.strip() != want:
        raise ValueError(f"{rev}:{path}: header is '{head.strip()}', not '{want}'")
    values = body.split()
    if len(values) != len(header_36):
        raise ValueError(
            f"{rev}:{path}: {len(values)} values, expected {len(header_36)}"
        )
    return dict(zip(header_36, values))


def ratio(a: str, b: str) -> str:
    try:
        return f"{float(a) / float(b):.2f}"
    except (ValueError, ZeroDivisionError):
        return ""


def short(label: str) -> str:
    return label.split("-")[-1]


def main() -> int:
    graft_rev = sys.argv[1] if len(sys.argv) > 1 else kGraftRev
    repair_rev = sys.argv[2] if len(sys.argv) > 2 else kRepairRev

    # (label, tree dir, tree name in the log header, revision)
    runs = [
        ("BTree-graft", 0, "BTree", graft_rev),
        ("BTree-repair", 0, "BTree", repair_rev),
        ("CPAM", 1, "CPAM", graft_rev),
    ]

    rows: List[List[str]] = []
    for dis in kWantBench:
        bench = display_bench(dis)
        by_label = {
            label: read_run(rev, tree_dir, tree, dis)
            for label, tree_dir, tree, rev in runs
        }
        for label, _, _, _ in runs:
            rows.append(
                [bench, label, kLeafWrap] + [by_label[label][c] for c in kValueCols]
            )
        for a, b in kRatios:
            rows.append(
                [bench, f"{short(a)}/{short(b)}", kLeafWrap]
                + [ratio(by_label[a][c], by_label[b][c]) for c in kValueCols]
            )

    kStorePath.parent.mkdir(parents=True, exist_ok=True)
    with kStorePath.open("w", newline="") as fout:
        writer = csv.writer(fout)
        writer.writerow(KEEP_COLUMNS)
        writer.writerows(rows)

    print(
        f"Written: {kStorePath.relative_to(kRepo)}  ({len(rows)} rows, "
        f"graft {graft_rev}, repair {repair_rev})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

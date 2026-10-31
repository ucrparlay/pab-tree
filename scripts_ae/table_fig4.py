#!/usr/bin/env python3
"""Fig. 4 as a table, in seconds.

HOW TO RUN (from scripts_ae/, after the merge step)
    python3 table_fig4.py [data/incremental_updates_new/incre_updates_new.csv]

Writes plots/fig4_synthetic.csv: one row per distribution and tree with the
build; incremental insertion in batches of 0.01%, 0.1%, 1% and 10%; Find and
Range Report on the tree the 1% insertion left half full; then the same for
deletion. PaB-tree is the leaf-64 run.
"""
import csv
import sys
from pathlib import Path

src = Path(sys.argv[1] if len(sys.argv) > 1 else
           "data/incremental_updates_new/incre_updates_new.csv")
dst = Path("plots/fig4_synthetic.csv")

DISTS = {"uni": "Uniform", "skew": "Skewed", "adversary": "Adversarial"}
TREES = {"BTree": "PaB-tree", "PAM": "PAM", "CPAM": "CPAM", "PMA": "PMA",
         "MassTree": "Mass-tree", "BwTree": "Bw-tree"}
RATIOS = ["0.01%", "0.1%", "1%", "10%"]
# the paper's column names; a group name and its sub-column in one cell
COLUMNS = ([("Build", "build")]
           + [(f"Incremental Insert {r}", f"ins_{r}") for r in RATIOS]
           + [("Query after Inc. Ins. Find", "ins_1%_MIX"),
              ("Query after Inc. Ins. Range Report", "ins_1%_RRM")]
           + [(f"Incremental Delete {r}", f"del_{r}") for r in RATIOS]
           + [("Query after Inc. Del. Find", "del_1%_MIX"),
              ("Query after Inc. Del. Range Report", "del_1%_RRM")])

rows = list(csv.DictReader(src.open()))
table = []
for dist, dist_name in DISTS.items():
    for tree, tree_name in TREES.items():
        for r in rows:
            if (r["benchmark"] == dist and r["tree"] == tree
                    and (tree != "BTree" or r["leaf_wrap"] == "64")):
                table.append([dist_name, tree_name] + [r[c] for _, c in COLUMNS])

dst.parent.mkdir(exist_ok=True)
with dst.open("w", newline="") as f:
    writer = csv.writer(f)
    writer.writerow(["Dist.", "Trees"] + [name for name, _ in COLUMNS])
    writer.writerows(table)
print(f"Written {len(table)} rows to {dst}")

#!/usr/bin/env python3
import sys
import csv
from pathlib import Path

# python3 filter_summary.py summary

input_type = sys.argv[1]
input_path = "data/summary/" + input_type + ".csv"
store_path = "data/summary/" + input_type + "_filtered.csv"

# Column indices in the source CSV (0-based):
#  0  benchmark
#  1  tree
#  2  leaf_wrap
#  3  build
#  4  ins 0.01%
#  5  ins 0.1%
#  6  ins 1%
#  7  ins 10%
#  8  del 0.01%
#  9  del 0.1%
# 10  del 1%
# 11  del 10%
# 12  IND_Find
# 13  OOD_Find
# 14  Half_Find
# 15  RRS
# 16  RRM
# 17  RRL
# 18  RCS
# 19  RCM
# 20  RCL

KEEP_COLS = [0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16, 19]
OUT_HEADER = [
    "benchmark", "tree", "build",
    "ins_0.01%", "ins_0.1%", "ins_1%", "ins_10%",
    "del_0.01%", "del_0.1%", "del_1%", "del_10%",
    "IND_Find", "OOD_Find", "RRM", "RCM",
]

Path(store_path).parent.mkdir(parents=True, exist_ok=True)

with open(input_path, newline="") as fin, open(store_path, "w", newline="") as fout:
    reader = csv.reader(fin)
    writer = csv.writer(fout)
    writer.writerow(OUT_HEADER)

    next(reader)  # skip source header
    for row in reader:
        tree = row[1]
        leaf_wrap = row[2]
        if tree == "BTree" and leaf_wrap != "64":
            continue
        writer.writerow([row[i] for i in KEEP_COLS])

print(f"Written: {store_path}")

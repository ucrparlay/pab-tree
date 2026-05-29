#!/usr/bin/env python3
# python3 merge_randomness.py 100batches

import subprocess
import sys
import csv
from pathlib import Path

input_type = sys.argv[1]
log_folder_name = "randomness"

input_path = f"logs/{log_folder_name}/{input_type}.log"
store_path = f"data/{log_folder_name}/{input_type}.csv"

# Column indices within each data row: randomness=0, insert_t=1, delete_t=2, query_t=3
OPERATIONS = ["insert_t(s)", "delete_t(s)", "query_t(s)"]
OP_COL_IDX = [1, 2, 3]

BENCH_ORDER = ["uni"]
TREE_ORDER = ["BTree", "PAM", "CPAM", "PMA", "MassTree", "BwTree"]


def parse_log(path):
    """Return list of (benchmark, tree, buf_value, data_rows).
    data_rows is a list of token lists, one per randomness level.
    """
    blocks = []
    lines = open(path).readlines()
    i = 0
    while i < len(lines):
        tokens = lines[i].split()
        if not tokens:
            i += 1
            continue
        if tokens[0] == "Tree:":
            # Tree: BTree; Buf: 64; InputFile: ...; Dis: uni;
            tree = tokens[1].rstrip(";")
            buf_value = int(tokens[3].rstrip(";"))
            benchmark = tokens[7].rstrip(";")
            i += 2  # skip Tree: line and the omitted second line
            data_rows = []
            while i < len(lines):
                row_tokens = lines[i].split()
                if not row_tokens:
                    i += 1
                    continue
                if row_tokens[0] == "Tree:":
                    break
                data_rows.append(row_tokens)
                i += 1
            blocks.append((benchmark, tree, buf_value, data_rows))
        else:
            i += 1
    return blocks


def build_rows(blocks):
    """One CSV row per (benchmark, tree, buf_value, randomness).
    Columns: benchmark, tree, buf_value, randomness, insert_t(s), delete_t(s), query_t(s)
    """
    rows = []
    for benchmark, tree, buf_value, data_rows in blocks:
        for row in data_rows:
            rand = int(row[0])
            values = [row[col_idx] if col_idx < len(row) else "" for col_idx in OP_COL_IDX]
            rows.append([benchmark, tree, buf_value, rand] + values)
    return rows


def sort_rows(rows):
    def key(r):
        b = BENCH_ORDER.index(r[0]) if r[0] in BENCH_ORDER else 99
        t = TREE_ORDER.index(r[1]) if r[1] in TREE_ORDER else 99
        return (b, t, int(r[2]), int(r[3]))

    return sorted(rows, key=key)


def write_csv(rows, path):
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["benchmark", "tree", "buf_value", "randomness"] + OPERATIONS)
        writer.writerows(rows)
    print(f"Written {len(rows)} rows to {path}")


blocks = parse_log(input_path)
rows = build_rows(blocks)
rows = sort_rows(rows)
write_csv(rows, store_path)

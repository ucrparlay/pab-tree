#!/usr/bin/env python3
import subprocess
import sys
import csv
from pathlib import Path

input_type = sys.argv[1]
log_folder_name = "osm"

subprocess.run(
    f"""
    > logs/{log_folder_name}/{input_type}.log
    for file in $(ls logs/{log_folder_name}/*/*/*.log); do
        cat "$file" >> logs/{log_folder_name}/{input_type}.log
        echo "" >> logs/{log_folder_name}/{input_type}.log
    done
    """,
    shell=True,
)

input_path = "logs/" + log_folder_name + "/" + input_type + ".log"
store_path = "data/" + log_folder_name + "/" + input_type + ".csv"

START_YEAR = 2014
# Column indices within each data row: insert_n=0, delete_n=1, tree_n=2, insert_t=3, delete_t=4, query_t=5
OPERATIONS = ["insert_t(s)", "delete_t(s)", "find_t(s)", "range_t(s)"]
OP_COL_IDX = [3, 4, 5, 6]

BENCH_ORDER = ["osm"]
TREE_ORDER = ["BTree", "PAM", "CPAM", "PMA", "MassTree", "BwTree"]


def parse_log(path):
    """Return list of (benchmark, tree, buf_value, data_rows).
    data_rows is a list of token lists, one per year.
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
            # Tree: BTree; Buf: 64; InputFile: ...; Dis: osm;
            tree = tokens[1].rstrip(";")
            buf_value = int(tokens[3].rstrip(";"))
            benchmark = tokens[7].rstrip(";")
            i += 2  # skip Tree: line and the column-header line
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
    """Pivot blocks into one CSV row per (solver, operation).
    Columns: benchmark, tree, buf_value, operation, <year0>, <year1>, ...
    """
    rows = []
    for benchmark, tree, buf_value, data_rows in blocks:
        years = [START_YEAR + j for j in range(len(data_rows))]
        for op_name, col_idx in zip(OPERATIONS, OP_COL_IDX):
            values = [row[col_idx] if col_idx < len(row) else "" for row in data_rows]
            rows.append([benchmark, tree, buf_value, op_name] + values)
    return rows


def sort_rows(rows):
    def key(r):
        b = BENCH_ORDER.index(r[0]) if r[0] in BENCH_ORDER else 99
        t = TREE_ORDER.index(r[1]) if r[1] in TREE_ORDER else 99
        o = OPERATIONS.index(r[3]) if r[3] in OPERATIONS else 99
        return (b, o, t, int(r[2]))

    return sorted(rows, key=key)


def write_csv(rows, path):
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    max_years = max((len(r) - 4 for r in rows), default=0)
    year_header = [START_YEAR + j for j in range(max_years)]
    with open(path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["benchmark", "tree", "buf_value", "operation"] + year_header)
        writer.writerows(rows)
    print(f"Written {len(rows)} rows to {path}")


blocks = parse_log(input_path)
rows = build_rows(blocks)
rows = sort_rows(rows)
write_csv(rows, store_path)

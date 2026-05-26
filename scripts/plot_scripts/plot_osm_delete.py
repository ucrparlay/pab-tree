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
    cwd=Path(__file__).parent.parent,
)

input_path = Path(__file__).parent.parent / "logs" / log_folder_name / f"{input_type}.log"
store_path = Path(__file__).parent.parent / "data" / log_folder_name / f"{input_type}_delete_by_year.csv"

START_YEAR = 2014
DELETE_N_COL_IDX = 1
TREE_N_COL_IDX = 2
DELETE_T_COL_IDX = 4

REINSERT_SIZE     = [0, 0, 0, 0, 0, 11871445, 10901829, 23081884, 34282994, 31576696]
LEAF_BALANCE_SIZE = [0, 0, 0, 0, 0, 0, 921601, 0, 0, 0]

TREE_ORDER = ["BTree", "PAM", "CPAM", "PMA", "MassTree", "BwTree"]


def parse_log(path):
    blocks = []
    lines = open(path).readlines()
    i = 0
    while i < len(lines):
        tokens = lines[i].split()
        if not tokens:
            i += 1
            continue
        if tokens[0] == "Tree:":
            tree = tokens[1].rstrip(";")
            buf_value = int(tokens[3].rstrip(";"))
            i += 2  # skip Tree: header line and column-header line
            data_rows = []
            while i < len(lines):
                row_tokens = lines[i].split()
                if not row_tokens:
                    i += 1
                    continue
                if row_tokens[0] == "Tree:":
                    break
                # skip non-data lines (e.g. column headers mixed with extra output)
                try:
                    int(row_tokens[0])
                except ValueError:
                    i += 1
                    continue
                data_rows.append(row_tokens)
                i += 1
            blocks.append((tree, buf_value, data_rows))
        else:
            i += 1
    return blocks


blocks = parse_log(input_path)

solvers = []
seen = set()
for tree, buf_value, data_rows in blocks:
    if tree not in seen:
        seen.add(tree)
        t = TREE_ORDER.index(tree) if tree in TREE_ORDER else 99
        solvers.append((tree, t, buf_value))

solvers.sort(key=lambda x: (x[1], x[2]))
solver_labels = [s[0] for s in solvers]

max_steps = max((len(data_rows) for _, _, data_rows in blocks), default=0)

delete_t_by_solver = {label: [""] * max_steps for label in solver_labels}
delete_n_by_step   = [""] * max_steps
tree_n_by_step     = [""] * max_steps

for tree, buf_value, data_rows in blocks:
    for j, row in enumerate(data_rows):
        def get(idx, r=row):
            return r[idx] if idx < len(r) else ""
        delete_t_by_solver[tree][j] = get(DELETE_T_COL_IDX)
        if not delete_n_by_step[j]:
            delete_n_by_step[j] = get(DELETE_N_COL_IDX)
        if not tree_n_by_step[j]:
            tree_n_by_step[j] = get(TREE_N_COL_IDX)

store_path.parent.mkdir(parents=True, exist_ok=True)
with open(store_path, "w", newline="") as f:
    writer = csv.writer(f)
    writer.writerow(["year", "delete_n", "tree_n"] + solver_labels + ["reinsert_size", "leaf_balance_size"])
    for j in range(max_steps):
        rs  = REINSERT_SIZE[j]     if j < len(REINSERT_SIZE)     else ""
        lbs = LEAF_BALANCE_SIZE[j] if j < len(LEAF_BALANCE_SIZE) else ""
        writer.writerow(
            [START_YEAR + j, delete_n_by_step[j], tree_n_by_step[j]]
            + [delete_t_by_solver[label][j] for label in solver_labels]
            + [rs, lbs]
        )

print(f"Written {max_steps} rows to {store_path}")

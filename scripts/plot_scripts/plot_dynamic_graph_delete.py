#!/usr/bin/env python3
import subprocess
import sys
import csv
from pathlib import Path

input_type = sys.argv[1]
log_folder_name = "temp"

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
OP_COL_IDX = 4  # delete_t column index within each data row
SIZE_COL_IDX = 1  # delete_n column index within each data row

REINSERT_SIZE = [1663694, 43197, 67520, 347383, 53515, 0, 74753, 13107, 34605, 0]
LEAF_BALANCE_SIZE = [1095915, 665325, 436139, 294096, 243864, 238578, 249072, 281956, 196682, 122536]

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
            i += 2
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

max_years = max((len(data_rows) for _, _, data_rows in blocks), default=0)
values_by_solver = {label: [""] * max_years for label in solver_labels}
size_by_year = [""] * max_years

for tree, buf_value, data_rows in blocks:
    for j, row in enumerate(data_rows):
        values_by_solver[tree][j] = row[OP_COL_IDX] if OP_COL_IDX < len(row) else ""
        if not size_by_year[j]:
            size_by_year[j] = row[SIZE_COL_IDX] if SIZE_COL_IDX < len(row) else ""

store_path.parent.mkdir(parents=True, exist_ok=True)
with open(store_path, "w", newline="") as f:
    writer = csv.writer(f)
    writer.writerow(["year", "size"] + solver_labels + ["reinsert_size", "leaf_balance_size"])
    for j in range(max_years):
        rs = REINSERT_SIZE[j] if j < len(REINSERT_SIZE) else ""
        lbs = LEAF_BALANCE_SIZE[j] if j < len(LEAF_BALANCE_SIZE) else ""
        writer.writerow([START_YEAR + j, size_by_year[j]] + [values_by_solver[label][j] for label in solver_labels] + [rs, lbs])

print(f"Written {max_years} rows to {store_path}")

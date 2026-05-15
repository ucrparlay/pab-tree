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
store_path = Path(__file__).parent.parent / "data" / log_folder_name / f"{input_type}_insert_by_year.csv"

START_YEAR = 2014
INSERT_COL_IDX = 3  # insert_t column index within each data row
INSERT_N_COL_IDX = 0  # insert_n column index within each data row

TREE_ORDER = ["BTree", "PAM", "CPAM", "PMA", "MassTree", "BwTree"]


def parse_log(path):
    """Return list of (tree, buf_value, data_rows).
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
            tree = tokens[1].rstrip(";")
            buf_value = int(tokens[3].rstrip(";"))
            i += 2  # skip Tree: line and column-header line
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


def solver_label(tree, buf_value):
    return tree


def sort_key(label, tree, buf_value):
    t = TREE_ORDER.index(tree) if tree in TREE_ORDER else 99
    return (t, buf_value)


blocks = parse_log(input_path)

# Collect all solvers in order and determine number of years
solvers = []
seen = set()
for tree, buf_value, data_rows in blocks:
    label = solver_label(tree, buf_value)
    if label not in seen:
        seen.add(label)
        solvers.append((label, tree, buf_value))

solvers.sort(key=lambda x: sort_key(x[0], x[1], x[2]))
solver_labels = [s[0] for s in solvers]

# Build a dict: solver_label -> list of insert times per year (index = year offset)
max_years = max((len(data_rows) for _, _, data_rows in blocks), default=0)
insert_by_solver = {label: [""] * max_years for label in solver_labels}
size_by_year = [""] * max_years

for tree, buf_value, data_rows in blocks:
    label = solver_label(tree, buf_value)
    for j, row in enumerate(data_rows):
        insert_by_solver[label][j] = row[INSERT_COL_IDX] if INSERT_COL_IDX < len(row) else ""
        if not size_by_year[j]:
            size_by_year[j] = row[INSERT_N_COL_IDX] if INSERT_N_COL_IDX < len(row) else ""

# Write CSV: one row per year, columns = [year, size, solver1, solver2, ...]
store_path.parent.mkdir(parents=True, exist_ok=True)
with open(store_path, "w", newline="") as f:
    writer = csv.writer(f)
    writer.writerow(["year", "size"] + solver_labels)
    for j in range(max_years):
        year = START_YEAR + j
        row = [year, size_by_year[j]] + [insert_by_solver[label][j] for label in solver_labels]
        writer.writerow(row)

print(f"Written {max_years} rows to {store_path}")

#!/usr/bin/env python3
"""Fig. 5: the insert times by year, as the plot reads them.

HOW TO RUN (from scripts_ae/, after ae_run_osm.sh)
    python3 merge_osm_insert.py osm

Reads logs/osm/<tree>/osm/64.log; writes data/osm/osm_insert_by_year.csv.
From the paper's scripts/plot_scripts/plot_osm_insert.py, paths moved to
scripts_ae/.
"""
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
    cwd=Path(__file__).parent,
)

log_dir = Path(__file__).parent / "logs" / log_folder_name
store_path = (
    Path(__file__).parent
    / "data"
    / log_folder_name
    / f"{input_type}_insert_by_year.csv"
)

START_YEAR = 2014
INSERT_N_COL_IDX = 0
TREE_N_COL_IDX = 2
INSERT_T_COL_IDX = 3

SPLIT_LEAF_SIZE = [
    0,
    8037248,
    16419615,
    30690764,
    27822034,
    43917124,
    45498211,
    32798938,
    33427717,
    34286185,
]

TREE_ORDER = ["BTree", "PAM", "CPAM", "PMA", "MassTree", "BwTree"]
# the tree of logs/osm/<tree id>/osm/64.log, common.sh's ids
TREE_IDS = {"0": "BTree", "1": "CPAM", "2": "PAM", "3": "PMA", "5": "MassTree",
            "6": "BwTree"}
# a value the run did not print
NA = "N.A."


def parse_log(path):
    blocks = []
    # timeout(1) writes this into the row when a Mass-tree or Bw-tree run
    # crashes
    lines = [l.replace("timeout: the monitored command dumped core", " ")
             for l in open(path).readlines()]
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


# one log at a time, so that a log with no header (the run died first, or
# never started) still names its tree
blocks = []
for log in sorted(log_dir.glob("*/*/*.log")):
    found = parse_log(log)
    if not found and log.parent.parent.name in TREE_IDS:
        found = [(TREE_IDS[log.parent.parent.name], 0, [])]
    blocks += found

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

insert_t_by_solver = {label: [NA] * max_steps for label in solver_labels}
insert_n_by_step = [NA] * max_steps
tree_n_by_step = [NA] * max_steps

for tree, buf_value, data_rows in blocks:
    for j, row in enumerate(data_rows):

        def get(idx, r=row):
            return r[idx] if idx < len(r) else NA

        insert_t_by_solver[tree][j] = get(INSERT_T_COL_IDX)
        if insert_n_by_step[j] == NA:
            insert_n_by_step[j] = get(INSERT_N_COL_IDX)
        if tree_n_by_step[j] == NA:
            tree_n_by_step[j] = get(TREE_N_COL_IDX)

store_path.parent.mkdir(parents=True, exist_ok=True)
with open(store_path, "w", newline="") as f:
    writer = csv.writer(f)
    writer.writerow(
        ["year", "insert_n", "tree_n"] + solver_labels + ["split_leaf_size"]
    )
    for j in range(max_steps):
        sls = SPLIT_LEAF_SIZE[j] if j < len(SPLIT_LEAF_SIZE) else ""
        writer.writerow(
            [START_YEAR + j, insert_n_by_step[j], tree_n_by_step[j]]
            + [insert_t_by_solver[label][j] for label in solver_labels]
            + [sls]
        )

print(f"Written {max_steps} rows to {store_path}")

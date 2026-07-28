#!/usr/bin/env python3
"""Merge the incremental-update benchmark logs into a single CSV.

Usage:  ./merge_incre_updates.py [<name>]        # <name> defaults to incremental_updates

Reads   logs/incremental_updates/<tree>/<dis>/<leaf_wrap>.log
Writes  logs/incremental_updates/<name>.log     (concatenation of all the above)
        data/incremental_updates/<name>.csv     (one row per run)

Each log holds a header line

    Tree: BTree; Buf: 32; InputFile: 1.base.bin; Dis: uni;

followed by one whitespace-separated row of 73 timings:

    build | 8 x (update_time, IND, OOD, RRS, RRM, RRL, RCS, RCM, RCL)

the 8 blocks being, in order, ins 0.01% / 0.1% / 1% / 10% then del 0.01% /
0.1% / 1% / 10%.  Lines that are not numeric (e.g. numactl / driver errors)
are skipped with a note on stderr.
"""
import csv
import subprocess
import sys
from pathlib import Path
from typing import List

log_folder_name = "incremental_updates"
input_type = sys.argv[1] if len(sys.argv) > 1 else log_folder_name

input_path = "logs/" + log_folder_name + "/" + input_type + ".log"
store_path = "data/" + log_folder_name + "/" + input_type + ".csv"

batch_ratio_seq = ["0.01%", "0.1%", "1%", "10%"]
query_seq = ["IND", "OOD", "RRS", "RRM", "RRL", "RCS", "RCM", "RCL"]
block_seq = [f"{op}_{r}" for op in ("ins", "del") for r in batch_ratio_seq]
kFieldNum = 1 + len(block_seq) * (1 + len(query_seq))  # build + 8 x 9 = 73

bench_order = ["uni", "exp", "norm", "dual_norm"]
tree_order = ["BTree", "PAM", "CPAM", "PMA", "MassTree", "BwTree"]


def concat_logs():
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


def is_number(tok) -> bool:
    try:
        float(tok)
        return True
    except ValueError:
        return False


def combine(P) -> List:
    buf_value = 0
    benchmark = ""
    tree = ""
    all_data = []

    for line in open(P, "r").readlines():
        lin_sep = line.split()

        if len(lin_sep) == 0:  # Skip empty lines
            continue

        # Tree information - parse Buf value
        if lin_sep[0] == "Tree:":
            tree = str(lin_sep[1].strip(";"))
            buf_value = int(lin_sep[3].strip(";"))
            benchmark = str(lin_sep[7].strip(";"))
            continue

        # parse result - one row of timings per run
        if not all(is_number(t) for t in lin_sep):
            print(f"skip non-numeric line: {line.strip()}", file=sys.stderr)
            continue

        if tree == "":
            print(f"skip orphan result line: {line.strip()}", file=sys.stderr)
            continue

        if len(lin_sep) != kFieldNum:
            print(
                f"warn: {benchmark}/{tree}/{buf_value} has {len(lin_sep)} fields, "
                f"expected {kFieldNum}",
                file=sys.stderr,
            )

        all_data.append([benchmark, tree, buf_value] + lin_sep)

    return all_data


def post_processing(data):
    def order_of(seq, key):  # unknown names sort last instead of raising
        return seq.index(key) if key in seq else len(seq)

    sorted_lst = sorted(
        data,
        key=lambda x: (
            order_of(bench_order, x[0]),
            order_of(tree_order, x[1]),
            int(x[2]),
        ),
    )
    return sorted_lst


def csvSetup():
    target_path = Path(store_path)
    target_path.parent.mkdir(parents=True, exist_ok=True)
    csv_file_pointer = open(store_path, "w", newline="")
    print(csv_file_pointer.name)
    csv_file_pointer.truncate()
    csv_writer = csv.writer(csv_file_pointer)

    header = ["benchmark", "tree", "leaf_wrap", "build"]
    for block in block_seq:
        header.append(block)
        header.extend(query_seq)
    csv_writer.writerow(header)

    return csv_writer, csv_file_pointer


concat_logs()
csv_writer, csv_file_pointer = csvSetup()
data = combine(input_path)
data = post_processing(data)
csv_writer.writerows(data)
csv_file_pointer.close()

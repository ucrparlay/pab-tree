#!/usr/bin/env python3
import subprocess
import sys
import csv
from typing import List
from pathlib import Path

input_type = sys.argv[1]
log_folder_name = "batch_updates"

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

input_path = f"logs/{log_folder_name}/{input_type}.log"
store_path = f"data/{log_folder_name}/{input_type}.csv"

BATCH_RATIOS = [
    "0.01%", "0.02%", "0.05%", "0.1%", "0.2%", "0.5%", "1%",
    "2%", "5%", "10%", "20%", "50%", "100%",
]


def combine(P) -> List:
    lines = open(P, "r").readlines()

    buf_value = 0
    benchmark = ""
    tree = ""
    index = 0
    all_data = []
    while index < len(lines):
        lin_sep = " ".join(lines[index].split()).split(" ")

        if not lin_sep or lin_sep[0] == "":
            index += 1
            continue

        if lin_sep[0] == "Tree:":
            tree = lin_sep[1].strip(";")
            buf_value = int(lin_sep[3].strip(";"))
            benchmark = lin_sep[7].strip(";")
            index += 1
        else:
            all_data.append([benchmark, tree, buf_value] + lin_sep)
            index += 1

    return all_data


def post_processing(data):
    bench_order = ["uni", "exp", "norm", "dual_norm"]
    tree_order = ["BTree", "PAM", "CPAM", "PMA", "MassTree", "BwTree"]

    return sorted(
        data,
        key=lambda x: (
            bench_order.index(x[0]) if x[0] in bench_order else 99,
            tree_order.index(x[1]) if x[1] in tree_order else 99,
            int(x[2]),
        ),
    )


def csv_setup():
    target_path = Path(store_path)
    target_path.parent.mkdir(parents=True, exist_ok=True)
    fp = open(store_path, "w", newline="")
    fp.truncate()
    writer = csv.writer(fp)
    writer.writerow(
        ["benchmark", "tree", "leaf_wrap", "build"]
        + [f"ins_{r}" for r in BATCH_RATIOS]
        + [f"del_{r}" for r in BATCH_RATIOS]
    )
    return writer, fp


writer, fp = csv_setup()
data = combine(input_path)
data = post_processing(data)
writer.writerows(data)
fp.close()
print(f"Written to {store_path}")

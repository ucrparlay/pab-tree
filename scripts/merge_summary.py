#!/usr/bin/env python3
import re
import subprocess
import sys
import csv
import os
import io
from typing import Dict, List, Tuple, Optional
from dataclasses import dataclass, field
from pathlib import Path

solver = ""
ratio_map = {}
input_type = sys.argv[1]
log_folder_name = "summary"

subprocess.run(
    f"""
    > logs/{log_folder_name}/{input_type}.log
    for file in $(ls logs/{log_folder_name}/*/*/*.log); do
        filename=$(basename "$file")
        cat "$file" >> logs/{log_folder_name}/{input_type}.log
        echo "" >> logs/{log_folder_name}/{input_type}.log
    done
    """,
    shell=True,
)

input_path = "logs/" + log_folder_name + "/" + input_type + ".log"
store_path = "data/" + log_folder_name + "/" + input_type + ".csv"
type = ""


def to_float(match):
    return float(match.group(1)) if match else None


def combine(P) -> List:
    lines = open(P, "r").readlines()

    buf_value = 0
    benchmark = ""
    tree = ""
    index = 0
    all_data = []
    while index < len(lines):
        lin_sep = " ".join(lines[index].split())
        lin_sep = lin_sep.split(" ")
        print(lin_sep)

        if len(lin_sep) == 0 or lin_sep[0] == "":  # Skip empty lines
            index += 1
            continue

        # Tree information - parse Buf value
        if lin_sep[0] == "Tree:":
            tree = str(lin_sep[1].strip(";"))
            buf_value = int(lin_sep[3].strip(";"))
            benchmark = str(lin_sep[7].strip(";"))

            index += 1

        else:
            # parse result - summarize by Buf value
            all_data.append([benchmark, tree, buf_value] + lin_sep)
            index += 1

    return all_data


def post_processing(data):
    # desired order for x[0]
    bench_order = ["uni", "exp", "norm", "dual_norm"]
    tree_order = ["BTree", "PAM", "CPAM", "PMA",
                  "MassTree", "BwTree"]  # desired order for x[1]

    sorted_lst = sorted(
        data,
        key=lambda x: (
            bench_order.index(x[0]),
            tree_order.index(x[1]),
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
    csv_writer.writerow(
        [
            "benchmark",
            "tree",
            "leaf_wrap",
            "build",
            "0.01%",
            "0.1%",
            "1%",
            "10%",
            "0.01%",
            "0.1%",
            "1%",
            "10%",
            "IND_Find",
            "OOD_Find",
            "Half_Find",
            "RRS",
            "RRM",
            "RRL",
            "RCS",
            "RCM",
            "RCL",
        ]
    )
    return csv_writer, csv_file_pointer


csv_writer, csv_file_pointer = csvSetup()
data = combine(input_path)
# print(data)
data = post_processing(data)
# print(data)
csv_writer.writerows(data)

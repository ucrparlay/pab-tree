#!/usr/bin/env python3
"""Merge the scalability benchmark logs into a single CSV.

Usage:  ./merge_scalability.py [<name>]        # <name> defaults to scalability

Reads   logs/scalability/<tree>/<dis>/<cores>.log
Writes  logs/scalability/<name>.log     (concatenation of all the above)
        data/scalability/<name>.csv     (one row per run)

Produced by `scalability.sh` / `scalability_concurrent.sh`, which sweep
PARLAY_NUM_THREADS over 1 4 16 32 64 112 224 and write one log per
(tree, distribution, core count).  Each log holds a header line

    Tree: BTree; Buf: 64; InputFile: 1.base.bin; Dis: uni;

followed by one whitespace-separated row of 3 timings:

    build  insert  delete

`tag=2#0011` selects batch insert (1<<0) and batch delete (1<<1), and the
primal build always runs and always reports, so every run is 3 values.
`query_type=0`, so there are no query columns -- unlike the incremental sweep.

NOTE: the core count appears **only in the file name**, never in the header
(`Buf:` is the leaf size -- 64 for BTree, 1 for everything else).  So the logs
have to be read per file to know which thread count a row belongs to; parsing a
flat concatenation the way merge_incre_updates.py does would lose it.  The
concatenated log is still written, for reading by hand.

The CSV is long/tidy -- one row per (dis, tree, cores) rather than one row per
tree with the core counts spread across columns -- because cores is the x axis
of every plot this feeds, and because it keeps the speedup columns meaningful.
`*_su` is self-relative speedup, t(1 core) / t(n cores), left blank when the
1-core run for that (dis, tree) is missing.
"""
import csv
import os
import subprocess
import sys
from pathlib import Path
from typing import List

log_folder_name = "scalability"
input_type = sys.argv[1] if len(sys.argv) > 1 else log_folder_name

log_root = "logs/" + log_folder_name
input_path = log_root + "/" + input_type + ".log"
store_path = "data/" + log_folder_name + "/" + input_type + ".csv"

# tag 2#0011: the primal build, then batch insert, then batch delete
op_seq = ["build", "insert", "delete"]
kFieldNum = len(op_seq)

# the thread counts scalability.sh sweeps, in the order it sweeps them
core_seq = [1, 4, 16, 32, 64, 112, 224]
kBaseCore = core_seq[0]  # speedup denominator

# a value masstree writes for the queries it does not implement
kMissingValue = "-"

# stderr messages that get interleaved into a result row by `2>&1 | tee`
kNoisePhrases = ("madvise superpage: Cannot allocate memory",)

bench_order = ["uni", "norm", "seg"]
tree_order = ["BTree", "PAM", "CPAM", "PMA", "MassTree", "BwTree"]


def concat_logs():
    subprocess.run(
        f"""
        > {log_root}/{input_type}.log
        for file in $(ls {log_root}/*/*/*.log); do
            echo ">>> $file" >> {log_root}/{input_type}.log
            cat "$file" >> {log_root}/{input_type}.log
            echo "" >> {log_root}/{input_type}.log
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


def is_value(tok) -> bool:
    return tok == kMissingValue or is_number(tok)


def run_logs():
    # walk <tree>/<dis>/<cores>.log explicitly: the depth carries the meaning,
    # and this keeps the merged <name>.log at the root from being read back in
    for tree_dir in sorted(os.listdir(log_root)):
        if not os.path.isdir(os.path.join(log_root, tree_dir)):
            continue
        for dis_dir in sorted(os.listdir(os.path.join(log_root, tree_dir))):
            sub = os.path.join(log_root, tree_dir, dis_dir)
            if not os.path.isdir(sub):
                continue
            for name in sorted(os.listdir(sub)):
                if name.endswith(".log"):
                    yield os.path.join(sub, name), Path(name).stem


def parse_log(P) -> List:
    """Return [(benchmark, tree, values), ...] -- one entry per run in P."""
    runs = []
    context = None
    values = []

    def flush():
        # one run ends where the next header begins, not at a line break
        if context is not None and len(values) > 0:
            runs.append((*context, list(values)))
        values.clear()

    for line in open(P, "r").readlines():
        for phrase in kNoisePhrases:
            line = line.replace(phrase, " ")

        lin_sep = line.split()

        if len(lin_sep) == 0:  # Skip empty lines
            continue

        if lin_sep[0] == "Tree:":
            flush()
            if len(lin_sep) < 8:
                print(f"{P}: skip short header line: {line.strip()}", file=sys.stderr)
                context = None
                continue
            context = (
                str(lin_sep[7].strip(";")),  # benchmark, i.e. the distribution
                str(lin_sep[1].strip(";")),  # tree
            )
            continue

        if not all(is_value(t) for t in lin_sep):
            print(f"{P}: skip non-numeric line: {line.strip()}", file=sys.stderr)
            continue

        if context is None:
            print(f"{P}: skip orphan result line: {line.strip()}", file=sys.stderr)
            continue

        values.extend(lin_sep)

    flush()  # the last run in the file has no header after it

    if len(runs) == 0:
        # a run that died before printing its timings still leaves the header
        print(f"{P}: no timings, skipped (aborted run?)", file=sys.stderr)

    return runs


def combine() -> List:
    all_data = []

    for path, stem in run_logs():
        if not stem.isdigit():
            print(f"{path}: core count not a number, skipped", file=sys.stderr)
            continue
        cores = int(stem)

        for benchmark, tree, values in parse_log(path):
            if len(values) != kFieldNum:
                print(
                    f"warn: {benchmark}/{tree}/{cores} has {len(values)} values, "
                    f"expected {kFieldNum}"
                    + (" (padded)" if len(values) < kFieldNum else " (kept as is)"),
                    file=sys.stderr,
                )
                values += [""] * (kFieldNum - len(values))  # no-op when long
            all_data.append([benchmark, tree, cores] + values)

    return all_data


def add_speedup(data) -> List:
    # self-relative speedup t(1) / t(n), per (benchmark, tree)
    base = {
        (row[0], row[1]): row[3:]
        for row in data
        if row[2] == kBaseCore
    }

    for row in data:
        ref = base.get((row[0], row[1]))
        for i in range(len(op_seq)):
            val = ""
            if ref is not None and is_number(row[3 + i]) and is_number(ref[i]):
                t = float(row[3 + i])
                if t > 0:
                    val = f"{float(ref[i]) / t:.4f}"
            row.append(val)

    return data


def post_processing(data):
    def order_of(seq, key):  # unknown names sort last instead of raising
        return seq.index(key) if key in seq else len(seq)

    sorted_lst = sorted(
        data,
        key=lambda x: (
            order_of(bench_order, x[0]),
            order_of(tree_order, x[1]),
            int(x[2]),  # cores sort numerically, not 1 112 16 224 ...
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

    header = ["benchmark", "tree", "cores"] + op_seq
    header += [f"{op}_su" for op in op_seq]
    csv_writer.writerow(header)

    return csv_writer, csv_file_pointer


concat_logs()
csv_writer, csv_file_pointer = csvSetup()
data = combine()
data = add_speedup(data)
data = post_processing(data)
csv_writer.writerows(data)
csv_file_pointer.close()

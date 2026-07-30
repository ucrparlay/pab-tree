#!/usr/bin/env python3
"""Merge the incremental-update benchmark logs into a single CSV.

Usage:  ./merge_incre_updates.py [<name>]        # <name> defaults to incremental_updates

Reads   logs/incremental_updates/<tree>/<dis>/<leaf_wrap>.log
Writes  logs/incremental_updates/<name>.log     (concatenation of all the above)
        data/incremental_updates/<name>.csv     (one row per run)

Each log holds a header line

    Tree: BTree; Buf: 32; InputFile: 1.base.bin; Dis: uni;

followed by one whitespace-separated row of 90 values:

    build | 9 queries | 8 x (update_time, 9 queries)

The 9 queries are always in the same order: IND, OOD, MIX (membership over
keys that are all in the tree, all absent from it, then a shuffled mix of
both), RRS, RRM, RRL (range report over small / mid / large boxes), then
RCS, RCM, RCL (range count over the same three box sizes).

The first (build, 9 queries) pair is the statically built tree -- tag 1<<5.
The 8 update blocks that follow are, in order, ins then del, each at ratio
0.0001 / 0.001 / 0.01 / 0.1 of the input, i.e. 0.01% / 0.1% / 1% / 10%.

Two log quirks are handled.  Masstree prints "-" for the range counts it does
not implement, so "-" is a legal value.  And a message the allocator writes to
stderr can be spliced into the middle of a row by `2>&1 | tee`, splitting it
across lines -- so values are accumulated until the next header rather than
being read one row per line.  A run whose values end short (a killed process)
is padded to keep the CSV columns aligned, and reported on stderr.
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
query_seq = ["IND", "OOD", "MIX", "RRS", "RRM", "RRL", "RCS", "RCM", "RCL"]
block_seq = [f"{op}_{r}" for op in ("ins", "del") for r in batch_ratio_seq]
# build + its 9 queries, then 8 update blocks of (time + 9 queries) = 90
kFieldNum = (1 + len(query_seq)) * (1 + len(block_seq))

# a row from the older driver: build + 8 blocks of (time + 8 queries), i.e. no
# MIX membership and no query block over the statically built tree.  Its values
# line up with nothing in the current layout, and its header is identical to a
# current one, so such rows are dropped rather than written as duplicate keys.
kLegacyFieldNum = 1 + len(block_seq) * (1 + len(query_seq) - 1)  # 73

# a value masstree writes for the queries it does not implement
kMissingValue = "-"

# stderr messages that get interleaved into a result row by `2>&1 | tee`
kNoisePhrases = ("madvise superpage: Cannot allocate memory",)

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


def is_value(tok) -> bool:
    return tok == kMissingValue or is_number(tok)


def combine(P) -> List:
    all_data = []
    context = None  # (benchmark, tree, buf) of the header we are under
    values = []  # values collected since that header

    def flush():
        # one run ends where the next header begins, not at a line break
        if context is None or len(values) == 0:
            return
        benchmark, tree, buf_value = context
        row = list(values)
        if len(row) == kLegacyFieldNum:
            print(
                f"skip {benchmark}/{tree}/{buf_value}: {kLegacyFieldNum} values, "
                f"the pre-MIX layout -- stale log, rerun or move it aside",
                file=sys.stderr,
            )
            values.clear()
            return
        if len(row) != kFieldNum:
            print(
                f"warn: {benchmark}/{tree}/{buf_value} has {len(row)} values, "
                f"expected {kFieldNum}"
                + (" (padded)" if len(row) < kFieldNum else " (kept as is)"),
                file=sys.stderr,
            )
            row += [""] * (kFieldNum - len(row))  # no-op when the row is long
        all_data.append([benchmark, tree, buf_value] + row)
        values.clear()

    for line in open(P, "r").readlines():
        for phrase in kNoisePhrases:
            line = line.replace(phrase, " ")

        lin_sep = line.split()

        if len(lin_sep) == 0:  # Skip empty lines
            continue

        # Tree information - parse Buf value
        if lin_sep[0] == "Tree:":
            flush()
            if len(lin_sep) < 8:
                print(f"skip short header line: {line.strip()}", file=sys.stderr)
                context = None
                continue
            context = (
                str(lin_sep[7].strip(";")),  # benchmark, i.e. the distribution
                str(lin_sep[1].strip(";")),  # tree
                int(lin_sep[3].strip(";")),  # leaf wrap
            )
            continue

        # parse result - the values of one run, possibly split over lines
        if not all(is_value(t) for t in lin_sep):
            print(f"skip non-numeric line: {line.strip()}", file=sys.stderr)
            continue

        if context is None:
            print(f"skip orphan result line: {line.strip()}", file=sys.stderr)
            continue

        values.extend(lin_sep)

    flush()  # the last run in the file has no header after it

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

    # the query names repeat once per block, so prefix them with their block
    header = ["benchmark", "tree", "leaf_wrap"]
    for block in ["build"] + block_seq:
        header.append(block)
        header.extend(f"{block}_{q}" for q in query_seq)
    csv_writer.writerow(header)

    return csv_writer, csv_file_pointer


concat_logs()
csv_writer, csv_file_pointer = csvSetup()
data = combine(input_path)
data = post_processing(data)
csv_writer.writerows(data)
csv_file_pointer.close()

#!/usr/bin/env python3
"""Merge the incremental-update benchmark logs into one 36-value CSV.

Usage:  ./merge_incre_updates_new.py [<name>]

Reads   logs/incremental_updates_new/<tree>/<dis>/<leaf_wrap>.log   (36 values)
        logs/incremental_updates/<tree>/<dis>/<leaf_wrap>.log       (90 values)
Writes  logs/incremental_updates_new/<name>.log   (concatenation of both, for
                                                   reading by hand)
        data/incremental_updates_new/<name>.csv   (one row per run)

Each log holds a header line

    Tree: CPAM; Buf: 64; InputFile: 1.batch.bin; Dis: exp;

followed by one whitespace-separated row of values.

The current driver emits **36 values** -- `build | 3 queries | 8 x
(update_time, 3 queries)`, 9 blocks of 4.  The 3 queries are MIX (membership
over a shuffled mix of present and absent keys), RRM (range report, rec_type 1)
and RCM (range count, rec_type 1).  The older driver emitted **90**, running
IND/OOD membership passes either side of MIX and three box sizes (S/M/L) for
each of range report and range count.

The 36 columns are a strict subset of the 90, so the older logs are *projected*
down rather than skipped: for each block we keep the timing plus MIX, RRM and
RCM, and drop IND/OOD/RRS/RRL/RCS/RCL.  Projection is done **by column name**,
not by index, so it cannot silently drift if either layout changes.

Block order is the same in both: the first (build, queries) pair is the
statically built tree (tag 1<<5), then 8 update blocks, ins then del, each at
ratio 0.0001 / 0.001 / 0.01 / 0.1 of the input, i.e. 0.01% / 0.1% / 1% / 10%.

Only uni, norm and seg are emitted (see kWantBench for what is dropped and how
to bring it back), and norm and seg are reported under the names skew and
adversary (see kBenchDisplay).  Each comes from exactly one sweep -- norm and
seg from the new logs, uni from the old ones.  norm is in both sweeps and is
taken from the new one; a tree that has no norm
run there is left absent rather than backfilled from the old logs, because the
two ran on different datasets and must not be mixed inside a distribution (see
kSources).  BTree is additionally pinned to leaf size 64: it is the only tree
swept over leaf sizes, so the old uni logs carry 2..1024 for it and one row per
(distribution, tree) is what we want.  A (benchmark, tree, leaf_wrap) seen twice
is reported and dropped rather than duplicated.

`leaf_wrap` is read from the header's `Buf:` field rather than the filename:
the new sweep names every file <leaf_wrap>.log after the BTree leaf size, so
PAM/PMA/MassTree runs sit in a 64.log while reporting `Buf: 1`.

Cutting this down to the 20-column view (the update timings plus the build and
1% blocks' queries) is filter_incremental_updates.py's job, so that the column
list lives in exactly one place.

Three log quirks are handled.  "-" is a legal value (older masstree logs print
it for the range counts it did not implement).  A message the allocator writes
to stderr can be spliced into the middle of a row by `2>&1 | tee`, splitting it
across lines -- so values are accumulated until the next header rather than
being read one row per line.  And a run whose values end short (a killed
process) is padded to keep the CSV columns aligned, and reported on stderr.
"""
import csv
import sys
from pathlib import Path
from typing import Dict, List, Tuple

log_folder_name = "incremental_updates_new"
input_type = sys.argv[1] if len(sys.argv) > 1 else log_folder_name

concat_path = "logs/" + log_folder_name + "/" + input_type + ".log"
store_path = "data/" + log_folder_name + "/" + input_type + ".csv"

batch_ratio_seq = ["0.01%", "0.1%", "1%", "10%"]
block_seq = [f"{op}_{r}" for op in ("ins", "del") for r in batch_ratio_seq]

# the trimmed suite the driver runs now, and the full one it used to run
query_seq = ["MIX", "RRM", "RCM"]
query_seq_full = ["IND", "OOD", "MIX", "RRS", "RRM", "RRL", "RCS", "RCM", "RCL"]

# the distributions worth keeping, in output order.  norm and seg are only in
# the new sweep, uni only in the old one.
#
# exp, seg_10 and dual_norm are dropped.  Their logs are still on disk -- adding
# a name back here (and to kSources) is all it takes to bring one back.  seg_10
# is the same sliding-segment shape as seg at a wider window (the generator's
# -batch_param mult), so the two are separate distributions rather than two runs
# of one; only the narrower one is reported.
kWantBench = ["uni", "norm", "seg"]

# what each distribution is called in the CSV.  The keys are the generator's own
# names, which is what the driver writes into the log header's `Dis:` field and
# what kWantBench/kSources match on; the values are the reporting names.  Doing
# this here rather than renaming the logs means a rerun of the benchmark, which
# writes `Dis: norm` / `Dis: seg` again, does not undo it.
kBenchDisplay = {"norm": "skew", "seg": "adversary"}


def display_bench(name: str) -> str:
    return kBenchDisplay.get(name, name)

# BTree is the only tree swept over leaf sizes (the old uni logs carry 2..1024);
# every other tree reports a single Buf.  Keep just the default leaf size so the
# CSV holds one row per (distribution, tree).
kBTreeLeafWrap = 64

# a row from the driver before MIX existed: build + 8 blocks of (time + 8
# queries), with no query block over the statically built tree.  Unlike the
# 90-value layout this one cannot be projected onto the 36 columns -- it has no
# MIX column at all -- so it is skipped rather than mapped.
kLegacyFieldNum = 1 + len(block_seq) * (1 + len(query_seq_full) - 1)  # 73

# a value masstree writes for the queries it does not implement
kMissingValue = "-"

# stderr messages that get interleaved into a result row by `2>&1 | tee`
kNoisePhrases = ("madvise superpage: Cannot allocate memory",)

# rows carry the display name, so sort on that
bench_order = [display_bench(b) for b in kWantBench]
tree_order = ["BTree", "PAM", "CPAM", "PMA", "MassTree", "BwTree"]

kIdCols = ["benchmark", "tree", "leaf_wrap"]


def value_header(queries: List[str]) -> List[str]:
    # the query names repeat once per block, so prefix them with their block
    header = []
    for block in ["build"] + block_seq:
        header.append(block)
        header.extend(f"{block}_{q}" for q in queries)
    return header


header_36 = value_header(query_seq)
header_90 = value_header(query_seq_full)

# (folder, how many values a row in it should have, which distributions to take
# from it).  The two sweeps ran on different datasets -- the old one's exp
# timings track its own uni timings, roughly 9x the new sweep's -- so they must
# not be mixed inside one distribution.  exp therefore comes wholly from the new
# sweep and uni wholly from the old one.  A tree missing from the new sweep
# (BwTree, as of this writing) is left absent from exp rather than backfilled
# from the old logs at the wrong scale.
kSources = [
    (log_folder_name, len(header_36), ["norm", "seg"]),
    ("incremental_updates", len(header_90), ["uni"]),
]


def iter_logs(folder: str) -> List[Path]:
    return sorted(Path("logs", folder).glob("*/*/*.log"))


def is_number(tok) -> bool:
    try:
        float(tok)
        return True
    except ValueError:
        return False


def is_value(tok) -> bool:
    return tok == kMissingValue or is_number(tok)


def combine(paths: List[Path], field_num: int) -> List[Tuple[Tuple, List[str]]]:
    """Parse one source into [((benchmark, tree, leaf_wrap), values), ...]."""
    all_data = []
    context = None  # (benchmark, tree, buf) of the header we are under
    values = []  # values collected since that header

    def flush():
        # one run ends where the next header begins, not at a line break
        if context is None or len(values) == 0:
            return
        benchmark, tree, buf_value = context
        row = list(values)
        values.clear()
        if len(row) == kLegacyFieldNum:
            print(
                f"skip {benchmark}/{tree}/{buf_value}: {kLegacyFieldNum} values, "
                f"the pre-MIX layout -- no MIX column to project, rerun it",
                file=sys.stderr,
            )
            return
        if len(row) != field_num:
            print(
                f"warn: {benchmark}/{tree}/{buf_value} has {len(row)} values, "
                f"expected {field_num}"
                + (" (padded)" if len(row) < field_num else " (kept as is)"),
                file=sys.stderr,
            )
            row += [""] * (field_num - len(row))  # no-op when the row is long
        all_data.append((context, row))

    for path in paths:
        for line in path.open("r").readlines():
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

        flush()  # the last run in a file has no header after it

    return all_data


def project(values: List[str], src_header: List[str]) -> List[str]:
    """Map a row onto the 36 columns, by name rather than by index."""
    if src_header == header_36:
        return values
    by_name = dict(zip(src_header, values))
    return [by_name[name] for name in header_36]


def collect() -> List[List[str]]:
    rows: Dict[Tuple, List[str]] = {}
    for folder, field_num, take_bench in kSources:
        src_header = header_36 if field_num == len(header_36) else header_90
        for context, values in combine(iter_logs(folder), field_num):
            benchmark, tree, buf_value = context
            if benchmark not in take_bench:
                continue
            if tree == "BTree" and buf_value != kBTreeLeafWrap:
                continue
            if context in rows:
                # the 36-value sweep is parsed first and wins
                print(
                    f"skip {benchmark}/{tree}/{buf_value} from {folder}: already "
                    f"taken from the {len(header_36)}-value sweep",
                    file=sys.stderr,
                )
                continue
            # keyed on the log name so dedupe is unaffected, reported as the
            # display name
            rows[context] = [display_bench(benchmark), tree, buf_value] + project(
                values, src_header
            )
    return list(rows.values())


def post_processing(data):
    def order_of(seq, key):  # unknown names sort last instead of raising
        return seq.index(key) if key in seq else len(seq)

    return sorted(
        data,
        key=lambda x: (
            order_of(bench_order, x[0]),
            order_of(tree_order, x[1]),
            int(x[2]),
        ),
    )


def write_concat_log():
    target = Path(concat_path)
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("w") as out:
        for folder, _, _ in kSources:
            for path in iter_logs(folder):
                out.write(f">>> {path}\n")
                out.write(path.read_text().rstrip("\n") + "\n\n")


def main() -> int:
    write_concat_log()

    data = post_processing(collect())
    header = kIdCols + header_36

    target = Path(store_path)
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("w", newline="") as fout:
        writer = csv.writer(fout)
        writer.writerow(header)
        writer.writerows(data)

    print(f"Written: {target}  ({len(data)} rows, {len(header)} columns)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

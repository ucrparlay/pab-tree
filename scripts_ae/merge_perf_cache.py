#!/usr/bin/env python3
"""Fig. 7: the perf counters as one csv.

HOW TO RUN (from scripts_ae/, after ae_run_perf.sh)
    python3 merge_perf_cache.py

Reads   logs/perf_cache/<tree>/<dis>/<region>.{log,stat}
Writes  data/perf_cache/perf_cache.csv, one row per region, tree and
        distribution: the counts of each event, the region's seconds as the
        driver timed it (time_s), perf's enabled seconds summed over the CPUs
        (enabled_s), and scope: "all" when the counts include the kernel,
        "user" when perf could count user space only (event names end in :u).
"""
import csv
import re
import sys
from pathlib import Path

EVENTS = ["cycles", "instructions", "cache-references", "cache-misses"]
TREES = {"0": "BTree", "1": "CPAM", "2": "PAM", "3": "PMA"}
REGIONS = ["build", "membership", "incre_insert", "incre_delete"]

rows = []
for stat in sorted(Path("logs/perf_cache").glob("*/*/*.stat")):
    tree, dis, region = stat.parts[-3], stat.parts[-2], stat.stem
    if tree not in TREES or region not in REGIONS:
        continue
    counts, enabled, scope = {}, "", "all"
    for line in stat.read_text().splitlines():
        f = line.split(",")
        if line.startswith("#") or len(f) < 4:
            continue
        event = f[2]
        if event.endswith(":u"):
            event, scope = event[:-2], "user"
        if event in EVENTS:
            counts[event] = f[0] if f[0][:1].isdigit() else ""
            if event == "cycles" and f[3]:
                enabled = f"{float(f[3]) / 1e9:.6f}"
    n = time_s = ""
    log = stat.with_suffix(".log")
    if log.exists():
        for line in log.read_text().splitlines():
            if line.startswith("n: "):
                n = line[3:].strip()
            m = re.fullmatch(rf"{region} (\S+)", line.strip())
            if m:
                time_s = m.group(1)
    if not counts or not time_s:
        print(f"skipped {stat}: no counts or no time", file=sys.stderr)
        continue
    rows.append([region, TREES[tree], dis, n]
                + [counts.get(e, "") for e in EVENTS]
                + [time_s, enabled, scope])

order = {"uni": 0, "norm": 1, "seg": 2}
rows.sort(key=lambda r: (order.get(r[2], 9), REGIONS.index(r[0]),
                         list(TREES.values()).index(r[1])))
dst = Path("data/perf_cache/perf_cache.csv")
dst.parent.mkdir(parents=True, exist_ok=True)
with dst.open("w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["region", "tree", "dis", "n"] + EVENTS
               + ["time_s", "enabled_s", "scope"])
    w.writerows(rows)
print(f"Written {len(rows)} rows to {dst}")

#!/bin/bash
#
# HOW TO RUN
#   ./ae_run_incre_updates.sh                          # Fig. 4 on uni, norm, seg
#   WITH_MASSTREE_BWTREE=1 ./ae_run_incre_updates.sh   # with Mass-tree, Bw-tree
#
# Tag 224: a static build, then incremental insertion and deletion in batches
# of 0.01%, 0.1%, 1% and 10%, each followed by queries on the half-full tree.
# Reads ae_data_generate.sh incre. Writes
# logs/incremental_updates_new/<tree>/<dis>/64.log.

set -euo pipefail
source "$(dirname "$0")/common.sh"

n=$INPUT_SIZE

for dis in uni norm seg; do
	for tree in $(trees 0 2 1 3 5 6); do
		log_file="$LOG_DIR/incremental_updates_new/$tree/$dis/64.log"
		mkdir -p "$(dirname "$log_file")"
		: >"$log_file"
		run_tree "$log_file" "$tree" leaf64 \
			-p "$SYNTH_DIR/incre/$dis/${n}_1/ins/100/1.batch.bin" \
			-r "$ROUNDS" -s 0 -t 224 -q 0 -b 0.01 -D "$dis"
	done
done

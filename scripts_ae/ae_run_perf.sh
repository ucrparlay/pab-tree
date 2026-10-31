#!/bin/bash
#
# HOW TO RUN (perf must read the counters: ./docker-run.sh ... --perf)
#   ./ae_run_perf.sh              # Fig. 7: the Skewed (norm) data
#   ./ae_run_perf.sh uni seg      # the other distributions
#
# Hardware counters (cycles, instructions, cache references and misses) of
# four regions of the incremental runs at the 1% step, for PaB-tree, PAM,
# CPAM and PMA: the build of n entries, n/100 Find queries, incremental
# insertion and deletion of all n (tests/perf_cache.cpp). One run per
# region, under perf stat with counting off outside the region. Reads
# ae_data_generate.sh incre. Writes logs/perf_cache/<tree>/<dis>/<region>.
# {log,stat}.

set -euo pipefail
source "$(dirname "$0")/common.sh"

n=$INPUT_SIZE
EVENTS=cycles,instructions,cache-references,cache-misses
# the perf driver of each tree
TREE_TARGET=([0]=perf_btree [1]=perf_cpam [2]=perf_cpam [3]=perf_pma)
# a retry would reuse the control fifos, which a failed run may leave dirty
RETRIES=0

if ! out=$(perf stat -x, -e "$EVENTS" -- true 2>&1) ||
	grep -q -e '<not supported>' -e '<not counted>' <<<"$out"; then
	log "perf cannot read the hardware counters; run the container with --perf"
	echo "$out"
	exit 1
fi

fifo_dir=$(mktemp -d)
trap 'rm -rf "${fifo_dir:?}"' EXIT

# TREE DIS REGION
run() {
	local tree=$1 dis=$2 region=$3 op=ins dir ctl ack
	if [ "$region" = incre_delete ]; then
		op=del
	fi
	dir="$LOG_DIR/perf_cache/$tree/$dis"
	mkdir -p "$dir"
	rm -f "$fifo_dir/ctl" "$fifo_dir/ack"
	mkfifo "$fifo_dir/ctl" "$fifo_dir/ack"
	# read-write, so opening blocks neither side
	exec {ctl}<>"$fifo_dir/ctl" {ack}<>"$fifo_dir/ack"
	RUN_PREFIX=(perf stat -x, -e "$EVENTS" -D -1 --control "fd:$ctl,$ack"
		-o "$dir/$region.stat" -- "${NUMA[@]}")
	echo "n: $n" >"$dir/$region.log"
	run_tree "$dir/$region.log" "$tree" leaf64 \
		-p "$SYNTH_DIR/incre/$dis/${n}_1/$op/100/1.batch.bin" -D "$dis" \
		-pcr "$region" -pcf "$ctl" -pcaf "$ack"
	exec {ctl}>&- {ack}>&-
}

if [ $# -eq 0 ]; then
	set -- norm
fi
for dis; do
	for tree in 0 2 1 3; do
		for region in build membership incre_insert incre_delete; do
			run "$tree" "$dis" "$region"
		done
	done
done

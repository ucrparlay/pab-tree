#!/bin/bash
#
# HOW TO RUN
#   ./ae_run_scalability.sh uni norm seg              # Fig. 9
#   THREADS="1 4 64" ./ae_run_scalability.sh uni      # other thread counts
#
# Tag 3: build a tree of n entries, then insert one batch of n/100 and delete
# one, at each thread count. Reads ae_data_generate.sh single.
# Writes logs/scalability/<tree>/<dis>/<threads>.log.
#
# Thread counts default to 1, 4, 16, 32, 64 up to the core count, the core
# count, then every hyper-thread: 1 ... 112, 224 on the paper's machine. Below
# the hyper-thread count, the parallel trees run on one CPU per core, spread
# over the sockets, with memory interleaved; the concurrent indexes (Mass-tree,
# Bw-tree) are not pinned, as in the paper.

set -euo pipefail
source "$(dirname "$0")/common.sh"

n=$INPUT_SIZE
logical=$(nproc)
cores=$(lscpu -p=CORE,SOCKET | grep -v '^#' | sort -u | wc -l)

if [ -z "${THREADS:-}" ]; then
	THREADS=""
	for t in 1 4 16 32 64; do
		if [ "$t" -lt "$cores" ]; then
			THREADS+="$t "
		fi
	done
	THREADS+="$cores"
	if [ "$logical" -gt "$cores" ]; then
		THREADS+=" $logical"
	fi
fi

# one CPU per core, cores taken round-robin over the sockets, then the
# hyper-thread siblings
mapfile -t cpus < <(lscpu -p=CPU,CORE,SOCKET | grep -v '^#' | sort -t, -k1,1n |
	awk -F, '!($2 in seen) { seen[$2] = 1; print 0, idx[$3]++, $3, $1; next }
		{ print 1, 0, 0, $1 }' |
	sort -k1,1n -k2,2n -k3,3n -k4,4n | awk '{ print $4 }')

# THREADS -> the command prefix of a parallel tree
prefix_for() {
	local t=$1 list
	list=$(IFS=,; echo "${cpus[*]:0:$t}")
	if [ "$t" -ge "$logical" ]; then
		RUN_PREFIX=("${NUMA[@]}")
	elif [ "$t" -eq 1 ]; then
		RUN_PREFIX=(taskset -c "$list")
	else
		RUN_PREFIX=(taskset -c "$list" "${NUMA[@]}")
	fi
}

if [ $# -eq 0 ]; then
	set -- uni norm seg
fi
log "threads: $THREADS"
for dis; do
	for tree in $(trees 0 2 1 3 5 6); do
		for t in $THREADS; do
			if [ "$tree" -ge 5 ]; then
				RUN_PREFIX=()
			else
				prefix_for "$t"
			fi
			# one round on 1 and 4 threads, two above, as in the paper
			rounds=2
			if [ "$t" -le 4 ]; then
				rounds=1
			fi
			log_file="$LOG_DIR/scalability/$tree/$dis/$t.log"
			mkdir -p "$(dirname "$log_file")"
			: >"$log_file"
			export PARLAY_NUM_THREADS=$t
			run_tree "$log_file" "$tree" leaf64 \
				-p "$SYNTH_DIR/single/$dis/${n}_1/ins/100/1.base.bin" \
				-r "$rounds" -s 0 -t 3 -q 0 -b 0.01 -D "$dis"
		done
	done
done
unset PARLAY_NUM_THREADS

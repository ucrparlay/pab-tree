#!/bin/bash
#
# HOW TO RUN (in the container; docker-run.sh full runs it)
#   ./run.sh                                 # every figure, then merge and plot
#   ./run.sh synthetic                       # Fig. 4, 7, 9
#   ./run.sh real                            # Fig. 5, 6, 8
#   ./run.sh fig4 fig9 merge plot            # chosen steps
#   ./run.sh --with-masstree-bwtree fig4     # add Mass-tree and Bw-tree
#   INPUT_SIZE=1000000 ./run.sh synthetic    # a quick check
#   KEEP_DATA=1 ./run.sh                     # keep the generated datasets
#
# Steps: fig4 fig5 fig6 fig7 fig8 fig9 merge plot, the paper's experiments
# outside the appendix but Fig. 1; fig7 needs perf (docker-run.sh --perf).
# A step that needs synthetic data generates it first, and data no later
# step needs is deleted unless KEEP_DATA=1. Mass-tree and Bw-tree (Fig. 4,
# 5, 9) run only with --with-masstree-bwtree. Results: logs/, data/ (csv),
# plots/ (pdf; Fig. 4 is a csv table). A step that fails is listed in
# logs/failed.txt and the rest go on.

set -euo pipefail
source "$(dirname "$0")/common.sh"
cd "$AE_DIR"

steps=()
for arg; do
	case $arg in
	--with-masstree-bwtree) export WITH_MASSTREE_BWTREE=1 ;;
	*) steps+=("$arg") ;;
	esac
done
if [ ${#steps[@]} -eq 0 ]; then
	steps=(all)
fi
case " ${steps[*]} " in
*" all "*) steps+=(fig4 fig7 fig9 fig5 fig6 fig8 merge plot) ;;
esac
case " ${steps[*]} " in
*" synthetic "*) steps+=(fig4 fig7 fig9 merge plot) ;;
esac
case " ${steps[*]} " in
*" real "*) steps+=(fig5 fig6 fig8 merge plot) ;;
esac
want() { [[ " ${steps[*]} " == *" $1 "* ]]; }

# CMD...: run it; a failure is recorded and the run goes on
try() {
	if ! "$@"; then
		echo "$*" >>"$LOG_DIR/failed.txt"
		log "FAILED: $*"
	fi
}

drop() {
	if [ "${KEEP_DATA:-0}" != 1 ]; then
		./ae_data_generate.sh clean "$@"
	fi
}

mkdir -p "$LOG_DIR" data plots
rm -f "$LOG_DIR/failed.txt"

{
	echo "date: $(date -Is)"
	echo "commit: ${PAB_TREE_COMMIT:-$(git -C "$REPO_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)}"
	echo "steps: ${steps[*]}"
	echo "INPUT_SIZE=$INPUT_SIZE ROUNDS=$ROUNDS DATA_PREFIX=$DATA_PREFIX"
	echo "WITH_MASSTREE_BWTREE=$WITH_MASSTREE_BWTREE"
	echo "numactl: ${NUMA[*]:-not available}"
	g++ --version | head -1
	cmake --version | head -1
	perf --version 2>/dev/null || echo "perf: not available"
	lscpu | grep -E '^(Model name|Socket|Core|Thread|CPU\(s\)|NUMA node\(s\))'
	free -g | head -2
} >"$LOG_DIR/env.txt"
cat "$LOG_DIR/env.txt"

# Fig. 4 and 7 read the incremental datasets; Fig. 7 only batches of n/100
if want fig4 || want fig7; then
	if want fig4; then
		try ./ae_data_generate.sh incre
	else
		try ./ae_data_generate.sh incre 100
	fi
	if want fig4; then
		try ./ae_run_incre_updates.sh
	fi
	if want fig7; then
		try ./ae_run_perf.sh
	fi
	drop incre
fi

# Fig. 9 reads the one-batch datasets, one distribution at a time
if want fig9; then
	for dis in uni norm seg; do
		try ./ae_data_generate.sh single "$dis"
		try ./ae_run_scalability.sh "$dis"
		drop single "$dis"
	done
fi

if want fig5; then
	try ./ae_run_osm.sh
fi
if want fig6; then
	try ./ae_run_temporal_graph.sh
fi
if want fig8; then
	try ./ae_run_randomness.sh
fi

# merge LOG_FOLDER SCRIPT [ARG]: when the logs exist
merge() {
	if [ -d "logs/$1" ]; then
		try python3 "$2" "${@:3}"
	fi
}
if want merge; then
	merge incremental_updates_new merge_incre_updates_new.py incre_updates_new
	merge perf_cache merge_perf_cache.py
	merge scalability merge_scalability.py scalability
	for op in insert delete range member; do
		merge osm "merge_osm_$op.py" osm
	done
	merge temp_signal merge_dynamic_graph.py temp_signal
	merge temp_osm merge_dynamic_graph.py temp_osm
	merge randomness merge_randomness.py dynamic
fi

# plot CSV CMD...: when the merged csv exists
plot() {
	if [ -f "$1" ]; then
		try "${@:2}"
	fi
}
if want plot; then
	plot data/incremental_updates_new/incre_updates_new.csv python3 table_fig4.py
	plot data/osm/osm_insert_by_year.csv Rscript plot_fig5_osm.R
	plot data/temp_signal/temp_signal.csv Rscript plot_fig6_temporal_graph.R
	plot data/perf_cache/perf_cache.csv Rscript plot_fig7_perf_cache.R
	plot data/randomness/dynamic.csv Rscript plot_fig8_randomness.R
	plot data/scalability/scalability.csv Rscript plot_fig9_scalability.R
fi

log "results:"
ls -l plots
if [ -s "$LOG_DIR/failed.txt" ]; then
	log "failed steps (logs/failed.txt):"
	cat "$LOG_DIR/failed.txt"
	exit 1
fi
log "done"

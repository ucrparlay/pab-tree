#!/bin/bash
#
# HOW TO RUN
#   source "$(dirname "$0")/common.sh"    # at the top of every ae_*.sh
#
# Settings and helpers shared by the artifact-evaluation scripts. Every
# setting can be overridden from the environment.
# shellcheck disable=SC2034 # the variables are for the sourcing scripts

AE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_DIR=$(dirname "$AE_DIR")

DATA_PREFIX="${DATA_PREFIX:-/data}"    # datasets
INPUT_SIZE="${INPUT_SIZE:-1000000000}" # entries per synthetic dataset
ROUNDS="${ROUNDS:-3}"                  # timed runs, after one warm-up
BUILD_ROOT="${BUILD_ROOT:-$REPO_DIR/build/ae}"
export DATA_PREFIX INPUT_SIZE ROUNDS BUILD_ROOT # the plots read INPUT_SIZE
# a Mass-tree or Bw-tree run still going after RUN_TIMEOUT is killed and
# retried (Bw-tree can hang); a timeout(1) duration, 0 for none. Not
# exported: Fig. 9 sets its own per thread count unless RUN_TIMEOUT comes
# from the environment.
RUN_TIMEOUT_SET=${RUN_TIMEOUT:+1}
RUN_TIMEOUT="${RUN_TIMEOUT:-2h}"
SYNTH_DIR="$DATA_PREFIX/synthetic"
REAL_DIR="$DATA_PREFIX/real_world"
LOG_DIR="$AE_DIR/logs"

# the driver of each tree id (-T)
declare -A TREE_TARGET=(
	[0]=b_test                    # PaB-tree
	[1]=cpam                      # CPAM
	[2]=cpam                      # PAM
	[3]=pma                       # PMA
	[5]=index_microbench_workload # Mass-tree
	[6]=index_microbench_workload # Bw-tree
)

# Mass-tree and Bw-tree run only when asked: 1 adds them (run.sh
# --with-masstree-bwtree), only runs them alone (--only-masstree-bwtree)
WITH_MASSTREE_BWTREE="${WITH_MASSTREE_BWTREE:-0}"
export WITH_MASSTREE_BWTREE

# trees ID...: the ids, less Mass-tree (5) and Bw-tree (6) unless asked for;
# only those two with WITH_MASSTREE_BWTREE=only
trees() {
	local id
	for id; do
		case $id in
		5 | 6)
			case $WITH_MASSTREE_BWTREE in
			1 | only) echo "$id" ;;
			esac
			;;
		*)
			if [ "$WITH_MASSTREE_BWTREE" != only ]; then
				echo "$id"
			fi
			;;
		esac
	done
}

# Mass-tree allocates more than tcmalloc reports by default
export TCMALLOC_LARGE_ALLOC_REPORT_THRESHOLD=1099511627776

# interleave memory over the sockets when allowed; a container needs SYS_NICE
NUMA=()
if command -v numactl >/dev/null && numactl -i all true 2>/dev/null; then
	NUMA=(numactl -i all)
fi
RUN_PREFIX=("${NUMA[@]}")

log() { echo "[$(date '+%F %T')] $*"; }

# cmake options of each build configuration
config_options() {
	case $1 in
	leaf*) echo "-DLEAF=${1#leaf}" ;;
	osm) echo "-DLEAF=64 -DOSM=ON" ;;
	graph) echo "-DLEAF=64 -DDYNAMIC_GRAPH=ON" ;;
	*) return 1 ;;
	esac
}

# build CONFIG TARGET: one build tree per configuration, configured once, so
# a rerun rebuilds only what changed
build() {
	local dir="$BUILD_ROOT/$1" target=$2 options
	options=$(config_options "$1") || {
		log "unknown build configuration: $1"
		return 1
	}
	mkdir -p "$dir"
	if [ ! -f "$dir/CMakeCache.txt" ]; then
		# shellcheck disable=SC2086 # options is a list
		if ! cmake -S "$REPO_DIR" -B "$dir" -DDEBUG=OFF -DJEMA=ON $options \
			>"$dir/cmake.log" 2>&1; then
			tail -20 "$dir/cmake.log"
			return 1
		fi
	fi
	if ! cmake --build "$dir" --target "$target" -j "$(nproc)" \
		>"$dir/$target.log" 2>&1; then
		grep -m 5 -E 'error|Error' "$dir/$target.log"
		return 1
	fi
}

# run_tree LOG TREE CONFIG [DRIVER_OPTION...]: run tree TREE's driver from
# build CONFIG with RUN_PREFIX, appending to LOG. A Mass-tree or Bw-tree run
# that fails or passes RUN_TIMEOUT is retried up to RETRIES times (Bw-tree
# crashes or hangs now and then); only the last attempt reaches LOG, since the
# merges drop a run logged twice, and the merges write N.A. for what it did
# not print. The other trees run once, with no limit. A final failure goes to
# logs/failed.txt and does not stop the caller.
run_tree() {
	local log_file=$1 tree=$2 config=$3
	shift 3
	local target=${TREE_TARGET[$tree]} attempt status why tmp="$log_file.part"
	local retries=0 limit=() note=""
	mkdir -p "$(dirname "$log_file")" "$LOG_DIR"
	if ! build "$config" "$target"; then
		echo "build $config $target" >>"$LOG_DIR/failed.txt"
		return 0
	fi
	case $tree in
	5 | 6)
		retries=${RETRIES:-2}
		# right before the driver, so that numactl's memory policy still
		# applies to it; --foreground keeps Ctrl-C reaching it
		if [ "$RUN_TIMEOUT" != 0 ]; then
			limit=(timeout --foreground -k 1m "$RUN_TIMEOUT")
			note=" (limit $RUN_TIMEOUT)"
		fi
		;;
	esac
	for ((attempt = 0; attempt <= retries; attempt++)); do
		log "${RUN_PREFIX[*]} $target -T $tree $* >> ${log_file#"$AE_DIR"/}$note"
		status=0
		"${RUN_PREFIX[@]}" "${limit[@]}" "$BUILD_ROOT/$config/$target" \
			-T "$tree" "$@" 2>&1 | tee "$tmp" || status=${PIPESTATUS[0]}
		if [ "$status" = 0 ]; then
			cat "$tmp" >>"$log_file"
			rm -f "$tmp"
			return 0
		fi
		why="exit $status"
		if [ "$status" = 124 ]; then
			why="timed out after $RUN_TIMEOUT"
		fi
		log "failed ($why): attempt $((attempt + 1))"
	done
	cat "$tmp" >>"$log_file"
	rm -f "$tmp"
	echo "${log_file#"$AE_DIR"/}: $target -T $tree $* ($why)" \
		>>"$LOG_DIR/failed.txt"
}

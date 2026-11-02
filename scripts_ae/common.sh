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
SYNTH_DIR="$DATA_PREFIX/synthetic"
REAL_DIR="$DATA_PREFIX/real_world"
LOG_DIR="$AE_DIR/logs"

# Hosted datasets: datasets.sha256 names each file, relative to $DATA_PREFIX.
# DOWNLOAD=0 (docker-run.sh --no-download) fetches nothing.
HF_URL="${HF_URL:-https://huggingface.co/datasets/romaLzhih/pab-tree-data/resolve/main}"
DOWNLOAD="${DOWNLOAD:-1}"

# the driver of each tree id (-T)
declare -A TREE_TARGET=(
	[0]=b_test                    # PaB-tree
	[1]=cpam                      # CPAM
	[2]=cpam                      # PAM
	[3]=pma                       # PMA
	[5]=index_microbench_workload # Mass-tree
	[6]=index_microbench_workload # Bw-tree
)

# Mass-tree and Bw-tree run only when asked: run.sh --with-masstree-bwtree
WITH_MASSTREE_BWTREE="${WITH_MASSTREE_BWTREE:-0}"
export WITH_MASSTREE_BWTREE

# trees ID...: the ids, less Mass-tree (5) and Bw-tree (6) unless asked for
trees() {
	local id
	for id; do
		case $id in
		5 | 6)
			if [ "$WITH_MASSTREE_BWTREE" = 1 ]; then
				echo "$id"
			fi
			;;
		*) echo "$id" ;;
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

# fetch PATH: download PATH, as datasets.sha256 names it, into $DATA_PREFIX
# and check it. Fails when it is not hosted or will not download. A broken
# transfer resumes; five tries. HTTP 416: .part already reaches the end of the
# file, left by a run killed during the checksum, so the checksum decides.
fetch() {
	local path=$1 tmp="$DATA_PREFIX/$1.part" sum attempt code
	sum=$(awk -v path="$path" '$2 == path { print $1 }' "$AE_DIR/datasets.sha256")
	if [ "$DOWNLOAD" = 0 ] || [ -z "$sum" ]; then
		return 1
	fi
	mkdir -p "$(dirname "$tmp")"
	log "download $path"
	for ((attempt = 1; attempt <= 5; attempt++)); do
		if code=$(curl -fL -C - -o "$tmp" -w '%{http_code}' "$HF_URL/$path") ||
			[ "$code" = 416 ]; then
			if echo "$sum  $tmp" | sha256sum -c --quiet -; then
				mv "$tmp" "$DATA_PREFIX/$path"
				return 0
			fi
			rm -f "$tmp"
		fi
		log "failed: attempt $attempt"
	done
	rm -f "$tmp"
	return 1
}

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
# build CONFIG with RUN_PREFIX, appending to LOG. A run that fails is retried
# up to RETRIES times (Bw-tree crashes now and then); only the last attempt
# reaches LOG, since the merges drop a run logged twice. A final failure goes
# to logs/failed.txt and does not stop the caller.
run_tree() {
	local log_file=$1 tree=$2 config=$3
	shift 3
	local target=${TREE_TARGET[$tree]} attempt tmp="$log_file.part"
	mkdir -p "$(dirname "$log_file")" "$LOG_DIR"
	if ! build "$config" "$target"; then
		echo "build $config $target" >>"$LOG_DIR/failed.txt"
		return 0
	fi
	for ((attempt = 0; attempt <= ${RETRIES:-2}; attempt++)); do
		log "${RUN_PREFIX[*]} $target -T $tree $* >> ${log_file#"$AE_DIR"/}"
		if "${RUN_PREFIX[@]}" "$BUILD_ROOT/$config/$target" -T "$tree" "$@" \
			2>&1 | tee "$tmp"; then
			cat "$tmp" >>"$log_file"
			rm -f "$tmp"
			return 0
		fi
		log "failed: attempt $((attempt + 1))"
	done
	cat "$tmp" >>"$log_file"
	rm -f "$tmp"
	echo "${log_file#"$AE_DIR"/}: $target -T $tree $*" >>"$LOG_DIR/failed.txt"
}

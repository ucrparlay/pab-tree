#!/bin/bash
# Measure hardware cache behaviour of Build / BatchInsert / BatchDelete.
#
# The binary is run under `perf` with counting initially disabled (-D -1) and
# re-enabled over exactly one call of the operation under test, via perf's
# --control fd protocol (see tests/perf_control.h). Without that gating the
# counts would be swamped by reading the 100M-entry input file and by the
# untimed setup Build that precedes every BatchInsert / BatchDelete.
#
# Two modes:
#   stat   (default) exact aggregate counts per region -> data/<log>/*.csv
#   record sampled profile -> per-function attribution via `perf report`
#
# Sample usage at the end of this file.

set -o pipefail

# ---------------------------------------------------------------- configuration
MODE="${MODE:-stat}"       # stat | record
NODES="${NODES:-100000000}"
DIM="${DIM:-1}"
ROUND=1                    # MUST stay 1: perf accumulates across time_loop rounds
SUMMARY=0                  # MUST stay 0: -s 1 sweeps 4 ratios into one count
QUERY_TYPE=0               # keep query regions out of the run
BATCH_RATIO="${BATCH_RATIO:-0.1}"
THREADS="${THREADS:-1}"    # 1 = clean per-function attribution; else real-world
DIS_SEQ=(${DIS_SEQ:-uni exp norm dual_norm})
REGIONS=(${REGIONS:-build insert delete})
DATA_PREFIX="${DATA_PREFIX:-/data/zmen002/kv_pair}"
EXT="${EXT:-.bin}"
DROP_CACHES="${DROP_CACHES:-1}"

# cycles+instructions sit on fixed counters, the other four on the 4 general
# purpose ones -- no multiplexing on this Icelake part. Adding a 5th general
# purpose event starts time-sharing and every count becomes an extrapolation.
EVENTS="${EVENTS:-cycles,instructions,cache-references,cache-misses,LLC-load-misses,L1-dcache-load-misses}"

LOG_NAME="${LOG_NAME:-perf_cache}"
LOG_PATH="logs/${LOG_NAME}"
CSV="data/${LOG_NAME}/perf_cache.csv"

# region -> the -t tag that makes DefaultTestFunc execute it
declare -A REGION_TAG=([build]=0 [insert]=1 [delete]=2)
# region -> which dataset subdir holds its base/batch pair
declare -A REGION_OP=([build]=ins [insert]=ins [delete]=del)

mkdir -p "${LOG_PATH}" "$(dirname "${CSV}")"

# ------------------------------------------------------------------ csv header
if [[ "${MODE}" == "stat" ]]; then
	printf 'region,tree,dis,n,batch_ratio,threads,%s\n' \
		"$(echo "${EVENTS}" | tr ',' ',')" >"${CSV}"
fi

# Run one (solver, tree, dis, node, region) point under perf.
function run_one() {
	local exe="$1" tree_name="$2" tree="$3" dis="$4" node="$5" region="$6"

	local tag="${REGION_TAG[$region]}"
	local batch_op="${REGION_OP[$region]}"
	local x
	x=$(bc <<<"scale=0; 1/${BATCH_RATIO}") || return 1
	local base="${DATA_PREFIX}/${dis}/${node}_${DIM}/${batch_op}/${x}/1.base${EXT}"

	if [[ ! -f "${base}" ]]; then
		echo "!!! missing ${base} -- skipping ${region}/${dis}/${node}" >&2
		return 0
	fi

	local stem="${LOG_PATH}/${tree_name}_${dis}_${node}_${region}"
	mkdir -p "${stem%/*}"

	# --- perf control fifos. Opened read-write so neither side blocks on open.
	local ctl_fifo ack_fifo
	ctl_fifo=$(mktemp -u /tmp/perf_ctl.XXXXXX.fifo)
	ack_fifo=$(mktemp -u /tmp/perf_ack.XXXXXX.fifo)
	mkfifo "${ctl_fifo}" "${ack_fifo}" || return 1
	exec {ctl_fd}<>"${ctl_fifo}"
	exec {ack_fd}<>"${ack_fifo}"

	[[ "${DROP_CACHES}" == "1" && -x /usr/bin/drop_caches ]] && /usr/bin/drop_caches

	export PARLAY_NUM_THREADS="${THREADS}"

	local -a prog=("${exe}" -p "${base}" -r "${ROUND}" -s "${SUMMARY}"
		-t "${tag}" -q "${QUERY_TYPE}" -b "${BATCH_RATIO}"
		-T "${tree}" -D "${dis}"
		-pcf "${ctl_fd}" -pcaf "${ack_fd}" -pcr "${region}")

	echo ">>> ${region} ${tree_name} ${dis} n=${node} threads=${THREADS}"

	if [[ "${MODE}" == "stat" ]]; then
		# -x, gives machine-readable "count,unit,event,..." lines
		perf stat -x, -e "${EVENTS}" -D -1 \
			--control "fd:${ctl_fd},${ack_fd}" \
			-o "${stem}.stat" -- "${prog[@]}" >"${stem}.out" 2>"${stem}.err"
	else
		# NOTE: -F matters. At perf's default period a ~20ms region yields ~50
		# cache-miss samples, which is not enough to rank functions. Raise FREQ
		# (or widen the region with a larger -b / n) until "# Samples" in the
		# report is in the thousands.
		perf record -s -D -1 -F "${FREQ:-20000}" \
			--control "fd:${ctl_fd},${ack_fd}" \
			-o "${stem}.data" -e "${EVENTS}" \
			-- "${prog[@]}" >"${stem}.out" 2>"${stem}.err"
		perf report --stdio --percent-limit 1 \
			--input="${stem}.data" >"${stem}.report" 2>/dev/null
	fi
	local rc=$?

	exec {ack_fd}>&-
	exec {ctl_fd}>&-
	rm -f "${ctl_fifo}" "${ack_fifo}"

	if [[ ${rc} -ne 0 ]]; then
		echo "!!! perf exited ${rc}; see ${stem}.err" >&2
		return 0
	fi

	if [[ "${MODE}" == "stat" ]]; then
		# Pull the counts out in the order EVENTS lists them.
		local row="${region},${tree_name},${dis},${node},${BATCH_RATIO},${THREADS}"
		local ev
		for ev in ${EVENTS//,/ }; do
			local v
			v=$(awk -F, -v e="${ev}" '$3==e {print $1; exit}' "${stem}.stat")
			row+=",${v:-NA}"
		done
		echo "${row}" | tee -a "${CSV}"
	else
		# Print just the cache-misses section: the report concatenates one
		# section per event, each opening with "# Samples: N of event 'X'".
		# Demangled template names are enormous, so squash them to the
		# innermost identifier to keep the terminal readable.
		echo "--- cache-misses attribution (${region} ${dis}) ---"
		awk '
			/^# Samples:.*of event/ { inseg = (/'"'"'cache-misses'"'"'/) ? 1 : 0 }
			inseg && /^# (Samples|Event count)/ { print; next }
			inseg && /^ +[0-9]/ {
				pct = $1; sym = $0
				sub(/.*\[[.k]\] /, "", sym)          # strip up to the symbol
				sub(/[<(].*/, "", sym)                # drop template/arg soup
				printf "  %-8s %s\n", pct, sym
			}
		' "${stem}.report"
	fi
}

function run_tree() {
	local solver="$1" tree="$2" tree_name="$3"
	local exe="../build/${solver}"
	[[ -x "${exe}" ]] || {
		echo "!!! ${exe} not built -- skipping ${tree_name}" >&2
		return 0
	}
	local dis node region
	for dis in "${DIS_SEQ[@]}"; do
		for node in "${NODES[@]}"; do
			for region in "${REGIONS[@]}"; do
				run_one "${exe}" "${tree_name}" "${tree}" "${dis}" "${node}" "${region}"
			done
		done
	done
}

function run_cmake() {
	local cur_path
	cur_path=$(pwd)
	cd ../build || exit 1
	cmake -DDEBUG=OFF -DOSM=OFF -DDYNAMIC_GRAPH=OFF \
		-DCMAKE_CXX_COMPILER=g++ "${HOME}/ParallelMultiwayOrderedSet" >/dev/null
	make b_test -j"$(nproc)" || exit 1
	cd "${cur_path}" || exit 1
}

[[ "${SKIP_BUILD:-0}" == "1" ]] || run_cmake

run_tree b_test 0 BTree

if [[ "${MODE}" == "stat" ]]; then
	echo
	echo "=== ${CSV} ==="
	column -s, -t <"${CSV}"
fi

# Sample usage:
#
#   # all three regions, uniform only, 1e8, serial (clean attribution)
#   cd scripts && DIS_SEQ=uni ./perf_cache.sh
#
#   # just batch insert, all four distributions, parallel (real-world rates)
#   cd scripts && REGIONS=insert THREADS=64 ./perf_cache.sh
#
#   # per-function breakdown of where the misses land, instead of totals
#   cd scripts && MODE=record REGIONS=delete DIS_SEQ=uni ./perf_cache.sh
#
#   # quick check against a small dataset, reusing the existing build
#   cd scripts && SKIP_BUILD=1 DROP_CACHES=0 NODES=1000000 DIS_SEQ=uni \
#       DATA_PREFIX=/ssd1/zmen002/kv_pair ./perf_cache.sh
#
#   # narrow the event set (fewer events = no risk of counter multiplexing)
#   cd scripts && EVENTS=cache-references,cache-misses ./perf_cache.sh

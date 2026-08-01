#!/bin/bash
# Measure hardware cache behaviour of Build / BatchInsert / BatchDelete and of
# the incremental (by-step) insert and delete chains.
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
# ============================================================================
# HOW TO RUN
# ============================================================================
# Always run from inside scripts/ (paths to ../build and data/, logs/ are
# relative). Everything is configured through environment variables -- there are
# no positional arguments.
#
#   cd scripts && DIS_SEQ=uni ./perf_cache.sh
#
# Results land in two places:
#   data/${LOG_NAME}/perf_cache.csv   one row per (region, tree), all events
#   logs/${LOG_NAME}/*.stat|.report   raw perf output, one file per run
#
# --- knobs -------------------------------------------------------------------
#   LOG_NAME     output dir name under data/ and logs/   (default perf_cache)
#   TREES        space-separated "solver:-T:name" specs
#                (default "b_test:0:BTree cpam:1:CPAM cpam:2:PAM pma:3:PMA";
#                 CPAM and PAM share the cpam binary, selected by -T)
#   REGIONS      subset of "build membership incre_insert incre_delete
#                insert delete"    (default "build membership incre_insert
#                incre_delete" -- the tag 1<<5 / 1<<6 / 1<<7 family)
#   DIS_SEQ      subset of "uni exp norm dual_norm"      (default all four)
#   NODES        base size, e.g. 100000000 / 1000000000  (default 1e8)
#   BATCH_RATIO  batch as a fraction of base (default 0.1); also picks the
#                dataset dir, <op>/<1/ratio>.  For incre_insert / incre_delete
#                it is the STEP of the chain, not a batch size -- see below.
#   THREADS      PARLAY_NUM_THREADS                      (default 1)
#   MODE         stat | record                           (default stat)
#   FREQ         record mode sampling frequency          (default 20000)
#   EVENTS       perf event list (see counter budget note below)
#   DATA_PREFIX  root for insert/delete    (default /data/zmen002/kv_pair)
#   INCRE_PREFIX root for the 1<<5/6/7 family
#                                    (default /data/zmen002/kv_pair_incre)
#   EXT          .bin | .in                              (default .bin)
#   SKIP_BUILD   1 = do not re-run cmake/make            (default 0)
#   DROP_CACHES  1 = drop page cache before each run     (default 1)
#   NUMACTL      workload launcher prefix     (default "numactl -i all");
#                set NUMACTL= (empty) for first-touch placement
#
# --- recipes -----------------------------------------------------------------
#   # THE ONE: build -> membership -> incre insert -> incre delete, 1e9 at a 1%
#   # step, uni and exp, all four trees. ~4h; drop PAM on a <128GB box.
#   cd scripts && NODES=1000000000 BATCH_RATIO=0.01 DIS_SEQ="uni exp" \
#       THREADS=224 DROP_CACHES=1 \
#       TREES="b_test:0:BTree cpam:1:CPAM pma:3:PMA cpam:2:PAM" \
#       LOG_NAME=perf_incre_1e9_b001 ./perf_cache.sh \
#     && ./merge_perf_cache.py -v perf_incre_1e9_b001
#
#   # the older batch-update regions (different tags, different dataset root)
#   cd scripts && REGIONS="insert delete" NODES=100000000 DIS_SEQ=uni \
#       THREADS=64 ./perf_cache.sh
#
#   # 1e9. PAM needs ~110GB at insert -- drop it on a <128GB box, and put it
#   # last regardless so an OOM does not cost the other trees.
#   cd scripts && NODES=1000000000 DIS_SEQ=uni THREADS=64 \
#       TREES="b_test:0:BTree cpam:1:CPAM pma:3:PMA" \
#       LOG_NAME=perf_cache_1e9 ./perf_cache.sh
#
#   # serial -- the meaningful setting for misses/element. At THREADS=64 idle
#   # parlay workers spinning in scheduler::get_own_job are counted too. Not
#   # advisable for the incre regions at 1e9: they walk the WHOLE input.
#   cd scripts && THREADS=1 DIS_SEQ=uni REGIONS=insert ./perf_cache.sh
#
#   # which functions the misses land in, instead of totals
#   cd scripts && MODE=record REGIONS=delete DIS_SEQ=uni ./perf_cache.sh
#
#   # fast smoke test: small dataset, existing build, keep the page cache
#   cd scripts && SKIP_BUILD=1 DROP_CACHES=0 NODES=1000000 DIS_SEQ=uni \
#       DATA_PREFIX=/ssd1/zmen002/kv_pair LOG_NAME=smoke ./perf_cache.sh
#
#   # one tree only
#   cd scripts && TREES=cpam:2:PAM DIS_SEQ=uni ./perf_cache.sh
#
#   # first-touch placement instead of interleaved (not comparable to the
#   # rest of the project, which all uses numactl -i all)
#   cd scripts && NUMACTL= DIS_SEQ=uni ./perf_cache.sh
#
#   # fewer events (zero risk of counter multiplexing)
#   cd scripts && EVENTS=cache-references,cache-misses ./perf_cache.sh
#
# --- the regions -------------------------------------------------------------
#   region        tag     what perf counts                     per-elem divisor
#   build         1<<5    one Build() over the whole input      NODES
#   membership    1<<5    the half-half Contain() probe pass    NODES*0.01
#   incre_insert  1<<6    ALL 1/BATCH_RATIO BatchInsert calls   NODES
#   incre_delete  1<<7    ALL 1/BATCH_RATIO BatchDelete calls   NODES
#   insert        1       one BatchInsert() of the batch        NODES*BATCH_RATIO
#   delete        2       one BatchDelete() of the batch        NODES*BATCH_RATIO
#
# * MIND THE DIVISOR. incre_insert / incre_delete count the entire chain, which
#   walks the whole input in 1/BATCH_RATIO steps -- so BATCH_RATIO is the STEP
#   SIZE, not the amount of work. All of NODES gets inserted (or deleted)
#   whatever the ratio; a smaller ratio just means more, smaller batches. That
#   is the opposite of `insert`/`delete`, where BATCH_RATIO IS the work.
#   Comparing incre_insert against insert is therefore comparing "build the
#   whole tree incrementally" against "add one batch to a finished tree".
# * `membership` measures the HALF-HALF probe only: half the keys are in the
#   tree, half are not, shuffled. It runs inside incre_update_query, which the
#   tag 1<<5 block calls right after the build -- so it needs no -q bit, and
#   the probed tree holds NODES/2 entries (BuildTree's kOutputRatio=2 hands
#   back the half-filled tree). The two single-sided passes next to it in
#   incre_update_query are commented out upstream and unarmed regardless.
# * RUN ONE TAG PER PROCESS. incre_update_query is shared by tags 1<<5, 1<<6
#   and 1<<7, so a -t with several of them set would run the membership pass
#   several times and perf would sum them. run_one always passes exactly one.
# * ROUND and SUMMARY are pinned to 1 and 0 below and must stay there: perf
#   sums over every enable/disable interval, so -r N reports N+1 executions
#   added together (time_loop's warmup pass counts too), and -s 1 would sweep
#   four batch ratios into a single count. For the same reason the incre
#   ratio sweep in test_framework.h collapses to the single -b ratio whenever
#   -pcf/-pcaf are set.
# * Verify no counter multiplexing before trusting a changed EVENTS list:
#     awk -F, 'NF>=5 && $5+0<100' logs/<LOG_NAME>/*.stat
#   must print nothing (field 5 is the percentage of time the event ran).
# * Single-shot runs. Repeat the whole invocation with a different LOG_NAME for
#   error bars; averaging inside one process is not possible given -r 1.
#   Interleaved placement is far more repeatable: measured spread on BTree
#   insert cache-misses was 22.6% under first-touch vs 1.0% with numactl,
#   because first-touch placement depends on which worker faulted each page.
# * NUMA placement changes the miss COUNT, not just latency: this box has a
#   per-socket LLC and the counter sums both, so interleaving repartitions
#   the working set across the two caches. Measured at 1e8 on BTree,
#   interleave vs first-touch: build -5.3% misses but +30.2% cycles;
#   insert -19.3% misses and -20.3% cycles. Do not mix numa modes in one
#   table -- the `numa` column in the csv records which was used.
# ============================================================================

set -o pipefail

# ---------------------------------------------------------------- configuration
MODE="${MODE:-stat}"       # stat | record
NODES="${NODES:-100000000}"
DIM="${DIM:-1}"
ROUND=1                    # MUST stay 1: perf accumulates across time_loop rounds
SUMMARY=0                  # MUST stay 0: -s 1 sweeps 4 ratios into one count
# NOTE: QUERY_TYPE is per region now (see REGION_QUERY): the update regions
# want 0 so no query work runs at all, but `membership` needs -q 1 to make
# DefaultTestFunc execute the membership block.
BATCH_RATIO="${BATCH_RATIO:-0.1}"
THREADS="${THREADS:-1}"    # 1 = clean per-function attribution; else real-world
DIS_SEQ=(${DIS_SEQ:-uni exp norm dual_norm})
REGIONS=(${REGIONS:-build membership incre_insert incre_delete})
DATA_PREFIX="${DATA_PREFIX:-/data/zmen002/kv_pair}"
# NOTE: the tag 1<<5 / 1<<6 / 1<<7 family reads a different dataset root. Those
# files are laid out so that consecutive windows of base_n*ratio entries ARE the
# batches, which only holds for the ratio they were generated at; the standard
# kv_pair datasets are globally shuffled, so every batch there comes out uniform
# no matter what <dis> says. They also have no base/batch pair -- the whole
# <i>.batch<EXT> file is the stream, and it holds all NODES entries.
INCRE_PREFIX="${INCRE_PREFIX:-/data/zmen002/kv_pair_incre}"
EXT="${EXT:-.bin}"
DROP_CACHES="${DROP_CACHES:-1}"

# NOTE: matches the rest of the project (summary.sh, incre_updates.sh, ... all
# use `numactl -i all`). Interleaving matters here beyond comparability: this box
# has 2 NUMA nodes at 2.0x remote latency, and under first-touch the placement
# depends on which parlay worker happened to fault each page in, which varies
# per run -- measured 22.6% run-to-run spread on BTree insert cache-misses
# first-touch vs 1.0% interleaved. Set NUMACTL= (empty) to disable.
# Kept as an array: perf execs the workload directly, so an unsplit
# "numactl -i all" string would be treated as one filename.
NUMACTL="${NUMACTL-numactl -i all}"
NUMACTL_CMD=(${NUMACTL})
NUMA_LABEL="${NUMACTL:+interleave}"
NUMA_LABEL="${NUMA_LABEL:-first-touch}"

# cycles+instructions sit on fixed counters, the other four on the 4 general
# purpose ones -- no multiplexing on this Icelake part. Adding a 5th general
# purpose event starts time-sharing and every count becomes an extrapolation.
EVENTS="${EVENTS:-cycles,instructions,cache-references,cache-misses,LLC-load-misses,L1-dcache-load-misses}"

LOG_NAME="${LOG_NAME:-perf_cache}"
LOG_PATH="logs/${LOG_NAME}"
CSV="data/${LOG_NAME}/perf_cache.csv"

# region -> the -t tag that makes DefaultTestFunc execute it. Exactly one tag
# per run: incre_update_query is shared by 1<<5 / 1<<6 / 1<<7, so setting two of
# them would run the membership pass twice and perf would sum both.
# 32 = 1<<5 static build then query, 64 = 1<<6 incre insert, 128 = 1<<7 incre
# delete. `build` and `membership` are two regions of the same tag-32 run,
# separated by -pcr.
declare -A REGION_TAG=(
	[build]=32 [membership]=32 [incre_insert]=64 [incre_delete]=128
	[insert]=1 [delete]=2
)
# region -> -q value. The 1<<5/6/7 family calls incre_update_query
# unconditionally, so it needs no query bit; the legacy insert/delete regions do
# no query work at all.
declare -A REGION_QUERY=(
	[build]=0 [membership]=0 [incre_insert]=0 [incre_delete]=0
	[insert]=0 [delete]=0
)
# region -> which dataset subdir it reads.
declare -A REGION_OP=(
	[build]=ins [membership]=ins [incre_insert]=ins [incre_delete]=del
	[insert]=ins [delete]=del
)
# region -> which file -p points at, and thus which root. The 1<<5/6/7 family
# takes the batch stream out of INCRE_PREFIX (there is no .base there); the
# legacy regions take the .base of a base/batch pair out of DATA_PREFIX.
declare -A REGION_STEM=(
	[build]=batch [membership]=batch [incre_insert]=batch [incre_delete]=batch
	[insert]=base [delete]=base
)

mkdir -p "${LOG_PATH}" "$(dirname "${CSV}")"

# ------------------------------------------------------------------ csv header
if [[ "${MODE}" == "stat" ]]; then
	printf 'region,tree,dis,n,batch_ratio,threads,numa,%s\n' "${EVENTS}" >"${CSV}"
fi

# Run one (solver, tree, dis, node, region) point under perf.
function run_one() {
	local exe="$1" tree_name="$2" tree="$3" dis="$4" node="$5" region="$6"

	local tag="${REGION_TAG[$region]}"
	local batch_op="${REGION_OP[$region]}"
	local stem_kind="${REGION_STEM[$region]}"
	local x
	x=$(bc <<<"scale=0; 1/${BATCH_RATIO}") || return 1

	local root="${DATA_PREFIX}"
	[[ "${stem_kind}" == "batch" ]] && root="${INCRE_PREFIX}"
	local base="${root}/${dis}/${node}_${DIM}/${batch_op}/${x}/1.${stem_kind}${EXT}"

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
		-t "${tag}" -q "${REGION_QUERY[$region]}" -b "${BATCH_RATIO}"
		-T "${tree}" -D "${dis}"
		-pcf "${ctl_fd}" -pcaf "${ack_fd}" -pcr "${region}")

	echo ">>> ${region} ${tree_name} ${dis} n=${node} threads=${THREADS} numa=${NUMA_LABEL}"

	if [[ "${MODE}" == "stat" ]]; then
		# -x, gives machine-readable "count,unit,event,..." lines
		perf stat -x, -e "${EVENTS}" -D -1 \
			--control "fd:${ctl_fd},${ack_fd}" \
			-o "${stem}.stat" -- "${NUMACTL_CMD[@]}" "${prog[@]}" >"${stem}.out" 2>"${stem}.err"
	else
		# NOTE: -F matters. At perf's default period a ~20ms region yields ~50
		# cache-miss samples, which is not enough to rank functions. Raise FREQ
		# (or widen the region with a larger -b / n) until "# Samples" in the
		# report is in the thousands.
		perf record -s -D -1 -F "${FREQ:-20000}" \
			--control "fd:${ctl_fd},${ack_fd}" \
			-o "${stem}.data" -e "${EVENTS}" \
			-- "${NUMACTL_CMD[@]}" "${prog[@]}" >"${stem}.out" 2>"${stem}.err"
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
		local row="${region},${tree_name},${dis},${node},${BATCH_RATIO},${THREADS},${NUMA_LABEL}"
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

# NOTE: "solver:-T:name" -- CPAM and PAM share the cpam binary, selected by -T.
# TREES overrides the set, e.g. TREES="b_test:0:BTree cpam:1:CPAM".
TREE_SPECS=(${TREES:-b_test:0:BTree cpam:1:CPAM cpam:2:PAM pma:3:PMA})

function run_cmake() {
	local cur_path spec
	cur_path=$(pwd)
	cd ../build || exit 1
	cmake -DDEBUG=OFF -DOSM=OFF -DDYNAMIC_GRAPH=OFF \
		-DCMAKE_CXX_COMPILER=g++ "${HOME}/ParallelMultiwayOrderedSet" >/dev/null
	local -A seen=()
	for spec in "${TREE_SPECS[@]}"; do
		local solver="${spec%%:*}"
		[[ -n "${seen[$solver]:-}" ]] && continue
		seen[$solver]=1
		make "${solver}" -j"$(nproc)" || exit 1
	done
	cd "${cur_path}" || exit 1
}

[[ "${SKIP_BUILD:-0}" == "1" ]] || run_cmake

for spec in "${TREE_SPECS[@]}"; do
	IFS=: read -r spec_solver spec_tree spec_name <<<"${spec}"
	run_tree "${spec_solver}" "${spec_tree}" "${spec_name}"
done

if [[ "${MODE}" == "stat" ]]; then
	echo
	echo "=== ${CSV} ==="
	column -s, -t <"${CSV}"
	echo
	echo "derived per-element metrics:"
	echo "  ./merge_perf_cache.py ${LOG_NAME}"
fi

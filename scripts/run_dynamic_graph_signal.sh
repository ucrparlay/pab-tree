#!/bin/bash
#
set -o xtrace

/usr/bin/drop_caches

declare -a leaf_wrap_seq=(64)

Nodes=(1000000000)
Dim=(1)
tag=64
summary=0
query_type=$((2#00))

log_path="logs/temp_signal"
mkdir -p "${log_path}"

data_prefix="/data/zmen002/geometry/temporal_graph/signal"

batch_op_seq=("ins")
dis_seq=("redd" "ref" "sem")
batch_ratio=0.01

function run_test() {
	local leaf_idx="$1"
	local solver="$2"
	local tree="$3"
	local round="$4"

	local exe="../build/${solver}"

	for dim in "${Dim[@]}"; do
		for node in "${Nodes[@]}"; do
			for batch_op in "${batch_op_seq[@]}"; do
				for dis in "${dis_seq[@]}"; do

					local dest="${log_path}/${tree}/${dis}/${leaf_wrap_seq[${leaf_idx}]}.log"
					local data_path="${data_prefix}/${dis}/0.bin"
					mkdir -p "${dest%/*}"
					: >"${dest}"
					echo ">>>${dest}"

					numactl -i all "${exe}" -p "${data_path}" -r "${round}" -s ${summary} -t ${tag} -q ${query_type} -b "${batch_ratio}" -T "${tree}" -D "${dis}" 2>&1 | tee -a "${dest}"
				done
			done
		done
	done
}

function run_cmake() {
	# /usr/bin/drop_caches
	local leaf_idx="$1"
	local solver="$2"

	local cur_path
	cur_path=$(pwd)
	cd ../build || exit
	rm -rf *
	cmake -DDEBUG=OFF -DCMAKE_CXX_COMPILER=g++ -DOSM=OFF -DDYNAMIC_GRAPH=ON -DJEMA=ON -DLEAF="${leaf_wrap_seq[${leaf_idx}]}" "${HOME}/ParallelMultiwayOrderedSet"
	make "${solver}"
	cd "${cur_path}" || exit
}

# Masstree
# export TCMALLOC_LARGE_ALLOC_REPORT_THRESHOLD=1099511627776
# tree=5
# solver="index_microbench_workload"
# run_cmake 0 "${solver}"
# run_test 0 "${solver}" "${tree}"
#
# BwTree
# export TCMALLOC_LARGE_ALLOC_REPORT_THRESHOLD=1099511627776
# tree=6
# solver="index_microbench_workload"
# run_cmake 0 "${solver}"
# run_test 0 "${solver}" "${tree}"

# PMA
tree=3
solver="pma"
round=4
run_cmake 0 "${solver}"
run_test 0 "${solver}" "${tree}" "${round}"

# PAM
tree=2
solver="cpam"
round=4
run_cmake 0 "${solver}"
run_test 0 "${solver}" "${tree}" "${round}"

# CPAM
tree=1
solver="cpam"
round=4
run_cmake 0 "${solver}"
run_test 0 "${solver}" "${tree}" "${round}"

# ours
solver="b_test"
tree=0
round=4
for idx in "${!leaf_wrap_seq[@]}"; do
	run_cmake "${idx}" "${solver}"
	run_test "${idx}" "${solver}" "${tree}" "${round}"
done

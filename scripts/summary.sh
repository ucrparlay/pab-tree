#!/bin/bash
#
set -o xtrace

/usr/bin/drop_caches

declare -a leaf_wrap_seq=(2 4 8 16 32 64 128 256 512 1024)

Nodes=(1000000000)
Dim=(1)
tag=$((2#0011))
summary=1
query_type=$((2#111))
instance_n=1
round=4

log_path="logs/summary"
mkdir -p "${log_path}"

data_prefix="/data/zmen002/kv_pair"
batch_op_seq=("ins")
dis_seq=("uni" "exp" "norm" "dual_norm")
batch_ratio=0.01

function run_test() {
	local leaf_idx="$1"
	local solver="$2"
	local tree="$3"

	local exe="../build/${solver}"

	for dim in "${Dim[@]}"; do
		for dis in "${dis_seq[@]}"; do
			for batch_op in "${batch_op_seq[@]}"; do
				for node in "${Nodes[@]}"; do

					local x=$(bc <<<"scale=0; 1/$batch_ratio") || return 1
					local file_prefix="${data_prefix}/${dis}/${node}_${dim}/${batch_op}/${x}"

					local dest="${log_path}/${tree}/${dis}/${leaf_wrap_seq[${leaf_idx}]}.log"
					mkdir -p "${dest%/*}"
					: >"${dest}"
					echo ">>>${dest}"

					for ((j = 1; j <= instance_n; j++)); do
						numactl -i all "${exe}" -p "${file_prefix}/${j}.base.in" -r "${round}" -s ${summary} -t ${tag} -q ${query_type} -b "${batch_ratio}" -T "${tree}" -D "${dis}" 2>&1 | tee -a "${dest}"
					done
				done
			done
		done
	done
}

function run_cmake() {
	/usr/bin/drop_caches
	local leaf_idx="$1"
	local solver="$2"

	local cur_path
	cur_path=$(pwd)
	cd ../build || exit
	# cmake -DDEBUG=OFF -DCMAKE_CXX_COMPILER=clang++ -DLEAF="${leaf_wrap_seq[${leaf_idx}]}" "${HOME}/ParallelMultiwayOrderedSet"
	cmake -DDEBUG=OFF -DCMAKE_CXX_COMPILER=g++ -DJEMA=ON -DLEAF="${leaf_wrap_seq[${leaf_idx}]}" "${HOME}/ParallelMultiwayOrderedSet"
	make "${solver}"
	cd "${cur_path}" || exit
}

# Masstree
export TCMALLOC_LARGE_ALLOC_REPORT_THRESHOLD=1099511627776

tree=5
solver="index_microbench_workload"
run_cmake 0 "${solver}"
run_test 0 "${solver}" "${tree}"

# BwTree
tree=6
solver="index_microbench_workload"
run_cmake 0 "${solver}"
run_test 0 "${solver}" "${tree}"

# # PMA
# tree=3
# solver="pma"
# run_cmake 0 "${solver}"
# run_test 0 "${solver}" "${tree}"

# PAM
# tree=2
# solver="cpam"
# run_cmake 0 "${solver}"
# run_test 0 "${solver}" "${tree}"

# CPAM
# tree=1
# solver="cpam"
# run_cmake 4 "${solver}"
# run_test 4 "${solver}" "${tree}"

# ours
# solver="b_test"
# tree=0
# for idx in "${!leaf_wrap_seq[@]}"; do
# 	run_cmake "${idx}" "${solver}"
# 	run_test "${idx}" "${solver}" "${tree}"
# done

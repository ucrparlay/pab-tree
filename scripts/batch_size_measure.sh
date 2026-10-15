#!/bin/bash
#
# Batch vs serial update by batch size (spec: ../batch_size_measure.md).
# b_test tag 1<<8 (batch_vs_serial_by_size in tests/test_framework.h): for k in
# 1, 10, ..., 1e8, the first k entries of ins/<x>/1.batch.bin into a tree built
# from ins/<x>/1.base.bin by batch_insert and by serial_insert, then the first
# k keys of del/<x>/1.batch.bin out of a tree built from del/<x>/1.base.bin by
# batch_delete and by serial_delete. The tree is rebuilt before every timed
# call. Writes <leaf>_<j>.csv next to the log: batch_size, insert_time_batch,
# insert_time_serial, delete_time_batch, delete_time_serial (seconds, mean of
# `round` runs). Run from scripts/.
set -o xtrace

/usr/bin/drop_caches

declare -a leaf_wrap_seq=(64)

Nodes=(1000000000)
Dim=(1)
tag=$((2#100000000))
summary=0
query_type=$((2#0))
instance_n=1
# NOTE: round > 1 adds one untimed warm-up run per measurement. Rough wall
# time at 1e9: ~40 min with 3, ~16 min with 1, mostly the k = 1e8 serial runs,
# the 1e9 rebuilds and five 8 GB reads.
round=3

log_path="logs/batch_size_measure"
mkdir -p "${log_path}"

# NOTE: the globally shuffled datasets. -b 1 picks ins/1 and del/1 (batch ==
# base, 1e9 entries each), enough for every batch size up to 1e8; b_test reads
# del/<x>/ next to the -p file's ins/<x>/.
data_prefix="/data/zmen002/kv_pair"
dis_seq=("uni")
batch_ratio="1"

function run_test() {
	local leaf_idx="$1"
	local solver="$2"
	local tree="$3"

	local exe="../build/${solver}"

	for dim in "${Dim[@]}"; do
		for dis in "${dis_seq[@]}"; do
			for node in "${Nodes[@]}"; do

				local x=$(bc <<<"scale=0; 1/$batch_ratio") || return 1
				local file_prefix="${data_prefix}/${dis}/${node}_${dim}/ins/${x}"

				local dest="${log_path}/${tree}/${dis}/${leaf_wrap_seq[${leaf_idx}]}.log"
				mkdir -p "${dest%/*}"
				: >"${dest}"
				echo ">>>${dest}"

				for ((j = 1; j <= instance_n; j++)); do
					local csv="${dest%.log}_${j}.csv"
					numactl -i all "${exe}" -p "${file_prefix}/${j}.base.bin" -r "${round}" -s ${summary} -t ${tag} -q ${query_type} -b "${batch_ratio}" -T "${tree}" -D "${dis}" -o "${csv}" 2>&1 | tee -a "${dest}"
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
	cmake -DDEBUG=OFF -DCMAKE_CXX_COMPILER=g++ -DOSM=OFF -DDYNAMIC_GRAPH=OFF -DJEMA=ON -DLEAF="${leaf_wrap_seq[${leaf_idx}]}" "${HOME}/ParallelMultiwayOrderedSet"
	make "${solver}" -j
	cd "${cur_path}" || exit
}

# ours: the serial calls exist only in pab_tree
solver="b_test"
tree=0
for idx in "${!leaf_wrap_seq[@]}"; do
	run_cmake "${idx}" "${solver}"
	run_test "${idx}" "${solver}" "${tree}"
done

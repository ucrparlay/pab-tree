#!/bin/bash
#
set -o xtrace

/usr/bin/drop_caches

declare -a leaf_wrap_seq=(2 4 8 16 32 64 128 512 1024)

Nodes=(1000000000)
Dim=(1)
tag=$((2#0001))
summary=1
query_type=$((2#001))
instance_n=1
round=3

log_path="logs/sieve"
mkdir -p "${log_path}"

data_prefix="/data/zmen002/kv_pair"
batch_op_seq=("ins")
dis_seq=("uni" "exp" "norm" "dual_norm")
insert_opt=("sort" "sieve")
# insert_opt=("sieve")
batch_ratio=0.01

function run_test() {
	local leaf_idx="$1"
	local solver="$2"
	local tree="$3"
	local opt="$4"

	local exe="../build/${solver}"

	for dim in "${Dim[@]}"; do
		for dis in "${dis_seq[@]}"; do
			for batch_op in "${batch_op_seq[@]}"; do
				for node in "${Nodes[@]}"; do

					local x=$(bc <<<"scale=0; 1/$batch_ratio") || return 1
					local file_prefix="${data_prefix}/${dis}/${node}_${dim}/${batch_op}/${x}"

					local dest="${log_path}/${tree}/${dis}/${opt}/${leaf_wrap_seq[${leaf_idx}]}.log"
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
	local leaf_idx="$1"
	local solver="$2"
	local opt="$3"

	local cur_path
	cur_path=$(pwd)

	local opt_flag
	if [ "${opt}" = "sort" ]; then
		opt_flag="OFF"
	else
		opt_flag="ON"
	fi

	cd ../build || exit
	cmake -DDEBUG=OFF -DCMAKE_CXX_COMPILER=g++ -DSIEVE="${opt_flag}" -DLEAF="${leaf_wrap_seq[${leaf_idx}]}" "${HOME}/ParallelMultiwayOrderedSet"
	make "${solver}"
	cd "${cur_path}" || exit

}

solver="b_test"
tree=0
for opt in "${insert_opt[@]}"; do
	for idx in "${!leaf_wrap_seq[@]}"; do
		run_cmake "${idx}" "${solver}" "${opt}"
		run_test "${idx}" "${solver}" "${tree}" "${opt}"
	done
done

#!/bin/bash
#
set -o xtrace

/usr/bin/drop_caches

declare -a leaf_wrap=(2 4 8 16 32 64 128 512 1024)

Nodes=(1000000000)
Dim=(2)
declare -A datas
# datas["/data/legacy/data3/zmen002/kdtree/ss_varden/"]="../benchmark/ss_varden/"
datas["/data/zmen002/kv_pair/uniform_bigint/"]="../benchmark/uniform/"

tag=$((2#0001))
summary=1
read_file=1
query_type=$((2#001))
instance_n=1
round=3

log_path="logs/linear_wrap"
mkdir -p "${log_path}"

# make -C ../build/ testers

tree=0
solver="b_test"

for i in "${!leaf_wrap[@]}"; do
	cur_path=$(pwd)
	cd ../build || exit
	cmake -DDEBUG=OFF -DCMAKE_CXX_COMPILER=g++ -DLEAF="${leaf_wrap[${i}]}" "${HOME}/ParallelMultiwayOrderedSet"
	make ${solver}
	cd "${cur_path}" || exit

	for dim in "${Dim[@]}"; do
		for dataPath in "${!datas[@]}"; do
			for node in "${Nodes[@]}"; do

				# Hilbert curve is not supported for 3D with p_test
				# if [ ${dim} -eq 3 ] && [ ${tree} -eq 2 ] && [ ${split} -eq 1 ]; then
				#     continue
				# fi

				file_prefix="${dataPath}${node}_${dim}"
				dest="${log_path}/${leaf_wrap[${i}]}.log"
				: >"${dest}"
				echo ">>>${dest}"
				exe="../build/${solver}"

				for ((j = 1; j <= instance_n; j++)); do
					numactl -i all ${exe} -p "${file_prefix}/${j}.in" -r "${round}" -i ${read_file} -s ${summary} -t ${tag} -q ${query_type} -T ${tree} 2>&1 | tee -a "${dest}"
				done
			done
		done
	done
done

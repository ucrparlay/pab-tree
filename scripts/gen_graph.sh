#!/bin/bash
set -o xtrace

# path_prefix=/ssd1/zmen002/kv_pair
# base_n_seq=(1000000 5000000 10000000 50000000)
# file_num=10
# batch_ratio_seq=(100)

path_prefix=/data/zmen002/kv_pair
base_n_seq=(1000000000)
file_num=1
batch_ratio_seq=(10000 1000 100 10)

dis_seq=(0 1 2)
batch_op=(0 1)
val_n=1

logger_path="log.in"
: >"${logger_path}"

make -C ../build/ data_generator || exit

for base_n in "${base_n_seq[@]}"; do
	for batch_ratio in "${batch_ratio_seq[@]}"; do
		batch_n=$((base_n / batch_ratio))
		for dis in "${dis_seq[@]}"; do
			if [ "${dis}" -eq 0 ]; then
				param="1"
			elif [ "${dis}" -eq 1 ]; then
				if [ "${base_n}" -lt 100000000 ]; then
					param="10.0"
				elif [ "${base_n}" -lt 1000000000 ]; then
					param="15"
				else
					param="20"
				fi
			else
				if [ "${base_n}" -lt 100000000 ]; then
					param="0.1"
				elif [ "${base_n}" -lt 1000000000 ]; then
					param="0.2"
				else
					param="0.2"
				fi
			fi
			for b_op in "${batch_op[@]}"; do
				./../build/data_generator -path "${path_prefix}" -base_n "${base_n}" -batch_n "${batch_n}" -val_n "${val_n}" -file_num "${file_num}" -dis "${dis}" -batch_op "${b_op}" -param "${param}" 2>&1 | tee -a "${logger_path}"
			done
		done
	done
done

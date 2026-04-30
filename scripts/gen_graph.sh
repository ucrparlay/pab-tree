#!/bin/bash
set -o xtrace

# path_prefix=/ssd1/zmen002/kv_pair
# path_prefix=$1
# base_n_seq=(1000000 5000000 10000000 50000000)
# base_n_seq=(5000000)
# file_num=10
# file_num=3
# batch_ratio_seq=(100)

# path_prefix=/data/zmen002/kv_pair
path_prefix=/data/zmen002/kv_pair/batch_updates/
base_n_seq=(1000000000)
file_num=1
# batch_ratio_seq=(10000 1000 100 10)
# batch_ratio_seq=(10000 2000 5000 1000 200 500 100 20 50 10 2 5 1)
batch_ratio_seq=(1)

# dis_seq=(0 1 2 3)
dis_seq=(1)
# batch_op=(0 1)
batch_op=(0)
val_n=1

logger_path="log.in"
: >"${logger_path}"

make -C ../build/ data_generator || exit

for base_n in "${base_n_seq[@]}"; do
	for batch_ratio in "${batch_ratio_seq[@]}"; do
		batch_n=$((base_n / batch_ratio))
		for dis in "${dis_seq[@]}"; do
			if [ "${dis}" -eq 0 ]; then #uni+uni
				batch_param="1"
			elif [ "${dis}" -eq 1 ]; then # uni+exp
				if [ "${base_n}" -lt 100000000 ]; then
					batch_param="10.0"
				elif [ "${base_n}" -lt 1000000000 ]; then
					batch_param="15"
				else
					batch_param="20"
				fi
			elif [ "${dis}" -eq 2 ]; then #uni+normal
				if [ "${base_n}" -lt 100000000 ]; then
					batch_param="0.5,0.1"
				else
					batch_param="0.5,0.2"
				fi
			else #nmoral+normal
				if [ "${base_n}" -lt 100000000 ]; then
					base_param="0.25,0.15"
					batch_param="0.75,0.15"
				else
					# base_param="0.25,0.25"
					# batch_param="0.75,0.25"
					base_param="0.25,0.4"
					batch_param="0.75,0.4"
				fi
			fi
			for b_op in "${batch_op[@]}"; do
				./../build/data_generator -path "${path_prefix}" -base_n "${base_n}" -batch_n "${batch_n}" -val_n "${val_n}" -file_num "${file_num}" -dis "${dis}" -batch_op "${b_op}" -base_param "${base_param}" -batch_param "${batch_param}" 2>&1 | tee -a "${logger_path}"
			done
		done
	done
done

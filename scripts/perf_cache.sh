#!/bin/bash
#
set -o xtrace

/usr/bin/drop_caches

Nodes=(100000000)
Dim=(1)
tag=$((2#0000))
func_name="Membership"
summary=0
query_type=$((2#001))
instance_n=1
round=1

log_path="logs/query_cache"
mkdir -p "${log_path}"

data_prefix="/ssd1/zmen002/kv_pair"
batch_op_seq=("ins")
dis_seq=("uni" "exp" "norm" "dual_norm")
batch_ratio=0.01

function run_test() {
	local solver="$1"
	local tree="$2"

	local exe="../build/${solver}"

	for dim in "${Dim[@]}"; do
		for dis in "${dis_seq[@]}"; do
			for batch_op in "${batch_op_seq[@]}"; do
				for node in "${Nodes[@]}"; do

					local x=$(bc <<<"scale=0; 1/$batch_ratio") || return 1
					local file_prefix="${data_prefix}/${dis}/${node}_${dim}/${batch_op}/${x}"

					local dest="${log_path}/${tree}/${dis}/res.log"
					mkdir -p "${dest%/*}"
					: >"${dest}"
					echo ">>>${dest}"
					export PARLAY_NUM_THREADS=1

					perf_data_name="${log_path}/perf/${tree}_${dis}_${func_name}_perf.data"
					perf_report_name="${log_path}/perf/${tree}_${dis}_${func_name}_perf.report"
					mkdir -p "${perf_data_name%/*}"
					mkdir -p "${perf_report_name%/*}"
					: >"${perf_data_name}"
					: >"${perf_report_name}"
					echo ">>>${perf_data_name}"
					echo ">>>${perf_report_name}"

					ctl_dir=/tmp/

					ctl_fifo=${ctl_dir}perf_ctl.fifo
					test -p ${ctl_fifo} && unlink ${ctl_fifo}
					mkfifo ${ctl_fifo}
					exec {ctl_fd}<>${ctl_fifo}

					ctl_ack_fifo=${ctl_dir}perf_ctl_ack.fifo
					test -p ${ctl_ack_fifo} && unlink ${ctl_ack_fifo}
					mkfifo ${ctl_ack_fifo}
					exec {ctl_fd_ack}<>${ctl_ack_fifo}

					perf record -s -D -1 --control fd:${ctl_fd},${ctl_fd_ack} -o "${perf_data_name}" -e cycles,instructions,cache-references,cache-misses,branch-instructions,branch-misses "${exe}" -p "${file_prefix}/1.base.in" -r "${round}" -s ${summary} -t ${tag} -q ${query_type} -b "${batch_ratio}" -T "${tree}" -D "${dis}" -pcf ${ctl_fd} -pcaf ${ctl_fd_ack}
					# perf record -s -D -1 --control fd:${ctl_fd},${ctl_fd_ack} -o "${perf_data_name}" -e cache-misses "${exe}" -p "${file_prefix}/1.base.in" -r "${round}" -s ${summary} -t ${tag} -q ${query_type} -b "${batch_ratio}" -T "${tree}" -D "${dis}" -pcf ${ctl_fd} -pcaf ${ctl_fd_ack}

					exec {ctl_fd_ack}>&-
					unlink ${ctl_ack_fifo}

					exec {ctl_fd}>&-
					unlink ${ctl_fifo}

					perf report --stdio --input="${perf_data_name}" >"${perf_report_name}"

					# echo -n "1.base.in " >>"${dest}"
					if [ "${tree}" = "0" ]; then
						tree_name="BTree"
					elif [ "${tree}" = "1" ]; then
						tree_name="CPAM"
					elif [ "${tree}" = "2" ]; then
						tree_name="PAM"
					elif [ "${tree}" = "3" ]; then
						tree_name="PMA"
					fi
					echo "Tree: ${tree_name}; Buf: 32; InputFile: 1.base.in; Dis: ${dis};" >"${dest}"
					res=$(grep -E "Samples|Event count|\\b"${func_name}\\b "${perf_report_name}" | awk '/Event count/ {print $NF} /'"${func_name}"'/ {gsub("%", "", $1); }' | awk '{if($1>100){print; kp=1}else if(kp){print; kp=0}}')
					echo -n "$res" | paste -sd ' ' >>"${dest}"
				done
			done
		done
	done
}

function run_cmake() {
	local cur_path
	cur_path=$(pwd)
	cd ../build || exit
	cmake -DDEBUG=OFF -DCMAKE_CXX_COMPILER=g++ "${HOME}/ParallelMultiwayOrderedSet"
	make -j
	cd "${cur_path}" || exit
}

run_cmake

solver="baselines"

# PMA
tree=3
run_test "${solver}" "${tree}"

# PAM
tree=2
run_test "${solver}" "${tree}"

# CPAM
tree=1
run_test "${solver}" "${tree}"

solver="b_test"
tree=0
run_test "${solver}" "${tree}"

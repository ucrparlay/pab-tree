#!/bin/bash
set -o xtrace

Nodes=(1000000 5000000 10000000 50000000)
# Nodes=(50000000)

# NOTE: parameters
tester="b_ccp"

cur_path=$(pwd)
cd ../build || exit
cmake -DDEBUG=ON -DCCP=ON ..
make clean
make ${tester}
cd "${cur_path}" || exit

tag=$((2#0001))
batch_ratio=1
query_t_seq=$((2#001))
trees=(0)
dims=(1)
count=1 # count the number of ok in the output

# NOTE: log path
dest="ccp_logger.in"
: >${dest}

data_prefix="/ssd1/zmen002/kv_pair"
batch_op_seq=("ins")
dis_seq=("uni" "exp" "norm")

#* check node
for query_type in "${query_t_seq[@]}"; do
	for dis in "${dis_seq[@]}"; do
		for node in "${Nodes[@]}"; do
			for dim in "${dims[@]}"; do
				for tree in "${trees[@]}"; do
					for batch_op in "${batch_op_seq[@]}"; do

						files_path="${data_prefix}/${dis}/${node}_${dim}/${batch_op}/$((batch_ratio * 100))"
						echo "${files_path}"

						for file in "${files_path}"/*.base.in; do
							[[ -e "$file" ]] || continue
							echo "------->${file}"
							../build/${tester} -p "${file}" -s 0 -d "${dim}" -t "${tag}" -r 2 -T "${tree}" -q "${query_type}" -b "${batch_ratio}" 2>&1 | tee -a "${dest}"

							nc=$(grep -i -o "ok" ${dest} | wc -l)
							if [[ ${nc} -ne ${count} ]]; then
								echo 'wrong'
								exit
							fi
							count=$((count + 1))
						done
					done
				done
			done
		done
	done
done

echo "OK, Well done :)"
echo "OK, Well done :)" >>"log.in"

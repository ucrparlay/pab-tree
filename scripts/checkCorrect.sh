#!/bin/bash
set -o xtrace

Nodes=(1000000 5000000 8000000 10000000 50000000)
# Nodes=(50000000)

# NOTE: parameters
tester="b_ccp"
# tester="kd_ccp"

make -C ../build ${tester}

tag=$((2#0000))
read_file=0
queryTypes=$((2#000))
trees=(0)
dims=(2)
K=100
count=1 # count the number of ok in the output

# NOTE: log path
resFile="Correct.out"
dest="logger.in"
out="log.in"
: >${dest}

# Paths=("/ssd1/zmen002/kv_pair/ss_varden_bigint/" "/ssd1/zmen002/kv_pair/uniform_bigint/")
Paths=("/ssd1/zmen002/kv_pair/uniform_bigint/")

#* check node
make -C ../build ${tester}
for queryType in "${queryTypes[@]}"; do
	for path in "${Paths[@]}"; do
		for node in "${Nodes[@]}"; do
			for dim in "${dims[@]}"; do
				for tree in "${trees[@]}"; do

					files_path="${path}${node}_${dim}"
					echo "${files_path}"

					for file in "${files_path}/"*.in; do
						echo "------->${file}"
						../build/${tester} -p "${file}" -i "${read_file}" -s 0 -d "${dim}" -k ${K} -t ${tag} -r 2 -T "${tree}" -q ${queryType} 2>&1 | tee -a "${dest}"

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

echo "OK, Well done :)"
echo "OK, Well done :)" >>"log.in"

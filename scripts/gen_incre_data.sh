#!/bin/bash
#
# HOW TO RUN
#   cd scripts && ./gen_incre_data.sh            # generate everything missing
#   cd scripts && ./gen_incre_data.sh -f         # regenerate, overwriting
#
# Generates the datasets for the incremental-update tags (1<<6 insert, 1<<7
# delete) using incre_data_generator, which -- unlike data_generator -- keeps
# the requested distribution inside every batch instead of only globally.
#
# ONE DATASET PER RATIO.  test_framework.h's incre_ratio_seq slices at
# {1e-4, 1e-3, 1e-2, 1e-1} and reloads a file per ratio, so each ratio needs its
# own dataset generated at that batch size.  The ratio directory is base_n /
# batch_n, i.e. 1/ratio, which is what ParseInsertBatchPath(ratio) asks for.
#
# COST.  32 files x 8GB = ~256GB, and several hours: pool compaction is
# O(base_n * batch_num / 2), so ratio 1e-4 (10000 batches) is by far the most
# expensive and 1e-1 (10 batches) is nearly free.  Ratios are generated cheapest
# first so a partial run still leaves usable data.  Existing outputs are skipped
# unless -f is passed, so the script is resumable.
#
# NOTE ON PATH.  path_prefix is a FRESH root on purpose.  The directory layout
# is identical to the standard datasets, so pointing this at
# /data/zmen002/kv_pair would overwrite them in place.
set -o xtrace

force=0
[[ "$1" == "-f" ]] && force=1

path_prefix=/data/zmen002/kv_pair_incre
base_n=1000000000
file_num=1
val_n=1
binary=1

# NOTE: cheapest first -- ratio dir is 1/ratio, so 10 == ratio 0.1.
batch_ratio_seq=(10 100 1000 10000)

# PARA: 0 uni, 1 exp, 2 norm, 3 dual_norm, 4 seg.  Here -dis shapes each BATCH;
# the base is always the dense uniform key space.
# dis_seq=(0 1 2 3 4)
dis_seq=(4)
batch_op_seq=(0 1)

logger_path="log_incre_data.in"
: >"${logger_path}"

make -C ../build/ incre_data_generator || exit

# NOTE: lambda has to be scaled per ratio -- a single fixed value cannot work.
# The exponential asks for `batch_n * lambda / pool` of the keys at the very
# front, and nothing can supply more than 100% of them, so lambda is capped by
# pool/batch, which is just the number of batches still to come.
#
# Measured (see CLAUDE.md): the sampler reproduces the target shape EXACTLY
# while that demand stays at or below 1, and the pool halves over the first half
# of the run, so demand doubles.  Setting demand to 0.5 at the start keeps every
# batch in the first half exactly exponential -- and the first half is what
# builds the tree state the queries run against.
#
# lambda = batch_ratio / 2 -> {5, 50, 500, 5000}.  Front-vs-back odds inside a
# batch are e^lambda, so even the widest batches (10% of the pool, lambda 5) now
# lean ~148x instead of the 2.7x a smaller lambda would give.
#
# norm / dual_norm need no scaling: their mass sits in a band ~2*dev wide, so
# even the widest batch here stays under the limit.
function exp_lambda() {
	# lambda = batch_ratio / 2
	echo $(($1 / 2))
}

for batch_ratio in "${batch_ratio_seq[@]}"; do
	batch_n=$((base_n / batch_ratio))
	for dis in "${dis_seq[@]}"; do
		case "${dis}" in
		0) # uni -- parameter ignored
			batch_param="1"
			dis_name="uni"
			;;
		1) # exp: lambda, scaled per ratio (see exp_lambda above)
			batch_param="$(exp_lambda "${batch_ratio}")"
			dis_name="exp"
			;;
		2) # norm: mean,dev as fractions of the pool
			batch_param="0.5,0.2"
			dis_name="norm"
			;;
		3) # dual_norm: two modes, mean1,dev1,mean2,dev2
			batch_param="0.25,0.1,0.75,0.1"
			dis_name="dual_norm"
			;;
		4) # seg: window multiplier. Each batch of m is drawn uniformly from the
			# leading 2m surviving keys. NOTE this needs NO per-ratio tuning --
			# the window is defined relative to batch_n, not to the pool, so the
			# same value is equally concentrated at every ratio. That is the one
			# thing exp cannot do (its lambda is capped by pool/batch).
			batch_param="2"
			dis_name="seg"
			;;
		esac

		for b_op in "${batch_op_seq[@]}"; do
			# NOTE: same stem for both ops -- the file IS the batch stream,
			# not one half of a base/batch pair. Only the directory differs.
			if [ "${b_op}" -eq 0 ]; then
				out_dir="ins"
			else
				out_dir="del"
			fi
			out_stem="batch"
			out_file="${path_prefix}/${dis_name}/${base_n}_${val_n}/${out_dir}/${batch_ratio}/1.${out_stem}.bin"

			if [[ -s "${out_file}" && "${force}" -eq 0 ]]; then
				echo "skip existing ${out_file}" | tee -a "${logger_path}"
				continue
			fi

			numactl -i all ./../build/incre_data_generator -path "${path_prefix}" \
				-base_n "${base_n}" -batch_n "${batch_n}" -val_n "${val_n}" \
				-file_num "${file_num}" -binary "${binary}" -dis "${dis}" \
				-batch_op "${b_op}" -batch_param "${batch_param}" 2>&1 |
				tee -a "${logger_path}"
		done
	done
done

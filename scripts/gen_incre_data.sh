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

# NOTE: the output tree is named after the distribution, so two exp variants of
# different steepness cannot share a root -- give the softer one its own.
path_prefix="/data/zmen002/kv_pair_incre"
# path_prefix=/data/zmen002/kv_pair_incre_soft

# exp steepness: lambda = batch_ratio / exp_div.  Judge it by how many of a
# batch's keys land in the same 64-key leaf, NOT by the p10-p90 width -- the
# exponential's tail keeps touching leaves long after the peak has flattened, so
# width overstates how much softening you actually get.  Measured at ratio 100:
#
#   exp_div      2      4      8     20     50   | uniform
#   keys/leaf  5.67   3.09   1.98   1.56   1.40  |  1.34
#   vs uniform  4.2x   2.3x   1.5x   1.2x   1.0x |  1.0x
#
# So the usable range is roughly exp_div 2..4; by 8 it is already close to the
# uniform control and past 20 it is indistinguishable from it.  exp_div=4 sits
# at the geometric midpoint between sharp and uniform and is the recommended
# soft variant.  For reference seg mult=2 is sharper than any of these (10.0
# keys/leaf) -- a hard-edged window concentrates better than an exponential can.
#
# Pair a change here with a matching path_prefix above.
# exp_div=2
exp_div=15

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
# lambda = batch_ratio / exp_div; at the default exp_div=2 that is
# {5, 50, 500, 5000}.  See the exp_div block at the top for what other values do.
#
# CAVEAT at ratio 10: only 10 batches exist there, so a hot region wider than
# ~10 batch-widths cannot fit.  exp_div=10 gives lambda=1, which spreads a batch
# over ~77% of the key space -- effectively the uniform control.  The soft
# variant is only meaningful at ratios 100/1000/10000.
#
# norm / dual_norm need no scaling: their mass sits in a band ~2*dev wide, so
# even the widest batch here stays under the limit.
function exp_lambda() {
	local l=$(($1 / exp_div))
	[ "${l}" -lt 1 ] && l=1 # lambda 0 would be a flat density, not an exponential
	echo "${l}"
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
			batch_param="0.5,0.05"
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

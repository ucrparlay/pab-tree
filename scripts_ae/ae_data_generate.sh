#!/bin/bash
#
# HOW TO RUN
#   ./ae_data_generate.sh incre                 # Fig. 4 (and 7)
#   ./ae_data_generate.sh incre 100             # Fig. 7: batches of n/100
#   ./ae_data_generate.sh single uni seg norm   # Fig. 9
#   ./ae_data_generate.sh clean incre           # delete them again
#   ./ae_data_generate.sh clean single uni
#   FORCE=1 ./ae_data_generate.sh incre         # regenerate existing files
#
# The synthetic datasets, INPUT_SIZE (n) entries each, under
# $DATA_PREFIX/synthetic:
#   incre/<dis>/<n>_1/{ins,del}/<x>/1.batch.bin          batches of n/x
#   single/<dis>/<n>_1/{ins,del}/<x>/1.{base,batch}.bin  a tree, one batch of n/x
# uni is uniform. norm (Skewed) draws each batch from normal(0.5, 0.1) over
# the keys left; seg (Adversarial) from the leading 2n/x keys left. single
# is generated at x = 100 only, the one size Fig. 9 uses.

set -euo pipefail
source "$(dirname "$0")/common.sh"

n=$INPUT_SIZE
incre_x=(10 100 1000 10000)
single_x=(100)
gen_log="$LOG_DIR/data_generate.log"

dis_id() {
	case $1 in
	uni) echo 0 ;;
	norm) echo 2 ;;
	seg) echo 4 ;;
	*) return 1 ;;
	esac
}

dis_param() {
	case $1 in
	uni) echo 1 ;;
	norm) echo 0.5,0.1 ;;
	seg) echo 2 ;;
	esac
}

op_dir() { if [ "$1" = 0 ]; then echo ins; else echo del; fi; }

# true when FILE has to be (re)generated
stale() { [ "${FORCE:-0}" = 1 ] || [ ! -s "$1" ]; }

incre_gen() {
	"${NUMA[@]}" "$BUILD_ROOT/leaf64/incre_data_generator" -val_n 1 \
		-file_num 1 -binary 1 "$@" >>"$gen_log" 2>&1
}

# gen_incre [X...]: batches of n/X, every size by default
gen_incre() {
	build leaf64 incre_data_generator
	local dis x op out xs=("$@")
	if [ ${#xs[@]} -eq 0 ]; then
		xs=("${incre_x[@]}")
	fi
	for dis in uni norm seg; do
		for x in "${xs[@]}"; do
			for op in 0 1; do
				out="$SYNTH_DIR/incre/$dis/${n}_1/$(op_dir $op)/$x/1.batch.bin"
				stale "$out" || continue
				log "generate ${out#"$DATA_PREFIX"/}"
				incre_gen -path "$SYNTH_DIR/incre" -base_n "$n" \
					-batch_n $((n / x)) -dis "$(dis_id $dis)" -batch_op $op \
					-batch_param "$(dis_param $dis)"
			done
		done
	done
}

# uni and norm: data_generator writes base/batch pairs directly
gen_pairs() {
	local dis=$1 x=$2 op out
	for op in 0 1; do
		out="$SYNTH_DIR/single/$dis/${n}_1/$(op_dir $op)/$x/1.base.bin"
		stale "$out" || continue
		log "generate ${out#"$DATA_PREFIX"/}"
		"${NUMA[@]}" "$BUILD_ROOT/leaf64/data_generator" -path "$SYNTH_DIR/single" \
			-base_n "$n" -batch_n $((n / x)) -val_n 1 -file_num 1 -binary 1 \
			-dis "$(dis_id "$dis")" -batch_op $op -base_param 1 \
			-batch_param "$(dis_param "$dis")" >>"$gen_log" 2>&1
	done
}

# seg: the first batch of a seg stream. The insert stream covers n + m keys,
# so the tree left after cutting the batch out holds n.
gen_seg_pair() {
	local x=$1 m=$((n / x)) tmp="$SYNTH_DIR/tmp" out
	out="$SYNTH_DIR/single/seg/${n}_1"
	if stale "$out/ins/$x/1.base.bin"; then
		log "generate ${out#"$DATA_PREFIX"/}/ins/$x"
		incre_gen -path "$tmp" -base_n $((n + m)) -batch_n $m -dis 4 \
			-batch_op 0 -batch_param 2
		python3 "$AE_DIR/make_single_batch.py" ins \
			"$tmp/seg/$((n + m))_1/ins/$(((n + m) / m))/1.batch.bin" $m "$out/ins/$x"
	fi
	if stale "$out/del/$x/1.base.bin"; then
		log "generate ${out#"$DATA_PREFIX"/}/del/$x"
		incre_gen -path "$tmp" -base_n "$n" -batch_n $m -dis 4 -batch_op 1 \
			-batch_param 2
		python3 "$AE_DIR/make_single_batch.py" del \
			"$tmp/seg/${n}_1/del/$x/1.batch.bin" $m "$out/del/$x"
	fi
	rm -rf "${tmp:?}"
}

gen_single() {
	build leaf64 data_generator
	build leaf64 incre_data_generator
	local dis x
	for dis; do
		dis_id "$dis" >/dev/null || {
			log "unknown distribution: $dis"
			return 1
		}
		for x in "${single_x[@]}"; do
			if [ "$dis" = seg ]; then
				gen_seg_pair "$x"
			else
				gen_pairs "$dis" "$x"
			fi
		done
	done
}

clean() {
	local kind=$1 dis
	shift
	case $kind in
	incre)
		log "delete ${SYNTH_DIR:?}/incre"
		rm -rf "${SYNTH_DIR:?}/incre"
		;;
	single)
		for dis; do
			log "delete ${SYNTH_DIR:?}/single/${dis:?}"
			rm -rf "${SYNTH_DIR:?}/single/${dis:?}"
		done
		;;
	*)
		log "clean what? incre or single DIS..."
		return 1
		;;
	esac
}

mkdir -p "$LOG_DIR"
case "${1:-}" in
incre)
	shift
	gen_incre "$@"
	;;
single)
	shift
	gen_single "$@"
	;;
clean)
	shift
	clean "$@"
	;;
*)
	sed -n '3,10p' "$0"
	exit 1
	;;
esac

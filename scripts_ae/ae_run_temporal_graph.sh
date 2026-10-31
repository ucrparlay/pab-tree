#!/bin/bash
#
# HOW TO RUN
#   ./ae_run_temporal_graph.sh    # Fig. 6
#
# The temporal-graph build (-DDYNAMIC_GRAPH=ON). RED, REF and SEM have no
# insert or delete tags: tag 64 slides a three-year window over their edges.
# PLANET has the tags: tag 16 applies its insertion and deletion batches.
# Each year ends with Neighbor and Flatten queries. Reads
# $REAL_DIR/temporal_graph (download_data.sh). Writes
# logs/temp_signal/<tree>/<graph>/64.log and logs/temp_osm/<tree>/temp_osm/64.log.

set -euo pipefail
source "$(dirname "$0")/common.sh"

graphs="$REAL_DIR/temporal_graph"

for g in redd ref sem; do
	input="$graphs/signal/$g/0.bin"
	if [ ! -s "$input" ]; then
		log "skip $g: no $input"
		continue
	fi
	for tree in 0 2 1 3; do
		log_file="$LOG_DIR/temp_signal/$tree/$g/64.log"
		mkdir -p "$(dirname "$log_file")"
		: >"$log_file"
		run_tree "$log_file" "$tree" graph \
			-p "$input" -r "$ROUNDS" -s 0 -t 64 -q 0 -b 0.01 -D "$g"
	done
done

input="$graphs/osm/planet_year/"
if [ ! -s "$input/0_insert.bin" ]; then
	log "skip PLANET: no $input"
	exit 0
fi
for tree in 0 2 1 3; do
	log_file="$LOG_DIR/temp_osm/$tree/temp_osm/64.log"
	mkdir -p "$(dirname "$log_file")"
	: >"$log_file"
	run_tree "$log_file" "$tree" graph \
		-p "$input" -r "$ROUNDS" -s 0 -t 16 -q 0 -b 0.01 -D temp_osm
done

#!/bin/bash
#
# HOW TO RUN
#   ./ae_run_randomness.sh    # Fig. 8
#
# Tag 32 of the temporal-graph build: PLANET's batches, then Neighbor
# queries, with the leaf randomness ratio at 0%, 10%, ..., 100%. PaB-tree
# only; Fig. 8's CPAM lines come from Fig. 6's CPAM run on PLANET.
# Reads $REAL_DIR/temporal_graph/osm/planet_year. Writes logs/randomness/dynamic.log.

set -euo pipefail
source "$(dirname "$0")/common.sh"

input="$REAL_DIR/temporal_graph/osm/planet_year/"
if [ ! -s "$input/0_insert.bin" ]; then
	log "skip Fig. 8: no $input"
	exit 0
fi

log_file="$LOG_DIR/randomness/dynamic.log"
mkdir -p "$(dirname "$log_file")"
: >"$log_file"
run_tree "$log_file" 0 graph -p "$input" -r "$ROUNDS" -s 0 -t 32 -q 0 -b 0.01 -D osm

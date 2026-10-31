#!/bin/bash
#
# HOW TO RUN
#   ./ae_run_osm.sh    # Fig. 5
#
# Tag 8 of the OSM build (-DOSM=ON): a sliding window over the OSM records
# batched by year, inserting each year and deleting the one three years
# before, with membership and range queries after each year.
# Reads $REAL_DIR/osm (download_data.sh). Writes logs/osm/<tree>/osm/64.log.

set -euo pipefail
source "$(dirname "$0")/common.sh"

input="${OSM_FILE:-$REAL_DIR/osm/asia_year_zcode.bin}"
if [ ! -s "$input" ]; then
	log "skip Fig. 5: no $input"
	exit 0
fi

for tree in $(trees 0 2 1 3 5 6); do
	log_file="$LOG_DIR/osm/$tree/osm/64.log"
	mkdir -p "$(dirname "$log_file")"
	: >"$log_file"
	run_tree "$log_file" "$tree" osm \
		-p "$input" -r "$ROUNDS" -s 0 -t 8 -q 0 -b 0.01 -D osm
done

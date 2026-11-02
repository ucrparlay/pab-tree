#!/bin/bash
#
# HOW TO RUN
#   ./download_data.sh [DATA_PREFIX]    # docker-run.sh runs it before run and full
#
# Fetches the real-world datasets of Fig. 5, 6 and 8, 88 GB, into
# DATA_PREFIX/real_world (default $DATA_PREFIX). What is already there is
# skipped; what fails is reported, and run.sh skips its figures. The synthetic
# datasets download when a step needs them (ae_data_generate.sh).

set -euo pipefail
source "$(dirname "$0")/common.sh"

DATA_PREFIX="${1:-$DATA_PREFIX}"

missing=0
while read -r _ path; do
	if [ ! -s "$DATA_PREFIX/$path" ] && ! fetch "$path"; then
		log "missing $path"
		missing=$((missing + 1))
	fi
done < <(grep ' real_world/' "$AE_DIR/datasets.sha256")

if [ "$missing" -gt 0 ]; then
	log "$missing real-world file(s) missing: Fig. 5, 6, 8 need them"
fi

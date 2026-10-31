#!/bin/bash
#
# HOW TO RUN
#   ./download_data.sh [DATA_PREFIX]    # docker-run.sh runs it before run and full
#
# Fetches the real-world datasets of Fig. 5, 6 and 8 into DATA_PREFIX/real_world
# (default $DATA_PREFIX). What is already there is skipped; what has no URL yet
# is reported, and its figures are skipped by run.sh.

set -euo pipefail
source "$(dirname "$0")/common.sh"

DATA_PREFIX="${1:-$DATA_PREFIX}"
REAL_DIR="$DATA_PREFIX/real_world"

# path under real_world | URL | sha256. A .tar.gz URL is unpacked into the
# path's directory. TODO: URLs and checksums once the datasets are hosted.
datasets=(
	"osm/asia_year_zcode.bin||"
	"temporal_graph/signal/redd/0.bin||"
	"temporal_graph/signal/ref/0.bin||"
	"temporal_graph/signal/sem/0.bin||"
	"temporal_graph/osm/planet_year/0_insert.bin||"
)

missing=0
for entry in "${datasets[@]}"; do
	IFS='|' read -r path url sum <<<"$entry"
	dest="$REAL_DIR/$path"
	if [ -s "$dest" ]; then
		continue
	fi
	if [ -z "$url" ]; then
		log "missing $dest (no download URL yet)"
		missing=$((missing + 1))
		continue
	fi
	mkdir -p "$(dirname "$dest")"
	tmp="$dest.part"
	log "download $url"
	curl -fL --retry 3 -o "$tmp" "$url"
	if [ -n "$sum" ]; then
		echo "$sum  $tmp" | sha256sum -c -
	fi
	if [[ "$url" == *.tar.gz ]]; then
		tar -xzf "$tmp" -C "$(dirname "$dest")"
		rm -f "$tmp"
	else
		mv "$tmp" "$dest"
	fi
done

if [ "$missing" -gt 0 ]; then
	log "$missing real-world dataset(s) missing: Fig. 5, 6, 8 need them"
fi

#!/bin/bash
#
# HOW TO RUN
#   ./download_data.sh [DATA_PREFIX]    # docker-run.sh runs it before run and full
#
# Fetches the real-world datasets of Fig. 5, 6 and 8, 88 GB, into
# DATA_PREFIX/real_world (default $DATA_PREFIX): the files datasets.sha256 names,
# relative to DATA_PREFIX. What is already there is skipped; what fails is
# reported, and run.sh skips its figures.

set -euo pipefail
source "$(dirname "$0")/common.sh"

DATA_PREFIX="${1:-$DATA_PREFIX}"
HF_URL="${HF_URL:-https://huggingface.co/datasets/romaLzhih/pab-tree-data/resolve/main}"

# fetch SHA256 PATH: download PATH into $DATA_PREFIX and check it. A broken
# transfer resumes; five tries. HTTP 416: .part already reaches the end of the
# file, left by a run killed during the checksum, so the checksum decides.
fetch() {
	local sum=$1 path=$2 tmp="$DATA_PREFIX/$2.part" attempt code
	mkdir -p "$(dirname "$tmp")"
	log "download $path"
	for ((attempt = 1; attempt <= 5; attempt++)); do
		if code=$(curl -fL -C - -o "$tmp" -w '%{http_code}' "$HF_URL/$path") ||
			[ "$code" = 416 ]; then
			if echo "$sum  $tmp" | sha256sum -c --quiet -; then
				mv "$tmp" "$DATA_PREFIX/$path"
				return 0
			fi
			rm -f "$tmp"
		fi
		log "failed: attempt $attempt"
	done
	rm -f "$tmp"
	return 1
}

missing=0
while read -r sum path; do
	if [ ! -s "$DATA_PREFIX/$path" ] && ! fetch "$sum" "$path"; then
		log "missing $path"
		missing=$((missing + 1))
	fi
done <"$AE_DIR/datasets.sha256"

if [ "$missing" -gt 0 ]; then
	log "$missing real-world file(s) missing: Fig. 5, 6, 8 need them"
fi

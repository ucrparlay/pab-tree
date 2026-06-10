#!/bin/bash
#
# Convert all text .in dataset files under a directory (recursively) to the
# binary .bin format using data_to_binary (see tests/binary_io.h for the
# format). Each <file>.in produces <file>.bin next to it.
#
# Usage:
#   ./scripts/convert_to_binary.sh <data_dir> [-f] [-d]
#     -f  re-convert even if the .bin already exists and is newer than the .in
#     -d  delete the original .in after each successful conversion
#         (skipped up-to-date files are NOT deleted; combine with -f for that)
#
# Example:
#   ./scripts/convert_to_binary.sh /data/zmen002/kv_pair/uni/100000000_1

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <data_dir> [-f] [-d]" >&2
    exit 1
fi

data_dir="$1"
shift
force=0
delete_orig=0
for arg in "$@"; do
    case "${arg}" in
    -f) force=1 ;;
    -d) delete_orig=1 ;;
    *)
        echo "Unknown option: ${arg}" >&2
        echo "Usage: $0 <data_dir> [-f] [-d]" >&2
        exit 1
        ;;
    esac
done

if [[ ! -d "${data_dir}" ]]; then
    echo "Not a directory: ${data_dir}" >&2
    exit 1
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
converter="${script_dir}/../build/data_to_binary"
if [[ ! -x "${converter}" ]]; then
    echo "data_to_binary not found at ${converter}; build it first:" >&2
    echo "  cd build && make data_to_binary -j\$(nproc)" >&2
    exit 1
fi

total=0
converted=0
skipped=0
failed=0
deleted=0

while IFS= read -r -d '' in_file; do
    total=$((total + 1))
    bin_file="${in_file%.in}.bin"
    # skip if up-to-date (unless -f)
    if [[ ${force} -eq 0 && -f "${bin_file}" && "${bin_file}" -nt "${in_file}" ]]; then
        skipped=$((skipped + 1))
        continue
    fi
    if "${converter}" -p "${in_file}" -o "${bin_file}"; then
        converted=$((converted + 1))
        if [[ ${delete_orig} -eq 1 && -s "${bin_file}" ]]; then
            rm "${in_file}"
            deleted=$((deleted + 1))
        fi
    else
        echo "FAILED: ${in_file}" >&2
        failed=$((failed + 1))
    fi
done < <(find "${data_dir}" -type f -name '*.in' -print0 | sort -z)

echo "Found ${total} .in files: converted ${converted}, skipped ${skipped} (up-to-date), failed ${failed}, deleted ${deleted} originals."
if [[ ${failed} -gt 0 ]]; then
    exit 1
fi

# Sample usage:
#   ./scripts/convert_to_binary.sh /data/zmen002/kv_pair/uni/100000000_1
#       convert every .in under that tree, keeping originals
#   ./scripts/convert_to_binary.sh /data/zmen002/kv_pair -f
#       force re-convert all (e.g. after a .bin format change)
#   ./scripts/convert_to_binary.sh /data/zmen002/kv_pair -f -d
#       force re-convert all and delete each .in after success

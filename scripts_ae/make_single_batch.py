#!/usr/bin/env python3
"""Cut a one-batch dataset out of an incre_data_generator stream.

HOW TO RUN
    python3 make_single_batch.py {ins|del} STREAM M OUT_DIR

The first M entries of STREAM are its first batch. Writes
    ins: OUT_DIR/1.batch.bin = the first M entries, 1.base.bin = the rest
    del: OUT_DIR/1.batch.bin = the first M entries, 1.base.bin = STREAM itself,
         moved there
Files are in tests/binary_io.h's format: a uint64 header {N, dim, width},
then N rows of dim values of width bytes each.
"""
import os
import struct
import sys

kHeader = struct.Struct("<3Q")
kChunk = 1 << 26


def copy_rows(src, path, first, count, row_bytes, dim, width):
    with open(path, "wb") as dst:
        dst.write(kHeader.pack(count, dim, width))
        src.seek(kHeader.size + first * row_bytes)
        left = count * row_bytes
        while left:
            buf = src.read(min(kChunk, left))
            if not buf:
                sys.exit(f"{src.name}: shorter than its header says")
            dst.write(buf)
            left -= len(buf)


def main():
    if len(sys.argv) != 5 or sys.argv[1] not in ("ins", "del"):
        sys.exit(__doc__)
    op, stream, m, out_dir = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
    os.makedirs(out_dir, exist_ok=True)
    with open(stream, "rb") as src:
        n, dim, width = kHeader.unpack(src.read(kHeader.size))
        if not 0 < m <= n:
            sys.exit(f"{stream}: batch of {m} from {n} entries")
        row_bytes = dim * width
        copy_rows(src, os.path.join(out_dir, "1.batch.bin"), 0, m, row_bytes, dim, width)
        if op == "ins":
            copy_rows(src, os.path.join(out_dir, "1.base.bin"), m, n - m, row_bytes,
                      dim, width)
    if op == "del":
        os.replace(stream, os.path.join(out_dir, "1.base.bin"))


if __name__ == "__main__":
    main()

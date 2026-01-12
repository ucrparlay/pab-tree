#pragma once

#include <cstdint>

namespace gbbs {

typedef uint32_t flags;
flags const no_output = 1;
flags const pack_edges = 2;
flags const sparse_blocked = 4;
flags const dense_forward = 8;
flags const dense_parallel = 16;
flags const remove_duplicates = 32;
flags const no_dense = 64;
flags const in_edges = 128;        // map over in edges instead of out edges
flags const fine_parallel = 256;   // split to a node-size of 1
flags const compact_blocks = 512;  // used in SAGE
flags const dense_only = 1024;
inline bool should_output(flags const& fl) { return !(fl & no_output); }

}  // namespace gbbs

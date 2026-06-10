// Convert a text ".in" dataset (produced by data_generate.cpp) into a raw
// binary ".bin" file for fast loading.
//
// Text format:   "<N> <dim>\n" header, then N lines of dim decimal integers
//                (key, then dim-1 value columns).
// Binary format: little-endian uint64_t stream:
//                [0]            N
//                [1]            dim
//                [2 .. 2+N*dim) entries, row-major (key, val...)
//
// Usage: data_to_binary -p <input.in> [-o <output.bin>]
//        -o defaults to the input path with ".in" replaced by ".bin".

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "common/IO.h"
#include "common/parse_command_line.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/sequence.h"

// Sample usage:
//   ./data_to_binary -p /data/zmen002/kv_pair/uni/100000000_1/del/10/1.base.in
//     -> writes /data/zmen002/kv_pair/uni/100000000_1/del/10/1.base.bin
//   ./data_to_binary -p /data/zmen002/kv_pair/uni/100000000_1/del/10/1.batch.in \
//                    -o /tmp/1.batch.bin
int
main(int argc, char* argv[])
{
  commandLine P(argc, argv, "[-p <inFile>] [-o <outFile>]");
  std::string in_path = P.getOptionValue("-p", "");
  std::string out_path = P.getOptionValue("-o", "");

  if (in_path.empty()) {
    std::cerr << "Missing input file (-p)." << std::endl;
    return 1;
  }
  if (out_path.empty()) {
    out_path = in_path;
    size_t pos = out_path.rfind(".in");
    if (pos == out_path.size() - 3) {
      out_path.replace(pos, 3, ".bin");
    } else {
      out_path += ".bin";
    }
  }

  std::cout << "Reading " << in_path << std::endl;
  parlay::sequence<char> S = benchIO::readStringFromFile(in_path.c_str());
  parlay::sequence<char*> W = benchIO::stringToWords(S);

  uint64_t const n = std::stoull(W[0]);
  uint64_t const dim = std::stoull(W[1]);
  if (W.size() - 2 != n * dim) {
    std::cerr << "Token count mismatch: header says " << n << " x " << dim
              << " = " << n * dim << ", file has " << W.size() - 2
              << " tokens." << std::endl;
    return 1;
  }

  std::cout << "Parsing " << n << " entries with dim " << dim << std::endl;
  parlay::sequence<uint64_t> data(2 + n * dim);
  data[0] = n;
  data[1] = dim;
  parlay::parallel_for(0, n * dim, [&](size_t i) {
    data[2 + i] = std::strtoull(W[2 + i], nullptr, 10);
  });

  std::cout << "Writing " << out_path << std::endl;
  std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    std::cerr << "Unable to open output file: " << out_path << std::endl;
    return 1;
  }
  out.write(reinterpret_cast<char const*>(data.data()),
            static_cast<std::streamsize>(data.size() * sizeof(uint64_t)));
  out.close();

  std::cout << "Done: " << data.size() * sizeof(uint64_t) << " bytes."
            << std::endl;
  return 0;
}

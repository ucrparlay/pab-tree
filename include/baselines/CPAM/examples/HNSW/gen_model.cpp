#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
// #include <memory>
#include <tuple>

#include "HNSW.hpp"
// #include "benchUtils.h" // fix multiple inclusion
// #include "dist_l2.hpp"
#include "dist_ang.hpp"
using ANN::HNSW;

int main(int argc, char** argv) {
  for (int i = 0; i < argc; ++i) printf("%s ", argv[i]);
  putchar('\n');

  commandLine parameter(
      argc, argv,
      "-n <numInput> -ml <m_l> -m <m> "
      "-efc <ef_construction> -alpha <alpha> -r <recall@R> [-b <batchBase>]"
      "-in <inFile> -out <modelFile>");
  char* file_in = parameter.getOptionValue("-in");
  char const* file_out = parameter.getOptionValue("-out");
  uint32_t const cnt_points = parameter.getOptionLongValue("-n", 0);
  float const m_l = parameter.getOptionDoubleValue("-ml", 0.36);
  uint32_t const m = parameter.getOptionIntValue("-m", 40);
  uint32_t const efc = parameter.getOptionIntValue("-efc", 60);
  float const alpha = parameter.getOptionDoubleValue("-alpha", 1);
  float const batch_base = parameter.getOptionDoubleValue("-b", 2);
  bool const do_fixing = !!parameter.getOptionIntValue("-f", 0);

  if (file_in == nullptr || file_out == nullptr)
    return fputs("in/out files are not indicated\n", stderr), 1;

  char* spec_input = std::strchr(file_in, ':');
  if (spec_input == nullptr) return fputs("Unrecognized file spec", stderr), 2;

  parlay::internal::timer t("HNSW", true);

  *(spec_input++) = '\0';
  parlay::sequence<fvec> ps;
  uint32_t dim;
  if (spec_input[0] == '/')
    std::tie(ps, dim) = ps_from_HDF5(file_in, spec_input);
  else if (std::strcmp(spec_input, "fvec"))
    std::tie(ps, dim) = ps_from_SIFT(file_in);
  else
    return fputs("Unsupported file spec", stderr), 2;

  t.next("Read inFile");

  fputs("Start building HNSW\n", stderr);
  HNSW<descr_fvec> g(ps.begin(), ps.begin() + cnt_points, dim, m_l, m, efc,
                     alpha, batch_base, do_fixing);
  t.next("Build index");

  g.save(file_out);

  t.next("Write to the file");
  return 0;
}

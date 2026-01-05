#include <filesystem>
#include <iostream>
#include <numeric>
#include <string>

#include "common/IO.h"
#include "common/geometryIO.h"
#include "common/parse_command_line.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/sequence.h"
#include "test_framework.h"
///**********************************START*********************************///

using Axis = uint64_t;
Axis const kValueUB = 1'000'000'000;
// Axis const kValueUB = 1'000'000;

inline std::string toString(auto const& a) { return std::to_string(a); }

void PrintPoints(auto const& wp) {
  for (auto const& p : wp) {
    std::cout << p << std::endl;
  }
  std::cout << std::endl;
  return;
}

void PrintToFile(std::string const& path, auto const& wp) {
  using E = typename std::remove_reference_t<decltype(wp)>::value_type;

  size_t const gen_num = wp.size();
  auto constexpr gen_dim = 1 + std::tuple_size_v<typename E::V>;

  auto header = toString(gen_num) + " " + toString(gen_dim);
  // NOTE: create a parlay::sequence<char> in order to call the writeSeqToFile
  auto wp_str = parlay::tabulate(wp.size(), [&](size_t i) {
    std::string str;
    str += std::to_string(wp[i].key) + " ";
    if constexpr (std::tuple_size<typename std::remove_reference_t<
                      decltype(wp)>::value_type::V>::value == 1) {
      str += std::to_string(std::get<0>(wp[i].val)) + " ";
    }
    // for (int j = 0; std::cmp_less(j, gen_dim); j++) {
    //   str += std::to_string(wp[i].pnt[j]) + " ";
    // }
    parlay::sequence<char> cstr(str.begin(), str.end());
    return cstr;
  });

  // NOTE: write to file in parallel
  writeSeqToFile(header, wp_str, path.c_str());

  // NOTE: sometimes parallel written cannot be finished correcly
  // naive check for it
  std::ifstream file(path);
  int lineCount = 0;
  std::string line;
  while (std::getline(file, line)) {
    ++lineCount;
    if (lineCount > 3) break;
  }
  if (lineCount <= 3) {
    throw std::runtime_error("parallel written error, please retry." + path);
  }
  file.close();
  return;
}

template <typename E, typename BatchDisFunc>
class Generator {
 public:
  using Points = parlay::sequence<E>;

  static Points Gen(size_t const base_n, size_t batch_n) {
    Points wp(base_n);
    auto ground_seq = GenUniformSeq<typename E::K>(base_n + batch_n);
    BatchDisFunc::template GenSeq<size_t>(batch_n, ground_seq.size());
  }
};

template <class T>
static parlay::sequence<T> GenUniformSeq(size_t n) {
  printf("uniform distribution with num_keys: %zu\n", n);
  parlay::sequence<T> seq(n);
  parlay::parallel_for(0, n, [&](size_t i) {
    size_t v = i;
    seq[i] = v;
  });
  return parlay::random_shuffle(seq);
}

template <class T>
parlay::sequence<T> exponential_generator(size_t n, double lambda) {
  printf("exponential distribution with lambda: %.10f\n", lambda);
  size_t cutoff = n;
  parlay::sequence<size_t> nums(cutoff + 1, 0);

  parlay::parallel_for(0, cutoff, [&](size_t i) {
    nums[i] = std::max(1.0, n * (lambda * exp(-lambda * (i + 0.5))));
  });
  size_t tot = scan_inplace(make_slice(nums));
  assert(tot >= n);

  parlay::sequence<T> seq(n);
  parlay::parallel_for(0, cutoff, [&](size_t i) {
    parlay::parallel_for(nums[i], min(n, nums[i + 1]), [&](size_t j) {
      // seq[j] = _hash(static_cast<T>(i));
      seq[j] = i;
    });
  });
  return random_shuffle(seq);
}

template <class T>
parlay::sequence<T> zipfian_generator(size_t n, double s) {
  printf("zipfian distribution with s: %f\n", s);
  size_t cutoff = n;
  auto harmonic = parlay::delayed_seq<double>(
      cutoff, [&](size_t i) { return 1.0 / pow(i + 1, s); });
  double sum = parlay::reduce(make_slice(harmonic));
  double v = n / sum;
  parlay::sequence<size_t> nums(cutoff + 1, 0);
  parlay::parallel_for(
      0, cutoff, [&](size_t i) { nums[i] = max(1.0, v / pow(i + 1, s)); });
  size_t tot = scan_inplace(make_slice(nums));
  assert(tot >= n);
  parlay::sequence<T> seq(n);
  parlay::parallel_for(0, cutoff, [&](size_t i) {
    parlay::parallel_for(nums[i], min(n, nums[i + 1]), [&](size_t j) {
      // seq[j] = _hash(static_cast<T>(i));
      seq[j] = i;
    });
  });
  return random_shuffle(seq);
}

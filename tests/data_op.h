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

constexpr size_t duplicate_ratio = 2;

template <class T = size_t>
class UniformGenerator {
 public:
  static parlay::sequence<T> Generate(double, size_t n, size_t max_k) {
    printf("uniform distribution with num_keys: %zu\n", n);
    parlay::sequence<T> seq(n);
    parlay::parallel_for(0, n, [&](size_t i) { seq[i] = i % max_k; });
    return parlay::random_shuffle(seq);
  }
};

template <class T = size_t>
class ExponentialGenerator {
 public:
  static parlay::sequence<T> Generate(double lambda, size_t n, size_t max_k) {
    printf("exponential distribution with lambda: %.10f\n", lambda);
    size_t cutoff = duplicate_ratio * n;
    parlay::sequence<size_t> nums(cutoff + 1, 0);

    parlay::parallel_for(0, cutoff, [&](size_t i) {
      nums[i] = std::max(1.0, n * (lambda * exp(-lambda * (i + 0.5))));
      nums[i] %= max_k;
    });

    nums = parlay::remove_duplicates(nums);
    if (nums.size() < n) {
      throw std::runtime_error(
          "ExponentialGenerator: not enough unique keys generated.");
    }
    return parlay::random_shuffle(nums.cut(0, n));
  }
};

template <class T = size_t>
class ZipfianGenerator {
 public:
  static parlay::sequence<T> Generate(double s, size_t n, size_t max_k) {
    printf("zipfian distribution with s: %f\n", s);
    size_t cutoff = duplicate_ratio * n;
    auto harmonic = parlay::delayed_seq<double>(
        cutoff, [&](size_t i) { return 1.0 / pow(i + 1, s); });
    double sum = parlay::reduce(make_slice(harmonic));
    double v = n / sum;
    parlay::sequence<size_t> nums(cutoff + 1, 0);
    parlay::parallel_for(0, cutoff, [&](size_t i) {
      nums[i] = max(1.0, v / pow(i + 1, s));
      nums[i] %= max_k;
    });

    nums = parlay::remove_duplicates(nums);
    if (nums.size() < n) {
      throw std::runtime_error(
          "ExponentialGenerator: not enough unique keys generated.");
    }
    return parlay::random_shuffle(nums.cut(0, n));
  }
};

template <typename E, typename BatchDisFunc>
class Generator {
 public:
  using EntrySeq = parlay::sequence<E>;

  static EntrySeq ToEntry(parlay::sequence<size_t> const& key_seq) {
    return parlay::tabulate(key_seq.size(), [&](size_t i) {
      if constexpr (std::is_same_v<typename E::V, std::tuple<>>) {
        return E{.key = key_seq[i]};
      } else if constexpr (std::tuple_size_v<typename E::V> == 1) {
        return E{.key = key_seq[i], .val = {i}};
      } else {
        static_assert(false,
                      "Generator::ToEntry: Unsupported Entry value type.");
      }
    });
  }

  static std::pair<EntrySeq, EntrySeq> GenBatch(size_t const base_n,
                                                size_t const batch_n,
                                                size_t const tot_size,
                                                double const param) {
    auto ground_k_seq = UniformGenerator<size_t>::Generate(
        param, tot_size, std::numeric_limits<size_t>::max());
    auto batch_idx_seq =
        BatchDisFunc::Generate(param, batch_n, ground_k_seq.size());

    // gen base keys
    parlay::sequence<bool> flag(tot_size, true);
    parlay::parallel_for(0, batch_n,
                         [&](size_t i) { flag[batch_idx_seq[i]] = false; });
    auto base_k_seq = parlay::pack(ground_k_seq, flag);

    // gen batch indexes
    parlay::parallel_for(0, flag.size(), [&](size_t i) { flag[i] = !flag[i]; });
    auto batch_k_seq = parlay::pack(ground_k_seq, flag);

    // assert(base_k_seq.size() == base_n);
    // assert(batch_k_seq.size() == batch_n);
    if (base_k_seq.size() != base_n || batch_k_seq.size() != batch_n) {
      throw std::runtime_error(
          "Generator::GenBatch: Generated batch size "
          "mismatch. Please adjust the parameters.");
    }
    return {ToEntry(base_k_seq), ToEntry(batch_k_seq)};
  }

  static auto GenInsertBatch(size_t const base_n, size_t const batch_n,
                             double const param) {
    return GenBatch(base_n, batch_n, base_n + batch_n, param);
  }

  static auto GenDeleteBatch(size_t const base_n, size_t const batch_n,
                             double const param) {
    return GenBatch(base_n, batch_n, base_n, param);
  }
};

// template <class T>
// parlay::sequence<T> exponential_generator(size_t n, double lambda) {
//   printf("exponential distribution with lambda: %.10f\n", lambda);
//   size_t cutoff = n;
//   parlay::sequence<size_t> nums(cutoff + 1, 0);
//
//   parlay::parallel_for(0, cutoff, [&](size_t i) {
//     nums[i] = std::max(1.0, n * (lambda * exp(-lambda * (i + 0.5))));
//   });
//   size_t tot = scan_inplace(make_slice(nums));
//   assert(tot >= n);
//
//   parlay::sequence<T> seq(n);
//   parlay::parallel_for(0, cutoff, [&](size_t i) {
//     parlay::parallel_for(nums[i], min(n, nums[i + 1]), [&](size_t j) {
//       // seq[j] = _hash(static_cast<T>(i));
//       seq[j] = i;
//     });
//   });
//   return random_shuffle(seq);
// }
//
// template <class T>
// parlay::sequence<T> zipfian_generator(size_t n, double s) {
//   printf("zipfian distribution with s: %f\n", s);
//   size_t cutoff = n;
//   auto harmonic = parlay::delayed_seq<double>(
//       cutoff, [&](size_t i) { return 1.0 / pow(i + 1, s); });
//   double sum = parlay::reduce(make_slice(harmonic));
//   double v = n / sum;
//   parlay::sequence<size_t> nums(cutoff + 1, 0);
//   parlay::parallel_for(
//       0, cutoff, [&](size_t i) { nums[i] = max(1.0, v / pow(i + 1, s)); });
//   size_t tot = scan_inplace(make_slice(nums));
//   assert(tot >= n);
//   parlay::sequence<T> seq(n);
//   parlay::parallel_for(0, cutoff, [&](size_t i) {
//     parlay::parallel_for(nums[i], min(n, nums[i + 1]), [&](size_t j) {
//       // seq[j] = _hash(static_cast<T>(i));
//       seq[j] = i;
//     });
//   });
//   return random_shuffle(seq);
// }

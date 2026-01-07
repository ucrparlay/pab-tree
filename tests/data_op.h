#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>

#include "common/IO.h"
#include "common/geometryIO.h"
#include "common/parse_command_line.h"
#include "parlay/internal/group_by.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/sequence.h"
#include "test_framework.h"
///**********************************START*********************************///

using Axis = uint64_t;
Axis const kValueUB = 1'000'000'000;
int rand_seed = 1;
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

template <typename Seq>
auto bucketize(Seq const& data, size_t const numBuckets = 10) {
  parlay::sequence<size_t> buckets(numBuckets, 0);
  // Find min and max values
  auto minVal = *std::min_element(data.begin(), data.end());
  auto maxVal = *std::max_element(data.begin(), data.end());

  // int const numBuckets = 10;
  auto range = maxVal - minVal;
  auto bucketWidth = range / numBuckets;

  // Count elements in each bucket
  for (auto val : data) {
    int bucketIdx = static_cast<int>((val - minVal) / bucketWidth);
    // if (bucketIdx == 0) {
    //   std::cout << val << " ";
    // }
    // Handle edge case for maximum value
    if (bucketIdx >= numBuckets) bucketIdx = numBuckets - 1;
    buckets[bucketIdx]++;
  }
  // std::flush(std::cout);

  // Display results
  std::cout << std::scientific << std::setprecision(4);
  std::cout << "Range: [" << minVal << ", " << maxVal << "]\n";
  std::cout << "Bucket width: " << bucketWidth << "\n\n";

  // for (int i = 0; i < numBuckets; i++) {
  for (int i = 0; i < 10; i++) {
    double lower = minVal + i * bucketWidth;
    double upper = minVal + (i + 1) * bucketWidth;
    std::cout << "Bucket " << std::setw(2) << i << " [" << std::setw(10)
              << lower << ", " << std::setw(10) << upper << "): " << buckets[i]
              << " elements\n";
  }

  std::cout << data.size() << " elements total.\n\n";
  std::flush(std::cout);
  return std::make_tuple(buckets[0], minVal, maxVal);
}

template <class Seq>
auto SampleWeightedSeq(Seq&& weight_seq, size_t n) {
  // std::random_device rd;      // a seed source for the random number engine
  // std::mt19937 gen_mt(rd());  // mersenne_twister_engine seeded with rd()
  // std::uniform_int_distribution<int> distrib(1, kValueUB);

  auto [cum_seq, total] = parlay::scan(weight_seq);
  parlay::random_generator gen(rand_seed);
  std::uniform_real_distribution<double> dis(0, total);

  auto random_dis = parlay::tabulate(n, [&](size_t i) {
    auto r = gen[i];
    return dis(r);
  });

  auto sample_idx = parlay::tabulate(n, [&](size_t i) -> size_t {
    return std::lower_bound(cum_seq.begin(), cum_seq.end(), random_dis[i]) -
           cum_seq.begin();
  });
  return sample_idx;
}

template <class Seq>
auto SupplySample(Seq const& current_seq, size_t extra_n, size_t l, size_t r) {
  std::set<size_t> selected_set;
  auto sorted_seq = parlay::sort(current_seq);
  while (selected_set.size() < extra_n) {
    // std::cout << selected_set.size() << "/" << extra_n << "\n" << std::flush;
    size_t idx = get_random_index(l, r, 0);
    if (*std::lower_bound(sorted_seq.begin(), sorted_seq.end(), idx) != idx &&
        selected_set.find(idx) == selected_set.end()) {
      selected_set.insert(idx);
    }
  }
  return Seq(selected_set.begin(), selected_set.end());
}

constexpr size_t duplicate_ratio = 2;

template <class T = size_t>
class UniformGenerator {
 public:
  static parlay::sequence<T> GenerateGroundSeq(double, size_t n, size_t max_k) {
    printf("uniform distribution ground with num_keys: %zu\n", n);
    return parlay::tabulate(n, [&](size_t i) { return i; });
  }

  static parlay::sequence<T> Generate(double, size_t batch_n, size_t ground_n) {
    printf("uniform distribution with num_keys: %zu\n", batch_n);
    auto sample = parlay::remove_duplicates(SampleWeightedSeq(
        parlay::tabulate(ground_n, [&](size_t i) -> size_t { return 1; }),
        batch_n));
    sample.append(SupplySample(sample, batch_n - sample.size(), 0, ground_n));
    bucketize(sample);
    // std::cout << sample.size() << std::endl;

    if (parlay::remove_duplicates(sample).size() < batch_n) {
      std::cout << "Generated unique keys: " << sample.size()
                << ", required: " << batch_n << std::endl;
      throw std::runtime_error(
          "UniformGenerator: not enough unique keys generated.");
    } else {
      std::cout << "Generated unique keys: " << sample.size()
                << ", required: " << batch_n << std::endl;
    }
    return sample;
  }
};

template <class T = size_t>
class ExponentialGenerator {
 public:
  static double constexpr val_scale_fac = 1e10;
  static double constexpr x_scale_fac = 0.0005;

  static parlay::sequence<T> Generate(double lambda, size_t batch_n,
                                      size_t ground_n) {
    printf("exponential distribution with lambda: %.10f\n", lambda);
    size_t cutoff = ground_n;
    parlay::sequence<double> nums(cutoff, 0);

    parlay::parallel_for(0, cutoff, [&](size_t i) {
      // nums[i] = std::max(1.0, n * (lambda * exp(-lambda * (i + 0.5))));
      nums[i] = lambda * exp(-lambda * (x_scale_fac * i));
      // nums[i] %= max_k;
    });
    std::cout << std::endl;
    auto sample = parlay::remove_duplicates(SampleWeightedSeq(nums, batch_n));
    // sample = parlay::remove_duplicates(sample);
    size_t bucket_n = 10;
    auto [first_bkt_n, min_v, max_v] = bucketize(sample, bucket_n);
    size_t remain_n = batch_n - sample.size();
    size_t supply_factor =
        ((max_v - min_v) / bucket_n - first_bkt_n) > 2 * remain_n ? 10 : 1;
    std::cout << "supply factor: " << supply_factor << std::endl;

    sample.append(SupplySample(sample, batch_n - sample.size(), 0,
                               cutoff / supply_factor));
    bucketize(sample);
    // std::cout << sample.size() << std::endl;

    if (parlay::remove_duplicates(sample).size() < batch_n) {
      std::cout << "Generated unique keys: " << sample.size()
                << ", required: " << batch_n << std::endl;
      throw std::runtime_error(
          "ExponentialGenerator: not enough unique keys generated.");
    } else {
      std::cout << "Generated unique keys: " << sample.size()
                << ", required: " << batch_n << std::endl;
    }
    return sample;
  }
};

template <class T = size_t>
class ZipfianGenerator {
  static double constexpr x_scale_fac = 0.0005;
  static size_t constexpr supply_factor = 10;

 public:
  static parlay::sequence<T> Generate(double s, size_t batch_n,
                                      size_t ground_n) {
    printf("zipfian distribution with s: %f\n", s);
    size_t cutoff = ground_n;
    auto harmonic = parlay::delayed_seq<double>(
        cutoff, [&](size_t i) { return 1.0 / pow(i + 1, s); });
    double sum = parlay::reduce(make_slice(harmonic));
    double v = sum;
    parlay::sequence<double> nums(cutoff, 0);
    parlay::parallel_for(0, cutoff,
                         [&](size_t i) { nums[i] = v / pow(i + 1, s); });

    auto sample = SampleWeightedSeq(nums, batch_n);
    // nums = parlay::remove_duplicates(nums);
    // bucketize(sample, 10);
    sample = parlay::remove_duplicates(sample);
    // bucketize(sample, 10);

    sample.append(SupplySample(sample, batch_n - sample.size(), 0,
                               cutoff / supply_factor));

    bucketize(sample, 10);
    if (sample.size() < batch_n) {
      std::cout << "Generated unique keys: " << nums.size()
                << ", required: " << batch_n << std::endl;
      throw std::runtime_error("zipfian: not enough unique keys generated.");
    }
    // return parlay::random_shuffle(nums.cut(0, n));
    return sample;
  }
};

template <typename E, typename BatchDisFunc>
class Generator {
 public:
  using EntrySeq = parlay::sequence<E>;

  static void Validate(int batch_op, size_t base_n, size_t batch_n,
                       auto base_k_seq, auto batch_k_seq, auto ground_k_seq) {
    if (parlay::remove_duplicates(base_k_seq).size() !=
        (batch_op == 0 ? base_n : base_n - batch_n)) {
      std::cout << "base_k_seq size: " << base_k_seq.size()
                << " after remove duplicates: "
                << parlay::remove_duplicates(base_k_seq).size()
                << ", expected: " << (batch_op == 0 ? base_n : base_n - batch_n)
                << std::endl;
      throw std::runtime_error(
          "Validate: Base sequence size mismatch after remove duplicates.");
    }

    if (parlay::remove_duplicates(batch_k_seq).size() != batch_n) {
      throw std::runtime_error(
          "Validate: Batch sequence size mismatch after remove duplicates.");
    }

    if (parlay::remove_duplicates(ground_k_seq).size() !=
        (batch_op == 0 ? base_n + batch_n : base_n)) {
      throw std::runtime_error(
          "Validate: Ground sequence size mismatch after remove duplicates.");
    }

    if (ground_k_seq.size() != (batch_op == 0 ? base_n + batch_n : base_n)) {
      throw std::runtime_error("Validate: Ground sequence size mismatch.");
    }

    base_k_seq = parlay::sort(base_k_seq);
    batch_k_seq = parlay::sort(batch_k_seq);
    ground_k_seq = parlay::sort(ground_k_seq);
    if (batch_op == 0 && parlay::any_of(batch_k_seq, [&](size_t idx) {
          auto it = std::lower_bound(base_k_seq.begin(), base_k_seq.end(), idx);
          return it != base_k_seq.end() && *it == idx;
        })) {
      throw std::runtime_error(
          "Validate: Insert batch has keys already in base sequence.");
    } else if (batch_op == 1 && !parlay::all_of(batch_k_seq, [&](size_t idx) {
                 auto it = std::lower_bound(ground_k_seq.begin(),
                                            ground_k_seq.end(), idx);
                 return it != ground_k_seq.end() && *it == idx;
               })) {
      throw std::runtime_error(
          "Validate: Delete batch has keys not in base sequence.");
    }

    std::cout << "Validation passed." << std::endl;
  }

  static EntrySeq ToEntry(parlay::sequence<size_t> const& key_seq) {
    // shuffle keys
    auto rand_seq = parlay::random_shuffle(parlay::make_slice(key_seq));
    return parlay::tabulate(key_seq.size(), [&](size_t i) {
      if constexpr (std::is_same_v<typename E::V, std::tuple<>>) {
        return E{.key = rand_seq[i]};
      } else if constexpr (std::tuple_size_v<typename E::V> == 1) {
        return E{.key = rand_seq[i], .val = {i}};
      } else {
        static_assert(false,
                      "Generator::ToEntry: Unsupported Entry value type.");
      }
    });
  }

  static std::pair<EntrySeq, EntrySeq> GenBatch(int const batch_op,
                                                size_t const base_n,
                                                size_t const batch_n,
                                                double const param) {
    // PARA: initiaze size before take batch
    size_t tot_size =
        batch_op == 0 ? base_n + batch_n : base_n;  // insert or delete
    auto ground_k_seq = UniformGenerator<size_t>::GenerateGroundSeq(
        param, tot_size, std::numeric_limits<size_t>::max());
    assert(parlay::remove_duplicates(ground_k_seq).size() == tot_size);
    auto batch_idx_seq =
        BatchDisFunc::Generate(param, batch_n, ground_k_seq.size());

    // gen base keys
    parlay::sequence<bool> flag(tot_size, true);
    parlay::parallel_for(0, batch_n,
                         [&](size_t i) { flag[batch_idx_seq[i]] = false; });
    auto base_k_seq = parlay::pack(ground_k_seq, flag);

    // gen batch indexes
    parlay::parallel_for(0, flag.size(), [&](size_t i) {
      flag[i] = flag[i] == false ? true : false;
    });
    auto batch_k_seq = parlay::pack(ground_k_seq, flag);

    Validate(batch_op, base_n, batch_n, base_k_seq, batch_k_seq, ground_k_seq);

    return batch_op == 0
               ? std::make_pair(ToEntry(base_k_seq), ToEntry(batch_k_seq))
               : std::make_pair(ToEntry(ground_k_seq), ToEntry(batch_k_seq));
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

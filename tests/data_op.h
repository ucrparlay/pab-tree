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
/************************************START***********************************/

using axis = uint64_t;
axis const k_value_ub = 1'000'000'000;
int rand_seed = 1;
/* Axis const k_value_ub = 1'000'000; */

inline std::string toString(auto const &a)
{
	return std::to_string(a);
}

void PrintPoints(auto const &wp)
{
	for (auto const &p : wp) {
		std::cout << p << std::endl;
	}
	std::cout << std::endl;
	return;
}

void print_to_file(std::string const &path, auto const &wp)
{
	using entry_type =
		typename std::remove_reference_t<decltype(wp)>::value_type;

	size_t const gen_num = wp.size();
	auto constexpr gen_dim =
		1 + std::tuple_size_v<typename entry_type::mapped_type>;

	auto header = toString(gen_num) + " " + toString(gen_dim);

	/*
	 * NOTE: create a parlay::sequence<char> in order to call the writeSeqToFile
	 */
	auto wp_str = parlay::tabulate(wp.size(), [&](size_t i) {
		std::string str;
		str += std::to_string(wp[i].key) + " ";
		if constexpr (std::tuple_size<typename std::remove_reference_t<
						  decltype(wp)>::value_type::mapped_type>::value == 1) {
			str += std::to_string(std::get<0>(wp[i].val)) + " ";
		}
		/*
		 * for (int j = 0; std::cmp_less(j, gen_dim); j++) {
		 *   str += std::to_string(wp[i].pnt[j]) + " ";
		 * }
		 */
		parlay::sequence<char> cstr(str.begin(), str.end());
		return cstr;
	});

	/* NOTE: write to file in parallel */
	writeSeqToFile(header, wp_str, path.c_str());

	/*
	 * NOTE: sometimes parallel written cannot be finished correcly
	 * naive check for it
	 */
	std::ifstream file(path);
	int lineCount = 0;
	std::string line;
	while (std::getline(file, line)) {
		++lineCount;
		if (lineCount > 3)
			break;
	}
	if (lineCount <= 3) {
		throw std::runtime_error("parallel written error, please retry." +
								 path);
	}
	file.close();
	return;
}

/*
 * NOTE: binary counterpart of print_to_file. Writes the ".bin" format described
 * in binary_io.h (header [N, dim, width], values narrowed to 32-bit when the
 * data range fits).
 */
void print_to_binary_file(std::string const &path, auto const &wp)
{
	using entry_type =
		typename std::remove_reference_t<decltype(wp)>::value_type;

	uint64_t const gen_num = wp.size();
	uint64_t constexpr gen_dim =
		1 + std::tuple_size_v<typename entry_type::mapped_type>;
	static_assert(
		std::tuple_size_v<typename entry_type::mapped_type> == 1,
		"print_to_binary_file: only key + 1 value column is supported.");

	auto payload = parlay::sequence<uint64_t>(gen_num * gen_dim);
	parlay::parallel_for(0, gen_num, [&](size_t i) {
		payload[i * gen_dim] = wp[i].key;
		payload[(i * gen_dim) + 1] = std::get<0>(wp[i].val);
	});
	write_binary_entries(path, gen_num, gen_dim, payload);
	return;
}

template <typename Seq>
auto bucketize(Seq const &data, size_t const numBuckets = 10)
{
	parlay::sequence<size_t> buckets(numBuckets, 0);
	/* Find min and max values */
	auto minVal = *std::min_element(data.begin(), data.end());
	auto maxVal = *std::max_element(data.begin(), data.end());

	/* int const numBuckets = 10; */
	auto range = maxVal - minVal;
	auto bucketWidth = range / numBuckets;

	/* Count elements in each bucket */
	for (auto val : data) {
		size_t bucketIdx = static_cast<size_t>((val - minVal) / bucketWidth);
		/*
		 * if (bucketIdx == 0) {
		 *   std::cout << val << " ";
		 * }
		 * Handle edge case for maximum value
		 */
		if (bucketIdx >= numBuckets)
			bucketIdx = numBuckets - 1;
		buckets[bucketIdx]++;
	}
	/* std::flush(std::cout); */

	/* Display results */
	std::cout << std::scientific << std::setprecision(4);
	std::cout << "Range: [" << minVal << ", " << maxVal << "]\n";
	std::cout << "Bucket width: " << bucketWidth << "\n\n";

	/* for (int i = 0; i < numBuckets; i++) { */
	for (int i = 0; i < 10; i++) {
		double lower = minVal + i * bucketWidth;
		double upper = minVal + (i + 1) * bucketWidth;
		std::cout << "Bucket " << std::setw(2) << i << " [" << std::setw(10)
				  << lower << ", " << std::setw(10) << upper
				  << "): " << buckets[i] << " elements\n";
	}

	std::cout << data.size() << " elements total.\n\n";
	std::flush(std::cout);
	return std::make_tuple(buckets[0], minVal, maxVal);
}

template <class Seq>
auto SampleWeightedSeq(Seq &&weight_seq, size_t n)
{
	/*
	 * std::random_device rd;      // a seed source for the random number engine
	 * std::mt19937 gen_mt(rd());  // mersenne_twister_engine seeded with rd()
	 * std::uniform_int_distribution<int> distrib(1, k_value_ub);
	 */

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
auto SupplySample(Seq const &current_seq, size_t extra_n, size_t l, size_t r)
{
	std::set<size_t> selected_set;
	auto sorted_seq = parlay::sort(current_seq);
	while (selected_set.size() < extra_n) {
		/*
		 * std::cout << selected_set.size() << "/" << extra_n << "\n" << std::flush;
		 */
		size_t idx = get_random_index(l, r);
		if (*std::lower_bound(sorted_seq.begin(), sorted_seq.end(), idx) !=
				idx &&
			selected_set.find(idx) == selected_set.end()) {
			selected_set.insert(idx);
		}
	}
	return Seq(selected_set.begin(), selected_set.end());
}

constexpr size_t duplicate_ratio = 2;

template <class T = double>
class UniformGenerator {
public:
	static parlay::sequence<T> GenerateGroundSeq(double, size_t n, size_t max_k)
	{
		printf("uniform distribution ground with num_keys: %zu\n", n);
		return parlay::tabulate(n, [&](size_t i) { return i; });
	}

	template <typename Param>
	static parlay::sequence<T> Generate(Param const &, size_t n)
	{
		parlay::random_generator gen(rand_seed);
		std::uniform_real_distribution<double> dis(0, 1);

		return parlay::tabulate(n, [&](size_t i) {
			auto r = gen[i];
			return dis(r);
		});
	}
};

template <class T = double>
class ExponentialGenerator {
public:
	template <typename Param>
	static parlay::sequence<T> Generate(Param const &param, size_t n)
	{
		if (param.size() != 1) {
			throw std::runtime_error(
				"ExponentialGenerator::Generate requires 1 parameter: lambda.");
		}
		double lambda = param[0];
		std::cout << "exponential distribution with lambda: " << lambda
				  << std::endl;
		parlay::random_generator gen(rand_seed);
		std::exponential_distribution<double> dis(lambda);
		return parlay::tabulate(n, [&](size_t i) -> double {
			auto r = gen[i];
			return dis(r);
		});
	}
};

template <class T = double>
class NormalGenerator {
public:
	template <typename Param>
	static parlay::sequence<T> Generate(Param const &param, size_t n)
	{
		if (param.size() != 2) {
			throw std::runtime_error(
				"NormalGenerator::Generate requires 2 parameters: mean and dev.");
		}
		double mean = param[0], dev = param[1];
		std::cout << "normal distribution with mean: " << mean
				  << " dev: " << dev << std::endl;
		parlay::random_generator gen(rand_seed);
		std::normal_distribution<double> dis(mean, dev);

		return parlay::tabulate(n, [&](size_t i) -> double {
			auto r = gen[i];
			return dis(r);
		});
	}
};

template <class T = double>
class ZipfianGenerator {
	static double constexpr x_scale_fac = 0.0005;
	static size_t constexpr supply_factor = 10;

public:
	static parlay::sequence<T> Generate(double s, size_t batch_n,
										size_t ground_n)
	{
		printf("zipfian distribution with s: %f\n", s);
		throw std::runtime_error("ZipfianGenerator::Generate not implemented.");
	}
};

template <typename Entry, typename BaseDisFunc, typename BatchDisFunc>
class Generator {
public:
	using entry_seq = parlay::sequence<Entry>;

	static void validate(int batch_op, size_t base_n, size_t batch_n,
						 auto const &base_k_seq, auto const &batch_k_seq)
	{
		if (parlay::remove_duplicates(base_k_seq).size() != base_n) {
			std::cout << "base_k_seq size: " << base_k_seq.size()
					  << " after remove duplicates: "
					  << parlay::remove_duplicates(base_k_seq).size()
					  << ", expected: "
					  << (batch_op == 0 ? base_n : base_n - batch_n)
					  << std::endl;
			throw std::runtime_error(
				"validate: Base sequence size mismatch after remove duplicates.");
		}

		if (parlay::remove_duplicates(batch_k_seq).size() != batch_n) {
			throw std::runtime_error(
				"validate: Batch sequence size mismatch after remove duplicates.");
		}

		auto base_sort_seq = parlay::sort(base_k_seq);
		auto batch_sort_seq = parlay::sort(batch_k_seq);
		if (batch_op == 0 && parlay::any_of(batch_sort_seq, [&](size_t key) {
				auto it = std::lower_bound(base_sort_seq.begin(),
										   base_sort_seq.end(), key);
				return it != base_sort_seq.end() && *it == key;
			})) {
			throw std::runtime_error(
				"validate: Insert batch has keys already in base sequence.");
		} else if (batch_op == 1) {
			if (!parlay::all_of(batch_sort_seq, [&](size_t idx) {
					auto it = std::lower_bound(base_sort_seq.begin(),
											   base_sort_seq.end(), idx);
					return it != base_sort_seq.end() && *it == idx;
				})) {
				throw std::runtime_error(
					"validate: Delete batch has keys not in base sequence.");
			}
		}

		std::cout << "Validation passed." << std::endl;
	}

	static entry_seq to_entry(parlay::sequence<size_t> const &key_seq)
	{
		/* shuffle keys */
		auto rand_seq = parlay::random_shuffle(parlay::make_slice(key_seq));
		return parlay::tabulate(key_seq.size(), [&](size_t i) {
			if constexpr (std::is_same_v<typename Entry::mapped_type,
										 std::tuple<>>) {
				return Entry{ .key = rand_seq[i] };
			} else if constexpr (std::tuple_size_v<
									 typename Entry::mapped_type> == 1) {
				return Entry{ .key = rand_seq[i], .val = { i } };
			} else {
				static_assert(
					false,
					"Generator::to_entry: Unsupported entry_traits value type.");
			}
		});
	}

	static std::pair<parlay::sequence<size_t>, parlay::sequence<size_t>>
	Discreatize(auto const &base_ran_seq, auto const &batch_ran_seq,
				int batch_op)
	{
		/*
		 * the first element is the random number
		 * the second element is the flag for base or batch
		 * the third is the numbered id
		 */
		auto base_flag_seq = parlay::map(base_ran_seq, [&](double x) {
			return std::make_tuple(x, bool(0), size_t(0));
		});
		auto batch_flag_seq = parlay::map(batch_ran_seq, [&](double x) {
			return std::make_tuple(x, bool(1), size_t(0));
		});

		/* append both seq and sort */
		auto ground_seq =
			parlay::sort(parlay::append(base_flag_seq, batch_flag_seq),
						 [&](auto const &a, auto const &b) {
							 return std::get<0>(a) < std::get<0>(b);
						 });

		/* assign id to the sorted sequence */
		parlay::parallel_for(0, ground_seq.size(),
							 [&](size_t i) { std::get<2>(ground_seq[i]) = i; });

		return std::make_pair(
			parlay::map(
				batch_op == 0 ? parlay::filter(ground_seq,
											   [&](auto const &t) {
												   return std::get<1>(t) == 0;
											   }) :
								ground_seq,
				[&](auto const &t) -> size_t { return std::get<2>(t); }),
			parlay::map(
				parlay::filter(
					ground_seq,
					[&](auto const &t) { return std::get<1>(t) == 1; }),
				[&](auto const &t) -> size_t { return std::get<2>(t); }));
	}

	template <typename Param>
	static std::pair<entry_seq, entry_seq>
	GenBatch(int const batch_op, size_t const base_n, size_t const batch_n,
			 Param const &base_param, Param const &batch_param)
	{
		auto base_ran_seq = BaseDisFunc::Generate(
			base_param, batch_op == 0 ? base_n : base_n - batch_n);
		auto batch_ran_seq = BatchDisFunc::Generate(batch_param, batch_n);
		bucketize(base_ran_seq, 10);
		bucketize(batch_ran_seq, 10);
		auto [dis_base_seq, dis_batch_seq] = Discreatize(
			std::move(base_ran_seq), std::move(batch_ran_seq), batch_op);
		std::cout << "final base size: " << dis_base_seq.size()
				  << ", batch size: " << dis_batch_seq.size() << std::endl;
		validate(batch_op, base_n, batch_n, dis_base_seq, dis_batch_seq);
		return { to_entry(dis_base_seq), to_entry(dis_batch_seq) };
	}
};

/*
 * template <class T>
 * parlay::sequence<T> exponential_generator(size_t n, double lambda) {
 *   printf("exponential distribution with lambda: %.10f\n", lambda);
 *   size_t cutoff = n;
 *   parlay::sequence<size_t> nums(cutoff + 1, 0);
 *
 *   parlay::parallel_for(0, cutoff, [&](size_t i) {
 *     nums[i] = std::max(1.0, n * (lambda * exp(-lambda * (i + 0.5))));
 *   });
 *   size_t tot = scan_inplace(make_slice(nums));
 *   assert(tot >= n);
 *
 *   parlay::sequence<T> seq(n);
 *   parlay::parallel_for(0, cutoff, [&](size_t i) {
 *     parlay::parallel_for(nums[i], min(n, nums[i + 1]), [&](size_t j) {
 *       // seq[j] = _hash(static_cast<T>(i));
 *       seq[j] = i;
 *     });
 *   });
 *   return random_shuffle(seq);
 * }
 *
 * template <class T>
 * parlay::sequence<T> zipfian_generator(size_t n, double s) {
 *   printf("zipfian distribution with s: %f\n", s);
 *   size_t cutoff = n;
 *   auto harmonic = parlay::delayed_seq<double>(
 *       cutoff, [&](size_t i) { return 1.0 / pow(i + 1, s); });
 *   double sum = parlay::reduce(make_slice(harmonic));
 *   double v = n / sum;
 *   parlay::sequence<size_t> nums(cutoff + 1, 0);
 *   parlay::parallel_for(
 *       0, cutoff, [&](size_t i) { nums[i] = max(1.0, v / pow(i + 1, s)); });
 *   size_t tot = scan_inplace(make_slice(nums));
 *   assert(tot >= n);
 *   parlay::sequence<T> seq(n);
 *   parlay::parallel_for(0, cutoff, [&](size_t i) {
 *     parlay::parallel_for(nums[i], min(n, nums[i + 1]), [&](size_t j) {
 *       // seq[j] = _hash(static_cast<T>(i));
 *       seq[j] = i;
 *     });
 *   });
 *   return random_shuffle(seq);
 * }
 */

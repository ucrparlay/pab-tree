#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <ios>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "binary_io.h"
#include "common/geometryIO.h"
#include "common/parse_command_line.h"
#include "common/time_loop.h"
#include "pab_tree/dependence/tree_node.h"
#include "pab_tree/dependence/type_trait.h"
#include "options.h"
#include "parlay/internal/group_by.h"
#include "parlay/monoid.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/random.h"
#include "parlay/slice.h"
#include "pab_tree/b_tree.h"
#include "wrapper.h"

#ifdef CCP
using Coord = long;
// using Coord = double;
#else
using Coord = long;
// using Coord = double;
#endif // CCP

using Typename = Coord;
using namespace pab_tree;

// NOTE: membership size
static constexpr double kMemberQueryRatio = 0.01;
// NOTE: OSM
static constexpr size_t kMemberQueryOsmSize = 100'000'000;
static constexpr int kOsmWindowLen = 3;
static constexpr int kOSMRecType = 1;
static constexpr int kOSMRangeQueryNum = 500'000;

// NOTE: Dynamic Graph
constexpr uint64_t kDynamicGraphRangeNum = 1e7;
constexpr uint64_t kDynamicGraphMaxSingleRange = 1506;
constexpr uint64_t kDynamicFlattenSize = 5e8;

// constexpr uint64_t kDynamicGraphSignalRangeNum = 1e6;

// NOTE: rectangle numbers
static constexpr int kRangeQueryNum = 50'000;
static constexpr int kRangeQueryType = 3;
static constexpr int kRangeQueryWithLogNum = 100;

// NOTE: rectangle numbers for inba ratio
static constexpr int rangeQueryNumInbaRatio = 50000;
// NOTE: insert batch ratio for inba ratio
static constexpr double insertBatchInbaRatio = 0.001;
// NOTE: knn batch ratio for inba ratio
static constexpr double knnBatchInbaRatio = 0.1;

// NOTE: Insert Ratio when summary
static constexpr double kBatchInsertRatio = 0.01;
// NOTE: DIff Ratio when summary
static constexpr double kBatchDiffTotalRatio = 0.01;
static constexpr double kBatchDiffOverlapRatio = 0.2;

// NOTE: rectange type used in summary
static constexpr int summaryRangeQueryType = 2;
// NOTE: range query num in summary
static constexpr int kSummaryRangeQueryNum = 50000;

// NOTE: helper for delete type
enum DeleteType { kBatchDelete, kBatchDiff };

// * [a,b)
inline size_t GetRandomIndex(size_t a, size_t b)
{
	return size_t((rand() % (b - a)) + a);
}

template <typename TT, bool SavePoint, bool FixSize>
auto GenRectangles(int rec_num, int const type, auto const &WP)
{
	using Entry = TT::Entry;
	using Box = typename TT::Box;
	using box_pair_seq = std::conditional_t<
		SavePoint == false, parlay::sequence<std::pair<Box, size_t>>,
		parlay::sequence<std::pair<Box, typename TT::entry_seq>>>;

	size_t n = WP.size();
	std::pair<size_t, size_t> range;
	if constexpr (FixSize) {
		if (type == 0) { //* small bracket
			range.first = 1;
			// range.second = static_cast<size_t>(std::sqrt(std::sqrt(1.0 * n)));
			range.second = 100;
		} else if (type == 1) { //* medium bracket
			range.first = 100;
			// range.second = static_cast<size_t>(std::sqrt(1.0 * n));
			range.second = 10000;
		} else if (type == 2) { //* large bracket
			range.first = 10000;
			if (n > 1'000'000) {
				range.second = 1'000'000;
			} // ensure we can generate 50k rect.
			else {
				range.second = n - 1;
			}
		}
	} else {
		if (type == 0) { //* small bracket
			range.first = 1;
			range.second = static_cast<size_t>(
				std::sqrt(std::sqrt(1.0 * n)));
		} else if (type == 1) { //* medium bracket
			range.second = static_cast<size_t>(
				std::sqrt(std::sqrt(1.0 * n)));
			range.second = static_cast<size_t>(std::sqrt(1.0 * n));
		} else if (type == 2) { //* large bracket
			range.second = static_cast<size_t>(std::sqrt(1.0 * n));
			if (n > 1'000'000) {
				range.second = 1'000'000;
			} // ensure we can generate 50k rect.
			else {
				range.second = n - 1;
			}
		}
	}
	box_pair_seq boxes(rec_num);
	[[maybe_unused]] int cnt = 0;

	srand(10);
	auto wp = parlay::sort(WP);
	assert(wp.size() == parlay::remove_duplicates_ordered(wp).size());
	size_t max_size = 0;

	for (int i = 0; i < rec_num; i++) {
		size_t len = GetRandomIndex(range.first, range.second);
		auto l = GetRandomIndex(0, wp.size() - len);
		auto r = l + len;
		max_size = std::max(max_size, len);
		auto lk = Entry::get_key(wp[l]);
		auto rk = Entry::get_key(wp[r]);
		if constexpr (SavePoint) {
			// NOTE: Left close and right open
			boxes[i] = std::make_pair(Box(lk, rk), wp.subseq(l, r));
		} else {
			boxes[i] = std::make_pair(Box(lk, rk), r - l);
		}
	}

	return std::make_pair(boxes, max_size);
}

template <typename TypeTrait, typename Tree, bool kTestTime = true,
	  int kPrint = 1, int kOutputRatio = 1>
void BuildTree(typename TypeTrait::entry_seq const &WP, int const &rounds,
	       Tree &pkd)
{
	using TT [[maybe_unused]] = TypeTrait;
	using entry_seq [[maybe_unused]] = typename TT::entry_seq;
	using Leaf [[maybe_unused]] = typename Tree::Leaf;
	using Interior [[maybe_unused]] = typename Tree::Interior;

	double loop_late = rounds > 1 ? 0.01 : -100;
	size_t n = WP.size();
	// size_t n = 100;
	// entry_seq wp = entry_seq::uninitialized(n);
	auto const wp = Tree::parse_input(WP);
	auto const partial_wp =
		Tree::parse_input(WP.subseq(0, n / kOutputRatio));

	if constexpr (kTestTime) {
		pkd.delete_tree();

		double aveBuild = time_loop(
			rounds, loop_late, [&]() {}, [&]() { pkd.build(wp); },
			[&]() { pkd.delete_tree(); });

		pkd.build(partial_wp);

		if constexpr (kPrint) {
			std::cout << aveBuild << " " << std::flush;
		}
	} else {
		// NOTE: always return a built tree
		pkd.delete_tree();
		pkd.build(partial_wp);
	}
}

template <typename TypeTrait, typename Tree, bool kTestTime = true,
	  bool kSerial = false>
void batch_insert(Tree &pkd, typename TypeTrait::entry_seq const &WP,
		  typename TypeTrait::entry_seq const &WI, int const rounds)
{
	auto const wp = Tree::parse_input(WP);
	auto const wi = Tree::parse_input(WI);
	double loop_late = rounds > 1 ? 0.01 : -100;

	if constexpr (kTestTime) { // NOTE: clean and measure time
		pkd.delete_tree();
		double aveInsert = time_loop(
			rounds, loop_late, [&]() { pkd.build(wp); },
			[&]() { pkd.batch_insert(wi); },
			[&]() { pkd.delete_tree(); });
		std::cout << aveInsert << " " << std::flush;
		// std::cout << ratio << " " << aveInsert << " " << std::endl;
		BuildTree<TypeTrait, Tree, false>(WP, rounds, pkd);
	} else { // NOTE: insert the points from previous tree
		std::cout << "Insert " << wi.size() << " elements\n"
			  << std::flush;
		pkd.batch_insert(wi);
		std::cout << "finish ccp insert" << std::endl;
	}
}

struct StepUpdateLogger {
	int id;
	double t;

	friend std::ostream &operator<<(std::ostream &os,
					StepUpdateLogger const &log)
	{
		os << "(" << log.id << ", " << log.t << ")";
		return os;
	}
};

template <typename TT, typename Tree, bool kInsert>
void BatchInsertByStep(Tree &pkd, auto const &WP, double const insert_ratio,
		       int const rounds)
{
	auto wp = WP;
	size_t n = wp.size();
	auto step = static_cast<size_t>(insert_ratio * n);
	size_t slice_num = (n / step) + (n % step != 0);
	auto insert_batch_seq = parlay::tabulate(slice_num, [&](size_t i) {
		return Tree::parse_input(
			wp.subseq(i * step, std::min((i + 1) * step, n)));
	});

	pkd.delete_tree();

	auto prepare_func = [&]() {
		insert_batch_seq = parlay::tabulate(slice_num, [&](size_t i) {
			return Tree::parse_input(wp.subseq(
				i * step, std::min((i + 1) * step, n)));
		});
	};
	auto incre_build = [&](size_t const batch_num) {
		for (size_t i = 0; i < batch_num; i++) {
			pkd.batch_insert(insert_batch_seq[i]);
		}
	};

	double ave_time = time_loop(
		rounds, 0.01, [&]() { prepare_func(); },
		[&]() { incre_build(slice_num); },
		[&]() { pkd.delete_tree(); });

	std::cout << ave_time << " " << std::flush;

	// NOTE: restore status -- hand back the tree half way through the incremental
	// build, i.e. after inserting the first slice_num / 2 batches (~n / 2
	// entries).
	prepare_func();
	incre_build(slice_num / 2);
}

// NOTE: mirror of BatchInsertByStep. Builds the tree over the whole of WP once,
// then deletes WP back out of it in `1 / delete_ratio` equal-sized batches
// until the tree is empty. Only the chain of batch_delete calls is timed -- the
// tree (re)build and the regeneration of the delete batches both happen in the
// untimed init phase of time_loop.
template <typename TT, typename Tree, bool kDelete>
void BatchDeleteByStep(Tree &pkd, auto const &WP, double const delete_ratio,
		       int const rounds)
{
	size_t const n = WP.size();
	auto const step = std::max<size_t>(
		static_cast<size_t>(delete_ratio * n), size_t(1));
	size_t const slice_num = (n / step) + (n % step != 0);

	// NOTE: build does not consume its input, so parse it once and reuse it for
	// every round.
	auto const build_input = Tree::parse_input(WP);
	parlay::sequence<typename Tree::input_delete_range_type>
		delete_batch_seq;

	// NOTE: batch_delete may consume/permute its input, and every timed run has to
	// start from a freshly built tree holding all of WP.
	auto prepare_func = [&]() {
		delete_batch_seq = parlay::tabulate(slice_num, [&](size_t i) {
			return Tree::parse_delete_input(WP.subseq(
				i * step, std::min((i + 1) * step, n)));
		});
		pkd.delete_tree();
		pkd.build(build_input);
	};
	auto incre_delete = [&](size_t const batch_num) {
		for (size_t i = 0; i < batch_num; i++) {
			pkd.batch_delete(delete_batch_seq[i]);
		}
	};

	pkd.delete_tree();

	double ave_time = time_loop(
		rounds, 0.01, [&]() { prepare_func(); },
		[&]() { incre_delete(slice_num); },
		[&]() { pkd.delete_tree(); });

	std::cout << ave_time << " " << std::flush;

	// NOTE: restore status -- hand back the tree half way through the incremental
	// teardown, i.e. after deleting the first slice_num / 2 batches (~n / 2
	// entries left). prepare_func() rebuilds the full tree first.
	prepare_func();
	incre_delete(slice_num / 2);
}

struct PBTreeLeafSortPred {
	PBTreeLeafSortPred() = default;

	PBTreeLeafSortPred(int ratio)
		: _randomness(ratio)
	{
	}

	template <typename InputNode, typename Range>
	bool operator()(InputNode const *T, Range In) const noexcept
	{
		auto const v = (static_cast<size_t>(T->size) + In.size()) *
				       2654435761ULL ^
			       (reinterpret_cast<uintptr_t>(In.begin()) >> 3) ^
			       (reinterpret_cast<uintptr_t>(In.end()) >> 3);

		// - _randomness = 0: 0–99 ≥ 0 → always true → always sort
		// - _randomness = 100: 0–99 ≥ 100 → always false → never sort
		return (v % 100) >= this->_randomness;
	}

	size_t _randomness = 50;
};

template <typename TypeTrait, typename Tree>
	requires(!pab_tree::is_pab_tree<Tree>)
void TestLeafRandom(Tree &, auto const &, auto const &, auto const &, int const,
		    int const, int const)
{
}

template <typename TypeTrait, typename Tree>
	requires pab_tree::is_pab_tree<Tree>
void TestLeafRandom(Tree &pkd, auto const &WP, auto const &insert_batch_seq,
		    auto const &delete_batch_seq, int const time_period_num,
		    int const rounds, int const ratio)
{
	using TT [[maybe_unused]] = TypeTrait;
	auto const wp = Tree::parse_input(WP);

	auto const query_size =
		static_cast<size_t>(wp.size() * kMemberQueryRatio);
	auto query_ele = wp.subseq(0, query_size);
	auto query_ele_step = query_size / time_period_num;
	for (int i = 0; i < time_period_num; i++) {
		parlay::parallel_for(0, query_ele_step, [&](size_t j) {
			query_ele[(i * query_ele_step) + j] =
				insert_batch_seq[i][j];
		});
	}
	auto member_res = parlay::sequence<bool>(query_ele.size(), true);

	constexpr auto leaf_entries_pattern =
		TypeTrait::leaf_entries_pattern::hybrid;
	constexpr bool sorted_input = false;
	parlay::internal::timer t;

	parlay::sequence<double> insert_time(time_period_num, 0.0);
	parlay::sequence<double> delete_time(time_period_num, 0.0);
	double member_query_time = 0.0;
	double sorted_leaf_ratio = 0.0;

	for (int r = 0; r < rounds; r++) {
		pkd.delete_tree();
		pkd.build(wp);

		for (int i = 0; i < time_period_num; i++) {
			t.reset(), t.start();
			pkd.batch_delete(delete_batch_seq[i],
					 leaf_entries_pattern, sorted_input,
					 PBTreeLeafSortPred(ratio));
			t.stop();
			delete_time[i] += t.total_time();

			t.reset(), t.start();
			pkd.batch_insert(insert_batch_seq[i],
					 leaf_entries_pattern, sorted_input,
					 PBTreeLeafSortPred(ratio));
			t.stop();
			insert_time[i] += t.total_time();
		}

		// sorted-leaf ratio (measured after all updates, before membership)
		sorted_leaf_ratio += pkd.sorted_leaf_ratio();

		// membership
		t.reset(), t.start();
		parlay::parallel_for(0, query_size, [&](size_t j) {
			auto contain =
				pkd.contain(Tree::Entry::get_key(query_ele[j]));
			member_res[j] = contain;
		});
		t.stop();
		member_query_time += t.total_time();
	}

	auto insert_ave =
		parlay::reduce(insert_time) / (rounds * time_period_num);
	auto delete_ave =
		parlay::reduce(delete_time) / (rounds * time_period_num);
	auto member_query_ave = member_query_time / rounds;
	auto sorted_leaf_ratio_ave = sorted_leaf_ratio / rounds;

	std::cout << ratio << " " << insert_ave << " " << delete_ave << " "
		  << member_query_ave << " " << sorted_leaf_ratio_ave
		  << std::endl;
}

template <typename TypeTrait, typename Tree>
	requires(!pab_tree::is_pab_tree<Tree>)
void TestLeafRandomDynamicGraph(Tree &, auto const &, auto const &, auto &,
				int const, int const, int const)
{
}

template <typename TypeTrait, typename Tree>
	requires pab_tree::is_pab_tree<Tree>
void TestLeafRandomDynamicGraph(Tree &pkd, auto const &insert_batch_seq,
				auto const &delete_batch_seq,
				auto &membership_res, int const time_period_num,
				int const rounds, int const ratio)
{
	using TT [[maybe_unused]] = TypeTrait;
	pkd.delete_tree();
	parlay::sequence<typename Tree::input_range_type> wi(time_period_num);
	parlay::sequence<typename Tree::input_delete_range_type> wd(
		time_period_num);
	parlay::sequence<typename Tree::input_range_type> insert_entry(
		time_period_num);
	parlay::sequence<typename Tree::input_delete_range_type> delete_entry(
		time_period_num);
	const size_t member_query_n = membership_res.size();
	auto random_osm_records =
		parlay::random_shuffle(parlay::flatten(insert_batch_seq));
	auto query_points = parlay::tabulate(member_query_n, [&](size_t i) {
		return TT::Entry::get_key(random_osm_records[i]);
	});

	// NOTE: generate box (same as TestDynamicGraph): count how often each
	// source vertex occurs (its out-degree) and build a (vertex, occurrence)
	// sequence, highest occurrence first. The top vertices are used as the
	// keys for the range queries.
	auto edge_list =
		parlay::tabulate(random_osm_records.size(), [&](size_t i) {
			auto key = TT::Entry::get_key(random_osm_records[i]);
			uint64_t u = key >> 32;
			uint64_t v = key & 0xffffffff;
			return std::make_pair(u, v);
		});
	auto range_query_vertex_occurrence = parlay::histogram_by_key(
		parlay::map(edge_list, [](auto const &e) { return e.first; }));
	parlay::sort_inplace(range_query_vertex_occurrence,
			     [](auto const &a, auto const &b) {
				     return a.second > b.second;
			     });
	range_query_vertex_occurrence = range_query_vertex_occurrence.subseq(
		0, std::min(range_query_vertex_occurrence.size(),
			    static_cast<size_t>(kDynamicGraphRangeNum)));
	auto [range_query_offset, tot_range_size] = parlay::scan(
		parlay::map(range_query_vertex_occurrence,
			    [](auto const &p) { return p.second; }),
		parlay::addm<size_t>());
	range_query_offset.push_back(tot_range_size);
	auto range_report_output = typename TT::entry_seq(tot_range_size);

	auto box_seq = parlay::tabulate(
		range_query_vertex_occurrence.size(), [&](size_t i) {
			uint64_t u = range_query_vertex_occurrence[i].first;
			return std::make_pair(
				static_cast<uint64_t>(u << 32),
				static_cast<uint64_t>((u + 1) << 32));
		});
	parlay::sequence<size_t> size_seq(box_seq.size(), 0);

	for (int i = 0; i < time_period_num; i++) {
		wi[i] = Tree::parse_input(insert_batch_seq[i]);
		wd[i] = Tree::parse_delete_input(delete_batch_seq[i]);
	}

	parlay::sequence<double> insert_time(time_period_num, 0.0);
	parlay::sequence<double> delete_time(time_period_num, 0.0);
	parlay::sequence<double> member_query_time(time_period_num, 0.0);
	parlay::sequence<double> range_query_time(time_period_num, 0.0);

	// accumulate the per-round total times; averaged over rounds at the end.
	double insert_sum = 0.0;
	double delete_sum = 0.0;
	double member_query_sum = 0.0;
	double range_query_sum = 0.0;
	double sorted_leaf_ratio_ave = 0.0;

	for (int r = 0; r < rounds; r++) {
		auto const &insert_entry = wi;
		auto const &delete_entry = wd;
		if (rounds == 1) {
			for (int batch_id = 0; batch_id < time_period_num;
			     batch_id++) {
				insert_time[batch_id] = 0.0;
				delete_time[batch_id] = 0.0;
				member_query_time[batch_id] = 0.0;
			}
		}

		// running the osm
		constexpr auto leaf_entries_pattern =
			TT::leaf_entries_pattern::hybrid;
		constexpr bool sorted_input = false;
		parlay::internal::timer t;
		pkd.delete_tree();
		for (int batch_id = 0; batch_id < time_period_num; batch_id++) {
			t.reset(), t.start();
			pkd.batch_insert(insert_entry[batch_id],
					 leaf_entries_pattern, sorted_input,
					 PBTreeLeafSortPred(ratio));
			t.stop();
			insert_time[batch_id] = t.total_time();

			t.reset(), t.start();
			pkd.batch_delete(delete_entry[batch_id],
					 leaf_entries_pattern, sorted_input,
					 PBTreeLeafSortPred(ratio));
			t.stop();
			delete_time[batch_id] = t.total_time();

			// NOTE: test membership query
			t.reset(), t.start();
			parlay::parallel_for(0, member_query_n, [&](size_t j) {
				auto contain = pkd.contain(query_points[j]);
				membership_res[j] = contain;
			});
			t.stop();
			member_query_time[batch_id] = t.total_time();
		}

		// NOTE: end of round — sum across batches for each metric.
		// Omit the first (warmup) round when more than one round is requested.
		if (rounds == 1 || r >= 1) {
			insert_sum += parlay::reduce(insert_time);
			delete_sum += parlay::reduce(delete_time);
			member_query_sum += parlay::reduce(member_query_time);
			// sorted-leaf ratio (measured after all updates, before membership)
			sorted_leaf_ratio_ave += pkd.sorted_leaf_ratio();
		}
	}

	// average of the per-round total times over counted rounds
	double const denom = rounds > 1 ? (rounds - 1) : 1;
	double const insert_ave = insert_sum / denom;
	double const delete_ave = delete_sum / denom;
	double const member_query_ave = member_query_sum / denom;
	sorted_leaf_ratio_ave /= denom;

	std::cout << ratio << " " << insert_ave << " " << delete_ave << " "
		  << member_query_ave << " " << sorted_leaf_ratio_ave
		  << std::endl;
}

template <typename TypeTrait, typename Tree, bool kTestTime = true,
	  bool kSerial = false>
void batch_delete(Tree &pkd, typename TypeTrait::entry_seq const &WP,
		  typename TypeTrait::entry_seq const &WI, int const rounds)
{
	auto const wp = Tree::parse_input(WP);
	auto const wi = Tree::parse_delete_input(WI);
	double loop_late = rounds > 1 ? 0.01 : -100;

	if constexpr (kTestTime) { // NOTE: clean and measure time
		pkd.delete_tree();
		double aveInsert = time_loop(
			rounds, loop_late, [&]() { pkd.build(wp); },
			[&]() { pkd.batch_delete(wi); },
			[&]() { pkd.delete_tree(); });
		std::cout << aveInsert << " " << std::flush;
		// std::cout << ratio << " " << aveInsert << " " << std::endl;
		BuildTree<TypeTrait, Tree, false>(WP, rounds, pkd);
	} else { // NOTE: delete the points from previous tree
		std::cout << "begin ccp delete" << std::endl;
		pkd.batch_delete(wi);
		std::cout << "finish ccp delete" << std::endl;
	}
}

template <typename TT, typename Tree>
void range_count(auto const &wp, Tree const &pkd, int const kRounds,
		 int const rec_num, int const rec_type)
{
	using entry_seq [[maybe_unused]] = typename TT::entry_seq;

	auto [query_box_seq, max_size] =
		GenRectangles<TT, false, false>(rec_num, rec_type, wp);
	parlay::sequence<size_t> kdknn(rec_num, 0);

	double ave_query = time_loop(
		kRounds, 0.01, [&]() {},
		[&]() {
			parlay::parallel_for(0, rec_num, [&](size_t i) {
				// parlay::parallel_for(0, 1, [&](size_t i) {
				[[maybe_unused]] auto size =
					pkd.range_count(query_box_seq[i].first);
				kdknn[i] = size;
				[[maybe_unused]] int output_size =
					query_box_seq[i].second;
				assert(std::cmp_equal(kdknn[i], output_size));
			});
		},
		[&]() {});

	std::cout << "check range count: " << rec_num << " " << rec_type << " "
		  << max_size << std::endl;
	for (int i = 0; i < rec_num; i++) {
		int output_size = query_box_seq[i].second;
		if (std::cmp_not_equal(kdknn[i], output_size)) {
			std::cout << kdknn[i] << " " << output_size << " "
				  << query_box_seq[i].first.first
				  << query_box_seq[i].first.second << std::endl;
		}
		assert(std::cmp_equal(kdknn[i], output_size));
	}

	std::cout << ave_query << "\n" << std::flush;
	return;
}
// NOTE: run range query and check the correct
template <typename TT, typename Tree>
void RangeQuery(auto const &wp, Tree const &pkd, int const kRounds,
		int const rec_num, int const rec_type)
{
	using entry_seq = typename TT::entry_seq;

	auto [query_box_seq, max_size] =
		GenRectangles<TT, true, false>(rec_num, rec_type, wp);
	auto [offset, tot_size] = parlay::scan(
		parlay::delayed_tabulate(
			rec_num,
			[&](size_t i) -> size_t {
				return query_box_seq[i].second.size();
			}),
		parlay::addm<size_t>());
	offset.push_back(tot_size);
	entry_seq Out(tot_size);
	parlay::sequence<size_t> kdknn(rec_num, 0);

	double ave_query = time_loop(
		kRounds, 0.01, [&]() {},
		[&]() {
			parlay::parallel_for(0, rec_num, [&](size_t i) {
				// parlay::parallel_for(0, 1, [&](size_t i) {
				[[maybe_unused]] auto size = pkd.range_report(
					query_box_seq[i].first,
					Out.cut(offset[i], offset[i + 1]));
				kdknn[i] = size;
				[[maybe_unused]] size_t output_size =
					query_box_seq[i].second.size();
				assert(std::cmp_equal(kdknn[i], size) &&
				       Out.size() >= output_size);
			});
		},
		[&]() {});

	std::cout << "check range query: " << rec_num << " " << rec_type << " "
		  << max_size << std::endl;
	for (int i = 0; i < rec_num; i++) {
		int output_size = query_box_seq[i].second.size();
		if (std::cmp_not_equal(kdknn[i], output_size)) {
			std::cout << kdknn[i] << " " << output_size << " "
				  << query_box_seq[i].first.first
				  << query_box_seq[i].first.second << std::endl;
		}
		assert(std::cmp_equal(kdknn[i], output_size));
		// std::cout << kdknn[i] << " " << output_size << " "
		//     << query_box_seq[i].first.first << query_box_seq[i].first.second
		//     << std::endl;
		parlay::sort_inplace(Out.cut(offset[i], offset[i + 1]),
				     [&](auto const &a, auto const &b) {
					     return TT::Entry::get_key(a) <
						    TT::Entry::get_key(b);
				     });
		parlay::sort_inplace(query_box_seq[i].second,
				     [&](auto const &a, auto const &b) {
					     return TT::Entry::get_key(a) <
						    TT::Entry::get_key(b);
				     });
		parlay::parallel_for(0, output_size, [&](size_t j) {
			if (Out[offset[i] + j] !=
			    query_box_seq[i].second.at(j)) {
				std::cout << "wrong: bounding box: "
					  << query_box_seq[i].first.first << " "
					  << query_box_seq[i].first.second
					  << std::endl;
				std::cout << "------ " << Out[offset[i] + j]
					  << " "
					  << query_box_seq[i].second.at(j)
					  << std::endl;
				assert(false);
			}
		});
		// bool flag = true;
		//   for (size_t j = 0; j < output_size; j++) {
		//     if (Out[offset[i] + j] != query_box_seq[i].second.at(j)) {
		//       std::cout << "wrong: bounding box: " <<
		//       query_box_seq[i].first.first
		//                 << " " << query_box_seq[i].first.second << std::endl;
		//       std::cout << "------ " << Out[offset[i] + j] << " "
		//                 << query_box_seq[i].second.at(j) << std::endl;
		//       flag = false;
		//     }
		//   }
		//   assert(flag);
	}

	std::cout << ave_query << "\n" << std::flush;
	return;
}

//* test range count for fix rectangle

//
// template<typename Point>
// void rangeCountFixWithLog(const parlay::sequence<Point>& WP,
// BaseTree<Point>& pkd, Typename* kdknn, const int& rounds,
//                           int rec_type, int rec_num, int DIM) {
//     using Tree = BaseTree<Point>;
//     using Points = typename Tree::Points;
//     using node = typename Tree::node;
//     using Box = typename Tree::Box;
//
//     int n = WP.size();
//
//     auto [query_box_seq, max_size] = gen_rectangles(rec_num, rec_type, WP,
//     DIM); parlay::sequence<size_t> visLeafNum(rec_num, 0),
//     visInterNum(rec_num, 0); parlay::internal::timer t; for (int i = 0; i <
//     rec_num; i++) {
//         double ave_query = time_loop(
//             rounds, -1.0,
//             [&]() {
//                 visInterNum[i] = 0;
//                 visLeafNum[i] = 0;
//             },
//             [&]() { kdknn[i] = pkd.range_count(query_box_seq[i].first,
//             visLeafNum[i], visInterNum[i]); }, [&]() {});
//         if (query_box_seq[i].second != kdknn[i]) std::cout << "wrong" <<
//         std::endl; std::cout << query_box_seq[i].second << " " <<
//         std::scientific << ave_query
//         << std::endl;
//     }
//
//     return;
// }
//
//* test range query for fix rectangle
template <typename TT, typename Tree>
void Membership(Tree const &pkd, auto const &query_ele, int kRounds, auto &res)
{
	using Entry = typename TT::Entry;
	int query_num = query_ele.size();
	parlay::sequence<size_t> vis_leaf(query_num);
	parlay::sequence<size_t> vis_inter(query_num);

	double loop_late = kRounds > 1 ? 0.01 : -100;

	double ave_query = time_loop(
		kRounds, loop_late, [&]() {},
		[&]() {
			parlay::parallel_for(
				0, query_num,
				[&](size_t i) {
					// for (int i = 0; i < query_num; i++) {
					auto contain = pkd.contain(
						Entry::get_key(query_ele[i]));

					res[i] = contain;
					// }
				},
				1);
		},
		[&]() {});

	// std::cout << query_num << " ";
	std::cout << ave_query << " " << std::flush;
}

template <typename TT, typename Tree>
void RangeCountFix(Tree &pkd, [[maybe_unused]] int const &rounds, int rec_num,
		   auto const &query_box_seq)
{
	// auto [query_box_seq, max_size] =
	//     gen_rectangles<Point, Tree, false>(rec_num, rec_type, WP, DIM);
	parlay::sequence<size_t> vis_leaf(rec_num), vis_inter(rec_num);

	// int n = WP.size();
	// size_t step = Out.size() / rec_num;
	// using ref_t = std::reference_wrapper<Point>;
	// parlay::sequence<ref_t> out_ref( Out.size(), std::ref( Out[0] ) );

	double ave_query = time_loop(
		rounds, 0.01, [&]() {},
		[&]() {
			parlay::parallel_for(0, rec_num, [&](size_t i) {
				// for (int i = 0; i < rec_num; i++) {
				[[maybe_unused]] auto size =
					pkd.range_count(query_box_seq[i].first);
			});
		},
		[&]() {});

	std::cout << ave_query << " " << std::flush;
	return;
}

template <typename TT, typename Tree>
void RangeQueryFix(Tree &pkd, [[maybe_unused]] int const &rounds, auto &Out,
		   int rec_num, auto const &query_box_seq)
{
	// auto [query_box_seq, max_size] =
	//     gen_rectangles<Point, Tree, false>(rec_num, rec_type, WP, DIM);
	parlay::sequence<size_t> vis_leaf(rec_num), vis_inter(rec_num);

	auto [offset, tot_size] = parlay::scan(
		parlay::tabulate(rec_num,
				 [&](size_t i) -> size_t {
					 return query_box_seq[i].second;
				 }),
		parlay::addm<size_t>());
	offset.push_back(tot_size);
	Out.resize(tot_size);

	// int n = WP.size();
	// size_t step = Out.size() / rec_num;
	// using ref_t = std::reference_wrapper<Point>;
	// parlay::sequence<ref_t> out_ref( Out.size(), std::ref( Out[0] ) );

	double ave_query = time_loop(
		rounds, 0.01, [&]() {},
		[&]() {
			parlay::parallel_for(0, rec_num, [&](size_t i) {
				// for (int i = 0; i < rec_num; i++) {
				[[maybe_unused]] auto size = pkd.range_report(
					query_box_seq[i].first,
					Out.cut(offset[i], offset[i + 1]));
				// std::cout << size << " " << query_box_seq[i].second << std::endl;
				// if (size != query_box_seq[i].second) {
				//   std::cout << "wrong: bounding box: " <<
				//   query_box_seq[i].first.first
				//             << " " << query_box_seq[i].first.second << std::endl;
				//   std::cout << "------ " << size << " " << query_box_seq[i].second
				//             << std::endl;
				// }
				// }
			});
		},
		[&]() {});

	std::cout << ave_query << " " << std::flush;
	// std::cout << static_cast<double>(parlay::reduce(vis_leaf.cut(0, rec_num)))
	// /
	//                static_cast<double>(rec_num)
	//           << " " << std::flush;
	// std::cout << static_cast<double>(parlay::reduce(vis_inter.cut(0, rec_num)))
	// /
	//                static_cast<double>(rec_num)
	//           << " " << std::flush;
	// std::cout << static_cast<double>(parlay::reduce(gen_box.cut(0, rec_num))) /
	//                static_cast<double>(rec_num)
	//           << " " << std::flush;
	// std::cout << static_cast<double>(parlay::reduce(full_box.cut(0, rec_num)))
	// /
	//                static_cast<double>(rec_num)
	//           << " " << std::flush;
	// std::cout << static_cast<double>(parlay::reduce(skip_box.cut(0, rec_num)))
	// /
	//                static_cast<double>(rec_num)
	//           << " " << std::flush;
	return;
}

template <typename TT, typename Tree>
void RangeQuerySerialWithLog(Tree &pkd, [[maybe_unused]] int const &rounds,
			     auto &Out, const int rec_num, const int rec_type,
			     auto const &query_box_seq)
{
	auto [offset, tot_size] = parlay::scan(
		parlay::delayed_tabulate(rec_num,
					 [&](size_t i) -> size_t {
						 return query_box_seq[i].second;
					 }),
		parlay::addm<size_t>());
	offset.push_back(tot_size);
	Out.resize(tot_size);

	for (int i = 0; i < rec_num; i++) {
		parlay::internal::timer t;
		t.reset(), t.start();
		[[maybe_unused]] auto size =
			pkd.range_report(query_box_seq[i].first,
					 Out.cut(offset[i], offset[i + 1]));
		t.stop();
		std::cout << rec_type << " " << query_box_seq[i].second << " "
			  << std::scientific << t.total_time() << std::endl;
	}

	return;
}

template <typename TT, typename Tree>
void InsertOsmByTime(
	const parlay::sequence<typename TT::entry_seq> &node_by_time, Tree &pkd,
	const int rounds, const int sliding_window_len,
	const size_t member_query_n, auto &member_res, auto &range_res)
{
	pkd.delete_tree();
	const int time_period_num = node_by_time.size();
	parlay::sequence<typename Tree::input_range_type> wi(time_period_num);
	parlay::sequence<typename Tree::input_delete_range_type> wd(
		time_period_num);
	parlay::sequence<typename Tree::input_range_type> insert_entry(
		time_period_num);
	parlay::sequence<typename Tree::input_delete_range_type> delete_entry(
		time_period_num);
	auto query_points = TT::key_seq::uninitialized(member_query_n);
	auto step = member_query_n / time_period_num;
	if (member_query_n % time_period_num != 0) {
		std::cout
			<< "the member query num should be divisible by time period num"
			<< std::endl;
		return;
	}

	for (int i = 0; i < time_period_num; i++) {
		wi[i] = Tree::parse_input(node_by_time[i]);
		wd[i] = Tree::parse_delete_input(node_by_time[i]);

		if (node_by_time[i].size() < step) {
			std::cout << "not enough points in time period " << i
				  << " for member query, need " << step
				  << std::endl;
			return;
		}
		parlay::parallel_for(0, step, [&](size_t j) {
			query_points[(i * step) + j] =
				TT::Entry::get_key(node_by_time[i][j]);
		});
	}

	// generate the range query box
	parlay::sequence<std::pair<int, int>> batch_in_tree;
	for (int i = 0; i < time_period_num; i++) {
		if (i < sliding_window_len) {
			batch_in_tree.emplace_back(0, i + 1);
		} else {
			batch_in_tree.emplace_back(i - sliding_window_len,
						   i + 1);
		}
	}
	auto box_seq_by_year =
		parlay::tabulate(time_period_num, [&](size_t batch_id) {
			bool constexpr kSavePoint = false;
			bool constexpr kFixSize = false;
			auto wp = parlay::flatten(node_by_time.cut(
				batch_in_tree[batch_id].first,
				batch_in_tree[batch_id].second));
			// return query box seq and max size
			return GenRectangles<TT, kSavePoint, kFixSize>(
				kOSMRangeQueryNum, kOSMRecType, wp);
		});

	parlay::sequence<double> insert_time(time_period_num, 0.0);
	parlay::sequence<double> delete_time(time_period_num, 0.0);
	parlay::sequence<double> member_query_time(time_period_num, 0.0);
	parlay::sequence<double> range_query_time(time_period_num, 0.0);
	parlay::sequence<size_t> tree_size(time_period_num, 0);

	for (int r = 0; r < rounds; r++) {
		auto const &insert_entry = wi;
		auto const &delete_entry = wd;

		size_t current_tree_size = 0;
		if (r == 1) { // omit the first rounds
			for (int i = 0; i < time_period_num; i++) {
				insert_time[i] = 0.0;
				delete_time[i] = 0.0;
				member_query_time[i] = 0.0;
			}
		}

		// NOTE: running the osm
		parlay::internal::timer t;
		pkd.delete_tree();
		for (int batch_id = 0; batch_id < time_period_num; batch_id++) {
			t.reset(), t.start();
			pkd.batch_insert(insert_entry[batch_id]);
			t.stop();
			insert_time[batch_id] += t.total_time();
			current_tree_size += node_by_time[batch_id].size();
#ifdef DEBUG
			std::cerr << " insert batch: " << batch_id
				  << std::flush;
			pkd.validate();
#endif // DEBUG

			if (batch_id >= sliding_window_len) {
				t.reset(), t.start();
				pkd.batch_delete(
					delete_entry[batch_id -
						     sliding_window_len]);
				t.stop();
				delete_time[batch_id] += t.total_time();
				current_tree_size -=
					node_by_time[batch_id -
						     sliding_window_len]
						.size();
#ifdef DEBUG
				// std::cerr << " delete batch: " << batch_id - sliding_window_len
				//           << std::flush;
				pkd.validate();
#endif // DEBUG
			} else {
				delete_time[batch_id] = 0.0;
			}
			tree_size[batch_id] = current_tree_size;

			// NOTE: test membership query
			t.reset(), t.start();
			parlay::parallel_for(0, member_query_n, [&](size_t j) {
				auto contain = pkd.contain(query_points[j]);
				member_res[j] = contain;
			});
			t.stop();
			member_query_time[batch_id] += t.total_time();

			// NOTE: range query
			auto query_box_seq = box_seq_by_year[batch_id].first;
			auto [offset, tot_size] = parlay::scan(
				parlay::tabulate(
					kOSMRangeQueryNum,
					[&](size_t i) -> size_t {
						return query_box_seq[i].second;
					}),
				parlay::addm<size_t>());
			offset.push_back(tot_size);
			range_res.resize(tot_size);

			t.reset(), t.start();
			parlay::parallel_for(
				0, kOSMRangeQueryNum, [&](size_t i) {
					[[maybe_unused]] auto size =
						pkd.range_report(
							query_box_seq[i].first,
							range_res.cut(
								offset[i],
								offset[i + 1]));
				});
			t.stop();
			range_query_time[batch_id] += t.total_time();
		}
	}

	std::cout << std::right << " "
		  << "insert_n"
		  << " "
		  << "delete_n"
		  << " "
		  << "tree_n"
		  << " "
		  << "insert_t(s)"
		  << " "
		  << "delete_t(s)"
		  << " "
		  << "member_query_t(s)"
		  << " "
		  << "range_query_t(s)" << std::endl;
	for (int batch_id = 0; batch_id < time_period_num; batch_id++) {
		size_t const delete_n =
			batch_id >= sliding_window_len ?
				node_by_time[batch_id - sliding_window_len]
					.size() :
				0;
		std::cout << std::right << " " << node_by_time[batch_id].size()
			  << " " << delete_n << " " << tree_size[batch_id]
			  << " " << std::fixed << std::setprecision(6)
			  << insert_time[batch_id] / (rounds - 1) << " "
			  << delete_time[batch_id] / (rounds - 1) << " "
			  << member_query_time[batch_id] / (rounds - 1) << " "
			  << range_query_time[batch_id] / (rounds - 1)
			  << std::endl;
	}
}

// NOTE: geometric mean of a numeric range. Entries equal to 0 are omitted
// entirely (not clamped) so empty/idle batches do not drag the mean toward
// zero or trip log(0).
template <typename Range> double GeometricMean(Range const &v)
{
	auto const nz = parlay::filter(v, [](auto x) { return x != 0; });
	if (nz.size() == 0) {
		return 0.0;
	}
	double const log_sum = parlay::reduce(parlay::delayed_map(
		nz, [](auto x) { return std::log(static_cast<double>(x)); }));
	return std::exp(log_sum / static_cast<double>(nz.size()));
}

// NOTE: sum (total) of a numeric range across all batches.
template <typename Range> double Total(Range const &v)
{
	return parlay::reduce(parlay::delayed_map(
		v, [](auto x) { return static_cast<double>(x); }));
}

template <typename TT, typename Tree>
void TestTemporalSignal(
	const parlay::sequence<typename TT::entry_seq> &node_by_time, Tree &pkd,
	const int rounds, const int sliding_window_len,
	const size_t member_query_n, auto &membership_res,
	auto &range_report_res, auto &flatten_seq,
	auto const &membership_query_keys,
	auto const &range_query_vertex_occurrence,
	auto const &range_query_offset)
{
	pkd.delete_tree();
	const int time_period_num = node_by_time.size();
	parlay::sequence<typename Tree::input_range_type> wi(time_period_num);
	parlay::sequence<typename Tree::input_delete_range_type> wd(
		time_period_num);
	parlay::sequence<typename Tree::input_range_type> insert_entry(
		time_period_num);
	parlay::sequence<typename Tree::input_delete_range_type> delete_entry(
		time_period_num);

	for (int i = 0; i < time_period_num; i++) {
		// NOTE: parse input
		wi[i] = Tree::parse_input(node_by_time[i]);
		wd[i] = Tree::parse_delete_input(node_by_time[i]);
	}

	// NOTE: generate the range query box
	auto const range_query_num = range_query_vertex_occurrence.size();
	auto range_query_box_seq =
		parlay::tabulate(range_query_num, [&](size_t i) {
			uint64_t u = range_query_vertex_occurrence[i].first;
			return std::make_pair(
				static_cast<uint64_t>(u << 32),
				static_cast<uint64_t>((u + 1) << 32));
		});
	parlay::sequence<size_t> range_query_output_size(
		range_query_box_seq.size(), 0);

	// std::cout << "begin insert osm by time" << std::endl;

	parlay::sequence<double> insert_time(time_period_num, 0.0);
	parlay::sequence<double> delete_time(time_period_num, 0.0);
	parlay::sequence<double> member_query_time(time_period_num, 0.0);
	parlay::sequence<double> range_query_time(time_period_num, 0.0);
	parlay::sequence<double> flatten_time(time_period_num, 0.0);
	parlay::sequence<size_t> tree_size(time_period_num, 0);

	// puts("");
	for (int r = 0; r < rounds; r++) {
		auto const &insert_entry = wi;
		auto const &delete_entry = wd;

		size_t current_tree_size = 0;
		if (r == 1) { // omit the first rounds
			for (int i = 0; i < time_period_num; i++) {
				insert_time[i] = 0.0;
				delete_time[i] = 0.0;
				member_query_time[i] = 0.0;
			}
		}

		// NOTE: running the osm
		parlay::internal::timer t;
		pkd.delete_tree();
		for (int batch_id = 0; batch_id < time_period_num; batch_id++) {
			t.reset(), t.start();
			pkd.batch_insert(insert_entry[batch_id]);
			t.stop();
			insert_time[batch_id] += t.total_time();
			current_tree_size += node_by_time[batch_id].size();
#ifdef DEBUG
			std::cerr << " insert batch: " << batch_id
				  << std::flush;
			pkd.validate();
#endif // DEBUG

			if (batch_id >= sliding_window_len) {
				t.reset(), t.start();
				pkd.batch_delete(
					delete_entry[batch_id -
						     sliding_window_len]);
				t.stop();
				delete_time[batch_id] += t.total_time();
				current_tree_size -=
					node_by_time[batch_id -
						     sliding_window_len]
						.size();
#ifdef DEBUG
				// std::cerr << " delete batch: " << batch_id - sliding_window_len
				//           << std::flush;
				pkd.validate();
#endif // DEBUG
			} else {
				delete_time[batch_id] = 0.0;
			}
			tree_size[batch_id] = current_tree_size;
			// std::cout << "tree size: " << current_tree_size << std::endl;

			// NOTE: test membership query
			t.reset(), t.start();
			parlay::parallel_for(0, member_query_n, [&](size_t j) {
				auto contain =
					pkd.contain(membership_query_keys[j]);
				membership_res[j] = contain;
			});
			t.stop();
			// std::cout << "batch_id: " << batch_id
			//           << " membership query time: " << t.total_time() << std::endl;
			member_query_time[batch_id] += t.total_time();

			// NOTE: range query — probe the top-occurrence (highest out-degree)
			// source vertices selected from the sorted edge list.
			t.reset(), t.start();
			parlay::parallel_for(
				0, range_query_box_seq.size(), [&](size_t j) {
					[[maybe_unused]] auto size =
						pkd.range_report(
							range_query_box_seq[j],
							range_report_res.cut(
								range_query_offset
									[j],
								range_query_offset
									[j +
									 1]));
					range_query_output_size[j] = size;
				});
			// for (size_t j = 0; j < range_query_box_seq.size(); j++) {
			//   auto size = //     pkd.range_report(range_query_box_seq[j],
			//                     range_report_res.cut(range_query_offset[j],
			//                                          range_query_offset[j + 1]));
			//   range_query_output_size[j] = size;
			//   if (size != 0)
			//     std::cout << size << std::endl;
			// }
			t.stop();
			// std::cout << "batch_id: " << batch_id
			//           << " range query time: " << t.total_time() << std::endl;
			// exit(1);
			range_query_time[batch_id] += t.total_time();

			// NOTE: flatten
			t.reset(), t.start();
			pkd.get_entries(flatten_seq);
			t.stop();
			// std::cout << "batch_id: " << batch_id
			//           << " flatten time: " << t.total_time() << std::endl;
			flatten_time[batch_id] += t.total_time();
		}
	}

	std::cout << std::right << " "
		  << "insert_n"
		  << " "
		  << "delete_n"
		  << " "
		  << "tree_n"
		  << " "
		  << "insert_t(s)"
		  << " "
		  << "delete_t(s)"
		  << " "
		  << "member_query_t(s)"
		  << " "
		  << "range_query_t(s)"
		  << " "
		  << "flatten_t(s)" << std::endl;
	for (int batch_id = 0; batch_id < time_period_num; batch_id++) {
		size_t const delete_n =
			batch_id >= sliding_window_len ?
				node_by_time[batch_id - sliding_window_len]
					.size() :
				0;
		std::cout << std::right << " " << node_by_time[batch_id].size()
			  << " " << delete_n << " " << tree_size[batch_id]
			  << " " << std::fixed << std::setprecision(6)
			  << insert_time[batch_id] / (rounds - 1) << " "
			  << delete_time[batch_id] / (rounds - 1) << " "
			  << member_query_time[batch_id] / (rounds - 1) << " "
			  << range_query_time[batch_id] / (rounds - 1) << " "
			  << flatten_time[batch_id] / (rounds - 1) << std::endl;
	}

	// NOTE: geometric mean of every attribute across all batches
	auto const geo_col = [&](auto &&f) {
		return GeometricMean(
			parlay::tabulate(time_period_num, [&](size_t batch_id) {
				return static_cast<double>(f(batch_id));
			}));
	};
	std::cout << std::right << "> " << geo_col([&](size_t batch_id) {
		return node_by_time[batch_id].size();
	}) << " " << geo_col([&](size_t batch_id) {
		return batch_id >= sliding_window_len ?
			       node_by_time[batch_id - sliding_window_len]
				       .size() :
			       size_t{ 0 };
	}) << " " << geo_col([&](size_t batch_id) {
		return tree_size[batch_id];
	}) << " " << std::fixed
		  << std::setprecision(6) << geo_col([&](size_t batch_id) {
			     return insert_time[batch_id] / (rounds - 1);
		     })
		  << " " << geo_col([&](size_t batch_id) {
			     return delete_time[batch_id] / (rounds - 1);
		     })
		  << " " << geo_col([&](size_t batch_id) {
			     return member_query_time[batch_id] / (rounds - 1);
		     })
		  << " " << geo_col([&](size_t batch_id) {
			     return range_query_time[batch_id] / (rounds - 1);
		     })
		  << " " << geo_col([&](size_t batch_id) {
			     return flatten_time[batch_id] / (rounds - 1);
		     })
		  << std::endl;

	// NOTE: sum of every attribute across all batches
	auto const sum_col = [&](auto &&f) {
		return Total(
			parlay::tabulate(time_period_num, [&](size_t batch_id) {
				return static_cast<double>(f(batch_id));
			}));
	};
	std::cout << std::right << "# " << sum_col([&](size_t batch_id) {
		return node_by_time[batch_id].size();
	}) << " " << sum_col([&](size_t batch_id) {
		return batch_id >= sliding_window_len ?
			       node_by_time[batch_id - sliding_window_len]
				       .size() :
			       size_t{ 0 };
	}) << " " << sum_col([&](size_t batch_id) {
		return tree_size[batch_id];
	}) << " " << std::fixed
		  << std::setprecision(6) << sum_col([&](size_t batch_id) {
			     return insert_time[batch_id] / (rounds - 1);
		     })
		  << " " << sum_col([&](size_t batch_id) {
			     return delete_time[batch_id] / (rounds - 1);
		     })
		  << " " << sum_col([&](size_t batch_id) {
			     return member_query_time[batch_id] / (rounds - 1);
		     })
		  << " " << sum_col([&](size_t batch_id) {
			     return range_query_time[batch_id] / (rounds - 1);
		     })
		  << " " << sum_col([&](size_t batch_id) {
			     return flatten_time[batch_id] / (rounds - 1);
		     })
		  << std::endl;
}

template <typename TT, typename Tree>
void TestDynamicGraph(const auto &insert_batch_seq,
		      const auto &delete_batch_seq, Tree &pkd, const int rounds,
		      auto &membership_res, auto &range_report_output,
		      auto &flatten_seq, auto const &membership_query_keys,
		      auto const &range_query_vertex_occurrence,
		      auto const &range_query_offset)
{
	pkd.delete_tree();
	auto time_period_num = insert_batch_seq.size();
	parlay::sequence<typename Tree::input_range_type> wi(time_period_num);
	parlay::sequence<typename Tree::input_delete_range_type> wd(
		time_period_num);
	parlay::sequence<typename Tree::input_range_type> insert_entry(
		time_period_num);
	parlay::sequence<typename Tree::input_delete_range_type> delete_entry(
		time_period_num);
	const size_t member_query_n = membership_res.size();

	for (int i = 0; i < time_period_num; i++) {
		wi[i] = Tree::parse_input(insert_batch_seq[i]);
		wd[i] = Tree::parse_delete_input(delete_batch_seq[i]);
	}

	// NOTE: generate the range query box
	auto const range_query_num = range_query_vertex_occurrence.size();
	auto box_seq = parlay::tabulate(range_query_num, [&](size_t i) {
		uint64_t u = range_query_vertex_occurrence[i].first;
		return std::make_pair(static_cast<uint64_t>(u << 32),
				      static_cast<uint64_t>((u + 1) << 32));
	});
	parlay::sequence<size_t> size_seq(box_seq.size(), 0);

	parlay::sequence<double> insert_time(time_period_num, 0.0);
	parlay::sequence<double> delete_time(time_period_num, 0.0);
	parlay::sequence<double> member_query_time(time_period_num, 0.0);
	parlay::sequence<double> range_query_time(time_period_num, 0.0);
	parlay::sequence<double> flatten_time(time_period_num, 0.0);
	parlay::sequence<size_t> tree_size(time_period_num, 0);

	for (int r = 0; r < rounds; r++) {
		auto const &insert_entry = wi;
		auto const &delete_entry = wd;

		size_t current_tree_size = 0;
		if (r == 1) { // omit the first rounds
			for (int i = 0; i < time_period_num; i++) {
				insert_time[i] = 0.0;
				delete_time[i] = 0.0;
				member_query_time[i] = 0.0;
				range_query_time[i] = 0.0;
			}
		}

		// running the osm
		parlay::internal::timer t;
		pkd.delete_tree();
		for (int batch_id = 0; batch_id < time_period_num; batch_id++) {
			// std::cout << node_by_time[i].size() << " ";
			t.reset(), t.start();
			pkd.batch_insert(insert_entry[batch_id]);
			t.stop();
			insert_time[batch_id] += t.total_time();
			current_tree_size += insert_batch_seq[batch_id].size();

#ifdef DEBUG
			std::cerr << " insert batch: " << batch_id
				  << std::flush;
			pkd.validate();
#endif // DEBUG

			t.reset(), t.start();
			pkd.batch_delete(delete_entry[batch_id]);
			t.stop();
			delete_time[batch_id] += t.total_time();
			current_tree_size -= delete_batch_seq[batch_id].size();

#ifdef DEBUG
			std::cerr << " delete batch: " << batch_id
				  << std::flush;
			pkd.validate();
#endif // DEBUG

			tree_size[batch_id] = current_tree_size;

			// NOTE: test membership query
			t.reset(), t.start();
			parlay::parallel_for(0, member_query_n, [&](size_t j) {
				auto contain =
					pkd.contain(membership_query_keys[j]);
				membership_res[j] = contain;
			});
			t.stop();
			member_query_time[batch_id] += t.total_time();

			// NOTE: range report query
			t.reset(), t.start();
			parlay::parallel_for(0, box_seq.size(), [&](size_t j) {
				[[maybe_unused]] auto size = pkd.range_report(
					box_seq[j],
					range_report_output.cut(
						range_query_offset[j],
						range_query_offset[j + 1]));
				size_seq[j] = size;
			});
			t.stop();
			range_query_time[batch_id] += t.total_time();
			// std::cout << "Non empty range query num: "
			//           << parlay::reduce(
			//                parlay::delayed_tabulate(
			//                  box_seq.size(),
			//                  [&](size_t j) -> size_t { return size_seq[j] > 0; }),
			//                parlay::addm<size_t>())
			//           << std::endl;

			// NOTE: flatten
			t.reset(), t.start();
			pkd.get_entries(flatten_seq);
			t.stop();
			flatten_time[batch_id] += t.total_time();
			// std::cout << std::endl;
		}
	}

	std::cout << std::right << " "
		  << "insert_n"
		  << " "
		  << "delete_n"
		  << " "
		  << "tree_n"
		  << " "
		  << "insert_t(s)"
		  << " "
		  << "delete_t(s)"
		  << " "
		  << "find_t(s)"
		  << " "
		  << "range_t(s)"
		  << " "
		  << "flatten_t(s)" << std::endl;
	for (int batch_id = 0; batch_id < time_period_num; batch_id++) {
		// size_t const delete_n =
		//   batch_id >= sliding_window_len
		//     ? node_by_time[batch_id - sliding_window_len].size()
		//     : 0;
		std::cout << std::right << " "
			  << insert_batch_seq[batch_id].size() << " "
			  << delete_batch_seq[batch_id].size() << " "
			  << tree_size[batch_id] << " " << std::fixed
			  << std::setprecision(6)
			  << insert_time[batch_id] / (rounds - 1) << " "
			  << delete_time[batch_id] / (rounds - 1) << " "
			  << member_query_time[batch_id] / (rounds - 1) << " "
			  << range_query_time[batch_id] / (rounds - 1) << " "
			  << flatten_time[batch_id] / (rounds - 1) << std::endl;
	}

	// NOTE: geometric mean of every attribute across all batches
	auto const geo_col = [&](auto &&f) {
		return GeometricMean(
			parlay::tabulate(time_period_num, [&](size_t batch_id) {
				return static_cast<double>(f(batch_id));
			}));
	};
	std::cout << std::right << "> " << geo_col([&](size_t batch_id) {
		return insert_batch_seq[batch_id].size();
	}) << " " << geo_col([&](size_t batch_id) {
		return delete_batch_seq[batch_id].size();
	}) << " " << geo_col([&](size_t batch_id) {
		return tree_size[batch_id];
	}) << " " << std::fixed
		  << std::setprecision(6) << geo_col([&](size_t batch_id) {
			     return insert_time[batch_id] / (rounds - 1);
		     })
		  << " " << geo_col([&](size_t batch_id) {
			     return delete_time[batch_id] / (rounds - 1);
		     })
		  << " " << geo_col([&](size_t batch_id) {
			     return member_query_time[batch_id] / (rounds - 1);
		     })
		  << " " << geo_col([&](size_t batch_id) {
			     return range_query_time[batch_id] / (rounds - 1);
		     })
		  << " " << geo_col([&](size_t batch_id) {
			     return flatten_time[batch_id] / (rounds - 1);
		     })
		  << std::endl;

	// NOTE: sum of every attribute across all batches
	auto const sum_col = [&](auto &&f) {
		return Total(
			parlay::tabulate(time_period_num, [&](size_t batch_id) {
				return static_cast<double>(f(batch_id));
			}));
	};
	std::cout << std::right << "# " << sum_col([&](size_t batch_id) {
		return insert_batch_seq[batch_id].size();
	}) << " " << sum_col([&](size_t batch_id) {
		return delete_batch_seq[batch_id].size();
	}) << " " << sum_col([&](size_t batch_id) {
		return tree_size[batch_id];
	}) << " " << std::fixed
		  << std::setprecision(6) << sum_col([&](size_t batch_id) {
			     return insert_time[batch_id] / (rounds - 1);
		     })
		  << " " << sum_col([&](size_t batch_id) {
			     return delete_time[batch_id] / (rounds - 1);
		     })
		  << " " << sum_col([&](size_t batch_id) {
			     return member_query_time[batch_id] / (rounds - 1);
		     })
		  << " " << sum_col([&](size_t batch_id) {
			     return range_query_time[batch_id] / (rounds - 1);
		     })
		  << " " << sum_col([&](size_t batch_id) {
			     return flatten_time[batch_id] / (rounds - 1);
		     })
		  << std::endl;
}

template <typename E, int kInputType = 0>
auto ReadEntriesText(char const *iFile, parlay::sequence<E> &wp,
		     size_t offset = 0) -> std::pair<size_t, int>
{
	using K [[maybe_unused]] = E::K;
	using V [[maybe_unused]] = E::V;

	parlay::sequence<char> S = readStringFromFile(iFile);
	parlay::sequence<char *> W = stringToWords(S);
	size_t N = std::stoul(W[0], nullptr, 10);
	int dim = std::stoi(W[1]);
	assert(N > 0 && dim >= 1);

	auto pts = W.cut(2, W.size());
	assert(pts.size() % dim == 0);
	size_t n = pts.size() / dim;
	assert(n == N);
	wp.clear();
	wp.resize(N);
	parlay::parallel_for(0, n, [&](size_t i) -> void {
		if constexpr (kInputType == InputType::kDynamicGraphInput) {
			uint64_t u = std::stoull(pts[i * dim]);
			uint64_t v = std::stoull(pts[(i * dim) + 1]);
			uint64_t k = (u << 32) | v;
			wp[i].ReadKey(std::to_string(k));
			if (dim == 2) {
				wp[i].ReadVal(std::to_string(i + offset));
			} else if (dim == 3) {
				wp[i].ReadVal(pts[(i * dim) + 2]);
			}
		} else if constexpr (kInputType == InputType::kOsmInput) {
			wp[i].ReadKey(pts[i * dim]);
			wp[i].ReadVal(pts[(i * dim) + 1]);
		} else {
			wp[i].ReadKey(pts[i * dim]);
			wp[i].ReadVal(pts[(i * dim) + 1]);
		}
		// if (wp[i].key == 0 && wp[i].val == 0) {
		//   std::cout << "zero key val: " << i << " " << pts[i * dim] << " "
		//             << pts[(i * dim) + 1] << std::endl;
		// }
	});
	return std::make_pair(N, dim);
}

// NOTE: binary counterpart of ReadEntries. Reads the ".bin" format described
// in binary_io.h (header [N, dim, width], 32- or 64-bit values widened
// transparently). Same interface and semantics as ReadEntries (overwrites wp,
// returns (N, dim)).
template <typename E, int kInputType = 0>
auto ReadEntriesBinary(char const *iFile, parlay::sequence<E> &wp,
		       size_t offset = 0) -> std::pair<size_t, int>
{
	auto const [N, raw_dim, data] = ReadBinaryEntries(iFile);
	int dim = static_cast<int>(raw_dim);
	assert(N > 0 && dim >= 1);

	wp.clear();
	wp.resize(N);
	parlay::parallel_for(0, N, [&](size_t i) -> void {
		if constexpr (kInputType == InputType::kDynamicGraphInput) {
			uint64_t u = data[i * dim];
			uint64_t v = data[(i * dim) + 1];
			wp[i].key = (u << 32) | v;
			if (raw_dim == 2) {
				wp[i].val = i + offset;
			} else if (raw_dim == 3) {
				wp[i].val = data[(i * dim) + 2];
			}
		} else {
			// NOTE: both kOsmInput and kNormalInput store key/val directly — the
			// binary payload is already numeric, so no string parse (ReadKey/ReadVal
			// via std::stoul) is needed here (unlike the text reader).
			wp[i].key = data[i * dim];
			wp[i].val = data[(i * dim) + 1];
		}
	});
	return std::make_pair(N, dim);
}

// NOTE: dispatch layer — decides the reader from the file path:
// ".bin" suffix -> ReadEntriesBinary, anything else -> ReadEntriesText.
template <typename E, int kInputType = 0>
auto ReadEntries(char const *iFile, parlay::sequence<E> &wp, size_t offset = 0)
	-> std::pair<size_t, int>
{
	std::string_view const path(iFile);
	auto const res =
		path.ends_with(".bin") ?
			ReadEntriesBinary<E, kInputType>(iFile, wp, offset) :
			ReadEntriesText<E, kInputType>(iFile, wp, offset);

	// NOTE: manual sanity check of the read
	// size_t const head = std::min<size_t>(wp.size(), 5);
	// size_t const tail = std::min<size_t>(wp.size() - head, 5);
	// std::cout << "Read " << wp.size() << " entries from ";
	// std::cout << iFile << "; first " << head << ":";
	// for (size_t i = 0; i < head; i++) {
	//   std::cout << " (" << wp[i] << ")";
	// }
	// std::cout << "; last " << tail << ":";
	// for (size_t i = wp.size() - tail; i < wp.size(); i++) {
	//   std::cout << " (" << wp[i] << ")";
	// }
	// std::cout << std::endl;

	auto dedup = parlay::remove_duplicates_ordered(
		wp,
		[&](auto const &a, auto const &b) { return a.key < b.key; });
	if (dedup.size() != wp.size()) {
		std::cout
			<< "Warning: input contains "
			<< (wp.size() - dedup.size())
			<< " duplicate keys; only the first occurrence is kept."
			<< std::endl;
	}
	return res;
}

template <typename E>
auto ReadPointsSerial(char const *iFile, parlay::sequence<E> &wp,
		      [[maybe_unused]] int id_offset = 0)
	-> std::pair<size_t, int>
{
	std::ifstream fs;
	fs.open(iFile);
	size_t N(0);
	int dim(0);
	string str;
	fs >> str, N = stol(str);
	fs >> str, dim = stoi(str);
	// LOG << N << " " << Dim << ENDL;
	wp.resize(N);
	for (size_t i = 0; i < N; i++) {
		fs >> str;
		wp[i].ReadKey(str);
		for (int j = 1; j < dim; j++) {
			fs >> str;
			wp[i].ReadVal(str);
		}
	}
	return std::make_pair(N, dim);
}

void PrintTreeParam(char const *tree_name, int leaf_size)
{
	std::cout << "Tree: " << tree_name << "; ";
	std::cout << "Buf: " << leaf_size << "; ";
}

// NOTE: default test functions for all custom tree
static auto constexpr DefaultTestFunc = []<class TreeDesc, typename TypeTrait,
					   typename Options>(Options opts) {
	using Tree = TreeDesc::TreeType;
	using TT [[maybe_unused]] = TypeTrait;
	using entry_seq = TT::entry_seq;
	entry_seq wp;
	entry_seq wi;

	Tree tree;
	constexpr bool kTestTime = true;

	// PARA: lambda for membership
	auto membership_query = [&](auto const &query_ele) {
		auto res = parlay::sequence<bool>(query_ele.size(), true);
		Membership<TT>(tree, query_ele, opts.rounds, res);
	};

	// PARA: lambda for range query
	auto range_query = [&](auto const &base_points) {
		bool constexpr kSavePoint = false;
		bool constexpr kFixSize = false;
		// for (int rec_type = 0; rec_type < kRangeQueryType; rec_type++) {
		int rec_type = 1;
		auto [query_box_seq, max_size] =
			GenRectangles<TT, kSavePoint, kFixSize>(
				kRangeQueryNum, rec_type, base_points);
		typename TT::entry_seq Out;
		RangeQueryFix<TT>(tree, opts.rounds, Out, kRangeQueryNum,
				  query_box_seq);
		// }
	};

	// PARA: lambda for range count
	auto range_count = [&](auto const &base_points) {
		bool constexpr kSavePoint = false;
		bool constexpr kFixSize = false;
		// for (int rec_type = 0; rec_type < kRangeQueryType; rec_type++) {
		int rec_type = 1;
		auto [query_box_seq, max_size] =
			GenRectangles<TT, kSavePoint, kFixSize>(
				kRangeQueryNum, rec_type, base_points);
		RangeCountFix<TT>(tree, opts.rounds, kRangeQueryNum,
				  query_box_seq);
		// }
	};

	// PARA: lambda for the queries run on a half filled tree, shared by tags
	// 1 << 5, 1 << 6 and 1 << 7. Pass the half the tree holds and the half it
	// does not.
	auto incre_update_query = [&](auto const &entry_in_tree,
				      auto const &entry_not_in_tree) {
		// NOTE: take the ratio over the whole input, like the membership block
		// above.
		auto const batch_size = static_cast<size_t>(
			(entry_in_tree.size() + entry_not_in_tree.size()) *
			kMemberQueryRatio);

		// NOTE: three membership passes -- all hits, all misses, then a mix. For
		// the mix, put both halves together, shuffle, and take the first
		// batch_size. cut, not subseq: these are slices.
		// membership_query(entry_in_tree.cut(0, batch_size));
		// membership_query(entry_not_in_tree.cut(0, batch_size));
		auto const shuffled = parlay::random_shuffle(
			parlay::append(entry_in_tree, entry_not_in_tree));
		membership_query(shuffled.cut(0, batch_size));

		range_query(entry_in_tree);
		range_count(entry_in_tree);
	};

#ifdef MEMCHECK
	read_points_serial<typename TT::E>(opts.input_file_path.c_str(), wp, 0);
	tree.build(wp.cut(0, wp.size()));
	tree.delete_tree();
	return;
#endif // MEMCHECK

	if ((opts.tag & (1 << 5)) == 0) {
		ReadEntries<typename TT::E>(opts.input_file_path.c_str(), wp);
		BuildTree<TT, Tree, kTestTime, 2>(wp, opts.rounds, tree);

		// // NOTE: batch insert
		if (opts.tag & (1 << 0)) {
			if (opts.summary) {
				parlay::sequence<double> const ratio_seq = {
					0.0001, 0.001, 0.01, 0.1
				};
				for (auto ratio : ratio_seq) {
					auto [base_path, batch_path] =
						opts.ParseInsertBatchPath(
							ratio);
					// std::cout << base_path << " " << batch_path << std::endl;
					[[maybe_unused]] auto [n, d] =
						ReadEntries<typename TT::E>(
							base_path.c_str(), wp);
					[[maybe_unused]] auto [n2, d2] =
						ReadEntries<typename TT::E>(
							batch_path.c_str(), wi);
					batch_insert<TT, Tree, kTestTime>(
						tree, wp, wi, opts.rounds);
				}
			} else {
				auto [base_path, batch_path] =
					opts.ParseInsertBatchPath(
						opts.batch_ratio);
				// std::cout << base_path << " " << batch_path << std::endl;
				[[maybe_unused]] auto [n, d] =
					ReadEntries<typename TT::E>(
						base_path.c_str(), wp);
				[[maybe_unused]] auto [n2, d2] =
					ReadEntries<typename TT::E>(
						batch_path.c_str(), wi);
				batch_insert<TT, Tree, kTestTime>(tree, wp, wi,
								  opts.rounds);
			}
		}

		// NOTE: batch delete
		if (opts.tag & (1 << 1)) {
			if (opts.summary) {
				parlay::sequence<double> const ratio_seq = {
					0.0001, 0.001, 0.01, 0.1
				};
				// parlay::sequence<double> const ratio_seq = { 0.01, 0.1 };
				for (auto ratio : ratio_seq) {
					// puts("------------------------------");
					auto [base_path, batch_path] =
						opts.ParseDeleteBatchPath(
							ratio);
					// std::cout << base_path << " " << batch_path << std::endl;
					[[maybe_unused]] auto [n, d] =
						ReadEntries<typename TT::E>(
							base_path.c_str(), wp);
					[[maybe_unused]] auto [n2, d2] =
						ReadEntries<typename TT::E>(
							batch_path.c_str(), wi);
					batch_delete<TT, Tree, kTestTime>(
						tree, wp, wi, opts.rounds);
				}
			} else {
				auto [base_path, batch_path] =
					opts.ParseDeleteBatchPath(
						opts.batch_ratio);
				// std::cout << base_path << " " << batch_path << std::endl;
				[[maybe_unused]] auto [n, d] =
					ReadEntries<typename TT::E>(
						base_path.c_str(), wp);
				[[maybe_unused]] auto [n2, d2] =
					ReadEntries<typename TT::E>(
						batch_path.c_str(), wi);
				batch_delete<TT, Tree, kTestTime>(tree, wp, wi,
								  opts.rounds);
			}
		}

		if (opts.query_type != 0 && opts.tag != 0) {
			auto [base_path, batch_path] =
				opts.ParseInsertBatchPath(opts.batch_ratio);
			[[maybe_unused]] auto [n, d] =
				ReadEntries<typename TT::E>(base_path.c_str(),
							    wp);
			BuildTree<TT, Tree, kTestTime, 0>(wp, opts.rounds,
							  tree);
		}

		// NOTE: membership
		if (opts.query_type & (1 << 0)) {
			auto batch_size = static_cast<size_t>(
				wp.size() * kMemberQueryRatio);
			typename TT::entry_seq query_ele;
			if (wi.size() == 0) {
				query_ele = wp.subseq(0, batch_size);
				membership_query(query_ele);
			} else {
				query_ele = wp.subseq(0, batch_size);
				membership_query(query_ele);

				query_ele = wi.subseq(0, batch_size);
				membership_query(query_ele);

				query_ele = parlay::append(
					wp.subseq(0, batch_size / 2),
					wi.subseq(0, batch_size -
							     (batch_size / 2)));
				query_ele = parlay::random_shuffle(query_ele);
				membership_query(query_ele);
			}
		}

		// NOTE: range query
		if (opts.query_type & (1 << 1)) {
			range_query(wp);
		}

		// NOTE: range count
		if (opts.query_type & (1 << 2)) {
			range_count(wp);
		}

		// NOTE: range query with log
		if (opts.query_type & (1 << 3)) {
			bool constexpr kSavePoint = false;
			bool constexpr kFixSize = false;
			for (int rec_type = 0; rec_type < kRangeQueryType;
			     rec_type++) {
				auto [query_box_seq, max_size] =
					GenRectangles<TT, kSavePoint, kFixSize>(
						kRangeQueryWithLogNum, rec_type,
						wp);
				typename TT::entry_seq Out;
				RangeQuerySerialWithLog<TT>(
					tree, opts.rounds, Out,
					kRangeQueryWithLogNum, rec_type,
					query_box_seq);
			}
		}

		// NOTE: batch update by size
		if (opts.tag & (1 << 3)) {
			parlay::sequence<double> const ratio_seq = {
				0.0001, 0.0002, 0.0005, 0.001, 0.002,
				0.005,	0.01,	0.02,	0.05,  0.1,
				0.2,	0.5,	1.0
			};
			// parlay::sequence<double> const ratio_seq = { 1.0 };
			for (auto ratio : ratio_seq) {
				auto [base_path, batch_path] =
					opts.ParseInsertBatchPath(ratio);
				// std::cout << base_path << " " << batch_path << std::endl;
				[[maybe_unused]] auto [n, d] =
					ReadEntries<typename TT::E>(
						base_path.c_str(), wp);
				[[maybe_unused]] auto [n2, d2] =
					ReadEntries<typename TT::E>(
						batch_path.c_str(), wi);
				batch_insert<TT, Tree, kTestTime>(tree, wp, wi,
								  opts.rounds);
			}

			for (auto ratio : ratio_seq) {
				auto [base_path, batch_path] =
					opts.ParseDeleteBatchPath(ratio);
				// std::cout << base_path << " " << batch_path << std::endl;
				[[maybe_unused]] auto [n, d] =
					ReadEntries<typename TT::E>(
						base_path.c_str(), wp);
				[[maybe_unused]] auto [n2, d2] =
					ReadEntries<typename TT::E>(
						batch_path.c_str(), wi);
				batch_delete<TT, Tree, kTestTime>(tree, wp, wi,
								  opts.rounds);
			}
		}

		// NOTE: test randomness
		if (opts.tag & (1 << 4)) {
			const parlay::sequence<int> sort_ratio_seq = {
				0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100
			};
			auto const time_period_num = 100;
			auto const random_in = parlay::random_shuffle(wp);
			auto const step = wp.size() / time_period_num;
			auto const insert_batch_seq = parlay::tabulate(
				time_period_num, [&](size_t i) {
					return Tree::parse_input(
						random_in.subseq(
							i * step,
							std::min((i + 1) * step,
								 wp.size())));
				});
			auto const delete_batch_seq = parlay::tabulate(
				time_period_num, [&](size_t i) {
					return Tree::parse_delete_input(
						random_in.subseq(
							i * step,
							std::min((i + 1) * step,
								 wp.size())));
				});

			puts("");
			for (auto ratio : sort_ratio_seq) {
				TestLeafRandom<TT>(tree, wp, insert_batch_seq,
						   delete_batch_seq,
						   time_period_num, opts.rounds,
						   ratio);
			}
		}
	}

	//  NOTE: static build then query
	if (opts.tag & (1 << 5)) {
		ReadEntries<typename TT::E>(opts.input_file_path.c_str(), wp);
		// NOTE: subseq, not cut. BuildTree wants a sequence, a slice will not
		// bind. The extra copy is not timed.
		BuildTree<TT, Tree, kTestTime, 2, 2>(wp, opts.rounds, tree);

		// NOTE: only the first half of wp was built into the tree.
		incre_update_query(wp.cut(0, wp.size() / 2),
				   wp.cut(wp.size() / 2, wp.size()));
	}

	// NOTE: one dataset per ratio, reloaded inside the loop rather than one
	// file sliced four ways.  incre_data_generator lays its batches out so that
	// consecutive windows of `batch_n` entries ARE the batches, which only
	// lines up with the step BatchInsertByStep slices at when the file was
	// generated for that ratio -- ins/<1/ratio>/ is exactly the directory the
	// generator writes for -batch_n = base_n * ratio.  opts.batch_ratio (-b) is
	// deliberately unused here; the ratio_seq below is the axis.
	// NOTE: each entry needs its own generated dataset under ins/<1/ratio>/ and
	// del/<1/ratio>/ -- see scripts/gen_incre_data.sh.
	parlay::sequence<double> const incre_ratio_seq = { 0.0001, 0.001, 0.01,
							   0.1 };

	//  NOTE: incremental insert
	if (opts.tag & (1 << 6)) {
		for (auto ratio : incre_ratio_seq) {
			// NOTE: batch_path, not base_path -- incre_data_generator writes the
			// insert stream as ins/<1/ratio>/<i>.batch.bin. There is no base/batch
			// pair in these datasets; the whole file is the batch stream.
			auto [base_path, batch_path] =
				opts.ParseInsertBatchPath(ratio);
			ReadEntries<typename TT::E>(batch_path.c_str(), wp);
			BatchInsertByStep<TT, Tree, kTestTime>(tree, wp, ratio,
							       opts.rounds);

			// NOTE: the first half of wp was inserted, the second half was not.
			incre_update_query(wp.cut(0, wp.size() / 2),
					   wp.cut(wp.size() / 2, wp.size()));
		}
	}

	//  NOTE: incremental delete
	if (opts.tag & (1 << 7)) {
		for (auto ratio : incre_ratio_seq) {
			auto [base_path, batch_path] =
				opts.ParseDeleteBatchPath(ratio);
			ReadEntries<typename TT::E>(batch_path.c_str(), wi);
			BatchDeleteByStep<TT, Tree, kTestTime>(tree, wi, ratio,
							       opts.rounds);

			// NOTE: the first half of wi was deleted, so the second half is the one
			// left in the tree.
			incre_update_query(wi.cut(wi.size() / 2, wi.size()),
					   wi.cut(0, wi.size() / 2));
		}
	}

	std::cout << "\n" << std::flush;

	tree.delete_tree();

	return;
};

// NOTE: count how many sequentially-numbered batch file pairs
// (<i>_insert.bin / <i>_delete.bin) live under `dir`. Starts at i = 0 and
// stops at the first missing insert file, so the batches are expected to be
// contiguous from 0.
static int CountDynamicGraphBatches(std::string const &dir)
{
	int n = 0;
	while (std::filesystem::exists(dir + std::to_string(n) +
				       "_insert.bin")) {
		++n;
	}
	return n;
}

static auto constexpr TestTemporalGraph = []<class TreeDesc, typename TypeTrait,
					     typename Options>(Options opts) {
	using Tree = TreeDesc::TreeType;
	using TT [[maybe_unused]] = TypeTrait;
	using entry_seq = TT::entry_seq;
	entry_seq wp;
	entry_seq wi;

	Tree tree;
	constexpr bool kTestTime = true;
#if defined(DYNAMIC_GRAPH)
	constexpr int kInputType = InputType::kDynamicGraphInput;
#elif defined(OSM)
	constexpr int kInputType = InputType::kOsmInput;
#else
	constexpr int kInputType = InputType::kNormalInput;
#endif

	// NOTE: real world sliding window
	if (opts.tag & (1 << 3)) {
		string osm_path = opts.input_file_path;
		entry_seq osm_records;
		auto [input_num, input_dim] =
			ReadEntries<typename TT::E, kInputType>(
				osm_path.c_str(), osm_records);

		parlay::stable_sort_inplace(osm_records, [&](auto const &a,
							     auto const &b) {
			return TT::Entry::get_val(a) < TT::Entry::get_val(b);
		});
		const std::vector<int> files = { 2015, 2016, 2017, 2018, 2019,
						 2020, 2021, 2022, 2023, 2024 };
		const auto year_num = files.size();
		auto node_by_year = parlay::tabulate(year_num, [&](size_t i) {
			return parlay::filter(
				osm_records, [&](auto const &record) {
					return TT::Entry::get_val(record) ==
					       files[i];
				});
		});

		// membership
		auto member_res =
			parlay::sequence<bool>(kMemberQueryOsmSize, true);
		auto range_res = typename TT::entry_seq();

		InsertOsmByTime<TT>(node_by_year, tree, opts.rounds,
				    kOsmWindowLen, kMemberQueryOsmSize,
				    member_res, range_res);
	}

	// NOTE: temporal graph osm
	if (opts.tag & (1 << 4)) {
		// string dynamic_graph_path =
		//   "/data/zmen002/geometry/osm_temporal_graph/north_america/";
		string dynamic_graph_path = opts.input_file_path;

		int const kDynamicGraphBatchNum =
			CountDynamicGraphBatches(dynamic_graph_path);
		std::vector<std::string> insert_files(kDynamicGraphBatchNum);
		std::vector<std::string> delete_files(kDynamicGraphBatchNum);
		for (int i = 0; i < kDynamicGraphBatchNum; i++) {
			insert_files[i] = dynamic_graph_path +
					  std::to_string(i) + "_insert.bin";
			delete_files[i] = dynamic_graph_path +
					  std::to_string(i) + "_delete.bin";
		}
		parlay::sequence<entry_seq> insert_batch_seq(
			kDynamicGraphBatchNum);
		parlay::sequence<entry_seq> delete_batch_seq(
			kDynamicGraphBatchNum);
		size_t total_entries = 0;
		for (int i = 0; i < kDynamicGraphBatchNum; i++) {
			auto [n1, dim1] =
				ReadEntries<typename TT::E, kInputType>(
					insert_files[i].c_str(),
					insert_batch_seq[i], total_entries);
			total_entries += n1;

			auto [n2, dim2] =
				ReadEntries<typename TT::E, kInputType>(
					delete_files[i].c_str(),
					delete_batch_seq[i], total_entries);
			total_entries += n2;
		}

		// NOTE: membership query result
		auto membership_res =
			parlay::sequence<bool>(kMemberQueryOsmSize, true);
		auto random_osm_records = parlay::random_shuffle(
			parlay::flatten(insert_batch_seq));
		auto membership_query_points =
			parlay::tabulate(kMemberQueryOsmSize, [&](size_t i) {
				return TT::Entry::get_key(
					random_osm_records[i]);
			});

		// NOTE: generate box
		// NOTE: count how often each source vertex occurs (its out-degree) and
		// build a (vertex, occurrence) sequence, highest occurrence first. The
		// top vertices are later used as the keys for the range queries.
		auto osm_records = parlay::flatten(insert_batch_seq);
		auto edge_list =
			parlay::tabulate(osm_records.size(), [&](size_t i) {
				auto key = TT::Entry::get_key(osm_records[i]);
				uint64_t u = key >> 32;
				uint64_t v = key & 0xffffffff;
				return std::make_pair(u, v);
			});
		auto range_query_vertex_occurrence = parlay::histogram_by_key(
			parlay::map(edge_list,
				    [](auto const &e) { return e.first; }));
		parlay::sort_inplace(range_query_vertex_occurrence,
				     [](auto const &a, auto const &b) {
					     return a.second > b.second;
				     });
		range_query_vertex_occurrence =
			range_query_vertex_occurrence.subseq(
				0, kDynamicGraphRangeNum);
		// WARN: may don't have enough vertices for range query
		auto [range_query_offset, tot_range_size] = parlay::scan(
			parlay::map(range_query_vertex_occurrence,
				    [](auto const &p) { return p.second; }),
			parlay::addm<size_t>());
		range_query_offset.push_back(tot_range_size);

		auto range_report_output =
			typename TT::entry_seq(tot_range_size);

		// NOTE: flatten
		auto flatten_seq =
			TT::entry_seq::uninitialized(kDynamicFlattenSize);

		TestDynamicGraph<TT, Tree>(
			insert_batch_seq, delete_batch_seq, tree, opts.rounds,
			membership_res, range_report_output, flatten_seq,
			membership_query_points, range_query_vertex_occurrence,
			range_query_offset);
	}

	if (opts.tag & (1 << 5)) { // NOTE: temporal graph randomness
		// string dynamic_graph_path =
		//   "/data/zmen002/geometry/osm_temporal_graph/north_america/";
		string dynamic_graph_path = opts.input_file_path;

		int const kDynamicGraphBatchNum =
			CountDynamicGraphBatches(dynamic_graph_path);
		std::vector<std::string> insert_files(kDynamicGraphBatchNum);
		std::vector<std::string> delete_files(kDynamicGraphBatchNum);
		for (int i = 0; i < kDynamicGraphBatchNum; i++) {
			insert_files[i] = dynamic_graph_path +
					  std::to_string(i) + "_insert.bin";
			delete_files[i] = dynamic_graph_path +
					  std::to_string(i) + "_delete.bin";
		}
		parlay::sequence<entry_seq> insert_batch_seq(
			kDynamicGraphBatchNum);
		parlay::sequence<entry_seq> delete_batch_seq(
			kDynamicGraphBatchNum);
		size_t total_entries = 0;
		for (int i = 0; i < kDynamicGraphBatchNum; i++) {
			auto [n1, dim1] =
				ReadEntries<typename TT::E, kInputType>(
					insert_files[i].c_str(),
					insert_batch_seq[i], total_entries);
			total_entries += n1;

			auto [n2, dim2] =
				ReadEntries<typename TT::E, kInputType>(
					delete_files[i].c_str(),
					delete_batch_seq[i], total_entries);
			total_entries += n2;
		}

		// membership query result
		auto membership_res =
			parlay::sequence<bool>(kMemberQueryOsmSize, true);
		auto const time_period_num = insert_batch_seq.size();

		const parlay::sequence<int> sort_ratio_seq = { 0,  10, 20, 30,
							       40, 50, 60, 70,
							       80, 90, 100 };
		puts("");
		for (auto ratio : sort_ratio_seq) {
			TestLeafRandomDynamicGraph<TT, Tree>(
				tree, insert_batch_seq, delete_batch_seq,
				membership_res, time_period_num, opts.rounds,
				ratio);
		}
	}

	// NOTE: temporal graph signal sliding window
	if (opts.tag & (1 << 6)) {
		string osm_path = opts.input_file_path;
		entry_seq osm_records;
		auto [input_num, input_dim] =
			ReadEntries<typename TT::E, kInputType>(
				osm_path.c_str(), osm_records);

		parlay::stable_sort_inplace(osm_records, [&](auto const &a,
							     auto const &b) {
			return TT::Entry::get_val(a) < TT::Entry::get_val(b);
		});
		auto max_timestamp = osm_records.back().val;
		auto timestamp_seq = parlay::tabulate(
			max_timestamp, [&](size_t i) { return i + 1; });
		const auto year_num = timestamp_seq.size();
		auto node_by_year = parlay::tabulate(year_num, [&](size_t i) {
			return parlay::filter(
				osm_records, [&](auto const &record) {
					return TT::Entry::get_val(record) ==
					       timestamp_seq[i];
				});
		});

		// NOTE: membership query stuff
		auto membership_res =
			parlay::sequence<bool>(kMemberQueryOsmSize, true);
		auto random_osm_records = parlay::random_shuffle(osm_records);
		auto membership_query_points =
			parlay::tabulate(kMemberQueryOsmSize, [&](size_t i) {
				return TT::Entry::get_key(
					random_osm_records[i]);
			});

		// NOTE: range query stuff
		// NOTE: count how often each source vertex occurs (its out-degree) and
		// build a (vertex, occurrence) sequence, highest occurrence first. The
		// top vertices are later used as the keys for the range queries.
		auto edge_list =
			parlay::tabulate(osm_records.size(), [&](size_t i) {
				auto key = TT::Entry::get_key(osm_records[i]);
				uint64_t u = key >> 32;
				uint64_t v = key & 0xffffffff;
				return std::make_pair(u, v);
			});
		auto range_query_vertex_occurrence = parlay::histogram_by_key(
			parlay::map(edge_list,
				    [](auto const &e) { return e.first; }));
		parlay::sort_inplace(range_query_vertex_occurrence,
				     [](auto const &a, auto const &b) {
					     return a.second > b.second;
				     });
		range_query_vertex_occurrence =
			range_query_vertex_occurrence.subseq(
				0, std::min(edge_list.size(),
					    kDynamicGraphRangeNum));
		// WARN: may don't have enough vertices for range query
		auto [range_query_offset, tot_range_size] = parlay::scan(
			parlay::map(range_query_vertex_occurrence,
				    [](auto const &p) { return p.second; }),
			parlay::addm<size_t>());
		range_query_offset.push_back(tot_range_size);

		auto range_report_output =
			typename TT::entry_seq(tot_range_size);

		// NOTE: flatten stuff
		size_t tree_size = 0;
		size_t max_tree_size = 0;
		for (int batch_id = 0; batch_id < node_by_year.size();
		     batch_id++) {
			tree_size += node_by_year[batch_id].size();
			if (batch_id >= kOsmWindowLen) {
				tree_size -=
					node_by_year[batch_id - kOsmWindowLen]
						.size();
			}
			max_tree_size = std::max(max_tree_size, tree_size);
		}
		auto flatten_seq = TT::entry_seq::uninitialized(max_tree_size);

		// NOTE: main function call
		TestTemporalSignal<TT>(node_by_year, tree, opts.rounds,
				       kOsmWindowLen, kMemberQueryOsmSize,
				       membership_res, range_report_output,
				       flatten_seq, membership_query_points,
				       range_query_vertex_occurrence,
				       range_query_offset);
	}
};

// NOTE: driven functions
template <typename TreeWrapper, typename RunFunc>
static void Run(Options const &opts, RunFunc test_func)
{
	using TT = typename TreeWrapper::TreeType::TT;
	using E [[maybe_unused]] = typename TT::E;
	using entry_seq [[maybe_unused]] = typename TT::entry_seq;

	PrintTreeParam(TreeWrapper::TreeType::get_tree_name(),
		       TreeWrapper::TreeType::get_leaf_size());
	opts.PrintOptions();
	std::cout << "\n" << std::flush;

	// Apply the test function
	test_func.template operator()<TreeWrapper, TT>(opts);
};

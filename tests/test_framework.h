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
#include "dependence/tree_node.h"
#include "dependence/type_trait.h"
#include "options.h"
#include "parlay/internal/group_by.h"
#include "parlay/monoid.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/random.h"
#include "parlay/slice.h"
#include "pb_tree/b_tree.h"
#include "wrapper.h"

#ifdef CCP
using Coord = long;
// using Coord = double;
#else
using Coord = long;
// using Coord = double;
#endif // CCP

using Typename = Coord;
using namespace pb_tree;

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
enum DeleteType
{
  kBatchDelete,
  kBatchDiff
};

// * [a,b)
inline size_t
GetRandomIndex(size_t a, size_t b)
{
  return size_t((rand() % (b - a)) + a);
}

template<typename TT, bool SavePoint, bool FixSize>
auto
GenRectangles(int rec_num, int const type, auto const& WP)
{
  using Entry = TT::Entry;
  using Box = typename TT::Box;
  using BoxSeq =
    std::conditional_t<SavePoint == false,
                       parlay::sequence<std::pair<Box, size_t>>,
                       parlay::sequence<std::pair<Box, typename TT::EntrySeq>>>;

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
      range.second = static_cast<size_t>(std::sqrt(std::sqrt(1.0 * n)));
    } else if (type == 1) { //* medium bracket
      range.second = static_cast<size_t>(std::sqrt(std::sqrt(1.0 * n)));
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
  BoxSeq box_seq(rec_num);
  int cnt = 0;

  srand(10);
  auto wp = parlay::sort(WP);
  assert(wp.size() == parlay::remove_duplicates_ordered(wp).size());
  size_t max_size = 0;

  for (int i = 0; i < rec_num; i++) {
    size_t len = GetRandomIndex(range.first, range.second);
    auto l = GetRandomIndex(0, wp.size() - len);
    auto r = l + len;
    max_size = std::max(max_size, len);
    auto lk = Entry::GetKey(wp[l]);
    auto rk = Entry::GetKey(wp[r]);
    if constexpr (SavePoint) {
      // NOTE: Left close and right open
      box_seq[i] = std::make_pair(Box(lk, rk), wp.subseq(l, r));
    } else {
      box_seq[i] = std::make_pair(Box(lk, rk), r - l);
    }
  }

  return std::make_pair(box_seq, max_size);
}

template<typename TypeTrait,
         typename Tree,
         bool kTestTime = true,
         int kPrint = 1>
void
BuildTree(typename TypeTrait::EntrySeq const& WP, int const& rounds, Tree& pkd)
{
  using TT = TypeTrait;
  using EntrySeq = typename TT::EntrySeq;
  using Leaf = typename Tree::Leaf;
  using Interior = typename Tree::Interior;

  double loop_late = rounds > 1 ? 0.01 : -100;
  size_t n = WP.size();
  // size_t n = 100;
  // EntrySeq wp = EntrySeq::uninitialized(n);
  auto const wp = Tree::ParseInput(WP);

  if constexpr (kTestTime) {
    pkd.DeleteTree();

    double aveBuild = time_loop(
      rounds,
      loop_late,
      [&]() {},
      [&]() { pkd.Build(wp); },
      [&]() { pkd.DeleteTree(); });

    pkd.Build(wp);
    if constexpr (kPrint) {
      std::cout << aveBuild << " " << std::flush;
    }
  } else {
    // NOTE: always return a built tree
    pkd.DeleteTree();
    pkd.Build(wp);
  }
}

template<typename TypeTrait,
         typename Tree,
         bool kTestTime = true,
         bool kSerial = false>
void
BatchInsert(Tree& pkd,
            typename TypeTrait::EntrySeq const& WP,
            typename TypeTrait::EntrySeq const& WI,
            int const rounds)
{
  auto const wp = Tree::ParseInput(WP);
  auto const wi = Tree::ParseInput(WI);
  double loop_late = rounds > 1 ? 0.01 : -100;

  if constexpr (kTestTime) { // NOTE: clean and measure time
    pkd.DeleteTree();
    double aveInsert = time_loop(
      rounds,
      loop_late,
      [&]() { pkd.Build(wp); },
      [&]() { pkd.BatchInsert(wi); },
      [&]() { pkd.DeleteTree(); });
    std::cout << aveInsert << " " << std::flush;
    // std::cout << ratio << " " << aveInsert << " " << std::endl;
    BuildTree<TypeTrait, Tree, false>(WP, rounds, pkd);
  } else { // NOTE: insert the points from previous tree
    std::cout << "Insert " << wi.size() << " elements\n" << std::flush;
    pkd.BatchInsert(wi);
    std::cout << "finish ccp insert" << std::endl;
  }
}

struct StepUpdateLogger
{
  int id;
  double t;

  friend std::ostream& operator<<(std::ostream& os, StepUpdateLogger const& log)
  {
    os << "(" << log.id << ", " << log.t << ")";
    return os;
  }
};

template<typename TT, typename Tree, bool kInsert>
void
BatchInsertByStep(Tree& pkd,
                  auto const& WP,
                  int const rounds,
                  double const insert_ratio)
{
  auto wp = WP;
  size_t n = wp.size();
  auto step = static_cast<size_t>(insert_ratio * n);
  size_t slice_num = (n / step) + (n % step != 0);
  auto insert_batch_seq = parlay::tabulate(slice_num, [&](size_t i) {
    return Tree::ParseInput(wp.subseq(i * step, std::min((i + 1) * step, n)));
  });

  pkd.DeleteTree();

  auto prepare_func = [&]() {
    insert_batch_seq = parlay::tabulate(slice_num, [&](size_t i) {
      return Tree::ParseInput(wp.subseq(i * step, std::min((i + 1) * step, n)));
    });
  };
  auto incre_build = [&]() {
    for (int i = 0; i < slice_num; i++) {
      pkd.BatchInsert(insert_batch_seq[i]);
    }
  };

  double ave_time = time_loop(
    rounds,
    0.01,
    [&]() { prepare_func(); },
    [&]() { incre_build(); },
    [&]() { pkd.DeleteTree(); });

  std::cout << ave_time << " " << std::flush;

  // NOTE: restore status
  prepare_func();
  incre_build();
}

struct PBTreeLeafSortPred
{
  PBTreeLeafSortPred() = default;

  PBTreeLeafSortPred(int ratio)
    : _randomness(ratio)
  {
  }

  template<typename InputNode, typename Range>
  bool operator()(InputNode const* T, Range In) const noexcept
  {
    auto const v = (static_cast<size_t>(T->size) + In.size()) * 2654435761ULL ^
                   (reinterpret_cast<uintptr_t>(In.begin()) >> 3) ^
                   (reinterpret_cast<uintptr_t>(In.end()) >> 3);

    // - _randomness = 0: 0–99 ≥ 0 → always true → always sort
    // - _randomness = 100: 0–99 ≥ 100 → always false → never sort
    return (v % 100) >= this->_randomness;
  }

  size_t _randomness = 50;
};

template<typename TypeTrait, typename Tree>
  requires(!pb_tree::IsPBTree<Tree>)
void
TestLeafRandom(Tree&,
               auto const&,
               auto const&,
               auto const&,
               int const,
               int const,
               int const)
{
}

template<typename TypeTrait, typename Tree>
  requires pb_tree::IsPBTree<Tree>
void
TestLeafRandom(Tree& pkd,
               auto const& WP,
               auto const& insert_batch_seq,
               auto const& delete_batch_seq,
               int const time_period_num,
               int const rounds,
               int const ratio)
{
  using TT = TypeTrait;
  auto const wp = Tree::ParseInput(WP);

  auto const query_size = static_cast<size_t>(wp.size() * kMemberQueryRatio);
  auto query_ele = wp.subseq(0, query_size);
  auto query_ele_step = query_size / time_period_num;
  for (int i = 0; i < time_period_num; i++) {
    parlay::parallel_for(0, query_ele_step, [&](size_t j) {
      query_ele[(i * query_ele_step) + j] = insert_batch_seq[i][j];
    });
  }
  auto member_res = parlay::sequence<bool>(query_ele.size(), true);

  constexpr auto leaf_entries_pattern = TypeTrait::LeafEntriesPattern::kHybrid;
  constexpr bool sorted_input = false;
  parlay::internal::timer t;

  parlay::sequence<double> insert_time(time_period_num, 0.0);
  parlay::sequence<double> delete_time(time_period_num, 0.0);
  double member_query_time = 0.0;
  double sorted_leaf_ratio = 0.0;

  for (int r = 0; r < rounds; r++) {
    pkd.DeleteTree();
    pkd.Build(wp);

    for (int i = 0; i < time_period_num; i++) {
      t.reset(), t.start();
      pkd.BatchDelete(delete_batch_seq[i],
                      leaf_entries_pattern,
                      sorted_input,
                      PBTreeLeafSortPred(ratio));
      t.stop();
      delete_time[i] += t.total_time();

      t.reset(), t.start();
      pkd.BatchInsert(insert_batch_seq[i],
                      leaf_entries_pattern,
                      sorted_input,
                      PBTreeLeafSortPred(ratio));
      t.stop();
      insert_time[i] += t.total_time();
    }

    // sorted-leaf ratio (measured after all updates, before membership)
    sorted_leaf_ratio += pkd.SortedLeafRatio();

    // membership
    t.reset(), t.start();
    parlay::parallel_for(0, query_size, [&](size_t j) {
      auto [contain, logger] = pkd.Contain(Tree::Entry::GetKey(query_ele[j]));
      member_res[j] = contain;
    });
    t.stop();
    member_query_time += t.total_time();
  }

  auto insert_ave = parlay::reduce(insert_time) / (rounds * time_period_num);
  auto delete_ave = parlay::reduce(delete_time) / (rounds * time_period_num);
  auto member_query_ave = member_query_time / rounds;
  auto sorted_leaf_ratio_ave = sorted_leaf_ratio / rounds;

  std::cout << ratio << " " << insert_ave << " " << delete_ave << " "
            << member_query_ave << " " << sorted_leaf_ratio_ave << std::endl;
}

template<typename TypeTrait, typename Tree>
  requires(!pb_tree::IsPBTree<Tree>)
void
TestLeafRandomDynamicGraph(Tree&,
                           auto const&,
                           auto const&,
                           auto&,
                           int const,
                           int const,
                           int const)
{
}

template<typename TypeTrait, typename Tree>
  requires pb_tree::IsPBTree<Tree>
void
TestLeafRandomDynamicGraph(Tree& pkd,
                           auto const& insert_batch_seq,
                           auto const& delete_batch_seq,
                           auto& membership_res,
                           int const time_period_num,
                           int const rounds,
                           int const ratio)
{
  using TT = TypeTrait;
  pkd.DeleteTree();
  parlay::sequence<typename Tree::InputRangeT> wi(time_period_num);
  parlay::sequence<typename Tree::InputDeleteRangeT> wd(time_period_num);
  parlay::sequence<typename Tree::InputRangeT> insert_entry(time_period_num);
  parlay::sequence<typename Tree::InputDeleteRangeT> delete_entry(
    time_period_num);
  const size_t member_query_n = membership_res.size();
  auto random_osm_records =
    parlay::random_shuffle(parlay::flatten(insert_batch_seq));
  auto query_points = parlay::tabulate(member_query_n, [&](size_t i) {
    return TT::Entry::GetKey(random_osm_records[i]);
  });

  // NOTE: generate box (same as TestDynamicGraph): count how often each
  // source vertex occurs (its out-degree) and build a (vertex, occurrence)
  // sequence, highest occurrence first. The top vertices are used as the
  // keys for the range queries.
  auto edge_list = parlay::tabulate(random_osm_records.size(), [&](size_t i) {
    auto key = TT::Entry::GetKey(random_osm_records[i]);
    uint64_t u = key >> 32;
    uint64_t v = key & 0xffffffff;
    return std::make_pair(u, v);
  });
  auto range_query_vertex_occurrence = parlay::histogram_by_key(
    parlay::map(edge_list, [](auto const& e) { return e.first; }));
  parlay::sort_inplace(
    range_query_vertex_occurrence,
    [](auto const& a, auto const& b) { return a.second > b.second; });
  range_query_vertex_occurrence = range_query_vertex_occurrence.subseq(
    0,
    std::min(range_query_vertex_occurrence.size(),
             static_cast<size_t>(kDynamicGraphRangeNum)));
  auto [range_query_offset, tot_range_size] =
    parlay::scan(parlay::map(range_query_vertex_occurrence,
                             [](auto const& p) { return p.second; }),
                 parlay::addm<size_t>());
  range_query_offset.push_back(tot_range_size);
  auto range_report_output = typename TT::EntrySeq(tot_range_size);

  auto box_seq =
    parlay::tabulate(range_query_vertex_occurrence.size(), [&](size_t i) {
      uint64_t u = range_query_vertex_occurrence[i].first;
      return std::make_pair(static_cast<uint64_t>(u << 32),
                            static_cast<uint64_t>((u + 1) << 32));
    });
  parlay::sequence<size_t> size_seq(box_seq.size(), 0);

  for (int i = 0; i < time_period_num; i++) {
    wi[i] = Tree::ParseInput(insert_batch_seq[i]);
    wd[i] = Tree::ParseDeleteInput(delete_batch_seq[i]);
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
    auto const& insert_entry = wi;
    auto const& delete_entry = wd;
    if (rounds == 1) {
      for (int batch_id = 0; batch_id < time_period_num; batch_id++) {
        insert_time[batch_id] = 0.0;
        delete_time[batch_id] = 0.0;
        member_query_time[batch_id] = 0.0;
      }
    }

    // running the osm
    constexpr auto leaf_entries_pattern = TT::LeafEntriesPattern::kHybrid;
    constexpr bool sorted_input = false;
    parlay::internal::timer t;
    pkd.DeleteTree();
    for (int batch_id = 0; batch_id < time_period_num; batch_id++) {
      t.reset(), t.start();
      pkd.BatchInsert(insert_entry[batch_id],
                      leaf_entries_pattern,
                      sorted_input,
                      PBTreeLeafSortPred(ratio));
      t.stop();
      insert_time[batch_id] = t.total_time();

      t.reset(), t.start();
      pkd.BatchDelete(delete_entry[batch_id],
                      leaf_entries_pattern,
                      sorted_input,
                      PBTreeLeafSortPred(ratio));
      t.stop();
      delete_time[batch_id] = t.total_time();

      // NOTE: test membership query
      t.reset(), t.start();
      parlay::parallel_for(0, member_query_n, [&](size_t j) {
        auto [contain, logger] = pkd.Contain(query_points[j]);
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
      sorted_leaf_ratio_ave += pkd.SortedLeafRatio();
    }
  }

  // average of the per-round total times over counted rounds
  double const denom = rounds > 1 ? (rounds - 1) : 1;
  double const insert_ave = insert_sum / denom;
  double const delete_ave = delete_sum / denom;
  double const member_query_ave = member_query_sum / denom;
  sorted_leaf_ratio_ave /= denom;

  std::cout << ratio << " " << insert_ave << " " << delete_ave << " "
            << member_query_ave << " " << sorted_leaf_ratio_ave << std::endl;
}

template<typename TypeTrait,
         typename Tree,
         bool kTestTime = true,
         bool kSerial = false>
void
BatchDelete(Tree& pkd,
            typename TypeTrait::EntrySeq const& WP,
            typename TypeTrait::EntrySeq const& WI,
            int const rounds)
{
  auto const wp = Tree::ParseInput(WP);
  auto const wi = Tree::ParseDeleteInput(WI);
  double loop_late = rounds > 1 ? 0.01 : -100;

  if constexpr (kTestTime) { // NOTE: clean and measure time
    pkd.DeleteTree();
    double aveInsert = time_loop(
      rounds,
      loop_late,
      [&]() { pkd.Build(wp); },
      [&]() { pkd.BatchDelete(wi); },
      [&]() { pkd.DeleteTree(); });
    std::cout << aveInsert << " " << std::flush;
    // std::cout << ratio << " " << aveInsert << " " << std::endl;
    BuildTree<TypeTrait, Tree, false>(WP, rounds, pkd);
  } else { // NOTE: delete the points from previous tree
    std::cout << "begin ccp delete" << std::endl;
    pkd.BatchDelete(wi);
    std::cout << "finish ccp delete" << std::endl;
  }
}

template<typename TT, typename Tree>
void
RangeCount(auto const& wp,
           Tree const& pkd,
           int const kRounds,
           int const rec_num,
           int const rec_type)
{
  using EntrySeq = typename TT::EntrySeq;

  auto [query_box_seq, max_size] =
    GenRectangles<TT, false, false>(rec_num, rec_type, wp);
  parlay::sequence<size_t> kdknn(rec_num, 0);

  double ave_query = time_loop(
    kRounds,
    0.01,
    [&]() {},
    [&]() {
      parlay::parallel_for(0, rec_num, [&](size_t i) {
        // parlay::parallel_for(0, 1, [&](size_t i) {
        auto [size, logger] = pkd.RangeCount(query_box_seq[i].first);
        kdknn[i] = size;
        int output_size = query_box_seq[i].second;
        assert(std::cmp_equal(kdknn[i], output_size));
      });
    },
    [&]() {});

  std::cout << "check range count: " << rec_num << " " << rec_type << " "
            << max_size << std::endl;
  for (int i = 0; i < rec_num; i++) {
    int output_size = query_box_seq[i].second;
    if (kdknn[i] != output_size) {
      std::cout << kdknn[i] << " " << output_size << " "
                << query_box_seq[i].first.first << query_box_seq[i].first.second
                << std::endl;
    }
    assert(std::cmp_equal(kdknn[i], output_size));
  }

  std::cout << ave_query << "\n" << std::flush;
  return;
}
// NOTE: run range query and check the correct
template<typename TT, typename Tree>
void
RangeQuery(auto const& wp,
           Tree const& pkd,
           int const kRounds,
           int const rec_num,
           int const rec_type)
{
  using EntrySeq = typename TT::EntrySeq;

  auto [query_box_seq, max_size] =
    GenRectangles<TT, true, false>(rec_num, rec_type, wp);
  auto [offset, tot_size] = parlay::scan(
    parlay::delayed_tabulate(
      rec_num,
      [&](size_t i) -> size_t { return query_box_seq[i].second.size(); }),
    parlay::addm<size_t>());
  offset.push_back(tot_size);
  EntrySeq Out(tot_size);
  parlay::sequence<size_t> kdknn(rec_num, 0);

  double ave_query = time_loop(
    kRounds,
    0.01,
    [&]() {},
    [&]() {
      parlay::parallel_for(0, rec_num, [&](size_t i) {
        // parlay::parallel_for(0, 1, [&](size_t i) {
        auto [size, logger] = pkd.RangeReport(
          query_box_seq[i].first, Out.cut(offset[i], offset[i + 1]));
        kdknn[i] = size;
        int output_size = query_box_seq[i].second.size();
        assert(std::cmp_equal(kdknn[i], size) && Out.size() >= output_size);
      });
    },
    [&]() {});

  std::cout << "check range query: " << rec_num << " " << rec_type << " "
            << max_size << std::endl;
  for (int i = 0; i < rec_num; i++) {
    int output_size = query_box_seq[i].second.size();
    if (kdknn[i] != output_size) {
      std::cout << kdknn[i] << " " << output_size << " "
                << query_box_seq[i].first.first << query_box_seq[i].first.second
                << std::endl;
    }
    assert(std::cmp_equal(kdknn[i], output_size));
    // std::cout << kdknn[i] << " " << output_size << " "
    //     << query_box_seq[i].first.first << query_box_seq[i].first.second
    //     << std::endl;
    parlay::sort_inplace(Out.cut(offset[i], offset[i + 1]),
                         [&](auto const& a, auto const& b) {
                           return TT::Entry::GetKey(a) < TT::Entry::GetKey(b);
                         });
    parlay::sort_inplace(query_box_seq[i].second,
                         [&](auto const& a, auto const& b) {
                           return TT::Entry::GetKey(a) < TT::Entry::GetKey(b);
                         });
    parlay::parallel_for(0, output_size, [&](size_t j) {
      if (Out[offset[i] + j] != query_box_seq[i].second.at(j)) {
        std::cout << "wrong: bounding box: " << query_box_seq[i].first.first
                  << " " << query_box_seq[i].first.second << std::endl;
        std::cout << "------ " << Out[offset[i] + j] << " "
                  << query_box_seq[i].second.at(j) << std::endl;
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
template<typename TT, typename Tree>
void
Membership(Tree const& pkd, auto const& query_ele, int kRounds, auto& res)
{
  using Entry = typename TT::Entry;
  int query_num = query_ele.size();
  parlay::sequence<size_t> vis_leaf(query_num);
  parlay::sequence<size_t> vis_inter(query_num);

  double loop_late = kRounds > 1 ? 0.01 : -100;

  double ave_query = time_loop(
    kRounds,
    loop_late,
    [&]() {},
    [&]() {
      parlay::parallel_for(
        0,
        query_num,
        [&](size_t i) {
          // for (int i = 0; i < query_num; i++) {
          auto [contain, logger] = pkd.Contain(Entry::GetKey(query_ele[i]));

          res[i] = contain;
          // vis_leaf[i] = logger.vis_leaf_num;
          // vis_inter[i] = logger.vis_interior_num;
          // }
        },
        1);
    },
    [&]() {});

  // std::cout << query_num << " ";
  std::cout << ave_query << " " << std::flush;
}

template<typename TT, typename Tree>
void
RangeCountFix(Tree& pkd,
              int const& rounds,
              int rec_num,
              auto const& query_box_seq)
{
  // auto [query_box_seq, max_size] =
  //     gen_rectangles<Point, Tree, false>(rec_num, rec_type, WP, DIM);
  parlay::sequence<size_t> vis_leaf(rec_num), vis_inter(rec_num);

  // int n = WP.size();
  // size_t step = Out.size() / rec_num;
  // using ref_t = std::reference_wrapper<Point>;
  // parlay::sequence<ref_t> out_ref( Out.size(), std::ref( Out[0] ) );

  double ave_query = time_loop(
    rounds,
    0.01,
    [&]() {},
    [&]() {
      parlay::parallel_for(0, rec_num, [&](size_t i) {
        // for (int i = 0; i < rec_num; i++) {
        auto [size, logger] = pkd.RangeCount(query_box_seq[i].first);
        vis_leaf[i] = logger.vis_leaf_num;
        vis_inter[i] = logger.vis_interior_num;
      });
    },
    [&]() {});

  std::cout << ave_query << " " << std::flush;
  return;
}

template<typename TT, typename Tree>
void
RangeQueryFix(Tree& pkd,
              int const& rounds,
              auto& Out,
              int rec_num,
              auto const& query_box_seq)
{
  // auto [query_box_seq, max_size] =
  //     gen_rectangles<Point, Tree, false>(rec_num, rec_type, WP, DIM);
  parlay::sequence<size_t> vis_leaf(rec_num), vis_inter(rec_num);

  auto [offset, tot_size] = parlay::scan(
    parlay::tabulate(
      rec_num, [&](size_t i) -> size_t { return query_box_seq[i].second; }),
    parlay::addm<size_t>());
  offset.push_back(tot_size);
  Out.resize(tot_size);

  // int n = WP.size();
  // size_t step = Out.size() / rec_num;
  // using ref_t = std::reference_wrapper<Point>;
  // parlay::sequence<ref_t> out_ref( Out.size(), std::ref( Out[0] ) );

  double ave_query = time_loop(
    rounds,
    0.01,
    [&]() {},
    [&]() {
      parlay::parallel_for(0, rec_num, [&](size_t i) {
        // for (int i = 0; i < rec_num; i++) {
        auto [size, logger] = pkd.RangeReport(
          query_box_seq[i].first, Out.cut(offset[i], offset[i + 1]));
        vis_leaf[i] = logger.vis_leaf_num;
        vis_inter[i] = logger.vis_interior_num;
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

template<typename TT, typename Tree>
void
RangeQuerySerialWithLog(Tree& pkd,
                        int const& rounds,
                        auto& Out,
                        const int rec_num,
                        const int rec_type,
                        auto const& query_box_seq)
{
  auto [offset, tot_size] = parlay::scan(
    parlay::delayed_tabulate(
      rec_num, [&](size_t i) -> size_t { return query_box_seq[i].second; }),
    parlay::addm<size_t>());
  offset.push_back(tot_size);
  Out.resize(tot_size);

  for (int i = 0; i < rec_num; i++) {
    parlay::internal::timer t;
    t.reset(), t.start();
    auto [size, logger] = pkd.RangeReport(query_box_seq[i].first,
                                          Out.cut(offset[i], offset[i + 1]));
    t.stop();
    std::cout << rec_type << " " << query_box_seq[i].second << " "
              << std::scientific << t.total_time() << std::endl;
  }

  return;
}

template<typename TT, typename Tree>
void
InsertOsmByTime(const parlay::sequence<typename TT::EntrySeq>& node_by_time,
                Tree& pkd,
                const int rounds,
                const int sliding_window_len,
                const size_t member_query_n,
                auto& member_res,
                auto& range_res)
{
  pkd.DeleteTree();
  const int time_period_num = node_by_time.size();
  parlay::sequence<typename Tree::InputRangeT> wi(time_period_num);
  parlay::sequence<typename Tree::InputDeleteRangeT> wd(time_period_num);
  parlay::sequence<typename Tree::InputRangeT> insert_entry(time_period_num);
  parlay::sequence<typename Tree::InputDeleteRangeT> delete_entry(
    time_period_num);
  auto query_points = TT::KeySeq::uninitialized(member_query_n);
  auto step = member_query_n / time_period_num;
  if (member_query_n % time_period_num != 0) {
    std::cout << "the member query num should be divisible by time period num"
              << std::endl;
    return;
  }

  for (int i = 0; i < time_period_num; i++) {
    wi[i] = Tree::ParseInput(node_by_time[i]);
    wd[i] = Tree::ParseDeleteInput(node_by_time[i]);

    if (node_by_time[i].size() < step) {
      std::cout << "not enough points in time period " << i
                << " for member query, need " << step << std::endl;
      return;
    }
    parlay::parallel_for(0, step, [&](size_t j) {
      query_points[(i * step) + j] = TT::Entry::GetKey(node_by_time[i][j]);
    });
  }

  // generate the range query box
  parlay::sequence<std::pair<int, int>> batch_in_tree;
  for (int i = 0; i < time_period_num; i++) {
    if (i < sliding_window_len) {
      batch_in_tree.emplace_back(0, i + 1);
    } else {
      batch_in_tree.emplace_back(i - sliding_window_len, i + 1);
    }
  }
  auto box_seq_by_year =
    parlay::tabulate(time_period_num, [&](size_t batch_id) {
      bool constexpr kSavePoint = false;
      bool constexpr kFixSize = false;
      auto wp = parlay::flatten(node_by_time.cut(
        batch_in_tree[batch_id].first, batch_in_tree[batch_id].second));
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
    auto const& insert_entry = wi;
    auto const& delete_entry = wd;

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
    pkd.DeleteTree();
    for (int batch_id = 0; batch_id < time_period_num; batch_id++) {
      t.reset(), t.start();
      pkd.BatchInsert(insert_entry[batch_id]);
      t.stop();
      insert_time[batch_id] += t.total_time();
      current_tree_size += node_by_time[batch_id].size();
#ifdef DEBUG
      std::cerr << " insert batch: " << batch_id << std::flush;
      pkd.Validate();
#endif // DEBUG

      if (batch_id >= sliding_window_len) {
        t.reset(), t.start();
        pkd.BatchDelete(delete_entry[batch_id - sliding_window_len]);
        t.stop();
        delete_time[batch_id] += t.total_time();
        current_tree_size -= node_by_time[batch_id - sliding_window_len].size();
#ifdef DEBUG
        // std::cerr << " delete batch: " << batch_id - sliding_window_len
        //           << std::flush;
        pkd.Validate();
#endif // DEBUG
      } else {
        delete_time[batch_id] = 0.0;
      }
      tree_size[batch_id] = current_tree_size;

      // NOTE: test membership query
      t.reset(), t.start();
      parlay::parallel_for(0, member_query_n, [&](size_t j) {
        auto [contain, logger] = pkd.Contain(query_points[j]);
        member_res[j] = contain;
      });
      t.stop();
      member_query_time[batch_id] += t.total_time();

      // NOTE: range query
      auto query_box_seq = box_seq_by_year[batch_id].first;
      auto [offset, tot_size] =
        parlay::scan(parlay::tabulate(kOSMRangeQueryNum,
                                      [&](size_t i) -> size_t {
                                        return query_box_seq[i].second;
                                      }),
                     parlay::addm<size_t>());
      offset.push_back(tot_size);
      range_res.resize(tot_size);

      t.reset(), t.start();
      parlay::parallel_for(0, kOSMRangeQueryNum, [&](size_t i) {
        auto [size, logger] = pkd.RangeReport(
          query_box_seq[i].first, range_res.cut(offset[i], offset[i + 1]));
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
      batch_id >= sliding_window_len
        ? node_by_time[batch_id - sliding_window_len].size()
        : 0;
    std::cout << std::right << " " << node_by_time[batch_id].size() << " "
              << delete_n << " " << tree_size[batch_id] << " " << std::fixed
              << std::setprecision(6) << insert_time[batch_id] / (rounds - 1)
              << " " << delete_time[batch_id] / (rounds - 1) << " "
              << member_query_time[batch_id] / (rounds - 1) << " "
              << range_query_time[batch_id] / (rounds - 1) << std::endl;
  }
}

// NOTE: geometric mean of a numeric range. Entries equal to 0 are omitted
// entirely (not clamped) so empty/idle batches do not drag the mean toward
// zero or trip log(0).
template<typename Range>
double
GeometricMean(Range const& v)
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
template<typename Range>
double
Total(Range const& v)
{
  return parlay::reduce(
    parlay::delayed_map(v, [](auto x) { return static_cast<double>(x); }));
}

template<typename TT, typename Tree>
void
TestTemporalSignal(const parlay::sequence<typename TT::EntrySeq>& node_by_time,
                   Tree& pkd,
                   const int rounds,
                   const int sliding_window_len,
                   const size_t member_query_n,
                   auto& membership_res,
                   auto& range_report_res,
                   auto& flatten_seq,
                   auto const& membership_query_keys,
                   auto const& range_query_vertex_occurrence,
                   auto const& range_query_offset)
{
  pkd.DeleteTree();
  const int time_period_num = node_by_time.size();
  parlay::sequence<typename Tree::InputRangeT> wi(time_period_num);
  parlay::sequence<typename Tree::InputDeleteRangeT> wd(time_period_num);
  parlay::sequence<typename Tree::InputRangeT> insert_entry(time_period_num);
  parlay::sequence<typename Tree::InputDeleteRangeT> delete_entry(
    time_period_num);

  for (int i = 0; i < time_period_num; i++) {
    // NOTE: parse input
    wi[i] = Tree::ParseInput(node_by_time[i]);
    wd[i] = Tree::ParseDeleteInput(node_by_time[i]);
  }

  // NOTE: generate the range query box
  auto const range_query_num = range_query_vertex_occurrence.size();
  auto range_query_box_seq = parlay::tabulate(range_query_num, [&](size_t i) {
    uint64_t u = range_query_vertex_occurrence[i].first;
    return std::make_pair(static_cast<uint64_t>(u << 32),
                          static_cast<uint64_t>((u + 1) << 32));
  });
  parlay::sequence<size_t> range_query_output_size(range_query_box_seq.size(),
                                                   0);

  // std::cout << "begin insert osm by time" << std::endl;

  parlay::sequence<double> insert_time(time_period_num, 0.0);
  parlay::sequence<double> delete_time(time_period_num, 0.0);
  parlay::sequence<double> member_query_time(time_period_num, 0.0);
  parlay::sequence<double> range_query_time(time_period_num, 0.0);
  parlay::sequence<double> flatten_time(time_period_num, 0.0);
  parlay::sequence<size_t> tree_size(time_period_num, 0);

  // puts("");
  for (int r = 0; r < rounds; r++) {
    auto const& insert_entry = wi;
    auto const& delete_entry = wd;

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
    pkd.DeleteTree();
    for (int batch_id = 0; batch_id < time_period_num; batch_id++) {
      t.reset(), t.start();
      pkd.BatchInsert(insert_entry[batch_id]);
      t.stop();
      insert_time[batch_id] += t.total_time();
      current_tree_size += node_by_time[batch_id].size();
#ifdef DEBUG
      std::cerr << " insert batch: " << batch_id << std::flush;
      pkd.Validate();
#endif // DEBUG

      if (batch_id >= sliding_window_len) {
        t.reset(), t.start();
        pkd.BatchDelete(delete_entry[batch_id - sliding_window_len]);
        t.stop();
        delete_time[batch_id] += t.total_time();
        current_tree_size -= node_by_time[batch_id - sliding_window_len].size();
#ifdef DEBUG
        // std::cerr << " delete batch: " << batch_id - sliding_window_len
        //           << std::flush;
        pkd.Validate();
#endif // DEBUG
      } else {
        delete_time[batch_id] = 0.0;
      }
      tree_size[batch_id] = current_tree_size;
      // std::cout << "tree size: " << current_tree_size << std::endl;

      // NOTE: test membership query
      t.reset(), t.start();
      parlay::parallel_for(0, member_query_n, [&](size_t j) {
        auto [contain, logger] = pkd.Contain(membership_query_keys[j]);
        membership_res[j] = contain;
      });
      t.stop();
      // std::cout << "batch_id: " << batch_id
      //           << " membership query time: " << t.total_time() << std::endl;
      member_query_time[batch_id] += t.total_time();

      // NOTE: range query — probe the top-occurrence (highest out-degree)
      // source vertices selected from the sorted edge list.
      t.reset(), t.start();
      parlay::parallel_for(0, range_query_box_seq.size(), [&](size_t j) {
        auto [size, logger] =
          pkd.RangeReport(range_query_box_seq[j],
                          range_report_res.cut(range_query_offset[j],
                                               range_query_offset[j + 1]));
        range_query_output_size[j] = size;
      });
      // for (size_t j = 0; j < range_query_box_seq.size(); j++) {
      //   auto [size, logger] =
      //     pkd.RangeReport(range_query_box_seq[j],
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
      pkd.GetEntries(flatten_seq);
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
      batch_id >= sliding_window_len
        ? node_by_time[batch_id - sliding_window_len].size()
        : 0;
    std::cout << std::right << " " << node_by_time[batch_id].size() << " "
              << delete_n << " " << tree_size[batch_id] << " " << std::fixed
              << std::setprecision(6) << insert_time[batch_id] / (rounds - 1)
              << " " << delete_time[batch_id] / (rounds - 1) << " "
              << member_query_time[batch_id] / (rounds - 1) << " "
              << range_query_time[batch_id] / (rounds - 1) << " "
              << flatten_time[batch_id] / (rounds - 1) << std::endl;
  }

  // NOTE: geometric mean of every attribute across all batches
  auto const geo_col = [&](auto&& f) {
    return GeometricMean(
      parlay::tabulate(time_period_num, [&](size_t batch_id) {
        return static_cast<double>(f(batch_id));
      }));
  };
  std::cout << std::right << "> " << geo_col([&](size_t batch_id) {
    return node_by_time[batch_id].size();
  }) << " " << geo_col([&](size_t batch_id) {
    return batch_id >= sliding_window_len
             ? node_by_time[batch_id - sliding_window_len].size()
             : size_t{ 0 };
  }) << " " << geo_col([&](size_t batch_id) { return tree_size[batch_id]; })
            << " " << std::fixed << std::setprecision(6)
            << geo_col([&](size_t batch_id) {
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
  auto const sum_col = [&](auto&& f) {
    return Total(parlay::tabulate(time_period_num, [&](size_t batch_id) {
      return static_cast<double>(f(batch_id));
    }));
  };
  std::cout << std::right << "# " << sum_col([&](size_t batch_id) {
    return node_by_time[batch_id].size();
  }) << " " << sum_col([&](size_t batch_id) {
    return batch_id >= sliding_window_len
             ? node_by_time[batch_id - sliding_window_len].size()
             : size_t{ 0 };
  }) << " " << sum_col([&](size_t batch_id) { return tree_size[batch_id]; })
            << " " << std::fixed << std::setprecision(6)
            << sum_col([&](size_t batch_id) {
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

template<typename TT, typename Tree>
void
TestDynamicGraph(const auto& insert_batch_seq,
                 const auto& delete_batch_seq,
                 Tree& pkd,
                 const int rounds,
                 auto& membership_res,
                 auto& range_report_output,
                 auto& flatten_seq,
                 auto const& membership_query_keys,
                 auto const& range_query_vertex_occurrence,
                 auto const& range_query_offset)
{
  pkd.DeleteTree();
  auto time_period_num = insert_batch_seq.size();
  parlay::sequence<typename Tree::InputRangeT> wi(time_period_num);
  parlay::sequence<typename Tree::InputDeleteRangeT> wd(time_period_num);
  parlay::sequence<typename Tree::InputRangeT> insert_entry(time_period_num);
  parlay::sequence<typename Tree::InputDeleteRangeT> delete_entry(
    time_period_num);
  const size_t member_query_n = membership_res.size();

  for (int i = 0; i < time_period_num; i++) {
    wi[i] = Tree::ParseInput(insert_batch_seq[i]);
    wd[i] = Tree::ParseDeleteInput(delete_batch_seq[i]);
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
    auto const& insert_entry = wi;
    auto const& delete_entry = wd;

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
    pkd.DeleteTree();
    for (int batch_id = 0; batch_id < time_period_num; batch_id++) {
      // std::cout << node_by_time[i].size() << " ";
      t.reset(), t.start();
      pkd.BatchInsert(insert_entry[batch_id]);
      t.stop();
      insert_time[batch_id] += t.total_time();
      current_tree_size += insert_batch_seq[batch_id].size();

#ifdef DEBUG
      std::cerr << " insert batch: " << batch_id << std::flush;
      pkd.Validate();
#endif // DEBUG

      t.reset(), t.start();
      pkd.BatchDelete(delete_entry[batch_id]);
      t.stop();
      delete_time[batch_id] += t.total_time();
      current_tree_size -= delete_batch_seq[batch_id].size();

#ifdef DEBUG
      std::cerr << " delete batch: " << batch_id << std::flush;
      pkd.Validate();
#endif // DEBUG

      tree_size[batch_id] = current_tree_size;

      // NOTE: test membership query
      t.reset(), t.start();
      parlay::parallel_for(0, member_query_n, [&](size_t j) {
        auto [contain, logger] = pkd.Contain(membership_query_keys[j]);
        membership_res[j] = contain;
      });
      t.stop();
      member_query_time[batch_id] += t.total_time();

      // NOTE: range report query
      t.reset(), t.start();
      parlay::parallel_for(0, box_seq.size(), [&](size_t j) {
        auto [size, logger] =
          pkd.RangeReport(box_seq[j],
                          range_report_output.cut(range_query_offset[j],
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
      pkd.GetEntries(flatten_seq);
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
    std::cout << std::right << " " << insert_batch_seq[batch_id].size() << " "
              << delete_batch_seq[batch_id].size() << " " << tree_size[batch_id]
              << " " << std::fixed << std::setprecision(6)
              << insert_time[batch_id] / (rounds - 1) << " "
              << delete_time[batch_id] / (rounds - 1) << " "
              << member_query_time[batch_id] / (rounds - 1) << " "
              << range_query_time[batch_id] / (rounds - 1) << " "
              << flatten_time[batch_id] / (rounds - 1) << std::endl;
  }

  // NOTE: geometric mean of every attribute across all batches
  auto const geo_col = [&](auto&& f) {
    return GeometricMean(
      parlay::tabulate(time_period_num, [&](size_t batch_id) {
        return static_cast<double>(f(batch_id));
      }));
  };
  std::cout << std::right << "> " << geo_col([&](size_t batch_id) {
    return insert_batch_seq[batch_id].size();
  }) << " " << geo_col([&](size_t batch_id) {
    return delete_batch_seq[batch_id].size();
  }) << " " << geo_col([&](size_t batch_id) { return tree_size[batch_id]; })
            << " " << std::fixed << std::setprecision(6)
            << geo_col([&](size_t batch_id) {
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
  auto const sum_col = [&](auto&& f) {
    return Total(parlay::tabulate(time_period_num, [&](size_t batch_id) {
      return static_cast<double>(f(batch_id));
    }));
  };
  std::cout << std::right << "# " << sum_col([&](size_t batch_id) {
    return insert_batch_seq[batch_id].size();
  }) << " " << sum_col([&](size_t batch_id) {
    return delete_batch_seq[batch_id].size();
  }) << " " << sum_col([&](size_t batch_id) { return tree_size[batch_id]; })
            << " " << std::fixed << std::setprecision(6)
            << sum_col([&](size_t batch_id) {
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

template<typename E, int kInputType = 0>
auto
ReadEntriesText(char const* iFile, parlay::sequence<E>& wp, size_t offset = 0)
  -> std::pair<size_t, int>
{
  using K = E::K;
  using V = E::V;

  parlay::sequence<char> S = readStringFromFile(iFile);
  parlay::sequence<char*> W = stringToWords(S);
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
template<typename E, int kInputType = 0>
auto
ReadEntriesBinary(char const* iFile, parlay::sequence<E>& wp, size_t offset = 0)
  -> std::pair<size_t, int>
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
template<typename E, int kInputType = 0>
auto
ReadEntries(char const* iFile, parlay::sequence<E>& wp, size_t offset = 0)
  -> std::pair<size_t, int>
{
  std::string_view const path(iFile);
  auto const res = path.ends_with(".bin")
                     ? ReadEntriesBinary<E, kInputType>(iFile, wp, offset)
                     : ReadEntriesText<E, kInputType>(iFile, wp, offset);

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
    wp, [&](auto const& a, auto const& b) { return a.key < b.key; });
  if (dedup.size() != wp.size()) {
    std::cout << "Warning: input contains " << (wp.size() - dedup.size())
              << " duplicate keys; only the first occurrence is kept."
              << std::endl;
  }
  return res;
}

template<typename E>
auto
ReadPointsSerial(char const* iFile,
                 parlay::sequence<E>& wp,
                 [[maybe_unused]] int id_offset = 0) -> std::pair<size_t, int>
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

void
PrintTreeParam(char const* tree_name, int leaf_size)
{
  std::cout << "Tree: " << tree_name << "; ";
  std::cout << "Buf: " << leaf_size << "; ";
}

// NOTE: default test functions for all custom tree
static auto constexpr DefaultTestFunc =
  []<class TreeDesc, typename TypeTrait, typename Options>(Options opts) {
    using Tree = TreeDesc::TreeType;
    using TT = TypeTrait;
    using EntrySeq = TT::EntrySeq;
    EntrySeq wp;
    EntrySeq wi;

    Tree tree;
    constexpr bool kTestTime = true;

#ifdef MEMCHECK
    read_points_serial<typename TT::E>(opts.input_file_path.c_str(), wp, 0);
    tree.Build(wp.cut(0, wp.size()));
    tree.DeleteTree();
    return;
#endif // MEMCHECK

    ReadEntries<typename TT::E>(opts.input_file_path.c_str(), wp);
    BuildTree<TT, Tree, kTestTime, 2>(wp, opts.rounds, tree);

    // // NOTE: batch insert
    if (opts.tag & (1 << 0)) {
      if (opts.summary) {
        parlay::sequence<double> const ratio_seq = { 0.0001, 0.001, 0.01, 0.1 };
        for (auto ratio : ratio_seq) {
          auto [base_path, batch_path] = opts.ParseInsertBatchPath(ratio);
          // std::cout << base_path << " " << batch_path << std::endl;
          auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp);
          auto [n2, d2] = ReadEntries<typename TT::E>(batch_path.c_str(), wi);
          BatchInsert<TT, Tree, kTestTime>(tree, wp, wi, opts.rounds);
        }
      } else {
        auto [base_path, batch_path] =
          opts.ParseInsertBatchPath(opts.batch_ratio);
        // std::cout << base_path << " " << batch_path << std::endl;
        auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp);
        auto [n2, d2] = ReadEntries<typename TT::E>(batch_path.c_str(), wi);
        BatchInsert<TT, Tree, kTestTime>(tree, wp, wi, opts.rounds);
      }
    }

    // NOTE: batch delete
    if (opts.tag & (1 << 1)) {
      if (opts.summary) {
        parlay::sequence<double> const ratio_seq = { 0.0001, 0.001, 0.01, 0.1 };
        // parlay::sequence<double> const ratio_seq = { 0.01, 0.1 };
        for (auto ratio : ratio_seq) {
          // puts("------------------------------");
          auto [base_path, batch_path] = opts.ParseDeleteBatchPath(ratio);
          // std::cout << base_path << " " << batch_path << std::endl;
          auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp);
          auto [n2, d2] = ReadEntries<typename TT::E>(batch_path.c_str(), wi);
          BatchDelete<TT, Tree, kTestTime>(tree, wp, wi, opts.rounds);
        }
      } else {
        auto [base_path, batch_path] =
          opts.ParseDeleteBatchPath(opts.batch_ratio);
        // std::cout << base_path << " " << batch_path << std::endl;
        auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp);
        auto [n2, d2] = ReadEntries<typename TT::E>(batch_path.c_str(), wi);
        BatchDelete<TT, Tree, kTestTime>(tree, wp, wi, opts.rounds);
      }
    }

    if (opts.query_type != 0 && opts.tag != 0) {
      auto [base_path, batch_path] =
        opts.ParseInsertBatchPath(opts.batch_ratio);
      auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp);
      BuildTree<TT, Tree, kTestTime, 0>(wp, opts.rounds, tree);
    }

    // PARA: lambda for membership
    auto membership_query = [&](auto const& query_ele) {
      auto res = parlay::sequence<bool>(query_ele.size(), true);
      Membership<TT>(tree, query_ele, opts.rounds, res);
    };

    // PARA: lambda for range query
    auto range_query = [&](auto const& base_points) {
      bool constexpr kSavePoint = false;
      bool constexpr kFixSize = false;
      for (int rec_type = 0; rec_type < kRangeQueryType; rec_type++) {
        auto [query_box_seq, max_size] =
          GenRectangles<TT, kSavePoint, kFixSize>(
            kRangeQueryNum, rec_type, base_points);
        typename TT::EntrySeq Out;
        RangeQueryFix<TT>(
          tree, opts.rounds, Out, kRangeQueryNum, query_box_seq);
      }
    };

    // PARA: lambda for range count
    auto range_count = [&](auto const& base_points) {
      bool constexpr kSavePoint = false;
      bool constexpr kFixSize = false;
      for (int rec_type = 0; rec_type < kRangeQueryType; rec_type++) {
        auto [query_box_seq, max_size] =
          GenRectangles<TT, kSavePoint, kFixSize>(
            kRangeQueryNum, rec_type, base_points);
        RangeCountFix<TT>(tree, opts.rounds, kRangeQueryNum, query_box_seq);
      }
    };

    // NOTE: membership
    if (opts.query_type & (1 << 0)) {
      auto batch_size = static_cast<size_t>(wp.size() * kMemberQueryRatio);
      typename TT::EntrySeq query_ele;
      if (wi.size() == 0) {
        query_ele = wp.subseq(0, batch_size);
        membership_query(query_ele);
      } else {
        query_ele = wp.subseq(0, batch_size);
        membership_query(query_ele);

        query_ele = wi.subseq(0, batch_size);
        membership_query(query_ele);

        query_ele = parlay::append(wp.subseq(0, batch_size / 2),
                                   wi.subseq(0, batch_size - (batch_size / 2)));
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
      for (int rec_type = 0; rec_type < kRangeQueryType; rec_type++) {
        auto [query_box_seq, max_size] =
          GenRectangles<TT, kSavePoint, kFixSize>(
            kRangeQueryWithLogNum, rec_type, wp);
        typename TT::EntrySeq Out;
        RangeQuerySerialWithLog<TT>(tree,
                                    opts.rounds,
                                    Out,
                                    kRangeQueryWithLogNum,
                                    rec_type,
                                    query_box_seq);
      }
    }

    // // NOTE: incremental build
    // if (opts.tag & (1 << 2)) {
    //   parlay::sequence<double> const ratio_seq = { 0.0001, 0.001, 0.01, 0.1
    //   }; for (auto ratio : ratio_seq) {
    //     auto [base_path, batch_path] = opts.ParseInsertBatchPath(ratio);
    //     // std::cout << base_path << " " << batch_path << std::endl;
    //     auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp);
    //     auto [n2, d2] = ReadEntries<typename TT::E>(batch_path.c_str(), wi);
    //     BatchInsertByStep<TT, Tree, kTestTime>(tree, wp, ratio, opts.rounds);
    //     range_query(wp);
    //   }
    // }

    // NOTE: batch update by size
    if (opts.tag & (1 << 3)) {
      parlay::sequence<double> const ratio_seq = {
        0.0001, 0.0002, 0.0005, 0.001, 0.002, 0.005, 0.01,
        0.02,   0.05,   0.1,    0.2,   0.5,   1.0
      };
      // parlay::sequence<double> const ratio_seq = { 1.0 };
      for (auto ratio : ratio_seq) {
        auto [base_path, batch_path] = opts.ParseInsertBatchPath(ratio);
        // std::cout << base_path << " " << batch_path << std::endl;
        auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp);
        auto [n2, d2] = ReadEntries<typename TT::E>(batch_path.c_str(), wi);
        BatchInsert<TT, Tree, kTestTime>(tree, wp, wi, opts.rounds);
      }

      for (auto ratio : ratio_seq) {
        auto [base_path, batch_path] = opts.ParseDeleteBatchPath(ratio);
        // std::cout << base_path << " " << batch_path << std::endl;
        auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp);
        auto [n2, d2] = ReadEntries<typename TT::E>(batch_path.c_str(), wi);
        BatchDelete<TT, Tree, kTestTime>(tree, wp, wi, opts.rounds);
      }
    }

    // NOTE: test randomness
    if (opts.tag & (1 << 4)) {
      const parlay::sequence<int> sort_ratio_seq = { 0,  10, 20, 30, 40, 50,
                                                     60, 70, 80, 90, 100 };
      auto const time_period_num = 100;
      auto const random_in = parlay::random_shuffle(wp);
      auto const step = wp.size() / time_period_num;
      auto const insert_batch_seq =
        parlay::tabulate(time_period_num, [&](size_t i) {
          return Tree::ParseInput(
            random_in.subseq(i * step, std::min((i + 1) * step, wp.size())));
        });
      auto const delete_batch_seq =
        parlay::tabulate(time_period_num, [&](size_t i) {
          return Tree::ParseDeleteInput(
            random_in.subseq(i * step, std::min((i + 1) * step, wp.size())));
        });

      puts("");
      for (auto ratio : sort_ratio_seq) {
        TestLeafRandom<TT>(tree,
                           wp,
                           insert_batch_seq,
                           delete_batch_seq,
                           time_period_num,
                           opts.rounds,
                           ratio);
      }
    }

    std::cout << "\n" << std::flush;

    tree.DeleteTree();

    return;
  };

// NOTE: count how many sequentially-numbered batch file pairs
// (<i>_insert.bin / <i>_delete.bin) live under `dir`. Starts at i = 0 and
// stops at the first missing insert file, so the batches are expected to be
// contiguous from 0.
static int
CountDynamicGraphBatches(std::string const& dir)
{
  int n = 0;
  while (std::filesystem::exists(dir + std::to_string(n) + "_insert.bin")) {
    ++n;
  }
  return n;
}

static auto constexpr TestTemporalGraph =
  []<class TreeDesc, typename TypeTrait, typename Options>(Options opts) {
    using Tree = TreeDesc::TreeType;
    using TT = TypeTrait;
    using EntrySeq = TT::EntrySeq;
    EntrySeq wp;
    EntrySeq wi;

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
      EntrySeq osm_records;
      auto [input_num, input_dim] =
        ReadEntries<typename TT::E, kInputType>(osm_path.c_str(), osm_records);

      parlay::stable_sort_inplace(
        osm_records, [&](auto const& a, auto const& b) {
          return TT::Entry::GetVal(a) < TT::Entry::GetVal(b);
        });
      const std::vector<int> files = { 2015, 2016, 2017, 2018, 2019,
                                       2020, 2021, 2022, 2023, 2024 };
      const auto year_num = files.size();
      auto node_by_year = parlay::tabulate(year_num, [&](size_t i) {
        return parlay::filter(osm_records, [&](auto const& record) {
          return TT::Entry::GetVal(record) == files[i];
        });
      });

      // membership
      auto member_res = parlay::sequence<bool>(kMemberQueryOsmSize, true);
      auto range_res = typename TT::EntrySeq();

      InsertOsmByTime<TT>(node_by_year,
                          tree,
                          opts.rounds,
                          kOsmWindowLen,
                          kMemberQueryOsmSize,
                          member_res,
                          range_res);
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
        insert_files[i] =
          dynamic_graph_path + std::to_string(i) + "_insert.bin";
        delete_files[i] =
          dynamic_graph_path + std::to_string(i) + "_delete.bin";
      }
      parlay::sequence<EntrySeq> insert_batch_seq(kDynamicGraphBatchNum);
      parlay::sequence<EntrySeq> delete_batch_seq(kDynamicGraphBatchNum);
      size_t total_entries = 0;
      for (int i = 0; i < kDynamicGraphBatchNum; i++) {
        auto [n1, dim1] = ReadEntries<typename TT::E, kInputType>(
          insert_files[i].c_str(), insert_batch_seq[i], total_entries);
        total_entries += n1;

        auto [n2, dim2] = ReadEntries<typename TT::E, kInputType>(
          delete_files[i].c_str(), delete_batch_seq[i], total_entries);
        total_entries += n2;
      }

      // NOTE: membership query result
      auto membership_res = parlay::sequence<bool>(kMemberQueryOsmSize, true);
      auto random_osm_records =
        parlay::random_shuffle(parlay::flatten(insert_batch_seq));
      auto membership_query_points =
        parlay::tabulate(kMemberQueryOsmSize, [&](size_t i) {
          return TT::Entry::GetKey(random_osm_records[i]);
        });

      // NOTE: generate box
      // NOTE: count how often each source vertex occurs (its out-degree) and
      // build a (vertex, occurrence) sequence, highest occurrence first. The
      // top vertices are later used as the keys for the range queries.
      auto osm_records = parlay::flatten(insert_batch_seq);
      auto edge_list = parlay::tabulate(osm_records.size(), [&](size_t i) {
        auto key = TT::Entry::GetKey(osm_records[i]);
        uint64_t u = key >> 32;
        uint64_t v = key & 0xffffffff;
        return std::make_pair(u, v);
      });
      auto range_query_vertex_occurrence = parlay::histogram_by_key(
        parlay::map(edge_list, [](auto const& e) { return e.first; }));
      parlay::sort_inplace(
        range_query_vertex_occurrence,
        [](auto const& a, auto const& b) { return a.second > b.second; });
      range_query_vertex_occurrence =
        range_query_vertex_occurrence.subseq(0, kDynamicGraphRangeNum);
      // WARN: may don't have enough vertices for range query
      auto [range_query_offset, tot_range_size] =
        parlay::scan(parlay::map(range_query_vertex_occurrence,
                                 [](auto const& p) { return p.second; }),
                     parlay::addm<size_t>());
      range_query_offset.push_back(tot_range_size);

      auto range_report_output = typename TT::EntrySeq(tot_range_size);

      // NOTE: flatten
      auto flatten_seq = TT::EntrySeq::uninitialized(kDynamicFlattenSize);

      TestDynamicGraph<TT, Tree>(insert_batch_seq,
                                 delete_batch_seq,
                                 tree,
                                 opts.rounds,
                                 membership_res,
                                 range_report_output,
                                 flatten_seq,
                                 membership_query_points,
                                 range_query_vertex_occurrence,
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
        insert_files[i] =
          dynamic_graph_path + std::to_string(i) + "_insert.bin";
        delete_files[i] =
          dynamic_graph_path + std::to_string(i) + "_delete.bin";
      }
      parlay::sequence<EntrySeq> insert_batch_seq(kDynamicGraphBatchNum);
      parlay::sequence<EntrySeq> delete_batch_seq(kDynamicGraphBatchNum);
      size_t total_entries = 0;
      for (int i = 0; i < kDynamicGraphBatchNum; i++) {
        auto [n1, dim1] = ReadEntries<typename TT::E, kInputType>(
          insert_files[i].c_str(), insert_batch_seq[i], total_entries);
        total_entries += n1;

        auto [n2, dim2] = ReadEntries<typename TT::E, kInputType>(
          delete_files[i].c_str(), delete_batch_seq[i], total_entries);
        total_entries += n2;
      }

      // membership query result
      auto membership_res = parlay::sequence<bool>(kMemberQueryOsmSize, true);
      auto const time_period_num = insert_batch_seq.size();

      const parlay::sequence<int> sort_ratio_seq = { 0,  10, 20, 30, 40, 50,
                                                     60, 70, 80, 90, 100 };
      puts("");
      for (auto ratio : sort_ratio_seq) {
        TestLeafRandomDynamicGraph<TT, Tree>(tree,
                                             insert_batch_seq,
                                             delete_batch_seq,
                                             membership_res,
                                             time_period_num,
                                             opts.rounds,
                                             ratio);
      }
    }

    // NOTE: temporal graph signal sliding window
    if (opts.tag & (1 << 6)) {
      string osm_path = opts.input_file_path;
      EntrySeq osm_records;
      auto [input_num, input_dim] =
        ReadEntries<typename TT::E, kInputType>(osm_path.c_str(), osm_records);

      parlay::stable_sort_inplace(
        osm_records, [&](auto const& a, auto const& b) {
          return TT::Entry::GetVal(a) < TT::Entry::GetVal(b);
        });
      auto max_timestamp = osm_records.back().val;
      auto timestamp_seq =
        parlay::tabulate(max_timestamp, [&](size_t i) { return i + 1; });
      const auto year_num = timestamp_seq.size();
      auto node_by_year = parlay::tabulate(year_num, [&](size_t i) {
        return parlay::filter(osm_records, [&](auto const& record) {
          return TT::Entry::GetVal(record) == timestamp_seq[i];
        });
      });

      // NOTE: membership query stuff
      auto membership_res = parlay::sequence<bool>(kMemberQueryOsmSize, true);
      auto random_osm_records = parlay::random_shuffle(osm_records);
      auto membership_query_points =
        parlay::tabulate(kMemberQueryOsmSize, [&](size_t i) {
          return TT::Entry::GetKey(random_osm_records[i]);
        });

      // NOTE: range query stuff
      // NOTE: count how often each source vertex occurs (its out-degree) and
      // build a (vertex, occurrence) sequence, highest occurrence first. The
      // top vertices are later used as the keys for the range queries.
      auto edge_list = parlay::tabulate(osm_records.size(), [&](size_t i) {
        auto key = TT::Entry::GetKey(osm_records[i]);
        uint64_t u = key >> 32;
        uint64_t v = key & 0xffffffff;
        return std::make_pair(u, v);
      });
      auto range_query_vertex_occurrence = parlay::histogram_by_key(
        parlay::map(edge_list, [](auto const& e) { return e.first; }));
      parlay::sort_inplace(
        range_query_vertex_occurrence,
        [](auto const& a, auto const& b) { return a.second > b.second; });
      range_query_vertex_occurrence = range_query_vertex_occurrence.subseq(
        0, std::min(edge_list.size(), kDynamicGraphRangeNum));
      // WARN: may don't have enough vertices for range query
      auto [range_query_offset, tot_range_size] =
        parlay::scan(parlay::map(range_query_vertex_occurrence,
                                 [](auto const& p) { return p.second; }),
                     parlay::addm<size_t>());
      range_query_offset.push_back(tot_range_size);

      auto range_report_output = typename TT::EntrySeq(tot_range_size);

      // NOTE: flatten stuff
      size_t tree_size = 0;
      size_t max_tree_size = 0;
      for (int batch_id = 0; batch_id < node_by_year.size(); batch_id++) {
        tree_size += node_by_year[batch_id].size();
        if (batch_id >= kOsmWindowLen) {
          tree_size -= node_by_year[batch_id - kOsmWindowLen].size();
        }
        max_tree_size = std::max(max_tree_size, tree_size);
      }
      auto flatten_seq = TT::EntrySeq::uninitialized(max_tree_size);

      // NOTE: main function call
      TestTemporalSignal<TT>(node_by_year,
                             tree,
                             opts.rounds,
                             kOsmWindowLen,
                             kMemberQueryOsmSize,
                             membership_res,
                             range_report_output,
                             flatten_seq,
                             membership_query_points,
                             range_query_vertex_occurrence,
                             range_query_offset);
    }
  };

// NOTE: driven functions
template<typename TreeWrapper, typename RunFunc>
static void
Run(Options const& opts, RunFunc test_func)
{
  using TT = typename TreeWrapper::TreeType::TT;
  using E = typename TT::E;
  using EntrySeq = typename TT::EntrySeq;

  PrintTreeParam(TreeWrapper::TreeType::GetTreeName(),
                 TreeWrapper::TreeType::GetLeafSize());
  opts.PrintOptions();
  std::cout << "\n" << std::flush;

  // Apply the test function
  test_func.template operator()<TreeWrapper, TT>(opts);
};

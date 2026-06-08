#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <iostream>
#include <string>
#include <utility>

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
TestLeafRandom(Tree&, typename TypeTrait::EntrySeq const&, int const, int const)
{
}

template<typename TypeTrait, typename Tree>
  requires pb_tree::IsPBTree<Tree>
void
TestLeafRandom(Tree& pkd,
               typename TypeTrait::EntrySeq const& WP,
               int const rounds,
               int const ratio)
{
  using TT = TypeTrait;
  auto const wp = Tree::ParseInput(WP);

  auto const random_in = parlay::random_shuffle(WP);

  auto const time_period_num = 100;

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

  auto const query_size = static_cast<size_t>(wp.size() * kMemberQueryRatio);
  auto query_ele = wp.subseq(0, query_size);
  auto member_res = parlay::sequence<bool>(query_ele.size(), true);

  constexpr auto leaf_entries_pattern = TypeTrait::LeafEntriesPattern::kHybrid;
  constexpr bool sorted_input = false;
  parlay::internal::timer t;

  parlay::sequence<double> insert_time(time_period_num, 0.0);
  parlay::sequence<double> delete_time(time_period_num, 0.0);
  double member_query_time = 0.0;

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

  std::cout << ratio << " " << insert_ave << " " << delete_ave << " "
            << member_query_ave << std::endl;
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

  std::cout << std::right << " " << "insert_n" << " "
            << "delete_n" << " " << "tree_n" << " "
            << "insert_t(s)" << " " << "delete_t(s)" << " "
            << "member_query_t(s)" << " " << "range_query_t(s)" << std::endl;
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

template<typename TT, typename Tree>
void
TestDynamicGraph(const auto& insert_batch_seq,
                 const auto& delete_batch_seq,
                 Tree& pkd,
                 const int rounds,
                 auto& membership_res,
                 auto& range_report_output,
                 auto& flatten_seq)
{
  pkd.DeleteTree();
  auto time_period_num = insert_batch_seq.size();
  parlay::sequence<typename Tree::InputRangeT> wi(time_period_num);
  parlay::sequence<typename Tree::InputDeleteRangeT> wd(time_period_num);
  parlay::sequence<typename Tree::InputRangeT> insert_entry(time_period_num);
  parlay::sequence<typename Tree::InputDeleteRangeT> delete_entry(
    time_period_num);
  const size_t member_query_n = membership_res.size();
  auto query_points = TT::KeySeq::uninitialized(member_query_n);
  auto step = member_query_n / time_period_num;
  if (member_query_n % time_period_num != 0) {
    std::cout << "the member query num should be divisible by time period num"
              << std::endl;
    return;
  }

  for (int i = 0; i < time_period_num; i++) {
    wi[i] = Tree::ParseInput(insert_batch_seq[i]);
    wd[i] = Tree::ParseDeleteInput(delete_batch_seq[i]);
    assert(insert_batch_seq[i].size() >= step);
    parlay::parallel_for(0, step, [&](size_t j) {
      query_points[(i * step) + j] = TT::Entry::GetKey(insert_batch_seq[i][j]);
    });
  }

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
        auto [contain, logger] = pkd.Contain(query_points[j]);
        membership_res[j] = contain;
      });
      t.stop();
      member_query_time[batch_id] += t.total_time();

      // NOTE: range report query
      auto random_wi = parlay::random_shuffle(insert_batch_seq[batch_id]);
      auto box_seq = parlay::tabulate(kDynamicGraphRangeNum, [&](size_t i) {
        auto e = random_wi[i];
        uint64_t key = TT::Entry::GetKey(e);
        uint64_t u = key >> 32;
        return std::make_pair(u << 32, (u + 1) << 32);
      });
      parlay::sequence<size_t> size_seq(box_seq.size(), 0);
      t.reset(), t.start();
      parlay::parallel_for(0, box_seq.size(), [&](size_t j) {
        auto [size, logger] = pkd.RangeReport(
          box_seq[j],
          range_report_output.cut(kDynamicGraphMaxSingleRange * j,
                                  kDynamicGraphMaxSingleRange * (j + 1)));
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

  std::cout << std::right << " " << "insert_n" << " "
            << "delete_n" << " " << "tree_n" << " "
            << "insert_t(s)" << " " << "delete_t(s)" << " "
            << "find_t(s)" << " " << "range_t(s)" << " "
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
}

template<typename E, int kInputType = 0>
auto
ReadEntries(char const* iFile, parlay::sequence<E>& wp, size_t offset = 0)
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
      wp[i].ReadVal(std::to_string(i + offset));
    } else if constexpr (kInputType == InputType::kOsmInput) {
      wp[i].ReadKey(pts[i * dim]);
      wp[i].ReadVal(pts[(i * dim) + 1]);
      // wp[i].ReadKey(pts[i * dim], offset + i);
      // wp[i].ReadVal(pts[(i * dim) + 1]);
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
        opts.ParseInsertBatchPath(kMemberQueryRatio);
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
      puts("");

      for (auto ratio : sort_ratio_seq) {
        TestLeafRandom<TT>(tree, wp, opts.rounds, ratio);
      }
    }

    std::cout << "\n" << std::flush;

    tree.DeleteTree();

    return;
  };

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

    if (opts.tag & (1 << 4)) { // NOTE: temporal graph
      // string dynamic_graph_path =
      //   "/data/zmen002/geometry/osm_temporal_graph/north_america/";
      string dynamic_graph_path = opts.input_file_path;

      constexpr int kDynamicGraphBatchNum = 10;
      std::array<std::string, kDynamicGraphBatchNum> insert_files;
      std::array<std::string, kDynamicGraphBatchNum> delete_files;
      for (int i = 0; i < kDynamicGraphBatchNum; i++) {
        insert_files[i] = dynamic_graph_path + std::to_string(i) + "_insert.in";
        delete_files[i] = dynamic_graph_path + std::to_string(i) + "_delete.in";
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

        // std::cout << n1 << " " << n2 << std::endl;
      }

      // generate box
      auto range_report_output = typename TT::EntrySeq(
        kDynamicGraphRangeNum * kDynamicGraphMaxSingleRange);

      // membership query result
      auto membership_res = parlay::sequence<bool>(kMemberQueryOsmSize, true);

      // flatten
      auto flatten_seq = TT::EntrySeq::uninitialized(kDynamicFlattenSize);
      TestDynamicGraph<TT, Tree>(insert_batch_seq,
                                 delete_batch_seq,
                                 tree,
                                 opts.rounds,
                                 membership_res,
                                 range_report_output,
                                 flatten_seq);
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

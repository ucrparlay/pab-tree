#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <iostream>
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
#include "pmos/b_tree.h"
#include "wrapper.h"

#ifdef CCP
using Coord = long;
// using Coord = double;
#else
using Coord = long;
// using Coord = double;
#endif // CCP

using Typename = Coord;
using namespace pmos;

// NOTE: membership size
static constexpr double kMemberQueryRatio = 0.01;
// NOTE: OSM
static constexpr size_t kMemberQueryOsmSize = 100'000'000;
static constexpr int kOsmWindowLen = 5;
// NOTE: rectangle numbers
static constexpr int kRangeQueryNum = 50'000;
// static constexpr int kRangeQueryNum = 500;
static constexpr int kRangeQueryType = 3;
static constexpr int kSingleQueryLogRepeatNum = 100;

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
  auto parsed_input = Tree::ParseInput(WP);
  auto const& wp = parsed_input;

  if constexpr (kTestTime) {
    pkd.DeleteTree();

    double aveBuild = time_loop(
      rounds,
      loop_late,
      [&]() { wp = parsed_input; },
      [&]() { pkd.Build(wp); },
      [&]() { pkd.DeleteTree(); });

    wp = parsed_input;
    pkd.Build(wp);
    if constexpr (kPrint) {
      std::cout << aveBuild << " " << std::flush;
    }
  } else {
    // NOTE: always return a built tree
    pkd.DeleteTree();
    wp = parsed_input;
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
  typename Tree::InputRangeT parsed_base = Tree::ParseInput(WP);
  typename Tree::InputRangeT parsed_batch = Tree::ParseInput(WI);
  auto const& wp = parsed_base;
  auto const& wi = parsed_batch;
  double loop_late = rounds > 1 ? 0.01 : -100;

  if constexpr (kTestTime) { // NOTE: clean and measure time
    pkd.DeleteTree();
    double aveInsert = time_loop(
      rounds,
      loop_late,
      [&]() {
        wp = parsed_base;
        wi = parsed_batch;
        pkd.Build(wp);
      },
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
  auto parsed_base = Tree::ParseInput(WP);
  auto parsed_batch = Tree::ParseDeleteInput(WI);
  auto const& wp = parsed_base;
  auto const& wi = parsed_batch;
  double loop_late = rounds > 1 ? 0.01 : -100;

  if constexpr (kTestTime) { // NOTE: clean and measure time
    pkd.DeleteTree();
    double aveInsert = time_loop(
      rounds,
      loop_late,
      [&]() {
        wp = parsed_base;
        wi = parsed_batch;
        pkd.Build(wp);
      },
      [&]() {
        // puts("-----------------------");
        pkd.BatchDelete(wi);
      },
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
      parlay::parallel_for(0, query_num, [&](size_t i) {
        // for (int i = 0; i < query_num; i++) {
        auto [contain, logger] = pkd.Contain(Entry::GetKey(query_ele[i]));

        res[i] = contain;
        // vis_leaf[i] = logger.vis_leaf_num;
        // vis_inter[i] = logger.vis_interior_num;
        // }
      });
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

template<typename Point, typename Tree>
void
RangeQuerySerialWithLog(Tree& pkd,
                        Typename* kdknn,
                        int const& rounds,
                        parlay::sequence<Point>& Out,
                        int rec_type,
                        int rec_num,
                        int DIM,
                        auto const& query_box_seq,
                        auto max_size)
{
  auto [offset, tot_size] = parlay::scan(
    parlay::delayed_tabulate(
      rec_num, [&](size_t i) -> size_t { return query_box_seq[i].second; }),
    parlay::addm<size_t>());
  offset.push_back(tot_size);
  // using ref_t = std::reference_wrapper<point>;
  // parlay::sequence<ref_t> out_ref( Out.size(), std::ref( Out[0] ) );
  Out.resize(tot_size);

  for (int i = 0; i < rec_num; i++) {
    parlay::internal::timer t;
    t.reset(), t.start();
    auto [size, logger] =
      pkd.RangeQuery(query_box_seq[i].first, Out.cut(offset[i], offset[i + 1]));
    t.stop();
    // double ave_query = time_loop(
    //     rounds, -1.0, [&]() {},
    //     [&]() {
    //       auto [size, logger] = pkd.RangeQuery(
    //           query_box_seq[i].first, Out.cut(offset[i], offset[i + 1]));
    //     },
    //     [&]() {});
    std::cout << rec_type << " " << query_box_seq[i].second << " "
              << std::scientific << t.total_time() << std::endl;
  }

  return;
}

// TODO: cpam change to slow insert
template<typename TT, typename Tree>
void
InsertOsmByTime(const parlay::sequence<typename TT::EntrySeq>& node_by_time,
                Tree& pkd,
                const int rounds,
                const int sliding_window_len,
                const size_t member_query_n,
                auto& res)
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
    // std::cout << i << " "
    //           << *parlay::max_element(node_by_time[i],
    //                                   [&](auto a, auto b) {
    //                                     return TT::Entry::GetKey(a) <
    //                                            TT::Entry::GetKey(b);
    //                                   })
    //           << *parlay::min_element(node_by_time[i],
    //                                   [&](auto a, auto b) {
    //                                     return TT::Entry::GetKey(a) <
    //                                            TT::Entry::GetKey(b);
    //                                   })
    //           << std::endl;

    if (node_by_time[i].size() < step) {
      std::cout << "not enough points in time period " << i
                << " for member query, need " << step << std::endl;
      return;
    }
    parlay::parallel_for(0, step, [&](size_t j) {
      query_points[(i * step) + j] = TT::Entry::GetKey(node_by_time[i][j]);
    });
  }
  // return;

  parlay::sequence<double> insert_time(time_period_num, 0.0);
  parlay::sequence<double> delete_time(time_period_num, 0.0);
  parlay::sequence<double> query_time(time_period_num, 0.0);
  parlay::sequence<size_t> tree_size(time_period_num, 0);

  for (int r = 0; r < rounds; r++) {
    auto const& insert_entry = wi;
    auto const& delete_entry = wd;

    size_t current_tree_size = 0;
    if (r == 1) { // omit the first rounds
      for (int i = 0; i < time_period_num; i++) {
        insert_time[i] = 0.0;
        delete_time[i] = 0.0;
        query_time[i] = 0.0;
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
      current_tree_size += node_by_time[batch_id].size();
#ifdef DEBUG
      std::cerr << " insert batch: " << batch_id << std::flush;
      pkd.Validate();
#endif // DEBUG

      if (batch_id >= sliding_window_len) {
        // puts("begin delete");
        // std::cout << delete_entry[i - cur_batch_id].size() << " ";
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
        // std::cout << "-1 " << std::flush;
        delete_time[batch_id] = 0.0;
      }
      tree_size[batch_id] = current_tree_size;

      // NOTE: test membership query
      t.reset(), t.start();
      parlay::parallel_for(0, member_query_n, [&](size_t j) {
        auto [contain, logger] = pkd.Contain(query_points[j]);
        res[j] = contain;
      });
      t.stop();
      query_time[batch_id] += t.total_time();

      // std::cout << std::endl;
    }
  }

  std::cout << std::right << std::setw(12) << "insert_n" << std::setw(12)
            << "delete_n" << std::setw(12) << "tree_n" << std::setw(14)
            << "insert_t(s)" << std::setw(14) << "delete_t(s)" << std::setw(14)
            << "query_t(s)" << std::endl;
  for (int batch_id = 0; batch_id < time_period_num; batch_id++) {
    size_t const delete_n =
      batch_id >= sliding_window_len
        ? node_by_time[batch_id - sliding_window_len].size()
        : 0;
    std::cout << std::right << std::setw(12) << node_by_time[batch_id].size()
              << std::setw(12) << delete_n << std::setw(12)
              << tree_size[batch_id] << std::setw(14) << std::fixed
              << std::setprecision(6) << insert_time[batch_id] / (rounds - 1)
              << std::setw(14) << delete_time[batch_id] / (rounds - 1)
              << std::setw(14) << query_time[batch_id] / (rounds - 1)
              << std::endl;
  }
}

template<typename TT, typename Tree>
void
TestDynamicGraph(const auto& insert_batch_seq,
                 const auto& delete_batch_seq,
                 Tree& pkd,
                 const size_t member_query_n)
{
  pkd.DeleteTree();
  const int time_period_num = insert_batch_seq.size();
  parlay::sequence<typename Tree::InputRangeT> insert_entry(time_period_num);
  parlay::sequence<typename Tree::InputDeleteRangeT> delete_entry(
    time_period_num);
  // typename TT::Points query_points(batchQueryOsmSize);
  auto query_points = TT::KeySeq::uninitialized(member_query_n);
  auto step = member_query_n / time_period_num;

  // check memer query size
  if (member_query_n % time_period_num != 0) {
    std::cout << "the member query num should be divisible by time period num"
              << std::endl;
    return;
  }

  for (int i = 0; i < time_period_num; i++) {
    insert_entry[i] = Tree::ParseInput(insert_batch_seq[i]);
    delete_entry[i] = Tree::ParseDeleteInput(delete_batch_seq[i]);

    // if (insert_entry[i].size() - delete_entry.size() < step) {
    //   std::cout << "not enough points in time period " << i
    //             << " for member query, need " << step << std::endl;
    //   return;
    // }
    // parlay::parallel_for(0, step, [&](size_t j) {
    //   query_points[(i * step) + j] = TT::Entry::GetKey(node_by_time[i][j]);
    // });
  }
  // return;

  int within_num = 0;
  std::cout << std::endl;
  for (int i = 0; i < time_period_num; i++) {
    std::cout << insert_batch_seq[i].size() << " " << delete_batch_seq[i].size()
              << " ";

    // INFO: batch insert
    parlay::internal::timer t;
    t.reset(), t.start();
    pkd.BatchInsert(insert_entry[i]);
    t.stop();
    std::cout << t.total_time() << " " << std::flush;

    // INFO: batch delete
    t.reset(), t.start();
    pkd.BatchDelete(delete_entry[i]);
    t.stop();
    std::cout << t.total_time() << " " << std::flush;

    // NOTE: test membership query
    t.reset(), t.start();
    parlay::parallel_for(0, member_query_n, [&](size_t j) {
      [[maybe_unused]] auto [contain, logger] = pkd.Contain(query_points[j]);
    });
    t.stop();
    std::cout << t.total_time();

    std::cout << std::endl;
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
  wp.resize(N);
  parlay::parallel_for(0, n, [&](size_t i) -> void {
    if constexpr (kInputType == InputType::kDynamicGraphInput) {
      wp[i].ReadKey(pts[i * dim], pts[(i * dim) + 1]);
      wp[i].ReadVal(std::to_string(i));
    } else if constexpr (kInputType == InputType::kOsmInput) {
      wp[i].ReadKey(pts[i * dim]);
      wp[i].ReadVal(pts[(i * dim) + 1]);
      // wp[i].ReadKey(pts[i * dim], offset + i);
      // wp[i].ReadVal(pts[(i * dim) + 1]);
    } else {
      wp[i].ReadKey(pts[i * dim]);
      wp[i].ReadVal(pts[(i * dim) + 1]);
    }
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
    auto membership_query = [&](auto const& query_ele) {};

    // PARA: lambda for range query
    auto range_query = [&](auto const& base_points) {
      for (int rec_type = 0; rec_type < kRangeQueryType; rec_type++) {
        auto [query_box_seq, max_size] = GenRectangles<TT, false, false>(
          kRangeQueryNum, rec_type, base_points);
        typename TT::EntrySeq Out;
        RangeQueryFix<TT>(
          tree, opts.rounds, Out, kRangeQueryNum, query_box_seq);
      }
    };

    // PARA: lambda for range count
    auto range_count = [&](auto const& base_points) {
      for (int rec_type = 0; rec_type < kRangeQueryType; rec_type++) {
        auto [query_box_seq, max_size] = GenRectangles<TT, false, false>(
          kRangeQueryNum, rec_type, base_points);
        RangeCountFix<TT>(tree, opts.rounds, kRangeQueryNum, query_box_seq);
      }
    };

    // NOTE: membership
    if (opts.query_type & (1 << 0)) {
      auto batch_size = static_cast<size_t>(wp.size() * kMemberQueryRatio);
      auto query_ele =
        wi.size() ? wi.subseq(0, batch_size) : wp.subseq(0, batch_size);
      auto res = parlay::sequence<bool>(query_ele.size(), true);
      Membership<TT>(tree, query_ele, opts.rounds, res);
    }

    // NOTE: range query
    if (opts.query_type & (1 << 1)) {
      range_query(wp);
    }

    // NOTE: range count
    if (opts.query_type & (1 << 2)) {
      range_count(wp);
    }

    // NOTE: incremental build
    if (opts.tag & (1 << 2)) {
      parlay::sequence<double> const ratio_seq = { 0.0001, 0.001, 0.01, 0.1 };
      for (auto ratio : ratio_seq) {
        auto [base_path, batch_path] = opts.ParseInsertBatchPath(ratio);
        // std::cout << base_path << " " << batch_path << std::endl;
        auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp);
        auto [n2, d2] = ReadEntries<typename TT::E>(batch_path.c_str(), wi);
        BatchInsertByStep<TT, Tree, kTestTime>(tree, wp, ratio, opts.rounds);
        range_query(wp);
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
      parlay::sort_inplace(osm_records, [&](auto const& a, auto const& b) {
        return TT::Entry::GetVal(a) < TT::Entry::GetVal(b);
      });
      const std::vector<int> files = { 2015, 2016, 2017, 2018, 2019,
                                       2020, 2021, 2022, 2023, 2024 };
      auto node_by_year = parlay::tabulate(files.size(), [&](size_t i) {
        return parlay::filter(osm_records, [&](auto const& record) {
          return TT::Entry::GetVal(record) == files[i];
        });
      });
      // TODO: shufle the records

      auto res = parlay::sequence<bool>(kMemberQueryOsmSize, true);
      InsertOsmByTime<TT>(node_by_year,
                          tree,
                          opts.rounds,
                          kOsmWindowLen,
                          kMemberQueryOsmSize,
                          res);
    }

    if (opts.tag & (1 << 4)) { // NOTE: temporal graph
      string dynamic_graph_path =
        "/data/zmen002/geometry/osm_temporal_graph/north_america/";

      constexpr int kDynamicGraphBatchNum = 10;
      std::array<std::string, kDynamicGraphBatchNum> insert_files;
      std::array<std::string, kDynamicGraphBatchNum> delete_files;
      for (int i = 0; i < kDynamicGraphBatchNum; i++) {
        insert_files[i] = dynamic_graph_path + std::to_string(i) + "_insert.in";
        delete_files[i] = dynamic_graph_path + std::to_string(i) + "_delete.in";
      }
      parlay::sequence<EntrySeq> insert_batch_seq(kDynamicGraphBatchNum);
      parlay::sequence<EntrySeq> delete_batch_seq(kDynamicGraphBatchNum);
      for (int i = 0; i < kDynamicGraphBatchNum; i++) {
        ReadEntries<typename TT::E, kInputType>(insert_files[i].c_str(),
                                                insert_batch_seq[i]);
        ReadEntries<typename TT::E, kInputType>(delete_files[i].c_str(),
                                                delete_batch_seq[i]);
      }

      TestDynamicGraph<TT, Tree>(
        insert_batch_seq, delete_batch_seq, tree, kMemberQueryOsmSize);
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

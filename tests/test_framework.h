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
#include "parlay/internal/group_by.h"
#include "parlay/monoid.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/random.h"
#include "parlay/slice.h"
#include "pmos/b_tree.h"

#ifdef CCP
using Coord = long;
// using Coord = double;
#else
using Coord = long;
// using Coord = double;
#endif // CCP

using Typename = Coord;
using namespace pmos;

// NOTE: KNN size
static constexpr double kBatchQueryRatio = 0.01;
static constexpr size_t kBatchQueryOsmSize = 10000000;
// NOTE: rectangle numbers
static constexpr int kRangeQueryNum = 50000;
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

template<typename TT, typename Tree, bool SavePoint, bool FixSize>
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
  auto wp = parsed_input;

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
  auto wp = parsed_base;
  auto wi = parsed_batch;
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
  size_t slice_num = n / step;
  size_t round_cnt = 0;

  pkd.DeleteTree();

  auto incre_build = [&]() {
    parlay::internal::timer t;
    size_t l = 0;
    size_t r = 0;
    size_t cnt = 0;
    while (l < n) {
      r = std::min(l + step, n);
      // std::cout << l << " " << r << " " << n << std::endl;
      pkd.BatchInsert(parlay::make_slice(wp.begin() + l, wp.begin() + r));
      l = r;
    }
    round_cnt++;
  };

  double ave_time = time_loop(
    rounds,
    0.01,
    [&]() { parlay::copy(WP, wp); },
    [&]() { incre_build(); },
    [&]() { pkd.DeleteTree(); });

  // WARN: restore status
  parlay::copy(WP, wp);
  incre_build();
  // auto original_box = Tree::GetBox(wp.cut(0, n / remain_divide_ratio));
  // auto tree_box = pkd.GetRootBox();
  // std::cout << original_box.first << ' ' << original_box.second << std::endl;
  // std::cout << tree_box.first << ' ' << tree_box.second << std::endl;
  // auto root = pkd.cpam_aug_map_;
  // std::cout << "root size: " << root.size() << std::endl;

  return;
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
  auto wp = parsed_base;
  auto wi = parsed_batch;
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

template<typename Point, typename Tree, bool kTestTime = true>
void
BatchDiff(Tree& pkd,
          parlay::sequence<Point> const& WP,
          int const& rounds,
          double const total_ratio = 1.0,
          double const overlap_ratio = 0.5)
{
  using Points = typename Tree::Points;
  size_t total_batch_size = static_cast<size_t>(WP.size() * total_ratio);
  size_t overlap_size = static_cast<size_t>(total_batch_size * overlap_ratio);

  Points WI = parlay::tabulate(total_batch_size, [&](size_t i) -> Point {
    if (i < overlap_size) {
      return WP[i];
    } else {
      Point p = WP[i];
      if constexpr (IsAugPoint<Point>) {
        p.aug.id = -1 * p.aug.id - 1;
      } else {
        std::transform(
          p.pnt.begin(), p.pnt.end(), p.pnt.begin(), std::negate<Coord>());
      }
      return p;
    }
  });

  Points wp = Points::uninitialized(WP.size());
  Points wi = Points::uninitialized(WI.size());

  if constexpr (kTestTime) {
    pkd.DeleteTree();
    double aveDelete = time_loop(
      rounds,
      0.01,
      [&]() {
        BuildTree<Point, Tree, false>(WP, rounds, pkd);
        parlay::copy(WI, wi);
      },
      [&]() { pkd.BatchDiff(wi.cut(0, total_batch_size)); },
      [&]() { pkd.DeleteTree(); });
    std::cout << aveDelete << " " << std::flush;
    BuildTree<Point, Tree, false>(WP, rounds, pkd);
  } else {
    parlay::copy(WI, wi);
    pkd.BatchDiff(wi.cut(0, total_batch_size));
  }
}

// NOTE: run range count and check the correct
template<typename Point, typename Tree>
void
RangeCount(parlay::sequence<Point> const& wp,
           Tree& pkd,
           int const& rounds,
           int rec_num,
           int rec_type,
           int const DIM)
{
  // using Points = typename Tree::Points;
  // using Box = typename Tree::Box;

  auto [query_box_seq, max_size] =
    GenRectangles<Point, Tree, false>(rec_num, rec_type, wp, DIM);
  parlay::sequence<size_t> kdknn(rec_num, 0);

  double aveCount = time_loop(
    rounds,
    0.01,
    [&]() {},
    [&]() {
      parlay::parallel_for(0, rec_num, [&](size_t i) {
        auto [size, logger] = pkd.RangeCount(query_box_seq[i].first);
        kdknn[i] = size;
      });
    },
    [&]() {});

  // NOTE: verify the solutions
  std::cout << "check range count: " << rec_num << " " << rec_type << std::endl;
  for (int i = 0; i < rec_num; i++) {
    if (kdknn[i] != query_box_seq[i].second) {
      std::cout << kdknn[i] << " " << query_box_seq[i].second << " "
                << query_box_seq[i].first.first << " "
                << query_box_seq[i].first.second << std::endl;
    }
    assert(std::cmp_equal(kdknn[i], query_box_seq[i].second));
  }

  std::cout << aveCount << "\n" << std::flush;

  return;
}

template<typename Point, typename Tree>
void
rangeCountRadius(parlay::sequence<Point> const& wp,
                 Tree& pkd,
                 Typename* kdknn,
                 int const& rounds,
                 int const& queryNum)
{
  // using Points = typename Tree::Points;
  // using node = typename Tree::node;
  using Box = typename Tree::Box;
  using circle = typename Tree::circle;

  int n = wp.size();

  double aveCount = time_loop(
    rounds,
    0.01,
    [&]() {},
    [&]() {
      parlay::parallel_for(0, queryNum, [&](size_t i) {
        Box query_box_seq = pkd.get_box(
          Box(wp[i], wp[i]), Box(wp[(i + n / 2) % n], wp[(i + n / 2) % n]));
        auto d = Tree::p2p_distance(wp[i], wp[(i + n / 2) % n], wp[i].GetDim());
        d = static_cast<Coord>(std::sqrt(d));
        circle cl = circle(wp[i], d);
        kdknn[i] = pkd.range_count(cl);
      });
    },
    [&]() {});

  std::cout << aveCount << " " << std::flush;

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
    GenRectangles<TT, Tree, true, false>(rec_num, rec_type, wp);
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
template<typename Point, typename Tree>
void
rangeCountFix(Tree& pkd,
              Typename* kdknn,
              int const& rounds,
              int rec_type,
              int rec_num,
              int DIM,
              auto const& query_box_seq,
              auto max_size)
{
  // using Tree = Tree;
  // using Points = typename Tree::Points;
  // using Box = typename Tree::Box;

  // int n = WP.size();

  // auto [query_box_seq, max_size] =
  //     gen_rectangles<Point, Tree, false>(rec_num, rec_type, WP, DIM);
  parlay::sequence<size_t> vis_leaf(rec_num), vis_inter(rec_num),
    gen_box(rec_num), full_box(rec_num), skip_box(rec_num);

  double aveCount = time_loop(
    rounds,
    0.01,
    [&]() {},
    [&]() {
      parlay::parallel_for(0, rec_num, [&](size_t i) {
        auto [size, logger] = pkd.RangeCount(query_box_seq[i].first);

        kdknn[i] = size;
        vis_leaf[i] = logger.vis_leaf_num;
        vis_inter[i] = logger.vis_interior_num;
        gen_box[i] = logger.generate_box_num;
        full_box[i] = logger.full_box_num;
        skip_box[i] = logger.skip_box_num;
      });
      // for (int i = 0; i < rec_num; i++) {
      //     kdknn[i] = pkd.RangeCount(query_box_seq[i].first);
      // }
    },
    [&]() {});

  std::cout << aveCount << " " << std::flush;
  std::cout << static_cast<double>(parlay::reduce(vis_leaf.cut(0, rec_num))) /
                 static_cast<double>(rec_num)
            << " " << std::flush;
  std::cout << static_cast<double>(parlay::reduce(vis_inter.cut(0, rec_num))) /
                 static_cast<double>(rec_num)
            << " " << std::flush;
  std::cout << static_cast<double>(parlay::reduce(gen_box.cut(0, rec_num))) /
                 static_cast<double>(rec_num)
            << " " << std::flush;
  std::cout << static_cast<double>(parlay::reduce(full_box.cut(0, rec_num))) /
                 static_cast<double>(rec_num)
            << " " << std::flush;
  std::cout << static_cast<double>(parlay::reduce(skip_box.cut(0, rec_num))) /
                 static_cast<double>(rec_num)
            << " " << std::flush;

  return;
}
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
    parlay::delayed_tabulate(
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

template<typename E>
auto
ReadEntries(char const* iFile,
            parlay::sequence<E>& wp,
            [[maybe_unused]] int id_offset = 0) -> std::pair<size_t, int>
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
    wp[i].ReadKey(pts[i * dim]);
    wp[i].ReadVal(pts[(i * dim) + 1]); // WARN: assume #val = 1
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

template<typename TreeWrapper>
void
PrintTreeParam()
{
  std::cout << "Tree: " << TreeWrapper::TreeType::GetTreeName() << "; ";
  std::cout << "Buf: " << static_cast<int>(TreeWrapper::TreeType::TT::kBuf)
            << "; ";
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

    ReadEntries<typename TT::E>(opts.input_file_path.c_str(), wp, 0);
    BuildTree<TT, Tree, kTestTime, 2>(wp, opts.rounds, tree);

    // // NOTE: batch insert
    if (opts.tag & (1 << 0)) {
      if (opts.summary) {
        parlay::sequence<double> const ratio_seq = { 0.0001, 0.001, 0.01, 0.1 };
        for (auto ratio : ratio_seq) {
          auto [base_path, batch_path] = opts.ParseInsertBatchPath(ratio);
          // std::cout << base_path << " " << batch_path << std::endl;
          auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp, 0);
          auto [n2, d2] =
            ReadEntries<typename TT::E>(batch_path.c_str(), wi, n);
          BatchInsert<TT, Tree, kTestTime>(tree, wp, wi, opts.rounds);
        }
      } else {
        auto [base_path, batch_path] =
          opts.ParseInsertBatchPath(opts.batch_ratio);
        // std::cout << base_path << " " << batch_path << std::endl;
        auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp, 0);
        auto [n2, d2] = ReadEntries<typename TT::E>(batch_path.c_str(), wi, n);
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
          auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp, 0);
          auto [n2, d2] =
            ReadEntries<typename TT::E>(batch_path.c_str(), wi, n);
          BatchDelete<TT, Tree, kTestTime>(tree, wp, wi, opts.rounds);
        }
      } else {
        auto [base_path, batch_path] =
          opts.ParseDeleteBatchPath(opts.batch_ratio);
        // std::cout << base_path << " " << batch_path << std::endl;
        auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp, 0);
        auto [n2, d2] = ReadEntries<typename TT::E>(batch_path.c_str(), wi, n);
        BatchDelete<TT, Tree, kTestTime>(tree, wp, wi, opts.rounds);
      }
    }

    // NOTE: incremental build
    if (opts.tag & (1 << 2)) {
      parlay::sequence<double> const ratio_seq = { 0.0001, 0.001, 0.01, 0.1 };
      for (auto ratio : ratio_seq) {
        auto [base_path, batch_path] = opts.ParseInsertBatchPath(ratio);
        // std::cout << base_path << " " << batch_path << std::endl;
        auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp, 0);
        auto [n2, d2] = ReadEntries<typename TT::E>(batch_path.c_str(), wi, n);
        BatchInsertByStep<TT, Tree, kTestTime>(tree, wp, ratio, opts.rounds);
      }
    }

    auto [base_path, batch_path] = opts.ParseInsertBatchPath(kBatchQueryRatio);
    auto [n, d] = ReadEntries<typename TT::E>(base_path.c_str(), wp, wp.size());
    // auto [n2, d2] =
    //   ReadEntries<typename TT::E>(batch_path.c_str(), wi, wp.size());
    BuildTree<TT, Tree, kTestTime, 0>(wp, opts.rounds, tree);

    if (opts.query_type & (1 << 0)) { // NOTE: membership
      auto batch_size = static_cast<size_t>(wp.size() * kBatchQueryRatio);
      auto query_ele =
        wi.size() ? wi.subseq(0, batch_size) : wp.subseq(0, batch_size);
      auto res = parlay::sequence<bool>(query_ele.size(), true);
      Membership<TT>(tree, query_ele, opts.rounds, res);
    }

    if (opts.query_type & (1 << 1)) { // NOTE: range query
      for (int rec_type = 0; rec_type < kRangeQueryType; rec_type++) {
        auto [query_box_seq, max_size] =
          GenRectangles<TT, Tree, false, false>(kRangeQueryNum, rec_type, wp);
        typename TT::EntrySeq Out;
        RangeQueryFix<TT>(
          tree, opts.rounds, Out, kRangeQueryNum, query_box_seq);
      }
    }

    std::cout << "\n" << std::flush;

    tree.DeleteTree();

    return;
  };

struct Options
{
  Options(commandLine& params)
  {
    this->val_n = params.getOptionIntValue("-d", 1);
    this->input_file_path = std::string(params.getOptionValue("-p"));
    this->tag = params.getOptionIntValue("-t", 1);
    this->rounds = params.getOptionIntValue("-r", 3);
    this->query_type = params.getOptionIntValue("-q", 0);
    this->summary = params.getOptionIntValue("-s", 0);
    this->batch_ratio = params.getOptionDoubleValue("-b", 0.1);
    this->tree_type = params.getOptionIntValue("-T", 0);
    this->dis_type = std::string(params.getOptionValue("-D"));
  }

  void PrintOptions() const
  {
    std::cout << "InputFile: "
              << std::filesystem::path(input_file_path).filename().string()
              << "; ";
    std::cout << "Dis: " << this->dis_type << "; ";
  }

  static auto ExtractPrefixes(std::string const& path)
    -> std::pair<std::string, std::string>
  {
    // locate pivot: first occurrence of "/ins/" or "/del/"
    size_t posIns = path.find("/ins/");
    size_t posDel = path.find("/del/");
    size_t pivot = std::string::npos;
    if (posIns != std::string::npos) {
      pivot = posIns;
    }
    if (posDel != std::string::npos &&
        (pivot == std::string::npos || posDel < pivot)) {
      pivot = posDel;
    }

    if (pivot == std::string::npos) {
      // pattern not found
      return { "", "" };
    }

    // directory prefix (up to just before /ins/ or /del/)
    std::string dirPrefix = path.substr(0, pivot) + "/";

    // filename part
    size_t lastSlash = path.find_last_of('/');
    std::string filename =
      (lastSlash == std::string::npos) ? path : path.substr(lastSlash + 1);
    // strip extensions
    auto strip_suffix = [](std::string& s, std::string const& suf) {
      if (s.size() >= suf.size() &&
          s.compare(s.size() - suf.size(), suf.size(), suf) == 0) {
        s.erase(s.size() - suf.size());
      }
    };
    strip_suffix(filename, ".in");
    strip_suffix(filename, ".base");

    return { dirPrefix, filename };
  }

  [[nodiscard]] auto ParseInsertBatchPath(double batch_ratio) const
    -> std::pair<std::string, std::string>
  {
    // PARA: /ssd1/zmen002/kv_pair/uni/1000000_1/ins/100"
    // PARA: dir_pre: ../kdtree/uni/1000000_1/
    // PARA: file_pre: 1
    auto [dir_prefix, file_prefix] = ExtractPrefixes(this->input_file_path);
    std::string base_path =
      dir_prefix + "ins/" +
      std::to_string(static_cast<size_t>(1 / batch_ratio)) + "/" + file_prefix +
      ".base.in";

    std::string batch_path = base_path;
    batch_path.replace(batch_path.find(".base."), 6, ".batch.");
    return std::make_pair(base_path, batch_path);
  }

  [[nodiscard]] auto ParseDeleteBatchPath(double batch_ratio) const
    -> std::pair<std::string, std::string>
  {
    // NOTE: ../kdtree/uni/1000000_1/ins/10/1.base.in
    auto [dir_prefix, file_prefix] = ExtractPrefixes(this->input_file_path);
    std::string base_path =
      dir_prefix + "del/" +
      std::to_string(static_cast<size_t>(1 / batch_ratio)) + "/" + file_prefix +
      ".base.in";

    std::string batch_path = base_path;
    batch_path.replace(batch_path.find(".base."), 6, ".batch.");

    return std::make_pair(base_path, batch_path);
  }

  int val_n;
  std::string input_file_path = "";
  int tag;
  int rounds;
  int query_type;
  bool summary;
  double batch_ratio; // decides the default batch file path
  int tree_type;
  std::string dis_type;
};

// NOTE: driven functions
template<typename TreeWrapper, typename RunFunc>
static void
Run(Options const& opts, RunFunc test_func)
{
  using TT = typename TreeWrapper::TreeType::TT;
  using E = typename TT::E;
  using EntrySeq = typename TT::EntrySeq;

  PrintTreeParam<TreeWrapper>();
  opts.PrintOptions();
  std::cout << "\n" << std::flush;

  // Apply the test function
  test_func.template operator()<TreeWrapper, TT>(opts);
};

class Wrapper
{
public:
  struct LeafAugEmpty
  {
    LeafAugEmpty() = default;

    template<typename Slice>
    LeafAugEmpty(Slice In){};

    template<typename Slice>
    void UpdateAug(Slice In)
    {
    }

    void Reset()
    {
    }
  };

  struct InteriorAugEmpty
  {
    InteriorAugEmpty() = default;

    template<typename TreeNodes>
    static bool Create(TreeNodes const& /*nodes*/)
    {
      return true;
    }

    [[nodiscard]] static bool ForceParallel(size_t /*sz*/,
                                            size_t /*granularity*/)
    {
      return true;
    }

    template<typename TreeNodes, typename SplitSeq>
    void Update(TreeNodes const& /*nodes*/, SplitSeq const& /*splits*/)
    {
    }

    void Update()
    {
    }

    void Reset()
    {
    }
  };

  // NOTE: the element we use
  template<typename KeyType, typename ValType>
  struct Element
  {
    using K = KeyType;
    using V = ValType;

    K GetKey() const
    {
      return key;
    }

    V GetVal() const
    {
      return val;
    }

    template<typename StrRange>
    void ReadKey(StrRange str, size_t i)
    {
      this->key = { std::stol(str), i };
    }

    template<typename StrRange>
    void ReadKey(StrRange str)
    {
      this->key = std::stol(str);
    }

    template<typename StrRange>
    void ReadVal(StrRange In)
    {
      this->val = std::stol(In);
    }

    bool operator<(Element const& rhs) const
    {
      return this->key < rhs.key;
    }

    bool operator==(Element const& rhs) const
    {
      return this->key == rhs.key;
    }

    friend std::ostream& operator<<(std::ostream& o, Element const& a)
    {
      o << "(";
      o << a.key << " " << a.val;
      o << ") " << std::flush;
      return o;
    }

    K key;
    V val;
  };

  // NOTE: user needs to add a layer to expose API to b_tree
  template<typename EntryType>
  struct ElementWrapper
  {
    using E = EntryType;
    using K = typename E::K;
    using V = typename E::V;

    // differs from different entry, interface for pmos structure
    static auto GetKey(E const& e)
    {
      return e.GetKey();
    }
    static auto GetVal(E const& e)
    {
      return e.GetVal();
    }
    static E ToEntry(K const& k, V const& v)
    {
      return E{ .key = k, .val = v };
    };
  };

  using K = uint32_t;
  using V = uint32_t;
  using E = Element<K, V>;
};

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <iostream>

#include "baselines/cpam.h"
#include "baselines/pam.h"
#include "baselines/pma.h"
#include "baselines/spma.h"
#include "common/geometryIO.h"
#include "common/parse_command_line.h"
#include "common/time_loop.h"
#include "dependence/concepts.h"
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
get_random_index(size_t a, size_t b, [[maybe_unused]] int seed)
{
  return size_t((rand() % (b - a)) + a);
}

template<typename Point, typename Tree, bool SavePoint>
size_t
recurse_box(parlay::slice<Point*, Point*> In,
            auto& box_seq,
            int DIM,
            std::pair<size_t, size_t> range,
            int& idx,
            int rec_num,
            int type)
{
  size_t n = In.size();
  if (idx >= rec_num || n < range.first || n == 0)
    return 0;

  size_t mx = 0;
  bool goon = false;
  if (n <= range.second) {
    if constexpr (SavePoint) {
      box_seq[idx++] =
        std::make_pair(Tree::GetBox(In), parlay::to_sequence(In));
    } else {
      box_seq[idx++] = std::make_pair(Tree::GetBox(In), In.size());
    }

    // WARN: Modify the coefficient to make the rectangle size distribute as
    // uniform as possible within the range in lograthmic scale
    if (parlay::all_of(
          // if ((type == 0 && n < 40 * range.first) ||
          // (type == 1 && n < 10 * range.first) ||
          // (type == 2 && n < 2 * range.first) ||
          // parlay::all_of(
          In,
          [&](Point const& p) {
            return p.SameDimension(In[0]);
          })) {
      // NOTE: handle the cose that all Points are the same which is
      // undivideable
      return In.size();
    } else {
      goon = true;
      mx = n;
    }
  }

  int dim = get_random_index(0, DIM, rand());
  size_t pos = get_random_index(0, n, rand());
  parlay::sequence<bool> flag(n, 0);
  parlay::parallel_for(0, n, [&](size_t i) {
    if (pmos::Num_Comparator<Coord>::Gt(In[i][dim], In[pos][dim]))
      flag[i] = 1;
    else
      flag[i] = 0;
  });
  auto [Out, m] = parlay::internal::split_two(In, flag);

  assert(Out.size() == n);
  // std::cout << dim << " " << Out[0] << Out[m] << std::endl;
  size_t l = 0, r = 0;
  l = recurse_box<Point, Tree, SavePoint>(
    Out.cut(0, m), box_seq, DIM, range, idx, rec_num, type);
  r = recurse_box<Point, Tree, SavePoint>(
    Out.cut(m, n), box_seq, DIM, range, idx, rec_num, type);

  if (goon) {
    return mx;
  } else {
    return std::max(l, r);
  }
}

template<typename Point, typename Tree, bool SavePoint, bool FixSize = false>
auto
gen_rectangles(int rec_num,
               int const type,
               parlay::sequence<Point> const& WP,
               int DIM)
{
  using Points = typename Tree::Points;
  using Box = typename Tree::Box;
  using BoxSeq = std::conditional_t<SavePoint == false,
                                    parlay::sequence<std::pair<Box, size_t>>,
                                    parlay::sequence<std::pair<Box, Points>>>;

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
  Points wp(n);

  srand(10);

  // std::cout << " " << range.first << " " << range.second << std::endl;

  size_t max_size = 0;
  while (cnt < rec_num) {
    parlay::copy(WP, wp);
    auto r = recurse_box<Point, Tree, SavePoint>(
      parlay::make_slice(wp), box_seq, DIM, range, cnt, rec_num, type);
    max_size = std::max(max_size, r);
    // std::cout << cnt << " " << max_size << std::endl;
  }
  // std::cout << "finish generate " << std::endl;
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
      [&]() {
        wp = parsed_input;
      },
      [&]() {
        pkd.Build(wp);
      },
      [&]() {
        pkd.DeleteTree();
      });

    wp = parsed_input;
    pkd.Build(wp);
    std::cout << aveBuild << " " << std::flush;
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
      [&]() {
        pkd.BatchInsert(wi);
      },
      [&]() {
        pkd.DeleteTree();
      });
    std::cout << aveInsert << " " << std::flush;
    // std::cout << ratio << " " << aveInsert << " " << std::endl;
    BuildTree<TypeTrait, Tree, false>(WP, rounds, pkd);
  } else { // NOTE: insert the points from previous tree
    pkd.DeleteTree();
    pkd.Build(wp);
    std::cout << "Insert " << wi.size() << " elements\n" << std::flush;
    pkd.BatchInsert(wi);
    std::cout << "finish ccp insert" << std::endl;
  }
}

template<typename Point,
         typename Tree,
         bool kTestTime = true,
         bool kSerial = false>
void
BatchDelete(Tree& pkd,
            parlay::sequence<Point> const& WP,
            parlay::sequence<Point> const& WI,
            int const rounds,
            double ratio = 1.0)
{
  using Points = typename Tree::Points;
  Points wp = Points::uninitialized(WP.size());
  Points wi = Points::uninitialized(WP.size());
  size_t batchSize = static_cast<size_t>(WP.size() * ratio);
  double loop_late = rounds > 1 ? 0.01 : -100;

  if constexpr (kTestTime) {
    pkd.DeleteTree();
    double aveDelete = time_loop(
      rounds,
      loop_late,
      [&]() {
        BuildTree<Point, Tree, false>(WP, rounds, pkd);
        parlay::copy(WI, wi);
      },
      [&]() {
        pkd.BatchDelete(wi.cut(0, batchSize));
      },
      [&]() {
        pkd.DeleteTree();
      });
    std::cout << aveDelete << " " << std::flush;
    BuildTree<Point, Tree, false>(WP, rounds, pkd);
  } else {
    parlay::copy(WI, wi);
    pkd.BatchDelete(wi.cut(0, batchSize));
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
      [&]() {
        pkd.BatchDiff(wi.cut(0, total_batch_size));
      },
      [&]() {
        pkd.DeleteTree();
      });
    std::cout << aveDelete << " " << std::flush;
    BuildTree<Point, Tree, false>(WP, rounds, pkd);
  } else {
    parlay::copy(WI, wi);
    pkd.BatchDiff(wi.cut(0, total_batch_size));
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

template<typename Point, typename Tree, bool kInsert>
void
BatchInsertByStep(Tree& pkd,
                  parlay::sequence<Point> const& WP,
                  int const rounds,
                  double const insert_ratio,
                  int const remain_divide_ratio = 2)
{
  using Points = typename Tree::Points;
  using Box = typename Tree::Box;
  Points wp = Points::uninitialized(WP.size());
  size_t n = wp.size();
  size_t step = static_cast<size_t>(insert_ratio * n);
  size_t slice_num = n / step;
  parlay::sequence<parlay::sequence<double>> time_table(
    rounds + 2, parlay::sequence<double>(slice_num, 0.0));
  parlay::sequence<StepUpdateLogger> log_time(slice_num);
  int id_cnt = 0;
  for (auto& i : log_time) {
    i = { id_cnt++, 0.0 };
  }
  size_t round_cnt = 0;

  pkd.DeleteTree();

  // NOTE: build the tree by type
  auto prepare_build = [&]() {
    pkd.DeleteTree();
    if constexpr (pmos::IsKdTree<Tree> || pmos::IsPTree<Tree>) {
      parlay::copy(WP, wp);
    } else if constexpr (pmos::IsOrthTree<Tree>) {
      parlay::copy(WP, wp);
      auto box = Tree::GetBox(wp.cut(0, n));
      pkd.SetBoundingBox(box);
    } else {
      parlay::copy(WP, wp);
    }
  };

  auto incre_build = [&](int rounds_num_cutoff) {
    parlay::internal::timer t;
    size_t l = 0, r = 0;
    size_t cnt = 0;
    while (l < n) {
      if (cnt >= rounds_num_cutoff) {
        break;
      }

      r = std::min(l + step, n);
      // std::cout << l << " " << r << " " << n << std::endl;
      pkd.BatchInsert(parlay::make_slice(wp.begin() + l, wp.begin() + r));
      l = r;
      time_table[round_cnt][cnt++] += t.next_time();
    }
    round_cnt++;
  };

  double loop_late = rounds > 1 ? 0.01 : -100;
  double ave_time = time_loop(
    rounds,
    loop_late,
    [&]() {
      prepare_build();
    },
    [&]() {
      incre_build(slice_num + 1);
    },
    [&]() {
      pkd.DeleteTree();
    });

  // begin count the time
  if (rounds != 1 && round_cnt - 1 != rounds) {
    throw std::runtime_error("rounds not match!");
  }
  for (int i = 1; i <= rounds; i++) {
    for (int j = 0; j < slice_num; j++) {
      log_time[j].t += time_table[i][j];
    }
  }
  for (int j = 0; j < slice_num; j++) {
    log_time[j].t /= rounds;
  }

  puts("# Insert");
  std::cout << "## " << insert_ratio << std::endl;
  std::sort(log_time.begin(), log_time.end(), [](auto const& a, auto const& b) {
    return a.t < b.t;
  });

  // Calculate average time from log_time
  double total_time = 0.0;
  for (auto const& log : log_time) {
    total_time += log.t;
  }
  double average_time = total_time / log_time.size();

  std::cout << "median: " << log_time[slice_num / 2]
            << "-> min: " << *log_time.begin()
            << "-> max: " << *log_time.rbegin() << "-> tot: " << ave_time
            << "-> avg: " << average_time << std::endl;

  // WARN: restore status
  prepare_build();
  incre_build(slice_num / remain_divide_ratio);
  // auto original_box = Tree::GetBox(wp.cut(0, n / remain_divide_ratio));
  // auto tree_box = pkd.GetRootBox();
  // std::cout << original_box.first << ' ' << original_box.second <<
  // std::endl; std::cout << tree_box.first << ' ' << tree_box.second <<
  // std::endl; auto root = pkd.cpam_aug_map_; std::cout << "root size: " <<
  // root.size() << std::endl;

  return;
}

template<typename Point, typename Tree, bool kInsert>
void
BatchDeleteByStep(Tree& pkd,
                  parlay::sequence<Point> const& WP,
                  int const rounds,
                  double const insert_ratio,
                  size_t const remain_divide_ratio = 2)
{
  using Points = typename Tree::Points;
  using Box = typename Tree::Box;
  Points wp = Points::uninitialized(WP.size());
  size_t n = wp.size();
  size_t step = static_cast<size_t>(insert_ratio * n);
  size_t slice_num = n / step;
  // std::cout << "n: " << n << " step: " << step << " slice_num: " <<
  // slice_num
  // << std::endl;
  parlay::sequence<parlay::sequence<double>> time_table(
    rounds + 2, parlay::sequence<double>(slice_num, 0.0));
  parlay::sequence<StepUpdateLogger> log_time(slice_num);
  int id_cnt = 0;
  for (auto& i : log_time) {
    i = { id_cnt++, 0.0 };
  }
  size_t round_cnt = 0;

  pkd.DeleteTree();

  // NOTE: build the tree by type
  auto build_tree_by_type = [&]() {
    parlay::copy(WP, wp);

    if constexpr (pmos::IsKdTree<Tree> || pmos::IsPTree<Tree>) {
      pkd.Build(parlay::make_slice(wp));
    } else if constexpr (pmos::IsOrthTree<Tree>) {
      auto box = Tree::GetBox(wp.cut(0, n));
      pkd.Build(parlay::make_slice(wp), box);
    } else {
      pkd.Build(parlay::make_slice(wp));
    }

    parlay::copy(WP, wp);
  };

  auto incre_delete = [&](int rounds_num_cutoff, auto wp_slice) {
    parlay::internal::timer t;
    size_t l = 0, r = 0;
    size_t cnt = 0;
    while (l < n) {
      if (cnt >= rounds_num_cutoff) {
        break;
      }

      r = std::min(l + step, n);
      // WARN: r may exceeds the right bounds with offset
      pkd.BatchDelete(
        parlay::make_slice(wp_slice.begin() + l, wp_slice.begin() + r));
      l = r;
      time_table[round_cnt][cnt++] += t.next_time();
    }
    round_cnt++;
  };

  double loop_late = rounds > 1 ? 0.01 : -100;
  double ave_time = time_loop(
    rounds,
    loop_late,
    [&]() {
      build_tree_by_type();
    },
    [&]() {
      incre_delete(slice_num, wp.cut(0, slice_num * step));
    },
    [&]() {
      pkd.DeleteTree();
    });

  if (rounds != 1 && round_cnt - 1 != rounds) {
    throw std::runtime_error("rounds not match!");
  }
  for (int i = 1; i <= rounds; i++) {
    for (int j = 0; j < slice_num; j++) {
      log_time[j].t += time_table[i][j];
    }
  }
  for (int j = 0; j < slice_num; j++) {
    log_time[j].t /= rounds;
  }

  puts("# Delete");
  std::cout << "## " << insert_ratio << std::endl;
  // puts("");
  // std::cout << ave_time << " " << std::flush;
  std::sort(log_time.begin(), log_time.end(), [](auto const& a, auto const& b) {
    return a.t < b.t;
  });
  // Calculate average time from log_time
  double total_time = 0.0;
  for (auto const& log : log_time) {
    total_time += log.t;
  }
  double average_time = total_time / log_time.size();

  std::cout << "median: " << log_time[slice_num / 2]
            << "-> min: " << *log_time.begin()
            << "-> max: " << *log_time.rbegin() << "-> tot: " << ave_time
            << "-> avg: " << average_time << std::endl;

  // WARN: restore status
  pkd.DeleteTree();
  build_tree_by_type();
  incre_delete(slice_num / remain_divide_ratio,
               wp.cut(slice_num / remain_divide_ratio * step, n));

  return;
}

// template <typename Point, typename Tree, bool printHeight = 0,
//           bool printVisNode = 1>
// void queryKNN([[maybe_unused]] uint_fast8_t const& Dim,
//               parlay::sequence<Point> const& WP, int const& rounds, Tree&
//               pkd, Typename* kdknn, int const K, bool const flattenTreeTag)
//               {
//   using Points = typename Tree::Points;
//   using Coord = typename Point::Coord;
//   using DisType = typename Point::DisType;
//   using nn_pair = std::pair<std::reference_wrapper<Point>, DisType>;
//   using Leaf = typename Tree::Leaf;
//   using Interior = typename Tree::Interior;
//   // using nn_pair = std::pair<Point, DisType>;
//   size_t n = WP.size();
//   // int LEAVE_WRAP = 32;
//   double loopLate = rounds > 1 ? 0.01 : -0.1;
//   auto* KDParallelRoot = pkd.GetRoot();
//
//   Points wp = WP;
//   // Points wp = parlay::random_shuffle(WP);
//
//   parlay::sequence<nn_pair> Out(
//       K * n, nn_pair(std::ref(wp[0]), static_cast<DisType>(0)));
//   // parlay::sequence<nn_pair> Out(K * n);
//   parlay::sequence<kBoundedQueue<Point, nn_pair>> bq =
//       parlay::sequence<kBoundedQueue<Point, nn_pair>>::uninitialized(n);
//   parlay::parallel_for(
//       0, n, [&](size_t i) { bq[i].resize(Out.cut(i * K, i * K + K)); });
//   parlay::sequence<size_t> vis_leaf(n), vis_inter(n), gen_box(n),
//   check_box(n),
//       skip_box(n);
//
//   double ave_query = time_loop(
//       rounds, loopLate,
//       [&]() { parlay::parallel_for(0, n, [&](size_t i) { bq[i].reset(); });
//       },
//       [&]() {
//         // if (!flattenTreeTag) {  // WARN: Need ensure pkd.size() ==
//         wp.size()
//         //   pkd.Flatten(parlay::make_slice(wp));
//         // }
//         parlay::parallel_for(0, n, [&](size_t i) {
//           // for (size_t i = 0; i < n; i++) {
//           auto [vis_leaf_num, vis_inter_num, gen_box_num, check_box_num,
//                 skip_box_num] = pkd.KNN(KDParallelRoot, wp[i], bq[i]);
//           kdknn[i] = bq[i].top().second;
//           vis_leaf[i] = vis_leaf_num;
//           vis_inter[i] = vis_inter_num;
//           gen_box[i] = gen_box_num;
//           check_box[i] = check_box_num;
//           skip_box[i] = skip_box_num;
//           // }
//         });
//       },
//       [&]() {});
//
//   std::cout << ave_query << " " << std::flush;
//   if (printHeight) {
//     // WARN: change when using multi-node
//     size_t max_deep = 0;
//     std::cout << pkd.template GetMaxTreeDepth<Leaf,
//     Interior>(pkd.GetRoot(),
//                                                               max_deep)
//               << " " << pkd.template GetAveTreeHeight<Leaf, Interior>() <<
//               "
//               "
//               << std::flush;
//   }
//   if (printVisNode) {
//     std::cout << static_cast<double>(parlay::reduce(vis_leaf.cut(0, n))) /
//                      static_cast<double>(n)
//               << " " << std::flush;
//     std::cout << static_cast<double>(parlay::reduce(vis_inter.cut(0, n))) /
//                      static_cast<double>(n)
//               << " " << std::flush;
//     std::cout << static_cast<double>(parlay::reduce(gen_box.cut(0, n))) /
//                      static_cast<double>(n)
//               << " " << std::flush;
//     std::cout << static_cast<double>(parlay::reduce(check_box.cut(0, n))) /
//                      static_cast<double>(n)
//               << " " << std::flush;
//     std::cout << static_cast<double>(parlay::reduce(skip_box.cut(0, n))) /
//                      static_cast<double>(n)
//               << " " << std::flush;
//   }
//
//   return;
// }

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
    gen_rectangles<Point, Tree, false>(rec_num, rec_type, wp, DIM);
  parlay::sequence<size_t> kdknn(rec_num, 0);

  double aveCount = time_loop(
    rounds,
    0.01,
    [&]() {
    },
    [&]() {
      parlay::parallel_for(0, rec_num, [&](size_t i) {
        auto [size, logger] = pkd.RangeCount(query_box_seq[i].first);
        kdknn[i] = size;
      });
    },
    [&]() {
    });

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
    [&]() {
    },
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
    [&]() {
    });

  std::cout << aveCount << " " << std::flush;

  return;
}

// NOTE: run range query and check the correct
template<typename Point, typename Tree>
void
RangeQuery(parlay::sequence<Point> const& wp,
           Tree& pkd,
           int const& rounds,
           int const rec_num,
           int const rec_type,
           int const DIM)
{
  using Points = typename Tree::Points;

  auto [query_box_seq, max_size] =
    gen_rectangles<Point, Tree, true>(rec_num, rec_type, wp, DIM);
  auto [offset, tot_size] = parlay::scan(
    parlay::delayed_tabulate(rec_num,
                             [&](size_t i) -> size_t {
                               return query_box_seq[i].second.size();
                             }),
    parlay::addm<size_t>());
  offset.push_back(tot_size);
  Points Out(tot_size);
  parlay::sequence<size_t> kdknn(rec_num, 0);
  parlay::sequence<size_t> vis_leaf(rec_num), vis_inter(rec_num),
    gen_box(rec_num), full_box(rec_num), skip_box(rec_num);

  double ave_query = time_loop(
    rounds,
    0.01,
    [&]() {
    },
    [&]() {
      parlay::parallel_for(0, rec_num, [&](size_t i) {
        auto [size, logger] = pkd.RangeQuery(query_box_seq[i].first,
                                             Out.cut(offset[i], offset[i + 1]));
        kdknn[i] = size;
        vis_leaf[i] = logger.vis_leaf_num;
        vis_inter[i] = logger.vis_interior_num;
        gen_box[i] = logger.generate_box_num;
        full_box[i] = logger.full_box_num;
        skip_box[i] = logger.skip_box_num;
      });
    },
    [&]() {
    });

  std::cout << "check range query: " << rec_num << " " << rec_type << " "
            << max_size << std::endl;
  for (int i = 0; i < rec_num; i++) {
    if (kdknn[i] != query_box_seq[i].second.size()) {
      std::cout << kdknn[i] << " " << query_box_seq[i].second.size() << " "
                << query_box_seq[i].first.first << query_box_seq[i].first.second
                << std::endl;
      std::cout << vis_leaf[i] << " " << gen_box[i] << " " << full_box[i] << " "
                << skip_box[i] << std::endl;
    }
    assert(std::cmp_equal(kdknn[i], query_box_seq[i].second.size()));
    // std::cout << kdknn[i] << " " << query_box_seq[i].second.size() << " "
    //     << query_box_seq[i].first.first << query_box_seq[i].first.second
    //     << std::endl;
    parlay::sort_inplace(Out.cut(offset[i], offset[i + 1]),
                         [&](auto const& a, auto const& b) {
                           return a.aug.id < b.aug.id;
                         });
    parlay::sort_inplace(query_box_seq[i].second,
                         [&](auto const& a, auto const& b) {
                           return a.aug.id < b.aug.id;
                         });
    for (size_t j = 0; j < query_box_seq[i].second.size(); j++) {
      if (Out[offset[i] + j] != query_box_seq[i].second.at(j)) {
        std::cout << "wrong" << query_box_seq[i].first.first
                  << query_box_seq[i].first.second << std::endl;
        std::cout << Out[offset[i] + j] << " " << query_box_seq[i].second.at(j)
                  << std::endl;
      }

      if constexpr (IsKdTree<Tree> ||
                    IsOrthTree<Tree>) { // TODO: fix this by enable kdtree
                                        // handling duplicates by id
        // assert(Out[offset[i] +
        // j].SameDimension(query_box_seq[i].second.at(j)));
        assert(Out[offset[i] + j] == query_box_seq[i].second.at(j));
      } else if constexpr (IsPTree<Tree>) {
        assert(Out[offset[i] + j] == query_box_seq[i].second.at(j));
      }

      // if (Out[i * step + j] != query_box_seq[i].second.at(j)) std::cout <<
      // "wrong
      // "; std::cout << Out[j] << " " << query_box_seq[i].second.at(j) <<
      // std::endl;
    }
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
    [&]() {
    },
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
    [&]() {
    });

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
    [&]() {
    },
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
    [&]() {
    });

  // std::cout << query_num << " ";
  std::cout << ave_query << " " << std::flush;
}

template<typename Point, typename Tree>
void
rangeQueryFix(Tree& pkd,
              Typename* kdknn,
              int const& rounds,
              parlay::sequence<Point>& Out,
              int rec_type,
              int rec_num,
              int DIM,
              auto const& query_box_seq,
              auto max_size)
{
  // auto [query_box_seq, max_size] =
  //     gen_rectangles<Point, Tree, false>(rec_num, rec_type, WP, DIM);
  parlay::sequence<size_t> vis_leaf(rec_num), vis_inter(rec_num),
    gen_box(rec_num), full_box(rec_num), skip_box(rec_num);
  auto [offset, tot_size] =
    parlay::scan(parlay::delayed_tabulate(rec_num,
                                          [&](size_t i) -> size_t {
                                            return query_box_seq[i].second;
                                          }),
                 parlay::addm<size_t>());
  offset.push_back(tot_size);
  // std::cout << "range query: " << rec_num << " " << rec_type << " " <<
  // tot_size
  //           << std::endl;
  Out.resize(tot_size);

  // int n = WP.size();
  // size_t step = Out.size() / rec_num;
  // using ref_t = std::reference_wrapper<Point>;
  // parlay::sequence<ref_t> out_ref( Out.size(), std::ref( Out[0] ) );

  double ave_query = time_loop(
    rounds,
    0.01,
    [&]() {
    },
    [&]() {
      parlay::parallel_for(0, rec_num, [&](size_t i) {
        auto [size, logger] = pkd.RangeQuery(query_box_seq[i].first,
                                             Out.cut(offset[i], offset[i + 1]));

        kdknn[i] = size;
        vis_leaf[i] = logger.vis_leaf_num;
        vis_inter[i] = logger.vis_interior_num;
        gen_box[i] = logger.generate_box_num;
        full_box[i] = logger.full_box_num;
        skip_box[i] = logger.skip_box_num;
      });
    },
    [&]() {
    });

  std::cout << ave_query << " " << std::flush;
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
  auto [offset, tot_size] =
    parlay::scan(parlay::delayed_tabulate(rec_num,
                                          [&](size_t i) -> size_t {
                                            return query_box_seq[i].second;
                                          }),
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
read_points(char const* iFile,
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
read_points_serial(char const* iFile,
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

    read_points<typename TT::E>(opts.input_file_path.c_str(), wp, 0);
    BuildTree<TT, Tree, kTestTime, 2>(wp, opts.rounds, tree);

    // // NOTE: batch insert
    if (opts.tag & (1 << 0)) {
      if (opts.summary) {
        parlay::sequence<double> const ratio_seq = { 0.0001, 0.001, 0.01, 0.1 };
        for (auto ratio : ratio_seq) {
          auto [base_path, batch_path] = opts.ParseInsertBatchPath(ratio);
          // std::cout << base_path << " " << batch_path << std::endl;
          auto [n, d] = read_points<typename TT::E>(base_path.c_str(), wp, 0);
          auto [n2, d2] =
            read_points<typename TT::E>(batch_path.c_str(), wi, n);
          BatchInsert<TT, Tree, kTestTime>(tree, wp, wi, opts.rounds);
        }
      } else {
        auto [base_path, batch_path] =
          opts.ParseInsertBatchPath(opts.batch_ratio);
        // std::cout << base_path << " " << batch_path << std::endl;
        auto [n, d] = read_points<typename TT::E>(base_path.c_str(), wp, 0);
        auto [n2, d2] = read_points<typename TT::E>(batch_path.c_str(), wi, n);
        BatchInsert<TT, Tree, kTestTime>(tree, wp, wi, opts.rounds);
      }
    }
    //
    // // NOTE: batch delete
    // if (kTag & (1 << 1)) {
    //   if (kSummary) {
    //     parlay::sequence<double> const ratios = {0.0001, 0.001, 0.01,
    //     0.1}; for (size_t i = 0; i < ratios.size(); i++) {
    //       BatchDelete<Point, Tree, kTestTime>(tree, wp, wp, opts.rounds,
    //       ratios[i]);
    //     }
    //   } else {
    //     BatchDelete<Point, Tree, kTestTime>(tree, wp, wp, opts.rounds,
    //                                         kBatchInsertRatio);
    //   }
    // }

    if (opts.query_type & (1 << 0)) { // NOTE: membership
      auto [base_path, batch_path] =
        opts.ParseInsertBatchPath(kBatchQueryRatio);
      auto [n2, d2] =
        read_points<typename TT::E>(batch_path.c_str(), wi, wp.size());
      auto batch_size = static_cast<size_t>(wp.size() * kBatchQueryRatio);
      auto query_ele =
        wi.size() ? wi.subseq(0, batch_size) : wp.subseq(0, batch_size);
      auto res = parlay::sequence<bool>(query_ele.size(), true);
      Membership<TT>(tree, query_ele, opts.rounds, res);
    }

    // Typename* kdknn = nullptr;
    // auto run_batch_knn = [&](Points const& query_pts, int kth) {
    //   kdknn = new Typename[query_pts.size()];
    //   queryKNN<Point>(kDim, query_pts, kRounds, tree, kdknn, kth, true);
    //   delete[] kdknn;
    // };
    //
    // auto generate_query_box = [&](int rec_num, int rec_total_type,
    //                               Points const& wp) {
    //   // NOTE: generate rectangles for the first half of the points
    //   parlay::sequence<parlay::sequence<std::pair<typename Tree::Box,
    //   size_t>>>
    //       query_box_seq(rec_total_type);
    //   parlay::sequence<size_t> query_max_size(rec_total_type);
    //   for (int i = 0; i < rec_total_type; i++) {
    //     auto [query_box, max_size] =
    //         gen_rectangles<Point, Tree, false, true>(rec_num, i, wp,
    //         kDim);
    //     query_box_seq[i] = query_box;
    //     query_max_size[i] = max_size;
    //   }
    //   return std::make_pair(query_box_seq, query_max_size);
    // };
    //
    // auto incre_update_test_bundle = [&](auto const& query_box_seq,
    //                                     auto const& query_max_size) {
    //   // NOTE: knn query
    //   {
    //     int k[3] = {1, 10, 100};
    //
    //     std::cout << "in-dis-skewed knn time: ";
    //     size_t batch_size = static_cast<size_t>(wp.size() *
    //     kBatchQueryRatio); for (int i = 0; i < 3; i++) {
    //       run_batch_knn(wp.subseq(0, batch_size), k[i]);
    //     }
    //     puts("");
    //
    //     std ::cout << "out-dis-skewed knn time: ";
    //     for (int i = 0; i < 3; i++) {
    //       run_batch_knn(wp.subseq(wp.size() - batch_size, wp.size()),
    //       k[i]);
    //     }
    //     puts("");
    //
    //     // NOTE: sample points within the whole input datasets
    //     auto query_pts =
    //         parlay::pack(wp, parlay::tabulate(wp.size(), [&](size_t i) ->
    //         bool {
    //                        return i % (wp.size() / (batch_size * 2)) ==
    //                        0;
    //                      }));
    //
    //     std::cout << "in-dis-uniform knn time: ";
    //     for (int i = 0; i < 3; i++) {
    //       run_batch_knn(parlay::random_shuffle(query_pts.subseq(0,
    //       batch_size)),
    //                     k[i]);
    //     }
    //     puts("");
    //
    //     std::cout << "out-dis-uniform knn time: ";
    //     for (int i = 0; i < 3; i++) {
    //       run_batch_knn(parlay::random_shuffle(
    //                         query_pts.subseq(batch_size,
    //                         query_pts.size())),
    //                     k[i]);
    //     }
    //     puts("");
    //   }
    //
    //   // NOTE: range count
    //   {
    //     int rec_num = query_box_seq[0].size();
    //     kdknn = new Typename[rec_num];
    //
    //     std::cout << "range count time: ";
    //     for (int i = 0; i < 3; i++) {
    //       rangeCountFix<Point>(tree, kdknn, kRounds, i, rec_num, kDim,
    //                            query_box_seq[i], query_max_size[i]);
    //     }
    //     delete[] kdknn;
    //     puts("");
    //   }
    //
    //   // NOTE: range query
    //   {
    //     int rec_num = query_box_seq[0].size();
    //     kdknn = new Typename[rec_num];
    //
    //     std::cout << "range query time: ";
    //     for (int i = 0; i < 3; i++) {
    //       Points Out;
    //       rangeQueryFix<Point>(tree, kdknn, kRounds, Out, i, rec_num,
    //       kDim,
    //                            query_box_seq[i], query_max_size[i]);
    //     }
    //     delete[] kdknn;
    //     puts("");
    //   }
    // };
    //
    // // NOTE: scalability
    // if (kTag & (1 << 2)) {
    //   puts("");
    //   BuildTree<Point, Tree, kTestTime, 0>(wp, kRounds, tree);
    //   BatchInsert<Point, Tree, kTestTime>(tree, wp, wi, kRounds,
    //                                       kBatchInsertRatio);
    //   BatchDelete<Point, Tree, kTestTime>(tree, wp, wp, kRounds,
    //                                       kBatchInsertRatio);
    // }
    //
    // // NOTE: batch insert by step
    // if (kTag & (1 << 3)) {
    //   puts("");
    //   BuildTree<Point, Tree, kTestTime, 3>(wp, kRounds, tree, 2);
    //
    //   auto [query_box_seq, query_max_size] =
    //       generate_query_box(kRangeQueryNum, 3, wp.subseq(0, wp.size() /
    //       2));
    //
    //   incre_update_test_bundle(query_box_seq, query_max_size);
    //
    //   parlay::sequence<double> const ratios = {0.1, 0.01, 0.001, 0.0001};
    //   for (auto rat : ratios) {
    //     BatchInsertByStep<Point, Tree, true>(tree, wp, kRounds, rat);
    //     incre_update_test_bundle(query_box_seq, query_max_size);
    //   }
    // }
    //
    // // NOTE: batch delete by step
    // if (kTag & (1 << 4)) {
    //   puts("");
    //   auto [query_box_seq, query_max_size] =
    //       generate_query_box(kRangeQueryNum, 3, wp.subseq(0, wp.size() /
    //       2));
    //   BuildTree<Point, Tree, kTestTime, 3>(wp, kRounds, tree, 2);
    //   incre_update_test_bundle(query_box_seq, query_max_size);
    //
    //   parlay::sequence<double> const ratios = {0.1, 0.01, 0.001, 0.0001};
    //   // parlay::sequence<double> const ratios = {0.001};
    //   for (auto rat : ratios) {
    //     BatchDeleteByStep<Point, Tree, true>(tree, wp, kRounds, rat);
    //     incre_update_test_bundle(query_box_seq, query_max_size);
    //     // incre_update_test_bundle(wp.subseq(0, wp.size() / 2));
    //   }
    // }
    //
    // // real world
    // if (kTag & (1 << 5)) {
    //   puts("");
    //   auto [query_box_seq, query_max_size] =
    //       generate_query_box(kRangeQueryNum, 3, wp.subseq(0, wp.size() /
    //       2));
    //
    //   BuildTree<Point, Tree, kTestTime, 3>(wp, kRounds, tree, 2);
    //   incre_update_test_bundle(query_box_seq, query_max_size);
    //
    //   BatchInsertByStep<Point, Tree, true>(tree, wp, kRounds, 0.0001);
    //   incre_update_test_bundle(query_box_seq, query_max_size);
    //
    //   BatchDeleteByStep<Point, Tree, true>(tree, wp, kRounds, 0.0001);
    //   incre_update_test_bundle(query_box_seq, query_max_size);
    // }
    //
    // // range query with log
    // if (kTag & (1 << 6)) {
    //   puts("");
    //   BatchInsertByStep<Point, Tree, true>(tree, wp, kRounds, 0.0001);
    //
    //   auto [query_box_seq, query_max_size] = generate_query_box(
    //       kSingleQueryLogRepeatNum, 3, wp.subseq(0, wp.size() / 2));
    //
    //   // NOTE: range query
    //   {
    //     int rec_num = kSingleQueryLogRepeatNum;
    //     kdknn = new Typename[rec_num];
    //
    //     std::cout << "range query time: " << std::endl;
    //     for (int i = 0; i < 3; i++) {
    //       // std::cout << "range query time: " << std::endl;
    //       Points Out;
    //       // rangeQueryFix<Point>(tree, kdknn, kRounds, Out, i, rec_num,
    //       kDim,
    //       //                      query_box_seq[i], query_max_size[i]);
    //       RangeQuerySerialWithLog<Point>(tree, kdknn, kRounds, Out, i,
    //       rec_num,
    //                                      kDim, query_box_seq[i],
    //                                      query_max_size[i]);
    //     }
    //     delete[] kdknn;
    //     puts("");
    //   }
    // }
    //
    // // range query with log
    // if (kTag & (1 << 7)) {
    //   puts("");
    //   parlay::sequence<double> const ratios = {
    //       0.0001, 0.0002, 0.0005, 0.001, 0.002, 0.005, 0.01,
    //       0.02,   0.05,   0.1,    0.2,   0.5,   1.0};
    //   std::cout << "Insert: ";
    //   for (size_t i = 0; i < ratios.size(); i++) {
    //     BatchInsert<Point, Tree, kTestTime>(tree, wp, wi, kRounds,
    //     ratios[i]);
    //   }
    //   std::cout << std::endl;
    //   std::cout << "Delete: ";
    //   for (size_t i = 0; i < ratios.size(); i++) {
    //     BatchDelete<Point, Tree, kTestTime>(tree, wp, wp, kRounds,
    //     ratios[i]);
    //   }
    //   puts("");
    // }
    // // WARN: compress the kdnode to MultiNode, should remove except for
    // // exp if constexpr (IsKdTree<Tree>) {
    // //   tree.Compress2Multi();
    // // }
    //
    // // BatchInsertByStep<Point, Tree, true>(tree, wp, kRounds,
    // 0.000000001);
    // if (kQueryType & (1 << 0)) {  // NOTE: KNN
    //   size_t batch_size = static_cast<size_t>(wp.size() *
    //   kBatchQueryRatio);
    //
    //   if (kSummary == 0) {
    //     int k[3] = {1, 10, 100};
    //     for (int i = 0; i < 3; i++) {
    //       run_batch_knn(wp.subseq(0, batch_size), k[i]);
    //     }
    //   } else {  // test kSummary
    //     run_batch_knn(wp.subseq(0, batch_size), K);
    //   }
    // }
    //
    // if (kQueryType & (1 << 1)) {  // NOTE: range count
    //   auto [query_box_seq, query_max_size] =
    //       generate_query_box(kRangeQueryNum, 3, wp);
    //
    //   if (!kSummary) {
    //     int recNum = kRangeQueryNum;
    //     kdknn = new Typename[recNum];
    //
    //     // std::cout << std::endl;
    //     for (int i = 0; i < 3; i++) {
    //       rangeCountFix<Point>(tree, kdknn, kRounds, i, recNum, kDim,
    //                            query_box_seq[i], query_max_size[i]);
    //     }
    //
    //     delete[] kdknn;
    //   }
    // }
    //
    // if (kQueryType & (1 << 2)) {  // NOTE: range query
    //   if (kSummary == 0) {
    //     auto [query_box_seq, query_max_size] =
    //         generate_query_box(kRangeQueryNum, 3, wp);
    //
    //     int recNum = kRangeQueryNum;
    //     kdknn = new Typename[recNum];
    //
    //     for (int i = 0; i < 3; i++) {
    //       Points Out;
    //       rangeQueryFix<Point>(tree, kdknn, kRounds, Out, i, recNum,
    //       kDim,
    //                            query_box_seq[i], query_max_size[i]);
    //     }
    //     delete[] kdknn;
    //   } else if (kSummary == 1) {  // NOTE: for kSummary
    //     auto [query_box_seq, query_max_size] =
    //         generate_query_box(kSummaryRangeQueryNum, 3, wp);
    //
    //     kdknn = new Typename[kSummaryRangeQueryNum];
    //     Points Out;
    //     rangeQueryFix<Point>(tree, kdknn, kRounds, Out, 2,
    //     kSummaryRangeQueryNum,
    //                          kDim, query_box_seq[2], query_max_size[2]);
    //     delete[] kdknn;
    //   }
    // }

    std::cout << "\n" << std::flush;

    tree.DeleteTree();

    return;
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
    InteriorAugEmpty()
    {
      force_par_indicator.reset();
    }
    InteriorAugEmpty(bool /*flag*/)
    {
      force_par_indicator.reset();
    }

    // use a bool to reload default constructor
    // template <typename Leaf, typename Interior>
    // static bool Create(Node* l, Node* r) {
    //   return true;
    // }

    template<typename TreeNodes>
    static bool Create(TreeNodes const& /*nodes*/)
    {
      return true;
    }

    void SetParallelFlag(bool const flag)
    {
      this->force_par_indicator.emplace(flag);
    }

    void ResetParallelFlag()
    {
      this->force_par_indicator.reset();
    }

    [[nodiscard]] bool GetParallelFlagIniStatus() const
    {
      return this->force_par_indicator.has_value();
    }

    [[nodiscard]] bool ForceParallel(size_t sz, size_t granularity) const
    {
      return this->force_par_indicator.has_value()
               ? this->force_par_indicator.value()
               : sz > granularity;
    }

    // template <typename Leaf, typename Interior>
    // void Update(Node*, Node*) {
    //   return;
    // }

    template<typename TreeNodes, typename SplitSeq>
    void Update(TreeNodes const& /*nodes*/, SplitSeq const& /*splits*/)
    {
      return;
    }

    void Update()
    {
      return;
    }

    void Reset()
    {
      this->force_par_indicator.reset();
    }

    // NOTE: use a tri-state bool to indicate whether a subtree needs to be
    // rebuilt. If aug is not INITIALIZED, then it means there is no need to
    // rebuild; otherwise, the value depends on the initial tree size before
    // rebuilding.
    std::optional<bool> force_par_indicator;
  };

  // NOTE: Trees
  template<typename TypeTrait>
  struct BTreeWrapper
  {
    using TreeType = typename pmos::BTree<TypeTrait>;
  };
  template<typename TypeTrait>
  struct CpamWrapper
  {
    using TreeType = typename CPAMTree::CpamRaw<TypeTrait>;
  };
  template<typename TypeTrait>
  struct PamWrapper
  {
    using TreeType = typename PAMTree::PamRaw<TypeTrait>;
  };
  template<typename TypeTrait>
  struct PmaWrapper
  {
    using TreeType = typename PMA::PmaWrapper<TypeTrait>;
  };
  template<typename TypeTrait>
  struct SpmaWrapper
  {
    using TreeType = typename SPMA::SpmaWrapper<TypeTrait>;
  };

  // NOTE: Apply the dim and split rule
  struct AugId
  {
    using IdType = int;
    IdType id;

    bool operator<(AugId const& rhs) const
    {
      return id < rhs.id;
    }
    bool operator==(AugId const& rhs) const
    {
      return id == rhs.id;
    }
    friend std::ostream& operator<<(std::ostream& os, AugId const& rhs)
    {
      os << rhs.id;
      return os;
    }
  };

  // For the spacial filling curve, we use the AugIdCode to
  // ensure the id is unique and the code is used to determine the
  // order of the points in the tree.
  struct AugIdCode
  {
    using IdType = int_fast32_t;
    using CurveCode = uint64_t;

    AugIdCode()
      : code(0)
      , id(0)
    {
    }

    void SetMember(CurveCode const& val)
    {
      code = val;
    }

    bool operator<(AugIdCode const& rhs) const
    {
      return code == rhs.code ? id < rhs.id : code < rhs.code;
    }

    bool operator==(AugIdCode const& rhs) const
    {
      // return code == rhs.code && id == rhs.id;
      // WARN: code is not important, we only need to ensure the id
      return id == rhs.id;
    }

    friend std::ostream& operator<<(std::ostream& os, AugIdCode const& rhs)
    {
      os << rhs.code << " " << rhs.id;
      return os;
    }

    CurveCode code;
    IdType id;
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
        std::to_string(static_cast<size_t>(1 / batch_ratio)) + "/" +
        file_prefix + ".base.in";

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
        std::to_string(static_cast<size_t>(batch_ratio * 100)) + "/" +
        file_prefix + ".base.in";

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
  static void Run(Options const& opts, RunFunc test_func)
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
    static auto GetKey(E const e)
    {
      return e.GetKey();
    }
    static auto GetVal(E const e)
    {
      return e.GetVal();
    }
    static E ToEntry(K const& k, V const& v)
    {
      return E{ .key = k, .val = v };
    };
  };

  // NOTE: For kd tree and orth tree
  template<typename RunFunc>
  static void ApplyOrthogonal(Options const& opts, RunFunc test_func)
  {
    // using K = std::pair<uint32_t, uint32_t>;
    using K = uint32_t;
    using V = uint32_t;
    using E = Element<K, V>;
    // using EleType = Element<uint32_t, uint32_t>;
#ifdef LEAF1
    static constexpr uint const kBuf = 1;
#elif defined(LEAF2)
    static constexpr uint const kBuf = 2;
#elif defined(LEAF4)
    static constexpr uint const kBuf = 4;
#elif defined(LEAF8)
    static constexpr uint const kBuf = 8;
#elif defined(LEAF16)
    static constexpr uint const kBuf = 16;
#elif defined(LEAF32)
    static constexpr uint const kBuf = 32;
#elif defined(LEAF64)
    static constexpr uint const kBuf = 64;
#elif defined(LEAF128)
    static constexpr uint const kBuf = 128;
#elif defined(LEAF256)
    static constexpr uint const kBuf = 256;
#elif defined(LEAF512)
    static constexpr uint const kBuf = 512;
#elif defined(LEAF1024)
    static constexpr uint const kBuf = 1024;
#else
    static constexpr uint const kBuf = 32; // Default
#endif // LEAF1

    using TypeTrait = pmos::BTreeTypeTrait<ElementWrapper<E>,
                                           kBuf,
                                           LeafAugEmpty,
                                           InteriorAugEmpty,
                                           8,
                                           pmos::ProtoNode>;
    if (opts.tree_type == 0) {
      Run<BTreeWrapper<TypeTrait>>(opts, test_func);
    }
  }

  template<typename RunFunc>
  static void ApplyBaselines(Options const& opts, RunFunc test_func)
  {
    // using K = std::pair<uint32_t, uint32_t>;
    using K = uint32_t;
    using V = uint32_t;
    using E = Element<K, V>;
    // using EleType = Element<uint32_t, uint32_t>;
    using TypeTrait = pmos::BTreeTypeTrait<ElementWrapper<E>,
                                           16,
                                           LeafAugEmpty,
                                           InteriorAugEmpty,
                                           8,
                                           pmos::ProtoNode>;
    if (opts.tree_type == 0) {
      // B-tree
    } else if (opts.tree_type == 1) {
      Run<CpamWrapper<TypeTrait>>(opts, test_func);
    } else if (opts.tree_type == 2) {
      Run<PamWrapper<TypeTrait>>(opts, test_func);
    } else if (opts.tree_type == 3) {
      Run<PmaWrapper<TypeTrait>>(opts, test_func);
    } else if (opts.tree_type == 4) {
      Run<SpmaWrapper<TypeTrait>>(opts, test_func);
    }
  }
};

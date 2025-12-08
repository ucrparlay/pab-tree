#include <iterator>
#include <set>
#include <tuple>

#include "test_framework.h"

size_t maxReduceSize = 0;
int const kCCPQueryNum = 10000;
size_t const kCCPBatchQuerySize = 1000000;
// size_t const kCCPBatchQuerySize = 110;
double const kBatchInsertCheckRatio = 0.1;
double const kCCPBatchDiffTotalRatio = 1.0;
double const kCCPBatchDiffOverlapRatio = 0.5;

template <typename TT, typename TreeDesc>
void runKDParallel(auto const& wp, auto const& wi, int query_num,
                   int const query_type, int const K, int const tag,
                   int const rounds) {
  using Tree = TreeDesc::TreeType;
  using EntrySeq = typename TT::EntrySeq;

  Tree tree;
  constexpr bool kTestTime = false;

  BuildTree<TT, Tree, kTestTime, 2>(wp, rounds, tree);
  tree.template Validate<typename Tree::Leaf, typename Tree::Interior>();

  puts("---------------finish build tree---------------");

  if (tag & (1 << 0)) {
    BatchInsert<TT, Tree, kTestTime>(tree, wp, wi, 2, kBatchInsertCheckRatio);
    assert(tree.GetSize() ==
           wp.size() + static_cast<size_t>(wi.size() * kBatchInsertCheckRatio));
    tree.template Validate<typename Tree::Leaf, typename Tree::Interior>();
    std::cout << "---------------finish insert----------------\n" << std::flush;
  }

  // if (tag &
  //     (1 << 1)) {  // NOTE: without tag & (1<<0), this will remove no points
  //   BatchDelete<Point, Tree, kTestTime>(tree, wp, wi, 2,
  //   batchInsertCheckRatio); tree.template Validate<typename Tree::Leaf,
  //   typename Tree::Interior,
  //                          typename Tree::SplitRuleType>();
  //   std::cout << "---------------finish delete----------------\n" <<
  //   std::flush;
  // }

  // if (tag & (1 << 2)) {
  //   BatchDiff<Point, Tree, kTestTime>(tree, wp, 2, kCCPBatchDiffTotalRatio,
  //                                     kCCPBatchDiffOverlapRatio);
  //   // BatchDiff<Point, Tree, kTestTime>(tree, wp, 2, 0.5, 1.0);
  //   tree.template Validate<typename Tree::Leaf, typename Tree::Interior,
  //                          typename Tree::SplitRuleType>();
  //   assert(tree.GetRoot()->size ==
  //          wp.size() - static_cast<size_t>(wp.size() *
  //          kCCPBatchDiffTotalRatio *
  //                                          kCCPBatchDiffOverlapRatio));
  //   std::cout << "---------------finish diff------------------\n" <<
  //   std::flush;
  // }

  // auto knn_checker = [&](Points const& wp, Points const& wi,
  //                        Points const query_points) {
  //   // assert(query_points.size() == kCCPBatchQuerySize);
  //   queryKNN<Point>(kDim, query_points, rounds, tree, kdknn, K, true);
  //   std::cout << "run cgal\n" << std::flush;
  //   runCGAL<Point>(wp, wi, query_points, cgknn, query_num, tag, query_type,
  //   K); std::cout << "check NN\n" << std::flush; size_t S =
  //   query_points.size(); for (size_t i = 0; i < S; i++) {
  //     if (std::abs(static_cast<double>(cgknn[i] - kdknn[i])) > 1e-4) {
  //       puts("");
  //       puts("wrong");
  //       std::cout << i << " " << cgknn[i] << " " << kdknn[i] << "\n"
  //                 << std::flush;
  //       abort();
  //     }
  //   }
  //   std::cout << "--------------finish NN query------------------\n"
  //             << std::flush;
  // };

  // if (tag & (1 << 3)) {  // incre insert
  //   parlay::sequence<double> const ratios = {0.001};
  //   for (auto rat : ratios) {
  //     BatchInsertByStep<Point, Tree, true>(tree, wp, rounds, rat);
  //     std::cout << "root size: " << tree.GetRoot()->size << std::endl;
  //   }
  //   tree.template Validate<typename Tree::Leaf, typename Tree::Interior,
  //                          typename Tree::SplitRuleType>();
  //   std::cout << "---------------finish incre build------------------\n"
  //             << std::flush;
  //   knn_checker(wp.subseq(0, wp.size() / 2), wi,
  //               wp.subseq(0, kCCPBatchQuerySize));
  //   knn_checker(wp.subseq(0, wp.size() / 2), wi,
  //               wp.subseq(wp.size() - kCCPBatchQuerySize, wp.size()));
  // }

  // if (tag & (1 << 4)) {  // incre delete
  //   parlay::sequence<double> const ratios = {0.001};
  //   for (auto rat : ratios) {
  //     BatchDeleteByStep<Point, Tree, true>(tree, wp, rounds, rat);
  //   }
  //   tree.template Validate<typename Tree::Leaf, typename Tree::Interior,
  //                          typename Tree::SplitRuleType>();
  //   std::cout << "---------------finish incre delete------------------\n"
  //             << std::flush;
  //   knn_checker(wp.subseq(0, wp.size() / 2), wi,
  //               wp.subseq(wp.size() - kCCPBatchQuerySize, wp.size()));
  //   knn_checker(wp.subseq(0, wp.size() / 2), wi,
  //               wp.subseq(0, kCCPBatchQuerySize));
  // }

  EntrySeq new_wp;
  if (tag == (1 << 0)) {  // insert
    new_wp = wp;
    new_wp.append(
        wi.cut(0, static_cast<size_t>(wi.size() * kBatchInsertCheckRatio)));
  } else if (tag == (1 << 1)) {
    new_wp = wp.subseq(static_cast<size_t>(wi.size() * kBatchInsertCheckRatio),
                       wp.size());
  } else if (tag & (1 << 2)) {  // WARN: this assume before we run diff, the
                                // tree should conation wp, e.g., just after
                                // build or build->insert->delete
    size_t total_batch_size =
        static_cast<size_t>(wp.size() * kCCPBatchDiffTotalRatio);
    size_t overlap_size =
        static_cast<size_t>(total_batch_size * kCCPBatchDiffOverlapRatio);
    new_wp = wp.subseq(overlap_size, wp.size());
  } else if (tag == (1 << 3)) {
    new_wp = wp;
    // new_wp.append(wi);
  } else if (tag == (1 << 4)) {
    new_wp = wp.subseq(wp.size() / 2, wp.size());
  } else {
    new_wp = wp;
  }

  // // NOTE: query phase
  if (query_type & (1 << 0)) {  // NOTE: membership
    size_t batch_size = static_cast<size_t>(wp.size() * kBatchInsertCheckRatio);
    auto query_ele = parlay::random_shuffle(new_wp).subseq(0, batch_size);
    auto res = parlay::sequence<bool>(query_ele.size(), false);
    Membership<TT>(tree, query_ele, rounds, res);
    assert(parlay::all_of(res, [&](auto r) { return r == true; }));
    std::cout << "\n--------------finish membership------------------\n"
              << std::flush;
  }

  // if (query_type & (1 << 1)) {  // NOTE: range count
  //   for (auto range_query_type : {0, 1, 2}) {
  //     // Points new_wp(tree.GetRoot()->size);
  //     // tree.Flatten(new_wp);
  //     RangeCount<Point>(new_wp, tree, rounds, query_num, range_query_type,
  //                       kDim);
  //   }
  //   std::cout << "--------------finish range count------------------\n"
  //             << std::flush;
  // }

  // if (query_type & (1 << 2)) {  // NOTE: range query
  //   for (auto range_query_type : {0, 1, 2}) {
  //     // Points new_wp(tree.GetRoot()->size);
  //     // tree.Flatten(new_wp);
  //     RangeQuery<Point>(new_wp, tree, rounds, kCCPQueryNum, range_query_type,
  //                       kDim);
  //   }
  //   std::cout << "--------------finish range query------------------\n"
  //             << std::flush;
  // }

  tree.DeleteTree();
  return;
}

int main(int argc, char* argv[]) {
  commandLine params(argc, argv);

  char* input_file_path = params.getOptionValue("-p");
  int K = params.getOptionIntValue("-k", 100);
  int dim = params.getOptionIntValue("-d", 3);
  size_t N = params.getOptionLongValue("-n", -1);
  int tag = params.getOptionIntValue("-t", 1);
  int kRounds = params.getOptionIntValue("-r", 3);
  int query_type = params.getOptionIntValue("-q", 0);
  int read_insert_file = params.getOptionIntValue("-i", 1);
  int tree_type = params.getOptionIntValue("-T", 0);

  auto run = [&]<typename TreeWrapper, typename TypeTrait>(
                 TypeTrait::EntrySeq const& wp, TypeTrait::EntrySeq const& wi,
                 size_t const& N, int const& K, int const& kRounds,
                 string const& kInsertFile, int const& kTag,
                 int const& kQueryType, int const kSummary) -> void {
    // NOTE: run the test
    runKDParallel<TypeTrait, TreeWrapper>(wp, wi, kCCPQueryNum, query_type, K,
                                          tag, kRounds);
  };

  Wrapper::ApplyOrthogonal(tree_type, dim, params, run);

  puts("\nok");
  return 0;
}

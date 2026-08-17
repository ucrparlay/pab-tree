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
void CheckCorrectness(Wrapper::Options opts)
{
	using Tree = TreeDesc::TreeType;
	using entry_seq = typename TT::entry_seq;

	Tree tree;
	constexpr bool kTestTime = false;
	entry_seq wp, wi;

	if (opts.tag & (1 << 0)) {
		auto [base_path, batch_path] =
			opts.ParseInsertBatchPath(opts.batch_ratio);
		std::cout << "base path: " << base_path << "\n";
		std::cout << "batch path: " << batch_path << "\n";
		auto [n, d] = read_points<typename TT::E>(base_path.c_str(), wp, 0);
		auto [n2, d2] = read_points<typename TT::E>(batch_path.c_str(), wi, n);
		// parlay::sort_inplace(
		//     wp, [](auto const& a, auto const& b) { return a.key < b.key; });
		// for (int i = 0; i < 10 && i < wp.size(); i++) {
		//   std::cout << "wp[" << i << "]: " << wp[i].key << "\n";
		// }
		// return;

		BuildTree<TT, Tree, kTestTime, 2>(wp, opts.rounds, tree);
		tree.template validate<typename Tree::Leaf, typename Tree::Interior>();
		puts("---------------finish build insert tree---------------");

		batch_insert<TT, Tree, kTestTime>(tree, wp, wi, 2);
		assert(tree.get_size() == wp.size() + wi.size());
		tree.template validate<typename Tree::Leaf, typename Tree::Interior>();
		std::cout << "---------------finish insert----------------\n"
				  << std::flush;
	}

	// if (tag &
	//     (1 << 1)) {  // NOTE: without tag & (1<<0), this will remove no points
	//   batch_delete<Point, Tree, kTestTime>(tree, wp, wi, 2,
	//   batchInsertCheckRatio); tree.template validate<typename Tree::Leaf,
	//   typename Tree::Interior,
	//                          typename Tree::SplitRuleType>();
	//   std::cout << "---------------finish delete----------------\n" <<
	//   std::flush;
	// }

	// if (tag & (1 << 2)) {
	//   BatchDiff<Point, Tree, kTestTime>(tree, wp, 2, kCCPBatchDiffTotalRatio,
	//                                     kCCPBatchDiffOverlapRatio);
	//   // BatchDiff<Point, Tree, kTestTime>(tree, wp, 2, 0.5, 1.0);
	//   tree.template validate<typename Tree::Leaf, typename Tree::Interior,
	//                          typename Tree::SplitRuleType>();
	//   assert(tree.get_root()->size ==
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
	//     std::cout << "root size: " << tree.get_root()->size << std::endl;
	//   }
	//   tree.template validate<typename Tree::Leaf, typename Tree::Interior,
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
	//   tree.template validate<typename Tree::Leaf, typename Tree::Interior,
	//                          typename Tree::SplitRuleType>();
	//   std::cout << "---------------finish incre delete------------------\n"
	//             << std::flush;
	//   knn_checker(wp.subseq(0, wp.size() / 2), wi,
	//               wp.subseq(wp.size() - kCCPBatchQuerySize, wp.size()));
	//   knn_checker(wp.subseq(0, wp.size() / 2), wi,
	//               wp.subseq(0, kCCPBatchQuerySize));
	// }

	entry_seq new_wp;
	if (opts.tag == (1 << 0)) { // insert
		new_wp = wp;
		new_wp.append(wi);
	}
	// else if (opts.tag == (1 << 1)) {
	//   new_wp = wp.subseq(static_cast<size_t>(wi.size() *
	//   kBatchInsertCheckRatio),
	//                      wp.size());
	// } else if (opts.tag & (1 << 2)) {  // WARN: this assume before we run
	// diff,
	//                                    // the tree should conation wp, e.g.,
	//                                    just
	//                                    // after build or
	//                                    build->insert->delete
	//   size_t total_batch_size =
	//       static_cast<size_t>(wp.size() * kCCPBatchDiffTotalRatio);
	//   size_t overlap_size =
	//       static_cast<size_t>(total_batch_size * kCCPBatchDiffOverlapRatio);
	//   new_wp = wp.subseq(overlap_size, wp.size());
	// } else if (opts.tag == (1 << 3)) {
	//   new_wp = wp;
	//   // new_wp.append(wi);
	// } else if (opts.tag == (1 << 4)) {
	//   new_wp = wp.subseq(wp.size() / 2, wp.size());
	// }
	else {
		new_wp = wp;
	}

	// // NOTE: query phase
	if (opts.query_type & (1 << 0)) { // NOTE: membership
		size_t batch_size =
			static_cast<size_t>(wp.size() * kBatchInsertCheckRatio);
		auto query_ele = parlay::random_shuffle(new_wp).subseq(0, batch_size);
		auto res = parlay::sequence<bool>(query_ele.size(), false);
		Membership<TT>(tree, query_ele, opts.rounds, res);
		assert(parlay::all_of(res, [&](auto r) { return r == true; }));
		std::cout << "\n--------------finish membership------------------\n"
				  << std::flush;
	}

	// if (query_type & (1 << 1)) {  // NOTE: range count
	//   for (auto range_query_type : {0, 1, 2}) {
	//     // Points new_wp(tree.get_root()->size);
	//     // tree.Flatten(new_wp);
	//     range_count<Point>(new_wp, tree, rounds, query_num, range_query_type,
	//                       kDim);
	//   }
	//   std::cout << "--------------finish range count------------------\n"
	//             << std::flush;
	// }

	// if (query_type & (1 << 2)) {  // NOTE: range query
	//   for (auto range_query_type : {0, 1, 2}) {
	//     // Points new_wp(tree.get_root()->size);
	//     // tree.Flatten(new_wp);
	//     RangeQuery<Point>(new_wp, tree, rounds, kCCPQueryNum, range_query_type,
	//                       kDim);
	//   }
	//   std::cout << "--------------finish range query------------------\n"
	//             << std::flush;
	// }

	tree.delete_tree();
	return;
}

int main(int argc, char *argv[])
{
	commandLine params(argc, argv);
	Wrapper::Options opts(params);

	auto run = [&]<typename TreeWrapper, typename TypeTrait>(
				   Wrapper::Options opts) -> void {
		// NOTE: run the test
		CheckCorrectness<TypeTrait, TreeWrapper>(opts);
	};

	Wrapper::ApplyOrthogonal(opts, run);

	puts("\nok");
	return 0;
}

#include "test_framework.h"

#include "baselines/pma.h"
#include "baselines/spma.h"

template <typename TypeTrait> struct PmaWrapper {
	using TreeType = typename PMA::PmaWrapper<TypeTrait>;
};
template <typename TypeTrait> struct SpmaWrapper {
	using TreeType = typename SPMA::SpmaWrapper<TypeTrait>;
};

template <typename Options, typename RunFunc>
void ApplyPMA(Options const &opts, RunFunc test_func)
{
	// using K = std::pair<uint32_t, uint32_t>;

#if defined(DYNAMIC_GRAPH)
	using Wrapper = Wrapper<uint64_t, uint32_t>;
#elif defined(OSM)
	using Wrapper = Wrapper<uint64_t, uint32_t>;
#else
	using Wrapper = Wrapper<uint32_t, uint32_t>;
#endif

	using K = Wrapper::K;
	using V = Wrapper::V;
	using E = Wrapper::E;
	// using EleType = Element<uint32_t, uint32_t>;
	using TypeTrait = pab_tree::btree_type_trait<
		Wrapper::ElementWrapper<E>, 16, Wrapper::LeafAugEmpty,
		Wrapper::InteriorAugEmpty, pab_tree::proto_node>;

	if (opts.tree_type == 3) {
		Run<PmaWrapper<TypeTrait>>(opts, test_func);
	} else if (opts.tree_type == 4) {
		// Run<SpmaWrapper<TypeTrait>>(opts, test_func);
	}
}

int main(int argc, char *argv[])
{
	commandLine params(argc, argv);
	Options opts(params);

	auto test_func = []<class TreeDesc, typename TypeTrait, typename Options>(
						 Options &opts) {
		// using Tree = TreeDesc::TreeType;
		// using TT = TypeTrait;
		// using entry_seq = TT::entry_seq;
		// entry_seq wp;
		// entry_seq wi;
		//
		// using PmaLeaf = uncompressed_leaf<typename TT::K, typename TT::V>;
		// using PmosPmaSettings = PMA_traits<PmaLeaf, InPlace, 0, false, false>;
		// using PmaPointer = PmaLeaf::element_ptr_type;
		// using PMA = CPMA<PmosPmaSettings>;
		//
		// ReadEntries<typename TT::E>(opts.input_file_path.c_str(), wp, 0);
		//
		// parlay::internal::timer t("PMA Batch Insert Test");
		// for (int i = 0; i < 4; i++) {
		//   auto P = Tree::parse_input(wp);
		//   PmaPointer it(P.first.begin(), P.second.begin());
		//   PMA p;
		//
		//   t.start();
		//   // parlay::sort_inplace(parlay::make_slice(P.first));
		//   // t.next("Sorted keys");
		//   p.insert_batch(it, P.first.size(), false);
		//   t.stop();
		// }
		// std::cout << "average insertion time for " << t.total_time() / 4
		//           << " over 4 rounds: ";

		// auto [base_path, batch_path] =
		// opts.ParseInsertBatchPath(opts.batch_ratio);
		// // std::cout << base_path << " " << batch_path << std::endl;
		// auto [n, d] = read_points<typename TT::E>(base_path.c_str(), wp, 0);
		// auto [n2, d2] = read_points<typename TT::E>(batch_path.c_str(), wi, n);

		// for (int i = 0; i < 4; i++) {
		//   auto WP = Tree::parse_input(wp).first;
		//   auto WI = Tree::parse_input(wi).first;
		//   PMA p;
		//   PmaPointer it(WP.begin());
		//   p.insert_batch(it, WP.size(), false);
		//   PmaPointer new_it(WI.begin());
		//   p.insert_batch(new_it, WI.size(), false);

		//   std::cout << "Round " << i << " done." << std::endl;
		// }

		std::cout << "Test finished." << std::endl;
	};

	// Wrapper::ApplyBaselines(opts, test_func);
	// ApplyPMA(opts, DefaultTestFunc);

#if defined(DYNAMIC_GRAPH) || defined(OSM)
	ApplyPMA(opts, TestTemporalGraph);
#else
	ApplyPMA(opts, DefaultTestFunc);
#endif // DYNAMIC_GRAPH
	return 0;
}

#include "test_framework.h"

#include "baselines/pma.h"
#include "baselines/spma.h"

template <typename TT>
struct PmaWrapper {
	using tree_type = typename PMA::PmaWrapper<TT>;
};
template <typename TT>
struct SpmaWrapper {
	using tree_type = typename SPMA::SpmaWrapper<TT>;
};

template <typename Options, typename RunFunc>
void apply_pma(Options const &opts, RunFunc test_func)
{
	/* using key_type = std::pair<uint32_t, uint32_t>; */

#if defined(DYNAMIC_GRAPH)
	using wrapper = wrapper<uint64_t, uint32_t>;
#elif defined(OSM)
	using wrapper = wrapper<uint64_t, uint32_t>;
#else
	using wrapper = wrapper<uint32_t, uint32_t>;
#endif

	using key_type = wrapper::key_type;
	using mapped_type = wrapper::mapped_type;
	using entry_type = wrapper::entry_type;
	/* using EleType = Element<uint32_t, uint32_t>; */
	using TT = pab_tree::btree_type_trait<
		wrapper::element_wrapper<entry_type>, 16, wrapper::leaf_aug_empty,
		wrapper::interior_aug_empty, pab_tree::proto_node>;

	if (opts.tree_type == 3) {
		Run<PmaWrapper<TT>>(opts, test_func);
	} else if (opts.tree_type == 4) {
		/* Run<SpmaWrapper<TT>>(opts, test_func); */
	}
}

int main(int argc, char *argv[])
{
	commandLine params(argc, argv);
	Options opts(params);

	auto test_func =
		[]<class TreeDesc, typename TT, typename Options>(Options &opts) {
			/*
		 * using Tree = TreeDesc::tree_type;
		 * using type_trait = TT;
		 * using entry_seq = type_trait::entry_seq;
		 * entry_seq wp;
		 * entry_seq wi;
		 *
		 * using pma_leaf = uncompressed_leaf<typename type_trait::key_type, typename
		 * type_trait::mapped_type>;
		 * using pmos_pma_settings = PMA_traits<pma_leaf, InPlace, 0, false, false>;
		 * using pma_pointer = pma_leaf::element_ptr_type;
		 * using PMA = CPMA<pmos_pma_settings>;
		 *
		 * read_entries<typename type_trait::entry_type>(opts.input_file_path.c_str(),
		 * wp, 0);
		 *
		 * parlay::internal::timer t("PMA Batch Insert Test");
		 * for (int i = 0; i < 4; i++) {
		 *   auto P = Tree::parse_input(wp);
		 *   pma_pointer it(P.first.begin(), P.second.begin());
		 *   PMA p;
		 *
		 *   t.start();
		 *   // parlay::sort_inplace(parlay::make_slice(P.first));
		 *   // t.next("Sorted keys");
		 *   p.insert_batch(it, P.first.size(), false);
		 *   t.stop();
		 * }
		 * std::cout << "average insertion time for " << t.total_time() / 4
		 *           << " over 4 rounds: ";
		 */

			/*
		 * auto [base_path, batch_path] =
		 * opts.parse_insert_batch_path(opts.batch_ratio);
		 * // std::cout << base_path << " " << batch_path << std::endl;
		 * auto [n, d] = read_points<typename
		 * type_trait::entry_type>(base_path.c_str(), wp, 0);
		 * auto [n2, d2] = read_points<typename
		 * type_trait::entry_type>(batch_path.c_str(), wi, n);
		 */

			/*
		 * for (int i = 0; i < 4; i++) {
		 *   auto WP = Tree::parse_input(wp).first;
		 *   auto WI = Tree::parse_input(wi).first;
		 *   PMA p;
		 *   pma_pointer it(WP.begin());
		 *   p.insert_batch(it, WP.size(), false);
		 *   pma_pointer new_it(WI.begin());
		 *   p.insert_batch(new_it, WI.size(), false);
		 */

			/*
		 *   std::cout << "Round " << i << " done." << std::endl;
		 * }
		 */

			std::cout << "Test finished." << std::endl;
		};

	/*
	 * wrapper::ApplyBaselines(opts, test_func);
	 * apply_pma(opts, DefaultTestFunc);
	 */

#if defined(DYNAMIC_GRAPH) || defined(OSM)
	apply_pma(opts, TestTemporalGraph);
#else
	apply_pma(opts, DefaultTestFunc);
#endif /* DYNAMIC_GRAPH */
	return 0;
}

#include "test_framework.h"

#include "baselines/tlxbtree.h"

template <typename TT>
struct tlx_btree_wrapper {
	using tree_type = typename TlxBTreeTree::tlx_btree_raw<TT>;
};

template <typename Options, typename RunFunc>
void apply_tlx_btree(Options const &opts, RunFunc test_func)
{
#if defined(DYNAMIC_GRAPH)
	using wrapper = wrapper<uint64_t, uint32_t>;
#elif defined(OSM)
	using wrapper = wrapper<uint64_t, uint32_t>;
#else
	using wrapper = wrapper<uint32_t, uint32_t>;
#endif

	using key_type [[maybe_unused]] = wrapper::key_type;
	using mapped_type [[maybe_unused]] = wrapper::mapped_type;
	using entry_type = wrapper::entry_type;
	using TT = pab_tree::btree_type_trait<
		wrapper::element_wrapper<entry_type>, 16, wrapper::leaf_aug_empty,
		wrapper::interior_aug_empty, pab_tree::proto_node>;
	Run<tlx_btree_wrapper<TT>>(opts, test_func);
}

int main(int argc, char *argv[])
{
	commandLine params(argc, argv);
	Options opts(params);

#if defined(DYNAMIC_GRAPH) || defined(OSM)
	apply_tlx_btree(opts, TestTemporalGraph);
#else
	apply_tlx_btree(opts, DefaultTestFunc);
#endif /* DYNAMIC_GRAPH */

	return 0;
}

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
	using entry_type = element<uint64_t, uint32_t>;
#elif defined(OSM)
	using entry_type = element<uint64_t, uint32_t>;
#else
	using entry_type = element<uint32_t, uint32_t>;
#endif

	using TT = pab_tree::btree_type_trait<entry_type, 16>;
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

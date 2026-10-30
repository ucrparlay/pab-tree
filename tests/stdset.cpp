#include "test_framework.h"

#include "baselines/stdset.h"

template <typename TT>
struct stdset_wrapper {
	using tree_type = typename StdSetTree::stdset_raw<TT>;
};

template <typename Options, typename RunFunc>
void apply_stdset(Options const &opts, RunFunc test_func)
{
#if defined(DYNAMIC_GRAPH)
	using entry_type = element<uint64_t, uint32_t>;
#elif defined(OSM)
	using entry_type = element<uint64_t, uint32_t>;
#else
	using entry_type = element<uint32_t, uint32_t>;
#endif

	using TT = pab_tree::btree_type_trait<entry_type, 16>;
	Run<stdset_wrapper<TT>>(opts, test_func);
}

int main(int argc, char *argv[])
{
	commandLine params(argc, argv);
	Options opts(params);

#if defined(DYNAMIC_GRAPH) || defined(OSM)
	apply_stdset(opts, TestTemporalGraph);
#else
	apply_stdset(opts, DefaultTestFunc);
#endif /* DYNAMIC_GRAPH */

	return 0;
}

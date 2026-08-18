#include "test_framework.h"

#include "baselines/tlxbtree.h"

template <typename TypeTrait>
struct TlxBTreeWrapper {
	using TreeType = typename TlxBTreeTree::TlxBTreeRaw<TypeTrait>;
};

template <typename Options, typename RunFunc>
void ApplyTlxBTree(Options const &opts, RunFunc test_func)
{
#if defined(DYNAMIC_GRAPH)
	using Wrapper = Wrapper<uint64_t, uint32_t>;
#elif defined(OSM)
	using Wrapper = Wrapper<uint64_t, uint32_t>;
#else
	using Wrapper = Wrapper<uint32_t, uint32_t>;
#endif

	using key_type = Wrapper::key_type;
	using mapped_type = Wrapper::mapped_type;
	using entry_type = Wrapper::entry_type;
	using TypeTrait = pab_tree::btree_type_trait<
		Wrapper::ElementWrapper<entry_type>, 16, Wrapper::LeafAugEmpty,
		Wrapper::InteriorAugEmpty, pab_tree::proto_node>;
	Run<TlxBTreeWrapper<TypeTrait>>(opts, test_func);
}

int main(int argc, char *argv[])
{
	commandLine params(argc, argv);
	Options opts(params);

	auto test_func = []<class TreeDesc, typename TypeTrait, typename Options>(
						 Options &opts) {};

#if defined(DYNAMIC_GRAPH) || defined(OSM)
	ApplyTlxBTree(opts, TestTemporalGraph);
#else
	ApplyTlxBTree(opts, DefaultTestFunc);
#endif // DYNAMIC_GRAPH

	return 0;
}

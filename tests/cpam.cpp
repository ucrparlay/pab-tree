#include "test_framework.h"

#include "baselines/cpam.h"
#include "baselines/pam.h"

template <typename TT>
struct cpam_wrapper {
	using tree_type = typename CPAMTree::cpam_raw<TT>;
};
template <typename TT>
struct PamWrapper {
	using tree_type = typename PAMTree::PamRaw<TT>;
};

template <typename Options, typename RunFunc>
void apply_cpam(Options const &opts, RunFunc test_func)
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
	/* using EleType = Element<uint32_t, uint32_t>; */
	using TT = pab_tree::btree_type_trait<
		wrapper::element_wrapper<entry_type>, 16, wrapper::leaf_aug_empty,
		wrapper::interior_aug_empty, pab_tree::proto_node>;
	if (opts.tree_type == 0) {
		/* B-tree */
	} else if (opts.tree_type == 1) {
		Run<cpam_wrapper<TT>>(opts, test_func);
	} else if (opts.tree_type == 2) {
		Run<PamWrapper<TT>>(opts, test_func);
	}
}

int main(int argc, char *argv[])
{
	commandLine params(argc, argv);
	Options opts(params);

	/*
	 * wrapper::ApplyBaselines(opts, test_func);
	 * apply_cpam(opts, DefaultTestFunc);
	 */

#if defined(DYNAMIC_GRAPH) || defined(OSM)
	apply_cpam(opts, TestTemporalGraph);
#else
	apply_cpam(opts, DefaultTestFunc);
#endif /* DYNAMIC_GRAPH */

	return 0;
}

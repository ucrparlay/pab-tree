#include "test_framework.h"
#include <cstdint>

/* NOTE: Trees */
template <typename TT>
struct btree_wrapper {
	using tree_type = typename pab_tree::btree<TT>;
};

template <typename Options, typename RunFunc>
static void apply_orthogonal(Options const &opts, RunFunc test_func)
{
#ifdef LEAF1
	static constexpr uint const k_buf = 1;
#elif defined(LEAF2)
	static constexpr uint const k_buf = 2;
#elif defined(LEAF4)
	static constexpr uint const k_buf = 4;
#elif defined(LEAF8)
	static constexpr uint const k_buf = 8;
#elif defined(LEAF16)
	static constexpr uint const k_buf = 16;
#elif defined(LEAF32)
	static constexpr uint const k_buf = 32;
#elif defined(LEAF64)
	static constexpr uint const k_buf = 64;
#elif defined(LEAF128)
	static constexpr uint const k_buf = 128;
#elif defined(LEAF256)
	static constexpr uint const k_buf = 256;
#elif defined(LEAF512)
	static constexpr uint const k_buf = 512;
#elif defined(LEAF1024)
	static constexpr uint const k_buf = 1024;
#else
	static constexpr uint const k_buf = 64; /* Default */
#endif /* LEAF1 */

#if defined(DYNAMIC_GRAPH)
	using entry_type = element<uint64_t, uint32_t>;
#elif defined(OSM)
	/* using entry_type = element<std::pair<uint64_t, uint64_t>, uint32_t>; */
	using entry_type = element<uint64_t, uint32_t>;
#else
	using entry_type = element<uint32_t, uint32_t>;
#endif

	using TT = pab_tree::btree_type_trait<entry_type, k_buf>;
	if (opts.tree_type == 0) {
		Run<btree_wrapper<TT>>(opts, test_func);
	}
}

int main(int argc, char *argv[])
{
	commandLine params(argc, argv);
	Options opts(params);

#if defined(DYNAMIC_GRAPH) || defined(OSM)
	apply_orthogonal(opts, TestTemporalGraph);
#else
	apply_orthogonal(opts, DefaultTestFunc);
#endif /* DYNAMIC_GRAPH */

	return EXIT_SUCCESS;
}

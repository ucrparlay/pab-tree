/*
 * HOW TO RUN (scripts_ae/ae_run_perf.sh runs it under perf)
 *   ./perf_btree -p <dir>/ins/100/1.batch.bin -T 0 -D uni -pcr build
 *   ./perf_cpam ... -T 1      # CPAM; -T 2 is PAM
 *   ./perf_pma ... -T 3       # PMA
 *   # count the region only; fds 10 and 11 open on two fifos:
 *   perf stat -x, -e cycles,instructions -D -1 --control fd:10,11 -- \
 *       ./perf_btree ... -pcf 10 -pcaf 11
 *
 * Fig. 7: hardware counters of one region of the
 * incremental-update runs (DefaultTestFunc, tags 1 << 5, 1 << 6 and 1 << 7,
 * at the 1% step), run once. -p holds the n entries; -pcr picks the region:
 *   build         build of all n                              (1 << 5)
 *   membership    n / 100 lookups, half hits, on the first n / 2 (1 << 5)
 *   incre_insert  insertion of all n from empty, n / 100 a batch (1 << 6)
 *   incre_delete  deletion of all n from a full tree, likewise   (1 << 7)
 * Each region follows the builds and frees DefaultTestFunc runs before it,
 * so the allocator is in the same state. Prints "<region> <seconds>".
 * A separate driver, so test_framework.h carries no perf hooks.
 */

#include "test_framework.h"
#include "perf_control.h"

#if defined(PERF_CPAM)
#include "baselines/cpam.h"
#include "baselines/pam.h"
#elif defined(PERF_PMA)
#include "baselines/pma.h"
#endif

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

/* the step of the incremental runs: n / 100 entries a batch */
static constexpr double k_step_ratio = 0.01;

static bool known_region(std::string_view r)
{
	return r == "build" || r == "membership" || r == "incre_insert" ||
		   r == "incre_delete";
}

/* F once, counted if -pcr names REGION, as time_loop runs one round */
template <typename F>
static double run_region(std::string_view region, F f)
{
	return time_loop(
		1, -100, [] {},
		[&] {
			perf_region const counted(region);
			f();
		},
		[] {});
}

static auto constexpr perf_cache_func = []<class TreeDesc, typename TT,
										   typename Options>(Options opts) {
	using Tree = TreeDesc::tree_type;
	using entry_traits = typename TT::entry_traits;
	std::string const region = perf_control::region;

	typename TT::entry_seq wp;
	read_entries<typename TT::entry_type>(opts.input_file_path.c_str(), wp);
	size_t const n = wp.size();
	size_t const step =
		std::max<size_t>(static_cast<size_t>(k_step_ratio * n), 1);
	size_t const slice_num = n / step + (n % step != 0);
	auto const all = Tree::parse_input(wp);
	Tree tree;
	double t = 0;

	if (region == "build" || region == "membership") {
		/* build_tree<..., 2, 2>, then incre_update_query's mixed probe */
		auto const half = Tree::parse_input(wp.subseq(0, n / 2));
		t = run_region("build", [&] { tree.build(all); });
		tree.delete_tree();
		tree.build(half);
		if (region == "membership") {
			auto const shuffled = parlay::random_shuffle(
				parlay::append(wp.cut(0, n / 2), wp.cut(n / 2, n)));
			auto const query = shuffled.cut(
				0, static_cast<size_t>(n * k_member_query_ratio));
			auto res = parlay::sequence<bool>(query.size(), true);
			t = run_region("membership", [&] {
				parlay::parallel_for(
					0, query.size(),
					[&](size_t i) {
						res[i] = tree.contain(entry_traits::get_key(query[i]));
					},
					1);
			});
		}
	} else if (region == "incre_insert") {
		/* the primal build_tree, then batch_insert_by_step from empty */
		tree.build(all);
		tree.delete_tree();
		tree.build(all);
		auto const batches = parlay::tabulate(slice_num, [&](size_t i) {
			return Tree::parse_input(
				wp.subseq(i * step, std::min((i + 1) * step, n)));
		});
		tree.delete_tree();
		t = run_region("incre_insert", [&] {
			for (size_t i = 0; i < slice_num; i++) {
				tree.batch_insert(batches[i]);
			}
		});
	} else {
		/* the primal build_tree, then batch_delete_by_step from full */
		tree.build(all);
		tree.delete_tree();
		tree.build(all);
		tree.delete_tree();
		auto const batches = parlay::tabulate(slice_num, [&](size_t i) {
			return Tree::parse_delete_input(
				wp.subseq(i * step, std::min((i + 1) * step, n)));
		});
		tree.build(all);
		t = run_region("incre_delete", [&] {
			for (size_t i = 0; i < slice_num; i++) {
				tree.batch_delete(batches[i]);
			}
		});
	}

	std::cout << region << " " << t << "\n" << std::flush;
	tree.delete_tree();
};

#if defined(PERF_CPAM)
template <typename TT>
struct cpam_wrapper {
	using tree_type = typename CPAMTree::cpam_raw<TT>;
};
template <typename TT>
struct pam_wrapper {
	using tree_type = typename PAMTree::PamRaw<TT>;
};
#elif defined(PERF_PMA)
template <typename TT>
struct pma_wrapper {
	using tree_type = typename PMA::PmaWrapper<TT>;
};
#else
template <typename TT>
struct btree_wrapper {
	using tree_type = typename pab_tree::btree<TT>;
};
#endif

/* runs the tree -T names; false when this binary does not hold it */
template <typename RunFunc>
static bool apply_perf(Options const &opts, RunFunc test_func)
{
	using entry_type = element<uint32_t, uint32_t>;

#if defined(PERF_CPAM)
	using TT = pab_tree::btree_type_trait<entry_type, 16>;
	if (opts.tree_type == 1) {
		Run<cpam_wrapper<TT>>(opts, test_func);
		return true;
	}
	if (opts.tree_type == 2) {
		Run<pam_wrapper<TT>>(opts, test_func);
		return true;
	}
#elif defined(PERF_PMA)
	using TT = pab_tree::btree_type_trait<entry_type, 16>;
	if (opts.tree_type == 3) {
		Run<pma_wrapper<TT>>(opts, test_func);
		return true;
	}
#else
	/* the leaf size, as b_test.cpp takes it */
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

	using TT = pab_tree::btree_type_trait<entry_type, k_buf>;
	if (opts.tree_type == 0) {
		Run<btree_wrapper<TT>>(opts, test_func);
		return true;
	}
#endif
	return false;
}

int main(int argc, char *argv[])
{
	commandLine params(argc, argv);
	Options opts(params);
	perf_control::install(params.getOptionIntValue("-pcf", 0),
						  params.getOptionIntValue("-pcaf", 0),
						  params.getOptionValue("-pcr", std::string("")));

	if (!known_region(perf_control::region)) {
		std::cerr << "-pcr must be build, membership, incre_insert or "
					 "incre_delete\n";
		return EXIT_FAILURE;
	}
	if (!apply_perf(opts, perf_cache_func)) {
		std::cerr << "-T " << opts.tree_type << " is not in this binary\n";
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

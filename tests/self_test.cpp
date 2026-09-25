/*
 * Dataset-free correctness tests. Generates its own keys from a seed and
 * checks every operation against std::map, so it runs anywhere.
 *
 * tests/unit_test.cpp is the other half: same tree, real benchmark data, and
 * it skips when the data is absent.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <atomic>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <functional>
#include <map>
#include <new>
#include <numeric>
#include <random>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <parlay/primitives.h>
#include <parlay/sequence.h>

#include "pab_tree/b_tree.h"

namespace
{

/* ---- the element the tree stores ---- */

/* Public key and val, so the tree reads it with no adaptor written. */
struct entry {
	uint32_t key;
	uint32_t val;
};

/* The same entry reached through an adaptor -- the other way in. */
struct ele {
	using entry_type = entry;
	using key_type = uint32_t;
	using mapped_type = uint32_t;

	static key_type get_key(entry_type const &e)
	{
		return e.key;
	}

	static mapped_type get_val(entry_type const &e)
	{
		return e.val;
	}

	static entry_type to_entry(key_type const &k, mapped_type const &v)
	{
		return entry_type{ k, v };
	}
};

/* The two describe the same tree; only the way in differs. */
static_assert(pab_tree::keyed_entry<entry>);
static_assert(!pab_tree::entry_adaptor<entry>);
static_assert(std::is_same_v<pab_tree::btree_type_trait<entry>::entry_type,
							 pab_tree::btree_type_trait<ele>::entry_type>);
static_assert(std::is_same_v<pab_tree::btree_type_trait<entry>::key_type,
							 pab_tree::btree_type_trait<ele>::key_type>);
static_assert(std::is_same_v<pab_tree::btree_type_trait<entry>::mapped_type,
							 pab_tree::btree_type_trait<ele>::mapped_type>);

/* ---- augmentations, checked against the tree they annotate ---- */

/* Sum of the keys in one leaf. */
struct sum_leaf_aug {
	uint64_t sum = 0;

	sum_leaf_aug() = default;

	template <typename Slice>
	explicit sum_leaf_aug(Slice in)
	{
		update_aug(in);
	}

	template <typename Slice>
	void update_aug(Slice in)
	{
		sum = 0;
		for (auto const &e : in) {
			sum += e.key;
		}
	}

	void reset()
	{
		sum = 0;
	}
};

/*
 * Entries below one interior node. Reads only proto_node::size, so it needs
 * no access to the children's own augmentation.
 */
struct subtree_size_aug {
	uint64_t total = 0;

	subtree_size_aug() = default;

	template <typename Nodes, typename Splits>
	void update(Nodes const &nodes, Splits const &)
	{
		total = 0;
		for (auto const *n : nodes) {
			total += n->size;
		}
	}

	[[nodiscard]] static bool force_parallel(size_t, size_t)
	{
		return true;
	}

	void reset()
	{
		total = 0;
	}
};

/*
 * The augmentation contract, pinned. The range-free half is checked by the
 * trait; the rest is checked where update_aug/update are called, against the
 * range that site passes.
 */
namespace aug_contract
{
using entry_slice = parlay::slice<entry *, entry *>;

static_assert(pab_tree::leaf_augmentation<sum_leaf_aug>);
static_assert(pab_tree::leaf_aug_constructible_from<sum_leaf_aug, entry_slice>);
static_assert(pab_tree::leaf_aug_updatable_with<sum_leaf_aug, entry_slice>);
static_assert(pab_tree::interior_augmentation<subtree_size_aug>);

/*
 * The empty defaults satisfy it too, so an unaugmented tree is not a special
 * case.
 */
static_assert(pab_tree::leaf_augmentation<pab_tree::empty_leaf_aug>);
static_assert(pab_tree::interior_augmentation<pab_tree::empty_interior_aug>);

/* And the ones that would not work are rejected. */
struct no_reset {
	uint64_t s = 0;
};
struct no_force_parallel {
	uint64_t s = 0;
	void reset()
	{
	}
};
struct not_updatable {
	uint64_t s = 0;
	void reset()
	{
	}
};
static_assert(!pab_tree::leaf_augmentation<no_reset>);
static_assert(!pab_tree::interior_augmentation<no_force_parallel>);
static_assert(!pab_tree::leaf_aug_updatable_with<not_updatable, entry_slice>);
static_assert(
	!pab_tree::leaf_aug_constructible_from<not_updatable, entry_slice>);
} /* namespace aug_contract */

/* ---- workloads ---- */

enum class dist { uniform, clustered, sequential, sparse, boundary };

char const *dist_name(dist d)
{
	switch (d) {
	case dist::uniform:
		return "uniform";
	case dist::clustered:
		return "clustered";
	case dist::sequential:
		return "sequential";
	case dist::sparse:
		return "sparse";
	case dist::boundary:
		return "boundary";
	}
	return "?";
}

/*
 * n distinct keys. Distinct is a precondition of the tree, so every shape
 * here dedupes before it returns.
 */
std::vector<uint32_t> gen_keys(size_t n, dist d, uint64_t seed)
{
	std::mt19937_64 rng(seed);
	std::vector<uint32_t> keys;
	keys.reserve(n);

	switch (d) {
	case dist::uniform: {
		/* Draw from a space wide enough that dedup rarely bites. */
		auto const span = static_cast<uint32_t>(
			std::min<uint64_t>(std::max<uint64_t>(n * 8, 64), 0xFFFFFFF0ULL));
		while (keys.size() < n * 2 && keys.size() < 4 * n + 64) {
			keys.push_back(static_cast<uint32_t>(rng() % span));
			if (keys.size() >= 2 * n) {
				break;
			}
		}
		break;
	}
	case dist::clustered: {
		/* A few hot regions, so leaves fill unevenly. */
		size_t const centres = std::max<size_t>(1, n / 500);
		std::vector<uint32_t> hot;
		for (size_t i = 0; i < centres; ++i) {
			hot.push_back(static_cast<uint32_t>(rng() % 1000000));
		}
		for (size_t i = 0; i < 2 * n; ++i) {
			auto const c = hot[rng() % hot.size()];
			auto const off = static_cast<int64_t>(rng() % 201) - 100;
			auto const k = static_cast<int64_t>(c) + off;
			keys.push_back(static_cast<uint32_t>(k < 0 ? 0 : k));
		}
		break;
	}
	case dist::sequential:
		for (size_t i = 0; i < n; ++i) {
			keys.push_back(static_cast<uint32_t>(i));
		}
		break;
	case dist::sparse:
		for (size_t i = 0; i < n; ++i) {
			keys.push_back(static_cast<uint32_t>(i * 65521u));
		}
		break;
	case dist::boundary:
		/*
		 * Runs of consecutive keys separated by gaps, so batches land
		 * exactly on split boundaries.
		 */
		for (size_t i = 0; i < n; ++i) {
			keys.push_back(static_cast<uint32_t>((i / 4) * 1024 + (i % 4)));
		}
		break;
	}

	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	if (keys.size() > n) {
		keys.resize(n);
	}
	std::shuffle(keys.begin(), keys.end(), rng);
	return keys;
}

std::vector<entry> to_entries(std::vector<uint32_t> const &keys)
{
	std::vector<entry> v;
	v.reserve(keys.size());
	for (auto k : keys) {
		/* A value derived from the key, so a misplaced entry is visible. */
		v.push_back(entry{ k, k ^ 0x5bf03635u });
	}
	return v;
}

/* ---- checks against the oracle ---- */

using oracle = std::map<uint32_t, uint32_t>;

void add_to_oracle(oracle &ref, std::vector<entry> const &v)
{
	for (auto const &e : v) {
		ref[e.key] = e.val;
	}
}

/*
 * Every leaf's augmentation equals the sum of the keys it holds, and every
 * interior's equals the entries below it. Catches an update_aug that a
 * rebalance forgot.
 */
template <typename Tree, typename NodeType>
void check_aug(NodeType const *node, size_t &leaves, size_t &interiors)
{
	using leaf_type = typename Tree::leaf_type;
	using interior_type = typename Tree::interior_type;

	if (node->is_leaf) {
		auto const *leaf = static_cast<leaf_type const *>(node);
		uint64_t want = 0;
		for (size_t i = 0; i < leaf->size; ++i) {
			want += leaf->elements[i].key;
		}
		ASSERT_EQ(leaf->get_aug().sum, want) << "leaf aug is stale";
		leaves++;
		return;
	}

	auto const *interior = static_cast<interior_type const *>(node);
	uint64_t want = 0;
	for (size_t i = 0; i < interior->get_node_n(); ++i) {
		want += interior->tree_nodes[i]->size;
	}
	ASSERT_EQ(interior->get_aug().total, want) << "interior aug is stale";
	ASSERT_EQ(interior->get_aug().total, interior->size)
		<< "interior aug != subtree size";
	interiors++;

	for (size_t i = 0; i < interior->get_node_n(); ++i) {
		ASSERT_NO_FATAL_FAILURE(
			check_aug<Tree>(interior->tree_nodes[i], leaves, interiors));
	}
}

/* Contents, ordering, membership and the range queries, all against ref. */
template <typename Tree>
void check_against(Tree &t, oracle const &ref)
{
	bool const structure_ok = t.template validate<true, false>();
	ASSERT_TRUE(structure_ok) << "validate() failed";
	ASSERT_EQ(t.size(), ref.size());
	ASSERT_EQ(t.empty(), ref.empty());

	/* Forward iteration, then backwards from end(). */
	{
		size_t cnt = 0;
		auto rt = ref.begin();
		for (auto it = t.begin(); it != t.end(); ++it, ++rt, ++cnt) {
			ASSERT_NE(rt, ref.end()) << "tree iterated past the oracle";
			ASSERT_EQ(it->key, rt->first);
			ASSERT_EQ(it->val, rt->second);
		}
		ASSERT_EQ(cnt, ref.size());

		auto it = t.end();
		auto rit = ref.end();
		while (it != t.begin()) {
			--it;
			--rit;
			ASSERT_EQ(it->key, rit->first) << "reverse walk diverged";
		}
		ASSERT_EQ(rit, ref.begin());
	}

	/* get_entries returns everything, in no particular order. */
	{
		parlay::sequence<entry> got(t.size());
		t.get_entries(got);
		ASSERT_EQ(got.size(), ref.size());
		std::vector<std::pair<uint32_t, uint32_t>> flat;
		flat.reserve(got.size());
		for (auto const &e : got) {
			flat.emplace_back(e.key, e.val);
		}
		std::sort(flat.begin(), flat.end());
		std::vector<std::pair<uint32_t, uint32_t>> want(ref.begin(), ref.end());
		ASSERT_EQ(flat, want) << "get_entries does not match the oracle";
	}

	/* to_seq is the same contents in key order. */
	{
		auto const seq = t.to_seq();
		ASSERT_EQ(seq.size(), ref.size());
		auto rt = ref.begin();
		for (size_t i = 0; i < seq.size(); ++i, ++rt) {
			ASSERT_EQ(seq[i].key, rt->first) << "to_seq out of order at " << i;
			ASSERT_EQ(seq[i].val, rt->second);
		}
	}

	if (ref.empty()) {
		ASSERT_EQ(t.begin(), t.end());
		return;
	}
}

/* Membership and ordered lookup over present and absent keys. */
template <typename Tree>
void check_lookups(Tree const &t, oracle const &ref,
				   std::vector<uint32_t> const &probes)
{
	for (auto k : probes) {
		bool const in_ref = ref.count(k) != 0;
		ASSERT_EQ(t.contain(k), in_ref) << "contain(" << k << ")";
		ASSERT_EQ(t.contains(k), in_ref);
		ASSERT_EQ(t.count(k), in_ref ? 1u : 0u);

		auto const f = t.find(k);
		ASSERT_EQ(f == t.end(), !in_ref) << "find(" << k << ")";
		if (in_ref) {
			ASSERT_EQ(f->key, k);
			ASSERT_EQ(f->val, ref.at(k));
		}

		auto const lb = t.lower_bound(k);
		auto const rlb = ref.lower_bound(k);
		ASSERT_EQ(lb == t.end(), rlb == ref.end())
			<< "lower_bound(" << k << ")";
		if (rlb != ref.end()) {
			ASSERT_EQ(lb->key, rlb->first);
		}

		auto const ub = t.upper_bound(k);
		auto const rub = ref.upper_bound(k);
		ASSERT_EQ(ub == t.end(), rub == ref.end())
			<< "upper_bound(" << k << ")";
		if (rub != ref.end()) {
			ASSERT_EQ(ub->key, rub->first);
		}

		auto const er = t.equal_range(k);
		ASSERT_EQ(er.first, lb);
		ASSERT_EQ(std::distance(er.first, er.second), in_ref ? 1 : 0);
	}
}

/* range_count and range_report over boxes drawn around live keys. */
template <typename Tree>
void check_ranges(Tree const &t, oracle const &ref, uint64_t seed, int boxes)
{
	if (ref.empty()) {
		ASSERT_EQ(t.range_count({ 0, 100 }), 0u);
		return;
	}

	std::mt19937_64 rng(seed);
	std::vector<uint32_t> keys;
	keys.reserve(ref.size());
	for (auto const &kv : ref) {
		keys.push_back(kv.first);
	}

	for (int b = 0; b < boxes; ++b) {
		uint32_t lo = 0;
		uint32_t hi = 0;
		if (b == 0) {
			/* The whole key space. */
			lo = 0;
			hi = 0xFFFFFFFFu;
		} else if (b == 1) {
			/* Empty box. */
			lo = keys[rng() % keys.size()];
			hi = lo;
		} else {
			lo = keys[rng() % keys.size()];
			hi = keys[rng() % keys.size()];
			if (lo > hi) {
				std::swap(lo, hi);
			}
			/* Half-open, so nudge sometimes to cover both edges. */
			if ((rng() & 1) && hi != 0xFFFFFFFFu) {
				hi++;
			}
		}

		auto const want = static_cast<size_t>(
			std::distance(ref.lower_bound(lo), ref.lower_bound(hi)));
		auto const got = t.range_count({ lo, hi });
		ASSERT_EQ(got, want)
			<< "range_count [" << lo << ", " << hi << ") box " << b;

		parlay::sequence<entry> out(want);
		auto const n = t.range_report({ lo, hi }, parlay::make_slice(out));
		ASSERT_EQ(n, want) << "range_report returned a different count";

		std::vector<uint32_t> got_keys;
		got_keys.reserve(n);
		for (size_t i = 0; i < n; ++i) {
			ASSERT_GE(out[i].key, lo) << "range_report escaped the box";
			ASSERT_LT(out[i].key, hi) << "range_report escaped the box";
			ASSERT_EQ(out[i].val, ref.at(out[i].key)) << "wrong value";
			got_keys.push_back(out[i].key);
		}
		std::sort(got_keys.begin(), got_keys.end());
		std::vector<uint32_t> want_keys([&] {
			std::vector<uint32_t> w;
			for (auto it = ref.lower_bound(lo); it != ref.lower_bound(hi);
				 ++it) {
				w.push_back(it->first);
			}
			return w;
		}());
		ASSERT_EQ(got_keys, want_keys) << "range_report contents";
	}
}

uint64_t g_seed = 20260817;
int g_rounds = 1;

/* Base options with the delete policy swapped in. */
template <typename Base, typename Policy>
struct with_policy : Base {
	using delete_policy = Policy;
};

using graft_policy = pab_tree::graft_delete;

template <typename Policy>
char const *policy_name()
{
	return std::is_same_v<Policy, graft_policy> ? "graft" : "repair";
}

/* One full scenario: build, query, insert, delete, re-query. */
template <unsigned KBuf, typename Policy = pab_tree::repair_delete>
void run_scenario(size_t n, dist d, uint64_t seed, bool with_ranges = true)
{
	/* No adaptor: the whole matrix runs on the entry itself. */
	using tt =
		pab_tree::btree_trait<entry, KBuf,
							  with_policy<pab_tree::default_options, Policy>>;
	using tree = pab_tree::btree<tt>;

	SCOPED_TRACE("k_buf=" + std::to_string(KBuf) + " n=" + std::to_string(n) +
				 " dist=" + dist_name(d) + " delete=" + policy_name<Policy>() +
				 " seed=" + std::to_string(seed) +
				 "  (rerun: ./self_test --seed=" + std::to_string(seed) + ")");

	auto const all = gen_keys(n, d, seed);
	/* Two thirds build the tree, one third is the insert batch. */
	size_t const split = all.size() * 2 / 3;
	std::vector<uint32_t> base(all.begin(), all.begin() + split);
	std::vector<uint32_t> extra(all.begin() + split, all.end());

	auto const base_e = to_entries(base);
	auto const extra_e = to_entries(extra);

	tree t;
	oracle ref;

	t.build(base_e);
	add_to_oracle(ref, base_e);
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));

	/* Probe live keys, the insert batch (absent so far), and neighbours. */
	std::vector<uint32_t> probes;
	{
		std::mt19937_64 rng(seed ^ 0x9e3779b97f4a7c15ULL);
		size_t const take = std::min<size_t>(base.size(), 64);
		for (size_t i = 0; i < take; ++i) {
			auto const k = base[rng() % base.size()];
			probes.push_back(k);
			probes.push_back(k + 1);
			if (k > 0) {
				probes.push_back(k - 1);
			}
		}
		for (size_t i = 0; i < std::min<size_t>(extra.size(), 32); ++i) {
			probes.push_back(extra[i]);
		}
		probes.push_back(0);
		probes.push_back(0xFFFFFFFFu);
	}
	ASSERT_NO_FATAL_FAILURE(check_lookups(t, ref, probes));
	if (with_ranges) {
		ASSERT_NO_FATAL_FAILURE(check_ranges(t, ref, seed ^ 1, 12));
	}

	/* Insert the rest. */
	t.batch_insert(extra_e);
	add_to_oracle(ref, extra_e);
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
	ASSERT_NO_FATAL_FAILURE(check_lookups(t, ref, probes));
	if (with_ranges) {
		ASSERT_NO_FATAL_FAILURE(check_ranges(t, ref, seed ^ 2, 12));
	}

	/* Delete half of what is in there, chosen across the whole key range. */
	{
		std::vector<uint32_t> live;
		for (auto const &kv : ref) {
			live.push_back(kv.first);
		}
		std::mt19937_64 rng(seed ^ 0xdeadbeefULL);
		std::shuffle(live.begin(), live.end(), rng);
		live.resize(live.size() / 2);

		t.batch_delete(live);
		for (auto k : live) {
			ref.erase(k);
		}
		ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
		ASSERT_NO_FATAL_FAILURE(check_lookups(t, ref, probes));
		if (with_ranges) {
			ASSERT_NO_FATAL_FAILURE(check_ranges(t, ref, seed ^ 3, 12));
		}
	}

	/* Delete everything that is left. */
	{
		std::vector<uint32_t> live;
		for (auto const &kv : ref) {
			live.push_back(kv.first);
		}
		t.batch_delete(live);
		ref.clear();
		ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
		ASSERT_TRUE(t.empty());
		ASSERT_EQ(t.size(), 0u);
	}
}

} /* namespace */

/* ---- the trait can be configured by name, not just by position ---- */

/*
 * Overriding a late parameter must not mean naming the earlier defaults.
 * btree_trait is an alias for btree_type_trait, so the two cannot drift:
 * these static asserts fail if the mapping is ever wrong.
 */
namespace trait_forms
{
struct desc : pab_tree::default_options {
	using key_compare = std::greater<uint32_t>;
};
struct alloc_only : pab_tree::default_options {
	using node_alloc = pab_tree::parlay_node_alloc;
};

/* everything defaulted */
static_assert(std::is_same_v<pab_tree::btree_trait<ele>,
							 pab_tree::btree_type_trait<ele>>);
/* BufferSize defaults to 64 */
static_assert(std::is_same_v<pab_tree::btree_type_trait<ele>,
							 pab_tree::btree_type_trait<ele, 64>>);
/* only the comparator overridden */
static_assert(
	std::is_same_v<
		pab_tree::btree_trait<ele, 32, desc>,
		pab_tree::btree_type_trait<
			ele, 32, pab_tree::empty_leaf_aug, pab_tree::empty_interior_aug,
			pab_tree::proto_node, std::greater<uint32_t>>>);
/* only the allocator named, explicitly equal to the default */
static_assert(std::is_same_v<pab_tree::btree_trait<ele, 8, alloc_only>,
							 pab_tree::btree_type_trait<ele, 8>>);
} /* namespace trait_forms */

TEST(SelfTest, TraitByName)
{
	/*
	 * An options struct declared inside a function: key_compare is a
	 * plain alias, not a member template, so this compiles.
	 */
	struct desc : pab_tree::default_options {
		using key_compare = std::greater<uint32_t>;
	};
	using tt = pab_tree::btree_trait<ele, 32, desc>;
	using tree = pab_tree::btree<tt>;

	auto const keys = gen_keys(5000, dist::uniform, g_seed ^ 0x7a17);
	tree t;
	t.build(to_entries(keys));
	bool const ok = t.template validate<true, false>();
	ASSERT_TRUE(ok);
	ASSERT_EQ(t.size(), keys.size());

	/* descending, because the option took effect */
	auto sorted = keys;
	std::sort(sorted.begin(), sorted.end(), std::greater<uint32_t>());
	auto it = t.begin();
	for (size_t i = 0; i < 50; ++i, ++it)
		ASSERT_EQ(it->key, sorted[i]);
}

/* ---- the element adaptor is used directly, with either signature ---- */

/*
 * By-value accessors. The library used to wrap every adaptor in a layer
 * that forced this signature; it now calls the adaptor as written.
 */
struct ele_by_value {
	using entry_type = entry;
	using key_type = uint32_t;
	using mapped_type = uint32_t;

	static key_type get_key(entry_type e)
	{
		return e.key;
	}
	static mapped_type get_val(entry_type e)
	{
		return e.val;
	}
	static entry_type to_entry(key_type k, mapped_type v)
	{
		return entry_type{ k, v };
	}
};

/* A fat entry, where a by-value copy would be 64 bytes if one happened. */
struct big_entry {
	uint32_t key;
	char payload[60];
};

struct ele_big {
	using entry_type = big_entry;
	using key_type = uint32_t;
	using mapped_type = uint32_t;

	static key_type get_key(entry_type const &e)
	{
		return e.key;
	}
	static mapped_type get_val(entry_type const &e)
	{
		return e.key;
	}
	static entry_type to_entry(key_type const &k, mapped_type const &)
	{
		entry_type e{};
		e.key = k;
		return e;
	}
};

static_assert(pab_tree::entry_adaptor<ele>);
static_assert(pab_tree::entry_adaptor<ele_by_value>);
static_assert(pab_tree::entry_adaptor<ele_big>);

TEST(SelfTest, ElementAdaptorSignatures)
{
	auto const keys = gen_keys(20000, dist::uniform, g_seed ^ 0xe1e);

	/* by-value accessors */
	{
		using tt = pab_tree::btree_type_trait<ele_by_value, 32>;
		pab_tree::btree<tt> t;
		oracle ref;
		auto const v = to_entries(keys);
		t.build(v);
		add_to_oracle(ref, v);
		ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
		for (size_t i = 0; i < keys.size(); i += 71)
			ASSERT_TRUE(t.contain(keys[i]));
	}

	/* a 64-byte entry, by-reference accessors */
	{
		using tt = pab_tree::btree_type_trait<ele_big, 32>;
		pab_tree::btree<tt> t;
		std::vector<big_entry> v(keys.size());
		for (size_t i = 0; i < keys.size(); ++i)
			v[i].key = keys[i];
		t.build(v);
		bool const ok = t.template validate<true, false>();
		ASSERT_TRUE(ok);
		ASSERT_EQ(t.size(), keys.size());
		for (size_t i = 0; i < keys.size(); i += 71)
			ASSERT_TRUE(t.contain(keys[i]));

		auto sorted = keys;
		std::sort(sorted.begin(), sorted.end());
		std::vector<uint32_t> half(sorted.begin(),
								   sorted.begin() + sorted.size() / 2);
		t.batch_delete(half);
		ASSERT_EQ(t.size(), keys.size() - half.size());
		ASSERT_TRUE((t.template validate<true, false>()));
	}
}

/* ---- get_entries' output-size contract ---- */

TEST(SelfTest, GetEntriesOutputSize)
{
	using tt = pab_tree::btree_type_trait<ele, 32>;
	using tree = pab_tree::btree<tt>;

	auto const all = to_entries(gen_keys(5000, dist::uniform, g_seed ^ 0x9e7));
	tree t;
	t.build(all);
	ASSERT_EQ(t.size(), all.size());

	/* Exactly sized: every entry lands, none past the end. */
	{
		parlay::sequence<entry> out(t.size());
		t.get_entries(out);
		std::vector<uint32_t> got;
		for (auto const &e : out)
			got.push_back(e.key);
		std::sort(got.begin(), got.end());
		std::vector<uint32_t> want;
		for (auto const &e : all)
			want.push_back(e.key);
		std::sort(want.begin(), want.end());
		EXPECT_EQ(got, want);
	}

	/* Oversized is fine; the tail is left alone. */
	{
		parlay::sequence<entry> out(t.size() + 100);
		for (size_t i = t.size(); i < out.size(); ++i)
			out[i] = entry{ 0xDEADBEEFu, 0xDEADBEEFu };
		t.get_entries(out);
		for (size_t i = t.size(); i < out.size(); ++i)
			ASSERT_EQ(out[i].key, 0xDEADBEEFu)
				<< "get_entries wrote past size()";
	}

	/* An empty tree writes nothing and does not dereference a null root. */
	{
		tree e;
		parlay::sequence<entry> out(4);
		out[0] = entry{ 7u, 7u };
		e.get_entries(out);
		EXPECT_EQ(out[0].key, 7u);
	}

	/*
	 * One short must be caught rather than overrun. Asserts are live in
	 * this build (-UNDEBUG); in a release build the call is undefined,
	 * which is what the README says.
	 */
#ifndef NDEBUG
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	parlay::sequence<entry> small(t.size() - 1);
	EXPECT_DEATH(
		{ t.get_entries(small); }, "out.size\\(\\) >=")
		<< "an undersized output was not caught";
#endif
}

/* ---- deleting a key that is not there does nothing ---- */

TEST(SelfTest, DeleteAbsentKeys)
{
	using tt = pab_tree::btree_type_trait<ele, 16>;
	using tree = pab_tree::btree<tt>;

	auto const seed = g_seed ^ 0xab5e27;
	SCOPED_TRACE("absent-key delete seed=" + std::to_string(seed));

	/* Even keys only, so every odd key is absent. */
	std::vector<entry> v;
	oracle ref;
	for (uint32_t i = 0; i < 40000; i += 2) {
		v.push_back(entry{ i, i ^ 0x5bf03635u });
		ref[i] = i ^ 0x5bf03635u;
	}
	tree t;
	t.build(v);
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));

	auto erase_both = [&](std::vector<uint32_t> keys, char const *what) {
		SCOPED_TRACE(what);
		std::sort(keys.begin(), keys.end());
		keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
		t.batch_delete(keys);
		for (auto k : keys)
			ref.erase(k); /* std::map::erase ignores absent keys too */
		ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
	};

	/* 1. every key absent, and more of them than the tree holds */
	{
		std::vector<uint32_t> odd;
		for (uint32_t i = 1; i < 120000; i += 2)
			odd.push_back(i);
		auto const before = t.size();
		ASSERT_NO_FATAL_FAILURE(erase_both(odd, "all absent, batch > tree"));
		ASSERT_EQ(t.size(), before) << "an absent-key delete changed the size";
	}

	/* 2. absent keys beyond both ends of the key range */
	{
		std::vector<uint32_t> outside{ 0xFFFFFFFEu, 0xFFFFFF00u, 1u, 3u };
		auto const before = t.size();
		ASSERT_NO_FATAL_FAILURE(erase_both(outside, "outside the range"));
		ASSERT_EQ(t.size(), before);
	}

	/* 3. a mix: half present, half not */
	{
		std::mt19937_64 rng(seed);
		std::vector<uint32_t> mixed;
		for (int i = 0; i < 4000; ++i) {
			uint32_t const k = static_cast<uint32_t>(rng() % 40000);
			mixed.push_back(k); /* even => present, odd => absent */
		}
		ASSERT_NO_FATAL_FAILURE(erase_both(mixed, "half present"));
	}

	/* 4. deleting the same present key twice: second time is a no-op */
	{
		auto const k = t.begin()->key;
		ASSERT_TRUE(t.contain(k));
		ASSERT_NO_FATAL_FAILURE(erase_both({ k }, "present key, first time"));
		ASSERT_FALSE(t.contain(k));
		auto const before = t.size();
		ASSERT_NO_FATAL_FAILURE(erase_both({ k }, "same key, second time"));
		ASSERT_EQ(t.size(), before);
	}

	/* 5. single-key erase(k) reports whether it was there */
	{
		auto const present = t.begin()->key;
		ASSERT_TRUE(t.erase(present));
		ASSERT_FALSE(t.erase(present));
		ASSERT_FALSE(t.erase(1u)); /* odd, never present */
		ref.erase(present);
		ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
	}

	/* 6. emptying the tree with a batch that also names absent keys */
	{
		std::vector<uint32_t> everything;
		for (auto const &kv : ref)
			everything.push_back(kv.first);
		for (uint32_t i = 1; i < 2000; i += 2)
			everything.push_back(i); /* absent */
		ASSERT_NO_FATAL_FAILURE(erase_both(everything, "empty it out"));
		ASSERT_TRUE(t.empty());
		ASSERT_EQ(t.begin(), t.end());
	}
}

/* ---- the default leaf-sort decision is reproducible ---- */

/* FNV over every leaf's is_sorted flag, in tree order. */
template <typename Tree, typename NodeType>
void hash_leaf_sortedness(NodeType const *node, uint64_t &h, size_t &leaves)
{
	using leaf_type = typename Tree::leaf_type;
	using interior_type = typename Tree::interior_type;

	if (node->is_leaf) {
		auto const *leaf = static_cast<leaf_type const *>(node);
		h = (h ^ (leaf->is_sorted ? 1u : 0u)) * 1099511628211ULL;
		leaves++;
		return;
	}
	auto const *interior = static_cast<interior_type const *>(node);
	for (size_t i = 0; i < interior->get_node_n(); ++i)
		hash_leaf_sortedness<Tree>(interior->tree_nodes[i], h, leaves);
}

/*
 * Runs the default insert/delete path and prints the leaf-sortedness
 * pattern. Called in the child process; see the test below.
 */
static void print_hybrid_pattern()
{
	using tt = pab_tree::btree_type_trait<ele, 32>;
	using tree = pab_tree::btree<tt>;

	auto const keys = gen_keys(60000, dist::uniform, g_seed ^ 0xdec1de);
	auto const all = to_entries(keys);
	std::vector<entry> base(all.begin(), all.begin() + all.size() / 2);
	std::vector<entry> rest(all.begin() + all.size() / 2, all.end());
	std::vector<uint32_t> del(keys.begin(), keys.begin() + keys.size() / 4);
	std::sort(del.begin(), del.end());

	tree t;
	t.build(base);
	t.batch_insert(rest); /* default path: hybrid + default_hybrid_pred */
	t.batch_delete(del);

	uint64_t h = 1469598103934665603ULL;
	size_t leaves = 0;
	hash_leaf_sortedness<tree>(t.get_root(), h, leaves);
	std::printf("PATTERN %016llx %zu\n", (unsigned long long)h, leaves);
	std::fflush(stdout);
}

/*
 * default_hybrid_pred mixes bit 3 of each end of the input slice into its
 * coin flip. That bit comes from the slice's offset inside its buffer, not
 * from the address -- allocations are 16-byte aligned, so bit 3 of the base
 * is 0 -- so the decision is the same from run to run.
 *
 * This has to be checked across processes. Inside one process the slices
 * the predicate sees are internal buffers that the pool hands back at the
 * same address every time, so two runs prove nothing; only a fresh process
 * gets a different ASLR base. Verified: with the shift changed from 3 to 12
 * the in-process version still passed, this one fails.
 */
TEST(SelfTest, DefaultHybridPredIsReproducible)
{
	/*
	 * Resolve our own path here: inside popen, /proc/self/exe is the
	 * shell, not this binary.
	 */
	char exe[4096];
	ssize_t const len = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	ASSERT_GT(len, 0) << "cannot resolve /proc/self/exe";
	exe[len] = '\0';

	auto read_pattern = [&](std::string &out) -> bool {
		std::string const cmd =
			std::string("PABTREE_HYBRID_CHILD=1 ") + exe +
			" --gtest_filter=SelfTest.DefaultHybridPredIsReproducible"
			" 2>/dev/null";
		FILE *f = popen(cmd.c_str(), "r");
		if (f == nullptr)
			return false;
		char buf[256];
		bool found = false;
		while (std::fgets(buf, sizeof(buf), f) != nullptr) {
			if (std::strncmp(buf, "PATTERN ", 8) == 0) {
				out = buf;
				found = true;
			}
		}
		pclose(f);
		return found;
	};

	std::string a, b;
	ASSERT_TRUE(read_pattern(a)) << "child process produced no pattern";
	ASSERT_TRUE(read_pattern(b)) << "child process produced no pattern";
	EXPECT_EQ(a, b)
		<< "the default leaf-sort decision differs between two processes; "
		   "it depends on the address layout, not just the input";
}

/* ---- a custom node base ---- */

/*
 * Same shape proto_node has, plus a field of its own. node_base is what
 * the tree requires: constructible from (bool, size_t), with public
 * is_leaf and a writable size.
 */
struct tagged_node {
	tagged_node()
		: is_leaf(false)
		, size(0)
		, tag(0)
	{
	}

	tagged_node(bool l, size_t s)
		: is_leaf(l)
		, size(s)
		, tag(0xAB)
	{
	}

	bool is_leaf;
	size_t size;
	unsigned tag;
};

/*
 * An aggregate qualifies too: C++20 parenthesized aggregate init (P0960)
 * makes this constructible from (bool, size_t) without a constructor.
 */
struct aggregate_node {
	bool is_leaf = false;
	size_t size = 0;
};

static_assert(pab_tree::node_base<pab_tree::proto_node>);
static_assert(pab_tree::node_base<tagged_node>);
static_assert(pab_tree::node_base<aggregate_node>);

/* And the ones the tree could not work with are rejected. */
struct no_size {
	no_size(bool l, size_t)
		: is_leaf(l)
	{
	}
	bool is_leaf;
};
struct const_size {
	const_size(bool l, size_t s)
		: is_leaf(l)
		, size(s)
	{
	}
	bool is_leaf;
	size_t const size;
};
static_assert(!pab_tree::node_base<no_size>);
static_assert(!pab_tree::node_base<const_size>);

TEST(SelfTest, CustomNodeBase)
{
	struct opts : pab_tree::default_options {
		using node = tagged_node;
	};
	using tt = pab_tree::btree_trait<ele, 32, opts>;
	using tree = pab_tree::btree<tt>;

	auto const seed = g_seed ^ 0xb45eULL;
	auto const keys = gen_keys(20000, dist::uniform, seed);
	auto const all = to_entries(keys);
	std::vector<entry> base(all.begin(), all.begin() + all.size() / 2);
	std::vector<entry> rest(all.begin() + all.size() / 2, all.end());

	oracle ref;
	tree t;
	t.build(base);
	add_to_oracle(ref, base);
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));

	t.batch_insert(rest);
	add_to_oracle(ref, rest);
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));

	/* The custom field survives on every node the tree built. */
	ASSERT_NE(t.get_root(), nullptr);
	ASSERT_EQ(t.get_root()->tag, 0xABu) << "custom node field lost";

	std::vector<uint32_t> half(keys.begin(), keys.begin() + keys.size() / 2);
	std::sort(half.begin(), half.end());
	t.batch_delete(half);
	for (auto k : half)
		ref.erase(k);
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
}

/* ---- a custom node allocator is used for every node ---- */

/*
 * Counts allocations and frees. Also a leak check: the tree must return
 * every node it took by the time it is destroyed.
 */
struct counting_alloc {
	static inline std::atomic<long> live{ 0 };
	static inline std::atomic<long> total{ 0 };

	template <typename node>
	static node *alloc()
	{
		live.fetch_add(1, std::memory_order_relaxed);
		total.fetch_add(1, std::memory_order_relaxed);
		return parlay::type_allocator<node>::alloc();
	}

	template <typename node>
	static void free(node *p)
	{
		live.fetch_sub(1, std::memory_order_relaxed);
		parlay::type_allocator<node>::retire(p);
	}
};

/*
 * Per-type allocation counts. Which recovery a delete took shows up here:
 * the in-place consolidation allocates interiors and never a leaf, the
 * flatten-and-rebuild allocates leaves.
 */
struct typed_counting_alloc {
	template <typename node>
	static inline std::atomic<long> allocs{ 0 };

	template <typename node>
	static inline std::atomic<long> frees{ 0 };

	template <typename node>
	static node *alloc()
	{
		allocs<node>.fetch_add(1, std::memory_order_relaxed);
		return parlay::type_allocator<node>::alloc();
	}

	template <typename node>
	static void free(node *p)
	{
		frees<node>.fetch_add(1, std::memory_order_relaxed);
		parlay::type_allocator<node>::retire(p);
	}
};

/* Plain new/delete, so a sanitizer sees a node used after it was freed. */
struct heap_alloc {
	template <typename node>
	static node *alloc()
	{
		return static_cast<node *>(
			::operator new(sizeof(node), std::align_val_t{ alignof(node) }));
	}

	template <typename node>
	static void free(node *p)
	{
		p->~node();
		::operator delete(p, std::align_val_t{ alignof(node) });
	}
};

/* (leaf, interior) allocation counts of one tree type, for deltas. */
template <typename tree>
std::pair<long, long> node_allocs()
{
	return {
		typed_counting_alloc::allocs<typename tree::leaf_type>.load(),
		typed_counting_alloc::allocs<typename tree::interior_type>.load()
	};
}

/*
 * Which recovery a delete is expected to take. Observable through
 * typed_counting_alloc: the in-place consolidation allocates interiors and
 * never a leaf, the flatten-and-rebuild allocates leaves, and the
 * bottom-up repair allocates no leaf either (interiors only on a split).
 */
enum class expect { consolidation, rebuild, repair, any };

/* Delete `gone`, assert the expected recovery, check against the oracle. */
template <typename tree>
void delete_and_check(tree &t, oracle &ref, std::vector<uint32_t> const &gone,
					  std::vector<uint32_t> const &probes, uint64_t seed,
					  expect path)
{
	using tt = typename tree::type_trait;
	constexpr bool counted =
		std::is_same_v<typename tt::node_alloc, typed_counting_alloc>;

	[[maybe_unused]] auto const before = node_allocs<tree>();
	t.batch_delete(gone);
	for (auto k : gone) {
		ref.erase(k);
	}
	if constexpr (counted) {
		auto const after = node_allocs<tree>();
		switch (path) {
		case expect::consolidation:
			ASSERT_EQ(after.first, before.first)
				<< "consolidation allocated a leaf";
			ASSERT_GT(after.second, before.second)
				<< "no interior level was rebuilt";
			break;
		case expect::rebuild:
			ASSERT_GT(after.first, before.first) << "expected the rebuild";
			break;
		case expect::repair:
			ASSERT_EQ(after.first, before.first) << "repair allocated a leaf";
			break;
		case expect::any:
			break;
		}
	}
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
	ASSERT_NO_FATAL_FAILURE(check_lookups(t, ref, probes));
	ASSERT_NO_FATAL_FAILURE(check_ranges(t, ref, seed, 12));
}

/*
 * Build the keys 0..n-1 and delete all but every eighth in one batch. Each
 * build leaf keeps fewer than k_buf_lb entries, so every node and the root
 * go invalid with entries still live; each leaf-parent keeps at least
 * k_buf_lb, so the in-place consolidation applies. `hole` also empties one
 * whole root subtree. Then all but every 32nd, then everything.
 */
template <typename tree>
void consolidation_scenario(size_t n, bool hole, uint64_t seed,
							auto &&after_each)
{
	using tt = typename tree::type_trait;
	SCOPED_TRACE("consolidate k_buf=" +
				 std::to_string(static_cast<unsigned>(tt::k_buf)) +
				 " n=" + std::to_string(n) + (hole ? " hole" : "") +
				 " seed=" + std::to_string(seed));

	std::vector<uint32_t> keys(n);
	std::iota(keys.begin(), keys.end(), 0u);
	auto const all = to_entries(keys);

	tree t;
	oracle ref;
	t.build(all);
	add_to_oracle(ref, all);

	std::vector<uint32_t> probes;
	{
		std::mt19937_64 rng(seed);
		for (int i = 0; i < 200; ++i) {
			probes.push_back(static_cast<uint32_t>(rng() % (n + 64)));
		}
	}

	std::vector<uint32_t> gone;
	for (auto k : keys) {
		if (k % 8 != 0) {
			gone.push_back(k);
		}
	}
	if (hole) {
		auto const *root =
			static_cast<typename tree::interior_type const *>(t.get_root());
		ASSERT_FALSE(root->is_leaf);
		ASSERT_GE(root->get_node_n(), 3u) << "the hole needs 3 root children";
		uint32_t const lo = root->split[0];
		uint32_t const hi = root->split[1];
		for (auto k : keys) {
			if (k % 8 == 0 && k >= lo && k < hi) {
				gone.push_back(k);
			}
		}
	}
	ASSERT_NO_FATAL_FAILURE(delete_and_check(t, ref, gone, probes, seed ^ 1,
											 expect::consolidation));
	ASSERT_NO_FATAL_FAILURE(after_each(t, "first consolidation"));

	gone.clear();
	for (auto const &kv : ref) {
		if (kv.first % 32 != 0) {
			gone.push_back(kv.first);
		}
	}
	ASSERT_NO_FATAL_FAILURE(delete_and_check(t, ref, gone, probes, seed ^ 2,
											 expect::consolidation));
	ASSERT_NO_FATAL_FAILURE(after_each(t, "second consolidation"));

	/* Everything left: the empty-root branch, not a consolidation. */
	gone.clear();
	for (auto const &kv : ref) {
		gone.push_back(kv.first);
	}
	t.batch_delete(gone);
	ref.clear();
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
	ASSERT_TRUE(t.empty());
}

template <typename tree>
void consolidation_scenario(size_t n, bool hole, uint64_t seed)
{
	consolidation_scenario<tree>(n, hole, seed, [](tree &, char const *) {});
}

struct counted_opts : pab_tree::default_options {
	using node_alloc = typed_counting_alloc;
};

template <unsigned KBuf>
using counted_tree =
	pab_tree::btree<pab_tree::btree_trait<ele, KBuf, counted_opts>>;

TEST(SelfTest, CustomAllocator)
{
	struct opts : pab_tree::default_options {
		using node_alloc = counting_alloc;
	};
	using tt = pab_tree::btree_trait<ele, 16, opts>;
	using tree = pab_tree::btree<tt>;

	auto const seed = g_seed ^ 0xa110cULL;
	SCOPED_TRACE("custom allocator seed=" + std::to_string(seed));

	counting_alloc::live = 0;
	counting_alloc::total = 0;

	auto const keys = gen_keys(20000, dist::uniform, seed);
	auto const all = to_entries(keys);
	std::vector<entry> base(all.begin(), all.begin() + all.size() / 2);
	std::vector<entry> rest(all.begin() + all.size() / 2, all.end());

	{
		tree t;
		t.build(base);
		ASSERT_GT(counting_alloc::total.load(), 0)
			<< "the custom allocator was never called";
		ASSERT_GT(counting_alloc::live.load(), 0);

		t.batch_insert(rest);
		bool ok = t.template validate<true, false>();
		ASSERT_TRUE(ok);
		ASSERT_EQ(t.size(), keys.size());

		/*
		 * A tree of n entries with leaf size 16 holds at least n/k_buf_ub
		 * leaves, so the count has to be in a sane range.
		 */
		ASSERT_GE(counting_alloc::live.load(),
				  static_cast<long>(keys.size() / (4 * 16)));

		std::vector<uint32_t> half(keys.begin(),
								   keys.begin() + keys.size() / 2);
		std::sort(half.begin(), half.end());
		t.batch_delete(half);
		ok = t.template validate<true, false>();
		ASSERT_TRUE(ok);
	}

	/* The tree is gone; every node it took must have come back. */
	EXPECT_EQ(counting_alloc::live.load(), 0)
		<< "leaked " << counting_alloc::live.load() << " nodes of "
		<< counting_alloc::total.load();
}

/* ---- a custom key comparator orders the whole tree ---- */

TEST(SelfTest, ReverseComparator)
{
	/*
	 * std::greater: the tree is ordered descending, so every structural
	 * invariant, the descent, the sorts and validate() must follow it.
	 */
	struct desc : pab_tree::default_options {
		using key_compare = std::greater<uint32_t>;
	};
	using tt = pab_tree::btree_trait<ele, 16, desc>;
	using tree = pab_tree::btree<tt>;

	auto const seed = g_seed ^ 0x9e77ULL;
	SCOPED_TRACE("reverse comparator seed=" + std::to_string(seed));

	auto const keys = gen_keys(20000, dist::uniform, seed);
	auto const all = to_entries(keys);
	std::vector<entry> base(all.begin(), all.begin() + all.size() / 2);
	std::vector<entry> rest(all.begin() + all.size() / 2, all.end());

	/* The oracle uses the same order. */
	std::map<uint32_t, uint32_t, std::greater<uint32_t>> ref;
	auto add = [&](std::vector<entry> const &v) {
		for (auto const &e : v)
			ref[e.key] = e.val;
	};

	tree t;
	t.build(base);
	add(base);

	auto check = [&](char const *stage) {
		SCOPED_TRACE(stage);
		bool const ok = t.template validate<true, false>();
		ASSERT_TRUE(ok) << "validate() failed under std::greater";
		ASSERT_EQ(t.size(), ref.size());

		/* Iteration follows the comparator, so it runs high to low. */
		auto rt = ref.begin();
		size_t cnt = 0;
		for (auto it = t.begin(); it != t.end(); ++it, ++rt, ++cnt) {
			ASSERT_NE(rt, ref.end());
			ASSERT_EQ(it->key, rt->first) << "order diverged at " << cnt;
		}
		ASSERT_EQ(cnt, ref.size());

		/* to_seq is descending. */
		auto const seq = t.to_seq();
		ASSERT_EQ(seq.size(), ref.size());
		for (size_t i = 1; i < seq.size(); ++i)
			ASSERT_GT(ele::get_key(seq[i - 1]), ele::get_key(seq[i]))
				<< "to_seq not descending at " << i;

		/* Membership and the ordered lookups. */
		for (size_t i = 0; i < keys.size(); i += 97) {
			auto const k = keys[i];
			ASSERT_EQ(t.contain(k), ref.count(k) != 0);
			auto const lb = t.lower_bound(k);
			auto const rlb = ref.lower_bound(k);
			ASSERT_EQ(lb == t.end(), rlb == ref.end());
			if (rlb != ref.end()) {
				ASSERT_EQ(lb->key, rlb->first);
			}
		}
	};

	ASSERT_NO_FATAL_FAILURE(check("after build"));

	t.batch_insert(rest);
	add(rest);
	ASSERT_NO_FATAL_FAILURE(check("after batch_insert"));

	/* range_count takes its box in comparator order: [hi, lo) descending. */
	{
		std::vector<uint32_t> sorted_keys = keys;
		std::sort(sorted_keys.begin(), sorted_keys.end());
		std::mt19937_64 rng(seed);
		for (int b = 0; b < 20; ++b) {
			auto lo = sorted_keys[rng() % sorted_keys.size()];
			auto hi = sorted_keys[rng() % sorted_keys.size()];
			if (lo < hi)
				std::swap(lo, hi); /* first is the greater under std::greater */
			auto const want = static_cast<size_t>(
				std::distance(ref.lower_bound(lo), ref.lower_bound(hi)));
			ASSERT_EQ(t.range_count({ lo, hi }), want)
				<< "range_count [" << lo << ", " << hi << ")";
		}
	}

	/* Deleting still rebalances correctly under the comparator. */
	{
		std::vector<uint32_t> live = keys;
		std::mt19937_64 rng(seed ^ 7);
		std::shuffle(live.begin(), live.end(), rng);
		live.resize(live.size() / 2);
		t.batch_delete(live);
		for (auto k : live)
			ref.erase(k);
	}
	ASSERT_NO_FATAL_FAILURE(check("after batch_delete"));
}

/* ---- an invalid root is consolidated in place, not rebuilt ---- */

TEST(SelfTest, ConsolidateInvalidRoot)
{
	auto const seed = g_seed ^ 0xc0501ULL;
	for (bool hole : { false, true }) {
		ASSERT_NO_FATAL_FAILURE(
			consolidation_scenario<counted_tree<2>>(4096, hole, seed));
		ASSERT_NO_FATAL_FAILURE(
			consolidation_scenario<counted_tree<8>>(200000, hole, seed));
		ASSERT_NO_FATAL_FAILURE(
			consolidation_scenario<counted_tree<64>>(1000000, hole, seed));
	}
}

/* The root is the leaf-parent: it takes the no-valid-leaf fallback itself. */
TEST(SelfTest, ConsolidateInvalidRootHeight2)
{
	using tree = counted_tree<8>;
	auto const seed = g_seed ^ 0x4e162ULL;

	auto run = [&](size_t n, expect path, bool leaf_root_after) {
		SCOPED_TRACE("height-2 n=" + std::to_string(n));
		std::vector<uint32_t> keys(n);
		std::iota(keys.begin(), keys.end(), 0u);
		auto const all = to_entries(keys);

		tree t;
		oracle ref;
		t.build(all);
		add_to_oracle(ref, all);
		ASSERT_FALSE(t.get_root()->is_leaf);
		ASSERT_TRUE(static_cast<tree::interior_type const *>(t.get_root())
						->tree_nodes[0]
						->is_leaf)
			<< "the root must be the leaf-parent";

		std::vector<uint32_t> gone;
		for (auto k : keys) {
			if (k % 4 != 0) {
				gone.push_back(k);
			}
		}
		ASSERT_NO_FATAL_FAILURE(
			delete_and_check(t, ref, gone, keys, seed, path));
		ASSERT_EQ(t.get_root()->is_leaf, leaf_root_after);
	};

	/* 100 entries left of 400: consolidated, root keeps 6 leaves. */
	ASSERT_NO_FATAL_FAILURE(run(400, expect::consolidation, false));
	/* 25 left of 100: below k_buf_ub, the guard sends it to the rebuild. */
	ASSERT_NO_FATAL_FAILURE(run(100, expect::rebuild, true));
}

/* A leaf-parent left with 1..k_buf_lb-1 entries fails the guard: rebuild. */
TEST(SelfTest, ConsolidateFallback)
{
	using tree = counted_tree<8>;
	auto const seed = g_seed ^ 0xfa11ULL;
	SCOPED_TRACE("consolidate fallback seed=" + std::to_string(seed));

	{
		/*
		 * Leaf-parents span 256 consecutive keys. Keep every fourth key
		 * below 1000 (four leaf-parents at 58..64 entries, each leaf at
		 * 4) plus the single key 10000: one leaf-parent holding 1 entry.
		 */
		size_t const n = 20000;
		std::vector<uint32_t> keys(n);
		std::iota(keys.begin(), keys.end(), 0u);
		auto const all = to_entries(keys);

		tree t;
		oracle ref;
		t.build(all);
		add_to_oracle(ref, all);

		std::vector<uint32_t> gone;
		for (auto k : keys) {
			bool const keep = (k < 1000 && k % 4 == 0) || k == 10000;
			if (!keep) {
				gone.push_back(k);
			}
		}
		ASSERT_NO_FATAL_FAILURE(
			delete_and_check(t, ref, gone, keys, seed, expect::rebuild));
		ASSERT_EQ(t.size(), 251u);
	}

	{
		/* Random keys, 99.9% gone in one batch; whichever path, correct. */
		auto const keys = gen_keys(200000, dist::uniform, seed);
		auto const all = to_entries(keys);

		tree t;
		oracle ref;
		t.build(all);
		add_to_oracle(ref, all);

		std::vector<uint32_t> gone(keys.begin() + 200, keys.end());
		t.batch_delete(gone);
		for (auto k : gone) {
			ref.erase(k);
		}
		std::vector<uint32_t> probes(keys.begin(), keys.begin() + 400);
		ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
		ASSERT_NO_FATAL_FAILURE(check_lookups(t, ref, probes));
		ASSERT_NO_FATAL_FAILURE(check_ranges(t, ref, seed ^ 3, 12));
	}
}

/*
 * A chain of 10% batches with a collapse in the middle: five random
 * batches, then one that keeps every eighth survivor (every leaf ends
 * underfull, so the root goes invalid with live entries), then an insert
 * and the remaining batches against the rebuilt levels.
 *
 * Whether a random 10% batch collapses the root depends on the shape --
 * at 1e9 the sixth one does, at 4e5 with leaf 64 none does -- so the
 * collapse is forced rather than waited for.
 */
template <typename tree>
void ten_percent_chain(size_t n, uint64_t seed)
{
	using tt = typename tree::type_trait;
	SCOPED_TRACE(
		"chain k_buf=" + std::to_string(static_cast<unsigned>(tt::k_buf)) +
		" n=" + std::to_string(n) + " seed=" + std::to_string(seed));

	std::vector<uint32_t> keys(n);
	std::iota(keys.begin(), keys.end(), 0u);
	auto const all = to_entries(keys);

	tree t;
	oracle ref;
	t.build(all);
	add_to_oracle(ref, all);

	std::vector<uint32_t> order = keys;
	std::mt19937_64 rng(seed);
	std::shuffle(order.begin(), order.end(), rng);
	std::vector<uint32_t> probes(order.begin(), order.begin() + 200);
	probes.push_back(static_cast<uint32_t>(n + 7));

	size_t const step = n / 10;
	auto random_batch = [&](int b) {
		SCOPED_TRACE("batch " + std::to_string(b));
		std::vector<uint32_t> gone(order.begin() + b * step,
								   order.begin() + (b + 1) * step);
		/* Later batches may name keys the collapse already removed. */
		t.batch_delete(gone);
		for (auto k : gone) {
			ref.erase(k);
		}
		ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
		ASSERT_NO_FATAL_FAILURE(check_lookups(t, ref, probes));
	};

	for (int b = 0; b < 5; ++b) {
		ASSERT_NO_FATAL_FAILURE(random_batch(b));
	}

	/* Every leaf is valid here, so keeping an eighth empties all of them. */
	{
		std::vector<uint32_t> gone;
		for (auto const &kv : ref) {
			if (kv.first % 8 != 0) {
				gone.push_back(kv.first);
			}
		}
		ASSERT_NO_FATAL_FAILURE(delete_and_check(t, ref, gone, probes, seed ^ 5,
												 expect::consolidation));
	}

	/* Fresh keys into the rebuilt levels. */
	std::vector<uint32_t> fresh(5000);
	std::iota(fresh.begin(), fresh.end(), static_cast<uint32_t>(n));
	auto const fresh_e = to_entries(fresh);
	t.batch_insert(fresh_e);
	add_to_oracle(ref, fresh_e);
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));

	for (int b = 5; b < 10; ++b) {
		ASSERT_NO_FATAL_FAILURE(random_batch(b));
	}

	std::vector<uint32_t> rest;
	for (auto const &kv : ref) {
		rest.push_back(kv.first);
	}
	t.batch_delete(rest);
	ref.clear();
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
	ASSERT_TRUE(t.empty());
}

TEST(SelfTest, DeleteChainTenPercent)
{
	auto const seed = g_seed ^ 0xc4a1aULL;
	ASSERT_NO_FATAL_FAILURE(ten_percent_chain<counted_tree<8>>(100000, seed));
	ASSERT_NO_FATAL_FAILURE(ten_percent_chain<counted_tree<64>>(400000, seed));
}

/* The merged leaves and the new interiors carry a current augmentation. */
TEST(SelfTest, AugmentationAfterConsolidation)
{
	using tt =
		pab_tree::btree_type_trait<ele, 8, sum_leaf_aug, subtree_size_aug>;
	using tree = pab_tree::btree<tt>;
	auto const seed = g_seed ^ 0xa06c0ULL;

	ASSERT_NO_FATAL_FAILURE(consolidation_scenario<tree>(
		30000, false, seed, [](tree &t, char const *stage) {
			SCOPED_TRACE(stage);
			size_t leaves = 0;
			size_t interiors = 0;
			ASSERT_NE(t.get_root(), nullptr);
			ASSERT_NO_FATAL_FAILURE(
				check_aug<tree>(t.get_root(), leaves, interiors));
			ASSERT_GT(interiors, 0u) << "expected rebuilt interior levels";
		}));
}

/* Every old interior is freed exactly once, every leaf exactly once. */
TEST(SelfTest, ConsolidateLeakCheck)
{
	using tree = counted_tree<8>;
	auto const seed = g_seed ^ 0x1eacULL;

	ASSERT_NO_FATAL_FAILURE(consolidation_scenario<tree>(200000, true, seed));

	/* Counts are per node type and outlive every tree of this trait. */
	EXPECT_EQ(typed_counting_alloc::allocs<tree::leaf_type>.load(),
			  typed_counting_alloc::frees<tree::leaf_type>.load())
		<< "leaf leak or double free";
	EXPECT_EQ(typed_counting_alloc::allocs<tree::interior_type>.load(),
			  typed_counting_alloc::frees<tree::interior_type>.load())
		<< "interior leak or double free";
}

/*
 * Same scenario on plain new/delete. Under a sanitizer this is where a
 * freed node still being read, or freed twice, shows up; parlay's pool
 * recycles the block and would hide it.
 */
TEST(SelfTest, ConsolidateHeapAllocator)
{
	struct heap_opts : pab_tree::default_options {
		using node_alloc = heap_alloc;
	};
	using tree = pab_tree::btree<pab_tree::btree_trait<ele, 8, heap_opts>>;
	auto const seed = g_seed ^ 0x4ea9ULL;

	ASSERT_NO_FATAL_FAILURE(consolidation_scenario<tree>(200000, true, seed));
	ASSERT_NO_FATAL_FAILURE(ten_percent_chain<tree>(100000, seed));
}

/* The separators carried into the new levels follow the comparator. */
TEST(SelfTest, ConsolidateReverseComparator)
{
	struct desc : pab_tree::default_options {
		using key_compare = std::greater<uint32_t>;
	};
	using tree = pab_tree::btree<pab_tree::btree_trait<ele, 8, desc>>;

	auto const seed = g_seed ^ 0x9e77c0ULL;
	SCOPED_TRACE("consolidate under std::greater seed=" + std::to_string(seed));

	size_t const n = 30000;
	std::vector<uint32_t> keys(n);
	std::iota(keys.begin(), keys.end(), 0u);
	auto const all = to_entries(keys);

	std::map<uint32_t, uint32_t, std::greater<uint32_t>> ref;
	tree t;
	t.build(all);
	for (auto const &e : all) {
		ref[e.key] = e.val;
	}

	auto check = [&](char const *stage) {
		SCOPED_TRACE(stage);
		bool const ok = t.template validate<true, false>();
		ASSERT_TRUE(ok) << "validate() failed under std::greater";
		ASSERT_EQ(t.size(), ref.size());

		auto rt = ref.begin();
		size_t cnt = 0;
		for (auto it = t.begin(); it != t.end(); ++it, ++rt, ++cnt) {
			ASSERT_NE(rt, ref.end());
			ASSERT_EQ(it->key, rt->first) << "order diverged at " << cnt;
			ASSERT_EQ(it->val, rt->second);
		}
		ASSERT_EQ(cnt, ref.size());

		auto const seq = t.to_seq();
		ASSERT_EQ(seq.size(), ref.size());
		for (size_t i = 1; i < seq.size(); ++i) {
			ASSERT_GT(seq[i - 1].key, seq[i].key) << "not descending at " << i;
		}

		for (uint32_t k = 0; k < n + 64; k += 97) {
			ASSERT_EQ(t.contain(k), ref.count(k) != 0)
				<< "contain(" << k << ")";
		}
	};

	auto erase_unless_multiple_of = [&](uint32_t m) {
		std::vector<uint32_t> gone;
		for (auto const &kv : ref) {
			if (kv.first % m != 0) {
				gone.push_back(kv.first);
			}
		}
		t.batch_delete(gone);
		for (auto k : gone) {
			ref.erase(k);
		}
	};

	erase_unless_multiple_of(8);
	ASSERT_NO_FATAL_FAILURE(check("first consolidation"));
	erase_unless_multiple_of(32);
	ASSERT_NO_FATAL_FAILURE(check("second consolidation"));

	std::vector<uint32_t> rest;
	for (auto const &kv : ref) {
		rest.push_back(kv.first);
	}
	t.batch_delete(rest);
	ref.clear();
	ASSERT_NO_FATAL_FAILURE(check("emptied"));
	ASSERT_TRUE(t.empty());
}

/* ---- a valid root repairs the levels below it bottom-up ---- */

/*
 * The tree's leaf-parents in key order, each as its leaves' sorted keys,
 * plus the index of the root child each one sits under. Scenarios are
 * built from this map, so they do not depend on the exact bucket sizes.
 */
struct leaf_map {
	std::vector<std::vector<std::vector<uint32_t>>> parents;
	std::vector<size_t> root_child;
};

template <typename tree>
leaf_map leaf_parents_of(tree const &t)
{
	using interior = typename tree::interior_type;
	using leaf = typename tree::leaf_type;
	leaf_map m;
	auto const *root = t.get_root();
	if (root == nullptr || root->is_leaf) {
		return m;
	}
	auto walk = [&](auto const &self, auto const *n, size_t top) -> void {
		auto const *in = static_cast<interior const *>(n);
		if (in->get_node_n() > 0 && in->tree_nodes[0]->is_leaf) {
			std::vector<std::vector<uint32_t>> lp;
			for (size_t i = 0; i < in->get_node_n(); ++i) {
				auto const *lf = static_cast<leaf const *>(in->tree_nodes[i]);
				std::vector<uint32_t> ks;
				for (size_t k = 0; k < lf->size; ++k) {
					ks.push_back(lf->elements[k].key);
				}
				std::sort(ks.begin(), ks.end());
				lp.push_back(std::move(ks));
			}
			m.parents.push_back(std::move(lp));
			m.root_child.push_back(top);
			return;
		}
		for (size_t i = 0; i < in->get_node_n(); ++i) {
			self(self, in->tree_nodes[i], top);
		}
	};
	auto const *ri = static_cast<interior const *>(root);
	for (size_t i = 0; i < ri->get_node_n(); ++i) {
		walk(walk, ri->tree_nodes[i], i);
	}
	return m;
}

/* Every key under leaf-parent p, or of its leaf l. */
inline void take_parent(leaf_map const &m, size_t p, std::vector<uint32_t> &out)
{
	for (auto const &lf : m.parents[p]) {
		out.insert(out.end(), lf.begin(), lf.end());
	}
}

inline void take_leaf(leaf_map const &m, size_t p, size_t l,
					  std::vector<uint32_t> &out)
{
	out.insert(out.end(), m.parents[p][l].begin(), m.parents[p][l].end());
}

/*
 * One repair shape. Each is a deletion drawn from the leaf map, so that a
 * particular path in repair_rec / balance_over_children runs:
 *   thin      one leaf-parent keeps k_buf_lb - 1 leaves: merged into its
 *             neighbour by pointer moves
 *   seam      one leaf-parent keeps a single key: its lone underfull leaf
 *             is carried into the neighbour and merged there
 *   overflow  enough thin leaf-parents in a row that the group exceeds
 *             k_buf_ub children: re-bucketed with generate_interior_nodes
 *   empties   whole leaf-parents emptied, dropped without a trace
 *   chain     a whole root subtree emptied but for one key: a thin chain
 *             absorbed level by level, the key merged at the bottom
 */
enum class shape { thin, seam, overflow, empties, chain };

inline char const *shape_name(shape s)
{
	switch (s) {
	case shape::thin:
		return "thin";
	case shape::seam:
		return "seam";
	case shape::overflow:
		return "overflow";
	case shape::empties:
		return "empties";
	case shape::chain:
		return "chain";
	}
	return "?";
}

/* The keys to delete for `s` on a tree described by `m`. */
template <unsigned KBuf>
std::vector<uint32_t> repair_deletion(leaf_map const &m, shape s)
{
	constexpr size_t lb = KBuf;
	std::vector<uint32_t> gone;
	size_t const np = m.parents.size();
	EXPECT_GE(np, 8u) << "the scenario needs more leaf-parents";
	if (np < 8) {
		return gone;
	}

	switch (s) {
	case shape::thin: { /* parent 2 keeps lb - 1 leaves */
		size_t const p = 2;
		for (size_t l = lb - 1; l < m.parents[p].size(); ++l) {
			take_leaf(m, p, l, gone);
		}
		break;
	}
	case shape::seam: { /* parent 3 keeps one key */
		size_t const p = 3;
		for (size_t l = 0; l < m.parents[p].size(); ++l) {
			take_leaf(m, p, l, gone);
		}
		gone.erase(std::find(gone.begin(), gone.end(), m.parents[p][0][0]));
		break;
	}
	case shape::overflow: {
		/*
		 * r thin parents of lb - 1 leaves before a full one: the group
		 * holds r * (lb - 1) + >= 2 lb children, more than 4 lb once
		 * r >= (2 lb + 1) / (lb - 1) rounded up.
		 */
		size_t const r = (2 * lb + 1 + lb - 2) / (lb - 1);
		for (size_t p = 1; p <= r; ++p) {
			for (size_t l = lb - 1; l < m.parents[p].size(); ++l) {
				take_leaf(m, p, l, gone);
			}
		}
		break;
	}
	case shape::empties: { /* parents 2..4 emptied, parent 5 thinned */
		for (size_t p = 2; p <= 4; ++p) {
			take_parent(m, p, gone);
		}
		for (size_t l = lb - 1; l < m.parents[5].size(); ++l) {
			take_leaf(m, 5, l, gone);
		}
		break;
	}
	case shape::chain: {
		/* every parent under root child 1, but one key of the first */
		bool first = true;
		for (size_t p = 0; p < np; ++p) {
			if (m.root_child[p] != 1) {
				continue;
			}
			take_parent(m, p, gone);
			if (first) {
				gone.erase(
					std::find(gone.begin(), gone.end(), m.parents[p][0][0]));
				first = false;
			}
		}
		EXPECT_FALSE(first) << "root child 1 has no leaf-parent";
		break;
	}
	}
	return gone;
}

/*
 * Build 0..n-1, apply one repair shape, check; then a random 30% (more
 * repairs on the repaired structure), then everything.
 */
template <typename tree>
void repair_scenario(size_t n, shape s, uint64_t seed, auto &&after_each)
{
	using tt = typename tree::type_trait;
	SCOPED_TRACE("repair " + std::string(shape_name(s)) +
				 " k_buf=" + std::to_string(static_cast<unsigned>(tt::k_buf)) +
				 " n=" + std::to_string(n) +
				 " delete=" + policy_name<typename tt::delete_policy>() +
				 " seed=" + std::to_string(seed));

	std::vector<uint32_t> keys(n);
	std::iota(keys.begin(), keys.end(), 0u);
	auto const all = to_entries(keys);

	tree t;
	oracle ref;
	t.build(all);
	add_to_oracle(ref, all);

	std::vector<uint32_t> probes;
	{
		std::mt19937_64 rng(seed);
		for (int i = 0; i < 200; ++i) {
			probes.push_back(static_cast<uint32_t>(rng() % (n + 64)));
		}
	}

	auto const m = leaf_parents_of(t);
	auto gone = repair_deletion<tt::k_buf>(m, s);
	ASSERT_FALSE(gone.empty());
	/* graft may split a leaf when it reinserts entries */
	constexpr expect path =
		std::is_same_v<typename tt::delete_policy, graft_policy> ?
			expect::any :
			expect::repair;
	ASSERT_NO_FATAL_FAILURE(
		delete_and_check(t, ref, gone, probes, seed ^ 1, path));
	ASSERT_NO_FATAL_FAILURE(after_each(t, "after the shape"));

	gone.clear();
	{
		std::mt19937_64 rng(seed ^ 2);
		for (auto const &kv : ref) {
			if (rng() % 10 < 3) {
				gone.push_back(kv.first);
			}
		}
	}
	ASSERT_NO_FATAL_FAILURE(
		delete_and_check(t, ref, gone, probes, seed ^ 3, expect::any));
	ASSERT_NO_FATAL_FAILURE(after_each(t, "after the random 30%"));

	gone.clear();
	for (auto const &kv : ref) {
		gone.push_back(kv.first);
	}
	t.batch_delete(gone);
	ref.clear();
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
	ASSERT_TRUE(t.empty());
}

template <typename tree>
void repair_scenario(size_t n, shape s, uint64_t seed)
{
	repair_scenario<tree>(n, s, seed, [](tree &, char const *) {});
}

constexpr shape k_shapes[] = { shape::thin, shape::seam, shape::overflow,
							   shape::empties, shape::chain };

TEST(SelfTest, RepairShapes)
{
	auto const seed = g_seed ^ 0x2e9a12ULL;
	for (shape s : k_shapes) {
		ASSERT_NO_FATAL_FAILURE(
			repair_scenario<counted_tree<2>>(4096, s, seed));
		ASSERT_NO_FATAL_FAILURE(
			repair_scenario<counted_tree<8>>(200000, s, seed));
		ASSERT_NO_FATAL_FAILURE(
			repair_scenario<counted_tree<64>>(2000000, s, seed));
	}
}

/* Merged and re-bucketed interiors carry a current augmentation. */
TEST(SelfTest, RepairAugmentation)
{
	using tt =
		pab_tree::btree_type_trait<ele, 8, sum_leaf_aug, subtree_size_aug>;
	using tree = pab_tree::btree<tt>;
	auto const seed = g_seed ^ 0xa062e9ULL;

	for (shape s : k_shapes) {
		ASSERT_NO_FATAL_FAILURE(repair_scenario<tree>(
			200000, s, seed, [](tree &t, char const *stage) {
				SCOPED_TRACE(stage);
				size_t leaves = 0;
				size_t interiors = 0;
				ASSERT_NE(t.get_root(), nullptr);
				ASSERT_NO_FATAL_FAILURE(
					check_aug<tree>(t.get_root(), leaves, interiors));
			}));
	}
}

/* Every absorbed or re-bucketed interior is freed exactly once. */
TEST(SelfTest, RepairLeakCheck)
{
	using tree = counted_tree<8>;
	auto const seed = g_seed ^ 0x1eac2eULL;

	for (shape s : k_shapes) {
		ASSERT_NO_FATAL_FAILURE(repair_scenario<tree>(200000, s, seed));
	}
	EXPECT_EQ(typed_counting_alloc::allocs<tree::leaf_type>.load(),
			  typed_counting_alloc::frees<tree::leaf_type>.load())
		<< "leaf leak or double free";
	EXPECT_EQ(typed_counting_alloc::allocs<tree::interior_type>.load(),
			  typed_counting_alloc::frees<tree::interior_type>.load())
		<< "interior leak or double free";
}

/* The same shapes on plain new/delete, for the sanitizer build. */
TEST(SelfTest, RepairHeapAllocator)
{
	struct heap_opts : pab_tree::default_options {
		using node_alloc = heap_alloc;
	};
	using tree = pab_tree::btree<pab_tree::btree_trait<ele, 8, heap_opts>>;
	auto const seed = g_seed ^ 0x4ea92eULL;

	for (shape s : k_shapes) {
		ASSERT_NO_FATAL_FAILURE(repair_scenario<tree>(200000, s, seed));
	}
}

template <typename Policy>
struct desc_opts : pab_tree::default_options {
	using key_compare = std::greater<uint32_t>;
	using delete_policy = Policy;
};

/* Separators carried between siblings follow the comparator. */
template <typename Policy>
void repair_reverse_scenario()
{
	using tree =
		pab_tree::btree<pab_tree::btree_trait<ele, 8, desc_opts<Policy>>>;
	auto const seed = g_seed ^ 0x9e772eULL;

	for (shape s : k_shapes) {
		SCOPED_TRACE(std::string("repair under std::greater, ") +
					 shape_name(s) + " delete=" + policy_name<Policy>() +
					 " seed=" + std::to_string(seed));
		size_t const n = 200000;
		std::vector<uint32_t> keys(n);
		std::iota(keys.begin(), keys.end(), 0u);
		auto const all = to_entries(keys);

		std::map<uint32_t, uint32_t, std::greater<uint32_t>> ref;
		tree t;
		t.build(all);
		for (auto const &e : all) {
			ref[e.key] = e.val;
		}

		auto check = [&](char const *stage) {
			SCOPED_TRACE(stage);
			bool const ok = t.template validate<true, false>();
			ASSERT_TRUE(ok) << "validate() failed under std::greater";
			ASSERT_EQ(t.size(), ref.size());
			auto rt = ref.begin();
			size_t cnt = 0;
			for (auto it = t.begin(); it != t.end(); ++it, ++rt, ++cnt) {
				ASSERT_NE(rt, ref.end());
				ASSERT_EQ(it->key, rt->first) << "order diverged at " << cnt;
			}
			ASSERT_EQ(cnt, ref.size());
			for (uint32_t k = 0; k < n + 64; k += 97) {
				ASSERT_EQ(t.contain(k), ref.count(k) != 0)
					<< "contain(" << k << ")";
			}
		};

		auto const m = leaf_parents_of(t);
		auto gone = repair_deletion<8>(m, s);
		ASSERT_FALSE(gone.empty());
		t.batch_delete(gone);
		for (auto k : gone) {
			ref.erase(k);
		}
		ASSERT_NO_FATAL_FAILURE(check("after the shape"));

		gone.clear();
		for (auto const &kv : ref) {
			gone.push_back(kv.first);
		}
		t.batch_delete(gone);
		ref.clear();
		ASSERT_NO_FATAL_FAILURE(check("emptied"));
		ASSERT_TRUE(t.empty());
	}
}

TEST(SelfTest, RepairReverseComparator)
{
	ASSERT_NO_FATAL_FAILURE(repair_reverse_scenario<pab_tree::repair_delete>());
}

/* ---- edge cases: the sizes where the structure changes shape ---- */

template <unsigned KBuf, typename Policy = pab_tree::repair_delete>
void small_sizes()
{
	for (size_t n :
		 { 0u,	 1u,   2u,	 3u,   4u,	 5u,   7u,	 8u,	9u,
		   15u,	 16u,  17u,	 31u,  32u,	 33u,  63u,	 64u,	65u,
		   127u, 128u, 129u, 255u, 256u, 257u, 511u, 1023u, 1025u }) {
		run_scenario<KBuf, Policy>(n, dist::sequential, g_seed + n);
		run_scenario<KBuf, Policy>(n, dist::uniform, g_seed + n + 1);
	}
}

TEST(SelfTest, SmallSizesLeaf2)
{
	small_sizes<2>();
}

TEST(SelfTest, SmallSizesLeaf4)
{
	small_sizes<4>();
}

TEST(SelfTest, SmallSizesLeaf8)
{
	small_sizes<8>();
}

TEST(SelfTest, SmallSizesLeaf32)
{
	small_sizes<32>();
}

TEST(SelfTest, SmallSizesLeaf64)
{
	small_sizes<64>();
}

/* ---- the shapes, at a size that builds a real multi-level tree ---- */

template <unsigned KBuf, typename Policy = pab_tree::repair_delete>
void shapes(size_t n)
{
	for (int r = 0; r < g_rounds; ++r) {
		auto const s = g_seed + static_cast<uint64_t>(r) * 7919;
		run_scenario<KBuf, Policy>(n, dist::uniform, s);
		run_scenario<KBuf, Policy>(n, dist::clustered, s + 1);
		run_scenario<KBuf, Policy>(n, dist::sequential, s + 2);
		run_scenario<KBuf, Policy>(n, dist::sparse, s + 3);
		run_scenario<KBuf, Policy>(n, dist::boundary, s + 4);
	}
}

TEST(SelfTest, ShapesLeaf2)
{
	shapes<2>(20000);
}

TEST(SelfTest, ShapesLeaf8)
{
	shapes<8>(50000);
}

TEST(SelfTest, ShapesLeaf64)
{
	shapes<64>(200000);
}

TEST(SelfTest, ShapesLeaf256)
{
	shapes<256>(200000);
}

/* ---- many small batches against one tree ---- */

template <typename Policy>
void churn()
{
	using tt =
		pab_tree::btree_trait<ele, 16,
							  with_policy<pab_tree::default_options, Policy>>;
	using tree = pab_tree::btree<tt>;

	auto const seed = g_seed ^ 0xc0ffeeULL;
	SCOPED_TRACE("churn delete=" + std::string(policy_name<Policy>()) +
				 " seed=" + std::to_string(seed) +
				 "  (rerun: ./self_test --seed=" + std::to_string(g_seed) +
				 " --gtest_filter=SelfTest.*Churn)");

	std::mt19937_64 rng(seed);
	tree t;
	oracle ref;

	auto const pool = gen_keys(60000, dist::uniform, seed);

	for (int round = 0; round < 40; ++round) {
		SCOPED_TRACE("round " + std::to_string(round));

		/* Insert a random slice of the pool that is not in the tree. */
		std::vector<entry> add;
		for (size_t i = 0; i < 800; ++i) {
			auto const k = pool[rng() % pool.size()];
			if (ref.count(k) == 0) {
				add.push_back(entry{ k, k ^ 0x5bf03635u });
				ref[k] = k ^ 0x5bf03635u;
			}
		}
		/* A batch must itself hold distinct keys. */
		std::sort(add.begin(), add.end(),
				  [](entry const &a, entry const &b) { return a.key < b.key; });
		add.erase(std::unique(add.begin(), add.end(),
							  [](entry const &a, entry const &b) {
								  return a.key == b.key;
							  }),
				  add.end());

		if (!add.empty()) {
			if (t.empty()) {
				t.build(add);
			} else {
				t.batch_insert(add);
			}
		}

		/* Delete a random slice of what is live. */
		std::vector<uint32_t> live;
		for (auto const &kv : ref) {
			live.push_back(kv.first);
		}
		if (!live.empty()) {
			std::shuffle(live.begin(), live.end(), rng);
			live.resize(std::min<size_t>(live.size(), 400));
			std::sort(live.begin(), live.end());
			t.batch_delete(live);
			for (auto k : live) {
				ref.erase(k);
			}
		}

		ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
	}

	ASSERT_NO_FATAL_FAILURE(check_ranges(t, ref, seed ^ 5, 20));
}

TEST(SelfTest, Churn)
{
	ASSERT_NO_FATAL_FAILURE(churn<pab_tree::repair_delete>());
}

/* ---- augmentation stays current through every path ---- */

TEST(SelfTest, Augmentation)
{
	using tt =
		pab_tree::btree_type_trait<ele, 8, sum_leaf_aug, subtree_size_aug>;
	using tree = pab_tree::btree<tt>;

	auto const seed = g_seed ^ 0xa06ULL;
	SCOPED_TRACE("augmentation seed=" + std::to_string(seed));

	auto const keys = gen_keys(30000, dist::uniform, seed);
	auto const all = to_entries(keys);
	std::vector<entry> base(all.begin(), all.begin() + all.size() / 2);
	std::vector<entry> rest(all.begin() + all.size() / 2, all.end());

	tree t;
	size_t leaves = 0;
	size_t interiors = 0;

	auto verify = [&](char const *stage) {
		SCOPED_TRACE(stage);
		leaves = interiors = 0;
		ASSERT_NE(t.get_root(), nullptr);
		ASSERT_NO_FATAL_FAILURE(
			check_aug<tree>(t.get_root(), leaves, interiors));
		ASSERT_GT(leaves, 0u);
	};

	t.build(base);
	ASSERT_NO_FATAL_FAILURE(verify("after build"));
	ASSERT_GT(interiors, 0u) << "expected a multi-level tree";

	t.batch_insert(rest);
	ASSERT_NO_FATAL_FAILURE(verify("after batch_insert"));

	/* Delete enough to force merges and redistribution. */
	{
		std::vector<uint32_t> live = keys;
		std::mt19937_64 rng(seed);
		std::shuffle(live.begin(), live.end(), rng);
		live.resize(live.size() * 3 / 4);
		std::sort(live.begin(), live.end());
		t.batch_delete(live);
	}
	ASSERT_NO_FATAL_FAILURE(verify("after batch_delete"));
}

/* ---- copy, move and assignment ---- */

TEST(SelfTest, ValueSemantics)
{
	using tt = pab_tree::btree_type_trait<ele, 16>;
	using tree = pab_tree::btree<tt>;

	auto const e = to_entries(gen_keys(5000, dist::uniform, g_seed ^ 0x5e));
	oracle ref;
	add_to_oracle(ref, e);

	tree a;
	a.build(e);
	ASSERT_NO_FATAL_FAILURE(check_against(a, ref));

	/* Copy is deep: mutating the copy leaves the original alone. */
	tree b = a;
	ASSERT_NO_FATAL_FAILURE(check_against(b, ref));
	ASSERT_TRUE(a == b);

	/* Drop the 1000 smallest keys from the copy. */
	std::vector<uint32_t> drop;
	for (auto const &kv : ref) {
		drop.push_back(kv.first);
		if (drop.size() == 1000) {
			break;
		}
	}
	b.batch_delete(drop);
	ASSERT_EQ(a.size(), ref.size()) << "deleting from the copy touched a";
	ASSERT_EQ(b.size(), ref.size() - drop.size());
	ASSERT_FALSE(a == b);
	ASSERT_NO_FATAL_FAILURE(check_against(a, ref));

	/*
	 * Ordering is lexicographic over the contents in key order, as in
	 * std::set. b lost its smallest keys, so it now starts higher.
	 */
	ASSERT_TRUE(a < b);
	ASSERT_TRUE(b > a);
	ASSERT_TRUE(a <= b);
	ASSERT_FALSE(b <= a);

	/* A prefix is less than what it is a prefix of. */
	{
		tree pre = a;
		std::vector<uint32_t> tail;
		for (auto it = ref.rbegin(); it != ref.rend() && tail.size() < 1000;
			 ++it) {
			tail.push_back(it->first);
		}
		std::sort(tail.begin(), tail.end());
		pre.batch_delete(tail);
		ASSERT_TRUE(pre < a);
		ASSERT_TRUE(a > pre);
	}

	/* Move leaves the source empty and usable. */
	tree c = std::move(a);
	ASSERT_NO_FATAL_FAILURE(check_against(c, ref));
	ASSERT_TRUE(a.empty());

	a = c;
	ASSERT_TRUE(a == c);

	/* Self-assignment must not free the tree. */
	tree *p = &c;
	c = *p;
	ASSERT_NO_FATAL_FAILURE(check_against(c, ref));

	/* swap and clear. */
	tree d;
	d.swap(c);
	ASSERT_TRUE(c.empty());
	ASSERT_NO_FATAL_FAILURE(check_against(d, ref));
	d.clear();
	ASSERT_TRUE(d.empty());
	ASSERT_EQ(d.begin(), d.end());
}

/* ---- single-element and iterator modifiers ---- */

TEST(SelfTest, SingleElementOps)
{
	using tt = pab_tree::btree_type_trait<ele, 8>;
	using tree = pab_tree::btree<tt>;

	auto const keys = gen_keys(3000, dist::uniform, g_seed ^ 0x51);
	tree t;
	oracle ref;
	t.build(to_entries(keys));
	add_to_oracle(ref, to_entries(keys));

	std::mt19937_64 rng(g_seed ^ 0x51);
	for (int i = 0; i < 40; ++i) {
		auto const k = keys[rng() % keys.size()];
		bool const was_there = ref.count(k) != 0;
		ASSERT_EQ(t.erase(k), was_there) << "erase(key) return value";
		ref.erase(k);
		ASSERT_EQ(t.size(), ref.size());

		uint32_t const fresh = 0x40000000u + static_cast<uint32_t>(i);
		t.insert(entry{ fresh, fresh });
		ref[fresh] = fresh;
		ASSERT_EQ(t.size(), ref.size());
	}
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));

	/* erase(iterator) and erase(first, last). */
	t.erase(t.begin());
	ref.erase(ref.begin());
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));

	auto const lo_key = std::next(ref.begin(), 100)->first;
	auto const hi_key = std::next(ref.begin(), 600)->first;
	t.erase(t.lower_bound(lo_key), t.lower_bound(hi_key));
	ref.erase(ref.lower_bound(lo_key), ref.lower_bound(hi_key));
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
}

/* ---- an empty and a default-constructed tree answer every query ---- */

TEST(SelfTest, EmptyTree)
{
	using tt = pab_tree::btree_type_trait<ele, 32>;
	using tree = pab_tree::btree<tt>;

	tree t;
	oracle ref;
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
	ASSERT_FALSE(t.contain(0));
	ASSERT_FALSE(t.contains(12345));
	ASSERT_EQ(t.count(7), 0u);
	ASSERT_EQ(t.range_count({ 0, 0xFFFFFFFFu }), 0u);
	ASSERT_EQ(t.find(1), t.end());
	ASSERT_EQ(t.lower_bound(1), t.end());
	ASSERT_EQ(t.upper_bound(1), t.end());
	ASSERT_EQ(t.begin(), t.end());
	ASSERT_EQ(t.rbegin(), t.rend());
	ASSERT_EQ(t.to_seq().size(), 0u);
	ASSERT_FALSE(t.erase(5u));

	/* Emptied by deletion, not merely never filled. */
	auto const e = to_entries(gen_keys(1000, dist::uniform, g_seed));
	t.build(e);
	std::vector<uint32_t> all;
	for (auto const &x : e) {
		all.push_back(x.key);
	}
	t.batch_delete(all);
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
	ASSERT_FALSE(t.contain(e[0].key));
	ASSERT_EQ(t.begin(), t.end());

	/* And usable again afterwards. */
	t.batch_insert(e);
	add_to_oracle(ref, e);
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
}

/* ---- graft_delete: the delete suites again, and shapes built by hand ---- */

template <unsigned KBuf>
using counted_graft_tree = pab_tree::btree<
	pab_tree::btree_trait<ele, KBuf, with_policy<counted_opts, graft_policy>>>;

TEST(SelfTest, GraftSmallSizes)
{
	small_sizes<2, graft_policy>();
	small_sizes<4, graft_policy>();
	small_sizes<8, graft_policy>();
	small_sizes<32, graft_policy>();
	small_sizes<64, graft_policy>();
}

TEST(SelfTest, GraftShapes)
{
	shapes<2, graft_policy>(20000);
	shapes<8, graft_policy>(50000);
	shapes<64, graft_policy>(200000);
	shapes<256, graft_policy>(200000);
}

TEST(SelfTest, GraftRepairShapes)
{
	auto const seed = g_seed ^ 0x2e9a12ULL;
	for (shape s : k_shapes) {
		ASSERT_NO_FATAL_FAILURE(
			repair_scenario<counted_graft_tree<2>>(4096, s, seed));
		ASSERT_NO_FATAL_FAILURE(
			repair_scenario<counted_graft_tree<8>>(200000, s, seed));
		ASSERT_NO_FATAL_FAILURE(
			repair_scenario<counted_graft_tree<64>>(2000000, s, seed));
	}
}

TEST(SelfTest, GraftAugmentation)
{
	struct opts : pab_tree::default_options {
		using leaf_aug = sum_leaf_aug;
		using interior_aug = subtree_size_aug;
		using delete_policy = graft_policy;
	};
	using tree = pab_tree::btree<pab_tree::btree_trait<ele, 8, opts>>;
	auto const seed = g_seed ^ 0xa062e9ULL;

	for (shape s : k_shapes) {
		ASSERT_NO_FATAL_FAILURE(repair_scenario<tree>(
			200000, s, seed, [](tree &t, char const *stage) {
				SCOPED_TRACE(stage);
				size_t leaves = 0;
				size_t interiors = 0;
				ASSERT_NE(t.get_root(), nullptr);
				ASSERT_NO_FATAL_FAILURE(
					check_aug<tree>(t.get_root(), leaves, interiors));
			}));
	}
}

TEST(SelfTest, GraftReverseComparator)
{
	ASSERT_NO_FATAL_FAILURE(repair_reverse_scenario<graft_policy>());
}

TEST(SelfTest, GraftChainTenPercent)
{
	auto const seed = g_seed ^ 0xc4a1aULL;
	ASSERT_NO_FATAL_FAILURE(
		ten_percent_chain<counted_graft_tree<8>>(100000, seed));
	ASSERT_NO_FATAL_FAILURE(
		ten_percent_chain<counted_graft_tree<64>>(400000, seed));
}

TEST(SelfTest, GraftChurn)
{
	ASSERT_NO_FATAL_FAILURE(churn<graft_policy>());
}

namespace
{
/*
 * A subtree to build node by node: a leaf of `entries` keys when kids is
 * empty. The test deletes every key under a doomed subtree.
 */
struct shape_spec {
	size_t entries = 0;
	std::vector<shape_spec> kids;
	bool doomed = false;
};

shape_spec leaf_of(size_t n)
{
	return shape_spec{ n, {}, false };
}

shape_spec node_of(std::vector<shape_spec> kids)
{
	return shape_spec{ 0, std::move(kids), false };
}

shape_spec doomed(shape_spec s)
{
	s.doomed = true;
	return s;
}

/* h levels of k children over leaves of k entries; valid for k = k_buf_lb. */
shape_spec thinnest(size_t h, size_t k)
{
	if (h == 0) {
		return leaf_of(k);
	}
	return node_of(std::vector<shape_spec>(k, thinnest(h - 1, k)));
}

std::vector<shape_spec> times(size_t n, shape_spec const &s)
{
	return std::vector<shape_spec>(n, s);
}

std::vector<shape_spec> cat(std::vector<shape_spec> a,
							std::vector<shape_spec> const &b)
{
	a.insert(a.end(), b.begin(), b.end());
	return a;
}

/* Keys 1, 2, ... in key order; a separator is the first key after it. */
template <typename tree>
struct shape_builder {
	using tt = typename tree::type_trait;

	uint32_t next = 1;
	oracle ref;
	std::vector<uint32_t> doomed_keys;

	typename tt::node_type *make(shape_spec const &s, bool dead = false)
	{
		dead = dead || s.doomed;
		if (s.kids.empty()) {
			std::vector<entry> es;
			for (size_t i = 0; i < s.entries; ++i, ++next) {
				es.push_back(entry{ next, next ^ 0x5bf03635u });
				ref[next] = next ^ 0x5bf03635u;
				if (dead) {
					doomed_keys.push_back(next);
				}
			}
			auto in = parlay::make_slice(es.data(), es.data() + es.size());
			return pab_tree::alloc_normal_leaf_node<decltype(in),
													typename tree::leaf_type>(
				in);
		}
		typename tt::node_seq kids;
		typename tt::split_seq seps;
		for (size_t i = 0; i < s.kids.size(); ++i) {
			if (i > 0) {
				seps.push_back(next);
			}
			kids.push_back(make(s.kids[i], dead));
		}
		auto *n =
			pab_tree::alloc_interior_node<typename tree::interior_type, tt>(
				kids, seps);
		n->update_aug();
		return n;
	}
};

template <typename tree>
void free_subtree(typename tree::node_type *n)
{
	if (n->is_leaf) {
		pab_tree::free_node<typename tree::leaf_type>(n);
		return;
	}
	auto *in = static_cast<typename tree::interior_type *>(n);
	for (size_t i = 0; i < in->get_node_n(); ++i) {
		free_subtree<tree>(in->tree_nodes[i]);
	}
	pab_tree::free_node<typename tree::interior_type>(n);
}

/* t takes the shape; its root node keeps its address. */
template <typename tree>
void install(tree &t, shape_builder<tree> &b, shape_spec const &s)
{
	using interior = typename tree::interior_type;
	std::vector<uint32_t> keys(4096);
	std::iota(keys.begin(), keys.end(), 0u);
	t.build(to_entries(keys));
	ASSERT_FALSE(t.get_root()->is_leaf);
	auto *root = static_cast<interior *>(t.get_root());
	for (size_t i = 0; i < root->get_node_n(); ++i) {
		free_subtree<tree>(root->tree_nodes[i]);
	}
	auto *fresh = static_cast<interior *>(b.make(s));
	*root = *fresh;
	pab_tree::free_node<interior>(fresh);
}

template <typename tree>
size_t tree_height(tree const &t)
{
	size_t h = 0;
	for (auto const *n = t.get_root(); !n->is_leaf; ++h) {
		n = static_cast<typename tree::interior_type const *>(n)->tree_nodes[0];
	}
	return h;
}

/*
 * Install s, delete its doomed keys, check against the oracle. A nonzero
 * height_after is the height the tree must have afterwards.
 */
template <typename tree>
void hand_shape(shape_spec const &s, char const *name, size_t height_after = 0)
{
	using tt = typename tree::type_trait;
	SCOPED_TRACE(std::string("shape ") + name +
				 " k_buf=" + std::to_string(static_cast<unsigned>(tt::k_buf)) +
				 " delete=" + policy_name<typename tt::delete_policy>());

	tree t;
	shape_builder<tree> b;
	ASSERT_NO_FATAL_FAILURE(install(t, b, s));
	oracle ref = b.ref;
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
	ASSERT_FALSE(b.doomed_keys.empty());

	std::vector<uint32_t> probes(b.next + 1);
	std::iota(probes.begin(), probes.end(), 0u);
	ASSERT_NO_FATAL_FAILURE(
		delete_and_check(t, ref, b.doomed_keys, probes, g_seed, expect::any));
	if (height_after != 0) {
		ASSERT_EQ(tree_height(t), height_after);
	}
}

/*
 * Each shape makes a valid node X invalid under the tomb T, for leaf size
 * B (k_buf_lb = B):
 *   order   X holds, in key order, leaf q, leaf-parent P, leaf v; P must
 *           be grafted before q and v, or v ends up inside q's node
 *   edge    X is the leftmost node: all items lie left of T's content
 *   inner   the same one level down, below the root
 *   raised  X keeps only a valid V whose first child went; V's bound rises
 */
template <unsigned B>
struct hand_shapes {
	/* a leaf-parent that keeps one leaf of B entries */
	static shape_spec one_leaf()
	{
		return node_of(cat({ leaf_of(B) }, times(B - 1, doomed(leaf_of(B)))));
	}

	/* height 2: q, P, v, then doomed leaf-parents up to B children */
	static shape_spec x2()
	{
		return node_of(cat({ one_leaf(), thinnest(1, B), one_leaf() },
						   times(B > 3 ? B - 3 : 0, doomed(thinnest(1, B)))));
	}

	static shape_spec order()
	{
		return node_of(
			{ thinnest(2, B), thinnest(2, B), x2(), thinnest(2, B) });
	}

	static shape_spec edge()
	{
		return node_of({ x2(), thinnest(2, B), thinnest(2, B) });
	}

	static shape_spec inner()
	{
		auto const tomb = node_of(cat({ x2() }, times(B, thinnest(2, B))));
		return node_of({ thinnest(3, B), tomb, thinnest(3, B) });
	}

	static shape_spec raised()
	{
		auto const v = node_of(cat({ one_leaf() }, times(B, thinnest(1, B))));
		auto const x =
			node_of(cat({ v }, times(B - 1, doomed(thinnest(2, B)))));
		return node_of({ thinnest(3, B), thinnest(3, B), x, thinnest(3, B) });
	}
};

/*
 * B = 4. The tomb is the root, with 16 children; X has 3 full valid
 * children and 13 invalid ones holding 3 valid children each. Grafting
 * splits X's left neighbour c, then the half of c that takes the lower
 * items, so the root ends with 17 children and splits.
 */
shape_spec tomb_overflow()
{
	constexpr size_t b = 4;
	auto const lp = thinnest(1, b);
	auto const full2 = node_of(times(4 * b, lp));
	auto const y = node_of(cat(times(3, lp), times(13, doomed(lp))));
	auto const x =
		node_of({ y, y, y, y, full2, y, y, y, y, full2, y, y, y, y, full2, y });
	auto const c = node_of(cat(times(15, thinnest(2, b)), { full2 }));
	return node_of(
		cat(cat(times(7, thinnest(3, b)), { c, x }), times(7, thinnest(3, b))));
}

template <unsigned B, typename Policy>
using hand_tree = pab_tree::btree<
	pab_tree::btree_trait<ele, B, with_policy<counted_opts, Policy>>>;

template <unsigned B, typename Policy>
void hand_shapes_at()
{
	using tree = hand_tree<B, Policy>;
	ASSERT_NO_FATAL_FAILURE(hand_shape<tree>(hand_shapes<B>::order(), "order"));
	ASSERT_NO_FATAL_FAILURE(hand_shape<tree>(hand_shapes<B>::edge(), "edge"));
	ASSERT_NO_FATAL_FAILURE(hand_shape<tree>(hand_shapes<B>::inner(), "inner"));
	ASSERT_NO_FATAL_FAILURE(
		hand_shape<tree>(hand_shapes<B>::raised(), "raised"));
	EXPECT_EQ(typed_counting_alloc::allocs<typename tree::leaf_type>.load(),
			  typed_counting_alloc::frees<typename tree::leaf_type>.load())
		<< "leaf leak or double free";
	EXPECT_EQ(typed_counting_alloc::allocs<typename tree::interior_type>.load(),
			  typed_counting_alloc::frees<typename tree::interior_type>.load())
		<< "interior leak or double free";
}
} /* namespace */

TEST(SelfTest, GraftHandShapes)
{
	ASSERT_NO_FATAL_FAILURE((hand_shapes_at<2, graft_policy>()));
	ASSERT_NO_FATAL_FAILURE((hand_shapes_at<4, graft_policy>()));
	ASSERT_NO_FATAL_FAILURE((hand_shapes_at<8, graft_policy>()));
	/* the same shapes through repair_rec */
	ASSERT_NO_FATAL_FAILURE((hand_shapes_at<2, pab_tree::repair_delete>()));
	ASSERT_NO_FATAL_FAILURE((hand_shapes_at<4, pab_tree::repair_delete>()));
	ASSERT_NO_FATAL_FAILURE((hand_shapes_at<8, pab_tree::repair_delete>()));
}

/*
 * The tomb overflows: as the root, the root splits and the height grows on
 * a delete; below it, its parent takes the halves; under a full root, that
 * parent splits in turn.
 */
TEST(SelfTest, GraftTombOverflow)
{
	using tree = hand_tree<4, graft_policy>;
	auto const below =
		node_of({ thinnest(4, 4), tomb_overflow(), thinnest(4, 4) });
	auto const cascade =
		node_of(cat(times(15, thinnest(4, 4)), { tomb_overflow() }));

	ASSERT_NO_FATAL_FAILURE(
		(hand_shape<tree>(tomb_overflow(), "tomb overflow", 5)));
	ASSERT_NO_FATAL_FAILURE(
		(hand_shape<tree>(below, "tomb overflow below", 5)));
	ASSERT_NO_FATAL_FAILURE(
		(hand_shape<tree>(cascade, "tomb overflow cascade", 6)));

	using repair_tree = hand_tree<4, pab_tree::repair_delete>;
	ASSERT_NO_FATAL_FAILURE(
		(hand_shape<repair_tree>(tomb_overflow(), "tomb overflow")));
	ASSERT_NO_FATAL_FAILURE(
		(hand_shape<repair_tree>(cascade, "tomb overflow cascade")));
}

/* Every node the graft path cuts out, frees or regroups, exactly once. */
TEST(SelfTest, GraftLeakCheck)
{
	using tree = counted_graft_tree<8>;
	auto const seed = g_seed ^ 0x1eac2eULL;

	for (shape s : k_shapes) {
		ASSERT_NO_FATAL_FAILURE(repair_scenario<tree>(200000, s, seed));
	}
	ASSERT_NO_FATAL_FAILURE(ten_percent_chain<tree>(100000, seed));
	EXPECT_EQ(typed_counting_alloc::allocs<tree::leaf_type>.load(),
			  typed_counting_alloc::frees<tree::leaf_type>.load())
		<< "leaf leak or double free";
	EXPECT_EQ(typed_counting_alloc::allocs<tree::interior_type>.load(),
			  typed_counting_alloc::frees<tree::interior_type>.load())
		<< "interior leak or double free";
}

/* The graft tests on plain new/delete, for the sanitizer build. */
TEST(SelfTest, GraftHeapAllocator)
{
	struct heap_opts : pab_tree::default_options {
		using node_alloc = heap_alloc;
		using delete_policy = graft_policy;
	};
	using tree = pab_tree::btree<pab_tree::btree_trait<ele, 8, heap_opts>>;
	using tree4 = pab_tree::btree<pab_tree::btree_trait<ele, 4, heap_opts>>;
	auto const seed = g_seed ^ 0x4ea92eULL;

	for (shape s : k_shapes) {
		ASSERT_NO_FATAL_FAILURE(repair_scenario<tree>(200000, s, seed));
	}
	ASSERT_NO_FATAL_FAILURE(ten_percent_chain<tree>(100000, seed));
	ASSERT_NO_FATAL_FAILURE(hand_shape<tree>(hand_shapes<8>::order(), "order"));
	ASSERT_NO_FATAL_FAILURE(hand_shape<tree>(hand_shapes<8>::edge(), "edge"));
	ASSERT_NO_FATAL_FAILURE(hand_shape<tree>(hand_shapes<8>::inner(), "inner"));
	ASSERT_NO_FATAL_FAILURE(
		hand_shape<tree>(hand_shapes<8>::raised(), "raised"));
	ASSERT_NO_FATAL_FAILURE(
		hand_shape<tree4>(tomb_overflow(), "tomb overflow", 5));
}

/*
 * HOW TO RUN
 *   ./self_test                         every test, fixed seed
 *   ./self_test --seed=12345            reproduce a reported failure
 *   ./self_test --rounds=20             more shapes per size, for fuzzing
 *   ./self_test --gtest_filter=SelfTest.Churn
 *
 * Needs no data files. Every failure prints the seed and the flag to
 * reproduce it.
 */
int main(int argc, char **argv)
{
	::testing::InitGoogleTest(&argc, argv);

	/*
	 * Child mode for SelfTest.DefaultHybridPredIsReproducible: print the
	 * pattern and exit, so the parent can compare two fresh processes.
	 */
	if (std::getenv("PABTREE_HYBRID_CHILD") != nullptr) {
		print_hybrid_pattern();
		return 0;
	}

	for (int i = 1; i < argc; ++i) {
		std::string const arg = argv[i];
		if (arg.starts_with("--seed=")) {
			g_seed = std::stoull(arg.substr(7));
		} else if (arg.starts_with("--rounds=")) {
			g_rounds = std::stoi(arg.substr(9));
		}
	}

	std::printf("self_test: seed=%llu rounds=%d\n",
				static_cast<unsigned long long>(g_seed), g_rounds);
	std::printf("reproduce any failure with: --seed=%llu\n",
				static_cast<unsigned long long>(g_seed));

	return RUN_ALL_TESTS();
}

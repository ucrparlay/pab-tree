// Dataset-free correctness tests. Generates its own keys from a seed and
// checks every operation against std::map, so it runs anywhere.
//
// tests/unit_test.cpp is the other half: same tree, real benchmark data, and
// it skips when the data is absent.

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
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include <parlay/primitives.h>
#include <parlay/sequence.h>

#include "pab_tree/b_tree.h"

namespace
{

// ---- the element the tree stores ----

struct entry {
	uint32_t key;
	uint32_t val;
};

struct ele {
	using E = entry;
	using K = uint32_t;
	using V = uint32_t;

	static K get_key(E const &e)
	{
		return e.key;
	}

	static V get_val(E const &e)
	{
		return e.val;
	}

	static E to_entry(K const &k, V const &v)
	{
		return E{ k, v };
	}
};

// ---- augmentations, checked against the tree they annotate ----

// Sum of the keys in one leaf.
struct sum_leaf_aug {
	uint64_t sum = 0;

	sum_leaf_aug() = default;

	template <typename Slice> explicit sum_leaf_aug(Slice in)
	{
		update_aug(in);
	}

	template <typename Slice> void update_aug(Slice in)
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

// Entries below one interior node. Reads only proto_node::size, so it needs
// no access to the children's own augmentation.
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

// ---- workloads ----

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

// n distinct keys. Distinct is a precondition of the tree, so every shape
// here dedupes before it returns.
std::vector<uint32_t> gen_keys(size_t n, dist d, uint64_t seed)
{
	std::mt19937_64 rng(seed);
	std::vector<uint32_t> keys;
	keys.reserve(n);

	switch (d) {
	case dist::uniform: {
		// Draw from a space wide enough that dedup rarely bites.
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
		// A few hot regions, so leaves fill unevenly.
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
		// Runs of consecutive keys separated by gaps, so batches land
		// exactly on split boundaries.
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
		// A value derived from the key, so a misplaced entry is visible.
		v.push_back(entry{ k, k ^ 0x5bf03635u });
	}
	return v;
}

// ---- checks against the oracle ----

using oracle = std::map<uint32_t, uint32_t>;

void add_to_oracle(oracle &ref, std::vector<entry> const &v)
{
	for (auto const &e : v) {
		ref[e.key] = e.val;
	}
}

// Every leaf's augmentation equals the sum of the keys it holds, and every
// interior's equals the entries below it. Catches an update_aug that a
// rebalance forgot.
template <typename Tree, typename Node>
void check_aug(Node const *T, size_t &leaves, size_t &interiors)
{
	using Leaf = typename Tree::Leaf;
	using Interior = typename Tree::Interior;

	if (T->is_leaf) {
		auto const *TL = static_cast<Leaf const *>(T);
		uint64_t want = 0;
		for (size_t i = 0; i < TL->size; ++i) {
			want += TL->elements[i].key;
		}
		ASSERT_EQ(TL->get_aug().sum, want) << "leaf aug is stale";
		leaves++;
		return;
	}

	auto const *TI = static_cast<Interior const *>(T);
	uint64_t want = 0;
	for (size_t i = 0; i < TI->get_node_n(); ++i) {
		want += TI->tree_nodes[i]->size;
	}
	ASSERT_EQ(TI->get_aug().total, want) << "interior aug is stale";
	ASSERT_EQ(TI->get_aug().total, TI->size) << "interior aug != subtree size";
	interiors++;

	for (size_t i = 0; i < TI->get_node_n(); ++i) {
		ASSERT_NO_FATAL_FAILURE(
			check_aug<Tree>(TI->tree_nodes[i], leaves, interiors));
	}
}

// Contents, ordering, membership and the range queries, all against ref.
template <typename Tree> void check_against(Tree &t, oracle const &ref)
{
	bool const structure_ok = t.template validate<true, false>();
	ASSERT_TRUE(structure_ok) << "validate() failed";
	ASSERT_EQ(t.size(), ref.size());
	ASSERT_EQ(t.empty(), ref.empty());

	// Forward iteration, then backwards from end().
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

	// get_entries returns everything, in no particular order.
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

	// to_seq is the same contents in key order.
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

// Membership and ordered lookup over present and absent keys.
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

// range_count and range_report over boxes drawn around live keys.
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
			// The whole key space.
			lo = 0;
			hi = 0xFFFFFFFFu;
		} else if (b == 1) {
			// Empty box.
			lo = keys[rng() % keys.size()];
			hi = lo;
		} else {
			lo = keys[rng() % keys.size()];
			hi = keys[rng() % keys.size()];
			if (lo > hi) {
				std::swap(lo, hi);
			}
			// Half-open, so nudge sometimes to cover both edges.
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

// One full scenario: build, query, insert, delete, re-query.
template <unsigned KBuf>
void run_scenario(size_t n, dist d, uint64_t seed, bool with_ranges = true)
{
	using tt = pab_tree::btree_type_trait<ele, KBuf>;
	using tree = pab_tree::btree<tt>;

	SCOPED_TRACE("k_buf=" + std::to_string(KBuf) + " n=" + std::to_string(n) +
				 " dist=" + dist_name(d) + " seed=" + std::to_string(seed) +
				 "  (rerun: ./self_test --seed=" + std::to_string(seed) + ")");

	auto const all = gen_keys(n, d, seed);
	// Two thirds build the tree, one third is the insert batch.
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

	// Probe live keys, the insert batch (absent so far), and neighbours.
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

	// Insert the rest.
	t.batch_insert(extra_e);
	add_to_oracle(ref, extra_e);
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
	ASSERT_NO_FATAL_FAILURE(check_lookups(t, ref, probes));
	if (with_ranges) {
		ASSERT_NO_FATAL_FAILURE(check_ranges(t, ref, seed ^ 2, 12));
	}

	// Delete half of what is in there, chosen across the whole key range.
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

	// Delete everything that is left.
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

} // namespace

// ---- the element adaptor is used directly, with either signature ----

// By-value accessors. The library used to wrap every adaptor in a layer
// that forced this signature; it now calls the adaptor as written.
struct ele_by_value {
	using E = entry;
	using K = uint32_t;
	using V = uint32_t;

	static K get_key(E e)
	{
		return e.key;
	}
	static V get_val(E e)
	{
		return e.val;
	}
	static E to_entry(K k, V v)
	{
		return E{ k, v };
	}
};

// A fat entry, where a by-value copy would be 64 bytes if one happened.
struct big_entry {
	uint32_t key;
	char payload[60];
};

struct ele_big {
	using E = big_entry;
	using K = uint32_t;
	using V = uint32_t;

	static K get_key(E const &e)
	{
		return e.key;
	}
	static V get_val(E const &e)
	{
		return e.key;
	}
	static E to_entry(K const &k, V const &)
	{
		E e{};
		e.key = k;
		return e;
	}
};

static_assert(pab_tree::entry_type<ele>);
static_assert(pab_tree::entry_type<ele_by_value>);
static_assert(pab_tree::entry_type<ele_big>);

TEST(SelfTest, ElementAdaptorSignatures)
{
	auto const keys = gen_keys(20000, dist::uniform, g_seed ^ 0xe1e);

	// by-value accessors
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

	// a 64-byte entry, by-reference accessors
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

// ---- get_entries' output-size contract ----

TEST(SelfTest, GetEntriesOutputSize)
{
	using tt = pab_tree::btree_type_trait<ele, 32>;
	using tree = pab_tree::btree<tt>;

	auto const all = to_entries(gen_keys(5000, dist::uniform, g_seed ^ 0x9e7));
	tree t;
	t.build(all);
	ASSERT_EQ(t.size(), all.size());

	// Exactly sized: every entry lands, none past the end.
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

	// Oversized is fine; the tail is left alone.
	{
		parlay::sequence<entry> out(t.size() + 100);
		for (size_t i = t.size(); i < out.size(); ++i)
			out[i] = entry{ 0xDEADBEEFu, 0xDEADBEEFu };
		t.get_entries(out);
		for (size_t i = t.size(); i < out.size(); ++i)
			ASSERT_EQ(out[i].key, 0xDEADBEEFu)
				<< "get_entries wrote past size()";
	}

	// An empty tree writes nothing and does not dereference a null root.
	{
		tree e;
		parlay::sequence<entry> out(4);
		out[0] = entry{ 7u, 7u };
		e.get_entries(out);
		EXPECT_EQ(out[0].key, 7u);
	}

	// One short must be caught rather than overrun. Asserts are live in
	// this build (-UNDEBUG); in a release build the call is undefined,
	// which is what the README says.
#ifndef NDEBUG
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	parlay::sequence<entry> small(t.size() - 1);
	EXPECT_DEATH(
		{ t.get_entries(small); }, "Out.size\\(\\) >=")
		<< "an undersized output was not caught";
#endif
}

// ---- deleting a key that is not there does nothing ----

TEST(SelfTest, DeleteAbsentKeys)
{
	using tt = pab_tree::btree_type_trait<ele, 16>;
	using tree = pab_tree::btree<tt>;

	auto const seed = g_seed ^ 0xab5e27;
	SCOPED_TRACE("absent-key delete seed=" + std::to_string(seed));

	// Even keys only, so every odd key is absent.
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
			ref.erase(k); // std::map::erase ignores absent keys too
		ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
	};

	// 1. every key absent, and more of them than the tree holds
	{
		std::vector<uint32_t> odd;
		for (uint32_t i = 1; i < 120000; i += 2)
			odd.push_back(i);
		auto const before = t.size();
		ASSERT_NO_FATAL_FAILURE(erase_both(odd, "all absent, batch > tree"));
		ASSERT_EQ(t.size(), before) << "an absent-key delete changed the size";
	}

	// 2. absent keys beyond both ends of the key range
	{
		std::vector<uint32_t> outside{ 0xFFFFFFFEu, 0xFFFFFF00u, 1u, 3u };
		auto const before = t.size();
		ASSERT_NO_FATAL_FAILURE(erase_both(outside, "outside the range"));
		ASSERT_EQ(t.size(), before);
	}

	// 3. a mix: half present, half not
	{
		std::mt19937_64 rng(seed);
		std::vector<uint32_t> mixed;
		for (int i = 0; i < 4000; ++i) {
			uint32_t const k = static_cast<uint32_t>(rng() % 40000);
			mixed.push_back(k); // even => present, odd => absent
		}
		ASSERT_NO_FATAL_FAILURE(erase_both(mixed, "half present"));
	}

	// 4. deleting the same present key twice: second time is a no-op
	{
		auto const k = t.begin()->key;
		ASSERT_TRUE(t.contain(k));
		ASSERT_NO_FATAL_FAILURE(erase_both({ k }, "present key, first time"));
		ASSERT_FALSE(t.contain(k));
		auto const before = t.size();
		ASSERT_NO_FATAL_FAILURE(erase_both({ k }, "same key, second time"));
		ASSERT_EQ(t.size(), before);
	}

	// 5. single-key erase(k) reports whether it was there
	{
		auto const present = t.begin()->key;
		ASSERT_TRUE(t.erase(present));
		ASSERT_FALSE(t.erase(present));
		ASSERT_FALSE(t.erase(1u)); // odd, never present
		ref.erase(present);
		ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
	}

	// 6. emptying the tree with a batch that also names absent keys
	{
		std::vector<uint32_t> everything;
		for (auto const &kv : ref)
			everything.push_back(kv.first);
		for (uint32_t i = 1; i < 2000; i += 2)
			everything.push_back(i); // absent
		ASSERT_NO_FATAL_FAILURE(erase_both(everything, "empty it out"));
		ASSERT_TRUE(t.empty());
		ASSERT_EQ(t.begin(), t.end());
	}
}

// ---- the default leaf-sort decision is reproducible ----

// FNV over every leaf's is_sorted flag, in tree order.
template <typename Tree, typename Node>
void hash_leaf_sortedness(Node const *T, uint64_t &h, size_t &leaves)
{
	using Leaf = typename Tree::Leaf;
	using Interior = typename Tree::Interior;

	if (T->is_leaf) {
		auto const *TL = static_cast<Leaf const *>(T);
		h = (h ^ (TL->is_sorted ? 1u : 0u)) * 1099511628211ULL;
		leaves++;
		return;
	}
	auto const *TI = static_cast<Interior const *>(T);
	for (size_t i = 0; i < TI->get_node_n(); ++i)
		hash_leaf_sortedness<Tree>(TI->tree_nodes[i], h, leaves);
}

// Runs the default insert/delete path and prints the leaf-sortedness
// pattern. Called in the child process; see the test below.
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
	t.batch_insert(rest); // default path: hybrid + default_hybrid_pred
	t.batch_delete(del);

	uint64_t h = 1469598103934665603ULL;
	size_t leaves = 0;
	hash_leaf_sortedness<tree>(t.get_root(), h, leaves);
	std::printf("PATTERN %016llx %zu\n", (unsigned long long)h, leaves);
	std::fflush(stdout);
}

// default_hybrid_pred mixes bit 3 of each end of the input slice into its
// coin flip. That bit comes from the slice's offset inside its buffer, not
// from the address -- allocations are 16-byte aligned, so bit 3 of the base
// is 0 -- so the decision is the same from run to run.
//
// This has to be checked across processes. Inside one process the slices
// the predicate sees are internal buffers that the pool hands back at the
// same address every time, so two runs prove nothing; only a fresh process
// gets a different ASLR base. Verified: with the shift changed from 3 to 12
// the in-process version still passed, this one fails.
TEST(SelfTest, DefaultHybridPredIsReproducible)
{
	// Resolve our own path here: inside popen, /proc/self/exe is the
	// shell, not this binary.
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

// ---- a custom node base ----

// Same shape proto_node has, plus a field of its own. node_base is what
// the tree requires: constructible from (bool, size_t), with public
// is_leaf and a writable size.
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

// An aggregate qualifies too: C++20 parenthesized aggregate init (P0960)
// makes this constructible from (bool, size_t) without a constructor.
struct aggregate_node {
	bool is_leaf = false;
	size_t size = 0;
};

static_assert(pab_tree::node_base<pab_tree::proto_node>);
static_assert(pab_tree::node_base<tagged_node>);
static_assert(pab_tree::node_base<aggregate_node>);

// And the ones the tree could not work with are rejected.
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
	using tt =
		pab_tree::btree_type_trait<ele, 32, pab_tree::empty_leaf_aug,
								   pab_tree::empty_interior_aug, tagged_node>;
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

	// The custom field survives on every node the tree built.
	ASSERT_NE(t.get_root(), nullptr);
	ASSERT_EQ(t.get_root()->tag, 0xABu) << "custom node field lost";

	std::vector<uint32_t> half(keys.begin(), keys.begin() + keys.size() / 2);
	std::sort(half.begin(), half.end());
	t.batch_delete(half);
	for (auto k : half)
		ref.erase(k);
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
}

// ---- a custom node allocator is used for every node ----

// Counts allocations and frees. Also a leak check: the tree must return
// every node it took by the time it is destroyed.
struct counting_alloc {
	static inline std::atomic<long> live{ 0 };
	static inline std::atomic<long> total{ 0 };

	template <typename T> static T *alloc()
	{
		live.fetch_add(1, std::memory_order_relaxed);
		total.fetch_add(1, std::memory_order_relaxed);
		return parlay::type_allocator<T>::alloc();
	}

	template <typename T> static void free(T *p)
	{
		live.fetch_sub(1, std::memory_order_relaxed);
		parlay::type_allocator<T>::retire(p);
	}
};

TEST(SelfTest, CustomAllocator)
{
	using tt = pab_tree::btree_type_trait<
		ele, 16, pab_tree::empty_leaf_aug, pab_tree::empty_interior_aug,
		pab_tree::proto_node, std::less<uint32_t>, counting_alloc>;
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

		// A tree of n entries with leaf size 16 holds at least n/k_buf_ub
		// leaves, so the count has to be in a sane range.
		ASSERT_GE(counting_alloc::live.load(),
				  static_cast<long>(keys.size() / (4 * 16)));

		std::vector<uint32_t> half(keys.begin(),
								   keys.begin() + keys.size() / 2);
		std::sort(half.begin(), half.end());
		t.batch_delete(half);
		ok = t.template validate<true, false>();
		ASSERT_TRUE(ok);
	}

	// The tree is gone; every node it took must have come back.
	EXPECT_EQ(counting_alloc::live.load(), 0)
		<< "leaked " << counting_alloc::live.load() << " nodes of "
		<< counting_alloc::total.load();
}

// ---- a custom key comparator orders the whole tree ----

TEST(SelfTest, ReverseComparator)
{
	// std::greater: the tree is ordered descending, so every structural
	// invariant, the descent, the sorts and validate() must follow it.
	using tt = pab_tree::btree_type_trait<
		ele, 16, pab_tree::empty_leaf_aug, pab_tree::empty_interior_aug,
		pab_tree::proto_node, std::greater<uint32_t>>;
	using tree = pab_tree::btree<tt>;

	auto const seed = g_seed ^ 0x9e77ULL;
	SCOPED_TRACE("reverse comparator seed=" + std::to_string(seed));

	auto const keys = gen_keys(20000, dist::uniform, seed);
	auto const all = to_entries(keys);
	std::vector<entry> base(all.begin(), all.begin() + all.size() / 2);
	std::vector<entry> rest(all.begin() + all.size() / 2, all.end());

	// The oracle uses the same order.
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

		// Iteration follows the comparator, so it runs high to low.
		auto rt = ref.begin();
		size_t cnt = 0;
		for (auto it = t.begin(); it != t.end(); ++it, ++rt, ++cnt) {
			ASSERT_NE(rt, ref.end());
			ASSERT_EQ(it->key, rt->first) << "order diverged at " << cnt;
		}
		ASSERT_EQ(cnt, ref.size());

		// to_seq is descending.
		auto const seq = t.to_seq();
		ASSERT_EQ(seq.size(), ref.size());
		for (size_t i = 1; i < seq.size(); ++i)
			ASSERT_GT(ele::get_key(seq[i - 1]), ele::get_key(seq[i]))
				<< "to_seq not descending at " << i;

		// Membership and the ordered lookups.
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

	// range_count takes its box in comparator order: [hi, lo) descending.
	{
		std::vector<uint32_t> sorted_keys = keys;
		std::sort(sorted_keys.begin(), sorted_keys.end());
		std::mt19937_64 rng(seed);
		for (int b = 0; b < 20; ++b) {
			auto lo = sorted_keys[rng() % sorted_keys.size()];
			auto hi = sorted_keys[rng() % sorted_keys.size()];
			if (lo < hi)
				std::swap(lo, hi); // first is the greater under std::greater
			auto const want = static_cast<size_t>(
				std::distance(ref.lower_bound(lo), ref.lower_bound(hi)));
			ASSERT_EQ(t.range_count({ lo, hi }), want)
				<< "range_count [" << lo << ", " << hi << ")";
		}
	}

	// Deleting still rebalances correctly under the comparator.
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

// ---- edge cases: the sizes where the structure changes shape ----

template <unsigned KBuf> void small_sizes()
{
	for (size_t n :
		 { 0u,	 1u,   2u,	 3u,   4u,	 5u,   7u,	 8u,	9u,
		   15u,	 16u,  17u,	 31u,  32u,	 33u,  63u,	 64u,	65u,
		   127u, 128u, 129u, 255u, 256u, 257u, 511u, 1023u, 1025u }) {
		run_scenario<KBuf>(n, dist::sequential, g_seed + n);
		run_scenario<KBuf>(n, dist::uniform, g_seed + n + 1);
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

// ---- the shapes, at a size that builds a real multi-level tree ----

template <unsigned KBuf> void shapes(size_t n)
{
	for (int r = 0; r < g_rounds; ++r) {
		auto const s = g_seed + static_cast<uint64_t>(r) * 7919;
		run_scenario<KBuf>(n, dist::uniform, s);
		run_scenario<KBuf>(n, dist::clustered, s + 1);
		run_scenario<KBuf>(n, dist::sequential, s + 2);
		run_scenario<KBuf>(n, dist::sparse, s + 3);
		run_scenario<KBuf>(n, dist::boundary, s + 4);
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

// ---- many small batches against one tree ----

TEST(SelfTest, Churn)
{
	using tt = pab_tree::btree_type_trait<ele, 16>;
	using tree = pab_tree::btree<tt>;

	auto const seed = g_seed ^ 0xc0ffeeULL;
	SCOPED_TRACE("churn seed=" + std::to_string(seed) +
				 "  (rerun: ./self_test --seed=" + std::to_string(g_seed) +
				 " --gtest_filter=SelfTest.Churn)");

	std::mt19937_64 rng(seed);
	tree t;
	oracle ref;

	auto const pool = gen_keys(60000, dist::uniform, seed);

	for (int round = 0; round < 40; ++round) {
		SCOPED_TRACE("round " + std::to_string(round));

		// Insert a random slice of the pool that is not in the tree.
		std::vector<entry> add;
		for (size_t i = 0; i < 800; ++i) {
			auto const k = pool[rng() % pool.size()];
			if (ref.count(k) == 0) {
				add.push_back(entry{ k, k ^ 0x5bf03635u });
				ref[k] = k ^ 0x5bf03635u;
			}
		}
		// A batch must itself hold distinct keys.
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

		// Delete a random slice of what is live.
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

// ---- augmentation stays current through every path ----

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

	// Delete enough to force merges and redistribution.
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

// ---- copy, move and assignment ----

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

	// Copy is deep: mutating the copy leaves the original alone.
	tree b = a;
	ASSERT_NO_FATAL_FAILURE(check_against(b, ref));
	ASSERT_TRUE(a == b);

	// Drop the 1000 smallest keys from the copy.
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

	// Ordering is lexicographic over the contents in key order, as in
	// std::set. b lost its smallest keys, so it now starts higher.
	ASSERT_TRUE(a < b);
	ASSERT_TRUE(b > a);
	ASSERT_TRUE(a <= b);
	ASSERT_FALSE(b <= a);

	// A prefix is less than what it is a prefix of.
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

	// Move leaves the source empty and usable.
	tree c = std::move(a);
	ASSERT_NO_FATAL_FAILURE(check_against(c, ref));
	ASSERT_TRUE(a.empty());

	a = c;
	ASSERT_TRUE(a == c);

	// Self-assignment must not free the tree.
	tree *p = &c;
	c = *p;
	ASSERT_NO_FATAL_FAILURE(check_against(c, ref));

	// swap and clear.
	tree d;
	d.swap(c);
	ASSERT_TRUE(c.empty());
	ASSERT_NO_FATAL_FAILURE(check_against(d, ref));
	d.clear();
	ASSERT_TRUE(d.empty());
	ASSERT_EQ(d.begin(), d.end());
}

// ---- single-element and iterator modifiers ----

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

	// erase(iterator) and erase(first, last).
	t.erase(t.begin());
	ref.erase(ref.begin());
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));

	auto const lo_key = std::next(ref.begin(), 100)->first;
	auto const hi_key = std::next(ref.begin(), 600)->first;
	t.erase(t.lower_bound(lo_key), t.lower_bound(hi_key));
	ref.erase(ref.lower_bound(lo_key), ref.lower_bound(hi_key));
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
}

// ---- an empty and a default-constructed tree answer every query ----

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

	// Emptied by deletion, not merely never filled.
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

	// And usable again afterwards.
	t.batch_insert(e);
	add_to_oracle(ref, e);
	ASSERT_NO_FATAL_FAILURE(check_against(t, ref));
}

// HOW TO RUN
//   ./self_test                         every test, fixed seed
//   ./self_test --seed=12345            reproduce a reported failure
//   ./self_test --rounds=20             more shapes per size, for fuzzing
//   ./self_test --gtest_filter=SelfTest.Churn
//
// Needs no data files. Every failure prints the seed and the flag to
// reproduce it.
int main(int argc, char **argv)
{
	::testing::InitGoogleTest(&argc, argv);

	// Child mode for SelfTest.DefaultHybridPredIsReproducible: print the
	// pattern and exit, so the parent can compare two fresh processes.
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

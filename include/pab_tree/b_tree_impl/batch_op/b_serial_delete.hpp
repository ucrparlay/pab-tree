#ifndef PABTREE_B_TREE_IMPL_BATCH_OP_B_SERIAL_DELETE_HPP_
#define PABTREE_B_TREE_IMPL_BATCH_OP_B_SERIAL_DELETE_HPP_

#include <cassert>

#include <algorithm>
#include <iterator>
#include <memory>
#include <numeric>
#include <vector>

#include "pab_tree/b_tree.h"
#include "parlay/slice.h"

namespace pab_tree
{
template <typename TT>
template <typename R>
void btree<TT>::serial_delete(R &&in)
{
	static_assert(parlay::is_random_access_range_v<R>);
	static_assert(std::is_constructible_v<parlay::range_value_type_t<R>,
										  parlay::range_reference_type_t<R>>);

	auto in_slice = parlay::make_slice(std::forward<R>(in));

	constexpr auto leaf_entries_pattern = TT::leaf_entries_pattern::hybrid;
	serial_delete_(in_slice, leaf_entries_pattern, default_hybrid_pred{});
}

template <typename TT>
template <typename R, typename LeafSortPred>
void btree<TT>::serial_delete(R &&in,
							  const TT::pattern_type leaf_entries_pattern,
							  [[maybe_unused]] const bool sorted_input,
							  LeafSortPred const &leaf_sort_pred)
{
	static_assert(parlay::is_random_access_range_v<R>);
	static_assert(std::is_constructible_v<parlay::range_value_type_t<R>,
										  parlay::range_reference_type_t<R>>);

	auto in_slice = parlay::make_slice(std::forward<R>(in));

	serial_delete_(in_slice, leaf_entries_pattern, leaf_sort_pred);
}

template <typename TT>
template <typename R, typename LeafSortPred>
	requires std::predicate<LeafSortPred, typename btree<TT>::node_type *, R>
void btree<TT>::serial_delete_(R in,
							   const TT::pattern_type leaf_entries_pattern,
							   LeafSortPred const &leaf_sort_pred)
{
	static_assert(
		std::same_as<typename decltype(in)::value_type, typename TT::key_type>);
	/* An underfull interior must keep a child to merge. */
	static_assert(TT::k_buf_lb >= 2, "serial_delete needs k_buf_lb >= 2");

	if (this->root_ == nullptr) {
		return;
	}

	/*
	 * The leaf predicate gets what batch_delete_ hands it, a slice of keys
	 * in memory. Any other range is copied first.
	 */
	std::vector<key_type> copy;
	typename TT::const_key_slice keys{ nullptr, nullptr };
	if constexpr (std::contiguous_iterator<decltype(std::begin(in))>) {
		keys = { std::to_address(std::begin(in)),
				 std::to_address(std::end(in)) };
	} else {
		copy.assign(std::begin(in), std::end(in));
		keys = { copy.data(), copy.data() + copy.size() };
	}

	for (size_t i = 0; i < keys.size(); i++) {
		if (!serial_delete_rec(this->root_, keys.cut(i, i + 1),
							   leaf_entries_pattern, leaf_sort_pred)) {
			continue;
		}

		/* A root left with one child: the child is the root. */
		while (!this->root_->is_leaf &&
			   static_cast<interior_type *>(this->root_)->get_node_n() == 1) {
			auto *old = static_cast<interior_type *>(this->root_);
			this->root_ = old->tree_nodes[0];
			free_node<interior_type>(old);
		}
	}
}

template <typename TT>
template <typename LeafSortPred>
bool btree<TT>::serial_delete_rec(node_type *node, TT::const_key_slice in,
								  const TT::pattern_type leaf_entries_pattern,
								  LeafSortPred const &leaf_sort_pred)
{
	assert(in.size() == 1);

	if (node->is_leaf) {
		return serial_delete_leaf(node, in, leaf_entries_pattern,
								  leaf_sort_pred);
	}

	auto *interior = static_cast<interior_type *>(node);
	assert(interior->get_node_n() > 0);
	buf_size_type const i = interior->get_node_id_containing_key(in[0]);
	node_type *const child = interior->tree_nodes[i];
	if (!serial_delete_rec(child, in, leaf_entries_pattern, leaf_sort_pred)) {
		return false;
	}
	interior->size -= 1;

	/*
	 * Below the root every node holds at least k_buf_lb. A root with one
	 * child has no sibling to use; the caller collapses it.
	 */
	bool const underfull =
		child->is_leaf ?
			child->size < TT::k_buf_lb :
			static_cast<interior_type *>(child)->get_node_n() < TT::k_buf_lb;
	if (underfull && interior->get_node_n() >= 2) {
		serial_rebalance(interior, i);
	}
	interior->update_aug();
	return true;
}

template <typename TT>
template <typename LeafSortPred>
bool btree<TT>::serial_delete_leaf(node_type *node, TT::const_key_slice in,
								   const TT::pattern_type leaf_entries_pattern,
								   LeafSortPred const &leaf_sort_pred)
{
	auto *leaf = static_cast<leaf_type *>(node);
	key_type const k = in[0];
	auto const first = leaf->get_ele_begin_iter();
	auto const last = leaf->get_ele_end_iter();
	auto const pos = leaf->is_sorted ?
						 first + leaf->binary_search_key(k) :
						 std::find_if(first, last, [&](entry_type const &e) {
							 return TT::key_equal(entry_traits::get_key(e), k);
						 });
	if (pos == last || !TT::key_equal(entry_traits::get_key(*pos), k)) {
		return false;
	}

	/*
	 * A sorted leaf the pattern may keep sorted closes the gap. Otherwise
	 * the last entry fills it, as in delete_points_4_leaf.
	 */
	bool const was_sorted = leaf->is_sorted;
	if (was_sorted &&
		leaf_entries_pattern != TT::leaf_entries_pattern::unsorted) {
		std::move(pos + 1, last, pos);
	} else {
		*pos = *(last - 1);
	}
	leaf->size -= 1;
	leaf->update_aug(parlay::make_slice(leaf->elements.begin(),
										leaf->elements.begin() + leaf->size));

	/* Then sorted or not as post_process_leaf_change decides. */
	bool keep_sorted = false;
	if (leaf_entries_pattern == TT::leaf_entries_pattern::sorted) {
		keep_sorted = true;
	} else if (leaf_entries_pattern == TT::leaf_entries_pattern::hybrid) {
		keep_sorted = leaf_sort_pred(node, in);
	} else {
		assert(leaf_entries_pattern == TT::leaf_entries_pattern::unsorted);
	}

	if (!keep_sorted) {
		leaf->is_sorted = false;
	} else if (!was_sorted) {
		ensure_leaf_sorted(leaf);
	}
	return true;
}

template <typename TT>
void btree<TT>::serial_rebalance(interior_type *node, buf_size_type i)
{
	buf_size_type const n = node->get_node_n();
	assert(n >= 2 && i < n);
	auto &nodes = node->tree_nodes;
	auto &keys = node->split;

	/* Pair child i with its larger sibling, the left one on a tie. */
	auto const width = [](node_type const *c) -> size_type {
		return c->is_leaf ? c->size :
							static_cast<interior_type const *>(c)->get_node_n();
	};
	buf_size_type const j =
		i + 1 == n || (i > 0 && width(nodes[i - 1]) >= width(nodes[i + 1])) ?
			i - 1 :
			i;

	node_type *const l = nodes[j];
	node_type *const r = nodes[j + 1];
	bool const merged =
		l->is_leaf ?
			serial_balance_leaves(static_cast<leaf_type *>(l),
								  static_cast<leaf_type *>(r), keys[j]) :
			serial_balance_interiors(static_cast<interior_type *>(l),
									 static_cast<interior_type *>(r), keys[j]);
	if (!merged) {
		return;
	}

	/* r went into l: r and the separator before it leave node. */
	if (r->is_leaf) {
		free_node<leaf_type>(r);
	} else {
		free_node<interior_type>(r);
	}
	std::move(nodes.begin() + j + 2, nodes.begin() + n, nodes.begin() + j + 1);
	std::move(keys.begin() + j + 1, keys.begin() + n - 1, keys.begin() + j);
	node->node_n = n - 1;
}

template <typename TT>
bool btree<TT>::serial_balance_leaves(leaf_type *l, leaf_type *r, key_type &sep)
{
	size_type const n = l->size + r->size;
	if (n < TT::k_buf_md) {
		std::copy(r->get_ele_begin_iter(), r->get_ele_end_iter(),
				  l->get_ele_end_iter());
		l->size = n;
		l->is_sorted = l->is_sorted && r->is_sorted;
		l->update_aug(parlay::make_slice(l->elements.begin(),
										 l->elements.begin() + l->size));
		return true;
	}

	/*
	 * Cut where generate_leaf_nodes cuts: l keeps the first n / 2. The
	 * giver is sorted first, so the entries that move are at its end.
	 */
	size_type const lo = n / 2;
	auto const le = l->get_ele_begin_iter();
	auto const re = r->get_ele_begin_iter();
	if (l->size > lo) {
		/* l's last entries go to the front of r. */
		ensure_leaf_sorted(l);
		size_type const k = l->size - lo;
		std::move_backward(re, re + r->size, re + r->size + k);
		std::copy(le + lo, le + l->size, re);
	} else {
		/* r's first entries go to the end of l. */
		assert(l->size < lo);
		ensure_leaf_sorted(r);
		size_type const k = lo - l->size;
		std::copy(re, re + k, le + l->size);
		std::move(re + k, re + r->size, re);
	}
	l->size = lo;
	r->size = n - lo;
	l->update_aug(
		parlay::make_slice(l->elements.begin(), l->elements.begin() + l->size));
	r->update_aug(
		parlay::make_slice(r->elements.begin(), r->elements.begin() + r->size));
	/* r's smallest key: what moved into r came sorted, ahead of the rest. */
	sep = entry_traits::get_key(r->elements[0]);
	return false;
}

template <typename TT>
bool btree<TT>::serial_balance_interiors(interior_type *l, interior_type *r,
										 key_type &sep)
{
	buf_size_type const ln = l->get_node_n();
	buf_size_type const rn = r->get_node_n();
	size_type const n = ln + rn;
	assert(ln > 0 && rn > 0);
	auto const lc = l->tree_nodes.begin();
	auto const lk = l->split.begin();
	auto const rc = r->tree_nodes.begin();
	auto const rk = r->split.begin();

	/* sep comes down between l's children and r's. */
	if (n < TT::k_buf_md) {
		std::copy(rc, rc + rn, lc + ln);
		lk[ln - 1] = sep;
		std::copy(rk, rk + rn - 1, lk + ln);
		l->node_n = n;
		l->size += r->size;
		l->update_aug();
		return true;
	}

	/*
	 * Cut where generate_interior_nodes cuts: l keeps the first n / 2
	 * children, and the separator at the cut goes up into sep.
	 */
	auto const entries = [](auto first, auto last) {
		return std::accumulate(
			first, last, size_type{ 0 },
			[](size_type acc, node_type const *c) { return acc + c->size; });
	};
	size_type const lo = n / 2;
	if (ln > lo) {
		/* l's last children go to the front of r. */
		size_type const k = ln - lo;
		size_type const moved = entries(lc + lo, lc + ln);
		std::move_backward(rc, rc + rn, rc + rn + k);
		std::move_backward(rk, rk + rn - 1, rk + rn - 1 + k);
		std::copy(lc + lo, lc + ln, rc);
		std::copy(lk + lo, lk + ln - 1, rk);
		rk[k - 1] = sep;
		sep = lk[lo - 1];
		l->size -= moved;
		r->size += moved;
	} else {
		/* r's first children go to the end of l. */
		assert(ln < lo);
		size_type const k = lo - ln;
		size_type const moved = entries(rc, rc + k);
		std::copy(rc, rc + k, lc + ln);
		lk[ln - 1] = sep;
		std::copy(rk, rk + k - 1, lk + ln);
		sep = rk[k - 1];
		std::move(rc + k, rc + rn, rc);
		std::move(rk + k, rk + rn - 1, rk);
		l->size += moved;
		r->size -= moved;
	}
	l->node_n = lo;
	r->node_n = n - lo;
	l->update_aug();
	r->update_aug();
	return false;
}

} /* namespace pab_tree */

#endif /* PABTREE_B_TREE_IMPL_BATCH_OP_B_SERIAL_DELETE_HPP_ */

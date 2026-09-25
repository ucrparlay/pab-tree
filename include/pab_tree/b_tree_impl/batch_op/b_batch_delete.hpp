#ifndef PABTREE_B_TREE_IMPL_BATCH_OP_B_BATCH_DELETE_HPP_
#define PABTREE_B_TREE_IMPL_BATCH_OP_B_BATCH_DELETE_HPP_

#include <algorithm>
#include <array>
#include <cassert>
#include <iterator>
#include <memory>
#include <numeric>

#include "pab_tree/b_tree.h"
#include "parlay/monoid.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/slice.h"
#include <tuple>

namespace pab_tree
{
template <typename TT>
template <typename R>
void btree<TT>::batch_delete(R &&in)
{
	static_assert(parlay::is_random_access_range_v<R>);
	static_assert(std::is_constructible_v<parlay::range_value_type_t<R>,
										  parlay::range_reference_type_t<R>>);

	auto in_slice = parlay::make_slice(std::forward<R>(in));

	constexpr auto leaf_entries_pattern = TT::leaf_entries_pattern::hybrid;
	constexpr bool sorted_input = false;
	batch_delete_(in_slice, leaf_entries_pattern, sorted_input,
				  default_hybrid_pred{});
}

template <typename TT>
template <typename R, typename LeafSortPred>
void btree<TT>::batch_delete(R &&in,
							 const TT::pattern_type leaf_entries_pattern,
							 const bool sorted_input,
							 LeafSortPred const &leaf_sort_pred)
{
	static_assert(parlay::is_random_access_range_v<R>);
	static_assert(std::is_constructible_v<parlay::range_value_type_t<R>,
										  parlay::range_reference_type_t<R>>);

	auto in_slice = parlay::make_slice(std::forward<R>(in));

	batch_delete_(in_slice, leaf_entries_pattern, sorted_input, leaf_sort_pred);
}

template <typename TT>
template <typename R, typename LeafSortPred>
	requires std::predicate<LeafSortPred, typename btree<TT>::node_type *, R>
void btree<TT>::batch_delete_(R in, const TT::pattern_type leaf_entries_pattern,
							  const bool sorted_input,
							  LeafSortPred const &leaf_sort_pred)
{
	if (this->root_ == nullptr) {
		return;
	}

	if (in.size() == 0) {
		return;
	}

	static_assert(
		std::same_as<typename decltype(in)::value_type, typename TT::key_type>);

	/* Accept any random-access range, as batch_insert does. */
	typename TT::key_seq sorted_seq;
	parlay::slice<const key_type *, const key_type *> sorted_slice{ nullptr,
																	nullptr };
	if (!sorted_input) {
		sorted_seq = parlay::sort(in, typename TT::key_compare{});
		sorted_slice =
			parlay::make_slice(sorted_seq.cbegin(), sorted_seq.cend());
	} else if constexpr (std::contiguous_iterator<decltype(std::begin(in))>) {
		sorted_slice = { std::to_address(std::begin(in)),
						 std::to_address(std::end(in)) };
	} else {
		sorted_seq = parlay::to_sequence(in);
		sorted_slice =
			parlay::make_slice(sorted_seq.cbegin(), sorted_seq.cend());
	}

	auto [invalid_flag, invalid_subtree_num, reinsert_entry_num] =
		batch_delete_recursive(this->root_, sorted_slice, leaf_entries_pattern,
							   leaf_sort_pred);
	(void)reinsert_entry_num; /* the repair moves nodes, not entries */

	if (invalid_flag) { /* root is invalid */
		if (this->root_->is_leaf) {
			return;
		}
		/*
		 * An invalid root is not an empty one: it is invalid as soon as
		 * every child is invalid, and a leaf is invalid at any size
		 * below k_buf_lb. Test the entry count, or a tree still holding
		 * live entries gets freed. The old assert(root_->size == 0)
		 * here asserted exactly the wrong thing and had to go; it fires
		 * on every tree this branch now recovers.
		 */
		if (this->root_->size == 0) {
			constexpr bool k_enable_granularity = false;
			this->delete_tree_rec<k_enable_granularity>(this->root_);
			this->root_ = nullptr;
			return;
		}
		if (!consolidate_invalid_root(leaf_entries_pattern, leaf_sort_pred)) {
			this->rebuild_from_flatten();
		}
		return;
	}

	if (invalid_subtree_num) {
		if (this->root_->size < TT::k_buf_ub &&
			!this->root_->is_leaf) { /* fits in one leaf directly use leaf */
			this->rebuild_from_flatten();
			return;
		}

#ifdef DEBUG
		[[maybe_unused]] const bool tree_ok = this->validate<false>();
		assert(tree_ok);
		puts(">>> pass tree aug after delete");
#endif /* DEBUG */

		if constexpr (std::same_as<typename TT::delete_policy, graft_delete>) {
			graft_repair(leaf_entries_pattern, leaf_sort_pred);
		} else {
			repair_rec(this->root_, leaf_entries_pattern, leaf_sort_pred);
		}

		/* A root left with one child is a level that only costs a hop. */
		while (!this->root_->is_leaf &&
			   static_cast<interior_type *>(this->root_)->get_node_n() == 1) {
			auto *old = static_cast<interior_type *>(this->root_);
			this->root_ = old->tree_nodes[0];
			free_node<interior_type>(old);
		}
	}
}

/* Flatten the whole tree and rebuild it from the surviving entries. */
template <typename TT>
void btree<TT>::rebuild_from_flatten()
{
	auto tree_entries = TT::entry_seq::uninitialized(this->root_->size);
	flatten_rec(this->root_, parlay::make_slice(tree_entries));
	this->delete_tree_rec<true>(this->root_);
	this->root_ = nullptr; /* build_ releases root_, which is already freed */
	build_(parlay::make_slice(tree_entries));
}

/*
 * The cheap recovery for an invalid root: no entry leaves its leaf-parent,
 * nothing is sorted, and only the interior levels are allocated afresh.
 */
template <typename TT>
template <typename LeafSortPred>
bool btree<TT>::consolidate_invalid_root(
	const TT::pattern_type leaf_entries_pattern,
	LeafSortPred const &leaf_sort_pred)
{
	assert(this->root_ != nullptr && !this->root_->is_leaf);
	assert(get_node_invalid_status(this->root_));

	/*
	 * Read-only guard. k_buf_ub entries leave at least two leaves, so
	 * the assembly never gets a single node; a leaf-parent holding fewer
	 * than k_buf_lb entries would collapse into one underfull leaf.
	 */
	if (this->root_->size < TT::k_buf_ub ||
		!leaf_parents_consolidable_rec(this->root_)) {
		return false;
	}

	const size_type leaf_n = consolidate_leaf_parents_rec(
		this->root_, leaf_entries_pattern, leaf_sort_pred);
	assert(leaf_n >= 2);

	/* keys[i] is the separator before leaf i; keys[0] is never read. */
	auto nodes = TT::node_seq::uninitialized(leaf_n);
	auto keys = TT::key_seq::uninitialized(leaf_n);
	collect_leaves_rec(this->root_, key_type{}, parlay::make_slice(nodes),
					   parlay::make_slice(keys));
	/* root_ is freed now; nothing may read it before the assignment. */
	this->root_ = assemble_interior_levels(
		parlay::make_slice(nodes), parlay::make_slice(keys).cut(1, leaf_n));

#ifdef DEBUG
	[[maybe_unused]] const bool tree_ok = this->validate<false>();
	assert(tree_ok);
#endif /* DEBUG */
	return true;
}

/* check whether local rebalance leaf is possible */
template <typename TT>
bool btree<TT>::leaf_parents_consolidable_rec(node_type const *node)
{
	assert(!node->is_leaf);
	auto const *interior = static_cast<interior_type const *>(node);
	const buf_size_type node_n = interior->get_node_n();

	if (node_has_leaf_children(node)) {
		size_type total = 0;
		for (buf_size_type i = 0; i < node_n; ++i) {
			total += interior->tree_nodes[i]->size;
		}
		return total == 0 || total >= TT::k_buf_lb;
	}

	boolean_buf_ub_arr ok;
	parlay::parallel_for(
		0, node_n,
		[&](buf_size_type i) {
			ok[i] = leaf_parents_consolidable_rec(interior->tree_nodes[i]);
		},
		force_parallel_recursion<true>(interior) ? 1 : TT::k_buf_ub);
	return std::all_of(ok.begin(), ok.begin() + node_n,
					   [](bool b) { return b; });
}

template <typename TT>
template <typename LeafSortPred>
TT::size_type btree<TT>::consolidate_leaf_parents_rec(
	node_type *node, const TT::pattern_type leaf_entries_pattern,
	LeafSortPred const &leaf_sort_pred)
{
	assert(!node->is_leaf);
	auto interior = static_cast<interior_type *>(node);
	const buf_size_type node_n = interior->get_node_n();

	if (node_has_leaf_children(node)) {
		/* Decided from the leaves, not from the deletion flags. */
		size_type total = 0;
		size_type underfull_n = 0;
		size_type underfull_sum = 0; /* what balance_over_leaves moves */
		for (buf_size_type i = 0; i < node_n; ++i) {
			auto const *leaf = interior->tree_nodes[i];
			total += leaf->size;
			if (leaf->size < TT::k_buf_lb) {
				underfull_n++;
				underfull_sum += leaf->size;
			}
		}
		if (total == 0) { /* every leaf is empty: drop them all */
			for (buf_size_type i = 0; i < node_n; ++i) {
				free_node<leaf_type>(interior->tree_nodes[i]);
			}
			interior->node_n = 0;
			return 0;
		}
		if (underfull_n > 0) {
			balance_over_leaves(node, underfull_sum, leaf_entries_pattern,
								leaf_sort_pred);
		}
		return interior->get_node_n();
	}

	size_buf_ub_arr leaf_n;
	parlay::parallel_for(
		0, node_n,
		[&](buf_size_type i) {
			leaf_n[i] = consolidate_leaf_parents_rec(
				interior->tree_nodes[i], leaf_entries_pattern, leaf_sort_pred);
		},
		force_parallel_recursion<true>(interior) ? 1 : TT::k_buf_ub);
	return std::accumulate(leaf_n.begin(), leaf_n.begin() + node_n,
						   static_cast<size_type>(0));
}

template <typename TT>
TT::size_type btree<TT>::count_leaves_rec(node_type const *node)
{
	assert(!node->is_leaf);
	auto const *interior = static_cast<interior_type const *>(node);
	const buf_size_type node_n = interior->get_node_n();

	/* node_n first: an emptied leaf-parent's slot 0 is a freed leaf. */
	if (node_n == 0 || node_has_leaf_children(node)) {
		return node_n;
	}

	size_buf_ub_arr leaf_n;
	parlay::parallel_for(
		0, node_n,
		[&](buf_size_type i) {
			leaf_n[i] = count_leaves_rec(interior->tree_nodes[i]);
		},
		force_parallel_recursion<true>(interior) ? 1 : TT::k_buf_ub);
	return std::accumulate(leaf_n.begin(), leaf_n.begin() + node_n,
						   static_cast<size_type>(0));
}

template <typename TT>
void btree<TT>::collect_leaves_rec(node_type *node, key_type const &key_before,
								   TT::node_slice out_nodes,
								   TT::split_slice out_keys)
{
	assert(!node->is_leaf);
	auto interior = static_cast<interior_type *>(node);
	const buf_size_type node_n = interior->get_node_n();
	assert(out_nodes.size() == out_keys.size());

	if (node_n == 0) { /* emptied: no leaf below */
		assert(out_nodes.size() == 0);
		free_node<interior_type>(node);
		return;
	}

	if (node_has_leaf_children(node)) {
		assert(out_nodes.size() == node_n);
		for (buf_size_type i = 0; i < node_n; ++i) {
			out_nodes[i] = interior->tree_nodes[i];
			out_keys[i] = i == 0 ? key_before : interior->split[i - 1];
		}
		free_node<interior_type>(node);
		return;
	}

	size_buf_ub_arr leaf_n;
	parlay::parallel_for(
		0, node_n,
		[&](buf_size_type i) {
			leaf_n[i] = count_leaves_rec(interior->tree_nodes[i]);
		},
		force_parallel_recursion<true>(interior) ? 1 : TT::k_buf_ub);

	std::array<size_type, TT::k_buf_ub + 1> offsets;
	offsets[0] = 0;
	for (buf_size_type i = 0; i < node_n; ++i) {
		offsets[i + 1] = offsets[i] + leaf_n[i];
	}
	assert(offsets[node_n] == out_nodes.size());

	/*
	 * Child i > 0 starts after split[i - 1]; child 0 starts where this
	 * node does. A child with no leaf gets an empty cut and no key.
	 */
	parlay::parallel_for(
		0, node_n,
		[&](buf_size_type i) {
			collect_leaves_rec(interior->tree_nodes[i],
							   i == 0 ? key_before : interior->split[i - 1],
							   out_nodes.cut(offsets[i], offsets[i + 1]),
							   out_keys.cut(offsets[i], offsets[i + 1]));
		},
		force_parallel_recursion<true>(interior) ? 1 : TT::k_buf_ub);

	free_node<interior_type>(node); /* after the join: children read split */
}

/*
 * Returns (invalid_flag, needs_fetch, reinsert_entry_num).
 * needs_fetch: any descendant needs fetch_invalid_subtree_nodes (not stored in
 * node).
 */
template <typename TT>
template <typename LeafSortPred>
std::tuple<bool, bool, typename TT::size_type>
btree<TT>::batch_delete_recursive(node_type *node, TT::const_key_slice in,
								  const TT::pattern_type leaf_entries_pattern,
								  LeafSortPred const &leaf_sort_pred)
{
	if (in.size() == 0) {
		/* stop here, than all subtree is valid */
		if (!node->is_leaf) {
			static_cast<interior_type *>(node)->reset_deletion_flag();
		}
		assert(node_is_valid(node) && node_contain_invalid_subtree(node) == 0 &&
			   get_node_reinsert_size(node) == 0);
		return std::make_tuple(false, 0, 0);
	}

	if (node->is_leaf) {
		[[maybe_unused]] auto old_size = node->size;
		[[maybe_unused]] auto new_node = delete_points_4_leaf(
			node, in, leaf_entries_pattern, leaf_sort_pred);
		assert(new_node == node);
		/* Absent keys are skipped, so at most in.size() came out. */
		assert(new_node->size <= old_size);
		assert(old_size - new_node->size <= in.size());
		/*
		 * return std::make_tuple(node->size < TT::k_buf_lb,
		 *                        node->size < TT::k_buf_lb ? 1 : 0,
		 */
		return std::make_tuple(get_node_invalid_status(node),
							   node_contain_invalid_subtree(node),
							   get_node_reinsert_size(node));
	}

	auto interior = static_cast<interior_type *>(node);

	size_buf_ub_arr split_pos_seq;
	partition_batch(split_pos_seq, in, interior);

#ifdef DEBUG
	/*
	 * Ensure all elements between two splits are correct
	 * i.e., less than the split value
	 */
	for (buf_size_type i = 1; i < interior->get_split_n(); i++) {
		assert(parlay::all_of(in.cut(split_pos_seq[i - 1], split_pos_seq[i]),
							  [&](key_type const k) {
								  return !TT::key_less(
											 k, interior->split[i - 1]) &&
										 TT::key_less(k, interior->split[i]);
							  }));
	}
#endif /* DEBUG */

	boolean_buf_ub_arr contain_invalid_subtree;
	boolean_buf_ub_arr invalid_subtree_flag;
	size_buf_ub_arr reinsert_entries_num;

	/*
	 * Hoist: same answer for every child since node's children are all at one
	 * level.
	 */
	const bool children_are_interior = !node_has_leaf_children(node);

	buf_size_buf_ub_arr nodes_map;
	size_buf_ub_arr touched_l, touched_r;
	buf_size_type cnt = 0;
	for (buf_size_type i = 0; i < interior->get_node_n(); i++) {
		size_type l = (i == 0 ? 0 : split_pos_seq[i - 1]);
		size_type r =
			(i == interior->get_node_n() - 1 ? in.size() : split_pos_seq[i]);
		if (l != r) {
			touched_l[cnt] = l;
			touched_r[cnt] = r;
			nodes_map[cnt++] = i;
		} else { /* no element in this subtree, so it's valid */
			invalid_subtree_flag[i] = false;
			contain_invalid_subtree[i] = 0;
			reinsert_entries_num[i] = 0;
			/*
			 * if a tree node receive no deletion, we need to
			 * explicitly reset its deletion information, because
			 * the reinsertion phase needs to access the reinsert
			 * information from the tree node directly
			 */
			if (children_are_interior) {
				static_cast<interior_type *>(interior->tree_nodes[i])
					->reset_deletion_flag();
			}
		}
	}

	parlay::parallel_for(
		0, cnt,
		[&](buf_size_type i) {
			std::tie(invalid_subtree_flag[nodes_map[i]],
					 contain_invalid_subtree[nodes_map[i]],
					 reinsert_entries_num[nodes_map[i]]) =
				batch_delete_recursive(interior->tree_nodes[nodes_map[i]],
									   in.cut(touched_l[i], touched_r[i]),
									   leaf_entries_pattern, leaf_sort_pred);
		},
		1);

	return batch_delete_update_interior_size(node, contain_invalid_subtree,
											 invalid_subtree_flag,
											 reinsert_entries_num);
}

} /* namespace pab_tree */

#endif /* PABTREE_B_TREE_IMPL_BATCH_OP_B_BATCH_DELETE_HPP_ */

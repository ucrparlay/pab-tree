#ifndef PABTREE_B_TREE_IMPL_TREE_OP_NODE_OP_HPP_
#define PABTREE_B_TREE_IMPL_TREE_OP_NODE_OP_HPP_

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <numeric>
#include <tuple>
#include <type_traits>
#include <utility>

#include "parlay/monoid.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "pab_tree/b_tree.h"

namespace pab_tree
{
template <typename TT>
template <typename OutputHandler>
TT::size_type btree<TT>::generate_leaf_nodes(TT::entry_slice in,
											 OutputHandler &&output_handler)
{
	size_type item_n = in.size();
	size_type bkt_n = get_bkt_num(item_n);
	assert(bkt_n > 0);

	parlay::parallel_for(0, bkt_n, [&](size_type i) {
		if (i < bkt_n - 1) {
			output_handler.set_key(
				i,
				entry_traits::get_key(in[get_bkt_end_idx(i, bkt_n, item_n)]));
		}
		size_type ele_l = get_bkt_st_idx(i, bkt_n, item_n);
		size_type ele_r =
			(i == bkt_n - 1) ? item_n : get_bkt_end_idx(i, bkt_n, item_n);
		output_handler.set_node(i,
								alloc_normal_leaf_node<entry_slice, leaf_type>(
									in.cut(ele_l, ele_r)));
	});
	return bkt_n;
}

template <typename TT>
template <typename OutputHandler>
TT::size_type btree<TT>::generate_interior_nodes(TT::node_slice node_in,
												 TT::split_slice key_in,
												 OutputHandler &&output_handler)
{
	size_type item_n = node_in.size();
	size_type bkt_n = get_bkt_num(item_n);
	assert(bkt_n > 0);

	parlay::parallel_for(0, bkt_n, [&](size_type i) {
		if (i < bkt_n - 1) {
			output_handler.set_key(
				i, key_in[get_bkt_end_idx(i, bkt_n, item_n) - 1]);
		}

		size_type node_l = get_bkt_st_idx(i, bkt_n, item_n);
		size_type node_r =
			(i == bkt_n - 1) ? item_n : get_bkt_end_idx(i, bkt_n, item_n);
		size_type key_l = get_bkt_st_idx(i, bkt_n, item_n);
		size_type key_r = (i == bkt_n - 1) ?
							  key_in.size() :
							  get_bkt_end_idx(i, bkt_n, item_n) - 1;

		auto *node = alloc_interior_node<interior_type, TT>(
			node_in.cut(node_l, node_r), key_in.cut(key_l, key_r));
		node->update_aug();
		output_handler.set_node(i, node);
	});
	return bkt_n;
}

template <typename TT>
TT::node_type *btree<TT>::batch_insert_update_interior(
	node_type *node, node_split_pair_slice node_split_slice,
	size_buf_ub_arr const &split_pos_arr, size_type new_node_n,
	size_buf_ub_arr const &return_node_n_arr)
{
	assert(!node->is_leaf);
	assert(new_node_n <= TT::k_buf_ub);

	auto interior = static_cast<interior_type *>(node);

	assert(std::numeric_limits<buf_size_type>::max() >= new_node_n);

	/*
	 * Fast path: no child split — tree_nodes/split arrays unchanged, only
	 * size differs. return_node_n_arr[i] is always 0 or >=2
	 * (generate_leaf_nodes/generate_interior_nodes always produce >=2
	 * buckets), so new_node_n == get_node_n() iff all are 0.
	 */
	if (new_node_n == static_cast<size_type>(interior->get_node_n())) {
		size_type tot = 0;
		for (buf_size_type i = 0; i < interior->get_node_n(); i++) {
			tot += interior->tree_nodes[i]->size;
		}
		interior->size = tot;
		interior->update_aug();
		return node;
	}

	/* Slow path: some child split — rebuild tree_nodes and split arrays. */
	size_type tot_entry_sz = 0;
	buf_size_type node_idx = 0;
	typename interior_type::node_arr new_nodes;
	for (buf_size_type i = 0; i < interior->get_node_n(); i++) {
		if (return_node_n_arr[i] == 0) {
			new_nodes[node_idx++] = interior->tree_nodes[i];
			tot_entry_sz += new_nodes[node_idx - 1]->size;
		} else {
			size_type st = 2 * (i == 0 ? 0 : split_pos_arr[i - 1]);
			for (buf_size_type j = 0; j < return_node_n_arr[i]; j++) {
				new_nodes[node_idx++] = node_split_slice[st + j].first;
				tot_entry_sz += new_nodes[node_idx - 1]->size;
			}
		}
	}

	/* the original split will always stay */
	typename interior_type::split_arr new_splits;
	buf_size_type split_idx = 0;
	for (buf_size_type i = 0; i < interior->get_node_n(); i++) {
		size_type st = 2 * (i == 0 ? 0 : split_pos_arr[i - 1]);
		/*
		 * HACK: if return_node_n_arr[i] == 0, then no new splits from
		 * this child should skip; but -1 always smaller than 0, so the
		 * loop never runs. Needs to handle the type carefully.
		 */
		for (int j = 0; j < static_cast<int>(return_node_n_arr[i]) - 1; j++) {
			new_splits[split_idx++] = node_split_slice[st + j].second;
		}
		if (i < interior->get_node_n() - 1) {
			new_splits[split_idx++] = interior->split[i];
		}
	}
	assert(node_idx == split_idx + 1);
	assert(node_idx == new_node_n);

	interior->tree_nodes = std::move(new_nodes);
	interior->split = std::move(new_splits);
	interior->size = tot_entry_sz;
	interior->node_n = new_node_n;

#ifdef DEBUG
	for (buf_size_type i = 0; i < interior->get_node_n(); i++) {
		assert(interior->tree_nodes[i] != nullptr);
		if (interior->get_node_n() == 1) {
			continue;
		}
		auto box = get_node_box_rec(interior->tree_nodes[i]);
		if (i == 0) {
			assert(TT::key_less(box.second, interior->split[0]));
		} else if (i == interior->get_node_n() - 1) {
			assert(!TT::key_less(box.first,
								 interior->split[interior->get_split_n() - 1]));
		} else {
			assert(!TT::key_less(box.first, interior->split[i - 1]) &&
				   TT::key_less(box.second, interior->split[i]));
		}
	}
#endif /* DEBUG */

	interior->update_aug();
	return node;
}

template <typename TT>
TT::size_type btree<TT>::handling_interior_split_overflow(
	node_type *node, node_split_pair_slice node_split_slice,
	size_buf_ub_arr const &split_pos_arr, size_type new_node_n,
	size_buf_ub_arr const &return_node_n_arr)
{
	assert(!node->is_leaf);
	auto interior = static_cast<interior_type *>(node);
	typename TT::node_seq flatten_node_seq =
		TT::node_seq::uninitialized(new_node_n);
	typename TT::split_seq flatten_split_seq =
		TT::split_seq::uninitialized(new_node_n - 1);

	/* merge node */
	size_type node_idx = 0;
	for (buf_size_type i = 0; i < interior->get_node_n(); i++) {
		if (return_node_n_arr[i] == 0) {
			flatten_node_seq[node_idx++] = interior->tree_nodes[i];
		} else {
			size_type st = 2 * (i == 0 ? 0 : split_pos_arr[i - 1]);

			parlay::parallel_for(0, return_node_n_arr[i], [&](size_type j) {
				flatten_node_seq[node_idx + j] = node_split_slice[st + j].first;
			});
			node_idx += return_node_n_arr[i];
		}
	}

	/* merge split */
	size_type split_idx = 0;
	for (buf_size_type i = 0; i < interior->get_node_n(); i++) {
		if (return_node_n_arr[i] > 0) {
			size_type st = 2 * (i == 0 ? 0 : split_pos_arr[i - 1]);
			assert(return_node_n_arr[i] > 0 ||
				   static_cast<int>(return_node_n_arr[i]) - 1 < 0);

			parlay::parallel_for(0, static_cast<int>(return_node_n_arr[i]) - 1,
								 [&](size_type j) {
									 flatten_split_seq[split_idx + j] =
										 node_split_slice[st + j].second;
								 });
			split_idx += return_node_n_arr[i] - 1;
		}
		if (i < interior->get_node_n() -
					1) { /* skip the new splits for last node */
			flatten_split_seq[split_idx++] = interior->split[i];
		}
	}

	assert(node_idx == split_idx + 1);
	assert(parlay::is_sorted(
		flatten_split_seq,
		[&](TT::key_type a, TT::key_type b) { return TT::key_less(a, b); }));

	/* launch rebuild */
	free_node<interior_type>(node);
	[[maybe_unused]] size_type rt_bkt_n = generate_interior_nodes(
		parlay::make_slice(flatten_node_seq),
		parlay::make_slice(flatten_split_seq),
		gen_nodes_output_handler{
			.set_key = [&](size_type i,
						   auto key) { node_split_slice[i].second = key; },
			.set_node =
				[&](size_type i, auto child) {
					node_split_slice[i].first = child;
				} });

	return rt_bkt_n;
}

template <typename TT>
std::tuple<bool, bool, typename TT::size_type>
btree<TT>::batch_delete_update_interior_size(
	node_type *node, boolean_buf_ub_arr const &invalid_subtree_num_arr,
	boolean_buf_ub_arr const &invalid_subtree_flag_arr,
	size_buf_ub_arr const &reinsert_entries_num_arr)
/*
 * update the size and valid status of the interior node, return the number of
 * invalid subtree
 */
{
	assert(!node->is_leaf);
	auto interior = static_cast<interior_type *>(node);
	size_type tot_entry_sz = 0;
	bool contain_invalid = 0;
	size_type reinsert_entries_n = 0;
	buf_size_type invalid_tree_n = 0;
	for (buf_size_type i = 0; i < interior->get_node_n(); i++) {
		tot_entry_sz += interior->tree_nodes[i]->size;
		invalid_tree_n += invalid_subtree_flag_arr[i] ? 1 : 0;
		contain_invalid |= invalid_subtree_num_arr[i];
		reinsert_entries_n += reinsert_entries_num_arr[i];
	}
	interior->size = tot_entry_sz;
	interior->update_aug();
	interior->valid =
		(node != this->root_ ?
			 (interior->get_node_n() - invalid_tree_n) >= TT::k_buf_lb :
			 (interior->get_node_n() - invalid_tree_n) > 0);
	interior->contain_invalid_subtree =
		interior->valid ? contain_invalid : true;
	interior->reinsert_size =
		interior->valid ? reinsert_entries_n : interior->size;
	return std::make_tuple(get_node_invalid_status(node),
						   node_contain_invalid_subtree(node),
						   get_node_reinsert_size(node));
}

/*
 * Bottom-up repair below a valid node. Children are repaired first, in
 * parallel; then the thin ones among them are merged into siblings.
 * Afterwards every node strictly below is valid. node itself may be
 * left thin, which its own parent resolves.
 */
template <typename TT>
template <typename LeafSortPred>
void btree<TT>::repair_rec(node_type *node,
						   const TT::pattern_type leaf_entries_pattern,
						   LeafSortPred const &leaf_sort_pred)
{
	assert(!node->is_leaf);
	auto interior = static_cast<interior_type *>(node);

	/* For every interior node, including possible root */
	if (!node_has_leaf_children(node)) {
		typename interior_type::node_arr todo;
		buf_size_type todo_n = 0;
		for (buf_size_type i = 0; i < interior->get_node_n(); ++i) {
			if (node_contain_invalid_subtree(interior->tree_nodes[i])) {
				todo[todo_n++] = interior->tree_nodes[i];
			}
		}
		parlay::parallel_for(
			0, todo_n,
			[&](buf_size_type i) {
				repair_rec(todo[i], leaf_entries_pattern, leaf_sort_pred);
			},
			1);
	}
	repair_node(node, leaf_entries_pattern, leaf_sort_pred);
}

/*
 * One node: merge its underfull leaves, or its thin interior children.
 * Decided from the children, not from the deletion flags, so it also
 * serves a node that just absorbed a sibling's children.
 */
template <typename TT>
template <typename LeafSortPred>
void btree<TT>::repair_node(node_type *node,
							const TT::pattern_type leaf_entries_pattern,
							LeafSortPred const &leaf_sort_pred)
{
	assert(!node->is_leaf);
	auto interior = static_cast<interior_type *>(node);
	const buf_size_type node_n = interior->get_node_n();

	if (node_n == 0) {
		interior->size = 0;
		interior->reset_deletion_flag();
		return;
	}

	if (!node_has_leaf_children(node)) {
		balance_over_children(node, leaf_entries_pattern, leaf_sort_pred);
		return;
	}

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
		interior->size = 0;
		interior->reset_deletion_flag();
		return;
	}
	/* A lone underfull leaf has no neighbour here; the parent finds one. */
	if (underfull_n > 0 && node_n >= 2) {
		balance_over_leaves(node, underfull_sum, leaf_entries_pattern,
							leaf_sort_pred);
		return;
	}
	interior->update_aug();
	interior->reset_deletion_flag();
}

/*
 * balance_over_leaves for interior children. A child with fewer than
 * k_buf_lb children is thin; thin children are grouped with the nearest
 * valid sibling to their right and their child arrays are concatenated
 * into it, or the group is re-bucketed when it would overflow k_buf_ub.
 * Only pointers move. A node that absorbed a thin sibling is repaired
 * again, since the sibling may have carried a thin child of its own.
 */
template <typename TT>
template <typename LeafSortPred>
void btree<TT>::balance_over_children(
	node_type *node, const TT::pattern_type leaf_entries_pattern,
	LeafSortPred const &leaf_sort_pred)
{
	auto interior = static_cast<interior_type *>(node);
	assert(!node->is_leaf && !node_has_leaf_children(node));

	/* Drop empty children, keeping every kept child's right separator. */
	{
		const buf_size_type n = interior->get_node_n();
		const buf_size_type split_n = interior->get_split_n();
		buf_size_type w = 0;
		buf_size_type ws = 0;
		for (buf_size_type i = 0; i < n; ++i) {
			auto *child = static_cast<interior_type *>(interior->tree_nodes[i]);
			if (child->get_node_n() == 0) {
				free_node<interior_type>(child);
				continue;
			}
			const bool has_split = i < split_n;
			if (i != w) {
				interior->tree_nodes[w] = child;
				if (has_split) {
					interior->split[ws] = interior->split[i];
				}
			}
			w++;
			ws += has_split;
		}
		interior->node_n = w;
	}
	const buf_size_type node_n = interior->get_node_n();
	if (node_n == 0) {
		interior->size = 0;
		interior->reset_deletion_flag();
		return;
	}

	auto child_n = [&](buf_size_type i) -> size_type {
		return static_cast<interior_type *>(interior->tree_nodes[i])
			->get_node_n();
	};

	/* Group assignment, as in balance_over_leaves phase 1. */
	buf_size_buf_ub_arr tree_group;
	size_buf_ub_arr group_n;
	bool any_thin = false;
	buf_size_type rpidx = TT::k_buf_ub;
	{
		buf_size_type pivot = TT::k_buf_ub;
		for (buf_size_type i = node_n; i-- > 0;) {
			if (child_n(i) >= TT::k_buf_lb) {
				if (rpidx == TT::k_buf_ub) {
					rpidx = i;
				}
				pivot = i;
				tree_group[i] = i;
				group_n[i] = child_n(i);
			} else {
				any_thin = true;
				tree_group[i] = pivot;
				if (pivot != TT::k_buf_ub) {
					group_n[pivot] += child_n(i);
				}
			}
		}
	}
	if (!any_thin) {
		interior->update_aug();
		interior->reset_deletion_flag();
		return;
	}
	if (rpidx == TT::k_buf_ub) { /* no valid child: one group, last child */
		rpidx = node_n - 1;
		group_n[rpidx] = 0;
		for (buf_size_type i = 0; i < node_n; ++i) {
			tree_group[i] = rpidx;
			group_n[rpidx] += child_n(i);
		}
	}
	for (buf_size_type i = rpidx + 1; i < node_n; ++i) {
		tree_group[i] = rpidx;
		group_n[rpidx] += child_n(i);
	}

	/*
	 * Compact in place: tree_idx <= i throughout, and every group's
	 * outer separator split[j - 1] is read before that slot is written.
	 */
	buf_size_type tree_idx = 0;
	buf_size_type split_idx = 0;
	for (buf_size_type i = 0; i < node_n;) {
		const buf_size_type g = tree_group[i];
		const bool is_rightmost = g == rpidx;
		const size_type sum_n = group_n[g];
		buf_size_type j = i + 1;
		while (j < node_n && tree_group[j] == g) {
			++j;
		}

		if (j == i + 1) {
			interior->tree_nodes[tree_idx++] = interior->tree_nodes[i];
			if (!is_rightmost) {
				interior->split[split_idx++] = interior->split[i];
			}
		} else if (sum_n <= TT::k_buf_ub) {
			/* Merge into the last member: shift its arrays right, fill. */
			auto *dest =
				static_cast<interior_type *>(interior->tree_nodes[j - 1]);
			const buf_size_type dest_n = dest->get_node_n();
			std::copy_backward(dest->tree_nodes.begin(),
							   dest->tree_nodes.begin() + dest_n,
							   dest->tree_nodes.begin() + sum_n);
			std::copy_backward(dest->split.begin(),
							   dest->split.begin() + (dest_n - 1),
							   dest->split.begin() + (sum_n - 1));
			buf_size_type wn = 0;
			buf_size_type wk = 0;
			size_type moved = 0;
			for (buf_size_type k = i; k < j - 1; ++k) {
				auto *src =
					static_cast<interior_type *>(interior->tree_nodes[k]);
				const buf_size_type src_n = src->get_node_n();
				std::copy_n(src->tree_nodes.begin(), src_n,
							dest->tree_nodes.begin() + wn);
				std::copy_n(src->split.begin(), src_n - 1,
							dest->split.begin() + wk);
				wn += src_n;
				wk += src_n - 1;
				dest->split[wk++] = interior->split[k];
				moved += src->size;
				free_node<interior_type>(src);
			}
			assert(wn == sum_n - dest_n && wk == wn);
			dest->node_n = sum_n;
			dest->size += moved;
			dest->update_aug();
			dest->reset_deletion_flag();
			interior->tree_nodes[tree_idx++] = dest;
			if (!is_rightmost) {
				interior->split[split_idx++] = interior->split[j - 1];
			}
			repair_node(dest, leaf_entries_pattern, leaf_sort_pred);
		} else {
			/* Re-bucket the whole group's children into fresh nodes. */
			auto node_buf = TT::node_seq::uninitialized(sum_n);
			auto key_buf = TT::key_seq::uninitialized(sum_n - 1);
			size_type wn = 0;
			size_type wk = 0;
			for (buf_size_type k = i; k < j; ++k) {
				auto *src =
					static_cast<interior_type *>(interior->tree_nodes[k]);
				const buf_size_type src_n = src->get_node_n();
				std::copy_n(src->tree_nodes.begin(), src_n,
							node_buf.begin() + wn);
				std::copy_n(src->split.begin(), src_n - 1,
							key_buf.begin() + wk);
				wn += src_n;
				wk += src_n - 1;
				if (k < j - 1) {
					key_buf[wk++] = interior->split[k];
				}
				free_node<interior_type>(src);
			}
			assert(wn == sum_n && wk == sum_n - 1);
			const buf_size_type base = tree_idx;
			const size_type bkt_n = generate_interior_nodes(
				parlay::make_slice(node_buf), parlay::make_slice(key_buf),
				gen_nodes_output_handler{
					.set_key =
						[&](size_type k, auto key) {
							interior->split[split_idx + k] = key;
						},
					.set_node =
						[&](size_type k, auto child) {
							interior->tree_nodes[base + k] = child;
						} });
			assert(bkt_n >= 2 && bkt_n <= static_cast<size_type>(j - i));
			tree_idx += bkt_n;
			split_idx += bkt_n - 1;
			if (!is_rightmost) {
				interior->split[split_idx++] = interior->split[j - 1];
			}
			for (buf_size_type k = 0; k < bkt_n; ++k) {
				repair_node(interior->tree_nodes[base + k],
							leaf_entries_pattern, leaf_sort_pred);
			}
		}
		i = j;
	}

	assert(tree_idx == split_idx + 1);
	interior->node_n = tree_idx;
	interior->update_aug();
	interior->reset_deletion_flag();
}

template <typename TT>
template <typename LeafSortPred>
void btree<TT>::balance_over_leaves(node_type *node,
									const size_type reinsert_size,
									const TT::pattern_type leaf_entries_pattern,
									LeafSortPred const &leaf_sort_pred)
{
	auto interior = static_cast<interior_type *>(node);

	/*
	 * Phase 1: Group assignment
	 *
	 * Each child is assigned to the nearest valid node on its right (its
	 * pivot). group_sum[pivot] accumulates the total entry count for that
	 * group. Invalid nodes right of all valid nodes are handled in the fix-up
	 * loop.
	 *
	 * For example, with B = 4:
	 * Index:       0  1  2  3  4
	 * Leaf size:   2  5  1  6  2
	 * Group pivot: 1  1  3  3  3
	 */
	buf_size_buf_ub_arr tree_group;
	size_buf_ub_arr group_sum;
	const buf_size_type rightmost_pivot_idx = [&]() -> buf_size_type {
		buf_size_type rpidx = TT::k_buf_ub;
		buf_size_type pivot = TT::k_buf_ub;

		/*
		 * Reverse loop on an unsigned index: test then decrement, so
		 * the body sees node_n-1 .. 0 and node_n == 0 runs it zero
		 * times. Writing `node_n - 1` instead would wrap.
		 */
		for (buf_size_type i = interior->get_node_n(); i-- > 0;) {
			const size_type sz = interior->tree_nodes[i]->size;
			if (node_is_valid(interior->tree_nodes[i])) {
				if (rpidx == TT::k_buf_ub) {
					rpidx = i;
				}
				pivot = i;
				tree_group[i] = i;
				group_sum[i] = sz;
			} else {
				tree_group[i] = pivot;
				if (pivot != TT::k_buf_ub) {
					group_sum[pivot] += sz;
				}
			}
		}
		/*
		 * No valid child: one group pivoted on the last child. The
		 * callers (repair_node, consolidate_invalid_root) pass a
		 * reinsert_size that covers every child, so entry_buf holds
		 * the group, and they skip a node with a single child.
		 */
		if (rpidx == TT::k_buf_ub) {
			assert(interior->get_node_n() >= 2);
			rpidx = interior->get_node_n() - 1;
			group_sum[rpidx] = 0;
			for (buf_size_type i = 0; i < interior->get_node_n(); ++i) {
				tree_group[i] = rpidx;
				group_sum[rpidx] += interior->tree_nodes[i]->size;
			}
			return rpidx;
		}
		for (buf_size_type i = rpidx + 1; i < interior->get_node_n(); ++i) {
			tree_group[i] = rpidx;
			group_sum[rpidx] += interior->tree_nodes[i]->size;
		}
		return rpidx;
	}();

#ifdef DEBUG
	for (buf_size_type i = 0; i < interior->get_node_n(); ++i) {
		if (get_node_invalid_status(interior->tree_nodes[i])) {
			assert(tree_group[i] ==
				   tree_group[i == interior->get_node_n() - 1 ? i - 1 : i + 1]);
		} else {
			assert(tree_group[i] == i);
		}
	}
#endif /* DEBUG */

	/*
	 * Phase 2: Compact and redistribute
	 *
	 * Results written directly into
	 * interior's arrays. Safety: tree_idx ≤ i holds throughout, so writes never
	 * clobber unread data.
	 */
	typename TT::entry_seq
		entry_buf; /* heap-allocated lazily; only if distribute fires */
	buf_size_type tree_idx = 0;
	buf_size_type split_idx = 0;

	/*
	 * Correctness validatation that the last-emitted node falls inside its
	 * expected key range. Called only from assert(), so has no cost in release
	 * builds.
	 */
	[[maybe_unused]] auto quick_check = [&](buf_size_type g) -> bool {
		const auto box = get_node_box_rec(interior->tree_nodes[tree_idx - 1]);
		if (tree_idx == 1) {
			if (split_idx == 0) { /* one tree node for the entire array */
				return true;
			}
			return TT::key_less(box.second, interior->split[split_idx - 1]);
		}
		if (g == rightmost_pivot_idx) {
			return !TT::key_less(box.first, interior->split[split_idx - 1]);
		}
		return !TT::key_less(box.first, interior->split[split_idx - 2]) &&
			   TT::key_less(box.second, interior->split[split_idx - 1]);
	};

	/*
	 * Case A: group is a single valid node — slide its pointer/split
	 * into place.
	 */
	auto handle_single = [&](buf_size_type i, [[maybe_unused]] buf_size_type g,
							 bool is_rightmost) {
		assert(node_is_valid(interior->tree_nodes[i]));
		interior->tree_nodes[tree_idx++] = interior->tree_nodes[i];
		if (!is_rightmost) {
			interior->split[split_idx++] = interior->split[i];
		}
		assert(quick_check(g));
	};

	/*
	 * Case B: group total fits in one leaf — merge all into the
	 * rightmost leaf.
	 */
	auto handle_merge = [&](buf_size_type i, buf_size_type j,
							[[maybe_unused]] buf_size_type g,
							bool is_rightmost) {
		auto *dest = static_cast<leaf_type *>(interior->tree_nodes[j - 1]);
		assert(node_is_valid(dest) || is_rightmost);
		auto *dst = dest->get_ele_end_iter();
		for (buf_size_type k = i; k < j - 1; ++k) {
			auto *leaf = static_cast<leaf_type *>(interior->tree_nodes[k]);
			assert(get_node_invalid_status(leaf) || is_rightmost);
			dst = std::copy_n(leaf->get_ele_begin_iter(), leaf->size, dst);
			free_node<leaf_type>(leaf);
		}
		dest->size = static_cast<size_type>(dst - dest->get_ele_begin_iter());
		auto updated_dest = post_process_leaf_change(
			dest,
			parlay::make_slice(dest->get_ele_begin_iter(),
							   dest->get_ele_end_iter()),
			leaf_entries_pattern, leaf_sort_pred);
		static_cast<leaf_type *>(updated_dest)
			->update_aug(
				static_cast<leaf_type *>(updated_dest)->get_ele_slice());
		interior->tree_nodes[tree_idx++] = updated_dest;
		if (!is_rightmost) {
			interior->split[split_idx++] = interior->split[j - 1];
		}
		assert(quick_check(g));
	};

	/*
	 * Case C: group must be redistributed — gather all entries, sort,
	 * scatter into evenly-sized buckets while reusing existing leaf node
	 * objects.
	 */
	auto handle_distribute = [&](buf_size_type i, buf_size_type j,
								 bool is_rightmost) {
		if (entry_buf.empty()) [[unlikely]] {
			entry_buf =
				TT::entry_seq::uninitialized(reinsert_size + TT::k_buf_ub);
		}

		auto *ep = entry_buf.begin();
		for (buf_size_type k = i; k < j; ++k) {
			auto *leaf = static_cast<leaf_type *>(interior->tree_nodes[k]);
			ep = std::copy_n(leaf->get_ele_begin_iter(), leaf->size, ep);
		}
		const auto item_n = static_cast<size_type>(ep - entry_buf.begin());

		std::sort(
			entry_buf.begin(), entry_buf.begin() + item_n,
			[](auto const &a, auto const &b) { return TT::entry_less(a, b); });

		const buf_size_type bkt_n = get_bkt_num(item_n);
		assert(bkt_n <= j - i);
		auto *sp = entry_buf.begin();
		size_type ele_l = 0;
		for (buf_size_type k = 0; k < bkt_n; ++k) {
			const size_type ele_r =
				(k == bkt_n - 1) ? item_n : get_bkt_end_idx(k, bkt_n, item_n);
			const size_type bkt_size = ele_r - ele_l;
			auto *leaf = static_cast<leaf_type *>(interior->tree_nodes[i + k]);
			std::copy_n(sp, bkt_size, leaf->get_ele_begin_iter());
			sp += bkt_size;
			leaf->size = bkt_size;
			leaf->update_aug(leaf->get_ele_slice());
			leaf->is_sorted = true;
			interior->tree_nodes[tree_idx++] = leaf;
			if (k < bkt_n - 1) {
				interior->split[split_idx++] = entry_traits::get_key(*sp);
			}
			ele_l = ele_r;
		}
		if (!is_rightmost) {
			interior->split[split_idx++] = interior->split[j - 1];
		}
		for (buf_size_type k = i + bkt_n; k < j; ++k) {
			free_node<leaf_type>(interior->tree_nodes[k]);
		}
	};

	/*
	 * Dispatch loop
	 */
	for (buf_size_type i = 0; i < interior->get_node_n();) {
		const buf_size_type g = tree_group[i];
		const bool is_rightmost = (g == rightmost_pivot_idx);
		const size_type sum = group_sum[g];
		buf_size_type j = i + 1;
		while (j < interior->get_node_n() && tree_group[j] == g) {
			++j;
		}

		if (j == i + 1) [[likely]] {
			handle_single(i, g, is_rightmost);
		} else if (sum < TT::k_buf_ub) {
			handle_merge(i, j, g, is_rightmost);
		} else {
			handle_distribute(i, j, is_rightmost);
		}

		i = j;
	}

	assert(tree_idx == split_idx + 1);
	interior->node_n = tree_idx;
	interior->update_aug();
	interior->reset_deletion_flag();

#ifdef DEBUG
	for (buf_size_type i = 0; i < interior->get_node_n(); i++) {
		assert(interior->tree_nodes[i] != nullptr);
		if (interior->get_node_n() == 1) {
			continue;
		}
		const auto box = get_node_box_rec(interior->tree_nodes[i]);
		if (i == 0) {
			assert(TT::key_less(box.second, interior->split[0]));
		} else if (i == interior->get_node_n() - 1) {
			assert(!TT::key_less(box.first,
								 interior->split[interior->get_split_n() - 1]));
		} else {
			assert(!TT::key_less(box.first, interior->split[i - 1]) &&
				   TT::key_less(box.second, interior->split[i]));
		}
	}
#endif /* DEBUG */
}

template <typename TT>
TT::node_type *btree<TT>::copy_tree_recursive(node_type const *node)
{
	if (node->is_leaf) {
		auto const leaf = static_cast<leaf_type const *>(node);
		/*
		 * get_ele() returns an entry_seq, and a template argument list
		 * needs the explicit typename.
		 */
		return alloc_normal_leaf_node<typename TT::entry_seq, leaf_type>(
			leaf->get_ele());
	}
	auto const interior = static_cast<interior_type const *>(node);
	auto new_nodes = parlay::tabulate(
		interior->get_node_n(), [&](size_t i) -> typename TT::node_type * {
			return copy_tree_recursive(interior->tree_nodes[i]);
		});
	/*
	 * get_split_slice() is const here, so use the sequence overload. Both
	 * template parameters are in non-deduced positions.
	 */
	auto const split_slice = interior->get_split_slice();
	typename TT::split_seq new_splits(split_slice.begin(), split_slice.end());
	auto *new_node =
		alloc_interior_node<interior_type, TT>(new_nodes, new_splits);
	new_node->update_aug();
	return new_node;
}

} /* namespace pab_tree */

#endif /* PABTREE_B_TREE_IMPL_TREE_OP_NODE_OP_HPP_ */

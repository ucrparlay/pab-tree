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

/*
 * An interior whose children are interior, after they returned; its size is
 * already down by what they lost. It is valid with k_buf_lb valid children
 * (the root: one). reinsert_size counts the inputs for its tomb: the
 * children's, plus each valid child of an invalid node.
 */
template <typename TT>
std::tuple<bool, bool, typename TT::size_type>
btree<TT>::batch_delete_update_interior_size(
	node_type *node, boolean_buf_ub_arr const &invalid_subtree_num_arr,
	boolean_buf_ub_arr const &invalid_subtree_flag_arr,
	size_buf_ub_arr const &reinsert_num_arr)
{
	assert(!node->is_leaf && !node_has_leaf_children(node));
	auto interior = static_cast<interior_type *>(node);
	bool contain_invalid = 0;
	size_type reinsert_n = 0;
	buf_size_type invalid_tree_n = 0;
	for (buf_size_type i = 0; i < interior->get_node_n(); i++) {
		invalid_tree_n += invalid_subtree_flag_arr[i] ? 1 : 0;
		contain_invalid |= invalid_subtree_num_arr[i];
		reinsert_n += reinsert_num_arr[i];
	}
	const buf_size_type valid_n = interior->get_node_n() - invalid_tree_n;
	interior->update_aug();
	interior->valid =
		node != this->root_ ? valid_n >= TT::k_buf_lb : valid_n > 0;
	interior->contain_invalid_subtree =
		interior->valid ? contain_invalid : true;
	interior->reinsert_size = reinsert_n + (interior->valid ? 0 : valid_n);
	return std::make_tuple(get_node_invalid_status(node),
						   node_contain_invalid_subtree(node),
						   get_node_reinsert_size(node));
}

/*
 * A leaf-parent after its leaves lost keys; its size is already down, and
 * underfull_n / underfull_sum count its underfull leaves and their entries
 * (all among the touched ones). The underfull leaves merge with their
 * siblings first, then the flags follow from what is left. An invalid one
 * hands every leaf to its tomb; an all-empty one stays whole for whoever
 * drops it.
 */
template <typename TT>
template <typename LeafSortPred>
std::tuple<bool, bool, typename TT::size_type>
btree<TT>::batch_delete_update_leaf_parent(
	node_type *node, size_type underfull_n, size_type underfull_sum,
	const TT::pattern_type leaf_entries_pattern,
	LeafSortPred const &leaf_sort_pred)
{
	assert(node_has_leaf_children(node));
	auto interior = static_cast<interior_type *>(node);
	const size_type total = interior->size;

#ifdef DEBUG
	{
		size_type sum = 0;
		size_type under = 0;
		for (buf_size_type i = 0; i < interior->get_node_n(); ++i) {
			sum += interior->tree_nodes[i]->size;
			under += interior->tree_nodes[i]->size < TT::k_buf_lb ? 1 : 0;
		}
		assert(sum == total && under == underfull_n);
	}
#endif /* DEBUG */

	buf_size_type valid_n = 0;
	if (total > 0 && underfull_n > 0 && interior->get_node_n() >= 2) {
		balance_over_leaves(node, underfull_sum, leaf_entries_pattern,
							leaf_sort_pred);
		for (buf_size_type i = 0; i < interior->get_node_n(); ++i) {
			valid_n += node_is_valid(interior->tree_nodes[i]) ? 1 : 0;
		}
	} else {
		interior->update_aug();
		valid_n =
			static_cast<buf_size_type>(interior->get_node_n() - underfull_n);
	}
	interior->valid =
		node != this->root_ ? valid_n >= TT::k_buf_lb : valid_n > 0;
	/* Balanced: a valid leaf-parent has no underfull leaf left. */
	assert(!interior->valid || valid_n == interior->get_node_n());
	interior->contain_invalid_subtree = !interior->valid;
	/*
	 * Inputs for the tomb: every leaf as an item, or, when the leaves
	 * merged into one underfull leaf, its entries.
	 */
	size_type inputs = 0;
	if (!interior->valid && total > 0) {
		assert(valid_n == interior->get_node_n() ||
			   (interior->get_node_n() == 1 && valid_n == 0));
		inputs = valid_n > 0 ? valid_n : total;
	}
	interior->reinsert_size = inputs;
	return std::make_tuple(get_node_invalid_status(node),
						   node_contain_invalid_subtree(node),
						   get_node_reinsert_size(node));
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
		 * callers (batch_delete_update_leaf_parent,
		 * consolidate_invalid_root) pass a reinsert_size that covers
		 * every child, so entry_buf holds the group, and they skip a
		 * node with a single child.
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

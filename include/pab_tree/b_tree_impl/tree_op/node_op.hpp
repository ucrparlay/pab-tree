#ifndef PABTREE_B_TREE_IMPL_TREE_OP_NODE_OP_HPP_
#define PABTREE_B_TREE_IMPL_TREE_OP_NODE_OP_HPP_

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iostream>
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
TT::size_type btree<TT>::generate_leaf_nodes(TT::entry_slice In,
											 OutputHandler &&output_handler)
{
	size_type item_n = In.size();
	size_type bkt_n = get_bkt_num(item_n);
	assert(bkt_n > 0);

	parlay::parallel_for(0, bkt_n, [&](size_type i) {
		if (i < bkt_n - 1) {
			output_handler.set_key(
				i, Entry::get_key(In[get_bkt_end_idx(i, bkt_n, item_n)]));
		}
		size_type ele_l = get_bkt_st_idx(i, bkt_n, item_n);
		size_type ele_r =
			(i == bkt_n - 1) ? item_n : get_bkt_end_idx(i, bkt_n, item_n);
		output_handler.set_node(
			i, alloc_normal_leaf_node<entry_slice, Leaf>(In.cut(ele_l, ele_r)));
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

		auto *node = alloc_interior_node<Interior, TT>(
			node_in.cut(node_l, node_r), key_in.cut(key_l, key_r));
		node->update_aug();
		output_handler.set_node(i, node);
	});
	return bkt_n;
}

template <typename TT>
TT::Node *btree<TT>::batch_insert_update_interior(
	Node *T, node_split_pair_slice node_split_slice,
	size_buf_ub_arr const &split_pos_arr, size_type new_node_n,
	size_buf_ub_arr const &return_node_n_arr)
{
	assert(!T->is_leaf);
	assert(new_node_n <= TT::k_buf_ub);

	auto TI = static_cast<Interior *>(T);

	assert(std::numeric_limits<buf_size_type>::max() >= new_node_n);

	// Fast path: no child split — tree_nodes/split arrays unchanged, only
	// size differs. return_node_n_arr[i] is always 0 or >=2
	// (generate_leaf_nodes/generate_interior_nodes always produce >=2
	// buckets), so new_node_n == get_node_n() iff all are 0.
	if (new_node_n == static_cast<size_type>(TI->get_node_n())) {
		size_type tot = 0;
		for (buf_size_type i = 0; i < TI->get_node_n(); i++) {
			tot += TI->tree_nodes[i]->size;
		}
		TI->size = tot;
		TI->update_aug();
		return T;
	}

	// Slow path: some child split — rebuild tree_nodes and split arrays.
	size_type tot_entry_sz = 0;
	buf_size_type node_idx = 0;
	typename Interior::node_arr new_nodes;
	for (buf_size_type i = 0; i < TI->get_node_n(); i++) {
		if (return_node_n_arr[i] == 0) {
			new_nodes[node_idx++] = TI->tree_nodes[i];
			tot_entry_sz += new_nodes[node_idx - 1]->size;
		} else {
			size_type st = 2 * (i == 0 ? 0 : split_pos_arr[i - 1]);
			for (buf_size_type j = 0; j < return_node_n_arr[i]; j++) {
				new_nodes[node_idx++] = node_split_slice[st + j].first;
				tot_entry_sz += new_nodes[node_idx - 1]->size;
			}
		}
	}

	// the original split will always stay
	typename Interior::split_arr new_splits;
	buf_size_type split_idx = 0;
	for (buf_size_type i = 0; i < TI->get_node_n(); i++) {
		size_type st = 2 * (i == 0 ? 0 : split_pos_arr[i - 1]);
		// HACK: if return_node_n_arr[i] == 0, then no new splits from
		// this child should skip; but -1 always smaller than 0, so the
		// loop never runs. Needs to handle the type carefully.
		for (int j = 0; j < static_cast<int>(return_node_n_arr[i]) - 1; j++) {
			new_splits[split_idx++] = node_split_slice[st + j].second;
		}
		if (i < TI->get_node_n() - 1) {
			new_splits[split_idx++] = TI->split[i];
		}
	}
	assert(node_idx == split_idx + 1);
	assert(node_idx == new_node_n);

	TI->tree_nodes = std::move(new_nodes);
	TI->split = std::move(new_splits);
	TI->size = tot_entry_sz;
	TI->node_n = new_node_n;

#ifdef DEBUG
	for (int i = 0; i < TI->get_node_n(); i++) {
		assert(TI->tree_nodes[i] != nullptr);
		if (TI->get_node_n() == 1) {
			continue;
		}
		auto node = TI->tree_nodes[i];
		auto box = get_node_box_rec(TI->tree_nodes[i]);
		if (i == 0) {
			assert(TT::key_less(box.second, TI->split[0]));
		} else if (i == TI->get_node_n() - 1) {
			assert(!TT::key_less(box.first, TI->split[TI->get_split_n() - 1]));
		} else {
			assert(!TT::key_less(box.first, TI->split[i - 1]) &&
				   TT::key_less(box.second, TI->split[i]));
		}
	}
#endif // DEBUG

	TI->update_aug();
	return T;
}

template <typename TT>
TT::size_type btree<TT>::handling_interior_split_overflow(
	Node *T, node_split_pair_slice node_split_slice,
	size_buf_ub_arr const &split_pos_arr, size_type new_node_n,
	size_buf_ub_arr const &return_node_n_arr)
{
	assert(!T->is_leaf);
	auto TI = static_cast<Interior *>(T);
	typename TT::node_seq flatten_node_seq =
		TT::node_seq::uninitialized(new_node_n);
	typename TT::split_seq flatten_split_seq =
		TT::split_seq::uninitialized(new_node_n - 1);

	// merge node
	size_type node_idx = 0;
	for (buf_size_type i = 0; i < TI->get_node_n(); i++) {
		if (return_node_n_arr[i] == 0) {
			flatten_node_seq[node_idx++] = TI->tree_nodes[i];
		} else {
			size_type st = 2 * (i == 0 ? 0 : split_pos_arr[i - 1]);

			parlay::parallel_for(0, return_node_n_arr[i], [&](size_type j) {
				flatten_node_seq[node_idx + j] = node_split_slice[st + j].first;
			});
			node_idx += return_node_n_arr[i];
		}
	}

	// merge split
	size_type split_idx = 0;
	for (buf_size_type i = 0; i < TI->get_node_n(); i++) {
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
		if (i < TI->get_node_n() - 1) { // skip the new splits for last node
			flatten_split_seq[split_idx++] = TI->split[i];
		}
	}

	assert(node_idx == split_idx + 1);
	assert(parlay::is_sorted(flatten_split_seq, [&](TT::K a, TT::K b) {
		return TT::key_less(a, b);
	}));

	// launch rebuild
	free_node<Interior>(T);
	[[maybe_unused]] size_type rt_bkt_n = generate_interior_nodes(
		parlay::make_slice(flatten_node_seq),
		parlay::make_slice(flatten_split_seq),
		gen_nodes_output_handler{
			.set_key = [&](size_type i,
						   auto key) { node_split_slice[i].second = key; },
			.set_node = [&](size_type i,
							auto node) { node_split_slice[i].first = node; } });

	return rt_bkt_n;
}

template <typename TT>
std::tuple<bool, bool, typename TT::size_type>
btree<TT>::batch_delete_update_interior_size(
	Node *T, boolean_buf_ub_arr const &invalid_subtree_num_arr,
	boolean_buf_ub_arr const &invalid_subtree_flag_arr,
	size_buf_ub_arr const &reinsert_entries_num_arr)
// update the size and valid status of the interior node, return the number of
// invalid subtree
{
	assert(!T->is_leaf);
	auto TI = static_cast<Interior *>(T);
	size_type tot_entry_sz = 0;
	bool contain_invalid = 0;
	size_type reinsert_entries_n = 0;
	buf_size_type invalid_tree_n = 0;
	for (buf_size_type i = 0; i < TI->get_node_n(); i++) {
		tot_entry_sz += TI->tree_nodes[i]->size;
		invalid_tree_n += invalid_subtree_flag_arr[i] ? 1 : 0;
		contain_invalid |= invalid_subtree_num_arr[i];
		reinsert_entries_n += reinsert_entries_num_arr[i];
	}
	TI->size = tot_entry_sz;
	TI->update_aug();
	TI->valid = (T != this->root_ ?
					 (TI->get_node_n() - invalid_tree_n) >= TT::k_buf_lb :
					 (TI->get_node_n() - invalid_tree_n) > 0);
	TI->contain_invalid_subtree = TI->valid ? contain_invalid : true;
	TI->reinsert_size = TI->valid ? reinsert_entries_n : TI->size;
	return std::make_tuple(get_node_invalid_status(T),
						   node_contain_invalid_subtree(T),
						   get_node_reinsert_size(T));
}

template <typename TT>
template <typename LeafSortPred>
void btree<TT>::fetch_invalid_subtree_nodes(
	Node *T, entry_slice reinsert_entries, const bool has_tomb,
	const TT::pattern_type leaf_entries_pattern,
	LeafSortPred const &leaf_sort_pred)
{
	constexpr bool k_sort_reinsert_entries = true;
	assert(reinsert_entries.size() == get_node_reinsert_size(T));

	if (get_node_invalid_status(T)) { // every invalid tree should be flatten
		assert(reinsert_entries.size() == T->size);
		assert(has_tomb == false);

		[[maybe_unused]] auto size = T->size;
		assert(T != nullptr);
		flatten_rec<decltype(reinsert_entries), false, k_sort_reinsert_entries>(
			T, reinsert_entries);
		delete_tree_rec<false>(T);
		return;
	}

	assert(node_is_valid(T));

	if (T->is_leaf) { // do nothing for a valid leaf
		assert(reinsert_entries.size() == 0);
		return;
	}

	auto TI = static_cast<Interior *>(T);

	if (!node_contain_invalid_subtree(T) && !get_node_reinsert_size(T)) {
		// no subtree is invalid, fully valid
		assert(TI->valid);
		assert(reinsert_entries.size() == 0);
		return;
	}

	if (self_balance_node(T,
						  has_tomb)) { // reach a node can be self balanced
		assert(node_is_valid(T));
		if (get_node_reinsert_size(T) == 0) { // just contain empty trees
			assert(node_contain_invalid_subtree(T));
			prune_out_empty_tree(T);
			TI->reset_deletion_flag();
			return;
		}

		balance_over_leaves(T, TI->reinsert_size, leaf_entries_pattern,
							leaf_sort_pred);
		TI->reset_deletion_flag();
		return;
	}

	// PARA: this node is valid, but contains invalid subtree
	typename Interior::node_arr invalid_subtree;
	assert(invalid_subtree.size() == TT::k_buf_ub);
	buf_size_type invalid_idx = 0;
	buf_size_type tree_node_idx = 0;
	buf_size_type split_idx = 0;

	bool reinsert_flag = false; // whether we will reinsert in this node
	for (buf_size_type i = 0; i < TI->get_node_n(); ++i) {
		auto node = TI->tree_nodes[i];

		// this node contain invalid subtree
		if (node_contain_invalid_subtree(node)) {
			invalid_subtree[invalid_idx] = node;
			invalid_idx++;
		}

		if (!reinsert_flag && get_node_invalid_status(node)) {
			reinsert_flag = true;
		}

		// check if this child node contain no invalid subtree, then
		// itself must be valid
		assert(node_contain_invalid_subtree(node) == 0 ? node_is_valid(node) :
														 true);
		assert(node_is_valid(node) ? 1 : node_contain_invalid_subtree(node));

		if (node_is_valid(node)) {
			const bool has_split = i < TI->get_split_n();
			if (i != tree_node_idx) {
				TI->tree_nodes[tree_node_idx] = node;
				if (has_split) {
					TI->split[split_idx] = TI->split[i];
				}
			}
			tree_node_idx += 1;
			split_idx += has_split;
		}
	}
	assert(TI->node_n >= invalid_idx);
	TI->node_n = tree_node_idx;

	size_buf_ub_arr reinsert_sum;
	reinsert_sum[0] = 0;
	for (buf_size_type i = 1; i < invalid_idx; i++) {
		auto node = invalid_subtree[i - 1];
		reinsert_sum[i] = reinsert_sum[i - 1] + get_node_reinsert_size(node);
	}

	//     node,
	//     flatten_node_slice.cut(subtree_sum[i],
	//                                                 : subtree_sum[i +
	// 1]),     reinsert_entries.cut(reinsert_sum[i],
	// : reinsert_sum[i + 1]),

	bool const next_tomb = has_tomb ? !reinsert_flag : false;
	parlay::parallel_for(
		0, invalid_idx,
		[&](buf_size_type i) {
			fetch_invalid_subtree_nodes(
				invalid_subtree[i],
				reinsert_entries.cut(reinsert_sum[i],
									 i == invalid_idx - 1 ?
										 reinsert_entries.size() :
										 reinsert_sum[i + 1]),
				next_tomb, leaf_entries_pattern, leaf_sort_pred);
		},
		1);

	TI->size = std::accumulate(
		TI->tree_nodes.begin(), TI->tree_nodes.begin() + TI->get_node_n(),
		static_cast<size_type>(0),
		[](size_type sum, Node *node) { return sum + node->size; });
	TI->update_aug();
	if (reinsert_flag && has_tomb) {
		assert(parlay::none_of(reinsert_entries, [](auto entry) {
			return Entry::get_val(entry) == 0;
		}));
		assert(parlay::is_sorted(reinsert_entries, [](auto a, auto b) {
			return TT::entry_less(a, b) &&
				   !TT::key_equal(Entry::get_key(a), Entry::get_key(b));
		}));
		reinsert_tree_after_delete(T, reinsert_entries, leaf_entries_pattern,
								   leaf_sort_pred);
	}

	assert((this->validate<true, false>(T)));
	TI->reset_deletion_flag();
}

template <typename TT>
template <typename LeafSortPred>
void btree<TT>::balance_over_leaves(Node *T, const size_type reinsert_size,
									const TT::pattern_type leaf_entries_pattern,
									LeafSortPred const &leaf_sort_pred)
{
	auto TI = static_cast<Interior *>(T);

	// ── Phase 1: Group assignment
	// ────────────────────────────────────────────── Each child is assigned
	// to the nearest valid node on its right (its pivot). group_sum[pivot]
	// accumulates the total entry count for that group. Invalid nodes right
	// of all valid nodes are handled in the fix-up loop.
	buf_size_buf_ub_arr tree_group;
	size_buf_ub_arr group_sum;
	const buf_size_type rightmost_pivot_idx = [&]() -> buf_size_type {
		buf_size_type rpidx = TT::k_buf_ub;
		buf_size_type pivot = TT::k_buf_ub;

		// WARN: signed; --i from 0 is safe
		for (int i = TI->get_node_n() - 1; i >= 0; --i) {
			const size_type sz = TI->tree_nodes[i]->size;
			if (node_is_valid(TI->tree_nodes[i])) {
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
		assert(rpidx != TT::k_buf_ub);
		for (buf_size_type i = rpidx + 1; i < TI->get_node_n(); ++i) {
			tree_group[i] = rpidx;
			group_sum[rpidx] += TI->tree_nodes[i]->size;
		}
		return rpidx;
	}();

#ifdef DEBUG
	for (buf_size_type i = 0; i < TI->get_node_n(); ++i) {
		if (get_node_invalid_status(TI->tree_nodes[i])) {
			assert(tree_group[i] ==
				   tree_group[i == TI->get_node_n() - 1 ? i - 1 : i + 1]);
		} else {
			assert(tree_group[i] == i);
		}
	}
#endif // DEBUG

	// ── Phase 2: Compact and redistribute
	// ───────────────────────────────────── Results written directly into
	// TI's arrays. Safety: tree_idx ≤ i holds throughout, so writes never
	// clobber unread data.
	typename TT::entry_seq
		entry_buf; // heap-allocated lazily; only if distribute fires
	buf_size_type tree_idx = 0;
	buf_size_type split_idx = 0;

	// Validates that the last-emitted node falls inside its expected key
	// range. Called only from assert(), so has no cost in release builds.
	[[maybe_unused]] auto quick_check = [&](buf_size_type g) -> bool {
		const auto box = get_node_box_rec(TI->tree_nodes[tree_idx - 1]);
		if (tree_idx == 1) {
			if (split_idx == 0) { // one tree node for the entire array
				return true;
			}
			return TT::key_less(box.second, TI->split[split_idx - 1]);
		}
		if (g == rightmost_pivot_idx) {
			return !TT::key_less(box.first, TI->split[split_idx - 1]);
		}
		return !TT::key_less(box.first, TI->split[split_idx - 2]) &&
			   TT::key_less(box.second, TI->split[split_idx - 1]);
	};

	// Case A: group is a single valid node — slide its pointer/split
	// into place.
	auto handle_single = [&](buf_size_type i, [[maybe_unused]] buf_size_type g,
							 bool is_rightmost) {
		assert(node_is_valid(TI->tree_nodes[i]));
		TI->tree_nodes[tree_idx++] = TI->tree_nodes[i];
		if (!is_rightmost) {
			TI->split[split_idx++] = TI->split[i];
		}
		assert(quick_check(g));
	};

	// Case B: group total fits in one leaf — merge all into the
	// rightmost leaf.
	auto handle_merge = [&](buf_size_type i, buf_size_type j,
							[[maybe_unused]] buf_size_type g,
							bool is_rightmost) {
		auto *dest = static_cast<Leaf *>(TI->tree_nodes[j - 1]);
		assert(node_is_valid(dest) || is_rightmost);
		auto *dst = dest->get_ele_end_iter();
		for (buf_size_type k = i; k < j - 1; ++k) {
			auto *leaf = static_cast<Leaf *>(TI->tree_nodes[k]);
			assert(get_node_invalid_status(leaf) || is_rightmost);
			dst = std::copy_n(leaf->get_ele_begin_iter(), leaf->size, dst);
			free_node<Leaf>(leaf);
		}
		dest->size = static_cast<size_type>(dst - dest->get_ele_begin_iter());
		auto updated_dest = post_process_leaf_change(
			dest,
			parlay::make_slice(dest->get_ele_begin_iter(),
							   dest->get_ele_end_iter()),
			leaf_entries_pattern, leaf_sort_pred);
		static_cast<Leaf *>(updated_dest)
			->update_aug(static_cast<Leaf *>(updated_dest)->get_ele_slice());
		TI->tree_nodes[tree_idx++] = updated_dest;
		if (!is_rightmost) {
			TI->split[split_idx++] = TI->split[j - 1];
		}
		assert(quick_check(g));
	};

	// Case C: group must be redistributed — gather all entries, sort,
	// scatter into evenly-sized buckets while reusing existing leaf node
	// objects.
	auto handle_distribute = [&](buf_size_type i, buf_size_type j,
								 bool is_rightmost) {
		if (entry_buf.empty()) [[unlikely]] {
			entry_buf =
				TT::entry_seq::uninitialized(reinsert_size + TT::k_buf_ub);
		}

		auto *ep = entry_buf.begin();
		for (buf_size_type k = i; k < j; ++k) {
			auto *leaf = static_cast<Leaf *>(TI->tree_nodes[k]);
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
			auto *leaf = static_cast<Leaf *>(TI->tree_nodes[i + k]);
			std::copy_n(sp, bkt_size, leaf->get_ele_begin_iter());
			sp += bkt_size;
			leaf->size = bkt_size;
			leaf->update_aug(leaf->get_ele_slice());
			leaf->is_sorted = true;
			TI->tree_nodes[tree_idx++] = leaf;
			if (k < bkt_n - 1) {
				TI->split[split_idx++] = Entry::get_key(*sp);
			}
			ele_l = ele_r;
		}
		if (!is_rightmost) {
			TI->split[split_idx++] = TI->split[j - 1];
		}
		for (buf_size_type k = i + bkt_n; k < j; ++k) {
			free_node<Leaf>(TI->tree_nodes[k]);
		}
	};

	// ── Dispatch loop
	// ──────────────────────────────────────────────────────────
	for (buf_size_type i = 0; i < TI->get_node_n();) {
		const buf_size_type g = tree_group[i];
		const bool is_rightmost = (g == rightmost_pivot_idx);
		const size_type sum = group_sum[g];
		buf_size_type j = i + 1;
		while (j < TI->get_node_n() && tree_group[j] == g) {
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
	TI->node_n = tree_idx;
	TI->update_aug();
	TI->reset_deletion_flag();

#ifdef DEBUG
	for (int i = 0; i < TI->get_node_n(); i++) {
		assert(TI->tree_nodes[i] != nullptr);
		if (TI->get_node_n() == 1) {
			continue;
		}
		const auto box = get_node_box_rec(TI->tree_nodes[i]);
		if (i == 0) {
			assert(TT::key_less(box.second, TI->split[0]));
		} else if (i == TI->get_node_n() - 1) {
			assert(!TT::key_less(box.first, TI->split[TI->get_split_n() - 1]));
		} else {
			assert(!TT::key_less(box.first, TI->split[i - 1]) &&
				   TT::key_less(box.second, TI->split[i]));
		}
	}
#endif // DEBUG
}

template <typename TT> void btree<TT>::prune_out_empty_tree(Node *T)
{
	assert(get_node_reinsert_size(T) == 0);
	assert(node_has_leaf_children(T));
	auto TI = static_cast<Interior *>(T);
	buf_size_type tree_node_idx = 0;
	buf_size_type split_idx = 0;
	for (buf_size_type i = 0; i < TI->get_node_n(); ++i) {
		auto node = TI->tree_nodes[i];

		assert(node_is_valid(node) || get_node_reinsert_size(node) == 0);
		if (node_is_valid(node)) {
			const bool has_split = i < TI->get_split_n();
			if (i != tree_node_idx) {
				TI->tree_nodes[tree_node_idx] = node;
				if (has_split) {
					TI->split[split_idx] = TI->split[i];
				}
			}
			tree_node_idx += 1;
			split_idx += has_split;
		} else {
			assert(node->size == 0);
			free_node<Leaf>(node);
		}
	}

	TI->node_n = tree_node_idx;
	TI->update_aug();
}

template <typename TT>
template <typename LeafSortPred>
void btree<TT>::reinsert_tree_after_delete(
	Node *T, entry_slice reinsert_entries,
	TT::pattern_type leaf_entries_pattern, LeafSortPred const &leaf_sort_pred)
{
	node_split_pair_seq node_split_seq =
		node_split_pair_seq::uninitialized(2 * reinsert_entries.size());
	size_type new_node_n = batch_insert_recursive(
		T, reinsert_entries, parlay::make_slice(node_split_seq),
		leaf_entries_pattern, leaf_sort_pred);
	// A non-zero result means batch_insert_recursive freed T and put the
	// replacement children in node_split_seq for the caller to splice.
	// There is no parent pointer here, so we cannot. Abort rather than
	// return to a caller that will use the freed T; the assert alone
	// vanishes under NDEBUG and leaves a use-after-free.
	if (new_node_n != 0) [[unlikely]] {
		std::cerr << "pab_tree: reinsert split the subtree root and "
					 "cannot splice it; see docs/open_issues.md #1\n";
		std::abort();
	}
	assert(new_node_n == 0);
}

template <typename TT> TT::Node *btree<TT>::copy_tree_recursive(Node const *T)
{
	if (T->is_leaf) {
		auto const TL = static_cast<Leaf const *>(T);
		// get_ele() returns an entry_seq, and a template argument list
		// needs the explicit typename.
		return alloc_normal_leaf_node<typename TT::entry_seq, Leaf>(
			TL->get_ele());
	}
	auto const TI = static_cast<Interior const *>(T);
	auto new_nodes = parlay::tabulate(
		TI->get_node_n(), [&](size_t i) -> typename TT::Node * {
			return copy_tree_recursive(TI->tree_nodes[i]);
		});
	// get_split_slice() is const here, so use the sequence overload. Both
	// template parameters are in non-deduced positions.
	auto const split_slice = TI->get_split_slice();
	typename TT::split_seq new_splits(split_slice.begin(), split_slice.end());
	auto *node = alloc_interior_node<Interior, TT>(new_nodes, new_splits);
	node->update_aug();
	return node;
}

} // namespace pab_tree

#endif // PABTREE_B_TREE_IMPL_TREE_OP_NODE_OP_HPP_

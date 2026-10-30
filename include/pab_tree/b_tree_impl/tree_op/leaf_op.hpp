#ifndef PABTREE_B_TREE_IMPL_LEAF_OP_HPP_
#define PABTREE_B_TREE_IMPL_LEAF_OP_HPP_

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <numeric>
#include <tuple>
#include <type_traits>
#include <utility>

#include "pab_tree/b_tree.h"
#include "pab_tree/dependence/concepts.h"

namespace pab_tree
{
template <typename TT>
template <typename R, typename LeafSortPred>
TT::node_type *
btree<TT>::post_process_leaf_change(node_type *node, R in,
									const TT::pattern_type leaf_entries_pattern,
									LeafSortPred const &leaf_sort_pred)
{
	/*
	 * WARN: does not know whether the node->elements are sorted or not,
	 * so might mark it as unsorted if the leaf entries pattern is
	 * unsort/hybrid, even if the entries are actually sorted.
	 */
	assert(node->is_leaf);
	auto leaf = static_cast<leaf_type *>(node);
	if (leaf_entries_pattern == TT::leaf_entries_pattern::sorted) {
		std::sort(leaf->get_ele_begin_iter(), leaf->get_ele_end_iter(),
				  key_less);
		leaf->is_sorted = true;
	} else if (leaf_entries_pattern == TT::leaf_entries_pattern::unsorted) {
		leaf->is_sorted = false;
	} else if (leaf_entries_pattern == TT::leaf_entries_pattern::hybrid) {
		if (leaf_sort_pred(node, in)) {
			std::sort(leaf->get_ele_begin_iter(), leaf->get_ele_end_iter(),
					  key_less);
			leaf->is_sorted = true;
		} else {
			leaf->is_sorted = false;
		}
	} else {
		assert(false && "Invalid leaf entries pattern");
	}
	return node;
}

template <typename TT>
template <typename R, bool Sorted>
void btree<TT>::extract_points_in_leaf(node_type *node, R out)
{
	auto leaf = static_cast<leaf_type *>(node);
	if constexpr (Sorted) {
		if (!leaf->is_sorted) {
			std::sort(leaf->elements.begin(),
					  leaf->elements.begin() + leaf->size, key_less);
			leaf->is_sorted = true;
		}
	}
	assert(out.size() >= leaf->size);
	std::ranges::copy_n(leaf->get_ele_begin_iter(), leaf->size, out.begin());
}

template <typename TT>
template <typename R, typename LeafSortPred>
TT::node_type *
btree<TT>::insert_points_2_leaf(node_type *node, R in,
								TT::pattern_type leaf_entries_pattern,
								LeafSortPred const &leaf_sort_pred)
{
	leaf_type *leaf = static_cast<leaf_type *>(node);

	assert(node->size + in.size() <= TT::k_buf_ub);
	assert(node->size == leaf->get_ele().size());
	if (leaf->elements.size() == 0) {
		assert(leaf->size == 0);
		leaf->initialize_ele_seq();
		assert(leaf->elements.size() == TT::k_buf_ub);
	}
	/* PERF: buf_size_type should be enough since total size fits within */
	std::ranges::copy_n(in.begin(), in.size(), leaf->get_ele_end_iter());
	leaf->size += in.size();
	leaf->update_aug(parlay::make_slice(leaf->elements.begin(),
										leaf->elements.begin() + leaf->size));

	return post_process_leaf_change(node, in, leaf_entries_pattern,
									leaf_sort_pred);
}

template <typename TT>
template <typename R>
TT::size_type
btree<TT>::handling_leaf_split_overflow(node_type *node, R in,
										node_split_pair_slice node_split_slice)
{
	leaf_type *leaf = static_cast<leaf_type *>(node);
	typename TT::entry_seq entry_seq;

	if (leaf->is_sorted) {
		/* in is sorted by batch_insert_. Parallel merge of two sorted runs. */
		entry_seq = parlay::merge(
			parlay::make_slice(leaf->get_ele_begin_iter(),
							   leaf->get_ele_end_iter()),
			in, [](entry_type const &a, entry_type const &b) {
				return TT::entry_less(a, b);
			});
		free_node<leaf_type>(node);
	} else {
		entry_seq = TT::entry_seq::uninitialized(node->size + in.size());
		std::ranges::copy_n(leaf->get_ele_begin_iter(), leaf->size,
							entry_seq.begin());
		parlay::copy(in, parlay::make_slice(entry_seq.begin() + leaf->size,
											entry_seq.end()));
		free_node<leaf_type>(node);
		entry_seq = parlay::sort(entry_seq, key_less);
	}

	size_type rt_bkt_n = generate_leaf_nodes(
		parlay::make_slice(entry_seq),
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
template <typename LeafSortPred>
TT::node_type *
btree<TT>::delete_points_4_leaf(node_type *node, TT::const_key_slice in,
								const TT::pattern_type leaf_entries_pattern,
								LeafSortPred const &leaf_sort_pred)
{
	auto leaf = static_cast<leaf_type *>(node);

	/*
	 * Keys that are not here are skipped, so in may be longer than the
	 * leaf and may name keys it does not hold.
	 */

	auto it = leaf->elements.begin();
	auto end = leaf->elements.begin() + leaf->size;
	buf_size_type delete_cnt = 0;
	for (size_type i = 0; i < in.size(); i++) {
		it = std::ranges::find_if(
			leaf->elements.begin(), end, [&](auto const &e) {
				return TT::key_equal(entry_traits::get_key(e), in[i]);
			});
		if (it != end) {
			std::ranges::iter_swap(it, --end);
			delete_cnt++;
		}
		if (end == leaf->elements.begin()) { /* all elements are deleted */
			break;
		}
	}

	/* Exactly the keys that were found came out. */
	assert(std::cmp_equal(std::distance(leaf->elements.begin(), end),
						  leaf->size - delete_cnt));
	assert(delete_cnt <= in.size());
	leaf->size = std::distance(leaf->elements.begin(), end);

	assert(leaf->size >= 0);
	leaf->update_aug(parlay::make_slice(leaf->elements.begin(),
										leaf->elements.begin() + leaf->size));

	return post_process_leaf_change(node, in, leaf_entries_pattern,
									leaf_sort_pred);
}

} /* namespace pab_tree */

#endif /* PABTREE_B_TREE_IMPL_LEAF_OP_HPP_ */

#ifndef PABTREE_B_TREE_IMPL_BATCH_OP_B_BATCH_INSERT_HPP_
#define PABTREE_B_TREE_IMPL_BATCH_OP_B_BATCH_INSERT_HPP_

#include <cassert>

#include <algorithm>
#include <iterator>
#include <memory>
#include <numeric>

#include "pab_tree/b_tree.h"
#include "pab_tree/dependence/type_trait.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/slice.h"

namespace pab_tree
{
template <typename TT>
template <typename R>
void btree<TT>::batch_insert(R &&in)
{
	static_assert(parlay::is_random_access_range_v<R>);
	static_assert(std::is_constructible_v<parlay::range_value_type_t<R>,
										  parlay::range_reference_type_t<R>>);

	auto in_slice = parlay::make_slice(std::forward<R>(in));

	constexpr auto leaf_entries_pattern = TT::leaf_entries_pattern::hybrid;
	constexpr bool sorted_input = false;
	batch_insert_(in_slice, leaf_entries_pattern, sorted_input,
				  default_hybrid_pred{});
}

template <typename TT>
template <typename R, typename LeafSortPred>
void btree<TT>::batch_insert(R &&in,
							 const TT::pattern_type leaf_entries_pattern,
							 const bool sorted_input,
							 LeafSortPred const &leaf_sort_pred)
{
	static_assert(parlay::is_random_access_range_v<R>);
	static_assert(std::is_constructible_v<parlay::range_value_type_t<R>,
										  parlay::range_reference_type_t<R>>);

	auto in_slice = parlay::make_slice(std::forward<R>(in));

	batch_insert_(in_slice, leaf_entries_pattern, sorted_input, leaf_sort_pred);
}

template <typename TT>
template <typename R, typename LeafSortPred>
	requires std::predicate<LeafSortPred, typename btree<TT>::node_type *, R>
void btree<TT>::batch_insert_(R in, const TT::pattern_type leaf_entries_pattern,
							  const bool sorted_input,
							  LeafSortPred const &leaf_sort_pred)
{
	if (this->root_ == nullptr) {
		return build_(in);
	}

	if (this->root_->size == 0) { /* empty ghost node should be eliminated */
		return build_(in); /* build_ releases the old tree */
	}

	/*
	 * Accept any random-access range. A contiguous one is borrowed as is;
	 * anything else is materialised, which is also what the sort does.
	 */
	typename TT::entry_seq sorted_seq;
	parlay::slice<const entry_type *, const entry_type *> sorted_slice{
		nullptr, nullptr
	};
	if (!sorted_input) {
		sorted_seq = parlay::sort(in, key_less);
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

	/*
	 * Keys must be distinct, in the batch and against the tree. Only the
	 * batch is checkable here.
	 */
	assert(parlay::is_sorted(sorted_slice,
							 [](entry_type const &a, entry_type const &b) {
								 return !TT::entry_less(b, a);
							 }));

	/* begin insert */
	node_split_pair_seq node_split_seq =
		node_split_pair_seq::uninitialized(2 * sorted_slice.size());
	size_type new_node_n = batch_insert_recursive(
		this->root_, sorted_slice, parlay::make_slice(node_split_seq),
		leaf_entries_pattern, leaf_sort_pred);

	if (new_node_n > 0) {
		auto updated_node_seq = parlay::tabulate(
			new_node_n, [&](size_type i) { return node_split_seq[i].first; });
		auto updated_split_seq =
			parlay::tabulate(new_node_n - 1, [&](size_type i) {
				return node_split_seq[i].second;
			});

		this->root_ =
			assemble_interior_levels(parlay::make_slice(updated_node_seq),
									 parlay::make_slice(updated_split_seq));
	}
}

template <typename TT>
template <typename R, typename LeafSortPred>
TT::size_type
btree<TT>::batch_insert_recursive(node_type *node, R in,
								  node_split_pair_slice node_split_slice,
								  const TT::pattern_type leaf_entries_pattern,
								  LeafSortPred const &leaf_sort_pred)
{
	assert(2 * in.size() == node_split_slice.size());

	size_type n = in.size();
	if (n == 0) {
		return 0;
	}

	if (node->is_leaf) {
		leaf_type *leaf = static_cast<leaf_type *>(node);
		if (leaf->size + n <= TT::k_buf_ub) {
			[[maybe_unused]] node_type *updated_node = insert_points_2_leaf(
				node, in, leaf_entries_pattern, leaf_sort_pred);
			assert(updated_node == node);
			return 0;
		}
		return handling_leaf_split_overflow(node, in, node_split_slice);
	}

	auto interior = static_cast<interior_type *>(node);

	size_buf_ub_arr split_pos_seq;
	assert(interior->get_node_n() > 0);
	partition_batch(split_pos_seq, in, interior);

	/*
	 *   // sieve_points(split_pos_seq, in, interior->get_split_slice());
	 * } else {
	 */
#ifdef DEBUG
	for (buf_size_type i = 1; i < interior->get_split_n(); i++) {
		assert(parlay::all_of(in.cut(split_pos_seq[i - 1], split_pos_seq[i]),
							  [&](entry_type const e) {
								  return !TT::key_less(
											 entry_traits::get_key(e),
											 interior->split[i - 1]) &&
										 TT::key_less(entry_traits::get_key(e),
													  interior->split[i]);
							  }));
	}
#endif /* DEBUG */

	size_buf_ub_arr new_split_node_n_arr;
	buf_size_buf_ub_arr nodes_map;
	buf_size_type cnt = 0;
	for (buf_size_type i = 0; i < interior->get_node_n(); i++) {
		size_type l = (i == 0 ? 0 : split_pos_seq[i - 1]);
		size_type r = (i == interior->get_node_n() - 1 ? n : split_pos_seq[i]);
		if (l != r) {
			nodes_map[cnt++] = i;
		} else {
			new_split_node_n_arr[i] = 0; /* i-th child remains the same */
		}
	}

	parlay::parallel_for(
		0, cnt,
		[&](buf_size_type i) {
			size_type l =
				nodes_map[i] == 0 ? 0 : split_pos_seq[nodes_map[i] - 1];
			size_type r = nodes_map[i] == interior->get_node_n() - 1 ?
							  n :
							  split_pos_seq[nodes_map[i]];
			new_split_node_n_arr[nodes_map[i]] = batch_insert_recursive(
				interior->tree_nodes[nodes_map[i]], in.cut(l, r),
				node_split_slice.cut(2 * l, 2 * r), leaf_entries_pattern,
				leaf_sort_pred);
		},
		1);

	/*
	 * count only over touched children: start from get_node_n() (each
	 * contributes 1) and add the excess for children that split (return
	 * value is 0 or >=2, never 1).
	 */
	size_type new_node_n = interior->get_node_n();
	for (buf_size_type i = 0; i < cnt; i++) {
		size_type split_n = new_split_node_n_arr[nodes_map[i]];
		if (split_n > 0) {
			new_node_n += split_n - 1;
		}
	}

	/* the interior node can be updated without split */
	if (new_node_n <= TT::k_buf_ub) {
		[[maybe_unused]] node_type *new_inter_node =
			batch_insert_update_interior(node, node_split_slice, split_pos_seq,
										 new_node_n, new_split_node_n_arr);
		return 0;
	}

	/* handle interior node split overflow */
	return handling_interior_split_overflow(node, node_split_slice,
											split_pos_seq, new_node_n,
											new_split_node_n_arr);
}

} /* namespace pab_tree */

#endif /* PABTREE_B_TREE_IMPL_BATCH_OP_B_BATCH_INSERT_HPP_ */

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
template <typename Range>
void btree<TT>::batch_insert(Range &&In)
{
	static_assert(parlay::is_random_access_range_v<Range>);
	static_assert(
		std::is_constructible_v<parlay::range_value_type_t<Range>,
								parlay::range_reference_type_t<Range>>);

	auto A = parlay::make_slice(std::forward<Range>(In));

	constexpr auto leaf_entries_pattern = TT::leaf_entries_pattern::hybrid;
	constexpr bool sorted_input = false;
	BatchInsert_(A, leaf_entries_pattern, sorted_input, default_hybrid_pred{});
}

template <typename TT>
template <typename Range, typename LeafSortPred>
void btree<TT>::batch_insert(Range &&In,
							 const TT::pattern_type leaf_entries_pattern,
							 const bool sorted_input,
							 LeafSortPred const &leaf_sort_pred)
{
	static_assert(parlay::is_random_access_range_v<Range>);
	static_assert(
		std::is_constructible_v<parlay::range_value_type_t<Range>,
								parlay::range_reference_type_t<Range>>);

	auto A = parlay::make_slice(std::forward<Range>(In));

	BatchInsert_(A, leaf_entries_pattern, sorted_input, leaf_sort_pred);
}

template <typename TT>
template <typename Range, typename LeafSortPred>
	requires std::predicate<LeafSortPred, typename btree<TT>::Node *, Range>
void btree<TT>::BatchInsert_(Range In,
							 const TT::pattern_type leaf_entries_pattern,
							 const bool sorted_input,
							 LeafSortPred const &leaf_sort_pred)
{
	if (this->root_ == nullptr) {
		return Build_(In);
	}

	if (this->root_->size == 0) { // empty ghost node should be eliminated
		return Build_(In); // Build_ releases the old tree
	}

	// Accept any random-access range. A contiguous one is borrowed as is;
	// anything else is materialised, which is also what the sort does.
	typename TT::entry_seq sorted_seq;
	parlay::slice<const E *, const E *> sorted_slice{ nullptr, nullptr };
	if (!sorted_input) {
		sorted_seq = parlay::sort(In, key_less);
		sorted_slice =
			parlay::make_slice(sorted_seq.cbegin(), sorted_seq.cend());
	} else if constexpr (std::contiguous_iterator<decltype(std::begin(In))>) {
		sorted_slice = { std::to_address(std::begin(In)),
						 std::to_address(std::end(In)) };
	} else {
		sorted_seq = parlay::to_sequence(In);
		sorted_slice =
			parlay::make_slice(sorted_seq.cbegin(), sorted_seq.cend());
	}

	// Keys must be distinct, in the batch and against the tree. Only the
	// batch is checkable here.
	assert(parlay::is_sorted(sorted_slice, [](E const &a, E const &b) {
		return Entry::get_key(a) <= Entry::get_key(b);
	}));

	// begin insert
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
template <typename Range, typename LeafSortPred>
TT::size_type
btree<TT>::batch_insert_recursive(Node *T, Range In,
								  node_split_pair_slice node_split_slice,
								  const TT::pattern_type leaf_entries_pattern,
								  LeafSortPred const &leaf_sort_pred)
{
	assert(2 * In.size() == node_split_slice.size());

	size_type n = In.size();
	if (n == 0) {
		return 0;
	}

	if (T->is_leaf) {
		Leaf *TL = static_cast<Leaf *>(T);
		if (TL->size + n <= TT::k_buf_ub) {
			[[maybe_unused]] Node *updated_node = insert_points_2_leaf(
				T, In, leaf_entries_pattern, leaf_sort_pred);
			assert(updated_node == T);
			return 0;
		}
		return handling_leaf_split_overflow(T, In, node_split_slice);
	}

	auto TI = static_cast<Interior *>(T);

	size_buf_ub_arr split_pos_seq;
	assert(TI->get_node_n() > 0);
	if (In.size() > TT::k_split_factor * TI->get_split_n()) {
		split_batch_binary_search(split_pos_seq, In, TI->get_split_slice());
	} else {
		split_batch_merge(split_pos_seq, In, TI->get_split_slice());
	}

	//   // SplitBatchSieve(split_pos_seq, In, TI->get_split_slice());
	// } else {
#ifdef DEBUG
	for (int i = 1; i < TI->get_split_n(); i++) {
		assert(parlay::all_of(
			In.cut(split_pos_seq[i - 1], split_pos_seq[i]), [&](E const e) {
				return Entry::get_key(e) >= TI->split[i - 1] &&
					   Entry::get_key(e) < TI->split[i];
			}));
	}
#endif // DEBUG

	size_buf_ub_arr new_split_node_n_arr;
	buf_size_buf_ub_arr nodes_map;
	buf_size_type cnt = 0;
	for (buf_size_type i = 0; i < TI->get_node_n(); i++) {
		size_type l = (i == 0 ? 0 : split_pos_seq[i - 1]);
		size_type r = (i == TI->get_node_n() - 1 ? n : split_pos_seq[i]);
		if (l != r) {
			nodes_map[cnt++] = i;
		} else {
			new_split_node_n_arr[i] = 0; // i-th child remains the same
		}
	}

	parlay::parallel_for(
		0, cnt,
		[&](buf_size_type i) {
			size_type l =
				nodes_map[i] == 0 ? 0 : split_pos_seq[nodes_map[i] - 1];
			size_type r = nodes_map[i] == TI->get_node_n() - 1 ?
							  n :
							  split_pos_seq[nodes_map[i]];
			new_split_node_n_arr[nodes_map[i]] = batch_insert_recursive(
				TI->tree_nodes[nodes_map[i]], In.cut(l, r),
				node_split_slice.cut(2 * l, 2 * r), leaf_entries_pattern,
				leaf_sort_pred);
		},
		1);

	// count only over touched children: start from get_node_n() (each
	// contributes 1) and add the excess for children that split (return
	// value is 0 or >=2, never 1).
	size_type new_node_n = TI->get_node_n();
	for (buf_size_type i = 0; i < cnt; i++) {
		size_type split_n = new_split_node_n_arr[nodes_map[i]];
		if (split_n > 0) {
			new_node_n += split_n - 1;
		}
	}

	// the interior node can be updated without split
	if (new_node_n <= TT::k_buf_ub) {
		[[maybe_unused]] Node *new_inter_node =
			batch_insert_update_interior(T, node_split_slice, split_pos_seq,
										 new_node_n, new_split_node_n_arr);
		return 0;
	}

	// handle interior node split overflow
	return handling_interior_split_overflow(T, node_split_slice, split_pos_seq,
											new_node_n, new_split_node_n_arr);
}

} // namespace pab_tree

#endif // PABTREE_B_TREE_IMPL_BATCH_OP_B_BATCH_INSERT_HPP_

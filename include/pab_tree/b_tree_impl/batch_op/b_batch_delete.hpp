#ifndef PABTREE_B_TREE_IMPL_BATCH_OP_B_BATCH_DELETE_HPP_
#define PABTREE_B_TREE_IMPL_BATCH_OP_B_BATCH_DELETE_HPP_

#include <cassert>
#include <iterator>
#include <memory>

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
		this->rebuild_from_flatten();
		return;
	}

	if (invalid_subtree_num) {
		if (this->root_->size < TT::k_buf_ub &&
			!this->root_->is_leaf) { /* fits in one leaf directly use leaf */
			this->rebuild_from_flatten();
			return;
		}
		auto reinsert_entries =
			TT::entry_seq::uninitialized(reinsert_entry_num);

#ifdef DEBUG
		this->validate<false>();
		puts(">>> pass tree aug after delete");
#endif /* DEBUG */

		const bool has_tomb = true;
		fetch_invalid_subtree_nodes(
			this->root_, parlay::make_slice(reinsert_entries), has_tomb,
			leaf_entries_pattern, leaf_sort_pred);
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
	for (int i = 1; i < interior->get_split_n(); i++) {
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

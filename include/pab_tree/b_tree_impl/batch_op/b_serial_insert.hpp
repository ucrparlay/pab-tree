#ifndef PABTREE_B_TREE_IMPL_BATCH_OP_B_SERIAL_INSERT_HPP_
#define PABTREE_B_TREE_IMPL_BATCH_OP_B_SERIAL_INSERT_HPP_

#include <cassert>

#include <algorithm>
#include <array>
#include <iterator>
#include <memory>
#include <vector>

#include "pab_tree/b_tree.h"
#include "parlay/slice.h"

namespace pab_tree
{
template <typename TT>
template <typename R>
void btree<TT>::serial_insert(R &&in)
{
	static_assert(parlay::is_random_access_range_v<R>);
	static_assert(std::is_constructible_v<parlay::range_value_type_t<R>,
										  parlay::range_reference_type_t<R>>);

	auto in_slice = parlay::make_slice(std::forward<R>(in));

	constexpr auto leaf_entries_pattern = TT::leaf_entries_pattern::hybrid;
	serial_insert_(in_slice, leaf_entries_pattern, default_hybrid_pred{});
}

template <typename TT>
template <typename R, typename LeafSortPred>
void btree<TT>::serial_insert(R &&in,
							  const TT::pattern_type leaf_entries_pattern,
							  [[maybe_unused]] const bool sorted_input,
							  LeafSortPred const &leaf_sort_pred)
{
	static_assert(parlay::is_random_access_range_v<R>);
	static_assert(std::is_constructible_v<parlay::range_value_type_t<R>,
										  parlay::range_reference_type_t<R>>);

	auto in_slice = parlay::make_slice(std::forward<R>(in));

	serial_insert_(in_slice, leaf_entries_pattern, leaf_sort_pred);
}

template <typename TT>
template <typename R, typename LeafSortPred>
	requires std::predicate<LeafSortPred, typename btree<TT>::node_type *, R>
void btree<TT>::serial_insert_(R in,
							   const TT::pattern_type leaf_entries_pattern,
							   LeafSortPred const &leaf_sort_pred)
{
	/* An empty interior root is dropped, as batch_insert_ drops it. */
	if (this->root_ != nullptr && this->root_->size == 0 &&
		!this->root_->is_leaf) {
		this->delete_tree();
	}
	if (this->root_ == nullptr) {
		this->root_ = alloc_empty_leaf_node<leaf_type>();
	}

	/*
	 * The leaf predicate gets what batch_insert_ hands it, a slice of
	 * entries in memory. Any other range is copied first.
	 */
	std::vector<entry_type> copy;
	parlay::slice<const entry_type *, const entry_type *> entries{ nullptr,
																   nullptr };
	if constexpr (std::contiguous_iterator<decltype(std::begin(in))>) {
		entries = { std::to_address(std::begin(in)),
					std::to_address(std::end(in)) };
	} else {
		copy.assign(std::begin(in), std::end(in));
		entries = { copy.data(), copy.data() + copy.size() };
	}

	for (size_t i = 0; i < entries.size(); i++) {
		auto const [right, sep] =
			serial_insert_rec(this->root_, entries.cut(i, i + 1),
							  leaf_entries_pattern, leaf_sort_pred);
		if (right == nullptr) {
			continue;
		}

		/* The root split: a new root above the two halves. */
		std::array<node_type *, 2> nodes{ this->root_, right };
		std::array<key_type, 1> keys{ sep };
		auto *root = alloc_interior_node<interior_type, TT>(
			parlay::make_slice(nodes.data(), nodes.data() + nodes.size()),
			parlay::make_slice(keys.data(), keys.data() + keys.size()));
		root->update_aug();
		this->root_ = root;
	}
}

template <typename TT>
template <typename R, typename LeafSortPred>
typename btree<TT>::node_split_pair
btree<TT>::serial_insert_rec(node_type *node, R in,
							 const TT::pattern_type leaf_entries_pattern,
							 LeafSortPred const &leaf_sort_pred)
{
	assert(in.size() == 1);

	if (node->is_leaf) {
		if (node->size < TT::k_buf_ub) {
			serial_insert_leaf(node, in, leaf_entries_pattern, leaf_sort_pred);
			return { nullptr, key_type{} };
		}
		return serial_split_leaf(node, in[0]);
	}

	auto *interior = static_cast<interior_type *>(node);
	assert(interior->get_node_n() > 0);
	buf_size_type const i =
		interior->get_node_id_containing_key(entry_traits::get_key(in[0]));
	auto const right = serial_insert_rec(interior->tree_nodes[i], in,
										 leaf_entries_pattern, leaf_sort_pred);
	interior->size += 1;

	if (right.first == nullptr) {
		interior->update_aug();
		return { nullptr, key_type{} };
	}

	buf_size_type const n = interior->get_node_n();
	if (n == TT::k_buf_ub) {
		return serial_split_interior(node, i, right);
	}

	/* Child i split: its right half goes in after it. */
	auto &nodes = interior->tree_nodes;
	auto &keys = interior->split;
	std::move_backward(nodes.begin() + i + 1, nodes.begin() + n,
					   nodes.begin() + n + 1);
	nodes[i + 1] = right.first;
	std::move_backward(keys.begin() + i, keys.begin() + n - 1,
					   keys.begin() + n);
	keys[i] = right.second;
	interior->node_n = n + 1;
	interior->update_aug();
	return { nullptr, key_type{} };
}

template <typename TT>
template <typename R, typename LeafSortPred>
void btree<TT>::serial_insert_leaf(node_type *node, R in,
								   const TT::pattern_type leaf_entries_pattern,
								   LeafSortPred const &leaf_sort_pred)
{
	auto *leaf = static_cast<leaf_type *>(node);
	entry_type const e = in[0];
	key_type const k = entry_traits::get_key(e);
	assert(leaf->size < TT::k_buf_ub);
	/* Keys are distinct, against the tree as well. */
	assert(!leaf->contain(k));

	/* Append, then sort or not as post_process_leaf_change decides. */
	bool const was_sorted = leaf->is_sorted;
	leaf->elements[leaf->size] = e;
	leaf->size += 1;
	leaf->update_aug(parlay::make_slice(leaf->elements.begin(),
										leaf->elements.begin() + leaf->size));

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
		return;
	}
	if (!was_sorted) {
		std::sort(leaf->get_ele_begin_iter(), leaf->get_ele_end_iter(),
				  key_less);
		leaf->is_sorted = true;
		return;
	}

	/*
	 * Only e is out of place. It is last and tests false against its own
	 * key, so the search still sees a sorted run.
	 */
	auto const first = leaf->get_ele_begin_iter();
	auto const last = leaf->get_ele_end_iter() - 1;
	auto const pos = first + leaf->binary_search_key(k);
	std::move_backward(pos, last, last + 1);
	*pos = e;
}

template <typename TT>
typename btree<TT>::node_split_pair
btree<TT>::serial_split_leaf(node_type *node, entry_type const &e)
{
	auto *leaf = static_cast<leaf_type *>(node);
	key_type const k = entry_traits::get_key(e);
	assert(leaf->size == TT::k_buf_ub);
	assert(!leaf->contain(k));

	if (!leaf->is_sorted) {
		std::sort(leaf->get_ele_begin_iter(), leaf->get_ele_end_iter(),
				  key_less);
		leaf->is_sorted = true;
	}

	/*
	 * The leaf merged with e, cut where generate_leaf_nodes cuts it: the
	 * leaf keeps the first n / 2 entries, a new leaf takes the rest.
	 */
	size_type const n = leaf->size + 1;
	size_type const lo = n / 2;
	assert(get_bkt_num(n) == 2);
	size_type const pos = leaf->binary_search_key(k);

	auto *right = alloc_empty_leaf_node<leaf_type>();
	auto const src = leaf->get_ele_begin_iter();
	auto const dst = right->get_ele_begin_iter();
	if (pos >= lo) {
		auto out = std::copy(src + lo, src + pos, dst);
		*out++ = e;
		std::copy(src + pos, src + leaf->size, out);
	} else {
		std::copy(src + lo - 1, src + leaf->size, dst);
		std::move_backward(src + pos, src + lo - 1, src + lo);
		src[pos] = e;
	}

	leaf->size = lo;
	right->size = n - lo;
	leaf->update_aug(parlay::make_slice(leaf->elements.begin(),
										leaf->elements.begin() + leaf->size));
	right->update_aug(parlay::make_slice(
		right->elements.begin(), right->elements.begin() + right->size));
	return { right, entry_traits::get_key(right->elements[0]) };
}

template <typename TT>
typename btree<TT>::node_split_pair
btree<TT>::serial_split_interior(node_type *node, buf_size_type i,
								 node_split_pair const &right)
{
	auto *interior = static_cast<interior_type *>(node);
	assert(interior->get_node_n() == TT::k_buf_ub);

	/* The children with right.first after child i; separators to match. */
	constexpr size_type n = TT::k_buf_ub + 1;
	std::array<node_type *, n> nodes;
	std::array<key_type, n - 1> keys;
	auto const &old_nodes = interior->tree_nodes;
	auto const &old_keys = interior->split;
	std::copy(old_nodes.begin(), old_nodes.begin() + i + 1, nodes.begin());
	nodes[i + 1] = right.first;
	std::copy(old_nodes.begin() + i + 1, old_nodes.begin() + n - 1,
			  nodes.begin() + i + 2);
	std::copy(old_keys.begin(), old_keys.begin() + i, keys.begin());
	keys[i] = right.second;
	std::copy(old_keys.begin() + i, old_keys.begin() + n - 2,
			  keys.begin() + i + 1);

	/* Cut where generate_interior_nodes cuts them. */
	size_type const lo = n / 2;
	assert(get_bkt_num(n) == 2);
	auto *sibling = alloc_interior_node<interior_type, TT>(
		parlay::make_slice(nodes.data() + lo, nodes.data() + n),
		parlay::make_slice(keys.data() + lo, keys.data() + n - 1));
	sibling->update_aug();

	std::copy(nodes.begin(), nodes.begin() + lo, interior->tree_nodes.begin());
	std::copy(keys.begin(), keys.begin() + lo - 1, interior->split.begin());
	interior->node_n = lo;
	interior->size -= sibling->size;
	interior->update_aug();
	return { sibling, keys[lo - 1] };
}

} /* namespace pab_tree */

#endif /* PABTREE_B_TREE_IMPL_BATCH_OP_B_SERIAL_INSERT_HPP_ */

#ifndef PABTREE_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_
#define PABTREE_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_

#include <cassert>

#include <parlay/range.h>
#include <parlay/slice.h>
#include <parlay/type_traits.h>

#include <cstddef>

#include "pab_tree/b_tree.h"
#include "parlay/delayed_sequence.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"

namespace pab_tree
{
template <typename TT>
template <typename R>
void btree<TT>::build(R &&in)
{
	static_assert(parlay::is_random_access_range_v<R>);
	static_assert(std::is_constructible_v<parlay::range_value_type_t<R>,
										  parlay::range_reference_type_t<R>>);

	auto in_slice = parlay::make_slice(std::forward<R>(in));
	build_(in_slice);
}

template <typename TT>
template <typename R>
void btree<TT>::build_(R in)
{
	/* Release the old tree; a second build would otherwise leak it. */
	this->delete_tree();
	auto sort_seq = parlay::sort(in, key_less);
	/*
	 * Keys must be distinct. is_sorted with <= is strict increase, so this
	 * catches duplicates; with < it would tolerate them.
	 */
	assert(parlay::is_sorted(sort_seq,
							 [](entry_type const &a, entry_type const &b) {
								 return !TT::entry_less(b, a);
							 }));
	this->root_ = build_recursive(parlay::make_slice(sort_seq));
	assert(this->root_ != nullptr);
}

template <typename TT>
TT::node_type *btree<TT>::assemble_interior_levels(TT::node_slice nodes,
												   TT::split_slice keys)
{
	auto aux_node_seq = TT::node_seq::uninitialized(nodes.size());
	auto aux_key_seq = TT::key_seq::uninitialized(keys.size());
	auto node_in = nodes;
	auto node_out = parlay::make_slice(aux_node_seq);
	auto key_in = keys;
	auto key_out = parlay::make_slice(aux_key_seq);

	auto swap_key_node = [&](size_type bkt_n) {
		key_in = key_in.cut(0, bkt_n - 1);
		key_out = key_out.cut(0, bkt_n - 1);
		node_in = node_in.cut(0, bkt_n);
		node_out = node_out.cut(0, bkt_n);
		std::ranges::swap(key_in, key_out);
		std::ranges::swap(node_in, node_out);
	};

	while (node_in.size() >= TT::k_buf_ub) {
		size_type interior_bkt_n = generate_interior_nodes(
			node_in, key_in,
			gen_nodes_output_handler{
				.set_key = [&](size_type i, auto key) { key_out[i] = key; },
				.set_node = [&](size_type i,
								auto node) { node_out[i] = node; } });
		swap_key_node(interior_bkt_n);
		assert(parlay::is_sorted(key_in, [&](TT::key_type a, TT::key_type b) {
			return TT::key_less(a, b);
		}));
	}

	auto *node = alloc_interior_node<interior_type, TT>(node_in, key_in);
	node->update_aug();
	return node;
}

template <typename TT>
template <typename R>
TT::node_type *btree<TT>::build_recursive(R in)
{
	if (in.size() == 0) {
		return alloc_empty_leaf_node<leaf_type>();
	}

	if (in.size() < TT::k_buf_ub) {
		return alloc_normal_leaf_node<R, leaf_type>(in);
	}

	size_type estimated_bkt_n = (in.size() / TT::k_buf_md) + 1;
	auto node_seq =
		parlay::sequence<node_type *>::uninitialized(estimated_bkt_n);
	auto key_seq =
		parlay::sequence<typename TT::key_type>::uninitialized(estimated_bkt_n);
	auto node_out = parlay::make_slice(node_seq);
	auto key_out = parlay::make_slice(key_seq);

	size_type bkt_n = generate_leaf_nodes(
		in,
		gen_nodes_output_handler{
			.set_key = [&](size_type i, auto key) { key_out[i] = key; },
			.set_node = [&](size_type i, auto node) { node_out[i] = node; } });
	assert(parlay::is_sorted(
		key_out.cut(0, bkt_n - 1),
		[&](TT::key_type a, TT::key_type b) { return TT::key_less(a, b); }));

	this->root_ = assemble_interior_levels(node_out.cut(0, bkt_n),
										   key_out.cut(0, bkt_n - 1));
	return this->root_;
}

} /* namespace pab_tree */

#endif /* PABTREE_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_ */

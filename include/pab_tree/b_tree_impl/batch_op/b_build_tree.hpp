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
template <typename Range>
void btree<TT>::build(Range &&In)
{
	static_assert(parlay::is_random_access_range_v<Range>);
	static_assert(
		std::is_constructible_v<parlay::range_value_type_t<Range>,
								parlay::range_reference_type_t<Range>>);

	auto A = parlay::make_slice(std::forward<Range>(In));
	Build_(A);
}

template <typename TT>
template <typename Range>
void btree<TT>::Build_(Range In)
{
	// Release the old tree; a second build would otherwise leak it.
	this->delete_tree();
	auto sort_seq = parlay::sort(In, key_less);
	// Keys must be distinct. is_sorted with <= is strict increase, so this
	// catches duplicates; with < it would tolerate them.
	assert(parlay::is_sorted(sort_seq, [](E const &a, E const &b) {
		return Entry::get_key(a) <= Entry::get_key(b);
	}));
	this->root_ = build_recursive(parlay::make_slice(sort_seq));
	assert(this->root_ != nullptr);
}

template <typename TT>
TT::Node *btree<TT>::assemble_interior_levels(TT::node_slice nodes,
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
		assert(
			parlay::is_sorted(key_in, [&](TT::K a, TT::K b) { return a < b; }));
	}

	auto *node = alloc_interior_node<Interior, TT>(node_in, key_in);
	node->update_aug();
	return node;
}

template <typename TT>
template <typename Range>
TT::Node *btree<TT>::build_recursive(Range In)
{
	if (In.size() == 0) {
		return alloc_empty_leaf_node<Range, Leaf>();
	}

	if (In.size() < TT::k_buf_ub) {
		return alloc_normal_leaf_node<Range, Leaf>(In);
	}

	size_type estimated_bkt_n = (In.size() / TT::k_buf_md) + 1;
	auto node_seq = parlay::sequence<Node *>::uninitialized(estimated_bkt_n);
	auto key_seq =
		parlay::sequence<typename TT::K>::uninitialized(estimated_bkt_n);
	auto node_out = parlay::make_slice(node_seq);
	auto key_out = parlay::make_slice(key_seq);

	size_type bkt_n = generate_leaf_nodes(
		In,
		gen_nodes_output_handler{
			.set_key = [&](size_type i, auto key) { key_out[i] = key; },
			.set_node = [&](size_type i, auto node) { node_out[i] = node; } });
	assert(parlay::is_sorted(key_out.cut(0, bkt_n - 1),
							 [&](TT::K a, TT::K b) { return a < b; }));

	this->root_ = assemble_interior_levels(node_out.cut(0, bkt_n),
										   key_out.cut(0, bkt_n - 1));
	return this->root_;
}

} // namespace pab_tree

#endif // PABTREE_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_

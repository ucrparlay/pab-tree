#ifndef PABTREE_B_TREE_IMPL_TREE_OP_FLATTEN_HPP_
#define PABTREE_B_TREE_IMPL_TREE_OP_FLATTEN_HPP_

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <numeric>
#include <tuple>
#include <type_traits>
#include <utility>

#include "pab_tree/b_tree.h"

namespace pab_tree
{

template <typename TT>
template <bool granularity>
inline bool btree<TT>::force_parallel_recursion(Interior const *TI)
{
	// NOTE: return true -> force parallel
	// return false -> serial
	return (granularity && TI->size > k_serial_build_cutoff) ||
	       (!granularity && TI->force_parallel());
}

template <typename TT>
template <typename Range, bool granularity, bool sorted>
void btree<TT>::flatten_rec(Node *T, Range Out)
{
	assert(T->size == Out.size());

	if (T->size == 0) {
		return;
	}

	if (T->is_leaf) {
		extract_points_in_leaf<Range, sorted>(T, Out);
		return;
	}

	auto TI = static_cast<Interior *>(T);

	assert(TI->size ==
	       std::accumulate(TI->tree_nodes.begin(),
			       TI->tree_nodes.begin() + TI->get_node_n(),
			       static_cast<size_t>(0),
			       [](size_t acc, Node *n) -> size_t {
				       return acc + n->size;
			       }));

	std::array<size_type, TT::k_buf_ub + 1> child_offsets;
	child_offsets[0] = 0;
	auto node_n = TI->get_node_n();
	for (buf_size_type i = 0; i < node_n; ++i) {
		child_offsets[i + 1] =
			child_offsets[i] + TI->tree_nodes[i]->size;
	}

	parlay::parallel_for(
		0, node_n,
		[&](buf_size_type i) {
			auto start = child_offsets[i];
			flatten_rec<Range, granularity, sorted>(
				TI->tree_nodes[i],
				Out.cut(start, child_offsets[i + 1]));
		},
		// NOTE: In our btree when we flatten the tree, the size is always match the
		// corresponding size, so we can use it as the granularity indicator
		force_parallel_recursion<granularity>(TI) ?
			1 :
			Interior::TT::k_buf_ub);
}

} // namespace pab_tree

#endif // PABTREE_B_TREE_IMPL_TREE_OP_FLATTEN_HPP_

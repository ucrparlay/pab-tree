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
template <bool Granularity>
inline bool btree<TT>::force_parallel_recursion(interior_type const *interior)
{
	/*
	 * return true -> force parallel
	 * return false -> serial
	 */
	return (Granularity && interior->size > k_serial_build_cutoff) ||
		   (!Granularity && interior->force_parallel());
}

template <typename TT>
template <typename R, bool Granularity, bool Sorted>
void btree<TT>::flatten_rec(node_type *node, R out)
{
	assert(node->size == out.size());

	if (node->size == 0) {
		return;
	}

	if (node->is_leaf) {
		extract_points_in_leaf<R, Sorted>(node, out);
		return;
	}

	auto interior = static_cast<interior_type *>(node);

	assert(interior->size ==
		   std::accumulate(
			   interior->tree_nodes.begin(),
			   interior->tree_nodes.begin() + interior->get_node_n(),
			   static_cast<size_t>(0), [](size_t acc, node_type *n) -> size_t {
				   return acc + n->size;
			   }));

	std::array<size_type, TT::k_buf_ub + 1> child_offsets;
	child_offsets[0] = 0;
	auto node_n = interior->get_node_n();
	for (buf_size_type i = 0; i < node_n; ++i) {
		child_offsets[i + 1] = child_offsets[i] + interior->tree_nodes[i]->size;
	}

	parlay::parallel_for(
		0, node_n,
		[&](buf_size_type i) {
			auto start = child_offsets[i];
			flatten_rec<R, Granularity, Sorted>(
				interior->tree_nodes[i], out.cut(start, child_offsets[i + 1]));
		},
		/*
		 * In our btree when we flatten the tree, the size is
		 * always match the corresponding size, so we can use it as the
		 * granularity indicator
		 */
		force_parallel_recursion<Granularity>(interior) ?
			1 :
			interior_type::type_trait::k_buf_ub);
}

} /* namespace pab_tree */

#endif /* PABTREE_B_TREE_IMPL_TREE_OP_FLATTEN_HPP_ */

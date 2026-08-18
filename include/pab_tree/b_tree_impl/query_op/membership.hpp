#ifndef PABTREE_B_TREE_IMPL_QUERY_OP_MEMBERSHIP_HPP_
#define PABTREE_B_TREE_IMPL_QUERY_OP_MEMBERSHIP_HPP_

#include <cassert>

#include "parlay/slice.h"
#include "pab_tree/b_tree.h"

namespace pab_tree
{

template <typename TT>
auto btree<TT>::contain(key_type const &k) const -> bool
{
	if (this->root_ == nullptr) [[unlikely]] {
		return false;
	}
	return contain_iter(this->root_, k);
}

/*
 * Iterative tree descent: avoids O(height) recursive call overhead.
 * At each interior level we do a branchless binary search on the split array
 * (via get_node_id_containing_key) and follow one child pointer.
 */
template <typename TT>
bool btree<TT>::contain_iter(node_type const *node, key_type const &k) const
{
	while (!node->is_leaf) {
		auto const *interior = static_cast<interior_type const *>(node);
		auto const pos = interior->get_node_id_containing_key(k);
		assert(pos < interior->get_node_n());
		assert(interior->validate_key_in_node(pos, k));
		node = interior->tree_nodes[pos];
	}
	return static_cast<leaf_type const *>(node)->contain(k);
}

} /* namespace pab_tree */

#endif /* PABTREE_B_TREE_IMPL_QUERY_OP_MEMBERSHIP_HPP_ */

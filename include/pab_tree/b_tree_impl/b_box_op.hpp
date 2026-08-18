#ifndef PABTREE_B_TREE_IMPL_B_BOX_OP_HPP_
#define PABTREE_B_TREE_IMPL_B_BOX_OP_HPP_

#include "pab_tree/b_tree.h"

namespace pab_tree
{

/*
 * template<typename TT>
 * TT::box_type
 *       return typename TT::box_type(
 *               entry_traits::get_key(*leaf->get_ele_begin_iter()),
 */

template <typename TT>
TT::box_type btree<TT>::get_node_box_rec(node_type const *node)
{
	if (node->is_leaf) {
		leaf_type const *leaf = static_cast<leaf_type const *>(node);
		return pab_tree::get_box<TT>(leaf->get_ele());
	} else {
		auto interior = static_cast<interior_type const *>(node);
		parlay::sequence<typename TT::box_type> box_seq(interior->get_node_n());
		parlay::parallel_for(0, interior->get_node_n(), [&](size_t i) {
			box_seq[i] = get_node_box_rec(interior->tree_nodes[i]);
		});
		return pab_tree::get_box<TT>(box_seq);
	}
}

template <typename TT>
TT::box_type
btree<TT>::get_node_slice_box(typename TT::node_slice const node_slice)
{
	return get_node_slice_box<TT>(parlay::make_slice(node_slice));
}

/*
 * template<typename TT>
 * TT::box_type
 */
} /* namespace pab_tree */

#endif /* PABTREE_B_TREE_IMPL_B_BOX_OP_HPP_ */

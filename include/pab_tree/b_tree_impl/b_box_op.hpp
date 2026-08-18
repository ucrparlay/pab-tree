#ifndef PABTREE_B_TREE_IMPL_B_BOX_OP_HPP_
#define PABTREE_B_TREE_IMPL_B_BOX_OP_HPP_

#include "pab_tree/b_tree.h"

namespace pab_tree
{

// template<typename type_trait>
// type_trait::box_type
//       return typename type_trait::box_type(
//               entry_traits::get_key(*TL->get_ele_begin_iter()),

template <typename type_trait>
type_trait::box_type btree<type_trait>::get_node_box_rec(node_type const *T)
{
	if (T->is_leaf) {
		leaf_type const *TL = static_cast<leaf_type const *>(T);
		return pab_tree::get_box<type_trait>(TL->get_ele());
	} else {
		auto TI = static_cast<interior_type const *>(T);
		parlay::sequence<typename type_trait::box_type> box_seq(
			TI->get_node_n());
		parlay::parallel_for(0, TI->get_node_n(), [&](size_t i) {
			box_seq[i] = get_node_box_rec(TI->tree_nodes[i]);
		});
		return pab_tree::get_box<type_trait>(box_seq);
	}
}

template <typename type_trait>
type_trait::box_type btree<type_trait>::get_node_slice_box(
	typename type_trait::node_slice const node_slice)
{
	return get_node_slice_box<type_trait>(parlay::make_slice(node_slice));
}

// template<typename type_trait>
// type_trait::box_type
} // namespace pab_tree

#endif // PABTREE_B_TREE_IMPL_B_BOX_OP_HPP_

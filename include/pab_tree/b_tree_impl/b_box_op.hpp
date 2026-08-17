#ifndef PABTREE_B_TREE_IMPL_B_BOX_OP_HPP_
#define PABTREE_B_TREE_IMPL_B_BOX_OP_HPP_

#include "pab_tree/b_tree.h"

namespace pab_tree
{

// template<typename TT>
// TT::Box
//       return typename TT::Box(Entry::get_key(*TL->get_ele_begin_iter()),

template <typename TT> TT::Box btree<TT>::get_node_box_rec(Node const *T)
{
	if (T->is_leaf) {
		Leaf const *TL = static_cast<Leaf const *>(T);
		return pab_tree::get_box<TT>(TL->get_ele());
	} else {
		auto TI = static_cast<Interior const *>(T);
		parlay::sequence<typename TT::Box> box_seq(TI->get_node_n());
		parlay::parallel_for(0, TI->get_node_n(), [&](size_t i) {
			box_seq[i] = get_node_box_rec(TI->tree_nodes[i]);
		});
		return pab_tree::get_box<TT>(box_seq);
	}
}

template <typename TT>
TT::Box btree<TT>::get_node_slice_box(typename TT::node_slice const node_slice)
{
	return get_node_slice_box<TT>(parlay::make_slice(node_slice));
}

// template<typename TT>
// TT::Box
} // namespace pab_tree

#endif // PABTREE_B_TREE_IMPL_B_BOX_OP_HPP_

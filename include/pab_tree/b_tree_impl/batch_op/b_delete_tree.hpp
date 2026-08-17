#ifndef PABTREE_B_TREE_IMPL_BATCH_OP_B_DELETE_TREE_HPP_
#define PABTREE_B_TREE_IMPL_BATCH_OP_B_DELETE_TREE_HPP_

#include <parlay/range.h>
#include <parlay/slice.h>
#include <parlay/type_traits.h>

#include "pab_tree/b_tree.h"

namespace pab_tree
{
template <typename TT> void btree<TT>::delete_tree()
{
	if (this->root_ == nullptr) {
		return;
	}
	this->delete_tree_rec(this->root_);
	this->root_ = nullptr;
}

template <typename TT>
template <bool granularity>
void btree<TT>::delete_tree_rec(Node *T)
{
	if (T == nullptr) {
		return;
	}
	if (T->is_leaf) {
		free_node<Leaf>(T);
	} else {
		auto TI = static_cast<Interior *>(T);

		// enable granularity control by default, if it is
		// disabled, always delete in parallel
		parlay::parallel_for(
			0, TI->get_node_n(),
			[&](size_t i) { delete_tree_rec(TI->tree_nodes[i]); },
			// WARN: the granularity control may not work well here
			force_parallel_recursion<granularity>(TI) ?
				1 :
				Interior::TT::k_buf_ub);

		free_node<Interior>(T);
	}
}
} // namespace pab_tree

#endif // PABTREE_B_TREE_IMPL_BATCH_OP_B_DELETE_TREE_HPP_

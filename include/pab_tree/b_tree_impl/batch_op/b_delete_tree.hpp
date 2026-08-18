#ifndef PABTREE_B_TREE_IMPL_BATCH_OP_B_DELETE_TREE_HPP_
#define PABTREE_B_TREE_IMPL_BATCH_OP_B_DELETE_TREE_HPP_

#include <parlay/range.h>
#include <parlay/slice.h>
#include <parlay/type_traits.h>

#include "pab_tree/b_tree.h"

namespace pab_tree
{
template <typename type_trait> void btree<type_trait>::delete_tree()
{
	if (this->root_ == nullptr) {
		return;
	}
	this->delete_tree_rec(this->root_);
	this->root_ = nullptr;
}

template <typename type_trait>
template <bool granularity>
void btree<type_trait>::delete_tree_rec(node_type *T)
{
	if (T == nullptr) {
		return;
	}
	if (T->is_leaf) {
		free_node<leaf_type>(T);
	} else {
		auto TI = static_cast<interior_type *>(T);

		// enable granularity control by default, if it is
		// disabled, always delete in parallel
		parlay::parallel_for(
			0, TI->get_node_n(),
			[&](size_t i) { delete_tree_rec(TI->tree_nodes[i]); },
			// WARN: the granularity control may not work well here
			force_parallel_recursion<granularity>(TI) ?
				1 :
				interior_type::type_trait::k_buf_ub);

		free_node<interior_type>(T);
	}
}
} // namespace pab_tree

#endif // PABTREE_B_TREE_IMPL_BATCH_OP_B_DELETE_TREE_HPP_

#ifndef PMOS_B_TREE_IMPL_BATCH_OP_B_DELETE_TREE_HPP_
#define PMOS_B_TREE_IMPL_BATCH_OP_B_DELETE_TREE_HPP_

#include <parlay/range.h>
#include <parlay/slice.h>
#include <parlay/type_traits.h>

#include "../../b_tree.h"

namespace pmos {
template<typename TT>
void
BTree<TT>::DeleteTree()
{
  if (this->root_ == nullptr) {
    return;
  }
  this->DeleteTreeRecursive(this->root_);
  this->root_ = nullptr;
  return;
}

template<typename TT>
template<bool granularity>
void
BTree<TT>::DeleteTreeRecursive(Node* T)
{
  if (T == nullptr) {
    return;
  }
  if (T->is_leaf) {
    FreeNode<Leaf>(T);
  } else {
    Interior* TI = static_cast<Interior*>(T);

    // NOTE: enable granularity control by default, if it is disabled,
    // always delete in parallel
    parlay::parallel_for(
      0,
      TI->GetNodeN(),
      [&](size_t i) { DeleteTreeRecursive(TI->tree_nodes[i]); },
      // WARN: the granularity control may not work well here
      ForceParallelRecursion<granularity>(TI) ? 1 : Interior::TT::kBuf);

    FreeNode<Interior>(T);
  }
}
} // namespace pmos

#endif // PMOS_B_TREE_IMPL_BATCH_OP_B_DELETE_TREE_HPP_

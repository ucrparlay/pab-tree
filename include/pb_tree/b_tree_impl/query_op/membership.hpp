#ifndef PBTREE_B_TREE_IMPL_QUERY_OP_MEMBERSHIP_HPP_
#define PBTREE_B_TREE_IMPL_QUERY_OP_MEMBERSHIP_HPP_

#include <cassert>

#include "parlay/slice.h"
#include "pb_tree/b_tree.h"

namespace pb_tree {

template<typename TT>
auto
BTree<TT>::Contain(K const& k) const -> bool
{
  if (this->root_ == nullptr) [[unlikely]] {
    return false;
  }
  return ContainIter(this->root_, k);
}

// Iterative tree descent: avoids O(height) recursive call overhead.
// At each interior level we do a branchless binary search on the split array
// (via GetNodeIdContainingKey) and follow one child pointer.
template<typename TT>
bool
BTree<TT>::ContainIter(Node const* T, K const& k) const
{
  while (!T->is_leaf) {
    auto const* TI = static_cast<Interior const*>(T);
    auto const pos = TI->GetNodeIdContainingKey(k);
    assert(pos < TI->GetNodeN());
    assert(TI->ValidateKeyInNode(pos, k));
    T = TI->tree_nodes[pos];
  }
  return static_cast<Leaf const*>(T)->Contain(k);
}


} // namespace pb_tree

#endif // PBTREE_B_TREE_IMPL_QUERY_OP_MEMBERSHIP_HPP_

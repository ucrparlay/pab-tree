#ifndef PABTREE_B_TREE_IMPL_QUERY_OP_MEMBERSHIP_HPP_
#define PABTREE_B_TREE_IMPL_QUERY_OP_MEMBERSHIP_HPP_

#include <cassert>

#include "parlay/slice.h"
#include "pab_tree/b_tree.h"

namespace pab_tree {

template<typename TT>
auto
btree<TT>::contain(K const& k) const -> bool
{
  if (this->root_ == nullptr) [[unlikely]] {
    return false;
  }
  return contain_iter(this->root_, k);
}

// Iterative tree descent: avoids O(height) recursive call overhead.
// At each interior level we do a branchless binary search on the split array
// (via get_node_id_containing_key) and follow one child pointer.
template<typename TT>
bool
btree<TT>::contain_iter(Node const* T, K const& k) const
{
  while (!T->is_leaf) {
    auto const* TI = static_cast<Interior const*>(T);
    auto const pos = TI->get_node_id_containing_key(k);
    assert(pos < TI->get_node_n());
    assert(TI->validate_key_in_node(pos, k));
    T = TI->tree_nodes[pos];
  }
  return static_cast<Leaf const*>(T)->contain(k);
}


} // namespace pab_tree

#endif // PABTREE_B_TREE_IMPL_QUERY_OP_MEMBERSHIP_HPP_

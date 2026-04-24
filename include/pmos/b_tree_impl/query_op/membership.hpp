#ifndef PMOS_B_TREE_IMPL_QUERY_OP_MEMBERSHIP_HPP_
#define PMOS_B_TREE_IMPL_QUERY_OP_MEMBERSHIP_HPP_

#include "parlay/slice.h"
#include "pmos/b_tree.h"

namespace pmos {

template<typename TT>
auto
BTree<TT>::Contain(K const& k) const -> std::pair<bool, MembershipLogger>
{
  MembershipLogger logger;
  return { ContainIter(this->root_, k), logger };
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

template<typename TT>
bool
BTree<TT>::ContainRec(Node const* T, K const& k) const
{
  if (T->is_leaf) {
    return static_cast<Leaf const*>(T)->Contain(k);
  }

  auto const* TI = static_cast<Interior const*>(T);
  auto pos = TI->GetNodeIdContainingKey(k);

  assert(pos < TI->GetNodeN());
  assert(TI->ValidateKeyInNode(pos, k));

  return ContainRec(TI->tree_nodes[pos], k);
}

} // namespace pmos

#endif // PMOS_B_TREE_IMPL_QUERY_OP_MEMBERSHIP_HPP_

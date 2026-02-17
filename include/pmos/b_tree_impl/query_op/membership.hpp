#ifndef PMOS_B_TREE_IMPL_QUERY_OP_MEMBERSHIP_HPP_
#define PMOS_B_TREE_IMPL_QUERY_OP_MEMBERSHIP_HPP_

#include "parlay/slice.h"
#include "pmos/b_tree.h"

namespace pmos {
template<typename TT>
auto
BTree<TT>::Contain(K const k) const -> std::pair<bool, MembershipLogger>
{
  MembershipLogger logger;
  return { ContainRec(this->root_, k), logger };
}

template<typename TT>
bool
BTree<TT>::ContainRec(Node const* T, K const k) const
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

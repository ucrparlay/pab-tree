#ifndef PMOS_B_TREE_IMPL_QUERY_OP_MEMBERSHIP_HPP_
#define PMOS_B_TREE_IMPL_QUERY_OP_MEMBERSHIP_HPP_

#include "b_tree.h"
#include "parlay/slice.h"

namespace pmos {
template <typename TT>
std::pair<bool, MembershipLogger> BTree<TT>::Contain(K const k) const {
  MembershipLogger logger;
  return {ContainRec(this->root_, k), logger};
}

template <typename TT>
bool BTree<TT>::ContainRec(Node const* T, K const k) const {
  if (T->is_leaf) {
    return static_cast<Leaf const*>(T)->Contain(k);
  }

  Interior const* TI = static_cast<Interior const*>(T);
  auto pos = parlay::internal::binary_search(
      parlay::make_slice(TI->split.begin(),
                         TI->split.begin() + TI->GetSplitN()),
      [&](K const key) { return k >= key; });

  assert(pos < TI->GetNodeN());
  assert(pos == 0 ? k < TI->split.front()
                  : (pos == TI->GetSplitN()
                         ? k >= TI->split[TI->GetNodeN() - 2]
                         : (k >= TI->split[pos - 1] && k < TI->split[pos])));
  return ContainRec(TI->tree_nodes[pos], k);
}

}  // namespace pmos

#endif  // PMOS_B_TREE_IMPL_QUERY_OP_MEMBERSHIP_HPP_

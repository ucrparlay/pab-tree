#ifndef PMOS_B_TREE_IMPL_B_BOX_OP_HPP_
#define PMOS_B_TREE_IMPL_B_BOX_OP_HPP_

#include "../b_tree.h"

namespace pmos {

template<typename TT>
TT::Box
BTree<TT>::GetNodeBox(Node const* T)
{
  if (T->is_leaf) {
    auto TL = static_cast<Leaf const*>(T);
    if (TL->is_sorted) {
      return typename TT::Box(Entry::GetKey(*TL->GetEleBeginIter()),
                              Entry::GetKey(*TL->GetEleRbeginIter()));
    }
    return pmos::GetBox<TT>(TL->GetEle()); // warning pass element
  }

  return static_cast<Interior const*>(T)->box;
}

template<typename TT>
TT::Box
BTree<TT>::GetNodeBoxRec(Node const* T)
{
  if (T->is_leaf) {
    Leaf const* TL = static_cast<Leaf const*>(T);
    return pmos::GetBox<TT>(TL->GetEle());
  } else {
    auto TI = static_cast<Interior const*>(T);
    parlay::sequence<typename TT::Box> box_seq(TI->GetNodeN());
    parlay::parallel_for(0, TI->GetNodeN(), [&](size_t i) {
      box_seq[i] = GetNodeBoxRec(TI->tree_nodes[i]);
    });
    return pmos::GetBox<TT>(box_seq);
  }
}

template<typename TT>
TT::Box
BTree<TT>::GetNodeSliceBox(typename TT::NodeSlice const node_slice)
{
  return GetNodeSliceBox<TT>(parlay::make_slice(node_slice));
}

template<typename TT>
TT::Box
BTree<TT>::GetNodeSliceBox(typename TT::ConstNodeSlice node_slice)
{
  auto box = GetEmptyBox<TT>();
  for (auto node : node_slice) {
    box = pmos::GetBox<TT>(box, GetNodeBox(node));
  }
  return box;
}
} // namespace pmos

#endif // PMOS_B_TREE_IMPL_B_BOX_OP_HPP_

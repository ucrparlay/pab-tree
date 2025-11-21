#ifndef PMOS_B_TREE_IMPL_BOX_OP_HPP_
#define PMOS_B_TREE_IMPL_BOX_OP_HPP_

#include "b_tree.h"
#include "pmos/dependence/concepts.h"

namespace pmos {
template <typename TT>
TT::Box BTree<TT>::GetBox(TT::Box const& a, TT::Box const& b) {
  return typename TT::Box(TT::Num::Min(a.first, b.first),
                          TT::Num::Max(a.second, b.second));
}

template <typename TT>
TT::Box BTree<TT>::GetEmptyBox() {
  return typename TT::Box(std::numeric_limits<typename TT::K>::max(),
                          std::numeric_limits<typename TT::K>::lowest());
}

template <typename TT>
TT::Box BTree<TT>::GetBox(TT::EleSeq const& ele_seq) {
  auto box = std::make_pair(TT::GetKey(ele_seq[0]), TT::GetKey(ele_seq[0]));
  parlay::parallel_for(1, ele_seq.size(), [&](size_t i) {
    auto key = TT::GetKey(ele_seq[i]);
    box.first = TT::Num::Min(box.first, key);
    box.second = TT::Num::Max(box.second, key);
  });
  return box;
}

template <typename TT>
TT::Box BTree<TT>::GetBox(TT::BoxSeq const& box_seq) {
  auto box = box_seq[0];
  parlay::parallel_for(1, box_seq.size(), [&](size_t i) {
    box.first = TT::Num::Min(box.first, box_seq[i].first);
    box.second = TT::Num::Max(box.second, box_seq[i].second);
  });
  return box;
}
}  // namespace pmos

#endif  // PMOS_B_TREE_IMPL_BOX_OP_HPP_

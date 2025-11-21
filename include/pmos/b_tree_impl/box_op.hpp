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

// TODO: this can be parallelized (needs to define the min/max of a box)
template <typename TT>
TT::Box BTree<TT>::GetBox(TT::EntrySeq const& ele_seq) {
  auto box =
      std::make_pair(Entry::GetKey(ele_seq[0]), Entry::GetKey(ele_seq[0]));
  for (int i = 1; i < ele_seq.size(); i++) {
    auto key = Entry::GetKey(ele_seq[i]);
    box.first = TT::Num::Min(box.first, key);
    box.second = TT::Num::Max(box.second, key);
  }
  return box;
}

template <typename TT>
TT::Box BTree<TT>::GetBox(TT::BoxSeq const& box_seq) {
  auto box = box_seq[0];
  for (int i = 1; i < box_seq.size(); i++) {
    box.first = TT::Num::Min(box.first, box_seq[i].first);
    box.second = TT::Num::Max(box.second, box_seq[i].second);
  }
  return box;
}
}  // namespace pmos

#endif  // PMOS_B_TREE_IMPL_BOX_OP_HPP_

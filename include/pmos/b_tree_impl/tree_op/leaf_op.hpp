#ifndef PMOS_B_TREE_IMPL_LEAF_OP_HPP_
#define PMOS_B_TREE_IMPL_LEAF_OP_HPP_

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <numeric>
#include <tuple>
#include <type_traits>
#include <utility>

#include "pmos/b_tree.h"
#include "pmos/dependence/concepts.h"

namespace pmos {

template<typename TT>
TT::Node*
BTree<TT>::InsertPoints2Leaf(Node* T, EntrySlice In)
{
  Leaf* TL = static_cast<Leaf*>(T);

  assert(T->size + In.size() <= TT::kBufUB);
  assert(T->size == TL->GetEle().size());
  if (TL->elements.size() == 0) {
    assert(TL->size == 0);
    TL->InitializeEleSeq();
    assert(TL->elements.size() == TT::kBufUB);
  }
  // PERF: BufSizeT should be enough since total size fits within
  for (BufSizeT i = 0; i < In.size(); i++) {
    TL->elements[TL->size + i] = In[i];
  }
  TL->size += In.size();

  TL->UpdateAug(TL->elements.cut(0, TL->size));
  return T;
}

template<typename TT>
TT::SizeT
BTree<TT>::HandlingLeafSplitOverflow(Node* T,
                                     EntrySlice In,
                                     NodeSplitPairSlice node_split_slice)
{
  auto entry_seq = TT::EntrySeq::uninitialized(T->size + In.size());
  Leaf* TL = static_cast<Leaf*>(T);

  for (SizeT i = 0; i < T->size; i++) { // Assume the TT::kBufUB is constant
    entry_seq[i] = TL->elements[i];
  }
  parlay::parallel_for(
    0, In.size(), [&](SizeT i) { entry_seq[T->size + i] = In[i]; });
  FreeNode<Leaf>(T);

  // NOTE: split
  entry_seq = parlay::sort(entry_seq); // PERF: we need sort as In is unsorted

  SizeT rt_bkt_n = GenerateLeafNodes(
    parlay::make_slice(entry_seq),
    GenNodesOutputHandler{
      .set_key = [&](SizeT i, auto key) { node_split_slice[i].second = key; },
      .set_node = [&](SizeT i,
                      auto node) { node_split_slice[i].first = node; } });

  return rt_bkt_n;
}

template<typename TT>
TT::SizeT
BTree<TT>::DeletePoints4Leaf(Node* T, EntrySlice In)
{
  assert(T->size >= In.size());
  Leaf* TL = static_cast<Leaf*>(T);

  auto it = TL->elements.begin();
  auto end = TL->elements.begin() + TL->size;
  BufSizeT delete_cnt = 0;
  for (size_t i = 0; i < In.size(); i++) {
    it = std::ranges::find(TL->elements.begin(), end, In[i]);
    if (it != end) {
      std::ranges::iter_swap(it, --end);
      delete_cnt++;
    }
  }

  assert(
    std::cmp_equal(std::distance(TL->pts.begin(), end), TL->size - In.size()));
  // TL->size -= In.size();
  TL->size = std::distance(TL->pts.begin(), end);
  assert(TL->size >= 0);
  TL->UpdateAug(TL->elements.cut(0, TL->size));
  // assert(TL->GetBox() == GetBox(TL->pts.cut(0, TL->size)));
  return delete_cnt;
}
//
// // NOTE: diff points from the leaf using std::set_difference
// // {1, 2, 5, 5, 5, 9} ∖ {2, 5, 7} == {1, 5, 5, 9}
// template <typename Point, typename DerivedTree, uint_fast8_t kSkHeight,
//           uint_fast8_t kImbaRatio>
// template <typename Leaf, typename ReturnType>
// ReturnType BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::DiffPoints4Leaf(
//     Node* T, Slice In) {
//   Leaf* TL = static_cast<Leaf*>(T);
//
//   if (TL->is_dummy) {
//     size_t cnt = parlay::count(In, TL->pts[0]);
//     TL->size = TL->size >= cnt ? TL->size - cnt : 0;
//     assert(TL->size >= 0);
//     if (TL->size == 0) {  // set points to normal leaf when all points in
//     dummy
//                           // node has been deleted
//       TL->is_dummy = false;
//       TL->pts = Points::uninitialized(kLeaveWrap);
//       TL->ResetAug();
//     }
//
//     if constexpr (std::same_as<ReturnType, Node*>) {
//       return T;
//     } else if constexpr (std::same_as<ReturnType, NodeBox>) {
//       if constexpr (HasBox<typename Leaf::AT>) {
//         return NodeBox(T, TL->GetBox());
//       } else {
//         return NodeBox(T,
//                        T->size ? Box(TL->pts[0], TL->pts[0]) :
//                        GetEmptyBox());
//       }
//     } else {
//       ;
//     }
//   }
//
//   // NOTE: for normal leaf, need to check whether all Points are in the leaf
//   auto diff_res = std::ranges::set_difference(
//       parlay::sort(TL->pts.cut(0, TL->size)), parlay::sort(In),
//       TL->pts.begin(),
//       [](Point const& p1, Point const& p2) { return p1 < p2; });
//   TL->size = std::ranges::distance(TL->pts.begin(), diff_res.out);
//   TL->UpdateAug(TL->pts.cut(0, TL->size));
//
//   if constexpr (std::same_as<ReturnType, Node*>) {
//     return T;
//   } else if constexpr (std::same_as<ReturnType, NodeBox>) {
//     if constexpr (HasBox<typename Leaf::AT>) {
//       return NodeBox(T, TL->GetBox());
//     } else {
//       return NodeBox(T, GetBox(TL->pts.cut(0, TL->size)));
//     }
//   } else {
//     ;
//   }
// }
} // namespace pmos

#endif // PMOS_B_TREE_IMPL_LEAF_OP_HPP_

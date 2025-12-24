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

#include "b_tree.h"
#include "dependence/concepts.h"

namespace pmos {

// template <typename Point, typename DerivedTree, uint_fast8_t kSkHeight,
//           uint_fast8_t kImbaRatio>
// template <typename Leaf, typename Range>
// void BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::ExtractPointsInLeaf(
//     Node* T, Range Out) {
//   Leaf* TL = static_cast<Leaf*>(T);
//   if (TL->is_dummy) {
//     std::ranges::fill_n(Out.begin(), TL->size, TL->pts[0]);
//   } else {
//     std::ranges::copy(TL->pts.begin(), TL->pts.begin() + TL->size,
//     Out.begin());
//   }
//   return;
// }

template <typename TT>
TT::Node* BTree<TT>::InsertPoints2Leaf(Node* T, EntrySlice In) {
  Leaf* TL = static_cast<Leaf*>(T);

  assert(T->size + In.size() <= TT::kBufUB);
  assert(T->size == TL->GetEle().size());
  if (TL->elements.size() == 0) {
    assert(TL->size == 0);
    throw std::runtime_error(
        "Error: Leaf elements not initialized before insertion.");
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

template <typename TT>
BTree<TT>::NodeSplitSeqPair BTree<TT>::HandlingLeafSplitOverflow(
    Node* T, EntrySlice In) {
  auto entry_seq = TT::EntrySeq::uninitialized(T->size + In.size());
  Leaf* TL = static_cast<Leaf*>(T);

  for (SizeT i = 0; i < T->size; i++) {  // Assume the TT::kBufUB is constant
    entry_seq[i] = TL->elements[i];
  }
  parlay::parallel_for(0, In.size(),
                       [&](SizeT i) { entry_seq[T->size + i] = In[i]; });
  FreeNode<Leaf>(T);
  // std::cout << "Split leaf node with size " << entry_seq.size() << std::endl;

  // NOTE: split
  entry_seq = parlay::sort(entry_seq);  // PERF: we need sort as In is unsorted
  SizeT bkt_n = GetBktNum(entry_seq.size());
  auto split_seq = TT::SplitSeq::uninitialized(bkt_n - 1);
  auto node_seq = TT::NodeSeq::uninitialized(bkt_n);

  SizeT rt_bkt_n = GenerateLeafNodes(parlay::make_slice(entry_seq),
                                     parlay::make_slice(split_seq),
                                     parlay::make_slice(node_seq));

  assert(rt_bkt_n == bkt_n);

  return {node_seq, split_seq};
}

// template <typename Point, typename DerivedTree, uint_fast8_t kSkHeight,
//           uint_fast8_t kImbaRatio>
// template <typename Leaf, typename ReturnType>
// ReturnType BTree<Point, DerivedTree, kSkHeight,
//                     kImbaRatio>::DeletePoints4Leaf(Node* T, Slice In) {
//   assert(T->size >= In.size());
//   Leaf* TL = static_cast<Leaf*>(T);
//
//   if (TL->is_dummy) {
//     assert(In.size() <= T->size);
//     TL->size -= In.size();  // WARN: this assumes that In\in T
//     if (TL->size == 0) {
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
//   auto it = TL->pts.begin(), end = TL->pts.begin() + TL->size;
//   for (size_t i = 0; i < In.size(); i++) {
//     it = std::ranges::find(TL->pts.begin(), end, In[i]);
//     assert(it != end);
//     std::ranges::iter_swap(it, --end);
//   }
//
//   assert(std::cmp_equal(std::distance(TL->pts.begin(), end),
//                         TL->size - In.size()));
//   TL->size -= In.size();
//   assert(TL->size >= 0);
//   TL->UpdateAug(TL->pts.cut(0, TL->size));
//   // assert(TL->GetBox() == GetBox(TL->pts.cut(0, TL->size)));
//
//   if constexpr (std::same_as<ReturnType, Node*>) {
//     return T;
//   } else if constexpr (std::same_as<ReturnType, NodeBox>) {
//     if constexpr (HasBox<typename Leaf::AT>) {
//       return NodeBox(T, TL->GetBox());
//       // return NodeBox(T, GetBox(TL->pts.cut(0, TL->size)));
//     } else {
//       return NodeBox(T, GetBox(TL->pts.cut(0, TL->size)));
//     }
//   } else {
//   }
// }
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
}  // namespace pmos

#endif  // PMOS_B_TREE_IMPL_LEAF_OP_HPP_

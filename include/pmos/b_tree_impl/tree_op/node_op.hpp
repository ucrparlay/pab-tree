#ifndef PMOS_B_TREE_IMPL_TREE_OP_NODE_OP_HPP_
#define PMOS_B_TREE_IMPL_TREE_OP_NODE_OP_HPP_

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <numeric>
#include <tuple>
#include <type_traits>
#include <utility>

#include "b_tree.h"
#include "parlay/monoid.h"

namespace pmos {
// template <typename Point, typename DerivedTree, uint_fast8_t kSkHeight,
//           uint_fast8_t kImbaRatio>
// template <typename Leaf, typename Interior>
// auto BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::RetriveBox(
//     Node const* node)
//   requires(HasBox<typename Leaf::AT> && HasBox<typename Interior::AT>)
// {
//   return node->is_leaf ? static_cast<Leaf const*>(node)->GetBox()
//                        : static_cast<Interior const*>(node)->GetBox();
// }
//
// template <typename Point, typename DerivedTree, uint_fast8_t kSkHeight,
//           uint_fast8_t kImbaRatio>
// template <typename Leaf, typename Interior>
// typename Interior::ST const&
// BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::GetSplit(Node const* node)
//   requires std::same_as<
//       typename BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::Box,
//       typename Interior::ST>
// {
//   return node->is_leaf ? static_cast<Leaf const*>(node)->GetSplit()
//                        : static_cast<Interior const*>(node)->GetSplit();
// }
//
// // NOTE: update the info of T by new children L and R
// template <typename Point, typename DerivedTree, uint_fast8_t kSkHeight,
//           uint_fast8_t kImbaRatio>
// template <IsBinaryNode Interior, bool UpdateParFlag>
// inline void BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::UpdateInterior(
//     Node* T, Node* L, Node* R) {
//   assert(!T->is_leaf);
//   Interior* TI = static_cast<Interior*>(T);
//   if constexpr (UpdateParFlag) {
//     TI->ResetParallelFlag();
//   }
//   TI->size = L->size + R->size;
//   TI->left = L;
//   TI->right = R;
//   TI->UpdateAug(L, R);
//   return;
// }
//
// template <typename Point, typename DerivedTree, uint_fast8_t kSkHeight,
//           uint_fast8_t kImbaRatio>
// template <IsBinaryNode Interior, bool UpdateParFlag>
// inline void BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::UpdateInterior(
//     Node* T, NodeBox const& L, NodeBox const& R) {
//   assert(!T->is_leaf);
//   Interior* TI = static_cast<Interior*>(T);
//   if constexpr (UpdateParFlag) {
//     TI->ResetParallelFlag();
//   }
//   TI->size = L.first->size + R.first->size;
//   TI->left = L.first;
//   TI->right = R.first;
//   TI->UpdateAug(L.first, R.first);
//   return;
// }
template <typename TT>
size_t BTree<TT>::GenerateLeafNodes(TT::EntrySlice In, TT::SplitSlice key_out,
                                    TT::NodeSlice node_out) {
  size_t item_n = In.size();
  size_t bkt_n = GetBktNum(item_n);
  assert(bkt_n > 0);
  assert(key_out.size() >= bkt_n - 1);
  assert(node_out.size() >= bkt_n);

  parlay::parallel_for(0, bkt_n, [&](size_t i) {
    if (i < bkt_n - 1) {
      // key_out[i] = TT::GetKey(In[idx_to_pack[i + 1]]);
      key_out[i] = Entry::GetKey(In[GetBktEndIdx(i, bkt_n, item_n)]);
    }
    size_t ele_l = GetBktStIdx(i, bkt_n, item_n);
    size_t ele_r = (i == bkt_n - 1) ? item_n : GetBktEndIdx(i, bkt_n, item_n);
    node_out[i] = AllocNormalLeafNode<EntrySlice, Leaf>(In.cut(ele_l, ele_r));
  });
  return bkt_n;
}

template <typename TT>
size_t BTree<TT>::GenerateInteriorNodes(TT::NodeSlice node_in,
                                        TT::NodeSlice node_out,
                                        TT::SplitSlice key_in,
                                        TT::SplitSlice key_out) {
  assert(key_in.size() == node_in.size() - 1);
  size_t item_n = node_in.size();
  size_t bkt_n = GetBktNum(item_n);
  assert(bkt_n > 0);
  assert(key_out.size() >= bkt_n - 1);
  assert(node_out.size() >= bkt_n);

  parlay::parallel_for(0, bkt_n, [&](size_t i) {
    if (i < bkt_n - 1) {
      key_out[i] = key_in[GetBktEndIdx(i, bkt_n, item_n) - 1];
    }

    size_t node_l = GetBktStIdx(i, bkt_n, item_n);
    size_t node_r = (i == bkt_n - 1) ? item_n : GetBktEndIdx(i, bkt_n, item_n);
    size_t key_l = GetBktStIdx(i, bkt_n, item_n);
    size_t key_r =
        (i == bkt_n - 1) ? key_in.size() : GetBktEndIdx(i, bkt_n, item_n) - 1;
    node_out[i] = AllocInteriorNode<Interior, TT>(node_in.cut(node_l, node_r),
                                                  key_in.cut(key_l, key_r));
  });
  return bkt_n;
}

template <typename TT>
TT::Node* BTree<TT>::UpdateInterior(Node* T, auto const& node_seq_list,
                                    auto const& split_seq_list,
                                    size_t new_node_n) {
  assert(!T->is_leaf);
  assert(new_node_n <= TT::kBufUB);

  Interior* TI = static_cast<Interior*>(T);
  // typename TT::NodeSeq new_nodes(new_node_n);
  // typename TT::SplitSeq new_splits(new_node_n - 1);
  typename TT::BufSizeT node_idx = 0, split_idx = 0;

  assert(node_seq_list.size() == TI->GetNodeN());
  assert(split_seq_list.size() == TI->GetSplitN());

  // merge node
  size_t tot_size = 0;
  for (typename TT::BufSizeT i = 0; i < TI->GetNodeN(); i++) {
    for (typename TT::BufSizeT j = 0; j < node_seq_list[i].size(); j++) {
      TI->tree_nodes[node_idx++] = node_seq_list[i][j];
      tot_size += node_seq_list[i][j]->size;
    }
  }

  // merge split
  typename Interior::SplitArr new_splits;
  for (typename TT::BufSizeT i = 0; i < TI->GetSplitN(); i++) {
    for (typename TT::BufSizeT j = 0; j < split_seq_list[i].size(); j++) {
      new_splits[split_idx++] = split_seq_list[i][j];
    }
    if (i < split_seq_list.size() - 1) {
      new_splits[split_idx++] = TI->split[i];
    }
  }
  assert(node_idx == split_idx + 1);
  assert(node_idx == new_node_n);

  // TI->tree_nodes = new_nodes;
  TI->split = new_splits;
  TI->size = tot_size;
  TI->node_n = new_node_n;

#ifdef DEBUG
  for (int i = 0; i < TI->GetNodeN(); i++) {
    assert(TI->tree_nodes[i] != nullptr);
    auto box = GetBox(TI->tree_nodes[i]);
    if (TI->tree_nodes[i] == 1) continue;
    if (i == 0) {
      assert(box.second < TI->split[0]);
    } else if (i == TI->GetNodeN() - 1) {
      assert(box.first >= TI->split[TI->GetSplitN() - 1]);
    } else {
      assert(box.first >= TI->split[i - 1] && box.second < TI->split[i]);
    }
  }
#endif  // DEBUG

  // TODO: we needs to update the Aug here
  // WARN: we needs to update the Aug here
  TI->UpdateAug();
  return T;
}

template <typename TT>
BTree<TT>::NodeSplitSeqPair BTree<TT>::HandlingInteriorSplitOverflow(
    Node* T, auto const& node_seq_list, auto const& split_seq_list) {
  assert(!T->is_leaf);
  Interior* TI = static_cast<Interior*>(T);
  auto flatten_node_seq = parlay::flatten(node_seq_list);
  auto flatten_split_seq = parlay::sort(
      parlay::append(typename TT::SplitSeq(TI->split.begin(),
                                           TI->split.begin() + TI->GetSplitN()),
                     parlay::flatten(split_seq_list)));
  assert(parlay::is_sorted(flatten_split_seq));
  // std::cout << "Split interior node with size " << flatten_node_seq.size()
  //           << std::endl;

  size_t bkt_n = GetBktNum(flatten_node_seq.size());
  auto split_seq = TT::SplitSeq::uninitialized(bkt_n - 1);
  auto node_seq = TT::NodeSeq::uninitialized(bkt_n);

  [[maybe_unused]] size_t rt_bkt_n = GenerateInteriorNodes(
      parlay::make_slice(flatten_node_seq), parlay::make_slice(node_seq),
      parlay::make_slice(flatten_split_seq), parlay::make_slice(split_seq));

  assert(rt_bkt_n == bkt_n);
  return {node_seq, split_seq};
}

}  // namespace pmos

#endif  // PMOS_B_TREE_IMPL_TREE_OP_NODE_OP_HPP_

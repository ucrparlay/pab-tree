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

#include "parlay/monoid.h"
#include "parlay/parallel.h"
#include "pmos/b_tree.h"

namespace pmos {
template<typename TT>
template<typename OutputHandler>
TT::SizeT
BTree<TT>::GenerateLeafNodes(TT::EntrySlice In, OutputHandler&& output_handler)
{
  SizeT item_n = In.size();
  SizeT bkt_n = GetBktNum(item_n);
  assert(bkt_n > 0);

  parlay::parallel_for(0, bkt_n, [&](SizeT i) {
    if (i < bkt_n - 1) {
      output_handler.set_key(i,
                             Entry::GetKey(In[GetBktEndIdx(i, bkt_n, item_n)]));
    }
    SizeT ele_l = GetBktStIdx(i, bkt_n, item_n);
    SizeT ele_r = (i == bkt_n - 1) ? item_n : GetBktEndIdx(i, bkt_n, item_n);
    output_handler.set_node(
      i, AllocNormalLeafNode<EntrySlice, Leaf>(In.cut(ele_l, ele_r)));
  });
  return bkt_n;
}

template<typename TT>
template<typename OutputHandler>
TT::SizeT
BTree<TT>::GenerateInteriorNodes(TT::NodeSlice node_in,
                                 TT::SplitSlice key_in,
                                 OutputHandler&& output_handler)
{
  SizeT item_n = node_in.size();
  SizeT bkt_n = GetBktNum(item_n);
  assert(bkt_n > 0);

  parlay::parallel_for(0, bkt_n, [&](SizeT i) {
    if (i < bkt_n - 1) {
      output_handler.set_key(i, key_in[GetBktEndIdx(i, bkt_n, item_n) - 1]);
    }

    SizeT node_l = GetBktStIdx(i, bkt_n, item_n);
    SizeT node_r = (i == bkt_n - 1) ? item_n : GetBktEndIdx(i, bkt_n, item_n);
    SizeT key_l = GetBktStIdx(i, bkt_n, item_n);
    SizeT key_r =
      (i == bkt_n - 1) ? key_in.size() : GetBktEndIdx(i, bkt_n, item_n) - 1;

    output_handler.set_node(
      i,
      AllocInteriorNode<Interior, TT>(node_in.cut(node_l, node_r),
                                      key_in.cut(key_l, key_r)));
  });
  return bkt_n;
}

template<typename TT>
TT::Node*
BTree<TT>::BatchInsertUpdateInterior(Node* T,
                                     NodeSplitPairSlice node_split_slice,
                                     SizeBufUBArr const& split_pos_arr,
                                     SizeT new_node_n,
                                     SizeBufUBArr const& return_node_n_arr)
{
  assert(!T->is_leaf);
  assert(new_node_n <= TT::kBufUB);

  auto TI = static_cast<Interior*>(T);

  // merge node
  assert(std::numeric_limits<BufSizeT>::max() >= new_node_n);
  SizeT tot_entry_sz = 0;
  BufSizeT node_idx = 0;
  typename Interior::NodeArr new_nodes;
  for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
    if (return_node_n_arr[i] == 0) {
      new_nodes[node_idx++] = TI->tree_nodes[i];
      tot_entry_sz += new_nodes[node_idx - 1]->size;
    } else {
      SizeT st = (i == 0 ? 0 : split_pos_arr[i - 1]);
      for (BufSizeT j = 0; j < return_node_n_arr[i]; j++) {
        new_nodes[node_idx++] = node_split_slice[st + j].first;
        tot_entry_sz += new_nodes[node_idx - 1]->size;
      }
    }
  }

  // merge split
  // NOTE:the original split will always stay
  typename Interior::SplitArr new_splits;
  BufSizeT split_idx = 0;
  for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
    SizeT st = (i == 0 ? 0 : split_pos_arr[i - 1]);
    // HACK: if return_node_n_arr[i] == 0, then no new splits from this child
    // should skip; but -1 always smaller than 0, so the loop never runs.
    // Needs to handle the type carefully.
    for (int j = 0; j < static_cast<int>(return_node_n_arr[i]) - 1; j++) {
      new_splits[split_idx++] = node_split_slice[st + j].second;
    }
    // append the tree split
    if (i < TI->GetNodeN() - 1) { // skip the new splits for last node
      new_splits[split_idx++] = TI->split[i];
    }
  }
  assert(node_idx == split_idx + 1);
  assert(node_idx == new_node_n);

  TI->tree_nodes = std::move(new_nodes);
  TI->split = std::move(new_splits);
  TI->size = tot_entry_sz;
  TI->node_n = new_node_n;

#ifdef DEBUG
  for (int i = 0; i < TI->GetNodeN(); i++) {
    assert(TI->tree_nodes[i] != nullptr);
    if (TI->GetNodeN() == 1) {
      continue;
    }
    auto box = GetBox(TI->tree_nodes[i]);
    if (i == 0) {
      assert(box.second < TI->split[0]);
    } else if (i == TI->GetNodeN() - 1) {
      assert(box.first >= TI->split[TI->GetSplitN() - 1]);
    } else {
      assert(box.first >= TI->split[i - 1] && box.second < TI->split[i]);
    }
  }
#endif // DEBUG

  TI->UpdateAug();
  return T;
}

template<typename TT>
std::pair<bool, typename TT::SizeT>
BTree<TT>::BatchDeleteUpdateInteriorSize(
  Node* T,
  BooleanBufUBArr const& invalid_subtree_flag_arr,
  SizeBufUBArr const& invalid_subtree_num_arr)
// update the size and valid status of the interior node, return the number of
// invalid subtree
{
  assert(!T->is_leaf);
  auto TI = static_cast<Interior*>(T);
  SizeT tot_entry_sz = 0;
  SizeT tot_invalid_subtree_n = 0;
  BufSizeT invalid_tree_n = 0;
  for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
    tot_entry_sz += TI->tree_nodes[i]->size;
    invalid_tree_n += invalid_subtree_flag_arr[i] ? 1 : 0;
    tot_invalid_subtree_n += invalid_subtree_num_arr[i];
  }
  TI->size = tot_entry_sz;
  TI->valid = T != this->root_ ? (TI->GetNodeN() - invalid_tree_n) >= TT::kBufLB
                               : (TI->GetNodeN() - invalid_tree_n) > 0;
  TI->invalid_subtree_num = TI->valid ? tot_invalid_subtree_n : 1;
  return std::make_pair(!TI->valid, TI->invalid_subtree_num);
}

template<typename TT>
TT::SizeT
BTree<TT>::HandlingInteriorSplitOverflow(Node* T,
                                         NodeSplitPairSlice node_split_slice,
                                         SizeBufUBArr const& split_pos_arr,
                                         SizeT new_node_n,
                                         SizeBufUBArr const& return_node_n_arr)
{
  assert(!T->is_leaf);
  auto TI = static_cast<Interior*>(T);
  typename TT::NodeSeq flatten_node_seq =
    TT::NodeSeq::uninitialized(new_node_n);
  typename TT::SplitSeq flatten_split_seq =
    TT::SplitSeq::uninitialized(new_node_n - 1);

  // merge node
  SizeT node_idx = 0;
  for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
    if (return_node_n_arr[i] == 0) {
      flatten_node_seq[node_idx++] = TI->tree_nodes[i];
    } else {
      SizeT st = (i == 0 ? 0 : split_pos_arr[i - 1]);

      parlay::parallel_for(0, return_node_n_arr[i], [&](SizeT j) {
        flatten_node_seq[node_idx + j] = node_split_slice[st + j].first;
      });
      node_idx += return_node_n_arr[i];
    }
  }

  // merge split
  SizeT split_idx = 0;
  for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
    if (return_node_n_arr[i] > 0) {
      SizeT st = (i == 0 ? 0 : split_pos_arr[i - 1]);
      assert(return_node_n_arr[i] > 0 ||
             static_cast<int>(return_node_n_arr[i]) - 1 < 0);

      parlay::parallel_for(
        0, static_cast<int>(return_node_n_arr[i]) - 1, [&](SizeT j) {
          flatten_split_seq[split_idx + j] = node_split_slice[st + j].second;
        });
      split_idx += return_node_n_arr[i] - 1;
    }
    if (i < TI->GetNodeN() - 1) { // skip the new splits for last node
      flatten_split_seq[split_idx++] = TI->split[i];
    }
  }

  assert(node_idx == split_idx + 1);
  assert(parlay::is_sorted(flatten_split_seq,
                           [&](TT::K a, TT::K b) { return a < b; }));

  // launch rebuild
  FreeNode<Interior>(T);
  [[maybe_unused]] SizeT rt_bkt_n = GenerateInteriorNodes(
    parlay::make_slice(flatten_node_seq),
    parlay::make_slice(flatten_split_seq),
    GenNodesOutputHandler{
      .set_key = [&](SizeT i, auto key) { node_split_slice[i].second = key; },
      .set_node = [&](SizeT i,
                      auto node) { node_split_slice[i].first = node; } });

  return rt_bkt_n;
}

template<typename TT>
TT::SizeT
BTree<TT>::FetchInvalidSubtreeNodes(Node* T, TT::NodeSlice flatten_node_slice)
{
  if (T->is_leaf) {
    if (T->size < TT::kBufLB) {
      assert(flatten_node_slice.size() == 1);
      flatten_node_slice[0] = T;
      return T->size;
    }
    return 0;
  }

  auto TI = static_cast<Interior*>(T);
  if (TI->valid) {                      // find a valid subtree
    if (TI->invalid_subtree_num == 0) { // no subtree is invalid
      return 0;
    } else { // contain a invalid subtree
      typename Interior::NodeArr invalid_node_arr;
      assert(invalid_node_arr.size() == TT::kBufUB);
      BufSizeT invalid_idx = 0;
      BufSizeT tree_node_idx = 0;

      for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
        if (GetNodeInvalidSubtreeNum(TI->tree_nodes[i])) {
          // this subtree needs to further check
          invalid_node_arr[invalid_idx] = TI->tree_nodes[i];
          invalid_idx++;
          if (!GetNodeInvalidStatus(TI->tree_nodes[i])) {
            // this subtree is a valid node, needs to keep in the
            // current interior node
            TI->tree_nodes[tree_node_idx] = TI->tree_nodes[i];
            tree_node_idx++;
          }
        } else {
          TI->tree_nodes[tree_node_idx] = TI->tree_nodes[i];
          tree_node_idx++;
        }
      }

      assert(TI->node_n >= invalid_idx);
      TI->node_n = tree_node_idx;

      assert(invalid_idx == TI->invalid_subtree_num);
      assert(flatten_node_slice.size() == invalid_idx);

      SizeBufUBArr remove_entries_num;
      parlay::parallel_for(
        0,
        invalid_idx,
        [&](BufSizeT i) {
          SizeT sum = 0;
          for (BufSizeT j = 0; j < i; j++) {
            sum += GetNodeInvalidSubtreeNum(invalid_node_arr[j]);
          }
          remove_entries_num[i] = FetchInvalidSubtreeNodes(
            invalid_node_arr[i],
            flatten_node_slice.cut(
              sum, GetNodeInvalidSubtreeNum(invalid_node_arr[i])));
        },
        1);

      SizeT total_remove_entries_n =
        std::accumulate(remove_entries_num.begin(),
                        remove_entries_num.begin() + invalid_idx,
                        static_cast<SizeT>(0));
      TI->size -= total_remove_entries_n;
      return total_remove_entries_n;
    }
  } else { // find a invalid tree
    assert(flatten_node_slice.size() == 1);
    assert(TI->invalid_subtree_num == 1);
    flatten_node_slice[0] = T;
    return T->size;
  }
}

template<typename TT>
TT::Node*
BTree<TT>::CopyTreeRecursive(Node* T)
{
  if (T->is_leaf) {
    auto const TL = static_cast<Leaf const*>(T);
    return AllocNormalLeafNode<TT::EntrySlice, Leaf>(TL->GetEle());
  }
  auto const TI = static_cast<Interior const*>(T);
  auto new_nodes = parlay::tabulate(TI->GetNodeN(), [&](size_t i) {
    return CopyTreeRecursive(TI->tree_nodes[i]);
  });
  return AllocInteriorNode(parlay::make_slice(new_nodes), TI->GetSplitSlice());
}

} // namespace pmos

#endif // PMOS_B_TREE_IMPL_TREE_OP_NODE_OP_HPP_

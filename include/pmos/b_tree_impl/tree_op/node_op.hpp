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
#include "parlay/primitives.h"
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
      SizeT st = 2 * (i == 0 ? 0 : split_pos_arr[i - 1]);
      for (BufSizeT j = 0; j < return_node_n_arr[i]; j++) {
        new_nodes[node_idx++] = node_split_slice[st + j].first;
        tot_entry_sz += new_nodes[node_idx - 1]->size;
      }
    }
  }

  // merge split
  // NOTE:the original split will always stay
  typename Interior::SplitArr new_splits;
  typename Interior::SplitArr old_splits = TI->split;
  BufSizeT split_idx = 0;
  for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
    SizeT st = 2 * (i == 0 ? 0 : split_pos_arr[i - 1]);
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
    auto node = TI->tree_nodes[i];
    auto box = GetNodeBoxRec(TI->tree_nodes[i]);
    // assert(node->is_leaf || box == GetNodeBox(node));
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
      SizeT st = 2 * (i == 0 ? 0 : split_pos_arr[i - 1]);

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
      SizeT st = 2 * (i == 0 ? 0 : split_pos_arr[i - 1]);
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
std::tuple<bool, typename TT::SizeT, typename TT::SizeT>
BTree<TT>::BatchDeleteUpdateInteriorSize(
  Node* T,
  SizeBufUBArr const& invalid_subtree_num_arr,
  BooleanBufUBArr const& invalid_subtree_flag_arr,
  SizeBufUBArr const& reinsert_entries_num_arr)
// update the size and valid status of the interior node, return the number of
// invalid subtree
{
  assert(!T->is_leaf);
  auto TI = static_cast<Interior*>(T);
  SizeT tot_entry_sz = 0;
  SizeT tot_invalid_subtree_n = 0;
  SizeT reinsert_entries_n = 0;
  BufSizeT invalid_tree_n = 0;
  for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
    tot_entry_sz += TI->tree_nodes[i]->size;
    invalid_tree_n += invalid_subtree_flag_arr[i] ? 1 : 0;
    tot_invalid_subtree_n += invalid_subtree_num_arr[i];
    reinsert_entries_n += reinsert_entries_num_arr[i];
  }
  // TODO: these values should be reset in following operations
  TI->size = tot_entry_sz;
  TI->valid =
    (T != this->root_ ? (TI->GetNodeN() - invalid_tree_n) >= TT::kBufLB
                      : (TI->GetNodeN() - invalid_tree_n) > 0);
  // TI->box = GetNodeSliceBox(TI->GetNodeSlice());
  TI->invalid_subtree_num = TI->valid ? tot_invalid_subtree_n : 1;
  TI->reinsert_size = TI->valid ? reinsert_entries_n : TI->size;
  return std::make_tuple(GetNodeInvalidStatus(T),
                         GetNodeInvalidSubtreeNum(T),
                         GetNodeReinsertSize(T));
}

template<typename TT>
TT::SizeT
BTree<TT>::FetchInvalidSubtreeNodes(Node* T,
                                    TT::NodeSlice flatten_node_slice,
                                    EntrySlice reinsert_entries,
                                    const bool has_tomb)
{
  constexpr bool kSortReinsertEntries = true;
  assert(flatten_node_slice.size() == GetNodeInvalidSubtreeNum(T));
  assert(reinsert_entries.size() == GetNodeReinsertSize(T));

  if (GetNodeInvalidStatus(T)) { // every invalid tree should be flatten
    assert(flatten_node_slice.size() == 1);
    assert(reinsert_entries.size() == T->size);
    assert(has_tomb == false);

    flatten_node_slice[0] = T;
    SizeT size = T->size;
    assert(T != nullptr);
    FlattenRec<decltype(reinsert_entries), true, kSortReinsertEntries>(
      T, reinsert_entries);
    DeleteTreeRec(T);
    T = nullptr;
    return size;
  }

  assert(NodeIsValid(T));

  if (T->is_leaf) { // do nothing for a valid leaf
    assert(flatten_node_slice.size() == 0);
    assert(reinsert_entries.size() == 0);
    return 0;
  }

  auto TI = static_cast<Interior*>(T);

  // TODO: if invalid subtree num == 1 but reinsert size ==0 then just delete
  if (TI->invalid_subtree_num == 0 &&
      TI->reinsert_size == 0) { // no subtree is invalid
    assert(TI->valid);
    assert(flatten_node_slice.size() == 0);
    assert(reinsert_entries.size() == 0);
    return 0;
  }

  // PARA: this node is valid, but contains invalid subtree
  typename Interior::NodeArr nodes_contain_invalid_subtree;
  // typename Interior::NodeArr new_tree_nodes;
  // typename Interior::SplitArr new_splits;
  assert(nodes_contain_invalid_subtree.size() == TT::kBufUB);
  BufSizeT invalid_idx = 0;
  BufSizeT tree_node_idx = 0;
  BufSizeT split_idx = 0;

  bool reinsert_flag = false; // whether we will reinsert in this node
  for (BufSizeT i = 0; i < TI->GetNodeN(); ++i) {
    auto node = TI->tree_nodes[i];

    // this node contain invalid subtree
    if (GetNodeInvalidSubtreeNum(node)) {
      nodes_contain_invalid_subtree[invalid_idx] = node;
      invalid_idx++;
    }

    if (!reinsert_flag && GetNodeInvalidStatus(node)) {
      reinsert_flag = true;
    }

    // check if this child node contain no invalid subtree, then itself must
    // be valid
    assert(GetNodeInvalidSubtreeNum(node) == 0 ? NodeIsValid(node) : true);
    assert(NodeIsValid(node) ? 1 : GetNodeInvalidSubtreeNum(node) == 1);

    if (NodeIsValid(node)) {
      // new_tree_nodes[tree_node_idx] = node;
      // tree_node_idx++;
      // if (i < TI->GetSplitN()) {
      //   new_splits[split_idx] = TI->split[i];
      //   split_idx++;
      // }

      const bool has_split = i < TI->GetSplitN();
      if (i != tree_node_idx) {
        TI->tree_nodes[tree_node_idx] = node;
        if (has_split) {
          TI->split[split_idx] = TI->split[i];
        }
      }
      tree_node_idx += 1;
      split_idx += has_split;
    }
  }
  // TI->tree_nodes = std::move(new_tree_nodes);
  // TI->split = std::move(new_splits);
  assert(TI->node_n >= invalid_idx);
  TI->node_n = tree_node_idx;

  SizeBufUBArr remove_entries_num;
  SizeBufUBArr subtree_sum;
  SizeBufUBArr reinsert_sum;
  subtree_sum[0] = 0;
  reinsert_sum[0] = 0;
  for (BufSizeT i = 1; i < invalid_idx; i++) {
    subtree_sum[i] =
      subtree_sum[i - 1] +
      GetNodeInvalidSubtreeNum(nodes_contain_invalid_subtree[i - 1]);
    reinsert_sum[i] = reinsert_sum[i - 1] +
                      GetNodeReinsertSize(nodes_contain_invalid_subtree[i - 1]);
  }

  // for (BufSizeT i = 0; i < invalid_idx; i++) {
  //   auto node = nodes_contain_invalid_subtree[i];
  //
  //   remove_entries_num[i] = FetchInvalidSubtreeNodes(
  //     node,
  //     flatten_node_slice.cut(subtree_sum[i],
  //                            i == invalid_idx - 1 ? flatten_node_slice.size()
  //                                                 : subtree_sum[i + 1]),
  //     reinsert_entries.cut(reinsert_sum[i],
  //                          i == invalid_idx - 1 ? reinsert_entries.size()
  //                                               : reinsert_sum[i + 1]),
  //     has_tomb ? !reinsert_flag : false);
  // }

  parlay::parallel_for(
    0,
    invalid_idx,
    [&](BufSizeT i) {
      remove_entries_num[i] = FetchInvalidSubtreeNodes(
        nodes_contain_invalid_subtree[i],
        flatten_node_slice.cut(subtree_sum[i],
                               i == invalid_idx - 1 ? flatten_node_slice.size()
                                                    : subtree_sum[i + 1]),
        reinsert_entries.cut(reinsert_sum[i],
                             i == invalid_idx - 1 ? reinsert_entries.size()
                                                  : reinsert_sum[i + 1]),
        has_tomb ? !reinsert_flag : false);
    },
    1);

  SizeT total_remove_entries_n =
    std::accumulate(remove_entries_num.begin(),
                    remove_entries_num.begin() + invalid_idx,
                    static_cast<SizeT>(0));

  assert(total_remove_entries_n == reinsert_entries.size());
  assert(parlay::is_sorted(reinsert_entries, [](auto a, auto b) {
    return Entry::GetKey(a) < Entry::GetKey(b) &&
           Entry::GetKey(a) != Entry::GetKey(b);
  }));
  // assert(parlay::all_of(flatten_node_slice, [&](auto node) {
  //   return node != nullptr && VerifyUnoverlappedTree(node,
  //   flatten_node_slice);
  // }));
  // assert((this->Validate<true, false>(T)));

  if (reinsert_flag && has_tomb) {
    TI->size -= total_remove_entries_n;
    ReInsertTreeAfterDelete(T, reinsert_entries);
  } else { // the subtrees may perform reinsertion
    TI->size =
      std::accumulate(TI->tree_nodes.begin(),
                      TI->tree_nodes.begin() + TI->GetNodeN(),
                      static_cast<SizeT>(0),
                      [](SizeT sum, Node* node) { return sum + node->size; });
  }

  assert((this->Validate<true, false>(T)));

  return total_remove_entries_n;
}

template<typename TT>
void
BTree<TT>::ReInsertTreeAfterDelete(Node* T, EntrySlice reinsert_entries)
{
  NodeSplitPairSeq node_split_seq =
    NodeSplitPairSeq::uninitialized(2 * reinsert_entries.size());
  SizeT new_node_n = BatchInsertRecursive(
    T, reinsert_entries, parlay::make_slice(node_split_seq));
  assert(new_node_n == 0);
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

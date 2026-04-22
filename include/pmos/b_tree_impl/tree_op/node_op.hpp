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
                                    EntrySlice reinsert_entries,
                                    const bool has_tomb)
{
  constexpr bool kSortReinsertEntries = true;
  assert(reinsert_entries.size() == GetNodeReinsertSize(T));

  if (GetNodeInvalidStatus(T)) { // every invalid tree should be flatten
    assert(reinsert_entries.size() == T->size);
    assert(has_tomb == false);

    auto size = T->size;
    assert(T != nullptr);
    FlattenRec<decltype(reinsert_entries), true, kSortReinsertEntries>(
      T, reinsert_entries);
    DeleteTreeRec(T);
    T = nullptr;
    return size;
  }

  assert(NodeIsValid(T));

  if (T->is_leaf) { // do nothing for a valid leaf
    assert(reinsert_entries.size() == 0);
    return 0;
  }

  auto TI = static_cast<Interior*>(T);

  if (TI->invalid_subtree_num == 0 && TI->reinsert_size == 0) {
    // no subtree is invalid, fully valid
    assert(TI->valid);
    assert(reinsert_entries.size() == 0);
    return 0;
  }

  if (SelfBalanceNode(T, has_tomb)) { // so far a good tree
    assert(NodeIsValid(T));
    if (GetNodeReinsertSize(T) == 0) {
      assert(TI->invalid_subtree_num != 0);
      PruneOutEmptyTree(T);
      return 0;
    }

    BalanceOverLeaves(T, TI->reinsert_size);
    return 0;
  }

  // PARA: this node is valid, but contains invalid subtree
  typename Interior::NodeArr invalid_subtree;
  // typename Interior::NodeArr new_tree_nodes;
  // typename Interior::SplitArr new_splits;
  assert(invalid_subtree.size() == TT::kBufUB);
  BufSizeT invalid_idx = 0;
  BufSizeT tree_node_idx = 0;
  BufSizeT split_idx = 0;

  bool reinsert_flag = false; // whether we will reinsert in this node
  for (BufSizeT i = 0; i < TI->GetNodeN(); ++i) {
    auto node = TI->tree_nodes[i];

    // this node contain invalid subtree
    if (GetNodeInvalidSubtreeNum(node)) {
      invalid_subtree[invalid_idx] = node;
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
  SizeBufUBArr reinsert_sum;
  reinsert_sum[0] = 0;
  for (BufSizeT i = 1; i < invalid_idx; i++) {
    auto node = invalid_subtree[i - 1];
    reinsert_sum[i] = reinsert_sum[i - 1] + GetNodeReinsertSize(node);
  }

  // for (BufSizeT i = 0; i < invalid_idx; i++) {
  //   auto node = invalid_subtree[i];
  //
  //   remove_entries_num[i] = FetchInvalidSubtreeNodes(
  //     node,
  //     flatten_node_slice.cut(subtree_sum[i],
  //                            i == invalid_idx - 1 ? flatten_node_slice.size()
  //                                                 : subtree_sum[i + 1]),
  //     reinsert_entries.cut(reinsert_sum[i],
  //                          i == invalid_idx - 1 ? reinsert_entries.size()
  //                                               : reinsert_sum[i + 1]),
  //     next_tomb);
  // }

  bool const next_tomb = has_tomb ? !reinsert_flag : false;
  parlay::parallel_for(
    0,
    invalid_idx,
    [&](BufSizeT i) {
      auto node = invalid_subtree[i];
      remove_entries_num[i] = FetchInvalidSubtreeNodes(
        node,
        reinsert_entries.cut(reinsert_sum[i],
                             i == invalid_idx - 1 ? reinsert_entries.size()
                                                  : reinsert_sum[i + 1]),
        next_tomb);
    },
    1);

  // SizeT total_remove_entries_n =
  //   std::accumulate(remove_entries_num.begin(),
  //                   remove_entries_num.begin() + invalid_idx,
  //                   static_cast<SizeT>(0));
  //
  // assert(total_remove_entries_n == reinsert_entries.size());
  // assert(parlay::all_of(flatten_node_slice, [&](auto node) {
  //   return node != nullptr && VerifyUnoverlappedTree(node,
  //   flatten_node_slice);
  // }));
  // assert((this->Validate<true, false>(T)));

  TI->size =
    std::accumulate(TI->tree_nodes.begin(),
                    TI->tree_nodes.begin() + TI->GetNodeN(),
                    static_cast<SizeT>(0),
                    [](SizeT sum, Node* node) { return sum + node->size; });
  if (reinsert_flag && has_tomb) {
    // TI->size -= total_remove_entries_n;
    assert(parlay::none_of(
      reinsert_entries, [](auto entry) { return Entry::GetVal(entry) == 0; }));
    assert(parlay::is_sorted(reinsert_entries, [](auto a, auto b) {
      return Entry::GetKey(a) < Entry::GetKey(b) &&
             Entry::GetKey(a) != Entry::GetKey(b);
    }));
    ReInsertTreeAfterDelete(T, reinsert_entries);
  }

  assert((this->Validate<true, false>(T)));
  TI->ResetDeletionFlag();

  return 0;
}

template<typename TT>
void
BTree<TT>::BalanceOverLeaves(Node* T, const SizeT reinsert_size)
{
  // std::cerr << "BalanceOverLeaves: " << reinsert_size << std::endl;
  auto TI = static_cast<Interior*>(T);
  BufSizeBufUBArr tree_group = { -1 };
  auto rightmost_pivot_idx = TT::kBufUB;
  auto pivot = TT::kBufUB;
  for (int i = TI->GetNodeN() - 1; i >= 0; --i) { // WARN: caution with --i
    if (NodeIsValid(TI->tree_nodes[i])) {
      if (rightmost_pivot_idx == TT::kBufUB) { // meet the rightmost valid tree
        rightmost_pivot_idx = i;
      }
      pivot = i;
      tree_group[i] = pivot;
      continue;
    }
    tree_group[i] = pivot;
  }
  assert(rightmost_pivot_idx != TT::kBufUB);
  for (BufSizeT i = rightmost_pivot_idx + 1; i < TI->GetNodeN(); ++i) {
    assert(tree_group[i] == TT::kBufUB);
    tree_group[i] = rightmost_pivot_idx;
  }

#ifdef DEBUG
  for (BufSizeT i = 0; i < TI->GetNodeN(); ++i) {
    if (GetNodeInvalidStatus(TI->tree_nodes[i])) {
      assert(tree_group[i] ==
             tree_group[i == TI->GetNodeN() - 1 ? i - 1 : i + 1]);
    } else {
      assert(tree_group[i] == i);
    }
  }
#endif // DEBUG

  auto entry_buf = TT::EntrySeq::uninitialized(reinsert_size + TT::kBufUB);
  typename Interior::NodeArr new_tree_nodes = { nullptr };
  typename Interior::SplitArr new_splits = { 0 };
  BufSizeT tree_idx = 0;
  BufSizeT split_idx = 0;
  BufSizeT i = 0;

  auto quick_check = [&](auto group_id) -> bool {
    auto cur_tree_idx = tree_idx - 1;
    auto cur_split_idx = split_idx - 1;
    auto box = GetNodeBoxRec(new_tree_nodes[cur_tree_idx]);
    if (tree_idx == 1) {
      return box.second < new_splits[cur_split_idx];
    } else if (group_id == rightmost_pivot_idx) {
      return box.first >= new_splits[cur_split_idx];
    } else {
      return box.first >= new_splits[cur_split_idx - 1] &&
             box.second < new_splits[cur_split_idx];
    }
  };

  while (i < TI->GetNodeN()) {
    assert(tree_group[i] < TT::kBufUB);
    BufSizeT j = i;
    SizeT sum = 0;
    while (j < tree_group.size() && tree_group[j] == tree_group[i]) {
      sum += TI->tree_nodes[j]->size;
      j++;
    }

    // NOTE: [i, j-1] is the tree nodes in same group
    if (j == i + 1) {
      assert(NodeIsValid(TI->tree_nodes[i]));
      new_tree_nodes[tree_idx++] = TI->tree_nodes[i];
      if (tree_group[i] != rightmost_pivot_idx) {
        new_splits[split_idx++] = TI->split[i];
      }
      assert(quick_check(tree_group[i]));
      i = j;
      continue;
    }

    if (sum < TT::kBufUB) { // fit into one leaf
      auto dest_idx = j - 1;
      auto dest_node = static_cast<Leaf*>(TI->tree_nodes[dest_idx]);
      assert(NodeIsValid(dest_node) || tree_group[i] == rightmost_pivot_idx);
      for (BufSizeT k = i; k < dest_idx; ++k) {
        auto leaf = static_cast<Leaf*>(TI->tree_nodes[k]);
        assert(GetNodeInvalidStatus(leaf) ||
               tree_group[i] == rightmost_pivot_idx);
        std::ranges::copy_n(
          leaf->GetEleBeginIter(), leaf->size, dest_node->GetEleEndIter());
        dest_node->size += leaf->size;
        FreeNode<Leaf>(leaf);
      }
      dest_node->is_sorted = false;
      new_tree_nodes[tree_idx++] = dest_node;
      if (tree_group[i] != rightmost_pivot_idx) {
        new_splits[split_idx++] = TI->split[dest_idx];
      }
      assert(quick_check(tree_group[i]));
      i = j;
      // TODO: may sort as well
      continue;
    }

    // need to distribute
    SizeT item_n = 0;
    for (BufSizeT k = i; k < j; ++k) {
      auto leaf = static_cast<Leaf*>(TI->tree_nodes[k]);
      std::ranges::copy_n(
        leaf->GetEleBeginIter(), leaf->size, entry_buf.begin() + item_n);
      item_n += leaf->size;
    }

    std::sort(entry_buf.begin(),
              entry_buf.begin() + item_n,
              [&](auto const& a, auto const& b) {
                return Entry::GetKey(a) < Entry::GetKey(b);
              });

    auto const bkt_n = GetBktNum(item_n);
    assert(bkt_n <= j - i);
    for (BufSizeT k = 0; k < bkt_n; ++k) {
      auto const ele_l = GetBktStIdx(k, bkt_n, item_n);
      auto const ele_r =
        (k == bkt_n - 1) ? item_n : GetBktEndIdx(k, bkt_n, item_n);
      auto leaf = static_cast<Leaf*>(TI->tree_nodes[i + k]);
      std::ranges::copy_n(
        entry_buf.begin() + ele_l, ele_r - ele_l, leaf->GetEleBeginIter());
      leaf->size = ele_r - ele_l;
      leaf->is_sorted = true;
      new_tree_nodes[tree_idx++] = leaf;
      if (k < bkt_n - 1) {
        new_splits[split_idx++] = Entry::GetKey(entry_buf[ele_r]);
        // assert(quick_check(tree_group[i]));
      }
    }
    if (tree_group[i] != rightmost_pivot_idx) {
      new_splits[split_idx++] = TI->split[j - 1];
    }
    assert(quick_check(tree_group[i]));
    for (BufSizeT k = i + bkt_n; k < j; ++k) {
      FreeNode<Leaf>(TI->tree_nodes[k]);
    }

    i = j;
  }

  assert(tree_idx == split_idx + 1);
  TI->tree_nodes = std::move(new_tree_nodes);
  TI->split = std::move(new_splits);
  TI->node_n = tree_idx;
  TI->ResetDeletionFlag();

#ifdef DEBUG
  // ensure the tree structure is correct after the compaction,
  // i.e., all the nodes in the interior node are valid and the split is
  // correct
  for (int i = 0; i < TI->GetNodeN(); i++) {
    assert(TI->tree_nodes[i] != nullptr);
    if (TI->GetNodeN() == 1) {
      continue;
    }
    auto box = GetNodeBoxRec(TI->tree_nodes[i]);
    if (i == 0) {
      assert(box.second < TI->split[0]);
    } else if (i == TI->GetNodeN() - 1) {
      assert(box.first >= TI->split[TI->GetSplitN() - 1]);
    } else {
      assert(box.first >= TI->split[i - 1] && box.second < TI->split[i]);
    }
  }
#endif // DEBUG
  // this->Validate<true, false>(T);
}

template<typename TT>
void
BTree<TT>::PruneOutEmptyTree(Node* T)
{
  assert(GetNodeReinsertSize(T) == 0);
  assert(NodeHasLeafChildren(T));
  auto TI = static_cast<Interior*>(T);
  BufSizeT tree_node_idx = 0;
  BufSizeT split_idx = 0;
  for (BufSizeT i = 0; i < TI->GetNodeN(); ++i) {
    auto node = TI->tree_nodes[i];

    assert(NodeIsValid(node) || GetNodeReinsertSize(node) == 0);
    if (NodeIsValid(node)) {
      const bool has_split = i < TI->GetSplitN();
      if (i != tree_node_idx) {
        TI->tree_nodes[tree_node_idx] = node;
        if (has_split) {
          TI->split[split_idx] = TI->split[i];
        }
      }
      tree_node_idx += 1;
      split_idx += has_split;
    } else {
      assert(node->size == 0);
      FreeNode<Leaf>(node);
    }
  }

  TI->node_n = tree_node_idx;
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

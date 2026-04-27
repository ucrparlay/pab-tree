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

  assert(std::numeric_limits<BufSizeT>::max() >= new_node_n);

  // Fast path: no child split — tree_nodes/split arrays unchanged, only size
  // differs. return_node_n_arr[i] is always 0 or >=2
  // (GenerateLeafNodes/GenerateInteriorNodes always produce >=2 buckets), so
  // new_node_n == GetNodeN() iff all are 0.
  if (new_node_n == static_cast<SizeT>(TI->GetNodeN())) {
    SizeT tot = 0;
    for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
      tot += TI->tree_nodes[i]->size;
    }
    TI->size = tot;
    TI->UpdateAug();
    return T;
  }

  // Slow path: some child split — rebuild tree_nodes and split arrays.
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

  // NOTE: the original split will always stay
  typename Interior::SplitArr new_splits;
  BufSizeT split_idx = 0;
  for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
    SizeT st = 2 * (i == 0 ? 0 : split_pos_arr[i - 1]);
    // HACK: if return_node_n_arr[i] == 0, then no new splits from this child
    // should skip; but -1 always smaller than 0, so the loop never runs.
    // Needs to handle the type carefully.
    for (int j = 0; j < static_cast<int>(return_node_n_arr[i]) - 1; j++) {
      new_splits[split_idx++] = node_split_slice[st + j].second;
    }
    if (i < TI->GetNodeN() - 1) {
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
std::tuple<bool, bool, typename TT::SizeT>
BTree<TT>::BatchDeleteUpdateInteriorSize(
  Node* T,
  BooleanBufUBArr const& invalid_subtree_num_arr,
  BooleanBufUBArr const& invalid_subtree_flag_arr,
  SizeBufUBArr const& reinsert_entries_num_arr)
// update the size and valid status of the interior node, return the number of
// invalid subtree
{
  assert(!T->is_leaf);
  auto TI = static_cast<Interior*>(T);
  SizeT tot_entry_sz = 0;
  bool contain_invalid = 0;
  SizeT reinsert_entries_n = 0;
  BufSizeT invalid_tree_n = 0;
  for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
    tot_entry_sz += TI->tree_nodes[i]->size;
    invalid_tree_n += invalid_subtree_flag_arr[i] ? 1 : 0;
    contain_invalid |= invalid_subtree_num_arr[i];
    reinsert_entries_n += reinsert_entries_num_arr[i];
  }
  TI->size = tot_entry_sz;
  TI->valid =
    (T != this->root_ ? (TI->GetNodeN() - invalid_tree_n) >= TT::kBufLB
                      : (TI->GetNodeN() - invalid_tree_n) > 0);
  // TI->box = GetNodeSliceBox(TI->GetNodeSlice());
  TI->contain_invalid_subtree = TI->valid ? contain_invalid : true;
  TI->reinsert_size = TI->valid ? reinsert_entries_n : TI->size;
  return std::make_tuple(GetNodeInvalidStatus(T),
                         NodeContainInvalidSubtree(T),
                         GetNodeReinsertSize(T));
}

template<typename TT>
void
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
    return;
  }

  assert(NodeIsValid(T));

  if (T->is_leaf) { // do nothing for a valid leaf
    assert(reinsert_entries.size() == 0);
    return;
  }

  auto TI = static_cast<Interior*>(T);

  if (!NodeContainInvalidSubtree(T) && !GetNodeReinsertSize(T)) {
    // no subtree is invalid, fully valid
    assert(TI->valid);
    assert(reinsert_entries.size() == 0);
    return;
  }

  if (SelfBalanceNode(T, has_tomb)) { // so far a good tree
    assert(NodeIsValid(T));
    if (GetNodeReinsertSize(T) == 0) {
      assert(NodeContainInvalidSubtree(T));
      PruneOutEmptyTree(T);
      TI->ResetDeletionFlag();
      return;
    }

    BalanceOverLeaves(T, TI->reinsert_size);
    TI->ResetDeletionFlag();
    return;
  }

  // PARA: this node is valid, but contains invalid subtree
  typename Interior::NodeArr invalid_subtree;
  assert(invalid_subtree.size() == TT::kBufUB);
  BufSizeT invalid_idx = 0;
  BufSizeT tree_node_idx = 0;
  BufSizeT split_idx = 0;

  bool reinsert_flag = false; // whether we will reinsert in this node
  for (BufSizeT i = 0; i < TI->GetNodeN(); ++i) {
    auto node = TI->tree_nodes[i];

    // this node contain invalid subtree
    if (NodeContainInvalidSubtree(node)) {
      invalid_subtree[invalid_idx] = node;
      invalid_idx++;
    }

    if (!reinsert_flag && GetNodeInvalidStatus(node)) {
      reinsert_flag = true;
    }

    // check if this child node contain no invalid subtree, then itself must
    // be valid
    assert(NodeContainInvalidSubtree(node) == 0 ? NodeIsValid(node) : true);
    assert(NodeIsValid(node) ? 1 : NodeContainInvalidSubtree(node));

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
    }
  }
  assert(TI->node_n >= invalid_idx);
  TI->node_n = tree_node_idx;

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
      FetchInvalidSubtreeNodes(invalid_subtree[i],
                               reinsert_entries.cut(reinsert_sum[i],
                                                    i == invalid_idx - 1
                                                      ? reinsert_entries.size()
                                                      : reinsert_sum[i + 1]),
                               next_tomb);
    },
    1);

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
}

template<typename TT>
void
BTree<TT>::BalanceOverLeaves(Node* T, const SizeT reinsert_size)
{
  auto TI = static_cast<Interior*>(T);

  // ── Phase 1: Group assignment ──────────────────────────────────────────────
  // Each child is assigned to the nearest valid node on its right (its pivot).
  // group_sum[pivot] accumulates the total entry count for that group.
  // Invalid nodes right of all valid nodes are handled in the fix-up loop.
  BufSizeBufUBArr tree_group;
  SizeBufUBArr group_sum;
  const BufSizeT rightmost_pivot_idx = [&]() -> BufSizeT {
    BufSizeT rpidx = TT::kBufUB;
    BufSizeT pivot = TT::kBufUB;

    // WARN: signed; --i from 0 is safe
    for (int i = TI->GetNodeN() - 1; i >= 0; --i) {
      const SizeT sz = TI->tree_nodes[i]->size;
      if (NodeIsValid(TI->tree_nodes[i])) {
        if (rpidx == TT::kBufUB) {
          rpidx = i;
        }
        pivot = i;
        tree_group[i] = i;
        group_sum[i] = sz;
      } else {
        tree_group[i] = pivot;
        if (pivot != TT::kBufUB) {
          group_sum[pivot] += sz;
        }
      }
    }
    assert(rpidx != TT::kBufUB);
    for (BufSizeT i = rpidx + 1; i < TI->GetNodeN(); ++i) {
      tree_group[i] = rpidx;
      group_sum[rpidx] += TI->tree_nodes[i]->size;
    }
    return rpidx;
  }();

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

  // ── Phase 2: Compact and redistribute ─────────────────────────────────────
  // Results written directly into TI's arrays.
  // Safety: tree_idx ≤ i holds throughout, so writes never clobber unread data.
  typename TT::EntrySeq
    entry_buf; // heap-allocated lazily; only if distribute fires
  BufSizeT tree_idx = 0;
  BufSizeT split_idx = 0;

  // Validates that the last-emitted node falls inside its expected key range.
  // Called only from assert(), so has no cost in release builds.
  auto quick_check = [&](BufSizeT g) -> bool {
    const auto box = GetNodeBoxRec(TI->tree_nodes[tree_idx - 1]);
    if (tree_idx == 1) {
      return box.second < TI->split[split_idx - 1];
    }
    if (g == rightmost_pivot_idx) {
      return box.first >= TI->split[split_idx - 1];
    }
    return box.first >= TI->split[split_idx - 2] &&
           box.second < TI->split[split_idx - 1];
  };

  // Case A: group is a single valid node — slide its pointer/split into place.
  auto handle_single = [&](BufSizeT i, BufSizeT g, bool is_rightmost) {
    assert(NodeIsValid(TI->tree_nodes[i]));
    TI->tree_nodes[tree_idx++] = TI->tree_nodes[i];
    if (!is_rightmost) {
      TI->split[split_idx++] = TI->split[i];
    }
    assert(quick_check(g));
  };

  // Case B: group total fits in one leaf — merge all into the rightmost leaf.
  auto handle_merge =
    [&](BufSizeT i, BufSizeT j, BufSizeT g, bool is_rightmost) {
      auto* dest = static_cast<Leaf*>(TI->tree_nodes[j - 1]);
      assert(NodeIsValid(dest) || is_rightmost);
      auto* dst = dest->GetEleEndIter();
      for (BufSizeT k = i; k < j - 1; ++k) {
        auto* leaf = static_cast<Leaf*>(TI->tree_nodes[k]);
        assert(GetNodeInvalidStatus(leaf) || is_rightmost);
        dst = std::copy_n(leaf->GetEleBeginIter(), leaf->size, dst);
        FreeNode<Leaf>(leaf);
      }
      dest->size = static_cast<SizeT>(dst - dest->GetEleBeginIter());
      // TODO: try sort it as well
      dest->is_sorted = false;
      TI->tree_nodes[tree_idx++] = dest;
      if (!is_rightmost) {
        TI->split[split_idx++] = TI->split[j - 1];
      }
      assert(quick_check(g));
    };

  // Case C: group must be redistributed — gather all entries, sort, scatter
  // into evenly-sized buckets while reusing existing leaf node objects.
  auto handle_distribute = [&](BufSizeT i, BufSizeT j, bool is_rightmost) {
    if (entry_buf.empty()) [[unlikely]] {
      entry_buf = TT::EntrySeq::uninitialized(reinsert_size + TT::kBufUB);
    }

    auto* ep = entry_buf.begin();
    for (BufSizeT k = i; k < j; ++k) {
      auto* leaf = static_cast<Leaf*>(TI->tree_nodes[k]);
      ep = std::copy_n(leaf->GetEleBeginIter(), leaf->size, ep);
    }
    const auto item_n = static_cast<SizeT>(ep - entry_buf.begin());

    std::sort(entry_buf.begin(),
              entry_buf.begin() + item_n,
              [](auto const& a, auto const& b) {
                return Entry::GetKey(a) < Entry::GetKey(b);
              });

    const BufSizeT bkt_n = GetBktNum(item_n);
    assert(bkt_n <= j - i);
    auto* sp = entry_buf.begin();
    SizeT ele_l = 0;
    for (BufSizeT k = 0; k < bkt_n; ++k) {
      const SizeT ele_r =
        (k == bkt_n - 1) ? item_n : GetBktEndIdx(k, bkt_n, item_n);
      const SizeT bkt_size = ele_r - ele_l;
      auto* leaf = static_cast<Leaf*>(TI->tree_nodes[i + k]);
      std::copy_n(sp, bkt_size, leaf->GetEleBeginIter());
      sp += bkt_size;
      leaf->size = bkt_size;
      leaf->is_sorted = true;
      TI->tree_nodes[tree_idx++] = leaf;
      if (k < bkt_n - 1) {
        TI->split[split_idx++] = Entry::GetKey(*sp);
      }
      ele_l = ele_r;
    }
    if (!is_rightmost) {
      TI->split[split_idx++] = TI->split[j - 1];
    }
    for (BufSizeT k = i + bkt_n; k < j; ++k) {
      FreeNode<Leaf>(TI->tree_nodes[k]);
    }
  };

  // ── Dispatch loop ──────────────────────────────────────────────────────────
  for (BufSizeT i = 0; i < TI->GetNodeN();) {
    const BufSizeT g = tree_group[i];
    const bool is_rightmost = (g == rightmost_pivot_idx);
    const SizeT sum = group_sum[g];
    BufSizeT j = i + 1;
    while (j < TI->GetNodeN() && tree_group[j] == g) {
      ++j;
    }

    if (j == i + 1) [[likely]] {
      handle_single(i, g, is_rightmost);
    } else if (sum < TT::kBufUB) {
      handle_merge(i, j, g, is_rightmost);
    } else {
      handle_distribute(i, j, is_rightmost);
    }

    i = j;
  }

  assert(tree_idx == split_idx + 1);
  TI->node_n = tree_idx;
  TI->ResetDeletionFlag();

#ifdef DEBUG
  for (int i = 0; i < TI->GetNodeN(); i++) {
    assert(TI->tree_nodes[i] != nullptr);
    if (TI->GetNodeN() == 1) {
      continue;
    }
    const auto box = GetNodeBoxRec(TI->tree_nodes[i]);
    if (i == 0) {
      assert(box.second < TI->split[0]);
    } else if (i == TI->GetNodeN() - 1) {
      assert(box.first >= TI->split[TI->GetSplitN() - 1]);
    } else {
      assert(box.first >= TI->split[i - 1] && box.second < TI->split[i]);
    }
  }
#endif // DEBUG
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

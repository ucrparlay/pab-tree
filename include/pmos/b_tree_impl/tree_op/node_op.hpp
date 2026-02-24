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
                                 TT::HeightT cur_height,
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
      AllocInteriorNode<Interior, TT>(
        node_in.cut(node_l, node_r), key_in.cut(key_l, key_r), cur_height));
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
  TI->box = GetNodeSliceBox(TI->GetNodeSlice());

#ifdef DEBUG
  for (int i = 0; i < TI->GetNodeN(); i++) {
    assert(TI->tree_nodes[i] != nullptr);
    if (TI->GetNodeN() == 1) {
      continue;
    }
    auto node = TI->tree_nodes[i];
    auto box = GetNodeBoxRec(TI->tree_nodes[i]);
    assert(node->is_leaf || box == GetNodeBox(node));
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
  auto cur_height = TI->height;
  FreeNode<Interior>(T);
  [[maybe_unused]] SizeT rt_bkt_n = GenerateInteriorNodes(
    parlay::make_slice(flatten_node_seq),
    parlay::make_slice(flatten_split_seq),
    cur_height,
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
  TI->valid = T != this->root_ ? (TI->GetNodeN() - invalid_tree_n) >= TT::kBufLB
                               : (TI->GetNodeN() - invalid_tree_n) > 0;
  TI->box = GetNodeSliceBox(TI->GetNodeSlice());
  TI->invalid_subtree_num = TI->valid ? tot_invalid_subtree_n : 1;
  TI->reinsert_size = TI->valid ? reinsert_entries_n : TI->size;
  return std::make_tuple(
    !TI->valid, TI->invalid_subtree_num, TI->reinsert_size);
}

template<typename TT>
TT::SizeT
BTree<TT>::FetchInvalidSubtreeNodes(Node* T,
                                    TT::NodeSlice flatten_node_slice,
                                    EntrySlice reinsert_entries,
                                    const bool has_tomb)
{
  if (T->is_leaf) {
    assert(GetNodeInvalidStatus(T));
    assert(flatten_node_slice.size() == 1);
    assert(reinsert_entries.size() == T->size);
    flatten_node_slice[0] = T;
    SizeT size = T->size;
    FlattenRec<EntrySlice, true, true>(T, reinsert_entries);
    return size;
  }

  auto TI = static_cast<Interior*>(T);
  assert(TI->valid || TI->invalid_subtree_num > 0);
  if (TI->valid) {                      // find a valid subtree
    if (TI->invalid_subtree_num == 0) { // no subtree is invalid
      return 0;
    } else { // contain a invalid subtree
      typename Interior::NodeArr nodes_contain_invalid_subtree;
      assert(nodes_contain_invalid_subtree.size() == TT::kBufUB);
      BufSizeT invalid_idx = 0;
      BufSizeT tree_node_idx = 0;
      BufSizeT split_idx = 0;

      bool reinsert_flag = false;
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

        assert(GetNodeInvalidSubtreeNum(node) == 0 ? !GetNodeInvalidStatus(node)
                                                   : true);

        if (!GetNodeInvalidSubtreeNum(node) || !GetNodeInvalidStatus(node)) {
          const bool has_split = i < TI->GetSplitN();
          if (i != tree_node_idx) {
            TI->tree_nodes[tree_node_idx] = node;
            if (has_split) {
              TI->split[split_idx] = TI->split[i];
            }
          }
          ++tree_node_idx;
          split_idx += has_split;
        }
      }

      assert(TI->node_n >= invalid_idx);
      TI->node_n = tree_node_idx;

      SizeBufUBArr remove_entries_num;

      parlay::parallel_for(
        0,
        invalid_idx,
        [&](BufSizeT i) {
          SizeT subtree_sum = 0;
          SizeT reinsert_sum = 0;
          for (BufSizeT j = 0; j < i; j++) {
            subtree_sum +=
              GetNodeInvalidSubtreeNum(nodes_contain_invalid_subtree[j]);
            reinsert_sum +=
              GetNodeReinsertSize(nodes_contain_invalid_subtree[j]);
          }

          remove_entries_num[i] = FetchInvalidSubtreeNodes(
            nodes_contain_invalid_subtree[i],
            flatten_node_slice.cut(
              subtree_sum,
              subtree_sum +
                GetNodeInvalidSubtreeNum(nodes_contain_invalid_subtree[i])),
            reinsert_entries.cut(
              reinsert_sum,
              reinsert_sum +
                GetNodeReinsertSize(nodes_contain_invalid_subtree[i])),
            has_tomb ? !reinsert_flag : false);
        },
        1);

      // for (int i = 0; i < invalid_idx; i++) {
      //   auto node = nodes_contain_invalid_subtree[i];
      //   if (node->is_leaf && GetNodeInvalidStatus(node)) {
      //     FreeNode<Leaf>(node);
      //   }
      // }
      SizeT total_remove_entries_n =
        std::accumulate(remove_entries_num.begin(),
                        remove_entries_num.begin() + invalid_idx,
                        static_cast<SizeT>(0));
      assert(total_remove_entries_n == reinsert_entries.size());
      assert(parlay::is_sorted(reinsert_entries, [](auto a, auto b) {
        return Entry::GetKey(a) < Entry::GetKey(b);
      }));
      assert(parlay::all_of(flatten_node_slice, [&](auto node) {
        return VerifyUnoverlappedTree(node, flatten_node_slice);
      }));

      TI->box = GetNodeSliceBox(TI->GetNodeSlice());
      if (reinsert_flag && has_tomb) {
        TI->size -= total_remove_entries_n;
        // parlay::write_max(&this->max_reinsert_batch_size,
        //                   reinsert_entries.size(),
        //                   std::less<size_t>());
        BatchInsert_(parlay::make_slice(reinsert_entries), true);
      } else { // the subtrees may perform reinsertion
        TI->size = std::accumulate(
          TI->tree_nodes.begin(),
          TI->tree_nodes.begin() + TI->GetNodeN(),
          static_cast<SizeT>(0),
          [](SizeT sum, Node* node) { return sum + node->size; });
      }

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

      return total_remove_entries_n;
    }
  } else { // find a invalid tree
    assert(flatten_node_slice.size() == 1);
    assert(TI->invalid_subtree_num == 1);
    assert(reinsert_entries.size() == TI->size);
    flatten_node_slice[0] = T;
    SizeT size = T->size;
    FlattenRec<decltype(reinsert_entries), true, true>(T, reinsert_entries);
    // DeleteTreeRec(T);
    return size;
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

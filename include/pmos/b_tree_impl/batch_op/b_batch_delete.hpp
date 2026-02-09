#ifndef PMOS_B_TREE_IMPL_BATCH_OP_B_Batch_DELETE_HPP_
#define PMOS_B_TREE_IMPL_BATCH_OP_B_Batch_DELETE_HPP_

#include "../../b_tree.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/slice.h"

namespace pmos {
template<typename TT>
template<typename Range>
void
BTree<TT>::BatchDelete(Range&& In)
{
  static_assert(parlay::is_random_access_range_v<Range>);
  static_assert(
    parlay::is_less_than_comparable_v<parlay::range_reference_type_t<Range>>);
  static_assert(std::is_constructible_v<parlay::range_value_type_t<Range>,
                                        parlay::range_reference_type_t<Range>>);

  auto A = parlay::make_slice(std::forward<Range>(In));

  BatchDelete_(A);
}

template<typename TT>
void
BTree<TT>::BatchDelete_(TT::KeySlice In)
{
  if (this->root_ == nullptr) {
    return;
  }
  // parlay::internal::timer t("BatchInsert_Sort");
  auto sorted_seq = parlay::sort(In);
  // t.next("Sorted");

  auto invalid_subtree_num =
    BatchDeleteRecursive(this->root_, parlay::make_slice(sorted_seq));

  if (invalid_subtree_num) {
    auto flatten_node_seq = TT::NodeSeq::uninitialized(invalid_subtree_num);
    auto reinsert_entry_num = FetchInvalidSubtreeNodes(
      this->root_, parlay::make_slice(flatten_node_seq));
    auto reinsert_entries = TT::EntrySeq::uninitialized(reinsert_entry_num);
    parlay::parallel_for(
      0,
      invalid_subtree_num,
      [&](SizeT i) {
        SizeT sum = 0;
        for (int j = 0; j < i; j++) {
          sum += flatten_node_seq[j]->size;
        }
        FlattenRec(flatten_node_seq[i],
                   reinsert_entries.cut(sum, sum + flatten_node_seq[i]->size));
        DeleteTreeRec(flatten_node_seq[i]);
      },
      1);
    BatchInsert_(parlay::make_slice(reinsert_entries));
  }
}

// PARA: return whether T and all its subtree is valid
// Only needs to ensure the tree size is correct
template<typename TT>
TT::SizeT
BTree<TT>::BatchDeleteRecursive(Node* T, TT::KeySlice In)
{

  if (In.size() == 0) {
    // stop here, than all subtree is valid
    return 0;
  }

  if (T->is_leaf) {
    [[maybe_unused]] auto delete_cnt = DeletePoints4Leaf(T, In);
    assert(delete_cnt == In.size());
    return In.size() >= TT::kBufLB ? 0 : 1;
  }

  auto TI = static_cast<Interior*>(T);

  SizeBufUBArr split_pos_seq;
  if (In.size() > TT::kSplitFactor * TI->GetSplitN()) {
    SplitBatchBinarySearch(split_pos_seq, In, TI->GetSplitSlice());
  } else {
    SplitBatchMerge(split_pos_seq, In, TI->GetSplitSlice());
  }

#ifdef DEBUG
  // Ensure all elements between two splits are correct
  // i.e., less than the split value
  for (int i = 1; i < TI->GetSplitN(); i++) {
    assert(parlay::all_of(
      In.cut(split_pos_seq[i - 1], split_pos_seq[i]),
      [&](K const k) { return k >= TI->split[i - 1] && k < TI->split[i]; }));
  }
#endif // DEBUG

  SizeBufUBArr invalid_subtree_num;
  BufSizeBufUBArr nodes_map;
  BufSizeT cnt = 0;
  for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
    SizeT l = (i == 0 ? 0 : split_pos_seq[i - 1]);
    SizeT r = (i == TI->GetNodeN() - 1 ? In.size() : split_pos_seq[i]);
    if (l != r) {
      nodes_map[cnt++] = i;
    } else {
      invalid_subtree_num[i] = 0; // no element in this subtree, so it's valid
    }
  }

  parlay::parallel_for(
    0,
    cnt,
    [&](BufSizeT i) {
      SizeT l = nodes_map[i] == 0 ? 0 : split_pos_seq[nodes_map[i] - 1];
      SizeT r = nodes_map[i] == TI->GetNodeN() - 1
                  ? In.size()
                  : split_pos_seq[nodes_map[i]];
      invalid_subtree_num[nodes_map[i]] =
        BatchDeleteRecursive(TI->tree_nodes[nodes_map[i]], In.cut(l, r));
    },
    // In.size() > 2048 ? 1 : cnt);
    1);

  return BatchDeleteUpdateInteriorSize(T, invalid_subtree_num);
}

} // namespace pmos

#endif // PMOS_B_TREE_IMPL_BATCH_OP_B_Batch_DELETE_HPP_

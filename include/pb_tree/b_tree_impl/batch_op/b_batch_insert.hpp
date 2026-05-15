#ifndef PBTREE_B_TREE_IMPL_BATCH_OP_B_BATCH_INSERT_HPP_
#define PBTREE_B_TREE_IMPL_BATCH_OP_B_BATCH_INSERT_HPP_

#include <algorithm>
#include <numeric>

#include "../../b_tree.h"
#include "dependence/type_trait.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/slice.h"

namespace pb_tree {
template<typename TT>
template<typename Range>
void
BTree<TT>::BatchInsert(Range&& In)
{
  static_assert(parlay::is_random_access_range_v<Range>);
  static_assert(
    parlay::is_less_than_comparable_v<parlay::range_reference_type_t<Range>>);
  static_assert(std::is_constructible_v<parlay::range_value_type_t<Range>,
                                        parlay::range_reference_type_t<Range>>);

  auto A = parlay::make_slice(std::forward<Range>(In));

  constexpr auto leaf_entries_pattern = TT::LeafEntriesPattern::kHybrid;
  constexpr bool sorted_input = false;
  BatchInsert_(A, leaf_entries_pattern, sorted_input);
}

template<typename TT>
template<typename Range>
void
BTree<TT>::BatchInsert(Range&& In,
                       const TT::PatternType leaf_entries_pattern,
                       const bool sorted_input)
{
  static_assert(parlay::is_random_access_range_v<Range>);
  static_assert(
    parlay::is_less_than_comparable_v<parlay::range_reference_type_t<Range>>);
  static_assert(std::is_constructible_v<parlay::range_value_type_t<Range>,
                                        parlay::range_reference_type_t<Range>>);

  auto A = parlay::make_slice(std::forward<Range>(In));

  BatchInsert_(A, leaf_entries_pattern, sorted_input);
}

template<typename TT>
template<typename Range>
void
BTree<TT>::BatchInsert_(Range In,
                        const TT::PatternType leaf_entries_pattern,
                        const bool sorted_input)
{
  if (this->root_ == nullptr) { // TODO: may check using explicity tag
    return Build_(In);
  }

  if (this->root_->size == 0) { // empty ghost node should be eliminated
    this->DeleteTree();
    return Build_(In);
  }

  parlay::slice<const E*, const E*> sorted_slice{ In.begin(), In.end() };
  typename TT::EntrySeq sorted_seq;
  if (!sorted_input) {
    sorted_seq = parlay::sort(In);
    sorted_slice = parlay::make_slice(sorted_seq.cbegin(), sorted_seq.cend());
  }

  this->sys_split_leaf_size = 0;

  // NOTE: begin insert
  NodeSplitPairSeq node_split_seq =
    NodeSplitPairSeq::uninitialized(2 * sorted_slice.size());
  SizeT new_node_n = BatchInsertRecursive(this->root_,
                                          sorted_slice,
                                          parlay::make_slice(node_split_seq),
                                          leaf_entries_pattern);

#ifdef DEBUG
  assert(!sorted_input || new_node_n == 0);
#endif // DEBUG

  if (new_node_n > 0) {
    auto updated_node_seq = parlay::tabulate(
      new_node_n, [&](SizeT i) { return node_split_seq[i].first; });
    auto updated_split_seq = parlay::tabulate(
      new_node_n - 1, [&](SizeT i) { return node_split_seq[i].second; });

    this->root_ = AssembleInteriorLevels(parlay::make_slice(updated_node_seq),
                                         parlay::make_slice(updated_split_seq));
  }

  std::cout << "split_leaf_size: " << this->sys_split_leaf_size << std::endl;
}

template<typename TT>
template<typename Range>
TT::SizeT
BTree<TT>::BatchInsertRecursive(Node* T,
                                Range In,
                                NodeSplitPairSlice node_split_slice,
                                const TT::PatternType leaf_entries_pattern)
{
  assert(2 * In.size() == node_split_slice.size());

  SizeT n = In.size();
  if (n == 0) {
    return 0;
  }

  if (T->is_leaf) {
    Leaf* TL = static_cast<Leaf*>(T);
    if (TL->size + n <= TT::kBufUB) {
      Node* updated_node = InsertPoints2Leaf(T, In, leaf_entries_pattern);
      assert(updated_node == T);
      return 0;
    }

    this->sys_split_leaf_size += T->size;

    return HandlingLeafSplitOverflow(T, In, node_split_slice);
  }

  auto TI = static_cast<Interior*>(T);

  SizeBufUBArr split_pos_seq;
  assert(TI->GetNodeN() > 0);
  if (In.size() > TT::kSplitFactor * TI->GetSplitN()) {
    SplitBatchBinarySearch(split_pos_seq, In, TI->GetSplitSlice());
  } else {
    SplitBatchMerge(split_pos_seq, In, TI->GetSplitSlice());
  }

  // if (In.size() < TT::kSplitFactor * TI->GetSplitN()) {
  //   SplitBatchMerge(split_pos_seq, In, TI->GetSplitSlice());
  //   // SplitBatchSieve(split_pos_seq, In, TI->GetSplitSlice());
  // } else {
  //   SplitBatchBinarySearch(split_pos_seq, In, TI->GetSplitSlice());
  // }
#ifdef DEBUG
  for (int i = 1; i < TI->GetSplitN(); i++) {
    assert(parlay::all_of(In.cut(split_pos_seq[i - 1], split_pos_seq[i]),
                          [&](E const e) {
                            return Entry::GetKey(e) >= TI->split[i - 1] &&
                                   Entry::GetKey(e) < TI->split[i];
                          }));
  }
#endif // DEBUG

  SizeBufUBArr new_split_node_n_arr;
  BufSizeBufUBArr nodes_map;
  BufSizeT cnt = 0;
  for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
    SizeT l = (i == 0 ? 0 : split_pos_seq[i - 1]);
    SizeT r = (i == TI->GetNodeN() - 1 ? n : split_pos_seq[i]);
    if (l != r) {
      nodes_map[cnt++] = i;
    } else {
      new_split_node_n_arr[i] = 0; // i-th child remains the same
    }
  }

  parlay::parallel_for(
    0,
    cnt,
    [&](BufSizeT i) {
      SizeT l = nodes_map[i] == 0 ? 0 : split_pos_seq[nodes_map[i] - 1];
      SizeT r =
        nodes_map[i] == TI->GetNodeN() - 1 ? n : split_pos_seq[nodes_map[i]];
      new_split_node_n_arr[nodes_map[i]] =
        BatchInsertRecursive(TI->tree_nodes[nodes_map[i]],
                             In.cut(l, r),
                             node_split_slice.cut(2 * l, 2 * r),
                             leaf_entries_pattern);
    },
    // In.size() > 2048 ? 1 : cnt);
    1);

  // count only over touched children: start from GetNodeN() (each contributes
  // 1) and add the excess for children that split (return value is 0 or >=2,
  // never 1).
  SizeT new_node_n = TI->GetNodeN();
  for (BufSizeT i = 0; i < cnt; i++) {
    SizeT split_n = new_split_node_n_arr[nodes_map[i]];
    if (split_n > 0) {
      new_node_n += split_n - 1;
    }
  }

  // the interior node can be updated without split
  if (new_node_n <= TT::kBufUB) {
    [[maybe_unused]] Node* new_inter_node = BatchInsertUpdateInterior(
      T, node_split_slice, split_pos_seq, new_node_n, new_split_node_n_arr);
    return 0;
  }

  // handle interior node split overflow
  return HandlingInteriorSplitOverflow(
    T, node_split_slice, split_pos_seq, new_node_n, new_split_node_n_arr);
}

} // namespace pb_tree

#endif // PBTREE_B_TREE_IMPL_BATCH_OP_B_BATCH_INSERT_HPP_

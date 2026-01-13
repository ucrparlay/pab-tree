#ifndef PMOS_B_TREE_IMPL_BATCH_OP_B_BATCH_INSERT_HPP_
#define PMOS_B_TREE_IMPL_BATCH_OP_B_BATCH_INSERT_HPP_

#include <numeric>

#include "../../b_tree.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/slice.h"

namespace pmos {
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

  EntrySlice A = parlay::make_slice(std::forward<Range>(In));
  BatchInsert_(A);
}

template<typename TT>
void
BTree<TT>::BatchInsert_(EntrySlice In)
{
  if (this->root_ == nullptr) { // TODO: may check using explicity tag
    return Build_(In);
  }

  auto sorted_seq = parlay::sort(In);

  NodeSplitPairSeq node_split_seq = NodeSplitPairSeq::uninitialized(In.size());
  SizeT new_node_n = BatchInsertRecursive(this->root_,
                                          parlay::make_slice(sorted_seq),
                                          parlay::make_slice(node_split_seq));

  if (new_node_n > 0) {
    auto updated_node_seq = parlay::tabulate(
      new_node_n, [&](SizeT i) { return node_split_seq[i].first; });
    auto updated_split_seq = parlay::tabulate(
      new_node_n - 1, [&](SizeT i) { return node_split_seq[i].second; });
    this->root_ =
      AllocInteriorNode<Interior, TT>(updated_node_seq, updated_split_seq);
  }
}

template<typename TT>
TT::SizeT
BTree<TT>::BatchInsertRecursive(Node* T,
                                EntrySlice In,
                                NodeSplitPairSlice node_split_slice)
{
  assert(In.size() == node_split_slice.size());

  SizeT n = In.size();
  if (n == 0) {
    return 0;
  }

  if (T->is_leaf) {
    Leaf* TL = static_cast<Leaf*>(T);
    if (TL->size + n <= TT::kBufUB) {
      [[maybe_unused]] Node* updated_node = InsertPoints2Leaf(T, In);
      assert(updated_node == T);
      return 0;
    }
    return HandlingLeafSplitOverflow(T, In, node_split_slice);
  }

  auto TI = static_cast<Interior*>(T);

  std::array<SizeT, TT::kBufUB> split_pos_seq;
  // PartitionBinarySearchRec(TI->GetSplitSlice(), split_pos_seq, In, 0, 0);
  // TODO: maybe we can decide it by skewed distribution
  SizeT offset = 0;
  for (int i = 0; i < TI->GetSplitN(); i++) {
    // split_pos_seq[i] = In.size() / TI->split.size() * (i + 1);
    SizeT pos = parlay::internal::binary_search(
      In.cut(i == 0 ? 0 : split_pos_seq[i - 1], In.size()),
      [&](E const e) { return Entry::GetKey(e) < TI->split[i]; });
    offset += pos;
    split_pos_seq[i] = offset;
  }

#ifdef DEBUG
  for (int i = 1; i < TI->GetSplitN(); i++) {
    assert(parlay::all_of(In.cut(split_pos_seq[i - 1], split_pos_seq[i]),
                          [&](E const e) {
                            return Entry::GetKey(e) >= TI->split[i - 1] &&
                                   Entry::GetKey(e) < TI->split[i];
                          }));
  }
#endif // DEBUG

  std::array<SizeT, TT::kBufUB> new_split_node_n_arr;
  std::array<SizeT, TT::kBufUB> nodes_map;
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
      // std::tie(node_seq_list[nodes_map[i]], split_seq_list[nodes_map[i]]) =
      new_split_node_n_arr[nodes_map[i]] = BatchInsertRecursive(
        TI->tree_nodes[nodes_map[i]], In.cut(l, r), node_split_slice.cut(l, r));
    },
    1);

  SizeT new_node_n = 0;
  for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
    new_node_n += new_split_node_n_arr[i] == 0 ? 1 : new_split_node_n_arr[i];
  }
  if (new_node_n <= TT::kBufUB) {
    [[maybe_unused]] Node* new_inter_node = UpdateInterior(
      T, node_split_slice, split_pos_seq, new_node_n, new_split_node_n_arr);
    return 0;
  }
  return HandlingInteriorSplitOverflow(
    T, node_split_slice, split_pos_seq, new_node_n, new_split_node_n_arr);
}

} // namespace pmos

#endif // PMOS_B_TREE_IMPL_BATCH_OP_B_BATCH_INSERT_HPP_

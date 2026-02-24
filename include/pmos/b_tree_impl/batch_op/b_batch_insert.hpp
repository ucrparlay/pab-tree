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

#ifndef SIEVE
  BatchInsert_(A);
#else
  BatchInsertSieve(A);
#endif // !SIEVE
}

template<typename TT>
void
BTree<TT>::BatchInsert_(EntrySlice In, bool sorted)
{
  if (this->root_ == nullptr) { // TODO: may check using explicity tag
    return Build_(In);
  }

  if (this->root_->size == 0) { // empty ghost node should be eliminated
    this->DeleteTree();
    return Build_(In);
  }

  EntrySlice sorted_slice = In;
  if (!sorted) {
    auto sorted_seq = parlay::sort(In);
    sorted_slice = parlay::make_slice(sorted_seq);
  }

  NodeSplitPairSeq node_split_seq = NodeSplitPairSeq::uninitialized(In.size());
  auto cur_height =
    this->root_->is_leaf ? 1 : static_cast<Interior*>(this->root_)->height;
  SizeT new_node_n = BatchInsertRecursive(
    this->root_, sorted_slice, parlay::make_slice(node_split_seq));

  if (new_node_n > 0) {
    auto updated_node_seq = parlay::tabulate(
      new_node_n, [&](SizeT i) { return node_split_seq[i].first; });
    auto updated_split_seq = parlay::tabulate(
      new_node_n - 1, [&](SizeT i) { return node_split_seq[i].second; });
    this->root_ = AllocInteriorNode<Interior, TT>(
      updated_node_seq, updated_split_seq, cur_height + 1);
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
      new_split_node_n_arr[nodes_map[i]] = BatchInsertRecursive(
        TI->tree_nodes[nodes_map[i]], In.cut(l, r), node_split_slice.cut(l, r));
    },
    // In.size() > 2048 ? 1 : cnt);
    1);

  // compute the number of new nodes
  SizeT new_node_n = 0;
  for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
    new_node_n += new_split_node_n_arr[i] == 0 ? 1 : new_split_node_n_arr[i];
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

// NOTE: Start as sieve
template<typename TT>
void
BTree<TT>::BatchInsertSieve(EntrySlice In)
{
  if (this->root_ == nullptr) { // TODO: may check using explicity tag
    return Build_(In);
  }

  auto aux_entry_seq = TT::EntrySeq::uninitialized(In.size());
  NodeSplitPairSeq node_split_seq = NodeSplitPairSeq::uninitialized(In.size());
  SizeT new_node_n =
    BatchInsertSieveRecursive(this->root_,
                              In,
                              parlay::make_slice(aux_entry_seq),
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

// NOTE: Start as sieve
template<typename TT>
TT::SizeT
BTree<TT>::BatchInsertSieveRecursive(Node* T,
                                     EntrySlice In,
                                     EntrySlice Out,
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

  // SizeBufUBArr split_pos_seq;
  // // PartitionBinarySearchRec(TI->GetSplitSlice(), split_pos_seq, In, 0, 0);
  // // TODO: maybe we can decide it by skewed distribution
  // SizeT offset = 0;
  // for (int i = 0; i < TI->GetSplitN(); i++) {
  //   // split_pos_seq[i] = In.size() / TI->split.size() * (i + 1);
  //   SizeT pos = parlay::internal::binary_search(
  //     In.cut(i == 0 ? 0 : split_pos_seq[i - 1], In.size()),
  //     [&](E const e) { return Entry::GetKey(e) < TI->split[i]; });
  //   offset += pos;
  //   split_pos_seq[i] = offset;
  // }

  SizeBufUBArr split_pos_seq;

  if (n <= 2048) {
    std::sort(In.begin(), In.end());
    SizeT offset = 0;
    for (int i = 0; i < TI->GetSplitN(); i++) {
      // split_pos_seq[i] = In.size() / TI->split.size() * (i + 1);
      SizeT pos = parlay::internal::binary_search(
        In.cut(i == 0 ? 0 : split_pos_seq[i - 1], In.size()),
        [&](E const e) { return Entry::GetKey(e) < TI->split[i]; });
      offset += pos;
      split_pos_seq[i] = offset;
    }
    std::ranges::swap_ranges(In, Out);
  } else {
    auto sums = SeievePoints(In, Out, TI->GetSplitSlice());
    assert(sums.size() == TI->GetNodeN());
    assert(std::accumulate(sums.begin(), sums.end(), 0) == n);

    SizeT offset = 0;
    for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
      offset += sums[i];
      split_pos_seq[i] = offset;
    }
  }

#ifdef DEBUG
  for (int i = 1; i < TI->GetSplitN(); i++) {
    assert(parlay::all_of(Out.cut(split_pos_seq[i - 1], split_pos_seq[i]),
                          [&](E const e) {
                            return Entry::GetKey(e) >= TI->split[i - 1] &&
                                   Entry::GetKey(e) < TI->split[i];
                          }));
  }
#endif // DEBUG

  SizeBufUBArr new_split_node_n_arr;
  SizeBufUBArr nodes_map;
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

  if (n <= 2048) {
    for (SizeT i = 0; i < cnt; i++) {
      SizeT l = nodes_map[i] == 0 ? 0 : split_pos_seq[nodes_map[i] - 1];
      SizeT r =
        nodes_map[i] == TI->GetNodeN() - 1 ? n : split_pos_seq[nodes_map[i]];
      new_split_node_n_arr[nodes_map[i]] =
        BatchInsertSieveRecursive(TI->tree_nodes[nodes_map[i]],
                                  Out.cut(l, r),
                                  In.cut(l, r),
                                  node_split_slice.cut(l, r));
    }
  } else {
    parlay::parallel_for(
      0,
      cnt,
      [&](BufSizeT i) {
        SizeT l = nodes_map[i] == 0 ? 0 : split_pos_seq[nodes_map[i] - 1];
        SizeT r =
          nodes_map[i] == TI->GetNodeN() - 1 ? n : split_pos_seq[nodes_map[i]];
        new_split_node_n_arr[nodes_map[i]] =
          BatchInsertSieveRecursive(TI->tree_nodes[nodes_map[i]],
                                    Out.cut(l, r),
                                    In.cut(l, r),
                                    node_split_slice.cut(l, r));
      },
      1);
  }

  SizeT new_node_n = 0;
  for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
    new_node_n += new_split_node_n_arr[i] == 0 ? 1 : new_split_node_n_arr[i];
  }
  if (new_node_n <= TT::kBufUB) {
    [[maybe_unused]] Node* new_inter_node = BatchInsertUpdateInterior(
      T, node_split_slice, split_pos_seq, new_node_n, new_split_node_n_arr);
    return 0;
  }
  return HandlingInteriorSplitOverflow(
    T, node_split_slice, split_pos_seq, new_node_n, new_split_node_n_arr);
}
// template <typename TT>
// void BTree<TT>::BatchInsert_(EntrySlice In) {
//   if (this->root_ == nullptr) {  // TODO: may check using explicity tag
//     return Build_(In);
//   }
//
//   auto aux_entry_seq = TT::EntrySeq::uninitialized(In.size());
//   auto [updated_node_seq, split_seq] =
//       BatchInsertRecursive(this->root_, In,
//       parlay::make_slice(aux_entry_seq));
//
//   if (split_seq.size() == 0) {
//     this->root_ = updated_node_seq.front();
//   } else {
//     this->root_ = AllocInteriorNode<Interior, TT>(updated_node_seq,
//     split_seq);
//   }
//   return;
// }

// NOTE: return the updated Node and the split sequence
// template <typename TT>
// BTree<TT>::NodeSplitSeqPair BTree<TT>::BatchInsertRecursive(Node* T,
//                                                             EntrySlice In,
//                                                             EntrySlice Out)
//                                                             {
//   size_t n = In.size();
//
//   if (n == 0) return {{T}, typename TT::SplitSeq()};
//
//   if (T->is_leaf) {
//     Leaf* TL = static_cast<Leaf*>(T);
//     if (TL->size + n <= TT::kBufUB) {
//       return {{InsertPoints2Leaf(T, In)}, typename TT::SplitSeq()};
//     } else {
//       return HandlingLeafSplitOverflow(T, In);
//     }
//   }
//
//   Interior* TI = static_cast<Interior*>(T);
//
//   // PARA: helpers to finalize interior node after batch insert
//   auto finalize_interior =
//       [&](NodeSeqList& node_seq_list,
//           SplitSeqList& split_seq_list) -> NodeSplitSeqPair {
//     size_t new_node_n =
//         std::accumulate(node_seq_list.begin(), node_seq_list.end(), 0,
//                         [](size_t acc, TT::NodeSeq const& node_seq) ->
//                         size_t
//                         {
//                           return acc + node_seq.size();
//                         });
//     if (new_node_n <= TT::kBufUB) {
//       return {{UpdateInterior(T, node_seq_list, split_seq_list,
//       new_node_n)},
//               typename TT::SplitSeq()};
//     }
//     return HandlingInteriorSplitOverflow(T, node_seq_list, split_seq_list);
//   };
//
//   // NOTE: serial build when In is small
//   if (n <= kSerialBuildCutoff) {
//     // if (1) {
//     parlay::sort_inplace(In);
//
//     auto split_pos_seq = parlay::tabulate(TI->split.size(), [&](size_t i) {
//       // return index to first key where is less is false
//       return parlay::internal::binary_search(
//           In, [&](E const e) { return Entry::GetKey(e) < TI->split[i]; });
//     });
// #ifdef DEBUG
//     assert(split_pos_seq.size() == TI->split.size());
//     for (int i = 1; i < split_pos_seq.size(); i++) {
//       assert(parlay::all_of(In.cut(split_pos_seq[i - 1], split_pos_seq[i]),
//                             [&](E const e) {
//                               return Entry::GetKey(e) >= TI->split[i - 1]
//                               &&
//                                      Entry::GetKey(e) < TI->split[i];
//                             }));
//     }
// #endif  // DEBUG
//     auto node_seq_list = NodeSeqList(TI->GetNodeN());
//     auto split_seq_list = SplitSeqList(TI->GetNodeN());
//
//     for (int i = 0; i < TI->GetNodeN(); i++) {
//       std::tie(node_seq_list[i], split_seq_list[i]) = BatchInsertRecursive(
//           TI->tree_nodes[i],
//           In.cut(i == 0 ? 0 : split_pos_seq[i - 1],
//                  i == TI->GetNodeN() - 1 ? n : split_pos_seq[i]),
//           Out.cut(i == 0 ? 0 : split_pos_seq[i - 1],
//                   i == TI->GetNodeN() - 1 ? n : split_pos_seq[i]));
//     }
//
//     return finalize_interior(node_seq_list, split_seq_list);
//   }
//
//   auto sums = SeievePoints(In, Out, TI->split); assert(sums.size()
//   == TI->GetNodeN()); assert(std::accumulate(sums.begin(),
//   sums.end(), 0)
//   == n);
//
//   auto node_seq_list = NodeSeqList(TI->GetNodeN());
//   auto split_seq_list = SplitSeqList(TI->GetNodeN());
//   parlay::parallel_for(
//       0, TI->GetNodeN(),
//       [&](typename TT::BucketType i) {
//         size_t s = 0;
//         for (typename TT::BucketType j = 0; j < i; j++) {
//           s += sums[j];
//         }
//
//         std::tie(node_seq_list[i], split_seq_list[i]) =
//         BatchInsertRecursive(
//             TI->tree_nodes[i], Out.cut(s, s + sums[i]), In.cut(s, s +
//             sums[i]));
//       },
//       1);
//
//   return finalize_interior(node_seq_list, split_seq_list);
// }
//

} // namespace pmos

#endif // PMOS_B_TREE_IMPL_BATCH_OP_B_BATCH_INSERT_HPP_

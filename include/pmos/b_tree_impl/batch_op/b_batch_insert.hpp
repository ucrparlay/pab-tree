#ifndef PMOS_B_TREE_IMPL_BATCH_OP_B_BATCH_INSERT_HPP_
#define PMOS_B_TREE_IMPL_BATCH_OP_B_BATCH_INSERT_HPP_

#include <numeric>

#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/slice.h"
#include "pmos/b_tree.h"

namespace pmos {
template <typename TT>
template <typename Range>
void BTree<TT>::BatchInsert(Range&& In) {
  static_assert(parlay::is_random_access_range_v<Range>);
  static_assert(
      parlay::is_less_than_comparable_v<parlay::range_reference_type_t<Range>>);
  static_assert(std::is_constructible_v<parlay::range_value_type_t<Range>,
                                        parlay::range_reference_type_t<Range>>);

  EntrySlice A = parlay::make_slice(In);
  BatchInsert_(A);
  return;
}

template <typename TT>
void BTree<TT>::BatchInsert_(EntrySlice In) {
  if (this->root_ == nullptr) {  // TODO: may check using explicity tag
    return Build_(In);
  }

  // parlay::internal::timer t;
  // t.reset(), t.start();
  auto sorted_seq = parlay::sort(In);
  // t.next("sort");

  // insert_leaf_time = 0.0;
  // lower_bound_time = 0.0;
  // merge_time = 0.0;
  // recurse_time = 0.0;
  // vis_leaf_num = 0;
  // vis_inter_num = 0;
  // insert_leaf_timer.reset();
  // lower_bound_timer.reset();
  // merge_timer.reset();
  // t.reset(), t.start();

  NodeSplitPairSeq node_split_seq = NodeSplitPairSeq::uninitialized(In.size());
  SizeT new_node_n =
      BatchInsertRecursive(this->root_, parlay::make_slice(sorted_seq),
                           parlay::make_slice(node_split_seq));
  // t.next("batch insert");
  // std::cout << "Insert leaf time: " << insert_leaf_time
  //           << " lower bound time: " << lower_bound_time
  //           << " merge time: " << merge_time << std::endl;

  // std::cout << GetHeight<Leaf, Interior>(this->root_) << std::endl;
  // t.next("get height");
  //
  // std::cout << GetTreeNodesNum<Leaf, Interior>(this->root_) << std::endl;
  // t.next("count nodes");
  // std::cout << In.size() << " " << vis_leaf_num << " leaf nodes visited, "
  //           << vis_inter_num << " interior nodes visited." << std::endl;

  if (new_node_n > 0) {
    auto updated_node_seq = parlay::tabulate(
        new_node_n, [&](SizeT i) { return node_split_seq[i].first; });
    auto updated_split_seq = parlay::tabulate(
        new_node_n - 1, [&](SizeT i) { return node_split_seq[i].second; });
    this->root_ =
        AllocInteriorNode<Interior, TT>(updated_node_seq, updated_split_seq);
  }

  // t.next("alloc root");
  // puts("-----------------------");
  return;
}

template <typename TT>
TT::SizeT BTree<TT>::BatchInsertRecursive(Node* T, EntrySlice In,
                                          NodeSplitPairSlice node_split_slice) {
  assert(In.size() == node_split_slice.size());

  SizeT n = In.size();
  // vis_inter_num++;

  // insert_leaf_timer.reset();
  // insert_leaf_timer.start();
  if (n == 0) return 0;
  // if (n == 0) return;
  // insert_leaf_timer.stop();
  // insert_leaf_time += insert_leaf_timer.total_time();

  if (T->is_leaf) {
    // vis_leaf_num++;
    // vis_inter_num--;
    // insert_leaf_timer.reset();
    // insert_leaf_timer.start();
    Leaf* TL = static_cast<Leaf*>(T);
    // NodeSplitSeqPair res;
    // return {{T}, typename TT::SplitSeq()};
    // if (TL->size + n <= TT::kBufUB) {
    // res = {{InsertPoints2Leaf(T, In)}, typename TT::SplitSeq()};
    // } else {
    // res = HandlingLeafSplitOverflow(T, In);
    // res = {{T}, typename TT::SplitSeq()};
    // }
    // insert_leaf_timer.stop();
    // insert_leaf_time += insert_leaf_timer.total_time();
    // return res;
    // return;
    if (TL->size + n <= TT::kBufUB) {
      [[maybe_unused]] Node* updated_node = InsertPoints2Leaf(T, In);
      assert(updated_node == T);
      return 0;
    } else {
      return HandlingLeafSplitOverflow(T, In, node_split_slice);
    }
  }

  Interior* TI = static_cast<Interior*>(T);

  // auto split_pos_seq = parlay::sequence<int>(TI->split.size());
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
// auto split_pos_seq = parlay::tabulate(
//     TI->split.size(),
//     [&](size_t i) {
//       // return index to first key where is less is false
//       // return In.size() / TI->split.size() * (i + 1);
//       return parlay::internal::binary_search(
//           In, [&](E const e) { return Entry::GetKey(e) < TI->split[i]; });
//     },
//     1);
#ifdef DEBUG
  for (int i = 1; i < TI->GetSplitN(); i++) {
    assert(parlay::all_of(In.cut(split_pos_seq[i - 1], split_pos_seq[i]),
                          [&](E const e) {
                            return Entry::GetKey(e) >= TI->split[i - 1] &&
                                   Entry::GetKey(e) < TI->split[i];
                          }));
  }
#endif  // DEBUG

  // if (TI->GetNodeN() > TT::kBufUB) {
  //   throw std::runtime_error(
  //       "Too many child nodes in interior node during batch insert.");
  // }
  // NodeSeqList node_seq_list(TI->GetNodeN());
  // SplitSeqList split_seq_list(TI->GetNodeN());
  // NodeSeqList node_seq_list(TT::kBufUB);
  // SplitSeqList split_seq_list(TT::kBufUB);
  // std::array<typename TT::NodeSeq, TT::kBufUB> node_seq_list;
  // std::array<typename TT::SplitSeq, TT::kBufUB> split_seq_list;
  std::array<SizeT, TT::kBufUB> new_split_node_n_arr;
  parlay::sequence<BufSizeT> nodes_map(TI->GetNodeN());
  BufSizeT cnt = 0;
  for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
    SizeT l = (i == 0 ? 0 : split_pos_seq[i - 1]);
    SizeT r = (i == TI->GetNodeN() - 1 ? n : split_pos_seq[i]);
    if (l != r) {
      nodes_map[cnt++] = i;
    } else {
      new_split_node_n_arr[i] = 0;  // i-th child remains the same
    }
  }

  parlay::parallel_for(
      0, cnt,
      [&](BufSizeT i) {
        SizeT l = nodes_map[i] == 0 ? 0 : split_pos_seq[nodes_map[i] - 1];
        SizeT r = nodes_map[i] == TI->GetNodeN() - 1
                      ? n
                      : split_pos_seq[nodes_map[i]];
        // std::tie(node_seq_list[nodes_map[i]], split_seq_list[nodes_map[i]]) =
        new_split_node_n_arr[i] =
            BatchInsertRecursive(TI->tree_nodes[nodes_map[i]], In.cut(l, r),
                                 node_split_slice.cut(l, r));
      },
      1);

  // for (int i = 0; i < TI->GetNodeN(); i++) {
  //   int l = (i == 0 ? 0 : split_pos_seq[i - 1]);
  //   int r = (i == TI->GetNodeN() - 1 ? n : split_pos_seq[i]);
  //   if (l == r) {
  //     node_seq_list[i] = {TI->tree_nodes[i]};
  //     split_seq_list[i] = typename TT::SplitSeq();
  //   } else
  //     // BatchInsertRecursive(TI->tree_nodes[i], In.cut(l, r));
  //     std::tie(node_seq_list[i], split_seq_list[i]) =
  //         // auto a =
  //         BatchInsertRecursive(TI->tree_nodes[i], In.cut(l, r));
  // }
  // merge_timer.reset();
  // merge_timer.start();
  //
  // NodeSplitSeqPair result;
  // if (new_node_n <= TT::kBufUB) {
  //   // return {{T}, typename TT::SplitSeq()};
  //   result = {{UpdateInterior(T, node_seq_list, split_seq_list, new_node_n)},
  //             typename TT::SplitSeq()};
  // } else {
  //   result = HandlingInteriorSplitOverflow(T, node_seq_list, split_seq_list);
  // }
  // merge_timer.stop();
  // merge_time += merge_timer.total_time();
  // return result;
  // return {{T}, typename TT::SplitSeq{}};
  // return;
  // size_t new_node_n =
  //     std::accumulate(node_seq_list.begin(), node_seq_list.end(), 0,
  //                     [](size_t acc, TT::NodeSeq const& node_seq) -> size_t {
  //                       return acc + node_seq.size();
  //                     });
  SizeT new_node_n = 0;
  for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
    new_node_n += new_split_node_n_arr[i] == 0 ? 1 : new_split_node_n_arr[i];
  }
  if (new_node_n <= TT::kBufUB) {
    [[maybe_unused]] Node* new_inter_node = UpdateInterior(
        T, node_split_slice, split_pos_seq, new_split_node_n_arr, new_node_n);
    return 0;
  } else {
    return HandlingInteriorSplitOverflow(T, node_split_slice, split_pos_seq,
                                         new_split_node_n_arr, new_node_n);
  }
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
//   // WARN: assume the B is large enough, otherwise should sieve multiplt
//   levels auto sums = SeievePoints(In, Out, TI->split); assert(sums.size()
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

}  // namespace pmos

#endif  // PMOS_B_TREE_IMPL_BATCH_OP_B_BATCH_INSERT_HPP_

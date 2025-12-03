#ifndef PMOS_B_TREE_IMPL_BATCH_OP_B_BATCH_INSERT_HPP_
#define PMOS_B_TREE_IMPL_BATCH_OP_B_BATCH_INSERT_HPP_

#include <numeric>

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

  auto aux_entry_seq = TT::EntrySeq::uninitialized(In.size());
  auto [updated_node_seq, split_seq] =
      BatchInsertRecursive(this->root_, In, parlay::make_slice(aux_entry_seq));

  if (split_seq.size() == 0) {
    this->root_ = updated_node_seq.front();
  } else {
    this->root_ = AllocInteriorNode<Interior, TT>(updated_node_seq, split_seq);
  }
  return;
}

// NOTE: return the updated Node and the split sequence
template <typename TT>
BTree<TT>::NodeSplitSeqPair BTree<TT>::BatchInsertRecursive(Node* T,
                                                            EntrySlice In,
                                                            EntrySlice Out) {
  size_t n = In.size();

  if (n == 0) return {{T}, typename TT::SplitSeq()};

  if (T->is_leaf) {
    Leaf* TL = static_cast<Leaf*>(T);
    if (TL->size + n <= TT::kBufUB) {
      return {{InsertPoints2Leaf(T, In)}, typename TT::SplitSeq()};
    } else {
      return HandlingLeafSplitOverflow(T, In);
    }
  }

  Interior* TI = static_cast<Interior*>(T);

  // PARA: helpers to finalize interior node after batch insert
  auto finalize_interior =
      [&](NodeSeqList& node_seq_list,
          SplitSeqList& split_seq_list) -> NodeSplitSeqPair {
    size_t new_node_n =
        std::accumulate(node_seq_list.begin(), node_seq_list.end(), 0,
                        [](size_t acc, TT::NodeSeq const& node_seq) -> size_t {
                          return acc + node_seq.size();
                        });
    if (new_node_n <= TT::kBufUB) {
      return {{UpdateInterior(T, node_seq_list, split_seq_list, new_node_n)},
              typename TT::SplitSeq()};
    }
    return HandlingInteriorSplitOverflow(T, node_seq_list, split_seq_list);
  };

  // NOTE: serial build when In is small
  if (n <= kSerialBuildCutoff) {
    // if (1) {
    parlay::sort_inplace(In);

    auto split_pos_seq = parlay::tabulate(TI->split.size(), [&](size_t i) {
      // return index to first key where is less is false
      return parlay::internal::binary_search(
          In, [&](E const e) { return Entry::GetKey(e) < TI->split[i]; });
    });
#ifdef DEBUG
    assert(split_pos_seq.size() == TI->split.size());
    for (int i = 1; i < split_pos_seq.size(); i++) {
      assert(parlay::all_of(In.cut(split_pos_seq[i - 1], split_pos_seq[i]),
                            [&](E const e) {
                              return Entry::GetKey(e) >= TI->split[i - 1] &&
                                     Entry::GetKey(e) < TI->split[i];
                            }));
    }
#endif  // DEBUG
    auto node_seq_list = NodeSeqList(TI->tree_nodes.size());
    auto split_seq_list = SplitSeqList(TI->tree_nodes.size());

    for (int i = 0; i < TI->tree_nodes.size(); i++) {
      std::tie(node_seq_list[i], split_seq_list[i]) = BatchInsertRecursive(
          TI->tree_nodes[i],
          In.cut(i == 0 ? 0 : split_pos_seq[i - 1],
                 i == TI->tree_nodes.size() - 1 ? n : split_pos_seq[i]),
          Out.cut(i == 0 ? 0 : split_pos_seq[i - 1],
                  i == TI->tree_nodes.size() - 1 ? n : split_pos_seq[i]));
    }

    return finalize_interior(node_seq_list, split_seq_list);
  }

  // WARN: assume the B is large enough, otherwise should sieve multiplt levels
  auto sums = SeievePoints(In, Out, TI->split);
  assert(sums.size() == TI->tree_nodes.size());
  assert(std::accumulate(sums.begin(), sums.end(), 0) == n);

  auto node_seq_list = NodeSeqList(TI->tree_nodes.size());
  auto split_seq_list = SplitSeqList(TI->tree_nodes.size());
  parlay::parallel_for(
      0, TI->tree_nodes.size(),
      [&](typename TT::BucketType i) {
        size_t s = 0;
        for (typename TT::BucketType j = 0; j < i; j++) {
          s += sums[j];
        }

        std::tie(node_seq_list[i], split_seq_list[i]) = BatchInsertRecursive(
            TI->tree_nodes[i], Out.cut(s, s + sums[i]), In.cut(s, s + sums[i]));
      },
      1);

  return finalize_interior(node_seq_list, split_seq_list);
}

}  // namespace pmos

#endif  // PMOS_B_TREE_IMPL_BATCH_OP_B_BATCH_INSERT_HPP_

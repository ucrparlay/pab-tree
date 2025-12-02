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

  Slice A = parlay::make_slice(In);
  BatchInsert_(A);
  return;
}

template <typename TT>
void BTree<TT>::BatchInsert_(Slice In) {
  if (this->root_ == nullptr) {  // TODO: may check using explicity tag
    return Build_(In);
  }
  auto [updated_node_seq, split_seq] = BatchInsertRecursive(In);
  if (split_seq.size() == 0) {
    this->root_ = updated_node_seq;
  } else {
    this->root_ = HandlingLeafSplitOverflow(updated_node_seq, split_seq);
  }
  return;
}

// NOTE: return the updated Node and the split sequence
template <typename TT>
BTree<TT>::NodeSplitSeqPair BTree<TT>::BatchInsertRecursive(Node* T, Slice In,
                                                            Slice Out) {
  size_t n = In.size();

  if (n == 0) return {T, TT::SplitSeq()};

  if (T->is_leaf) {
    Leaf* TL = static_cast<Leaf*>(T);
    if (TL->size + n <= TT::kBufUB) {
      return {InsertPoints2Leaf<Leaf>(T, In), TT::SplitSeq()};
    } else {
      return HandlingLeafSplitOverflow(T, In);
    }
  }

  if (n <= kSerialBuildCutoff) {
    parlay::sort_inplace(In);

    Interior* TI = static_cast<Interior*>(T);
    auto split_pos_seq = parlay::tabulate(TI->split.size(), [&](size_t i) {
      // return index to first key greater or equal to v
      return parlay::internal::binary_search(
          In, [&](Entry e) { return Entry::GetKey(e) < TI->split[i]; });
    });
    auto node_seq_list = NodeSeqList::uninitialized(TI->tree_nodes.size());
    auto split_seq_list = SplitSeqList::uninitialized(TI->tree_nodes.size());

    for (int i = 0; i < TI->tree_nodes.size(); i++) {
      std::tie(node_seq_list[i], split_seq_list[i]) = BatchInsertRecursive(
          TI->tree_nodes[i],
          In.cut(i == 0 ? 0 : split_pos_seq[i - 1],
                 i == TI->tree_nodes.size() - 1 ? n : split_pos_seq[i]),
          Out.cut(i == 0 ? 0 : split_pos_seq[i - 1],
                  i == TI->tree_nodes.size() - 1 ? n : split_pos_seq[i]));
    }

    size_t new_node_n =
        std::accumulate(node_seq_list.begin(), node_seq_list.end(), 0,
                        [](size_t acc, TT::NodeSeq const& node_seq) -> size_t {
                          return acc + node_seq->size;
                        });

    if (new_node_n <= TT::kBufUB) {
      return {TT::template UpdateInterior<TT::Interior>(T, node_seq_list,
                                                        split_seq_list),
              TT::SplitSeq()};
    } else {
      return {HandlingInteriorSplitOverflow(T, node_seq_list, split_seq_list),
              TT::SplitSeq()};
    }
  }

  // // NOTE: assign each Node a tag
  // InnerTree IT(*this);
  // assert(IT.rev_tag.size() == BT::kBucketNum);
  // IT.AssignNodeTag(T, 1);
  // assert(IT.tags_num > 0 && IT.tags_num <= BT::kBucketNum);
  //
  // BT::template SeievePoints<Interior>(In, Out, n, IT.tags, IT.sums,
  //                                     IT.tags_num);
  //
  // IT.TagInbalanceNode([&]([[maybe_unused]] BucketType idx) -> bool {
  //   auto const TI = static_cast<Interior*>(IT.tags[idx].first);
  //   return split_rule_.AllowRebuild() &&
  //          BT::ImbalanceNode(TI->left->size + IT.sums_tree[idx << 1],
  //                            TI->size + IT.sums_tree[idx]);
  // });
  // assert(IT.tags_num > 0 && IT.tags_num <= BT::kBucketNum);
  // auto tree_nodes = parlay::sequence<Node*>::uninitialized(IT.tags_num);
  //
  // parlay::parallel_for(
  //     0, IT.tags_num,
  //     [&](decltype(IT.tags_num) i) {
  //       size_t s = 0;
  //       for (decltype(IT.tags_num) j = 0; j < i; j++) {
  //         s += IT.sums_tree[IT.rev_tag[j]];
  //       }
  //
  //       DimsType next_dim = d, depth = IT.GetDepthByIndex(IT.rev_tag[i]);
  //       for (BucketType i = 0; i < depth; i++) {
  //         next_dim = split_rule_.NextDimension(next_dim);
  //       }
  //
  //       if (IT.tags[IT.rev_tag[i]].second == BT::kBucketNum + 1) {
  //         // NOTE: continue sieve
  //         tree_nodes[i] = BatchInsertRecursive(
  //             IT.tags[IT.rev_tag[i]].first,
  //             Out.cut(s, s + IT.sums_tree[IT.rev_tag[i]]),
  //             In.cut(s, s + IT.sums_tree[IT.rev_tag[i]]), next_dim);
  //       } else {  // NOTE: launch rebuild subtree
  //         assert(IT.tags[IT.rev_tag[i]].second == BT::kBucketNum + 2);
  //         assert(IT.tags[IT.rev_tag[i]].first->size +
  //                    IT.sums_tree[IT.rev_tag[i]] >=
  //                0);
  //
  //         tree_nodes[i] = BT::template RebuildWithInsert<Leaf, Interior>(
  //             IT.tags[IT.rev_tag[i]].first, prepare_func,
  //             Out.cut(s, s + IT.sums_tree[IT.rev_tag[i]]), next_dim);
  //       }
  //     },
  //     1);
  //
  // return IT.template UpdateInnerTree<InnerTree::kUpdatePointer>(tree_nodes);
}

}  // namespace pmos

#endif  // PMOS_B_TREE_IMPL_BATCH_OP_B_BATCH_INSERT_HPP_

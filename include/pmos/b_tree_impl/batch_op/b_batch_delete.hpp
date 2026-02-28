#ifndef PMOS_B_TREE_IMPL_BATCH_OP_B_Batch_DELETE_HPP_
#define PMOS_B_TREE_IMPL_BATCH_OP_B_Batch_DELETE_HPP_

#include "../../b_tree.h"
#include "dependence/binary_search.h"
#include "parlay/monoid.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/slice.h"
#include <tuple>

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
  parlay::internal::timer t(">>> BatchDelete");
  // std::cout << "Tree nodes before delete: "
  //           << GetTreeNodesNum<Leaf, Interior>(this->root_) << std::endl;
  static_assert(
    std::same_as<typename decltype(In)::value_type, typename TT::K>);
  auto sorted_seq = parlay::sort(In);
  // t.next("Sorted");

  // std::cout << " Before delete, root size: " << this->root_->size <<
  // std::endl;

  auto rt = BatchDeleteRecursive(this->root_, parlay::make_slice(sorted_seq));
  // t.next("BatchDeleteRecursive");

  // std::cout << In.size() << " keys to delete, Invalid flag: " << invalid_flag
  //           << ", Invalid subtree num: " << invalid_subtree_num
  //           << ", tree size after delete: " << this->root_->size <<
  //           std::endl;

  if (rt.is_invalid) {
    assert(this->root_->size == 0);
    this->DeleteTree();
    return;
  }

  if (rt.has_invalid_subtree) {
    // fetch the valid subtrees and leaf entries needs to be reinserted
    auto reinsert_node_seq =
      parlay::sequence<InteriorNodesWrapper>::uninitialized(
        rt.valid_subtree_num);
    auto reinsert_entries = TT::EntrySeq::uninitialized(rt.reinsert_size);
    FetchValidSubtreeAndInvalidLeaves(this->root_,
                                      parlay::make_slice(reinsert_node_seq),
                                      parlay::make_slice(reinsert_entries),
                                      true);
    auto sorted_node = parlay::sort(parlay::make_slice(reinsert_node_seq));
    auto node_split_seq = NodeSplitPairSeq::uninitialized(sorted_node.size());

    // reinsert valid subtrees
    SizeT st = 0;
    SizeT ed = 0;
    auto height = sorted_node.front().height;
    while (height) {
      ed = pmos::BinarySearch<16>(
        parlay::make_slice(sorted_node.begin() + st, sorted_node.end()),
        [&](auto const wrapper) { return wrapper.height >= height; });
      auto new_node_n = BatchInsertNodesRecursive(this->root_,
                                                  sorted_node.cut(st, ed),
                                                  node_split_seq.cut(st, ed),
                                                  height);

      assert(new_node_n == 0); // root should always not split
      height--;
      st = ed;
    }

    // reinsert leaf entries
    assert(parlay::is_sorted(reinsert_entries, [](auto a, auto b) {
      return Entry::GetKey(a) < Entry::GetKey(b);
    }));
    BatchInsert_(parlay::make_slice(reinsert_entries), true);

    // t.next("FetchInvalidSubtreeNodes");
    // parlay::parallel_for(0, invalid_subtree_num, [&](SizeT i) {
    //   SizeT sum = 0;
    //   for (SizeT j = 0; j < i; j++) {
    //     sum += flatten_node_seq[j]->size;
    //   }
    //   FlattenRec(flatten_node_seq[i],
    //              reinsert_entries.cut(sum, sum + flatten_node_seq[i]->size));
    //   DeleteTreeRec(flatten_node_seq[i]);
    // });

    // std::cout << "Flatten invalid subtrees: " << invalid_subtree_num
    //           << std::endl;
    // std::cout << "Max tree size: "
    //           << (*parlay::max_element(
    //                 flatten_node_seq,
    //                 [](Node* a, Node* b) { return a->size < b->size; }))
    //                ->size
    //           << std::endl;
    // std::cout << "Number of leaves: "
    //           << parlay::count_if(flatten_node_seq,
    //                               [](Node* T) { return T->is_leaf; })
    //           << std::endl;
    // std::cout << "Number of leaf entries: "
    //           << std::accumulate(flatten_node_seq.begin(),
    //                              flatten_node_seq.end(),
    //                              static_cast<SizeT>(0),
    //                              [](SizeT sum, Node* T) {
    //                                return sum + (T->is_leaf ? T->size : 0);
    //                              })
    //           << std::endl;
    // std::cout << "Reinsert entry num: " << reinsert_size << std::endl;
    // std::cout << "Max reinsert batch size: " << this->max_reinsert_batch_size
    //           << std::endl;
    // parlay::parallel_for(0, invalid_subtree_num, [&](SizeT i) {
    //   DeleteTreeRec(flatten_node_seq[i]);
    // });
    // t.next("DeleteTreeRec");
    // assert(parlay::is_sorted(reinsert_entries, [](auto a, auto b) {
    //   return Entry::GetKey(a) < Entry::GetKey(b);
    // }));
    // BatchInsert_(parlay::make_slice(reinsert_entries), true);
    // t.next("BatchInsert_");
  }
}

// PARA: return whether T is invalid, whether T contain invalid subtree, valid
// subtree num, and total # of leaf entries needs to be reinserted
template<typename TT>
typename BTree<TT>::DeleteInfoWrapper
BTree<TT>::BatchDeleteRecursive(Node* T, TT::KeySlice In)
{

  if (In.size() == 0) {
    // stop here, than all subtree below is valid
    return DeleteInfoWrapper{ .is_invalid = false,
                              .has_invalid_subtree = false,
                              .valid_subtree_num = 1,
                              .reinsert_size = 0 };
  }

  if (T->is_leaf) {
    [[maybe_unused]] auto delete_cnt = DeletePoints4Leaf(T, In);
    assert(delete_cnt == In.size());
    return DeleteInfoWrapper{ .is_invalid = T->size < TT::kBufLB,
                              .has_invalid_subtree = T->size < TT::kBufLB,
                              .valid_subtree_num = T->size < TT::kBufLB ? 0 : 1,
                              .reinsert_size =
                                T->size < TT::kBufLB ? T->size : 0 };
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

  BooleanBufUBArr subtree_is_invalid;
  BooleanBufUBArr subtree_has_invalid;
  SizeBufUBArr valid_subtree_num;
  SizeBufUBArr reinsert_entries_num;

  BufSizeBufUBArr nodes_map;
  BufSizeT cnt = 0;
  for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
    SizeT l = (i == 0 ? 0 : split_pos_seq[i - 1]);
    SizeT r = (i == TI->GetNodeN() - 1 ? In.size() : split_pos_seq[i]);
    if (l != r) {
      nodes_map[cnt++] = i;
    } else { // no element in this subtree, so it's valid
      subtree_is_invalid[i] = false;
      subtree_has_invalid[i] = false;
      valid_subtree_num[i] = 1; // this subtree itself is a valid subtree
      reinsert_entries_num[i] = 0;
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
      auto rt =
        BatchDeleteRecursive(TI->tree_nodes[nodes_map[i]], In.cut(l, r));
      subtree_is_invalid[nodes_map[i]] = rt.is_invalid;
      subtree_has_invalid[nodes_map[i]] = rt.has_invalid_subtree;
      valid_subtree_num[nodes_map[i]] = rt.valid_subtree_num;
      reinsert_entries_num[nodes_map[i]] = rt.reinsert_size;
    },
    // In.size() > 2048 ? 1 : cnt);
    1);

  return BatchDeleteUpdateInteriorSize(T,
                                       subtree_is_invalid,
                                       valid_subtree_num,
                                       subtree_has_invalid,
                                       reinsert_entries_num);
}

} // namespace pmos

#endif // PMOS_B_TREE_IMPL_BATCH_OP_B_Batch_DELETE_HPP_

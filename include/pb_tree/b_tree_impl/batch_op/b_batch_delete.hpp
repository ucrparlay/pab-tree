#ifndef PBTREE_B_TREE_IMPL_BATCH_OP_B_Batch_DELETE_HPP_
#define PBTREE_B_TREE_IMPL_BATCH_OP_B_Batch_DELETE_HPP_

#include "../../b_tree.h"
#include "parlay/monoid.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/slice.h"
#include <tuple>

namespace pb_tree {
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

  constexpr auto leaf_entries_pattern = TT::LeafEntriesPattern::kHybrid;
  constexpr bool sorted_input = false;
  BatchDelete_(A, leaf_entries_pattern, sorted_input, DefaultHybridPred{});
}

template<typename TT>
template<typename Range, typename LeafSortPred>
void
BTree<TT>::BatchDelete(Range&& In,
                       const TT::PatternType leaf_entries_pattern,
                       const bool sorted_input,
                       LeafSortPred const& leaf_sort_pred)
{
  static_assert(parlay::is_random_access_range_v<Range>);
  static_assert(
    parlay::is_less_than_comparable_v<parlay::range_reference_type_t<Range>>);
  static_assert(std::is_constructible_v<parlay::range_value_type_t<Range>,
                                        parlay::range_reference_type_t<Range>>);

  auto A = parlay::make_slice(std::forward<Range>(In));

  BatchDelete_(A, leaf_entries_pattern, sorted_input, leaf_sort_pred);
}

template<typename TT>
template<typename Range, typename LeafSortPred>
  requires std::predicate<LeafSortPred, typename BTree<TT>::Node*, Range>
void
BTree<TT>::BatchDelete_(Range In,
                        const TT::PatternType leaf_entries_pattern,
                        const bool sorted_input,
                        LeafSortPred const& leaf_sort_pred)
{
  if (this->root_ == nullptr) {
    return;
  }

  if (In.size() == 0) {
    return;
  }

  static_assert(
    std::same_as<typename decltype(In)::value_type, typename TT::K>);

  // parlay::internal::timer t;

  parlay::slice<const K*, const K*> sorted_slice{ In.begin(), In.end() };
  typename TT::KeySeq sorted_seq;
  if (!sorted_input) {
    sorted_seq = parlay::sort(In);
    sorted_slice = parlay::make_slice(sorted_seq.cbegin(), sorted_seq.cend());
  }

  auto [invalid_flag, invalid_subtree_num, reinsert_entry_num] =
    BatchDeleteRecursive(this->root_,
                         sorted_slice,
                         leaf_entries_pattern,
                         leaf_sort_pred);
  // t.next("remove points ");

  if (invalid_flag) { // NOTE: root is invalid
    if (this->root_->is_leaf) {
      return;
    }
    assert(this->root_->size == 0);
    constexpr bool kEnableGranularity = false;
    this->DeleteTreeRec<kEnableGranularity>(this->root_);
    this->root_ = nullptr;
    // t.next("delete tree ");
    return;
  }

  // this->sys_reinsert_size = 0;
  // this->sys_balance_leaf_size = 0;

  if (invalid_subtree_num) {
    if (this->root_->size < TT::kBufUB &&
        !this->root_->is_leaf) { // fits in one leaf directly use leaf
      auto tree_entries = TT::EntrySeq::uninitialized(this->root_->size);
      FlattenRec(this->root_, parlay::make_slice(tree_entries));
      this->DeleteTreeRec<true>(this->root_);
      Build_(parlay::make_slice(tree_entries));
      return;
    }
    auto reinsert_entries = TT::EntrySeq::uninitialized(reinsert_entry_num);

#ifdef DEBUG
    this->Validate<false>();
    puts(">>> pass tree aug after delete");
#endif // DEBUG

    const bool has_tomb = true;
    FetchInvalidSubtreeNodes(this->root_,
                             parlay::make_slice(reinsert_entries),
                             has_tomb,
                             leaf_entries_pattern,
                             leaf_sort_pred);
  }

  // std::cout << this->sys_reinsert_size << " ";
  // std::cout << this->sys_balance_leaf_size << std::endl;
}

// Returns (invalid_flag, needs_fetch, reinsert_entry_num).
// needs_fetch: any descendant needs FetchInvalidSubtreeNodes (not stored in
// node).
template<typename TT>
template<typename LeafSortPred>
std::tuple<bool, bool, typename TT::SizeT>
BTree<TT>::BatchDeleteRecursive(Node* T,
                                TT::ConstKeySlice In,
                                const TT::PatternType leaf_entries_pattern,
                                LeafSortPred const& leaf_sort_pred)
{

  if (In.size() == 0) {
    // stop here, than all subtree is valid
    if (!T->is_leaf) {
      static_cast<Interior*>(T)->ResetDeletionFlag();
    }
    assert(NodeIsValid(T) && NodeContainInvalidSubtree(T) == 0 &&
           GetNodeReinsertSize(T) == 0);
    return std::make_tuple(false, 0, 0);
  }

  if (T->is_leaf) {
    [[maybe_unused]] auto old_size = T->size;
    [[maybe_unused]] auto new_node =
      DeletePoints4Leaf(T, In, leaf_entries_pattern, leaf_sort_pred);
    assert(new_node == T);
    assert(new_node->size + In.size() == old_size);
    // assert(delete_cnt == In.size());
    // return std::make_tuple(T->size < TT::kBufLB,
    //                        T->size < TT::kBufLB ? 1 : 0,
    //                        T->size < TT::kBufLB ? T->size : 0);
    return std::make_tuple(GetNodeInvalidStatus(T),
                           NodeContainInvalidSubtree(T),
                           GetNodeReinsertSize(T));
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

  BooleanBufUBArr contain_invalid_subtree;
  BooleanBufUBArr invalid_subtree_flag;
  SizeBufUBArr reinsert_entries_num;

  // Hoist: same answer for every child since T's children are all at one level.
  const bool children_are_interior = !NodeHasLeafChildren(T);

  BufSizeBufUBArr nodes_map;
  SizeBufUBArr touched_l, touched_r;
  BufSizeT cnt = 0;
  for (BufSizeT i = 0; i < TI->GetNodeN(); i++) {
    SizeT l = (i == 0 ? 0 : split_pos_seq[i - 1]);
    SizeT r = (i == TI->GetNodeN() - 1 ? In.size() : split_pos_seq[i]);
    if (l != r) {
      touched_l[cnt] = l;
      touched_r[cnt] = r;
      nodes_map[cnt++] = i;
    } else { // no element in this subtree, so it's valid
      invalid_subtree_flag[i] = false;
      contain_invalid_subtree[i] = 0;
      reinsert_entries_num[i] = 0;
      // NOTE: if a tree node receive no deletion, we need to explicitly reset
      // its deletion information, because the reinsertion phase needs to access
      // the reinsert information from the tree node directly
      if (children_are_interior) {
        static_cast<Interior*>(TI->tree_nodes[i])->ResetDeletionFlag();
      }
    }
  }

  parlay::parallel_for(
    0,
    cnt,
    [&](BufSizeT i) {
      std::tie(invalid_subtree_flag[nodes_map[i]],
               contain_invalid_subtree[nodes_map[i]],
               reinsert_entries_num[nodes_map[i]]) =
        BatchDeleteRecursive(TI->tree_nodes[nodes_map[i]],
                             In.cut(touched_l[i], touched_r[i]),
                             leaf_entries_pattern,
                             leaf_sort_pred);
    },
    1);

  return BatchDeleteUpdateInteriorSize(
    T, contain_invalid_subtree, invalid_subtree_flag, reinsert_entries_num);
}

} // namespace pb_tree

#endif // PBTREE_B_TREE_IMPL_BATCH_OP_B_Batch_DELETE_HPP_

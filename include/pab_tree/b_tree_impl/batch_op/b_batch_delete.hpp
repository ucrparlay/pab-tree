#ifndef PABTREE_B_TREE_IMPL_BATCH_OP_B_Batch_DELETE_HPP_
#define PABTREE_B_TREE_IMPL_BATCH_OP_B_Batch_DELETE_HPP_

#include <cassert>

#include "pab_tree/b_tree.h"
#include "parlay/monoid.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/slice.h"
#include <tuple>

namespace pab_tree {
template<typename TT>
template<typename Range>
void
btree<TT>::batch_delete(Range&& In)
{
  static_assert(parlay::is_random_access_range_v<Range>);
  static_assert(
    parlay::is_less_than_comparable_v<parlay::range_reference_type_t<Range>>);
  static_assert(std::is_constructible_v<parlay::range_value_type_t<Range>,
                                        parlay::range_reference_type_t<Range>>);

  auto A = parlay::make_slice(std::forward<Range>(In));

  constexpr auto leaf_entries_pattern = TT::leaf_entries_pattern::hybrid;
  constexpr bool sorted_input = false;
  BatchDelete_(A, leaf_entries_pattern, sorted_input, default_hybrid_pred{});
}

template<typename TT>
template<typename Range, typename LeafSortPred>
void
btree<TT>::batch_delete(Range&& In,
                       const TT::pattern_type leaf_entries_pattern,
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
  requires std::predicate<LeafSortPred, typename btree<TT>::Node*, Range>
void
btree<TT>::BatchDelete_(Range In,
                        const TT::pattern_type leaf_entries_pattern,
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
  typename TT::key_seq sorted_seq;
  if (!sorted_input) {
    sorted_seq = parlay::sort(In);
    sorted_slice = parlay::make_slice(sorted_seq.cbegin(), sorted_seq.cend());
  }

  auto [invalid_flag, invalid_subtree_num, reinsert_entry_num] =
    batch_delete_recursive(this->root_,
                         sorted_slice,
                         leaf_entries_pattern,
                         leaf_sort_pred);
  // t.next("remove points ");

  if (invalid_flag) { // NOTE: root is invalid
    if (this->root_->is_leaf) {
      return;
    }
    // An invalid root is not an empty one: it is invalid as soon as every
    // child is invalid, and a leaf is invalid at any size below k_buf_lb. Test
    // the entry count, or a tree still holding live entries gets freed.
    // The old assert(root_->size == 0) here asserted exactly the wrong thing
    // and had to go; it fires on every tree this branch now recovers.
    if (this->root_->size == 0) {
      constexpr bool k_enable_granularity = false;
      this->delete_tree_rec<k_enable_granularity>(this->root_);
      this->root_ = nullptr;
      // t.next("delete tree ");
      return;
    }
    this->rebuild_from_flatten();
    return;
  }

  // this->sys_reinsert_size = 0;
  // this->sys_balance_leaf_size = 0;

  if (invalid_subtree_num) {
    if (this->root_->size < TT::k_buf_ub &&
        !this->root_->is_leaf) { // fits in one leaf directly use leaf
      this->rebuild_from_flatten();
      return;
    }
    auto reinsert_entries = TT::entry_seq::uninitialized(reinsert_entry_num);

#ifdef DEBUG
    this->validate<false>();
    puts(">>> pass tree aug after delete");
#endif // DEBUG

    const bool has_tomb = true;
    fetch_invalid_subtree_nodes(this->root_,
                             parlay::make_slice(reinsert_entries),
                             has_tomb,
                             leaf_entries_pattern,
                             leaf_sort_pred);
  }

  // std::cout << this->sys_reinsert_size << " ";
  // std::cout << this->sys_balance_leaf_size << std::endl;
}

// Flatten the whole tree and rebuild it from the surviving entries.
template<typename TT>
void
btree<TT>::rebuild_from_flatten()
{
  auto tree_entries = TT::entry_seq::uninitialized(this->root_->size);
  flatten_rec(this->root_, parlay::make_slice(tree_entries));
  this->delete_tree_rec<true>(this->root_);
  this->root_ = nullptr; // Build_ releases root_, which is already freed
  Build_(parlay::make_slice(tree_entries));
}

// Returns (invalid_flag, needs_fetch, reinsert_entry_num).
// needs_fetch: any descendant needs fetch_invalid_subtree_nodes (not stored in
// node).
template<typename TT>
template<typename LeafSortPred>
std::tuple<bool, bool, typename TT::size_type>
btree<TT>::batch_delete_recursive(Node* T,
                                TT::const_key_slice In,
                                const TT::pattern_type leaf_entries_pattern,
                                LeafSortPred const& leaf_sort_pred)
{

  if (In.size() == 0) {
    // stop here, than all subtree is valid
    if (!T->is_leaf) {
      static_cast<Interior*>(T)->reset_deletion_flag();
    }
    assert(node_is_valid(T) && node_contain_invalid_subtree(T) == 0 &&
           get_node_reinsert_size(T) == 0);
    return std::make_tuple(false, 0, 0);
  }

  if (T->is_leaf) {
    [[maybe_unused]] auto old_size = T->size;
    [[maybe_unused]] auto new_node =
      delete_points_4_leaf(T, In, leaf_entries_pattern, leaf_sort_pred);
    assert(new_node == T);
    assert(new_node->size + In.size() == old_size);
    // assert(delete_cnt == In.size());
    // return std::make_tuple(T->size < TT::k_buf_lb,
    //                        T->size < TT::k_buf_lb ? 1 : 0,
    //                        T->size < TT::k_buf_lb ? T->size : 0);
    return std::make_tuple(get_node_invalid_status(T),
                           node_contain_invalid_subtree(T),
                           get_node_reinsert_size(T));
  }

  auto TI = static_cast<Interior*>(T);

  size_buf_ub_arr split_pos_seq;
  if (In.size() > TT::k_split_factor * TI->get_split_n()) {
    split_batch_binary_search(split_pos_seq, In, TI->get_split_slice());
  } else {
    split_batch_merge(split_pos_seq, In, TI->get_split_slice());
  }

#ifdef DEBUG
  // Ensure all elements between two splits are correct
  // i.e., less than the split value
  for (int i = 1; i < TI->get_split_n(); i++) {
    assert(parlay::all_of(
      In.cut(split_pos_seq[i - 1], split_pos_seq[i]),
      [&](K const k) { return k >= TI->split[i - 1] && k < TI->split[i]; }));
  }
#endif // DEBUG

  boolean_buf_ub_arr contain_invalid_subtree;
  boolean_buf_ub_arr invalid_subtree_flag;
  size_buf_ub_arr reinsert_entries_num;

  // Hoist: same answer for every child since T's children are all at one level.
  const bool children_are_interior = !node_has_leaf_children(T);

  buf_size_buf_ub_arr nodes_map;
  size_buf_ub_arr touched_l, touched_r;
  buf_size_type cnt = 0;
  for (buf_size_type i = 0; i < TI->get_node_n(); i++) {
    size_type l = (i == 0 ? 0 : split_pos_seq[i - 1]);
    size_type r = (i == TI->get_node_n() - 1 ? In.size() : split_pos_seq[i]);
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
        static_cast<Interior*>(TI->tree_nodes[i])->reset_deletion_flag();
      }
    }
  }

  parlay::parallel_for(
    0,
    cnt,
    [&](buf_size_type i) {
      std::tie(invalid_subtree_flag[nodes_map[i]],
               contain_invalid_subtree[nodes_map[i]],
               reinsert_entries_num[nodes_map[i]]) =
        batch_delete_recursive(TI->tree_nodes[nodes_map[i]],
                             In.cut(touched_l[i], touched_r[i]),
                             leaf_entries_pattern,
                             leaf_sort_pred);
    },
    1);

  return batch_delete_update_interior_size(
    T, contain_invalid_subtree, invalid_subtree_flag, reinsert_entries_num);
}

} // namespace pab_tree

#endif // PABTREE_B_TREE_IMPL_BATCH_OP_B_Batch_DELETE_HPP_

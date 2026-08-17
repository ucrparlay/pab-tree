#ifndef PABTREE_B_TREE_IMPL_LEAF_OP_HPP_
#define PABTREE_B_TREE_IMPL_LEAF_OP_HPP_

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <numeric>
#include <tuple>
#include <type_traits>
#include <utility>

#include "pab_tree/b_tree.h"
#include "pab_tree/dependence/concepts.h"

namespace pab_tree {
template<typename TT>
template<typename Range, typename LeafSortPred>
TT::Node*
btree<TT>::post_process_leaf_change(Node* T,
                                 Range In,
                                 const TT::pattern_type leaf_entries_pattern,
                                 LeafSortPred const& leaf_sort_pred)
{
  // WARN: does not know whether the T->elements are sorted or not,
  // so might mark it as unsorted if the leaf entries pattern is unsort/hybrid,
  // even if the entries are actually sorted.
  assert(T->is_leaf);
  auto TL = static_cast<Leaf*>(T);
  if (leaf_entries_pattern == TT::leaf_entries_pattern::sorted) {
    std::sort(TL->get_ele_begin_iter(), TL->get_ele_end_iter());
    TL->is_sorted = true;
  } else if (leaf_entries_pattern == TT::leaf_entries_pattern::unsorted) {
    TL->is_sorted = false;
  } else if (leaf_entries_pattern == TT::leaf_entries_pattern::hybrid) {
    if (leaf_sort_pred(T, In)) {
      std::sort(TL->get_ele_begin_iter(), TL->get_ele_end_iter());
      TL->is_sorted = true;
    } else {
      TL->is_sorted = false;
    }
  } else {
    assert(false && "Invalid leaf entries pattern");
  }
  return T;
}

template<typename TT>
template<typename Range, bool sorted>
void
btree<TT>::extract_points_in_leaf(Node* T, Range Out)
{
  auto TL = static_cast<Leaf*>(T);
  if constexpr (sorted) {
    if (!TL->is_sorted) {
      std::sort(TL->elements.begin(), TL->elements.begin() + TL->size);
      TL->is_sorted = true;
    }
  }
  assert(Out.size() >= TL->size);
  std::ranges::copy_n(TL->get_ele_begin_iter(), TL->size, Out.begin());
}

template<typename TT>
template<typename Range, typename LeafSortPred>
TT::Node*
btree<TT>::insert_points_2_leaf(Node* T,
                             Range In,
                             TT::pattern_type leaf_entries_pattern,
                             LeafSortPred const& leaf_sort_pred)
{
  Leaf* TL = static_cast<Leaf*>(T);

  assert(T->size + In.size() <= TT::k_buf_ub);
  assert(T->size == TL->get_ele().size());
  if (TL->elements.size() == 0) {
    assert(TL->size == 0);
    TL->initialize_ele_seq();
    assert(TL->elements.size() == TT::k_buf_ub);
  }
  // PERF: buf_size_type should be enough since total size fits within
  std::ranges::copy_n(In.begin(), In.size(), TL->get_ele_end_iter());
  TL->size += In.size();
  TL->update_aug(
    parlay::make_slice(TL->elements.begin(), TL->elements.begin() + TL->size));

  return post_process_leaf_change(T, In, leaf_entries_pattern, leaf_sort_pred);
}

template<typename TT>
template<typename Range>
TT::size_type
btree<TT>::handling_leaf_split_overflow(Node* T,
                                     Range In,
                                     node_split_pair_slice node_split_slice)
{
  Leaf* TL = static_cast<Leaf*>(T);
  auto entry_seq = TT::entry_seq::uninitialized(T->size + In.size());

  if (TL->is_sorted) {
    // In is always sorted (sorted by BatchInsert_ before descent).
    // Both halves sorted: O(n) merge instead of O(n log n) sort.
    std::ranges::merge(TL->get_ele_begin_iter(),
                       TL->get_ele_end_iter(),
                       In.begin(),
                       In.end(),
                       entry_seq.begin(),
                       [](E const& a, E const& b) {
                         return Entry::get_key(a) < Entry::get_key(b);
                       });
    free_node<Leaf>(T);
  } else {
    std::ranges::copy_n(TL->get_ele_begin_iter(), TL->size, entry_seq.begin());
    parlay::copy(
      In, parlay::make_slice(entry_seq.begin() + TL->size, entry_seq.end()));
    free_node<Leaf>(T);
    entry_seq = parlay::sort(entry_seq);
  }

  size_type rt_bkt_n = generate_leaf_nodes(
    parlay::make_slice(entry_seq),
    gen_nodes_output_handler{
      .set_key = [&](size_type i, auto key) { node_split_slice[i].second = key; },
      .set_node = [&](size_type i,
                      auto node) { node_split_slice[i].first = node; } });

  return rt_bkt_n;
}

template<typename TT>
template<typename LeafSortPred>
TT::Node*
btree<TT>::delete_points_4_leaf(Node* T,
                             TT::const_key_slice In,
                             const TT::pattern_type leaf_entries_pattern,
                             LeafSortPred const& leaf_sort_pred)
{
  auto TL = static_cast<Leaf*>(T);

  assert(T->size >= In.size());
  assert(parlay::all_of(In, [&](auto a) {
    return std::find_if(TL->elements.begin(),
                        TL->elements.begin() + TL->size,
                        [&](auto const e) { return Entry::get_key(e) == a; }) !=
           TL->elements.begin() + TL->size;
  }));

  auto it = TL->elements.begin();
  auto end = TL->elements.begin() + TL->size;
  buf_size_type delete_cnt = 0;
  for (size_type i = 0; i < In.size(); i++) {
    it = std::ranges::find_if(TL->elements.begin(), end, [&](auto const& e) {
      return Entry::get_key(e) == In[i];
    });
    if (it != end) {
      std::ranges::iter_swap(it, --end);
      delete_cnt++;
    }
    if (end == TL->elements.begin()) { // all elements are deleted
      break;
    }
  }

  assert(end == TL->elements.begin() ||
         std::cmp_equal(std::distance(TL->elements.begin(), end),
                        TL->size - In.size()));
  TL->size = std::distance(TL->elements.begin(), end);

  assert(TL->size >= 0);
  TL->update_aug(
    parlay::make_slice(TL->elements.begin(), TL->elements.begin() + TL->size));

  return post_process_leaf_change(T, In, leaf_entries_pattern, leaf_sort_pred);
}

} // namespace pab_tree

#endif // PABTREE_B_TREE_IMPL_LEAF_OP_HPP_
